#!/usr/bin/env python3
"""Abort a many-game test run before the PC pages: kills the test games under one instance root when the free physical
memory falls below a floor (default 1024 MB), and says so.

  python tools/mp/ramwatch.py --root E:\\games\\bbport\\agents\\NAME\\mp [--floor-mb 1024] [--poll 2] [--log FILE]

Run it next to a 6-game run (docs/party/six_players.md). It waits for the first game under --root, watches while any is
alive and exits when the last one is gone (or after --max-wait seconds without a game). Only processes whose executable lies
under --root are ever killed. Exit code 3 when it had to kill.
"""
import argparse
import ctypes
import subprocess
import sys
import time


class MemStatus(ctypes.Structure):
    _fields_ = [("l", ctypes.c_ulong), ("load", ctypes.c_ulong), ("tp", ctypes.c_ulonglong), ("ap", ctypes.c_ulonglong),
                ("tpf", ctypes.c_ulonglong), ("apf", ctypes.c_ulonglong), ("tv", ctypes.c_ulonglong),
                ("av", ctypes.c_ulonglong), ("ae", ctypes.c_ulonglong)]


def free_mb():
    m = MemStatus()
    m.l = ctypes.sizeof(MemStatus)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return int(m.ap // (1 << 20))


def games(root):
    """(pid, path) of every bbport.exe whose image lies under root."""
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='bbport.exe'\" | ForEach-Object { \"$($_.ProcessId)`t$($_.ExecutablePath)\" }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    low = root.lower().replace("/", "\\")
    res = []
    for ln in out.splitlines():
        pid, _, path = ln.partition("\t")
        if pid.strip().isdigit() and path.lower().startswith(low):
            res.append((int(pid), path))
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True)
    ap.add_argument("--floor-mb", type=int, default=1024)
    ap.add_argument("--poll", type=float, default=2.0)
    ap.add_argument("--max-wait", type=float, default=3600.0)
    ap.add_argument("--log")
    a = ap.parse_args()
    t0 = time.time()
    seen = False
    low = free_mb()
    out = open(a.log, "a", encoding="utf-8") if a.log else sys.stdout

    def say(msg):
        out.write("[ramwatch %.0f s] %s\n" % (time.time() - t0, msg))
        out.flush()

    while True:
        g = games(a.root)
        if g:
            seen = True
        elif seen:
            say("the games are gone; lowest free memory %d MB" % low)
            return 0
        elif time.time() - t0 > a.max_wait:
            say("no game appeared")
            return 0
        f = free_mb()
        low = min(low, f)
        if g and f < a.floor_mb:
            say("FREE MEMORY %d MB < %d MB with %d game(s): killing them" % (f, a.floor_mb, len(g)))
            for pid, _ in g:
                subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
            return 3
        time.sleep(a.poll)


if __name__ == "__main__":
    sys.exit(main())
