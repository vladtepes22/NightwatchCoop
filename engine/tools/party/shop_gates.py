#!/usr/bin/env python3
"""Messenger shop gates (ShopLineupParam.qwcId = an event flag): what sets each one and whether the
party shares it (docs/party/shop_gates.md).

  python -I tools/party/shop_gates.py GAME [--md OUT.md] [--json OUT.json]

Per distinct gate flag: the shop rows (ids, the item), the setters found in the game data (an
ItemLotParam pickup flag, an EMEVD SetEventFlag, an EMEVD event whose id + slot is the flag, a
boss-defeat marker) and the class:
  progress - set by story / boss / NPC events: shared with the party (C2 category sync);
  item     - set because THIS player obtained / owns the item (a pickup flag of a lot, the
             5500+slot "owns weapon X" events): per player, never taken from the host.
"""
import argparse, collections, json, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402

SHOP_OWN_RANGE = (5500, 5521)  # common 5500+slot: ends once the player holds weapon param (event 5500)
# categories C2 delivers to guests (party_progress.cpp ModeFor: everything but item_lot_picked is Off or a candidate)
SYNCED = {"boss_defeated", "lamp_unlocked", "shortcut_door", "key_event", "cutscene_seen", "world_state", "npc_quest"}


def reaches_guest(cls, flag, category):
    """True when a shop gate of this class is delivered to guests: a `progress` gate must sit in a category C2
    shares; `item` / `open` gates are per player by design (no requirement). Chalice runtime flags (type 9,
    92905385..: set per dungeon by the engine) are out of scope."""
    if cls != "progress":
        return True
    if flag >= 90000000:
        return True
    return category in SYNCED


def names(game):
    msgs = {n.split("\\")[-1]: d for n, d in ft.bnd4(ft.read_maybe_dcx(Path(game) / "msg/engus/item.msgbnd.dcx")).items()}
    fm = {0: "武器名.fmg", 1: "防具名.fmg", 2: "アクセサリ名.fmg", 3: "アイテム名.fmg"}
    fm[4] = fm[3]

    def text(cat, i):
        d = msgs.get(fm.get(cat, ""))
        if not d:
            return "?"
        gc, = struct.unpack_from("<i", d, 0xC)
        so, = struct.unpack_from("<q", d, 0x18)
        for g in range(gc):
            idx, first, last = struct.unpack_from("<iii", d, 0x28 + g * 0x10)
            if first <= i <= last:
                off, = struct.unpack_from("<q", d, so + 8 * (idx + i - first))
                return ft.utf16z(d, off) if off else "?"
        return "?"
    return text


def build(game):
    game = Path(game)
    P = ft.load_params(game / "param/gameparam/gameparam.parambnd.dcx", game / "paramdef/paramdef.paramdefbnd.dcx",
                       ["ShopLineupParam", "ItemLotParam"])
    emedf = ft.load_emedf(ft.find_emedf(game / "event"))
    allrec = ft.expand_all(game / "event", emedf, cross_map=True)
    text = names(game)
    cats = ft.load_inc(Path(__file__).resolve().parents[2] / "gpu/shim/party/party_flags.inc")

    def cat(f):
        for lo, hi, c, _n in cats:
            if lo <= f <= hi:
                return c
        return "?"

    lots = collections.defaultdict(list)
    for r in P["ItemLotParam"]:
        for k in ("getItemFlagId",):
            if r[k] > 0:
                lots[r[k]].append(r["id"])
    setters = collections.defaultdict(list)
    for stem, recs in allrec.items():
        for r in recs:
            f = r["event"] + r["slot"]
            setters[f].append(f"{stem} event {r['event']} slot {r['slot']} (completion)")
            for o in r["ops"]:
                if o["op"] == "set" and o["state"] == 1:
                    for x in range(o["first"], o["last"] + 1 if o["last"] - o["first"] < 64 else o["first"] + 1):
                        setters[x].append(f"{stem} event {r['event']} slot {r['slot']} sets")
            for m in r["markers"]:
                if m["m"].startswith("boss"):
                    v = m["v"]
                    for x in v if isinstance(v, (list, tuple)) else [v]:
                        if isinstance(x, int) and x > 0:
                            setters[x].append(f"{stem} event {r['event']} {m['m']}")
    gates = collections.defaultdict(list)
    for r in P["ShopLineupParam"]:
        gates[r["qwcId"]].append(r)
    out = []
    for f in sorted(gates):
        rows = gates[f]
        # the 5 shop pages repeat the rows: keep one per (equipType, equipId)
        items = sorted({(r["equipType"], r["equipId"]) for r in rows})
        ids = sorted(r["id"] for r in rows)
        item_names = [text({0: 0, 1: 1, 2: 2, 3: 3}.get(t, 3), i) for t, i in items]
        st = sorted(set(setters.get(f, [])))
        lt = sorted(lots.get(f, []))
        if f < 0:
            cls, why = "open", "no gate"
        elif SHOP_OWN_RANGE[0] <= f <= SHOP_OWN_RANGE[1]:
            cls, why = "item", "event 5500+slot ends when the player owns the weapon (and also ends early for a phantom)"
        elif lt:
            cls, why = "item", "pickup flag of item lot(s) " + ",".join(map(str, lt[:4]))
        elif 6000 <= f <= 8999:
            cls, why = "item", "personal block (6000-8999): this player's own"
        else:
            cls, why = "progress", "; ".join(st[:3]) or "no setter found"
        out.append({"flag": f, "category": cat(f) if f >= 0 else "-", "class": cls, "why": why, "rows": ids[:6],
                    "n_rows": len(ids), "reaches_guest": reaches_guest(cls, f, cat(f) if f >= 0 else "-"), "items": list(zip(items, item_names))[:6], "setters": st[:4], "lots": lt[:4]})
    return out


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--md")
    ap.add_argument("--json")
    ap.add_argument("--check", action="store_true", help="exit 1 when a progress gate never reaches guests")
    a = ap.parse_args()
    out = build(a.game)
    if a.json:
        Path(a.json).write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")
    lines = ["# Messenger shop gates", "",
             "Generated by `tools/party/shop_gates.py` from ShopLineupParam, ItemLotParam and the EMEVD scripts.",
             "Class: `progress` = shared with the party, `item` = per player (the item was obtained / owned), `open` = no gate.", "",
             "| gate flag | category | class | shop rows | items | set by |", "|---|---|---|---|---|---|"]
    bad = [g for g in out if not g["reaches_guest"]]
    for g in out:
        its = ", ".join(f"{n} ({'wpn' if t == 0 else 'armor' if t == 1 else 'ring' if t == 2 else 'goods'} {i})" for (t, i), n in g["items"])
        lines.append(f"| {g['flag']} | {g['category']} | {g['class']} | {g['n_rows']} rows ({g['rows'][0]}..) | {its} | {g['why']} |")
    txt = "\n".join(lines) + "\n"
    if a.md:
        Path(a.md).write_text(txt, encoding="utf-8", newline="\n")
    else:
        print(txt)
    c = collections.Counter(g["class"] for g in out)
    print(dict(c), file=sys.stderr)
    for g in bad:
        print(f"NOT SYNCED: progress gate {g['flag']} is category {g['category']} ({g['items'][0][1] if g['items'] else '?'})", file=sys.stderr)
    return 1 if (bad and a.check) else 0


if __name__ == "__main__":
    sys.exit(main() or 0)
