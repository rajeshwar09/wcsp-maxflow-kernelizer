// End-to-end WCSP pipeline: parse -> CCG -> KERNELIZE -> SOLVE -> objective.
//
// Every earlier harness in this project stopped after kernelization. This one runs the whole way through and reports the FINAL WCSP OBJECTIVE, so the central claim can be tested directly:
//
//     kernelizing first must not change the answer, and should make the remaining solve cheaper
//
// Because the MWVC solver used here (`ilp`) is exact -- WCSPLift's MWVCSolverLinearProgramming adds BINARY variables, so Gurobi solves an
// integer program, not a relaxation -- every kernelizer choice must produce the SAME final objective. Any difference is a defect, not a tie-break.
//
// Kernelization is run to a fixed point, exactly as WCSPLift's own main.cpp does: removing vertices can expose more that are forced, so it repeats until no further variable is resolved
//
// Build (CPU + Gurobi):
//   g++  -std=c++17 -O2 -DHAVE_GUROBI -I. -I$GUROBI_HOME/include \
//        apps/e2e_solve.cpp \
//        third_party/wcsp-solver/src/LinearProgramSolver.cpp \
//        third_party/wcsp-solver/src/LinearProgramSolverGurobi.cpp \
//        -o e2e_solve -L$GUROBI_HOME/lib -lgurobi_c++ -lgurobi130 -lopenblas
//
// Build (adds the GPU kernelizer):
//   nvcc -x cu -std=c++17 -O2 -arch=sm_89 -DUSE_GPU -DHAVE_GUROBI -I. \
//        -I$GUROBI_HOME/include apps/e2e_solve.cpp ... (same sources/libs)
//
// Usage:
//   ./e2e_solve [options] <file.wcsp|file.uai>
//     --kernelizer none|cpu|gpu|lp     which kernelizer to run   (default cpu)
//     --solver     ilp|mp|none         how to solve the remnant  (default ilp)
//     --format     d|u|auto            input format              (default auto)
//     --max-rounds N                   cap kernelization rounds  (default 100)
//     --time-limit SEC                 solver time limit         (default none)
//     --perturb off|int|real           weight tie-breaking       (default off)
//     --spread K                       int mode: offsets 1..K    (default 8)
//     --delta D                        real mode: 0 = auto 1/2n  (default auto)
//     --seed S                         perturbation seed         (default 1)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <new>

#include "third_party/wcsp-solver/src/global.h"
#include "third_party/wcsp-solver/src/RunningTime.h"
#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"

//  MWVCSolverMessagePassing.h calls isinf() unqualified, which does not resolve on newer libstdc++. Pulling it into the global namespace here fixes the build without modifying the bundled solver
#include <cmath>
using std::isinf;
#include "third_party/wcsp-solver/src/MWVCSolverMessagePassing.h"

#include "src/integration/perturbation.h"
#include "src/integration/KernelizerMaxflow.h"

#ifdef USE_GPU
#include "src/integration/KernelizerMaxflowGPU.h"
#endif

#ifdef HAVE_GUROBI
#include "third_party/wcsp-solver/src/LinearProgramSolver.h"
#include "third_party/wcsp-solver/src/LinearProgramSolverGurobi.h"
#include "third_party/wcsp-solver/src/MWVCSolverLinearProgramming.h"
#include "third_party/wcsp-solver/src/KernelizerLinearProgramming.h"
#endif

typedef ConstraintCompositeGraph<> ccg_t;
typedef ccg_t::graph_t graph_t;
typedef ccg_t::variable_id_t vid_t;

using clk = std::chrono::high_resolution_clock;
static double secs(clk::time_point a, clk::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

static void usage(const char* prog) {
  std::cerr
    << "Usage: " << prog << " [options] <file.wcsp|file.uai>\n"
    << "  --kernelizer none|cpu|gpu|lp   kernelizer to use        (default: cpu)\n"
    << "  --solver     ilp|mp|none       remnant solver           (default: ilp)\n"
    << "  --format     d|u|auto          input format             (default: auto)\n"
    << "  --max-rounds N                 max kernelization rounds (default: 100)\n"
    << "  --time-limit SEC               solver time limit        (default: none)\n"
    << "  --perturb off|int|real         weight tie-breaking      (default: off)\n"
    << "  --spread K                     int mode: offsets 1..K   (default: 8)\n"
    << "  --delta D                      real mode: 0 = auto 1/2n (default: auto)\n"
    << "  --seed S                       perturbation seed        (default: 1)\n"
    << "  -h, --help                     this message\n\n"
    << "  none : skip kernelization entirely -- the baseline the others must match\n"
    << "  cpu  : max-flow kernelizer, CPU push-relabel\n"
    << "  gpu  : max-flow kernelizer, CUDA push-relabel (needs -DUSE_GPU build)\n"
    << "  lp   : Gurobi LP-relaxation kernelizer (needs -DHAVE_GUROBI build)\n\n"
    << "  ilp  : exact integer program via Gurobi -- objectives are comparable\n"
    << "  mp   : min-sum message passing, a heuristic that may not converge\n"
    << "  none : stop after kernelization, report no objective\n\n"
    << "  auto : format from the extension (.uai -> UAI, anything else -> DIMACS)\n\n"
    << "  Perturbation breaks weight ties so the LP optimum becomes unique. It is applied\n"
    << "  only inside the flow network the kernelizer builds; the CCG keeps its original\n"
    << "  weights, so the reported objective is always that of the real problem.\n"
    << "  A fresh perturbation is drawn each round -- that is what makes iterating useful.\n"
    << "  It does not run on non-integral weights (UAI).\n";
}

int main(int argc, char** argv) {
  std::string kern = "cpu";
  std::string solver = "ilp";
  std::string format = "auto";
  long max_rounds = 100;
  double time_limit = -1.0;
  const char* path = nullptr;

  std::string perturb = "off";
  long   pspread = 8;
  double pdelta = 0.0;
  std::uint64_t pseed = 1;

  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--kernelizer") && i + 1 < argc)      kern = argv[++i];
    else if (!std::strcmp(argv[i], "--solver") && i + 1 < argc)     solver = argv[++i];
    else if (!std::strcmp(argv[i], "--format") && i + 1 < argc)     format = argv[++i];
    else if (!std::strcmp(argv[i], "--max-rounds") && i + 1 < argc) max_rounds = std::atol(argv[++i]);
    else if (!std::strcmp(argv[i], "--time-limit") && i + 1 < argc) time_limit = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--perturb") && i + 1 < argc)    perturb = argv[++i];
    else if (!std::strcmp(argv[i], "--spread") && i + 1 < argc)     pspread = std::atol(argv[++i]);
    else if (!std::strcmp(argv[i], "--delta") && i + 1 < argc)      pdelta = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)       pseed = std::strtoull(argv[++i], nullptr, 10);
    else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
    else if (argv[i][0] == '-') { std::cerr << "unknown option: " << argv[i] << "\n"; usage(argv[0]); return 1; }
    else path = argv[i];
  }
  if (path == nullptr) { usage(argv[0]); return 1; }

  if (kern != "none" && kern != "cpu" && kern != "gpu" && kern != "lp") {
    std::cerr << "bad --kernelizer: " << kern << "\n"; return 1;
  }
  if (solver != "ilp" && solver != "mp" && solver != "none") {
    std::cerr << "bad --solver: " << solver << "\n"; return 1;
  }
#ifndef USE_GPU
  if (kern == "gpu") { std::cerr << "this binary was built without -DUSE_GPU\n"; return 3; }
#endif
#ifndef HAVE_GUROBI
  if (kern == "lp")     { std::cerr << "this binary was built without -DHAVE_GUROBI\n"; return 3; }
  if (solver == "ilp")  { std::cerr << "this binary was built without -DHAVE_GUROBI\n"; return 3; }
#endif

  //  Input format. `auto` decides from the extension: .uai -> UAI, else DIMACS. The benchmark artifact mixes both, so batch runs depend on this
  WCSPInstance<>::Format fformat = WCSPInstance<>::Format::DIMACS;
  {
    std::string f = format;
    if (f == "auto") {
      std::string p(path);
      f = (p.size() >= 4 && p.compare(p.size() - 4, 4, ".uai") == 0) ? "u" : "d";
    }
    if (f == "u" || f == "uai")                        fformat = WCSPInstance<>::Format::UAI;
    else if (f == "d" || f == "dimacs" || f == "wcsp") fformat = WCSPInstance<>::Format::DIMACS;
    else { std::cerr << "bad --format: " << format << " (expected d, u or auto)\n"; return 1; }
  }

  //  Perturbation settings. The base seed is advanced by the round number inside the loop, so each round draws a different perturbation and therefore lands on a
  //  different vertex of the LP polytope. With a fixed seed the whole run is reproducible
  maxflow::perturb_config pcfg_base;
  if (!maxflow::parse_perturb_mode(perturb, pcfg_base.mode)) {
    std::cerr << "bad --perturb: " << perturb << " (expected off, int or real)\n"; return 1;
  }
  if (pspread < 1) { std::cerr << "bad --spread: must be >= 1\n"; return 1; }
  pcfg_base.spread = pspread;
  pcfg_base.delta  = pdelta;
  pcfg_base.seed   = pseed;

  if (pcfg_base.mode != maxflow::perturb_mode::off && (kern == "lp" || kern == "none")) {
    std::cerr << "note: --perturb has no effect with --kernelizer " << kern << "\n";
  }

  RunningTime::GetInstance().setStartingTime(clk::now());

  //  Message passing has no convergence guarantee and its loop only exits on a timeout. Measured on these CCGs it does not converge at all -- over 200,000
  //  iterations on a 10-variable instance -- so without a limit it runs forever
  if (solver == "mp" && time_limit <= 0) {
    time_limit = 300.0;
    std::cout << "[e2e] note              : --solver mp has no convergence guarantee; "
                 "defaulting --time-limit to 300 s\n";
  }
  if (time_limit > 0)
    RunningTime::GetInstance().setTimeLimit(std::chrono::duration<double>(time_limit));

  std::cout << std::setprecision(std::numeric_limits<double>::digits10 + 1);
  std::cout << "[e2e] instance          : " << path << "\n";
  std::cout << "[e2e] format            : " << (fformat == WCSPInstance<>::Format::UAI ? "uai" : "dimacs") << "\n";
  std::cout << "[e2e] kernelizer        : " << kern << "\n";
  std::cout << "[e2e] solver            : " << solver << "\n";
  std::cout << "[e2e] max rounds        : " << max_rounds << "\n";
  std::cout << "[e2e] perturb           : " << maxflow::perturb_mode_name(pcfg_base.mode);
  if (pcfg_base.mode == maxflow::perturb_mode::integer)
    std::cout << "  spread=" << pspread << "  seed=" << pseed;
  else if (pcfg_base.mode == maxflow::perturb_mode::real)
    std::cout << "  delta=" << (pdelta > 0 ? std::to_string(pdelta) : std::string("auto"))
              << "  seed=" << pseed;
  std::cout << "\n";

  auto t_all0 = clk::now();

  // ---- parse -------------------------------------------------------------
  std::ifstream in(path);
  if (!in) { std::cerr << "cannot open " << path << "\n"; return 2; }
  auto t0 = clk::now();
  //  rc 5 = not Boolean (or a variable pinned to one value)
  //  rc 6 = arity too high to represent -- surfaces as bad_array_new_length, which
  //         derives from bad_alloc, when 2^arity overflows an array length
  std::unique_ptr<WCSPInstance<>> instp;
  try {
    instp.reset(new WCSPInstance<>(in, fformat));
  } catch (const std::domain_error& e) {
    std::cout << "[e2e] SKIP              : " << e.what() << "\n";
    return 5;
  } catch (const std::bad_alloc&) {
    std::cout << "[e2e] SKIP              : cost table too large to represent - high arity\n";
    return 6;
  } catch (const std::length_error&) {
    std::cout << "[e2e] SKIP              : cost table exceeds the container limit - high arity\n";
    return 6;
  } catch (const std::exception& e) {
    std::cout << "[e2e] SKIP              : parse failed: " << e.what() << "\n";
    return 6;
  }
  WCSPInstance<>& instance = *instp;
  auto t1 = clk::now();
  std::cout << "[stage] parse           : " << secs(t0, t1) << " s\n";

  // ---- build the constraint composite graph ------------------------------
  //  toPolynomial expands an arity-k constraint into up to 2^k terms. When arity reaches 580, allocation fails. It will catch it rather than aborting
  ccg_t ccg;
  ccg_t::weight_t s = 0;
  clk::time_point t2, t3;
  try {
    WCSPInstance<>::constraint_t::Polynomial p;
    for (const auto& c : instance.getConstraints()) c.toPolynomial(p);
    t2 = clk::now();
    std::cout << "[stage] toPolynomial    : " << secs(t1, t2) << " s\n";

    s = ccg.addPolynomial(p);
    t3 = clk::now();
    std::cout << "[stage] addPolynomial   : " << secs(t2, t3) << " s\n";
  } catch (const std::bad_alloc&) {
    std::cout << "[e2e] SKIP              : out of memory building the CCG (constraint arity too high)\n";
    return 6;
  } catch (const std::exception& e) {
    std::cout << "[e2e] SKIP              : CCG construction failed: " << e.what() << "\n";
    return 6;
  }

  //  simplify() resolves trivially-forced variables before any kernelizer runs, so it is common to every configuration and not attributed to the kernelizer
  std::map<vid_t, bool> assignments;
  ccg.simplify(assignments);
  auto t4 = clk::now();
  const size_t simplified = assignments.size();
  std::cout << "[stage] simplify        : " << secs(t3, t4) << " s\n";

  graph_t g = *ccg.getGraph();
  auto t5 = clk::now();
  std::cout << "[stage] getGraph copy   : " << secs(t4, t5) << " s\n";

  const size_t total_vars = ccg.getNumberOfVariables();
  auto stats = ccg.getStatistics();
  std::cout << "[graph] remnant s       : " << s << "\n";
  std::cout << "[graph] total variables : " << total_vars << "\n";
  std::cout << "[graph] real vars       : " << stats[0] << "\n";
  std::cout << "[graph] type1 aux       : " << stats[1] << "\n";
  std::cout << "[graph] type2 aux       : " << stats[2] << "\n";
  std::cout << "[graph] vertices before : " << boost::num_vertices(g) << "\n";
  std::cout << "[graph] edges before    : " << boost::num_edges(g) << "\n";
  std::cout << "[stage] simplified out  : " << simplified << "\n";

  // ---- kernelize to a fixed point ---------------------------------------
  //  Removing vertices can expose further forced variables, so the kernelizer is re-run until a round resolves nothing new. This mirrors WCSPLift's main.cpp;
  //  earlier harnesses in this project ran a single pass and so under-reported what kernelization achieves
  double kern_time = 0.0;
  long rounds = 0;
  if (kern != "none") {
    size_t prev = static_cast<size_t>(-1);
    for (long i = 1; i <= max_rounds && prev != assignments.size(); i++) {
      prev = assignments.size();
      auto k0 = clk::now();

      //  A fresh perturbation each round. This is the point of iterating: with unchanged weights round 2 sees an identical graph and the fixed point is immediate, which
      //  is exactly what happens on the d_m* family with --perturb off
      maxflow::perturb_config pc = pcfg_base;
      pc.seed    = pcfg_base.seed + static_cast<std::uint64_t>(i);
      pc.verbose = (i == 1);

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

      auto k1 = clk::now();
      double dt = secs(k0, k1);
      kern_time += dt;
      rounds = i;

      std::cout << "[kernel] round " << i
                << " : resolved=" << assignments.size()
                << "  new=" << (assignments.size() - prev)
                << "  remaining=" << (total_vars - assignments.size())
                << "  vertices=" << boost::num_vertices(g)
                << "  edges=" << boost::num_edges(g)
                << "  time=" << dt << " s\n";

      if (assignments.size() >= total_vars) break;
    }
  }

  std::cout << "[kernel] rounds         : " << rounds << "\n";
  std::cout << "[kernel] resolved total : " << assignments.size() << "\n";
  std::cout << "[kernel] by kernelizer  : " << (assignments.size() - simplified) << "\n";
  std::cout << "[kernel] time           : " << kern_time << " s\n";
  std::cout << "[graph] vertices after  : " << boost::num_vertices(g) << "\n";
  std::cout << "[graph] edges after     : " << boost::num_edges(g) << "\n";

  //  The headline reduction: how much smaller is the problem the solver sees.
  double vred = 0.0;
  {
    size_t before_v = stats[0] + stats[1] + stats[2];
    size_t after_v = boost::num_vertices(g);
    if (before_v > 0) vred = 100.0 * (1.0 - static_cast<double>(after_v) / static_cast<double>(before_v));
  }
  std::cout << "[kernel] vertex reduction: " << vred << " %\n";

  // ---- solve the remnant -------------------------------------------------
  double solve_time = 0.0;
  double mwvc_weight = 0.0;
  bool solved = false;
  bool timed_out = false;

  if (solver != "none") {
    if (assignments.size() >= total_vars) {
      std::cout << "[solve] skipped         : kernelization resolved every variable\n";
      solved = true;
    } else {
      auto s0 = clk::now();
      //  WCSPLift's solvers THROW on timeout rather than returning. Left uncaught this reaches terminate() and the process dies with a core dump and no objective
      //  line at all, which is indistinguishable from a crash. Catch it and report.
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
        std::cout << "[solve] TIMEOUT         : solver exceeded the time limit\n";
      }
#endif
      catch (const std::exception& e) {
        std::cout << "[solve] ERROR           : " << e.what() << "\n";
      }
      catch (...) {
        //  MWVCSolverMessagePassing also has a timeout exception type that does not derive from std::exception in every build. Never let it reach terminate().
        timed_out = true;
        std::cout << "[solve] TIMEOUT         : solver aborted (unrecognised exception)\n";
      }
      auto s1 = clk::now();
      solve_time = secs(s0, s1);
      std::cout << "[solve] time            : " << solve_time << " s\n";
      if (solved) std::cout << "[solve] mwvc weight     : " << mwvc_weight << "\n";
    }
  }

  // ---- final objective ---------------------------------------------------
  auto t_all1 = clk::now();
  std::cout << "[e2e] assignments       : " << assignments.size() << "\n";
  if (solved) {
    auto opt = instance.computeTotalWeight(assignments);
    std::cout << "[e2e] FINAL OPTIMUM     : " << opt << "\n";
  } else if (timed_out) {
    std::cout << "[e2e] FINAL OPTIMUM     : <time limit reached>\n";
  } else {
    std::cout << "[e2e] FINAL OPTIMUM     : <not solved>\n";
  }
  if (RunningTime::GetInstance().isTimeOut())
    std::cout << "[e2e] WARNING           : time limit reached, result may be suboptimal\n";
  std::cout << "[e2e] kernel time       : " << kern_time << " s\n";
  std::cout << "[e2e] solve time        : " << solve_time << " s\n";
  std::cout << "[e2e] TOTAL TIME        : " << secs(t_all0, t_all1) << " s\n";
  return timed_out ? 4 : 0;
}