#!/usr/bin/env bash
# Generates the full benchmark instance suite with FIXED, RECORDED seeds.
#
# Sizes are WCSP variable counts. The CCG (and the bipartite double cover the
# max-flow runs on) come out far larger -- roughly 13x for mixed arity and 3x
# for pairwise -- so a 1M-variable instance means a ~26M-node flow network.
#
# Usage:
#   scripts/gen_benchmark_suite.sh            # everything
#   scripts/gen_benchmark_suite.sh small      # up to 50k only
#   scripts/gen_benchmark_suite.sh 06_300k    # one instance by name
set -euo pipefail

OUT=data/bench
mkdir -p "$OUT"

#  name                N         M         seed  arity
SUITE=(
  "01_10               10        15        1001  mixed"
  "02_500              500       750       1002  mixed"
  "03_1k               1000      1500      1003  mixed"
  "04_10k              10000     15000     1004  mixed"
  "05_50k              50000     75000     1005  mixed"
  "06_300k             300000    450000    1006  mixed"
  "07_300k_pairwise    300000    450000    1007  pairwise"
  "08_500k             500000    750000    1008  mixed"
  "09_500k_pairwise    500000    750000    1009  pairwise"
  "10_1M               1000000   1500000   1010  mixed"
  "11_1M_pairwise      1000000   1500000   1011  pairwise"
)

SMALL_MAX=50000
FILTER="${1:-all}"

for row in "${SUITE[@]}"; do
  read -r name n m seed arity <<< "$row"

  case "$FILTER" in
    all)   ;;
    small) [ "$n" -le "$SMALL_MAX" ] || continue ;;
    *)     [ "$name" = "$FILTER" ] || continue ;;
  esac

  f="$OUT/$name.wcsp"
  if [ -f "$f" ]; then
    echo "skip   $name  (exists)"
    continue
  fi

  if [ "$arity" = "pairwise" ]; then
    script=scripts/gen_wcsp_lowarity.py
  else
    script=scripts/gen_wcsp.py
  fi

  echo "gen    $name  N=$n M=$m seed=$seed arity=$arity"
  t0=$SECONDS
  python3 "$script" "$n" "$m" "$seed" "$f"
  echo "       $((SECONDS - t0)) s, $(du -h "$f" | cut -f1)"
done

#  Manifest: seeds plus a checksum, so a result can always be traced back to
#  the exact bytes it was produced from.
{
  echo "# benchmark suite manifest"
  echo "# name  N  M  seed  arity  sha256(16)  bytes"
  for row in "${SUITE[@]}"; do
    read -r name n m seed arity <<< "$row"
    f="$OUT/$name.wcsp"
    [ -f "$f" ] || continue
    printf '%-18s %8s %8s %6s %-9s %s %s\n' \
      "$name" "$n" "$m" "$seed" "$arity" \
      "$(sha256sum "$f" | cut -c1-16)" "$(stat -c%s "$f")"
  done
} > "$OUT/MANIFEST.txt"

echo
cat "$OUT/MANIFEST.txt"