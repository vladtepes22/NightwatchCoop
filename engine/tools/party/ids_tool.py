#!/usr/bin/env python3
"""Bloodborne 1.09 id allowlists for the party co-op peer validation (bbport security pass):
generates gpu/shim/party/party_ids.inc, read by gpu/shim/party/party_ids.h.

Game data (read-only, untrusted; run with python -I). GAME = .../dvdroot_ps4.

  python -I tools/party/ids_tool.py gen-inc GAME [--out gpu/shim/party/party_ids.inc]

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
    for k in range(1, 9):
        ft.MARKERS[(2002, k)] = f"cutscene{k}"
    emedf = ft.load_emedf(ft.find_emedf(game / "event"))
    allrec = ft.expand_all(game / "event", emedf)
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


def cmd_gen_inc(a):
    game = Path(a.game)
    maps, msbs = scan_maps(game)
    P = ft.load_params(game / "param" / "gameparam" / "gameparam.parambnd.dcx",
                       game / "paramdef" / "paramdef.paramdefbnd.dcx", ["ReturnPointParam"])
    rp = sorted(r["id"] for r in P["ReturnPointParam"]
                if packed(r["areaNo"], r["blockNo"], 0, 0) in maps and r["id"] > 0)
    dropped_rp = sorted(r["id"] for r in P["ReturnPointParam"] if r["id"] not in rp)
    ents = msb_entities(msbs)
    e_remos, e_warps = emevd_ids(game)
    extra = {w for w in e_warps | set(rp) if 1000000 <= w < 100000000 and entity_map(w) in maps}
    missing_ents = sorted(extra - ents)
    ents |= extra
    files = remo_files(game)
    no_file = sorted(e_remos - files)
    remos = files | {r for r in e_remos if 0 < r < 100000000}
    out = [
        "// Generated by tools/party/ids_tool.py gen-inc from the game data (Bloodborne 1.09):",
        "// map folders and MSBs, ReturnPointParam, the EMEVD scripts and remo/. Do not edit by hand:",
        "// change the tool and regenerate. Read by party_ids.h (PARTY_IDS_ARRAY(name, type, count)).",
        f"// {len(maps)} maps, {len(rp)} return points (left out: {dropped_rp}),",
        f"// {len(ents)} entities ({len(missing_ents)} EMEVD / param warp points not seen in an MSB),",
        f"// {len(remos)} remos (EMEVD ids without a file: {no_file})",
    ]
    out += array_lines("kPartyMaps", "std::uint32_t", maps)
    out += array_lines("kPartyReturnPoints", "std::uint32_t", rp)
    out += array_lines("kPartyEntities", "std::uint32_t", ents, 10)
    out += array_lines("kPartyRemos", "std::uint32_t", remos)
    rows = {r["id"]: r["returnPointEntityId"] for r in P["ReturnPointParam"] if r["id"] in rp}
    spots = return_spots(msbs, set(rows.values()))
    spot_ids = sorted(i for i in rows if rows[i] in spots)
    out += [f"// {len(spot_ids)} return point spots (x, y, z, yaw radians) from the MSB revive events; "
            f"without a spot: {sorted(set(rp) - set(spot_ids))}"]
    out += array_lines("kPartyReturnSpotIds", "std::uint32_t", spot_ids)
    out.append(f"PARTY_IDS_ARRAY(kPartyReturnSpots, float, {len(spot_ids) * 4}) = {{")
    for i in spot_ids:
        x, y, z, yaw = spots[rows[i]]
        out.append(f"    {x:.4f}f, {y:.4f}f, {z:.4f}f, {yaw:.6f}f,  // {i}")
    out.append("};")
    warps = warp_spots(msbs, {w for w in e_warps if 1000000 <= w < 100000000})
    wids = sorted(warps)
    out += [f"// {len(wids)} stage warp point spots (x, y, z, yaw radians) from the MSB regions; "
            f"without a spot: {sorted({w for w in e_warps if 1000000 <= w < 100000000} - set(wids))}"]
    out += array_lines("kPartyWarpSpotIds", "std::uint32_t", wids)
    out.append(f"PARTY_IDS_ARRAY(kPartyWarpSpots, float, {len(wids) * 4}) = {{")
    for i in wids:
        x, y, z, yaw = warps[i]
        out.append(f"    {x:.4f}f, {y:.4f}f, {z:.4f}f, {yaw:.6f}f,  // {i}")
    out.append("};")
    outp = Path(a.out)
    outp.parent.mkdir(parents=True, exist_ok=True)
    outp.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {outp}: {len(maps)} maps, {len(rp)} return points, {len(ents)} entities, {len(remos)} remos")
    if missing_ents:
        print(f"  warp points not in an MSB: {missing_ents}")
    if no_file:
        print(f"  EMEVD remo ids without a file: {no_file}")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("gen-inc", help="write party_ids.inc")
    p.add_argument("game")
    p.add_argument("--out", default=str(Path(__file__).resolve().parents[2] / "gpu/shim/party/party_ids.inc"))
    p.set_defaults(fn=cmd_gen_inc)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
