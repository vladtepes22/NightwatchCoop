"""Round trip of the game's files through the merger's readers and writers: every file of a merged type (BND4 and what
it holds: PARAM, FMG, TPF, TAE, nested BND4) must write back byte-identical (decompressed), and a merge in which no
mod changes anything must give the game's bytes. TAE animations are also each replaced by a copy of themselves (the
merge's way of taking a mod's animation), which must read back equal.

  python -I -B -X utf8 roundtrip.py <game folder> [--jobs N]
"""
import concurrent.futures
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import MergeError, Report, containers  # noqa: E402
from merge_any import bnd_write, kind_of, merge_bnd  # noqa: E402
from merge_tae import Layout, anim_table, arg_sizes, copy_anim, read_anims, write_table  # noqa: E402
from merge_tpf import tpf_read, tpf_write  # noqa: E402


def check_payload(name, data, problems, counts):
    kind = kind_of(name, data)
    if kind is None:
        return
    counts[kind] = counts.get(kind, 0) + 1
    try:
        if kind == "bnd":
            if data[0x30:0x33] != b"\x01\x74\x00":
                counts["bnd (other format, not merged)"] = counts.get("bnd (other format, not merged)", 0) + 1
                counts[kind] -= 1
                return
            bnd = containers.bnd4_read(data)
            if bnd_write(bnd) != data:
                problems.append("%s: BND4 does not write back the same" % name)
            merged, _ = merge_bnd(Report(name), "", data, [], None, {})
            if merged != data:
                problems.append("%s: a merge without changes is not the game's BND4" % name)
            for _, _, inner, body in bnd["entries"]:
                payload = body
                if body[:4] == b"DCX\0":
                    payload, header = containers.dcx_read(body)
                check_payload(name + " > " + inner.replace("/", "\\").rsplit("\\", 1)[-1], payload, problems, counts)
        elif kind == "param":
            if containers.Param(data).write() != data:
                problems.append("%s: PARAM does not write back the same" % name)
        elif kind == "fmg":
            if containers.fmg_write(containers.fmg_read(data)) != data:
                problems.append("%s: FMG does not write back the same" % name)
        elif kind == "tpf":
            if tpf_write(tpf_read(data)) != data:
                problems.append("%s: TPF does not write back the same" % name)
        elif kind == "tae":
            check_tae(name, data, problems, counts)
    except (MergeError, ValueError, struct.error, IndexError, UnicodeDecodeError) as error:
        problems.append("%s: %s: %s" % (name, type(error).__name__, error))


def check_tae(name, data, problems, counts):
    sizes = arg_sizes(data, [])
    anims = read_anims(data, sizes)
    rows = anim_table(data)
    same = bytearray(data)
    write_table(same, rows, rows)
    if bytes(same) != data:
        problems.append("%s: TAE table write is not the same" % name)
    out = bytearray(data)
    copied = [(anim_id, copy_anim(out, data, anims[anim_id])) for anim_id, _ in rows]
    write_table(out, copied, rows)
    check = read_anims(bytes(out), sizes, Layout(data))
    bad = [a for a in anims if check[a].signature != anims[a].signature]
    if bad:
        problems.append("%s: %d animations do not read back after a copy (first %d)" % (name, len(bad), bad[0]))
    counts["tae animations"] = counts.get("tae animations", 0) + len(anims)


def check_file(path, rel):
    problems, counts = [], {}
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] == b"DCX\0":
        try:
            raw = containers.dcx_read(raw)[0]
        except ValueError as error:
            return ["%s: %s" % (rel, error)], counts
    check_payload(rel, raw, problems, counts)
    return problems, counts


def game_files(game):
    root = os.path.join(game, "dvdroot_ps4")
    for here, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            low = name.lower()
            if low.endswith((".dcx", ".tpf", ".fmg", ".param", ".tae")):
                path = os.path.join(here, name)
                yield path, os.path.relpath(path, root).replace("/", "\\")


def run(game, jobs=None, progress=None):
    problems, counts, files = [], {}, 0
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(check_file, path, rel) for path, rel in game_files(game)]
        for future in futures:
            p, c = future.result()
            problems += p
            for k, v in c.items():
                counts[k] = counts.get(k, 0) + v
            files += 1
            if progress and files % 1000 == 0:
                progress("%d files checked" % files)
    return problems, counts, files


def main(argv):
    if not argv or argv[0].startswith("-"):
        sys.exit(__doc__)
    jobs = int(argv[argv.index("--jobs") + 1]) if "--jobs" in argv else None
    problems, counts, files = run(argv[0], jobs, print)
    for p in problems[:500]:
        print("PROBLEM", p)
    print("checked %d files: %s; %d problems" % (files, ", ".join("%s %d" % kv for kv in sorted(counts.items())),
                                                  len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
