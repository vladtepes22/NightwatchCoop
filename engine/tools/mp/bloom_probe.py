#!/usr/bin/env python3
"""White-out probe (CL-75c935 / NW-747ed01): one game at a Forbidden Woods lamp, the camera panned
back and forth, a screenshot every --every seconds; the frames' mean brightness is checked for
jumps (a white-out frame: ~53 -> ~217 in the report). One game session sweeps the whole run.

  python tools/mp/bloom_probe.py --root E:\\...\\mp1 [--lamp 2702951] [--seconds 150] [--every 0.25]
         [--env KEY=VAL ...] [--tag NAME] [--walk X:Z]

The game is started by tools/mp/instances.py (the shared game queue), quits at --seconds, and its
screenshots are in ROOT/probe_<tag>/. Prints a summary: frames, mean brightness min / max, flash frames.
"""
import argparse, os, subprocess, sys, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def pad_sweep(pad: Path, start: float, stop: float, stop_evt):
    """Right stick to and fro (the camera pans), left stick still, after `start` s."""
    t0 = time.time()
    pad.write_text("lx=128 ly=128 rx=128 ry=128\n")
    while not stop_evt.is_set() and time.time() - t0 < stop:
        if time.time() - t0 < start:
            time.sleep(0.5)
            continue
        for rx, ry in ((200, 128), (128, 150), (56, 128), (128, 106)):
            if stop_evt.is_set():
                return
            pad.write_text("lx=128 ly=128 rx=%d ry=%d\n" % (rx, ry))
            time.sleep(2.5)
    pad.write_text("lx=128 ly=128 rx=128 ry=128\n")


def analyse(shots: Path, jump: float):
    from PIL import Image, ImageStat
    rows = []
    for p in sorted(shots.glob("*.png")):
        try:
            im = Image.open(p).convert("L")
            w, h = im.size
            # the 3D view without the HUD corners
            box = im.crop((w // 8, h // 8, w - w // 8, h - h // 8))
            rows.append((p.name, ImageStat.Stat(box).mean[0]))
        except OSError:
            pass
    flashes = []
    for i in range(1, len(rows)):
        if rows[i][1] - rows[i - 1][1] > jump and rows[i][1] > 150:
            flashes.append(rows[i])
    return rows, flashes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", ""))
    ap.add_argument("--lamp", default="2702951")
    ap.add_argument("--seconds", type=int, default=150)
    ap.add_argument("--every", type=float, default=0.25)
    ap.add_argument("--pan-start", type=float, default=45)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--director", default="")
    ap.add_argument("--fps", default="")
    ap.add_argument("--tag", default="base")
    ap.add_argument("--jump", type=float, default=60)
    a = ap.parse_args()
    root = Path(a.root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    shots = root / ("probe_" + a.tag)
    shots.mkdir(exist_ok=True)
    for old in shots.glob("*.png"):
        old.unlink()
    pad = root / "pad0.txt"
    pad.write_text("lx=128 ly=128 rx=128 ry=128\n")
    director = "god,log_pos=2" + ("," + a.director if a.director else "")
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", "1", "--headless", "0", "--party",
           "--fresh-saves", "--timestamps", "--idle", "0", "--seconds", str(a.seconds),
           "--root", str(root), "--build", a.build, "--label", "bloom probe " + a.tag]
    if a.game:
        cmd += ["--game", a.game]
    if a.fps:
        cmd += ["--fps", a.fps]
    for e in ["BB_PARTY_TEST_START_LAMP=" + a.lamp, "BB_TEST_REMO_SKIP_AFTER=5", "BB_PAD_FILE=" + str(pad),
              "BB_PARTY_DIRECTOR_TEST=" + director] + a.env:
        cmd += ["--env", e]
    env = dict(os.environ, BB_MP_LEASE=str(a.seconds + 300), BB_MP_PARENT_PID=str(os.getpid()))
    (root / "noreap").write_text(str(a.seconds + 120))
    print("[probe]", " ".join(cmd), flush=True)
    (root / "inst0" / "run.log").unlink(missing_ok=True)
    game = subprocess.Popen(cmd, env=env)
    # The game may wait for a slot of the shared queue: the shooter's clock starts with the world.
    log = root / "inst0" / "run.log"
    t_wait = time.time()
    while game.poll() is None and time.time() - t_wait < 1800:
        try:
            if "test: start lamp" in log.read_text(encoding="utf-8", errors="replace"):
                break
        except OSError:
            pass
        time.sleep(1)
    shooter = subprocess.Popen([sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(shots),
                                "--tag", a.tag, "--every", str(a.every), "--seconds", str(a.seconds + 60)])
    stop = threading.Event()
    th = threading.Thread(target=pad_sweep, args=(pad, a.pan_start, a.seconds, stop), daemon=True)
    th.start()
    game.wait()
    stop.set()
    shooter.terminate()
    rows, flashes = analyse(shots, a.jump)
    if rows:
        vals = [v for _, v in rows]
        print("[probe] %s: %d frames, mean brightness min %.1f max %.1f, white-out frames %d" %
              (a.tag, len(rows), min(vals), max(vals), len(flashes)))
        for n, v in flashes[:20]:
            print("[probe]   flash", n, "%.1f" % v)
    else:
        print("[probe] %s: no screenshots" % a.tag)
    return 0


if __name__ == "__main__":
    sys.exit(main())
