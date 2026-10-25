#!/usr/bin/env python3
"""Bloodborne 1.09 item lots for the party co-op C3 (docs/party/items.md): generates the lot tables
of gpu/shim/party/party_items.inc and checks the C3 ledger flag range.

Game data (read-only, untrusted; run with python -I). GAME = .../dvdroot_ps4.

  python -I tools/party/item_tool.py gen-inc GAME [--out gpu/shim/party/party_items.inc]
      ITEM_LOT_FLAG(lot, flag): every ItemLotParam row with a pickup flag (getItemFlagId > 0),
      except the Chalice Dungeons (lot >= 100,000,000 or a flag of area 29). Treasure follows
      flag = 50,000,000 + lot; NPC / key-item / boss lots do not, and 50 flags are shared by
      alternative lots, so the table lists every row.
      ITEM_LEDGER(idx, lot, done flag, note): the flagless (getItemFlagId <= 0) lots that an EMEVD
      2003[4] (host-only award) gives, minus the per-player / repeatable sources (docs/party/items.md
      6.1 item 3). `done flag` is the awarding event's completion flag (event id + slot), which
      the host's save keeps once the award ran. The index is the guest's ledger bit
      60009000 + idx, so it is stable: an existing --out file's indices are kept and new lots
      appended.
      ITEM_AWARD_SPOT(lot, spot flag, note): the flagged lots a map's EMEVD 2003[4] (host-only) award
      gives at an item spot (an altar, a corpse event) whose completion flag (event id + slot) is an
      item_lot_picked flag of party_flags.inc: that flag, not the lot's, hides the spot. A guest
      that receives the lot from the host also gets the spot flag in its own save (the spot is
      then gone for it, after loads too). Flagless lots keep their ledger bit only.
      ITEM_SHARED_TREASURE(lot, catchup, note): the map-treasure lots (pickup flag area != 0, not a
      Chalice) with a key item (EquipParamGoods.goodsType 1) plus SHARED_QUEST_LOTS (the daughter
      questline, goods that are not goodsType 1). The host's pickup goes to every member (C3)
      instead of staying per player (InstancedTreasureFlag). catchup 1 = SHARED_CATCHUP_LOTS, the
      progression keys / workshop tools a guest whose save has the flag but not the item is given
      again (KeyCatchupLots); the quest items may be handed in, so they are never re-given.
  python -I tools/party/item_tool.py check-ledger GAME [--first 60009000 --last 60009999]
      Scans every EMEVD, the param bnd, the talk ESD and the Lua bnds (decoded flag operands plus
      every u32 / f32 / f64 at any byte offset) for an id in the range; exit 1 on a hit.
"""
import argparse, glob, re, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402

LEDGER_BASE = 60009000
LEDGER_SIZE = 1000

# EMEVD sources whose lots are never replayed (docs/party/items.md 6.1 item 3).
DENY_EVENTS = {
    ("common", 9100): "NPC gift, repeatable (何度でも)",
    ("common", 9440): "covenant gem (per player)",
    ("common", 9500): "rune / gem use (per player)",
    ("m35_00_00_00", 13501940): "Clock Tower NPC gift, repeatable",
    ("m21_00_00_00", 12105064): "DLC messenger costumes (per player)",
}
DENY_LOTS = {
    0: "parameterised event, lot unresolved",
    43802: "cycle variant of 43800 (per-player flag 6671)",
}


# Not goodsType 1 but questline items the guests must have (Gascoigne's daughter: White / Red
# Messenger Ribbon, Red Jeweled Brooch; a guest never reached the sewer spots in the host's world).
SHARED_QUEST_LOTS = {2410900, 2410910, 2410920}
# Progression keys / workshop tools of 0.7.0 (quest_audit.md F2): re-given by the key catch-up.
SHARED_CATCHUP_LOTS = {2400450, 2800290, 2600550, 2600610, 3200720, 3200810, 3500800, 2200360, 2411000}


def is_chalice(lot, flag):
    return lot >= 100_000_000 or (flag > 0 and (flag // 100_000) % 100 == 29 and flag // 10_000_000 in (1, 5))


def shared_treasure(rows, goods):
    """[(lot, catchup, note)] for the shared map-treasure lots (see the module doc)."""
    out = []
    for lot, r in sorted(rows.items()):
        f = r["getItemFlagId"]
        if is_chalice(lot, f) or not (50_000_000 <= f < 60_000_000 and f // 100_000 % 100 != 0):
            continue
        keys = [r[f"lotItemId0{k}"] for k in range(1, 9)
                if r[f"lotItemCategory0{k}"] == 4 and r[f"lotItemId0{k}"] > 0
                and goods.get(r[f"lotItemId0{k}"], {}).get("goodsType") == 1]
        if keys or lot in SHARED_QUEST_LOTS:
            ids = keys or [r[f"lotItemId0{k}"] for k in range(1, 9) if r[f"lotItemCategory0{k}"] == 4]
            out.append((lot, int(lot in SHARED_CATCHUP_LOTS), "goods " + "+".join(str(i) for i in ids if i > 0)))
    return out


def boss_drops(rows, allrec):
    """[(lot, area, note)]: the host-only item lots (EMEVD 2003[4]) that a world boss's defeat event
    awards (an event with a boss / mid-boss defeat marker, 2003[12] / [53] / [15]), outside the Chalice
    Dungeons (m29). Party rule "Boss item drops for guests" (0.7.6): what the host's roll gives goes to
    the guests present at the kill, once (party_items ItemSource::BossDrop)."""
    out = {}
    for stem, recs in allrec.items():
        if stem.startswith("m29") or not re.match(r"m\d\d_", stem):
            continue
        for r in recs:
            if not ({m["m"] for m in r["markers"]} & {"boss_defeat", "midboss_defeat"}):
                continue
            for m in r["markers"]:
                if m["m"] != "item_lot":
                    continue
                vals = m["v"]
                lot = vals[0] if isinstance(vals, (list, tuple)) else vals
                if isinstance(lot, int) and lot > 0 and lot in rows and lot not in out:
                    out[lot] = (int(stem[1:3]), f"{stem} ev {r['event']}")
    return [(lot, a, n) for lot, (a, n) in sorted(out.items())]


def slot_items(row):
    """[(category, id)] of the slots of one ItemLotParam row that give something."""
    return [(row[f"lotItemCategory0{k}"], row[f"lotItemId0{k}"]) for k in range(1, 9)
            if row[f"lotItemId0{k}"] > 0 and row[f"lotItemCategory0{k}"] > 0]


def share_item(cat, item_id, goods):
    """Party policy (0.7.5): only key items and unique items are shared: goods of goodsType 1 or
    maxNum 1, and weapons / armor / rings. Consumables and repeatable stacks (Pebble, Numbing
    Mist, Blood Vials, Madman's Knowledge...), blood gems and anything else are per player."""
    if cat in (0, 1, 2):
        return True
    if cat == 8 and 200000 <= item_id < 300000:
        return True  # a Caryll rune (an NPC's reward: Eileen, Patches ...): unique, every member gets it (0.7.6)
    if cat == 4:
        g = goods.get(item_id, {})
        return g.get("goodsType") == 1 or g.get("maxNum") == 1
    return False


def lot_shareable(rows, goods, lot):
    """True when the lot, or a continuation row (lot + 1, +2 ... while rows exist: one award rolls
    them all), gives a shareable item."""
    n = lot
    while n in rows and n - lot < 8:
        if any(share_item(c, i, goods) for c, i in slot_items(rows[n])):
            return True
        n += 1
    return False


def load(game):
    game = Path(game)
    P = ft.load_params(game / "param" / "gameparam" / "gameparam.parambnd.dcx",
                       game / "paramdef" / "paramdef.paramdefbnd.dcx", ["ItemLotParam", "EquipParamGoods"])
    rows = {r["id"]: r for r in P["ItemLotParam"]}
    emedf = ft.load_emedf(ft.find_emedf(game / "event"))
    return rows, ft.expand_all(game / "event", emedf), {r["id"]: r for r in P["EquipParamGoods"]}


def ledger_candidates(rows, allrec):
    """{lot: (done flag, note)} for flagless host-only awards."""
    out = {}
    for stem, recs in allrec.items():
        for r in recs:
            for m in r["markers"]:
                if m["m"] != "item_lot":  # 2003[4]; 2003[36] (item_lot_client) gives on clients too
                    continue
                vals = m["v"]
                lot = vals[0] if isinstance(vals, (list, tuple)) else vals
                if not isinstance(lot, int) or lot in DENY_LOTS or stem.startswith("m29"):
                    continue
                row = rows.get(lot)
                if row is None or row["getItemFlagId"] > 0 or is_chalice(lot, -1):
                    continue
                if (stem, r["event"]) in DENY_EVENTS:
                    continue
                done = r["event"] + r["slot"]
                note = f"{stem} ev {r['event']} slot {r['slot']}"
                # The first non-denied source wins (sorted below for a stable choice).
                cur = out.get(lot)
                if cur is None or (done, note) < cur:
                    out[lot] = (done, note)
    return out


def award_spots(rows, allrec, flags_inc):
    """[(lot, spot flag, note)] for flagged host-only map awards whose event flag is an item spot's."""
    cats = ft.load_inc(flags_inc)

    def cat(f):
        for lo, hi, c, _n in cats:
            if lo <= f <= hi:
                return c
        return None

    out = set()
    for stem, recs in allrec.items():
        if not stem.startswith("m") or stem.startswith("m29"):
            continue
        for r in recs:
            if r["rest"] != 0:
                continue  # restarts on reload: its flag is not a "taken" state
            for m in r["markers"]:
                if m["m"] != "item_lot":
                    continue
                vals = m["v"]
                lot = vals[0] if isinstance(vals, (list, tuple)) else vals
                row = rows.get(lot) if isinstance(lot, int) else None
                if row is None or row["getItemFlagId"] <= 0 or is_chalice(lot, row["getItemFlagId"]):
                    continue
                if 6000 <= row["getItemFlagId"] <= 8999:
                    continue  # per-player lots (Messenger gifts, costumes): never replayed
                done = r["event"] + r["slot"]
                if done // 10_000_000 != 1 or cat(done) != "item_lot_picked":
                    continue
                out.add((lot, done, f"{stem} ev {r['event']} slot {r['slot']}"))
    return sorted(out)


INC_LEDGER = re.compile(r"ITEM_LEDGER\(\s*(\d+)\s*,\s*(\d+)\s*,")
INC_RETIRED = re.compile(r"retired ledger lot (\d+) \(index (\d+)")


def cmd_gen_inc(a):
    rows, allrec, goods = load(a.game)
    shared = shared_treasure(rows, goods)
    flagged = sorted((lot, r["getItemFlagId"]) for lot, r in rows.items()
                     if r["getItemFlagId"] > 0 and not is_chalice(lot, r["getItemFlagId"]))
    cand_all = ledger_candidates(rows, allrec)
    shared_lots = {t[0] for t in shared}
    # Party policy: a lot that gives no key / unique item is not shared (ITEM_UNSHARED), whether it
    # has a pickup flag or is a flagless host-only award. Its ledger index stays reserved.
    unshared = sorted({lot for lot in list(cand_all) + [f[0] for f in flagged]
                       if lot not in shared_lots and not lot_shareable(rows, goods, lot)})
    unshared_set = set(unshared)
    cand = {lot: v for lot, v in cand_all.items() if lot not in unshared_set}
    # Keep the indices an existing table already gave out.
    keep = {}
    outp = Path(a.out)
    if outp.exists():
        for m in INC_LEDGER.finditer(outp.read_text(encoding="utf-8")):
            keep[int(m.group(2))] = int(m.group(1))
        for m in INC_RETIRED.finditer(outp.read_text(encoding="utf-8")):
            keep.setdefault(int(m.group(1)), int(m.group(2)))
    order = sorted(cand, key=lambda lot: (keep.get(lot, 1 << 30), lot))
    used = set(keep.values())
    nxt = 0
    ledger = []
    for lot in order:
        if lot in keep:
            idx = keep[lot]
        else:
            while nxt in used:
                nxt += 1
            idx = nxt
            used.add(idx)
        if idx >= LEDGER_SIZE:
            raise ValueError("ledger full")
        ledger.append((idx, lot) + cand[lot])
    dropped = sorted(set(keep) - set(cand))
    unshared_set |= set(dropped)
    unshared = sorted(unshared_set)
    ledger.sort()
    bossdrops = boss_drops(rows, allrec)
    spots = award_spots(rows, allrec, Path(__file__).resolve().parents[2] / "gpu/shim/party/party_flags.inc")
    out = [
        "// Generated by tools/party/item_tool.py gen-inc from ItemLotParam and the EMEVD scripts",
        "// (Bloodborne 1.09). Do not edit by hand: change the tool's rules and regenerate.",
        "// ITEM_LOT_FLAG(lot, getItemFlagId): every lot row with a pickup flag (Chalice Dungeons",
        "// excluded). A lot award also rolls lot+1, +2... while rows exist (docs/party/items.md 2.1).",
        "// ITEM_LEDGER(idx, lot, done flag, note): flagless host-only awards; the guest keeps",
        f"// \"given\" in flag {LEDGER_BASE} + idx. Indices are stable (append only).",
        "// ITEM_AWARD_SPOT(lot, spot flag, note): flagged host-only map awards whose item spot is",
        "// hidden by the awarding event's own item_lot_picked flag (a guest given the lot gets it too).",
        "// ITEM_SHARED_TREASURE(lot, catchup, note): key-item / questline map treasure the host's pickup",
        "// gives to every member (not per player); catchup 1: the key catch-up re-gives it.",
        "// ITEM_UNSHARED(lot): lots that give no key / unique item (party policy 0.7.5: consumables and",
        "// repeatable stacks are per player): never broadcast, replayed or taken from the host.",
        "// ITEM_BOSS_DROP(lot, map area, note): host-only item lots a world boss's defeat event awards;",
        "// the host's roll goes to the guests present at the kill (party rule, 0.7.6).",
        f"// ---- {len(flagged)} flagged lots",
        "#ifdef ITEM_LOT_FLAG",
    ]
    for lot, flag in flagged:
        out.append(f"ITEM_LOT_FLAG({lot}, {flag})")
    out += ["#endif", f"// ---- {len(ledger)} ledger lots", "#ifdef ITEM_LEDGER"]
    for idx, lot, done, note in ledger:
        out.append(f'ITEM_LEDGER({idx}, {lot}, {done}, "{note}")')
    out.append("#endif")
    out += [f"// ---- {len(spots)} award spots", "#ifdef ITEM_AWARD_SPOT"]
    for lot, done, note in spots:
        out.append(f'ITEM_AWARD_SPOT({lot}, {done}, "{note}")')
    out.append("#endif")
    out += [f"// ---- {len(unshared)} unshared lots", "#ifdef ITEM_UNSHARED"]
    for lot in unshared:
        out.append(f"ITEM_UNSHARED({lot})")
    out.append("#endif")
    out += [f"// ---- {len(shared)} shared treasure lots", "#ifdef ITEM_SHARED_TREASURE"]
    for lot, catchup, note in shared:
        out.append(f'ITEM_SHARED_TREASURE({lot}, {catchup}, "{note}")')
    out.append("#endif")
    out += [f"// ---- {len(bossdrops)} boss drop lots", "#ifdef ITEM_BOSS_DROP"]
    for lot, area, note in bossdrops:
        out.append(f'ITEM_BOSS_DROP({lot}, {area}, "{note}")')
    out.append("#endif")
    for lot in dropped:
        out.append(f"// retired ledger lot {lot} (index {keep[lot]} stays reserved)")
    outp.parent.mkdir(parents=True, exist_ok=True)
    outp.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {outp}: {len(flagged)} flagged lots, {len(ledger)} ledger lots, {len(spots)} award spots, {len(shared)} shared treasure lots, {len(bossdrops)} boss drop lots")


def cmd_check_ledger(a):
    game = Path(a.game)
    lo, hi = a.first, a.last
    hits = []
    emedf = ft.load_emedf(ft.find_emedf(game / "event"))
    allrec = ft.expand_all(game / "event", emedf)

    def scan(v, where):
        if isinstance(v, bool):
            return
        if isinstance(v, int):
            if lo <= v <= hi:
                hits.append((where, v))
        elif isinstance(v, (list, tuple)):
            for x in v:
                scan(x, where)
        elif isinstance(v, dict):
            for x in v.values():
                scan(x, where)

    for stem, recs in allrec.items():
        for r in recs:
            where = f"{stem}:{r['event']}/{r['slot']}"
            for o in r["ops"]:
                scan([o["first"], o["last"]], where)
            for m in r["markers"]:
                scan(m["v"], where)
            for rd in r["reads"]:
                scan(rd["ids"], where)
    files = sorted(set(glob.glob(str(game / "event" / "*.emevd*")) + [str(game / "param" / "gameparam" / "gameparam.parambnd.dcx")]
                       + glob.glob(str(game / "script" / "**" / "*.dcx"), recursive=True)))
    nbytes = 0
    for p in files:
        b = ft.read_maybe_dcx(p)
        nbytes += len(b)
        for off in range(0, len(b) - 3):
            u, = struct.unpack_from("<I", b, off)
            f, = struct.unpack_from("<f", b, off)
            d = struct.unpack_from("<d", b, off)[0] if off + 8 <= len(b) else 0.0
            if lo <= u <= hi or lo <= f <= hi or lo <= d <= hi:
                hits.append((Path(p).name, off))
    print(f"{lo}..{hi}: {len(files)} files ({nbytes} bytes decompressed) + decoded EMEVD operands; "
          f"{len(hits)} hits")
    for h in hits[:40]:
        print("  ", h)
    return 1 if hits else 0


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("gen-inc")
    p.add_argument("game")
    p.add_argument("--out", default=str(Path(__file__).resolve().parents[2] / "gpu/shim/party/party_items.inc"))
    p = sp.add_parser("check-ledger")
    p.add_argument("game")
    p.add_argument("--first", type=int, default=LEDGER_BASE)
    p.add_argument("--last", type=int, default=LEDGER_BASE + LEDGER_SIZE - 1)
    a = ap.parse_args()
    try:
        return {"gen-inc": cmd_gen_inc, "check-ledger": cmd_check_ledger}[a.cmd](a) or 0
    except (ValueError, OSError, StopIteration, struct.error) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
