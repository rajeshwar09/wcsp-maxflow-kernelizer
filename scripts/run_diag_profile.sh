#!/usr/bin/env bash
#  run_diag_profile.sh  work-distribution diagnostics of the worklist GPU kernelizer
#
#  Runs the profile build (e2e_solve_gpu_diag, compiled with -DMAXFLOW_PROFILE) once per
#  instance and collects, per instance:
#    - the [diag] line printed by the worklist solver: vertex degrees, worklist sizes,
#      edges scanned per pass, the longest thread, BFS level-0 time
#    - the kernelization sub-stage times printed by the pipeline ([time] kern.*)
#  Output: results/<date>/diag_<HHMMSS>/ , never overwritten
#
#  usage : ./scripts/run_diag_profile.sh [list ...]
#          default lists: data/artifact/timing-large.list data/artifact/paper-synthetic.list

set -u
cd "$(dirname "$0")/.."

BIN=./e2e_solve_gpu_diag
LISTS=("$@")
[ ${#LISTS[@]} -gt 0 ] || LISTS=(data/artifact/timing-large.list data/artifact/paper-synthetic.list)

[ -x "$BIN" ] || { echo "missing $BIN, build it first"; exit 1; }
for l in "${LISTS[@]}"; do
  [ -f "$l" ] || { echo "missing list $l"; exit 1; }
done

OUT="results/$(date +%F)/diag_$(date +%H%M%S)"
RAW="$OUT/raw"
mkdir -p "$RAW"
TSV="$OUT/diag.tsv"
LOG="$OUT/run.log"

{
  echo "date        : $(date -Iseconds)"
  echo "commit      : $(git log -1 --format='%h %s') ($(git status --porcelain | wc -l) dirty file(s))"
  echo "branch      : $(git rev-parse --abbrev-ref HEAD)"
  echo "binary      : $BIN (built with -DMAXFLOW_PROFILE; times include profiling syncs)"
  echo "solver      : MAXFLOW_GPU=${MAXFLOW_GPU:-<default worklist>} MAXFLOW_GR_PERIOD=${MAXFLOW_GR_PERIOD:-<default>}"
  echo "lists       : ${LISTS[*]}"
} > "$OUT/env.txt"

KEYS="V E sink_deg max_deg n_deg64 n_deg1024 relabels bfs_levels bfs_kernel_s bfs_l0_s passes avg_wl max_wl avg_edges avg_per_thread avg_longest worst_longest imbalance hub_passes"
STAGES="kern.collect kern.build_flownet kern.maxflow kern.classify kern.apply"

{
  printf "instance\tkern_s\tresolved"
  for k in $KEYS; do printf "\t%s" "$k"; done
  for s in $STAGES; do printf "\t%s_s" "${s#kern.}"; done
  printf "\n"
} > "$TSV"

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

  "$BIN" --kernelizer gpu --solver none --max-rounds 1 "$inst" > "$RAW/$name.txt" 2>&1 </dev/null
  out="$RAW/$name.txt"

  kern=$(sed -n 's/^\[kernel\] time *: *\([0-9.]*\).*/\1/p' "$out" | head -1)
  res=$(sed -n 's/^\[kernel\] resolved total *: *\([0-9]*\).*/\1/p' "$out" | head -1)
  diag=$(sed -n 's/^\[diag\] //p' "$out" | head -1)
  if [ -z "$diag" ]; then
    echo "  no [diag] line, see $out" | tee -a "$LOG"
    continue
  fi

  {
    printf "%s\t%s\t%s" "$name" "${kern:-NA}" "${res:-NA}"
    for k in $KEYS; do
      v=$(printf "%s\n" $diag | sed -n "s/^$k=//p" | head -1)
      printf "\t%s" "${v:-NA}"
    done
    for s in $STAGES; do
      v=$(sed -n "s/^\[time\] $s *: *\([0-9.e-]*\).*/\1/p" "$out" | head -1)
      printf "\t%s" "${v:-NA}"
    done
    printf "\n"
  } >> "$TSV"

  grep -E "BFS level 0 only|degree  |worklist per pass|edges per pass|imbalance  " "$out" | sed 's/^/ /' | tee -a "$LOG"
done < <(cat "${LISTS[@]}")

cat > "$OUT/diag.legend.txt" << 'LEGEND'
diag.tsv  one row per instance, worklist GPU kernelizer, profile build (-DMAXFLOW_PROFILE)
Times in this table include the profiling synchronisations; true times are in nsys_summary.tsv.
  kern_s          kernelization time of this (profiled) run
  resolved        variables decided; must equal the plain run
  V, E            flow-network vertices (2 x CCG + 2) and half-edges
  sink_deg        edges at the sink = CCG vertices; BFS level 0 is one thread walking all of them
  max_deg         largest number of edges at any vertex other than source and sink
  n_deg64         vertices with at least 64 edges (more than two warps of work for one thread)
  n_deg1024       vertices with at least 1024 edges ("hubs")
  relabels        global relabels (backward BFS runs)
  bfs_levels      BFS levels over all relabels (one kernel launch each)
  bfs_kernel_s    time in BFS level kernels
  bfs_l0_s        time in BFS level 0 only (the sink's level)
  passes          push-relabel passes (outer iterations that launched push)
  avg_wl, max_wl  active vertices in the worklist per pass, average and largest
  avg_edges       edges scanned per pass, summed over all threads (one scan per vertex)
  avg_per_thread  average edges one thread scans in a pass
  avg_longest     edges scanned by the busiest thread in a pass, averaged over passes
  worst_longest   busiest thread over the whole run
  imbalance       avg_longest / avg_per_thread : how much longer the slowest thread works
                  than the average one; a kernel finishes only when its slowest thread does
  hub_passes      passes in which some thread had to scan 1024 or more edges
  *_s (stages)    kernelization sub-stages from the pipeline timer:
                  collect = read the CCG, build_flownet = build the double-cover network on the CPU,
                  maxflow = GPU solve (incl. copies and min-cut extraction), classify, apply
LEGEND
echo "done: $OUT" | tee -a "$LOG"