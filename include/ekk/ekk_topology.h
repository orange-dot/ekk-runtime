/**
 * @file ekk_topology.h
 * @brief EK-KOR v2 - Topological k-Neighbor Coordination
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * @license MIT
 * SPDX-License-Identifier: MIT
 *
 * Event-driven topology maintenance for a bounded neighbor set.
 *
 * In this extract, topology tracks the current `k`-nearest set under the
 * configured metric, but reelection is event-driven rather than continuous.
 * Discovery fills empty slots first, then only triggers reelection when a new
 * candidate beats the current farthest selected neighbor.
 */

#ifndef EKK_TOPOLOGY_H
#define EKK_TOPOLOGY_H

#include "ekk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TOPOLOGY CONFIGURATION
 * ============================================================================ */

/**
 * @brief Distance metric for neighbor selection
 */
typedef enum {
    EKK_DISTANCE_LOGICAL,       /**< Based on module ID proximity */
    EKK_DISTANCE_PHYSICAL,      /**< Based on position coordinates */
    EKK_DISTANCE_LATENCY,       /**< Placeholder for latency-derived distance */
    EKK_DISTANCE_CUSTOM,        /**< Application-defined metric */
} ekk_distance_metric_t;

/**
 * @brief Topology configuration
 */
typedef struct {
    uint32_t k_neighbors;                   /**< Target neighbor count (default 7) */
    ekk_distance_metric_t metric;           /**< How to measure distance */
    ekk_time_us_t discovery_period;         /**< How often to broadcast discovery */
    ekk_time_us_t reelection_delay;         /**< Delay before reelecting neighbors */
    uint32_t min_neighbors;                 /**< Minimum before DEGRADED state */
} ekk_topology_config_t;

/**
 * @brief Default configuration
 */
#define EKK_TOPOLOGY_CONFIG_DEFAULT { \
    .k_neighbors = EKK_K_NEIGHBORS, \
    .metric = EKK_DISTANCE_LOGICAL, \
    .discovery_period = 1000000, /* 1 second */ \
    .reelection_delay = 100000,  /* 100ms */ \
    .min_neighbors = 3, \
}

/* ============================================================================
 * MODULE POSITION (for physical distance metric)
 * ============================================================================ */

/**
 * @brief 3D position for physical distance calculation
 *
 * For EK3 modules in a rack, this could represent:
 * - x: slot position within rack
 * - y: rack row
 * - z: rack column
 */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} ekk_position_t;

typedef struct ekk_topology ekk_topology_t;

/* ============================================================================
 * CALLBACKS
 * ============================================================================ */

/**
 * @brief Callback when topology changes
 */
typedef void (*ekk_topology_changed_cb)(ekk_topology_t *topo,
                                         const ekk_neighbor_t *old_neighbors,
                                         uint32_t old_count,
                                         const ekk_neighbor_t *new_neighbors,
                                         uint32_t new_count);

/* ============================================================================
 * TOPOLOGY STATE
 * ============================================================================ */

/**
 * @brief Topology state for a module
 */
struct ekk_topology {
    ekk_module_id_t my_id;                          /**< This module's ID */
    ekk_position_t my_position;                     /**< This module's position */

    ekk_neighbor_t neighbors[EKK_K_NEIGHBORS];      /**< Current k-neighbors */
    uint32_t neighbor_count;                        /**< Actual neighbor count */

    ekk_module_id_t all_known[EKK_MAX_MODULES];     /**< All discovered modules */
    ekk_position_t known_positions[EKK_MAX_MODULES];
    uint32_t known_count;                           /**< Count of known modules */

    ekk_time_us_t last_discovery;                   /**< Last discovery broadcast */
    ekk_time_us_t last_reelection;                  /**< Last neighbor reelection */
    uint16_t next_discovery_sequence;

    ekk_topology_config_t config;                   /**< Configuration */
    ekk_topology_changed_cb changed_callback;
};

/* ============================================================================
 * TOPOLOGY API
 * ============================================================================ */

/**
 * @brief Initialize topology for a module
 *
 * @param topo Topology state (caller-allocated)
 * @param my_id This module's ID
 * @param my_position This module's physical position (or zeros)
 * @param config Configuration (or NULL for defaults)
 * @return EKK_OK on success
 */
ekk_error_t ekk_topology_init(ekk_topology_t *topo,
                               ekk_module_id_t my_id,
                               ekk_position_t my_position,
                               const ekk_topology_config_t *config);

/**
 * @brief Process discovery message from another module
 *
 * Called when receiving a discovery broadcast from another module.
 * Updates known modules list and triggers reelection if needed.
 *
 * @param topo Topology state
 * @param sender_id Sender's module ID
 * @param sender_position Sender's position
 * @return EKK_OK on success
 */
ekk_error_t ekk_topology_on_discovery(ekk_topology_t *topo,
                                       ekk_module_id_t sender_id,
                                       ekk_position_t sender_position);

/**
 * @brief Mark a neighbor as lost
 *
 * Called by heartbeat layer when a neighbor times out.
 * Triggers neighbor reelection.
 *
 * @param topo Topology state
 * @param lost_id Lost neighbor's ID
 * @return EKK_OK on success, EKK_ERR_NOT_FOUND if not a neighbor
 */
ekk_error_t ekk_topology_on_neighbor_lost(ekk_topology_t *topo,
                                           ekk_module_id_t lost_id);

/**
 * @brief Force neighbor reelection
 *
 * Recomputes the current `k`-nearest neighbors from the known set.
 * Called after discovery or neighbor loss when reelection is warranted.
 *
 * @param topo Topology state
 * @return Number of neighbors after reelection
 */
uint32_t ekk_topology_reelect(ekk_topology_t *topo);

/**
 * @brief Periodic tick (call from main loop)
 *
 * Handles discovery broadcasts and neighbor maintenance.
 *
 * @param topo Topology state
 * @param now Current timestamp
 * @return true if periodic maintenance triggered a rediscovery attempt
 *         or other topology-layer work that may require caller attention.
 *         This is not a guaranteed neighbor-set change signal.
 */
ekk_bool_t ekk_topology_tick(ekk_topology_t *topo, ekk_time_us_t now);

/**
 * @brief Get current neighbors
 *
 * @param topo Topology state
 * @param[out] neighbors Array to fill (at least k_neighbors elements)
 * @param max_count Maximum neighbors to return
 * @return Actual neighbor count
 */
uint32_t ekk_topology_get_neighbors(const ekk_topology_t *topo,
                                     ekk_neighbor_t *neighbors,
                                     uint32_t max_count);

/**
 * @brief Check if a module is a neighbor
 */
ekk_bool_t ekk_topology_is_neighbor(const ekk_topology_t *topo,
                                     ekk_module_id_t module_id);

/**
 * @brief Get neighbor by ID
 *
 * @param topo Topology state
 * @param module_id Neighbor's ID
 * @return Pointer to neighbor info, or NULL if not a neighbor
 */
const ekk_neighbor_t* ekk_topology_get_neighbor(const ekk_topology_t *topo,
                                                  ekk_module_id_t module_id);

/* ============================================================================
 * DISTANCE CALCULATION
 * ============================================================================ */

/**
 * @brief Compute logical distance between two modules
 *
 * For DISTANCE_LOGICAL: |id_a - id_b|
 * For DISTANCE_PHYSICAL: Euclidean distance
 * For DISTANCE_LATENCY: placeholder latency-derived score. The current
 * implementation falls back to logical distance until a transport-specific
 * latency source exists.
 *
 * @param topo Topology state (for metric selection)
 * @param id_a First module
 * @param pos_a First module's position
 * @param id_b Second module
 * @param pos_b Second module's position
 * @return Distance (lower = closer)
 */
int32_t ekk_topology_distance(const ekk_topology_t *topo,
                               ekk_module_id_t id_a, ekk_position_t pos_a,
                               ekk_module_id_t id_b, ekk_position_t pos_b);

/**
 * @brief Custom distance function (override for DISTANCE_CUSTOM)
 *
 * @note Implement this function to provide application-specific distance
 */
EKK_WEAK int32_t ekk_topology_distance_custom(ekk_module_id_t id_a,
                                               ekk_module_id_t id_b);

/* ============================================================================
 * DISCOVERY MESSAGE FORMAT
 * ============================================================================ */

/**
 * @brief Discovery message (broadcast periodically)
 */
EKK_PACK_BEGIN
typedef struct {
    uint8_t msg_type;               /**< EKK_MSG_DISCOVERY */
    ekk_module_id_t sender_id;      /**< Sender's module ID */
    ekk_position_t position;        /**< Sender's position */
    uint8_t neighbor_count;         /**< Sender's current neighbor count */
    uint8_t state;                  /**< Sender's state (ekk_module_state_t) */
    uint16_t sequence;              /**< Monotonic sequence */
} EKK_PACKED ekk_discovery_msg_t;
EKK_PACK_END

EKK_STATIC_ASSERT(sizeof(ekk_discovery_msg_t) <= 16, "Discovery message too large");

/**
 * @brief Register topology change callback
 */
void ekk_topology_set_callback(ekk_topology_t *topo,
                                ekk_topology_changed_cb callback);

#ifdef __cplusplus
}
#endif

#endif /* EKK_TOPOLOGY_H */
