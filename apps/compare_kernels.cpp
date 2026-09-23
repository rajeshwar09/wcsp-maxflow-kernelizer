// Runs kernelizers on the same CCG built from one .wcsp file and compares the resulting assignments variable by variable
//
// Usage:
//   ./compare_kernels <file.wcsp>                 max-flow and Gurobi, one process
//   ./compare_kernels mf    <file.wcsp> <out>     max-flow only, dump assignments
//   ./compare_kernels lp    <file.wcsp> <out>     Gurobi LP only, dump assignments
//   ./compare_kernels cplex <file.wcsp> <out>     CPLEX LP only, dump assignments (needs a -DHAVE_CPLEX build)
//   ./compare_kernels cmp   <a> <b>               compare any two dumps
//
// For anything at 300k or above, prefer the split form:
//   ./compare_kernels mf    data/bench/10_1M.wcsp /tmp/mf.txt
//   ./compare_kernels cplex data/bench/10_1M.wcsp /tmp/cplex.txt
//   ./compare_kernels cmp   /tmp/mf.txt /tmp/cplex.txt
//
// Exit code: 0 = done (for cmp: no contradictions), 3 = cmp found OPPOSITE values, 1/2 = usage or file errors

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "third_party/wcsp-solver/src/LinearProgramSolver.h"
#include "third_party/wcsp-solver/src/KernelizerLinearProgramming.h"
#include "third_party/wcsp-solver/src/LinearProgramSolverGurobi.h"
#ifdef HAVE_CPLEX
#include "third_party/wcsp-solver/src/LinearProgramSolverCplex.h"
#endif
#include "src/integration/KernelizerMaxflow.h"

typedef ConstraintCompositeGraph<>::variable_id_t vid_t;
typedef ConstraintCompositeGraph<>::graph_t graph_t;

//  Build the CCG. The copy out of ConstraintCompositeGraph is unavoidable --
//  getGraph() returns a const pointer and kernelize() needs a mutable graph --
//  so this transiently holds two CCGs. Keep it inside as tight a scope as
//  possible at the call site.
static void build(const char* path, graph_t& g, std::map<vid_t, bool>& pre) {
  std::ifstream in(path);
  if (!in) { std::cerr << "cannot open " << path << "\n"; std::exit(2); }
  WCSPInstance<> inst(in, WCSPInstance<>::Format::DIMACS);
  ConstraintCompositeGraph<> ccg;
  WCSPInstance<>::constraint_t::Polynomial p;
  for (const auto& c : inst.getConstraints()) c.toPolynomial(p);
  ccg.addPolynomial(p);
  ccg.simplify(pre);
  g = *ccg.getGraph();
}

//  Run the max-flow kernelizer and release the CCG before returning.
static void run_maxflow(const char* path, std::map<vid_t, bool>& out) {
  graph_t g;
  std::map<vid_t, bool> pre;
  build(path, g, pre);
  out = pre;
  std::map<vid_t, bool>().swap(pre);
  maxflow::KernelizerMaxflow<> k;
  k.kernelize(g, out);
  graph_t().swap(g);
}

//  Run an LP kernelizer with the given solver backend and release the CCG before returning.
//  KernelizerLinearProgramming takes ownership of the solver object.
static void run_lp(const char* path, std::map<vid_t, bool>& out, LinearProgramSolver* solver) {
  graph_t g;
  std::map<vid_t, bool> pre;
  build(path, g, pre);
  out = pre;
  std::map<vid_t, bool>().swap(pre);
  KernelizerLinearProgramming<> k(solver);
  k.kernelize(g, out);
  graph_t().swap(g);
}

static void dump(const std::map<vid_t, bool>& m, const char* path) {
  std::ofstream o(path);
  if (!o) { std::cerr << "cannot write " << path << "\n"; std::exit(2); }
  for (const auto& kv : m) o << kv.first << " " << (kv.second ? 1 : 0) << "\n";
  std::cerr << "wrote " << m.size() << " assignments to " << path << "\n";
}

static void load(const char* path, std::map<vid_t, bool>& m) {
  std::ifstream in(path);
  if (!in) { std::cerr << "cannot open " << path << "\n"; std::exit(2); }
  long v; int b;
  while (in >> v >> b) m[static_cast<vid_t>(v)] = (b != 0);
}

static std::string base_name(const char* path) {
  std::string s(path);
  size_t slash = s.find_last_of('/');
  return (slash == std::string::npos) ? s : s.substr(slash + 1);
}

//  Returns the number of variables the two kernels assigned OPPOSITE values
static size_t compare(const std::map<vid_t, bool>& a, const std::map<vid_t, bool>& b, const std::string& label, const std::string& name_a, const std::string& name_b) {
  size_t only_a = 0, only_b = 0, agree = 0, conflict = 0;
  for (const auto& kv : a) {
    auto it = b.find(kv.first);
    if (it == b.end()) { ++only_a; continue; }
    if (it->second == kv.second) ++agree; else ++conflict;
  }
  for (const auto& kv : b)
    if (a.find(kv.first) == a.end()) ++only_b;

  std::cout << "=== kernel comparison: " << label << " ===\n";
  std::cout << "  A = " << name_a << "\n";
  std::cout << "  B = " << name_b << "\n";
  std::cout << "  decided by A             : " << a.size()  << "\n";
  std::cout << "  decided by B             : " << b.size()  << "\n";
  std::cout << "  decided by A only        : " << only_a    << "\n";
  std::cout << "  decided by B only        : " << only_b    << "\n";
  std::cout << "  decided by both, SAME    : " << agree     << "\n";
  std::cout << "  decided by both, OPPOSITE: " << conflict  << "\n\n";
  if (conflict == 0)
    std::cout << "  VERDICT: no contradictions. The two kernels differ only in HOW MANY\n"
                 "           variables they decide, never in WHAT they decide. This is the\n"
                 "           expected consequence of the LP having multiple optimal solutions.\n";
  else
    std::cout << "  VERDICT: *** " << conflict << " variables were assigned OPPOSITE values. ***\n"
                 "           This is not explainable by non-unique optima. Investigate.\n";
  return conflict;
}

int main(int argc, char** argv) {
  if (argc >= 4 && std::strcmp(argv[1], "mf") == 0) {
    std::map<vid_t, bool> a;
    run_maxflow(argv[2], a);
    dump(a, argv[3]);
    return 0;
  }
  if (argc >= 4 && std::strcmp(argv[1], "lp") == 0) {
    std::map<vid_t, bool> a;
    run_lp(argv[2], a, new LinearProgramSolverGurobi());
    dump(a, argv[3]);
    return 0;
  }
  if (argc >= 4 && std::strcmp(argv[1], "cplex") == 0) {
#ifdef HAVE_CPLEX
    std::map<vid_t, bool> a;
    run_lp(argv[2], a, new LinearProgramSolverCplex());
    dump(a, argv[3]);
    return 0;
#else
    std::cerr << "this binary was built without -DHAVE_CPLEX\n";
    return 1;
#endif
  }
  if (argc >= 4 && std::strcmp(argv[1], "cmp") == 0) {
    std::map<vid_t, bool> a, b;
    load(argv[2], a);
    load(argv[3], b);
    const size_t conflict = compare(a, b, base_name(argv[2]) + " vs " + base_name(argv[3]),
                                    base_name(argv[2]), base_name(argv[3]));
    return conflict == 0 ? 0 : 3;
  }

  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <file.wcsp>\n"
              << "       " << argv[0] << " mf|lp|cplex <file.wcsp> <out.txt>\n"
              << "       " << argv[0] << " cmp <a.txt> <b.txt>\n";
    return 1;
  }

  //  Single-process mode. Each CCG is destroyed before the next is built, so
  //  peak memory is one CCG plus whichever solver structure is larger.
  std::map<vid_t, bool> a_mf, a_lp;
  run_maxflow(argv[1], a_mf);
  std::cerr << "  [max-flow stage done: " << a_mf.size() << " decided]\n";
  run_lp(argv[1], a_lp, new LinearProgramSolverGurobi());
  const size_t conflict = compare(a_mf, a_lp, argv[1], "max-flow", "Gurobi LP");
  return conflict == 0 ? 0 : 3;
}