#!/usr/bin/env python3
"""Make a test save with a LIVE Chalice boss from chalice_pthumeru_inside (layer 1 boss dead).

  python -I tools/party/make_chalice_boss_fixture.py SRC_SAVEDATA DST_SAVEDATA [--flags ID,ID...]

Copies the save folder and clears the layer-1 boss's "defeated" flags of the dungeon in slot 1 (area 29 ids
12901xxx are stored as 1410xxxx there: 14101800 = boss defeated, 14101809 / 14101686 / 14101693 / 14101708 =
its follow-up events and lots), using scripts/saveedit (the launcher's save editor). The player keeps its
position in the boss room (the lamp 2901952's flags stay). Test saves only, never the live savedata.
"""
import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from saveedit.model import SaveModel  # noqa: E402

DEFAULT_FLAGS = [14101800, 14101809, 14101686, 14101693, 14101708]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--flags", default=",".join(map(str, DEFAULT_FLAGS)))
    a = ap.parse_args()
    src, dst = Path(a.src), Path(a.dst)
    if "bloodborne" + chr(92) + "user" in str(src.resolve()).lower() or dst.exists():
        sys.exit("refusing: the live savedata as a source, or the destination exists")
    shutil.copytree(src, dst)
    flags = [int(x) for x in a.flags.split(",") if x]
    n = 0
    for f in sorted(dst.rglob("userdata0000")):
        m = SaveModel(f.read_bytes())
        for fid in flags:
            if m.flag(fid):
                m.set_flag(fid, False)
                n += 1
        f.write_bytes(m.to_bytes())
        print("%s: cleared %d of %d flags" % (f, n, len(flags)))


if __name__ == "__main__":
    main()
