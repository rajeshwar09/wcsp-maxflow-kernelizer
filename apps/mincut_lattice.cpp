// finds the most that any min-cut kernelizer could decide on one instance (the ceiling)
// after the max-flow, two nodes in the same strongly connected component (SCC) of the residual graph can reach each other
// along edges that still have capacity, so a cut that puts them on different sides would leave a residual edge crossing it
// no minimum cut can do that, which gives this test for a CCG vertex v with copies v_L and v_R in the double cover:
//   SCC(v_L) == SCC(v_R) : same side in every min cut, no min-cut method can decide v
//   SCC(v_L) != SCC(v_R) : some min cut separates them, v can be decided
// counting the second case gives the ceiling, comparing it with what our cut rule decides tells if the gap is
// a selection problem (a better cut exists) or a structural one (nothing more to win)
//
// build: g++ -std=c++17 -O2 -I. apps/mincut_lattice.cpp -o mincut_lattice -lopenblas
// usage: ./mincut_lattice [--format d|u|auto] [--max-arity N] <file.wcsp|file.uai>
// exit codes: 0 done, 1 bad arguments, 2 file not readable, 5 non Boolean or domain-1 variable, 6 arity too high
// the VERDICT and SCC lines are read by scripts/run_ceiling.sh

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
#include <cstdlib>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "src/integration/KernelizerMaxflow.h"

using namespace maxflow;
typedef ConstraintCompositeGraph<>::variable_id_t vid_t;
typedef ConstraintCompositeGraph<>::graph_t graph_t;
typedef boost::graph_traits<graph_t>::vertex_descriptor vertex_t;

// [ARITY] largest constraint scope of the instance
template <class Inst>
static size_t max_constraint_arity(const Inst& inst) {
  size_t m = 0;
  for (const auto& c : inst.getConstraints()) {
    size_t a = c.getVariables().size();
    if (a > m) m = a;
  }
  return m;
}

// [SCC] Tarjan without recursion, the double cover can have tens of millions of nodes and recursion would overflow the stack
static void strongly_connected(const flow_network<cap_t>& net, std::vector<int>& comp, int& ncomp) {
  const int N = net.num_nodes;
  comp.assign(N, -1);
  std::vector<int> index(N, -1), low(N, 0), onstk(N, 0);
  std::vector<int> stk;
  stk.reserve(N);
  int idx = 0;
  ncomp = 0;

  // [SCC] our own DFS stack of (vertex, next edge to look at)
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
        // [SCC] only edges with capacity left belong to the residual graph
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
        // [SCC] u is finished, if it is a root it closes one component
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

  // [ARGS] the format comes from the file extension unless --format is given
  std::string fmt = "auto";
  size_t max_arity = 15;
  const char* path = nullptr;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--format" && i + 1 < argc) fmt = argv[++i];
    else if (std::string(argv[i]) == "--max-arity" && i + 1 < argc)
      max_arity = static_cast<size_t>(std::atol(argv[++i]));
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

  // [PARSE] reasons an instance cannot be used
  //   domain_error : a variable is not Boolean or has only one value       (code 5)
  //   bad_alloc    : a cost table or polynomial is too large to represent  (code 6)
  // the second one shows up as bad_array_new_length, at arity 77 the length 2^77 overflows
  // and is refused before any memory is taken, the arity on this benchmark goes up to 580
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

  // [ARITY] checked before toPolynomial, which needs 2^(2*arity) doubles
  {
    size_t arity = max_constraint_arity(inst);
    if (arity > max_arity) {
      std::cout << "skipped -- constraint arity " << arity << " exceeds the limit of "
                << max_arity << " (CCG would allocate 2^(2*arity) doubles)\n";
      return 6;
    }
  }

  // [CCG] build and simplify the constraint composite graph
  ConstraintCompositeGraph<> ccg;
  WCSPInstance<>::constraint_t::Polynomial p;
  for (const auto& c : inst.getConstraints()) c.toPolynomial(p);
  ccg.addPolynomial(p);
  std::map<vid_t, bool> pre;
  ccg.simplify(pre);
  graph_t g = *ccg.getGraph();

  // [FLOW] the same double cover network as KernelizerMaxflow builds
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

  // [OUTPUT] instance and flow numbers
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

  // [ALLHALF] the all-half assignment costs W/2, if that equals the LP optimum F/2 then all-half is optimal and the symmetric cut is a valid answer
  std::cout << "=== ALL-HALF TEST ===\n";
  bool allhalf = (F >= total_weight - MAXFLOW_EPSILON);
  std::cout << "  max-flow vs W        : " << F << " vs " << total_weight << "\n";
  std::cout << "  all-half optimal?    : " << (allhalf ? "YES" : "no") << "\n";
  std::cout << "  => " << (allhalf
      ? "the symmetric (all-0.5) solution is an optimal LP solution here"
      : "the all-half solution is NOT optimal; a min-cut must decide something")
      << "\n\n";

  // [SCC] pairs in the same SCC are on the same side of every min cut, the pairs in different SCCs give the ceiling
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

  // [GAP] what our cut rule decides, to compare with the ceiling
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

  // [VERDICT] STRUCTURAL, SELECTION or OPTIMAL
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
