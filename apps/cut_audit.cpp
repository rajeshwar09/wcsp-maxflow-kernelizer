// checks that the cut KernelizerMaxflow uses is a real minimum cut, a gate test for the NT classification
// builds the same double cover network, solves it with the CPU solver and then looks at several source side choices:
//   STALE      : the heights as the solver left them (from the BFS at the top of the last loop, before the last push and remove)
//   FRESH      : a backward BFS from the sink over the final residual graph
//   SEEDED     : forward residual reachability from the source and from every vertex that still has excess (the rule in use now)
//   PHASE2-MIN : after the preflow is turned into a real flow, reachable from the source
//   PHASE2-MAX : after the preflow is turned into a real flow, cannot reach the sink
// for each one it prints the cut capacity and the residual edges that cross from the source side to the sink side
// a real min cut has capacity = max-flow and no crossing residual, else the NT classification from it is not justified
// it also checks that every capacity, residual and excess is a whole number (the integrality audit)
//
// build: g++ -std=c++17 -O2 -I. apps/cut_audit.cpp -o cut_audit -lopenblas
// usage: ./cut_audit <file.wcsp>

#include <cmath>
#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <queue>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "src/integration/KernelizerMaxflow.h"

using namespace maxflow;
typedef ConstraintCompositeGraph<>::variable_id_t vid_t;
typedef ConstraintCompositeGraph<>::graph_t graph_t;
typedef boost::graph_traits<graph_t>::vertex_descriptor vertex_t;

// [REPORT] prints one source side choice, src_side[v] != 0 means v is on the source side
static void report(const char* label, const flow_network<cap_t>& net,
                   const std::vector<char>& src_side, cap_t F, int n) {
  cap_t cap_sum = 0;      // capacity of the edges from the source side to the sink side
  cap_t leak_amt = 0;     // residual still left on those same edges
  long  leak_cnt = 0;
  long  side_sz  = 0;

  for (vertex_id_t u = 0; u < net.num_nodes; u++) {
    if (src_side[u]) side_sz++;
    for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
      vertex_id_t v = net.edge_dst[e];
      if (src_side[u] && !src_side[v]) {
        cap_sum += net.capacity[e];
        if (net.residual_capacity[e] > MAXFLOW_EPSILON) {
          leak_cnt++;
          leak_amt += net.residual_capacity[e];
        }
      }
    }
  }

  // [REPORT] NT classification for this choice
  long dec0 = 0, dec1 = 0, undec = 0;
  for (int i = 0; i < n; i++) {
    bool L = src_side[i + 1] != 0;
    bool R = src_side[n + i + 1] != 0;
    if (L && !R) dec0++;
    else if (!L && R) dec1++;
    else undec++;
  }

  std::cout << "--- " << label << " ---\n";
  std::cout << "  |source side|        : " << side_sz << " of " << net.num_nodes << "\n";
  std::cout << "  cut capacity         : " << cap_sum << "\n";
  std::cout << "  max-flow value       : " << F << "\n";
  std::cout << "  capacity - maxflow   : " << (cap_sum - F) << "\n";
  std::cout << "  crossing residual    : " << leak_cnt << " edges, " << leak_amt << " units\n";
  std::cout << "  VALID MIN-CUT?       : "
            << (leak_cnt == 0 ? "saturated" : "NO - leaks") << "\n";
  std::cout << "  NT decided (x=0)     : " << dec0 << "\n";
  std::cout << "  NT decided (x=1)     : " << dec1 << "\n";
  std::cout << "  NT decided TOTAL     : " << (dec0 + dec1) << "\n";
  std::cout << "  NT undecided (0.5)   : " << undec << "\n\n";
}

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: " << argv[0] << " <file.wcsp>\n"; return 1; }

  std::ifstream in(argv[1]);
  if (!in) { std::cerr << "cannot open " << argv[1] << "\n"; return 2; }
  WCSPInstance<> inst(in, WCSPInstance<>::Format::DIMACS);
  ConstraintCompositeGraph<> ccg;
  WCSPInstance<>::constraint_t::Polynomial p;
  for (const auto& c : inst.getConstraints()) c.toPolynomial(p);
  ccg.addPolynomial(p);
  std::map<vid_t, bool> pre;
  ccg.simplify(pre);
  graph_t g = *ccg.getGraph();

  std::cout << "pre-resolved by ccg.simplify(): " << pre.size() << "\n\n";

  // [FLOW] the same double cover network as KernelizerMaxflow builds
  auto vertex_id_map     = boost::get(boost::vertex_name, g);
  auto vertex_weight_map = boost::get(boost::vertex_weight, g);
  (void)vertex_id_map;

  std::vector<vertex_t> ccg_vertices;
  std::map<vertex_t, int> vertex_index;
  boost::graph_traits<graph_t>::vertex_iterator vi, vi_end;
  std::tie(vi, vi_end) = boost::vertices(g);
  for (auto it = vi; it != vi_end; it++) {
    vertex_index[*it] = static_cast<int>(ccg_vertices.size());
    ccg_vertices.push_back(*it);
  }
  int n = static_cast<int>(ccg_vertices.size());
  if (n == 0) { std::cout << "empty CCG\n"; return 0; }

  int flow_n = 2 * n + 2;
  vertex_id_t flow_source = 0;
  vertex_id_t flow_sink = 2 * n + 1;

  cap_t inf_cap = cap_t(1);
  for (int i = 0; i < n; i++)
    inf_cap += static_cast<cap_t>(vertex_weight_map[ccg_vertices[i]]);

  std::vector<edge<cap_t>> flow_edges;
  flow_edges.reserve(2 * n + 4 * n);
  for (int i = 0; i < n; i++) {
    cap_t w = static_cast<cap_t>(vertex_weight_map[ccg_vertices[i]]);
    flow_edges.push_back({flow_source, i + 1, w});
    flow_edges.push_back({n + i + 1, flow_sink, w});
  }
  boost::graph_traits<graph_t>::edge_iterator ei, ei_end;
  std::tie(ei, ei_end) = boost::edges(g);
  for (auto it = ei; it != ei_end; it++) {
    int u_idx = vertex_index[boost::source(*it, g)];
    int v_idx = vertex_index[boost::target(*it, g)];
    flow_edges.push_back({u_idx + 1, n + v_idx + 1, inf_cap});
    flow_edges.push_back({v_idx + 1, n + u_idx + 1, inf_cap});
  }

  flow_network<cap_t> net;
  net.build_from_edges(flow_n, flow_source, flow_sink, flow_edges);
  std::cout << "double cover: " << flow_n << " nodes, "
            << net.num_edges << " half-edges, n=" << n << "\n\n";

  static_max_flow_solver<cap_t> solver(net);
  cap_t F = solver.solve();

  // [EXCESS] excess of every vertex worked out again from the final residuals
  std::vector<cap_t> ex(flow_n, 0);
  {
    for (vertex_id_t u = 0; u < net.num_nodes; u++)
      for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
        cap_t f = net.capacity[e] - net.residual_capacity[e];
        if (f > 0) { ex[u] -= f; ex[net.edge_dst[e]] += f; }
      }
    long stranded = 0; cap_t stranded_amt = 0;
    for (vertex_id_t v = 0; v < net.num_nodes; v++)
      if (v != net.source && v != net.sink && ex[v] > MAXFLOW_EPSILON) {
        stranded++; stranded_amt += ex[v];
      }
    std::cout << "max-flow = " << F << "\n";
    std::cout << "stranded excess: " << stranded << " vertices, "
              << stranded_amt << " units\n";
    std::cout << "net flow out of source = " << -ex[net.source] << "\n\n";
  }

  // [INTEGRAL] WCSP costs are whole numbers and toPolynomial is a Mobius transform (sums and differences of whole numbers),
  // so every capacity should be a whole number, then a whole number max flow exists and every residual, the flow value
  // and the cut capacity should also be exact whole numbers, any drift means MAXFLOW_EPSILON is hiding a rounding error
  // the 0.5 values of NT are not this kind of fraction, the bipartite cut is integral (Koenig, total unimodularity),
  // a 0.5 only means the two copies of a variable ended on the same side
  {
    auto frac = [](cap_t x) {
      cap_t r = x - std::floor(x + 0.5);
      return r < 0 ? -r : r;
    };
    long cap_bad = 0, res_bad = 0, ex_bad = 0;
    cap_t cap_worst = 0, res_worst = 0, ex_worst = 0;

    for (edge_id_t e = 0; e < net.num_edges; e++) {
      cap_t fc = frac(net.capacity[e]);
      if (fc > 1e-9) { cap_bad++; if (fc > cap_worst) cap_worst = fc; }
      cap_t fr = frac(net.residual_capacity[e]);
      if (fr > 1e-9) { res_bad++; if (fr > res_worst) res_worst = fr; }
    }
    for (vertex_id_t v = 0; v < flow_n; v++) {
      cap_t fe = frac(ex[v]);
      if (fe > 1e-9) { ex_bad++; if (fe > ex_worst) ex_worst = fe; }
    }

    // [INTEGRAL] residuals that are exactly 0 against ones only below the epsilon, with clean arithmetic a full edge is exactly 0
    long exact_zero = 0, near_zero = 0;
    for (edge_id_t e = 0; e < net.num_edges; e++) {
      if (net.residual_capacity[e] == cap_t(0)) {
        exact_zero++;
      } else if (net.residual_capacity[e] <= MAXFLOW_EPSILON) {
        near_zero++;
      }
    }

    bool clean = (cap_bad + res_bad + ex_bad) == 0 && frac(F) <= 1e-9 && near_zero == 0;

    std::cout << "=== INTEGRALITY AUDIT ===\n";
    std::cout << "  non-integral capacities : " << cap_bad << " of " << net.num_edges
              << "   (worst " << cap_worst << ")\n";
    std::cout << "  non-integral residuals  : " << res_bad << " of " << net.num_edges
              << "   (worst " << res_worst << ")\n";
    std::cout << "  non-integral excess     : " << ex_bad << " of " << flow_n
              << "   (worst " << ex_worst << ")\n";
    std::cout << "  max-flow integral?      : " << (frac(F) <= 1e-9 ? "yes" : "NO") << "\n";
    std::cout << "  residual exactly 0      : " << exact_zero << "\n";
    std::cout << "  residual 0 < x <= eps   : " << near_zero
              << (near_zero ? "   <-- epsilon is hiding drift" : "") << "\n";
    std::cout << "  => " << (clean
        ? "CLEAN: arithmetic is exact; cap_t can be int64_t and MAXFLOW_EPSILON removed"
        : "DRIFT: floating point error present; keep MAXFLOW_EPSILON and investigate")
              << "\n\n";
  }

  // [STALE] heights as the solver left them
  {
    const std::vector<int>& h = solver.heights();
    std::vector<char> side(flow_n, 0);
    for (int v = 0; v < flow_n; v++) side[v] = (h[v] >= flow_n) ? 1 : 0;
    report("STALE heights (what KernelizerMaxflow uses today)", net, side, F, n);
  }

  // [FRESH] backward BFS from the sink over the final residuals
  {
    std::vector<int> h(flow_n, flow_n);
    h[net.sink] = 0;
    std::queue<vertex_id_t> q;
    q.push(net.sink);
    while (!q.empty()) {
      vertex_id_t u = q.front(); q.pop();
      for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
        vertex_id_t v = net.edge_dst[e];
        if (net.residual_capacity[net.reverse_index[e]] > MAXFLOW_EPSILON
            && h[v] == flow_n) {
          h[v] = h[u] + 1;
          q.push(v);
        }
      }
    }
    std::vector<char> side(flow_n, 0);
    for (int v = 0; v < flow_n; v++) side[v] = (h[v] >= flow_n) ? 1 : 0;
    report("FRESH backward BFS (what the final relabel computes)", net, side, F, n);
  }

  // [SEEDED] forward residual reachability from the source together with every vertex that still has excess
  // no residual edge leaves this set (it is closed under reachability) and it holds the source and all stranded excess,
  // so its capacity equals the max-flow, it is the smallest such set so it decides at least as many variables as any other min cut
  {
    std::vector<char> side(flow_n, 0);
    std::vector<vertex_id_t> stack;
    side[net.source] = 1;
    stack.push_back(net.source);
    long seeds = 1;
    for (vertex_id_t v = 0; v < net.num_nodes; v++) {
      if (v != net.source && v != net.sink && ex[v] > MAXFLOW_EPSILON && !side[v]) {
        side[v] = 1;
        stack.push_back(v);
        seeds++;
      }
    }
    while (!stack.empty()) {
      vertex_id_t u = stack.back(); stack.pop_back();
      for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
        vertex_id_t v = net.edge_dst[e];
        if (net.residual_capacity[e] > MAXFLOW_EPSILON && !side[v]) {
          side[v] = 1;
          stack.push_back(v);
        }
      }
    }
    std::cout << "(SEEDED started from " << seeds << " seed vertices)\n";
    report("SEEDED fwd BFS from source + excess (proposed fix)", net, side, F, n);
  }

  // [PHASE2] turn the preflow into a real flow by sending every unit of stranded excess back to the source
  // Lemma 3.5 says a residual path v -> s exists for each such v and these paths stay inside the source side,
  // so the cut cannot move, but reachability to the sink can change and that is what is tested here
  // how: one BFS from s over reversed residual edges gives every vertex that can reach s a parent pointer towards s,
  // the excess is drained along these chains, when a chain edge is full the BFS is done again
  // this changes net.residual_capacity, so it must run after the STALE, FRESH and SEEDED reports
  {
    std::vector<edge_id_t> parent(flow_n, -1);
    std::vector<char> seen(flow_n, 0);
    long rounds = 0;
    cap_t returned_total = 0;
    bool progress = true;

    while (progress && rounds < 200) {
      progress = false;
      rounds++;

      // [PHASE2] BFS from the source over reversed residual edges, parent[u] is the half edge u -> w that takes u towards s
      std::fill(parent.begin(), parent.end(), -1);
      std::fill(seen.begin(), seen.end(), 0);
      std::queue<vertex_id_t> q;
      seen[net.source] = 1;
      q.push(net.source);
      while (!q.empty()) {
        vertex_id_t w = q.front(); q.pop();
        for (edge_id_t e = net.offset[w]; e < net.offset[w + 1]; e++) {
          vertex_id_t u = net.edge_dst[e];
          edge_id_t back = net.reverse_index[e];   // the half edge u -> w
          if (!seen[u] && net.residual_capacity[back] > MAXFLOW_EPSILON) {
            seen[u] = 1;
            parent[u] = back;
            q.push(u);
          }
        }
      }

      for (vertex_id_t v = 0; v < net.num_nodes; v++) {
        if (v == net.source || v == net.sink) continue;
        if (ex[v] <= MAXFLOW_EPSILON || !seen[v]) continue;

        // [PHASE2] smallest residual on the chain from v to s
        cap_t b = ex[v];
        vertex_id_t u = v;
        while (u != net.source && b > MAXFLOW_EPSILON) {
          edge_id_t e = parent[u];
          if (e < 0) { b = 0; break; }
          if (net.residual_capacity[e] < b) b = net.residual_capacity[e];
          u = net.edge_dst[e];
        }
        if (b <= MAXFLOW_EPSILON) continue;

        // [PHASE2] send b units back along the same chain
        u = v;
        while (u != net.source) {
          edge_id_t e = parent[u];
          net.residual_capacity[e] -= b;
          net.residual_capacity[net.reverse_index[e]] += b;
          u = net.edge_dst[e];
        }
        ex[v] -= b;
        ex[net.source] += b;
        returned_total += b;
        progress = true;
      }
    }

    long left_cnt = 0; cap_t left_amt = 0;
    for (vertex_id_t v = 0; v < net.num_nodes; v++)
      if (v != net.source && v != net.sink && ex[v] > MAXFLOW_EPSILON) {
        left_cnt++; left_amt += ex[v];
      }

    std::cout << "=== PHASE 2 (preflow -> flow) ===\n";
    std::cout << "  BFS rounds           : " << rounds << "\n";
    std::cout << "  excess returned to s : " << returned_total << " units\n";
    std::cout << "  excess still stranded: " << left_cnt << " vertices, "
              << left_amt << " units\n";
    std::cout << "  net flow out of source = " << -ex[net.source]
              << "  (should now equal max-flow " << F << ")\n\n";

    // [PHASE2] MIN side: reachable from the source in the residual graph
    {
      std::vector<char> side(flow_n, 0);
      std::vector<vertex_id_t> stack;
      side[net.source] = 1;
      stack.push_back(net.source);
      while (!stack.empty()) {
        vertex_id_t u = stack.back(); stack.pop_back();
        for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
          vertex_id_t v = net.edge_dst[e];
          if (net.residual_capacity[e] > MAXFLOW_EPSILON && !side[v]) {
            side[v] = 1;
            stack.push_back(v);
          }
        }
      }
      report("PHASE2-MIN reachable from source (predict: same as SEEDED)", net, side, F, n);
    }

    // [PHASE2] MAX side: cannot reach the sink in the residual graph
    {
      std::vector<int> h(flow_n, flow_n);
      h[net.sink] = 0;
      std::queue<vertex_id_t> q2;
      q2.push(net.sink);
      while (!q2.empty()) {
        vertex_id_t u = q2.front(); q2.pop();
        for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
          vertex_id_t v = net.edge_dst[e];
          if (net.residual_capacity[net.reverse_index[e]] > MAXFLOW_EPSILON
              && h[v] == flow_n) {
            h[v] = h[u] + 1;
            q2.push(v);
          }
        }
      }
      std::vector<char> side(flow_n, 0);
      for (int v = 0; v < flow_n; v++) side[v] = (h[v] >= flow_n) ? 1 : 0;
      report("PHASE2-MAX cannot reach sink (the real unknown)", net, side, F, n);
    }
  }

  return 0;
}