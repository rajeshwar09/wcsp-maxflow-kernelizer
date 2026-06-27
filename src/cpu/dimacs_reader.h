#ifndef MAXFLOW_DIMACS_READER_H
#define MAXFLOW_DIMACS_READER_H

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>

#include "src/cpu/graph_csr.h"

namespace maxflow {
  
  // Read flow network in DIMACS max-flow format and fill flow_network<cap_t>
  // Format (1-index based) : 
  // c <comment>
  // p max <num_nodes> <num_edges>
  // n <id> s (source)
  // n <id> t (sink)
  // a <u> <v> <cap> (directed edge u->v with capacity cap)
  // convert 1-based to 0-based internally
  template <typename cap_t>
  flow_network<cap_t> read_dimacs_maxflow(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open input file: " + path);

    vertex_id_t num_nodes = 0;
    vertex_id_t source_1based = -1;
    vertex_id_t sink_1based = -1;
    std::vector<edge<cap_t>> edges;

    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      std::istringstream ss(line);
      char tag;
      ss >> tag;
      if (tag == 'c') {
        continue;
      } else if (tag == 'p') {
        std::string problem;
        int n, m;
        ss >> problem >> n >> m;
        num_nodes = n;
      } else if (tag == 'n') {
        int id;
        char which;
        ss >> id >> which;
        if (which == 's') {
          source_1based = id;
        } else if (which == 't') {
          sink_1based = id;
        }
      } else if (tag == 'a') {
        int u, v;
        cap_t c;
        ss >> u >> v >> c;
        edges.push_back(edge<cap_t>{u - 1, v - 1, c}); // 0-based
      }
    }

    if (source_1based < 0 || sink_1based < 0) {
      throw std::runtime_error("input must specity both source and sink");
    }

    flow_network<cap_t> net;
    net.build_from_edges(num_nodes, source_1based - 1, sink_1based - 1, edges);

    return net;
  }


} // namespace maxflow

#endif // MAXFLOW_DIMACS_READER_H