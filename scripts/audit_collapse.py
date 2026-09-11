#!/usr/bin/env pythong3

'''
  audit_collapse.py - independent proof tht wcsp_collapse did not change the problem

  NEED:
    wcsp_collapse removes every domain-1 variable (a vriable with a single legal value, ie. a constant) and rewrites the constraints around it
    Claim is :
      optimum (original) == optimum (collapsed) + folded_constant
    
    Cannot solve the original to check claims just by solving the original because orignal is the file that CCG loader refuses to load
    This script proves without solving that by re-deriving the collapse from the oroignal file with its own parsr and comparing against what
    wcsp_collapse actually wrote.

  CHECKS:
    A   header    collapsed variable count / domains / upper bound
    B   structure every collapsed contraint equals the original constraint with constant positions slicd out
    C   constant  folded constant that is re-derived which is equal to collapse reported
    D   semantics cost_original(assignment, constants = 0) == cost_collapsed (assignment) + folded_constant
    E   hygine    warnings for odd inputs that can hide some silent loss
  
  EXIT CODES
  0   PASS    all checks agreed
  1   FAIL    check disagreed
  2   ERROR

'''

import argparse
import itertools
import random
import sys

# parsing

def _tokens(path):
  '''
    give whitespace-separated tokens
  '''
  with open(path, "r", errors="replace") as fh:
    for line in fh:
      for tok in line.split():
        yield tok

def _num(tok):
  '''
    costs are integers in practice but few contains decimals
  '''
  try:
    return int(tok)
  except ValueError:
    return float(tok)


def parse_wcsp(path):
  '''Read a DIMACS .wcsp into a plain dictionary.

  Layout:
    line 1        <name> <nvars> <maxdom> <nconstraints> <upperbound>
    line 2        <domain size per variable>
    per constraint  <arity> <var...> <defaultcost> <ntuples> then ntuples lines of  <value...> <cost>
  '''
  t = _tokens(path)
  try:
    name = next(t)
    nvars = int(next(t))
    maxdom = int(next(t))
    ncons = int(next(t))
    ub = _num(next(t))
    dom = [int(next(t)) for _ in range(nvars)]

    cons = []
    dup_tuples = 0
    for _ in range(ncons):
      arity = int(next(t))
      scope = [int(next(t)) for _ in range(arity)]
      defcost = _num(next(t))
      ntup = int(next(t))
      table = {}
      for _ in range(ntup):
        vals = tuple(int(next(t)) for _ in range(arity))
        cost = _num(next(t))
        if vals in table:
          dup_tuples += 1
        table[vals] = cost
      cons.append({"scope": scope, "def": defcost, "table": table})
  except StopIteration:
    raise ValueError("file ended early")

  return {"name": name, "nvars": nvars, "maxdom": maxdom, "ncons": ncons, "ub": ub, "dom": dom, "cons": cons, "dup_tuples": dup_tuples}


# re-derive the collapse

def derive_collapse(orig):
  '''Independently work out what the collapsed file SHOULD contain.

  Rule: a domain-1 variable can only ever hold value 0, so
    - it is deleted, and the surviving variables are renumbered in order
    - a tuple that gives it any other value is unreachable and is dropped
    - a constraint whose whole scope disappears becomes a fixed cost, which is added to the folded constant instead of written to the file
  '''
  keep, kept = {}, 0
  dropped, multi = 0, 0
  for i, d in enumerate(orig["dom"]):
    if d == 1:
      dropped += 1
    elif d == 2:
      keep[i] = kept
      kept += 1
    else:
      multi += 1

  warnings = []
  exp_cons = []
  folded_const = 0
  folded_cons = 0
  shrunk_cons = 0
  unreachable_tuples = 0

  for ci, c in enumerate(orig["cons"]):
    scope = c["scope"]
    survivors = [p for p, v in enumerate(scope) if v in keep]
    new_scope = [keep[scope[p]] for p in survivors]

    table = {}
    dups = 0
    for vals, cost in c["table"].items():
      # a constant is pinned to 0 and anything else cannot happen
      bad = any(vals[p] != 0 for p in range(len(scope)) if scope[p] not in keep)
      if bad:
        unreachable_tuples += 1
        continue
      key = tuple(vals[p] for p in survivors)
      if key in table:
        dups += 1
      table[key] = cost
    if dups:
      warnings.append("constraint %d: %d tuple(s) collide after slicing -- one silently overwrites another" % (ci, dups))

    if isinstance(c["def"], (int, float)) and c["def"] < 0:
        warnings.append("constraint %d: negative default cost %s -- the WCSP loader materialises only positive defaults, so it reads this as 0" % (ci, c["def"]))

    if not new_scope:
      # whole scope was constants: contributes one fixed number
      if len(table) > 1:
        warnings.append("constraint %d: fully folded but %d tuples survived; only the first is folded in" % (ci, len(table)))
      cost = next(iter(table.values())) if table else c["def"]
      folded_const += cost
      folded_cons += 1
      continue

    if len(new_scope) != len(scope):
      shrunk_cons += 1
    exp_cons.append({"scope": new_scope, "def": c["def"], "table": table})

  return {"keep": keep, "kept": kept, "dropped": dropped, "multi": multi,
          "cons": exp_cons, "folded_const": folded_const,
          "folded_cons": folded_cons, "shrunk_cons": shrunk_cons,
          "unreachable_tuples": unreachable_tuples, "warnings": warnings}


# evaluation

def cost_of(cons, assign):
  '''
    Total cost of one full assignment: every constraint looks its own tuple up in its table, falling back to the default cost
  '''
  total = 0
  for c in cons:
    key = tuple(assign[v] for v in c["scope"])
    total += c["table"].get(key, c["def"])
  return total


def orig_cost(orig, keep, sub):
  '''
    Cost of the original model when the surviving variables take the values in `sub` and every constant is 0
  '''
  full = [0] * orig["nvars"]
  for old, new in keep.items():
    full[old] = sub[new]
  return cost_of(orig["cons"], full)


# main

def main():
  ap = argparse.ArgumentParser(description="verify a wcsp_collapse output")
  ap.add_argument("original")
  ap.add_argument("collapsed")
  ap.add_argument("--samples", type=int, default=200, help="random assignments to test when not exhaustive (default 200)")
  ap.add_argument("--exhaustive-max", type=int, default=12, help="test EVERY assignment when the collapsed instance has at most that many variables (default 12)")
  ap.add_argument("--budget", type=int, default=2000000, help="cap on assignments x constraints, huge instances will stay fast (default 2000000)")
  ap.add_argument("--brute", type=int, default=0, metavar="K", help="if collapsed instance has at most K variables, also brute-force the original model's true optimum (0 = off)")
  ap.add_argument("--seed", type=int, default=1)
  ap.add_argument("--quiet", action="store_true")
  args = ap.parse_args()

  def out(*a):
    if not args.quiet:
      print(*a)

  try:
    orig = parse_wcsp(args.original)
    coll = parse_wcsp(args.collapsed)
  except (OSError, ValueError) as e:
    print("parse error: %s" % e, file=sys.stderr)
    return 2

  exp = derive_collapse(orig)
  fails = []
  warns = list(exp["warnings"])
  if orig["dup_tuples"]:
    warns.append("original lists %d duplicate tuple(s)" % orig["dup_tuples"])
  if exp["multi"]:
    fails.append("original has %d variable(s) with domain >= 3; collapse should have refused this file" % exp["multi"])

  # A: header
  if coll["nvars"] != exp["kept"]:
    fails.append("variable count: collapsed says %d, expected %d" % (coll["nvars"], exp["kept"]))
  if any(d != 2 for d in coll["dom"]):
    fails.append("collapsed still contains variable whose domain is not 2")
  if coll["maxdom"] != 2:
    fails.append("collapsed max domain is %d, expected 2" % coll["maxdom"])
  if coll["ub"] != orig["ub"]:
    warns.append("upper bound changed: %s -> %s" % (orig["ub"], coll["ub"]))

  # B: structure
  if coll["ncons"] != len(exp["cons"]):
    fails.append("constraint count: collapsed has %d, expected %d" % (coll["ncons"], len(exp["cons"])))
  else:
    shown = 0
    for i, (a, b) in enumerate(zip(exp["cons"], coll["cons"])):
      why = None
      if a["scope"] != b["scope"]:
        why = "scope %s != %s" % (a["scope"], b["scope"])
      elif a["def"] != b["def"]:
        why = "default cost %s != %s" % (a["def"], b["def"])
      elif a["table"] != b["table"]:
        miss = set(a["table"]) - set(b["table"])
        extra = set(b["table"]) - set(a["table"])
        diff = [k for k in set(a["table"]) & set(b["table"]) if a["table"][k] != b["table"][k]]
        why = ("cost table differs: %d missing, %d unexpected, %d with a different cost" % (len(miss), len(extra), len(diff)))
      if why:
        if shown < 5:
          fails.append("constraint %d: %s" % (i, why))
          shown += 1
        elif shown == 5:
          fails.append("... more constraint mismatches suppressed")
          shown += 1

  # C: folded constant (compared against the collapse tool own report by the calling script)
  folded = exp["folded_const"]

  # D: semantics
  n = coll["nvars"]
  ncons_eff = max(1, len(orig["cons"]))
  checked, mode = 0, "none"
  if fails:
    mode = "skipped"            # structure already disagreed so no need to store
  elif n == 0:
    mode = "trivial"
    checked = 1
    if orig_cost(orig, exp["keep"], []) != folded:
      fails.append("empty collapsed instance: original cost %s != folded constant %s" % (orig_cost(orig, exp["keep"], []), folded))
  else:
    budget_samples = max(20, args.budget // ncons_eff)
    if n <= args.exhaustive_max and 2 ** n <= budget_samples:
      mode = "exhaustive"
      space = itertools.product((0, 1), repeat=n)
    else:
      mode = "random"
      rng = random.Random(args.seed)
      k = min(args.samples, budget_samples)
      space = ([rng.randint(0, 1) for _ in range(n)] for _ in range(k))

    for sub in space:
      sub = list(sub)
      lhs = orig_cost(orig, exp["keep"], sub)
      rhs = cost_of(coll["cons"], sub) + folded
      checked += 1
      if lhs != rhs:
        fails.append("assignment %s: original cost %s != collapsed cost %s + folded %s" % (sub[:12], lhs, rhs - folded, folded))
        break

  # optional brute force
  brute = "NA"
  if args.brute and not fails and 0 < n <= args.brute:
    best = None
    for sub in itertools.product((0, 1), repeat=n):
      v = orig_cost(orig, exp["keep"], list(sub))
      if best is None or v < best:
        best = v
    brute = best

  # report
  out("original      : %s" % args.original)
  out("collapsed     : %s" % args.collapsed)
  out("variables     : %d -> %d   (%d constants removed)" % (orig["nvars"], coll["nvars"], exp["dropped"]))
  out("constraints   : %d -> %d   (%d folded away, %d shrunk)" % (orig["ncons"], coll["ncons"], exp["folded_cons"], exp["shrunk_cons"]))
  out("folded const  : %s" % folded)
  out("tuples dropped: %d  (unreachable: constant given value other than 0)" % exp["unreachable_tuples"])
  out("cost identity : %s, %d assignment(s) verified" % (mode, checked))
  for w in warns:
    out("WARNING       : %s" % w)
  for f in fails:
    out("MISMATCH      : %s" % f)
  out("verdict       : %s" % ("FAIL" if fails else "PASS"))

  print("AUDIT verdict=%s orig_vars=%d kept_vars=%d dropped_vars=%d orig_cons=%d kept_cons=%d folded_cons=%d shrunk_cons=%d folded_const=%s checked=%d mode=%s warnings=%d brute=%s"
    % ("FAIL" if fails else "PASS", orig["nvars"], coll["nvars"], exp["dropped"], orig["ncons"], coll["ncons"], exp["folded_cons"], exp["shrunk_cons"], folded, checked, mode, len(warns), brute))
  return 1 if fails else 0


if __name__ == "__main__":
  sys.exit(main())