#!/usr/bin/env bash
#
# run_artifact.sh -- evaluate the kernelizers over the published benchmark artifact
#
# The artifact mixes DIMACS and UAI, and most of it has non-Boolean domains that no CCG method can express
# This script runs one configuration over a chosen set and writes one TSV row per instance
#
# note: the published figures count RESOLVED VARIABLES after a SINGLE kernelization round, so --max-rounds defaults to 1 and the comparison column is var_reduction_pct, not vertex_reduction_pct
#
set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

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
RESUME=1
RELIST=0

usage() {
cat <<'USAGE'
Usage: ./scripts/run_artifact.sh [options] [set]

Sets:
  uai        the Boolean UAI instances
  uai-mmap   MMAP only
  uai-pr     PR only
  evalgm     the Boolean evalgm instances

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
      --relist             rebuild the cached instance list
      --artifact DIR       default: ~/mtp/wcsp-maxflow/artifact
  -h, --help

Examples:
  ./scripts/run_artifact.sh --limit 5 uai              # smoke test
  ./scripts/run_artifact.sh -k lp,cpu uai
  ./scripts/run_artifact.sh -k cpu -p int -r 100 uai
  ./scripts/run_artifact.sh --max-vars 10000 evalgm
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
    --relist)         RELIST=1; shift ;;
    --artifact)       ART="$2"; shift 2 ;;
    -h|--help)        usage; exit 0 ;;
    -*)               echo "unknown option: $1" >&2; usage; exit 2 ;;
    *)                SET="$1"; shift ;;
  esac
done

#  set is either one of the built-in names, a directory, or a text file listing one instance path per line
#  SETNAME is the short label used for the log, cached list and the output table, so a path never leaks into a filename
case "$SET" in
  uai|uai-mmap|uai-pr|evalgm) SETNAME="$SET" ;;
  *) SETNAME="$(basename "${SET%/}")"; SETNAME="${SETNAME%.*}" ;;
esac

mf_init "artifact_${SETNAME}"
mkdir -p data/artifact

# the instance list (cached)
#
# Boolean domains only. For .uai the domain sizes are on line 3; for .wcsp the max domain is field 3 of line 1
# Cached in data/artifact/ because scanning thousands of files takes time

build_list() {
  local set="$1" out="$2"
  case "$set" in
    uai|uai-mmap|uai-pr)
      local dirs=""
      [ "$set" = "uai" ]      && dirs="$ART/uai/MMAP $ART/uai/PR"
      [ "$set" = "uai-mmap" ] && dirs="$ART/uai/MMAP"
      [ "$set" = "uai-pr" ]   && dirs="$ART/uai/PR"
      : > "$out"
      local d f
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
      find "$ART/evalgm" -name '*.wcsp' | sort | while IFS= read -r f; do
        read -r _ nv md _ _ < <(head -1 "$f")
        [ "$md" = "2" ] && printf '%s\t%s\n' "$f" "$nv" >> "$out"
      done
      ;;
    *)
      #  directory: every .wcsp/.uai below it. A text file: the paths it lists. Either way each entry is checked for Boolean domains, exactly as above
      local src=""
      if [ -d "$set" ]; then
        src="$(find "$set" \( -name '*.wcsp' -o -name '*.uai' \) -type f | sort)"
      elif [ -f "$set" ]; then
        src="$(grep -v '^[[:space:]]*$' "$set")"
      else
        echo "unknown set: $set (not a built-in name, a directory or a file)" >&2
        return 1
      fi
      : > "$out"
      printf '%s\n' "$src" | while IFS= read -r f; do
        [ -f "$f" ] || { echo "  missing: $f" >&2; continue; }
        case "$f" in
          *.uai)
            if [ "$(sed -n '3p' "$f" | tr ' ' '\n' | grep -v '^$' | sort -u | tr -d '\n')" = "2" ]; then
              printf '%s\t%s\n' "$f" "$(sed -n '2p' "$f" | tr -d ' \r')" >> "$out"
            fi ;;
          *)
            nv="$(head -1 "$f" | awk '{print $2}')"
            md="$(head -1 "$f" | awk '{print $3}')"
            [ "$md" = "2" ] && printf '%s\t%s\n' "$f" "$nv" >> "$out" ;;
        esac
      done
      ;;
  esac
}

#  Built-in sets keep their cached list in data/artifact/ (it tells about benchmark). An ad-hoc directory or list file is rebuilt every run and its list lives with that day's results
case "$SET" in
  uai|uai-mmap|uai-pr|evalgm) LIST="data/artifact/${SETNAME}.list" ;;
  *)                          LIST="$OUT/${SETNAME}.list"; RELIST=1 ;;
esac
if [ "$RELIST" = "1" ] || [ ! -s "$LIST" ]; then
  say "building instance list for '$SETNAME' (scanning)..."
  build_list "$SET" "$LIST" || exit 2
fi
TOTAL=$(wc -l < "$LIST")

record_env "$OUT/env_artifact.txt"
say "set           : $SETNAME  ($TOTAL instances)"
say "kernelizers   : $KERNS"
say "perturb       : $PERTURB  seed=$SEED"
say "max rounds    : $MAXROUNDS"
say "timeout       : $TIMEOUT s"
say "mem limit     : ${MEMGB} GiB"
say "max vars      : $([ "$MAXVARS" -gt 0 ] && echo "$MAXVARS" || echo none)"
say ""

# run it

HDR='instance	format	nvars	kernelizer	perturb	rounds	simplified	by_kernelizer	resolved_total	vars_left	var_reduction_pct	ccg_before	ccg_after	vertex_reduction_pct	kern_time_s	wall_s	peak_kb	status'

write_legend() {
  legend_for "$1" <<'EOF'
instance              file name of the instance
format                dimacs or uai
nvars                 WCSP variables in the instance
kernelizer            none | cpu (our max-flow) | gpu (our CUDA) | lp (Gurobi)
perturb               weight perturbation mode: off | int | real
rounds                kernelization rounds actually run
simplified            variables resolved by simplify() BEFORE any kernelizer
by_kernelizer         variables resolved by the kernelizer itself
resolved_total        simplified + by_kernelizer
vars_left             nvars - resolved_total
var_reduction_pct     100 * resolved_total / nvars  -- the column the published figures use (single round, resolved variables)
ccg_before            CCG vertices before kernelization
ccg_after             CCG vertices after
vertex_reduction_pct  CCG vertex reduction in percent
kern_time_s           time spent inside the kernelizer only
wall_s                wall-clock for the whole instance
peak_kb               peak memory in kB
status                ok | skip_domain (non-Boolean or domain-1)
                         | skip_arity (constraint too wide for the CCG)
                         | timeout | solver_timeout | oom | fail_rcN
EOF
}

IFS_SAVE="$IFS"
IFS=','
for k in $KERNS; do
  IFS="$IFS_SAVE"

  bin="./e2e_solve"
  [ "$k" = "gpu" ] && bin="./e2e_solve_gpu"
  if [ ! -x "$bin" ]; then say "SKIP kernelizer $k ($bin not built)"; IFS=','; continue; fi

  tag="${SETNAME}_${k}_${PERTURB}_r${MAXROUNDS}"
  TSV="$OUT/${tag}.tsv"
  [ "$RESUME" = "0" ] && rm -f "$TSV"
  if [ ! -s "$TSV" ]; then printf '%s\n' "$HDR" > "$TSV"; write_legend "$TSV"; fi

  done_n=$(( $(wc -l < "$TSV") - 1 ))
  say "=== $k / perturb=$PERTURB / rounds=$MAXROUNDS  ->  $TSV  ($done_n already done) ==="

  n=0
  while IFS=$'\t' read -r f nv; do
    name="$(basename "$f")"
    n=$((n+1))
    [ "$LIMIT" -gt 0 ] && [ "$n" -gt "$LIMIT" ] && break
    [ "$MAXVARS" -gt 0 ] && [ "$nv" -gt "$MAXVARS" ] && continue

    # resume: already recorded?
    if [ "$RESUME" = "1" ] && cut -f1 "$TSV" | grep -qxF "$name"; then continue; fi

    out="$RAW/.out.$$"
    res="$RAW/.res.$$"

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
    wall="$(wall_s "$res")"
    peak="$(peak_kb "$res")"
    status="$(status_of $rc)"
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

  say ""
  IFS=','
done
IFS="$IFS_SAVE"

say "done. output in $OUT/"