// Runs BOTH kernelizers on the same CCG built from one .wcsp file, then compares the resulting assignments variable by variable
//
// This answers: when the two kernelizers resolve different NUMBERS of variables, is that because the vertex-cover LP has several optimal solutions or because one of them assigns a variable the wrong value?
//
// Usage: ./compare_kernels <file.wcsp>

#include <iostream>
#include <fstream>
#include <map>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "third_party/wcsp-solver/src/KernelizerLinearProgramming.h"
#include "third_party/wcsp-solver/src/LinearProgramSolverGurobi.h"
#include "src/integration/KernelizerMaxflow.h"

typedef ConstraintCompositeGraph<>::variable_id_t vid_t;

static void build(const char* path, ConstraintCompositeGraph<>::graph_t& g, std::map<vid_t,bool>& pre) {
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

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: " << argv[0] << " <file.wcsp>\n"; return 1; }

  ConstraintCompositeGraph<>::graph_t g1, g2;
  std::map<vid_t,bool> a_mf, a_lp, pre1, pre2;
  build(argv[1], g1, pre1); a_mf = pre1;
  build(argv[1], g2, pre2); a_lp = pre2;

  maxflow::KernelizerMaxflow<> kmf; kmf.kernelize(g1, a_mf);
  KernelizerLinearProgramming<> klp(new LinearProgramSolverGurobi()); klp.kernelize(g2, a_lp);

  size_t only_mf=0, only_lp=0, agree=0, conflict=0;
  for (const auto& kv : a_mf) {
    auto it = a_lp.find(kv.first);
    if (it == a_lp.end()) { ++only_mf; continue; }
    if (it->second == kv.second) ++agree; else ++conflict;
  }
  for (const auto& kv : a_lp)
    if (a_mf.find(kv.first) == a_mf.end()) ++only_lp;

  std::cout << "=== kernel comparison: " << argv[1] << " ===\n";
  std::cout << "  decided by max-flow only : " << only_mf   << "\n";
  std::cout << "  decided by Gurobi only   : " << only_lp   << "\n";
  std::cout << "  decided by both, SAME    : " << agree     << "\n";
  std::cout << "  decided by both, OPPOSITE: " << conflict  << "\n\n";
  if (conflict == 0)
    std::cout << "  VERDICT: no contradictions. The two kernels differ only in HOW MANY\n"
                  "           variables they decide, never in WHAT they decide. This is the\n"
                  "           expected consequence of the LP having multiple optimal solutions.\n";
  else
    std::cout << "  VERDICT: *** " << conflict << " variables were assigned OPPOSITE values. ***\n"
                  "           This is not explainable by non-unique optima. Investigate.\n";
  return 0;
}