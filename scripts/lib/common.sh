#!/usr/bin/env bash
#
# common.sh -- shared items for every runner script in this folder
#
# Input to script:
#   mf_init NAME            cd to the repo root, create results/<YYYY-MM-DD>/ and results/<YYYY-MM-DD>/raw/, open NAME.log there, print a banner 
#   say ...                 echo to console AND the run log
#   hr                      horizontal rule
#   legend_for TSV          reads column descriptions from stdin, writes them to <TSV minus .tsv>.legend.txt and prints them, so every table ships with its own explanation
#   record_env F            write machine + git snapshot to F and show it
#   timed OUT RES CMD...    run CMD with /usr/bin/time -v and $TIMEOUT
#   field NAME F            text after the first ':' on the first line matching NAME
#   paired NAME F           "value<TAB>pct" from lines like "NAME : 123 (45.6%)"
#   wall_s F / peak_kb F    from a /usr/bin/time -v resource file
#   status_of RC            ok / solver_timeout / skip_domain / skip_arity / timeout / fail_rcN
#   build_core              compile mincut_lattice, e2e_solve, e2e_solve_gpu, wcsp_collapse
#   build_gates             compile cut_audit, compare_kernels, kernelizer_maxflow_test
#
# Every script that sources this behaves the same way:
#   results stored in                                       results/<YYYY-MM-DD>/        (tables, logs, legends)
#   raw program output in                                   results/<YYYY-MM-DD>/raw/
#   benchmark METADATA (censuses, manifests) belongs in     data/artifact/ 

# ---------------------------------------------------------------- init

mf_init() {
  local tag="$1"
  REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  cd "$REPO" || exit 1
  DAY="$(date +%Y-%m-%d)"
  OUT="results/$DAY"
  RAW="$OUT/raw"
  mkdir -p "$RAW"
  RUNLOG="$OUT/${tag}.log"
  {
    echo ""
    echo "================================================================"
    echo " $tag  --  $(date -Iseconds)"
    echo " branch $(git rev-parse --abbrev-ref HEAD 2>/dev/null)" \
         " commit $(git rev-parse --short HEAD 2>/dev/null)" \
         " ($(git status --porcelain 2>/dev/null | wc -l) dirty file(s))"
    echo " results -> $OUT/"
    echo "================================================================"
  } | tee -a "$RUNLOG"
}

say() { echo "$@" | tee -a "$RUNLOG"; }
hr()  { say "----------------------------------------------------------------"; }

# legend_for results/<day>/foo.tsv <<'EOF' ... EOF
legend_for() {
  local tsv="$1" lg="${1%.tsv}.legend.txt"
  {
    echo "Legend for $(basename "$tsv")   (written $(date -Iseconds))"
    echo ""
    cat
  } > "$lg"
  say ""
  say "--- what the columns of $(basename "$tsv") mean ---"
  tee -a "$RUNLOG" < "$lg" >/dev/null
  cat "$lg"
}

# ---------------------------------------------------------------- env

record_env() {
  local f="$1"
  {
    echo "date            : $(date -Iseconds)"
    echo "host            : $(hostname)"
    echo "os              : $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
    echo "kernel          : $(uname -r)"
    echo "cpu             : $(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"
    echo "ram total kB    : $(awk '/MemTotal/{print $2}' /proc/meminfo)"
    echo "ram avail kB    : $(awk '/MemAvailable/{print $2}' /proc/meminfo)"
    echo "g++             : $(g++ --version 2>/dev/null | head -1)"
    echo "nvcc            : $(nvcc --version 2>/dev/null | tail -1)"
    echo "gpu             : $(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null)"
    echo "gurobi home     : ${GUROBI_HOME:-<unset>}"
    echo "git branch      : $(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
    echo "git commit      : $(git rev-parse --short HEAD 2>/dev/null)"
    echo "git dirty       : $(git status --porcelain 2>/dev/null | wc -l) file(s)"
  } > "$f"
  tee -a "$RUNLOG" < "$f"
}

# ---------------------------------------------------------------- run + parse

# timed OUTFILE RESFILE CMD...   honours $TIMEOUT (0 or unset = no limit)
timed() {
  local out="$1"; shift
  local res="$1"; shift
  if [ "${TIMEOUT:-0}" -gt 0 ]; then
    /usr/bin/time -v -o "$res" timeout "$TIMEOUT" "$@" > "$out" 2>&1
  else
    /usr/bin/time -v -o "$res" "$@" > "$out" 2>&1
  fi
  return $?
}

field()  { sed -n "s/.*$1[^:]*: *//p" "$2" | head -1 | tr -d '\r'; }
paired() { sed -n "s/.*$1[^:]*: *\([0-9][0-9]*\)  *(\([0-9.e+-]*\)%).*/\1\t\2/p" "$2" | head -1; }
wall_s() { awk '/Elapsed .wall clock/{print $NF}' "$1" 2>/dev/null; }
peak_kb(){ awk '/Maximum resident set size/{print $NF}' "$1" 2>/dev/null; }

# exit-code -> status word, one vocabulary for every script
status_of() {
  case "$1" in
    0)   echo ok ;;
    4)   echo solver_timeout ;;
    5)   echo skip_domain ;;      # non-Boolean or domain-1 variable
    6)   echo skip_arity ;;       # constraint arity above --max-arity
    124) echo timeout ;;
    *)   echo "fail_rc$1" ;;
  esac
}

# ---------------------------------------------------------------- build

detect_arch() {
  if [ -n "${ARCH_OVERRIDE:-}" ]; then echo "$ARCH_OVERRIDE"; return; fi
  local cc
  cc="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')"
  [ -n "$cc" ] && echo "sm_${cc}" || echo ""
}

# sets GRB_FLAGS GRB_SRC GRB_LIBS; empty when Gurobi is absent
gurobi_flags() {
  GRB_FLAGS=""; GRB_SRC=""; GRB_LIBS=""
  if [ -n "${GUROBI_HOME:-}" ] && [ -d "$GUROBI_HOME" ]; then
    GRB_FLAGS="-DHAVE_GUROBI -I$GUROBI_HOME/include"
    GRB_SRC="third_party/wcsp-solver/src/LinearProgramSolver.cpp third_party/wcsp-solver/src/LinearProgramSolverGurobi.cpp"
    local lib
    lib="$(ls "$GUROBI_HOME"/lib/libgurobi[0-9]*.so 2>/dev/null | head -1)"
    lib="$(basename "${lib:-libgurobi130.so}" .so)"; lib="${lib#lib}"
    GRB_LIBS="-L$GUROBI_HOME/lib -lgurobi_c++ -l$lib"
    say "gurobi          : enabled ($lib)"
  else
    say "gurobi          : DISABLED (GUROBI_HOME unset) -- lp kernelizer unavailable"
  fi
}

build_core() {
  say "--- build: core tools ---"
  gurobi_flags

  say "compiling mincut_lattice"
  g++ -std=c++17 -O2 -I. apps/mincut_lattice.cpp -o mincut_lattice -lopenblas 2>&1 | tee -a "$RUNLOG"

  say "compiling e2e_solve"
  # shellcheck disable=SC2086
  g++ -std=c++17 -O2 $GRB_FLAGS -I. apps/e2e_solve.cpp $GRB_SRC -o e2e_solve $GRB_LIBS -lopenblas 2>&1 | tee -a "$RUNLOG"

  say "compiling wcsp_collapse"
  g++ -std=c++17 -O2 apps/wcsp_collapse.cpp -o wcsp_collapse 2>&1 | tee -a "$RUNLOG"

  local arch; arch="$(detect_arch)"
  if [ -n "$arch" ] && command -v nvcc >/dev/null 2>&1; then
    say "compiling e2e_solve_gpu ($arch)"
    # shellcheck disable=SC2086
    nvcc -x cu -std=c++17 -O2 -arch="$arch" -DUSE_GPU $GRB_FLAGS -I. apps/e2e_solve.cpp $GRB_SRC -o e2e_solve_gpu $GRB_LIBS -lopenblas 2>&1 | tee -a "$RUNLOG"
  else
    say "gpu             : SKIPPED (no nvcc or no device)"
  fi
  say ""
}

build_gates() {
  say "--- build: correctness gates ---"
  gurobi_flags
  say "compiling kernelizer_maxflow_test"
  g++ -std=c++17 -O2 -I. apps/kernelizer_maxflow_test.cpp -o kmf_test -lopenblas 2>&1 | tee -a "$RUNLOG"
  say "compiling cut_audit"
  g++ -std=c++17 -O2 -I. apps/cut_audit.cpp -o cut_audit -lopenblas 2>&1 | tee -a "$RUNLOG"
  if [ -n "$GRB_FLAGS" ]; then
    say "compiling compare_kernels"
    # shellcheck disable=SC2086
    g++ -std=c++17 -O2 $GRB_FLAGS -I. apps/compare_kernels.cpp $GRB_SRC -o compare_kernels $GRB_LIBS -lopenblas 2>&1 | tee -a "$RUNLOG"
  else
    say "compare_kernels : SKIPPED (needs Gurobi)"
  fi
  say ""
}