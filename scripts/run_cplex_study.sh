#!/usr/bin/env bash
#
# run_cplex_study.sh - IBM CPLEX as a second commercial LP baseline for the kernelizer
#
# Stages, run in this order:
#   licence  one CPLEX kernelize on the smallest instance with >= 2000 variables. Stops the whole study if CPLEX turns out to be the
#            size-limited Community Edition, so no hours are wasted on runs that can only fail
#   gate     correctness, in two parts:
#            (a) kernel agreement: max-flow, Gurobi LP and CPLEX LP kernelize the same CCG and their decisions are compared variable by
#                variable. No variable may be assigned OPPOSITE values by two kernelizers. Cheap, so it runs up to --gate-max-vars
#            (b) exact optimum: every kernelizer followed by the exact ilp solve; the final optimum must be identical. The exact solve on a
#                mixed-arity instance is extremely slow (02_500 ran 8.8 h without finishing), so this runs only up to --ilp-max-vars
#   timing   kernelization cost only (solver none): lp / cplex / cpu / gpu
#   probe    CPLEX LP method and thread count on one instance, so CPLEX is reported at its best setting
#
set -uo pipefail
export MF_DAY="${MF_DAY:-$(date +%Y-%m-%d)}"
. "$(dirname "$0")/lib/common.sh"

DIR="data/wcsp"
STAGES="licence,gate,timing,probe"
REPEAT=1
TIMEOUT=7200
MEMGB=18
GATE_MAXVARS=100000
ILP_MAXVARS=20
TIMING_MAXVARS=0
PROBE_MAXVARS=100000

usage() {
cat <<'USAGE'
Usage: ./scripts/run_cplex_study.sh [options]

  -d, --dir DIR             folder holding the synthetic .wcsp instances       (default: data/wcsp)
      --stages LIST         comma list of licence,gate,timing,probe             (default: all four)
      --repeat N            timing repeats per configuration                    (default: 1)
  -t, --timeout SEC         per run, 0 = none; force-killed 60 s after          (default: 7200)
  -m, --mem-limit GB        address-space cap per run, 0 = none                 (default: 18)
      --gate-max-vars N     kernel-agreement gate up to N variables             (default: 100000)
      --ilp-max-vars N      exact-optimum gate up to N variables                (default: 20)
      --timing-max-vars N   timing only up to N variables, 0 = all              (default: 0)
      --probe-max-vars N    probe the largest instance up to N variables        (default: 100000)
  -h, --help

Re-running with the same MF_DAY resumes: timing rows already recorded are skipped, so a later
"--stages timing --repeat 3 --timing-max-vars 100000" adds repeats 2 and 3 for the small instances only.
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -d|--dir)          DIR="$2"; shift 2 ;;
    --stages)          STAGES="$2"; shift 2 ;;
    --repeat)          REPEAT="$2"; shift 2 ;;
    -t|--timeout)      TIMEOUT="$2"; shift 2 ;;
    -m|--mem-limit)    MEMGB="$2"; shift 2 ;;
    --gate-max-vars)   GATE_MAXVARS="$2"; shift 2 ;;
    --ilp-max-vars)    ILP_MAXVARS="$2"; shift 2 ;;
    --timing-max-vars) TIMING_MAXVARS="$2"; shift 2 ;;
    --probe-max-vars)  PROBE_MAXVARS="$2"; shift 2 ;;
    -h|--help)         usage; exit 0 ;;
    *)                 echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

#  Resolve the folder BEFORE mf_init changes directory to the repo root
[ -d "$DIR" ] || { echo "no such folder: $DIR" >&2; exit 2; }
DIR="$(cd "$DIR" && pwd)"

mf_init cplex_study
record_env "$OUT/env_cplex_study.txt"

fail() { say "STOP: $*"; exit 1; }
has()  { case ",$STAGES," in *",$1,"*) return 0 ;; esac; return 1; }
show_tsv() { if command -v column >/dev/null 2>&1; then column -t -s $'\t' "$1"; else cat "$1"; fi; }
kfield() { sed -n "s/^\[$1\] $2 *: *//p" "$3" | head -1 | sed 's/ s$//' | tr -d '\r'; }

#  Run one program under the memory cap and the timeout. stdin is /dev/null so a child can never swallow the
#  instance list a surrounding loop is reading, and a child still alive 60 s after the timeout signal is force-killed.
#  Usage: guarded <out-file> <res-file> <command...>      exit code = the program's (124/137 = timed out)
guarded() {
  local out="$1" res="$2"; shift 2
  ( [ "$MEMGB" -gt 0 ] && ulimit -v $((MEMGB * 1024 * 1024))
    if [ "$TIMEOUT" -gt 0 ]; then
      /usr/bin/time -v -o "$res" timeout -k 60 "$TIMEOUT" "$@" < /dev/null > "$out" 2>&1
    else
      /usr/bin/time -v -o "$res" "$@" < /dev/null > "$out" 2>&1
    fi )
}

say "instances from : $DIR"
say "stages         : $STAGES"
say "repeats        : $REPEAT    timeout: $TIMEOUT s    mem limit: ${MEMGB} GiB"
say "cplex threads  : ${WCSP_CPLEX_THREADS:-1} (default)    method: ${WCSP_CPLEX_METHOD:-0} (0 = automatic)"
say ""

# preconditions

[ -x ./e2e_solve ] || fail "./e2e_solve is not built -- run ./scripts/build.sh --gates"
[ -n "${CPLEX_HOME:-}" ] || fail "CPLEX_HOME is unset -- CPLEX is not installed or ~/.zshrc was not reloaded"
./e2e_solve --kernelizer cplex --solver none /dev/null < /dev/null > /dev/null 2>&1
[ $? -ne 3 ] || fail "./e2e_solve was built WITHOUT CPLEX -- set CPLEX_HOME and re-run ./scripts/build.sh --gates"
if has gate; then
  [ -x ./compare_kernels ] || fail "./compare_kernels is not built -- run ./scripts/build.sh --gates"
fi

# instance list, smallest first

LIST="$OUT/cplex_study.instances.tsv"     # nvars <TAB> path
PLIST="$OUT/cplex_study.list"             # path only, the form run_pipeline_timing.sh reads
find "$DIR" -maxdepth 1 -name '*.wcsp' -type f | while IFS= read -r f; do
  printf '%s\t%s\n' "$(head -1 "$f" | awk '{print $2}')" "$f"
done | sort -n > "$LIST"
cut -f2 "$LIST" > "$PLIST"

NINST=$(wc -l < "$LIST")
[ "$NINST" -gt 0 ] || fail "no .wcsp files in $DIR"
say "$NINST instances (smallest first):"
while IFS=$'\t' read -r nv f; do say "  $(printf '%9s' "$nv") vars  $(basename "$f")"; done < "$LIST"
say ""

# licence

if has licence; then
  hr; say "=== stage licence ==="
  LF="$(awk -F'\t' '$1 >= 2000 { print $2; exit }' "$LIST")"
  if [ -z "$LF" ]; then
    say "no instance with >= 2000 variables -- licence check skipped"
  else
    out="$RAW/cplex_licence_$(basename "$LF").txt"
    guarded "$out" "$RAW/.res.$$" ./e2e_solve --kernelizer cplex --solver none --max-rounds 1 "$LF"
    rc=$?
    rm -f "$RAW/.res.$$"
    if grep -q 'Error 1016\|Community Edition' "$out"; then
      fail "CPLEX is the size-limited Community Edition (see $out) -- install the academic edition"
    fi
    [ "$rc" -eq 0 ] || fail "CPLEX kernelize failed with rc=$rc (see $out)"
    say "licence ok on $(basename "$LF"): kernel time $(kfield e2e 'kernel time' "$out") s," \
        "by kernelizer $(kfield kernel 'by kernelizer' "$out")"
  fi
  say ""
fi

# gate (a): kernel agreement + LP certificates

if has gate; then
  hr; say "=== stage gate (a): kernel agreement and LP certificates, instances up to $GATE_MAXVARS vars ==="
  KV="$OUT/cplex_gate_kernels.tsv"
  printf 'instance\tnvars\tdecided_mf\tdecided_lp\tdecided_cplex\tlp_obj_gurobi\tlp_obj_cplex\tnonhalf_gurobi\tnonhalf_cplex\tviolation_gurobi\tviolation_cplex\topposite_mf_lp\topposite_mf_cplex\tties_lp_cplex\tverdict\n' > "$KV"

  opposite() {   # opposite <dumpA> <dumpB> <log>  -> prints the OPPOSITE count, or NA
    if [ -s "$1" ] && [ -s "$2" ]; then
      ./compare_kernels cmp "$1" "$2" < /dev/null > "$3" 2>&1
      sed -n 's/^ *decided by both, OPPOSITE: *//p' "$3" | head -1
    else
      echo NA
    fi
  }
  lpc() {        # lpc <label> <log>  -> value of an [lp-check] line, or NA
    local v; v="$(sed -n "s/^\[lp-check\] $1 *: *//p" "$2" 2>/dev/null | head -1)"
    [ -n "$v" ] && echo "$v" || echo NA
  }

  while IFS=$'\t' read -r nv f; do
    [ "$nv" -le "$GATE_MAXVARS" ] || continue
    name="$(basename "$f")"
    declare -A dec=()
    for m in mf lp cplex; do
      dumpf="$RAW/gate_${m}_${name}.txt"
      rm -f "$dumpf"
      guarded "$RAW/gate_${m}_${name}.log" "$RAW/.res.$$" ./compare_kernels "$m" "$f" "$dumpf"
      rc=$?
      rm -f "$RAW/.res.$$"
      if [ "$rc" -eq 0 ] && [ -s "$dumpf" ]; then dec[$m]="$(wc -l < "$dumpf")"; else dec[$m]="$(status_of "$rc")"; rm -f "$dumpf"; fi
      say "  $(printf '%-24s %-6s decided=%s' "$name" "$m" "${dec[$m]}")"
    done

    LG="$RAW/gate_lp_${name}.log"; LC="$RAW/gate_cplex_${name}.log"
    so_g="$(lpc 'solver objective' "$LG")";     so_c="$(lpc 'solver objective' "$LC")"
    ob_g="$(lpc 'recomputed objective' "$LG")"; ob_c="$(lpc 'recomputed objective' "$LC")"
    nh_g="$(lpc 'count other' "$LG")";          nh_c="$(lpc 'count other' "$LC")"
    vi_g="$(lpc 'max violation' "$LG")";        vi_c="$(lpc 'max violation' "$LC")"

    o_ml="$(opposite "$RAW/gate_mf_${name}.txt" "$RAW/gate_lp_${name}.txt"    "$RAW/gate_cmp_mf_lp_${name}.log")"
    o_mc="$(opposite "$RAW/gate_mf_${name}.txt" "$RAW/gate_cplex_${name}.txt" "$RAW/gate_cmp_mf_cplex_${name}.log")"
    o_lc="$(opposite "$RAW/gate_lp_${name}.txt" "$RAW/gate_cplex_${name}.txt" "$RAW/gate_cmp_lp_cplex_${name}.log")"

    #  The verdict, in words:
    #    both LP solutions feasible (violation <= 1e-6) and half-integral (no value other than 0, 0.5, 1),
    #    each solver's reported objective equals the objective recomputed from its values,
    #    and Gurobi's and CPLEX's objectives are equal  =>  both are OPTIMAL LP solutions, so each LP kernel is safe;
    #    max-flow contradicts neither;
    #    Gurobi vs CPLEX opposites are then ties between two different optimal solutions, reported but allowed
    v="$(awk -v sg="$so_g" -v sc="$so_c" -v og="$ob_g" -v oc="$ob_c" -v ng="$nh_g" -v nc="$nh_c" \
             -v vg="$vi_g" -v vc="$vi_c" -v ml="$o_ml" -v mc="$o_mc" -v lc="$o_lc" '
      function close_to(a, b) { d = a - b; if (d < 0) d = -d; m = (a < 0 ? -a : a); if (m < 1) m = 1; return d <= 1e-6 * m }
      BEGIN {
        if (sg == "NA" || sc == "NA" || og == "NA" || oc == "NA" || ng == "NA" || nc == "NA" ||
            vg == "NA" || vc == "NA" || ml == "NA" || mc == "NA" || lc == "NA") { print "INCOMPLETE"; exit }
        ok = (ng + 0 == 0) && (nc + 0 == 0) && (vg + 0 <= 1e-6) && (vc + 0 <= 1e-6) &&
             close_to(sg + 0, og + 0) && close_to(sc + 0, oc + 0) && close_to(og + 0, oc + 0) &&
             (ml + 0 == 0) && (mc + 0 == 0)
        print (ok ? "MATCH" : "MISMATCH")
      }')"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$nv" "${dec[mf]}" "${dec[lp]}" "${dec[cplex]}" \
      "$ob_g" "$ob_c" "$nh_g" "$nh_c" "$vi_g" "$vi_c" "$o_ml" "$o_mc" "$o_lc" "$v" >> "$KV"
    unset dec
  done < "$LIST"

  legend_for "$KV" <<'LEG'
One row per instance. max-flow, Gurobi LP and CPLEX LP each kernelize the SAME constraint composite graph (built once per
kernelizer, from the same file, with the same simplify() pass). The two LP solutions are certified independently of the
solvers, and all three kernels are compared variable by variable.
instance           file name
nvars              WCSP variables
decided_mf         variables fixed to 0 or 1 by our max-flow kernelizer (includes those simplify() fixed first)
decided_lp         the same for the Gurobi LP kernelizer
decided_cplex      the same for the CPLEX LP kernelizer (a status word instead of a number means that kernelizer did not finish: timeout, oom, fail_rcN)
lp_obj_gurobi      LP objective of Gurobi's solution, recomputed from the values it returned (sum of weight x value)
lp_obj_cplex       the same for CPLEX. Equal objectives = both solutions are OPTIMAL for the same LP
nonhalf_gurobi     values in Gurobi's solution that are not 0, 0.5 or 1 (must be 0: optimal vertex solutions of this LP are half-integral)
nonhalf_cplex      the same for CPLEX
violation_gurobi   largest amount by which any x_u + x_v >= 1 constraint is violated (must be <= 1e-6: the solution is feasible)
violation_cplex    the same for CPLEX
opposite_mf_lp     variables decided by BOTH max-flow and Gurobi, with OPPOSITE values (must be 0)
opposite_mf_cplex  the same for max-flow vs CPLEX (must be 0)
ties_lp_cplex      variables Gurobi and CPLEX decided with opposite values. ALLOWED: an LP can have several optimal solutions,
                   and in a tie (e.g. two neighbouring vertices of equal weight) one solver may pick u and the other v. By the
                   Nemhauser-Trotter persistency theorem EACH optimal half-integral solution gives a safe kernel on its own;
                   two such kernels need not agree with each other. Max-flow deciding fewer variables and contradicting
                   neither is consistent with it fixing only what every optimal solution agrees on
verdict            MATCH      both LP solutions certified optimal (feasible, half-integral, equal objectives, solver objective =
                              recomputed objective) and max-flow contradicts neither
                   MISMATCH   one of those checks failed -- a defect, investigate
                   INCOMPLETE a kernelizer did not finish, nothing to compare
LEG
  show_tsv "$KV" | tee -a "$RUNLOG"
  grep -q 'MISMATCH' "$KV" && fail "gate (a) found a MISMATCH -- later stages not run (see $KV)"
  say ""

# gate (b): exact optimum

  hr; say "=== stage gate (b): exact optimum, instances up to $ILP_MAXVARS vars ==="
  ./scripts/run_pipeline_timing.sh -k none,lp,cplex,cpu -s ilp -r 1 -t "$TIMEOUT" -m "$MEMGB" \
      --max-vars "$ILP_MAXVARS" "$PLIST" < /dev/null

  GT="$OUT/pipeline_cplex_study_ilp_r1.tsv"
  V="$OUT/cplex_gate_optimum.tsv"
  awk -F'\t' -v OFS='\t' '
    FNR == NR { b = $2; sub(/.*\//, "", b); ord[++n] = b; nvs[b] = $1; next }
    FNR == 1  { next }
    $2 == 1   { o[$1, $4] = $17; s[$1, $4] = $18; seen[$1] = 1 }
    END {
      print "instance", "nvars", "opt_none", "opt_lp", "opt_cplex", "opt_cpu", "verdict"
      split("none lp cplex cpu", K, " ")
      for (i = 1; i <= n; i++) {
        b = ord[i]; if (!(b in seen)) continue
        ref = ""; nok = 0; bad = 0; line = b OFS nvs[b]
        for (j = 1; j <= 4; j++) {
          kk = K[j]; st = s[b, kk]
          v = (st == "ok") ? o[b, kk] : (st == "" ? "not_run" : st)
          if (st == "ok") { nok++; if (ref == "") ref = v; else if (v != ref) bad = 1 }
          line = line OFS v
        }
        print line, (bad ? "MISMATCH" : (nok >= 2 ? "MATCH" : "INCOMPLETE"))
      }
    }' "$LIST" "$GT" > "$V"

  legend_for "$V" <<'LEG'
One row per instance: every kernelizer followed by the SAME exact ilp solve (Gurobi), so only the kernelizer changes.
Run only on instances small enough for the exact solve to finish (--ilp-max-vars).
instance    file name
nvars       WCSP variables
opt_none    final optimum with no kernelization -- the reference answer
opt_lp      final optimum after the Gurobi LP kernelizer
opt_cplex   final optimum after the CPLEX LP kernelizer
opt_cpu     final optimum after our CPU max-flow kernelizer
verdict     MATCH | MISMATCH (a defect) | INCOMPLETE (fewer than two runs finished)
LEG
  show_tsv "$V" | tee -a "$RUNLOG"
  grep -q 'MISMATCH' "$V" && fail "gate (b) found a MISMATCH -- later stages not run (see $V)"
  say ""
fi

# timing

if has timing; then
  hr; say "=== stage timing: kernelization only, lp / cplex / cpu / gpu, $REPEAT repeat(s) ==="
  mv_arg=()
  [ "$TIMING_MAXVARS" -gt 0 ] && mv_arg=(--max-vars "$TIMING_MAXVARS") && say "timing only up to $TIMING_MAXVARS vars"
  ./scripts/run_pipeline_timing.sh -k lp,cplex,cpu,gpu -s none -r 1 --repeat "$REPEAT" -t "$TIMEOUT" -m "$MEMGB" \
      "${mv_arg[@]}" "$PLIST" < /dev/null

  TT="$OUT/pipeline_cplex_study_none_r1.tsv"
  S="$OUT/cplex_timing_summary.tsv"
  awk -F'\t' -v OFS='\t' '
    FNR == NR { b = $2; sub(/.*\//, "", b); ord[++n] = b; nvs[b] = $1; next }
    FNR == 1  { next }
    {
      key = $1 SUBSEP $4; runs[key]++
      if ($18 == "ok") {
        ok[key]++; t = $25 + 0
        if (!(key in mn) || t < mn[key]) mn[key] = t
        if (t > mx[key]) mx[key] = t
        p = ($33 + 0) / 1048576; if (p > pk[key]) pk[key] = p
      } else if (!(key in bad)) bad[key] = $18
    }
    END {
      print "instance", "nvars", "kernelizer", "runs", "ok_runs", "t_kern_best_s", "t_kern_worst_s", "peak_gib", "status", "speedup_vs_cplex"
      split("lp cplex cpu gpu", K, " ")
      for (i = 1; i <= n; i++) {
        b = ord[i]
        for (j = 1; j <= 4; j++) {
          kk = K[j]; key = b SUBSEP kk
          if (!(key in runs)) continue
          ck = b SUBSEP "cplex"
          ratio = ((ck in mn) && (key in mn) && mn[key] > 0) ? sprintf("%.2f", mn[ck] / mn[key]) : "NA"
          print b, nvs[b], kk, runs[key], ok[key] + 0,
                ((key in mn) ? sprintf("%.3f", mn[key]) : "NA"),
                ((key in mn) ? sprintf("%.3f", mx[key]) : "NA"),
                ((key in pk) ? sprintf("%.2f", pk[key]) : "NA"),
                ((key in bad) ? bad[key] : "ok"), ratio
        }
      }
    }' "$LIST" "$TT" > "$S"

  legend_for "$S" <<'LEG'
One row per instance and kernelizer, summarised over the repeats of the timing run (solver none: kernelization cost only).
instance          file name
nvars             WCSP variables
kernelizer        lp = Gurobi LP, cplex = CPLEX LP, cpu = our max-flow on CPU, gpu = our max-flow on GPU
runs              repeats attempted
ok_runs           repeats that finished
t_kern_best_s     fastest t_kernelize over the finished repeats, seconds (includes building the LP / flow network)
t_kern_worst_s    slowest t_kernelize over the finished repeats -- the gap to best is the measurement noise
peak_gib          highest peak resident memory over the finished repeats, GiB
status            ok if every repeat finished, otherwise the first failure word:
                  timeout = stopped at the time limit, timeout_killed = had to be force-killed after it, oom = out of memory
speedup_vs_cplex  t_kern_best of CPLEX divided by t_kern_best of this row. Above 1 = this kernelizer is faster than CPLEX,
                  below 1 = CPLEX is faster. NA when either side did not finish
LEG
  show_tsv "$S" | tee -a "$RUNLOG"
  say ""
fi

# probe

if has probe; then
  hr; say "=== stage probe: CPLEX method and threads ==="
  PF="$(awk -F'\t' -v m="$PROBE_MAXVARS" '$1 <= m { f = $2 } END { print f }' "$LIST")"
  if [ -z "$PF" ]; then
    say "no instance with <= $PROBE_MAXVARS variables -- probe skipped"
  else
    say "probe instance: $(basename "$PF")"
    P="$OUT/cplex_probe.tsv"
    printf 'instance\tthreads\tmethod\tstatus\tt_kernelize_s\tby_kernelizer\tpeak_gib\n' > "$P"

    probe() {
      local th="$1" me="$2"
      local out="$RAW/cplex_probe_t${th}_m${me}_$(basename "$PF").txt"
      local res="$RAW/.res.$$"
      WCSP_CPLEX_THREADS="$th" WCSP_CPLEX_METHOD="$me" \
        guarded "$out" "$res" ./e2e_solve --kernelizer cplex --solver none --max-rounds 1 "$PF"
      local rc=$? st tk bk pg
      st="$(status_of "$rc")"
      grep -qi 'bad_alloc\|cannot allocate\|out of memory' "$out" 2>/dev/null && st=oom
      tk="$(kfield e2e 'kernel time' "$out")";      [ -n "$tk" ] || tk=NA
      bk="$(kfield kernel 'by kernelizer' "$out")"; [ -n "$bk" ] || bk=NA
      pg="$(awk -v k="$(peak_kb "$res")" 'BEGIN { if (k == "") print "NA"; else printf "%.2f", k / 1048576 }')"
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(basename "$PF")" "$th" "$me" "$st" "$tk" "$bk" "$pg" >> "$P"
      say "$(printf '  threads=%-2s method=%s  %-14s kern=%-12s by_kernelizer=%-10s peak=%s GiB' "$th" "$me" "$st" "$tk" "$bk" "$pg")"
      rm -f "$res"
    }

    for me in 0 1 2 4 6; do probe 1 "$me"; done
    for th in 4 8;       do probe "$th" 0; done

    legend_for "$P" <<'LEG'
One row per CPLEX setting, all on the same instance, kernelization only.
instance        the probe instance (largest one up to --probe-max-vars variables)
threads         WCSP_CPLEX_THREADS: CPU threads CPLEX may use
method          WCSP_CPLEX_METHOD: 0 automatic, 1 primal simplex, 2 dual simplex, 4 barrier (with crossover), 6 concurrent
status          ok | timeout | timeout_killed | oom | fail_rcN
t_kernelize_s   whole CPLEX kernelize time, seconds
by_kernelizer   variables decided -- any optimal LP solution is valid, so this may move a little between methods
peak_gib        peak resident memory, GiB
If a setting other than threads=1 method=0 is clearly fastest, re-run the timing stage with it into a separate folder:
MF_DAY=<label>-method2 WCSP_CPLEX_METHOD=2 ./scripts/run_cplex_study.sh --stages timing
LEG
    show_tsv "$P" | tee -a "$RUNLOG"
  fi
  say ""
fi

hr
say "done. everything is in $OUT/"
say "  cplex_study.log              this study's log"
say "  cplex_gate_kernels.tsv       gate (a): kernel agreement per instance"
say "  cplex_gate_optimum.tsv       gate (b): exact optimum per instance"
say "  cplex_timing_summary.tsv     best/worst kernelize time and memory per instance and kernelizer"
say "  cplex_probe.tsv              CPLEX method / thread probe"
say "  pipeline_cplex_study_*.tsv   full per-run tables from run_pipeline_timing.sh"
say "  raw/                         every program's complete output"