#ifndef MAXFLOW_TYPES_H
#define MAXFLOW_TYPES_H

// Common small type aliases used across project.
// Capacities/flows are kept as a TEMPLATE parameter (cap_t) on data structures and algorithms
// current = int, later with CCG = float/double

namespace maxflow {

  // project-wide capacity/flow type
  using cap_t = double;
  
  // vertex is just an index into the node arrays
  using vertex_id_t = int;

  // half-edge is just an index into the edge arrays
  using edge_id_t = int;

  // Convergence threshold for floating-point capacities
  // Filters out residual micro-excess from double rounding
  // When cap_t = int, this evaluates to int(1e-8) = 0 (no effect)
  #define MAXFLOW_EPSILON cap_t(1e-8)

} // namespace maxflow

#endif // MAXFLOW_TYPES_H