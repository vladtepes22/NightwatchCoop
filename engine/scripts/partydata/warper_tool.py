#!/usr/bin/env python3
"""Bloodborne 1.09 equipment for the world warper's gear rotation (I3):
derived at run time from the player's own params (the engine: cache sections warperRight /
warperLeft / warperArmor, read by gpu/shim/party/world_warper_rules.h); nothing generated is committed.

Game data (read-only, untrusted; run with python -I). GAME = .../dvdroot_ps4.

  python -I scripts/partydata/warper_tool.py GAME     (prints the three lists)

Tables (sorted ascending, no duplicates):
  kWarperRightWeapons  EquipParamWeapon base rows (id % 10000 == 0, a menu sortId) that the right
                       hand takes (rightHandEquipable), one per equipModelId: every trick weapon's
                       look once (the uncanny / lost variants share the model).
  kWarperLeftWeapons   the same for the left hand (firearms, torch, shield, cannon...).
  kWarperArmorSets     EquipParamProtector head rows (headEquip) of id >= 10000 with a menu
                       sortId, one per equipModelId; the set's other pieces are head + 1000
                       (chest), + 2000 (arms), + 3000 (legs) when those rows exist (else that
                       slot keeps what it has).
"""
import argparse, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402


def load(game, pairs):
    game = Path(game)
    pb = ft.bnd4(ft.read_maybe_dcx(game / "param" / "gameparam" / "gameparam.parambnd.dcx"))
    db = ft.bnd4(ft.read_maybe_dcx(game / "paramdef" / "paramdef.paramdefbnd.dcx"))

    def find(d, name, ext):
        return next(v for k, v in d.items() if k.replace("/", "\\").endswith("\\" + name + ext))

    return {p: list(ft.param_rows(find(pb, p, ".param"), ft.paramdef_layout(find(db, d, ".paramdef"))))
            for p, d in pairs}


def one_per_model(rows):
    seen, out = set(), []
    for r in sorted(rows, key=lambda r: r["id"]):
        if r["equipModelId"] in seen:
            continue
        seen.add(r["equipModelId"])
        out.append(r["id"])
    return out


def tables(P):
    weapons = [r for r in P["EquipParamWeapon"]
               if r["id"] >= 1000000 and r["id"] % 10000 == 0 and r["sortId"] >= 0]
    right = one_per_model(r for r in weapons if r["rightHandEquipable"])
    left = one_per_model(r for r in weapons if r["leftHandEquipable"] and not r["rightHandEquipable"])
    heads = [r for r in P["EquipParamProtector"] if r["id"] >= 10000 and r["headEquip"] and r["sortId"] >= 0]
    armor = one_per_model(heads)
    return {"kWarperRightWeapons": right, "kWarperLeftWeapons": left, "kWarperArmorSets": armor}


def derive(game):
    """{section: [[id], ...]} for the cache: warperRight, warperLeft, warperArmor."""
    P = load(game, [("EquipParamWeapon", "EquipParamWeapon"), ("EquipParamProtector", "EquipParamProtector")])
    t = tables(P)
    return {"warperRight": [[str(v)] for v in t["kWarperRightWeapons"]],
            "warperLeft": [[str(v)] for v in t["kWarperLeftWeapons"]],
            "warperArmor": [[str(v)] for v in t["kWarperArmorSets"]]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    a = ap.parse_args()
    for k, v in derive(a.game).items():
        print(k, len(v), " ".join(r[0] for r in v))


if __name__ == "__main__":
    main()
