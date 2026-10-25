# Party balance: death debuff (C4), enemy scaling (D7), overhead display (E2)

Code: `gpu/shim/party/party_balance.{h,cpp}` (game side), `party_balance_rules.{h,cpp}` (pure,
`tests/test_party_balance.cpp`), settings in `bbport_settings.h` (`party_rot`, `party_boss_*`,
`party_enemy_*`, `party_overhead`), the System menu page "Party rules" in `bbport_game_menu_pages.cpp`, the
overlay's Party tab and tags in `bbport_overlay.cpp`, the VITALS trailer in `party_link.cpp`.
Addresses are our offsets (raw ELF VA of `out/eboot.elf`, 1.09).

## 1. The party's rules come from the host

The host's settings are the party's: `balance::Tick` (director tick, party mode) posts EVENT
`balance` `{"rot":true,"boss_hp":150,...}` to every guest on change and every 15 s (late joiners);
the guest's runtime hands it to `balance::OnHostRules` (JSON parsed with limits, every value clamped
to the menu's range). The host also sends it at once when a guest connects (0.7.3), so a guest
uses its own settings only until that first event arrives. On a guest every rule is a read-only line
with the host's value (System > Party rules: Enemies: "The host's setting: ...", from
`party_status::HostScaling`; the overlay's Party tab: notes); the guest's own `bbport.ini` values
are never changed by the host's and count again when it hosts. `BB_PARTY_BALANCE=0`: nothing (no hook, no param write, no
event).

## 2. D7: enemy scaling

### 2.1 What Bloodborne does (measured)

Read from the 1.09 params (`tools/party/flag_tool.py` layouts over `gameparam.parambnd.dcx`) and
the code (docs/party/four_players.md section 4):

- **Bosses**: the boss events (EMEVD `1003[109]` by cooperator count -> `2004[8]` SetSpEffect +
  `2004[53]` HP refresh) and native `Lua_MultiDoping` 0x138bc70 (chalice bosses) put SpEffect
  **7500** (1 cooperator) or **7501** (2) on the boss; party_fourp H4 uses 7502 for 3.

  | Row | maxHpRate | regist*ChangeRate | changeSuperArmorPoint | attack power rates |
  |---|---|---|---|---|
  | 7500 | 1.5 | 1.5 | +30 | 1.0 |
  | 7501 | 2.0 | 2.0 | +60 | 1.0 |
  | 7502 | 2.5 | 2.5 | +90 | 1.0 (unused in 1.09) |
  | 7503 | 3.0 | 3.0 | +120 | 1.0 (unused in 1.09) |

  So vanilla per extra hunter: boss HP +50 %, status resistance +50 %, poise +30, damage +0.
- **Normal enemies**: no co-op scaling at all. The max-HP rate 0x17e46e0 multiplies only the active
  SpEffects' `maxHpRate` (read live from the param row, node +0x48 -> row +0x10), the NG+ table
  0x4731ed0 by clear count and the 9200 level sync. BB's gameparam has no multiplayer correction
  param (no MultiHPCorrectionParam etc.).

### 2.2 Settings

System > Party rules: Enemies: six sliders (boss / enemy health, damage, poise; the help line shows the
value), also in the overlay's Party tab and bbport.ini. The page layout the port uses
("ControllSetting") has six rows; the System list itself scrolls, and the port's page cap
(MaxPages, one factory per page: our own constant, not the game's) is now 16. Live 2026-10-11
(single instance, the title's System menu walked with BB_TEST_MENU_FILE, BB_GAME_MENU_TRACE):
`Game menu: page 11 Party rules open`, `Game menu: page 12 Enemy scaling open` (the 13th page).

Per extra hunter (cooperator), Nightreign semantics where vanilla has a step (100 = vanilla):

| Setting (ini) | Unit | Range | Default | Vanilla |
|---|---|---|---|---|
| Boss health (`party_boss_hp`) | % of the boss HP step (+50 %) | 0..400 | **150** (+75 % HP per hunter) | 100 |
| Boss damage (`party_boss_damage`) | + % damage per hunter | 0..100 | **10** | 0 |
| Boss poise (`party_boss_poise`) | % of the poise step (+30) | 0..300 | 100 | 100 |
| Enemy health (`party_enemy_hp`) | % of the boss HP step | 0..400 | **50** (+25 % HP per hunter) | 0 |
| Enemy damage (`party_enemy_damage`) | + % damage per hunter | 0..100 | 0 | 0 |
| Enemy poise (`party_enemy_poise`) | % of the poise step | 0..300 | 0 | 0 |

Status resistance follows the HP setting (as in vanilla). Why these defaults (user decision
2026-10-11: harder than vanilla, ERSC-like boss boosts, adjustable): a party of three with free
builds, no tether and quick respawns trivialises vanilla bosses (2.0x HP for two cooperators);
ERSC's own defaults add per extra player +100 % boss HP and +35 % enemy HP with no damage
scaling. Ours: two cooperators give a boss 2.5x HP (vanilla 2.0x) and +20 % damage, normal enemies
1.5x HP; one cooperator 1.75x / 1.25x. Damage stays modest because Bloodborne's rally lets the
party regain HP. ERSC's note that in 3-4 player parties big enemies' poise drops (more
staggerable) is not offered: negative poise was tried (-100..-25 %) and measured live to make
enemies stagger *less* (0 of 20 hits at -100 % against 18 of 40 at 0 %, misc071), so the poise
sliders start at 0 % (user decision 2026-10-21). A negative value from an older bbport.ini, the
menu or a host's rules reads as 0. The default keeps vanilla poise.

### 2.3 How

- Rows: every second the host's values go into SpEffectParam 7500 / 7501 / 7502 (bosses with 1 / 2
  / 3 cooperators) and 7503 (normal enemies, for the current cooperator count from the game's own
  count 0x15bdc20, identical on every peer): maxHpRate +0x10, attack power rates +0x48..+0x54,
  status resist rates +0x108..+0x114, changeSuperArmorPoint +0x13a (s16). A row is written only
  while it holds the 1.09 value or what we wrote last (else logged and left alone); a param reload
  is re-written.
- Normal enemies: every 2 s, with cooperators present and any enemy setting not zero, every chr of
  this world's chr sets (WorldChrMan +0x850 tables) that is not a player and whose NpcParam row
  (`[[chr+0x3c8]+8]`) has teamType 23 (5792 of 6010 rows: the hostile enemies) and no
  isSoulGetByBoss (0x149 bit 4) gets SpEffect 7503 (chr vfunc +0x3f0), unless it is a boss: the 34
  entities the boss events dope (EMEVD 2004[8] with SpEffect 7500 / 7501, scanned in every 1.09
  map; `kBossEntities`), resolved each pass with GetChrByEntityId 0x13c97a0, never get it, so a
  boss is scaled once, by the boss rows. (The first version removed 7503 from a boss when its
  doping arrived, 0x17e1d90 = EMEVD 2004[21]'s remove; the Cleric Beast died in that run, but it
  also died ~30 s into later fights with no removal and no guest death, see 2.4, so the removal
  was probably not the cause. The entity table avoids the question.)
  Chalice bosses (m29): the 66 entities the parameterised chalice boss events (12904877, 12904881,
  12906791, 12906795, 12906806) are started with (2000[0] / 2000[6] arguments; `kChaliceBossEntities`,
  resolved only in area 29), plus isSoulGetByBoss. The game's per-frame HP update (0x1a0d7d0) recomputes max HP from the
  rows and raises current HP with the "independent" rate, so a full-health enemy stays full.
- Same HP on every screen: every peer writes the host's values and applies them to its copy of the
  enemies; the cooperator count is the game's, which counts the same on host and guests.

### 2.4 Live (2026-10-11, Cleric Beast, host + 1 guest)

`BB_PARTY_PROGRESS_TEST_CLEAR` un-killed the Cleric Beast, `host_move` into region 2412805 started
the fight. Both peers: `boss chr 1001409f: SpEffect 7500 (flags 4): HP 5277 / 5277 base 2700, rate
1.955` (2700 x 1.117 area row 7002 x 1.75; vanilla 1.5 would give 4523); the boss and Father
Gascoigne's two entities stayed out of 7503 (`3 bosses`). Normal enemies: sample max HP 520 ->
650. Damage, boss_damage 0 vs 100 (x1.0 vs x2.0 attack power at 1 cooperator): the players' hits
were 61-126 (largest 239) vs 216-534 (the defense formula is not linear, so not exactly x2). Poise:
see 2.5.

### 2.5 Poise: the game's own boss poise bonus never applied

Every poise sum in the engine (0x17e8590, 0x18d0351, 0x18fd400: `cmp word [row+0x156], 0x9b`)
counts an effect's changeSuperArmorPoint (+0x13a) only when its stateInfo (+0x156) is 155. The
1.09 doping rows 7500 / 7501 carry +30 / +60 with stateInfo 0, so vanilla bosses get no poise
from co-op at all (15 other rows with a poise change do use 155). Our rows set stateInfo 155
whenever the poise value is not 0, so the poise settings work (and boss poise 100 % = the +30 per
cooperator the game's data intended). The boss log line shows the engine's sum ("poise bonus +60
(counted)").

## 3. C4: death debuff ("rot")

ERSC: optional rot, 5 kinds, one random per death, stacking, removed only at a grace. Ours
(Bloodborne-themed): all five kinds in force since 0.6.2, all live-verified: health, stamina,
stamina cost, rally and item discovery (see the table).

- Each death of the local player (HP > 0 -> 0 in the world, any role) adds a stack, at most 5, of
  a kind picked at random among the kinds in force; a toast after the respawn names it ("Rot 2/5:
  stamina costs +10 % until you rest at a lamp"). `BB_PARTY_ROT_KIND=N[,N...]` forces the kinds (tests).

  | Kind | Per stack | Rate function (1.09, prologue replaced) | SpEffect field it multiplies |
  |---|---|---|---|
  | health | max HP -10 % | 0x17e46e0 `55 48 89 e5 53 50` | maxHpRate +0x10 (HP update 0x1a0d7d0) |
  | stamina | max stamina -10 % | 0x17e4a20 `55 48 89 e5 48 83 ec 10` | maxStaminaRate +0x18 (0x1a572e0) |
  | stamina cost | stamina use +10 % | none: the tick (0.6.2) | each stamina drop of the player times (factor - 1) taken from the bar (`RotExtraStaminaDrain`); the getter 0x17e4c50 (consumeStaminaRate +0x1b8) is never asked for a roll or a swing |
  | rally | rally (regain) -15 % | **in force** (0.6.2; `BB_PARTY_ROT_RALLY=0` off) | the getter 0x17e5590 (regainRate +0x1f4) was never asked in live fights, so the tick takes back 15 % a stack of each HP gain that comes within 6 s of damage (`RotRallyGainInWindow`) and is not a Blood Vial's (`RotRallyTakeBack`). Live 2026-10-15 (parfin c4, two stacks): 44 HP regained in 4 gains, 13 taken back |
  | discovery | item discovery -10 | **in force** (0.6.2; `BB_PARTY_ROT_DISCOVERY=0` off) | the itemDropRate sum 0x17e8010 is never asked, but every enemy drop roll's request carries the player's discovery (party_droproll, request +4; 1.0 = the stat screen's 100): each stack takes 0.1 off there, floor 0.1 (`RotItemDiscovery`). Live 2026-10-15 (parx s0b, two stacks): `shared drop roll ... item discovery 0.80 (own, rot: 1.00 before)` on the guest's game for each host kill |

  Live 2026-10-15 (parceh2 s1b): a roll from a full bar cost 10 before the stamina-cost stack and
  11 after it (`rot stamina cost: 100 -> 89 (+1 for x1.10)`); swings in a fight 20 and 27 extra
  over 9 and 13 drops.

  Each hook changes the result only for the local player's SpEffect list (`PlayerIns+0x1c8`). No
  param row or SpEffect is used (no unused SpEffect rows could be proven unreferenced), so nothing
  reaches the other peers' copies. ERSC's "damage dealt" kind is replaced by stamina cost / rally:
  the attack power rates are summed inside a large aggregator (0x17e5a30, ~7 KB of code), not a rate
  function. (SpEffect paramdef note: `hasTarget : 1` has spaces; flag_tool now parses it, so the
  fields after +0x164 have their real offsets.)
- A lamp / headstone warp the player starts (0x13CDF30, party_travel `SetLampUseObserver`,
  before the guest lamp interceptor) cleanses it; the respawn warp of a death (within 20 s of it)
  does not. Travelling along with the party (a replay) does not either: only who rests is cleansed.
- Shown (2026-10-20): System > Party has a *Rot* row (value "3/5", help "Rot 3/5: max HP -20 %,
  stamina costs +10 %. A lamp rest cleanses it"; `DescribeRot`), and the party HUD panel a line
  with the same text under the companions while there is rot. Each player shows its own stacks
  (party_status `SetRot`, set by the balance tick; zeros while the party's rule is off); the
  companions' stack counts are in the name tags' "Name and deaths". Live 2026-10-20 (uifix): a
  guest's death in the host's Dream (`BB_PARTY_ROT_KIND=0`, then 2): "Rot 1/5: max HP -10 %" /
  "Rot 1/5: stamina costs +10 %" under the party HUD after the respawn (screenshots); the Party
  page row is unit-tested (game-menu-pages-test), not seen live.
- Kept in `<user>/party_rot.txt` across restarts. Party mode only; host and guests alike; the
  host's on/off is the party's. Setting: System > Party rules: Rot > "Rot on death" (0.7.2: its own page; before on Deaths & revives) (and the overlay's Party tab, the
  launcher's Party Rules tab). Off by default since 0.7.2 (user decision 2026-10-22; on 0.6.0 - 0.7.1): an unset
  `party_rot` is off. The game's settings save (`BbSettings::Save`) wrote every key, so 0.6.0 - 0.7.1 left
  `party_rot=1` in most bbport.ini files unchosen: bbport.ini `settings_rev` 1 (`kSettingsRev`, launcher
  `INI_SETTINGS_REV`) sets `party_rot=0` once, in the game's settings load or the launcher's start, whichever comes
  first (coordinator decision 2026-10-22); a later explicit 1 stays.
- 0.7.2, what else cleanses it (`party_rot_rules.h`, unit test `party-rot-test`; three host rules on by default, rows
  under "Rot on death" on System > Party rules: Rot, ini `party_rot_cleanse_boss` / `_killer` / `_echoes`, an older host's rules: off). The
  `Ledger` keeps one entry per death that added a stack: its kind, its killer (entity id / chr / census key) and
  whether its killer still lives (this world) and its bloodstain is still there (the last death's only).
  - boss: the game's boss clear here (`BossBonusHook`, 0x1335400 with the boss entity) or the host's boss notice
    (phantom insight / boss_bonus events, the guest in the host's world): all stacks off, the ledger cleared.
  - killer: the lethal hit's attacker from the game's kill log 0x1FEB2A0(victim, attacker, ...) (the damage step
    0x1A2A600 calls it when a hit leaves the victim at HP 0 with an attacker; a trap object goes to 0x1FEBFF0
    instead; falls and ForceDead call neither): a non-player chr within 3 s of the HP 0 tick. Matched by its
    entity id in the kill record (a party kill seen here or relayed by the host) or its own chr seen at HP 0 here
    (same census key, no load since). A host load, the kill record's new world (rest, wipe) or, without a kill
    record that follows the host, any load drops the links; a guest's own load drops only the chr.
    An enemy without a map entity id (0 / -1) is not in the kill record: its death counts only when this
    player's own game sees that chr die (before its next load); a kill elsewhere in the party does not reach it.
  - echoes: party_b2's own stain watch (flag 1 -> 0 and the echoes back within 5 s) or the party stain recovered
    in the host's world: the stain's death's stack, once (not if its killer's death took it first).
  - A rule off still uses up its link (the event happened); the death keeps its stack and the other cleanse can
    still take it. `party_rot.txt` keeps the stain's kind (`slot S kinds ... stain K`; an older build ignores it).
  - Notices: "Rot cleansed: boss defeated", "Rot -1: your killer is dead", "Rot -1: Blood Echoes retrieved".

## 4. E2: overhead display

ERSC `overhead_player_display`: normal / none / ping / level / death count. Ours: the party HUD's
name tags (projected with the game camera above each companion in view, 1-80 m), now a setting of
their own, independent of the HUD panel: System > My party: Display > "Above companions": Nothing / Name
/ Name and level ("Hunter1  Lv 37") / Name and health (default, the HP bar as before) / Name and
deaths ("Hunter0  deaths 1, rot 1"). Local only. Bloodborne itself draws a phantom plate
(FrpgMenuDlgFloatingPCGauge: blue role line, name, thin HP bar), but hides it whenever anything is
in the way (live 2026-10-12: shown at 3-26 m in clear view, gone at 10-23 m behind a tree or a
corner), so in party mode it is off (party_hud_game.cpp: the entry setter FUN_0176cfa0 stores its
visible byte, MenuMan+0x900+i*0x90+0xc, as 0 at 0x176cfc3; restored outside a party;
BB_PARTY_HUD_GAME_PLATE=1 keeps it for comparisons) and ours replaces it everywhere in its style:
the HP bar's bottom at 1.82 m (the plate's bar, measured against it in screenshots), name
left-aligned on a bar 0.096 of the display wide, fixed screen size, the plate's blue, Libre
Baskerville (no role line). Our tag also adds level, deaths, rot when chosen. Ping is not offered (the Party members page shows it).

The HP behind the bars: data module (`chr+0x3b0 -> +0x20`) +0xf8 HP / +0xfc max, the HP struct the
game's kill (0x18bf9a0) and per-frame update (0x1a0d7d0) use. The party HUD read +0x128 / +0x12c
before, which stayed at 106 / 106 on a 719 HP hunter in the live runs (so the bars never moved);
party_hud_game now reads +0xf8 / +0xfc. (party_b2's guest HP keep still uses +0x128: worth a
look by its owner.)

**Reusable for I2 (pings, through-wall markers).** `party::hud::WorldToScreen(view, proj, world
point, display w, h, margin)` (party_hud.h, pure, tested in party-hud-test) projects a world point to
display pixels with the game camera (`BbCamera::Latest()`: view 3x4 + projection scales, captured
from the frame's constants), reports in-front / on-screen, and keeps x / y for off-screen points so
a caller can clamp an edge arrow. `party::hud::MemberColour(slot)` gives each member a fixed colour
(host gold, then pale blue, green, violet); the tags draw names in it. Approach found viable: the
**overlay** (ImGui background draw list over the presented frame) - live-verified for the tags,
drawn after the game's frame, so it naturally shows through walls (no depth test; I2 wants that).
The game's own Scaleform HUD (lock-on / enemy HP bar markers) was not investigated here; its
phantom label exists (blue "Cooperator" + name above phantoms) but its draw path is native.
Caveats for I2: the camera matrix is trusted only within 15 m of the player (cutscene cameras),
and the 1-80 m range filter is the tags' choice, not the projection's.

Level, deaths and rot stacks travel in a VITALS trailer: after the entries, `u8 n`, then per entry
`u16 level, u16 deaths, u8 rot`. Older peers stop reading after the entries; a frame without the
trailer reads as zeros (unknown: the tag shows the name only). The guest's single-entry VITALS
must say count 1 now.

## 5. Level sync and phantom health (0.7.5, levelsync)

Rules `level_sync` / `phantom_hp` (host's, default on = vanilla; System > Party rules: Phantoms; ini
`party_level_sync` / `party_phantom_hp`; party_levelsync_rules.h).

**What the game does (Ghidra, 1.09).**
- A per-frame ChrIns step `0x15077c0` (it also keeps the phantom SpEffects: kind by invade type 7 -> 9006,
  8 -> 9026, 0x15 / 0x16 / 0x17 -> 9036 / 9046 / 9056) takes the chr's PlayerGameData (chr vfunc +0x1c8) and the
  first game-data record with `+0x10 == 1` (the host: the local record, the four session records stride
  0x6a0 of GameDataMan 0x553b130 +0x10, then the 40 pointers at +0x20). For a record of kind 2..0x7ffc (a
  non-host player) it tests `(float)host_level * mult + add < (float)level` and puts SpEffect **9200**
  (0x23f0, row 9200: iconId 8817 = the blue icon, no stat fields) on the chr or takes it off, caching the
  result at step +0x2c. `mult = pct / 100 + 1`, `add` = SprjNetworkClientMan (0x5540288) +0x18: +0x8e4 /
  +0x8e0 (the serializer 0x1eb65e0 defaults both to 20; the code's own fallback is 20 % / 20 too).
- The SpEffect list's rate getters multiply by `curve(host * mult + add) / curve(own level)` (0x1582940,
  CalcCorrectGraph rows, piecewise on levels) while 9200 is on: **max HP** 0x17e46e0 (row 120),
  **max stamina** 0x17e4a20 (124), **defence rates** 0x17e76b0 (121: physics / magic / fire / thunder) and
  the poison / blood resistances 0x17e8110 (122 / 123). Nothing else reads 9200 but the name plate
  (0x16439b0: the icon) and the party list (0x1bfb2a0). **There is no attack-power term**: a downscaled
  hunter hits as hard as before; the "damage penalty" is the defence rate (it takes more damage) plus the
  lower HP. It is inseparable from the sync (one SpEffect id), so it has no toggle of its own: "Level sync"
  covers it.
- Phantom health: SpEffect 9006 / 9026 carry only `maxHpRate 0.7` (every other field default).

**Before 0.7.5 in our party joins.** Records, kinds and levels are right on every peer (a guest of level 37
with a host of level 10: both games see 37 / 10, kinds 2 / 1), but `ClientMan +0x18` holds an unlimited
placeholder (+0 %, +1000 levels: there is no matching server), so nobody was ever above the range and 9200
never came on. And 9006 / 9026 were always rewritten to 1.0 (seamless_rules.cpp, since the A6 agent's
seamless rules: guests at full health, no reason recorded beyond Seamless-Co-op-like play).

**0.7.6 Full health in PvP** (pvphp): while the host's *Friendly fire / PvP* is on, the host rule `pvp_full_hp`
(default on) lifts the cut (`levelsync::PhantomCutOn`), and every peer keeps its own hunter's HP fraction across
the max change (`HpRatioKeeper`, `HpAtRatio`): see pvp.md "Full health in PvP".

**How it is done.** Level sync on: `LevelSyncTick` (party_balance.cpp, every 0.5 s on every peer, the host's
rule) writes the game's normal range (+20 % +20) into ClientMan +0x18 (the original placeholder is put back
when the rule goes off), and keeps 9200 on exactly the non-host players above `host * 1.2 + 20` (the game's
own step adds it; ours does when the step's cache says it already did and the effect is not there, e.g. after
the rule was off; a second 9200 is removed). Off: 9200 is removed. Phantom health: `ApplyPhantomHp`
(seamless_rules.cpp) writes 0.7 / 1.0 into rows 9006 / 9026 (rewritten after a param reload).

**Measured** (live 2026-10-24, two rendered instances, test save: base max HP 719 / stamina 100 solo; the
guest's own chr, `BB_PARTY_LEVELSYNC_LOG`, host rules flipped inside one session):

| host / guest level | level sync | phantom health | max HP | max stamina | defence rate |
|---|---|---|---|---|---|
| solo | - | - | 719 | 100 | 1.000 |
| 10 / 37 | off | off | 719 | 100 | |
| 10 / 37 | off | on | 503 (0.7) | 100 | |
| 10 / 37 | on | off | 677 | 98 | |
| 10 / 37 | on | on | 474 | 98 | |
| 1 / 100 | off | on | 503 | 100 | 1.000 |
| 1 / 100 | on | on | 245 | 82 | 0.208 |

The icon: with the game's own name plate (System > My party: Display > Party name tags off) the blue icon
sits left of "Co-operator Hunter1" with the sync on and is gone with it off (screenshots). Our own name tag
(default) replaces the game's plate and does not draw the icon.

## 6. Tests

- `party-balance-test`: vanilla rules reproduce rows 7500-7502, defaults, poise below zero, JSON
  round trip / clamping / malformed input, rot factors, slider tables.
- `party-levelsync-test` (0.7.5): the game's range test, the 9200 decisions for both rule values, phantom HP rates;
  `party-balance-test` also round-trips `level_sync` / `phantom_hp`.
- `party-link-test` (vitals): level / deaths / rot relayed guest -> host -> other guest.
- `game-menu-pages-test`: the new page keeps the menu's limits (12 pages = MaxPages).
- Live: see the ERSC_PARITY rows C4 / D7 / E2 for the runs.
