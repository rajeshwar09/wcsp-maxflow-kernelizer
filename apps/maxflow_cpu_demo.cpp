// Demo : read flow network in DIMACS max-flow format, compute static maxflow, and print values

#include <iostream>
#include <string>

#include "src/cpu/dimacs_reader.h"
#include "src/cpu/maxflow_static.h"

int main(int argc, char const *argv[]) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <graph.dimacs\n";
    return 1;
  }

  using cap_t = long long;

  maxflow::flow_network<cap_t> net = maxflow::read_dimacs_maxflow<cap_t>(argv[1]);
  
  maxflow::static_max_flow_solver<cap_t> solver(net);
    cap_t flow = solver.solve();

    std::cout << "max-flow (source " << net.source
              << " -> sink " << net.sink << ") = " << flow << "\n";

    std::cout << "min-cut source side {";
    bool first = true;
    for (maxflow::vertex_id_t v = 0; v < net.num_nodes; ++v)
      if (solver.is_on_source_side(v)) {
        std::cout << (first ? "" : ", ") << v;
        first = false;
      }
    std::cout << "}\n";

  return 0;
}
