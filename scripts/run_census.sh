#!/usr/bin/env bash
# run_census.sh
# finds the true largest constraint arity of every instance, read from the parsed instance, the CCG is never built
# how: e2e_solve is run with --max-arity -1, it parses the file, records graph.max_arity and then refuses it with code 6
# a file the loader refuses (a non Boolean or a domain-1 variable) gets NA and code 5, collapse those first with run_collapse_audit.sh
#
# usage: ./scripts/run_census.sh [options] [evalgm|uai|DIR]      (default evalgm)
#   evalgm               every .wcsp under <artifact>/evalgm
#   uai                  every .uai under <artifact>/uai
#   DIR                  every .wcsp and .uai under any folder, for example ~/mtp/wcsp-maxflow/collapsed
#   --artifact DIR       the local benchmark copy                (default ~/mtp/wcsp-maxflow/artifact)
#   -n, --name LABEL     word added to the file names
#
# output in results/<date>/:
#   census_<set>_<time>[_label].log   readable log with the largest arity per family
#   census_<set>_<time>[_label].tsv   instance, family, max_arity, rc (same columns as data/artifact/arity-evalgm.tsv)
#   census_<set>_<time>[_label]/      the e2e_solve output of every file
# to update the record in git, copy the tsv over data/artifact/arity-<set>.tsv

set -u
cd "$(dirname "$0")/.." || exit 1

# [ARGS] defaults and command line
art=~/mtp/wcsp-maxflow/artifact
set_name="evalgm"
label=""
while [ $# -gt 0 ]; do
  case "$1" in
    --artifact) art="$2"; shift 2 ;;
    -n|--name) label="_$2"; shift 2 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    -*) echo "unknown option: $1 (see --help)"; exit 1 ;;
    *) set_name="$1"; shift ;;
  esac
done

# [INSTANCES] the files of the set, in sorted order
case "$set_name" in
  evalgm) dir="$art/evalgm"; tag=evalgm; pattern='*.wcsp' ;;
  uai) dir="$art/uai"; tag=uai; pattern='*.uai' ;;
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
if [ ! -x ./e2e_solve ]; then
  echo "./e2e_solve not found, run ./scripts/build.sh core first"
  exit 1
fi

# [FILES] names of the output files of this run
stamp="census_${tag}_$(date +%H%M%S)${label}"
out_dir="results/$(date +%F)"
raw_dir="$out_dir/$stamp"
log="$out_dir/$stamp.log"
tsv="$out_dir/$stamp.tsv"
mkdir -p "$raw_dir"

say() {
  echo "$*" | tee -a "$log"
}

say "census $(date '+%F %T')  commit $(git rev-parse --short HEAD)  branch $(git rev-parse --abbrev-ref HEAD)"
say "set $tag  folder $dir  files ${#files[@]}"

# [RUN] one probe per file, the arity comes from the [m] graph.max_arity line
printf 'instance\tfamily\tmax_arity\trc\n' > "$tsv"
i=0
for f in "${files[@]}"; do
  i=$((i + 1))
  name=$(basename "$f")
  name="${name%.*}"
  family=$(basename "$(dirname "$f")")
  raw="$raw_dir/${family}__$name.out"
  ./e2e_solve --kernelizer none --solver none --max-arity -1 "$f" > "$raw" 2>&1 < /dev/null
  rc=$?
  arity=$(sed -n 's/^\[m\] graph.max_arity : //p' "$raw" | head -1)
  printf '%s\t%s\t%s\t%s\n' "$name" "$family" "${arity:-NA}" "$rc" >> "$tsv"
  [ $((i % 200)) -eq 0 ] && say "  $i / ${#files[@]}"
done

# [SUMMARY] largest arity per family and the count of every exit code
say ""
say "largest arity per family (measured files only)"
awk -F'\t' 'NR > 1 && $3 != "NA" { if ($3 + 0 > m[$2]) m[$2] = $3 + 0; n[$2]++ }
  END { for (k in m) printf "  %6d  %-26s %d files\n", m[k], k, n[k] }' "$tsv" | sort -rn | tee -a "$log"
say ""
say "exit codes (6 = measured, 5 = refused by the loader, 2 = file not readable)"
awk -F'\t' 'NR > 1 { c[$4]++ } END { for (k in c) printf "  rc %-3s %6d\n", k, c[k] }' "$tsv" | sort | tee -a "$log"
say ""
say "files  $log  $tsv  $raw_dir/"
