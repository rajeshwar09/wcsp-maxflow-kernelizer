#!/usr/bin/env bash
#
# run_study.sh -- the min-cut selection study on SYNTHETIC instances
#
# Tells:
#   1. How much can ANY min-cut decide here?                     (the ceiling, from mincut_lattice)
#   2. How much does the rule/new kernelizer decide?             (compare, kernelizer=cpu/gpu)
#   3. How much does Gurobi's LP decide?                         (compare, kernelizer=lp)
#
#   ./scripts/run_study.sh [options] <phase>
#
# Everything is written to results/<YYYY-MM-DD>/ 
# Every table has a .legend.txt file with it
#
set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

# ------------------------------------------------------------------ defaults

SET="quick"
KERNS="cpu,lp"
TIMEOUT=1800
DO_BUILD=1
ARCH_OVERRIDE=""

usage() {
cat <<'USAGE'
Usage: ./scripts/run_study.sh [options] <phase>

Phases:
  build     compile the binaries, then stop
  lattice   run mincut_lattice over an instance set  -> lattice_<set>.tsv
  compare   run e2e_solve for each kernelizer        -> kernelizer_<set>.tsv
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
                        OR a path to a text file listing one .wcsp per line
  -k, --kernelizers L   comma list for compare: none,cpu,gpu,lp (default: cpu,lp)
  -t, --timeout SEC     per-instance limit, 0 = none (default: 1800)
      --arch SM         CUDA arch, e.g. sm_89 (default: autodetect)
      --no-build        skip compilation
  -h, --help            this message

Examples:
  ./scripts/run_study.sh all
  ./scripts/run_study.sh -s family-b lattice
  ./scripts/run_study.sh -s my_instances.list -k cpu compare
USAGE
}

PHASE=""
while [ $# -gt 0 ]; do
  case "$1" in
    -s|--set)          SET="$2"; shift 2 ;;
    -k|--kernelizers)  KERNS="$2"; shift 2 ;;
    -t|--timeout)      TIMEOUT="$2"; shift 2 ;;
    --arch)            ARCH_OVERRIDE="$2"; shift 2 ;;
    --no-build)        DO_BUILD=0; shift ;;
    -h|--help)         usage; exit 0 ;;
    -*)                echo "unknown option: $1" >&2; usage; exit 2 ;;
    *)                 PHASE="$1"; shift ;;
  esac
done

case "$PHASE" in
  build|lattice|compare|all) ;;
  "") echo "error: no phase given" >&2; usage; exit 2 ;;
  *)  echo "error: unknown phase '$PHASE'" >&2; usage; exit 2 ;;
esac

# ------------------------------------------------------------- instance sets

B=data/bench
H=data/bench/hard

set_files() {
  case "$1" in
    quick)
      echo "$B/01_10 $B/04_10k $H/d_m2000 $H/d_m2000_w10 $H/p_bin_t35 $H/p_bin_t50 $H/s_bin_t50_10k $B/07_300k_pairwise" ;;
    family-a)
      echo "$B/01_10 $B/02_500 $B/03_1k $B/04_10k $B/05_50k $B/06_300k $B/08_500k $H/p_bin_t50_a3" ;;
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
      echo "$(set_files family-a) $(set_files family-b) $(set_files family-c) $(set_files degenerate)" ;;
    huge)
      echo "$B/10_1M $B/12_1_3M" ;;
    *) echo "" ;;
  esac
}

# a set can also be a plain file: one instance path per line, .wcsp optional
if [ -f "$SET" ]; then
  FILES="$(sed 's/\.wcsp$//' "$SET" | grep -v '^[[:space:]]*$' | tr '\n' ' ')"
  SETNAME="$(basename "$SET")"; SETNAME="${SETNAME%.*}"
else
  FILES="$(set_files "$SET")"
  SETNAME="$SET"
fi
if [ -z "$FILES" ]; then
  echo "error: unknown set '$SET' (not a named set, not a readable file)" >&2; exit 2
fi

mf_init "study_${SETNAME}"

LATTSV="$OUT/lattice_${SETNAME}.tsv"
KERNTSV="$OUT/kernelizer_${SETNAME}.tsv"
SUMMARY="$OUT/summary_${SETNAME}.txt"

# huge needs headroom; refuse rather than trigger the OOM killer
if [ "$SETNAME" = "huge" ]; then
  need=20971520
  have="$(awk '/MemAvailable/{print $2}' /proc/meminfo)"
  say "huge set: need ${need} kB available, have ${have} kB"
  if [ "$have" -lt "$need" ]; then
    say "refusing to start: not enough free RAM. Close other work and retry."
    exit 3
  fi
fi

record_env "$OUT/env_study.txt"
say "set             : $SETNAME"
say "kernelizers     : $KERNS"
say "timeout s       : $TIMEOUT"
say ""

# ------------------------------------------------------------------- lattice

do_lattice() {
  say "--- lattice: set=$SETNAME ---"
  printf 'instance\tn\tcover_nodes\tcover_edges\tW\tmaxflow\tW_minus_F\tall_half\tsame_scc\tsame_pct\tdiff_scc\tdiff_pct\tdecided\tdecided_pct\trecoverable\tverdict\twall_s\tpeak_kb\n' > "$LATTSV"

  legend_for "$LATTSV" <<'EOF'
instance      benchmark name
n             vertices in the CCG (the graph the kernelizer works on)
cover_nodes   nodes in the bipartite double-cover flow network
cover_edges   edges in that flow network
W             total vertex weight of the CCG
maxflow       max-flow value on the double cover
W_minus_F     W - maxflow; 0 means the "everything at 1/2" LP solution is optimal
all_half      YES = every variable sits at 1/2, so NO min-cut can decide anything
same_scc      variables whose two copies share one SCC of the residual graph; these sit on the same side of EVERY min cut -> undecidable by any cut
same_pct      the above as a percentage of n
diff_scc      variables whose copies are in different SCCs -> the CEILING: the most that any min-cut kernelizer (ours or Gurobi's) could decide
diff_pct      the ceiling as a percentage
decided       what our current cut actually decided
decided_pct   the above as a percentage
recoverable   ceiling minus decided: what a better cut choice could still win
verdict       STRUCTURAL = ceiling is 0, nothing to win for anyone
              OPTIMAL    = we already sit at the ceiling
              SELECTION  = a better cut exists, worth attacking
wall_s        wall-clock seconds for this instance
peak_kb       peak memory (kB) for this instance
EOF

  local name wcsp out res rc
  for f in $FILES; do
    name="$(basename "$f")"
    wcsp="$f.wcsp"
    if [ ! -f "$wcsp" ]; then
      say "SKIP $name (file not found)"; continue
    fi
    say "lattice $name"
    out="$RAW/lattice_${SETNAME}_$name.txt"
    res="$RAW/.res"
    timed "$out" "$res" ./mincut_lattice "$wcsp"
    rc=$?

    if grep -q "empty CCG" "$out" 2>/dev/null; then
      printf '%s\t0\t0\t0\t0\t0\t0\tEMPTY\t0\t0\t0\t0\t0\t0\t0\tEMPTY_CCG\t%s\t%s\n' "$name" "$(wall_s "$res")" "$(peak_kb "$res")" >> "$LATTSV"
      say "  empty CCG -- no graph to analyse"
      continue
    fi
    if [ $rc -ne 0 ]; then
      printf '%s\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\tNA\t%s\t%s\t%s\n' "$name" "$(status_of $rc)" "$(wall_s "$res")" "$(peak_kb "$res")" >> "$LATTSV"
      say "  $(status_of $rc) rc=$rc (see $out)"
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
  rm -f "$RAW/.res"
  say ""
}

# ------------------------------------------------------------------- compare

do_compare() {
  say "--- compare: set=$SETNAME kernelizers=$KERNS ---"
  printf 'instance\tkernelizer\ttotal_vars\tvertices_before\tdecided\treduction_pct\trounds\tkern_time_s\twall_s\tpeak_kb\n' > "$KERNTSV"

  legend_for "$KERNTSV" <<'EOF'
instance         benchmark name
kernelizer       none | cpu (our max-flow) | gpu (our CUDA max-flow) | lp (Gurobi)
total_vars       WCSP variables in the instance
vertices_before  CCG vertices before kernelization
decided          variables the kernelizer resolved (simplify() excluded)
reduction_pct    CCG vertex reduction in percent -- the headline number
rounds           kernelization rounds until nothing new was resolved
kern_time_s      time spent inside the kernelizer only
wall_s           wall-clock seconds for the whole run
peak_kb          peak memory (kB)
EOF

  local name wcsp out res rc bin IFS_SAVE
  for f in $FILES; do
    name="$(basename "$f")"
    wcsp="$f.wcsp"
    [ -f "$wcsp" ] || { say "SKIP $name (file not found)"; continue; }

    IFS_SAVE="$IFS"; IFS=','
    for k in $KERNS; do
      IFS="$IFS_SAVE"
      bin="./e2e_solve"
      [ "$k" = "gpu" ] && bin="./e2e_solve_gpu"
      if [ ! -x "$bin" ]; then say "SKIP $name/$k ($bin not built)"; continue; fi

      say "compare $name / $k"
      out="$RAW/compare_${SETNAME}_${name}_${k}.txt"
      res="$RAW/.res"
      timed "$out" "$res" "$bin" --kernelizer "$k" --solver none "$wcsp"
      rc=$?

      if [ $rc -ne 0 ]; then
        printf '%s\t%s\tNA\tNA\tNA\tNA\tNA\tNA\t%s\t%s\n' "$name" "$k" "$(wall_s "$res")" "$(peak_kb "$res")" >> "$KERNTSV"
        say "  $(status_of $rc) rc=$rc (see $out)"
        IFS=','
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
  rm -f "$RAW/.res"
  say ""
}

# ------------------------------------------------------------------- summary

do_summary() {
  {
    echo "================================================================"
    echo " MIN-CUT SELECTION STUDY -- $(date -Iseconds)"
    echo " set: $SETNAME   phase: $PHASE   results: $OUT"
    echo "================================================================"
    echo
    if [ -s "$LATTSV" ]; then
      echo "--- CEILING vs ACHIEVED (from $(basename "$LATTSV")) ---"
      awk -F'\t' 'NR==1{next} {printf "%-22s  n=%-9s all-half=%-5s  decided=%8s%%  ceiling=%8s%%  gap=%8s  %s\n", $1,$2,$8,$14,$12,$15,$16}' "$LATTSV"
      echo
      echo "all-half law check (all-half YES must imply decided 0):"
      awk -F'\t' 'NR==1{next} $8=="YES" && $13!=0 {bad++} END{printf "  violations: %d\n", bad+0}' "$LATTSV"
      echo
    fi
    if [ -s "$KERNTSV" ]; then
      echo "--- KERNELIZER COMPARISON (from $(basename "$KERNTSV")) ---"
      awk -F'\t' 'NR==1{next} {printf "%-22s %-5s  decided=%-8s reduction=%-12s rounds=%-3s time=%s s\n", $1,$2,$5,$6,$7,$8}' "$KERNTSV"
      echo
    fi
    echo "Files written to $OUT/:"
    ls -1 "$OUT" | sed 's/^/  /'
  } > "$SUMMARY"
  tee -a "$RUNLOG" < "$SUMMARY"
}

# ---------------------------------------------------------------------- main

case "$PHASE" in
  build)   [ "$DO_BUILD" = "1" ] && build_core ;;
  lattice) [ "$DO_BUILD" = "1" ] && build_core; do_lattice; do_summary ;;
  compare) [ "$DO_BUILD" = "1" ] && build_core; do_compare; do_summary ;;
  all)     [ "$DO_BUILD" = "1" ] && build_core; do_lattice; do_compare; do_summary ;;
esac

say ""
say "done. everything in $OUT/"