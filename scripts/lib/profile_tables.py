#!/usr/bin/env python3
# profile_tables.py
# turns the nsys traces and ncu reports of the GPU kernelizer into tables, used by scripts/run_profile.sh
# usage:
#   python3 scripts/lib/profile_tables.py metrics
#       prints the ncu metric list, comma separated, given to ncu by the script
#   python3 scripts/lib/profile_tables.py launches <trace.sqlite>
#       prints "kernel launches" for every kernel in the trace, the script uses it to pick the ncu sample
#   python3 scripts/lib/profile_tables.py instance <set> <name> <plain.out> <nsys.out|-> <trace.sqlite|-> <summary.tsv> <kernels.tsv> [ncu.csv ...]
#       adds one row for the instance to summary.tsv and one row per kernel to kernels.tsv
#       "-" for the trace = nsys gave no trace, then only the plain run numbers are written
#   python3 scripts/lib/profile_tables.py compare <old.tsv> <new.tsv> <out.tsv> [bench.tsv ...]
#       before and after table of two profile runs, bench.tsv files with "-k lp" runs add the Gurobi time

import csv
import os
import sqlite3
import statistics
import sys

from collect_metrics import read_output

SUM_COLS = ["set", "instance", "status", "resolved", "prof_resolved", "resolved_match", "ccg_vertices",
            "kern_s", "maxflow_s", "gpu_solve_s", "gpu_passes", "gpu_bfs_levels", "gpu_mb",
            "gpu_window_s", "gpu_busy_s", "gpu_idle_pct", "kernel_s", "kernel_launches",
            "pct_bfs", "pct_push", "pct_remove", "pct_worklist", "pct_other", "top_kernel",
            "htod_ms", "dtoh_ms", "htod_count", "dtoh_count", "sync_api_s", "sync_calls",
            "memcpy_api_s", "launch_api_s", "ncu_kernels"]

KER_COLS = ["set", "instance", "kernel", "step", "launches", "total_ms", "avg_us", "min_us", "max_us",
            "pct_kernel_time", "pct_gpu_window",
            "ncu_launches", "ncu_avg_us", "grid_blocks", "block_threads", "regs_per_thread",
            "static_smem_bytes", "sectors_per_request", "active_threads_per_warp",
            "sectors_per_active_thread", "theoretical_occupancy_pct", "achieved_occupancy_pct",
            "sm_active_pct", "dram_throughput_pct", "sm_throughput_pct", "l1_hit_pct", "l2_hit_pct"]

# [NCU] metrics read from the ncu reports, the same list is given to ncu by run_profile.sh
NCU_METRICS = {
    "time": "gpu__time_duration.sum",
    "grid": "launch__grid_size",
    "block": "launch__block_size",
    "regs": "launch__registers_per_thread",
    "smem": "launch__shared_mem_per_block_static",
    "sect": "l1tex__t_sectors_pipe_lsu_mem_global_op_ld.sum",
    "req": "l1tex__t_requests_pipe_lsu_mem_global_op_ld.sum",
    "thr": "smsp__thread_inst_executed_per_inst_executed.ratio",
    "theo": "sm__maximum_warps_per_active_cycle_pct",
    "occ": "sm__warps_active.avg.pct_of_peak_sustained_active",
    "sm_act": "sm__cycles_active.avg",
    "sm_el": "sm__cycles_elapsed.avg",
    "dram": "dram__throughput.avg.pct_of_peak_sustained_elapsed",
    "smthr": "sm__throughput.avg.pct_of_peak_sustained_elapsed",
    "l1": "l1tex__t_sector_hit_rate.pct",
    "l2": "lts__t_sector_hit_rate.pct",
}


def fmt(v, d=3):
    if v is None or v == "":
        return "NA"
    if isinstance(v, float):
        return f"{v:.{d}f}"
    return str(v)


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


# [TSV] add one row, the header is written when the file is new
def append(path, cols, row):
    new = not os.path.exists(path)
    with open(path, "a") as f:
        if new:
            f.write("\t".join(cols) + "\n")
        f.write("\t".join(fmt(row.get(c)) for c in cols) + "\n")


# [STEP] which step of the solver a kernel belongs to
def step_of(name):
    if "bfs" in name:
        return "bfs"
    if "push_relabel" in name:
        return "push"
    if "remove_invalid" in name:
        return "remove"
    if "worklist" in name:
        return "worklist"
    return "other"


# [TRACE] total length of a set of [start, end) intervals, overlaps counted once
def union_ns(intervals):
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


# [TRACE] read the GPU activity tables of an exported nsys trace
def read_trace(path):
    db = sqlite3.connect(path)

    def rows(table, sql):
        q = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?"
        if db.execute(q, (table,)).fetchone() is None:
            return []
        return db.execute(sql).fetchall()

    kern = rows("CUPTI_ACTIVITY_KIND_KERNEL",
                "SELECT s.value, k.start, k.end FROM CUPTI_ACTIVITY_KIND_KERNEL k JOIN StringIds s ON k.shortName = s.id")
    mcpy = rows("CUPTI_ACTIVITY_KIND_MEMCPY", "SELECT copyKind, start, end FROM CUPTI_ACTIVITY_KIND_MEMCPY")
    mset = rows("CUPTI_ACTIVITY_KIND_MEMSET", "SELECT start, end FROM CUPTI_ACTIVITY_KIND_MEMSET")
    api = rows("CUPTI_ACTIVITY_KIND_RUNTIME",
               "SELECT s.value, r.end - r.start FROM CUPTI_ACTIVITY_KIND_RUNTIME r JOIN StringIds s ON r.nameId = s.id")
    db.close()
    return kern, mcpy, mset, api


# [TRACE] launches, total, min and max time of every kernel
def per_kernel(kern):
    per = {}
    for name, s, e in kern:
        d = e - s
        if name not in per:
            per[name] = {"n": 0, "total": 0, "min": d, "max": d}
        p = per[name]
        p["n"] += 1
        p["total"] += d
        p["min"] = min(p["min"], d)
        p["max"] = max(p["max"], d)
    return per


# [NCU] unit factor, ncu picks the unit per file (usecond or msecond, sector or Ksector)
def unit_factor(unit, key):
    unit = (unit or "").strip()
    if key == "time":
        return {"nsecond": 1e-3, "usecond": 1.0, "msecond": 1e3, "second": 1e6}.get(unit, 1.0)
    prefix = {"K": 1e3, "M": 1e6, "G": 1e9, "T": 1e12}
    if len(unit) > 1 and unit[0] in prefix and unit[1].islower():
        return prefix[unit[0]]
    return 1.0


# [NCU] metric values of every sampled launch, grouped by kernel name
def read_ncu(files):
    per = {}
    for path in files:
        with open(path, newline="") as f:
            rows = list(csv.reader(f))
        heads = [i for i, r in enumerate(rows) if "Kernel Name" in r]
        if not heads:
            continue
        h = heads[0]
        head, units, data = rows[h], rows[h + 1], rows[h + 2:]
        col = {k: head.index(m) for k, m in NCU_METRICS.items() if m in head}
        fac = {k: unit_factor(units[i] if i < len(units) else "", k) for k, i in col.items()}
        kcol = head.index("Kernel Name")
        for r in data:
            if len(r) <= kcol:
                continue
            name = r[kcol].split("(")[0].split("::")[-1].strip()
            vals = {}
            for k, i in col.items():
                v = num(r[i].replace(",", "")) if i < len(r) else None
                vals[k] = v * fac[k] if v is not None else None
            per.setdefault(name, []).append(vals)
    return per


# [NCU] average of the sampled launches of one kernel, as table columns
def ncu_columns(lst):
    def avg(k):
        v = [x[k] for x in lst if x.get(k) is not None]
        return sum(v) / len(v) if v else None

    def ratio(a, b):
        va, vb = avg(a), avg(b)
        return va / vb if va is not None and vb else None

    spr = ratio("sect", "req")
    thr = avg("thr")
    sma = ratio("sm_act", "sm_el")
    return {
        "ncu_launches": len(lst), "ncu_avg_us": avg("time"), "grid_blocks": avg("grid"),
        "block_threads": avg("block"), "regs_per_thread": avg("regs"), "static_smem_bytes": avg("smem"),
        "sectors_per_request": spr, "active_threads_per_warp": thr,
        "sectors_per_active_thread": spr / thr if spr is not None and thr else None,
        "theoretical_occupancy_pct": avg("theo"), "achieved_occupancy_pct": avg("occ"),
        "sm_active_pct": 100 * sma if sma is not None else None,
        "dram_throughput_pct": avg("dram"), "sm_throughput_pct": avg("smthr"),
        "l1_hit_pct": avg("l1"), "l2_hit_pct": avg("l2"),
    }


def cmd_launches(path):
    for name, p in sorted(per_kernel(read_trace(path)[0]).items()):
        print(name, p["n"])
    return 0


def cmd_instance(set_name, name, plain_out, prof_out, trace, sum_tsv, ker_tsv, ncu_files):
    # [PLAIN] numbers of the run without profiler, these are the true times
    m, info = read_output(plain_out)
    prof_res = read_output(prof_out)[0].get("kern.resolved_total") if prof_out != "-" else None
    row = {
        "set": set_name, "instance": name, "status": info.get("status", "no_status"),
        "resolved": m.get("kern.resolved_total"), "prof_resolved": prof_res,
        "ccg_vertices": m.get("graph.vertices_before"), "kern_s": num(m.get("time.kernelize")),
        "maxflow_s": num(m.get("time.kern.maxflow")), "gpu_solve_s": num(m.get("time.gpu.solve")),
        "gpu_passes": m.get("gpu.passes"), "gpu_bfs_levels": m.get("gpu.bfs_levels"),
        "gpu_mb": num(m.get("mem.gpu_solver_mb")), "ncu_kernels": 0,
    }
    if trace == "-":
        row["resolved_match"] = "no_trace"
        append(sum_tsv, SUM_COLS, row)
        print(f"no_trace resolved {row['resolved']}")
        return 0

    kern, mcpy, mset, api = read_trace(trace)
    if not kern:
        row["resolved_match"] = "no_kernels"
        append(sum_tsv, SUM_COLS, row)
        print("no_kernels")
        return 0
    per = per_kernel(kern)
    kern_ns = sum(p["total"] for p in per.values())

    # [TRACE] GPU window = first to last GPU activity, busy = time with a kernel, copy or memset running
    iv = [(s, e) for _, s, e in kern] + [(s, e) for _, s, e in mcpy] + list(mset)
    window_ns = max(e for _, e in iv) - min(s for s, _ in iv)
    busy_ns = union_ns(iv)

    # [TRACE] share of kernel time per solver step
    share = {"bfs": 0, "push": 0, "remove": 0, "worklist": 0, "other": 0}
    for k, p in per.items():
        share[step_of(k)] += p["total"]

    # [TRACE] copies (CUPTI copyKind 1 = host to device, 2 = device to host) and CPU side CUDA calls
    row.update({
        "resolved_match": "yes" if row["resolved"] is not None and row["resolved"] == prof_res else "NO",
        "gpu_window_s": window_ns / 1e9, "gpu_busy_s": busy_ns / 1e9,
        "gpu_idle_pct": 100.0 * (window_ns - busy_ns) / window_ns if window_ns else 0.0,
        "kernel_s": kern_ns / 1e9, "kernel_launches": sum(p["n"] for p in per.values()),
        "top_kernel": max(per, key=lambda k: per[k]["total"]),
        "htod_ms": sum(e - s for c, s, e in mcpy if c == 1) / 1e6,
        "dtoh_ms": sum(e - s for c, s, e in mcpy if c == 2) / 1e6,
        "htod_count": sum(1 for c, _, _ in mcpy if c == 1),
        "dtoh_count": sum(1 for c, _, _ in mcpy if c == 2),
        "sync_api_s": sum(d for n, d in api if "Synchronize" in n) / 1e9,
        "sync_calls": sum(1 for n, _ in api if "Synchronize" in n),
        "memcpy_api_s": sum(d for n, d in api if n.startswith("cudaMemcpy")) / 1e9,
        "launch_api_s": sum(d for n, d in api if n.startswith("cudaLaunchKernel")) / 1e9,
    })
    for k, v in share.items():
        row["pct_" + k] = 100.0 * v / kern_ns

    # [KERNELS] one row per kernel, with the ncu columns when ncu was run on it
    ncu = read_ncu(ncu_files)
    row["ncu_kernels"] = len(ncu)
    for k in sorted(per, key=lambda k: -per[k]["total"]):
        p = per[k]
        kr = {
            "set": set_name, "instance": name, "kernel": k, "step": step_of(k), "launches": p["n"],
            "total_ms": p["total"] / 1e6, "avg_us": p["total"] / p["n"] / 1e3,
            "min_us": p["min"] / 1e3, "max_us": p["max"] / 1e3,
            "pct_kernel_time": 100.0 * p["total"] / kern_ns, "pct_gpu_window": 100.0 * p["total"] / window_ns,
        }
        if k in ncu:
            kr.update(ncu_columns(ncu[k]))
        append(ker_tsv, KER_COLS, kr)
    append(sum_tsv, SUM_COLS, row)

    print(f"{row['resolved_match']} idle {row['gpu_idle_pct']:.1f} bfs {row['pct_bfs']:.1f} "
          f"push {row['pct_push']:.1f} remove {row['pct_remove']:.1f} worklist {row['pct_worklist']:.1f} "
          f"ncu {row['ncu_kernels']}")
    return 0


def cmd_compare(old_tsv, new_tsv, out_tsv, bench_files):
    def load(path):
        with open(path, newline="") as f:
            return {r["instance"]: r for r in csv.DictReader(f, delimiter="\t")}

    old, new = load(old_tsv), load(new_tsv)

    # [GUROBI] mean lp kernelization time per instance, a later file wins over an earlier one
    gurobi = {}
    for path in bench_files:
        times = {}
        with open(path, newline="") as f:
            for r in csv.DictReader(f, delimiter="\t"):
                if r.get("kern") == "lp" and r.get("status") == "ok" and num(r.get("time.kernelize")) is not None:
                    times.setdefault(r["instance"], []).append(float(r["time.kernelize"]))
        for k, v in times.items():
            gurobi[k] = statistics.mean(v)

    cols = ["instance", "ccg_vertices", "old_s", "new_s", "speedup", "resolved_old", "resolved_new",
            "resolved_same", "gurobi_s", "vs_gurobi_old", "vs_gurobi_new", "pct_bfs_old", "pct_bfs_new",
            "pct_pushrem_old", "pct_pushrem_new", "kernel_s_old", "kernel_s_new"]

    def pushrem(r):
        a, b = num(r.get("pct_push")), num(r.get("pct_remove"))
        return a + b if a is not None and b is not None else None

    rows, bad = [], []
    for k, o in old.items():
        if k not in new:
            print(f"only in old run: {k}")
            continue
        n = new[k]
        to, tn = num(o["kern_s"]), num(n["kern_s"])
        same = o["resolved"] == n["resolved"] and o["resolved_match"] in ("yes", "no_trace") \
            and n["resolved_match"] in ("yes", "no_trace")
        if not same:
            bad.append(f"{k}: old {o['resolved']} new {n['resolved']} (profiled {n['prof_resolved']})")
        g = gurobi.get(k)
        rows.append({
            "instance": k, "ccg_vertices": o["ccg_vertices"], "old_s": to, "new_s": tn,
            "speedup": to / tn if to and tn else None,
            "resolved_old": o["resolved"], "resolved_new": n["resolved"], "resolved_same": "yes" if same else "NO",
            "gurobi_s": g, "vs_gurobi_old": g / to if g and to else None, "vs_gurobi_new": g / tn if g and tn else None,
            "pct_bfs_old": num(o["pct_bfs"]), "pct_bfs_new": num(n["pct_bfs"]),
            "pct_pushrem_old": pushrem(o), "pct_pushrem_new": pushrem(n),
            "kernel_s_old": num(o["kernel_s"]), "kernel_s_new": num(n["kernel_s"]),
        })
    for k in new:
        if k not in old:
            print(f"only in new run: {k}")

    if os.path.exists(out_tsv):
        os.remove(out_tsv)
    for r in rows:
        append(out_tsv, cols, r)

    # [PRINT] short table for the log
    print(f"{'instance':<44}{'old_s':>9}{'new_s':>9}{'speedup':>9}{'gurobi_s':>10}{'vsG_old':>9}{'vsG_new':>9}  resolved")
    for r in rows:
        print(f"{r['instance'][:43]:<44}{fmt(r['old_s']):>9}{fmt(r['new_s']):>9}{fmt(r['speedup'], 2):>9}"
              f"{fmt(r['gurobi_s']):>10}{fmt(r['vs_gurobi_old'], 2):>9}{fmt(r['vs_gurobi_new'], 2):>9}  {r['resolved_same']}")
    t_old = sum(r["old_s"] for r in rows if r["old_s"])
    t_new = sum(r["new_s"] for r in rows if r["new_s"])
    print(f"total  old {t_old:.3f} s  new {t_new:.3f} s  speedup {t_old / t_new if t_new else 0:.2f}x")
    with_g = [r for r in rows if r["gurobi_s"]]
    if with_g:
        win_o = sum(1 for r in with_g if r["vs_gurobi_old"] and r["vs_gurobi_old"] > 1)
        win_n = sum(1 for r in with_g if r["vs_gurobi_new"] and r["vs_gurobi_new"] > 1)
        print(f"GPU faster than Gurobi  old {win_o}  new {win_n}  of {len(with_g)}")
    if bad:
        print("resolved count changed on:")
        for x in bad:
            print("  " + x)
        return 1
    print("resolved counts same on all instances")
    return 0


def main():
    a = sys.argv[1:]
    if a == ["metrics"]:
        print(",".join(NCU_METRICS.values()))
        return 0
    if len(a) == 2 and a[0] == "launches":
        return cmd_launches(a[1])
    if len(a) >= 8 and a[0] == "instance":
        return cmd_instance(a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8:])
    if len(a) >= 4 and a[0] == "compare":
        return cmd_compare(a[1], a[2], a[3], a[4:])
    print("usage: profile_tables.py metrics|launches|instance|compare ... (see the top of this file)")
    return 2


if __name__ == "__main__":
    sys.exit(main())
