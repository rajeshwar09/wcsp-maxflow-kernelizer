#!/usr/bin/env python3
"""Compare two nsys profiling runs of the GPU kernelizer (before / after a change).

usage: compare_runs.py BASE_SUMMARY NEW_SUMMARY OUT_TSV [GUROBI_TSV ...]

BASE_SUMMARY, NEW_SUMMARY : nsys_summary.tsv files written by scripts/run_nsys_profile.sh
OUT_TSV                   : comparison table, one row per instance (a .legend.txt is written next to it)
GUROBI_TSV                : optional pipeline_*_none_r1.tsv files from scripts/run_pipeline_timing.sh;
                            the Gurobi (kernelizer lp) kernelization time is the mean of its ok runs.
                            When an instance appears in several files, the LATER file wins.
"""
import csv
import os
import statistics as st
import sys


def load(path):
    with open(path, newline="") as f:
        return {r["instance"]: r for r in csv.DictReader(f, delimiter="\t")}


def load_gurobi(paths):
    g = {}
    for p in paths:
        with open(p, newline="") as f:
            per = {}
            for r in csv.DictReader(f, delimiter="\t"):
                if r.get("kernelizer") == "lp" and r.get("status") == "ok":
                    key = os.path.basename(r["instance"])
                    if key.endswith(".wcsp"):
                        key = key[:-5]
                    per.setdefault(key, []).append(float(r["t_kernelize"]))
        for k, v in per.items():
            g[k] = st.mean(v)  # later files override earlier ones
    return g


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def pr_pct(r):
    a, b = num(r.get("pct_push_relabel")), num(r.get("pct_remove_invalid"))
    return f"{a + b:.1f}" if a is not None and b is not None else "NA"


def f(v, d=2):
    return "NA" if v is None else f"{v:.{d}f}"


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    base, new, out = load(sys.argv[1]), load(sys.argv[2]), sys.argv[3]
    gur = load_gurobi(sys.argv[4:])

    missing = [k for k in base if k not in new]
    if missing:
        print("WARNING: in base but not in new run:", ", ".join(missing))

    rows, bad = [], []
    for k, b in base.items():
        if k not in new:
            continue
        n = new[k]
        tb, tn = float(b["plain_kern_s"]), float(n["plain_kern_s"])
        # no_trace = nsys failed but the plain run is valid (see scripts/nsys_summary.py)
        same = (b["plain_resolved"] == n["plain_resolved"]
                and b["resolved_match"] in ("yes", "no_trace") and n["resolved_match"] in ("yes", "no_trace"))
        if not same:
            bad.append(f"{k}: base {b['plain_resolved']}  new {n['plain_resolved']} (profiled {n['prof_resolved']})")
        g = gur.get(k)
        rows.append({
            "instance": k, "ccg_vertices": b["ccg_vertices"],
            "base_s": tb, "new_s": tn, "speedup": tb / tn if tn > 0 else None,
            "resolved_base": b["plain_resolved"], "resolved_new": n["plain_resolved"],
            "resolved_same": "yes" if same else "NO",
            "gurobi_s": g,
            "vs_gurobi_base": g / tb if g else None, "vs_gurobi_new": g / tn if g else None,
            "relabel_pct_base": b["pct_global_relabel"], "relabel_pct_new": n["pct_global_relabel"],
            "pushrem_pct_base": pr_pct(b), "pushrem_pct_new": pr_pct(n),
            "kernel_s_base": num(b["kernel_s"]), "kernel_s_new": num(n["kernel_s"]),
        })

    cols = ["instance", "ccg_vertices", "base_s", "new_s", "speedup", "resolved_base", "resolved_new",
            "resolved_same", "gurobi_s", "vs_gurobi_base", "vs_gurobi_new", "relabel_pct_base",
            "relabel_pct_new", "pushrem_pct_base", "pushrem_pct_new", "kernel_s_base", "kernel_s_new"]
    with open(out, "w") as fo:
        fo.write("\t".join(cols) + "\n")
        for r in rows:
            fo.write("\t".join(f(r[c], 3) if isinstance(r[c], float) else ("NA" if r[c] is None else str(r[c]))
                               for c in cols) + "\n")
    with open(os.path.splitext(out)[0] + ".legend.txt", "w") as fl:
        fl.write(f"""{os.path.basename(out)}  before/after comparison of two nsys profiling runs
  base run : {sys.argv[1]}
  new run  : {sys.argv[2]}
  base_s, new_s          kernelization time of the plain (unprofiled) run, seconds
  speedup                base_s / new_s ; above 1 = the change made it faster
  resolved_base/new      variables decided; resolved_same must be yes on every row
  gurobi_s               Gurobi LP kernelization time (mean of its runs) from: {', '.join(sys.argv[4:]) or 'not given'}
  vs_gurobi_base/new     gurobi_s / GPU time ; above 1 = GPU faster than Gurobi
  relabel_pct, pushrem_pct  share of GPU kernel time in global relabel / push+remove-invalid
  kernel_s_base/new      sum of GPU kernel time in the profiled run
""")

    print(f"{'instance':<42}{'ccg':>10}{'base s':>9}{'new s':>9}{'speedup':>9}{'gurobi':>9}{'vsG old':>9}{'vsG new':>9}  resolved")
    for r in rows:
        print(f"{r['instance'][:41]:<42}{r['ccg_vertices']:>10}{r['base_s']:>9.2f}{r['new_s']:>9.2f}"
              f"{f(r['speedup']):>9}{f(r['gurobi_s']):>9}{f(r['vs_gurobi_base']):>9}{f(r['vs_gurobi_new']):>9}  {r['resolved_same']}")
    tb = sum(r["base_s"] for r in rows)
    tn = sum(r["new_s"] for r in rows)
    wins_b = sum(1 for r in rows if r["vs_gurobi_base"] and r["vs_gurobi_base"] > 1)
    wins_n = sum(1 for r in rows if r["vs_gurobi_new"] and r["vs_gurobi_new"] > 1)
    with_g = sum(1 for r in rows if r["gurobi_s"])
    print(f"\ntotal: base {tb:.1f} s  new {tn:.1f} s  speedup {tb / tn:.2f}x")
    print(f"GPU faster than Gurobi: base {wins_b} / new {wins_n}  (of {with_g} instances with a Gurobi time)")
    if bad:
        print("\nERROR: resolved count changed on:")
        for x in bad:
            print("  " + x)
        sys.exit(1)
    print("resolved counts: identical on all instances")


if __name__ == "__main__":
    main()