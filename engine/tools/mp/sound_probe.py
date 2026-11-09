#!/usr/bin/env python3
"""Sound census probe (rank 14c, CL-58d4cc): one game, the frame-rate sweep (BB_FRAME_SWEEP) at several rates in
ONE session, with the sound thread's passes and the sounds started counted per test (BB_SOUND_CENSUS=1;
bbport_frame_sweep.cpp logs "sound idle / 10 swings / run" lines). The game quits after the sweep.

  python tools/mp/sound_probe.py --root E:\...\snd1 [--rates 30,60,90,144] [--wake] [--seconds 1500]

--wake: BB_SOUND_WAKE_PER_FRAME=1 (the sound thread woken once a frame) instead of the game's own sleep.
Prints the "Frame sweep: ... sound" lines and the census lines of the run. No test saves are used (fresh
saves, the Cainhurst maid lamp warp), the game is muted (BB_AUDIO=none would stop the sound thread: not set;
the port's audio runs with the window in the background).
"""
import argparse, os, re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", ""))
    ap.add_argument("--rates", default="30,60,90,144")
    ap.add_argument("--seconds", type=int, default=1500)
    ap.add_argument("--lamp", default="2502950")
    ap.add_argument("--wake", action="store_true")
    ap.add_argument("--start-fps", default="60")
    ap.add_argument("--env", action="append", default=[])
    a = ap.parse_args()
    root = Path(a.root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    pad = root / "pad0.txt"
    pad.write_text("")
    env_list = ["BB_PARTY_TEST_START_LAMP=" + a.lamp, "BB_PAD_FILE=" + str(pad), "BB_PARTY_DIRECTOR_TEST_SOLO=1",
                "BB_PARTY_DIRECTOR_TEST=god", "BB_FRAME_SWEEP=" + a.rates, "BB_FRAME_SWEEP_WAIT=240",
                "BB_SOUND_CENSUS=1", "BB_LIVE_FPS=0", "BB_WINDOW_BACKGROUND=1"]
    if a.wake:
        env_list.append("BB_SOUND_WAKE_PER_FRAME=1")
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", "1", "--headless", "0", "--party",
           "--fresh-saves", "--timestamps", "--idle", "0", "--seconds", str(a.seconds), "--fps", a.start_fps,
           "--until", r"Frame sweep: done", "--root", str(root), "--build", a.build,
           "--label", "sound probe" + (" wake" if a.wake else "")]
    if a.game:
        cmd += ["--game", a.game]
    for e in env_list + a.env:
        cmd += ["--env", e]
    envp = dict(os.environ, BB_MP_LEASE=str(a.seconds + 300), BB_MP_PARENT_PID=str(os.getpid()))
    (root / "noreap").write_text(str(a.seconds + 120))
    rc = subprocess.call(cmd, env=envp)
    log = root / "inst0" / "run.log"
    try:
        text = log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        print("[probe] no log")
        return rc or 1
    for line in text.splitlines():
        if re.search(r"Frame sweep: .*(sound|timing)|Sound census|Sound thread|Frame sweep: done|STOP|crash", line):
            print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main())
