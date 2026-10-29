# Campaign soak: new game to end credits with a party (2 players)

Live soak of the whole campaign path with the in-game party flow, 2026-10-11 (edgetravel,
integration `windows-upstream-0.5` @ 25348ea .. 0bf258b). Two loopback instances
(`tools/mp/instances.py run --count 2 --party`), **fresh characters** for both (empty
`user\savedata_party`), and from then on the **same saves for every run**: each run starts from
the title with *Continue party game* (auto-continue), so every run after the first is also a
"Continue resumes the same world on both sides" check. No OS input: the director's test steps
(`BB_PARTY_DIRECTOR_TEST`, party_director.cpp) and the test aids of test_newgame.h.

Runs are short (the machine-wide game queue ends a run after 240 s when others wait), so the
soak is a chain of runs 1, 2, 3, 4, 4b, 5, 5b-5d, 6-6d over the same two saves.

Evidence is the `Party progress: test read (load mode N, own save / visible)` line both
instances log at every steady load (`BB_PARTY_PROGRESS_TEST_READ`): `own` is what the player's
**own save** holds, `visible` what its game sees now (the host's overlay while summoned).

## Checklist

| # | Milestone | How (test steps) | Session survives | Same map | Progress in the guest's own save | Crash-free | Result |
|---|---|---|---|---|---|---|---|
| 0 | Integration HEAD healthy | merge + build + all party-*-test + party-security-test | - | - | - | - | **pass** (18/18) |
| 1 | New party game, fresh characters, clinic -> Dream -> Central Yharnam | `BB_TEST_NEWGAME=1` both; host `BB_TEST_PROLOGUE_SKIP=1 WARP=dream`; guest catch-up; `host_warp=2412951` | yes | yes (after the host's warp) | campaign-start flags + weapons (catch-up) | yes | **pass after fix 1** |
| 2 | Cleric Beast | `host_warp=2412952`, `host_kill=2410800` | yes (K10 kept the party) | m24_01 | 12411700 = 1/1, Insight +3 (parity), item grant | yes | **pass** |
| 3 | Father Gascoigne | `host_warp=2412953` (Tomb of Oedon), `host_kill=2410810` | yes | m24_01 | 12411800 = 1/1 | yes | **pass** |
| 4 | Cathedral Ward | `host_warp=2402950` | yes | m24_00 | - | yes | **pass** |
| 5 | Vicar Amelia | `host_warp=2402951`, `host_kill=2400800` | yes | m24_00 | 12401800 = 1/1, grant | yes | **pass**; the skull (event 12401803, action on object 2401801) by `host_walk=74.79:37.9:351.33:3.14` + `host_press=0x4000`: cutscene 24000030 on host and guest, the host's time of day changed (row 5b) |
| 5b | Evening -> night (Amelia's skull) | runs 9c / 9d (late save line): `host_walk` to the skull, `host_press`, then a host lamp warp 120 s later | yes | m24_00 | host's own save 12401803 = 1/1 after the warp (9d); host time of day changed (9802 1 -> 0); guest: the scene once (no own replay) | yes | **pass**: cutscene 24000030 on host and guest (82 s, live mirror), re-join 0.1 s after the host's warp (9d). In 9c a warp 75 s after the press cut the scene's end and the flag stayed 0 (test timing). Screenshots: 9c only (both windows mid-scene, same frame); 9d's screenshot watcher was killed by another agent's cleanup |
| 6 | Forbidden Woods | `host_warp=2702950` | yes | m27 | - | yes | **pass** |
| 7 | Rom | `host_warp=3202951`, `host_kill=3200800` | yes | m32 | 13201800 = 1/1 | yes | **pass** |
| 7b | Blood Moon (Rom -> Queen Yharnam) | `host_walk` 4 m from Queen Yharnam (MSB 3200801 at (-510.9, -271.3, 370.4)) | yes | host's event warp to m28, guest followed (B2) | cutscene 32000000 on both (live mirror); time of day 3 written to the guest's own pool (9800..9802 0 -> 7) | yes | **partial**: the tod write did not survive into the next session (see open 2) |
| 8 | Nightmare Frontier | `host_warp=3302950` | yes | m33 | - | yes | **pass** |
| 9 | Micolash / Mergo's Wet Nurse | lamp 2602953 (`host_warp`), `host_walk=146.46:1124.6:-37.9:1.57/147.6:1124.6:-28.4:1.57` into region 2602805 (the arena-entry event 12601802), then `host_kill=2600802` x2 (the HP holder), `host_kill=2600801`, `host_kill=2600800`; Micolash by `host_flag=12601850` | yes (K10) | m26 | 12601800 boss defeat = 1/1 on the guest, Insight to the guest | yes | **pass** (run 5f): entry cutscene 26000010 on the host and the guest (live mirror), then `host boss kill: event 12601800` |
| 10 | Gehrman | `host_flag=72100131` (the refusal choice), `host_skipremo`, `host_kill=2100800` | yes (K10) | m21 | 12101800 boss defeat, Insight to the guest | yes | **pass**; refusal cutscene 21000040 on both (live mirror, 108 s) |
| 11 | Ending B (Honoring Wishes) + credits, guest gets them | `host_walk` to the Gehrman arena (MSB point 2102808 at (28.4, -9.2, 5.6), on hit parts 2103601-5) | until the ending (the guest leaves to play it in its own Dream, by design) | m21 | guest's own save: 12101800, 9180, 6604 / 6601 / 6603, **22**, NG cycle 0 -> 1 | yes | **pass**: host event 12100000 -> remo 21000020 (72 s) -> NG cycle 1 -> ending step (world down = staff roll); guest got `ending 2` at the same second, warped to its own Dream, same remo 72 s, flags, NG cycle 1, ending step |
| 13 | A fresh guest joins the advanced host late (run 7: host save from before the ending, guest `BB_TEST_NEWGAME`) | catch-up, gather, summon, `host_warp=2412952` | yes | yes | after the gather load (own world, m21): 12401800, 13201800, 12601850, 12101800 = 1; after the summon + travel to m24_01: 12411700, 12411800 = 1/1 (the current map's boss flags land at that map's load); **time of day 9800..9802 = 0/1, 13201803 = 0/1** | yes | **partial**: every boss caught up into the own save; the Blood Moon did not (open 2b) |
| 13b | Blood Moon catch-up (fix 3) | run 7c: the same late guest (own 9800..9802 = 0) summoned again | yes | yes | `time of day behind the host's: own save 9800..9802 0 -> 7 (host 7)`, save requested; reads 1/1 | yes | **pass**  |
| 13c | The caught-up time of day survives a restart | run 7d: Continue on both | yes | yes | guest's first own-world load (mode 0): 9800..9802 = 1/1 | yes | **pass** |
| 14 | 3 players (base save copies, `BB_PARTY_MAX=3`) | `host_warp=2412952`, `host_warp=2402951`, `host_kill=2400800` (already dead in that save), `host_warp=2412951` | yes, both guests clients throughout | yes: each warp re-joined both guests (`re-join done` on both, host cooperators back) | boss flags 1/1 on both guests | yes | **pass** (travel chain; no fresh 3-player campaign) |
| 1c | Gather to the host's real map (fix 4) | both fresh again, host's prologue ends in the Dream | yes | **yes**: `gather at the host's lamp: lamp warp to 2102950` (a Dream lamp; before: 2412959 in m24_01), summoned into m21 | - | yes | **pass** |
| 12 | Continue party game resumes the same world | every run 2..6d | yes | yes | yes | yes | **pass** (host back where it saved; guest re-summoned, 20-110 s after start) |

User design decisions (ERSC_PARITY.md "Design decisions"), per milestone:

| Decision | Result |
|---|---|
| A guest behind the host catches up fully in its own save (bosses, Blood Moon, lamps, shortcuts) | **partial**: bosses **pass** (rows 2, 3, 5, 7, 9 while summoned; row 13 for a fresh guest joining late: all six boss flags in its own save); campaign start **pass** (rows 1, 13); Blood Moon **pass after fix 3** (row 13b; before: row 7b not kept, row 13 a late joiner's own time of day stayed 0); lamps / shortcuts not measured in this soak |
| NPC quest choices are shared (anyone's first choice counts) | **partial**: a guest's NPC talk choices (`set 72400310` / `72400520 (npc_quest)`, Cathedral Ward NPCs 2400700 / 2400900) reached the host's world live (`host flag ... (npc_quest)`) and the guest's own save (Continue: 72400520 = 1/1 on the guest). The host's Continue read them 0: the host never requested a save after applying them (fix 7); the Continue re-check after fix 7 was not run before the 0.5.2 wrap-up |
| Cutscenes play for everyone | **pass**: Blood Moon 32000000, Gehrman's refusal 21000040, Mergo's arena entry 26000010 and Amelia's skull (evening -> night, 24000030, 77 s) played on the host and on the summoned guest (live mirror); screenshots of both windows mid-scene show the same frame |

## Fixes made during the soak (continued below)

6. **Cutscene played twice on a client** (open 4): the host-world copy of the event also runs on the
   client and played its own 2002 scene 0.1 s after the live mirror (run 5f, Mergo 26000010).
   party_story remembers mirrored events; party_b2 skips a client's 2002[1..7] of such an event
   for 10 min. Opt-in for 0.5.2; **on by default since 0.6.1** (`BB_PARTY_STORY_DEDUPE=0` turns it off; verified live 2026-10-13 with the Gascoigne intro, ERSC_PARITY D10).
   In run 9c (skull, without the switch) the guest saw the scene once.
7. **The host's world change by a guest was not saved by the host** (NPC quest row): party_worldact
   now requests a save on the host after it applied a guest's world action.

## Fixes made during the soak

1. **A party started from two new games never summoned** (row 1). The guest connected while the
   host was still in its prologue; the late-joiner catch-up read "the host is in its prologue:
   nothing to catch up to" from the host's first flag snapshot and the guest waited in its first
   Dream for good (the prologue flags are not in the change stream). The host now re-sends its
   snapshot when 12410000 && 9401 turn on (party_progress_bridge.cpp). Live: catch-up, gather,
   summon 27 s after the host's prologue, then the host's warp to Central Yharnam with the guest
   following (B2, 14.7 s).
3. **Blood Moon catch-up** (open 2 / 2b): party_catchup `TimeOfDayCatchup` - while summoned the
   game's own view of 9800..9802 is the host's overlay; when it is ahead of the guest's own save the
   value goes into the own pool (only upward) with a save request. The C2 snapshot does not carry
   these flags (checked: no line in run 7b), so the summoned view is the source.
4. **Gather to the map the host stands in** (open 1): `GatherLampFor` (party_travel, tested) keeps
   the host's last lamp when it is in the host's map, else a ReturnPointParam row of that map;
   the director's gather and the late-joiner catch-up use it (row 1c).
5. Test steps added (party_director.cpp): `%MAP` on any step (steady-world clock, so warps and
   kills chain without re-joins), `host_walk=chrENTITY`, `host_flag=ID` (a talk-menu choice),
   `host_skipremo` / `guest_skipremo` (the game's own cutscene skip request), `host_kill` logs
   whether the entity is in the character table.

## Open (not fixed here)

1. **(Fixed by fix 4)** **Gather target for a fresh host** (row 1): the guest's gather warp used the host's "last lamp"
   2412959 (the record a fresh character has: m24_01) while the host stood in the Dream, so the
   guest joined the host's world in m24_01 and the two were split until the host's next travel.
   Better: gather to the host's current map (roster) when its last-lamp record is the default.
2. **(Fixed by fix 3)** **Time of day written while summoned did not persist** (row 7b): the guest logged `own save
   9800..9802 0 -> 7 (pool)` 0.4 s before the run was ended; in the next session its own save read
   9800 = 0 (13201803 too). The write already requests a save (`RequestGameSave`, party_story),
   but GameSaveTick waits for 30 steady frames (and at most one save per 15 s), so a quit right
   after the cutscene loses it, and the story item is not replayed later. Fix idea: keep the item
   queued until a save went through, or re-derive the own time of day from the host's snapshot
   at each join.
2b. **(Fixed by fix 3)** **Blood Moon is not part of the late-joiner catch-up** (row 13): the C2 snapshot brings boss
   flags into the own save, but the time of day (9800..9802, set by the host's event 13201803 with
   `2003[...]` value ops, not a boolean the change stream carries) and 13201803 stay 0 in a late
   guest's own save. Needed for the user decision "a guest behind catches up fully": copy the host's
   time-of-day value into the own pool at the catch-up / join (party_story already has the write
   for the live case).
3. **(Solved, row 9)** **Mergo's Wet Nurse could not be killed by the test tooling**: the boss is spawned by
   event 12601802 (condition: the host inside region 2602805), which a flag write does not run. Two
   `host_walk` hops into the region fire it; 2600800 / 2600801 are immortal parts damage-linked to
   2600802 (the HP holder), so 2600802 is killed first. `host_kill` reporting "NOT loaded here" is
   unreliable for bosses (GuestSvcFindChr misses them).
4. **(Root-caused, fix 6 opt-in)** **A cutscene played a second time on the guest**: the client's own copy of
   the host-world event ran `2002` right after the live mirror ended (run 5f: `guest: EMEVD 2002[3] ... from
   event 12601802`, 0.1 s after the mirror). Gehrman's refusal in run 6b had the same pattern.
5. Summons took longer late in the day (host hosting 85-108 s after start, guest joined 70 s
   later in run 6): machine load (10+ queued runs); not investigated.
6. **Visual (screenshots, run 9c): a stale host nameplate during the host's load.** After the skull
   cutscene the host warped (load screen); the guest's window kept the host's native nameplate
   ("Beckoner / Hunter0") at the host's last spot by the altar for 20+ s, until the guest's own
   fallback load (the B2 re-join timed out at 25 s, machine load). At 13 m in the dark it is unclear
   whether a body is under it; worth checking against the user's "health bars floating in midair".
7. **Visual: the party member plate (bottom left, "Hunter1 13 m") stays on screen during cutscenes**
   on both windows (run 9c, skull scene). Cosmetic.

## H3 slice: does the guest's own save keep the host's progress (2026-10-13, agent h3)

Base save copies (`savedata_20261010`, title join + summon in m21). Run 1: host `set_flag` of lamps
13407800 / 13507800, shortcuts 13401210 / 13501101, boss 13401800, cutscene 13401801 (all 0 in both
saves, `BB_PARTY_PROGRESS_TEST_READ`); the guest applied all 6 in load mode 1 and requested a save
0 s later (its `savedata_party` file written the same second); the run then killed both games.
Run 2 (Continue): the guest's first read is `own 1` for all six, so nothing was lost by the kill.
The host's own read was 0: `set_flag` is a bare test write that no game save follows (a real lamp /
lever / boss triggers the game's own save), so it is not a product gap.
Not covered: echoes from a host kill (`host_kill=0` picked 2410740, not loaded; no Echoes change),
an item picked by the guest, the guest's Leave path (the guest never loaded its own world).
Notes for the next run: the guest's `BB_PARTY_PROGRESS_TEST_CLEAR` only runs in load mode 0, so a
title-joined guest never clears; a `guest_*` travel step never fired for a title-joined guest
(no (re)join event on its clock), so `--crash-when` on it did not trigger.

## Device lost on the guest's Moonside Lake load (2026-10-15, agent devlost)

parg's G3 run 1 (2026-10-14) ended with the guest's game losing the GPU (device lost) on its m32
load right after the host's lamp travel to Moonside Lake 3202952 with Rom dead; the Blood Moon
scene 32000000 started on the host while the guest was loading. Its hang shader dump went with
parg's environment.

Re-run on one PC (RTX 3070, 2 loopback instances, 720p, breadcrumbs on, test saves, host
`BB_PARTY_DIRECTOR_TEST=host_flag=13201800@5%m,` then `host_warp=3202952@12%m,host_warp=2412951@12%m`
repeated; the `%m` keeps a `host_warp` after a flag step from waiting for a re-join):

| Session | What | Guest loads | Guest m32 loads | Device lost |
| --- | --- | --- | --- | --- |
| r2 | Rom dead; each arrival at the Lake ran the Blood Moon scene on the host and sent it to the loading guest (5 x `story ... cutscene 32000000 sent to the party`) | 20 | 5 | none |
| r3 | Rom dead + 13201803 + 9800..9802 (Blood Moon world) set first | 14 | 3 | none |

Both sessions were ended from outside after 606 s / 430 s (both games `exit 4294967295` at the
same moment, no crash log, no harness verdict: not this run's doing). Screenshots every 20 s of both
windows: the guest at the Lake with Queen Yharnam, Central Yharnam, loading screens; no glitch.
Not reproduced in 8 Lake loads of the guest (34 loads in all).

A lead from 2026-10-06 (docs/CHANGES_2026-10-06.md): the one GPU hang the breadcrumbs located so far
was the particle compute shader `cs 2da7fe60` (indirect dispatch after `cs 4ca76892`); its two
loops run up to a count read from a read-only buffer (binding 3, dwords 0 and 580 of the emitter
data), so stale or not yet uploaded emitter data during a load would make them run for billions of
iterations (a TDR). Unconfirmed: the m32 dump was lost.

New diagnostics so the next occurrence is caught (any player, no switch needed):
`out\device_lost_<time>.log` written before the game closes (VK_EXT_device_fault address / vendor
info where the driver has it, VRAM in use against the budget, the breadcrumbs report), the stuck
command's guest code `out\hang_shader_<hash>.bin` and its SPIR-V from the shader cache
`out\hang_shader_<hash>_<perm>.spv`; the launcher's automatic support bundle starts on a device-lost
report and takes all of them. A second thread seeing the device lost waits (up to 5 s) for the
report before asserting. Checked live with `BB_DEVICE_LOST_REPORT_TEST=240` on the guest: the report,
`GPU breadcrumbs: on; device fault info (VK_EXT_device_fault): on`, a dispatch's `.bin` (64 KiB)
and `.spv` (valid SPIR-V 1.6, spirv-dis).

The other device lost seen this cycle (port7's try of Mrsuss60 25f95440's timer change, not
merged: `read_tsc` through the process counter at a pinned 1 GHz) came ~7.7 s into boot after a
GPU library assertion: a guest-side timing change, not a shader or a load; common to both is only
that neither left a report on disk, which the new `device_lost_*.log` covers.

## Device lost in a guest's first load, 0.7.7 (agent devlost077)

Smoke saw rare device losts (bossrunes br, upport sj at 30 FPS, clothfps mds, lifts fw/fwg/lf) whose
visible asserts (`Device lost during submit` at the bindSparse in buffer_cache.cpp / the submit in
vk_scheduler.cpp, and the following vk_scheduler / vk_semaphore asserts) are only the first thread to
notice an already lost device, not the cause. Not reproduced: seam_rejoin `sj` at 30 FPS, 10 runs
back to back on one PC (RTX 3070, 2 loopback instances), 0 device losts (the failed steps of those
runs were seam logic steps, none a GPU one). Left planned: the next occurrence is already
captured by `out\device_lost_<time>.log` (breadcrumbs, device fault address, hang shader `.bin` /
`.spv`; see the section above). Whoever sees it should read that report first: an "invalid write
at 0" fault address points to a shader using an unbound buffer (stale descriptor in a first load).

## 2026-10-15 segment: Cathedral Ward -> Old Yharnam -> Forbidden Woods -> Byrgenwerth (agent pard)

The next main-path segment of the live playtests, 2 loopback instances, the test saves
(`instances.py run --count 2 --party`; the first run of a chain with `--fresh-saves`, each later run
a *Continue party game* of the saves the run before left). Test steps: `host_warp` (party travel),
`host_kill` (Lua ForceDead by id), the guest's fog by `BB_PARTY_WORLDACT_TEST=obj=<fog>` (stands at
the fog until the prompt is up, Cross). Boss flags read with `BB_PARTY_PROGRESS_TEST_READ`, own
pools dumped with `BB_PARTY_FLAG_DUMP=1`, `tools/party/save_check.py` on S5b.

| Run | Milestone | Result |
|---|---|---|
| S1d | Vicar Amelia's first fight (host's Amelia flags cleared), started by the **guest** at her fog | **pass**: guest through the fog 165.6 s; the host's first-fight event ran (host's chr visit 0.07 s, put back 217 m away at the lamp); intro 24000060 on the host and the guest's live mirror at 166.0 s; the guest in the fight with Amelia's health bar (screenshots) |
| S5a | Old Yharnam: party travel from the Dream, guest-started Blood-starved Beast fight | **pass / partial**: travel and re-join 20.9 s; the guest's fog started the fight (boss bar host 58.05 s, guest 58.11 s; the base save had BSB's first entry already, so this was the re-fight path); the `host_kill` step found no target (a boss is not in the enemy census) - test aid fixed (`host_kill` of a boss entity uses ForceDead by id again), BSB kill not re-run |
| S5b | Continue; Forbidden Woods travel, Shadows of Yharnam (3 kills) | **pass**: travel, re-join 33.5 s; boss defeat 12701800: the guest +2 Insight, reward (blood gem) given while summoned, 12701800 / 9463 in the guest's own pool (dumps) and still there after its Leave; save_check consistent |
| S5c | Continue; Byrgenwerth (Moonside Lake lamp), Rom | **pass**: both read 12701800 = 1/1 after the restart; travel, re-join 13.3 s; Rom's defeat 13201800: guest +2 Insight, reward lot 51001900 (goods 1591) given while summoned, 13201800 / 9465 in its own pool |

Not covered here: the Blood Moon (Rom dead + the host near Queen Yharnam) and its travel to
Yahar'gul (the "Travel with the party?" path of scripted warps, quest_audit P8), Cathedral Ward's
plaza gate with the shared Hunter Chief Emblem, the Blood-starved Beast kill.

## 2026-10-15 segment 2: Blood Moon -> Yahar'gul -> ... -> the ending (agent pard2)

The rest of the main path, continuing pard's segment the same way (2 loopback instances, test
saves, `instances.py run --count 2 --party`, the first run with `--fresh-saves`, each later run a
*Continue party game* of the saves the run before left, kept between runs under the agent's
`campaign_state/`; guest Continue = title join unless noted). Each run 230 s (the shared queue's
lease); `BB_PARTY_FLAG_DUMP=1`, `tools/party/save_check.py` after each; screenshots every 15 s.

| Run | Milestone | Result |
|---|---|---|
| c1 | Byrgenwerth: Rom killed (`host_kill=3200800`), host to Moonside Lake 3202952: the Blood Moon | **pass**: 13201800 boss kill, guest +2 Insight (6 -> 8); Blood Moon scene 32000000 on the host 59.4 s and the guest's live mirror 59.4 s (36 s); the guest's own time of day 6 -> 7 (pool); story warp 2802958 `this player's own travel`, the question 2.0 s after the scene, YES, party travel; guest replayed it and stood beside the host in Yahar'gul (screenshot: the two in the chapel); save_check consistent |
| c2 | Continue in Yahar'gul (Blood Moon): the guest's title join, the One Reborn's first fight started by the **guest** | **pass**: guest through fog 2801800 at 55.5 s, host visit 0.14 s, intro 28000000 on both (live mirror, 79-80 s), the host kept at the arena entrance through the scene and put back 1 s after it, alive; the `host_kill` step (75 s) hit during the scene and killed nothing lasting (the boss entity is 2800803, see c13) |
| c3 | Cathedral Ward: the plaza gate (12400160 cleared on the host, reload) | **partial**: the guest's prompt at the gatekeeper was the "need the emblem" talk (10010310: a summoned guest's key items read 0 in the host's world, so its copy of 12400760 takes the no-emblem branch); the host's `host_obj` + Cross found no prompt at its spot: the gate stayed shut. As in vanilla only the host can open it; not re-run |
| c4 | Old Yharnam 2302950, the Blood-starved Beast killed (`host_kill=2300800`, the fixed boss path) | **pass / found**: boss kill 12301800, guest +2 Insight; the Pthumeru Chalice (lot 80000000, key item) waited for the guest's own world, which a title-joining guest never loads, and the run ended first (save_check LOST) -> **fixed**: owed key items on disk, given at the next own world (c12) |
| c5 | Nightmare Frontier 3302950: the Amygdala's first fight started by the **guest**, then killed | **pass**: guest through fog 3301800, host visit 0.07 s (no scene: boss started at once), first-fight Insight +3 to the guest, boss kill 13301800, Insight +3; reward lot 80000200 waited (as c4) |
| c6 | Yahar'gul 2802950: the One Reborn | **partial**: `host_kill=2800800` ForceDead without a defeat (the boss bar entity is 2800803); owed lots 80000000 / 80000200 recorded on the guest (`owed key item: lot ... kept`) |
| c7 | Lecture Building 3202959 | **pass**: party travel, re-join 4.4 s; the door fixture named the doors' SFX entity 3202000 (`not loaded`): the real door not run |
| c8 | Nightmare of Mensis 2602950: Micolash's flag set, Mergo's Wet Nurse's first fight started by the **guest**, killed | **pass**: guest through fog 2601800 71.5 s, host visit, intro 26000010 on both (29 s), host kept through it and put back; `host_kill` 2600802 -> boss kill 12601800 at 173 s, guest +3 Insight; reward lot 55100000 (One Third of Umbilical Cord) owed for the guest's own world |
| c9 | Upper Cathedral Ward 2422950: Ebrietas killed | **pass**: boss kill 12421800, guest Insight 21 -> 24; reward lot 80000300 (a chalice) owed for the guest's own world |
| c10 | Hunter's Dream: Gehrman's refusal (72100131), the fight, the ending | **pass**: refusal scene 21000040 on both (live mirror, 55 s with the skip), Gehrman killed (12101800, +3 Insight to the guest), ending 2 (event 12100000) sent to the party at 131.5 s: the host's remo and the guest's own (in its own Dream, by design) both 72-73 s, NG cycle 0 -> 1 on both, staff roll on both windows (screenshots) |
| c11-c13 | Continue after the ending | **pass (as designed)**: both load NG+ (NG cycle 1) at the clinic; the prologue rule holds summons ("members in their prologue", #19), so the c11 walk, c13's One Reborn kill (`host_kill=2800803`) and c12's join did not run |
| c14 | Continue, the guest without title join (its own world first), owed key items from c4-c9 restored | **pass**: 4 s into its own world `owed lot 55100000: gave goods 4323 x1 (held 0 -> 1)` (One Third of Umbilical Cord, the Wet Nurse's), the three chalices already held (the chalice share gave them at c10's own-world ending: "not given again"), a save requested, the owed file empty after |

Results: the main path from the Blood Moon to the ending ran with 2 players, a Continue between every
run, the host and the guest in the same world after every travel (re-join 4.4-9.0 s on the host,
one loading screen each), boss flags and Insight on the guest each time, the ending and staff roll
on both. Found and fixed here: the host killed at the end of a guest-started first fight's intro
scene (pard2 P5 work, quest_audit.md P5), and boss key items never reaching a guest who keeps
joining from its title (owed lots, c4 / c14). Not done: the plaza gate by the guest (vanilla: only
the host's emblem opens it; the host's spot press found no prompt), the One Reborn's kill (c6 named
the wrong entity; c13 fell into NG+), the Lecture Building door by the guest (wrong entity),
Cainhurst / Hemwick / the Chalice Dungeons in this chain, 3 players.


## 2026-10-15 segment 3: the soak's open items (agent pard3)

2 loopback instances (3 for O2: two windows + one headless guest, the A7 way), test saves (the
character owns the DLC and the Eye), each run its own session unless noted; screenshots every 3-15 s
under `agents/pard3/runs/<run>` (kept in `agents/pard3_shots_keep`).

| Run | Milestone | Result |
|---|---|---|
| L2 | Lecture Building: the 1F door (3201124, ObjAct 13200124) used by the **guest** | **pass**: the guest's kick reached the host's game natively (`ObjAct kick (another player)`), both games ran 13200310's story warp, one question (P4: the host's), YES, the party in the Nightmare of Mensis, re-join 12.9 s (screenshots: both side by side in Mensis) |
| O1 / R1 / R3 / R4 | Rom killed, the **guest** walks up to Yharnam at Moonside Lake: the Blood Moon | **pass**: `slot 1 stands at story event 13201803's spot`, the host's chr visited the lamp spot 0.1 s, scene 32000000 on both, the host put back, the story warp's question, YES, both in Yahar'gul; there the host's "Ritual secret broken" notice showed on the guest too (R3 / R4) |
| O2 | Yahar'gul, **3 players**: the One Reborn started by guest 1, killed by its boss entity 2800803 | **pass**: intro 28000000 on all three, kill 12801800, +3 Insight on both guests, the headless guest's own save `12801800=1` after it went home |
| P1b / P1d | Cathedral Ward: the main gate (12400160 cleared on the host) opened by the **guest** with its own Hunter Chief Emblem | **pass**: the guest's copy took the emblem branch (`key item gate ... as held`), "A special emblem opened the main gate" on both screens, 12400160 on the host (its own copy ran from the guest's press) and in the guest's own save; P1c (Continue): the host's world keeps the gate open |
| L1c, L4, L5b, L3c | DLC first fights started by the **guest** (Ludwig, the Living Failures, the Orphan of Kos, Laurence) | see quest_audit.md P5 (pard3) |
| E1 / E2 / E3 | The host's ending (test aid) asks the guest | **pass**: NOT NOW = home, NG 0, no ending flags on disk; YES = staff roll, NG 0 -> 1, 6601/6603/6604 on disk; YES with three cords = Childhood's Beginning (6602) |

Not done here: Cainhurst / Hemwick in this chain; a lone guest's boss fight (LB2: no defeat ran on
the guest's own game).


## 2026-10-15 segment 4: Amelia's skull, Hemwick, Cainhurst (agent pard4)

Loopback instances, test saves (the default save), **3 players** for B2 / B3 (two windows + one
headless guest), screenshots every 3-5 s under `agents/pard4/runs/<run>` (kept in
`agents/pard4_shots_keep`). B2 is one continuous session (Cathedral Ward -> Hemwick -> Cainhurst).

| Run | Milestone | Result |
|---|---|---|
| B2 | Grand Cathedral, evening (fixture: the host's 9801 / 12401803 cleared): Amelia's skull examined by the **guest** | **pass**: the guest's copy of 12401803 ran (its client / chr-type checks skipped), the game passed the press to the host's own event: scene 24000030 on the host and both guests (screenshots: the same frame "Master Willem, I've come to bid you farewell." on both windows), the host's time of day 2 (night) sent to the party |
| B2 | Party travel to Hemwick; the Witch of Hemwick's first fight started by the **guest** (P5) | **pass**: the host's first-fight event 12201802 ran 0.3 s after the guest's fog, both witches killed (host_kill), 12201800 / 9452 on the host, Insight to all |
| B2 | The carriage: the host in its region (12200130: Cainhurst Summons held, the Witch dead) | **pass**: scene 22000030 on host and guests; the carriage's own press was not reproduced (the test stand put the host into a wall), so its warp ran as the story warp (`host_lamp=2502959:story`): the travel question, YES, the party in Cainhurst |
| B2 | Cainhurst: Vicar Logarius's first fight started by the **guest**, killed | **pass**: intro 25000020 mirrored on the guests (the guest's own copy skipped as already played), the boss doped with 7501 (two cooperators) on every game, HP 9081 on all three, poise +60 counted; 12501800 / 9460 on the host; found: each guest got +6 Insight against the host's +3 (fixed: the relay now tops up only to the host's grant, live B3 / B4: guest 6 -> 9 like the host) |
| B3 | Hemwick again, 3 players, `BB_PARTY_FLAG_DUMP=1` | **pass**: `save_check`: both guests' own pools hold every host flag of the synced categories (0 missing), items consistent; every save on disk has 12201800 |
| B4 / B5 | The Rune Workshop Tool (shared key treasure 2200360) picked up by the **guest** | see ERSC_PARITY D4 |

Not done here: the carriage's press (above), Queen Annalise, the Chalice Dungeons in this chain.
