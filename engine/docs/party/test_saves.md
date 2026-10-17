# Test save fixtures (start mid-campaign)

Live party tests never start from a new game: every run Continues a test save (auto-continue) and
uses test aids to get to the moment it tests. The saves live **outside the repo** under
`E:\games\bbport\test_saves` (copies only; never the player's `E:\games\bloodborne\user\savedata`).
Each fixture folder holds a `savedata` tree that `tools/mp/instances.py` copies into an instance
(`--saves`, or `BB_TEST_SAVES`); `--fresh-saves` copies it again for every run.

The saves are the game's own files (made by playing, or by editing a copy of a save) and are never committed. This page
only describes what each fixture is for; the exact event flags, goods and coordinates of a fixture are read from the save
itself (`python -I tools/party/save_decode.py <userdata0000> --flags ID,...`) and from the smoke phase that uses it
(`tools/mp/smoke_party.py`), not repeated here.

| Fixture (`E:\games\bbport\test_saves\...`) | State | Made by |
|---|---|---|
| `savedata` (the default) | A mid-game hunter in the Hunter's Dream: the first three bosses dead, night, the Central Yharnam and Cathedral Ward lamps lit, the Oedon Tomb Key and the Old Hunter Bell held, no chalice | copy of the user's 2026-10-12 backup |
| `chalice_pthumeru_ritual\savedata` | The default save after the **Pthumeru Chalice ritual at the First Ritual Altar** (Dream gravestone, chalice slot 1: a shared fixed dungeon, depth 1); chalice held, Ritual Blood used; standing at the altar | smoke run altar7 (2026-10-15) |
| `chalice_hintertomb_ritual\savedata` | The default save after the **Hintertomb Chalice ritual** (depth 2): the Hintertomb tile set | run fx_hintertomb (2026-10-15) |
| `chalice_loran_ritual\savedata` | The default save after the **Ailing Loran Chalice ritual** (depth 4): the Loran tile set | run fx_loran (2026-10-15) |
| `chalice_isz_ritual\savedata` | The default save after the **Great Isz Chalice ritual** (depth 5): the Isz tile set | run fx_isz (2026-10-15) |
| `cathedral_day\savedata` | The default save put back **before day -> evening**: time-of-day flags off, the Cathedral Ward door event off (the door is closed and its prompt works again), at the Cathedral Ward lamp | run fixture_cw (2026-10-15) |
| `rom_dead_dream\savedata` | The default save with **Rom dead**, in the Hunter's Dream: a lamp travel to the Moonside Lake lamp plays the Blood Moon scene right at the arrival (the lamp is inside its trigger) | run fixture_rom2 (2026-10-15) |
| `blood_moon_yahargul\savedata` | After the **Blood Moon**: all time-of-day flags on, Rom dead, in Yahar'gul (the scene's warp target) | run fixture_bm (2026-10-15) |
| `chalice_pthumeru_cursed\savedata` | `chalice_pthumeru_inside` with a **Cursed** layout (0.7.7 cursedhp): the record in slot 1 has the Cursed feature in its layout list, so the game derives the case flag itself at the load (a bare flag bit in the slot group is wiped at the load). Host Continue: max HP halved by the curse effect; smoke `chalice_curse` (chc / chn / chw) | made 2026-10-29 |
| `chalice_central_ritual\savedata` | The default save after the **Central Pthumeru Chalice ritual**: a dungeon in slot 1 whose layer 2 boss sets the Bone Ash unlock when it dies; used as the GUEST's save (the host enters it by the altar's "Enter the party's dungeon"), smoke `chalice_insight` (cis) | made 2026-10-30 (insight077) with smoke phase `caq` |
| `chalice_lowerhint_fixed\savedata` | The default save after the **Lower Hintertomb Chalice ritual** (the FIXED chalice): a shared fixed dungeon in slot 1 whose layer 2 boss is a twin pair; both dead sets the Madman set unlock. Used as the GUEST's save by smoke `chalice_madman` (cim) | made 2026-11-05 (cim079) with smoke phase `caq` |
| `chalice_lowerhint_ritual\savedata` | A Lower Hintertomb **Root** Chalice ritual dungeon (a generic ritual chalice): its script folder never starts the Madman unlock event, so it never sets the unlock (cim079: traced). Kept only as a ritual-chalice example; the Madman rows test uses `chalice_lowerhint_fixed` | made 2026-11-05 (shop079) |
| `chalice_pthumeru_boss\savedata` | `chalice_pthumeru_inside` with the layer 1 boss's flags cleared (`python -I tools/party/make_chalice_boss_fixture.py SRC DST`, the launcher's save editor model): the host Continues in the dungeon's boss room beside the lamp and the boss is alive and engaged by the real route (the player starts INSIDE the arena, so the boss stays dormant until a player comes in through the arena's closed door from outside) | run g1b3 (2026-10-15) |
| `chalice_pthumeru_inside\savedata` | A **host inside the Pthumeru dungeon** (entered by "Enter the party's dungeon"), layer 1 boss dead, standing in its boss room: Continue loads the dungeon, a guest title-joins straight into it (used with the guest `chalice_pthumeru_ritual`) | run g1b3 (2026-10-15) |
| `chalice_guest_brought\savedata` | The default save after it **played the host's Pthumeru dungeon** (boss room lamp lit) and left the party: the dungeon is in its slot 1 with the lamp's flags, its own altar lists the dungeon's layers | smoke `chalice_progress` (cpr), 2026-10-25 |
| `ng_plus_guest\savedata` | A **real NG+ save of a playtest guest** (a private player save, never committed; one character, NG cycle 2, Central Yharnam): loads with `--party`; use for NG-cycle mismatch / NG+ rejoin phases (a host at cycle 0 or 1 beside it). Its NG field is not decoded by `tools/party/save_decode.py` | player folder in `player_logs` |
| `cleric_beast_lamp\savedata` | The default save with the **Cleric Beast alive again** and the player at the **Great Bridge lamp**; walking onto the bridge starts the first fight. Used by `tools/mp/soak_party.py` (pad-only soak: no aids during the run) | run fixture_cleric (confid 2026-10-15) |
| `amelia_alive\savedata` | The default save with **Vicar Amelia never killed**, consistently: her defeat flags, intro, skull event and the night flag cleared (evening kept), the Grand Cathedral lamp's lit flag and its common lamp events' completion flags cleared; the Cathedral Ward lamp lit and the last lamp; standing at that lamp. Clearing the night made the eye repair take the Eye of a Blood-drunk Hunter and its flags, as a save without the night must. Walking in through her fog plays her intro and starts the fight | run amfix |
| `play4_guest_poisoned\savedata` | **A real guest save of the 2026-11-01 4-player playtest** (a private player save, never committed; DLC entered): poisoned by the stale summoned-gives replay of CL-55cea7 (several hunter badges given twice, key items without their pickup flags). Plus the default fixture's profile files (without them the game starts at the brightness screen and never loads the character). The save carries the phantom mark (written as a guest); smoke `sgp` clears it in its copy (`repair_phantom_mark`) | player folder |
| `lecture_door\savedata` (0.7.6) | EU title folder, slot 0 = Byrgenwerth, about 11.6 h: standing in the Lecture Building right in front of the door to the Nightmare Frontier (a story warp). A copy of a private player save | player folder |
| `lamp_half\savedata` (0.7.8 hostlamp078, GitHub issue #28) | The default save with two half-lit lamps: the lamp-lit flags set, their object flags clear (one DLC lamp, one Cathedral Ward lamp) | made with the save editor script |

Fixtures that need no save of their own start from the default save with progress test aids
(`BB_PARTY_PROGRESS_TEST_SET` / `_CLEAR`, `BB_PARTY_TEST_START_LAMP`, the director test steps): Rom's fog door (smoke phase
`rf`, group `rom_fog`: Rom's flags cleared, a party travel to the Moonside Lake lamp, the guest's fog first, then the host's
object press), the Lecture Building lamps (`ll`, group `lecture_lamp`), the altar autopilot and ritual-altar phases
(0.7.7 altar077, group `chalice_enter`: `cae`, `BB_PARTY_ALTAR_AUTO=1`), and the Queen Yharnam layer (0.7.7 chalboss077
`caq` makes the Ihyll Root Chalice ritual; `caw` puts the host inside her arena: the arena's only door is a big double door in
the east wall; HP phases via `host_kill=ENTITY:hpNN@S`; the wake comes ~230 s into the run, `CAW_SECONDS=580`).

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
mirror's path). Log: `test: armor set N put on: 4 of 4 pieces`. The sets are the world warper's armor sets,
derived from the player's game data at load. Used by the samemodel runs (2026-10-20): host
default look, guest 1 and guest 2 each `armor=<set>` (`--ienv "1:BB_PARTY_DIRECTOR_TEST=armor=SET"`) next to the host's steps
(3 players, the host's death at the Tomb of Oedon lamp).

## How a fixture is made (reproducible)

One instance, the default save, `--party` (a host alone), test aids only, then copy
`ROOT\inst0\user\savedata_party` to `E:\games\bbport\test_saves\<name>\savedata` once the game has
saved (its file times; check with `python -I tools/party/save_decode.py <userdata0000> --flags ID,...`).

* **Ritual fixtures** (altar7 -> `chalice_pthumeru_ritual`; fx_hintertomb / fx_loran / fx_isz -> `chalice_<family>_ritual`):
  `BB_PAD_FILE=<root>\pad0.txt` (the walk needs the pad file) and a `BB_PARTY_DIRECTOR_TEST` of `goods=` grants (the chalice and
  Ritual Blood), `echoes=`, `approach=X:Y:TOL` (walks to the First Ritual Altar, a gravestone in the Dream, closed loop on
  the position; a `stand=` warp onto it does not move the player) and a `seq=` menu walk (`prompt=Ritual Altar` taps
  the face button until the altar's prompt shows; the chalice list has a tab per family, Left / Right switch them, wrapping
  round; the walk ends at "Conduct a ritual to create a Chalice Dungeon?"). Log: `shared fixed chalice dungeon ... made on first
  use`, "The ritual is finished, and a chalice rests upon the altar." on screen. The exact step strings live in the smoke
  phases `caq` and `cae` (`CAQ_SEQ`, `CAQ_GOODS`).
* **Progress fixtures** (amfix, fixture_cw, fixture_rom2, fixture_bm, fixture_cleric): one instance, `--party`, a
  `BB_PARTY_PROGRESS_TEST_CLEAR` / `_SET` list of the progress flags the fixture needs (read them back from the saved fixture with
  `save_decode.py`), `BB_PARTY_TEST_START_LAMP=<lamp>` so a load happens and the game saves, 100-150 s; then copy
  `savedata_party`.

`stand=obj:ENTITY[:DIST[:ANGLE[:BUTTON]]]` stands at a map object as `host_obj` does, then the
button and `seq=`; for objects on the walkable floor (levers, doors).

* **g1b3** (`chalice_pthumeru_inside`, parfin3): the g1m entry (host default save, guest `chalice_pthumeru_ritual`,
  `BB_PARTY_ALTAR_AUTO=1`, an `approach` / `face` / `prompt` / `seq` walk into "Enter the party's dungeon"), then the host's
  `host_move` / `host_kill` aids to the boss room and the saved host copy.
* **A reusable Chalice fixture without a ritual (0.7.6)**: `chalice_guest_brought` is made by the smoke group `chalice_progress`
  (the guest's save is kept in `<agent>\smoke\cpr_guest_save`); `chalice_altar` (cpa) opens its altar menu.
