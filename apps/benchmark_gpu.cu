// --------------------------------------------------
// Benchmark: CPU vs GPU topology vs GPU worklist max-flow
// on bipartite double-cover graphs up to 1M vertices.
//
// Runs on Colab (Tesla T4). No Boost or Gurobi needed.
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
#include <algorithm>
#include <cuda_runtime.h>

#include "src/common/types.h"
#include "src/cpu/graph_csr.h"
#include "src/cpu/maxflow_static.h"
#include "src/gpu/maxflow_gpu_topology.h"
#include "src/gpu/maxflow_gpu_worklist.h"

using namespace maxflow;

// --------------------------------------------------
// Random graph generator — O(E) geometric skip method
//
// For small n (< 5000): iterate all pairs (simple, correct)
// For large n (>= 5000): geometric skip (O(E), fast)
//
// Both produce Erdos-Renyi G(n,p) distributions.
// Geometric skip avoids the O(n^2) loop that would take
// hours for n = 500K or 1M.
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
  std::vector<std::pair<int,int>> original_edges;

  if (n < 5000) {
    // Small graphs: iterate all pairs
    for (int u = 0; u < n; u++) {
      for (int v = u + 1; v < n; v++) {
        if ((double)rand() / RAND_MAX < p) {
          original_edges.push_back({u, v});
        }
      }
    }
  } else {
    // Large graphs: geometric skip method
    // For each vertex u, skip ahead by a geometrically distributed number of vertices to find the next edge
    // Produces the exact same distribution as the all-pairs method but in O(E) time instead of O(n^2)
    double log_1_minus_p = log(1.0 - p);
    for (int u = 0; u < n - 1; u++) {
      int v = u;
      while (true) {
        double r = (double)rand() / RAND_MAX;
        if (r == 0.0) r = 1e-15;  // avoid log(0)
        int skip = 1 + (int)(log(r) / log_1_minus_p);
        v += skip;
        if (v >= n) break;
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
  flow_network<cap_t> net_copy = net;
  auto t0 = std::chrono::high_resolution_clock::now();
  static_max_flow_solver<cap_t> solver(net_copy);
  flow_out = solver.solve();
  auto t1 = std::chrono::high_resolution_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double time_gpu_topo(flow_network<cap_t>& net, cap_t& flow_out) {
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
  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, 0);
  std::cout << "GPU: " << prop.name
            << " (compute " << prop.major << "." << prop.minor << ")\n";
  std::cout << "cap_t = " << sizeof(cap_t) << " bytes ("
            << (sizeof(cap_t) == 8 ? "double" : "int/float") << ")\n";
  std::cout << "MAXFLOW_EPSILON = " << MAXFLOW_EPSILON << "\n\n";

  // Benchmark parameters
  //   run_cpu = false for very large graphs (CPU is too slow)
  struct benchmark_config {
    int n;
    double p;
    const char* label;
    bool run_cpu;
  };

  benchmark_config configs[] = {
    {100, 0.100, "100", true},
    {500, 0.020, "500", true},
    {1000, 0.010, "1K", true},
    {5000, 0.002, "5K", true},
    {10000, 0.001, "10K", true},
    {50000, 0.0002, "50K", true},
    {100000, 0.0001, "100K", true},
    {500000, 0.00002, "500K", false},
    {1000000, 0.00001, "1M", false},
  };

  int num_configs = sizeof(configs) / sizeof(configs[0]);
  unsigned int seed = 42;

  // Print header
  std::cout << std::setw(8)  << "Verts"
            << std::setw(10) << "Edges"
            << std::setw(10) << "FlowV"
            << std::setw(12) << "FlowE"
            << std::setw(12) << "CPU(ms)"
            << std::setw(12) << "GPUtopo(ms)"
            << std::setw(12) << "GPUwl(ms)"
            << std::setw(12) << "TopoSpdup"
            << std::setw(12) << "WlSpdup"
            << std::setw(8)  << "Match?"
            << "\n";
  std::cout << std::string(108, '-') << "\n";

  for (int c = 0; c < num_configs; ++c) {
    auto& cfg = configs[c];

    // Generate graph (print label immediately so user sees progress)
    std::cout << std::setw(8) << cfg.label << std::flush;
    flow_network<cap_t> net;
    auto stats = build_double_cover(cfg.n, cfg.p, seed, 1.0, 100.0, net);

    if (stats.original_edges == 0) {
      std::cout << "  (no edges, skipping)\n";
      continue;
    }

    std::cout << std::setw(10) << stats.original_edges
              << std::setw(10) << stats.flow_vertices
              << std::setw(12) << stats.flow_edges
              << std::flush;

    // CPU solver (skip for very large graphs)
    cap_t flow_cpu = -1, flow_topo = -1, flow_wl = -1;
    double t_cpu = -1;
    if (cfg.run_cpu) {
      t_cpu = time_cpu_solver(net, flow_cpu);
      std::cout << std::setw(12) << std::fixed << std::setprecision(2) << t_cpu
                << std::flush;
    } else {
      std::cout << std::setw(12) << "---" << std::flush;
    }

    // GPU topology solver
    double t_topo = time_gpu_topo(net, flow_topo);
    std::cout << std::setw(12) << std::fixed << std::setprecision(2) << t_topo
              << std::flush;

    // GPU worklist solver
    double t_wl = time_gpu_worklist(net, flow_wl);
    std::cout << std::setw(12) << std::fixed << std::setprecision(2) << t_wl
              << std::flush;

    // Speedup columns (CPU / GPU), only if CPU was run
    if (cfg.run_cpu && t_topo > 0) {
      std::cout << std::setw(11) << std::setprecision(1) << (t_cpu / t_topo) << "x";
    } else {
      std::cout << std::setw(12) << "---";
    }
    if (cfg.run_cpu && t_wl > 0) {
      std::cout << std::setw(11) << std::setprecision(1) << (t_cpu / t_wl) << "x";
    } else {
      std::cout << std::setw(12) << "---";
    }

    // Match check: compare GPU topo vs GPU worklist
    // (if CPU ran, also compare against CPU)
    // Tolerance = 1.0 to account for floating-point rounding
    bool match = (std::abs(flow_topo - flow_wl) < 1.0);
    if (cfg.run_cpu) {
      match = match && (std::abs(flow_cpu - flow_topo) < 1.0);
    }
    std::cout << std::setw(8) << (match ? "YES" : "FAIL") << "\n";
  }

  std::cout << std::string(108, '-') << "\n";
  std::cout << "TopoSpdup = CPU time / GPU topology time\n";
  std::cout << "WlSpdup   = CPU time / GPU worklist time\n";
  std::cout << "Match     = all solvers agree on max-flow value (within tolerance 1.0)\n";
  std::cout << "---       = CPU skipped (too slow at this scale)\n";

  return 0;
}