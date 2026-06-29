/**
 * @file ek3_rack.c
 * @brief EK3 rack coordination harness.
 *
 * Host-side integration example for Electrokombinacija EK3 design-intent
 * values. This is not charger firmware and not a CAN-FD wire-protocol adapter.
 *
 * Copyright (c) 2026 mamut-studio.com
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ekk/ekk.h"

#define EK3_DEMO_MODULES 7u
#define EK3_RACK_MODULES 84u
#define EK3_RACK_COLUMNS 4u
#define EK3_CONTINUOUS_POWER_W 3300u
#define EK3_TICK_INTERVAL_US 10000u
#define EK3_TOTAL_TICKS 80u
#define EK3_FAILURE_TICK 40u
#define EK3_FAILED_SLOT 3u

static ekk_module_t g_modules[EK3_RACK_MODULES];
static bool g_module_alive[EK3_RACK_MODULES];
static char g_module_names[EK3_RACK_MODULES][24];

static void print_usage(const char *program)
{
    fprintf(stderr, "usage: %s [--modules 7|84]\n", program);
}

static uint32_t parse_module_count(int argc, char **argv)
{
    if (argc == 1) {
        return EK3_DEMO_MODULES;
    }

    if (argc == 3 && strcmp(argv[1], "--modules") == 0) {
        char *end = NULL;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (end != NULL && *end == '\0' &&
            (value == EK3_DEMO_MODULES || value == EK3_RACK_MODULES)) {
            return (uint32_t)value;
        }
    }

    print_usage(argv[0]);
    return 0;
}

static ekk_position_t position_for_slot(uint32_t slot)
{
    ekk_position_t position = {
        .x = (int16_t)(slot % EK3_RACK_COLUMNS),
        .y = (int16_t)(slot / EK3_RACK_COLUMNS),
        .z = 0,
    };
    return position;
}

static ekk_fixed_t ratio_to_fixed(uint32_t value, uint32_t denominator)
{
    if (denominator == 0u || value >= denominator) {
        return EKK_FIXED_ONE;
    }

    return (ekk_fixed_t)(((uint64_t)value * (uint64_t)EKK_FIXED_ONE) /
                         (uint64_t)denominator);
}

static ekk_fixed_t temp_to_fixed(uint32_t temp_c)
{
    const uint32_t min_operating_c = 25u;
    const uint32_t max_operating_c = 125u;

    if (temp_c <= min_operating_c) {
        return 0;
    }

    if (temp_c >= max_operating_c) {
        return EKK_FIXED_ONE;
    }

    return ratio_to_fixed(temp_c - min_operating_c,
                          max_operating_c - min_operating_c);
}

static uint32_t simulated_power_w(uint32_t slot, uint64_t tick)
{
    uint32_t power_w = 1800u +
        (uint32_t)(((tick * 73u) + ((slot + 1u) * 97u)) % 1200u);

    if ((slot % 11u) == 0u) {
        power_w += 250u;
    }

    if (power_w > EK3_CONTINUOUS_POWER_W) {
        power_w = EK3_CONTINUOUS_POWER_W;
    }

    return power_w;
}

static uint32_t simulated_temp_c(uint32_t slot, uint32_t power_w, uint64_t tick)
{
    uint32_t temp_c = 34u +
        ((power_w * 56u) / EK3_CONTINUOUS_POWER_W) +
        (slot % 9u);

    if (tick > 25u && slot == 2u) {
        temp_c += 18u;
    }

    return temp_c;
}

static ekk_error_t init_module(uint32_t slot)
{
    ekk_module_id_t id = (ekk_module_id_t)(slot + 1u);
    ekk_position_t position = position_for_slot(slot);
    ekk_error_t err;

    snprintf(g_module_names[slot], sizeof(g_module_names[slot]), "ek3-%02u",
             (unsigned)id);

    err = ekk_module_init(&g_modules[slot], id, g_module_names[slot], position);
    if (err != EKK_OK) {
        return err;
    }

    ekk_capability_t caps = EKK_CAP_THERMAL_OK | EKK_CAP_POWER_HIGH;
    if (slot == 0u) {
        caps |= EKK_CAP_GATEWAY;
    }
    if ((slot % 12u) == 0u) {
        caps |= EKK_CAP_V2G;
    }

    err = ekk_module_set_capabilities(&g_modules[slot], caps);
    if (err != EKK_OK) {
        return err;
    }

    g_module_alive[slot] = true;
    return ekk_module_start(&g_modules[slot]);
}

static ekk_error_t direct_discovery(uint32_t count)
{
    for (uint32_t receiver = 0u; receiver < count; ++receiver) {
        if (!g_module_alive[receiver]) {
            continue;
        }

        for (uint32_t sender = 0u; sender < count; ++sender) {
            if (receiver == sender || !g_module_alive[sender]) {
                continue;
            }

            ekk_error_t err =
                ekk_topology_on_discovery(&g_modules[receiver].topology,
                                          g_modules[sender].id,
                                          g_modules[sender].topology.my_position);
            if (err != EKK_OK && err != EKK_ERR_LIMIT) {
                return err;
            }
        }

        (void)ekk_topology_reelect(&g_modules[receiver].topology);
    }

    return EKK_OK;
}

static void remove_failed_module(uint32_t count)
{
    g_module_alive[EK3_FAILED_SLOT] = false;
    (void)ekk_module_stop(&g_modules[EK3_FAILED_SLOT]);

    for (uint32_t slot = 0u; slot < count; ++slot) {
        if (!g_module_alive[slot]) {
            continue;
        }

        (void)ekk_topology_on_neighbor_lost(&g_modules[slot].topology,
                                            g_modules[EK3_FAILED_SLOT].id);
        (void)ekk_topology_reelect(&g_modules[slot].topology);
    }
}

static ekk_error_t tick_alive_modules(uint32_t count, ekk_time_us_t now_us)
{
    for (uint32_t slot = 0u; slot < count; ++slot) {
        if (!g_module_alive[slot]) {
            continue;
        }

        uint64_t tick = now_us / EK3_TICK_INTERVAL_US;
        uint32_t power_w = simulated_power_w(slot, tick);
        uint32_t temp_c = simulated_temp_c(slot, power_w, tick);

        ekk_error_t err =
            ekk_module_update_field(&g_modules[slot],
                                    ratio_to_fixed(power_w,
                                                   EK3_CONTINUOUS_POWER_W),
                                    temp_to_fixed(temp_c),
                                    ratio_to_fixed(power_w,
                                                   EK3_CONTINUOUS_POWER_W));
        if (err != EKK_OK) {
            return err;
        }

        err = ekk_module_tick(&g_modules[slot], now_us);
        if (err != EKK_OK && err != EKK_ERR_DEGRADED &&
            err != EKK_ERR_NOT_FOUND) {
            return err;
        }
    }

    return EKK_OK;
}

static uint8_t expected_neighbor_count(uint32_t alive_count)
{
    if (alive_count == 0u) {
        return 0u;
    }

    uint32_t expected = alive_count - 1u;
    if (expected > EKK_K_NEIGHBORS) {
        expected = EKK_K_NEIGHBORS;
    }

    return (uint8_t)expected;
}

static bool summarize(uint32_t count)
{
    uint32_t alive_count = 0u;
    uint8_t min_neighbors = EKK_K_NEIGHBORS;

    for (uint32_t slot = 0u; slot < count; ++slot) {
        if (!g_module_alive[slot]) {
            continue;
        }

        ++alive_count;

        uint32_t neighbors = g_modules[slot].topology.neighbor_count;
        if (neighbors < min_neighbors) {
            min_neighbors = (uint8_t)neighbors;
        }
    }

    uint8_t expected = expected_neighbor_count(alive_count);
    uint32_t capacity_w = alive_count * EK3_CONTINUOUS_POWER_W;

    printf("EK3_RACK_SUMMARY modules=%u alive=%u failed=%u neighbor_min=%u "
           "neighbor_expected=%u capacity_w=%u\n",
           (unsigned)count,
           (unsigned)alive_count,
           (unsigned)(count - alive_count),
           (unsigned)min_neighbors,
           (unsigned)expected,
           (unsigned)capacity_w);

    if (alive_count == count - 1u && min_neighbors == expected) {
        printf("EK3_RACK_OK modules=%u\n", (unsigned)count);
        return true;
    }

    return false;
}

int main(int argc, char **argv)
{
    uint32_t count = parse_module_count(argc, argv);
    if (count == 0u) {
        return 2;
    }

    if (EKK_MAX_MODULES <= EK3_RACK_MODULES) {
        fprintf(stderr,
                "ek3_rack requires EKK_MAX_MODULES greater than %u; got %u\n",
                (unsigned)EK3_RACK_MODULES,
                (unsigned)EKK_MAX_MODULES);
        return 2;
    }

    ekk_error_t err = ekk_init();
    if (err != EKK_OK) {
        fprintf(stderr, "ekk_init failed: %d\n", err);
        return 1;
    }
    ekk_hal_set_module_id(1);

    for (uint32_t slot = 0u; slot < count; ++slot) {
        err = init_module(slot);
        if (err != EKK_OK) {
            fprintf(stderr, "init_module(%u) failed: %d\n", (unsigned)slot, err);
            return 1;
        }
    }

    err = direct_discovery(count);
    if (err != EKK_OK) {
        fprintf(stderr, "direct_discovery failed: %d\n", err);
        return 1;
    }

    for (uint64_t tick = 1u; tick <= EK3_TOTAL_TICKS; ++tick) {
        ekk_time_us_t now_us = tick * EK3_TICK_INTERVAL_US;
        ekk_hal_set_mock_time(now_us);

        if (tick == EK3_FAILURE_TICK) {
            remove_failed_module(count);
            err = direct_discovery(count);
            if (err != EKK_OK) {
                fprintf(stderr, "rediscovery failed: %d\n", err);
                return 1;
            }
        }

        err = tick_alive_modules(count, now_us);
        if (err != EKK_OK) {
            fprintf(stderr, "tick failed at %" PRIu64 " us: %d\n",
                    now_us, err);
            return 1;
        }
    }

    bool ok = summarize(count);

    for (uint32_t slot = 0u; slot < count; ++slot) {
        if (g_module_alive[slot]) {
            (void)ekk_module_stop(&g_modules[slot]);
        }
    }

    ekk_hal_set_mock_time(0);
    return ok ? 0 : 1;
}
