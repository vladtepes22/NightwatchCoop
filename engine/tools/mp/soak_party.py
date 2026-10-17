#!/usr/bin/env python3
"""Party soak (docs/party/robustness_backlog.md item 4): two rendered games play for 60-90 min with
NO test aids beyond scripted pad input, while tools/mp/soak_sample.py logs memory / VRAM / handles /
threads every minute and BB_PARTY_PERF=1 logs the frame time every 60 s.

  python tools/mp/soak_party.py [--minutes 75] [--root DIR] [--out DIR]
  python tools/mp/soak_party.py --report --out DIR

What the players do (pad input only: the director's host_stick / guest_stick walks by the left stick
through the in-process pad file, closed loop on the position, no warp, no invincibility; host_press /
guest_press taps pad buttons): the fixture save `cleric_beast_lamp` (test_saves.md: the Cleric
Beast alive, both players at the Great Bridge lamp 2412952) is Continued; the guest title-joins. Then
in a loop: both walk onto the bridge into the Cleric Beast's arena (the fight starts by walking in),
lock on (R3) and attack (R1) until they die or the beast does; whoever dies respawns at the lamp
(the party's own death rules: limbo, wipe, the host's death load); both walk back to the lamp and use
it (Cross, then Circle: the lamp's menu opened and closed, which clears rot and rests). No warps, no
ForceDead, no flags written during the run, no invincibility.

Output: OUT/soak.csv (sampler), OUT/inst{0,1}.log, OUT/shots (every 60 s), OUT/report.txt (the
sampler summary, the frame time per 10 min, deaths / wipes / boss kills / joins / loads / Session
lost / crashes counted from the logs).
"""
import argparse, os, re, subprocess, sys, time
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
DEFAULT_ROOT = (AGENT_ROOT / "soakroot") if AGENT_ROOT.parent.name == "agents" else (REPO.parent / "soakroot")
SAVES = Path(r"E:\games\bbport\test_saves\cleric_beast_lamp\savedata")

# Positions in m24_01 (x, z): the Great Bridge lamp 2412952 and the Cleric Beast's first-fight trigger
# region 2412805 (12 x 4 m box at (-136.5, 52.8)); see docs/party/b2_seamless.md 6.7.
LAMP = (-124.8, 64.3)      # where the game puts the player at the lamp (fixture run log_pos)
ARENA_HOST = (-136.5, 52.8)
ARENA_GUEST = (-134.6, 54.7)
R1, R3, CROSS, CIRCLE = 0x800, 0x4, 0x4000, 0x2000


def chain(role, cycles, arena, lamp):
    p = role + "_"
    one = [
        "%sstick=%.2f:%.2f:2:80@8%%m24_01" % (p, arena[0], arena[1]),   # walk into the arena (the fight starts)
        "%spress=0x%x@1%%m24_01!" % (p, R3),                              # lock on
    ]
    one += ["%spress=0x%x@2%%m24_01!" % (p, R1)] * 10                  # attack
    one += [
        "%sstick=%.2f:%.2f:1.5:80@3%%m24_01" % (p, lamp[0], lamp[1]),   # back to the lamp (or from the respawn)
        "%spress=0x%x@1%%m24_01!" % (p, CROSS),                          # use the lamp
        "%spress=0x%x@4%%m24_01" % (p, CIRCLE),                          # close its menu
        "%spress=0x%x@2%%m24_01" % (p, CIRCLE),
    ]
    return ",".join(one * cycles)


def run(a, out: Path):
    root = Path(a.root)
    root.mkdir(parents=True, exist_ok=True)
    secs = int(a.minutes * 60)
    (root / "noreap").write_text(str(secs + 300))
    out.mkdir(parents=True, exist_ok=True)
    starts = {}
    for i in range(2):
        p = root / f"inst{i}" / "run.log"
        # instances.py moves the old run.log to run.prev.log, unless something holds it open (then
        # it appends): remember the file id and size, and copy from there when it is the same file.
        starts[i] = (p.stat().st_ino, p.stat().st_size) if p.exists() else (None, 0)
    cycles = max(1, secs // 60)
    lamp = tuple(float(x) for x in a.lamp.split(",")) if a.lamp else LAMP
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", "2", "--party", "--fresh-saves", "--timestamps",
           "--idle", "0", "--seconds", str(secs), "--root", str(root), "--build", str(a.build), "--game", str(a.game),
           "--saves", str(SAVES), "--label", "soak %d min (pad only)" % a.minutes,
           "--env", "BB_PARTY_PERF=1", "--env", "BB_PARTY_PERF_SECONDS=60",
           "--env", "BB_PAD_FILE=%s" % (root / "pad{i}.txt"),
           "--ienv", "0:BB_PARTY_DIRECTOR_TEST=" + chain("host", cycles, ARENA_HOST, lamp),
           "--ienv", "1:BB_PARTY_DIRECTOR_TEST=" + chain("guest", cycles, ARENA_GUEST, lamp)]
    env = dict(os.environ, BB_MP_LEASE=str(secs + 600))
    (out / "harness.log").unlink(missing_ok=True)
    sampler = LaterProc(out / "harness.log", [sys.executable, str(HERE / "soak_sample.py"), "--root", str(root), "--out",
                                str(out / "soak.csv"), "--every", "60", "--seconds", str(secs + 300)],
                               stdout=open(out / "sampler.log", "w"), stderr=subprocess.STDOUT)
    shooter = LaterProc(out / "harness.log", [sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(out / "shots"),
                                "--tag", "soak", "--every", "60", "--grey", "--seconds", str(secs + 600)],
                               stdout=open(out / "shot.log", "w"), stderr=subprocess.STDOUT)
    (out / "shots").mkdir(exist_ok=True)
    print("[soak] " + " ".join(cmd)[:400] + " ...", flush=True)
    try:
        subprocess.run(cmd, env=env, stdout=open(out / "harness.log", "w"), stderr=subprocess.STDOUT)
    finally:
        sampler.terminate()
        shooter.terminate()
    for i in range(2):
        with open(root / f"inst{i}" / "run.log", "rb") as f:
            f.seek(starts[i][1] if os.fstat(f.fileno()).st_ino == starts[i][0] else 0)
            (out / f"inst{i}.log").write_bytes(f.read())
    try:
        (root / "noreap").unlink()
    except OSError:
        pass


COUNT = [
    ("own deaths (HP 0 / death path)", r"(?i)Party B2: .*(died|death)|guest: dead|host_death|held death"),
    ("wipes", r"(?i)a wipe; the party respawns"),
    ("boss fight starts (boss bar)", r"(?i)boss (health )?bar (shown|up)"),
    ("boss kills", r"(?i)host boss kill|boss kill|PREY SLAUGHTERED"),
    ("joins (host cooperators 0 -> 1)", r"cooperators 0 -> 1"),
    ("loading screens", r"loading screen up"),
    ("stick walks arrived", r"approach: arrived"),
    ("stick walks blocked / gave up", r"approach: (BLOCKED|GAVE UP)"),
    ("Session lost", r"Session lost"),
    ("crash restarts", r"a crash\); restart"),
    ("watchdog stops", r"host watchdog: stop"),
    ("desync digests that differed", r"(?i)digest.*(differ|mismatch)"),
]


def report(a, out: Path):
    lines = []
    for i in range(2):
        p = out / f"inst{i}.log"
        text = p.read_text(encoding="utf-8", errors="replace") if p.exists() else ""
        lines.append("inst%d:" % i)
        for name, rx in COUNT:
            lines.append("   %-34s %d" % (name, len(re.findall(rx, text, re.M))))
    r = subprocess.run([sys.executable, str(HERE / "soak_sample.py"), "--summary", str(out / "soak.csv"),
                        "--log", str(out / "inst0.log"), "--log", str(out / "inst1.log")], capture_output=True, text=True)
    text = "\n".join(lines) + "\n\n" + r.stdout + r.stderr
    grey = out / "shots" / "grey.log"
    if grey.exists():
        text += "\ngrey screens flagged by shot.py --grey:\n" + grey.read_text(errors="replace")
    (out / "report.txt").write_text(text, encoding="utf-8")
    print(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minutes", type=float, default=75)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--out", default=str(AGENT_ROOT / "soak" / time.strftime("%Y%m%d_%H%M")))
    ap.add_argument("--lamp", help="X,Z of the lamp spot (default: LAMP)")
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    if Path(a.game).resolve() == Path(r"E:\games\bloodborne").resolve():
        sys.exit("never the user's game folder as the test game (agents/RULES_062.txt)")
    out = Path(a.out)
    if not a.report:
        run(a, out)
    report(a, out)


if __name__ == "__main__":
    main()
