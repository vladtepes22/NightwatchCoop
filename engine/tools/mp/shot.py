#!/usr/bin/env python3
"""Screenshots of the test instances' game windows.

How a shot is taken (0.8.0): the GAME writes its own presented frame on request (BB_SHOT_TRIGGER /
BB_SHOT_DIR, set by instances.py): this tool creates inst<i>/shot.trigger, the renderer's next
presents copy the swapchain image into inst<i>/shotdump/f<N>_shot_<w>x<h>_bgra.raw. That is the
image the game sent to the display, so a covered, background or locked-desktop window cannot make
it black (PrintWindow / BitBlt read DWM's redirection surface, which a flip-model Vulkan swapchain
does not fill while the window is occluded or the session locked). Fallback when the game does not
answer within 2 s (an old build, a game still booting, HDR output, a minimised window that does
not present): PrintWindow with PW_RENDERFULLCONTENT. Every shot is checked for a black frame
(tools/mp/shotcheck.py); black ones are named in the log ("BLACK") and in <out>/capture.log.

  python tools/mp/shot.py --root DIR --out DIR [--tag NAME]
      one PNG per bbport.exe window whose process runs from DIR\\inst*\\out
  python tools/mp/shot.py --root DIR --out DIR --watch "regex=tag[@delay[,delay...]]" ...
      tails DIR\\inst*\\run.log and shoots every window when a line matches regex
      (after each delay in s; default 0), until --seconds pass.
  python tools/mp/shot.py --root DIR --out DIR --burst "START => END" ...
      a ride burst (liftfix): when a line matches START (in any instance's log), every window every
      second (ride<N>_<t>s_inst<i>.png, N counting the rides from 1, t the seconds since START) until
      2 s after every log that matched START has matched END (at most 90 s; a far game's copy ends at
      once); another START meanwhile is the same ride.
  python tools/mp/shot.py --root DIR --out DIR --grey [--watch ...]
      also every 2 s: a window whose 3D view (the centre, without the HUD corners) is nearly
      uniform for over 3 s while its log says the world is up (no loading screen) is a grey
      screen: "GREY SCREEN instN" on stdout and in DIR/grey.log, with a PNG in --out.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shotcheck  # noqa: E402

try:
    Image = shotcheck.require_pil()
except shotcheck.ShotDependencyError as e:
    # Loud, and with a non-zero exit the harness can see (it used to die inside the subprocess).
    print("shot.py: ERROR: " + str(e), file=sys.stderr, flush=True)
    sys.exit(3)

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi

# Per-monitor DPI aware: otherwise GetClientRect reports the scaled (logical) size of the game's
# window while PrintWindow draws physical pixels, so on a 150 % / 200 % display only the top-left
# part of the frame was saved (the bottom-centre item-get box and the top-right Insight / echoes
# counter were never in the shots).
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # PER_MONITOR_AWARE_V2
except (AttributeError, OSError):
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except (AttributeError, OSError):
        user32.SetProcessDPIAware()


def proc_path(pid):
    h = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ""
    buf = ctypes.create_unicode_buffer(1024)
    n = wt.DWORD(1024)
    ok = kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
    kernel32.CloseHandle(h)
    return buf.value if ok else ""


def windows(root, any_size=False):
    root = str(Path(root).resolve()).lower()
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        p = proc_path(pid.value).lower()
        if p.endswith("bbport.exe") and p.startswith(root):
            m = re.search(r"inst(\d+)", p)
            r = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            if r.right > 100 or any_size:  # a minimised window has no client area, its game still runs
                found.append((int(m.group(1)) if m else -1, hwnd))
        return True

    user32.EnumWindows(cb, 0)
    return sorted(found)


def grab(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    if w <= 0 or h <= 0:  # window closing / minimized: PIL "tile cannot extend outside image"
        return None
    hdc = user32.GetDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    buf = ctypes.create_string_buffer(w * h * 4)
    bmi = (ctypes.c_byte * 44)()
    ctypes.memmove(bmi, ctypes.byref(ctypes.c_uint32(40)), 4)
    ctypes.memmove(ctypes.byref(bmi, 4), ctypes.byref(ctypes.c_int32(w)), 4)
    ctypes.memmove(ctypes.byref(bmi, 8), ctypes.byref(ctypes.c_int32(-h)), 4)
    ctypes.memmove(ctypes.byref(bmi, 12), ctypes.byref(ctypes.c_uint16(1)), 2)
    ctypes.memmove(ctypes.byref(bmi, 14), ctypes.byref(ctypes.c_uint16(32)), 2)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, bmi, 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    img = Image.frombytes("RGBA", (w, h), buf.raw, "raw", "BGRA").convert("RGB")
    if img.convert("L").resize((64, 36)).getextrema()[1] < 12 and not getattr(grab, "warned", False):
        grab.warned = True  # 2026-10-13: every shot black while the desktop was locked (LockApp up)
        print("WARNING: black capture - is the desktop locked or the display off? DWM then draws no "
              "game window (PrintWindow and screen copies are black)", flush=True)
    return img


def read_raw(path):
    """A BB_SHOT dump (f<N>_shot_<w>x<h>_<bgra|rgba>.raw) as a PIL RGB image, or None."""
    m = re.search(r"_(\d+)x(\d+)_(bgra|rgba)\.raw$", path.name)
    if not m:
        return None
    w, h, fmt = int(m.group(1)), int(m.group(2)), m.group(3)
    data = path.read_bytes()
    if len(data) < w * h * 4:
        return None
    return Image.frombytes("RGBA", (w, h), data[:w * h * 4], "raw", "BGRA" if fmt == "bgra" else "RGBA").convert("RGB")


def ask_game(root, insts, timeout=2.0):
    """Asks each instance's game for its presented frame (all at once); {inst: image} of those that
    answered within `timeout` s."""
    root = Path(root)
    pending = {}
    for inst in insts:
        d = root / f"inst{inst}" / "shotdump"
        d.mkdir(parents=True, exist_ok=True)
        for old in d.glob("f*_shot_*.raw"):
            try:
                old.unlink()
            except OSError:
                pass
        trig = root / f"inst{inst}" / "shot.trigger"
        try:
            trig.write_bytes(b"1")
        except OSError:
            continue
        pending[inst] = (d, trig)
    got, t0 = {}, time.time()
    while pending and time.time() - t0 < timeout:
        for inst, (d, trig) in list(pending.items()):
            files = sorted(d.glob("f*_shot_*.raw"))
            if files:
                try:
                    img = read_raw(files[-1])
                except OSError:
                    continue
                if img is not None:
                    got[inst] = img
                    del pending[inst]
        time.sleep(0.03)
    for inst, (d, trig) in pending.items():  # not consumed: take it back so it is not a late stale shot
        try:
            trig.unlink()
        except OSError:
            pass
    return got


def capture(root, tag, out):
    """[(inst, image, method)] for every game window of `root`: the game's own frame, else PrintWindow."""
    wins = dict(windows(root, any_size=True))
    if not wins:
        return []
    # BB_SHOT_METHOD=printwindow: only the old window capture (the A/B of the occlusion proof).
    frames = {} if os.environ.get("BB_SHOT_METHOD") == "printwindow" else ask_game(root, sorted(wins))
    result = []
    for inst in sorted(wins):
        if inst in frames:
            result.append((inst, frames[inst], "ingame"))
            continue
        try:
            img = grab(wins[inst])
        except Exception as e:  # one bad capture must not end the series
            print(f"shot {tag} inst{inst}: capture failed ({e})", flush=True)
            continue
        if img is not None:
            result.append((inst, img, "printwindow"))
    return result


def note_capture(out, tag, inst, method, black, stats):
    line = "%s inst%d method=%s%s luma max=%d mean=%.1f std=%.1f" % (
        tag, inst, method, " BLACK" if black else "", stats[0], stats[1], stats[2])
    try:
        with open(Path(out) / "capture.log", "a", encoding="utf-8") as f:
            f.write(line + chr(10))
    except OSError:
        pass


def shoot(root, out, tag):
    out.mkdir(parents=True, exist_ok=True)
    names = []
    for inst, img, method in capture(root, tag, out):
        p = out / f"{tag}_inst{inst}.png"
        img.save(p)
        names.append(p.name)
        st = shotcheck.frame_stats(img)
        black = shotcheck.is_black_stats(*st)
        note_capture(out, tag, inst, method, black, st)
        if black:
            print(f"BLACK {p.name} (method {method}, max luma {st[0]}, std {st[2]:.1f})", flush=True)
    print("shot", tag, names, flush=True)


def view_stddev(img):
    """Luminance std-dev of the 3D view: the centre (x 20-80 %, y 15-70 %), clear of the HUD's
    corners (HP / stamina / quick items, Insight, the party panel)."""
    w, h = img.size
    box = img.crop((int(w * 0.2), int(h * 0.15), int(w * 0.8), int(h * 0.7))).convert("L").resize((96, 48))
    px = list(box.getdata())
    m = sum(px) / len(px)
    return (sum((v - m) ** 2 for v in px) / len(px)) ** 0.5


class GreyCheck:
    """Per instance: the loading state from its run.log and how long the view has been uniform."""

    def __init__(self, root, out):
        self.root, self.out = Path(root), Path(out)
        self.loading, self.since, self.flagged, self.next = {}, {}, set(), 0.0

    def note_log(self, inst, text):
        for line in text.splitlines():
            if "loading screen up" in line or "world down" in line:
                self.loading[inst] = True
            elif "loading screen gone" in line or "world up" in line and "loading no" in line:
                self.loading[inst] = False

    def tick(self):
        now = time.time()
        if now < self.next:
            return
        self.next = now + 2.0
        live = [i for i, _ in windows(self.root, any_size=True) if not self.loading.get(i, True)]
        for inst in list(self.since):
            if inst not in live:
                self.since.pop(inst, None)
        frames = ask_game(self.root, live) if live else {}
        for inst in live:
            try:
                img = frames.get(inst)
                if img is None:
                    img = grab(dict(windows(self.root))[inst])
                sd = view_stddev(img)
            except Exception:
                continue
            if sd >= 4.0:
                self.since.pop(inst, None)
                self.flagged.discard(inst)
                continue
            first = self.since.setdefault(inst, now)
            if now - first > 3.0 and inst not in self.flagged:
                self.flagged.add(inst)
                self.out.mkdir(parents=True, exist_ok=True)
                name = f"GREY_inst{inst}_{time.strftime('%H%M%S')}.png"
                img.save(self.out / name)
                line = f"GREY SCREEN inst{inst} {time.strftime('%H:%M:%S')} (view std-dev {sd:.1f}) {name}"
                print(line, flush=True)
                with open(self.root / "grey.log", "a", encoding="utf-8") as f:
                    f.write(line + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tag", default="shot")
    ap.add_argument("--watch", action="append", default=[])
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--every", type=float, default=0,
                    help="also shoot every N seconds (a timed series: TAG_<elapsed>s)")
    ap.add_argument("--grey", action="store_true", help="also flag grey screens (see above)")
    ap.add_argument("--burst", action="append", default=[], help='"START => END": a ride burst (see above)')
    a = ap.parse_args()
    out = Path(a.out)
    grey = GreyCheck(a.root, out) if a.grey else None
    bursts = []  # [start rx, end rx]
    for b in a.burst:
        st, _, en = b.partition(" => ")
        bursts.append((re.compile(st), re.compile(en)))
    ride = {"n": 0, "start": None, "next": None, "end": None}
    if not a.watch and not grey and not a.every and not bursts:
        shoot(a.root, out, a.tag)
        return
    rules = []
    for w in a.watch:
        rx, _, rest = w.rpartition("=")
        tag, _, d = rest.partition("@")
        rules.append((re.compile(rx), tag, [float(x) for x in d.split(",")] if d else [0.0]))
    logs = sorted(Path(a.root).glob("inst*/run.log"))
    pos = {p: (p.stat().st_size if p.exists() else 0) for p in logs}
    fired, pending, t0 = set(), [], time.time()
    next_shot, seen_window, last_window = (t0 + a.every if a.every else None), False, t0
    while time.time() - t0 < a.seconds:
        # Logs of instances that start after the watcher (a run folder made later) are read from
        # their first line.
        for p in Path(a.root).glob("inst*/run.log"):
            if p not in pos:
                pos[p] = 0
                logs.append(p)
        # Done when the games are gone: no window of this root for 20 s after one was seen.
        if windows(a.root):
            seen_window, last_window = True, time.time()
        elif seen_window and time.time() - last_window > 20:
            return
        if next_shot and time.time() >= next_shot:
            el = time.time() - t0
            # Sub-second series (--every 0.5): tenths in the name, or every other shot overwrote one.
            shoot(a.root, out, f"{a.tag}_{int(el)}s" if a.every >= 1 else f"{a.tag}_{el:05.1f}s")
            next_shot += a.every
        for p in logs:
            if not p.exists():
                continue
            try:
                size = p.stat().st_size
            except OSError:  # hfix080: the instance's run.log was replaced between exists() and stat() (a restart)
                continue
            if size < pos.get(p, 0):
                pos[p] = 0
            with open(p, "rb") as f:
                f.seek(pos.get(p, 0))
                text = f.read().decode("utf-8", "replace")
                pos[p] = size
            if grey:
                m = re.search(r"inst(\d+)", str(p))
                grey.note_log(int(m.group(1)) if m else -1, text)
            for srx, erx in bursts:
                m = srx.search(text)
                if m and ride["start"] is None:
                    ride.update(n=ride["n"] + 1, start=time.time(), next=time.time(), end=None, began=set(), ended=set())
                if m and ride["start"] is not None and ride["end"] is None:
                    ride["began"].add(p)
                    text = text[m.end():]  # an end in the same chunk must come after the start
                if ride["start"] is not None and ride["end"] is None and erx.search(text) and p in ride["began"]:
                    # over once every game whose car rode has ended it (a far copy ends at once)
                    ride["ended"].add(p)
                    if ride["ended"] >= ride["began"]:
                        ride["end"] = time.time()
            for rx, tag, delays in rules:
                if tag not in fired and rx.search(text):
                    fired.add(tag)
                    for d in delays:
                        pending.append((time.time() + d, f"{tag}_{d:g}s"))
        now = time.time()
        if ride["start"] is not None:
            if now >= ride["next"]:
                shoot(a.root, out, "ride%d_%ds" % (ride["n"], int(round(ride["next"] - ride["start"]))))
                ride["next"] += 1.0
            if (ride["end"] is not None and now >= ride["end"] + 2.0) or now - ride["start"] > 90:
                ride.update(start=None, next=None, end=None)
        for item in sorted(pending):
            if item[0] <= now:
                shoot(a.root, out, item[1])
                pending.remove(item)
        if grey:
            grey.tick()
        elif rules and len(fired) == len(rules) and not pending and not a.every and not bursts:
            return
        time.sleep(0.1 if ride["start"] is not None else 0.2)


if __name__ == "__main__":
    main()
