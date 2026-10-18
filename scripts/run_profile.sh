#!/usr/bin/env bash
# run_profile.sh
# per kernel GPU times of the GPU kernelizer with nsys, and when asked detailed kernel numbers with ncu
# every instance runs one kernelization round with no exact solve, in up to three ways:
#   plain : no profiler, gives the true times and the resolved count
#   nsys  : CUDA trace, gives time and launches of every kernel, the copies and the idle time of the GPU
#   ncu   : only with --ncu, every kernel seen in the trace is measured on --ncu-count launches from the middle of the run
#
# usage:
#   ./scripts/run_profile.sh [options] [-- extra e2e_solve flags]
#   ./scripts/run_profile.sh --compare OLD.tsv NEW.tsv [--gurobi BENCH.tsv]
#   -s, --set real|synthetic|all|FILE   instances to run, same as in run_bench.sh   (default all)
#       --only PATTERN                  only instances whose path matches this grep pattern
#       --ncu                           also run ncu, it is slow so better together with --only
#       --ncu-count N                   launches measured for every kernel           (default 10)
#   -t, --timeout SEC                   limit for one program run, 0 = no limit      (default 3600)
#   -n, --name LABEL                    word added to the file names, to recognise the run later
#       --compare OLD NEW               before and after table of two profile_*.tsv files, nothing is profiled
#       --gurobi FILE                   bench tsv with "-k lp" runs, adds the Gurobi time to the compare table, can be repeated
#   --                                  everything after it goes to e2e_solve, for example -- --hub-deg 0
#
# output in results/<date>/, a new set of files for every run:
#   profile_<time>[_label].log           readable log, one line per instance
#   profile_<time>[_label].tsv           one row per instance
#   profile_<time>[_label]_kernels.tsv   one row per instance and kernel, the ncu columns are filled with --ncu
#   profile_<time>[_label]/              program outputs, .nsys-rep and .ncu-rep files to open in the Nsight GUIs
#   compare_<time>[_label].log and .tsv  for --compare

set -u
cd "$(dirname "$0")/.." || exit 1

# [ARGS] defaults
set_name="all"
only=""
use_ncu=0
ncu_count=10
timeout_s=3600
label=""
extra=()
cmp_old=""
cmp_new=""
gurobi=()

# [ARGS] read the command line
while [ $# -gt 0 ]; do
  case "$1" in
    -s|--set) set_name="$2"; shift 2 ;;
    --only) only="$2"; shift 2 ;;
    --ncu) use_ncu=1; shift ;;
    --ncu-count) ncu_count="$2"; shift 2 ;;
    -t|--timeout) timeout_s="$2"; shift 2 ;;
    -n|--name) label="_$2"; shift 2 ;;
    --compare) cmp_old="$2"; cmp_new="$3"; shift 3 ;;
    --gurobi) gurobi+=("$2"); shift 2 ;;
    --) shift; extra=("$@"); break ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 1 ;;
  esac
done

day=$(date +%F)
out_dir="results/$day"
mkdir -p "$out_dir"

# [COMPARE] before and after table, no profiling
if [ -n "$cmp_old" ]; then
  for f in "$cmp_old" "$cmp_new" "${gurobi[@]}"; do
    if [ ! -f "$f" ]; then
      echo "file not found: $f"
      exit 1
    fi
  done
  stamp="compare_$(date +%H%M%S)${label}"
  {
    echo "compare $(date '+%F %T')"
    echo "old     $cmp_old"
    echo "new     $cmp_new"
    echo "gurobi  ${gurobi[*]:--}"
    python3 scripts/lib/profile_tables.py compare "$cmp_old" "$cmp_new" "$out_dir/$stamp.tsv" "${gurobi[@]}"
    rc=$?
    echo "files  $out_dir/$stamp.log  $out_dir/$stamp.tsv"
    exit $rc
  } 2>&1 | tee "$out_dir/$stamp.log"
  exit "${PIPESTATUS[0]}"
fi

# [FILES] names of the output files of this run
stamp="profile_$(date +%H%M%S)${label}"
raw_dir="$out_dir/$stamp"
log="$out_dir/$stamp.log"
sum_tsv="$out_dir/$stamp.tsv"
ker_tsv="$out_dir/${stamp}_kernels.tsv"
mkdir -p "$raw_dir"

# [INSTANCES] make the lists from the two folders, then pick the set, same as run_bench.sh
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

# [CHECK] program and profilers
if [ ! -x ./e2e_solve_gpu ]; then
  echo "./e2e_solve_gpu not found, run ./scripts/build.sh first"
  exit 1
fi
if ! command -v nsys >/dev/null 2>&1; then
  echo "nsys not found"
  exit 1
fi
if [ "$use_ncu" = 1 ] && ! command -v ncu >/dev/null 2>&1; then
  echo "ncu not found"
  exit 1
fi

# [NSYS] some nsys versions write only a .qdstrm file, this program turns it into the .nsys-rep
importer=$(ls /usr/lib/nsight-systems/host-linux-x64/QdstrmImporter /opt/nvidia/nsight-systems/*/host-linux-x64/QdstrmImporter 2>/dev/null | head -1)
ncu_metrics=$(python3 scripts/lib/profile_tables.py metrics)

# [LOG] write a line to the screen and to the log
say() {
  echo "$*" | tee -a "$log"
}

# [RUN] run a command with the time limit
run_limited() {
  if [ "$timeout_s" -gt 0 ]; then
    timeout --kill-after=30 "$timeout_s" "$@"
  else
    "$@"
  fi
}

# [LOG] header with the machine, the code version, the tools and the settings
gpu_name=$(nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader 2>/dev/null | head -1)
dirty=$(git status --porcelain --untracked-files=no 2>/dev/null | wc -l)
say "profile $(date '+%F %T')  commit $(git rev-parse --short HEAD)  branch $(git rev-parse --abbrev-ref HEAD)  changed_files $dirty"
say "machine  gpu ${gpu_name:-none}"
say "tools  $(nsys --version 2>&1 | head -1)  ncu $([ "$use_ncu" = 1 ] && ncu --version | tail -1 || echo off)  importer ${importer:-none}"
say "settings  set $set_name (${#files[@]} instances)  ncu $use_ncu  ncu_count $ncu_count  timeout $timeout_s  extra ${extra[*]:--}"
say "$(printf '%-8s  %-9s  %-44s  %-12s  %9s  %9s  %s' time set instance status resolved kern_s 'match / gpu idle % / share of kernel time %')"

n_inst=0
n_trace=0
for f in "${files[@]}"; do
  set_of="list"
  case "$f" in
    instances/real/*) set_of="real" ;;
    instances/synthetic/*) set_of="synthetic" ;;
  esac
  name=$(basename "$f")
  name="${name%.*}"
  base="$raw_dir/$name"
  cmd=(./e2e_solve_gpu --kernelizer gpu --solver none --max-rounds 1 "${extra[@]}" "$f")
  n_inst=$((n_inst + 1))

  # [PLAIN] run without profiler
  echo "# cmd: ${cmd[*]}" > "$base.plain.out"
  run_limited "${cmd[@]}" >> "$base.plain.out" 2>&1 < /dev/null
  rc=$?
  echo "# rc: $rc" >> "$base.plain.out"
  status=$(sed -n 's/^\[info\] status : //p' "$base.plain.out" | head -1)
  [ -z "$status" ] && status="fail_rc$rc"
  res=$(sed -n 's/^\[m\] kern.resolved_total : //p' "$base.plain.out" | head -1)
  kt=$(sed -n 's/^\[m\] time.kernelize : //p' "$base.plain.out" | head -1)
  line_start=$(printf '%-8s  %-9s  %-44s  %-12s  %9s  %9.3f' "$(date +%T)" "$set_of" "${name:0:44}" "$status" "${res:--}" "${kt:-0}")

  # [PLAIN] no profiling when the plain run failed, the row keeps the status
  if [ "$status" != "ok" ]; then
    python3 scripts/lib/profile_tables.py instance "$set_of" "$name" "$base.plain.out" - - "$sum_tsv" "$ker_tsv" > /dev/null
    say "$line_start  not profiled"
    continue
  fi

  # [NSYS] same run under nsys, CUDA trace only
  run_limited nsys profile --trace=cuda --sample=none --cpuctxsw=none --force-overwrite=true -o "$base" "${cmd[@]}" \
    > "$base.nsys.out" 2>&1 < /dev/null
  if [ ! -f "$base.nsys-rep" ] && [ -f "$base.qdstrm" ] && [ -n "$importer" ]; then
    "$importer" -f -i "$base.qdstrm" >> "$base.nsys.out" 2>&1 < /dev/null
  fi
  trace="-"
  if [ -f "$base.nsys-rep" ]; then
    rm -f "$base.qdstrm"
    nsys export --type=sqlite --force-overwrite=true -o "$base.sqlite" "$base.nsys-rep" >> "$base.nsys.out" 2>&1 < /dev/null
    [ -f "$base.sqlite" ] && trace="$base.sqlite"
  fi
  [ "$trace" != "-" ] && n_trace=$((n_trace + 1))

  # [NCU] every kernel of the trace, launches taken from the middle of the run so they are not only the first ones
  csvs=()
  if [ "$use_ncu" = 1 ] && [ "$trace" != "-" ]; then
    while read -r kname klaunches; do
      skip=0
      [ "$klaunches" -gt "$ncu_count" ] && skip=$(( (klaunches - ncu_count) / 2 ))
      rep="$base.ncu_$kname"
      run_limited ncu --set full --import-source yes --metrics "$ncu_metrics" -k "regex:$kname" -s "$skip" -c "$ncu_count" \
        -f -o "$rep" "${cmd[@]}" > "$rep.out" 2>&1 < /dev/null
      if [ -f "$rep.ncu-rep" ]; then
        ncu --import "$rep.ncu-rep" --page raw --csv --metrics "$ncu_metrics" > "$rep.csv" 2>> "$rep.out"
        csvs+=("$rep.csv")
      fi
    done < <(python3 scripts/lib/profile_tables.py launches "$trace")
  fi

  # [TABLE] rows of this instance, the big sqlite file is not kept
  nsys_out="$base.nsys.out"
  [ "$trace" = "-" ] && nsys_out="-"
  summary=$(python3 scripts/lib/profile_tables.py instance "$set_of" "$name" "$base.plain.out" "$nsys_out" "$trace" \
    "$sum_tsv" "$ker_tsv" "${csvs[@]}" 2>&1)
  rm -f "$base.sqlite"
  say "$line_start  $summary"
done

# [SUMMARY] counts and file names
say "done  instances $n_inst  traced $n_trace"
say "files  $log  $sum_tsv  $ker_tsv  $raw_dir/"
