/**
 * @file ekk_types.c
 * @brief EK-KOR v2 - Type utilities and fixed-point math
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 */

#include "ekk/ekk_types.h"
#include <string.h>

/* ============================================================================
 * STATIC ASSERTIONS
 * ============================================================================ */

EKK_STATIC_ASSERT(sizeof(ekk_module_id_t) == 1, "module_id must be 1 byte");
EKK_STATIC_ASSERT(sizeof(ekk_ballot_id_t) == 2, "ballot_id must be 2 bytes");
EKK_STATIC_ASSERT(sizeof(ekk_time_us_t) == 8, "time_us must be 8 bytes");
EKK_STATIC_ASSERT(sizeof(ekk_fixed_t) == 4, "fixed must be 4 bytes");
EKK_STATIC_ASSERT(EKK_K_NEIGHBORS >= 3, "k-neighbors must be at least 3");
EKK_STATIC_ASSERT(EKK_K_NEIGHBORS <= 15, "k-neighbors should not exceed 15");
EKK_STATIC_ASSERT(EKK_MAX_MODULES <= 256, "max modules limited to uint8_t range");
EKK_STATIC_ASSERT(EKK_FIELD_COUNT == 6, "field count must be 6");

/* ============================================================================
 * FIXED-POINT ARITHMETIC
 * ============================================================================ */

/**
 * @brief Multiply two Q16.16 fixed-point numbers
 */
ekk_fixed_t ekk_fixed_mul(ekk_fixed_t a, ekk_fixed_t b)
{
    int64_t result = ((int64_t)a * (int64_t)b) >> 16;
    if (result > (int64_t)INT32_MAX) return INT32_MAX;
    if (result < (int64_t)INT32_MIN) return INT32_MIN;
    return (ekk_fixed_t)result;
}

/**
 * @brief Divide two Q16.16 fixed-point numbers
 */
ekk_fixed_t ekk_fixed_div(ekk_fixed_t a, ekk_fixed_t b)
{
    if (b == 0) {
        return (a >= 0) ? INT32_MAX : INT32_MIN;
    }
    int64_t result = (((int64_t)a) << 16) / b;
    if (result > (int64_t)INT32_MAX) return INT32_MAX;
    if (result < (int64_t)INT32_MIN) return INT32_MIN;
    return (ekk_fixed_t)result;
}

/**
 * @brief Compute exponential decay approximation (piecewise linear)
 *
 * Approximates exp(-elapsed/tau) using piecewise linear segments.
 * Error is < 15% vs true exponential.
 *
 * @param elapsed_us Time elapsed in microseconds
 * @param tau_us Time constant in microseconds
 * @return Decay factor in Q16.16 format [0.0, 1.0]
 */
ekk_fixed_t ekk_fixed_exp_decay(ekk_time_us_t elapsed_us, ekk_time_us_t tau_us)
{
    if (tau_us == 0) return 0;
    if (elapsed_us == 0) return EKK_FIXED_ONE;
    if (tau_us > (UINT64_MAX / 5)) return 0;
    if (elapsed_us >= tau_us * 5) return 0;

    /* Guard against left-shift overflow: if elapsed_us is large enough
     * that (elapsed_us << 16) would overflow uint64_t, the signal is
     * fully decayed anyway. */
    if (elapsed_us > (UINT64_MAX >> 16)) return 0;

    uint64_t ratio_q16 = (elapsed_us << 16) / tau_us;
    if (ratio_q16 > (uint64_t)INT32_MAX) return 0;
    ekk_fixed_t x = (ekk_fixed_t)ratio_q16;

    /* exp(-x) ≈ 1/(1 + x + x²/2) */
    ekk_fixed_t x2 = ekk_fixed_mul(x, x);
    ekk_fixed_t d = EKK_FIXED_ONE + x + (x2 >> 1);
    return ekk_fixed_div(EKK_FIXED_ONE, d);
}

/**
 * @brief Linear interpolation between two Q16.16 values
 *
 * @param a Start value
 * @param b End value
 * @param t Interpolation factor [0.0, 1.0] in Q16.16
 * @return a + (b - a) * t
 */
ekk_fixed_t ekk_fixed_lerp(ekk_fixed_t a, ekk_fixed_t b, ekk_fixed_t t)
{
    /* Clamp t to [0, 1] */
    if (t <= 0) return a;
    if (t >= EKK_FIXED_ONE) return b;

    int64_t diff = (int64_t)b - (int64_t)a;
    int64_t scaled = (diff * t) >> 16;
    return a + (ekk_fixed_t)scaled;
}

/**
 * @brief Absolute value of Q16.16
 */
ekk_fixed_t ekk_fixed_abs(ekk_fixed_t x)
{
    if (x == INT32_MIN) return INT32_MAX;
    return (x >= 0) ? x : -x;
}

/**
 * @brief Square root approximation for Q16.16
 *
 * Uses integer Newton-Raphson iteration.
 * Only valid for non-negative inputs.
 */
ekk_fixed_t ekk_fixed_sqrt(ekk_fixed_t x)
{
    if (x <= 0) return 0;

    /* Initial guess: shift right by 8 (sqrt of Q16.16 scale) */
    uint32_t guess = (uint32_t)x;
    uint32_t root = guess >> 8;
    if (root == 0) root = 1;

    /* Newton-Raphson: root = (root + x/root) / 2 */
    for (int i = 0; i < 8; i++) {
        uint32_t div = guess / root;
        root = (root + div) >> 1;
    }

    return (ekk_fixed_t)(root << 8);
}

/* ============================================================================
 * FIELD UTILITIES
 * ============================================================================ */

/**
 * @brief Initialize a field to zero
 */
void ekk_field_clear(ekk_field_t *field)
{
    memset(field, 0, sizeof(ekk_field_t));
}

/**
 * @brief Fill all field components with the same value
 */
void ekk_field_fill(ekk_field_t *field, ekk_fixed_t value)
{
    for (int i = 0; i < EKK_FIELD_COUNT; i++) {
        field->components[i] = value;
    }
}

/**
 * @brief Check if field has been initialized (has a source and timestamp)
 */
ekk_bool_t ekk_field_is_initialized(const ekk_field_t *field)
{
    return field->source != EKK_INVALID_MODULE_ID && field->timestamp > 0;
}

/* NOTE: ekk_field_add, ekk_field_scale, ekk_field_lerp are in ekk_field.c */

/* ============================================================================
 * ERROR STRING CONVERSION
 * ============================================================================ */

/**
 * @brief Convert error code to string
 */
const char* ekk_error_str(ekk_error_t err)
{
    switch (err) {
        case EKK_OK:                return "OK";
        case EKK_ERR_INVALID_ARG:   return "INVALID_ARG";
        case EKK_ERR_NO_MEMORY:     return "NO_MEMORY";
        case EKK_ERR_TIMEOUT:       return "TIMEOUT";
        case EKK_ERR_BUSY:          return "BUSY";
        case EKK_ERR_NOT_FOUND:     return "NOT_FOUND";
        case EKK_ERR_ALREADY_EXISTS:return "ALREADY_EXISTS";
        case EKK_ERR_NO_QUORUM:     return "NO_QUORUM";
        case EKK_ERR_INHIBITED:     return "INHIBITED";
        case EKK_ERR_NEIGHBOR_LOST: return "NEIGHBOR_LOST";
        case EKK_ERR_FIELD_EXPIRED: return "FIELD_EXPIRED";
        case EKK_ERR_HAL_FAILURE:   return "HAL_FAILURE";
        case EKK_ERR_DEGRADED:      return "DEGRADED";
        default:                    return "UNKNOWN";
    }
}

/**
 * @brief Convert module state to string
 */
const char* ekk_module_state_str(ekk_module_state_t state)
{
    switch (state) {
        case EKK_MODULE_INIT:       return "INIT";
        case EKK_MODULE_DISCOVERING:return "DISCOVERING";
        case EKK_MODULE_ACTIVE:     return "ACTIVE";
        case EKK_MODULE_DEGRADED:   return "DEGRADED";
        case EKK_MODULE_ISOLATED:   return "ISOLATED";
        case EKK_MODULE_REFORMING:  return "REFORMING";
        case EKK_MODULE_SHUTDOWN:   return "SHUTDOWN";
        default:                    return "UNKNOWN";
    }
}

/**
 * @brief Convert health state to string
 */
const char* ekk_health_state_str(ekk_health_state_t state)
{
    switch (state) {
        case EKK_HEALTH_UNKNOWN:    return "UNKNOWN";
        case EKK_HEALTH_ALIVE:      return "ALIVE";
        case EKK_HEALTH_SUSPECT:    return "SUSPECT";
        case EKK_HEALTH_DEAD:       return "DEAD";
        default:                    return "UNKNOWN";
    }
}

/**
 * @brief Convert vote result to string
 */
const char* ekk_vote_result_str(ekk_vote_result_t result)
{
    switch (result) {
        case EKK_VOTE_PENDING:      return "PENDING";
        case EKK_VOTE_APPROVED:     return "APPROVED";
        case EKK_VOTE_REJECTED:     return "REJECTED";
        case EKK_VOTE_TIMEOUT:      return "TIMEOUT";
        case EKK_VOTE_CANCELLED:    return "CANCELLED";
        default:                    return "UNKNOWN";
    }
}
