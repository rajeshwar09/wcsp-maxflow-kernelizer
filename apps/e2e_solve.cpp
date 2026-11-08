// full pipeline for one instance: parse, build the CCG, kernelize, solve the remnant, final objective
// kernelization runs in rounds until a round decides nothing new, like WCSPLift main.cpp
// every number of the run is printed at the end as "[m] name : value", the scripts read these lines
//
// usage:
//   ./e2e_solve [options] <file.wcsp|file.uai>
//
//   --kernelizer none|cpu|gpu|lp    kernelizer                  (default cpu)
//   --solver ilp|mp|none            solver for the remnant      (default ilp)
//   --format d|u|auto               input format                (default auto, .uai means UAI)
//   --max-rounds N                  kernelization rounds        (default 100)
//   --max-arity N                   skip instances above this   (default 15)
//   --time-limit SEC                solver time limit           (default none)
//   --perturb off|int|real          tie-breaking perturbation   (default off)
//   --spread K                      int mode offsets 1..K       (default 8)
//   --delta D                       real mode, 0 = 1/(2n)       (default 0)
//   --seed S                        perturbation seed           (default 1)
//
//   GPU settings, defaults are the tuned values (fixes A to E):
//   --sink-bfs 0|1                  fix A, parallel BFS level 0           (default 1)
//   --hub-deg N                     fix B, warp per vertex with >= N edges (default 64, 0 = off)
//   --block-deg N                   fix C, block per vertex with >= N edges (default 1024, 0 = off)
//   --bfs-split 0|1                 fix D, split BFS levels by degree     (default 1)
//   --block-threads N               fix D2, threads per block             (default 0 = automatic)
//   --async 0|1                     fix E, one wait per pass              (default 1)
//   --gr-period N                   passes between global relabels        (default 75)
//   --kernel-cycles N               push steps per vertex per pass        (default 0 = |E|/|V|)
//   --gpu-kernel-times              time each step of the GPU loop, slower, only for testing
//
// exit codes: 0 ok, 2 cannot open, 3 missing backend, 4 solver timeout, 5 not Boolean, 6 arity too high or out of memory, 7 failure

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "third_party/wcsp-solver/src/RunningTime.h"
#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/global.h"

// [BUILD] MWVCSolverMessagePassing.h calls isinf() without std::, this makes it compile on new libstdc++
#include <cmath>
using std::isinf;
#include "third_party/wcsp-solver/src/MWVCSolverMessagePassing.h"

#include "src/common/metrics.h"
#include "src/integration/KernelizerMaxflow.h"
#include "src/integration/perturbation.h"

#ifdef USE_GPU
#include "src/integration/KernelizerMaxflowGPU.h"
#endif

#ifdef HAVE_GUROBI
#include "third_party/wcsp-solver/src/KernelizerLinearProgramming.h"
#include "third_party/wcsp-solver/src/LinearProgramSolver.h"
#include "third_party/wcsp-solver/src/LinearProgramSolverGurobi.h"
#include "third_party/wcsp-solver/src/MWVCSolverLinearProgramming.h"
#endif

typedef ConstraintCompositeGraph<> ccg_t;
typedef ccg_t::graph_t graph_t;
typedef ccg_t::variable_id_t vid_t;

using maxflow::metrics;
using maxflow::now_sec;

// [ARITY] largest constraint arity of the instance
template <class Inst>
static size_t max_constraint_arity(const Inst& inst) {
  size_t m = 0;
  for (const auto& c : inst.getConstraints()) {
    m = std::max(m, c.getVariables().size());
  }
  return m;
}

// [ARITY] bytes toPolynomial needs for this arity, capped so it does not overflow
static double polynomial_bytes(size_t arity) {
  if (arity >= 32) {
    return 1e300;
  }
  double side = static_cast<double>(1ULL << arity);
  return side * side * 8.0;
}

// [MEM] peak resident memory of this process in MB, ru_maxrss is in KB on Linux
static double host_peak_mb() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss / 1024.0;
}

// [OUTPUT] status line, all metrics, then the exit code
static int finish(const std::string& status, int rc, double t_all0) {
  metrics::get().set("time.total", now_sec() - t_all0);
  metrics::get().set("mem.host_peak_mb", host_peak_mb());
  std::cout << "[info] status : " << status << "\n";
  metrics::get().print(std::cout);
  return rc;
}

static void usage(const char* prog) {
  std::cerr << "usage: " << prog << " [options] <file.wcsp|file.uai>\n"
            << "  --kernelizer none|cpu|gpu|lp   --solver ilp|mp|none   --format d|u|auto\n"
            << "  --max-rounds N   --max-arity N   --time-limit SEC\n"
            << "  --perturb off|int|real   --spread K   --delta D   --seed S\n"
            << "  gpu: --sink-bfs 0|1 --hub-deg N --block-deg N --bfs-split 0|1 --block-threads N\n"
            << "       --async 0|1 --gr-period N --kernel-cycles N --gpu-kernel-times\n"
            << "  see the comment at the top of apps/e2e_solve.cpp for details\n";
}

static int run(int argc, char** argv) {
  double t_all0 = now_sec();

  // [ARGS] defaults
  std::string kern = "cpu";
  std::string solver = "ilp";
  std::string format = "auto";
  long max_rounds = 100;
  long max_arity = 15;
  double time_limit = -1.0;
  const char* path = nullptr;
  std::string perturb = "off";
  long pspread = 8;
  double pdelta = 0.0;
  std::uint64_t pseed = 1;

  // [ARGS] GPU settings, same defaults as gpu_settings in maxflow_gpu_worklist.h
  int g_sink_bfs = 1;
  int g_hub_deg = 64;
  int g_block_deg = 1024;
  int g_bfs_split = 1;
  int g_block_threads = 0;
  int g_async = 1;
  int g_gr_period = 75;
  int g_kernel_cycles = 0;
  bool g_kernel_times = false;

  // [ARGS] read the command line
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    bool has_val = (i + 1 < argc);
    if (a == "--kernelizer" && has_val) kern = argv[++i];
    else if (a == "--solver" && has_val) solver = argv[++i];
    else if (a == "--format" && has_val) format = argv[++i];
    else if (a == "--max-rounds" && has_val) max_rounds = std::atol(argv[++i]);
    else if (a == "--max-arity" && has_val) max_arity = std::atol(argv[++i]);
    else if (a == "--time-limit" && has_val) time_limit = std::atof(argv[++i]);
    else if (a == "--perturb" && has_val) perturb = argv[++i];
    else if (a == "--spread" && has_val) pspread = std::atol(argv[++i]);
    else if (a == "--delta" && has_val) pdelta = std::atof(argv[++i]);
    else if (a == "--seed" && has_val) pseed = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--sink-bfs" && has_val) g_sink_bfs = std::atoi(argv[++i]);
    else if (a == "--hub-deg" && has_val) g_hub_deg = std::atoi(argv[++i]);
    else if (a == "--block-deg" && has_val) g_block_deg = std::atoi(argv[++i]);
    else if (a == "--bfs-split" && has_val) g_bfs_split = std::atoi(argv[++i]);
    else if (a == "--block-threads" && has_val) g_block_threads = std::atoi(argv[++i]);
    else if (a == "--async" && has_val) g_async = std::atoi(argv[++i]);
    else if (a == "--gr-period" && has_val) g_gr_period = std::atoi(argv[++i]);
    else if (a == "--kernel-cycles" && has_val) g_kernel_cycles = std::atoi(argv[++i]);
    else if (a == "--gpu-kernel-times") g_kernel_times = true;
    else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      return 0;
    } else if (a[0] == '-') {
      std::cerr << "unknown option: " << a << "\n";
      usage(argv[0]);
      return 1;
    } else {
      path = argv[i];
    }
  }

  // [ARGS] checks
  if (path == nullptr) {
    usage(argv[0]);
    return 1;
  }
  if (kern != "none" && kern != "cpu" && kern != "gpu" && kern != "lp") {
    std::cerr << "bad --kernelizer: " << kern << "\n";
    return 1;
  }
  if (solver != "ilp" && solver != "mp" && solver != "none") {
    std::cerr << "bad --solver: " << solver << "\n";
    return 1;
  }
#ifndef USE_GPU
  if (kern == "gpu") {
    std::cerr << "this binary was built without -DUSE_GPU\n";
    return 3;
  }
#endif
#ifndef HAVE_GUROBI
  if (kern == "lp" || solver == "ilp") {
    std::cerr << "this binary was built without -DHAVE_GUROBI\n";
    return 3;
  }
#endif

  // [ARGS] input format from the option or the file extension
  WCSPInstance<>::Format fformat = WCSPInstance<>::Format::DIMACS;
  {
    std::string f = format;
    if (f == "auto") {
      std::string p(path);
      f = (p.size() >= 4 && p.compare(p.size() - 4, 4, ".uai") == 0) ? "u" : "d";
    }
    if (f == "u" || f == "uai") {
      fformat = WCSPInstance<>::Format::UAI;
    } else if (f != "d" && f != "dimacs" && f != "wcsp") {
      std::cerr << "bad --format: " << format << "\n";
      return 1;
    }
  }

  // [ARGS] perturbation, the seed changes every round so each round gets a fresh draw
  maxflow::perturb_config pcfg_base;
  if (!maxflow::parse_perturb_mode(perturb, pcfg_base.mode)) {
    std::cerr << "bad --perturb: " << perturb << "\n";
    return 1;
  }
  if (pspread < 1) {
    std::cerr << "bad --spread: must be >= 1\n";
    return 1;
  }
  pcfg_base.spread = pspread;
  pcfg_base.delta = pdelta;
  pcfg_base.seed = pseed;

#ifdef USE_GPU
  // [ARGS] pass the GPU settings to the solver
  maxflow::gpu_settings& gs = maxflow::gpu_config();
  gs.sink_bfs = (g_sink_bfs != 0);
  gs.hub_deg = g_hub_deg;
  gs.block_deg = g_block_deg;
  gs.bfs_split = (g_bfs_split != 0);
  gs.block_threads = g_block_threads;
  gs.async = (g_async != 0);
  gs.gr_period = g_gr_period;
  gs.kernel_cycles = g_kernel_cycles;
  gs.kernel_times = g_kernel_times;
#endif

  // [TIMEOUT] message passing never stops on its own, so give it a limit
  RunningTime::GetInstance().setStartingTime(std::chrono::high_resolution_clock::now());
  if (solver == "mp" && time_limit <= 0) {
    time_limit = 300.0;
  }
  if (time_limit > 0) {
    RunningTime::GetInstance().setTimeLimit(std::chrono::duration<double>(time_limit));
  }

  // [OUTPUT] run settings
  std::cout << std::setprecision(15);
  std::cout << "[info] instance : " << path << "\n";
  std::cout << "[info] format : " << (fformat == WCSPInstance<>::Format::UAI ? "uai" : "dimacs") << "\n";
  std::cout << "[info] kernelizer : " << kern << "\n";
  std::cout << "[info] solver : " << solver << "\n";
  std::cout << "[info] max_rounds : " << max_rounds << "\n";
  std::cout << "[info] perturb : " << maxflow::perturb_mode_name(pcfg_base.mode) << " spread=" << pspread << " delta=" << pdelta << " seed=" << pseed << "\n";
  if (kern == "gpu") {
    std::cout << "[info] gpu : sink_bfs=" << g_sink_bfs << " hub_deg=" << g_hub_deg << " block_deg=" << g_block_deg
              << " bfs_split=" << g_bfs_split << " block_threads=" << g_block_threads << " async=" << g_async
              << " gr_period=" << g_gr_period << " kernel_cycles=" << g_kernel_cycles << " kernel_times=" << g_kernel_times << "\n";
  }
  if (pcfg_base.mode != maxflow::perturb_mode::off && (kern == "lp" || kern == "none")) {
    std::cout << "[info] note : --perturb has no effect with --kernelizer " << kern << "\n";
  }

  // [PARSE] read the instance, non-Boolean instances are skipped with code 5
  std::ifstream in(path);
  if (!in) {
    std::cerr << "cannot open " << path << "\n";
    return 2;
  }
  double t = now_sec();
  std::unique_ptr<WCSPInstance<>> instp;
  try {
    instp.reset(new WCSPInstance<>(in, fformat));
  } catch (const std::domain_error& e) {
    std::cout << "[info] skip : " << e.what() << "\n";
    return finish("skip_domain", 5, t_all0);
  }
  WCSPInstance<>& instance = *instp;
  t = maxflow::mark("time.parse", t);

  // [ARITY] checked before toPolynomial, which allocates 2^(2*arity) doubles
  size_t arity = max_constraint_arity(instance);
  metrics::get().set("graph.max_arity", arity);
  metrics::get().set("graph.ccg_need_gib", polynomial_bytes(arity) / 1073741824.0);
  if (static_cast<long>(arity) > max_arity) {
    std::cout << "[info] skip : constraint arity " << arity << " exceeds --max-arity " << max_arity << "\n";
    return finish("skip_arity", 6, t_all0);
  }

  // [CCG] build the constraint composite graph
  ccg_t ccg;
  WCSPInstance<>::constraint_t::Polynomial p;
  for (const auto& c : instance.getConstraints()) {
    c.toPolynomial(p);
  }
  t = maxflow::mark("time.to_polynomial", t);
  ccg_t::weight_t s = ccg.addPolynomial(p);
  t = maxflow::mark("time.add_polynomial", t);

  // [CCG] simplify() decides the trivially forced variables, same for every kernelizer
  std::map<vid_t, bool> assignments;
  ccg.simplify(assignments);
  t = maxflow::mark("time.simplify", t);
  const size_t simplified = assignments.size();
  graph_t g = *ccg.getGraph();
  t = maxflow::mark("time.graph_copy", t);

  // [CCG] size of the graph before kernelization
  const size_t total_vars = ccg.getNumberOfVariables();
  auto stats = ccg.getStatistics();
  metrics::get().set("graph.remnant_s", s);
  metrics::get().set("graph.total_vars", total_vars);
  metrics::get().set("graph.real_vars", stats[0]);
  metrics::get().set("graph.type1_aux", stats[1]);
  metrics::get().set("graph.type2_aux", stats[2]);
  metrics::get().set("graph.vertices_before", boost::num_vertices(g));
  metrics::get().set("graph.edges_before", boost::num_edges(g));
  metrics::get().set("kern.simplified", simplified);

  // [KERNEL] repeat rounds until a round adds nothing or everything is decided
  double kern_time = 0.0;
  long rounds = 0;
  bool kern_aborted = false;
  std::string kern_status = "ok";
  if (kern != "none") {
    size_t prev = static_cast<size_t>(-1);
    for (long i = 1; i <= max_rounds && prev != assignments.size(); i++) {
      prev = assignments.size();
      double k0 = now_sec();

      // [KERNEL] fresh perturbation seed for every round
      maxflow::perturb_config pc = pcfg_base;
      pc.seed = pcfg_base.seed + static_cast<std::uint64_t>(i);
      pc.verbose = (i == 1);

      try {
        if (kern == "cpu") {
          maxflow::KernelizerMaxflow<> k(pc);
          k.kernelize(g, assignments);
        }
#ifdef USE_GPU
        else if (kern == "gpu") {
          maxflow::KernelizerMaxflowGPU<> k(pc);
          k.kernelize(g, assignments);
        }
#endif
#ifdef HAVE_GUROBI
        else if (kern == "lp") {
          KernelizerLinearProgramming<> k(new LinearProgramSolverGurobi());
          k.kernelize(g, assignments);
        }
#endif
      }
#ifdef HAVE_GUROBI
      catch (const LinearProgramSolver::TimeOutException&) {
        kern_aborted = true;
        kern_status = "kernel_timeout";
        std::cout << "[info] kernel_timeout : kernelizer exceeded the time limit\n";
      }
#endif
      catch (const std::exception& e) {
        kern_aborted = true;
        kern_status = "kernel_error";
        std::cout << "[info] kernel_error : " << e.what() << "\n";
      }

      double dt = now_sec() - k0;
      kern_time += dt;
      rounds = i;
      if (kern_aborted) {
        break;
      }

      // [KERNEL] one line per round
      std::cout << "[round] " << i << " resolved=" << assignments.size() << " new=" << (assignments.size() - prev)
                << " remaining=" << (total_vars - assignments.size()) << " vertices=" << boost::num_vertices(g)
                << " edges=" << boost::num_edges(g) << " time=" << dt << "\n";

      if (assignments.size() >= total_vars) {
        break;
      }
    }
  }

  // [KERNEL] results of kernelization
  metrics::get().set("kern.rounds", rounds);
  metrics::get().set("kern.resolved_total", assignments.size());
  metrics::get().set("kern.by_kernelizer", assignments.size() - simplified);
  metrics::get().set("kern.var_reduction_pct", total_vars > 0 ? 100.0 * assignments.size() / total_vars : 0.0);
  metrics::get().set("time.kernelize", kern_time);
  metrics::get().set("graph.vertices_after", boost::num_vertices(g));
  metrics::get().set("graph.edges_after", boost::num_edges(g));
  size_t before_v = stats[0] + stats[1] + stats[2];
  double vred = before_v > 0 ? 100.0 * (1.0 - static_cast<double>(boost::num_vertices(g)) / before_v) : 0.0;
  metrics::get().set("kern.vertex_reduction_pct", vred);

  // [SOLVE] exact ILP or message passing on what is left
  bool solved = false;
  bool timed_out = false;
  if (solver != "none") {
    if (assignments.size() >= total_vars) {
      solved = true;
    } else {
      double s0 = now_sec();
      double mwvc_weight = 0.0;
      // [SOLVE] WCSPLift solvers throw on timeout, catch it so the run still prints its numbers
      try {
        if (solver == "mp") {
          MWVCSolverMessagePassing<> ms(1e-6);
          mwvc_weight = ms.solve(g, assignments);
          solved = true;
        }
#ifdef HAVE_GUROBI
        else if (solver == "ilp") {
          MWVCSolverLinearProgramming<> ms(new LinearProgramSolverGurobi());
          mwvc_weight = ms.solve(g, assignments);
          solved = true;
        }
#endif
      }
#ifdef HAVE_GUROBI
      catch (const LinearProgramSolver::TimeOutException&) {
        timed_out = true;
      }
#endif
      catch (const std::exception& e) {
        std::cout << "[info] solve_error : " << e.what() << "\n";
      }
      catch (...) {
        timed_out = true;
      }
      metrics::get().set("time.solve", now_sec() - s0);
      if (solved) {
        metrics::get().set("result.mwvc_weight", mwvc_weight);
      }
    }
  }

  // [RESULT] final objective of the original problem
  if (solved) {
    double opt = instance.computeTotalWeight(assignments);
    metrics::get().set("result.final_optimum", opt);
    std::cout << "[info] final_optimum : " << opt << "\n";
  } else {
    std::cout << "[info] final_optimum : " << (timed_out ? "time_limit" : "not_solved") << "\n";
  }

  // [RESULT] status for the scripts
  std::string status = "ok";
  if (kern_aborted) {
    status = kern_status;
  } else if (timed_out) {
    status = "solver_timeout";
  } else if (RunningTime::GetInstance().isTimeOut()) {
    status = "solve_limit_hit";
  }
  return finish(status, timed_out ? 4 : 0, t_all0);
}

// [MAIN] last safety net, a failure still prints a status line and an exit code
int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::bad_alloc&) {
    std::cout << "[info] status : oom\n";
    return 6;
  } catch (const std::domain_error& e) {
    std::cout << "[info] skip : " << e.what() << "\n";
    std::cout << "[info] status : skip_domain\n";
    return 5;
  } catch (const std::exception& e) {
    std::cout << "[info] error : " << e.what() << "\n";
    std::cout << "[info] status : error\n";
    return 7;
  } catch (...) {
    std::cout << "[info] status : error\n";
    return 7;
  }
}
