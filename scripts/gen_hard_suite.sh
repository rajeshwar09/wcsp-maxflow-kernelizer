#!/usr/bin/env bash
# Generates the "hard instance" suite into data/bench/hard/ with fixed seeds.
#
# This sweeps the two standard random-CSP hardness parameters, tightness and density, plus the cost model, looking for settings where the LP comes out
# fractional. Which settings those are is not known in advance, so the CODE is a PROBE: generate cheaply, measure the LP reduction, then run full solves only
# on the settings that turn out to be hard
#
# Usage:
#   scripts/gen_hard_suite.sh probe     small instances for the parameter sweep
#   scripts/gen_hard_suite.sh scale     larger instances at the chosen settings
#   scripts/gen_hard_suite.sh           both
set -euo pipefail

OUT=data/bench/hard
mkdir -p "$OUT"
GEN=scripts/gen_wcsp_hard.py

#  name                       n     m      seed  arity model    tight hi topology
PROBE=(
  "p_bin_t25                  2000  3000   5001  2     binary   0.25  1  random"
  "p_bin_t50                  2000  3000   5002  2     binary   0.50  1  random"
  "p_bin_t75                  2000  3000   5003  2     binary   0.75  1  random"
  "p_narrow_t50               2000  3000   5005  2     narrow   0.50  3  random"
  "p_uniform_ctrl             2000  3000   5006  2     uniform  0.50 10  random"
  "p_bin_t50_d1               2000  2000   5007  2     binary   0.50  1  random"
  "p_bin_t50_d3               2000  6000   5008  2     binary   0.50  1  random"
  "p_bin_t50_d5               2000  10000  5009  2     binary   0.50  1  random"
  "p_bin_t50_scalefree        2000  3000   5010  2     binary   0.50  1  scalefree"
  "p_bin_t50_clustered        2000  3000   5011  2     binary   0.50  1  clustered"
  "p_bin_t50_a3               2000  3000   5012  3     binary   0.50  1  random"
  "p_bin_t10                  2000  3000   5013  2     binary   0.10  1  random"
  "p_bin_t15                  2000  3000   5014  2     binary   0.15  1  random"
  "p_bin_t20                  2000  3000   5015  2     binary   0.20  1  random"
  "p_bin_t30                  2000  3000   5016  2     binary   0.30  1  random"
  "p_bin_t35                  2000  3000   5017  2     binary   0.35  1  random"
  "p_bin_t40                  2000  3000   5018  2     binary   0.40  1  random"
  "q_t30_s1  2000 3000 6001 2 binary 0.30 1 random"
  "q_t30_s2  2000 3000 6002 2 binary 0.30 1 random"
  "q_t30_s3  2000 3000 6003 2 binary 0.30 1 random"
  "q_t35_s1  2000 3000 6011 2 binary 0.35 1 random"
  "q_t35_s2  2000 3000 6012 2 binary 0.35 1 random"
  "q_t35_s3  2000 3000 6013 2 binary 0.35 1 random"
  "q_t37_s1  2000 3000 6021 2 binary 0.37 1 random"
  "q_t37_s2  2000 3000 6022 2 binary 0.37 1 random"
  "q_t37_s3  2000 3000 6023 2 binary 0.37 1 random"
  "q_t40_s1  2000 3000 6031 2 binary 0.40 1 random"
  "q_t40_s2  2000 3000 6032 2 binary 0.40 1 random"
  "q_t40_s3  2000 3000 6033 2 binary 0.40 1 random"
  "q_t45_s1  2000 3000 6041 2 binary 0.45 1 random"
  "q_t45_s2  2000 3000 6042 2 binary 0.45 1 random"
  "q_t45_s3  2000 3000 6043 2 binary 0.45 1 random"
)

#  Larger sizes, generated once a hard setting is identified.
SCALE=(
  "s_bin_t50_10k              10000  15000  5101  2    binary   0.50  1  random"
  "s_bin_t50_50k              50000  75000  5102  2    binary   0.50  1  random"
  "s_bin_t50_100k             100000 150000 5103  2    binary   0.50  1  random"
)

emit() {
  local row="$1"
  read -r name n m seed arity model tight hi topo <<< "$row"
  local f="$OUT/$name.wcsp"
  if [ -f "$f" ]; then echo "skip   $name (exists)"; return; fi
  echo "gen    $name"
  python3 "$GEN" -n "$n" -m "$m" --seed "$seed" -o "$f" \
      --arity "$arity" --cost-model "$model" --tightness "$tight" \
      --cost-hi "$hi" --topology "$topo"
}

MODE="${1:-all}"
case "$MODE" in
  probe) for r in "${PROBE[@]}"; do emit "$r"; done ;;
  scale) for r in "${SCALE[@]}"; do emit "$r"; done ;;
  all)   for r in "${PROBE[@]}" "${SCALE[@]}"; do emit "$r"; done ;;
  *)     echo "usage: $0 [probe|scale|all]"; exit 1 ;;
esac

{
  echo "# hard instance suite manifest"
  echo "# name  n  m  seed  arity  model  tightness  cost_hi  topology  sha256(16)  bytes"
  for r in "${PROBE[@]}" "${SCALE[@]}"; do
    read -r name n m seed arity model tight hi topo <<< "$r"
    f="$OUT/$name.wcsp"
    [ -f "$f" ] || continue
    printf '%-26s %7s %7s %6s %2s %-8s %5s %3s %-10s %s %s\n' \
      "$name" "$n" "$m" "$seed" "$arity" "$model" "$tight" "$hi" "$topo" \
      "$(sha256sum "$f" | cut -c1-16)" "$(stat -c%s "$f")"
  done
} > "$OUT/MANIFEST.txt"

echo
cat "$OUT/MANIFEST.txt"