# wcsp-maxflow-kernelizer

A GPU-accelerated **Nemhauser–Trotter kernelizer** for Weighted Constraint
Satisfaction Problems (WCSPs), built as a drop-in replacement for the Gurobi
linear-programming kernelizer inside **WCSPLift**.

Algorithm source: *Scalable Maxflow Processing for Dynamic Graphs* —
Kannappan, Kumar and Nasre (IIT Madras, 2025). Only the **static** max-flow
algorithm is used.

---

## 1. What the project does

A WCSP is turned into a graph — the **constraint composite graph (CCG)** — whose
minimum-weight vertex cover solves the original problem. Before the real solver
runs, a **kernelizer** fixes every variable whose value is already forced, so the
solver only searches what is left.

WCSPLift does that by solving a linear program with Gurobi. This project does it
by computing a **max-flow / min-cut** on the bipartite double cover of the CCG,
once on the CPU and once on the GPU, and compares all three for correctness,
speed and memory.

```
  .wcsp file
       |
       v
   parse  ->  polynomial  ->  CCG  ->  KERNELIZER  ->  reduced problem
                                          |
                        +-----------------+-----------------+
                        |                 |                 |
                   Gurobi LP        CPU max-flow      GPU max-flow
                   (baseline)        (reference)       (this work)
```

---

## 2. Running a test

One command, one benchmark file, one log:

```bash
./scripts/run_test.sh data/bench/06_300k.wcsp
```

This compiles what is needed, collects the system configuration, measures the
graph sizes, runs the correctness checks, times every available kernelizer, and
writes everything to:

```
log/06_300k_20260812_143052.log
        ^          ^        ^
    test name     date     time
```

Re-running never overwrites an earlier log.

### Prerequisites

| Requirement | Needed for | If missing |
|---|---|---|
| g++ (C++17), Boost Graph, OpenBLAS | everything | build fails |
| CUDA toolkit (`nvcc`) + NVIDIA GPU | GPU kernelizer | GPU steps skipped automatically |
| Gurobi, with `GUROBI_HOME` set | LP baseline | LP steps skipped automatically |
| Python 3 | generating instances | only needed for `gen_*` scripts |

On Ubuntu:

```bash
sudo apt install g++ libboost-graph-dev libboost-program-options-dev libopenblas-dev python3
```

The script reads the GPU's compute capability from `nvidia-smi` and picks the
right `-arch=sm_XX` itself, so nothing needs editing for a different card.

### Memory warning

A 1,000,000-variable instance peaks near **17 GB of RAM**. Run only one copy of
the script at a time — two together will be killed by the Linux out-of-memory
handler. Use `-t` to put a time limit on very large runs.

---

## 3. Runner flags

```
./scripts/run_test.sh [options] <file.wcsp>
```

| Flag | Meaning | Default |
|---|---|---|
| `-a, --approach LIST` | Which kernelizers to run: `cpu`, `gpu`, `gurobi`, `all`. Comma-separated, e.g. `-a cpu,gpu` | `all` |
| `-r, --repeats N` | Timed repeats per approach. Use 3+ for numbers you intend to quote — run-to-run variance is about 6% | `1` |
| `-o, --outdir DIR` | Where the log is written | `log` |
| `-n, --name NAME` | Test name used in the log filename | input file's base name |
| `-t, --timeout SEC` | Kill any single run after this many seconds | none |
| `--arch sm_XX` | Force the CUDA architecture instead of auto-detecting | auto |
| `--kernel-cycles N` | Override the push-relabel `kernel_cycles` parameter | solver heuristic \|E\|/\|V\| |
| `--bfs MODE` | GPU BFS variant: `topology` or `frontier` | `topology` |
| `--no-build` | Use existing binaries, skip compiling | build |
| `--no-audit` | Skip the min-cut validity and integrality check | audit on |
| `--no-compare` | Skip the kernel agreement comparisons | compare on |
| `--no-profile` | Skip the GPU per-kernel profiling run | profile on |
| `-q, --quiet` | Less console output; the log is unchanged | verbose |
| `-h, --help` | Show all options | — |

### Examples

```bash
# Everything, default settings
./scripts/run_test.sh data/bench/06_300k.wcsp

# Only the two max-flow kernelizers, three repeats for stable timings
./scripts/run_test.sh -a cpu,gpu -r 3 data/bench/07_300k_pairwise.wcsp

# A large run: skip comparisons, cap at one hour, custom log name
./scripts/run_test.sh -n bigrun --no-compare -t 3600 data/bench/10_1M.wcsp

# Parameter experiment on the GPU solver
./scripts/run_test.sh --bfs frontier --kernel-cycles 50 data/bench/05_50k.wcsp

# Re-run without recompiling
./scripts/run_test.sh --no-build data/bench/04_10k.wcsp
```

---

## 4. Reading the log

The log ends with a **SUMMARY** section collecting the important lines.

| Line | Meaning |
|---|---|
| `CCG vertex count` | size of the constraint composite graph |
| `double cover: N nodes, M half-edges` | size of the flow network actually solved |
| `Remnant s=..., resolved=N` | how many variables the kernelizer fixed |
| `VALID MIN-CUT? : saturated` | the cut really is a minimum cut |
| `=> CLEAN` | all arithmetic stayed exactly integral |
| `decided by both, OPPOSITE: 0` | **must be 0** — a non-zero value means a kernelizer is wrong |
| `[KernelizerMaxflow] 41.9 s` | time in the kernelization stage alone |
| `[resource] wall … peak RSS … KB` | wall time and peak memory of that step |
| `global relabel: … (99.6%)` | share of GPU time per kernel |

Stages named `[parse DIMACS]`, `[toPolynomial all]`, `[ccg.addPolynomial]`,
`[ccg.simplify]` and `[getGraph copy]` are shared by all three approaches, so
only the `[Kernelizer…]` line should be used when comparing them.

---

## 5. Generating benchmark instances

```bash
./scripts/gen_benchmark_suite.sh small     # everything up to 50k variables
./scripts/gen_benchmark_suite.sh 10_1M     # one instance by name
./scripts/gen_benchmark_suite.sh           # all 11 instances
```

Seeds are fixed inside the script, so the same command always produces
byte-identical files. `data/bench/MANIFEST.txt` records each instance's seed and
checksum. A single instance can also be made directly:

```bash
python3 scripts/gen_wcsp.py 100000 150000 1234 out.wcsp           # mixed arity 2-4
python3 scripts/gen_wcsp_lowarity.py 100000 150000 1234 out.wcsp  # pairwise only
```

---

## 6. File guide

### Runner and scripts (`scripts/`)

| File | Description |
|---|---|
| `run_test.sh` | **Main entry point.** Runs all checks and benchmarks on one `.wcsp` file and writes a single timestamped log. |
| `run_benchmarks.sh` | Batch version: runs a whole set of instances in sequence, one log directory per session. |
| `gen_benchmark_suite.sh` | Generates the 11-instance benchmark suite from fixed seeds and writes a manifest. |
| `gen_wcsp.py` | Generates one random WCSP with mixed arity (2–4 variables per constraint). |
| `gen_wcsp_lowarity.py` | Same, restricted to pairwise (arity 2) constraints. |
| `benchmark_to_excel.py` | Collects benchmark numbers into a spreadsheet. |
| `summarize_results.py` | Summarises CCG size trends across instances. |

### Programs (`apps/`)

| File | Description |
|---|---|
| `kernelizer_maxflow_test.cpp` | Unit tests on small hand-built graphs with known answers. If these fail, nothing else is trustworthy. |
| `maxflow_cpu_demo.cpp` | Runs the CPU max-flow solver on a plain DIMACS graph; prints the flow value and the min-cut. |
| `maxflow_gpu_topology_demo.cu` | The same on the GPU. Its output must match the CPU exactly. |
| `e2e_pipeline_test.cpp` | Full pipeline using the **CPU** kernelizer, with per-stage timings. |
| `e2e_pipeline_test_gpu.cu` | Full pipeline using the **GPU** kernelizer. Compile with `-DMAXFLOW_PROFILE` for the per-kernel breakdown. |
| `e2e_pipeline_test_gurobi.cpp` | Full pipeline using the **Gurobi LP** kernelizer — the baseline. |
| `e2e_pipeline_nokernelize.cpp` | Stops before kernelizing; used to measure CCG size and build time alone. |
| `compare_kernels.cpp` | Compares the max-flow kernel with the Gurobi kernel variable by variable. Has a split mode (`mf` / `lp` / `cmp`) so large instances can run as separate processes. |
| `compare_kernels_gpu.cu` | Compares the CPU max-flow kernel with the GPU max-flow kernel. |
| `cut_audit.cpp` | Verifies the cut is a true minimum cut and that all arithmetic stayed integral. |
| `benchmark_cpu_gurobi.cpp` | Older standalone CPU-vs-Gurobi timing harness. |

### Algorithm code (`src/`)

| File | Description |
|---|---|
| `common/types.h` | Shared aliases: `cap_t` (capacity type), vertex/edge ids, and the floating-point tolerance. |
| `cpu/graph_csr.h` | The flow network structure (Bi-CSR — both directions stored, with reverse-edge indices for constant-time push). |
| `cpu/dimacs_reader.h` | Reads plain DIMACS max-flow graph files. |
| `cpu/maxflow_static.h` | CPU push-relabel max-flow solver — the reference implementation. |
| `gpu/maxflow_gpu_common.h` | CUDA kernels shared by all GPU variants (see kernel table below). |
| `gpu/maxflow_gpu_topology.h` | The GPU solver: the push-relabel kernel, the host-side main loop, and min-cut extraction. |
| `integration/KernelizerMaxflow.h` | Plugs the CPU solver into WCSPLift: builds the double cover, solves it, reads off the NT assignment. |
| `integration/KernelizerMaxflowGPU.h` | The same using the GPU solver. |

### Data (`data/`)

| Path | Description |
|---|---|
| `easy/`, `medium/`, `hard/` | Small DIMACS graphs for checking the solver. `medium.dimacs` has 6 nodes and is verifiable by hand (max-flow 23, cut {0,1,2,4}). |
| `bench/` | Generated WCSP benchmarks. `.wcsp` files are not in git (too large); `MANIFEST.txt` holds the seeds and checksums to regenerate them. |
| `wcsp/` | Earlier instance set, kept with its manifest as a historical record. |

`third_party/wcsp-solver/` is an unmodified copy of WCSPLift, supplying the WCSP
parser, the CCG construction and the Gurobi LP baseline.

---

## 7. CUDA kernels

All follow the same pattern: the host launches a kernel, waits for it, and
repeats. The four marked **main loop** run once per outer iteration.

| Kernel | File | Description |
|---|---|---|
| `gpu_initialize_kernel` | `maxflow_gpu_common.h` | Sets every residual capacity back to its original capacity, zeroes all excess, and sets heights (source = \|V\|, everything else 0). One thread per edge and per vertex. |
| `gpu_saturate_source_kernel` | `maxflow_gpu_common.h` | Pushes the full capacity of every outgoing source edge at once, creating the initial preflow. One thread per source out-edge. |
| `gpu_check_active_kernel` | `maxflow_gpu_common.h` | **Main loop.** Sets a flag if any vertex still has positive excess and height below \|V\|. When no vertex does, the algorithm has converged. |
| `gpu_bfs_init_kernel` | `maxflow_gpu_common.h` | Resets all heights to \|V\| (meaning "cannot reach the sink") and the sink to 0, before each global relabel. |
| `gpu_bfs_step_kernel` | `maxflow_gpu_common.h` | **Main loop.** One layer of the backward BFS that recomputes heights as shortest distance to the sink. Topology-driven: one thread per graph vertex, and threads not on the current layer exit immediately. This is the default and consumes about 99% of GPU time. |
| `gpu_bfs_frontier_step_kernel` | `maxflow_gpu_common.h` | Alternative BFS layer, data-driven: one thread per *frontier* vertex, claiming vertices with `atomicCAS`. Enabled with `--bfs frontier`. Measured about 15% slower than the topology-driven version; kept as a recorded negative result. |
| `topo_push_relabel_kernel` | `maxflow_gpu_topology.h` | **Main loop.** The core step: each active vertex finds its lowest neighbour reachable through a residual edge, then either pushes flow to it or raises its own height. Repeats up to `kernel_cycles` times per launch. |
| `gpu_remove_invalid_edges_kernel` | `maxflow_gpu_common.h` | **Main loop.** Repairs height-invariant violations caused by threads reading stale heights: cancels any residual edge that has become too steep and returns its flow. |
| `gpu_count_active_kernel` | `maxflow_gpu_common.h` | Profiling only. Counts active vertices instead of just flagging them, producing the active-vertex curve. Replaces `gpu_check_active_kernel` in `-DMAXFLOW_PROFILE` builds, so no extra launch is added. |

---

## 8. Current status

- CPU and GPU kernelizers produce **identical** kernels on all benchmark
  instances from 10 to 1,000,000 variables.
- Neither ever contradicts the Gurobi LP kernel — zero opposite assignments
  across roughly 1.6 million decided variables.
- All arithmetic is exactly integral, verified on networks up to 2.6 million nodes.
- On a 1M-variable mixed-arity instance the Gurobi kernelizer is killed by the
  out-of-memory handler at 19.5 GB (with and without presolve), while both
  max-flow kernelizers complete at 16.6 GB.
- Kernel reduction is scale-invariant within an arity class: about 1.1% of
  variables decided on mixed arity, about 89.8% on pairwise.
- Known limitation: about 99% of GPU kernelizer time is the global-relabel BFS.