#ifndef MAXFLOW_GPU_TOPOLOGY_H
#define MAXFLOW_GPU_TOPOLOGY_H

#include "src/gpu/maxflow_gpu_common.h"

#include <cstdlib>

#ifdef MAXFLOW_PROFILE
#include <chrono>
#include <cstdio>
#endif

namespace maxflow {
  //  Topology-driven Push-Relabel approach
  //
  //  File contains topology-specific kernel and solver
  //  Shared kernels: (init, saturate, BFS, remove-invalid, check-active) taken from "maxflow_gpu_common.h"

  //  Algorithm 2: Push-relabel sweep
  //  Each thread does upto kernel_cycles times
  //  Reference: staticMaxFlow_kernel_14
  __global__ void topo_push_relabel_kernel(int num_nodes, int source, int sink, int kernel_cycles, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, int* height) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }

    for (int cnt = 0; cnt < kernel_cycles; cnt++) {
      if (!(height[u] < num_nodes && excess[u] > MAXFLOW_EPSILON)) {
        break;  // u not active
      }

      //  Find lowest-height neighbour reachable via residual edge
      int lowest_h = num_nodes + 1;
      int v_hat = -1;
      int e_hat = -1;

      for (int e = offset[u]; e < offset[u + 1]; e++) {
        if (residual_capacity[e] > MAXFLOW_EPSILON && height[edge_dst[e]] < lowest_h) {
          lowest_h = height[edge_dst[e]];
          v_hat = edge_dst[e];
          e_hat = e;
        }
      }

      if (v_hat == -1) {
        break;
      }

      if (height[u] > lowest_h) {
        //  Push: send min(excess[u], residual[e_hat]) along e_hat
        cap_t a = excess[u];
        cap_t b = residual_capacity[e_hat];
        cap_t d = (a < b) ? a : b;
        atomicAdd(&residual_capacity[e_hat], -d);
        atomicAdd(&residual_capacity[reverse_index[e_hat]], d);
        atomicAdd(&excess[u], -d);
        atomicAdd(&excess[v_hat], d);
      } else {
        //  Relabel: raise u just above its lowest neighbour
        height[u] = lowest_h + 1;
      }
    }
  }

  //  Host Solver Class - Topology
  //
  //  Takes a host-side flow_network<cap_t>, copies it to GPU and runs Algorithm 1
  //  Copies result back to host
  class gpu_topology_solver {
    public:
      explicit gpu_topology_solver(flow_network<cap_t>& net): net(net) {
        int V = net.num_nodes;
        int E = net.num_edges;

        //  Allocate device arrays
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_offset,            (V + 1) * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_edge_dst,          E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_capacity,          E       * sizeof(cap_t)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_residual_capacity, E       * sizeof(cap_t)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_reverse_index,     E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_excess,            V       * sizeof(cap_t)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_height,            V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_flag,              1       * sizeof(int)));

        //  Frontier buffers for the data-driven global relabel.
        //  A whole BFS claims each vertex at most once, so one layer can never
        //  hold more than V entries
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_frontier_in,       V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_frontier_out,      V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_frontier_count,    1       * sizeof(int)));

        //  Copy graph struct to device
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_offset, net.offset.data(), (V + 1) * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_edge_dst, net.edge_dst.data(), E * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_capacity, net.capacity.data(), E * sizeof(cap_t), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_reverse_index, net.reverse_index.data(), E * sizeof(int), cudaMemcpyHostToDevice));
      }

      ~gpu_topology_solver() {
        cudaFree(d_offset);
        cudaFree(d_edge_dst);
        cudaFree(d_capacity);
        cudaFree(d_residual_capacity);
        cudaFree(d_reverse_index);
        cudaFree(d_excess);
        cudaFree(d_height);
        cudaFree(d_flag);
        cudaFree(d_frontier_in);
        cudaFree(d_frontier_out);
        cudaFree(d_frontier_count);
      }

      //  Disallow copy (raw pointers)
      gpu_topology_solver(const gpu_topology_solver&) = delete;
      gpu_topology_solver& operator = (const gpu_topology_solver&) = delete;

      //  Run the full static max-flow (Algorithm 1) on GPU
      cap_t solve(int kernel_cycles = 0) {
        int V = net.num_nodes;
        int E = net.num_edges;
        if (kernel_cycles <= 0) {
          //  Optional -- override for parameter sweeps, e.g. MAXFLOW_KERNEL_CYCLES=50 ./e2e_gpu_prof file.wcsp
          const char* env_cycles = std::getenv("MAXFLOW_KERNEL_CYCLES");
          if (env_cycles != nullptr) {
            kernel_cycles = std::atoi(env_cycles);
          }
        }
        if (kernel_cycles <= 0) {
          //  Paper's heuristic: average degree |E|/|V|
          kernel_cycles = std::max(1, E / std::max(1, V));
        }

        int threads = 256;
        int blocks_v = (V + threads - 1) / threads;
        int blocks_max = (std::max(V, E) + threads - 1) / threads;

        //  Algorithm 1: initialize
        gpu_initialize_kernel<<<blocks_max, threads>>>(V, E, net.source, d_capacity, d_residual_capacity, d_excess, d_height);
        MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

        //  Algorithm 1: saturate source
        int src_start = net.offset[net.source];
        int src_end = net.offset[net.source + 1];
        int src_count = src_end - src_start;
        if (src_count > 0) {
          int blocks_s = (src_count + threads - 1) / threads;
          gpu_saturate_source_kernel<<<blocks_s, threads>>>(src_start, src_end, net.source, d_edge_dst, d_capacity, d_residual_capacity, d_reverse_index, d_excess);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
        } 

#ifdef MAXFLOW_PROFILE
        //  In this build the check-active kernel is swapped for a counting kernel, so no extra launch is introduced and the stage timings stay representative of the normal build
        double t_check = 0, t_bfs = 0, t_push = 0, t_remove = 0;
        //  Split the relabel cost into GPU work vs host round-trip latency
        double t_bfs_kernel = 0, t_bfs_copy = 0;
        long n_outer = 0, n_bfs_levels = 0;
        std::vector<int> active_history;
        auto clk = []() { return std::chrono::high_resolution_clock::now(); };
        auto secs = [](std::chrono::high_resolution_clock::time_point a,
                       std::chrono::high_resolution_clock::time_point b) {
          return std::chrono::duration<double>(b - a).count();
        };
#endif

        //  BFS variant: topology-driven by default, frontier via MAXFLOW_BFS=frontier
        bool use_frontier_bfs = false;
        {
          const char* env_bfs = std::getenv("MAXFLOW_BFS");
          if (env_bfs != nullptr && std::string(env_bfs) == "frontier") {
            use_frontier_bfs = true;
          }
        }

        //  Algorithm 1: main loop
        int h_flag;
        while (true) {
          //  Check for active vertices
#ifdef MAXFLOW_PROFILE
          auto tc0 = clk();
          int n_act = 0;
          h_flag = 0;
          MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &h_flag, sizeof(int), cudaMemcpyHostToDevice));
          gpu_count_active_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_excess, d_height, d_flag);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
          MAXFLOW_CUDA_CHECK(cudaMemcpy(&n_act, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
          h_flag = (n_act > 0) ? 1 : 0;
          active_history.push_back(n_act);
          n_outer++;
          t_check += secs(tc0, clk());
#else
          h_flag = 0;
          MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &h_flag, sizeof(int), cudaMemcpyHostToDevice));
          gpu_check_active_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_excess, d_height, d_flag);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
          MAXFLOW_CUDA_CHECK(cudaMemcpy(&h_flag, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
#endif
          if (!h_flag) {
            break;  // no active vertex => done
          }

          //  Algorithm 4: global relabel (backwards BFS, data-driven)
#ifdef MAXFLOW_PROFILE
          auto tb0 = clk();
#endif
          gpu_bfs_init_kernel<<<blocks_v, threads>>>(V, net.sink, d_height);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

          if (!use_frontier_bfs) {
            //  Topology-driven: |V| threads per layer, most exit immediately
            for (int level = 0; level < V; level++) {
              h_flag = 0;
#ifdef MAXFLOW_PROFILE
              auto tm0 = clk();
#endif
              MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &h_flag, sizeof(int), cudaMemcpyHostToDevice));
#ifdef MAXFLOW_PROFILE
              t_bfs_copy += secs(tm0, clk());
              auto tk0 = clk();
#endif
              gpu_bfs_step_kernel<<<blocks_v, threads>>>(V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
              MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
              t_bfs_kernel += secs(tk0, clk());
              auto tm1 = clk();
#endif
              MAXFLOW_CUDA_CHECK(cudaMemcpy(&h_flag, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
#ifdef MAXFLOW_PROFILE
              t_bfs_copy += secs(tm1, clk());
              n_bfs_levels++;
#endif
              if (!h_flag) {
                break;  //  BFS finished => no more layers
              }
            }
          } else {
            //  Data-driven: one thread per frontier vertex.
            //  Measured ~15% SLOWER than the topology-driven loop above on both
            //  03_100k and 05_300k_pairwise despite launching far fewer threads
            MAXFLOW_CUDA_CHECK(cudaMemcpy(d_frontier_in, &net.sink, sizeof(int), cudaMemcpyHostToDevice));
            int frontier_size = 1;

            for (int level = 0; frontier_size > 0 && level < V; level++) {
              int zero = 0;
#ifdef MAXFLOW_PROFILE
              auto tm0 = clk();
#endif
              MAXFLOW_CUDA_CHECK(cudaMemcpy(d_frontier_count, &zero, sizeof(int), cudaMemcpyHostToDevice));
#ifdef MAXFLOW_PROFILE
              t_bfs_copy += secs(tm0, clk());
              auto tk0 = clk();
#endif
              int blocks_f = (frontier_size + threads - 1) / threads;
              gpu_bfs_frontier_step_kernel<<<blocks_f, threads>>>(frontier_size, V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_frontier_in, d_frontier_out, d_frontier_count);
              MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
              t_bfs_kernel += secs(tk0, clk());
              auto tm1 = clk();
#endif
              MAXFLOW_CUDA_CHECK(cudaMemcpy(&frontier_size, d_frontier_count, sizeof(int), cudaMemcpyDeviceToHost));
#ifdef MAXFLOW_PROFILE
              t_bfs_copy += secs(tm1, clk());
              n_bfs_levels++;
#endif
              std::swap(d_frontier_in, d_frontier_out);
            }
          }

#ifdef MAXFLOW_PROFILE
          t_bfs += secs(tb0, clk());
          auto tp0 = clk();
#endif

          //  Algorithm 2: push-relabel sweep
          topo_push_relabel_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, kernel_cycles, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
          t_push += secs(tp0, clk());
          auto tr0 = clk();
#endif

          //  Algorithm 3: remove invalid edges
          gpu_remove_invalid_edges_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
          t_remove += secs(tr0, clk());
#endif
        }

#ifdef MAXFLOW_PROFILE
        {
          double tot = t_check + t_bfs + t_push + t_remove;
          if (tot <= 0) {
            tot = 1e-12;
          }
          std::fprintf(stderr, "\n=== GPU topology profile ===\n");
          std::fprintf(stderr, "  graph              : V=%d  E=%d  kernel_cycles=%d\n", V, E, kernel_cycles);
          std::fprintf(stderr, "  outer iterations   : %ld\n", n_outer);
          std::fprintf(stderr, "  BFS levels (total) : %ld\n", n_bfs_levels);
          std::fprintf(stderr, "  kernel launches    : %ld\n", n_bfs_levels + 4 * n_outer);
          std::fprintf(stderr, "  check-active  : %8.3f s  (%5.1f%%)\n", t_check,  100.0 * t_check  / tot);
          std::fprintf(stderr, "  global relabel: %8.3f s  (%5.1f%%)\n", t_bfs,    100.0 * t_bfs    / tot);
          std::fprintf(stderr, "    of which kernel : %8.3f s\n", t_bfs_kernel);
          std::fprintf(stderr, "    of which memcpy : %8.3f s\n", t_bfs_copy);
          std::fprintf(stderr, "    per level       : %8.4f ms kernel, %8.4f ms memcpy\n",
                       n_bfs_levels ? 1000.0 * t_bfs_kernel / n_bfs_levels : 0.0,
                       n_bfs_levels ? 1000.0 * t_bfs_copy   / n_bfs_levels : 0.0);     
          std::fprintf(stderr, "  push-relabel  : %8.3f s  (%5.1f%%)\n", t_push,   100.0 * t_push   / tot);
          std::fprintf(stderr, "  remove-invalid: %8.3f s  (%5.1f%%)\n", t_remove, 100.0 * t_remove / tot);
          std::fprintf(stderr, "  measured total: %8.3f s\n", tot);
          std::fprintf(stderr, "  active vertices per outer iteration (first 40):\n   ");
          for (size_t i = 0; i < active_history.size() && i < 40; i++) {
            std::fprintf(stderr, " %d", active_history[i]);
          }
          std::fprintf(stderr, "\n  total outer iterations recorded: %zu\n\n", active_history.size());
        }
#endif

        //  Read back max-flow = excess[sink]
        cap_t flow;
        MAXFLOW_CUDA_CHECK(cudaMemcpy(&flow, d_excess + net.sink, sizeof(cap_t), cudaMemcpyDeviceToHost));

        //  Read back final residuals and excess, then classify on the host
        h_residual_capacity.resize(E);
        h_excess.resize(V);
        MAXFLOW_CUDA_CHECK(cudaMemcpy(h_residual_capacity.data(), d_residual_capacity, E * sizeof(cap_t), cudaMemcpyDeviceToHost));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(h_excess.data(), d_excess, V * sizeof(cap_t), cudaMemcpyDeviceToHost));

        compute_source_side();

        return flow;
      }

      //  After solve(): true if v is on the source side of the min-cut
      bool is_on_source_side(vertex_id_t v) const {
        return h_source_side[v] != 0;
      }

    private:
      flow_network<cap_t>& net;

      // Device pointers
      int* d_offset = nullptr;
      int* d_edge_dst = nullptr;
      cap_t* d_capacity = nullptr;
      cap_t* d_residual_capacity = nullptr;
      int* d_reverse_index = nullptr;
      cap_t* d_excess = nullptr;
      int* d_height = nullptr;
      int* d_flag = nullptr;

      //  Frontier buffers for the data-driven global relabel
      int* d_frontier_in = nullptr;
      int* d_frontier_out = nullptr;
      int* d_frontier_count = nullptr;

      //  Host copies of the final state + the computed min-cut source side
      std::vector<cap_t> h_residual_capacity;
      std::vector<cap_t> h_excess;
      std::vector<char> h_source_side;

      //  Min-cut source side: residual-reachable from the source together with every vertex still holding excess
      //
      //  The algorithm terminates on a PREFLOW, not a flow: some units of flow never make it back to the source, so the source's own out-edges look
      //  saturated and a plain forward BFS from the source alone would return just {source} 
      //  Seeding the search with the stranded-excess vertices fixes exactly that
      //  The resulting set is a reachability closure, so no residual edge leaves it, and it holds the source plus all excess --
      //  its capacity equals the max-flow value
      //  It depends only on the final residual graph, so it is deterministic of how the parallel push-relabel happened to schedule
      void compute_source_side() {
        int V = net.num_nodes;
        h_source_side.assign(V, 0);

        std::vector<vertex_id_t> stack;
        stack.reserve(V);

        h_source_side[net.source] = 1;
        stack.push_back(net.source);
        for (vertex_id_t v = 0; v < V; v++) {
          if (v != net.source && v != net.sink
              && h_excess[v] > MAXFLOW_EPSILON && !h_source_side[v]) {
            h_source_side[v] = 1;
            stack.push_back(v);
          }
        }

        while (!stack.empty()) {
          vertex_id_t u = stack.back();
          stack.pop_back();
          for (edge_id_t e = net.offset[u]; e < net.offset[u + 1]; e++) {
            vertex_id_t v = net.edge_dst[e];
            if (h_residual_capacity[e] > MAXFLOW_EPSILON && !h_source_side[v]) {
              h_source_side[v] = 1;
              stack.push_back(v);
            }
          }
        }
      }
  };

} // namespace maxflow

#endif // MAXFLOW_GPU_TOPOLOGY_H