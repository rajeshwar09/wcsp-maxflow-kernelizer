#!/usr/bin/env bash
#
# run_test.sh -- run the WCSP max-flow kernelizer on ONE benchmark file and record everything to a single log.
#
#   ./scripts/run_test.sh [options] <file.wcsp>
#
# The log is written to  log/<testname>_<YYYYMMDD>_<HHMMSS>.log  and contains the system configuration, the graph/node sizes, per-stage timings for every
# approach, kernel-level GPU timings, correctness checks and peak memory.
#
# Run with -h for the full option list.
#
# MEMORY: one 1,000,000-variable instance can peak near 17 GB. Run one copy of this script at a time; two together will be killed by the Linux OOM handler.
#
set -uo pipefail

# ------------------------------------------------------------------ defaults

APPROACH="all"        # cpu | gpu | gurobi | all
REPEATS=1
OUTDIR="log"
TESTNAME=""
DO_BUILD=1
DO_AUDIT=1
DO_COMPARE=1
DO_PROFILE=1
DO_TIME=1
ARCH_OVERRIDE=""
TIMEOUT=0             # 0 = no limit
KCYCLES=""            # empty = solver's own heuristic
QUIET=0

usage() {
cat <<'USAGE'
Usage: ./scripts/run_test.sh [options] <file.wcsp>

Runs the kernelizer on one benchmark file and writes one log with the system
configuration, graph sizes, timings, correctness checks and peak memory.

Options:
  -a, --approach LIST   Which kernelizers to run: cpu, gpu, gurobi, all Comma-separated, e.g. -a cpu,gpu       (default: all)
  -r, --repeats N       Timed repeats per approach             (default: 1)
  -o, --outdir DIR      Directory for the log file             (default: log)
  -n, --name NAME       Test name used in the log filename (default: the input file's base name)
  -t, --timeout SEC     Kill any single run after SEC seconds  (default: none)

  --arch sm_XX          Force the CUDA architecture instead of auto-detecting
  --kernel-cycles N     Override the push-relabel kernel_cycles parameter

  --no-build            Use existing binaries, do not compile
  --no-audit            Skip the min-cut validity and integrality check
  --no-compare          Skip the kernel agreement comparisons
  --no-profile          Skip the GPU per-kernel profiling run
  --no-time             Skip the timed runs (correctness checks only)
  --graph-only          Only report graph and node sizes, then stop
  -q, --quiet           Less console output (the log is unchanged)
  -h, --help            Show this message

Examples:
  ./scripts/run_test.sh data/bench/06_300k.wcsp
  ./scripts/run_test.sh -a cpu,gpu -r 3 data/bench/07_300k_pairwise.wcsp
  ./scripts/run_test.sh -n bigrun --no-compare -t 3600 data/bench/10_1M.wcsp
  ./scripts/run_test.sh --bfs frontier --kernel-cycles 50 data/bench/05_50k.wcsp
USAGE
}

# ------------------------------------------------------------- parse options

INPUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    -a|--approach)      APPROACH="$2"; shift 2 ;;
    -r|--repeats)       REPEATS="$2"; shift 2 ;;
    -o|--outdir)        OUTDIR="$2"; shift 2 ;;
    -n|--name)          TESTNAME="$2"; shift 2 ;;
    -t|--timeout)       TIMEOUT="$2"; shift 2 ;;
    --arch)             ARCH_OVERRIDE="$2"; shift 2 ;;
    --kernel-cycles)    KCYCLES="$2"; shift 2 ;;
    --no-build)         DO_BUILD=0; shift ;;
    --no-audit)         DO_AUDIT=0; shift ;;
    --no-compare)       DO_COMPARE=0; shift ;;
    --no-profile)       DO_PROFILE=0; shift ;;
    --no-time)          DO_TIME=0; shift ;;
    --graph-only)       DO_AUDIT=0; DO_COMPARE=0; DO_PROFILE=0; DO_TIME=0; shift ;;
    -q|--quiet)         QUIET=1; shift ;;
    -h|--help)          usage; exit 0 ;;
    -*)                 echo "unknown option: $1"; usage; exit 1 ;;
    *)                  INPUT="$1"; shift ;;
  esac
done

if [ -z "$INPUT" ]; then
  echo "error: no input file given"; echo; usage; exit 1
fi
if [ ! -f "$INPUT" ]; then
  echo "error: cannot read '$INPUT'"; exit 2
fi

[ -z "$TESTNAME" ] && TESTNAME=$(basename "$INPUT" .wcsp)
STAMP=$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUTDIR"
LOG="$OUTDIR/${TESTNAME}_${STAMP}.log"

want() { case ",$APPROACH," in *,all,*) return 0 ;; *,$1,*) return 0 ;; *) return 1 ;; esac; }

#  Everything goes to the log; the console gets a copy unless -q.
if [ "$QUIET" = "1" ]; then
  exec 3>&1 1>>"$LOG" 2>&1
else
  exec 3>&1 1> >(tee -a "$LOG") 2>&1
fi
say() { printf '\n===== %s =====\n' "$*"; }
tick() { printf '   %s\n' "$*" >&3; }

TIMER=""
[ "$TIMEOUT" != "0" ] && TIMER="timeout $TIMEOUT"

# ---------------------------------------------------------- system + input

say "TEST"
echo "test name      : $TESTNAME"
echo "input file     : $INPUT"
echo "input size     : $(du -h "$INPUT" | cut -f1)"
echo "input header   : $(head -1 "$INPUT")   (format: <name> <numVars> <maxDomain> <numConstraints> <upperBound>)"
echo "input sha256   : $(sha256sum "$INPUT" | cut -c1-32)"
echo "started        : $(date -Iseconds)"
echo "log file       : $LOG"

say "SYSTEM CONFIGURATION"
echo "host           : $(hostname)"
echo "os             : $(uname -sr)"
[ -r /etc/os-release ] && echo "distribution   : $(. /etc/os-release; echo "$PRETTY_NAME")"
[ -r /proc/cpuinfo ] && {
  echo "cpu            : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs)"
  echo "cpu cores      : $(nproc) logical"
}
echo "ram total      : $(free -h 2>/dev/null | awk '/^Mem:/{print $2}')"
echo "ram available  : $(free -h 2>/dev/null | awk '/^Mem:/{print $7}')"
echo "swap total     : $(free -h 2>/dev/null | awk '/^Swap:/{print $2}')"
echo "disk free      : $(df -h . | awk 'NR==2{print $4}')"
echo "g++            : $(g++ --version 2>/dev/null | head -1)"
echo "nvcc           : $(nvcc --version 2>/dev/null | tail -1)"
echo "python3        : $(python3 --version 2>&1)"
echo "gpu            : $(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null)"
echo "gpu memory     : $(nvidia-smi --query-gpu=memory.total --format=csv,noheader 2>/dev/null)"
echo "gpu driver     : $(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null)"
echo "GUROBI_HOME    : ${GUROBI_HOME:-<not set>}"
echo "git commit     : $(git rev-parse --short HEAD 2>/dev/null || echo '<not a git repo>')"
echo "git state      : $(git status --porcelain 2>/dev/null | grep -c '^ M'; true) modified file(s)"

say "RUN SETTINGS"
echo "approaches     : $APPROACH"
echo "repeats        : $REPEATS"
echo "audit          : $([ $DO_AUDIT = 1 ] && echo on || echo off)"
echo "compare        : $([ $DO_COMPARE = 1 ] && echo on || echo off)"
echo "gpu profile    : $([ $DO_PROFILE = 1 ] && echo on || echo off)"
echo "timed runs     : $([ $DO_TIME = 1 ] && echo on || echo off)"
echo "timeout        : $([ "$TIMEOUT" = 0 ] && echo none || echo "${TIMEOUT}s")"
echo "kernel_cycles  : ${KCYCLES:-<solver heuristic |E|/|V|>}"
echo "profile only   : $([ $DO_TIME = 0 ] && [ $DO_PROFILE = 1 ] && echo yes || echo no)"

#  Detect what this machine can actually do.
HAVE_GPU=0; ARCH=""
if command -v nvcc >/dev/null 2>&1; then
  if [ -n "$ARCH_OVERRIDE" ]; then
    ARCH="$ARCH_OVERRIDE"; HAVE_GPU=1
  else
    CC=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
    [ -n "$CC" ] && { ARCH="sm_${CC}"; HAVE_GPU=1; }
  fi
fi
HAVE_GUROBI=0
[ -n "${GUROBI_HOME:-}" ] && [ -d "${GUROBI_HOME}/include" ] && HAVE_GUROBI=1
echo "cuda arch      : ${ARCH:-<no gpu>}"
echo "gurobi usable  : $([ $HAVE_GUROBI = 1 ] && echo yes || echo no)"

want gpu    && [ $HAVE_GPU    = 0 ] && echo "NOTE: GPU requested but unavailable -- GPU steps skipped"
want gurobi && [ $HAVE_GUROBI = 0 ] && echo "NOTE: Gurobi requested but unavailable -- LP steps skipped"

FAILED=0

#  Run one command, echoing it first, and record wall time and peak memory.
measure() {
  local label="$1"; shift
  echo
  echo "--- $label ---"
  echo "\$ $*"
  local t0 t1
  t0=$(date +%s.%N)
  if command -v /usr/bin/time >/dev/null 2>&1; then
    $TIMER /usr/bin/time -f "[resource] wall %e s | peak RSS %M KB | cpu %P" "$@"
  else
    $TIMER "$@"
  fi
  local rc=$?
  t1=$(date +%s.%N)
  if [ $rc -ne 0 ]; then
    echo "[status] FAILED with exit code $rc"
    FAILED=$((FAILED + 1))
  else
    echo "[status] ok"
  fi
  printf '[elapsed] %.2f s\n' "$(echo "$t1 - $t0" | bc 2>/dev/null || echo 0)"
  sync; sleep 2
}

# ------------------------------------------------------------------- build

if [ "$DO_BUILD" = "1" ]; then
  say "BUILD"
  GRB_SRC="third_party/wcsp-solver/src/LinearProgramSolver.cpp third_party/wcsp-solver/src/LinearProgramSolverGurobi.cpp"
  BUILD_OK=1
  bld() { echo "\$ $*"; "$@" || { echo "BUILD STEP FAILED"; BUILD_OK=0; }; }

  bld g++ -std=c++17 -O2 -I. apps/e2e_pipeline_nokernelize.cpp -o e2e_nokern -lopenblas
  want cpu && bld g++ -std=c++17 -O2 -I. apps/e2e_pipeline_test.cpp -o e2e_cpu -lopenblas
  [ "$DO_AUDIT" = "1" ] && bld g++ -std=c++17 -O2 -I. apps/cut_audit.cpp -o cut_audit -lopenblas

  if want gpu && [ $HAVE_GPU = 1 ]; then
    bld nvcc -std=c++17 -O2 -arch="$ARCH" -I. apps/e2e_pipeline_test_gpu.cu -o e2e_gpu -lopenblas
    [ "$DO_COMPARE" = "1" ] && bld nvcc -std=c++17 -O2 -arch="$ARCH" -I. apps/compare_kernels_gpu.cu -o compare_gpu -lopenblas
    [ "$DO_PROFILE" = "1" ] && bld nvcc -std=c++17 -O2 -arch="$ARCH" -DMAXFLOW_PROFILE -I. apps/e2e_pipeline_test_gpu.cu -o e2e_gpu_prof -lopenblas
  fi
  if want gurobi && [ $HAVE_GUROBI = 1 ]; then
    bld g++ -std=c++17 -O2 -DHAVE_GUROBI -I. -I"$GUROBI_HOME/include" \
        apps/e2e_pipeline_test_gurobi.cpp $GRB_SRC \
        -o e2e_gurobi -L"$GUROBI_HOME/lib" -lgurobi_c++ -lgurobi130 -lopenblas
    [ "$DO_COMPARE" = "1" ] && bld g++ -std=c++17 -O2 -DHAVE_GUROBI -I. -I"$GUROBI_HOME/include" \
        apps/compare_kernels.cpp $GRB_SRC \
        -o compare_kernels -L"$GUROBI_HOME/lib" -lgurobi_c++ -lgurobi130 -lopenblas
  fi

  if [ "$BUILD_OK" != "1" ]; then
    echo; echo "Build failed -- stopping. See the log for compiler output."
    tick "BUILD FAILED -- see $LOG"
    exit 1
  fi
  echo "build ok"
fi

# ------------------------------------------------- graph / node information

say "GRAPH AND NODE INFORMATION"
echo "The constraint composite graph (CCG) is built from the WCSP. The max-flow"
echo "runs on its bipartite double cover, which has 2n+2 nodes for n CCG vertices."
tick "collecting graph sizes"
measure "CCG construction and size" ./e2e_nokern "$INPUT"

# ---------------------------------------------------- correctness / audit

if [ "$DO_AUDIT" = "1" ]; then
  say "MIN-CUT VALIDITY AND INTEGRALITY AUDIT"
  echo "Checks that the extracted cut really is a minimum cut (capacity equals the"
  echo "max-flow, no residual capacity crosses it) and that every capacity,"
  echo "residual and excess value stayed an exact integer."
  tick "running cut audit"
  measure "cut_audit" ./cut_audit "$INPUT"
fi

if [ "$DO_COMPARE" = "1" ]; then
  if want cpu && want gpu && [ $HAVE_GPU = 1 ]; then
    say "KERNEL AGREEMENT: CPU max-flow vs GPU max-flow"
    echo "OPPOSITE must be 0. A non-zero value means one backend is wrong."
    tick "comparing CPU and GPU kernels"
    measure "compare_gpu" ./compare_gpu "$INPUT"
  fi
  if want gurobi && [ $HAVE_GUROBI = 1 ]; then
    say "KERNEL AGREEMENT: max-flow vs Gurobi LP"
    echo "OPPOSITE must be 0. 'Gurobi only' counts variables the LP decides and"
    echo "max-flow does not -- expected, since the LP has several optimal solutions."
    tick "comparing max-flow and Gurobi kernels"
    NVARS=$(head -1 "$INPUT" | awk '{print $2}')
    if [ "${NVARS:-0}" -ge 400000 ] 2>/dev/null; then
      echo "(large instance: running the two kernelizers as separate processes)"
      measure "kernel dump: max-flow" ./compare_kernels mf "$INPUT" "/tmp/${TESTNAME}_mf.txt"
      measure "kernel dump: Gurobi"   ./compare_kernels lp "$INPUT" "/tmp/${TESTNAME}_lp.txt"
      measure "kernel compare"        ./compare_kernels cmp "/tmp/${TESTNAME}_mf.txt" "/tmp/${TESTNAME}_lp.txt"
    else
      measure "compare_kernels" ./compare_kernels "$INPUT"
    fi
  fi
fi

# --------------------------------------------------------------- timings

#  Set BEFORE the timed runs and OUTSIDE the DO_TIME block, so that --no-time
#  still applies these to the profiling run below. Guarded with [ -n ... ]
#  because a bare `export` prints the entire environment.
[ -n "$KCYCLES" ] && export MAXFLOW_KERNEL_CYCLES="$KCYCLES"

if [ "$DO_TIME" = "1" ]; then
  say "TIMED RUNS"
  echo "Each run prints per-stage times. The [Kernelizer...] line is the"
  echo "kernelization stage alone; the other stages are shared by all approaches."

  for r in $(seq 1 "$REPEATS"); do
    if want cpu; then
      tick "CPU run $r/$REPEATS"
      measure "CPU max-flow kernelizer (run $r of $REPEATS)" ./e2e_cpu "$INPUT"
    fi
    if want gpu && [ $HAVE_GPU = 1 ]; then
      tick "GPU run $r/$REPEATS"
      measure "GPU max-flow kernelizer (run $r of $REPEATS)" ./e2e_gpu "$INPUT"
    fi
    if want gurobi && [ $HAVE_GUROBI = 1 ]; then
      tick "Gurobi run $r/$REPEATS"
      measure "Gurobi LP kernelizer (run $r of $REPEATS)" ./e2e_gurobi "$INPUT"
    fi
  done
fi

# ------------------------------------------------------- gpu kernel profile

if [ "$DO_PROFILE" = "1" ] && want gpu && [ $HAVE_GPU = 1 ]; then
  say "GPU PER-KERNEL PROFILE"
  echo "Splits GPU time across the four CUDA kernels and reports the number of"
  echo "active vertices per outer iteration."
  tick "profiling GPU kernels"
  measure "e2e_gpu_prof" ./e2e_gpu_prof "$INPUT"
fi

# ------------------------------------------------------------------ summary

say "SUMMARY"
{
  echo "test name        : $TESTNAME"
  echo "input            : $INPUT"
  echo "finished         : $(date -Iseconds)"
  echo "failed steps     : $FAILED"
  echo
  echo "-- graph size --"
  grep -h -E "CCG vertex count|Post-simplify vars|double cover:|Variables simplified" "$LOG" | sort -u
  echo
  echo "-- kernelization result --"
  grep -h -E "^Remnant s=" "$LOG" | sort -u
  echo
  echo "-- correctness --"
  grep -h -E "VALID MIN-CUT\?|=> (CLEAN|DRIFT)|^  (decided|resolved) by" "$LOG"
  echo
  echo "-- stage timings --"
  grep -h -E "^\[(parse|toPolynomial|ccg|getGraph|Kernelizer)" "$LOG"
  echo
  echo "-- gpu kernels --"
  grep -h -E "bfs variant|outer iterations|BFS levels|global relabel|push-relabel|remove-invalid|check-active|measured total|per level" "$LOG"
  echo
  echo "-- resources --"
  grep -h -E "^\[resource\]" "$LOG"
} | tee /dev/fd/3

echo
echo "Log written to $LOG"
tick "done -- log: $LOG"
[ $FAILED -ne 0 ] && exit 1
exit 0