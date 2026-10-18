#!/usr/bin/env bash
# runs e2e_solve on a set of instances and keeps time and memory of every run
# instances are found in instances/real/ and instances/synthetic/ (files or links), the lists are made fresh at every start
# so adding an instance = putting the file or a link to it in one of the two folders, nothing else
#
# usage: ./scripts/run_bench.sh [options] [-- extra e2e_solve flags]
#   -s, --set real|synthetic|all|FILE   instances to run, FILE = text file with one path per line   (default all)
#   -k, --kern LIST                     kernelizers, comma list of gpu,cpu,lp,none                 (default gpu)
#   -r, --repeat N                      runs of every instance and kernelizer                      (default 1)
#       --solver none|ilp|mp            solver after kernelization                                 (default none)
#       --rounds N                      kernelization rounds                                       (default 1)
#       --perturb off|int|real          tie-breaking perturbation                                  (default off)
#       --seed S                        perturbation seed                                          (default 1)
#   -t, --timeout SEC                   limit for one run, 0 = no limit                            (default 3600)
#   -m, --mem-limit GB                  memory limit for cpu, lp and none runs, 0 = no limit       (default 18)
#       --only PATTERN                  only instances whose path matches this grep pattern
#   -n, --name LABEL                    word added to the file names, to recognise the run later
#   --                                  everything after it goes to e2e_solve, for example -- --hub-deg 0
#
# output in results/<date>/, a new set of files for every run:
#   bench_<time>[_label].log   readable log, one line per program run and a summary
#   bench_<time>[_label].tsv   every metric of every program run, one row each
#   bench_<time>[_label]/      the full output of every program run and the instance list used

set -u
cd "$(dirname "$0")/.." || exit 1

# [ARGS] defaults
set_name="all"
kerns="gpu"
repeat=1
solver="none"
rounds=1
perturb="off"
seed=1
timeout_s=3600
mem_gb=18
only=""
label=""
extra=()

# [ARGS] read the command line
while [ $# -gt 0 ]; do
  case "$1" in
    -s|--set) set_name="$2"; shift 2 ;;
    -k|--kern) kerns="$2"; shift 2 ;;
    -r|--repeat) repeat="$2"; shift 2 ;;
    --solver) solver="$2"; shift 2 ;;
    --rounds) rounds="$2"; shift 2 ;;
    --perturb) perturb="$2"; shift 2 ;;
    --seed) seed="$2"; shift 2 ;;
    -t|--timeout) timeout_s="$2"; shift 2 ;;
    -m|--mem-limit) mem_gb="$2"; shift 2 ;;
    --only) only="$2"; shift 2 ;;
    -n|--name) label="_$2"; shift 2 ;;
    --) shift; extra=("$@"); break ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 1 ;;
  esac
done

# [FILES] names of the output files of this run
day=$(date +%F)
stamp="bench_$(date +%H%M%S)${label}"
out_dir="results/$day"
raw_dir="$out_dir/$stamp"
log="$out_dir/$stamp.log"
tsv="$out_dir/$stamp.tsv"
index="$raw_dir/index.tsv"
mkdir -p "$raw_dir"

# [INSTANCES] make the lists from the two folders, then pick the set
mkdir -p instances/real instances/synthetic
find -L instances/real -type f \( -name '*.wcsp' -o -name '*.uai' \) | sort > instances/real.list
find -L instances/synthetic -type f \( -name '*.wcsp' -o -name '*.uai' \) | sort > instances/synthetic.list
case "$set_name" in
  real) cp instances/real.list "$raw_dir/instances.list" ;;
  synthetic) cp instances/synthetic.list "$raw_dir/instances.list" ;;
  all) cat instances/real.list instances/synthetic.list > "$raw_dir/instances.list" ;;
  *)
    if [ ! -f "$set_name" ]; then
      echo "set not found: $set_name"
      rm -rf "$raw_dir"
      exit 1
    fi
    grep -v '^[[:space:]]*$' "$set_name" | cut -f1 > "$raw_dir/instances.list"
    ;;
esac
if [ -n "$only" ]; then
  grep -E "$only" "$raw_dir/instances.list" > "$raw_dir/instances.tmp"
  mv "$raw_dir/instances.tmp" "$raw_dir/instances.list"
fi
mapfile -t files < "$raw_dir/instances.list"
if [ "${#files[@]}" -eq 0 ]; then
  echo "no instances to run, put files or links in instances/real or instances/synthetic, or check --only"
  rm -rf "$raw_dir"
  exit 1
fi

# [CHECK] the programs must be built
IFS=',' read -r -a kern_list <<< "$kerns"
for k in "${kern_list[@]}"; do
  bin="./e2e_solve"
  [ "$k" = "gpu" ] && bin="./e2e_solve_gpu"
  if [ ! -x "$bin" ]; then
    echo "$bin not found, run ./scripts/build.sh first"
    exit 1
  fi
done

# [LOG] write a line to the screen and to the log
say() {
  echo "$*" | tee -a "$log"
}

# [LOG] header with the machine, the code version and the settings
gpu_name=$(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null | head -1)
cpu_name=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')
ram_gb=$(free -g | awk '/^Mem:/ {print $2}')
dirty=$(git status --porcelain --untracked-files=no 2>/dev/null | wc -l)
say "bench $(date '+%F %T')  commit $(git rev-parse --short HEAD)  branch $(git rev-parse --abbrev-ref HEAD)  changed_files $dirty"
say "machine  gpu ${gpu_name:-none}  cpu $cpu_name  ram ${ram_gb} GB"
say "settings  set $set_name (${#files[@]} instances)  kern $kerns  repeat $repeat  solver $solver  rounds $rounds  perturb $perturb  seed $seed  timeout $timeout_s  mem_limit $mem_gb  extra ${extra[*]:--}"
say "$(printf '%-8s  %-9s  %-44s  %-4s  %-3s  %-14s  %9s  %9s  %9s  %9s  %8s' time set instance kern rep status resolved kern_s total_s host_mb gpu_mb)"

# [INDEX] one row per program run, read later by collect_metrics.py
printf 'set\tinstance\tkern\trep\tstatus\trc\twall_s\traw\n' > "$index"

# [RUN] read one metric from a program output
metric() {
  sed -n "s/^\[m\] $1 : //p" "$2" | head -1
}

n_runs=0
n_ok=0
for f in "${files[@]}"; do
  # [RUN] set name from the folder the instance came from
  set_of="list"
  case "$f" in
    instances/real/*) set_of="real" ;;
    instances/synthetic/*) set_of="synthetic" ;;
  esac
  name=$(basename "$f")
  name="${name%.*}"

  for k in "${kern_list[@]}"; do
    for rep in $(seq 1 "$repeat"); do
      bin="./e2e_solve"
      [ "$k" = "gpu" ] && bin="./e2e_solve_gpu"
      raw="$raw_dir/${name}__${k}__r${rep}.out"
      cmd=("$bin" --kernelizer "$k" --solver "$solver" --max-rounds "$rounds" --perturb "$perturb" --seed "$seed" "${extra[@]}" "$f")

      # [RUN] the program, with a time limit and for non GPU runs a memory limit (CUDA needs a large virtual space)
      echo "# cmd: ${cmd[*]}" > "$raw"
      t0=$(date +%s.%N)
      (
        if [ "$k" != "gpu" ] && [ "$mem_gb" -gt 0 ]; then
          ulimit -v $((mem_gb * 1024 * 1024))
        fi
        if [ "$timeout_s" -gt 0 ]; then
          exec timeout --kill-after=30 "$timeout_s" "${cmd[@]}"
        else
          exec "${cmd[@]}"
        fi
      ) >> "$raw" 2>&1 < /dev/null
      rc=$?
      t1=$(date +%s.%N)
      wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", b - a }')
      echo "# rc: $rc  wall_s: $wall" >> "$raw"

      # [RUN] status from the program, or from the exit code when the program did not finish
      status=$(sed -n 's/^\[info\] status : //p' "$raw" | head -1)
      if [ -z "$status" ]; then
        case "$rc" in
          124|137) status="timeout" ;;
          *) status="fail_rc$rc" ;;
        esac
      fi
      n_runs=$((n_runs + 1))
      [ "$status" = "ok" ] && n_ok=$((n_ok + 1))

      # [LOG] one line per run, full numbers stay in the raw file and the tsv
      res=$(metric kern.resolved_total "$raw")
      kt=$(metric time.kernelize "$raw")
      tt=$(metric time.total "$raw")
      hm=$(metric mem.host_peak_mb "$raw")
      gm=$(metric mem.gpu_solver_mb "$raw")
      say "$(printf '%-8s  %-9s  %-44s  %-4s  %-3s  %-14s  %9s  %9.3f  %9.3f  %9.0f  %8.0f' \
        "$(date +%T)" "$set_of" "${name:0:44}" "$k" "$rep" "$status" "${res:--}" "${kt:-0}" "${tt:-0}" "${hm:-0}" "${gm:-0}")"
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$set_of" "$name" "$k" "$rep" "$status" "$rc" "$wall" "$raw" >> "$index"
    done
  done
done

# [TABLE] every metric of every run in one tsv
python3 scripts/lib/collect_metrics.py "$index" "$tsv"

# [SUMMARY] count of runs and total kernelization time per kernelizer
say "done  runs $n_runs  ok $n_ok  failed $((n_runs - n_ok))"
for k in "${kern_list[@]}"; do
  total=$(awk -F'\t' -v k="$k" 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == "time.kernelize") c = i; next }
    $3 == k && $5 == "ok" && c { s += $c } END { printf "%.3f", s }' "$tsv")
  say "kern $k  total kern_s (ok runs) $total"
done
say "files  $log  $tsv  $raw_dir/"
