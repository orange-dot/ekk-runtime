(*
 * EkkHeartbeat.thy -- Heartbeat correctness proofs.
 *
 * Proof targets (per VERIFICATION_PLAN.md):
 *
 *   B1 (toolchain validation):
 *     ekk_heartbeat_init_verif returns EKK_OK when given a valid pointer
 *     and a valid module ID.
 *
 *   B3 (health transition refinement):
 *     set_neighbor_health_verif correctly implements health_step.
 *     health_transition_valid encodes exactly the valid edges of the
 *     UNKNOWN -> ALIVE -> SUSPECT -> DEAD state machine.
 *     This closes the gap between the TLA+ safety property (Track A)
 *     and the C implementation.
 *)

theory EkkHeartbeat
imports
  EkkTypes
begin

(* =========================================================================
 * Abstract specification of the health-step state machine (Track A / B3).
 *
 * health_step formalises: given a neighbor's current health and its missed
 * heartbeat count, what is the new health state?
 *
 * The 'timeout' parameter corresponds to ekk_heartbeat_config_t.timeout_count.
 * ========================================================================= *)

datatype health =
    HUnknown
  | HAlive
  | HSuspect
  | HDead

fun health_step :: "health \<Rightarrow> nat \<Rightarrow> nat \<Rightarrow> health" where
  "health_step HAlive   missed timeout = (if missed \<ge> timeout then HSuspect else HAlive)"
| "health_step HSuspect missed timeout = (if missed \<ge> timeout then HDead   else HSuspect)"
| "health_step h        _      _       = h"

(* =========================================================================
 * B1: Toolchain validation
 *
 * Prove that ekk_heartbeat_init_verif' returns EKK_OK when hb is non-NULL
 * and my_id is a valid module ID.
 *
 * This lemma is trivial — its purpose is to confirm that:
 *   (a) the C parser accepted ekk_heartbeat_verif.c,
 *   (b) AutoCorres generated a sensible lifted definition, and
 *   (c) the Hoare triple syntax works as expected.
 *
 * EKK_OK = 0, EKK_INVALID_MODULE_ID = 0xFF (check ekk_types.h).
 * ========================================================================= *)

context ekk_verif begin

(*
 * TODO B1: Uncomment and discharge once the session builds.
 *
 * The return value constant names depend on how AutoCorres maps the
 * ekk_error_t enum. Adjust 'EKK_OK_' / 'EKK_ERR_INVALID_ARG_' to match
 * the generated constant names (use 'thm ekk_heartbeat_init_verif'_def'
 * to inspect).
 *
 * lemma ekk_heartbeat_init_verif_ok:
 *   "\<lbrace> \<lambda>s. hb \<noteq> NULL \<and> my_id \<noteq> 0xFF \<rbrace>
 *    ekk_heartbeat_init_verif' hb my_id
 *    \<lbrace> \<lambda>r s. r = EKK_OK_ \<rbrace>!"
 *   unfolding ekk_heartbeat_init_verif'_def
 *   apply wp
 *   apply clarsimp
 *   done
 *)

(*
 * TODO B3: Health-step refinement.
 *
 * Prove that set_neighbor_health_verif' correctly updates the health field:
 *
 * lemma set_neighbor_health_verif_updates_field:
 *   "\<lbrace> \<lambda>s. neighbor \<noteq> NULL \<rbrace>
 *    set_neighbor_health_verif' neighbor new_state
 *    \<lbrace> \<lambda>_ s. s[neighbor\<rightarrow>health] = new_state \<rbrace>!"
 *   unfolding set_neighbor_health_verif'_def
 *   apply wp
 *   apply clarsimp
 *   done
 *)

end

end
