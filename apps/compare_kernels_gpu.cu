// Runs BOTH max-flow kernelizers (CPU and GPU) on the same CCG built from one
// .wcsp file, then compares the resulting assignments variable by variable.
//
// This answers: when the CPU and GPU kernelizers resolve different NUMBERS of
// variables, is that because the vertex-cover LP has several optimal solutions
// (different but equally valid min-cuts), or because one of them assigns a
// variable the WRONG value?
//
// Usage: ./compare_kernels_gpu <file.wcsp>

#include <iostream>
#include <fstream>
#include <map>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "src/integration/KernelizerMaxflow.h"
#include "src/integration/KernelizerMaxflowGPU.h"

typedef ConstraintCompositeGraph<>::variable_id_t vid_t;

static void build(const char* path, ConstraintCompositeGraph<>::graph_t& g, std::map<vid_t,bool>& pre) {
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

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: " << argv[0] << " <file.wcsp>\n"; return 1; }

  std::map<vid_t,bool> a_cpu, a_gpu;
  {
    ConstraintCompositeGraph<>::graph_t g;
    std::map<vid_t,bool> pre;
    build(argv[1], g, pre);
    a_cpu = pre;
    maxflow::KernelizerMaxflow<> kcpu;
    kcpu.kernelize(g, a_cpu);
  }
  {
    ConstraintCompositeGraph<>::graph_t g;
    std::map<vid_t,bool> pre;
    build(argv[1], g, pre);
    a_gpu = pre;
    maxflow::KernelizerMaxflowGPU<> kgpu;
    kgpu.kernelize(g, a_gpu);
  }

  size_t only_cpu = 0, only_gpu = 0, agree = 0, conflict = 0;
  for (const auto& kv : a_cpu) {
    auto it = a_gpu.find(kv.first);
    if (it == a_gpu.end()) { ++only_cpu; continue; }
    if (it->second == kv.second) ++agree; else ++conflict;
  }
  for (const auto& kv : a_gpu)
    if (a_cpu.find(kv.first) == a_cpu.end()) ++only_gpu;

  std::cout << "=== CPU vs GPU kernel comparison: " << argv[1] << " ===\n";
  std::cout << "  resolved by CPU max-flow : " << a_cpu.size() << "\n";
  std::cout << "  resolved by GPU max-flow : " << a_gpu.size() << "\n";
  std::cout << "  decided by CPU only      : " << only_cpu << "\n";
  std::cout << "  decided by GPU only      : " << only_gpu << "\n";
  std::cout << "  decided by both, SAME    : " << agree    << "\n";
  std::cout << "  decided by both, OPPOSITE: " << conflict << "\n\n";
  if (conflict == 0)
    std::cout << "  VERDICT: no contradictions. Both are valid NT kernels that differ\n"
                 "           only in HOW MANY variables they decide.\n";
  else
    std::cout << "  VERDICT: *** " << conflict << " variables assigned OPPOSITE values. ***\n"
                 "           At least one kernel is genuinely wrong.\n";
  return 0;
}
