/**
 * @file ekk_consensus.c
 * @brief EK-KOR v2 - Threshold-Based Distributed Consensus Implementation
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * NOVELTY: Threshold Consensus with Mutual Inhibition
 * - Density-dependent threshold voting
 * - Supermajority support for safety-critical decisions
 * - Mutual inhibition for competing proposals
 */

#include "ekk/ekk_consensus.h"
#include "ekk/ekk_hal.h"
#include <string.h>

/* ============================================================================
 * PRIVATE HELPERS
 * ============================================================================ */

static ekk_ballot_key_t make_ballot_key(ekk_module_id_t proposer_id,
                                        ekk_ballot_id_t ballot_id)
{
    ekk_ballot_key_t key = {
        .proposer_id = proposer_id,
        .ballot_id = ballot_id,
    };
    return key;
}

static ekk_ballot_key_t ballot_key_from_ballot(const ekk_ballot_t *ballot)
{
    return make_ballot_key(ballot->proposer, ballot->id);
}

static ekk_bool_t ballot_key_equals(ekk_ballot_key_t a, ekk_ballot_key_t b)
{
    return (a.proposer_id == b.proposer_id && a.ballot_id == b.ballot_id)
        ? EKK_TRUE
        : EKK_FALSE;
}

/**
 * @brief Find ballot by canonical key
 * @return Index if found, -1 otherwise
 */
static int find_ballot_index(const ekk_consensus_t *cons,
                             ekk_module_id_t proposer_id,
                             ekk_ballot_id_t ballot_id)
{
    ekk_ballot_key_t needle = make_ballot_key(proposer_id, ballot_id);

    for (uint32_t i = 0; i < cons->active_ballot_count; i++) {
        if (ballot_key_equals(ballot_key_from_ballot(&cons->ballots[i]), needle)) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Find a voter slot within a ballot snapshot
 * @return Index if found, -1 otherwise
 */
static int find_ballot_voter_slot(const ekk_ballot_t *ballot, ekk_module_id_t voter_id)
{
    for (uint32_t i = 0; i < ballot->eligible_voter_count; i++) {
        if (ballot->eligible_voters[i] == voter_id) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Append voter ID if valid and not already present
 */
static void append_unique_voter(ekk_module_id_t *voters,
                                uint8_t *voter_count,
                                ekk_module_id_t voter_id,
                                uint32_t max_voters)
{
    if (voters == NULL || voter_count == NULL ||
        voter_id == EKK_INVALID_MODULE_ID || *voter_count >= max_voters) {
        return;
    }

    for (uint32_t i = 0; i < *voter_count; i++) {
        if (voters[i] == voter_id) {
            return;
        }
    }

    voters[*voter_count] = voter_id;
    (*voter_count)++;
}

/**
 * @brief Reset ballot vote bookkeeping
 */
static void reset_ballot_votes(ekk_ballot_t *ballot)
{
    memset(ballot->votes, EKK_VOTE_ABSTAIN, sizeof(ballot->votes));
    ballot->vote_count = 0;
    ballot->yes_count = 0;
    ballot->no_count = 0;
}

/**
 * @brief Snapshot the current electorate into a ballot
 */
static void snapshot_ballot_electorate(const ekk_consensus_t *cons, ekk_ballot_t *ballot)
{
    ballot->eligible_voter_count = 0;
    memset(ballot->eligible_voters, EKK_INVALID_MODULE_ID, sizeof(ballot->eligible_voters));
    reset_ballot_votes(ballot);

    for (uint32_t i = 0; i < cons->electorate_count; i++) {
        append_unique_voter(ballot->eligible_voters,
                            &ballot->eligible_voter_count,
                            cons->electorate[i],
                            EKK_ARRAY_SIZE(ballot->eligible_voters));
    }

    if (cons->config.allow_self_vote) {
        append_unique_voter(ballot->eligible_voters,
                            &ballot->eligible_voter_count,
                            cons->my_id,
                            EKK_ARRAY_SIZE(ballot->eligible_voters));

        int self_slot = find_ballot_voter_slot(ballot, cons->my_id);
        if (self_slot >= 0) {
            ballot->votes[self_slot] = EKK_VOTE_YES;
            ballot->vote_count = 1;
            ballot->yes_count = 1;
        }
    }
}

/**
 * @brief Check if a ballot is inhibited
 */
static ekk_bool_t is_inhibited(const ekk_consensus_t *cons,
                               ekk_module_id_t proposer_id,
                               ekk_ballot_id_t ballot_id,
                               ekk_time_us_t now)
{
    ekk_ballot_key_t needle = make_ballot_key(proposer_id, ballot_id);

    for (uint32_t i = 0; i < cons->inhibit_count; i++) {
        if (ballot_key_equals(cons->inhibited[i], needle) &&
            cons->inhibit_until[i] > now) {
            return EKK_TRUE;
        }
    }
    return EKK_FALSE;
}

static void finalize_ballot(ekk_consensus_t *cons, ekk_ballot_t *ballot,
                             ekk_vote_result_t result);

static void record_inhibition(ekk_consensus_t *cons,
                              ekk_module_id_t proposer_id,
                              ekk_ballot_id_t ballot_id,
                              ekk_time_us_t now)
{
    ekk_ballot_key_t key = make_ballot_key(proposer_id, ballot_id);

    for (uint32_t i = 0; i < cons->inhibit_count; i++) {
        if (ballot_key_equals(cons->inhibited[i], key)) {
            cons->inhibit_until[i] = now + cons->config.inhibit_duration;
            goto cancel_local_ballot;
        }
    }

    if (cons->inhibit_count >= EKK_MAX_BALLOTS) {
        for (uint32_t i = 0; i < cons->inhibit_count - 1; i++) {
            cons->inhibited[i] = cons->inhibited[i + 1];
            cons->inhibit_until[i] = cons->inhibit_until[i + 1];
        }
        cons->inhibit_count--;
    }

    cons->inhibited[cons->inhibit_count] = key;
    cons->inhibit_until[cons->inhibit_count] = now + cons->config.inhibit_duration;
    cons->inhibit_count++;

cancel_local_ballot:
    {
        int idx = find_ballot_index(cons, proposer_id, ballot_id);
        if (idx >= 0 && !cons->ballots[idx].completed) {
            finalize_ballot(cons, &cons->ballots[idx], EKK_VOTE_CANCELLED);
        }
    }
}

/**
 * @brief Allocate a free ballot slot
 * @return Index of free slot, -1 if none available
 */
static int allocate_ballot_slot(ekk_consensus_t *cons)
{
    if (cons->active_ballot_count >= EKK_MAX_BALLOTS) {
        return -1;
    }

    return (int)cons->active_ballot_count;
}

/**
 * @brief Evaluate ballot result based on votes and threshold
 */
static ekk_vote_result_t evaluate_ballot(const ekk_ballot_t *ballot)
{
    if (ballot->completed) {
        return ballot->result;
    }

    uint32_t eligible_voters = ballot->eligible_voter_count;
    uint32_t total_votes = ballot->vote_count;
    uint32_t yes_votes = ballot->yes_count;

    if (eligible_voters == 0) {
        return EKK_VOTE_REJECTED;
    }

    /* Ratio of yes votes seen so far */
    ekk_fixed_t yes_ratio =
        (ekk_fixed_t)(((int64_t)yes_votes << 16) / eligible_voters);

    /* If not all votes received, still pending */
    if (total_votes < eligible_voters) {
        /* Best case: all remaining vote yes — check if threshold is still reachable */
        uint32_t remaining = eligible_voters - total_votes;
        uint32_t max_yes = yes_votes + remaining;
        ekk_fixed_t max_ratio =
            (ekk_fixed_t)(((int64_t)max_yes << 16) / eligible_voters);

        if (max_ratio < ballot->threshold) {
            return EKK_VOTE_REJECTED;
        }

        /* Check if threshold already reached with votes so far */
        if (yes_ratio >= ballot->threshold) {
            return EKK_VOTE_APPROVED;
        }

        return EKK_VOTE_PENDING;
    }

    /* All votes received */
    if (yes_ratio >= ballot->threshold) {
        return EKK_VOTE_APPROVED;
    } else {
        return EKK_VOTE_REJECTED;
    }
}

/**
 * @brief Finalize a ballot with a result
 */
static void finalize_ballot(ekk_consensus_t *cons, ekk_ballot_t *ballot,
                             ekk_vote_result_t result)
{
    ballot->result = result;
    ballot->completed = EKK_TRUE;

    /* Invoke completion callback */
    if (cons->complete_callback != NULL) {
        cons->complete_callback(cons, cons->complete_context, ballot, result);
    }
}

/**
 * @brief Remove completed ballots from active list
 */
static void cleanup_completed_ballots(ekk_consensus_t *cons)
{
    uint32_t write_idx = 0;

    for (uint32_t read_idx = 0; read_idx < cons->active_ballot_count; read_idx++) {
        if (!cons->ballots[read_idx].completed) {
            if (write_idx != read_idx) {
                cons->ballots[write_idx] = cons->ballots[read_idx];
            }
            write_idx++;
        }
    }

    cons->active_ballot_count = write_idx;
}

/**
 * @brief Broadcast proposal to neighbors
 */
static ekk_error_t broadcast_proposal(const ekk_consensus_t *cons,
                                       const ekk_ballot_t *ballot)
{
    EKK_UNUSED(cons);

    ekk_proposal_msg_t msg = {
        .msg_type = EKK_MSG_PROPOSAL,
        .proposer_id = ballot->proposer,
        .ballot_id = ballot->id,
        .type = ballot->type,
        .data = ballot->proposal_data,
        .threshold = ballot->threshold,
    };

    return ekk_hal_broadcast(EKK_MSG_PROPOSAL, &msg, sizeof(msg));
}

/**
 * @brief Send vote to proposer
 */
static ekk_error_t send_vote(const ekk_consensus_t *cons,
                              ekk_module_id_t proposer_id,
                              ekk_ballot_id_t ballot_id,
                              ekk_vote_value_t vote)
{
    ekk_vote_msg_t msg = {
        .msg_type = EKK_MSG_VOTE,
        .voter_id = cons->my_id,
        .proposer_id = proposer_id,
        .ballot_id = ballot_id,
        .vote = vote,
        .timestamp = (uint32_t)(ekk_hal_time_us() & 0xFFFFFFFF),
    };

    return ekk_hal_send(proposer_id, EKK_MSG_VOTE, &msg, sizeof(msg));
}

/**
 * @brief Broadcast inhibition for a specific ballot key
 */
static ekk_error_t send_inhibit(const ekk_consensus_t *cons,
                                 ekk_module_id_t proposer_id,
                                 ekk_ballot_id_t ballot_id)
{
    EKK_UNUSED(cons);

    ekk_inhibit_msg_t msg = {
        .msg_type = EKK_MSG_INHIBIT,
        .proposer_id = proposer_id,
        .ballot_id = ballot_id,
        .timestamp = (uint32_t)(ekk_hal_time_us() & 0xFFFFFFFF),
    };

    return ekk_hal_broadcast(EKK_MSG_INHIBIT, &msg, sizeof(msg));
}

/* ============================================================================
 * INITIALIZATION
 * ============================================================================ */

ekk_error_t ekk_consensus_init(ekk_consensus_t *cons,
                                ekk_module_id_t my_id,
                                const ekk_consensus_config_t *config)
{
    if (cons == NULL || my_id == EKK_INVALID_MODULE_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    memset(cons, 0, sizeof(ekk_consensus_t));
    cons->my_id = my_id;
    cons->next_ballot_id = 1;  /* 0 is invalid */

    /* Apply configuration */
    if (config != NULL) {
        cons->config = *config;
    } else {
        ekk_consensus_config_t default_config = EKK_CONSENSUS_CONFIG_DEFAULT;
        cons->config = default_config;
    }

    return EKK_OK;
}

ekk_error_t ekk_consensus_set_electorate(ekk_consensus_t *cons,
                                          const ekk_module_id_t *voters,
                                          uint32_t voter_count)
{
    if (cons == NULL || (voters == NULL && voter_count > 0)) {
        return EKK_ERR_INVALID_ARG;
    }

    cons->electorate_count = 0;
    memset(cons->electorate, EKK_INVALID_MODULE_ID, sizeof(cons->electorate));

    for (uint32_t i = 0; i < voter_count; i++) {
        ekk_module_id_t voter_id = voters[i];

        if (voter_id == EKK_INVALID_MODULE_ID || voter_id == cons->my_id) {
            continue;
        }

        append_unique_voter(cons->electorate,
                            &cons->electorate_count,
                            voter_id,
                            EKK_ARRAY_SIZE(cons->electorate));
    }

    return EKK_OK;
}

/* ============================================================================
 * PROPOSAL CREATION
 * ============================================================================ */

ekk_error_t ekk_consensus_propose(ekk_consensus_t *cons,
                                   ekk_proposal_type_t type,
                                   uint32_t data,
                                   ekk_fixed_t threshold,
                                   ekk_ballot_id_t *ballot_id)
{
    if (cons == NULL || ballot_id == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Allocate ballot slot */
    int idx = allocate_ballot_slot(cons);
    if (idx < 0) {
        return EKK_ERR_BUSY;
    }

    ekk_time_us_t now = ekk_hal_time_us();

    /* Initialize ballot */
    ekk_ballot_t *ballot = &cons->ballots[idx];
    memset(ballot, 0, sizeof(ekk_ballot_t));

    ballot->id = cons->next_ballot_id++;
    ballot->type = type;
    ballot->proposer = cons->my_id;
    ballot->proposal_data = data;
    ballot->threshold = threshold;
    ballot->deadline = now + cons->config.vote_timeout;
    ballot->result = EKK_VOTE_PENDING;
    ballot->completed = EKK_FALSE;

    /* Snapshot the current electorate so later topology changes do not
     * retroactively change this ballot's quorum semantics. */
    snapshot_ballot_electorate(cons, ballot);

    cons->active_ballot_count++;

    /* Broadcast proposal */
    broadcast_proposal(cons, ballot);

    *ballot_id = ballot->id;
    return EKK_OK;
}

/* ============================================================================
 * VOTING
 * ============================================================================ */

ekk_error_t ekk_consensus_vote(ekk_consensus_t *cons,
                                ekk_module_id_t proposer_id,
                                ekk_ballot_id_t ballot_id,
                                ekk_vote_value_t vote)
{
    if (cons == NULL || proposer_id == EKK_INVALID_MODULE_ID ||
        ballot_id == EKK_INVALID_BALLOT_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Find ballot */
    int idx = find_ballot_index(cons, proposer_id, ballot_id);
    if (idx < 0) {
        return EKK_ERR_NOT_FOUND;
    }

    ekk_ballot_t *ballot = &cons->ballots[idx];

    /* Cannot vote on completed ballot */
    if (ballot->completed) {
        return EKK_ERR_BUSY;
    }

    /* Send vote to proposer */
    return send_vote(cons, ballot->proposer, ballot->id, vote);
}

/* ============================================================================
 * INHIBITION
 * ============================================================================ */

ekk_error_t ekk_consensus_inhibit(ekk_consensus_t *cons,
                                   ekk_module_id_t proposer_id,
                                   ekk_ballot_id_t ballot_id)
{
    if (cons == NULL || proposer_id == EKK_INVALID_MODULE_ID ||
        ballot_id == EKK_INVALID_BALLOT_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    ekk_time_us_t now = ekk_hal_time_us();
    record_inhibition(cons, proposer_id, ballot_id, now);

    /* Broadcast inhibit message */
    send_inhibit(cons, proposer_id, ballot_id);

    return EKK_OK;
}

/* ============================================================================
 * INCOMING MESSAGE HANDLERS
 * ============================================================================ */

ekk_error_t ekk_consensus_on_vote(ekk_consensus_t *cons,
                                   ekk_module_id_t voter_id,
                                   ekk_module_id_t proposer_id,
                                   ekk_ballot_id_t ballot_id,
                                   ekk_vote_value_t vote)
{
    if (cons == NULL || voter_id == EKK_INVALID_MODULE_ID ||
        proposer_id == EKK_INVALID_MODULE_ID ||
        ballot_id == EKK_INVALID_BALLOT_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    if (vote == EKK_VOTE_INHIBIT) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Find ballot */
    int idx = find_ballot_index(cons, proposer_id, ballot_id);
    if (idx < 0) {
        /* Unknown ballot - might be from a proposal we haven't seen */
        return EKK_ERR_NOT_FOUND;
    }

    ekk_ballot_t *ballot = &cons->ballots[idx];

    /* Only proposer can receive votes */
    if (ballot->proposer != cons->my_id) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Cannot vote on completed ballot */
    if (ballot->completed) {
        return EKK_OK;  /* Ignore late votes */
    }

    /* Only explicitly eligible voters may participate in this ballot. */
    int voter_slot = find_ballot_voter_slot(ballot, voter_id);
    if (voter_slot < 0) {
        return EKK_ERR_NOT_FOUND;
    }

    /* Check for duplicate vote */
    if (ballot->votes[voter_slot] != EKK_VOTE_ABSTAIN) {
        return EKK_OK;  /* Already voted */
    }

    /* Record vote */
    ballot->votes[voter_slot] = vote;
    ballot->vote_count++;

    switch (vote) {
        case EKK_VOTE_YES:
            ballot->yes_count++;
            break;
        case EKK_VOTE_NO:
            ballot->no_count++;
            break;
        default:
            break;
    }

    /* Check if we can determine result early */
    ekk_vote_result_t result = evaluate_ballot(ballot);
    if (result != EKK_VOTE_PENDING) {
        finalize_ballot(cons, ballot, result);
    }

    return EKK_OK;
}

ekk_error_t ekk_consensus_on_inhibit(ekk_consensus_t *cons,
                                      ekk_module_id_t sender_id,
                                      ekk_module_id_t proposer_id,
                                      ekk_ballot_id_t ballot_id)
{
    if (cons == NULL || sender_id == EKK_INVALID_MODULE_ID ||
        proposer_id == EKK_INVALID_MODULE_ID ||
        ballot_id == EKK_INVALID_BALLOT_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    if (sender_id == cons->my_id) {
        return EKK_OK;
    }

    record_inhibition(cons, proposer_id, ballot_id, ekk_hal_time_us());
    return EKK_OK;
}

ekk_error_t ekk_consensus_on_proposal(ekk_consensus_t *cons,
                                       ekk_module_id_t proposer_id,
                                       ekk_ballot_id_t ballot_id,
                                       ekk_proposal_type_t type,
                                       uint32_t data,
                                       ekk_fixed_t threshold)
{
    if (cons == NULL || proposer_id == EKK_INVALID_MODULE_ID ||
        ballot_id == EKK_INVALID_BALLOT_ID) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Ignore self proposals */
    if (proposer_id == cons->my_id) {
        return EKK_OK;
    }

    ekk_time_us_t now = ekk_hal_time_us();

    /* Check if inhibited */
    if (is_inhibited(cons, proposer_id, ballot_id, now)) {
        /* Send inhibit response */
        send_inhibit(cons, proposer_id, ballot_id);
        return EKK_ERR_INHIBITED;
    }

    /* Check if we already have this ballot */
    int idx = find_ballot_index(cons, proposer_id, ballot_id);
    if (idx >= 0) {
        /* Duplicate proposal */
        return EKK_OK;
    }

    /* Allocate slot for remote ballot tracking */
    idx = allocate_ballot_slot(cons);
    if (idx < 0) {
        /* No room - vote no */
        send_vote(cons, proposer_id, ballot_id, EKK_VOTE_NO);
        return EKK_ERR_BUSY;
    }

    /* Store ballot info */
    ekk_ballot_t *ballot = &cons->ballots[idx];
    memset(ballot, 0, sizeof(ekk_ballot_t));

    ballot->id = ballot_id;
    ballot->type = type;
    ballot->proposer = proposer_id;
    ballot->proposal_data = data;
    ballot->threshold = threshold;
    ballot->deadline = now + cons->config.vote_timeout;
    ballot->result = EKK_VOTE_PENDING;
    ballot->completed = EKK_FALSE;
    ballot->eligible_voter_count = 0;
    memset(ballot->eligible_voters, EKK_INVALID_MODULE_ID, sizeof(ballot->eligible_voters));
    reset_ballot_votes(ballot);

    cons->active_ballot_count++;

    /* Decide how to vote */
    ekk_vote_value_t my_vote = EKK_VOTE_ABSTAIN;

    if (cons->decide_callback != NULL) {
        my_vote = cons->decide_callback(cons, cons->decide_context, ballot);
    } else {
        /* Default: vote yes */
        my_vote = EKK_VOTE_YES;
    }

    /* Send vote */
    send_vote(cons, proposer_id, ballot_id, my_vote);

    return EKK_OK;
}

/* ============================================================================
 * QUERY
 * ============================================================================ */

ekk_vote_result_t ekk_consensus_get_result(const ekk_consensus_t *cons,
                                            ekk_module_id_t proposer_id,
                                            ekk_ballot_id_t ballot_id)
{
    if (cons == NULL || proposer_id == EKK_INVALID_MODULE_ID ||
        ballot_id == EKK_INVALID_BALLOT_ID) {
        return EKK_VOTE_PENDING;
    }

    int idx = find_ballot_index(cons, proposer_id, ballot_id);
    if (idx < 0) {
        return EKK_VOTE_PENDING;  /* Unknown */
    }

    return cons->ballots[idx].result;
}

/* ============================================================================
 * PERIODIC TICK
 * ============================================================================ */

uint32_t ekk_consensus_tick(ekk_consensus_t *cons, ekk_time_us_t now)
{
    if (cons == NULL) {
        return 0;
    }

    uint32_t completed_count = 0;

    /* Check each active ballot for timeout */
    for (uint32_t i = 0; i < cons->active_ballot_count; i++) {
        ekk_ballot_t *ballot = &cons->ballots[i];

        if (ballot->completed) {
            continue;
        }

        /* Check for inhibition */
        if (is_inhibited(cons, ballot->proposer, ballot->id, now)) {
            finalize_ballot(cons, ballot, EKK_VOTE_CANCELLED);
            completed_count++;
            continue;
        }

        /* Check for timeout */
        if (now >= ballot->deadline) {
            ekk_vote_result_t result = evaluate_ballot(ballot);

            if (result == EKK_VOTE_PENDING) {
                if (cons->config.require_all_neighbors &&
                    ballot->vote_count < ballot->eligible_voter_count) {
                    result = EKK_VOTE_TIMEOUT;
                } else {
                    result = EKK_VOTE_REJECTED;
                }
            }

            finalize_ballot(cons, ballot, result);
            completed_count++;
        }
    }

    /* Clean up expired inhibitions */
    uint32_t write_idx = 0;
    for (uint32_t read_idx = 0; read_idx < cons->inhibit_count; read_idx++) {
        if (cons->inhibit_until[read_idx] > now) {
            if (write_idx != read_idx) {
                cons->inhibited[write_idx] = cons->inhibited[read_idx];
                cons->inhibit_until[write_idx] = cons->inhibit_until[read_idx];
            }
            write_idx++;
        }
    }
    cons->inhibit_count = write_idx;

    /* Clean up completed ballots periodically */
    if (completed_count > 0) {
        cleanup_completed_ballots(cons);
    }

    return completed_count;
}

/* ============================================================================
 * CALLBACKS
 * ============================================================================ */

void ekk_consensus_set_decide_callback(ekk_consensus_t *cons,
                                        ekk_consensus_decide_cb callback,
                                        void *context)
{
    if (cons == NULL) {
        return;
    }

    cons->decide_callback = callback;
    cons->decide_context = context;
}

void ekk_consensus_set_complete_callback(ekk_consensus_t *cons,
                                          ekk_consensus_complete_cb callback,
                                          void *context)
{
    if (cons == NULL) {
        return;
    }

    cons->complete_callback = callback;
    cons->complete_context = context;
}

/* ============================================================================
 * BYZANTINE QUARANTINE PROTOCOL (ROJ Paper Section V, Algorithm 3)
 * ============================================================================ */

/**
 * @brief Simple hash for evidence verification
 *
 * Computes a 32-bit hash of evidence data for quick verification.
 * Full evidence is verified separately when needed.
 */
ekk_error_t ekk_quarantine_init(ekk_quarantine_state_t *state)
{
    if (state == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(ekk_quarantine_state_t));
    return EKK_OK;
}

ekk_error_t ekk_consensus_propose_quarantine(ekk_consensus_t *cons,
                                               ekk_module_id_t suspect_id,
                                               const ekk_byzantine_evidence_t *evidence,
                                               ekk_ballot_id_t *ballot_id)
{
    if (cons == NULL || suspect_id == EKK_INVALID_MODULE_ID ||
        evidence == NULL || ballot_id == NULL) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Verify evidence before proposing */
    if (!ekk_consensus_verify_evidence(evidence)) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Quarantine requires 2/3 supermajority */
    return ekk_consensus_propose(cons,
                                  EKK_PROPOSAL_QUARANTINE,
                                  (uint32_t)suspect_id,
                                  EKK_THRESHOLD_SUPERMAJORITY,
                                  ballot_id);
}

ekk_bool_t ekk_consensus_verify_evidence(const ekk_byzantine_evidence_t *evidence)
{
    if (evidence == NULL) {
        return EKK_FALSE;
    }

    /* Verify evidence type is known */
    switch (evidence->evidence_type) {
        case EKK_EVIDENCE_EQUIVOCATION:
        case EKK_EVIDENCE_INVALID_MAC:
        case EKK_EVIDENCE_TIMING_ANOMALY:
        case EKK_EVIDENCE_STATE_INVALID:
            break;
        default:
            return EKK_FALSE;
    }

    /* Verify suspect ID is valid */
    if (evidence->suspect_id == EKK_INVALID_MODULE_ID ||
        evidence->suspect_id >= EKK_MAX_MODULES) {
        return EKK_FALSE;
    }

    /* Verify at least one witness */
    if (evidence->witness_count == 0) {
        return EKK_FALSE;
    }

    /* Verify witness IDs are valid and not the suspect */
    for (uint8_t i = 0; i < evidence->witness_count && i < EKK_QUARANTINE_MAX_WITNESSES; i++) {
        ekk_module_id_t w = evidence->witness_ids[i];
        if (w == EKK_INVALID_MODULE_ID || w >= EKK_MAX_MODULES || w == evidence->suspect_id) {
            return EKK_FALSE;
        }
    }

    /* Type-specific verification */
    switch (evidence->evidence_type) {
        case EKK_EVIDENCE_EQUIVOCATION:
            /* Evidence data should contain conflicting message hashes */
            /* First 4 bytes: term number */
            /* Next 8 bytes: hash of message 1 */
            /* Next 8 bytes: hash of message 2 */
            /* Messages should differ (non-zero XOR) */
            {
                uint64_t hash1, hash2;
                memcpy(&hash1, &evidence->evidence_data[4], 8);
                memcpy(&hash2, &evidence->evidence_data[12], 8);
                if (hash1 == hash2) {
                    return EKK_FALSE;  /* Not actually equivocation */
                }
            }
            break;

        case EKK_EVIDENCE_INVALID_MAC:
            /* Cannot verify MAC evidence without shared key infrastructure.
             * Reject until proper authentication is implemented. */
            return EKK_FALSE;

        case EKK_EVIDENCE_TIMING_ANOMALY:
            /* Evidence data: expected_time (8), actual_time (8), threshold (8) */
            {
                uint64_t expected, actual, threshold;
                memcpy(&expected, &evidence->evidence_data[0], 8);
                memcpy(&actual, &evidence->evidence_data[8], 8);
                memcpy(&threshold, &evidence->evidence_data[16], 8);

                /* Check that actual differs from expected by more than threshold */
                uint64_t diff = (actual > expected) ? (actual - expected) : (expected - actual);
                if (diff <= threshold) {
                    return EKK_FALSE;  /* Within acceptable bounds */
                }
            }
            break;

        case EKK_EVIDENCE_STATE_INVALID:
            /* Evidence: from_state (1), to_state (1), valid_transitions bitmap (4) */
            {
                uint8_t from_state = evidence->evidence_data[0];
                uint8_t to_state = evidence->evidence_data[1];
                uint32_t valid_bitmap;
                memcpy(&valid_bitmap, &evidence->evidence_data[2], 4);

                /* Bounds check: shift amount must be < 32 to avoid UB */
                uint32_t shift = (uint32_t)from_state * 8U + (uint32_t)to_state;
                if (shift >= 32) {
                    return EKK_FALSE;  /* Invalid state values -- reject evidence */
                }

                /* Check if transition is in valid bitmap */
                uint32_t transition_bit = 1U << shift;
                if (valid_bitmap & transition_bit) {
                    return EKK_FALSE;  /* Transition was actually valid */
                }
            }
            break;
    }

    return EKK_TRUE;
}

ekk_error_t ekk_consensus_execute_quarantine(ekk_consensus_t *cons,
                                               ekk_quarantine_state_t *state,
                                               ekk_module_id_t module_id,
                                               ekk_time_us_t now)
{
    if (cons == NULL || state == NULL ||
        module_id == EKK_INVALID_MODULE_ID || module_id >= EKK_MAX_MODULES) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Cannot quarantine self */
    if (module_id == cons->my_id) {
        return EKK_ERR_INVALID_ARG;
    }

    /* Already quarantined? */
    if (state->quarantined[module_id]) {
        return EKK_OK;
    }

    /* Execute quarantine */
    state->quarantined[module_id] = EKK_TRUE;
    state->quarantine_time[module_id] = now;
    state->quarantine_count++;

    return EKK_OK;
}

ekk_error_t ekk_quarantine_lift(ekk_quarantine_state_t *state,
                                  ekk_module_id_t module_id)
{
    if (state == NULL || module_id == EKK_INVALID_MODULE_ID ||
        module_id >= EKK_MAX_MODULES) {
        return EKK_ERR_INVALID_ARG;
    }

    if (!state->quarantined[module_id]) {
        return EKK_OK;  /* Not quarantined */
    }

    state->quarantined[module_id] = EKK_FALSE;
    state->quarantine_time[module_id] = 0;
    if (state->quarantine_count > 0) state->quarantine_count--;

    return EKK_OK;
}
