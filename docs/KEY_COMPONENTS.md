<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
<!-- Copyright (c) 2026 Elektrokombinacija -->
# Key C Components

This extract is intentionally small. These are the files worth showing first if the goal is to discuss concepts rather than every implementation detail.

## 1. Module Coordination Loop

- [ekk_module.h](../include/ekk/ekk_module.h)
- [ekk_module.c](../src/ekk_module.c)

Why it matters:

- this is the "module-first" framing
- there is no single global scheduler object
- each module owns tasks, topology, heartbeat state, consensus state, and published field data
- the main tick is the easiest place to discuss what belongs in a userspace service versus what definitely does not belong in a kernel

The important sequence in `ekk_module_tick()` is:

1. process incoming messages
2. update heartbeat state
3. update topology
4. sample neighbor fields
5. compute gradients
6. update consensus
7. refresh slack/deadline state
8. select a local task
9. publish the updated field at the tick's logical time
10. refresh module state

`ekk_module_tick()` is also where the extract's three-way outcome contract lives:

- `EKK_OK`
- `EKK_ERR_DEGRADED`
- hard error

## 2. `k=7` Topology

- [ekk_topology.h](../include/ekk/ekk_topology.h)
- [ekk_topology.c](../src/ekk_topology.c)

Why it matters:

- this is where the fixed-cardinality neighbor idea lives
- it is the clearest trace of the starling-inspired `k=7` thinking
- it is also probably the easiest part to reinterpret as a userspace topology service in `Microkit`

The most relevant function is `ekk_topology_reelect()`, which recomputes the current neighbor set from known modules.
`on_discovery()` now uses an event-driven `beats-current-worst` rule, so the target model is "current `k`-nearest" without a full reelection on every new discovery message.

## 3. Decaying Coordination Fields

- [ekk_field.h](../include/ekk/ekk_field.h)
- [ekk_field.c](../src/ekk_field.c)

Why it matters:

- this is the core "potential field" idea
- modules publish field values into a shared region
- neighbors sample those values with temporal decay
- gradients are computed as `neighbor - self`

This is also the best candidate for the question:

"Is this really a kernel primitive, or just a shared-memory coordination mechanism?"

## 4. Heartbeat And Failure Detection

- [ekk_heartbeat.h](../include/ekk/ekk_heartbeat.h)
- [ekk_heartbeat.c](../src/ekk_heartbeat.c)

Why it matters:

- this encodes `UNKNOWN -> ALIVE -> SUSPECT -> DEAD`
- it is where neighbor liveness changes feed back into the module
- in this extract, callbacks are instance-local instead of global, which makes the code much safer to discuss and run

## 5. Threshold Consensus

- [ekk_consensus.h](../include/ekk/ekk_consensus.h)
- [ekk_consensus.c](../src/ekk_consensus.c)

Why it matters:

- this is the clearest expression of the threshold-voting idea
- it lets you talk concretely about ballots, votes, thresholds, inhibition, and completion
- it is another good example of something that might be useful as a userspace policy engine rather than as a kernel concept

Important scope note:

- this extract implements per-ballot threshold voting over a bounded electorate snapshot
- it does not claim to solve general multi-proposer contention or global proposal serialization

## 6. POSIX HAL For Local Discussion

- [ekk_hal.h](../include/ekk/ekk_hal.h)
- [ekk_hal_posix.c](../src/hal/ekk_hal_posix.c)

Why it matters:

- gives just enough runtime support to build and run the extract locally
- includes a host-side `ekk_hal_set_module_id()` helper so multiple logical modules can run inside one process demo
- keeps the discussion grounded in working code rather than static snippets
