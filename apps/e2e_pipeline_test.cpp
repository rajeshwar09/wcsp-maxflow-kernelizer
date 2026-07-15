// end-to-end pipeline test:
//   .wcsp DIMACS file -> WCSPInstance -> toPolynomial() -> CCG -> KernelizerMaxflow
//
// Reports timing for each stage. Does NOT run Gurobi / the final MWVC solve --
// this is purely for testing/timing the kernelization stage on a real WCSP instance
//
// Usage: ./e2e_pipeline_test <file.wcsp>

#include <iostream>
#include <fstream>
#include <map>
#include <chrono>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "src/integration/KernelizerMaxflow.h"

using clk = std::chrono::high_resolution_clock;
double elapsed_s(clk::time_point a, clk::time_point b) {
  return std::chrono::duration<double>(b-a).count();
}

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: " << argv[0] << " <file.wcsp>\n"; return 1; }

  auto t0 = clk::now();
  std::ifstream in(argv[1]);
  if (!in) { std::cerr << "cannot open " << argv[1] << "\n"; return 2; }
  WCSPInstance<> instance(in, WCSPInstance<>::Format::DIMACS);
  auto t1 = clk::now();
  std::cout << "[parse DIMACS]        " << elapsed_s(t0,t1) << " s\n";

  ConstraintCompositeGraph<> ccg;
  WCSPInstance<>::constraint_t::Polynomial p;
  for (const auto& c : instance.getConstraints())
    c.toPolynomial(p);
  auto t2 = clk::now();
  std::cout << "[toPolynomial all]    " << elapsed_s(t1,t2) << " s\n";

  auto s = ccg.addPolynomial(p);
  auto t3 = clk::now();
  std::cout << "[ccg.addPolynomial]   " << elapsed_s(t2,t3) << " s\n";

  std::map<ConstraintCompositeGraph<>::variable_id_t, bool> assignments;
  ccg.simplify(assignments);
  auto t4 = clk::now();
  std::cout << "[ccg.simplify]        " << elapsed_s(t3,t4) << " s\n";

  auto g = *ccg.getGraph();
  auto t5 = clk::now();
  std::cout << "[getGraph copy]       " << elapsed_s(t4,t5) << " s\n";

  maxflow::KernelizerMaxflow<> kmf;
  kmf.kernelize(g, assignments);
  auto t6 = clk::now();
  std::cout << "[KernelizerMaxflow]   " << elapsed_s(t5,t6) << " s\n";

  std::cout << "TOTAL: " << elapsed_s(t0,t6) << " s\n";
  std::cout << "Remnant s=" << s << ", resolved=" << assignments.size() << "\n";
  return 0;
}