#!/usr/bin/env bash
# gen_hard_suite.sh
# makes the "hard" WCSP instances in data/bench/hard/ with fixed seeds, made by scripts/gen_wcsp_hard.py
# these instances try to make the vertex cover LP fractional (ties and odd cycles), so the kernelizer cannot decide everything
# probe : small instances that sweep tightness, density, cost model and topology
# scale : larger instances at the setting picked from the probe
# a file that is already there is not made again
#
# usage: ./scripts/gen_hard_suite.sh [probe|scale|all]     (default all)

set -u
cd "$(dirname "$0")/.." || exit 1

out=data/bench/hard
mkdir -p "$out"

# [SUITE] name  n  m  seed  arity  cost_model  tightness  cost_hi  topology
probe=(
  "p_bin_t25            2000  3000   5001  2  binary   0.25  1   random"
  "p_bin_t50            2000  3000   5002  2  binary   0.50  1   random"
  "p_bin_t75            2000  3000   5003  2  binary   0.75  1   random"
  "p_narrow_t50         2000  3000   5005  2  narrow   0.50  3   random"
  "p_uniform_ctrl       2000  3000   5006  2  uniform  0.50  10  random"
  "p_bin_t50_d1         2000  2000   5007  2  binary   0.50  1   random"
  "p_bin_t50_d3         2000  6000   5008  2  binary   0.50  1   random"
  "p_bin_t50_d5         2000  10000  5009  2  binary   0.50  1   random"
  "p_bin_t50_scalefree  2000  3000   5010  2  binary   0.50  1   scalefree"
  "p_bin_t50_clustered  2000  3000   5011  2  binary   0.50  1   clustered"
  "p_bin_t50_a3         2000  3000   5012  3  binary   0.50  1   random"
  "p_bin_t10            2000  3000   5013  2  binary   0.10  1   random"
  "p_bin_t15            2000  3000   5014  2  binary   0.15  1   random"
  "p_bin_t20            2000  3000   5015  2  binary   0.20  1   random"
  "p_bin_t30            2000  3000   5016  2  binary   0.30  1   random"
  "p_bin_t35            2000  3000   5017  2  binary   0.35  1   random"
  "p_bin_t40            2000  3000   5018  2  binary   0.40  1   random"
  "q_t30_s1             2000  3000   6001  2  binary   0.30  1   random"
  "q_t30_s2             2000  3000   6002  2  binary   0.30  1   random"
  "q_t30_s3             2000  3000   6003  2  binary   0.30  1   random"
  "q_t35_s1             2000  3000   6011  2  binary   0.35  1   random"
  "q_t35_s2             2000  3000   6012  2  binary   0.35  1   random"
  "q_t35_s3             2000  3000   6013  2  binary   0.35  1   random"
  "q_t37_s1             2000  3000   6021  2  binary   0.37  1   random"
  "q_t37_s2             2000  3000   6022  2  binary   0.37  1   random"
  "q_t37_s3             2000  3000   6023  2  binary   0.37  1   random"
  "q_t40_s1             2000  3000   6031  2  binary   0.40  1   random"
  "q_t40_s2             2000  3000   6032  2  binary   0.40  1   random"
  "q_t40_s3             2000  3000   6033  2  binary   0.40  1   random"
  "q_t45_s1             2000  3000   6041  2  binary   0.45  1   random"
  "q_t45_s2             2000  3000   6042  2  binary   0.45  1   random"
  "q_t45_s3             2000  3000   6043  2  binary   0.45  1   random"
)
scale=(
  "s_bin_t50_10k        10000   15000   5101  2  binary  0.50  1  random"
  "s_bin_t50_50k        50000   75000   5102  2  binary  0.50  1  random"
  "s_bin_t50_100k       100000  150000  5103  2  binary  0.50  1  random"
)

# [GEN] make one instance from its row
make_one() {
  read -r name n m seed arity model tight hi topo <<< "$1"
  local f="$out/$name.wcsp"
  if [ -f "$f" ]; then
    echo "skip  $name (already there)"
    return
  fi
  echo "gen   $name"
  python3 scripts/gen_wcsp_hard.py -n "$n" -m "$m" --seed "$seed" -o "$f" --arity "$arity" \
    --cost-model "$model" --tightness "$tight" --cost-hi "$hi" --topology "$topo" > /dev/null
}

case "${1:-all}" in
  probe) for r in "${probe[@]}"; do make_one "$r"; done ;;
  scale) for r in "${scale[@]}"; do make_one "$r"; done ;;
  all) for r in "${probe[@]}" "${scale[@]}"; do make_one "$r"; done ;;
  -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
  *) echo "usage: $0 [probe|scale|all]"; exit 1 ;;
esac

# [MANIFEST] settings, seed and checksum of every file that exists
{
  echo "# hard suite manifest, made by scripts/gen_hard_suite.sh"
  echo "# name  n  m  seed  arity  model  tightness  cost_hi  topology  sha256(16)  bytes"
  for r in "${probe[@]}" "${scale[@]}"; do
    read -r name n m seed arity model tight hi topo <<< "$r"
    f="$out/$name.wcsp"
    [ -f "$f" ] || continue
    printf '%-26s %7s %7s %6s %2s %-8s %5s %3s %-10s %s %s\n' "$name" "$n" "$m" "$seed" "$arity" "$model" \
      "$tight" "$hi" "$topo" "$(sha256sum "$f" | cut -c1-16)" "$(stat -c%s "$f")"
  done
} > "$out/MANIFEST.txt"

echo
cat "$out/MANIFEST.txt"
