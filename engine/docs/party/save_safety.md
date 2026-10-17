# Save safety after party sessions (0.6.2)

What protects a player's saves around a party session, on top of the rotating backups
(`BB_SAVE_BACKUP_MINUTES` / `BB_SAVE_BACKUP_KEEP`, `src/runtime_savepolicy.c`, docs/SAVES.md).

## 1. The snapshot before every party session

`runtime_savepolicy_start` (game start, `src/runtime_savepolicy.c`): in a party process (BB_PARTY
set: the launcher's Host / Join or the title's Party menu, which relaunches the game) that is **not**
a crash restart (`BB_PARTY_RESTARTED=1` continues the same session), before the game mounts any
save, both folders are copied:

```
user\save_backups\party_sessions\<YYYYMMDD-HHMMSS>-host\      (or -guest)
    savedata\...          the single-player saves as they were
    savedata_party\...    the party saves as they were
    SESSION.txt           kind=session-start, role, time, files / character slots / hash per folder
```

The copy goes to `<name>.partial` and is renamed when complete (an interrupted copy never counts).
The newest 5 are kept (`BB_SAVE_SESSION_KEEP`, 0 = none; `BB_SAVE_BACKUP=0` does not turn it off).
Log: `Saves: party session snapshot ...\20261015-103735-host (role host; the launcher's "Undo last
party session" restores it)`. Two snapshots in the same second are numbered (`-02`) and sort in
order. Unit test: `save-policy-test` "party session snapshots" (both folders byte-identical by
hash, SESSION.txt fields, rotation, none at a crash restart, one at a party start).

## 2. Undo last party session (launcher, Party tab)

`scripts/party_session.py` (shipped in the release's `scripts\`):

1. **What will be replaced** (`describe`): per folder the number of files that differ from the
   newest snapshot, and per character slot the character now and then, e.g.
   `slot 1: now visy level 37, 6 h 31 min played -> back to visy level 37, 6 h 30 min played`
   (name, level, NG cycle, play time from the save; `empty` for a slot without a character).
   The dialog names the snapshot ("2026-10-15 10:37:35, start of a party session as host").
2. **Yes**: the current `savedata` and `savedata_party` are first copied to a new
   `<time>-before-undo` snapshot (the newest one, so pressing *Undo last party session* again
   brings them back: the undo is itself undoable), then each folder is copied from the snapshot
   to `<folder>.undo`, checked file by file (SHA-256), swapped in, and checked again against the
   snapshot. A folder that did not exist before the session comes back empty. Any failure stops
   with the current saves untouched or in the before-undo snapshot (the message names it).
3. The launcher refuses while a game of this install runs.

The launcher keeps at most 8 snapshots of its own making (the game's rotation keeps 5 of its own).

## 3. The check after a session

When the game the launcher started ends, and at every launcher start, the newest snapshot of kind
`session-start` that has no `CHECKED.txt` yet is checked against the party saves
(`party_session.check`), skipped while a game of this install runs:

| Check | Level |
|---|---|
| `userdata0010` (game data) has its size and is not all zeros; `param.bin` has its size | error |
| every character slot (userdata0000-0009 with data) loads: 0x140000 bytes, save version 0x41, the NG-cycle block and the event-flag section where the save header says | error |
| a slot that had a character before the session still has one | error |
| NG cycle went down, or up by more than one | error |
| NG cycle up by one without an ending seen (ending type or the ending flags 21-23 / 6600-6604 changed) | warning |
| same NG cycle: a boss kill (`1AAB1800` / `1AAB1850` of the main game and DLC, 19 flags) or a lit lamp (`7AAB02xx`, areas 21-28, 28 flags) set before the session and clear now | error |
| level went down; play time went back by more than a minute (an older save came back) | error |

A slot whose character name changed (a new character made in it) is not compared. The flag lists
are `party_flags.inc` rows (boss_defeated / lamp_unlocked; chalice dungeons and the DLC's lamps,
which the DLC-removal events may clear, are left out); `tests/test_party_session.py` checks they
still categorize that way and that the flag-group mapping equals `tools/party/save_decode.py`'s.
The result is written to the snapshot's `CHECKED.txt` (no second nag) and to the launcher log
(`[PARTY] Save check after the party session ...: OK`). On a problem the launcher lists it and
asks *Undo the last party session?*, which opens the undo dialog above.

The save header layout (found for this check, 2026-10-15): u32 version, u32 map id, u32 play time
(ms), then {u32 offset, u32 size} pairs; +0x0C GameDataMan block, +0x14 PlayerGameData, +0x2C/+0x30
the NG-cycle block (8 bytes: u32 NG cycle = GameDataMan+0x68, u32 ending type = +0x6C), +0x34/+0x38
the event flag section (150001 bytes). Evidence: the save writer `0x156AA50` writes the 0x54-byte
header, `0x14EA560` writes the GameDataMan section and stores the NG block's offset at header+0x2C
(`*(int*)(hdr+0x2c) = pos; write(GameDataMan+0x68); write(GameDataMan+0x6C); *(int*)(hdr+0x30) =
8`); the loader `0x14EAA80` reads them back into +0x68 / +0x6C. All 43 non-empty character saves on
this PC have a consistent table and NG cycle 0 (no NG+ save was at hand: a non-zero value is not
yet seen in a file; the in-game log `NG cycle N at world-up` is the cross-check when one is).

A player who starts party games without the launcher (the title's Party menu after a plain
`run.bat`) still gets the snapshot; the check runs at the next launcher start.

## 4. Live evidence (2026-10-15, agent robust1)

Run 1 (two loopback instances, `tools/mp/instances.py run --count 2 --party --crash-plan
"0:cooperators 0 -> 1@15"`, test save "visy" level 37 in the Hunter's Dream): the host logged
`party session snapshot ...20261015-103735-host`, the guest `...20261015-103737-guest`; the host's
crash restart made only its `restart` backup, **no second snapshot**. After the session (both
games ended), per instance:

* the snapshot's `savedata_party` equals the harness's source save (SHA-256 of every file);
* the check found no problem (`check() -> []`);
* `describe`: `savedata: unchanged since then`, `savedata_party: 4 files differ`, `slot 1: now visy
  level 37 ... -> back to visy level 37 ...`;
* `undo`: both folders **byte-identical** to the snapshot afterwards (15 files each); the
  before-undo snapshot equals the post-session state; a second undo brought the post-session
  state back byte for byte.
