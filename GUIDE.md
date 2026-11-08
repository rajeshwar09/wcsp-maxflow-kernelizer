# Guide

This guide explains every file of the project, how to set it up on a new machine, how to run each program and script, what every flag does, and what every column of the output means. The README has the short version.

Contents

1. Files of the project
2. Setup and build
3. Instances
4. The pipeline program e2e_solve
5. Benchmark runs with run_bench.sh
6. Profiling with run_profile.sh
7. Benchmark data scripts
8. Gate tests
9. The GPU solver in short
10. Common tasks
11. Problems and fixes

## 1. Files of the project

### apps/ (programs)

| file | program | what it is |
|---|---|---|
| `e2e_solve.cpp` | `e2e_solve`, `e2e_solve_gpu` | the full pipeline for one instance: parse, CCG, kernelize, solve, final optimum. The `_gpu` build also has the GPU kernelizer |
| `wcsp_collapse.cpp` | `wcsp_collapse` | removes domain-1 variables (constants) from a .wcsp file so the CCG loader can read it |
| `mincut_lattice.cpp` | `mincut_lattice` | finds the most that any min-cut kernelizer could decide on an instance (the ceiling) |
| `kernelizer_maxflow_test.cpp` | `kmf_test` | gate test, 6 small hand-made graphs with known LP answers |
| `cut_audit.cpp` | `cut_audit` | gate test, checks that the cut we use is a real minimum cut and that all numbers stay whole numbers |
| `compare_kernels.cpp` | `compare_kernels` | gate test, max-flow kernel against the Gurobi LP kernel, variable by variable |
| `compare_kernels_gpu.cu` | `compare_kernels_gpu` | gate test, CPU kernel against GPU kernel, variable by variable |

### src/ (the max-flow code, header only)

| file | what it is |
|---|---|
| `common/types.h` | basic types: `cap_t` (capacity, double), vertex and edge ids, `MAXFLOW_EPSILON` |
| `common/metrics.h` | one store for all numbers of a run, printed at the end as `[m] name : value` |
| `cpu/graph_csr.h` | the flow network in CSR form with both directions of every edge and the index of the reverse edge |
| `cpu/maxflow_static.h` | push-relabel max-flow on the CPU and the min-cut source side |
| `gpu/maxflow_gpu_worklist.h` | push-relabel max-flow on the GPU (worklist version), all CUDA kernels and the GPU settings |
| `integration/KernelizerMaxflow.h` | the Nemhauser-Trotter kernelizer with the CPU solver: builds the double cover, solves it, fixes the variables |
| `integration/KernelizerMaxflowGPU.h` | the same kernelizer with the GPU solver |
| `integration/perturbation.h` | optional tie-breaking perturbation of the vertex weights (`--perturb`) |

### scripts/

| file | what it is |
|---|---|
| `build.sh` | builds all programs |
| `run_bench.sh` | runs e2e_solve on a set of instances, keeps time and memory of every run |
| `run_profile.sh` | nsys and ncu profiles of the GPU kernelizer, and before and after comparison |
| `gen_benchmark_suite.sh` | makes the synthetic instances of `data/bench/` and `data/wcsp/` |
| `gen_hard_suite.sh` | makes the "hard" synthetic instances of `data/bench/hard/` |
| `gen_wcsp.py` | makes one random Boolean WCSP (mixed arity 2 to 4, or pairwise) |
| `gen_wcsp_hard.py` | makes one random WCSP where the LP is likely to be fractional |
| `stage_artifact.sh` | copies the usable files of the published benchmark from the drive and writes the manifest |
| `run_collapse_audit.sh` | runs wcsp_collapse on the domain-1 instances and checks every result |
| `audit_collapse.py` | the checker used by run_collapse_audit.sh |
| `run_census.sh` | largest constraint arity of every instance of a set |
| `run_ceiling.sh` | runs mincut_lattice on a set and counts the verdicts |
| `lib/collect_metrics.py` | joins the `[m]` lines of many program outputs into one table (used by run_bench.sh) |
| `lib/profile_tables.py` | makes the tables of run_profile.sh from the nsys traces and ncu reports |

### data/

| path | what it is |
|---|---|
| `artifact/timing-large.list` | the 25 real instances used for timing (all are collapsed domain-1 instances), one path per line |
| `artifact/paper-synthetic.list` | the 7 synthetic instances used in the paper, path and size |
| `artifact/evalgm.list`, `artifact/uai.list` | the usable evalgm and uai files of the benchmark, path and variable count |
| `artifact/manifest.tsv` | class of every file of the published benchmark (boolean, domain1, multi), written by stage_artifact.sh |
| `artifact/arity-evalgm.tsv`, `artifact/arity-collapsed.tsv` | largest arity of every instance, written by run_census.sh |
| `artifact/domain1-audit.tsv`, `artifact/artifact-loadable.tsv`, `artifact/uai-census.tsv` | older records of the benchmark (domain-1 counts, loadable files, uai sizes) |
| `artifact/*.legend.txt` | column meanings of the tables above |
| `bench/MANIFEST.txt`, `wcsp/MANIFEST.txt` | name, size, seed and checksum of every synthetic instance. The .wcsp files are not in git, the suite script makes them again |

### other

- `third_party/wcsp-solver/` is WCSPLift. It gives the WCSP parser, the CCG, the Gurobi LP kernelizer, the ILP solver and the message passing solver. It is not changed.
- `instances/` holds `real/` and `synthetic/`, the folders that the scripts read. It is local only.
- `results/` gets every log and table. It is local only.

## 2. Setup and build

### Packages

```bash
sudo apt install g++ libboost-graph-dev libopenblas-dev python3
```

For the GPU kernelizer you need an NVIDIA GPU and the CUDA toolkit, so that `nvcc` and `nvidia-smi` work. For profiling you need `nsys` and `ncu` (Nsight Systems and Nsight Compute).

### Gurobi

Gurobi is needed for the LP kernelizer (`--kernelizer lp`), the ILP solver (`--solver ilp`) and `compare_kernels`. Without it everything else still builds and runs.

```bash
export GUROBI_HOME=/path/to/gurobi1302/linux64
export LD_LIBRARY_PATH=$GUROBI_HOME/lib:$LD_LIBRARY_PATH
export GRB_LICENSE_FILE=/path/to/gurobi.lic
```

Put these lines in `~/.bashrc` (or `~/.zshrc`) so they are always set.

### Build

```bash
./scripts/build.sh            # everything
./scripts/build.sh core       # e2e_solve, e2e_solve_gpu, mincut_lattice, wcsp_collapse
./scripts/build.sh gates      # kmf_test, cut_audit, compare_kernels, compare_kernels_gpu
./scripts/build.sh --arch sm_86 core    # give the GPU architecture by hand
```

| option | meaning |
|---|---|
| `core`, `gates`, `all` | what to build (default `all`) |
| `--arch sm_XX` | GPU architecture, by default it is read from `nvidia-smi` |

It prints one line per program: `ok`, `FAILED` or `skipped` with the reason. A program is skipped when it needs something the machine does not have (no nvcc for the GPU programs, no Gurobi for compare_kernels). The full compiler output is kept in `results/<date>/build_<time>.log`.

The programs are written to the repo folder (`./e2e_solve` and so on). After a build, run `./kmf_test`, it must print `Results: 6 / 6 passed`.

## 3. Instances

The scripts read instances from two folders:

```
instances/real/        real instances (from the WCSPLift benchmark)
instances/synthetic/   generated instances
```

A file in these folders can be a real file or a link. Every time `run_bench.sh` or `run_profile.sh` starts, it makes `instances/real.list` and `instances/synthetic.list` again from what is in the folders. So to add an instance you only put it (or a link to it) in one of the folders. Nothing in the scripts has to change. Both `.wcsp` and `.uai` files are used.

### Synthetic instances

```bash
./scripts/gen_benchmark_suite.sh                 # data/bench/, 01_10 to 12_1_3M
./scripts/gen_benchmark_suite.sh bench small     # only up to 50k variables
./scripts/gen_benchmark_suite.sh wcsp            # data/wcsp/, the older suite (03_100k is in the paper set)
./scripts/gen_benchmark_suite.sh wcsp 03_100k    # one instance
```

The seeds are fixed in the script, so the files are the same bytes every time. The script writes the checksums to `MANIFEST.txt`, compare them with the committed manifest to be sure. The big files take some minutes and some GB of disk.

To link the paper set:

```bash
cut -f1 data/artifact/paper-synthetic.list | while read -r f; do ln -s "$(realpath "$f")" instances/synthetic/; done
```

### Real instances

The real instances come from the published WCSPLift benchmark (about 200 GB, on a separate drive). Three steps:

1. `./scripts/stage_artifact.sh --source <drive folder>` copies the usable files to `~/mtp/wcsp-maxflow/artifact` (section 7).
2. `./scripts/run_collapse_audit.sh domain1` collapses the domain-1 instances into `~/mtp/wcsp-maxflow/collapsed` and checks them.
3. Link the timing set:

```bash
cut -f1 data/artifact/timing-large.list | while read -r f; do ln -s "$f" instances/real/; done
```

The paths inside the lists are the paths on the machine where they were made. On another machine change the start of the paths, for example with `sed 's#/home/raje/#/home/<you>/#'`.

## 4. The pipeline program e2e_solve

```bash
./e2e_solve [options] <file.wcsp|file.uai>
./e2e_solve_gpu [options] <file.wcsp|file.uai>
```

`e2e_solve` has the cpu, lp and none kernelizers. `e2e_solve_gpu` has all of them plus gpu. Both are built by `build.sh core`.

Steps: parse the file, check the arity, turn the constraints into polynomials, build and simplify the CCG, kernelize in rounds until a round decides nothing new, solve what is left, and work out the final optimum of the original problem.

### Main options

| option | values | default | meaning |
|---|---|---|---|
| `--kernelizer` | `none`, `cpu`, `gpu`, `lp` | `cpu` | which kernelizer. `none` skips kernelization, `lp` is the Gurobi baseline |
| `--solver` | `ilp`, `mp`, `none` | `ilp` | solver for the part left after kernelization. `ilp` is Gurobi, `mp` is message passing, `none` stops after kernelization |
| `--format` | `d`, `u`, `auto` | `auto` | input format, `auto` takes `.uai` files as UAI and everything else as DIMACS wcsp |
| `--max-rounds` | number | 100 | most kernelization rounds |
| `--max-arity` | number | 15 | an instance with a wider constraint is skipped (status `skip_arity`). The CCG needs 2^(2*arity) numbers for one constraint, about 8 GB at arity 15 |
| `--time-limit` | seconds | none | time limit for the solver, `mp` gets 300 s when no limit is given |

### Perturbation options

Many instances have ties (equal weights). The LP then has many optimal solutions, max-flow can land on the all-0.5 one and the min cut decides nothing. Small different changes to the weights break the ties without changing which covers are optimal. It only works with the cpu and gpu kernelizers, and only when all weights are whole numbers (UAI weights are decimals, so it switches itself off there).

| option | values | default | meaning |
|---|---|---|---|
| `--perturb` | `off`, `int`, `real` | `off` | `int`: w' = M*w + r with r from 1 to K and M > n*K, values stay whole numbers. `real` (for testing): w' = w + eps with eps below delta |
| `--spread` | number | 8 | K of int mode, must be at least 2 (with 1 everyone gets the same offset and no tie is broken) |
| `--delta` | number | 0 | delta of real mode, 0 means 1/(2n) |
| `--seed` | number | 1 | seed of the random offsets, every round uses seed + round |

### GPU options

These change the GPU solver, they only matter with `--kernelizer gpu`. The defaults are the tuned values, so normally you do not give them. They are for experiments, for example to switch one fix off and see what it gave.

| option | default | meaning |
|---|---|---|
| `--sink-bfs 0\|1` | 1 | fix A, BFS level 0 with one thread per sink edge |
| `--hub-deg N` | 64 | fix B, one warp for every vertex with at least N edges, 0 = off |
| `--block-deg N` | 1024 | fix C, one block for every vertex with at least N edges, 0 = off |
| `--bfs-split 0\|1` | 1 | fix D, each BFS level split into small, hub and block kernels |
| `--block-threads N` | 0 | fix D2, threads per block, 0 = chosen from the largest vertex (256, 512 or 1024) |
| `--async 0\|1` | 1 | fix E, one wait for the GPU per pass instead of one after every step |
| `--gr-period N` | 75 | passes between two global relabels (BFS from the sink) |
| `--kernel-cycles N` | 0 | push steps per vertex in one launch, 0 = edges / vertices |
| `--gpu-kernel-times` | off | time every step of the GPU loop. It adds waits, so the total gets slower, use it only for testing |

### Output

The program prints three kinds of lines:

```
[info] instance : data/bench/05_50k.wcsp      settings and results as text
[round] 1 resolved=552 new=552 ...            one line per kernelization round
[m] time.kernelize : 0.991                    every number of the run, at the end
```

The scripts read the `[m]` lines. A new number added to the program later shows up in the tables of run_bench.sh without any change to the scripts.

### Status and exit code

The line `[info] status : ...` gives the result:

| status | exit code | meaning |
|---|---|---|
| `ok` | 0 | finished |
| `solve_limit_hit` | 0 | the solver stopped at its time limit |
| `solver_timeout` | 4 | the ILP solver gave up at the time limit |
| `kernel_timeout` | 0 | the LP kernelizer hit the time limit |
| `kernel_error` | 0 | the kernelizer threw an error |
| `skip_domain` | 5 | a variable is not Boolean or has domain 1 (collapse the file first) |
| `skip_arity` | 6 | a constraint is wider than `--max-arity` |
| `oom` | 6 | out of memory |
| `error` | 7 | any other failure |

Exit code 1 means wrong options, 2 means the file cannot be opened, 3 means the program was built without GPU or Gurobi support for what was asked.

### The numbers ([m] lines)

Times are in seconds, memory in MB.

| name | meaning |
|---|---|
| `time.parse` | reading the file |
| `graph.max_arity` | widest constraint |
| `graph.ccg_need_gib` | memory the polynomial of the widest constraint needs, in GB |
| `time.to_polynomial`, `time.add_polynomial`, `time.simplify`, `time.graph_copy` | the CCG build steps |
| `graph.remnant_s` | constant part of the objective (same for every kernelizer, a good check) |
| `graph.total_vars` | WCSP variables |
| `graph.real_vars`, `graph.type1_aux`, `graph.type2_aux` | CCG vertices for variables and for the two kinds of auxiliary vertices |
| `graph.vertices_before`, `graph.edges_before` | CCG size before kernelization |
| `kern.simplified` | variables fixed by `ccg.simplify()` before the kernelizer |
| `time.kern.collect`, `time.kern.build_flownet`, `time.kern.maxflow`, `time.kern.classify`, `time.kern.apply` | the steps inside the max-flow kernelizer, summed over rounds |
| `time.gpu.setup` | GPU memory and copy of the network (includes the CUDA start on the first call) |
| `time.gpu.solve`, `time.gpu.result` | GPU max-flow and copy of the result back |
| `time.gpu.k_build`, `time.gpu.k_bfs`, `time.gpu.k_push`, `time.gpu.k_remove` | per step times, only with `--gpu-kernel-times` |
| `gpu.block_threads` | threads per block used |
| `gpu.passes`, `gpu.relabels`, `gpu.bfs_levels` | loop passes, global relabels and BFS levels of the GPU solver |
| `mem.gpu_alloc_mb` | GPU memory the solver asked for with cudaMalloc |
| `mem.gpu_solver_mb` | drop of free GPU memory while the solver ran |
| `mem.gpu_device_used_mb`, `mem.gpu_device_total_mb` | used and total memory of the whole GPU at that point |
| `kern.rounds` | kernelization rounds done |
| `kern.resolved_total` | variables fixed in total (simplify plus kernelizer) |
| `kern.by_kernelizer` | variables fixed by the kernelizer alone |
| `kern.var_reduction_pct` | fixed variables as a percent of all variables |
| `time.kernelize` | total kernelization time, the main number to compare kernelizers |
| `graph.vertices_after`, `graph.edges_after`, `kern.vertex_reduction_pct` | CCG size after kernelization and how much smaller it got |
| `time.solve` | solver time |
| `result.mwvc_weight`, `result.final_optimum` | weight of the vertex cover and the final optimum of the WCSP |
| `time.total` | whole run |
| `mem.host_peak_mb` | peak memory of the process in RAM |

## 5. Benchmark runs with run_bench.sh

```bash
./scripts/run_bench.sh [options] [-- extra e2e_solve options]
```

| option | default | meaning |
|---|---|---|
| `-s, --set` | `all` | `real`, `synthetic`, `all`, or a text file with one path per line (a second column is ignored) |
| `-k, --kern` | `gpu` | kernelizers, comma list of `gpu`, `cpu`, `lp`, `none` |
| `-r, --repeat` | 1 | runs of every instance and kernelizer |
| `--solver` | `none` | `none`, `ilp` or `mp` |
| `--rounds` | 1 | kernelization rounds |
| `--perturb` | `off` | `off`, `int` or `real` |
| `--seed` | 1 | perturbation seed |
| `-t, --timeout` | 3600 | seconds for one run, 0 = no limit |
| `-m, --mem-limit` | 18 | GB of memory for cpu, lp and none runs, 0 = no limit. GPU runs get no limit because CUDA needs a large virtual address space |
| `--only` | | only instances whose path matches this pattern (grep -E) |
| `-n, --name` | | word added to the file names |
| `--` | | everything after it goes to e2e_solve, for example `-- --hub-deg 0` |

gpu runs use `./e2e_solve_gpu`, the others use `./e2e_solve`.

Output in `results/<date>/`:

- `bench_<time>[_name].log` has a header (date, commit, branch, changed files, GPU, CPU, RAM, settings), one line per run and a summary.
- `bench_<time>[_name].tsv` has one row per run with every `[m]` number as a column.
- `bench_<time>[_name]/` has the full output of every run (`<instance>__<kern>__r<rep>.out`, with the command at the top and the exit code at the end) and the instance list used.

Log columns: time, set, instance, kern, rep, status, resolved (`kern.resolved_total`), kern_s (`time.kernelize`), total_s (`time.total`), host_mb (`mem.host_peak_mb`), gpu_mb (`mem.gpu_solver_mb`). When the program did not finish, the status is `timeout` or `fail_rcN`.

The tsv starts with the run columns set, instance, kern, rep, status, rc, wall_s (wall clock time of the whole program as the script saw it), raw (output file) and final_optimum, then one column per `[m]` name. A number that a run does not have (for example GPU memory in a cpu run) is empty.

Examples:

```bash
./scripts/run_bench.sh -k gpu,cpu,lp -r 3                        # all three kernelizers, 3 repeats
./scripts/run_bench.sh -s real -k gpu -n real-gpu                 # real set only
./scripts/run_bench.sh --only rus2 -k gpu -- --hub-deg 0          # fix B off, a few instances
./scripts/run_bench.sh -s synthetic -k gpu --solver ilp          # with the exact solve
./scripts/run_bench.sh -k gpu --perturb int -n perturb            # with perturbation
```

## 6. Profiling with run_profile.sh

```bash
./scripts/run_profile.sh [options] [-- extra e2e_solve options]
./scripts/run_profile.sh --compare OLD.tsv NEW.tsv [--gurobi BENCH.tsv]
```

Every instance runs the GPU kernelizer for one round with no solve, up to three times:

1. plain, no profiler: the true time and resolved count
2. under nsys: a CUDA trace with the time and launches of every kernel, the copies and how long the GPU waited
3. under ncu, only with `--ncu`: every kernel seen in the trace is measured on `--ncu-count` launches taken from the middle of the run, one ncu run per kernel

| option | default | meaning |
|---|---|---|
| `-s, --set`, `--only` | `all` | same as run_bench.sh |
| `--ncu` | off | also run ncu, it is slow, use it with `--only` |
| `--ncu-count` | 10 | launches measured per kernel |
| `-t, --timeout` | 3600 | seconds for one program run |
| `-n, --name` | | word added to the file names |
| `--compare OLD NEW` | | before and after table of two `profile_*.tsv` files, nothing is profiled |
| `--gurobi FILE` | | a bench tsv with `-k lp` runs, adds the Gurobi time to the compare table, can be given more than once |

Output in `results/<date>/`:

- `profile_<time>[_name].log` has one line per instance: status, resolved, kern_s, then match, GPU idle percent and the share of kernel time of each step.
- `profile_<time>[_name].tsv` has one row per instance (columns below).
- `profile_<time>[_name]_kernels.tsv` has one row per instance and kernel (columns below).
- `profile_<time>[_name]/` has the program outputs, the `.nsys-rep` and the `.ncu-rep` files. Open them in the Nsight Systems and Nsight Compute programs to look at the timeline or the source lines (the GPU build keeps line info for this).

Instance table columns:

| column | meaning |
|---|---|
| status, resolved, kern_s, maxflow_s, gpu_solve_s | from the plain run |
| prof_resolved, resolved_match | resolved count of the nsys run, `yes` when it is the same as plain (the profiler changed nothing) |
| ccg_vertices, gpu_passes, gpu_bfs_levels, gpu_mb | size and GPU loop counts of the plain run |
| gpu_window_s | first to last GPU activity in the trace |
| gpu_busy_s, gpu_idle_pct | time with a kernel, copy or memset running, and the part of the window the GPU was waiting |
| kernel_s, kernel_launches | sum of all kernel times and the number of launches |
| pct_bfs, pct_push, pct_remove, pct_worklist, pct_other | share of kernel time per step (global relabel BFS, push-relabel, remove-invalid, worklist build, the rest) |
| top_kernel | kernel with the most time |
| htod_ms, dtoh_ms, htod_count, dtoh_count | copies host to device and device to host |
| sync_api_s, sync_calls | CPU time waiting in CUDA synchronize calls |
| memcpy_api_s, launch_api_s | CPU time in cudaMemcpy calls and in kernel launch calls |
| ncu_kernels | kernels measured with ncu |

Kernel table columns: launches, total_ms, avg_us, min_us, max_us, pct_kernel_time and pct_gpu_window from nsys. With `--ncu` also: ncu_launches, ncu_avg_us, grid_blocks, block_threads, regs_per_thread, static_smem_bytes, sectors_per_request, active_threads_per_warp, sectors_per_active_thread, theoretical_occupancy_pct, achieved_occupancy_pct, sm_active_pct, dram_throughput_pct, sm_throughput_pct, l1_hit_pct, l2_hit_pct.

Some notes on the ncu numbers:

- ncu locks the clocks, so `ncu_avg_us` is slower than the nsys time. Compare kernels with each other, not with nsys.
- `sectors_per_request` is 4 for well coalesced 4 byte loads and 8 for 8 byte loads. 32 means every thread of the warp read a different sector.
- `active_threads_per_warp` is 32 when all threads of a warp do work, a low value means idle or diverged threads.

The compare table has, per instance: old_s, new_s, speedup (old / new), the resolved counts and resolved_same, gurobi_s, vs_gurobi_old and vs_gurobi_new (Gurobi time / GPU time, above 1 means the GPU is faster), and the BFS and push+remove shares. The script exits with 1 when a resolved count changed.

nsys 2023.4 on Ubuntu writes only a `.qdstrm` file. The script then turns it into `.nsys-rep` with `QdstrmImporter` by itself.

Examples:

```bash
./scripts/run_profile.sh -n nsys-all                                     # nsys on every instance
./scripts/run_profile.sh --only rus2_100_200_0_1 --ncu -n ncu-rus2        # nsys and ncu on one instance
./scripts/run_profile.sh --only PackupWeighted -n hub-off -- --hub-deg 0  # profile with fix B off
./scripts/run_profile.sh --compare results/<day>/profile_A.tsv results/<day>/profile_B.tsv --gurobi results/<day>/bench_X_lp.tsv
```

## 7. Benchmark data scripts

### stage_artifact.sh

```bash
./scripts/stage_artifact.sh --source /run/media/<you>/<drive> --dry-run
./scripts/stage_artifact.sh --source /run/media/<you>/<drive>
```

| option | default | meaning |
|---|---|---|
| `--source DIR` | needed | where the drive is mounted, the evalgm/ and uai/ folders are searched up to 4 levels below it |
| `--dest DIR` | `~/mtp/wcsp-maxflow/artifact` | the local copy |
| `--dry-run` | off | only writes the manifest, nothing is copied or renamed |

It reads the header of every file and puts it in a class: boolean (copied), domain1 (copied, collapse it later), multi (not copied, no CCG method can use it) or unreadable (not copied). The result goes to `data/artifact/manifest.tsv`. Without `--dry-run` the old copy is renamed to `<dest>.old-<date>`, it is never deleted. The log goes to `results/<date>/`.

### run_collapse_audit.sh and audit_collapse.py

```bash
./scripts/run_collapse_audit.sh domain1                       # all domain-1 instances of the manifest
./scripts/run_collapse_audit.sh --only rand695_l1 --brute-max 16 --ilp domain1
```

For every instance: `wcsp_collapse` writes the collapsed file, then `audit_collapse.py` does the collapse again on its own and checks the header, every constraint, the folded constant, and the cost of many assignments (`cost_original(x) = cost_collapsed(x) + folded_constant`).

| option | default | meaning |
|---|---|---|
| set | `domain1` | `domain1` (from the manifest), a folder, or a file with one path per line |
| `--dest DIR` | `~/mtp/wcsp-maxflow/collapsed` | where the collapsed files go, same folder layout |
| `--artifact DIR` | `~/mtp/wcsp-maxflow/artifact` | the local benchmark copy |
| `--only NAMES` | | comma list of names without extension |
| `--limit N` | | stop after N instances |
| `--samples N` | 200 | random assignments to check |
| `--exhaustive-max N` | 12 | check every assignment when there are fewer variables than this |
| `--budget N` | 2000000 | limit on assignments x constraints |
| `--brute-max K` | 0 | find the true optimum of the original by trying all assignments, when at most K variables are left |
| `--ilp` | off | also solve the collapsed file with the ILP solver |
| `--ilp-max-vars N` | 40 | only for files up to N variables |
| `-t, --timeout` | 300 | seconds for one ILP solve |
| `--resume FILE` | | continue an earlier audit table, instances in it are skipped |
| `-n, --name` | | word added to the file names |

Audit verdicts: `PASS`, `FAIL`, `SKIP_NO_CONSTANTS` (no domain-1 variable), `SKIP_MULTIVALUED` (a domain of 3 or more), `FAIL_COLLAPSE_rcN`, `FAIL_AUDIT_rcN`. The column to watch is `shrunk_cons`: constraints that had both constants and free variables, their cost table had to be cut. `end_to_end` is `MATCH` when the brute force optimum equals the ILP optimum plus the folded constant.

`audit_collapse.py` can also be run alone: `python3 scripts/audit_collapse.py original.wcsp collapsed.wcsp`. Exit code 0 is PASS, 1 is FAIL, 2 means a file could not be read.

### run_census.sh

```bash
./scripts/run_census.sh evalgm
./scripts/run_census.sh ~/mtp/wcsp-maxflow/collapsed
```

Runs `e2e_solve --max-arity -1` on every file. The program reads the file, records the arity and then refuses it with code 6, so nothing heavy is built. The table has instance, family, max_arity and rc (6 = measured, 5 = refused by the loader, 2 = file not readable), the same columns as `data/artifact/arity-*.tsv`. To update the record in git, copy the new table over the old one. Options: `--artifact DIR`, `-n, --name`.

### run_ceiling.sh

```bash
./scripts/run_ceiling.sh uai
./scripts/run_ceiling.sh -t 300 --max-vars 2000 ~/mtp/wcsp-maxflow/collapsed
```

Runs `mincut_lattice` on every file of a set (`uai`, `evalgm` or a folder).

| option | default | meaning |
|---|---|---|
| `-t, --timeout` | 900 | seconds for one instance |
| `--min-vars N`, `--max-vars N` | | only instances with at least N, or less than N, variables |
| `--resume FILE` | | continue an earlier ceiling table |
| `--artifact DIR`, `-n, --name` | | as above |

Table columns: instance, group, n (CCG vertices), W (total weight), integral, allhalf, same_pct (variables no min cut can decide), ceiling_pct (the most any min cut can decide), decided_pct (what our rule decides), verdict and wall_s.

Verdicts:

- `STRUCTURAL`: the ceiling is 0, no min-cut method can decide anything
- `OPTIMAL`: our rule already reaches the ceiling
- `SELECTION`: a different min cut would decide more
- `SKIP_DOMAIN`, `SKIP_ARITY`, `EMPTY`, `TIMEOUT`, `FAIL_rcN`

### gen_hard_suite.sh, gen_wcsp.py and gen_wcsp_hard.py

```bash
./scripts/gen_hard_suite.sh probe      # small instances that sweep the settings
./scripts/gen_hard_suite.sh scale      # bigger instances at the chosen setting
python3 scripts/gen_wcsp.py 100000 150000 1234 out.wcsp              # mixed arity 2 to 4
python3 scripts/gen_wcsp.py 100000 150000 1234 out.wcsp --pairwise   # arity 2 only
python3 scripts/gen_wcsp_hard.py -n 2000 -m 3000 --seed 1 -o out.wcsp --cost-model binary --tightness 0.5
```

`gen_wcsp_hard.py` options: `--arity` (2), `--cost-model binary|narrow|uniform` (binary), `--tightness` (0.5, part of the tuples with the high cost), `--cost-lo` (0), `--cost-hi` (1), `--topology random|scalefree|clustered` (random), `--communities` (50), `--cross-prob` (0.05).

## 8. Gate tests

Run these after any change to the max-flow code. They must all pass before timings mean anything.

| test | command | must show |
|---|---|---|
| hand-made graphs | `./kmf_test` | `Results: 6 / 6 passed` |
| min cut check | `./cut_audit <file.wcsp>` | `=> CLEAN` and `VALID MIN-CUT? : saturated` for the SEEDED rule |
| max-flow against Gurobi | `./compare_kernels <file.wcsp>` | `decided by both, OPPOSITE: 0` |
| CPU against GPU | `./compare_kernels_gpu <file.wcsp>` | `decided by both, OPPOSITE: 0` |

cut_audit prints several ways to pick the source side of the cut. SEEDED (reachable from the source and from vertices with excess) is the rule the kernelizers use. STALE (heights as the solver left them) is the old rule, kept for comparison. For each one `capacity - maxflow` must be 0 and `crossing residual` must be 0 edges. The integrality part checks that every capacity, residual and flow is a whole number, `CLEAN` means none is hiding inside the epsilon.

For compare_kernels, `decided by Gurobi only` above 0 is fine: the LP has more than one optimum and Gurobi can land on one that fixes more variables. For a large file use the split form so only one CCG is in memory:

```bash
./compare_kernels mf data/bench/10_1M.wcsp /tmp/mf.txt
./compare_kernels lp data/bench/10_1M.wcsp /tmp/lp.txt
./compare_kernels cmp /tmp/mf.txt /tmp/lp.txt
```

mincut_lattice is not a pass or fail test, it gives the ceiling of one instance: `./mincut_lattice [--format d|u|auto] [--max-arity N] <file>`.

## 9. The GPU solver in short

The GPU solver is a push-relabel max-flow. Every pass only the active vertices (excess > 0) get threads (worklist version). Every `--gr-period` passes a global relabel (backward BFS from the sink) sets the heights again.

| kernel | step |
|---|---|
| `gpu_initialize_kernel`, `gpu_saturate_source_kernel` | start: reset the network, push all source edges |
| `gpu_bfs_init_kernel`, `gpu_bfs_sink_level_kernel`, `gpu_bfs_step_kernel` | global relabel, level 0 and the plain BFS step |
| `wl_bfs_step_small_kernel`, `wl_bfs_step_hub_kernel`, `wl_bfs_step_block_kernel` | BFS level split by vertex size (fix D) |
| `wl_build_worklist_kernel` | list of active vertices, split into small, hub and big |
| `wl_push_relabel_kernel`, `wl_push_relabel_hub_kernel`, `wl_push_relabel_block_kernel` | push or relabel, one thread, one warp or one block per vertex (fixes B and C) |
| `wl_remove_invalid_edges_kernel`, `wl_remove_invalid_hub_kernel`, `wl_remove_invalid_block_kernel` | fix edges that broke the height rule |

The fixes A to E and their options are in section 4 (GPU options).

## 10. Common tasks

| task | command |
|---|---|
| timing table of all kernelizers | `./scripts/run_bench.sh -k gpu,cpu,lp -r 3` |
| GPU only, real set | `./scripts/run_bench.sh -s real -k gpu` |
| end to end with the exact solve | `./scripts/run_bench.sh -k gpu,lp --solver ilp` |
| effect of one GPU fix | run `run_profile.sh` with and without `-- --<fix option> 0`, then `run_profile.sh --compare` |
| which kernel takes the time | `./scripts/run_profile.sh -n nsys-all`, then look at `_kernels.tsv` |
| coalescing and occupancy of one instance | `./scripts/run_profile.sh --only <name> --ncu` |
| add a new instance | put the file or a link in `instances/real/` or `instances/synthetic/` |
| one instance by hand | `./e2e_solve_gpu --kernelizer gpu --solver none <file>` |

## 11. Problems and fixes

- **`./e2e_solve_gpu not found`**: run `./scripts/build.sh`, and look at its log in `results/<date>/` if a line says FAILED.
- **`this binary was built without -DHAVE_GUROBI`**: `GUROBI_HOME` was not set at build time, or use `--solver none` or `--solver mp`.
- **Gurobi library not found when running**: add `$GUROBI_HOME/lib` to `LD_LIBRARY_PATH`.
- **Out of memory on big instances**: a 1M variable instance needs about 18 GB of RAM. Run one program at a time and close other big programs. cpu and lp runs of run_bench.sh are stopped at `--mem-limit`.
- **Times jump between runs**: back to back runs of big instances push the memory, mostly the CPU flow network build changes. Use `-r 3` for numbers you want to quote.
- **`skip_arity` or `skip_domain`**: the instance cannot be used as it is. Domain-1 instances can be collapsed with run_collapse_audit.sh, high arity ones cannot.
- **`git diff` or `git log` opens a pager**: press `q`, or use `git --no-pager ...`.
