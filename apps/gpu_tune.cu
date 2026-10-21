// builds the flow network of one instance once, then runs only the GPU max-flow many times with different settings, so a settings sweep costs seconds instead of a full pipeline run
// it changes one setting at a time and checks that every run gives the same flow value and the same number of decided vertices as the defaults
// it first warms up the GPU with untimed runs, then measures the settings in a mixed order, every setting once per round
//
// build: made by ./scripts/build.sh gates (needs nvcc)
// usage: ./gpu_tune [options] <file.wcsp|file.uai>
//   --repeat N            runs of every setting                          (default 3)
//   --hub-list L          values of hub_deg, comma list                   (default 0,16,32,64,128,256)
//   --block-list L        values of block_deg                             (default 0,256,512,1024,2048,4096)
//   --threads-list L      values of block_threads, 0 = automatic          (default 0,256,512,1024)
//   --gr-list L           values of gr_period                             (default 25,50,75,100,150,400)
//   --max-arity N         skip instances with a wider constraint          (default 15)
//
// output lines, read by scripts/run_tune.sh:
//   [info] name : value        instance, sizes, the default settings and the warm up
//   [deg] lo hi : count        degree histogram of the flow network, source and sink not counted
//   [run] knob value rep solve_s passes flow decided same
//   [mean] knob value solve_s passes same                (mean over the repeats)
//   [best] knob value solve_s default_s gain_pct         (best value of every knob, and "all" for the overall best)
// exit codes: 0 ok, 1 bad options, 2 file not readable, 5 non Boolean or domain-1 variable, 6 arity too high,
//             7 a setting gave a different flow value or decided count

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "third_party/wcsp-solver/src/WCSPInstance.h"
#include "third_party/wcsp-solver/src/ConstraintCompositeGraph.h"
#include "src/integration/KernelizerMaxflow.h"
#include "src/gpu/maxflow_gpu_worklist.h"

using namespace maxflow;
typedef ConstraintCompositeGraph<> ccg_t;
typedef ccg_t::graph_t graph_t;
typedef ccg_t::variable_id_t vid_t;
typedef boost::graph_traits<graph_t>::vertex_descriptor vertex_t;

// [RUN] numbers of one GPU max-flow run
struct run_result {
  double solve_s;
  double passes;
  cap_t flow;
  long decided;
};

// [ARGS] "0,16,32" to a list of numbers
static std::vector<int> parse_list(const std::string& s) {
  std::vector<int> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      out.push_back(std::atoi(item.c_str()));
    }
  }
  return out;
}

// [OUTPUT] a number with a fixed count of decimals
static std::string fix(double v, int decimals) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

// [ARITY] largest constraint arity of the instance
template <class Inst>
static size_t max_constraint_arity(const Inst& inst) {
  size_t m = 0;
  for (const auto& c : inst.getConstraints()) {
    m = std::max(m, c.getVariables().size());
  }
  return m;
}

// [RUN] one GPU max-flow with the current gpu_config(), the solver times itself as time.gpu.solve
static run_result run_once(flow_network<cap_t>& net, int n) {
  double s0 = metrics::get().value("time.gpu.solve");
  double p0 = metrics::get().value("gpu.passes");
  run_result r;
  gpu_worklist_solver solver(net);
  r.flow = solver.solve();
  r.solve_s = metrics::get().value("time.gpu.solve") - s0;
  r.passes = metrics::get().value("gpu.passes") - p0;
  r.decided = 0;
  for (int i = 0; i < n; i++) {
    if (solver.is_on_source_side(i + 1) != solver.is_on_source_side(n + i + 1)) {
      r.decided++;
    }
  }
  return r;
}

int main(int argc, char** argv) {
  // [ARGS] defaults
  int repeat = 3;
  long max_arity = 15;
  std::vector<int> hub_list = {0, 16, 32, 64, 128, 256};
  std::vector<int> block_list = {0, 256, 512, 1024, 2048, 4096};
  std::vector<int> threads_list = {0, 256, 512, 1024};
  std::vector<int> gr_list = {25, 50, 75, 100, 150, 400};
  const char* path = nullptr;

  // [ARGS] read the command line
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    bool has_val = (i + 1 < argc);
    if (a == "--repeat" && has_val) repeat = std::max(1, std::atoi(argv[++i]));
    else if (a == "--hub-list" && has_val) hub_list = parse_list(argv[++i]);
    else if (a == "--block-list" && has_val) block_list = parse_list(argv[++i]);
    else if (a == "--threads-list" && has_val) threads_list = parse_list(argv[++i]);
    else if (a == "--gr-list" && has_val) gr_list = parse_list(argv[++i]);
    else if (a == "--max-arity" && has_val) max_arity = std::atol(argv[++i]);
    else if (a[0] == '-') {
      std::cerr << "unknown option: " << a << "\n";
      return 1;
    } else {
      path = argv[i];
    }
  }
  if (path == nullptr) {
    std::cerr << "usage: " << argv[0] << " [options] <file.wcsp|file.uai>\n";
    return 1;
  }

  // [PARSE] format from the file extension
  std::string p(path);
  WCSPInstance<>::Format fformat = (p.size() >= 4 && p.compare(p.size() - 4, 4, ".uai") == 0) ? WCSPInstance<>::Format::UAI : WCSPInstance<>::Format::DIMACS;
  std::ifstream in(path);
  if (!in) {
    std::cerr << "cannot open " << path << "\n";
    return 2;
  }
  std::unique_ptr<WCSPInstance<>> instp;
  try {
    instp.reset(new WCSPInstance<>(in, fformat));
  } catch (const std::domain_error& e) {
    std::cout << "[info] skip : " << e.what() << "\n";
    return 5;
  }
  size_t arity = max_constraint_arity(*instp);
  if (static_cast<long>(arity) > max_arity) {
    std::cout << "[info] skip : constraint arity " << arity << " exceeds --max-arity " << max_arity << "\n";
    return 6;
  }

  // [CCG] same steps as e2e_solve before the kernelizer
  ccg_t ccg;
  WCSPInstance<>::constraint_t::Polynomial poly;
  for (const auto& c : instp->getConstraints()) {
    c.toPolynomial(poly);
  }
  ccg.addPolynomial(poly);
  std::map<vid_t, bool> pre;
  ccg.simplify(pre);
  graph_t g = *ccg.getGraph();
  instp.reset();

  // [FLOW] the double cover network, built once and used by every run
  std::vector<vertex_t> ccg_vertices;
  std::map<vertex_t, int> vertex_index;
  boost::graph_traits<graph_t>::vertex_iterator vi, vi_end;
  for (std::tie(vi, vi_end) = boost::vertices(g); vi != vi_end; ++vi) {
    vertex_index[*vi] = static_cast<int>(ccg_vertices.size());
    ccg_vertices.push_back(*vi);
  }
  int n = static_cast<int>(ccg_vertices.size());
  if (n == 0) {
    std::cout << "[info] skip : empty CCG\n";
    return 0;
  }
  flow_network<cap_t> net;
  perturb_config no_perturb;
  build_double_cover<ccg_t>(g, ccg_vertices, vertex_index, no_perturb, net);

  std::cout << std::setprecision(15);
  std::cout << "[info] instance : " << path << "\n";
  std::cout << "[info] ccg_vertices : " << n << "\n";
  std::cout << "[info] flow_nodes : " << net.num_nodes << "\n";
  std::cout << "[info] flow_half_edges : " << net.num_edges << "\n";

  // [DEGREE] histogram in powers of two (1, 2-3, 4-7, ...), source and sink are listed alone
  std::vector<long> buckets(32, 0);
  int max_deg = 0;
  for (int v = 0; v < net.num_nodes; v++) {
    if (v == net.source || v == net.sink) {
      continue;
    }
    int d = net.offset[v + 1] - net.offset[v];
    max_deg = std::max(max_deg, d);
    int b = 0;
    while ((2 << b) <= d) {
      b++;
    }
    buckets[d == 0 ? 0 : b]++;
  }
  for (int b = 0; b < 32; b++) {
    if (buckets[b] > 0) {
      std::cout << "[deg] " << (1 << b) << " " << ((2 << b) - 1) << " : " << buckets[b] << "\n";
    }
  }
  std::cout << "[info] max_degree : " << max_deg << "\n";
  std::cout << "[info] source_degree : " << net.offset[net.source + 1] - net.offset[net.source] << "\n";
  std::cout << "[info] sink_degree : " << net.offset[net.sink + 1] - net.offset[net.sink] << "\n";

  // [DEFAULTS] the values every knob goes back to
  gpu_settings& st = gpu_config();
  const gpu_settings defaults = st;
  std::cout << "[info] defaults : hub_deg=" << defaults.hub_deg << " block_deg=" << defaults.block_deg
            << " block_threads=" << defaults.block_threads << " gr_period=" << defaults.gr_period << "\n";

  // [WARMUP] untimed runs with the defaults, at least 5 runs and 1 second, the first runs after a pause are slower
  // the first run also gives the reference answer
  double t_warm = now_sec();
  run_result ref = run_once(net, n);
  int warm_runs = 1;
  while (warm_runs < 5 || now_sec() - t_warm < 1.0) {
    run_once(net, n);
    warm_runs++;
  }
  std::cout << "[info] warmup_runs : " << warm_runs << "\n";
  std::cout << "[info] warmup_s : " << fix(now_sec() - t_warm, 3) << "\n";
  std::cout << "[info] reference_flow : " << ref.flow << "\n";
  std::cout << "[info] reference_decided : " << ref.decided << "\n";

  // [SWEEP] every knob with its list of values, "default" is the run with all defaults
  struct knob {
    std::string name;
    int* field;
    std::vector<int> values;
  };
  std::vector<knob> knobs = {
    {"default", nullptr, {0}},
    {"hub_deg", &st.hub_deg, hub_list},
    {"block_deg", &st.block_deg, block_list},
    {"block_threads", &st.block_threads, threads_list},
    {"gr_period", &st.gr_period, gr_list},
  };

  // [SWEEP] flat list of the settings to measure, the default value of a knob is skipped, the "default" row already has it
  struct setting {
    std::string knob;
    int* field;
    int value;
    double sum_s;
    double sum_passes;
    bool same;
  };
  std::vector<setting> settings;
  for (const knob& k : knobs) {
    for (int value : k.values) {
      if (k.field != nullptr && *k.field == value) {
        continue;
      }
      settings.push_back({k.name, k.field, value, 0.0, 0.0, true});
    }
  }

  // [SWEEP] mixed order, every setting once per round, so a slow down during the sweep hits all settings the same
  for (int rep = 1; rep <= repeat; rep++) {
    for (setting& s : settings) {
      st = defaults;
      if (s.field != nullptr) {
        *s.field = s.value;
      }
      run_result r = run_once(net, n);
      bool ok = (r.flow == ref.flow) && (r.decided == ref.decided);
      s.same = s.same && ok;
      s.sum_s += r.solve_s;
      s.sum_passes += r.passes;
      std::cout << "[run] " << s.knob << " " << s.value << " " << rep << " " << fix(r.solve_s, 4) << " " << fix(r.passes, 0)
                << " " << r.flow << " " << r.decided << " " << (ok ? "yes" : "NO") << "\n";
    }
  }
  st = defaults;

  // [MEAN] mean of every setting, the "default" row is the first one
  bool all_same = true;
  double default_s = settings[0].sum_s / repeat;
  for (const setting& s : settings) {
    all_same = all_same && s.same;
    std::cout << "[mean] " << s.knob << " " << s.value << " " << fix(s.sum_s / repeat, 4) << " " << fix(s.sum_passes / repeat, 0) << " "
              << (s.same ? "yes" : "NO") << "\n";
  }

  // [BEST] best value of every knob, only settings that give the same answer can win
  std::string best_all = "default";
  double best_all_s = default_s;
  for (const knob& k : knobs) {
    if (k.field == nullptr) {
      continue;
    }
    std::string best_value = "-";
    double best_s = 0.0;
    for (const setting& s : settings) {
      double mean_s = s.sum_s / repeat;
      if (s.knob != k.name || !s.same) {
        continue;
      }
      if (best_value == "-" || mean_s < best_s) {
        best_value = std::to_string(s.value);
        best_s = mean_s;
      }
      if (mean_s < best_all_s) {
        best_all = k.name + "=" + std::to_string(s.value);
        best_all_s = mean_s;
      }
    }
    if (best_value != "-") {
      double gain = default_s > 0 ? 100.0 * (default_s - best_s) / default_s : 0.0;
      std::cout << "[best] " << k.name << " " << best_value << " " << fix(best_s, 4) << " " << fix(default_s, 4) << " " << fix(gain, 1) << "\n";
    }
  }

  // [BEST] overall best single change, "default" when no change beats the defaults
  double gain_all = default_s > 0 ? 100.0 * (default_s - best_all_s) / default_s : 0.0;
  std::cout << "[best] all " << best_all << " " << fix(best_all_s, 4) << " " << fix(default_s, 4) << " " << fix(gain_all, 1) << "\n";
  std::cout << "[info] all_same : " << (all_same ? "yes" : "NO") << "\n";
  return all_same ? 0 : 7;
}
