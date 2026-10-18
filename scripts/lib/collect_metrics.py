#!/usr/bin/env python3
# joins the raw program outputs of one run into a single table, one row per program run
# columns = the run info from the index file + every "[m] name : value" line found in the outputs
# a metric added to the program later becomes a new column here without any change
# usage: python3 scripts/lib/collect_metrics.py <index.tsv> <out.tsv>
#   index.tsv : one line per run, tab separated, first line is the header, column "raw" is the output file

import csv
import sys


# [PARSE] metrics and info lines of one program output
def read_output(path):
    metrics = {}
    info = {}
    try:
        with open(path, errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                if line.startswith("[m] ") and " : " in line:
                    name, value = line[4:].split(" : ", 1)
                    metrics[name.strip()] = value.strip()
                elif line.startswith("[info] ") and " : " in line:
                    name, value = line[7:].split(" : ", 1)
                    info[name.strip()] = value.strip()
    except OSError:
        pass
    return metrics, info


def main():
    if len(sys.argv) != 3:
        print("usage: collect_metrics.py <index.tsv> <out.tsv>")
        return 1

    # [INDEX] run info written by the bench script
    with open(sys.argv[1]) as f:
        runs = list(csv.DictReader(f, delimiter="\t"))
    if not runs:
        return 0
    base_cols = list(runs[0].keys())

    # [JOIN] metrics of every run, metric columns in the order they first appear
    metric_cols = []
    rows = []
    for run in runs:
        metrics, info = read_output(run["raw"])
        for name in metrics:
            if name not in metric_cols:
                metric_cols.append(name)
        row = dict(run)
        row["final_optimum"] = info.get("final_optimum", "")
        row.update(metrics)
        rows.append(row)

    # [WRITE] missing values are left empty, for example GPU memory in a cpu run
    cols = base_cols + ["final_optimum"] + metric_cols
    with open(sys.argv[2], "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, delimiter="\t", extrasaction="ignore")
        w.writeheader()
        for row in rows:
            w.writerow({c: row.get(c, "") for c in cols})
    return 0


if __name__ == "__main__":
    sys.exit(main())
