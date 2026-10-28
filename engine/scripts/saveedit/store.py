"""Find, back up, write and restore character saves for the launcher's save editor.

Rules (docs/superpowers/specs/2026-10-28-save-editor-design.md 4):
  * nothing is written while a game of this install runs (the caller passes `running`, a function that
    returns the running instances; a non-empty result refuses);
  * every write is preceded by a copy of the whole save folder to
    <user>\\save_backups\\<savedata|savedata_party>\\<time>-before-edit (the game's own backup layout, so
    user\\save_backups\\README.txt's restore steps apply); the newest KEEP of those stay;
  * the slot file is written through a temp file and os.replace, read back, parsed again, and compared
    byte for byte with what was meant; on any mismatch the backup is put back.
"""
import os
import shutil
import sys
import time
from pathlib import Path

try:
    from .model import SaveModel, SaveFormatError, SLOT_SIZE
except ImportError:  # run with scripts\saveedit on the path
    from model import SaveModel, SaveFormatError, SLOT_SIZE

KINDS = {"solo": "savedata", "party": "savedata_party"}
BACKUPS = "save_backups"
KEEP = 20
INFO = "EDIT.txt"
SIDE_INFO = ("BACKUP_INFO.txt", INFO)


class GameRunning(RuntimeError):
    pass


class EditConflict(RuntimeError):
    pass


def slot_files(user, kind):
    """{slot: path} of the character saves of one kind."""
    root = Path(user) / KINDS[kind]
    out = {}
    for p in sorted(root.glob("*/*/SPRJ0005/userdata000[0-9]")):
        out.setdefault(int(p.name[-1]), p)
    return out


def list_slots(user, kind):
    """[{slot, path, name, level, ng, play_ms, phantom, guest_written}] of the slots that hold a character."""
    rows = []
    for slot, p in slot_files(user, kind).items():
        try:
            m = SaveModel(p.read_bytes())
        except (OSError, SaveFormatError):
            continue
        if m.empty:
            continue
        rows.append(dict(slot=slot, path=p, name=m.name, level=m.level, play_ms=m.play_ms,
                         phantom=m.phantom_mark, guest_written=m.world_empty))
    return rows


def open_slot(user, kind, slot):
    p = slot_files(user, kind).get(slot)
    if p is None:
        raise FileNotFoundError(f"no slot {slot} in {KINDS[kind]}")
    return SaveModel(p.read_bytes())


def _stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def _unique(base, stamp, reason):
    final, n = base / f"{stamp}-{reason}", 2
    while final.exists():
        final, n = base / f"{stamp}-{reason}-{n:02d}", n + 1
    return final


def _tree(folder):
    return {p.relative_to(folder): p.stat().st_size for p in Path(folder).rglob("*")
            if p.is_file() and not p.name.endswith(".bbtmp")}


def make_backup(user, kind, reason="before-edit", note=""):
    folder = Path(user) / KINDS[kind]
    base = Path(user) / BACKUPS / KINDS[kind]
    base.mkdir(parents=True, exist_ok=True)
    final = _unique(base, _stamp(), reason)
    partial = final.with_name(final.name + ".partial")
    shutil.copytree(folder, partial, ignore=shutil.ignore_patterns("*.bbtmp"))
    if _tree(partial) != _tree(folder):
        shutil.rmtree(partial, ignore_errors=True)
        raise OSError(f"could not back up {folder}; nothing changed")
    (partial / INFO).write_text(f"kind={kind}\nreason={reason}\n{note}\n", encoding="utf-8")
    partial.rename(final)
    _prune(base)
    return final


def _prune(base):
    mine = sorted((p for p in base.iterdir() if p.is_dir() and "-before-edit" in p.name), reverse=True)
    for p in mine[KEEP:]:
        shutil.rmtree(p, ignore_errors=True)


def list_backups(user, kind):
    """Newest first: [{path, name, reason}] of the editor's backups (before-edit / before-restore)."""
    base = Path(user) / BACKUPS / KINDS[kind]
    if not base.is_dir():
        return []
    out = []
    for p in sorted(base.iterdir(), reverse=True):
        if p.is_dir() and not p.name.endswith(".partial") and ("-before-edit" in p.name or "-before-restore" in p.name):
            out.append(dict(path=p, name=p.name, reason="before-restore" if "-before-restore" in p.name else "before-edit"))
    return out


def _refuse_if_running(running):
    found = running() if running else []
    if found:
        raise GameRunning("close the game first: Bloodborne is running")


def apply_edit(user, kind, slot, model, running, update_party=None):
    """Write the edited model into its slot after a backup; returns the backup folder.
    update_party(user, slot, data): the party_characters.txt sync (scripts/save_editor), called for party saves."""
    _refuse_if_running(running)
    path = slot_files(user, kind).get(slot)
    if path is None:
        raise FileNotFoundError(f"no slot {slot}")
    if path.read_bytes() != model.orig:
        raise EditConflict("the save changed on disk since it was opened; open it again")
    new = model.to_bytes()
    SaveModel(new)  # the edited file must still parse
    backup = make_backup(user, kind, "before-edit", f"slot={slot}\nname={model.name}")
    tmp = path.with_name(path.name + ".editor-tmp")
    try:
        tmp.write_bytes(new)
        os.replace(tmp, path)
        back = path.read_bytes()
        SaveModel(back)
        if back != new:
            raise OSError("the written file differs from the edit")
    except Exception:
        try:
            tmp.unlink()
        except OSError:
            pass
        _put_back(backup, path)
        raise
    if kind == "party" and update_party is not None:
        update_party(user, slot, new)
    return backup


def _put_back(backup, path):
    rel = None
    for p in Path(backup).rglob(path.name):
        rel = p
        break
    if rel is not None:
        shutil.copyfile(rel, path)


def restore_backup(user, kind, backup, running):
    """Put an editor backup's save folder back; the current folder is first kept as -before-restore."""
    _refuse_if_running(running)
    backup = Path(backup)
    folder = Path(user) / KINDS[kind]
    keep = make_backup(user, kind, "before-restore") if folder.is_dir() else None
    staging = folder.with_name(folder.name + ".restore")
    old = folder.with_name(folder.name + ".replaced")
    for p in (staging, old):
        shutil.rmtree(p, ignore_errors=True)
    staging.mkdir(parents=True)
    for child in backup.iterdir():
        if child.name in SIDE_INFO:
            continue
        if child.is_dir():
            shutil.copytree(child, staging / child.name)
        else:
            shutil.copyfile(child, staging / child.name)
    if folder.is_dir():
        os.replace(folder, old)
    os.replace(staging, folder)
    shutil.rmtree(old, ignore_errors=True)
    return keep
