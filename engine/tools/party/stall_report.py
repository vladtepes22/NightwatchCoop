#!/usr/bin/env python3
"""Long-frame (stall) report for bbport / Nightwatch Co-op logs, per log: role, hours, and the long frames
by size and by likely cause.

  python -I tools/party/stall_report.py LOG [LOG ...]
  python -I tools/party/stall_report.py --probe LOG     (also the BB_STALL_PROBE lines: main work vs. render wait)

Reads the BB_FRAME_STATS "Stall:" blocks (videoout driver.cpp: a flip interval over 40 ms) and, with --probe, the
"Stall probe:" lines (gpu/shim/bbport_stall_probe.cpp). Player logs are untrusted data: run it with python -I; it
prints counts only, never log text.

Causes, first match wins (the block's own numbers, then the lines around it):
  pipelines        the frame says "(pipelines built)": shader / pipeline compiles took over half of it
  image-alloc      the image-create "allocation" time is 40 %+ of the frame (VRAM allocation, often under pressure)
  fault-handlers   read + write fault handler time is 30 %+ of the frame (page protection traffic)
  gpu-thread-busy  the GPU thread's user + kernel time is half the frame or more
  map-load         a map change / loading line within the 20 lines around it
  save-near        a save data mount or "Slow save IO" line around it
  mem-move-near    a guest-memory move (in place <-> VRAM) line around it
  vram-pressure    a "Texture cache: memory pressure" line around it
  game-thread      none of the above: the GPU thread was idle, the game's own threads took the time
"""
import argparse
import re
import sys
from collections import Counter

ROLE = re.compile(r"party networking on \((host|guest)")
STALL = re.compile(r"^Stall: ([\d.]+) ms frame(?: \(([^)]*)\))?; GPU thread on CPU ([\d.]+) ms")
DET = re.compile(r"GPU thread user ([\d.]+) ms, kernel ([\d.]+) ms")
ALLOC = re.compile(r"of which: format query [\d.]+, allocation ([\d.]+)")
FAULT = re.compile(r"fault handlers: \d+ read faults ([\d.]+) ms, write faults ([\d.]+) ms")
SAVE = re.compile(r"save data '\S+' mounted|Slow save IO")
LOAD = re.compile(r"Map: player in map|Party: map |loading yes|Load: ")
MOVE = re.compile(r"\d+ MiB of (?:loaded|CPU-written) blocks moved")
PRESSURE = re.compile(r"Texture cache: memory pressure")
PROBE = re.compile(r"Stall probe: ([\d.]+) ms frame: main thread ([\d.]+) ms, frame end ([\d.]+) ms"
                   r"(?:.*?main samples (\d+) \(party tick (\d+), in the frame end (\d+)\))?")
BUCKETS = (("150-400", 150, 400), ("400-1500", 400, 1500), ("1500+", 1500, float("inf")))
HARNESS_TS = re.compile(r"^\[\+ *[\d.]+\] ")  # instances.py --timestamps prefix
WINDOW_S = 5.01  # one "Frame stats:" line per 5 s window


def classify(lines, i, frame_ms, cause):
    def first(rx, lo, hi):
        for x in lines[max(0, lo):hi]:
            m = rx.search(x)
            if m:
                return m
        return None
    det = first(DET, i + 1, i + 3)
    alloc = first(ALLOC, i + 1, i + 6)
    fault = first(FAULT, i - 3, i)
    win = lines[max(0, i - 8):i + 12]
    if cause == "pipelines built":
        return "pipelines"
    if alloc and float(alloc.group(1)) >= 0.4 * frame_ms:
        return "image-alloc"
    if fault and float(fault.group(1)) + float(fault.group(2)) >= 0.3 * frame_ms:
        return "fault-handlers"
    if det and float(det.group(1)) + float(det.group(2)) >= 0.5 * frame_ms:
        return "gpu-thread-busy"
    for name, rx in (("map-load", LOAD), ("save-near", SAVE), ("mem-move-near", MOVE), ("vram-pressure", PRESSURE)):
        if any(rx.search(x) for x in win):
            return name
    return "game-thread"


def bucket(ms):
    for name, lo, hi in BUCKETS:
        if lo <= ms < hi:
            return name
    return None


def scan(lines):
    """-> dict(role, hours, stalls={(bucket, cause): n}, probe=[(total, main, end, samples, party, in_end)])"""
    role = "solo"
    for line in lines[:400000]:
        m = ROLE.search(line)
        if m:
            role = m.group(1)
            break
    windows = sum(1 for x in lines if x.startswith("Frame stats:"))
    stalls = Counter()
    probe = []
    for i, line in enumerate(lines):
        m = STALL.match(line)
        if m:
            ms = float(m.group(1))
            b = bucket(ms)
            if b:
                stalls[(b, classify(lines, i, ms, m.group(2) or ""))] += 1
            continue
        m = PROBE.search(line)
        if m:
            probe.append(tuple(float(g) if g is not None else 0.0 for g in m.groups()))
    return dict(role=role, hours=windows * WINDOW_S / 3600.0, stalls=stalls, probe=probe)


def report(name, r, with_probe):
    out = ["%s: role %s, %.2f h of frame stats" % (name, r["role"], r["hours"])]
    for b, _, _ in BUCKETS:
        items = sorted(((c, n) for (bb, c), n in r["stalls"].items() if bb == b), key=lambda x: (-x[1], x[0]))
        total = sum(n for _, n in items)
        rate = ("%.1f/h" % (total / r["hours"])) if r["hours"] else "-"
        out.append("  %-9s %5d (%s)%s%s" % (b, total, rate, ": " if items else "", ", ".join("%s %d" % kv for kv in items)))
    if with_probe:
        p = r["probe"]
        if not p:
            out.append("  probe: no 'Stall probe:' lines (BB_STALL_PROBE off?)")
        else:
            main_dom = sum(1 for t in p if t[1] >= t[2])
            party = sum(1 for t in p if t[3] and t[4] >= 0.5 * t[3])
            worst = max(p, key=lambda t: t[0])
            out.append("  probe: %d long frames; main work the larger part in %d, render wait in %d; party tick half "
                       "the main samples or more in %d; worst %.0f ms (main %.0f, frame end %.0f)"
                       % (len(p), main_dom, len(p) - main_dom, party, worst[0], worst[1], worst[2]))
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--probe", action="store_true", help="also summarise the BB_STALL_PROBE lines")
    a = ap.parse_args(argv)
    for path in a.logs:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = [HARNESS_TS.sub("", x, count=1) for x in f.read().splitlines()]
        print(report(path[-70:], scan(lines), a.probe))
    return 0


if __name__ == "__main__":
    sys.exit(main())
