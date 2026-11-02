#!/usr/bin/env python3
"""Walker A/B numbers from smoke runs (docs/party/walker.md).

  python tools/mp/walk_report.py RUNDIR [RUNDIR ...]      RUNDIR = a smoke --out dir (e.g. runs/cao_nav_1)

Per run and instance: walks started / arrived / gave up, seconds walked, side steps, hops / warps, stalls, and which
walker ran (nav lines or the old "approach: (x, z) -> ..." step lines). Then a total per walker.
Logs are the games' own (untrusted text), parsed with regexes only.
"""
import re, sys
from pathlib import Path

END = re.compile(r"test: approach: (arrived|BLOCKED|GAVE UP) at ([\d.]+) m(?: \(nav(?: FAILED: ([^:]*))?: ([\d.]+) s, (\d+) replan, "
                 r"(\d+) back-up, (\d+) strafe, (\d+) roll, (\d+) warp, (\d+) stall)?")
ROUTE = re.compile(r"test: route: (DONE|STOPPED) at waypoint (\d+)/(\d+) .* after (\d+) s, (\d+) side step\(s\), (\d+) hop\(s\)")
STAMP = re.compile(r"\[\+\s*([\d.]+)\]")


def stamp(line):
    m = STAMP.search(line)
    return float(m.group(1)) if m else None


def one(path):
    r = dict(nav=0, old=0, ok=0, bad=0, secs=0.0, side=0, hops=0, rolls=0, stalls=0, replans=0, fails=[])
    start = None
    for line in Path(path).read_text(errors="replace").splitlines():
        if "test: nav: planned" in line:
            r["nav"] += 1
            if start is None:
                start = stamp(line)
        if start is None and re.search(r"test: approach: \(-?[\d.]+, -?[\d.]+\) ->", line):
            start = stamp(line)
        if re.search(r"test: (guest|host)_(stick|route|move): (walk to|\d+ waypoint|walked to)", line):
            start = stamp(line)
        m = END.search(line)
        if m:
            t = stamp(line)
            if start is not None and t is not None:
                r["secs"] += t - start
            start = None
            if m.group(4) is None:
                r["old"] += 1
            else:
                r["replans"] += int(m.group(5))
                r["side"] += int(m.group(6)) + int(m.group(7))
                r["rolls"] += int(m.group(8))
                r["stalls"] += int(m.group(10))
            if m.group(1) == "arrived":
                r["ok"] += 1
            else:
                r["bad"] += 1
                r["fails"].append(m.group(3) or m.group(1))
        m = ROUTE.search(line)
        if m and "nav" not in r.get("_w", ""):
            if r["old"] and not r["nav"]:
                r["side"] += int(m.group(5))
        if "test: route: HOP" in line:
            r["hops"] += 1
    return r


def main():
    tot = {}
    for d in sys.argv[1:]:
        for log in sorted(Path(d).glob("*/inst[0-9].log")):
            r = one(log)
            if not (r["nav"] or r["ok"] or r["bad"]):
                continue
            walker = "nav" if r["nav"] else "old"
            print("%-34s %-4s %s: arrived %d, failed %d%s, %.0f s, %d replan, %d side/back-up, %d roll, %d hop/warp, %d stall" % (
                Path(d).name, walker, log.stem, r["ok"], r["bad"], (" (" + ", ".join(r["fails"]) + ")") if r["fails"] else "",
                r["secs"], r["replans"], r["side"], r["rolls"], r["hops"], r["stalls"]))
            t = tot.setdefault(walker, dict(ok=0, bad=0, secs=0.0, side=0, hops=0, n=0))
            t["ok"] += r["ok"]
            t["bad"] += r["bad"]
            t["secs"] += r["secs"]
            t["side"] += r["side"]
            t["hops"] += r["hops"]
            t["n"] += 1
    for w, t in tot.items():
        n = t["ok"] + t["bad"]
        print("TOTAL %-4s walks %d, arrived %d (%.0f%%), side/back-up %d, hops/warps %d, nav seconds %.0f" % (
            w, n, t["ok"], 100.0 * t["ok"] / max(1, n), t["side"], t["hops"], t["secs"]))


if __name__ == "__main__":
    main()
