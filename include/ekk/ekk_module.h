/**
 * @file ekk_module.h
 * @brief EK-KOR v2 - Module as First-Class Citizen
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * @license MIT
 * SPDX-License-Identifier: MIT
 *
 * Module-local coordination loop for the extracted runtime.
 *
 * There is no global scheduler object in this extract. Each module owns its
 * tasks, field state, topology, heartbeat, and consensus engine, then uses a
 * local arbitration policy during `ekk_module_tick()`.
 *
 * Runtime contract note:
 * - this code is single-threaded and non-reentrant by design
 * - `ekk_module_tick()` can complete as `EKK_OK`, `EKK_ERR_DEGRADED`, or a
 *   hard error
 */

#ifndef EKK_MODULE_H
#define EKK_MODULE_H

#include "ekk_types.h"
#include "ekk_field.h"
#include "ekk_topology.h"
#include "ekk_heartbeat.h"
#include "ekk_consensus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * INTERNAL TASK (per-module, not globally scheduled)
 * ============================================================================ */

/**
 * @brief Task state within a module
 */
typedef enum {
    EKK_TASK_IDLE       = 0,    /**< Not running */
    EKK_TASK_READY      = 1,    /**< Ready to run */
    EKK_TASK_RUNNING    = 2,    /**< Currently executing */
    EKK_TASK_BLOCKED    = 3,    /**< Waiting for event */
} ekk_task_state_t;

/**
 * @brief Task function signature
 */
typedef void (*ekk_task_fn)(void *arg);

/**
 * @brief Internal task (owned by module, not kernel)
 */
typedef struct {
    ekk_task_id_t id;               /**< Task ID within module */
    const char *name;               /**< Task name (debug) */
    ekk_task_fn function;           /**< Task function */
    void *arg;                      /**< Task argument */
    ekk_task_state_t state;         /**< Current state */
    uint8_t priority;               /**< Local priority (0 = highest) */
    ekk_time_us_t period;           /**< Period for periodic tasks (0 = one-shot) */
    ekk_time_us_t next_run;         /**< Next scheduled run time */
    uint32_t run_count;             /**< Execution count */
    ekk_time_us_t total_runtime;    /**< Total runtime in microseconds */

    /* MAPF-HET Integration: Deadline-aware scheduling */
    ekk_bool_t has_deadline;        /**< True if task has a deadline */
    ekk_deadline_t deadline;        /**< Deadline info (valid if has_deadline) */
    ekk_capability_t required_caps; /**< Capabilities required to run this task */
} ekk_internal_task_t;

/* ============================================================================
 * MODULE STRUCTURE
 * ============================================================================ */

/**
 * @brief Module - the first-class citizen of EK-KOR v2
 *
 * Each module is a self-contained coordination unit that:
 * - Owns internal tasks
 * - Publishes its coordination field
 * - Maintains k-neighbor topology
 * - Participates in consensus voting
 * - Self-schedules based on gradient fields
 */
typedef struct ekk_module {
    /* Identity */
    ekk_module_id_t id;                     /**< Module ID */
    const char *name;                       /**< Module name (debug) */
    ekk_module_state_t state;               /**< Current state */

    /* Coordination field (what I publish) */
    ekk_field_t my_field;                   /**< My current field values */
    ekk_field_t neighbor_aggregate;         /**< Aggregated neighbor fields */
    ekk_fixed_t gradients[EKK_FIELD_COUNT]; /**< Current gradients */

    /* Topology (who I coordinate with) */
    ekk_topology_t topology;                /**< Topological state */
    ekk_heartbeat_t heartbeat;              /**< Liveness tracking */

    /* Consensus (voting participation) */
    ekk_consensus_t consensus;              /**< Consensus engine */

    /* Internal tasks (what I execute) */
    ekk_internal_task_t tasks[EKK_MAX_TASKS_PER_MODULE];
    uint32_t task_count;
    ekk_task_id_t active_task;              /**< Currently running task */

    /* Timing */
    ekk_time_us_t last_tick;                /**< Last tick timestamp */
    ekk_time_us_t tick_period;              /**< Tick period */

    /* Statistics */
    uint32_t ticks_total;
    uint32_t ticks_soft_degraded;
    uint32_t ticks_hard_error;
    uint32_t field_publish_ok;
    uint32_t field_publish_fail;
    uint32_t field_sample_fail;
    uint32_t neighbor_set_changes;
    uint32_t discovery_broadcasts;
    uint32_t heartbeat_state_changes;
    uint32_t ballots_started;
    uint32_t ballots_completed;
    uint32_t ballots_cancelled;
    uint32_t ballots_timed_out;

    /* MAPF-HET Integration: Capability-based coordination */
    ekk_capability_t capabilities;      /**< This module's current capabilities */

    /* Callbacks */
    void (*on_field_change)(struct ekk_module *self);
    void (*on_neighbor_lost)(struct ekk_module *self, ekk_module_id_t lost_id);
    void (*on_neighbor_found)(struct ekk_module *self, ekk_module_id_t found_id);
    void (*on_vote_request)(struct ekk_module *self, const ekk_ballot_t *ballot);
    void (*on_consensus_complete)(struct ekk_module *self, const ekk_ballot_t *ballot);
    void (*on_state_change)(struct ekk_module *self, ekk_module_state_t old_state);

    /* User data */
    void *user_data;

} ekk_module_t;

/* ============================================================================
 * MODULE LIFECYCLE
 * ============================================================================ */

/**
 * @brief Initialize a module
 *
 * @param mod Module structure (caller-allocated)
 * @param id Module ID (must be unique in cluster)
 * @param name Module name (for debugging)
 * @param position Physical position (for distance calculation)
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_init(ekk_module_t *mod,
                             ekk_module_id_t id,
                             const char *name,
                             ekk_position_t position);

/**
 * @brief Start module operation
 *
 * Transitions from INIT to DISCOVERING state.
 * Begins broadcasting discovery messages.
 *
 * @param mod Module
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_start(ekk_module_t *mod);

/**
 * @brief Stop module operation
 *
 * Graceful shutdown: notifies neighbors, stops tasks.
 *
 * @param mod Module
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_stop(ekk_module_t *mod);

/**
 * @brief Main tick function - call periodically
 *
 * This is the heart of the coordination loop:
 * 1. Update heartbeats
 * 2. Sample neighbor fields
 * 3. Compute gradients
 * 4. Decide which internal task to run
 * 5. Execute task
 * 6. Publish updated field
 *
 * @param mod Module
 * @param now Current timestamp
 * @return EKK_OK on success, EKK_ERR_DEGRADED if the tick completed with
 *         stale or partial data, or a hard error if the tick aborted
 */
ekk_error_t ekk_module_tick(ekk_module_t *mod, ekk_time_us_t now);

/* ============================================================================
 * INTERNAL TASK MANAGEMENT
 * ============================================================================ */

/**
 * @brief Add internal task to module
 *
 * Tasks are owned by the module, not globally scheduled.
 * The module decides which task to run based on gradients.
 *
 * @param mod Module
 * @param name Task name
 * @param function Task function
 * @param arg Task argument
 * @param priority Local priority (0 = highest)
 * @param period Period in microseconds (0 = run once when ready)
 * @param[out] task_id Assigned task ID
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_add_task(ekk_module_t *mod,
                                 const char *name,
                                 ekk_task_fn function,
                                 void *arg,
                                 uint8_t priority,
                                 ekk_time_us_t period,
                                 ekk_task_id_t *task_id);

/**
 * @brief Set task ready to run
 */
ekk_error_t ekk_module_task_ready(ekk_module_t *mod, ekk_task_id_t task_id);

/**
 * @brief Block task
 */
ekk_error_t ekk_module_task_block(ekk_module_t *mod, ekk_task_id_t task_id);

/* ============================================================================
 * FIELD OPERATIONS
 * ============================================================================ */

/**
 * @brief Update module's coordination field
 *
 * Call this when module state changes significantly.
 * Field will be published to neighbors.
 *
 * @param mod Module
 * @param load Current load (0.0 - 1.0 as Q16.16)
 * @param thermal Temperature (normalized)
 * @param power Power consumption (normalized)
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_update_field(ekk_module_t *mod,
                                     ekk_fixed_t load,
                                     ekk_fixed_t thermal,
                                     ekk_fixed_t power);

/**
 * @brief Get current gradient for a component
 *
 * @param mod Module
 * @param component Field component
 * @return Gradient (positive = neighbors higher, negative = neighbors lower)
 */
ekk_fixed_t ekk_module_get_gradient(const ekk_module_t *mod,
                                     ekk_field_component_t component);

/* ============================================================================
 * DEADLINE / SLACK OPERATIONS (MAPF-HET)
 * ============================================================================ */

/**
 * @brief Compute slack for all tasks with deadlines
 *
 * Updates the slack field for each task that has a deadline:
 *   slack = deadline - (now + duration_estimate)
 *   critical = slack < EKK_SLACK_THRESHOLD_US
 *
 * Also updates the module's EKK_FIELD_SLACK component with the minimum
 * slack across all deadline-constrained tasks (normalized to 0.0-1.0).
 *
 * Algorithm from MAPF-HET deadline_cbs.go:231-270
 *
 * @param mod Module
 * @param now Current timestamp
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_compute_slack(ekk_module_t *mod, ekk_time_us_t now);

/**
 * @brief Set task deadline
 *
 * @param mod Module
 * @param task_id Task to modify
 * @param deadline Absolute deadline timestamp
 * @param duration_est Estimated task duration
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_set_task_deadline(ekk_module_t *mod,
                                          ekk_task_id_t task_id,
                                          ekk_time_us_t deadline,
                                          ekk_time_us_t duration_est);

/**
 * @brief Clear task deadline
 *
 * @param mod Module
 * @param task_id Task to modify
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_clear_task_deadline(ekk_module_t *mod,
                                            ekk_task_id_t task_id);

/* ============================================================================
 * CAPABILITY OPERATIONS (MAPF-HET)
 * ============================================================================ */

/**
 * @brief Set module capabilities
 *
 * Updates the module's capability bitmask. Typically called when:
 * - Thermal state changes (enable/disable EKK_CAP_THERMAL_OK)
 * - Configuration changes (V2G, gateway role)
 *
 * @param mod Module
 * @param caps New capability bitmask
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_set_capabilities(ekk_module_t *mod, ekk_capability_t caps);

/**
 * @brief Get module capabilities
 *
 * @param mod Module
 * @return Current capability bitmask
 */
ekk_capability_t ekk_module_get_capabilities(const ekk_module_t *mod);

/**
 * @brief Set required capabilities for a task
 *
 * Task will only be selected for execution if the module has all
 * required capabilities (checked via ekk_can_perform).
 *
 * @param mod Module
 * @param task_id Task to modify
 * @param caps Required capabilities
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_set_task_capabilities(ekk_module_t *mod,
                                              ekk_task_id_t task_id,
                                              ekk_capability_t caps);

/* ============================================================================
 * CONSENSUS SHORTCUTS
 * ============================================================================ */

/**
 * @brief Propose mode change to neighbors
 *
 * Convenience function for common operation.
 *
 * @param mod Module
 * @param new_mode Proposed mode
 * @param[out] ballot_id Ballot for tracking
 * @return EKK_OK on success
 */
ekk_error_t ekk_module_propose_mode(ekk_module_t *mod,
                                     uint32_t new_mode,
                                     ekk_ballot_id_t *ballot_id);

/**
 * @brief Propose power limit change
 */
ekk_error_t ekk_module_propose_power_limit(ekk_module_t *mod,
                                            uint32_t power_limit_mw,
                                            ekk_ballot_id_t *ballot_id);

/* ============================================================================
 * DECISION LOGIC (application overrides)
 * ============================================================================ */

/**
 * @brief Default task selection based on gradients
 *
 * Override this for custom scheduling logic.
 *
 * @param mod Module
 * @param now Authoritative logical time for this scheduling decision
 * @return Task ID to run, or 0xFF for idle
 */
EKK_WEAK ekk_task_id_t ekk_module_select_task(ekk_module_t *mod,
                                               ekk_time_us_t now);

/**
 * @brief Default vote decision
 *
 * Override this for custom voting logic.
 *
 * @param mod Module
 * @param ballot Ballot to vote on
 * @return Vote value
 */
EKK_WEAK ekk_vote_value_t ekk_module_decide_vote(ekk_module_t *mod,
                                                  const ekk_ballot_t *ballot);

/* ============================================================================
 * STATUS AND DEBUGGING
 * ============================================================================ */

/**
 * @brief Get module status summary
 */
typedef struct {
    ekk_module_id_t id;
    ekk_module_state_t state;
    uint32_t neighbor_count;
    ekk_fixed_t load_gradient;
    ekk_fixed_t thermal_gradient;
    uint32_t active_ballots;
    uint32_t ticks_total;
    uint32_t ticks_soft_degraded;
    uint32_t ticks_hard_error;
    uint32_t field_publish_ok;
    uint32_t field_publish_fail;
    uint32_t field_sample_fail;
    uint32_t neighbor_set_changes;
    uint32_t discovery_broadcasts;
    uint32_t heartbeat_state_changes;
    uint32_t ballots_started;
    uint32_t ballots_completed;
    uint32_t ballots_cancelled;
    uint32_t ballots_timed_out;
} ekk_module_status_t;

ekk_error_t ekk_module_get_status(const ekk_module_t *mod,
                                   ekk_module_status_t *status);

/**
 * @brief Print module status (for debugging)
 */
void ekk_module_print_status(const ekk_module_t *mod);

#ifdef __cplusplus
}
#endif

#endif /* EKK_MODULE_H */
