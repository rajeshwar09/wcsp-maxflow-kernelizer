// Audits the min-cut that KernelizerMaxflow actually uses.
//
// Builds the same bipartite double-cover network, solves it with the CPU
// solver, then evaluates TWO candidate source-side definitions:
//
//   STALE : height[] as left by the solver (heights from the BFS at the top of
//           the last loop iteration, i.e. BEFORE the final push/remove)
//   FRESH : a backward BFS from the sink over the FINAL residual graph
//
// For each it reports the cut capacity and any residual edge crossing from
// source side to sink side. A genuine min-cut has capacity == max-flow and
// zero crossing residual. Anything else is not a min-cut, and the NT
// classification derived from it is not justified.
//
// Usage: ./cut_audit <file.wcsp>

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

// Report one candidate partition. src_side[v] != 0 means v is on the source side.
static void report(const char* label, const flow_network<cap_t>& net,
                   const std::vector<char>& src_side, cap_t F, int n) {
  cap_t cap_sum = 0;      // capacity of edges crossing source side -> sink side
  cap_t leak_amt = 0;     // residual still available across that same boundary
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

  // NT classification under this partition
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

  // --- rebuild the double cover exactly as KernelizerMaxflow does ---
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

  // --- preflow health: excess recomputed from the final residuals ---
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

  // --- STALE partition: heights as the solver left them ---
  {
    const std::vector<int>& h = solver.heights();
    std::vector<char> side(flow_n, 0);
    for (int v = 0; v < flow_n; v++) side[v] = (h[v] >= flow_n) ? 1 : 0;
    report("STALE heights (what KernelizerMaxflow uses today)", net, side, F, n);
  }

  // --- FRESH partition: backward BFS from sink over the final residuals ---
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

  // --- SEEDED partition: forward residual reachability from the source
  //     TOGETHER WITH every vertex still holding excess.
  //     No residual edge leaves this set (it is a reachability closure), and it
  //     contains the source and all stranded excess, so its capacity equals the
  //     max-flow. It is the SMALLEST such set, so it decides at least as many
  //     variables as any other min-cut. ---
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

  // ------------------------------------------------------------------
  //  PHASE 2: convert the preflow into a true flow by returning every unit
  //  of stranded excess to the source. Lemma 3.5 guarantees a residual path
  //  v -> s exists for each such v, and those paths lie entirely inside the
  //  source side, so the cut itself cannot move -- but reachability TO THE
  //  SINK can change, which is what we are testing.
  //
  //  Method: one BFS from s over REVERSED residual edges builds a parent
  //  pointer toward s for every vertex that can reach s. Drain along those
  //  parent chains. If a chain edge saturates, rebuild and repeat.
  //  NOTE: this MUTATES net.residual_capacity, so it must run after the
  //  STALE / FRESH / SEEDED reports above.
  // ------------------------------------------------------------------
  {
    std::vector<edge_id_t> parent(flow_n, -1);
    std::vector<char> seen(flow_n, 0);
    long rounds = 0;
    cap_t returned_total = 0;
    bool progress = true;

    while (progress && rounds < 200) {
      progress = false;
      rounds++;

      //  BFS from the source over reversed residual edges.
      //  parent[u] = the half-edge u -> w that u should use to step toward s.
      std::fill(parent.begin(), parent.end(), -1);
      std::fill(seen.begin(), seen.end(), 0);
      std::queue<vertex_id_t> q;
      seen[net.source] = 1;
      q.push(net.source);
      while (!q.empty()) {
        vertex_id_t w = q.front(); q.pop();
        for (edge_id_t e = net.offset[w]; e < net.offset[w + 1]; e++) {
          vertex_id_t u = net.edge_dst[e];
          edge_id_t back = net.reverse_index[e];   //  the half-edge u -> w
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

        //  bottleneck along v -> ... -> s
        cap_t b = ex[v];
        vertex_id_t u = v;
        while (u != net.source && b > MAXFLOW_EPSILON) {
          edge_id_t e = parent[u];
          if (e < 0) { b = 0; break; }
          if (net.residual_capacity[e] < b) b = net.residual_capacity[e];
          u = net.edge_dst[e];
        }
        if (b <= MAXFLOW_EPSILON) continue;

        //  push b units back along the same chain
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

    //  PHASE2-MIN : reachable from the source in the residual graph
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

    //  PHASE2-MAX : cannot reach the sink in the residual graph
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