/**
 * @file basic.c
 * @brief EK-KOR v2 Basic Example - Module Lifecycle Demo
 *
 * Demonstrates:
 * - ekk_init() / ekk_module_init()
 * - ekk_module_start() / tick() / stop()
 * - ekk_field_publish() / sample()
 * - Printf visualization of state
 *
 * Run with: ./example_basic
 *
 * Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>

#include "ekk/ekk.h"

/* Configuration */
#define TICK_INTERVAL_US    10000   /* 10ms tick rate */
#define TOTAL_TICKS         100     /* Run for 100 ticks = 1 second */
#define PRINT_INTERVAL      10      /* Print every 10 ticks */

/* Global module */
static ekk_module_t g_module;

/**
 * @brief Print module status
 */
static void print_status(uint32_t tick)
{
    printf("\n=== Tick %u ===\n", tick);
    printf("Module ID:    %u\n", g_module.id);
    printf("State:        %s\n", ekk_module_state_str(g_module.state));
    printf("Field - Load: %.3f, Thermal: %.3f, Power: %.3f\n",
           EKK_FIXED_TO_FLOAT(g_module.my_field.components[EKK_FIELD_LOAD]),
           EKK_FIXED_TO_FLOAT(g_module.my_field.components[EKK_FIELD_THERMAL]),
           EKK_FIXED_TO_FLOAT(g_module.my_field.components[EKK_FIELD_POWER]));
    printf("Gradients - Load: %.3f, Thermal: %.3f, Power: %.3f\n",
           EKK_FIXED_TO_FLOAT(g_module.gradients[EKK_FIELD_LOAD]),
           EKK_FIXED_TO_FLOAT(g_module.gradients[EKK_FIELD_THERMAL]),
           EKK_FIXED_TO_FLOAT(g_module.gradients[EKK_FIELD_POWER]));
    printf("Ticks: %u, Field publishes: %u\n",
           g_module.ticks_total, g_module.field_publish_ok);
}

/**
 * @brief Simulate changing load
 */
static void simulate_activity(uint32_t tick)
{
    /* Simulate sinusoidal load (0.2 - 0.8) */
    float load_f = 0.5f + 0.3f * ((tick % 50) < 25 ?
                   (float)(tick % 25) / 25.0f :
                   1.0f - (float)(tick % 25) / 25.0f);

    /* Thermal increases gradually */
    float thermal_f = 0.1f + 0.005f * (float)tick;
    if (thermal_f > 0.9f) thermal_f = 0.9f;

    /* Power correlates with load */
    float power_f = 0.3f + 0.5f * load_f;

    /* Update module field */
    ekk_module_update_field(&g_module,
                            EKK_FLOAT_TO_FIXED(load_f),
                            EKK_FLOAT_TO_FIXED(thermal_f),
                            EKK_FLOAT_TO_FIXED(power_f));
}

int main(void)
{
    ekk_error_t err;

    printf("\n");
    printf("*********************************************\n");
    printf("*  EK-KOR v2: Basic Example                 *\n");
    printf("*  Module Lifecycle Demo                    *\n");
    printf("*  Copyright (c) 2026 Elektrokombinacija    *\n");
    printf("*********************************************\n");
    printf("\n");

    /* 1. Initialize EK-KOR system */
    printf("[1] Initializing EK-KOR system...\n");
    err = ekk_init();
    if (err != EKK_OK) {
        fprintf(stderr, "ERROR: ekk_init() failed: %d\n", err);
        return 1;
    }
    ekk_hal_set_module_id(1);
    printf("    System initialized (version %s)\n", EKK_VERSION_STRING);

    /* 2. Initialize module */
    printf("[2] Initializing module...\n");
    ekk_position_t pos = {.x = 0, .y = 0, .z = 0};
    err = ekk_module_init(&g_module, 1, "BASIC_DEMO", pos);
    if (err != EKK_OK) {
        fprintf(stderr, "ERROR: ekk_module_init() failed: %d\n", err);
        return 1;
    }
    printf("    Module %u initialized (k=%u neighbors)\n",
           g_module.id, EKK_K_NEIGHBORS);
    printf("    State: %s\n", ekk_module_state_str(g_module.state));

    /* 3. Start module */
    printf("[3] Starting module...\n");
    err = ekk_module_start(&g_module);
    if (err != EKK_OK) {
        fprintf(stderr, "ERROR: ekk_module_start() failed: %d\n", err);
        return 1;
    }
    printf("    State: %s\n", ekk_module_state_str(g_module.state));

    /* 4. Main tick loop */
    printf("[4] Running tick loop (%u ticks)...\n", TOTAL_TICKS);

    ekk_time_us_t now = 1000;

    for (uint32_t tick = 0; tick < TOTAL_TICKS; tick++) {
        /* Advance time */
        now += TICK_INTERVAL_US;
        ekk_hal_set_mock_time(now);

        /* Run module tick */
        err = ekk_module_tick(&g_module, now);
        if (err != EKK_OK && err != EKK_ERR_NOT_FOUND) {
            printf("    WARN: tick %u returned %d\n", tick, err);
        }

        /* Simulate activity */
        simulate_activity(tick);

        /* Print status periodically */
        if ((tick % PRINT_INTERVAL) == 0 || tick == TOTAL_TICKS - 1) {
            print_status(tick);
        }
    }

    /* 5. Stop module */
    printf("\n[5] Stopping module...\n");
    err = ekk_module_stop(&g_module);
    if (err != EKK_OK) {
        fprintf(stderr, "ERROR: ekk_module_stop() failed: %d\n", err);
        return 1;
    }
    printf("    State: %s\n", ekk_module_state_str(g_module.state));

    /* Summary */
    printf("\n");
    printf("*********************************************\n");
    printf("*  EXAMPLE COMPLETE                         *\n");
    printf("*  Total ticks: %u                         \n", g_module.ticks_total);
    printf("*  Field publishes: %u                     \n", g_module.field_publish_ok);
    printf("*********************************************\n");

    ekk_hal_set_mock_time(0);
    return 0;
}
