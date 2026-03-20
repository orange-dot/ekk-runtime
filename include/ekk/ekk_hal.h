/**
 * @file ekk_hal.h
 * @brief EK-KOR v2 - Hardware Abstraction Layer
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * @license MIT
 * SPDX-License-Identifier: MIT
 *
 * HAL provides platform-independent interface for:
 * - Time measurement
 * - Message transmission (CAN-FD, shared memory, etc.)
 * - Critical sections
 * - Platform-specific initialization
 *
 * Supported platforms:
 * - STM32G474 (Cortex-M4, primary target)
 * - TriCore TC397XP (future)
 * - POSIX simulation (testing)
 */

#ifndef EKK_HAL_H
#define EKK_HAL_H

#include "ekk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * MESSAGE TYPES
 * ============================================================================ */

/**
 * @brief Message types for inter-module communication
 */
typedef enum {
    EKK_MSG_HEARTBEAT   = 0x01,     /**< Liveness check */
    EKK_MSG_DISCOVERY   = 0x02,     /**< Module discovery */
    EKK_MSG_FIELD       = 0x03,     /**< Coordination field update */
    EKK_MSG_PROPOSAL    = 0x04,     /**< Consensus proposal */
    EKK_MSG_VOTE        = 0x05,     /**< Consensus vote */
    EKK_MSG_INHIBIT     = 0x06,     /**< Proposal inhibition */
    EKK_MSG_REFORM      = 0x07,     /**< Mesh reformation */
    EKK_MSG_SHUTDOWN    = 0x08,     /**< Graceful shutdown */
    EKK_MSG_USER_BASE   = 0x80,     /**< Application messages start here */
} ekk_msg_type_t;

/* ============================================================================
 * TIME FUNCTIONS
 * ============================================================================ */

/**
 * @brief Get current time in microseconds
 *
 * Must be monotonically increasing. Wraparound handled by caller.
 *
 * @return Current time in microseconds
 */
ekk_time_us_t ekk_hal_time_us(void);

/**
 * @brief Get current time in milliseconds
 */
static inline uint32_t ekk_hal_time_ms(void)
{
    return (uint32_t)(ekk_hal_time_us() / 1000);
}

/**
 * @brief Busy-wait delay
 *
 * @param us Microseconds to wait
 */
void ekk_hal_delay_us(uint32_t us);

/**
 * @brief Set mock time for testing (POSIX HAL only)
 *
 * @param time_us Time value in microseconds (0 to disable mock and use real time)
 */
void ekk_hal_set_mock_time(ekk_time_us_t time_us);

/**
 * @brief Force the next `ekk_hal_recv()` call to return a specific error
 *
 * Host-side simulation and test hook. The forced error is consumed by the next
 * receive attempt and then cleared automatically.
 */
void ekk_hal_set_recv_error_once(ekk_error_t err);

/* ============================================================================
 * MESSAGE TRANSMISSION
 * ============================================================================ */

/**
 * @brief Send message to specific module
 *
 * @param dest_id Destination module ID (or EKK_BROADCAST_ID)
 * @param msg_type Message type
 * @param data Message payload
 * @param len Payload length
 * @return EKK_OK on success
 */
ekk_error_t ekk_hal_send(ekk_module_id_t dest_id,
                          ekk_msg_type_t msg_type,
                          const void *data,
                          uint32_t len);

/**
 * @brief Broadcast message to all modules
 *
 * @param msg_type Message type
 * @param data Message payload
 * @param len Payload length
 * @return EKK_OK on success
 */
ekk_error_t ekk_hal_broadcast(ekk_msg_type_t msg_type,
                               const void *data,
                               uint32_t len);

/**
 * @brief Check for received message
 *
 * Non-blocking check for incoming messages.
 *
 * @param[out] sender_id Sender's module ID
 * @param[out] msg_type Message type
 * @param[out] data Buffer for payload
 * @param[in,out] len Max length in, actual length out
 * @return EKK_OK if message received, EKK_ERR_NOT_FOUND if no message
 */
ekk_error_t ekk_hal_recv(ekk_module_id_t *sender_id,
                          ekk_msg_type_t *msg_type,
                          void *data,
                          uint32_t *len);

/**
 * @brief Message receive callback type
 */
typedef void (*ekk_hal_recv_cb)(ekk_module_id_t sender_id,
                                 ekk_msg_type_t msg_type,
                                 const void *data,
                                 uint32_t len);

/**
 * @brief Register receive callback (interrupt-driven platforms)
 */
void ekk_hal_set_recv_callback(ekk_hal_recv_cb callback);

/* ============================================================================
 * CRITICAL SECTIONS
 * ============================================================================ */

/**
 * @brief Enter critical section (disable interrupts)
 *
 * @return State to restore
 */
uint32_t ekk_hal_critical_enter(void);

/**
 * @brief Exit critical section
 *
 * @param state State from ekk_hal_critical_enter()
 */
void ekk_hal_critical_exit(uint32_t state);

#ifdef EKK_VERIFICATION
/* Sequential memory model for l4v -- barriers and atomics are identity operations.
 * The proof verifies functional correctness under sequential execution.
 * Concurrent safety of the seqlock is a separate argument. */
#  define ekk_hal_memory_barrier()   ((void)0)
#  define ekk_hal_atomic_inc(ptr)    (++(*(ptr)))
#  define ekk_hal_atomic_dec(ptr)    (--(*(ptr)))
#  define ekk_hal_cas32(ptr, expected, desired) \
    (*(ptr) == (expected) ? (*(ptr) = (desired), EKK_TRUE) : EKK_FALSE)
#else
/**
 * @brief Memory barrier
 */
void ekk_hal_memory_barrier(void);

/* ============================================================================
 * ATOMIC OPERATIONS
 * ============================================================================ */

/**
 * @brief Atomic compare-and-swap
 *
 * @param ptr Pointer to value
 * @param expected Expected current value
 * @param desired Value to set if current == expected
 * @return true if swapped, false otherwise
 */
ekk_bool_t ekk_hal_cas32(volatile uint32_t *ptr, uint32_t expected, uint32_t desired);

/**
 * @brief Atomic increment
 *
 * @return Value after increment
 */
uint32_t ekk_hal_atomic_inc(volatile uint32_t *ptr);

/**
 * @brief Atomic decrement
 *
 * @return Value after decrement
 */
uint32_t ekk_hal_atomic_dec(volatile uint32_t *ptr);
#endif /* EKK_VERIFICATION */

/* ============================================================================
 * SHARED MEMORY (for coordination fields)
 * ============================================================================ */

/**
 * @brief Get pointer to shared field region
 *
 * On single-core: returns static region
 * On multi-core: returns shared memory address
 * On networked: returns local cache (synced via messages)
 *
 * @return Pointer to field region
 */
void* ekk_hal_get_field_region(void);

/**
 * @brief Synchronize shared field region
 *
 * Called after publishing to ensure visibility.
 */
void ekk_hal_sync_field_region(void);

/* ============================================================================
 * PLATFORM INITIALIZATION
 * ============================================================================ */

/**
 * @brief Initialize HAL
 *
 * Platform-specific initialization.
 *
 * @return EKK_OK on success
 */
ekk_error_t ekk_hal_init(void);

/**
 * @brief Get platform name
 */
const char* ekk_hal_platform_name(void);

/**
 * @brief Get this module's hardware ID
 *
 * Returns unique ID based on hardware (e.g., MCU unique ID).
 */
ekk_module_id_t ekk_hal_get_module_id(void);

/**
 * @brief Override module ID for host-side simulations
 *
 * Useful when running multiple logical modules in a single process.
 */
void ekk_hal_set_module_id(ekk_module_id_t id);

/* ============================================================================
 * DEBUG OUTPUT
 * ============================================================================ */

/**
 * @brief Debug print (printf-like)
 */
void ekk_hal_printf(const char *fmt, ...);

/**
 * @brief Assert handler
 */
void ekk_hal_assert_fail(const char *file, int line, const char *expr);

#define EKK_ASSERT(expr) \
    do { if (!(expr)) ekk_hal_assert_fail(__FILE__, __LINE__, #expr); } while(0)

#ifdef __cplusplus
}
#endif

#endif /* EKK_HAL_H */
