#!/usr/bin/env bash
# Nsight Systems profile of the GPU kernelizer, kernelization only
#
#  For every instance in the given lists, one at a time:
#    1. a plain run with no profiler        true kernel time and resolved count
#    2. the same run under nsys              CUDA trace, converted to .nsys-rep
#    3. scripts/nsys_summary.py             one row per kernel, one row per instance
#  Output: results/<date>/nsys_<HHMMSS>/ , never overwritten
#
#  usage : ./scripts/run_nsys_profile.sh [list ...]
#          default lists: data/artifact/timing-large.list data/artifact/paper-synthetic.list
#  env   : MAXFLOW_GPU / MAXFLOW_GR_PERIOD are passed through (default: worklist, 400)
#          KEEP_SQLITE=1 keeps the exported .sqlite files (large)

set -u
cd "$(dirname "$0")/.."

BIN=./e2e_solve_gpu_prof
IMPORTER=/usr/lib/nsight-systems/host-linux-x64/QdstrmImporter
LISTS=("$@")
[ ${#LISTS[@]} -gt 0 ] || LISTS=(data/artifact/timing-large.list data/artifact/paper-synthetic.list)

[ -x "$BIN" ] || { echo "missing $BIN, build it first"; exit 1; }
[ -x "$IMPORTER" ] || { echo "missing $IMPORTER"; exit 1; }
for l in "${LISTS[@]}"; do
  [ -f "$l" ] || { echo "missing list $l"; exit 1; }
done

OUT="results/$(date +%F)/nsys_$(date +%H%M%S)"
RAW="$OUT/raw"
mkdir -p "$RAW"
SUM="$OUT/nsys_summary.tsv"
KER="$OUT/nsys_kernels.tsv"
LOG="$OUT/run.log"

{
  echo "date        : $(date -Iseconds)"
  echo "commit      : $(git log -1 --format='%h %s') ($(git status --porcelain | wc -l) dirty file(s))"
  echo "branch      : $(git rev-parse --abbrev-ref HEAD)"
  echo "solver      : MAXFLOW_GPU=${MAXFLOW_GPU:-<default worklist>} MAXFLOW_GR_PERIOD=${MAXFLOW_GR_PERIOD:-<default>}"
  echo "lists       : ${LISTS[*]}"
  echo "nvcc        : $(nvcc --version | tail -1)"
  echo "nsys        : $(nsys --version 2>&1 | head -1)"
  echo "gpu/driver  : $(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader)"
} > "$OUT/env.txt"

g() { printf "%s" "$1" | sed -n "s/^\[$2\] $3 *: *\([0-9.]*\).*/\1/p" | head -1; }

while IFS= read -r inst; do
  [ -n "$inst" ] || continue
  case "$inst" in \#*) continue ;; esac
  inst=${inst%%$'\t'*} 
  name=$(basename "$inst" .wcsp)
  if [ ! -f "$inst" ]; then
    echo "=== $name : file missing, skipped" | tee -a "$LOG"
    continue
  fi
  echo "=== $name   $(date +%T)" | tee -a "$LOG"

  #  1. plain run, no profiler
  plain=$("$BIN" --kernelizer gpu --solver none --max-rounds 1 "$inst" 2>&1 </dev/null)
  printf "%s\n" "$plain" > "$RAW/$name.plain.txt"
  p_time=$(g "$plain" kernel "time")
  p_res=$(g "$plain" kernel "resolved total")
  ccg=$(g "$plain" graph "vertices before")
  p_mf=$(printf "%s" "$plain" | sed -n 's/^\[time\] kern\.maxflow *: *\([0-9.]*\).*/\1/p' | head -1)

  #  2. same run under nsys, CUDA trace only
  nsys profile --trace=cuda --sample=none --cpuctxsw=none --force-overwrite=true \
    -o "$RAW/$name" "$BIN" --kernelizer gpu --solver none --max-rounds 1 "$inst" \
    > "$RAW/$name.nsys.txt" 2>&1 </dev/null
  if [ ! -f "$RAW/$name.nsys-rep" ] && [ -f "$RAW/$name.qdstrm" ]; then
    "$IMPORTER" -f -i "$RAW/$name.qdstrm" >> "$RAW/$name.nsys.txt" 2>&1 </dev/null
  fi
  rm -f "$RAW/$name.qdstrm"
  if [ ! -f "$RAW/$name.nsys-rep" ]; then
    echo "  no report produced, see $RAW/$name.nsys.txt" | tee -a "$LOG"
    continue
  fi
  nsys export --type=sqlite --force-overwrite=true -o "$RAW/$name.sqlite" "$RAW/$name.nsys-rep" \
    >> "$RAW/$name.nsys.txt" 2>&1 </dev/null
  q_res=$(g "$(cat "$RAW/$name.nsys.txt")" kernel "resolved total")

  #  3. summarise
  python3 scripts/nsys_summary.py "$RAW/$name.sqlite" "$name" "${ccg:-NA}" "${p_time:-NA}" \
    "${p_mf:-NA}" "${p_res:-NA}" "${q_res:-NA}" "$SUM" "$KER" 2>&1 | tee -a "$LOG"
  [ "${KEEP_SQLITE:-0}" = 1 ] || rm -f "$RAW/$name.sqlite"
done < <(cat "${LISTS[@]}")

cat > "$OUT/nsys_summary.legend.txt" << 'EOF'
nsys_summary.tsv  one row per instance, GPU kernelizer, one kernelization round, no exact solve
  ccg_vertices        CCG vertices before kernelization
  plain_kern_s        kernelization time of the plain run (no profiler); the true time
  plain_maxflow_s     max-flow stage of the plain run (kern.maxflow)
  plain_resolved      variables decided in the plain run
  prof_resolved       variables decided in the profiled run
  resolved_match      yes when both runs decided the same count (profiler changed nothing)
  gpu_window_s        first to last GPU activity in the trace
  gpu_busy_s          time the GPU was running a kernel, copy or memset
  gpu_idle_pct        share of the window the GPU sat idle waiting for the CPU
                      (launch overhead, host synchronisation, host side work)
  kernel_s            sum of all kernel durations
  kernel_launches     number of kernel launches
  pct_*               share of kernel time per algorithm step:
                        global_relabel = gpu_bfs_* kernels (backward BFS)
                        push_relabel   = push-relabel kernel
                        remove_invalid = remove-invalid-edges kernel
                        build_worklist = worklist construction kernel
                        other          = initialise, saturate source, etc.
  top_kernel          kernel with the largest total time
  htod_ms / dtoh_ms   GPU time of host-to-device / device-to-host copies
  dtoh_count          number of device-to-host copies (flag and count readbacks)
  sync_api_s          CPU time spent inside cudaDeviceSynchronize, and number of calls
  memcpy_api_s        CPU time spent inside cudaMemcpy calls
  launch_api_s        CPU time spent inside cudaLaunchKernel calls
Kernel and copy durations come from the GPU trace and are accurate; the profiler inflates
wall-clock time, so true end-to-end times are the plain_* columns.

nsys_kernels.tsv  one row per instance and kernel
  launches, total_ms, avg_us, max_us, pct_kernel_time (of all kernels), pct_gpu_window
EOF
cp "$OUT/nsys_summary.legend.txt" "$OUT/nsys_kernels.legend.txt"
echo "done: $OUT" | tee -a "$LOG"