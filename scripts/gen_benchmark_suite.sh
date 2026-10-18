#!/usr/bin/env bash
# gen_benchmark_suite.sh
# makes the synthetic WCSP instances with fixed seeds and writes a MANIFEST.txt with the seed and checksum of every file
# two suites, both made by scripts/gen_wcsp.py:
#   bench : data/bench/, 01_10 to 12_1_3M
#   wcsp  : data/wcsp/, the older suite, 03_100k from it is used in the paper set
# sizes are WCSP variables, the flow network is much larger (about 13x for mixed arity, 3x for pairwise)
# a file that is already there is not made again, only its manifest line is written
#
# usage: ./scripts/gen_benchmark_suite.sh [bench|wcsp] [all|small|NAME]
#   ./scripts/gen_benchmark_suite.sh                  every instance of the bench suite
#   ./scripts/gen_benchmark_suite.sh bench small      bench instances up to 50k variables
#   ./scripts/gen_benchmark_suite.sh wcsp 03_100k     one instance by name

set -u
cd "$(dirname "$0")/.." || exit 1

# [ARGS] suite and filter
suite="${1:-bench}"
filter="${2:-all}"

# [SUITE] name  variables  constraints  seed  arity
case "$suite" in
  bench)
    out=data/bench
    rows=(
      "01_10             10       15       1001  mixed"
      "02_500            500      750      1002  mixed"
      "03_1k             1000     1500     1003  mixed"
      "04_10k            10000    15000    1004  mixed"
      "05_50k            50000    75000    1005  mixed"
      "06_300k           300000   450000   1006  mixed"
      "07_300k_pairwise  300000   450000   1007  pairwise"
      "08_500k           500000   750000   1008  mixed"
      "09_500k_pairwise  500000   750000   1009  pairwise"
      "10_1M             1000000  1500000  1010  mixed"
      "11_1M_pairwise    1000000  1500000  1011  pairwise"
      "12_1_3M           1300000  1950000  1013  mixed"
    )
    ;;
  wcsp)
    out=data/wcsp
    rows=(
      "01_20             20       30       1     mixed"
      "02_500            500      800      2     mixed"
      "03_100k           100000   150000   3     mixed"
      "04_300k           300000   450000   4     mixed"
      "05_300k_pairwise  300000   450000   4     pairwise"
      "06_1M             1000000  1500000  5     mixed"
      "07_1M_pairwise    1000000  1500000  5     pairwise"
    )
    ;;
  -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
  *) echo "unknown suite: $suite (bench or wcsp)"; exit 1 ;;
esac
mkdir -p "$out"

# [GEN] make the missing files
for row in "${rows[@]}"; do
  read -r name n m seed arity <<< "$row"
  case "$filter" in
    all) ;;
    small) [ "$n" -le 50000 ] || continue ;;
    *) [ "$name" = "$filter" ] || continue ;;
  esac

  f="$out/$name.wcsp"
  if [ -f "$f" ]; then
    echo "skip  $name (already there)"
    continue
  fi
  flag=""
  [ "$arity" = "pairwise" ] && flag="--pairwise"
  echo "gen   $name  variables $n  constraints $m  seed $seed  $arity"
  t0=$SECONDS
  python3 scripts/gen_wcsp.py "$n" "$m" "$seed" "$f" $flag > /dev/null
  echo "      $((SECONDS - t0)) s, $(du -h "$f" | cut -f1)"
done

# [MANIFEST] seed and checksum of every file that exists, so a result can be traced back to the exact file
{
  echo "# $suite suite manifest, made by scripts/gen_benchmark_suite.sh"
  echo "# name  variables  constraints  seed  arity  sha256(16)  bytes"
  for row in "${rows[@]}"; do
    read -r name n m seed arity <<< "$row"
    f="$out/$name.wcsp"
    [ -f "$f" ] || continue
    printf '%-18s %8s %8s %6s %-9s %s %s\n' "$name" "$n" "$m" "$seed" "$arity" \
      "$(sha256sum "$f" | cut -c1-16)" "$(stat -c%s "$f")"
  done
} > "$out/MANIFEST.txt"

echo
cat "$out/MANIFEST.txt"
