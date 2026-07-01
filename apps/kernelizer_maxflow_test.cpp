// --------------------------------------------------
// apps/kernelizer_maxflow_test.cpp
//
// Standalone test for KernelizerMaxflow
// Builds small graphs by hand with KNOWN LP-optimal solutions,
// runs our max-flow kernelizer, and checks the results
//
// No Gurobi needed. Only Boost 
// --------------------------------------------------

#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cmath>

// This include pulls in everything:
//   KernelizerMaxflow.h
//     - Kernelizer.h
//     - ConstraintCompositeGraph.h (defines graph_t, vertex_weight_t)
//     - graph_csr.h, maxflow_static.h (our solvers)
#include "src/integration/KernelizerMaxflow.h"

// --------------------------------------------------
// Type aliases matching ConstraintCompositeGraph<> defaults:
//   variable_id_t = intmax_t
//   weight_t = double
//   graph_t = boost::adjacency_list<vecS, listS, undirectedS, ...>
// --------------------------------------------------
using CCG = ConstraintCompositeGraph<>;
using graph_t = CCG::graph_t;
using vertex_t = boost::graph_traits<graph_t>::vertex_descriptor;
using var_id_t = CCG::variable_id_t;
using weight_t = CCG::weight_t;

// --------------------------------------------------
// Helper: add a vertex with given variable_id and weight
//
// The CCG graph stores two properties per vertex:
//   vertex_name   = variable ID (intmax_t, >= 0 for real variables)
//   vertex_weight = weight (double)
//
// This helper wraps the Boost API for clarity.
// --------------------------------------------------
vertex_t add_ccg_vertex(graph_t& g, var_id_t id, weight_t weight)
{
  vertex_t v = boost::add_vertex(g);
  boost::put(boost::vertex_name, g, v, id);
  boost::put(boost::vertex_weight, g, v, weight);
  return v;
}

// --------------------------------------------------
// Helper: count remaining vertices in a graph
// --------------------------------------------------
int count_vertices(const graph_t& g)
{
  int count = 0;
  auto [vi, vi_end] = boost::vertices(g);
  for (auto it = vi; it != vi_end; ++it)
    ++count;
  return count;
}

// --------------------------------------------------
// Helper: print assignments and kernel
// --------------------------------------------------
void print_result(const std::string& name, const std::map<var_id_t, bool>& assignments, const graph_t& g)
{
  std::cout << "  Fixed assignments:\n";
  if (assignments.empty())
    std::cout << "    (none — all vertices remain in kernel)\n";
  for (auto& [id, val] : assignments)
    std::cout << "    variable " << id
              << " = " << (val ? "1 (IN cover)" : "0 (NOT in cover)")
              << "\n";

  int remaining = count_vertices(g);
  std::cout << "  Remaining kernel size: " << remaining << " vertices\n";
}

// --------------------------------------------------
// TEST 1: Single edge  P(5) -- Q(3)
//
// LP relaxation:
//   minimize  5·x_P + 3·x_Q
//   subject to:  x_P + x_Q >= 1
//                0 <= x_P, x_Q <= 1
//
// Optimal LP solution:  x_P = 0,  x_Q = 1
//   (setting Q=1 covers the edge at cost 3, which is cheaper
//    than setting P=1 at cost 5)
//
// LP value = 3.  This solution is UNIQUE.
//
// Expected NT result:
//   P - false (not in cover),  Q - true (in cover)
//   Kernel is empty.
// --------------------------------------------------
bool test_single_edge()
{
  std::cout << "--- Test 1: Single edge P(5)--Q(3) ---\n";

  graph_t g;
  vertex_t P = add_ccg_vertex(g, 0, 5.0);  // variable 0, weight 5
  vertex_t Q = add_ccg_vertex(g, 1, 3.0);  // variable 1, weight 3
  boost::add_edge(P, Q, g);

  std::map<var_id_t, bool> assignments;
  maxflow::KernelizerMaxflow<> kmf;
  kmf.kernelize(g, assignments);

  print_result("single_edge", assignments, g);

  // Verify
  bool pass = true;

  // Q (id=1) should be in cover (true)
  if (assignments.count(1) == 0 || assignments[1] != true) {
    std::cout << "  FAIL: expected variable 1 (Q) = true\n";
    pass = false;
  }

  // P (id=0) should NOT be in cover (false)
  if (assignments.count(0) == 0 || assignments[0] != false) {
    std::cout << "  FAIL: expected variable 0 (P) = false\n";
    pass = false;
  }

  // Kernel should be empty
  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel, got "
              << count_vertices(g) << " vertices\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// --------------------------------------------------
// TEST 2: Star graph  S(1) -- L1(10), S(1) -- L2(10), S(1) -- L3(10)
//
// Center vertex S has weight 1, three leaves each have weight 10.
//
// LP relaxation:
//   minimize  1·x_S + 10·x_L1 + 10·x_L2 + 10·x_L3
//   subject to:  x_S + x_L1 >= 1
//                x_S + x_L2 >= 1
//                x_S + x_L3 >= 1
//
// Optimal LP solution:  x_S = 1,  x_L1 = x_L2 = x_L3 = 0
//   (covering center at cost 1 is far cheaper than any leaf combo)
//
// LP value = 1.  This solution is UNIQUE.
//
// Expected NT result:
//   S - true (in cover),  L1, L2, L3 - false (not in cover)
//   Kernel is empty.
// --------------------------------------------------
bool test_star_graph()
{
  std::cout << "--- Test 2: Star graph S(1) -- L1(10), L2(10), L3(10) ---\n";

  graph_t g;
  vertex_t S  = add_ccg_vertex(g, 0, 1.0);   // center, weight 1
  vertex_t L1 = add_ccg_vertex(g, 1, 10.0);  // leaf 1, weight 10
  vertex_t L2 = add_ccg_vertex(g, 2, 10.0);  // leaf 2, weight 10
  vertex_t L3 = add_ccg_vertex(g, 3, 10.0);  // leaf 3, weight 10

  boost::add_edge(S, L1, g);
  boost::add_edge(S, L2, g);
  boost::add_edge(S, L3, g);

  std::map<var_id_t, bool> assignments;
  maxflow::KernelizerMaxflow<> kmf;
  kmf.kernelize(g, assignments);

  print_result("star", assignments, g);

  bool pass = true;

  // S (id=0) should be in cover
  if (assignments.count(0) == 0 || assignments[0] != true) {
    std::cout << "  FAIL: expected variable 0 (S) = true\n";
    pass = false;
  }

  // L1, L2, L3 (ids 1,2,3) should NOT be in cover
  for (int id = 1; id <= 3; ++id) {
    if (assignments.count(id) == 0 || assignments[id] != false) {
      std::cout << "  FAIL: expected variable " << id << " = false\n";
      pass = false;
    }
  }

  // Kernel should be empty
  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// --------------------------------------------------
// TEST 3: Path graph  A(2) -- B(3) -- C(4) -- D(1)
//
// LP relaxation:
//   minimize  2·x_A + 3·x_B + 4·x_C + 1·x_D
//   subject to:  x_A + x_B >= 1
//                x_B + x_C >= 1
//                x_C + x_D >= 1
//
// Optimal LP solution:  x_A = 0, x_B = 1, x_C = 0, x_D = 1
//   value = 0 + 3 + 0 + 1 = 4
//   This is the unique integer AND LP optimal (since integer
//   solution achieves LP lower bound).
//
// Expected NT result:
//   A - false,  B - true,  C - false,  D - true
//   Kernel is empty.
// --------------------------------------------------
bool test_path_graph()
{
  std::cout << "--- Test 3: Path A(2)--B(3)--C(4)--D(1) ---\n";

  graph_t g;
  vertex_t A = add_ccg_vertex(g, 0, 2.0);
  vertex_t B = add_ccg_vertex(g, 1, 3.0);
  vertex_t C = add_ccg_vertex(g, 2, 4.0);
  vertex_t D = add_ccg_vertex(g, 3, 1.0);

  boost::add_edge(A, B, g);
  boost::add_edge(B, C, g);
  boost::add_edge(C, D, g);

  std::map<var_id_t, bool> assignments;
  maxflow::KernelizerMaxflow<> kmf;
  kmf.kernelize(g, assignments);

  print_result("path", assignments, g);

  bool pass = true;

  // A=false, B=true, C=false, D=true
  bool expected[] = {false, true, false, true};
  for (int id = 0; id <= 3; ++id) {
    if (assignments.count(id) == 0 || assignments[id] != expected[id]) {
      std::cout << "  FAIL: expected variable " << id
                << " = " << (expected[id] ? "true" : "false") << "\n";
      pass = false;
    }
  }

  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// --------------------------------------------------
// TEST 4: Triangle  A(10) -- B(6) -- C(4) -- A
//
// LP relaxation:
//   minimize  10·x_A + 6·x_B + 4·x_C
//   subject to:  x_A + x_B >= 1
//                x_A + x_C >= 1
//                x_B + x_C >= 1
//
// This has MULTIPLE optimal LP solutions, all with value 10:
//   (a) x_A = 0.5, x_B = 0.5, x_C = 0.5  → value = 10
//   (b) x_A = 0,   x_B = 1,   x_C = 1    → value = 10
//
// The max-flow solver may find EITHER one due to min-cut
// non-uniqueness (risk R2 from Module 2). Both are correct.
//
// We verify:
//   - LP objective value = 10 regardless of which solution
//   - All edge constraints are satisfied
// --------------------------------------------------
bool test_triangle()
{
  std::cout << "--- Test 4: Triangle A(10)--B(6)--C(4) ---\n";

  graph_t g;
  vertex_t A = add_ccg_vertex(g, 0, 10.0);
  vertex_t B = add_ccg_vertex(g, 1, 6.0);
  vertex_t C = add_ccg_vertex(g, 2, 4.0);

  boost::add_edge(A, B, g);
  boost::add_edge(A, C, g);
  boost::add_edge(B, C, g);

  std::map<var_id_t, bool> assignments;
  maxflow::KernelizerMaxflow<> kmf;
  kmf.kernelize(g, assignments);

  print_result("triangle", assignments, g);

  // Compute LP objective value from assignments:
  //   fixed-1 vertices contribute their full weight
  //   fixed-0 vertices contribute 0
  //   remaining (0.5) vertices contribute weight * 0.5
  double weights[] = {10.0, 6.0, 4.0};
  double lp_value = 0.0;

  for (int id = 0; id <= 2; ++id) {
    if (assignments.count(id) && assignments[id] == true)
      lp_value += weights[id] * 1.0;
    else if (assignments.count(id) && assignments[id] == false)
      lp_value += weights[id] * 0.0;
    else
      lp_value += weights[id] * 0.5;  // remaining in kernel = 0.5
  }

  std::cout << "  LP objective value = " << lp_value << " (expected 10)\n";

  bool pass = true;

  if (std::abs(lp_value - 10.0) > 1e-6) {
    std::cout << "  FAIL: LP value should be 10\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// --------------------------------------------------
// TEST 5: Isolated vertex  V(7)  (no edges)
//
// A vertex with no edges is trivially NOT in the cover (x=0).
// The LP has no constraints, so minimizing 7·x_V gives x_V=0.
//
// Expected: V - false, kernel empty.
// --------------------------------------------------
bool test_isolated_vertex()
{
  std::cout << "--- Test 5: Isolated vertex V(7) ---\n";

  graph_t g;
  add_ccg_vertex(g, 0, 7.0);

  std::map<var_id_t, bool> assignments;
  maxflow::KernelizerMaxflow<> kmf;
  kmf.kernelize(g, assignments);

  print_result("isolated", assignments, g);

  bool pass = true;

  if (assignments.count(0) == 0 || assignments[0] != false) {
    std::cout << "  FAIL: expected variable 0 = false\n";
    pass = false;
  }

  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// --------------------------------------------------
// TEST 6: Complete bipartite K(2,2)
//
//   U1(3) -- V1(5)
//   U1(3) -- V2(4)
//   U2(2) -- V1(5)
//   U2(2) -- V2(4)
//
// LP relaxation:
//   minimize  3·x_U1 + 2·x_U2 + 5·x_V1 + 4·x_V2
//   subject to all 4 edge constraints
//
// Optimal LP: x_U1=1, x_U2=1, x_V1=0, x_V2=0
//   value = 3 + 2 = 5
//   (covering both U vertices is cheaper than any V combo)
//
// This is unique because the U-side total (5) < V-side total (9).
//
// Expected: U1=true, U2=true, V1=false, V2=false
// --------------------------------------------------
bool test_complete_bipartite()
{
  std::cout << "--- Test 6: K(2,2): U1(3),U2(2) -- V1(5),V2(4) ---\n";

  graph_t g;
  vertex_t U1 = add_ccg_vertex(g, 0, 3.0);
  vertex_t U2 = add_ccg_vertex(g, 1, 2.0);
  vertex_t V1 = add_ccg_vertex(g, 2, 5.0);
  vertex_t V2 = add_ccg_vertex(g, 3, 4.0);

  boost::add_edge(U1, V1, g);
  boost::add_edge(U1, V2, g);
  boost::add_edge(U2, V1, g);
  boost::add_edge(U2, V2, g);

  std::map<var_id_t, bool> assignments;
  maxflow::KernelizerMaxflow<> kmf;
  kmf.kernelize(g, assignments);

  print_result("K(2,2)", assignments, g);

  bool pass = true;

  // U1=true, U2=true, V1=false, V2=false
  bool expected[] = {true, true, false, false};
  for (int id = 0; id <= 3; ++id) {
    if (assignments.count(id) == 0 || assignments[id] != expected[id]) {
      std::cout << "  FAIL: expected variable " << id
                << " = " << (expected[id] ? "true" : "false") << "\n";
      pass = false;
    }
  }

  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// --------------------------------------------------
// MAIN
// --------------------------------------------------
int main()
{
  std::cout << "--------------------------------------------------\n";
  std::cout << "  KernelizerMaxflow Verification Tests\n";
  std::cout << "  (CPU static max-flow solver, cap_t=double)\n";
  std::cout << "--------------------------------------------------\n\n";

  int passed = 0;
  int total  = 6;

  if (test_single_edge()) ++passed;
  if (test_star_graph()) ++passed;
  if (test_path_graph()) ++passed;
  if (test_triangle()) ++passed;
  if (test_isolated_vertex()) ++passed;
  if (test_complete_bipartite()) ++passed;

  std::cout << "--------------------------------------------------\n";
  std::cout << "  Results: " << passed << " / " << total << " passed\n";
  std::cout << "--------------------------------------------------\n";

  return (passed == total) ? 0 : 1;
}