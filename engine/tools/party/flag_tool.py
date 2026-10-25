#!/usr/bin/env python3
"""Bloodborne 1.09 event flags: game-data analysis and flag snapshot dump/compare (party co-op C2).

Game data (read-only, untrusted; run with python -I):

  python -I tools/party/flag_tool.py emevd  GAME/event [--map m24_00_00_00] [--json OUT] [--all-ops]
      Every EMEVD file: per map/event/slot the concrete flag writes (SetEventFlag 2003[2],
      batch 2003[22], random 2003[17], toggle 2003[9], value ops 2003[31/32/41/42/43]), the
      flag-bearing registrations (lamp 2009[3], ladder 2009[0], wandering 2009[1/2], obj damage
      2005[9], obj event delete 2005[12], hawk 2003[33/34]) and markers (boss defeat 2003[12/15/53],
      item lot 2003[4/36], cutscene 2002[*], map move 2003[14], trophy 2003[28]). Parameterised
      events are expanded once per InitializeEvent 2000[0] call (the slot's own args), so the ids
      printed are the ones the game writes. Event names come from the matching .emeld (Japanese).
  python -I tools/party/flag_tool.py params GAME/param/gameparam/gameparam.parambnd.dcx
                                            GAME/paramdef/paramdef.paramdefbnd.dcx
      Flag ids in params: ItemLotParam getItemFlagId(01..08), ShopLineupParam eventFlag,
      ObjActParam spQualifiedPassEventFlag.
  python -I tools/party/flag_tool.py gen-inc GAME [--out gpu/shim/party/party_flags.inc]
      Regenerates the category table: specific rows from the scripts/params (boss defeat, lamps,
      doors/ladders, chests/item lots, cutscenes, NPC events, restart events), then the global
      and per-map zone rows (GLOBAL_ROWS / ZONE_DEFAULT below). GAME = .../dvdroot_ps4.
  python -I tools/party/flag_tool.py classify GAME [--inc gpu/shim/party/party_flags.inc]
      Both of the above, then each flag id against the category table (party_flags.inc):
      per category counts and the flags no row covers.

Snapshots (the game side writes them with BB_PARTY_FLAG_DUMP=1; format below):

  python -I tools/party/flag_tool.py dump  A.bbpf [--set-only] [--inc party_flags.inc]
  python -I tools/party/flag_tool.py diff  A.bbpf B.bbpf [--inc party_flags.inc] [--sync-only]
          exit status 0 = no difference (in the compared set), 1 = differences, 2 = error
  python -I tools/party/flag_tool.py make  OUT.bbpf --set 1000,12404800 [--blocks auto]
          a synthetic snapshot for tests.

BBPF snapshot format, version 1 (little endian; written by the game side, read here):

  header, 64 bytes:
    0x00 char[4] magic "BBPF"
    0x04 u16     version (1)
    0x06 u16     header size (64)
    0x08 u32     bits per block (SprjEventFlagMan+0x1c; 1000 in 1.09)
    0x0c u32     block count (entries that follow)
    0x10 u32     packed map id (*(WorldChrMan+0x60)+0x3f8: area<<24 | block<<16 | ...), or 0xffffffff
    0x14 s32     flag store load mode (SprjEventFlagMan+0x80: 0 own world, 1 summoned/overlay)
    0x18 s32     session role (SprjSessionManager+0x124: 0 idle, 3 host, 6 client, ...), or -1
    0x1c u32     reserved (0)
    0x20 u64     unix time in ms
    0x28 u64     frame counter (any monotonic tick)
    0x30 char[16] label (NUL padded, e.g. "host", "guest1")
  then `block count` entries, ascending block index:
    u32 block index (flag id / bits per block)
    u16 byte length (ceil(bits per block / 8) = 125)
    u8  storage kind (node +0x28: 1 pooled / saved, 2 pointer / overlay)
    u8  reserved (0)
    u8  data[byte length]   the block's bytes as the game holds them: flag f of the block is
                            bit (7 - f % 8) of byte f / 8 (first flag of each byte = MSB).
"""
import argparse, json, os, re, struct, sys, time, zlib
from pathlib import Path

# ---------------------------------------------------------------------------------------------
# containers


def read_maybe_dcx(path):
    b = Path(path).read_bytes()
    if b[:4] != b"DCX\0":
        return b
    if b[0x24:0x28] != b"DCP\0" or b[0x28:0x2c] != b"DFLT":
        raise ValueError(f"{path}: unsupported DCX compression {b[0x28:0x2c]!r}")
    usz, csz = struct.unpack_from(">II", b, 0x1c)
    d = zlib.decompress(b[0x4c:0x4c + csz])
    if len(d) != usz:
        raise ValueError(f"{path}: DCX size mismatch")
    return d


def bnd4(b):
    """{name: bytes} of an uncompressed-entry BND4 (format 0x74, as the game's parambnds)."""
    if b[:4] != b"BND4":
        raise ValueError("not BND4")
    count, = struct.unpack_from("<I", b, 0x0c)
    esz, = struct.unpack_from("<Q", b, 0x20)
    unicode = b[0x30]
    out = {}
    for i in range(count):
        o = 0x40 + i * esz
        csz, = struct.unpack_from("<q", b, o + 8)
        off, _fid, noff = struct.unpack_from("<IiI", b, o + 0x18)
        if unicode:
            e = noff
            while e + 1 < len(b) and b[e:e + 2] != b"\0\0":
                e += 2
            name = b[noff:e].decode("utf-16le")
        else:
            e = b.index(b"\0", noff)
            name = b[noff:e].decode("shift_jis", "replace")
        if off + csz > len(b):
            raise ValueError("BND4 entry out of range")
        out[name] = b[off:off + csz]
    return out


def utf16z(b, at):
    e = at
    while e + 1 < len(b) and b[e:e + 2] != b"\0\0":
        e += 2
    return b[at:e].decode("utf-16le", "replace")

# ---------------------------------------------------------------------------------------------
# EMEDF (instruction definitions; common.emedf in the event folder)

# arg type codes (low 32 bits of the EMEDF arg type)
T_FMT = {0: "B", 1: "H", 2: "I", 3: "b", 4: "h", 5: "i", 6: "f", 8: "Q"}
T_SIZE = {0: 1, 1: 2, 2: 4, 3: 1, 4: 2, 5: 4, 6: 4, 8: 8}


def load_emedf(path):
    """{(bank, id): (japanese name, [(arg name, type code)])}"""
    b = read_maybe_dcx(path)
    if b[:4] != b"EDF\0":
        raise ValueError(f"{path}: not EDF")
    hdr = [struct.unpack_from("<QQ", b, o) for o in range(0x10, 0xb0, 0x10)]
    (ncls, ocls), (_nins, oins), (_narg, oarg) = hdr[0], hdr[1], hdr[2]
    ostr = hdr[8][1]
    out = {}
    for c in range(ncls):
        bank, ni, io, _nm = struct.unpack_from("<qqqq", b, ocls + c * 32)
        for i in range(ni):
            iid, na, ao, inm = struct.unpack_from("<qqqq", b, oins + io + i * 32)
            args = []
            for a in range(na):
                f = struct.unpack_from("<8q", b, oarg + ao + a * 64)
                args.append((utf16z(b, ostr + f[0]), f[1] & 0xffffffff))
            out[(bank, iid)] = (utf16z(b, ostr + inm), args)
    return out


def unpack_args(types, raw):
    """EMEVD argument packing: each value aligned to its own size."""
    vals, pos = [], 0
    for t in types:
        sz = T_SIZE.get(t, 4)
        pos = (pos + sz - 1) // sz * sz
        if pos + sz > len(raw):
            break
        vals.append(struct.unpack_from("<" + T_FMT.get(t, "i"), raw, pos)[0])
        pos += sz
    return vals

# ---------------------------------------------------------------------------------------------
# EMEVD (Bloodborne: little endian, 64-bit, version 0xCC)


class Emevd:
    def __init__(self, b):
        if b[:4] != b"EVD\0" or struct.unpack_from("<I", b, 8)[0] != 0xCC:
            raise ValueError("not a Bloodborne EMEVD")
        h = struct.unpack_from("<16q", b, 0x10)
        ev_n, ev_o, in_n, in_o = h[0], h[1], h[2], h[3]
        pa_n, pa_o = h[8], h[9]
        args_o = h[13]
        self.events = []
        for i in range(ev_n):
            eid, icnt, ioff, pcnt, poff, rest = struct.unpack_from("<qqqqqI", b, ev_o + i * 0x30)
            ins = []
            for k in range(icnt):
                bank, iid, alen, aoff, _layer = struct.unpack_from("<IIqqq", b, in_o + ioff + k * 0x20)
                ins.append([bank, iid, bytes(b[args_o + aoff:args_o + aoff + alen])])
            params = []
            for k in range(pcnt):
                ii, tgt, src, cnt = struct.unpack_from("<qqqi", b, pa_o + poff + k * 0x20)
                params.append((ii, tgt, src, cnt))
            self.events.append({"id": eid, "rest": rest, "ins": ins, "params": params})
        _ = (in_n, pa_n)


def load_emeld(path):
    try:
        b = read_maybe_dcx(path)
    except FileNotFoundError:
        return {}
    if b[:4] != b"ELD\0":
        return {}
    cnt, off = struct.unpack_from("<qq", b, 0x10)
    so, = struct.unpack_from("<q", b, 0x48)
    out = {}
    for i in range(cnt):
        eid, no = struct.unpack_from("<qq", b, off + i * 16)
        out[eid] = utf16z(b, so + no)
    return out

# instructions whose args name event flags: (bank, id) -> (op, [indices of flag args], extra)
FLAG_OPS = {
    (2003, 2): "set",          # SetEventFlag(flag, state 0 off / 1 on / 2 toggle)
    (2003, 9): "toggle",       # ToggleEventFlag(flag)
    (2003, 17): "random",      # RandomlySetEventFlagInRange(min, max, state)
    (2003, 22): "batch",       # BatchSetEventFlags(first, last, state)
    (2003, 31): "value_inc",   # IncrementEventValue(first, bits, max)
    (2003, 32): "value_clear",  # ClearEventValue(first, bits)
    (2003, 41): "value_op",    # EventValueOperation(first, bits, operand, operand first, operand bits, calc)
    (2003, 42): "value_itemcount",  # StoreItemAmountHeldInEventValue(type, id, first, bits)
    (2003, 43): "value_itemdirect",  # DirectlyGivePlayerItem(type, id, first, bits)
    (2003, 25): "ai_sos",      # CreateAiPlayerSos(type, npc, point, summon flag, dismiss flag)
    (2003, 51): "npc_summon",  # SummonNPC(type, npc, point, summon flag, dismiss flag)
    (2003, 33): "hawk_info",   # (flag)
    (2003, 34): "hawk_drop",   # (lot, region, flag, hit)
    (2009, 0): "ladder",       # RegisterLadder(flag, flag2, entity)
    (2009, 1): "wander_init",  # (flag, entity, appear flag)
    (2009, 2): "wander_reg",   # (flag, entity, entity2)
    (2009, 3): "bonfire",      # RegisterBonfire (DS leftover, unused by Bloodborne data)
    (2009, 5): "lamp",         # RegisterHealingFountain = Bloodborne lamp (flag 1AAB78x0, entity)
    (2005, 9): "obj_damage",   # (flag, ...)
    (2005, 12): "obj_event_del",  # (flag)
    (2004, 45): "npc_humanity",  # (npc, first flag)
}
MARKERS = {
    (2003, 12): "boss_defeat", (2003, 53): "boss_defeat", (2003, 15): "midboss_defeat",
    (2003, 4): "item_lot", (2003, 36): "item_lot_client", (2003, 14): "map_move",
    (2003, 28): "trophy", (2003, 21): "ng_plus", (2003, 27): "title_exit", (2003, 49): "warp_return",
    (2000, 5): "save_request", (2000, 2): "network_sync", (2009, 6): "boss_room_enter",
}
for _k in range(1, 9):
    MARKERS[(2002, _k)] = "cutscene"
# conditions / control flow that read flags (bank 3 / 1003 + 4 + 1003[101/103])
READ_OPS = {(3, 0), (3, 1), (3, 10), (3, 12), (3, 20), (1003, 0), (1003, 1), (1003, 2), (1003, 3),
            (1003, 4), (1003, 101), (1003, 103), (2004, 32)}


def flag_args(op, vals):
    """[(first, last, state or None)] the instruction writes (or registers)."""
    v = vals
    try:
        if op == "set":
            return [(v[0], v[0], v[1])]
        if op == "toggle":
            return [(v[0], v[0], 2)]
        if op in ("random", "batch"):
            return [(v[0], v[1], v[2] if op == "batch" else "rand")]
        if op == "value_inc" or op == "value_clear":
            return [(v[0], v[0] + v[1] - 1, "value")]
        if op == "value_op":
            return [(v[0], v[0] + v[1] - 1, "value")]
        if op in ("value_itemcount", "value_itemdirect"):
            return [(v[2], v[2] + v[3] - 1, "value")]
        if op in ("ai_sos", "npc_summon"):
            return [(v[3], v[3], "reg"), (v[4], v[4], "reg")]
        if op in ("hawk_info",):
            return [(v[0], v[0], "reg")]
        if op == "hawk_drop":
            return [(v[2], v[2], "reg")]
        if op == "ladder":
            return [(v[0], v[0], "reg"), (v[1], v[1], "reg")]
        if op in ("wander_init",):
            return [(v[0], v[0], "reg"), (v[2], v[2], "reg")]
        if op in ("wander_reg", "lamp", "bonfire", "obj_damage", "obj_event_del"):
            return [(v[0], v[0], "reg")]
        if op == "npc_humanity":
            return [(v[1], v[1], "reg")]
    except IndexError:
        pass
    return []


def expand_file(path, emedf, names_path=None, external=None):
    """[{event, slot, name, args, ops:[...], reads:[...], markers:[...]}] for one EMEVD file.

    external: {event id: [(slot, param bytes, caller label)]} - concrete InitializeEvent calls
    into this file from other files (map scripts starting common.emevd events)."""
    external = external or {}
    ev = Emevd(read_maybe_dcx(path))
    names = load_emeld(names_path) if names_path else {}
    by_id = {e["id"]: e for e in ev.events}
    # instances: event id -> [(slot, param bytes)]; events 0 and 50 run unparameterised
    calls = {}
    for e in ev.events:
        for bank, iid, raw in e["ins"]:
            if (bank, iid) == (2000, 0) and len(raw) >= 8:
                slot, target = struct.unpack_from("<iI", raw, 0)
                calls.setdefault(target, []).append((e["id"], slot, raw[8:], e))
    out = []

    def concretise(e, pbuf):
        """instruction list with the event's params substituted from pbuf."""
        ins = [[bk, ii, bytearray(raw)] for bk, ii, raw in e["ins"]]
        for ii, tgt, src, cnt in e["params"]:
            if 0 <= ii < len(ins) and pbuf is not None and src + cnt <= len(pbuf):
                ins[ii][2][tgt:tgt + cnt] = pbuf[src:src + cnt]
        return ins

    def instances(eid, depth=0):
        if eid not in calls and eid not in external:
            return [(0, None, "root")] if eid in by_id else []
        res = [(slot, pbuf, caller) for slot, pbuf, caller in external.get(eid, [])]
        for caller, slot, pbuf, ce in calls.get(eid, []):
            if ce["params"] and depth < 4:
                # a parameterised caller: expand its own instances first
                for cslot, cpbuf, _ in instances(caller, depth + 1):
                    cins = concretise(ce, cpbuf)
                    for bk, ii, raw in cins:
                        if (bk, ii) == (2000, 0) and len(raw) >= 8 and struct.unpack_from("<I", raw, 4)[0] == eid:
                            res.append((struct.unpack_from("<i", raw, 0)[0], bytes(raw[8:]), caller))
                    _ = cslot
            else:
                res.append((slot, pbuf, caller))
        # de-duplicate identical instances
        seen, uniq = set(), []
        for r in res:
            k = (r[0], r[1])
            if k not in seen:
                seen.add(k)
                uniq.append(r)
        return uniq

    for e in ev.events:
        insts = instances(e["id"])
        if not insts and e["id"] not in (0, 50):
            insts = [(0, None, "uncalled")]
        if not insts:
            insts = [(0, None, "root")]
        for slot, pbuf, caller in insts:
            ins = concretise(e, pbuf) if (e["params"] and pbuf is not None) else [[a, b_, bytearray(c)] for a, b_, c in e["ins"]]
            rec = {"event": e["id"], "slot": slot, "rest": e["rest"], "name": names.get(e["id"], ""),
                   "caller": caller, "params_unresolved": bool(e["params"] and pbuf is None),
                   "ops": [], "reads": [], "markers": [], "calls": []}
            for idx, (bank, iid, raw) in enumerate(ins):
                key = (bank, iid)
                types = [t for _n, t in emedf.get(key, ("", []))[1]]
                if key == (2000, 0):
                    if len(raw) >= 8:
                        s, t = struct.unpack_from("<iI", raw, 0)
                        rec["calls"].append((s, t, bytes(raw[8:])))
                    continue
                vals = unpack_args(types, bytes(raw)) if types else []
                if key in FLAG_OPS:
                    for first, last, state in flag_args(FLAG_OPS[key], vals):
                        rec["ops"].append({"i": idx, "op": FLAG_OPS[key], "first": first, "last": last, "state": state})
                elif key in MARKERS:
                    rec["markers"].append({"i": idx, "m": MARKERS[key], "v": vals})
                elif key in READ_OPS:
                    names_ = [n for n, _t in emedf.get(key, ("", []))[1]]
                    ids = [v for n, v in zip(names_, vals) if ("イベントフラグID" in n or "イベントフラグ先頭" in n) and "タイプ" not in n]
                    if ids:
                        rec["reads"].append({"i": idx, "k": f"{bank}[{iid}]", "ids": ids})
            out.append(rec)
    return out


def event_files(game_event_dir, only=None):
    d = Path(game_event_dir)
    for p in sorted(d.rglob("*.emevd*")):
        stem = p.name.split(".")[0]
        if only and stem != only:
            continue
        eld = p.with_name(stem + ".emeld" + (".dcx" if p.name.endswith(".dcx") else ""))
        yield stem, p, eld


def find_emedf(game_event_dir):
    for n in ("common.emedf.dcx", "common.emedf"):
        p = Path(game_event_dir) / n
        if p.exists():
            return p
    raise FileNotFoundError("common.emedf(.dcx) not found in the event folder")


def expand_all(event_dir, emedf, only=None, cross_map=False):
    """{file stem: records}; common.emevd last, with the map files' calls into it resolved.

    cross_map: also resolve the calls a map file (m29_20_90_00) makes into an event another file defines
    (m29.emevd's area-wide events such as 12906824 "set a dedicated flag on a boss defeat"): without it
    those instances keep unresolved parameters and the flags they set show as 0."""
    files = list(event_files(event_dir))
    allrec, external = {}, {}
    common = [f for f in files if f[0] == "common"]
    common_ids = {e["id"] for e in Emevd(read_maybe_dcx(common[0][1])).events} if common else set()
    own_ids = {stem: {e["id"] for e in Emevd(read_maybe_dcx(p)).events} for stem, p, _eld in files} if cross_map else {}
    ext_by_file = {}
    for stem, p, eld in files:
        if stem == "common":
            continue
        recs = expand_file(p, emedf, eld)
        own = {r["event"] for r in recs}
        for r in recs:
            for slot, target, pbuf in r["calls"]:
                if target in common_ids and target not in own:
                    external.setdefault(target, []).append((slot, pbuf, f"{stem}:{r['event']}"))
                elif cross_map and target not in own:
                    for ostem, ids in own_ids.items():
                        if ostem not in ("common", stem) and target in ids:
                            ext_by_file.setdefault(ostem, {}).setdefault(target, []).append((slot, pbuf, f"{stem}:{r['event']}"))
        if not only or stem == only:
            allrec[stem] = recs
    for ostem, ext in ext_by_file.items():  # re-expand the files that received cross-map calls
        if only and ostem != only:
            continue
        _s, p, eld = next(f for f in files if f[0] == ostem)
        allrec[ostem] = expand_file(p, emedf, eld, ext)
    for stem, p, eld in common:
        if not only or stem == only:
            allrec[stem] = expand_file(p, emedf, eld, external)
    return allrec


def cmd_emevd(a):
    emedf = load_emedf(find_emedf(a.event_dir))
    allrec = expand_all(a.event_dir, emedf, a.map)
    for stem, recs in allrec.items():
        if a.json:
            continue
        for r in recs:
            if not r["ops"] and not (a.all_ops and (r["markers"] or r["reads"])):
                continue
            mk = ",".join(sorted({m["m"] for m in r["markers"]}))
            print(f"{stem} ev {r['event']} slot {r['slot']} [{r['name']}]" + (f" <{mk}>" if mk else "")
                  + (" (params unresolved)" if r["params_unresolved"] else ""))
            for o in r["ops"]:
                rng = f"{o['first']}" if o["first"] == o["last"] else f"{o['first']}..{o['last']}"
                print(f"    {o['op']:<14} {rng} {o['state']}")
            if a.all_ops:
                for m in r["markers"]:
                    print(f"    *{m['m']:<13} {m['v']}")
                for rd in r["reads"]:
                    print(f"    ?{rd['k']:<13} {rd['ids']}")
    if a.json:
        Path(a.json).write_text(json.dumps(allrec, ensure_ascii=False, indent=0, default=lambda o: o.hex() if isinstance(o, (bytes, bytearray)) else str(o)), encoding="utf-8")
        print(f"wrote {a.json}: {sum(len(v) for v in allrec.values())} event instances")

# ---------------------------------------------------------------------------------------------
# params

PFMT = {"s8": "b", "u8": "B", "s16": "h", "u16": "H", "s32": "i", "u32": "I", "f32": "f"}
PSZ = {"s8": 1, "u8": 1, "s16": 2, "u16": 2, "s32": 4, "u32": 4, "f32": 4}
BYSZ = {1: "u8", 2: "u16", 4: "u32"}


def paramdef_layout(b):
    n, = struct.unpack_from("<H", b, 0x08)
    lay, off, bit, bt = [], 0, 0, None
    for i in range(n):
        o = 0x38 + i * 0xd0
        t = b[o + 0x70:o + 0x90].split(b"\0")[0].decode("latin-1")
        nm = b[o + 0x90:o + 0xb0].split(b"\0")[0].decode("latin-1")
        cnt, = struct.unpack_from("<I", b, o + 0x64)
        base = t if t in PFMT else (None if t.startswith(("dummy", "fixstr")) else BYSZ.get(cnt))
        m = re.match(r"(\w+)\s*:\s*(\d+)$", nm)  # "hasTarget : 1" has spaces (SpEffect)
        if m and base:
            w = int(m.group(2))
            if bt != base or bit + w > PSZ[base] * 8:
                if bt is not None:
                    off += PSZ[bt]
                bt, bit = base, 0
            lay.append((m.group(1), base, off, bit, w))
            bit += w
            continue
        if bt is not None:
            off += PSZ[bt]
            bt, bit = None, 0
        m = re.match(r"(\w+)\[(\d+)\]$", nm)
        lay.append((m.group(1) if m else nm, base or "raw", off, None, cnt))
        off += cnt
    return lay


def param_rows(pb, lay):
    cnt, = struct.unpack_from("<H", pb, 0x0a)
    for i in range(cnt):
        rid, = struct.unpack_from("<i", pb, 0x40 + i * 0x18)
        doff, = struct.unpack_from("<q", pb, 0x40 + i * 0x18 + 8)
        r = {"id": rid}
        for name, t, off, bit, w in lay:
            if t not in PFMT:
                continue
            v, = struct.unpack_from("<" + PFMT[t], pb, doff + off)
            r[name] = (v >> bit) & ((1 << w) - 1) if bit is not None else v
        yield r


def load_params(parambnd, paramdefbnd, names):
    pb = bnd4(read_maybe_dcx(parambnd))
    db = bnd4(read_maybe_dcx(paramdefbnd))
    out = {}
    for n in names:
        p = next(v for k, v in pb.items() if k.replace("/", "\\").endswith("\\" + n + ".param"))
        d = next(v for k, v in db.items() if k.replace("/", "\\").endswith("\\" + n + ".paramdef"))
        out[n] = list(param_rows(p, paramdef_layout(d)))
    return out


def param_flags(parambnd, paramdefbnd):
    """[(flag, source, row id, note)]"""
    P = load_params(parambnd, paramdefbnd, ["ItemLotParam", "ShopLineupParam", "ObjActParam"])
    res = []
    for r in P["ItemLotParam"]:
        for k in ["getItemFlagId"] + [f"getItemFlagId0{i}" for i in range(1, 9)]:
            f = r.get(k, 0)
            if f and f > 0:
                res.append((f, "ItemLotParam." + k, r["id"], ""))
        f = r.get("cumulateNumFlagId", 0)
        if f and f > 0:
            res.append((f, "ItemLotParam.cumulateNumFlagId", r["id"], "drop counter"))
    for r in P["ShopLineupParam"]:
        if r.get("eventFlag", 0) > 0:
            res.append((r["eventFlag"], "ShopLineupParam.eventFlag", r["id"], f"qty {r.get('sellQuantity')}"))
        if r.get("qwcId", 0) > 0:
            res.append((r["qwcId"], "ShopLineupParam.qwcId", r["id"], "shop row gate"))
    for r in P["ObjActParam"]:
        if r.get("spQualifiedPassEventFlag", 0) > 0:
            res.append((r["spQualifiedPassEventFlag"], "ObjActParam.spQualifiedPassEventFlag", r["id"], ""))
    return res


def cmd_params(a):
    for f, src, rid, note in sorted(param_flags(a.parambnd, a.paramdefbnd)):
        print(f"{f}\t{src}\t{rid}\t{note}")

# ---------------------------------------------------------------------------------------------
# category table (party_flags.inc)

INC_RE = re.compile(r"\{\s*(\d+)u?\s*,\s*(\d+)u?\s*,\s*Category::(\w+)\s*,\s*\"((?:[^\"\\]|\\.)*)\"\s*\}")


def load_inc(path):
    rows = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        s = line.split("//", 1)[0] if not line.lstrip().startswith("{") else line
        m = INC_RE.search(s)
        if m:
            rows.append((int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)))
    return rows


def categorize(rows, fid):
    """first matching row (the table lists specific rows before broad ones)."""
    for lo, hi, cat, note in rows:
        if lo <= fid <= hi:
            return cat, note
    return None, None


def cmd_classify(a):
    game = Path(a.game)
    ev_dir = game / "event"
    emedf = load_emedf(find_emedf(ev_dir))
    rows = load_inc(a.inc)
    flags = {}  # id -> set(sources)
    for stem, recs in expand_all(ev_dir, emedf).items():
        for r in recs:
            if r["event"] >= 1000:
                flags.setdefault(r["event"] + r["slot"], set()).add(f"{stem}:event_done")
            for o in r["ops"]:
                if o["last"] - o["first"] > 2000 or o["first"] < 0:
                    continue
                for f in range(o["first"], o["last"] + 1):
                    flags.setdefault(f, set()).add(f"{stem}:{o['op']}")
    pp = game / "param" / "gameparam" / "gameparam.parambnd.dcx"
    pd = game / "paramdef" / "paramdef.paramdefbnd.dcx"
    if pp.exists() and pd.exists():
        for f, src, _rid, _n in param_flags(pp, pd):
            flags.setdefault(f, set()).add(src.split(".")[0])
    per, unc = {}, []
    for f in sorted(flags):
        cat, _ = categorize(rows, f)
        if cat is None:
            unc.append(f)
        else:
            per[cat] = per.get(cat, 0) + 1
    for c, n in sorted(per.items(), key=lambda x: -x[1]):
        print(f"{c:<18} {n}")
    print(f"{'(unclassified)':<18} {len(unc)}")
    for f in unc[: a.show]:
        print(f"  {f}  {sorted(flags[f])[:4]}")

# ---------------------------------------------------------------------------------------------
# category table generator (gpu/shim/party/party_flags.inc)

# the flag store's map table (0x47314d0: 17 {area, block} pairs) - every map with pooled groups
STORE_MAPS = [(21, 0), (21, 1), (22, 0), (23, 0), (24, 0), (24, 1), (24, 2), (25, 0), (26, 0), (27, 0),
              (28, 0), (29, 0), (32, 0), (33, 0), (34, 0), (35, 0), (36, 0)]

# hand rows: global ids (type 0, ids 0..9999) and the other global groups. Evidence: EMEVD writers
# (EMELD names), ItemLotParam/ShopLineupParam, the flag store's load-mode code (0x13bb590) and the
# vanilla snapshot (0x13be3c0). First match wins: generated per-flag rows come before these.
GLOBAL_ROWS = [
    (0, 0, "never_sync", "flag 0: constant-false operand of thousands of EMEVD conditions"),
    (1, 20, "session_runtime", "warp-menu selection values (m29 12904029/30 value ops)"),
    (21, 23, "never_sync", "ending reached A/B/C -> staff roll / NG+ (cutscenes_endings.md); C4 replay only"),
    (24, 999, "never_sync", "unclassified (debug 101, widow 999); no progress writers"),
    (1000, 1999, "npc_quest", "NPC quest state machines, 20 flags per NPC; vanilla snapshot group 1"),
    (2000, 2099, "session_runtime", "online/session state, 2000-2022 rewritten every frame by session code (bbhost)"),
    (2100, 3699, "world_state", "per-area progress AAxx; the area gate bits of tables 0x47304B0/0x47301B0 are never_sync overrides (ON blocks the bells)"),
    (3700, 4999, "never_sync", "no EMEVD/param writers"),
    (5000, 5299, "item_lot_picked", "global ItemLotParam.getItemFlagId (key items, gifts)"),
    (5300, 5899, "session_runtime", "5500+slot shop-transfer events (restart)"),
    (5900, 5999, "world_state", "bath messenger lineup unlocks (boss kills / NPC deaths, re-set by preconstructor)"),
    (6000, 6399, "never_sync", "per-player: item-possession checks, chalice unlocks 62xx, caryll runes 63xx (own pool in load mode 1)"),
    (6400, 6599, "world_state", "multiplayer time-of-day unlock 640x guest / 650x host (common 9191/9192)"),
    (6600, 6999, "never_sync", "per-player: messenger shop lineup, League counters"),
    (7000, 7399, "session_runtime", "completion flags of common lamp events 7000/7100/7200/7300 + slot"),
    (7400, 7599, "never_sync", "per-player: Doll level-up 75xx"),
    (7600, 7699, "session_runtime", "multiplayer trap wall 7600 + slot"),
    (7700, 8999, "never_sync", "personal block (blocks 7,8 stay in the own pool in load mode 1)"),
    (9000, 9039, "never_sync", "warp/dungeon bookkeeping; 9020-9026 one-hot chalice slot selects flag-store groups"),
    (9040, 9119, "item_lot_picked", "NPC hands over an item - the item side is C3"),
    (9120, 9179, "never_sync", "unclassified"),
    (9180, 9180, "never_sync", "a cutscene is playing (set around every story remo; cutscenes_endings.md)"),
    (9181, 9199, "never_sync", "insight return 9181/9182 (per player)"),
    (9200, 9299, "session_runtime", "insight hallucinations / bell maiden events (restart)"),
    (9300, 9359, "never_sync", "a boss's first-encounter Insight given (per player: read and set by the first-fight events around their 9350)"),
    (9360, 9399, "npc_quest", "kin rune + NPC kill events"),
    (9400, 9419, "key_event", "9400/9401 constructor progress gates, 9402 first death, 9403 ladder top, 9404"),
    (9420, 9439, "key_event", "kidnapper warp to Yahar'gul / first sacrifice"),
    (9440, 9451, "never_sync", "covenant rune obtained (per player)"),
    (9452, 9479, "boss_defeated", "boss-killed bits set by every boss-defeat event"),
    (9480, 9499, "npc_quest", "kin rune NPC kill (2)"),
    (9500, 9599, "never_sync", "per-player item use counters"),
    (9600, 9699, "never_sync", "unclassified"),
    (9700, 9799, "npc_quest", "NPC misc state (dozing, avenger, Patches)"),
    (9800, 9802, "time_of_day", "9800 evening, 9801 night, 9802 Blood Moon (cutscene end 0x1CECEB0); cutscene replay only"),
    (9803, 9899, "never_sync", "unclassified"),
    (9900, 9919, "never_sync", "C-ending umbilical cord count (player-owned progression)"),
    (9920, 9999, "never_sync", "unclassified"),
    (10000000, 10009999, "never_sync", "type-1 global groups: no data writers"),
    (50000000, 50009999, "never_sync", "type-5 global groups: no data writers"),
    (60000000, 60009999, "never_sync", "type-6 global groups: no data writers"),
    (70000000, 70009999, "never_sync", "type-7 global: 70000030 set at lamp activation, NPC door knock, avenger state - unclassified"),
]
# Overrides emitted BEFORE the generated rows (first match wins): ids whose data-derived category is
# wrong or dangerous to sync silently. Evidence: docs/party/campaign_start.md section 2.2 (area gate
# tables dumped from the eboot: TBL1 0x47304B0, the host bell / summon validator 0x131D7B0 - an ON
# flag BLOCKS multiplayer in that area; TBL0 0x47301B0, the resonant/responder check 0x191A750) and
# docs/party/cutscenes_endings.md (ending and time-of-day flags).
AREA_GATE_TBL1 = [2100, 2200, 2300, 2301, 2400, 2401, 2402, 2410, 2411, 2412, 2420, 2421, 2500, 2600, 2601,
                  2700, 2701, 2800, 3200, 3300, 3400, 3401, 3402, 3403, 3510, 3511, 3512, 3513, 3600, 3601,
                  12901800, 12901801, 12901802, 12901803]
AREA_GATE_TBL0 = [2105, 2205, 2305, 2306, 2405, 2406, 2407, 2415, 2425, 2426, 2505, 2605, 2606, 2705, 2706,
                  2805, 3205, 3305, 3405, 3406, 3407, 3408, 3515, 3516, 3517, 3518, 3605, 3606,
                  12901805, 12901806, 12901807, 12901808]
OVERRIDE_ROWS = (
    [(f, f, "never_sync", "area gate (TBL1 0x47304B0: ON blocks the host bell / summons in the area)")
     for f in AREA_GATE_TBL1]
    + [(f, f, "never_sync", "area gate (TBL0 0x47301B0: resonant/responder check)") for f in AREA_GATE_TBL0]
    + [
        (21, 23, "never_sync", "ending reached A/B/C -> staff roll / NG+; C4 replay only"),
        (6600, 6604, "never_sync", "ending bookkeeping (6600 A, 6601 B, 6602 C, 6603 any, 6604 ending reached -> NG+ key item strip)"),
        (9180, 9180, "never_sync", "a cutscene is playing (runtime)"),
        # Snatcher kidnap trigger: m24_00 12400780 / m27 12700500 set it when a Snatcher kills the
        # own-world player (chr type 0: the host); common 9421 then sets the respawn point to the
        # Hypogean Gaol (2802959) and clears it. A guest's save holding it would wake in the Gaol
        # after its next solo death (docs/party/quest_audit.md).
        (9420, 9420, "never_sync", "Snatcher kidnap trigger (transient: common 9421 moves the respawn point to the Hypogean Gaol and clears it)"),
        (9800, 9802, "time_of_day", "9800 evening, 9801 night, 9802 Blood Moon; cutscene replay only (C4)"),
        (9900, 9901, "never_sync", "C ending: 9900 three cords eaten, 9901 cord count value"),
        (9905, 9905, "never_sync", "C ending: cord count event"),
        (9909, 9909, "never_sync", "C ending: sets 9900"),
        (12100000, 12100000, "never_sync", "ending event (m21); C4 replay only"),
        (12100002, 12100002, "never_sync", "ending event (m21); C4 replay only"),
        (12100180, 12100180, "never_sync", "A ending event (m21); C4 replay only"),
        (72100130, 72100131, "never_sync", "Gehrman's offer submit/refuse -> A ending / fight; C4 replay only"),
        # Playtest 1 bug 10: each player levels up at its own Doll (ERSC_PARITY H5). The Doll's
        # level-up handshake and its animations (m21 events 12100112-12100118, network-synced on
        # the player who talks) set in the host's world made the HOST kneel / the Doll gesture.
        (12100112, 12100118, "never_sync", "per-player: Doll level-up kneel / gesture / SFX / bow / reactions (m21 events 12100112-118)"),
        (72100100, 72100102, "never_sync", "per-player: Doll talk t210304 level-up handshake (72100100 -> 72100101 + 72100102)"),
        (72100105, 72100108, "never_sync", "per-player: Doll idle talk / bow / surprise triggers (m21 events 12100111, 12100115, 12100116)"),
        (72100110, 72100110, "never_sync", "per-player: Doll talk t210304 state (set as the talk opens)"),
        (72100113, 72100113, "never_sync", "per-player: Doll talk t210304 state after a level-up"),
        (72100202, 72100202, "never_sync", "per-player: the Dream mirror t210696 one-time note ('Wear clothing to change appearance.')"),
        # 0.7.3 (items for every player): the starting weapon messengers' done flags. m21 events
        # 12101020 / 12101021 hide the messengers once their flag is set (1003[101] -> label 0)
        # and set it only after the player's own pick (ShopLineupParam 2000-2002 / 2010-2011,
        # stock flags 72101000-72101002 / 72101010-72101011, per player). As npc_quest the host's
        # pick reached every guest's save: its messengers were gone with no weapon taken.
        (12101020, 12101021, "never_sync", "per-player: starting weapon messengers done (m21 events 12101020 / 12101021; the pick is each player's own)"),
    ])

# covenant_outcome (0.7.3, user decision 2026-10-22: covenant outcomes are per player): the quest
# state of the covenant leaders and of the NPC whose quest ends a covenant. Emitted FIRST (before the
# overrides): the game side treats these as their base category (the first row that is not one of
# these) only while the host's rule "Covenant outcomes per player" is off. Evidence: EMEVD writers
# and readers (EMELD names, `flag_tool.py emevd`), the NPC state machines 1000-1999 (20 flags per NPC,
# reset by common 50) and the talk blocks of the T7 map groups:
#   Annalise (穢れた血族の女王, Vilebloods): state 1040-1059 (1040 on the throne, 1041 hostile, 1042
#     the flesh: dead, killed by the player or by Alfred - m25 12500010 sets it when Alfred 1348 is in
#     the castle); m25 events 12500010-12500021 (init, kneel, offer / give blood, damage, the flesh
#     12500020 -> 9043); m25 T7 72500300-72500339 (damage count 304-309, kneel count 317-319, 326 the
#     revival trigger, 328 revived, 329-339 talk / animation state); the revival with the Queenly
#     Flesh, m24_02 12420500 (remo 24020000).
#   Alfred (血族狩り): state 1340-1359 (1347 handed the Cainhurst summons, 1348 in the castle,
#     1349 / 1341 / 1345 hostile, 1342 / 1346 / 1350 dead, 1351 talked to after the queen); m24_00 events
#     12400590-12400595, m25 events 12500050-12500054; m24 T7 72400460-72400479 (461 met, 465 the
#     invitation, 471, 475-476 attack counts), m25 T7 72500350-72500359; common 9770 (moved out of the
#     castle, sets 72500359).
#   Valtr (連盟の長, the League): state 1790-1809 (1790 dead, 1791 the successor, 1800 / 1801 / 1792
#     talk states, 1805 hostile; 1780-1789 are the Fishing Hamlet priest's); m27 events 12700901
#     (init), 12700903-12700905 (death, attack count, hostile), 12700910 (after his death); m27 T7
#     72700300-72700339 (304 / 306 init reads, 320 attack count, 321 / 322 the Impurity rune and League
#     Staff triggers of 12700906, 330 the helmet on his death).
# Not here (unchanged): the covenant rune flags 9440-9451 (never_sync), the oath itself (the equipped
# rune, the player's record), the personal block 6000-8999 (6676 / 6678 the Valtr gifts, 6813 League
# successor: never_sync), the item awards of these NPCs (item_lot_picked: C3, party_items) and Eileen
# (1360-1379: her quest stays shared).
COVENANT_ROWS = [
    (1040, 1059, "covenant_outcome", "Annalise (Vilebloods): alive / hostile / the flesh"),
    (1340, 1359, "covenant_outcome", "Alfred: the Cainhurst quest (in the castle, the queen's death, hostile, dead)"),
    (1790, 1809, "covenant_outcome", "Valtr (the League): talk states, successor, hostile, dead"),
    (9770, 9770, "covenant_outcome", "Alfred moved out of Cainhurst (common 9770)"),
    (12400590, 12400595, "covenant_outcome", "Alfred's events (m24_00): init, invitation, hostile, death"),
    (12420500, 12420500, "covenant_outcome", "Annalise's revival with the Queenly Flesh (m24_02, remo 24020000)"),
    (12500010, 12500021, "covenant_outcome", "Annalise's events (m25): init, kneel, blood, damage, the flesh"),
    (12500050, 12500054, "covenant_outcome", "Alfred's events (m25): init, talked to, hostile, death"),
    (12700901, 12700901, "covenant_outcome", "Valtr's init (m27)"),
    (12700903, 12700905, "covenant_outcome", "Valtr's death / attack count / hostile (m27)"),
    (12700910, 12700910, "covenant_outcome", "the League after Valtr's death (m27)"),
    (72400460, 72400479, "covenant_outcome", "Alfred's talk block (m24 T7)"),
    (72500300, 72500339, "covenant_outcome", "Annalise's talk / kneel / blood / revival block (m25 T7)"),
    (72500350, 72500359, "covenant_outcome", "Alfred in Cainhurst (m25 T7)"),
    (72700300, 72700339, "covenant_outcome", "Valtr's talk block, rune / staff triggers (m27 T7)"),
]

# per map, per type / zone default (zone = 4th digit from the right of TAABZnnn)
ZONE_DEFAULT = {
    (1, 0): ("world_state", "map objects/events (persistent; vanilla shares with phantoms)"),
    (1, 1): ("world_state", "boss-defeat events x800, elevators, shop events (persistent)"),
    (1, 4): ("boss_area", "boss fight / fog / NPC summon / insight transient (bbhost: 1AAB4800 fight state)"),
    (1, 5): ("session_runtime", "enemy AI / patrol / logic events"),
    (1, 6): ("session_runtime", "boss parts, rolling traps, ambience"),
    (1, 7): ("session_runtime", "lamp/warp object events (lamp flags are specific rows)"),
    (5, None): ("item_lot_picked", "ItemLotParam pickup flags"),
    (6, 0): ("world_state", "vanilla snapshot group (T6 zone 0)"),
    (6, 1): ("world_state", "vanilla snapshot group (T6 zone 1)"),
    (7, 0): ("npc_quest", "per-map NPC state (T7 zone 0)"),
    (7, 1): ("never_sync", "ShopLineupParam stock flags (purchases are per player)"),
    (7, 9): ("never_sync", "ShopLineupParam stock flags (purchases are per player)"),
}
DOOR_KW = ("扉", "門", "エレベ", "ハシゴ", "梯子", "はしご", "ショートカット", "レバー", "跳ね橋", "隠し", "仕掛け", "開放", "鍵を開け", "リフト")
DOOR_SKIP = ("msg", "Msg", "メッセージ", "動かない", "PlayLog", "プレイログ", "ナビメッシュ", "初期化")
NPC_KW = ("NPC", "人形", "老人", "メッセンジャー", "乞食", "娼婦", "少女", "神父", "女医", "復讐者", "血族狩り", "クモ男",
          "老婆", "偏屈", "ヘンリック", "門番", "連盟", "学長", "司祭", "アルフレート", "アイリーン", "ヴァルトール", "シモン")
KEY_KW = ("時間帯変化", "エンド", "人さらい", "異様な月")


def esc(s):
    return s.replace("\\", "\\\\").replace('"', "'")


SHOP_OWN_RANGE = (5500, 5521)  # common event 5500+slot "the player owns weapon X": per player


def gen_specific(allrec, pflags):
    """{flag id: (category, note, priority)} from the data."""
    spec = {}

    def put(f, cat, note, prio):
        if f < 0:
            return
        old = spec.get(f)
        if old is None or prio > old[2]:
            spec[f] = (cat, note, prio)

    for stem, recs in allrec.items():
        for r in recs:
            ms = {m["m"] for m in r["markers"]}
            name = r["name"]
            done = r["event"] + r["slot"] if r["event"] >= 10000000 else None
            tag = f"{stem} ev {r['event']}" + (f"+{r['slot']}" if r["slot"] else "")
            if ms & {"boss_defeat", "midboss_defeat"}:
                if done:
                    put(done, "boss_defeated", f"{tag} boss defeat event done", 9)
                for o in r["ops"]:
                    if o["state"] == 1 and o["first"] == o["last"]:
                        f = o["first"]
                        if f < 2100 or 5000 <= f <= 8999 or 9000 <= f <= 9451:
                            continue
                        put(f, "boss_defeated", f"set by {tag} (boss defeat)", 8)
            if name.startswith("ワープOBJ_"):
                ids = [i for rd in r["reads"] for i in rd["ids"]] + [o["first"] for o in r["ops"] if o["first"] == o["last"]]
                for f in ids:
                    if f // 10000000 != 7 or f // 1000 % 10 != 0:
                        continue
                    if name.startswith("ワープOBJ_起動") and f % 1000 // 100 == 2:
                        put(f, "lamp_unlocked", f"lamp lit (7AAB02xx), awaited by {tag}; cleared for absent DLC", 9)
                    else:
                        put(f, "session_runtime", f"lamp warp request, {tag}", 6)
            for o in r["ops"]:
                if o["op"] == "lamp" and o["first"] > 0:
                    put(o["first"], "lamp_unlocked", f"lamp object flag (2009[5] registration) by {tag}", 9)
                elif o["op"] == "ladder" and o["first"] > 0:
                    put(o["first"], "shortcut_door", f"ladder 2009[0] registered by {tag}", 7)
            if not done:
                continue
            zone, typ = done // 1000 % 10, done // 10000000
            if typ != 1:
                continue
            if r["rest"] == 1 and zone in (0, 1):
                put(done, "session_runtime", f"{tag} restarts on reload", 3)
                continue
            if zone not in (0, 1):
                continue

            def sets_in_zone01():
                for o in r["ops"]:
                    f = o["first"]
                    if o["state"] == 1 and f == o["last"] and f >= 10000000 and f // 10000000 == 1 and f // 1000 % 10 in (0, 1):
                        yield f
            if any(k in name for k in KEY_KW):
                put(done, "key_event", f"{tag} world progression", 6)
                for f in sets_in_zone01():
                    put(f, "key_event", f"set by {tag}", 5)
            elif any(k in name for k in DOOR_KW) and not any(k in name for k in DOOR_SKIP):
                put(done, "shortcut_door", f"{tag} door/lever/elevator", 6)
                for f in sets_in_zone01():
                    put(f, "shortcut_door", f"set by {tag}", 5)
            elif "宝箱" in name or ms & {"item_lot", "item_lot_client"}:
                put(done, "item_lot_picked", f"{tag} chest / item award", 5)
            elif "cutscene" in ms:
                put(done, "cutscene_seen", f"{tag} cutscene played", 4)
            elif any(k in name for k in NPC_KW):
                put(done, "npc_quest", f"{tag} NPC", 4)
    for f, src, rid, _n in pflags:
        if src.startswith("ItemLotParam") and src != "ItemLotParam.cumulateNumFlagId":
            put(f, "item_lot_picked", f"{src} row {rid}", 2)
        elif src.startswith("ShopLineupParam") and not src.endswith("qwcId"):
            put(f, "never_sync", f"shop stock row {rid} (per-player purchase)", 2)
    # Messenger shop gates (ShopLineupParam.qwcId) that are story / boss progress: the guest must get them
    # from the party (C2 world_state, set-only), or its shop never lists the row (0.7.6: Bone Ash / Madman
    # sets, flags 5090 / 5091, were in the item-lot range 5000-5299, which only C3's lot replay delivers, and
    # no lot carries them). Per-player gates stay per player: pickup flags of a lot, "owns the weapon" 5500-5521,
    # the personal block 6000-8999, the chalice runtime groups (type 9).
    lot_flags = {f for f, src, _r, _n in pflags if src.startswith("ItemLotParam")}
    static = structural_rows()
    for f, src, rid, _n in pflags:
        if src != "ShopLineupParam.qwcId" or f in lot_flags or SHOP_OWN_RANGE[0] <= f <= SHOP_OWN_RANGE[1]                 or 6000 <= f <= 8999 or f >= 90000000:
            continue
        old = spec.get(f)
        if old is not None:
            have = old[0]
        else:
            have = next((c for lo, hi, c, _x in static if lo <= f <= hi), "never_sync")
        if have in ("item_lot_picked", "never_sync", "session_runtime"):
            put(f, "world_state", f"shop gate (ShopLineupParam {rid}): progress without an item lot, shared set-only", 10)
    return spec


def merge_rows(spec):
    """contiguous ids of one category inside one 1000-flag group become one row."""
    rows, cur = [], None
    for f in sorted(spec):
        cat, note, _ = spec[f]
        blk = f // 1000
        if cur and cur[2] == cat and f == cur[1] + 1 and cur[4] == blk:
            cur[1] = f
            if note not in cur[5]:
                cur[5].append(note)
            continue
        if cur:
            rows.append(cur)
        cur = [f, f, cat, note, blk, [note]]
    if cur:
        rows.append(cur)
    out = []
    for lo, hi, cat, note, _blk, notes in rows:
        if len(notes) > 1:
            note = notes[0] + f" (+{len(notes) - 1} more)"
        out.append((lo, hi, cat, note))
    return out


def structural_rows():
    rows = list(GLOBAL_ROWS)
    for typ in (1, 5, 6, 7, 9):
        for area, block in STORE_MAPS:
            for zone in range(10):
                lo = typ * 10000000 + area * 100000 + block * 10000 + zone * 1000
                if typ == 9 or area == 29:
                    cat, note = "never_sync", "chalice dungeon / type 9 groups (out of scope)"
                elif (typ, None) in ZONE_DEFAULT:
                    cat, note = ZONE_DEFAULT[(typ, None)]
                else:
                    cat, note = ZONE_DEFAULT.get((typ, zone), ("never_sync", "pooled group with no data writers"))
                rows.append((lo, lo + 999, cat, f"m{area}_{block:02d} T{typ} zone {zone}: {note}"))
    for typ in (1, 5, 6, 7, 9):  # m29 blocks 1..9: chalice layouts (flag-store groups only via load mode / chalice remap)
        lo = typ * 10000000 + 29 * 100000 + 10000
        rows.append((lo, lo + 89999, "never_sync", f"m29 blocks 1-9 T{typ}: chalice layouts"))
    for area in range(41, 47):  # chalice instance areas (0x13bc710: areas 41..46 -> map slots 17..22)
        for typ in (1, 5, 6, 7, 9):
            lo = typ * 10000000 + area * 100000
            rows.append((lo, lo + 9999, "never_sync", f"chalice instance area {area} T{typ}"))
    for typ in (1, 5, 6, 7):  # area 99: generic dungeon groups (+0xb0 tree, not saved)
        lo = typ * 10000000 + 99 * 100000
        rows.append((lo, lo + 9999, "never_sync", f"area 99 T{typ}: chalice runtime groups, not in the saved pool"))
    return rows


def cmd_gen_inc(a):
    game = Path(a.game)
    ev_dir = game / "event"
    emedf = load_emedf(find_emedf(ev_dir))
    allrec = expand_all(ev_dir, emedf)
    pflags = param_flags(game / "param" / "gameparam" / "gameparam.parambnd.dcx",
                         game / "paramdef" / "paramdef.paramdefbnd.dcx")
    spec_rows = merge_rows(gen_specific(allrec, pflags))
    srows = structural_rows()
    out = [
        "// Generated by tools/party/flag_tool.py gen-inc from the game's EMEVD/EMELD and params",
        "// (Bloodborne 1.09). Do not edit by hand: change the tool's rules and regenerate.",
        "// Row: {first id, last id, Category, note}. FIRST MATCH WINS: specific rows (from the",
        "// event scripts and params) come first, then global rows, then per-map zone defaults.",
        "// Flag id layout: TAABZnnn - T type digit (0 global, 1/5/6/7/9), AA area, B block,",
        "// Z zone, nnn bit in the 1000-flag group (see docs/party/event_flags.md).",
        "// Categories: boss_defeated boss_area lamp_unlocked shortcut_door npc_quest",
        "// item_lot_picked key_event cutscene_seen world_state session_runtime never_sync",
        "// time_of_day (9800-9802: applied only through the cutscene replay path, never silently).",
        "// covenant_outcome (per player while the host's rule is on; else the next row's category).",
        "// Ids no row covers: treat as never_sync.",
        f"// ---- {len(COVENANT_ROWS)} covenant outcome rows (per player: Annalise, Alfred, Valtr)",
    ]
    for lo, hi, cat, note in COVENANT_ROWS:
        out.append(f'{{{lo}u, {hi}u, Category::{cat}, "{esc(note)}"}},')
    out.append(f"// ---- {len(OVERRIDE_ROWS)} override rows (area gates, endings, cutscene/time-of-day flags)")
    for lo, hi, cat, note in OVERRIDE_ROWS:
        out.append(f'{{{lo}u, {hi}u, Category::{cat}, "{esc(note)}"}},')
    out.append(f"// ---- {len(spec_rows)} specific rows")
    for lo, hi, cat, note in spec_rows:
        out.append(f'{{{lo}u, {hi}u, Category::{cat}, "{esc(note)}"}},')
    out.append(f"// ---- {len(srows)} global and structural rows")
    for lo, hi, cat, note in srows:
        out.append(f'{{{lo}u, {hi}u, Category::{cat}, "{esc(note)}"}},')
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {a.out}: {len(spec_rows)} specific + {len(srows)} structural rows")

# ---------------------------------------------------------------------------------------------
# snapshots

MAGIC = b"BBPF"
HDR = struct.Struct("<4sHHIIIii I QQ16s")
assert HDR.size == 64
ENT = struct.Struct("<IHBB")


class Snapshot:
    def __init__(self):
        self.bits = 1000
        self.map_id = 0xffffffff
        self.load_mode = 0
        self.role = -1
        self.unix_ms = 0
        self.frame = 0
        self.label = ""
        self.blocks = {}  # index -> (kind, bytes)

    @classmethod
    def read(cls, path):
        b = Path(path).read_bytes()
        if len(b) < 64 or b[:4] != MAGIC:
            raise ValueError(f"{path}: not a BBPF snapshot")
        m, ver, hsz, bits, n, mp, lm, role, _r, ms, fr, lab = HDR.unpack_from(b, 0)
        if ver != 1:
            raise ValueError(f"{path}: BBPF version {ver} not supported")
        s = cls()
        s.bits, s.map_id, s.load_mode, s.role, s.unix_ms, s.frame = bits, mp, lm, role, ms, fr
        s.label = lab.split(b"\0")[0].decode("ascii", "replace")
        pos = hsz
        for _ in range(n):
            idx, ln, kind, _res = ENT.unpack_from(b, pos)
            pos += ENT.size
            if pos + ln > len(b):
                raise ValueError(f"{path}: truncated block {idx}")
            s.blocks[idx] = (kind, b[pos:pos + ln])
            pos += ln
        return s

    def write(self, path):
        out = bytearray(HDR.pack(MAGIC, 1, 64, self.bits, len(self.blocks), self.map_id, self.load_mode,
                                 self.role, 0, self.unix_ms, self.frame, self.label.encode()[:16]))
        for idx in sorted(self.blocks):
            kind, data = self.blocks[idx]
            out += ENT.pack(idx, len(data), kind, 0) + data
        Path(path).write_bytes(out)

    def get(self, fid):
        blk, bit = divmod(fid, self.bits)
        if blk not in self.blocks:
            return None
        data = self.blocks[blk][1]
        return bool(data[bit >> 3] & (0x80 >> (bit & 7)))

    def set(self, fid, on=True):
        blk, bit = divmod(fid, self.bits)
        if blk not in self.blocks:
            self.blocks[blk] = (1, bytes((self.bits + 7) // 8))
        kind, data = self.blocks[blk]
        d = bytearray(data)
        if on:
            d[bit >> 3] |= 0x80 >> (bit & 7)
        else:
            d[bit >> 3] &= ~(0x80 >> (bit & 7)) & 0xff
        self.blocks[blk] = (kind, bytes(d))

    def set_ids(self):
        for blk in sorted(self.blocks):
            data = self.blocks[blk][1]
            for i, byte in enumerate(data):
                if byte:
                    for k in range(8):
                        if byte & (0x80 >> k):
                            f = i * 8 + k
                            if f < self.bits:
                                yield blk * self.bits + f


def describe(s):
    mp = s.map_id
    mtxt = "none" if mp == 0xffffffff else f"m{mp >> 24:02d}_{(mp >> 16) & 0xff:02d} (0x{mp:08x})"
    return (f"label={s.label!r} bits/block={s.bits} blocks={len(s.blocks)} map={mtxt} "
            f"load_mode={s.load_mode} role={s.role} frame={s.frame} t={s.unix_ms}")


def cmd_dump(a):
    s = Snapshot.read(a.snap)
    rows = load_inc(a.inc) if a.inc else []
    print(describe(s))
    if not a.set_only:
        for blk in sorted(s.blocks):
            kind, data = s.blocks[blk]
            print(f"block {blk} kind {kind} set {sum(bin(x).count('1') for x in data)}")
    for f in s.set_ids():
        cat = categorize(rows, f)[0] if rows else None
        print(f"{f}" + (f"\t{cat}" if cat else ""))


def cmd_diff(a):
    A, B = Snapshot.read(a.a), Snapshot.read(a.b)
    rows = load_inc(a.inc) if a.inc else []
    print("A: " + describe(A))
    print("B: " + describe(B))
    if A.bits != B.bits:
        print("bits per block differ")
        return 1
    only_a = sorted(set(A.blocks) - set(B.blocks))
    only_b = sorted(set(B.blocks) - set(A.blocks))
    if only_a:
        print(f"blocks only in A: {only_a}")
    if only_b:
        print(f"blocks only in B: {only_b}")
    sa, sb = set(A.set_ids()), set(B.set_ids())
    common = set(A.blocks) & set(B.blocks)
    changes = []
    for f in sorted(sa ^ sb):
        if f // A.bits not in common:
            continue
        cat = categorize(rows, f)[0] if rows else None
        if a.sync_only and (cat is None or cat in ("never_sync", "session_runtime")):
            continue
        changes.append((f, f in sa, f in sb, cat))
    per = {}
    for f, ia, ib, cat in changes:
        per[cat] = per.get(cat, 0) + 1
        print(f"{f}\tA={int(ia)} B={int(ib)}" + (f"\t{cat}" if cat else ""))
    print(f"{len(changes)} differing flags" + (" " + json.dumps(per) if per else ""))
    return 1 if changes else 0


def cmd_make(a):
    s = Snapshot()
    s.label = a.label
    s.unix_ms = int(time.time() * 1000)
    if a.map:
        s.map_id = int(a.map, 0)
    for blk in (int(x) for x in a.blocks.split(",") if x) if a.blocks != "auto" else []:
        s.blocks[blk] = (1, bytes((s.bits + 7) // 8))
    for f in (int(x) for x in a.set.split(",") if x):
        s.set(f, True)
    s.write(a.out)
    print(describe(s))

# ---------------------------------------------------------------------------------------------


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("emevd")
    p.add_argument("event_dir")
    p.add_argument("--map")
    p.add_argument("--json")
    p.add_argument("--all-ops", action="store_true")
    p = sp.add_parser("params")
    p.add_argument("parambnd")
    p.add_argument("paramdefbnd")
    p = sp.add_parser("classify")
    p.add_argument("game")
    p.add_argument("--inc", default=str(Path(__file__).resolve().parents[2] / "gpu/shim/party/party_flags.inc"))
    p.add_argument("--show", type=int, default=40)
    p = sp.add_parser("gen-inc")
    p.add_argument("game")
    p.add_argument("--out", default=str(Path(__file__).resolve().parents[2] / "gpu/shim/party/party_flags.inc"))
    p = sp.add_parser("dump")
    p.add_argument("snap")
    p.add_argument("--set-only", action="store_true")
    p.add_argument("--inc")
    p = sp.add_parser("diff")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--inc")
    p.add_argument("--sync-only", action="store_true")
    p = sp.add_parser("make")
    p.add_argument("out")
    p.add_argument("--set", default="")
    p.add_argument("--blocks", default="auto")
    p.add_argument("--map")
    p.add_argument("--label", default="synthetic")
    a = ap.parse_args()
    try:
        r = {"emevd": cmd_emevd, "params": cmd_params, "classify": cmd_classify, "gen-inc": cmd_gen_inc, "dump": cmd_dump,
             "diff": cmd_diff, "make": cmd_make}[a.cmd](a)
    except (ValueError, OSError, StopIteration, struct.error) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    return r or 0


if __name__ == "__main__":
    sys.exit(main())
