/**
 * @file ekk_heartbeat.c
 * @brief EK-KOR v2 - Heartbeat and Liveness Detection Implementation
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * NOVELTY: Kernel-Integrated Failure Detection
 * - Automatic neighbor health tracking
 * - State machine: Unknown → Alive → Suspect → Dead
 * - Callbacks on state transitions
 */

#include "ekk/ekk_heartbeat.h"
#include "ekk/ekk_hal.h"
#include <string.h>

/* ============================================================================
 * PRIVATE HELPERS
 * ============================================================================ */

/**
 * @brief Find neighbor index by ID
 * @return Index if found, -1 otherwise
 */
static int find_neighbor_index(const ekk_heartbeat_t *hb, ekk_module_id_t id)
{
    for (uint32_t i = 0; i < hb->neighbor_count; i++) {
        if (hb->neighbors[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Transition neighbor to new health state
 */
static void set_neighbor_health(ekk_heartbeat_t *hb,
                                 ekk_heartbeat_neighbor_t *neighbor,
                                 ekk_health_state_t new_state)
{
    ekk_health_state_t old_state = neighbor->health;
    if (old_state == new_state) {
        return;
    }

    neighbor->health = new_state;

    /* Invoke callback */
    switch (new_state) {
        case EKK_HEALTH_ALIVE:
            if (hb->on_neighbor_alive) {
                hb->on_neighbor_alive(hb->callback_context, neighbor->id);
            }
            break;
        case EKK_HEALTH_SUSPECT:
            if (hb->on_neighbor_suspect) {
                hb->on_neighbor_suspect(hb->callback_context, neighbor->id);
            }
            break;
        case EKK_HEALTH_DEAD:
            if (hb->on_neighbor_dead) {
                hb->on_neighbor_dead(hb->callback_context, neighbor->id);
            }
            break;
        default:
            break;
    }
}

static ekk_error_t send_heartbeat_at(ekk_heartbeat_t *hb, ekk_time_us_t now)
{
    if (hb == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    ekk_heartbeat_msg_t msg = {
        .msg_type = EKK_MSG_HEARTBEAT,
        .sender_id = hb->my_id,
        .sequence = hb->send_sequence++,
        .state = hb->current_state,
        .neighbor_count = (uint8_t)hb->neighbor_count,
        .load_percent = 0,
        .thermal_percent = 0,
        .flags = 0,
    };

    ekk_error_t err = ekk_hal_broadcast(EKK_MSG_HEARTBEAT, &msg, sizeof(msg));
    if (err == EKK_OK) {
        hb->last_send = now;
    }

    return err;
}

/* ============================================================================
 * INITIALIZATION
 * ============================================================================ */

ekk_error_t ekk_heartbeat_init(ekk_heartbeat_t *hb,
                                ekk_module_id_t my_id,
                                const ekk_heartbeat_config_t *config)
{
    if (hb == NULL || my_id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    memset(hb, 0, sizeof(ekk_heartbeat_t));
    hb->my_id = my_id;

    /* Apply configuration */
    if (config != NULL) {
        hb->config = *config;
    } else {
        static const ekk_heartbeat_config_t default_cfg = EKK_HEARTBEAT_CONFIG_DEFAULT;
        hb->config = default_cfg;
    }

    return EKK_OK;
}

/* ============================================================================
 * NEIGHBOR MANAGEMENT
 * ============================================================================ */

ekk_error_t ekk_heartbeat_add_neighbor(ekk_heartbeat_t *hb,
                                        ekk_module_id_t neighbor_id)
{
    if (hb == NULL || neighbor_id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Check if already tracked */
    if (find_neighbor_index(hb, neighbor_id) >= 0) {
        return EKK_ERR_ALREADY_EXISTS;
    }

    /* Check capacity */
    if (hb->neighbor_count >= EKK_MAX_MODULES) {
        return EKK_ERR_NO_MEMORY;
    }

    /* Add neighbor */
    ekk_heartbeat_neighbor_t *neighbor = &hb->neighbors[hb->neighbor_count];
    neighbor->id = neighbor_id;
    neighbor->health = EKK_HEALTH_UNKNOWN;
    neighbor->last_seen = 0;
    neighbor->missed_count = 0;
    neighbor->sequence = 0;
    neighbor->avg_heartbeat_gap_us = 0;

    hb->neighbor_count++;
    return EKK_OK;
}

ekk_error_t ekk_heartbeat_remove_neighbor(ekk_heartbeat_t *hb,
                                           ekk_module_id_t neighbor_id)
{
    if (hb == NULL || neighbor_id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    int idx = find_neighbor_index(hb, neighbor_id);
    if (idx < 0) {
        return EKK_ERR_NOT_FOUND;
    }

    /* Shift remaining neighbors down */
    for (uint32_t i = (uint32_t)idx; i < hb->neighbor_count - 1; i++) {
        hb->neighbors[i] = hb->neighbors[i + 1];
    }
    hb->neighbor_count--;

    return EKK_OK;
}

/* ============================================================================
 * HEARTBEAT RECEIVE
 * ============================================================================ */

ekk_error_t ekk_heartbeat_received(ekk_heartbeat_t *hb,
                                    ekk_module_id_t sender_id,
                                    uint8_t sequence,
                                    ekk_time_us_t now)
{
    if (hb == NULL || sender_id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Find neighbor */
    int idx = find_neighbor_index(hb, sender_id);
    if (idx < 0) {
        /* Not tracking this neighbor - could auto-add or ignore */
        return EKK_OK;
    }

    ekk_heartbeat_neighbor_t *neighbor = &hb->neighbors[idx];

    /* Update latency tracking if enabled */
    if (hb->config.track_latency && neighbor->last_seen > 0) {
        ekk_time_us_t gap_us = now - neighbor->last_seen;
        /* Simple exponential moving average: avg = 0.875*avg + 0.125*new */
        if (neighbor->avg_heartbeat_gap_us <= (UINT64_MAX / 8)) {
            neighbor->avg_heartbeat_gap_us =
                (neighbor->avg_heartbeat_gap_us * 7 + gap_us) / 8;
        }
    }

    /* Update state */
    neighbor->last_seen = now;
    neighbor->sequence = sequence;
    neighbor->missed_count = 0;

    /* Transition to alive (from any state) */
    set_neighbor_health(hb, neighbor, EKK_HEALTH_ALIVE);

    return EKK_OK;
}

/* ============================================================================
 * PERIODIC TICK
 * ============================================================================ */

uint32_t ekk_heartbeat_tick(ekk_heartbeat_t *hb, ekk_time_us_t now)
{
    if (hb == NULL) {
        return 0;
    }

    if (hb->config.period == 0) {
        return 0;
    }

    uint32_t state_changes = 0;

    /* Check each neighbor for timeout */
    for (uint32_t i = 0; i < hb->neighbor_count; i++) {
        ekk_heartbeat_neighbor_t *neighbor = &hb->neighbors[i];

        /* Skip if never seen */
        if (neighbor->health == EKK_HEALTH_UNKNOWN) {
            continue;
        }

        /* Calculate time since last heartbeat */
        ekk_time_us_t elapsed = now - neighbor->last_seen;
        uint32_t missed = (uint32_t)(elapsed / hb->config.period);

        if (missed > neighbor->missed_count) {
            neighbor->missed_count = (uint8_t)EKK_MIN(missed, 255U);
        }

        /* Determine health state based on missed count */
        ekk_health_state_t old_state = neighbor->health;
        ekk_health_state_t new_state = old_state;

        if (neighbor->missed_count == 0) {
            new_state = EKK_HEALTH_ALIVE;
        }
        else if (neighbor->missed_count < hb->config.timeout_count) {
            new_state = EKK_HEALTH_SUSPECT;
        }
        else {
            new_state = EKK_HEALTH_DEAD;
        }

        if (new_state != old_state) {
            set_neighbor_health(hb, neighbor, new_state);
            state_changes++;
        }
    }

    /* Send heartbeat if auto_broadcast and period elapsed */
    if (hb->config.auto_broadcast) {
        if (now - hb->last_send >= hb->config.period) {
            send_heartbeat_at(hb, now);
        }
    }

    return state_changes;
}

/* ============================================================================
 * STATE SETTER
 * ============================================================================ */

void ekk_heartbeat_set_state(ekk_heartbeat_t *hb, ekk_module_state_t state)
{
    if (hb == NULL) {
        return;
    }
    hb->current_state = (uint8_t)state;
}

/* ============================================================================
 * HEARTBEAT SEND
 * ============================================================================ */

ekk_error_t ekk_heartbeat_send(ekk_heartbeat_t *hb)
{
    return send_heartbeat_at(hb, ekk_hal_time_us());
}

/* ============================================================================
 * QUERIES
 * ============================================================================ */

ekk_health_state_t ekk_heartbeat_get_health(const ekk_heartbeat_t *hb,
                                             ekk_module_id_t neighbor_id)
{
    if (hb == NULL || neighbor_id == EKK_INVALID_MODULE_ID) {
        return EKK_HEALTH_UNKNOWN;
    }

    int idx = find_neighbor_index(hb, neighbor_id);
    if (idx < 0) {
        return EKK_HEALTH_UNKNOWN;
    }

    return hb->neighbors[idx].health;
}

ekk_time_us_t ekk_heartbeat_time_since(const ekk_heartbeat_t *hb,
                                        ekk_module_id_t neighbor_id)
{
    if (hb == NULL || neighbor_id == EKK_INVALID_MODULE_ID) {
        return UINT64_MAX;
    }

    int idx = find_neighbor_index(hb, neighbor_id);
    if (idx < 0 || hb->neighbors[idx].last_seen == 0) {
        return UINT64_MAX;
    }

    ekk_time_us_t now = ekk_hal_time_us();
    return now - hb->neighbors[idx].last_seen;
}

/* ============================================================================
 * CALLBACKS
 * ============================================================================ */

void ekk_heartbeat_set_callbacks(ekk_heartbeat_t *hb,
                                  void (*on_alive)(void *context, ekk_module_id_t),
                                  void (*on_suspect)(void *context, ekk_module_id_t),
                                  void (*on_dead)(void *context, ekk_module_id_t),
                                  void *context)
{
    if (hb == NULL) {
        return;
    }

    hb->on_neighbor_alive = on_alive;
    hb->on_neighbor_suspect = on_suspect;
    hb->on_neighbor_dead = on_dead;
    hb->callback_context = context;
}
