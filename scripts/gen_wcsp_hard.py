#!/usr/bin/env python3
# gen_wcsp_hard.py
# makes random Boolean WCSP instances where the vertex cover LP is likely to be fractional
# so the Nemhauser-Trotter kernelizer cannot decide every variable
# fractional LP comes from odd cycles with equal weights, a unit triangle has LP optimum 1.5 (all x = 0.5) but integer optimum 2
# so this generator has settings that make ties and odd structure
#
# parameters follow the usual random CSP model <n, d, p1, p2>
#   n  = variables, d = domain size (always 2 here)
#   p1 = constraint density, set by the constraint count m
#   p2 = constraint tightness, the part of the tuples that get the high cost (Model B, Model RB of Xu and Li 2006)
#
# cost models
#   binary  : cost is 0 or --cost-hi, a --tightness part of the tuples get the high cost, gives the most ties
#   narrow  : high costs come from a band of 3 values, ties are common but not always
#   uniform : costs from --cost-lo to --cost-hi, the easy case, kept for control instances
# topologies
#   random    : variables picked uniformly
#   scalefree : a few hub variables are in many constraints, gives high degree CCG vertices
#   clustered : variables split in communities, most constraints stay inside one
#
# usage: python3 scripts/gen_wcsp_hard.py -n 2000 -m 4000 --seed 1 -o out.wcsp --cost-model binary --tightness 0.5 --arity 2

import argparse
import itertools
import random
import sys


# [COST] the 2^arity costs of one constraint
def make_cost_table(rng, model, arity, tightness, lo, hi):
    ntuples = 2 ** arity

    if model == 'uniform':
        return [rng.randint(lo, hi) for _ in range(ntuples)]

    if model == 'binary':
        # [COST] the same penalty on a tightness part of the tuples, the equal penalties make the ties
        k = int(round(tightness * ntuples))
        costs = [hi] * k + [0] * (ntuples - k)
        rng.shuffle(costs)
        return costs

    if model == 'narrow':
        # [COST] penalties from hi-1 to hi+1, between binary and uniform
        k = int(round(tightness * ntuples))
        costs = [rng.randint(hi - 1, hi + 1) for _ in range(k)] + [0] * (ntuples - k)
        rng.shuffle(costs)
        return costs

    raise ValueError('unknown cost model: ' + model)


# [SCOPE] picks the variables of each constraint for the chosen topology
class ScopePicker:
    def __init__(self, rng, n, topology, communities, cross_prob):
        self.rng = rng
        self.n = n
        self.topology = topology
        self.cross_prob = cross_prob
        if topology == 'scalefree':
            # [SCOPE] one entry per constraint a variable is in, so popular variables are picked more
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


# [GEN] whole file as text and the number of constraints written
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
            # [GEN] no new scope found in 64 tries, skip this constraint
            continue
        seen.add(scope)
        emitted += 1

        out.append('{} {} 0 {}'.format(arity, ' '.join(str(i) for i in scope), 2 ** arity))
        costs = make_cost_table(rng, model, arity, tightness, lo, hi)
        for idx, assign in enumerate(itertools.product(*([[0, 1]] * arity))):
            out.append(' '.join(str(a) for a in assign) + ' ' + str(costs[idx]))

    # [GEN] the header has the real constraint count
    out[0] = 'unknown {} 2 {} 99999'.format(n, emitted)
    return '\n'.join(out) + '\n', emitted


def main():
    p = argparse.ArgumentParser(description='random WCSP with a fractional LP relaxation')
    p.add_argument('-n', '--variables', type=int, required=True)
    p.add_argument('-m', '--constraints', type=int, required=True)
    p.add_argument('--seed', type=int, default=0)
    p.add_argument('-o', '--out', required=True)
    p.add_argument('--arity', type=int, default=2, help='arity of every constraint (default 2)')
    p.add_argument('--cost-model', default='binary', choices=['binary', 'narrow', 'uniform'])
    p.add_argument('--tightness', type=float, default=0.5, help='part of the tuples with the high cost, 0 to 1 (default 0.5)')
    p.add_argument('--cost-lo', type=int, default=0)
    p.add_argument('--cost-hi', type=int, default=1, help='the high cost, 1 gives unit weights (default 1)')
    p.add_argument('--topology', default='random', choices=['random', 'scalefree', 'clustered'])
    p.add_argument('--communities', type=int, default=50)
    p.add_argument('--cross-prob', type=float, default=0.05)
    a = p.parse_args()

    if a.arity < 2:
        p.error('--arity must be at least 2')
    if not (0.0 <= a.tightness <= 1.0):
        p.error('--tightness must be between 0 and 1')

    text, emitted = gen(a.variables, a.constraints, a.seed, a.arity, a.cost_model, a.tightness,
                        a.cost_lo, a.cost_hi, a.topology, a.communities, a.cross_prob)
    with open(a.out, 'w') as f:
        f.write(text)

    if emitted < a.constraints:
        print('note: only {} of {} scopes were unique, header changed'.format(emitted, a.constraints), file=sys.stderr)
    print('wrote {}: n={} m={} arity={} model={} tightness={} hi={} topology={} seed={}'.format(
        a.out, a.variables, emitted, a.arity, a.cost_model, a.tightness, a.cost_hi, a.topology, a.seed))


if __name__ == '__main__':
    main()
