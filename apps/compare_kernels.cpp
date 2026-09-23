// Runs kernelizers on the same CCG built from one .wcsp file and compares the resulting assignments variable by variable
//
// Usage:
//   ./compare_kernels <file.wcsp>                 max-flow and Gurobi, one process
//   ./compare_kernels mf    <file.wcsp> <out>     max-flow only, dump assignments
//   ./compare_kernels lp    <file.wcsp> <out>     Gurobi LP only, dump assignments
//   ./compare_kernels cplex <file.wcsp> <out>     CPLEX LP only, dump assignments (needs a -DHAVE_CPLEX build)
//   ./compare_kernels cmp   <a> <b>               compare any two dumps
//
// For anything at 300k or above, prefer the split form:
//   ./compare_kernels mf    data/bench/10_1M.wcsp /tmp/mf.txt
//   ./compare_kernels cplex data/bench/10_1M.wcsp /tmp/cplex.txt
//   ./compare_kernels cmp   /tmp/mf.txt /tmp/cplex.txt
//
// In lp and cplex mode the LP solution itself is also certified (printed to stderr as [lp-check] lines):
//   solver objective, the objective recomputed from the returned values, how many values are 0 / 0.5 / 1 / anything else,
//   and the worst violation of any x_u + x_v >= 1 constraint. A feasible, half-integral solution whose objective equals the
//   other solver's objective is an optimal LP solution, and by Nemhauser-Trotter persistency its 0/1 part is a safe kernel.
//
// Exit code: 0 = done (for cmp: no contradictions), 3 = cmp found OPPOSITE values, 1/2 = usage or file errors

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "third_party/wcsp-solver/src/LinearProgramSolver.h"
#include "third_party/wcsp-solver/src/KernelizerLinearProgramming.h"
#include "third_party/wcsp-solver/src/LinearProgramSolverGurobi.h"
#ifdef HAVE_CPLEX
#include "third_party/wcsp-solver/src/LinearProgramSolverCplex.h"
#endif
#include "src/integration/KernelizerMaxflow.h"

typedef ConstraintCompositeGraph<>::variable_id_t vid_t;
typedef ConstraintCompositeGraph<>::graph_t graph_t;

//  Build the CCG. The copy out of ConstraintCompositeGraph is unavoidable --
//  getGraph() returns a const pointer and kernelize() needs a mutable graph --
//  so this transiently holds two CCGs. Keep it inside as tight a scope as
//  possible at the call site.
static void build(const char* path, graph_t& g, std::map<vid_t, bool>& pre) {
  std::ifstream in(path);
  if (!in) { std::cerr << "cannot open " << path << "\n"; std::exit(2); }
  WCSPInstance<> inst(in, WCSPInstance<>::Format::DIMACS);
  ConstraintCompositeGraph<> ccg;
  WCSPInstance<>::constraint_t::Polynomial p;
  for (const auto& c : inst.getConstraints()) c.toPolynomial(p);
  ccg.addPolynomial(p);
  ccg.simplify(pre);
  g = *ccg.getGraph();
}

//  Wraps any LP solver, forwards every call unchanged, and keeps its own copy of the objective coefficients and of the
//  x_u + x_v >= 1 constraints, so that after solve() it can certify the returned solution independently of the solver.
class CheckedSolver : public LinearProgramSolver
{
private:
  std::unique_ptr<LinearProgramSolver> inner;
  std::string name;
  std::vector<double> cost;                     // indexed by the solver's variable id
  std::vector<std::pair<int, int>> pairs;       // the x_u + x_v >= 1 constraints
  size_t otherRows = 0;                         // any constraint of another shape (none expected)

public:
  CheckedSolver(LinearProgramSolver* s, const std::string& n) : inner(s), name(n) {}

  variable_id_t addVariable(double coefficient, VarType type, double lb, double ub) override {
    const variable_id_t id = inner->addVariable(coefficient, type, lb, ub);
    if (id >= 0) {
      if (static_cast<size_t>(id) >= cost.size()) cost.resize(static_cast<size_t>(id) + 1, 0.0);
      cost[static_cast<size_t>(id)] = coefficient;
    }
    return id;
  }

  constraint_id_t addConstraint(const std::vector<variable_id_t>& v, const std::vector<double>& c,
                                double rhs, ConstraintType type) override {
    if (v.size() == 2 && c.size() == 2 && c[0] == 1.0 && c[1] == 1.0 && rhs == 1.0 &&
        type == ConstraintType::GREATER_EQUAL)
      pairs.emplace_back(v[0], v[1]);
    else
      ++otherRows;
    return inner->addConstraint(v, c, rhs, type);
  }

  void setObjectiveType(ObjectiveType type) override { inner->setObjectiveType(type); }
  void setTimeLimit(double t) override { inner->setTimeLimit(t); }

  void reset() override {
    inner->reset();
    std::vector<double>().swap(cost);
    std::vector<std::pair<int, int>>().swap(pairs);
    otherRows = 0;
  }

  double solve(std::vector<double>& x) override {
    const double obj = inner->solve(x);

    size_t zero = 0, half = 0, one = 0, other = 0;
    double recomputed = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
      const double v = x[i];
      if (std::fabs(v) <= 1e-5) ++zero;
      else if (std::fabs(v - 0.5) <= 1e-5) ++half;
      else if (std::fabs(v - 1.0) <= 1e-5) ++one;
      else ++other;
      if (i < cost.size()) recomputed += cost[i] * v;
    }
    double worst = 0.0;   // largest amount by which any x_u + x_v >= 1 is violated
    for (const auto& p : pairs) {
      const double s = x.at(static_cast<size_t>(p.first)) + x.at(static_cast<size_t>(p.second));
      worst = std::max(worst, 1.0 - s);
    }

    std::cerr << std::setprecision(15)
              << "[lp-check] solver               : " << name << "\n"
              << "[lp-check] solver objective     : " << obj << "\n"
              << "[lp-check] recomputed objective : " << recomputed << "\n"
              << "[lp-check] variables            : " << x.size() << "\n"
              << "[lp-check] edge constraints     : " << pairs.size() << "\n"
              << "[lp-check] other constraints    : " << otherRows << "\n"
              << "[lp-check] count zero           : " << zero << "\n"
              << "[lp-check] count half           : " << half << "\n"
              << "[lp-check] count one            : " << one << "\n"
              << "[lp-check] count other          : " << other << "\n"
              << "[lp-check] max violation        : " << worst << "\n";
    return obj;
  }
};

//  Run the max-flow kernelizer and release the CCG before returning.
static void run_maxflow(const char* path, std::map<vid_t, bool>& out) {
  graph_t g;
  std::map<vid_t, bool> pre;
  build(path, g, pre);
  out = pre;
  std::map<vid_t, bool>().swap(pre);
  maxflow::KernelizerMaxflow<> k;
  k.kernelize(g, out);
  graph_t().swap(g);
}

//  Run an LP kernelizer with the given solver backend and release the CCG before returning.
//  KernelizerLinearProgramming takes ownership of the solver object.
static void run_lp(const char* path, std::map<vid_t, bool>& out, LinearProgramSolver* solver) {
  graph_t g;
  std::map<vid_t, bool> pre;
  build(path, g, pre);
  out = pre;
  std::map<vid_t, bool>().swap(pre);
  KernelizerLinearProgramming<> k(solver);
  k.kernelize(g, out);
  graph_t().swap(g);
}

static void dump(const std::map<vid_t, bool>& m, const char* path) {
  std::ofstream o(path);
  if (!o) { std::cerr << "cannot write " << path << "\n"; std::exit(2); }
  for (const auto& kv : m) o << kv.first << " " << (kv.second ? 1 : 0) << "\n";
  std::cerr << "wrote " << m.size() << " assignments to " << path << "\n";
}

static void load(const char* path, std::map<vid_t, bool>& m) {
  std::ifstream in(path);
  if (!in) { std::cerr << "cannot open " << path << "\n"; std::exit(2); }
  long v; int b;
  while (in >> v >> b) m[static_cast<vid_t>(v)] = (b != 0);
}

static std::string base_name(const char* path) {
  std::string s(path);
  size_t slash = s.find_last_of('/');
  return (slash == std::string::npos) ? s : s.substr(slash + 1);
}

//  Returns the number of variables the two kernels assigned OPPOSITE values
static size_t compare(const std::map<vid_t, bool>& a, const std::map<vid_t, bool>& b,
                      const std::string& label, const std::string& name_a, const std::string& name_b) {
  size_t only_a = 0, only_b = 0, agree = 0, conflict = 0;
  std::vector<std::pair<vid_t, bool>> opposite;     // variable and A's value, for the listing below
  for (const auto& kv : a) {
    auto it = b.find(kv.first);
    if (it == b.end()) { ++only_a; continue; }
    if (it->second == kv.second) ++agree;
    else { ++conflict; if (opposite.size() < 20) opposite.emplace_back(kv.first, kv.second); }
  }
  for (const auto& kv : b)
    if (a.find(kv.first) == a.end()) ++only_b;

  std::cout << "=== kernel comparison: " << label << " ===\n";
  std::cout << "  A = " << name_a << "\n";
  std::cout << "  B = " << name_b << "\n";
  std::cout << "  decided by A             : " << a.size()  << "\n";
  std::cout << "  decided by B             : " << b.size()  << "\n";
  std::cout << "  decided by A only        : " << only_a    << "\n";
  std::cout << "  decided by B only        : " << only_b    << "\n";
  std::cout << "  decided by both, SAME    : " << agree     << "\n";
  std::cout << "  decided by both, OPPOSITE: " << conflict  << "\n";
  for (const auto& o : opposite)
    std::cout << "    opposite: variable " << o.first << "   A=" << (o.second ? 1 : 0) << "  B=" << (o.second ? 0 : 1) << "\n";
  if (conflict > opposite.size()) std::cout << "    (first " << opposite.size() << " listed)\n";
  std::cout << "\n";
  if (conflict == 0)
    std::cout << "  VERDICT: no contradictions. The two kernels differ only in HOW MANY\n"
                 "           variables they decide, never in WHAT they decide. This is the\n"
                 "           expected consequence of the LP having multiple optimal solutions.\n";
  else
    std::cout << "  VERDICT: " << conflict << " variables were assigned OPPOSITE values.\n"
                 "           Between two DIFFERENT optimal LP solutions this can be a tie (both kernels safe);\n"
                 "           check the [lp-check] certificates of both runs before calling it a defect.\n";
  return conflict;
}

int main(int argc, char** argv) {
  if (argc >= 4 && std::strcmp(argv[1], "mf") == 0) {
    std::map<vid_t, bool> a;
    run_maxflow(argv[2], a);
    dump(a, argv[3]);
    return 0;
  }
  if (argc >= 4 && std::strcmp(argv[1], "lp") == 0) {
    std::map<vid_t, bool> a;
    run_lp(argv[2], a, new CheckedSolver(new LinearProgramSolverGurobi(), "gurobi"));
    dump(a, argv[3]);
    return 0;
  }
  if (argc >= 4 && std::strcmp(argv[1], "cplex") == 0) {
#ifdef HAVE_CPLEX
    std::map<vid_t, bool> a;
    run_lp(argv[2], a, new CheckedSolver(new LinearProgramSolverCplex(), "cplex"));
    dump(a, argv[3]);
    return 0;
#else
    std::cerr << "this binary was built without -DHAVE_CPLEX\n";
    return 1;
#endif
  }
  if (argc >= 4 && std::strcmp(argv[1], "cmp") == 0) {
    std::map<vid_t, bool> a, b;
    load(argv[2], a);
    load(argv[3], b);
    const size_t conflict = compare(a, b, base_name(argv[2]) + " vs " + base_name(argv[3]),
                                    base_name(argv[2]), base_name(argv[3]));
    return conflict == 0 ? 0 : 3;
  }

  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <file.wcsp>\n"
              << "       " << argv[0] << " mf|lp|cplex <file.wcsp> <out.txt>\n"
              << "       " << argv[0] << " cmp <a.txt> <b.txt>\n";
    return 1;
  }

  //  Single-process mode. Each CCG is destroyed before the next is built, so
  //  peak memory is one CCG plus whichever solver structure is larger.
  std::map<vid_t, bool> a_mf, a_lp;
  run_maxflow(argv[1], a_mf);
  std::cerr << "  [max-flow stage done: " << a_mf.size() << " decided]\n";
  run_lp(argv[1], a_lp, new LinearProgramSolverGurobi());
  const size_t conflict = compare(a_mf, a_lp, argv[1], "max-flow", "Gurobi LP");
  return conflict == 0 ? 0 : 3;
}