// Measures how much ANY minimum cut could possibly decide
//
// On some instances the max-flow kernelizer resolves 0 variables while the Gurobi LP kernelizer resolves up to 100%
// cut_audit already showed that the max-flow cut is a genuine minimum cut, and that different min-cuts decide
// different amounts (SEEDED 0, FRESH 0, STALE 5 on d_m2000)
//
// In the residual graph left behind by the max-flow, two nodes in the same strongly connected component are each reachable from the other along edges that still have capacity
// A cut placing them on opposite sides would leave a residual edge crossing it, which no minimum cut can do
// That gives a test:
//
//   for a CCG vertex v with copies v_L and v_R in the double cover,
//     SCC(v_L) == SCC(v_R)  ->  they are on the same side in EVERY min-cut, so v is undecidable by any min-cut method
//     SCC(v_L) != SCC(v_R)  ->  SOME min-cut separates them, so v is decidable in principle
//
// Counting the second case gives an exact UPPER BOUND on what any min-cut based kernelizer can achieve on this instance. Comparing that bound with what the
// current rule actually achieves says whether the gap is a selection problem (fixable) or a structural one (fundamental)
//
// Build:
//   g++ -std=c++17 -O2 -I. apps/mincut_lattice.cpp -o mincut_lattice -lopenblas
// Usage:
//   ./mincut_lattice [--format d|u|auto] <file.wcsp|file.uai>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <stack>
#include <stdexcept>
#include <string>
#include <vector>
#include <new>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "src/integration/KernelizerMaxflow.h"

using namespace maxflow;
typedef ConstraintCompositeGraph<>::variable_id_t vid_t;
typedef ConstraintCompositeGraph<>::graph_t graph_t;
typedef boost::graph_traits<graph_t>::vertex_descriptor vertex_t;

//  Iterative Tarjan. The double cover reaches tens of millions of nodes, so a recursive implementation would overflow the stack
static void strongly_connected(const flow_network<cap_t>& net, std::vector<int>& comp, int& ncomp) {
  const int N = net.num_nodes;
  comp.assign(N, -1);
  std::vector<int> index(N, -1), low(N, 0), onstk(N, 0);
  std::vector<int> stk;
  stk.reserve(N);
  int idx = 0;
  ncomp = 0;

  //  Explicit DFS stack: (vertex, next edge slot to examine)
  std::vector<std::pair<vertex_id_t, edge_id_t>> call;

  for (vertex_id_t root = 0; root < N; root++) {
    if (index[root] != -1) continue;
    call.push_back({root, net.offset[root]});
    index[root] = low[root] = idx++;
    stk.push_back(root);
    onstk[root] = 1;

    while (!call.empty()) {
      vertex_id_t u = call.back().first;
      edge_id_t& e = call.back().second;

      if (e < net.offset[u + 1]) {
        edge_id_t cur = e++;
        //  Only residual edges exist in the residual graph
        if (net.residual_capacity[cur] <= MAXFLOW_EPSILON) continue;
        vertex_id_t v = net.edge_dst[cur];
        if (index[v] == -1) {
          index[v] = low[v] = idx++;
          stk.push_back(v);
          onstk[v] = 1;
          call.push_back({v, net.offset[v]});
        } else if (onstk[v]) {
          if (index[v] < low[u]) low[u] = index[v];
        }
      } else {
        //  u is finished: close a component if it is a root
        if (low[u] == index[u]) {
          while (true) {
            vertex_id_t w = stk.back();
            stk.pop_back();
            onstk[w] = 0;
            comp[w] = ncomp;
            if (w == u) break;
          }
          ncomp++;
        }
        call.pop_back();
        if (!call.empty()) {
          vertex_id_t p = call.back().first;
          if (low[u] < low[p]) low[p] = low[u];
        }
      }
    }
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " [--format d|u|auto] <file.wcsp|file.uai>\n";
    return 1;
  }

  //  Format is chosen by extension unless --format overrides it.
  std::string fmt = "auto";
  const char* path = nullptr;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--format" && i + 1 < argc) fmt = argv[++i];
    else path = argv[i];
  }
  if (path == nullptr) { std::cerr << "no input file given\n"; return 1; }

  WCSPInstance<>::Format fformat = WCSPInstance<>::Format::DIMACS;
  {
    std::string f = fmt;
    if (f == "auto") {
      std::string p(path);
      f = (p.size() >= 4 && p.compare(p.size() - 4, 4, ".uai") == 0) ? "u" : "d";
    }
    if (f == "u" || f == "uai")                        fformat = WCSPInstance<>::Format::UAI;
    else if (f == "d" || f == "dimacs" || f == "wcsp") fformat = WCSPInstance<>::Format::DIMACS;
    else { std::cerr << "bad --format: " << fmt << " (expected d, u or auto)\n"; return 1; }
  }

  std::ifstream in(path);
  if (!in) { std::cerr << "cannot open " << path << "\n"; return 2; }

  //  reasons an instance can be unusable
  //    domain_error  -> a variable is not Boolean, or is pinned to one value  (rc 5)
  //    bad_alloc     -> a cost table or polynomial is too large to represent  (rc 6)
  //  The second shows up as bad_array_new_length: at arity 77 the code computes 2^77
  //  as an array length, which overflows and is rejected before any allocation is
  //  attempted. Measured arity on this benchmark reaches 580.
  std::unique_ptr<WCSPInstance<>> instp;
  try {
    instp.reset(new WCSPInstance<>(in, fformat));
  } catch (const std::domain_error& e) {
    std::cout << "skipped -- " << e.what() << "\n";
    return 5;
  } catch (const std::bad_alloc&) {
    std::cout << "skipped -- cost table too large to represent - high arity\n";
    return 6;
  } catch (const std::length_error&) {
    std::cout << "skipped -- cost table exceeds the container limit - high arity\n";
    return 6;
  } catch (const std::exception& e) {
    std::cout << "skipped -- parse failed: " << e.what() << "\n";
    return 6;
  }
  WCSPInstance<>& inst = *instp;

  //  toPolynomial expands an arity-k constraint into up to 2^k terms. 
  //  immediate exhaustion. Normal to the CCG construction, not a defect and is repair
  //  but it must not abort the process and take a whole batch run with it
  ConstraintCompositeGraph<> ccg;
  try {
    WCSPInstance<>::constraint_t::Polynomial p;
    for (const auto& c : inst.getConstraints()) c.toPolynomial(p);
    ccg.addPolynomial(p);
  } catch (const std::bad_alloc&) {
    std::cout << "skipped -- out of memory building the CCG (constraint arity too high)\n";
    return 6;
  } catch (const std::exception& e) {
    std::cout << "skipped -- CCG construction failed: " << e.what() << "\n";
    return 6;
  }
  std::map<vid_t, bool> pre;
  ccg.simplify(pre);
  graph_t g = *ccg.getGraph();

  //  Rebuild the double cover exactly as KernelizerMaxflow does
  auto vertex_weight_map = boost::get(boost::vertex_weight, g);
  std::vector<vertex_t> ccg_vertices;
  std::map<vertex_t, int> vertex_index;
  boost::graph_traits<graph_t>::vertex_iterator vi, vi_end;
  std::tie(vi, vi_end) = boost::vertices(g);
  for (auto it = vi; it != vi_end; it++) {
    vertex_index[*it] = static_cast<int>(ccg_vertices.size());
    ccg_vertices.push_back(*it);
  }
  const int n = static_cast<int>(ccg_vertices.size());
  if (n == 0) { std::cout << "empty CCG -- nothing to analyse\n"; return 0; }

  const int flow_n = 2 * n + 2;
  const vertex_id_t src = 0, snk = 2 * n + 1;

  cap_t total_weight = 0;
  cap_t inf_cap = cap_t(1);
  bool w_integral = true;
  for (int i = 0; i < n; i++) {
    cap_t w = static_cast<cap_t>(vertex_weight_map[ccg_vertices[i]]);
    total_weight += w;
    inf_cap += w;
    if (!is_integral(w)) w_integral = false;
  }

  std::vector<edge<cap_t>> fe;
  fe.reserve(2 * n + 4 * n);
  for (int i = 0; i < n; i++) {
    cap_t w = static_cast<cap_t>(vertex_weight_map[ccg_vertices[i]]);
    fe.push_back({src, i + 1, w});
    fe.push_back({n + i + 1, snk, w});
  }
  boost::graph_traits<graph_t>::edge_iterator ei, ei_end;
  std::tie(ei, ei_end) = boost::edges(g);
  for (auto it = ei; it != ei_end; it++) {
    int u = vertex_index[boost::source(*it, g)];
    int v = vertex_index[boost::target(*it, g)];
    fe.push_back({u + 1, n + v + 1, inf_cap});
    fe.push_back({v + 1, n + u + 1, inf_cap});
  }

  flow_network<cap_t> net;
  net.build_from_edges(flow_n, src, snk, fe);
  static_max_flow_solver<cap_t> solver(net);
  cap_t F = solver.solve();

  std::cout << "=== INSTANCE ===\n";
  std::cout << "  file                 : " << path << "\n";
  std::cout << "  format               : " << (fformat == WCSPInstance<>::Format::UAI ? "uai" : "dimacs") << "\n";
  std::cout << "  CCG vertices n       : " << n << "\n";
  std::cout << "  double cover nodes   : " << flow_n << "\n";
  std::cout << "  double cover edges   : " << net.num_edges << "\n";
  std::cout << "  total vertex weight W: " << total_weight << "\n";
  std::cout << "  weights integral?    : " << (w_integral ? "yes" : "no") << "\n";
  std::cout << "  max-flow             : " << F << "\n";
  std::cout << "  LP optimum (= F/2)   : " << (F / 2) << "\n\n";

  //  The all-half assignment costs W/2. If that equals the LP optimum F/2, then all-half is itself optimal and the symmetric min-cut is a valid answer
  std::cout << "=== ALL-HALF TEST ===\n";
  bool allhalf = (F >= total_weight - MAXFLOW_EPSILON);
  std::cout << "  max-flow vs W        : " << F << " vs " << total_weight << "\n";
  std::cout << "  all-half optimal?    : " << (allhalf ? "YES" : "no") << "\n";
  std::cout << "  => " << (allhalf
      ? "the symmetric (all-0.5) solution is an optimal LP solution here"
      : "the all-half solution is NOT optimal; a min-cut must decide something")
      << "\n\n";

  //  Nodes sharing a strongly connected component of the residual graph fall on the same side of EVERY minimum cut, so counting the pairs that do NOT share one
  //  gives the exact ceiling for any min-cut based method
  std::vector<int> comp;
  int ncomp = 0;
  strongly_connected(net, comp, ncomp);

  long same = 0, diff = 0;
  for (int i = 0; i < n; i++) {
    if (comp[i + 1] == comp[n + i + 1]) same++; else diff++;
  }

  std::cout << "=== MIN-CUT CEILING ANALYSIS ===\n";
  std::cout << "  SCCs in residual graph      : " << ncomp << "\n";
  std::cout << "  pairs v_L,v_R in SAME SCC   : " << same
            << "  (" << (100.0 * same / n) << "%)  <- undecidable by ANY min-cut\n";
  std::cout << "  pairs in DIFFERENT SCCs     : " << diff
            << "  (" << (100.0 * diff / n) << "%)  <- decidable by SOME min-cut\n\n";

  //  What the rule currently in use actually achieves, for comparison.
  long decided_now = 0;
  for (int i = 0; i < n; i++) {
    bool L = solver.is_on_source_side(i + 1);
    bool R = solver.is_on_source_side(n + i + 1);
    if (L != R) decided_now++;
  }

  std::cout << "=== GAP ===\n";
  std::cout << "  decided by current rule     : " << decided_now
            << "  (" << (100.0 * decided_now / n) << "%)\n";
  std::cout << "  upper bound (any min-cut)   : " << diff
            << "  (" << (100.0 * diff / n) << "%)\n";
  std::cout << "  recoverable by better choice: " << (diff - decided_now) << "\n\n";

  std::cout << "  VERDICT: ";
  if (diff == 0) {
    std::cout << "STRUCTURAL. No min-cut can decide anything on this instance.\n"
                 "           Any gap against the LP kernelizer cannot be closed by\n"
                 "           choosing a different cut.\n";
  } else if (diff - decided_now > 0) {
    std::cout << "SELECTION. " << (diff - decided_now) << " more variables are\n"
                 "           decidable by a different min-cut. This is fixable by\n"
                 "           choosing a better closed set in the SCC DAG.\n";
  } else {
    std::cout << "OPTIMAL. The current rule already attains the upper bound.\n";
  }
  return 0;
}