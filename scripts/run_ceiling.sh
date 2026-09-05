#!/usr/bin/env bash
#
# run_ceiling.sh -- measure, per instance, the maximum ANY minimum-cut kernelizer could resolve
#
# For each variable the script asks whether its two copies in the double cover lie in the same SCC of the residual graph
# Nodes sharing a component fall on the same side of EVERY minimum cut, so they can never be resolved by a method of this kind -- by us or by Gurobi
# Counting the pairs that do NOT share one gives the ceiling
#
# Result:
#   STRUCTURAL  ceiling is exactly 0   -- nothing to win, for anyone
#   OPTIMAL     we already attain it   -- nothing to fix
#   SELECTION   a better cut exists    -- worth attacking
#   SKIP        loader refused it      -- non-Boolean, or a variable of domain 1
#   EMPTY       the CCG has no vertices
#   TIMEOUT     ran out of time
#
# SKIP and TIMEOUT are counted separately
#
set -uo pipefail

ART=~/mtp/wcsp-maxflow/artifact
SET="uai"
TIMEOUT=900
MINVARS=0
MAXVARS=0
OUTDIR=""
RESUME=1

usage() {
cat <<'USAGE'
Usage: ./scripts/run_ceiling.sh [options] <set>

Sets:
  uai        the 295 UAI files (Boolean ones are analysed, rest SKIP)
  evalgm     the evalgm .wcsp instances

Options:
  -t, --timeout SEC     per instance, 0 = none        (default: 900)
      --min-vars N      only instances with >= N vars (default: 0)
      --max-vars N      only instances with <  N vars (default: no cap)
      --fresh           ignore existing rows
  -o, --outdir DIR      default: log/<date>/ceiling
      --artifact DIR    default: ~/mtp/wcsp-maxflow/artifact
  -h, --help

Examples:
  ./scripts/run_ceiling.sh --max-vars 10000 evalgm
  ./scripts/run_ceiling.sh --min-vars 10000 -t 1800 evalgm
  ./scripts/run_ceiling.sh uai
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -t|--timeout)  TIMEOUT="$2"; shift 2 ;;
    --min-vars)    MINVARS="$2"; shift 2 ;;
    --max-vars)    MAXVARS="$2"; shift 2 ;;
    --fresh)       RESUME=0; shift ;;
    -o|--outdir)   OUTDIR="$2"; shift 2 ;;
    --artifact)    ART="$2"; shift 2 ;;
    -h|--help)     usage; exit 0 ;;
    -*)            echo "unknown option: $1" >&2; usage; exit 2 ;;
    *)             SET="$1"; shift ;;
  esac
done

[ -z "$OUTDIR" ] && OUTDIR="log/$(date +%Y-%m-%d)/ceiling"
mkdir -p "$OUTDIR"

tag="$SET"
[ "$MINVARS" -gt 0 ] && tag="${tag}_ge${MINVARS}"
[ "$MAXVARS" -gt 0 ] && tag="${tag}_lt${MAXVARS}"
TSV="$OUTDIR/${tag}.tsv"
LOG="$OUTDIR/${tag}.log"

[ "$RESUME" = "0" ] && rm -f "$TSV"
[ -s "$TSV" ] || printf 'instance\tgroup\tn\tW\tintegral\tallhalf\tsame_pct\tceiling_pct\tdecided_pct\tverdict\twall_s\n' > "$TSV"

# nvars: line 2 of a .uai, field 2 of line 1 of a .wcsp
nvars_of() {
  case "$1" in
    *.uai)  sed -n '2p' "$1" | tr -d ' \r' ;;
    *)      head -1 "$1" | awk '{print $2}' ;;
  esac
}

case "$SET" in
  uai)    LIST=$(find "$ART/uai" -name '*.uai' | sort) ;;
  evalgm) LIST=$(find "$ART/evalgm" -name '*.wcsp' | sort) ;;
  *)      echo "unknown set: $SET" >&2; exit 2 ;;
esac

total=$(printf '%s\n' "$LIST" | grep -c . )
echo "set '$SET': $total files  ->  $TSV"
{
  echo "date       : $(date -Iseconds)"
  echo "set        : $SET   files: $total"
  echo "timeout    : $TIMEOUT s   min-vars: $MINVARS   max-vars: $MAXVARS"
  echo "git commit : $(git rev-parse --short HEAD 2>/dev/null)"
  echo "git branch : $(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
} | tee -a "$LOG"

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

  #  Distinguish the four non-result outcomes properly.
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

  printf '  [%4d/%-4d] %-38s %-16s %-12s %ss\n' "$i" "$total" "${b:0:38}" "$grp" "$v" "$e" | tee -a "$LOG"
done

echo
echo "=== verdicts ==="
awk -F'\t' 'NR>1{c[$10]++; t++} END{for(k in c) printf "  %-12s %4d  (%.1f%%)\n", k, c[k], 100*c[k]/t; printf "  ------------ %d\n", t}' "$TSV"
echo
echo "=== among instances that produced a verdict ==="
awk -F'\t' 'NR>1 && ($10=="STRUCTURAL"||$10=="OPTIMAL"||$10=="SELECTION"){c[$10]++; t++}
  END{for(k in c) printf "  %-12s %4d  (%.1f%%)\n", k, c[k], 100*c[k]/t; printf "  ------------ %d\n", t}' "$TSV"