#ifndef MAXFLOW_GPU_WORKLIST_H
#define MAXFLOW_GPU_WORKLIST_H

#include "src/gpu/maxflow_gpu_common.h"

#include <cstdlib>
#include <string>

#ifdef MAXFLOW_PROFILE
#include <chrono>
#include <cstdio>
#endif

namespace maxflow {
  // Worklist push-relabel

  // same algorithm as gpu_topology_solver, same answers
  // difference is how many threads are launched per pass

  // this solver writes the active vertices into a worklist first, then launches 1 thread per worklist entry for push-relabel and remove-invalid kernel

  // Build worklist of active vertices
  // Active = not source, not sink ,positive excess, height below |V|
  // replaces gpu_check_active_kernel: worklist size is the active count
  __global__ void wl_build_worklist_kernel(int num_nodes, int source, int sink, const cap_t* excess, const int* height, int* worklist, int* count) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }
    if (excess[u] > MAXFLOW_EPSILON && height[u] < num_nodes) {
      int pos = atomicAdd(count, 1);
      worklist[pos] = u;
    }
  }

  // algorithm 2: push-relabel (now data-driven)
  __global__ void wl_push_relabel_kernel(int work_size, int num_nodes, int kernel_cycles, const int* worklist, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, int* height) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= work_size) {
      return;
    }
    int u = worklist[idx]; // source and sink excluded when list is made

    for (int cnt = 0; cnt < kernel_cycles; cnt++) {
      if (!(height[u] < num_nodes && excess[u] > MAXFLOW_EPSILON)) {
        break; // u not active
      }

      // lowest-height neighbour reachable by residual edge
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
        // push min(excess[u], residual[e_hat]) along e_hat
        cap_t a = excess[u];
        cap_t b = residual_capacity[e_hat];
        cap_t d = (a < b) ? a : b;
        atomicAdd(&residual_capacity[e_hat], -d);
        atomicAdd(&residual_capacity[reverse_index[e_hat]], d);
        atomicAdd(&excess[u], -d);
        atomicAdd(&excess[v_hat], d);
      } else {
        // relabel - raise u just above its lowest neighbour
        height[u] = lowest_h + 1;
      }
    }
  }

  // algorithm 3 : remove invalid steep edges, data-driven
  __global__ void wl_remove_invalid_edges_kernel(int work_size, const int* worklist, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int*reverse_index, cap_t* excess, const int* height) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= work_size) {
      return;
    }
    int u = worklist[idx];

    for (int e = offset[u]; e < offset[u + 1]; e++) {
      int v = edge_dst[e];
      cap_t rc = residual_capacity[e];
      if (rc > MAXFLOW_EPSILON && height[u] > height[v] + 1) {
        residual_capacity[e] = cap_t(0);
        atomicAdd(&residual_capacity[reverse_index[e]], rc);
        atomicAdd(&excess[u], -rc);
        atomicAdd(&excess[v], rc);
      }
    }
  }

  // host solver
  class gpu_worklist_solver {
    public:
      explicit gpu_worklist_solver(flow_network<cap_t>& net): net(net) {
        int V = net.num_nodes;
        int E = net.num_edges;
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_offset,            (V + 1) * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_edge_dst,          E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_capacity,          E       * sizeof(cap_t)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_residual_capacity, E       * sizeof(cap_t)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_reverse_index,     E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_excess,            V       * sizeof(cap_t)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_height,            V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_flag,              1       * sizeof(int)));

        //  The worklist can never hold more than V entries -vertex is appended at most once per build, and source and sink are not appended
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_worklist,          V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_worklist_count,    1       * sizeof(int)));

        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_frontier_in,       V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_frontier_out,      V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_frontier_count,    1       * sizeof(int)));

        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_offset, net.offset.data(), (V + 1) * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_edge_dst, net.edge_dst.data(), E * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_capacity, net.capacity.data(), E * sizeof(cap_t), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_reverse_index, net.reverse_index.data(), E * sizeof(int), cudaMemcpyHostToDevice));
      }

      ~gpu_worklist_solver() {
        cudaFree(d_offset); cudaFree(d_edge_dst); cudaFree(d_capacity);
        cudaFree(d_residual_capacity); cudaFree(d_reverse_index);
        cudaFree(d_excess); cudaFree(d_height); cudaFree(d_flag);
        cudaFree(d_worklist); cudaFree(d_worklist_count);
        cudaFree(d_frontier_in); cudaFree(d_frontier_out); cudaFree(d_frontier_count);
      }

      gpu_worklist_solver(const gpu_worklist_solver&) = delete;
      gpu_worklist_solver& operator = (const gpu_worklist_solver&) = delete;

      cap_t solve(int kernel_cycles = 0) {
        int V = net.num_nodes;
        int E = net.num_edges;

        if (kernel_cycles <= 0) {
          const char* env_cycles = std::getenv("MAXFLOW_KERNEL_CYCLES");
          if (env_cycles != nullptr) {
            kernel_cycles = std::atoi(env_cycles);
          }
        }
        if (kernel_cycles <= 0) {
          //  Paper's heuristic: average degree |E|/|V|
          kernel_cycles = std::max(1, E / std::max(1, V));
        }

        //  Passes between global relabels. Same meaning and same default as in the topology solver
        int gr_period = 400;
        {
          const char* env_gr = std::getenv("MAXFLOW_GR_PERIOD");
          if (env_gr != nullptr && std::atoi(env_gr) > 0) {
            gr_period = std::atoi(env_gr);
          }
        }

        //  Safety - also rebuild the worklist before remove-invalid. Off by default. Turning it on must not change any result --- only the runtime
        bool rebuild_before_remove = false;
        {
          const char* env_rb = std::getenv("MAXFLOW_WL_REBUILD");
          if (env_rb != nullptr && std::atoi(env_rb) != 0) {
            rebuild_before_remove = true;
          }
        }

        bool use_frontier_bfs = false;
        {
          const char* env_bfs = std::getenv("MAXFLOW_BFS");
          if (env_bfs != nullptr && std::string(env_bfs) == "frontier") {
            use_frontier_bfs = true;
          }
        }

        int threads = 256;
        int blocks_v = (V + threads - 1) / threads;
        int blocks_max = (std::max(V, E) + threads - 1) / threads;

        gpu_initialize_kernel<<<blocks_max, threads>>>(V, E, net.source, d_capacity, d_residual_capacity, d_excess, d_height);
        MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

        int src_start = net.offset[net.source];
        int src_end = net.offset[net.source + 1];
        int src_count = src_end - src_start;
        if (src_count > 0) {
          int blocks_s = (src_count + threads - 1) / threads;
          gpu_saturate_source_kernel<<<blocks_s, threads>>>(src_start, src_end, net.source, d_edge_dst, d_capacity, d_residual_capacity, d_reverse_index, d_excess);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
        }

#ifdef MAXFLOW_PROFILE
        double t_worklist = 0, t_bfs = 0, t_push = 0, t_remove = 0;
        double t_bfs_kernel = 0, t_bfs_copy = 0;
        long n_outer = 0, n_bfs_levels = 0, n_relabels = 0;
        //  Total threads actually launched, so the saving over the topology-driven solver can be quoted as a number
        long long threads_push = 0, threads_remove = 0;
        std::vector<int> active_history;
        auto clk = []() { return std::chrono::high_resolution_clock::now(); };
        auto secs = [](std::chrono::high_resolution_clock::time_point a, std::chrono::high_resolution_clock::time_point b) {
          return std::chrono::duration<double>(b - a).count();
        };
#endif

        int outer_iter = 0;
        bool heights_fresh = false;
        int work_size = 0;

        while (true) {
          //  Build the worklist. Its size doubles as the termination test, so no separate check-active pass is needed
#ifdef MAXFLOW_PROFILE
          auto tw0 = clk();
#endif
          int zero = 0;
          MAXFLOW_CUDA_CHECK(cudaMemcpy(d_worklist_count, &zero, sizeof(int), cudaMemcpyHostToDevice));
          wl_build_worklist_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_excess, d_height, d_worklist, d_worklist_count);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
          MAXFLOW_CUDA_CHECK(cudaMemcpy(&work_size, d_worklist_count, sizeof(int), cudaMemcpyDeviceToHost));
#ifdef MAXFLOW_PROFILE
          t_worklist += secs(tw0, clk());
          active_history.push_back(work_size);
          n_outer++;
#endif

          if (work_size == 0) {
            //  No active vertices. If the last pass skipped its global relabel the heights may be stale, so refresh once and look again before stopping
            if (heights_fresh) {
              break;
            }
            outer_iter = 0;   //  makes the relabel below launch on this pass
          }

          const bool do_global_relabel = (outer_iter % gr_period == 0);
          heights_fresh = do_global_relabel;

          if (do_global_relabel) {
#ifdef MAXFLOW_PROFILE
            auto tb0 = clk();
            n_relabels++;
#endif
            gpu_bfs_init_kernel<<<blocks_v, threads>>>(V, net.sink, d_height);
            MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

            if (!use_frontier_bfs) {
              int level = 0;
              while (true) {
#ifdef MAXFLOW_PROFILE
                auto tk0 = clk();
#endif
                int bfs_flag = 0;
                MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &bfs_flag, sizeof(int), cudaMemcpyHostToDevice));
                gpu_bfs_step_kernel<<<blocks_v, threads>>>(V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
                MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
                t_bfs_kernel += secs(tk0, clk());
                auto tm0 = clk();
#endif
                MAXFLOW_CUDA_CHECK(cudaMemcpy(&bfs_flag, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
#ifdef MAXFLOW_PROFILE
                t_bfs_copy += secs(tm0, clk());
                n_bfs_levels++;
#endif
                if (!bfs_flag) {
                  break;
                }
                level++;
              }
            } else {
              int frontier_size = 1;
              MAXFLOW_CUDA_CHECK(cudaMemcpy(d_frontier_in, &net.sink, sizeof(int), cudaMemcpyHostToDevice));
              int level = 0;
              while (frontier_size > 0) {
#ifdef MAXFLOW_PROFILE
                auto tk0 = clk();
#endif
                int out_zero = 0;
                MAXFLOW_CUDA_CHECK(cudaMemcpy(d_frontier_count, &out_zero, sizeof(int), cudaMemcpyHostToDevice));
                int blocks_f = (frontier_size + threads - 1) / threads;
                gpu_bfs_frontier_step_kernel<<<blocks_f, threads>>>(frontier_size, V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_frontier_in, d_frontier_out, d_frontier_count);
                MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
                t_bfs_kernel += secs(tk0, clk());
                auto tm0 = clk();
#endif
                MAXFLOW_CUDA_CHECK(cudaMemcpy(&frontier_size, d_frontier_count, sizeof(int), cudaMemcpyDeviceToHost));
#ifdef MAXFLOW_PROFILE
                t_bfs_copy += secs(tm0, clk());
                n_bfs_levels++;
#endif
                level++;
                std::swap(d_frontier_in, d_frontier_out);
              }
            }
#ifdef MAXFLOW_PROFILE
            t_bfs += secs(tb0, clk());
#endif
          }

          //  A global relabel can remove every remaining active vertex. Rebuild so the push pass does not launch threads for vertices that are now inactive
          if (do_global_relabel && work_size > 0) {
            int zero2 = 0;
            MAXFLOW_CUDA_CHECK(cudaMemcpy(d_worklist_count, &zero2, sizeof(int), cudaMemcpyHostToDevice));
            wl_build_worklist_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_excess, d_height, d_worklist, d_worklist_count);
            MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
            MAXFLOW_CUDA_CHECK(cudaMemcpy(&work_size, d_worklist_count, sizeof(int), cudaMemcpyDeviceToHost));
          }

          if (work_size > 0) {
            //  Algorithm 2: one thread per ACTIVE vertex
#ifdef MAXFLOW_PROFILE
            auto tp0 = clk();
            threads_push += work_size;
#endif
            int blocks_w = (work_size + threads - 1) / threads;
            wl_push_relabel_kernel<<<blocks_w, threads>>>(work_size, V, kernel_cycles, d_worklist, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
            MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
            t_push += secs(tp0, clk());
#endif

            if (rebuild_before_remove) {
              int zero3 = 0;
              MAXFLOW_CUDA_CHECK(cudaMemcpy(d_worklist_count, &zero3, sizeof(int), cudaMemcpyHostToDevice));
              wl_build_worklist_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_excess, d_height, d_worklist, d_worklist_count);
              MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
              MAXFLOW_CUDA_CHECK(cudaMemcpy(&work_size, d_worklist_count, sizeof(int), cudaMemcpyDeviceToHost));
            }

            //  Algorithm 3: one thread per worklist entry
            if (work_size > 0) {
#ifdef MAXFLOW_PROFILE
              auto tr0 = clk();
              threads_remove += work_size;
#endif
              int blocks_r = (work_size + threads - 1) / threads;
              wl_remove_invalid_edges_kernel<<<blocks_r, threads>>>(work_size, d_worklist, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
              MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
              t_remove += secs(tr0, clk());
#endif
            }
          }

          outer_iter++;
        }

#ifdef MAXFLOW_PROFILE
        {
          double tot = t_worklist + t_bfs + t_push + t_remove;
          if (tot <= 0) {
            tot = 1e-12;
          }
          std::fprintf(stderr, "\n=== GPU worklist (data-driven) profile ===\n");
          std::fprintf(stderr, "  graph              : V=%d  E=%d  kernel_cycles=%d  gr_period=%d\n", V, E, kernel_cycles, gr_period);
          std::fprintf(stderr, "  rebuild-before-remove: %s\n", rebuild_before_remove ? "on" : "off");
          std::fprintf(stderr, "  outer iterations   : %ld\n", n_outer);
          std::fprintf(stderr, "  global relabels    : %ld\n", n_relabels);
          std::fprintf(stderr, "  BFS levels (total) : %ld\n", n_bfs_levels);
          std::fprintf(stderr, "  build-worklist: %8.3f s  (%5.1f%%)\n", t_worklist, 100.0 * t_worklist / tot);
          std::fprintf(stderr, "  global relabel: %8.3f s  (%5.1f%%)\n", t_bfs, 100.0 * t_bfs / tot);
          std::fprintf(stderr, "    of which kernel : %8.3f s\n", t_bfs_kernel);
          std::fprintf(stderr, "    of which memcpy : %8.3f s\n", t_bfs_copy);
          std::fprintf(stderr, "  push-relabel  : %8.3f s  (%5.1f%%)\n", t_push, 100.0 * t_push / tot);
          std::fprintf(stderr, "  remove-invalid: %8.3f s  (%5.1f%%)\n", t_remove, 100.0 * t_remove / tot);
          std::fprintf(stderr, "  measured total: %8.3f s\n", tot);
          std::fprintf(stderr, "  threads launched, push-relabel  : %lld\n", threads_push);
          std::fprintf(stderr, "  threads launched, remove-invalid: %lld\n", threads_remove);
          std::fprintf(stderr, "  topology-driven would have launched, each: %lld\n", (long long)n_outer * (long long)V);
          if (threads_push > 0) {
            std::fprintf(stderr, "  thread reduction, push-relabel  : %.1fx\n", (double)((long long)n_outer * (long long)V) / (double)threads_push);
          }
          if (threads_remove > 0) {
            std::fprintf(stderr, "  thread reduction, remove-invalid: %.1fx\n", (double)((long long)n_outer * (long long)V) / (double)threads_remove);
          }
          std::fprintf(stderr, "  worklist size per outer iteration (first 40):\n   ");
          for (size_t i = 0; i < active_history.size() && i < 40; i++) {
            std::fprintf(stderr, " %d", active_history[i]);
          }
          std::fprintf(stderr, "\n  total outer iterations recorded: %zu\n\n", active_history.size());
        }
#endif

        cap_t flow;
        MAXFLOW_CUDA_CHECK(cudaMemcpy(&flow, d_excess + net.sink, sizeof(cap_t), cudaMemcpyDeviceToHost));

        h_residual_capacity.resize(E);
        h_excess.resize(V);
        MAXFLOW_CUDA_CHECK(cudaMemcpy(h_residual_capacity.data(), d_residual_capacity, E * sizeof(cap_t), cudaMemcpyDeviceToHost));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(h_excess.data(), d_excess, V * sizeof(cap_t), cudaMemcpyDeviceToHost));

        compute_source_side();
        return flow;
      }

      bool is_on_source_side(vertex_id_t v) const {
        return h_source_side[v] != 0;
      }

    private:
      flow_network<cap_t>& net;

      int* d_offset = nullptr;
      int* d_edge_dst = nullptr;
      cap_t* d_capacity = nullptr;
      cap_t* d_residual_capacity = nullptr;
      int* d_reverse_index = nullptr;
      cap_t* d_excess = nullptr;
      int* d_height = nullptr;
      int* d_flag = nullptr;

      int* d_worklist = nullptr;
      int* d_worklist_count = nullptr;

      int* d_frontier_in = nullptr;
      int* d_frontier_out = nullptr;
      int* d_frontier_count = nullptr;

      std::vector<cap_t> h_residual_capacity;
      std::vector<cap_t> h_excess;
      std::vector<char> h_source_side;

      // Min-cut source side: residual-reachable from the source together with every vertex still holding excess
      // same to the topology solver ---- it depends only on the final residual graph, not on how the passes were scheduled which is why both solvers must return the same cut
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

}

#endif // MAXFLOW_GPU_WORKLIST_H