#!/usr/bin/env bash
#
# compare_kernelizers.sh -- side-by-side kernelizer timings for one kernelizer table.
#
#   ./scripts/compare_kernelizers.sh results/<date>/kernelizer_<set>.tsv
#
# Reads a table written by run_study.sh compare (one row per instance per
# kernelizer) and prints one row per instance: CCG size, the kernelizer time for
# lp, cpu and gpu, and which was fastest. Nothing is re-run.
#
# The comparison is printed AND saved beside the table:
#   results/<date>/kernelizer_<set>.comparison.txt
#
set -uo pipefail

TSV="${1:-}"
if [ -z "$TSV" ] || [ ! -f "$TSV" ]; then
  echo "usage: $0 <kernelizer tsv>" >&2
  echo "  e.g. $0 results/2026-09-28/kernelizer_family-a.tsv" >&2
  exit 2
fi

OUTFILE="${TSV%.tsv}.comparison.txt"

{
  echo "=== KERNELIZER COMPARISON: $(basename "$TSV")   (written $(date -Iseconds)) ==="
  echo "    kernelizer time only (kern_time_s), seconds; sorted by CCG size"
  echo ""
  awk -F'\t' '
    NR>1 && $8!="NA" { t[$1,$2]=$8+0; have[$1,$2]=1; v[$1]=$4; inst[$1]=1 }
    END {
      printf "%-26s %10s %10s %10s %10s %8s\n", "instance", "ccg_vert", "lp_s", "cpu_s", "gpu_s", "fastest"
      #  rows go through sort (by CCG size); close() flushes them before the tally
      srt = "sort -k2 -n"
      ni = 0
      for (i in inst) {
        ni++
        w="-"; b=1e30
        if (((i,"lp")  in have) && t[i,"lp"]  < b) { w="lp";  b=t[i,"lp"]  }
        if (((i,"cpu") in have) && t[i,"cpu"] < b) { w="cpu"; b=t[i,"cpu"] }
        if (((i,"gpu") in have) && t[i,"gpu"] < b) { w="gpu"; b=t[i,"gpu"] }
        L = ((i,"lp")  in have) ? sprintf("%.3f", t[i,"lp"])  : "FAILED"
        C = ((i,"cpu") in have) ? sprintf("%.3f", t[i,"cpu"]) : "FAILED"
        G = ((i,"gpu") in have) ? sprintf("%.3f", t[i,"gpu"]) : "FAILED"
        printf "%-26s %10s %10s %10s %10s %8s\n", substr(i,1,26), v[i], L, C, G, w | srt
        wins[w]++
      }
      close(srt)
      printf "\nfastest: gpu %d   cpu %d   lp %d   (of %d instances)\n", wins["gpu"]+0, wins["cpu"]+0, wins["lp"]+0, ni
    }' "$TSV"
} | tee "$OUTFILE"

echo ""
echo "comparison saved: $OUTFILE"