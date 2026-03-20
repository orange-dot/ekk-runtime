<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
<!-- Copyright (c) 2026 Elektrokombinacija -->
# EK-KOR v2 — seL4 Systems Review

**Reviewer:** sel4-systems-reviewer agent
**Date:** 2026-03-18
**Scope:** Full source review — `src/`, `include/ekk/`, `src/hal/ekk_hal_posix.c`
**Not covered:** `examples/`, `tests/`, generated build artefacts

---

## Summary

EK-KOR v2 is a statically-allocated, polling-based coordination kernel for distributed power electronics. It has no dynamic memory, no recursion, and bounded loops throughout — the right foundation for verification. The C code is generally disciplined. The verification infrastructure (dual-track TLA+ / Isabelle) and the l4v compatibility audit are thoughtful.

The review below finds **4 correctness defects**, **3 undefined-behaviour paths**, **2 safety-critical design weaknesses**, and several verification-readiness gaps beyond those already catalogued in `project_l4v_compat_audit.md`.

Severity tiers used here:
- **CRITICAL** — could cause data corruption or safety failure in the field
- **HIGH** — undefined behaviour or incorrect protocol semantics
- **MEDIUM** — logic flaw that does not cause UB but produces wrong answers
- **LOW** — code hygiene, documentation, or minor inconsistency

---

## 1. Correctness Defects

### 1.1 [CRITICAL] Seqlock GC race — fresh field silently invalidated

**File:** `src/ekk_field.c`, lines 336–369 (`ekk_field_gc`)

Between the seqlock consistency read (detecting a stale field, lines 343–352) and the seqlock write that marks the slot invalid (lines 362–368), another module may have published a fresh field to the **same slot**. The GC then overwrites the valid, fresh field with `EKK_INVALID_MODULE_ID`.

```c
/* GC reads: source=X, timestamp=old  (seq_before=2) */
ekk_time_us_t age = now - timestamp;
if (age > max_age_us) {
    /* RACE: module X publishes fresh field here; seq is now 4 */
    ekk_hal_atomic_inc(&cf->sequence);   /* bumps to ODD */
    cf->field.source = EKK_INVALID_MODULE_ID;  /* clobbers fresh field */
    ekk_hal_atomic_inc(&cf->sequence);   /* bumps to EVEN */
```

The GC must re-read the sequence after the stale check and abort the invalidation if it has changed. The pattern requires a CAS on the sequence counter, not just paired increments.

---

### 1.2 [HIGH] `ekk_field_publish` copies stale sequence into `field.sequence`

**File:** `src/ekk_field.c`, line 98

```c
cf->field.sequence = (uint8_t)cf->sequence;  /* sequence is ODD here */
```

This copies the write-in-progress (odd) sequence counter into the field's own `sequence` field while the seqlock write is in progress. Readers use `cf->field.sequence` for out-of-band consistency checks (e.g. in `ekk_heartbeat_received` via `hb_msg->sequence`). The published heartbeat sequence should come from `hb->send_sequence++`, not from the seqlock counter. The two sequence namespaces are conflated here.

---

### 1.3 [HIGH] `ekk_fixed_div` does not saturate — silent cast to UB

**File:** `src/ekk_types.c`, lines 42–49

```c
ekk_fixed_t ekk_fixed_div(ekk_fixed_t a, ekk_fixed_t b)
{
    if (b == 0) { return (a >= 0) ? INT32_MAX : INT32_MIN; }
    int64_t result = (((int64_t)a) << 16) / b;
    return (ekk_fixed_t)result;          /* no saturation */
}
```

When `a` is large and `b` is small (e.g. the distance-weight computation in `ekk_field_sample_neighbors`, line 234), `result` can exceed `INT32_MAX`. The cast to `int32_t` is implementation-defined for out-of-range values (C11 §6.3.1.3¶3). On ARM Cortex-M4 this silently wraps; on other targets it may signal a hardware exception.

Compare with `ekk_fixed_mul` (lines 32–37) which saturates correctly. Apply the same pattern.

---

### 1.4 [MEDIUM] Module never enters `EKK_MODULE_REFORMING` state

**File:** `src/ekk_module.c`, function `update_module_state` (lines 41–103)

`EKK_MODULE_REFORMING` is declared in `ekk_types.h` and handled in `update_module_state` (lines 82–92), but no code path ever transitions **into** that state. There is no `case EKK_MODULE_ACTIVE:` branch that sets `new_state = EKK_MODULE_REFORMING`, and no external call to set it either. The state is reachable only if the caller sets it directly, which none of the module API functions do.

The heartbeat callback chain (`on_neighbor_dead_cb → ekk_topology_on_neighbor_lost → ekk_topology_reelect`) would be the natural trigger point. The missing transition leaves the module in `EKK_MODULE_DEGRADED` instead of `EKK_MODULE_REFORMING`, which prevents callers from distinguishing "we have fewer than k neighbors but are stable" from "we are actively reforming."

---

## 2. Undefined Behaviour

### 2.1 [HIGH] Left-shift overflow in `ekk_fixed_exp_decay`

**File:** `src/ekk_types.c`, line 69

```c
uint64_t ratio_q16 = (elapsed_us << 16) / tau_us;
```

`elapsed_us` is `uint64_t`. The expression `elapsed_us << 16` invokes UB if `elapsed_us > (UINT64_MAX >> 16)` ≈ 2.8 × 10¹⁴ µs ≈ 9 years. In normal use this is unreachable, but the early-return guard on line 65 is `elapsed_us >= tau_us * 5`. If `tau_us` is large enough that `tau_us * 5` wraps, the guard misfires and the shift proceeds. Fix: use `(elapsed_us >> n) << (16 - n)` with a chosen `n`, or clamp inside the shift.

---

### 2.2 [MEDIUM] `position_distance_sq` — intermediate int32 overflow

**File:** `src/ekk_topology.c`, lines 99–103

```c
int32_t dx = (int32_t)a.x - (int32_t)b.x;
return dx*dx + dy*dy + dz*dz;
```

`dx*dx` overflows `int32_t` when `|dx| > 46340`. The result is UB under signed overflow rules (C11 §6.5¶5). The product is passed to `isqrt(int32_t n)`; if it arrives negative, `isqrt` returns 0, making distant modules appear equidistant with themselves. Correct fix: compute the squares in `int64_t` and saturate on the way out, or use `uint32_t` position coordinates (since distances are always non-negative).

---

### 2.3 [LOW] `ekk_fixed_sqrt` casts negative input to unsigned

**File:** `src/ekk_types.c`, lines 113–129

```c
uint32_t guess = (uint32_t)x;   /* x is int32_t; x <= 0 handled above */
```

The guard `if (x <= 0) return 0` correctly handles the x = 0 case, but the function is not used anywhere in the codebase. If it is ever called with `x = INT32_MIN`, `(uint32_t)INT32_MIN` is defined (wraps), but the comment implies `x` should be non-negative. This is a latent risk. The function is not in the verification plan so it is LOW priority.

---

## 3. Safety-Critical Design Weaknesses

### 3.1 [CRITICAL] Byzantine evidence for `EKK_EVIDENCE_INVALID_MAC` accepted without proof

**File:** `src/ekk_consensus.c`, lines 821–826

```c
case EKK_EVIDENCE_INVALID_MAC:
    /* Evidence data should contain the invalid message + computed MAC */
    /* We can't verify this without the shared key, so accept if structured */
    break;
```

`ekk_consensus_verify_evidence` returns `EKK_TRUE` for `INVALID_MAC` evidence unconditionally (only structure is checked). Any module can therefore fabricate an `INVALID_MAC` claim, pass verification, and initiate a quarantine proposal (`EKK_THRESHOLD_SUPERMAJORITY`) against any target module it chooses. Because the quarantine requires only 2/3 vote — not cryptographic proof — a coalition of three cooperating faulty modules out of seven can quarantine any correct module without genuine evidence.

For a safety-critical power-electronics cluster, quarantining a healthy charger module is a physical safety event (power delivery interrupted, possibly unsafe load distribution). This weakness must be addressed before deployment.

---

### 3.2 [HIGH] No message source authentication — voter impersonation possible

**File:** `src/ekk_module.c`, lines 153–158; `src/ekk_consensus.c`, lines 444–533

`voter_id` in `ekk_consensus_on_vote` is taken directly from `vote_msg->voter_id` in the received message. There is no MAC, nonce, or hardware-bound credential. A module that guesses the IDs in a ballot's `eligible_voters` list can cast votes under any eligible voter's identity. Combined with the lack of duplicate-vote replay protection across separate ballot rounds (the `EKK_VOTE_ABSTAIN` check is per-ballot, not per-node per-time), this allows a single compromised module to decide any ballot unilaterally if it can craft messages faster than legitimate voters.

The claim of "Byzantine fault tolerance" in the header comments (`ekk.h:35`) is therefore not substantiated by the message-handling code. The consensus protocol is Byzantine-tolerant in its vote-counting logic but not Byzantine-secure at the network layer.

---

## 4. Verification-Readiness Gaps

The l4v compatibility audit (`project_l4v_compat_audit.md`) already documents 9 hard blockers and 14 adaptations. The following are **additional** findings not in that audit.

### 4.1 `ekk_heartbeat_tick` division by zero not proven unreachable

**File:** `src/ekk_heartbeat.c`, line 215

```c
uint32_t missed = (uint32_t)(elapsed / hb->config.period);
```

There is no precondition check that `hb->config.period > 0`. The AutoCorres-lifted version of this function will require an explicit precondition `period ≠ 0` in any HOL proof. The existing Isabelle scaffold does not mention this. The ACSL contract (if written for Frama-C) must also carry `\requires config->period > 0`. This is not a bug in the POSIX simulation (default config uses a non-zero period) but it is a verification gap.

### 4.2 `ekk_consensus_on_proposal` votes NO on resource exhaustion without caller knowledge

**File:** `src/ekk_consensus.c`, lines 570–575

```c
idx = allocate_ballot_slot(cons);
if (idx < 0) {
    send_vote(cons, proposer_id, ballot_id, EKK_VOTE_NO);
    return EKK_ERR_BUSY;
}
```

The function returns `EKK_ERR_BUSY` to its caller (the module's message dispatcher) but has already sent a NO vote to the proposer. The caller has no way to distinguish "I rejected the proposal on policy grounds" from "I voted NO because I ran out of ballot slots." The `EKK_VOTE_NO` resulting from resource exhaustion is semantically indistinguishable from a deliberate rejection. In the HOL proof of the consensus validity property ("APPROVED only when sufficient YES votes"), this branch must be shown to not violate the quorum threshold — which it doesn't in isolation, but it makes the system's vote distribution non-deterministic under load, which complicates TLA+ modeling.

### 4.3 `ekk_field_sample` seqlock read does not retry

**File:** `src/ekk_field.c`, lines 146–150, 172–174

```c
uint32_t seq_before = cf->sequence;
if (seq_before & 1) {
    return EKK_ERR_BUSY;   /* no retry */
}
...
if (cf->sequence != seq_before) {
    return EKK_ERR_BUSY;   /* no retry */
}
```

The function never retries on torn reads — it always returns `EKK_ERR_BUSY` to the caller. The header documents `ekk_field_sample_consistent` with retry support, but `ekk_field_sample_neighbors` (called from `ekk_module_tick`) calls `ekk_field_sample` directly (line 218), not the consistent variant. Under high publish rates, `ekk_field_sample` returns `EKK_ERR_BUSY` most of the time and the module ticks with stale or zero aggregate. The header documents this API split but the module tick wires up the non-retrying path.

### 4.4 Weak symbol semantics unverifiable by AutoCorres

**File:** `src/ekk_module.c`, lines 670, 731

```c
EKK_WEAK ekk_task_id_t ekk_module_select_task(ekk_module_t *mod) { ... }
EKK_WEAK ekk_vote_value_t ekk_module_decide_vote(...) { ... }
```

Weak symbols have no proof obligation in the base theory — they can be overridden by any linked translation unit. AutoCorres proofs about these functions prove only the default implementation. Any overriding implementation receives no verification. The verification plan should note that proofs about `ekk_module_tick` calling `ekk_module_select_task` are conditional on the override (if any) satisfying the same postconditions. Consider factoring the verifiable logic out of the weak function body into a non-weak helper, so the proof target is always concrete.

---

## 5. Code Hygiene

### 5.1 [RESOLVED] `heartbeat_msg_t.state` now follows heartbeat engine state

**File:** `src/ekk_heartbeat.c`, line 267

```c
ekk_heartbeat_msg_t msg = {
    ...
    .state = EKK_MODULE_ACTIVE,  /* Will be set by caller if needed */
```

This finding applied to the earlier extract state. The runtime now populates the
heartbeat message from `hb->current_state`, so the issue is no longer current.

### 5.2 [LOW] `avg_heartbeat_gap_us` EMA overflow unchecked

**File:** `src/ekk_heartbeat.c`, line 203

```c
neighbor->avg_heartbeat_gap_us =
    (neighbor->avg_heartbeat_gap_us * 7 + gap_us) / 8;
```

`avg_heartbeat_gap_us` is `uint64_t`. `avg_heartbeat_gap_us * 7` overflows if
`avg_heartbeat_gap_us > UINT64_MAX / 7` ≈ 2.6 × 10¹⁸ µs ≈ 82 years. In practice
this cannot happen with monotonic clocks, but the overflow is silent (unsigned
wrap, no assert). A one-line clamp or a pre-multiply check costs nothing.

### 5.3 [RESOLVED] `ekk_module_tick` now publishes with the caller's logical `now`

**File:** `src/ekk_module.c`

```c
err = ekk_field_publish_at(mod->id, &mod->my_field, now);
```

This finding applied before the time-model cleanup. The runtime now publishes
fields with the same logical `now` used by the rest of the tick.

### 5.4 [LOW] POSIX HAL message queue does not filter by `dest_id`

**File:** `src/hal/ekk_hal_posix.c`, function `ekk_hal_recv` (lines 212–248)

Any caller of `ekk_hal_recv` drains messages regardless of `dest_id`. In a multi-module POSIX simulation (multiple `ekk_module_t` structs calling `ekk_module_tick` from separate threads), messages intended for module A are consumed by module B. This is not a bug in the production MCU path, but it makes multi-module integration tests on POSIX silently wrong. The HAL needs per-module receive queues, or a filter on `sender_id`/`dest_id` at dequeue time, before multi-module POSIX simulation is trustworthy.

---

## 6. Things Done Right

The following properties are explicitly confirmed and should be preserved:

- **No `malloc`/`free`** anywhere in the core. All allocation is static or on the stack. This is a hard invariant for the verification path.
- **All loops are bounded** by fixed constants (`EKK_MAX_MODULES`, `EKK_K_NEIGHBORS`, `EKK_MAX_BALLOTS`, etc.).
- **All pointer arguments are NULL-checked** at the top of every public function before dereference.
- **No recursion.** The topology reelection calls from the heartbeat callback chain (`on_neighbor_dead_cb → ekk_topology_on_neighbor_lost → ekk_topology_reelect`) do not call back into heartbeat or consensus — no cycles in the call graph.
- **Fixed-point Q16.16 throughout core.** No floating-point in any path that will be AutoCorrected. The `EKK_VERIFICATION` guard correctly excludes float macros.
- **Seqlock pattern is architecturally correct.** The writer protocol (odd → copy → even) and reader protocol (check even → copy → verify unchanged) are sound. The defects found above are implementation flaws, not design flaws.
- **`ekk_fixed_mul` is correctly saturating.** The pattern should be replicated in `ekk_fixed_div`.
- **Static assertions in `ekk_types.c`** provide compile-time layout guarantees. These are exactly the kind of ground truth that AutoCorres proofs can depend on.
- **The l4v compatibility audit is thorough and honest** about blockers. The `EKK_VERIFICATION` preprocessor guard cleanly isolates incompatible features. The `ekk_stdint.h` shim approach is the right strategy.

---

## 7. Priority Action List

| # | Severity | File / Line | Action |
|---|----------|-------------|--------|
| 1 | CRITICAL | `ekk_consensus.c:822` | Replace unconditional accept of `INVALID_MAC` evidence with explicit rejection pending proper MAC verification infrastructure |
| 2 | CRITICAL | `ekk_field.c:360–368` | Fix GC seqlock write: CAS on sequence before invalidation, abort if sequence changed since read |
| 3 | HIGH | `ekk_types.c:47` | Add saturation to `ekk_fixed_div` (same pattern as `ekk_fixed_mul`) |
| 4 | HIGH | `ekk_consensus.c:444` | Document (or enforce) that voter_id is caller-controlled and unauthenticated; update Byzantine claim in header comments accordingly |
| 5 | HIGH | `ekk_field.c:98` | Fix `cf->field.sequence` write: copy the post-write even value, not the mid-write odd value |
| 6 | MEDIUM | `ekk_module.c:~97` | Implement the `→ EKK_MODULE_REFORMING` transition trigger or remove the state from the enum |
| 7 | MEDIUM | `ekk_field.c:218` | Wire `ekk_module_tick` through `ekk_field_sample_consistent` (with retry) rather than bare `ekk_field_sample` |
| 8 | MEDIUM | `ekk_heartbeat.c:215` | Add `EKK_ASSERT(hb->config.period > 0)` and document as AutoCorres precondition |
| 9 | LOW | `ekk_hal_posix.c` | Add per-module receive queues or dest_id filtering before multi-module POSIX tests |
| 10 | LOW | `ekk_heartbeat.c:267` | Propagate actual module state into heartbeat message `.state` field |

---

*End of review.*
