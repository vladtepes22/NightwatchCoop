#!/usr/bin/env python3
"""Symbolize and sum a BB_STALL_PROBE_DUMP file (hitch082): where the main thread (M), the game's render thread
(R), our GPU command thread (G) and the draw pipe's recording thread (B) were during each long frame.

  python -I tools/party/hitch_stacks.py DUMP --exe out/bbport.exe [--functions deps/ghidra-export/functions.tsv]
         [--min-ms 60] [--frame N] [--top 12] [--depth 6]

Per frame (or summed over all frames of at least --min-ms): per thread the share of samples per leaf and per
"stack" (the first --depth symbolized frames, innermost first). Exe addresses (eXXXX, offsets from the image
base 0x140000000) go through addr2line; guest addresses (gXXXX) through the Ghidra function export when given.
"""
import argparse, bisect, re, subprocess, sys
from collections import Counter, defaultdict

IMAGE_BASE = 0x140000000


def load_functions(path):
    ents = []
    if not path:
        return ents
    for line in open(path, errors="replace"):
        p = line.rstrip("\n").split("\t")
        try:
            ents.append((int(p[0], 16), p[1]))
        except (ValueError, IndexError):
            pass
    ents.sort()
    return ents


def addr2line(exe, offs, tool):
    """{offset: 'function (file:line)'} for return addresses / instruction pointers (offset - 1 for returns is
    not applied: the caller's line may be one off)."""
    out = {}
    offs = sorted(offs)
    for i in range(0, len(offs), 400):
        part = offs[i:i + 400]
        r = subprocess.run([tool, "-f", "-C", "-e", exe] + ["0x%x" % (IMAGE_BASE + o) for o in part],
                           capture_output=True, text=True, errors="replace")
        lines = r.stdout.splitlines()
        for k, o in enumerate(part):
            fn = lines[2 * k] if 2 * k < len(lines) else "?"
            src = lines[2 * k + 1] if 2 * k + 1 < len(lines) else "?"
            src = re.sub(r"^.*[/\\](gpu|src|third_party)[/\\]", r"\1/", src)
            fn = re.sub(r"\(.*", "", fn)
            out[o] = "%s (%s)" % (fn, src.split("/")[-1])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--functions")
    ap.add_argument("--addr2line", default=r"C:\msys64\mingw64\bin\addr2line.exe")
    ap.add_argument("--min-ms", type=float, default=60)
    ap.add_argument("--max-ms", type=float, default=1e9)
    ap.add_argument("--frame", type=int, default=-1, help="only this frame (index in the dump, over --min-ms or not)")
    ap.add_argument("--from-s", type=float, default=0)
    ap.add_argument("--to-s", type=float, default=1e18)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--per-frame", action="store_true")
    a = ap.parse_args()
    frames = []
    for line in open(a.dump, errors="replace"):
        if line.startswith("frame "):
            p = line.split()
            frames.append({"ms": float(p[1]), "main": float(p[4]), "end": float(p[6]), "at": float(p[8]), "s": []})
        elif frames:
            p = line.split()
            if len(p) >= 3:
                frames[-1]["s"].append((p[0], float(p[1]), p[2:]))
    t0 = frames[0]["at"] if frames else 0
    sel = [(i, f) for i, f in enumerate(frames) if (a.frame < 0 and a.min_ms <= f["ms"] <= a.max_ms and
                                                  a.from_s <= f["at"] - t0 <= a.to_s) or i == a.frame]
    exe_offs = set()
    for _, f in sel:
        for _, _, st in f["s"]:
            for x in st:
                if x[0] == "e":
                    exe_offs.add(int(x[1:], 16))
    names = addr2line(a.exe, exe_offs, a.addr2line) if exe_offs else {}
    fns = load_functions(a.functions)
    keys = [e[0] for e in fns]

    def sym(x):
        if x[0] == "e" and x[1:2] != "x":
            try:
                return names.get(int(x[1:], 16), x)
            except ValueError:
                return x
        if x[0] == "g":
            o = int(x[1:], 16)
            i = bisect.bisect_right(keys, o) - 1
            return "g:%s+0x%x" % (fns[i][1], o - fns[i][0]) if i >= 0 else "g:+0x%x" % o
        return x

    def show(title, fs):
        print(title)
        for th, name in (("M", "main thread"), ("R", "render thread"), ("G", "GPU thread"), ("B", "draw recording thread"), ("V", "Vulkan recorder thread"), ("C", "copy thread 0")):
            leaf, stack = Counter(), Counter()
            n = 0
            for f in fs:
                for t, _, st in f["s"]:
                    if t != th:
                        continue
                    n += 1
                    s = [sym(x) for x in st]
                    # consecutive duplicates (a function with several return addresses on its stack) once
                    d = [s[0]] + [s[k] for k in range(1, len(s)) if s[k] != s[k - 1]]
                    leaf[d[0]] += 1
                    stack[" < ".join(d[:a.depth])] += 1
            if not n:
                continue
            print("  %s: %d samples" % (name, n))
            print("    leaf:")
            for k, c in leaf.most_common(a.top):
                print("      %5.1f%%  %s" % (100.0 * c / n, k))
            print("    stacks:")
            for k, c in stack.most_common(a.top):
                print("      %5.1f%%  %s" % (100.0 * c / n, k))

    if a.per_frame or a.frame >= 0:
        for i, f in sel:
            show("frame %d: %.1f ms (main %.1f, frame end %.1f) at %.2f s" % (i, f["ms"], f["main"], f["end"], f["at"] - t0), [f])
    else:
        show("%d frames of %.0f ms or more (%.0f ms in all)" % (len(sel), a.min_ms, sum(f["ms"] for _, f in sel)), [f for _, f in sel])


if __name__ == "__main__":
    main()
