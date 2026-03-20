/*
 * ekk_consensus_verif.c -- verification-only C file for AutoCorres / l4v C parser.
 *
 * Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * This file is NOT compiled as part of the normal build. It is processed
 * exclusively by the l4v C parser under the EKK_VERIFICATION flag.
 *
 * Design choices vs production src/ekk_consensus.c:
 *
 *   1. evaluate_ballot_pure takes flat arguments, not a struct pointer.
 *      This avoids the heap/pointer model for the B2 proof target and
 *      makes the refinement to the HOL spec more direct.
 *
 *   2. No callbacks, no global state, no HAL calls.
 *
 * Proof targets (per VERIFICATION_PLAN.md):
 *   B2 -- evaluate_ballot_pure: correctness of threshold voting logic
 *         (APPROVED iff yes_ratio >= threshold with all votes in, or
 *          impossible for remaining votes to reach threshold -> REJECTED,
 *          otherwise PENDING)
 */

#ifndef EKK_VERIFICATION
#define EKK_VERIFICATION
#endif

#include "ekk/ekk_types.h"
#include "ekk/ekk_consensus.h"

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
 * Returns: EKK_VOTE_APPROVED, EKK_VOTE_REJECTED, or EKK_VOTE_PENDING.
 *
 * Proof obligations (see EkkConsensus.thy):
 *   1. APPROVED  iff yes_votes * 65536 >= eligible_voters * threshold
 *      when total_votes == eligible_voters
 *   2. REJECTED  early iff (yes_votes + remaining) * 65536 <
 *      eligible_voters * threshold  (threshold unreachable)
 *   3. PENDING   iff votes incomplete and threshold still reachable but
 *      not yet reached
 *   4. eligible_voters == 0  =>  REJECTED  (degenerate case)
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

    /* Ratio of yes votes to electorate size (Q16.16) */
    yes_ratio = (ekk_fixed_t)(((int64_t)yes_votes << 16) / eligible_voters);

    if (total_votes < eligible_voters) {
        /* Votes still arriving — check early termination conditions */
        remaining = eligible_voters - total_votes;
        max_yes   = yes_votes + remaining;
        max_ratio = (ekk_fixed_t)(((int64_t)max_yes << 16) / eligible_voters);

        /* Early rejection: even if all remaining vote yes, threshold unreachable */
        if (max_ratio < threshold) {
            return EKK_VOTE_REJECTED;
        }

        /* Early approval: threshold already met */
        if (yes_ratio >= threshold) {
            return EKK_VOTE_APPROVED;
        }

        return EKK_VOTE_PENDING;
    }

    /* All votes received */
    if (yes_ratio >= threshold) {
        return EKK_VOTE_APPROVED;
    }

    return EKK_VOTE_REJECTED;
}
