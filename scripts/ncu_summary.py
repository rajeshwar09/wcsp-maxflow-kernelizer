#!/usr/bin/env python3
"""Summarise Nsight Compute raw CSV exports of the GPU kernelizer.

usage: ncu_summary.py OUT_TSV INSTANCE CSV [CSV ...]

Each CSV is `ncu --import X.ncu-rep --page raw --csv --metrics ...`.
Appends one row per kernel name (averaged over the sampled launches) to OUT_TSV.
"""
import csv
import os
import sys

M = {
    "time": "gpu__time_duration.sum",
    "grid": "launch__grid_size",
    "block": "launch__block_size",
    "sect": "l1tex__t_sectors_pipe_lsu_mem_global_op_ld.sum",
    "req": "l1tex__t_requests_pipe_lsu_mem_global_op_ld.sum",
    "thr": "smsp__thread_inst_executed_per_inst_executed.ratio",
    "occ": "sm__warps_active.avg.pct_of_peak_sustained_active",
    "sm_act": "sm__cycles_active.avg",
    "sm_el": "sm__cycles_elapsed.avg",
    "dram": "dram__throughput.avg.pct_of_peak_sustained_elapsed",
    "smthr": "sm__throughput.avg.pct_of_peak_sustained_elapsed",
}


def num(s):
    s = (s or "").replace(",", "").strip()
    try:
        return float(s)
    except ValueError:
        return None


TIME_TO_US = {"nsecond": 1e-3, "usecond": 1.0, "msecond": 1e3, "second": 1e6}
PREFIX = {"K": 1e3, "M": 1e6, "G": 1e9, "T": 1e12}


def scale(unit, key):
    """Factor that turns a value in `unit` into the base unit.

    ncu picks a readable unit per column and per file (e.g. msecond in one file,
    usecond in another, Ksector instead of sector), so every value is converted:
    times to microseconds, counts to plain counts.
    """
    unit = (unit or "").strip()
    if key == "time":
        return TIME_TO_US.get(unit, 1.0)
    if len(unit) > 1 and unit[0] in PREFIX and unit[1].islower():
        return PREFIX[unit[0]]
    return 1.0


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    out, inst, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    per = {}
    for f in files:
        with open(f, newline="") as fh:
            rows = list(csv.reader(fh))
        # skip any lines before the header (ncu may print ==PROF== notes)
        h = next(i for i, r in enumerate(rows) if "Kernel Name" in r)
        head = rows[h]
        units = rows[h + 1]   # row h+1 holds the units of each column
        data = rows[h + 2:]
        col = {k: head.index(v) for k, v in M.items() if v in head}
        fac = {k: scale(units[i] if i < len(units) else "", k) for k, i in col.items()}
        kcol = head.index("Kernel Name")
        for r in data:
            if len(r) <= kcol:
                continue
            name = r[kcol].split("(")[0].replace("maxflow::", "").strip()
            vals = {}
            for k, i in col.items():
                v = num(r[i]) if i < len(r) else None
                vals[k] = v * fac[k] if v is not None else None
            per.setdefault(name, []).append(vals)

    new = not os.path.exists(out)
    with open(out, "a") as fo:
        if new:
            fo.write("\t".join(["instance", "kernel", "launches_sampled", "avg_time_us", "grid_blocks",
                                "block_threads", "sectors_per_request", "active_threads_per_warp",
                                "sectors_per_active_thread",
                                "achieved_occupancy_pct", "sm_active_pct", "dram_throughput_pct",
                                "sm_throughput_pct"]) + "\n")
        for name, lst in sorted(per.items()):
            def avg(k):
                v = [x[k] for x in lst if x.get(k) is not None]
                return sum(v) / len(v) if v else None

            def ratio(a, b):
                va, vb = avg(a), avg(b)
                return va / vb if va is not None and vb else None

            t = avg("time")
            spr = ratio("sect", "req")
            sma = ratio("sm_act", "sm_el")
            cells = [inst, name, len(lst),
                     t,  # already in microseconds
                     avg("grid"), avg("block"), spr, avg("thr"),
                     spr / avg("thr") if spr is not None and avg("thr") else None,
                     avg("occ"),
                     100 * sma if sma is not None else None, avg("dram"), avg("smthr")]
            fmt = []
            for c in cells:
                if c is None:
                    fmt.append("NA")
                elif isinstance(c, float):
                    fmt.append(f"{c:.2f}")
                else:
                    fmt.append(str(c))
            fo.write("\t".join(fmt) + "\n")
            print("  " + "  ".join(f"{h}={v}" for h, v in zip(
                ["kernel", "n", "us", "grid", "block", "sect/req", "thr/warp", "sect/thread", "occ%", "sm_active%", "dram%", "sm%"],
                fmt[1:])))


if __name__ == "__main__":
    main()