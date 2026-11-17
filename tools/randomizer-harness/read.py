"""Reads a solo arena chain's session.log against its plan (chain.py JSON). Per arena: load, fog (host_obj found, started
flag on after Cross), bar (shown, Game status boss name = expected), AI (attack animations), damage (HP drop under R1),
kill (defeat flag and whether it came before the ForceDead fallback), lamp flags, award lots, faults.
usage: read.py <plan json> <session.log>"""
import json, re, sys
sys.stdout.reconfigure(encoding="utf-8")
plan = json.load(open(sys.argv[1], encoding="utf-8"))
step_re = re.compile(r"test: travel step (\d+)/(\d+): (\S+)")
ts_re = re.compile(r"\[(\d\d:\d\d:\d\d)\.\d+\]")
flag_re = re.compile(r"test: flags (.*)")
bar_re = re.compile(r"boss health bar shown: a boss fight \(event (\d+), entity (\d+), slot (\d+)\)")
status_re = re.compile(r"^Game status: map (\w+), boss (-?\d+)")
anim_re = re.compile(r"target anim: entity (\d+) animation (-?\d+) \(HP (\d+)/(\d+)")
kill_re = re.compile(r"host_kill: (?:boss )?entity (\d+)(.*)")
obj_re = re.compile(r"test: host_obj (\d+) (.*)")
award_re = re.compile(r"award lot (-?\d+)")
lampuse_re = re.compile(r"Lamp travel: lamp (\d+) \(\w+\): (.{0,40})")
fault_re = re.compile(r"Guest fault|C\+\+ exception|STOP:|VK_ERROR")
import os, ast
_lf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lamps_%s.txt" % {"van": "vanilla"}.get(plan["world"], plan["world"]))
expected_awards = {}
for _l in open(_lf, encoding="utf-8"):
    _m = re.match(r"(\w+) .* award (\[[^\]]*\])", _l)
    if _m:
        expected_awards[_m.group(1)] = ast.literal_eval(_m.group(2))
lines_by_step = {}
step, last_ts, total = 0, "", None
flag_events = []   # (step, ts, flag, value)
for line in open(sys.argv[2], encoding="utf-8", errors="replace"):
    m = ts_re.search(line)
    if m:
        last_ts = m.group(1)
    m = step_re.search(line)
    if m:
        step = int(m.group(1))
        total = int(m.group(2))
    lines_by_step.setdefault(step, []).append((last_ts, line.rstrip()))
    m = flag_re.search(line)
    if m:
        for part in m.group(1).split():
            if "=" in part:
                f, v = part.split("=")[:2]
                if f.isdigit():
                    flag_events.append((step, last_ts, int(f), v[:1]))
print("last step", step, "of", total)
for p in plan["plan"]:
    first, last = p["first_step"], p["first_step"] + p["steps"] - 1
    seg = [x for s in range(first, last + 1) for x in lines_by_step.get(s, [])]
    reached = first in lines_by_step
    out = {"reached": reached}
    obj = [l for _, l in seg if obj_re.search(l) and str(p["fog"]) in l] if p["fog"] else []
    out["fog_obj"] = obj[0][obj[0].find("host_obj"):][:90] if obj else None
    fl = {}
    for s, ts, f, v in flag_events:
        if first <= s <= last + 1:
            fl.setdefault(f, []).append("%s@%d %s" % (v, s, ts))
    out["started"] = fl.get(p["started"])
    out["defeat"] = fl.get(p["defeat"])
    out["lamp_flags"] = {f: fl.get(f) for f in p["lamp_flags"] if fl.get(f)}
    bars = [bar_re.search(l).groups() for _, l in seg if bar_re.search(l)]
    out["bars_shown"] = bars
    names = [int(status_re.search(l).group(2)) for _, l in seg if status_re.search(l)]
    out["status_boss"] = sorted(set(n for n in names if n > 0))
    out["expected_names"] = sorted(set(n for _, n in p["bars"]))
    anims, hp, hit = {}, {}, {}
    killed = False
    for _, l in seg:
        if kill_re.search(l):
            killed = True
        m = anim_re.search(l)
        if m and not killed:  # HP lost to the R1 presses, before any host_kill
            e = int(m.group(1))
            hit.setdefault(e, [int(m.group(3)), int(m.group(3))])
            hit[e][1] = min(hit[e][1], int(m.group(3)))
        if m:
            e = int(m.group(1))
            anims.setdefault(e, set()).add(int(m.group(2)))
            hp.setdefault(e, []).append((int(m.group(3)), int(m.group(4))))
    out["anims"] = {e: sorted(a)[:12] for e, a in anims.items()}
    out["attacks"] = {e: len([a for a in v if a >= 0 and 2000 <= a % 10000 < 4000]) for e, v in anims.items()}
    out["hp"] = {e: (v[0], min(v)[0], v[-1]) for e, v in hp.items()}
    out["r1_damage"] = {e: v[0] - v[1] for e, v in hit.items()}
    out["kills"] = [(ts, kill_re.search(l).group(1), kill_re.search(l).group(2)[:110]) for ts, l in seg if kill_re.search(l)]
    out["awards"] = [award_re.search(l).group(1) for _, l in seg if award_re.search(l)]
    out["awards_expected"] = expected_awards.get(p["key"])
    out["lamp_use"] = sorted(set("%s %s" % lampuse_re.search(l).groups() for _, l in seg if lampuse_re.search(l)))
    out["scenes"] = [l[l.find("remo"):][:60] for _, l in seg if "Party story: remo ended" in l]
    out["faults"] = [l[:160] for _, l in seg if fault_re.search(l)]
    print("==", p["key"], p["mode"], "bodies", p["bodies"], "expected bars", p["bars"])
    for k, v in out.items():
        print("   ", k, v)
