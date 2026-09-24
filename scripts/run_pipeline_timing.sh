#!/usr/bin/env bash
#
# run_pipeline_timing.sh - full-pipeline timing breakdown, one row per run
#
set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

ART=~/mtp/wcsp-maxflow/artifact
SET="uai"
KERNS="none,lp,cpu,gpu"
SOLVER="ilp"
PERTURB="off"
SEED=1
MAXROUNDS=1
TIMEOUT=900
MEMGB=18
MAXVARS=0
LIMIT=0
REPEAT=1
RESUME=1

usage() {
cat <<'USAGE'
Usage: ./scripts/run_pipeline_timing.sh [options] [set]

Sets:
  uai        the Boolean UAI instances
  evalgm     the Boolean evalgm instances
  <dir>      every .wcsp/.uai below a directory
  <file>     a text file listing one instance path per line

Options:
  -k, --kernelizers LIST   comma list, none is the control (default: none,lp,cpu,gpu)
  -s, --solver NAME        none | mp | ilp   exact solve after kernelizing (default: ilp)
  -p, --perturb MODE       off | int | real                     (default: off)
      --seed S             perturbation seed                    (default: 1)
  -r, --max-rounds N       kernelization rounds, 100 = to fixed point (default: 1)
  -t, --timeout SEC        per run, 0 = none                    (default: 900)
  -m, --mem-limit GB       address-space cap, 0 = none          (default: 18)
      --max-vars N         skip instances above N variables
      --limit N            stop after N instances
      --repeat N           run each configuration N times       (default: 1)
      --fresh              ignore existing rows, start over
      --artifact DIR       default: ~/mtp/wcsp-maxflow/artifact
  -h, --help

Examples:
  ./scripts/run_pipeline_timing.sh -k none,lp,cpu,gpu -s ilp data/artifact/timing-large.list

  # kernelizer cost only, no exact solve, 3 repeats
  ./scripts/run_pipeline_timing.sh -k cpu,gpu -s none --repeat 3 data/artifact/timing-large.list
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -k|--kernelizers) KERNS="$2"; shift 2 ;;
    -s|--solver)      SOLVER="$2"; shift 2 ;;
    -p|--perturb)     PERTURB="$2"; shift 2 ;;
    --seed)           SEED="$2"; shift 2 ;;
    -r|--max-rounds)  MAXROUNDS="$2"; shift 2 ;;
    -t|--timeout)     TIMEOUT="$2"; shift 2 ;;
    -m|--mem-limit)   MEMGB="$2"; shift 2 ;;
    --max-vars)       MAXVARS="$2"; shift 2 ;;
    --limit)          LIMIT="$2"; shift 2 ;;
    --repeat)         REPEAT="$2"; shift 2 ;;
    --fresh)          RESUME=0; shift ;;
    --artifact)       ART="$2"; shift 2 ;;
    -h|--help)        usage; exit 0 ;;
    -*)               echo "unknown option: $1" >&2; usage; exit 2 ;;
    *)                SET="$1"; shift ;;
  esac
done

case "$SET" in
  uai|evalgm) SETNAME="$SET" ;;
  *) SETNAME="$(basename "${SET%/}")"; SETNAME="${SETNAME%.*}" ;;
esac

mf_init "pipeline_${SETNAME}"
mkdir -p data/artifact

# instance list
#
# Boolean domains only, checked the same way run_artifact.sh checks them: the per-variable domain sizes on line 2 of a .wcsp or line 3 of a .uai, not the
# max-domain field on line 1 (which is 2 even when the file also has domain-1 variables that the loader refuses)

boolean_only() {
  local f="$1"
  case "$f" in
    *.uai)
      [ "$(sed -n '3p' "$f" | tr ' ' '\n' | grep -v '^$' | sort -u | tr -d '\n')" = "2" ] ;;
    *)
      [ "$(sed -n '2p' "$f" | tr ' ' '\n' | grep -v '^$' | sort -u | tr -d '\n')" = "2" ] ;;
  esac
}

nvars_of() {
  case "$1" in
    *.uai) sed -n '2p' "$1" | tr -d ' \r' ;;
    *)     head -1 "$1" | awk '{print $2}' ;;
  esac
}

case "$SET" in
  uai)    SRC="$(find "$ART/uai" -name '*.uai' -type f | sort)" ;;
  evalgm) SRC="$(find "$ART/evalgm" -name '*.wcsp' -type f | sort)" ;;
  *)
    if [ -d "$SET" ]; then
      SRC="$(find "$SET" \( -name '*.wcsp' -o -name '*.uai' \) -type f | sort)"
    elif [ -f "$SET" ]; then
      SRC="$(grep -v '^[[:space:]]*$' "$SET")"
    else
      echo "unknown set: $SET (not a built-in name, a directory or a file)" >&2; exit 2
    fi ;;
esac

LIST="$OUT/${SETNAME}.pipeline.list"
: > "$LIST"
printf '%s\n' "$SRC" | while IFS= read -r f; do
  [ -n "$f" ] || continue
  if [ ! -f "$f" ]; then say "  missing: $f"; continue; fi
  if boolean_only "$f"; then
    printf '%s\t%s\n' "$f" "$(nvars_of "$f")" >> "$LIST"
  fi
done
TOTAL=$(wc -l < "$LIST")

TSV="$OUT/pipeline_${SETNAME}_${SOLVER}_r${MAXROUNDS}.tsv"
[ "$RESUME" = "0" ] && rm -f "$TSV"

HDR='instance	rep	format	kernelizer	solver	perturb	nvars	rounds	simplified	by_kernelizer	resolved_total	var_reduction_pct	ccg_vertices_before	ccg_edges_before	ccg_vertices_after	vertex_reduction_pct	final_optimum	status	t_parse	t_topolynomial	t_addpolynomial	t_getgraph_copy	t_ccg_build	t_simplify	t_kernelize	t_solve	k_collect	k_build_flownet	k_maxflow	k_classify	k_apply	k_other	peak_kb	wall_s	t_total'

if [ ! -s "$TSV" ]; then
  printf '%s\n' "$HDR" > "$TSV"
  legend_for "$TSV" <<'EOF'
IDENTITY AND SIZE
instance              file name of the instance
rep                   repeat number, 1..N (--repeat). Same work each time; the spread is the measurement noise
format                dimacs (.wcsp) or uai
kernelizer            none = THE CONTROL, no kernelization at all
                      lp   = Gurobi's linear-programming kernelizer (the baseline we replace)
                      cplex = IBM CPLEX's linear-programming kernelizer (second commercial baseline)
                      cpu  = our max-flow kernelizer on the CPU
                      gpu  = our max-flow kernelizer on the GPU
solver                exact solver run after kernelizing: none | mp (message passing) | ilp (Gurobi)
perturb               weight perturbation: off | int | real
nvars                 WCSP variables in the instance
rounds                kernelization rounds actually run
simplified            variables resolved by simplify() BEFORE any kernelizer
by_kernelizer         variables resolved by the kernelizer itself
resolved_total        simplified + by_kernelizer
var_reduction_pct     100 * resolved_total / nvars
ccg_vertices_before   CCG vertices before kernelization
ccg_edges_before      CCG edges before kernelization
ccg_vertices_after    CCG vertices after kernelization
vertex_reduction_pct  CCG vertex reduction in percent
final_optimum         objective from the exact solve, or a marker if it did not finish
status                ok | solver_timeout | skip_domain | skip_arity | timeout | oom | fail_rcN

PIPELINE STAGE TIMES, seconds  (these are the stages of the whole run)
t_parse               read the .wcsp/.uai file and build the WCSP instance
t_topolynomial        convert each constraint to its polynomial form
t_addpolynomial       add those polynomials into the constraint composite graph
t_getgraph_copy       take the working copy of the CCG
t_ccg_build           t_topolynomial + t_addpolynomial + t_getgraph_copy
                      i.e. the whole cost of CONSTRUCTING the CCG
t_simplify            simplify() pass that resolves the easy variables before kernelizing
t_kernelize           all kernelization rounds together
t_solve               the exact solve on whatever the kernelizer left behind

KERNELIZE SUB-STEPS, seconds  (summed over every round)
                      Filled for cpu and gpu. NA for lp, whose kernelizer is
                      third-party code we do not instrument, and NA for none.
k_collect             walk the CCG and build the vertex index mapping
k_build_flownet       build the bipartite double-cover flow network
k_maxflow             the max-flow solve itself -- the part the GPU accelerates
k_classify            read the min-cut and decide each variable 0 / 1 / undecided
k_apply               delete the decided vertices from the CCG
k_other               t_kernelize minus the five above: per-round bookkeeping not
                      inside any phase. Should be small; if it is not, something
                      unaccounted is expensive

RESOURCES AND TOTAL
peak_kb               peak resident memory in kB
wall_s                wall-clock seconds measured by this script, process start to exit
t_total               the program's own end-to-end total. ALWAYS THE LAST COLUMN
EOF
fi

record_env "$OUT/env_pipeline.txt"
say "set           : $SETNAME  ($TOTAL Boolean instances)"
say "kernelizers   : $KERNS"
say "solver        : $SOLVER"
say "perturb       : $PERTURB  seed=$SEED"
say "max rounds    : $MAXROUNDS"
say "repeats       : $REPEAT"
say "timeout       : $TIMEOUT s    mem limit: ${MEMGB} GiB"
say "table         : $TSV"
say ""

#  Pull "[stage] NAME : VALUE s" / "[time] NAME : VALUE s" style lines out of the captured output. Prints nothing when the line is absent, so the caller's
#  default of NA survives.
g_field() { sed -n "s/^\[$1\] $2 *: *//p" "$3" | head -1 | sed 's/ s$//' | tr -d '\r'; }

IFS_SAVE="$IFS"
rep=1
while [ "$rep" -le "$REPEAT" ]; do
  IFS=','
  for k in $KERNS; do
    IFS="$IFS_SAVE"

    bin="./e2e_solve"
    [ "$k" = "gpu" ] && bin="./e2e_solve_gpu"
    if [ ! -x "$bin" ]; then say "SKIP kernelizer $k ($bin not built)"; IFS=','; continue; fi

    say "=== rep $rep / kernelizer $k / solver $SOLVER / rounds $MAXROUNDS ==="

    n=0
    while IFS=$'\t' read -r f nv; do
      name="$(basename "$f")"
      n=$((n+1))
      [ "$LIMIT" -gt 0 ] && [ "$n" -gt "$LIMIT" ] && break
      [ "$MAXVARS" -gt 0 ] && [ "$nv" -gt "$MAXVARS" ] && continue

      if [ "$RESUME" = "1" ] && awk -F'\t' -v a="$name" -v r="$rep" -v kk="$k" 'NR>1 && $1==a && $2==r && $4==kk {found=1} END{exit !found}' "$TSV"; then
        continue
      fi

      out="$RAW/pipe_${k}_${rep}_${name}.txt"
      res="$RAW/.res.$$"

      cmd=("$bin" --kernelizer "$k" --solver "$SOLVER" --max-rounds "$MAXROUNDS"
           --perturb "$PERTURB" --seed "$SEED" "$f")

      if [ "$MEMGB" -gt 0 ]; then
        ( ulimit -v $((MEMGB * 1024 * 1024))
          if [ "$TIMEOUT" -gt 0 ]; then
            /usr/bin/time -v -o "$res" timeout -k 60 "$TIMEOUT" "${cmd[@]}" < /dev/null > "$out" 2>&1
          else
            /usr/bin/time -v -o "$res" "${cmd[@]}" < /dev/null > "$out" 2>&1
          fi )
      else
        if [ "$TIMEOUT" -gt 0 ]; then
          /usr/bin/time -v -o "$res" timeout -k 60 "$TIMEOUT" "${cmd[@]}" < /dev/null > "$out" 2>&1
        else
          /usr/bin/time -v -o "$res" "${cmd[@]}" < /dev/null > "$out" 2>&1
        fi
      fi
      rc=$?

      status="$(status_of $rc)"
      grep -qi 'bad_alloc\|cannot allocate\|out of memory' "$out" 2>/dev/null && status=oom

      fmt="$(g_field e2e format "$out")"
      rounds="$(g_field kernel rounds "$out")"
      simp="$(g_field stage 'simplified out' "$out")"
      bykern="$(g_field kernel 'by kernelizer' "$out")"
      restot="$(g_field kernel 'resolved total' "$out")"
      vbefore="$(g_field graph 'vertices before' "$out")"
      ebefore="$(g_field graph 'edges before' "$out")"
      vafter="$(g_field graph 'vertices after' "$out")"
      vred="$(sed -n 's/^\[kernel\] vertex reduction: *\([0-9.e+-]*\) %.*/\1/p' "$out" | head -1)"
      opt="$(g_field e2e 'FINAL OPTIMUM' "$out")"
      tv="$(g_field graph 'total variables' "$out")"

      t_parse="$(g_field stage parse "$out")"
      t_topoly="$(g_field stage toPolynomial "$out")"
      t_addpoly="$(g_field stage addPolynomial "$out")"
      t_getgraph="$(g_field stage 'getGraph copy' "$out")"
      t_simplify="$(g_field stage simplify "$out")"
      t_kern="$(g_field e2e 'kernel time' "$out")"
      t_solve="$(g_field e2e 'solve time' "$out")"
      t_total="$(g_field e2e 'TOTAL TIME' "$out")"

      k_collect="$(g_field time kern.collect "$out")"
      k_flownet="$(g_field time kern.build_flownet "$out")"
      k_maxflow="$(g_field time kern.maxflow "$out")"
      k_classify="$(g_field time kern.classify "$out")"
      k_apply="$(g_field time kern.apply "$out")"

      #  Derived columns, computed here rather than stored by the program
      t_ccg="$(awk -v a="$t_topoly" -v b="$t_addpoly" -v c="$t_getgraph" \
               'BEGIN{ if (a=="" && b=="" && c=="") print ""; else printf "%.9g", a+b+c }')"
      k_other="$(awk -v t="$t_kern" -v a="$k_collect" -v b="$k_flownet" -v c="$k_maxflow" -v d="$k_classify" -v e="$k_apply" \
                 'BEGIN{ if (a=="" || t=="") print ""; else printf "%.9g", t-(a+b+c+d+e) }')"

      #  Percent of variables resolved
      varred="$(awk -v a="$tv" -v b="$restot" 'BEGIN{ if (a=="" || b=="" || a+0<=0) print ""; else printf "%.6f", 100*b/a }')"

      na() { if [ -z "$1" ]; then printf 'NA'; else printf '%s' "$1"; fi; }

      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$rep" "$(na "$fmt")" "$k" "$SOLVER" "$PERTURB" "$(na "${tv:-$nv}")" \
        "$(na "$rounds")" "$(na "$simp")" "$(na "$bykern")" "$(na "$restot")" "$(na "$varred")" \
        "$(na "$vbefore")" "$(na "$ebefore")" "$(na "$vafter")" "$(na "$vred")" \
        "$(na "$opt")" "$status" \
        "$(na "$t_parse")" "$(na "$t_topoly")" "$(na "$t_addpoly")" "$(na "$t_getgraph")" \
        "$(na "$t_ccg")" "$(na "$t_simplify")" "$(na "$t_kern")" "$(na "$t_solve")" \
        "$(na "$k_collect")" "$(na "$k_flownet")" "$(na "$k_maxflow")" "$(na "$k_classify")" \
        "$(na "$k_apply")" "$(na "$k_other")" \
        "$(na "$(peak_kb "$res")")" "$(na "$(wall_s "$res")")" "$(na "$t_total")" >> "$TSV"

      printf '  [%3d/%-3d] rep%s %-4s %-40s %-14s kern=%-10s solve=%-10s total=%s\n' \
        "$n" "$TOTAL" "$rep" "$k" "${name:0:40}" "$status" \
        "$(na "$t_kern")" "$(na "$t_solve")" "$(na "$t_total")" | tee -a "$RUNLOG"

      rm -f "$res"
    done < "$LIST"

    say ""
    IFS=','
  done
  IFS="$IFS_SAVE"
  rep=$((rep+1))
done

# summary

say "=== Totals per kernelizer (seconds) ==="
say "$(printf '  %-6s %5s %10s %10s %10s %10s %10s %10s' kern n parse ccg_build simplify kernelize solve TOTAL)"
awk -F'\t' 'NR>1 && $18=="ok" {
    k=$4; n[k]++;
    p[k]+=$19; cg[k]+=$23; sm[k]+=$24; ke[k]+=$25; so[k]+=$26; to[k]+=$35 }
  END{ for (k in n) printf "  %-6s %5d %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f\n", k, n[k], p[k], cg[k], sm[k], ke[k], so[k], to[k] }' "$TSV" | sort | tee -a "$RUNLOG"

say ""
say "=== INSIDE KERNELIZATION, totals per kernelizer (seconds) ==="
say "$(printf '  %-6s %5s %10s %10s %10s %10s %10s %10s %10s' kern n collect flownet MAXFLOW classify apply other kernelize)"
awk -F'\t' 'NR>1 && $18=="ok" && $27!="NA" {
    k=$4; n[k]++; c[k]+=$27; b[k]+=$28; m[k]+=$29; cl[k]+=$30; a[k]+=$31; o[k]+=$32; t[k]+=$25 }
  END{ for (k in n) printf "  %-6s %5d %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f\n", k, n[k], c[k], b[k], m[k], cl[k], a[k], o[k], t[k] }' "$TSV" | sort | tee -a "$RUNLOG"

say ""
say "=== (kernelize + solve, vs the none control) ==="
say "$(printf '  %-6s %5s %14s %14s' kern n kernel+solve 'vs control')"
awk -F'\t' 'NR>1 && $18=="ok" { k=$4; n[k]++; s[k]+=$25+$26 }
  END{ base = s["none"];
       for (k in n) {
         if (k=="none") { printf "  %-6s %5d %14.2f %14s\n", k, n[k], s[k], "(control)" }
         else if (base > 0) { printf "  %-6s %5d %14.2f %13.2fx\n", k, n[k], s[k], base/s[k] }
         else { printf "  %-6s %5d %14.2f %14s\n", k, n[k], s[k], "-" } } }' "$TSV" | sort | tee -a "$RUNLOG"

say ""
say "=== MEASUREMENT SPREAD across repeats ==="
awk -F'\t' 'NR>1 && $18=="ok" { key=$1"|"$4; r=$35+0; if (!(key in lo) || r<lo[key]) lo[key]=r; if (r>hi[key]) hi[key]=r; c[key]++ }
  END{ worst=0; nn=0;
       for (key in c) { if (c[key]>1 && lo[key]>0) { sp=100*(hi[key]-lo[key])/lo[key]; nn++; if (sp>worst) { worst=sp; wk=key } } }
       if (nn==0) { print "  single run per configuration -- re-run with --repeat 3 for a spread"; }
       else { printf "  configurations with repeats: %d   worst spread: %.1f%%  (%s)\n", nn, worst, wk } }' "$TSV" | tee -a "$RUNLOG"

say ""
say "done. table: $TSV"