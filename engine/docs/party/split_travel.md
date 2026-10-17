# Split travel: each player travels alone, the session stays (ERSC B3 / B5)

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA; bbhost = ours + 0x400000).
Builds on b2_seamless.md (B2: the K hooks, CSMultiPlayerInsTask, the flow states), travel.md (B1,
the funnel, edgetravel's stale map-hit guard) and guest_services.md (lamps / headstones).

Marks: **[C]** confirmed in disassembly / decompile, **[R]** confirmed at runtime here, **[I]**
inferred.

User decisions this follows (ERSC_PARITY.md "Design decisions", binding):

* A lamp, headstone or Hunter's Mark **asks each time**: travel alone or travel with the party.
* The Hunter's Dream is **always shared**: everyone in the Dream is in the host's Dream together.
* Death outside a boss fight: every player, host included, respawns at their **own** last lamp,
  even in another map; the others play on (the hostdeath agent uses the mechanism below).
* The host is needed: the shared world exists only while the host plays.

## 1. Feasibility (answers)

**Verdict: feasible with the engine's own pieces; implemented for lamps and headstones.** A
member can stand in a different map from the host in host-world mode for as long as it likes, and
the session, the room and the party link stay up. The pieces:

1. **A guest-only host-world load needs no host snapshot.** The B2 guest load goes flow +0x84
   1 (await snapshot) -> 2 (applied) -> 3 (host-world load) -> 4. Only state 3 matters for the load
   itself: the MoveMapStep ctor `0x1937570` picks host-world mode (GSM +0x1592 = 0, the flag overlay)
   from +0x84 == 3, and the old stage dtor keeps the room and the tasks in state 3 [C]. The flags
   come from the host-world overlay, which the host-world mirror (event_flags.md 12) keeps filled
   with **every** group of the host's world, not only the snapshot's 12 [R]. So a guest can go 4 ->
   3 directly and warp (K7 keeps the room in the funnel): an *alone load*.
2. **The host's `CHR ` world blob does no harm in another map.** WorldChrMan `0x191BDD0` applies
   the packet-0x11 blob record by record, matching each entity id against the loaded map's chr
   groups (`(group key ^ id) < 0x1000000`, then the entity inside the group); records of another
   map match nothing and are skipped; the blob is freed after the first apply [C]. Enemies of a
   map the host is not in are therefore fresh (as after a rest), simulated by that player's game.
3. **Remote chrs in another map.** Every PlayerIns dies with its own world; remote chrs exist only
   through their member task's STEP_Create, which B2 drives (rewind). A remote chr whose owner
   stands in another map just follows the owner's coordinates (the edgetravel stale map-hit guard
   keeps it from crashing on streamed-out collision) [R, travel.md 7]. Split travel removes such a
   chr (`0x15BC080` slot release + `0x19186E0` WorldChrMan remove, as STEP_DeleteWait does, already
   live in the lone-death respawn r17) and keeps its task; the chr is built again (rewind) when the
   two meet.
4. **Packets of a snapshot sent to a member in state 4 wait.** The receive `0x1783160` pops 0x19 /
   0x11 / 0x10 only in flow state 1 (`0x17893E0(flow, type, buf, size)`, role 3/6, from the session
   owner) [C]; anything the host sends to a member that does not follow stays queued and would be
   applied at that member's next state 1 - stale (another map, older flags). Split travel drains
   those three types right before any state-1 entry.
5. **Nothing else ends the session while apart.** OnMatchingCheck (`0x13808B0`, K5) is the one
   game path that leaves when a member's chr is missing; it is suppressed while members are apart.
   The host watchdog (party_watchdog) counts members that are apart as present. The network
   flow's 60 s timer runs only in states 1..3.

What is **not** possible cheaply: an in-world warp to a lamp of the same map (no load). Bloodborne
lamps only go to the Dream and headstones to a lamp, so "same map" only happens for a Hunter's
Mark; the lamp's spot is a ReturnPointParam row resolved by the load (MSB region), and the chr
warp `+0x5F8` needs the position. A guest's warp into the map the host is in is a load for that
guest only (the B2 re-join, with the host's fresh snapshot).

## 2. Authority while apart

| What | Rule |
|---|---|
| World flags (doors, lamps, bosses, NPC state) | The host's world: the mirror keeps every player's overlay current. A flag a guest sets in a map the host is not in stays in that guest's overlay only (D3: guest flag routing, as `lamp_lit` does for lamps, is the next step) |
| Ordinary enemies | Simulated by each player's own game in the map it loaded; they respawn on every load as in single-player |
| Bosses / unique enemies | Their defeat flags are the host's (K10 / D1); a guest's lone kill in a map the host is not in is not sent to the host yet (open, D3) |
| Item pickups | Vanilla client rules in host-world mode; the shared-world items are the host's (C3 grants) |
| Meeting again | The member whose world is older loads once (the B2 re-join, the host's snapshot) unless both are in the Hunter's Dream (no enemies: the chrs are just built again) |

## 3. Interface (party_b2.h) - for reuse (hostdeath)

```c++
namespace coop::b2 {
// Why a member travels alone (logs, the dead-respawn records).
enum class AloneWhy { Lamp, Headstone, HuntersMark, Death };
// Guest, main thread, world up, B2 on, a client in the host's world. Loads t's destination
// ALONE and stays in the session: the other members do not load.
//  - destination in the host's current area: a B2 re-join (the host re-sends its snapshot,
//    one load for this guest only), placed at the destination (not beside the host);
//  - otherwise: an alone load in host-world mode (no snapshot; flags from the mirror).
// `why == Death`: the player is dead; its own respawn records are kept (the death load goes to
// the guest's OWN last lamp). False: not possible now (the caller keeps the old behaviour).
bool TravelAlone(const TravelIntent& t, AloneWhy why);
// Host, main thread: the next own travel the funnel captures is alone (TravelIntent::alone):
// guests do not follow, the host does not re-send its snapshot after the load.
void HostTravelAloneNext();
// Members apart now (host: members in another area; guest: 1 when the host is elsewhere).
int MembersApart();
}
```

Wire (PartyLink EVENT "b2"): `{"op":"away","pos":[x,y,z]}` (guest -> host, it leaves the shared
area: the host drops its chr), `{"op":"rejoin","pos":[...]}` (guest -> host: re-send the snapshot
and the destination, as `respawn` without the death), `{"op":"meet"}` (either way: chrs built
again without a load, Dream). `TravelIntent::alone` ("alone": 1 in the travel JSON) marks a
host travel the guests do not follow.

For hostdeath (party_travel.h / party_b2.h):

* **Host death, the guests play on**: `coop::SetHostDeathAlone(true)` - every captured
  `TravelKind::HostDeath` is then sent with `alone` set; the guests stay where they are (no load,
  the host's chr is removed on their side), the host loads its own last lamp, re-sends nothing
  (`LastAloneCaptureAgo() < 10 s` at its load) and meets the guests again without a load when it
  is back in their area (SplitTick: chrs re-created, net sync re-registered).
* **Guest death, own last lamp**: `coop::b2::RespawnAloneAtOwnLamp()` (main thread, the held
  death's place: `HeldDeathTick` / `SoloRespawn`) = `TravelAlone(LampIntent(GSM +0x1528),
  AloneWhy::Death)`: a re-join when that lamp is in the host's area, else an alone load. The death
  load uses the guest's own respawn records, which already name that lamp.
* `MembersApart()` > 0 keeps the host watchdog quiet and K5 suppressed.

## 3a. Moves inside the loaded map: no loading screen (B4 / H1)

User priority (2026-10-11): only a real map change may show a loading screen. The lamp spots come
from the game data: `tools/party/ids_tool.py gen-inc` now also writes `kPartyReturnSpotIds` /
`kPartyReturnSpots` (x, y, z, yaw) for 75 of the 85 ReturnPointParam rows - the MSB revive event
named by `returnPointEntityId` (EVENT_PARAM_ST entry: base data +8 entity id, type data +0 region
index) and that region's position / rotation (POINT_PARAM_ST +0x14 / +0x20, degrees) [C]. Checked:
2412951 -> (-193.40, -28.65, 68.50) 45 deg, 2412952 -> (-124.8, -26.88, 64.3), 2102950 ->
(-9.2, -6.77, -19.4) -135 deg: the hosts' logged arrival spots. The 10 without a spot are Chalice
Dungeon / DLC points without a revive event (29xxxxx is never warped in-world: generated layouts).

`party_ids.h ReturnSpot(id)`, `b2::WarpToLampNoLoad(id, beside)` (the chr warp vfunc +0x5F8 when the
lamp is in the block the player stands in) and `b2::HostPartyWarpNoLoad(id)` (host + EVENT b2
`warp`: guests of this map warp beside it, guests elsewhere travel there with their own one load).

Audit of what loads a stage in a party, and what is same-map:

| Load | Same map possible? | Now |
|---|---|---|
| Host lamp / headstone / Hunter's Mark (0x13CDF30, Mark item funnel site 0x18FFA1D) | Mark to a lamp of this map; a lamp of this map from the question | in-world (host + the guests of this map), no load |
| Guest lamp / headstone / Mark "alone" | Mark, lamp of the host's loaded map | in-world, no load |
| Guest lamp "with the party" (lamp_request, host runs it) | as the host's | `HostPartyWarpNoLoad` first |
| Guest B2 follow of a host load (host death, stage warp, a host that loaded anyway) | destination block = the guest's block | `InWorldFollow`: no load; the guest waits for the host's `dest`, drains the snapshot packets, warps beside it, says `arrived` (the host re-creates its chr) |
| Fall recovery (6 s falling) | the lamp of this map | in-world |
| World lamp -> Dream, headstone -> world | never (another map) | load (alone: the traveller only) |
| Gather before a bell (guest's own world -> the host's last lamp) | never (it runs only when the maps differ) | load (own world) |
| Bell join (own world -> host world) | the world changes even with the same map id | load |
| Guest lone death respawn (SoloRespawn: dead follow, host's lamp) | yes | still a death load: a dead chr needs the revive (hostdeath agent: revive + `WarpToLampNoLoad`) |
| Host death respawn | yes | hostdeath agent (same mechanism; guests then follow in-world) |

`BB_PARTY_INWORLD=0` turns the in-world follow off (A/B tests).


### 3b. From hostdeath: remote chr render state, and re-creating a remote chr (2026-10-11)

* A remote player chr's draw factor is **PlayerIns +0x208** (float): it fades 0 -> 1 when the chr is
  built and 1 -> 0 in the death dissolve (live: `draw 0.25 / 0.75 / 1.00` after a summon, `0.88 /
  0.47 / 0.05 / 0.00` over 4 s after the owner died). The name plate and HP bar stay where the chr is
  even at draw 0: a dead copy left in a world is a "plate without a body". +0x20C/+0x210 (ChrFadeIn /
  ChrFadeOut alpha) stayed 1 / 0 throughout. A copy re-created by its task reads +0x1E1 `0001000098`
  (bit 0x1000 clear) where the first copy read `0001001098`; meaning unknown.
* hostdeath re-creates the copy of a player who respawned without a load the way the host does for
  a lone respawn: remove the dead copy (slot release `0x15BC080`, WorldChrMan remove `0x19186E0`) and
  rewind its task to STEP_Create (`member_revived` / `host_revived`, party_b2.cpp
  `MemberRevivedTick` / `HostRevivedTick`). The new copy is built at the owner's current position
  with draw 1 (live hd28: `0.5 m draw 1.00`). A shared "re-create the remote chr on all peers" path
  for split travel's arrival can reuse it.
* `remote chrs:` log lines (party_b2.cpp `RemoteChrRenderWatch`, any session) print every remote
  chr's position, distance, draw factor, alpha, flags and HP when they change.
* A local chr warp (vfunc +0x5F8) on a REMOTE copy does not move its name plate (tried: plate stayed at
  the summon spot); the ghost plate at the summon spot after a B2 arrival is not fixed that way.

## 4. Implementation (gpu/shim/party/party_split.cpp, party_b2.cpp, party_travel.cpp)

**The question.** A lamp or headstone warp `0x13CDF30(id)` the player starts, or the Hunter's Mark
item reaching the funnel (site `0x18FFA1D`), is consumed in a party (host with a member task, or a
guest that is a client) and held. The main thread then opens the game's own generic dialog, the
way the Lua `OnYesNoDialog 0x132D2C0` does: the struct at `*(0x5562878 +0xD08)` gets `[0]` the
message id, `[1]`/`[2]` = 1 / 2 (YES / NO buttons), `[4]` byte 1, `[6]` = -1 (the result),
`0x5562878 +0x254` = 0, then `0x16BEBF0(dialog, 7)` [C]. The message is the port's text id
(`BbGameMenu::Pages::SplitTravelText`, answered by the System menu's text lookup hook):
"Travel with the party? YES: everyone travels together. NO: you travel alone." The answer is
`[6]`: 1 = YES, 0 / 2 = NO / closed (the event `0x531CFD0`'s check reads it the same way) [C].

* YES: the old party move. Host: `RunLampWarpDecided(id, false)` (the interceptor stands aside,
  the funnel capture broadcasts TRAVEL, every guest follows with B2). Guest: `lamp_request` to the
  host (guest_services.md 3). A guest's Hunter's Mark "with the party" goes to the host's last lamp.
* NO: host: `HostTravelAloneNext()` + `RunLampWarpDecided(id, true)`: the captured TravelIntent
  carries `alone`; guests receiving it do not follow (`b2::OnHostTravel`: no load, the host's chr
  is removed on their side); the host's B2 driver does not re-send its snapshot after that load.
  Guest: `TravelAlone(LampIntent(id))`.
* `BB_PARTY_TRAVEL_CHOICE=party|alone` skips the question (always that answer); `ask` (default).

**Guest alone, into the host's area** (the Dream when the host is there, or the host's map): a
B2 re-join that the guest starts (`BeginRejoin(solo)`): stale snapshot packets are drained, flow
state 1, EVENT b2 `rejoin` -> the host re-sends its snapshot (the lone-respawn path, without the
dead chr), the guest loads ITS destination (the lamp warp itself, not the transform beside the
host; GSM +0x1521 cleared for the load so the summoned placement does not apply), arrives, both
sides rewind the member tasks. One load, for the traveller only.

**Guest alone, elsewhere**: `TravelAlone` writes flow +0x84 = 3 and runs the lamp warp with K7
armed (the funnel keeps the room), EVENT b2 `away` (the host removes the guest's chr). The load is
a host-world load (flags from the overlay); the game sets flow 4 at its end. Phase `AwayLoading`.

**Members apart** (`SplitTick`, both roles, every frame while no re-join runs): members are
placed by the party link's vitals (map + position, ~4 Hz). A remote chr whose owner (the member
reporting a position within 5 m of it) is in another area is removed (task kept). A member in our
area for 2 s without a chr here is built again (rewind; the host also re-registers its net sync,
`0x18F0DB0` / `0x18F0E80`, without snapshot packets) - a meet without a load. `MembersApart()`
feeds the host watchdog (present, never "stuck") and K5 (OnMatchingCheck stays quiet while
members are apart and 15 s after any split chr change).

**Tests.** Director steps `host_lamp=ID`, `guest_lamp=ID` (the player's warp, with the question),
`host_alone=ID` (decided alone); `BB_PARTY_SPLIT_TEST=no,yes,...` answers the questions in order
with the in-process pad (YES = Cross; NO = Right, then Cross: the cursor starts on YES).

## 5. Live results (2026-10-11, two loopback instances, base save savedata_20261010)

Director steps: `host_warp=2412951` (party to Central Yharnam first), then `guest_lamp=` /
`host_alone=` / `host_mark` with `BB_PARTY_SPLIT_TEST` answers.

| Run | What | Result |
|---|---|---|
| split2 | First dialog: kind 7, buttons 1/2 | A one-button "OK" box (screenshot); fixed: kind 8, buttons 3/4 = YES/NO, result [6] 1 YES / 2 NO |
| split3, split5, split7, split9 | Guest alone to the Dream (host in m24_01) | **Grey screen** (the user's report): the guest loaded no world and fell (`m24_9999` asked for, PlayerIns stage 0x18010000 at the Dream's coordinates). Neither the lamp warp, a forced transform, the 0x19 value (+0x1598) nor the world blobs fixed it |
| split10 | + the summoned placement GSM +0x14C4 / +0x14D0 / +0x14E0 = the destination | **Fixed**: `arrived alone in m21_00_00_00 ... stage 0x15000000 pos (-9.2, -6.8, -19.4)`, the Doll in view (screenshot); the host never loaded. The return then landed in the Dream again (+0x14C4 now the Dream's) |
| split13 | Guest: m24_01 -> Dream alone -> lamp 2412952 alone | Full cycle: alone load to the Dream (host stays, `split: 1 member(s) in another area`), back with one load for the guest only (`re-join with the host's snapshot`, arrived at the lamp's spot, host `slot 1 travels alone into this area` -> `a guest arrived; 1 member task(s) rewound`) |
| split4 / split6 | Host alone to the Dream | Guest stays, no load; guest sent home by OnLeave_Limit 5 s later -> fixed (dropped while the host is apart and connected) |
| split14 | Host: m24_01 -> Dream alone -> m24_01 alone | Guest never loads; the host's chr is removed on its side; send-home dropped; back in m24_01 both sides `split meet ... rewound (no load)`, the guest is a cooperator again (`(re)joined #3`) |
| inworld1 | Host Hunter's Mark (alone) and guest lamp 2412952 (alone) inside m24_01 | Both `in-world warp ... no load` (no loading screen) |
| inworld2 | Host Mark "with the party" + guest lamp "with the party" inside m24_01 | Host and guest move together in-world (`the host moved to lamp 2412951 inside its map (no load); following`), the guest's request becomes `HostPartyWarpNoLoad`; no loading screen on either |

Not done / open:

* A guest's death in a map the host is not in (TravelAlone refuses `Death` elsewhere; hostdeath
  owns the death rules, `RespawnAloneAtOwnLamp` covers the host's area).
* Flags a guest sets in a map the host is not in (bosses, levers) are not routed to the host (D3).
* Enemy state is per player while apart; a meet without a load does not reconcile it (each game
  keeps its own copies; the host re-registers its net sync).
* Three or more players apart: chrs are matched to members by the vitals' positions (works for
  any count in principle), not run live.
* A guest's own last-lamp record is not updated by an alone travel.

## 6. A lone guest's world actions in a map the host is not in (D3 interface, for guestprog)

guestprog's world actions (`party_worldact`, EVENT `world_action` {v, map, set, clr, kick}; not
merged when this was written) send a guest's local overlay changes to the host, which checks each
id with `WorldActionAllowed(id, clear, host_map)` - "the host's area" - and sets it with
SetEventFlag. A guest that travelled alone stands in another area, so every map flag it sets there
would be refused. The interface split travel provides and expects:

* `coop::b2::MemberApart(slot)` / `coop::b2::MemberMap(slot)` (party_b2.h): whether party slot
  `slot` is in another area than the host, and its map (the party link's vitals, ~4 Hz).
* Host side of world_action: `const u32 check_map = b2::MemberApart(slot) ? b2::MemberMap(slot) :
  host_map;` and `WorldActionAllowed(id, clear, check_map)`: a flag of the map the guest stands in
  is accepted while it is there alone; the host's own world gets it (doors, levers, shortcuts,
  elevators stay open for everyone; the mirror and C2 carry it back).
* Boss kills while alone (category `boss_defeated`, never synced by world_action today): allowed
  only from an apart guest, for the area it stands in, set-only. The host then holds the boss as
  defeated in its world (no rewards for the host; the guest got its own; D1's K10 / Insight are the
  guest's game's). Until that lands, a lone guest's boss kill lives in its own save only (C2) and
  the boss is alive again in the host's world.
* **2026-10-15 (pard4, D3): a lone guest cannot start a boss fight.** Live LB2 showed why the
  routing above never fires: a lone guest is still a client of the host's world, the host's game
  does not have that area loaded, and a boss's first-fight / start and defeat events are host-only
  copies, so the boss stayed idle and its HP 0 ran no defeat on the guest. Making the guest's game
  run those events as a host is not reliable (enemy ownership, chr-type and client checks across
  every boss event), so correct over seamless: while the host's area is another one the guest's
  fog keeps the vanilla phantom rule (`GuestFogSkipAllowed`: the host-entered / first-fight
  conditions are not skipped, no prompt), a notice at the fog says "Boss fight: the host is in
  another area. Travel together to fight this boss", and a guest that is in a boss room while the
  host is in another area (its prompt had opened while the host was there, or the host travelled
  alone during the fight) is put back outside the fog 2.5 s later (`FogPutBackTick`, its "guest in"
  flag cleared so another walk-in is seen). Live A2: the guest alone in Old Yharnam, the host in
  the Dream: `boss fog (guest-entry event 12304811) keeps the vanilla phantom rule`, two presses at
  the fog, no entry, the notice on screen. The boss stays for the party (travel together, or the
  host comes: the fog opens for the guest after the host's own entry, vanilla).
* Items: instanced per player (decision "World item pickups"); nothing to send.
* Enemies: section 5 / the `dead` op (union of kills at a meet without a load); ordinary enemies
  respawn on every load as in single-player.

## 7. Remote chrs: visible, at the right spot, on every peer (shared path; for hostdeath / balance / fourp)

User reports (split24, hd22, hd26, a guest death respawn, once in the Dream): on the host "the health
bar but no model" of the guest - the plate where the guest stands, no body. Findings (live,
render probe diffing a visible and an invisible remote chr, vis2-vis4):

* Not a position problem: the remote chr's physics position (+0x58 -> +8 -> +0x3B0 -> +0x68
  +0x1E0) was 0.85 m from the host, where the guest stood (`host watch`), on both screens. (chr
  +0xC0 is not a position: it reads 0 on remote chrs.)
* The difference is **chr +0x1E0 bit 20 (0x00100000)**: set on every drawn player chr (local and
  remote, both games: `0x00109800`), clear on the invisible re-created one (`0x00009800`,
  `0x400`). The chr data module's +0x1BC bit 1 (what STEP_Delete sets at a leave) is NOT it: the
  game sets it on visible remote chrs too.
* **Shared fix, automatic on every peer**: `RemoteVisibleTick` (party_b2, every 0.25 s from
  `b2::Tick`, host and guests): every remote player chr whose task is in its Update step and
  lacks the bit gets it back (`remote chr ... draw bit +0x1E0 0x400 -> 0x100400 (was not
  drawn)`). Live vis4: the guest invisible on the host after a meet, fixed within 0.25 s
  (screenshot: both hunters on both windows). It covers every path that re-creates a remote chr
  (B2 arrival, a meet, a respawn, hostdeath's respawns): nothing to call.
* Removing a remote chr (split travel, a dead member's respawn): `RemoveRemoteChr` plays the
  task's own STEP_Delete leave first (EnterleaveDirector `0x19704B0`, then slot release
  `0x15BC080` and WorldChrMan remove `0x19186E0` when the leave has played or after 2 s).
* A meet without a load: both sides re-broadcast their own player record (`0x1781AC0`, what
  InGameStart sends after a load; EVENT b2 `record` asks the other side) and re-create 1.5 s
  later; a remote chr that stays > 4 m from every member's reported position for 3 s is removed
  and built again (self-heal).
* In-world warps reset the camera (Lua CamReset `0x132F5A0(ctx, 10000, 1)`): before it the
  camera swung through the walls after a long in-world move (vis6 screenshot).

Start positions (the "2.2 m" report): measured 0.57 - 0.9 m apart on both screens after a bell
join, a B2 party travel and an in-world party move (logs: the guest's own position and the host's
copy of it agree, e.g. guest `(-192.9, -28.6, 68.0)`, host's copy `(-192.9, -28.6, 68.0)`, host
`(-193.5, -28.6, 68.6)`). It was case (c) of the coordinator's list in appearance only: the guest
was placed at 0.5 m and the host's copy was there too, but invisible, while the plate looked
offset by the camera angle. Fixed by the draw bit above.

Same-map warps, every entry point: party_travel's lamp warp hook (every `0x13CDF30(id)`, after the
question) calls `SameMapLampWarp`: a lamp of the loaded block is an in-world move (a host with
members: the party move unless it travels alone), and a lamp the player already stands at (within
2 m) is a no-op that only records it as the last lamp - no question either. Live vis6: test step
`host_lamp=2412951` at the lamp: `already standing at it (0.2 m): no warp, no load`; from 68 m
away: `in-world warp (the host's party move ...) no load`, the guest followed in-world. Not covered
yet: a Lua WarpNextStage (0x132E010) to a warp point of the loaded map (its entity position is
not in the spot table) and death respawns (the revive is hostdeath's).


## Floating phantom name plate: what agent/balance learned (2026-10-11)

User screenshots (split24 run; also a 3-player Dream run): on the host the guest's chr is not drawn
and the game's own blue "Co-operator / name" plate with its HP bar floats where nobody stands; the
guest sees both players fine.

* The plate is the game's `FrpgMenuDlgFloatingPCGauge` (string 0x4930683; built next to the enemy
  bars in 0x167c460), drawn over the host's own copy of the guest PlayerIns, at that copy's
  position. The party HUD showed the guest 0.9 m away at the same time: that distance comes from
  the guest's own VITALS because the host's copy was more than 8 m from the guest's report, i.e.
  the host's copy kept a stale position (no updates) and was not drawn: consistent with the host's
  chr remove / rebuild leaving a stale or hidden copy whose plate stays alive.
* A disabled chr (ChrIns_SetDisable 0x18c6390 sets [chr+0x18]+0x20 bit 0 and [chr+0x58]+0x114 bit
  0x40) is not drawn; if the copy is disabled while its plate is up, the plate is the leftover.
* The port's own HUD / tags no longer imply a member is there: a disabled chr is not "here", a chr
  more than 8 m from its member's own report is never matched to it (logged as `stale remote
  chr? ...`, once a minute), and a tag needs a chr read this tick, alive, on our map, from a fresh
  board (`party::hud::TagVisible`; `Party HUD: tag X hidden: why`). `BB_PARTY_HUD_POSDIAG=1` (with
  `BB_PARTY_HUD_LOG=1`) dumps position-like values in each remote PlayerIns for the anchor hunt.

Note (splittravel): in the vis2-vis4 runs the host's copy did get position updates (0.85 m from the host, matching the guest's own); the difference to a drawn chr was chr +0x1E0 bit 20 (section 7).

## 8. 0.5.3 round (2026-10-12)

* **Draw bit, root cause.** `CSMultiPlayerInsTask::STEP_Create` (0x1E50090) creates a remote
  phantom hidden: at 0x1E502C3 it clears chr +0x1E2 bit 4 (= +0x1E0 bit 20) for chr types 1 / 2 /
  12 (`0x1006 >> type & 1`). The game shows it either through the summon presentation or at the end
  of a load: InGameStart 0x193A3E0 sets the bit (0x193A93C) and calls chr vfunc +0x610(1) for every
  player chr whose vfunc +0x1B8 is 0. A chr re-created by our rewind in a running world got
  neither. Now every task `RewindRemoteTasks` rewinds is remembered and its new chr gets
  InGameStart's two steps (`remote chr ... shown as InGameStart does (re-created by a rewind)`,
  live split31-split36 on host and guest after B2 arrivals; host screenshot: the guest drawn
  beside it). The old repair is a safety net only (a remote chr without the bit for 4 s).
* **Grey screen regression (split31).** A plain B2 party travel landed the guest in the Dream's
  stage at Central Yharnam's coordinates: the summoned placement (GSM +0x14C4 stage) had been made
  conditional on GSM +0x1521 upstream; a host-world load takes its stage from +0x14C4 regardless.
  The write is unconditional again (`WriteSummonedPlacement`). Live split32-split36: no grey screen.
* **Grey-screen detector**: `tools/mp/shot.py --grey` grabs every window each 2 s; a 3D view (the
  centre, without the HUD corners) with a luminance std-dev under 4 for over 3 s while the log says
  the world is up is logged `GREY SCREEN instN` (stdout, `<root>/grey.log`, a PNG). Measured: grey
  screens 0.1 / 1.1, normal views 13-20.
* Lone guest in a map the host is not in: alone travel to Central Yharnam with the host in the
  Dream works (screenshot), an enemy killed there is recorded (`enemy 2410740 killed while the party
  is apart`); the meet that applies it and the world action of a lever / flag there were not reached
  in the time the test runs had (the central reaper ends a run after 10 s without new party log
  lines). `BB_PARTY_SPLIT_ENEMIES` stays off.
* Not done: a Lua stage warp (0x132E010) inside the loaded map: its warp point ids (EMEVD 2003[14]
  args, e.g. 2100980) are not entities of any MSB (ids_tool: "warp points not in an MSB"), so the
  destination spot is not known without the game's own resolution.

## 9. 0.5.6 round (2026-10-12)

* **Alone load without a forced transform.** A guest's alone travel into another area is now the
  plain lamp warp (host-world load with the summoned placement GSM +0x14C4 / +0x14D0 / +0x14E0 for
  the stage) and a chr warp onto the lamp's spot after the arrival (hostdeath's B2 arrival approach,
  b2_seamless.md §6.13): the forced placement (GSM +0x14F0 / +0x1500 / +0x1510, what the own save
  records as its spawn) keeps the own save spot. `BB_PARTY_SPLIT_ALONE_FORCED=1`: the old forced
  transform. Live c7a: `alone arrival 0.0 m from the lamp's spot; forced placement (own save spot)
  map 0x15000000 (-9.2, -6.8, -19.4)`, screenshot in Central Yharnam at the clinic lamp (host in
  the Dream, no load for it).
* **The own save spot of a title-joined guest whose save is in the host's map.** The title join read
  the own spot from the forced placement only when its map differed from the host's, else from the
  spawn record (+0x15B0 / +0x15E0), which in a Dream save held m24_01 at (0, 0, 0): that went into the
  forced placement, and the guest's next solo Continue fell through Central Yharnam (c4: y -390,
  -895 ...), after the fix of the record alone it stood at the Dream's origin (c6). Now a record at
  (0, 0, 0) is no spot, a forced placement in the host's map is read as the own spot when it holds
  one, and with no known own spot the forced placement becomes the own last lamp
  (`BB_PARTY_TITLE_OWN_LAMP_SPOT`, on). Live c7: party run with the guest's alone travel to Central
  Yharnam, then the guest's save Continued (as instance 0): `map - -> m21_00_00_00`, (-9.2, -6.8,
  -19.4), screenshot at its own Dream lamp - not at the alone-travel lamp.
* **Scripted stage warps inside the loaded map** (`BB_PARTY_STAGE_INWORLD`, default off). The warp
  point of a stage warp (GSM +0x10, written by Lua WarpNextStage 0x132E010 and EMEVD 2003[14]) is the
  entity id of an MSB region (POINT_PARAM_ST entry: +0x48 entity data offset -> +0 the id; +0x14
  position, +0x20 rotation): the earlier "not in any MSB" was the AAB-range scan. `ids_tool.py
  gen-inc` writes `kPartyWarpSpotIds` / `kPartyWarpSpots` (12 of the 27 EMEVD warp points: Dream
  2102808 / 2102809, m24_00 2402200, Nightmare / Hamlet / Research Hall points; the rest are Chalice
  or not in a block MSB) and `ids::WarpSpot`. The funnel hook (party_travel `SetSameMapStage`) hands
  every ScriptedWarp / LuaStageWarp site the map (+0x0C) and point; `SameMapStageWarp` makes it a
  chr warp + camera reset when the point is in the loaded block (1: alone; 2: also a host with
  members, who then moves alone). Live st1 (mode 2, test step `host_stage=21:0:0:0:2102808`):
  `in-world warp (stage warp, point 2102808): (-10.6, -7.0, -21.2) -> (28.4, -9.2, 5.6) in
  m21_00_00_00, no load`; screenshots: host in the Dream's flower field, the guest still at the
  lamp (host's tag at 48 m). Off by default: no real scripted warp of the loaded map was exercised,
  and the party semantics (should guests follow?) are open.
  0.5.9 (inworld): closed, stays off. An EMEVD scan of every warp instruction: the 12 points with a
  spot are all cutscene warps (2002[4] / [6]: 12101802 / 12101852 Gehrman ending, 12400750 Cathedral
  Ward day to evening, boss intros 13300200, 13401851, 13404825, 13501105, 13501801), whose move does
  not go through the travel funnel (no 2002 site among its 15 callers, travel.md 1.6), so
  `SameMapStageWarp` never sees them; the only 2003[14] warps into their own block are the Chalice
  layers (m29_02 event 12901710, points 2900980-89: no MSB spot, and a new layer layout needs the
  load). Lua WarpNextStage points are not in the table either. No real warp reaches the switch, so
  turning it on would change nothing in play; it stays as a test aid (`host_stage`).
* Test aid note: `BB_PARTY_SPLIT_TEST=no` missed its Right press once (cont1: the dialog took YES and
  the host travelled); `BB_PARTY_TRAVEL_CHOICE=alone` on the guest is the deterministic choice.

## 2026-10-15 (parab2): title-joined guests stay; enemy reconciliation stays off

* **Host alone, title-joined guest (b5e, live)**: 6 s after the host's alone lamp warp the guest's
  game ran OnLeave_Limit (`0x138A51C`). The split veto drops that for a summoned guest, but the
  title-join check ran first (a title-joined guest has no own-world snapshot, so its send-home is
  replaced by a leave + lamp warp home): the guest loaded its own map and joined the host again
  later with two more loads. `SendHomeVeto` now checks the split case first (not for the player's
  own Leave, which `party_travel` marks while the veto runs). b5e2 / b5e3: the guest stayed in the
  host's world for 190 s / 45 s with no load (`a title-joined guest` in the drop line).
* **`BB_PARTY_SPLIT_ENEMIES=1` (b5e3, live)**: the guest killed three enemies of the Cathedral Ward
  while the host was in the Dream (census `1 died` x3); none was recorded as an apart kill and the
  meet sent 0. Without the reconciliation the host's world wins at the meet: the guest's census
  logs `RESPAWN without a load: 3 enemies alive again` right after the meet, and both players see
  the same living enemies. The switch stays off (default decided with this run).
