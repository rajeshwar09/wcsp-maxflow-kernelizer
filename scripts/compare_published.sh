#!/usr/bin/env bash
#
# compare_published.sh <our.tsv> [nt-reduction.txt]
#
# Joins our per-instance results against the published NT figures and reports agreement. The published numbers are resolved VARIABLES after ONE round, so the
# input TSV must come from a --max-rounds 1 run for the MATCH column to mean anything
#
set -uo pipefail

TSV="${1:?usage: compare_published.sh <our.tsv> [nt-reduction.txt]}"
REF="${2:-$HOME/mtp/wcsp-maxflow/artifact/nt-reduction.txt}"

awk -F'[,\t]' -v OFS='\t' '
  FNR==NR {
    if (FNR==1) next
    n=split($1,a,"/"); key=a[n]
    gsub(/^[ \t]+|[ \t]+$/,"",$2); gsub(/^[ \t]+|[ \t]+$/,"",$3)
    ref[key]=$2+0; reftot[key]=$2+$3
    next
  }
  FNR==1 { print "instance","published","ours","published_tot","our_tot","verdict"; next }
  {
    key=$1
    if (!(key in ref)) { unmatched++; next }
    ours = ($9=="NA" ? -1 : $9+0)
    v = (ours<0) ? "no_result" : (ours==ref[key] ? "MATCH" : "DIFFER")
    if (v=="MATCH") m++; else if (v=="DIFFER") d++; else nr++
    print key, ref[key], (ours<0?"-":ours), reftot[key], $3, v
  }
  END {
    printf "\n  MATCH      : %d\n  DIFFER     : %d\n  no result  : %d\n  not in ref : %d\n",
           m+0, d+0, nr+0, unmatched+0 > "/dev/stderr"
  }
' "$REF" "$TSV" | column -t -s$'\t' 2>/dev/null || cat