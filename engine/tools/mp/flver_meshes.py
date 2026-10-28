#!/usr/bin/env python3
"""The meshes of a character's model and their face sets' index counts (one count per LOD), the numbers the
frame analyzer lists per draw (`index counts` in a BB_CAPTURE_TRIGGER record, docs/upscaler.md): which of a
chr's meshes a frame drew.

  python -I tools/mp/flver_meshes.py GAME/dvdroot_ps4/chr/c2150.chrbnd.dcx

Game data is untrusted input: run with python -I. FLVER2 (Bloodborne 0x20014): header 0x80, dummies 0x40,
materials 0x20 (+0 name, +4 mtd), bones 0x80, meshes 0x30 (+4 material, +0x20 face set count, +0x24 their
indices), face sets 0x20 (+0 flags: 0x1000000 LOD 1, 0x2000000 LOD 2, 0x80000000 the motion-blur copy;
+4 triangle strip; +8 index count).
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "party"))
import flag_tool as ft  # noqa: E402  (BND4 / DCX readers)


def meshes(flver):
    """[(material name, mtd file, [(flags, index count), ...])] of one FLVER2 file's bytes."""
    b = flver
    if b[:6] != b"FLVER\0":
        raise ValueError("not a FLVER")
    u = lambda f, o: struct.unpack_from(f, b, o)
    nd, nm, nb, nme, _nvb = u("<5i", 0x14)
    uni = b[0x49]
    nfs, = u("<i", 0x50)

    def text(o):
        if uni:
            e = o
            while b[e:e + 2] != b"\0\0":
                e += 2
            return b[o:e].decode("utf-16le")
        return b[o:b.index(b"\0", o)].decode("shift_jis", "replace")
    o = 0x80 + nd * 0x40
    mats = [(text(u("<i", o + i * 0x20)[0]), text(u("<i", o + i * 0x20 + 4)[0])) for i in range(nm)]
    mesh0 = o + nm * 0x20 + nb * 0x80
    fs0 = mesh0 + nme * 0x30
    sets = [(u("<I", fs0 + i * 0x20)[0], u("<i", fs0 + i * 0x20 + 8)[0]) for i in range(nfs)]
    out = []
    for i in range(nme):
        mat, = u("<i", mesh0 + i * 0x30 + 4)
        n, at = u("<ii", mesh0 + i * 0x30 + 0x20)
        idx = u("<%di" % n, at)
        out.append((mats[mat][0], mats[mat][1].replace("\\", "/").split("/")[-1], [sets[k] for k in idx]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("chrbnd", help="a .chrbnd.dcx (or .chrbnd) file")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    bnd = ft.bnd4(ft.read_maybe_dcx(a.chrbnd))
    name, data = next((k, v) for k, v in bnd.items() if k.lower().endswith(".flver"))
    print(name.replace("\\", "/").split("/")[-1])
    for i, (mat, mtd, sets) in enumerate(meshes(data)):
        print("mesh %2d  %-24s %-24s %s" % (i, mat, mtd, " ".join("%d%s" % (c, "*" if f & 0x80000000 else "")
                                                                  for f, c in sets)))


if __name__ == "__main__":
    main()
