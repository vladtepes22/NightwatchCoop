r"""Copy a party character to the single-player save folder, cleaned so it plays offline.

A character that played in a party carries state only the party mode understands. The pure part,
`clean_for_offline(data)`, takes the bytes of one slot file (userdata000N) and returns the copy to
write and a report of what it changed; `copy_party_to_offline(user, party_slot, solo_slot)` backs the
single-player folder up and writes it. Nothing here touches the party folder or runs while a game runs
(the launcher checks that). Saves are untrusted input; nothing here runs code from them.

What the cleaning does (evidence: docs/party/save_layout.md, chalice.md 4 and 9, DEBUG_SWITCHES.md
BB_PARTY_OWNTYPE_FIX, tests/test_party_offline.py on the fixtures of docs/party/test_saves.md):
  * the remembered chr type (PlayerGameData +0xA4) a summoned guest's save keeps (1 = phantom) goes back
    to 0, so the own world builds a normal hunter (otherwise: an own-world soft lock / no revive);
  * chalice slot 0, the network slot a quick join writes the host's dungeon into (flags
    94000000 + 0..: ChannelId, map 0x1dxxxxxx, layout, lamps, the case flags 94005300 + n), is put back
    to the never-used state of a fresh save (identical in every unused slot of every test save).
    The character's own dungeons (slots 1 to 6, also a host's dungeon the guest took home with
    progress) stay: they are the character's progress.
Left alone, on purpose: event flags and bosses / lamps / quests (shared progress is the character's own
after the party), items, the NG cycle (it only changes on an ending), the empty world-state blocks a
guest-written save has (the game itself loads those at home), and the party's side files
(owed items, summoned gives, bloodstains, in <user>\party_*.txt: not part of the save, so a copy to the
single-player folder never carries them).
Warned about, not changed: a character last saved inside a dungeon whose map is not in its own slots.
"""
import os
import re
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from saveedit.model import SaveModel, SaveFormatError, group_key, SLOT_SIZE  # noqa: E402
from saveedit import store  # noqa: E402

SOLO = "savedata"
PARTY = "savedata_party"
SLOT_BASE = 94000000
SLOT_STRIDE = 100000
SLOTS = 7  # network slot 0 and the member's own ritual slots 1..6
NO_RECORD = bytes.fromhex("ffffffff")
# group bytes of a never-used chalice slot (flags 94xxx000 and 94xxx001 .. 94xxx999 of z = 0 and 1; z >= 2 is all zero)
PRISTINE = {0: bytes.fromhex("ffffffff0000000080000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff000000000000000000000000000000000000000000000000000000000000000002"),
            1: bytes.fromhex("0030000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")}
DUNGEON_MAP_AREA = 0x1D


class CopyError(RuntimeError):
    pass


def _group(m: SaveModel, slot: int, z: int) -> tuple:
    k = group_key(9 * 10**7 + (40 + slot) * 10**5 + z * 1000)
    at = m.pool + k * 125
    return at, bytes(m.d[at:at + 125])


def _pristine(z: int) -> bytes:
    return PRISTINE.get(z, bytes(125))


def slot_record(m: SaveModel, slot: int):
    """The map word of a chalice slot's record (0x1dxxxxxx) or None when the slot holds no dungeon."""
    _, g = _group(m, slot, 0)
    return None if g[:4] == NO_RECORD else struct.unpack(">I", g[:4])[0]


def selected_slot(m: SaveModel):
    """The one-hot chalice slot selector 9020..9026 (None when none or several are set)."""
    on = [k for k in range(SLOTS) if m.flag(9020 + k)]
    return on[0] if len(on) == 1 else None


def clean_for_offline(data: bytes):
    """(bytes for the single-player folder, report). report = {"changed": [text], "kept": [text],
    "warnings": [text]} in plain words for the launcher. Raises SaveFormatError for a file that is not a
    character save. The input is never modified; a save with nothing party-only comes back equal."""
    m = SaveModel(data)
    if m.empty:
        raise SaveFormatError("this slot holds no character")
    changed, warnings = [], []
    if m.phantom_mark:
        m.repair_phantom_mark()
        changed.append("the remembered 'summoned guest' body type was cleared (the character is a normal "
                       "hunter in its own world again)")
    rec = slot_record(m, 0)
    dirty = [z for z in range(10) if _group(m, 0, z)[1] != _pristine(z)]
    if dirty:
        for z in dirty:
            at, _ = _group(m, 0, z)
            m.d[at:at + 125] = _pristine(z)
        what = (f"the host's Chalice Dungeon it was last quick-joined into (map {rec:#010x}) was removed from "
                f"the network slot" if rec is not None else "leftovers of a joined Chalice Dungeon were "
                "removed from the network slot")
        changed.append(what + "; its own dungeons (altar slots 1 to 6) are kept")
    area = struct.unpack_from("<I", m.d, 4)[0] >> 24
    sel = selected_slot(m)
    if area == DUNGEON_MAP_AREA and (sel is None or slot_record(m, sel) is None):
        warnings.append("this character was last saved inside a Chalice Dungeon that is not in its own altar "
                        "list: single-player may not load it correctly. Load it in the party first, leave to "
                        "the Hunter's Dream, then copy again")
    kept = ["flags (bosses, lamps, quests), items, NG cycle and play time are copied as they are",
            "party-only extras (owed items, bloodstains, party gives) live outside the save and are not copied"]
    if m.world_empty:
        kept.append("last saved in another hunter's world: the game rebuilds your own world's details at home")
    return m.to_bytes(), {"changed": changed, "kept": kept, "warnings": warnings}


def describe(report: dict) -> str:
    out = ["Cleaned for single-player:" if report["changed"] else "Nothing party-only was in this character."]
    out += [f"- {t}" for t in report["changed"]]
    out += [f"- kept: {t}" for t in report["kept"]]
    out += [f"WARNING: {t}" for t in report["warnings"]]
    return "\n".join(out)


def _target_for(user: Path, party_file: Path, solo_slot: int) -> Path:
    existing = store.slot_files(user, "solo")
    if existing:
        any_file = next(iter(existing.values()))
        return any_file.with_name(f"userdata{solo_slot:04d}")
    rel = party_file.relative_to(user / PARTY)
    return user / SOLO / rel.with_name(f"userdata{solo_slot:04d}")


def copy_party_to_offline(user, party_slot: int, solo_slot=None, replace=False, running=lambda: []) -> dict:
    r"""Write the cleaned party character of party_slot into slot solo_slot (default: the same number) of
    <user>\savedata. The party folder is only read. CopyError when a game runs, the party slot holds no
    character, or the single-player slot holds one and replace is false. The single-player folder is first
    copied to <user>\save_backups\savedata\<time>-before-party-copy. Returns {"report", "backup",
    "target", "party_slot", "solo_slot"}."""
    user = Path(user)
    solo_slot = party_slot if solo_slot is None else solo_slot
    if not 0 <= solo_slot <= 9:
        raise CopyError("the slot must be 0 to 9")
    if running():
        raise CopyError("close the game first")
    src = store.slot_files(user, "party").get(party_slot)
    if src is None:
        raise CopyError(f"no party character in slot {party_slot + 1}")
    try:
        data, report = clean_for_offline(src.read_bytes())
    except (OSError, SaveFormatError) as ex:
        raise CopyError(f"the party save cannot be read: {ex}") from ex
    target = _target_for(user, src, solo_slot)
    old = None
    if target.is_file():
        try:
            old = SaveModel(target.read_bytes())
        except SaveFormatError:
            old = None
        if old is not None and not old.empty and not replace:
            raise CopyError(f"single-player slot {solo_slot + 1} already holds {old.name}")
    backup = None
    solo_dir = user / SOLO
    if store_has_files(solo_dir):
        backup = store.make_backup(user, "solo", reason="before-party-copy",
                                   note=f"party slot {party_slot + 1} copied to slot {solo_slot + 1}")
    else:
        # no single-player save yet: take the party folder's other files (sce_sys etc.) as the frame
        shutil.copytree(user / PARTY, solo_dir, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("userdata000?", "*.bbtmp"))
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name(target.name + ".party-copy")
    tmp.write_bytes(data)
    if tmp.read_bytes() != data:
        tmp.unlink()
        raise CopyError(f"could not write {tmp}")
    os.replace(tmp, target)
    return {"report": report, "backup": backup, "target": target, "party_slot": party_slot, "solo_slot": solo_slot}


def store_has_files(folder: Path) -> bool:
    return folder.is_dir() and any(p.is_file() and not p.name.endswith(".bbtmp") for p in folder.rglob("*"))
