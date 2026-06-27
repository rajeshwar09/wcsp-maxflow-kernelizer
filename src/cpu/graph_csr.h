#ifndef MAXFLOW_GRAPH_CSR_H
#define MAXFLOW_GRAPH_CSR_H

#include <vector>
#include <cstddef>
#include <ostream>

#include "src/common/types.h"

namespace maxflow {
  
  // Single directed edge from input: u -> v with capacity cap
  template<typename cap_t>
  struct edge {
    vertex_id_t u;
    vertex_id_t v;
    cap_t cap;
  };

  // Directed flow network stored in Bi-CSR form
  // For every directed edge u -> v, also store v -> u
  // This helps in pushing flow forward and pushing residual flow back

  // Calling each store direct edge "half-edge"
  // Every half-edge e has partner in opposite direction and it location is given by reverse_index[e]

  // cap_t is capacity/flow type

  template<typename cap_t>
  class flow_network {
    public:
      vertex_id_t num_nodes = 0;  // number of vertices
      edge_id_t num_edges = 0;    // number of half-edges ( = 2 x input edges)
      vertex_id_t source = -1;
      vertex_id_t sink = -1;

      // offset size = num_nodes + 1.
      // half-edges leaving vertex v are the slots [offset[v]..offset[v+1]]
      std::vector<vertex_id_t> offset;

      // edge_dst[e] : vertex half-edge e points to
      std::vector<vertex_id_t> edge_dst;

      // capacity[e] : original capacity of half-edge e
      std::vector<cap_t> capacity;

      // residual_capacity[e] : how much more flow can still go along e
      std::vector<cap_t> residual_capacity;

      // reverse_index[e] : index of e's partner in opposite direction
      // pushing d units along e =>
      //    residual_capacity[e] -= d
      //    residual_capacity[reverse_index[e]] += d
      std::vector<edge_id_t> reverse_index;

      // Build the Bi-CSR arrays from a plain list of directed edges
      //
      // num_nodes_in : number of vertices (ids 0..num_nodes_in-1)
      // source_in : source vertex s
      // sink_in : sink vertex t
      // edges : directed input edges (u -> v, capacity cap)
      //
      // For each input edge u->v we create two half-edges
      //   forward half-edge u->v with capacity = cap
      //   reverse half-edge v->u with capacity = 0
      // linked as partners. So num_edges = 2 * edges.size()
      void build_from_edges(vertex_id_t num_nodes_in, vertex_id_t source_in, vertex_id_t sink_in, const std::vector<edge<cap_t>>& edges) {
        num_nodes = num_nodes_in;
        source = source_in;
        sink = sink_in;
        num_edges = static_cast<edge_id_t>(edges.size() * 2);

        // step 1 : lay out half-edges in temp list
        //          for input edge i, slot 2*i is forward half-edge & slot 2*i+1 is reverse one
        // Partners are always neighbours so partner of slot k is k ^ 1
        std::vector<vertex_id_t> he_src(num_edges);
        std::vector<vertex_id_t> he_dst(num_edges);
        std::vector<cap_t> he_cap(num_edges);
        for (std::size_t i = 0; i < edges.size(); i++) {
          edge_id_t f = static_cast<edge_id_t>(2 * i);
          edge_id_t r = f + 1;
          he_src[f] = edges[i].u; he_dst[f] = edges[i].v; he_cap[f] = edges[i].cap;
          he_src[r] = edges[i].v; he_dst[r] = edges[i].u; he_cap[r] = cap_t(0);
        }

        // step 2 : count how many half-edges leave each vertex (outdegree)
        offset.assign(num_nodes + 1, 0);
        for (edge_id_t k = 0; k < num_edges; k++) {
          offset[he_src[k] + 1]++; 
        }

        // step 3 : prefix-sum to find v's edges beginning point
        for (vertex_id_t v = 0; v < num_nodes; v++) {
          offset[v + 1] += offset[v];
        }

        // step 4 : placeing half-edges to its final position
        // next_pos[v] = moving cursor point at the next free slot for vertex v
        edge_dst.assign(num_edges, 0);
        capacity.assign(num_edges, cap_t(0));
        residual_capacity.assign(num_edges, cap_t(0));
        reverse_index.assign(num_edges, 0);

        std::vector<edge_id_t> next_pos(offset.begin(), offset.end());
        std::vector<edge_id_t> final_pos(num_edges);

        for (edge_id_t k = 0; k < num_edges; k++) {
          vertex_id_t s = he_src[k];
          edge_id_t pos = next_pos[s]++;
          final_pos[k] = pos;
          edge_dst[pos] = he_dst[k];
          capacity[pos] = he_cap[k];
          residual_capacity[pos] = he_cap[k];
        }

        // step 5 : link partner edges (k and k ^ 1)
        for (edge_id_t k = 0; k < num_edges; k++) {
          reverse_index[final_pos[k]] = final_pos[k ^ 1];
        }
      }

      // Print the Bi-CSR arrays for checking the structure
      void print_debug(std::ostream& os) const {
        os << "num_nodes=" << num_nodes
           << " num_edges=" << num_edges
           << " source=" << source
           << " sink=" << sink << "\n";
        os << "offset: ";
        for (auto x : offset) os << x << ' ';
        os << "\n";
        for (vertex_id_t v = 0; v < num_nodes; ++v) {
          os << "vertex " << v << ":\n";
          for (edge_id_t e = offset[v]; e < offset[v + 1]; ++e) {
            os << "  e" << e
                << " dst=" << edge_dst[e]
                << " cap=" << capacity[e]
                << " res=" << residual_capacity[e]
                << " rev=" << reverse_index[e] << "\n";
          }
        }
      }
  };

} // namespace maxflow


#endif // MAXFLOW_GRAPH_CSR_H