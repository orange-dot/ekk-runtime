<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
<!-- Copyright (c) 2026 Elektrokombinacija -->
# EK-KOR Agentic Experiment - Runtime Hardening Milestone

Date: 2026-03-18

This note marks a clean cut after the `ekkor-agentic-experiment` hardening pass.

## Scope Completed

The following work is now complete:

- consensus ballot identity normalization
- topology `k`-nearest reelection fix
- removal of hidden shared topology scratch state
- explicit `module_tick()` error model
- single-tick logical time cleanup
- metric model refactor
- targeted regression tests for the main failure modes
- API and docs alignment with the current runtime behavior

## Current Runtime Contract

The extract now has a narrower and more explicit contract:

- the runtime is single-threaded and non-reentrant by design
- `ekk_consensus` implements per-ballot threshold voting over a bounded electorate snapshot
- canonical ballot identity is `(proposer_id, ballot_id)`
- `ekk_topology` targets the current `k`-nearest set under the configured metric
- topology reelection is event-driven, not continuous
- `ekk_module_tick()` can return `EKK_OK`, `EKK_ERR_DEGRADED`, or a hard error
- metric counters are split by meaning rather than mixed into umbrella totals

## What Improved

- multi-proposer ballot ID collisions no longer alias each other
- better topology candidates can displace the current farthest neighbor
- topology reelection no longer depends on hidden mutable static scratch state
- tick-time logic uses one authoritative `now` across sampling, scheduling, and publish
- degraded outcomes are visible to the caller instead of being silently swallowed
- metrics now separate:
  - degraded vs hard tick outcomes
  - field sample vs publish failures
  - neighbor-set changes vs discovery broadcasts vs heartbeat state changes
  - ballot start/completion/cancel/timeout outcomes
- docs no longer overstate the extract as a kernel or a full protocol stack

## Verification State

The runtime and test scaffold were rechecked at this cut with:

```bash
cmake -S . -B build
cmake --build build -j4
cd build && ctest --output-on-failure
build/ekx_tests
```

At this milestone, the test suite is green.

## Known Non-Blockers

These are still worth remembering, but they are not blockers for this cut:

- the topology changed callback currently uses a `container-of` style recovery in the module integration layer
- some older analytical and backlog documents intentionally still mention rejected terminology while explaining the design history
- the `EKK_DISTANCE_LATENCY` path remains a placeholder and currently falls back to logical distance

## Good Next Starts

Any of these would be a clean next phase:

- a fresh external-style review pass over the hardened runtime
- mapping the runtime concepts into `Microkit` / `LionsOS` terms
- starting the next implementation track without reopening this slice

## Cut Decision

This is a good stopping point for the current hardening line.
