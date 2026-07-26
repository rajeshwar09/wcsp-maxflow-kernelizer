#ifndef MAXFLOW_KERNELIZER_MAXFLOW_GPU_H
#define MAXFLOW_KERNELIZER_MAXFLOW_GPU_H

//  KernelizerMaxflowGPU - NT kernelization using GPU max-flow on bipartite double-cover graph
//
//  Identical to KernelizerMaxflow except the max-flow solve runs on the GPU (gpu_topology_solver)
//
//  The bipartite double-cover construction, index mapping, NT classification,
//  and graph modification are byte-for-byte the same as the CPU version
//  Only the solver backend differs.

#include <map>
#include <vector>
#include <limits>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>

#include "third_party/wcsp-solver/src/Kernelizer.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"

#include "src/common/types.h"
#include "src/cpu/graph_csr.h"

#include "src/gpu/maxflow_gpu_topology.h"

namespace maxflow {

  //  KernelizerMaxflowGPU class
  //
  //  Template parameter CCG matches the base class Kernelizer<CCG>
  //  Taking default as ConstraintCompositeGraph<> which is same as used in LP
  //
  //  This uses the GPU topology-driven static max-flow solver

  template <class CCG = ConstraintCompositeGraph<>>
  class KernelizerMaxflowGPU : public Kernelizer<CCG> {
    public:
      //  kernelize()
      //
      //  Same signature as KernelizerMaxflow::kernelize() and KernelizerLinearProgramming::kernelize()
      //
      //  Input:
      //    g - CCG graph (Boost adjacency_list, undirected, vertex_name=variable_id, vertex_weight=weight)
      //
      //  Output:
      //    out - map from variable_id to bool (true=in cover, false=not in cover, 0.5=variables left in kernel)

      virtual void kernelize(typename CCG::graph_t& g, std::map<typename CCG::variable_id_t, bool>& out) {
        using namespace boost;

        //  Type aliases from CCG
        typedef typename CCG::graph_t graph_t;
        typedef typename graph_traits<graph_t>::vertex_descriptor vertex_t;
        typedef typename CCG::weight_t weight_t;
        typedef typename CCG::variable_id_t variable_id_t;

        //  Property maps for vertex_name (variable ID) and vertex_weight (weight)
        auto vertex_id_map = get(vertex_name, g);
        auto vertex_weight_map = get(vertex_weight, g);

    
        //  1 : Collect CCG vertices and build index mapping
        //
        //  Flow network vertex layout (bipartite double-cover):
        //    0 = source (s)
        //    1 .. n = left copies  (vertex i maps to flow ID i+1)
        //    n+1 .. 2n = right copies (vertex i maps to flow ID n+i+1)
        //    2n+1 = sink (t)
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

        //  2 : Build bipartite double-cover flow network    

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

    
        //  3 : Solve max-flow ON THE GPU
    
        //
        //  gpu_topology_solver exposes the SAME is_on_source_side(v) query as the CPU solver (both defined as height[v] >= num_nodes)

        gpu_topology_solver solver(net);
        solver.solve();

        // TEMP DIAGNOSTIC: how many flow-network nodes are source-side?
        {
          int src_side = 0;
          for (int n_id = 0; n_id < flow_n; n_id++) {
            if (solver.is_on_source_side(n_id)) src_side++;
          }
          std::cerr << "[source-side nodes] " << src_side << " of " << flow_n << "\n";
        }

        //  4 : Extract NT classification from min-cut    
        //
        //  Classification:
        //    L on source, R on sink   -> x = 0 (NOT in cover)
        //    L on sink,   R on source -> x = 1 (IN cover)
        //    both same side           -> x = 0.5 (undecided, keep)

        // 0 = not in cover, 1 = in cover, 2 = undecided (0.5)
        std::vector<int> classification(n, 2);

        for (int i = 0; i < n; i++) {
          bool left_source = solver.is_on_source_side(i + 1);
          bool right_source = solver.is_on_source_side(n + i + 1);

          if (left_source && !right_source) {
            classification[i] = 0;
          } else if (!left_source && right_source) {
            classification[i] = 1;
          }
          //  else: both on same side => x = 0.5, keep as 2
        }

        //  5 : Modify the CCG graph    

        for (int i = 0; i < n; i++) {
          if (classification[i] == 2) {
            continue;  //  x = 0.5 => undecided, keep it
          }

          vertex_t v = ccg_vertices[i];
          auto id = vertex_id_map[v];

          if (classification[i] == 1) {
            if (id >= 0) {
              out[id] = true;
            }
          } else {
            if (id >= 0) {
              out[id] = false;
            }
          }

          clear_vertex(v, g);
          remove_vertex(v, g);
        }
      }
  };

} // namespace maxflow

#endif // MAXFLOW_KERNELIZER_MAXFLOW_GPU_H