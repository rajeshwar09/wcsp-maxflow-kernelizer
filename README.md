# wcsp-maxflow-kernelizer

A Nemhauser-Trotter kernelizer for Weighted Constraint Satisfaction Problems (WCSPs) that uses max-flow instead of a linear program. It works inside the WCSPLift pipeline, where the original kernelizer solves an LP with Gurobi. Here the same kernel is found with a push-relabel max-flow on the bipartite double cover of the constraint composite graph (CCG), on the CPU and on the GPU.

The max-flow algorithm follows the static part of *Scalable Maxflow Processing for Dynamic Graphs* (Kannappan, Kumar and Nasre, IIT Madras, 2025).

## How it works

A WCSP is turned into a graph, the constraint composite graph, whose minimum weight vertex cover gives the answer of the WCSP. Before the exact solver runs, the kernelizer fixes all variables whose value is already known from the LP relaxation, so the solver only has to search the rest (the kernel).

```
  .wcsp / .uai file
        |
        v
  parse -> polynomial -> CCG -> kernelizer -> remnant -> exact solver -> final optimum
                                    |
                 +------------------+------------------+
                 |                  |                  |
             Gurobi LP         CPU max-flow       GPU max-flow
            (lp, baseline)        (cpu)              (gpu)
```

All three kernelizers give a valid kernel. The max-flow ones never give a variable the opposite value of the LP one. They can decide a different number of variables, because the LP can have more than one optimal solution.

## Requirements

- Linux with g++ (C++17), Boost graph headers, OpenBLAS and Python 3
- NVIDIA GPU with the CUDA toolkit (`nvcc`) for the GPU kernelizer
- Gurobi with `GUROBI_HOME` set, for the LP kernelizer, the ILP solver and `compare_kernels`
- Nsight Systems (`nsys`) and Nsight Compute (`ncu`), only for profiling

The project was developed on Ubuntu 26.04 with CUDA 12.4, g++ 15.2, Gurobi 13.0.2 and an RTX 4050 (sm_89).

```bash
sudo apt install g++ libboost-graph-dev libopenblas-dev python3
```

## Quick start

```bash
# 1. build every program (the GPU architecture is found with nvidia-smi)
./scripts/build.sh

# 2. check the kernelizer on the small hand-made graphs (6 / 6 must pass)
./kmf_test

# 3. put instances (files or links) in the two instance folders
mkdir -p instances/real instances/synthetic
./scripts/gen_benchmark_suite.sh bench small
ln -s "$PWD"/data/bench/0*.wcsp instances/synthetic/

# 4. run the GPU kernelizer on all of them, time and memory go to results/<date>/
./scripts/run_bench.sh -k gpu

# 5. one instance by hand
./e2e_solve_gpu --kernelizer gpu --solver none data/bench/05_50k.wcsp
```

## Main scripts

| script | what it does |
|---|---|
| `scripts/build.sh` | builds all programs, one ok or FAILED line each |
| `scripts/run_bench.sh` | runs the pipeline on a set of instances, keeps every time and memory number |
| `scripts/run_profile.sh` | per kernel GPU times with nsys, detailed kernel numbers with ncu, before and after comparison |
| `scripts/gen_benchmark_suite.sh` | makes the synthetic instances from fixed seeds |
| `scripts/stage_artifact.sh` | makes a clean local copy of the published WCSPLift benchmark |
| `scripts/run_collapse_audit.sh` | removes domain-1 variables from benchmark files and checks the result |
| `scripts/run_census.sh`, `scripts/run_ceiling.sh` | arity census and min-cut ceiling of a benchmark set |

Every script writes its log and tables into `results/<date>/`, with a new set of files for every run. The `results/` folder is not part of git.

## Folders

```
apps/        programs: e2e_solve (the pipeline), wcsp_collapse, mincut_lattice and the gate tests
src/         the max-flow code: common/, cpu/, gpu/ and integration/ (the kernelizers)
scripts/     build, benchmark, profile and data scripts, scripts/lib/ has their python helpers
data/        instance lists, benchmark records and manifests of the synthetic instances
instances/   real/ and synthetic/ folders read by the scripts (local only, not in git)
third_party/ WCSPLift (wcsp-solver): parser, CCG, Gurobi LP kernelizer and solvers, not changed
```

## More details

[GUIDE.md](GUIDE.md) explains every file, how to set up the data, how to run each program and script, every flag, and every column of the output tables.
