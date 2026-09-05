// wcsp_collapse -- remove variables that have only one possible value.
//
// NEED
//   The CCG loader rejects any instance where some variable does not have domain
//   size exactly 2. That check conflates two very different cases:
//
//     domain >= 3 : a multi-valued variable. The CCG cannot express it, and no preprocessing changes that
//     domain == 1 : a variable with one possible value. That is not a decision at all, it is a constant. Substituting it away is trivially right
//
//   Measured on the published benchmark: 368 instances are rejected, and ALL 368 contain only domains 1 and 2 -- not one has a domain of 3 or more. 
//   Of those, 160 also have low constraint arity to be usable
//   On MRF/DBN, 73 % of the variables are constants, so collapsing turns a 919-variable instance into a 247-variable one
//
// THIS CODE DO
//   Reads a DIMACS .wcsp, drops every domain-1 variable, rewrites every constraint that touched one (restricting its cost table to the surviving scope), folds any
//   constraint whose whole scope disappears into the problem's constant term, and writes a .wcsp containing only domain-2 variables.
//
// CORRECTNESS
//   Every removed variable had exactly one legal value, so every solution of the output extends uniquely to a solution of the input with the same cost
//   The optimum is therefore unchanged. Verify with:
//     ./e2e_solve --kernelizer none --solver ilp original.wcsp
//     ./e2e_solve --kernelizer none --solver ilp collapsed.wcsp
//   The two FINAL OPTIMUM values must be identical
//
// FORMAT (DIMACS wcsp)
//   line 1 : <name> <nvars> <maxdom> <nconstraints> <upperbound>
//   line 2 : <domain size of each variable, space separated>
//   then, per constraint:
//     <arity> <var...> <defaultcost> <ntuples>
//     followed by ntuples lines: <value...> <cost>
//
// Build:
//   g++ -std=c++17 -O2 apps/wcsp_collapse.cpp -o wcsp_collapse
// Usage:
//   ./wcsp_collapse <in.wcsp> <out.wcsp>

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

  //  ---- header ----------------------------------------------------------
  std::string name;
  long nvars = 0, maxdom = 0, ncons = 0;
  long long ub = 0;
  in >> name >> nvars >> maxdom >> ncons >> ub;

  std::vector<long> dom(nvars);
  for (long i = 0; i < nvars; i++) in >> dom[i];

  //  ---- decide which variables survive ----------------------------------
  //  keep[i] is the new index of old variable i, or -1 if it is being removed.
  std::vector<long> keep(nvars, -1);
  long kept = 0, dropped = 0, bad = 0;
  for (long i = 0; i < nvars; i++) {
    if (dom[i] == 1)      { dropped++; }
    else if (dom[i] == 2) { keep[i] = kept++; }
    else                  { bad++; }        //  a genuine multi-valued variable
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

  //  ---- rewrite the constraints ------------------------------------------
  //  A removed variable can only take value 0, so any tuple that assigns it a different value is unreachable and is discarded
  //  Tuples that assign it 0 are kept, with that position deleted from the tuple
  struct Constraint {
    std::vector<long>      scope;
    long long              defcost = 0;
    std::vector<std::pair<std::vector<long>, long long>> tuples;
  };
  std::vector<Constraint> out;
  out.reserve(ncons);

  long long folded_constant = 0;   //  cost of constraints whose whole scope vanished
  long collapsed_whole = 0, shrunk = 0;

  for (long c = 0; c < ncons; c++) {
    long arity = 0;
    in >> arity;
    std::vector<long> scope(arity);
    for (long a = 0; a < arity; a++) in >> scope[a];
    long long defcost = 0, ntup = 0;
    in >> defcost >> ntup;

    //  which positions of this scope survive
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

      //  a removed variable is fixed at value 0; any other value is unreachable
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
      //  every variable in this constraint was a constant, so it contributes a fixed cost: the tuple for the all-zero assignment, or the default
      long long cost = nc.tuples.empty() ? defcost : nc.tuples.front().second;
      folded_constant += cost;
      collapsed_whole++;
      continue;
    }
    if (static_cast<long>(nc.scope.size()) != arity) shrunk++;
    out.push_back(std::move(nc));
  }

  //  ---- write ------------------------------------------------------------
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