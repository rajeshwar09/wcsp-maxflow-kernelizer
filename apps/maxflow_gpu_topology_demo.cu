//  Demo: read a DIMACS max-flow graph on the host, solve it on the GPU using topology push-relabel
//  Print results

#include <iostream>
#include <string>
#include <cuda_runtime.h>

#include "src/cpu/dimacs_reader.h"
#include "src/gpu/maxflow_gpu_topology.h"

int main(int argc, char const *argv[]) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <graph.dimacs>\n";
    return 1;
  }

  //  Print GPU device info
  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, 0);
  std::cout << "GPU: " << prop.name << "\n";

  //  Read graph on host
  maxflow::flow_network<int> net = maxflow::read_dimacs_maxflow<int>(argv[1]);
  std::cout << "graph: " << net.num_nodes << " nodes, "
            << net.num_edges << " half-edges, source=" << net.source
            << ", sink=" << net.sink << "\n";
  
  //  Solve on GPU and record timing
  maxflow::gpu_topology_solver solver(net);

  cudaEvent_t t_start, t_stop;
  cudaEventCreate(&t_start);
  cudaEventCreate(&t_stop);

  cudaEventRecord(t_start);
  int flow = solver.solve();
  cudaEventRecord(t_stop);
  cudaEventSynchronize(t_stop);

  float ms = 0.0f;
  cudaEventElapsedTime(&ms, t_start, t_stop);

  //  Print results
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
