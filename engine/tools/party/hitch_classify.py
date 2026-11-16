#!/usr/bin/env python3
"""Classify the long frames of a BB_STALL_PROBE_DUMP file (hitch082): per frame, how many ms the main thread spent
in each known wait / work class, from its 1 ms stack samples.

  python -I tools/party/hitch_classify.py DUMP --exe out/bbport.exe [--min-ms 55] [--log inst0.log]

Classes (first match over the sample's symbolized frames, inlined functions included):
  readback      a write fault of the game thread on GPU-written memory: BufferCache::ReadMemory -> a command for the
                GPU thread, waited for (the frame's whole queued GPU work runs first)
  fault         other write-fault handling (OnWriteFault / InvalidateMemory) on the game thread
  reflex-sleep  Vulkan::Reflex::PreInputSample (the NVIDIA latency sleep before the pad read)
  ll-gate       the low-latency pacing gate (VideoOut::WaitFlipQueue)
  frame-end     the GX frame end: waiting for the game's render thread
  party         our party tick (coop::)
  file-io       host file reads
  game          anything else with the instruction pointer in game code
  other         the rest (waits in system DLLs without a known caller, runtime shims)
With --log (the instance log with harness timestamps and log_pos lines) each frame gets the player's position.
"""
import argparse, bisect, re, subprocess, sys
from collections import Counter, defaultdict

IMAGE_BASE = 0x140000000
RULES = [
    ("readback", ("BufferCache::ReadMemory", "SendCommand<true")),
    ("fault", ("OnWriteFault", "GuestFaultSignalHandler", "InvalidateMemory")),
    ("reflex-sleep", ("Reflex::PreInputSample",)),
    ("ll-gate", ("WaitFlipQueue", "LowLatencyGate")),
    ("party", ("coop::", "CoopTick", "navwalk::")),
    ("file-io", ("runtime_file", "host_read", "ReadFile", "pread")),
]


def symbolize(exe, offs, tool):
    """{offset: 'fn1 | fn2 | ...'} with the inlined frames (addr2line -i)."""
    out = {}
    offs = sorted(offs)
    for i in range(0, len(offs), 300):
        part = offs[i:i + 300]
        r = subprocess.run([tool, "-f", "-C", "-i", "-a", "-e", exe] + ["0x%x" % (IMAGE_BASE + o) for o in part],
                           capture_output=True, text=True, errors="replace")
        cur = None
        fn = True
        for line in r.stdout.splitlines():
            if line.startswith("0x"):
                cur = int(line, 16) - IMAGE_BASE
                out[cur] = []
                fn = True
            elif cur is not None:
                if fn:
                    out[cur].append(line)
                fn = not fn
    return {k: " | ".join(v) for k, v in out.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--addr2line", default=r"C:\msys64\mingw64\bin\addr2line.exe")
    ap.add_argument("--min-ms", type=float, default=55)
    ap.add_argument("--log")
    ap.add_argument("--thread", default="M")
    a = ap.parse_args()
    frames = []
    for line in open(a.dump, errors="replace"):
        if line.startswith("frame "):
            p = line.split()
            frames.append({"ms": float(p[1]), "main": float(p[4]), "end": float(p[6]), "at": float(p[8]), "s": []})
        elif frames:
            p = line.split()
            if len(p) >= 3 and p[0] == a.thread:
                frames[-1]["s"].append(p[2:])
    sel = [f for f in frames if f["ms"] >= a.min_ms]
    offs = set()
    for f in sel:
        for st in f["s"]:
            for x in st:
                if x[0] == "e" and re.fullmatch(r"e[0-9a-f]+", x):
                    offs.add(int(x[1:], 16))
    names = symbolize(a.exe, offs, a.addr2line)

    def classify(st):
        text = " ; ".join(names.get(int(x[1:], 16), "") for x in st if re.fullmatch(r"e[0-9a-f]+", x))
        for name, keys in RULES:
            if any(k in text for k in keys):
                return name
        if st[0][0] == "g":
            return "game"
        return "other"

    # positions: the dump's clock is steady_clock seconds; the log's is the harness clock. The stall probe line
    # of a frame is printed right at its end, so match the frames to "Stall probe: N ms frame" lines in order.
    where = {}
    if a.log:
        TS = re.compile(r"^\[\+\s*(\d+(?:\.\d+)?)\]\s?(.*)$")
        POS = re.compile(r"pos area \w+ \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)")
        pos, probes = [], []
        for line in open(a.log, errors="replace"):
            m = TS.match(line)
            if not m:
                continue
            t, s = float(m.group(1)), m.group(2)
            p = POS.search(s)
            if p:
                pos.append((t, tuple(float(v) for v in p.groups())))
            q = re.search(r"Stall probe: (\d+) ms frame", s)
            if q:
                probes.append((t, int(q.group(1))))
        if probes and frames:
            # offset between the clocks from the first probe line that matches a frame by its ms
            off = None
            for t, ms in probes:
                for f in frames:
                    if round(f["ms"]) == ms:
                        off = t - f["at"]
                        break
                if off is not None:
                    break
            pt = [p[0] for p in pos]
            for f in sel:
                if off is None:
                    break
                i = bisect.bisect_right(pt, f["at"] + off) - 1
                if i >= 0:
                    where[id(f)] = (f["at"] + off, pos[i][1])
    total = Counter()
    print("%8s %7s  %s" % ("t", "ms", "main thread ms by class"))
    for f in sel:
        n = len(f["s"])
        if not n:
            continue
        c = Counter(classify(st) for st in f["s"])
        per = f["ms"] / n
        parts = ", ".join("%s %.0f" % (k, v * per) for k, v in c.most_common() if v * per >= 3)
        for k, v in c.items():
            total[k] += v * per
        w = where.get(id(f))
        print("%8.1f %7.1f  %s%s" % (w[0] if w else f["at"] - frames[0]["at"], f["ms"], parts,
                                      "  at (%.1f, %.1f, %.1f)" % w[1] if w else ""))
    s = sum(total.values())
    print("\n%d frames of %.0f ms or more, %.0f ms in all; main thread ms by class:" % (len(sel), a.min_ms, s))
    for k, v in total.most_common():
        print("  %-13s %7.0f ms  %4.1f%%" % (k, v, 100 * v / s if s else 0))


if __name__ == "__main__":
    main()
