// Demo : read flow network in DIMACS max-flow format and print its structure

#include <iostream>
#include <string>

#include "src/cpu/dimacs_reader.h"

int main(int argc, char const *argv[]) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << "<graph.dimacs\n";
    return 1;
  }

  using cap_t = long long;

  wmk::flow_network<cap_t> net = wmk::read_dimacs_maxflow<cap_t>(argv[1]);
  net.print_debug(std::cout);

  return 0;
}
