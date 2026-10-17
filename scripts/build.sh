#!/usr/bin/env bash
# build.sh
# compiles the programs of the project into the repo folder
# usage:
#   ./scripts/build.sh                    core and gates
#   ./scripts/build.sh core               e2e_solve, e2e_solve_gpu, mincut_lattice, wcsp_collapse
#   ./scripts/build.sh gates              kmf_test, cut_audit, compare_kernels, compare_kernels_gpu
#   ./scripts/build.sh --arch sm_89 core  force the GPU architecture instead of asking nvidia-smi
# Gurobi is used when GUROBI_HOME points to it, without Gurobi the lp kernelizer and the ilp solver are not built in
# the compiler output of every build is kept in results/<date>/build_<time>.log

set -u
cd "$(dirname "$0")/.." || exit 1

# [ARGS] what to build and an optional GPU architecture
what="all"
arch=""
while [ $# -gt 0 ]; do
  case "$1" in
    core|gates|all) what="$1"; shift ;;
    --arch) arch="$2"; shift 2 ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
done

# [LOG] one log file per build in the folder of the day
day=$(date +%F)
mkdir -p "results/$day"
log="results/$day/build_$(date +%H%M%S).log"
say() {
  echo "$*" | tee -a "$log"
}

# [ARCH] compute capability of the GPU, empty when there is no GPU or no nvcc
if [ -z "$arch" ] && command -v nvcc >/dev/null 2>&1; then
  cc=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
  [ -n "$cc" ] && arch="sm_$cc"
fi

# [GUROBI] flags, sources and libraries for the lp kernelizer and the ilp solver
grb_flags=""
grb_src=""
grb_libs=""
if [ -n "${GUROBI_HOME:-}" ] && [ -d "$GUROBI_HOME" ]; then
  lib=$(ls "$GUROBI_HOME"/lib/libgurobi[0-9]*.so 2>/dev/null | head -1)
  lib=$(basename "${lib:-libgurobi130.so}" .so)
  grb_flags="-DHAVE_GUROBI -I$GUROBI_HOME/include"
  grb_src="third_party/wcsp-solver/src/LinearProgramSolver.cpp third_party/wcsp-solver/src/LinearProgramSolverGurobi.cpp"
  grb_libs="-L$GUROBI_HOME/lib -lgurobi_c++ -l${lib#lib}"
fi

# [LOG] header of the build
say "build $(date '+%F %T')  commit $(git rev-parse --short HEAD 2>/dev/null)  branch $(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
say "target $what  gpu_arch ${arch:-none}  gurobi ${GUROBI_HOME:-none}"

# [BUILD] compile one program, the compiler output goes only to the log
fails=0
compile() {
  local name="$1"
  shift
  echo "### $name : $*" >> "$log"
  if "$@" >> "$log" 2>&1; then
    say "$(printf '%-22s ok' "$name")"
  else
    say "$(printf '%-22s FAILED, see %s' "$name" "$log")"
    fails=$((fails + 1))
  fi
}

# [BUILD] skip line for a program that cannot be built on this machine
skip() {
  say "$(printf '%-22s skipped, %s' "$1" "$2")"
}

# [CORE] the pipeline program (cpu and gpu builds) and the two instance tools
if [ "$what" = "core" ] || [ "$what" = "all" ]; then
  compile e2e_solve g++ -std=c++17 -O2 $grb_flags -I. apps/e2e_solve.cpp $grb_src -o e2e_solve $grb_libs -lopenblas
  if [ -n "$arch" ]; then
    compile e2e_solve_gpu nvcc -x cu -std=c++17 -O2 -arch="$arch" -DUSE_GPU $grb_flags -I. apps/e2e_solve.cpp $grb_src -o e2e_solve_gpu $grb_libs -lopenblas
  else
    skip e2e_solve_gpu "no nvcc or no GPU"
  fi
  compile mincut_lattice g++ -std=c++17 -O2 -I. apps/mincut_lattice.cpp -o mincut_lattice -lopenblas
  compile wcsp_collapse g++ -std=c++17 -O2 apps/wcsp_collapse.cpp -o wcsp_collapse
fi

# [GATES] correctness checks
if [ "$what" = "gates" ] || [ "$what" = "all" ]; then
  compile kmf_test g++ -std=c++17 -O2 -I. apps/kernelizer_maxflow_test.cpp -o kmf_test -lopenblas
  compile cut_audit g++ -std=c++17 -O2 -I. apps/cut_audit.cpp -o cut_audit -lopenblas
  if [ -n "$grb_flags" ]; then
    compile compare_kernels g++ -std=c++17 -O2 $grb_flags -I. apps/compare_kernels.cpp $grb_src -o compare_kernels $grb_libs -lopenblas
  else
    skip compare_kernels "needs Gurobi"
  fi
  if [ -n "$arch" ]; then
    compile compare_kernels_gpu nvcc -x cu -std=c++17 -O2 -arch="$arch" -I. apps/compare_kernels_gpu.cu -o compare_kernels_gpu -lopenblas
  else
    skip compare_kernels_gpu "no nvcc or no GPU"
  fi
fi

# [LOG] result of the whole build
if [ "$fails" -gt 0 ]; then
  say "build failed for $fails program(s)"
  exit 1
fi
say "build done, log $log"
