#!/usr/bin/env bash
# run_collapse_audit.sh
# collapses the domain-1 instances with ./wcsp_collapse and checks that every collapse kept the same problem
# for every instance:
#   1. ./wcsp_collapse in.wcsp out.wcsp       removes the domain-1 constants
#   2. scripts/audit_collapse.py in out       does the collapse again and checks structure, folded constant and
#                                             cost_original(x, constants = 0) = cost_collapsed(x) + folded_constant
#   3. optional, for tiny instances: the true optimum of the original by trying all assignments (--brute-max)
#      against an ILP solve of the collapsed file (--ilp), the two must match
#
# usage: ./scripts/run_collapse_audit.sh [options] [domain1|DIR|FILE]      (default domain1)
#   domain1               every instance that data/artifact/manifest.tsv marks as domain1 and copied
#   DIR                   every .wcsp under a folder
#   FILE                  text file with one .wcsp path per line
#       --dest DIR        where the collapsed files go              (default ~/mtp/wcsp-maxflow/collapsed)
#       --artifact DIR    the local benchmark copy                  (default ~/mtp/wcsp-maxflow/artifact)
#       --only NAMES      comma list of instance names, without extension
#       --limit N         stop after N instances
#       --samples N       random assignments per instance           (default 200)
#       --exhaustive-max N  check every assignment below N variables (default 12)
#       --budget N        limit on assignments x constraints        (default 2000000)
#       --brute-max K     true optimum of the original when the collapsed file has at most K variables, 0 = off (default 0)
#       --ilp             also solve each collapsed file with e2e_solve --solver ilp
#       --ilp-max-vars N  only ILP solve files up to N variables    (default 40)
#   -t, --timeout SEC     limit for one ILP solve, 0 = no limit     (default 300)
#       --resume FILE     continue an earlier audit tsv, instances already in it are skipped
#   -n, --name LABEL      word added to the file names
#
# output in results/<date>/:
#   collapse_audit_<set>_<time>[_label].log   one line per instance and a summary
#   collapse_audit_<set>_<time>[_label].tsv   one row per instance (or the --resume file)
#   collapse_audit_<set>_<time>[_label]/      collapse, audit and ilp output of every instance

set -u
cd "$(dirname "$0")/.." || exit 1

# [ARGS] defaults and command line
art=~/mtp/wcsp-maxflow/artifact
dest=~/mtp/wcsp-maxflow/collapsed
set_name="domain1"
only=""
limit=0
samples=200
exh_max=12
budget=2000000
brute_max=0
use_ilp=0
ilp_max_vars=40
timeout_s=300
resume=""
label=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dest) dest="$2"; shift 2 ;;
    --artifact) art="$2"; shift 2 ;;
    --only) only="$2"; shift 2 ;;
    --limit) limit="$2"; shift 2 ;;
    --samples) samples="$2"; shift 2 ;;
    --exhaustive-max) exh_max="$2"; shift 2 ;;
    --budget) budget="$2"; shift 2 ;;
    --brute-max) brute_max="$2"; shift 2 ;;
    --ilp) use_ilp=1; shift ;;
    --ilp-max-vars) ilp_max_vars="$2"; shift 2 ;;
    -t|--timeout) timeout_s="$2"; shift 2 ;;
    --resume) resume="$2"; shift 2 ;;
    -n|--name) label="_$2"; shift 2 ;;
    -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
    -*) echo "unknown option: $1 (see --help)"; exit 1 ;;
    *) set_name="$1"; shift ;;
  esac
done

# [INSTANCES] each line is <full path><TAB><path inside the collection>, the second part keeps the folder layout in --dest
manifest=data/artifact/manifest.tsv
if [ "$set_name" = "domain1" ]; then
  tag=domain1
  if [ ! -s "$manifest" ]; then
    echo "$manifest not found, run ./scripts/stage_artifact.sh first"
    exit 1
  fi
  mapfile -t list < <(awk -F'\t' -v art="$art" 'NR > 1 && $6 == "domain1" && $7 == "yes" { print art "/" $1 "\t" $1 }' "$manifest")
elif [ -d "$set_name" ]; then
  tag=$(basename "$set_name")
  mapfile -t list < <(find "$set_name" -name '*.wcsp' -type f | sort | awk -v root="$set_name" '{ print $0 "\t" substr($0, length(root) + 2) }')
elif [ -f "$set_name" ]; then
  tag=$(basename "$set_name")
  tag="${tag%.*}"
  mapfile -t list < <(grep -v '^[[:space:]]*$' "$set_name" | cut -f1 | awk -F/ '{ print $0 "\t" $(NF-1) "/" $NF }')
else
  echo "set not found: $set_name"
  exit 1
fi

# [INSTANCES] keep only the names given with --only
if [ -n "$only" ]; then
  mapfile -t list < <(printf '%s\n' "${list[@]}" | awk -F'\t' -v want=",$only," '{ n = split($1, p, "/"); b = p[n]; sub(/\.wcsp$/, "", b); if (index(want, "," b ",")) print }')
fi

# [CHECK] programs and the resume file
if [ ! -x ./wcsp_collapse ]; then
  echo "./wcsp_collapse not found, run ./scripts/build.sh core first"
  exit 1
fi
if [ "$use_ilp" = 1 ] && [ ! -x ./e2e_solve ]; then
  echo "./e2e_solve not found, run ./scripts/build.sh core first"
  exit 1
fi
if [ -n "$resume" ] && [ ! -f "$resume" ]; then
  echo "resume file not found: $resume"
  exit 1
fi

# [FILES] names of the output files of this run
stamp="collapse_audit_${tag}_$(date +%H%M%S)${label}"
out_dir="results/$(date +%F)"
raw_dir="$out_dir/$stamp"
log="$out_dir/$stamp.log"
tsv="$out_dir/$stamp.tsv"
mkdir -p "$raw_dir" "$dest"
if [ -n "$resume" ]; then
  tsv="$resume"
else
  printf 'instance\tfamily\torig_vars\tkept_vars\tdropped_vars\torig_cons\tkept_cons\tfolded_cons\tshrunk_cons\tfolded_const\taudit\tchecked\tcheck_mode\twarnings\tbrute_optimum\tilp_optimum\tend_to_end\twall_s\n' > "$tsv"
fi

say() {
  echo "$*" | tee -a "$log"
}

say "collapse audit $(date '+%F %T')  commit $(git rev-parse --short HEAD)  branch $(git rev-parse --abbrev-ref HEAD)"
say "set $tag  instances ${#list[@]}  collapsed into $dest  resume ${resume:-no}"
say "samples $samples  exhaustive below $exh_max vars  budget $budget  brute_max $brute_max  ilp $use_ilp (up to $ilp_max_vars vars)"
say "table $tsv"

i=0
for entry in "${list[@]}"; do
  f="${entry%%$'\t'*}"
  rel="${entry#*$'\t'}"
  i=$((i + 1))
  [ "$limit" -gt 0 ] && [ "$i" -gt "$limit" ] && break
  name=$(basename "$f")
  name="${name%.wcsp}"
  family=$(basename "$(dirname "$f")")

  if [ -n "$resume" ] && awk -F'\t' -v a="$name" 'NR > 1 && $1 == a { found = 1 } END { exit !found }' "$tsv"; then
    continue
  fi
  if [ ! -f "$f" ]; then
    say "  missing $name ($f)"
    continue
  fi

  t0=$(date +%s)
  cf="$dest/$rel"
  mkdir -p "$(dirname "$cf")"

  # [COLLAPSE] code 3 = a variable with domain 3 or more, code 4 = no domain-1 variable
  ./wcsp_collapse "$f" "$cf" > "$raw_dir/${name}.collapse.out" 2>&1 < /dev/null
  crc=$?
  verdict=""
  checked=NA; mode=NA; warn=NA; brute=NA; ilp=NA; e2e=NA
  ov=NA; kv=NA; dv=NA; oc=NA; kc=NA; fc=NA; sc=NA; fk=NA
  case $crc in
    0) ;;
    3) verdict=SKIP_MULTIVALUED ;;
    4) verdict=SKIP_NO_CONSTANTS ;;
    *) verdict="FAIL_COLLAPSE_rc$crc" ;;
  esac

  # [AUDIT] values come from the last "AUDIT key=value" line
  if [ -z "$verdict" ]; then
    ao="$raw_dir/${name}.audit.out"
    python3 scripts/audit_collapse.py "$f" "$cf" --samples "$samples" --exhaustive-max "$exh_max" --budget "$budget" \
      --brute "$brute_max" > "$ao" 2>&1 < /dev/null
    arc=$?
    line=$(grep '^AUDIT ' "$ao" | tail -1)
    if [ -z "$line" ]; then
      verdict="FAIL_AUDIT_rc$arc"
    else
      kv_of() {
        printf '%s\n' "$line" | sed -n "s/.* $1=\([^ ]*\).*/\1/p"
      }
      verdict=$(kv_of verdict)
      ov=$(kv_of orig_vars); kv=$(kv_of kept_vars); dv=$(kv_of dropped_vars)
      oc=$(kv_of orig_cons); kc=$(kv_of kept_cons); fc=$(kv_of folded_cons)
      sc=$(kv_of shrunk_cons); fk=$(kv_of folded_const)
      checked=$(kv_of checked); mode=$(kv_of mode); warn=$(kv_of warnings); brute=$(kv_of brute)
      if grep -q '^MISMATCH' "$ao"; then
        say "  mismatches for $name"
        grep '^MISMATCH' "$ao" | head -5 | tee -a "$log"
      fi
    fi
  fi

  # [ILP] optional solve of the collapsed file, the original optimum must be the ILP optimum plus the folded constant
  if [ "$use_ilp" = 1 ] && [ "$verdict" = "PASS" ] && [ "$kv" != "NA" ] && [ "$kv" -gt 0 ] && [ "$kv" -le "$ilp_max_vars" ]; then
    so="$raw_dir/${name}.ilp.out"
    if [ "$timeout_s" -gt 0 ]; then
      timeout "$timeout_s" ./e2e_solve --kernelizer none --solver ilp "$cf" > "$so" 2>&1 < /dev/null
    else
      ./e2e_solve --kernelizer none --solver ilp "$cf" > "$so" 2>&1 < /dev/null
    fi
    ilp=$(sed -n 's/^\[info\] final_optimum : //p' "$so" | head -1)
    case "$ilp" in
      ''|*[!0-9.eE+-]*) ilp=NA ;;
    esac
    if [ "$brute" != "NA" ] && [ "$ilp" != "NA" ]; then
      e2e=$(awk -v b="$brute" -v i="$ilp" -v k="$fk" 'BEGIN { print (b == i + k) ? "MATCH" : "DIFFER" }')
    fi
  fi

  wall=$(( $(date +%s) - t0 ))
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$family" "$ov" "$kv" "$dv" \
    "$oc" "$kc" "$fc" "$sc" "$fk" "$verdict" "$checked" "$mode" "$warn" "$brute" "$ilp" "$e2e" "$wall" >> "$tsv"
  say "$(printf '%4d / %-4d  %-38s  %-14s  %-8s  vars %6s -> %-6s  shrunk %-5s  %s s' "$i" "${#list[@]}" "${name:0:38}" "$family" "$verdict" "$ov" "$kv" "$sc" "$wall")"
done

# [SUMMARY] verdicts, the risky sliced constraints, the size saved and the end to end check
say ""
say "audit verdicts"
awk -F'\t' 'NR > 1 { c[$11]++; t++ } END { for (k in c) printf "  %-20s %5d  %5.1f %%\n", k, c[k], 100 * c[k] / t; printf "  total                %5d\n", t }' "$tsv" | sort | tee -a "$log"
say ""
say "instances with constraints sliced across constants (the risky path)"
awk -F'\t' 'NR > 1 && $9 != "NA" && $9 + 0 > 0 { n++ } END { printf "  %d\n", n + 0 }' "$tsv" | tee -a "$log"
say ""
say "size after collapse (PASS only)"
awk -F'\t' 'NR > 1 && $11 == "PASS" { ov += $3; kv += $4; n++ }
  END { if (n) printf "  %d instances: %d variables -> %d  (%.1f %% were constants)\n", n, ov, kv, 100 * (ov - kv) / ov; else print "  none" }' "$tsv" | tee -a "$log"
say ""
say "end to end optimum check"
awk -F'\t' 'NR > 1 && $17 != "NA" { c[$17]++ } END { if (length(c) == 0) { print "  not run (use --ilp with --brute-max)"; exit } for (k in c) printf "  %-8s %5d\n", k, c[k] }' "$tsv" | tee -a "$log"
say ""
say "files  $log  $tsv  $raw_dir/"
