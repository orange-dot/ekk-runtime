/*
 * ekk_heartbeat_verif.c -- verification-only C file for AutoCorres / l4v C parser.
 *
 * Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * This file is NOT compiled as part of the normal build. It is processed
 * exclusively by the l4v C parser under the EKK_VERIFICATION flag.
 *
 * Design choices vs production src/ekk_heartbeat.c:
 *
 *   1. No memset. ekk_heartbeat_init_verif uses an explicit loop to zero
 *      the neighbors array. The production code uses memset; this is
 *      semantically equivalent for the init postcondition proof.
 *
 *   2. No callbacks. set_neighbor_health_verif does not invoke function
 *      pointers. The health-transition proof only needs the state update.
 *
 *   3. No ekk_hal_send. ekk_heartbeat_send is out of scope.
 *
 * The functions here are renamed with _verif suffix to avoid name clashes
 * if other C files are ever added to the session.
 *
 * Proof targets (per VERIFICATION_PLAN.md):
 *   B1 -- ekk_heartbeat_init_verif: toolchain validation (trivial postcondition)
 *   B3 -- set_neighbor_health_verif: health-step state machine refinement
 */

#ifndef EKK_VERIFICATION
#define EKK_VERIFICATION
#endif

#include "ekk/ekk_types.h"
#include "ekk/ekk_heartbeat.h"

/* =========================================================================
 * B1 TARGET: ekk_heartbeat_init_verif
 *
 * Simplified init: validates arguments and zeros the struct via loop.
 * Proof obligation: if hb != NULL and my_id != EKK_INVALID_MODULE_ID,
 * then result == EKK_OK.
 * ========================================================================= */

ekk_error_t ekk_heartbeat_init_verif(ekk_heartbeat_t *hb,
                                      ekk_module_id_t my_id)
{
    uint32_t i;

    if (hb == NULL || my_id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Zero neighbors array (replaces memset for l4v C parser) */
    for (i = 0; i < EKK_MAX_MODULES; i++) {
        hb->neighbors[i].id           = EKK_INVALID_MODULE_ID;
        hb->neighbors[i].health       = EKK_HEALTH_UNKNOWN;
        hb->neighbors[i].last_seen    = 0;
        hb->neighbors[i].missed_count = 0;
        hb->neighbors[i].sequence     = 0;
        hb->neighbors[i].avg_heartbeat_gap_us = 0;
    }

    hb->my_id          = my_id;
    hb->neighbor_count = 0;
    hb->last_send      = 0;
    hb->send_sequence  = 0;

    return EKK_OK;
}

/* =========================================================================
 * B3 TARGET: set_neighbor_health_verif
 *
 * Pure state transition: updates health field only, no callbacks.
 * Proof obligation: health field equals new_state after call, and the
 * transition is a valid edge in the UNKNOWN->ALIVE->SUSPECT->DEAD machine.
 * ========================================================================= */

void set_neighbor_health_verif(ekk_heartbeat_neighbor_t *neighbor,
                                ekk_health_state_t new_state)
{
    if (neighbor == NULL) {
        return;
    }
    neighbor->health = new_state;
}

/* =========================================================================
 * B3 HELPER: health_transition_valid
 *
 * Returns 1 (EKK_TRUE) iff the transition from->to is a valid edge.
 * This encodes the state machine in pure C for the refinement proof.
 * ========================================================================= */

ekk_bool_t health_transition_valid(ekk_health_state_t from,
                                    ekk_health_state_t to)
{
    /* Self-transitions are always valid */
    if (from == to) {
        return EKK_TRUE;
    }

    /* Allowed forward transitions */
    if (from == EKK_HEALTH_UNKNOWN  && to == EKK_HEALTH_ALIVE)   return EKK_TRUE;
    if (from == EKK_HEALTH_ALIVE    && to == EKK_HEALTH_SUSPECT) return EKK_TRUE;
    if (from == EKK_HEALTH_SUSPECT  && to == EKK_HEALTH_DEAD)    return EKK_TRUE;

    /* Re-discovery: DEAD or SUSPECT back to ALIVE */
    if (from == EKK_HEALTH_DEAD     && to == EKK_HEALTH_ALIVE)   return EKK_TRUE;
    if (from == EKK_HEALTH_SUSPECT  && to == EKK_HEALTH_ALIVE)   return EKK_TRUE;

    return EKK_FALSE;
}
