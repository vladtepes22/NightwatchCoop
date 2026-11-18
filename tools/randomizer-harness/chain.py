"""Solo arena chain for one world: per arena warp to its lamp, stand at the fog and press Cross (fight start), warp beside
the boss and hit it with R1 (AI + damage), then the kill plan (HP steps where a fight hands over, HP 0 on every bar),
a ForceDead fallback, then light the boss lamp. Writes the steps file and a JSON with every expectation for read.py.
usage: chain.py <world folder name under work/harness/worlds, or van> <out name> <arena key> [...]"""
import glob, json, os, sys
sys.stdout.reconfigure(encoding="utf-8")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "randomizer"))
from gamedata import GameFiles, _load_events
import arenas as A

# worlds\<name>\Randomizer ...\ (generated worlds) and the .steps/.json outputs live in the tree's ignored work\harness
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "work", "harness")
os.makedirs(HERE, exist_ok=True)
# key: (lamp warp, fog object or None, lamp object or None) -- DATA: breaker's arena table, event 0's template 7000 runs
STATIC = {
    "gehrman": (2102950, None, None), "moon_presence": (2102950, None, None),
    "blood_starved_beast": (2302950, 2301800, 2301951), "paarl": (2302950, 2301810, 2301952),
    "amelia": (2402950, 2401800, 2401951), "cleric_beast": (2412950, 2411800, 2411952),
    "gascoigne": (2412953, 2411810, 2411953), "ebrietas": (2422950, 2421800, 2421951),
    "emissary": (2422950, 2421700, 2421952), "logarius": (2502950, 2501800, 2501952),
    "wet_nurse": (2602953, 2601800, 2601951), "witch": (2202950, 2201800, 2201951),
    "shadows": (2702950, 2701800, 2701951), "one_reborn": (2802950, 2801800, 2801951),
    "rom": (3202952, None, None), "amygdala": (3302950, 3301800, 3301951),
    "ludwig": (3402952, 3401800, 3401952), "laurence": (3402953, 3401850, 3401953),
    "maria": (3502950, 3501800, 3501952), "failures": (3502950, 3501810, 3501951),
    "orphan": (3602950, 3601800, 3601952),
}
FIGHT_EVENTS = {
    "gehrman": (12104802,), "moon_presence": (12104852,), "blood_starved_beast": (12304802,), "paarl": (12304702,),
    "amelia": (12404802,), "cleric_beast": (12414702,), "gascoigne": (12414802, 12414807), "ebrietas": (12424802,),
    "emissary": (12424790, 12424702), "logarius": (12504802,), "wet_nurse": (12604802,), "witch": (12204802, 12204807),
    "shadows": (12704802,), "one_reborn": (12804802,), "rom": (13204802,), "amygdala": (13304802,),
    "ludwig": (13404802, 13404825), "laurence": (13404852,), "maria": (13504802,), "failures": (13504852, 13504891),
    "orphan": (13604802,),
}
HANDOVER_PCT = {"gascoigne": 30, "ludwig": 40, "witch": 40}


def lamp_flags(obj, ev):
    """The template 7000 run naming this lamp object in event 0: its flag word (and +10), plus the lamp-lit flag guess."""
    for b, i, w in ev[0][0]:
        if (b, i) == (2000, 0) and len(w) > 5 and w[1] == 7000 and w[3] == obj:
            return [w[5], obj + 1000]  # the bonfire flag 2009[05] registers, the lamp's own flag (a lamp use sets it)
    return []


def main():
    world, out, keys = sys.argv[1], sys.argv[2], sys.argv[3:]
    if world == "van":  # vanilla: no mods, every body keeps its own boss
        folder = None
        spoiler = {"code": "vanilla", "report": {"arenas": {a.key: {"bodies": {str(e): a.key for e in a.bodies}, "mode": "vanilla"}
                                                            for a in A.ARENAS}}}
        files = GameFiles(r"C:\Games\CUSA03173", [])
    else:
        folder = glob.glob(os.path.join(HERE, "worlds", world, "Randomizer*"))[0]
        spoiler = json.load(open(os.path.join(folder, "randomizer_spoiler.json"), encoding="utf-8"))
        files = GameFiles(r"C:\Games\CUSA03173", [folder])
    events = {}
    steps, plan, flags = ["god"], [], []
    for key in keys:
        arena = A.BY_KEY[key]
        lamp, fog, lampobj = STATIC[key]
        stem = arena.stem
        mm = "%" + stem
        if stem not in events:
            events[stem] = _load_events(files, stem + "_00_00")
        ev = events[stem]
        bars = []
        for e in FIGHT_EVENTS[key]:
            for b, i, w in ev.get(e, ((), {}))[0]:
                if (b, i) == (2003, 11) and w[0] == 1 and w[1] not in [x[0] for x in bars]:
                    bars.append((w[1], w[3]))
        info = spoiler["report"]["arenas"].get(key, {})
        bodies = {int(k): v for k, v in info.get("bodies", {}).items()}
        body1 = arena.main if arena.main in bodies else (sorted(bodies)[0] if bodies else arena.bodies[0])
        defeat, started = arena.flags
        lf = lamp_flags(lampobj, ev) if lampobj else []
        flags += [defeat, started] + ([arena.intro] if arena.intro else []) + lf
        s = []
        s.append("host_lamp=%d@20" % lamp)
        if key == "gehrman":
            s += ["host_press=0x0@8%s!" % mm, "host_flag=72100131@2%s" % mm, "host_press=0x0@25%s!" % mm,
                  "host_flag=%d@2%s" % (started, mm), "host_press=0x0@10%s!" % mm]
        elif key == "moon_presence":
            s += ["host_press=0x0@8%s!" % mm, "host_flag=9900@2%s" % mm, "host_press=0x0@30%s!" % mm,
                  "host_flag=%d@2%s" % (started, mm), "host_press=0x0@10%s!" % mm]
        elif fog:
            if stem == "m25_00":  # Cainhurst's arrival scene (flag 12500000) plays first and eats the presses
                s.append("host_press=0x0@45%s!" % mm)
            for ang in (0, 180):  # the fog button is a box 1.0-1.2 m deep from the fog (ActionButtonParam depth); front side only
                s += ["host_obj=%d:0.6:%d@12%s" % (fog, ang, mm), "host_press=0x4000@2%s!" % mm,
                      "host_press=0x4000@2%s!" % mm, "host_press=0x0@12%s!" % mm]
        s.append("host_chr=%d:2@3%s" % (body1, mm))
        s += ["host_press=0x800@2%s!" % mm] * 6
        kills = []
        handover = HANDOVER_PCT.get(key)
        orphan_bar = [e for e, _ in bars if e % 1000 >= 860 and e % 1000 < 900]
        if bodies.get(body1) == "orphan" and key not in ("witch",):
            handover = 40
        if handover:
            kills.append("host_kill=%d:hp%d@3%s" % (body1, handover, mm))
            kills.append("host_press=0x0@20%s!" % mm)
        if key == "witch":
            kills.append("host_kill=%d:hp%d@3%s" % (body1, 40, mm))
            kills.append("host_press=0x0@15%s!" % mm)
        targets = [e for e, _ in bars]
        if arena.kind in (A.GROUP,) or (arena.kind == A.ARENA_ONLY and info.get("mode") == "mix" and key == "failures"):
            targets = sorted(set(targets) | set(bodies))
        if key in ("gascoigne", "ludwig"):
            later = [e for e in targets if e != body1]
            if later:
                kills.append("host_chr=%d:2@2%s" % (later[0], mm))
                kills += ["host_press=0x800@2%s!" % mm] * 3
            targets = later or targets
        for e in targets:
            kills.append("host_kill=%d:hp@2%s" % (e, mm))
        kills.append("host_press=0x0@30%s!" % mm)
        for e in targets:  # fallback: ForceDead, the boss's real defeat path (only matters when HP 0 ended nothing)
            kills.append("host_kill=%d@2%s" % (e, mm))
        kills.append("host_press=0x0@15%s!" % mm)
        s += kills
        if lampobj:
            for ang in (0, 90, 180, 270):
                s += ["host_obj=%d:1.0:%d@3%s" % (lampobj, ang, mm), "host_press=0x4000@2%s!" % mm,
                      "host_press=0x2000@3%s!" % mm, "host_press=0x0@3%s!" % mm]  # Circle: close the warp menu a lit lamp opens
        steps += s
        plan.append({"key": key, "stem": stem, "kind": arena.kind, "mode": info.get("mode"), "bodies": bodies,
                     "body1": body1, "bars": bars, "defeat": defeat, "started": started, "intro": arena.intro,
                     "lamp": lamp, "fog": fog, "lampobj": lampobj, "lamp_flags": lf, "targets": targets,
                     "first_step": len(steps) - len(s), "steps": len(s)})
    steps.insert(1, "log_flag=" + "/".join(str(f) for f in flags))
    open(os.path.join(HERE, out + ".steps"), "w", encoding="utf-8", newline="\n").write(",".join(steps))
    json.dump({"world": world, "folder": folder, "code": spoiler["code"], "plan": plan},
              open(os.path.join(HERE, out + ".json"), "w", encoding="utf-8"), indent=1)
    print(out, "arenas", len(plan), "steps", len(steps), "chars", len(",".join(steps)))
    for p in plan:
        print(" ", p["key"], p["mode"], p["bodies"], "bars", p["bars"], "kill", p["targets"])


main()
