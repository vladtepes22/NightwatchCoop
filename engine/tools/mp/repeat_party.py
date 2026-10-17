#!/usr/bin/env python3
"""Repeat cycles for the race-prone party rows, many cycles in ONE game session (single-run rule),
with a success count and timings per cycle.

  python tools/mp/repeat_party.py SCENARIO [--cycles 22] [--root DIR] [--out DIR]
  python tools/mp/repeat_party.py SCENARIO --check-only --out DIR

Scenarios (2 rendered players, 127.0.0.1, default test save; run through tools/mp/instances.py):
  a5   A5 / A3 / E4: the guest's Party > Leave, then Rejoin, N times; the host stands in Central
       Yharnam (BB_PARTY_TEST_START_LAMP=2412951) while the guest's own world is the Hunter's Dream,
       so the first join is cross-map. Per cycle: Rejoin press -> the host's room (host
       "cooperators 0 -> 1"), the guest's map after it.
  c6   C6 / A4: the guest's game is killed (instances.py --crash-plan) 15 s after each join; run.bat
       restarts it and it title-joins. Per cycle: kill -> host "cooperators 0 -> 1" (C6), the restarted
       game's start -> the host's room (A4: a title join from a cold start).
  c3   C3: a wipe outside a boss fight N times: the guest dies (guest_die), the host dies 3 s after
       its down notice (BB_PARTY_TEST_DIE_ON_DOWN=3), the party respawns together. Per cycle: the
       host's "a wipe" line, the guest back in (host "cooperators 0 -> 1") and the time.
  a6   A6: the host's Party > Leave with the guest in, then the host's Rejoin and the guest's Rejoin,
       N times. Per cycle: the guest out (its "out of the party") after the host's press, the guest's
       game session idle, both back.

Test aids: the director's Party > Leave / Rejoin presses and guest_die (ForceDead) and the host's
die-on-down, the start lamp, crash-plan kills. Output: OUT/inst{0,1}.log, OUT/repeat.txt (one line per
cycle and the totals). Exit 0 when every cycle passed.
"""
import argparse, os, re, statistics, subprocess, sys, time
from pathlib import Path


class LaterProc:
    """Starts a helper (shot.py, the sampler) only once the harness has started the games (after the
    shared game queue's wait), so its run timer and window watch cover the run, not the wait."""

    def __init__(self, harness_log, args, **kw):
        import threading
        self.p, self.stop = None, False

        def run():
            while not self.stop:
                try:
                    if "instance 0 started" in Path(harness_log).read_text(errors="replace"):
                        break
                except OSError:
                    pass
                time.sleep(1)
            if not self.stop:
                self.p = subprocess.Popen(args, **kw)

        threading.Thread(target=run, daemon=True).start()

    def terminate(self):
        self.stop = True
        if self.p:
            self.p.terminate()

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
AGENT_ROOT = REPO.parent
DEFAULT_ROOT = (AGENT_ROOT / "mptest") if AGENT_ROOT.parent.name == "agents" else (REPO.parent / "mptest")
TS = re.compile(r"^\[\+ *([\d.]+)\] ")


def scenario(name, n, players=2):
    full = players - 1  # cooperators on the host with everyone in
    if name == "a5":
        g = ",".join(["guest_leave@8", "guest_rejoin@5"] * n)
        return dict(seconds=60 + 45 * n, env=[], ienv=["0:BB_PARTY_TEST_START_LAMP=2412951", "1:BB_PARTY_DIRECTOR_TEST=" + g],
                    crash=[])
    if name == "c6":
        return dict(seconds=60 + 50 * n, env=["BB_PARTY_RESTART_MAX=%d" % (n + 5), "BB_PARTY_DIRECTOR_TEST_RESTART=none"],
                    ienv=["0:BB_PARTY_TEST_START_LAMP=2412951"], crash=[r"1/0:cooperators \d -> %d@15" % full] * n)
    if name == "c3":
        g = ",".join(["guest_die@20%m24_01"] * n)
        return dict(seconds=60 + 75 * n, env=[],
                    ienv=["0:BB_PARTY_TEST_START_LAMP=2412951", "0:BB_PARTY_TEST_DIE_ON_DOWN=3", "1:BB_PARTY_DIRECTOR_TEST=" + g], crash=[])
    if name == "a6":
        h = ",".join(["host_leave@12", "host_rejoin@5"] * n)
        g = ",".join(["guest_rejoin@8"] * n)
        return dict(seconds=60 + 50 * n, env=[],
                    ienv=["0:BB_PARTY_TEST_START_LAMP=2412951", "0:BB_PARTY_DIRECTOR_TEST=" + h, "1:BB_PARTY_DIRECTOR_TEST=" + g], crash=[])
    sys.exit("unknown scenario " + name)


def run(a, sc, out: Path):
    root = Path(a.root)
    root.mkdir(parents=True, exist_ok=True)
    (root / "noreap").write_text(str(sc["seconds"] + 120))
    starts = {}
    for i in range(a.players):
        p = root / f"inst{i}" / "run.log"
        # instances.py moves the old run.log to run.prev.log, unless something holds it open (then
        # it appends): remember the file id and size, and copy from there when it is the same file.
        starts[i] = (p.stat().st_ino, p.stat().st_size) if p.exists() else (None, 0)
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", str(a.players), "--party", "--fresh-saves", "--timestamps",
           "--idle", "0", "--seconds", str(sc["seconds"]), "--root", str(root), "--build", str(a.build),
           "--game", str(a.game), "--label", "repeat %s x%d" % (a.scenario, a.cycles)]
    if a.players > 2:
        cmd += ["--headless", str(a.players - 2)]
    for e in sc["env"]:
        cmd += ["--env", e]
    for e in sc["ienv"]:
        cmd += ["--ienv", e]
    for c in sc["crash"]:
        cmd += ["--crash-plan", c]
    out.mkdir(parents=True, exist_ok=True)
    shots = out / "shots"
    shots.mkdir(exist_ok=True)
    (out / "harness.log").unlink(missing_ok=True)
    shooter = LaterProc(out / "harness.log", [sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(shots),
                                "--tag", a.scenario, "--every", "30", "--seconds", str(sc["seconds"] + 600)],
                               stdout=open(out / "shot.log", "w"), stderr=subprocess.STDOUT)
    env = dict(os.environ, BB_MP_LEASE=os.environ.get("BB_MP_LEASE", "1200"))
    print("[repeat] " + " ".join(cmd), flush=True)
    try:
        subprocess.run(cmd, env=env, stdout=open(out / "harness.log", "w"), stderr=subprocess.STDOUT)
    finally:
        shooter.terminate()
    for i in range(a.players):
        with open(root / f"inst{i}" / "run.log", "rb") as f:
            f.seek(starts[i][1] if os.fstat(f.fileno()).st_ino == starts[i][0] else 0)
            (out / f"inst{i}.log").write_bytes(f.read())
    try:
        (root / "noreap").unlink()
    except OSError:
        pass


def lines(out, i):
    res = []
    p = out / f"inst{i}.log"
    for line in (p.read_text(encoding="utf-8", errors="replace").splitlines() if p.exists() else []):
        m = TS.match(line)
        if m:
            res.append((float(m.group(1)), line[m.end():]))
    return res


def first(ls, rx, after, before=1e9):
    r = re.compile(rx)
    for t, l in ls:
        if after <= t < before and r.search(l):
            return t, l
    return None


def stats(xs):
    if not xs:
        return "-"
    xs = sorted(xs)
    return "median %.1f s, min %.1f, max %.1f, p90 %.1f (n=%d)" % (statistics.median(xs), xs[0], xs[-1],
                                                                    xs[min(len(xs) - 1, int(0.9 * len(xs)))], len(xs))


def check(a, out: Path):
    h, g = lines(out, 0), lines(out, 1)
    rows, ok_n, times, times2 = [], 0, [], []
    if a.scenario in ("a5", "a6"):
        who = g if a.scenario == "a5" else h
        presses = [t for t, l in who if re.search(r"(guest|host)_leave: Party > Leave pressed", l)]
        for k, t in enumerate(presses):
            nxt = presses[k + 1] if k + 1 < len(presses) else 1e9
            out_l = first(g, r"travel steps: out of the party|rejected: the host ended the party|out of the party", t, nxt)
            home = first(g, r"world up: world up", t + 0.5, nxt)
            rj = first(g if a.scenario == "a5" else h, r"Rejoin pressed|back in the party without a press", t, nxt)
            back = first(h, r"cooperators 0 -> 1", (rj or (t, ""))[0], nxt)
            lost = first(g, r"Session lost", t, nxt)
            mp = first(g, r"map .* -> (m\d\d_\d\d)", (rj or (t, ""))[0], nxt)
            ok = bool(out_l and back and not lost)
            ok_n += ok
            if back and rj:
                times.append(back[0] - rj[0])
            if out_l:
                times2.append(out_l[0] - t)
            rows.append("cycle %2d %s: Leave +%.1f; out %s; Rejoin %s; host room %s%s%s" % (
                k + 1, "PASS" if ok else "FAIL", t, "+%.1f s" % (out_l[0] - t) if out_l else "NO",
                "+%.1f" % rj[0] if rj else "NO", "%.1f s after the Rejoin" % (back[0] - rj[0]) if back and rj else "NO",
                "; map %s" % re.search(r"-> (\S+)", mp[1]).group(1) if mp else "", "; SESSION LOST" if lost else ""))
        summary = "Leave -> out: %s; Rejoin -> host room: %s" % (stats(times2), stats(times))
    elif a.scenario == "c6":
        harness = (out / "harness.log").read_text(errors="replace") if (out / "harness.log").exists() else ""
        crashes = [float(x) for x in re.findall(r"\[harness \+([\d.]+) s\] crash \d+ of \d+: instance 1", harness)]
        # harness clock and the --timestamps clock both start at the first instance start
        boots = [t for t, l in g if "Boot: run.bat started" in l or "restart" in l and "to rejoin the party" in l]
        for k, t in enumerate(crashes):
            nxt = crashes[k + 1] if k + 1 < len(crashes) else 1e9
            back = first(h, r"cooperators %d -> %d" % (a.players - 2, a.players - 1), t, nxt)
            tj = first(g, r"title join: in the world after ([\d.]+) s", t, nxt)
            start = first(g, r"Boot: run.bat started|state at the first tick", t, nxt)
            ok = bool(back)
            ok_n += ok
            if back:
                times.append(back[0] - t)
                if start:
                    times2.append(back[0] - start[0])
            kicked = first(h, r"KickoutRoomMember req", t, nxt)
            failed = first(g, r"title join: FAILED", t, nxt)
            ok = ok and not kicked
            ok_n -= (bool(back) and bool(kicked))
            rows.append("cycle %2d %s: kill +%.1f; host room %s; restarted game's first tick -> host room %s; %s%s%s" % (
                k + 1, "PASS" if ok else "FAIL", t, "%.1f s after the kill" % (back[0] - t) if back else "NO",
                "%.1f s" % (back[0] - start[0]) if back and start else "-", tj[1][-40:] if tj else "no title-join line",
                "; TITLE JOIN FAILED" if failed else "", "; KICK: " + kicked[1][-60:] if kicked else ""))
        summary = "C6 kill -> host room: %s; A4 cold start (first tick) -> host room: %s" % (stats(times), stats(times2))
    elif a.scenario == "c3":
        deaths = [t for t, l in g if re.search(r"travel step \d+/\d+: guest_die", l)]
        for k, t in enumerate(deaths):
            nxt = deaths[k + 1] if k + 1 < len(deaths) else 1e9
            hd = first(h, r"die on down: the host dies", t, nxt)
            wipe = first(h, r"(?i)a wipe|every member is down", t, nxt)
            back = first(h, r"cooperators 0 -> 1", (wipe or (t, ""))[0], nxt)
            gl = first(g, r"held death goes with the host's travel", t, nxt)
            lost = first(g, r"Session lost", t, nxt)
            loads = len([1 for tt, l in g if t <= tt < nxt and "loading screen up" in l])
            ok = bool(hd and wipe and back and not lost)
            ok_n += ok
            if back:
                times.append(back[0] - t)
            rows.append("cycle %2d %s: guest down +%.1f; host dies %s; wipe %s; guest held %s; back %s; guest loads %d%s" % (
                k + 1, "PASS" if ok else "FAIL", t, "yes" if hd else "NO", "yes" if wipe else "NO", "yes" if gl else "no",
                "%.1f s after the guest's death" % (back[0] - t) if back else "NO", loads, "; SESSION LOST" if lost else ""))
        summary = "guest death -> back together: %s" % stats(times)
    n = len(rows)
    text = "\n".join(rows) + "\n\n%s: %d of %d cycles passed. %s\n" % (a.scenario, ok_n, n, summary)
    (out / "repeat.txt").write_text(text, encoding="utf-8")
    print(text)
    return ok_n == n and n > 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", choices=["a5", "c6", "c3", "a6"])
    ap.add_argument("--cycles", type=int, default=22)
    ap.add_argument("--players", type=int, default=2, help="2, or 3-4 with the extra guests headless-lite")
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--out")
    ap.add_argument("--check-only", action="store_true")
    a = ap.parse_args()
    if Path(a.game).resolve() == Path(r"E:\games\bloodborne").resolve():
        sys.exit("never the user's game folder as the test game (agents/RULES_062.txt)")
    out = Path(a.out or (AGENT_ROOT / "repeat" / ("%s_%s" % (a.scenario, time.strftime("%Y%m%d_%H%M")))))
    if not a.check_only:
        run(a, scenario(a.scenario, a.cycles, a.players), out)
    sys.exit(0 if check(a, out) else 1)


if __name__ == "__main__":
    main()
