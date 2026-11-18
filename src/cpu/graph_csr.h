// flow network stored in CSR form with both directions of every edge (bi-CSR)
// usage:
//   flow_network<cap_t> net;
//   net.build_from_edges(num_nodes, source, sink, edge_list);

#ifndef MAXFLOW_GRAPH_CSR_H
#define MAXFLOW_GRAPH_CSR_H

#include <cstddef>
#include <vector>

#include "src/common/types.h"

namespace maxflow {

  // [EDGE] one input edge u -> v with its capacity
  template <typename cap_t>
  struct edge {
    vertex_id_t u;
    vertex_id_t v;
    cap_t cap;
  };

  // [CSR] every input edge becomes two half-edges, forward with cap and reverse with 0
  // the reverse half-edge is used to send flow back, its position is reverse_index[e]
  template <typename cap_t>
  class flow_network {
    public:
      vertex_id_t num_nodes = 0;
      edge_id_t num_edges = 0;     // half-edges, 2 per input edge
      vertex_id_t source = -1;
      vertex_id_t sink = -1;

      // [CSR] half-edges of vertex v are in [offset[v], offset[v+1])
      std::vector<vertex_id_t> offset;
      std::vector<vertex_id_t> edge_dst;
      std::vector<cap_t> capacity;
      std::vector<cap_t> residual_capacity;
      std::vector<edge_id_t> reverse_index;

      // [CSR-BUILD] fill all arrays from a plain edge list
      // input edge i gives the forward half-edge (u -> v, cap) and right after it the reverse half-edge (v -> u, 0),
      // both are placed at the next free slot of their own vertex, so every vertex keeps its half-edges in input order
      void build_from_edges(vertex_id_t num_nodes_in, vertex_id_t source_in, vertex_id_t sink_in, const std::vector<edge<cap_t>>& edges) {
        num_nodes = num_nodes_in;
        source = source_in;
        sink = sink_in;
        num_edges = static_cast<edge_id_t>(edges.size() * 2);

        // [CSR-BUILD] step 1, half-edges per vertex, one forward at u and one reverse at v for every input edge
        offset.assign(num_nodes + 1, 0);
        for (std::size_t i = 0; i < edges.size(); i++) {
          offset[edges[i].u + 1]++;
          offset[edges[i].v + 1]++;
        }

        // [CSR-BUILD] step 2, prefix sum gives the start of each vertex
        for (vertex_id_t v = 0; v < num_nodes; v++) {
          offset[v + 1] += offset[v];
        }

        // [CSR-BUILD] step 3, place both half-edges of every input edge, each knows its partner right away
        // [DIRECT] no temporary half-edge list and no separate partner pass (both were 27 % of the CSR time on 10_1M)
        edge_dst.assign(num_edges, 0);
        capacity.assign(num_edges, cap_t(0));
        reverse_index.assign(num_edges, 0);
        std::vector<edge_id_t> next_pos(offset.begin(), offset.end() - 1);
        for (std::size_t i = 0; i < edges.size(); i++) {
          edge_id_t f = next_pos[edges[i].u]++;
          edge_id_t r = next_pos[edges[i].v]++;
          edge_dst[f] = edges[i].v;
          capacity[f] = edges[i].cap;
          reverse_index[f] = r;
          edge_dst[r] = edges[i].u;
          reverse_index[r] = f;
        }

        // [RESIDUAL] not filled here: the CPU solver sets residual = capacity at the start of solve() and the GPU solver
        // keeps its own residual on the GPU, so a copy here only cost time and memory on the GPU path
        residual_capacity.clear();
      }
  };

} // namespace maxflow

#endif // MAXFLOW_GRAPH_CSR_H
