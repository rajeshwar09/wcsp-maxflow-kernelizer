// --------------------------------------------------
// apps/benchmark_mac.cpp
//
// Mac benchmark: CPU max-flow kernelizer vs Gurobi LP kernelizer on synthetic CCG graphs of increasing size.
//
// Compile (needs Boost + Gurobi):
//   g++ -std=c++17 -I. -I/opt/homebrew/include \
//       -I${GUROBI_HOME}/include \
//       -L${GUROBI_HOME}/lib \
//       -o benchmark_mac apps/benchmark_mac.cpp \
//       third_party/wcsp-solver/src/LinearProgramSolver.cpp \
//       third_party/wcsp-solver/src/LinearProgramSolverGurobi.cpp \
//       -lgurobi_c++ -lgurobi110 -lm
//
// Run:
//   ./benchmark_mac
// --------------------------------------------------

#include <iostream>
#include <map>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iomanip>

#include "src/integration/KernelizerMaxflow.h"
#include "third_party/wcsp-solver/src/KernelizerLinearProgramming.h"
#include "third_party/wcsp-solver/src/LinearProgramSolverGurobi.h"

using CCG = ConstraintCompositeGraph<>;
using graph_t = CCG::graph_t;
using vertex_t = boost::graph_traits<graph_t>::vertex_descriptor;
using var_id_t = CCG::variable_id_t;
using weight_t = CCG::weight_t;

// --------------------------------------------------
// Generate random CCG graph (Boost adjacency_list)
// --------------------------------------------------

vertex_t add_v(graph_t& g, var_id_t id, weight_t w) {
  vertex_t v = boost::add_vertex(g);
  boost::put(boost::vertex_name, g, v, id);
  boost::put(boost::vertex_weight, g, v, w);
  return v;
}

graph_t generate_ccg(int n, double p, unsigned int seed) {
  srand(seed);
  graph_t g;

  std::vector<vertex_t> verts(n);
  for (int i = 0; i < n; ++i) {
    double w = 1.0 + 99.0 * ((double)rand() / RAND_MAX);
    verts[i] = add_v(g, i, w);
  }

  for (int u = 0; u < n; ++u)
    for (int v = u + 1; v < n; ++v)
      if ((double)rand() / RAND_MAX < p)
        boost::add_edge(verts[u], verts[v], g);

  return g;
}

int count_v(const graph_t& g) {
  int c = 0;
  auto [vi, ve] = boost::vertices(g);
  for (auto it = vi; it != ve; ++it) ++c;
  return c;
}

int count_e(const graph_t& g) {
  int c = 0;
  auto [ei, ee] = boost::edges(g);
  for (auto it = ei; it != ee; ++it) ++c;
  return c;
}

// --------------------------------------------------
// Benchmark runner
// --------------------------------------------------

int main() {
  std::cout << "============================================================\n";
  std::cout << " Mac Benchmark: CPU Max-Flow vs Gurobi LP Kernelizer\n";
  std::cout << "============================================================\n\n";

  struct config { int n; double p; const char* label; };
  config configs[] = {
    {100, 0.10, "100"},
    {500, 0.02, "500"},
    {1000, 0.01, "1K"},
    {5000, 0.002, "5K"},
    {10000, 0.001, "10K"},
  };
  int num = sizeof(configs) / sizeof(configs[0]);
  unsigned int seed = 42;

  std::cout << std::setw(8)  << "Verts"
            << std::setw(8)  << "Edges"
            << std::setw(14) << "Gurobi(ms)"
            << std::setw(14) << "CPUmaxflow(ms)"
            << std::setw(10) << "GurKern"
            << std::setw(10) << "MFKern"
            << std::setw(10) << "Match?"
            << "\n";
  std::cout << std::string(74, '-') << "\n";

  for (int c = 0; c < num; ++c) {
    auto& cfg = configs[c];

    // Generate TWO identical copies of the same graph
    graph_t g_gurobi = generate_ccg(cfg.n, cfg.p, seed);
    graph_t g_maxflow = generate_ccg(cfg.n, cfg.p, seed);

    int edges = count_e(g_gurobi);
    if (edges == 0) {
      std::cout << std::setw(8) << cfg.label << "  (no edges, skipping)\n";
      continue;
    }

    // Run Gurobi LP kernelizer
    std::map<var_id_t, bool> out_gurobi;
    auto t0 = std::chrono::high_resolution_clock::now();
    KernelizerLinearProgramming<> klp(new LinearProgramSolverGurobi());
    klp.kernelize(g_gurobi, out_gurobi);
    auto t1 = std::chrono::high_resolution_clock::now();
    double t_gurobi = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Run CPU max-flow kernelizer
    std::map<var_id_t, bool> out_maxflow;
    t0 = std::chrono::high_resolution_clock::now();
    maxflow::KernelizerMaxflow<> kmf;
    kmf.kernelize(g_maxflow, out_maxflow);
    t1 = std::chrono::high_resolution_clock::now();
    double t_maxflow = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Kernel sizes (remaining vertices after kernelization)
    int kern_gurobi  = count_v(g_gurobi);
    int kern_maxflow = count_v(g_maxflow);

    // Check if both kernelizers fixed the same NUMBER of vertices
    // (specific assignments may differ due to LP non-uniqueness)
    bool match = (out_gurobi.size() == out_maxflow.size());

    std::cout << std::setw(8)  << cfg.label
              << std::setw(8)  << edges
              << std::setw(14) << std::fixed << std::setprecision(2) << t_gurobi
              << std::setw(14) << t_maxflow
              << std::setw(10) << kern_gurobi
              << std::setw(10) << kern_maxflow
              << std::setw(10) << (match ? "YES" : "DIFF")
              << "\n";
  }

  std::cout << std::string(74, '-') << "\n";
  std::cout << "GurKern = kernel size after Gurobi LP\n";
  std::cout << "MFKern  = kernel size after CPU max-flow\n";
  std::cout << "Match   = both fixed same number of variables\n";
  std::cout << "         (DIFF is OK — different valid LP optima, see R2)\n";

  return 0;
}