#!/usr/bin/env bash
#
# build.sh - compile the project with a logged build
#
# Usage:
#   ./scripts/build.sh            core tools: mincut_lattice, e2e_solve, e2e_solve_gpu, wcsp_collapse
#   ./scripts/build.sh --gates    core tools + correctness gates: compare_kernels, cut_audit, kmf_test
#

set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

GATES=0
[ "${1:-}" = "--gates" ] && GATES=1

mf_init build
record_env "$OUT/env_build.txt"

BINS="e2e_solve e2e_solve_gpu mincut_lattice wcsp_collapse"
[ "$GATES" = "1" ] && BINS="$BINS compare_kernels cut_audit kmf_test"

# shellcheck disable=SC2086
rm -f $BINS
build_core
[ "$GATES" = "1" ] && build_gates

hr
say "--- build result ---"
for b in $BINS; do
  if [ -x "$b" ]; then say "  ok       $b"; else say "  MISSING  $b   (compiler errors are in $RUNLOG)"; fi
done
if [ -n "${CPX_FLAGS:-}" ]; then
  say "  cplex    compiled in (--kernelizer cplex and compare_kernels cplex available)"
else
  say "  cplex    NOT compiled in (CPLEX_HOME unset or cplex.h not found)"
fi
say ""
say "log: $RUNLOG"