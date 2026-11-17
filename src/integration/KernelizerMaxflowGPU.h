// same kernelizer as KernelizerMaxflow, only the max-flow runs on the GPU (worklist solver)
// usage:
//   maxflow::KernelizerMaxflowGPU<> k(perturb_cfg);
//   k.kernelize(ccg_graph, assignments);

#ifndef MAXFLOW_KERNELIZER_MAXFLOW_GPU_H
#define MAXFLOW_KERNELIZER_MAXFLOW_GPU_H

#include <iostream>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <vector>

#include "src/gpu/maxflow_gpu_worklist.h"
#include "src/integration/KernelizerMaxflow.h"

namespace maxflow {

  template <class CCG = ConstraintCompositeGraph<>>
  class KernelizerMaxflowGPU : public Kernelizer<CCG> {
    public:
      KernelizerMaxflowGPU() : pcfg() {}
      explicit KernelizerMaxflowGPU(const perturb_config& cfg) : pcfg(cfg) {}

      // [KERNELIZE] one round, the caller repeats it until nothing new is decided
      virtual void kernelize(typename CCG::graph_t& g, std::map<typename CCG::variable_id_t, bool>& out) {
        using namespace boost;
        typedef typename CCG::graph_t graph_t;
        typedef typename graph_traits<graph_t>::vertex_descriptor vertex_t;

        double t = now_sec();

        // [COLLECT] integer index for every CCG vertex
        std::vector<vertex_t> ccg_vertices;
        // [INDEX] hash map from vertex to index, a std::map lookup walks a tree of millions of nodes and was most of
        // the flow-network build time on the big instances; the indices are the same, so the network is the same
        std::unordered_map<vertex_t, int> vertex_index;
        vertex_index.reserve(num_vertices(g));
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

        // [DOUBLE-COVER] same network as the CPU kernelizer
        flow_network<cap_t> net;
        build_double_cover<CCG>(g, ccg_vertices, vertex_index, pcfg, net);
        t = mark("time.kern.build_flownet", t);

        // [MAXFLOW] GPU worklist solver, the source side is copied out before the solver is freed
        // [CERT] a result that fails the proof check is never used: the max-flow is solved again from the start,
        // after three failed tries the round stops with an error
        std::vector<char> source_side(net.num_nodes, 0);
        for (int attempt = 1;; attempt++) {
          gpu_worklist_solver solver(net);
          solver.solve();
          if (!solver.certified()) {
            if (attempt >= 3) {
              throw std::runtime_error("gpu max-flow failed its proof check 3 times");
            }
            metrics::get().add("gpu.cert_retries", 1);
            std::cerr << "[warn] solving the max-flow again (try " << attempt + 1 << " of 3)\n";
            continue;
          }
          for (int v = 0; v < net.num_nodes; v++) {
            source_side[v] = solver.is_on_source_side(v) ? 1 : 0;
          }
          break;
        }
        t = mark("time.kern.maxflow", t);

        // [CLASSIFY] side of the left and right copy of every vertex
        std::vector<int> side(n, 2);
        for (int i = 0; i < n; i++) {
          bool left_src = source_side[i + 1] != 0;
          bool right_src = source_side[n + i + 1] != 0;
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

#endif // MAXFLOW_KERNELIZER_MAXFLOW_GPU_H
