# Feature `animated_attachments` (cosmetic)

Index: [README.md](README.md). Co-op class: **cosmetic** (not in the party hash; each player's game draws the parts on
the characters it has).

Minimal mod:

```
Mods/Serpentry/
  dvdroot_ps4/parts/hd_a_9761.partsbnd.dcx      a part with its own skeleton and animations (layout below)
  dvdroot_ps4/parts/hd_a_9762.partsbnd.dcx
  nightwatch.json                               {"id": "serpentry", "requires": "0.3.3", "features": ["animated_attachments"]}
  nightwatch/animated_attachments.json          the table below
```

A character can wear one model per equipment slot, and a part file draws bone indices up to 255 only. A model with
more bones (a head of many snakes) needs more parts on the same character. `animated_attachments` loads up to two
extra part files on a player character while it has a SpEffect on, in two model slots the game never fills, and
unloads them when the SpEffect ends.

## Table: `nightwatch/animated_attachments.json`

```json
{
  "attachments": [
    {"speffect": 9111, "part": "hd_a_9761"},
    {"speffect": 9111, "part": "hd_a_9762"}
  ]
}
```

| Field | Meaning |
|---|---|
| `speffect` | a SpEffectParam id, whole number 1..2147483647: the part is worn while the character has it on |
| `part` | the part file's name without folder and extension: `parts/<part>.partsbnd.dcx` in the mod (letters, digits, `_`; at most 32) |
| `version` | optional; when given it must be 1 |

The manifest must list `"animated_attachments"` in `features` ([manifest](manifest.md)). Tables merge in load order; a
row two mods both list stays once. At most 64 rows per file and after the merge. A table that lists the same
(`speffect`, `part`) twice is refused. A bad table is dropped whole with one line: `Mods: <folder>:
nightwatch/animated_attachments.json: row N needs "speffect" (1..2147483647) and "part" (...): its animated_attachments
table is not used`.

## The part file

Built like an equipped part that animates (a head part with its own animations):

- the FLVER (entry 200) with u16 bone indices, at most 255 bones drawn per part (index 256 and up spike or vanish);
- entry 400: an anibnd whose skeleton is rooted at a bone the character has (for a head: `Head`, which then follows
  the hunter's head), with the clips the part plays;
- the clip id that matches the character's current animation plays; `a999` is the fallback (an always-on loop).

When the game is told to use a character's low part set (the `_L` files, as for its armor), the engine asks for
`<part>_L` instead, so ship `<part>_l.partsbnd.dcx` too (the same file is fine).

## What changes in game

- Only player characters (yours and the other players in your world). Each gets the first two rows (table order) whose
  SpEffect it has; the parts go to the character's model slots 15 and 16 (slot 15 the first). A third row with its
  SpEffect on is not shown (logged once).
- The part is requested the way the game requests armor (`parts:/<PART>.partsbnd`), loaded in the background, then
  built by the game itself as the model of slot 15 / 16, the way it builds the helmet (animation player included when
  the part has an anibnd). Nothing is drawn until it is loaded.
- When the SpEffect ends the game destroys the model and the engine releases the file. Slots 15 and 16 (ArrowCase,
  BoltPouch) read the quiver entries of the equipment, which Bloodborne never fills.

## Co-op

Cosmetic: each player's game loads the parts for the characters it has, from the SpEffects it sees on them (a
SpEffect the game does not send to other players shows only on the player's own screen). Players may run different
mods. The hooks are installed for every player with or without rows. With mods turned off (`BB_MODS_ENABLED=0`) no
table is read.

## Logs

- `Mods: animated_attachments: N rows from M mods` (bb-start): the table was read.
- `animated_attachments: feature animated_attachments: installed (3 hooks, slots 15 16, N rows: 9111 HD_A_9761, ...)`.
  `not installed (...)` says why (code differs, `BB_ANIMATED_ATTACHMENTS=0`).
- `animated_attachments: local player (chr 0x...) slot 15: parts:/HD_A_9761.partsbnd -> loaded (SpEffect 9111)`: the game built
  the model; on screen from now on. `-> NOT built (the slot stayed empty)` means the game refused the file.
- `animated_attachments: local player (chr 0x...) slot 15: HD_A_9761 -> unloaded (SpEffect 9111 off)`.
- `... -> not loaded after 10 s (cap state N; is the file in the mod's parts folder?)`: the file was not found or not
  readable.
- `... -> released while still loading (SpEffect off)` / `(character gone)`: dropped before it showed.
- `BB_ANIMATED_ATTACHMENTS_TEST=<part>` (diagnostic): wears that part on the local player at all times, no SpEffect
  needed; with `BB_ANIMATED_ATTACHMENTS_TEST_SECONDS=N` only for N seconds after its first use (tests the unload).
  `BB_ANIMATED_ATTACHMENTS_LOG=1` adds the request and slot-call lines.

## How the engine does it (1.09 disassembly, 20 Nov 2026)

- ChrIns +0x4C0 is the character's parts manager; 0x1CA9790 runs it every frame and requests each equipment part as
  `parts:/<name>.partsbnd` through 0x1422A80 (a refcounted resource; +0x88 the parts resource once loaded). The engine
  requests and releases its parts in a pre-hook of 0x1CA9790, in the same place.
- In the same ChrIns step (0x18F84F0 at 0x18F875A, and 0x18F42F3, 0x1DABF6A, 0x1DAC56B) 0x1C9CD20(ChrIns +0x4D0,
  ChrAsm, manager) calls 0x1C9D070 once per slot 0..23 with the manager entry of that slot. Slots 15 / 16 (ArrowCase,
  BoltPouch) get the entries 8 / 9, the quiver models Bloodborne never equips. 0x1C9D070 reads only the entry's
  resource cap: with a loaded one it builds the slot's model (0x170 bytes, its animation player when the part has an
  anibnd), without one it destroys it.
- The engine hooks the two calls for slots 15 / 16 (0x1C9CEB8, 0x1C9CED1): for the length of the call the manager entry
  holds the engine's cap, then the game's own value goes back. Unloading = one call with the entry as the game has it.
