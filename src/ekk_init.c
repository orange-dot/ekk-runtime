/**
 * @file ekk_init.c
 * @brief EK-KOR v2 - System Initialization
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * Initializes the EK-KOR v2 system:
 * - HAL (platform-specific hardware)
 * - Global field region (shared memory for coordination fields)
 */

#include "ekk/ekk.h"
#include <string.h>

/* ============================================================================
 * GLOBAL STATE
 * ============================================================================ */

/** Initialization flag */
static ekk_bool_t g_initialized = EKK_FALSE;

/* ============================================================================
 * SYSTEM INITIALIZATION
 * ============================================================================ */

ekk_error_t ekk_init(void)
{
    if (g_initialized) {
        return EKK_OK;  /* Already initialized */
    }

    /* Initialize HAL first */
    ekk_error_t err = ekk_hal_init();
    if (err != EKK_OK) {
        return err;
    }

    /* Initialize shared field region provided by the HAL */
    err = ekk_field_init((ekk_field_region_t *)ekk_hal_get_field_region());
    if (err != EKK_OK) {
        return err;
    }

    g_initialized = EKK_TRUE;

    /* Print startup banner */
    ekk_hal_printf("EK-KOR v%s initialized\n", EKK_VERSION_STRING);
    ekk_hal_printf("  Platform: %s\n", ekk_hal_platform_name());
    ekk_hal_printf("  k-neighbors: %d\n", EKK_K_NEIGHBORS);
    ekk_hal_printf("  Max modules: %d\n", EKK_MAX_MODULES);

    return EKK_OK;
}

/* ============================================================================
 * FIELD REGION ACCESS
 * ============================================================================ */

ekk_field_region_t* ekk_get_field_region(void)
{
    return (ekk_field_region_t *)ekk_hal_get_field_region();
}
