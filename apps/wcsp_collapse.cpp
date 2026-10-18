// removes the variables that have only one possible value (domain 1) from a DIMACS .wcsp file
// the CCG loader refuses any variable whose domain is not exactly 2, but that mixes two different cases:
//   domain >= 3 : a real multi valued variable, the CCG cannot express it
//   domain == 1 : not a decision at all, it is a constant and can be substituted away
// on the published benchmark all 368 refused instances have only domains 1 and 2, on MRF/DBN 73 % of the variables are
// constants, so a 919 variable instance becomes a 247 variable one
//
// what it is doing: drops every domain-1 variable, cuts the cost table of every constraint that had one down to the remaining scope,
// adds a constraint whose whole scope is gone to a fixed constant, and writes a file with only domain-2 variables
// why correct: a removed variable had only one value, so every solution of the output extends to exactly one solution of the
// input with the same cost, the optimum changes only by the folded constant (scripts/audit_collapse.py checks this)
//
// DIMACS wcsp layout
//   line 1         : <name> <nvars> <maxdom> <nconstraints> <upperbound>
//   line 2         : domain size of every variable
//   per constraint : <arity> <var ...> <defaultcost> <ntuples>, then ntuples lines of <value ...> <cost>
//
// build: g++ -std=c++17 -O2 apps/wcsp_collapse.cpp -o wcsp_collapse
// usage: ./wcsp_collapse <in.wcsp> <out.wcsp>
// exit codes: 0 done, 1 bad arguments, 2 file not readable or writable, 3 a domain of 3 or more, 4 no domain-1 variable

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <in.wcsp> <out.wcsp>\n";
    return 1;
  }

  std::ifstream in(argv[1]);
  if (!in) { std::cerr << "cannot open " << argv[1] << "\n"; return 2; }

  // [HEADER] name, sizes and domains
  std::string name;
  long nvars = 0, maxdom = 0, ncons = 0;
  long long ub = 0;
  in >> name >> nvars >> maxdom >> ncons >> ub;

  std::vector<long> dom(nvars);
  for (long i = 0; i < nvars; i++) in >> dom[i];

  // [KEEP] keep[i] is the new number of old variable i, or -1 when it is removed
  std::vector<long> keep(nvars, -1);
  long kept = 0, dropped = 0, bad = 0;
  for (long i = 0; i < nvars; i++) {
    if (dom[i] == 1)      { dropped++; }
    else if (dom[i] == 2) { keep[i] = kept++; }
    else                  { bad++; }        // a real multi valued variable
  }

  if (bad > 0) {
    std::cerr << "refusing: " << bad << " variable(s) have domain >= 3. "
                 "This tool only removes domain-1 constants.\n";
    return 3;
  }
  if (dropped == 0) {
    std::cerr << "nothing to do: no domain-1 variables in this instance\n";
    return 4;
  }

  // [REWRITE] a removed variable can only take value 0, a tuple with another value for it can never happen and is dropped
  // tuples with 0 for it are kept, with that position taken out
  struct Constraint {
    std::vector<long>      scope;
    long long              defcost = 0;
    std::vector<std::pair<std::vector<long>, long long>> tuples;
  };
  std::vector<Constraint> out;
  out.reserve(ncons);

  long long folded_constant = 0;   // cost of the constraints whose whole scope is gone
  long collapsed_whole = 0, shrunk = 0;

  for (long c = 0; c < ncons; c++) {
    long arity = 0;
    in >> arity;
    std::vector<long> scope(arity);
    for (long a = 0; a < arity; a++) in >> scope[a];
    long long defcost = 0, ntup = 0;
    in >> defcost >> ntup;

    // [REWRITE] positions of this scope that stay
    std::vector<long> survivors;
    for (long a = 0; a < arity; a++) if (keep[scope[a]] >= 0) survivors.push_back(a);

    Constraint nc;
    for (long a : survivors) nc.scope.push_back(keep[scope[a]]);
    nc.defcost = defcost;

    for (long t = 0; t < ntup; t++) {
      std::vector<long> vals(arity);
      for (long a = 0; a < arity; a++) in >> vals[a];
      long long cost = 0;
      in >> cost;

      // [REWRITE] skip the tuple when a removed variable has a value other than 0
      bool reachable = true;
      for (long a = 0; a < arity; a++)
        if (keep[scope[a]] < 0 && vals[a] != 0) { reachable = false; break; }
      if (!reachable) continue;

      std::vector<long> nv;
      nv.reserve(survivors.size());
      for (long a : survivors) nv.push_back(vals[a]);
      nc.tuples.emplace_back(std::move(nv), cost);
    }

    if (nc.scope.empty()) {
      // [FOLD] every variable was a constant, the constraint is one fixed cost: the all zero tuple, or the default
      long long cost = nc.tuples.empty() ? defcost : nc.tuples.front().second;
      folded_constant += cost;
      collapsed_whole++;
      continue;
    }
    if (static_cast<long>(nc.scope.size()) != arity) shrunk++;
    out.push_back(std::move(nc));
  }

  // [WRITE] the collapsed file, every domain is 2
  std::ofstream o(argv[2]);
  if (!o) { std::cerr << "cannot write " << argv[2] << "\n"; return 2; }

  o << name << " " << kept << " 2 " << out.size() << " " << ub << "\n";
  for (long i = 0; i < kept; i++) o << (i ? " " : "") << 2;
  o << "\n";

  for (const auto& c : out) {
    o << c.scope.size();
    for (long v : c.scope) o << " " << v;
    o << " " << c.defcost << " " << c.tuples.size() << "\n";
    for (const auto& t : c.tuples) {
      for (size_t k = 0; k < t.first.size(); k++) o << (k ? " " : "") << t.first[k];
      o << " " << t.second << "\n";
    }
  }

  // [REPORT] sizes before and after, the folded constant is not in the output file
  std::cerr << "collapse: " << nvars << " -> " << kept << " variables"
            << "  (" << dropped << " constants removed)\n"
            << "          " << ncons << " -> " << out.size() << " constraints"
            << "  (" << collapsed_whole << " folded away, " << shrunk << " shrunk)\n"
            << "          folded constant cost: " << folded_constant << "\n";
  if (folded_constant != 0) {
    std::cerr << "WARNING: a constant cost of " << folded_constant
              << " was folded out and is NOT in the output file.\n";
  }
  return 0;
}
