/**
 * @file ekk_field.h
 * @brief EK-KOR v2 - Coordination Field Primitives
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * @license MIT
 * SPDX-License-Identifier: MIT
 *
 * Shared coordination fields for the extracted runtime.
 *
 * Modules publish decaying field values into a shared region; neighbors sample
 * those values and derive gradients. In this extract the field engine is a
 * shared-memory coordination mechanism, not a kernel primitive.
 */

#ifndef EKK_FIELD_H
#define EKK_FIELD_H

#include "ekk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * FIELD CONFIGURATION
 * ============================================================================ */

/**
 * @brief Field decay model
 */
typedef enum {
    EKK_DECAY_EXPONENTIAL,      /**< f(t) = f0 * exp(-t/tau) */
    EKK_DECAY_LINEAR,           /**< f(t) = f0 * (1 - t/tau), clamped to 0 */
    EKK_DECAY_STEP,             /**< f(t) = f0 if t < tau, else 0 */
} ekk_decay_model_t;

/**
 * @brief Field configuration per component
 */
typedef struct {
    ekk_fixed_t decay_tau;          /**< Decay time constant (Q16.16 seconds) */
    ekk_decay_model_t decay_model;  /**< Decay function */
    ekk_fixed_t min_value;          /**< Floor (clamp) */
    ekk_fixed_t max_value;          /**< Ceiling (clamp) */
    ekk_fixed_t default_value;      /**< Value when no data */
} ekk_field_config_t;

/* ============================================================================
 * COORDINATION FIELD WITH SEQUENCE COUNTER (LOCK-FREE CONSISTENCY)
 * ============================================================================ */

/**
 * @brief Coordination field with sequence counter for lock-free reads
 *
 * Uses the classic seqlock pattern:
 * - Writer increments sequence to ODD before write (write in progress)
 * - Writer increments sequence to EVEN after write (write complete)
 * - Reader checks sequence before/after read; retries if mismatched or odd
 *
 * This allows lock-free, wait-free reads with consistency guarantees.
 */
typedef struct {
    ekk_field_t field;              /**< The actual field data */
    volatile uint32_t sequence;     /**< Sequence counter (odd = write in progress) */
} ekk_coord_field_t;

/* ============================================================================
 * FIELD ENGINE STATE
 * ============================================================================ */

/**
 * @brief Shared field region (one per cluster)
 *
 * This is the "environment" through which modules coordinate.
 * Placed in shared memory accessible by all modules.
 *
 * Uses ekk_coord_field_t for lock-free consistency via sequence counters.
 */
typedef struct {
    ekk_coord_field_t fields[EKK_MAX_MODULES]; /**< Published fields with seqlock */
    volatile uint32_t update_flags[(EKK_MAX_MODULES + 31) / 32]; /**< Bitmask of updated modules */
    ekk_time_us_t last_gc;                     /**< Last garbage collection */
} ekk_field_region_t;

/* ============================================================================
 * FIELD API
 * ============================================================================ */

/**
 * @brief Initialize field engine
 *
 * @param region Pointer to shared field region (must be in shared memory)
 * @return EKK_OK on success
 */
ekk_error_t ekk_field_init(ekk_field_region_t *region);

/**
 * @brief Publish module's coordination field
 *
 * Updates the shared field region with this module's current state.
 * Other modules will see this field and can compute gradients.
 *
 * @param module_id Publishing module's ID
 * @param field Field values to publish
 * @return EKK_OK on success
 *
 * @note Thread-safe, uses atomic operations
 * @note Timestamp is set automatically to current time
 */
ekk_error_t ekk_field_publish(ekk_module_id_t module_id,
                               const ekk_field_t *field);

/**
 * @brief Publish field using caller-provided logical time
 *
 * Use this when the caller already owns the authoritative time base for the
 * current coordination step and wants publication timestamps to match it.
 *
 * @param module_id Publishing module's ID
 * @param field Field values to publish
 * @param now Logical timestamp to stamp into the published field
 * @return EKK_OK on success
 */
ekk_error_t ekk_field_publish_at(ekk_module_id_t module_id,
                                  const ekk_field_t *field,
                                  ekk_time_us_t now);

/**
 * @brief Sample a specific module's field with decay applied
 *
 * Reads the field published by target_id and applies temporal decay
 * based on how old the field is.
 *
 * @param target_id Module to sample
 * @param[out] field Decayed field values
 * @return EKK_OK on success, EKK_ERR_FIELD_EXPIRED if too old
 */
ekk_error_t ekk_field_sample(ekk_module_id_t target_id,
                              ekk_field_t *field);

/**
 * @brief Sample a specific module's field using caller-provided logical time
 *
 * @param target_id Module to sample
 * @param[out] field Decayed field values
 * @param now Logical timestamp for age/expiry evaluation
 * @return EKK_OK on success, EKK_ERR_FIELD_EXPIRED if too old
 */
ekk_error_t ekk_field_sample_at(ekk_module_id_t target_id,
                                 ekk_field_t *field,
                                 ekk_time_us_t now);

/**
 * @brief Sample all k-neighbors and compute aggregate
 *
 * Returns weighted average of neighbor fields, with weights based on:
 * - Recency (newer fields weighted higher)
 * - Health state (healthy neighbors weighted higher)
 * - Logical distance (closer neighbors weighted higher)
 *
 * @param neighbors Array of neighbor info (from topology layer)
 * @param neighbor_count Number of neighbors
 * @param[out] aggregate Aggregated field
 * @return EKK_OK on success, EKK_ERR_DEGRADED if eligible neighbors exist
 *         but no field sample succeeds
 */
ekk_error_t ekk_field_sample_neighbors(const ekk_neighbor_t *neighbors,
                                        uint32_t neighbor_count,
                                        ekk_field_t *aggregate);

/**
 * @brief Sample all k-neighbors using caller-provided logical time
 *
 * @param neighbors Array of neighbor info (from topology layer)
 * @param neighbor_count Number of neighbors
 * @param[out] aggregate Aggregated field
 * @param now Logical timestamp for age/decay evaluation
 * @return EKK_OK on success, EKK_ERR_DEGRADED if eligible neighbors exist
 *         but no field sample succeeds
 */
ekk_error_t ekk_field_sample_neighbors_at(const ekk_neighbor_t *neighbors,
                                           uint32_t neighbor_count,
                                           ekk_field_t *aggregate,
                                           ekk_time_us_t now);

/**
 * @brief Compute gradient for a specific field component
 *
 * Returns the direction of decreasing potential:
 * - Positive: neighbors have higher values (I should increase activity)
 * - Negative: neighbors have lower values (I should decrease activity)
 * - Zero: balanced
 *
 * @param my_field This module's field
 * @param neighbor_aggregate Aggregated neighbor field
 * @param component Which component to compute gradient for
 * @return Gradient value (Q16.16)
 *
 * EXAMPLE USE:
 * @code
 * ekk_fixed_t load_gradient = ekk_field_gradient(&my_field, &neighbors, EKK_FIELD_LOAD);
 * if (load_gradient > THRESHOLD) {
 *     // Neighbors are overloaded, I should take more work
 * }
 * @endcode
 */
ekk_fixed_t ekk_field_gradient(const ekk_field_t *my_field,
                                const ekk_field_t *neighbor_aggregate,
                                ekk_field_component_t component);

/**
 * @brief Compute gradient vector for all components
 *
 * @param my_field This module's field
 * @param neighbor_aggregate Aggregated neighbor field
 * @param[out] gradients Array of EKK_FIELD_COUNT gradients
 */
void ekk_field_gradient_all(const ekk_field_t *my_field,
                             const ekk_field_t *neighbor_aggregate,
                             ekk_fixed_t *gradients);

/**
 * @brief Apply decay to a field based on elapsed time
 *
 * @param field Field to decay (modified in place)
 * @param elapsed_us Microseconds since field was published
 */
void ekk_field_apply_decay(ekk_field_t *field, ekk_time_us_t elapsed_us);

/**
 * @brief Garbage collect expired fields
 *
 * Marks fields older than max_age as invalid.
 * Called periodically by kernel.
 *
 * @param max_age_us Maximum field age in microseconds
 * @return Number of fields expired
 */
uint32_t ekk_field_gc(ekk_time_us_t max_age_us);

/* ============================================================================
 * LOCK-FREE CONSISTENT READ API
 * ============================================================================ */

/**
 * @brief Read field with consistency check (single attempt)
 *
 * Uses sequence counter to detect torn reads:
 * 1. Read sequence before
 * 2. If odd, return false (write in progress)
 * 3. Copy field data
 * 4. Read sequence after
 * 5. If sequence changed, return false (torn read)
 *
 * @param target_id Module to read
 * @param[out] field Output field (only valid if return is true)
 * @return true if read was consistent, false if should retry
 */
ekk_bool_t ekk_field_read_consistent(ekk_module_id_t target_id, ekk_field_t *field);

/**
 * @brief Sample field with automatic retry on inconsistency
 *
 * Attempts up to max_retries reads until consistent.
 * Applies decay after successful read.
 *
 * @param target_id Module to sample
 * @param[out] field Decayed field values
 * @param max_retries Maximum retry attempts (typically 3)
 * @return EKK_OK on success, EKK_ERR_BUSY if all retries failed, EKK_ERR_FIELD_EXPIRED if too old
 */
ekk_error_t ekk_field_sample_consistent(ekk_module_id_t target_id,
                                         ekk_field_t *field,
                                         uint32_t max_retries);

/**
 * @brief Sample field with automatic retry using caller-provided logical time
 *
 * @param target_id Module to sample
 * @param[out] field Decayed field values
 * @param max_retries Maximum retry attempts (typically 3)
 * @param now Logical timestamp for age/expiry evaluation
 * @return EKK_OK on success, EKK_ERR_BUSY if all retries failed,
 *         EKK_ERR_FIELD_EXPIRED if too old
 */
ekk_error_t ekk_field_sample_consistent_at(ekk_module_id_t target_id,
                                            ekk_field_t *field,
                                            uint32_t max_retries,
                                            ekk_time_us_t now);

/**
 * @brief Publish field with sequence counter update
 *
 * Atomically updates field using seqlock pattern:
 * 1. Increment sequence to odd (write starting)
 * 2. Memory barrier
 * 3. Copy field data
 * 4. Memory barrier
 * 5. Increment sequence to even (write complete)
 * 6. Set update flag
 *
 * @param module_id Publishing module's ID
 * @param field Field values to publish
 * @return EKK_OK on success
 */
ekk_error_t ekk_field_publish_consistent(ekk_module_id_t module_id,
                                          const ekk_field_t *field);

/* ============================================================================
 * FIELD UTILITIES
 * ============================================================================ */

/**
 * @brief Check if field is valid (not expired)
 */
static inline ekk_bool_t ekk_field_is_valid(const ekk_field_t *field,
                                              ekk_time_us_t now,
                                              ekk_time_us_t max_age_us)
{
    return (field->source != EKK_INVALID_MODULE_ID) &&
           ((now - field->timestamp) < max_age_us);
}

/**
 * @brief Create field from raw values
 */
static inline void ekk_field_set(ekk_field_t *field,
                                  ekk_fixed_t load,
                                  ekk_fixed_t thermal,
                                  ekk_fixed_t power)
{
    field->components[EKK_FIELD_LOAD] = load;
    field->components[EKK_FIELD_THERMAL] = thermal;
    field->components[EKK_FIELD_POWER] = power;
}

/* ============================================================================
 * FIELD ARITHMETIC
 * ============================================================================ */

/**
 * @brief Add two fields component-wise
 */
void ekk_field_add(ekk_field_t *result,
                   const ekk_field_t *a,
                   const ekk_field_t *b);

/**
 * @brief Scale field by fixed-point factor
 */
void ekk_field_scale(ekk_field_t *field, ekk_fixed_t factor);

/**
 * @brief Linear interpolation between two fields
 *
 * result = a * (1 - t) + b * t
 *
 * @param t Interpolation factor (0 = a, EKK_FIXED_ONE = b)
 */
void ekk_field_lerp(ekk_field_t *result,
                    const ekk_field_t *a,
                    const ekk_field_t *b,
                    ekk_fixed_t t);

/* ============================================================================
 * STIGMERGY THERMAL OPTIMIZATION (ROJ Paper Section VII, Algorithm 4)
 * ============================================================================ */

/**
 * @brief Stigmergy configuration for thermal optimization
 *
 * ROJ Paper Algorithm 4: Stigmergy-based thermal balancing using
 * decaying heat tags and neighbor-relative ranking.
 *
 * Parameters:
 * - target_temp: Target junction temperature (e.g., 348K = 75C)
 * - temp_threshold: Deadband (default 5K = 5 degrees)
 * - alpha: Tag update rate (default 0.1)
 * - tau: Tag decay time constant (default 60s)
 * - max_load_adjust: Maximum load redistribution (default 10%)
 */
typedef struct {
    ekk_fixed_t target_temp;         /**< Target Tj in Kelvin (Q16.16) */
    ekk_fixed_t temp_threshold;      /**< Deadband threshold (Q16.16, default 5K) */
    ekk_fixed_t alpha;               /**< Tag update rate (Q16.16, default 0.1) */
    ekk_time_us_t tau;               /**< Decay time constant (microseconds, default 60s) */
    ekk_fixed_t max_load_adjust;     /**< Max load adjustment (Q16.16, default 0.1 = 10%) */
} ekk_stigmergy_config_t;

/**
 * @brief Default stigmergy configuration
 *
 * - Target: 75C (348K)
 * - Threshold: 5K
 * - Alpha: 0.1
 * - Tau: 60 seconds
 * - Max adjustment: 10%
 */
#define EKK_STIGMERGY_CONFIG_DEFAULT { \
    .target_temp = 22806528,    /* 348.0 in Q16.16 (348 * 65536) */ \
    .temp_threshold = 327680,   /* 5.0 in Q16.16 (5 * 65536) */ \
    .alpha = 6553,              /* 0.1 in Q16.16 (int)(0.1 * 65536) */ \
    .tau = 60000000,            /* 60 seconds */ \
    .max_load_adjust = 6553,    /* 0.1 in Q16.16 (int)(0.1 * 65536) */ \
}

/**
 * @brief Stigmergy context for a module
 *
 * Maintains the heat tag and neighbor tags for computing load adjustments.
 */
typedef struct {
    ekk_fixed_t my_tag;                          /**< This module's heat tag */
    ekk_fixed_t neighbor_tags[EKK_K_NEIGHBORS];  /**< Cached neighbor tags */
    ekk_module_id_t neighbor_ids[EKK_K_NEIGHBORS]; /**< IDs of k-neighbors */
    uint8_t neighbor_count;                      /**< Active neighbor count */
    ekk_time_us_t last_update;                   /**< Last tag update time */
    ekk_stigmergy_config_t config;               /**< Configuration */
} ekk_stigmergy_ctx_t;

/**
 * @brief Initialize stigmergy context
 *
 * @param ctx Stigmergy context (caller-allocated)
 * @param config Configuration (or NULL for defaults)
 * @return EKK_OK on success
 */
ekk_error_t ekk_stigmergy_init(ekk_stigmergy_ctx_t *ctx,
                                const ekk_stigmergy_config_t *config);

/**
 * @brief Update heat tag based on junction temperature
 *
 * Algorithm 4 from ROJ paper:
 * 1. Apply exponential decay: tag(t) = tag(t-1) * exp(-dt/tau)
 * 2. Update based on temperature delta:
 *    - If Tj > target + threshold: tag += alpha * (Tj - target)
 *    - If Tj < target - threshold: tag -= alpha * (target - Tj)
 *
 * @param ctx Stigmergy context
 * @param Tj Current junction temperature in Kelvin (Q16.16)
 * @param now Current timestamp
 * @return EKK_OK on success
 */
ekk_error_t ekk_stigmergy_update_tag(ekk_stigmergy_ctx_t *ctx,
                                       ekk_fixed_t Tj,
                                       ekk_time_us_t now);

/**
 * @brief Update neighbor tag from received field
 *
 * @param ctx Stigmergy context
 * @param neighbor_id Neighbor module ID
 * @param neighbor_tag Neighbor's heat tag
 * @return EKK_OK on success
 */
ekk_error_t ekk_stigmergy_update_neighbor(ekk_stigmergy_ctx_t *ctx,
                                            ekk_module_id_t neighbor_id,
                                            ekk_fixed_t neighbor_tag);

/**
 * @brief Compute load adjustment factor
 *
 * Based on rank among k-neighbors:
 * - Cooler modules (lower tag, lower rank) get positive adjustment (more load)
 * - Hotter modules (higher tag, higher rank) get negative adjustment (less load)
 *
 * Formula: adjustment = max_adjust * (k/2 - rank) / k
 *
 * @param ctx Stigmergy context
 * @return Load adjustment factor (Q16.16, range [-max_adjust, +max_adjust])
 */
ekk_fixed_t ekk_stigmergy_compute_load_factor(const ekk_stigmergy_ctx_t *ctx);

/**
 * @brief Get current heat tag
 */
static inline ekk_fixed_t ekk_stigmergy_get_tag(const ekk_stigmergy_ctx_t *ctx) {
    return ctx->my_tag;
}

/**
 * @brief Exponential decay helper
 *
 * Computes exp(-elapsed/tau) using fixed-point approximation.
 * Uses Taylor series for small values, lookup for larger.
 *
 * @param elapsed_us Elapsed time in microseconds
 * @param tau_us Time constant in microseconds
 * @return Decay factor (Q16.16, 0.0-1.0)
 */
ekk_fixed_t ekk_fixed_exp_decay(ekk_time_us_t elapsed_us, ekk_time_us_t tau_us);

#ifdef __cplusplus
}
#endif

#endif /* EKK_FIELD_H */
