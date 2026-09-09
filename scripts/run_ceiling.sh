#!/usr/bin/env bash
#
# run_ceiling.sh -- measure, per instance, the maximum ANY minimum-cut kernelizer could resolve
#
# For each variable it asks whether its two copies in the double cover lie in the same SCC of the residual graph
# Nodes sharing a component fall on the same side of EVERY minimum cut, so no method of this kind -- cpu/gpu or Gurobi's -- can ever resolve them
# Counting the pairs that do NOT share one gives the ceiling
#
set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

ART=~/mtp/wcsp-maxflow/artifact
SET="uai"
TIMEOUT=900
MINVARS=0
MAXVARS=0
RESUME=1

usage() {
cat <<'USAGE'
Usage: ./scripts/run_ceiling.sh [options] <set>

Sets:
  uai        the UAI files (Boolean ones are analysed, rest SKIP)
  evalgm     the evalgm .wcsp instances
  <dir>      any directory: every .wcsp/.uai below it

Options:
  -t, --timeout SEC     per instance, 0 = none        (default: 900)
      --min-vars N      only instances with >= N vars (default: 0)
      --max-vars N      only instances with <  N vars (default: no cap)
      --fresh           ignore existing rows
      --artifact DIR    default: ~/mtp/wcsp-maxflow/artifact
  -h, --help

Examples:
  ./scripts/run_ceiling.sh --max-vars 10000 evalgm
  ./scripts/run_ceiling.sh --min-vars 10000 -t 1800 evalgm
  ./scripts/run_ceiling.sh uai
  ./scripts/run_ceiling.sh ~/mtp/wcsp-maxflow/collapsed
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -t|--timeout)  TIMEOUT="$2"; shift 2 ;;
    --min-vars)    MINVARS="$2"; shift 2 ;;
    --max-vars)    MAXVARS="$2"; shift 2 ;;
    --fresh)       RESUME=0; shift ;;
    --artifact)    ART="$2"; shift 2 ;;
    -h|--help)     usage; exit 0 ;;
    -*)            echo "unknown option: $1" >&2; usage; exit 2 ;;
    *)             SET="$1"; shift ;;
  esac
done

case "$SET" in
  uai)    LIST=$(find "$ART/uai" -name '*.uai' | sort); SETNAME=uai ;;
  evalgm) LIST=$(find "$ART/evalgm" -name '*.wcsp' | sort); SETNAME=evalgm ;;
  *)      if [ -d "$SET" ]; then
            LIST=$(find "$SET" \( -name '*.wcsp' -o -name '*.uai' \) | sort)
            SETNAME="$(basename "$SET")"
          else
            echo "unknown set: $SET" >&2; exit 2
          fi ;;
esac

mf_init "ceiling_${SETNAME}"

tag="$SETNAME"
[ "$MINVARS" -gt 0 ] && tag="${tag}_ge${MINVARS}"
[ "$MAXVARS" -gt 0 ] && tag="${tag}_lt${MAXVARS}"
TSV="$OUT/ceiling_${tag}.tsv"

[ "$RESUME" = "0" ] && rm -f "$TSV"
if [ ! -s "$TSV" ]; then
  printf 'instance\tgroup\tn\tW\tintegral\tallhalf\tsame_pct\tceiling_pct\tdecided_pct\tverdict\twall_s\n' > "$TSV"
  legend_for "$TSV" <<'EOF'
instance     benchmark name (extension stripped)
group        parent folder, i.e. the family
n            CCG vertices
W            total vertex weight of the CCG
integral     are all CCG weights whole numbers (needed for perturbation proof)
allhalf      YES = LP optimum puts every variable at 1/2; ceiling is then 0
same_pct     % of variables whose two copies share a residual SCC -> provably undecidable by ANY min-cut method, Gurobi included
ceiling_pct  % in different SCCs = the most any min-cut kernelizer can decide
decided_pct  % our current rule actually decides
verdict      STRUCTURAL   ceiling is exactly 0 -- nothing to win, for anyone
             OPTIMAL      we already attain the ceiling -- nothing to fix
             SELECTION    a better cut exists -- worth attacking
             SKIP_DOMAIN  loader refused: non-Boolean or domain-1 variable
             SKIP_ARITY   a constraint is too wide for the CCG (2^(2*arity))
             EMPTY        the CCG has no vertices
             TIMEOUT      ran out of time
             FAIL_RCn     crashed with exit code n
wall_s       seconds spent on this instance
EOF
fi

total=$(printf '%s\n' "$LIST" | grep -c .)
record_env "$OUT/env_ceiling.txt"
say "set        : $SETNAME   files: $total"
say "timeout    : $TIMEOUT s   min-vars: $MINVARS   max-vars: $MAXVARS"
say "table      : $TSV"
say ""

# nvars: line 2 of a .uai, field 2 of line 1 of a .wcsp
nvars_of() {
  case "$1" in
    *.uai)  sed -n '2p' "$1" | tr -d ' \r' ;;
    *)      head -1 "$1" | awk '{print $2}' ;;
  esac
}

i=0
printf '%s\n' "$LIST" | while IFS= read -r f; do
  [ -n "$f" ] || continue
  i=$((i+1))
  b=$(basename "$f"); b="${b%.uai}"; b="${b%.wcsp}"
  grp=$(basename "$(dirname "$f")")

  if [ "$RESUME" = "1" ] && awk -F'\t' -v a="$b" -v g="$grp" 'NR>1 && $1==a && $2==g {found=1} END{exit !found}' "$TSV"; then
    continue
  fi

  nv=$(nvars_of "$f")
  case "$nv" in ''|*[!0-9]*) nv=0 ;; esac
  [ "$MINVARS" -gt 0 ] && [ "$nv" -lt "$MINVARS" ] && continue
  [ "$MAXVARS" -gt 0 ] && [ "$nv" -ge "$MAXVARS" ] && continue

  s=$(date +%s)
  if [ "$TIMEOUT" -gt 0 ]; then
    o=$(timeout "$TIMEOUT" ./mincut_lattice "$f" 2>&1); rc=$?
  else
    o=$(./mincut_lattice "$f" 2>&1); rc=$?
  fi
  e=$(( $(date +%s) - s ))

  #  Distinguish the non-result outcomes properly
  if [ "$rc" = "6" ]; then v=SKIP_ARITY
  elif [ "$rc" = "5" ]; then v=SKIP_DOMAIN
  elif printf '%s' "$o" | grep -q 'empty CCG'; then v=EMPTY
  elif [ "$rc" = "124" ]; then v=TIMEOUT
  elif printf '%s' "$o" | grep -q 'skipped --'; then v=SKIP_DOMAIN
  else
    v=$(printf '%s' "$o" | sed -n 's/.*VERDICT: \([A-Z]*\).*/\1/p' | head -1)
    [ -z "$v" ] && v="FAIL_rc$rc"
  fi

  g() { printf '%s' "$o" | sed -n "s/.*$1[^:]*: *//p" | head -1; }
  pct() { printf '%s' "$o" | sed -n "s/.*$1 *: *[0-9]*  *(\([0-9.e+-]*\)%).*/\1/p" | head -1; }

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$b" "$grp" "$(g 'CCG vertices n')" "$(g 'total vertex weight W')" \
    "$(g 'weights integral?')" "$(g 'all-half optimal?')" \
    "$(pct 'SAME SCC')" "$(pct 'DIFFERENT SCCs')" "$(pct 'decided by current rule')" \
    "$v" "$e" >> "$TSV"

  printf '  [%4d/%-4d] %-38s %-16s %-12s %ss\n' "$i" "$total" "${b:0:38}" "$grp" "$v" "$e" | tee -a "$RUNLOG"
done

say ""
say "=== verdicts ==="
awk -F'\t' 'NR>1{c[$10]++; t++} END{for(k in c) printf "  %-12s %4d  (%.1f%%)\n", k, c[k], 100*c[k]/t; printf "  ------------ %d\n", t}' "$TSV" | tee -a "$RUNLOG"
say ""
say "=== among instances that produced a verdict ==="
awk -F'\t' 'NR>1 && ($10=="STRUCTURAL"||$10=="OPTIMAL"||$10=="SELECTION"){c[$10]++; t++}
  END{if(t==0){print "  none"; exit}
      for(k in c) printf "  %-12s %4d  (%.1f%%)\n", k, c[k], 100*c[k]/t
      printf "  at the ceiling (STRUCTURAL+OPTIMAL): %.1f%%\n", 100*(c["STRUCTURAL"]+c["OPTIMAL"])/t}' "$TSV" | tee -a "$RUNLOG"
say ""
say "done. table: $TSV"