#!/usr/bin/env bash
#
# run_artifact.sh -- evaluate the kernelizers over the published benchmark artifact
#
# The artifact mixes DIMACS and UAI, and 70.8 % of it has non-Boolean domains that no CCG method can express. This script filters to the usable subset, runs one
# configuration over it, and writes one TSV row per instance.
#
# Metric note: the published figures count RESOLVED VARIABLES after a SINGLE kernelization round. So --max-rounds defaults to 1 and the comparison column is
# var_reduction, not vertex_reduction
#
set -uo pipefail

ART=~/mtp/wcsp-maxflow/artifact
SET="uai"
KERNS="lp,cpu"
PERTURB="off"
SEED=1
MAXROUNDS=1
TIMEOUT=300
MEMGB=18
MAXVARS=0
LIMIT=0
OUTDIR=""
RESUME=1

usage() {
cat <<'USAGE'
Usage: ./scripts/run_artifact.sh [options] [set]

Sets:
  uai        the 160 Boolean UAI instances          (Tier A / B)
  uai-mmap   MMAP only, 81 instances
  uai-pr     PR only, 79 instances
  evalgm     the 878 Boolean evalgm instances       (Tier C)

Options:
  -k, --kernelizers LIST   comma list: none,cpu,gpu,lp     (default: lp,cpu)
  -p, --perturb MODE       off | int | real                (default: off)
      --seed S             perturbation seed               (default: 1)
  -r, --max-rounds N       1 matches the published protocol (default: 1)
  -t, --timeout SEC        per instance, 0 = none          (default: 300)
  -m, --mem-limit GB       address-space cap, 0 = none     (default: 18)
      --max-vars N         skip instances above N variables (default: no cap)
      --limit N            stop after N instances (smoke test)
      --fresh              ignore existing rows, start over
  -o, --outdir DIR         default: log/<date>/artifact
      --artifact DIR       default: ~/mtp/wcsp-maxflow/artifact
  -h, --help

Examples:
  ./scripts/run_artifact.sh --limit 5 uai              # smoke test
  ./scripts/run_artifact.sh -k lp,cpu uai              # Tier A
  ./scripts/run_artifact.sh -k cpu -p int -r 100 uai   # Tier B, perturbed
  ./scripts/run_artifact.sh --max-vars 10000 evalgm    # Tier C, small end
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -k|--kernelizers) KERNS="$2"; shift 2 ;;
    -p|--perturb)     PERTURB="$2"; shift 2 ;;
    --seed)           SEED="$2"; shift 2 ;;
    -r|--max-rounds)  MAXROUNDS="$2"; shift 2 ;;
    -t|--timeout)     TIMEOUT="$2"; shift 2 ;;
    -m|--mem-limit)   MEMGB="$2"; shift 2 ;;
    --max-vars)       MAXVARS="$2"; shift 2 ;;
    --limit)          LIMIT="$2"; shift 2 ;;
    --fresh)          RESUME=0; shift ;;
    -o|--outdir)      OUTDIR="$2"; shift 2 ;;
    --artifact)       ART="$2"; shift 2 ;;
    -h|--help)        usage; exit 0 ;;
    -*)               echo "unknown option: $1" >&2; usage; exit 2 ;;
    *)                SET="$1"; shift ;;
  esac
done

[ -z "$OUTDIR" ] && OUTDIR="log/$(date +%Y-%m-%d)/artifact"
mkdir -p "$OUTDIR" data/artifact

LISTDIR=data/artifact
RUNLOG="$OUTDIR/run_${SET}.log"

# -------------------------------------------------- build the instance list once
#
# Boolean domains only. For .uai the domain sizes are on line 3
# for .wcsp they are field 3 of line 1
# Cached, because scanning 3,000 files takes time

build_list() {
  local set="$1" out="$2"
  case "$set" in
    uai|uai-mmap|uai-pr)
      local dirs=""
      [ "$set" = "uai" ]      && dirs="$ART/uai/MMAP $ART/uai/PR"
      [ "$set" = "uai-mmap" ] && dirs="$ART/uai/MMAP"
      [ "$set" = "uai-pr" ]   && dirs="$ART/uai/PR"
      : > "$out"
      for d in $dirs; do
        for f in "$d"/*.uai; do
          [ -f "$f" ] || continue
          if [ "$(sed -n '3p' "$f" | tr ' ' '\n' | grep -v '^$' | sort -u | tr -d '\n')" = "2" ]; then
            printf '%s\t%s\n' "$f" "$(sed -n '2p' "$f" | tr -d ' \r')" >> "$out"
          fi
        done
      done
      ;;
    evalgm)
      : > "$out"
      find "$ART/evalgm" -name '*.wcsp' | sort | while read -r f; do
        read -r _ nv md _ _ < <(head -1 "$f")
        [ "$md" = "2" ] && printf '%s\t%s\n' "$f" "$nv" >> "$out"
      done
      ;;
    *) echo "unknown set: $set" >&2; return 1 ;;
  esac
}

LIST="$LISTDIR/${SET}.list"
if [ ! -s "$LIST" ]; then
  echo "building instance list for '$SET' (one-time scan)..."
  build_list "$SET" "$LIST" || exit 2
fi
TOTAL=$(wc -l < "$LIST")
echo "instances in set '$SET': $TOTAL"

# ------------------------------------------------------------------- environment
{
  echo "date          : $(date -Iseconds)"
  echo "set           : $SET  ($TOTAL instances)"
  echo "kernelizers   : $KERNS"
  echo "perturb       : $PERTURB  seed=$SEED"
  echo "max rounds    : $MAXROUNDS"
  echo "timeout       : $TIMEOUT s"
  echo "mem limit     : ${MEMGB} GiB"
  echo "max vars      : $([ "$MAXVARS" -gt 0 ] && echo "$MAXVARS" || echo none)"
  echo "git commit    : $(git rev-parse --short HEAD 2>/dev/null)"
  echo "git branch    : $(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
} | tee "$OUTDIR/env_${SET}.txt"
echo

# ------------------------------------------------------------------------ run it

HDR='instance	format	nvars	kernelizer	perturb	rounds	simplified	by_kernelizer	resolved_total	vars_left	var_reduction_pct	ccg_before	ccg_after	vertex_reduction_pct	kern_time_s	wall_s	peak_kb	status'

IFS_SAVE="$IFS"
IFS=','
for k in $KERNS; do
  IFS="$IFS_SAVE"

  bin="./e2e_solve"
  [ "$k" = "gpu" ] && bin="./e2e_solve_gpu"
  if [ ! -x "$bin" ]; then echo "SKIP kernelizer $k ($bin not built)"; IFS=','; continue; fi

  tag="${SET}_${k}_${PERTURB}_r${MAXROUNDS}"
  TSV="$OUTDIR/${tag}.tsv"
  [ "$RESUME" = "0" ] && rm -f "$TSV"
  [ -s "$TSV" ] || printf '%s\n' "$HDR" > "$TSV"

  done_n=$(( $(wc -l < "$TSV") - 1 ))
  echo "=== $k / perturb=$PERTURB / rounds=$MAXROUNDS  ->  $TSV  ($done_n already done) ==="

  n=0
  while IFS=$'\t' read -r f nv; do
    name="$(basename "$f")"
    n=$((n+1))
    [ "$LIMIT" -gt 0 ] && [ "$n" -gt "$LIMIT" ] && break
    [ "$MAXVARS" -gt 0 ] && [ "$nv" -gt "$MAXVARS" ] && continue

    # resume: already recorded?
    if [ "$RESUME" = "1" ] && cut -f1 "$TSV" | grep -qxF "$name"; then continue; fi

    out="$OUTDIR/.out.$$"
    res="$OUTDIR/.res.$$"

    cmd=("$bin" --kernelizer "$k" --solver none --max-rounds "$MAXROUNDS"
         --perturb "$PERTURB" --seed "$SEED" "$f")

    if [ "$MEMGB" -gt 0 ]; then
      ( ulimit -v $((MEMGB * 1024 * 1024))
        if [ "$TIMEOUT" -gt 0 ]; then
          /usr/bin/time -v -o "$res" timeout "$TIMEOUT" "${cmd[@]}" > "$out" 2>&1
        else
          /usr/bin/time -v -o "$res" "${cmd[@]}" > "$out" 2>&1
        fi )
    else
      if [ "$TIMEOUT" -gt 0 ]; then
        /usr/bin/time -v -o "$res" timeout "$TIMEOUT" "${cmd[@]}" > "$out" 2>&1
      else
        /usr/bin/time -v -o "$res" "${cmd[@]}" > "$out" 2>&1
      fi
    fi
    rc=$?

    g() { sed -n "s/.*$1[^:]*: *//p" "$out" | head -1 | tr -d '\r'; }
    wall="$(awk '/Elapsed .wall clock/{print $NF}' "$res" 2>/dev/null)"
    peak="$(awk '/Maximum resident set size/{print $NF}' "$res" 2>/dev/null)"

    case $rc in
      0)   status=ok ;;
      5)   status=skip_nonboolean ;;
      124) status=timeout ;;
      *)   status="fail_rc$rc" ;;
    esac
    grep -q 'std::bad_alloc\|Cannot allocate' "$out" 2>/dev/null && status=oom

    if [ "$status" = "ok" ]; then
      tv="$(g 'total variables')"
      simp="$(g 'simplified out')"
      bykern="$(g 'by kernelizer')"
      restot="$(g 'resolved total')"
      rounds="$(sed -n 's/^\[kernel\] rounds  *: *//p' "$out" | head -1)"
      before="$(g 'vertices before')"
      after="$(g 'vertices after')"
      vred="$(sed -n 's/.*vertex reduction: *\([0-9.e+-]*\) %.*/\1/p' "$out" | head -1)"
      kt="$(sed -n 's/^\[kernel\] time  *: *\([0-9.e+-]*\) s.*/\1/p' "$out" | head -1)"
      left=$(awk -v a="$tv" -v b="$restot" 'BEGIN{print a-b}')
      varred=$(awk -v a="$tv" -v b="$restot" 'BEGIN{if(a>0) printf "%.6f", 100*b/a; else print 0}')
    else
      tv="$nv"; simp=NA; bykern=NA; restot=NA; rounds=NA
      before=NA; after=NA; vred=NA; kt=NA; left=NA; varred=NA
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$name" "$(g 'format')" "$tv" "$k" "$PERTURB" "$rounds" \
      "$simp" "$bykern" "$restot" "$left" "$varred" \
      "$before" "$after" "$vred" "$kt" "$wall" "$peak" "$status" >> "$TSV"

    printf '  [%4d/%-4d] %-42s %-16s vars=%-8s resolved=%-8s %s%%\n' \
      "$n" "$TOTAL" "$name" "$status" "$tv" "$restot" "$varred" | tee -a "$RUNLOG"

    rm -f "$out" "$res"
  done < "$LIST"

  echo
  IFS=','
done
IFS="$IFS_SAVE"

echo "done. output in $OUTDIR/"