// Data worklist GPU max-flow
// Same interface as the topology — read DIMACS, solve, print

#include <iostream>
#include <string>
#include <cuda_runtime.h>
#include "src/cpu/dimacs_reader.h"
#include "src/gpu/maxflow_gpu_worklist.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <graph.dimacs>\n";
    return 1;
  }

  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, 0);
  std::cout << "GPU: " << prop.name << "\n";

  maxflow::flow_network<int> net = maxflow::read_dimacs_maxflow<int>(argv[1]);
  std::cout << "graph: " << net.num_nodes << " nodes, "
            << net.num_edges << " half-edges, source=" << net.source
            << ", sink=" << net.sink << "\n";

  maxflow::gpu_worklist_solver solver(net);

  cudaEvent_t t_start, t_stop;
  cudaEventCreate(&t_start);
  cudaEventCreate(&t_stop);

  cudaEventRecord(t_start);
  int flow = solver.solve();
  cudaEventRecord(t_stop);
  cudaEventSynchronize(t_stop);

  float ms = 0.0f;
  cudaEventElapsedTime(&ms, t_start, t_stop);

  std::cout << "max-flow = " << flow << "\n";
  std::cout << "min-cut source side {";
  bool first = true;
  for (maxflow::vertex_id_t v = 0; v < net.num_nodes; ++v)
    if (solver.is_on_source_side(v)) {
      std::cout << (first ? "" : ", ") << v;
      first = false;
    }
  std::cout << "}\n";
  std::cout << "GPU time: " << ms << " ms\n";

  cudaEventDestroy(t_start);
  cudaEventDestroy(t_stop);
  return 0;
}