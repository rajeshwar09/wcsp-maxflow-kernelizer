#!/usr/bin/env python3
"""
usage: nsys_summary.py SQLITE|- INSTANCE CCG PLAIN_KERN_S PLAIN_MAXFLOW_S PLAIN_RES PROF_RES SUMMARY_TSV KERNELS_TSV

With "-" instead of a SQLITE file (nsys produced no trace), only the plain run's numbers
are written, with resolved_match = no_trace and NA in every trace column.

Reads the GPU activity tables of the exported trace and appends
  one row per kernel name to KERNELS_TSV
  one row for the instance to SUMMARY_TSV
A header is written when a file does not exist yet.
"""
import os
import sqlite3
import sys


KER_HEADER = ["instance", "kernel", "step", "launches", "total_ms", "avg_us", "max_us",
              "pct_kernel_time", "pct_gpu_window"]
SUM_HEADER = ["instance", "ccg_vertices", "plain_kern_s", "plain_maxflow_s", "plain_resolved",
              "prof_resolved", "resolved_match", "gpu_window_s", "gpu_busy_s", "gpu_idle_pct",
              "kernel_s", "kernel_launches", "pct_global_relabel", "pct_push_relabel",
              "pct_remove_invalid", "pct_build_worklist", "pct_other", "top_kernel",
              "htod_ms", "dtoh_ms", "dtoh_count", "sync_api_s", "sync_calls",
              "memcpy_api_s", "launch_api_s"]


def table_exists(db, name):
    q = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?"
    return db.execute(q, (name,)).fetchone() is not None


def rows(db, sql, table):
    if not table_exists(db, table):
        return []
    return db.execute(sql).fetchall()


def append(path, header, line):
    new = not os.path.exists(path)
    with open(path, "a") as f:
        if new:
            f.write("\t".join(header) + "\n")
        f.write("\t".join(str(x) for x in line) + "\n")


def union_ns(intervals):
    # total length covered by a set of [start, end) intervals, overlaps counted once
    total = 0
    cur_s = cur_e = None
    for s, e in sorted(intervals):
        if cur_e is None or s > cur_e:
            if cur_e is not None:
                total += cur_e - cur_s
            cur_s, cur_e = s, e
        else:
            cur_e = max(cur_e, e)
    if cur_e is not None:
        total += cur_e - cur_s
    return total


def group(name):
    # which algorithm step a kernel belongs to
    if "bfs" in name:
        return "global_relabel"
    if "push_relabel" in name:
        return "push_relabel"
    if "remove_invalid" in name:
        return "remove_invalid"
    if "worklist" in name:
        return "build_worklist"
    return "other"


def main():
    if len(sys.argv) != 10:
        print(__doc__)
        sys.exit(2)
    sqlite_path, inst, ccg, plain_kern, plain_mf, plain_res, prof_res, sum_tsv, ker_tsv = sys.argv[1:10]

    if sqlite_path == "-":
        # nsys could not produce a trace for this instance: keep the plain run's numbers,
        # mark every trace column NA, and write no kernel rows
        row = [inst, ccg, plain_kern, plain_mf, plain_res, prof_res, "no_trace"]
        row += ["NA"] * (len(SUM_HEADER) - len(row))
        append(sum_tsv, SUM_HEADER, row)
        print(f"  no trace: plain run kept (time {plain_kern} s, resolved {plain_res})")
        return

    db = sqlite3.connect(sqlite_path)

    kern = rows(db, """SELECT s.value, k.start, k.end
                       FROM CUPTI_ACTIVITY_KIND_KERNEL k JOIN StringIds s ON k.shortName = s.id""",
                "CUPTI_ACTIVITY_KIND_KERNEL")
    mcpy = rows(db, "SELECT copyKind, bytes, start, end FROM CUPTI_ACTIVITY_KIND_MEMCPY",
                "CUPTI_ACTIVITY_KIND_MEMCPY")
    mset = rows(db, "SELECT start, end FROM CUPTI_ACTIVITY_KIND_MEMSET",
                "CUPTI_ACTIVITY_KIND_MEMSET")
    api = rows(db, """SELECT s.value, r.end - r.start
                      FROM CUPTI_ACTIVITY_KIND_RUNTIME r JOIN StringIds s ON r.nameId = s.id""",
               "CUPTI_ACTIVITY_KIND_RUNTIME")

    if not kern:
        print(f"  {inst}: no kernels found in trace")
        sys.exit(1)

    # per kernel totals
    per = {}
    for name, s, e in kern:
        d = e - s
        t = per.setdefault(name, [0, 0, 0])
        t[0] += d
        t[1] += 1
        t[2] = max(t[2], d)
    kern_ns = sum(t[0] for t in per.values())
    launches = sum(t[1] for t in per.values())

    # GPU window: first to last GPU activity (kernels, copies, memsets)
    iv = [(s, e) for _, s, e in kern] + [(s, e) for _, _, s, e in mcpy] + list(mset)
    window_ns = max(e for _, e in iv) - min(s for s, _ in iv)
    busy_ns = union_ns(iv)
    idle_pct = 100.0 * (window_ns - busy_ns) / window_ns if window_ns else 0.0

    # copies: CUPTI copyKind 1 = host to device, 2 = device to host
    htod_ns = sum(e - s for k, _, s, e in mcpy if k == 1)
    dtoh_ns = sum(e - s for k, _, s, e in mcpy if k == 2)
    dtoh_n = sum(1 for k, _, _, _ in mcpy if k == 2)

    # CPU side CUDA calls
    sync_ns = sum(d for n, d in api if n.startswith("cudaDeviceSynchronize"))
    sync_n = sum(1 for n, _ in api if n.startswith("cudaDeviceSynchronize"))
    memcpy_api_ns = sum(d for n, d in api if n.startswith("cudaMemcpy"))
    launch_api_ns = sum(d for n, d in api if n.startswith("cudaLaunchKernel"))

    # share of kernel time by algorithm step
    share = {"global_relabel": 0, "push_relabel": 0, "remove_invalid": 0, "build_worklist": 0, "other": 0}
    for name, t in per.items():
        share[group(name)] += t[0]
    pct = {k: 100.0 * v / kern_ns for k, v in share.items()}
    top = max(per, key=lambda n: per[n][0])

    match = "yes" if plain_res == prof_res and plain_res != "NA" else "NO"

    for name in sorted(per, key=lambda n: -per[n][0]):
        t = per[name]
        append(ker_tsv, KER_HEADER,
               [inst, name, group(name), t[1], f"{t[0] / 1e6:.3f}", f"{t[0] / t[1] / 1e3:.2f}",
                f"{t[2] / 1e3:.2f}", f"{100.0 * t[0] / kern_ns:.1f}", f"{100.0 * t[0] / window_ns:.1f}"])

    append(sum_tsv, SUM_HEADER,
           [inst, ccg, plain_kern, plain_mf, plain_res, prof_res, match,
            f"{window_ns / 1e9:.3f}", f"{busy_ns / 1e9:.3f}", f"{idle_pct:.1f}",
            f"{kern_ns / 1e9:.3f}", launches,
            f"{pct['global_relabel']:.1f}", f"{pct['push_relabel']:.1f}",
            f"{pct['remove_invalid']:.1f}", f"{pct['build_worklist']:.1f}", f"{pct['other']:.1f}",
            top, f"{htod_ns / 1e6:.3f}", f"{dtoh_ns / 1e6:.3f}", dtoh_n,
            f"{sync_ns / 1e9:.3f}", sync_n, f"{memcpy_api_ns / 1e9:.3f}", f"{launch_api_ns / 1e9:.3f}"])

    print(f"  window {window_ns / 1e9:.3f}s  idle {idle_pct:.1f}%  relabel {pct['global_relabel']:.1f}%  "
          f"push {pct['push_relabel']:.1f}%  remove {pct['remove_invalid']:.1f}%  "
          f"worklist {pct['build_worklist']:.1f}%  resolved_match={match}")


if __name__ == "__main__":
    main()