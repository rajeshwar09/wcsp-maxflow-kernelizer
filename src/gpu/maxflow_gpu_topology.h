#ifndef MAXFLOW_GPU_TOPOLOGY_H
#define MAXFLOW_GPU_TOPOLOGY_H

#include "src/gpu/maxflow_gpu_common.h"

namespace maxflow {
  
  //  ------------------------------------------------------------------------------------
  //  Topology-driven Push-Relabel approach
  //
  //  File contains topology-specific kernel and solver
  //  Shared kernels: (init, saturate, BFS, remove-invalid, check-active)
  //  taken from "maxflow_gpu_common.h"
  //  ------------------------------------------------------------------------------------

  //  Algorithm 2: Push-relabel sweep
  //  Each thread does upto kernel_cycles times
  //  Reference: staticMaxFlow_kernel_14
  __global__ void topo_push_relabel_kernel(int num_nodes, int source, int sink, int kernel_cycles, const int* offset, const int* edge_dst, int* residual_capacity, const int* reverse_index, int* excess, int* height) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }

    for (int cnt = 0; cnt < kernel_cycles; cnt++) {
      if (!(height[u] < num_nodes && excess[u] > 0)) {
        break;  // u not active
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
        //  Push: send min(excess[u], residual[e_hat]) along e_hat
        // int d = min(excess[u], residual_capacity[e_hat]);
        int a = excess[u];
        int b = residual_capacity[e_hat];
        int d = (a < b) ? a : b;
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
  //  Takes a host-side flow_network<int>, copies it to GPU and run algorith 1
  //  Copies result back to host
  class gpu_topology_solver {
    public:
      explicit gpu_topology_solver(flow_network<int>& net): net(net) {
        int V = net.num_nodes;
        int E = net.num_edges;

        //  Allocate device arrays
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_offset,            (V + 1) * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_edge_dst,          E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_capacity,          E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_residual_capacity, E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_reverse_index,     E       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_excess,            V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_height,            V       * sizeof(int)));
        MAXFLOW_CUDA_CHECK(cudaMalloc(&d_flag,              1       * sizeof(int)));

        //  Copy graph struct to device
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_offset, net.offset.data(), (V + 1) * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_edge_dst, net.edge_dst.data(), E * sizeof(int), cudaMemcpyHostToDevice));
        MAXFLOW_CUDA_CHECK(cudaMemcpy(d_capacity, net.capacity.data(), E * sizeof(int), cudaMemcpyHostToDevice));
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
      }

      //  Disallow copy (raw pointers)
      gpu_topology_solver(const gpu_topology_solver&) = delete;
      gpu_topology_solver* operator = (const gpu_topology_solver&) = delete;

      //  Run the full static max-flow (Algorithm 1) on GPU
      int solve(int kernel_cycles = 0) {
        int V = net.num_nodes;
        int E = net.num_edges;
        if (kernel_cycles <= 0) {
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

        //  Algorithm 1: main loop
        int h_flag;
        while (true) {
          //  Check for active vertices
          h_flag = 0;
          MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &h_flag, sizeof(int), cudaMemcpyHostToDevice));
          gpu_check_active_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_excess, d_height, d_flag);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
          MAXFLOW_CUDA_CHECK(cudaMemcpy(&h_flag, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
          if (!h_flag) {
            break;  // no active vertex => done
          }

          //  Algorithm 4: global relabel (backwards BFS)
          gpu_bfs_init_kernel<<<blocks_v, threads>>>(V, net.sink, d_height);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

          for (int level = 0; level < V; level++) {
            h_flag = 0;
            MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &h_flag, sizeof(int), cudaMemcpyHostToDevice));
            gpu_bfs_step_kernel<<<blocks_v, threads>>>(V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
            MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
            MAXFLOW_CUDA_CHECK(cudaMemcpy(&h_flag, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
            if (!h_flag) {
              break;  //  BFS finished => no more layers
            }
          }

          //  Algorithm 2: push-relabel sweep
          topo_push_relabel_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, kernel_cycles, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

          //  Algorithm 3: remove invalid edges
          gpu_remove_invalid_edges_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
        }

        //  Read back max-flow = excess[sink]
        int flow;
        MAXFLOW_CUDA_CHECK(cudaMemcpy(&flow, d_excess + net.sink, sizeof(int), cudaMemcpyDeviceToHost));

        //  Read back heights for min-cut
        h_height.resize(V);
        MAXFLOW_CUDA_CHECK(cudaMemcpy(h_height.data(), d_height, V * sizeof(int), cudaMemcpyDeviceToHost));

        return flow;
      }

      //  After solve(): true if v is on the source side of min-cut
      bool is_on_source_side(vertex_id_t v) const {
        return h_height[v] >= net.num_nodes;
      }
    
    private:
      flow_network<int>& net;

      // Device pointers
      int* d_offset = nullptr;
      int* d_edge_dst = nullptr;
      int* d_capacity = nullptr;
      int* d_residual_capacity = nullptr;
      int* d_reverse_index = nullptr;
      int* d_excess = nullptr;
      int* d_height = nullptr;
      int* d_flag = nullptr;

      //  Host copy of heights
      std::vector<int> h_height;
  };

} // namespace maxflow

#endif // MAXFLOW_GPU_TOPOLOGY_H