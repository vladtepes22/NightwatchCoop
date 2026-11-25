"""Writes hks_base.json: per top-level unit of the community decomp of the game's behaviour scripts (c0000.hks,
c9999.hks), a hash of its code. The merger tells with it which functions a mod changed; the decomp itself is never
shipped (it is the decompiled game script and carries its authors' header).

  python -I -B -X utf8 gen_hks_base.py <folder holding the decomp's .hks files>
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from merge_hks import _BASE_FILE, units  # noqa: E402


def main(folder):
    table = {"_about": "hashes of the code of each top-level unit of the community decomp (DSLuaDecompiler, refined "
                       "by foxyhooligans) of Bloodborne 1.09's behaviour scripts; made by gen_hks_base.py"}
    for name in sorted(os.listdir(folder)):
        if not name.lower().endswith(".hks"):
            continue
        with open(os.path.join(folder, name), "rb") as f:
            text = f.read().decode("utf-8-sig")
        table[name.lower()] = {key: h for key, _, h in units(text)}
        print("%s: %d units" % (name, len(table[name.lower()])))
    with open(_BASE_FILE, "w", encoding="utf-8", newline="\n") as f:
        json.dump(table, f, indent=0, sort_keys=True, ensure_ascii=True)
        f.write("\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
