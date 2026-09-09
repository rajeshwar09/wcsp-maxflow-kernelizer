#!/usr/bin/env bash
#
# run_census.sh -- measure the TRUE maximum constraint arity of every instance, from the parsed instance, without ever building the CCG
#
# Method: e2e_solve prints "[graph] max arity : N" from the parsed constraints BEFORE its arity guard runs
# With --max-arity -1 the guard then refuses everything, so the program parses, reports, and exits 6
# Nothing heavy runs and nothing crash
# Instances the loader refuses (non-Boolean or domain-1 variables) report NA with rc 5 -- collapse those first if just wanted their arity
#
set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

ART=~/mtp/wcsp-maxflow/artifact
SET="evalgm"
OUTTSV=""

usage() {
cat <<'USAGE'
Usage: ./scripts/run_census.sh [options] [set]

Sets:
  evalgm     every .wcsp under <artifact>/evalgm          (default)
  uai        every .uai under <artifact>/uai
  <dir>      any directory: every .wcsp/.uai below it

Options:
  -o, --out FILE       output table (default: data/artifact/arity-<set>.tsv)
      --artifact DIR   default: ~/mtp/wcsp-maxflow/artifact
  -h, --help

Examples:
  ./scripts/run_census.sh evalgm
  ./scripts/run_census.sh ~/mtp/wcsp-maxflow/collapsed
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out)      OUTTSV="$2"; shift 2 ;;
    --artifact)    ART="$2"; shift 2 ;;
    -h|--help)     usage; exit 0 ;;
    -*)            echo "unknown option: $1" >&2; usage; exit 2 ;;
    *)             SET="$1"; shift ;;
  esac
done

case "$SET" in
  evalgm) LIST=$(find "$ART/evalgm" -name '*.wcsp' | sort); SETNAME=evalgm ;;
  uai)    LIST=$(find "$ART/uai" -name '*.uai' | sort); SETNAME=uai ;;
  *)      if [ -d "$SET" ]; then
            LIST=$(find "$SET" \( -name '*.wcsp' -o -name '*.uai' \) | sort)
            SETNAME="$(basename "$SET")"
          else
            echo "unknown set: $SET" >&2; exit 2
          fi ;;
esac

mf_init "census_${SETNAME}"
[ -z "$OUTTSV" ] && OUTTSV="data/artifact/arity-${SETNAME}.tsv"
mkdir -p "$(dirname "$OUTTSV")"

[ -x ./e2e_solve ] || { say "error: ./e2e_solve not built (run ./scripts/run_study.sh build)"; exit 2; }

total=$(printf '%s\n' "$LIST" | grep -c .)
say "set    : $SETNAME   files: $total"
say "table  : $OUTTSV"

printf 'instance\tfamily\tmax_arity\trc\n' > "$OUTTSV"
legend_for "$OUTTSV" <<'EOF'
instance    file name without extension
family      parent folder of the instance
max_arity   TRUE maximum constraint arity, measured on the parsed instance (largest number of variables in any single constraint); NA when the loader refused the file before measuring
rc          exit code of the probe:
              6 = arity measured, then refused by the --max-arity -1 guard (this is the EXPECTED code for a successful measurement)
              5 = loader refused: a non-Boolean or domain-1 variable; run wcsp_collapse first if the file is domain-1
              2 = file could not be opened
EOF

i=0
printf '%s\n' "$LIST" | while IFS= read -r f; do
  [ -n "$f" ] || continue
  i=$((i+1))
  o=$(./e2e_solve --kernelizer none --solver none --max-arity -1 "$f" 2>&1); rc=$?
  a=$(printf '%s\n' "$o" | sed -n 's/^\[graph\] max arity *: *\([0-9][0-9]*\).*/\1/p' | head -1)
  b=$(basename "$f"); b="${b%.uai}"; b="${b%.wcsp}"
  printf '%s\t%s\t%s\t%s\n' "$b" "$(basename "$(dirname "$f")")" "${a:-NA}" "$rc" >> "$OUTTSV"
  [ $((i % 200)) -eq 0 ] && say "  ...$i/$total"
done

say ""
say "=== true max arity per family (measured instances only) ==="
awk -F'\t' 'NR>1 && $3!="NA" {if($3+0>m[$2]) m[$2]=$3+0; n[$2]++}
  END{for(k in m) printf "  %6d  %-26s (%d instances)\n", m[k], k, n[k]}' "$OUTTSV" | sort -rn | tee -a "$RUNLOG"
say ""
say "=== probe outcomes ==="
awk -F'\t' 'NR>1 {c[$4]++} END{for(k in c) printf "  rc %-3s %6d\n", k, c[k]}' "$OUTTSV" | tee -a "$RUNLOG"
say ""
say "done. table: $OUTTSV  (commit it -- it describes the benchmark)"