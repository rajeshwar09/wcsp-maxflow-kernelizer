#!/usr/bin/env bash
# runs gpu_tune on a set of instances, one GPU setting changed at a time, and keeps the solve time of every setting
# instances are found in instances/real/ and instances/synthetic/ (files or links), same as run_bench.sh
#
# usage: ./scripts/run_tune.sh [options] [-- extra gpu_tune options]
#   -s, --set real|synthetic|all|FILE   instances to run, FILE = text file with one path per line   (default all)
#   -r, --repeat N                      runs of every setting                                     (default 3)
#   -t, --timeout SEC                   limit for one instance, 0 = no limit                       (default 1800)
#       --only PATTERN                  only instances whose path matches this grep pattern
#   -n, --name LABEL                    word added to the file names, to recognise the run later
#   --                                  everything after it goes to gpu_tune, for example -- --gr-list 200,400,800
#
# output in results/<date>/, a new set of files for every run:
#   tune_<time>[_label].log           readable log, one line per instance and a summary per setting
#   tune_<time>[_label].tsv           mean solve time of every setting of every instance
#   tune_<time>[_label]_degrees.tsv   degree histogram of the flow network of every instance
#   tune_<time>[_label]/              the full output of every gpu_tune run and the instance list used

set -u
cd "$(dirname "$0")/.." || exit 1

# [ARGS] defaults
set_name="all"
repeat=3
timeout_s=1800
only=""
label=""
extra=()

# [ARGS] read the command line
while [ $# -gt 0 ]; do
  case "$1" in
    -s|--set) set_name="$2"; shift 2 ;;
    -r|--repeat) repeat="$2"; shift 2 ;;
    -t|--timeout) timeout_s="$2"; shift 2 ;;
    --only) only="$2"; shift 2 ;;
    -n|--name) label="_$2"; shift 2 ;;
    --) shift; extra=("$@"); break ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 1 ;;
  esac
done

# [CHECK] the program must be built
if [ ! -x ./gpu_tune ]; then
  echo "./gpu_tune not found, run ./scripts/build.sh gates first"
  exit 1
fi

# [FILES] names of the output files of this run
day=$(date +%F)
stamp="tune_$(date +%H%M%S)${label}"
out_dir="results/$day"
raw_dir="$out_dir/$stamp"
log="$out_dir/$stamp.log"
tsv="$out_dir/$stamp.tsv"
deg_tsv="$out_dir/${stamp}_degrees.tsv"
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

# [LOG] write a line to the screen and to the log
say() {
  echo "$*" | tee -a "$log"
}

# [LOG] header with the machine, the code version and the settings
gpu_name=$(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null | head -1)
cpu_name=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')
dirty=$(git status --porcelain --untracked-files=no 2>/dev/null | wc -l)
say "tune $(date '+%F %T')  commit $(git rev-parse --short HEAD)  branch $(git rev-parse --abbrev-ref HEAD)  changed_files $dirty"
say "machine  gpu ${gpu_name:-none}  cpu $cpu_name"
say "settings  set $set_name (${#files[@]} instances)  repeat $repeat  timeout $timeout_s  extra ${extra[*]:--}"
say "$(printf '%-8s  %-9s  %-44s  %-12s  %9s  %-22s  %9s  %7s  %4s' time set instance status default_s best best_s gain_% same)"

# [TABLE] headers of the two tsv files
printf 'set\tinstance\tstatus\tknob\tvalue\tsolve_s\tpasses\tsame\n' > "$tsv"
printf 'set\tinstance\tlo\thi\tcount\n' > "$deg_tsv"

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
  raw="$raw_dir/${name}.out"
  cmd=(./gpu_tune --repeat "$repeat" "${extra[@]}" "$f")

  # [RUN] the program with a time limit
  echo "# cmd: ${cmd[*]}" > "$raw"
  t0=$(date +%s.%N)
  if [ "$timeout_s" -gt 0 ]; then
    timeout --kill-after=30 "$timeout_s" "${cmd[@]}" >> "$raw" 2>&1 < /dev/null
  else
    "${cmd[@]}" >> "$raw" 2>&1 < /dev/null
  fi
  rc=$?
  t1=$(date +%s.%N)
  wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", b - a }')
  echo "# rc: $rc  wall_s: $wall" >> "$raw"

  # [RUN] status from the exit code
  case "$rc" in
    0) status="ok" ;;
    5) status="skip_domain" ;;
    6) status="skip_arity" ;;
    7) status="different" ;;
    124|137) status="timeout" ;;
    *) status="fail_rc$rc" ;;
  esac
  if [ "$status" = "ok" ] && ! grep -q '^\[best\] all ' "$raw"; then
    status="empty"
  fi
  n_runs=$((n_runs + 1))
  [ "$status" = "ok" ] && n_ok=$((n_ok + 1))

  # [TABLE] mean rows and degree rows of this instance
  awk -v s="$set_of" -v i="$name" -v st="$status" '$1 == "[mean]" { printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", s, i, st, $2, $3, $4, $5, $6 }' "$raw" >> "$tsv"
  awk -v s="$set_of" -v i="$name" '$1 == "[deg]" { printf "%s\t%s\t%s\t%s\t%s\n", s, i, $2, $3, $5 }' "$raw" >> "$deg_tsv"

  # [LOG] one line per instance, [best] all = knob=value solve_s default_s gain_pct
  best=$(awk '$1 == "[best]" && $2 == "all" { print $3, $4, $5, $6 }' "$raw")
  read -r b_set b_s d_s gain <<< "${best:-- - - -}"
  same=$(sed -n 's/^\[info\] all_same : //p' "$raw" | head -1)
  say "$(printf '%-8s  %-9s  %-44s  %-12s  %9s  %-22s  %9s  %7s  %4s' \
    "$(date +%T)" "$set_of" "${name:0:44}" "$status" "$d_s" "${b_set:0:22}" "$b_s" "$gain" "${same:--}")"
done

# [SUMMARY] total solve time of every setting over the ok instances, the default row is the reference
say "done  instances $n_runs  ok $n_ok  not_ok $((n_runs - n_ok))"
say "$(printf '%-14s  %6s  %11s  %11s  %7s  %4s' knob value total_s default_s gain_% same)"
awk -F'\t' 'NR > 1 && $3 == "ok" {
    key = $4 " " $5
    if (!(key in total)) order[n++] = key
    total[key] += $6
    count[key]++
    if ($8 != "yes") diff[key]++
    if ($4 == "default") def += $6
  }
  END {
    for (j = 0; j < n; j++) {
      k = order[j]
      split(k, kv, " ")
      g = def > 0 ? 100 * (def - total[k]) / def : 0
      printf "%-14s  %6s  %11.4f  %11.4f  %7.1f  %4s\n", kv[1], kv[2], total[k], def, g, (k in diff) ? "NO" : "yes"
    }
  }' "$tsv" | tee -a "$log"
say "files  $log  $tsv  $deg_tsv  $raw_dir/"
