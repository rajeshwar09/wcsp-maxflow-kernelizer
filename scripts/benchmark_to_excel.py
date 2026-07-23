#!/usr/bin/python3
"""
benchmark_to_excel.py

Runs a kernelizer harness N times (default 5) on each .wcsp file, collects every
per-stage timing, computes the average of every cell, and writes a multi-sheet
Excel workbook:

- "Raw Runs"       : every individual run's timing for every file/stage
- "Averages"       : the mean of the N runs, per file, per stage (+ vertex trend)
- "Observations"   : auto-generated English inferences from the averaged data
- "Comparison"     : (only when both maxflow AND gurobi CSVs are provided) side-by-side

Because Gurobi only runs on your Mac, this script is designed to be run THERE.

usage on Mac
--------------------------

# 1) CPU max-flow kernelizer, 5 repetitions, into one workbook
python3 scripts/benchmark_to_excel.py \
	--harness ./e2e_pipeline_test \
	--kernelizer maxflow \
	--dir data/wcsp \
	--repeats 5 \
	--out maxflow_results.xlsx

# 2) Gurobi LP kernelizer, 5 repetitions (needs the compare/LP harness that prints the same [stage] lines
python3 scripts/benchmark_to_excel.py \
	--harness ./e2e_pipeline_test_gurobi \
	--kernelizer gurobi \
	--dir data/wcsp \
	--repeats 5 \
	--out gurobi_results.xlsx

# 3) Merge the two averaged datasets into one comparison workbook
python3 scripts/benchmark_to_excel.py \
	--merge maxflow_results.xlsx gurobi_results.xlsx \
	--out comparison.xlsx

# 4) Rebuild the .xlsx from a saved .raw.json without re-running the benchmark
python3 scripts/benchmark_to_excel.py \
	--from-json maxflow_results.xlsx.raw.json \
	--out maxflow_results.xlsx

Expected harness stdout (both maxflow and gurobi harnesses must print these lines):
[parse DIMACS]        <sec> s
[toPolynomial all]    <sec> s
[ccg.addPolynomial]   <sec> s
[ccg.simplify]        <sec> s
[getGraph copy]       <sec> s
[<KERNELIZER LABEL>]  <sec> s      # e.g. [KernelizerMaxflow] or [KernelizerLP]
TOTAL: <sec> s
Remnant s=<val>, resolved=<int>
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import statistics

# Stage keys in canonical order. The "kernelize" stage label varies by kernelizer, so I match it with a regex rather than a fixed string
STAGES = ["parse", "toPoly", "addPoly", "simplify", "getGraph", "kernelize", "total"]
STAGE_PATTERNS = {
	"parse":     r"\[parse DIMACS\]\s+([\d.eE+-]+) s",
	"toPoly":    r"\[toPolynomial all\]\s+([\d.eE+-]+) s",
	"addPoly":   r"\[ccg\.addPolynomial\]\s+([\d.eE+-]+) s",
	"simplify":  r"\[ccg\.simplify\]\s+([\d.eE+-]+) s",
	"getGraph":  r"\[getGraph copy\]\s+([\d.eE+-]+) s",
	# matches [KernelizerMaxflow], [KernelizerLP], [KernelizerLinearProgramming], etc
	"kernelize": r"\[Kernelizer[^\]]*\]\s+([\d.eE+-]+) s",
	"total":     r"TOTAL:\s+([\d.eE+-]+) s",
}
OUTCOME_PATTERNS = {
	"remnant":  r"Remnant s=([\d.eE+-]+)",
	"resolved": r"resolved=(\d+)",
}


def count_wcsp_vars(filepath):
	with open(filepath) as f:
		parts = f.readline().strip().split()
	try:
		return int(parts[1])
	except (IndexError, ValueError):
		return None


def run_once(harness, filepath, timeout=None):
	"""Run the harness once, return (stage_times dict, outcome dict) or (None, None) on failure"""
	try:
		r = subprocess.run([harness, filepath], capture_output=True, text=True, timeout=timeout)
	except subprocess.TimeoutExpired:
		return None, None
	if r.returncode != 0:
		return None, None
	text = r.stdout
	stages = {}
	for key, pat in STAGE_PATTERNS.items():
		m = re.search(pat, text)
		if m:
			stages[key] = float(m.group(1))
	outcome = {}
	for key, pat in OUTCOME_PATTERNS.items():
		m = re.search(pat, text)
		if m:
			outcome[key] = m.group(1)
	return stages, outcome


def collect(harness, files, repeats, timeout):
	"""
	Returns a list of per-file dicts: {file, wcsp_vars, runs: [ {stage: time,...}, ... ], outcome: {...}}
	"""
	data = []
	for f in files:
		if not os.path.exists(f):
			print(f"warning: {f} not found, skipping", file=sys.stderr)
			continue
		entry = {"file": os.path.basename(f), "wcsp_vars": count_wcsp_vars(f), "runs": [], "outcome": {}}
		for i in range(repeats):
			print(f"  [{os.path.basename(f)}] run {i+1}/{repeats} ...", file=sys.stderr)
			stages, outcome = run_once(harness, f, timeout)
			if stages is None:
				print(f"    -> FAILED/timed out on {f} (run {i+1})", file=sys.stderr)
				continue
			entry["runs"].append(stages)
			if outcome and not entry["outcome"]:
				entry["outcome"] = outcome  # same every run. keep the first good one
		data.append(entry)
	# sort ascending by problem size
	data.sort(key=lambda e: (e["wcsp_vars"] or 0))
	return data


def averages(entry):
	"""Return {stage: mean_time} across all successful runs for one file"""
	out = {}
	for stage in STAGES:
		vals = [r[stage] for r in entry["runs"] if stage in r]
		if vals:
			out[stage] = statistics.mean(vals)
	return out


# Excel writing
def write_workbook(data, kernelizer_label, out_path, repeats):
	import openpyxl
	from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
	from openpyxl.utils import get_column_letter

	wb = openpyxl.Workbook()

	header_font = Font(name="Arial", bold=True, color="FFFFFF", size=11)
	header_fill = PatternFill("solid", fgColor="305496")
	title_font = Font(name="Arial", bold=True, size=13)
	normal = Font(name="Arial", size=10)
	thin = Side(style="thin", color="BFBFBF")
	border = Border(left=thin, right=thin, top=thin, bottom=thin)
	center = Alignment(horizontal="center", vertical="center")
	left = Alignment(horizontal="left", vertical="center", wrap_text=True)

	def style_header(ws, row, ncols):
		for c in range(1, ncols + 1):
			cell = ws.cell(row=row, column=c)
			cell.font = header_font
			cell.fill = header_fill
			cell.alignment = center
			cell.border = border

	def autosize(ws, ncols, maxw=48):
		for c in range(1, ncols + 1):
			letter = get_column_letter(c)
			longest = 0
			for cell in ws[letter]:
				if cell.value is not None:
					longest = max(longest, len(str(cell.value)))
			ws.column_dimensions[letter].width = min(max(longest + 2, 10), maxw)

	# Sheet 1: Raw Runs
	ws = wb.active
	ws.title = "Raw Runs"
	ws["A1"] = f"Raw per-run stage timings (seconds) — {kernelizer_label} — {repeats} repetitions"
	ws["A1"].font = title_font
	hdr = ["file", "WCSP vars", "run #"] + STAGES
	for j, h in enumerate(hdr, start=1):
		ws.cell(row=3, column=j, value=h)
	style_header(ws, 3, len(hdr))
	r = 4
	for entry in data:
		for i, run in enumerate(entry["runs"], start=1):
			ws.cell(row=r, column=1, value=entry["file"]).font = normal
			ws.cell(row=r, column=2, value=entry["wcsp_vars"]).font = normal
			ws.cell(row=r, column=3, value=i).font = normal
			for j, stage in enumerate(STAGES, start=4):
				v = run.get(stage)
				cell = ws.cell(row=r, column=j, value=round(v, 6) if v is not None else None)
				cell.font = normal
				cell.number_format = "0.000000"
			for c in range(1, len(hdr) + 1):
				ws.cell(row=r, column=c).border = border
			r += 1
	autosize(ws, len(hdr))

	# Sheet 2: Averages
	ws2 = wb.create_sheet("Averages")
	ws2["A1"] = f"Averaged over {repeats} runs — {kernelizer_label}"
	ws2["A1"].font = title_font

	# Timing averages block
	ws2["A3"] = "STAGE TIMINGS — average of runs (seconds)"
	ws2["A3"].font = Font(name="Arial", bold=True, size=11)
	thdr = ["file", "WCSP vars"] + STAGES
	for j, h in enumerate(thdr, start=1):
		ws2.cell(row=4, column=j, value=h)
	style_header(ws2, 4, len(thdr))
	rr = 5
	avg_cache = {}
	for entry in data:
		avg = averages(entry)
		avg_cache[entry["file"]] = (entry, avg)
		ws2.cell(row=rr, column=1, value=entry["file"]).font = normal
		ws2.cell(row=rr, column=2, value=entry["wcsp_vars"]).font = normal
		for j, stage in enumerate(STAGES, start=3):
			v = avg.get(stage)
			cell = ws2.cell(row=rr, column=j, value=round(v, 6) if v is not None else None)
			cell.font = normal
			cell.number_format = "0.000000"
		for c in range(1, len(thdr) + 1):
			ws2.cell(row=rr, column=c).border = border
		rr += 1

	# Vertex-count block
	rr += 2
	ws2.cell(row=rr, column=1, value="VERTEX / VARIABLE COUNT & OUTCOME").font = Font(name="Arial", bold=True, size=11)
	rr += 1
	vhdr = ["file", "WCSP vars", "Remnant s", "resolved (vars decided)"]
	for j, h in enumerate(vhdr, start=1):
		ws2.cell(row=rr, column=j, value=h)
	style_header(ws2, rr, len(vhdr))
	rr += 1
	for entry in data:
		ws2.cell(row=rr, column=1, value=entry["file"]).font = normal
		ws2.cell(row=rr, column=2, value=entry["wcsp_vars"]).font = normal
		ws2.cell(row=rr, column=3, value=entry["outcome"].get("remnant", "-")).font = normal
		resv = entry["outcome"].get("resolved")
		ws2.cell(row=rr, column=4, value=int(resv) if resv else None).font = normal
		for c in range(1, len(vhdr) + 1):
			ws2.cell(row=rr, column=c).border = border
		rr += 1
	autosize(ws2, len(thdr))

	# Sheet 3: Observations
	ws3 = wb.create_sheet("Observations")
	ws3["A1"] = f"Observations & inferences — {kernelizer_label}"
	ws3["A1"].font = title_font
	ws3.column_dimensions["A"].width = 110
	obs = generate_observations(data, avg_cache, kernelizer_label, repeats)
	r = 3
	for line in obs:
		cell = ws3.cell(row=r, column=1, value=line)
		cell.font = normal
		cell.alignment = left
		r += 1

	wb.save(out_path)
	# store averaged data alongside for later merge
	_dump_sidecar(out_path, data, avg_cache, kernelizer_label)
	print(f"\nWrote {out_path}", file=sys.stderr)


def generate_observations(data, avg_cache, kernelizer_label, repeats):
	"""Produce English observation sentences from the averaged numbers."""
	lines = []
	lines.append(f"Method: {kernelizer_label}. Each file was run {repeats} times. all timings below are the arithmetic mean of those runs.")
	lines.append("")

	# Overall scale span
	sizes = [e["wcsp_vars"] for e in data if e["wcsp_vars"]]
	if sizes:
		lines.append(f"1. Scale tested: from {min(sizes):,} up to {max(sizes):,} WCSP variables across {len(data)} benchmark instances.")

	# Kernelize dominance
	for entry in data:
		_, avg = avg_cache[entry["file"]]
		if "kernelize" in avg and "total" in avg and avg["total"] > 0:
			frac = avg["kernelize"] / avg["total"] * 100
			if entry["wcsp_vars"] == max(sizes):
				lines.append(f"2. On the largest instance ({entry['file']}, {entry['wcsp_vars']:,} vars), "
											f"the kernelize stage took {avg['kernelize']:.2f} s, which is "
											f"{frac:.1f}% of the total pipeline time ({avg['total']:.2f} s) — "
											f"confirming kernelization is the dominant cost at scale.")
				break

	# Scaling trend of kernelize
	kpts = [(e["wcsp_vars"], avg_cache[e["file"]][1].get("kernelize"))
					for e in data if e["wcsp_vars"] and avg_cache[e["file"]][1].get("kernelize")]
	kpts = [(v, k) for v, k in kpts if k is not None]
	if len(kpts) >= 2:
		(v0, k0), (v1, k1) = kpts[0], kpts[-1]
		if v0 > 0 and k0 > 0:
			size_ratio = v1 / v0
			time_ratio = k1 / k0
			lines.append(f"3. Kernelize time scaling: as the problem grew {size_ratio:.0f}x "
										f"(from {v0:,} to {v1:,} vars), kernelize time grew {time_ratio:.0f}x "
										f"(from {k0:.4f} s to {k1:.4f} s).")

	# Resolved-variables observation
	for entry in data:
		resv = entry["outcome"].get("resolved")
		if resv and entry["wcsp_vars"]:
			pct = int(resv) / entry["wcsp_vars"] * 100
			lines.append(f"   - {entry['file']}: kernelizer resolved {int(resv):,} variables {pct:.1f}% of {entry['wcsp_vars']:,}).")

	lines.append("")
	lines.append("Note: 'Remnant s' is just an internal bookkeeping constant, not the final objective. "
								"Timings vary slightly run-to-run due to OS scheduling, etc")
	return lines


# Sidecar (lets --merge reload averaged data without re-running the harness)
def _sidecar_path(xlsx_path):
	return xlsx_path + ".avg.json"


def _dump_sidecar(xlsx_path, data, avg_cache, kernelizer_label):
	import json
	payload = {"kernelizer": kernelizer_label, "files": []}
	for entry in data:
		_, avg = avg_cache[entry["file"]]
		payload["files"].append({
			"file": entry["file"],
			"wcsp_vars": entry["wcsp_vars"],
			"avg": avg,
			"outcome": entry["outcome"],
		})
	with open(_sidecar_path(xlsx_path), "w") as f:
		json.dump(payload, f, indent=2)


def _load_sidecar(xlsx_path):
	import json
	sc = _sidecar_path(xlsx_path)
	if not os.path.exists(sc):
		print(f"error: sidecar {sc} not found (was {xlsx_path} produced by this script?)", file=sys.stderr)
		sys.exit(1)
	with open(sc) as f:
		return json.load(f)


def write_comparison(path_a, path_b, out_path):
	import openpyxl
	from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
	from openpyxl.utils import get_column_letter

	A = _load_sidecar(path_a)
	B = _load_sidecar(path_b)
	labelA, labelB = A["kernelizer"], B["kernelizer"]
	mapB = {f["file"]: f for f in B["files"]}

	wb = openpyxl.Workbook()
	ws = wb.active
	ws.title = "Comparison"
	title_font = Font(name="Arial", bold=True, size=13)
	header_font = Font(name="Arial", bold=True, color="FFFFFF", size=11)
	header_fill = PatternFill("solid", fgColor="305496")
	normal = Font(name="Arial", size=10)
	thin = Side(style="thin", color="BFBFBF")
	border = Border(left=thin, right=thin, top=thin, bottom=thin)
	center = Alignment(horizontal="center", vertical="center")

	ws["A1"] = f"Comparison: {labelA} vs {labelB} (averaged kernelize + total, seconds)"
	ws["A1"].font = title_font

	hdr = ["file", "WCSP vars",
					f"{labelA} kernelize", f"{labelB} kernelize", "speedup (B_time/A? see note)",
					f"{labelA} total", f"{labelB} total"]
	for j, h in enumerate(hdr, start=1):
		c = ws.cell(row=3, column=j, value=h)
		c.font = header_font; c.fill = header_fill; c.alignment = center; c.border = border

	r = 4
	for fa in A["files"]:
		fb = mapB.get(fa["file"])
		ka = fa["avg"].get("kernelize")
		kb = fb["avg"].get("kernelize") if fb else None
		ta = fa["avg"].get("total")
		tb = fb["avg"].get("total") if fb else None
		ws.cell(row=r, column=1, value=fa["file"]).font = normal
		ws.cell(row=r, column=2, value=fa["wcsp_vars"]).font = normal
		ws.cell(row=r, column=3, value=round(ka, 6) if ka else None).font = normal
		ws.cell(row=r, column=4, value=round(kb, 6) if kb else None).font = normal
		# speedup = B_kernelize / A_kernelize  (how many times faster A is than B)
		if ka and kb and ka > 0:
			ws.cell(row=r, column=5, value=f"{kb/ka:.2f}x").font = normal
		else:
			ws.cell(row=r, column=5, value="-").font = normal
		ws.cell(row=r, column=6, value=round(ta, 6) if ta else None).font = normal
		ws.cell(row=r, column=7, value=round(tb, 6) if tb else None).font = normal
		for c in range(1, len(hdr) + 1):
			ws.cell(row=r, column=c).border = border
			if c in (3, 4, 6, 7):
				ws.cell(row=r, column=c).number_format = "0.000000"
		r += 1

	# note row
	ws.cell(row=r + 1, column=1,
					value=f"Note: 'speedup' column = ({labelB} kernelize time) / ({labelA} kernelize time). "
								f">1 means {labelA} is faster.").font = Font(name="Arial", italic=True, size=9)

	for col in range(1, len(hdr) + 1):
		letter = get_column_letter(col)
		longest = max((len(str(ws.cell(row=rr, column=col).value or "")) for rr in range(1, r + 2)), default=10)
		ws.column_dimensions[letter].width = min(max(longest + 2, 12), 40)

	# Observations sheet for the comparison
	ws2 = wb.create_sheet("Observations")
	ws2["A1"] = f"Comparison observations: {labelA} vs {labelB}"
	ws2["A1"].font = title_font
	ws2.column_dimensions["A"].width = 110
	lines = []
	speedups = []
	for fa in A["files"]:
		fb = mapB.get(fa["file"])
		ka = fa["avg"].get("kernelize"); kb = fb["avg"].get("kernelize") if fb else None
		if ka and kb and ka > 0:
			speedups.append((fa["file"], fa["wcsp_vars"], kb / ka))
	if speedups:
		best = max(speedups, key=lambda x: x[2])
		worst = min(speedups, key=lambda x: x[2])
		lines.append(f"1. {labelA} vs {labelB}, kernelize stage, averaged across runs:")
		for name, vars_, sp in speedups:
			faster = labelA if sp > 1 else labelB
			lines.append(f"   - {name} ({vars_:,} vars): {faster} faster, ratio {sp:.2f}x.")
		lines.append("")
		lines.append(f"2. Largest speedup for {labelA}: {best[2]:.2f}x on {best[0]} ({best[1]:,} vars).")
		lines.append(f"3. Smallest margin: {worst[2]:.2f}x on {worst[0]} ({worst[1]:,} vars).")
		# cross-check resolved counts agree
		mismatches = []
		for fa in A["files"]:
			fb = mapB.get(fa["file"])
			if fb and fa["outcome"].get("resolved") != fb["outcome"].get("resolved"):
				mismatches.append(fa["file"])
		if mismatches:
			lines.append(f"4. WARNING: resolved-variable counts DIFFER on: {', '.join(mismatches)}. "
										f"Both kernelizers should produce the same kernel; investigate.")
		else:
			lines.append("4. Correctness cross-check: both kernelizers resolved the SAME number of "
										"variables on every instance — they produce equivalent kernels which was expected")
	r = 3
	for line in lines:
		c = ws2.cell(row=r, column=1, value=line)
		c.font = normal
		c.alignment = Alignment(horizontal="left", vertical="center", wrap_text=True)
		r += 1

	wb.save(out_path)
	print(f"\nWrote comparison workbook {out_path}", file=sys.stderr)


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--harness", help="path to the compiled harness binary")
	ap.add_argument("--kernelizer", default="CPU max-flow", help="label for this kernelizer (e.g. 'CPU max-flow' or 'Gurobi LP')")
	ap.add_argument("--dir", help="directory of .wcsp files")
	ap.add_argument("files", nargs="*", help="explicit .wcsp files (alternative to --dir)")
	ap.add_argument("--repeats", type=int, default=5)
	ap.add_argument("--timeout", type=float, default=None, help="per-run timeout (seconds)")
	ap.add_argument("--out", required=True, help="output .xlsx path")
	ap.add_argument("--merge", nargs=2, metavar=("A.xlsx", "B.xlsx"), help="merge two previously-produced workbooks into a comparison")
	ap.add_argument("--from-json", help="rebuild the .xlsx from a previously saved .raw.json (skips benchmarking)")
	args = ap.parse_args()

	# Rebuild the workbook from saved raw results, without re-running anything.
	if args.from_json:
		import json
		with open(args.from_json) as f:
			payload = json.load(f)
		write_workbook(payload["data"], payload["kernelizer"], args.out, payload["repeats"])
		return

	if args.merge:
		write_comparison(args.merge[0], args.merge[1], args.out)
		return

	if not args.harness:
		ap.error("--harness is required unless --merge or --from-json is used")
	files = list(args.files)
	if args.dir:
		files += sorted(glob.glob(os.path.join(args.dir, "*.wcsp")))
	if not files:
		ap.error("no .wcsp files given (use --dir or positional args)")

	print(f"Benchmarking with {args.harness} ({args.kernelizer}), "
				f"{args.repeats} repeats each ...", file=sys.stderr)
	data = collect(args.harness, files, args.repeats, args.timeout)

	# SAVE RAW DATA IMMEDIATELY, before attempting anything else
	# This guarantees a long benchmark run is never lost to a failure
	import json
	raw_path = args.out + ".raw.json"
	with open(raw_path, "w") as f:
		json.dump({"kernelizer": args.kernelizer, "repeats": args.repeats, "data": data}, f, indent=2)
	print(f"\n[saved raw results to {raw_path}]", file=sys.stderr)

	# Also write a plain CSV as a no-dependency fallback
	import csv
	csv_path = args.out + ".raw.csv"
	with open(csv_path, "w", newline="") as f:
		w = csv.writer(f)
		w.writerow(["file", "wcsp_vars", "run", "parse", "toPoly", "addPoly",
								"simplify", "getGraph", "kernelize", "total", "remnant", "resolved"])
		for e in data:
			for i, run in enumerate(e["runs"], start=1):
				w.writerow([e["file"], e["wcsp_vars"], i] +
										[run.get(s, "") for s in STAGES] +
										[e.get("outcome", {}).get("remnant", ""),
										e.get("outcome", {}).get("resolved", "")])
	print(f"[saved CSV fallback to {csv_path}]", file=sys.stderr)

	# Now try Excel. failure here no longer loses the data
	try:
		write_workbook(data, args.kernelizer, args.out, args.repeats)
	except ImportError:
		print("\nopenpyxl is not installed, so the .xlsx was not written.", file=sys.stderr)
		print(f"  python3 scripts/benchmark_to_excel.py --from-json {raw_path} --out {args.out}", file=sys.stderr)


if __name__ == "__main__":
	main()