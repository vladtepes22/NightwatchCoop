#!/usr/bin/env python3
"""SHA-256 of every save file, for the playtest session kit (docs/party/PLAYTEST_RUNBOOK.md, row A2).

  python\python.exe -I scripts\save_hash.py . -o savehash_pre.json      (BEFORE the session, in the bbport folder)
  python\python.exe -I scripts\save_hash.py . -o savehash_post.json     (AFTER the session)

Hashes user\savedata (the single-player saves) and user\savedata_party (the party saves) under the
given game folder (or its user folder). Only relative file names and hashes are written: no player data.
tools/party/session_score.py compares the two files: the single-player saves must be identical.
Standard library only.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path


def hash_saves(path):
    p = Path(path)
    if (p / "user").is_dir():
        p = p / "user"
    files = {}
    for sub in ("savedata", "savedata_party"):
        d = p / sub
        if d.is_dir():
            for f in sorted(d.rglob("*")):
                if f.is_file():
                    h = hashlib.sha256()
                    with open(f, "rb") as fh:
                        for chunk in iter(lambda: fh.read(1 << 20), b""):
                            h.update(chunk)
                    files[f.relative_to(p).as_posix()] = h.hexdigest()
    return {"version": 1, "files": files}


def main(argv=None):
    ap = argparse.ArgumentParser(description="Hash the single-player and party save files.")
    ap.add_argument("dir", help="the bbport folder (or its user folder)")
    ap.add_argument("-o", help="write the JSON here (default: print it)")
    a = ap.parse_args(argv)
    doc = hash_saves(a.dir)
    if not doc["files"]:
        print("no save files found under user\savedata or user\savedata_party", file=sys.stderr)
    text = json.dumps(doc, indent=1)
    if a.o:
        Path(a.o).write_text(text)
        print(f"{len(doc['files'])} save file(s) hashed into {a.o}")
    else:
        print(text)
    return 0 if doc["files"] else 2


if __name__ == "__main__":
    sys.exit(main())
