# Playtest 1 triage (2026-10-13 Internet session, build 0.6.0 8143fda)

Input: `E:\games\bbport\logs_playtest1\` (bb_bugs.txt with 18 notes; host = 5 runs + 1 crash dump; guest
= 5 runs). It was one ~2.5 h session, about 17:08 to 20:40 local time. Head checked for "already fixed" is
`windows-upstream-0.5` at aeeb408 (git log 8143fda..HEAD, CHANGES.md "## 0.6.1 (unreleased)", and the code).
This is read-only triage: nothing was changed, built or run. Batch 1 (deaths, rests, world reset) landed 2026-10-14 (agent/pt1deaths; b2_seamless.md "2026-10-14 (pt1deaths)"); its rows are marked in the summary.

Log names give each run's END time. H1..H5 = `visy_host/launcher_20261013_183856 / _184410 / _192054 / _193023 /
_193033_last`, G1..G5 = `guest/launcher_20261013_184415 / _185717 / _192049 / _193046 / _194949`.
The logs have no per-line timestamps, so evidence is cited as `file:line`. Maps: m21_00 = Hunter's Dream,
m24_01 = Central Yharnam / clinic, m24_00 = Central Yharnam part 2 (Gascoigne), m23_00 = Old Yharnam.

**Coverage gap:** there are no guest logs for H1 (before 18:41), for H5 after 19:49:49 (Gascoigne, Cathedral/Oedon,
Old Yharnam), or for the rejoin after the crash. Bugs #8, #14, #16, #17 and #18 and most of #13 therefore rest on
the host side only. **Ask the guest for his launcher logs after 19:49:49** (and the 17:xx-18:40 runs).

Fix direction (from the user): what changes play the most comes first: players warped around, a world that does not
reset, broken death warps and occlusion, wrong rejoin spawns. Where a vanilla loading screen fixes an issue, the
loading-screen path is the proposed fix. A seamless (no-load) variant comes back only later, as an opt-in.

## Summary

| # | Bug (players' words, shortened) | Severity | Fixed on head? | Most likely cause (short) | Batch |
|---|---|---|---|---|---|
| 1 | Direct summon while the host is in its prologue | major | **fixed (pt1join)**; the title-join path too (pt1verify, live refusal) | `dsummon::HostRefusal` has no start-ready check; the host makes a room on demand (H2:919-992) | 2 |
| 2 | Guest's bell prompt also shown on the host | minor | **fixed (pt1join)** | inferred: the host director auto-rings the Beckoning Bell (SpEffect 9000) whenever a guest waits or rings | 2 |
| 3 | Only a host rest resets the world | major | **fixed (batch 1, live)**: a guest's alone rest gives the host a rest load | a guest's rest at a lamp asks "Travel with the party?"; NO sends it to the Dream alone with no reset (design gap) | 1 |
| 4 | Enemy loot from the guest not shared | minor | **fixed** (pt1items: one shared roll per kill, `BB_PARTY_SHARED_DROP_ROLL`) | ERSC_PARITY D4: per-player drop rolls. A design decision to revisit, not a bug | decision |
| 5 | Guests cannot knock on doors / see plot dialogue | major | **yes (live, pt1follow2)**: Iosefka / fake doctor doors (pt1npc) and the window residents' door dummies 2410870/71 (prompt, dialogue on the guest); a knock sets only a local knock-sound trigger, nothing for the host; Gilbert's hit chr 2410782 not reproduced in this save state | unconfirmed: ghost NpcParam rows (Iosefka 6060) and/or a client-only talk gate; the logs do not print the override counter | 4 |
| 6 | Host death blocks guest loot | major | **fixed (batch 1)**: host death = party respawn load; the guest's send-home is followed by the load (live) | host revive-in-place leaves the guest's death and send-home state half-done until its next load | 1 |
| 7 | Host death warp breaks world / occlusion | major | **fixed (batch 1, live)**: host death = party respawn load for everyone | host death = revive in place (moved up to ~190 m, no load, no world reset) | 1 |
| 8 | Fog wall for the guest until the host goes in | minor (on head) | **fixed (pt1follow2)**: cause found live: the guest's position (+0xC0) stays at its arrival spot in the host's world, so the 5 m check never passed; physics position now (unit; notice after the fix not yet seen live) | B2 guest fog-first (1f981e9) covers plain fogs; first fights with an intro (Gascoigne) still start with the host | 4 |
| 9 | Rejoined guest lands in the Dream, not in Central Yharnam | **blocker** | **fixed (pt1join, live)** | the guest rings before its own map is known, so no gather; cross-map direct summon never loads the host's map (aeeb408 only keeps the gather as default) | 2 |
| 10 | Doll level-up plays on the host + position jumps back | major | move part **fixed (batch 1)**: same-map guest lamp = own load, the host never moves; flags: batch 4; flags **fixed (pt1npc)**: never_sync Doll rows; **live (pt1follow2)**: guest level-up, no world action, host never moved | Doll flags 72100100-102/113, 12100112-114 synced as npc_quest; a same-map lamp warp became a host-led party move | 1 (move) + 4 (flags) |
| 11 | Gehrman is a ghost for the guest | minor | **fixed (pt1npc)**: 804000 in the talk-NPC table, written live | NpcParam 804000 has isChangeWanderGhost; only the Doll's 803000 is patched | 4 |
| 12 | Item prompt stuck on the guest at host death | major | death part **fixed (batch 1)**: a load follows a host death; the question closes on load / death / 30 s (live); prompt part **fixed (pt1items)**: item-get watchdog clears a box left over a load / death (unit-tested); **live (pt1follow2)**: the guest's open box is gone after the host-death load (screenshots) | `ShowItemGet` is gated only on `Calm()`, plus the half-done death state of #6 | 3 (gate) + 1 (death) |
| 13 | Host's dropped echoes not shown, could not recover | minor | **logged (batch 1)**: stain changes logged; a host death is a load again | not proven: 2 of 6 host bloodstains never picked up; the revive-in-place warp is a suspect; no pickup log | 1 |
| 14 | Gascoigne cutscene played twice | minor (on head) | **yes** + host guard (pt1npc): story_up refused if played < 10 min or boss fight | the guest's own intro event replayed on the host; fixed by story dedupe default on (025b0a9) + live check (a5033c3). Residual: the host does not refuse a replay mid-fight | 4 |
| 15 | Boss loot prompt frozen on the guest, key items missing? | major | **fixed** (pt1items: popup gate alive / cutscene / boss banner + watchdog) | badge pushed via `ShowItemGet` in the same second as the boss defeat banner; no alive/cutscene/banner/load gate | 3 |
| 16 | Oedon Tomb Key (and other key items) not on the guest | major | **fixed** (pt1items: logged decisions, boss-reward re-give when not held) | the host sent lot 31000 (H5:8051-8062, again at H5:11875); the guest side is not logged. Suspects: #15 frozen box, or ledger seeding from a synced boss flag | 3 |
| 17 | "Red guy" in Oedon Chapel is a ghost for the guest | major | **fixed (pt1npc)**: 42 talk-NPC NpcParam rows written live (Alfred, Dweller, Eileen ...) | the same ghost bit on almost every talk-NPC row (Alfred 6151/6155/6154, Chapel Dweller 113010, Eileen, Arianna...) | 4 |
| 18 | Guest echoes not takeable when both died | major | **fixed (batch 1, unit)**: recount every frame + early down notice; wipe live (pt1follow, pt1verify); a guest after a B2 load / title join left the party on death (chr type 0): fixed (pt1verify, live) | wipe race: the host counted "0 down" just before the guest's death arrived (H5:10184 vs 10210), so it revived alone instead of a wipe; plus the #6 state | 1 |
| 19 | Guest's own prologue: a loading screen right after taking the sidearm, woke in the clinic with Iosefka instead of the Notebook + headstone to the sickroom (not in bb_bugs.txt; players' report afterwards) | major | **fixed (prolog)**: ready only after the first Dream visit; no summon / gather / sync while anybody is in its prologue | the guest counted as "ready" as soon as both weapons were taken (still in its first Dream), and the guest's gather lamp-warped it to the host's lamp 2412959 in m24_01 although the host was itself still in its prologue (G2:14543-14574) | prologue |
| 20 | "Travel with the party?" asked on BOTH players for one lamp / headstone use | major | **fixed (splitq, live)**: only the player whose own game set the lamp / headstone flag is asked; the other logs "host/guest X is choosing travel" (toast) | the same warp event (headstone 12107000, world lamp 7200) also runs on the other player's game and reaches the lamp warp 0x13CDF30, which the split interceptor took for that player's own use | split |
| 21 | Rot not removed on a lamp "rest" (just triggering the lamp, not travelling) | minor | **fixed (pt1extra, unit) + rotfix**: a dialog opening beside a lamp (4 m from its revive spot or from the lamp object, ReturnPoint id - 1000 / - 2000) is a rest, a level up too; a lamp warp is the respawn only while the player is still down; a title-joined guest's home warp is no rest (**live**). The lamp box opened live (rotfix run 2, screenshot) but 5.7 m from the revive spot, so the 4 m rule missed it: now also measured to the lamp. **Cleanse live (menuhide)**: the direct-lamp-travel warp menu and the old box (`BB_LAMP_TRAVEL=0`) both count | rot was cleansed only by the lamp warp 0x13CDF30 (the travel), so opening the lamp's box and answering NO kept it; the 20 s "after a death" rule also called a real lamp use the respawn (H5:6072/10228) | pt1extra |
| 22 | Lamps did nothing for a player in Join-a-party mode with no host connected (reported after the playtest: "no lamps worked in single player"; not in bb_bugs.txt) | major | **fixed (lampsave, live)**: with nobody to travel with (a guest in its own world, a host alone) lamps, headstones and the Hunter's Mark are vanilla; the save was fine (no repair) | the guest still had the launcher on "Join a party" with no host running, so every party hook was on in its own world: the same-map lamp warp was a party in-world move (or swallowed beside the lamp), a guest's Mark could go to a stale host lamp. The 0.6.0 log shows no lamp warp attempt at all (see below) | lampsave |
| crash | Host crash 20:38:10, eboot +0x2199310 null+8 | major | **yes** (batch 5: guard at +0x2199303, frame allocator 1 MiB) | per-frame arena allocator +0xe45560 returned NULL. FUN_02198fd0 checks the 1st alloc, not the 2nd (+0x21992fb); `BB_ARENA_GUARD` covers only the +0x21d5b35 caller | 5 |

Top extra problems (details below): G4's direct summon failed 8 of 10 times with nothing logging why; the guest
left the session on its own 92.7 s after joining (G4:10655); the party watchdog stopped "stuck" summons when the
host travelled mid-join (H1:3694, H2:1938); host refusals are never logged on the host; a guest restart leaves a
"crashed member's connection is still held" refusal and a premature "no arrival within 180 s"; the guest ran with
Tracing=on (198 false engine-watchdog dumps, 57k `[BLOCKED >3s]`, 20 MB logs); "stale remote chr" ghosts after
revive-in-place; the Old Hunter Bell is silently lost for a guest; the host texture cache was over budget for most
of H5.

## Fix batches (ordered by play impact; files chosen so parallel agents do not collide)

1. **Batch 1: deaths, rests and world reset, loading-screen path** (#7, #6, #18, #13, #3, #12 death part, #10 position jump).
   - Make `BB_PARTY_B2_HOST_REVIVE=0` the default (that path is live-tested): a host death becomes one respawn load
     for everyone. Revive-in-place stays an opt-in.
   - Re-check the dead count at the revive tick so that both players down becomes a wipe load.
   - A guest rest asks the host for a rest load where everybody stands, and the prompt says the world resets.
   - Close or cancel open split questions on a load, a host travel or a death, with a shorter timeout.
   - A same-map lamp warp by a guest is a solo warp with a loading screen, never a party move.
   - Log bloodstain pickups.
   - **Files:** `party_b2.cpp` (host-death/revive/wipe region only, ~l.2990-3950), `party_split.cpp`,
     `party_guestsvc.cpp`, `docs/party/DEBUG_SWITCHES.md`, `b2_seamless.md`, `ERSC_PARITY.md`.
   - **Verify:** instances.py host + guest with `BB_PARTY_TEST_START_LAMP`, `host_kill`/death aids,
     `BB_PARTY_ENEMY_CENSUS=1`; screenshot both instances after each death and rest.
2. **Batch 2: joins, rejoin spawn and summons** - DONE 2026-10-14 (pt1join, see "Batch 2 result" below) (#9, #1, #2, G4 summon failures, refusal logging, held connection
   after a guest restart, watchdog stop during a host travel).
   - The guest waits for its own map before ringing.
   - A guest from another area refuses the room and gathers first (a lamp warp with a loading screen).
   - The host refuses a direct summon while it is not start-ready or is on another map, and logs every refusal.
   - The host skips the auto-ring while a direct summon for that member is pending or has just succeeded.
   - Clear a held connection when the member's new run says hello.
   - **Files:** `party_dsummon.h/.cpp`, `party_director.h/.cpp`, `party_b2.cpp` (`DirectSummonTick` /
     `DirectSummonEvent` region only, ~l.5000+), `tests/test_party_dsummon.cpp`.
   - **Merge note:** shares `party_b2.cpp` with batch 1 in disjoint regions; whichever merges second rebases.
3. **Batch 3: item prompts and key items** (#15, #12 prompt part, #16, Old Hunter Bell).
   - Gate `ShowItemGet` on alive, no cutscene or banner, and no load or travel pending.
   - A watchdog clears a box stuck for 20 s or more across a load or death.
   - Log every grant decision. Seed the ledger only from flags present before the first join.
   - Replay the denied Old Hunter Bell when the guest gets back to its own world.
   - **Files:** `party_items.cpp/.h`, `party_items.inc`, `game_state.cpp` (ShowItemGet only).
4. **Batch 4: NPCs, story and flags** (#17, #11, #5, #10 flags, #14 residual, #8 residual, catch-up replay of an
   already-seen scene).
   - Clear isChangeWanderGhost for every talk-NPC row (6040-6990, 8120-8125, 113010, 804000).
   - Add never_sync rows for the Doll level-up flags.
   - The host ignores a guest's story replay while it is in a fight, or when it played the same scene within N s.
   - Record scenes the guest saw in its own world.
   - A rate-limited normal log for guest talk-override hits, needed to settle #5. Put that one line in batch 1's
     `party_guestsvc.cpp` change, or land it after batch 1.
   - **Files:** `seamless_rules.cpp`, `party_flags.inc`, `party_story.cpp`, `party_worldact.cpp` + `party_hud.cpp` (#8 residual only), `docs/party/guest_services.md`.
5. **Batch 5: stability** (crash, watchdog noise).
   - A second arena guard at +0x2199303 that skips the submit if r12 == NULL, then audit the other 44 callers of
     +0xe45560.
   - The `src/probe.c` watchdog dumps only on stalled flips.
   - The launcher warns when Tracing is on in a party.
   - Look at texture-cache budget behaviour on an 8 GiB card.
   - **Files:** `gpu/shim/bbport_stability.cpp`, `src/probe.c`, launcher.

**Order by play impact:** batch 1, then batch 2, then batch 5 (small, independent; run alongside), then batch 3,
then batch 4. **#4 needs a decision first:** keep per-player drops (ERSC parity) or move to a seeded shared roll in
`party_worldact.cpp` behind a switch. Do not start it in parallel with batch 4's #8 residual, which also touches
`party_worldact.cpp`.

---

# Bug details

## Section A: joins, summons, rejoins, Doll (bugs 1, 2, 9, 10) and the session timeline

Abbreviations: H1..H5 = host logs `visy_host/launcher_20261013_183856 / _184410 / _192054 / _193023 / _193033_last`,
G1..G5 = guest logs `guest/launcher_20261013_184415 / _185717 / _192049 / _193046 / _194949`
(the file name is the END time of the run). Line numbers are `file:line`. Maps: m21_00 = Hunter's Dream
(0x15000000 = 352321536), m24_01 = Central Yharnam / clinic (0x18010000 = 402718720), m24_00 = Central
Yharnam part 2, m23_00 = Old Yharnam. Head = aeeb408.

Per user direction (relayed by the coordinator): fixes are ordered by play impact (players warped around, world not
resetting, broken death warps, wrong rejoin spawns first); where a vanilla loading screen solves it (rejoin
spawn, Doll level-up position jump) the loading-screen path is the proposed fix, and a seamless variant
is at most a later opt-in.

### Bug 1: prologue skip / direct summon while the prologue is in progress

**Evidence.**
- H2 (game 2, new game, save slot 2): `H2:919 start (prologue_solo): step clinic ... -> not ready`, and
  10 lines later the guest is served anyway: `H2:929 the guest joined (slot 1)`, `B2: direct summon: creating
  this host's room for slot 1 -> 1`, `H2:949 room 1001 created`, `H2:959 direct summon: slot 1 (the guest) gets
  this host's room (area 0x18010000)`, `H2:965 the guest joined room 1001`, `H2:992 reports it joined`. The
  host's own director never rang (it only rings when `start_ready`), so this came only from the direct
  summon path.
- G1 (guest save already past the prologue, game-1 save): `G1:4056 step ready`, `G1:4131 direct summon:
  asked the host for its room`, `G1:4223 joined room 1001`, then the guest's game loads the host's clinic
  world: `G1:8107 step clinic ... -> not ready`, `G1:8114 map - -> m24_01_00_00`. The guest died there alone
  (`G1:12201 funnel: guest_died`, `G1:12373 died alone: revived at its own last lamp 2412950`).
- The join never completed on the host: the host kept `cooperators 0` with the guest "in_host_world" and its
  own state "prologue" (`H2` roster at ~1900: `visy(slot 0, prologue ...), the guest(slot 1, in_host_world ...)`),
  then `H2:1938 host watchdog: stop #1: host with 0 cooperators; the joining member made no progress for 60 s
  (stuck summon)` -> `H2:1940 left the session` -> room closed -> `G1:12637 refused by the host: the host
  ended the party`. The players then quit and started over (H3/G2 both new games).
- Game 1 (H1) shows the same family: the guest started a new game while it was a member of the host's room
  (`H1:4380 the guest started a new game while in room 1003; its old membership ends`), the host's travel then
  waited 180 s for it (`H1:4389 host travel: no arrival within 180 s`), and the next direct summon was refused
  (`H1:4428 slot 1 refused (a crashed member's connection is still held)`).
- G2 (guest new game) asked three times while the host was still in its prologue: `G2:15765/15783`,
  `G2:18353/18357`, `G2:24989/24993 direct summon: the host's answer: refused by the host`. H3 logs NO
  direct-summon line at all for these requests (no `slot 1 refused (...)`), so the reason is invisible;
  the same holds for `G5:4187/4217` (refused) with nothing in H5. The refusal was right (host in prologue)
  only by luck.

**Cause.** Bug. `dsummon::HostRefusal` (gpu/shim/party/party_dsummon.cpp:151) refuses for not-in-world, boss,
cutscene, full, sign-summon and crashed-hold, but not for "the host has not finished its start" (the
director's `start_ready`, party_director.cpp:278/443, `StartReady` in party_start.h). `DirectSummonTick`
(party_b2.cpp:5051) even creates the host's room on demand (`Create` at 0x1ecfcd0) while the host is in the
clinic. ERSC_PARITY / campaign_start.md C1 say a guest is only summoned once both have finished their start
(prologue_solo). Secondary: some refusals reach the guest without a host log line (H3, H5): a logging gap
in the host's request path (party_b2.cpp DirectSummonEvent / DirectSummonTick) that should be closed so
the next playtest shows why.

**Fixed on head?** No. `git log 8143fda..HEAD -- party_dsummon.cpp party_b2.cpp` only adds A11 (boss /
cutscene) and A5 changes; `HostRefusal` at head has no start/prologue clause.

**Severity.** Major (it broke game 1 and the first try of game 2; a guest gets pulled into the host's
prologue, dies there and the host's watchdog kicks it).

**Proposed fix (smallest).** Add `bool host_ready` to `dsummon::HostView`; `HostRefusal` returns "the host is
still in its prologue" when it is false (with a `Busy::Prologue`-style wait word so the guest's Party page
says "Waiting: the host is in its prologue"). Fill it in `DirectSummonTick` from a new
`party::LocalStartReady()` accessor in party_director.h/.cpp (returns `st.start_ready`). Do not create a
room for a request while not ready. Log every refusal sent (also in `DirectSummonEvent`). Mirror for the
guest: a guest not `start_ready` never asks (it already does not ring; keep the dsum request behind the
same gate). Unit: extend tests/test_party_dsummon.cpp with a `host_ready=false` case.
**Verify on one PC.** `python tools/mp/instances.py run --count 2 --party --env BB_PARTY_LOG_TIME=1
--ienv 0:BB_TEST_NEWGAME=1` (host fresh, guest with a save past the prologue): expect on the guest
`direct summon: the host's answer: refused ... prologue` until the host log shows `step ready`, then one
join; screenshot both windows (guest must stay in its own world while the host is in the clinic).
**Files.** party_dsummon.h, party_dsummon.cpp, party_b2.cpp (DirectSummonTick / DirectSummonEvent),
party_director.h/.cpp (accessor), tests/test_party_dsummon.cpp.

### Bug 2: the guest's bell prompt also shown to the host

**Evidence (indirect: no log line names a prompt).** Every guest ring makes the host ring too: the host's
director rings the Beckoning Bell "for a ready party member that waits" right after a guest connects or rings:
`H1:1710`, `H1:1923` (x2), `H1:3480`, `H1:3715`, `H1:4372`, `H3:1147`, `H3:2638`, `H3:2760` `ringing the
Beckoning Bell (host ready, a ready party member waits): SpEffect 9000 applied`. On the guest side
`ringing the Small Resonant Bell (guest ...): SpEffect 9005 applied` (G1:4134, G2:15768/18356/24992,
G3:4531, G4 and G5 every ask). SpEffect 9000 is the bell's ringing state on the host's own hunter (the
resonance effect and sound), so the host sees "its" bell ring although only the guest asked. No other
prompt on the host correlates with guest actions. (This triage first claimed the split "Travel with the
party?" dialog is only ever asked on the player who used the lamp. That was wrong: it was asked on both
players for one lamp / headstone use; see bug #20.)
A second, smaller cause: the bell grant at the end of the prologue (`Party: bells: Beckoning Bell (lot
10010): awarded`, `Small Resonant Bell (goods 205): given`, H1:1166-1170, H3:1138-1142, G2:14544-14548)
pops the game's item-received banner on whoever finishes the prologue; it is not mirrored to the other
player (no `award lot 10010` on the host when the guest got it), so it is not this bug unless the players
meant that banner. Ask the players which text they saw.

**Cause (most likely).** Design gap: `party_director.cpp` host branch (`HostMayRing` -> `Ring(st, kHostBell,
...)`, ~line 2903) applies the visible bell SpEffect 9000 on the host whenever a member waits, a leftover of
the sign path; with the direct summon (on by default) the host's ring is only needed as the fallback.

**Fixed on head?** No (director host ring unchanged since 0.6.0).

**Severity.** Minor (cosmetic, but confusing: the host thinks it rang).

**Proposed fix.** Host: do not apply the visible ring when a direct summon for that member is under way or
succeeded within the last 20 s; ring only as the fallback after `direct summon: FAILED` / a refusal, and then
without the ringing animation if possible (apply the matching state the sign path needs, not the item use).
Simplest first step: skip `Ring(kHostBell)` while `DS()` has a pending request or `summons_seen` for that slot.
**Verify.** 2 instances `--party`; guest rings; host log must not show `ringing the Beckoning Bell` for a
successful direct summon; screenshot the host at the ring (no bell effect). **Files.** party_director.cpp
(host ring gate), party_b2.h (a `DirectSummonPending()` query) - keep away from party_dsummon.cpp edits of
batch "join gate" by putting the query in party_b2.cpp only.

### Bug 9: rejoined guest lands in the Hunter's Dream instead of Central Yharnam (should land where the host is, bells anywhere)

**Evidence.**
- G4 (guest Continue at 19:24, its save spot is the Dream): the first ring happened before the guest's own
  map was known: `first bell 0.3 s after the world was steady` (just before G4:5637), `G4:5637 direct summon: asked the
  host for its room`, and only after it `G4:5641 Party: map - -> m21_00_00_00`; the catch-up line just before
  shows `map - (0xffffffff)` with `world up, loading no`. No `gather:` line exists in G4 at all. The host was
  in Central Yharnam: `H4:943 direct summon: slot 1 gets this host's room (area 0x18010000)`,
  `G4:5683 summon message type 1, host map 0x18010000`, `G4:5861 joined the host's room (3.4 s)`, then the
  guest stays in m21: `G4:5860/7576 where [slot 1 m21_00_00_00] [slot 0 m24_01_00_00]; apart 1`. The guest
  sat as a client in flow 2 with `cooperators 0` (never entered the host world) until its own game left
  the session: `G4:10678 BeginLeaveSession called from +0x178a51f (role 6)`. Meanwhile the host travelled
  to the Dream to fetch it (`H4:1598 host travel ... m21_00_00_00`, `H4:1714 slot 1 arrived`,
  `H4:1747 member 2 left room 1001 (leave_room)`).
- Same race again after the guest's next load: `G4:18795 asked` -> `G4:18799 map - -> m24_01_00_00`;
  host now in the Dream (`H4:2167 area 0x15000000`); `G4:18883 joined (3.2 s)`, `G4:18882/19099 apart 1`,
  guest flow 2, then `G4:23142 BeginLeaveSession from +0x178a51f`. The players chased each other between
  the two maps for ~6 minutes; 8 direct summons failed (`G4:10889, 14558, 16186, 24404, 26074, 29880, 30088,
  32784 FAILED: the bell sign path takes over (8.1 s)`; host `H4:1797, 1981, 2062, 2533, 2702, 2748, 2764
  reports a failure`), the host's loads took 40.5 s and 59.9 s (`H4:1855`, `H4:2591`), and the host finally
  restarted (`G4:32713 refused by the host: the host ended the party`; H5 starts 10 s after H4 ends).
- Every later rejoin of the guest also restores its own save spot first: `forced placement (own save spot)
  map 0x15000000` (G2:31464, G3:6857, G3:27169, G3:40585, G3:43323) and `map 0x18010000` (G5:43093 ...),
  i.e. the guest's Continue loads where the guest last saved, not where the host is.
- When the order was right the gather works: `G2:14549 gather: own world m21_00_00_00, host in m24_01_00_00;
  to the host's lamp 2412959 before ringing` and `G2:24874 gather: own world m24_01_00_00, host in
  m21_00_00_00; to the host's lamp 2102961`.

**Cause.** Bug (race) plus a known open design gap.
1. `party_director.cpp` guest ring path (~2875-2896): it waits for the host's map but not for its OWN map.
   `GatherGuest` (party_director.cpp:2951) returns `false` ("not gathering") when `s.map_id == 0xffffffff`
   (line ~2997), so the caller rings at once and the direct summon joins the host's room cross-map.
2. A cross-map direct summon joins the room but the game does not load the host's map (documented at head:
   aeeb408 / ERSC_PARITY A5 2026-10-13: "joins the room but does not load the host's map"; join_timing.md).
   The guest then sits in flow 2 until the game drops the session (+0x178a51f).

**Fixed on head?** Partly. aeeb408 keeps the gather as the default and makes the no-gather variant opt-in
(`BB_PARTY_GATHER_DSUM=1`), but the own-map-unknown race in the ring path is still there at head (code read:
`if (!host_map || ... || s.map_id == 0xffffffffu || ...) return false;` then `Ring(...)`), and nothing
recovers a guest that joined cross-map.

**Severity.** Blocker for rejoining (after every guest restart the guest can land in the wrong map and get
stuck; it cost this session ~6 minutes and a host restart).

**Proposed fix (loading-screen path, per user direction).**
1. Guest ring gate: in the `GuestMayRing` branch also `return` (log "waiting for the own map") while
   `s.map_id` is 0 / 0xffffffff, so `GatherGuest` always runs with both maps known; the gather is a vanilla
   lamp warp with a loading screen to the host's lamp / the lamp nearest the host.
2. Safety net in `DirectSummonTick` (guest side, party_b2.cpp): if the room's `area` (dsum_room carries it)
   differs from the own map, do not hand the room to the game; report a failure with reason "other map" and
   let the director gather first (loading screen), then ask again.
3. Host side: refuse a dsum request whose guest roster map is known and another area than the host's
   (`HostRefusal` reason "other map: gather first") - cheap belt and braces.
4. Later opt-in only: the seamless cross-map carry (`BB_PARTY_GATHER_DSUM`).
**Verify on one PC.** Guest save in the Dream, host in Central Yharnam: `instances.py run --count 2 --party
--ienv 0:BB_PARTY_TEST_START_LAMP=2412951 --env BB_PARTY_LOG_TIME=1`, start the guest second
(`--join-sequential`); expect on the guest `gather: own world m21_00_00_00, host in m24_01_00_00; to the
host's lamp ...` BEFORE `direct summon: asked`, then one loading screen and `join: arrived ... from the
host`; repeat 3 times with `instances.py crash --instance 1` + restart (rejoin), screenshots of both windows
side by side (guest beside the host, no "apart 1" in `status: where`).
**Files.** party_director.cpp (ring gate, GatherGuest), party_b2.cpp (DirectSummonTick guest delivery),
party_dsummon.h/.cpp (reason text). Shares party_dsummon.* and party_b2.cpp DirectSummonTick with bug 1:
same batch.

### Bug 10: the guest's Doll level-up activates on the host too (animation) and positions jump back to spawn

**Evidence.**
- Level-up handshake flags of the guest's Doll talk went to the host as world actions and were written into
  the host's world: guest `G5:44580 world action sent to the host (m21_00_00_00): set 72100100(npc_quest)
  72100101(npc_quest) 72100102(npc_quest)`, `G5:44874 set 12100114`, `G5:44941 clear 72100100 72100102`,
  `G5:44965 set 12100112; clear 72100101`; host `H5:3095 slot 1 changed the host's world ... 72100100=1
  72100101=1 72100102=1`, `H5:3109 12100114=1`, `H5:3115 72100100=0 72100102=0`, `H5:3121 12100112=1
  72100101=0`, `H5:3129 72100113=1`; the guest's level-up itself worked (`G5:44898 own character: level 10
  -> 17, echoes 9878 -> 937`). 12100112 = the player's kneel animation event, 12100113/72100102 = the Doll's
  gesture, 12100114 = level-up SFX (party_guestsvc.h:129-135). Setting them in the host's world plays the
  kneel/gesture/SFX on the host ("activates on host too + animation"); the host's HP also changes right after
  (`H5` status lines: hp 552 -> 616 around H5:3185), worth a check that no level was applied to the host.
- "Position jumps back to spawn": just before, the guest's game made a warp to the Dream's own lamp 2102950
  (the Dream arrival spot), split travel asked "Travel with the party?" and the guest answered yes
  (`G5:43349 lamp 2102950 (guest): YES -> travel with the party`, `G5:43350 lamp: request 2102950 sent to the
  host`), so the HOST was moved too: `H5:3080 lamp: slot 1 used a lamp / headstone: the host warps to 2102950,
  the party follows`, `H5:3081 in-world warp (the host's party move, lamp 2102950): (-7.5, -6.3, -17.8) ->
  (-9.2, -6.8, -19.4), no load`, guest `G5:43352 in-world warp ... (-6.5, -5.6, -14.4) -> (-9.6, -6.8,
  -19.0)`. Both players were 2-5 m from the lamp, beside the Doll.

**Cause.** Two bugs.
1. Flag classification: `party_flags.inc` has no per-player row for the Doll's level-up handshake; they fall
   into `{12100110, 12100117, npc_quest}` (line 138) and `{72100000, 72100999, npc_quest}` (line 2370), so
   party_worldact sends them as shared NPC state. The design (ERSC_PARITY H5: every player levels at the
   Doll on its own hunter) needs them local, like `{7400, 7599, never_sync, "per-player: Doll level-up 75xx"}`
   (line 1833).
2. A guest's warp to a lamp in the map the party already stands in becomes a host-led party move
   (party_guestsvc.cpp:518-548 `GuestLampIntercept` -> `GuestLampRequestNow`, host party_guestsvc.cpp:1846
   -> `b2::HostPartyWarpNoLoad`). For a same-map lamp that moves the host for nothing.

**Fixed on head?** No (no change to party_flags.inc Doll rows or the guest lamp request since 8143fda).

**Severity.** Major (the host gets yanked across the Dream and plays someone else's level-up; every guest
levels up in the Dream).

**Proposed fix.**
1. Add `never_sync` rows BEFORE the npc_quest ranges (first match wins, party_progress.h:73):
   `{72100100u, 72100102u, never_sync, "per-player: Doll level-up handshake (t210304)"}`,
   `{72100113u, 72100113u, never_sync, ...}` (check 72100110 too: set by the same talk, H5:3076) and
   `{12100112u, 12100114u, never_sync, "per-player: Doll level-up kneel / gesture / SFX"}`. The existing
   client-side chr-type rewrite (`DollLevelUpCharTypeTest`) keeps the guest's own animation working.
2. Guest lamp in the same map as the host: no party move. Per user direction take the loading-screen path:
   the guest's own vanilla lamp warp with a loading screen (as a guest's split "travel alone" does,
   `arrived alone ... still a client`), or simply ignore a warp to the lamp both already stand at. Do not ask
   "Travel with the party?" when target map == current map == host map.
**Verify on one PC.** 2 instances `--party`, host + guest in the Dream (`BB_PARTY_TEST_START_LAMP=2102950`
on the host); guest: `BB_PARTY_GUESTSVC_TEST=echoes=20000` then level up at the Doll by hand (or `talkprobe`);
expect no `slot 1 changed the host's world ... 7210010x / 1210011x` on the host, the host's hunter does
not kneel (screenshot both); then `BB_PARTY_GUESTSVC_TEST=lampwalk;lamppress` on the guest in the Dream:
expect no `the host warps to 2102950` on the host and an unchanged host position in `status:` lines.
**Files.** party_flags.inc (rows only), party_guestsvc.cpp (same-map lamp request), party_split.cpp
(skip the question for a same-map lamp).

### Bug 19: the guest's prologue broken by a gather load right after the sidearm (Dream -> clinic with Iosefka)

Reported by the players after the session (not in bb_bugs.txt): in the guest's own new game (G2, its own
prologue from G2:7351), right after taking the sidearm from the messengers in the first Hunter's Dream visit
a loading screen came, and the guest woke in Iosefka's clinic with Iosefka present, instead of reading the
Notebook and using the headstone to the clinic's other room (the 1st Floor Sickroom lamp 2412950).

**Evidence (G2 = guest `..._185717`, H3 = host `..._192054`).**
- G2:11899 `start (prologue_solo): step first-dream: ... 12101020 0, 12101021 0 -> not ready` (the guest's
  first Dream, after its own first death and cutscene 21000000 at G2:11707-11884).
- G2:14543 `step ready: ... 9401 1, 12101020 1, 12101021 1, 12100105 0 ... -> READY for party summons`: the
  second weapon is taken - the guest is still in the Dream (map m21_00, Doll asleep, Insight 0).
- G2:14544-14548 the bell grant runs at once (`Beckoning Bell (lot 10010): awarded`, `Small Resonant Bell ...
  given`) - the item banners on top of the messengers' scene.
- G2:14549 `gather: own world m21_00_00_00, host in m24_01_00_00; to the host's lamp 2412959 before ringing`
  and G2:14550 `travel: replay now #0 lamp ... lamp 2412959 ... via lamp warp 0x13CDF30` -> G2:14571
  `loading screen up` -> G2:15780 `map - -> m24_01_00_00` at `(-195.60, -40.53, 220.33)` (G2:15784). 2412959
  is the opening's respawn record (campaign_start.md 1.4: 12410000 sets it), i.e. the clinic's ground floor
  where the fresh character wakes, not the sickroom lamp 2412950 the headstone leads to.
- The host was itself in its prologue the whole time: the guest's roster shows `visy(slot 0, prologue, map
  402718720 ...)` before and after (G2:13288, G2:14551), the host's own log has `step clinic` (H3:930) and
  reaches `step ready` only at H3:1137. So the guest's bell was refused anyway (G2:15765/15783 `direct summon:
  ... refused by the host`) - the gather load was for nothing.
- Same pattern again later: G2:24874 `gather: own world m24_01_00_00, host in m21_00_00_00; to the host's lamp
  2102961` while the host's roster entry still said `prologue` (host ready only at G2:26348 `visy(slot 0,
  home ...)`), plus other party actions into the prologue worlds: the host's first-Dream cutscene replayed in
  the guest's world (G2:21127 `guest #...544 cutscene 21000000 ... taken`, H3:1070 the host's own 9401 scene),
  host flags of its prologue sent to the party (H3:995-1015 `host flag 9400 = 1 (key_event)` ...), rot for a
  prologue death (G2:9066 `death 1; rot on`), the host's sign shown to a prologue member's game (H3:992).

**Cause.** Bug, two parts.
1. `ClassifyStart` (party_start.cpp) called a player ready once 12410000, 9401 and both starting-weapon flags
   were on. The weapons are taken in the FIRST Dream visit, but the vanilla prologue continues there (Notebook
   messenger, then the headstone to the sickroom lamp 2412950). From "ready" on the party treated the player as
   done: bell grant, gather, bell.
2. `GuestMayRing` (and so the gather, `GatherGuest` in party_director.cpp) only checked the guest's own
   readiness, never the host's roster state: a ready guest lamp-warped to the host's lamp while the host was in
   its own prologue. Several other paths (story replays, flag / item syncs, catch-up, rot, the sign board) had
   no prologue check either.

**Fix (prolog, 2026-10-14).** Rule: no party action touches a world that is in its prologue, and summons start
only once every connected member is ready, whoever finishes first.
- `party_start`: the first Dream visit lasts until the player is out of the Dream with both weapons (the
  headstone; latched for the session) or the Doll is awake; an unknown map counts as not ready. The roster
  state Prologue follows the prologue steps (not a ready player's scripted cutscene, the old "host shows
  prologue" glitch).
- Summons: `HostMayRing` / `GuestMayRing` require nobody connected in Prologue; the direct summon and the
  title join refuse while another member is in its prologue (new wait word `member_prologue`, "Waiting: a
  party member is in the prologue"); the host's sign board shows no sign to or from a prologue member; a guest
  whose save was in its prologue when last seen does not title-join (plain Continue into its own world).
- No gather or bell while anybody is in its prologue (the guest logs `bell: waiting: X in the prologue`).
- Syncs held (queued, applied once both are ready): host flag changes, item grants, story replays; the host's
  own prologue cutscenes are not broadcast, a guest's are not sent up, the host refuses story_up in its prologue;
  no rot for prologue deaths; the late-joiner catch-up (opening skip, start flags, warp) is opt-in
  (`BB_PARTY_CATCHUP=1`) since it rewrote a prologue world; its time-of-day write waits too.
- Party page: "Waiting: X is in the prologue" (host: "2/3, Waiting: X is in the prologue") while the own
  prologue is over and another member's is not.
- Unit: party-start-test (#19 Dream rule, both finishing orders), party-dsummon-test, party-status-bridge-test.
  Live (2026-10-14, one PC, two test saves past the prologue put back into it with `BB_PARTY_TEST_PROLOGUE`,
  one run per order, screenshots every 10 s): **guest first** - the host in its prologue (12101021 off): the
  guest's title join refused ("Waiting: the host is in its prologue"), the guest Continued into its own Dream,
  was ready and waited 56 s (`bell: waiting: Hunter0 in the prologue`, Party page "Waiting: Hunter0 is in the
  prologue"), no gather, no load; host ready -> guest rang 0.8 s later, in the host's world 4 s later.
  **Host first** - both in their prologue, the host done after 60 s: Party page "2/3, Waiting: Hunter1 is in
  the prologue" for 65 s, no bell, no summon; the guest done -> it rang at once and was in the host's world 3.9 s
  later. Screenshots: both alone in their own Dreams while waiting, no load, no glitch; the guest beside the
  host after the join. **Not live:** the Dream rule itself (weapons taken, still in the first Dream visit) -
  the test save's Doll flag 12100105 is set back by the game at once (Insight), so that state could not be
  forced; a fresh prologue cannot be driven (BB_TEST_NEWGAME stops at the contract screen). Unit-tested.

### Extra problems seen in these logs (section A scope)

- **Host party watchdog kicks a stuck joiner twice** (`H1:3694` "no progress for 72 s (stuck summon)" after the
  host travelled to the Dream while the guest was joining in m24_01: `H1:3549 reports it joined` ->
  `H1:3630 host travel ... m21` -> stop; `H2:1938` the prologue case of bug 1). A host travel while a member
  is mid-join should wait for the join (or the join should follow the travel).
- **Host travelling while a direct summon is in flight** fails the summon (`H4:2012 gets room` -> `H4:2023
  host travel` -> `H4:2062 failure`; G4:16102 `travel ... ignored: in own world (not summoned)`). Hold host
  lamp travel (or the split prompt) for ~10 s after serving a dsum request.
- **Long host loads**: `H4:1855 host travel: arrived 40.5 s after the warp`, `H4:2591 arrived 59.9 s after the
  warp` (normal is 2.6-12.8 s). Both right after a member left / summon failed with the room being closed:
  the leave + load overlap.
- **Silent host refusals** (bug 1 note): G2 x3 and G5 x1 got `refused by the host` with no host log line.
- **Guest restarts while in the host's room** leave a held connection: `H1:4380 started a new game while in
  room 1003`, `H1:4389 no arrival within 180 s`, `H1:4428 refused (a crashed member's connection is still
  held)`; `H3:2647 left room 1001 (restarted)`, `H3:2654 no arrival within 180 s`.
- **Guest-only watchdog dumps**: `>>> WATCHDOG: NO PROGRESS FOR 5 SECONDS!` 198 times on the guest PC
  (G1 7, G2 37, G3 76, G4 23, G5 55) and 0 on the host; threads `CSChrThread`, `EzWorkPool_TypeB1-3`,
  `FMOD nonblocking` BLOCKED >3 s; the guest also calls `__error` ~700k times per run (G1:13801 `697908
  calls`). Likely long hitches on the guest's PC (slower disk / shader compile / memory pressure: `Texture
  cache: memory pressure` lines); not a party bug but it lengthens every join and load. Worth one look at a
  dump (outside section A).
- **Host roster state "prologue"** for a ready host (`G4:21464 roster: visy(slot 0, prologue, ...)` after
  READY) - cosmetic, but the Party page shows it.
- The guest's own map id stays `-` (0xffffffff) for a few ticks after `world up, loading no` (G4 5530-5641,
  root of bug 9): any other code that keys off `s.map_id` right after a load has the same race.


## Cluster B: rest, deaths, loot after deaths, bloodstains (bugs 3, 6, 7, 12, 13, 18)

Sessions (log name = end time): H1 `visy_host/launcher_20261013_183856`, H2 `..._184410`, H3 `..._192054`,
H4 `..._193023`, H5 `..._193033_last` (crash 20:38:10, restart at line 11028); G1 `guest/launcher_20261013_184415`,
G2 `..._185717`, G3 `..._192049`, G4 `..._193046`, G5 `..._194949`. The guest's logs for H1 and for
H5 after line 4076 (19:49:49 onwards: the guest left and joined again at H5:4091, and again after the crash at H5:11691) are missing.

Fix direction (user): what matters most in play comes first (warping players around, a world that does not reset, broken death
warps and occlusion, wrong rejoin spawns). Where a vanilla loading screen fixes something, the loading-screen path is the fix;
the seamless (no-load) variant stays only as a later opt-in.

None of the code paths below changed between 0.6.0 (8143fda) and head aeeb408: `git diff 8143fda..HEAD` changes nothing
in `HostDeathIntercept`, `ReviveLocal`, `SendHomeVeto`, `GuestBloodstain` or `party_split.cpp` (party_split.cpp is not in the diff at all).
The only nearby 0.6.1 entries are "Deaths no longer bring enemies back" (enemy census, H4: it confirms that the no-load
behaviour is intended) and the kill-echo payment, and neither touches these bugs.

### Bug 3: only host resting resets world (and show fast travel instead of back to hunter's dream?)

**Evidence**
- Every world reset in the session was a host rest: `Party phantom: sent rested #...` at H3:2684, 3603; H4:1600, 2096;
  H5:2958, 3827, 6089, 8160, 9692, 10401, 13254, 13418. On the guest each one shows as `host rested #...: refill after the follow load`
  (G3:4356, 40358; G4:9160, 18711; G5:42865, 76263).
- When the guest used a lamp, the split question was asked, and the guest answered NO 5 times:
  G1:7814, G2:50179, G2:52017, G3:24483, G3:26974 `Party split: lamp 2102950 (guest): NO -> travel alone`, followed by
  `guest: travels alone (lamp) to m21_00_00_00, the host is in m24_01_00_00: alone load in host-world mode (K7 armed)`.
  Only the guest loads. The host's world, and so its enemies, is not reset.
- The one YES works: G3:40355 `(guest): YES -> travel with the party` led to H3:3603 `sent rested #1791906293271`, a party rest.
- 3 more guest questions were never answered: G2:31090 to 40589, G3:5268 to 15344 and G5:66384 to 73619, each ending in
  `no answer in 120 s; nothing runs`. All 3 were asked at the moment the host travelled to the same lamp
  (G3:5321 `guest: host travels (#... lamp 2412951`, G2:31126, G5:66408). The question was left open across the B2 load (see the extra problems).

**Cause**: a design gap in `party_split.cpp` (the question) and `party_guestsvc.cpp`/`party_b2.cpp` (guest lamp use).
The question mixes "rest" and "travel". NO means the guest travels alone with no reset for anyone. YES means a party move.
ERSC_PARITY D9 / row 39 say a rest resets the enemies for everyone, but a guest's rest only does that through YES.
The "fast travel" request: Bloodborne lamps outside the Dream only go to the Dream (headstones do the lamp-to-lamp travel). This is a UX wish, not a bug.

**Fixed on head**: no (party_split.cpp did not change after 0.6.0; there is no CHANGES 0.6.1 entry).

**Severity**: major (a world that does not reset is one of the high-impact play problems).

**Proposed fix (loading-screen path)**
- When a guest rests at a lamp (YES or NO), the host gets `{"op":"rest_request"}` and runs a real rest load for its own world:
  a host load to its current spot, with the forced transform = its own position, so nobody is warped.
  This uses the same loading screen and enemy reset as vanilla, and the guests follow with B2 as for any host load.
  A guest who answered NO still travels alone afterwards.
- Change the question text to "Rest: the world resets for the party. Travel with the party?" so the reset is clear.
- Lamp-to-lamp "fast travel" from a lamp: defer it (design only).

**Files**: party_split.cpp, party_b2.cpp (host rest load on request), party_guestsvc.cpp.

**Verify on one PC**: `instances.py run --count 2 --party --ienv 0:BB_PARTY_ENEMY_CENSUS=1 --ienv 1:BB_PARTY_SPLIT_TEST=no --ienv 1:BB_PARTY_DIRECTOR_TEST=host_kill=0@40,guest_lamp=2412951@70`.
Expected after the fix: the host logs a load and `sent rested`, and the census shows the killed enemy alive again after the load (no `RESPAWN without a load`).
Check with shot.py screenshots of both games.

### Bug 6: host death prevents guest loot (related to death warp)

**Evidence**
- G5: the host died at G5:70428 `tag visy hidden: dead`. At G5:70625 `guest: the host was revived in place (no load)` and G5:70630
  `guest: send-home at +0x1381a93 dropped (B2: the host's respawn travel follows)`, but no respawn travel follows (the host was revived in place).
  After that the guest has no `map item pickup` for about 5,700 lines until its next loading screen (G5:76376), although it kept killing enemies
  (enemy drop rolls at G5:71960, 72277, 72356, 73562, 73638, 74009). Its last pickup was at G5:70514, before the revive.
- G3: the same pattern. G3:95719/95724 (revive, send-home dropped), then 7 enemy drop rolls (G3:96957 to 106613) and no pickup
  until the log ends at G3:109246. The last pickup was at G3:88775.
- Host side over the same span: the host picks up normally (H5:3590+ lines 124/155/160 `map item pickup on this game (role 3)`), so the problem is guest-only.

**Cause**: a bug from the K9 veto. In the guest's Lua, HostDead (0x1381870) runs HostDead_1 (0x1381A60, travel.md:221):
`*0x553B0D8 = 2; GSM+0x1520 = 1`, then the send-home call at 0x1381A93. `SendHomeVeto` (party_b2.cpp:432) drops only the call.
It assumes a host respawn load will follow, as it did before 6.13. Since the host is revived in place (`HostDeathIntercept`,
party_b2.cpp:3086, "no stage request, no party move"), no load ever comes. The guest's death/leave state machine therefore stays
half-done and, until the next load, blocks the action-button interactions (pickups, prompts). This is a likely mechanism, not yet confirmed by RE.

**Fixed on head**: no.

**Severity**: major (the loot is lost or blocked for minutes, and every host death triggers it).

**Proposed fix**: one fix for bugs 6, 7, 12 and 18 (the "host death load" batch). A host death becomes a loading-screen respawn again: set the default of
`BB_PARTY_B2_HOST_REVIVE` to 0 (the existing, live-tested path: "the host ... moves the whole party to its lamp again (one load for everyone)").
The guest's dropped HostDead_1 send-home is then followed by the respawn travel, as K9 expects, and the load clears the half-done state.
This warps the guests to the host's lamp. If that is not wanted, a follow-up (opt-in) is a host respawn load with each guest's B2 follow load
keeping the guest's own position (forced transform = the guest's current spot).
The seamless revive in place stays as an opt-in (`BB_PARTY_B2_HOST_REVIVE=1`) and only after the guest side is fixed:
on `host_revived`, restore `*0x553B0D8` and GSM+0x1520 to their values before HostDead_1 and reset the guest's HostDead Lua state (needs RE first).

**Files**: party_b2.cpp (the HostReviveOn default, SendHomeVeto), docs/party/DEBUG_SWITCHES.md, b2_seamless.md.

**Verify**: `instances.py run --count 2 --party --ienv 0:BB_PARTY_DIRECTOR_TEST=host_die@60 --ienv 1:BB_PARTY_DIRECTOR_TEST=host_kill=0@75,guest_obj=<item entity>@90,guest_press=0x4000@92`.
Expected on the guest after the fix: `loading screen up` after the host's death, then `map item pickup on this game (role 6)`.
Check with screenshots of both games.

### Bug 7: host death warp breaks world / occlusion (should use loading screen reset like vanilla)

**Evidence**: every party-time host death was a revive in place with a chr warp and no load:
- H3:4844 `host death taken: no stage request, no party move; revive at lamp 2412951`, then H3:4847 `after: ... pos (-207.5,-26.8,-24.3) drawn (-193.4,-28.6,68.5)`.
- H5:3622 (lamp 2412952, from (-159.4,-27.0,28.7)), H5:6019 (from (-71.9,-51.3,29.5) to (-193.4,-28.6,68.5), about 130 m), H5:10184 (Old Yharnam
  lamp 2302950 from (62.1,-99.6,17.6)) and H5:13134 (post-crash, Old Yharnam, from (-37.8,-108.5,-81.3), about 190 m).
- The camera or chr is put on a lamp up to about 190 m away inside the loaded map, with no map/draw-group reload. The world is not reset either (by design, the H4 census).
- The "revive: a stage request after the revive; ChrCtrl death bits set again for the load" workaround (H5:3825, 6086, 10398, 13252) shows the revived chr is still in a half-dead state until the next load.

**Cause**: a design choice (C2 "host revived ALONE at its lamp in the loaded map, no load", ERSC_PARITY row 97, b2_seamless.md 6.13).
The revive (`ReviveLocal`, party_b2.cpp:3160) is a chr warp (+0x5F8), and the map's draw groups/occlusion and collision hit follow the
chr only after a load or a walk across hit parts. The visual glitches come from this; the players expect vanilla.

**Fixed on head**: no.

**Severity**: major (on the play-impact list: broken death warps and occlusion).

**Proposed fix**: the same as bug 6. Default `BB_PARTY_B2_HOST_REVIVE=0`, so a host death is a real respawn load with the vanilla loading screen,
a world reset and correct occlusion. Update ERSC_PARITY C2/H4 and the README party section. The seamless revive stays only as an opt-in.

**Files**: party_b2.cpp, docs/party/ERSC_PARITY.md, DEBUG_SWITCHES.md, README party section.

**Verify**: `--ienv 0:BB_PARTY_DIRECTOR_TEST=host_move=<far point>@50,host_die@60` in Central Yharnam, then shot.py
`--every 2` on both games. Expected: `travel #... host_death sent to the party`, `loading screen up` on both, the host on the lamp with correct geometry.

### Bug 12: item prompt on guest stuck on screen at host death

**Prompt part fixed (pt1items, 2026-10-14):** `ItemGetWatch` (party_items) clears the item-get list the way its confirm does once it is left over a load, the player is dead for 3 s, or its confirm check is held for 20 s (`item-get dialog cleared (...)`, `BB_PARTY_ITEMGET_WATCH=0` off). Unit-tested (party-items-test `test_itemget_watch`); live, both runs ended before the `host_die` step fired (it needs a fresh (re)join count, `host_die@S#N`), so the clear after a host death is not yet seen live. The split-question part stays with batch 1.

**Evidence**
- G5: the guest's last pickup prompt was at G5:70514 (`map item pickup on this game (role 6): item handle 0x2`), between the host's death (G5:70428) and the
  revive (G5:70625). Then the send-home was dropped (G5:70630) and there was no load until G5:76376.
- The same handle 0x2 pickup at G3:88775 came before the G3 host death.
- A second open "prompt" on the guest at the same time: G5:66384 `asking "Travel with the party?"` stayed open (Ask::Open, dialog dumps
  G5:70513 x13) until G5:73619 `no answer in 120 s`, across the host's travel load (G5:66408) and the host's death.

**Cause**: most likely the same as bug 6 (the HostDead_1 state left half-done by the K9 veto keeps the action-button/item menu up).
A contributing cause is party_split.cpp: the split question is not cancelled when a load, a host travel or a death comes in (it stays Open for 120 s).

**Fixed on head**: no.

**Severity**: major (the prompt cannot be dismissed until a load).

**Proposed fix**: (1) the bug 6 fix (a host death load). (2) In party_split.cpp `Ask::Open`, call `finish()` and close the dialog
(as the 120 s timeout does) when `s.loading`, a host travel arrives, or the local or host chr dies. Also lower the timeout to about 30 s.

**Files**: party_b2.cpp, party_split.cpp.

**Verify**: `--ienv 1:BB_PARTY_DIRECTOR_TEST=guest_lamp=2412951@50 --ienv 0:BB_PARTY_DIRECTOR_TEST=host_die@52`, without BB_PARTY_SPLIT_TEST
(the question stays open). Expected: the guest logs the question closed at the load, and the screenshot shows no prompt.

### Bug 13: blood echoes dropped not shown for host at least once, couldn't recover

**Evidence**: host bloodstain records (`own world: bloodstain flag 1, record N echoes at ...`; `record -1` = taken):
- H3:4844, 2910 echoes. The next session shows record -1 with echoes 3875 (H4:838), so it was taken.
- H5:3622, 1096 echoes. H5:3870 shows record -1 and echoes 1394, so it was taken.
- **H5:6019, 4843 echoes** at (-71.9,-51.3,29.5) Central Yharnam. The record stays through the CY loads at H5:6239, 8493 and 10001 and is never taken.
  Death 3 (H5:10184) overwrote it, so 4843 echoes were lost. This is most likely the one in the bug note.
- H5:10184, 2341 echoes. H5:10443 shows record -1 and echoes 2651, so it was taken.
- **H5:13134 (post-crash), 5395 echoes** at (-37.8,-108.5,-81.3) Old Yharnam. The record is still there at the log's end (H5:13555, echoes 141), so it was not recovered.

**Cause**: not certain. Candidates:
- (a) Vanilla: a nearby enemy takes the echoes (its eyes glow) and no stain shows on the ground.
- (b) Revive in place: the stain object is made live by 0x13D2910 at death time. The host was moved away about 130 m with no load, and the first death 2 load
  came only at the lamp/Dream travel (H5:6095). After that, the CY loads in host mode with a session should spawn the stain from the record, which is unverified with cooperators present.
- (c) Occlusion/draw group after the warp (bug 7) hid it.

**Fixed on head**: no.

**Severity**: minor (possibly vanilla; there are 2 of 6 cases).

**Proposed fix**: first make (a)/(b) testable. The bug 6/7 load path removes (b) and (c), because the stain then comes from the load as in vanilla.
Add a log line when the record changes to -1 (taken) and log the enemy holder (if the record has a holder field), so the next playtest can tell (a) from a bug.

**Files**: party_b2.cpp (OwnStainLogTick).

**Verify**: `--ienv 0:BB_PARTY_DIRECTOR_TEST=host_die@60,host_move=<death spot>@90,host_press=0x4000@92`. Expected: `record -1` and echoes back,
and a screenshot showing the stain and "Acquire Blood Echoes" before the press.

### Bug 18: guest blood echoes cannot be taken if both died, but shown on ground

**Evidence**
- The guest recovered its stain when only it died: G3:32940 `bloodstain recorded: echoes 735 -> 0`, then G3:40360 `guest state ... echoes 735` (taken in the host's world).
  G3:20548 (639 echoes) was taken the same way. So b2_seamless.md:727 ("not shown in the host's world") is out of date.
- "Both died" happened twice, both with no guest log:
  - H5:10166/10171: host death 3 and the guest down in the same second. The host intercept had already counted `1 member(s), 0 down` (H5:10184), and only then
    `slot 1 is down` (H5:10210). The host was revived in place instead of taking a wipe respawn.
  - H5:13107/13209: post-crash, the host died and was revived, then the guest died, followed by a host travel at H5:13252 while the guest was down.
- In both cases the guest had also seen the host die and its HostDead_1 send-home was dropped with no load (bug 6 state). Its pickups, its own stain included, were blocked.

**Cause**:
- (1) The same as bug 6: the guest's half-done HostDead_1 state blocks the "Acquire Blood Echoes" action.
- (2) A wipe race in `HostDeathIntercept` (party_b2.cpp:3086): `member_down` arrives a few hundred ms after the host's own death, so `dead >= members` fails, and a near-simultaneous double death is handled as "host alone" instead of a wipe.

**Fixed on head**: no.

**Severity**: major (the guest's echoes are lost).

**Proposed fix**:
- (1) The bug 6 fix (a host death load).
- (2) Decide wipe vs revive at the revive tick (1 s later, `ReviveHost`) by re-checking `DeadMemberCount()`. If every member is down, return to the respawn travel (a wipe, everyone loads).
- (3) Optional, needs live check: after a guest load in host-world mode, if the own record's map equals the loaded map and no stain object exists, re-run 0x13D2910-style stain creation with GSM+0x1592 = 1 (as GuestBloodstain does).

**Files**: party_b2.cpp.

**Verify**: `--ienv 0:BB_PARTY_DIRECTOR_TEST=host_die@60 --ienv 1:BB_PARTY_DIRECTOR_TEST=guest_die@60,guest_move=<stain>@100,guest_press=0x4000@102`.
Expected: `host death: every member is down too` (a wipe), a load for both, then the guest's echoes back (0.6.1 Echoes change log line).

### Deaths list

| # | Log:line | Who | Context | What followed |
|---|---|---|---|---|
| 1 | H1:1072 | host | game 1 prologue, solo | `host_death` travel to the Dream (not sent: nobody in world), load; stain 475 (H1:1118) |
| 2 | H1:4763 | guest | game 1, Central Yharnam (guest log missing) | H1:4790 `slot 1 was revived in place (no load)` |
| 3 | G1:12201 / H2:1921 | guest | game 2, CY lamp 2412950 | G1:12202 `no bloodstain (echoes 0)`; G1:12373 revived at own lamp, no load; then H2:1940 host watchdog left the session, so G1:12637 `refused by the host: the host ended the party` |
| 4 | H3:1019 | host | game 2, solo before the join | `host_death` travel, load to the Dream; stain 240 (taken later) |
| 5 | G3:20547 / H3:3132 | guest | CY | stain 639 at (-219.8,-40.4,72.0); G3:21715 revived at own lamp 2412951, no load; taken (G3:26979) |
| 6 | G3:32939 / H3:3410 | guest | CY | stain 735; G3:33104 revived, no load; taken (G3:40360 echoes 735) |
| 7 | G3:67116 / H3:4245 | guest | CY | stain 1743; G3:68295 revived, no load; taken by G4 (echoes 3549, record -1) |
| 8 | H3:4825 / G3:95547 | host | CY | revived in place at lamp 2412951 (H3:4844); guest send-home dropped (G3:95724); **guest pickups blocked until end of log**; stain 2910 taken |
| 9 | H5:3605 / G5:70428 | host | CY | revived in place at lamp 2412952 (H5:3622); guest send-home dropped (G5:70630); **guest pickups blocked until its load at G5:76376**; stain 1096 taken |
| 10 | H5:4453 | guest | CY (guest log missing) | H5:4494 revived in place; `slot 1 is down` H5:4473 |
| 11 | H5:5994 | host | CY | revived in place at lamp 2412951, about 130 m warp (H5:6019); stain 4843 **never taken, lost at death 13** |
| 12 | H5:10171 | guest | Old Yharnam, near-simultaneous with #13 | `slot 1 is down` after the host's intercept (H5:10210); H5:10236 revived in place |
| 13 | H5:10166 | host | Old Yharnam | revived in place at lamp 2302950 (H5:10184, "0 down": wipe race); stain 2341 taken; host travelled to the Dream at H5:10398 |
| 14 | H5:10919 | guest | Old Yharnam (guest log missing) | H5:10945 revived in place; host crash at 20:38:10 (H5 crash log) about 100 lines later |
| 15 | H5:13107 | host | post-crash, Old Yharnam | revived in place at lamp 2302950 (H5:13134); stain 5395 **never taken** |
| 16 | H5:13209 (x2) | guest | Old Yharnam, right after #15 | `slot 1 is down` (H5:13240); the host rested/travelled to the Dream at H5:13252 with the guest down (dead-follow load); bug 18 |
| 17 | H5:13675 | guest | post-crash (guest log missing) | H5:13697 revived in place |

### Extra problems found (cluster B)

- **Guest split question left open across loads** (party_split.cpp): G2:31090, G3:5268 and G5:66384 were asked as the host travelled to the same lamp; each ran 120 s to "no answer"
  with the dialog stale. The host's own question also stays open while the host roams, fights and picks up (H5:10227 to 10397, about 170 lines with pickups and kills).
- **Wipe race** (H5:10184): the host death intercept decides with "0 down" a moment before the guest's `member_down` arrives (H5:10210).
- **Rot classification**: H5:6072/10228 `lamp warp 19 s after a death: the respawn, not a rest`. Here the host used a lamp after a revive in place (no respawn warp was due),
  so the balance code took a real lamp use for the death respawn (party_balance.cpp, minor).
- **Doc out of date**: b2_seamless.md:727/748 says a guest's stain is not shown or takeable in the host's world. G3 shows it is taken when only the guest died (and the host was alive).

## Cluster C: items and loot (bugs 4, 15, 16)

### Bug 4: enemy loot from guest not synced to host ("roll should be the same for all always")

**Evidence.** Both games roll every enemy death on their own: host `Party: world: enemy drop roll on this game (role 3)` x243 (H3 `launcher_20261013_192054.log` 57, H5 `..._193033_last.log` 186), guest `(role 6)` x101 (G2 4, G3 51, G5 46). Every session's startup shows the mode: `Party: world: guest enemy drops: full, own roll per player` (e.g. H1 `launcher_20261013_183856.log:472`), `Party items: full enemy drops for clients (+0x17dd191): applied` (H5:11459). No error or skipped drop is logged on either side. The guest saw an item drop that the host did not get, which is how instanced drops work.

**Cause.** This is a design decision, not a bug. ERSC_PARITY.md row "Item pickups and enemy drops: Instanced per player ... every enemy drop is per player too (changed 2026-10-11)", and D4 "Enemy drops per player". Each game runs the game's own roll `0x17CEAF0` with its own RNG (`GSM+0xA78/+0xA80`, items.md 2.2/6.3), so the rolls differ. `EnemyDropEntry` (`party_worldact.cpp:1630`) only logs. The players are asking for a *shared roll* (same items for everyone, each player still gets its own copy). ERSC itself rolls per player, so this would be a design change.

**Fixed on head?** No. Nothing in `git log 8143fda..HEAD` or CHANGES 0.6.1 touches drop rolls. The kill-echo payment (dc2f93a) concerns echoes, not items.

**Severity.** Minor (a feature request against a deliberate design). Decide this before writing any code.

**Fixed (pt1items, 2026-10-14, user decision: one shared roll).** `party_droproll.cpp`: the 0x17DD0B0 roll runs with the game MT19937 (GSM+0xA78) saved, seeded from (host salt, host world generation from EVENT `drop_seed`, map, entity +0x1F0, lot) and restored; the guest's request uses the host's item discovery. Live: 4 host kills logged the same seed and the same result on both games (`goods 1000 x2` for entity 2410202). `BB_PARTY_SHARED_DROP_ROLL=0` = old own rolls.

**Smallest fix (if accepted).** Make the roll deterministic per enemy rather than synced: in the existing drop-roll entry hook (0x17DD0B0, `EnemyDropEntry`), save the game RNG state, seed it from `hash(map entity id of the dying chr, host world epoch: rest counter / NG cycle)`, let the roll run, then restore the RNG in a matching exit hook. No message is needed because every game sees the same entity id and the same epoch. Keep the per-player pickup (each player takes its own copy). Gate it with `BB_PARTY_SHARED_DROP_ROLL` (default to be decided). Update ERSC_PARITY D4 and items.md.
Files: `party_worldact.cpp` (hook plus RNG save/restore), `party_items.h/.cpp` if the epoch helper lives there, docs.

**Verify on one PC.** `tools/mp/instances.py run --count 2 --party --ienv 0:BB_PARTY_DIRECTOR_TEST=host_kill=0@40,host_kill=0@50,...` (host kills nearby census enemies). Compare the logged roll results per chr handle on both instances (add a log line `roll result lot/items` in the hook) and expect identical item lists for each entity. Then repeat after a rest: the items should differ from the first pass but still match between the two games.

### Bug 15: boss loot prompt frozen on the guest's screen, cannot dismiss; "didn't get the key items?"

**Fixed (pt1items, 2026-10-14).** `ItemApplyState` gained alive / cutscene / boss_quiet (10 s after the boss bar goes); `GuestItems::Step` holds popups with a logged reason (`item popup held (boss defeat banner)`), and `ItemGetWatch` clears the native list (the 0x16D86E0 confirm path, `ClearItemGet`) when it is left over a load, the player is dead 3 s or the confirm check is held 20 s. Live: the guest's grant held through the Cleric Beast banner, given after; key items 31000 / 50000010 given while summoned.

**Evidence.**
- Cleric Beast (G5 `guest/launcher_20261013_194949.log`). Host: H5:2873 boss HP 0 -> 2886 `award lot 50000010 (hostOnly 1, flag -1) in the own world`, 2889 `host award #0 lot 50000010 ... queued for the party; items [4:4114x1]`, 2893 `items: 1 lots sent to the party`. Guest: 40203 `guest got 1 grants from the host`, 40207 `gave #1791909040364 lot 50000010 ... while summoned: goods 4114 x1 (held 0 -> 1, item-get dialog)`, 40209 `boss health bar: off (a boss defeat...)`, then 41537/41539 `send-home ... dropped (B2: the party stays after the boss)`. The Sword Hunter Badge **was given**. Our push of the native item-get dialog happened in the same second as the boss defeat (before the "Prey Slaughtered" sequence ended).
- Gascoigne (H5:8035-8062): `award lot 31000` -> `queued for the party; items [4:4000x1]` -> `items: 1 lots sent to the party`. The guest was `in_host_world` (roster lines). The guest log for this part (host lines 4090-11028, guest rejoined from <guest address>) is **missing** from guest, so we cannot see whether the dialog froze there or the item was given.

**Cause (likely).** The summoned give `ApplySummoned` (`party_items.cpp:1665`) adds the item with GiveItemDirect and then calls `ShowItemGet` (`game_state.cpp:385`). That function pushes an entry onto the native item-get list (`0x16D8850`) behind the game's back. Per items.md 10.2 the dialog is modal and clears only on the confirm check `0x1CD91F0`, which is gated by a player-chr flag (`WorldChrMan+0x60 -> +0x58 -> +0x38 -> +0x41` bit 7) and `*0x5540050`.

The only apply gate is `Calm()` (`party_items.cpp:1991`: no HP lost for 15 s). It does not check:
- whether the player is alive (an HP-0 player counts as "calm"),
- whether a boss-clear banner, remo or menu is up,
- whether a host travel or death warp is imminent.

When the entry lands while that input gate is closed, the box shows but confirm is never accepted. A B2 load or PlayerIns re-creation (spectate, death, the host's travel) can then leave the entry orphaned on screen. This is the same mechanism as bug 12 ("item prompt on guest stuck on screen at host death").

**Fixed on head?** No. No commit since 8143fda touches `ShowItemGet`, `Calm` or the apply gate.

**Severity.** Major. The prompt cannot be dismissed, the screen stays blocked, and players believe they lost the key item.

**Smallest fix.**
1. Gate grants at the source. `ItemApplyState` gains `alive` (HP > 0), `no_remo`, a `boss_clear_quiet` (more than 8 s since the boss-bar-off event) and "no host travel pending". `GuestItems::Step` holds grants until all are true. Files: `party_items.h` (ItemApplyState), `party_items.cpp` (`ItemsTick` fills them).
2. Add a stuck-dialog watchdog in `ItemsTick`: if `*0x5562878+0x130 == 1` with a non-empty list for more than 20 s across a load or PlayerIns change, or while the player is dead, clear the list the way the confirm path does (call the list clear used by `0x16D86E0`, or pop nodes back to the free list). Log `item-get dialog cleared (stuck N s)`.
3. Once the dialog has been released, give the player its own notice line (the party HUD toast already exists) so the item is visibly in the inventory.

**Verify on one PC.**
`instances.py run --count 2 --party --env BB_PARTY_ITEMGET_PROBE=1 --ienv 0:BB_PARTY_ITEMS_HOST_TEST=31000 --ienv 1:BB_PARTY_DIRECTOR_TEST=guest_die@18`. The guest dies just before the host's award at +20 s. Expected:
- the guest log shows the grant held (`apply gate closed ... dead`) and given after the revive,
- the probe shows the dialog list going 1 -> 0 after a confirm,
- `tools/mp/shot.py` shows no box left on screen.

Repeat with `host_die` at +21 s on the host for bug 12.

### Bug 16: Oedon Tomb Key not synced to the guest; "same with other key items except from chest"

**Fixed (pt1items, 2026-10-14).** Every give / skip is logged; a boss-reward ledger lot (done flag boss_defeated) or a per-player gift whose done flag is set but whose unique goods are not held is given anyway (`LedgerSkipSuspicious`). Old Hunter Bell: a summoned guest's refused hostOnly Messenger lot (10000/10010/10050) is given to itself (`ItemSource::Own`); live run 2 (`BB_PARTY_GIFT_TEST=10050`, `..._HOSTONLY=1`): `own gift lot 10050 ... given to the guest itself`, flag 6670 set but bell not held -> re-give chosen, but the plan skipped the row (its flag is the done flag): fixed after the run (row flags ignored on a re-give, unit-tested), not re-run live. Live run 2 also re-gave the Sword Hunter Badge (ledger bit set, badge not held: `giving it after all` -> `held 0 -> 1`).

**Evidence.**
- Host H5:8051-8062: lot 31000 (goods 4000 Oedon Tomb Key, ledger 25 = flag 60009025) awarded and sent to the party while the guest was summoned. After the host crash and the rejoin it is in the full list again: H5:11875 `full item list for slot 1: 21 lots` (it was 20 at 4097 before Gascoigne). On the host side the sync worked both times.
- The guest-side logs for both deliveries are missing: G5 ends at 19:49:49, before Gascoigne. That session is host lines 4090-11028, and the post-crash rejoin is 11690-14104. We cannot tell from the guest whether it skipped, deferred or gave the key.
- The only other host-side key reward in the whole session, Cleric Beast's Sword Hunter Badge (lot 50000010), **did** reach the guest (G5:40207).
- Host `award lot` lines over all host logs: 10010/10000 (bells), 50000010, 10050 (Old Hunter Bell, Dream) and 31000. No other key item was awarded to the host, so "other key items" probably means treasure pickups. Those are deliberately per player (`host flag 524xxxxx: map treasure, per player (guests pick up their own)`, 40+ lines in H5), or the per-player Dream gift below.

**Cause (likely).** There are two candidates, both on the guest:
- (a) Bug 15: the key was given with an item-get box that froze, and the player read the frozen box as "not received". If the box was orphaned by a load before GiveItemDirect committed, the give was really lost.
- (b) The grant was handled in the restarted session. In the after-crash full list, the guest's own save already had 12411800 (Gascoigne defeated, written by C2 while summoned, H5:8057 `host flag 12411800 = 1 (boss_defeated)`). Ledger seeding (`LedgerBitsToSeed`, `party_items.cpp:178`) converts any **done flag** found in the own save into a ledger bit when `kItemLedgerSeeded` is not yet set. If the seeding marker was not set yet on that save (first party session on that character after an update or new game), the guest's own save got ledger bit 60009025 from the C2-written 12411800 and the key was skipped as "already given". items.md 6.2 describes exactly this class (12401800 -> 50000001 skipped), which was fixed for the order "seed before C2". That fix should be re-checked for a restart in which C2 wrote the boss flag to the own save in the *previous* session before that session saved its ledger marker.

**Fixed on head?** No. No item-path changes since 8143fda. gift_up (D5, 2984899) covers NPC gifts to a guest going up to the host, not host boss rewards going down.

**Severity.** Major. Oedon Tomb Key gates progress: without it the guest's own world cannot open the Oedon Tomb door.

**Smallest fix.**
1. Ask the players for (or keep from now on) the guest logs of every session. Ask the user for the guest's launcher logs after 19:49:49.
2. Make the guest log every grant decision at info level once per lot, including "skip ... ledger seeded from done flag" (today that path only says `already given (flag 60009025 set)`).
3. In `SeedLedgerFromOwnSave`, seed only from done flags that were in the own save **before the first party join on that character**. Write `kItemLedgerSeeded` into the own save in the same save request as the first C2 write, so a crash between the two cannot leave C2 boss flags in an unseeded save.
4. Fold in the bug 15 gate and watchdog.

Files: `party_items.cpp`, `party_items.h`.

**Verify on one PC.**
`instances.py run --count 2 --party --fresh-saves --ienv 0:BB_PARTY_ITEMS_HOST_TEST=31000 --ienv 0:BB_PARTY_ITEMS_HOST_TEST_FLAGS=12411800`, then `instances.py crash --instance 1 --after 25` (guest crashes right after the award), then the guest title-joins again. Expect `gave ... lot 31000 ... goods 4000 x1 (held 0 -> 1)` exactly once over the two guest sessions, and `skip ... already given` on the second. Check the guest's own save in its next solo Continue: goods 4000 held.

### Item-related extra findings

- **Guest logs missing for the host's middle and post-crash sessions.** Host H5 lines 4090-11028 (rejoin from <guest address>; Gascoigne kill, Oedon Tomb Key, Old Yharnam treasure) and 11690-14104 (post-crash rejoin, full list 21 lots) have no guest counterpart in `guest/` (the last guest log ends 19:49:49). Bugs 15 and 16 cannot be confirmed from the guest side without them.
- **The guest's own Old Hunter Bell is lost in the host's Dream.**
  - G5:76541 `award lot 10050 (hostOnly 1, flag 6670) in another's world, role guest`. The vanilla gate refuses hostOnly lots on a client (items.md 3, `0x17DDCD9..0x17DDD3E`).
  - Lot 10050 (Old Hunter Bell, goods 4312, talk t210720, campaign_start.md:52) is deny-listed as a per-player Messenger gift (`party_items.cpp:223`), so neither C3 nor gift_up replays it.
  - Meanwhile the guest's event flag 12101026 fires and is `kept local` (G5:63010, 77619), so the talk can look done.
  - Fix: for per-player Messenger lots (10000/10010/10050) refused on a summoned guest, award them through `ApplySummoned` as a local grant (source `Mark`-like, own pool), or route the talk's award through hostOnly 0. Minor.
- **The direct give relies only on `Calm()` (15 s without HP loss).** It ignores death, remo, loading edges and boss-clear banners (see bug 15). The same gate decides when the native item-get box pops for catch-up grants after every rejoin. Each join gets `guest got 19 grants ... 19 pending` (G1:1589, G2:1575, G3:1580, G4:1590, G5:1577), all of them benign `skip #N ... already given` (19 distinct lots x5 sessions, no failures).
- **The host's ledger seeding** (H1:1017 `ledger seeded from the own save (load mode 0): 0 lots ... (marker flag 60009999)`) works. No `NOT written`, `direct give is not available`, `not while summoned` or `waits for the own world` lines appear anywhere in the playtest logs, so the summoned give itself never failed where it was observed.
- **No gift_up traffic** (0.6.0 has no D5). No NPC talk gift to the guest is visible in G1-G5, so the 0.6.1 gift_up change is neither confirmed nor contradicted by this playtest.

## Cluster D: NPCs, story, fog (bugs 5, 8, 11, 14, 17)

**Status after pt1npc (2026-10-14, branch agent/pt1npc).** Live run (2 instances, save_backup
savedata_20261010, host `host_warp=2412953` + the Gascoigne CLEAR fixture, guest `guest_obj=2411810`
then `BB_PARTY_WORLDACT_TEST=chr=2410700/2410710/2410860/2410780`):
- #11 / #17: the guest's startup wrote all 42 talk-NPC rows (`Gehrman stays solid for guests
  (NpcParam 804000 isChangeWanderGhost): 128 -> 0`, the Dweller 113010 `192 -> 64`, Alfred
  6151/6155 ...). Not looked at on screen (screenshots mistimed).
- #5: `t241314 asks IsClientPlayer (NPC talk in the host's world: answered 0 (D8))`, Iosefka's door
  (2410700) prompt 10010200 + Cross -> guest `set 12410739(npc_quest)` -> host `slot 1 changed the
  host's world ... 12410739=1`; the fake doctor (2410710) prompt 10010400. The window residents
  (2410860 t241440, 2410780 t241319) were answered 0 too but showed no prompt at 16 spots around the
  chr: the window knock goes through the door dummy OBJ (2410870/71), which the fixture did not try.
  Next: `BB_PARTY_WORLDACT_TEST=obj=2410870/2410871`. The override counter and the label now log
  outside tests (once a minute).
- #8: the guest stood 3 m from Gascoigne's fog (2411810) for 20 s with 12411802/12414800/12411800 all
  0 and the notice did NOT fire (no log line); cause not found in the code. FogWaitTick now logs each
  in-block gate's state (`guest: boss fogs of this block: [event: read intro host dead fog near/...]`)
  on every change, so the next run shows which check fails.
- #10 (flags) and #14 (host guard): unit-tested only (party-worldact-test, party-story-test).

### Bug 5: guests cannot knock on doors (all party members should see plot dialogue)

**Evidence**
- The guest's game did run the Central Yharnam window/door talk scripts in the host's world, and every time it asked `IsClientPlayer`. G1 `guest/launcher_20261013_184415.log:8183-8184`: `talk t241314 asks IsClientPlayer (as before)` / `t241313` (clinic door: fake doctor and Iosefka), with `session client (6, ...)` at :8175. G2 `..._185717.log:48694` `t241319` (Gilbert's window). G3 `..._192049.log:24279` `t241441`, `:49968` `t241440`, `:76838` `t241461` (the generic window residents). G5 `..._194949.log:17344` `t241320` (the little girl's window).
- No talk produced a quest flag in G3. Within ±400 lines of each G3 talk, no `world action sent to the host ... npc_quest` follows (`grep` around 24279 / 49968 / 76838), so the dialogue never moved on. In G5 some NPC flags did go up (e.g. `:28858 set 72410511(npc_quest)`), so world talk is not dead in general.
- `(as before)` is only a diagnostic label (`party_guestsvc.cpp:1716`). The real override counter `guest NPC talk in the host's world asked IsClientPlayer N times (answered 0)` is printed **only in test runs** (`LogLampTalks`, `party_guestsvc.cpp:1705`, `&& !g.test.empty()`). The logs therefore cannot show whether the override answered 0.
- Host side: nothing is logged. A talk the guest never completes sends nothing.

**Most likely cause** (bug; root cause not confirmed)
- The D8 world-talk override (`IsClientFilter`, `party_guestsvc.cpp:376-410`) should answer 0. `BB_PARTY_GUEST_TALK` was on (G\*:479 `guests can talk to NPCs in the host's world`), and `worldact::WorldTalkAllowed(2413xx/2414xx)` is true (`party_worldact.cpp:197-209`, map 241 is in 220..369 and is not a lamp).
- The ESD gets past `IsClientPlayer`, but the knock / talk still does not open. Two candidates:
  - (a) For the in-person NPCs, the NPC chr is a **wandering ghost on the guest** (see Bug 11/17). The MSB shows that Iosefka (`会話用_診療所の女医`, entity 2410700, talk 241313) uses NpcParam **6060** and the fake doctor (2410710, t241314) uses **6070**, and both have `isChangeWanderGhost = 1`. Chr type 4 means "no talk" (guest_services.md §2.5).
  - (b) The window residents (`汎用住人*`, `少女`, `重病人`=Gilbert, t241319/320/440/441/460/461) use NpcParam **901000** (no ghost bit). They talk through a door dummy object (`扉ダミーOBJ`). The cause there is another client gate: the dummy's action button or an EMEVD client branch. This needs a repro.
- Caveat: ERSC_PARITY D8 (2026-10-11) says Iosefka at the clinic door and Gilbert worked for a guest. So the failure may depend on state, for example a talk tried before the world-talk gate opened, or during the 4 s `item apply gate closed` window after a load.
- "Everyone should see plot dialogue" is a **design gap**. Talk is local to the talker: there is no subtitle or dialogue mirroring (ERSC does not mirror talk either). Only the resulting quest flags are shared (D8).

**Fixed on head aeeb408:** no. No commit since 8143fda touches the talk gate (`git log 8143fda..HEAD -- gpu/shim/party/party_guestsvc.cpp party_worldact.cpp` has only D5 gift_up / B2 fog). D8 Follow (3001f6b) only syncs quest states.

**Severity:** major. NPC quest lines such as Gilbert, the girl and Iosefka are unreachable for a guest.

**Proposed fix**
1. Diagnostics first, which is cheap: make `LogLampTalks` log the world-talk override count outside tests at a low rate (once per talk id), so the next playtest shows "answered 0".
2. Repro on one PC with 2 instances (`tools/mp/instances.py`): host + guest in Central Yharnam, guest fixture `BB_PARTY_WORLDACT_TEST=chr=2410860/2410862/2410780/2410760/2410700;after=10`. That stands at each window and waits for an action prompt. Expect the prompt plus `world action sent to the host ... npc_quest` on the guest and `slot 1 changed the host's world` on the host.
3. If (a) holds, apply the Bug 11/17 param fix. If (b), trace the dummy OBJ's EMEVD: run `flag_tool.py emevd` on m24_01 for 2410870/2410871 (the door dummies) and look for a client-only branch, then add an `EmevdSkipRule::client_only` like `12100110:2003:17@26`.

**Files:** `gpu/shim/party/party_guestsvc.cpp`, maybe `seamless_rules.cpp` (shared with Bug 11/17).

### Bug 8: guest saw fog wall, couldn't interact until host activated boss / fog

**Evidence**
- 0.6.0 had no guest fog-first logic. No guest log has a fog line (`grep -i fog` is empty apart from patch names).
- Cleric Beast: the host started it, H5 `visy_host/launcher_20261013_193033_last.log:2608` `boss health bar shown: a boss fight (event 12414702, entity 2410800)`. The guest followed 0.x s later, G5 `..._194949.log:28947` (same event), and sent `:28949 world action ... set 9300 9350 12411702 12411703`.
- Gascoigne: the host's fog / intro is at H5:7560/7605. The guest log for that period (after 19:49:49) is missing, so the wall the players saw was probably at Gascoigne or at Cleric Beast before the host entered.

**Most likely cause:** design gap in 0.6.0. A client's `ゲストがボス部屋入場` event waits for the host-entered flag (vanilla co-op rule).

**Fixed on head:** partly.
- B2 "anyone can go through a boss fog first" (eafce0a, 8df2d85, 1f981e9; CHANGES 0.6.1; ERSC_PARITY B2; `seamless_rules.cpp:496-551` `g_fog_first`, `BB_PARTY_GUEST_FOG_FIRST`). Live: Cleric Beast rematch.
- Left: "boss-alive and **intro-seen checks stay**". A first fight with an intro cutscene still starts with the host (CHANGES: "First fights with an intro cutscene still start with the host"). That applies exactly to Gascoigne (intro 24010010) and to every first-time story boss. There the guest still faces a wall it cannot pass.

**Severity:** minor on head (major in 0.6.0).

**Proposed fix:** for intro bosses, let the guest's fog report (`world_action fog`) make the **host** play the intro (the host's event plays 2002 and sends it via story) before opening the fight, instead of refusing. Alternatively, show the guest a notice ("Waiting for the host: first fight starts with an intro"). Smallest change: the notice in `party_hud` plus the boss-gate refusal path in `seamless_rules.cpp`.

**Verify (one PC):** 2 instances, Gascoigne fixture from campaign_soak.md row 3 (`host_warp=2412953`, `BB_PARTY_PROGRESS_TEST_CLEAR=12411800,12411802,...` as in ERSC D10). Walk the guest through the fog with `guest_stick=obj:<fog entity>:...` and expect the guest-first path or the notice.

**Files:** `seamless_rules.cpp`, `party_worldact.cpp` (B2 fog), `party_hud.cpp`.

### Bug 11: Gherman is in ghost state for guest

**Evidence:** no log line; the game does not log chr type. MSB data (read-only, `flag_tool.load_params` + MSB parts, game files `E:\games\bloodborne\dvdroot_ps4`): `m21_00_00_00` part `NPC_拠点老人` (Gehrman in the wheelchair) has entity **2100600**, NpcParam **804000** and talk **t210305**. NpcParam 804000 has **isChangeWanderGhost = 1**, ghostModelId 8045. guest_services.md §2.5 already notes "804000 too".

**Cause:** the same mechanism as the Doll (`seamless_rules.cpp:325-330`). On a client, chr creation `0x18E4F94..0x18E4FDC` turns any NPC whose NpcParam byte 0x145 bit 7 is set into chr type 4 (translucent, no collision, no talk). Only row 803000 (the Doll) is cleared. This is a bug (incomplete rule list).

**Fixed on head:** no. `seamless_rules.cpp` on aeeb408 still lists only 803000 (`grep 804000` finds only the comment and docs).

**Severity:** minor (visual). Gehrman's talk t210305 is deliberately not a guest service (`party_guestsvc.h:70`: "Not: Gehrman 210305 (the last choice)"), so making him solid changes only the look.

**Proposed fix:** add `{"Gehrman stays solid for guests (NpcParam 804000 ...)", "NpcParam", "NPC_PARAM_ST", 0x184, 804000, 0x145, 1, 128.0f, 0.0f, "BB_PARTY_GUEST_SERVICES"}`. Better still, one rule per talk-NPC row; see Bug 17.

**Verify:** 2 instances, guest summoned into the host's Dream. Check the startup line `Party seamless: ... NpcParam 804000 ...: 128 -> 0`, then screenshot the guest (`tools/mp/shot.py`): Gehrman should be opaque. An `entprobe=2100600` / chr type probe should read 5, not 4.

**Files:** `gpu/shim/party/seamless_rules.cpp` (+ guest_services.md §2.5).

### Bug 14: Father Gascoigne cutscene played twice host and guest

**Evidence (host H5 `launcher_20261013_193033_last.log`)**
- `:7560` `host #1791911654615 cutscene 24010010 (mode 2, 2002[3], event 12411802 ...)`, then `:7561` sent to the party, then `:7595 remo ended after 47.4 s`.
- `:7605 boss health bar shown (event 12414802, entity 2410810)`. The fight had already started.
- `:7607 a guest's cutscene (D10): remo 24010010 mode 0 ... -> queued`, then `:7608 host: slot 1 triggered cutscene 24010010 (event 12411802): played here and sent to every guest`, then `:7609 host #...616 cutscene 24010010 (mode 2, 2002[1] ...)`, then `:7633 remo ended after 12.2 s` (second play, during the boss fight, re-broadcast to the guest).
- The guest-side log for this period is missing (G5 ends at 19:49:49).

**Cause:** bug, fixed after 0.6.0.
- The guest watched the host's live mirror. Then its **own** copy of host-world event 12411802 ran its 2002 on the guest. `RemoHandlerEntry` (`party_story.cpp:931-960`) sent it up as `story_up`, and the host's `HostPlayGuestStory` (`party_story.cpp:1686-1714`) played it again for everyone.
- In 0.6.0 the client-side dedupe (`ClientIntroSkip` → `StoryDedupeSkip`, `party_b2.cpp:2267-2280`) was opt-in.

**Fixed on head:** yes. D10 a5c134f / 025b0a9 turned `BB_PARTY_STORY_DEDUPE` on by default, and a5033c3 verified it live on exactly this scene: "Gascoigne intro 24010010 ... the guest's own copy of the event then logged `event 12411802's 2002[3] skipped: its cutscene already played as the live mirror`" (ERSC_PARITY D10, CHANGES 0.6.1).

**Residual gap (not fixed):**
- `HostStoryFromGuest` / `HostPlayGuestStory` have no host-side guard. If the guest missed the mirror (loading, not in the map, the 10-min `kMirrorDedupeSeconds` expired), the host still replays a scene it has just played, even in a boss fight. The host's boss bar was up at :7605 before the replay at :7609.

**Severity:** major in 0.6.0, minor on head.

**Proposed fix (belt and braces):** in `HostStoryFromGuest` (`party_story.cpp:~1665`), refuse a `story_up` when either holds:
- the host played the same remo or event in the last 10 min, or the event's flag (12411802) is already 1 on the host;
- or the host is in a boss fight (A11 state).

**Verify:** the ERSC D10 fixture (2 instances; `BB_PARTY_PROGRESS_TEST_CLEAR=12411800,12411802,12411803,12414800,12414801,12414802,12414223,2412,9457,2410`, `host_warp=2412953`, `host_move` into region 2412815). Run once with `BB_PARTY_STORY_DEDUPE=0` on the guest to force the up-path. Expect a host log `story_up from slot 1 refused: already played`.

**Files:** `gpu/shim/party/party_story.cpp` (+ `tests/` for the guard).

### Bug 17: red guy in Oedon Chapel is in ghost state for guest

**Evidence:**
- No guest log covers Cathedral Ward. G5 ends before the host's `:8796 cutscene 24000020 (event 12400750, map 18000000 = m24_00 Cathedral Ward)`, so this happened after 19:49.
- MSB / NpcParam (m24_00_00_00) shows that every talk NPC there has `isChangeWanderGhost = 1`:
  - **Oedon Chapel Dweller** `会話NPC_避難所の盲人`: entity 2400700, NpcParam **113010** (ghostModelId 1135), talk t240312.
  - **Alfred** `会話NPC_血族狩り_*`: 2400760 / 2400763, NpcParam **6155 / 6151 / 6154**, t240333 / t240343. Red-brown executioner garb, met at the Oedon Chapel steps; likely the "red guy".
  - Old man 6080 (2400765), lonely old woman 6090 (2400730), Arianna 6110, Eileen 6160 / 6161 (2400900-903), Adella, and the beast beggar 504020.
- In m24_01: Iosefka 6060, fake doctor 6070 / 6071, Arianna after birth 6113.

**Cause:** the same as Bug 11, an incomplete NpcParam rule list (bug). On a guest these NPCs become wandering ghosts: translucent, **and they cannot be talked to**. That breaks D8 (guest talk) for Alfred, Eileen, the Dweller and Iosefka, and possibly Bug 5.

**Fixed on head:** no.

**Severity:** major. It blocks the guest's NPC talk and quests, not only the look.

**Proposed fix:** make the param rule data-driven. Clear byte 0x145 bit 7 for every NpcParam row that a talk NPC uses. From the 1.09 data these are rows **6040-6990** (6040, 6060, 6070, 6071, 6080, 6090, 6100, 6110-6113, 6120, 6130, 6140-6144, 6150, 6151, 6154, 6155, 6160, 6161, 6180, 6210, 6500, 6510, 6511, 6521, 6590, 6970, 6980, 6990), **8120-8125**, **113010** and **804000**, plus the existing 803000. Keep the enemy rows (109100, 111010, 402xxx, 504020, ...) vanilla. Implementation: a small `{id list}` loop in `ApplyParamRules` writing `byte & 0x7f` (the current rule writes 128 → 0, which works because the byte is exactly 0x80; check each row's byte first and refuse on a mismatch, as the rule already does).

**Verify (one PC):**
- Instances 2, guest summoned: startup log lines `NpcParam <id> ... 128 -> 0` for each row.
- Guest fixture `BB_PARTY_WORLDACT_TEST=chr=2400700/2400760/2410700;after=10` (stand at each NPC until the action prompt shows). Expect a prompt on the guest, and that talking sets an npc_quest world action.
- Screenshot both windows: the NPC should be solid on the guest.

**Files:** `gpu/shim/party/seamless_rules.cpp`, `docs/party/guest_services.md`.

### Story / boss events list (all logs)

Session / save context: the host started a **new game three times**, using party save slots 1, 2 and 3 (H1:992 `slot 1 (was 0)`, H2:883 `slot 2`, H3:892 `slot 3`). The guest started new games too: H1:4380 and H3:2646 `the guest started a new game while in room ...`, G2:7241 `slot 1 (was 0)`.

| Log:line | Event |
|---|---|
| H1 183856:1006 | host cutscene 24010005 (opening, event 12410000), A11 hold :1014, ended :1024 (5.5 s) |
| H1:1126 | host cutscene 21000000 (first Dream, event 9401), A11 :1130, ended :1151 (12.0 s) |
| H2 184410:897 | host cutscene 24010005 (new game 2), A11 :902, ended :914 (4.0 s) |
| H3 192054:906 | host cutscene 24010005 (new game 3), A11 :911, ended :925 (6.7 s) |
| H3:1070 | host cutscene 21000000 (event 9401), A11 :1073, ended :1099 (27.2 s) |
| G2 185717:7351 | guest's OWN prologue: EMEVD 2002[3] from event 12410000 (own world), ended :7563 (6.9 s) |
| G2:11707 | guest's OWN 2002[3] from event 9401 (own first Dream), ended :11884 (7.8 s) |
| G2:21127-21128 | guest takes the host's #...544 cutscene 21000000 as a **replay** in its own Central Yharnam; played 27.7 s (:24872). **A second play of a scene it saw at :11707** (extra issue below) |
| H5 193033_last:2608 / G5 194949:28947 | Cleric Beast fight (event 12414702, entity 2410800) on both; host A11 hold :2609 |
| H5:2884-2891 / G5:40209 | Cleric Beast defeated (12411700, 9456); guest keeps 12411700 local (G5:40212) |
| H5:7560 | host cutscene 24010010 (Gascoigne intro, event 12411802, mode 2), 47.4 s |
| H5:7605 | Gascoigne fight (event 12414802, entity 2410810), A11 :7606 |
| H5:7607-7633 | **guest-triggered replay of 24010010 during the fight** (Bug 14), 12.2 s |
| H5:8049-8057 | Gascoigne defeated (12411800, 9457) |
| H5:8796 | host cutscene 24000020 (Cathedral Ward entry, event 12400750, warp 2402200 @ m24_00), tod 1 at :8959, ended :8972 (37.9 s) |
| A11 refusals | none of the "join refused/waits" kind in any log. Only the host's own `no bell while the host watches a cutscene / fights a boss` holds listed above. G\*: no `Waiting: the host is in a boss fight` |
| Fog entries | none logged (0.6.0 has no fog-first logging) |

### Extra problems found (this scope)

1. **Catch-up replay of a cutscene the guest already watched in its own world** (G2:11707 own 21000000 → G2:21127 replay of the host's 21000000, 27.7 s, while the guest was in its own Central Yharnam and the host was in the Dream).
   - `RequestGuestStory` only drops remos in `g_self_played`, which records host-world plays only (`party_story.cpp:944`, `!OwnWorld()`).
   - Fix: also record own-world 2002 plays, or skip a replay whose `flags` (12417810 here) are already 1 in the guest's own save.
   - Not fixed on head. Minor.
2. **The host's guest-story replay ignores the boss fight state** (H5:7605 boss bar → H5:7609 replay). See Bug 14 residual gap.
3. **World-talk diagnostics are test-only**, so playtest logs cannot show whether NPC talk overrides fired (`party_guestsvc.cpp:1705-1710`). One rate-limited non-test line per talk id would have answered Bug 5.

## Crash and additional problems

### Crash: host at 20:38:10, guest eboot +0x2199310 (null + 8): unguarded frame-arena allocation in a render-command builder

**Evidence**
- `visy_host/crash_20261013_203810.log`: `Faulting RIP 0x802199310 (guest +0x2199310)`, fault address 0x8, RAX=0, R12=0, RBX=0x10283f6e20 (a frame context), main thread. RBP chain +0x1d97858 (FUN_01d976e0) -> +0x103d438 (FUN_0103d3b0) -> +0x103d2c1 -> +0x1067579 -> +0x103deff -> +0xf8a48c -> ... (the frame loop / draw submission, no party code in the chain).
- `visy_host/launcher_20261013_193033_last.log` (H5, 68 min into the session):
  - 11010 `Stability: arena allocator (guest +0xe45560) returned NULL #1: arena 0x10283f6e98, request 0x4d0 (chunk 0x800; ... 2 chunks, 2 allocations); backing allocator 0x10283b5f30, vtable guest +0x52e6fd0, alloc guest +0xe453b0`
  - 11011 `Stability: render command 0x1028721e20 dropped (#1): frame context 0x10283f6e20's arena returned NULL (guest +0x21d5b20)` (our existing guard at +0x21d5b35 worked)
  - 11012 / 11013 `... returned NULL #2: request 0x20` and `#3: request 0x10` (same arena, frame context 0x10283f6e20 = the crash's RBX)
  - 11014 `GPU breadcrumbs while the GPU makes no progress`: stream 0 finished #224928204 (`pass camera motion`), did not start the next compute dispatch.
  - 11027 `Party: the game exited with 139 (a crash); restart 1 of 5 in 3 s to rejoin the party`.
  - From line 3837 on (about 20 min into H5) the texture cache is permanently over budget: 379 `Texture cache: memory pressure, 5480-5836 of 5415-5558 MiB (critical ~6110)` lines up to 10699 (RTX 3070, 8 GiB). Earlier host sessions: 3 / 0 / 4 / 0 such lines.
- Party activity just before (10700-11009): ordinary Cathedral Ward (m23_00_00_00) play with the guest present: enemy drop rolls, map pickups, guest death + "revived in place" (re-creation of the guest's chr, 1.5 s), a `stale remote chr? (0,0,0) HP 0/0` line. No travel, story, load or join at the moment of the crash.

**Cause (disassembly of the eboot at the site)**
FUN_02198fd0 (called from FUN_01d976e0 +0x1d97853) takes two blocks from the frame context's arena:
```
21992fb  call 0xe45560            ; arena alloc(frameCtx+0x78, 0x10, 16)
2199300  mov  r12, rax
2199303..230d  unlock (vtbl+0x28)
2199310  mov  [r12+8], r13        ; <- r12 = NULL: fault at 0x8
...
2199341  call 0x21b4b00           ; submit the command
```
The first alloc (0x2199266, 0x20 bytes) has a NULL test (`test r12,r12; je`), the second (0x21992fb, 0x10 bytes) has none. The arena's backing allocator (vtable +0x52e6fd0, alloc +0xe453b0, the per-frame allocator, not CSGraphicsPrivateAllocator, so our probe printed no heap numbers) ran dry; our `BB_ARENA_GUARD` covers only the +0x21d5b35 caller (1 of 45 call sites of +0xe45560 in calls.tsv). Root of the exhaustion is not proven: candidates are the GPU stall seen in the breadcrumbs (frame contexts not recycled while the GPU is stuck) and long-session VRAM over-budget on the 8 GiB host (constant eviction for 45 min). It is a generic long-session render/memory crash, not a party-protocol bug; the party only adds load (2 players, re-created chrs).

**Fixed on head?** No. `git log 8143fda..aeeb408` touches nothing in `gpu/shim/bbport_stability.cpp` or the GPU path.

**FIXED (batch 5, 2026-10-14)** in `gpu/shim/bbport_stability.cpp`:
- Why it ran dry (disassembly): the arena's backing allocator (vtable +0x52e6fd0, alloc +0xe453b0 ->
  +0x2085120) is a plain linear (bump) allocator of **256 KiB**, one per frame context (renderer +0x370 /
  +0x378, made in FUN_02194580 by `mov edx, 0x40000` at +0x2195c09 / +0x2195c46 -> +0xe45200) and reset
  at every frame flip in FUN_02198fd0 (vtbl+0x78 at +0x2199134 / +0x2199587 / +0x21999a3; the frame's
  arenas +0x48 and +0x78 restart with 2 KiB chunks via +0xe45530). The failing arena held only 2 chunks,
  so the other consumers of that frame's 256 KiB had used it up: a per-frame budget overflow on a busy
  frame (two players, re-created chr, effects), not a leak across frames (the flip resets it) and not
  the texture cache. Not measured on the playtest's frame, so "busy frame" stays the likeliest reading;
  the GPU stall in the breadcrumbs came after the NULLs.
- The allocator is now 1 MiB per frame context (`BB_FRAME_ARENA_KIB`, 0 = the game's 256 KiB; +1.5 MiB
  from the renderer's heap at boot). A probe at the three flip sites logs `Stability: frame allocator:
  the fullest frame so far used N of M KiB` (from 64 KiB, each +25 %), and `ArenaNull` prints the
  allocator's used/capacity/blocks, so the next report shows how close a frame came.
- Second guard at +0x2199303 (bytes checked +0x21992ed..+0x219934e): a NULL runs the unlock and skips
  the fill and the submission (+0x21b4b00) to +0x2199346; log `render command 0 dropped ... (guest
  +0x21992fb)`. Test aid `BB_ARENA_FAIL_TEST=N` drops every N-th pass. Live (one single-player
  Continue into the Dream, `BB_ARENA_FAIL_TEST=300`): `arena-guard on (fail test 1/300); frame-arena
  1024KiB; frame-arena-probe on`, two drops from +0x21992fb, no fault.
- Still open: the audit of the other ~43 call sites of +0xe45560 (FUN_0224f120, FUN_022a0a30 first);
  with 4x the budget they should no longer see a NULL.
- Watchdog noise (additional problem 1): `src/probe.c` dumps `WATCHDOG: NO PROGRESS` only when flips
  stalled 5 s too, and prints `[BLOCKED >3s]` only while flips stall 3 s; the launcher warns (Party tab
  hint and the launcher log) when Debug tracing is on in a party.

**Severity:** major (one crash in ~2.5 h; the auto-restart + rejoin recovered it, see below). Would be a blocker if it recurs per hour.

**Proposed fix (smallest correct change)**
1. `gpu/shim/bbport_stability.cpp`: a second guard like `InstallArenaGuard` at +0x2199303 (after `mov r12, rax`): byte-check 0x21992fb..0x2199346, emit `test r12,r12; jz .skip`; `.skip` runs the unlock (`mov rax,[rbx+0xc8]; mov rdi,r14; call [rax+0x28]`) and jumps to +0x2199346 (after the `call 0x21b4b00` submission), dropping the command for that frame and logging via `CommandDropped`. Gate it with the existing `BB_ARENA_GUARD`.
2. Extend `ArenaNull` to print the backing allocator's capacity/used when its vtable is +0x52e6fd0 (decompile +0xe453b0 for the fields) so the next report tells whether it is exhaustion or a missed per-frame reset.
3. Follow-up (separate): audit the other 43 `+0xe45560` call sites (calls.tsv) for missing NULL checks; FUN_0224f120 (4 sites), FUN_022a0a30 (3) first.

**Verify on one PC**
- Add a test aid `BB_ARENA_FAIL_TEST=N` (in bbport_stability.cpp: make the probe stub return NULL for every Nth call of +0xe45560 whose arena is a frame context) and document it in docs/party/DEBUG_SWITCHES.md.
- `python tools/mp/instances.py` with host + 1 guest (BB_PARTY_TEST_START_LAMP to skip the prologue), `BB_ARENA_FAIL_TEST=500`, walk 5 minutes: expect `render command ... dropped` lines from both guard sites and no crash; without the new guard the run crashes at +0x2199310 within minutes. Screenshot both instances (visual-checks rule) to confirm only one-frame flickers.
- Long soak: 70+ min host+guest in Cathedral Ward with `Texture cache` budget logged; check `returned NULL` count stays 0 or dropped commands only.

**After the restart**: worked and fast. `launcher_20261013_193033_last.log` 11028 `Boot: launching at 20:38:14.43`; +6.4 s `auto-continue: party link up; confirming Continue (crash restart)`; the guest's link re-joined at the same moment (11690 `the guest joined (slot 1, from <guest address>)`); world up at 15.4 s (17 Cross presses, the FROM sign-in held Continue ~8 s); room 1001 created and the guest joined it (11928 room created, 11958 the guest joined). Afterwards: a host death (13134 `host death taken: no stage request ... echoes 5395 go to the bloodstain`), several lamp travels m23 <-> m21 <-> m24 with `slot 1 arrived`. One wrinkle: 13469 `host: arrived alone in m21_00_00_00` (the guest did not follow one m23->m21 travel, it arrived on the next one). Guest-side logs after 19:49:49 are missing, so the guest's view of the crash/rejoin cannot be checked.

### Additional problems not in the players' notes

1. **Guest ran with Tracing=on (huge logs, false watchdog dumps, lower FPS).** All 5 guest logs: `Features: ... Tracing=on ... PreUpload=Full (~3 GB more VRAM, 12 GB+ GPU)` (host: Tracing=off). Consequences: 198 `WATCHDOG: NO PROGRESS FOR 5 SECONDS` dumps (G1 7, G2 37, G3 76, G4 23, G5 55; host 0) and 57,831 `[BLOCKED >3s]` lines; logs of 13-109k lines (20 MB). The dumps are false positives: in every dump `main` is `RUNNING / SPINNING`, the heartbeat shows 40-60 flips/s, and the trigger in `src/probe.c` watchdog_worker is `g_last_progress_tick` stale >= 10 s, which only `pre_import_hook` (unbound/traced import calls) refreshes. Also `[IMPORT CALL #n] __error` traces (G1 1888+). Guest FPS from heartbeats: avg 42.5-55 per session, 29 one-second samples under 20 FPS. Severity: minor (diagnostics), but it makes triage hard and costs guest performance. Fix: `src/probe.c`: dump only when flips also stalled (`zero_flip_seconds >= 5`), or refresh progress on each flip; launcher: warn when Tracing is on in a party session (or turn it off for party). Files: src/probe.c, launcher settings UI. Verify: instances.py with Tracing on, 5 min idle walk: 0 watchdog dumps while flips advance.

2. **Repeated direct-summon failures in G4 (8 of 10 tries failed after 8.1 s).** `guest/launcher_20261013_193046.log` 10889, 14558, 16186, 24404, 26074, 29880, 30088, 32784 `direct summon: FAILED: the bell sign path takes over (8.1 s after the request)`; host `launcher_20261013_193023.log` 1797, 1981, 2062, 2533, 2702, 2748, 2764 `reports a failure: its signs are offered again`. In each failure the host's room answer arrived (`the host's answer: its room`) but the room was never handed to the game (no `handed to the game` line): `DirectSummonTick` only delivers when `world_up && !loading && WaitingGuestEntry() != 0`, and the bell's waiting SSM entry was absent (the bell was `already on`: `SpEffect 9005 applied, was already on`). No log says which condition blocked. Severity: major (long joins: the successful G4 join 18883 came minutes after the first try). Fix: gpu/shim/party/party_b2.cpp DirectSummonTick: log the blocking condition once per request; when no waiting entry exists, re-ring the bell (drop/re-apply 9005) so the game re-creates its passive entry before the 8 s timeout, or extend the wait while the bell is re-arming. Verify: instances.py host+guest, guest leaves and re-rings 10x (`BB_PARTY_TEST_START_LAMP`), count `joined the host's room` vs `FAILED`.

3. **Silent host refusals of direct summons.** Guest `185717.log` 15783, 18357, 24993; `192049.log` 4532; `194949.log` 4217: `direct summon: the host's answer: refused by the host` with no reason; the matching host logs (`192054.log`, `193033_last.log`) have no `direct summon` line at all: the refusal comes from `DirectSummonEvent` Kind::Req when `!DirectSummonOn()` (host still in its prologue / not READY), which sends `EncodeRefuse()` without logging. Severity: minor (diagnostic; the sign path then joined). Fix: party_b2.cpp DirectSummonEvent: log `slot N refused (direct summon off: prologue/not ready)` and send a Busy reason the guest can show ("host is in its prologue"). Ties into bug 1 (prologue).

4. **Unexplained guest session leave 92.7 s after a join (G4).** `193046.log` 10655 `leave: BeginLeaveSession called from +0x178a51f (role 6)` (FUN_01789b60, the session event handler) right after the host rested at a lamp in the same map (guest followed in-world, `in-world warp ... no load`). Host `193023.log` 1747-1749 `member 2 (the guest) left room 1001 (leave_room)` + `penalty/notify_multi_play_error`, then `funnel: guest_sent_home from +0x138a51c` and the host's own session ended (`BeginLeaveSession from +0x1789f99 (role 3)`, 1800) and the host reloaded (`arrived 40.5 s after the warp`). This happened in the prologue-era of game 2. Severity: major (drop to own world). Fix: log the session event that reached FUN_01789b60 (event id at the caller) in party_b2.cpp leave hook; check rest + in-world follow (`party_travel.cpp` rested path) does not leave the guest's session flag inconsistent. Verify: instances.py host+guest, host rests at a lamp 5x with the guest beside it; guest must stay (`session client` unchanged).

5. **Host texture cache permanently over budget in long sessions (8 GiB GPU).** H5 379 `memory pressure` lines from 3837 on (5480-5836 MiB vs budget 5415-5558, hundreds of evictions per report), precedes the crash. Severity: major (likely contributor to the crash; also stutter). Fix: investigate eviction thrash (0 written back, re-uploads), consider lowering the budget target or freeing the party's re-created chr textures; files: gpu texture cache (gpu/shadps4 video_core texture cache), not party. Verify: 60-min soak with BB_PARTY test aids, track the pressure line count.

6. **Stale remote characters.** 32 `Party HUD: stale remote chr?` lines in all sessions (8 to 280 m from the nearest member report, some `(0.0 0.0 0.0) HP 0/0`), e.g. H5 10950 right after `member revived in place ... re-created`. A dead/old copy of the other player survives re-creation (possible cause of "ghost" looking players). Severity: minor/major (visual). Fix: party_b2.cpp revive/re-create path should destroy the old remote PlayerIns before the rewind; party_hud.cpp keeps the detection. Verify: instances.py, guest dies and is revived 5x (`host_kill`/guest death aids), stale count 0, screenshot both.

7. **Position GAPs of the remote player.** 31 `Party HUD: GAP` lines, 2.0-5.9 m between where a game draws the other player and its reported position (G5 9, H5 12, G3 6). Severity: minor (rubber-banding after warps). Fix: none urgent; watch after split/follow fixes.

8. **`no arrival within 180 s` printed immediately when the guest restarts.** H1 `183856.log` 4389 and H3 `192054.log` 2654 directly after `the guest started a new game while in room`: the travel watchdog fires on the restart, not after 180 s, and leaves the room. Severity: minor. Fix: party_travel.cpp: cancel the arrival wait on a member restart instead of reporting a timeout.

9. **Crashed-member hold blocks a quick rejoin.** H1 4428 `direct summon: slot 1 refused (a crashed member's connection is still held)` and H3 2620 `the guest's signs are held for 26 s (the host's old connection to it must time out first)`; H1 3457 `member 2 (the guest) left room 1001 (timeout)`. A guest that restarts waits 20-26 s plus a sign-path join. Severity: minor. Fix: party_dsummon/FromApi: drop the hold as soon as the party link reports the same player re-joined with a new port (the link knows it restarted: `the guest restarted its game`).

10. **Misleading NAT message while the host restarts.** G4 32908, 34155, 34362 `connection lost; reconnecting: no UDP answer from the host (behind a NAT: it needs your guest code)` while the host was simply relaunching (H4 ended 19:30:23, H5 started 19:30:33). Severity: minor (UX). Fix: party_link: after `the host ended the party`, say "waiting for the host to come back" instead of the NAT hint for the first ~60 s.

11. **HTTP errors to the local FROM API after a host leave.** G1 `184415.log` 12691/12717/12740 `POST http://bbparty.invalid:18671/wandering_ghost/get ... -> 0 (0 bytes, 0 ms) error` (also blood/tomb messenger) right after `refused by the host: the host ended the party`. Harmless (the game retries) but means the local API was down for a moment after the party ended. Severity: minor.

No flag-apply failures (`failed N>0`: 0 in all logs), no other arena NULLs, no Vulkan errors of note (19-38 hidden warnings per run).

---

# Session timeline


Times come from each run's start line and file name (end time); events inside a run are in log order
(no per-line timestamps). "join" = the guest in the host's world; "travel" = host lamp travel sent to the party.

| When | Host | Guest |
|---|---|---|
| 17:08:04 | **H1 starts** (game 1, new game, save slot 1). Solo prologue: host death in the clinic -> Dream (`H1:1072`, cutscenes 24010005 / 21000000 `H1:1006, 1126`), `step ready` `H1:1165`, bells granted `H1:1168-1170`, lamp travel to m24_01 `H1:1176`. | (guest runs before 18:41 are NOT in the folder) |
| H1 | `H1:1700 the guest joined (slot 1)` (guest at title), host rings `H1:1710`; guest leaves/rejoins the link `H1:1898-1914`. | |
| H1 | Room 1001 `H1:2200`, direct summon `H1:2211`, guest joins room `H1:2220/2249`. Guest drops by timeout `H1:3421/3457 left room 1001 (timeout)`, back `H1:3470`. | |
| H1 | Room 1002 `H1:3521`, direct summon `H1:3532`, joined `H1:3549`; host travels to the Dream during the join `H1:3630` -> **party watchdog stop (stuck summon, 72 s)** `H1:3694`, room closed. | |
| H1 | Room 1003 in the Dream `H1:3746`, summon + join `H1:3756-3785`; link drop/rejoin `H1:4353-4363`; **guest starts a new game while in the room** `H1:4380` (game 1 abandoned by the guest); `H1:4389 no arrival within 180 s`; **direct summon refused: crashed member's connection held** `H1:4428`; room 1004 + bell-path join `H1:4448-4471`; travel to m24_01 `H1:4598`, arrival `H1:4671` (re-join 8.2 s); guest dies `H1:4763`, revived in place `H1:4791`. | |
| 18:38:56 | **H1 ends** (host quits; game 2 begins). | |
| 18:39:52 | **H2 starts** (game 2, new game, save slot 2). Opening cutscene `H2:897`, `step clinic -> not ready` `H2:919`. | |
| 18:41:50 | | **G1 starts** (Continue of the guest's game-1 save, past the prologue: `G1:4056 step ready`). 7 watchdog dumps. |
| H2/G1 | `H2:929 the guest joined`; **direct summon served while the host is in the clinic** `H2:949-992` (bug 1). | `G1:4131 asked`, `G1:4223 joined room 1001`, loads the host's prologue world `G1:8114 m24_01`, step clinic `G1:8107`. Guest dies alone `G1:12201/12373`. |
| H2/G1 | Guest "in_host_world" but host cooperators 0 -> **party watchdog stop (stuck summon, 60 s)** `H2:1938`, room closed `H2:1945`, host shuts the party. | `G1:12473 room gone (host_left)`, `G1:12637 refused: the host ended the party`, back home m21 `G1:12737`. |
| 18:44:10 / 18:44:15 | **H2 ends** / **G1 ends**. | |
| 18:44:44 | **H3 starts** (game 2 again, new game, save slot 3). Opening `H3:907`, `the guest joined` `H3:939` (guest at title/prologue). Host prologue death -> Dream `H3:1019`, cutscene 21000000 `H3:1071`, `step ready` `H3:1137`, bells `H3:1138-1142`, host rings `H3:1147`, room 1001 `H3:1154`. | |
| 18:46:46 | | **G2 starts** (new game, slot 1): opening `G2:7326`, clinic `G2:7445/7565`, Dream `G2:10413`, `step ready` `G2:14543`, bells `G2:14544-14548`, **gather** to the host's lamp `G2:14549`; 3 direct summons **refused** while the host is not ready (`G2:15783, 18357, 24993`, no host log); second gather `G2:24874`. 37 watchdog dumps. |
| H3/G2 | `H3:1164 visy summons the guest into room 1001` (sign path), `H3:1190 the guest joined room 1001`. | `G2:27558 joined room 1001`, `G2:27905 arrived 10.4 m from the host; put beside it` (Dream). |
| H3/G2 | Travel to m24_01 `H3:2107`, re-join 8.3 s `H3:2192`; second arrival `H3:2581-2586` (7.9 s). | `G2:31453-31468 arrived by the host's travel` (m24_01); `G2:50293-50309` guest goes to the Dream **alone** (split "travel alone"), back to m24_01 `G2:53207-53230`. 14+ watchdog dumps. |
| 18:57:17 | Guest restarts: `H3:2619 the guest left`, `H3:2647 left room 1001 (restarted)`, `H3:2654 no arrival within 180 s`, `H3:2676 split question dropped (session changed)`, travel to the Dream `H3:2677`. | **G2 ends** (exit code 0). |
| 18:57:21 | Room 1002 in the Dream `H3:2767`, `the guest joined room 1002` `H3:2781`. | **G3 starts** (Continue): `G3:4528 asked` -> refused `G3:4532`, bell path `G3:4900 joined room 1002`, `G3:5191 arrived 0.3 m from the host`. 76 watchdog dumps. |
| H3/G3 | Travels: m24_01 `H3:2834` (arrival 7.9 s), Dream `H3:3601` (5.1 s), m24_01 `H3:3707` (7.9 s); 3 guest deaths revived in place `H3:3158, 3433, 4274`; split prompts timed out twice `H3:3429, 3975 no answer in 120 s`. | Guest arrivals `G3:6856, 27166, 40584, 43322` (each `forced placement (own save spot)`); guest deaths `G3:20547, 32939, 67116` -> `died alone: revived at its own last lamp, no load`; guest goes to the Dream alone `G3:25673-25689`; **host death** seen by the guest `G3:95719 the host was revived in place (no load)`. |
| 19:20:49 / 19:20:54 | `H3:5108 the guest left`. **H3 ends**. | **G3 ends**. |
| 19:23:06 | **H4 starts** (Continue). Ready in m24_01 `H4:839/850`, `the guest joined` `H4:871`. | |
| 19:24:19 | | **G4 starts** (Continue; save spot the Dream). 23 watchdog dumps. |
| H4/G4 | Room 1001 `H4:933`, dsummon `H4:943` (area m24_01), `H4:1030 reports it joined`; host travels to the Dream to fetch the guest `H4:1598`, `slot 1 arrived` `H4:1714`, then `H4:1747 member 2 left room 1001`. | **Ring before own map known** `G4:5637/5641` (bug 9), joined cross-map `G4:5861`, `apart 1`, game leaves the session `G4:10678`. |
| H4/G4 | 8 failed direct summons (`H4:1797 ... 2764`), rooms 1002-1004 created/closed, host loads 40.5 s / 59.9 s (`H4:1855, 2591`), travels m21/m24_01 `H4:2023, 2094, 2262`; one more cross-map join `H4:2167-2198` (room 1003 in the Dream) ending `H4:2482 left room 1003`. | `G4:10889 ... 32784 FAILED` x8; second cross-map join `G4:18795-18883` (ring before own map known again), `G4:23142 BeginLeaveSession`; `G4:32713 refused: the host ended the party`. |
| 19:30:23 / 19:30:46 | **H4 ends** (host restarts). | **G4 ends**. |
| 19:30:34 | **H5 starts** (Continue, m24_01). `the guest joined` `H5:862`, room 1001 `H5:917`, `the guest joined room 1001` `H5:948`. | |
| 19:31:32 | | **G5 starts** (Continue; save spot m24_01). `G5:4187 asked` -> refused `G5:4217` (no host log line), bell path `G5:4398 joined room 1001`, `G5:6195 arrived 3.4 m from the host`. 55 watchdog dumps. |
| H5/G5 | Travels Dream/m24_01 `H5:2956, 3455, 3824, 3927` (re-joins 4.6-7.8 s). **Guest Doll level-up and lamp party move in the Dream** `H5:3069-3153` (bug 10). Link drop + rejoin `H5:4074-4148` (room 1002). Guest death revived in place `H5:4494`. More travels `H5:6085, 6193`. Cutscene 24010010 sent twice `H5:7561, 7610`, cutscene 24000020 `H5:8797` (Gascoigne area; other sections), map m24_00 `H5:8992`, m23_00 Old Yharnam `H5:9545`. | Arrivals `G5:43092` (Dream), `G5:66703` (m24_01), `G5:76491` (Dream), `G5:77910` (m24_01); host death seen `G5:70625 host revived in place`; level 10 -> 17 `G5:44898`. |
| 19:49:49 | Host plays on: travels `H5:8158, 8447, 9690, 9952` (Old Yharnam lamp 2302950, re-join 11.3 s), guest death `H5:10236`, travels `H5:10397, 10513` (11.4 s), guest death `H5:10945`. | **G5 ends**. Guest logs from 19:49:49 to the end of the session are missing (the guest's later runs, incl. the rejoin after the host crash, are not in the folder). |
| 20:38:10 | **Host crash** `H5:11023 crash_20261013_203810.log` (in m23_00 Old Yharnam, shortly after a guest death revived in place `H5:10945`); `H5:11027 restart 1 of 5 in 3 s`; `H5:11028 Boot 20:38:14`; `H5:11635 auto-continue: confirming Continue (crash restart)`; `H5:11691 the guest joined`; room 1001 `H5:11928`, `the guest joined room 1001` `H5:11958` (rejoin OK). | (no log) |
| after | Travels Dream `H5:13251`, m23 `H5:13337`, Dream `H5:13416` (**host arrived alone, guest NO-chr** `H5:13469`), m24_00 `H5:13505` (re-join 12.9 s `H5:13588`), guest death `H5:13697`, `H5:14104 the guest left`, H5 ends. | (no log) |

Counts per run: party watchdog stops (stuck summon) H1 1, H2 1; 5-s engine watchdog dumps G1 7 / G2 37 / G3 76 /
G4 23 / G5 55, hosts 0; loading screens H1 5, H2 1, H3 7, H4 13, H5 18, G1 3, G2 8, G3 7, G4 3, G5 6; direct
summon outcomes: served-and-joined H1 3, H2 1, H4 2 (both cross-map), refused/failed G2 3, G3 1, G4 8, G5 1;
re-join after a host travel 1.5-12.9 s on the host (longest into Old Yharnam and m24_00), 0.0 s on the guest.

## Batch 2 result (pt1join, 2026-10-14)

- **#9 fixed (live).** The guest's ring waits for its OWN map (`bell: waiting for this guest's own map before ringing`), so
  `GatherGuest` always sees both maps and gathers with a vanilla lamp warp (loading screen). Safety nets: the guest
  never hands its game a room of another map (`dsummon::RoomElsewhere` -> `FAILED (the host's room is in another
  map ...)`), and the host refuses a request whose roster map is another area (`"wait":"map"`); either way the
  guest's bell (SpEffect 9005, its sign) goes down and the gather runs at once. Chalice dungeons (area 29) and the
  `BB_PARTY_GATHER_DSUM` experiment keep the cross-map carry.
  Live (one run + one re-run, loopback, `BB_NET_SIM=dsl`, save_backup/savedata_20261010, host
  `BB_PARTY_TEST_START_LAMP=2412951`): re-run with title join off on the guest (the playtest's in-world ring path):
  `gather: own world m21_00_00_00, host in m24_01_00_00; to the host's lamp 2412951` 0.2 s after the own map came up,
  one load, then `direct summon: joined the host's room (3.4 s after the request)`, host cooperators 0 -> 1.
  Guest killed (`--crash-when` once beside the host) and restarted: Continue in m24_01, `joined (2.9 s after the
  request)`, `join: arrived 1.0 m from the host`; `status: where ... apart 0` and screenshots of both games side by
  side. First run (title join on): the restart title-joined straight beside the host (0.9 m, screenshot).
- **#1 fixed (unit).** `HostView.host_ready` (the director's `LocalStartReady`) and `guest_in_prologue` (roster):
  refused with `"wait":"prologue"` ("Waiting: the host is in its prologue") before any room is created. The guest
  side was already gated (`GuestMayRing` needs start ready). Not exercised live (no run budget left for a new-game
  save): covered by party-dsummon-test.
- **#2 fixed (live).** The host's Beckoning Bell is the direct summon's fallback only (`dsummon::HostBellHeld`): held
  for 15 s after a member starts waiting in the host's map (its own ring comes first), while a served room is on its
  way, 20 s after a join, and after a refusal that says wait; rings at once after a reported failure or a plain
  refusal (sign path). Live: `no Beckoning Bell while the direct summon handles it` on the rejoin. (The first join of
  the re-run still rang once because the grace counted the guest's time in the Dream; fixed after the run: a member
  in another map restarts the grace. Not re-run.)
- **Refusal logging.** Every host refusal logs slot, name, reason, both maps and the wait word, including the
  previously silent `BB_PARTY_DIRECT_SUMMON=0` path (the likely source of H3/H5's unlogged refusals); every guest
  `FAILED` says why (no answer / entry not up / game refused 3x / no join within 8 s / other map).
- **Held connection after a guest restart.** Once the host's game has cleared the restarted member's stale peer, its
  sign / direct summon hold ends (`<name>: its sign hold ended`), so the new run is not refused for "a crashed
  member's connection is still held". Live rejoin: no refusal.
- **G4 8-of-10 failures:** their cause was the cross-map chase (#9); with the room/map checks they now end in one
  gather instead of repeated failed summons. Not touched (other files): host-travel-while-joining watchdog stop,
  holding host lamp travel ~10 s after serving a room, the premature "no arrival within 180 s".

## Batch 6 result (pt1follow, 2026-10-14): follow-ups of batches 1 and 2

- **A guest's alone rest no longer leaves the host waiting 60 s.** `HostRestLoadTick` counts who follows the rest load
  (`deathrules::RestLoadExpected`): the resting guest only when its lamp is in the host's area, other members only in
  the host's area (or still loading). Nobody: the host's load is an alone load (no snapshot re-send, no wait). Live
  (one run + one re-run, loopback, Central Yharnam): guest NO at the Dream lamp -> `0 member(s) here follow (an alone
  load ...)`, `host: arrived alone` 6.4 s after the warp (was: `60 s without arrival`); guest NO back to lamp 2412951 (the
  host's area) -> `1 member(s) here follow`, the guest followed the load, `re-join done after 6.5 s`. The first run had
  counted that second case as alone: the guest waited for a snapshot that never came and fell back to B1 after 45 s
  (fixed before the re-run).
- **No watchdog stop through a host travel (H1:3694).** The cooperator count reads -1 while loading, and that frame
  kept the watchdog's clocks running through the load, so a join stalled before the travel was "stuck for 72 s" the
  moment the host arrived. Now a loading screen or a running B2 re-join (`b2::HostRejoinRunning`, new
  `HostWatchdogInput.host_travel`) restarts the clocks. Unit: party-watchdog-test `test_host_travel`. No stop in any run.
- **The host's lamp travel waits for a member it just let in (H1:3630).** `dsummon::HostTravelHeld`: a host lamp warp
  within 30 s of serving a room waits until the joiner's chr is in its world (or the join failed / the hold ran out),
  then runs (`HostHeldLampTick`). Live (test aid `BB_PARTY_TEST_TRAVEL_ON_SERVE`): `host lamp warp to 2102950 held: a
  member is joining`, released 3.8 s later after `reports it joined`, then one party travel (both loading screens).
  The live runs also showed the opposite race: a room served while the host's own warp was already armed (the stage
  request) joined the guest into the map the host was leaving (it arrived in an empty Dream, as in H1). The host now
  refuses that request (`the host is about to travel (its loading screen is next)`, no wait word); the guest's
  next ring (30 s later) joined it by direct summon in the host's new map (live, twice).
- **"no arrival within 180 s" right after a guest restart (H1:4389, H3:2654).** The travel mark stayed set while B2
  kept the room (the empty-room check is skipped then), so the first frame without a member task timed out at once.
  `PlanEmptyRoom`: while B2 holds the room the mark is dropped once the host is in its world and the re-join is over
  (`the B2 re-join kept the room`); the 180 s count runs only while nobody holds the room. Live: guest killed and
  restarted after a load: `Hunter1 restarted its game`, rejoined by direct summon, no "no arrival" line. Unit:
  party-travel-test `test_empty_room`.
- **Host bell (batch 2 #2 follow-up), live:** the bell rang 0.03 s after a guest's request (the request arrives one
  tick before the room is served, and `HostBellHeld` read "request answered with no wait word"). An unanswered
  request now holds the bell (`since_refused`). Re-run: `no Beckoning Bell while the direct summon handles it` on each
  join; the host rang only after the plain "about to travel" refusal (its sign path, as designed).
- **Wipe (5a), live once:** guest `guest_die`, the host died 1 s after the guest's down notice (test aid
  `BB_PARTY_TEST_DIE_ON_DOWN`, no `host_kill`): `host death: every member is down too (1 of 1): a wipe; the party
  respawns together (one load)`; guest `held death goes with the host's travel; no redirect, no leave`; both at the lamp
  afterwards (screenshot). That guest then fell back to B1 because of the rest-load bug above (fixed); the re-run's die
  step landed inside a load (the load undid it), so the clean wipe follow is not re-checked live.
- **Not done live: a direct summon refused while the HOST is in its prologue (5b).** First try: the harness copied a
  character save into the "empty" host (its default `--saves`), so the host was past its prologue (the join worked).
  Re-run with an empty save: the new-game driver (`BB_TEST_NEWGAME=1`) gave up at the contract screen ("the name box
  never opened"). Still covered by party-dsummon-test only.
## Batch 7 result (pt1follow2, 2026-10-14)

Two live runs (2 instances, loopback, save_backup/savedata_20261010; run 1 lost its Central Yharnam part to a
held split question on the test start lamp and ended at the 240 s lease; run 2 used `host_warp` instead).
Screenshots: `agents/pt1follow2/shots1`, `shots2` (removed with the env; findings below).

- **#8 cause found (live) and fixed.** Guest 3 m from Gascoigne's fog (2411810), host's Gascoigne flags cleared:
  `boss fogs of this block: [12414811: read 1 intro 0 host 0 dead 0 fog within 20 m]`. The flags were right; the
  distance was not: `MyPos` read ChrIns +0xC0, which on a guest in the host's world stays at its arrival spot (the
  B2 status line shows `pos (-52.3, -40.8, 68.7) drawn (-36.8, -40.8, 84.3)` for the whole run, 19 m from the fog).
  `MyPos` now reads the physics position (+0x58 -> +8 -> +0x3B0 -> +0x68: +0x1E0, `PickPlayerPos`), and the notice
  condition is `BossFogNoticeDue` (unit-tested). First fights with an intro still start with the host (the notice).
  The notice firing after the fix is not yet seen live.
- **#5 window residents (live).** The knock goes through the door dummy objects: 2410870 and 2410871 each gave
  prompt 10010220 within 4-6 fixture spots, Cross opened the resident's dialogue on the guest ("...Oh, a hunter of
  beasts, are you?"). The knock event 12410850 sets only the per-door knock-sound trigger 70000050 / 70000051
  (never_sync type-7 global, a transient sound cue): there is no world state for the host, so nothing is sent; quest
  talks (Iosefka) already reach the host. The earlier fixture used 2410780 (Gilbert's model chr); his talk chr is
  the hit chr 2410782 (`会話用_重病人(ヒット判定用`), which showed no prompt at 16 spots in this save state (it
  worked live 2026-10-11), nor did the girl 2410760. No code change.
- **#10 Doll level-up (live).** Guest `level 37 -> 38, echoes 20000 -> 11366` in the host's Dream; no world action
  from the guest, no `slot 1 changed the host's world` on the host, the host's position unchanged in every status
  line (-10.6, -7.0, -21.2); host screenshot: no kneel.
- **Item box at host death (live).** `host_die@S` without `%MAP` only runs as the first step after a (re)join
  (batch 4's runs had a step before it); `host_die@40%m24_01` runs on the steady-world clock. The guest's box
  (Sword Hunter Badge + Old Hunter Bell) was on screen at the host's death and gone after the death load
  (screenshots); the load closed it, so the watchdog had nothing to clear (no `item-get dialog cleared` line).
- **Old Hunter Bell (live, both runs).** `guest's own gift lot 10050 ... given to the guest itself` -> `gave #0 lot
  10050 ... goods 4312 x1 (held 0 -> 1, item-get dialog)`; the box shows "Old Hunter Bell".
- **Gehrman screenshot: inconclusive.** The guest stood 2.5 m from 2100600 facing him; the frame shows the workshop
  but Gehrman is not visible (he may be hidden by the item box and the player model). Not settled.
- Test-aid note: a host test start lamp (`BB_PARTY_TEST_START_LAMP`) that fires as the guest joins is held by the
  split question; use `host_warp=ID@S%MAP` (a party move without the question) or `BB_PARTY_SPLIT_TEST=yes`.


## Verification replay (pt1verify, 2026-10-14, head a59f836 + fixes below)

Four live runs (2 loopback instances, `BB_NET_SIM=dsl`, test saves E:\games\bbport\test_saves, `--fresh-saves`;
S1/S2 the replay, S3 the re-run, S4 the check of the two fixes; one S1 start was aborted after 50 s for a wrong
`%MAP` filter: lamp 2412953 and Gascoigne's fog 2411810 are in **m24_01**, not m24_00 as the map list above says).
Screenshots every 5 s of both windows (desktop not locked), looked at for each check.

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Join from the Dream to a host in Central Yharnam | **PASS** | S1 guest (title join off): `gather: own world m21_00_00_00, host in m24_01_00_00; to the host's lamp 2412953 before ringing` -> one load -> `direct summon: joined the host's room (2.8 s after the request)` -> `join: arrived 0.5 m from the host`; host `no Beckoning Bell while the direct summon handles it` (no host bell). Shot 55 s: side by side, "Entered the world of Hunter0" only on the guest |
| 2 | Gascoigne fog, guest first; intro once; fight | **PASS** | S1 guest 1.2 m from 2411810: `boss fogs ... [12414811: read 1 intro 0 host 0 dead 0 fog near]`, `at boss fog 2411810 ... first fight here - the host goes in first ... notice shown` (on screen: "Boss fog: the host goes in first (first fight here)"). Host `host_move` into 2412815: host `cutscene 24010010` once + `sent to the party`; guest `live mirror` once, `event 12411802's 2002[3] skipped: its cutscene already played as the live mirror`; both `remo ended after 47.x s` and `boss health bar shown (event 12414802)` within 0.05 s; guest `the host's hold: boss`. Shots 300/340 s: same scene on both |
| 3 | Host death with the guest present | **PASS** (loot: see open items) | S1 `host death: the respawn load for the whole party at lamp 2412953 (vanilla loading screen, the world resets)`; guest `send-home ... dropped (B2: the host's respawn travel follows)`, one load each, `re-join done`; host census before 242 alive / 2 dead + the host_kill victim 2400125 dead, after the load `244 alive, 0 dead`; no `RESPAWN without a load`, no stale chrs except at test teleports. Shots 80-105 s: YOU DIED, both loading screens, both back at the lamp, the guest's open item box gone. After it the guest killed an enemy and got its echoes (258); the drop pickup was not shown (the fixture's target stood on a roof, the guest fell to its death there) |
| 4 | Guest crash-restart mid-session | **PASS** | S2 (title join on): crash at the first `join: arrived`; host `Hunter1 restarted its game: its old peer is cleared`; restart `title join: in the world after 14.1 s` -> `join: arrived 0.5 m from the host`; no `no arrival` line, no held-connection refusal. S3/S4 the same (0.5 m) |
| 5 | Guest rest alone (NO) | **PASS** | S2 guest lamp 2102950 + NO: host `slot 1 rested at lamp 2102950 ... a rest load ... 0 member(s) here follow`, `host: arrived alone` 6.4 s after; host_kill victim 2410015 alive again (`327 alive, 3 dead` = the first baseline). No 60 s wait |
| 6 | Wipe -> one respawn for both | **PASS after a fix** | S2/S3 (guest from a direct summon): host `every member is down too (1 of 1): a wipe; the party respawns together (one load)`, guest `held death goes with the host's travel; no redirect, no leave`, both back at the lamp (S3 shot 110 s). **FAIL** in S3 for a guest that had title-joined: its death left the session (`BeginLeaveSession called from +0x13cdef6`), it loaded its own world and rejoined 52 s later by direct summon (one failure first). Cause and fix below; S4 (fixed): the same title-joined guest `own chr type 0 -> 1`, then the wipe: `funnel: guest_died from +0x1382663` -> `held death goes with the host's travel` |
| 7 | Prologue refusal | **PASS (title join, live) / unit (direct summon)** | A gap found by reading: the title-join path made and served a room while the host was not start-ready (only the direct summon had the batch 2 check). Fixed; S3 shows the refusal end to end (host `title join: slot 1 refused: Waiting: the host is in its prologue`, guest `title join: the host's room: 0 bytes ... (refused) - Waiting: the host is in its prologue` -> Continue in its own world) - but that refusal came 0.1 s after the host's own lamp load, before its start flags were read again (a false "prologue"), so unknown-start requests now wait a frame instead (S4: `waiting for a room here` -> served 2.5 s later). A real prologue state was reached (`host_flag=12101021:0` -> `step first-dream ... not ready`, S3/S4) but no join was tried in it (run order). `BB_TEST_NEWGAME` not run (its contract-screen failure in batch 6 is not diagnosed) |

**Fixes made (unit-tested, live-checked in S4):**
- **Guest chr type after a B2 load / title join** (`party_b2.cpp` `GuestChrTypeTick`, `party_death_rules.h`
  `GuestChrTypeFixDue`, switch `BB_PARTY_GUEST_CHR_TYPE=0` to turn off). The B2 status line showed it in every run:
  `type 1/7` after a direct summon, `type 0/7` after a host-death / wipe re-join (S1 98 s, S3 104.9 s) and after a title
  join (S2 77 s). PlayerIns +0x78 = 0 is an own-world body; the death dispatch `OnEvent_4000 0x13813D0` then takes
  SoloPlayDeath, which leaves the session at 0x13CDEF6 instead of PartyGhostDeath_2 (the B2 funnel at 0x1382663). It
  also explains S1's lone guest death (after the host-death load) leaving the session instead of "died alone: revived
  at its own last lamp", and type 0 makes host-only EMEVD branches (chr type 0) fire on the guest. Now set back to 1
  once the guest is alive in flow 4 for 2 s. S4: `own chr type 0 -> 1` after the wipe re-join and after the title join;
  no visible change on either screen (shots 100/195 s).
- **Title join refused in the host's prologue** (`party_dsummon` `TitleJoinBusy`, `TitleJoinHostTick`,
  `coop::LocalStartInPrologue`).
- Test aid: `host_flag=ID:0` clears a flag (DEBUG_SWITCHES).

**Open items (evidence above; not fixed here):**
- A guest that rests back into the host's area (NO at the Dream lamp to 2412951) gets two loading screens: its own
  alone travel (S2 132.9-139.0 s), then the host's queued rest load it follows (147.4-153.7 s); the host queues the rest
  load at 132.1 s but runs it only after the guest's re-join. Correct, but ~15 s of loads; one load would need the
  guest to follow the host's rest load instead of travelling first.
- Guest loot after a host death not shown directly: S1's pickup fixture chose a rooftop enemy (the guest fell to its
  death), S3's guest had no echoes to drop. Kill echoes after the death load were paid (258).
- Cosmetic: the loading screens show "Travelling: host_death" (an internal name) on both games; after a wipe load the
  host sees "The bell's resonance has beckoned Hunter1"; the host has an "Acquire Blood Echoes" prompt for a 0-echo
  stain; the guest's death screen says "You died. Returning to your world." while it stays.
- Test aids: a `guest_obj` / move step runs even when the guest is dead (S1); a die step S s after another die step can
  land inside the first wipe's load (S4 step 2: no second wipe). Use `#N` (join count) on the host side.
- The direct-summon prologue refusal (`HostRefusal` with `host_ready`) can see the same "not ready right after a
  load" moment; it only makes the guest wait for its next ring (30 s), so it was left as is.

**Open items fixed (pt1polish, 2026-10-14, head 61f8ee5 + fixes).** One live session (r2; an r1 start
ended before any step: the guest joined in the Dream before the host's start-lamp warp, so `--stagger 50`),
2 loopback instances, test saves, `--fresh-saves`, host `BB_PARTY_TEST_START_LAMP=2412953`
`BB_PARTY_TEST_DIE_ON_DOWN=4`, guest `BB_PARTY_SPLIT_TEST=no,no` and
`guest_lamp=2102950@20%m24_01,guest_lamp=2412953@20%m21,guest_die@20%m24_01,guest_obj=2411810@3%m24_01,guest_die@8%m24_01`;
screenshots every 5 s of both windows, looked at.

| Item | Fix | Live (r2) |
|---|---|---|
| Rest back into the host's area: two loads | The rest rides on the re-join (`deathrules::RestRidesRejoin`): the guest's `rejoin` op carries `"rest":LAMP`; the host queues the rest load, holds the re-join (`RestCarryHeld`, max 8 s until the load starts; `HostServesRejoin`), runs the rest load (the resting guest is not counted as a follower), then sends the snapshot. The guest raises its loading screen at once and keeps it until it arrives. A re-join arriving during a host load is served by that load's own re-sync (no second snapshot) | Guest `travels alone (headstone) to m24_01 ... after the host's rest load (one loading screen)` 116.7 s; host `re-join waits for the rest load it carries`, rest load 117.4-120.5 s (enemies reset), `re-join done after 6.7 s`; guest one loading screen 116.7-127.2 s (10.5 s, was 2 screens / ~15 s). Shot 120 s: the guest's loading screen |
| "Travelling: host_death" | `coop::TravelKindLabel` (plain text: "Respawning with the party", "Travelling to a lamp with the party", ...) for the toast / last event | Shot 155 s: the guest's loading screen toast "Respawning with the party" |
| Guest "You died. Returning to your world." | Banner filter on ShowSummonMessage 0x1BFFE80 (`deathrules::HidePartyBanner`): with B2 a guest never sees 103100/103200..103500/103101; join banners 102001..102799 are hidden for 15 s after a B2 re-join in a session older than 20 s (the host's "beckoned" after a wipe). `BB_PARTY_BANNERS=0`: off | `banner 103100 hidden (the guest stays with the party)` at both guest deaths; shot 150 s: "YOU DIED" only. The host showed no "beckoned" banner in r2 (nothing to hide; unit-tested) |
| 0-echo bloodstain prompt | The death's stain record 0x13D2910 is hooked at its 4 call sites: after a record with 0 echoes the flag (GameDataMan +0x28) is cleared and 0x1868A50(stains, 0) destroys the live stain (it builds one only for flag && show); a per-frame backstop clears a 0-echo flag. `BB_PARTY_EMPTY_STAIN=1`: as the game does | r2 had only the backstop (flag cleared after the record): the stain still stood after the respawn load (shot 190 s, "Acquire Blood Echoes" at the host's lamp) - the record builds the live stain at once (decompile 0x13D2910 end: flag = 1, 0x1868A50(.., echoes >= 0)). The record hook came from this: unit-tested, **not yet run live** (the session's re-run was used) |
| Test steps while dead / inside a wipe | `deathrules::TestStepWaits`: a step that moves / acts with the own chr waits while it is dead; every step waits while a B2 re-join runs; the delay counts from the end of the wait | `travel step 4 (guest_obj) waits: the own chr is dead` 150.2 s ... `no longer waits` 190.4 s after the wipe's re-join; step 5 `guest_die` 8 s later made the second wipe (`every member is down too`, 211.6 s) |

**Last live checks (pt1last, 2026-10-14, head 290f7cd + fixes).** Two loopback instances, test saves, `--fresh-saves`,
screenshots every 5 s of both windows, looked at. Session 1 (host `BB_PARTY_TEST_START_LAMP=2412953`
`BB_PARTY_TEST_DIE_ON_DOWN=4`, steps `guest_die@20%m24_01,guest_die@25%m24_01`: two wipes, the host at 0 echoes),
session 3 (host `BB_PARTY_PROGRESS_TEST_CLEAR=12101021` = step first-dream, guest `BB_PARTY_TITLE_JOIN=0`), and
one re-run of session 1 with the fixes.

| Check | Result | Evidence |
|---|---|---|
| 0-echo death: no stain | **FAIL, fixed, PASS** | Session 1: the death's record hook removed the stain (`its bloodstain removed`), but "Acquire Blood Echoes" stood at the lamp after the respawn load (shots 130/140 s). Cause: the respawn load calls 0x13D2910 again with record 0; that path skips the record but still sets GameDataMan +0x28 = 1 and builds the stain from the old 0-echo record (decompile: `LAB_013d2cf8`). The hook now checks every call (`deathrules::SkipEmptyStainRecord(party, flag after, record echoes)`). Re-run: `bloodstain record (record 0) with 0 Blood Echoes: its bloodstain removed` at 113.3 s / 178.5 s; no prompt at the lamp after either wipe (shots 130/135/200 s) |
| Host "beckoned" after a wipe | **FAIL, fixed, PASS** | Session 1: "The bell's resonance has beckoned Hunter1." on the host at 135 s, no `banner` log: the join banner (102001 = menu FMG 30 "...beckoned <?joinName?>.") is built inline by the join handler fragment 0x196BAE9 (pending byte 0x55813D8 = 1, id from 0x553D784 + type * 0x80), not via ShowSummonMessage. Now its call 0x196BE0D -> 0x196F360 is hooked and a hidden id clears the pending byte. Re-run: `join banner 102001 shown` at the first summon (64 s), `banner 102001 hidden (a re-join...)` after both wipe re-joins (128.4 / 193.6 s); shots 130-140 s and 195-205 s: no banner |
| Direct summon into the host's prologue | **PASS** | Session 3: host `test: cleared ... 12101021:1->0`, `step first-dream ... not ready`; guest gathered to m24_01, rang; host `direct summon: slot 1 (Hunter1) refused (the host is still in its prologue ...; wait prologue)` 4 times (every 30 s), guest `the host's answer: refused by the host - Waiting: the host is in its prologue`; shot 65 s: the guest alone in its own world with "Waiting: the host is in its prologue" on screen, the host alone. `BB_TEST_NEWGAME`'s contract-screen failure not diagnosed (the flag clear made the prologue state instead) |

## Extra problems: status (pt1extra, 2026-10-14)

The open "extra problems" of sections A, B, D and "Additional problems" checked on head (cf9435d) and fixed where
still real. Live: 3 loopback runs (2 instances, test saves, `--fresh-saves`, screenshots every 10-20 s); runs end
at the queue's 240 s lease, so the 10-ring sweep got 7 rings.

| Item | Still real on head? | Fix | Evidence |
|---|---|---|---|
| Direct summon failed 8 of 10 (G4) | **Root gone, guard added.** Every failing ring in G4 logged `SpEffect 9005 applied, was already on`, every success `now on`: the bell's effect survived a cross-map join that sat in flow 2 with no load (bug 9, fixed in pt1join), and an "already on" bell makes no new summon entry | `party_b2.cpp` / `party_dsummon`: with the room here and no waiting entry 2.5 s on, the bell's 9005 is dropped and re-applied once (the entry wait restarts); the blocking condition (no world / a load / no entry, 9005 on/off) is logged once per request. Test aid `guest_home@S` (the guest's own leave, the link stays) | Unit (`party-dsummon-test`). Live run 1: `guest_home` x7 alternating 2 s / 15 s after each join: **7 of 7 joined** (3.0-3.5 s after the request), every ring `now on`, no re-ring needed, no FAILED; the guest beside the host after each (shot 180 s). The stale-bell state itself did not occur on head |
| Guest left the session 92.7 s after a join (G4:10655) | **No (root fixed by bug 9).** The guest had joined a room of another map (host in the Dream, guest in m21), never entered the host's world (host side: its chr at stage 0xffffffff, (0, 4.2, 0), HP 1), sat in flow 2 and the game dropped the session (+0x178a51f) | none beyond pt1join's no-cross-map rule | Live run 2: the host used its lamp 5 times with the guest beside it (on head a lamp use at the lamp you stand on is `already standing at it: no warp, no load`); the guest stayed a client throughout (no session change after its join; shot 190 s: both at the lamp) |
| Catch-up replay of a cutscene the guest watched in its own world (G2) | **No**: fixed in bfe5bd1 (own-world 2002 plays `NoteSeen`, the replay checks `Seen`) | - | unit `party-story-test` |
| Stale remote chr ghosts (32 lines) | **Diagnostic only.** Every playtest line read was a warp or a re-creation: one remote chr per member, at the old spot or at (0, 0, 0) HP 0/0 while the game builds it (H5:10950: the dead chr removed, the new one shown 1 line later). No second PlayerIns survived; revive in place is off by default | `party_hud_game.cpp`: a chr is "stale" only when built (max HP > 0) and far from every report for 5 s; the line names the chr and member counts | unit `party-hud-test`; live runs 1-3: 0 stale lines (7 re-summons, 5 lamp uses, deaths) |
| Misleading NAT text while the host restarts (G4 32908) | **Yes** | `party_link.cpp` `host_no_answer_hint`: for 60 s after the host ended the party (BYE) or an open link dropped, "the host left N s ago (restarting?): waiting for the host to come back" instead of the NAT advice (message only; process-wide, so a Rejoin's fresh link keeps it) | unit `party-link-test` |
| Long host loads 40-60 s (H4:1855, H4:2591) | **Yes, and not a load**: both times a member had left the room with an error and the host's own game ran OnLeave_Limit's send-home (`funnel: guest_sent_home from +0x138a51c (role host)`); its "home" load had no return data and went to the **title** (`title:` lines, `world down`), then back in | `party_travel.cpp` `RetargetSendHome`: a host never runs that send-home (logged and dropped); the session handler closes the room for the member that left | code read of both cases; not reproducible on one PC (needs the member's error leave) |
| Host roster "prologue" while READY (G4:21464) | **Yes**: right after each host load the start flags are unread (`step no-world`), which the roster showed as "prologue" | fixed on head by the prologue work (#19): the roster's "prologue" comes from the known start step only (`LocalState(s, st.start_prologue)`), an unknown step after a load is "home" (pt1extra's own variant was dropped in the merge) | -. The map id `-` after a load: the bell waits for its own map (pt1join); the HUD and roster treat 0xffffffff as unknown; no other consumer found |
| Rot: real lamp use called "the respawn" (H5:6072/10228) | **Yes** | `party_balance_rules` `LampWarpIsRespawn`: respawn only when dead at the warp or not up again since the death (was: any warp within 20 s of a death) | unit `party-balance-test`; live run 2: 5 host lamp uses logged `lamp rest`; run 3: guest death `rot on, 1 stack`, notice shown |
| #21 Rot kept after "triggering" a lamp | **Yes** | `party_balance.cpp`: a dialog (MenuMan +0x78) opening with the player alive within 4 m (3 m height) of a lamp revive spot of the loaded map is a rest (the lamp's "Return to the Hunter's Dream?" box, also when answered NO); a level up is a rest | unit; **not seen live**: in runs 2 and 3 the scripted Cross presses ran while the guest was still dead after `guest_die` (press steps do not wait for the own chr), and the one re-run was used. Item boxes (Sword Hunter Badge, shots run 2 190 s, run 3 70 s) did not trigger it. Open: one live check of the lamp box; a title-joined guest's home lamp warp at its leave counts as a rest (run 3 205 s) |
| `__error` ~700k calls per run (G1:13801) | **Not a cost**: counted only because the guest ran with Tracing on (every import through the trace thunk). With tracing off `__error` is bound directly to `runtime_thread.c` `guest_error` (returns a thread-local pointer); ~5.4k calls/s, tracking `pthread_mutex_trylock` 1:1 (a guest lock wrapper reading errno after each trylock) | none (tracing is off by default since 0.6.1 and the launcher warns) | G1 counts 144k / 368k / 698k vs trylock 167k / 404k / 707k |

Seen in these runs (not fixed here): a guest that dies within a second of its title join (run 2: `guest_die` 1 s
after joining while the title-join flow was still 1-3) dies again after the respawn, leaves the session through
SoloPlayDeath (+0x13cdef6) and goes to the title; it rejoined 50 s later.

## rotfix (2026-10-14): #21 live check, going home is no rest, a death right after a title join

Two sessions (one sweep + one re-run), 2 instances, loopback, host `BB_PARTY_TEST_START_LAMP=2412951`, guest
`BB_PARTY_ROT_KIND=0`, `BB_PARTY_DIRECTOR_TEST=guest_die@1,guest_obj=2411951:1.2:0@8%m24_01,guest_press=0x4000@3%m24_01!,guest_press=0x2000@5%m24_01!,guest_home@12%m24_01`
(the new `!` makes a press step wait like a step with the own chr: alive, world up, no re-join running).

| Item | Result | Evidence |
|---|---|---|
| Death ~1 s after a title join (the pt1extra note above) | **PASS, not reproduced** (both runs) | guest `title join: in the world after 8.4 s`, `joined #1` 18:05:45.4, `guest_die` 18:05:46.4 (HP 647 -> 0), `funnel: guest_died from +0x1382663`, `died alone: revived at the host's last lamp 2412951 in the loaded map ... no load`; cooperators stayed 1, no `BeginLeaveSession` until the scripted leave; status `type 1/7` before the death. Shots run 2 304 s: both hunters at the lamp. The type was already 1 at the death in both runs (the pt1verify chr-type fix, which the playtest build did not have, explains the playtest's leave + 50 s re-join). pt1extra's die-again / title case did not recur with this timing (its death came while the title join's flow was still 1-3; the step clock here starts at `joined`, flow 4); if it comes back, its log needs the chr type at the death. Hardening: the fix now runs 0.25 s after the arrival (was 2 s) and at once at HP 0 (`party_death_rules.h` `GuestChrTypeFixDue`, unit) |
| Going home = rest | **FIXED, live** | before: the title join's home warp ran the lamp observer (pt1extra run 3). Now `RunLampWarpNotRest`: guest `lamp warp 2102961 (title join: going home): no lamp use of the player, not a rest`; no `lamp rest` line in the guest log, the rot (2 stacks, max HP 575) kept after the trip home. The fall recovery's warp is no rest either |
| #21 lamp box | **box live; cleanse not live** | run 1: `guest_obj=2410951` is not the lamp (`not loaded`); the revive spot faces away from the lamp (shot 84 s: no prompt). Run 2: `guest_obj 2411951: target (-196.75, -28.65, 65.39)`, prompt "Return to the Hunter's Dream" (shot 312 s), Cross -> "Return to the Hunter's Dream? YES / NO" (shot 316 s), Circle. No cleanse: the guest stood 5.7 m from the revive spot (-193.4, 68.5), beyond the 4 m rule. Fixed: the distance to the lamp object (entity = ReturnPoint id - 1000 or - 2000, `worldact::ObjectPoint`) counts too, and a dialog opening within 15 m of a lamp spot that is not taken as the lamp's menu is logged (`dialog N opened ... no lamp menu, not a rest`), so the next run shows whether MenuMan +0x78 sees the box at all. Not re-run (one re-run per task) |

## menuhide (2026-10-14): #21 cleanse live; party overlay under the game's own menus

One session (2 loopback instances, test saves, `--fresh-saves`, screenshots every 2 s; a first launch was stopped
because the host's start-lamp "Travel with the party?" went unanswered, re-run with `BB_PARTY_SPLIT_TEST=yes`).
Host `BB_PARTY_TEST_START_LAMP=2412951 BB_LAMP_TRAVEL=0`, steps `host_obj=2411951:1.2:60@70%m24_01,host_press=0x4000@3%m24_01!,host_press=0x2000@6%m24_01!`;
guest `BB_PARTY_ROT_KIND=0`, steps `guest_die@3%m24_01,guest_obj=2411951:1.2:0@8%m24_01,guest_press=0x4000@3%m24_01!,guest_press=0x2000@6%m24_01!,guest_press=0x8@4%m24_01!,guest_press=0x4000@3%m24_01!,guest_press=0x2000@4%m24_01!,guest_press=0x2000@2%m24_01!`.

| Item | Result | Evidence |
|---|---|---|
| #21 lamp's warp menu (direct lamp travel, the new path) | **PASS, live** | guest `Lamp travel: lamp 2412951 (m24_01_00_00): the warp menu instead of the Dream question; 7 entries`, same tick `Party balance: lamp menu at lamp 2412951 (spot 5.8 m, lamp 1.2 m, dialog 0): rot cleansed (2 stack(s))`, then `max HP now 719 (HP 719) ... (0 stack(s))` (was 575, 2 stacks). Shots: the HP bar before (shorter) and after (full length); the warp menu "Awaken Above Ground" with the notice "The rot is cleansed" |
| #21 old box (`BB_LAMP_TRAVEL=0`) | **PASS, live** | host `lamp menu at lamp 2412951 (spot 5.4 m, lamp 1.2 m, dialog 1): a rest (no rot to cleanse)` (the host had no rot); shot: "Return to the Hunter's Dream? YES / NO". MenuMan +0x78 does see the box (dialog 1) |
| Party overlay over the game's menus | **FAIL with IsFullscreenMenu, fixed by RE (not re-run)** | The game's own `IsFullscreenMenu` (FUN_0176bb70: MenuMan +0x3c/+0x40/+0x80/+0x88/+0x98/+0x9c/+0x120/+0x138/+0x13c) stayed 0 in the inventory and the warp menu (no `game full-screen menu open` line; shots: the party panel and the notice crisp over the inventory and the warp menu, the name tag over the dimmed YES/NO box). The in-game menus are the menu flow's (`*0x5562870` +0xa88 = the open menu, 0 = none; FUN_01baf8c0 opens "MenuMan.IngameTop" only while it is 0; the game's menu-bits function 0x176b7e0 reports it as 0x800008, which lamp_travel logged right after the warp menu opened while every IsFullscreenMenu field was 0). Now: that OR IsFullscreenMenu hides the party panel and tags and holds the notices / alert / NPC caption (their time stops); MenuMan +0x78 (the generic YES/NO box) hides the name tags only. Each change is logged (`Party HUD: game full-screen menu open/closed (flow menu 0x...; ...)`). Unit `party-hud-test`; **open: one live look** at the inventory, the level-up screen and the warp menu |

Seen: the guest's `death 1 in the prologue: no rot (#19)` after a join in Central Yharnam (its test save), and `2 stack(s) kept from the last game` despite `--fresh-saves` (the rot file is not part of the reset).

**Follow-up (menuhide2, 2026-10-14, 2 live runs, `--fresh-saves`, shots every 2 s).**
- Prologue misread, **fixed, live**: in another player's world the event flags are that world's (the guest's
  `start` line read `step opening: 12410000 0, 9401 0 ...` while summoned into the host's Dream / Central Yharnam);
  it held story / item / progress syncs, gave no rot and wrote `party_prologue.txt` = 1. `UpdateStart` now treats
  a player in another world (session client or WorldTransitionState +0x1592 == 0) as ready and past its prologue,
  without reading or saving the flags: `start (prologue_solo): in the host's world (m24_01_00_00): not in its own
  prologue, ready`; the guest's death there: `death 1; rot on, 1 stack(s) (this one: health)`, max HP 719 -> 647.
- Rot kept across fresh saves, **fixed, live**: `--fresh-saves` also removes `party_rot.txt` and
  `party_prologue.txt` (a seeded `stacks 2` file, then the last run's `stacks 1`: both runs `0 stack(s) kept`), and
  stacks seen while the own save is 3 s in its prologue (a new game) are dropped (unit `party-balance-test`).
- Overlay under the menus, **live**: hidden over the lamp's warp menu "Awaken Above Ground", the top menu, the
  inventory and its full item description (Square), back after Circle; MenuMan's IsFullscreenMenu fields stayed 0
  throughout, the flow menu +0xa88 carried it. The lamp's YES/NO box also opens through the flow menu (+0x78 follows
  ~35 ms later), which hid the panel too; a flow menu joined by the box within 0.25 s is now that box
  (`game dialog box via the menu flow ...`): live, the panel stays and only the name tag hides. Not seen: the Doll's
  level-up screen (the guest's Cross went to an item box, then Circle broke off the Doll's greeting).

## Bug 20: "Travel with the party?" shown on both players (splitq, 2026-10-14)

**Players' report (0.6.0):** the question appeared on both players. Expected: only the player who used the
lamp / headstone / Hunter's Mark is asked; the others get the outcome (the party travel with a loading
screen, or nothing).

**Evidence (playtest logs).** Each held lamp has a twin on the other side with the same lamp id:
- H3:2822 host `lamp 2412951 (host): held for the choice` (Dream headstone) and G3:41867 guest `lamp 2412951
  (guest): held for the choice`; the guest stood 4.5 m from the headstone (G3:41863 status `pos (-8.2, -6.0,
  -14.3)`, the host at the headstone `(-5.5, -6.1, -17.8)`), its question opened before the host's travel
  arrived (G3:41892 `host travels (#...333 lamp 2412951)`). The same at H3:2095 / G3:5267 (lamp 2412951 /
  2412950, both players at the headstone) and the reverse direction: the guest's own world lamp (G3:40116
  `local flag change(s) kept local: 72410101`, G3:40201 `lamp 2102950 (guest): held`) gave the host, who was
  fighting 20 m away, `lamp 2102950 (host): held` (H3:3221) and a question nobody answered (H3:3429 `no answer in
  120 s`). H5 / G5 repeat the pattern (H5:8151 / G5:75197 and others), sometimes with two party travels.
- Not the follow travel: every twin question opened before the host's TRAVEL event (replays also pass
  `g_in_replay`), and not a stale dialog (bug 12 closes those after 30 s now).

**Cause (live, splitq runs, two loopback instances).** A lamp's / headstone's talk script sets its "used" flag
(world lamp 7AAB01xx for common event 7200, headstone 72102xxx / 72103xxx for m21 event 12107000) on the
user's own game: `lamp flag 72102410 set by this game` 4 s before the user's warp. The warp is run by the
event's `2003[49]` on EVERY game in that map: the other player's game called the lamp warp 0x13CDF30 from the
same EMEVD site 0.05 s later (`warp called from +17c1cf5 < +17b948b` on both) although it set no flag, got no
0x0e / 0x0f packet and no mirror bit for it (one run also showed the world mirror carrying 72102410 to the
guest). The split interceptor treated every non-replay 0x13CDF30 on a party member as that player's own use,
so both were asked; each "YES" could start its own party travel.

**Fix.** `deathrules::PlanLampOrigin`: a lamp warp is this player's only when its own game set a lamp /
headstone flag in the last 10 s (the progress SetEventFlag / CompleteEvent hooks, `LampTriggerFlag`), or a test
step used the lamp, and no peer's lamp flag / event (packet 0x0f / 0x0e) came after it. Otherwise the warp is
dropped: `lamp N (guest): not this player's use (...): ... that member chooses`. The asking player sends EVENT
`split_choosing`; the others log `host Hunter0 is choosing travel (...)` and show the toast "X is choosing
travel". The world mirror no longer carries the lamp / headstone trigger flags (per-player triggers). The
Hunter's Mark (item use) is unchanged. If the SetEventFlag hook is missing, every warp asks as before.

**Live (splitq run 5, one session):** host headstone: host `lamp flag 72102410 set by this game`, `lamp 2412950
(host): held for the choice`; guest `lamp 2412950 (guest): not this player's use` + `host Hunter0 is choosing
travel` (guest window: the loading screen, no dialog). Guest headstone (guestheadstone): guest `lamp flag
72102410 set by this game`, `lamp 2412950 (guest): held for the choice`; host `lamp 2412950 (host): not this
player's use` + `guest Hunter1 is choosing travel`; screenshots: the question only in the guest's window, the
host's window shows the toast "Hunter1 is choosing travel". Before the fix (runs 1-4) the same host headstone
gave both `held for the choice` lines 0.04 s apart. Not run live: a world lamp press (the lamppress fixture did
not get the lamp prompt in this save), a 3-player party.

## Bug 22: lamps in "Join a party" mode with no host (lampsave, 2026-10-14)

**Report (0.6.0, after the playtest):** the guest played "single player" (tracing off, uncapped FPS ~124/s) and
found that no lamp worked. It was not single player: the launcher was still on "Join a party" and no host was
running, so the game ran as a party guest in its own world (role guest, `Party split: on`, the lamp warp
0x13CDF30 replaced, the lamp gate +0x49 patched, guest lamps on), with the party save (user\savedata_party,
his only save folder).

**Evidence.** The guest's log of that session (G6, 18:16): the link never came up (no TCP / UDP answer, the
title's "Failed to acquire server information"); he continued slot 0 (the party character, level 10), used a
Dream headstone to lamp 2412950 (it worked: `lamp rest`, a load into m24_01), then spent ~8 min in m24_01
(items, a hit, menus) and restarted. Every lamp warp passes the lamp-use observer first (`Party balance: lamp
rest`), so no lamp's warp ever ran in m24_01 and nothing of ours held one (no `held for the choice`, no
request). Whatever he pressed at the lamp is not in the log; the second process quit at the title.

**The save is fine.** `savedata_party.zip` (each slot file's header holds the event-flag block's offset at +0x34 and its size at +0x38: size 150001 = the 1200 x 125-byte pool + 1): no lamp / headstone trigger flag
(7AAB01xx for common event 7200, 72102xxx / 72103xxx for event 12107000) is set in any slot or backup. The
EMEVD completion flags 7215 / 12107015 (event id + slot, written by every ended event, 0x12ED100) are set,
as in ordinary saves (the test save has 7205, 7210, 7211, 7216-7218): they do not stop the lamp (live: slot 0
with 7215 set, its lamp 2412950's 7200 ran and warped). Slot 0 has only the Sickroom lamp lit (1AAB7810+20n):
the lamps lit during the party were lit in the host's world. No repair tool or load-time repair is needed.

**Live (head, two instances, copies of his save, 127.0.0.1, no host on the code's port):** solo and "Join a
party, no host" side by side, the same scripted lamp use (lamp_travel test aid `titleany;dream;startany;
start=2412950;lamp=2412950`): both got the lamp's "Return to the Hunter's Dream?" box (talk command 0x11) and
travelled to the Dream with a loading screen. So head already works for the reported case; the 0.6.0
failure itself was not reproduced (0.6.0 has no scripted lamp aid; its code path for this case is the same).

**What was still wrong for a guest with no host (and a host alone):** `SameMapLampWarp` (B2) turned a lamp
warp to a lamp of the loaded map into a party in-world move without a load (no world reset, no rest), and
swallowed it when standing at that lamp ("already standing at it: no warp, no load"); live before the fix:
`in-world warp (own move, lamp 2412950) ... no load` on the unconnected guest. A guest's Hunter's Mark /
death redirect (`RedirectGuest`) checked only the party role, so in its own world it could follow a stale
host lamp or travel from an earlier session.

**Fix.** `deathrules::PartyTravelActive` / `GuestInHostWorld` (unit `party-death-rules-test`): the party's
lamp handling applies only with somebody to travel with - a guest in the host's world (session client, or
the flag store still on the host-world overlay, load mode 1: a phantom's Mark leaves the session before the
funnel) or a host with members. Otherwise: `lamp N in the loaded map: no party here (session role R): the
vanilla lamp warp`, and `... in the guest's own world (...): its own, as in single player`. The split
question, the guest's lamp request and the IsClientPlayer lift were already gated on the client role; the
lamp gate +0x49 and the IsMultiplayerInProgress lift only lower values that are already 0 without a session.

**Live after the fix (one instance, "Join a party" with no host, test save, direct lamp travel):** lamp
2412950 -> the travel list -> lamp 2412952 of the same map: `no party here (session role 0): the vanilla lamp
warp`, a loading screen, `arrived: lamp 2412952, map m24_01_00_00, 0.1 m from its spot`; screenshot: the
player at the Central Yharnam lamp with the area banner.

**For the guest:** nothing to repair. With 0.6.1, lamps work in "Join a party" mode even when the host is
not there. For single player, set the launcher's party mode to "Off": that plays user\savedata (his own
characters; the party character lives only in user\savedata_party and is not touched by solo play).
