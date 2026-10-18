#!/usr/bin/env bash
# run_ceiling.sh
# finds, for every instance, the most that any min-cut kernelizer could decide (the ceiling), with ./mincut_lattice
# for each variable it checks if its two copies in the double cover are in the same SCC of the residual graph
# copies in the same SCC are on the same side of every min cut, so no min-cut method (cpu, gpu or Gurobi LP) can decide them
# the variables whose copies are in different SCCs give the ceiling
#
# usage: ./scripts/run_ceiling.sh [options] [uai|evalgm|DIR]      (default uai)
#   uai                  every .uai under <artifact>/uai, non Boolean ones end as SKIP_DOMAIN
#   evalgm               every .wcsp under <artifact>/evalgm
#   DIR                  every .wcsp and .uai under any folder, for example ~/mtp/wcsp-maxflow/collapsed
#   -t, --timeout SEC    limit for one instance, 0 = no limit         (default 900)
#       --min-vars N     only instances with at least N variables
#       --max-vars N     only instances with less than N variables
#       --resume FILE    continue an earlier ceiling tsv, instances already in it are skipped and new rows go into it
#       --artifact DIR   the local benchmark copy                     (default ~/mtp/wcsp-maxflow/artifact)
#   -n, --name LABEL     word added to the file names
#
# output in results/<date>/:
#   ceiling_<set>_<time>[_label].log   one line per instance and the count of every verdict
#   ceiling_<set>_<time>[_label].tsv   one row per instance (or the --resume file)
#   ceiling_<set>_<time>[_label]/      the mincut_lattice output of every instance
# verdicts: STRUCTURAL (ceiling is 0), OPTIMAL (we already reach the ceiling), SELECTION (a better cut exists),
#           SKIP_DOMAIN, SKIP_ARITY, EMPTY (no CCG vertices), TIMEOUT, FAIL_rcN

set -u
cd "$(dirname "$0")/.." || exit 1

# [ARGS] defaults and command line
art=~/mtp/wcsp-maxflow/artifact
set_name="uai"
timeout_s=900
min_vars=0
max_vars=0
resume=""
label=""
while [ $# -gt 0 ]; do
  case "$1" in
    -t|--timeout) timeout_s="$2"; shift 2 ;;
    --min-vars) min_vars="$2"; shift 2 ;;
    --max-vars) max_vars="$2"; shift 2 ;;
    --resume) resume="$2"; shift 2 ;;
    --artifact) art="$2"; shift 2 ;;
    -n|--name) label="_$2"; shift 2 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    -*) echo "unknown option: $1 (see --help)"; exit 1 ;;
    *) set_name="$1"; shift ;;
  esac
done

# [INSTANCES] the files of the set, in sorted order
case "$set_name" in
  uai) dir="$art/uai"; tag=uai; pattern='*.uai' ;;
  evalgm) dir="$art/evalgm"; tag=evalgm; pattern='*.wcsp' ;;
  *) dir="$set_name"; tag=$(basename "$set_name"); pattern='' ;;
esac
if [ ! -d "$dir" ]; then
  echo "folder not found: $dir"
  exit 1
fi
if [ -n "$pattern" ]; then
  mapfile -t files < <(find "$dir" -name "$pattern" | sort)
else
  mapfile -t files < <(find "$dir" \( -name '*.wcsp' -o -name '*.uai' \) | sort)
fi
if [ ! -x ./mincut_lattice ]; then
  echo "./mincut_lattice not found, run ./scripts/build.sh core first"
  exit 1
fi
if [ -n "$resume" ] && [ ! -f "$resume" ]; then
  echo "resume file not found: $resume"
  exit 1
fi

# [FILES] names of the output files of this run, the size filter goes into the name
[ "$min_vars" -gt 0 ] && tag="${tag}_ge$min_vars"
[ "$max_vars" -gt 0 ] && tag="${tag}_lt$max_vars"
stamp="ceiling_${tag}_$(date +%H%M%S)${label}"
out_dir="results/$(date +%F)"
raw_dir="$out_dir/$stamp"
log="$out_dir/$stamp.log"
tsv="$out_dir/$stamp.tsv"
mkdir -p "$raw_dir"
if [ -n "$resume" ]; then
  tsv="$resume"
else
  printf 'instance\tgroup\tn\tW\tintegral\tallhalf\tsame_pct\tceiling_pct\tdecided_pct\tverdict\twall_s\n' > "$tsv"
fi

say() {
  echo "$*" | tee -a "$log"
}

say "ceiling $(date '+%F %T')  commit $(git rev-parse --short HEAD)  branch $(git rev-parse --abbrev-ref HEAD)"
say "set $tag  folder $dir  files ${#files[@]}  timeout $timeout_s  min_vars $min_vars  max_vars $max_vars  resume ${resume:-no}"
say "table $tsv"

# [RUN] variable count: line 2 of a .uai, field 2 of line 1 of a .wcsp
nvars_of() {
  case "$1" in
    *.uai) sed -n '2p' "$1" | tr -d ' \r' ;;
    *) head -1 "$1" | awk '{print $2}' ;;
  esac
}

# [RUN] text after the first ':' of the first line with this label
field() {
  sed -n "s/.*$1[^:]*: *//p" "$2" | head -1
}

# [RUN] the percent of a line like "label : 123 (45.6%)"
pct() {
  sed -n "s/.*$1 *: *[0-9]*  *(\([0-9.e+-]*\)%).*/\1/p" "$2" | head -1
}

i=0
for f in "${files[@]}"; do
  i=$((i + 1))
  name=$(basename "$f")
  name="${name%.*}"
  group=$(basename "$(dirname "$f")")

  # [RUN] skip instances already in the resume table and instances outside the size filter
  if [ -n "$resume" ] && awk -F'\t' -v a="$name" -v g="$group" 'NR > 1 && $1 == a && $2 == g { found = 1 } END { exit !found }' "$tsv"; then
    continue
  fi
  nv=$(nvars_of "$f")
  case "$nv" in
    ''|*[!0-9]*) nv=0 ;;
  esac
  [ "$min_vars" -gt 0 ] && [ "$nv" -lt "$min_vars" ] && continue
  [ "$max_vars" -gt 0 ] && [ "$nv" -ge "$max_vars" ] && continue

  raw="$raw_dir/${group}__$name.out"
  t0=$(date +%s)
  if [ "$timeout_s" -gt 0 ]; then
    timeout "$timeout_s" ./mincut_lattice "$f" > "$raw" 2>&1 < /dev/null
  else
    ./mincut_lattice "$f" > "$raw" 2>&1 < /dev/null
  fi
  rc=$?
  wall=$(( $(date +%s) - t0 ))

  # [VERDICT] from the exit code or the output of mincut_lattice
  if [ "$rc" = "6" ]; then
    v=SKIP_ARITY
  elif [ "$rc" = "5" ]; then
    v=SKIP_DOMAIN
  elif grep -q 'empty CCG' "$raw"; then
    v=EMPTY
  elif [ "$rc" = "124" ]; then
    v=TIMEOUT
  elif grep -q 'skipped --' "$raw"; then
    v=SKIP_DOMAIN
  else
    v=$(sed -n 's/.*VERDICT: \([A-Z]*\).*/\1/p' "$raw" | head -1)
    [ -z "$v" ] && v="FAIL_rc$rc"
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$group" \
    "$(field 'CCG vertices n' "$raw")" "$(field 'total vertex weight W' "$raw")" \
    "$(field 'weights integral?' "$raw")" "$(field 'all-half optimal?' "$raw")" \
    "$(pct 'SAME SCC' "$raw")" "$(pct 'DIFFERENT SCCs' "$raw")" "$(pct 'decided by current rule' "$raw")" \
    "$v" "$wall" >> "$tsv"
  say "$(printf '%4d / %-4d  %-38s  %-16s  %-12s  %s s' "$i" "${#files[@]}" "${name:0:38}" "$group" "$v" "$wall")"
done

# [SUMMARY] count of every verdict, and the part at the ceiling among real verdicts
say ""
say "verdicts"
awk -F'\t' 'NR > 1 { c[$10]++; t++ } END { for (k in c) printf "  %-12s %5d  %5.1f %%\n", k, c[k], 100 * c[k] / t; printf "  total        %5d\n", t }' "$tsv" | tee -a "$log"
say ""
say "among STRUCTURAL, OPTIMAL and SELECTION"
awk -F'\t' 'NR > 1 && ($10 == "STRUCTURAL" || $10 == "OPTIMAL" || $10 == "SELECTION") { c[$10]++; t++ }
  END { if (t == 0) { print "  none"; exit }
        for (k in c) printf "  %-12s %5d  %5.1f %%\n", k, c[k], 100 * c[k] / t
        printf "  at the ceiling (STRUCTURAL + OPTIMAL) %.1f %%\n", 100 * (c["STRUCTURAL"] + c["OPTIMAL"]) / t }' "$tsv" | tee -a "$log"
say ""
say "files  $log  $tsv  $raw_dir/"
