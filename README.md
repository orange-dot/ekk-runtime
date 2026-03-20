<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
<!-- Copyright (c) 2026 Elektrokombinacija -->

# ekk -- Coordination Runtime

A small C coordination runtime (~7100 lines) implementing decaying shared fields,
k=7 topological neighbor election, heartbeat-based failure detection, and threshold
consensus. Written with seL4-grade coding discipline: no dynamic allocation, no
recursion, bounded loops, static configuration. Includes a working
Isabelle/AutoCorres verification session.

## Components

- **Field engine** (`ekk_field`) -- Decaying shared coordination fields with seqlock-protected publish/sample and gradient computation.
- **Topology** (`ekk_topology`) -- Event-driven k-nearest neighbor election under configurable distance metrics.
- **Heartbeat** (`ekk_heartbeat`) -- Liveness tracking with UNKNOWN/ALIVE/SUSPECT/DEAD state machine and inter-arrival timing.
- **Consensus** (`ekk_consensus`) -- Per-ballot threshold voting with early rejection, mutual inhibition, and Byzantine evidence framework.
- **Module** (`ekk_module`) -- Module-local coordination loop composing all subsystems into a single tick with three-way outcome contract.

## Building

POSIX test build (Linux, macOS):

```bash
cmake -S . -B build
cmake --build build
cd build && ctest --output-on-failure
```

## Verification

The `docs/isabelle/` directory contains an Isabelle/AutoCorres session that proves
properties of the heartbeat state machine and consensus voting logic. To build it,
you need:

- [Isabelle](https://isabelle.in.tum.de/) (2025 or later)
- [l4v](https://github.com/seL4/l4v) with AutoCorres

Then run:

```bash
<isabelle>/bin/isabelle build \
  -d <l4v> \
  -d docs/isabelle \
  EkkVerification
```

Replace `<isabelle>` and `<l4v>` with paths to your local installations.

## Known Issues

See [docs/SEL4_REVIEW.md](docs/SEL4_REVIEW.md) for a detailed systems review
covering correctness defects, undefined-behaviour paths, and verification-readiness
gaps.

## License

- Code (`.c`, `.h`, `.thy`, `ROOT`): MIT
- Documentation (`.md`): CC-BY-SA-4.0

Copyright (c) 2026 Elektrokombinacija
