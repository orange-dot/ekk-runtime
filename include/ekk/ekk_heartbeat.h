/**
 * @file ekk_heartbeat.h
 * @brief EK-KOR v2 - Heartbeat and Liveness Detection
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * @license MIT
 * SPDX-License-Identifier: MIT
 *
 * Heartbeat and liveness tracking for the extracted coordination runtime.
 *
 * In this extract, heartbeat is a userspace/runtime component:
 * - it tracks `UNKNOWN -> ALIVE -> SUSPECT -> DEAD`
 * - it exposes instance-local callbacks for health changes
 * - it records heartbeat inter-arrival timing when enabled
 */

#ifndef EKK_HEARTBEAT_H
#define EKK_HEARTBEAT_H

#include "ekk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * HEARTBEAT CONFIGURATION
 * ============================================================================ */

/**
 * @brief Heartbeat configuration
 */
typedef struct {
    ekk_time_us_t period;           /**< Heartbeat send period */
    uint32_t timeout_count;         /**< Missed beats before failure */
    ekk_bool_t auto_broadcast;      /**< Automatically broadcast heartbeats */
    ekk_bool_t track_latency;       /**< Track heartbeat timing gaps to neighbors */
} ekk_heartbeat_config_t;

#define EKK_HEARTBEAT_CONFIG_DEFAULT { \
    .period = EKK_HEARTBEAT_PERIOD_US, \
    .timeout_count = EKK_HEARTBEAT_TIMEOUT_COUNT, \
    .auto_broadcast = EKK_TRUE, \
    .track_latency = EKK_FALSE, \
}

/* ============================================================================
 * HEARTBEAT STATE
 * ============================================================================ */

/**
 * @brief Per-neighbor heartbeat tracking
 */
typedef struct {
    ekk_module_id_t id;             /**< Neighbor ID */
    ekk_health_state_t health;      /**< Current health state */
    ekk_time_us_t last_seen;        /**< Last heartbeat received */
    uint8_t missed_count;           /**< Consecutive missed heartbeats */
    uint8_t sequence;               /**< Last seen sequence number */
    ekk_time_us_t avg_heartbeat_gap_us; /**< Average heartbeat inter-arrival gap */
} ekk_heartbeat_neighbor_t;

/**
 * @brief Heartbeat engine state
 */
typedef struct {
    ekk_module_id_t my_id;

    ekk_heartbeat_neighbor_t neighbors[EKK_MAX_MODULES];
    uint32_t neighbor_count;

    ekk_time_us_t last_send;        /**< Last heartbeat sent */
    uint8_t send_sequence;          /**< Outgoing sequence number */
    uint8_t current_state;          /**< Module state for heartbeat messages */

    ekk_heartbeat_config_t config;

#ifndef EKK_VERIFICATION
    /* Callbacks — function pointers not supported by l4v C parser */
    void (*on_neighbor_alive)(void *context, ekk_module_id_t id);
    void (*on_neighbor_suspect)(void *context, ekk_module_id_t id);
    void (*on_neighbor_dead)(void *context, ekk_module_id_t id);
    void *callback_context;
#endif
} ekk_heartbeat_t;

/* ============================================================================
 * HEARTBEAT API
 * ============================================================================ */

/**
 * @brief Initialize heartbeat engine
 *
 * @param hb Heartbeat state (caller-allocated)
 * @param my_id This module's ID
 * @param config Configuration (or NULL for defaults)
 * @return EKK_OK on success
 */
ekk_error_t ekk_heartbeat_init(ekk_heartbeat_t *hb,
                                ekk_module_id_t my_id,
                                const ekk_heartbeat_config_t *config);

/**
 * @brief Add neighbor to track
 *
 * @param hb Heartbeat state
 * @param neighbor_id Neighbor to track
 * @return EKK_OK on success
 */
ekk_error_t ekk_heartbeat_add_neighbor(ekk_heartbeat_t *hb,
                                        ekk_module_id_t neighbor_id);

/**
 * @brief Remove neighbor from tracking
 *
 * @param hb Heartbeat state
 * @param neighbor_id Neighbor to remove
 * @return EKK_OK on success
 */
ekk_error_t ekk_heartbeat_remove_neighbor(ekk_heartbeat_t *hb,
                                           ekk_module_id_t neighbor_id);

/**
 * @brief Process received heartbeat
 *
 * Called when heartbeat message received from neighbor.
 * Updates neighbor's health state.
 *
 * @param hb Heartbeat state
 * @param sender_id Sender's module ID
 * @param sequence Heartbeat sequence number
 * @param now Current timestamp
 * @return EKK_OK on success
 */
ekk_error_t ekk_heartbeat_received(ekk_heartbeat_t *hb,
                                    ekk_module_id_t sender_id,
                                    uint8_t sequence,
                                    ekk_time_us_t now);

/**
 * @brief Periodic tick
 *
 * Checks for timeouts and sends heartbeats if auto_broadcast enabled.
 *
 * @param hb Heartbeat state
 * @param now Current timestamp
 * @return Number of neighbors whose state changed
 */
uint32_t ekk_heartbeat_tick(ekk_heartbeat_t *hb, ekk_time_us_t now);

/**
 * @brief Send heartbeat now
 *
 * Manually trigger heartbeat broadcast.
 *
 * @param hb Heartbeat state
 * @return EKK_OK on success
 */
ekk_error_t ekk_heartbeat_send(ekk_heartbeat_t *hb);

/**
 * @brief Get neighbor health state
 *
 * @param hb Heartbeat state
 * @param neighbor_id Neighbor to query
 * @return Health state, or UNKNOWN if not tracked
 */
ekk_health_state_t ekk_heartbeat_get_health(const ekk_heartbeat_t *hb,
                                             ekk_module_id_t neighbor_id);

/**
 * @brief Get time since last heartbeat
 *
 * @param hb Heartbeat state
 * @param neighbor_id Neighbor to query
 * @return Microseconds since last seen, or UINT64_MAX if never seen
 */
ekk_time_us_t ekk_heartbeat_time_since(const ekk_heartbeat_t *hb,
                                        ekk_module_id_t neighbor_id);

/* ============================================================================
 * HEARTBEAT MESSAGE FORMAT
 * ============================================================================ */

/**
 * @brief Heartbeat message (broadcast periodically)
 */
EKK_PACK_BEGIN
typedef struct {
    uint8_t msg_type;               /**< EKK_MSG_HEARTBEAT */
    ekk_module_id_t sender_id;      /**< Sender's module ID */
    uint8_t sequence;               /**< Monotonic sequence */
    uint8_t state;                  /**< Sender's state (ekk_module_state_t) */
    uint8_t neighbor_count;         /**< Sender's neighbor count */
    uint8_t load_percent;           /**< Load 0-100% */
    uint8_t thermal_percent;        /**< Thermal 0-100% */
    uint8_t flags;                  /**< Reserved */
} EKK_PACKED ekk_heartbeat_msg_t;
EKK_PACK_END

EKK_STATIC_ASSERT(sizeof(ekk_heartbeat_msg_t) == 8, "Heartbeat message wrong size");

/**
 * @brief Set the module state reported in heartbeat messages
 *
 * @param hb Heartbeat state
 * @param state Current module state
 */
void ekk_heartbeat_set_state(ekk_heartbeat_t *hb, ekk_module_state_t state);

/* ============================================================================
 * CALLBACKS
 * ============================================================================ */

#ifndef EKK_VERIFICATION
/**
 * @brief Set callbacks for health state changes
 */
void ekk_heartbeat_set_callbacks(ekk_heartbeat_t *hb,
                                  void (*on_alive)(void *context, ekk_module_id_t),
                                  void (*on_suspect)(void *context, ekk_module_id_t),
                                  void (*on_dead)(void *context, ekk_module_id_t),
                                  void *context);
#endif

#ifdef __cplusplus
}
#endif

#endif /* EKK_HEARTBEAT_H */
