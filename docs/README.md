<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
<!-- Copyright (c) 2026 Elektrokombinacija -->

# Docs

Use this directory as the documentation entrypoint for `ekk-runtime`.

## Start Here

- `KEY_COMPONENTS.md` - quickest subsystem map for the runtime
- `SEL4_REVIEW.md` - current correctness and hardening gap register
- `MILESTONE_RUNTIME_HARDENING.md` - implementation hardening sequence

## Verification Track

- `VERIFICATION_PLAN.md` - proof scope and intended milestones
- `L4V_PARSEABILITY_PLAN.md` - parseability and AutoCorres readiness plan
- `isabelle/` - Isabelle/AutoCorres session and proof-side C mirrors

## Reading Order

1. Read the root `README.md`.
2. Read `KEY_COMPONENTS.md`.
3. Read `SEL4_REVIEW.md`.
4. Only then dive into the verification track if you need proof-layer detail.
