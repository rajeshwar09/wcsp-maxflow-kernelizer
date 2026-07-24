// Similar as e2e_pipeline_test.cpp
// But kernelizes with WCSPLift's original Gurobi LP kernelizer (and not KernelizerMaxflow)

// Usage : ./e2e_pipeline_test_gurobi <file.wcsp>

#include <iostream>
#include <fstream>
#include <map>
#include <chrono>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "third_party/wcsp-solver/src/KernelizerLinearProgramming.h"
#include "third_party/wcsp-solver/src/LinearProgramSolverGurobi.h"

using clk = std::chrono::high_resolution_clock;
static double es(clk::time_point a, clk::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: " << argv[0] << " <file.wcsp>\n"; return 1; }

  auto t0 = clk::now();
  std::ifstream in(argv[1]);
  if (!in) { std::cerr << "cannot open " << argv[1] << "\n"; return 2; }
  WCSPInstance<> inst(in, WCSPInstance<>::Format::DIMACS);
  auto t1 = clk::now(); std::cout << "[parse DIMACS]        " << es(t0, t1) << " s\n";

  ConstraintCompositeGraph<> ccg;
  WCSPInstance<>::constraint_t::Polynomial p;
  for (const auto& c : inst.getConstraints()) c.toPolynomial(p);
  auto t2 = clk::now(); std::cout << "[toPolynomial all]    " << es(t1, t2) << " s\n";

  auto s = ccg.addPolynomial(p);
  auto t3 = clk::now(); std::cout << "[ccg.addPolynomial]   " << es(t2, t3) << " s\n";

  std::map<ConstraintCompositeGraph<>::variable_id_t, bool> as;
  ccg.simplify(as);
  auto t4 = clk::now(); std::cout << "[ccg.simplify]        " << es(t3, t4) << " s\n";

  auto g = *ccg.getGraph();
  auto t5 = clk::now(); std::cout << "[getGraph copy]       " << es(t4, t5) << " s\n";

  KernelizerLinearProgramming<> klp(new LinearProgramSolverGurobi());
  klp.kernelize(g, as);
  auto t6 = clk::now(); std::cout << "[KernelizerLP]        " << es(t5, t6) << " s\n";

  std::cout << "TOTAL: " << es(t0, t6) << " s\n";
  std::cout << "Remnant s=" << s << ", resolved=" << as.size() << "\n";
  return 0;
}