#!/usr/bin/env python3
"""Find EMEVD events with a "stale off": a state instruction skipped by a multiplayer guard whose reverse
(same instruction, same leading args, other last arg) runs later, un-guarded. A guest that runs a host-copy
event runs the off but not the on, which can undo the guest's own state (Rom's lake jump: 13204820 skipped its
invincibility on, ran the off 5 s later; fixed 0.7.7 by a client-only skip rule).

Game data (read-only, untrusted; run with python -I -X utf8). GAME = .../dvdroot_ps4.

  python -I -X utf8 tools/party/stale_off_tool.py scan GAME/event [--all]

Guards: 1003[5] (skip N if multiplayer state), 1003[105] (jump to label), 1003[109] (client count jump).
Without --all the boss-room rematch object toggles (2005[3]) and boss speffect-on-part (2004[53]) are left out.
Exit status 0 always (a review list, not a gate).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bossroom_tool as bt  # noqa: E402
import flag_tool as ft  # noqa: E402

STATE_BANKS = (2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2012, 2013, 2014)
MP_GUARDS = (5, 105, 109)


def guard_range(ins, i):
    b, n, a = ins[i]
    if n == 5:
        return i + 1, i + 1 + a[0]
    e = next((k for k in range(i + 1, len(ins)) if ins[k][0] == 1014 and ins[k][1] == a[0]), None)
    return (i + 1, e) if e is not None else None


def scan(event_dir, everything=False):
    edf = ft.load_emedf(ft.find_emedf(event_dir))
    out = []
    for stem, eid, name, ins in bt.events(event_dir):
        for g, (b, n, a) in enumerate(ins):
            if b != 1003 or n not in MP_GUARDS:
                continue
            r = guard_range(ins, g)
            if not r:
                continue
            s, e = r
            for x in range(s, min(e, len(ins))):
                bx, nx, ax = ins[x]
                if bx not in STATE_BANKS or (not everything and (bx, nx) in ((2004, 53), (2005, 3))):
                    continue
                for y in range(e, len(ins)):
                    by, ny, ay = ins[y]
                    if (by, ny) == (bx, nx) and ay[:-1] == ax[:-1] and ay[-1] != ax[-1]:
                        out.append((stem, eid, name, g, n, x, y, bx, nx, edf.get((bx, nx), ("?",))[0], ax, ay))
    return out


def main():
    if len(sys.argv) < 3 or sys.argv[1] != "scan":
        print(__doc__)
        return 2
    for r in scan(sys.argv[2], "--all" in sys.argv):
        print("%s %d %s guard@%d 1003[%d] on@%d off@%d %d[%d] %s %s -> %s" % r)


if __name__ == "__main__":
    sys.exit(main() or 0)
