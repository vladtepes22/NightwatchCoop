"""Runs one visible, isolated game through a list of steps, then always stops it.

    python tools/visible-test.py <steps.json> <save SPRJ0005 dir> <maps dir> <game dir> [ini lines...]
    python tools/visible-test.py --check <steps.json>      validate the steps only, start nothing

<maps dir> holds atlas\\maps and baked. Steps are a JSON list, run in order; the first failing step
stops the run. Input goes through work\\winctl.ps1 (window-relative coordinates):
  {"key": "0x4D", "hold_ms": 60}          press a virtual key
  {"move": [x, y]}                         mouse to x, y
  {"wheel": 1}                             mouse wheel clicks (negative = down)
  {"drag": [x0, y0, x1, y1]}               drag inside the window
  {"sleep_ms": 500}
  {"wait_log": "Map: on", "timeout": 90}   wait until session.log contains the text
  {"shot": "name.png"}                     screenshot into the report folder
Report: work\\visible-test\\report\\results.json, the PNG shots and the game's logs. The game is
stopped (process tree) in a finally block, and the run fails if it is still alive afterwards.
Local test aid: work/ is not committed. Only for the map check under the 23 Oct exception.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DATA = REPO / "work" / "visible-test"
REPORT = DATA / "report"
WINCTL = REPO / "work" / "winctl.ps1"
KEYS = {"key", "move", "wheel", "drag", "sleep_ms", "wait_log", "shot"}


def check_steps(steps):
    """Return a list of problems; an empty list means the steps are valid."""
    problems = []
    if not isinstance(steps, list) or not steps:
        return ["steps must be a non-empty JSON list"]
    for i, step in enumerate(steps, 1):
        if not isinstance(step, dict) or len(KEYS & set(step)) != 1:
            problems.append(f"step {i}: needs exactly one of {sorted(KEYS)}: {step}")
            continue
        if "key" in step and not str(step["key"]).lower().startswith("0x"):
            problems.append(f"step {i}: key must be a hex virtual-key code such as 0x4D")
        if "move" in step and len(step["move"]) != 2:
            problems.append(f"step {i}: move needs [x, y]")
        if "drag" in step and len(step["drag"]) != 4:
            problems.append(f"step {i}: drag needs [x0, y0, x1, y1]")
        if "shot" in step and not str(step["shot"]).endswith(".png"):
            problems.append(f"step {i}: shot must end in .png")
    return problems


def prepare(save, maps, game, extra):
    """Copy the save into the isolated user folder and write the test config, as map_visible.py did."""
    user = DATA / "user"
    target = user / "savedata" / "1" / "CUSA00207" / "SPRJ0005"
    if not target.exists():
        shutil.copytree(save, target)
        sys_dir = save.parent / "SPRJ0005.sce_sys"
        if sys_dir.exists():
            shutil.copytree(sys_dir, target.parent / "SPRJ0005.sce_sys")
    config = DATA / "bbport.ini"
    config.write_text("\n".join(["upscaler=off", "output_res=1600x900", "live_resolution=0", "fullscreen=0",
                                 "skip_intro=1", "show_fps=0", "compile_note=0", *extra]) + "\n", encoding="utf-8")
    logs = DATA / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update(BB_AUDIO="none", BB_PREBUILT="1", BB_DATA_DIR=str(DATA), BB_CONFIG=str(config),
               BB_USER_DIR=str(user), BB_GAME_DIR=game, BB_PROBE=str(REPO / "engine" / "out" / "bb-probe.exe"),
               BB_FULLSCREEN="0", BB_FPS="60", BB_GPU_CACHE_DIR=str(user / "gpu-cache"),
               BB_LOG_FILE=str(logs / "session.log"), BB_LAMP_TRAVEL_TEST=os.environ.get("MAP_TEST", "continue"),
               BB_MAP_DIR=str(maps / "baked"), BB_MAP_DATA=str(maps / "atlas" / "maps"),
               BB_MAP_PARCHMENT=str(REPO / "work" / "map-generator" / "parchment.bbra"), PYTHONUTF8="1")
    env["PATH"] = r"C:\msys64\clang64\bin;" + env["PATH"]
    for key in [k for k in env if k.startswith("BB_PARTY")]:
        del env[key]
    return env, logs


def winctl(*args):
    """Run one input action; returns an error string or None."""
    result = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(WINCTL), *map(str, args)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return (result.stderr or result.stdout).strip().splitlines()[-1:] or ["winctl failed"]
    return None


def run_step(step, n, log_file):
    """Run one step; returns (ok, detail)."""
    if "key" in step:
        err = winctl("key", step["key"], step.get("hold_ms", 60))
    elif "move" in step:
        err = winctl("move", *step["move"])
    elif "wheel" in step:
        err = winctl("wheel", step["wheel"])
    elif "drag" in step:
        err = winctl("drag", *step["drag"])
    elif "sleep_ms" in step:
        time.sleep(step["sleep_ms"] / 1000)
        return True, "slept"
    elif "wait_log" in step:
        deadline = time.time() + step.get("timeout", 60)
        while time.time() < deadline:
            if log_file.exists() and step["wait_log"] in log_file.read_text(encoding="utf-8", errors="replace"):
                return True, f"found '{step['wait_log']}'"
            time.sleep(0.5)
        return False, f"'{step['wait_log']}' not in session.log after {step.get('timeout', 60)} s"
    else:
        name = REPORT / step["shot"]
        err = winctl("shot", name)
        if err is None and not name.exists():
            err = ["no screenshot written"]
    return (err is None), (err[0] if err else "done")


def stop_game(proc):
    """Stop the launcher and everything it started; True only if the root process is gone."""
    subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"], capture_output=True)
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        return False
    return proc.poll() is not None


def main():
    args = sys.argv[1:]
    if args[:1] == ["--check"]:
        steps = json.loads(Path(args[1]).read_text(encoding="utf-8"))
        problems = check_steps(steps)
        print("\n".join(problems) or f"{len(steps)} steps, valid")
        return 1 if problems else 0

    steps = json.loads(Path(args[0]).read_text(encoding="utf-8"))
    problems = check_steps(steps)
    if problems:
        print("\n".join(problems))
        return 1
    save, maps, game, extra = Path(args[1]), Path(args[2]), args[3], args[4:]
    REPORT.mkdir(parents=True, exist_ok=True)
    for old in REPORT.glob("*.png"):
        old.unlink()
    env, logs = prepare(save, maps, game, extra)
    log_file = logs / "session.log"
    out = (logs / "start.log").open("w", encoding="utf-8")
    proc = subprocess.Popen([str(REPO / "engine" / "out" / "bb-start.exe"), "--game-dir", game], env=env,
                            stdout=out, stderr=out, cwd=REPO / "engine")
    results = []
    passed = True
    try:
        time.sleep(3)  # let the window appear before the first input
        for n, step in enumerate(steps, 1):
            ok, detail = run_step(step, n, log_file)
            results.append({"step": n, "action": step, "ok": ok, "detail": detail})
            print(f"{n:2d} {'PASS' if ok else 'FAIL'} {detail}")
            if not ok:
                passed = False
                break
    finally:
        stopped = stop_game(proc)
        if not stopped:
            passed = False
            print(f"game PID {proc.pid} is still running: stop it with taskkill /PID {proc.pid} /T /F")
        out.close()
        (REPORT / "results.json").write_text(json.dumps({"passed": passed and stopped, "stopped": stopped,
                                                         "steps": results}, indent=1), encoding="utf-8")
    print("PASS" if passed and stopped else "FAIL", "| report:", REPORT)
    return 0 if passed and stopped else 1


if __name__ == "__main__":
    sys.exit(main())
