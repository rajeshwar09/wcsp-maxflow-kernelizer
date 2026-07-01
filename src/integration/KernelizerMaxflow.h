#ifndef MAXFLOW_KERNELIZER_MAXFLOW_H
#define MAXFLOW_KERNELIZER_MAXFLOW_H

//  KernelizerMaxflow - NT kernelization using max-flor on bipartite double-cover graph
//
//  Implementing an alternative to KernelizerLinearProgramming
//  Inherit from Kernelizer<CCG> and implement the same kernelize(g, out)
//  From main.cpp, select either one by changing a single line
//
//  Mathematical basis:
//    LP relaxation of MWVC on graph G is equivalen to max-flow/min-cut on bipartite double-cover
//    Min-cut partition classifies each vertex as:
//      0 (not in cover)  - left copy on source size, right on sink
//      1 (int cover)     - left copy on sink size, right on source
//      0.5 (undecided)   - both copies on the same side
//  It is called Nemhauser-Trotter classification

#include <map>
#include <vector>
#include <limits>

//  Boost Graph header (for iteration over CCG vertices/edges)
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>

//  WCSP headers (from third_party/wcsp-solver/src)
#include "third_party/wcsp-solver/src/Kernelizer.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"

//  Max-flow headers
#include "src/common/types.h"
#include "src/cpu/graph_csr.h"
#include "src/cpu/maxflow_static.h"

namespace maxflow {
  
  //  --------------------------------------------------
  //  KernelizerMaxflow class
  //
  //  Template paramater CCG matches the base class Kernelizer<CCG>
  //  Taking default as ConstraintCompositeGraph<> which is same as used in LP
  //
  //  This is using CPU static max-flow (for easy testing the integration)
  //  --------------------------------------------------

  template <class CCG = ConstraintCompositeGraph<>>
  class KernelizerMaxflow : public Kernelizer<CCG> {
    public:
      //  kernelize()
      //
      //  It has same signature as KernelizerLinearProgramming::kernelize()
      //
      //  Input:
      //    g - CCG graph (Boost adjacency_list, undirected, vertex_name=variable_id, vertex_weight=weight)
      //
      //  Output:
      //    out - map from variable_id to bool (true=in cover, false=not in cover, 0.5=variables left in kernel)

      virtual void kernelize(typename CCG::graph_t& g, std::map<typename CCG::variable_id_t, bool>& out) {
        using namespace boost;

        //  Type alisases from CCG
        //  graph_t - boost::adjacency_list<vecS, listS, unidrectedS, ...>
        //    listS => vertex descriptors are point-like and not integers
        //    Task is to map them to integer indices for flow network
        typedef typename CCG::graph_t graph_t;
        typedef typename graph_traits<graph_t>::vertex_descriptor vertex_t;
        typedef typename CCG::weight_t weight_t;
        typedef typename CCG::variable_id_t variable_id_t;

        //  Property mapping - which help reading vertex_name (variable ID) and vertex_weight(weight) from descriptor
        auto vertex_id_map = get(vertex_name, g);
        auto vertex_weight_map = get(vertex_weight, g);

        //  --------------------------------------------------
        //  1 : Collect CCG vertices and build index mapping
        //  --------------------------------------------------
        //
        //  The CCG uses boost::listS for its vertex container so vertex_t is a pointer-like handle (not an integer)
        //  flow_network uses integer vertex IDs (0, 1, 2, ...)
        //
        //  build a mapping:
        //    ccg_vertex[i] = the i-th CCG vertex descriptor
        //    vertex_index[v] = the integer index i for vertex v
        //
        //  Flow network vertex layout (bipartite double-cover):
        //
        //    0 = source (s)
        //    1 .. n = left copies  (vertex i maps to flow ID i+1)
        //    n+1 .. 2n = right copies (vertex i maps to flow ID n+i+1)
        //    2n+1 = sink (t)
        //
        //  Total vertices in flow network = 2n + 2

        std::vector<vertex_t> ccg_vertices;
        std::map<vertex_t, int> vertex_index;

        typename graph_traits<graph_t>::vertex_iterator vi, vi_end;
        std::tie(vi, vi_end) = vertices(g);
        for (auto it = vi; it != vi_end; it++) {
          vertex_index[*it] = static_cast<int>(ccg_vertices.size());
          ccg_vertices.push_back(*it);
        }

        int n = static_cast<int>(ccg_vertices.size());

        //  Edge case : if the graph is empty, nothing to kernelize
        if (n == 0) {
          return;
        }

        //  --------------------------------------------------
        //  2 : Build bipartite double-cover flow network
        //  --------------------------------------------------
        //
        //  For each CCG vertex v with weight w(v):
        //    - source (0) -> left copy (i+1)      capacity = w(v)
        //    - right copy (n+i+1) -> sink (2n+1)  capacity = w(v)
        //
        //  For each CCG edge (u, v):
        //    - left copy of u -> right copy of v   capacity = INF
        //    - left copy of v -> right copy of u   capacity = INF
        //
        //  INF is set to 1 + sum of all vertex weights
        //  This will certain that INF exceeds the maximum possible flow
        
        int flow_n = 2 * n + 2;             //  total flow-network vertices
        vertex_id_t flow_source = 0;        //  source ID
        vertex_id_t flow_sink = 2 * n + 1;  //  sink ID

        //  Compute INF = 1 + sum of all weights
        cap_t inf_cap = cap_t(1);
        for (int i = 0; i < n; i++) {
          inf_cap += static_cast<cap_t>(vertex_weight_map[ccg_vertices[i]]);
        }

        //  collect directed edges from flow network
        std::vector<edge<cap_t>> flow_edges;

        //  Reserving space: n source edges + n sink edges + at most 2*|E| cross edges
        flow_edges.reserve(2 * n + 4 * n);

        //  source and sink edges (1 per CCG vertex)
        for (int i = 0; i < n; i++) {
          cap_t w = static_cast<cap_t>(vertex_weight_map[ccg_vertices[i]]);

          int left_copy = i + 1;
          int right_copy = n + i + 1;

          //  source -> left copy, capacity = vertex weight
          flow_edges.push_back({flow_source, left_copy, w});

          //  right copy -> sink, capacity = vertex weight
          flow_edges.push_back({right_copy, flow_sink, w});
        }

        //  cross edges (1 pair per CCG edge)
        typename graph_traits<graph_t>::edge_iterator ei, ei_end;
        std::tie(ei, ei_end) = edges(g);
        for (auto it = ei; it != ei_end; it++) {
          //  take 2 endpoints of this CCG edge
          vertex_t u_desc = source(*it, g);
          vertex_t v_desc = target(*it, g);

          int u_idx = vertex_index[u_desc];
          int v_idx = vertex_index[v_desc];

          int u_left = u_idx + 1;
          int v_left = v_idx + 1;
          int u_right = n + u_idx + 1;
          int v_right = n + v_idx + 1;

          //  left copy of u -> right copy of v, capacity = INF
          flow_edges.push_back({u_left, v_right, inf_cap});

          //  left copy of v -> right copy of u, capacity = INF
          flow_edges.push_back({v_left, u_right, inf_cap});
        }

        //  build Bi-CSR flow network from edge list
        flow_network<cap_t> net;
        net.build_from_edges(flow_n, flow_source, flow_sink, flow_edges);

        //  --------------------------------------------------
        //  3 : Solve max-flow
        //  --------------------------------------------------
        //
        //  Using the CPU solver. After solve(), query is_on_source_side(v) for
        //  any vertex to find which side of the min-cut it falls on 
        //  This is retrieve from the height[] array:
        //    height[v] >= num_nodes  <=>  v is on the source side

        static_max_flow_solver<cap_t> solver(net);
        solver.solve();

        //  --------------------------------------------------
        //  4 : Extract NT classification from min-cut
        //  --------------------------------------------------
        //
        //  For each original CCG vertex i:
        //    left copy  = i + 1       in the flow network
        //    right copy = n + i + 1   in the flow network
        //
        //  Classification:
        //    L on source, R on sink  -> x = 0 (NOT in cover)
        //    L on sink,   R on source -> x = 1 (IN cover)
        //    both same side           -> x = 0.5 (undecided, keep)
        //
        //  Storing the classification to later modify the graph in separate pass 
        //  (modifying a Boost graph while iterating it not good)

        // 0 = not in cover, 1 = in cover, 2 = undecided (0.5)
        std::vector<int> classification(n, 2);

        for (int i = 0; i < n; i++) {
          bool left_source = solver.is_on_source_side(i + 1);
          bool right_source = solver.is_on_source_side(n + i + 1);

          if (left_source && !right_source) {
            //  L on source side, R on sink side => x = 0
            classification[i] = 0;
          } else if (!left_source && right_source) {
            //  L on sink side, R on source side => x = 1
            classification[i] = 1;
          }
          //  else: both on same side => x = 0.5, keep it as 2 (int)
        }

        //  --------------------------------------------------
        //  5 : Modify the CCG graph
        //  --------------------------------------------------
        //
        //  Remove vertices classified as 0 or 1
        //  Record their variable assignments in `out`
        //
        //  Iterating using the saved ccg_vertices vector (before any changes),
        //  not from vertices(g) as removing vertices from a listS graph invalidates iterators.
        //
        //  It matches exactly with KernelizerLinearProgramming
        
        for (int i = 0; i < n; i++) {
          if (classification[i] == 2) {
            //  x = 0.5 => undecided, keep it
            continue;
          }

          vertex_t v = ccg_vertices[i];
          auto id = vertex_id_map[v];

          if (classification[i] == 1) {
            //  x = 1 => vertex is in cover
            //  keep assignment variables with id >= 0 not for others
            if (id >= 0) {
              out[id] = true;
            }
          } else {
            //  x = 0 => vertex not in cover
            if (id >= 0) {
              out[id] = false;
            }
          }

          //  Remove v and all its edges from CCG
          clear_vertex(v, g);
          remove_vertex(v, g);
        }
      }
  };

} // namespace maxflow

#endif // MAXFLOW_KERNELIZER_MAXFLOW_H