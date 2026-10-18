// gate test: runs the CPU and the GPU max-flow kernelizers on the same CCG from one .wcsp file
// and compares their assignments variable by variable
// a different count is fine (the LP can have more than one optimum, so more than one valid min cut),
// a variable with opposite values means one of the two kernels is wrong
// build: made by ./scripts/build.sh gates (needs nvcc)
// usage: ./compare_kernels_gpu <file.wcsp>

#include <iostream>
#include <fstream>
#include <map>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "src/integration/KernelizerMaxflow.h"
#include "src/integration/KernelizerMaxflowGPU.h"

typedef ConstraintCompositeGraph<>::variable_id_t vid_t;

// [CCG] builds the CCG of the file and simplifies it, pre gets the variables fixed by simplify
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

  // [RUN] CPU then GPU kernelizer, each on its own fresh CCG
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

  // [COMPARE] decided by one side only, by both with the same value, by both with opposite values
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
