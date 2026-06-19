#!/bin/sh
# Parseability gate for the l4v / AutoCorres verification track.
#
# Compiles the verified core under -DEKK_VERIFICATION with -fsyntax-only. This
# is the gcc-level proxy from docs/L4V_PARSEABILITY_PLAN.md Phase 1: it confirms
# the EKK_VERIFICATION path is well-formed C and stays consistent with the
# parser-safe definitions (no struct field guarded out while still used, no
# stray GCC-only construct that the guards were meant to replace).
#
# It is NOT the l4v C parser itself. The real parse still needs a local l4v +
# Isabelle install and the docs/isabelle session.
#
# The POSIX HAL is excluded by design (uses pthreads/clock_gettime/builtins).
set -eu

cc=${CC:-gcc}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Verified core translation units; src/hal/ekk_hal_posix.c is out of scope.
set -- \
    "$root/src/ekk_types.c" \
    "$root/src/ekk_init.c" \
    "$root/src/ekk_field.c" \
    "$root/src/ekk_heartbeat.c" \
    "$root/src/ekk_consensus.c" \
    "$root/src/ekk_module.c" \
    "$root/src/ekk_topology.c"

"$cc" -DEKK_VERIFICATION -I"$root/include" -fsyntax-only "$@"
echo "EKK_VERIFICATION parse gate: OK ($# core TUs, $cc -fsyntax-only)"
