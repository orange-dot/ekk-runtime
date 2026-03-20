/**
 * @file ekk_topology.c
 * @brief EK-KOR v2 - Topological k-Neighbor Coordination Implementation
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * NOVELTY: Topological k-Neighbor Coordination
 * - Scale-free coordination using fixed neighbor count (k=7)
 * - Based on Cavagna & Giardina starling research
 * - Distance-based neighbor election with multiple metrics
 */

#include "ekk/ekk_topology.h"
#include "ekk/ekk_hal.h"
#include <string.h>

/* ============================================================================
 * PRIVATE HELPERS
 * ============================================================================ */

/**
 * @brief Find module index in known list
 * @return Index if found, -1 otherwise
 */
static int find_known_index(const ekk_topology_t *topo, ekk_module_id_t id)
{
    for (uint32_t i = 0; i < topo->known_count; i++) {
        if (topo->all_known[i] == id) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Find neighbor index by ID
 * @return Index if found, -1 otherwise
 */
static int find_neighbor_index(const ekk_topology_t *topo, ekk_module_id_t id)
{
    for (uint32_t i = 0; i < topo->neighbor_count; i++) {
        if (topo->neighbors[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Add module to known list
 * @return Index in known list, -1 if full
 */
static int add_to_known(ekk_topology_t *topo, ekk_module_id_t id, ekk_position_t pos)
{
    /* Check if already known */
    int idx = find_known_index(topo, id);
    if (idx >= 0) {
        /* Update position */
        topo->known_positions[idx] = pos;
        return idx;
    }

    /* Check capacity */
    if (topo->known_count >= EKK_MAX_MODULES) {
        return -1;
    }

    /* Add new module */
    idx = (int)topo->known_count;
    topo->all_known[idx] = id;
    topo->known_positions[idx] = pos;
    topo->known_count++;

    return idx;
}

/**
 * @brief Remove module from known list
 */
static void remove_from_known(ekk_topology_t *topo, ekk_module_id_t id)
{
    int idx = find_known_index(topo, id);
    if (idx < 0) {
        return;
    }

    /* Shift remaining modules down */
    for (uint32_t i = (uint32_t)idx; i < topo->known_count - 1; i++) {
        topo->all_known[i] = topo->all_known[i + 1];
        topo->known_positions[i] = topo->known_positions[i + 1];
    }
    topo->known_count--;
}

/**
 * @brief Compute squared Euclidean distance between positions
 */
static int32_t position_distance_sq(ekk_position_t a, ekk_position_t b)
{
    int64_t dx = (int64_t)a.x - (int64_t)b.x;
    int64_t dy = (int64_t)a.y - (int64_t)b.y;
    int64_t dz = (int64_t)a.z - (int64_t)b.z;
    int64_t sq = dx * dx + dy * dy + dz * dz;
    return (sq > (int64_t)INT32_MAX) ? INT32_MAX : (int32_t)sq;
}

/**
 * @brief Integer square root approximation
 */
static int32_t isqrt(int32_t n)
{
    if (n <= 0) return 0;
    if (n == 1) return 1;

    /* Bounded Newton-Raphson: 16 iterations converge for all int32 inputs.
     * Bounded loop required for AutoCorres / seL4 verification compliance. */
    int32_t x = n;
    int32_t y = (x + 1) / 2;
    for (int i = 0; i < 16 && y < x; i++) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

/**
 * @brief Structure for sorting modules by distance
 */
typedef struct {
    ekk_module_id_t id;
    int32_t distance;
    ekk_position_t position;
} distance_entry_t;

/**
 * @brief Compare two distance entries (primary: distance, secondary: module ID)
 * @return true if a should come before b
 */
static ekk_bool_t entry_less_than(const distance_entry_t *a, const distance_entry_t *b)
{
    if (a->distance != b->distance) {
        return a->distance < b->distance;
    }
    /* Tie-breaker: lower module ID first */
    return a->id < b->id;
}

/**
 * @brief Find the current farthest neighbor slot
 * @return Index if a neighbor exists, -1 otherwise
 */
static int find_farthest_neighbor_index(const ekk_topology_t *topo)
{
    if (topo == NULL || topo->neighbor_count == 0) {
        return -1;
    }

    int worst = 0;
    for (uint32_t i = 1; i < topo->neighbor_count; i++) {
        distance_entry_t current_worst = {
            .id = topo->neighbors[worst].id,
            .distance = topo->neighbors[worst].logical_distance,
        };
        distance_entry_t candidate = {
            .id = topo->neighbors[i].id,
            .distance = topo->neighbors[i].logical_distance,
        };

        if (entry_less_than(&current_worst, &candidate)) {
            worst = (int)i;
        }
    }

    return worst;
}

/**
 * @brief Check whether a discovered module outranks the current worst neighbor
 */
static ekk_bool_t candidate_beats_current_worst(const ekk_topology_t *topo,
                                                ekk_module_id_t candidate_id,
                                                int32_t candidate_distance)
{
    int worst_idx = find_farthest_neighbor_index(topo);
    if (worst_idx < 0) {
        return EKK_TRUE;
    }

    distance_entry_t candidate = {
        .id = candidate_id,
        .distance = candidate_distance,
    };
    distance_entry_t worst = {
        .id = topo->neighbors[worst_idx].id,
        .distance = topo->neighbors[worst_idx].logical_distance,
    };

    return entry_less_than(&candidate, &worst);
}

/**
 * @brief Insertion sort for distance entries (efficient for small k)
 * Sorts by distance first, then by module ID for tie-breaking
 */
static void sort_by_distance(distance_entry_t *entries, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++) {
        distance_entry_t key = entries[i];
        int32_t j = (int32_t)i - 1;

        while (j >= 0 && entry_less_than(&key, &entries[j])) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

/**
 * @brief Send discovery broadcast
 */
static ekk_error_t send_discovery(ekk_topology_t *topo)
{
    ekk_discovery_msg_t msg = {
        .msg_type = EKK_MSG_DISCOVERY,
        .sender_id = topo->my_id,
        .position = topo->my_position,
        .neighbor_count = (uint8_t)topo->neighbor_count,
        .state = EKK_MODULE_ACTIVE,
        .sequence = topo->next_discovery_sequence++,
    };

    return ekk_hal_broadcast(EKK_MSG_DISCOVERY, &msg, sizeof(msg));
}

/* ============================================================================
 * INITIALIZATION
 * ============================================================================ */

ekk_error_t ekk_topology_init(ekk_topology_t *topo,
                               ekk_module_id_t my_id,
                               ekk_position_t my_position,
                               const ekk_topology_config_t *config)
{
    if (topo == NULL || my_id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    memset(topo, 0, sizeof(ekk_topology_t));
    topo->my_id = my_id;
    topo->my_position = my_position;

    /* Apply configuration */
    if (config != NULL) {
        topo->config = *config;
    } else {
        ekk_topology_config_t default_config = EKK_TOPOLOGY_CONFIG_DEFAULT;
        topo->config = default_config;
    }

    /* Initialize all neighbor slots as invalid */
    for (uint32_t i = 0; i < EKK_K_NEIGHBORS; i++) {
        topo->neighbors[i].id = EKK_INVALID_MODULE_ID;
        topo->neighbors[i].health = EKK_HEALTH_UNKNOWN;
    }

    return EKK_OK;
}

/* ============================================================================
 * DISCOVERY MESSAGE HANDLING
 * ============================================================================ */

ekk_error_t ekk_topology_on_discovery(ekk_topology_t *topo,
                                       ekk_module_id_t sender_id,
                                       ekk_position_t sender_position)
{
    if (topo == NULL || sender_id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Ignore self */
    if (sender_id == topo->my_id) {
        return EKK_OK;
    }

    /* Add to known modules */
    int idx = add_to_known(topo, sender_id, sender_position);
    if (idx < 0) {
        return EKK_ERR_NO_MEMORY;
    }

    int32_t candidate_distance = ekk_topology_distance(
        topo, topo->my_id, topo->my_position, sender_id, sender_position
    );
    int neighbor_idx = find_neighbor_index(topo, sender_id);

    /* Fill mode: always reelect until we have k neighbors. */
    if (topo->neighbor_count < topo->config.k_neighbors) {
        ekk_topology_reelect(topo);
        return EKK_OK;
    }

    /* Existing neighbor changed its advertised position/distance. Reelect so the
     * current k-nearest set stays in sync with the updated metric. */
    if (neighbor_idx >= 0) {
        if (topo->neighbors[neighbor_idx].logical_distance != candidate_distance) {
            ekk_topology_reelect(topo);
        }
        return EKK_OK;
    }

    /* New or newly improved non-neighbor only triggers reelection if it beats
     * the current farthest selected neighbor. */
    if (candidate_beats_current_worst(topo, sender_id, candidate_distance)) {
        ekk_topology_reelect(topo);
    }

    return EKK_OK;
}

/* ============================================================================
 * NEIGHBOR LOSS HANDLING
 * ============================================================================ */

ekk_error_t ekk_topology_on_neighbor_lost(ekk_topology_t *topo,
                                           ekk_module_id_t lost_id)
{
    if (topo == NULL || lost_id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Find and remove from neighbors */
    int idx = find_neighbor_index(topo, lost_id);
    if (idx < 0) {
        return EKK_ERR_NOT_FOUND;
    }

    /* Mark neighbor slot as invalid */
    topo->neighbors[idx].id = EKK_INVALID_MODULE_ID;
    topo->neighbors[idx].health = EKK_HEALTH_DEAD;

    /* Also remove from known list */
    remove_from_known(topo, lost_id);

    /* Compact neighbor array */
    for (uint32_t i = (uint32_t)idx; i < topo->neighbor_count - 1; i++) {
        topo->neighbors[i] = topo->neighbors[i + 1];
    }
    topo->neighbor_count--;

    /* Clear last slot */
    if (topo->neighbor_count < EKK_K_NEIGHBORS) {
        topo->neighbors[topo->neighbor_count].id = EKK_INVALID_MODULE_ID;
        topo->neighbors[topo->neighbor_count].health = EKK_HEALTH_UNKNOWN;
    }

    /* Trigger reelection to find replacement */
    ekk_topology_reelect(topo);

    return EKK_OK;
}

/* ============================================================================
 * NEIGHBOR REELECTION (k-Nearest Algorithm)
 * ============================================================================ */

uint32_t ekk_topology_reelect(ekk_topology_t *topo)
{
    if (topo == NULL) {
        return 0;
    }

    if (topo->known_count == 0) {
        topo->neighbor_count = 0;
        return 0;
    }

    /* Save old neighbors for callback */
    ekk_neighbor_t old_neighbors[EKK_K_NEIGHBORS];
    uint32_t old_count = topo->neighbor_count;
    memcpy(old_neighbors, topo->neighbors, sizeof(old_neighbors));

    /* Build distance entries for all known modules.
     * Bounded stack allocation keeps reelection instance-local even though the
     * runtime as a whole remains single-threaded and non-reentrant. */
    distance_entry_t entries[EKK_MAX_MODULES];
    uint32_t entry_count = 0;

    for (uint32_t i = 0; i < topo->known_count; i++) {
        ekk_module_id_t id = topo->all_known[i];

        /* Skip self */
        if (id == topo->my_id) {
            continue;
        }

        entries[entry_count].id = id;
        entries[entry_count].position = topo->known_positions[i];
        entries[entry_count].distance = ekk_topology_distance(
            topo,
            topo->my_id, topo->my_position,
            id, topo->known_positions[i]
        );
        entry_count++;
    }

    /* Sort by distance */
    sort_by_distance(entries, entry_count);

    /* Take k nearest as new neighbors */
    uint32_t new_count = EKK_MIN(entry_count, topo->config.k_neighbors);

    for (uint32_t i = 0; i < new_count; i++) {
        /* Check if this was already a neighbor */
        int old_idx = -1;
        for (uint32_t j = 0; j < old_count; j++) {
            if (old_neighbors[j].id == entries[i].id) {
                old_idx = (int)j;
                break;
            }
        }

        if (old_idx >= 0) {
            /* Keep existing neighbor data */
            topo->neighbors[i] = old_neighbors[old_idx];
        } else {
            /* New neighbor */
            topo->neighbors[i].id = entries[i].id;
            topo->neighbors[i].health = EKK_HEALTH_UNKNOWN;
            topo->neighbors[i].last_seen = 0;
            topo->neighbors[i].logical_distance = entries[i].distance;
            topo->neighbors[i].missed_heartbeats = 0;
            memset(&topo->neighbors[i].last_field, 0, sizeof(ekk_field_t));
        }

        /* Update distance (may have changed) */
        topo->neighbors[i].logical_distance = entries[i].distance;
    }

    /* Clear remaining slots */
    for (uint32_t i = new_count; i < EKK_K_NEIGHBORS; i++) {
        topo->neighbors[i].id = EKK_INVALID_MODULE_ID;
        topo->neighbors[i].health = EKK_HEALTH_UNKNOWN;
    }

    topo->neighbor_count = new_count;
    topo->last_reelection = ekk_hal_time_us();

    /* Invoke callback if topology changed */
    if (topo->changed_callback != NULL) {
        ekk_bool_t changed = (old_count != new_count);
        if (!changed) {
            for (uint32_t i = 0; i < new_count && !changed; i++) {
                if (topo->neighbors[i].id != old_neighbors[i].id) {
                    changed = EKK_TRUE;
                }
            }
        }

        if (changed) {
            topo->changed_callback(topo, old_neighbors, old_count,
                                   topo->neighbors, new_count);
        }
    }

    return new_count;
}

/* ============================================================================
 * PERIODIC TICK
 * ============================================================================ */

ekk_bool_t ekk_topology_tick(ekk_topology_t *topo, ekk_time_us_t now)
{
    if (topo == NULL) {
        return EKK_FALSE;
    }

    ekk_bool_t changed = EKK_FALSE;

    /* Send discovery broadcast if period elapsed */
    if (now - topo->last_discovery >= topo->config.discovery_period) {
        send_discovery(topo);
        topo->last_discovery = now;
    }

    /* Check if we need more neighbors (not called from reelect to avoid recursion) */
    if (topo->neighbor_count < topo->config.min_neighbors) {
        /* Trigger discovery more aggressively */
        if (now - topo->last_discovery >= topo->config.discovery_period / 4) {
            send_discovery(topo);
            topo->last_discovery = now;
            changed = EKK_TRUE; /* topology degraded: seeking neighbors */
        }
    }

    return changed;
}

/* ============================================================================
 * NEIGHBOR QUERIES
 * ============================================================================ */

uint32_t ekk_topology_get_neighbors(const ekk_topology_t *topo,
                                     ekk_neighbor_t *neighbors,
                                     uint32_t max_count)
{
    if (topo == NULL || neighbors == NULL) {
        return 0;
    }

    uint32_t count = EKK_MIN(topo->neighbor_count, max_count);
    memcpy(neighbors, topo->neighbors, count * sizeof(ekk_neighbor_t));
    return count;
}

ekk_bool_t ekk_topology_is_neighbor(const ekk_topology_t *topo,
                                     ekk_module_id_t module_id)
{
    if (topo == NULL || module_id == EKK_INVALID_MODULE_ID) {
        return EKK_FALSE;
    }

    return find_neighbor_index(topo, module_id) >= 0;
}

const ekk_neighbor_t* ekk_topology_get_neighbor(const ekk_topology_t *topo,
                                                  ekk_module_id_t module_id)
{
    if (topo == NULL || module_id == EKK_INVALID_MODULE_ID) {
        return NULL;
    }

    int idx = find_neighbor_index(topo, module_id);
    if (idx < 0) {
        return NULL;
    }

    return &topo->neighbors[idx];
}

/* ============================================================================
 * DISTANCE CALCULATION
 * ============================================================================ */

int32_t ekk_topology_distance(const ekk_topology_t *topo,
                               ekk_module_id_t id_a, ekk_position_t pos_a,
                               ekk_module_id_t id_b, ekk_position_t pos_b)
{
    if (topo == NULL) {
        return INT32_MAX;
    }

    switch (topo->config.metric) {
        case EKK_DISTANCE_LOGICAL:
            /* Logical distance: |id_a - id_b| */
            return (int32_t)(id_a > id_b ? id_a - id_b : id_b - id_a);

        case EKK_DISTANCE_PHYSICAL:
            /* Physical distance: Euclidean */
            return isqrt(position_distance_sq(pos_a, pos_b));

        case EKK_DISTANCE_LATENCY:
            /* Latency-based: would require HAL support for RTT measurement */
            /* Fall back to logical for now */
            return (int32_t)(id_a > id_b ? id_a - id_b : id_b - id_a);

        case EKK_DISTANCE_CUSTOM:
            /* Application-defined */
            return ekk_topology_distance_custom(id_a, id_b);

        default:
            return INT32_MAX;
    }
}

/**
 * @brief Default custom distance function (weak, can be overridden)
 */
EKK_WEAK int32_t ekk_topology_distance_custom(ekk_module_id_t id_a,
                                               ekk_module_id_t id_b)
{
    /* Default implementation: same as logical */
    return (int32_t)(id_a > id_b ? id_a - id_b : id_b - id_a);
}

/* ============================================================================
 * CALLBACKS
 * ============================================================================ */

void ekk_topology_set_callback(ekk_topology_t *topo,
                                ekk_topology_changed_cb callback)
{
    if (topo == NULL) {
        return;
    }

    topo->changed_callback = callback;
}
