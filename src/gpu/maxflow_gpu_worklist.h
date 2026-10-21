// push-relabel max-flow on the GPU, data driven: every pass only the active vertices get threads
// vertices are split by degree, small ones get a thread, hubs get a warp and big ones get a block
// usage:
//   gpu_config().hub_deg = 64;              optional, defaults are the tuned values
//   gpu_worklist_solver s(net);
//   cap_t f = s.solve();
//   bool src = s.is_on_source_side(v);

#ifndef MAXFLOW_GPU_WORKLIST_H
#define MAXFLOW_GPU_WORKLIST_H

#include <cuda_runtime.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "src/common/metrics.h"
#include "src/common/types.h"
#include "src/cpu/graph_csr.h"

namespace maxflow {

  // [CHECK] stop the program with file and line when a CUDA call fails
  #define MAXFLOW_CUDA_CHECK(call) do {                                \
    cudaError_t err_ = (call);                                         \
    if (err_ != cudaSuccess) {                                         \
      std::cerr << "CUDA error at " << __FILE__ << ":" << __LINE__     \
                << " : " << cudaGetErrorString(err_) << "\n";          \
      std::exit(EXIT_FAILURE);                                         \
    }                                                                  \
  } while (0)

  // [SETTINGS] tuning values, set from e2e_solve flags, defaults are the values used in the report
  struct gpu_settings {
    bool sink_bfs = true;       // fix A, BFS level 0 with one thread per sink edge
    int hub_deg = 64;           // fix B, vertices with at least this many edges get a warp, 0 = off
    int block_deg = 1024;       // fix C, vertices with at least this many edges get a block, 0 = off
    bool bfs_split = true;      // fix D, BFS levels also split into thread, warp and block kernels
    int block_threads = 0;      // fix D2, threads per block for big vertices, 0 = chosen from the largest vertex
    bool async = true;          // fix E, wait for the GPU only once per pass
    int gr_period = 75;         // passes between two global relabels
    int kernel_cycles = 0;      // push or relabel steps per vertex in one pass, 0 = |E|/|V|
    bool kernel_times = false;  // time each step of the loop, adds waits so only for testing
  };

  inline gpu_settings& gpu_config() {
    static gpu_settings s;
    return s;
  }

  // [INIT] algorithm 1, residual = capacity, excess = 0, height = 0 and |V| for the source
  __global__ void gpu_initialize_kernel(int num_nodes, int num_edges, int source, const cap_t* capacity, cap_t* residual_capacity, cap_t* excess, int* height) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid < num_edges) {
      residual_capacity[tid] = capacity[tid];
    }
    if (tid < num_nodes) {
      excess[tid] = cap_t(0);
      height[tid] = (tid == source) ? num_nodes : 0;
    }
  }

  // [INIT] algorithm 1, one thread per source edge pushes its full capacity
  __global__ void gpu_saturate_source_kernel(int src_start, int src_end, int source, const int* edge_dst, const cap_t* capacity, cap_t* residual_capacity, const int* reverse_index, cap_t* excess) {
    int e = src_start + blockIdx.x * blockDim.x + threadIdx.x;
    if (e >= src_end) {
      return;
    }
    cap_t cap = capacity[e];
    if (cap > MAXFLOW_EPSILON) {
      residual_capacity[e] = cap_t(0);
      atomicAdd(&residual_capacity[reverse_index[e]], cap);
      atomicAdd(&excess[edge_dst[e]], cap);
      atomicAdd(&excess[source], -cap);
    }
  }

  // [BFS] algorithm 4, all heights = |V| (not reached), sink = 0
  __global__ void gpu_bfs_init_kernel(int num_nodes, int sink, int* height) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid < num_nodes) {
      height[tid] = (tid == sink) ? 0 : num_nodes;
    }
  }

  // [BFS-SINK] fix A, level 0 has only the sink, so one thread per sink edge instead of one thread for all
  __global__ void gpu_bfs_sink_level_kernel(int sink_start, int sink_end, int num_nodes, const int* edge_dst, const cap_t* residual_capacity, const int* reverse_index, int* height, int* flag) {
    int e = sink_start + blockIdx.x * blockDim.x + threadIdx.x;
    if (e >= sink_end) {
      return;
    }
    int v = edge_dst[e];
    if (residual_capacity[reverse_index[e]] > MAXFLOW_EPSILON && height[v] == num_nodes) {
      height[v] = 1;
      *flag = 1;
    }
  }

  // [BFS] one level, one thread per vertex, used when fix D is off
  __global__ void gpu_bfs_step_kernel(int num_nodes, int level, const int* offset, const int* edge_dst, const cap_t* residual_capacity, const int* reverse_index, int* height, int* flag) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || height[u] != level) {
      return;
    }
    for (int e = offset[u]; e < offset[u + 1]; e++) {
      int v = edge_dst[e];
      // [BFS] v reaches u when v -> u still has residual capacity
      if (residual_capacity[reverse_index[e]] > MAXFLOW_EPSILON && height[v] == num_nodes) {
        height[v] = level + 1;
        *flag = 1;
      }
    }
  }

  // [BFS-SPLIT] fix D, small vertices of the level, one thread each, bigger ones are in the fixed lists
  // all writes in a level are height = level + 1 on unvisited vertices, so the order of threads does not matter
  __global__ void wl_bfs_step_small_kernel(int num_nodes, int level, int skip_deg, const int* offset, const int* edge_dst, const cap_t* residual_capacity, const int* reverse_index, int* height, int* flag) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || height[u] != level) {
      return;
    }
    int start = offset[u];
    int end = offset[u + 1];
    if (end - start >= skip_deg) {
      return;
    }
    for (int e = start; e < end; e++) {
      int v = edge_dst[e];
      if (residual_capacity[reverse_index[e]] > MAXFLOW_EPSILON && height[v] == num_nodes) {
        height[v] = level + 1;
        *flag = 1;
      }
    }
  }

  // [BFS-SPLIT] fix D, hub vertices, one warp each, a warp leaves at once if its vertex is not on this level
  __global__ void wl_bfs_step_hub_kernel(int list_size, int num_nodes, int level, const int* list, const int* offset, const int* edge_dst, const cap_t* residual_capacity, const int* reverse_index, int* height, int* flag) {
    int warp = (blockIdx.x * blockDim.x + threadIdx.x) / 32;
    int lane = threadIdx.x & 31;
    if (warp >= list_size) {
      return;
    }
    int u = list[warp];
    if (height[u] != level) {
      return;
    }
    // [BFS-SPLIT] lanes read neighbouring edges, so the loads are coalesced
    for (int e = offset[u] + lane; e < offset[u + 1]; e += 32) {
      int v = edge_dst[e];
      if (residual_capacity[reverse_index[e]] > MAXFLOW_EPSILON && height[v] == num_nodes) {
        height[v] = level + 1;
        *flag = 1;
      }
    }
  }

  // [BFS-SPLIT] fix D, big vertices, one block each
  __global__ void wl_bfs_step_block_kernel(int list_size, int num_nodes, int level, const int* list, const int* offset, const int* edge_dst, const cap_t* residual_capacity, const int* reverse_index, int* height, int* flag) {
    if ((int)blockIdx.x >= list_size) {
      return;
    }
    int u = list[blockIdx.x];
    if (height[u] != level) {
      return;
    }
    for (int e = offset[u] + threadIdx.x; e < offset[u + 1]; e += blockDim.x) {
      int v = edge_dst[e];
      if (residual_capacity[reverse_index[e]] > MAXFLOW_EPSILON && height[v] == num_nodes) {
        height[v] = level + 1;
        *flag = 1;
      }
    }
  }

  // [LIST] active vertex = not source or sink, excess > 0 and height < |V|
  // each active vertex goes to one of three lists by its degree, counts[0..2] are the list sizes
  __global__ void wl_build_worklist_kernel(int num_nodes, int source, int sink, int hub_deg, int block_deg, const int* offset, const cap_t* excess, const int* height, int* worklist, int* hub_list, int* big_list, int* counts) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }
    if (excess[u] > MAXFLOW_EPSILON && height[u] < num_nodes) {
      int d = offset[u + 1] - offset[u];
      if (block_deg > 0 && d >= block_deg) {
        big_list[atomicAdd(&counts[2], 1)] = u;
      } else if (hub_deg > 0 && d >= hub_deg) {
        hub_list[atomicAdd(&counts[1], 1)] = u;
      } else {
        worklist[atomicAdd(&counts[0], 1)] = u;
      }
    }
  }

  // [PUSH] algorithm 2, one thread per small active vertex
  __global__ void wl_push_relabel_kernel(int work_size, int num_nodes, int kernel_cycles, const int* worklist, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, int* height) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= work_size) {
      return;
    }
    int u = worklist[idx];

    for (int cnt = 0; cnt < kernel_cycles; cnt++) {
      if (!(height[u] < num_nodes && excess[u] > MAXFLOW_EPSILON)) {
        break;
      }

      // [PUSH] lowest neighbour over residual edges
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
        // [PUSH] send min(excess, residual) to the lowest neighbour
        cap_t a = excess[u];
        cap_t b = residual_capacity[e_hat];
        cap_t d = (a < b) ? a : b;
        atomicAdd(&residual_capacity[e_hat], -d);
        atomicAdd(&residual_capacity[reverse_index[e_hat]], d);
        atomicAdd(&excess[u], -d);
        atomicAdd(&excess[v_hat], d);
      } else {
        // [RELABEL] lift u just above its lowest neighbour
        height[u] = lowest_h + 1;
      }
    }
  }

  // [REMOVE] algorithm 3, one thread per small active vertex, cancel residual on edges with h(u) > h(v) + 1
  __global__ void wl_remove_invalid_edges_kernel(int work_size, const int* worklist, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, const int* height) {
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

  // [PUSH-HUB] fix B, one warp per hub vertex, same rule as wl_push_relabel_kernel
  __global__ void wl_push_relabel_hub_kernel(int hub_size, int num_nodes, int kernel_cycles, const int* hub_list, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, int* height) {
    const unsigned FULL = 0xffffffffu;
    int warp = (blockIdx.x * blockDim.x + threadIdx.x) / 32;
    int lane = threadIdx.x & 31;
    if (warp >= hub_size) {
      return;
    }
    int u = hub_list[warp];
    int start = offset[u];
    int end = offset[u + 1];

    for (int cnt = 0; cnt < kernel_cycles; cnt++) {
      // [PUSH-HUB] lane 0 checks if u is still active and tells the warp
      int active = 0;
      if (lane == 0) {
        active = (height[u] < num_nodes && excess[u] > MAXFLOW_EPSILON) ? 1 : 0;
      }
      active = __shfl_sync(FULL, active, 0);
      if (!active) {
        break;
      }

      // [PUSH-HUB] each lane finds the lowest neighbour in its own edges lane, lane+32 and so on
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

      // [PUSH-HUB] warp reduction, lowest height wins, ties go to the smaller edge like the serial loop
      for (int off = 16; off > 0; off >>= 1) {
        int oh = __shfl_down_sync(FULL, best_h, off);
        int oe = __shfl_down_sync(FULL, best_e, off);
        if (oh < best_h || (oh == best_h && oe < best_e)) {
          best_h = oh;
          best_e = oe;
        }
      }

      // [PUSH-HUB] lane 0 does the push or relabel
      int stop = 0;
      if (lane == 0) {
        if (best_h > num_nodes) {
          stop = 1;
        } else if (height[u] > best_h) {
          int v_hat = edge_dst[best_e];
          cap_t a = excess[u];
          cap_t b = residual_capacity[best_e];
          cap_t d = (a < b) ? a : b;
          atomicAdd(&residual_capacity[best_e], -d);
          atomicAdd(&residual_capacity[reverse_index[best_e]], d);
          atomicAdd(&excess[u], -d);
          atomicAdd(&excess[v_hat], d);
        } else {
          height[u] = best_h + 1;
        }
      }
      __syncwarp();
      stop = __shfl_sync(FULL, stop, 0);
      if (stop) {
        break;
      }
    }
  }

  // [REMOVE-HUB] fix B, one warp per hub vertex, lanes share the edge list
  __global__ void wl_remove_invalid_hub_kernel(int hub_size, const int* hub_list, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, const int* height) {
    int warp = (blockIdx.x * blockDim.x + threadIdx.x) / 32;
    int lane = threadIdx.x & 31;
    if (warp >= hub_size) {
      return;
    }
    int u = hub_list[warp];
    int hu = height[u];
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

  // [PUSH-BLOCK] fix C, one block per big vertex, warps reduce with shuffles and meet in shared memory
  __global__ void wl_push_relabel_block_kernel(int big_size, int num_nodes, int kernel_cycles, const int* big_list, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, int* height) {
    const unsigned FULL = 0xffffffffu;
    __shared__ int s_h[32];
    __shared__ int s_e[32];
    __shared__ int s_active;
    __shared__ int s_stop;
    if ((int)blockIdx.x >= big_size) {
      return;
    }
    int tid = threadIdx.x;
    int lane = tid & 31;
    int wid = tid >> 5;
    int nwarps = blockDim.x >> 5;
    int u = big_list[blockIdx.x];
    int start = offset[u];
    int end = offset[u + 1];

    for (int cnt = 0; cnt < kernel_cycles; cnt++) {
      // [PUSH-BLOCK] thread 0 checks if u is still active
      if (tid == 0) {
        s_active = (height[u] < num_nodes && excess[u] > MAXFLOW_EPSILON) ? 1 : 0;
      }
      __syncthreads();
      if (!s_active) {
        break;
      }

      // [PUSH-BLOCK] each thread scans edges tid, tid+blockDim and so on
      int best_h = num_nodes + 1;
      int best_e = INT_MAX;
      for (int e = start + tid; e < end; e += blockDim.x) {
        if (residual_capacity[e] > MAXFLOW_EPSILON) {
          int h = height[edge_dst[e]];
          if (h < best_h) {
            best_h = h;
            best_e = e;
          }
        }
      }

      // [PUSH-BLOCK] reduction inside each warp
      for (int off = 16; off > 0; off >>= 1) {
        int oh = __shfl_down_sync(FULL, best_h, off);
        int oe = __shfl_down_sync(FULL, best_e, off);
        if (oh < best_h || (oh == best_h && oe < best_e)) {
          best_h = oh;
          best_e = oe;
        }
      }
      if (lane == 0) {
        s_h[wid] = best_h;
        s_e[wid] = best_e;
      }
      __syncthreads();

      // [PUSH-BLOCK] warp 0 reduces the warp results, then thread 0 does the push or relabel
      if (wid == 0) {
        best_h = (lane < nwarps) ? s_h[lane] : num_nodes + 1;
        best_e = (lane < nwarps) ? s_e[lane] : INT_MAX;
        for (int off = 16; off > 0; off >>= 1) {
          int oh = __shfl_down_sync(FULL, best_h, off);
          int oe = __shfl_down_sync(FULL, best_e, off);
          if (oh < best_h || (oh == best_h && oe < best_e)) {
            best_h = oh;
            best_e = oe;
          }
        }
        if (lane == 0) {
          int stop = 0;
          if (best_h > num_nodes) {
            stop = 1;
          } else if (height[u] > best_h) {
            int v_hat = edge_dst[best_e];
            cap_t a = excess[u];
            cap_t b = residual_capacity[best_e];
            cap_t d = (a < b) ? a : b;
            atomicAdd(&residual_capacity[best_e], -d);
            atomicAdd(&residual_capacity[reverse_index[best_e]], d);
            atomicAdd(&excess[u], -d);
            atomicAdd(&excess[v_hat], d);
          } else {
            height[u] = best_h + 1;
          }
          s_stop = stop;
        }
      }
      __syncthreads();
      if (s_stop) {
        break;
      }
    }
  }

  // [REMOVE-BLOCK] fix C, one block per big vertex, each thread sums what it removes and does one atomic at the end
  __global__ void wl_remove_invalid_block_kernel(int big_size, const int* big_list, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, const int* height) {
    if ((int)blockIdx.x >= big_size) {
      return;
    }
    int u = big_list[blockIdx.x];
    int hu = height[u];
    cap_t removed = cap_t(0);
    for (int e = offset[u] + threadIdx.x; e < offset[u + 1]; e += blockDim.x) {
      int v = edge_dst[e];
      cap_t rc = residual_capacity[e];
      if (rc > MAXFLOW_EPSILON && hu > height[v] + 1) {
        residual_capacity[e] = cap_t(0);
        atomicAdd(&residual_capacity[reverse_index[e]], rc);
        atomicAdd(&excess[v], rc);
        removed += rc;
      }
    }
    if (removed > cap_t(0)) {
      atomicAdd(&excess[u], -removed);
    }
  }

  // [HOST] solver object, owns all device memory of one flow network
  class gpu_worklist_solver {
    public:
      // [ALLOC] allocate device arrays and copy the graph, timed as time.gpu.setup
      explicit gpu_worklist_solver(flow_network<cap_t>& net) : net(net) {
        double t0 = now_sec();
        int V = net.num_nodes;
        int E = net.num_edges;

        // [MEM] free memory before our allocations, the drop later is what this solver takes
        size_t total_b = 0;
        MAXFLOW_CUDA_CHECK(cudaMemGetInfo(&free_before, &total_b));

        dev_alloc(&d_offset, V + 1);
        dev_alloc(&d_edge_dst, E);
        dev_alloc(&d_capacity, E);
        dev_alloc(&d_residual_capacity, E);
        dev_alloc(&d_reverse_index, E);
        dev_alloc(&d_excess, V);
        dev_alloc(&d_height, V);
        dev_alloc(&d_flag, 1);
        dev_alloc(&d_worklist, V);
        dev_alloc(&d_counts, 3);

        // [ALLOC] small pinned host buffer for the list sizes and the BFS flag, copies to it are direct
        MAXFLOW_CUDA_CHECK(cudaMallocHost(&h_pinned, 4 * sizeof(int)));

        // [COPY] graph to the device
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_offset, net.offset.data(), (V + 1) * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_edge_dst, net.edge_dst.data(), E * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_capacity, net.capacity.data(), E * sizeof(cap_t), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_reverse_index, net.reverse_index.data(), E * sizeof(int), cudaMemcpyHostToDevice));

        mark("time.gpu.setup", t0);
      }

      ~gpu_worklist_solver() {
        cudaFree(d_offset);
        cudaFree(d_edge_dst);
        cudaFree(d_capacity);
        cudaFree(d_residual_capacity);
        cudaFree(d_reverse_index);
        cudaFree(d_excess);
        cudaFree(d_height);
        cudaFree(d_flag);
        cudaFree(d_worklist);
        cudaFree(d_counts);
        cudaFree(d_hub_list);
        cudaFree(d_big_list);
        cudaFree(d_bfs_hub_list);
        cudaFree(d_bfs_big_list);
        cudaFreeHost(h_pinned);
      }

      gpu_worklist_solver(const gpu_worklist_solver&) = delete;
      gpu_worklist_solver& operator=(const gpu_worklist_solver&) = delete;

      // [SOLVE] run push-relabel to the end, return the flow value
      cap_t solve() {
        const gpu_settings& st = gpu_config();
        double t_solve = now_sec();
        int V = net.num_nodes;
        int E = net.num_edges;
        int threads = 256;
        int blocks_v = (V + threads - 1) / threads;
        int blocks_max = (std::max(V, E) + threads - 1) / threads;

        int kernel_cycles = st.kernel_cycles > 0 ? st.kernel_cycles : std::max(1, E / std::max(1, V));
        int gr_period = st.gr_period > 0 ? st.gr_period : 75;

        // [SETTINGS] degree thresholds, a list that would stay empty is switched off
        int hub_deg = std::max(0, st.hub_deg);
        int block_deg = std::max(0, st.block_deg);
        int bfs_hub_deg = hub_deg;
        int bfs_block_deg = block_deg;
        int largest = 0;
        int hub_capacity = 0;
        int big_capacity = 0;
        for (int v = 0; v < V; v++) {
          if (v == net.source || v == net.sink) {
            continue;
          }
          int d = net.offset[v + 1] - net.offset[v];
          largest = std::max(largest, d);
          if (block_deg > 0 && d >= block_deg) {
            big_capacity++;
          } else if (hub_deg > 0 && d >= hub_deg) {
            hub_capacity++;
          }
        }
        if (hub_capacity == 0) {
          hub_deg = 0;
        }
        if (big_capacity == 0) {
          block_deg = 0;
        }

        // [SETTINGS] fix D2, smallest of 256, 512, 1024 that gives each thread at most 16 edges of the largest vertex
        int block_threads = 256;
        if (st.block_threads >= 32 && st.block_threads <= 1024 && st.block_threads % 32 == 0) {
          block_threads = st.block_threads;
        } else {
          while (block_threads < 1024 && (largest + block_threads - 1) / block_threads > 16) {
            block_threads *= 2;
          }
        }

        // [ALLOC] lists for hub and big active vertices, rebuilt every pass
        dev_alloc(&d_hub_list, std::max(1, hub_capacity));
        dev_alloc(&d_big_list, std::max(1, big_capacity));

        // [BFS-SPLIT] fix D, fixed hub and big lists for the BFS levels, built once, source and sink included
        bool bfs_split = st.bfs_split;
        std::vector<int> h_bfs_hub;
        std::vector<int> h_bfs_big;
        if (bfs_split) {
          for (int v = 0; v < V; v++) {
            int d = net.offset[v + 1] - net.offset[v];
            if (bfs_block_deg > 0 && d >= bfs_block_deg) {
              h_bfs_big.push_back(v);
            } else if (bfs_hub_deg > 0 && d >= bfs_hub_deg) {
              h_bfs_hub.push_back(v);
            }
          }
          if (h_bfs_hub.empty() && h_bfs_big.empty()) {
            bfs_split = false;
          }
        }
        int bfs_skip_deg = INT_MAX;
        if (bfs_hub_deg > 0) {
          bfs_skip_deg = std::min(bfs_skip_deg, bfs_hub_deg);
        }
        if (bfs_block_deg > 0) {
          bfs_skip_deg = std::min(bfs_skip_deg, bfs_block_deg);
        }
        int bfs_hub_size = static_cast<int>(h_bfs_hub.size());
        int bfs_big_size = static_cast<int>(h_bfs_big.size());
        dev_alloc(&d_bfs_hub_list, std::max(1, bfs_hub_size));
        dev_alloc(&d_bfs_big_list, std::max(1, bfs_big_size));
        if (bfs_hub_size > 0) {
          MAXFLOW_CUDA_CHECK(cudaMemcpy(d_bfs_hub_list, h_bfs_hub.data(), bfs_hub_size * sizeof(int), cudaMemcpyHostToDevice));
        }
        if (bfs_big_size > 0) {
          MAXFLOW_CUDA_CHECK(cudaMemcpy(d_bfs_big_list, h_bfs_big.data(), bfs_big_size * sizeof(int), cudaMemcpyHostToDevice));
        }
        int blocks_bfs_hub = (bfs_hub_size * 32 + threads - 1) / threads;

        // [MEM] all device memory is allocated now, so this is the peak of the solve
        record_memory();
        metrics::get().keep_max("gpu.block_threads", block_threads);

        // [INIT] reset arrays and saturate the source edges
        gpu_initialize_kernel<<<blocks_max, threads>>>(V, E, net.source, d_capacity, d_residual_capacity, d_excess, d_height);
        MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
        int src_start = net.offset[net.source];
        int src_end = net.offset[net.source + 1];
        if (src_end > src_start) {
          int blocks_s = (src_end - src_start + threads - 1) / threads;
          gpu_saturate_source_kernel<<<blocks_s, threads>>>(src_start, src_end, net.source, d_edge_dst, d_capacity, d_residual_capacity, d_reverse_index, d_excess);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
        }

        // [BFS-SINK] fix A, edge range of the sink for level 0
        int sink_start = net.offset[net.sink];
        int sink_end = net.offset[net.sink + 1];
        int blocks_sink = (sink_end - sink_start + threads - 1) / threads;
        bool sink_bfs = st.sink_bfs && sink_end > sink_start;

        // [SYNC] fix E, after a launch only check for launch errors, the next read of the list sizes waits for everything
        auto after_launch = [&]() {
          if (st.async) {
            MAXFLOW_CUDA_CHECK(cudaGetLastError());
          } else {
            MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
          }
        };

        // [TIME] optional step timers, each one waits for the GPU first so only use for testing
        double t_step = now_sec();
        auto step_time = [&](const char* name) {
          if (st.kernel_times) {
            MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
            t_step = mark(name, t_step);
          }
        };

        // [LIST] build the three lists of active vertices and read their sizes
        int small_size = 0;
        int hub_size = 0;
        int big_size = 0;
        int work_size = 0;
        auto build_lists = [&]() {
          MAXFLOW_CUDA_CHECK(cudaMemsetAsync(d_counts, 0, 3 * sizeof(int)));
          wl_build_worklist_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, hub_deg, block_deg, d_offset, d_excess, d_height, d_worklist, d_hub_list, d_big_list, d_counts);
          MAXFLOW_CUDA_CHECK(cudaGetLastError());
          MAXFLOW_CUDA_CHECK(cudaMemcpyAsync(h_pinned, d_counts, 3 * sizeof(int), cudaMemcpyDeviceToHost));
          MAXFLOW_CUDA_CHECK(cudaStreamSynchronize(0));
          small_size = h_pinned[0];
          hub_size = h_pinned[1];
          big_size = h_pinned[2];
          work_size = small_size + hub_size + big_size;
        };

        // [BFS] global relabel, one BFS level per launch until no new vertex is reached
        auto global_relabel = [&]() {
          gpu_bfs_init_kernel<<<blocks_v, threads>>>(V, net.sink, d_height);
          after_launch();
          for (int level = 0; ; level++) {
            MAXFLOW_CUDA_CHECK(cudaMemsetAsync(d_flag, 0, sizeof(int)));
            if (level == 0 && sink_bfs) {
              gpu_bfs_sink_level_kernel<<<blocks_sink, threads>>>(sink_start, sink_end, V, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
            } else if (bfs_split) {
              wl_bfs_step_small_kernel<<<blocks_v, threads>>>(V, level, bfs_skip_deg, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
              if (bfs_hub_size > 0) {
                wl_bfs_step_hub_kernel<<<blocks_bfs_hub, threads>>>(bfs_hub_size, V, level, d_bfs_hub_list, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
              }
              if (bfs_big_size > 0) {
                wl_bfs_step_block_kernel<<<bfs_big_size, block_threads>>>(bfs_big_size, V, level, d_bfs_big_list, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
              }
            } else {
              gpu_bfs_step_kernel<<<blocks_v, threads>>>(V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
            }
            after_launch();
            // [BFS] one wait per level, the flag says if this level reached anyone
            MAXFLOW_CUDA_CHECK(cudaMemcpyAsync(h_pinned + 3, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
            MAXFLOW_CUDA_CHECK(cudaStreamSynchronize(0));
            bfs_levels++;
            if (h_pinned[3] == 0) {
              break;
            }
          }
        };

        // [LOOP] each pass: build lists, maybe relabel, push-relabel, remove invalid edges
        long passes = 0;
        long relabels = 0;
        int outer_iter = 0;
        bool heights_fresh = false;
        while (true) {
          t_step = now_sec();
          build_lists();
          passes++;
          step_time("time.gpu.k_build");

          // [LOOP] no active vertex left, stop only if the heights are fresh, else relabel once more and check again
          if (work_size == 0) {
            if (heights_fresh) {
              break;
            }
            outer_iter = 0;
          }

          bool do_relabel = (outer_iter % gr_period == 0);
          heights_fresh = do_relabel;
          if (do_relabel) {
            global_relabel();
            relabels++;
            step_time("time.gpu.k_bfs");
            // [LOOP] the relabel can make vertices inactive, so build the lists again
            if (work_size > 0) {
              build_lists();
              step_time("time.gpu.k_build");
            }
          }

          if (work_size > 0) {
            // [PUSH] one launch per list, thread, warp or block per vertex
            if (small_size > 0) {
              wl_push_relabel_kernel<<<(small_size + threads - 1) / threads, threads>>>(small_size, V, kernel_cycles, d_worklist, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
            }
            if (hub_size > 0) {
              wl_push_relabel_hub_kernel<<<(hub_size * 32 + threads - 1) / threads, threads>>>(hub_size, V, kernel_cycles, d_hub_list, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
            }
            if (big_size > 0) {
              wl_push_relabel_block_kernel<<<big_size, block_threads>>>(big_size, V, kernel_cycles, d_big_list, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
            }
            after_launch();
            step_time("time.gpu.k_push");

            // [REMOVE] same three lists
            if (small_size > 0) {
              wl_remove_invalid_edges_kernel<<<(small_size + threads - 1) / threads, threads>>>(small_size, d_worklist, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
            }
            if (hub_size > 0) {
              wl_remove_invalid_hub_kernel<<<(hub_size * 32 + threads - 1) / threads, threads>>>(hub_size, d_hub_list, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
            }
            if (big_size > 0) {
              wl_remove_invalid_block_kernel<<<big_size, block_threads>>>(big_size, d_big_list, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
            }
            after_launch();
            step_time("time.gpu.k_remove");
          }
          outer_iter++;
        }

        // [COUNT] loop counters, summed over kernelization rounds
        metrics::get().add("gpu.passes", passes);
        metrics::get().add("gpu.relabels", relabels);
        metrics::get().add("gpu.bfs_levels", bfs_levels);
        t_solve = mark("time.gpu.solve", t_solve);

        // [COPY] flow value and the residual graph back to the host
        cap_t flow;
        MAXFLOW_CUDA_CHECK(cudaMemcpy(&flow, d_excess + net.sink, sizeof(cap_t), cudaMemcpyDeviceToHost));
        h_residual_capacity.resize(E);
        h_excess.resize(V);
        MAXFLOW_CUDA_CHECK(cudaMemcpy(h_residual_capacity.data(), d_residual_capacity, E * sizeof(cap_t), cudaMemcpyDeviceToHost));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(h_excess.data(), d_excess, V * sizeof(cap_t), cudaMemcpyDeviceToHost));

        compute_source_side();
        mark("time.gpu.result", t_solve);
        return flow;
      }

      // [CUT] true when v is on the source side of the min cut
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
      int* d_big_list = nullptr;
      int* d_bfs_hub_list = nullptr;
      int* d_bfs_big_list = nullptr;
      int* h_pinned = nullptr;   // [0..2] list sizes, [3] BFS flag

      size_t alloc_bytes = 0;
      size_t free_before = 0;
      long bfs_levels = 0;

      std::vector<cap_t> h_residual_capacity;
      std::vector<cap_t> h_excess;
      std::vector<char> h_source_side;

      // [ALLOC] cudaMalloc that also counts the bytes we asked for
      template <typename T>
      void dev_alloc(T** p, size_t count) {
        MAXFLOW_CUDA_CHECK(cudaMalloc(p, count * sizeof(T)));
        alloc_bytes += count * sizeof(T);
      }

      // [MEM] three numbers in MB, the bytes we asked for, what the device lost because of us, and the device total in use
      void record_memory() {
        size_t free_b = 0;
        size_t total_b = 0;
        MAXFLOW_CUDA_CHECK(cudaMemGetInfo(&free_b, &total_b));
        const double mb = 1024.0 * 1024.0;
        metrics::get().keep_max("mem.gpu_alloc_mb", alloc_bytes / mb);
        metrics::get().keep_max("mem.gpu_solver_mb", (free_before > free_b ? free_before - free_b : 0) / mb);
        metrics::get().keep_max("mem.gpu_device_used_mb", (total_b - free_b) / mb);
        metrics::get().keep_max("mem.gpu_device_total_mb", total_b / mb);
      }

      // [CUT] source side = reachable from the source and from every vertex still holding excess, same as the CPU solver
      void compute_source_side() {
        int V = net.num_nodes;
        h_source_side.assign(V, 0);
        std::vector<vertex_id_t> stack;
        stack.reserve(V);
        h_source_side[net.source] = 1;
        stack.push_back(net.source);
        for (vertex_id_t v = 0; v < V; v++) {
          if (v != net.source && v != net.sink && h_excess[v] > MAXFLOW_EPSILON && !h_source_side[v]) {
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

#endif // MAXFLOW_GPU_WORKLIST_H
