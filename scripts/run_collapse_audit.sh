#!/usr/bin/env bash
#
# run_collapse_audit.sh -- collapse the domain-1 instances and prove that each collapse preserved the problem
#
# For every instance it do:
#   1. ./wcsp_collapse in.wcsp out.wcsp        -- remove the domain-1 constants
#   2. scripts/audit_collapse.py in out        -- re-drive the collapse and check it: structure, folded constant, and the cost identity
#          cost_original(x, constants=0) == cost_collapsed(x) + folded_constant
#   3. optionally, for small instances, brute-force the original optimum and compare it against a real ILP solve of the collapsed file, end to end

set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

ART=~/mtp/wcsp-maxflow/artifact
DEST=~/mtp/wcsp-maxflow/collapsed
SET="domain1"
ONLY=""
LIMIT=0
SAMPLES=200
EXHMAX=12
BUDGET=2000000
BRUTEMAX=0
ILP=0
ILPMAXVARS=40
TIMEOUT=300
RESUME=1

usage() {
cat <<'USAGE'
Usage: ./scripts/run_collapse_audit.sh [options] [set]

Sets:
domain1    every instance manifest classes as domain1   (default)
<dir>      every .wcsp below a directory
<file>     text file listing one .wcsp path per line

Options:
    --dest DIR          where collapsed files go (default: ~/mtp/wcsp-maxflow/collapsed)
    --artifact DIR      default: ~/mtp/wcsp-maxflow/artifact
    --only NAMES        comma list of instance names to run (no extension)
    --limit N           stop after N instances
    --samples N         random assignments per instance   (default 200)
    --exhaustive-max N  test EVERY assignment below N vars (default 12)
    --budget N          cap on assignments x constraints  (default 2000000)
    --brute-max K       brute-force the ORIGINAL optimum when the collapsed instance has <= K variables, 0 = off (default 0)
    --ilp               also solve each collapsed file with e2e_solve --solver ilp
    --ilp-max-vars N    only ILP-solve instances up to N variables (default 40)
-t, --timeout SEC       per instance, 0 = none            (default 300)
    --fresh             ignore existing rows, start over
-h, --help

Examples:
# step 1: the four instances that exercise the tricky partial-scope path
./scripts/run_collapse_audit.sh --only rand695_l1 --brute-max 16 --ilp domain1

# step 2: the whole domain-1 group
./scripts/run_collapse_audit.sh domain1
USAGE
}

while [ $# -gt 0 ]; do
case "$1" in
  --dest)            DEST="$2"; shift 2 ;;
  --artifact)        ART="$2"; shift 2 ;;
  --only)            ONLY="$2"; shift 2 ;;
  --limit)           LIMIT="$2"; shift 2 ;;
  --samples)         SAMPLES="$2"; shift 2 ;;
  --exhaustive-max)  EXHMAX="$2"; shift 2 ;;
  --budget)          BUDGET="$2"; shift 2 ;;
  --brute-max)       BRUTEMAX="$2"; shift 2 ;;
  --ilp)             ILP=1; shift ;;
  --ilp-max-vars)    ILPMAXVARS="$2"; shift 2 ;;
  -t|--timeout)      TIMEOUT="$2"; shift 2 ;;
  --fresh)           RESUME=0; shift ;;
  -h|--help)         usage; exit 0 ;;
  -*)                echo "unknown option: $1" >&2; usage; exit 2 ;;
  *)                 SET="$1"; shift ;;
esac
done

# the instance list
# Each entry is  <absolute path><TAB><path relative to the collection root>

MANIFEST="data/artifact/manifest.tsv"
case "$SET" in
domain1)
  SETNAME=domain1
  if [ ! -s "$MANIFEST" ]; then
    echo "error: $MANIFEST not found -- run ./scripts/stage_artifact.sh first" >&2
    exit 2
  fi
  LIST=$(awk -F'\t' -v art="$ART" 'NR>1 && $6=="domain1" && $7=="yes" {print art "/" $1 "\t" $1}' "$MANIFEST")
  ;;
*)
  if [ -d "$SET" ]; then
    SETNAME="$(basename "$SET")"
    LIST=$(find "$SET" -name '*.wcsp' -type f | sort | awk -v root="$SET" '{print $0 "\t" substr($0, length(root)+2)}')
  elif [ -f "$SET" ]; then
    SETNAME="$(basename "$SET")"; SETNAME="${SETNAME%.*}"
    LIST=$(grep -v '^[[:space:]]*$' "$SET" | awk -F/ '{print $0 "\t" $(NF-1) "/" $NF}')
  else
    echo "unknown set: $SET" >&2; exit 2
  fi
  ;;
esac

if [ -n "$ONLY" ]; then
LIST=$(printf '%s\n' "$LIST" | awk -F'\t' -v want=",$ONLY," '{ n=split($1,p,"/"); b=p[n]; sub(/\.wcsp$/,"",b) if (index(want, "," b ",")) print }')
fi

mf_init "collapse_audit_${SETNAME}"
mkdir -p "$DEST"

[ -x ./wcsp_collapse ] || { say "error: ./wcsp_collapse not built (./scripts/run_study.sh build)"; exit 2; }
[ "$ILP" = "1" ] && { [ -x ./e2e_solve ] || { say "error: ./e2e_solve not built"; exit 2; }; }

TSV="$OUT/collapse_audit_${SETNAME}.tsv"
[ "$RESUME" = "0" ] && rm -f "$TSV"
if [ ! -s "$TSV" ]; then
printf 'instance\tfamily\torig_vars\tkept_vars\tdropped_vars\torig_cons\tkept_cons\tfolded_cons\tshrunk_cons\tfolded_const\taudit\tchecked\tcheck_mode\twarnings\tbrute_optimum\tilp_optimum\tend_to_end\twall_s\n' > "$TSV"
legend_for "$TSV" <<'EOF'
instance       instance name no extension
family         parent folder of original instance
orig_vars      variables in original file
kept_vars      variables left after removing the domain-1 constants
dropped_vars   how many constants were removed (orig_vars - kept_vars)
orig_cons      constraints in original file
kept_cons      constraints written to collapsed file
folded_cons    constraints whose whole scope was constants and each became one fixed number instead of a constraint
shrunk_cons    constraints that straddled constants and free variables, so their cost table had to be sliced -- THE RISKY PATH, this is the column to watch
folded_const   total fixed cost taken out of  file
audit          PASS               every check agreed: the collapsed instance is same problem minus the constants
              FAIL               a check disagreed - read the .log for which
              SKIP_NO_CONSTANTS  no domain-1 variable here, nothing to do
              SKIP_MULTIVALUED   a variable has domain >= 3; collapse refuses - no CCG method can take it
              FAIL_COLLAPSE_rcN  wcsp_collapse itself failed
              FAIL_AUDIT_rcN     auditor could not run
checked        how many assignments checked for cost identity cost_original(x, constants=0) == cost_collapsed(x) + folded_const
check_mode     exhaustive = every possible assignment was tested
              random     = a fixed-seed sample
              trivial    = nothing survived so only the constant was checked
warnings       count of input oddities noted in the log (duplicate tuples, a
              negative default cost, ...). Warnings do NOT fail the audit
brute_optimum  true optimum of the ORIGINAL model, found by trying every
              assignment. Only for tiny instances (--brute-max), else NA
ilp_optimum    optimum of the COLLAPSED file from real solve
              (e2e_solve --solver ilp). NA unless --ilp was given
end_to_end     MATCH  = brute_optimum == ilp_optimum + folded_const, i.e. the
                      whole chain agrees on the real answer
              DIFFER = they disagree -- investigate before trusting the group
              NA     = one of the two numbers was not computed
wall_s         seconds spent on this instance
EOF
fi

total=$(printf '%s\n' "$LIST" | grep -c .)
record_env "$OUT/env_collapse_audit.txt"
say "set            : $SETNAME   instances: $total"
say "collapsed into : $DEST"
say "samples        : $SAMPLES   exhaustive below: $EXHMAX vars   budget: $BUDGET"
say "brute-force    : $([ "$BRUTEMAX" -gt 0 ] && echo "<= $BRUTEMAX vars" || echo off)"
say "ilp solve      : $([ "$ILP" = "1" ] && echo "on, <= $ILPMAXVARS vars" || echo off)"
say ""

i=0
printf '%s\n' "$LIST" | while IFS=$'\t' read -r f rel; do
[ -n "$f" ] || continue
i=$((i+1))
[ "$LIMIT" -gt 0 ] && [ "$i" -gt "$LIMIT" ] && break
b="$(basename "$f")"; b="${b%.wcsp}"
fam="$(basename "$(dirname "$f")")"

if [ "$RESUME" = "1" ] && awk -F'\t' -v a="$b" 'NR>1 && $1==a {found=1} END{exit !found}' "$TSV"; then
  continue
fi
if [ ! -f "$f" ]; then say "  MISSING $b ($f)"; continue; fi

s=$(date +%s)
cf="$DEST/$rel"
mkdir -p "$(dirname "$cf")"

# 1 collapse
co="$RAW/collapse_${b}.txt"
./wcsp_collapse "$f" "$cf" > "$co" 2>&1
crc=$?

verdict=""; checked=NA; mode=NA; warn=NA; brute=NA; ilp=NA; e2e=NA
ov=NA; kv=NA; dv=NA; oc=NA; kc=NA; fc=NA; sc=NA; fk=NA

case $crc in
  0) ;;
  3) verdict=SKIP_MULTIVALUED ;;
  4) verdict=SKIP_NO_CONSTANTS ;;
  *) verdict="FAIL_COLLAPSE_rc$crc" ;;
esac

# 2 audit
if [ -z "$verdict" ]; then
  ao="$RAW/audit_${b}.txt"
  python3 scripts/audit_collapse.py "$f" "$cf" --samples "$SAMPLES" --exhaustive-max "$EXHMAX" --budget "$BUDGET" --brute "$BRUTEMAX" > "$ao" 2>&1
  arc=$?
  line="$(grep '^AUDIT ' "$ao" | tail -1)"
  if [ -z "$line" ]; then
    verdict="FAIL_AUDIT_rc$arc"
  else
    kv_of() { printf '%s\n' "$line" | sed -n "s/.*$1=\([^ ]*\).*/\1/p"; }
    verdict="$(kv_of verdict)"
    ov="$(kv_of orig_vars)";    kv="$(kv_of kept_vars)"
    dv="$(kv_of dropped_vars)"; oc="$(kv_of orig_cons)"
    kc="$(kv_of kept_cons)";    fc="$(kv_of folded_cons)"
    sc="$(kv_of shrunk_cons)";  fk="$(kv_of folded_const)"
    checked="$(kv_of checked)"; mode="$(kv_of mode)"
    warn="$(kv_of warnings)";   brute="$(kv_of brute)"
    grep -q '^MISMATCH' "$ao" && say "  --- mismatches for $b ---" && grep '^MISMATCH' "$ao" | head -5 | tee -a "$RUNLOG"
    grep '^WARNING' "$ao" | head -3 | sed "s/^/  [$b] /" | tee -a "$RUNLOG" >/dev/null
  fi
fi

# 3 optional end-to-end solve
if [ "$ILP" = "1" ] && [ "$verdict" = "PASS" ] && [ "$kv" != "NA" ] \
    && [ "$kv" -le "$ILPMAXVARS" ] && [ "$kv" -gt 0 ]; then
  so="$RAW/ilp_${b}.txt"
  if [ "$TIMEOUT" -gt 0 ]; then
    timeout "$TIMEOUT" ./e2e_solve --kernelizer none --solver ilp "$cf" > "$so" 2>&1
  else
    ./e2e_solve --kernelizer none --solver ilp "$cf" > "$so" 2>&1
  fi
  ilp="$(sed -n 's/^\[e2e\] FINAL OPTIMUM *: *//p' "$so" | head -1)"
  case "$ilp" in ''|\<*) ilp=NA ;; esac
  if [ "$brute" != "NA" ] && [ "$ilp" != "NA" ]; then
    e2e=$(awk -v b="$brute" -v i="$ilp" -v k="$fk" 'BEGIN{print (b==i+k) ? "MATCH" : "DIFFER"}')
  fi
fi

e=$(( $(date +%s) - s ))
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$b" "$fam" "$ov" "$kv" "$dv" "$oc" "$kc" "$fc" "$sc" "$fk" \
  "$verdict" "$checked" "$mode" "$warn" "$brute" "$ilp" "$e2e" "$e" >> "$TSV"

printf '  [%4d/%-4d] %-38s %-14s %-8s vars %6s -> %-6s shrunk=%-5s %ss\n' \
  "$i" "$total" "${b:0:38}" "$fam" "$verdict" "$ov" "$kv" "$sc" "$e" | tee -a "$RUNLOG"
done

# summary

say ""
say "=== audit verdicts"
awk -F'\t' 'NR>1{c[$11]++; t++} END{for(k in c) printf "  %-20s %5d  (%.1f%%)\n", k, c[k], 100*c[k]/t; printf "  -------------------- %d\n", t}' "$TSV" | sort | tee -a "$RUNLOG"
say ""
say "=== risky path: constraints sliced across constants"
awk -F'\t' 'NR>1 && $9!="NA" && $9+0>0 {n++; printf "  %-38s %-14s shrunk=%-6s audit=%s\n", $1, $2, $9, $11}
END{printf "  instances exercising it: %d\n", n+0}' "$TSV" | tee -a "$RUNLOG"
say ""
say "=== recovered size"
awk -F'\t' 'NR>1 && $11=="PASS" {ov+=$3; kv+=$4; n++}
END{if(n) printf "  %d instances PASSED: %d variables -> %d  (%.1f%% were constants)\n", n, ov, kv, 100*(ov-kv)/ov; else print "  none"}' "$TSV" | tee -a "$RUNLOG"
say ""
say "=== end-to-end optimum check"
awk -F'\t' 'NR>1 && $17!="NA" {c[$17]++} END{if(length(c)==0){print "  not run (use --ilp with --brute-max)"; exit} for(k in c) printf "  %-8s %5d\n", k, c[k]}' "$TSV" | tee -a "$RUNLOG"
say ""
say "collapsed files : $DEST"
say "done. table: $TSV"