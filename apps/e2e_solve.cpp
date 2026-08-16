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
//   ./e2e_solve [options] <file.wcsp>
//     --kernelizer none|cpu|gpu|lp     which kernelizer to run   (default cpu)
//     --solver     ilp|mp|none         how to solve the remnant  (default ilp)
//     --max-rounds N                   cap kernelization rounds  (default 100)
//     --time-limit SEC                 solver time limit         (default none)

#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <string>

#include "third_party/wcsp-solver/src/global.h"
#include "third_party/wcsp-solver/src/RunningTime.h"
#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"

//  MWVCSolverMessagePassing.h calls isinf() unqualified, which does not resolve on newer libstdc++. Pulling it into the global namespace here fixes the build without modifying the bundled solver
#include <cmath>
using std::isinf;
#include "third_party/wcsp-solver/src/MWVCSolverMessagePassing.h"

#include "src/integration/KernelizerMaxflow.h"

#ifdef USE_GPU
#include "src/integration/KernelizerMaxflowGPU.h"
#endif

#ifdef HAVE_GUROBI
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
    << "Usage: " << prog << " [options] <file.wcsp>\n"
    << "  --kernelizer none|cpu|gpu|lp   kernelizer to use        (default: cpu)\n"
    << "  --solver     ilp|mp|none       remnant solver           (default: ilp)\n"
    << "  --max-rounds N                 max kernelization rounds (default: 100)\n"
    << "  --time-limit SEC               solver time limit        (default: none)\n"
    << "  -h, --help                     this message\n\n"
    << "  none : skip kernelization entirely -- the baseline the others must match\n"
    << "  cpu  : max-flow kernelizer, CPU push-relabel\n"
    << "  gpu  : max-flow kernelizer, CUDA push-relabel (needs -DUSE_GPU build)\n"
    << "  lp   : Gurobi LP-relaxation kernelizer (needs -DHAVE_GUROBI build)\n\n"
    << "  ilp  : exact integer program via Gurobi -- objectives are comparable\n"
    << "  mp   : min-sum message passing, a heuristic that may not converge\n"
    << "  none : stop after kernelization, report no objective\n";
}

int main(int argc, char** argv) {
  std::string kern = "cpu";
  std::string solver = "ilp";
  long max_rounds = 100;
  double time_limit = -1.0;
  const char* path = nullptr;

  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--kernelizer") && i + 1 < argc)      kern = argv[++i];
    else if (!std::strcmp(argv[i], "--solver") && i + 1 < argc)     solver = argv[++i];
    else if (!std::strcmp(argv[i], "--max-rounds") && i + 1 < argc) max_rounds = std::atol(argv[++i]);
    else if (!std::strcmp(argv[i], "--time-limit") && i + 1 < argc) time_limit = std::atof(argv[++i]);
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
  std::cout << "[e2e] kernelizer        : " << kern << "\n";
  std::cout << "[e2e] solver            : " << solver << "\n";
  std::cout << "[e2e] max rounds        : " << max_rounds << "\n";

  auto t_all0 = clk::now();

  // ---- parse -------------------------------------------------------------
  std::ifstream in(path);
  if (!in) { std::cerr << "cannot open " << path << "\n"; return 2; }
  auto t0 = clk::now();
  WCSPInstance<> instance(in, WCSPInstance<>::Format::DIMACS);
  auto t1 = clk::now();
  std::cout << "[stage] parse           : " << secs(t0, t1) << " s\n";

  // ---- build the constraint composite graph ------------------------------
  ccg_t ccg;
  WCSPInstance<>::constraint_t::Polynomial p;
  for (const auto& c : instance.getConstraints()) c.toPolynomial(p);
  auto t2 = clk::now();
  std::cout << "[stage] toPolynomial    : " << secs(t1, t2) << " s\n";

  ccg_t::weight_t s = ccg.addPolynomial(p);
  auto t3 = clk::now();
  std::cout << "[stage] addPolynomial   : " << secs(t2, t3) << " s\n";

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

      if (kern == "cpu") {
        maxflow::KernelizerMaxflow<> k;
        k.kernelize(g, assignments);
      }
#ifdef USE_GPU
      else if (kern == "gpu") {
        maxflow::KernelizerMaxflowGPU<> k;
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

  if (solver != "none") {
    if (assignments.size() >= total_vars) {
      std::cout << "[solve] skipped         : kernelization resolved every variable\n";
      solved = true;
    } else {
      auto s0 = clk::now();
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
      auto s1 = clk::now();
      solve_time = secs(s0, s1);
      std::cout << "[solve] time            : " << solve_time << " s\n";
      std::cout << "[solve] mwvc weight     : " << mwvc_weight << "\n";
    }
  }

  // ---- final objective ---------------------------------------------------
  auto t_all1 = clk::now();
  std::cout << "[e2e] assignments       : " << assignments.size() << "\n";
  if (solved) {
    auto opt = instance.computeTotalWeight(assignments);
    std::cout << "[e2e] FINAL OPTIMUM     : " << opt << "\n";
  } else {
    std::cout << "[e2e] FINAL OPTIMUM     : <not solved>\n";
  }
  if (RunningTime::GetInstance().isTimeOut())
    std::cout << "[e2e] WARNING           : time limit reached, result may be suboptimal\n";
  std::cout << "[e2e] kernel time       : " << kern_time << " s\n";
  std::cout << "[e2e] solve time        : " << solve_time << " s\n";
  std::cout << "[e2e] TOTAL TIME        : " << secs(t_all0, t_all1) << " s\n";
  return 0;
}