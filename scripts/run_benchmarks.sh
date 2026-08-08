#!/usr/bin/env bash
# Records timings and kernel sizes for every approach on the benchmark suite.
#
# Results go to results/<timestamp>/ which is gitignored -- benchmark output is
# never committed. Each run is logged in full; the summary at the end is derived
# from those logs, so nothing is lost if a long run is interrupted.
#
# Usage:
#   scripts/run_benchmarks.sh build           # compile everything, then stop
#   scripts/run_benchmarks.sh small           # instances up to 50k
#   scripts/run_benchmarks.sh mid             # up to 500k
#   scripts/run_benchmarks.sh all             # everything (1M runs are long)
#   scripts/run_benchmarks.sh one 06_300k     # a single instance
#
# Environment:
#   REPEATS=3      how many timed repeats per approach (default 3)
#   SKIP_GUROBI=1  omit the LP kernelizer (it dominates runtime at 1M)
set -euo pipefail

REPEATS="${REPEATS:-3}"
SKIP_GUROBI="${SKIP_GUROBI:-0}"
BENCH=data/bench
STAMP=$(date +%Y%m%d_%H%M%S)
RES="results/$STAMP"

ARCH=sm_89
GRB_SRC="third_party/wcsp-solver/src/LinearProgramSolver.cpp third_party/wcsp-solver/src/LinearProgramSolverGurobi.cpp"

MIN_FREE_GB="${MIN_FREE_GB:-12}"

preflight() {
  echo "=== existing benchmark processes ==="
  pgrep -a -f 'compare_kernels|run_benchmarks|e2e_cpu|e2e_gpu|e2e_gurobi|cut_audit' \
    | grep -v "^$$ " || echo "  none"
  echo "=== memory ==="
  free -h
  swapon --show || true
  local avail
  avail=$(free -g | awk '/^Mem:/{print $7}')
  echo "=== available: ${avail} GB (minimum ${MIN_FREE_GB} GB) ==="
  if [ "$avail" -lt "$MIN_FREE_GB" ]; then
    echo "ABORT: not enough free RAM. Close other work or set MIN_FREE_GB lower."
    exit 1
  fi
  local others
  others=$(pgrep -c -f 'compare_kernels|e2e_cpu|e2e_gpu|e2e_gurobi' || true)
  if [ "${others:-0}" -gt 0 ]; then
    echo "ABORT: a benchmark process is already running. Wait for it to finish."
    exit 1
  fi
}

build() {
  echo "=== building ==="
  g++  -std=c++17 -O2 -I. apps/e2e_pipeline_test.cpp -o e2e_cpu -lopenblas
  nvcc -std=c++17 -O2 -arch=$ARCH -I. apps/e2e_pipeline_test_gpu.cu -o e2e_gpu -lopenblas
  nvcc -std=c++17 -O2 -arch=$ARCH -DMAXFLOW_PROFILE -I. apps/e2e_pipeline_test_gpu.cu -o e2e_gpu_prof -lopenblas
  nvcc -std=c++17 -O2 -arch=$ARCH -I. apps/compare_kernels_gpu.cu -o compare_gpu -lopenblas
  g++  -std=c++17 -O2 -I. apps/cut_audit.cpp -o cut_audit -lopenblas
  if [ "$SKIP_GUROBI" = "0" ]; then
    g++ -std=c++17 -O2 -DHAVE_GUROBI -I. -I"$GUROBI_HOME/include" \
      apps/e2e_pipeline_test_gurobi.cpp $GRB_SRC \
      -o e2e_gurobi -L"$GUROBI_HOME/lib" -lgurobi_c++ -lgurobi130 -lopenblas
    g++ -std=c++17 -O2 -DHAVE_GUROBI -I. -I"$GUROBI_HOME/include" \
      apps/compare_kernels.cpp $GRB_SRC \
      -o compare_kernels -L"$GUROBI_HOME/lib" -lgurobi_c++ -lgurobi130 -lopenblas
  fi
  echo "=== build ok ==="
}

#  Isolation matters: back-to-back runs share a warm page cache and a hot GPU,
#  which showed up as ~6% timing variance in earlier measurements.
settle() { sync; sleep 5; }

#  Every heavy run is wrapped so peak RSS is recorded alongside the timing
#  Runs strictly sequentially -- never background a benchmark on this machine
timed() {
  local log="$1"; shift
  /usr/bin/time -v "$@" > "$log" 2>&1 || echo "  (FAILED or OOM-killed -- see $log)"
  grep -E "Maximum resident set size|Elapsed \(wall clock\)" "$log" \
    | sed 's/^/  /' || true
}

run_one() {
  local name="$1"
  local f="$BENCH/$name.wcsp"
  [ -f "$f" ] || { echo "missing $f -- run gen_benchmark_suite.sh first"; return; }

  echo
  echo "############ $name ############"
  mkdir -p "$RES/$name"

  #  Correctness first: one validity certificate per instance
  echo "--- cut_audit ---"
  ./cut_audit "$f" > "$RES/$name/cut_audit.txt" 2>&1 || echo "  (failed)"
  grep -E "INTEGRALITY|=>|VALID MIN-CUT|NT decided TOTAL" "$RES/$name/cut_audit.txt" | head -8 || true
  settle

  echo "--- CPU vs GPU kernel ---"
  ./compare_gpu "$f" > "$RES/$name/compare_gpu.txt" 2>&1 || echo "  (failed)"
  grep -E "resolved by|OPPOSITE" "$RES/$name/compare_gpu.txt" || true
  settle

  if [ "$SKIP_GUROBI" = "0" ]; then
    echo "--- max-flow vs Gurobi kernel ---"
    ./compare_kernels "$f" > "$RES/$name/compare_kernels.txt" 2>&1 || echo "  (failed)"
    grep -E "decided by|OPPOSITE" "$RES/$name/compare_kernels.txt" || true
    settle
  fi

  #  Timed repeats
  for r in $(seq 1 "$REPEATS"); do
    echo "--- CPU  run $r ---"
    ./e2e_cpu "$f" > "$RES/$name/cpu_$r.txt" 2>&1 || echo "  (failed)"
    grep -E "KernelizerMaxflow\]|TOTAL|resolved" "$RES/$name/cpu_$r.txt" || true
    settle

    echo "--- GPU  run $r ---"
    ./e2e_gpu "$f" > "$RES/$name/gpu_$r.txt" 2>&1 || echo "  (failed)"
    grep -E "KernelizerMaxflowGPU\]|TOTAL|resolved" "$RES/$name/gpu_$r.txt" || true
    settle

    if [ "$SKIP_GUROBI" = "0" ]; then
      echo "--- LP   run $r ---"
      ./e2e_gurobi "$f" > "$RES/$name/gurobi_$r.txt" 2>&1 || echo "  (failed)"
      grep -E "Kernelizer|TOTAL|resolved" "$RES/$name/gurobi_$r.txt" || true
      settle
    fi
  done

  #  One profiled GPU run for the stage breakdown / active-vertex curve
  echo "--- GPU profile ---"
  ./e2e_gpu_prof "$f" > "$RES/$name/gpu_profile.txt" 2>&1 || echo "  (failed)"
  grep -E "outer iterations|BFS levels|global relabel|push-relabel|measured total" \
    "$RES/$name/gpu_profile.txt" || true
  settle
}

SMALL=(01_10 02_500 03_1k 04_10k 05_50k)
MID=(06_300k 07_300k_pairwise 08_500k 09_500k_pairwise)
BIG=(10_1M 11_1M_pairwise)

MODE="${1:-small}"
case "$MODE" in
  build) build; exit 0 ;;
  small) build; LIST=("${SMALL[@]}") ;;
  mid)   build; LIST=("${SMALL[@]}" "${MID[@]}") ;;
  all)   build; LIST=("${SMALL[@]}" "${MID[@]}" "${BIG[@]}") ;;
  one)   build; LIST=("${2:?usage: run_benchmarks.sh one <name>}") ;;
  *)     echo "unknown mode: $MODE"; exit 1 ;;
esac

mkdir -p "$RES"
{
  echo "host    : $(hostname)"
  echo "date    : $(date -Iseconds)"
  echo "cpu     : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs)"
  echo "ram     : $(free -g | awk '/^Mem:/{print $2" GB"}')"
  echo "gpu     : $(nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader)"
  echo "nvcc    : $(nvcc --version | tail -1)"
  echo "gcc     : $(g++ --version | head -1)"
  echo "repeats : $REPEATS"
  echo "gurobi  : $([ "$SKIP_GUROBI" = "0" ] && echo enabled || echo skipped)"
} | tee "$RES/environment.txt"

for name in "${LIST[@]}"; do
  run_one "$name"
done

echo
echo "=== all results under $RES ==="