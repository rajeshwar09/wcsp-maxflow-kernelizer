#ifndef MAXFLOW_GPU_WORKLIST_H
#define MAXFLOW_GPU_WORKLIST_H

#include "src/gpu/maxflow_gpu_common.h"

namespace maxflow {

  //  ------------------------------------------------------------------------------------
  //  Data-driven Worklist approach
  //
  // It adds two new kernels for the data-driven worklist:
  //   worklist_build_kernel - scan vertices, collect active into worklist
  //   worklist_push_relabel_kernel - process only worklist vertices
  //  Shared kernels: (init, saturate, BFS, remove-invalid, check-active)
  //  taken from "maxflow_gpu_common.h"
  //  ------------------------------------------------------------------------------------

  
  // Reset all per-vertex flags to 0 before each worklist round
  __global__ void worklist_reset_flags_kernel(int num_nodes, int* in_worklist) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid < num_nodes) {
      in_worklist[tid] = 0;
    }
  }

  //  Build initial worklist: scan all vertices, colelct active ones
  //  Active = not s/t, excess > 0, height < |V|
  __global__ void worklist_build_kernel(int num_nodes, int source, int sink, const int* excess, const int* height, int* worklist, int* wl_count) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }

    if (excess[u] > 0 && height[u] < num_nodes) {
      int pos = atomicAdd(wl_count, 1);
      worklist[pos] = u;
    }
  }

  //  Algorithm 2: Push-relabel on worklist vertices only
  //  Each thread processes one vertex from worklist_in
  //  Push activates a neighbour or relabel/residual-excess keep vertex active,
  //  vertex is appended to worklist_out and host can schedule more round without full global relabel
  __global__ void worklist_push_relabel_kernel(
    int wl_size, int num_nodes, int source, int sink, int kernel_cycles, const int* worklist_in, const int* offset, const int* edge_dst, 
    int* residual_capacity, const int* reverse_index, int* excess, int* height, int* worklist_out, int* wl_out_count, int* in_worklist) {
      
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= wl_size) {
      return;
    }
    int u = worklist_in[idx];

    for (int cnt = 0; cnt < kernel_cycles; cnt++) {
      if (!(height[u] < num_nodes && excess[u] > 0)) {
        break;
      }

      //  Find lowest-height neighbour reachable via residual edge
      int lowest_h = num_nodes + 1;
      int v_hat = -1;
      int e_hat = -1;

      for (int e = offset[u]; e < offset[u + 1]; e++) {
        if (residual_capacity[e] > 0 && height[edge_dst[e]] < lowest_h) {
          lowest_h = height[edge_dst[e]];
          v_hat = edge_dst[e];
          e_hat = e;
        }
      }

      if (v_hat == -1) {
        break;
      }

      if (height[u] > lowest_h) {
        //  Push
        int a = excess[u];
        int b = residual_capacity[e_hat];
        int d = (a < b) ? a : b;
        atomicAdd(&residual_capacity[e_hat], -d);
        atomicAdd(&residual_capacity[reverse_index[e_hat]], d);
        atomicAdd(&excess[u], -d);
        atomicAdd(&excess[v_hat], d);

        //  v_hat may now be active => add it to output worklist
        if (v_hat != source && v_hat != sink) {
          if (atomicCAS(&in_worklist[v_hat], 0, 1) == 0) {
            int pos = atomicAdd(wl_out_count, 1);
            worklist_out[pos] = v_hat;
          }
        }
      } else {
        //  Relabel
        height[u] = lowest_h + 1;
      }
    }

    //  If u still active after kernel_cycles then add it back to output worklist
    if (excess[u] > 0 && height[u] < num_nodes) {
      if (atomicCAS(&in_worklist[u], 0, 1) == 0) {
        int pos = atomicAdd(wl_out_count, 1);
        worklist_out[pos] = u;
      }
    }
  }

  //  Host Solver Class - Data Worklist
  //
  //  Same outer loop as topology. push-relabel phase turn an inner worlist loop
  //  Only active vertices feed the next round (no full global relabel)

  class gpu_worklist_solver {
    public:
      explicit gpu_worklist_solver(flow_network<int>& net) : net(net) {
        int V = net.num_nodes;
        int E = net.num_edges;

        //  Graph arrays
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_offset,            (V + 1) * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_edge_dst,          E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_capacity,          E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_residual_capacity, E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_reverse_index,     E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_excess,            V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_height,            V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_flag,              1       * sizeof(int)));

        //  Worklist arrays
        int wl_max = V + E;
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_worklist_in,  wl_max * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_worklist_out, wl_max * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_wl_in_count, sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_wl_out_count, sizeof(int)));

        // Per-vertex dedup flag array (prevents duplicates in worklist)
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_in_worklist, V * sizeof(int)));

        //  Copy graph structure to device
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_offset, net.offset.data(), (V + 1) * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_edge_dst, net.edge_dst.data(), E * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_capacity, net.capacity.data(), E * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_reverse_index, net.reverse_index.data(), E * sizeof(int), cudaMemcpyHostToDevice));
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
        cudaFree(d_worklist_in);
        cudaFree(d_worklist_out);
        cudaFree(d_wl_in_count);
        cudaFree(d_wl_out_count);
        cudaFree(d_in_worklist);
      }

      gpu_worklist_solver(const gpu_worklist_solver&) = delete;
      gpu_worklist_solver& operator=(const gpu_worklist_solver&) = delete;

      int solve(int kernel_cycles = 0) {
        int V = net.num_nodes;
        int E = net.num_edges;
        if (kernel_cycles <= 0) {
          kernel_cycles = std::max(1, E / std::max(1, V));
        }

        int threads = 256;
        int blocks_v = (V + threads - 1) / threads;
        int blocks_max = (std::max(V, E) + threads - 1) / threads;
        int h_val;

        //  Algorithm 1 : initialize
        gpu_initialize_kernel<<<blocks_v, threads>>>(V, E, net.source, d_capacity, d_residual_capacity, d_excess, d_height);
        MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

        //  Algorithm 1 : saturate source
        int src_start = net.offset[net.source];
        int src_end = net.offset[net.source + 1];
        int src_count = src_end - src_start;
        if (src_count > 0) {
          int blocks_s = (src_count + threads - 1) / threads;
          gpu_saturate_source_kernel<<<blocks_s, threads>>>(src_start, src_end, net.source, d_edge_dst, d_capacity, d_residual_capacity, d_reverse_index, d_excess);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
        }

        //  Main loop
        while(true) {
          //  Check for active vertices
          h_val = 0;
          MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &h_val, sizeof(int), cudaMemcpyHostToDevice));

          gpu_check_active_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_excess, d_height, d_flag);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
          
          MAXFLOW_CUDA_CHECK(cudaMemcpy(&h_val, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
          
          if (!h_val) {
            break;
          }

          //  Algorithm 4 : global relabel
          gpu_bfs_init_kernel<<<blocks_v, threads>>>(V, net.sink, d_height);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

          for (int level = 0; level < V; level++) {
            h_val = 0;
            MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &h_val, sizeof(int), cudaMemcpyHostToDevice));
            
            gpu_bfs_step_kernel<<<blocks_v, threads>>>(V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
            MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
            
            MAXFLOW_CUDA_CHECK(cudaMemcpy(&h_val, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
            
            if (!h_val) {
              break;
            }
          }

          //  Build worklist of current active vertices
          h_val = 0;
          MAXFLOW_CUDA_CHECK(cudaMemcpy(d_wl_in_count, &h_val, sizeof(int), cudaMemcpyHostToDevice));

          worklist_build_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_excess, d_height, d_worklist_in, d_wl_in_count);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

          int wl_size;
          MAXFLOW_CUDA_CHECK(cudaMemcpy(&wl_size, d_wl_in_count, sizeof(int), cudaMemcpyDeviceToHost));

          //  Inner worklist loop
          //  Multiple push-relabel without redoing global relabel
          while (wl_size > 0) {
            // Reset output count and dup flags
            worklist_reset_flags_kernel<<<blocks_v, threads>>>(V, d_in_worklist);
            h_val = 0;
            MAXFLOW_CUDA_CHECK(cudaMemcpy(d_wl_out_count, &h_val, sizeof(int), cudaMemcpyHostToDevice));

            int blocks_wl = (wl_size + threads - 1) / threads;
            worklist_push_relabel_kernel<<<blocks_wl, threads>>>(wl_size, V, net.source, net.sink, kernel_cycles, d_worklist_in, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height, d_worklist_out, d_wl_out_count, d_in_worklist);
            MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

            // Read output worklist size
            MAXFLOW_CUDA_CHECK(cudaMemcpy(&wl_size, d_wl_out_count, sizeof(int), cudaMemcpyDeviceToHost));

            // Swap in/out for next round
            std::swap(d_worklist_in, d_worklist_out);
          }

          //  Algorithm 3 : remove invalid edges
          gpu_remove_invalid_edges_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
        }

        //  Read back results
        int flow;
        MAXFLOW_CUDA_CHECK(cudaMemcpy(&flow, d_excess + net.sink, sizeof(int), cudaMemcpyDeviceToHost));

        h_height.resize(V);
        MAXFLOW_CUDA_CHECK(cudaMemcpy(h_height.data(), d_height, V * sizeof(int), cudaMemcpyDeviceToHost));

        return flow;
      }

      bool is_on_source_side(vertex_id_t v) const {
        return h_height[v] >= net.num_nodes;
      }

    private:
      flow_network<int>& net;

      // Device graph arrays
      int* d_offset = nullptr;
      int* d_edge_dst = nullptr;
      int* d_capacity = nullptr;
      int* d_residual_capacity = nullptr;
      int* d_reverse_index = nullptr;
      int* d_excess = nullptr;
      int* d_height = nullptr;
      int* d_flag = nullptr;

      //  Device worklist arrays
      int* d_worklist_in = nullptr;  // vertices to process this round
      int* d_worklist_out = nullptr; // newly active vertices for next round
      int* d_wl_in_count = nullptr;  // number of entries in worklist_in
      int* d_wl_out_count = nullptr; // atomic counter for worklist_out

      // Per-vertex dedup flag (prevents duplicate worklist entries)
      int* d_in_worklist  = nullptr;

      //  Host copy of heights
      std::vector<int> h_height;
  };

} // namespace maxflow


#endif // MAXFLOW_GPU_WORKLIST_H