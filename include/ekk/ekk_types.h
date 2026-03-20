/**
 * @file ekk_types.h
 * @brief EK-KOR v2 - Base types and configuration
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * @license MIT
 * SPDX-License-Identifier: MIT
 *
 * EK-KOR v2: Field-Centric Coordination RTOS
 *
 * NOVELTY CLAIMS:
 * - Potential field scheduling (no central scheduler)
 * - Topological k-neighbor coordination
 * - Threshold-based distributed consensus
 * - Adaptive mesh reformation
 */

#ifndef EKK_TYPES_H
#define EKK_TYPES_H

#ifdef EKK_VERIFICATION
#  include "ekk/ekk_stdint.h"
#else
#  include <stdint.h>
#endif
#ifndef EKK_VERIFICATION
#include <stddef.h>
#endif

#ifdef EKK_VERIFICATION
/* l4v C parser does not support C99 bool. Use unsigned int. */
typedef unsigned int ekk_bool_t;
#define EKK_TRUE  1u
#define EKK_FALSE 0u
#else
#include <stdbool.h>
typedef bool ekk_bool_t;
#define EKK_TRUE  true
#define EKK_FALSE false
#endif

/* ARM ACLE intrinsics for Cortex-M4 SIMD */
#if defined(__ARM_FEATURE_SIMD32) && __ARM_FEATURE_SIMD32
#include <arm_acle.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CONFIGURATION
 * ============================================================================ */

/**
 * @brief Number of topological neighbors per module
 *
 * Based on Cavagna & Giardina (2010): starling flocks use k=6-7 neighbors
 * for scale-free coordination. We use k=7 as default.
 */
#ifndef EKK_K_NEIGHBORS
#define EKK_K_NEIGHBORS             7U
#endif

/**
 * @brief Maximum modules in a cluster
 *
 * This extracted showcase keeps 8-bit module IDs, so the default is capped
 * to a smaller, internally consistent value.
 */
#ifndef EKK_MAX_MODULES
#define EKK_MAX_MODULES             64
#endif

/**
 * @brief Maximum tasks per module (internal to module)
 */
#ifndef EKK_MAX_TASKS_PER_MODULE
#define EKK_MAX_TASKS_PER_MODULE    8
#endif

/**
 * @brief Field decay time constant in microseconds
 *
 * Potential fields decay exponentially: field(t) = field(0) * exp(-t/tau)
 * Default: 100ms decay constant
 */
#ifndef EKK_FIELD_DECAY_TAU_US
#define EKK_FIELD_DECAY_TAU_US      100000
#endif

/**
 * @brief Heartbeat period in microseconds
 */
#ifndef EKK_HEARTBEAT_PERIOD_US
#define EKK_HEARTBEAT_PERIOD_US     10000   /* 10ms */
#endif

/**
 * @brief Heartbeat timeout (missed heartbeats before failure)
 */
#ifndef EKK_HEARTBEAT_TIMEOUT_COUNT
#define EKK_HEARTBEAT_TIMEOUT_COUNT 5       /* 50ms at 10ms period */
#endif

/**
 * @brief Consensus vote timeout in microseconds
 */
#ifndef EKK_VOTE_TIMEOUT_US
#define EKK_VOTE_TIMEOUT_US         50000   /* 50ms */
#endif

/**
 * @brief Maximum concurrent ballots
 */
#ifndef EKK_MAX_BALLOTS
#define EKK_MAX_BALLOTS             4
#endif

/* ============================================================================
 * BASIC TYPES
 * ============================================================================ */

/** @brief Module identifier (0 = invalid) */
typedef uint8_t ekk_module_id_t;

/** @brief Task identifier within a module */
typedef uint8_t ekk_task_id_t;

/** @brief Ballot identifier for consensus voting */
typedef uint16_t ekk_ballot_id_t;

/** @brief Timestamp in microseconds */
typedef uint64_t ekk_time_us_t;

/** @brief Tick count (system ticks) */
typedef uint32_t ekk_tick_t;

/** @brief Fixed-point Q16.16 for field values */
typedef int32_t ekk_fixed_t;

#define EKK_FIXED_ONE               (1 << 16)
#define EKK_FIXED_HALF              (1 << 15)
#define EKK_FIXED_QUARTER           (1 << 14)
#ifndef EKK_VERIFICATION
#define EKK_FLOAT_TO_FIXED(f)       ((ekk_fixed_t)((f) * 65536.0f))
#define EKK_FIXED_TO_FLOAT(x)       ((float)(x) / 65536.0f)
#endif
#define EKK_FIXED_MUL(a, b)         (((int64_t)(a) * (b)) >> 16)
/* WARNING: EKK_FIXED_DIV has no saturation and no div-by-zero guard.
 * Prefer ekk_fixed_div() which saturates correctly. */
#define EKK_FIXED_DIV(a, b)         (((int64_t)(a) << 16) / (b))

/* ============================================================================
 * FIELD COMPONENTS (defined early for use in gradient struct)
 * ============================================================================ */

typedef enum {
    EKK_FIELD_LOAD          = 0,    /**< Computational load potential */
    EKK_FIELD_THERMAL       = 1,    /**< Thermal gradient */
    EKK_FIELD_POWER         = 2,    /**< Power consumption */
    EKK_FIELD_CUSTOM_0      = 3,    /**< Application-defined */
    EKK_FIELD_CUSTOM_1      = 4,    /**< Application-defined */
    EKK_FIELD_SLACK         = 5,    /**< Deadline slack (MAPF-HET integration) */
    EKK_FIELD_COUNT         = 6,
} ekk_field_component_t;

/* ============================================================================
 * Q15 FIXED-POINT (SIMD-OPTIMIZED GRADIENTS)
 * ============================================================================ */

/**
 * @brief Fixed-point Q1.15 for gradient storage (SIMD-friendly)
 *
 * Q15 format: 1 sign bit + 15 fractional bits
 * Range: [-1.0, +0.99997] with ~0.00003 resolution
 *
 * Benefits:
 * - Fits in 16 bits (half the size of Q16.16)
 * - Cortex-M4 SIMD can process 2 values at once via __SSUB16/__SADD16
 * - Sufficient precision for gradient-based scheduling decisions
 */
typedef int16_t ekk_q15_t;

#define EKK_Q15_ONE                 0x7FFF      /**< +0.99997 (max positive) */
#define EKK_Q15_HALF                0x4000      /**< +0.5 */
#define EKK_Q15_ZERO                0x0000      /**< 0.0 */
#define EKK_Q15_NEG_ONE             ((int16_t)0x8000)  /**< -1.0 */
#define EKK_Q15_NEG_HALF            ((int16_t)0xC000)  /**< -0.5 */

/**
 * @brief Convert Q16.16 to Q15 (saturating)
 *
 * Shifts right by 1 bit (Q16.16 → Q1.15 range) and saturates.
 * Values outside [-1.0, +1.0) are clamped.
 */
static inline ekk_q15_t ekk_fixed_to_q15(ekk_fixed_t f) {
    int32_t shifted = f >> 1;
    if (shifted > 0x7FFF) return 0x7FFF;
    if (shifted < -0x8000) return (ekk_q15_t)0x8000;
    return (ekk_q15_t)shifted;
}

/**
 * @brief Convert Q15 to Q16.16
 */
static inline ekk_fixed_t ekk_q15_to_fixed(ekk_q15_t q) {
    return ((ekk_fixed_t)q) << 1;
}

/**
 * @brief Q15 multiplication: (a * b) >> 15
 */
static inline ekk_q15_t ekk_q15_mul(ekk_q15_t a, ekk_q15_t b) {
    int32_t result = ((int32_t)a * (int32_t)b) >> 15;
    if (result > 0x7FFF) return 0x7FFF;
    if (result < -0x8000) return (ekk_q15_t)0x8000;
    return (ekk_q15_t)result;
}

/**
 * @brief Q15 saturating addition
 */
static inline ekk_q15_t ekk_q15_add_sat(ekk_q15_t a, ekk_q15_t b) {
    int32_t result = (int32_t)a + (int32_t)b;
    if (result > 0x7FFF) return 0x7FFF;
    if (result < -0x8000) return (ekk_q15_t)0x8000;
    return (ekk_q15_t)result;
}

/**
 * @brief Q15 saturating subtraction
 */
static inline ekk_q15_t ekk_q15_sub_sat(ekk_q15_t a, ekk_q15_t b) {
    int32_t result = (int32_t)a - (int32_t)b;
    if (result > 0x7FFF) return 0x7FFF;
    if (result < -0x8000) return (ekk_q15_t)0x8000;
    return (ekk_q15_t)result;
}

/**
 * @brief Gradient vector using Q15 for SIMD optimization
 *
 * Stores EKK_FIELD_COUNT gradients in Q15 format.
 * On Cortex-M4, adjacent pairs can be processed with single SIMD instruction.
 */
typedef struct {
    ekk_q15_t components[EKK_FIELD_COUNT];
} ekk_gradient_t;

/* SIMD intrinsics for Cortex-M4 DSP extension */
#if defined(__ARM_FEATURE_SIMD32) && __ARM_FEATURE_SIMD32

/**
 * @brief SIMD: Compute 2 Q15 gradients in parallel (neighbor - my)
 *
 * Uses __ssub16 to subtract two pairs of 16-bit values simultaneously.
 * Result = packed(neighbor[1] - my[1], neighbor[0] - my[0])
 */
static inline uint32_t ekk_gradient_simd2_sub(uint32_t my_pair, uint32_t neigh_pair) {
    return __ssub16(neigh_pair, my_pair);
}

/**
 * @brief SIMD: Add two pairs of Q15 values
 */
static inline uint32_t ekk_gradient_simd2_add(uint32_t a, uint32_t b) {
    return __sadd16(a, b);
}

#endif /* __ARM_FEATURE_SIMD32 */

/* ============================================================================
 * ERROR CODES
 * ============================================================================ */

typedef enum {
    EKK_OK                  = 0,
    EKK_ERR_INVALID_ARG     = -1,
    EKK_ERR_NO_MEMORY       = -2,
    EKK_ERR_TIMEOUT         = -3,
    EKK_ERR_BUSY            = -4,
    EKK_ERR_NOT_FOUND       = -5,
    EKK_ERR_ALREADY_EXISTS  = -6,
    EKK_ERR_NO_QUORUM       = -7,
    EKK_ERR_INHIBITED       = -8,
    EKK_ERR_NEIGHBOR_LOST   = -9,
    EKK_ERR_FIELD_EXPIRED   = -10,
    EKK_ERR_HAL_FAILURE     = -11,
    EKK_ERR_NOT_SUPPORTED   = -12,  /**< Feature not supported on platform */
    EKK_ERR_LIMIT           = -13,  /**< Resource limit exceeded */
    EKK_ERR_DEGRADED        = -14,  /**< Operation completed in degraded mode */
} ekk_error_t;

/* ============================================================================
 * DEADLINE / SLACK (MAPF-HET Integration)
 * ============================================================================ */

/**
 * @brief Slack threshold for critical deadline detection (10 seconds)
 *
 * Tasks with slack below this threshold are marked critical and get
 * priority in gradient-based scheduling decisions.
 */
#define EKK_SLACK_THRESHOLD_US      10000000

/**
 * @brief Deadline information for a task
 *
 * Used for deadline-aware task selection via slack field gradient.
 * Slack computation: slack = deadline - (now + duration_estimate)
 *
 * MAPF-HET Integration: From deadline_cbs.go:231-270
 * Modules with tight deadlines (low slack) get priority through
 * the EKK_FIELD_SLACK gradient mechanism.
 */
typedef struct {
    ekk_time_us_t deadline;         /**< Absolute deadline timestamp */
    ekk_time_us_t duration_est;     /**< Estimated task duration */
    ekk_fixed_t slack;              /**< Computed slack (Q16.16) */
    ekk_bool_t critical;            /**< True if slack < SLACK_THRESHOLD_US */
} ekk_deadline_t;

/* ============================================================================
 * CAPABILITY BITMASK (MAPF-HET Integration)
 * ============================================================================ */

/**
 * @brief Module capability bitmask
 *
 * Even identical EK3 modules have runtime heterogeneity:
 * - Thermal state varies (some modules cooler than others)
 * - V2G capability depends on configuration
 * - Gateway role assigned dynamically
 *
 * MAPF-HET Integration: Enables capability-based task assignment
 * where tasks are only assigned to modules with matching capabilities.
 */
typedef uint16_t ekk_capability_t;

/* Standard capability flags (bits 0-3: runtime state) */
#define EKK_CAP_THERMAL_OK      (1 << 0)    /**< Within thermal limits */
#define EKK_CAP_POWER_HIGH      (1 << 1)    /**< High power mode available */
#define EKK_CAP_GATEWAY         (1 << 2)    /**< Can aggregate/route messages */
#define EKK_CAP_V2G             (1 << 3)    /**< Bidirectional power capable */

/* MAPF-HET Task Type Mapping (bits 4-7)
 *
 * Maps EK-KOR capability bits to MAPF-HET robot types for planning-execution bridge.
 * See mapf-het-research/internal/core/types.go for corresponding Go definitions.
 *
 * TypeA (Mobile, holonomic): CAP_SWAP_MODULE | CAP_DIAGNOSE
 * TypeB (Rail-mounted): CAP_SWAP_BATTERY | CAP_SWAP_MODULE | CAP_DIAGNOSE
 * TypeC (Aerial/drone): CAP_AERIAL_TASK only
 */
#define EKK_CAP_SWAP_BATTERY    (1 << 4)    /**< Full battery pack swap (TypeB only) */
#define EKK_CAP_SWAP_MODULE     (1 << 5)    /**< Single EK3 module swap (TypeA or TypeB) */
#define EKK_CAP_AERIAL_TASK     (1 << 6)    /**< Aerial inspection/delivery (TypeC only) */
#define EKK_CAP_DIAGNOSE        (1 << 7)    /**< Diagnostic scan capability (TypeA or TypeB) */

/* Application-defined capability flags (bits 8-11) */
#define EKK_CAP_CUSTOM_0        (1 << 8)    /**< Application-defined 0 */
#define EKK_CAP_CUSTOM_1        (1 << 9)    /**< Application-defined 1 */
#define EKK_CAP_CUSTOM_2        (1 << 10)   /**< Application-defined 2 */
#define EKK_CAP_CUSTOM_3        (1 << 11)   /**< Application-defined 3 */

/* Predefined capability sets for common robot types */
#define EKK_CAP_ROBOT_TYPE_A    (EKK_CAP_SWAP_MODULE | EKK_CAP_DIAGNOSE)
#define EKK_CAP_ROBOT_TYPE_B    (EKK_CAP_SWAP_BATTERY | EKK_CAP_SWAP_MODULE | EKK_CAP_DIAGNOSE)
#define EKK_CAP_ROBOT_TYPE_C    (EKK_CAP_AERIAL_TASK)

/**
 * @brief Check if module has required capabilities
 *
 * @param have Module's current capabilities
 * @param need Required capabilities for task
 * @return true if all required capabilities are present
 */
static inline ekk_bool_t ekk_can_perform(ekk_capability_t have, ekk_capability_t need) {
    return (have & need) == need;
}

/* ============================================================================
 * MODULE ROLE
 * ============================================================================ */

/**
 * @brief Module role in the EK-KOR cluster
 *
 * Different roles have different responsibilities:
 * - ChargerModule: Individual 3.3kW power module, publishes thermal/load fields
 * - SegmentGateway: Bridges segments, aggregates fields, routes messages
 *
 * Role is set at compile time via EKK_ROLE_GATEWAY CMake option,
 * or at runtime during module initialization.
 */
typedef enum {
    EKK_ROLE_CHARGER_MODULE  = 0,   /**< Standard charging module (L1 node) */
    EKK_ROLE_SEGMENT_GATEWAY = 1,   /**< Segment gateway/aggregator (L2 node) */
    EKK_ROLE_SUPERVISOR      = 2,   /**< Safety supervisor (L3, optional) */
} ekk_module_role_t;

/**
 * @brief Default role (can be overridden by CMake)
 */
#ifndef EKK_DEFAULT_ROLE
#ifdef EKK_ROLE_GATEWAY
#define EKK_DEFAULT_ROLE        EKK_ROLE_SEGMENT_GATEWAY
#else
#define EKK_DEFAULT_ROLE        EKK_ROLE_CHARGER_MODULE
#endif
#endif

/* ============================================================================
 * MODULE STATE
 * ============================================================================ */

typedef enum {
    EKK_MODULE_INIT         = 0,    /**< Initializing, not yet in mesh */
    EKK_MODULE_DISCOVERING  = 1,    /**< Discovering neighbors */
    EKK_MODULE_ACTIVE       = 2,    /**< Normal operation */
    EKK_MODULE_DEGRADED     = 3,    /**< Some neighbors lost */
    EKK_MODULE_ISOLATED     = 4,    /**< No neighbors reachable */
    EKK_MODULE_REFORMING    = 5,    /**< Mesh reformation in progress */
    EKK_MODULE_SHUTDOWN     = 6,    /**< Graceful shutdown */
} ekk_module_state_t;

/** Convert module state to string (for debug output) */
const char* ekk_module_state_str(ekk_module_state_t state);

/* ============================================================================
 * HEALTH STATE (per neighbor)
 * ============================================================================ */

typedef enum {
    EKK_HEALTH_UNKNOWN      = 0,    /**< Never seen */
    EKK_HEALTH_ALIVE        = 1,    /**< Recent heartbeat */
    EKK_HEALTH_SUSPECT      = 2,    /**< Missed 1-2 heartbeats */
    EKK_HEALTH_DEAD         = 3,    /**< Timeout exceeded */
} ekk_health_state_t;

/* ============================================================================
 * VOTE VALUES
 * ============================================================================ */

typedef enum {
    EKK_VOTE_ABSTAIN        = 0,    /**< No vote cast */
    EKK_VOTE_YES            = 1,    /**< Approve proposal */
    EKK_VOTE_NO             = 2,    /**< Reject proposal */
    EKK_VOTE_INHIBIT        = 3,    /**< Block competing proposal */
} ekk_vote_value_t;

typedef enum {
    EKK_VOTE_PENDING        = 0,    /**< Voting in progress */
    EKK_VOTE_APPROVED       = 1,    /**< Threshold reached (yes) */
    EKK_VOTE_REJECTED       = 2,    /**< Threshold not reached */
    EKK_VOTE_TIMEOUT        = 3,    /**< Voting timed out */
    EKK_VOTE_CANCELLED      = 4,    /**< Cancelled by inhibition */
} ekk_vote_result_t;

/* ============================================================================
 * COORDINATION FIELD STRUCTURE
 * ============================================================================ */

/**
 * @brief Coordination field published by each module
 *
 * This is the core data structure for potential field scheduling.
 * Each module publishes its field; neighbors sample and compute gradients.
 *
 * Fields decay exponentially with time constant EKK_FIELD_DECAY_TAU_US.
 */
typedef struct {
    ekk_fixed_t components[EKK_FIELD_COUNT];  /**< Field values (Q16.16) */
    ekk_time_us_t timestamp;                   /**< When published */
    ekk_module_id_t source;                    /**< Publishing module */
    uint8_t sequence;                          /**< Monotonic sequence number */
} ekk_field_t;

/* ============================================================================
 * NEIGHBOR INFO
 * ============================================================================ */

/**
 * @brief Information about a neighbor module
 */
typedef struct {
    ekk_module_id_t id;                        /**< Neighbor's module ID */
    ekk_health_state_t health;                 /**< Current health state */
    ekk_time_us_t last_seen;                   /**< Last heartbeat timestamp */
    ekk_field_t last_field;                    /**< Last received field */
    int32_t logical_distance;                  /**< Distance metric for k-selection */
    uint8_t missed_heartbeats;                 /**< Consecutive missed */
    ekk_capability_t capabilities;             /**< Neighbor's capabilities (MAPF-HET stub — not exchanged in this extract) */
} ekk_neighbor_t;

/* ============================================================================
 * UTILITY MACROS
 * ============================================================================ */

#define EKK_INVALID_MODULE_ID       0
#define EKK_INVALID_BALLOT_ID       0
#define EKK_BROADCAST_ID            0xFF

/* No side effects in arguments -- double-evaluation is safe */
#define EKK_MIN(a, b)  ((a) < (b) ? (a) : (b))
#define EKK_MAX(a, b)  ((a) > (b) ? (a) : (b))
#define EKK_CLAMP(x, lo, hi)        EKK_MIN(EKK_MAX(x, lo), hi)

#define EKK_ARRAY_SIZE(arr)         (sizeof(arr) / sizeof((arr)[0]))

#ifndef EKK_VERIFICATION
#define EKK_UNUSED(x)               ((void)(x))
#endif

/* ============================================================================
 * COMPILER COMPATIBILITY
 * ============================================================================ */

#ifdef EKK_VERIFICATION
/* l4v C parser does not support __attribute__, _Static_assert, or pragmas.
 * Define all compiler-specific macros as no-ops for the verification build. */
#ifndef EKK_WEAK
#define EKK_WEAK
#endif
#ifndef EKK_PACKED
#define EKK_PACKED
#endif
#define EKK_PACK_BEGIN
#define EKK_PACK_END
#define EKK_STATIC_ASSERT(cond, msg)
#define EKK_UNUSED(x)

#elif defined(_MSC_VER)
/* Microsoft Visual C++ */

/** @brief Weak symbol - not supported in MSVC, functions are always strong */
#ifndef EKK_WEAK
#define EKK_WEAK
#endif

/** @brief Packed struct - use pragma pack for MSVC */
#ifndef EKK_PACKED
#define EKK_PACKED
#endif

/** @brief Start packed region (use before struct) */
#define EKK_PACK_BEGIN  __pragma(pack(push, 1))
/** @brief End packed region (use after struct) */
#define EKK_PACK_END    __pragma(pack(pop))

/** @brief Static assertion */
#define EKK_STATIC_ASSERT(cond, msg) static_assert(cond, msg)

#else
/* GCC / Clang (including ARM GCC for MCU) */

/** @brief Weak symbol for optional overrides */
#ifndef EKK_WEAK
#define EKK_WEAK                    __attribute__((weak))
#endif

/** @brief Packed struct for wire format */
#ifndef EKK_PACKED
#define EKK_PACKED                  __attribute__((packed))
#endif

/** @brief Start packed region (no-op for GCC, uses attribute) */
#define EKK_PACK_BEGIN
/** @brief End packed region (no-op for GCC) */
#define EKK_PACK_END

/** @brief Static assertion */
#define EKK_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

#endif /* EKK_VERIFICATION / _MSC_VER / GCC */

#ifdef __cplusplus
}
#endif

#endif /* EKK_TYPES_H */
