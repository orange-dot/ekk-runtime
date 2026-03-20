/**
 * @file ekk_consensus.h
 * @brief EK-KOR v2 - Threshold-Based Distributed Consensus
 *
 * @copyright Copyright (c) 2026 Elektrokombinacija
 * @license MIT
 * SPDX-License-Identifier: MIT
 *
 * Threshold voting over a bounded electorate snapshot.
 *
 * The current extract implements per-ballot threshold voting with explicit
 * ballot identity `(proposer_id, ballot_id)`. It supports proposal tracking,
 * votes, inhibition, and completion callbacks, but it does not claim to solve
 * general multi-proposer contention or global proposal serialization.
 */

#ifndef EKK_CONSENSUS_H
#define EKK_CONSENSUS_H

#include "ekk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ekk_consensus ekk_consensus_t;

/* ============================================================================
 * CONSENSUS CONFIGURATION
 * ============================================================================ */

/**
 * @brief Standard threshold values
 */
#define EKK_THRESHOLD_SIMPLE_MAJORITY   32768   /* 0.50 in Q16.16 */
#define EKK_THRESHOLD_SUPERMAJORITY     43908   /* 0.67 in Q16.16 (int)(0.67 * 65536) */
#define EKK_THRESHOLD_UNANIMOUS         65536   /* 1.00 in Q16.16 */

/**
 * @brief Maximum voters that can participate in a ballot snapshot
 *
 * A proposer may count up to k current neighbors plus an optional self-vote.
 */
#define EKK_MAX_BALLOT_VOTERS           (EKK_K_NEIGHBORS + 1U)

/**
 * @brief Proposal types (application can extend)
 */
typedef enum {
    EKK_PROPOSAL_MODE_CHANGE    = 0,    /**< Change operational mode */
    EKK_PROPOSAL_POWER_LIMIT    = 1,    /**< Set cluster power limit */
    EKK_PROPOSAL_SHUTDOWN       = 2,    /**< Graceful shutdown */
    EKK_PROPOSAL_REFORMATION    = 3,    /**< Mesh reformation */
    EKK_PROPOSAL_QUARANTINE     = 4,    /**< Byzantine module quarantine (ROJ Section V) */
    EKK_PROPOSAL_CUSTOM_0       = 16,   /**< Application-defined */
    EKK_PROPOSAL_CUSTOM_1       = 17,
    EKK_PROPOSAL_CUSTOM_2       = 18,
    EKK_PROPOSAL_CUSTOM_3       = 19,
} ekk_proposal_type_t;

/**
 * @brief Consensus configuration
 */
typedef struct {
    ekk_time_us_t vote_timeout;         /**< Timeout for vote collection */
    ekk_time_us_t inhibit_duration;     /**< How long inhibition lasts */
    ekk_bool_t allow_self_vote;          /**< Can proposer vote for own proposal */
    ekk_bool_t require_all_neighbors;   /**< Require votes from all neighbors */
} ekk_consensus_config_t;

#define EKK_CONSENSUS_CONFIG_DEFAULT { \
    .vote_timeout = EKK_VOTE_TIMEOUT_US, \
    .inhibit_duration = 100000, /* 100ms */ \
    .allow_self_vote = EKK_TRUE, \
    .require_all_neighbors = EKK_FALSE, \
}

/* ============================================================================
 * BALLOT IDENTITY
 * ============================================================================ */

/**
 * @brief Canonical ballot identity within the cluster
 *
 * `ballot_id` remains proposer-local. A ballot is globally identified by the
 * proposer plus that local ID.
 */
typedef struct {
    ekk_module_id_t proposer_id;
    ekk_ballot_id_t ballot_id;
} ekk_ballot_key_t;

/* ============================================================================
 * BALLOT STRUCTURE
 * ============================================================================ */

/**
 * @brief Ballot (voting round)
 */
typedef struct {
    ekk_ballot_id_t id;                     /**< Unique ballot ID */
    ekk_proposal_type_t type;               /**< What we're voting on */
    ekk_module_id_t proposer;               /**< Who proposed it */

    uint32_t proposal_data;                 /**< Proposal-specific data */

    ekk_fixed_t threshold;                  /**< Required approval threshold */
    ekk_time_us_t deadline;                 /**< When voting ends */

    /* Vote tracking */
    ekk_module_id_t eligible_voters[EKK_MAX_BALLOT_VOTERS];
    uint8_t eligible_voter_count;           /**< Number of voters in this ballot */
    uint8_t votes[EKK_MAX_BALLOT_VOTERS];   /**< Votes by eligible voter slot */
    uint8_t vote_count;                     /**< Votes received */
    uint8_t yes_count;                      /**< Approvals */
    uint8_t no_count;                       /**< Rejections */

    ekk_vote_result_t result;               /**< Final result */
    ekk_bool_t completed;                   /**< Voting finished */
} ekk_ballot_t;

/* ============================================================================
 * CALLBACKS
 * ============================================================================ */

/**
 * @brief Callback when proposal needs local decision
 *
 * Application implements this to decide how to vote on proposals.
 */
#ifndef EKK_VERIFICATION
/* Function pointer typedefs — not supported by l4v C parser */
typedef ekk_vote_value_t (*ekk_consensus_decide_cb)(ekk_consensus_t *cons,
                                                     void *context,
                                                     const ekk_ballot_t *ballot);

/**
 * @brief Callback when ballot completes
 */
typedef void (*ekk_consensus_complete_cb)(ekk_consensus_t *cons,
                                           void *context,
                                           const ekk_ballot_t *ballot,
                                           ekk_vote_result_t result);
#endif /* EKK_VERIFICATION */

/* ============================================================================
 * CONSENSUS STATE
 * ============================================================================ */

/**
 * @brief Consensus engine state
 */
struct ekk_consensus {
    ekk_module_id_t my_id;                      /**< This module's ID */

    ekk_ballot_t ballots[EKK_MAX_BALLOTS];      /**< Active ballots */
    uint32_t active_ballot_count;

    ekk_ballot_key_t inhibited[EKK_MAX_BALLOTS]; /**< Inhibited ballot keys */
    ekk_time_us_t inhibit_until[EKK_MAX_BALLOTS];
    uint32_t inhibit_count;

    ekk_ballot_id_t next_ballot_id;             /**< Next ballot ID to use */
    ekk_module_id_t electorate[EKK_K_NEIGHBORS];
    uint8_t electorate_count;                  /**< Current peer electorate */

    ekk_consensus_config_t config;              /**< Configuration */
#ifndef EKK_VERIFICATION
    /* Callback fields — function pointers not supported by l4v C parser */
    ekk_consensus_decide_cb decide_callback;
    ekk_consensus_complete_cb complete_callback;
    void *decide_context;
    void *complete_context;
#endif
};

/* ============================================================================
 * CONSENSUS API
 * ============================================================================ */

ekk_error_t ekk_consensus_init(ekk_consensus_t *cons,
                                ekk_module_id_t my_id,
                                const ekk_consensus_config_t *config);

ekk_error_t ekk_consensus_set_electorate(ekk_consensus_t *cons,
                                          const ekk_module_id_t *voters,
                                          uint32_t voter_count);

ekk_error_t ekk_consensus_propose(ekk_consensus_t *cons,
                                   ekk_proposal_type_t type,
                                   uint32_t data,
                                   ekk_fixed_t threshold,
                                   ekk_ballot_id_t *ballot_id);

ekk_error_t ekk_consensus_vote(ekk_consensus_t *cons,
                                ekk_module_id_t proposer_id,
                                ekk_ballot_id_t ballot_id,
                                ekk_vote_value_t vote);

ekk_error_t ekk_consensus_inhibit(ekk_consensus_t *cons,
                                   ekk_module_id_t proposer_id,
                                   ekk_ballot_id_t ballot_id);

ekk_error_t ekk_consensus_on_vote(ekk_consensus_t *cons,
                                   ekk_module_id_t voter_id,
                                   ekk_module_id_t proposer_id,
                                   ekk_ballot_id_t ballot_id,
                                   ekk_vote_value_t vote);

ekk_error_t ekk_consensus_on_inhibit(ekk_consensus_t *cons,
                                      ekk_module_id_t sender_id,
                                      ekk_module_id_t proposer_id,
                                      ekk_ballot_id_t ballot_id);

ekk_error_t ekk_consensus_on_proposal(ekk_consensus_t *cons,
                                       ekk_module_id_t proposer_id,
                                       ekk_ballot_id_t ballot_id,
                                       ekk_proposal_type_t type,
                                       uint32_t data,
                                       ekk_fixed_t threshold);

ekk_vote_result_t ekk_consensus_get_result(const ekk_consensus_t *cons,
                                            ekk_module_id_t proposer_id,
                                            ekk_ballot_id_t ballot_id);

uint32_t ekk_consensus_tick(ekk_consensus_t *cons, ekk_time_us_t now);

#ifndef EKK_VERIFICATION
void ekk_consensus_set_decide_callback(ekk_consensus_t *cons,
                                        ekk_consensus_decide_cb callback,
                                        void *context);

void ekk_consensus_set_complete_callback(ekk_consensus_t *cons,
                                          ekk_consensus_complete_cb callback,
                                          void *context);
#endif

/* ============================================================================
 * VOTE MESSAGE FORMAT
 * ============================================================================ */

/**
 * @brief Vote message (sent to proposer)
 */
EKK_PACK_BEGIN
typedef struct {
    uint8_t msg_type;               /**< EKK_MSG_VOTE */
    ekk_module_id_t voter_id;       /**< Voter's ID */
    ekk_module_id_t proposer_id;    /**< Proposer collecting this vote */
    ekk_ballot_id_t ballot_id;      /**< Which ballot */
    uint8_t vote;                   /**< The vote (ekk_vote_value_t) */
    uint32_t timestamp;             /**< Voting timestamp (truncated) */
} EKK_PACKED ekk_vote_msg_t;
EKK_PACK_END

EKK_STATIC_ASSERT(sizeof(ekk_vote_msg_t) <= 12, "Vote message too large");

/**
 * @brief Proposal message (broadcast to neighbors)
 */
EKK_PACK_BEGIN
typedef struct {
    uint8_t msg_type;               /**< EKK_MSG_PROPOSAL */
    ekk_module_id_t proposer_id;    /**< Proposer's ID */
    ekk_ballot_id_t ballot_id;      /**< Ballot ID */
    uint8_t type;                   /**< Proposal type (ekk_proposal_type_t) */
    uint32_t data;                  /**< Proposal data */
    ekk_fixed_t threshold;          /**< Required threshold */
} EKK_PACKED ekk_proposal_msg_t;
EKK_PACK_END

EKK_STATIC_ASSERT(sizeof(ekk_proposal_msg_t) <= 16, "Proposal message too large");

/**
 * @brief Inhibit message (broadcast to peers)
 */
EKK_PACK_BEGIN
typedef struct {
    uint8_t msg_type;               /**< EKK_MSG_INHIBIT */
    ekk_module_id_t proposer_id;    /**< Proposer whose ballot is inhibited */
    ekk_ballot_id_t ballot_id;      /**< Proposer-local ballot ID */
    uint32_t timestamp;             /**< Inhibition timestamp (truncated) */
} EKK_PACKED ekk_inhibit_msg_t;
EKK_PACK_END

EKK_STATIC_ASSERT(sizeof(ekk_inhibit_msg_t) <= 12, "Inhibit message too large");

/* ============================================================================
 * BYZANTINE QUARANTINE PROTOCOL (ROJ Paper Section V, Algorithm 3)
 * ============================================================================ */

/**
 * @brief Byzantine evidence types
 *
 * Evidence required to propose quarantine of a misbehaving module.
 * Target metric: 99.3% Byzantine fault detection rate.
 */
typedef enum {
    EKK_EVIDENCE_EQUIVOCATION    = 0x01,  /**< Conflicting messages in same term */
    EKK_EVIDENCE_INVALID_MAC     = 0x02,  /**< Message authentication failure */
    EKK_EVIDENCE_TIMING_ANOMALY  = 0x03,  /**< Heartbeat/response timing violation */
    EKK_EVIDENCE_STATE_INVALID   = 0x04,  /**< Impossible state transition */
} ekk_evidence_type_t;

/**
 * @brief Maximum witnesses required for quarantine evidence
 */
#define EKK_QUARANTINE_MAX_WITNESSES    3

/**
 * @brief Byzantine evidence structure
 *
 * Contains proof of Byzantine behavior sufficient for quarantine vote.
 */
typedef struct {
    ekk_evidence_type_t evidence_type;      /**< Type of misbehavior detected */
    ekk_module_id_t suspect_id;             /**< Module under suspicion */
    ekk_module_id_t witness_ids[EKK_QUARANTINE_MAX_WITNESSES]; /**< Corroborating witnesses */
    uint8_t witness_count;                  /**< Number of witnesses */
    uint8_t evidence_data[24];              /**< Evidence payload (type-specific) */
    uint32_t timestamp;                     /**< When evidence was collected */
} ekk_byzantine_evidence_t;

/**
 * @brief Quarantine state tracking
 */
typedef struct {
    ekk_bool_t quarantined[EKK_MAX_MODULES];    /**< Quarantine status per module */
    ekk_time_us_t quarantine_time[EKK_MAX_MODULES]; /**< When quarantine started */
    uint8_t quarantine_count;                   /**< Number of quarantined modules */
} ekk_quarantine_state_t;

/**
 * @brief Initialize quarantine state
 *
 * @param state Quarantine state (caller-allocated)
 * @return EKK_OK on success
 */
ekk_error_t ekk_quarantine_init(ekk_quarantine_state_t *state);

/**
 * @brief Propose quarantine of a suspect module
 *
 * Initiates a 2/3 supermajority vote to quarantine a Byzantine module.
 * Evidence must be verifiable by all voting neighbors.
 *
 * @param cons Consensus state
 * @param suspect_id Module to quarantine
 * @param evidence Proof of Byzantine behavior
 * @param[out] ballot_id Assigned ballot ID
 * @return EKK_OK on success
 */
ekk_error_t ekk_consensus_propose_quarantine(ekk_consensus_t *cons,
                                               ekk_module_id_t suspect_id,
                                               const ekk_byzantine_evidence_t *evidence,
                                               ekk_ballot_id_t *ballot_id);

/**
 * @brief Verify Byzantine evidence
 *
 * Checks if evidence is valid and sufficient for quarantine.
 * Called automatically when receiving quarantine proposal.
 *
 * @param evidence Evidence to verify
 * @return true if evidence is valid
 */
ekk_bool_t ekk_consensus_verify_evidence(const ekk_byzantine_evidence_t *evidence);

/**
 * @brief Execute quarantine of a module
 *
 * Called when quarantine vote succeeds. Isolates the module:
 * - Removes from neighbor list
 * - Filters messages from module
 * - Notifies application layer
 *
 * @param cons Consensus state
 * @param state Quarantine state
 * @param module_id Module to quarantine
 * @param now Current timestamp
 * @return EKK_OK on success
 */
ekk_error_t ekk_consensus_execute_quarantine(ekk_consensus_t *cons,
                                               ekk_quarantine_state_t *state,
                                               ekk_module_id_t module_id,
                                               ekk_time_us_t now);

/**
 * @brief Check if module is quarantined
 *
 * @param state Quarantine state
 * @param module_id Module to check
 * @return true if module is quarantined
 */
static inline ekk_bool_t ekk_consensus_is_quarantined(const ekk_quarantine_state_t *state,
                                                        ekk_module_id_t module_id) {
    if (state == NULL || module_id == EKK_INVALID_MODULE_ID ||
        module_id >= EKK_MAX_MODULES) {
        return EKK_FALSE;
    }
    return state->quarantined[module_id];
}

/**
 * @brief Lift quarantine (rehabilitation)
 *
 * Removes quarantine after module has been repaired/replaced.
 * Requires consensus vote.
 *
 * @param state Quarantine state
 * @param module_id Module to rehabilitate
 * @return EKK_OK on success
 */
ekk_error_t ekk_quarantine_lift(ekk_quarantine_state_t *state,
                                  ekk_module_id_t module_id);

/* ============================================================================
 * QUARANTINE MESSAGE FORMAT
 * ============================================================================ */

/**
 * @brief Quarantine proposal message
 *
 * Includes suspect ID and evidence hash for verification.
 */
EKK_PACK_BEGIN
typedef struct {
    uint8_t msg_type;                   /**< EKK_MSG_PROPOSAL (type=QUARANTINE) */
    ekk_module_id_t proposer_id;        /**< Who is proposing */
    ekk_ballot_id_t ballot_id;          /**< Ballot ID */
    ekk_module_id_t suspect_id;         /**< Module to quarantine */
    uint8_t evidence_type;              /**< ekk_evidence_type_t */
    uint8_t evidence_hash[4];           /**< Hash of full evidence for verification */
    uint8_t witness_count;              /**< Number of corroborating witnesses */
    uint8_t reserved;
} EKK_PACKED ekk_quarantine_proposal_msg_t;
EKK_PACK_END

EKK_STATIC_ASSERT(sizeof(ekk_quarantine_proposal_msg_t) <= 12, "Quarantine proposal too large");

#ifdef __cplusplus
}
#endif

#endif /* EKK_CONSENSUS_H */
