#!/usr/bin/env python3
"""Bloodborne 1.09 id allowlists for the party co-op peer validation (bbport security pass):
derived at run time from the player's own game files (gpu/shim/gamedata derives the cache natively; this is the Python oracle of it
user/party_tables.txt that gpu/shim/party/party_gamedata.h loads; nothing generated is committed).

Game data (read-only, untrusted; run with python -I). GAME = .../dvdroot_ps4.

  derive(GAME): the tables below as cache sections.

Tables (every array sorted ascending, no duplicates):
  kPartyMaps         packed map ids area << 24 | block << 16 | region << 8 | index of every
                     map/mAA_BB_CC_DD folder and every map/mapstudio/mAA_BB_CC_DD.msb.dcx. The
                     travel funnel's +0x0C, a forced-placement map and a sign's AreaId are ids of
                     this form (docs/party/travel.md 1.1, from_api_schema.md 5).
  kPartyReturnPoints ReturnPointParam row ids whose (areaNo, blockNo) map exists: the "WarpParam"
                     the lamp warp 0x13CDF30 looks up (FUN_01F26B40: row +0 area, +1 block,
                     +4 returnPointEntityId, +8 returnAnimId, isRegistDeadReturn 0 = Hunter's
                     Dream), i.e. every lamp / headstone / last-lamp record id. Rows of area 0
                     (1, 9902950) have no map and are left out.
  kPartyEntities     entity ids AABnnnn (AA area, B block < 10) that a warp can name in map
                     mAA_0B: every 4-byte aligned int32 of that map's MSB files in the id range of
                     the map (a superset of the MSB point / part entity ids; an id that is not in
                     any MSB of its map cannot be a warp point there), plus the warp points of
                     EMEVD 2003[14] / 2002[2,4,6] and the return points.
  kPartyRemos        cutscene ids AABBNNNN of remo/sAA_BB_NNNN.remobnd(.dcx) plus every id an
                     EMEVD 2002[1..7] plays (a played id without a file is reported).

  python -I scripts/partydata/ids_tool.py list-npcs GAME

  npcMirror               the NPC presence mirror's allow-list (party_npc_mirror.h): entity ids of the
                     MSB character parts (PARTS_PARAM_ST type 2) that are people the party meets
                     (names with 会話 "talk" or NPC, plus NPC_NAMED), never an object-like chr that
                     carries a talk script (lamps' and headstones' "warp OBJ talk CHR", the Dream's
                     messengers and dummy chrs, bullet owners, event targets) nor an item
                     placeholder (NPCアイテム).
"""
import argparse, array, math, re, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402

MAP_RE = re.compile(r"^m(\d\d)_(\d\d)_(\d\d)_(\d\d)(?:\.msb(?:\.dcx)?)?$")
REMO_RE = re.compile(r"^s(\d\d)_(\d\d)_(\d{4})\.remobnd(?:\.dcx)?$")


def packed(a, b, c, d):
    return a << 24 | b << 16 | c << 8 | d


def scan_maps(game):
    maps, msbs = set(), []
    for p in (game / "map").iterdir():
        m = MAP_RE.match(p.name)
        if m and p.is_dir():
            maps.add(packed(*map(int, m.groups())))
    for p in sorted((game / "map" / "mapstudio").rglob("*.msb*")):
        m = MAP_RE.match(p.name)
        if m and p.is_file():
            g = tuple(map(int, m.groups()))
            maps.add(packed(*g))
            msbs.append((g, p))
    return maps, msbs


def msb_entities(msbs):
    """{entity id} of the AABnnnn range of each MSB's own map."""
    out = set()
    for (a, b, _c, _d), path in msbs:
        if b >= 10:
            continue  # Chalice layouts: no AAB id space
        lo = a * 100000 + b * 10000
        data = ft.read_maybe_dcx(path)
        if data[:4] != b"MSB ":
            raise ValueError(f"{path}: not an MSB")
        words = array.array("i")
        words.frombytes(data[:len(data) // 4 * 4])
        if sys.byteorder != "little":
            words.byteswap()
        out.update(v for v in words if lo <= v < lo + 10000)
    return out


def msb_lists(data):
    """[(list name, [entry offsets], end)] of an MSB (MSBB: MODEL, EVENT, POINT, ROUTE, LAYER, PARTS)."""
    out, off = [], 0x10
    while off and off < len(data):
        _ver, cnt, name_off = struct.unpack_from("<iiq", data, off)
        name = ft.utf16z(data, name_off) if name_off else ""
        offs = struct.unpack_from(f"<{cnt}q", data, off + 16)
        out.append((name, list(offs[:-1])))
        off = offs[-1]
    return out


def return_spots(msbs, rp_ids):
    """{return point entity id: (x, y, z, yaw radians)}: where a lamp / headstone warp puts the
    player. ReturnPointParam.returnPointEntityId names an MSB event (EVENT_PARAM_ST entry: +0x18
    base data offset -> +8 entity id; +0x20 type data offset -> +0 the region index) whose region
    (POINT_PARAM_ST entry: +0x14 position, +0x20 rotation in degrees) is the spot. Checked live:
    2412951 -> region 595 of m24_01 at (-193.4, -28.65, 68.5), 45 deg = the host's arrival."""
    want = set(rp_ids)
    spots = {}
    for (a, b, c, d), path in msbs:
        if c or d or b >= 10:
            continue
        data = ft.read_maybe_dcx(path)
        lists = dict(msb_lists(data))
        events, points = lists.get("EVENT_PARAM_ST", []), lists.get("POINT_PARAM_ST", [])
        for o in events:
            base, typ = struct.unpack_from("<qq", data, o + 0x18)
            if not base or not typ or o + base + 12 > len(data):
                continue
            eid = struct.unpack_from("<i", data, o + base + 8)[0]
            if eid not in want or eid in spots:
                continue
            region = struct.unpack_from("<i", data, o + typ)[0]
            if not 0 <= region < len(points):
                continue
            r = points[region]
            x, y, z = struct.unpack_from("<3f", data, r + 0x14)
            yaw = struct.unpack_from("<3f", data, r + 0x20)[1]
            spots[eid] = (x, y, z, math.radians(yaw))
    return spots


def emevd_ids(game):
    """(remo ids played by 2002[1..7], warp entity ids of 2003[14] / 2002[2,4,6])."""
    # Tag each 2002 instruction separately (flag_tool tags them all "cutscene").
    saved = dict(ft.MARKERS)
    for k in range(1, 9):
        ft.MARKERS[(2002, k)] = f"cutscene{k}"
    try:
        emedf = ft.load_emedf(ft.find_emedf(game / "event"))
        allrec = ft.expand_all(game / "event", emedf)
    finally:
        ft.MARKERS.clear()
        ft.MARKERS.update(saved)
    remos, warps = set(), set()
    for recs in allrec.values():
        for r in recs:
            for m in r["markers"]:
                v = m["v"]
                if m["m"].startswith("cutscene") and m["m"] != "cutscene8" and v and v[0] > 0:
                    remos.add(v[0])
                if m["m"] in ("cutscene2", "cutscene4", "cutscene6") and len(v) >= 3 and v[2] > 0:
                    warps.add(v[2])
                if m["m"] == "map_move" and len(v) >= 3 and v[2] > 0:
                    warps.add(v[2])
    return remos, warps


def entity_map(eid):
    return packed(eid // 100000, eid // 10000 % 10, 0, 0)


def remo_files(game):
    out = set()
    for p in (game / "remo").iterdir():
        m = REMO_RE.match(p.name)
        if m:
            a, b, n = map(int, m.groups())
            out.add(a * 1000000 + b * 10000 + n)
    return out


def array_lines(name, ctype, values, per_line=8):
    vals = sorted(values)
    out = [f"PARTY_IDS_ARRAY({name}, {ctype}, {len(vals)}) = {{"]
    for i in range(0, len(vals), per_line):
        out.append("    " + ", ".join(f"{v}u" if ctype == "std::uint32_t" else str(v)
                                      for v in vals[i:i + per_line]) + ",")
    out.append("};")
    return out


def warp_spots(msbs, ids):
    """{warp point entity id: (x, y, z, yaw radians)}: the MSB region (POINT_PARAM_ST entry: +0x14
    position, +0x20 rotation in degrees, +0x48 entity data offset -> +0 entity id) a stage warp's
    warp point names (EMEVD 2003[14] / Lua WarpNextStage, GSM +0x10). E.g. 2102808 -> region 5 of
    m21_00 at (28.4, -9.24, 5.6), 135 deg."""
    want = set(ids)
    spots = {}
    for (a, b, c, d), path in msbs:
        if c or d or b >= 10:
            continue
        data = ft.read_maybe_dcx(path)
        for r in dict(msb_lists(data)).get("POINT_PARAM_ST", []):
            ent = struct.unpack_from("<q", data, r + 0x48)[0]
            if not 0 < ent or r + ent + 4 > len(data):
                continue
            eid = struct.unpack_from("<i", data, r + ent)[0]
            if eid not in want or eid in spots:
                continue
            x, y, z = struct.unpack_from("<3f", data, r + 0x14)
            yaw = struct.unpack_from("<3f", data, r + 0x20)[1]
            spots[eid] = (x, y, z, math.radians(yaw))
    return spots


NPC_NAME = re.compile("会話|NPC")
NPC_NOT = re.compile("アイテム|ワープ|メッセンジャー|墓石|ダミーCHR")
# Named NPCs whose MSB part name has neither 会話 nor NPC (checked by hand in the part list).
NPC_NAMED = ("獣の守護者_デュラ", "ヤマムラ")


def npc_parts(msbs):
    """{entity id: part name} of the MSB character parts that are NPCs (kNpcMirrorIds). Part entry:
    +0 name offset, +0x14 type (2 = character), +0xB0 entity data offset -> +0 entity id."""
    out = {}
    for (a, b, c, d), path in msbs:
        if c or d or b >= 10:
            continue
        lo = a * 100000 + b * 10000
        data = ft.read_maybe_dcx(path)
        for o in dict(msb_lists(data)).get("PARTS_PARAM_ST", []):
            name_off, = struct.unpack_from("<q", data, o)
            typ, = struct.unpack_from("<i", data, o + 0x14)
            ent_off, = struct.unpack_from("<q", data, o + 0xB0)
            if typ != 2 or not 0 < ent_off or o + ent_off + 4 > len(data):
                continue
            eid, = struct.unpack_from("<i", data, o + ent_off)
            name = ft.utf16z(data, o + name_off) if name_off > 0 else ""
            if not lo <= eid < lo + 10000:
                continue
            if (NPC_NAME.search(name) and not NPC_NOT.search(name)) or name in NPC_NAMED:
                out[eid] = name
    return out


def fmt_spot(i, spot):
    x, y, z, yaw = spot
    return [str(i), f"{x:.4f}", f"{y:.4f}", f"{z:.4f}", f"{yaw:.6f}"]


def derive(game):
    """{section: [row, ...]} of the id tables from the player's own game folder (dvdroot_ps4; a mods
    overlay view is read like the game reads it). Rows are lists of strings (the oracle writes
    them to the cache); see the module doc for what each section holds."""
    game = Path(game)
    maps, msbs = scan_maps(game)
    P = ft.load_params(game / "param" / "gameparam" / "gameparam.parambnd.dcx",
                       game / "paramdef" / "paramdef.paramdefbnd.dcx", ["ReturnPointParam"])
    rp = sorted(r["id"] for r in P["ReturnPointParam"]
                if packed(r["areaNo"], r["blockNo"], 0, 0) in maps and r["id"] > 0)
    ents = msb_entities(msbs)
    e_remos, e_warps = emevd_ids(game)
    extra = {w for w in e_warps | set(rp) if 1000000 <= w < 100000000 and entity_map(w) in maps}
    ents |= extra
    remos = remo_files(game) | {r for r in e_remos if 0 < r < 100000000}
    rows = {r["id"]: r["returnPointEntityId"] for r in P["ReturnPointParam"] if r["id"] in rp}
    spots = return_spots(msbs, set(rows.values()))
    spot_ids = sorted(i for i in rows if rows[i] in spots)
    warps = warp_spots(msbs, {w for w in e_warps if 1000000 <= w < 100000000})
    npcs = npc_parts(msbs)
    one = lambda vals: [[str(v)] for v in sorted(vals)]  # noqa: E731
    return {
        "maps": one(maps), "returnPoints": one(rp), "entities": one(ents), "remos": one(remos),
        "returnSpots": [fmt_spot(i, spots[rows[i]]) for i in spot_ids],
        "warpSpots": [fmt_spot(i, warps[i]) for i in sorted(warps)],
        "npcMirror": one(npcs),
    }, npcs


def cmd_list_npcs(a):
    _maps, msbs = scan_maps(Path(a.game))
    npcs = npc_parts(msbs)
    print(f"{len(npcs)} NPC entities")
    for e in sorted(npcs):
        print(e, npcs[e])


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list-npcs", help="print every NPC entity id with its MSB part name")
    p.add_argument("game")
    p.set_defaults(fn=cmd_list_npcs)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
