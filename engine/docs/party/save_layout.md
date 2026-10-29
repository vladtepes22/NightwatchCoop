# Character save layout used by the launcher's save editor (scripts/saveedit)

Every offset the editor writes was pinned on the test saves (E:\games\bbport\test_saves, 23 non-empty slots
from 14 fixtures, tests/test_saveedit.py `test_every_test_save_parses_and_roundtrips`). Anything not in the
"pinned" table is not editable. Reference for the feature list: Noxde's Bloodborne-save-editor (GPL-3.0);
no code of it is used, the facts below come from our saves and from docs/party/save_safety.md.

`name` = the character name field: header section pair 0 at file 0x0C {u32 size, u32 end}, `name = end - size - 0x10C91`
(held on all 23 slots, and on the NG+ save whose name has a trailing space that the old regex missed).

| field | where | evidence |
|---|---|---|
| name | UTF-16, 17 x u16 incl. the 0 (next field at +0x22) | 16 chars max |
| vigor, endurance, strength, skill, bloodtinge, arcane | u32 at name-0x68, -0x60, -0x50, -0x48, -0x40, -0x38 (-0x58 = 0, the unused slot) | the 4 slots of the party fixture equal CharaInitParam rows 2004 (9,12,9,15,7,8 at level 10) and others; order = the param's baseVit, baseWil, (baseEnd 0), baseStr, baseDex, baseMag, baseFai |
| level | u32 at name-0x18 = sum of the six stats - 50 | holds on every test save (class rows start at level 10 = sum 60, level 4 = 54) |
| HP / max HP | u32 at name-0x94 / -0x90 | read only (the game recomputes) |
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

Icons (scripts/saveedit/icons.py): menu/common.tpf.dcx sheets MENU_Icon_GGSSS, 80 px cells, 12 a row, 144 a sheet; id N = group N // 1000,
sheet (N % 1000) // 144 + 1, cell (N % 1000) % 144. PS4-tiled BC7 (TPF format 0x66) or BC1 (0). Item icon ids: EquipParamWeapon.iconId (of
the +0 row), EquipParamProtector.iconIdM, EquipParamAccessory.iconId, EquipParamGoods.iconId; gems 3000 + shape block (1:0, 2:200, 4:400,
8:600, 63:1000) + 100 if cursed + the first effect's gemIconIdOffset. The PNGs are cut on the player's PC into user\cache\saveedit\icons.

Endings (docs/party/cutscenes_endings.md): only 6600-6603 (ending A / B / C / any seen: shop and messenger unlocks) are offered. 21-23 roll
an ending's staff roll and NG+ on the next load and 6604 strips the key items on the next load, so they are never offered.

Not pinned (read only / not offered): the 4th inventory u32 ("serial": a sort key built from a per item part x 40960 plus a counter that is
shared with the weapon/armour handle numbers 0x4E.., so a new entry cannot be forged safely), moving gems between weapons, gem shapes,
storage (a 16-byte list near 0xE1AB4, not identified), equipped slots, covenant, appearance, spawn lamp. The editor therefore edits and
removes existing entries but does not add new ones.

Names: from the player's game (msg\<lang>\item.msgbnd.dcx FMGs: goods, weapon, armor, accessory, NPC names by chr id; menu.msgbnd.dcx event text
7000AABn = the n-th lamp of map AAB, matching the lamp flag 7AAB02nn: lamp counts per area agree, 72102410 "1st Floor Sickroom" = lamp 2412950).
Param row ids (EquipParam*) bound valid weapon levels.
