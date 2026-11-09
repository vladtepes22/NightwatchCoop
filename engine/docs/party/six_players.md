# Six players (CL-10e56b): limits, why 5 is the ceiling today, and the design for 6

Verdict: **5 seats (host + 4 remote, invaders included) is the engine's hard ceiling and we can give it to players safely
now. 6 seats is NOT a flag: it needs five fixed-size engine tables extended (about 60-100 patch sites, one of them a
struct layout change), which cannot be done blind. Design below; recommend it as its own item with a probe first.**

"Seat" = host + every remote player the game holds, a phantom or an invader. The game has 5 seats (host + 4 remote). Our
code called it "four game slots" (`kGameSlots = 4`, host + 3), which is why a 5-party can never take an invader today.

## 1. Limits table

| # | Limit | Where | Value | Whose | Evidence | For 5 | For 6 |
|---|---|---|---|---|---|---|---|
| 1 | `Capacity(party_max)` = min(max, 4) | party_invade_rules.h:33,166 | 4 | ours | source; the invader path (HELLO role 1) reaches the world by direct summon, no SOS filter | change 4 -> 5 (one constant) | 6 |
| 2 | `kMaxPartyPlayers` (link slots, VITALS, HUD rows, Party members page, BB_PARTY_MAX clamp) | party_link.h:58, party_fourp.h:44 | 5 | ours | one constant sizes them (four_players.md A7) | none | 6 + menu page (see 6) |
| 3 | Launcher offers 2-4; adopt keys 2-5; ini `party_max` | launcher.py:71,922 | 2-4 | ours | source | add 5 behind a toggle | add 6 |
| 4 | HELLO / WELCOME / admit | party_link.cpp:725,983,1803,1934 | `max_players` 2..kMax, slots 1..max-1 | ours | invaders take link slots too, so they count against max already | none | follows #2 |
| 5 | Matching2 emulation, host room cap, discovery / VPN lobby `max` | np_matching2.cpp:524 (clamp 8), party_host_service.cpp:25 (2..8), party_discovery.cpp:141 (<=8), nwlobby.py:173 (<=8) | 8 | ours | source | none | none |
| 6 | System menu Party members page: 6 visible rows | bbport_game_menu_pages.cpp:253,1253 | 6 rows | ours/game UI | page is at the game's 12-page limit | already shows 5 + 1 (Last event steps aside) | 6 members fill the page, no notices row |
| 7 | Net bandwidth / CPU | network_robustness.md:118,383 | ~3 KB/s per relayed guest, ~180 KB/min UDP per guest | measured | full mesh: 5 seats = 10 pairs, 6 = 15 (each player sends to 5 peers, still < 20 KB/s) | fine | fine; RAM per game unchanged (one PC each); one PC with 5 games = 20 GB WS (fp5 note), 6 games = stress, test only headless |
| 8 | SOS filter literal `coop+inv+other+outgoing < 4` | 0x1874bbe `cmp esi,4` | 4 -> 5 by H6 | game, **already patched** | four_players.md "Five players" | done (H6) | `cmp esi,5` -> 6 (1 byte) |
| 9 | Selector max coop / max inv | sel+0x1fc/0x200 set by ctor 0x1871259/64 (3/3) | 3/3 -> N by H5 | game, **already patched** | four_players.md H5 | done | H5 writes N (field already generic) |
| 10 | SessionSlotSummary 4 records (2 coop + 2 invader) | 0x186fe40 | 2+2 | game, H1 lifts coop | four_players.md #1 | done (H1 reads the game's own count, invaders rec2/rec3 give Max invaders 2) | H1 rule is generic; invader records stay 2 (Max invaders cap 2 holds) |
| 11 | **Presentation object**: 5 slots (stride 0x14 at +0x1c), tracked handles at +0x80..0x8c, tail fields to +0xa8, allocated 0xb0 by the flow ctor (`0x1567864`, ctor 0x15ba630) | 0x15bc590 (loop `4 < i`), 0x15bbc00 (`!= 5`), 0x15bce10 (`!= 0x80`), 0x15bc080, 0x15bcd30 ... about 20 functions 0x15ba630-0x15be047, 50 tail-offset sites in them, plus external readers of +0x90/+0x98/+0xa8 | **5** | game, **hard** | decompile 0x15bc590 / 0x15bbc00 / 0x15bce10 (in this repo's ghidra cache); bbhost `network_script_state.hpp` presentations[5] + tracked_chr_handles[4] at 0x80 | fits | extend to 6: alloc 0xb0 -> 0xc4, shift every tail access +0x14, loop bounds 5 -> 6. Layout change; also the object is serialised in the world-transition snapshot (every peer patched alike) |
| 12 | **Player ChrSet**: capacity 5 (local + 4), alloc 0x118 = 5 x 0x38 | init 0x18da8d0; free-slot scan in 0x18db500 | **5** | game, hard | bbhost `world_chr_man.hpp` (`NATIVE_PLAYER_CHR_SET_CAPACITY = 5`, `ALLOCATION_SIZE = 0x118`) | fits | alloc 0x118 -> 0x150, count 5 -> 6 (few sites, plus the handle table is 0x3e entries, ok) |
| 13 | **GameDataMan remote rows**: 4 rows x 0x6a0, occupancy bytes at `*(GDM+0x18)`, `slot_flags[4]` inline at GDM+0xa9, pointer table of 0x28 session records | rows alloc 0x1a90; readers in 0x178b5e0 case 0xe (slot pick), 0x1e50090 (`slot < 4`), 0x18d8100 alloc, 0x17e46e0 level sync scans all 4 rows, reset / cleanup loops, `SprjSessionMemberSlot::MAX_PLAYER_SLOTS = 4` | **4 remote** | game, hard | bbhost `game_data_man.hpp` (`PLAYER_SLOT_COUNT = 4`), `session_manager.hpp:250`, `world_session_object_man.hpp:131`; decompile 0x1e50090 `uVar2 < 4` | fits (host + 4) | rows alloc + `slot_flags` (inline in a struct: layout change) + every `< 4` check |
| 14 | Matching2 room max the game itself passes (`room+0xc`, from our emulation, literal 5 in the game's room create) | 0x1788ed0 -> 0x1ecfcd0 | 5 | game value, our emulation answers it | four_players.md #6 | ok | our Matching2 reply can say 6; the game's `slots+0x14 < room max` checks then pass |
| 15 | Boss scaling rows 7500..7503 (1.5/2.0/2.5/3.0) | H2-H4 | 3 rows | game data | four_players.md section 4 | H2 clamps 4 cooperators to the 7502 row (fp5 log) | 5 cooperators would map to 7503 (3.0x) - balance call |
| 16 | NPC sign / maiden EMEVD counts (`3[29]` `<2` / `<3`) | E6 + bbhost maiden rewrite | coop < 3, inv < 2 | game data, ours rewrites | four_players.md #10 | ok | counts are `<N` thresholds, generic |
| 17 | Unknown per-player arrays (name-plate pool `FrpgMenuDlgFloatingPCGauge`, per-peer net buffers 0x63F8, Lua `GetWhiteGhostCount`, UI member lists) | not mapped | >= 5 proven by fp5 run | game, unknown for 6 | fp5 smoke | proven | **not proven** - only a probe can show |

Ours-side total for 5 with invaders: item 1 (one constant) plus the launcher choice. For 6: items 2, 3, 6 are small.
Game-side for 6: items 8, 11, 12, 13 (+14 free, 17 unknown).

## 2. Why 6 is not "one more patch"

The engine was built for host + 4 remote and every table that matters is a fixed array whose size is baked in as an
allocation size, a loop bound and a literal compare, with other fields placed right behind it (item 11's tail fields start at
the end of slot 4; item 13's `slot_flags` sits inline in GameDataMan). A 6th remote player (seat 6) therefore hits, in order:
(a) the presentation add (0x15bc590) finds no free slot and returns 0, (b) the player ChrSet has no free index, (c) the
GameDataMan row pick (`slot < 4`) fails. Each is a silent miss, not a crash, until a table is extended half way, and a half
extension corrupts the neighbouring field. I did not patch any of them blind.

## 3. Proposed design for 6 (new item, needs its own agent and a probe)

1. **Probe first (no game files changed)**: BB_PARTY_SIX_PROBE logs, with 5 seats full, what the game does when a 6th member
   joins the room (Matching2 maxSlot 6): which of (a)(b)(c) fires and the callers. One 6-instance run, headless.
2. **Relocate, do not extend in place.** For items 12 and 13 allocate the bigger arrays ourselves in a hook on their
   constructors (alloc size and count are immediates in 0x18da8d0 and the GameDataMan init) and patch the compare literals
   (`cmp ..., 4/5`) found by a capstone scan of exactly the functions named above (the scan list is the work product:
   `tools/re` script, verified byte by byte like the other `PatchBytes`).
3. **Item 11 (layout)**: allocate 0xc4, keep slots 0-4 where they are, put slot 5 at +0x80 (the old tail start) and move the
   tail fields +0x14; patch the ~50 tail-offset sites in the class's functions with a table-driven patcher (old disp -> new
   disp, instruction by instruction, refusing to run if any decode differs). External readers of +0x90/+0x98/+0xa8: found
   by scanning for those displacements next to the object pointer load (`*(flow+0x16f8)`, ~290 byte matches to triage).
4. Every peer must have the same layout (add the set to the party-patch hash, as H1-H6), all behind `BB_PARTY_LARGE` (off).
5. Prove in order: ChrSet + rows with 5 seats (no behaviour change), then the 6th seat, then boss fog and travel.

Risk: high (memory layout of live engine objects, 20+ functions, unknown readers); effort estimate 3-5 agent days; do it
only after the probe shows (a)(b)(c) are the only misses.

## 4. What shipped (0.8.0)

- `Capacity` = min(Max players, 5 seats): a 5-party takes an invader (host + 3 guests + 1 invader); a direct summon counts
  invaders against the seats (party_b2 dsummon HostView).
- Toggle `BB_PARTY_LARGE` / launcher *Large parties* (off by default): without it Max players is held at 4 locally; with it 5.
  A guest always takes the host's value up to 5 (host-owned).
- Smoke `fp5` (with the toggle) and `fpi5` (host + 3 guests + invader); unit tests `party-invade-test`, `party-link-test`.
- 6 stays a documented, designed follow-up (`kMaxPartyPlayers` stays 5; nothing offers 6).

## 5. Proof (0.8.0, six080, 2026-11-07, loopback, 5 games: host + one guest drawn, three headless-lite)

- `fpi5`: three guests in (`cooperators 2 -> 3`), the invader (instance 4) dials at 150 s and is admitted into slot 4 with
  five seats full, told to every guest, never refused for room; it falls 25 s after it is in and goes home (host
  `invasion of slot 4 ends: invader_died`). 19 steps pass. Host and guest screenshots show five hunters, the invader red, four HUD rows.
- `fp5` with `BB_PARTY_LARGE=1`: four title joins, party lamp travel (all four guests in m24_01), Cleric Beast fight (H2 clamp), a
  guest's death and arena revive, boss kill, Leave and Rejoin (4 -> 3 -> 4). 26 steps pass; the drawn windows ran 9-10 FPS (five
  games on one PC, a stress test). Harness fix: fp5's `until` stopped at the travel's new session (`0 -> 3 -> 4`) before the fight.
- Not run: six players (not possible, see above), two invaders into a 5-party (needs a 6th game; unit-tested only).

## 6. Probe and census for 6 seats (six080, round 2, static; no game run)

`python tools/re/six_census.py` (capstone over the 1.09 eboot, addresses only) lists every function that touches the fixed tables.
What it and the decompiles (ghidra cache) show:

| Table | How it is built (evidence) | Sites a 6th remote player touches |
|---|---|---|
| Player ChrSet | ctor 0x18da8d0 allocates 0x118 = 5 x 0x38 and writes count 5; the free-slot scan 0x18db500 reads the count (`*param_1 <= i`), so no other literal | 1 hook (post-ctor: allocate 6 x 0x38 zeroed, count 6). Cheap, safe. |
| GameDataMan remote rows | init 0x14e4e80: `mov esi,0x1a90` at 0x14e5561 (0x10 + 4 x 0x6a0), four unrolled `PlayerGameData` ctor calls (0x14f1510), `[rbx+8] = 4` at 0x14e55c5, occupancy bytes: 4-byte allocation at GDM+0x18 (0x14e55db) | allocation size, count, 5th ctor and occupancy (post-init hook is enough: the rows are fresh) PLUS about 25 loops / checks `< 4` / `3 <` over the rows: 0x131d470, 0x131ddc0 (two), 0x1336260 (two), 0x133aa20, 0x13835f0 (two), 0x13cda50, 0x15077c0, 0x1783160 (three), 0x178b2c0, 0x178b5e0 (two + the slot pick loop at the end of case 0xe), 0x17e46e0 / 0x17e4880 / 0x17e4a20 / 0x17e76b0 / 0x17e8110 (level sync and friends), 0x1906480, 0x18d8100, 0x1e50090 (`uVar2 < 4`), 0x1710a30 (a 0x1a80 byte loop). `GameDataManPlayerSlotFlags[4]` sits inline at GDM+0xa9 (bbhost game_data_man.hpp): a 5th flag would overwrite the next field (needs a relocation or a side table). 46 functions use the 0x6a0 stride in total |
| Presentation object | allocated 0xb0 at 0x1567864 (flow +0x16f8), ctor 0x15ba630; 5 slots at +0x1c (stride 0x14), tracked handles +0x80..0x8c, state / count fields +0x90, +0x94, +0x98, +0x9c, +0xa0, +0xa8 | the class's own ~20 functions (0x15ba630-0x15be047) plus **104 functions that load the object** (census), about 40 of them read the tail field +0x98 ("is presented"), `+0x14` is read in about 50. A 6th slot moves the whole tail by 0x14: every one of those displacements must change in place (same length: disp32), each verified as this object and not another with a field at the same offset |
| Matching2 room | answered by our emulation (maxSlot) | none |
| SOS filter / selector | H5 / H6 | one more byte each (H6 `cmp esi,5` -> 6) |

So the 6-seat patch set is: ChrSet (1), GameDataMan (about 30), presentation (about 70 including the displacement moves),
filter (1), plus whatever the first 6-game run shows among the arrays this census cannot see (name plates, per-peer buffers).
About 100 byte-checked sites. Design: one table-driven patcher (`party_sixp.{h,cpp}`, rules tag `6p:vN`, all behind
`BB_PARTY_LARGE=1` and Max players 6; nothing patched otherwise): a site is (address, expected bytes, new bytes, owner function), applied
with `Matches()` first and refused as a whole if any site differs; the displacement moves are generated by `six_census.py` and
reviewed per function against the decompile, never applied by pattern alone. Stages, each proven with `fp5` / `fpi5` before the next:
(1) ChrSet + room max + filter literals (still 5 players, nothing visible), (2) GameDataMan rows and their `< 4` sites (5 players:
same), (3) presentation tail move (5 players: same), (4) the 6th seat: `fp6` and `fpi6`.

## 7. Where round 2 stopped, and why (read before scheduling the work)

Stages 1-4 are mechanical but about 100 sites across 150 functions, with no way to prove a site except a run with the table full.
A wrong displacement or bound is not a refusal: it is memory corruption in the game's session objects (a neighbouring field
overwritten, an out-of-range row read), and the symptoms show only at the 6th join. A 6-game run needs about 24 GB of working
set (fp5: five games held about 20 GB on this 32 GB PC and drove free memory to 0.4 GB, drawn windows at 9-10 FPS); a sixth game
means paging, so every iteration is long and noisy. I stopped before patching anything: nothing in the repo changes behaviour for 6,
the launcher offers 5 at most, and 5 stays as shipped. Recommended next step: stage 1 and 2 together as one agent day with a
read-only runtime probe first (log the ChrSet count, the GameDataMan row count and the occupancy bytes at the title, to confirm the
offsets above on a live game), then a decision on the presentation tail (70 sites) against its value (a 6th player).

## 8. Round 3: the 6-seat patch set (six080, 2026-11-07)

`gpu/shim/party/party_sixp.{h,cpp}` (+ `party_sixp_sites2.inc`, `party_sixp_sites3.inc`, generated by the census scans). Off unless
`BB_PARTY_LARGE=2` and `BB_PARTY_MAX=6` at start (the launcher sets them for Max players 6 under *Large parties*, and for a join with an
adopted host value 6); rules tag `6p:v1` and the hook list are part of the party version check, so every peer of a 6-player party runs the
same set. `BB_PARTY_SIX_STAGE` (1-3, default 3) limits the stages for proofs; `BB_PARTY_SIX_PROBE=1` logs the tables every 10 s
(`Party 6p: probe: ...`). A set is applied whole: every site is compared with the 1.09 bytes (or the patched ones) first, one
mismatch leaves the game at 5 seats; nothing is patched with the switch off.

| Stage | What | Sites | Proof (5 players with the set booted: `fp5x`, `fpi5x`) |
|---|---|---|---|
| 0 probe | read-only log at runtime | - | 1.09 offsets confirmed live: ChrSet count at WorldChrMan+0x30 (5 vanilla, 6 patched), GameDataMan row count at rows-8 (4 vanilla), occupancy bytes at GDM+0x18, presentation members at obj+0x14 |
| 1 | player ChrSet 5 -> 6 (alloc 0x118 -> 0x150, count, memset size), SOS filter literal 4 -> 6 (H6), Matching2 room 6 (host service and the room the game is told), placement spot 5 | 3 + 1 | 45/45 steps (commit d00396bb) |
| 2 | GameDataMan remote rows 4 -> 5: init alloc 0x1a90 -> 0x2130 and count, a post-init hook builds row 4 (`0x14f1510`) and an 8-byte occupancy array, 73 loop / index bounds (`cmp reg, 4` / `3` -> `5` / `4`) in 40 functions, two `0x1a80` byte loops -> `0x2120` | 77 + 1 hook | 44/45 (the one miss was a log regexp: the boss kill ran by its fallback path), commit 159460f4 |
| 3 | presentation object 5 -> 6 slots: slot 5 sits at +0x80 over the old tracked-handle area (0x80..0x93, exactly 0x14 bytes), the four tracked handles and their count move to +0xb0..+0xc3 (object 0xb0 -> 0xc8); 45 displacement moves in 0x15bcd30, 0x15bed10, 0x1e4b830, 0x15bad90; 25 slot-loop bounds; hooks: ctor (init slot 5 and tracked), reset call site 0x1942312 (reset slot 5 and tracked), release 0x15bc080 (slot 5 -> slot 4 compaction, which the game's unrolled blocks lack) | 71 + 3 hooks | 45/45, 151 sites in all, commit 4c1ab925 |

Why stage 3 is far smaller than the census feared: the tail fields after the handles (+0x94 .. +0xa8, read by about 40 functions)
do not move at all, because the 6th slot fits exactly over the four handles and their count. Only the code that touches those
five fields (four functions) and the slot loops (class functions 0x15ba630-0x15bf4xx, plus 0x1879e60) changed.

Known limits of the set (not hit in any run yet): the game's unrolled compaction in the release functions covers slots 0-3 (scan bounds
4 -> 5 here, slot 5 -> 4 by our hook); the "reset all" of GameDataMan (0x178b5e0, rows 0-2 only) leaves rows 3 and 4 to the per-slot leave
path, as vanilla did for row 3; 0x178ab53 (a 4-iteration loop over `+0x5c0` data) and the `< 5` compares in 0x17692f0, 0x1872360,
0x17b21a0 and 0x1506820 were read and left alone (type switches, not seat counts).

## 9. Running 6 games

Six games need about 24 GB of working set; free memory under 1 GB must abort the run, not page. `tools/mp/ramwatch.py --root <instance root>`
kills the test games under that root when the free physical memory falls below `--floor-mb` (default 1024) and exits 3. The shared
queue (`agents/game_queue.json`) must allow 6 slots for a solo run: `"max_solo": 6` (a 5-slot cap refuses `--count 6` forever); the
coordinator owns that file. Then, from the agent's clone, with everything else on the PC closed:

    python tools/mp/ramwatch.py --root E:\games\bbport\agents\NAME\mp --log ramwatch.log
    python tools/mp/smoke_party.py --root E:/games/bbport/agents/NAME/mp --build E:/games/bbport/agents/NAME/repo --game E:/games/bbport/agents/NAME/game --out E:/games/bbport/agents/NAME/smoke_out/six --phase fp6,fpi6

(the first command in a second window or in the background). `fp6`: host + 5 guests (2 drawn, 4 headless-lite), joins to 5 cooperators,
lamp travel, Cleric Beast fog fight (H2 clamps to 7502), a death and arena revive, the kill, a Leave and Rejoin (5 -> 4 -> 5).
`fpi6`: host + 4 guests + an invader = 6 seats.

## 10. Status (rewritten after round 4, see section 12)

The patch set boots and runs with all three stages (149 sites, 4 hooks) plus the fixes of section 12. Proven so far with 6 seats: `fpi6` (host + 4 guests +
an invader, 20/20, screenshots read) and the join, the full presentation and the Leave of `fp6j` (6 players). Not proven: a 6-player Rejoin of a middle
member (section 12) and the travel / boss part of `fp6` (the PC runs out of memory when six games load a map together). The launcher does not offer 6
until both are closed (`PARTY_SIX_PROVEN = False` in launcher.py; `BB_PARTY_LARGE=2` with `BB_PARTY_MAX=6` by hand works).

## 11. fp6 attempts (2026-11-07): the 6th guest crashes while entering the host's world

Six runs of `fp6` (6 games, 2 drawn, ramwatch: lowest free memory 3.6 GB, never under the floor). The host and guests 1-4 join and run;
**the 6th game (instance 5, the 5th guest) crashes 35-49 s in, at the moment its world appears with five remote chrs, every run**
(`Guest fault`, DEP-execute at 0x5399f90 or a null call at 0x1bf9986; the harness' own restart of the guest then leaves a game running,
kill it by hand). Evidence:

- Crash site: `FUN_01bf7c20` (menu update), the dispatch of the menu event queue: `movsxd r14,[menu + 0x15c8 + read*4]` at 0x1bf993e, then
  `call [handler table 0x298 + r14*0x58]` at 0x1bf9986 (18 entries, ids 0..17). `r14` was 503 (0x1f7), 503, 100: not an event id.
- A value filter on that load (`BB_PARTY_SIX_PROBE=1`, `MenuIdFilter`) shows the queue really holds 503 at slot 0 (read 0, write 1) while every
  producer of the queue (0x13813d0, 0x13835f0, 0x17c37a0, 0x203ffd0) range-checks its id to 1..17. So something writes a HP value into it: 503 is a
  remote chr's HP, 719 its max HP (`remote chrs: ... hp 503` in the same log).
- The dump of the 0x200 bytes before the queue (`MENU ID context`) is a table of 0x98-byte records (the enemy-gauge table `MenuMan+0x10f8 + i*0x98`,
  8 entries, filled by 0x1644cf0 / 0x1654d80, read by the gauge menu 0x1bfb2a0, all of them bounded `< 8`); its *9th* record starts at +0x1598 and
  already holds max HP 719 and HP 503, 503 at +0x15c8 = the queue's first int. So a writer fills a gauge record past the 8th, into the event queue.
- The 5-player game (fp5x, fpi5x, all stages) never gets there: five remote chrs fit the table. With a 6th chr there are more chrs wanting a
  gauge than the table has entries (6 players + the boss and others), and one writer is not bounded the way 0x1644cf0 / 0x1654d80 are. I did
  not find that writer (no indexed access to disp 0x1108-0x112c exists, so it works through a computed pointer).
- Not the sixp sites: the writer stores values of a chr, the sites change counts and offsets of other tables; nothing of the stages writes 503.

What is left (next step for whoever continues): find the unbounded writer of the gauge records (watch +0x15b8 / +0x15c8 of the menu object: a
VEH hardware watchpoint on the first int of the queue, or the stores of `hp` / `maxhp` of a PlayerIns into `[rax + 0x20/0x24]` of a computed
record pointer), then either bound it or enlarge the 8-entry table (a relocation like stage 3: base 0x10f8, stride 0x98, 8 -> 10 entries,
about 15 sites in 0x1644cf0, 0x1654d80, 0x1bfb2a0 and the writer). `PARTY_SIX_PROVEN` stays False.

## 12. Round 4 (2026-11-08): the gauge-array overflow, the room size, the invader, the rejoin

Found with the write watchpoint (`BB_PARTY_SIX_PROBE=1`: debug registers on `menu+0x15c8` / `+0x15b8` in every thread, a vectored handler that logs rip and
the guest stack) in one fp6 run:

- **The overflow** is `FUN_017692f0` (a per-GameDataMan-row HUD update): `mov [menu + 0x15b8 + row*4], hp` at 0x1769cb2, a 4-int last-HP array that sits
  directly in front of the menu event queue (+0x15c8). Row 4 (the 5th remote player) writes the HP into the queue head, and the menu update then
  dispatches the id 503 / 100 (the crash of section 11). Fix: the stage 2 loop bounds of that function (0x1769b60, 0x1769cc3) are NOT patched, rows 0-3 only
  (the 5th remote has no HP-change display). Result: the 6th guest no longer crashes.
- **The room size** the game keeps in `*(0x553d6d0)+0xc` is 5 whatever our Matching2 answer says; 0x15bc590 / 0x1e50090 refuse a member when the
  presentation count reached it (the host counted 4 cooperators with 5 guests in the room). `SixpTick` keeps that field at 6 while the set is on.
- **The invader** (`fpi6`): `Capacity()` counted 5 seats (now 6, Max players decides), and the direct summon counted the asking invader against the
  cooperator cap (4 + itself = 5 = full), so it fell back to a sign that stalled 120 s. The asking slot is skipped now.
- **Harness**: a game the party launcher restarts after a crash was left running (instances.py now ends everything under the root at teardown), and
  smoke_party.py's `Logs` / `starts` handled 5 instances (now 8).
- **Memory**: free memory fell under 1 GB (500, 817 and 175 MB) at the Cleric Beast's map load in three `fp6` runs (six games loading together, all
  headless but the host, `BB_TEXTURE_QUALITY=quarter`): ramwatch killed them, which the harness reports as an "UNPLANNED CRASH" of instance 0. So `fp6`
  with the travel cannot run on this 32 GB PC; `fp6j` is the same party without the travel.

Results (this PC, 6 games, host drawn):

| Phase | Result |
|---|---|
| `fpi6` | 20/20: 4 guests in, the invader admitted into slot 5, in the world, falls (`invader_died`); the host's window shows six hunters (the invader red) and five party rows |
| `fp6j` | 23/24: 6 players join (`cooperators 4 -> 5`, presentation 6 slots, room max 6, every game draws every chr), guest 3 Leaves (5 -> 4; the game's compaction plus our slot 5 -> 4 hook ran: `presentation: slot 5 moved to slot 4`). **The Rejoin fails**: the direct summon joins the room (member 7) and the guest loads into the host's world, but the host's member task stays at `FirstSyncCompleteWait` (data request 0xA repeated 20+ times), so `cooperators` never returns to 5 |
| `fp6k` | 25/25: the same 6 players, but the LAST guest (slot 5) Leaves and Rejoins: `cooperators 5 -> 4 -> 5`, every game draws every chr, no crash. So the 6-seat Rejoin works when no slot has to be compacted |
| `fp5x` + `fpi5x` (final set, 5 players with the 6-seat set booted) | 45/45 |
| `fp6` (travel, boss) | not completed: memory floor |

Open: the Rejoin of a MIDDLE member at 6 seats (a Leave of slot 3 compacts slots 4 and 5 down: the game's own block plus our slot 5 -> 4 move; the new member then stays at `FirstSyncCompleteWait`; the last member's Rejoin, `fp6k`, is fine, so it is the compaction or the member ids that follow it), and the travel on a PC
with enough memory. `PARTY_SIX_PROVEN` stays False.

## 13. Round 6 (2026-11-08): the middle-member Rejoin

Root cause: **one missed row bound.** A leaving member's row is freed in `0x178b5e0` (`cmp eax, 3; ja` at 0x178d2b9, then row id -1, occupancy byte
0, slot flags 0): that compare was not in the stage 2 list, because a `mov` sits between the `cmp` and the `ja` and the first scan only took a `cmp`
followed directly by a jump. So at 6 seats a leaver on row 4 freed nothing (the `fp6k` last-guest Rejoin worked because the guest kept its own number),
and a leaver on a middle row stayed marked used: after the leave all five occupancy bytes still read 0x1f, the host found no free row for the
rejoiner (it sent player number 7 instead of 4: `packet 7 from the host gives 7`) and the new member's first sync never completed
(`FirstSyncCompleteWait`, data request 0xA repeated). Evidence: the probe now prints the 8 occupancy bytes (`occupancy 0x0000001f1f1f1f1f` before and after
the Leave). A/B: with our slot 5 -> 4 compaction hook off (`BB_PARTY_SIX_NOCOMPACT=1`, a test aid) the Rejoin failed the same way, so the hook was not
the cause. Fix: site 0x178d2b9 `83 f8 03` -> `83 f8 04` in `Stage2Sites` (78 compare sites now; a rescan with up to three instructions between `cmp`
and the jump finds no other).

5 players have no such gap: rows 0-3 are all inside the vanilla guard, and `fp5x` (guest 3 of 4 Leaves and Rejoins, the player number comes
back as 4) passes with the 6-seat set booted and in vanilla `fp5` since 0.7.8.

Results: `fp6j` and `fp6k` 50/50 steps (6 players join, guest 3 resp. guest 5 Leaves and Rejoins, 5 -> 4 -> 5), `fp5x` see below.
Still not run: `fp6` (travel, boss; six games loading a map need more RAM than this PC has free).

## 14. Round 7: the 6-player proof and the launcher (2026-11-08)

User decision: `fpi6` + `fp6j` + `fp6k` are the 6-player proof. `fpi6` rerun on the final set (the 0x178d2b9 site in, master 427a5f38 merged): 20/20.
The launcher offers Max players 6 under *Large parties* (labelled experimental, off by default; `PARTY_SIX_PROVEN = True`).
**Not tested: `fp6`** (the 6-player lamp travel, the Cleric Beast fight with six, a death and revive): six games loading one map need more
memory than this 32 GB PC has free (three runs fell under 1 GB free), so it is a manual playtest item on real PCs.
