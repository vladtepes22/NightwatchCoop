#!/usr/bin/env python3
"""A/B of the guest CPU savings (BB_JOB_BARRIER_STEAL, BB_WORKPOOL_NODE_CACHE) inside ONE game session.

Runs smoke phase `svl` (one player, a test save, optional lamp warps) with the features installed but off
(mode 2) and flips them through BB_TOGGLE_FILE (experiment bits 128 = job barrier steal, 256 = work-pool node
cache) every --period seconds once the world is up in the target area, in a fixed A B A B ... order. The log
(BB_LIVE_FPS_LOG=1: per-thread CPU and the `CPU save:` state line once a second, BB_FRAME_RATE_LOG=2) is then
summed per state by --report.

  python tools/mp/cpusave_ab.py run --feature barrier|cache|both --lamp 2412951 --blocks 4 --period 30
  python tools/mp/cpusave_ab.py report E:/.../inst0/run.log

The game is started by the harness (slot queue, time limit, ends by itself); nothing is left running.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
AGENT_ROOT = REPO.parent

BITS = {"barrier": 128, "cache": 256, "both": 128 + 256}
RATE = re.compile(r"Frame rate: [\d.]+ s real, \d+ steps \(([\d.]+) per s\)")
STATE = re.compile(r"CPU save: barrier (on|off) calls (\d+) sweeps (\d+) steals (\d+) \| node cache (on|off) pools (\d+) "
                   r"ended (\d+) hits (\d+) misses (\d+) kept (\d+) passed (\d+)")
THREADS = re.compile(r"Live clock threads: \d+ threads; busy \(ms CPU per s\):(.*)")
THREAD = re.compile(r"\[(.*?) (\d+) p(-?\d+) (\d+)\]")


def flipper(log: Path, toggle: Path, bit: int, blocks: int, period: float, marker: str, stop: threading.Event):
    """Waits for the marker line, then writes the toggle file: off, on, off, on ... (blocks of `period` s)."""
    toggle.write_text("0 0\n")
    deadline = time.time() + 900
    while not stop.is_set() and time.time() < deadline:
        try:
            if re.search(marker, log.read_text(errors="replace")):
                break
        except OSError:
            pass
        time.sleep(1)
    else:
        return
    time.sleep(22)  # the warp's load and the area's first seconds streaming in
    t0 = time.time()
    for k in range(blocks):
        on = k % 2 == 1
        toggle.write_text("0 %d\n" % (bit if on else 0))
        print("[ab] block %d: %s at +%.0f s" % (k, "ON" if on else "off", time.time() - t0), flush=True)
        if stop.wait(period):
            return
    toggle.write_text("0 0\n")


def run(a):
    root = Path(a.root)
    toggle = AGENT_ROOT / "cpusave_toggle.txt"
    env = os.environ.copy()
    env.update({"BB_TOGGLE_FILE": str(toggle), "BB_LIVE_FPS_LOG": "1", "BB_FRAME_RATE_LOG": "2",
                "BB_JOB_BARRIER_STEAL": "2" if a.feature in ("barrier", "both") else "0",
                "BB_WORKPOOL_NODE_CACHE": "2" if a.feature in ("cache", "both") else "0",
                "BB_FRAME_STATS": "1", "BB_AUDIO": "none", "BB_WINDOW_BACKGROUND": "1",
                "BB_SVL_FIXTURE": "E:/games/bbport/test_saves/cathedral_day",
                "BB_SVL_SECONDS": str(int(a.blocks * a.period + 40)),
                "BB_SVL_STEPS": "host_warp=%s@12" % a.lamp if a.lamp else ""})
    if a.fps:
        env["BB_FPS"] = a.fps
    for old in (root / "inst0" / "run.log",):
        if old.exists():
            old.unlink()  # the flipper waits for THIS run's marker
    stop = threading.Event()
    th = threading.Thread(target=flipper, args=(root / "inst0" / "run.log", toggle, BITS[a.feature], a.blocks,
                                                a.period, a.marker, stop), daemon=True)
    th.start()
    cmd = [sys.executable, str(REPO / "tools" / "mp" / "smoke_party.py"), "--root", str(root), "--phase", "svl",
           "--out", str(AGENT_ROOT / "smoke" / ("ab_" + a.feature + "_" + time.strftime("%H%M%S"))), "--no-shots"]
    if a.fps:
        cmd += ["--fps", a.fps]
    rc = subprocess.call(cmd, env=env, cwd=str(REPO))
    stop.set()
    th.join(5)
    print("[ab] smoke exit code", rc)
    return rc


def report(path: Path):
    lines = path.read_text(errors="replace").splitlines()
    # only the area after the warp's load (the second "loading screen gone")
    armed = [i for i, l in enumerate(lines) if "host_warp done; stage request armed" in l]
    if armed:
        lines = lines[armed[0]:]
        states = [i for i, l in enumerate(lines) if "CPU save:" in l]
        if len(states) > 24:
            lines = lines[states[22]:]  # the warp's load and the first seconds of the area
    rows = []  # per second: state, fps, thread cpu
    cur = {}
    for l in lines:
        m = RATE.search(l)
        if m:
            cur["fps"] = float(m.group(1))
        m = THREADS.search(l)
        if m:
            cur["threads"] = {(n, tid): int(ms) for n, tid, _p, ms in THREAD.findall(m.group(1))}
        m = STATE.search(l)
        if m:
            cur["barrier"], cur["cache"] = m.group(1), m.group(5)
            cur["counts"] = tuple(int(x) for x in m.groups()[1:4]) + tuple(int(x) for x in m.groups()[7:11])
            rows.append(dict(cur))
            cur = {"fps": cur.get("fps")}
    print("seconds with a state line:", len(rows))
    # blocks of equal state (drop 4 s after each change)
    out = {}
    prev = None
    skip = 0
    for r in rows:
        key = (r["barrier"], r["cache"])
        if key != prev:
            prev, skip = key, 4
        if skip:
            skip -= 1
            continue
        out.setdefault(key, []).append(r)
    for key, rs in sorted(out.items()):
        fps = [r["fps"] for r in rs if r.get("fps")]
        busy = {}
        for r in rs:
            for k, ms in r.get("threads", {}).items():
                busy.setdefault(k, []).append(ms)
        top = sorted(busy.items(), key=lambda kv: -statistics.mean(kv[1]))[:8]
        print("barrier=%s cache=%s: %d s, rate median %.1f mean %.1f/s | busiest threads (ms CPU/s): %s" % (
            key[0], key[1], len(rs), statistics.median(fps) if fps else 0, statistics.mean(fps) if fps else 0,
            " ".join("%s:%d" % (k[0][:10], statistics.mean(v)) for k, v in top)))
    if rows:
        print("last counters (barrier calls/sweeps/steals, cache hits/misses/kept/passed):", rows[-1]["counts"])


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--feature", choices=sorted(BITS), default="both")
    r.add_argument("--lamp", default="2412951", help="host_warp lamp id after the load (empty: stay in the Ward)")
    r.add_argument("--blocks", type=int, default=4)
    r.add_argument("--period", type=float, default=30)
    r.add_argument("--fps", default="uncap")
    r.add_argument("--root", default=str(AGENT_ROOT / "mp_root"))
    r.add_argument("--marker", default=r"host_warp done; stage request armed")
    p = sub.add_parser("report")
    p.add_argument("log")
    a = ap.parse_args()
    if a.cmd == "run":
        sys.exit(run(a))
    report(Path(a.log))


if __name__ == "__main__":
    main()
