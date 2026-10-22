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

## 5. A guest's save written in the host's world (0.7.3, agent guestsave)

Player report: save editors crashed on a character save last written while the player was a guest in
another world; hosting the own world and saving again made it readable.

**The differing field.** The save writer `0x156AA50` writes after the event flags a world-state section
(header +0x3C offset, +0x40 size): three blocks `{u32 size, data}` "CHR " (WorldChrMan, magic
`0x1509170043485220`), "OBJ " and "DECL" (WorldDecalMan, the bytes `4C 43 45 44` = "LCED" that save
editors search for), then the position block (+0x44; +0x48 is its end counted from +0x3C), the FrpgNetMan
block (+0x4C / +0x50, 0x20004 bytes), a `{u16, u16, u32 size, data}` block (`0x175A4E0`, 0x1000 bytes) and 27
bytes; the rest of the 0x140000-byte file is the game's uninitialized buffer (`0x1569740` allocates it and
writes all of it). At home (GSM +0x1592 = 1) the writer serializes the live managers. In another world
(+0x1592 = 0) it writes the **snapshot of the own world** at GSM +0x1690/+0x1698 (CHR), +0x16B0/+0x16B8 (OBJ),
+0x16D0/+0x16D8 (DECL): `{u32 size}` and, when the pointer is set, the data. Vanilla fills them at the summon
(the SummonedMapReload serializer `0x131E5C0`, the same encoding); the party's B2 joins (direct summon, title
join, seamless travel) never run it, so they are 0 (`Party B2: ... blobs ... (own: 0 / 0 / 0)` in every guest
log) and every save written in the host's world got three empty blocks: section size 12 instead of ~0x72 or
more, no "LCED". Confirmed on the agent saves: `agents/lamprule/mptest/inst2` (the guest, saved by "shared
lamps from the host's world" in load mode 1) has a 12-byte section, its session-start snapshot 0x72 bytes;
the other sections differ only by what the session changed. Of 195 harness character saves it is the only
one: most harness guests are killed before a save in the host's world.

**What it breaks.** Editors that locate data by these blocks. Noxde/Bloodborne-save-editor (Rust/Tauri,
GPL-3, commit d8d0365; read, not built: no Rust toolchain here and no installers) finds everything by
searching (`offsets.rs`, `position.rs`): "FACE" from 0xF000, the name 469 + 34028 bytes before it, "LCED"
after "FACE", and the position after the first `FF FF FF FF 00*8` from "LCED" on
(`Coordinates::new(..).unwrap()`, a panic when none). Without "LCED" it searches from byte 0 and takes
the first empty inventory entry `{0, -1, 0, 0}` for the position: on the lamprule guest save at 0xA28C,
(0, NaN, 0); its position edit writes there, into the inventory. `tools/party/editor_check.py` is a port
of that parse path (exit 1 on such a save): all other harness saves read correctly, the guest save does
not. No panic on that file by this reading (the empty inventory entry is always there), so the crash the
player saw is not reproduced: which editor or version it was is not known.

**The runtime fix** (user decision 2026-10-22, re-added after a first take-out; `runtime_savepolicy_world_fix`,
called by `runtime_file.c`'s commit of a party session's `userdata0000..0009`): when the game's new save has
three empty blocks and the file it replaces (the game renames it to `backupNNNN` first: BB_SAVE_TRACE shows
`rename(userdata0000, backup0000)` then the write) has them, for the same character (name, play time not
newer), the old blocks go into the new save, the rest of its data (to the data's end: the next block + 8 +
its size + 27) moves behind them and the header's +0x40/+0x44/+0x48/+0x4C follow. Nothing in the game's
memory changes. Log: `Saves: userdata0000 was written in another player's world without the own world's
state ...: the N bytes of backup0000 kept in it`. Off: `BB_SAVE_GUEST_WORLD_FIX=0`. Run lp3 (lamps group
22/22): 5 guest saves in the host's world kept their 114 bytes; editor_check and save_check pass on them.

Limits: the in-memory snapshot GSM +0x1690.. stays empty, so a go-home inside the session (B2 leave, the
own world loaded from that snapshot) is not helped; a quit and Continue is (the blocks come from the file).
The live check of a guest's own-world kill surviving a go-home (run side2) did not get there: the guest's
home load went to the Dream, not its fixture's Cathedral Ward, and the kill step never ran. Saves the guest
writes during a title join's load are written in own-world mode (+0x1592 = 1) and carry the live world,
i.e. the host's map state (seen as LCED map lists of the host's map in guest saves); not changed here.

**Export for save editor / Import from save editor** (launcher, Party tab; `scripts/save_editor.py`):
* export: any character save (`savedata` or `savedata_party`, slot 0..9) is copied to a folder the player
  picks. A save with three empty blocks gets minimal blocks in the copy (each `{u32 20}` + magic and
  version + the empty list's end `FF FF FF FF 00*8`, what the writer emits for a world with nothing
  saved), the data behind them moves by 48 bytes and the header's +0x40/+0x44/+0x48/+0x4C follow (the
  end of the data: the next block + 8 + its size + 27; the rest of the file is the game's uninitialized
  buffer, `0x1569740` writes all 0x140000 bytes). `bbport_export.json` beside it: kind, slot, source and
  export SHA-256, the character, what was changed (original bytes and the 48 tail bytes); for a party
  save the party side files (`party_files\`).
* import: the edited file must have the manifest beside it and parse (size, version 0x41, the header
  lines up); asks first when the live slot changed since the export or the name changed; backs the folder
  up to `user\save_backups\<folder>\<time>-before-editor-import`; takes the minimal blocks out again when
  the editor left them as exported (an unedited export comes back byte for byte); writes through a temp
  file; for a party save updates the slot's `party_characters.txt` line (name hash, play time) so the
  game does not drop the slot's party state for an edited name.

Checks: `tests/test_save_editor.py` (round trip on synthetic saves and on the test fixture: the guest-like
save fails `editor_check`, its export passes, the import gives it back with only the edit), `save_check.py`'s
`saves` rule (every guest character save: the export reads in the editor and comes back unchanged, else
EDITOR), `save_decode.py --check-world`.
