#!/usr/bin/env python3
"""Per-player item discovery on the shared enemy drop roll (gpu/shim/party/party_droproll.h):
for every 1.09 enemy drop lot row, what the SAME draw gives two players with different item
discovery. Mirrors the game's row pick (0x17CEAF0, LotRowPick in party_droproll.cpp): weights
basePoint * 100 x (enableLuck ? discovery : 1), truncated, at least 1; one draw walks them in
slot order. Draw space is exact (interval overlay, no sampling).

Game data (read-only, untrusted; run with python -I). GAME = .../dvdroot_ps4.

  python -I tools/party/drop_discovery.py GAME [--pairs 1.0:2.1,1.0:1.7,1.7:2.1,1.0:3.1]

Prints per row class: rows, and for the higher discovery vs the lower: draw share where an item
turned into nothing (must be 0), a luck item into another item, a non-luck item into another item, nothing into an item.
"""
import argparse, struct, sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402

NPC_LOT_OFFSET = 0x2C  # 0x17DD0B0 reads the enemy's lot at NpcParam row +0x2C


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def item(row, i):
    if row[f"lotItemNum0{i}"] == 0 or row[f"lotItemId0{i}"] <= 0:
        return None
    return (row[f"lotItemCategory0{i}"], row[f"lotItemId0{i}"])


def segments(row, d):
    """[(start, end, slot)] over the draw space [0, 1) at discovery d."""
    ent = []
    for i in range(1, 9):
        b = row[f"lotItemBasePoint0{i}"] * 100
        if b <= 0:  # cumulate-only slots: blended by a per-player drop counter, not modelled
            continue
        w = f32(f32(b) * f32(d if row[f"enableLuck0{i}"] else 1.0))
        ent.append((i, max(int(w), 1) if w > 0 else 0))
    total = sum(u for _, u in ent)
    out, acc = [], 0
    for i, u in ent:
        out.append((acc / total, (acc + u) / total, i))
        acc += u
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--pairs", default="1.0:2.1,1.0:1.7,1.7:2.1,1.0:3.1")
    a = ap.parse_args()
    game = Path(a.game)
    pb, db = game / "param" / "gameparam" / "gameparam.parambnd.dcx", game / "paramdef" / "paramdef.paramdefbnd.dcx"
    lay = ft.paramdef_layout(next(v for k, v in ft.bnd4(ft.read_maybe_dcx(db)).items()
                                  if k.replace("/", "\\").endswith("\\NpcParam.paramdef")))
    lot_field = next(n for n, t, off, bit, w in lay if off == NPC_LOT_OFFSET and bit is None)
    P = ft.load_params(pb, db, ["ItemLotParam", "NpcParam"])
    il = {r["id"]: r for r in P["ItemLotParam"]}
    lots = {r[lot_field] for r in P["NpcParam"] if 0 < r[lot_field] < 100_000_000}
    rows, seen = [], set()
    for lot in sorted(lots):
        x = lot
        while x in il and x not in seen:
            seen.add(x)
            rows.append(il[x])
            x += 1
    pairs = [tuple(float(v) for v in p.split(":")) for p in a.pairs.split(",")]
    stats = defaultdict(lambda: [0, 0.0, 0.0, 0.0, 0.0])  # rows, item->nothing, luck->other, plain->luck, gained
    for row in rows:
        slots = [i for i in range(1, 9) if row[f"lotItemBasePoint0{i}"] > 0]
        luck = [i for i in slots if row[f"enableLuck0{i}"] and item(row, i)]
        plain = [i for i in slots if not row[f"enableLuck0{i}"] and item(row, i)]
        if len(slots) <= 1 or not any(row[f"enableLuck0{i}"] for i in slots) or \
                all(row[f"enableLuck0{i}"] for i in slots):
            key = "not affected (one slot, or no / only luck slots)"
        else:
            key = f"{min(len(luck), 2)}{'+' if len(luck) > 2 else ''} luck item slot(s), " \
                  f"{'with' if plain else 'no'} non-luck item"
        st = stats[key]
        st[0] += 1
        if key.startswith("not"):
            continue
        for lo_d, hi_d in pairs:
            lo, hi = segments(row, lo_d), segments(row, hi_d)
            pts = sorted({p for s0, s1, _ in lo + hi for p in (s0, s1)})
            for p0, p1 in zip(pts, pts[1:]):
                m = (p0 + p1) / 2
                i = next(s for s0, s1, s in lo if s0 <= m < s1)
                j = next(s for s0, s1, s in hi if s0 <= m < s1)
                if i == j:
                    continue
                share = (p1 - p0) / len(pairs)
                if item(row, i) and not item(row, j):
                    st[1] += share
                elif item(row, i) and row[f"enableLuck0{i}"]:
                    st[2] += share
                elif item(row, i):
                    st[3] += share
                else:
                    st[4] += share
    print(f"enemy lot rows (NpcParam.{lot_field} and their chains): {len(rows)}; pairs {a.pairs}")
    print("rows  item->nothing  luck item->other  non-luck->other  nothing->item  class (draw share per row)")
    for key, (n, lost, swap, trade, gain) in sorted(stats.items()):
        n1 = max(n, 1)
        print(f"{n:4}  {lost / n1:13.4f}  {swap / n1:16.4f}  {trade / n1:15.4f}  {gain / n1:13.4f}  {key}")
    bad = sum(v[1] for v in stats.values())
    one_luck_swap = sum(v[2] for k, v in stats.items() if k.startswith("1 "))
    print(f"item->nothing anywhere: {bad:.6f}; a single luck item lost: {one_luck_swap:.6f}")
    return 1 if bad or one_luck_swap else 0


if __name__ == "__main__":
    sys.exit(main())
