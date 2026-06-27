#ifndef MAXFLOW_MAXFLOW_STATIC_H
#define MAXFLOW_MAXFLOW_STATIC_H

#include <vector>
#include <queue>
#include <limits>
#include <algorithm>

#include "src/cpu/graph_csr.h"

namespace maxflow {
  
  // Static max-flow on the CPU (paper's algorithms)
  // cap_t is the capacity/flow number type (template parameter)
  template <typename cap_t>
  class static_max_flow_solver {
    public:
      // net : the flow network to solve
      explicit static_max_flow_solver(flow_network<cap_t>& net) 
        : net(net), 
          excess(net.num_nodes, cap_t(0)),
          height(net.num_nodes, 0) {}
      
      // Run static max-flow and return the value
      // kernel_cycles = how many push/relabel steps one vertex may do before height is recomputed
      cap_t solve(int kernel_cycles = 0) {
        if (kernel_cycles <= 0) {
          kernel_cycles = std::max(1, net.num_edges / std::max(1, net.num_nodes));
        }

        initialize();
        saturate_source();

        while (has_active_vertex()) {
          global_relabel(); // algorithm 4
          push_relabel_sweep(kernel_cycles); // algorithm 2
          remove_invalid_edges(); // algorithm 3
        }

        return excess[net.sink];
      }

      // after solve(), a vertex is on the SOURCE side of min cut iff it cannot reach sink
      // This will be reused in kernelizer
      bool is_on_source_side(vertex_id_t v) const {
        return height[v] >= net.num_nodes;
      }
      const std::vector<int>& heights() const {
        return height;
      }
    
    private:
      flow_network<cap_t>& net;
      std::vector<cap_t> excess;  // excess[v] = e(v) : flow stuck at v
      std::vector<int> height;    // height[v] = h(v) : push-relabel height label

      // Algorithm 1
      // residuals back to full capacity, excess/height to 0
      void initialize() {
        for (edge_id_t e = 0; e < net.num_edges; e++) {
          net.residual_capacity[e] = net.capacity[e];
        }
        std::fill(excess.begin(), excess.end(), cap_t(0));
        std::fill(height.begin(), height.end(), 0);
        height[net.source] = net.num_nodes;
      }
      
      // Algorithm 1
      // Source push outgoing capacity to its neighbouts
      void saturate_source() {
        vertex_id_t s = net.source;
        for (edge_id_t e = net.offset[s]; e < net.offset[s + 1]; e++) {
          cap_t d = net.capacity[e];
          if (d > cap_t(0)) {
            net.residual_capacity[e] = cap_t(0);
            net.residual_capacity[net.reverse_index[e]] += d;
            excess[net.edge_dst[e]] += d;
            excess[s] -= d;
          }
        }
      }

      // Algorithm 1
      // Check if still an active vertex
      bool has_active_vertex() const {
        for (vertex_id_t v = 0; v < net.num_nodes; v++) {
          if (v != net.source && v != net.sink && excess[v] > 0 && height[v] < net.num_nodes) {
            return true;
          }
        }
        return false;
      }

      // Algorithm 4
      // Global relabel using backward BFS rooted at sink
      // DEVIATION : GPU -> CPU - instead of one kernel launch per layer, using FIFO queue
      void global_relabel() {
        std::fill(height.begin(), height.end(), net.num_nodes);
        height[net.sink] = 0;
        std::queue<vertex_id_t> q;
        q.push(net.sink);
        while(!q.empty()) {
          vertex_id_t u = q.front();
          q.pop();
          
          // For each u->v : vertex v can step to u iff v->u still has residual capacity
          for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
            vertex_id_t v = net.edge_dst[e];
            if (net.residual_capacity[net.reverse_index[e]] > cap_t(0) && height[v] == net.num_nodes) {
              height[v] = height[u] + 1;
              q.push(v);
            }
          }
        }
      }

      // Algorithm 2 : One push-relabel pass (performed kernel_cycles times)
      void push_relabel_sweep(int kernel_cycles) {
        for (vertex_id_t u = 0; u < net.num_nodes; u++) {
          if (u == net.source || u == net.sink) {
            continue;
          }

          for (int cnt = 0; cnt < kernel_cycles; cnt++) {
            if (!(height[u] < net.num_nodes && excess[u] > cap_t(0))) {
              break;
            }

            // Find the lowest-height neighbour
            int lowest_h = std::numeric_limits<int>::max();
            vertex_id_t v_hat = -1;
            edge_id_t e_hat = -1;
            for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
              if (net.residual_capacity[e] > cap_t(0) && height[net.edge_dst[e]] < lowest_h) {
                lowest_h = height[net.edge_dst[e]];
                v_hat = net.edge_dst[e];
                e_hat = e;
              }
            }

            if (v_hat == -1) {
              break; // no residual out edge left
            }

            if (height[u] > lowest_h) {
              // push : send as much possible to lower neighbour
              cap_t d = std::min(excess[u], net.residual_capacity[e_hat]);
              net.residual_capacity[e_hat] -= d;
              net.residual_capacity[net.reverse_index[e_hat]] += d;
              excess[u] -= d;
              excess[v_hat] += d;
            } else {
              // relabel : lift u by 1
              height[u] = lowest_h + 1;
            }
          }
        }
      }

      // Algorithm 3
      // Remove steep edges. u->v if h(u) > h(v) + 1 means invariant is broken
      // For CPU, it never breaks. So kept for GPU 
      void remove_invalid_edges() {
        for (vertex_id_t u = 0; u < net.num_nodes; u++) {
          if (u == net.source || u == net.sink) {
            continue;
          }

          for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
            vertex_id_t v = net.edge_dst[e];
            if (net.residual_capacity[e] > cap_t(0) && height[u] > height[v] + 1) {
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