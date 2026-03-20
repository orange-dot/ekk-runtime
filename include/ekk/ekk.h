/**
 * @file ekk.h
 * @brief EK-KOR v2 - Master Include File
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * @license MIT
 * SPDX-License-Identifier: MIT
 *
 * EK-KOR agentic extract: a small coordination runtime, not a kernel.
 *
 * This header aggregates the runtime pieces that survived extraction from the
 * older `ek-korv2` codebase:
 *
 * - decaying shared fields
 * - event-driven `k`-nearest topology maintenance
 * - heartbeat/liveness tracking
 * - per-ballot threshold voting
 * - module-local task arbitration
 *
 * The current extract is intentionally narrow:
 *
 * - it is single-threaded and non-reentrant by design
 * - it is meant for concept review and disciplined iteration
 * - it does not claim to be a full RTOS or a formally specified protocol stack
 *
 * USAGE:
 *
 * @code
 * #include <ekk/ekk.h>
 *
 * ekk_module_t my_module;
 *
 * int main(void) {
 *     ekk_hal_init();
 *
 *     ekk_position_t pos = {.x = 1, .y = 2, .z = 0};
 *     ekk_module_init(&my_module, 42, "charger-42", pos);
 *
 *     ekk_module_add_task(&my_module, "charge", charge_task, NULL, 0, 1000, NULL);
 *     ekk_module_add_task(&my_module, "thermal", thermal_task, NULL, 1, 5000, NULL);
 *
 *     ekk_module_start(&my_module);
 *
 *     while (1) {
 *         ekk_time_us_t now = ekk_hal_time_us();
 *         ekk_error_t tick = ekk_module_tick(&my_module, now);
 *         if (tick != EKK_OK && tick != EKK_ERR_DEGRADED) {
 *             break;
 *         }
 *     }
 * }
 * @endcode
 *
 * @author Elektrokombinacija
 * @date 2026
 */

#ifndef EKK_H
#define EKK_H

/* Core types and configuration */
#include "ekk_types.h"

/* Hardware abstraction */
#include "ekk_hal.h"

/* Coordination field engine */
#include "ekk_field.h"

/* Topological neighbor management */
#include "ekk_topology.h"

/* Threshold consensus */
#include "ekk_consensus.h"

/* Heartbeat and liveness */
#include "ekk_heartbeat.h"

/* Module - first class citizen */
#include "ekk_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * VERSION
 * ============================================================================ */

#define EKK_VERSION_MAJOR   2
#define EKK_VERSION_MINOR   0
#define EKK_VERSION_PATCH   0
#define EKK_VERSION_STRING  "2.0.0"

/**
 * @brief Get version as packed integer (major << 16 | minor << 8 | patch)
 */
static inline uint32_t ekk_version(void)
{
    return (EKK_VERSION_MAJOR << 16) | (EKK_VERSION_MINOR << 8) | EKK_VERSION_PATCH;
}

/* ============================================================================
 * SYSTEM INITIALIZATION
 * ============================================================================ */

/**
 * @brief Initialize EK-KOR v2 system
 *
 * Initializes HAL, field region, and internal state.
 * Call once at startup before creating modules.
 *
 * @return EKK_OK on success
 */
ekk_error_t ekk_init(void);

/**
 * @brief Get global field region
 *
 * Returns pointer to shared coordination field region.
 * Used internally by modules.
 */
ekk_field_region_t* ekk_get_field_region(void);

#ifdef __cplusplus
}
#endif

#endif /* EKK_H */
