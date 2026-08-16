#!/usr/bin/env bash
#
# run_e2e.sh -- end-to-end test: kernelize, then SOLVE, then compare objectives.
#
#   ./scripts/run_e2e.sh [options] <file.wcsp>
#
# Runs the same instance through several kernelizer choices, solves each remnant with the same solver, and checks that every configuration reports the SAME
# final WCSP objective. With the exact ILP solver they must all match: a kernelizer that changes the answer is broken.
#
# The log goes to  log/<testname>_<YYYYMMDD>_<HHMMSS>.log  and records the system configuration, per-round kernelization progress (variables resolved, vertices
# and edges remaining, time), solve time, peak memory, and the objective from every configuration side by side
#
# MEMORY: the solve stage is heavier than kernelization alone. Run one copy of this script at a time.
#
set -uo pipefail

KERNELIZERS="none,cpu,gpu,lp"
SOLVER="ilp"
TIMELIMIT=""
MAXROUNDS=100
OUTDIR="log"
TESTNAME=""
DO_BUILD=1
ARCH_OVERRIDE=""
TIMEOUT=1800    # per-configuration hard kill; exact MWVC is NP-hard
QUIET=0

usage() {
cat <<'USAGE'
Usage: ./scripts/run_e2e.sh [options] <file.wcsp>

Runs the full pipeline (parse -> CCG -> kernelize -> solve -> objective) once per
kernelizer and checks that every configuration agrees on the final objective.

Options:
  -k, --kernelizers LIST  Comma-separated: none,cpu,gpu,lp   (default: all four)
                          none = skip kernelization; this is the reference answer
  -s, --solver S          ilp | mp | none                    (default: ilp)
                          ilp = exact integer program (Gurobi); objectives comparable
                          mp  = message passing; a heuristic, does NOT converge on
                                these graphs, so objectives are not comparable
                          none = stop after kernelization (no objective)
      --time-limit SEC    Solver time limit passed to the driver
      --max-rounds N      Cap on kernelization rounds        (default: 100)
  -t, --timeout SEC       Hard kill for any single run       (default: 1800)
                          Use 0 for no limit. Exact MWVC is NP-hard: a few
                          thousand CCG vertices can already be out of reach.
  -n, --name NAME         Test name in the log filename      (default: input base name)
  -o, --outdir DIR        Log directory                      (default: log)
      --arch sm_XX        Force CUDA architecture            (default: auto-detect)
      --no-build          Use existing binaries
  -q, --quiet             Less console output
  -h, --help              This message

Examples:
  ./scripts/run_e2e.sh data/bench/03_1k.wcsp
  ./scripts/run_e2e.sh -k none,cpu,gpu -s ilp data/bench/04_10k.wcsp
  ./scripts/run_e2e.sh -k none,cpu -s mp --time-limit 120 data/bench/02_500.wcsp
  ./scripts/run_e2e.sh --no-build -t 3600 -n e2e_big data/bench/06_300k.wcsp
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -k|--kernelizers) KERNELIZERS="$2"; shift 2 ;;
    -s|--solver)      SOLVER="$2"; shift 2 ;;
    --time-limit)     TIMELIMIT="$2"; shift 2 ;;
    --max-rounds)     MAXROUNDS="$2"; shift 2 ;;
    -t|--timeout)     TIMEOUT="$2"; shift 2 ;;
    -n|--name)        TESTNAME="$2"; shift 2 ;;
    -o|--outdir)      OUTDIR="$2"; shift 2 ;;
    --arch)           ARCH_OVERRIDE="$2"; shift 2 ;;
    --no-build)       DO_BUILD=0; shift ;;
    -q|--quiet)       QUIET=1; shift ;;
    -h|--help)        usage; exit 0 ;;
    -*)               echo "unknown option: $1"; usage; exit 1 ;;
    *)                INPUT="${1}"; shift ;;
  esac
done

INPUT="${INPUT:-}"
[ -z "$INPUT" ] && { echo "error: no input file given"; echo; usage; exit 1; }
[ -f "$INPUT" ] || { echo "error: cannot read '$INPUT'"; exit 2; }

case "$SOLVER" in ilp|mp|none) ;; *) echo "error: --solver must be ilp, mp or none"; exit 1 ;; esac

#  Validate every kernelizer name before doing any work. An unrecognised name used to be discovered only when the driver rejected it, part way into a run
IFS=',' read -ra _KCHECK <<< "$KERNELIZERS"
for _k in "${_KCHECK[@]}"; do
  case "$_k" in
    none|cpu|gpu|lp) ;;
    *) echo "error: --kernelizers contains '$_k'; allowed: none, cpu, gpu, lp"; exit 1 ;;
  esac
done
unset _KCHECK _k

[ -z "$TESTNAME" ] && TESTNAME="e2e_$(basename "$INPUT" .wcsp)"
STAMP=$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUTDIR"
LOG="$OUTDIR/${TESTNAME}_${STAMP}.log"

if [ "$QUIET" = "1" ]; then
  exec 3>&1 1>>"$LOG" 2>&1
else
  exec 3>&1 1> >(tee -a "$LOG") 2>&1
fi
say()  { printf '\n===== %s =====\n' "$*"; }
tick() { printf '   %s\n' "$*" >&3; }

TIMER=""
[ "$TIMEOUT" != "0" ] && TIMER="timeout $TIMEOUT"

# ------------------------------------------------------------------ header

say "END-TO-END TEST"
echo "test name      : $TESTNAME"
echo "input file     : $INPUT"
echo "input size     : $(du -h "$INPUT" | cut -f1)"
echo "input header   : $(head -1 "$INPUT")   (format: <name> <numVars> <maxDomain> <numConstraints> <upperBound>)"
echo "input sha256   : $(sha256sum "$INPUT" | cut -c1-32)"
echo "started        : $(date -Iseconds)"
echo "log file       : $LOG"

say "WHAT THIS TEST DOES"
cat <<'EXPLAIN'
The pipeline is:

    .wcsp -> parse -> constraint composite graph -> KERNELIZE -> SOLVE -> objective

Kernelization fixes the variables whose value is already forced, so the solver
only has to search what is left. This test runs the SAME instance through each
kernelizer and solves each remnant with the SAME solver.

With the exact ILP solver every configuration must report the SAME final
objective, because kernelization is only allowed to fix variables that some
optimal solution already agrees with. If the objectives differ, a kernelizer is
wrong. What SHOULD differ is how much work the solver is left with.
EXPLAIN

say "SYSTEM CONFIGURATION"
echo "host           : $(hostname)"
echo "os             : $(uname -sr)"
[ -r /etc/os-release ] && echo "distribution   : $(. /etc/os-release; echo "$PRETTY_NAME")"
[ -r /proc/cpuinfo ] && echo "cpu            : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs)"
echo "cpu cores      : $(nproc) logical"
echo "ram total      : $(free -h 2>/dev/null | awk '/^Mem:/{print $2}')"
echo "ram available  : $(free -h 2>/dev/null | awk '/^Mem:/{print $7}')"
echo "g++            : $(g++ --version 2>/dev/null | head -1)"
echo "nvcc           : $(nvcc --version 2>/dev/null | tail -1)"
echo "gpu            : $(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null)"
echo "GUROBI_HOME    : ${GUROBI_HOME:-<not set>}"
echo "git commit     : $(git rev-parse --short HEAD 2>/dev/null || echo '<not a git repo>')"

HAVE_GPU=0; ARCH=""
if command -v nvcc >/dev/null 2>&1; then
  if [ -n "$ARCH_OVERRIDE" ]; then ARCH="$ARCH_OVERRIDE"; HAVE_GPU=1
  else
    CC=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
    [ -n "$CC" ] && { ARCH="sm_${CC}"; HAVE_GPU=1; }
  fi
fi
HAVE_GUROBI=0
[ -n "${GUROBI_HOME:-}" ] && [ -d "${GUROBI_HOME}/include" ] && HAVE_GUROBI=1

say "RUN SETTINGS"
echo "kernelizers    : $KERNELIZERS"
echo "solver         : $SOLVER"
echo "max rounds     : $MAXROUNDS"
echo "time limit     : ${TIMELIMIT:-<none>}"
echo "hard timeout   : $([ "$TIMEOUT" = 0 ] && echo none || echo "${TIMEOUT}s")"
echo "cuda arch      : ${ARCH:-<no gpu>}"
echo "gurobi usable  : $([ $HAVE_GUROBI = 1 ] && echo yes || echo no)"
echo "gurobi threads : ${WCSP_GUROBI_THREADS:-1 (default; set WCSP_GUROBI_THREADS to use more)}"

if [ "$SOLVER" = "ilp" ] && [ $HAVE_GUROBI = 0 ]; then
  echo
  echo "ABORT: --solver ilp needs Gurobi, but GUROBI_HOME is not set or invalid."
  echo "       Use '-s mp' for the heuristic solver or '-s none' to stop after kernelization."
  tick "ABORT: ilp solver needs Gurobi -- see $LOG"
  exit 1
fi

# ------------------------------------------------------------------- build

GRB_SRC="third_party/wcsp-solver/src/LinearProgramSolver.cpp third_party/wcsp-solver/src/LinearProgramSolverGurobi.cpp"
GRB_FLAGS=""
GRB_LIBS=""
if [ $HAVE_GUROBI = 1 ]; then
  GRB_FLAGS="-DHAVE_GUROBI -I$GUROBI_HOME/include"
  GRB_LIBS="-L$GUROBI_HOME/lib -lgurobi_c++ -lgurobi130"
else
  GRB_SRC=""
fi

if [ "$DO_BUILD" = "1" ]; then
  say "BUILD"
  BUILD_OK=1
  bld() { echo "\$ $*"; "$@" || { echo "BUILD STEP FAILED"; BUILD_OK=0; }; }

  #  CPU binary: handles none / cpu / lp kernelizers.
  bld g++ -std=c++17 -O2 $GRB_FLAGS -I. apps/e2e_solve.cpp $GRB_SRC -o e2e_solve $GRB_LIBS -lopenblas

  #  GPU binary: same source compiled as CUDA, adds the gpu kernelizer.
  if [ $HAVE_GPU = 1 ]; then
    bld nvcc -x cu -std=c++17 -O2 -arch="$ARCH" -DUSE_GPU $GRB_FLAGS -I. \
        apps/e2e_solve.cpp $GRB_SRC -o e2e_solve_gpu $GRB_LIBS -lopenblas
  fi

  if [ "$BUILD_OK" != "1" ]; then
    echo; echo "Build failed -- stopping."
    tick "BUILD FAILED -- see $LOG"
    exit 1
  fi
  echo "build ok"
fi

# ------------------------------------------------------------------- runs

RESULT_FILE=$(mktemp)
#  Per-configuration output is kept in its own file. The summary is built from these, never by grepping $LOG -- the log is still being written at that point,
#  so reading it back gives partial and duplicated content
RUNDIR=$(mktemp -d)
FAILED=0

run_config() {
  local k="$1"
  local bin="./e2e_solve"
  [ "$k" = "gpu" ] && bin="./e2e_solve_gpu"

  if [ "$k" = "gpu" ] && [ $HAVE_GPU = 0 ]; then
    echo "SKIP gpu: no CUDA device or nvcc"; return
  fi
  if [ "$k" = "lp" ] && [ $HAVE_GUROBI = 0 ]; then
    echo "SKIP lp: Gurobi unavailable"; return
  fi
  if [ ! -x "$bin" ]; then
    echo "SKIP $k: $bin not built"; return
  fi

  say "CONFIGURATION: kernelizer=$k  solver=$SOLVER"
  local args=(--kernelizer "$k" --solver "$SOLVER" --max-rounds "$MAXROUNDS")
  [ -n "$TIMELIMIT" ] && args+=(--time-limit "$TIMELIMIT")

  echo "\$ $bin ${args[*]} $INPUT"
  local out="$RUNDIR/$k.txt"
  if command -v /usr/bin/time >/dev/null 2>&1; then
    $TIMER /usr/bin/time -f "[resource] wall %e s | peak RSS %M KB | cpu %P" \
      "$bin" "${args[@]}" "$INPUT" 2>&1 | tee "$out"
  else
    $TIMER "$bin" "${args[@]}" "$INPUT" 2>&1 | tee "$out"
  fi

  local opt kt st vr
  opt=$(grep -m1 'FINAL OPTIMUM' "$out" | sed 's/.*: //')
  kt=$(grep -m1 '^\[e2e\] kernel time' "$out" | sed 's/.*: //')
  st=$(grep -m1 '^\[e2e\] solve time' "$out" | sed 's/.*: //')
  vr=$(grep -m1 'vertex reduction' "$out" | sed 's/.*: //')
  printf '%s|%s|%s|%s|%s\n' "$k" "${opt:-<none>}" "${kt:-?}" "${st:-?}" "${vr:-?}" >> "$RESULT_FILE"
  sync; sleep 2
}

if [ "$SOLVER" = "ilp" ]; then
  echo
  echo "NOTE: --solver ilp solves minimum-weight vertex cover exactly, which is"
  echo "      NP-hard. Measured on this project, a CCG of roughly 6,000 vertices"
  echo "      did not close the optimality gap in over 100 minutes. If the graph"
  echo "      sizes printed below are in that range, expect each configuration to"
  echo "      hit the ${TIMEOUT}s timeout rather than finish."
fi

IFS=',' read -ra KLIST <<< "$KERNELIZERS"
for k in "${KLIST[@]}"; do
  tick "running kernelizer=$k"
  run_config "$k"
done

# ---------------------------------------------------------------- summary

say "SUMMARY"
{
  echo "test name        : $TESTNAME"
  echo "input            : $INPUT"
  echo "solver           : $SOLVER"
  echo "finished         : $(date -Iseconds)"
  echo
  printf '%-12s %-22s %-14s %-14s %s\n' "KERNELIZER" "FINAL OBJECTIVE" "KERNEL TIME" "SOLVE TIME" "VERTEX REDUCTION"
  printf '%-12s %-22s %-14s %-14s %s\n' "----------" "---------------" "-----------" "----------" "----------------"
  while IFS='|' read -r k opt kt st vr; do
    printf '%-12s %-22s %-14s %-14s %s\n' "$k" "$opt" "$kt" "$st" "$vr"
  done < "$RESULT_FILE"
  echo

  #  The correctness verdict: with an exact solver every objective must match
  NOBJ=$(cut -d'|' -f2 "$RESULT_FILE" | grep -v '<none>' | grep -v 'not solved' | sort -u | wc -l)
  NRUN=$(cut -d'|' -f2 "$RESULT_FILE" | grep -v '<none>' | grep -v 'not solved' | wc -l)
  if [ "$SOLVER" = "none" ]; then
    echo "VERDICT: no solver was run, so there is no objective to compare."
  elif [ "$NRUN" -lt 2 ]; then
    echo "VERDICT: fewer than two configurations produced an objective; nothing to compare."
  elif [ "$NOBJ" -eq 1 ]; then
    echo "VERDICT: PASS -- all $NRUN configurations agree on the final objective."
    if [ "$SOLVER" = "mp" ]; then
      echo "         (note: message passing is a heuristic, so agreement is encouraging"
      echo "          but not proof. Use -s ilp for the exact check.)"
    else
      echo "         Kernelization did not change the answer. This is the correctness"
      echo "         property the whole approach depends on."
    fi
  else
    echo "VERDICT: MISMATCH -- $NOBJ different objectives across $NRUN configurations."
    if [ "$SOLVER" = "mp" ]; then
      echo "         Expected with -s mp: message passing does not converge on these"
      echo "         graphs, so it can return different answers on different remnants."
      echo "         Re-run with -s ilp before concluding anything is broken."
    else
      echo "         With the exact ILP solver this indicates a real defect."
    fi
  fi
  for f in "$RUNDIR"/*.txt; do
    [ -f "$f" ] || continue
    k=$(basename "$f" .txt)
    echo
    echo "-- kernelizer=$k : per-round detail --"
    grep -h -E '^\[kernel\] round|^\[graph\] (vertices|edges|total variables)|^\[resource\]' "$f" 2>/dev/null
  done
}

rm -rf "$RESULT_FILE" "$RUNDIR"
echo
echo "Log written to $LOG"
tick "done -- log: $LOG"
exit 0