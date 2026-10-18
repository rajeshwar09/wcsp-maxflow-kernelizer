// basic types used in the whole project
// usage: #include "src/common/types.h"

#ifndef MAXFLOW_TYPES_H
#define MAXFLOW_TYPES_H

namespace maxflow {

  // [TYPES] capacity and flow values, double because perturbed weights can be large
  using cap_t = double;

  // [TYPES] vertex and half-edge are plain indices into the arrays
  using vertex_id_t = int;
  using edge_id_t = int;

  // [TYPES] values below this are treated as zero, it hides double rounding noise
  #define MAXFLOW_EPSILON cap_t(1e-8)

} // namespace maxflow

#endif // MAXFLOW_TYPES_H
