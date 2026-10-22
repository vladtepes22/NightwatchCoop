"""Export a character save for a save editor, and import the edited file back (launcher, Saves).

A save the game wrote while the player was a guest in another player's world has an empty
world-state section: the header's +0x3C/+0x40 section holds three blocks {u32 size, data} "CHR ",
"OBJ ", "DECL", and in another world the game writes them from its summon snapshot, which a party
join never fills (docs/party/save_safety.md 5). Save editors that find the position by the "LCED"
bytes of the DECL block (Noxde's Bloodborne save editor) then read and edit the wrong bytes. The live
save is never changed for that: the export works on a copy.

export_save(user, kind, slot, dest)
    Copies <user>\\<savedata|savedata_party>\\...\\userdata000N into dest (same file name) and, when
    its world-state section is empty, puts minimal world-state blocks in (each block: magic, version,
    the empty list's end marker; what the game writes for a world with nothing saved), moving the rest
    of the data behind them. Writes dest\\bbport_export.json (what was changed, the source's hash,
    the character) and, for a party save, the party files' lines of that slot (party_files\\).
import_save(user, file, force=False)
    Checks the edited file (size, save version, header, the same kind and slot as exported via the
    manifest beside it), refuses when the live save changed since the export unless force (ImportNeedsConfirm
    says why), backs the live folder up to <user>\\save_backups\\<folder>\\<time>-before-editor-import,
    takes the minimal blocks out again (the live format stays as the game wrote it), writes the file
    into the slot (a temp file renamed into place) and, for a party save, updates the slot's line in
    party_characters.txt so a renamed character keeps its party state.

Saves are untrusted input; nothing here runs code from them.
"""
import hashlib
import json
import os
import re
import shutil
import struct
import time
from pathlib import Path

SLOT_SIZE = 0x140000
SAVE_VERSION = 0x41
KINDS = {"solo": "savedata", "party": "savedata_party"}
MANIFEST = "bbport_export.json"
BACKUPS = "save_backups"
EMPTY_WORLD = bytes(12)
# Minimal world-state blocks: {u32 size} + magic (4cc and version, as the writer 0x156AA50 puts them) +
# the list end {u32 0xffffffff, u32 0} + u32 0 (the writer's 8 + entries + 0xc bytes, with no entries).
_END = b"\xff\xff\xff\xff" + bytes(8)
MIN_WORLD = b"".join(struct.pack("<I", 20) + magic + _END for magic in (
    b" RHC\x00\x17\x09\x15", b" JBO\x00\x17\x09\x15", b"LCED\x00\x03\x02\x15"))
PARTY_FILES = ("party_rot.txt", "party_stain.txt", "party_prologue.txt", "party_gestures.txt",
               "party_summoned_gives.txt", "party_owed_lots.txt", "party_characters.txt")


class SaveFormatError(ValueError):
    pass


class ImportNeedsConfirm(Exception):
    """The import would replace something the player should agree to first (the message says what)."""


def _u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def layout(d: bytes) -> dict:
    """The header's world-state entries and the end of the data (the rest of the file is the game's
    unused buffer). SaveFormatError when they do not line up."""
    if len(d) != SLOT_SIZE:
        raise SaveFormatError(f"{len(d)} bytes, expected {SLOT_SIZE}")
    if _u32(d, 0) != SAVE_VERSION:
        raise SaveFormatError(f"save version {_u32(d, 0):#x}, expected {SAVE_VERSION:#x}")
    a, an, b, bspan, c, cn = struct.unpack_from("<6I", d, 0x3c)
    if not (a >= 0x54 and an >= 12 and a + an == b and bspan >= an and b + bspan - an == c and c + cn + 8 <= len(d)):
        raise SaveFormatError("the save header's sections do not line up")
    end = c + cn + 8 + _u32(d, c + cn + 4) + 27  # the {u16, u16, u32 size, data} block (0x175A4E0), 27 bytes
    if end > len(d):
        raise SaveFormatError("the save's data runs past the end of the file")
    at, blocks = a, []
    for magic in (b" RHC", b" JBO", b"LCED"):
        n = _u32(d, at) if at + 4 <= a + an else None
        if n is None or at + 4 + n > a + an or (n and d[at + 4:at + 8] != magic):
            raise SaveFormatError("the world-state section is damaged")
        blocks.append(n)
        at += 4 + n
    if at != a + an:
        raise SaveFormatError("the world-state section is damaged")
    return {"a": a, "an": an, "b": b, "bspan": bspan, "c": c, "end": end, "blocks": blocks}


def world_empty(d: bytes) -> bool:
    return not any(layout(d)["blocks"])


def _replace_world(d: bytes, new: bytes, tail: bytes = b"") -> bytes:
    """d with its world-state section replaced by new; the data after it moves, the header follows.
    tail: the bytes for the end of the data when it moves back (the file's unused buffer as it was)."""
    L = layout(d)
    delta = len(new) - L["an"]
    if L["end"] + delta > len(d):
        raise SaveFormatError("no room in the file for the world-state blocks")
    out = bytearray(d)
    rest = d[L["a"] + L["an"]:L["end"]]
    out[L["a"]:L["a"] + len(new)] = new
    out[L["a"] + len(new):L["a"] + len(new) + len(rest)] = rest
    if delta < 0:
        gap = L["end"] + delta
        out[gap:L["end"]] = (tail + bytes(-delta))[:-delta]
    struct.pack_into("<4I", out, 0x40, len(new), L["b"] + delta, L["bspan"] + delta, L["c"] + delta)
    assert len(out) == len(d)
    return bytes(out)


def _name_bytes(d: bytes):
    """The character name's UTF-16 bytes (save_decode.py / runtime_savepolicy.c slot_character's reading)."""
    m = re.search(rb"(?:[A-Za-z\xc0-\xff]\x00){4,16}\x00\x00", d[0x18:0x40000])
    return m.group()[:-2] if m else b""


def character(d: bytes) -> dict:
    nb = _name_bytes(d)
    m = re.search(re.escape(nb) + b"\x00\x00", d[:0x40000]) if nb else None
    level = _u32(d, m.start() - 0x18) if m and m.start() >= 0x18 else None
    return {"name": nb.decode("utf-16le", "replace"), "level": level if level is not None and level < 1000 else None,
            "play_ms": _u32(d, 8)}


def _fnv(data: bytes) -> int:
    h = 1469598103934665603
    for x in data:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def slot_files(user, kind: str) -> dict:
    """{slot: path} of the character saves of one kind (solo / party)."""
    root = Path(user) / KINDS[kind]
    out = {}
    for p in sorted(root.glob("*/*/SPRJ0005/userdata000[0-9]")):
        out.setdefault(int(p.name[-1]), p)
    return out


def list_saves(user) -> list:
    """Every character save: dicts kind, slot, path, name, level, play_ms, guest_written (empty world state)."""
    rows = []
    for kind in KINDS:
        for slot, p in slot_files(user, kind).items():
            try:
                d = p.read_bytes()
                if not any(d[:16]):
                    continue
                c = character(d)
                guest = world_empty(d)
            except (OSError, SaveFormatError):
                continue
            rows.append(dict(kind=kind, slot=slot, path=p, guest_written=guest, **c))
    return rows


def describe(row: dict) -> str:
    h, m = divmod(row["play_ms"] // 60000, 60)
    kind = "Party save" if row["kind"] == "party" else "Single-player save"
    note = " - last saved as a guest in another world" if row.get("guest_written") else ""
    return f"{kind}, slot {row['slot'] + 1}: {row['name'] or '?'}, level {row['level']}, {h} h {m:02d} min{note}"


def editor_copy(d: bytes):
    """(the copy for an editor, what was changed: a list for the manifest)."""
    L = layout(d)
    if any(L["blocks"]):
        return d, []
    delta = len(MIN_WORLD) - L["an"]
    ch = {"what": "world_state", "original_hex": d[L["a"]:L["a"] + L["an"]].hex(), "inserted_hex": MIN_WORLD.hex(),
          "tail_hex": d[L["end"]:L["end"] + delta].hex()}
    return _replace_world(d, MIN_WORLD), [ch]


def restore_from_editor(d: bytes, changed: list) -> bytes:
    """What editor_copy changed put back as the game wrote it, where the editor left it as exported."""
    for ch in changed:
        L = layout(d)
        if ch.get("what") == "world_state" and d[L["a"]:L["a"] + L["an"]] == bytes.fromhex(ch["inserted_hex"]):
            d = _replace_world(d, bytes.fromhex(ch["original_hex"]), bytes.fromhex(ch.get("tail_hex", "")))
            layout(d)
    return d


def _sha(d: bytes) -> str:
    return hashlib.sha256(d).hexdigest()


def export_save(user, kind: str, slot: int, dest) -> Path:
    """Copy one character save to dest for a save editor (see the module doc). Returns the file."""
    src = slot_files(user, kind).get(slot)
    if src is None:
        raise FileNotFoundError(f"no {kind} save in slot {slot + 1}")
    d = src.read_bytes()
    out, changed = editor_copy(d)
    info = {"kind": kind, "slot": slot, "source": str(src.relative_to(Path(user))), "source_sha256": _sha(d),
            "exported": time.strftime("%Y-%m-%d %H:%M:%S"), "character": character(d), "changed": changed}
    info["exported_sha256"] = _sha(out)
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    target = dest / src.name
    target.write_bytes(out)
    if kind == "party":
        side = dest / "party_files"
        for name in PARTY_FILES:
            p = Path(user) / name
            if p.is_file():
                side.mkdir(exist_ok=True)
                shutil.copy2(p, side / name)
        info["party_files"] = sorted(x.name for x in side.glob("*")) if side.is_dir() else []
    (dest / MANIFEST).write_text(json.dumps(info, indent=1), encoding="utf-8")
    return target


def _backup(user, kind: str) -> Path:
    folder = Path(user) / KINDS[kind]
    base = Path(user) / BACKUPS / KINDS[kind]
    stamp = time.strftime("%Y%m%d-%H%M%S")
    final, n = base / f"{stamp}-before-editor-import", 2
    while final.exists():
        final, n = base / f"{stamp}-before-editor-import-{n:02d}", n + 1
    shutil.copytree(folder, final, ignore=shutil.ignore_patterns("*.bbtmp"))
    for p in folder.rglob("*"):
        if p.is_file() and not p.name.endswith(".bbtmp"):
            q = final / p.relative_to(folder)
            if not q.is_file() or q.stat().st_size != p.stat().st_size:
                shutil.rmtree(final, ignore_errors=True)
                raise OSError(f"could not back up {folder}; nothing changed")
    return final


def _update_party_characters(user, slot: int, d: bytes):
    """party_characters.txt remembers each party slot's character (name hash, play time); the game
    drops a slot's party state when they change (runtime_savepolicy_side_files). An edited name or
    play time is the same character: its line follows the import."""
    p = Path(user) / "party_characters.txt"
    if not p.is_file():
        return
    lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
    want = f"slot {slot} used 1 play {_u32(d, 8)} name {_fnv(_name_bytes(d)):016x}"
    lines = [want if ln.startswith(f"slot {slot} ") else ln for ln in lines]
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")


def check_import(user, file) -> dict:
    """What an import of file would do: {"manifest", "target", "data" (to write), "notes" (to confirm)}."""
    file = Path(file)
    man_path = file.parent / MANIFEST
    if not man_path.is_file():
        raise SaveFormatError(f"{MANIFEST} is not next to {file.name}: import a file exported by the launcher")
    man = json.loads(man_path.read_text(encoding="utf-8"))
    kind, slot = man.get("kind"), man.get("slot")
    if kind not in KINDS or not isinstance(slot, int) or not 0 <= slot <= 9:
        raise SaveFormatError(f"{MANIFEST} names no save slot")
    d = file.read_bytes()
    layout(d)  # SaveFormatError: the editor broke the file
    d = restore_from_editor(d, man.get("changed", []))
    target = slot_files(user, kind).get(slot)
    if target is None:
        target = Path(user) / man["source"]
    notes = []
    if target.is_file():
        live = target.read_bytes()
        if _sha(live) != man.get("source_sha256"):
            notes.append("the game saved this slot after the export: importing replaces that newer progress")
        if _name_bytes(live) != _name_bytes(d):
            notes.append(f"the character's name changes from {character(live)['name']!r} to {character(d)['name']!r}")
    if _sha(file.read_bytes()) == man.get("exported_sha256"):
        notes.append("the file was not changed since the export")
    return {"manifest": man, "kind": kind, "slot": slot, "target": target, "data": d, "notes": notes}


def import_save(user, file, force=False) -> dict:
    """Write an edited export back into its slot (see the module doc). Returns check_import's dict plus
    "backup"; ImportNeedsConfirm (unless force) when check_import has notes."""
    plan = check_import(user, file)
    serious = [n for n in plan["notes"] if not n.startswith("the file was not changed")]
    if serious and not force:
        raise ImportNeedsConfirm("; ".join(serious))
    target = Path(plan["target"])
    plan["backup"] = _backup(user, plan["kind"]) if (Path(user) / KINDS[plan["kind"]]).is_dir() else None
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name(target.name + ".editor-import")
    tmp.write_bytes(plan["data"])
    if tmp.read_bytes() != plan["data"]:
        tmp.unlink()
        raise OSError(f"could not write {tmp}")
    os.replace(tmp, target)
    if plan["kind"] == "party":
        _update_party_characters(user, plan["slot"], plan["data"])
    return plan
