// Nemhauser-Trotter kernelizer for MWVC using max-flow on the bipartite double cover, CPU version
// it replaces KernelizerLinearProgramming of WCSPLift, same kernelize(g, out) signature
// usage:
//   maxflow::KernelizerMaxflow<> k(perturb_cfg);
//   k.kernelize(ccg_graph, assignments);
//
// idea: the LP relaxation of MWVC is solved by a min cut on the double cover
//   left copy on source side and right copy on sink side  -> x = 0, not in cover
//   left copy on sink side and right copy on source side  -> x = 1, in cover
//   both copies on the same side                         -> x = 0.5, stays in the kernel

#ifndef MAXFLOW_KERNELIZER_MAXFLOW_H
#define MAXFLOW_KERNELIZER_MAXFLOW_H

#include <map>
#include <vector>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>

#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "third_party/wcsp-solver/src/Kernelizer.h"

#include "src/common/metrics.h"
#include "src/common/types.h"
#include "src/cpu/graph_csr.h"
#include "src/cpu/maxflow_static.h"
#include "src/integration/perturbation.h"

namespace maxflow {

  // [DOUBLE-COVER] builds the flow network of a CCG, shared by the CPU and GPU kernelizers
  // flow vertex ids: 0 = source, 1..n = left copies, n+1..2n = right copies, 2n+1 = sink
  //   source -> left copy of v       capacity w(v)
  //   right copy of v -> sink        capacity w(v)
  //   left u -> right v, left v -> right u for every CCG edge (u, v), capacity INF = 1 + sum of weights
  template <class CCG>
  void build_double_cover(typename CCG::graph_t& g, const std::vector<typename boost::graph_traits<typename CCG::graph_t>::vertex_descriptor>& ccg_vertices,
                          std::map<typename boost::graph_traits<typename CCG::graph_t>::vertex_descriptor, int>& vertex_index,
                          const perturb_config& pcfg, flow_network<cap_t>& net) {
    using namespace boost;
    typedef typename CCG::graph_t graph_t;
    typedef typename graph_traits<graph_t>::vertex_descriptor vertex_t;

    auto vertex_weight_map = get(vertex_weight, g);
    int n = static_cast<int>(ccg_vertices.size());
    vertex_id_t flow_source = 0;
    vertex_id_t flow_sink = 2 * n + 1;

    // [PERTURB] total weight and integrality decide if the perturbation is safe
    cap_t total_w = cap_t(0);
    bool w_integral = true;
    for (int i = 0; i < n; i++) {
      cap_t w = static_cast<cap_t>(vertex_weight_map[ccg_vertices[i]]);
      total_w += w;
      if (!is_integral(w)) {
        w_integral = false;
      }
    }
    perturber pert(pcfg, n, total_w, w_integral);

    // [DOUBLE-COVER] vertex capacities after perturbation, INF is larger than any possible flow
    std::vector<cap_t> weights(n);
    cap_t inf_cap = cap_t(1);
    for (int i = 0; i < n; i++) {
      weights[i] = pert.weight(i, static_cast<cap_t>(vertex_weight_map[ccg_vertices[i]]));
      inf_cap += weights[i];
    }

    // [DOUBLE-COVER] source and sink edges, one pair per CCG vertex
    std::vector<edge<cap_t>> flow_edges;
    flow_edges.reserve(6 * n);
    for (int i = 0; i < n; i++) {
      flow_edges.push_back({flow_source, i + 1, weights[i]});
      flow_edges.push_back({n + i + 1, flow_sink, weights[i]});
    }

    // [DOUBLE-COVER] cross edges, one pair per CCG edge
    typename graph_traits<graph_t>::edge_iterator ei, ei_end;
    for (std::tie(ei, ei_end) = edges(g); ei != ei_end; ++ei) {
      vertex_t a = source(*ei, g);
      vertex_t b = target(*ei, g);
      int u = vertex_index[a];
      int v = vertex_index[b];
      flow_edges.push_back({u + 1, n + v + 1, inf_cap});
      flow_edges.push_back({v + 1, n + u + 1, inf_cap});
    }

    net.build_from_edges(2 * n + 2, flow_source, flow_sink, flow_edges);
  }

  // [APPLY] write the decided variables to out and remove their vertices from the CCG
  // side[i] is 0 (not in cover), 1 (in cover) or 2 (undecided, kept)
  template <class CCG>
  void apply_classification(typename CCG::graph_t& g, const std::vector<typename boost::graph_traits<typename CCG::graph_t>::vertex_descriptor>& ccg_vertices,
                            const std::vector<int>& side, std::map<typename CCG::variable_id_t, bool>& out) {
    using namespace boost;
    auto vertex_id_map = get(vertex_name, g);
    for (std::size_t i = 0; i < ccg_vertices.size(); i++) {
      if (side[i] == 2) {
        continue;
      }
      auto v = ccg_vertices[i];
      auto id = vertex_id_map[v];
      // [APPLY] only real and type 1 auxiliary variables have id >= 0
      if (id >= 0) {
        out[id] = (side[i] == 1);
      }
      clear_vertex(v, g);
      remove_vertex(v, g);
    }
  }

  template <class CCG = ConstraintCompositeGraph<>>
  class KernelizerMaxflow : public Kernelizer<CCG> {
    public:
      KernelizerMaxflow() : pcfg() {}
      explicit KernelizerMaxflow(const perturb_config& cfg) : pcfg(cfg) {}

      // [KERNELIZE] one round, the caller repeats it until nothing new is decided
      virtual void kernelize(typename CCG::graph_t& g, std::map<typename CCG::variable_id_t, bool>& out) {
        using namespace boost;
        typedef typename CCG::graph_t graph_t;
        typedef typename graph_traits<graph_t>::vertex_descriptor vertex_t;

        double t = now_sec();

        // [COLLECT] give every CCG vertex an integer index, the vertex list is listS so descriptors are not integers
        std::vector<vertex_t> ccg_vertices;
        std::map<vertex_t, int> vertex_index;
        typename graph_traits<graph_t>::vertex_iterator vi, vi_end;
        for (std::tie(vi, vi_end) = vertices(g); vi != vi_end; ++vi) {
          vertex_index[*vi] = static_cast<int>(ccg_vertices.size());
          ccg_vertices.push_back(*vi);
        }
        int n = static_cast<int>(ccg_vertices.size());
        t = mark("time.kern.collect", t);
        if (n == 0) {
          return;
        }

        // [DOUBLE-COVER] flow network of this CCG
        flow_network<cap_t> net;
        build_double_cover<CCG>(g, ccg_vertices, vertex_index, pcfg, net);
        t = mark("time.kern.build_flownet", t);

        // [MAXFLOW] CPU push-relabel
        static_max_flow_solver<cap_t> solver(net);
        solver.solve();
        t = mark("time.kern.maxflow", t);

        // [CLASSIFY] side of the left and right copy of every vertex
        std::vector<int> side(n, 2);
        for (int i = 0; i < n; i++) {
          bool left_src = solver.is_on_source_side(i + 1);
          bool right_src = solver.is_on_source_side(n + i + 1);
          if (left_src && !right_src) {
            side[i] = 0;
          } else if (!left_src && right_src) {
            side[i] = 1;
          }
        }
        t = mark("time.kern.classify", t);

        // [APPLY] update the CCG
        apply_classification<CCG>(g, ccg_vertices, side, out);
        mark("time.kern.apply", t);
      }

    private:
      perturb_config pcfg;
  };

} // namespace maxflow

#endif // MAXFLOW_KERNELIZER_MAXFLOW_H
