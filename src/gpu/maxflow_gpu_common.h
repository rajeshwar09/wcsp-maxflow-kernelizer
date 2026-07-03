#ifndef MAXFLOW_GPU_COMMON_H
#define MAXFLOW_GPU_COMMON_H

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

  //  Shared CUDA Kernels — used by BOTH topology-driven and data-driven worklist solvers
  //  These are parts of Algorithm 1 that are same as of how push-relabel is scheduled (init, saturate, BFS, remove-invalid, active check)
  //
  //  Only the push-relabel kernel itself (Algorithm 2) differs between topology-driven and worklist

  //  Algorithm 1: Initialize
  //  Paper: cf(e) = c(e); e(v) = 0; h(s) = |V|; h(v) = 0
  //  Reference : staticMaxFlow_kernel_1
  __global__ void gpu_initialize_kernel(int num_nodes, int num_edges, int source, const cap_t* capacity, cap_t* residual_capacity, cap_t* excess, int* height) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    //  One thread per half-edge: reset residual to original capacity
    if (tid < num_edges) {
      residual_capacity[tid] = capacity[tid];
    }

    //  One thread per vertex: reset excess and set heights
    if (tid < num_nodes) {
      excess[tid] = cap_t(0);
      height[tid] = (tid == source) ? num_nodes : 0;
    }
  }

  //  Algorithm 1: Saturate source
  //  Paper: cf(s,v) = 0; cf(v,s) += c(s,v); e(v) += c(s,v)
  //  Reference: staticMaxFlow_kernal_7
  //  One thread per outgoing edge of the source
  __global__ void gpu_saturate_source_kernel(int src_start, int src_end, int source, const int* edge_dst, const cap_t* capacity, cap_t* residual_capacity, const int* reverse_index, cap_t* excess) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    int e = src_start + idx;
    if (e >= src_end) {
      return;
    }

    cap_t cap = capacity[e];
    if (cap > cap_t(0)) {
      residual_capacity[e] = cap_t(0);                      // cf(s,v) = 0
      atomicAdd(&residual_capacity[reverse_index[e]], cap); // cf(v,s) += c(s,v)
      atomicAdd(&excess[edge_dst[e]], cap);                 // e(v) += c(s,v)
      atomicAdd(&excess[source], -cap);                     // e(s) -= c(s,v)
    }
  }

  //  Algorithm 4: Initialize backward BFS
  //  Set all heights to |V| (unreachable), sink to 0
  //  Reference: staticMaxFlow_kernel_10
  __global__ void gpu_bfs_init_kernel(int num_nodes, int sink, int* height) {
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
  __global__ void gpu_bfs_step_kernel(int num_nodes, int level, const int* offset, const int* edge_dst, const cap_t* residual_capacity, const int* reverse_index, int* height, int* flag) {
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
      if (residual_capacity[reverse_index[e]] > cap_t(0) && height[v] == num_nodes) {
        height[v] = level + 1;
        *flag = 1;  // => host must launch another layer
      }
    }
  }

  //  Algorithm 3: Remove invalid steep edges
  //  If height[v] > height[u] + 1 for residual u->v => cancel it
  //  To remove parallel push races in GPU
  //  Reference: staticMaxFlow_kernel_17
  __global__ void gpu_remove_invalid_edges_kernel(int num_nodes, int source, int sink, const int* offset, const int* edge_dst, cap_t* residual_capacity, const int* reverse_index, cap_t* excess, const int* height) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }

    for (int e = offset[u]; e < offset[u + 1]; e++) {
      int v = edge_dst[e];
      cap_t rc = residual_capacity[e];
      if (rc > cap_t(0) && height[u] > height[v] + 1) {
        residual_capacity[e] = cap_t(0);
        atomicAdd(&residual_capacity[reverse_index[e]], rc);
        atomicAdd(&excess[u], -rc);
        atomicAdd(&excess[v], rc);
      }
    }
  }

  //  Convergence check: is any vertex still active
  __global__ void gpu_check_active_kernel(int num_nodes, int source, int sink, const cap_t* excess, const int* height, int* flag) {
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    if (u >= num_nodes || u == source || u == sink) {
      return;
    }
    if (excess[u] > cap_t(0) && height[u] < num_nodes) {
      *flag = 1;
    }
  }
} // namespace maxflow

#endif // MAXFLOW_GPU_COMMON_H