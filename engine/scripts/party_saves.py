"""Party save folder (BB_PARTY_SAVE=separate) helpers for the launcher.

A party session with separate saves plays from <user>\\savedata_party (src/runtime_savepolicy.c);
the single-player saves in <user>\\savedata are never written by it. The first time, the launcher
offers to copy the single-player save into the party folder: that copy only reads savedata.
Backups of either folder are in <user>\\save_backups (made by the game, README.txt there).
"""
import shutil
import time
from pathlib import Path

SOLO = "savedata"
PARTY = "savedata_party"
BACKUPS = "save_backups"


def has_saves(folder: Path) -> bool:
    """Any save file (not a leftover .bbtmp) under folder."""
    folder = Path(folder)
    if not folder.is_dir():
        return False
    return any(p.is_file() and not p.name.endswith(".bbtmp") for p in folder.rglob("*"))


def should_offer_copy(user: Path) -> bool:
    """First party session with separate saves: no party save yet, a single-player save to copy."""
    user = Path(user)
    return has_saves(user / SOLO) and not has_saves(user / PARTY)


def _files(folder: Path) -> dict:
    return {p.relative_to(folder).as_posix(): p.stat().st_size
            for p in folder.rglob("*") if p.is_file() and not p.name.endswith(".bbtmp")}


def copy_solo_to_party(user: Path, replace: bool = False):
    """Copy <user>\\savedata to <user>\\savedata_party (savedata is only read).

    An existing party save is kept: FileExistsError unless replace=True, which first copies it to
    <user>\\save_backups\\savedata_party\\<time>-replaced (a backup the game's rotation also keeps).
    Returns that backup's path, or None. The copy is made next to the target and renamed into
    place, so an interrupted copy never looks like a party save.
    """
    user = Path(user)
    solo, party = user / SOLO, user / PARTY
    if not has_saves(solo):
        raise FileNotFoundError(f"no single-player save in {solo}")
    backup = None
    if has_saves(party) and not replace:
        raise FileExistsError(f"{party} already has a party save")
    staging = user / (PARTY + ".copying")
    if staging.exists():
        shutil.rmtree(staging)
    shutil.copytree(solo, staging, ignore=shutil.ignore_patterns("*.bbtmp"))
    if _files(staging) != _files(solo):
        shutil.rmtree(staging, ignore_errors=True)
        raise OSError(f"the copy of {solo} is incomplete; nothing changed")
    if party.exists():
        if has_saves(party):
            backup = user / BACKUPS / PARTY / (time.strftime("%Y%m%d-%H%M%S") + "-replaced")
            n = 2
            while backup.exists():
                backup = backup.with_name(f"{time.strftime('%Y%m%d-%H%M%S')}-replaced-{n:02d}")
                n += 1
            shutil.copytree(party, backup)
            if _files(backup) != _files(party):
                shutil.rmtree(staging, ignore_errors=True)
                raise OSError(f"could not back up {party}; nothing changed")
        shutil.rmtree(party)
    staging.rename(party)
    return backup
