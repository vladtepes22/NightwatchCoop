# Test save fixtures (start mid-campaign)

Live party tests never start from a new game: every run Continues a test save (auto-continue) and
uses test aids to get to the moment it tests. The saves live **outside the repo** under
`E:\games\bbport\test_saves` (copies only; never the player's `E:\games\bloodborne\user\savedata`).
Each fixture folder holds a `savedata` tree that `tools/mp/instances.py` copies into an instance
(`--saves`, or `BB_TEST_SAVES`); `--fresh-saves` copies it again for every run.

| Fixture (`E:\games\bbport\test_saves\...`) | State | Made by |
|---|---|---|
| `savedata` (the default) | Character "visy", level 37, in the Hunter's Dream; Cleric Beast, Gascoigne, Vicar Amelia dead; night (9800 + 9801); lamps of Central Yharnam / Cathedral Ward; Oedon Tomb Key, Old Hunter Bell; no chalice | copy of the user's 2026-10-12 backup |
| `chalice_pthumeru_ritual\savedata` | The default save after the **Pthumeru Chalice ritual at the First Ritual Altar** (Dream gravestone, slot 1: shared fixed dungeon 1110692594440089, glyph `wmqbhonw`, depth 1); Pthumeru Chalice held, Ritual Blood (1) used, 4000 Echoes; standing at the altar | run altar7 below (2026-10-15) |
| `chalice_hintertomb_ritual\savedata` | The default save after the **Hintertomb Chalice ritual** (goods 6219, depth 2) at the First Ritual Altar: shared fixed dungeon 241243705840283 (glyph `igy8vobq`), the Hintertomb tile set; 4000 Echoes left | run fx_hintertomb below (2026-10-15) |
| `chalice_loran_ritual\savedata` | The default save after the **Ailing Loran Chalice ritual** (goods 6429, depth 4): shared fixed dungeon 1104142770415792 (glyph `5hm6k63y`), the Loran tile set | run fx_loran below (2026-10-15) |
| `chalice_isz_ritual\savedata` | The default save after the **Great Isz Chalice ritual** (goods 6539, depth 5): shared fixed dungeon 4036451041035724 (glyph `9xcp9xg4`), the Isz tile set | run fx_isz below (2026-10-15) |
| `cathedral_day\savedata` | The default save put back **before day -> evening**: 9800 / 9801 off, the Cathedral Ward door event 12400750 off (the door 2401210 is closed and its prompt works again), at the Cathedral Ward lamp 2402950 | run fixture_cw below (2026-10-15) |
| `rom_dead_dream\savedata` | The default save with **Rom dead** (13201800), in the Hunter's Dream: a lamp travel to Moonside Lake (`host_lamp=3202952` / `BB_PARTY_TEST_START_LAMP=3202952`) plays the Blood Moon scene 13201803 right at the arrival (the lamp is inside its trigger) | run fixture_rom2 below (2026-10-15) |
| `blood_moon_yahargul\savedata` | After the **Blood Moon**: 9800..9802 on, Rom dead, 70002802 on, in Yahar'gul (m28_00, the scene's warp 2802958) | run fixture_bm below (2026-10-15) |
| `chalice_pthumeru_cursed\savedata` | `chalice_pthumeru_inside` with a **Cursed** layout (0.7.7 cursedhp): its record in slot 1 has a 9th layout pair {0, 70} (DungeonFeatureParam 70 = Cursed, eventflagId 70), so the game derives the case flag 92905370 itself at the load (a bare flag bit in the slot group is wiped at the load). Host Continue: max HP x0.5 (SpEffect 4730); smoke `chalice_curse` (chc / chn / chw) | made 2026-10-29 |
| `chalice_pthumeru_boss\savedata` | `chalice_pthumeru_inside` with the layer 1 boss's flags 14101800 / 14101809 / 14101686 / 14101693 / 14101708 cleared (`python -I tools/party/make_chalice_boss_fixture.py SRC DST`, the launcher's save editor model): the host Continues in the dungeon's boss room beside the lamp 2901952 and the layer 1 boss 2900100 (HP 3175) is loaded and alive (`host_kill=1` lists it). **Not engaged**: the boss is dormant (invisible, no bar) until its arena is entered by the real route (lever 12900192, layer door, fog); a bare `host_move` next to it does not wake it | made 2026-10-29 (altar077), smoke `chalice_boss` (cab) |
| `chalice_pthumeru_inside\savedata` | A **host inside the Pthumeru dungeon** of glyph `ksxx7rqb` (entered by "Enter the party's dungeon"), layer 1 boss 2900100 dead, standing in its boss room: Continue loads the dungeon, a guest title-joins straight into it (used with the guest `chalice_pthumeru_ritual`); the lift car at the top | run g1b3 below (2026-10-15) |
| `chalice_guest_brought\savedata` | The default save after it **played the host's Pthumeru dungeon** (`chalice_pthumeru_inside`, boss room lamp 2902952 lit) and left the party: the dungeon (glyph `ksxx7rqb`) is in its slot 1 with lamps 2902952's flags, its own altar lists "Chamber of the Seal" and "Layer One: Heart"; standing in the Dream | smoke `chalice_progress` (cpr), 2026-10-25 |
| `ng_plus_guest\savedata` | A **real NG+ save of a playtest guest** (a private player save, never committed; copy of the folder a player sent after the 2026-10-24 party that finished the game): one character, 11.3 h, **NG cycle 2** (the party log's `NG cycle 2 at world-up`), Central Yharnam (m24_01), level 70; loads with `--party` (checked 2026-10-25: `NG guard: own NG cycle 2`). Use for NG-cycle mismatch / NG+ rejoin phases (a host at cycle 0 or 1 beside it). Its NG field is not decoded by `tools/party/save_decode.py` (offset not found yet) | player folder `player_logs/2026-10-25/ng_plus_coop/save_banana` |
| `cleric_beast_lamp\savedata` | The default save with the **Cleric Beast alive again** (its b2_seamless 6.7 flags cleared) and the player at the **Great Bridge lamp 2412952** in m24_01 (spawn spot (-124.8, -27.0, 64.3)); walking ~16 m onto the bridge (region 2412805 at (-136.5, 52.8)) starts the first fight. Used by `tools/mp/soak_party.py` (pad-only soak: no aids during the run) | run fixture_cleric below (confid 2026-10-15) |

No save of its own for Rom's fog door (smoke phase `rf`, group `rom_fog`): it starts from the default save (Rom alive) with `BB_PARTY_PROGRESS_TEST_CLEAR=13201800,13201802,13204800,13204801,9303`, a party travel to the Moonside Lake lamp 3202952 and the test aids `host_flag=13201802` / `guest_flag=13201802` (her fight has begun: the fog door 3201800 on the terrace side of the Lecture Building prompts), `guest_obj=3201800` + Cross for the guest first, `guest_flag=13204801` for the landing (a bare chr warp into region 3202800 does not trigger the landing event 13204821), then the host's `host_obj` + Cross.

Use one for a single instance or the host with `--saves`, e.g.

```
python tools/mp/instances.py run --count 2 --party --fresh-saves --saves E:\games\bbport\test_saves\chalice_pthumeru_ritual\savedata ...
```

For a **different save per instance** (e.g. a guest that has a ritual dungeon and a host that has
none), put the fixture into that instance's own copy before the run: `instances.py setup` copies
`--saves` into `ROOT\instN\user\savedata` only when that folder does not exist, so replace
`ROOT\inst1\user\savedata` with the fixture's `savedata` folder and keep `--fresh-saves` (it copies
`savedata` to `savedata_party`, the folder a party session plays from).

### Distinct-looking players (no save copy needed)

Every instance Continues the same save, so every hunter looks alike and a wrong look on another
player's screen cannot be seen. The director test step `armor=SET` (`BB_PARTY_DIRECTOR_TEST`,
per instance with `--ienv I:...`) puts on armor set SET (EquipParamProtector SET, SET+1000,
SET+2000, SET+3000: head, chest, arms, legs) 3 s into the instance's first world, as the
equipment menu does, and in a session asks the peers to rebuild the chr from the new record (the
mirror's path). Log: `test: armor set N put on: 4 of 4 pieces`. The ids are the world warper's
`kWarperArmorSets` (world_warper_content.inc). Used by the samemodel runs (2026-10-20): host
default look, guest 1 `armor=280000`, guest 2 `armor=200000`, e.g. `--ienv "1:BB_PARTY_DIRECTOR_TEST=armor=280000"`
`--ienv "2:BB_PARTY_DIRECTOR_TEST=armor=200000"` next to the host's steps (3 players, the host's
death at the Tomb of Oedon lamp).

## How a fixture is made (reproducible)

One instance, the default save, `--party` (a host alone), test aids only, then copy
`ROOT\inst0\user\savedata_party` to `E:\games\bbport\test_saves\<name>\savedata` once the game has
saved (its file times; check with `python -I tools/party/save_decode.py <userdata0000> --flags ID,...`).

* **altar7** (`chalice_pthumeru_ritual`): `BB_PAD_FILE=<root>\pad0.txt` (the walk needs the pad
  file) and
  `BB_PARTY_DIRECTOR_TEST=goods=6109:1,goods=7000:2,echoes=5000,approach=-18.27:-16.88:1.3,seq=!cross@Ritual Altar/?cross@Use the Ritual Altar/cross@Search by Chalice Glyph/cross@Select chalice to use/cross@Rite materials/left@Conduct a ritual to create a Chalice Dungeon?/cross/wait/wait`.
  The Dream's ritual altars are gravestones (m21_00 MSB "メッセンジャー_墓石_D..."):
  2100954 "Makeshift Altar" at (-15.26, -5.86, -18.77), 2100955 **First Ritual Altar** at
  (-18.27, -5.43, -16.88), 2100956 at (-20.39, -5.1, -14.14); the chalice set objects 21012x0..x8 sit on
  them. A `stand=` warp onto them does not move the player (the PlayerIns warp keeps the old spot),
  so `approach=` walks there (closed loop on the position) and the `seq=` menu walk starts when it
  arrived (new: `approach` + `seq` without `stand`). Log: `shared fixed chalice dungeon
  1110692594440089 (glyph wmqbhonw ...) made on first use`, "The ritual is finished, and a chalice
  rests upon the altar." on screen.
* **fx_hintertomb / fx_loran / fx_isz** (`chalice_<family>_ritual`): altar7's walk with `prompt=Ritual Altar` (the face taps until the altar's prompt shows) and the family's tab in the chalice list (the list has a tab per family: Pthumeru, Hintertomb, Loran, Isz; Left / Right switch them, wrapping round): `BB_PARTY_DIRECTOR_TEST=goods=6219:1,goods=7001:3,goods=7150:3,echoes=5800,approach=-18.27:-16.88:1.3,prompt=Ritual Altar,seq=!cross@Ritual Altar/?cross@Use the Ritual Altar/cross@Search by Chalice Glyph/right@Select chalice to use/cross/cross@Rite materials/left@Conduct a ritual to create a Chalice Dungeon?/cross/wait/wait` (Hintertomb); Loran: `goods=6429:1,goods=7003:9,goods=7012:4,echoes=9500` and `right@Select chalice to use/right/cross`; Isz: `goods=6539:1,goods=7004:9,goods=7100:3,goods=7050:25,echoes=15500` and `left@Select chalice to use/cross`. Each logs `shared fixed chalice dungeon ... (chalice F, depth D, rites 256) made on first use`; the game saved about 75 s later. The ReturnPointParam rows 2902950..2902956 (the world warper's `chalice` rows) are in every fixed dungeon's MSB, so they lead into whichever dungeon the save has.
* **fixture_cw** (`cathedral_day`): `BB_PARTY_PROGRESS_TEST_CLEAR=9800,9801,12400750`,
  `BB_PARTY_TEST_START_LAMP=2402950`.
* **fixture_rom2** (`rom_dead_dream`): `BB_PARTY_PROGRESS_TEST_SET=13201800`,
  `BB_PARTY_TEST_START_LAMP=2102950` (a load in the Dream, so the game saves).
* **fixture_bm** (`blood_moon_yahargul`): `BB_PARTY_PROGRESS_TEST_SET=13201800`,
  `BB_PARTY_TEST_START_LAMP=3202952`, 140 s (the scene 32000000 plays on arrival, then its warp to
  Yahar'gul; the game saves there).
  `BB_PARTY_TEST_START_LAMP=3202952`.

`stand=obj:ENTITY[:DIST[:ANGLE[:BUTTON]]]` (new) stands at a map object as `host_obj` does, then the
button and `seq=`; for objects on the walkable floor (levers, doors).

* **g1b3** (`chalice_pthumeru_inside`, parfin3): the g1m entry (host default save, guest `chalice_pthumeru_ritual`, `BB_PARTY_ALTAR_AUTO=1`, `approach=-17.0:-16.9:0.35,face=-18.27:-16.88,prompt=First Ritual Altar,seq=cross/?cross@Use the Ritual Altar/wait/down/down/cross`), then `host_move=-45:30.7:-93@30%m29,host_kill=2900100@8%m29`; the host's `savedata_party` copied after the boss-clear (K10). Later runs without `--fresh-saves` Continue in the dungeon.
* **fixture_cleric** (`cleric_beast_lamp`, confid): one instance, `--party`, `BB_PARTY_PROGRESS_TEST_CLEAR=12411700,12411701,12411702,12411703,12414700,12414701,12414223,72410511,2411,9456,2410`, `BB_PARTY_TEST_START_LAMP=2412952`, `BB_PARTY_DIRECTOR_TEST=log_pos`, 100 s; `savedata_party` copied (save_decode: 12411700 / 9456 / 2411 off).

* **A reusable Chalice fixture without a ritual (0.7.6)**: `chalice_guest_brought` is made by the smoke group `chalice_progress` (the guest's save is kept in `<agent>\smoke\cpr_guest_save`); `chalice_altar` (cpa) opens its altar menu (`place=0x15000000:-15.5:-5.43:-16.9:0,approach=-17.0:-16.9:0.35,face=-18.27:-16.88,prompt=First Ritual Altar,seq=cross/...`, `pad=1`; a walk from the save's own spot is blocked at (-12.3, -18.9)). Other agents can start a guest from it to test the dungeon at the altar, or a host from `chalice_pthumeru_inside` / `chalice_pthumeru_ritual`.

* **lecture_door (0.7.6)**: `E:\games\bbport\test_saves\lecture_door\savedata` (EU title folder CUSA00207, slot 0 = map m32_00, about 11.6 h): standing in the Lecture Building right in front of the door to the Nightmare Frontier (door object 3201123, ObjAct 13200123, event 13200311 -> story warp to 3302959). It is a copy of a private player save: kept only under `test_saves`, never committed. Used by smoke phase `ff` (group `frontier_fall`): both players start from it, the host opens the door, nobody may fall before the warp.

* **Altar autopilot + ritual-altar phases (0.7.7 altar077)**: smoke group `chalice_enter`. `cae`: the host (default save) at the First Ritual Altar with the guest `chalice_pthumeru_ritual` summoned into its Dream: `BB_PARTY_ALTAR_AUTO=1`, the altar item "Enter the party's dungeon" (`seq=wait x8/cross/?cross@Use the Ritual Altar/wait/down/down/cross`), the autopilot answers glyph box, Search Results, YES, the altar, the awaken point; both end in m29 (screenshots every 4 s: `smoke\<run>\cae\shots`). `cao`: the host from `chalice_pthumeru_ritual` (its own dungeon, every boss alive) enters it from the First Ritual Altar's awaken list ("Select location to awaken in", Chamber of the Seal, lamp 2902950) with a default-save guest present; `CAO_TAIL=` appends director steps (e.g. `host_move=-45:30.7:-93@3%m29`). `cab`: the host from `chalice_pthumeru_boss`, guest quick-joined; logs the loaded bosses and, via `log_pos`, `test: boss bar N: ... hp H` on both games when a bar is up. The **Makeshift Altar (2100954)** (probed, not driven to the end): Cross without the key item shows "A Short Ritual Root Chalice is required to conduct a ritual."; with `goods=4105:1` it first shows a one-time box ("Use the Makeshift Altar to quick-search and join Chalice Dungeons, or to quick-search and join co-op or even threaten other players."), so its quick-search / short-ritual menu needs more `seq=` steps after the OK (open: no ritual performed at it).
