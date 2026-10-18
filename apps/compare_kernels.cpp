// gate test: runs the max-flow kernelizer and the Gurobi LP kernelizer on the same CCG from one .wcsp file
// and compares their assignments variable by variable, the two may decide a different number of variables
// (the LP can have more than one optimum) but must never give a variable opposite values
//
// build: needs Gurobi, made by ./scripts/build.sh gates
// usage:
//   ./compare_kernels <file.wcsp>              both kernelizers in one process
//   ./compare_kernels mf  <file.wcsp> <out>    max-flow only, writes the assignments to a file
//   ./compare_kernels lp  <file.wcsp> <out>    Gurobi only, writes the assignments to a file
//   ./compare_kernels cmp <a> <b>              compares two such files (a = max-flow, b = Gurobi)
// for 300k variables or more use the split form so only one CCG is in memory at a time:
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

// [CCG] builds the CCG of the file, the graph has to be copied (getGraph gives a const pointer and kernelize changes the graph),
// so for a short time two CCGs are in memory, keep the call inside a small scope
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

// [RUN] max-flow kernelizer, the CCG is freed before returning
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

// [RUN] Gurobi LP kernelizer, the CCG is freed before returning
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

// [FILE] writes "variable value" lines
static void dump(const std::map<vid_t, bool>& m, const char* path) {
  std::ofstream o(path);
  if (!o) { std::cerr << "cannot write " << path << "\n"; std::exit(2); }
  for (const auto& kv : m) o << kv.first << " " << (kv.second ? 1 : 0) << "\n";
  std::cerr << "wrote " << m.size() << " assignments to " << path << "\n";
}

// [FILE] reads a file written by dump
static void load(const char* path, std::map<vid_t, bool>& m) {
  std::ifstream in(path);
  if (!in) { std::cerr << "cannot open " << path << "\n"; std::exit(2); }
  long v; int b;
  while (in >> v >> b) m[static_cast<vid_t>(v)] = (b != 0);
}

// [COMPARE] counts variables decided by one side only, by both with the same value, and by both with opposite values
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
  // [ARGS] split form: mf, lp or cmp
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

  // [RUN] one process mode, each CCG is freed before the next one is built, so the peak memory is one CCG plus the larger of the two solvers
  std::map<vid_t, bool> a_mf, a_lp;
  run_maxflow(argv[1], a_mf);
  std::cerr << "  [max-flow stage done: " << a_mf.size() << " decided]\n";
  run_gurobi(argv[1], a_lp);
  compare(a_mf, a_lp, argv[1]);
  return 0;
}