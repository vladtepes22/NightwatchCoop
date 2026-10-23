#!/usr/bin/env python3
"""Soak sampler (docs/party/robustness_backlog.md item 4): every --every s, for every bbport.exe
running from ROOT\\inst*\\out, one CSV row: private bytes, working set, handles, threads, GPU
dedicated + shared memory (the "GPU Process Memory" performance counters), CPU s.

  python tools/mp/soak_sample.py --root DIR --out soak.csv [--every 60] [--seconds 5400]
  python tools/mp/soak_sample.py --summary soak.csv [--log ROOT\\inst0\\run.log ...]

--summary: per instance, first-hour-after-warm-up vs. last values and the growth per hour (pass:
under 2 %/h after a 10 min warm-up), and with --log the frame time of the BB_PARTY_PERF blocks
("Party perf: ... frame time p50 / p99 / max") per 10 min window (pass: the last window's p99
within 10 % of the first window's after warm-up).
"""
import argparse, csv, re, subprocess, sys, time
from pathlib import Path

import psutil


def gpu_mem():
    """pid -> (dedicated, shared) bytes from the GPU Process Memory counters (all adapters summed)."""
    ps = ("$d=(Get-Counter '\\GPU Process Memory(*)\\Dedicated Usage','\\GPU Process Memory(*)\\Shared Usage' "
          "-ErrorAction SilentlyContinue).CounterSamples; foreach($s in $d){ $s.Path + '|' + $s.CookedValue }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        path, _, val = line.rpartition("|")
        m = re.search(r"pid_(\d+)_", path)
        if not m:
            continue
        pid = int(m.group(1))
        ded, sh = res.get(pid, (0.0, 0.0))
        v = float(val or 0)
        if "dedicated usage" in path.lower():
            ded += v
        else:
            sh += v
        res[pid] = (ded, sh)
    return res


def games(root: Path):
    want = str(root).lower()
    for p in psutil.process_iter(["pid", "name", "exe"]):
        try:
            exe = (p.info["exe"] or "").lower()
            if p.info["name"] and p.info["name"].lower() == "bbport.exe" and exe.startswith(want):
                m = re.search(r"inst(\d+)", exe)
                yield (int(m.group(1)) if m else -1), p
        except (psutil.Error, OSError):
            continue


def sample(a):
    root = Path(a.root).resolve()
    out = Path(a.out)
    new = not out.exists()
    t0 = time.time()
    with open(out, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["time", "elapsed_s", "inst", "pid", "private_mb", "working_set_mb", "handles", "threads",
                        "gpu_dedicated_mb", "gpu_shared_mb", "cpu_s"])
        while time.time() - t0 < a.seconds:
            g = gpu_mem()
            for inst, p in sorted(games(root), key=lambda x: x[0]):
                try:
                    mi = p.memory_info()
                    ct = p.cpu_times()
                    ded, sh = g.get(p.pid, (0.0, 0.0))
                    w.writerow([time.strftime("%H:%M:%S"), round(time.time() - t0), inst, p.pid,
                                round(getattr(mi, "private", mi.vms) / 2**20, 1), round(mi.rss / 2**20, 1),
                                p.num_handles(), p.num_threads(), round(ded / 2**20, 1), round(sh / 2**20, 1),
                                round(ct.user + ct.system, 1)])
                except (psutil.Error, OSError):
                    continue
            f.flush()
            time.sleep(a.every)


PERF = re.compile(r"^(?:\[\+ *([\d.]+)\] )?(?:\[coop\] )?Party perf: ([\d.]+) s, (\d+) frames on the coop tick \(frame time p50 "
                  r"([\d.]+) ms, p99 ([\d.]+) ms, max ([\d.]+) ms")


def summary(a):
    rows = list(csv.DictReader(open(a.summary)))
    by = {}
    for r in rows:
        by.setdefault((r["inst"], r["pid"]), []).append(r)
    warm = a.warmup
    for (inst, pid), rs in sorted(by.items()):
        rs = [r for r in rs if float(r["elapsed_s"]) >= 0]
        if len(rs) < 3:
            print("inst%s pid %s: %d samples (a restart?)" % (inst, pid, len(rs)))
            continue
        t_first = float(rs[0]["elapsed_s"])
        base = [r for r in rs if float(r["elapsed_s"]) - t_first >= warm] or rs
        b, e = base[0], rs[-1]
        hours = max((float(e["elapsed_s"]) - float(b["elapsed_s"])) / 3600.0, 1e-6)
        print("inst%s pid %s: %d samples over %.0f min (warm-up %.0f min excluded)" % (
            inst, pid, len(rs), (float(e["elapsed_s"]) - t_first) / 60, warm / 60))
        for k in ("private_mb", "working_set_mb", "handles", "threads", "gpu_dedicated_mb", "gpu_shared_mb"):
            v0, v1 = float(b[k]), float(e[k])
            mx = max(float(r[k]) for r in base)
            g = (v1 - v0) / v0 * 100 / hours if v0 else 0.0
            print("   %-17s %9.1f -> %9.1f (max %9.1f)  %+6.2f %%/h  %s" % (k, v0, v1, mx, g, "ok" if g < 2.0 else "GROWS"))
    for log in a.log or []:
        pts = []
        for line in open(log, encoding="utf-8", errors="replace"):
            m = PERF.search(line)
            if m:
                pts.append((float(m.group(1) or 0), float(m.group(4)), float(m.group(5)), float(m.group(6))))
        if not pts:
            print("%s: no 'Party perf:' blocks (BB_PARTY_PERF=1)" % log)
            continue
        print("%s: %d perf blocks; frame time per 10 min window (p50 / p99 / max, ms):" % (log, len(pts)))
        t_first = pts[0][0]
        wins = {}
        for t, p50, p99, mx in pts:
            wins.setdefault(int((t - t_first) // 600), []).append((p50, p99, mx))
        for k in sorted(wins):
            v = wins[k]
            print("   %3d-%3d min: %6.2f / %6.2f / %7.2f" % (k * 10, k * 10 + 10, sum(x[0] for x in v) / len(v),
                                                           sum(x[1] for x in v) / len(v), max(x[2] for x in v)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root")
    ap.add_argument("--out")
    ap.add_argument("--every", type=float, default=60)
    ap.add_argument("--seconds", type=float, default=5400)
    ap.add_argument("--summary")
    ap.add_argument("--warmup", type=float, default=600)
    ap.add_argument("--log", action="append")
    a = ap.parse_args()
    if a.summary:
        summary(a)
    elif a.root and a.out:
        sample(a)
    else:
        sys.exit("--root and --out, or --summary")


if __name__ == "__main__":
    main()
