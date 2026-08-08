// Runs BOTH kernelizers on the same CCG built from one .wcsp file, then compares the resulting assignments variable by variable
//
// Usage:
//   ./compare_kernels <file.wcsp>              both kernelizers, one process
//   ./compare_kernels mf  <file.wcsp> <out>    max-flow only, dump assignments
//   ./compare_kernels lp  <file.wcsp> <out>    Gurobi only, dump assignments
//   ./compare_kernels cmp <a> <b>              compare two dumps (a=mf, b=lp)
//
// For anything at 300k or above, prefer the split form:
//   ./compare_kernels mf data/bench/10_1M.wcsp /tmp/mf.txt
//   ./compare_kernels lp data/bench/10_1M.wcsp /tmp/lp.txt
//   ./compare_kernels cmp /tmp/mf.txt /tmp/lp.txt

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "third_party/wcsp-solver/src/KernelizerLinearProgramming.h"
#include "third_party/wcsp-solver/src/LinearProgramSolverGurobi.h"
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

//  Run the LP kernelizer and release the CCG before returning.
static void run_gurobi(const char* path, std::map<vid_t, bool>& out) {
  graph_t g;
  std::map<vid_t, bool> pre;
  build(path, g, pre);
  out = pre;
  std::map<vid_t, bool>().swap(pre);
  KernelizerLinearProgramming<> k(new LinearProgramSolverGurobi());
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

static void compare(const std::map<vid_t, bool>& a_mf,
                    const std::map<vid_t, bool>& a_lp,
                    const char* label) {
  size_t only_mf = 0, only_lp = 0, agree = 0, conflict = 0;
  for (const auto& kv : a_mf) {
    auto it = a_lp.find(kv.first);
    if (it == a_lp.end()) { ++only_mf; continue; }
    if (it->second == kv.second) ++agree; else ++conflict;
  }
  for (const auto& kv : a_lp)
    if (a_mf.find(kv.first) == a_mf.end()) ++only_lp;

  std::cout << "=== kernel comparison: " << label << " ===\n";
  std::cout << "  decided by max-flow      : " << a_mf.size() << "\n";
  std::cout << "  decided by Gurobi        : " << a_lp.size() << "\n";
  std::cout << "  decided by max-flow only : " << only_mf  << "\n";
  std::cout << "  decided by Gurobi only   : " << only_lp  << "\n";
  std::cout << "  decided by both, SAME    : " << agree    << "\n";
  std::cout << "  decided by both, OPPOSITE: " << conflict << "\n\n";
  if (conflict == 0)
    std::cout << "  VERDICT: no contradictions. The two kernels differ only in HOW MANY\n"
                 "           variables they decide, never in WHAT they decide. This is the\n"
                 "           expected consequence of the LP having multiple optimal solutions.\n";
  else
    std::cout << "  VERDICT: *** " << conflict << " variables were assigned OPPOSITE values. ***\n"
                 "           This is not explainable by non-unique optima. Investigate.\n";
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
    run_gurobi(argv[2], a);
    dump(a, argv[3]);
    return 0;
  }
  if (argc >= 4 && std::strcmp(argv[1], "cmp") == 0) {
    std::map<vid_t, bool> a_mf, a_lp;
    load(argv[2], a_mf);
    load(argv[3], a_lp);
    compare(a_mf, a_lp, argv[2]);
    return 0;
  }

  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <file.wcsp>\n"
              << "       " << argv[0] << " mf|lp <file.wcsp> <out.txt>\n"
              << "       " << argv[0] << " cmp <mf.txt> <lp.txt>\n";
    return 1;
  }

  //  Single-process mode. Each CCG is destroyed before the next is built, so
  //  peak memory is one CCG plus whichever solver structure is larger.
  std::map<vid_t, bool> a_mf, a_lp;
  run_maxflow(argv[1], a_mf);
  std::cerr << "  [max-flow stage done: " << a_mf.size() << " decided]\n";
  run_gurobi(argv[1], a_lp);
  compare(a_mf, a_lp, argv[1]);
  return 0;
}