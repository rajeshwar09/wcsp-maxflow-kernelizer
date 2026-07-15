#!/usr/bin/python3
# Low-arity variant of gen_wcsp.py: forces arity=2 for every constraint (pairwise only),
# which minimizes the polynomial-expansion blow-up in the CCG (each 2-variable
# constraint only ever adds ONE multi-variable polynomial term).
import random
import itertools
import sys

def gen(num_variables, num_constraints, seed, cost_range=(0,10), integer_cost=True):
  random.seed(seed)
  out = []
  out.append('unknown {} 2 {} 99999'.format(num_variables, num_constraints))
  out.append('2 ' * num_variables)

  constraint_set = set()
  arity = 2  # fixed: pairwise constraints only
  for _ in range(num_constraints):
    line = [str(arity)]
    while True:
      variables = frozenset(random.sample(range(num_variables), arity))
      if variables not in constraint_set:
        break
    constraint_set.add(variables)
    line.append(' '.join(str(i) for i in variables))
    line.append('0 {}'.format(2 ** arity))
    out.append(' '.join(line))

    for assignments in itertools.product(*([[0, 1],] * arity)):
      cost = random.randint(*cost_range) if integer_cost else random.uniform(*cost_range)
      out.append(' '.join(str(a) for a in assignments) + ' ' + str(cost))
  return '\n'.join(out) + '\n'

if __name__ == '__main__':
  nv = int(sys.argv[1])
  nc = int(sys.argv[2])
  seed = int(sys.argv[3])
  outfile = sys.argv[4]
  with open(outfile, 'w') as f:
    f.write(gen(nv, nc, seed))
  print(f"wrote {outfile}: {nv} vars, {nc} constraints (arity=2 fixed), seed={seed}")