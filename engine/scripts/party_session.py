"""Party session snapshots: "Undo last party session" and the check after a session (launcher).

Right before every party session (host and guests; not a crash restart, which continues the same
session) the game copies <user>\\savedata and <user>\\savedata_party to
<user>\\save_backups\\party_sessions\\<YYYYMMDD-HHMMSS>-<host|guest>\\ with SESSION.txt
(src/runtime_savepolicy.c, runtime_savepolicy_session_snapshot; the newest 5 are kept).

  undo(user)       puts the newest snapshot back, byte for byte, after copying the current state to
                   a "<time>-before-undo" snapshot the same way (so the undo can itself be undone)
  describe(user)   what an undo would replace (for the confirmation dialog)
  check(user)      after the session: every party character save reads (version, the event flag
                   section, the save header), its NG cycle is unchanged unless an ending was played,
                   no boss kill / lamp it had before the session is gone, its level and play time did
                   not go down and no character vanished

Save files are untrusted input: only parsed, never executed. Layout (save version 0x41; header
from the game's save writer 0x156AA50 / 0x14EA560, docs/party/save_safety.md): u32 version, u32 map
id, u32 play time (ms), then {u32 offset, u32 size} pairs: +0x2C/+0x30 the NG cycle block (u32 NG
cycle = GameDataMan+0x68, u32 ending type = +0x6C), +0x34/+0x38 the event flag section (1200 groups
x 125 bytes + 1; tools/party/save_decode.py has the flag id -> group mapping).
"""
import hashlib
import re
import shutil
import struct
import time
from datetime import datetime
from pathlib import Path

FOLDERS = ("savedata", "savedata_party")
SESSIONS = Path("save_backups") / "party_sessions"
INFO = "SESSION.txt"
KEEP = 8  # snapshots kept by the launcher's own copies (the game keeps BB_SAVE_SESSION_KEEP = 5 of its own)
CHECKED = "CHECKED.txt"
SLOT_SIZE = 0x140000
SYSTEM_SIZE = 0x40000
PARAM_SIZE = 1328
POOL_BYTES = 1200 * 125
SAVE_VERSION = 0x41

# Flags that stay set for the rest of an NG cycle: boss kills (map event 1AAB1800 / 1AAB1850) and lit lamps of
# the game and its DLC (7AAB02xx; the DLC lamps can be cleared by the DLC-removal events). Chalice dungeons
# (area 29) are left out: their flags follow the dungeon instance. A flag here set before a session and clear
# after it (same NG cycle) is lost progress. The lists are not stored: check() gets them from the player's own
# game files (scripts/saveedit/flagsdata.derive via names.Catalogue, the `progress` dict {"bosses": [[flag, ...]],
# "lamps": [flag]}); without them the boss / lamp comparison is skipped.
# Set by the endings (docs/party/cutscenes_endings.md 1.4): 21/22/23 ending A/B/C, 6600-6603 the
# endings' unlocks, 6604 "ending reached" (cleared on the next cycle's first load).
ENDING_FLAGS = (21, 22, 23, 6600, 6601, 6602, 6603, 6604)

_MAPS = [(21, 0), (21, 1), (22, 0), (23, 0), (24, 0), (24, 1), (24, 2), (25, 0), (26, 0), (27, 0), (28, 0),
         (29, 0), (32, 0), (33, 0), (34, 0), (35, 0), (36, 0)]
_GLOBAL_TYPES = {0: 0, 1: 1, 5: 2, 6: 3, 7: 4}
_MAP_TYPES = {1: 0, 5: 1, 6: 2, 7: 3, 9: 4}


def _group_key(fid):
    """The flag store group of a flag id (tools/party/save_decode.py group_key)."""
    t, aa, b, z = fid // 10**7, (fid // 10**5) % 100, (fid // 10**4) % 10, (fid // 1000) % 10
    if fid < 10000:
        return z * 5
    if aa == 0 and b == 0 and t in _GLOBAL_TYPES:
        return _GLOBAL_TYPES[t] + z * 5
    if t not in _MAP_TYPES:
        return None
    if (aa, b) in _MAPS:
        mi = _MAPS.index((aa, b))
    elif aa == 40:
        mi = 11
    elif 41 <= aa <= 46 and b == 0:
        mi = 17 + aa - 41
    else:
        return None
    return z * 115 + 50 + 23 * _MAP_TYPES[t] + mi


class SaveError(ValueError):
    pass


class CharacterSave:
    """One character slot (userdata0000..0009)."""

    def __init__(self, data: bytes):
        d = self.d = bytes(data)
        if len(d) != SLOT_SIZE:
            raise SaveError(f"{len(d)} bytes, expected {SLOT_SIZE} (truncated or damaged)")
        self.version, self.map_id, self.play_ms = struct.unpack_from("<III", d, 0)
        if self.version != SAVE_VERSION:
            raise SaveError(f"save version {self.version:#x}, expected {SAVE_VERSION:#x}")
        ng_off, ng_size, self.pool, pool_size = struct.unpack_from("<IIII", d, 0x2C)
        if ng_size != 8 or ng_off + 8 > len(d):
            raise SaveError("the NG cycle block is missing from the save header")
        if pool_size != POOL_BYTES + 1 or self.pool + pool_size > len(d):
            raise SaveError("the event flag section is missing or damaged")
        self.ng_cycle, self.ending_type = struct.unpack_from("<II", d, ng_off)
        if self.ng_cycle > 7 or self.ending_type > 3:
            raise SaveError(f"NG cycle {self.ng_cycle} / ending type {self.ending_type} out of range")

    def flag(self, fid):
        k = _group_key(fid)
        if k is None:
            return None
        return bool(self.d[self.pool + k * 125 + (fid % 1000) // 8] & (0x80 >> (fid % 8)))

    def character(self):
        """{"name", "level"} (the name is UTF-16 after the stats; save_decode.py)."""
        m = re.search(rb"(?:[A-Za-z\xc0-\xff]\x00){4,16}\x00\x00", self.d[:0x40000])
        if not m or m.start() < 0x18:
            return {}
        level = struct.unpack_from("<I", self.d, m.start() - 0x18)[0]
        return {"name": m.group()[:-2].decode("utf-16le", "replace"), "level": level if level < 1000 else None}


class Session:
    def __init__(self, path: Path, info: dict):
        self.path = Path(path)
        self.info = info
        self.role = info.get("role", "?")
        self.kind = info.get("kind", "session-start")
        try:
            self.time = datetime.strptime(info.get("time", self.path.name[:15]), "%Y%m%d-%H%M%S")
        except ValueError:
            self.time = None

    @property
    def label(self):
        when = self.time.strftime("%Y-%m-%d %H:%M:%S") if self.time else self.path.name
        if self.kind == "before-undo":
            return f"{when}, your saves before an undo"
        return f"{when}, start of a party session as {'host' if self.role == 'host' else 'guest'}"


def _read_info(path: Path) -> dict:
    out = {}
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def sessions(user) -> list:
    """Every finished snapshot, newest first."""
    base = Path(user) / SESSIONS
    if not base.is_dir():
        return []
    out = [Session(p, _read_info(p / INFO)) for p in base.iterdir()
           if p.is_dir() and not p.name.endswith(".partial") and (p / INFO).is_file()]

    def key(s):
        m = re.search(r"-(\d\d)$", s.path.name)
        return (s.path.name[:15], int(m.group(1)) if m else 1)
    return sorted(out, key=key, reverse=True)


def last_session(user):
    s = sessions(user)
    return s[0] if s else None


def _files(folder: Path) -> dict:
    """{relative path: sha256} of every save file (no leftover .bbtmp)."""
    folder = Path(folder)
    if not folder.is_dir():
        return {}
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in folder.rglob("*") if p.is_file() and not p.name.endswith(".bbtmp")}


def _slots(folder: Path) -> dict:
    """{relative path: bytes} of the character slots that hold a character."""
    out = {}
    folder = Path(folder)
    if folder.is_dir():
        for p in sorted(folder.rglob("userdata000[0-9]")):
            data = p.read_bytes()
            if data.strip(b"\0"):
                out[p.relative_to(folder).as_posix()] = data
    return out


def _stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def snapshot_now(user, kind="before-undo", role="") -> Path:
    """Copy both save folders to a new snapshot (as the game does before a session)."""
    user = Path(user)
    base = user / SESSIONS
    base.mkdir(parents=True, exist_ok=True)
    when = _stamp()
    final = base / f"{when}-{kind}"
    n = 2
    while final.exists() or Path(str(final) + ".partial").exists():
        final = base / f"{when}-{kind}-{n:02d}"
        n += 1
    partial = Path(str(final) + ".partial")
    lines = [f"kind={kind}", f"role={role}", f"time={when}"]
    partial.mkdir(parents=True)
    for folder in FOLDERS:
        src = user / folder
        if src.is_dir():
            shutil.copytree(src, partial / folder, ignore=shutil.ignore_patterns("*.bbtmp"))
        if _files(partial / folder) != _files(src):
            shutil.rmtree(partial, ignore_errors=True)
            raise OSError(f"could not copy {src}; nothing changed")
        lines.append(f"{folder}_files={len(_files(src))}")
    (partial / INFO).write_text("\r\n".join(lines) + "\r\n", encoding="utf-8")
    partial.rename(final)
    _prune(base)
    return final


def _prune(base, keep=KEEP):
    """The newest `keep` snapshots stay (the game rotates its own the same way, BB_SAVE_SESSION_KEEP)."""
    for s in sessions(base.parent.parent)[keep:]:
        shutil.rmtree(s.path, ignore_errors=True)


def _describe_slot(data):
    try:
        s = CharacterSave(data)
    except SaveError as ex:
        return f"damaged ({ex})"
    c = s.character()
    who = c.get("name") or "a character"
    lvl = f" level {c['level']}" if c.get("level") else ""
    ng = f", NG+{s.ng_cycle}" if s.ng_cycle else ""
    minutes = s.play_ms // 60000
    return f"{who}{lvl}{ng}, {minutes // 60} h {minutes % 60:02d} min played"


def describe(user, session=None) -> list:
    """Lines for the confirmation dialog: what the undo replaces with what."""
    user = Path(user)
    session = session or last_session(user)
    if session is None:
        return []
    lines = []
    for folder in FOLDERS:
        now, then = _files(user / folder), _files(session.path / folder)
        changed = sorted(k for k in set(now) | set(then) if now.get(k) != then.get(k))
        if not changed:
            lines.append(f"{folder}: unchanged since then")
            continue
        lines.append(f"{folder}: {len(changed)} file{'s' if len(changed) != 1 else ''} differ")
        if folder == "savedata_party" or folder == "savedata":
            s_now, s_then = _slots(user / folder), _slots(session.path / folder)
            for rel in sorted(set(s_now) | set(s_then)):
                if s_now.get(rel) == s_then.get(rel):
                    continue
                slot = int(rel[-1]) + 1
                a = _describe_slot(s_now[rel]) if rel in s_now else "empty"
                b = _describe_slot(s_then[rel]) if rel in s_then else "empty"
                lines.append(f"  slot {slot}: now {a} -> back to {b}")
    return lines


def undo(user, session=None):
    """Put the snapshot back (both folders, byte for byte). Returns (session, before-undo snapshot)."""
    user = Path(user)
    session = session or last_session(user)
    if session is None:
        raise FileNotFoundError("no party session snapshot to restore")
    backup = snapshot_now(user, "before-undo")
    for folder in FOLDERS:
        src, dst = session.path / folder, user / folder
        staging, old = user / (folder + ".undo"), user / (folder + ".undo-old")
        for p in (staging, old):
            if p.exists():
                shutil.rmtree(p)
        if src.is_dir():
            shutil.copytree(src, staging)
        else:
            staging.mkdir()
        if _files(staging) != _files(src):
            shutil.rmtree(staging, ignore_errors=True)
            raise OSError(f"the copy of {src} is incomplete; {dst} is unchanged (backup: {backup})")
        if dst.exists():
            dst.rename(old)
        staging.rename(dst)
        shutil.rmtree(old, ignore_errors=True)
        if _files(dst) != _files(src):
            raise OSError(f"{dst} differs from {src} after the restore (your saves before it: {backup})")
    mark_checked(user, session, [])
    return session, backup


def check(user, session=None, progress=None) -> list:
    """Problems with the party saves after the session: [(level, text)], level "error" / "warning".
    `progress`: the boss / lamp flag lists of the player's game (see the note at BOSS / lamp flags above)."""
    boss_flags = (progress or {}).get("bosses", ())
    lamp_flags = (progress or {}).get("lamps", ())
    user = Path(user)
    session = session or last_session(user)
    problems = []
    folder = user / "savedata_party"
    if not folder.is_dir():
        return problems
    for p in sorted(folder.rglob("*")):
        if not p.is_file() or p.name.endswith(".bbtmp"):
            continue
        size = p.stat().st_size
        if p.name == "userdata0010" and (size != SYSTEM_SIZE or
                                         (not p.read_bytes().strip(b"\0") and _had_data(session, folder, p))):
            # All zeros is only damage when the session started with real data there: a game quit
            # at the title before its first save leaves the file as it was, zeros (a false alarm).
            problems.append(("error", f"{p.name}: the game data file is damaged ({size} bytes)"))
        elif p.name == "param.bin" and size != PARAM_SIZE:
            problems.append(("error", f"the save description (param.bin) is damaged ({size} bytes)"))
    now = _slots(folder)
    then = _slots(session.path / "savedata_party") if session else {}
    for rel in sorted(set(now) | set(then)):
        slot = f"slot {int(rel[-1]) + 1}"
        if rel not in now:
            problems.append(("error", f"{slot}: the character that was there before the session is gone"))
            continue
        try:
            cur = CharacterSave(now[rel])
        except SaveError as ex:
            problems.append(("error", f"{slot}: the save does not load ({ex})"))
            continue
        if rel not in then:
            continue
        try:
            old = CharacterSave(then[rel])
        except SaveError:
            continue
        c_now, c_then = cur.character(), old.character()
        if c_now.get("name") != c_then.get("name"):
            continue  # another character made in this slot (New Game): nothing to compare
        who = f"{slot} ({c_now.get('name') or 'character'})"
        ending = cur.ending_type != old.ending_type or any(cur.flag(f) != old.flag(f) for f in ENDING_FLAGS)
        if cur.ng_cycle < old.ng_cycle:
            problems.append(("error", f"{who}: NG cycle went back from NG+{old.ng_cycle} to NG+{cur.ng_cycle}"))
        elif cur.ng_cycle > old.ng_cycle and not (cur.ng_cycle == old.ng_cycle + 1 and ending):
            problems.append(("warning", f"{who}: NG cycle went from NG+{old.ng_cycle} to NG+{cur.ng_cycle} "
                                        "without an ending in this session"))
        if cur.ng_cycle == old.ng_cycle:
            lost_boss = [f for f, *_ in boss_flags if old.flag(f) and not cur.flag(f)]
            lost_lamp = [f for f in lamp_flags if old.flag(f) and not cur.flag(f)]
            if lost_boss:
                problems.append(("error", f"{who}: {len(lost_boss)} boss kill{'s' if len(lost_boss) > 1 else ''} "
                                          f"from before the session are undone (flags {', '.join(map(str, lost_boss[:4]))})"))
            if lost_lamp:
                problems.append(("error", f"{who}: {len(lost_lamp)} lamp{'s' if len(lost_lamp) > 1 else ''} "
                                          f"lit before the session are unlit again (flags {', '.join(map(str, lost_lamp[:4]))})"))
        if c_now.get("level") and c_then.get("level") and c_now["level"] < c_then["level"]:
            problems.append(("error", f"{who}: level went down from {c_then['level']} to {c_now['level']}"))
        if cur.play_ms + 60000 < old.play_ms:
            problems.append(("error", f"{who}: play time went back from {old.play_ms / 3.6e6:.1f} h to "
                                      f"{cur.play_ms / 3.6e6:.1f} h (an older save came back)"))
    return problems


def needs_check(user):
    """The newest snapshot when it is a session start not checked yet, else None."""
    s = last_session(user)
    if s is None or s.kind != "session-start" or (s.path / CHECKED).is_file():
        return None
    return s


def check_completed_session(user):
    """Validate the saves and record the result on the session-start snapshot."""
    session = needs_check(user)
    problems = check(user, session)
    if session is not None:
        mark_checked(user, session, problems)
    return problems


def mark_checked(user, session, problems):
    try:
        text = [f"time={_stamp()}", f"result={'problems' if problems else 'ok'}"]
        text += [f"{level}={msg}" for level, msg in problems]
        (session.path / CHECKED).write_text("\r\n".join(text) + "\r\n", encoding="utf-8")
    except OSError:
        pass
