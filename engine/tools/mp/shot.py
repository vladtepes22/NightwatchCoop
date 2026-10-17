#!/usr/bin/env python3
"""Screenshots of the test instances' game windows (PrintWindow, works for background windows).

  python tools/mp/shot.py --root DIR --out DIR [--tag NAME]
      one PNG per bbport.exe window whose process runs from DIR\\inst*\\out
  python tools/mp/shot.py --root DIR --out DIR --watch "regex=tag[@delay[,delay...]]" ...
      tails DIR\\inst*\\run.log and shoots every window when a line matches regex
      (after each delay in s; default 0), until --seconds pass.
  python tools/mp/shot.py --root DIR --out DIR --grey [--watch ...]
      also every 2 s: a window whose 3D view (the centre, without the HUD corners) is nearly
      uniform for over 3 s while its log says the world is up (no loading screen) is a grey
      screen: "GREY SCREEN instN" on stdout and in DIR/grey.log, with a PNG in --out.
"""
import argparse, ctypes, ctypes.wintypes as wt, os, re, time
from pathlib import Path

from PIL import Image

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


def windows(root):
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
            if r.right > 100:
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


def shoot(root, out, tag):
    out.mkdir(parents=True, exist_ok=True)
    names = []
    for inst, hwnd in windows(root):
        p = out / f"{tag}_inst{inst}.png"
        try:
            img = grab(hwnd)
        except Exception as e:  # one bad capture must not end the series
            print(f"shot {tag} inst{inst}: capture failed ({e})", flush=True)
            continue
        if img is None:
            continue
        img.save(p)
        names.append(p.name)
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
        for inst, hwnd in windows(self.root):
            if self.loading.get(inst, True):
                self.since.pop(inst, None)
                continue
            try:
                img = grab(hwnd)
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
    a = ap.parse_args()
    out = Path(a.out)
    grey = GreyCheck(a.root, out) if a.grey else None
    if not a.watch and not grey and not a.every:
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
            shoot(a.root, out, f"{a.tag}_{int(time.time() - t0)}s")
            next_shot += a.every
        for p in logs:
            if not p.exists():
                continue
            size = p.stat().st_size
            if size < pos.get(p, 0):
                pos[p] = 0
            with open(p, "rb") as f:
                f.seek(pos.get(p, 0))
                text = f.read().decode("utf-8", "replace")
                pos[p] = size
            if grey:
                m = re.search(r"inst(\d+)", str(p))
                grey.note_log(int(m.group(1)) if m else -1, text)
            for rx, tag, delays in rules:
                if tag not in fired and rx.search(text):
                    fired.add(tag)
                    for d in delays:
                        pending.append((time.time() + d, f"{tag}_{d:g}s"))
        now = time.time()
        for item in sorted(pending):
            if item[0] <= now:
                shoot(a.root, out, item[1])
                pending.remove(item)
        if grey:
            grey.tick()
        elif rules and len(fired) == len(rules) and not pending and not a.every:
            return
        time.sleep(0.2)


if __name__ == "__main__":
    main()
