#!/usr/bin/python3
# Adapted from wcsp-solver/scripts/gen-random-wcsp.py
# Adds CLI args for variable/constraint count and a fixed seed for reproducibility
# Constraint arity is randomly 2-4 (like the original script) -- expect CCG blow-up vs WCSP variable count (auxiliary vertices per polynomial term)
import random
import itertools
import sys

def gen(num_variables, num_constraints, seed, arity_range=(2,4), cost_range=(0,10), integer_cost=True):
  random.seed(seed)
  out = []
  out.append('unknown {} 2 {} 99999'.format(num_variables, num_constraints))
  out.append('2 ' * num_variables)

  constraint_set = set()
  for _ in range(num_constraints):
    arity = random.randint(*arity_range)
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
  print(f"wrote {outfile}: {nv} vars, {nc} constraints, seed={seed}")