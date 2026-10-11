#!/usr/bin/env bash
#  run_ncu_profile.sh  Nsight Compute measurements of the GPU kernelizer's main kernels
#
#  For every instance in the given list, two captures:
#    bfs  : the FIRST gpu_bfs_step_kernel launch = BFS level 0 of the first global relabel
#           (the level where one thread walks every edge of the sink)
#    pr   : 20 launches of push-relabel / remove-invalid, taken after the first SKIP
#           launches so the sample is from the middle of the run, not the start
#  Each capture keeps the .ncu-rep (open it in the Nsight Compute GUI) and a raw CSV
#  of the metrics below; scripts/ncu_summary.py turns the CSVs into ncu_summary.tsv.
#  Output: results/<date>/ncu_<HHMMSS>/ , never overwritten
#
#  usage : ./scripts/run_ncu_profile.sh LIST
#  env   : SKIP (default 1000) launches to skip before the push/remove sample

set -u
cd "$(dirname "$0")/.."

BIN=./e2e_solve_gpu_prof
LIST=${1:-}
SKIP=${SKIP:-1000}
METRICS="gpu__time_duration.sum,launch__grid_size,launch__block_size,l1tex__t_sectors_pipe_lsu_mem_global_op_ld.sum,l1tex__t_requests_pipe_lsu_mem_global_op_ld.sum,smsp__thread_inst_executed_per_inst_executed.ratio,sm__warps_active.avg.pct_of_peak_sustained_active,sm__cycles_active.avg,sm__cycles_elapsed.avg,dram__throughput.avg.pct_of_peak_sustained_elapsed,sm__throughput.avg.pct_of_peak_sustained_elapsed"

[ -n "$LIST" ] && [ -f "$LIST" ] || { echo "usage: $0 LIST"; exit 1; }
[ -x "$BIN" ] || { echo "missing $BIN, build it first"; exit 1; }

OUT="results/$(date +%F)/ncu_$(date +%H%M%S)"
RAW="$OUT/raw"
mkdir -p "$RAW"
SUM="$OUT/ncu_summary.tsv"
LOG="$OUT/run.log"

{
  echo "date        : $(date -Iseconds)"
  echo "commit      : $(git log -1 --format='%h %s') ($(git status --porcelain | wc -l) dirty file(s))"
  echo "branch      : $(git rev-parse --abbrev-ref HEAD)"
  echo "binary      : $BIN"
  echo "ncu         : $(ncu --version 2>&1 | tail -1)"
  echo "gpu/driver  : $(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader)"
  echo "push sample : skip $SKIP matching launches, then 20"
  echo "list        : $LIST"
} > "$OUT/env.txt"

while IFS= read -r inst; do
  [ -n "$inst" ] || continue
  case "$inst" in \#*) continue ;; esac
  inst=${inst%%$'\t'*}    # keep only the path; some lists carry a size column after a tab
  name=$(basename "$inst" .wcsp)
  if [ ! -f "$inst" ]; then
    echo "=== $name : file missing, skipped" | tee -a "$LOG"
    continue
  fi
  echo "=== $name   $(date +%T)" | tee -a "$LOG"

  #  bfs: first BFS level launch only
  ncu --set full --metrics "$METRICS" -k regex:gpu_bfs_step_kernel -c 1 -f -o "$RAW/$name.bfs" \
    "$BIN" --kernelizer gpu --solver none --max-rounds 1 "$inst" > "$RAW/$name.bfs.txt" 2>&1 </dev/null
  #  pr: 20 push-relabel / remove-invalid launches from the middle of the run
  ncu --set full --metrics "$METRICS" -k "regex:wl_push_relabel_kernel|wl_remove_invalid_edges_kernel" \
    -s "$SKIP" -c 20 -f -o "$RAW/$name.pr" \
    "$BIN" --kernelizer gpu --solver none --max-rounds 1 "$inst" > "$RAW/$name.pr.txt" 2>&1 </dev/null

  csvs=()
  for c in bfs pr; do
    if [ -f "$RAW/$name.$c.ncu-rep" ]; then
      ncu --import "$RAW/$name.$c.ncu-rep" --page raw --csv --metrics "$METRICS" > "$RAW/$name.$c.csv" 2>/dev/null
      csvs+=("$RAW/$name.$c.csv")
    else
      echo "  no $c report, see $RAW/$name.$c.txt" | tee -a "$LOG"
    fi
  done
  [ ${#csvs[@]} -gt 0 ] && python3 scripts/ncu_summary.py "$SUM" "$name" "${csvs[@]}" 2>&1 | tee -a "$LOG"
done < "$LIST"

cat > "$OUT/ncu_summary.legend.txt" << 'LEGEND'
ncu_summary.tsv  one row per instance and kernel, averaged over the sampled launches
  launches_sampled         how many launches were measured (bfs: 1 = level 0; pr: about 10 each)
  avg_time_us              kernel duration under ncu (clocks locked to base, so slower than normal runs;
                           compare kernels with each other, not with nsys times)
  grid_blocks, block_threads  launch size; threads launched = grid x block
  sectors_per_request      32-byte memory sectors fetched per warp load instruction.
                           Coalesced 4-byte loads need 4 (one 128-byte line for 32 threads);
                           8-byte (double) loads need 8; 32 means every thread hit a different sector
  active_threads_per_warp  of the 32 threads in a warp, how many did useful work on average
                           (32 = perfect; low = idle or diverged threads)
  achieved_occupancy_pct   warps resident on an SM as a share of the maximum the SM can hold
  sm_active_pct            share of the kernel's time the average SM had any work at all
  dram_throughput_pct      memory bandwidth used, as a share of the GPU's peak
  sm_throughput_pct        compute throughput used, as a share of the GPU's peak
LEGEND
echo "done: $OUT" | tee -a "$LOG"