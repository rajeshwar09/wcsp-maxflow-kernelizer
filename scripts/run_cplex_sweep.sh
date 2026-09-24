#!/usr/bin/env bash
#
# run_cplex_sweep.sh -- give CPLEX every reasonable chance: time its LP kernelizer under many settings
#
# Each configuration below is one combination of CPLEX settings (threads, LP algorithm, presolve, barrier crossover,
# parallel mode, memory emphasis). For every instance the script runs "default" first, then every other chosen
# configuration, and records per run:
#   the kernelize time, CPLEX's own model-build and optimize times, the algorithm CPLEX actually used, iterations,
#   peak memory, and a SAFETY check: the returned LP solution must be optimal (same objective as "default") and
#   half-integral (every value 0, 0.5 or 1). A fast setting that breaks either is not a usable kernelizer.
#
# Cost control: after "default" has run on an instance, every other configuration on that instance is stopped at
#   min(--timeout, max(--cap-min, --cap-factor x default's time)).
# A setting that is several times slower than CPLEX's default cannot change the conclusion, so it is recorded as
# "capped" instead of being allowed to run for hours.
#
# Results go to results/<MF_DAY>/ next to the CPLEX study, so the final table can put the best CPLEX setting beside
# Gurobi and our max-flow kernelizers (from cplex_timing_summary.tsv). Re-running resumes: rows already recorded are skipped.
set -uo pipefail
export MF_DAY="${MF_DAY:-$(date +%Y-%m-%d)}"
. "$(dirname "$0")/lib/common.sh"

DIR="data/wcsp"
INSTANCES="all"
MAXVARS=300000
CONFIG_SEL="all"
RANK_ON="03_100k.wcsp"
REPEAT=1
TIMEOUT=7200
CAP_FACTOR=3
CAP_MIN=300
MEMGB=18
LIST_ONLY=0

#  name                 threads method presolve crossover parallel mem_emph  what it tests
CONFIGS='
default                 1  0  -  -  -  -  CPLEX out of the box on one thread: the reference every other row is compared to
primal                  1  1  -  -  -  -  primal simplex
dual                    1  2  -  -  -  -  dual simplex
network                 1  3  -  -  -  -  network simplex (extracts any network structure it finds)
barrier                 1  4  -  -  -  -  barrier (interior point) followed by crossover to a vertex solution
sifting                 1  5  -  -  -  -  sifting (meant for LPs with far more columns than rows)
auto_t2                 2  0  -  -  -  -  automatic algorithm, 2 threads
auto_t4                 4  0  -  -  -  -  automatic algorithm, 4 threads
auto_t8                 8  0  -  -  -  -  automatic algorithm, 8 threads
auto_t16               16  0  -  -  -  -  automatic algorithm, 16 threads (every hardware thread)
barrier_t4              4  4  -  -  -  -  barrier with crossover, 4 threads (barrier parallelises well)
barrier_t8              8  4  -  -  -  -  barrier with crossover, 8 threads
barrier_t16            16  4  -  -  -  -  barrier with crossover, 16 threads
concurrent_t2           2  6  -  -  -  -  concurrent: several algorithms race, first to finish wins, 2 threads
concurrent_t4           4  6  -  -  -  -  concurrent, 4 threads
concurrent_t8           8  6  -  -  -  -  concurrent, 8 threads
concurrent_t16         16  6  -  -  -  -  concurrent, 16 threads
concurrent_t8_opp       8  6  -  -  -1 -  concurrent, 8 threads, opportunistic (non-deterministic) parallel mode
auto_nopresolve         1  0  0  -  -  -  automatic, presolve switched off
dual_nopresolve         1  2  0  -  -  -  dual simplex, presolve switched off
barrier_t8_nocross      8  4  -  -1 -  -  barrier WITHOUT crossover: fast but may return a non-vertex point (safety check shows it)
auto_mememph            1  0  -  -  -  1  automatic with memory emphasis (trades speed for a smaller footprint)
'

usage() {
cat <<'USAGE'
Usage: ./scripts/run_cplex_sweep.sh [options]

  -d, --dir DIR          folder with the .wcsp instances                          (default: data/wcsp)
      --instances LIST   comma list of file names, or "all"                       (default: all)
      --max-vars N       with "all": only instances up to N variables              (default: 300000)
      --configs LIST     comma list of configuration names, "all", or "top:K"      (default: all)
                         top:K = "default" plus the K fastest SAFE configurations already measured on --rank-on
      --rank-on NAME     instance used to rank for top:K                           (default: 03_100k.wcsp)
      --repeat N         runs per instance and configuration                       (default: 1)
  -t, --timeout SEC      hard limit per run; force-killed 60 s after               (default: 7200)
      --cap-factor F     non-default runs stop at F x default's time on that instance (default: 3)
      --cap-min SEC      but never below this                                      (default: 300)
  -m, --mem-limit GB     address-space cap per run, 0 = none                       (default: 18)
      --list             print the configurations and exit
  -h, --help
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -d|--dir)        DIR="$2"; shift 2 ;;
    --instances)     INSTANCES="$2"; shift 2 ;;
    --max-vars)      MAXVARS="$2"; shift 2 ;;
    --configs)       CONFIG_SEL="$2"; shift 2 ;;
    --rank-on)       RANK_ON="$2"; shift 2 ;;
    --repeat)        REPEAT="$2"; shift 2 ;;
    -t|--timeout)    TIMEOUT="$2"; shift 2 ;;
    --cap-factor)    CAP_FACTOR="$2"; shift 2 ;;
    --cap-min)       CAP_MIN="$2"; shift 2 ;;
    -m|--mem-limit)  MEMGB="$2"; shift 2 ;;
    --list)          LIST_ONLY=1; shift ;;
    -h|--help)       usage; exit 0 ;;
    *)               echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

if [ "$LIST_ONLY" = 1 ]; then
  printf '%-20s %7s %6s %8s %9s %8s %8s  %s\n' name threads method presolve crossover parallel mem_emph "what it tests"
  echo "$CONFIGS" | awk 'NF { printf "%-20s %7s %6s %8s %9s %8s %8s  ", $1,$2,$3,$4,$5,$6,$7; for (i=8;i<=NF;i++) printf "%s ", $i; print "" }'
  exit 0
fi

[ -d "$DIR" ] || { echo "no such folder: $DIR" >&2; exit 2; }
DIR="$(cd "$DIR" && pwd)"

mf_init cplex_sweep
record_env "$OUT/env_cplex_sweep.txt"

fail() { say "STOP: $*"; exit 1; }
show_tsv() { if command -v column >/dev/null 2>&1; then column -t -s $'\t' "$1"; else cat "$1"; fi; }
field() { sed -n "s/^\[$1\] $2 *: *//p" "$3" 2>/dev/null | head -1 | sed 's/ s$//; s/ (optimal)$//' | tr -d '\r'; }

guarded() {   # guarded <timeout> <out> <res> <command...>
  local tmo="$1" out="$2" res="$3"; shift 3
  ( [ "$MEMGB" -gt 0 ] && ulimit -v $((MEMGB * 1024 * 1024))
    if [ "$tmo" -gt 0 ]; then
      /usr/bin/time -v -o "$res" timeout -k 60 "$tmo" "$@" < /dev/null > "$out" 2>&1
    else
      /usr/bin/time -v -o "$res" "$@" < /dev/null > "$out" 2>&1
    fi )
}

[ -x ./e2e_solve ] || fail "./e2e_solve is not built -- run ./scripts/build.sh --gates"
[ -n "${CPLEX_HOME:-}" ] || fail "CPLEX_HOME is unset"
./e2e_solve --kernelizer cplex --solver none /dev/null < /dev/null > /dev/null 2>&1
[ $? -ne 3 ] || fail "./e2e_solve was built WITHOUT CPLEX -- run ./scripts/build.sh --gates with CPLEX_HOME set"

# instances, smallest first

ILIST="$OUT/cplex_sweep.instances.tsv"
find "$DIR" -maxdepth 1 -name '*.wcsp' -type f | while IFS= read -r f; do
  printf '%s\t%s\n' "$(head -1 "$f" | awk '{print $2}')" "$f"
done | sort -n | awk -F'\t' -v sel="$INSTANCES" -v mx="$MAXVARS" '
  BEGIN { n = split(sel, s, ","); for (i = 1; i <= n; i++) want[s[i]] = 1 }
  { b = $2; sub(/.*\//, "", b)
    if (sel == "all" ? ($1 <= mx) : (b in want)) print }' > "$ILIST"
[ -s "$ILIST" ] || fail "no instance selected (check --instances / --max-vars)"

# tables

T="$OUT/cplex_sweep.tsv"
HDR='instance\tnvars\tconfig\trep\tthreads\tmethod\tpresolve\tcrossover\tparallel\tmem_emph\tcap_s\tstatus\tt_kernelize\tt_build\tt_optimize\tmethod_used\tsimplex_iters\tbarrier_iters\tobjective\tnonhalf\tby_kernelizer\tpeak_gib\tsafe\n'
[ -s "$T" ] || printf "$HDR" > "$T"

legend_for "$T" <<'LEG'
One row per run: one instance, one CPLEX configuration, one repeat. Kernelization only (no exact solve afterwards).
instance        file name
nvars           WCSP variables
config          configuration name (./scripts/run_cplex_sweep.sh --list shows them all)
rep             repeat number
threads         CPU threads CPLEX may use
method          LP algorithm asked for: 0 automatic, 1 primal, 2 dual, 3 network, 4 barrier, 5 sifting, 6 concurrent
presolve        - = CPLEX default (on), 0 = off
crossover       - = CPLEX default, -1 = no crossover after barrier
parallel        - = CPLEX default (deterministic), -1 = opportunistic
mem_emph        - = CPLEX default (off), 1 = memory emphasis on
cap_s           the time limit this run had, seconds (default: --timeout; others: the cap derived from default's time)
status          ok | capped (stopped at cap_s: much slower than default) | oom | timeout | fail_rcN
t_kernelize     whole kernelize time as the pipeline measures it, seconds (build the LP + solve + read back)
t_build         CPLEX: loading the model (columns and rows), seconds
t_optimize      CPLEX: the LP optimisation itself, seconds
method_used     algorithm CPLEX actually finished with (automatic and concurrent choose for themselves)
simplex_iters   simplex iterations
barrier_iters   barrier iterations
objective       LP objective value
nonhalf         returned values that are not 0, 0.5 or 1
by_kernelizer   variables decided by the kernel
peak_gib        peak resident memory, GiB
safe            yes      = CPLEX status optimal, objective equal to default's, every value 0 / 0.5 / 1 -> a valid kernel
                NO_nonhalf / NO_objective / NO_status = the setting returned something that is not a valid kernel
                NA       = the run did not finish
LEG

has_row() { awk -F'\t' -v i="$1" -v c="$2" -v r="$3" '$1==i && $3==c && $4==r { f=1 } END { exit !f }' "$T"; }
default_time() { awk -F'\t' -v i="$1" '$1==i && $3=="default" && $12=="ok" { t=$13; if (b=="" || t+0 < b+0) b=t } END { print b }' "$T"; }
default_obj()  { awk -F'\t' -v i="$1" '$1==i && $3=="default" && $12=="ok" { print $19; exit }' "$T"; }

# choose configurations

CFG="$OUT/cplex_sweep.configs.txt"
case "$CONFIG_SEL" in
  all)   echo "$CONFIGS" | awk 'NF' > "$CFG" ;;
  top:*) K="${CONFIG_SEL#top:}"
         top="$(awk -F'\t' -v r="$RANK_ON" '$1==r && $12=="ok" && $23=="yes" && $3!="default" { if (!($3 in b) || $13+0 < b[$3]) b[$3]=$13+0 }
                END { for (c in b) print b[c] "\t" c }' "$T" | sort -n | head -"$K" | cut -f2)"
         [ -n "$top" ] || fail "no safe measured configurations on $RANK_ON yet -- run the sweep there first"
         echo "$CONFIGS" | awk -v keep="default $top" 'BEGIN { n = split(keep, k, /[ \n]+/); for (i=1;i<=n;i++) w[k[i]]=1 } NF && ($1 in w)' > "$CFG" ;;
  *)     echo "$CONFIGS" | awk -v keep="default,$CONFIG_SEL" 'BEGIN { n = split(keep, k, ","); for (i=1;i<=n;i++) w[k[i]]=1 } NF && ($1 in w)' > "$CFG" ;;
esac
NCFG=$(wc -l < "$CFG")
[ "$NCFG" -gt 0 ] || fail "no configuration selected"

say "instances  :"; while IFS=$'\t' read -r nv f; do say "  $(printf '%9s' "$nv") vars  $(basename "$f")"; done < "$ILIST"
say "configs    : $NCFG  ($(cut -d' ' -f1 "$CFG" | awk '{print $1}' | paste -sd' '))"
say "repeats    : $REPEAT    hard timeout: $TIMEOUT s    cap: ${CAP_FACTOR}x default, at least $CAP_MIN s    mem limit: ${MEMGB} GiB"
say "table      : $T"
say ""

# run

while IFS=$'\t' read -r nv f; do
  name="$(basename "$f")"
  hr; say "=== $name  ($nv vars) ==="
  for rep in $(seq 1 "$REPEAT"); do
    while read -r cname th me pre cro par mem _desc; do
      [ -n "$cname" ] || continue
      if has_row "$name" "$cname" "$rep"; then say "  $(printf '%-20s rep%s' "$cname" "$rep")  already recorded"; continue; fi

      cap="$TIMEOUT"
      if [ "$cname" != "default" ]; then
        bt="$(default_time "$name")"
        if [ -n "$bt" ]; then
          cap="$(awk -v b="$bt" -v f="$CAP_FACTOR" -v m="$CAP_MIN" -v t="$TIMEOUT" 'BEGIN { c = b * f; if (c < m) c = m; if (t > 0 && c > t) c = t; printf "%d", c + 0.999 }')"
        fi
      fi

      envs=(WCSP_CPLEX_THREADS="$th" WCSP_CPLEX_METHOD="$me")
      [ "$pre" != "-" ] && envs+=(WCSP_CPLEX_PRESOLVE="$pre")
      [ "$cro" != "-" ] && envs+=(WCSP_CPLEX_CROSSOVER="$cro")
      [ "$par" != "-" ] && envs+=(WCSP_CPLEX_PARALLEL="$par")
      [ "$mem" != "-" ] && envs+=(WCSP_CPLEX_MEMEMPHASIS="$mem")

      out="$RAW/sweep_${cname}_${rep}_${name}.txt"
      res="$RAW/.res.$$"
      guarded "$cap" "$out" "$res" env "${envs[@]}" ./e2e_solve --kernelizer cplex --solver none --max-rounds 1 "$f"
      rc=$?

      st="$(status_of "$rc")"
      grep -qi 'bad_alloc\|cannot allocate\|out of memory' "$out" 2>/dev/null && st=oom
      if [ "$cname" != "default" ] && { [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; } && [ "$cap" -lt "$TIMEOUT" ]; then st=capped; fi

      tk="$(field e2e 'kernel time' "$out")";       [ -n "$tk" ] || tk=NA
      tb="$(field cplex 'model build' "$out")";     [ -n "$tb" ] || tb=NA
      to="$(field cplex 'optimize' "$out")"; [ -n "$to" ] || to=NA
      mu="$(field cplex 'method used' "$out")";     [ -n "$mu" ] || mu=NA
      si="$(field cplex 'simplex iters' "$out")";   [ -n "$si" ] || si=NA
      bi="$(field cplex 'barrier iters' "$out")";   [ -n "$bi" ] || bi=NA
      cs="$(field cplex 'status' "$out")";          [ -n "$cs" ] || cs=NA
      ob="$(field cplex 'objective' "$out")";       [ -n "$ob" ] || ob=NA
      nh="$(field cplex 'non-half values' "$out")"; [ -n "$nh" ] || nh=NA
      bk="$(field kernel 'by kernelizer' "$out")";  [ -n "$bk" ] || bk=NA
      pg="$(awk -v k="$(peak_kb "$res")" 'BEGIN { if (k == "") print "NA"; else printf "%.2f", k / 1048576 }')"
      rm -f "$res"

      safe=NA
      if [ "$st" = ok ]; then
        ref="$(default_obj "$name")"; [ "$cname" = default ] && ref="$ob"
        safe="$(awk -v cs="$cs" -v nh="$nh" -v ob="$ob" -v ref="$ref" '
          BEGIN {
            if (cs != "1") { print "NO_status"; exit }
            if (nh + 0 != 0) { print "NO_nonhalf"; exit }
            if (ref == "" || ref == "NA") { print "yes"; exit }
            d = ob - ref; if (d < 0) d = -d; m = (ref < 0 ? -ref : ref); if (m < 1) m = 1
            print (d <= 1e-6 * m ? "yes" : "NO_objective") }')"
      fi

      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$nv" "$cname" "$rep" "$th" "$me" "$pre" "$cro" "$par" "$mem" "$cap" "$st" "$tk" "$tb" "$to" \
        "$mu" "$si" "$bi" "$ob" "$nh" "$bk" "$pg" "$safe" >> "$T"
      say "  $(printf '%-20s rep%s  %-8s kern=%-12s opt=%-12s used=%-3s peak=%-6s safe=%s  (cap %ss)' \
            "$cname" "$rep" "$st" "$tk" "$to" "$mu" "$pg" "$safe" "$cap")"
    done < "$CFG"
  done
done < "$ILIST"

# summaries

S="$OUT/cplex_sweep_summary.tsv"
awk -F'\t' -v OFS='\t' '
  FNR == 1 { next }
  { key = $1 SUBSEP $3; nv[$1] = $2; seen[key] = 1; ord[$1]; runs[key]++
    if ($12 == "ok") { ok[key]++; t = $13 + 0; if (!(key in best) || t < best[key]) best[key] = t
                       p = $22 + 0; if (p > pk[key]) pk[key] = p }
    else if (!(key in bad)) bad[key] = $12
    if ($23 != "yes" && $23 != "NA" && !(key in unsafe)) unsafe[key] = $23 }
  END {
    print "instance", "nvars", "config", "runs", "ok_runs", "t_kern_best_s", "peak_gib", "status", "safe", "speedup_vs_default"
    for (k in seen) { split(k, a, SUBSEP); i = a[1]; c = a[2]; d = i SUBSEP "default"
      r = ((k in best) && (d in best) && best[k] > 0) ? sprintf("%.2f", best[d] / best[k]) : "NA"
      #  three hidden sort keys first (size, name, time with unfinished runs last), removed by cut below
      print nv[i], i, ((k in best) ? best[k] : 1e18),
            i, nv[i], c, runs[k], ok[k] + 0, ((k in best) ? sprintf("%.3f", best[k]) : "NA"),
            ((k in pk) ? sprintf("%.2f", pk[k]) : "NA"), ((k in bad) ? bad[k] : "ok"),
            ((k in unsafe) ? unsafe[k] : ((k in best) ? "yes" : "NA")), r } }' "$T" \
  | { IFS= read -r h; echo "$h"; sort -t$'\t' -k1,1n -k2,2 -k3,3g | cut -f4-; } > "$S"

legend_for "$S" <<'LEG'
One row per instance and configuration, over all its repeats. Sorted by instance size, then fastest first.
t_kern_best_s       fastest kernelize time over the finished repeats, seconds
peak_gib            highest peak memory, GiB
status              ok if every repeat finished, otherwise the first failure word (capped, oom, timeout, fail_rcN)
safe                yes if every finished repeat returned a valid kernel, otherwise the first reason it did not
speedup_vs_default  default's best time divided by this configuration's best time. Above 1 = faster than CPLEX's default
LEG

B="$OUT/cplex_sweep_vs_maxflow.tsv"
TS="$OUT/cplex_timing_summary.tsv"
awk -F'\t' -v OFS='\t' -v ts="$TS" '
  BEGIN { while ((getline line < ts) > 0) { split(line, a, "\t")
            if (a[3] == "lp" || a[3] == "cpu" || a[3] == "gpu") other[a[1], a[3]] = a[6] } }
  FNR == 1 { next }
  { nv[$1] = $2
    if ($3 == "default" && $6 != "NA") dt[$1] = $6
    if ($9 == "yes" && $6 != "NA" && (!($1 in bt) || $6 + 0 < bt[$1] + 0)) { bt[$1] = $6; bc[$1] = $3 } }
  END {
    print "instance", "nvars", "cplex_default_s", "cplex_best_s", "cplex_best_config", "gurobi_s", "maxflow_cpu_s", "maxflow_gpu_s",
          "gurobi_vs_best_cplex", "cpu_vs_best_cplex", "gpu_vs_best_cplex"
    for (i in nv) {
      g = ((i, "lp") in other) ? other[i, "lp"] : "NA"; c = ((i, "cpu") in other) ? other[i, "cpu"] : "NA"
      u = ((i, "gpu") in other) ? other[i, "gpu"] : "NA"; b = (i in bt) ? bt[i] : "NA"
      print i, nv[i], ((i in dt) ? dt[i] : "NA"), b, ((i in bc) ? bc[i] : "NA"), g, c, u,
            ((b != "NA" && g != "NA" && g + 0 > 0) ? sprintf("%.2f", b / g) : "NA"),
            ((b != "NA" && c != "NA" && c + 0 > 0) ? sprintf("%.2f", b / c) : "NA"),
            ((b != "NA" && u != "NA" && u + 0 > 0) ? sprintf("%.2f", b / u) : "NA") } }' "$S" \
  | { IFS= read -r h; echo "$h"; sort -t$'\t' -k2,2n -k1,1; } > "$B"

legend_for "$B" <<'LEG'
One row per instance: CPLEX at its default and at its BEST SAFE configuration, next to Gurobi and our max-flow kernelizers.
cplex_default_s       CPLEX, default configuration (1 thread, automatic algorithm), best time, seconds
cplex_best_s          CPLEX, fastest configuration that still returned a valid kernel, seconds
cplex_best_config     which configuration that was
gurobi_s              Gurobi LP kernelizer, best time from cplex_timing_summary.tsv
maxflow_cpu_s         our max-flow kernelizer on the CPU, best time from cplex_timing_summary.tsv
maxflow_gpu_s         our max-flow kernelizer on the GPU, best time from cplex_timing_summary.tsv
gurobi_vs_best_cplex  cplex_best_s / gurobi_s       -- above 1: Gurobi is faster than CPLEX even at CPLEX's best
cpu_vs_best_cplex     cplex_best_s / maxflow_cpu_s  -- above 1: our CPU kernelizer is faster than CPLEX at its best
gpu_vs_best_cplex     cplex_best_s / maxflow_gpu_s  -- above 1: our GPU kernelizer is faster than CPLEX at its best
NA = that kernelizer has no finished run on this instance
LEG

hr
say "=== summary: every configuration ==="
show_tsv "$S" | tee -a "$RUNLOG"
say ""
say "=== CPLEX at its best vs Gurobi and max-flow ==="
show_tsv "$B" | tee -a "$RUNLOG"
say ""
say "done. tables: $T, $S, $B"