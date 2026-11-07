# Character save layout used by the launcher's save editor (scripts/saveedit)

Every offset the editor writes was pinned on the test saves (E:\games\bbport\test_saves, 23 non-empty slots
from 14 fixtures, tests/test_saveedit.py `test_every_test_save_parses_and_roundtrips`). Anything not in the
"pinned" table is not editable. Reference for the feature list: Noxde's Bloodborne-save-editor (GPL-3.0). The layout
facts below come from our saves and from docs/party/save_safety.md; the add-item records follow Noxde's `inventory.rs`
(through the Nightwatch save tools, GPL-3.0) and are checked on our saves and in the game (see Adding items).

`name` = the character name field: header section pair 0 at file 0x0C {u32 size, u32 end}, `name = end - size - 0x10C91`
(held on all 23 slots, and on the NG+ save whose name has a trailing space that the old regex missed).

| field | where | evidence |
|---|---|---|
| name | UTF-16, 17 x u16 incl. the 0 (next field at +0x22) | 16 chars max |
| vigor, endurance, strength, skill, bloodtinge, arcane | u32 at name-0x68, -0x60, -0x50, -0x48, -0x40, -0x38 (-0x58 = 0, the unused slot) | the 4 slots of the party fixture equal CharaInitParam rows 2004 (9,12,9,15,7,8 at level 10) and others; order = the param's baseVit, baseWil, (baseEnd 0), baseStr, baseDex, baseMag, baseFai |
| level | u32 at name-0x18 = sum of the six stats - 50 | holds on every test save (class rows start at level 10 = sum 60, level 4 = 54) |
| HP / max HP / base max HP | u32 at name-0x94 / -0x90 / -0x8C; the endurance trio at -0x88 / -0x84 / -0x80, stamina / max / base max at -0x78 / -0x74 / -0x70 | written by the editor when vigor or endurance changes, as a level-up does (the game fills them only at creation and level-up, eboot 0x14e63e8): HP and the endurance trio from CalcCorrectGraph rows 100 / 101 of the player's own gameparam (left as they are without the game files), stamina from the eboot's formula (0x14e6420); real 1.09 saves: Vigor 30 = 1100 HP, Endurance 15 = 100 stamina |
| Blood Echoes | u32 at name-0x14 | save_decode / graft note |
| Insight | u32 at name-0x24 | same |
| remembered chr type (phantom mark) | u32 at name-4: 1 = last written as a summoned guest | 3 saves of the lecture_door fixture = 1, all others 0 |
| NG cycle, ending type | u32, u32 at the offset in header +0x2C (size 8 at +0x30) | NG+ fixture = 2 / 2, all others 0 / 0 |
| inventory | normal list at name+0x1C4: u32 used count + 1984 x {handle, id (category<<28 \| id), count, serial}; then the saved key list and the summoned key list (u32 count + 64 entries) | the used count equals the used entries on every save; empty entry {0, 0xFFFFFFFF, 0, 0}; holes occur in real saves |
| weapon level | id = base + 100 x level (22000300 = Threaded Cane +3) | names from the game's own FMG agree (Hunter Blunderbuss 6000300, Flamesprayer 18100300 at +3) |
| event flags | the 150001-byte section (header table), group/bit rule of tools/party/save_decode.py | |

Item instance table (scripts/saveedit/instances.py; pinned on 3 saves: party level 10 and 90, solo level 544 with 232 gems, all gem
handles resolve): from file 0x54 up to the name area, records by the handle's top byte: 0x80 weapon / 0x90 armor = 60 bytes {handle, id,
durability, u32, u32, 5 x {u32 slot shape (0x80000000 = no slot), u32 gem handle (0 = empty)}}; 0xC0 gem = 40 bytes {handle,
0x80000000 | GemGen id, type (1 blood gem, 2 rune), shape (1 radial, 2 triangle, 4 waning, 8 circle, 63 droplet/rune), 6 x GemeffectParam
id (0xFFFFFFFF empty)}; empty = {0, 0xFFFFFFFF}. Inventory gems (category 8) point at their record by handle; a socketed gem is only in its
weapon's slot. Weapon 1000 is the Memory Altar (slots 1,1,1,2: three Caryll runes and the oath) and 1000000 (Bare Fists) fills empty hands:
both are hidden and never editable. A save keeps stale weapon records too (13 Memory Altar copies on the level 90 save), so only gems in
weapons that are in the inventory are listed. Runes carry hidden effects without text (Milkweed has 3, Beast's Embrace 5), so only blood gem
effects are edited, 1 to 3 (the most the game rolls; the record has room for 6). Editing writes only the 24 effect bytes of that record.

Gem names (menu.msgbnd 魔石名, 0 misses on all 77 gems and runes of the level 90 save): key = GemGen.gemNameIdOffset + shape digit x 1e6
(1:1, 2:2, 4:3, 8:4, 63:6) + 1e5 if any effect is cursed + the first effect's gemNameIdOffset. Cursed = the effect's categoryId is a
GemCategoryParam row with isNegative (741 of 16101 effects). Effect texts: 魔石効果 by effect id.

Endings (docs/party/cutscenes_endings.md): the editor offers all eight ending flags by id; 21-23 roll an ending's staff roll and NG+
on the next load and 6604 strips the key items on the next load, so only change them knowing that.

Not pinned (read only / not offered): moving gems between weapons, gem shapes, storage (a 16-byte list near 0xE1AB4, not
identified), equipped slots, covenant, appearance, spawn lamp. Existing entries are edited and removed; adding is below.

Names: from the player's game (msg\<lang>\item.msgbnd.dcx FMGs: goods, weapon, armor, accessory, NPC names by chr id; menu.msgbnd.dcx event text
7000AABn = the n-th lamp of map AAB, matching the lamp flag 7AAB02nn: lamp counts per area agree, 72102410 "1st Floor Sickroom" = lamp 2412950).
Param row ids (EquipParam*) bound valid weapon levels.

Adding items (ported from Noxde's Bloodborne Save Editor, GPL-3.0, `inventory.rs`; checked byte for byte against the
expected bytes of its own test `inventory_add_armor_or_weapon` on its `newsave`: record, inventory entry, both counters). Offered:
goods (consumable, material, key item, chalice), weapons (at +0) and armour. Not offered: blood gems and runes (their effect rolls
are a separate record), the storage box.
- Goods: the first empty entry of the normal list gets `{0xB0000000 | id, 0x40000000 | id, count, index}`; the index (the 4th u32) is
  `sortId << 12 | (slot + 64)` with the goods row's sortId (what the game writes; an entry with index 0 is kept and counted but the
  game's item menu does not list it). The list's used count goes up by one and the second counter (the u32 right behind the summoned
  key list, name + 34256) becomes the highest slot used (+ 64) or goes up by one when there is no sortId. Counts are held to
  EquipParamGoods.maxNum.
- Weapon and armour: the item instance table (file 0x54, 4096 slots, packed) gets a 60-byte record
  `{handle, id (armour 0x10000000 | id), 0xC8, 0, 1, 5 x {0x80000000, 0}}` in place of its first empty 8-byte slot; the handle is
  `0x80800000` (armour `0x90800000`) `| (highest u16 handle number in the table, gems included, + 1)`. Everything behind moves 52 bytes
  up and the last 52 bytes of the file fall off (not zero on any save we have; Noxde's tool does the same). The inventory entry
  carries the same handle and id, count 1, serial 0.
- The header's section table is not touched, so after an add its absolute offsets lie 52 bytes behind the data. `SaveModel` takes the
  name from the "FACE" appearance block (name = FACE - 34044 - 0x1C4) when the header's place does not check out, moves the flag pool
  and the header offsets that lay behind the name (NG cycle, world-state blocks) by `shift`, and `diff()` then shows the whole
  moved range. The game rewrites the header on its next save.
- Gems and runes: a new loose blood gem or rune is a 40-byte record (`0xC0800000 | n` handle, GemGen id, type, shape, effects) with its
  inventory entry (category 8); a rune carries every fixed effect of its GemGen row (Milkweed's third effect transforms the Kos
  Parasite). Blood gem effects are edited in place (24 bytes of the record).
- Proof in the game (2026-11-05, saveedit079, release 0.7.8 build, harness instance on a copy of the default test save): a copy with a
  goods item, two weapons, the four Gascoigne armor pieces, a blood gem and a rune added loaded ("Saves: check OK", the world up at
  the Hunter's Dream), and the inventory screen listed the new goods item (Beast Blood Pellet x5, with its sortId index). The added
  weapons, armor, gem and rune did not stop the load or the inventory, but their tabs were not looked at (the scripted menu walk
  did not reach them). Not proven: that the game saves such a file again (the run was ended by the harness before a save).
