#!/usr/bin/env python3
"""Local multiplayer test harness: runs several game instances on one PC.

Each instance gets its own folder (hard links to the build's run.bat, scripts, patches and
out\\*.exe/*.dll, so no copies), its own user\\ (saves, shader cache, add-on folder), its own
bbport.ini (720p, upscaler off) and its own log. Instances find each other over loopback.

  python tools/mp/instances.py setup --count 2 [--root DIR] [--build DIR] [--saves DIR]
  python tools/mp/instances.py run --count 2 [--seconds 120] [--party] [--env KEY=VALUE ...]
  python tools/mp/instances.py crash --instance 1 [--after 0]
  python tools/mp/instances.py logs --count 2 [--grep REGEX]
  python tools/mp/instances.py kill

Instance i (0 = host) gets BB_MP_INSTANCE=i and BB_MP_PORT=base+i, which the party code reads
when BB_MP_LOCAL_TEST=1. Windows are placed side by side.

--party: a loopback party. Instance 0 hosts (BB_PARTY=host), the others join (BB_PARTY=join) with
the code the host writes to ROOT/party_code.txt (BB_PARTY_CODE_FILE, deleted before the run);
BB_PARTY_PORT=base+i, BB_PARTY_NAME=Hunter{i}, no UPnP, no public STUN, 127.0.0.1 everywhere.
--env still overrides any of these; --ienv I:KEY=VALUE sets a variable for instance I only (e.g. a
test action on the guest). --crash-at S kills instance --crash-instance's bbport.exe
(TerminateProcess, as a crash) S seconds into the run; run.bat then restarts it (BB_PARTY_RESTART).
--crash-plan I[/W]:REGEX[@D] (repeatable, in order): several crashes in one run; each kills instance
I's game D s (default 0) after instance W's log (default I) matched REGEX, looking only at what was
logged after the previous crash (e.g. the host crashes during a travel, then the guest crashes during
the restarted host's death load: 0:step 5/5@2 then 1/0:host_die@3).
"""
import argparse, ctypes, json, os, re, shutil, subprocess, sys, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_ROOT = REPO.parent / "mptest"
DEFAULT_GAME = Path(os.environ.get("BB_GAME_DIR", r"E:\games\bloodborne"))
BASE_PORT = 47600  # default root; see base_port()


def base_port(root) -> int:
    """Each harness root gets its own port range, so agents' parallel runs (tools/agent/agent_env.py
    gives each its own root) never collide: BB_MP_BASE_PORT, else 47600 for the default root, else
    47700 + 10 * (a stable hash of the root path % 150)."""
    if os.environ.get("BB_MP_BASE_PORT"):
        return int(os.environ["BB_MP_BASE_PORT"])
    r = Path(root).resolve()
    # Only the main checkout's default root keeps 47600. An agent worktree's default root
    # (agents/NAME/mptest) is its own folder, but every agent got 47600 too (2026-10-15: two
    # agents' hosts on one port; the second host's guests never found its party code).
    if r == Path(DEFAULT_ROOT).resolve() and REPO.parent.parent.name != "agents":
        return BASE_PORT
    import zlib
    return 47700 + 10 * (zlib.crc32(str(r).lower().encode()) % 150)
# Up to 4 test games share one GPU and RAM: lowest-cost settings (720p, no upscaler, every optional
# effect off, lowest model detail). Agents read screenshots, so 720p stays.
INI = ("upscaler=off\npreset=0\noutput_res=1280x720\nlive_resolution=0\nshow_fps=1\n"
       "skip_intro=1\nmodel_lod=2\neffect_chromatic_aberration=0\neffect_dof=0\neffect_motion_blur=0\n"
       "effect_ssao=0\neffect_game_aa=0\neffect_dynamic_shadows=0\neffect_ssr=0\n")


def link_tree(src: Path, dst: Path):
    for root, dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        dirs[:] = [d for d in dirs if d != "__pycache__"]
        for f in files:
            target = dst / rel / f
            if target.exists():
                target.unlink()
            try:
                os.link(Path(root) / f, target)
            except OSError:
                shutil.copy2(Path(root) / f, target)


def builds_running():
    """Names of running ninja/ld processes (a build may be rewriting out\\bbport.exe)."""
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout.lower()
    return sorted({n for n in ("ninja.exe", "ld.exe", "lto-wrapper.exe") if f'"{n}"' in out})


def wait_for_builds(build=None, limit=600):
    """Waits until the build's own out\bbport.exe is a complete PE file that has not changed for
    5 s (other worktrees' builds - other agents - do not matter). Without `build`: until no
    ninja/ld runs at all (the old behaviour)."""
    t0 = time.time()
    if build is None:
        while (running := builds_running()) and time.time() - t0 < limit:
            print("waiting for the build to finish:", ", ".join(running), flush=True)
            time.sleep(10)
        if builds_running():
            sys.exit("a build is still running after %d s; not copying the executables" % limit)
        return
    exe = Path(build) / "out" / "bbport.exe"

    def stamp():
        try:
            st = exe.stat()
            return (st.st_size, st.st_mtime_ns)
        except OSError:
            return None

    while time.time() - t0 < limit:
        a = stamp()
        if a and time.time() - a[1] / 1e9 > 5 and exe_ok(exe, 1 << 20):
            time.sleep(2)
            if stamp() == a:
                return
        print("waiting for", exe, "to be complete and stable", flush=True)
        time.sleep(5)
    sys.exit("%s is not complete after %d s; not copying the executables" % (exe, limit))


def exe_ok(path: Path, min_size=1):
    """A complete PE file: non-empty (at least min_size bytes) and starting with MZ."""
    try:
        if path.stat().st_size < max(min_size, 2):
            return False
        with open(path, "rb") as f:
            return f.read(2) == b"MZ"
    except OSError:
        return False


def min_size_for(name: str):
    return (1 << 20) if name.lower() == "bbport.exe" else 1


def copy_exe(src: Path, dst: Path, timeout=60):
    """Copies an exe to a temporary name, checks it (size equals the source's, MZ, bbport.exe
    > 1 MB) and moves it into place; retries while the source is being written."""
    tmp = dst.with_name(dst.name + ".copying")
    t0 = time.time()
    while True:
        try:
            size = src.stat().st_size
            shutil.copy2(src, tmp)
            if (size > 0 and tmp.stat().st_size == size == src.stat().st_size
                    and exe_ok(tmp, min_size_for(src.name))):
                os.replace(tmp, dst)
                return
        except OSError:
            pass
        if time.time() - t0 > timeout:
            if tmp.exists():
                tmp.unlink()
            sys.exit(f"{src} is not a complete executable after {timeout} s (a build writing it?); not copied")
        time.sleep(2)


def validate_instances(root, count):
    """Every exe/dll in each instance's out\\ must be a complete PE file; never launch otherwise."""
    bad = []
    for i in range(count):
        out = Path(root) / f"inst{i}" / "out"
        if not exe_ok(out / "bbport.exe", 1 << 20):
            bad.append(str(out / "bbport.exe"))
        for f in out.glob("*.dll"):
            if not exe_ok(f):
                bad.append(str(f))
    return bad


def snapshot_exes(build: Path, root: Path):
    """One checked copy of the build's executables per setup (ROOT\\exe): the instances hard-link
    it. bbport.exe is ~360 MB; a copy per instance was most of setup's time and disk writes. A copy
    (not a link to the build's own file): a rebuild rewrites that in place."""
    snap = root / "exe"
    snap.mkdir(parents=True, exist_ok=True)
    for f in (build / "out").iterdir():
        if f.suffix.lower() == ".exe":
            t = snap / f.name
            try:
                same = t.stat().st_size == f.stat().st_size and t.stat().st_mtime_ns == f.stat().st_mtime_ns
            except OSError:
                same = False
            if not same:
                # Never rewrite the snapshot in place: running games may hold links to it.
                if t.exists():
                    try:
                        t.unlink()
                    except OSError:
                        t.rename(t.with_name("%s.old%d" % (t.name, time.time())))
                copy_exe(f, t)
    return snap


def setup(a):
    build = Path(a.build)
    wait_for_builds(build)
    t0 = time.time()
    snap = snapshot_exes(build, Path(a.root))
    for i in range(a.count):
        inst = Path(a.root) / f"inst{i}"
        (inst / "out").mkdir(parents=True, exist_ok=True)
        for name in ("run.bat", "launcher.py"):
            if (build / name).exists():
                shutil.copy2(build / name, inst / name)
        for sub in ("scripts", "patches"):
            link_tree(build / sub, inst / sub)
        for f in (build / "out").iterdir():
            if f.suffix.lower() in (".exe", ".dll"):
                t = inst / "out" / f.name
                src = snap / f.name if f.suffix.lower() == ".exe" else f
                if t.exists():
                    t.unlink()
                try:
                    os.link(src, t)
                except OSError:
                    shutil.copy2(src, t)
        user = inst / "user"
        user.mkdir(exist_ok=True)
        if a.saves and Path(a.saves).is_dir() and not (user / "savedata").exists():
            shutil.copytree(a.saves, user / "savedata")
        (inst / "bbport.ini").write_text(INI, encoding="utf-8")
        print("instance", i, "->", inst)


def party_saves_from(user: Path, fresh: bool = False):
    r"""A party session always plays from user\savedata_party (src/runtime_savepolicy.c): the test
    saves are copied there too the first time, so --party runs keep their characters. fresh
    (--fresh-saves): copied again for every run, so a run never inherits the last run's state (a
    guest dropped in the host's world saved its spot there and spawned outside the map next time).
    Only the instance's own test copy under the run root is touched."""
    if fresh and (user / "savedata").is_dir():
        if (user / "savedata_party").exists():
            shutil.rmtree(user / "savedata_party")
        # the last run's crash marker, story state, rot stacks and prologue reading: they belong to
        # the characters of the save copied again here
        for state in ("party_state.json", "party_story.json", "party_rot.txt", "party_prologue.txt", "party_stain.txt"):
            (user / state).unlink(missing_ok=True)
    if (user / "savedata").is_dir() and not (user / "savedata_party").exists():
        shutil.copytree(user / "savedata", user / "savedata_party")


PID_FILE = "harness_pids.json"  # the run.bat shells of the last run (stop_leftovers)


def list_processes():
    """(pid, parent pid, exe name) of every process (Toolhelp snapshot; Windows only)."""
    from ctypes import wintypes

    class PE(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                    ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]
    k32 = ctypes.windll.kernel32
    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    out = []
    if not snap or snap == wintypes.HANDLE(-1).value:
        return out
    try:
        e = PE()
        e.dwSize = ctypes.sizeof(PE)
        ok = k32.Process32FirstW(wintypes.HANDLE(snap), ctypes.byref(e))
        while ok:
            out.append((e.th32ProcessID, e.th32ParentProcessID, e.szExeFile))
            ok = k32.Process32NextW(wintypes.HANDLE(snap), ctypes.byref(e))
    finally:
        k32.CloseHandle(wintypes.HANDLE(snap))
    return out


def _open_query(pid):
    k32 = ctypes.windll.kernel32
    k32.OpenProcess.restype = ctypes.c_void_p
    return k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION


def process_path(pid) -> str:
    h = _open_query(pid)
    if not h:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        n = ctypes.c_ulong(1024)
        ok = ctypes.windll.kernel32.QueryFullProcessImageNameW(ctypes.c_void_p(h), 0, buf, ctypes.byref(n))
        return buf.value if ok else ""
    finally:
        ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(h))


def process_created(pid) -> int:
    """The process's creation time (FILETIME as an integer; 0 if unknown): tells a recorded PID
    from a later process that reuses it."""
    h = _open_query(pid)
    if not h:
        return 0
    try:
        t = [ctypes.c_ulonglong() for _ in range(4)]
        ok = ctypes.windll.kernel32.GetProcessTimes(ctypes.c_void_p(h), *(ctypes.byref(x) for x in t))
        return t[0].value if ok else 0
    finally:
        ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(h))


def stop_leftovers(root):
    """Ends what an earlier run of this root left behind (the harness killed by a timeout leaves
    run.bat's restart loop and the game running): its game would keep writing into the new
    run.log, and its restart loop would start the game again."""
    if os.name != "nt":
        return
    wants = {str(Path(root)).lower(), str(Path(root).resolve()).lower()}
    pid_file = Path(root) / PID_FILE
    if pid_file.exists():
        # Fast path (no PowerShell, ~0.9 s): the run.bat shells the last run recorded, and every
        # game process whose executable lies under this root.
        try:
            recorded = json.loads(pid_file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            recorded = []
        procs = list_processes()
        for pid, _ppid, name in procs:
            if pid == os.getpid():
                continue
            hit = False
            low = name.lower()
            if low == "cmd.exe":
                hit = any(r.get("pid") == pid and r.get("created") == process_created(pid) for r in recorded)
            elif low.startswith(("bbport", "bb-probe")):
                path = process_path(pid).lower()
                hit = any(path.startswith(w + os.sep) for w in wants)
            if hit:
                subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
                print(f"stopped a leftover {name} ({pid}) of an earlier run", flush=True)
        return
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='cmd.exe' or Name like 'bbport%' or Name like 'bb-probe%'\" | "
          "ForEach-Object { $_.ProcessId.ToString() + '|' + $_.Name + '|' + $_.ExecutablePath + '|' + $_.CommandLine }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    for line in out.splitlines():
        pid, _, rest = line.partition("|")
        name, _, rest = rest.partition("|")
        path, _, cmdline = rest.partition("|")
        if name.lower() == "cmd.exe":
            hit = any(w + os.sep + "inst" in cmdline.lower() for w in wants) and "run.bat" in cmdline.lower()
        else:
            hit = any(path.lower().startswith(w + os.sep) for w in wants)
        if hit and pid.strip().isdigit() and int(pid) != os.getpid():
            subprocess.run(["taskkill", "/PID", pid.strip(), "/T", "/F"], capture_output=True)
            print(f"stopped a leftover {name} ({pid}) of an earlier run", flush=True)


def open_log(path: Path):
    """Opens an instance's run.log for its run. The previous one is kept as run.prev.log. The handle
    the children inherit may only append (FILE_APPEND_DATA without FILE_WRITE_DATA): run.bat, its
    restart loop's games and the Python helpers all write at the current end, and nothing can
    rewrite or truncate earlier output - a writer's stale offset after someone truncated the file
    used to leave the log's beginning as NUL bytes. Returns (file, offset this run starts at)."""
    if path.exists():
        try:
            os.replace(path, path.with_name("run.prev.log"))
        except OSError:
            pass  # still open somewhere: append to it
    if os.name == "nt":
        import msvcrt
        from ctypes import wintypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateFileW.restype = wintypes.HANDLE
        k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        FILE_APPEND_DATA, SYNCHRONIZE, FILE_READ_ATTRIBUTES = 0x4, 0x100000, 0x80
        h = k32.CreateFileW(str(path), FILE_APPEND_DATA | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                            7, None, 4, 0x80, None)  # share read/write/delete, OPEN_ALWAYS, normal
        if h and h != wintypes.HANDLE(-1).value:
            f = os.fdopen(msvcrt.open_osfhandle(h, os.O_APPEND), "ab", buffering=0)
            return f, os.path.getsize(path)
    f = open(path, "ab")
    return f, f.tell()


def tile_rect(slot):
    """Screen rectangle for a game window by its machine-wide slot: slots 0-3 stacked top to bottom
    in the left column (then the next column), each a quarter of the primary work area's height,
    16:9. Physical pixels: the game's window positions are DPI-unaware of Windows scaling."""
    if os.name != "nt":
        return None
    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2
    except (AttributeError, OSError):
        ctypes.windll.user32.SetProcessDPIAware()
    class RECT(ctypes.Structure):
        _fields_ = [("l", ctypes.c_long), ("t", ctypes.c_long), ("r", ctypes.c_long), ("b", ctypes.c_long)]
    wa = RECT()
    ctypes.windll.user32.SystemParametersInfoW(0x30, 0, ctypes.byref(wa), 0)  # SPI_GETWORKAREA
    rows = 4
    frame = 40  # title bar and borders of each window
    h = (wa.b - wa.t) // rows - frame
    w = h * 16 // 9
    col, row = divmod(slot, rows)
    return wa.l + 8 + col * (w + 16), wa.t + row * (h + frame) + 31, w, h


def stamp_lines(src, log, t0):
    """--timestamps: copies a child's output into its log, each line prefixed "[+SSS.mmm] "."""
    try:
        for raw in iter(src.readline, b""):
            log.write(b"[+%8.3f] " % (time.time() - t0) + raw)
    except (OSError, ValueError):
        pass


def run(a):
    t_slot = time.time()
    # A rebuild since the last setup: copy it first, or the run would test the old exe.
    built = Path(a.build) / "out" / "bbport.exe"
    # Every instance of this run (parx 2026-10-15: a 3-player run after 2-player runs had a stale
    # inst2 exe from an older build; the host refused it as another bbport build).
    exes = [Path(a.root) / f"inst{i}" / "out" / "bbport.exe" for i in range(a.count)]
    if built.exists() and any(not e.exists() or built.stat().st_mtime > e.stat().st_mtime + 1 for e in exes):
        print("the build is newer than the instances' exe: running setup first", flush=True)
        setup(a)
    port0 = base_port(a.root)
    stop_leftovers(a.root)
    user32 = ctypes.windll.user32 if os.name == "nt" else None
    bad = validate_instances(a.root, a.count)
    if bad:
        print("not launching: incomplete executables (run setup again once no build runs):", *bad, sep="\n  ")
        sys.exit(1)
    if os.name == "nt":
        # No error dialogs on the desktop from the children (inherited error mode): log-only tests.
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    procs = []
    ts0 = time.time()
    print("[harness] %.1f s from the game slot to the first start (setup, leftovers check)" % (ts0 - t_slot), flush=True)
    starts = [0] * a.count  # where this run begins in each run.log
    code_file = Path(a.root) / "party_code.txt"
    if a.party and code_file.exists():
        code_file.unlink()  # a stale code (old secret) would only get the guests rejected
    for i in range(a.count):
        inst = Path(a.root) / f"inst{i}"
        env = os.environ.copy()
        env.update({
            "BB_GAME_DIR": str(a.game), "BB_FROM_LAUNCHER": "1", "BB_NO_LAUNCHER_SETTINGS": "1",
            "BB_FULLSCREEN": "0", "BB_FPS": str(a.fps), "BB_TIMEOUT": str(a.seconds), "BB_VRAM_LIMIT_MB": str(a.vram),
            "BB_SHADER_PRECOMPILE": "1", "BB_MUTE_UNFOCUSED": "1", "BB_WINDOW_BACKGROUND": "1",
            "BB_AUDIO": "none",  # test games are silent (timer audio backend, also less CPU)
            "BB_DLSS": "0",  # no DLSS probe or library in test games (upscaler is off anyway)
            # One boot image for every instance (scripts/boot_image.py), hard-linked into each instance
            "BB_BOOT_CACHE_DIR": str(Path(a.root) / "bootcache"),
            # Fewer worker threads per test game: 4 games share one CPU.
            "BB_SHADER_THREADS": "2", "BB_ASYNC_PIPELINE_THREADS": "2", "BB_PREP_WORKERS": "1",
            "BB_PATCHES": "Skip Online/Offline Choice;Skip Intro",
            "BB_MP_LOCAL_TEST": "1", "BB_MP_INSTANCE": str(i), "BB_MP_PORT": str(port0 + i),
            "BB_MP_NAME": f"Hunter{i}",
            "BB_WINDOW_TITLE": f"[{(getattr(a, 'slots', None) or list(range(a.count)))[i]}] "
                               f"{(a.label or Path(a.root).parent.name)[:40]} i{i}"
                               f"{('h' if i == 0 else 'g') if a.party else ''}",
            **({"BB_WINDOW_X": str(r[0]), "BB_WINDOW_Y": str(r[1]), "BB_WINDOW_W": str(r[2]), "BB_WINDOW_H": str(r[3])}
               if (r := tile_rect(getattr(a, "slots", list(range(a.count)))[i])) else {}),
        })
        if a.party:
            party_saves_from(inst / "user", getattr(a, "fresh_saves", False))
            env.update({
                "BB_PARTY": "host" if i == 0 else "join", "BB_PARTY_AUTOCONTINUE": "1", "BB_PARTY_CODE_FILE": str(code_file),
                "BB_PARTY_PORT": str(port0 + i), "BB_PARTY_NAME": f"Hunter{i}", "BB_PARTY_UPNP": "0",
                "BB_PARTY_STUN": "off", "BB_PARTY_LOOPBACK": "1", "BB_PARTY_LOCAL_IP": "127.0.0.1",
                "BB_SKIP_NETWORK_CHOICE": "online", "BB_PATCHES": "Skip Intro",
            })
        if i >= a.count - getattr(a, "headless", 0):
            # Headless-lite (gpu/shim/bbport_headless.h): minimised, nothing drawn or presented,
            # game logic and the party code as usual. Its window gives no screenshots.
            # Less VRAM: nothing is drawn (5 instances x 1792 MiB overcommitted an 8 GB GPU and the
            # last one's first frame stalled: "Stall during rendering at flush 1", 2026-10-12).
            env.update({"BB_HEADLESS": "1", "BB_DLSS": "0", "BB_UPSCALER": "off",
                        "BB_VRAM_LIMIT_MB": str(min(int(a.vram), 1024)),
                        "BB_WINDOW_TITLE": env["BB_WINDOW_TITLE"] + " [headless]"})
        for kv in a.env or []:
            k, _, v = kv.partition("=")
            env[k] = v.replace("{i}", str(i)).replace("{port}", str(port0 + i))
        for ikv in a.ienv or []:  # I:KEY=VALUE, instance I only
            who, _, kv = ikv.partition(":")
            if who.strip() == str(i):
                k, _, v = kv.partition("=")
                env[k] = v.replace("{i}", str(i)).replace("{port}", str(port0 + i))
        if (getattr(a, "headless_delay_after_join", 0) and a.party and i > 0 and
                i == a.count - getattr(a, "headless", 0)):
            # The headless instances start once the first guest has joined (the host's
            # "cooperators 0 -> 1") plus the delay: the first join and load are not slowed by them.
            host_log = Path(a.root) / "inst0" / "run.log"
            deadline = time.time() + 300
            while time.time() < deadline and any(q.poll() is None for _, q, _ in procs):
                try:
                    if "cooperators 0 -> 1" in host_log.read_text(encoding="utf-8", errors="replace"):
                        break
                except OSError:
                    pass
                time.sleep(2)
            print("[harness +%.1f s] the first guest joined (or 300 s passed): headless instances in %.0f s"
                  % (time.time() - ts0, a.headless_delay_after_join), flush=True)
            time.sleep(a.headless_delay_after_join)
        if getattr(a, "join_sequential", False) and a.party and i >= 2:
            # One guest at a time: guest i starts once guest i-1 has joined (the host's
            # "cooperators i-2 -> i-1"), at least --stagger s after the last start; 300 s at most.
            host_log = Path(a.root) / "inst0" / "run.log"
            want = f"cooperators {i - 2} -> {i - 1}"
            deadline = time.time() + 300
            while time.time() < deadline and any(q.poll() is None for _, q, _ in procs):
                try:
                    if want in host_log.read_text(encoding="utf-8", errors="replace"):
                        break
                except OSError:
                    pass
                time.sleep(2)
            print("[harness +%.1f s] %s: starting instance %d" % (time.time() - ts0, want, i), flush=True)
        log, starts[i] = open_log(inst / "run.log")
        # Test games run ABOVE_NORMAL (children inherit) so agents' builds never starve them.
        prio = 0x00008000 if os.name == "nt" else 0
        if a.timestamps:
            # Every line gets "[+SSS.mmm] " (seconds since the harness started the first
            # instance, the same clock for all instances): join / summon timelines.
            p = subprocess.Popen(["cmd", "/c", str(inst / "run.bat")], cwd=inst, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, creationflags=prio)
            threading.Thread(target=stamp_lines, args=(p.stdout, log, ts0), daemon=True).start()
        else:
            p = subprocess.Popen(["cmd", "/c", str(inst / "run.bat")], cwd=inst, env=env,
                                 stdout=log, stderr=subprocess.STDOUT, creationflags=prio)
        procs.append((i, p, log))
        print("[harness +%.1f s] instance %d started" % (time.time() - ts0, i), flush=True)
        if os.name == "nt":
            try:
                (Path(a.root) / PID_FILE).write_text(json.dumps(
                    [{"pid": q.pid, "created": process_created(q.pid)} for _, q, _ in procs]), encoding="utf-8")
            except OSError:
                pass
        if i + 1 < a.count:
            time.sleep(a.stagger)
    # The run's clock (--crash-at, --seconds, the lease) starts one stagger after the last start,
    # as before the stagger was skipped after the last instance.
    t0 = time.time() + (a.stagger if a.count > 1 else 0)
    # The crash plan: (instance, watched log, regex or None, at s or None, delay s), run in order.
    plan = []
    if a.crash_at or a.crash_when:
        plan.append((a.crash_instance, a.crash_instance, re.compile(a.crash_when) if a.crash_when else None,
                     a.crash_at or None, 0.0))
    for item in a.crash_plan or []:
        who, _, rest = item.partition(":")
        inst, _, watch = who.partition("/")
        rx, _, delay = rest.rpartition("@") if re.search(r"@[\d.]+$", rest) else (rest, "", "0")
        plan.append((int(inst), int(watch or inst), re.compile(rx), None, float(delay or 0)))
    crashed = False
    want_crash = bool(plan)
    plan_next, plan_hit = 0, None
    plan_marks = list(starts)
    # --until RX (any log) or I:RX (instance I's log only, e.g. "2:the host ended the party").
    until = []
    for u in a.until or []:
        m = re.match(r"^(\d):(.*)$", u)
        until.append((int(m.group(1)), re.compile(m.group(2))) if m else (None, re.compile(u)))
    marks = list(starts)  # log offsets at the crash: --until then looks at what follows it
    verdict = None

    def log_text(i, start=0):
        try:
            with open(Path(a.root) / f"inst{i}" / "run.log", "rb") as f:
                f.seek(start)
                return f.read().decode("utf-8", errors="replace")
        except OSError:
            return ""

    # Lease: the machine has 2 game slots for every agent (tools/mp/game_queue.py). Once this run
    # is past its lease and someone is waiting, it ends (a run should stop at its --until markers
    # anyway; idle games after the interesting part only hold the slots).
    lease = float(os.environ.get("BB_MP_LEASE", "240"))
    last_lease_check = 0.0
    # Idle stop: once a party run has done something and then no NEW kind of party log line appears
    # for --idle seconds (periodic status lines don't count), the run has what it can get: end it.
    idle_rx = re.compile(r"^(Party|Net): ")
    idle_limit = a.idle
    last_party_count, last_party_change = -1, time.time()
    while any(p.poll() is None for _, p, _ in procs) and time.time() - t0 < a.seconds + 90:
        if time.time() - t0 > lease and time.time() - last_lease_check > 5:
            last_lease_check = time.time()
            try:
                from game_queue import _locked, _prune
                with _locked("game") as st:
                    _prune(st)
                    waiting = len(st["queue"])
            except Exception:
                waiting = 0
            if waiting:
                verdict = verdict or ("LEASE EXPIRED after %.0f s: %d run(s) waiting for the game slots"
                                      % (time.time() - t0, waiting))
                print(verdict, flush=True)
                break
        if plan_next < len(plan):
            inst, watch, rx, at, delay = plan[plan_next]
            if plan_hit is None and ((at and time.time() - t0 >= at) or
                                     (rx and rx.search(log_text(watch, plan_marks[watch])))):
                plan_hit = time.time()
            if plan_hit is not None and time.time() - plan_hit >= delay:
                plan_next, plan_hit = plan_next + 1, None
                crashed = plan_next == len(plan)
                marks = [os.path.getsize(Path(a.root) / f"inst{i}" / "run.log") for i in range(a.count)]
                plan_marks = list(marks)
                print("[harness +%.1f s] crash %d of %d: instance %d" % (time.time() - ts0, plan_next, len(plan), inst),
                      flush=True)
                crash_instance(a.root, inst)
        if idle_limit > 0:
            # Distinct party lines with numbers blanked: periodic status/roster/state lines repeat
            # the same text and don't count as progress.
            n = len({re.sub(r"0x[0-9a-fA-F]+|\d[\d.:]*", "#", l) for i in range(a.count)
                     for l in log_text(i, starts[i]).splitlines() if idle_rx.match(l)})
            in_world = all((re.findall(r"^Party: (?:\[[^\]]*\] )?(world up|world down)\b", log_text(i), re.M) or ["-"])[-1]
                           == "world up" and (re.findall(r"^Party: (?:\[[^\]]*\] )?loading screen (up|gone)\b", log_text(i), re.M)
                                              or ["gone"])[-1] == "gone" for i in range(a.count))
            if n != last_party_count or not in_world:  # boots, titles and loads never count as idle
                last_party_count, last_party_change = n, time.time()
            elif n > 0 and time.time() - last_party_change > idle_limit and not (want_crash and not crashed):
                verdict = verdict or ("IDLE: no new party log lines for %.0f s (run ended at %.0f s)"
                                      % (idle_limit, time.time() - t0))
                print(verdict, flush=True)
                break
        if until and (crashed or not want_crash):
            texts = [log_text(i, marks[i]) for i in range(a.count)]
            text = chr(10).join(texts)
            if all(u.search(texts[i] if i is not None and i < a.count else text) for i, u in until):
                verdict = "all --until markers seen after %.0f s" % (time.time() - t0)
                break
        time.sleep(1)
    if until or verdict:
        print("verdict:", verdict or "TIMED OUT waiting for --until markers", flush=True)
    for i, p, log in procs:
        if p.poll() is None:
            subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
        code = p.wait()
        if a.timestamps:
            time.sleep(0.5)  # the copier thread drains the pipe
        log.close()
        print(f"instance {i}: exit {code}")


def logs(a):
    rx = re.compile(a.grep) if a.grep else None
    for i in range(a.count):
        path = Path(a.root) / f"inst{i}" / "run.log"
        if not path.exists():
            continue
        print(f"===== instance {i}")
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if not rx or rx.search(line):
                print(line)


def bbport_pids(root):
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process bbport*,bb-probe* -ErrorAction SilentlyContinue | "
                          "ForEach-Object { $_.Id.ToString() + '|' + $_.Path }"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        pid, _, path = line.partition("|")
        if path.lower().startswith(str(Path(root)).lower()):
            yield pid, path


def crash_instance(root, i):
    """Kills instance i's game process only (not run.bat): a crash, as far as run.bat can tell."""
    want = str(Path(root) / f"inst{i}").lower() + os.sep
    for pid, path in bbport_pids(root):
        if path.lower().startswith(want):
            subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True)
            print(f"crashed instance {i}: killed {pid} {path}", flush=True)


def crash(a):
    time.sleep(a.after)
    crash_instance(a.root, a.instance)


def kill(a):
    root = str(Path(a.root)).lower()
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process bbport*,bb-probe* -ErrorAction SilentlyContinue | "
                          "ForEach-Object { $_.Id.ToString() + '|' + $_.Path }"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        pid, _, path = line.partition("|")
        if path.lower().startswith(root):
            subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True)
            print("killed", pid, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["setup", "run", "logs", "kill", "crash"])
    ap.add_argument("--count", type=int, default=2)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=str(DEFAULT_GAME))
    # Never the player's live saves: a copy made from a backup (E:\games\bbport\test_saves).
    ap.add_argument("--saves", default=os.environ.get("BB_TEST_SAVES", r"E:\games\bbport\test_saves\savedata"))
    ap.add_argument("--seconds", type=int, default=120)
    # 4 instances share one 8 GB card: 4 x 1792 MiB plus the desktop fit; 3072 each overcommitted
    # VRAM and spilled into system RAM.
    ap.add_argument("--vram", type=int, default=int(os.environ.get("BB_MP_VRAM", "1792")))
    ap.add_argument("--fresh-saves", action="store_true",
                    help="run: every instance's party save starts again from its test save (no state from the last run)")
    ap.add_argument("--headless-delay-after-join", type=float, default=0,
                    help="run (--party --headless N): start the headless instances this many s after the host's "
                         "first join (cooperators 0 -> 1)")
    ap.add_argument("--join-sequential", action="store_true",
                    help="run --party: each guest starts once the previous one has joined the host (and --stagger s apart)")
    ap.add_argument("--headless", type=int, default=0,
                    help="run: the last N instances headless-lite (BB_HEADLESS=1: minimised, no draws, no present)")
    ap.add_argument("--fps", default=os.environ.get("BB_MP_FPS", "30"),
                    help="frame cap of the test instances (30, 60, 90, uncap: the game patch presets)")
    ap.add_argument("--stagger", type=float, default=float(os.environ.get("BB_MP_STAGGER", "2")),
                    help="run: s between instance starts (guests wait for the host's party code file anyway)")
    ap.add_argument("--party", action="store_true", help="instance 0 hosts a loopback party, the others join")
    ap.add_argument("--crash-at", type=float, default=0, help="run: kill one instance's game this many s in")
    ap.add_argument("--crash-instance", type=int, default=1)
    ap.add_argument("--crash-when", help="run: kill that instance's game once its log matches this regex")
    ap.add_argument("--crash-plan", action="append",
                    help="run: I[/W]:REGEX[@D], repeatable, in order: kill instance I's game D s after instance W's "
                         "log (default I; since the previous crash) matched REGEX")
    ap.add_argument("--label", default="",
                    help="run: what this test is for; shown in the windows' title bars and the queue status")
    ap.add_argument("--idle", type=float, default=float(os.environ.get("BB_MP_IDLE", "10")),
                    help="run: end once the party logs (Party:/Net: lines) stay unchanged this many s (0 = off)")
    ap.add_argument("--until", action="append",
                    help="run: stop (kill all) once every such regex matched in the logs (after the crash, if any); "
                         "I:REGEX matches instance I's log only")
    ap.add_argument("--instance", type=int, default=1, help="crash: which instance")
    ap.add_argument("--after", type=float, default=0, help="crash: wait this many s first")
    ap.add_argument("--env", action="append")
    ap.add_argument("--ienv", action="append", help="run: I:KEY=VALUE, set for instance I only (after --env)")
    ap.add_argument("--timestamps", action="store_true",
                    help="run: prefix every log line with [+seconds] since the run started (one clock for all instances)")
    ap.add_argument("--grep")
    a = ap.parse_args()
    if a.cmd == "run":
        # At most max_clients game clients on this machine across all agents (tools/mp/game_queue.py).
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from game_queue import game_slots
        with game_slots(a.count, a.root, label=a.label) as slots:
            a.slots = slots
            run(a)
        return
    {"setup": setup, "run": run, "logs": logs, "kill": kill, "crash": crash}[a.cmd](a)


if __name__ == "__main__":
    main()
