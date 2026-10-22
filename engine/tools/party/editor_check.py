#!/usr/bin/env python3
"""Would Noxde's Bloodborne save editor read this character save? (a port of its parse path)

  python -I tools/party/editor_check.py USERDATA [...]

Noxde/Bloodborne-save-editor (GPL-3, https://github.com/Noxde/Bloodborne-save-editor, read at commit
d8d0365) finds everything by searching, not by the save's header (src-tauri/src/data_handling):
offsets.rs (the gems from 0x54 in 40-byte records; "FACE" from 0xF000, the inventory 34028 bytes
before it, the name 469 bytes before that; "LCED" after "FACE"), position.rs (the first
FF FF FF FF 00*8 from "LCED" on; the three floats after it are the position; Coordinates::new(..).unwrap()
panics when there is none) and save.rs (SaveData::build: an Err of any step -> "Failed to load file").
This port follows those steps and says where the result would be wrong:

  * no "FACE": the editor refuses the file;
  * no "LCED" (a save the game wrote in another player's world: its world-state section has three
    empty blocks, docs/party/save_safety.md 5): the editor searches from byte 0, takes the first empty
    inventory entry for the position (y reads as NaN) and its position edit writes into the inventory;
  * no FF*4 00*8 after it: the editor panics (the app closes).

Exit 0 when the editor would read the save as the game wrote it, 1 otherwise. Saves are untrusted input:
run with python -I.
"""
import math
import struct
import sys
from pathlib import Path

GEMS = [bytes([1, 0, 0, 0, s, 0, 0, 0]) for s in (1, 2, 4, 8, 0x3f)] + [bytes([2, 0, 0, 0, s, 0, 0, 0]) for s in (1, 2)]
POS_MARK = b"\xff\xff\xff\xff" + bytes(8)
USERNAME_TO_INV = 469
FACE_TO_INV = 34028


def parse(data: bytes) -> dict:
    """The offsets and values the editor would use, and "problems": what would go wrong (empty: none)."""
    b = bytes(data)
    out = {"problems": []}
    i = 84
    while i + 16 <= len(b) and b[i + 8:i + 16] in GEMS:
        i += 40
    out["upgrades"] = (i - 84) // 40
    face = b.find(b"FACE", 0xF000)
    if face < 0:
        out["problems"].append('no "FACE": the editor refuses the file ("Failed to load file")')
        return out
    inv = face - FACE_TO_INV
    name_at = inv - USERNAME_TO_INV
    if name_at < 147:
        out["problems"].append("the name would lie before the start of the file: the editor crashes")
        return out
    out["name"] = bytes(b[name_at + 1:name_at + 33:2]).split(b"\0")[0].decode("latin-1")
    out["level"] = struct.unpack_from("<I", b, name_at - 23)[0]
    lced = b.find(b"LCED", face)
    out["lced"] = lced if lced >= 0 else None
    start = lced if lced >= 0 else 0
    j = b.find(POS_MARK, start)
    if j < 0 or j + 24 > len(b):
        out["problems"].append("no position after LCED: the editor panics (Coordinates::new unwrap) and closes")
        return out
    out["position_at"] = j + 12
    out["position"] = struct.unpack_from("<fff", b, j + 12)
    if lced < 0:
        out["problems"].append(f'no "LCED" (the world state is missing: a save written in another player\'s '
                               f"world): the editor reads the position at {j + 12:#x}, inside the character "
                               f"data, as {tuple(round(v, 2) for v in out['position'])}, and a position edit "
                               f"would overwrite bytes there")
    elif any(math.isnan(v) or abs(v) > 1e5 for v in out["position"]):
        out["problems"].append(f"the position read is not one: {out['position']}")
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    bad = 0
    for p in argv[1:]:
        r = parse(Path(p).read_bytes())
        ok = not r["problems"]
        bad += not ok
        pos = r.get("position")
        lced = f"{r['lced']:#x}" if r.get("lced") is not None else "none"
        where = f", position {tuple(round(v, 2) for v in pos)}" if pos else ""
        print(f"{p}: {'OK' if ok else 'PROBLEM'}: name {r.get('name')!r}, level {r.get('level')}, LCED {lced}{where}")
        for prob in r["problems"]:
            print("  " + prob)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
