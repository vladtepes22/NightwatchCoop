# Guest actions in the host's world: levers, doors, NPC talk, pickups (D3, D4, D8, G2, H5)

Bloodborne 1.09, our port; addresses are **our offsets** (raw ELF VA of `eboot.elf`). Code:
`gpu/shim/party/party_worldact.{h,cpp}` (world actions, ObjAct kick log, live test fixture),
`party_progress` (the guest's local changes, instanced items), `party_guestsvc` (NPC talk gate),
`party_phantom` (encounter Insight), `party_story` (time of day catch-up). Tests:
`tests/test_party_worldact.cpp` (`party-worldact-test`), `tests/party_fuzz.cpp` (`lamp` target:
`worldact::RequestFromJson`), `tests/test_party_phantom.cpp`.

User decisions (ERSC_PARITY.md "Design decisions", binding): world item pickups are **instanced**
per player; NPC quest choices are **shared** (the first choice anyone makes applies to the host's
world and every player's save); a guest behind the host **catches up fully** (bosses, Blood Moon,
lamps, shortcuts in its own save).

**0.5.2 defaults.** On: world actions (levers, doors, shortcuts, NPC talk and quest flags), NPC
talk for guests, shared NPC quests, ObjAct kick log / replay, guest-triggered cutscenes for the
party (D10), encounter Insight for guests. **On since 2026-10-12**: full per-player
enemy drops (`BB_PARTY_GUEST_DROPS=0` turns it off). **Off (opt-in)**: instanced treasure
(`BB_PARTY_GUEST_PICKUP=1`), the area-gate
catch-up (`BB_PARTY_GATE_CATCHUP=1`), the time-of-day catch-up (`BB_PARTY_TOD_CATCHUP=1`; its first
version queued a story item on a guest still at home, which held the guest's bell: it never
joined - fixed to "only once in the host's world", not re-run).

## 1. The model: a guest's game writes the host's world copy

A summoned guest (session role 6) runs its own EMEVD events, talk scripts and ObjActs on its copy
of the host's world: the host-world flag overlay (load mode 1, `event_flags.md` 3), which the
host-world mirror keeps equal to the host's world (`event_flags.md` 12). Whatever the guest does
there - pull a lever, open a door, kick down a ladder, ride an elevator, talk to an NPC - is a
change of that overlay only. The mirror applies, live, only the bits **the host** changed since its
last apply, so the guest's own changes stay in its overlay; `GuestWorldMirror::LocalChanges`
lists them: overlay bits (mirror mask) that differ from what the host's image last put there,
skipping groups with a host change not yet applied.

Before this work only lamp lighting went back to the host (EVENT `lamp_lit`). Now every such
change of an allowed category does.

## 2. EVENT `world_action` (guest -> host)

Guest (`worldact::GuestTick`, main thread, role 6, load mode 1, world up, not loading):

* a **baseline** 3 s after the world became steady (each load, each map change, each overlay
  rebuild): the local differences the guest's own game made while loading the map are held back.
  Client branches of events end early (`1003[6]` "end if client"), and an ended event writes its
  completion flag (0x12ED100) - those must never reach the host. Live: 93-102 such differences
  per load (m21_00, m24_01).
* **own map block only**: a map flag (`TAABZnnn`) is sent only when its area and block are the
  guest's current map's. A streamed-in neighbour block runs its constructors on the guest (live
  run 9: m24_00's NPC init flags 12400501.. appeared while the guest's map was m24_01 and were
  sent - wrong); now they stay local (run 10: m27's 12700701.. "kept local"), and once the guest's
  map is that block the new baseline holds them back.
* every 0.25 s: new local differences. Allowed ones (`WorldActionAllowed`, below) go to the host
  as `{"v":1,"map":<guest map>,"set":[ids],"clr":[ids]}` (at most 32 / 16 ids); the set ones are
  also written into the guest's **own pool** (its own save keeps the progress; a save request
  follows). The rest is logged as "kept local".

Host (`worldact::OnHostEvent` on the link thread -> queue -> `HostTick` on the main thread in its
own world, steady): `RequestFromJson` validates shape and ids, then each id again against the
host's **current map** (`WorldActionAllowed(id, clear, host_map)`), a per-slot rate limit (96
flags per 10 s) and applies with SetEventFlag. The world mirror brings the result back to every
guest; C2 puts it into every guest's save.

`WorldActionAllowed(id, clear, host_map)`:

| Category | Set | Clear | Notes |
|---|---|---|---|
| shortcut_door | yes | yes | levers, gates, one-way doors, ladders; **not** two-state lift flags (C2's state ids, e.g. 12410350/351): the lift's own network-synced events and the replicated kick move it on the host, a guest's late value would fight the host's ride |
| world_state | yes | yes | world objects' states |
| npc_quest | yes | yes | shared NPC quests; a quest step clears the old state flag and sets the new one (Iosefka live: 1141 off, 1143 on) |
| key_event | yes | no | world progression |
| cutscene_seen | yes | no | D10: a scene a guest's event played counts as seen in the host's world |
| covenant_outcome | no | no | 0.7.3: Annalise / Alfred / Valtr's quest state (event_flags.md 7): per player while the host's rule *Covenants per player* is on; kept in the guest's overlay ("kept local"), not in its own save. Off: as its base category |
| anything else | no | no | boss_defeated, boss_area, lamp_unlocked (EVENT lamp_lit), cutscene_seen, item_lot_picked (instanced, 4), session_runtime, time_of_day, never_sync |

Also required: a mirror group and mirror-mask bit (`WorldMirrorKey`, `WorldMirrorMask`: never
2000-2099, 9020-9026, 6000-8999, chalice areas, area 99), not `NeverWritable`; a map flag
(`TAABZnnn`, AA > 0) must name the **host's area**; area-0 ids only for npc_quest / key_event.
`party_fuzz` (target `lamp`) checks every accepted request against these rules and the round trip.

Switch: `BB_PARTY_WORLD_ACTIONS=0` turns it off (both sides). Diagnostics:
`BB_PARTY_WORLD_WATCH=id,...` logs visible / own-save / host-image values of those flags on each
change (both roles).

## 3. ObjActs (levers, doors) on a guest

No phantom gate exists in the ObjAct code (`phantom_limits.md` 2.4) and the live run confirms it:
the guest's lever prompt (text 10010501) shows and the lever works on the guest.

The ObjAct manager (`0x13B2790`, update; per-block managers 0xD8 bytes at `+0x28`, count `+0x20`;
entries 0x140 bytes at `+0x90`, count `+0x98`: `+0x8` list index, `+0xC` kick event id, `+0x58`
object handle, `+0x5C` trigger state):

* `0x13AE370(blockman, idx, chr)` - the **local** player's kick (from the action-button update
  `0x13ADCC0`; entry `+0x78..0x7A` = kick, change, send-net);
* `0x13AF830(blockman, idx, chr, state)` - a kick **another player** made, from the manager's
  network queue (`0x13B2F90`); `0x13AF540` re-applies a state;
* packets: 0x2E (state, all peers), 0x2F (host -> client state); `0x13AE600` runs the kick.

`party_worldact` logs both entries ("ObjAct kick (local player|another player)").

**Live result (run 3): vanilla replicates a client's kick.** The guest's Cross on lever 2411310 ran
`0x13AE370` on the guest (`kick event 12410200` = the lever event's id, object handle 0x20500045,
entry 7) and 0.07 s later `0x13AF830` on the **host** for the same entry: the host's own event
12410200 saw "ObjAct executed", played the gate animation in the host's world and set its flag
itself (the guest's `world_action` 3.5 s later found `12410200=1(was)`). Host screenshots: the
gate closed at +0.5 s, open at +9 s. So the host sees the object animate natively; the
`world_action` route adds the guest's own save and covers what the kick does not (NPC flags,
objects whose host-side event is not loaded, a host far away). The guest also sends the kick
event id (`kick:[12410200]`); the host replays it on its own entry through `0x13AF830(blockman,
idx, -1, current state)` only when its game did not get the kick in the last 15 s (run 3:
"already reached the host's game (vanilla)").

## 4. Items: instanced world pickups (D4)

Vanilla: MSB treasure is never created on a client (`0x17DC300`: role 6 / session slot gates) and a
client's pickup skips the pickup flag and the Lua event (`0x17D6780`). With
`BB_PARTY_GUEST_PICKUP` (default on, party mode; `=0` off) byte patches (verified):

| Site | Original | What |
|---|---|---|
| 0x17DC3D6 | `0f 84 a5 0c 00 00` (je, role 6) | treasure spawned on a client |
| 0x17DC3E3 | `0f 85 98 0c 00 00` (jne, session slot) | treasure spawned on a client |
| 0x17D6838 | `0f 84 2b 02 00 00` (je, not own world) | the pickup sets its flag and runs its Lua event on a client |
| 0x17D6AF6 | `0f 84 5c 05 00 00` (je, not own world) | the pickup sets the lot's pickup flag (record +0x28) on a client |
| 0x17D52D9 / 0x17D52E6 | `74 26` / `75 1c` (role 6 / slot) | a client's treasure record gets its own handle (all were 0x3: the pickup's lookup 0x17D81A0 found another record) |
| 0x17D34A0 / 0x17D40AB | `e8 ..` call 0x17D5B40 | the host's item sync (net handler 0x17D2F30, "clear kind 3") no longer drops the guest's treasure records |

Live 2026-10-12 (agent treasure, pristine fresh saves, clinic): before the fixes the guest spawned 53
treasure records and the host's join sync cleared them all 4 s later. With them: the guest picked up
treasure 52410140 through 0x17D8570 (item-get "Madman's Knowledge" on its screen, own-save flag set,
host image 0); the host then took its own copy and C3 did not forward it (`InstancedTreasureFlag`:
a map treasure flag 5AABZnnn with no award capture is per player).

Instancing (`progress::SetInstancedItems`): the world mirror leaves the item_lot_picked bits alone
(`WorldItemMask`); after every mirror apply the overlay's item bits become the guest's **own pool**
bits (full copy after a rebuild, set-only live so a fresh pickup is not undone) - the guest sees the
treasure it has not taken itself, whatever the host took. A guest's pickup sets the bit in its
overlay; `GuestLocalWorldChanges` reports overlay item bits the pool lacks and the guest writes
them into its own pool (never to the host). Only map treasure flags (`5AABZnnn`, area > 0) and
only within 15 s of a real pickup (`0x17D8570` entry logged): the guest game's own global item
bookkeeping (9042-9075, set at its load) is not a pickup (run 10 recorded those - fixed). Pickups
are never part of the baseline (a pickup during a map change still counts). The host's world keeps its item. C3 still hands the
host's pickups to the guests (their pool flag then hides the treasure for them too); boss rewards
and key items stay C3's (everyone present).

### 4.1 Enemy drops: per player (user decision, 2026-10-11)

Every player's game runs an enemy's death and rolls its drop itself (`0x17DD0B0`, from the chr
death `0x18C4F90`); a client kept only items with `isGuestDrop` (Blood Vials, Quicksilver Bullets)
- the filter at `0x17DD75D` is skipped for a non-client by `0x17DD764 je`. `BB_PARTY_GUEST_DROPS`
(default on) turns that `je` into a `jmp` (`0f 84 3a 02 00 00` -> `e9 3b 02 00 00 90`): a guest
keeps its whole own roll (materials, gems, rare drops) - each player has its own drop, into its own
save (a unique drop's flag is an item_lot_picked bit: own pool, instanced like treasure). The drop
roll and every map item pickup are logged ("enemy drop roll on this game", "map item pickup").

## 5. NPC talk with a guest present (D8)

Every world NPC talk script asks `IsClientPlayer` (talk test 19; scan of the talkesdbnd files:
all of t22xxxx-t36xxxx with NPCs do, 2-7 times each) and parks a client in its "client wait"
state. The guest-services filter (`party_guestsvc` `IsClientFilter`, `0x203BA78`) now answers 0
for a client when `WorldTalkAllowed(talk)`: the world maps' scripts (map 220-369), not the Dream's
(guest services keep their own list; Gehrman's ending is never a guest's), not the world lamps.
`BB_PARTY_GUEST_TALK=0` turns it off. What the talk sets (quest flags) is then a world action
(npc_quest), so the host's world and the guest's own save both advance; the host's npc_quest
flags reach every guest's save through C2 (`BB_PARTY_PROGRESS_NPC`, now **on** by default: the
user's decision). Gifts given in dialogue go to the player who talked (its own inventory). The host gets the same gift too (D5, `party_items` EVENT `gift_up`: the NPC moved on in the host's
world, so the gift would otherwise be lost for everyone else); C3 then hands it to every other guest.

## 5.1 Cutscenes for the whole party (D10)

Host-triggered scenes already play for every guest (C4 live mirror, cutscenes_endings.md). New:
a scene a **guest's** own event plays in the host's world. The guest's bank-2002 handler hook
(`party_story` `RemoHandlerEntry`, which only logged on a guest before) turns a plain play
(2002[1/3/7], a known remo, no warp) into EVENT `story_up` to the host and remembers it; the host
(`HostStoryFromGuest`, sanitized like any peer story) plays it itself (mode 0) once it is in its own
world and no remo plays, then broadcasts it as a story of its own (`PushHost`): every other guest
watches it through the live mirror; the guest that triggered it drops the copy (it watched it,
180 s window). The scene's flags travel as world actions (cutscene_seen, set only). Warp variants
(2002[2/4/6]) stay with the triggering player. While a remo plays on a game, its party HUD panel
and name tags are hidden (`party::hud::SetCutscene`, from the story tick; live 2026-10-12: both
instances show the bare scene, the panel and the partner's tag return after it). Test: `BB_PARTY_STORY_TEST=up:<remo>` on a guest
plays the remo there as its own event would and sends it up.

## 6. Insight and the bath (G2)

* Boss Insight: native +1 for a white phantom plus the director's N-1 (`phantom_limits.md` 5).
* **New**: Insight the host gets from a 9350 start that is not a boss kill (a boss's first
  encounter, story moments such as entering a clinic) goes to every guest present too (EVENT
  `phantom {insight, n, boss:false}`; the guest applies all N, no native +1). Older hosts omit
  `boss` (= a boss kill).
* Guest cap (0.6.1): `InsightBudget`, a rolling 60 s budget of 20 Insight, 10 per event (was one
  event per 30 s, which dropped a boss kill's Insight right after its first-encounter +1). Live check:
  `BB_PARTY_PHANTOM_PROBE=host9350` on the host feeds its observer a story 9350(1) 6 s after a
  cooperator is in, then a boss kill 9350(3) 3 s later (guest log: `host encounter / story Insight`,
  `host boss kill`, `Party: Insight 6 -> 7 -> 8 -> 9`).
* Insight items: white-enabled (`phantom_limits.md` 1.1); they act on the guest's own record.
* Bath Messengers (t210691/694/695/699 shops, t210698 Insight shop, t210697 dress-up): open to
  guests (guest_services.md 2.2), purchases on the guest's own echoes / Insight and own stock flags
  (6000-8999 stay in the own pool; the one-time stock flags 70009xxx / 72101xxx are overlay groups:
  the guest's shop shows its own pool's stock and its purchases go there, event_flags.md 3).

## 7. Catch-up of a guest behind the host

| Progress | Into the guest's own save | How |
|---|---|---|
| Bosses (boss_defeated) | yes | C2 set-only (+ the boss-in-this-map hold, event_flags.md 11) |
| Lamps (lamp_unlocked) | yes | C2 |
| Shortcuts / doors (shortcut_door, world_state, key_event) | yes | C2 |
| NPC quests | yes (now default) | C2 with BB_PARTY_PROGRESS_NPC=1 (default) |
| Blood Moon / time of day (9800-9802) | yes (**new**) | C4: the host now re-sends its time of day to each member that joins (`StoryMemberJoined`); the guest's planner never turns its clock back |
| Items the host picked up | yes | C3 full list on join |
| Area gate flags 2AAx / 3AAx (boss cleared per area) | yes (**new**) | still never_sync in C2 (2410-style flags block summons in the host's world); the guest derives them in its OWN save from its own boss flags: `worldact::GateRules()` (from every EMEVD boss-defeat event: its flag and the 2000-3999 flags it sets; 2410 needs 12411800 and 12411700), applied at each steady world (`GateCatchup`, log "guest catch-up: area gate flags") |

## 8. Character freedom audit (H5)

Talk scripts of the Dream (`IsClientPlayer` counts from the talkesdbnd scan): the Doll, storage,
workbench, Memory Altar (t210699, chr 2100203), mirror, every Bath Messenger (shops, Insight shop, dress-up) are on the guest list;
t210690 (no client test, no character in the Dream layout) needs none. Live h5 (2026-10-13): the Memory Altar and the mirror (BB_DREAM_MIRROR layout) open for a guest in the host's Dream. Chalice altars (t210050/100/110) stay host-only here (chalice
rows, G1). Equipment, outfits (equip menu), gestures (menu), runes already held, items in the
storage box: local record, no gate. Blood Vials / Quicksilver Bullets: bought in the Messengers'
shops (open), refilled by the phantom refill.

## 9. Live evidence

Two loopback instances, save `savedata_20261010` in each (level 37), host lamp-warps to Central
Yharnam (2412951), guest follows (B2). Host test save: `BB_PARTY_PROGRESS_TEST_CLEAR` closes the
m24_01 gates 12410200/202/203/205 in the host's world. Guest fixture
`BB_PARTY_WORLDACT_TEST=obj=...;chr=...` stands at each object / NPC, waits for the action
prompt, presses Cross (in-process pad).

Run 2 (2026-10-11 11:4x):

| Step | Guest | Host |
|---|---|---|
| lever 2411310 (gate 2411300) | prompt 10010501 at the first spot, Cross; +5.9 s `world action sent ... set 12410200(shortcut_door); 1 written to the own save` | +0.06 s `slot 1 changed the host's world ... 12410200=1[shortcut_door]`; mirror back: guest `host image 1` |
| lever 2411312 (gate 2411302) | same, `set 12410202` | `12410202=1` |
| Gilbert (chr 2410782, talk at his window) | prompt 10010200 (Talk), dialogue paged with Cross | - |
| Iosefka (chr 2410702, clinic door) | prompt 10010400, talk; `set 62411300(world_state)`, `set 12410739(npc_quest)`, then `set 1143 72410337 (npc_quest)` | all applied |

Run 3 (12:07, host standing 7 m from gate 2411300): guest Cross 106.622 -> guest `ObjAct kick
(local player) ... kick event 12410200` 106.645 -> host `ObjAct kick (another player) ... kick
event 12410200` 106.690 -> host's own event sets 12410200 (guest's world_action at 110.354 finds
it set). Host screenshots `lever_0.5s_inst0.png` (gate closed) / `lever_9s_inst0.png` (open).

Run 4 (Iosefka again, the quest already advanced in the host's save): guest `set
12410739(npc_quest)` -> host `12410739=1(was)`.

Runs 6-9 (D4): the guest **sees world treasure** in the host's world (`pickup_0.5s_inst1.png`: an
item glow at the guest's feet; vanilla never spawns treasure on a client; 264 MSB treasure spawns
logged on the guest), and **takes it**: run 9 `map item pickup on this game (role 6)` after the
"Search Body" prompt (10010101), item-get dialog "Madman's Knowledge" on the guest
(`drop_0.5s_inst1.png`). That pickup happened while the guest's map changed (m24_01 -> m24_00),
so the 3 s baseline swallowed its flag (not written to the own save) - fixed (pickups are never
baselined). Run 9 also: the guest's game rolls enemy drops itself (`enemy drop roll on this game
(role 6)`); a kill by the fixture's local ForceDead did not reach the host's game (no host roll),
so "a guest kills an enemy" with real damage is still to be run.

Run 12 (host kill fixture `kill=2` on the host, guest `pickup=2`): the host's kill of an enemy
rolled its drop on **both** games for the same enemy: host `enemy drop roll on this game (role
3): chr handle 0x10014001` at 161.366, guest `... (role 6): chr handle 0x10014001` at 162.315 -
each player has its own roll; with `0x17DD764` patched the guest keeps all of it. The m24_00
neighbour block's NPC init flags (12400501..) now stay local ("kept local", own-block rule). The
guest's two treasure tries found no "Search Body" spot (no pickup).

Run 10 (Cathedral Ward, host lamp-warps to 2402950, `map=m24_00` fixture): Oedon Chapel Dweller
(chr 2400700) talk prompt 10010200 -> guest `set 72400310(npc_quest)` -> host `72400310=1`;
Eileen (chr 2400900) talk -> guest `set 72400520(npc_quest)` -> host `72400520=1`
(`cw_npc_*`, `eileen_*` screenshots). Alfred (2400760/762/763) had no talk prompt in this save
state (not placed / not talkable yet).

Runs 20-25 (2026-10-12, opt-ins on, `pickup=1;kill=1;attack`): not verified, the switches stay off.
The test tool is what failed, not the features: around the Central Yharnam lamp every treasure
within 30 m is taken in the test save (C3 re-gives the host's pickups on join, so clearing a pickup
flag on one side does not stick; the guest's own-world `TEST_CLEAR` never runs on a fast-joining
guest), the nearest untaken one (53 m, (-208.5, -47.5, 115.7)) showed no pickup at 17 spots
(screenshot: bare cobblestones, 8 fps). Enemy HP is read from the ChrIns data module `+0xF8` (max
`+0xFC`); the nearest live non-player chr near the lamp (HP 194, chr type field 4) took no damage
from 60 R1 taps: not a hostile enemy (screenshot: the guest swinging at empty air by the lamp).
A decisive test needs a known hostile enemy's entity and a treasure flag the host never took
(placed by hand), or a manual play test. Fixture lines now carry `step-xx` tags (no digits) so the
harness reaper sees activity.

Runs 32-34 (2026-10-12, FRESH characters: BB_TEST_NEWGAME, both in Iosefka's Clinic, opt-ins on):
the host's debug kill of the clinic beast (entity 2410019) rolled its drop on **both** games for
the same enemy (host `role 3`, guest `role 6`, chr handle 0x10014009); the guest stood on the
corpse and picked up **its own** drop: `map item pickup on this game (role 6)`, item-get dialog
"Blood Vial x3" on the guest's screen (`pickhook_2s_inst1.png`); both players got the kill's 535
echoes. Blood Vials are guest-allowed in vanilla too, so the full-drop patch itself (materials,
gems) is still unproven: `BB_PARTY_GUEST_DROPS` stays opt-in. The clinic floor treasure (12.9 m)
gave no pickup at 17 spots (the guest saw the item's glow beside it): `BB_PARTY_GUEST_PICKUP`
stays opt-in. Test-driver fix found on the way: the ImeDialog now opens the overlay's text box,
so the scripted new game (BB_TEST_NEWGAME) stood at "Please Enter Name" - it now gets "Hunter"
without the box (src/runtime_services.c; players type as before).

Runs 40-43 (2026-10-12, fresh characters; run 43 on pristine saves restored before the run, no
player moves): the host killed two clinic enemies (ForceDead on the host, no walk); each death rolled
a drop on **both** games for the same enemy (host `role 3`, guest `role 6`, handles 0x10014053,
0x10014058); the guest took its own drop through the game's pickup function: run 40 "Pebble",
run 43 **"Iosefka's Blood Vial"** (item-get dialogs on the guest's screen, `gloot_*_inst1.png`) -
items a guest's game dropped before the patch (not isGuestDrop). **`BB_PARTY_GUEST_DROPS` is on by
default now.** Treasure (`BB_PARTY_GUEST_PICKUP`): the guest's MapItemMan held no treasure record at
all (kind-3 list +0x28 empty, run 43), though the treasure spawn hook ran 53 times on the guest -
the records are not created on a client by the three patches: stays opt-in (open).
Run 45 (default build, no D4 switch set, pristine saves): the same result - the guest kept "Iosefka's
Blood Vial" from its own roll; the join banner "Entered the world of Hunter0" was still on screen over
the item-get box (cosmetic). Test hygiene since: saves restored from a pristine copy before every
run, no player moves in the D4 runs, wait lines with fixed text and hard timeouts.

## 10. Open items

* Elevators: see 10.1 (lifts071, live).
* Instanced pickup into the own save: the fixed path (no baseline) not yet re-run live; enemy
  drops: a guest's real kill (damage, not a local ForceDead) and both players' rolls not yet seen.
* Memory Altar / covenants / chalice ritual by a guest not run live.

## 10.1 Lifts (lifts071, 2026-10-21, live)

How a lift moves on every game (live, EMEVD trace of the lift's events + a per-frame log of its flags
and of the car's animation state on each game, `log_flag=` / `log_objanim=`):

* Every game runs its own copy of a lift's events (Central Yharnam 12410321-12410325: init, up, down,
  "host state"; the older lifts only up / down). A player stepping into the car or pulling a call
  lever starts its own copy, and the game's own network sync starts the copy of every other game
  within ~0.05 s (l5: a guest's ride, the host's ride, the host's lever, each on all three games).
  The world mirror then writes the host's values into the guests (equal: nothing changes).
* The car only animates where the object is active (the player near it, `ObjInsUpdate` flag 0x40 at
  entry +0x1FC); a far game's copy finds its "wait for the animation's end" (2003[18], waiter vtable
  0x532C3F0, check 0x13CC180: entry +0x208 = the anim id, +0x1FE bit 3 playing) true at once and ends
  its ride in 0.1 s, with the same flags. The car takes the last requested animation when it becomes
  active (l5: the host came near and its car showed the guest's end).
* A guest's step into the car does NOT drop it: the guest falls down the shaft only in a test that
  warps the guest straight into the car from far away (l1-l4: the car's object was not active yet,
  so its own copy ended at once while the guest stood on nothing). Walking up (or standing next to it
  for a few seconds) and then stepping in rides normally (l5: -44.3 -> -58.1 in 5 s on the guest,
  the host's copy finished at once 131 m away, both games' flags equal at the end).
* A guest calls the lift while the host stands in it after a ride: "Device is not currently
  operable." on the guest, as in single player (the down event waits until everybody left the car);
  the lift stays the same on both games (l5).
* **The desync (fixed):** a game with the lift's map block streamed out (Cathedral Ward's
  12400824 / 12400825 lift while the host is 130 m away in Central Yharnam) never runs its copy, and
  lift flags were never world actions ("their own sync"). The guest's ride (l7: 12400827 0 -> 1 on
  the guest) stayed out of the host's world; when the host walked back, the block's init event
  12400823 put the car at the old end from the host's flags (l8: anim 30, the bottom, while the
  guest's car was at the top). Each game then showed the car where the other had an empty shaft,
  and a lever or the car region on one game did nothing on the other. Now a guest's finished ride
  (its busy flag off, `GuestLiftRideDone`) goes up as `lift_set` / `lift_clr` (the lift's position
  flag); the host writes it (and the host-state flag of the newer lifts) only while the lift's block
  is not loaded in its game (`HostLiftSync`: `same` when its own copy ran, `busy` while a ride runs
  there, `loaded` when the car is loaded but its copy did not follow: left alone, a flag alone would
  not move the car). l9: `lift 2401102 12400827=1(applied: the lift's block is not loaded here)`,
  then the host's init at the stream-in took anim 0 (the top), where the guest's car was. Lift table:
  `worldact::Lifts()` (21 lifts, from the EMEVD of every up / down event; liftfall added Cainhurst 12500074, the six Nightmare of Mensis lifts of its common up / down events 12601320 / 12601330, its library lift 12601253 and the Nightmare Frontier's 13300115).
* Long lifts (liftfall): the report's 60 m ride (y 1003 -> 943) is the Nightmare of Mensis' library lift 2601260 (events 12601351 / 12601352, usable after Micolash; regions 2602241-44 from the MSB: in the car at the bottom / top at x 119.4, the call spots at x 117.7). The fall recovery nets took the ride for a fall out of the world (the host warped to its lamp mid-ride). Now a fall sample needs a drop faster than a lift's (10 m/s) unless no lift car plays a ride where the chr stands (`worldact::OnMovingLiftCar`; the +0x1E5 bit 0x02 ride flag: not on the local chr in liftfall's el1 on 2601240, but on it for the whole library lift ride in liftfix's runs), and the nets fire only after 40 m in their 6 s (`deathrules::FallSample` / `FallRecoveryDue`); the fall-death drop test counts from the last ride sample (`PeakSinceRide`).
* The library lift's car is a one-hunter birdcage (screenshots of the 0.7.1 full smoke: two hunters
  in it overlap, weapons through the bars). Its position flag 12601253 (1: the car at the top; a state
  id, mirrored) flips when the rider has left the car at the far end, after the end animation
  (12601253 = 1: gate 2601261 anim 9, car anim 7; = 0: gate 7, car 9; init: car 2 / 4). It IS synced
  like every lift (liftfix, EMEVD traces of 12601352 on both games): a step into the car on one game
  starts every game's copy within ~0.05 s, both cars ride and arrive together.
* **The ping-pong (liftfix, fixed):** the game's sync passes a wait of the event on every game once one
  game's copy passes it. Every lift but two ends its ride when ALL players are out of the arrival region
  (3[7]); Cainhurst's lift (12500076 / 12500077) and the library lift end it when the LOCAL player is out
  (3[2] on 10000). In a party the copy of a player outside the car passed that at once, so every game's
  ride ended at the arrival with the rider still in the car, the flag flipped, and the restarted copy saw
  the rider in its start region: the car went straight back with it, back and forth until the rider
  stepped out at the right moment (run 2: the guest's copy passed its "outside" wait 0.1 s after the
  host's while it stood in the car, then its start wait in the same frame; 15 rides for 3; the 0.7.1
  full smoke had 5-6 for 3). Their end checks become the all-players check on every game at the first
  dispatch (`liftrules::LocalEndToAllPlayers`, `WorldactRewriter`: id 2 -> 7, the region to +4; start
  checks stay the local player's). The id stays in the loaded script, but every run of the event copies
  its argument bytes afresh (run 3: the second ride's 3[07] read 10000 as its region and never ended), so
  the region is moved on every dispatch; the id and the log line once per instruction and map load
  (run 4: four lines per game, eight later 3[07] dispatches). Installed with the party on only (the
  worldact init, before any map loads, so no event runs yet); alone in a party session 3[7] sees one
  player, as vanilla. `tools/party/lift_tool.py check GAME` lists the local-end rides of
  the game data and compares them with `kLocalEndRideEvents`.
* **The flag hold (liftfix, fixed):** a guest's copy reads the flag to pick its end once its player is
  out of the car, so the host's flip must not reach it before. 0.7.1 put the flip back while the player
  stood within a few m of the car, which also put back the copy's own end written in the same tick as the
  host's flip (full smoke 0.7.1: "kept at 1 until its end" after the car's anim 9 had ended; the guest's
  lift kept the old end until its next ride, the host refused its report as "occupied"). Now the world
  mirror holds the flag (`GuestWorldMirror::Hold`, `liftrules::MirrorHold`) only while the guest's car
  shows a ride animation or plays its end, and at the release the copy's own end stands (the image may be
  older than the ride: applying it wrote the host's previous end back, run 2); a later host change goes in
  as usual, a differing end goes up as the guest's ride report.
* Ride reports (liftfix): a guest's second ride to an end it had reported once in a map never went up
  (the `known` set): reports are per ride now (`liftrules::GuestLiftReport`).
* The fall in the 0.7.1 full smoke's el ride 1 (the host 998 -> 943 at up to 23 m/s, also rerun 2's 11 m/s
  over the last 15 m) was the test's: both hunters warped onto one spot of the one-hunter cage, the guest
  first, the host 1.3 s later onto the car already moving; the chrs stayed 0.3-0.5 m apart all the ride
  and the pushed one fell through the floor (guest rides alone in the same runs: no fall). The car itself
  never moved without a rider (the host's copy started from the guest's step, as every game's does).
* `worldact::LiftReplayTick` (any role) still plays the end animations (0x13CBFD0) on a game whose flag
  changed without its own ride. A guest's ride goes to the host as `lift_set` / `lift_clr`; the host
  applies it with the car loaded too, unless the host stands at the car or its own copy rides
  (`occupied`). Smoke group `elevator` (phase el: a guest ride with the host's copy ending first, a host
  ride with the guest's ending first, a guest who stays 2 s in the car; per ride both games' flag and car
  end, the rider's ride bit all the way, vertical speed).
* Riding together (liftfix, live run 4, smoke group `lifts_together`, phase lt): host and guest on
  Central Yharnam's lift 2411320 (a platform; its end waits for every player), each placed on the car by
  the game's own in-world warp (the host 0.75 m beside the trigger box, the guest into it once the host
  stood there), down and back up: both rode the whole way (16-17 samples each with the ride bit, at the
  far end on arrival), the same flags and car end on both games after each ride, no fall, no recovery.
  Run 4: el 22/22, lf 16/16, lt 16/16.
* Cainhurst's lift (liftfix, live run 6, smoke group `cainhurst_lift`, phase ch: the guest down, the guest
  up staying 2 s in the car, the host down): every ride on both games within 0.06 s, flags (12500074 /
  12505074) and car end equal after each, the rider on the car all the way (its peak 12.7 m/s: the lift
  speed limit is 16 m/s now), the end check rewritten once per event on each game. Run 5 found the host
  sitting 40 s in Cainhurst's first-arrival cutscene (the fixture now sets 12500000): its copies did not
  follow the guest's ride and the host's world kept the car at the top ("loaded, its copy did not
  follow"). Cainhurst's lift therefore replays like the library lift, and a ride refused while the host
  stands in the car (within 1.2 m now; 4 m refused every ride seen from its landing) is applied once
  the host is out (`HostLiftWaitTick`).
* The lf approach (liftfix run 6): the bottom landing's floor collision is not there right after a long
  warp (the guest fell through at two spots, runs 2-5); the move step's `:pin=4` holds the chr in front
  of the bottom lever while it streams in ("Operate Device" shown), and the speed check runs from the
  approach on.
* Ride frames (liftfix runs 7 / 8): the smoke shoots every rendered window each second through every
  ride (`shot.py --burst`, `ride<N>_<t>s_inst<i>.png` in each group's shots folder) and checks the
  position samples on the car for a jump (over 2 m sideways, or faster up or down than a lift). All 12
  rides: the riders on the car all the way, no jump, no fall. Seen in them: the library lift has a
  second cage (2601261, so far called its "gate"; regions 2602243 / 2602244 are its trigger spots) that
  moves the other way: it arrives beside a player waiting at the far end; Cainhurst's car is a wide
  platform (the landing spots of phase ch, 2.5 m from its axis, are on it, so both hunters rode every
  ch ride); in the narrow shafts (Cathedral Ward's, Cainhurst's) the camera goes into the rider's own
  model for a few seconds. Run 9 (liftfix): the second cage is in the lift table (`Lift::gate_rides`:
  the in-car and moving-car tests look at both cages; the event, flag, hold, end rewrite and replay are
  shared) and ridden (el ride D: the host down in it, 39 samples in the cage, the same flag / end on
  both games); Cainhurst's rides with one rider and the other player on a landing 1 m in front of a
  call lever (3.2-3.4 m from the axis: off the platform, its height steady, no ride bit) pass too.
* **The Research Hall lifts (liftfix, a player's report, fixed):** "riding an elevator I went through the
  ground and died" (lift 3501105, HP 0 at y 1546.1). Live (runs 11-17, the host alone and in a party, 30 and
  60 FPS, EMEVD traces of 13501112 / 13501113): the ride events of the Research Hall's 1F <-> 3F and 3F <->
  5F lifts clear their busy flag in the frame their "every player out of the arrival box" check passes; the
  other direction's start ("the player in that box") still reads the rider inside in that frame, so the car
  left again at once, with the rider on its wide platform's edge or about to step back in, who then fell into
  the 12 m pit under the car (y 1546, the report's spot). Every other lift clears busy after its end
  animation (the Research Hall's altar lift waits 1 s). `liftrules::BusyDefer`: the clear (index 26) is
  rewritten to keep busy on, and the event's end (index 30) clears it, after the end animation. Run 18
  (smoke `research_solo` / `research_lift`, 60 FPS): alone down, out, the car stays, back in, up; in the
  party down alone, up alone, down and up together: 4 ride starts only, every rider on the car all the way,
  no fall, no death, the ride frames checked. The fall-death accounting no longer counts a chr falling faster
  than any lift without its ride bit as riding (the report's 12 m fall was logged as 0.6 m).
* The lf smoke's guest ride (lifts071) never was one: the guest was put on the car object's root, which
  stays at the TOP while the car is at the bottom; it fell the 48 m shaft through the switch region, the
  up event ran with nobody in the car and the guest died at the shaft's foot (god mode does not stop a
  fall death; the verdict read flags only). It now steps in beside the car at the bottom.
* Fall recovery (liftfix): the nets also need one sample faster than any lift (16 m/s; Cainhurst's car peaks at 12.7), so no lift ride
  is a fall even where the ride test misses the car (the Chalice Dungeons' generated lifts).
* Ladders: the shortcut ladder of Central Yharnam's start (2411204, lowered by lever 2411316, event
  12410285): a guest's pull lowers it on every game (l10: the host's game gets the guest's ObjAct kick
  natively, anim 1 -> 2 and 12410285 = 1 on all three games, screenshots). The raised ladders of the
  ladder system (2009[0]) in the test saves are already down (dlast); no "kick" prompt was found.
* Illusory walls: Bloodborne has none. No event of any map (main game, DLC, the Chalice templates)
  is a hidden wall (EMEVD event names: only boss part breaks and one boss room window break).

Smoke: `python tools/mp/smoke_party.py --only lifts,ladder` (phase lf).

### 10.1.1 Guarded in 0.2.5: a rider drops through its moving car (the Nightmare Frontier report, 2026-10-26)

**Why lifts are still buggy.** The fixes above each closed one mechanism that the EMEVD traces explained:
the desync (D3), the ping-pong, the flag hold, the Research Hall's early busy clear, and the Forbidden Woods
runaway put-backs. One class has never had its cause found: **the game itself stops carrying a rider while
the car moves** (the chr's ride bit, PlayerIns +0x1E5 bit 0x02, goes off and the chr falls through the
car's floor). Nothing in the party code makes that happen. What exists is a symptom net, the rider guard
(`liftrules::RiderTick`, `worldact::LiftRiderTick`), and it has three weak points:
1. It guesses the car's height (the last ride sample plus that animation's speed) because the car object's
   matrix does not follow the ride (2701032 reads its top height with the car at the bottom). If the
   floor is not where the guess says, a put-back cannot hold.
2. Its put-backs were only seen live with `BB_TEST_LIFT_RIDEBIT_OFF`, which fakes the bit off while the
   chr still stands on a working car floor. A real drop-through, where the floor does not hold the chr,
   was never reproduced.
3. Since lifts2 it stops after `kRiderMaxPuts` (3) put-backs a ride, a bound added for the Forbidden
   Woods runaway. After that the fall recovery is the only net, and it needs 6 s and 40 m
   (`deathrules::FallRecoveryDue`). A lift shaft kills the player sooner.

The smokes ride the lifts of Central Yharnam, Cathedral Ward, Cainhurst, Hemwick, the Forbidden Woods and the
Research Hall, plus the library lift and the first shortcut lift of the Nightmare of Mensis. The other lifts of the table, including the Nightmare Frontier's
(3301000, flags 13300115 / 13300116, added in liftfall from the EMEVD only), were never ridden in a smoke.

**The report (a player's log, 2026-10-26, a build with the lifts2 guard).** The player was **alone in
their own world**: the party link never reached a host, so the replay, the mirror hold and the guest ride
reports did not run. No event of this lift is rewritten (only the Research Hall's `BusyDefer` and the
local-end rides of Cainhurst and the library lift are). Only the generic paths ran: the rider guard, the
fall-death accounting and the fall recovery. Nightmare Frontier lift 3301000, top about y 1493, bottom
about y 1446 (47 m):
* Ride 1: on the car at (10.9, 1493.0, -36.3), the recovery logged the ride (0.1 m/s down: "no fall").
  It rode 18 m normally. At y 1475.2 the bit went off and the chr dropped 1.1 m under the car. The guard
  put it back 3 times (1476.6, 1475.3, 1474.0); each time it dropped again 1.0-1.1 m under, about 1.3 m
  lower than the time before. Then the guard gave up. HP 0 at y 1445.8, "dropped 23.9 m in the 3 s
  before", killed by nobody (a fall).
* Ride 2 (after the bloodstain was taken back): the same pattern at the same spot (drops at 1475.8, 1475.2,
  1474.3; HP 0 at 1445.8, the shaft's foot).
* Frame time: 60.5 FPS steady (worst frame 24 ms, p99 22 ms, no frame over 1.5x the median), and no stall
  line before the drops. The liftfix explanation (the car's step past the chr's ground probe on a long
  frame) does not cover this one.

**Reproduced (headless, 2026-10-26).** Alone, a new game with the start lamp 3302950, 13300111 set (the
lift's one-time unlock: the bottom region 3302702 visited), the host put into the top in-car region
3302700 (10.5, 1492.75, -36.0); `log_pos=0.25`, `log_objanim=3301000`, the EMEVD trace of 13300112. The
same ride under both frame-rate presets (`tools/smoke-game.py --fps`); both headless runs step about 20
frames a second (the event's 10-frame wait took 0.51 s in each), so the game advances at the same rate in
both and only the preset differs:
* **30 FPS preset: clean.** The car's ride animation (anim 2) plays 7.1 s, the rider's ride bit stays on
  all the way, its speed follows the car (peak ~4 m/s wall), and it stops on the car at y 1471.17, the
  bottom in-car region 3302701 (y 1470.95). The ride is 21.8 m, not 47 m: the shaft goes on 25 m under the
  bottom stop (a pit, y ~1446).
* **60 FPS preset: the report.** The car plays the same anim 2 for the same 7.2 s (the car is not
  faster). The rider is not: it keeps up with the car for the first second, then descends faster than the
  car, 1.1x, 1.3x, then **2x the car's speed** 1.9 s into the ride (7.8 m/s against the car's ~4 m/s),
  **with the ride bit still on**. It sinks into the car's floor, the bit goes off at y 1484.4, and it falls
  through the floor at the car's pace or faster. The guard fired only at y 1470.1, under the bottom stop.
  Its 3 put-backs did not hold, it gave up, and HP 0 came at y 1445.7, the pit, as in the player's log.

**Why lifts are still buggy (the cause, as far as proven).** Under Lantern's `60 FPS++` the game
carries a character standing on a moving car faster than the car moves; at the 30 FPS preset it does
not. The party code does not cause it (the report and the repro are both alone in their own world), and
visy's lift code only reacts to it. On the Frontier's car the overshoot grows with the car's speed
until the rider is under the floor. It may also explain the earlier reports (unproven: none rerun at
30 FPS): the Research Hall rider "through the ground" at 60 FPS, the Forbidden Woods rider rising "a
foot at a time", the fall off Hemwick's car edge. Each fix so far handled the consequence (the event
timing, the guard, the recovery), never the carry itself.

Bisect of `60 FPS++` (158 lines; a variant writes chosen lines back to the executable's own bytes, so
the start's 1.09 check still accepts it; one headless ride each, `pos` sampled every 0.25 s):

| variant (lines restored to 30 FPS values) | ride |
| --- | --- |
| none (the full patch), 3 runs | drop 2 (bit off at y 1484 / 1468, guard 3x / 1x), clean 1 (h0, peak 4.7 m/s) |
| 26-65 (40 of the `1/30 -> 1/60` step constants) | clean |
| 66-104 (the other `1/30 -> 1/60` constants) | drop |
| 132-157 (code pointed at new constants, 0.1 values) | drop |
| 26-35, 26-30, 31-35 | clean each |
| 105-131 (`30 -> 60`), 36-65 in tens | not ridden (the headless new game's opening skip failed) |
| 30 FPS preset, 3 runs | clean all |

The full patch also rode clean once (h0), so **the drop does not happen on every ride** and one ride
per variant cannot clear a line: the clean rows above may be luck. The rider's peak speed with its bit
on varies from ride to ride under the same patch (4.7-9.5 m/s at 60, 5.1-6.6 at 30; the car's ~4 m/s).
The headless game steps about 20 frames a second, not 60, so the drop may depend on the real frame time
against the patch's fixed 1/60 steps; the player's report was at a steady 60.5 FPS, so this is not the
whole story. Lines 27-35 are `movl $1/30` into an update message (vtable 0x52efd30, its step at +8) or an
object's step field, in nine different functions (image offsets 0x63fc3c, 0xeb5452, 0xeb55b3,
0xf734ed, 0x12efe59, 0x17f9b71, 0x180ca97, 0x19aa658); line 26 is a branch at 0xbbc40f.

Why the guard cannot catch it: it measures the car's speed from the rider's own ride samples, and the
rider is already moving at the overshot speed while its bit is on, so its car height guess sinks with
the rider. It sees nothing until the rider is 1 m under that wrong guess, then puts the chr where the car
is NOT (below the real floor), three times, and stops.

**Fix plan:**
1. Root (engine, a `work/LANTERN_DIVERGENCE.md` row): finish the bisect above (full-patch repeats,
   then lines 26-35 one by one), read what the guilty step feeds (the character's ride on a moving
   hit, or the car's own motion), and correct its value per preset in the frame-rate
   corrections (`scripts/patches.py` `corrected_frame_rate`, checked by `tools/re/fps_corrections.py`)
   so every preset carries a rider exactly with its floor. Proof: the headless ride above at 60 / 90 /
   uncapped matches the 30 FPS trace (ride bit on, the rider's y with the car's within 0.1 m, the stop at
   1471.17).
2. Guard (visy's code, after 1): measure the car, not the rider. The car's map matrix does not follow
   the ride (3301000 stays at y 1492.71 all the way), so the guard needs the live object's transform;
   until then its speed must come from samples where the rider is not faster than any earlier sample
   by more than a lift can accelerate, and a put-back must clear the chr's fall speed.
3. Smoke: this ride as a QA row (alone, 60 FPS: no drop, no death, the stop at the bottom region), and
   every lift of the table ridden once (about half never were).

**Status (0.2.5):** step 2 is done; step 1 (the 60 FPS root) stays open.
- The drop is intermittent (full `60 FPS++` patch: 2 drops in 3 headless runs), so the bisect stopped;
  the exact constant is unproven.
- The car's live height: the car's hit object, `*(entry+0x288)+0xac` of its ObjAnimState entry
  (found with a temporary pointer scan, not shipped). It stayed 0.27 m under the rider through every
  ride at 30 and 60 FPS, and at rest at the top it equals the car's placement (1492.71).
  `LiftCarHeight` reads it (NaN when unreadable or more than 80 m from the placement).
- `liftrules::RiderTickMeasured` (via `RiderTick`'s `car_y`): the rider's offset to the car is kept
  from the ride's first sample; a rider more than kRiderSlip under car + offset is put back on the
  floor, with its ride bit on (carried ahead of the car) or off (falling), at most
  kRiderMaxPutsMeasured times a ride. Without a readable height the old speed guess runs unchanged.
  Unit cases in `test_party_lift_rules.cpp` (the report's ride, stepping off, a chr at the landing,
  the cap, the NaN fallback).
- Still open: the root in the frame-rate corrections, and every lift of the table ridden once.

**bbport 0.7.7 (lifts077, 38f45e63):** the measured guard above was ported as e57eb466 and then changed:
- At a lift's arrival the car animation ends and the measured height reads the end position at once while the
  rider is still 3-8 m below being carried; a put-back there threw the rider 3-5 m up when the car stopped
  (NW-666485f). No put-back now while the rider is rising, unless it is more than 12 m behind.
- Cainhurst and Research Hall cars report a height that never follows the ride; the measured guard looped
  put-backs there (12, then a fall). A car whose measured height stays static while the rider moves falls back
  to the speed guess.
- Open: a guest ride that starts before the car's block init ran snaps to the top at 90/120/60fg (CL-4e8bd3);
  `BB_TEST_LIFT_TRACE=1` logs rider and car height every 0.1 s.

### 10.1.2 A lift car that is not built yet (liftre077, 2026-10-29, live, eboot decompiled)

**Why guests fell through lifts or rode them "instantly" (CL-4e8bd3).** A map object (a lift car included) is not
a finished object when a player comes near it. The game builds it by levels, and the build takes 2-4 s of
resource loading:

* Object entries are `0x2E0` bytes in the blocks of the table at `0x553B168` (`0x13C9AF0` finds one by entity
  id). `+0x1F8` is the state (0 none .. 4 built), `+0x1F9` the level asked for (0 none, 2 model, 4 full),
  `+0x1FC` bit `0x40` active, `+0x1C8` the resource bits (bit 4: loaded), `+0x278` the model, `+0x288` the hit
  object (the floor), `+0x290` the animation controller, `+0x208` the last requested animation, `+0x1FE`
  bit 3 "an animation is playing".
* The level comes from a pass over the grid cells the loaded area covers (`0x15B2B20` -> `SetLevel 0x15A4810`:
  argument 2 builds fully, 1 model only, 3 drops). The state machine `0x15A30F0` builds (`0x15A2A00` ->
  `0x15A1710`) once the resources arrive, and the build replays the last requested animation
  (`0x15A25C0` with the record at `+0x2B0`).
* `PlayAnim` (`0x15A25C0`) on a car without controller plays nothing and does not set the playing bit, and the
  game clears a bit set by hand within a step. The lift event's wait for the animation's end (`2003[18]`,
  waiter `0x179BC30`, check `0x13CC180`) therefore passes at once. Live (Cathedral Ward 2401102): anim 31, 32
  and 7 requested in 44 ms, flags flipped, the car shows the end pose when it is built 1.5 s later and the
  rider guard snaps the rider (that is the "instant ride" and the "no ride seen" of the lf smoke).
* Until the car is built it has no floor: a player in its footprint falls down the shaft (live: lf at 60 FPS with
  the ride 1.2 s after the arrival, the guest dropped 25 m through the unbuilt car and died; that is the
  "phased through the elevator" of the report). The vanilla game never meets it: a loading screen builds
  everything around the lamp. The party's in-world warps (gather, party travel), a slow machine or a
  running player do.
* It is not frame-rate dependent in itself (it is wall time of resource loading), but a slower game or a
  harness wait shorter than the build makes it show at some rates only. The smoke waited for `?obj=2401102:30`,
  which matched the OLD stop of the lamp load's pose (lifts2077).

**The fix (all four are test-switchable, default on; `BB_PARTY_LIFT_PREBUILD=0`, `BB_PARTY_LIFT_HOLD=0`):**

1. *Prebuild* (`LiftPrebuildTick`, `SetLevelHook`): lift cars within 150 m of the local chr are asked for the
   full level (`0x15A4810` called by us once a second per car, and the game's own "drop" / "model only"
   requests for them are turned into 2). They are built before anyone can stand on them: measured 0.2-0.9 s
   after the lamp load.
2. *Hold* (`PlayAnimHook` on `0x15A25C0`, `WaitCheckHook` on `0x13CC180`): when a car that is active but not built
   is asked to play an animation, the wait for it keeps answering "playing" until the build replays the
   request for real (12 s at most, `liftrules::HoldCarAnim`). Far from every player nothing changes (the game's
   fast forward is right when nobody sees the car).
3. *Floor guard* (`LiftBuildGuardTick`): a local chr that starts to fall near an unbuilt car is put back on the
   spot it stood on and held until the car is built, then for 2.5 s more. A last resort: a fall at the build
   itself can still gain speed, the prebuild is what makes it rare.
4. *Ride end after a block change* (`g_lift_ride_seen`): a guest's ride end written while the guest crosses
   into the next block (Cathedral Ward's top is m24_00) was swallowed by that load's baseline and the host's
   world kept the car at the other end. A ride seen in the last 25 s is reported.

The rider guard (`liftrules::RiderTick`) stays as the backstop.

**How the game carries a rider (decompiled, for the next person).** Per game step: ChrIns update `0x18BE0A0` ->
controller step `0x1514500` (gets the step's dt) -> moving-ground test `0x18CB0E0`. That function clears the
ride bit (PlayerIns `+0x1E5` bit 2) at `0x18CB14F` every step and sets it again at `0x18CB369` when the standing
hit's object is flagged as a mover; the rider is attached to the standing object's node when that node's
motion vector (node array element `+0x40` / `+0x50`, base `*(*(0x553D700+0x28)+8)`, stride `0x80`) has a squared
length over 0.0005 (`0x4927D20`); the displacement is the anchored node transform delta
(`0x1A4DD20`, `0x1804620`), with no dt in it. The chr position is written from the Havok proxy by `0x1E38910`
(`0x1E389ED`, blend factor at `+0x300`). In every ride traced (Cathedral Ward, 30-120 FPS targets, the game
stepping at 30-60/s) the ride bit stayed on and the chr followed the car. No game-side fall-through of a
built car was seen; the frame-rate hooks do not touch these functions. What is unproven: whether the 0.0005
threshold turns the carry off in the slow ease phases at a real 120 FPS (the probe prints the node vector:
`BB_TEST_LIFT_PROBE=2`).

**Test aids.** `BB_TEST_LIFT_PROBE=1|2` (per-step car build state, hit, chr carry, node vector, the wait check),
`BB_TEST_LIFT_WATCH=1|2` (hardware write breakpoints on the chr's position / ride bit / standing hit, or on the
car's build fields; `party_lift_watch.cpp`), smoke phase `lfx` (the lf ride with two players), `BB_LF_PIN` /
`BB_LF_CAR_DELAY` (shorten the wait on the landing: the ride before the build).

## 11. Interface (for split travel and other callers)

* Wire format: EVENT `world_action` (`worldact::kEventName`), guest -> host, body
  `worldact::RequestToJson(Request{map, set, clr, kick})`; parse and shape-check with
  `worldact::RequestFromJson` (sizes 32 / 16 / 4, no duplicates, every id allowed in its own area).
* Policy: `worldact::WorldActionAllowed(id, clear, host_map)` - the category whitelist (section 2)
  plus "map flags must be of `host_map`'s area". The host applies a request in `HostTick` with
  `host_map` = **the host's current map**. A lone guest in a map the host is not in (split travel)
  produces flags of another area: such a caller must validate against the flag's own area
  (`WorldActionAllowed(id, clear, (id / 100000 % 100) << 24)`, as `RequestFromJson` does) and keep
  the rest (category whitelist, mirror mask, NeverWritable, rate limit `RateLimit::Take`).
* Guest-side detection is automatic only in load mode 1 (the guest in the host's world: the
  overlay vs. the mirror image, `progress::GuestLocalWorldChanges`). A guest in its own world (load
  mode 0) writes its own pool directly; whatever it changes there that the host's world should
  get must be collected by the caller (e.g. a pool diff over `WorldMirrorKeys()` filtered by
  `WorldActionAllowed`) and sent with the same EVENT.
* Own-save writes are the sender's job (the guest writes its own pool; the host never writes a
  guest's save).

## 12. Shared NPC dialogue

When a party member talks to an NPC, the members near that NPC get the conversation too
(`gpu/shim/party/party_npctalk.cpp`, EVENT `npc_talk`). Passive: nothing opens a menu or takes
input on a listener.

**RE (eboot 1.09, our offsets).**

* `0x2049170(state, voiceId)` plays every talk line: the talk command `TalkToPlayer` (case 1 of the
  talk command switch `0x203FFD0`, `0x2045870` -> `0x20478A0`) and each next row of the chain in the
  talk update `0x2047CB0`. `state+4` = the NPC's entity id, `+8` the base TalkParam id, `+0xC` the
  row index (TalkParam row = base + index: `{msgId, voiceId, ...}`), `+0x31` playing, `+0x34` the
  shown msgId. The voice: the name `v%09d` (voiceId), `0x27D3A80(handle out, name, pos, -1, 0, 1, 0)`
  at the NPC's position (`0x18C3BF0` dummy poly, else the chr's physics position), then
  `0x27E2C50` / `0x27E2380` on the handle (`0x27E23F0` stops it); `0x553D6F0 != 0`: no sound.
* The subtitle: while a line plays the talk update writes MessageUI (`*0x5562878`) `+0x80C` =
  msgId, `+0x810` = 0, `+0x58` = 4 (talk request), `+0x280` = 1; the talk stop `0x2048E60` writes
  `+0x280` = 0.
* Talk menu: command `0x13` AddTalkListData(index, msgId, ..) pushes `{msgId, index}` into
  MessageUI `+0xEB0`, `0x14` ClearTalkListData; the chosen row index is MessageUI `+0xEC0` (test
  `0x17` GetTalkListEntryResult). A command argument is `{value, ?, type}`: type 1 a float, type 2
  an int (live: AddTalkListData's index and message id are type 2). Live (the Doll): `0x14`, 4 x
  `0x13`, `0xA` (the menu), then `0x44` every frame with `+0xEC0` unchanged until the row is
  decided (the menu UI writes it on OK, 0x16B11C0 event 0x3ED), then the branch (`0`, `1`
  TalkToPlayer). `+0xEC0` is never reset: it keeps the last decided row.
* A game dialog (YES / NO, e.g. "Travel with the party?") is MessageUI `+0x78` != 0.
* Talk text: FMG category 1 (`会話`, 2835 lines; per speaker in blocks of 10000: 190xxxx Gilbert,
  130xxxx Iosefka, 40xxxx the Doll, 50xxxx Gehrman ...). Item names: 10 goods, 11 weapons, 12 armor.

**Talker.** Two observer hooks (they only read): the line `0x2049170` and the command switch
`0x203FFD0` (menu rows, the choice). Each line goes out as `{k:"line", npc, talk, msg, voice, map,
pos}`; the talk's stop (`state+0x31` = 0 or `+0x34` = -1, from `0x2048E60`) as `end` (a listener also
ends a line after its speaking time, ~0.07 s a character, 2.5-10 s: live, `+0x31` can stay set
long after the subtitle is gone, and `+0x38` / MessageUI `+0x280` drop while a line still shows); the chosen row as `choice`; an item lot the game
awards within 30 s of a line (party_items' award hook) as `gift`. A guest sends to the host; the
host names the talker from its roster (`who`, `slot`, never trusted from a guest), relays it to the
other members and broadcasts its own. 24 events per 10 s per member at most.

**Listener.** Same world (both in the host's world) and same map, then the distance from the local
player to the NPC (the local NPC chr, else the talker's position) against the range (Party rules
"NPC dialogue range", default 25 m, 0 = the whole map; `BB_PARTY_NPC_TALK_RANGE`):

* within range: the game's own subtitle (MessageUI writes above, every frame while no menu is up and
  the local game plays no talk of its own), the voice at the NPC (attenuated by the game's 3D sound
  like the game's own lines), the caption "[Hunter talking to Gilbert]" above the subtitles, the
  talker's choice ("Hunter chose: ...") and gift ("Hunter received ... from Gilbert");
* further away: a quiet party notice "Hunter is talking to Gilbert" (once a minute per NPC);
* a line the listener's own game played for the same NPC in the last 20 s is not doubled.

Logged as `Party: npc_talk: ...` (every local line with its talk id, every shared line with the
distance decision). Switches: Party rules "Share NPC dialogue" (bbport.ini `party_npc_talk`,
launcher Party > Share NPC dialogue, `BB_PARTY_NPC_TALK=0`), `BB_PARTY_NPC_TALK_VOICE=0`,
`BB_PARTY_NPC_TALK_SUBTITLE=0` (our caption line instead of the game's subtitle),
`BB_PARTY_NPC_TALK_TRACE=1` (every talk command id with its args, the menu result, MessageUI
`+0x58` / `+0x78`).

**Choice detection.** After the menu shows, the choice is the first command of that NPC whose
`+0xEC0` differs from its value when the menu showed, or the NPC's next ClearTalkListData (the same
row chosen again leaves `+0xEC0` unchanged). A choice / gift line stays under the caption header for
6 s, also when the NPC's answer line follows at once. While a game dialog is up (`+0x78`), the
listener writes no subtitle request and draws no caption (live: both were drawn across the
"Travel with the party?" text).

**Only conversations (2026-10-20).** The workbench (t210692), the Bath Messengers' shops (t210691,
t210698), storage and the headstones (t210000) are talk scripts with a talk menu but no talk line.
Their choices went out with the talk id of that player's last conversation, so the others saw
"[Hunter talking to the Doll]" (or Gehrman) at the workbench (2026-10-20 session logs: `choice from
... npc 210692 (the Doll)`). A choice is now shared only when its NPC spoke this game's last talk
line (`ChoiceInConversation`); the others are logged on the talker (`local choice: ... a menu
without talk (workbench, shop, headstone), not shared`). Live 2026-10-20 (uifix, guest in the
host's Dream, one session): the Doll's talk and "Channel Blood Echoes" showed the caption on the host
(`choice shown: Hunter1 chose: Channel Blood Echoes`); the workbench's "Fortify Weapon" and the
messenger's "Acquire Items" (a purchase) were logged as not shared on the guest, and the host logged
no npc_talk line and showed no caption (screenshots while the guest's workbench menu was open).

**Live (2026-10-14, npctalk2, 2 instances in the Hunter's Dream).** Lines: each shared line reached
the listener 9-30 ms after the talker's line; ends: the talker's talk stop reached the listener 10-30
ms later (the subtitle went with the talker's, screenshots); a line the talker holds past its speaking
time ended on the listener up to 1.1 s early (speaking-time estimate; "Very well, let the echoes
become your strength." 5.3 s vs 6.4 s on the talker). Choices, both directions: guest -> host
`choice shown: Hunter1 chose: Channel Blood Echoes`, host -> guest `Hunter0 chose: Channel Blood
Echoes` in the log (before the fix of the argument types and the decision rule no choice was ever sent: rows
read as 0, the menu resolved on the first per-frame wait command). Voice: `voice v210400250 at
(-5.8, -5.6, -13.6) through 0x27D3A80 (handle ...)` for every shared line (muted test: log only).
The dialog rule live: no subtitle request on the host while its dialog was up (screenshot: the dialog alone). The 6 s choice line was added after the run (unit-free, not re-run). Not seen live: the far notice (the host found no prompt at Gehrman in this save), a gift. Open: the
caption (and the party name tags) draw over the listener's own full-screen menus (e.g. its level-up
screen: no "game menu open" signal yet; the game's own subtitle is dimmed under the menu).

## 2026-10-25 (simonwipe): a guest's boss phase event no longer starts the host's scene

Player report (Nightwatch 0.7.4 / 0.7.5): a party wipe at Ludwig (Hunter's Nightmare), then the Holy Blade scene
(34000030, event 13404825, warp 3402807) played at the host's respawn and the host stood alone in the second
half. Host log: guests dead in the arena for minutes (the host's wipe load runs while they still hold the fight),
the host's flag 13401802 turning on at the load (a peer's state of Ludwig's "corpse flinches" event), then the scene
~16 s after the load. Ludwig's events are network-synced (packet 0x31, `BB_PARTY_EVENT_SYNC_LOG=1` lists them);
a guest's copy of the heat-up event 13404824 (Ludwig's HP ratio in (0, 0.5), `4[2]`) finishing on the guest's own
chr tells every peer, and the host's 13404825 (waits on 13404824) plays the scene and warps into the arena.
Reproduced on loopback (smoke phase lw, group `ludwig_wipe`): after the wipe's respawn load the guest's own
Ludwig copy is set to 40 % (`guest_kill=3400800:hp40`): the host logged the scene and the second bar 0.1 s later.
Fix: `bossfog::HostOnlyBossEvent` (every `..ヒートアップ..` event of the EMEVD scan, Ludwig 13404820-25): the host
drops a peer's 0x31 state for them (`EventSyncRead`, log `Boss phase: event N's state from a guest not applied`);
guests still take the host's. `BB_PARTY_BOSS_PHASE_SYNC=1` restores the old behaviour (test A/B). What is not
proven: why a real guest's copy fell below half (the stale doping row 7502 against the reloaded host chr's HP is the
likely source); the filter does not depend on it.
