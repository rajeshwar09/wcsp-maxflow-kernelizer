#!/usr/bin/env bash
#
# run_logged.sh -- run ANY command and keep its output in results/<date>/adhoc/
#
#   ./scripts/run_logged.sh <label> <command> [args...]
#
# Each run becomes one file, named by the time it started and the label:
#
#     results/2026-09-21/adhoc/181502_comp18-profile.txt
#
# The file starts with a header recording the exact command
#
# Examples:
#   ./scripts/run_logged.sh comp18-profile ./e2e_gpu_prof --kernelizer gpu --solver none --max-rounds 1 comp18.wcsp
#
#   for p in 10 50 100; do
#     MAXFLOW_GR_PERIOD=$p ./scripts/run_logged.sh grperiod-$p ./e2e_gpu_prof --kernelizer gpu --solver none --max-rounds 1 comp18.wcsp
#   done
#
set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

if [ $# -lt 2 ]; then
  echo "usage: $0 <label> <command> [args...]" >&2
  echo "  e.g. $0 comp18-profile ./e2e_gpu_prof --kernelizer gpu --solver none comp18.wcsp" >&2
  exit 2
fi

label="$1"
shift

mf_init "adhoc"
dir="$OUT/adhoc"
mkdir -p "$dir"
file="$dir/$(date +%H%M%S)_${label}.txt"

{
  echo "# label    : $label"
  echo "# started  : $(date -Iseconds)"
  echo "# command  : $*"
  echo "# settings : $(env | grep -E '^MAXFLOW_' | sort | tr '\n' ' ')"
  echo "# commit   : $(git rev-parse --short HEAD 2>/dev/null) ($(git status --porcelain 2>/dev/null | wc -l) dirty file(s))"
  echo "#"
} > "$file"

"$@" 2>&1 | tee -a "$file"
rc=${PIPESTATUS[0]}

echo "# exit code: $rc" >> "$file"
say "saved: $file  (exit $rc)"
exit "$rc"