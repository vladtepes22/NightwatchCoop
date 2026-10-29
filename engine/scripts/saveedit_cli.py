"""JSON interface to the save editor library; no GUI dependencies."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import sys

from saveedit import flagsdata, icons, instances, names, store
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


# Weapon rows the game keeps in every inventory and never shows: 1000 holds the Memory Altar's runes and
# oath in its slots, 1000000 (Bare Fists) fills the empty hand slots. Never listed, never editable.
PLACEHOLDERS = {(0, 1000), (0, 1000000)}
SOCKETED = 3  # item section of a gem sitting in a weapon slot (not in the inventory lists)
GEM_EFFECTS = 3


def gem_sort(gem, catalogue):
    """The GemeffectParam sort category a gem's effects come from: 0 blood gem, 1 Caryll rune, 2 oath rune."""
    if gem.type == 1:
        return 0
    first = catalogue.effect(gem.effects[0]) if gem.effects else None
    return first[3] if first else None


def gem_view(gem, catalogue):
    name, icon, cursed = catalogue.gem(gem.gen, gem.shape, gem.effects)
    return name, icon, dict(handle=gem.handle, type=gem.type, shape=gem.shape, sort=gem_sort(gem, catalogue),
                            cursed=cursed, effects=[dict(id=e, text=catalogue.effect_text(e)) for e in gem.effects])


def describe(model, catalogue, icon_paths=lambda ids: {}):
    flags = [dict(id=f, kind=kind, group=group, label=label, on=bool(model.flag(f)))
             for f, kind, group, label in flag_rows(catalogue) if model.flag(f) is not None]
    table = instances.read(model)
    gems = table.gems if table else {}
    rows = []
    for i in model.items():
        if (i.category, i.number) in PLACEHOLDERS:
            continue
        row = dict(section=i.section, slot=i.slot, kind=i.kind, number=i.number,
                   name=catalogue.name(i.category, i.number), count=i.count, level=i.level,
                   max_level=max_level(catalogue, i), icon=catalogue.icon(i.category, i.number))
        if i.category == 8 and i.handle in gems:
            row["name"], row["icon"], row["gem"] = gem_view(gems[i.handle], catalogue)
        elif i.category == 0 and table and table.weapons.get(i.handle):
            row["sockets"] = [dict(shape=shape, gem=gem_view(gems[h], catalogue)[0] if h else None)
                              for shape, h in table.weapons[i.handle]]
        rows.append(row)
    if table:
        # Only weapons in the inventory: a save also keeps stale weapon records (13 Memory Altar copies on a
        # level 90 save), whose gems the game no longer shows.
        owned = {i.handle for i in model.items((0,)) if i.category == 0}
        held = sorted((g, w) for g, w in table.socketed().items() if w in owned)
        for k, (gh, wh) in enumerate(held):
            name, icon, gem = gem_view(gems[gh], catalogue)
            weapon = table.weapon_ids.get(wh)
            gem["socketed_in"] = dict(number=weapon, name=catalogue.name(0, weapon) if weapon else None)
            rows.append(dict(section=SOCKETED, slot=k, kind="gem", number=gems[gh].gen, name=name, count=1,
                             level=None, max_level=None, icon=icon, gem=gem, locked=True))
    paths = icon_paths({r["icon"] for r in rows if r["icon"]})
    for r in rows:
        r["icon"] = paths.get(r["icon"])
    return dict(revision=revision(model), character=dict(name=model.name, stats=model.stats,
                level=model.level, echoes=model.echoes, insight=model.insight, ng_cycle=model.ng_cycle),
                items=rows,
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
    cache = user / "cache" / "saveedit"
    catalogue = names.Catalogue.load(args.game, cache)
    if args.command == "effects":
        return [dict(id=e, text=text) for e, text in catalogue.effects_for(0)]  # blood gem effects
    model = store.open_slot(user, kind, args.slot)
    if args.command == "show":
        return describe(model, catalogue, lambda ids: icons.ensure(args.game, cache, ids) if args.game else {})
    if not isinstance(edit, dict):
        raise ValueError("expected a JSON edit object")
    if edit.get("revision") != revision(model):
        raise store.EditConflict("the save changed on disk; open it again")
    unknown = set(edit) - {"revision", "name", "stats", "echoes", "insight", "ng_cycle", "items", "flags", "repair",
                           "gems"}
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
        if key in seen or key not in items or (items[key].category, items[key].number) in PLACEHOLDERS:
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
    gem_edits = edit.get("gems", [])
    if gem_edits:
        table = instances.read(model)
        if table is None or not catalogue.effects_for(0):
            raise ValueError("gem effects need the save's gem table and the game's gem list")
        done = set()
        for change in gem_edits:
            if set(change) != {"handle", "effects"} or change["handle"] in done or change["handle"] not in table.gems:
                raise ValueError("invalid or repeated gem")
            gem, effects = table.gems[change["handle"]], change["effects"]
            sort = gem_sort(gem, catalogue)
            # Runes carry hidden effects (no text) that make the rune; only blood gems are edited.
            # At most 3 effects, the most the game rolls (the record has room for 6).
            if (gem.type != 1 or not isinstance(effects, list) or not 1 <= len(effects) <= GEM_EFFECTS or not all(type(e) is int for e in effects)
                    or any((catalogue.effect(e) or [None] * 4)[3] != sort for e in effects)):
                raise ValueError("an effect does not fit this gem")
            instances.set_effects(model, gem, effects)
            done.add(change["handle"])
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
        parser.add_argument("command", choices=("slots", "show", "apply", "backups", "restore", "effects"))
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
