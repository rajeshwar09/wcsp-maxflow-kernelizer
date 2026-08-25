#!/usr/bin/python3
"""
Generate WCSP instances designed to make the vertex-cover LP relaxation FRACTIONAL, so that Nemhauser-Trotter kernelization cannot resolve everything.

Fractionality in vertex cover comes from odd cycles with near-equal weights: a triangle with unit weights has LP optimum 1.5 (every x = 0.5) against an integer
optimum of 2. This generator therefore exposes the knobs that create ties and odd structure.

Parameters follow the standard random-CSP model <n, d, p1, p2>:
  n  = number of variables
  d  = domain size (2 here; all variables are Boolean)
  p1 = constraint density, controlled by the constraint count m
  p2 = constraint tightness, the fraction of tuples given the high cost

Tightness is the classical hardness parameter for random CSPs (Model B, and
Model RB of Xu & Li 2006, which underlies the BHOSLIB hard benchmark suite).

Cost models
-----------
binary  costs are either 0 or --cost-hi, with a --tightness fraction set high. This is the Model-B forbidden/allowed pattern expressed as costs, and the most likely to produce ties.
equal   every non-zero cost is identical. Maximum degeneracy.
narrow  non-zero costs drawn from a 3-value band, so ties are common but not universal.
uniform costs drawn from [cost-lo, cost-hi]. This is the easy baseline, kept so the same script can generate the control instances.

Topologies
----------
random     variables chosen uniformly (Erdos-Renyi style)
scalefree  preferential attachment: a few hub variables appear in many constraints, producing high-degree CCG vertices
clustered  variables split into communities; most constraints stay inside one


Usage:
python3 gen_wcsp_hard.py -n 2000 -m 4000 --seed 1 -o out.wcsp \\
    --cost-model binary --tightness 0.5 --arity 2
"""

import argparse
import itertools
import random
import sys


# ---------------------------------------------------------------- cost models

def make_cost_table(rng, model, arity, tightness, lo, hi):
  """Return the 2^arity costs for one constraint."""
  ntuples = 2 ** arity

  if model == 'uniform':
    return [rng.randint(lo, hi) for _ in range(ntuples)]

  if model == 'binary':
    #  Model-B style: a tightness fraction of tuples are penalised by the
    #  SAME amount, everything else costs nothing. Identical penalties are
    #  what produce the ties that make the LP fractional.
    k = int(round(tightness * ntuples))
    costs = [hi] * k + [0] * (ntuples - k)
    rng.shuffle(costs)
    return costs

  if model == 'narrow':
    #  A three-value band. Ties are frequent but not guaranteed, which sits
    #  between 'binary' and 'uniform'.
    k = int(round(tightness * ntuples))
    costs = [rng.randint(hi - 1, hi + 1) for _ in range(k)] + [0] * (ntuples - k)
    rng.shuffle(costs)
    return costs

  raise ValueError('unknown cost model: ' + model)


# ----------------------------------------------------------------- topologies

class ScopePicker:
  def __init__(self, rng, n, topology, communities, cross_prob):
    self.rng = rng
    self.n = n
    self.topology = topology
    self.cross_prob = cross_prob
    if topology == 'scalefree':
      #  Bag holds one entry per constraint a variable already belongs to,
      #  so drawing uniformly from it favours already-popular variables.
      self.bag = list(range(n))
    elif topology == 'clustered':
      self.k = max(1, min(communities, n))
      self.size = max(8, n // self.k)

  def pick(self, arity):
    if arity > self.n:
      raise ValueError('arity %d exceeds variable count %d' % (arity, self.n))

    if self.topology == 'random':
      return frozenset(self.rng.sample(range(self.n), arity))

    if self.topology == 'scalefree':
      chosen = set()
      guard = 0
      while len(chosen) < arity and guard < 200:
        chosen.add(self.rng.choice(self.bag))
        guard += 1
      while len(chosen) < arity:
        chosen.add(self.rng.randrange(self.n))
      for v in chosen:
        self.bag.append(v)
      return frozenset(chosen)

    if self.topology == 'clustered':
      if self.rng.random() < self.cross_prob:
        return frozenset(self.rng.sample(range(self.n), arity))
      c = self.rng.randrange(max(1, self.n // self.size))
      lo = c * self.size
      hi = min(self.n, lo + self.size)
      if hi - lo < arity:
        return frozenset(self.rng.sample(range(self.n), arity))
      return frozenset(self.rng.sample(range(lo, hi), arity))

    raise ValueError('unknown topology: ' + self.topology)


# ----------------------------------------------------------------- generation

def gen(n, m, seed, arity, model, tightness, lo, hi, topology, communities, cross_prob):
  rng = random.Random(seed)
  picker = ScopePicker(rng, n, topology, communities, cross_prob)

  out = ['unknown {} 2 {} 99999'.format(n, m), '2 ' * n]
  seen = set()
  emitted = 0

  for _ in range(m):
    scope = None
    for _attempt in range(64):
      cand = picker.pick(arity)
      if cand not in seen:
        scope = cand
        break
    if scope is None:
      continue          # scope space exhausted; stop adding duplicates
    seen.add(scope)
    emitted += 1

    out.append('{} {} 0 {}'.format(arity, ' '.join(str(i) for i in scope), 2 ** arity))
    costs = make_cost_table(rng, model, arity, tightness, lo, hi)
    for idx, assign in enumerate(itertools.product(*([[0, 1]] * arity))):
      out.append(' '.join(str(a) for a in assign) + ' ' + str(costs[idx]))

  out[0] = 'unknown {} 2 {} 99999'.format(n, emitted)
  return '\n'.join(out) + '\n', emitted


def main():
  p = argparse.ArgumentParser(description='Generate WCSP instances with a fractional LP relaxation.',formatter_class=argparse.RawDescriptionHelpFormatter)
  p.add_argument('-n', '--variables', type=int, required=True)
  p.add_argument('-m', '--constraints', type=int, required=True)
  p.add_argument('--seed', type=int, default=0)
  p.add_argument('-o', '--out', required=True)
  p.add_argument('--arity', type=int, default=2, help='constraint arity, fixed for every constraint (default 2)')
  p.add_argument('--cost-model', default='binary', choices=['binary', 'narrow', 'uniform'])
  p.add_argument('--tightness', type=float, default=0.5, help='fraction of tuples given the high cost, 0..1 (default 0.5)')
  p.add_argument('--cost-lo', type=int, default=0)
  p.add_argument('--cost-hi', type=int, default=1, help='the high cost. 1 gives unit weights, the classic fractional-LP setting (default 1)')
  p.add_argument('--topology', default='random', choices=['random', 'scalefree', 'clustered'])
  p.add_argument('--communities', type=int, default=50)
  p.add_argument('--cross-prob', type=float, default=0.05)
  a = p.parse_args()

  if a.arity < 2:
    p.error('--arity must be at least 2')
  if not (0.0 <= a.tightness <= 1.0):
    p.error('--tightness must be between 0 and 1')

  text, emitted = gen(a.variables, a.constraints, a.seed, a.arity, a.cost_model, a.tightness, a.cost_lo, a.cost_hi, a.topology, a.communities, a.cross_prob)
  with open(a.out, 'w') as f:
    f.write(text)

  if emitted < a.constraints:
    print('note: only {} of {} scopes were unique; header adjusted'.format(
      emitted, a.constraints), file=sys.stderr)
  print('wrote {}: n={} m={} arity={} model={} tightness={} hi={} topology={} seed={}'.format(a.out, a.variables, emitted, a.arity, a.cost_model, a.tightness, a.cost_hi, a.topology, a.seed))


if __name__ == '__main__':
  main()