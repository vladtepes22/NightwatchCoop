# Party game-data tables (read from the player's own game)

No FromSoftware data is committed. The party code still needs lists that only the game's files know: which maps,
lamps, cutscenes and NPCs exist, which item lots carry a pickup flag, which awards are host-only, the Beckoning-Bell
helpers, the world warper's gear. The engine derives them in C++ on the player's machine, as it starts, and caches
them. No Python runs at game start (a launcher that starts the engine directly and deletes `scripts\`, like
Nightwatch's `bb-start.exe`, gets the tables too).

## Flow

1. `src/probe.c` calls `bbgpu_party_data_start(app0, user_dir)` right after the file system is configured
   (`gpu/shim/gamedata/build.cpp`, `StartBackground`). `app0` is the game view `run.bat` / `run.sh` made (mods overlay
   merged), so mods are respected: the files read are the ones the game reads. `BB_PARTY_TABLES_BUILD=0` skips the
   build; `BB_PARTY_TABLES=<file>` names a ready cache and skips it too.
2. The key is a hash of the derivation version (`kDerivationVersion` in `build.cpp`: bump it when a rule changes
   what the tables hold), and of the name, size and modification time of the params, the paramdef, every event
   script, every MSB, plus the `map` / `remo` listings. No file is read for the key (the old Python key read the
   scripts in full and took 2 s). Key equal to the one in `<user dir>/party_tables.txt`: the file is parsed and
   done (milliseconds). Otherwise the tables are derived, the file is written atomically, and the text is parsed
   into the tables the game serves (so a fresh build and a cached start give the same numbers). The cache keeps the
   text format of the Python era (`BBPARTYTABLES 1`): readable, diffable against the oracle, no second format.
3. A pool of `cores - 1` threads (`BB_PARTY_TABLES_THREADS` overrides; below-normal priority) runs per-file work in
   parallel: one task per event script (read, DCX, parse), per MSB (the heaviest, started first) and for the
   params; then the event expansion per file (parallel), the calls into `common.emevd` gathered in file order and
   `common` expanded last; then the four derivations (items, ids, warper, NPC fog) in parallel. Results are stored
   by index and merged in the sorted file order, so the output never depends on scheduling.
4. Progress: console lines `[party-data] <step>: NN%` (steps: file list, params / events / maps, expanding event
   scripts, deriving the tables) and `[party-data] done in N s: <per step timings>`. Percent is weighted by input
   bytes. `BB_PARTY_DATA_TRACE=1` prints every step's wall time.
5. `gamedata::Get()` (`gpu/shim/party/party_gamedata.h`) waits for the build when one runs; `Preparing()` /
   `PreparingPercent()` never block. Starting a party while it runs raises the notice "Preparing co-op data from
   your game files: NN%" (`party_status_bridge.cpp`); a build that failed raises "Party game data missing" once it
   ended. No build (tests, a named cache): `Get()` parses the cache file the first time a table is asked for. No
   tables or a damaged file: every table is empty, the reason is logged once and the consumers say no (an unknown
   map, lamp or entity id is refused, no lot is shared, no NPC is mirrored, no bell helper is followed). Nothing
   crashes: every reader is bounds checked and a damaged file only fails the build with a message.

Measured (16 cores, 1.09 game with the DLC, 2668 event scripts, 92 MSBs): cold build about 1.8 s wall in the
background (file listing 1.25 s of that on a cold file system, read 0.25 s, expand 0.09 s, derive 0.02 s); the
Python derivation took 18.9 s on the same machine. Cache hit: key + parse, a few ms.

## Readers (`gpu/shim/gamedata/`, namespace `bbdata`)

| file | reads |
|---|---|
| `bytes.h` | bounds-checked LE / BE reads, UTF-16 to UTF-8 (`FormatError` on any out-of-range read) |
| `container.*` | DCX (Bloodborne: `DCP` + `DFLT`, a zlib stream; other compressions are refused), BND4 |
| `param.*` | PARAM + PARAMDEF (field layout, bitfields; `ParamTable::Get(row, "name")`) |
| `emevd.*` | EMEVD (version 0xCC), EMEDF (instruction definitions), EMELD (event names), argument unpacking |
| `events.*` | expansion of parameterised events per InitializeEvent call: flag operations, markers (boss defeat, item lot, cutscene, map move ...), flag reads, calls |
| `msb.*` | the entry lists of an MSB, POINT_PARAM_ST entries, the entity-id word scan |

## Sections (`derive_*.cpp`; `RenderCache` has the text format)

| section | derived by | consumer |
|---|---|---|
| maps returnPoints entities remos returnSpots warpSpots | `derive_ids.cpp` `DeriveIds` | `party_ids.h` |
| npcMirror | `DeriveIds` | `party_npc_mirror.h` |
| npcFog | `DeriveNpcFog` | `party_npcfight_rules.h` |
| warperRight warperLeft warperArmor | `DeriveWarper` | `world_warper_rules.h` |
| flagRows | `derive_flags.cpp` `DeriveFlagRows` (the port of `flag_tool.category_rows`) | `party_progress.cpp` |
| lamps | `DeriveLamps` | `lamp_travel_rules.h` `Lamps()` |
| hostOnlyEvents | `DeriveHostOnlyEvents` | `party_bossfog_rules.h` |
| bossGates gateRules | `derive_worldact.cpp` `DeriveWorldAct` | `party_worldact.cpp` `BossGates()` / `GateRules()` |
| bossEntities | `DeriveBossEntities` | `party_balance.cpp` (the boss chrs of a pass) |
| shopGates | `derive_shop.cpp` `DeriveShopGates` | `party_items.cpp` `ShopGateWeapon` / `ShopGatesToRepair` |
| bossArenas firstFightSpots | `derive_spots.cpp` `DeriveSpots` (MSB points and parts) | `world_warper_rules.h` `BossArenaSpot`, `party_worldact.cpp` `FirstFightSpotFor` |
| stateIds runtimeIds | `derive_progress.cpp` `DeriveProgressIds` | `party_progress.cpp` `ModeFor` / `HostCandidate` |
| gestureNames | `build.cpp` (the item-name FMG, `fmg.cpp`) | `party_gamedata.h` `GestureText` |
| item* (lot flags, ledger, award spots, unshared, shared treasure, boss drops, audit rows, kill awards) | `derive_items.cpp` + the flag category table (`derive_flags.cpp`, `flag_category_rows.inc`) | `party_items.cpp` |

The ledger bit of a flagless award is `60009000 + index` in the player's own save, so an index must never change:
`kLedgerLegacy` keeps the indices the releases up to 0.7.9 gave out (our own flag allocation) and a new lot takes
the next free one from 43.

bossGates (`guest_event host_flag guest_flag boss_flag intro_flag fog_entity landing_event`): per event named
`ゲストがボス部屋入場`, its 3[0] flag conditions in order (boss defeated, host's first-fight entry, host entered last),
the 3[24] action button's object (the fog) and the flag it sets (the landing event `ゲストが着地` that waits on the
same host flag, when the entry event sets none). An entry event with two conditions takes as intro flag the host's
`ホストがボス部屋入場_初戦` event of that boss that sets its own id. gateRules (`boss_event gate_flag also`): every
boss-defeat event (2003[12/15/53]) and the 2000-3999 flags it sets to 1, the first event to set a gate flag owning
it (the Omega / Patient B re-fights set the same ones), plus an event that sets a gate flag once two boss-defeat
flags are on (`also`). On 1.09 both equal the tables that were typed into `party_worldact.cpp` (20 gates, 29 rules).
bossEntities (`entity chalice`): the entities given SpEffect 7500 / 7501 by a 2004[8] instruction, per instance of the
events (the chalice dungeon boss events of `m29.emevd` are expanded once more with the calls the dungeon maps make into
them, `chalice` = 1); equal to the former arrays (34 + 66 entities) on 1.09.
`cpp-only`: the Python oracle has no counterpart, `tests/test_party_tables.py` leaves them out of its comparison;
`gamedata-test` covers them with made-up event records. Without the sections no gate, fog gate or rule applies.

shopGates (`flag weapon`): the Messenger shop's per-player weapon gates. A shop row (ShopLineupParam, weapons) gates a weapon
behind an event flag (`qwcId`) that only means "this player got the weapon": the pickup flag of an item lot that gives
exactly that weapon (the Chalice dungeons' Uncanny / Lost weapons, 5100-5282 on 1.09), or the completion flag
`5500 + slot` of common event 5500, whose 3[4] (player holds item) names the weapon the shop gates on that flag (the Old
Hunters ones, 5500-5521). Flags that gate several weapons, lots with several items and progress gates (boss / NPC
events, 5090 / 5091, 59xx) are not in it. On 1.09 the 52 rows equal the `kShopGates` array that was typed into
`party_items.cpp` (30 lot rows + 22 event rows). Without the section the shop-gate repair decides nothing and does not mark
the save.

bossArenas (`entity x y z`) and firstFightSpots (`event x y z chained scene`) are read from the MSBs of the maps of the
boss gates (`derive_spots.cpp`; file header has the rules). bossArenas: per gate, the arena's entry floor is the POINT named
"warp target when stuck in the fog wall" (`霧壁埋まりワープ先`) within 5 m of the fog object, else the nearest "room entry
target" POINT within 40 m; it is given to the boss characters the gate's boss-defeat event waits on or marks, and to the
character parts of the map whose entity id differs from one of those only in the last digit. On 1.09: 46 rows; the 24
typed ones (`kBossArenas`, world warper) are all there and 23 of them equal (within the typed rounding); Rom (3200800) differs: the
typed spot was a corner of the boss room (a hand pick), the derivation takes the stuck-warp point just inside her fog
(96 m higher than the typed spot). The 22 extra rows are other characters of the same fights (Micolash,
the Living Failures, the Orphan's second phase and so on) that the warper never asks for (it asks for boss entities
only). firstFightSpots: the host's first-fight event of a gate (its `intro` event) has one position condition: 3[2] (player in a
region) gives the region's POINT raised 0.5 m, 3[3] (player near a character) the character's part raised 0.5 m and
moved 2 m along z; `scene` = the event plays a cutscene (2002), `chained` = its name says second half (`後半`). Events with
no such condition (Ebrietas, Rom, Lady Maria: the fight starts by an action) have no row. On 1.09 the 17 rows equal the typed
array in `party_worldact.cpp`, flags included.

stateIds / runtimeIds (`flag`): flags the progress sync treats specially, scanned from the map scripts (`derive_progress.cpp`).
stateIds: every shortcut_door / world_state flag a map script (not common) writes to 0 or toggles (elevators, gates, bridges
that the game moves back and forth): the sync copies the host's current state instead of only turning flags on. runtimeIds:
the flags in those two categories the scripts count or randomise (value operations, RandomlySetEventFlagInRange): never
synced. The typed lists these replace were the result of the same scan plus a hand review. On 1.09 the scan finds 69 state
flags (43 of the 45 typed ones, the other two are our own list; the 26 more are runtime flags that stay excluded by `kExcluded` or `runtimeIds`, an area 29 chalice flag
that is never writable, and the DLC clean-up's flag in `common`, which is not read) and 23 runtime flags. The effective result,
computed over all 243,577 flags of the candidate categories, is identical to the old typed `kStateIds` + `kExcluded`:
`kExcluded` shrank from 22 to 10 ranges (the personal blocks 6000-8999 and the nine runtime / fight-state ranges that are our
own decision about flags the category table cannot tell from a door) and `kOwnStateIds` keeps the two elevator events'
own flags (12400147 / 12400148, which the scripts do not write). The guards for endings, area gate flags, 9180 and the
time of day went: the category table's own never_sync / time_of_day rows already cover them. Without the tables the category
table is empty too, so nothing is shared at all.

Adding a table: derive it in a new `derive_*.cpp` (or extend one), add it to `DeriveAll` in `build.cpp`, parse it in
`Parse` of `party_gamedata.h`, bump `kDerivationVersion`, and add it to the Python oracle if it has a counterpart.

## Tests

* `gamedata-test` (`tests/test_gamedata.cpp`): the readers and the derivation on synthetic data built in the test
  (DCX, BND4, PARAM + PARAMDEF, EMEDF / EMEVD with a parameterised event, MSB, a synthetic game folder, a param
  edit that the next build follows, cache reuse, no game data). `cmake --build out/gpu --target gamedata-test`.
* `party-gamedata-test` checks the cache parser with made-up numbers.
* `tests/test_party_tables.py` builds the tables with `party-tables-cli` (`tests/party_tables_cli.cpp`) and with the
  Python oracle (`scripts/partydata/oracle.py`, dev only, never run by the game: the former derivation, kept as the reference; the save editor still imports `flag_tool` from the same folder)
  on `BB_GAME_DIR` (else the clone's `../game`) and requires the cache text to be byte-identical (the key line
  aside); a second test does the same on a temporary mod view whose param file has one pickup flag edited, and
  requires the tables to follow it. Skipped without a game folder or the built CLI.
* Tests that need real tables build a cache from the clone's game folder and skip without it:
  `out/gpu/party-tables-cli.exe <game dir> <dir>/party_tables.txt`, then `BB_PARTY_TABLES=<dir>/party_tables.txt
  out/gpu/<test>.exe` (`tests/party_tables_test.h`). `tests/test_item_audit.py` runs the Python item audit on the
  game (`python -I scripts/partydata/item_tool.py audit GAME --out FILE`, written only where asked, never into the repo).

## Event-flag categories (`flagRows`)

`party_flags.inc` (277 KB, 2761 rows) is gone. `party_progress.cpp` reads the table from the `flagRows` section
(`first<TAB>last<TAB>category<TAB>note`, file order = match order, first match wins, no row = never_sync;
`FlagRow` in `party_gamedata.h`). A row naming a category this build does not know is skipped. Without the section
every id is never_sync, so nothing is shared (progress, doors, lamps, NPC quests, shop gates) until the tables exist.
`Rows()` is built on first use, so tests install their tables (`tests/party_tables_test.h`) before the first lookup.

Derivation, in match order (reference implementation: `flag_tool.category_rows`, prints with
`python -I scripts/partydata/flag_tool.py table GAME`; the C++ port `DeriveFlagRows` reproduces its rows exactly: on 1.09 the 2761 rows, notes included, equal the former `party_flags.inc`, and the whole cache text equals the oracle's):

1. Covenant rows (`COVENANT_ROWS`, 15 ranges) and override rows (`OVERRIDE_ROWS`, 87 ranges: the area-gate flags, endings,
   cutscene / time-of-day flags, per-player Doll / messenger / badge flags). Hand rules with our own notes, no game scan.
2. Specific rows (about 1700 ranges) from the game: every EMEVD script expanded once per `InitializeEvent` call with its
   own arguments (name from the matching `.emeld`), plus ItemLotParam / ShopLineupParam / ObjActParam. Per event record
   (done flag = event id + slot for events >= 10000000), priority wins, equal priority = last write:
   boss-defeat marker (2003[12/15/53]) -> the done flag (9) and the single flags the event sets, except ids below 2100,
   5000-8999 and 9000-9451 (8) -> `boss_defeated`;
   lamp events (`ワープOBJ_起動`) -> `lamp_unlocked` 7AAB02xx (9), other 7AAB0xxx reads -> `session_runtime` (6); lamp
   registration 2009[5] -> `lamp_unlocked` (9), ladder 2009[0] -> `shortcut_door` (7); restart-on-reload events in zones
   0/1 -> `session_runtime` (3); by event name keyword (key events, door / lever / elevator words, chest / item-lot
   marker, cutscene marker, NPC words) -> `key_event` / `shortcut_door` / `item_lot_picked` / `cutscene_seen` /
   `npc_quest` (6 / 6 / 5 / 4 / 4); ItemLotParam getItemFlagId and cumulate flags -> `item_lot_picked` (2), ShopLineupParam
   eventFlag -> `never_sync` "shop stock row" (2), and each qwcId gate that has no lot, is not 5500-5521 / 6000-8999 /
   type 9 and would otherwise be item_lot / never_sync / session_runtime -> `world_state` (10). Contiguous ids of one
   category inside one 1000-flag group merge into one row.
3. Global rows (`GLOBAL_ROWS`, about 50 ranges, our notes) and the structural rows: per map of the flag store's 17-map
   table and per type 1/5/6/7/9 and zone 0-9 a default (`ZONE_DEFAULT`), the m29 layouts, chalice instance areas 41-46
   and area 99 -> `never_sync`.

The scan reads the player's own files, so a mod that changes a script or a param changes its rows.

Other consumers of the same derivation, none of them committed data:
- `scripts/saveedit/flagsdata.py` `derive(game)` (the save editor is Python): the boss kill flags (`boss_defeated` rows
  of the form 1AAB1800 / 1AAB1850, areas below 40, not 29) and lit-lamp flags (`lamp_unlocked` rows 7AAB02nn, same area
  rule), plus each boss's name id for the name lookup: the name its health bar shows, 2003[11] DisplayBossHealthBar
  (on, entity, slot, name id) of the entity its boss-defeat marker names (a defeat event that names another entity than
  the bar, m26's, is found by the bar's own event, kill event + 3002), the key of its text in the NPC name list. All 19
  bosses get their name (the NPC param id route, MSB part -> param id at +8, named only 14 of them). `names.Catalogue` caches the result (`progress`) with
  the item names and the launcher hands it to `party_session.check` for the after-session save comparison. Same lists
  as the old typed ones: 19 bosses, 44 lamps (equal on vanilla).
- Tools (`save_check.py`, `save_decode.py`, `shop_gates.py`, `flag_tool.py dump / diff`) read the section from a cache
  file (`--tables`, default `BB_PARTY_TABLES` or `user/party_tables.txt`) or derive it from the game folder.

## Lamps, boss phase events, gesture names (no typed tables)

* `lamps` (`AAB<TAB>n<TAB>group`): every lamp whose lit flag 7AAB02nn the category table files under `lamp_unlocked`
  (areas below 40, not the chalice area 29): 44 on 1.09, group and set equal to the former typed `kLamps`. The
  Hunter's Dream headstones' menu layout (`Headstones()` in `derive_flags.cpp`: which maps each headstone lists, in
  order; map 320 splits by lamp index) is our own layout of the warp menu, the talk scripts are not read; a lamp of an
  unlisted map goes last under the first headstone. Within Upper Cathedral Ward, Cainhurst, Yahar'gul and Nightmare
  of Mensis the lamps are now listed by lamp index (the old table had them in the talk script's order: 9 of the 44 swap
  places in the warp menu).
* `hostOnlyEvents`: the id of every event whose EMELD name holds `ヒートアップ` (a boss's HP-driven phase change): the 32
  ids of the former typed list on 1.09, Ludwig's included.
* `gestureNames`: the 23 gesture names (goods 800 .. 822) from the item-name FMG of `msg/<language>/item.msgbnd.dcx`
  (English folders first), read while the tables are built; the rest of the code shows `Gesture <goods id>` when the
  section is missing. `item.msgbnd.dcx` of every language is part of the cache key.
