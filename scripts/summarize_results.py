#!/usr/bin/python3
"""
Runs e2e_pipeline_nokernelize (or e2e_pipeline_test) on a list of
.wcsp files and produces a single comparison table -- including the vertex-count trend at each pipeline stage -- for easy comparison across benchmark scales

Usage:
  python3 scripts/summarize_results.py data/wcsp/small.wcsp data/wcsp/mid.wcsp ...

  # Or, to automatic get every .wcsp file in a folder:
  python3 scripts/summarize_results.py --dir data/wcsp

  # To also run the full kernelize step give --with-kernelize
  # This requires ./e2e_pipeline_test to exist
  python3 scripts/summarize_results.py --dir data/wcsp --with-kernelize
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time


def parse_nokernelize_output(text):
  """Extract fields from e2e_pipeline_nokernelize's stdout."""
  out = {}
  patterns = {
    "parse_s":          r"\[parse DIMACS\]\s+([\d.eE+-]+) s",
    "toPolynomial_s":   r"\[toPolynomial all\]\s+([\d.eE+-]+) s",
    "addPolynomial_s":  r"\[ccg\.addPolynomial\]\s+([\d.eE+-]+) s",
    "simplify_s":       r"\[ccg\.simplify\]\s+([\d.eE+-]+) s",
    "getGraph_s":       r"\[getGraph copy\]\s+([\d.eE+-]+) s",
    "remnant_s":        r"Remnant s=([\d.eE+-]+)",
    "vars_simplified":  r"Variables simplified: (\d+)",
    "post_simplify_vars": r"Post-simplify vars: (\d+)",
    "type1_aux":        r"type1 aux: (\d+)",
    "type2_aux":        r"type2 aux: (\d+)",
    "ccg_vertices":     r"CCG vertex count \(post-simplify, pre-kernelize\): (\d+)",
    "total_excl_s":     r"TOTAL \(excl\. kernelize\): ([\d.eE+-]+) s",
  }
  for key, pat in patterns.items():
    m = re.search(pat, text)
    if m:
      out[key] = m.group(1)
  return out


def parse_full_output(text):
  """Extract fields from e2e_pipeline_test's stdout (includes KernelizerMaxflow timing)."""
  out = {}
  patterns = {
      "parse_s":          r"\[parse DIMACS\]\s+([\d.eE+-]+) s",
      "toPolynomial_s":   r"\[toPolynomial all\]\s+([\d.eE+-]+) s",
      "addPolynomial_s":  r"\[ccg\.addPolynomial\]\s+([\d.eE+-]+) s",
      "simplify_s":       r"\[ccg\.simplify\]\s+([\d.eE+-]+) s",
      "getGraph_s":       r"\[getGraph copy\]\s+([\d.eE+-]+) s",
      "kernelize_s":      r"\[KernelizerMaxflow\]\s+([\d.eE+-]+) s",
      "total_s":          r"TOTAL: ([\d.eE+-]+) s",
      "remnant_s":        r"Remnant s=([\d.eE+-]+)",
      "resolved":         r"resolved=(\d+)",
  }
  for key, pat in patterns.items():
      m = re.search(pat, text)
      if m:
          out[key] = m.group(1)
  return out


def count_wcsp_vars(filepath):
  """Read the WCSP DIMACS header line to get the original variable count."""
  with open(filepath) as f:
      first_line = f.readline().strip()
  # format: "unknown <num_vars> <max_arity_field> <num_constraints> <default_cost>"
  parts = first_line.split()
  if len(parts) >= 2:
      try:
          return int(parts[1])
      except ValueError:
          return None
  return None


def run_binary(binary, filepath, timeout=None):
  t0 = time.time()
  try:
      result = subprocess.run(
          [binary, filepath], capture_output=True, text=True, timeout=timeout
      )
      elapsed = time.time() - t0
      return result.stdout, result.returncode, elapsed
  except subprocess.TimeoutExpired:
      return "", -1, time.time() - t0


def fmt(val, width=14):
  if val is None:
      return "-".rjust(width)
  return str(val).rjust(width)


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("files", nargs="*", help=".wcsp files to run")
  ap.add_argument("--dir", help="directory to auto-discover .wcsp files from")
  ap.add_argument("--with-kernelize", action="store_true", help="also run ./e2e_pipeline_test (full pipeline incl. KernelizerMaxflow)")
  ap.add_argument("--nokernelize-bin", default="./e2e_pipeline_nokernelize")
  ap.add_argument("--full-bin", default="./e2e_pipeline_test")
  ap.add_argument("--timeout", type=float, default=None, help="per-file timeout in seconds (applies to --with-kernelize runs)")
  ap.add_argument("--csv", help="optional path to also write a CSV file")
  args = ap.parse_args()

  files = list(args.files)
  if args.dir:
      files += sorted(glob.glob(os.path.join(args.dir, "*.wcsp")))
  if not files:
      print("No .wcsp files given. Use positional args or --dir.", file=sys.stderr)
      sys.exit(1)

  rows = []
  for f in files:
      if not os.path.exists(f):
          print(f"warning: {f} not found, skipping", file=sys.stderr)
          continue

      wcsp_vars = count_wcsp_vars(f)
      row = {"file": os.path.basename(f), "wcsp_vars": wcsp_vars}

      print(f"Running nokernelize on {f} ...", file=sys.stderr)
      stdout, rc, elapsed = run_binary(args.nokernelize_bin, f)
      if rc != 0:
          print(f"  -> nokernelize failed (exit {rc}) on {f}", file=sys.stderr)
      row.update(parse_nokernelize_output(stdout))
      row["wall_s_nokernelize"] = f"{elapsed:.2f}"

      if args.with_kernelize:
          print(f"Running FULL pipeline (with kernelize) on {f} ...", file=sys.stderr)
          stdout2, rc2, elapsed2 = run_binary(args.full_bin, f, timeout=args.timeout)
          if rc2 != 0:
              print(f"  -> full pipeline failed or timed out (exit {rc2}) on {f}", file=sys.stderr)
          row.update(parse_full_output(stdout2))
          row["wall_s_full"] = f"{elapsed2:.2f}"

      rows.append(row)

  # Table 1: stage timings
  print("\n" + "=" * 100)
  print("STAGE TIMINGS (seconds)")
  print("=" * 100)
  header = ["file", "wcsp_vars", "parse", "toPoly", "addPoly", "simplify", "getGraph"]
  if args.with_kernelize:
      header += ["kernelize", "TOTAL"]
  else:
      header += ["TOTAL(excl.kern)"]
  print(" | ".join(h.ljust(16) for h in header))
  print("-" * 100)
  for r in rows:
      line = [
          r["file"].ljust(16),
          fmt(r.get("wcsp_vars"), 16),
          fmt(r.get("parse_s"), 16),
          fmt(r.get("toPolynomial_s"), 16),
          fmt(r.get("addPolynomial_s"), 16),
          fmt(r.get("simplify_s"), 16),
          fmt(r.get("getGraph_s"), 16),
      ]
      if args.with_kernelize:
          line.append(fmt(r.get("kernelize_s"), 16))
          line.append(fmt(r.get("total_s"), 16))
      else:
          line.append(fmt(r.get("total_excl_s"), 16))
      print(" | ".join(line))

  # Table 2: vertex-count trend (the "size at each stage" view)
  print("\n" + "=" * 100)
  print("VERTEX / VARIABLE COUNT TREND (see the blow-up at each stage)")
  print("=" * 100)
  header2 = ["file", "WCSP vars\n(input)", "post-simplify\nvars", "type1 aux", "type2 aux", "CCG vertices\n(final, pre-kernelize)", "blow-up\nfactor"]
  print(" | ".join(h.replace("\n", " ").ljust(20) for h in header2))
  print("-" * 130)
  for r in rows:
      wcsp_vars = r.get("wcsp_vars")
      ccg_vertices = r.get("ccg_vertices")
      blowup = "-"
      if wcsp_vars and ccg_vertices:
          try:
              blowup = f"{int(ccg_vertices) / int(wcsp_vars):.2f}x"
          except (ValueError, ZeroDivisionError):
              pass
      line = [
          r["file"].ljust(20),
          fmt(wcsp_vars, 20),
          fmt(r.get("post_simplify_vars"), 20),
          fmt(r.get("type1_aux"), 20),
          fmt(r.get("type2_aux"), 20),
          fmt(ccg_vertices, 20),
          fmt(blowup, 20),
      ]
      print(" | ".join(line))

  # Table 3: kernelization outcome (only if --with-kernelize)
  if args.with_kernelize:
      print("\n" + "=" * 100)
      print("KERNELIZATION OUTCOME")
      print("=" * 100)
      header3 = ["file", "Remnant s", "resolved (vars decided)"]
      print(" | ".join(h.ljust(24) for h in header3))
      print("-" * 80)
      for r in rows:
          line = [
              r["file"].ljust(24),
              fmt(r.get("remnant_s"), 24),
              fmt(r.get("resolved"), 24),
          ]
          print(" | ".join(line))

  # Optional CSV export
  if args.csv:
      import csv
      all_keys = set()
      for r in rows:
          all_keys.update(r.keys())
      all_keys = sorted(all_keys)
      with open(args.csv, "w", newline="") as f:
          writer = csv.DictWriter(f, fieldnames=all_keys)
          writer.writeheader()
          writer.writerows(rows)
      print(f"\nWrote CSV to {args.csv}", file=sys.stderr)


if __name__ == "__main__":
  main()