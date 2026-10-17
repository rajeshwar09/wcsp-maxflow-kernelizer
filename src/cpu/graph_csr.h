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
      void build_from_edges(vertex_id_t num_nodes_in, vertex_id_t source_in, vertex_id_t sink_in, const std::vector<edge<cap_t>>& edges) {
        num_nodes = num_nodes_in;
        source = source_in;
        sink = sink_in;
        num_edges = static_cast<edge_id_t>(edges.size() * 2);

        // [CSR-BUILD] step 1, temporary half-edge list, slot 2i is forward and 2i+1 is reverse
        std::vector<vertex_id_t> he_src(num_edges);
        std::vector<vertex_id_t> he_dst(num_edges);
        std::vector<cap_t> he_cap(num_edges);
        for (std::size_t i = 0; i < edges.size(); i++) {
          edge_id_t f = static_cast<edge_id_t>(2 * i);
          edge_id_t r = f + 1;
          he_src[f] = edges[i].u;
          he_dst[f] = edges[i].v;
          he_cap[f] = edges[i].cap;
          he_src[r] = edges[i].v;
          he_dst[r] = edges[i].u;
          he_cap[r] = cap_t(0);
        }

        // [CSR-BUILD] step 2, out-degree of every vertex
        offset.assign(num_nodes + 1, 0);
        for (edge_id_t k = 0; k < num_edges; k++) {
          offset[he_src[k] + 1]++;
        }

        // [CSR-BUILD] step 3, prefix sum gives the start of each vertex
        for (vertex_id_t v = 0; v < num_nodes; v++) {
          offset[v + 1] += offset[v];
        }

        // [CSR-BUILD] step 4, place each half-edge at the next free slot of its vertex
        edge_dst.assign(num_edges, 0);
        capacity.assign(num_edges, cap_t(0));
        residual_capacity.assign(num_edges, cap_t(0));
        reverse_index.assign(num_edges, 0);

        std::vector<edge_id_t> next_pos(offset.begin(), offset.end());
        std::vector<edge_id_t> final_pos(num_edges);
        for (edge_id_t k = 0; k < num_edges; k++) {
          edge_id_t pos = next_pos[he_src[k]]++;
          final_pos[k] = pos;
          edge_dst[pos] = he_dst[k];
          capacity[pos] = he_cap[k];
          residual_capacity[pos] = he_cap[k];
        }

        // [CSR-BUILD] step 5, partners are k and k^1 in the temporary list
        for (edge_id_t k = 0; k < num_edges; k++) {
          reverse_index[final_pos[k]] = final_pos[k ^ 1];
        }
      }
  };

} // namespace maxflow

#endif // MAXFLOW_GRAPH_CSR_H
