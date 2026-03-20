<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
<!-- Copyright (c) 2026 Elektrokombinacija -->
# Verification Plan

Two orthogonal verification tracks, not three sequential levels.

TLA+ and Isabelle answer fundamentally different questions and can proceed
independently. Frama-C is available as an optional iteration tool during
Isabelle work but is not a required stage.

There is no deadline. The goal is to learn where the ideas hold and where
the assumptions are wrong.

---

## Why not everything in Isabelle

Isabelle and TLA+ address different questions:

- **Isabelle + AutoCorres** asks: *does this C function compute what the
  abstract spec says?* It reasons about individual functions, sequential
  execution, and memory.

- **TLA+** asks: *does this distributed protocol have the safety and liveness
  properties we claim, regardless of message ordering?* It reasons about
  concurrent state across multiple modules over time.

seL4 itself does not use TLA+ because the kernel is a single-node sequential
program. This project is different — the coordination protocol is distributed
by design. The question "will a module that stops sending eventually be
declared dead by all of its neighbors" is a liveness property over a
distributed system. TLA+'s temporal logic (`<>`, `[]`) handles this naturally.
Encoding the same property in Isabelle/HOL is possible but significantly
harder.

The two tracks are therefore complementary, not redundant.

---

## Track A — TLA+ Protocol Model

**Question:** Do the heartbeat and consensus protocols have the safety and
liveness properties we intend, independent of any C implementation?

**Tool:** TLA+ with TLC model checker. Java is already installed — no further
setup required beyond the TLA+ VS Code extension or Toolbox.

**File layout:**
```
docs/tla/
  Heartbeat.tla       -- heartbeat state machine
  HeartbeatMC.tla     -- TLC model config (symmetry sets, state constraints)
  Consensus.tla       -- threshold voting protocol
  ConsensusMC.tla
  Topology.tla        -- k-nearest election (stretch goal)
```

### Phase A1 — Heartbeat state machine

Model a single module's view of its neighbors. Each neighbor is a TLA+
variable with state in `{UNKNOWN, ALIVE, SUSPECT, DEAD}`. The heartbeat
tick is an action. The re-discovery message is a separate action.

Properties to check with TLC:

- *Safety:* health transitions only follow defined edges — no
  `UNKNOWN → SUSPECT` skip, no `DEAD → ALIVE` without re-discovery
  ```tla
  HealthTransition(from, to) ==
      \/ from = to
      \/ from = "UNKNOWN"  /\ to = "ALIVE"
      \/ from = "ALIVE"    /\ to = "SUSPECT"
      \/ from = "SUSPECT"  /\ to = "DEAD"
  ```

- *Liveness:* a module that stops sending will eventually reach `DEAD`
  ```tla
  <>(missed_count >= TIMEOUT_COUNT => health = "DEAD")
  ```
  This requires a fairness assumption on the tick action — make it explicit.

### Phase A2 — Consensus protocol

Model N modules, each holding a copy of the active ballot. Votes arrive
as TLA+ actions from any module to any other.

Properties to check:

- *Agreement:* no two modules can simultaneously observe `APPROVED` and
  `REJECTED` for the same ballot ID
- *Validity:* `APPROVED` is only reachable when `yes_count / total >= threshold`
  at the point all votes are counted
- *Termination:* once all neighbors have voted, the ballot reaches a terminal
  state in finite steps (under fairness)
- *Early rejection correctness:* the `max_ratio` early-exit in `evaluate_ballot`
  — if the impossibility condition fires, verify it is genuinely impossible
  to reach threshold with remaining votes

The early rejection property is particularly worth checking in TLA+. It is a
subtle arithmetic claim and the TLC counterexample finder is good at finding
off-by-one errors in threshold logic.

### Phase A3 — Topology election (stretch)

Model the k-nearest election. Simplify distance to an arbitrary total order
(concrete distances are not the interesting part). Check:

- Elected set has cardinality `<= k` at all times
- Self is never in the elected neighbor set
- After a module is lost, remaining modules re-elect within bounded steps
  (under fairness)

### Deliverable

TLC checks all properties without violations. A short note per property
describes any counterexample found during development and what it revealed
about the protocol assumptions.

---

## Track B — Isabelle / AutoCorres Refinement

**Question:** Do the C functions correctly implement their abstract
specifications?

**Tool:** Isabelle/HOL + AutoCorres. Required components:
```
<l4v>/tools/autocorres/
<l4v>/tools/c-parser/
<isabelle>/
```

**File layout:**
```
docs/isabelle/
  ROOT                  -- Isabelle session definition
  EkkTypes.thy          -- shared type definitions and lemmas
  EkkHeartbeat.thy      -- heartbeat abstract spec + AutoCorres import + proofs
  EkkConsensus.thy      -- consensus abstract spec + AutoCorres import + proofs
```

### How AutoCorres fits in

AutoCorres parses the C source (via the l4v C99 parser) and generates
Isabelle definitions in a shallow monadic embedding. Each C function becomes
an Isabelle constant. You then write an abstract spec in pure HOL — no
pointers, no fixed-point arithmetic, no C types — and prove that the lifted
constant refines it.

### Phase B1 — Toolchain validation

Start with `ekk_heartbeat_init`. The function is simple: memset to zero,
assign `my_id`, apply config. AutoCorres lifts it to a monadic Isabelle
function. Prove one trivial postcondition:

```isabelle
lemma ekk_heartbeat_init_sets_id:
  "\<lbrace> \<lambda>s. hb \<noteq> NULL \<rbrace>
   ekk_heartbeat_init' hb my_id config
   \<lbrace> \<lambda>r s. r = EKK_OK \<rbrace>"
```

The proof itself is not interesting. The point is to confirm the C parser
handles the ekk headers, AutoCorres generates sensible definitions, and
the session builds cleanly before attempting anything harder.

### Phase B2 — evaluate_ballot correctness

`evaluate_ballot` is the best early target for a real proof:
- No loops
- No aliasing or pointer arithmetic
- No global state
- Concrete correctness claim: APPROVED iff yes ratio meets threshold

Abstract spec in pure HOL:

```isabelle
fun eval_ballot :: "nat ⇒ nat ⇒ nat ⇒ nat ⇒ vote_result" where
  "eval_ballot yes total neighbors threshold =
     (if total < neighbors
      then
        let max_yes = yes + (neighbors - total) in
        if max_yes * 65536 < neighbors * threshold then Rejected
        else if yes * 65536 >= neighbors * threshold then Approved
        else Pending
      else
        if yes * 65536 >= neighbors * threshold then Approved
        else Rejected)"
```

The refinement proof must bridge the Q16.16 fixed-point arithmetic in the
C code (`(int64_t)yes_votes << 16) / neighbor_count`) to the integer
multiplication in the HOL spec. This is the hardest part and likely requires
a supporting lemma about the equivalence of the two comparisons under the
given bounds.

### Phase B3 — Heartbeat state transitions

The `set_neighbor_health` function implements the `UNKNOWN → ALIVE → SUSPECT
→ DEAD` state machine. Abstract spec:

```isabelle
datatype health = Unknown | Alive | Suspect | Dead

fun health_step :: "health ⇒ nat ⇒ nat ⇒ health" where
  "health_step Alive  missed timeout = (if missed ≥ timeout then Suspect else Alive)"
| "health_step Suspect missed timeout = (if missed ≥ timeout then Dead   else Suspect)"
| "health_step h      _      _       = h"
```

Prove that every call to `set_neighbor_health` in `ekk_heartbeat_tick`
corresponds to a valid `health_step` transition and that the transition
relation from Track A is preserved.

This is where the two tracks connect: the TLA+ safety property (no illegal
transitions) is re-proven at the C level in Isabelle.

### What is out of scope

- `ekk_module_tick` — composes too many subsystems, too large for an early target
- `ekk_hal_posix.c` — POSIX shim, not part of the interesting logic
- `ekk_topology` distance sorting — numerical approximation, not
  correctness-critical
- `ekk_field` gradient and decay arithmetic beyond monotonicity

### Deliverable

A session that builds cleanly with:
```bash
<isabelle>/bin/isabelle build \
  -d <l4v> \
  -d docs/isabelle \
  EkkVerification
```

Checked theories for `ekk_heartbeat_init`, `evaluate_ballot`, and
`health_step` refinement. Each theory states the abstract spec, imports the
C via AutoCorres, and proves the refinement lemma.

---

## Frama-C as an optional iteration tool

Frama-C / WP operates at the same level as Isabelle (C function correctness)
but uses SMT solvers instead of interactive proof. It is faster to iterate on —
write a contract, get feedback in seconds — but the result is weaker
("no counterexample found" vs "formally proved").

It is not a required stage. If Isabelle work stalls on a proof obligation,
Frama-C can quickly tell whether the contract is plausibly true or obviously
wrong, which saves time before re-entering Isabelle. The ACSL contracts also
serve as precise documentation of intended function behavior.

Install if needed:
```bash
opam install frama-c alt-ergo
```

---

## Relationship between the two tracks

```
Track A (TLA+)                     Track B (Isabelle)
─────────────────                  ──────────────────
Protocol properties                Function correctness
Distributed behavior               Sequential C code
Safety + liveness over time        Pre/post conditions + refinement
TLC finds counterexamples          Isabelle checks proofs

         ↕
Phase B3 connects them:
The TLA+ health transition safety property is re-proven
at the C level in Isabelle, closing the gap between
protocol intent and implementation.
```

---

## What not to verify

- `ekk_hal_posix.c` — POSIX shim, not part of the interesting logic
- `ekk_module_tick()` as a unit — verify the functions it calls instead
- The examples in `examples/` — test scaffolding, not logic
- Gradient and distance arithmetic beyond sign and monotonicity
