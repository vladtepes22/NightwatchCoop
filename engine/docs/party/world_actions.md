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
  (6000-8999 stay in the own pool).

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

* Elevators: toggling state flags are shortcut_door; a guest and the host using one elevator at
  the same time can fight over the state (not tested).
* Instanced pickup into the own save: the fixed path (no baseline) not yet re-run live; enemy
  drops: a guest's real kill (damage, not a local ForceDead) and both players' rolls not yet seen.
* Memory Altar / covenants / chalice ritual by a guest not run live.

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
