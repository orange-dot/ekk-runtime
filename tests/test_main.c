/*
 * Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "ekk/ekk.h"

#define TEST_ASSERT(cond)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "TEST FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return false;                                                         \
        }                                                                         \
    } while (0)

static int g_topology_cb_a = 0;
static int g_topology_cb_b = 0;
static ekk_vote_result_t g_last_consensus_result = EKK_VOTE_PENDING;
static uint32_t g_periodic_task_runs = 0;

static void drain_hal_messages(void)
{
    ekk_module_id_t sender_id;
    ekk_msg_type_t msg_type;
    uint8_t buffer[64];
    uint32_t len = 0;

    do {
        len = sizeof(buffer);
    } while (ekk_hal_recv(&sender_id, &msg_type, buffer, &len) == EKK_OK);
}

static bool ballot_has_voter(const ekk_ballot_t *ballot, ekk_module_id_t voter_id)
{
    for (uint32_t i = 0; i < ballot->eligible_voter_count; i++) {
        if (ballot->eligible_voters[i] == voter_id) {
            return true;
        }
    }
    return false;
}

static ekk_ballot_t *find_ballot(ekk_consensus_t *cons,
                                 ekk_module_id_t proposer_id,
                                 ekk_ballot_id_t ballot_id)
{
    for (uint32_t i = 0; i < cons->active_ballot_count; i++) {
        ekk_ballot_t *ballot = &cons->ballots[i];
        if (ballot->proposer == proposer_id && ballot->id == ballot_id) {
            return ballot;
        }
    }
    return NULL;
}

static bool topology_has_neighbor_ids(const ekk_topology_t *topo,
                                      ekk_module_id_t first,
                                      ekk_module_id_t second)
{
    return topo->neighbor_count == 2 &&
           topo->neighbors[0].id == first &&
           topo->neighbors[1].id == second;
}

static void topology_changed_a(ekk_topology_t *topo,
                               const ekk_neighbor_t *old_neighbors,
                               uint32_t old_count,
                               const ekk_neighbor_t *new_neighbors,
                               uint32_t new_count)
{
    EKK_UNUSED(topo);
    EKK_UNUSED(old_neighbors);
    EKK_UNUSED(old_count);
    EKK_UNUSED(new_neighbors);
    EKK_UNUSED(new_count);
    g_topology_cb_a++;
}

static void topology_changed_b(ekk_topology_t *topo,
                               const ekk_neighbor_t *old_neighbors,
                               uint32_t old_count,
                               const ekk_neighbor_t *new_neighbors,
                               uint32_t new_count)
{
    EKK_UNUSED(topo);
    EKK_UNUSED(old_neighbors);
    EKK_UNUSED(old_count);
    EKK_UNUSED(new_neighbors);
    EKK_UNUSED(new_count);
    g_topology_cb_b++;
}

static void on_consensus_complete(ekk_consensus_t *cons,
                                  void *context,
                                  const ekk_ballot_t *ballot,
                                  ekk_vote_result_t result)
{
    EKK_UNUSED(cons);
    EKK_UNUSED(context);
    EKK_UNUSED(ballot);
    g_last_consensus_result = result;
}

static void count_task_runs(void *arg)
{
    uint32_t *counter = arg;

    if (counter != NULL) {
        (*counter)++;
    }
}

static bool test_consensus_tracks_real_voter_ids(void)
{
    ekk_consensus_t cons;
    ekk_ballot_id_t ballot_id = EKK_INVALID_BALLOT_ID;
    ekk_module_id_t voters[] = {1, 8, 15};

    ekk_hal_set_mock_time(1000);

    TEST_ASSERT(ekk_consensus_init(&cons, 5, NULL) == EKK_OK);
    TEST_ASSERT(ekk_consensus_set_electorate(&cons, voters, EKK_ARRAY_SIZE(voters)) == EKK_OK);
    TEST_ASSERT(ekk_consensus_propose(&cons,
                                      EKK_PROPOSAL_MODE_CHANGE,
                                      0x1234U,
                                      EKK_THRESHOLD_SUPERMAJORITY,
                                      &ballot_id) == EKK_OK);

    TEST_ASSERT(ballot_id != EKK_INVALID_BALLOT_ID);
    TEST_ASSERT(cons.active_ballot_count == 1);
    TEST_ASSERT(cons.ballots[0].eligible_voter_count == 4);
    TEST_ASSERT(ballot_has_voter(&cons.ballots[0], 1));
    TEST_ASSERT(ballot_has_voter(&cons.ballots[0], 8));
    TEST_ASSERT(ballot_has_voter(&cons.ballots[0], 15));
    TEST_ASSERT(ballot_has_voter(&cons.ballots[0], 5));
    TEST_ASSERT(cons.ballots[0].vote_count == 1);
    TEST_ASSERT(cons.ballots[0].yes_count == 1);

    TEST_ASSERT(ekk_consensus_on_vote(&cons, 8, cons.my_id, ballot_id, EKK_VOTE_YES) == EKK_OK);
    TEST_ASSERT(cons.ballots[0].vote_count == 2);
    TEST_ASSERT(cons.ballots[0].yes_count == 2);

    TEST_ASSERT(ekk_consensus_on_vote(&cons, 8, cons.my_id, ballot_id, EKK_VOTE_YES) == EKK_OK);
    TEST_ASSERT(cons.ballots[0].vote_count == 2);
    TEST_ASSERT(cons.ballots[0].yes_count == 2);

    TEST_ASSERT(ekk_consensus_on_vote(&cons, 22, cons.my_id, ballot_id, EKK_VOTE_YES) == EKK_ERR_NOT_FOUND);

    TEST_ASSERT(ekk_consensus_on_vote(&cons, 15, cons.my_id, ballot_id, EKK_VOTE_YES) == EKK_OK);
    TEST_ASSERT(cons.ballots[0].completed);
    TEST_ASSERT(cons.ballots[0].result == EKK_VOTE_APPROVED);
    TEST_ASSERT(cons.ballots[0].yes_count == 3);

    return true;
}

static bool test_consensus_timeout_policy(void)
{
    ekk_consensus_t cons_timeout;
    ekk_consensus_t cons_reject;
    ekk_ballot_id_t timeout_ballot = EKK_INVALID_BALLOT_ID;
    ekk_ballot_id_t reject_ballot = EKK_INVALID_BALLOT_ID;
    ekk_module_id_t voters[] = {2, 7, 11};
    ekk_consensus_config_t require_all = EKK_CONSENSUS_CONFIG_DEFAULT;
    ekk_consensus_config_t majority_only = EKK_CONSENSUS_CONFIG_DEFAULT;

    require_all.allow_self_vote = false;
    require_all.require_all_neighbors = true;
    majority_only.allow_self_vote = false;
    majority_only.require_all_neighbors = false;

    ekk_hal_set_mock_time(5000);

    TEST_ASSERT(ekk_consensus_init(&cons_timeout, 9, &require_all) == EKK_OK);
    g_last_consensus_result = EKK_VOTE_PENDING;
    ekk_consensus_set_complete_callback(&cons_timeout, on_consensus_complete, NULL);
    TEST_ASSERT(ekk_consensus_set_electorate(&cons_timeout, voters, EKK_ARRAY_SIZE(voters)) == EKK_OK);
    TEST_ASSERT(ekk_consensus_propose(&cons_timeout,
                                      EKK_PROPOSAL_MODE_CHANGE,
                                      1U,
                                      EKK_THRESHOLD_SUPERMAJORITY,
                                      &timeout_ballot) == EKK_OK);
    TEST_ASSERT(ekk_consensus_on_vote(&cons_timeout, 2, cons_timeout.my_id,
                                      timeout_ballot, EKK_VOTE_YES) == EKK_OK);
    ekk_hal_set_mock_time(60000);
    TEST_ASSERT(ekk_consensus_tick(&cons_timeout, ekk_hal_time_us()) == 1);
    TEST_ASSERT(g_last_consensus_result == EKK_VOTE_TIMEOUT);
    TEST_ASSERT(cons_timeout.active_ballot_count == 0);

    TEST_ASSERT(ekk_consensus_init(&cons_reject, 10, &majority_only) == EKK_OK);
    g_last_consensus_result = EKK_VOTE_PENDING;
    ekk_consensus_set_complete_callback(&cons_reject, on_consensus_complete, NULL);
    TEST_ASSERT(ekk_consensus_set_electorate(&cons_reject, voters, EKK_ARRAY_SIZE(voters)) == EKK_OK);
    ekk_hal_set_mock_time(70000);
    TEST_ASSERT(ekk_consensus_propose(&cons_reject,
                                      EKK_PROPOSAL_MODE_CHANGE,
                                      2U,
                                      EKK_THRESHOLD_SUPERMAJORITY,
                                      &reject_ballot) == EKK_OK);
    TEST_ASSERT(ekk_consensus_on_vote(&cons_reject, 2, cons_reject.my_id,
                                      reject_ballot, EKK_VOTE_YES) == EKK_OK);
    ekk_hal_set_mock_time(130000);
    TEST_ASSERT(ekk_consensus_tick(&cons_reject, ekk_hal_time_us()) == 1);
    TEST_ASSERT(g_last_consensus_result == EKK_VOTE_REJECTED);
    TEST_ASSERT(cons_reject.active_ballot_count == 0);

    return true;
}

static bool test_consensus_ballot_keys_disambiguate_collisions(void)
{
    ekk_consensus_t cons;
    ekk_ballot_id_t local_ballot = EKK_INVALID_BALLOT_ID;
    ekk_module_id_t voters[] = {8, 15};
    ekk_ballot_t *local = NULL;
    ekk_ballot_t *remote = NULL;

    ekk_hal_set_mock_time(150000);

    TEST_ASSERT(ekk_consensus_init(&cons, 5, NULL) == EKK_OK);
    TEST_ASSERT(ekk_consensus_set_electorate(&cons, voters, EKK_ARRAY_SIZE(voters)) == EKK_OK);
    TEST_ASSERT(ekk_consensus_propose(&cons,
                                      EKK_PROPOSAL_MODE_CHANGE,
                                      0xCAFEU,
                                      EKK_THRESHOLD_SIMPLE_MAJORITY,
                                      &local_ballot) == EKK_OK);
    TEST_ASSERT(local_ballot != EKK_INVALID_BALLOT_ID);

    TEST_ASSERT(ekk_consensus_on_proposal(&cons, 9, local_ballot,
                                          EKK_PROPOSAL_MODE_CHANGE,
                                          0xBEEFU,
                                          EKK_THRESHOLD_SIMPLE_MAJORITY) == EKK_OK);
    TEST_ASSERT(cons.active_ballot_count == 2);

    local = find_ballot(&cons, 5, local_ballot);
    remote = find_ballot(&cons, 9, local_ballot);
    TEST_ASSERT(local != NULL);
    TEST_ASSERT(remote != NULL);
    TEST_ASSERT(local != remote);
    TEST_ASSERT(remote->vote_count == 0);

    TEST_ASSERT(ekk_consensus_on_vote(&cons, 8, 5, local_ballot, EKK_VOTE_YES) == EKK_OK);
    TEST_ASSERT(local->completed);
    TEST_ASSERT(local->result == EKK_VOTE_APPROVED);
    TEST_ASSERT(remote->completed == EKK_FALSE);
    TEST_ASSERT(remote->vote_count == 0);

    TEST_ASSERT(ekk_consensus_on_inhibit(&cons, 12, 9, local_ballot) == EKK_OK);
    TEST_ASSERT(remote->completed);
    TEST_ASSERT(remote->result == EKK_VOTE_CANCELLED);
    TEST_ASSERT(local->result == EKK_VOTE_APPROVED);
    TEST_ASSERT(ekk_consensus_get_result(&cons, 5, local_ballot) == EKK_VOTE_APPROVED);
    TEST_ASSERT(ekk_consensus_get_result(&cons, 9, local_ballot) == EKK_VOTE_CANCELLED);

    return true;
}

static bool test_topology_instances_are_isolated(void)
{
    ekk_topology_t topo_a;
    ekk_topology_t topo_b;
    ekk_topology_config_t config = EKK_TOPOLOGY_CONFIG_DEFAULT;

    config.metric = EKK_DISTANCE_PHYSICAL;
    config.k_neighbors = 2;

    TEST_ASSERT(ekk_topology_init(&topo_a, 1, (ekk_position_t){0, 0, 0}, &config) == EKK_OK);
    TEST_ASSERT(ekk_topology_init(&topo_b, 9, (ekk_position_t){1000, 0, 0}, &config) == EKK_OK);

    g_topology_cb_a = 0;
    g_topology_cb_b = 0;
    ekk_topology_set_callback(&topo_a, topology_changed_a);
    ekk_topology_set_callback(&topo_b, topology_changed_b);

    TEST_ASSERT(ekk_topology_on_discovery(&topo_a, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo_a, 3, (ekk_position_t){100, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_reelect(&topo_a) == 2);
    TEST_ASSERT(g_topology_cb_a >= 1);
    TEST_ASSERT(g_topology_cb_b == 0);
    TEST_ASSERT(topo_a.neighbor_count == 2);
    TEST_ASSERT(topo_a.neighbors[0].id == 2);
    TEST_ASSERT(topo_a.neighbors[0].logical_distance == 1);
    TEST_ASSERT(topo_a.neighbors[1].id == 3);
    TEST_ASSERT(topo_a.neighbors[1].logical_distance == 100);

    TEST_ASSERT(ekk_topology_on_discovery(&topo_b, 20, (ekk_position_t){1001, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo_b, 21, (ekk_position_t){1002, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_reelect(&topo_b) == 2);
    TEST_ASSERT(g_topology_cb_b >= 1);
    TEST_ASSERT(topo_b.neighbor_count == 2);
    TEST_ASSERT(topo_b.neighbors[0].id == 20);
    TEST_ASSERT(topo_b.neighbors[0].logical_distance == 1);
    TEST_ASSERT(topo_b.neighbors[1].id == 21);
    TEST_ASSERT(topo_b.neighbors[1].logical_distance == 2);

    TEST_ASSERT(ekk_topology_reelect(&topo_a) == 2);
    TEST_ASSERT(topo_a.neighbor_count == 2);
    TEST_ASSERT(topo_a.neighbors[0].id == 2);
    TEST_ASSERT(topo_a.neighbors[0].logical_distance == 1);
    TEST_ASSERT(topo_a.neighbors[1].id == 3);
    TEST_ASSERT(topo_a.neighbors[1].logical_distance == 100);

    return true;
}

static bool test_topology_promotes_better_candidate_without_manual_reelect(void)
{
    ekk_topology_t topo;
    ekk_topology_config_t config = EKK_TOPOLOGY_CONFIG_DEFAULT;

    config.metric = EKK_DISTANCE_PHYSICAL;
    config.k_neighbors = 2;

    TEST_ASSERT(ekk_topology_init(&topo, 1, (ekk_position_t){0, 0, 0}, &config) == EKK_OK);

    TEST_ASSERT(ekk_topology_on_discovery(&topo, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo, 3, (ekk_position_t){100, 0, 0}) == EKK_OK);
    TEST_ASSERT(topology_has_neighbor_ids(&topo, 2, 3));

    TEST_ASSERT(ekk_topology_on_discovery(&topo, 4, (ekk_position_t){2, 0, 0}) == EKK_OK);
    TEST_ASSERT(topology_has_neighbor_ids(&topo, 2, 4));
    TEST_ASSERT(topo.neighbors[0].logical_distance == 1);
    TEST_ASSERT(topo.neighbors[1].logical_distance == 2);

    return true;
}

static bool test_topology_ignores_worse_candidate_without_churn(void)
{
    ekk_topology_t topo;
    ekk_topology_config_t config = EKK_TOPOLOGY_CONFIG_DEFAULT;

    config.metric = EKK_DISTANCE_PHYSICAL;
    config.k_neighbors = 2;

    TEST_ASSERT(ekk_topology_init(&topo, 1, (ekk_position_t){0, 0, 0}, &config) == EKK_OK);

    TEST_ASSERT(ekk_topology_on_discovery(&topo, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo, 4, (ekk_position_t){2, 0, 0}) == EKK_OK);
    TEST_ASSERT(topology_has_neighbor_ids(&topo, 2, 4));

    TEST_ASSERT(ekk_topology_on_discovery(&topo, 5, (ekk_position_t){200, 0, 0}) == EKK_OK);
    TEST_ASSERT(topology_has_neighbor_ids(&topo, 2, 4));
    TEST_ASSERT(topo.neighbors[0].logical_distance == 1);
    TEST_ASSERT(topo.neighbors[1].logical_distance == 2);

    return true;
}

static bool test_topology_arrival_order_is_independent(void)
{
    ekk_topology_t topo_a;
    ekk_topology_t topo_b;
    ekk_topology_config_t config = EKK_TOPOLOGY_CONFIG_DEFAULT;

    config.metric = EKK_DISTANCE_PHYSICAL;
    config.k_neighbors = 2;

    TEST_ASSERT(ekk_topology_init(&topo_a, 1, (ekk_position_t){0, 0, 0}, &config) == EKK_OK);
    TEST_ASSERT(ekk_topology_init(&topo_b, 1, (ekk_position_t){0, 0, 0}, &config) == EKK_OK);

    TEST_ASSERT(ekk_topology_on_discovery(&topo_a, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo_a, 3, (ekk_position_t){100, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo_a, 4, (ekk_position_t){2, 0, 0}) == EKK_OK);

    TEST_ASSERT(ekk_topology_on_discovery(&topo_b, 3, (ekk_position_t){100, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo_b, 4, (ekk_position_t){2, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo_b, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);

    TEST_ASSERT(topology_has_neighbor_ids(&topo_a, 2, 4));
    TEST_ASSERT(topology_has_neighbor_ids(&topo_b, 2, 4));
    TEST_ASSERT(topo_a.neighbors[0].logical_distance == topo_b.neighbors[0].logical_distance);
    TEST_ASSERT(topo_a.neighbors[1].logical_distance == topo_b.neighbors[1].logical_distance);

    return true;
}

static bool test_topology_neighbor_position_update_can_trigger_replacement(void)
{
    ekk_topology_t topo;
    ekk_topology_config_t config = EKK_TOPOLOGY_CONFIG_DEFAULT;

    config.metric = EKK_DISTANCE_PHYSICAL;
    config.k_neighbors = 2;

    TEST_ASSERT(ekk_topology_init(&topo, 1, (ekk_position_t){0, 0, 0}, &config) == EKK_OK);

    TEST_ASSERT(ekk_topology_on_discovery(&topo, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo, 3, (ekk_position_t){2, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&topo, 4, (ekk_position_t){10, 0, 0}) == EKK_OK);
    TEST_ASSERT(topology_has_neighbor_ids(&topo, 2, 3));

    TEST_ASSERT(ekk_topology_on_discovery(&topo, 3, (ekk_position_t){50, 0, 0}) == EKK_OK);
    TEST_ASSERT(topology_has_neighbor_ids(&topo, 2, 4));
    TEST_ASSERT(topo.neighbors[0].logical_distance == 1);
    TEST_ASSERT(topo.neighbors[1].logical_distance == 10);

    return true;
}

static bool test_module_proposal_snapshots_neighbors(void)
{
    ekk_module_t mod;
    ekk_ballot_id_t ballot_id = EKK_INVALID_BALLOT_ID;

    TEST_ASSERT(ekk_module_init(&mod, 5, "module-5", (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 1, (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 8, (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 15, (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_reelect(&mod.topology) == 3);

    ekk_hal_set_mock_time(200000);
    TEST_ASSERT(ekk_module_propose_mode(&mod, 42U, &ballot_id) == EKK_OK);
    TEST_ASSERT(ballot_id != EKK_INVALID_BALLOT_ID);
    TEST_ASSERT(mod.consensus.active_ballot_count == 1);
    TEST_ASSERT(mod.consensus.ballots[0].eligible_voter_count == 4);
    TEST_ASSERT(ballot_has_voter(&mod.consensus.ballots[0], 1));
    TEST_ASSERT(ballot_has_voter(&mod.consensus.ballots[0], 8));
    TEST_ASSERT(ballot_has_voter(&mod.consensus.ballots[0], 15));
    TEST_ASSERT(ballot_has_voter(&mod.consensus.ballots[0], 5));

    return true;
}

static bool test_module_tick_reports_degraded_on_missing_neighbor_fields(void)
{
    ekk_module_t mod;

    drain_hal_messages();
    TEST_ASSERT(ekk_field_init(ekk_get_field_region()) == EKK_OK);

    ekk_hal_set_mock_time(1000);
    TEST_ASSERT(ekk_module_init(&mod, 5, "module-5", (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_module_start(&mod) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);
    TEST_ASSERT(mod.topology.neighbor_count == 1);
    mod.topology.neighbors[0].health = EKK_HEALTH_ALIVE;

    ekk_hal_set_mock_time(2000);
    TEST_ASSERT(ekk_module_tick(&mod, ekk_hal_time_us()) == EKK_ERR_DEGRADED);
    TEST_ASSERT(mod.last_tick == 2000);
    TEST_ASSERT(mod.ticks_total == 1);
    TEST_ASSERT(mod.ticks_soft_degraded == 1);
    TEST_ASSERT(mod.ticks_hard_error == 0);
    TEST_ASSERT(mod.field_sample_fail == 1);
    TEST_ASSERT(mod.field_publish_ok == 1);
    TEST_ASSERT(mod.field_publish_fail == 0);

    return true;
}

static bool test_module_tick_propagates_hal_recv_hard_error(void)
{
    ekk_module_t mod;

    drain_hal_messages();
    TEST_ASSERT(ekk_field_init(ekk_get_field_region()) == EKK_OK);

    ekk_hal_set_mock_time(3000);
    TEST_ASSERT(ekk_module_init(&mod, 6, "module-6", (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_module_start(&mod) == EKK_OK);

    ekk_time_us_t previous_last_tick = mod.last_tick;

    ekk_hal_set_mock_time(4000);
    ekk_hal_set_recv_error_once(EKK_ERR_HAL_FAILURE);
    TEST_ASSERT(ekk_module_tick(&mod, ekk_hal_time_us()) == EKK_ERR_HAL_FAILURE);
    TEST_ASSERT(mod.last_tick == previous_last_tick);
    TEST_ASSERT(mod.ticks_total == 1);
    TEST_ASSERT(mod.ticks_soft_degraded == 0);
    TEST_ASSERT(mod.ticks_hard_error == 1);
    TEST_ASSERT(mod.field_publish_ok == 0);

    return true;
}

static bool test_field_sampling_uses_caller_time_for_age(void)
{
    ekk_field_t field = {0};
    ekk_field_t aggregate = {0};
    ekk_neighbor_t neighbor = {0};

    drain_hal_messages();
    TEST_ASSERT(ekk_field_init(ekk_get_field_region()) == EKK_OK);

    field.components[EKK_FIELD_LOAD] = EKK_FIXED_ONE;
    field.source = 2;

    neighbor.id = 2;
    neighbor.health = EKK_HEALTH_ALIVE;
    neighbor.logical_distance = 1;

    ekk_hal_set_mock_time(9000000);
    TEST_ASSERT(ekk_field_publish_at(2, &field, 1000) == EKK_OK);
    TEST_ASSERT(ekk_field_sample_neighbors_at(&neighbor, 1, &aggregate, 2000) == EKK_OK);
    TEST_ASSERT(aggregate.timestamp == 1000);
    TEST_ASSERT(aggregate.components[EKK_FIELD_LOAD] > 0);

    return true;
}

static bool test_heartbeat_tick_records_logic_send_time(void)
{
    ekk_heartbeat_t hb;
    ekk_heartbeat_config_t config = EKK_HEARTBEAT_CONFIG_DEFAULT;

    drain_hal_messages();
    config.period = 1000;
    config.auto_broadcast = EKK_TRUE;

    TEST_ASSERT(ekk_heartbeat_init(&hb, 1, &config) == EKK_OK);

    ekk_hal_set_mock_time(100);
    TEST_ASSERT(ekk_heartbeat_tick(&hb, 5000) == 0);
    TEST_ASSERT(hb.last_send == 5000);
    drain_hal_messages();

    return true;
}

static bool test_module_tick_uses_logic_time_for_periodic_due_selection(void)
{
    ekk_module_t mod;
    ekk_task_id_t task_id = 0xFF;

    g_periodic_task_runs = 0;
    drain_hal_messages();
    TEST_ASSERT(ekk_field_init(ekk_get_field_region()) == EKK_OK);

    ekk_hal_set_mock_time(1000);
    TEST_ASSERT(ekk_module_init(&mod, 7, "module-7", (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_module_start(&mod) == EKK_OK);
    TEST_ASSERT(ekk_module_add_task(&mod, "periodic", count_task_runs, &g_periodic_task_runs,
                                    1, 1000, &task_id) == EKK_OK);
    TEST_ASSERT(ekk_module_task_ready(&mod, task_id) == EKK_OK);
    mod.tasks[task_id].next_run = 5000;

    TEST_ASSERT(ekk_module_tick(&mod, 5000) == EKK_OK);
    TEST_ASSERT(g_periodic_task_runs == 1);
    TEST_ASSERT(mod.tasks[task_id].run_count == 1);
    TEST_ASSERT(mod.tasks[task_id].next_run == 6000);

    return true;
}

static bool test_module_tick_publishes_field_with_tick_timestamp(void)
{
    ekk_module_t mod;
    ekk_field_t published = {0};

    drain_hal_messages();
    TEST_ASSERT(ekk_field_init(ekk_get_field_region()) == EKK_OK);

    ekk_hal_set_mock_time(1000);
    TEST_ASSERT(ekk_module_init(&mod, 10, "module-10", (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_module_start(&mod) == EKK_OK);

    /* Deliberately diverge HAL time from the logical tick time. */
    ekk_hal_set_mock_time(1234);
    TEST_ASSERT(ekk_module_tick(&mod, 5000) == EKK_OK);
    TEST_ASSERT(ekk_field_sample_at(mod.id, &published, 5000) == EKK_OK);
    TEST_ASSERT(published.timestamp == 5000);
    TEST_ASSERT(mod.last_tick == 5000);
    TEST_ASSERT(mod.field_publish_ok == 1);

    return true;
}

static bool test_module_tick_reports_degraded_on_field_publish_failure(void)
{
    ekk_module_t mod;
    ekk_module_id_t original_id;

    drain_hal_messages();
    TEST_ASSERT(ekk_field_init(ekk_get_field_region()) == EKK_OK);

    ekk_hal_set_mock_time(1000);
    TEST_ASSERT(ekk_module_init(&mod, 11, "module-11", (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_module_start(&mod) == EKK_OK);

    /* White-box fault injection: force the publish path to hit INVALID_ARG
     * without perturbing the rest of the tick pipeline. */
    original_id = mod.id;
    mod.id = EKK_INVALID_MODULE_ID;

    TEST_ASSERT(ekk_module_tick(&mod, 2000) == EKK_ERR_DEGRADED);
    TEST_ASSERT(mod.ticks_total == 1);
    TEST_ASSERT(mod.ticks_soft_degraded == 1);
    TEST_ASSERT(mod.ticks_hard_error == 0);
    TEST_ASSERT(mod.field_publish_ok == 0);
    TEST_ASSERT(mod.field_publish_fail == 1);
    TEST_ASSERT(mod.last_tick == 2000);

    mod.id = original_id;
    return true;
}

static bool test_module_metrics_track_topology_and_heartbeat_events(void)
{
    ekk_module_t mod;

    drain_hal_messages();
    TEST_ASSERT(ekk_field_init(ekk_get_field_region()) == EKK_OK);

    ekk_hal_set_mock_time(1000);
    TEST_ASSERT(ekk_module_init(&mod, 8, "module-8", (ekk_position_t){0, 0, 0}) == EKK_OK);

    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);
    TEST_ASSERT(mod.neighbor_set_changes == 1);

    TEST_ASSERT(ekk_heartbeat_add_neighbor(&mod.heartbeat, 2) == EKK_OK);
    TEST_ASSERT(ekk_heartbeat_received(&mod.heartbeat, 2, 1, 1000) == EKK_OK);
    TEST_ASSERT(mod.heartbeat_state_changes == 1);

    TEST_ASSERT(ekk_module_start(&mod) == EKK_OK);
    TEST_ASSERT(ekk_module_tick(&mod, 2000000) == EKK_OK);
    TEST_ASSERT(mod.discovery_broadcasts == 1);

    return true;
}

static bool test_module_metrics_count_only_real_neighbor_set_changes(void)
{
    ekk_module_t mod;

    drain_hal_messages();
    TEST_ASSERT(ekk_field_init(ekk_get_field_region()) == EKK_OK);

    ekk_hal_set_mock_time(1000);
    TEST_ASSERT(ekk_module_init(&mod, 12, "module-12", (ekk_position_t){0, 0, 0}) == EKK_OK);
    mod.topology.config.metric = EKK_DISTANCE_PHYSICAL;
    mod.topology.config.k_neighbors = 2;

    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 3, (ekk_position_t){2, 0, 0}) == EKK_OK);
    TEST_ASSERT(mod.neighbor_set_changes == 2);

    /* Worse candidate should not trigger a topology-changed callback. */
    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 4, (ekk_position_t){100, 0, 0}) == EKK_OK);
    TEST_ASSERT(mod.neighbor_set_changes == 2);

    /* Better candidate should replace the current worst neighbor. */
    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 5, (ekk_position_t){1, 1, 0}) == EKK_OK);
    TEST_ASSERT(mod.neighbor_set_changes == 3);
    TEST_ASSERT(topology_has_neighbor_ids(&mod.topology, 2, 5));

    return true;
}

static bool test_module_metrics_track_consensus_outcomes(void)
{
    ekk_module_t mod;
    ekk_ballot_id_t approved_ballot = EKK_INVALID_BALLOT_ID;
    ekk_ballot_id_t cancelled_ballot = EKK_INVALID_BALLOT_ID;
    ekk_ballot_id_t timeout_ballot = EKK_INVALID_BALLOT_ID;

    drain_hal_messages();
    TEST_ASSERT(ekk_field_init(ekk_get_field_region()) == EKK_OK);

    ekk_hal_set_mock_time(1000);
    TEST_ASSERT(ekk_module_init(&mod, 9, "module-9", (ekk_position_t){0, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 2, (ekk_position_t){1, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_on_discovery(&mod.topology, 3, (ekk_position_t){2, 0, 0}) == EKK_OK);
    TEST_ASSERT(ekk_topology_reelect(&mod.topology) == 2);

    TEST_ASSERT(ekk_module_propose_mode(&mod, 11, &approved_ballot) == EKK_OK);
    TEST_ASSERT(mod.ballots_started == 1);
    TEST_ASSERT(ekk_consensus_on_vote(&mod.consensus, 2, mod.id, approved_ballot, EKK_VOTE_YES) == EKK_OK);
    TEST_ASSERT(ekk_consensus_on_vote(&mod.consensus, 3, mod.id, approved_ballot, EKK_VOTE_YES) == EKK_OK);
    TEST_ASSERT(mod.ballots_completed == 1);
    TEST_ASSERT(mod.ballots_cancelled == 0);
    TEST_ASSERT(mod.ballots_timed_out == 0);

    TEST_ASSERT(ekk_module_propose_mode(&mod, 12, &cancelled_ballot) == EKK_OK);
    TEST_ASSERT(mod.ballots_started == 2);
    TEST_ASSERT(ekk_consensus_on_inhibit(&mod.consensus, 77, mod.id, cancelled_ballot) == EKK_OK);
    TEST_ASSERT(mod.ballots_cancelled == 1);

    mod.consensus.config.require_all_neighbors = EKK_TRUE;
    ekk_hal_set_mock_time(5000);
    TEST_ASSERT(ekk_module_propose_mode(&mod, 13, &timeout_ballot) == EKK_OK);
    TEST_ASSERT(mod.ballots_started == 3);
    ekk_hal_set_mock_time(70000);
    TEST_ASSERT(ekk_consensus_tick(&mod.consensus, ekk_hal_time_us()) == 1);
    TEST_ASSERT(mod.ballots_timed_out == 1);

    return true;
}

int main(void)
{
    int failures = 0;

    if (ekk_init() != EKK_OK) {
        fprintf(stderr, "TEST FAILED: ekk_init() did not succeed\n");
        return 1;
    }

    ekk_hal_set_module_id(1);

    if (!test_consensus_tracks_real_voter_ids()) {
        failures++;
    }

    if (!test_consensus_timeout_policy()) {
        failures++;
    }

    if (!test_consensus_ballot_keys_disambiguate_collisions()) {
        failures++;
    }

    if (!test_topology_instances_are_isolated()) {
        failures++;
    }

    if (!test_topology_promotes_better_candidate_without_manual_reelect()) {
        failures++;
    }

    if (!test_topology_ignores_worse_candidate_without_churn()) {
        failures++;
    }

    if (!test_topology_arrival_order_is_independent()) {
        failures++;
    }

    if (!test_topology_neighbor_position_update_can_trigger_replacement()) {
        failures++;
    }

    if (!test_module_proposal_snapshots_neighbors()) {
        failures++;
    }

    if (!test_module_tick_reports_degraded_on_missing_neighbor_fields()) {
        failures++;
    }

    if (!test_module_tick_propagates_hal_recv_hard_error()) {
        failures++;
    }

    if (!test_field_sampling_uses_caller_time_for_age()) {
        failures++;
    }

    if (!test_heartbeat_tick_records_logic_send_time()) {
        failures++;
    }

    if (!test_module_tick_uses_logic_time_for_periodic_due_selection()) {
        failures++;
    }

    if (!test_module_tick_publishes_field_with_tick_timestamp()) {
        failures++;
    }

    if (!test_module_tick_reports_degraded_on_field_publish_failure()) {
        failures++;
    }

    if (!test_module_metrics_track_topology_and_heartbeat_events()) {
        failures++;
    }

    if (!test_module_metrics_count_only_real_neighbor_set_changes()) {
        failures++;
    }

    if (!test_module_metrics_track_consensus_outcomes()) {
        failures++;
    }

    ekk_hal_set_mock_time(0);

    if (failures > 0) {
        fprintf(stderr, "%d test group(s) failed\n", failures);
        return 1;
    }

    printf("All ekkor-agentic-experiment tests passed\n");
    return 0;
}
