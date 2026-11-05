#!/usr/bin/env python3
"""Summarise the BB_LATENCY_LOG lines of a game log (engine/gpu/shim/bbport_latency.h; visy's analyze_latency.py).

  python -I tools/latency_digest.py session.log [--skip 3]

Each "latency: n=.. pad_age_us avg/p95/max=.. read_to_present_ms avg/p95/max=.. reads=.." line is one second.
Prints, over the seconds kept (--skip drops the first seconds: loading and warm-up; seconds with fewer than 10
frames are left out), the mean of the per-second averages and the worst per-second p95 / max. pad_age is how
old the pad state was when the game read it; read_to_present is from that read to the CPU present call of its
frame. Neither includes the display or the physical device.
"""
import argparse
import re
import statistics
import sys

LINE = re.compile(r"latency: n=(\d+) pad_age_us avg/p95/max=([\d.]+)/([\d.]+)/([\d.]+) "
                  r"read_to_present_ms avg/p95/max=([\d.]+)/([\d.]+)/([\d.]+) reads=(\d+)"
                  r"(?: read_to_flip_ms avg/p95/max=([\d.]+)/([\d.]+)/([\d.]+)"
                  r" flip_to_present_ms avg/p95/max=([\d.]+)/([\d.]+)/([\d.]+))?")


def parse(text):
    rows = []
    for m in LINE.finditer(text):
        n, *vals = m.groups()
        rows.append((int(n), *[float(v) if v is not None else 0.0 for v in vals]))
    return rows


def summarise(rows, skip=3):
    rows = [r for r in rows[skip:] if r[0] >= 10]
    if not rows:
        return None
    return {
        "seconds": len(rows),
        "frames_per_s": statistics.mean(r[0] for r in rows),
        "pad_age_us_avg": statistics.mean(r[1] for r in rows),
        "pad_age_us_p95": max(r[2] for r in rows),
        "pad_age_us_max": max(r[3] for r in rows),
        "read_to_present_ms_avg": statistics.mean(r[4] for r in rows),
        "read_to_present_ms_p95": max(r[5] for r in rows),
        "read_to_present_ms_max": max(r[6] for r in rows),
        "read_to_flip_ms_avg": statistics.mean(r[8] for r in rows),
        "flip_to_present_ms_avg": statistics.mean(r[11] for r in rows),
        "flip_to_present_ms_p95": max(r[12] for r in rows),
        # A queue that creeps up shows in the last 10 s (the average hides it).
        "first10_read_to_present_ms_avg": statistics.mean(r[4] for r in rows[:10]),
        "last10_read_to_present_ms_avg": statistics.mean(r[4] for r in rows[-10:]),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("--skip", type=int, default=3)
    args = ap.parse_args(argv)
    with open(args.log, encoding="utf-8", errors="replace") as f:
        summary = summarise(parse(f.read()), args.skip)
    if not summary:
        print("no latency lines (run with BB_LATENCY_LOG=1)")
        return 1
    for key, value in summary.items():
        print(f"{key}: {value:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
