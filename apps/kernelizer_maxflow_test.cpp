// gate test for KernelizerMaxflow: 6 small graphs made by hand whose LP optimum is known,
// the max-flow kernelizer runs on each and its assignments are checked, no Gurobi needed
// build: g++ -std=c++17 -O2 -I. apps/kernelizer_maxflow_test.cpp -o kmf_test -lopenblas
// usage: ./kmf_test
// exit code: 0 when all 6 pass, 1 otherwise

#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <cmath>

// brings in Kernelizer.h, ConstraintCompositeGraph.h (graph_t, vertex weights) and our CPU solver
#include "src/integration/KernelizerMaxflow.h"

// [TYPES] same as the ConstraintCompositeGraph<> defaults: variable id intmax_t, weight double, boost adjacency_list graph
using CCG = ConstraintCompositeGraph<>;
using graph_t = CCG::graph_t;
using vertex_t = boost::graph_traits<graph_t>::vertex_descriptor;
using var_id_t = CCG::variable_id_t;
using weight_t = CCG::weight_t;

// [HELPER] adds a vertex with a variable id (vertex_name, >= 0 for real variables) and a weight (vertex_weight)
vertex_t add_ccg_vertex(graph_t& g, var_id_t id, weight_t weight) {
  vertex_t v = boost::add_vertex(g);
  boost::put(boost::vertex_name, g, v, id);
  boost::put(boost::vertex_weight, g, v, weight);
  return v;
}

// [HELPER] number of vertices left in the graph
int count_vertices(const graph_t& g) {
  int count = 0;
  auto [vi, vi_end] = boost::vertices(g);
  for (auto it = vi; it != vi_end; ++it)
    ++count;
  return count;
}

// [HELPER] prints the fixed assignments and the size of the kernel left
void print_result(const std::string& name, const std::map<var_id_t, bool>& assignments, const graph_t& g) {
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

// [TEST1] single edge P(5) - Q(3)
// LP: minimize 5 x_P + 3 x_Q with x_P + x_Q >= 1, the only optimum is x_P = 0, x_Q = 1 (value 3)
// expected: P = 0, Q = 1, empty kernel
bool test_single_edge() {
  std::cout << "--- Test 1: Single edge P(5)--Q(3) ---\n";

  graph_t g;
  vertex_t P = add_ccg_vertex(g, 0, 5.0);  // variable 0, weight 5
  vertex_t Q = add_ccg_vertex(g, 1, 3.0);  // variable 1, weight 3
  boost::add_edge(P, Q, g);

  std::map<var_id_t, bool> assignments;
  maxflow::KernelizerMaxflow<> kmf;
  kmf.kernelize(g, assignments);

  print_result("single_edge", assignments, g);

  bool pass = true;

  // [CHECK] Q (id 1) in the cover
  if (assignments.count(1) == 0 || assignments[1] != true) {
    std::cout << "  FAIL: expected variable 1 (Q) = true\n";
    pass = false;
  }

  // [CHECK] P (id 0) not in the cover
  if (assignments.count(0) == 0 || assignments[0] != false) {
    std::cout << "  FAIL: expected variable 0 (P) = false\n";
    pass = false;
  }

  // [CHECK] empty kernel
  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel, got "
              << count_vertices(g) << " vertices\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// [TEST2] star: centre S(1) joined to leaves L1(10), L2(10), L3(10)
// LP: the only optimum takes the centre, x_S = 1 and every leaf 0 (value 1)
// expected: S = 1, L1 = L2 = L3 = 0, empty kernel
bool test_star_graph() {
  std::cout << "--- Test 2: Star graph S(1) -- L1(10), L2(10), L3(10) ---\n";

  graph_t g;
  vertex_t S  = add_ccg_vertex(g, 0, 1.0);   // centre, weight 1
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

  // [CHECK] S (id 0) in the cover
  if (assignments.count(0) == 0 || assignments[0] != true) {
    std::cout << "  FAIL: expected variable 0 (S) = true\n";
    pass = false;
  }

  // [CHECK] leaves (ids 1, 2, 3) not in the cover
  for (int id = 1; id <= 3; ++id) {
    if (assignments.count(id) == 0 || assignments[id] != false) {
      std::cout << "  FAIL: expected variable " << id << " = false\n";
      pass = false;
    }
  }

  // [CHECK] empty kernel
  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// [TEST3] path A(2) - B(3) - C(4) - D(1)
// LP: the only optimum is A = 0, B = 1, C = 0, D = 1 (value 4), it is integer so it is also the integer optimum
// expected: A = 0, B = 1, C = 0, D = 1, empty kernel
bool test_path_graph() {
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

  // [CHECK] A = 0, B = 1, C = 0, D = 1
  bool expected[] = {false, true, false, true};
  for (int id = 0; id <= 3; ++id) {
    if (assignments.count(id) == 0 || assignments[id] != expected[id]) {
      std::cout << "  FAIL: expected variable " << id
                << " = " << (expected[id] ? "true" : "false") << "\n";
      pass = false;
    }
  }

  // [CHECK] empty kernel
  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// [TEST4] triangle A(10) - B(6) - C(4) - A
// LP: more than one optimum with value 10, all at 0.5 or A = 0, B = 1, C = 1
// the min cut is not unique so the solver may give either one, both are right
// expected: LP value 10 for whatever assignment comes out
bool test_triangle() {
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

  // [CHECK] LP value: a vertex fixed to 1 adds its weight, fixed to 0 adds nothing, left in the kernel (0.5) adds half
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

// [TEST5] isolated vertex V(7) with no edges
// LP: no constraint, so minimizing 7 x_V gives x_V = 0
// expected: V = 0, empty kernel
bool test_isolated_vertex() {
  std::cout << "--- Test 5: Isolated vertex V(7) ---\n";

  graph_t g;
  add_ccg_vertex(g, 0, 7.0);

  std::map<var_id_t, bool> assignments;
  maxflow::KernelizerMaxflow<> kmf;
  kmf.kernelize(g, assignments);

  print_result("isolated", assignments, g);

  bool pass = true;

  // [CHECK] V (id 0) not in the cover
  if (assignments.count(0) == 0 || assignments[0] != false) {
    std::cout << "  FAIL: expected variable 0 = false\n";
    pass = false;
  }

  // [CHECK] empty kernel
  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// [TEST6] complete bipartite K(2,2) with U1(3), U2(2) on one side and V1(5), V2(4) on the other
// LP: the only optimum takes both U vertices (value 5), the U side total 5 is less than the V side total 9
// expected: U1 = 1, U2 = 1, V1 = 0, V2 = 0, empty kernel
bool test_complete_bipartite() {
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

  // [CHECK] U1 = 1, U2 = 1, V1 = 0, V2 = 0
  bool expected[] = {true, true, false, false};
  for (int id = 0; id <= 3; ++id) {
    if (assignments.count(id) == 0 || assignments[id] != expected[id]) {
      std::cout << "  FAIL: expected variable " << id
                << " = " << (expected[id] ? "true" : "false") << "\n";
      pass = false;
    }
  }

  // [CHECK] empty kernel
  if (count_vertices(g) != 0) {
    std::cout << "  FAIL: expected empty kernel\n";
    pass = false;
  }

  std::cout << "  " << (pass ? "PASS" : "FAIL") << "\n\n";
  return pass;
}

// [MAIN] runs the 6 tests and prints the count that passed
int main() {
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