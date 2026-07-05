// --------------------------------------------------
// Benchmark: CPU vs GPU topology vs GPU worklist max-flow on bipartite double-cover graphs of increasing size
//
// Runs on Colab (Tesla T4). No Boost or Gurobi needed
//
// Compile:
//   nvcc -std=c++17 -arch=sm_75 -I. -O2 apps/benchmark_gpu.cu -o benchmark_gpu
// Run:
//   ./benchmark_gpu
// --------------------------------------------------

#include <iostream>
#include <vector>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <cuda_runtime.h>

#include "src/common/types.h"
#include "src/cpu/graph_csr.h"
#include "src/cpu/maxflow_static.h"
#include "src/gpu/maxflow_gpu_topology.h"
#include "src/gpu/maxflow_gpu_worklist.h"

using namespace maxflow;

// --------------------------------------------------
// Random graph generator
//
// Generates an Erdos-Renyi random undirected weighted graph and builds its bipartite double-cover flow network
//
// Parameters:
//   n = number of original vertices
//   p = edge probability (0.0 to 1.0)
//   seed = random seed for reproducibility
//   w_min = minimum vertex weight
//   w_max = maximum vertex weight
//
// Output: flow_network<cap_t> with 2n+2 vertices
// --------------------------------------------------

struct graph_stats {
  int original_vertices;
  int original_edges;
  int flow_vertices;
  int flow_edges;
};

graph_stats build_double_cover(int n, double p, unsigned int seed, double w_min, double w_max, flow_network<cap_t>& net) {
  srand(seed);

  // Generate random vertex weights
  std::vector<cap_t> weights(n);
  cap_t total_weight = cap_t(0);
  for (int i = 0; i < n; i++) {
    weights[i] = w_min + (w_max - w_min) * ((double)rand() / RAND_MAX);
    total_weight += weights[i];
  }

  // Generate random edges (Erdos-Renyi)
  // Store as pairs to avoid duplicates
  std::vector<std::pair<int,int>> original_edges;
  for (int u = 0; u < n; u++) {
    for (int v = u + 1; v < n; v++) {
      double r = (double)rand() / RAND_MAX;
      if (r < p) {
        original_edges.push_back({u, v});
      }
    }
  }

  int num_orig_edges = (int)original_edges.size();

  // Build bipartite double-cover flow network
  //   Vertex layout:
  //     0 = source
  //     1..n = left copies
  //     n+1..2n = right copies
  //     2n+1 = sink
  int flow_n = 2 * n + 2;
  vertex_id_t flow_source = 0;
  vertex_id_t flow_sink = 2 * n + 1;

  // INF capacity = 1 + sum of all weights
  cap_t inf_cap = cap_t(1) + total_weight;

  std::vector<edge<cap_t>> flow_edges;
  flow_edges.reserve(2 * n + 4 * num_orig_edges);

  // Source and sink edges
  for (int i = 0; i < n; i++) {
    int left  = i + 1;
    int right = n + i + 1;
    flow_edges.push_back({flow_source, left, weights[i]});
    flow_edges.push_back({right, flow_sink, weights[i]});
  }

  // Cross edges
  for (auto& [u, v] : original_edges) {
    int u_left  = u + 1;
    int v_left  = v + 1;
    int u_right = n + u + 1;
    int v_right = n + v + 1;
    flow_edges.push_back({u_left, v_right, inf_cap});
    flow_edges.push_back({v_left, u_right, inf_cap});
  }

  net.build_from_edges(flow_n, flow_source, flow_sink, flow_edges);

  return {n, num_orig_edges, flow_n, net.num_edges};
}

// --------------------------------------------------
// Timing helpers
// --------------------------------------------------

double time_cpu_solver(flow_network<cap_t>& net, cap_t& flow_out) {
  // Make a copy because solver modifies residual_capacity
  flow_network<cap_t> net_copy = net;
  auto t0 = std::chrono::high_resolution_clock::now();
  static_max_flow_solver<cap_t> solver(net_copy);
  flow_out = solver.solve();
  auto t1 = std::chrono::high_resolution_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double time_gpu_topo(flow_network<cap_t>& net, cap_t& flow_out) {
  // Make a copy
  flow_network<cap_t> net_copy = net;
  cudaEvent_t t0, t1;
  cudaEventCreate(&t0);
  cudaEventCreate(&t1);

  gpu_topology_solver solver(net_copy);
  cudaEventRecord(t0);
  flow_out = solver.solve();
  cudaEventRecord(t1);
  cudaEventSynchronize(t1);

  float ms;
  cudaEventElapsedTime(&ms, t0, t1);
  cudaEventDestroy(t0);
  cudaEventDestroy(t1);
  return (double)ms;
}

double time_gpu_worklist(flow_network<cap_t>& net, cap_t& flow_out) {
  // Make a copy
  flow_network<cap_t> net_copy = net;
  cudaEvent_t t0, t1;
  cudaEventCreate(&t0);
  cudaEventCreate(&t1);

  gpu_worklist_solver solver(net_copy);
  cudaEventRecord(t0);
  flow_out = solver.solve();
  cudaEventRecord(t1);
  cudaEventSynchronize(t1);

  float ms;
  cudaEventElapsedTime(&ms, t0, t1);
  cudaEventDestroy(t0);
  cudaEventDestroy(t1);
  return (double)ms;
}

// --------------------------------------------------
// Main benchmark
// --------------------------------------------------

int main() {
  // Print GPU info
  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, 0);
  std::cout << "GPU: " << prop.name
            << " (compute " << prop.major << "." << prop.minor << ")\n";
  std::cout << "cap_t = " << sizeof(cap_t) << " bytes ("
            << (sizeof(cap_t) == 8 ? "double" : "int/float") << ")\n\n";

  // Benchmark parameters
  struct benchmark_config {
    int n;           // original vertices
    double p;        // edge probability
    const char* label;
  };

  benchmark_config configs[] = {
    {100, 0.10, "100"},
    {500, 0.02, "500"},
    {1000, 0.01, "1K"},
    {5000, 0.002, "5K"},
    {10000, 0.001, "10K"},
    {50000, 0.0002, "50K"},
  };

  int num_configs = sizeof(configs) / sizeof(configs[0]);
  unsigned int seed = 42;

  // Print header
  std::cout << std::setw(8)  << "Vertices"
            << std::setw(8)  << "Edges"
            << std::setw(10) << "FlowV"
            << std::setw(10) << "FlowE"
            << std::setw(12) << "CPU(ms)"
            << std::setw(12) << "GPUtopo(ms)"
            << std::setw(12) << "GPUwl(ms)"
            << std::setw(10) << "Speedup"
            << std::setw(10) << "Match?"
            << "\n";
  std::cout << std::string(92, '-') << "\n";

  for (int c = 0; c < num_configs; ++c) {
    auto& cfg = configs[c];
    flow_network<cap_t> net;
    auto stats = build_double_cover(cfg.n, cfg.p, seed, 1.0, 100.0, net);

    // Skip if graph is too small to be interesting
    if (stats.original_edges == 0) {
      std::cout << std::setw(8) << cfg.label << "  (no edges, skipping)\n";
      continue;
    }

    // Run all three solvers
    cap_t flow_cpu, flow_topo, flow_wl;
    double t_cpu = time_cpu_solver(net, flow_cpu);
    double t_topo = time_gpu_topo(net, flow_topo);
    double t_wl = time_gpu_worklist(net, flow_wl);

    // Check all three agree
    bool match = (std::abs(flow_cpu - flow_topo) < 1e-6) && (std::abs(flow_cpu - flow_wl) < 1e-6);

    // Speedup = CPU time / best GPU time
    double best_gpu = std::min(t_topo, t_wl);
    double speedup  = (best_gpu > 0) ? t_cpu / best_gpu : 0.0;

    std::cout << std::setw(8)  << cfg.label
              << std::setw(8)  << stats.original_edges
              << std::setw(10) << stats.flow_vertices
              << std::setw(10) << stats.flow_edges
              << std::setw(12) << std::fixed << std::setprecision(2) << t_cpu
              << std::setw(12) << t_topo
              << std::setw(12) << t_wl
              << std::setw(10) << std::setprecision(1) << speedup << "x"
              << std::setw(9)  << (match ? "YES" : "FAIL")
              << "\n";
  }

  std::cout << std::string(92, '-') << "\n";
  std::cout << "Speedup = CPU time / best GPU time\n";
  std::cout << "Match = all three solvers agree on max-flow value\n";

  return 0;
}