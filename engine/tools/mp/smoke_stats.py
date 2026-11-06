#!/usr/bin/env python3
"""Per-phase wall-time breakdown and pass/fail/retry history of saved smoke_party.py output dirs.

    python tools/mp/smoke_stats.py DIR [DIR ...] [--csv out.csv]

A DIR is a smoke --out folder (DIR/<phase>/harness.log, instN.log, rows.json, wall.txt; retries are
DIR/<phase>_tryN). For every phase run (retries included) it prints:

  queue    seconds waiting for game slots (game_queue)
  ready    run start -> the phase's first scripted step (game boot, title, load, join)
  steps    first step -> last step (the scripted test itself)
  tail     last step -> end of the run (until-hold, markers, the quiet wait, timeouts)
  end      how the run ended: ok (all --until markers seen), chains (chains done), notmet (UNTIL NOT MET),
           timeout (the --seconds cap), crash, stuck / dead (the fail-fast verdicts), lease, none
  failed   failed verdict rows of that run (rows.json; only the final run of a phase has one)

then the totals by end kind and the phases that needed a retry (flaky) or failed.
"""
import argparse, collections, glob, json, os, re, sys

STEP = re.compile(r"^\[\+ *([\d.]+)\][^\n]*?test: (?:travel )?step \d+", re.M)
TS = re.compile(r"^\[\+ *([\d.]+)\]", re.M)


def end_kind(v):
    for key, k in (("all --until markers seen", "ok"), ("all test chains done", "chains"), ("UNTIL NOT MET", "notmet"),
                   ("TIMED OUT", "timeout"), ("UNPLANNED CRASH", "crash"), ("STUCK", "stuck"), ("DEAD", "dead"),
                   ("LEASE", "lease"), ("HARNESS GONE", "gone"), ("IDLE", "idle")):
        if v.startswith(key):
            return k
    return "none"


def run_stats(d):
    h = os.path.join(d, "harness.log")
    if not os.path.exists(h):
        return None
    t = open(h, errors="replace").read()
    m = re.search(r"^verdict: (.*)$", t, re.M)
    verdict = m.group(1) if m else ""
    qw = sum(int(x) for x in re.findall(r"after waiting (\d+) s", t))
    first, last, end = None, None, 0.0
    for i in range(5):
        p = os.path.join(d, "inst%d.log" % i)
        if not os.path.exists(p):
            continue
        l = open(p, errors="replace").read()
        st = [float(x) for x in STEP.findall(l)]
        ts = [float(x) for x in TS.findall(l)]
        if st:
            first = min(first, st[0]) if first is not None else st[0]
            last = max(last, st[-1]) if last is not None else st[-1]
        if ts:
            end = max(end, ts[-1])
    rows = None
    rj = os.path.join(d, "rows.json")
    if os.path.exists(rj):
        try:
            rows = json.load(open(rj))
        except ValueError:
            pass
    wall = None
    if os.path.exists(os.path.join(d, "wall.txt")):
        try:
            wall = int(open(os.path.join(d, "wall.txt")).read())
        except ValueError:
            pass
    return dict(queue=qw, ready=first, steps=(last - first) if first is not None else None,
                tail=(end - last) if last is not None else None, run=end, end=end_kind(verdict), verdict=verdict,
                failed=[r[0] for r in rows if not r[1]] if rows is not None else None, wall=wall)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--csv")
    a = ap.parse_args()
    runs = []
    for top in a.dirs:
        for d in sorted(glob.glob(os.path.join(top, "*/"))):
            d = d.rstrip("/\\")
            name = os.path.basename(d)
            ph = re.sub(r"_try\d+$", "", name)
            s = run_stats(d)
            if s:
                runs.append(dict(s, run=os.path.basename(top.rstrip("/\\")), phase=ph, dir=name, retry="_try" in name))
    f = lambda x: "%5.0f" % x if x is not None else "    -"
    print("%-8s %-10s %5s %5s %5s %5s  %-8s %s" % ("run", "phase", "queue", "ready", "steps", "tail", "end", "failed rows"))
    for r in runs:
        print("%-8s %-10s %s %s %s %s  %-8s %s" % (r["run"][:8], r["dir"], f(r["queue"]), f(r["ready"]), f(r["steps"]),
                                                   f(r["tail"]), r["end"], ("; ".join(r["failed"])[:90] if r["failed"] else "")))
    tot = collections.Counter()
    n = collections.Counter()
    for r in runs:
        tot[r["end"]] += r["run"] if isinstance(r["run"], (int, float)) else 0
    by = collections.defaultdict(lambda: [0.0, 0])
    for r in runs:
        by[r["end"]][0] += (r["ready"] or 0) + (r["steps"] or 0) + (r["tail"] or 0)
        by[r["end"]][1] += 1
    print("\nwall by how the run ended (ready+steps+tail, s):")
    for k, (s, c) in sorted(by.items(), key=lambda kv: -kv[1][0]):
        print("  %-8s %3d runs %6.0f s" % (k, c, s))
    ready = [r["ready"] for r in runs if r["ready"] is not None and r["end"] == "ok"]
    steps = [r["steps"] for r in runs if r["steps"] is not None and r["end"] == "ok"]
    tail = [r["tail"] for r in runs if r["tail"] is not None and r["end"] == "ok"]
    print("\nsuccessful runs: %d, ready %.0f s, steps %.0f s, tail %.0f s in all" % (len(ready), sum(ready), sum(steps), sum(tail)))
    ph = collections.defaultdict(list)
    for r in runs:
        ph[r["phase"]].append(r)
    print("\nphases with more than one run (flaky: needed a retry) or failed rows:")
    for p, l in sorted(ph.items()):
        if len(l) > 1 or any(x["failed"] for x in l):
            print("  %-8s %d run(s): %s" % (p, len(l), " | ".join("%s%s" % (x["end"], "(%d rows)" % len(x["failed"]) if x["failed"] else "") for x in l)))
    if a.csv:
        with open(a.csv, "w") as fh:
            fh.write("run,dir,phase,queue,ready,steps,tail,end,failed\n")
            for r in runs:
                fh.write("%s,%s,%s,%s,%s,%s,%s,%s,%d\n" % (r["run"], r["dir"], r["phase"], r["queue"], r["ready"], r["steps"],
                                                           r["tail"], r["end"], len(r["failed"] or [])))


if __name__ == "__main__":
    main()
