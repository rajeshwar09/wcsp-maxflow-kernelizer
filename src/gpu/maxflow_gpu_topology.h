#ifndef MAXFLOW_GPU_TOPOLOGY_H
#define MAXFLOW_GPU_TOPOLOGY_H

#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <algorithm>

#include "src/cpu/graph_csr.h"

namespace maxflow {
  
  // CUDA error checking
  #define MAXFLOW_CUDA_CHECK(call) do {                             \
    cudaError_t err = (call);                                       \
    if (err != cudaSuccess) {                                       \
      std::cerr << "CUDA error at " << __FILE__ << ":" << __LINE__  \
                << " : " << cudaGetErrorString(err) << "\n";        \
      std::exit(EXIT_FAILURE);                                      \
    }                                                               \
  } while (0)

  //  ------------------------------------------------------------------------------------
  //  CUDA Kernels - Algorithms 1-4 (Topology Driven)
  //
  //  Each kernel corersponds to specific part of paper
  //  Topology-driven => every kernel launch convers ALL vertices (one thread per vertex)
  //  ------------------------------------------------------------------------------------

  //  Algorithm 1: Initialize
  //  Paper: cf(e) = c(e); e(v) = 0; h(s) = |V|; h(v) = 0
  //  Reference : staticMaxFlow_kernel_1
  __global__ void topo_initialize_kernel(int num_nodes, int num_edges, int source, const int* capacity, int* residual_capacity, int* excess, int* height) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;

    //  One thread per half-edge: reset residual to original capacity
    if (tid < num_edges) {
      residual_capacity[tid] = capacity[tid];
    }

    //  One thread per vertex: reset excess and set heights
    if (tid < num_nodes) {
      excess[tid] = 0;
      height[tid] = (tid == source) ? num_nodes : 0;
    }
  }

  //  Algorithm 1: Saturate source
  //  Paper: cf(s,v) = 0; cf(v,s) += c(s,v); e(v) += c(s,v)
  //  Reference: staticMaxFlow_kernal_7
  //  One thread per outgoing edge of the source
  __global__ void topo_saturate_source_kernel(int src_start, int src_end, int source, const int* edge_dst, const int* capacity, int* residual_capacity, const int* reverse_index, int* excess) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    int e = src_start + idx;
    if (e >= src_end) {
      return;
    }

    int cap = capacity[e];
    if (cap > 0) {
      residual_capacity[e] = 0;                             // cf(s,v) = 0
      atomicAdd(&residual_capacity[reverse_index[e]], cap); // cf(v,s) += c(s,v)
      atomicAdd(&excess[edge_dst[e]], cap);                 // e(v) += c(s,v)
      atomicAdd(&excess[source], -cap);                     // e(s) -= c(s,v)
    }
  }

  //  Algorithm 4: Initialize backward BFS
  //  Set all heights to |V| (unreachable), sink to 0
  //  Reference: staticMaxFlow_kernel_10
  __global__ void topo_bfs_init_kernel(int num_nodes, int sink, int* height) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid < num_nodes) {
      height[tid] = (tid == sink) ? 0 : num_nodes;
    }
  }

  //  Algorithm 4: One BFS layer
  //  For each u at current level, look at out-edges
  //  If v->u has residual & v is undiscovered
  //  set height[v] = level + 1
  //  Reference: staticMaxFlow_kernel_11
  __global__ void topo_bfs_step_kernel(int num_nodes, int level, const int* offset, const int* edge_dst, const int* residual_capacity, const int* reverse_index, int* height, int* flag) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes) {
      return;
    }
    if (height[u] != level) {
      return;
    }

    for (int e = offset[u]; e < offset[u + 1]; e++) {
      int v = edge_dst[e];
      //  v can step to u iff v->u has residual
      if (residual_capacity[reverse_index[e]] > 0 && height[v] == num_nodes) {
        height[v] = level + 1;
        *flag = 1;  // => host must launch another layer
      }
    }
  }

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
        int d = std::min(excess[u], residual_capacity[e_hat]);
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

  //  Algorithm 3: Remove invalid steep edges
  //  If height[v] > height[u] + 1 for residual u->v => cancel it
  //  To remove parallel push races in GPU
  //  Reference: staticMaxFlow_kernel_17
  __global__ void topo_remove_invalid_edges_kernel(int num_nodes, int source, int sink, const int* offset, const int* edge_dst, int* residual_capacity, const int* reverse_index, int* excess, const int* height) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }

    for (int e = offset[u]; e < offset[u + 1]; e++) {
      int v = edge_dst[e];
      int rc = residual_capacity[e];
      if (rc > 0 && height[u] > height[v] + 1) {
        residual_capacity[e] = 0;
        atomicAdd(&residual_capacity[reverse_index[e]], rc);
        atomicAdd(&excess[u], -rc);
        atomicAdd(&excess[v], rc);
      }
    }
  }

  //  Convergence check: is any vertex still active
  __global__ void topo_check_active_kernel(int num_nodes, int source, int sink, const int* excess, const int* height, int* flag) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }
    if (excess[u] > 0 && height[u] < num_nodes) {
      *flag = 1;
    }
  }

  //  Host Solver Class
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
        topo_initialize_kernel<<<blocks_max, threads>>>(V, E, net.source, d_capacity, d_residual_capacity, d_excess, d_height);
        MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

        //  Algorithm 1: saturate source
        int src_start = net.offset[net.source];
        int src_end = net.offset[net.source + 1];
        int src_count = src_end - src_start;
        if (src_count > 0) {
          int blocks_s = (src_count + threads - 1) / threads;
          topo_saturate_source_kernel<<<blocks_s, threads>>>(src_start, src_end, net.source, d_edge_dst, d_capacity, d_residual_capacity, d_reverse_index, d_excess);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
        } 

        //  Algorithm 1: main loop
        int h_flag;
        while (true) {
          //  Check for active vertices
          h_flag = 0;
          MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &h_flag, sizeof(int), cudaMemcpyHostToDevice));
          topo_check_active_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_excess, d_height, d_flag);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());
          MAXFLOW_CUDA_CHECK(cudaMemcpy(&h_flag, d_flag, sizeof(int), cudaMemcpyDeviceToHost));
          if (!h_flag) {
            break;  // no active vertex => done
          }

          //  Algorithm 4: global relabel (backwards BFS)
          topo_bfs_init_kernel<<<blocks_v, threads>>>(V, net.sink, d_height);
          MAXFLOW_CUDA_CHECK(cudaDeviceSynchronize());

          for (int level = 0; level < V; level++) {
            h_flag = 0;
            MAXFLOW_CUDA_CHECK(cudaMemcpy(d_flag, &h_flag, sizeof(int), cudaMemcpyHostToDevice));
            topo_bfs_step_kernel<<<blocks_v, threads>>>(V, level, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_height, d_flag);
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
          topo_remove_invalid_edges_kernel<<<blocks_v, threads>>>(V, net.source, net.sink, d_offset, d_edge_dst, d_residual_capacity, d_reverse_index, d_excess, d_height);
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