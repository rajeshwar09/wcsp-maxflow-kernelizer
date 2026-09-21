#!/usr/bin/env bash
#
# summarize_pipeline.sh -- the summary tables for one pipeline-timing TSV.
#
#   ./scripts/summarize_pipeline.sh results/<date>/pipeline_<set>_<solver>_r<N>.tsv
#
# The summary is printed AND saved beside the table it describes:
#
#   results/<date>/pipeline_<...>.tsv            the measurements
#   results/<date>/pipeline_<...>.summary.txt    this summary
#
set -uo pipefail

TSV="${1:-}"
if [ -z "$TSV" ] || [ ! -f "$TSV" ]; then
  echo "usage: $0 <pipeline tsv>" >&2
  echo "  e.g. $0 results/2026-09-21/pipeline_kernel-effective-sample_ilp_r1.tsv" >&2
  exit 2
fi

SUMMARY="${TSV%.tsv}.summary.txt"

{
echo "=== SUMMARY of $(basename "$TSV")   (written $(date -Iseconds)) ==="
echo "    rows: $(( $(wc -l < "$TSV") - 1 ))"
echo ""
#
#  A row whose SOLVE hit --solve-limit still has perfectly valid parse, CCG, simplify and kernelize timings -- only the solve is capped
#  So the stage and kernelize summaries count every row that got THROUGH kernelization, and only the "does it pay" block restricts itself to fully solved rows
#
#  "got through kernelization" = ok | solve_limit_hit | solver_timeout

echo "=== WHERE THE TIME GOES, per kernelizer (seconds, summed over runs) ==="
echo "    every run that completed kernelization is counted; the solve column is summed over"
echo "    SOLVED runs only, because a capped solve time is a limit, not a measurement"
echo "$(printf '  %-6s %5s %10s %10s %10s %11s %11s %7s %7s' kern runs parse ccg_build simplify kernelize solve solved capped)"
awk -F'\t' 'NR>1 && ($18=="ok" || $18=="solve_limit_hit" || $18=="solver_timeout") {
    k=$4; n[k]++; p[k]+=$19; cg[k]+=$23; sm[k]+=$24; ke[k]+=$25;
    if ($18=="ok") { so[k]+=$26; ok[k]++ } else { cap[k]++ } }
  END{ for (k in n) printf "  %-6s %5d %10.2f %10.2f %10.2f %11.2f %11.2f %7d %7d\n", k, n[k], p[k], cg[k], sm[k], ke[k], so[k], ok[k]+0, cap[k]+0 }' "$TSV" | sort

echo ""
echo "=== INSIDE KERNELIZATION, per kernelizer (seconds, summed over runs) ==="
echo "$(printf '  %-6s %5s %10s %10s %11s %10s %10s %10s %11s' kern runs collect flownet MAXFLOW classify apply other kernelize)"
awk -F'\t' 'NR>1 && ($18=="ok" || $18=="solve_limit_hit" || $18=="solver_timeout") && $27!="NA" {
    k=$4; n[k]++; c[k]+=$27; b[k]+=$28; m[k]+=$29; cl[k]+=$30; a[k]+=$31; o[k]+=$32; t[k]+=$25 }
  END{ for (k in n) printf "  %-6s %5d %10.3f %10.3f %11.3f %10.3f %10.3f %10.3f %11.3f\n", k, n[k], c[k], b[k], m[k], cl[k], a[k], o[k], t[k] }' "$TSV" | sort

echo ""
echo "=== KERNELIZER SPEED, head to head (instances where lp, cpu and gpu all kernelized) ==="
echo "    per-instance mean over repeats; 'fastest' counts which kernelizer won each instance"
awk -F'\t' 'NR>1 && ($18=="ok" || $18=="solve_limit_hit" || $18=="solver_timeout") && ($4=="lp" || $4=="cpu" || $4=="gpu") {
    s[$1,$4]+=$25; c[$1,$4]++; inst[$1]=1 }
  END{ nb=0;
       for (i in inst) {
         if (c[i,"lp"]>0 && c[i,"cpu"]>0 && c[i,"gpu"]>0) {
           nb++; L=s[i,"lp"]/c[i,"lp"]; C=s[i,"cpu"]/c[i,"cpu"]; G=s[i,"gpu"]/c[i,"gpu"];
           tl+=L; tc+=C; tg+=G;
           w="lp"; b=L; if (C<b) { w="cpu"; b=C } if (G<b) { w="gpu"; b=G }
           win[w]++ } }
       if (nb==0) { print "  needs all three of lp, cpu and gpu in the same run"; exit }
       printf "  instances compared : %d\n", nb;
       printf "  fastest            : gpu %d   cpu %d   lp %d\n", win["gpu"]+0, win["cpu"]+0, win["lp"]+0;
       printf "  total kernelize    : lp %.2f s   cpu %.2f s   gpu %.2f s\n", tl, tc, tg;
       if (tg>0) printf "  gpu vs lp          : %.2fx   (above 1 means the GPU is faster)\n", tl/tg;
       if (tc>0) printf "  cpu vs lp          : %.2fx\n", tl/tc;
       if (tg>0) printf "  gpu vs cpu         : %.2fx\n", tc/tg }' "$TSV"

echo ""
echo "=== DOES KERNELIZATION PAY FOR ITSELF? ==="
echo "    solved        = runs that reached a PROVEN optimum within --solve-limit"
echo "    made solvable = instances the none control could NOT solve but this kernelizer could"
echo "    common subset = instances every configuration solved; kernelize+solve compared there only"
awk -F'\t' 'NR>1 {
    k=$4; i=$1; kset[k]=1; inst[i]=1;
    if ($18=="ok") { ok[i,k]=1; v[i,k]+=$25+$26; vc[i,k]++ } }
  END{ nc=0;
       for (i in inst) {
         all=1; for (k in kset) if (!((i,k) in ok)) all=0;
         if (all) { nc++; for (k in kset) cs[k]+=v[i,k]/vc[i,k] } }
       printf "  %-6s %8s %14s %16s %12s\n", "kern", "solved", "made solvable", "kernel+solve", "vs control";
       for (k in kset) {
         sv=0; for (i in inst) if ((i,k) in ok) sv++;
         ms="-";
         if (("none" in kset) && k!="none") { m=0; for (i in inst) if (!((i,"none") in ok) && ((i,k) in ok)) m++; ms=m }
         if (nc==0) cmp="-";
         else if (k=="none") cmp="(control)";
         else if (cs[k]>0) cmp=sprintf("%.2fx", cs["none"]/cs[k]);
         else cmp="-";
         printf "  %-6s %8d %14s %16s %12s\n", k, sv, ms, (nc>0 ? sprintf("%.3f", cs[k]) : "-"), cmp }
       printf "  instances in the common subset: %d\n", nc }' "$TSV"

echo ""
echo "=== MEASUREMENT SPREAD across repeats (only meaningful with --repeat > 1) ==="
awk -F'\t' 'NR>1 && ($18=="ok" || $18=="solve_limit_hit" || $18=="solver_timeout") {
    key=$1"|"$4; r=$25+0; if (!(key in lo) || r<lo[key]) lo[key]=r; if (r>hi[key]) hi[key]=r; c[key]++ }
  END{ worst=0; nn=0;
       for (key in c) { if (c[key]>1 && lo[key]>=1) { sp=100*(hi[key]-lo[key])/lo[key]; nn++; if (sp>worst) { worst=sp; wk=key } } }
       if (nn==0) { print "  no repeated run of 1 s or more -- re-run with --repeat 3 for a spread" }
       else { printf "  kernelize spread, runs of 1 s or more: %d configurations, worst %.1f%% (%s)\n", nn, worst, wk } }' "$TSV"

echo ""
} | tee "$SUMMARY"

echo "summary saved: $SUMMARY"