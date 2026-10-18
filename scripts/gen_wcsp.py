#!/usr/bin/env python3
# gen_wcsp.py
# makes a random Boolean WCSP file with a fixed seed, so the same command always gives the same file
# adapted from wcsp-solver/scripts/gen-random-wcsp.py
# default: every constraint gets a random arity of 2 to 4, the CCG then grows a lot compared to the variable count
# --pairwise: every constraint has arity 2, this keeps the CCG small (one polynomial term per constraint)
# usage: python3 scripts/gen_wcsp.py <variables> <constraints> <seed> <out.wcsp> [--pairwise]
# files made by the old gen_wcsp_lowarity.py are the same as --pairwise here (same seed, same bytes)

import argparse
import itertools
import random


# [GEN] whole file as text, costs are whole numbers from 0 to 10
def gen(num_variables, num_constraints, seed, pairwise):
    random.seed(seed)
    out = ['unknown {} 2 {} 99999'.format(num_variables, num_constraints)]
    out.append('2 ' * num_variables)

    seen = set()
    for _ in range(num_constraints):
        # [GEN] the random arity is drawn only in the default mode, so the random sequence of both modes stays as before
        arity = 2 if pairwise else random.randint(2, 4)
        line = [str(arity)]
        while True:
            variables = frozenset(random.sample(range(num_variables), arity))
            if variables not in seen:
                break
        seen.add(variables)
        line.append(' '.join(str(i) for i in variables))
        line.append('0 {}'.format(2 ** arity))
        out.append(' '.join(line))

        for assignment in itertools.product(*([[0, 1]] * arity)):
            out.append(' '.join(str(a) for a in assignment) + ' ' + str(random.randint(0, 10)))
    return '\n'.join(out) + '\n'


def main():
    p = argparse.ArgumentParser(description='random Boolean WCSP with a fixed seed')
    p.add_argument('variables', type=int)
    p.add_argument('constraints', type=int)
    p.add_argument('seed', type=int)
    p.add_argument('out')
    p.add_argument('--pairwise', action='store_true', help='every constraint has arity 2')
    a = p.parse_args()

    with open(a.out, 'w') as f:
        f.write(gen(a.variables, a.constraints, a.seed, a.pairwise))
    kind = 'pairwise' if a.pairwise else 'mixed arity 2-4'
    print('wrote {}: {} vars, {} constraints, {}, seed={}'.format(a.out, a.variables, a.constraints, kind, a.seed))


if __name__ == '__main__':
    main()
