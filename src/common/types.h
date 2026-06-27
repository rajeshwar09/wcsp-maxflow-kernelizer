#ifndef WMK_TYPES_H
#define WMK_TYPES_H

// Common small type aliases used across project.
// Capacities/flows are kept as a TEMPLATE parameter (cap_t) on data structures and algorithms
// current = int, later with CCG = float/double

namespace wmk { // wcsp-maxflow-kernelizer
  
  // vertex is just an index into the node arrays
  using vertex_id_t = int;

  // half-edge is just an index into the edge arrays
  using edge_id_t = int;

} // namespace wmk

#endif // WMK_TYPES_H