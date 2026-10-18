// push-relabel max-flow on the CPU, same steps as the GPU version (algorithms 1 to 4 of the paper)
// usage:
//   static_max_flow_solver<cap_t> s(net);
//   cap_t f = s.solve();
//   bool src = s.is_on_source_side(v);

#ifndef MAXFLOW_MAXFLOW_STATIC_H
#define MAXFLOW_MAXFLOW_STATIC_H

#include <algorithm>
#include <limits>
#include <queue>
#include <vector>

#include "src/cpu/graph_csr.h"

namespace maxflow {

  template <typename cap_t>
  class static_max_flow_solver {
    public:
      explicit static_max_flow_solver(flow_network<cap_t>& net)
        : net(net), excess(net.num_nodes, cap_t(0)), height(net.num_nodes, 0) {}

      // [SOLVE] run to the end and return the flow value
      // kernel_cycles = push or relabel steps per vertex in one sweep, 0 means |E|/|V|
      cap_t solve(int kernel_cycles = 0) {
        if (kernel_cycles <= 0) {
          kernel_cycles = std::max(1, net.num_edges / std::max(1, net.num_nodes));
        }

        initialize();
        saturate_source();

        // [LOOP] relabel, push, then cancel steep edges until no vertex is active
        while (has_active_vertex()) {
          global_relabel();
          push_relabel_sweep(kernel_cycles);
          remove_invalid_edges();
        }

        compute_source_side();
        return excess[net.sink];
      }

      // [CUT] true when v is on the source side of the min cut
      bool is_on_source_side(vertex_id_t v) const {
        return source_side[v] != 0;
      }

      // [CUT] heights after the solve, used by cut_audit
      const std::vector<int>& heights() const {
        return height;
      }

    private:
      flow_network<cap_t>& net;
      std::vector<cap_t> excess;
      std::vector<int> height;
      std::vector<char> source_side;

      // [CUT] source side = everything reachable from the source and from vertices still holding excess
      // the solver stops at a preflow, so the excess vertices must be added to get a real min cut
      void compute_source_side() {
        source_side.assign(net.num_nodes, 0);
        std::vector<vertex_id_t> stack;
        source_side[net.source] = 1;
        stack.push_back(net.source);
        for (vertex_id_t v = 0; v < net.num_nodes; v++) {
          if (v != net.source && v != net.sink && excess[v] > MAXFLOW_EPSILON && !source_side[v]) {
            source_side[v] = 1;
            stack.push_back(v);
          }
        }
        // [CUT] depth first search over residual edges
        while (!stack.empty()) {
          vertex_id_t u = stack.back();
          stack.pop_back();
          for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
            vertex_id_t v = net.edge_dst[e];
            if (net.residual_capacity[e] > MAXFLOW_EPSILON && !source_side[v]) {
              source_side[v] = 1;
              stack.push_back(v);
            }
          }
        }
      }

      // [INIT] algorithm 1, residual = capacity, excess and height = 0, source height = |V|
      void initialize() {
        for (edge_id_t e = 0; e < net.num_edges; e++) {
          net.residual_capacity[e] = net.capacity[e];
        }
        std::fill(excess.begin(), excess.end(), cap_t(0));
        std::fill(height.begin(), height.end(), 0);
        height[net.source] = net.num_nodes;
      }

      // [INIT] algorithm 1, push full capacity out of the source
      void saturate_source() {
        vertex_id_t s = net.source;
        for (edge_id_t e = net.offset[s]; e < net.offset[s + 1]; e++) {
          cap_t d = net.capacity[e];
          if (d > MAXFLOW_EPSILON) {
            net.residual_capacity[e] = cap_t(0);
            net.residual_capacity[net.reverse_index[e]] += d;
            excess[net.edge_dst[e]] += d;
            excess[s] -= d;
          }
        }
      }

      // [ACTIVE] a vertex is active when it has excess and its height is below |V|
      bool has_active_vertex() const {
        for (vertex_id_t v = 0; v < net.num_nodes; v++) {
          if (v != net.source && v != net.sink && excess[v] > 0 && height[v] < net.num_nodes) {
            return true;
          }
        }
        return false;
      }

      // [BFS] algorithm 4, heights = distance to the sink, found by a backward BFS with a queue
      void global_relabel() {
        std::fill(height.begin(), height.end(), net.num_nodes);
        height[net.sink] = 0;
        std::queue<vertex_id_t> q;
        q.push(net.sink);
        while (!q.empty()) {
          vertex_id_t u = q.front();
          q.pop();
          // [BFS] v can reach u when the edge v -> u still has residual capacity
          for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
            vertex_id_t v = net.edge_dst[e];
            if (net.residual_capacity[net.reverse_index[e]] > MAXFLOW_EPSILON && height[v] == net.num_nodes) {
              height[v] = height[u] + 1;
              q.push(v);
            }
          }
        }
      }

      // [PUSH] algorithm 2, every active vertex does up to kernel_cycles push or relabel steps
      void push_relabel_sweep(int kernel_cycles) {
        for (vertex_id_t u = 0; u < net.num_nodes; u++) {
          if (u == net.source || u == net.sink) {
            continue;
          }
          for (int cnt = 0; cnt < kernel_cycles; cnt++) {
            if (!(height[u] < net.num_nodes && excess[u] > MAXFLOW_EPSILON)) {
              break;
            }

            // [PUSH] lowest neighbour over residual edges
            int lowest_h = std::numeric_limits<int>::max();
            vertex_id_t v_hat = -1;
            edge_id_t e_hat = -1;
            for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
              if (net.residual_capacity[e] > MAXFLOW_EPSILON && height[net.edge_dst[e]] < lowest_h) {
                lowest_h = height[net.edge_dst[e]];
                v_hat = net.edge_dst[e];
                e_hat = e;
              }
            }
            if (v_hat == -1) {
              break;
            }

            if (height[u] > lowest_h) {
              // [PUSH] send as much as possible down to the lowest neighbour
              cap_t d = std::min(excess[u], net.residual_capacity[e_hat]);
              net.residual_capacity[e_hat] -= d;
              net.residual_capacity[net.reverse_index[e_hat]] += d;
              excess[u] -= d;
              excess[v_hat] += d;
            } else {
              // [RELABEL] lift u just above its lowest neighbour
              height[u] = lowest_h + 1;
            }
          }
        }
      }

      // [REMOVE] algorithm 3, cancel residual on edges where h(u) > h(v) + 1
      void remove_invalid_edges() {
        for (vertex_id_t u = 0; u < net.num_nodes; u++) {
          if (u == net.source || u == net.sink) {
            continue;
          }
          for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
            vertex_id_t v = net.edge_dst[e];
            if (net.residual_capacity[e] > MAXFLOW_EPSILON && height[u] > height[v] + 1) {
              cap_t d = net.residual_capacity[e];
              excess[u] -= d;
              excess[v] += d;
              net.residual_capacity[net.reverse_index[e]] += d;
              net.residual_capacity[e] = cap_t(0);
            }
          }
        }
      }
  };

} // namespace maxflow

#endif // MAXFLOW_MAXFLOW_STATIC_H
