/**
 * @file ekk_module.c
 * @brief EK-KOR v2 - Module as First-Class Citizen Implementation
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * NOVELTY: Module-Centric RTOS
 * - No global scheduler - each module self-organizes
 * - Gradient-based task selection
 * - Integrated topology, heartbeat, and consensus
 */

#include "ekk/ekk_module.h"
#include "ekk/ekk_heartbeat.h"
#include "ekk/ekk_hal.h"
#include <stddef.h>
#include <string.h>

/* ============================================================================
 * FORWARD DECLARATIONS
 * ============================================================================ */

static void on_neighbor_alive_cb(void *context, ekk_module_id_t id);
static void on_neighbor_suspect_cb(void *context, ekk_module_id_t id);
static void on_neighbor_dead_cb(void *context, ekk_module_id_t id);
static void on_topology_changed_cb(ekk_topology_t *topo,
                                    const ekk_neighbor_t *old_neighbors,
                                    uint32_t old_count,
                                    const ekk_neighbor_t *new_neighbors,
                                    uint32_t new_count);
static ekk_vote_value_t on_consensus_decide_cb(ekk_consensus_t *cons,
                                                void *context,
                                                const ekk_ballot_t *ballot);
static void on_consensus_complete_cb(ekk_consensus_t *cons,
                                      void *context,
                                      const ekk_ballot_t *ballot,
                                      ekk_vote_result_t result);
static void sync_consensus_electorate(ekk_module_t *mod);

/* ============================================================================
 * PRIVATE HELPERS
 * ============================================================================ */

/**
 * @brief Update module state based on topology
 */
static void update_module_state(ekk_module_t *mod)
{
    ekk_module_state_t old_state = mod->state;
    ekk_module_state_t new_state = old_state;

    switch (old_state) {
        case EKK_MODULE_INIT:
            /* Stay in INIT until started */
            break;

        case EKK_MODULE_DISCOVERING:
            /* Transition to ACTIVE once we have minimum neighbors */
            if (mod->topology.neighbor_count >= mod->topology.config.min_neighbors) {
                new_state = EKK_MODULE_ACTIVE;
            }
            break;

        case EKK_MODULE_ACTIVE:
            if (mod->topology.neighbor_count == 0) {
                new_state = EKK_MODULE_ISOLATED;
            } else if (mod->topology.neighbor_count < mod->topology.config.min_neighbors) {
                new_state = EKK_MODULE_DEGRADED;
            }
            break;

        case EKK_MODULE_DEGRADED:
            if (mod->topology.neighbor_count == 0) {
                new_state = EKK_MODULE_ISOLATED;
            } else if (mod->topology.neighbor_count >= mod->topology.config.min_neighbors) {
                new_state = EKK_MODULE_ACTIVE;
            }
            break;

        case EKK_MODULE_ISOLATED:
            if (mod->topology.neighbor_count >= mod->topology.config.min_neighbors) {
                new_state = EKK_MODULE_ACTIVE;
            } else if (mod->topology.neighbor_count > 0) {
                new_state = EKK_MODULE_DEGRADED;
            }
            break;

        case EKK_MODULE_REFORMING:
            /* Reformation logic - return to appropriate state */
            if (mod->topology.neighbor_count >= mod->topology.config.min_neighbors) {
                new_state = EKK_MODULE_ACTIVE;
            } else if (mod->topology.neighbor_count > 0) {
                new_state = EKK_MODULE_DEGRADED;
            } else {
                new_state = EKK_MODULE_ISOLATED;
            }
            break;

        case EKK_MODULE_SHUTDOWN:
            /* Stay in shutdown */
            break;
    }

    if (new_state != old_state) {
        mod->state = new_state;
        if (mod->on_state_change != NULL) {
            mod->on_state_change(mod, old_state);
        }
    }
}

/**
 * @brief Process incoming messages
 */
static ekk_error_t merge_tick_status(ekk_error_t current, ekk_error_t update)
{
    if (update == EKK_OK) {
        return current;
    }

    if (update == EKK_ERR_DEGRADED) {
        return (current == EKK_OK) ? EKK_ERR_DEGRADED : current;
    }

    return update;
}

static ekk_error_t process_rx_messages(ekk_module_t *mod, ekk_time_us_t now)
{
    ekk_module_id_t sender_id;
    ekk_msg_type_t msg_type;
    uint8_t buffer[64];
    uint32_t len;
    ekk_error_t status = EKK_OK;

    /* Process all pending messages */
    for (int i = 0; i < 16; i++) {  /* Max 16 messages per tick */
        len = sizeof(buffer);
        ekk_error_t err = ekk_hal_recv(&sender_id, &msg_type, buffer, &len);

        if (err == EKK_ERR_NOT_FOUND) {
            return status;  /* No more messages */
        }

        if (err != EKK_OK) {
            return err;
        }

        switch (msg_type) {
            case EKK_MSG_HEARTBEAT: {
                if (len < sizeof(ekk_heartbeat_msg_t)) break;
                const ekk_heartbeat_msg_t *hb_msg = (const ekk_heartbeat_msg_t *)buffer;
                ekk_heartbeat_received(&mod->heartbeat, hb_msg->sender_id,
                                       hb_msg->sequence, now);
                break;
            }

            case EKK_MSG_DISCOVERY: {
                if (len < sizeof(ekk_discovery_msg_t)) break;
                const ekk_discovery_msg_t *disc_msg = (const ekk_discovery_msg_t *)buffer;
                err = ekk_topology_on_discovery(&mod->topology, disc_msg->sender_id,
                                                disc_msg->position);
                if (err == EKK_ERR_NO_MEMORY) {
                    status = merge_tick_status(status, EKK_ERR_DEGRADED);
                }
                /* Also add to heartbeat tracking */
                err = ekk_heartbeat_add_neighbor(&mod->heartbeat, disc_msg->sender_id);
                if (err == EKK_ERR_NO_MEMORY) {
                    status = merge_tick_status(status, EKK_ERR_DEGRADED);
                }
                break;
            }

            case EKK_MSG_PROPOSAL: {
                if (len < sizeof(ekk_proposal_msg_t)) break;
                const ekk_proposal_msg_t *prop_msg = (const ekk_proposal_msg_t *)buffer;
                uint32_t active_before = mod->consensus.active_ballot_count;
                if (prop_msg->proposer_id != sender_id) break;
                err = ekk_consensus_on_proposal(&mod->consensus, prop_msg->proposer_id,
                                                prop_msg->ballot_id, prop_msg->type,
                                                prop_msg->data, prop_msg->threshold);
                if (err == EKK_OK &&
                    mod->consensus.active_ballot_count > active_before) {
                    mod->ballots_started++;
                }
                break;
            }

            case EKK_MSG_VOTE: {
                if (len < sizeof(ekk_vote_msg_t)) break;
                const ekk_vote_msg_t *vote_msg = (const ekk_vote_msg_t *)buffer;
                if (vote_msg->voter_id != sender_id) break;
                ekk_consensus_on_vote(&mod->consensus, vote_msg->voter_id,
                                      vote_msg->proposer_id,
                                      vote_msg->ballot_id, vote_msg->vote);
                break;
            }

            case EKK_MSG_INHIBIT: {
                if (len < sizeof(ekk_inhibit_msg_t)) break;
                const ekk_inhibit_msg_t *inhibit_msg = (const ekk_inhibit_msg_t *)buffer;
                ekk_consensus_on_inhibit(&mod->consensus, sender_id,
                                         inhibit_msg->proposer_id,
                                         inhibit_msg->ballot_id);
                break;
            }

            case EKK_MSG_FIELD: {
                /* Field messages - update neighbor's last field */
                /* This is handled via shared memory in field.c */
                break;
            }

            default:
                /* Unknown message type - ignore */
                break;
        }
    }

    return status;
}

/**
 * @brief Run selected task
 */
static void run_task(ekk_module_t *mod, ekk_task_id_t task_id, ekk_time_us_t now)
{
    if (task_id >= mod->task_count) {
        return;
    }

    ekk_internal_task_t *task = &mod->tasks[task_id];
    if (task->state != EKK_TASK_READY) {
        return;
    }

    /* Run the task */
    task->state = EKK_TASK_RUNNING;
    mod->active_task = task_id;

    ekk_time_us_t start = ekk_hal_time_us();

    if (task->function != NULL) {
        task->function(task->arg);
    }

    ekk_time_us_t elapsed = ekk_hal_time_us() - start;
    task->total_runtime += elapsed;
    task->run_count++;

    task->state = EKK_TASK_IDLE;
    mod->active_task = 0xFF;

    /* Schedule next run if periodic */
    if (task->period > 0) {
        task->next_run = now + task->period;
        task->state = EKK_TASK_READY;
    }
}

/**
 * @brief Refresh proposer electorate from the current topology view
 */
static void sync_consensus_electorate(ekk_module_t *mod)
{
    ekk_module_id_t voters[EKK_K_NEIGHBORS];
    uint32_t voter_count = EKK_MIN(mod->topology.neighbor_count, EKK_K_NEIGHBORS);

    for (uint32_t i = 0; i < voter_count; i++) {
        voters[i] = mod->topology.neighbors[i].id;
    }

    ekk_consensus_set_electorate(&mod->consensus, voters, voter_count);
}

/* ============================================================================
 * INITIALIZATION
 * ============================================================================ */

ekk_error_t ekk_module_init(ekk_module_t *mod,
                             ekk_module_id_t id,
                             const char *name,
                             ekk_position_t position)
{
    if (mod == NULL || id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    memset(mod, 0, sizeof(ekk_module_t));
    mod->id = id;
    mod->name = name;
    mod->state = EKK_MODULE_INIT;
    mod->active_task = 0xFF;  /* No active task */
    mod->tick_period = 1000;  /* 1ms default tick */

    /* Initialize subsystems */
    ekk_error_t err;

    /* Initialize topology */
    err = ekk_topology_init(&mod->topology, id, position, NULL);
    if (err != EKK_OK) {
        return err;
    }
    ekk_topology_set_callback(&mod->topology, on_topology_changed_cb);

    /* Initialize consensus */
    err = ekk_consensus_init(&mod->consensus, id, NULL);
    if (err != EKK_OK) {
        return err;
    }

    /* Initialize heartbeat */
    err = ekk_heartbeat_init(&mod->heartbeat, id, NULL);
    if (err != EKK_OK) {
        return err;
    }

    /* Set up heartbeat callbacks */
    ekk_heartbeat_set_callbacks(&mod->heartbeat,
                                 on_neighbor_alive_cb,
                                 on_neighbor_suspect_cb,
                                 on_neighbor_dead_cb,
                                 mod);

    /* Set up consensus callbacks */
    ekk_consensus_set_decide_callback(&mod->consensus, on_consensus_decide_cb, mod);
    ekk_consensus_set_complete_callback(&mod->consensus, on_consensus_complete_cb, mod);

    /* Initialize field */
    memset(&mod->my_field, 0, sizeof(ekk_field_t));
    mod->my_field.source = id;
    sync_consensus_electorate(mod);

    return EKK_OK;
}

/* ============================================================================
 * LIFECYCLE
 * ============================================================================ */

ekk_error_t ekk_module_start(ekk_module_t *mod)
{
    if (mod == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    if (mod->state != EKK_MODULE_INIT) {
        return EKK_ERR_BUSY;
    }

    mod->state = EKK_MODULE_DISCOVERING;
    mod->last_tick = ekk_hal_time_us();

    return EKK_OK;
}

ekk_error_t ekk_module_stop(ekk_module_t *mod)
{
    if (mod == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    mod->state = EKK_MODULE_SHUTDOWN;

    /* Notify neighbors via broadcast */
    /* Could broadcast shutdown message here */

    return EKK_OK;
}

/* ============================================================================
 * MAIN TICK
 * ============================================================================ */

ekk_error_t ekk_module_tick(ekk_module_t *mod, ekk_time_us_t now)
{
    ekk_error_t tick_status = EKK_OK;

    if (mod == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Skip if not running */
    if (mod->state == EKK_MODULE_INIT || mod->state == EKK_MODULE_SHUTDOWN) {
        return EKK_OK;
    }

    mod->ticks_total++;

    /* Phase 1: Process incoming messages */
    ekk_error_t err = process_rx_messages(mod, now);
    if (err != EKK_OK && err != EKK_ERR_DEGRADED) {
        mod->ticks_hard_error++;
        return err;
    }
    tick_status = merge_tick_status(tick_status, err);

    /* Phase 2: Update heartbeats, detect failures */
    (void)ekk_heartbeat_tick(&mod->heartbeat, now);

    /* Phase 3: Update topology */
    ekk_time_us_t previous_last_discovery = mod->topology.last_discovery;
    ekk_bool_t topo_changed = ekk_topology_tick(&mod->topology, now);
    if (mod->topology.last_discovery != previous_last_discovery) {
        mod->discovery_broadcasts++;
    }
    EKK_UNUSED(topo_changed);
    sync_consensus_electorate(mod);

    /* Phase 4: Sample neighbor fields and compute gradients */
    err = ekk_field_sample_neighbors_at(
        mod->topology.neighbors,
        mod->topology.neighbor_count,
        &mod->neighbor_aggregate,
        now
    );

    if (err == EKK_OK) {
        /* Compute gradients for each component */
        ekk_field_gradient_all(&mod->my_field, &mod->neighbor_aggregate,
                               mod->gradients);
    } else if (err == EKK_ERR_DEGRADED) {
        mod->field_sample_fail++;
        tick_status = merge_tick_status(tick_status, err);
    } else {
        mod->ticks_hard_error++;
        return err;
    }

    /* Phase 5: Update consensus (check timeouts) */
    (void)ekk_consensus_tick(&mod->consensus, now);

    /* Phase 5b: Refresh deadline/slack state against the same tick time */
    err = ekk_module_compute_slack(mod, now);
    if (err != EKK_OK) {
        mod->ticks_hard_error++;
        return err;
    }

    /* Phase 6: Select and run task based on gradients */
    ekk_task_id_t task_to_run = ekk_module_select_task(mod, now);
    if (task_to_run < mod->task_count) {
        run_task(mod, task_to_run, now);
    }

    /* Phase 7: Publish updated field */
    mod->my_field.timestamp = now;
    err = ekk_field_publish_at(mod->id, &mod->my_field, now);
    if (err == EKK_OK) {
        mod->field_publish_ok++;
    } else {
        mod->field_publish_fail++;
        tick_status = merge_tick_status(tick_status, EKK_ERR_DEGRADED);
    }

    /* Phase 8: Update module state from topology */
    update_module_state(mod);

    /* Propagate current state into heartbeat messages */
    ekk_heartbeat_set_state(&mod->heartbeat, mod->state);

    if (tick_status == EKK_ERR_DEGRADED) {
        mod->ticks_soft_degraded++;
    }

    mod->last_tick = now;
    return tick_status;
}

/* ============================================================================
 * TASK MANAGEMENT
 * ============================================================================ */

ekk_error_t ekk_module_add_task(ekk_module_t *mod,
                                 const char *name,
                                 ekk_task_fn function,
                                 void *arg,
                                 uint8_t priority,
                                 ekk_time_us_t period,
                                 ekk_task_id_t *task_id)
{
    if (mod == NULL || function == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    if (mod->task_count >= EKK_MAX_TASKS_PER_MODULE) {
        return EKK_ERR_NO_MEMORY;
    }

    ekk_internal_task_t *task = &mod->tasks[mod->task_count];
    task->id = (ekk_task_id_t)mod->task_count;
    task->name = name;
    task->function = function;
    task->arg = arg;
    task->state = EKK_TASK_IDLE;
    task->priority = priority;
    task->period = period;
    task->next_run = 0;
    task->run_count = 0;
    task->total_runtime = 0;

    if (task_id != NULL) {
        *task_id = task->id;
    }

    mod->task_count++;
    return EKK_OK;
}

ekk_error_t ekk_module_task_ready(ekk_module_t *mod, ekk_task_id_t task_id)
{
    if (mod == NULL || task_id >= mod->task_count) {
        return EKK_ERR_INVALID_ARG;
    }

    mod->tasks[task_id].state = EKK_TASK_READY;
    return EKK_OK;
}

ekk_error_t ekk_module_task_block(ekk_module_t *mod, ekk_task_id_t task_id)
{
    if (mod == NULL || task_id >= mod->task_count) {
        return EKK_ERR_INVALID_ARG;
    }

    mod->tasks[task_id].state = EKK_TASK_BLOCKED;
    return EKK_OK;
}

/* ============================================================================
 * FIELD OPERATIONS
 * ============================================================================ */

ekk_error_t ekk_module_update_field(ekk_module_t *mod,
                                     ekk_fixed_t load,
                                     ekk_fixed_t thermal,
                                     ekk_fixed_t power)
{
    if (mod == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    mod->my_field.components[EKK_FIELD_LOAD] = load;
    mod->my_field.components[EKK_FIELD_THERMAL] = thermal;
    mod->my_field.components[EKK_FIELD_POWER] = power;

    if (mod->on_field_change != NULL) {
        mod->on_field_change(mod);
    }

    return EKK_OK;
}

ekk_fixed_t ekk_module_get_gradient(const ekk_module_t *mod,
                                     ekk_field_component_t component)
{
    if (mod == NULL || component >= EKK_FIELD_COUNT) {
        return 0;
    }

    return mod->gradients[component];
}

/* ============================================================================
 * DEADLINE / SLACK OPERATIONS (MAPF-HET)
 * ============================================================================ */

/**
 * Normalization constant for slack field (100 seconds)
 * Slack values are normalized to [0, 1] where:
 * - 0.0 = critical (at deadline or past due)
 * - 1.0 = maximum slack (100+ seconds)
 */
#define EKK_SLACK_NORMALIZE_US      100000000

ekk_error_t ekk_module_compute_slack(ekk_module_t *mod, ekk_time_us_t now)
{
    if (mod == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    int64_t min_slack_us = (int64_t)EKK_SLACK_NORMALIZE_US;  /* Start at max */
    ekk_bool_t has_any_deadline = EKK_FALSE;

    /* Iterate all tasks and compute slack for those with deadlines */
    for (uint32_t i = 0; i < mod->task_count; i++) {
        ekk_internal_task_t *task = &mod->tasks[i];

        if (!task->has_deadline) {
            continue;
        }

        has_any_deadline = EKK_TRUE;

        /* Compute slack: deadline - (now + duration_estimate) */
        int64_t completion_time = (int64_t)now + (int64_t)task->deadline.duration_est;
        int64_t slack_us = (int64_t)task->deadline.deadline - completion_time;

        /* Update deadline struct — fixed-point, no float */
        if (slack_us >= 0) {
            task->deadline.slack = (ekk_fixed_t)((uint64_t)slack_us * 65536ULL / EKK_SLACK_NORMALIZE_US);
        } else {
            task->deadline.slack = -(ekk_fixed_t)((uint64_t)(-slack_us) * 65536ULL / EKK_SLACK_NORMALIZE_US);
        }
        task->deadline.critical = (slack_us < EKK_SLACK_THRESHOLD_US);

        /* Track minimum slack */
        if (slack_us < min_slack_us) {
            min_slack_us = slack_us;
        }
    }

    /* Update the slack field component — fixed-point, no float */
    if (has_any_deadline) {
        /* Normalize to [0, 1] range and clamp */
        ekk_fixed_t slack_fixed;
        if (min_slack_us <= 0) {
            slack_fixed = 0;
        } else if (min_slack_us >= (int64_t)EKK_SLACK_NORMALIZE_US) {
            slack_fixed = EKK_FIXED_ONE;
        } else {
            slack_fixed = (ekk_fixed_t)((uint64_t)min_slack_us * 65536ULL / EKK_SLACK_NORMALIZE_US);
        }

        mod->my_field.components[EKK_FIELD_SLACK] = slack_fixed;
    } else {
        /* No deadlines - maximum slack (1.0) */
        mod->my_field.components[EKK_FIELD_SLACK] = EKK_FIXED_ONE;
    }

    return EKK_OK;
}

ekk_error_t ekk_module_set_task_deadline(ekk_module_t *mod,
                                          ekk_task_id_t task_id,
                                          ekk_time_us_t deadline,
                                          ekk_time_us_t duration_est)
{
    if (mod == NULL || task_id >= mod->task_count) {
        return EKK_ERR_INVALID_ARG;
    }

    ekk_internal_task_t *task = &mod->tasks[task_id];
    task->has_deadline = EKK_TRUE;
    task->deadline.deadline = deadline;
    task->deadline.duration_est = duration_est;
    task->deadline.slack = 0;
    task->deadline.critical = EKK_FALSE;

    return EKK_OK;
}

ekk_error_t ekk_module_clear_task_deadline(ekk_module_t *mod,
                                            ekk_task_id_t task_id)
{
    if (mod == NULL || task_id >= mod->task_count) {
        return EKK_ERR_INVALID_ARG;
    }

    ekk_internal_task_t *task = &mod->tasks[task_id];
    task->has_deadline = EKK_FALSE;
    task->deadline.deadline = 0;
    task->deadline.duration_est = 0;
    task->deadline.slack = 0;
    task->deadline.critical = EKK_FALSE;

    return EKK_OK;
}

/* ============================================================================
 * CAPABILITY OPERATIONS (MAPF-HET)
 * ============================================================================ */

ekk_error_t ekk_module_set_capabilities(ekk_module_t *mod, ekk_capability_t caps)
{
    if (mod == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    mod->capabilities = caps;
    return EKK_OK;
}

ekk_capability_t ekk_module_get_capabilities(const ekk_module_t *mod)
{
    if (mod == NULL) {
        return 0;
    }

    return mod->capabilities;
}

ekk_error_t ekk_module_set_task_capabilities(ekk_module_t *mod,
                                              ekk_task_id_t task_id,
                                              ekk_capability_t caps)
{
    if (mod == NULL || task_id >= mod->task_count) {
        return EKK_ERR_INVALID_ARG;
    }

    mod->tasks[task_id].required_caps = caps;
    return EKK_OK;
}

/* ============================================================================
 * CONSENSUS SHORTCUTS
 * ============================================================================ */

ekk_error_t ekk_module_propose_mode(ekk_module_t *mod,
                                     uint32_t new_mode,
                                     ekk_ballot_id_t *ballot_id)
{
    if (mod == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    sync_consensus_electorate(mod);

    ekk_error_t err = ekk_consensus_propose(&mod->consensus,
                                             EKK_PROPOSAL_MODE_CHANGE,
                                             new_mode,
                                             EKK_THRESHOLD_SUPERMAJORITY,
                                             ballot_id);
    if (err == EKK_OK) {
        mod->ballots_started++;
    }

    return err;
}

ekk_error_t ekk_module_propose_power_limit(ekk_module_t *mod,
                                            uint32_t power_limit_mw,
                                            ekk_ballot_id_t *ballot_id)
{
    if (mod == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    sync_consensus_electorate(mod);

    ekk_error_t err = ekk_consensus_propose(&mod->consensus,
                                             EKK_PROPOSAL_POWER_LIMIT,
                                             power_limit_mw,
                                             EKK_THRESHOLD_SIMPLE_MAJORITY,
                                             ballot_id);
    if (err == EKK_OK) {
        mod->ballots_started++;
    }

    return err;
}

/* ============================================================================
 * DECISION LOGIC (weak, can be overridden)
 * ============================================================================ */

EKK_WEAK ekk_task_id_t ekk_module_select_task(ekk_module_t *mod,
                                               ekk_time_us_t now)
{
    if (mod == NULL || mod->task_count == 0) {
        return 0xFF;
    }
    ekk_task_id_t best_task = 0xFF;
    uint8_t best_priority = 0xFF;
    ekk_bool_t best_is_critical = EKK_FALSE;

    for (uint32_t i = 0; i < mod->task_count; i++) {
        ekk_internal_task_t *task = &mod->tasks[i];

        /* Skip non-ready tasks */
        if (task->state != EKK_TASK_READY) {
            continue;
        }

        /* Check if periodic task is due */
        if (task->period > 0 && task->next_run > now) {
            continue;
        }

        /* MAPF-HET: Check capability requirements */
        if (task->required_caps != 0) {
            if (!ekk_can_perform(mod->capabilities, task->required_caps)) {
                continue;  /* Module lacks required capabilities */
            }
        }

        /* MAPF-HET: Critical deadline tasks get priority boost */
        ekk_bool_t is_critical = task->has_deadline && task->deadline.critical;

        /* Priority rules:
         * 1. Critical deadline tasks always beat non-critical
         * 2. Among critical tasks, lower priority number wins
         * 3. Among non-critical tasks, lower priority number wins
         */
        ekk_bool_t select_this_task = EKK_FALSE;

        if (is_critical && !best_is_critical) {
            /* Critical beats non-critical */
            select_this_task = EKK_TRUE;
        } else if (is_critical == best_is_critical) {
            /* Same criticality - compare priority */
            if (task->priority < best_priority) {
                select_this_task = EKK_TRUE;
            }
        }

        if (select_this_task) {
            best_priority = task->priority;
            best_task = task->id;
            best_is_critical = is_critical;
        }
    }

    return best_task;
}

EKK_WEAK ekk_vote_value_t ekk_module_decide_vote(ekk_module_t *mod,
                                                  const ekk_ballot_t *ballot)
{
    EKK_UNUSED(mod);
    EKK_UNUSED(ballot);

    /* Default: vote yes */
    return EKK_VOTE_YES;
}

/* ============================================================================
 * STATUS
 * ============================================================================ */

ekk_error_t ekk_module_get_status(const ekk_module_t *mod,
                                   ekk_module_status_t *status)
{
    if (mod == NULL || status == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    status->id = mod->id;
    status->state = mod->state;
    status->neighbor_count = mod->topology.neighbor_count;
    status->load_gradient = mod->gradients[EKK_FIELD_LOAD];
    status->thermal_gradient = mod->gradients[EKK_FIELD_THERMAL];
    status->active_ballots = mod->consensus.active_ballot_count;
    status->ticks_total = mod->ticks_total;
    status->ticks_soft_degraded = mod->ticks_soft_degraded;
    status->ticks_hard_error = mod->ticks_hard_error;
    status->field_publish_ok = mod->field_publish_ok;
    status->field_publish_fail = mod->field_publish_fail;
    status->field_sample_fail = mod->field_sample_fail;
    status->neighbor_set_changes = mod->neighbor_set_changes;
    status->discovery_broadcasts = mod->discovery_broadcasts;
    status->heartbeat_state_changes = mod->heartbeat_state_changes;
    status->ballots_started = mod->ballots_started;
    status->ballots_completed = mod->ballots_completed;
    status->ballots_cancelled = mod->ballots_cancelled;
    status->ballots_timed_out = mod->ballots_timed_out;

    return EKK_OK;
}

void ekk_module_print_status(const ekk_module_t *mod)
{
    if (mod == NULL) {
        return;
    }

    ekk_hal_printf("Module %d (%s): state=%s neighbors=%u ticks=%u degraded=%u hard=%u\n",
                   mod->id,
                   mod->name ? mod->name : "?",
                   ekk_module_state_str(mod->state),
                   mod->topology.neighbor_count,
                   mod->ticks_total,
                   mod->ticks_soft_degraded,
                   mod->ticks_hard_error);

    ekk_hal_printf("  Gradients: load=%d thermal=%d power=%d\n",
                   mod->gradients[EKK_FIELD_LOAD],
                   mod->gradients[EKK_FIELD_THERMAL],
                   mod->gradients[EKK_FIELD_POWER]);
    ekk_hal_printf("  Metrics: field_ok=%u field_fail=%u sample_fail=%u neighbor_changes=%u discovery=%u heartbeat_changes=%u ballots(start=%u done=%u cancel=%u timeout=%u)\n",
                   mod->field_publish_ok,
                   mod->field_publish_fail,
                   mod->field_sample_fail,
                   mod->neighbor_set_changes,
                   mod->discovery_broadcasts,
                   mod->heartbeat_state_changes,
                   mod->ballots_started,
                   mod->ballots_completed,
                   mod->ballots_cancelled,
                   mod->ballots_timed_out);
}

/* ============================================================================
 * INTERNAL CALLBACKS
 * ============================================================================ */

static void on_neighbor_alive_cb(void *context, ekk_module_id_t id)
{
    ekk_module_t *mod = context;

    if (mod == NULL) {
        return;
    }

    mod->heartbeat_state_changes++;

    /* Add to topology if new */
    ekk_position_t pos = {0, 0, 0};  /* Position unknown from heartbeat */
    ekk_topology_on_discovery(&mod->topology, id, pos);

    if (mod->on_neighbor_found != NULL) {
        mod->on_neighbor_found(mod, id);
    }
}

static void on_neighbor_suspect_cb(void *context, ekk_module_id_t id)
{
    ekk_module_t *mod = context;
    EKK_UNUSED(id);

    if (mod == NULL) {
        return;
    }

    mod->heartbeat_state_changes++;
}

static void on_neighbor_dead_cb(void *context, ekk_module_id_t id)
{
    ekk_module_t *mod = context;

    if (mod == NULL) {
        return;
    }

    mod->heartbeat_state_changes++;

    ekk_topology_on_neighbor_lost(&mod->topology, id);

    /* Transition to REFORMING when a neighbor is lost and the module
     * was previously ACTIVE or DEGRADED — signals active mesh reformation. */
    if (mod->state == EKK_MODULE_ACTIVE || mod->state == EKK_MODULE_DEGRADED) {
        mod->state = EKK_MODULE_REFORMING;
    }

    if (mod->on_neighbor_lost != NULL) {
        mod->on_neighbor_lost(mod, id);
    }
}

static ekk_vote_value_t on_consensus_decide_cb(ekk_consensus_t *cons,
                                                void *context,
                                                const ekk_ballot_t *ballot)
{
    EKK_UNUSED(cons);
    ekk_module_t *mod = context;

    if (mod == NULL) {
        return EKK_VOTE_YES;
    }

    if (mod->on_vote_request != NULL) {
        mod->on_vote_request(mod, ballot);
    }

    return ekk_module_decide_vote(mod, ballot);
}

static void on_consensus_complete_cb(ekk_consensus_t *cons,
                                      void *context,
                                      const ekk_ballot_t *ballot,
                                      ekk_vote_result_t result)
{
    EKK_UNUSED(cons);
    ekk_module_t *mod = context;

    if (mod == NULL) {
        return;
    }

    switch (result) {
        case EKK_VOTE_APPROVED:
        case EKK_VOTE_REJECTED:
            mod->ballots_completed++;
            break;
        case EKK_VOTE_CANCELLED:
            mod->ballots_cancelled++;
            break;
        case EKK_VOTE_TIMEOUT:
            mod->ballots_timed_out++;
            break;
        default:
            break;
    }

    if (mod->on_consensus_complete != NULL) {
        mod->on_consensus_complete(mod, ballot);
    }
}

static void on_topology_changed_cb(ekk_topology_t *topo,
                                    const ekk_neighbor_t *old_neighbors,
                                    uint32_t old_count,
                                    const ekk_neighbor_t *new_neighbors,
                                    uint32_t new_count)
{
    EKK_UNUSED(old_neighbors);
    EKK_UNUSED(old_count);
    EKK_UNUSED(new_neighbors);
    EKK_UNUSED(new_count);

    if (topo == NULL) {
        return;
    }

    ekk_module_t *mod = NULL;
    if (topo->my_id != EKK_INVALID_MODULE_ID) {
        /* Module owns topology as an embedded field, so recover it by address. */
        mod = (ekk_module_t *)((char *)topo - offsetof(ekk_module_t, topology));
    }

    if (mod == NULL) {
        return;
    }

    mod->neighbor_set_changes++;
}
