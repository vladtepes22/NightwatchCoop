#!/usr/bin/env python3
"""Central idle reaper for agents' test games (works whatever harness version an agent runs).

  python tools/mp/reaper.py [--idle 10]

Every 2 s, for each run holding game slots (tools/mp/game_queue.py), it reads the run's
<root>/inst*/run.log and counts the distinct "Party:"/"Net:" lines (numbers blanked, so periodic
status lines don't count). When that count has not grown for --idle seconds, the run has what it
can get: its run.bat shells and games are ended (the harness then sees them exit and finishes).
"""
import argparse, json, re, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from game_queue import _alive, _locked, _prune, QUEUE_FILE  # noqa: E402

g_QUEUE_DIR = QUEUE_FILE.parent

PARTY = re.compile(r"^(?:\[\+ *[\d.]+\] )?(Party|Net): ")
NUM = re.compile(r"0x[0-9a-fA-F]+|\d[\d.:]*")


def distinct_party_lines(root: Path) -> int:
    seen = set()
    for log in root.glob("inst*/run.log"):
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            if PARTY.match(line):
                seen.add(NUM.sub("#", line))
    return len(seen)


def all_in_world(root: Path) -> bool:
    """Every instance's latest director world line is "Party: world up" and its latest loading
    line is not "Party: loading screen up"."""
    logs = list(root.glob("inst*/run.log"))
    if not logs:
        return False
    for log in logs:
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return False
        worlds = re.findall(r"^(?:\[\+ *[\d.]+\] )?Party: (?:\[[^\]]*\] )?(world up|world down)\b", text, re.M)
        loads = re.findall(r"^(?:\[\+ *[\d.]+\] )?Party: (?:\[[^\]]*\] )?loading screen (up|gone)\b", text, re.M)
        if not worlds or worlds[-1] != "world up" or (loads and loads[-1] == "up"):
            return False
    return True


def end_run(root: Path):
    """Ends the run.bat shells and games under `root` (not the harness itself)."""
    r = str(root).replace("/", "\\").rstrip("\\").lower()
    ps = ("Get-CimInstance Win32_Process | Where-Object { "
          "($_.Name -in 'bbport.exe','cmd.exe') -and "
          f"((\"$($_.ExecutablePath)\".ToLower().StartsWith('{r}\\')) -or (\"$($_.CommandLine)\".ToLower().Contains('{r}\\'))) "
          "} | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; $_.ProcessId }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    return out.split()


def tile_windows():
    r"""Stack every visible test game window (bbport.exe under E:\games\bbport, not the user's game
    folder) four high in the left column, oldest process first, in physical pixels. Central, so
    agents running an older harness still get the same layout."""
    import ctypes
    from ctypes import wintypes
    u = ctypes.windll.user32
    try:
        u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except (AttributeError, OSError):
        u.SetProcessDPIAware()
    ps = (r"Get-Process bbport -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'E:\games\bbport\*' } | "
          "Sort-Object StartTime | ForEach-Object { $_.Id }")
    pids = [int(x) for x in subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                                           capture_output=True, text=True).stdout.split()]
    if not pids:
        return
    wins = {}
    EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, _):
        if u.IsWindowVisible(hwnd) and not u.GetWindow(hwnd, 4):  # GW_OWNER: top-level only
            pid = wintypes.DWORD()
            u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value in pids and u.GetWindowTextLengthW(hwnd) > 0:
                wins.setdefault(pid.value, hwnd)
        return True
    u.EnumWindows(EnumProc(cb), 0)

    class RECT(ctypes.Structure):
        _fields_ = [("l", ctypes.c_long), ("t", ctypes.c_long), ("r", ctypes.c_long), ("b", ctypes.c_long)]
    wa = RECT()
    u.SystemParametersInfoW(0x30, 0, ctypes.byref(wa), 0)
    rows = 4
    cell_h = (wa.b - wa.t) // rows
    for slot, pid in enumerate(p for p in pids if p in wins):
        col, row = divmod(slot, rows)
        hwnd = wins[pid]
        wr, cr = RECT(), RECT()
        u.GetWindowRect(hwnd, ctypes.byref(wr))
        u.GetClientRect(hwnd, ctypes.byref(cr))
        fw = (wr.r - wr.l) - (cr.r - cr.l)  # frame width/height around the client area
        fh = (wr.b - wr.t) - (cr.b - cr.t)
        h = cell_h - fh
        w = h * 16 // 9
        x, y = wa.l + col * (w + fw), wa.t + row * cell_h
        if (wr.l, wr.t, cr.r - cr.l, cr.b - cr.t) != (x, y, w, h):
            # SWP_NOZORDER | SWP_NOACTIVATE: never take the user's focus.
            u.SetWindowPos(hwnd, 0, x, y, w + fw, h + fh, 0x0004 | 0x0010)



AGENTS_PREFIX = "e:\\games\\bbport\\agents\\"  # test games only: never the user's own game


def _holder_prefixes(holders):
    """Path prefixes of the roots holding game slots, lower case with a trailing backslash. A root
    stored relative (an older game_queue.py copy, run from the agent's folder) matches as a path
    component anywhere, so its games are never taken for unqueued ones."""
    out = []
    for e in holders:
        r = str(e.get("root", "")).replace("/", "\\").rstrip("\\").lower()
        if not r:
            continue
        out.append((r + "\\", (len(r) > 2 and r[1] == ":") or r.startswith("\\\\")))
    return out


def plan_unqueued(procs, holders, state, now, grace=30.0):
    r"""The test games running outside the shared game queue, and what to end for each.

    procs: {pid: (name, ppid, exe_path, cmdline)} of bbport.exe and cmd.exe processes. A bbport.exe
    under E:\games\bbport\agents\ whose path is under no root holding game slots is unqueued (a
    harness that died: its instances.py's slot was pruned); after `grace` s it is ended together
    with its run.bat shell (the parent cmd.exe running run.bat): ending the game alone let run.bat's
    party crash restart (BB_PARTY_RESTART, up to 5 in 10 min) boot it again, and every restart got a
    new 30 s grace. Returns [(pid, path, [shell pids])]; `state` keeps "unq:PID" first-seen times."""
    prefixes = _holder_prefixes(holders)
    kills = []
    seen = set()
    for pid, (name, ppid, path, _cmd) in procs.items():
        if name.lower() != "bbport.exe":
            continue
        low = (path or "").replace("/", "\\").lower()
        if not low.startswith(AGENTS_PREFIX):
            continue  # never the user's own game (nor one whose path cannot be read)
        key = "unq:%s" % pid
        seen.add(key)
        if any(low.startswith(r) if absolute else ("\\" + r) in low for r, absolute in prefixes):
            state.pop(key, None)
            continue
        first = state.setdefault(key, now)
        if now - first <= grace:
            continue
        shells = []
        parent = procs.get(ppid)
        if parent and parent[0].lower() == "cmd.exe" and "run.bat" in (parent[3] or "").lower():
            shells.append(ppid)
        kills.append((pid, path, shells))
        state.pop(key, None)
    for key in [k for k in state if k.startswith("unq:") and k not in seen]:
        del state[key]  # gone by itself
    return kills


def list_test_procs():
    """{pid: (name, ppid, exe_path, cmdline)} of every bbport.exe and cmd.exe."""
    ps = (r"Get-CimInstance Win32_Process -Filter \"Name='bbport.exe' or Name='cmd.exe'\" | "
          r"ForEach-Object { $_.ProcessId.ToString() + '|' + $_.ParentProcessId + '|' + $_.Name + '|' + "
          r"$_.ExecutablePath + '|' + $_.CommandLine }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    procs = {}
    for line in out.splitlines():
        parts = line.split("|", 4)
        if len(parts) < 5 or not parts[0].strip().isdigit():
            continue
        pid, ppid, name, path, cmd = parts
        procs[int(pid)] = (name, int(ppid) if ppid.strip().isdigit() else 0, path, cmd)
    return procs


def stop_unqueued(holders, state, now):
    for pid, path, shells in plan_unqueued(list_test_procs(), holders, state, now):
        for sh in shells:  # the run.bat shell first: no crash restart of the game
            subprocess.run(["taskkill", "/PID", str(sh), "/F"], capture_output=True)
        subprocess.run(["taskkill", "/PID", str(pid), "/F", "/T"], capture_output=True)
        print(time.strftime("%H:%M:%S"), f"unqueued game ended: {path}"
              + (f" (and its run.bat shell {shells[0]})" if shells else ""), flush=True)


def orphaned_holders(holders, alive):
    """Slot holders whose harness died: a holder entry naming a "parent" pid (BB_MP_PARENT_PID, set
    by smoke_party.py for its instances.py) that is no longer alive. Its instances.py would keep
    the games running to their --seconds cap (up to 10 min) with the slot held."""
    return [e for e in holders if int(e.get("parent") or 0) > 0 and not alive(int(e["parent"]))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--idle", type=float, default=10)
    a = ap.parse_args()
    state = {}  # holder id -> (count, last change)
    while True:
        try:
            with _locked("game") as st:
                _prune(st)
                holders = [dict(e) for e in st["holders"]]
                # Fairness for 3-4 client tests (agents run their own, possibly old, copy of
                # game_queue.py, whose backfill lets 2-client tests jump the line forever): while a
                # waiting job of 3+ clients has waited over 60 s, cap the pool at 3 so no 2-client
                # job fits next to a running one; restore the normal 4 afterwards.
                def waited(e):
                    if "t" in e:
                        return time.time() - e["t"]
                    try:
                        h, m, sec = map(int, e.get("since", "").split(":"))
                        lt = time.localtime()
                        return (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec - (h * 3600 + m * 60 + sec)) % 86400
                    except ValueError:
                        return 0
                starved = any(e["count"] >= 3 and waited(e) > 60 for e in st["queue"])
                want = min(3, st.get("normal_max", 4)) if starved else st.get("normal_max", 4)
                if st.get("max_clients") != want:
                    st.setdefault("normal_max", 4)
                    st["max_clients"] = want
                    print(time.strftime("%H:%M:%S"), f"fairness: game pool max {want}"
                          + (" (a 3-4 client test is waiting)" if starved else ""), flush=True)
        except (SystemExit, OSError):
            holders = []
        now = time.time()
        live = set()
        # Blocked agents (block.txt next to the queue file, one root fragment per line): their runs
        # are ended at once - for agents relaunching a broken test before fixing it.
        try:
            blocked = [l.strip().lower() for l in (g_QUEUE_DIR / "block.txt").read_text().splitlines() if l.strip()]
        except OSError:
            blocked = []
        for e in list(holders):
            r = e["root"].replace("/", "\\").lower()
            if any(b in r for b in blocked):
                end_run(Path(e["root"]))
                subprocess.run(["taskkill", "/PID", str(e["pid"]), "/T", "/F"], capture_output=True)
                print(time.strftime("%H:%M:%S"), "blocked: stopped", e.get("label") or e["root"], flush=True)
                holders.remove(e)
        # Harness gone (smoke_party.py killed): its instances.py still holds the slots and runs
        # the games to their cap. End them now (instances.py also watches BB_MP_PARENT_PID itself).
        for e in orphaned_holders(holders, _alive):
            end_run(Path(e["root"]))
            subprocess.run(["taskkill", "/PID", str(e["pid"]), "/T", "/F"], capture_output=True)
            print(time.strftime("%H:%M:%S"), "harness gone: stopped", e.get("label") or e["root"], flush=True)
            holders.remove(e)
        # Gate: while agents/gate.txt exists only priority roots (priority.txt) may hold game slots;
        # end anything else (agents on an older queue that doesn't know the gate).
        qdir = g_QUEUE_DIR
        if (qdir / "gate.txt").exists():
            try:
                prio = [l.strip().lower() for l in (qdir / "priority.txt").read_text().splitlines() if l.strip()]
            except OSError:
                prio = []
            for e in holders:
                r = e["root"].replace("/", "\\").lower()
                if not any(f in r for f in prio):
                    end_run(Path(e["root"]))
                    subprocess.run(["taskkill", "/PID", str(e["pid"]), "/T", "/F"], capture_output=True)
                    print(time.strftime("%H:%M:%S"), "gate: stopped", e.get("label") or e["root"], flush=True)
            holders = [e for e in holders if any(f in e["root"].replace("/", "\\").lower() for f in prio)]
        # Required build features (require.txt next to the queue file: one regex per line that
        # every instance log must show within 120 s of the run starting, e.g. the fast-join patch).
        try:
            reqs = [re.compile(l.strip()) for l in (g_QUEUE_DIR / "require.txt").read_text().splitlines()
                    if l.strip()]
        except OSError:
            reqs = []
        for e in list(holders):
            if not reqs:
                break
            root = Path(e["root"])
            logs = list(root.glob("inst*/run.log"))
            started = state.get("start:" + e["id"]) or now
            state["start:" + e["id"]] = started
            if logs and now - started > 120:
                texts = []
                for lg in logs:
                    try:
                        texts.append(lg.read_text(encoding="utf-8", errors="replace"))
                    except OSError:
                        texts.append("")
                # Only party runs (patches.py logs "party co-op: ..."); solo runs have nothing to require.
                party = any("party co-op:" in t for t in texts)
                missing = [r.pattern for r in reqs if party and not all(r.search(t) for t in texts)]
                if missing:
                    end_run(root)
                    subprocess.run(["taskkill", "/PID", str(e["pid"]), "/T", "/F"], capture_output=True)
                    print(time.strftime("%H:%M:%S"), "require: stopped", e.get("label") or e["root"],
                          "- build lacks", missing, flush=True)
                    holders.remove(e)
        for e in holders:
            hid = e["id"]
            live.add(hid)
            root = Path(e["root"])
            # A run with a legitimately long quiet phase opts out with a file <root>/noreap
            # holding the number of seconds of quiet it needs.
            idle = a.idle
            try:
                idle = max(idle, float((root / "noreap").read_text().strip() or 600))
            except (OSError, ValueError):
                pass
            n = distinct_party_lines(root)
            # Only once every instance has reached its world: boots, title and loads are quiet
            # phases that must not end a run.
            # Not every slot's game has started yet (e.g. headless clients started late): a quiet
            # phase, not an idle run. Count only logs written since this run took its slots.
            run_start = e.get('t') or (now - 3600)
            started = sum(1 for lg in root.glob('inst*/run.log') if lg.stat().st_mtime >= run_start - 5)
            if started < e.get('count', 0) or not all_in_world(root):
                state[hid] = (n, now)
                # Stuck before the world (e.g. a first-run screen nobody presses through): no new
                # kind of party line for 120 s while not in the world ends the run too (the game
                # itself keeps logging frame stats, so log size proves nothing).
                pn, pn_since = state.get("pre:" + hid, (-1, now))
                if n != pn:
                    state["pre:" + hid] = (n, now)
                elif n > 0 and now - pn_since > max(120, idle):  # party runs only
                    pids = end_run(root)
                    print(time.strftime("%H:%M:%S"), "stuck before the world (no new party lines 120 s): ended",
                          e.get("label") or root, f"({len(pids)} processes)", flush=True)
                    state["pre:" + hid] = (n, now + 3600)
                continue
            count, since = state.get(hid, (-1, now))
            if n != count:
                state[hid] = (n, now)
            elif n > 0 and now - since > idle:
                pids = end_run(root)
                print(time.strftime("%H:%M:%S"), f"idle {idle:.0f}s: ended {e.get('label') or root} "
                      f"({len(pids)} processes)", flush=True)
                state[hid] = (n, now + 3600)  # don't repeat while the harness winds down
        # Games outside the queue: a test game (bbport.exe under an agents root) whose root holds no
        # game slot is ended after a 30 s grace (setup/teardown), so nothing runs unqueued.
        try:
            stop_unqueued(holders, state, now)
        except Exception as ex:  # never let this break the reaper
            print("unqueued:", ex, flush=True)
        try:
            tile_windows()
        except Exception as ex:  # never let layout break the reaper
            print("tile:", ex, flush=True)
        for hid in list(state):
            if not hid.startswith("unq:") and hid not in live and not (hid.split(":", 1)[-1] in live and ":" in hid):
                del state[hid]
        time.sleep(2)


if __name__ == "__main__":
    main()
