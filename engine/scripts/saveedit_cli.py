"""JSON interface to the save editor library; no GUI dependencies."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import sys

from saveedit import flagsdata, names, store
from save_editor import _update_party_characters


def running_games():
    """Fail closed if the Windows process snapshot cannot be inspected."""
    if os.name != "nt":
        raise OSError("process inspection requires Windows")

    class Entry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
                    ("pid", wintypes.DWORD), ("heap", ctypes.c_size_t),
                    ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG),
                    ("flags", wintypes.DWORD), ("name", wintypes.WCHAR * 260)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = Entry()
        entry.size = ctypes.sizeof(entry)
        if not kernel.Process32FirstW(snapshot, ctypes.byref(entry)):
            raise ctypes.WinError(ctypes.get_last_error())
        found = []
        while True:
            if entry.name.lower() in ("bb-probe.exe", "bb-start.exe"):
                found.append(entry.pid)
            if not kernel.Process32NextW(snapshot, ctypes.byref(entry)):
                if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                    raise ctypes.WinError(ctypes.get_last_error())
                break
        return found
    finally:
        kernel.CloseHandle(snapshot)


def revision(model):
    return hashlib.sha256(model.orig).hexdigest()


def flag_rows(catalogue):
    """[(id, kind, group, label)] of the flags the editor offers, in display order."""
    rows = [(f, "boss", g, label) for f, g, label in flagsdata.bosses(catalogue)]
    rows += [(f, "lamp", g, label) for f, g, label in flagsdata.lamps(catalogue)]
    rows += [(f, "ending", g, label) for f, g, label in flagsdata.endings()]
    return rows


def max_level(catalogue, item):
    """Highest reinforcement level the game has a row for, None when the item or the game list is unknown."""
    ids = catalogue.valid_ids(0)
    if item.level is None or not ids:
        return None
    base = item.number - item.level * 100
    return max((lv for lv in range(item.level + 11) if base + 100 * lv in ids), default=None)


def describe(model, catalogue):
    flags = [dict(id=f, kind=kind, group=group, label=label, on=bool(model.flag(f)))
             for f, kind, group, label in flag_rows(catalogue) if model.flag(f) is not None]
    return dict(revision=revision(model), character=dict(name=model.name, stats=model.stats,
                level=model.level, echoes=model.echoes, insight=model.insight, ng_cycle=model.ng_cycle),
                items=[dict(section=i.section, slot=i.slot, kind=i.kind, number=i.number,
                            name=catalogue.name(i.category, i.number), count=i.count, level=i.level,
                            max_level=max_level(catalogue, i))
                       for i in model.items()],
                flags=flags,
                findings=dict(phantom=bool(model.phantom_mark),
                              duplicate_keys=[catalogue.name(4, n) or str(n) for n in model.duplicate_keys()],
                              world_empty=bool(model.world_empty)))


REPAIRS = ("phantom", "duplicate_keys")


def execute(args, edit=None, running=running_games):
    user, kind = args.user, args.kind
    if args.command == "slots":
        return [dict(slot=r["slot"], name=r["name"], level=r["level"],
                     echoes=store.open_slot(user, kind, r["slot"]).echoes)
                for r in store.list_slots(user, kind)]
    if args.command == "backups":
        return store.list_backups(user, kind)
    if args.command == "restore":
        backup = next((r["path"] for r in store.list_backups(user, kind)
                       if r["name"] == args.backup), None)
        if backup is None:
            raise ValueError("unknown backup")
        return {"backup": store.restore_backup(user, kind, backup, running)}
    model = store.open_slot(user, kind, args.slot)
    catalogue = names.Catalogue.load(args.game, user / "cache" / "saveedit")
    if args.command == "show":
        return describe(model, catalogue)
    if not isinstance(edit, dict):
        raise ValueError("expected a JSON edit object")
    if edit.get("revision") != revision(model):
        raise store.EditConflict("the save changed on disk; open it again")
    unknown = set(edit) - {"revision", "name", "stats", "echoes", "insight", "ng_cycle", "items", "flags", "repair"}
    if unknown:
        raise ValueError("unknown edit fields")
    if "name" in edit:
        model.set_name(edit["name"])
    for stat, value in edit.get("stats", {}).items():
        model.set_stat(stat, value)
    for field in ("echoes", "insight", "ng_cycle"):
        if field in edit:
            getattr(model, "set_" + field)(edit[field])
    items = {(i.section, i.slot): i for i in model.items()}
    seen = set()
    for change in edit.get("items", []):
        if set(change) - {"section", "slot", "count", "level", "remove"}:
            raise ValueError("unknown item fields")
        key = (change["section"], change["slot"])
        if key in seen or key not in items:
            raise ValueError("invalid or repeated item slot")
        seen.add(key)
        item = items[key]
        if change.get("remove"):
            model.remove(item)
        else:
            if "count" in change:
                model.set_count(item, change["count"])
            if "level" in change:
                model.set_weapon_level(item, change["level"], catalogue.valid_ids(0))
    offered = {f for f, *_ in flag_rows(catalogue) if model.flag(f) is not None}
    for change in edit.get("flags", []):
        if set(change) - {"id", "on"} or change.get("id") not in offered or not isinstance(change.get("on"), bool):
            raise ValueError("invalid flag edit")
        model.set_flag(change["id"], change["on"])
    repairs = edit.get("repair", [])
    if not isinstance(repairs, list) or set(repairs) - set(REPAIRS):
        raise ValueError("unknown repair")
    if "phantom" in repairs:
        model.repair_phantom_mark()
    if "duplicate_keys" in repairs:
        model.merge_duplicate_keys()
    return {"backup": store.apply_edit(user, kind, args.slot, model, running, _update_party_characters)}


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise ValueError(message)


def main(argv=None, stdin=None, stdout=None, running=running_games):
    stdout = stdout or sys.stdout
    try:
        parser = Parser(description=__doc__)
        parser.add_argument("command", choices=("slots", "show", "apply", "backups", "restore"))
        parser.add_argument("--user", type=Path, required=True)
        parser.add_argument("--kind", choices=tuple(store.KINDS), required=True)
        parser.add_argument("--slot", type=int)
        parser.add_argument("--game", type=Path)
        parser.add_argument("--backup")
        args = parser.parse_args(argv)
        if args.command in ("show", "apply") and args.slot not in range(10):
            raise ValueError("slot must be 0 to 9")
        edit = json.load(stdin or sys.stdin) if args.command == "apply" else None
        result = execute(args, edit, running)
        print(json.dumps(result, default=str, ensure_ascii=True), file=stdout)
        return 0
    except Exception as error:
        message = "close the game before changing saves" if isinstance(error, store.GameRunning) else str(error)
        print(json.dumps({"error": message, "type": type(error).__name__}), file=stdout)
        return 1


if __name__ == "__main__":
    sys.exit(main())
