#!/usr/bin/env python3
"""BB_PARTY_PERF summary (gpu/shim/party/party_perf.h). Aggregates 'Party perf:' blocks of a run.log: per zone, worst per-frame p99 / max, frames > 0.2 / 2 ms,
calls, per-call max. usage: perfsum.py run.log [skip_blocks]"""
import re, sys
from collections import defaultdict

path = sys.argv[1]
skip = int(sys.argv[2]) if len(sys.argv) > 2 else 0
rx = re.compile(r"Party perf:\s+(\S+)\s+per frame p50\s+([\d.]+) p99\s+([\d.]+) max\s+([\d.]+) ms \((\d+) > 0.2 ms, (\d+) > 2 ms\) \|\s+(\d+) calls \(max (\d+)/frame, (\d+) off the tick thread\) p50\s+([\d.]+) p99\s+([\d.]+) max\s+([\d.]+) us, total ([\d.]+) ms")
hdr = re.compile(r"Party perf: ([\d.]+) s, (\d+) frames on the coop tick \(frame time p50 ([\d.]+) ms, p99 ([\d.]+) ms, max ([\d.]+) ms")
z = defaultdict(lambda: dict(p50=[], p99=[], fmax=0.0, o02=0, o2=0, calls=0, maxpf=0, off=0, cmax=0.0, total=0.0, cp99=0.0))
frames = 0
block = -1
fts = []
for line in open(path, encoding="utf-8", errors="replace"):
    m = hdr.search(line)
    if m:
        block += 1
        if block >= skip:
            frames += int(m.group(2))
            fts.append((float(m.group(3)), float(m.group(4)), float(m.group(5))))
        continue
    m = rx.search(line)
    if not m or block < skip:
        continue
    d = z[m.group(1)]
    d["p50"].append(float(m.group(2)))
    d["p99"].append(float(m.group(3)))
    d["fmax"] = max(d["fmax"], float(m.group(4)))
    d["o02"] += int(m.group(5))
    d["o2"] += int(m.group(6))
    d["calls"] += int(m.group(7))
    d["maxpf"] = max(d["maxpf"], int(m.group(8)))
    d["off"] += int(m.group(9))
    d["cp99"] = max(d["cp99"], float(m.group(11)))
    d["cmax"] = max(d["cmax"], float(m.group(12)))
    d["total"] += float(m.group(13))
print(f"{block + 1 - skip} blocks, {frames} frames; frame time p50 per block {[f[0] for f in fts]}")
print(f"{'zone':22} {'med p50':>8} {'worst p99':>9} {'max ms':>8} {'>0.2':>5} {'>2':>4} {'calls':>8} {'max/fr':>6} {'off':>6} {'call p99us':>10} {'call max us':>11} {'ms/frame':>8}")
for name, d in z.items():
    p50 = sorted(d["p50"])[len(d["p50"]) // 2] if d["p50"] else 0
    print(f"{name:22} {p50:8.4f} {max(d['p99']):9.4f} {d['fmax']:8.3f} {d['o02']:5d} {d['o2']:4d} {d['calls']:8d} {d['maxpf']:6d} {d['off']:6d} {d['cp99']:10.2f} {d['cmax']:11.2f} {d['total'] / max(frames, 1):8.4f}")
