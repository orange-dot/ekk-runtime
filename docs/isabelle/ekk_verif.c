/*
 * ekk_verif.c -- unified verification-only C file for AutoCorres / l4v C parser.
 *
 * Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * This file is NOT compiled as part of the normal build. It is processed
 * exclusively by the l4v C parser under the EKK_VERIFICATION flag.
 *
 * All proof targets are collected here to avoid duplicate constant declarations
 * when AutoCorres processes multiple C files sharing the same type headers.
 *
 * Proof targets (per VERIFICATION_PLAN.md):
 *   B1 -- ekk_heartbeat_init_verif   : toolchain validation (trivial postcondition)
 *   B2 -- evaluate_ballot_pure       : threshold voting correctness
 *   B3 -- set_neighbor_health_verif  : health-step state machine refinement
 *         health_transition_valid    : encodes valid transition edges
 */

#ifndef EKK_VERIFICATION
#define EKK_VERIFICATION
#endif

#include "ekk/ekk_types.h"
#include "ekk/ekk_heartbeat.h"
#include "ekk/ekk_consensus.h"

/* =========================================================================
 * B1 TARGET: ekk_heartbeat_init_verif
 *
 * Simplified init: validates arguments and zeros the struct via loop.
 * Replaces memset with explicit field assignments — l4v C parser does not
 * model memset in AutoCorres by default.
 *
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
 * Returns EKK_TRUE iff the transition from->to is a valid edge.
 * Encodes the state machine in pure C for the refinement proof.
 * ========================================================================= */

ekk_bool_t health_transition_valid(ekk_health_state_t from,
                                    ekk_health_state_t to)
{
    if (from == to) {
        return EKK_TRUE;
    }

    if (from == EKK_HEALTH_UNKNOWN  && to == EKK_HEALTH_ALIVE)   return EKK_TRUE;
    if (from == EKK_HEALTH_ALIVE    && to == EKK_HEALTH_SUSPECT) return EKK_TRUE;
    if (from == EKK_HEALTH_SUSPECT  && to == EKK_HEALTH_DEAD)    return EKK_TRUE;
    if (from == EKK_HEALTH_DEAD     && to == EKK_HEALTH_ALIVE)   return EKK_TRUE;
    if (from == EKK_HEALTH_SUSPECT  && to == EKK_HEALTH_ALIVE)   return EKK_TRUE;

    return EKK_FALSE;
}

/* =========================================================================
 * B2 TARGET: evaluate_ballot_pure
 *
 * Flat-argument version of the static evaluate_ballot in ekk_consensus.c.
 * All inputs are plain integers — no struct pointer, no heap access.
 *
 * Arguments:
 *   yes_votes       -- number of yes votes received
 *   total_votes     -- total votes received (yes + no + abstain)
 *   eligible_voters -- size of the electorate (denominator)
 *   threshold       -- Q16.16 fixed-point threshold (e.g. 43691 = 2/3)
 *
 * Proof obligations (see EkkConsensus.thy):
 *   1. APPROVED  iff yes_votes * 65536 >= eligible_voters * threshold
 *      when total_votes == eligible_voters
 *   2. REJECTED  early iff (yes_votes + remaining) * 65536 <
 *      eligible_voters * threshold  (threshold unreachable)
 *   3. PENDING   iff votes incomplete and threshold still reachable
 *   4. eligible_voters == 0  =>  REJECTED
 * ========================================================================= */

ekk_vote_result_t evaluate_ballot_pure(uint32_t yes_votes,
                                        uint32_t total_votes,
                                        uint32_t eligible_voters,
                                        ekk_fixed_t threshold)
{
    ekk_fixed_t yes_ratio;
    uint32_t remaining;
    uint32_t max_yes;
    ekk_fixed_t max_ratio;

    if (eligible_voters == 0) {
        return EKK_VOTE_REJECTED;
    }

    yes_ratio = (ekk_fixed_t)(((int64_t)yes_votes << 16) / eligible_voters);

    if (total_votes < eligible_voters) {
        remaining = eligible_voters - total_votes;
        max_yes   = yes_votes + remaining;
        max_ratio = (ekk_fixed_t)(((int64_t)max_yes << 16) / eligible_voters);

        if (max_ratio < threshold) {
            return EKK_VOTE_REJECTED;
        }

        if (yes_ratio >= threshold) {
            return EKK_VOTE_APPROVED;
        }

        return EKK_VOTE_PENDING;
    }

    if (yes_ratio >= threshold) {
        return EKK_VOTE_APPROVED;
    }

    return EKK_VOTE_REJECTED;
}
