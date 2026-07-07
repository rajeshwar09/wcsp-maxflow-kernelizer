// --------------------------------------------------
// Mac benchmark: CPU max-flow vs Gurobi LP kernelizer
// Graphs up to 500K vertices (Gurobi skipped above 100K)
// --------------------------------------------------

#include <iostream>
#include <map>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iomanip>
#include <unistd.h>
#include <fcntl.h>

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
  if (n < 5000) {
    for (int u = 0; u < n; ++u)
      for (int v = u + 1; v < n; ++v)
        if ((double)rand() / RAND_MAX < p)
          boost::add_edge(verts[u], verts[v], g);
  } else {
    double log_1_minus_p = log(1.0 - p);
    for (int u = 0; u < n - 1; ++u) {
      int v = u;
      while (true) {
        double r = (double)rand() / RAND_MAX;
        if (r == 0.0) r = 1e-15;
        int skip = 1 + (int)(log(r) / log_1_minus_p);
        v += skip;
        if (v >= n) break;
        boost::add_edge(verts[u], verts[v], g);
      }
    }
  }
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

// Suppress stdout (for Gurobi's verbose output)
int suppress_stdout() {
  std::cout.flush();
  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  int null_fd = open("/dev/null", O_WRONLY);
  dup2(null_fd, STDOUT_FILENO);
  close(null_fd);
  return saved;
}

void restore_stdout(int saved) {
  std::cout.flush();
  fflush(stdout);
  dup2(saved, STDOUT_FILENO);
  close(saved);
}

int main() {
  std::cout << "============================================================\n";
  std::cout << " Mac Benchmark: CPU Max-Flow vs Gurobi LP Kernelizer\n";
  std::cout << " Hardware: Apple M2, 8GB RAM\n";
  std::cout << "============================================================\n\n";

  struct config {
    int n;
    double p;
    const char* label;
    bool run_gurobi;
  };

  config configs[] = {
    {      100,   0.100,  "100",  true},
    {      500,   0.020,  "500",  true},
    {     1000,   0.010,   "1K",  true},
    {     5000,   0.002,   "5K",  true},
    {    10000,   0.001,  "10K",  true},
    {    50000,  0.0002,  "50K",  true},
    {   100000,  0.0001, "100K",  true},
    {   500000, 0.00002, "500K",  true},
    {  1000000, 0.00001, "1M",  true},
  };

  int num = sizeof(configs) / sizeof(configs[0]);
  unsigned int seed = 42;

  // Print header
  std::cout << std::setw(8)  << "Verts"
            << std::setw(10) << "Edges"
            << std::setw(14) << "Gurobi(ms)"
            << std::setw(14) << "CPUmf(ms)"
            << std::setw(12) << "MF/Gurobi"
            << std::setw(10) << "GurKern"
            << std::setw(10) << "MFKern"
            << std::setw(8)  << "Match?"
            << "\n";
  std::cout << std::string(86, '-') << "\n";
  std::cout.flush();

  for (int c = 0; c < num; ++c) {
    auto& cfg = configs[c];

    // Generate graph for max-flow
    graph_t g_maxflow = generate_ccg(cfg.n, cfg.p, seed);
    int edges = count_e(g_maxflow);

    if (edges == 0) {
      std::cout << std::setw(8) << cfg.label << "  (no edges, skipping)\n";
      std::cout.flush();
      continue;
    }

    // Gurobi run (with output suppressed)
    double t_gurobi = -1;
    int kern_gurobi = -1;
    int gurobi_fixed = -1;

    if (cfg.run_gurobi) {
      graph_t g_gurobi = generate_ccg(cfg.n, cfg.p, seed);

      int saved = suppress_stdout();

      auto t0 = std::chrono::high_resolution_clock::now();
      KernelizerLinearProgramming<> klp(new LinearProgramSolverGurobi());
      std::map<var_id_t, bool> out_gurobi;
      klp.kernelize(g_gurobi, out_gurobi);
      auto t1 = std::chrono::high_resolution_clock::now();

      t_gurobi = std::chrono::duration<double, std::milli>(t1 - t0).count();
      kern_gurobi = count_v(g_gurobi);
      gurobi_fixed = (int)out_gurobi.size();

      restore_stdout(saved);
    }

    // CPU max-flow run
    std::map<var_id_t, bool> out_maxflow;
    auto t0 = std::chrono::high_resolution_clock::now();
    maxflow::KernelizerMaxflow<> kmf;
    kmf.kernelize(g_maxflow, out_maxflow);
    auto t1 = std::chrono::high_resolution_clock::now();
    double t_maxflow = std::chrono::duration<double, std::milli>(t1 - t0).count();
    int kern_maxflow = count_v(g_maxflow);
    int mf_fixed = (int)out_maxflow.size();

    // Print row (all at once, after both solvers are done)
    std::cout << std::setw(8) << cfg.label
              << std::setw(10) << edges;

    if (cfg.run_gurobi) {
      std::cout << std::setw(14) << std::fixed << std::setprecision(2) << t_gurobi;
    } else {
      std::cout << std::setw(14) << "---";
    }

    std::cout << std::setw(14) << std::fixed << std::setprecision(2) << t_maxflow;

    if (cfg.run_gurobi && t_maxflow > 0) {
      double ratio = t_gurobi / t_maxflow;
      std::cout << std::setw(11) << std::setprecision(1) << ratio << "x";
    } else {
      std::cout << std::setw(12) << "---";
    }

    if (cfg.run_gurobi) {
      std::cout << std::setw(10) << kern_gurobi;
    } else {
      std::cout << std::setw(10) << "---";
    }

    std::cout << std::setw(10) << kern_maxflow;

    if (cfg.run_gurobi) {
      bool match = (gurobi_fixed == mf_fixed);
      std::cout << std::setw(8) << (match ? "YES" : "DIFF");
    } else {
      std::cout << std::setw(8) << "---";
    }

    std::cout << "\n";
    std::cout.flush();
  }

  std::cout << std::string(86, '-') << "\n";
  std::cout << "MF/Gurobi = Gurobi time / CPU max-flow time (>1.0 = max-flow wins)\n";
  std::cout << "GurKern   = vertices remaining after Gurobi LP kernelization\n";
  std::cout << "MFKern    = vertices remaining after CPU max-flow kernelization\n";
  std::cout << "Match     = both fixed same number of variables\n";
  std::cout << "            (DIFF is OK — different valid LP optima)\n";

  return 0;
}