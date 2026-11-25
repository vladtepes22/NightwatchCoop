"""Nightwatch Co-op mod merger: merges mod folders into a new mod folder.

  python -I -B -X utf8 merger.py merge --game <game dir> --out <new mod folder> --log <log file> --mod <dir> [--mod ...]

`--mod` order is the merge order: when two mods change the same thing, the later one wins. Every log line is printed
too: CONFLICT, NOT MERGED, MERGED, COPIED, INFO, and DONE <files> files, <conflicts> conflicts last. Exit 0 on success;
otherwise "ERROR <message>" on stderr and a non-zero exit. What merges: README.md beside this file.
"""
import argparse
import concurrent.futures
import os
import shutil
import struct
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import MergeError, Report, containers, plural  # noqa: E402
from merge_any import Context, describe, merge_payload, unpack  # noqa: E402
from merge_param import Paramdefs  # noqa: E402
from merge_tables import HOST_ONLY, merge_host_only, merge_tables  # noqa: E402

SERIALS = ("CUSA03173", "CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363", "CUSA03179", "CUSA03014",
           "CUSA03023")
GAME_FOLDERS = frozenset(("action", "adhoc", "chr", "event", "facegen", "font", "map", "menu", "movie", "msg", "mtd",
                          "obj", "other", "param", "paramdef", "parts", "remo", "script", "sfx", "shader", "sound"))
LOOSE = (".txt", ".md", ".jpg", ".png", ".ini")
TMP_MARK = ".nightwatch-merger-tmp"  # in <out>.tmp while a merge runs

# Item names for log lines: PARAM table -> the id of its name FMG in msg\<lang>\item.msgbnd.dcx.
NAME_FMG = {"EquipParamGoods": 10, "EquipParamWeapon": 11, "EquipParamProtector": 12, "EquipParamAccessory": 13,
            "Magic": 14}
ITEM_MSG = "msg/engus/item.msgbnd.dcx"


class Failure(Exception):
    pass


def _entries(folder):
    try:
        return sorted(os.listdir(folder))
    except OSError:
        return []


def _child(folder, name):
    for entry in _entries(folder):
        if entry.lower() == name.lower():
            return os.path.join(folder, entry)
    return os.path.join(folder, name)


def content_root(folder):
    """(base, prefix) like the engine's ContentRoot (engine\\start\\mods.cpp): files under `base` count when their path,
    with `prefix` before it, starts with dvdroot_ps4. None when the folder has no game files."""
    if not os.path.isdir(folder):
        return None
    for wrapper in ("", "app0") + SERIALS:
        base = _child(folder, wrapper) if wrapper else folder
        if os.path.isdir(_child(base, "dvdroot_ps4")):
            return base, ""
    entries = [e for e in _entries(folder) if not e.startswith(".")]
    folders = [e for e in entries if os.path.isdir(os.path.join(folder, e))]
    if folders and all(e.lower() in GAME_FOLDERS for e in folders):
        return folder, "dvdroot_ps4"
    files = [e for e in entries if os.path.isfile(os.path.join(folder, e))]
    if len(folders) == 1 and all(os.path.splitext(e)[1].lower() in LOOSE for e in files):
        return content_root(os.path.join(folder, folders[0]))
    return None


def game_files(folder):
    """{lower-case path under dvdroot_ps4 (/ separated): (path as spelled, file)} of a mod or game folder."""
    found = content_root(folder)
    if found is None:
        raise Failure("no game files in %s (expected dvdroot_ps4, or chr\\, parts\\, ... inside it)" % folder)
    base, prefix = found
    out = {}
    for here, dirs, files in os.walk(base):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(here, name)
            rel = os.path.relpath(path, base).replace("\\", "/")
            if prefix:
                rel = prefix + "/" + rel
            first, _, rest = rel.partition("/")
            if first.lower() != "dvdroot_ps4" or not rest or os.path.islink(path):
                continue
            out[rest.lower()] = (rest, path)
    return out


def update_folder(game):
    """The game's update kept beside it (<game>-update or <game>-patch with an eboot.bin), as the engine finds it."""
    parent, name = os.path.split(os.path.normpath(game))
    for entry in _entries(parent):
        low = entry.lower()
        if low in (name.lower() + "-update", name.lower() + "-patch"):
            path = os.path.join(parent, entry)
            if os.path.isdir(path) and os.path.isfile(_child(path, "eboot.bin")):
                return path
    return None


class Game:
    """The game's files, its update's first."""

    def __init__(self, folder):
        found = content_root(folder)
        if found is None or found[1]:
            raise Failure("no game files in %s (expected its dvdroot_ps4)" % folder)
        self.roots = []
        update = update_folder(folder)
        if update and content_root(update):
            self.roots.append(_child(content_root(update)[0], "dvdroot_ps4"))
        self.roots.append(_child(found[0], "dvdroot_ps4"))

    def path(self, rel):
        for root in self.roots:
            candidate = os.path.join(root, rel.replace("/", os.sep))
            if os.path.isfile(candidate):
                return candidate
        return None

    def read(self, rel):
        path = self.path(rel)
        if path is None:
            return None
        with open(path, "rb") as f:
            return f.read()


def read(path):
    with open(path, "rb") as f:
        return f.read()


class ItemNames:
    """English item names for log lines, read at merge time: the mods' item FMGs (later first), then the game's."""

    def __init__(self, game, mods):
        self.sources = [path for _, files in reversed(mods) for key, (_, path) in files.items() if key == ITEM_MSG]
        self.game = game
        self.lock = threading.Lock()
        self.fmgs = None

    def _load(self):
        layers = []
        for loader in [lambda p=p: read(p) for p in self.sources] + [lambda: self.game.read(ITEM_MSG)]:
            try:
                data = loader()
                if data is None:
                    continue
                bnd = containers.bnd4_read(unpack(data)[0])
                layers.append({entry[1]: containers.fmg_read(entry[3])["texts"] for entry in bnd["entries"]})
            except (MergeError, ValueError, struct.error, IndexError, UnicodeDecodeError, OSError):
                continue
        return layers

    def __call__(self, table, row_id):
        fmg_id = NAME_FMG.get(table.split(" ")[-1])
        if fmg_id is None:
            return None
        with self.lock:
            if self.fmgs is None:
                self.fmgs = self._load()
        for layer in self.fmgs:
            texts = layer.get(fmg_id, {})
            text = texts.get(row_id)
            if text and text != "*":
                return text
            if table.endswith("EquipParamWeapon") and row_id % 100:
                base = texts.get(row_id - row_id % 100)
                if base and base != "*":
                    return "%s +%d" % (base, row_id % 100)
        return None


def merge_one(key, game, mods, ctx, tmp):
    """Merges or copies one game file into the output; returns (lines, conflicts)."""
    shippers = [(name, files[key]) for name, files in mods if key in files]
    shown = shippers[-1][1][0]
    label = shown.replace("/", "\\")
    report = Report(label)
    game_path = game.path(key)
    vanilla = read(game_path) if game_path else None
    if game_path:
        shown = os.path.relpath(game_path, next(r for r in game.roots if game_path.startswith(r))).replace("\\", "/")
    target = os.path.join(tmp, "dvdroot_ps4", shown.replace("/", os.sep))
    contents = [(name, read(path)) for name, (_, path) in shippers]
    changed = [(n, d) for n, d in contents if vanilla is None or d != vanilla]
    if len(changed) > 1:
        # Files that differ only in their DCX packing hold the same content.
        payload = Payloads()
        if vanilla is not None:
            changed = [(n, d) for n, d in changed if not payload.same(d, vanilla)]
        distinct = []
        for n, d in changed:
            if not any(payload.same(d, x) for _, x in distinct):
                distinct.append((n, d))
    else:
        distinct = changed
    winner = None
    if not changed:
        winner = contents[-1]
        report.lines.append("COPIED %s from %s (the same as the game's)" % (label, winner[0]))
    elif len(distinct) == 1:
        winner = changed[-1]
        others = [n for n, _ in changed if n != winner[0]]
        report.lines.append("COPIED %s from %s%s" % (label, winner[0], (" (%s ship%s the same file)" % (
            ", ".join(others), "s" if len(others) == 1 else "")) if others else ""))
    if winner is not None:
        _write(target, winner[1])
        return report
    counts = {}
    try:
        payloads = [(n, unpack(d)) for n, d in changed]
        base, base_header = unpack(vanilla) if vanilla is not None else (None, None)
        merged = merge_payload(report, "", key, base, [(n, p) for n, (p, _) in payloads], ctx, counts)
        header = base_header or next((h for _, (_, h) in reversed(payloads) if h), None)
        data = containers.dcx_write(merged, header) if header else merged
    except MergeError as error:
        report.lines = []  # the failed attempt's lines (NOT MERGED of inner tables included) go
        report.conflicts = 0
        report.not_merged("", [n for n, _ in changed], changed[-1][0], str(error))
        _write(target, changed[-1][1])
        return report
    _write(target, data)
    report.lines.insert(0, "MERGED %s from %d mods: %s, %s" % (
        label, len(changed), describe(counts), plural(report.conflicts, "conflict")))
    return report


class Payloads:
    """Compares files by content: two DCX files with the same payload are the same file."""

    def __init__(self):
        self.cache = {}

    def _payload(self, data):
        if id(data) not in self.cache:
            try:
                self.cache[id(data)] = (data, unpack(data)[0])
            except MergeError:
                self.cache[id(data)] = (data, data)
        return self.cache[id(data)][1]

    def same(self, a, b):
        return a == b or (a[:4] == b"DCX\0" and b[:4] == b"DCX\0" and self._payload(a) == self._payload(b))


def _write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


class Log:
    def __init__(self, path):
        folder = os.path.dirname(os.path.abspath(path))
        os.makedirs(folder, exist_ok=True)
        self.file = open(path, "w", encoding="utf-8", newline="\n")
        self.conflicts = 0

    def __call__(self, line):
        if line.startswith("CONFLICT "):
            self.conflicts += 1
        self.file.write(line + "\n")
        self.file.flush()
        sys.stdout.write(line + "\n")
        sys.stdout.flush()

    def close(self):
        self.file.close()


def run(args):
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        raise Failure("the output folder exists already: %s" % out)
    if not args.mod:
        raise Failure("no --mod given")
    game = Game(args.game)
    mods = []
    for folder in args.mod:
        name = os.path.basename(os.path.normpath(folder))
        if any(name == n for n, _ in mods):
            raise Failure("two mods are named %s" % name)
        if os.path.normcase(os.path.abspath(folder)) == os.path.normcase(out):
            raise Failure("the output folder is one of the mods")
        mods.append((name, game_files(folder)))
    tmp = out + ".tmp"
    log_path = os.path.normcase(os.path.abspath(args.log))
    for folder in (out, tmp):
        if log_path.startswith(os.path.normcase(folder) + os.sep):
            raise Failure("the log file cannot be inside the output folder: %s" % args.log)
    if os.path.exists(tmp):
        # Only a folder this tool made (a merge that was stopped) is removed; anything else there is the user's.
        if not os.path.isfile(os.path.join(tmp, TMP_MARK)):
            raise Failure("%s exists already and was not made by the merger: remove or rename it" % tmp)
        shutil.rmtree(tmp)
    log = Log(args.log)
    try:
        os.makedirs(tmp)
        with open(os.path.join(tmp, TMP_MARK), "w", encoding="utf-8", newline="\n") as f:
            f.write("a merge in progress; removed when it ends\n")
        log("INFO merging %d mods in this order (a later mod wins a conflict): %s" % (
            len(mods), ", ".join(n for n, _ in mods)))
        defs = game.read("paramdef/paramdef.paramdefbnd.dcx")
        paramdefs = Paramdefs(unpack(defs)[0] if defs else None)
        if not paramdefs.defs:
            log("INFO the game's paramdefs cannot be read: param fields are named by their offsets")
        ctx = Context(paramdefs, ItemNames(game, mods))
        keys = sorted({key for _, files in mods for key in files})
        files = 0
        workers = max(1, min(8, os.cpu_count() or 4))
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            jobs = [pool.submit(merge_one, key, game, mods, ctx, tmp) for key in keys]
            for job in jobs:
                report = job.result()
                for line in report.lines:
                    log(line)
                files += 1
        folders = [(name, os.path.abspath(folder)) for (name, _), folder in zip(mods, args.mod)]
        merge_tables(folders, tmp, os.path.basename(out), log)
        merge_host_only(folders, tmp, log)
        for name, folder in folders:
            kept = ("nightwatch.json", "nightwatch", HOST_ONLY)
            extra = sorted(e for e in _entries(folder) if e.lower() not in kept and
                           os.path.isfile(os.path.join(folder, e)))
            if extra:
                log("INFO %s: not copied (beside its game files): %s" % (name, ", ".join(extra)))
        os.remove(os.path.join(tmp, TMP_MARK))
        os.rename(tmp, out)
        log("DONE %d files, %d conflicts" % (files, log.conflicts))
    except BaseException:
        log.close()
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    log.close()


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise Failure(message)


def main(argv=None):
    try:
        sys.stdout.reconfigure(encoding="utf-8", newline="\n")
        sys.stderr.reconfigure(encoding="utf-8", newline="\n")
    except AttributeError:
        pass
    parser = Parser(prog="merger.py", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command")
    merge = sub.add_parser("merge")
    merge.error = parser.error
    merge.add_argument("--game", required=True)
    merge.add_argument("--out", required=True)
    merge.add_argument("--log", required=True)
    merge.add_argument("--mod", action="append", default=[])
    try:
        args = parser.parse_args(argv)
        if args.command != "merge":
            raise Failure("usage: merger.py merge --game <dir> --out <dir> --log <file> --mod <dir> [--mod <dir> ...]")
        run(args)
    except Failure as error:
        sys.stderr.write("ERROR %s\n" % error)
        return 2
    except Exception as error:  # noqa: BLE001  (any other failure: one line for the launcher)
        sys.stderr.write("ERROR %s: %s\n" % (type(error).__name__, error))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
