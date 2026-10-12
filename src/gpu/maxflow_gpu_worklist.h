#ifndef MAXFLOW_GPU_WORKLIST_H
#define MAXFLOW_GPU_WORKLIST_H

#include "src/gpu/maxflow_gpu_common.h"

#include <climits>
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
  //
  // Fix B: active vertices with hub_deg or more edges go to a second list (hub_list) and are later processed by a whole warp each; all others go to worklist as before
  // counts[0] = size of worklist, counts[1] = size of hub_list. hub_deg = 0 puts every vertex in worklist, which is exactly the old behaviour
  __global__ void wl_build_worklist_kernel(int num_nodes, int source, int sink, int hub_deg, const int* offset, const cap_t* excess, const int* height, int* worklist, int* hub_list, int* counts) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }
    if (excess[u] > MAXFLOW_EPSILON && height[u] < num_nodes) {
      if (hub_deg > 0 && offset[u + 1] - offset[u] >= hub_deg) {
        int pos = atomicAdd(&counts[1], 1);
        hub_list[pos] = u;
      } else {
        int pos = atomicAdd(&counts[0], 1);
        worklist[pos] = u;
      }
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

  // Fix B: algorithm 2 for hub vertices, ONE WARP per hub vertex
  //
  // Same rule as wl_push_relabel_kernel. The only difference is how the lowest neighbour is found: the 32 lanes of the warp share the edge list (lane l reads edges l, l+32, l+64, ...),
  // so neighbouring lanes read neighbouring edges (coalesced loads), then a warp reduction picks the lowest (height, edge) pair. Ties go to the smaller edge index, which is the edge the
  // single-thread loop would pick. Lane 0 then does the push or relabel exactly as before
  __global__ void wl_push_relabel_hub_kernel(int hub_size, int num_nodes, int kernel_cycles, const int* hub_list, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, int* height) {
    const unsigned FULL = 0xffffffffu;
    int warp = (blockIdx.x * blockDim.x + threadIdx.x) / 32;
    int lane = threadIdx.x & 31;
    if (warp >= hub_size) {
      return; // the whole warp leaves together: all 32 lanes have the same warp number
    }
    int u = hub_list[warp];
    int start = offset[u];
    int end = offset[u + 1];

    for (int cnt = 0; cnt < kernel_cycles; cnt++) {
      int active = 0;
      if (lane == 0) {
        active = (height[u] < num_nodes && excess[u] > MAXFLOW_EPSILON) ? 1 : 0;
      }
      active = __shfl_sync(FULL, active, 0);
      if (!active) {
        break; // u not active
      }

      // each lane: lowest-height residual neighbour among its own edges
      int best_h = num_nodes + 1;
      int best_e = INT_MAX;
      for (int e = start + lane; e < end; e += 32) {
        if (residual_capacity[e] > MAXFLOW_EPSILON) {
          int h = height[edge_dst[e]];
          if (h < best_h) {
            best_h = h;
            best_e = e;
          }
        }
      }

      // warp reduction: lowest height, ties to the smaller edge index
      for (int off = 16; off > 0; off >>= 1) {
        int oh = __shfl_down_sync(FULL, best_h, off);
        int oe = __shfl_down_sync(FULL, best_e, off);
        if (oh < best_h || (oh == best_h && oe < best_e)) {
          best_h = oh;
          best_e = oe;
        }
      }

      int stop = 0;
      if (lane == 0) {
        if (best_h > num_nodes) {
          stop = 1; // no residual edge
        } else if (height[u] > best_h) {
          // push min(excess[u], residual[best_e]) along best_e
          int v_hat = edge_dst[best_e];
          cap_t a = excess[u];
          cap_t b = residual_capacity[best_e];
          cap_t d = (a < b) ? a : b;
          atomicAdd(&residual_capacity[best_e], -d);
          atomicAdd(&residual_capacity[reverse_index[best_e]], d);
          atomicAdd(&excess[u], -d);
          atomicAdd(&excess[v_hat], d);
        } else {
          // relabel - raise u just above its lowest neighbour
          height[u] = best_h + 1;
        }
      }
      __syncwarp(); // lane 0's updates are visible to the whole warp before the next round
      stop = __shfl_sync(FULL, stop, 0);
      if (stop) {
        break;
      }
    }
  }

  // Fix B: algorithm 3 for hub vertices, ONE WARP per hub vertex. Same rule as wl_remove_invalid_edges_kernel; the lanes simply share the edge list
  __global__ void wl_remove_invalid_hub_kernel(int hub_size, const int* hub_list, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, const int* height) {
    int warp = (blockIdx.x * blockDim.x + threadIdx.x) / 32;
    int lane = threadIdx.x & 31;
    if (warp >= hub_size) {
      return;
    }
    int u = hub_list[warp];
    int hu = height[u]; // this kernel never changes heights
    for (int e = offset[u] + lane; e < offset[u + 1]; e += 32) {
      int v = edge_dst[e];
      cap_t rc = residual_capacity[e];
      if (rc > MAXFLOW_EPSILON && hu > height[v] + 1) {
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
        //  counts[0] = worklist size, counts[1] = hub list size (Fix B)
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_counts,            2       * sizeof(int)));

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
        cudaFree(d_worklist); cudaFree(d_counts); cudaFree(d_hub_list);
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

        //  Fix A: BFS level 0 (the sink's level) with one thread per sink edge. On by default; MAXFLOW_SINK_BFS=0 restores the old one-thread level 0, for before/after comparisons
        bool sink_level_parallel = true;
        {
          const char* env_sink = std::getenv("MAXFLOW_SINK_BFS");
          if (env_sink != nullptr && std::atoi(env_sink) == 0) {
            sink_level_parallel = false;
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

        //  Edge range of the sink, for the parallel BFS level 0 (Fix A)
        int sink_start = net.offset[net.sink];
        int sink_end = net.offset[net.sink + 1];
        int sink_count = sink_end - sink_start;
        int blocks_sink = (sink_count + threads - 1) / threads;
        if (sink_count <= 0) {
          sink_level_parallel = false;
        }

        //  Fix B: vertices with at least hub_deg edges are processed by a whole warp in push-relabel and remove-invalid. Default 64; MAXFLOW_HUB_DEG=0 turns Fix B off (every vertex gets one thread, the old behaviour)
        int hub_deg = 64;
        {
          const char* env_hub = std::getenv("MAXFLOW_HUB_DEG");
          if (env_hub != nullptr) {
            hub_deg = std::max(0, std::atoi(env_hub));
          }
        }
        //  The hub list can hold at most the vertices that have hub_deg or more edges
        int hub_capacity = 0;
        if (hub_deg > 0) {
          for (int v = 0; v < V; v++) {
            if (v != net.source && v != net.sink && net.offset[v + 1] - net.offset[v] >= hub_deg) {
              hub_capacity++;
            }
          }
        }
        if (hub_capacity == 0) {
          hub_deg = 0; // no vertex is big enough: nothing to split
        }
        if (d_hub_list != nullptr) {
          cudaFree(d_hub_list);
          d_hub_list = nullptr;
        }
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_hub_list, std::max(1, hub_capacity) * sizeof(int)));

        int small_size = 0;  // vertices in d_worklist, one thread each
        int hub_size = 0;    // vertices in d_hub_list, one warp each
        int work_size = 0;   // all active vertices = small_size + hub_size
        auto build_lists = [&]() {
          MAXFLOW_CUDA_CHECK(cudaMemset(d_counts, 0, 2 * sizeof(int)));
          wl_build_worklist_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, hub_deg, d_offset, d_excess, d_height, d_worklist, d_hub_list, d_counts);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
          int h_counts[2];
          MAXFLOW_CUDA_CHECK(cudaMemcpy(h_counts, d_counts, 2 * sizeof(int), cudaMemcpyDeviceToHost));
          small_size = h_counts[0];
          hub_size = h_counts[1];
          work_size = small_size + hub_size;
        };

#ifdef MAXFLOW_PROFILE
        double t_worklist = 0, t_bfs = 0, t_push = 0, t_remove = 0;
        double t_bfs_kernel = 0, t_bfs_copy = 0;
        long n_outer = 0, n_bfs_levels = 0, n_relabels = 0;
        //  Total threads actually launched, so the saving over the topology-driven solver can be quoted as a number
        long long threads_push = 0, threads_remove = 0;
        std::vector<int> active_history;
        //  Diagnostics: how evenly the work is spread over the threads (profile build only)
        double t_bfs_level0 = 0;
        long long wl_passes = 0, wl_sum_size = 0, wl_max_size = 0;
        long long wl_sum_edges = 0, wl_sum_maxdeg = 0, wl_max_maxdeg = 0, wl_hub_passes = 0;
        long long wl_sum_threads = 0, wl_sum_hubs = 0;
        std::vector<int> h_wl, h_hub;
        auto deg = [&](int v) { return (long long)(net.offset[v + 1] - net.offset[v]); };
        long long max_deg = 0;
        int max_deg_v = -1, n_deg64 = 0, n_deg1024 = 0;
        for (int v = 0; v < V; v++) {
          if (v == net.source || v == net.sink) {
            continue;
          }
          long long d = deg(v);
          if (d > max_deg) {
            max_deg = d;
            max_deg_v = v;
          }
          if (d >= 64) {
            n_deg64++;
          }
          if (d >= 1024) {
            n_deg1024++;
          }
        }
        auto clk = []() { return std::chrono::high_resolution_clock::now(); };
        auto secs = [](std::chrono::high_resolution_clock::time_point a, std::chrono::high_resolution_clock::time_point b) {
          return std::chrono::duration<double>(b - a).count();
        };
#endif

        int outer_iter = 0;
        bool heights_fresh = false;

        while (true) {
          //  Build the worklist. Its size doubles as the termination test, so no separate check-active pass is needed
#ifdef MAXFLOW_PROFILE
          auto tw0 = clk();
#endif
          build_lists();
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
                if (level == 0 && sink_level_parallel) {
                  gpu_bfs_sink_level_kernel<<<blocks_sink, threads>>>(sink_start, sink_end, V, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
                } else {
                  gpu_bfs_step_kernel<<<blocks_v, threads>>>(V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
                }
                MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
                t_bfs_kernel += secs(tk0, clk());
                if (level == 0) {
                  t_bfs_level0 += secs(tk0, clk());
                }
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
                if (level == 0 && sink_level_parallel) {
                  gpu_bfs_frontier_sink_level_kernel<<<blocks_sink, threads>>>(sink_start, sink_end, V, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_frontier_out, d_frontier_count);
                } else {
                  gpu_bfs_frontier_step_kernel<<<blocks_f, threads>>>(frontier_size, V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_frontier_in, d_frontier_out, d_frontier_count);
                }
                MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
                t_bfs_kernel += secs(tk0, clk());
                if (level == 0) {
                  t_bfs_level0 += secs(tk0, clk());
                }
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
            build_lists();
          }

          if (work_size > 0) {
            //  Algorithm 2: one thread per ACTIVE vertex
#ifdef MAXFLOW_PROFILE
            {
              //  Copy this pass's lists to the host and measure them; done before the timer starts.
              //  max_d = edges scanned by the busiest GPU thread: a small vertex's whole edge list,
              //  or ceil(deg/32) for a hub, whose edges are shared by the 32 lanes of a warp
              h_wl.resize(small_size);
              h_hub.resize(hub_size);
              if (small_size > 0) {
                MAXFLOW_CUDA_CHECK(cudaMemcpy(h_wl.data(), d_worklist, small_size * sizeof(int), cudaMemcpyDeviceToHost));
              }
              if (hub_size > 0) {
                MAXFLOW_CUDA_CHECK(cudaMemcpy(h_hub.data(), d_hub_list, hub_size * sizeof(int), cudaMemcpyDeviceToHost));
              }
              long long sum_d = 0, max_d = 0;
              for (int i = 0; i < small_size; i++) {
                long long d = deg(h_wl[i]);
                sum_d += d;
                if (d > max_d) {
                  max_d = d;
                }
              }
              for (int i = 0; i < hub_size; i++) {
                long long d = deg(h_hub[i]);
                sum_d += d;
                if ((d + 31) / 32 > max_d) {
                  max_d = (d + 31) / 32;
                }
              }
              wl_passes++;
              wl_sum_threads += (long long)small_size + 32LL * hub_size;
              wl_sum_hubs += hub_size;
              wl_sum_size += work_size;
              if (work_size > wl_max_size) {
                wl_max_size = work_size;
              }
              wl_sum_edges += sum_d;
              wl_sum_maxdeg += max_d;
              if (max_d > wl_max_maxdeg) {
                wl_max_maxdeg = max_d;
              }
              if (max_d >= 1024) {
                wl_hub_passes++;
              }
            }
            auto tp0 = clk();
            threads_push += work_size;
#endif
            if (small_size > 0) {
              int blocks_w = (small_size + threads - 1) / threads;
              wl_push_relabel_kernel<<<blocks_w, threads>>>(small_size, V, kernel_cycles, d_worklist, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
            }
            if (hub_size > 0) {
              int blocks_h = (hub_size * 32 + threads - 1) / threads;  // one warp per hub
              wl_push_relabel_hub_kernel<<<blocks_h, threads>>>(hub_size, V, kernel_cycles, d_hub_list, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
            }
            MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
#ifdef MAXFLOW_PROFILE
            t_push += secs(tp0, clk());
#endif

            if (rebuild_before_remove) {
              build_lists();
            }

            //  Algorithm 3: one thread per worklist entry
            if (work_size > 0) {
#ifdef MAXFLOW_PROFILE
              auto tr0 = clk();
              threads_remove += work_size;
#endif
              if (small_size > 0) {
                int blocks_r = (small_size + threads - 1) / threads;
                wl_remove_invalid_edges_kernel<<<blocks_r, threads>>>(small_size, d_worklist, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
              }
              if (hub_size > 0) {
                int blocks_h = (hub_size * 32 + threads - 1) / threads;  // one warp per hub
                wl_remove_invalid_hub_kernel<<<blocks_h, threads>>>(hub_size, d_hub_list, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
              }
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
          {
            double avg_size  = wl_passes > 0 ? (double)wl_sum_size / wl_passes : 0.0;
            double avg_edges = wl_passes > 0 ? (double)wl_sum_edges / wl_passes : 0.0;
            double avg_maxd  = wl_passes > 0 ? (double)wl_sum_maxdeg / wl_passes : 0.0;
            double avg_threads = wl_passes > 0 ? (double)wl_sum_threads / wl_passes : 0.0;
            double avg_hubs = wl_passes > 0 ? (double)wl_sum_hubs / wl_passes : 0.0;
            double avg_per_thread = avg_threads > 0 ? avg_edges / avg_threads : 0.0;
            double imbalance = avg_per_thread > 0 ? avg_maxd / avg_per_thread : 0.0;
            double l0_share = t_bfs_kernel > 0 ? 100.0 * t_bfs_level0 / t_bfs_kernel : 0.0;
            std::fprintf(stderr, "  BFS level 0 only : %8.3f s  (%5.1f%% of BFS kernel time)\n", t_bfs_level0, l0_share);
            std::fprintf(stderr, "  degree           : sink %lld  source %lld  largest other %lld (vertex %d)  vertices with >=64 edges %d, >=1024 edges %d\n",
                         deg(net.sink), deg(net.source), max_deg, max_deg_v, n_deg64, n_deg1024);
            std::fprintf(stderr, "  worklist per pass: average %.1f vertices (%.1f of them hubs, one warp each), largest %lld\n", avg_size, avg_hubs, wl_max_size);
            std::fprintf(stderr, "  hub threshold    : %d edges (0 = Fix B off)\n", hub_deg);
            std::fprintf(stderr, "  edges per pass   : total %.1f, per thread %.1f, longest thread %.1f (worst %lld)\n",
                         avg_edges, avg_per_thread, avg_maxd, wl_max_maxdeg);
            std::fprintf(stderr, "  imbalance        : longest thread does %.1fx the average; passes with a >=1024-edge thread: %lld of %lld\n",
                         imbalance, wl_hub_passes, wl_passes);
            std::fprintf(stderr, "[diag] V=%d E=%d sink_deg=%lld max_deg=%lld n_deg64=%d n_deg1024=%d relabels=%ld bfs_levels=%ld bfs_kernel_s=%.6f bfs_l0_s=%.6f passes=%lld avg_wl=%.1f max_wl=%lld avg_edges=%.1f avg_per_thread=%.2f avg_longest=%.1f worst_longest=%lld imbalance=%.1f hub_passes=%lld avg_hubs=%.1f hub_deg=%d\n",
                         V, E, deg(net.sink), max_deg, n_deg64, n_deg1024, n_relabels, n_bfs_levels, t_bfs_kernel, t_bfs_level0,
                         wl_passes, avg_size, wl_max_size, avg_edges, avg_per_thread, avg_maxd, wl_max_maxdeg, imbalance, wl_hub_passes, avg_hubs, hub_deg);
          }
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
      int* d_counts = nullptr;
      int* d_hub_list = nullptr;

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