#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Dev-only test oracle: the party game-data tables derived in Python (not shipped, not run by the game).

The engine derives the tables in C++ (gpu/shim/gamedata/). This is the slow reference implementation the C++
output is compared with (tests/test_party_tables.py): same sections, same cache text, no key.

  python -I -X utf8 scripts/partydata/oracle.py GAME_DIR OUT_FILE
"""
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def game_root(path):
    p = Path(path)
    return p / "dvdroot_ps4" if (p / "dvdroot_ps4").is_dir() else p


def derive_all(game):
    """{section: [row]}: every table, from the player's own files."""
    import ids_tool
    import item_tool
    import npcfog_tool
    import warper_tool
    sections = {}
    item_sections, _audit = item_tool.derive(game)  # first: ids_tool changes flag_tool's marker table for a moment
    sections.update(item_sections)
    id_sections, _npcs = ids_tool.derive(game)
    sections.update(id_sections)
    sections["npcFog"] = npcfog_tool.derive(game)
    sections.update(warper_tool.derive(game))
    import flag_tool
    sections["flagRows"] = [[str(lo), str(hi), cat, flag_tool.esc(note)] for lo, hi, cat, note in flag_tool.category_rows(game)[0]]
    sections.update(_flag_derived(game, ft_mod=flag_tool))
    return sections


HEADSTONES = [(1, [241, 240, 242, 230]), (2, [220, 270, 320]), (3, [280, 250, 211]), (4, [320, 330, 260]), (5, [340, 350, 360])]


def _flag_derived(game, ft_mod):
    """lamps, hostOnlyEvents, gestureNames (the sections the engine derives from the category rows, event names and
    message files)."""
    import struct
    ev = Path(game) / "event"
    allrec = ft_mod.expand_all(ev, ft_mod.load_emedf(ft_mod.find_emedf(ev)))
    rows = ft_mod.category_rows(game, allrec)[0]
    lamps = set()
    for lo, hi, cat, _n in rows:
        if hi - lo > 2000 or cat != "lamp_unlocked":
            continue
        for f in range(lo, hi + 1):
            area = f // 10**5 % 100
            if area == 29 or area >= 40 or not (7 * 10**7 <= f < 8 * 10**7) or f // 100 % 100 != 2:
                continue
            if ft_mod.categorize(rows, f)[0] == "lamp_unlocked":
                lamps.add(f)
    out = []
    for f in lamps:
        aab, n, group, order = f // 10000 % 1000, f % 100, 1, 1000
        o = 0
        for g, maps in HEADSTONES:
            for m in maps:
                o += 1
                if m != aab or (m == 320 and ((g == 2) != (n % 2 == 0))):
                    continue
                group, order = g, o
        out.append((order, aab, n, group))
    host = sorted({r["event"] for recs in allrec.values() for r in recs if "ヒートアップ" in r["name"] and 0 < r["event"] < 2**32})
    names = {}
    msg = Path(game) / "msg"
    langs = ["engus", "enggb"] + (sorted(p.name for p in msg.iterdir()) if msg.is_dir() else [])
    for lang in langs:
        p = msg / lang / "item.msgbnd.dcx"
        if p.is_file():
            ents = ft_mod.bnd4(ft_mod.read_maybe_dcx(p))
            key = next((k for k in ents if k.endswith("アイテム名.fmg")), None)
            if key:
                d = ents[key]
                groups, = struct.unpack_from("<I", d, 0x0c)
                strings, = struct.unpack_from("<I", d, 0x10)
                offs, = struct.unpack_from("<I", d, 0x18)
                for g in range(groups):
                    idx, first, last = struct.unpack_from("<iii", d, 0x28 + g * 16)
                    for i in range(first, last + 1):
                        so, = struct.unpack_from("<q", d, offs + (idx + i - first) * 8)
                        if so:
                            names[i] = ft_mod.utf16z(d, so)
                break
    return {
        "lamps": [[str(a), str(n), str(g)] for _o, a, n, g in sorted(out)],
        "hostOnlyEvents": [[str(e)] for e in host],
        "gestureNames": [[str(i), names[i]] for i in range(800, 823) if names.get(i)],
    }


def render(key, sections):
    lines = ["BBPARTYTABLES 1", f"key {key}"]
    for name in sorted(sections):
        rows = sections[name]
        lines.append(f"[{name}] {len(rows)}")
        for r in rows:
            lines.append("\t".join(r))
    return "\n".join(lines) + "\n"


def main():
    game, out = game_root(sys.argv[1]), Path(sys.argv[2])
    t0 = time.time()
    text = render("oracle", derive_all(game))
    out.write_bytes(text.encode("utf-8"))
    print(f"oracle: {time.time() - t0:.1f} s -> {out}")


if __name__ == "__main__":
    main()
