/**
 * @file multi_module.c
 * @brief EK-KOR v2 Multi-Module Example - 7 Modules Simulation
 *
 * Demonstrates:
 * - 7 modules in a single process (k=7 topology)
 * - Topology discovery (all modules "see" each other)
 * - Heartbeat exchange via field region
 * - Field propagation and gradient sampling
 * - One module "dies" - mesh reformation
 *
 * Run with: ./example_multi_module
 *
 * Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ekk/ekk.h"

/* Configuration */
#define NUM_MODULES         7       /* k=7 neighbors */
#define TICK_INTERVAL_US    10000   /* 10ms tick rate */
#define TOTAL_TICKS         200     /* Run for 200 ticks = 2 seconds */
#define PRINT_INTERVAL      50      /* Print every 50 ticks */
#define FAILURE_TICK        100     /* Module 4 "dies" at tick 100 */

/* Module states */
static ekk_module_t g_modules[NUM_MODULES];
static bool g_module_alive[NUM_MODULES];
static char g_module_names[NUM_MODULES][16];

/**
 * @brief State name helper
 */
static const char* state_short(ekk_module_state_t state)
{
    switch (state) {
        case EKK_MODULE_INIT:        return "INIT";
        case EKK_MODULE_DISCOVERING: return "DISC";
        case EKK_MODULE_ACTIVE:      return "ACTV";
        case EKK_MODULE_DEGRADED:    return "DEGR";
        case EKK_MODULE_ISOLATED:    return "ISOL";
        case EKK_MODULE_REFORMING:   return "REFM";
        case EKK_MODULE_SHUTDOWN:    return "SHUT";
        default:                     return "????";
    }
}

/**
 * @brief Print status of all modules
 */
static void print_all_status(uint32_t tick)
{
    printf("\n=== Tick %u ===\n", tick);
    printf("%-4s %-6s %-4s %-5s %-5s %-5s %-6s\n",
           "ID", "State", "Neig", "Load", "Therm", "Power", "Ticks");
    printf("---- ------ ---- ----- ----- ----- ------\n");

    for (int i = 0; i < NUM_MODULES; i++) {
        if (!g_module_alive[i]) {
            printf("%-4u %-6s (DEAD)\n", g_modules[i].id, "DEAD");
            continue;
        }

        ekk_module_t *m = &g_modules[i];
        printf("%-4u %-6s %-4u %5.2f %5.2f %5.2f %-6u\n",
               m->id,
               state_short(m->state),
               m->topology.neighbor_count,
               EKK_FIXED_TO_FLOAT(m->my_field.components[EKK_FIELD_LOAD]),
               EKK_FIXED_TO_FLOAT(m->my_field.components[EKK_FIELD_THERMAL]),
               EKK_FIXED_TO_FLOAT(m->my_field.components[EKK_FIELD_POWER]),
               m->ticks_total);
    }
}

/**
 * @brief Simulate discovery message from one module to another
 *
 * In real hardware this would be via CAN bus.
 * Here we directly call the topology layer.
 */
static void simulate_discovery(ekk_module_t *sender, ekk_module_t *receiver)
{
    if (!g_module_alive[sender->id - 1] || !g_module_alive[receiver->id - 1]) {
        return;
    }

    /* Simulate discovery message reception */
    ekk_topology_on_discovery(&receiver->topology,
                              sender->id,
                              sender->topology.my_position);
}

/**
 * @brief Simulate full mesh discovery (all modules discover each other)
 */
static void simulate_full_discovery(void)
{
    for (int i = 0; i < NUM_MODULES; i++) {
        for (int j = 0; j < NUM_MODULES; j++) {
            if (i != j) {
                simulate_discovery(&g_modules[i], &g_modules[j]);
            }
        }
    }

    /* Trigger reelection for all modules */
    for (int i = 0; i < NUM_MODULES; i++) {
        if (g_module_alive[i]) {
            ekk_topology_reelect(&g_modules[i].topology);
        }
    }
}

/**
 * @brief Simulate heartbeat exchange between all alive modules
 */
static void simulate_heartbeat_exchange(ekk_time_us_t now)
{
    EKK_UNUSED(now);
    for (int i = 0; i < NUM_MODULES; i++) {
        if (!g_module_alive[i]) continue;

        ekk_module_t *sender = &g_modules[i];

        /* Publish field to shared region */
        ekk_field_publish(sender->id, &sender->my_field);
    }
}

/**
 * @brief Simulate activity for a module
 */
static void simulate_activity(ekk_module_t *m, uint32_t tick)
{
    /* Each module has slightly different load patterns */
    float phase_offset = (float)m->id * 0.2f;
    float load_f = 0.3f + 0.4f * ((float)((tick + (int)(phase_offset * 50)) % 50) / 50.0f);

    /* Thermal increases with ID (higher IDs = hotter) */
    float thermal_f = 0.2f + 0.05f * (float)m->id + 0.002f * (float)tick;
    if (thermal_f > 0.95f) thermal_f = 0.95f;

    /* Power correlates with load */
    float power_f = 0.2f + 0.6f * load_f;

    /* Update field */
    ekk_module_update_field(m,
                            EKK_FLOAT_TO_FIXED(load_f),
                            EKK_FLOAT_TO_FIXED(thermal_f),
                            EKK_FLOAT_TO_FIXED(power_f));
}

/**
 * @brief Kill a module (simulate failure)
 */
static void kill_module(int idx)
{
    printf("\n!!! Module %u FAILED !!!\n", g_modules[idx].id);
    g_module_alive[idx] = false;

    /* Notify other modules of the loss */
    for (int i = 0; i < NUM_MODULES; i++) {
        if (i != idx && g_module_alive[i]) {
            ekk_topology_on_neighbor_lost(&g_modules[i].topology,
                                          g_modules[idx].id);
        }
    }
}

int main(void)
{
    ekk_error_t err;

    printf("\n");
    printf("*********************************************\n");
    printf("*  EK-KOR v2: Multi-Module Example          *\n");
    printf("*  7 Modules Topology Simulation            *\n");
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
    printf("    System initialized (k=%u neighbors)\n", EKK_K_NEIGHBORS);

    /* 2. Initialize all modules */
    printf("[2] Initializing %d modules...\n", NUM_MODULES);
    for (int i = 0; i < NUM_MODULES; i++) {
        /* Create unique name */
        snprintf(g_module_names[i], sizeof(g_module_names[i]), "MODULE_%d", i + 1);

        /* Position in a grid: row = i/3, col = i%3 */
        ekk_position_t pos = {
            .x = (int16_t)(i % 3),
            .y = (int16_t)(i / 3),
            .z = 0
        };

        /* Initialize module (IDs 1-7) */
        err = ekk_module_init(&g_modules[i], (ekk_module_id_t)(i + 1),
                              g_module_names[i], pos);
        if (err != EKK_OK) {
            fprintf(stderr, "ERROR: ekk_module_init(%d) failed: %d\n", i + 1, err);
            return 1;
        }

        g_module_alive[i] = true;
        printf("    Module %u initialized at (%d, %d)\n",
               g_modules[i].id, pos.x, pos.y);
    }

    /* 3. Start all modules */
    printf("[3] Starting all modules...\n");
    for (int i = 0; i < NUM_MODULES; i++) {
        err = ekk_module_start(&g_modules[i]);
        if (err != EKK_OK) {
            fprintf(stderr, "ERROR: ekk_module_start(%d) failed: %d\n", i + 1, err);
            return 1;
        }
    }
    printf("    All modules in DISCOVERING state\n");

    /* 4. Simulate discovery - all modules discover each other */
    printf("[4] Simulating mesh discovery...\n");
    simulate_full_discovery();
    printf("    Full mesh established\n");

    /* 5. Main tick loop */
    printf("[5] Running tick loop (%u ticks)...\n", TOTAL_TICKS);
    printf("    Module 4 will fail at tick %u\n", FAILURE_TICK);

    ekk_time_us_t now = 1000;

    for (uint32_t tick = 0; tick < TOTAL_TICKS; tick++) {
        /* Advance time */
        now += TICK_INTERVAL_US;
        ekk_hal_set_mock_time(now);

        /* Simulate module failure at specific tick */
        if (tick == FAILURE_TICK) {
            kill_module(3);  /* Module 4 (index 3) dies */
        }

        /* Tick all alive modules */
        for (int i = 0; i < NUM_MODULES; i++) {
            if (!g_module_alive[i]) continue;

            /* Run tick */
            ekk_hal_set_module_id(g_modules[i].id);
            ekk_module_tick(&g_modules[i], now);

            /* Simulate activity */
            simulate_activity(&g_modules[i], tick);
        }

        /* Simulate heartbeat/field exchange */
        simulate_heartbeat_exchange(now);

        /* Periodic discovery (re-election) every 20 ticks after failure */
        if (tick > FAILURE_TICK && (tick - FAILURE_TICK) % 20 == 0) {
            for (int i = 0; i < NUM_MODULES; i++) {
                if (g_module_alive[i]) {
                    ekk_topology_reelect(&g_modules[i].topology);
                }
            }
        }

        /* Print status periodically */
        if ((tick % PRINT_INTERVAL) == 0 || tick == FAILURE_TICK ||
            tick == FAILURE_TICK + 1 || tick == TOTAL_TICKS - 1) {
            print_all_status(tick);
        }
    }

    /* 6. Stop all modules */
    printf("\n[6] Stopping all modules...\n");
    for (int i = 0; i < NUM_MODULES; i++) {
        if (g_module_alive[i]) {
            ekk_module_stop(&g_modules[i]);
        }
    }

    /* Summary */
    printf("\n");
    printf("*********************************************\n");
    printf("*  EXAMPLE COMPLETE                         *\n");
    printf("*  Demonstrated:                            *\n");
    printf("*  - 7 modules topology discovery           *\n");
    printf("*  - Field publishing and sampling          *\n");
    printf("*  - Module failure (module 4)              *\n");
    printf("*  - Mesh reformation after failure         *\n");
    printf("*********************************************\n");

    ekk_hal_set_mock_time(0);
    return 0;
}
