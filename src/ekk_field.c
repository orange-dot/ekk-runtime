/**
 * @file ekk_field.c
 * @brief EK-KOR v2 - Coordination Field Implementation
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * NOVELTY: Potential Field Scheduling
 * - Modules publish decaying fields to shared memory
 * - Neighbors sample and compute gradients
 * - Scheduling decisions emerge from gradient-following
 */

#include "ekk/ekk_field.h"
#include "ekk/ekk_hal.h"
#include <string.h>

/* ============================================================================
 * EXTERNAL DECLARATIONS (from ekk_types.c)
 * ============================================================================ */

extern ekk_fixed_t ekk_fixed_mul(ekk_fixed_t a, ekk_fixed_t b);
extern ekk_fixed_t ekk_fixed_div(ekk_fixed_t a, ekk_fixed_t b);
extern ekk_fixed_t ekk_fixed_exp_decay(ekk_time_us_t elapsed_us, ekk_time_us_t tau_us);

/* ============================================================================
 * PRIVATE STATE
 * ============================================================================ */

/** Global field region pointer (set by ekk_field_init) */
static ekk_field_region_t *g_field_region = NULL;

/** Default decay tau in microseconds */
static ekk_time_us_t g_decay_tau_us = EKK_FIELD_DECAY_TAU_US;

/** Maximum field age (5 * tau) */
#define EKK_FIELD_MAX_AGE_US    (EKK_FIELD_DECAY_TAU_US * 5)

/* ============================================================================
 * INITIALIZATION
 * ============================================================================ */

ekk_error_t ekk_field_init(ekk_field_region_t *region)
{
    if (region == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Clear all fields */
    memset(region, 0, sizeof(ekk_field_region_t));

    /* Initialize all fields as invalid */
    for (int i = 0; i < EKK_MAX_MODULES; i++) {
        region->fields[i].field.source = EKK_INVALID_MODULE_ID;
        region->fields[i].sequence = 0;
    }

    g_field_region = region;
    return EKK_OK;
}

/* ============================================================================
 * FIELD PUBLISH
 * ============================================================================ */

ekk_error_t ekk_field_publish_at(ekk_module_id_t module_id,
                                  const ekk_field_t *field,
                                  ekk_time_us_t now)
{
    if (g_field_region == NULL) {
        return EKK_ERR_HAL_FAILURE;
    }

    if (module_id == EKK_INVALID_MODULE_ID || module_id >= EKK_MAX_MODULES) {
        return EKK_ERR_INVALID_ARG;
    }

    if (field == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Get target slot (using coord field with seqlock) */
    ekk_coord_field_t *cf = &g_field_region->fields[module_id];

    /* Memory barrier before write */
    ekk_hal_memory_barrier();

    /* Increment sequence to ODD (write in progress) */
    ekk_hal_atomic_inc(&cf->sequence);

    ekk_hal_memory_barrier();

    /* Copy field data */
    for (int i = 0; i < EKK_FIELD_COUNT; i++) {
        cf->field.components[i] = field->components[i];
    }
    cf->field.timestamp = now;
    cf->field.source = module_id;
    cf->field.sequence = field->sequence;

    /* Memory barrier after write */
    ekk_hal_memory_barrier();

    /* Increment sequence to EVEN (write complete) */
    ekk_hal_atomic_inc(&cf->sequence);

    /* Set update flag */
    uint32_t word = module_id / 32;
    uint32_t bit = (1u << (module_id % 32));
    uint32_t state = ekk_hal_critical_enter();
    g_field_region->update_flags[word] |= bit;
    ekk_hal_critical_exit(state);

    /* Sync to ensure visibility */
    ekk_hal_sync_field_region();

    return EKK_OK;
}

ekk_error_t ekk_field_publish(ekk_module_id_t module_id, const ekk_field_t *field)
{
    return ekk_field_publish_at(module_id, field, ekk_hal_time_us());
}

/* ============================================================================
 * FIELD SAMPLE
 * ============================================================================ */

ekk_error_t ekk_field_sample_at(ekk_module_id_t target_id,
                                 ekk_field_t *field,
                                 ekk_time_us_t now)
{
    if (g_field_region == NULL) {
        return EKK_ERR_HAL_FAILURE;
    }

    if (target_id == EKK_INVALID_MODULE_ID || target_id >= EKK_MAX_MODULES) {
        return EKK_ERR_INVALID_ARG;
    }

    if (field == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Read from coord field with seqlock consistency check */
    const ekk_coord_field_t *cf = &g_field_region->fields[target_id];

    /* Memory barrier before read */
    ekk_hal_memory_barrier();

    /* Read sequence before */
    uint32_t seq_before = cf->sequence;
    if (seq_before & 1) {
        /* Write in progress, try later */
        return EKK_ERR_BUSY;
    }

    ekk_hal_memory_barrier();

    /* Check validity */
    if (cf->field.source == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_NOT_FOUND;
    }

    /* Check age */
    ekk_time_us_t age = now - cf->field.timestamp;
    if (age > EKK_FIELD_MAX_AGE_US) {
        return EKK_ERR_FIELD_EXPIRED;
    }

    /* Copy field data */
    memcpy(field, &cf->field, sizeof(ekk_field_t));

    /* Memory barrier after read */
    ekk_hal_memory_barrier();

    /* Check sequence after - retry if changed */
    if (cf->sequence != seq_before) {
        return EKK_ERR_BUSY;  /* Torn read, caller should retry */
    }

    /* Apply decay based on age */
    ekk_field_apply_decay(field, age);

    return EKK_OK;
}

ekk_error_t ekk_field_sample(ekk_module_id_t target_id, ekk_field_t *field)
{
    return ekk_field_sample_at(target_id, field, ekk_hal_time_us());
}

/* ============================================================================
 * CONSISTENT FIELD SAMPLE (with retry)
 * ============================================================================ */

ekk_error_t ekk_field_sample_consistent_at(ekk_module_id_t target_id,
                                            ekk_field_t *field,
                                            uint32_t max_retries,
                                            ekk_time_us_t now)
{
    ekk_error_t err = EKK_ERR_BUSY;

    for (uint32_t attempt = 0; attempt <= max_retries; attempt++) {
        err = ekk_field_sample_at(target_id, field, now);
        if (err != EKK_ERR_BUSY) {
            return err;
        }
    }

    return err;
}

ekk_error_t ekk_field_sample_consistent(ekk_module_id_t target_id,
                                         ekk_field_t *field,
                                         uint32_t max_retries)
{
    return ekk_field_sample_consistent_at(target_id, field, max_retries,
                                          ekk_hal_time_us());
}

/* ============================================================================
 * NEIGHBOR AGGREGATION
 * ============================================================================ */

ekk_error_t ekk_field_sample_neighbors_at(const ekk_neighbor_t *neighbors,
                                           uint32_t neighbor_count,
                                           ekk_field_t *aggregate,
                                           ekk_time_us_t now)
{
    if (neighbors == NULL || aggregate == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Clear aggregate */
    memset(aggregate, 0, sizeof(ekk_field_t));

    if (neighbor_count == 0) {
        return EKK_OK;
    }

    /* Accumulate weighted sum */
    int64_t sums[EKK_FIELD_COUNT] = {0};
    ekk_fixed_t total_weight = 0;
    uint32_t eligible_count = 0;
    uint32_t valid_count = 0;
    ekk_time_us_t max_timestamp = 0;

    for (uint32_t i = 0; i < neighbor_count; i++) {
        const ekk_neighbor_t *neighbor = &neighbors[i];

        /* Skip dead or unknown neighbors */
        if (neighbor->health == EKK_HEALTH_DEAD ||
            neighbor->health == EKK_HEALTH_UNKNOWN) {
            continue;
        }

        eligible_count++;

        /* Sample neighbor's field with retry to tolerate torn reads */
        ekk_field_t nfield;
        ekk_error_t err = ekk_field_sample_consistent_at(neighbor->id, &nfield, 3, now);
        if (err != EKK_OK) {
            continue;
        }

        /* Compute weight based on health and distance */
        ekk_fixed_t weight = EKK_FIXED_ONE;

        /* Health factor: suspect neighbors weighted at 50% */
        if (neighbor->health == EKK_HEALTH_SUSPECT) {
            weight = EKK_FIXED_HALF;
        }

        /* Distance factor: closer neighbors weighted higher */
        /* w = 1 / (1 + distance/256) approximated */
        if (neighbor->logical_distance > 0) {
            ekk_fixed_t dist_factor = ekk_fixed_div(
                EKK_FIXED_ONE,
                EKK_FIXED_ONE + (neighbor->logical_distance << 8)
            );
            weight = ekk_fixed_mul(weight, dist_factor);
        }

        /* Accumulate weighted components */
        for (int c = 0; c < EKK_FIELD_COUNT; c++) {
            sums[c] += (int64_t)ekk_fixed_mul(nfield.components[c], weight);
        }
        total_weight += weight;
        valid_count++;

        if (nfield.timestamp > max_timestamp) {
            max_timestamp = nfield.timestamp;
        }
    }

    /* Compute weighted average using 64-bit arithmetic to avoid division by zero
     * when total_weight < 65536 (i.e. total_weight >> 16 == 0) */
    if (total_weight > 0) {
        for (int c = 0; c < EKK_FIELD_COUNT; c++) {
            aggregate->components[c] = (ekk_fixed_t)((int64_t)sums[c] * 65536LL / total_weight);
        }
    }

    aggregate->timestamp = max_timestamp;
    aggregate->source = EKK_INVALID_MODULE_ID;  /* Aggregate, not single source */
    aggregate->sequence = 0;

    if (eligible_count > 0 && valid_count == 0) {
        return EKK_ERR_DEGRADED;
    }

    return EKK_OK;
}

ekk_error_t ekk_field_sample_neighbors(const ekk_neighbor_t *neighbors,
                                        uint32_t neighbor_count,
                                        ekk_field_t *aggregate)
{
    return ekk_field_sample_neighbors_at(neighbors, neighbor_count, aggregate,
                                         ekk_hal_time_us());
}

/* ============================================================================
 * GRADIENT COMPUTATION
 * ============================================================================ */

ekk_fixed_t ekk_field_gradient(const ekk_field_t *my_field,
                                const ekk_field_t *neighbor_aggregate,
                                ekk_field_component_t component)
{
    if (my_field == NULL || neighbor_aggregate == NULL) {
        return 0;
    }

    if (component >= EKK_FIELD_COUNT) {
        return 0;
    }

    /* Gradient = neighbor_value - my_value
     * Positive: neighbors have higher values (I should increase)
     * Negative: neighbors have lower values (I should decrease)
     */
    return neighbor_aggregate->components[component] - my_field->components[component];
}

void ekk_field_gradient_all(const ekk_field_t *my_field,
                             const ekk_field_t *neighbor_aggregate,
                             ekk_fixed_t *gradients)
{
    if (my_field == NULL || neighbor_aggregate == NULL || gradients == NULL) {
        return;
    }

    for (int i = 0; i < EKK_FIELD_COUNT; i++) {
        gradients[i] = neighbor_aggregate->components[i] - my_field->components[i];
    }
}

/* ============================================================================
 * DECAY APPLICATION
 * ============================================================================ */

void ekk_field_apply_decay(ekk_field_t *field, ekk_time_us_t elapsed_us)
{
    if (field == NULL) {
        return;
    }

    /* Compute decay factor */
    ekk_fixed_t decay_factor = ekk_fixed_exp_decay(elapsed_us, g_decay_tau_us);

    /* Apply to all components */
    for (int i = 0; i < EKK_FIELD_COUNT; i++) {
        field->components[i] = ekk_fixed_mul(field->components[i], decay_factor);
    }
}

/* ============================================================================
 * GARBAGE COLLECTION
 * ============================================================================ */

uint32_t ekk_field_gc(ekk_time_us_t max_age_us)
{
    if (g_field_region == NULL) {
        return 0;
    }

    ekk_time_us_t now = ekk_hal_time_us();
    uint32_t expired_count = 0;

    for (int i = 0; i < EKK_MAX_MODULES; i++) {
        ekk_coord_field_t *cf = &g_field_region->fields[i];

        /* Read with seqlock protection to avoid torn reads */
        uint32_t seq1 = cf->sequence;
        ekk_hal_memory_barrier();

        if (seq1 & 1) {
            continue;  /* Write in progress, skip this slot */
        }

        ekk_module_id_t source = cf->field.source;
        ekk_time_us_t timestamp = cf->field.timestamp;

        ekk_hal_memory_barrier();
        if (cf->sequence != seq1) {
            continue;  /* Torn read, skip this slot */
        }

        if (source == EKK_INVALID_MODULE_ID) {
            continue;
        }

        ekk_time_us_t age = now - timestamp;
        if (age > max_age_us) {
            /* Re-check sequence before invalidating: a fresh publish
             * may have raced us since the consistency read above. */
            ekk_hal_memory_barrier();
            if (cf->sequence != seq1) {
                continue;  /* Fresh write raced us, skip */
            }

            /* Mark as invalid — uses seqlock write protocol */
            ekk_hal_atomic_inc(&cf->sequence);
            ekk_hal_memory_barrier();
            cf->field.source = EKK_INVALID_MODULE_ID;
            ekk_hal_memory_barrier();
            ekk_hal_atomic_inc(&cf->sequence);
            expired_count++;
        }
    }

    g_field_region->last_gc = now;
    return expired_count;
}

/* ============================================================================
 * FIELD ARITHMETIC
 * ============================================================================ */

void ekk_field_add(ekk_field_t *result, const ekk_field_t *a, const ekk_field_t *b)
{
    if (result == NULL || a == NULL || b == NULL) {
        return;
    }

    for (int i = 0; i < EKK_FIELD_COUNT; i++) {
        int64_t sum = (int64_t)a->components[i] + (int64_t)b->components[i];
        result->components[i] = (sum > INT32_MAX) ? INT32_MAX :
                                (sum < INT32_MIN) ? INT32_MIN :
                                (ekk_fixed_t)sum;
    }

    result->timestamp = EKK_MAX(a->timestamp, b->timestamp);
    result->source = a->source;
    result->sequence = a->sequence;
}

void ekk_field_scale(ekk_field_t *field, ekk_fixed_t factor)
{
    if (field == NULL) {
        return;
    }

    for (int i = 0; i < EKK_FIELD_COUNT; i++) {
        field->components[i] = ekk_fixed_mul(field->components[i], factor);
    }
}

void ekk_field_lerp(ekk_field_t *result, const ekk_field_t *a,
                    const ekk_field_t *b, ekk_fixed_t t)
{
    if (result == NULL || a == NULL || b == NULL) {
        return;
    }

    /* Clamp t to [0, 1] */
    if (t < 0) t = 0;
    if (t > EKK_FIXED_ONE) t = EKK_FIXED_ONE;

    ekk_fixed_t one_minus_t = EKK_FIXED_ONE - t;

    for (int i = 0; i < EKK_FIELD_COUNT; i++) {
        ekk_fixed_t va = ekk_fixed_mul(a->components[i], one_minus_t);
        ekk_fixed_t vb = ekk_fixed_mul(b->components[i], t);
        int64_t sum = (int64_t)va + (int64_t)vb;
        result->components[i] = (sum > (int64_t)INT32_MAX) ? INT32_MAX :
                                 (sum < (int64_t)INT32_MIN) ? INT32_MIN :
                                 (ekk_fixed_t)sum;
    }

    result->timestamp = EKK_MAX(a->timestamp, b->timestamp);
    result->source = a->source;
    result->sequence = a->sequence;
}

/* ============================================================================
 * ACCESSOR FOR TESTING
 * ============================================================================ */

ekk_field_region_t* ekk_field_get_region(void)
{
    return g_field_region;
}
