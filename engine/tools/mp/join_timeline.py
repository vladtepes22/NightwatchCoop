#!/usr/bin/env python3
"""Join / summon timeline from a timestamped 2-instance run (instances.py run --timestamps).

  python tools/mp/join_timeline.py [--root DIR] [--from 1:"Launching Bloodborne":2] [--all]

Prints the milestones of an automatic summon (docs/party/join_timing.md) in order: each one is
the first matching line at or after the previous milestone's time (so a crash rejoin can be
measured from the restart: --from INST:REGEX:N starts at the N-th match of REGEX in instance
INST's log). Times are the harness's "[+SSS.mmm]" prefixes (one clock for both instances).
--all lists every match of every milestone instead.
"""
import argparse, re
from pathlib import Path

HERE = Path(__file__).resolve().parent

# (instance, label, regex[, label of the milestone it must follow]). Instance 0 hosts, 1 joins.
MILESTONES = [
    (0, "host: game started", r"Launching Bloodborne PC"),
    (1, "guest: game started", r"Launching Bloodborne PC"),
    (0, "host: party link hosting", r"Party: hosting on port"),
    (1, "guest: party link connected", r"Party: connected to the host as slot"),
    (0, "host: Continue pressed", r"auto-continue: Cross #1\b"),
    (1, "guest: Continue pressed", r"auto-continue: Cross #1\b"),
    (0, "host: world up", r"Party: world up"),
    (0, "host: in world (load done)", r"Party: loading screen gone"),
    (1, "guest: world up", r"Party: world up"),
    (1, "guest: in world (load done)", r"Party: loading screen gone"),
    (1, "guest: rings (Small Resonant Bell)", r"ringing the Small Resonant Bell"),
    (1, "guest: summon_messenger/create", r"summon_messenger/create"),
    (0, "host: board has the sign", r"from api: Hunter1 put up a sign"),
    (0, "host: rings (Beckoning Bell)", r"ringing the Beckoning Bell"),
    (0, "host: get poll shows the sign", r"from api: Hunter0 asks for signs: [1-9]"),
    (0, "host: summon_messenger/request", r"from api: Hunter0 summons Hunter1"),
    (1, "guest: guest_invite", r"np: guest_invite"),
    (1, "guest: signaling activate", r"sceNpSignalingActivateConnection Hunter"),
    (1, "guest: session trying-to-join (4)", r"-> trying-to-join \(4"),
    (1, "guest: Matching2 JoinRoom", r"sceNpMatching2JoinRoom req"),
    (1, "guest: joined room", r"np: joined room"),
    (0, "host: member joined room", r"np: member \d+ \(Hunter1\) joined"),
    (1, "guest: session client (6)", r"-> client \(6"),
    (1, "guest: loading screen up (host world)", r"Party: loading screen up", "guest: session trying-to-join (4)"),
    (0, "host: cooperators 1", r"cooperators 0 -> 1"),
    (1, "guest: in host world (load done)", r"Party: loading screen gone", "guest: session client (6)"),
]
STAMP = re.compile(r"^\[\+\s*([0-9.]+)\] ?(.*)$")


def read(root, i):
    out = []
    path = Path(root) / f"inst{i}" / "run.log"
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = STAMP.match(line)
        if m:
            out.append((float(m.group(1)), m.group(2)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(HERE.parent.parent.parent / "mptest"))
    ap.add_argument("--from", dest="start", help="INST:REGEX:N - start at the N-th match (1-based)")
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    logs = {0: read(a.root, 0), 1: read(a.root, 1)}
    t = 0.0
    if a.start:
        inst, rx, n = a.start.split(":", 2)[0], a.start.split(":")[1], int(a.start.rsplit(":", 1)[1])
        hits = [ts for ts, line in logs[int(inst)] if re.search(rx, line)]
        if len(hits) < n:
            raise SystemExit(f"--from: only {len(hits)} match(es) of {rx!r} in instance {inst}")
        t = hits[n - 1]
        print(f"from {t:8.3f} s ({rx!r} #{n} in instance {inst})")
    if a.all:
        for inst, label, rx in MILESTONES:
            ts = [f"{x:.3f}" for x, line in logs[inst] if x >= t and re.search(rx, line)]
            print(f"{label:40s} {' '.join(ts[:8])}")
        return
    # Each milestone: its first match from the start (or after the milestone it names), then all
    # in time order with the gap to the previous one.
    found, rows, missing = {}, [], []
    for m in MILESTONES:
        inst, label, rx = m[:3]
        after = found.get(m[3], (None,))[0] if len(m) > 3 else t
        if after is None:
            missing.append(label)
            continue
        hit = next(((x, line) for x, line in logs[inst] if x >= after and re.search(rx, line)), None)
        if not hit:
            missing.append(label)
            continue
        found[label] = hit
        rows.append((hit[0], label, hit[1]))
    prev = t
    for x, label, line in sorted(rows):
        print(f"{x:9.3f}  {x - prev:+7.3f}  {label:40s} | {line[:100]}")
        prev = x
    for label in missing:
        print(f"{'-':>9}  {'':>7}  {label}")
    print(f"total {prev - t:.3f} s")


if __name__ == "__main__":
    main()
