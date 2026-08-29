#!/usr/bin/env bash
#
# run_study.sh -- generate the min-cut selection study data.
#
# Tells:
#   1. How much can ANY min-cut decide on this instance?   (the ceiling)
#   2. How much does rule decide?                          (where)
#   3. How much does Gurobi's LP decide?                   (is the ceiling real)
#
#   ./scripts/run_study.sh [options] <phase>
#
# Every run writes to  log/<YYYY-MM-DD>/  so days never overwrite each other
#
set -uo pipefail

# ------------------------------------------------------------------ defaults

SET="quick"
OUTDIR=""
KERNS="cpu,lp"
TIMEOUT=1800
DO_BUILD=1
ARCH_OVERRIDE=""

usage() {
cat <<'USAGE'
Usage: ./scripts/run_study.sh [options] <phase>

Phases:
  build     compile the binaries this study needs, then stop
  lattice   run mincut_lattice over an instance set  -> lattice.tsv
  compare   run e2e_solve for each kernelizer        -> kernelizer.tsv
  all       build + lattice + compare

Options:
  -s, --set NAME        instance set (default: quick)
                          quick     8 representative instances, ~10 min
                          family-a  mixed-arity / arity>=3
                          family-b  pairwise, one forbidden tuple
                          family-c  pairwise, two or more forbidden tuples
                          degenerate  empty-CCG controls
                          all       everything except huge
                          huge      10_1M and 12_1_3M  (needs ~20 GiB free)
  -k, --kernelizers L   comma list for `compare`: none,cpu,gpu,lp
                          (default: cpu,lp)
  -o, --outdir DIR      output directory (default: log/YYYY-MM-DD)
  -t, --timeout SEC     per-instance limit, 0 = none (default: 1800)
      --arch SM         CUDA arch, e.g. sm_89 (default: autodetect)
      --no-build        skip compilation
  -h, --help            this message

Examples:
  ./scripts/run_study.sh all
  ./scripts/run_study.sh -s family-b lattice
  ./scripts/run_study.sh -s family-c -k cpu,lp compare
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -s|--set)          SET="$2"; shift 2 ;;
    -k|--kernelizers)  KERNS="$2"; shift 2 ;;
    -o|--outdir)       OUTDIR="$2"; shift 2 ;;
    -t|--timeout)      TIMEOUT="$2"; shift 2 ;;
    --arch)            ARCH_OVERRIDE="$2"; shift 2 ;;
    --no-build)        DO_BUILD=0; shift ;;
    -h|--help)         usage; exit 0 ;;
    -*)                echo "unknown option: $1" >&2; usage; exit 2 ;;
    *)                 PHASE="$1"; shift ;;
  esac
done

PHASE="${PHASE:-}"
case "$PHASE" in
  build|lattice|compare|all) ;;
  "") echo "error: no phase given" >&2; usage; exit 2 ;;
  *)  echo "error: unknown phase '$PHASE'" >&2; usage; exit 2 ;;
esac

[ -z "$OUTDIR" ] && OUTDIR="log/$(date +%Y-%m-%d)"
mkdir -p "$OUTDIR"

RUNLOG="$OUTDIR/run.log"
LATTSV="$OUTDIR/lattice.tsv"
KERNTSV="$OUTDIR/kernelizer.tsv"
SUMMARY="$OUTDIR/summary.txt"
ENVTXT="$OUTDIR/env.txt"

# ------------------------------------------------------------- instance sets

B=data/bench
H=data/bench/hard

set_files() {
  case "$1" in
    quick)
      echo "$B/01_10 $B/04_10k $H/d_m2000 $H/d_m2000_w10 \
            $H/p_bin_t35 $H/p_bin_t50 $H/s_bin_t50_10k $B/07_300k_pairwise" ;;
    family-a)
      echo "$B/01_10 $B/02_500 $B/03_1k $B/04_10k $B/05_50k \
            $B/06_300k $B/08_500k $H/p_bin_t50_a3" ;;
    family-b)
      echo "$H/d_m2000 $H/d_m2000_w10 $H/d_m3000 $H/d_m4000 $H/d_m6000 $H/d_m8000 \
            $H/p_bin_t15 $H/p_bin_t20 $H/p_bin_t25 $H/p_bin_t30 $H/p_bin_t35 \
            $H/q_t30_s1 $H/q_t30_s2 $H/q_t30_s3 \
            $H/q_t35_s1 $H/q_t35_s2 $H/q_t35_s3 \
            $H/q_t37_s1 $H/q_t37_s2 $H/q_t37_s3" ;;
    family-c)
      echo "$H/p_bin_t40 $H/p_bin_t50 $H/p_bin_t75 \
            $H/p_bin_t50_d1 $H/p_bin_t50_d3 $H/p_bin_t50_d5 \
            $H/p_bin_t50_clustered $H/p_bin_t50_scalefree \
            $H/p_narrow_t50 $H/p_uniform_ctrl \
            $H/q_t40_s1 $H/q_t40_s2 $H/q_t40_s3 \
            $H/q_t45_s1 $H/q_t45_s2 $H/q_t45_s3 \
            $H/s_bin_t50_10k $H/s_bin_t50_50k $H/s_bin_t50_100k \
            $B/07_300k_pairwise $B/09_500k_pairwise $B/11_1M_pairwise" ;;
    degenerate)
      echo "$H/p_bin_t10 $H/p_equal $H/s_equal_10k $H/s_equal_50k" ;;
    all)
      echo "$(set_files family-a) $(set_files family-b) \
            $(set_files family-c) $(set_files degenerate)" ;;
    huge)
      echo "$B/10_1M $B/12_1_3M" ;;
    *) echo "" ;;
  esac
}

FILES="$(set_files "$SET")"
if [ -z "$FILES" ]; then
  echo "error: unknown set '$SET'" >&2; exit 2
fi

# `compare` on the biggest instances costs hours of Gurobi; warn rather than silently spend the evening on it
free_kb() { awk '/MemAvailable/{print $2}' /proc/meminfo; }

if [ "$SET" = "huge" ]; then
  need=20971520
  have="$(free_kb)"
  echo "huge set: need ${need} kB available, have ${have} kB"
  if [ "$have" -lt "$need" ]; then
    echo "refusing to start: not enough free RAM. Close other work and retry." >&2
    exit 3
  fi
fi

# ------------------------------------------------------------------ plumbing

say() { echo "$@" | tee -a "$RUNLOG"; }

# field NAME FILE  -> the text after the first colon on the matching line
field() {
  sed -n "s/.*$1[^:]*: *//p" "$2" | head -1 | tr -d '\r'
}

# paired VALUE(PCT) -> "value<TAB>pct"
paired() {
  sed -n "s/.*$1[^:]*: *\([0-9][0-9]*\)  *(\([0-9.e+-]*\)%).*/\1\t\2/p" "$2" | head -1
}

peak_kb() { awk '/Maximum resident set size/{print $NF}' "$1"; }
wall_s()  { awk '/Elapsed .wall clock/{print $NF}' "$1"; }

# run CMD... capturing stdout+stderr to $2 and resource use to $3
timed() {
  local out="$1"; shift
  local res="$1"; shift
  if [ "$TIMEOUT" -gt 0 ]; then
    /usr/bin/time -v -o "$res" timeout "$TIMEOUT" "$@" > "$out" 2>&1
  else
    /usr/bin/time -v -o "$res" "$@" > "$out" 2>&1
  fi
  return $?
}

# ------------------------------------------------------------------ env dump

record_env() {
  {
    echo "date            : $(date -Iseconds)"
    echo "host            : $(hostname)"
    echo "os              : $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
    echo "kernel          : $(uname -r)"
    echo "cpu             : $(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo | sed 's/^ *//')"
    echo "ram total kB    : $(awk '/MemTotal/{print $2}' /proc/meminfo)"
    echo "ram avail kB    : $(free_kb)"
    echo "g++             : $(g++ --version 2>/dev/null | head -1)"
    echo "nvcc            : $(nvcc --version 2>/dev/null | tail -1)"
    echo "gpu             : $(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null)"
    echo "gurobi home     : ${GUROBI_HOME:-<unset>}"
    echo "git branch      : $(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
    echo "git commit      : $(git rev-parse --short HEAD 2>/dev/null)"
    echo "git dirty       : $(git status --porcelain 2>/dev/null | wc -l) file(s)"
    echo "set             : $SET"
    echo "kernelizers     : $KERNS"
    echo "timeout s       : $TIMEOUT"
  } > "$ENVTXT"
  cat "$ENVTXT" | tee -a "$RUNLOG"
}

# --------------------------------------------------------------------- build

detect_arch() {
  if [ -n "$ARCH_OVERRIDE" ]; then echo "$ARCH_OVERRIDE"; return; fi
  local cc
  cc="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')"
  [ -n "$cc" ] && echo "sm_${cc}" || echo ""
}

do_build() {
  say "--- build ---"
  local grb_flags="" grb_src="" grb_libs=""
  if [ -n "${GUROBI_HOME:-}" ] && [ -d "$GUROBI_HOME" ]; then
    grb_flags="-DHAVE_GUROBI -I$GUROBI_HOME/include"
    grb_src="third_party/wcsp-solver/src/LinearProgramSolver.cpp third_party/wcsp-solver/src/LinearProgramSolverGurobi.cpp"
    local lib
    lib="$(ls "$GUROBI_HOME"/lib/libgurobi[0-9]*.so 2>/dev/null | head -1)"
    lib="$(basename "${lib:-libgurobi130.so}" .so)"; lib="${lib#lib}"
    grb_libs="-L$GUROBI_HOME/lib -lgurobi_c++ -l$lib"
    say "gurobi          : enabled ($lib)"
  else
    say "gurobi          : DISABLED (GUROBI_HOME unset) -- lp kernelizer unavailable"
  fi

  say "compiling mincut_lattice"
  g++ -std=c++17 -O2 -I. apps/mincut_lattice.cpp -o mincut_lattice -lopenblas 2>&1 | tee -a "$RUNLOG"

  say "compiling e2e_solve"
  # shellcheck disable=SC2086
  g++ -std=c++17 -O2 $grb_flags -I. apps/e2e_solve.cpp $grb_src \
      -o e2e_solve $grb_libs -lopenblas 2>&1 | tee -a "$RUNLOG"

  local arch; arch="$(detect_arch)"
  if [ -n "$arch" ] && command -v nvcc >/dev/null 2>&1; then
    say "compiling e2e_solve_gpu ($arch)"
    # shellcheck disable=SC2086
    nvcc -x cu -std=c++17 -O2 -arch="$arch" -DUSE_GPU $grb_flags -I. \
        apps/e2e_solve.cpp $grb_src -o e2e_solve_gpu $grb_libs -lopenblas 2>&1 | tee -a "$RUNLOG"
  else
    say "gpu             : SKIPPED (no nvcc or no device)"
  fi
  say ""
}

# ------------------------------------------------------------------- lattice

do_lattice() {
  say "--- lattice: set=$SET ---"
  printf 'instance\tn\tcover_nodes\tcover_edges\tW\tmaxflow\tW_minus_F\tall_half\tsame_scc\tsame_pct\tdiff_scc\tdiff_pct\tdecided\tdecided_pct\trecoverable\tverdict\twall_s\tpeak_kb\n' > "$LATTSV"

  for f in $FILES; do
    local name; name="$(basename "$f")"
    local wcsp="$f.wcsp"
    if [ ! -f "$wcsp" ]; then
      say "SKIP $name (file not found)"; continue
    fi
    say "lattice $name"
    local out="$OUTDIR/lattice_$name.txt"
    local res="$OUTDIR/.res"
    timed "$out" "$res" ./mincut_lattice "$wcsp"
    local rc=$?

    if grep -q "empty CCG" "$out" 2>/dev/null; then
      printf '%s\t0\t0\t0\t0\t0\t0\tEMPTY\t0\t0\t0\t0\t0\t0\t0\tEMPTY_CCG\t%s\t%s\n' \
        "$name" "$(wall_s "$res")" "$(peak_kb "$res")" >> "$LATTSV"
      say "  empty CCG -- no graph to analyse"
      continue
    fi
    if [ $rc -ne 0 ]; then
      printf '%s\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tFAILED_rc%s\t%s\t%s\n' \
        "$name" "$rc" "$(wall_s "$res")" "$(peak_kb "$res")" >> "$LATTSV"
      say "  FAILED rc=$rc (see $out)"
      continue
    fi

    local n cn ce w mf ah same diff dec rec verdict
    n="$(field 'CCG vertices n' "$out")"
    cn="$(field 'double cover nodes' "$out")"
    ce="$(field 'double cover edges' "$out")"
    w="$(field 'total vertex weight W' "$out")"
    mf="$(sed -n 's/^  max-flow  *: *//p' "$out" | head -1)"
    ah="$(field 'all-half optimal?' "$out")"
    same="$(paired 'SAME SCC' "$out")"
    diff="$(paired 'DIFFERENT SCCs' "$out")"
    dec="$(paired 'decided by current rule' "$out")"
    rec="$(field 'recoverable by better choice' "$out")"
    verdict="$(sed -n 's/.*VERDICT: \([A-Z]*\).*/\1/p' "$out" | head -1)"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$name" "$n" "$cn" "$ce" "$w" "$mf" \
      "$(awk -v a="$w" -v b="$mf" 'BEGIN{printf "%.6g", a-b}')" \
      "$ah" "$same" "$diff" "$dec" "$rec" "$verdict" \
      "$(wall_s "$res")" "$(peak_kb "$res")" >> "$LATTSV"

    say "  n=$n  all-half=$ah  decided=$(echo "$dec" | cut -f2)%  ceiling=$(echo "$diff" | cut -f2)%  $verdict"
  done
  rm -f "$OUTDIR/.res"
  say ""
}

# ------------------------------------------------------------------- compare

do_compare() {
  say "--- compare: set=$SET kernelizers=$KERNS ---"
  printf 'instance\tkernelizer\ttotal_vars\tvertices_before\tdecided\treduction_pct\trounds\tkern_time_s\twall_s\tpeak_kb\n' > "$KERNTSV"

  for f in $FILES; do
    local name; name="$(basename "$f")"
    local wcsp="$f.wcsp"
    [ -f "$wcsp" ] || { say "SKIP $name (file not found)"; continue; }

    local IFS_SAVE="$IFS"; IFS=','
    for k in $KERNS; do
      IFS="$IFS_SAVE"
      local bin="./e2e_solve"
      [ "$k" = "gpu" ] && bin="./e2e_solve_gpu"
      if [ ! -x "$bin" ]; then say "SKIP $name/$k ($bin not built)"; continue; fi

      say "compare $name / $k"
      local out="$OUTDIR/compare_${name}_${k}.txt"
      local res="$OUTDIR/.res"
      timed "$out" "$res" "$bin" --kernelizer "$k" --solver none "$wcsp"
      local rc=$?

      if [ $rc -ne 0 ]; then
        printf '%s\t%s\tNA\tNA\tNA\tNA\tNA\tNA\t%s\t%s\n' \
          "$name" "$k" "$(wall_s "$res")" "$(peak_kb "$res")" >> "$KERNTSV"
        say "  FAILED rc=$rc (see $out)"
        continue
      fi

      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$k" \
        "$(field 'total variables' "$out")" \
        "$(field 'vertices before' "$out")" \
        "$(field 'by kernelizer' "$out")" \
        "$(sed -n 's/.*vertex reduction: *\([0-9.e+-]*\) %.*/\1/p' "$out" | head -1)" \
        "$(sed -n 's/^\[kernel\] rounds  *: *//p' "$out" | head -1)" \
        "$(sed -n 's/^\[kernel\] time  *: *\([0-9.e+-]*\) s.*/\1/p' "$out" | head -1)" \
        "$(wall_s "$res")" "$(peak_kb "$res")" >> "$KERNTSV"

      say "  decided=$(field 'by kernelizer' "$out")  reduction=$(sed -n 's/.*vertex reduction: *//p' "$out" | head -1)"
      IFS=','
    done
    IFS="$IFS_SAVE"
  done
  rm -f "$OUTDIR/.res"
  say ""
}

# ------------------------------------------------------------------- summary

do_summary() {
  {
    echo "================================================================"
    echo " MIN-CUT SELECTION STUDY -- $(date -Iseconds)"
    echo " set: $SET   phase: $PHASE   outdir: $OUTDIR"
    echo "================================================================"
    echo
    if [ -s "$LATTSV" ]; then
      echo "--- CEILING vs ACHIEVED (from lattice.tsv) ---"
      awk -F'\t' 'NR==1{next} {printf "%-22s  n=%-9s all-half=%-5s  decided=%8s%%  ceiling=%8s%%  gap=%8s  %s\n", $1,$2,$8,$14,$12,$15,$16}' "$LATTSV"
      echo
      echo "all-half law check (all-half YES must imply decided 0):"
      awk -F'\t' 'NR==1{next} $8=="YES" && $13!=0 {bad++} END{printf "  violations: %d\n", bad+0}' "$LATTSV"
      echo
    fi
    if [ -s "$KERNTSV" ]; then
      echo "--- KERNELIZER COMPARISON (from kernelizer.tsv) ---"
      awk -F'\t' 'NR==1{next} {printf "%-22s %-5s  decided=%-8s reduction=%-12s rounds=%-3s time=%s s\n", $1,$2,$5,$6,$7,$8}' "$KERNTSV"
      echo
    fi
    echo "Files written:"
    ls -1 "$OUTDIR"
  } > "$SUMMARY"
  cat "$SUMMARY"
}

# ---------------------------------------------------------------------- main

: > "$RUNLOG"
record_env

case "$PHASE" in
  build)   [ "$DO_BUILD" = "1" ] && do_build ;;
  lattice) [ "$DO_BUILD" = "1" ] && do_build; do_lattice; do_summary ;;
  compare) [ "$DO_BUILD" = "1" ] && do_build; do_compare; do_summary ;;
  all)     [ "$DO_BUILD" = "1" ] && do_build; do_lattice; do_compare; do_summary ;;
esac

echo
echo "done. everything in $OUTDIR/"