# Party timeouts audit (0.8.0, item 23b, CL-b2f7f6)

Question: do travel, catch-up, join, warp, boss/fog, rejoin and revive waits count seconds, not frames or
ticks, and do wall-clock waits wrongly keep running while a loading screen is up (45 s on a busy machine)?
Origin: bbhost 8f2d32a (frame-counted waits ran out inside a loading screen, where frames flip hundreds of
times a second).

Result: no wait that bounds a duration counts frames or ticks. Every give-up limit in `gpu/shim/party/*` is a
steady-clock time (`Now()`, or the injected `now` / `t` of the rules classes). The frame counters that exist
(`kStableTicks`, `kStableFrames`, `st.stable`, `++steady < 60`) are "the gate held for this many frames in a
row" debounces; a loading screen resets them (the gate is false while loading), so they cannot run out inside
a load. Their only fps dependence is the length of the debounce (30 frames is 0.5 s at 60 fps). The real
defects were wall-clock limits that kept running during a load: 8 limits found, 8 fixed.

## Found and fixed

| file:line (after the fix) | waits for | unit | risk | fix |
|---|---|---|---|---|
| `party_start.cpp:329` (`Catchup::Step`, `kWarpTimeout` 40 s) | the late joiner's gather warp load | s | The 40 s ran from the warp, so a load of 40 s or more ended the catch-up as "no load after the gather warp (timeout)" while it was still loading | time with the world down is added back to `phase_at_`; the limit is now "no load started within 40 s". Unit test: a 55 s load ends as "arrived" |
| `party_b2.cpp:9019` (`JoinBesideTick`, 60 s from the client role) | the arrived guest to be put beside the host | s | the summon's host-world load (up to 45 s) used the 60 s; the beside-the-host fix then never ran | `pending_since` shifted by the loading time (`LoadingDt`) |
| `party_b2.cpp:9231` (`CarryGameDataTick`, 60 s from the A5 arrival start) | the A5 arrival load, then the guest's GameData fix | s | a slow load expired it: the guest stayed a non-cooperator (host counts 0 cooperators, its watchdog ends the session) | start shifted by the loading time |
| `party_b2.cpp:9312` (`CarryArrivalTick`, 60 s from the cross-map join) | the guest up (not loading) in the host's map, to start the one arrival load | s | the join's own loads (gather, summon) ran the 60 s out; the first tick after the load gave up and cleared `g_carry_hold` | start shifted by the loading time |
| `party_b2.cpp:12143` (title join, host loading, was 40 s) | the host's room reply while the host loads its world | s | a host load of 40 s or more failed the title join: the guest Continued into its own world | 60 s when the host is loading (the unreachable 70 s cap is now 90 s); the 10 s limit for a host still at its title is unchanged |
| `party_b2.cpp:9755` (`AwaitSnapshot` in flow state 2, was 45 s) | the host's destination, sent after the host's own load | s | a host load of 45 s fell back to B1 (leave, bell) | 55 s from state 2 (the game's own flow leaves at 60 s); the snapshot wait before state 2 stays 45 s (the smoke greps its text) |
| `party_b2.cpp:10101` (host `Resynced`, was 60 s) | a guest's "arrived" after its host-world load | s | guest load (45 s) plus prep passed 60 s: the host ended the re-join and rewound tasks while the guest still loaded | 100 s (the guest's `Loading` gives up at 90 s) |
| `party_story.cpp:877` (`GuestStory::Step`, `kMirrorTimeout` 60 s) | a mirrored cutscene's gate (world up, steady) | s | a guest in a long load lost the host's scene | time with `st.loading` does not age the mirror. Unit test: a 100 s load keeps it |

## Checked, not changed

| file:line | waits for | unit | verdict |
|---|---|---|---|
| `party_travel.cpp:478` `kPendingTimeout` 180 s, `:492` `kActiveTimeout` 120 s, `kStableTicks` 30 | guest travel replay gate; the replayed load to finish | s / frames | wall limits are 2.7x and 4x a 45 s load; the 30 frames reset while loading |
| `party_travel.cpp:435` `kEmptyRoomTimeout` 180 s | the host's empty-room check after a travel | s | contains the load with a 4x margin; the check runs only once `load_seen && in_world` |
| `party_b2.cpp:9544` `AwayLoading` 90 s, `:9849` `Loading` 90 s, `Rewinding` 60 s | the guest's own loads | s | these wait for the load itself; 2x a slow load. A load over 90 s is a hang. The `st.stable >= 30` tests are post-load debounces |
| `party_b2.cpp:8324` alone-travel pending 40 s, `:8328` `++steady < 60` | a load to start; 60 steady frames | s / frames | the 40 s test is skipped while loading; the 60 frames reset on a load |
| `party_phantom.cpp:507-511` `kLoadWait` 45 s, `kTimeout` 240 s | expected load to start; refill gate | s | 45 s is "load to start" |
| `lamp_travel.cpp:1080` 60 s after a warp | a load to start | s | fine; the arrival check (3 s steady) starts after `load_seen` |
| `world_warper.cpp:798/866/881` 300 s / 25 s / 240 s | title to world; load to start; load to end | s | developer tool, generous |
| `party_gather.cpp:45` `kArrivalWaitS` 120 s | arrival beside the host | s | contains one load with a 2.7x margin |
| `party_invade.cpp:196` 90 s | a pending reward until the world is up | s | contains the load, 2x. Left; low risk |
| `party_story.h:239` `kTravelTimeout` 180 s, `kRemoTimeout` 600 s | ending travel; a scene | s | fine |
| `party_dsummon.h:188-191` 4 / 8 / 8 s | host answer, bell entry, join session | s | network and UI waits, not loads |
| `party_progress.cpp:2352` `kMode0Window` 600 frames | the post-load window in which mode-0 flag changes apply | frames | counts only steady-world frames (a load resets it), but its length depends on fps (10 s at 60 fps, 4 s at 144). Not a load problem. Changing it alters behaviour and needs a live fps sweep: open |
| test and fixture timers (`lamp_travel` test, `party_worldact` fixtures, `test_newgame`, `party_split` asks) | test aids | s | not player paths |

The shared helper is `LoadingDt` in `party_b2.cpp`: seconds a loading screen was up since a timer's previous
call (at most 5 s per call, so an idle gap is not load time). A timer whose wait is not the load shifts its
start by it.

## Boss-rush aftermath flags (bbhost 78f2ca0, 2d09097, bf60a65, 8f2d32a) against our boss code

bbhost's boss rush reopens a fight by clearing the boss's flag family, and saves and restores the flags the
defeat event sets outside the family (map progress, the boss's global flag, a few others; Ludwig's global flag
makes his events end at once while it is set). It needs a list because the plugin clears flags itself.

Ours does not: the port has no boss rush, practice or reset code (not planned). A party wipe leaves the boss
undefeated, so nothing outside the family was set, and the game's own re-fight events run (smoke `lwf`).
A defeat sets its aftermath flags through the game's own events, and our flag sync (`party_progress`) copies
them by category from tables derived at runtime from the player's game files (`party_gamedata.h` flagRows);
there is no hand-kept list. Mismatches against bbhost's list: none in a reset, because there is no reset.
The one place that knows a flag outside a family is the m34 confinement wall in `party_bossfog_rules.h`
(Ludwig's global flag; it was in the repo before this audit). No ids were copied from bbhost.

Open: if boss practice or a "reset boss" tool is ever added, it must read the aftermath set from the loaded
event scripts (the set-flag instructions of the defeat event) at runtime, not from a table.

## Needs a live run

Not run (no game slots): a slow-load check (a loading screen of 45 s or more, for example under load or with a
delayed load) of the title join to a loading host, the A5 cross-map join, a host travel with a slow guest, a
mirrored cutscene with a slow guest, and the catch-up gather warp.
