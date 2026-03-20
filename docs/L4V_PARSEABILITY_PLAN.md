<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
<!-- Copyright (c) 2026 Elektrokombinacija -->
# l4v Parseability Plan for `ekkor-agentic-experiment`

**Goal:** Make the core C code fully parseable and liftable by AutoCorres, on the
same basis as the seL4 kernel. The POSIX HAL is excluded from verification scope.
No new features. No logic changes. Pure mechanical transformation.

---

## Audit summary

The l4v C parser rejects a well-defined subset of C. The code has four hard
blocker categories, one adaptation category, and a substantial compliant base.

### Already compliant (keep as-is)

- No `malloc`/`free` — all structs are caller-allocated
- No recursion — call graph is a DAG
- No VLAs — all arrays are compile-time bounded
- Bounded loops — all `for` loops iterate over fixed-size arrays (`EKK_MAX_MODULES`,
  `EKK_K_NEIGHBORS`, `EKK_FIELD_COUNT`, `EKK_MAX_BALLOTS`)
- Consistent error returns via `ekk_error_t`
- Clean header / implementation split
- `EKK_STATIC_ASSERT` already wraps `_Static_assert` behind a macro
- No `goto`, no computed gotos
- No pointer arithmetic beyond array indexing

---

## Hard blockers (prevent parsing)

### B1 — Statement expressions in `EKK_MIN` / `EKK_MAX`

**File:** `include/ekk/ekk_types.h:486-487`

```c
// CURRENT (GCC extension — hard blocker)
#define EKK_MIN(a, b) __extension__({ __typeof__(a) _a = (a); ... })
#define EKK_MAX(a, b) __extension__({ __typeof__(a) _a = (a); ... })
```

The l4v parser does not accept statement expressions (`({ ... })`), `__typeof__`,
or `__extension__`. These are parsed before any C99 rule applies.

**Fix:** Revert to plain ternary. Accept the double-evaluation limitation — in
the verified core, all call sites use pure expressions with no side effects.
Document this as a precondition on the macro.

```c
#define EKK_MIN(a, b)  ((a) < (b) ? (a) : (b))
#define EKK_MAX(a, b)  ((a) > (b) ? (a) : (b))
```

Call-site audit required: confirm no call site passes an expression with side
effects (function calls, `++`, etc.). There are none in the current codebase.

---

### B2 — GCC atomic and barrier builtins

**Files:**
- `include/ekk/ekk_hal.h` — `ekk_hal_atomic_inc`, `ekk_hal_memory_barrier`
- `src/ekk_field.c` — call sites of both

```c
// CURRENT (hard blockers)
#define ekk_hal_atomic_inc(ptr)    __atomic_fetch_add((ptr), 1, __ATOMIC_RELAXED)
#define ekk_hal_memory_barrier()   __sync_synchronize()
```

The parser rejects `__atomic_*`, `__sync_*`, and all `__builtin_*`.

**Fix — two-layer approach:**

The seqlock pattern depends on these operations. For verification purposes the
memory model is sequential — barriers and atomics are no-ops in the proof.
Introduce a `EKK_VERIFICATION` guard:

```c
#ifdef EKK_VERIFICATION
  /* Sequential memory model for l4v — barriers and atomics are identity ops */
  #define ekk_hal_atomic_inc(ptr)   (++(*(ptr)))
  #define ekk_hal_memory_barrier()  ((void)0)
#else
  #define ekk_hal_atomic_inc(ptr)   __atomic_fetch_add((ptr), 1, __ATOMIC_RELAXED)
  #define ekk_hal_memory_barrier()  __sync_synchronize()
#endif
```

This is honest: the proof verifies functional correctness under a sequential
model, not concurrent safety. The concurrent safety argument lives separately
(seqlock correctness by construction, not by AutoCorres proof).

---

### B3 — `bool` / `stdbool.h`

**Files:** `include/ekk/ekk_types.h`, every header that includes it, and
`src/ekk_consensus.c`, `src/ekk_field.c`, `src/ekk_module.c`, `src/ekk_topology.c`

seL4 does not use C99 `bool`. It defines `bool_t` as `word_t` (unsigned machine
word). The l4v parser sees `bool` from `<stdbool.h>` as an unknown type unless
the header is provided in a parser-compatible form.

**Fix:** Define `ekk_bool_t` as `unsigned int`. Replace all `bool` in verified
headers and sources with `ekk_bool_t`, `EKK_TRUE`/`EKK_FALSE` instead of
`true`/`false`. Keep `<stdbool.h>` only in the POSIX HAL.

```c
/* In ekk_types.h, under EKK_VERIFICATION guard: */
#ifdef EKK_VERIFICATION
  typedef unsigned int ekk_bool_t;
  #define EKK_TRUE  1u
  #define EKK_FALSE 0u
#else
  #include <stdbool.h>
  typedef bool ekk_bool_t;
  #define EKK_TRUE  true
  #define EKK_FALSE false
#endif
```

Scope of change: ~40 occurrences across 6 files.

---

### B4 — `EKK_FLOAT_TO_FIXED` macro uses `float` arithmetic

**File:** `include/ekk/ekk_types.h`

```c
// CURRENT (hard blocker — float not supported by l4v parser in expressions)
#define EKK_FLOAT_TO_FIXED(f)  ((ekk_fixed_t)((f) * 65536.0f))
```

The parser rejects floating-point literals and arithmetic.

**Fix:** The macro is only used to define named constants (e.g., `EKK_FIXED_HALF`,
`EKK_FIXED_ONE`). Replace all uses with pre-computed integer literals.

```c
// CURRENT
#define EKK_FIXED_ONE    EKK_FLOAT_TO_FIXED(1.0f)   // = 65536
#define EKK_FIXED_HALF   EKK_FLOAT_TO_FIXED(0.5f)   // = 32768
#define EKK_FIXED_ZERO   EKK_FLOAT_TO_FIXED(0.0f)   // = 0

// REPLACE WITH
#define EKK_FIXED_ONE    65536
#define EKK_FIXED_HALF   32768
#define EKK_FIXED_ZERO   0
```

After replacing all uses, remove `EKK_FLOAT_TO_FIXED` from verified headers
(keep in a non-verified compatibility header if needed for test code).

---

## Requires adaptation (parseable with changes)

### A1 — `int64_t` / `uint64_t` — include path

**Files:** `include/ekk/ekk_types.h` includes `<stdint.h>`

The l4v parser can handle `<stdint.h>` types but requires the header to be
provided in a parser-compatible form (the system `<stdint.h>` may include
compiler-specific extensions the parser rejects).

**Fix:** Mirror the seL4 approach — provide a minimal `ekk_stdint.h` stub for
verification builds that defines only the types actually used (`uint8_t`,
`uint16_t`, `uint32_t`, `uint64_t`, `int8_t`, `int16_t`, `int32_t`, `int64_t`
as plain C typedef aliases of `unsigned char`, etc.). Use `EKK_VERIFICATION`
guard to select the stub vs system `<stdint.h>`.

---

### A2 — Function pointers (callbacks)

**Files:** `include/ekk/ekk_consensus.h`, `include/ekk/ekk_heartbeat.h`,
`include/ekk/ekk_module.h`

The parser accepts function pointer types. AutoCorres lifts them as HOL function
pointer values. However, verification through a function pointer requires either:
(a) case-splitting on all possible targets, or
(b) treating the callback as an abstract function with a stated contract.

**Fix:** No code change required for parseability. For verification, document
each callback site with an ACSL-style precondition comment stating the contract
the callback must satisfy. AutoCorres proofs will abstract over the callback.

---

### A3 — `void *` callback contexts

**Files:** `include/ekk/ekk_consensus.h` (`decide_context`, `complete_context`),
`include/ekk/ekk_heartbeat.h`

`void *` is parseable but creates a verification gap — the type of the pointed-to
object is unknown to the proof.

**Fix:** No code change required for parseability. Proofs will treat context
pointers as opaque. Document as out-of-scope for B2/B3 proof targets (which
don't use context pointers anyway — `evaluate_ballot` and `health_step` are
pure functions with no callbacks).

---

### A4 — `static` local variable in `ekk_topology_reelect`

**File:** `src/ekk_topology.c:308`

```c
static distance_entry_t entries[EKK_MAX_MODULES];
```

Parseable, but AutoCorres treats static locals as global state. The proof
will need a frame condition stating this function modifies `entries`. Not a
blocker but requires awareness when writing the topology theory.

---

### A5 — `_Static_assert` via `EKK_STATIC_ASSERT`

**File:** `include/ekk/ekk_types.h`

The l4v parser supports `_Static_assert` as of recent versions (it is in seL4
itself). Verify during B1 toolchain validation — if it fails, wrap in
`#ifndef EKK_VERIFICATION`.

---

## POSIX HAL — excluded entirely

**File:** `src/hal/ekk_hal_posix.c`

Contains `<pthread.h>`, `<time.h>`, `<sys/time.h>`, `clock_gettime`, mutex
operations. None of this is part of the verification target.

**Fix:** Exclude from AutoCorres via `roots=` scoping in the Isabelle ROOT file.
The HAL is tested by the existing test suite, not by proof.

---

## Transformation plan

### Phase 1 — Mechanical (unblocks parsing, ~half day)

All changes go behind `EKK_VERIFICATION` guard or are unconditionally safe
(pre-computed constants, ternary macros).

| # | Change | File | Risk |
|---|--------|------|------|
| 1.1 | Replace `EKK_MIN`/`EKK_MAX` with plain ternary (unconditional) | `ekk_types.h` | None — no call site has side effects |
| 1.2 | Pre-compute `EKK_FIXED_*` constants as integer literals | `ekk_types.h` | None — values are identical |
| 1.3 | Remove `EKK_FLOAT_TO_FIXED` from verified headers | `ekk_types.h` | None |
| 1.4 | Add `EKK_VERIFICATION` guard for atomics/barriers | `ekk_hal.h` | Sequential model only |
| 1.5 | Add `ekk_bool_t` / `EKK_TRUE` / `EKK_FALSE` with guard | `ekk_types.h` | ~40 replacements |
| 1.6 | Add minimal `ekk_stdint.h` stub for verification builds | new file | Mechanical |
| 1.7 | Add `EKK_VERIFICATION` guard over `<stdbool.h>` include | `ekk_types.h` | Mechanical |

After Phase 1: `gcc -DEKK_VERIFICATION -Iinclude -fsyntax-only src/*.c` must
pass clean. Then attempt C parser run on `ekk_types.c` and `ekk_heartbeat.c`.

---

### Phase 2 — l4v session scaffold (~2h)

Create `docs/isabelle/` layout:

```
docs/isabelle/
  ROOT                  -- session EkkVerification
  EkkTypes.thy          -- shared typedef imports via AutoCorres
  EkkHeartbeat.thy      -- B1 toolchain validation target
  EkkConsensus.thy      -- B2 evaluate_ballot target
```

ROOT should scope `roots=` to exclude HAL and examples. Start with the seL4
`autocorres` examples in `<l4v>/tools/autocorres/tests/` as template.

---

### Phase 3 — Proof targets (per VERIFICATION_PLAN.md)

Phase 3 follows the existing VERIFICATION_PLAN.md Track B exactly:
B1 → `ekk_heartbeat_init` postcondition (toolchain check),
B2 → `evaluate_ballot` correctness,
B3 → `health_step` refinement.

Phase 1 and 2 are prerequisites. Phase 3 is unchanged.

---

## What this does NOT do

- Does not change any logic, algorithm, or protocol behavior
- Does not make the seqlock concurrently safe under the HOL proof (that is a
  separate argument, not AutoCorres scope)
- Does not verify the POSIX HAL
- Does not require Frama-C (still optional per VERIFICATION_PLAN.md)

---

## Build compatibility

After Phase 1, two build modes exist:

```bash
# Normal build (unchanged behavior)
cmake -S . -B build && cmake --build build

# Verification build (for C parser / l4v)
gcc -DEKK_VERIFICATION -Iinclude -fsyntax-only src/ekk_types.c src/ekk_heartbeat.c src/ekk_consensus.c src/ekk_field.c
```

The `EKK_VERIFICATION` flag is the single switch that activates all parser-safe
definitions. It does not affect the non-verification build in any way.
