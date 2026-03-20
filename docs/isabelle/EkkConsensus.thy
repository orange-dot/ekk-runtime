(*
 * EkkConsensus.thy -- Consensus correctness proofs.
 *
 * Proof target (per VERIFICATION_PLAN.md):
 *
 *   B2 (evaluate_ballot_pure correctness):
 *     The C function evaluate_ballot_pure correctly computes the vote result.
 *     Specifically:
 *       - When all votes are in: APPROVED iff yes_ratio >= threshold
 *       - Early REJECTED iff even best-case (all remaining = yes) cannot
 *         reach threshold
 *       - PENDING otherwise
 *       - REJECTED when eligible_voters = 0 (degenerate)
 *
 * The hard part of this proof is bridging the Q16.16 fixed-point comparison
 * in the C code to the integer multiplication in the HOL spec.
 * A supporting lemma is provided for this equivalence.
 *)

theory EkkConsensus
imports
  EkkTypes
begin

(* =========================================================================
 * Abstract specification of evaluate_ballot in pure HOL.
 *
 * We avoid fixed-point arithmetic entirely: the spec uses natural number
 * multiplication. The refinement proof must show that the C code's
 * Q16.16 comparison is equivalent under the given bounds.
 *
 * Constructors correspond to ekk_vote_result_t enum values:
 *   EKK_VOTE_PENDING   = 0
 *   EKK_VOTE_APPROVED  = 1
 *   EKK_VOTE_REJECTED  = 2
 * (Check generated definitions after AutoCorres lifts the enum.)
 * ========================================================================= *)

datatype vote_result = Pending | Approved | Rejected

fun eval_ballot_spec :: "nat \<Rightarrow> nat \<Rightarrow> nat \<Rightarrow> nat \<Rightarrow> vote_result" where
  "eval_ballot_spec yes tot eligible threshold =
    (if eligible = 0
     then Rejected
     else if tot < eligible
       then
         let remaining = eligible - tot;
             max_yes   = yes + remaining
         in
         if max_yes * 65536 < eligible * threshold then Rejected
         else if yes * 65536 \<ge> eligible * threshold then Approved
         else Pending
       else
         if yes * 65536 \<ge> eligible * threshold then Approved
         else Rejected)"

(* =========================================================================
 * Key bridge lemma: Q16.16 comparison equivalence.
 *
 * The C code computes:  yes_ratio = (yes << 16) / eligible
 * and compares:         yes_ratio >= threshold
 *
 * This is equivalent to:  yes * 65536 >= eligible * threshold
 * provided eligible > 0 and the intermediate product fits in int64.
 *
 * TODO: Discharge this lemma. The proof requires reasoning about integer
 * division and the Q16.16 representation. Likely strategy:
 *   apply (simp add: div_le_iff_le_mult)
 *   followed by unat_arith or word_arith.
 * ========================================================================= *)

lemma q16_comparison_equiv:
  "\<lbrakk> eligible > 0;
     yes \<le> eligible;
     (eligible :: nat) < 2 ^ 16 \<rbrakk>
   \<Longrightarrow> (yes * 65536 \<ge> eligible * threshold) =
       ((yes * 65536) div eligible \<ge> threshold)"
  sorry (* TODO: discharge using Nat div lemmas, e.g. Nat.div_le_iff_le_mult *)

(* =========================================================================
 * B2: evaluate_ballot_pure correctness.
 *
 * TODO: Uncomment and discharge once EkkTypes session builds and
 * AutoCorres generates ekk_consensus_verif definitions.
 *
 * context ekk_consensus_verif begin
 *
 * lemma evaluate_ballot_pure_correct:
 *   "\<lbrakk> eligible_voters > 0;
 *      yes_votes \<le> eligible_voters;
 *      total_votes \<le> eligible_voters \<rbrakk>
 *    \<Longrightarrow>
 *    evaluate_ballot_pure' yes_votes total_votes eligible_voters threshold =
 *    (case eval_ballot_spec yes_votes total_votes eligible_voters threshold of
 *       Approved  \<Rightarrow> EKK_VOTE_APPROVED_
 *     | Rejected  \<Rightarrow> EKK_VOTE_REJECTED_
 *     | Pending   \<Rightarrow> EKK_VOTE_PENDING_)"
 *   unfolding evaluate_ballot_pure'_def
 *   apply (clarsimp split: if_split)
 *   (* Bridge fixed-point arithmetic using q16_comparison_equiv *)
 *   sorry
 *
 * end
 *
 * The 'sorry' is a placeholder. The real proof discharges the arithmetic
 * obligations using q16_comparison_equiv and the word arithmetic tactics.
 * ========================================================================= *)

end
