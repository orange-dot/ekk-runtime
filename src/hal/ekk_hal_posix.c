/**
 * @file ekk_hal_posix.c
 * @brief EK-KOR v2 - POSIX/Windows HAL Implementation
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * Hardware abstraction layer for testing on desktop systems.
 * Supports both POSIX (Linux, macOS) and Windows.
 *
 * Features:
 * - High-resolution timing via clock_gettime/QueryPerformanceCounter
 * - Message queues using thread-safe ring buffers
 * - Atomic operations via compiler builtins
 * - Printf for debug output
 */

#include "ekk/ekk_hal.h"
#include "ekk/ekk_field.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#endif

/* ============================================================================
 * PRIVATE STATE
 * ============================================================================ */

/** Start time for relative timestamps */
#ifdef _WIN32
static LARGE_INTEGER g_start_time;
static LARGE_INTEGER g_freq;
#else
static struct timespec g_start_time;
#endif

/** Message queue for simulation */
#define MSG_QUEUE_SIZE      64
#define MSG_MAX_LEN         64

typedef struct {
    ekk_module_id_t sender_id;
    ekk_msg_type_t msg_type;
    uint8_t data[MSG_MAX_LEN];
    uint32_t len;
    bool valid;
} hal_message_t;

static hal_message_t g_msg_queue[MSG_QUEUE_SIZE];
static volatile uint32_t g_msg_head = 0;
static volatile uint32_t g_msg_tail = 0;

/** Critical section lock */
#ifdef _WIN32
static CRITICAL_SECTION g_critical;
#else
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/** Receive callback */
static ekk_hal_recv_cb g_recv_callback = NULL;

/** Module ID (for simulation) */
static ekk_module_id_t g_module_id = 1;

/** Initialized flag */
static bool g_hal_initialized = false;

/** Mock time support for testing */
static bool g_mock_time_enabled = false;
static ekk_time_us_t g_mock_time = 0;

/** One-shot receive fault injection for tests */
static ekk_error_t g_recv_error_once = EKK_OK;

/* ============================================================================
 * TIME FUNCTIONS
 * ============================================================================ */

ekk_time_us_t ekk_hal_time_us(void)
{
    /* Use mock time if enabled (for testing) */
    if (g_mock_time_enabled) {
        return g_mock_time;
    }

#ifdef _WIN32
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (ekk_time_us_t)((now.QuadPart - g_start_time.QuadPart) * 1000000 / g_freq.QuadPart);
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int64_t elapsed_sec = (int64_t)now.tv_sec - (int64_t)g_start_time.tv_sec;
    int64_t elapsed_nsec = (int64_t)now.tv_nsec - (int64_t)g_start_time.tv_nsec;
    if (elapsed_nsec < 0) {
        elapsed_sec -= 1;
        elapsed_nsec += 1000000000LL;
    }

    return (ekk_time_us_t)(elapsed_sec * 1000000LL + elapsed_nsec / 1000LL);
#endif
}

/**
 * @brief Set mock time for testing
 * @param time_us Time value in microseconds (0 to disable mock)
 */
void ekk_hal_set_mock_time(ekk_time_us_t time_us)
{
    if (time_us == 0) {
        g_mock_time_enabled = false;
        g_mock_time = 0;
    } else {
        g_mock_time_enabled = true;
        g_mock_time = time_us;
    }
}

void ekk_hal_set_recv_error_once(ekk_error_t err)
{
    g_recv_error_once = err;
}

void ekk_hal_delay_us(uint32_t us)
{
#ifdef _WIN32
    /* Windows high-resolution sleep */
    if (us >= 1000) {
        Sleep(us / 1000);
    } else {
        /* Busy wait for sub-millisecond */
        ekk_time_us_t start = ekk_hal_time_us();
        while (ekk_hal_time_us() - start < us) {
            /* Spin */
        }
    }
#else
    usleep(us);
#endif
}

/* ============================================================================
 * MESSAGE TRANSMISSION
 * ============================================================================ */

ekk_error_t ekk_hal_send(ekk_module_id_t dest_id,
                          ekk_msg_type_t msg_type,
                          const void *data,
                          uint32_t len)
{
    EKK_UNUSED(dest_id);

    if (data == NULL && len > 0) {
        return EKK_ERR_INVALID_ARG;
    }

    if (len > MSG_MAX_LEN) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Add to queue */
    uint32_t state = ekk_hal_critical_enter();

    uint32_t next_head = (g_msg_head + 1) % MSG_QUEUE_SIZE;
    if (next_head == g_msg_tail) {
        ekk_hal_critical_exit(state);
        return EKK_ERR_NO_MEMORY;  /* Queue full */
    }

    hal_message_t *msg = &g_msg_queue[g_msg_head];
    msg->sender_id = g_module_id;
    msg->msg_type = msg_type;
    if (len > 0) {
        memcpy(msg->data, data, len);
    }
    msg->len = len;
    msg->valid = true;

    g_msg_head = next_head;

    /* Copy message data before exiting critical section so the ring buffer
     * slot can be safely reused without racing with the callback */
    uint8_t local_data[MSG_MAX_LEN];
    uint32_t local_len = msg->len;
    ekk_msg_type_t local_type = msg->msg_type;
    ekk_module_id_t local_sender = msg->sender_id;
    bool deliver = (g_recv_callback != NULL && dest_id == g_module_id);
    if (deliver && local_len <= sizeof(local_data)) {
        memcpy(local_data, msg->data, local_len);
    } else {
        deliver = false;
    }

    ekk_hal_critical_exit(state);

    /* Invoke callback with local copy outside critical section */
    if (deliver) {
        g_recv_callback(local_sender, local_type, local_data, local_len);
    }

    return EKK_OK;
}

ekk_error_t ekk_hal_broadcast(ekk_msg_type_t msg_type,
                               const void *data,
                               uint32_t len)
{
    return ekk_hal_send(EKK_BROADCAST_ID, msg_type, data, len);
}

ekk_error_t ekk_hal_recv(ekk_module_id_t *sender_id,
                          ekk_msg_type_t *msg_type,
                          void *data,
                          uint32_t *len)
{
    if (sender_id == NULL || msg_type == NULL || data == NULL || len == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    if (g_recv_error_once != EKK_OK) {
        ekk_error_t err = g_recv_error_once;
        g_recv_error_once = EKK_OK;
        return err;
    }

    uint32_t state = ekk_hal_critical_enter();

    if (g_msg_tail == g_msg_head) {
        ekk_hal_critical_exit(state);
        return EKK_ERR_NOT_FOUND;  /* Queue empty */
    }

    hal_message_t *msg = &g_msg_queue[g_msg_tail];
    if (!msg->valid) {
        ekk_hal_critical_exit(state);
        return EKK_ERR_NOT_FOUND;
    }

    *sender_id = msg->sender_id;
    *msg_type = msg->msg_type;

    uint32_t copy_len = (msg->len < *len) ? msg->len : *len;
    if (copy_len > 0) {
        memcpy(data, msg->data, copy_len);
    }
    *len = copy_len;

    msg->valid = false;
    g_msg_tail = (g_msg_tail + 1) % MSG_QUEUE_SIZE;

    ekk_hal_critical_exit(state);

    return EKK_OK;
}

void ekk_hal_set_recv_callback(ekk_hal_recv_cb callback)
{
    g_recv_callback = callback;
}

/* ============================================================================
 * CRITICAL SECTIONS
 * ============================================================================ */

uint32_t ekk_hal_critical_enter(void)
{
#ifdef _WIN32
    EnterCriticalSection(&g_critical);
    return 0;
#else
    pthread_mutex_lock(&g_mutex);
    return 0;
#endif
}

void ekk_hal_critical_exit(uint32_t state)
{
    EKK_UNUSED(state);
#ifdef _WIN32
    LeaveCriticalSection(&g_critical);
#else
    pthread_mutex_unlock(&g_mutex);
#endif
}

void ekk_hal_memory_barrier(void)
{
#ifdef _WIN32
    MemoryBarrier();
#else
    __sync_synchronize();
#endif
}

/* ============================================================================
 * ATOMIC OPERATIONS
 * ============================================================================ */

bool ekk_hal_cas32(volatile uint32_t *ptr, uint32_t expected, uint32_t desired)
{
#ifdef _WIN32
    return InterlockedCompareExchange((volatile LONG *)ptr, desired, expected) == (LONG)expected;
#else
    return __sync_bool_compare_and_swap(ptr, expected, desired);
#endif
}

uint32_t ekk_hal_atomic_inc(volatile uint32_t *ptr)
{
#ifdef _WIN32
    return InterlockedIncrement((volatile LONG *)ptr);
#else
    return __sync_add_and_fetch(ptr, 1);
#endif
}

uint32_t ekk_hal_atomic_dec(volatile uint32_t *ptr)
{
#ifdef _WIN32
    return InterlockedDecrement((volatile LONG *)ptr);
#else
    return __sync_sub_and_fetch(ptr, 1);
#endif
}

/* ============================================================================
 * SHARED MEMORY
 * ============================================================================ */

/** Global field region (static allocation for POSIX) */
static uint8_t g_field_region_storage[sizeof(ekk_field_region_t)];

void* ekk_hal_get_field_region(void)
{
    return g_field_region_storage;
}

void ekk_hal_sync_field_region(void)
{
    /* No-op for single-process simulation */
    ekk_hal_memory_barrier();
}

/* ============================================================================
 * PLATFORM INITIALIZATION
 * ============================================================================ */

ekk_error_t ekk_hal_init(void)
{
    if (g_hal_initialized) {
        return EKK_OK;
    }

#ifdef _WIN32
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start_time);
    InitializeCriticalSection(&g_critical);
#else
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
#endif

    /* Clear message queue */
    memset(g_msg_queue, 0, sizeof(g_msg_queue));
    g_msg_head = 0;
    g_msg_tail = 0;

    /* Clear field region */
    memset(g_field_region_storage, 0, sizeof(g_field_region_storage));
    g_recv_error_once = EKK_OK;

    g_hal_initialized = true;
    return EKK_OK;
}

const char* ekk_hal_platform_name(void)
{
#ifdef _WIN32
    return "Windows (POSIX HAL)";
#elif defined(__linux__)
    return "Linux (POSIX HAL)";
#elif defined(__APPLE__)
    return "macOS (POSIX HAL)";
#else
    return "POSIX HAL";
#endif
}

ekk_module_id_t ekk_hal_get_module_id(void)
{
    return g_module_id;
}

/**
 * @brief Set module ID for simulation
 */
void ekk_hal_set_module_id(ekk_module_id_t id)
{
    g_module_id = id;
}

/* ============================================================================
 * DEBUG OUTPUT
 * ============================================================================ */

void ekk_hal_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

void ekk_hal_assert_fail(const char *file, int line, const char *expr)
{
    fprintf(stderr, "ASSERTION FAILED: %s:%d: %s\n", file, line, expr);
    fflush(stderr);
    abort();
}
