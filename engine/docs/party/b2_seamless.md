# B2: true seamless travel (session and remote players survive map loads)

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA; bbhost = ours + 0x400000).
Sources: Ghidra decompiles (`tools/re/decomp.py`, a persistent headless request server for this
work), capstone over `eboot.elf` with the SCE relative relocations applied (vtables / step tables),
the runtime probe of the prototype (section 6). Builds on travel.md (B1), event_flags.md,
npc_peer.md, four_players.md.

Marks: **[C]** confirmed in disassembly / decompile, **[R]** confirmed at runtime here,
**[I]** inferred (shape / names), needs a runtime check (two instances).

## TL;DR

* The "remote players can't be re-created after a load" assumption of travel.md §4 is **wrong**.
  Remote `PlayerIns` are created by a per-member task, `CSMultiPlayerInsTask`, whose step machine
  is re-enterable: rewinding it to `STEP_Create` (+0x54 = 4) with its create bit (+0xED |= 1)
  rebuilds the chr from data that already survives a load (the network flow's member list and the
  GameDataMan remote rows). No join, no summon blob, no NpId re-negotiation is needed. [C]
* The join snapshot is **re-sendable to one peer**: `0x1786050(_, peerId)` (packet 0x10 flags),
  `0x1786360(_, peerId)` (packet 0x11: decals + map items + `CHR ` enemy states of the current
  map), `0x17894E0(flow, peerId, type, buf, n)` for 0x19 / 0x42, plus the per-peer net-sync
  registrations `0x18F0DB0` / `0x18F0E80`, bullets `0x1A04190`, NPC summons `0x15BD0E0`. [C]
* The guest's "enter the host world" step is a state of the network flow (`0x553D6D0 +0x84`):
  1 = joined, waiting for the host snapshot; 2 = snapshot applied; 3 = loading into the host
  world; 4 = in world. Putting a connected guest back to 1, letting the host re-send, then 3 + a
  warp **re-runs only the world-entry half of a join** with the room and signaling untouched. [C]
* Host side, the room survives a load with the known retarget plus two more call sites; the
  CSMultiPlayMan reset can simply be skipped for human tasks (they hold peer ids, not chr
  pointers). [C]
* **Verdict: feasible.** Not "research-grade, months": the engine already contains every piece
  (it is how a vanilla guest gets from its own world into the host's world). Effort for a working
  2-player B2: **~2-3 weeks** (prototype here + two-instance bring-up + edge cases), 4 players and
  death/boss/Dream edge cases another 1-2 weeks. Main risks in section 7.

## 1. What a map load destroys and keeps

### 1.1 Session / room / signaling

| Object | On a host load (vanilla) | On a guest host-world load (+0x84 == 3) |
|---|---|---|
| SprjSessionManager room (`0x5540290`, role +0x124, peers +0xF8..+0x100 stride 0x20) | closed: stage payload dtor `0x19470F0` calls BeginLeaveSession `0x1ED07A0` at **0x19471B1** whenever flow +0x84 != 3; also stage-record UID change `0x193C657` (role 1/3) [C] | kept: the dtor skips both the leave and the CSMultiPlayMan reset when +0x84 == 3 (`cmp [flow+0x84],3 ; je 0x194720C` at 0x1947171) [C] |
| Network flow `0x553D6D0` (+0x68 member list {PlayerId @+0x10, GameData slot @+0x30}, +0x48 pending-data list, +0x84 state) | kept (a persistent singleton; only `0x178A900` "session ended" clears it, and that is deferred while +0xC5 / +0x84 == 3) [C] | kept [C] |
| Signaling / P2P | follows the room (Matching2 leave) [C via travel.md]; with the room kept, untouched [I] | untouched [C] |
| Flow state timer | states 1..3 accumulate +0x108; at >= 60.0 s (`0x55640E0`, set by 0x1790243) BeginLeave [C] | same |

### 1.2 Remote PlayerIns, ChrIns, manipulators

* **Every PlayerIns dies with the world.** The world dtor `0x1547B40` calls WorldChrMan finalize
  `0x1914F40`: clears the map chr set (+0x48) and the player set (+0x30, `0x18DA510` destroys each
  entry) and writes WorldChrMan +0x60 = 0. The local player is re-created in MoveMapStep
  `0x1939470` (`if (+0x60 == 0) 0x18DB3C0(...)`). [C] (runtime: section 6)
* Remote PlayerIns are created only by `CSMultiPlayerInsTask::STEP_Create` `0x1E50090` →
  `0x18DB500(WorldChrMan+0x30, chrType, row+0x10, peerId)` (first free entry from index 1), then
  vfunc +0x640, +0x1A0, +0xB0, +0x610(1), the slot-table registration `0x15BC590(...,kind 1)` and
  (host) a playlog record. The NetworkManipulator comes with the PlayerIns factory. [C]
* `0x1E54830` (CSMultiPlayMan reset, also from the dtor at **0x1947207**) destroys every task
  (human vector +0x10/+0x18, NPC vector +0x38/+0x40); the task dtor `0x1E54340` only frees the
  task (no chr access). [C]
* The tasks reference their member by **PlayerId only** (+0xC8, 0x20 bytes, compared with
  `0xCA1C70`); every step looks the chr up again (`WorldChrMan+0x38` entries, chr +0x3D8). So a
  kept task across a load is safe. [C]

### 1.3 The guest's host-world state

* Host world mode = `GSM(0x5556678)+0x1592 == 0`, set by the MoveMapStep ctor `0x1937570` when
  flow +0x84 == 3 (also FrpgNetMan +0xB14 |= 8, local GameData +0x5D8 = session type,
  `SetLoadMode(1)`); +0x1592 is set back to 1 by the stage-end step `0x1942720` and re-zeroed by
  the next ctor if +0x84 is still 3. [C]
* The flags of a host-world load come from the overlay (+0x88 of SprjEventFlagMan), filled only
  by the packet 0x10 apply `0x13BECA0` (12 groups: globals + **the current map**). [C] Since
  2026-10-11 the host-world mirror fills the rest right after that apply (K11 calls
  `progress::OnVanillaSnapshotApplied`, and again after the overlay guard below): every group of
  the host's world but the boss / enemy zones (event_flags.md 12). [R]
* The enemy / object state of a host-world load comes from the host's packet 0x11: the receive
  (state 1 only) stores three blobs at GSM +0x16C0 / +0x16A0 / +0x1684; WorldChrMan `0x191BDD0`
  applies the `CHR ` blob (+0x1684 in host-world mode, the guest's own +0x1690 at home). [C]
* The guest's "go home" data: own-world snapshots +0x1690/+0x16B0/+0x16D0 (written by the
  SummonedMapReload serializer `0x131E5C0`, which also sets +0x84 = 3) and the return spot
  GSM +0x14A0..+0x14C4 → forced placement in `0x1942720`. Both must **not** be rewritten by a B2
  re-entry (or the guest's own world would get the host's state when it goes home). [C]

## 2. The join, as the engine runs it (what B2 re-runs)

`CSMultiPlayerInsTask` (0x100 B, vtable 0x534F650, ctor `0x1E4F9F0(task, peerId, mode)`; step
table built at 0x1E50F00, names from the strings) [C]:

| Step | Fn | Waits for / does |
|---|---|---|
| 0 Init | 0x1E4FCD0 | - |
| 1 StartMultiPlayNotifyWait | 0x1E4FCF0 | flag +0xED bit 2 (packet 0x42 from the member: +0xEC = its presentation slot; also bit 5 from FrpgNetMan +0xA98) |
| 2 StartMultiPlayWait | 0x1E4FD50 | enter presentation (EnterleaveDirector 0x553E8B0); mode 0 copies the return spot +0x1620.. → +0x14A0.. |
| 3 FirstSyncCompleteWait | 0x1E4FF70 | modes 1/2: bit 3 (set when the member's packet 0xC arrives, 0x178351D) |
| 4 **Create** | 0x1E50090 | modes 1/2: **bit 0**; finds the member's GameData slot in flow +0x68, builds the PlayerIns (above) |
| 5 CreateWait | 0x1E505E0 | presentation; clears bit 0 |
| 6 Update | 0x1E50830 | until bit 1 (leave) |
| 7 WaitExitMultiPlay | 0x1E50860 | |
| 8 Delete / 9 DeleteWait | 0x1E50880 / 0x1E50A70 | leave presentation; release slot `0x15BC080`, remove chr `0x19186E0` |
| 10 | 0x1E50D70 | +0x54 = -1 (done) |

Fields: +0x50 current step, +0x54 requested step (the update `0x1E52FE0` copies +0x54 to +0x50
and runs it; steps advance with `0x1E54250` = +0x54 += 1; vfunc +0x28 = set +0x54), +0xC8 PlayerId,
+0xE8 mode (0 local guest, 1 a guest member, 2 the session owner as seen by a guest, 3 local host),
+0xEC presentation slot, +0xED flags (0 create, 1 leave, 2 start notified, 3 first sync, 4 return
via presentation, 5). Mode makers (standalone, uncalled copies of the inline code): 0x1E549C0 (0),
0x1E54B60 (1), 0x1E54A90 (2), 0x1E548F0 (3). [C]

Session events, handler `0x178B5E0(flow, event)` [C]:
* 7 create success (host): flow +0x84 = 4, reset GameData remote rows, local task mode 3.
* 8/9 join success (guest): +0x84 = 1, Lua `OnJoinMutiplay`, local task mode 0.
* 0xE member joined: task mode 1 (or 2 if the member is the room owner `SessionMan+0xD0`), a free
  GameData row (`*(GameDataMan+0x18)+1+i |= 1`), the member into +0x68 and the pending list +0x48;
  **host only**: packet 7 (4 B), 0x19 (0x44 B, GameDataMan +0x68), `0x1786050` flags (0x10),
  `0x1786360` world (0x11), bullets `0x1A04190`; all roles: packet 0x42 (GSM +0x16F4),
  `0x18F0DB0(WorldChrMan+0xA40, id, *(0x553B148)+0x20)`.
* 0xF member left: delete its task (`0x1E54DB0`), free its row.

Per-frame packet handler `0x1783160` (from the flow update `0x1789B60`) [C]:
* 0xA (data request) on a state-4 peer → replies 0xB, 8 (0x168 B player record), 0x3B/0x3C,
  0xC, 0xD, 0x13, and (host) `0x15BD0E0` NPC summons + `0x18F0E80`.
* 8 / 0xC / 0xD fill the member's GameData row; occupancy byte bits 2/4/8 (+0x10 elsewhere);
  0xC also sets the task's bit 3.
* state 1 only: 0x19 → GSM +0x1593/+0x1594; 0x11 → the three blobs; then (role 6, +0x16A0 > 0)
  0x10 → inflate, `0x13BECA0`, **+0x84 = 2**, Lua `OnBeJoinStart_White` (`0x1317780`, call
  **0x178524F**). The Lua then runs `SummonReloadStart` (`0x1389440` → `0x131E5C0`: own-world
  snapshot, +0x84 = 3) and the warp.
* At the end of a host-world MoveMapStep, `0x1939470` → `0x178D9C0`: +0x84 = 4, packet 0xA to
  every pending member. `0x1947310` (net update, +0x84 == 4) pops a pending member whose row
  occupancy is 0x1F and sets its task's bit 0 → STEP_Create. At "InGameStart" (`0x193A3E0`) every
  player re-broadcasts its own record (`0x1781AC0`: 0xB, 8, 0x3B/0x3C, 0xD) - **on every load**.

## 3. Answers

**(1) destroyed / kept** - section 1. Room: closed by two call sites (fixable). Remote chrs: always
destroyed with the world, their tasks destroyed by one call site (fixable), their data (member
list, GameData rows, task flags) kept. Guest host-world state: kept except what is per map (flag
overlay groups of the new map, the `CHR` blob) - both come with the re-sent snapshot.

**(2) re-join without leaving** - yes, and it does not need the join-time insertion code path as
such: keep or recreate the task, rewind to STEP_Create with bit 0. Inputs: PlayerId (task +0xC8 /
flow +0x68 / SessionMan peers), the GameData row (kept), the presentation slot (+0xEC, kept or
packet 0x42). No summon blob, no session type is needed (the row carries chr type +0xA4 and
session type +0x5D8). The guest side re-enters the host world through the flow states 1 → 2 → 3
→ 4 with the room up; only the Lua join start must be kept quiet (it would overwrite the
own-world snapshot).

**(3) the guest's load and world sync** - the guest's load must be a host-world load
(+0x84 == 3 before the warp; the old stage dtor reads it, and the MoveMapStep ctor), the
destination is the host's own (B1's TravelIntent replayed with `0x13CDF30` / `0x132E010`). The
snapshot senders take a target peer and read the *current* world, so they must run after the host
has finished its own load and before the guest's load starts. The receive side reads 0x11 / 0x10
only in state 1 (queued until then). Enemy net sync per peer is re-armed by `0x18F0DB0` /
`0x18F0E80` on the host's new WorldChrMan (+0xA40 is per world [I]).

## 4. Patch / hook list (byte-verified, prototype `gpu/shim/party/party_b2.{h,cpp}`)

| Id | Site | Bytes (1.09) | Change | Why |
|---|---|---|---|---|
| K1 | 0x19471B1 | `e8 ea 95 58 00` call 0x1ED07A0 | call-site hook: no leave while role 3/6 (or the existing XML retarget `e8 ea 8e 58 00` → 0x1ED00A0) | host keeps the room at unload |
| K2 | 0x1947207 | `e8 24 d6 50 00` call 0x1E54830 | call-site hook: with a session, keep human tasks, destroy NPC tasks as the reset does | tasks survive |
| K3 | 0x193C657 | `e8 44 41 59 00` call 0x1ED07A0 | no leave with a session | stage-record change (streamed blocks) |
| K4 | 0x193C689 | `e8 82 26 c8 ff` call 0x15BED10 | skip with a session | keeps the presentation members |
| K5 | 0x13809B7 | `e8 e4 fd b4 00` call 0x1ED07A0 | no leave while a re-join runs | OnMatchingCheck: member chr missing while loading |
| K6 | 0x178524F | `e8 2c 25 b9 ff` call 0x1317780 | skip while a guest re-join runs | no vanilla SummonReloadStart |
| K7 | 0x13CDEAE | `75 6c` | `eb 6c` written / restored around the guest's own warp (main thread) | guest warp without leaving |
| K8 | 0x18C4D7D | `e8 2e 00 59 00` call 0x1E54DB0 | skip on the host (role 3) | ChrIns_OnDeath asks every human task to return when the host dies |
| K9 | 0x1381A93 (B1 SendHomeHook) | - | `SetSendHomeVeto`: a B2 guest drops HostDead_1's send-home | the guest follows the host's death travel instead of going home |
| K10 | 0x13858F5 / 0x1385A95 | `e8 a6 ae b4 00` / `e8 06 ad b4 00` call 0x1ED07A0 | call-site hook: no leave with role 3 or 6 | the party stays after a boss (BlockClear2_1 / _3 run on host and guest; 6.7) |
| S1 | GameData +0x5D8 of members / the guest itself | - | last counted session type put back after a load | cooperator count 0x15BDC20 (watchdog, EMEVD 1003[109]) stays right (6.7) |
| E1 | NowLoading `*0x5562878` | - | guest at the host's travel event: +0x808 = 5, byte 0x556286B = 1, +0x17C0 = 0, +0x804 = 1 (the inline request of 0x1929007); dropped with 0x176D3C0 on a fallback | one continuous loading screen for the guest |
| D4 | task update | - | a rewound task whose +0x54 stays unapplied for 2 s gets one update run by us (`0x1E52FE0(task, &{0x52EFD40, dt})`, as 0x1E556F0 does), not during SprjRemo | rewind gap fallback |
| D1 | flow +0x84 | - | guest: 1 on host travel, 3 after the snapshot (the setter 0x178D9A0's write) | host-world load |
| D2 | task +0x54 / +0xED | - | = 4 / \|= 1 for remote tasks without a chr | re-create remote chrs |
| D3 | per member | - | host after its load: 0x19, `0x1786050`, `0x1786360`, `0x1A04190`, `0x18F0DB0`, `0x18F0E80`, `0x15BD0E0`, 0x42 | fresh snapshot / net sync |

Ordering: guest D1(1) on the TRAVEL event (room still up) → host loads (K1, K2, K3/K4) → host D3
→ guest gets 0x11 + 0x10, +0x84 = 2 (K6 quiet) → guest D1(3) + B1 replay under K7 → guest
load (old dtor keeps tasks and room by itself in state 3) → +0x84 = 4 → guest D2 (host chr),
PartyLink "arrived" → host D2 (guest chr). Fallback: no snapshot within 45 s → +0x84 = 4 and the
B1 path.

## 5. Prototype status

`BB_PARTY_B2=1` (implies the party layer; `BB_PARTY_B2_FORCE=1` exercises K2 without a session).
Wired: `party_director` (tick, outgoing "b2" events), `party_runtime` (the guest takes the host's
TRAVEL event when B2 is on; the host receives "arrived"), `party_travel::ReplayTravelNow`.
Single-instance results in section 6; the two-instance path (D1-D3) is **untested**.

## 6. Runtime (single instance)

Runs: `tools/mp/instances.py run --count 1 --env BB_PARTY=host --env BB_PARTY_AUTO=0
--env BB_PARTY_B2=1 --env BB_PARTY_B2_FORCE=1 --env BB_PARTY_TEST_NPC=auto
--env BB_PARTY_TEST_NPC_WARP=2412951 --env BB_PARTY_B2_TEST_WARP=2412951` (save in the Hunter's
Dream; the fixture warps to Central Yharnam and summons NPC 2410158, B2 warps once more with the
NPC task alive).

* All sites byte-verified at install: K2-K7 hooked; K1 found already retargeted by the XML
  "Keep session on map reload" (`e8 ea 8e 58 00`), as party mode applies it. [R]
* **Local PlayerIns is re-created by every load**: `0 -> 0x10149d0840` (title → Dream),
  `0x10149d0840 -> 0x10229bc550` (Dream → m24_01); the player set stays 5 entries. Confirms 1.2:
  remote PlayerIns cannot survive a load, only their tasks can. [R]
* K2 with a live NPC task (`[npc step 1 life 0x62]`): reset skipped, the NPC task destroyed by
  our replica of the reset's NPC half, load completed, the fixture re-summoned the NPC in the new
  map; no crash, no leaked slot. [R]
* Not checkable with one instance: K1/K3-K7 actions, D1-D3 (need a session).

### 6.1 Live, two instances (loopback party, `--party`, save in the Hunter's Dream)

`instances.py run --count 2 --party --env BB_PARTY_B2=1 --env BB_PARTY_B2_TEST_WARP=2412951
--env BB_PARTY_B2_TEST_HOSTWARP=1` (the host lamp-warps to Central Yharnam once the guest's chr
has stood in its world; the director's bells did the first summon in the Dream).

Run 1-3 (20:00-20:30) [R]:
* Host: K2 kept both tasks through its load (`[mode 3 step 6] [mode 1 step 6 0x0c]`), role
  stayed 3, flow 4; the guest's chr gone after the load (NO-chr) as predicted; "join snapshot
  re-sent (0x19, flags ok, world ok, bullets, net sync, NPC, 0x42)".
* Guest: took the TRAVEL event, flow 1; the game applied the host's snapshot (flow 2, K6 ate
  `OnBeJoinStart`); flow 3 + lamp warp under K7: **one loading screen, role stayed 6 (client),
  no "returned home"**; the old stage dtor kept its tasks (flow 3); `0x178D9C0` set flow 4; K5
  suppressed one OnMatchingCheck leave; rewind → **host chr re-created on the guest**
  (`[mode 2 step 6 0x0c chr]`).
* Host: rewind → **guest chr re-created on the host** (`[mode 1 step 6 0x0c chr]`).
* Bug found: the guest landed in **m21_00 (stage 0x15000000), not m24_01**: a guest's loads use
  the *summoned placement* while GSM +0x1521 is set (OnJoinMutiplay): the descriptor builder
  `0x1943F10` takes type 2 with GSM +0x14C4 (stage), +0x14D0 (position), +0x14E0 (rotation),
  ignoring the lamp warp's +0x0C. Fix (commit 1411e30): after its load the host sends its stage
  (PlayerIns +0x3F8) and physics transform (+0x58→+8→+0x3B0→+0x68: +0x1E0 position, +0x1D0
  rotation) in a "b2" "dest" event; the guest writes them (1.5 m aside) into +0x14C4/+0x14D0/
  +0x14E0 before its warp. The host side "arrived" event was never delivered: the host had no
  `on_event` callback in party_runtime (fixed in the same commit); the host fell back to its 60 s
  timer.

Run 5 (21:10, with the fixes) [R]: **the guest arrived in m24_01 (stage 0x18010000) at the host's
spot, in session (role 6, flow 4), after one loading screen**; the host got "arrived", rewound,
and the guest's chr stood 0.4 m from the host on the host's screen (`chr pos (-193.4, -28.6,
68.9)`). Open: on the guest the rewound host task stayed `step 6 next 4 flags 0x0d NO-chr` for the
10 s the run lasted - the task update (0x1E556F0, from the flow update 0x1789B60, skipped while
SprjRemo plays) did not run it. In runs 1-3 the same rewind did create the host chr. Next run
logs the flow timer (+0x98, advanced by 0x1789B60), GSM +0x1521 and SprjRemo to tell which.
GSM +0x1521 read 0 on the guest in run 5 (1 expected after OnJoinMutiplay), which is why the lamp
warp's own target was used; with +0x1521 = 1 our +0x14C4/D0/E0 write gives the same spot.

### 6.2 Runs 8-10 (after the gate; fast join merged) [R]

* Run 8 (host lamp warp): guest still stuck at `step 6 next 4`; the status showed **SprjRemo
  +0x168 bit 0 = 1** on the guest from its arrival on (a remo playing: the C4 story log says
  "remo playing (map m24_01)"; bit 0 is set by the remo start 0x1CFA310). The flow update skips
  every task while it is set - the rewind gap's cause.
* Run 9 (D4 kick also during a remo, after 3 s): **closed** - `kick: task mode 2 stalled ... (remo
  1); running its update` → `guest: re-join done after 3.0 s`, host chr on the guest at the
  host's position; host got "arrived" and re-created the guest's chr. Whole lamp warp in 52 s of
  run time; the early guest loading screen (E1) was raised at the travel event.
* Run 10 (host death, ForceDead(10000) on the host 5 s after the guest stood in its world): K8 kept
  the member task (twice), B1 captured `host_death` and broadcast it; the guest dropped
  HostDead_1's send-home, stayed client, raised its loading screen, got the snapshot and the
  host's respawn spot, loaded into m24_01 in host-world mode, rewound; the host re-created the
  guest's chr on "arrived". **No session loss, no "returned home".**
* `HostCloseEmptyRoom` (B1, upstream b58a940) is off under B2 (the room must stay).

### 6.3 Runs 11-16: boss clear, gather, what players see, spacing [R]

* Boss clear (runs 11-13, `BB_PARTY_B2_TEST_BOSS=BlockClear2` raised on the guest by name): the
  Lua state ran (dispatcher 1) and the guest stayed a client for 30+ s, but no BlockClear2_1 /
  K10 / send-home followed - the white-phantom branch of `BlockClear2` 0x13849A0 needs a real boss
  kill state before it chains on. Inconclusive for K10; no send-home leak seen. A real boss fight
  is needed to close this.
* Split maps at session start (runs 12-13): the guest's own save in the Hunter's Dream, the host's
  in m24_01; the bell join placed the guest at its sign spot - the Dream - **in the host's world**
  (bells anywhere). Fixed by **gather** (`BB_PARTY_GATHER`, default on, party_director): before a
  guest rings, if the host's roster map (slot 0) is another area/block, the guest's own world
  lamp-warps to the host's last lamp (`GuestDeathRedirectLamp`, kept by party_phantom). Run 13:
  `gather: own world m21_00_00_00, host in m24_01_00_00; to the host's last lamp 2412951` →
  joined in m24_01 next to the host. The same check runs before every ring, so it also covers
  "after any return home".
* What players see (run 14-15, `tools/mp/shot.py` = PrintWindow screenshots on log events): at the
  host's lamp warp both windows show the loading screen (item tips) from the travel event on; the
  guest's screen stays a loading screen through its snapshot wait and its own load (E1 works) and
  opens in m24_01 beside the host; a remo plays on the guest right after arrival (the SprjRemo
  bit seen before). Which remo is open (not identified).
* Spacing (runs 14-16): the game's join placement is the sign spot = the shared lamp spot (0.2 m
  from the host, the host got shoved 0.4 m). Writing GSM +0x14D0 before the join load had no
  effect (the descriptor is built earlier); instead after any host-world arrival (flow 3 → 4,
  bell join or B2) the guest's chr is warped beside the host with the chr warp vfunc +0x5F8
  (`0x18CD600(chr, pos, rot, 1)`, as the NPC summon step does): `BB_PARTY_SUMMON_OFFSET` metres
  (default 0.5 since 2026-10-11, side by side: right / left / 2x right by slot; 2.2 put guests
  into walls at narrow lamp spots such as Central Yharnam; 3.0 judged too far), each try checked
  30 ticks later (reached, same height ±1.5 m) with the next side as fallback and a log of how far
  the host moved (push check). Run 15 (3 m): warped from 0.4 m to 3.0 m. No ground probe yet:
  the host's height is used.
* Lamp ids: 2412951 is the outdoor Central Yharnam lamp (screenshots of the arrival are a street,
  position (-193.4, -28.6, 68.5) in m24_01), as travel.md's shadp2p capture says; 2412952 Great
  Bridge; 2412950 (not seen here) is the likely 1st Floor Sickroom. The "guest in the clinic with
  the opening cutscene" report came from a fresh `savedata_party`: both instances were re-set up
  with the current harness (level 37 character, "Saves: check OK ... 1 character slot used").

### 6.4 Runs 17-20: placement written once, opening cutscene, saves [R]

* Placement (run 17): the guest now steps aside **in its own world before ringing** (pre-ring
  spacing: right / left / behind by slot, each try checked 30 ticks later: reached and same
  height ±1.5 m = walkable, else the next side, else back to the start spot); its sign, and so the
  join placement, is beside the host. A B2 arrival uses an exact-transform warp (forced placement
  +0x14F0/+0x1500/+0x1510/+0x1520) beside the host's arrival spot, written once before the load.
  Nothing moves a player after a load any more. Arrival watch (3 s, both instances): guest
  `(-191.85, -28.67, 66.94)` 2.20 m from the host on both screens, host not moved (the guest's
  height on the host's screen settles from -28.65 to -27.5 within 1 s: the remote chr landing).
* Opening cutscene on the guest: EMEVD **event 12410000, 2002[3]** in the host's world. The
  guest's overlay right after a plain bell join (host in the Dream) read 12410000=0 9401=1
  9402=1 12417810=0; after the B2 re-sent snapshot **everything was 0** (9401 too), before and
  after its load (run 18). Safety net in place: a client in the host's world never runs event
  12410000's 2002 (EMEVD skip predicate, `seamless_rules SetEmevdSkipPredicate`): logged
  `client: opening cutscene (event 12410000, 2002[3]) skipped in the host's world` and no remo.
  Overlay guard (save the host-world overlay at the travel event, restore every group the new
  snapshot left all-zero): restored 7 of 11 non-zero groups, but 9401's group came back
  non-zero with the bit cleared - the re-sent snapshot itself carries different data. Root cause
  of the poor re-sent snapshot: **the apply 0x13BECA0 ran in load mode 1** (a join applies it in
  the guest's own world, mode 0). Run 20: host at the re-send 12410000=1 9401=1 12417810=1; guest
  after its load all 0. **K11** (call site 0x17851CE `e8 cd 9a c3 ff` → 0x13BECA0): on a B2
  re-entry the guest's store goes back to mode 0 (SetLoadMode 0x13BB590) before the apply; its
  MoveMapStep sets mode 1 from the fresh overlay. Run 21: guest after the B2 load 12410000=1
  9401=1 9402=1 12417810=1 12411800=1 (2410=0: an area gate, never shared), no opening
  cutscene and no skip needed. The skip predicate stays as the net for a plain bell join whose
  snapshot was taken in another map (12410000=0 after a join from a host in the Dream).
* Recovery net: a player falling for 6 s in its own world is warped to its own last lamp
  (0x13CDF30, WorldTransitionState +0x1528).
* Saves: every B2 run now starts from the base save `E:\games\bbport\save_backup\savedata_20261010`
  copied into each instance's `user\savedata_party`. A player whose party character got a bad
  position restores it with the game closed: copy a folder from
  `user\save_backups\savedata_party\<date>-start` (written at every party start, newest 10 kept,
  plus every 10 min) over `user\savedata_party`.

### 6.5 Runs 22-25: host death, guest state, three players [R]

* Host death (run 22, fresh saves, host and guest in the Dream): K8 kept the member task, the
  guest dropped HostDead_1's send-home and followed the respawn travel, **the host's chr came back
  on the guest** (`re-join done`, host chr at its respawn spot).
* Guest state through a B2 load (run 24, guest HP set to 100 of 106, then host death): the load
  refilled the guest to 106; B2 now puts back the HP it had at the host's travel event
  (`guest: HP 106 after the load -> 100 as before the travel`). Items / buffs: a phantom's
  inventory lives in its own PlayerGameData and is not touched by the load (not measured).
* (Superseded by 6.13: the host is now revived in place.) Respawn in place (no guest reload) - not feasible with the game's code: every death goes
  through the funnel 0x13CDE30 → a stage request → MoveMapStep, and the world dtor 0x1547B40
  frees every chr set (WorldChrMan 0x1914F40); the game has no revive-in-place for the host
  (DS2's seamless mod found the same). A custom one (cancel the death load, revive the chr, warp
  to the lamp when it is in the current map, bloodstain / enemy reset by hand) would be a separate
  project. So host death = case (2): one loading screen, the guests stay in the session and keep
  their state.
* Three players (run 25, `BB_PARTY_B2_TEST_MEMBERS=2`): host lamp warp with two guests in the
  world; both guests stayed clients, loaded the host's map in host-world mode, re-created the
  host's chr; the host got two "arrived" and re-created both guests (76 s of run time).

### 6.6 Runs 26-28: B1 fallback, guest consumables [R]

* Forced failure (`BB_PARTY_B2_TEST_FAIL=resync`: the host sends no snapshot). Run 26: after 45 s
  the guest dropped its loading screen, fell back to B1 (lamp warp in its own world, left the
  room), but the re-summon failed: the network flow still held the old member list, so the new
  room's member event 0xE created no task for the host and the join was left 2 s later. Fix: once
  idle after a fallback, B2 runs the flow's own end-of-session cleanup 0x178A900 (clearing +0xC5,
  set by every MoveMapStep ctor, which defers it). Run 27: `flow cleanup 0x178A900 ran ... flow
  now 0`, the director's bells re-summoned the guest 20 s later (client again).
* Guest consumables across a host death (run 28): `goods 900:3 1000:15 200:1 205:1`, stamina
  100/100 identical before the travel and after the B2 load; HP refilled by the load and put back
  (100/106). Buffs (SpEffects) not measured.

### 6.7 Runs r1-r7 (bossk10): a real boss kill with the party, cooperator count, buffs [R]

Setup: base save `savedata_20261010` on both instances; the Cleric Beast "un-killed" with
`BB_PARTY_PROGRESS_TEST_CLEAR=12411700,12411701,12411702,12411703,12414700,12414701,12414223,
72410511,2411,9456,2410` (both saves; log `test: cleared ... 12411700:1->0 ... 2410:1->0`).
Note: in m24_01 the Cleric Beast is **12411700** (EMEVD name 教区長, entity **2410800**, lot
50000010, boss-count bit 9456, area bit 2411); **12411800 is Father Gascoigne** (2410810/811,
cutscene 24010010, lot 31000). First fight: host (chr type 0) inside region **2412805** (box
12 x 2 x 4 at (-136.5, -27.4, 52.8), MSB) -> 12411702 short-warps the boss (2412831) -> 12414702.
No fog on the first fight (the fog object 2411800 only for a rematch, 12414730); the guest's own
"enter boss room" 12414731 needs the fog action button (no OS input here), so the guest was
warped into the arena instead. Test chain (director): host `host_warp=2412952@8,
host_move=-136.5:-26.6:52.8@12%m24_01,host_kill=2410800@6`, guest
`guest_move=-134.6:-26.6:54.7@3%m24_01` (y -27.4 = the region floor: r5 fell through the
bridge, both players died falling).

Bugs found and fixed:
* **Host cooperator count 0 after every B2 load (r1, r2)** -> the host watchdog ended the party
  60 s after any host travel ("joining member made no progress"). The presence slot table was
  intact (`[1 h 10044001 kind 1 st 3 chr ok]`); the count 0x15BDC20 also asks chr vfunc +0x558 =
  SessionTypeDesc[GameData +0x5D8] +0x15 (counted in {1,5,7,19}). r3: the re-created guest chr
  had session type **0** (was 7) on the host, and the guest's **own** +0x5D8 read 0 after its
  host-world load (r6), so its InGameStart record carried 0. Fix `MemberSessionTypeTick`: the last
  counted session type per member task (and a guest's own player) is put back (`member task:
  session type 0 after the load / re-creation (not a cooperator) -> 7 as before`, both sides in r7;
  cooperators stay 1 on host and guest).
* **Host leaves after a boss (r3)**: 12 s after the kill the host went 3 -> leaving 7 and the
  guest's session ended although its own K10 fired. The host runs the same BlockClear2_1 / _3
  Lua states; **K10 now drops the leave for both roles** (r5-r7: `K10: boss-clear leave
  suppressed (host; party stays)`). A BeginLeaveSession entry trace (ReplacePrologue on 0x1ED07A0,
  `leave: BeginLeaveSession called from +0x...`) names any other path that ends a B2 session
  (r5: +0x1789F99 = flow update after the host's 60 s arrival timeout; +0x13CDEF6 = the funnel
  on a guest's death).
* **Guest buffs lost in a B2 load** (r1: 13 SpEffects at the travel -> 9 after; the test buffs
  3240 / 2200, 60 s, gone; 9006 / 9120 / 9121 -> 9110 are the session's own). SpEffect node: +0x00
  time left, +0x04 duration (f32), +0x40 id, +0x58 next. Fix `RestoreBuffs`: timed effects (not
  9000-9199) held at the host's travel and missing after the load are applied again with the time
  they had left (`buffs lost in the load restored: 3240(60.0 s) 2200(60.0 s)`;
  `BB_PARTY_B2_KEEP_BUFFS=0` off). Test aid `BB_PARTY_B2_TEST_SPEFFECT=id[,id]` (guest, at the
  travel event).

r7 (final, merged tree): fight started (12411702 [12] boss warp, 12414702), host_kill 6 s later:
both instances ran 12411700 [10] 2007[02] (PREY SLAUGHTERED banner, screenshot on the guest
with the host beside it) and [15] **2003[12]**; host `host boss kill ... 3 Insight`, K10 (host);
guest K10 (guest) + `send-home at +0x13859df dropped`, stayed **client (6) in the host's m24_01
for 2.5 min** until the lease ended the run; host cooperators 1 throughout. Guest rewards: Insight
6 -> 9 (C-phantom parity), echoes 0 -> 4000, C3 lot 50000010 (Sword Hunter Badge) already given at
the join from the host's history (`guest got 1 grants from the host, 0 new` at the kill: no
duplicate).

### 6.8 Runs r8-r15: wipes (guest and host dead), fog gate by pad [R]

Steps: host `host_warp=2412952@8,host_die@4#2`, guest `guest_die@S%m24_01` (die steps with
`%MAP` run S s after the last load, no re-join needed).

* r8: the guest's death went first and B1 took it (redirect to the host's lamp in its own world =
  a leave, then the bell re-summon). Now **held** (`HoldGuestDeath`, party_travel
  `SetGuestDeathHold`, `BB_PARTY_B2_DEATH_GRACE` 8 s): a host travel in flight or arriving within
  the grace takes the guest along (`held death goes with the host's travel (2.8 s after it); no
  redirect, no leave`); otherwise the usual redirect runs (`GuestDeathRedirectNow`).
* r8 then waited 45 s for a snapshot and fell back to B1: the host was still in
  `HostPhase::Resynced` from the warp's re-join (it stays there up to 60 s) when it died, so its
  second load was never noticed and nothing was re-sent - also the cause of r5's 30 s wait. Fix:
  a load in `Resynced` re-enters `Loading` (`host: another own load before the last re-join
  settled`); r11-r15 re-sent after the death load.
* r11 then reproduced the **Hunter's Dream placement**: the dead guest's load is a death load and
  goes to its own respawn records (GSM +0x1528 / +0x1538 / +0x153C: the Dream) whatever the forced
  transform says - it loaded m21_00 in host-world mode, 142 m from the host on both screens
  (`host watch: ... guest (-9.20, -6.77, -19.40) 142.72 m`). Fix: for a dead follow the host's
  records from the travel intent are written before the load and the own ones put back after the
  arrival (`respawn records = the host's (last lamp 0x18b50002016b1 -> 0x18b2b0024d198, mode 0 ->
  1, respawn -1 -> 0x18b2b0024d198)`); a death load lands on the lamp spot itself (0.00 m from the
  host in r13/r14), so the guest is then warped beside the host (chr warp +0x5F8).
  r13 (guest dies first) and r14 (host first: `held (the host's travel is in flight)`): the guest
  arrives in m24_01 in the host's world, client throughout, host cooperators back to 1.
* r12 (both fall through the bridge): the host's fall recovery lamp-warps it (a travel); the
  falling guest follows before it dies and arrives 2.2 m beside the host. Fall recovery of a party
  guest now targets the host's last lamp (not its own, often the Dream) and leaves a pending
  stage request alone.
* r10, fog gate by in-process pad: `guest_move=-155.15:-26.6:34.37:-2.35@4,guest_press=0x4000@1.5`
  (fog 2411800 at (-154.44, -27.02, 35.07); yaw -2.35 = the fog object's own facing; Cross via
  `runtime_pad_director_press`, no OS input): EMEVD 12414731 ran [8] (turn + walk-through anim
  101130) and [17] set **12414701** (the guest entered the boss room) after the host's 12414700.
  The earlier try (yaw 0.79, before 12414700 was set) did nothing.

### 6.9 Runs r16-r17: a guest dying alone respawns in the host's world [R]

ER-Seamless-style: a guest that dies while the host lives stays a client and respawns at the
host's last lamp in the host's world. After the held death's grace (6.8) without a host travel,
`SoloRespawn` (guest, `BB_PARTY_B2_SOLO_RESPAWN=0`: off) builds a lamp intent for the host's last
lamp (ReturnPointParam row via `0x1F26B40(out, id)`, record {+4, +8} as 0x13CDF30 writes it), runs
the B2 re-join as a dead follow (flow 1, host's records into GSM +0x1528 / +0x1538 / +0x153C, death
load in host-world mode, own records back on arrival, no beside-the-host warp) and sends
`{"op":"respawn"}`. The host re-sends the snapshot and its spot, and removes the dead member chr
the way the task's own STEP_DeleteWait 0x1E50A70 does (slot release `0x15BC080`, WorldChrMan
remove `0x19186E0`), keeping the task (K8 already keeps it on any death while hosting); on
"arrived" the task is rewound to STEP_Create and the chr is built again from the member's
GameData row.

* r16 (without the removal): the guest respawned at the lamp in m24_01, still a client, but the
  host kept its dead chr (`hp 1`, presence state 5) at the death spot and rewound 0 tasks.
* r17: `host: dead member chr ... (hp 1) removed; its task is kept`, guest `respawned at the host's
  lamp in the host's world (stage 0x18010000 pos (-124.8, -27.0, 64.3) hp 106), still a client`,
  host `task mode 1 rewound to STEP_Create`, cooperators 0 -> 1 14 s after the death; screenshot:
  the host sees "Co-operator Hunter1" alive 0.8 m away at the lamp. No leave, no bell.

Death rules on a lone respawn: full HP; buffs end (not restored, unlike a plain B2 load);
Insight and consumables are kept; echoes go into a bloodstain since 6.13 (design decision 3; before,
they were kept as a vanilla phantom's); the host's world is not reset. Fallback: no snapshot within 45 s -> the B1 fallback as before (redirect, leave, bell).

### 6.10 Runs r18-r25: lone-death variants [R]

* **Host's last lamp in another map** (r24-r25; fixture `host_lastlamp=2402950`: the host's
  +0x1528 names a Cathedral Ward lamp while the party stands in m24_01 - the state after walking
  into the next area. A stage warp 0x132E010 to m24_00 was tried first: points 2402950 /
  2401950 are no warp regions, the host fell and recovered, r18/r21). `SoloRespawn` sees the lamp's
  map (`ids::EntityMap`) differ from the current one and picks a lamp of the current map:
  ReturnPointParam ids AAB295k, lit = common event 7000's 2009[5] flag `1AAB7800 + 20k` (m24_01:
  2410950.. -> 12417800 / 820 / 840 / 860, from the EMEVD args). In the host-world overlay these
  flags read 0 for every lamp (r25: `2412950:- 2412951:- 2412952:- ...`), so the first lamp of the
  map is used (any ReturnPointParam row of the map is a valid respawn spot). r24 (before that
  fallback) found none and took the old redirect (leave, bell; re-summoned 60 s later). r25: `the
  host's last lamp 2402950 is not in m24_01_00_00; lit lamp here: 2412950` -> respawned in m24_01 at
  (-199.7, -50.8, 179.4), still a client; the host removed the dead chr and re-created it.
* **Three players** (r19, r22): r19 removed **both** guests' chrs on the host (remote chrs' HP read
  1 on both). Now the guest sends where it died (`{"op":"respawn","pos":[...]}`) and the host
  removes only the member chr within 6 m of it. r22: one removal, host cooperators 2 -> 1 -> 2, the
  other guest untouched (its own view of the dying guest re-created 0.5 s later).
* **45 s fallback** (r20, r23; host `BB_PARTY_B2_TEST_FAIL=resync`, both in the Dream): r20 stayed
  dead in the host's world - the B1 replay of the synthetic intent was dropped ("not newer than
  the last"). Fix: a failed lone-death respawn runs `GuestDeathRedirectNow` (r23: `FALLBACK (no host
  snapshot within 45 s): the lone death's respawn failed; the usual death redirect runs` -> leave,
  own-world respawn at the host's last lamp, bell). **Open**: in r23 the bell re-summon did not
  complete in the 2 min left (host sign polled, signaling resolved, no join; the host room timed
  out twice via +0x1789F99) - in r24 the same redirect re-summoned in 60 s, so the test-fail mode or
  the Dream may matter; not resolved.
* **Echoes** (r21, r25; `echoes=5000` director test aid on the guest): `echoes 5000` at the death and
  after the respawn load; nothing dropped, as decided in 6.9.

### 6.11 Runs r26-r32: re-summon after a redirect / crash; lit lamps [R]

* **r23's failed re-summon did not reproduce** in 6 later attempts: r28 and r31 (the same scenario:
  host `BB_PARTY_B2_TEST_FAIL=resync`, lone death in the Dream, 45 s fallback, redirect), r26 and r29
  (plain redirect, `BB_PARTY_B2_SOLO_RESPAWN=0`), r32 (three redirects in a row in one run), r30
  (guest crash at 60 s, `--crash-at`). Every re-join completed: bell -> client 4.4-6.5 s after the
  guest's own world was ready, ~19.5 s after the redirect (13 s of that is the guest's own load; the
  host's lone room times out via OnRoomTimeOut +0x1789F99 and is re-made 7 s later meanwhile).
  r32: three consecutive redirect -> re-join cycles, 4.5 s ring -> client each.
* What r23 showed: the guest got the host's `guest_invite` but its game never called
  `sceNpMatching2JoinRoom`; the host's game sent no vport-40 summon handshake (no
  ActivateConnection at the summon, its conn 2 to the guest was made right after the guest left).
  A re-announce of ESTABLISHED on a re-activated signaling connection was tried and dropped: r28 /
  r31 joined without any re-activation, so it is not the cause. One stale state found and fixed:
  the host's `g_rejoin` stayed set across later rooms when its room ended during a member's
  respawn re-join (r23: `rejoin 1` at both failed summons) - now cleared with the session
  (`host: the session ended during a re-join; re-join state cleared`, r31). K5 (the only host-side
  user) suppresses OnMatchingCheck leaves while it is set.
* **Lit lamps** (r27): the lamp-lit flag is common event 7100's `7AAB0200 + k` for lamp object
  AAB195k (m24_01: 72410200..203, EMEVD args; a T7 group the host-world mirror copies). r27, host's
  last lamp set to another map: `lamps of m24_01_00_00: 2412950:- 2412951:lit` -> respawned at the
  lit Central Yharnam lamp (-193.4, -28.6, 68.5); unlit 2412950 (1st Floor Sickroom) skipped.

### 6.12 A guest dying after (or during) a boss fight stayed dead (2026-10-11, overlay)

The "guest_die after the boss did nothing" test oddity was a real bug, not the test step:

* The die step's own gates were open (`ForceDead gates: GSM +0x176F 0, chr ... 0`), entity 10000
  was our chr, and ForceDead set HP (data module +0xF8; +0x128 is not HP) to 0 at once:
  `HP 503 -> 0 right after`. A fall into the void after the kill (no die step) did the same:
  `Party balance: death 1`, no respawn for 100+ s.
* Yet no `guest_died` funnel call came: the guest lay dead in the host's world. The boss-clear Lua
  state BlockClear2_3 (0x1385930) ends in the send-home tail jump K10 drops, so the phantom's
  state machine never leaves it and never runs PartyGhostDeath (0x13824C0). One run also had a
  death *during* the Cleric Beast fight (before the kill) that went unprocessed for 17 s.
* Fix (`DeadAfterClearTick`, party_b2.cpp): guest, HP 0 for 12 s after a K10 on this load (15 s
  otherwise) with no death funnel or B2 death hold -> `GuestDeathRedirectNow()`, the normal lone
  death path (respawn at the host's last lamp, the bell brings the guest back).
  Live (`guest dies after boss: watchdog 2`): kill +0, guest ForceDead +23 s, `dead for 12 s and the
  game's death state never ran (after a boss clear ...)`, `guest_died ... to the host's last lamp
  2412952`, loading screen 5 s later.
* The K10 invincibility guess (Lua EnableInvincible, chr +0x1E1 bit 14) was wrong: `no invincibility
  left 10 s after the clear` in every run; the clear stays as a harmless check.

### 6.13 C2: the host dies alone - revived at its lamp, the guests stay (hostdeath, 2026-10-11) [R]

ERSC rule (user decisions, ERSC_PARITY.md): a dead player respawns at its own last lamp and the
others play on; nobody else loads. Until now every host death went through the funnel and moved
the whole party (6.5). Now:

**The death.** SoloPlayDeath_2 `0x1381DE0` (Lua state after YOU DIED) selects the last lamp
(`0x1332BC0`), calls the funnel `0x13CDE30` at **0x1381EDE**, then records the bloodstain
`0x13D2910(SprjEventMan +0x80, player, 1)` (GameDataMan +0x30 record: position, map, echoes;
PlayerGameData +0x94 = 0; GameDataMan +0x28 = 1) [C]. party_travel's funnel hook now asks an
interceptor first (`SetHostDeathIntercept`); B2's `HostDeathIntercept` takes the call when a member
task exists, not every member is down and the host's last lamp has a known spawn spot in the loaded
map: no stage request, no capture, no TRAVEL broadcast. The rest of SoloPlayDeath_2 runs as usual,
so the echoes go into a live bloodstain at the death spot ("Acquire Blood Echoes" prompt on the
revived host, hd10) [R].

**The lamp's spot.** After every own load that put the player on its lamp (GSM +0x1538 respawn mode
1/2 with a +0x153C record, or +0x1524 from the selector) the player's transform is kept with the
lamp id (GSM +0x1528 after the load) and map: `lamp spot: lamp 2412951 in m24_01_00_00 at
(-193.40, -28.65, 68.50) yaw 0.79`. The revive uses it when +0x1528 still names that lamp and the
map's area/block match the player's stage (PlayerIns +0x3F8).

**The revive** (`ReviveHost`, main thread, 1 s after the taken funnel, `BB_PARTY_B2_HOST_REVIVE_DELAY`).
What a death leaves, from a live dword diff of the host's chr alive vs dead (`BB_PARTY_B2_REVIVE_DIFF=1`, hd5)
and the decompiles: HP module (+0x3B0 -> +0x20) **+0xF8 HP = 0** (+0xFC max; +0x128 is not the HP);
PlayerIns +0x1E1 bit 0x10000 (dying, cleared again by the game before the funnel), +0x208 draw factor
1 -> 0; ChrCtrl (+0x58) +0x114 0 -> 0x10001; the action request block ChrCtrl +0x370 and module +8
(death request, kind +0x1B4/+0x1B8 = 2/9); the behavior in state BeDead (c0000.hks), the model
dissolved. Steps (`BB_PARTY_B2_REVIVE_RECIPE`, default `recover,flags,ctrl,fade,reset,idle,warp,gsm`):

| Step | What | Source |
|---|---|---|
| recover | `0x1900710(player)` (RevivePlayer / OnSpEffectRevive: body form, SpEffects, stamina) | Lua RevivePlayer 0x1332B20 |
| HP | HP module +0xF8 = +0xFC | always |
| flags | +0x1E1 &= ~(0x10000 \| 0x20000 \| 0x2000 \| 0x40000) (dying, death drops done) | 0x18BD9D0, 0x18C4F90, 0x18C5070 |
| ctrl | ChrCtrl +0x114 &= ~0x10001, [ChrCtrl +0x38] +0x48 &= ~1 | ChrIns_OnDeath 0x18C49F0 |
| fade | +0x20C alpha 0 -> 1 at 1.5/s, +0x1E5 &= ~4 | ChrFadeIn 0x1339010 |
| reset | `0x18BCF30(player, 1)` + PlayerIns +0x1E4 \|= 2 (transform re-applied, ChrCtrl vfunc +0x38, behavior graph reset) | Lua ChrResetRequest 0x13370B0 |
| idle | behavior event L"Idle_wild" (0x4992C40) via `0x1A19F60([+0x3B0]+0x30, name)` when its +0x384 is set: out of BeDead | Lua ChrResetAnimation 0x13370F0 |
| warp | chr warp vfunc +0x5F8 to the lamp spot | as the B2 spacing |
| gsm | GSM +0x1522 / +0x1523 (local player died / fell) = 0 | ChrIns_OnDeath |

Live (hd2-hd12, 2 instances, Central Yharnam lamp 2412951): with HP written to +0x128 (wrong
field) the chr stayed dead; with HP but without `reset` + `idle` it stood at the lamp invisible on
both screens and did not act; with `reset` + `idle` it is visible on the host and the guest (hd10
r2 screenshots: the host beside its bloodstain, "Acquire Blood Echoes"); with `ctrl` too it acts: a
Circle tap (in-process pad) backsteps it 2.3 m, (-193.4, 68.5) -> (-191.8, 70.1) (hd11, hd12, both
revivals). With the ChrCtrl death bits clear, a later stage request stayed armed and no load began
(hd11: the next lamp warp never loaded, the guest fell back to B1 after 45 s; the same with the full
alive copy of ChrCtrl +0x370 / module +8 written back, hd6 / hd7). So the bits are set again as soon
as a stage request is armed after a revive (`a stage request after the revive; ChrCtrl death bits
set again for the load`); the load re-creates the chr anyway (hd12: both post-revive lamp warps
loaded, the guest followed with B2 each time).

**The guests.** `{"op":"host_revived"}` to every member: each guest removes its copy of the host's
chr (it died with the host: +0x1E1 0x10000, invisible) as the host does for a guest's lone respawn
(slot release `0x15BC080`, WorldChrMan remove `0x19186E0`) and rewinds the host's task to
STEP_Create: the chr comes back at the lamp from the task (`guest (host revived): task mode 2
(step 6) rewound to STEP_Create`). The guest's HostDead_1 send-home is still vetoed (K9). No load,
no loading screen, still a client (role 6, flow 4); the host keeps its member tasks (K8).

**Fallbacks / other cases.**
* Lamp in another map (design decision 1): the host loads it ALONE and the guests stay - the
  captured TravelIntent carries split travel's `alone` (`HostTravelAloneNext`, docs/party/split_travel.md
  3): hd18, host's last lamp set to the Dream (2102961) while the party stood in m24_01: `host death:
  last lamp 2102961 is not in the loaded map (stage 0x18010000): the host loads it alone (the guests
  stay)`, the host `arrived alone in m21_00_00_00`, the guest (no load) `the host travels alone ...`,
  `remote chr ... removed (the host travelled alone); its task is kept`. `BB_PARTY_HOST_DEATH_ALONE=0`:
  the old party move.
* Spot of a lamp never arrived at in this session: the MSB revive point from the game data
  (`ids::ReturnSpot`, split_travel.md 3a; yaw in degrees) when the lamp's map is the loaded one.
* Every member down too (each guest sends `{"op":"dead"}` when its death is held; cleared on its
  "arrived"): a wipe, the vanilla funnel and the party respawns together at the host's lamp (6.8).
  Also while the host waits for its revive: `every member went down while the host was ... the
  stock funnel runs` (`HostDeathFunnelNow`: capture + broadcast + stock funnel).
* A load starting while the host is down (a guest's lamp request, a rest) ends the wait: the load
  re-creates everyone.

**Boss fights (C5, limbo).** A boss fight = a boss health bar is up: EMEVD 2003[11] (handler
`0x17C0420` case 0xB) writes the HUD object `*0x5562878 +0x1658 + slot*0x14` {name id, chr handle} for
slots 0..2 and resets them to -1; counted while the handle resolves to a living chr (HP +0xF8 > 0).
* Host dies in a boss fight: the death is taken but not revived (`host death in a boss fight (boss bar
  0: ...): limbo`); it lies dead (YOU DIED, no load) until the bar goes (boss killed: revived at its
  lamp as above, or the party move if the lamp is elsewhere), every guest is down (a wipe: the stock
  funnel, everyone to the host's lamp) or a load starts (someone rests).
* A guest dies in a boss fight: its held death (6.8) is kept past the grace while the bar is up
  (`guest: dead in a boss fight (...): limbo until the boss is gone, a wipe or a rest`), then the lone
  respawn (6.9) at the host's last lamp; a host travel (wipe, rest) takes it along as before.
* Shared boss lamp (design decision, ERSC_PARITY @ 7552b38): every respawn after a boss fight goes to
  the same lamp - the host's last lamp (a guest's lone respawn already uses the host's last lamp, or
  a lit lamp of the loaded map, 6.10; the wipe and the host use the host's lamp).
* Spectating: none; the dead player's camera stays on its body.

**Echoes (design decision 3).** The host: vanilla (bloodstain via SoloPlayDeath_2, live and
pickable at once, hd10). A guest (`BB_PARTY_GUEST_BLOODSTAIN`, default on): at its held death the
same record `0x13D2910` runs with GSM +0x1592 = 1 and +0x1770 = 1 for the call (else it takes the
forced pre-summon spot), both put back: `guest: bloodstain recorded: echoes 5000 -> 0, stain 5000
echoes at (...) map 0x18010000`. The stain is in the guest's own records, so it shows in the guest's
game (in the host's world after its respawn load, and in its own world in that map).

**Guest deaths: no load when the lamp is in the loaded map** (Bloodborne rule; user, hd24). After the
held death's grace (limbo aside) the guest picks its lamp - its OWN last lamp outside a boss fight,
the host's last lamp after one (shared boss lamp) - and when that lamp is in the loaded map
(`ids::EntityMap` area/block = the player's stage, spot from `ids::ReturnSpot`) it is revived exactly
like the host (`ReviveLocal`; the `recover` step is host-only: 0x1900710 writes PlayerIns +0x78 = 0,
which would turn the phantom into a body) and put on the spot; `{"op":"member_revived","pos":...}`
(where it died) makes the host and the other guests remove the dead member chr nearest to that spot
and rewind its task (the chr is built again at the member's new spot). Own lamp in another map:
split travel's `RespawnAloneAtOwnLamp` (a load for this guest only); when it refuses (a death
outside the host's area, hd19) the host's lamp, in place if it is in the loaded map, else the old
lone respawn (6.9). A member standing on the spot: the revived player is put 0.5 m beside it
(`BesideHost`). `BB_PARTY_GUEST_REVIVE=0`: off.
Live hd25 / hd26: `guest: died alone: revived at its own last lamp 2412951 in the loaded map
(-193.4, -28.6, 68.5), no load`, `guest revive #1: after: hp 431 ... type 1` (still a phantom), host
`slot 1 was revived in place (no load)`, `member revived in place: its dead chr ... removed (0.0 m
from where it died); re-created`; no loading screen on either game after the first summon; the
guest's screen shows both hunters side by side at the lamp. The overlay watchdog of 6.12
(`DeadAfterClearTick`) now also respects a hold that was already consumed by a respawn (hd22: it
redirected a guest 8 s into its lone respawn and ended the session).

**Live runs** (2026-10-11, two / three loopback instances, base save savedata_20261010):
* hd10-hd12, Central Yharnam 2412951, two revivals per run: see the revive table above; hd12 both
  post-revive lamp warps loaded and the guest followed (B2) each time.
* hd17, the Hunter's Dream 2102961: `host death taken`, `host revive #1: after: hp 719 ... at lamp
  2102961`, then the host's lamp warp to Central Yharnam re-armed and loaded, the guest followed.
* hd16: host revived, then the guest died alone: `guest: bloodstain recorded: echoes 5000 -> 0`,
  lone respawn at the host's lamp, the host `slot 1 died alone and respawns in this world`.
* hd13 (Cleric Beast un-killed with the 6.7 fixture): host died in the fight -> limbo; host_kill of
  the boss -> `the boss fight is over (...); respawn at the lamp here` -> revived; K10 kept the party.
* hd14c: guest died while the boss bar was up -> `limbo`; then the host died -> `every member is down
  too (1 of 1): a wipe` -> stock funnel, TRAVEL, the guest's held death went with it. That run's
  host death load took > 60 s on the loaded machine, so the guest's 45 s snapshot wait fell back to
  B1 (redirect + bell): the wait does not know the host is still loading (open; not C2-specific).
* hd15c: a guest respawning when the host died counted as down -> wipe path (by design).
* Movement: a Circle tap (in-process pad) 3 s after a revive backstepped the host 2.3 m (hd11, hd12);
  the camera follows the revived chr; the HUD, the bloodstain prompt and the next loads work.

Placement (coordinator check, upstream 12490f5 BesideHost 0.5 m): in the Central Yharnam
screenshots of these runs the guest stands 0.7-2.4 m from the host on open cobblestones beside the
lamp (not in a wall); the host is not visibly shoved. The revived host itself goes to the lamp's
spawn spot (it is the lamp's player, so no BesideHost).

**Remote copies after a death (user report "health bar floating, no model").** `remote chrs:` lines
(`RemoteChrRenderWatch`) log every remote player chr's position, distance, draw factor (PlayerIns
+0x208: 0 -> 1 when built, 1 -> 0 in the death dissolve), alpha and flags. hd28: the guest's copy on
the host dissolved to `draw 0.00` after its death and was replaced by a new copy `0.5 m draw 1.00`
after its in-place revive (`member revived in place: its dead chr ... removed ...; re-created`); the
guest's view of the revived host likewise (screenshots hd25-hd30: both hunters side by side on the
guest's screen). The remaining "plate at the summon spot, no body" on the HOST's screen exists
from the bell join on (hd27/hd28 `dead_1s` / `join` shots, before any death) - the B2 arrival / remote
chr path splittravel is fixing (split_travel.md 3b has these findings). A chr warp of a remote
copy does not move its plate (tried, removed).

**Test setup.** The director's `host_warp=ID` now only sets the last-lamp record when the host already
stands within 5 m of that lamp (`host_warp 2412951 skipped: already at that lamp (last-lamp record
set, no load)`, hd30): no setup loading screen that looks like the join's.

**Guest Continue drift (open).** A guest killed while summoned continued at its host-world spot
(m24_01 beside the host) instead of its own world's (Dream) spot (hd31 / hd32, fresh saves). The B2
placement writes to GSM +0x14C4/+0x14D0/+0x14E0 (overlapping the return spot +0x14A0..) are now
skipped while +0x1521 is clear (they were unused then), but the drift stayed: the game itself
re-wrote the return spot to the host-world spot during the B2 load, and the save's position source
is not identified yet. Next: find the save serializer's map / position source while +0x1592 == 0
and pin it to the pre-summon spot.
`BB_PARTY_POS_SCAN=1` (diagnostics) found the live copies of the player's position in GSM: +0x15B0
and +0x15F0 follow the chr in the own world AND in the host's world (a likely save source), +0x14A0
is the return spot (pre-summon), +0x14D0 the summoned placement, +0x1500 the forced placement (pos1
run). Pinning +0x15B0/+0x15F0 (and their map ids) to the return spot while a client is the next step;
not done for 0.5.2.

**2026-10-12 follow-ups.**
* Continue drift: `PinOwnSpotTick` (`BB_PARTY_GUEST_PIN_SAVE_POS=1`, default OFF) pins the pre-summon
  spot into GSM +0x14A0 / +0x15B0 / +0x15F0 and the forced placement +0x14F0 / +0x1500 / +0x1510
  while a guest is in the host's world. Not a fix: on fresh saves (pin3, pin4) the Continue still
  started at the B2 arrival spot beside the host (-193.05, -28.65, 68.15) - exactly the B2 load's
  forced transform - so the save is taken at the arrival load itself, before the pin runs. Next: keep
  the B2 load from leaving the forced transform as the save's spawn (e.g. place the guest with a
  chr warp after a plain host-world load instead of the forced transform), or find the autosave at
  load end. pos4 also showed a confound: an enemy killed a low-HP guest in Central Yharnam, its death
  state never ran, the overlay watchdog redirected it home (session left) and the bell re-joined it
  from Central Yharnam, so its own world legitimately moved there.
* Guest bloodstain pickup (stain1-stain3): the guest's stain is recorded in its own records
  (`bloodstain recorded: echoes 5000 -> 0, stain 5000 echoes at (-192.8, -28.6, 66.1)`) but no
  stain object appears in the host's world: standing on the spot and pressing Cross gave no
  "Acquire Blood Echoes" prompt and echoes stayed 0 (screenshots). It can only be picked up in the
  guest's own world (not verified). Open.
* `BB_PARTY_GUEST_DEATH_ALONE=1` (default OFF, not run live): a guest whose own lamp is in another
  map is revived where it lies and then travels alone (split travel `TravelAlone`, Lamp) to it.

**2026-10-12 (2): Continue drift fixed, guest stain recoverable in its own world.**
* The guest's own save records the FORCED placement (GSM +0x14F0 map, +0x1500 position, +0x1510
  rotation): the join leaves the pre-summon spot there, and the B2 loads replaced it with their exact
  transform beside the host - a later Continue started there (pin3 / pin4). Now (`BB_PARTY_B2_ARRIVAL_WARP`,
  default on) a guest's B2 load is the host's own travel (a lamp warp etc.; the stage comes from
  +0x14C4 = the host's stage) without a forced transform, and the guest is put beside the host with a
  chr warp once it arrived: `guest: arrived by the host's own travel; placed beside the host
  (-193.05, -28.65, 68.15); forced placement (own save spot) map 0x15000000 (-11.1, -7.0, -20.6)`.
  Fresh saves: B2 load Dream -> Central Yharnam, then Continue (arr1b) and again after a second B2 load
  (arr1d): `map - -> m21_00_00_00`, the guest at its pre-summon spot in its own Dream (screenshot);
  arrival screenshot: both hunters side by side in Central Yharnam. The dead follow (wipe) and split
  travel's alone loads still use the forced transform (not changed). `PinOwnSpotTick`
  (`BB_PARTY_GUEST_PIN_SAVE_POS`, off) is no longer needed.
* The guest's bloodstain (its own record, 6.13) is recoverable in its OWN world, ERSC-like ("echoes
  drop into a recoverable bloodstain"): bs2 the guest died in the host's Dream (stain 5000 echoes at
  (-10.2, -6.8, -19.7) map 0x15000000); bs4 its next Continue in its own Dream: `own world: bloodstain
  flag 1, record 5000 echoes at (-10.2, -6.8, -19.7)`, and the screenshot shows the stain and
  "Acquire Blood Echoes". Not shown in the host's world (stain1-3). Design: a guest recovers its echoes
  in its own world, at the spot where it died (same map coordinates).

**2026-10-12 (3): no forced transform in the dead follow and the solo re-join either; wipes.**
* The dead follow (a wipe / lone respawn load) and split travel's solo re-join into the host's area
  no longer write a forced transform (the death load uses the respawn records anyway; the solo re-join
  writes the stage into +0x14C4 whatever +0x1521 says - the summoned-placement gate of 6.13 had
  skipped it - and is put on the lamp spot by a chr warp on arrival). Split travel's alone load into
  another area still uses its forced transform (it needs it to resolve the map): a Continue after one
  can still start there (open).
* Op clash fixed: split travel's dead-enemies event is also `"op":"dead"`; the guest's "I am down"
  note is now `member_down` (before, the host never counted a dead guest, so a host death after a
  guest's became a revive instead of a wipe).
* A dead guest whose host's death load goes in-world (split travel's same-map follow) is now revived
  beside the host (it stayed dead before).
* Live wipe5a/5b (fresh saves): guest died, then the host: `every member is down too (1 of 1): a
  wipe`, `held death goes with the host's travel`, `dead at the host's in-world respawn (a wipe):
  revived beside the host, no load`; screenshots: both hunters side by side on both screens; the
  following Continue: `map - -> m21_00_00_00` (the guest's pre-summon Dream).

**2026-10-12 (4): C5 spectating; C7 note.**
* Spectate in a boss-fight limbo (`BB_PARTY_SPECTATE`, default on): the dead player's camera follows
  its own chr, so that (dissolved, draw 0) chr is carried 3 m behind a living party member with the
  chr warp every 0.2 s; peers get `{"op":"spectating"}` and disable the dead copy (Lua SetDisable
  0x18C6390(chr, 1): no body, no name plate - removing it instead let its task build it again and its
  plate followed the spectator, spec3). The revive / respawn ends it (the copy is replaced as usual;
  `member_revived` now rewinds the task also when the copy is gone).
  Live spec3/spec4 (Cleric Beast): host died in the fight -> `spectating a living party member`; the
  host's screen shows the guest 3 m ahead fighting the boss (boss bar up); the guest's screen shows
  no host body or plate; boss killed -> `the boss fight is over ... respawn at the lamp here`, host
  revived at lamp 2412952, `spectating ended`.
  Known: the spectator chr moves through the arena (region events see it; nothing seen to trigger).
* 2026-10-13 (c5): member switch: Cross (`runtime_pad_cross_presses`, a dead chr ignores it) moves
  to the next living member in task order; a followed member that dies or leaves is replaced by the
  next one (`SpectateTarget`, party_spectate.h, unit party-spectate-test); `spectating: following slot N`
  logs each change. Live c5spec1/c5spec2: guest died in the Cleric Beast fight, host alive: limbo,
  spectate (host: dead copy disabled, 3.0 m behind it), Cross logged, boss killed -> guest revived at
  the host's lamp 2412952 in the loaded map (no load), host re-created its chr at the lamp.
* C7 (c7a, inconclusive): the host's lamp warp while the boss fight is up became split travel's
  in-world party move (no load): the guest was moved beside the host, but an in-world move does not
  reset the boss fight or the enemies, which design decision 4 wants for a rest. A rest during a boss
  fight should be a real load (or reset the fight); that is split travel's party-move rule.
* Join oddity seen while setting up (spec1/spec2, not hostdeath code): a guest whose own world is the
  Dream joining a host in Central Yharnam landed in m21_00 in the host's world - the gather does not
  run while the guest does not know the host's last lamp yet (GatherLampFor(GuestDeathRedirectLamp())).

**2026-10-12 (5): C7 rest during a boss fight; bell join into the host's map.**
* A lamp use while a boss bar is up is a real rest: split travel's same-map moves
  (`WarpToLampNoLoad`, `HostPartyWarpNoLoad`) and the guest's in-world follow of a host load step
  aside during a boss fight, so the host loads (the fight and the enemies reset) and the guests
  follow with their own B2 load. Live c7c (Cleric Beast): guest in the arena with the boss bar up
  (screenshot), host lamp 2412952 -> `the party moves with a real load (fight and enemies reset)`,
  guest `host travels ... arrived by the host's own travel; placed beside the host`, `A11: the boss
  fight / cutscene is over`; screenshot after: both hunters at the Great Bridge lamp, no boss bar.
* Bell join (party_director): a guest no longer rings while the host's roster map is unknown (the
  host still loading; spec1/spec2 rang then and were summoned at the guest's own Dream spot into a
  host in Central Yharnam); the gather picks the lamp of the host's map nearest to the host's VITALS
  position when the host's last lamp is not known yet (not the map's first lamp, the clinic); and no
  ring follows a gather until its load (join4: `gather: own world m21_00_00_00, host in m24_01_00_00;
  to the host's lamp 2412951 before ringing`, then the ring in m24_01).
  Open: (a) the gather's own-world lamp warp to 2412951 landed the guest at (-195.6, -40.5, 220.3),
  far from that lamp (the lamp not lit in the guest's own world?), so the summon placed it 150 m from
  the host; (b) the title-join path (another agent's) still joins at the guest's own map first, then
  leaves and gathers, and leaves the forced placement unset (`map 0xffffffff`), so a Continue after it
  has no own save spot from the join.

**2026-10-12 (6): bell join arrival beside the host.** After a bell join's arrival (role becomes
client, flow 4, the host's chr in this world), a guest more than 4 m from the host
(`BB_PARTY_JOIN_BESIDE_MIN`, test aid) is put beside it with the chr warp (`BesideHost`, like a B2
arrival); `BB_PARTY_JOIN_BESIDE=0`: off. Live join7b/join8 (guest's own world in the Dream, host in
Central Yharnam): `gather: own world m21_00_00_00, host in m24_01_00_00; to the host's lamp 2412951`,
the bell join landed 0.5 m from the host (`join: arrived 0.5 m from the host; left there`); forced
with min 0.1 m (join8): `put beside it (-193.0, -28.6, 68.1)`; screenshots: side by side on both
screens, the host sees the guest's body and plate. The 150 m case of join4 did not reproduce.

**2026-10-14 (pt1deaths): playtest 1 - loading screens where the no-load paths glitched.**
User direction after the first Internet playtest (docs/party/playtest1_triage.md, batch 1): where a
vanilla loading screen fixes a glitch, the loading screen is the default and the seamless path an
opt-in. The decisions are pure functions in `party_death_rules.h` (unit party-death-rules-test).
* Host death (bugs 6, 7, 12, 13): `BB_PARTY_B2_HOST_REVIVE` is now opt-in (`=1`). By default
  `HostDeathIntercept` lets the funnel run: one respawn load for the whole party at the host's lamp
  (the host's TRAVEL host_death; the guests' HostDead_1 send-home is dropped by K9 and this travel
  follows, which ends their half-done HostDead state: pickups and item boxes work again).
  `BB_PARTY_INWORLD` (the guest's in-world follow of a host travel into its map) is opt-in too, so
  the guest loads as well (otherwise a same-map host death respawn moved it in-world). A death in a
  boss fight still takes the funnel for the limbo (spectate); when the boss is gone the stock
  funnel runs (the party load) unless the revive is on.
* Wipe race (bug 18): the dead are recounted every frame while the host is down (`PlanDownTick`), and
  a guest's chr at HP 0 sends `member_down` at once (`GuestDownNoticeTick`) instead of only when its
  death state runs seconds later.
* Guest rest (bug 3): a guest that answers NO at a lamp / headstone travels alone as before and sends
  a `rest` lamp request; the host (`HostRestLoadTick`, gate `PlanRestLoad`: in the world, alive, no
  boss fight, not in the Dream, nothing pending, 20 s apart) runs `HostTravelTransformNow`: its own
  world loaded at its own transform (the forced-placement sequence), broadcast as a Transform travel
  so the other guests in its world follow. Live: census 326 alive / 4 dead -> 327 / 3.
  Open: the host then waits up to 60 s (B2 Resynced) for an arrival from a guest that is elsewhere.
* Guest same-map lamp (bug 10): no question; split travel queues the guest's own load
  (`GuestSoloLampLoad` -> `TravelAlone`: a re-join into the host's area) and the host ignores a guest
  request for a lamp of its loaded map. `BB_PARTY_GUEST_LAMP_INWORLD=1`: the old chr warp.
* The split question closes on a load, the player's death or after 30 s (Circle cancels the dialog).
* `OwnStainWatchTick` logs every change of the own bloodstain record (recorded / taken / replaced).
* Live (one session + a re-run, 2 instances, Central Yharnam): host death -> `the respawn load for the
  whole party`, guest loaded and arrived beside the host, enemies reset on both (the killed 2410015
  alive again), screenshots: both at the lamp, normal geometry, the host's stain prompt; guest rest
  NO -> host rest load, enemies back. Not reached live: the wipe (the guest's test death was taken
  by the boss-clear watchdog after a `host_kill` ForceDead raised BlockClear2_3 on the guest, so it
  left and rejoined by the bell - a test-aid artifact) and the guest's same-map solo load (it came
  while the host death's re-join ran and was correctly dropped: "nothing runs", the host stayed).

## 7. Risks / open items

* (Closed in 6.7 / 6.8) K10 / boss clear with a real kill; wipes (guest and host dead) and falls
  follow the host; the guest's fog-gate entry by the in-process pad. A guest that dies alone respawns in
  the host's world (6.9).

* (Closed in run 9) Guest rewind not executing in run 5. Static analysis: the update `0x1E52FE0` copies
  +0x54 to +0x50 unconditionally on entry, so `step 6 next 4` means the task update was not
  called at all on the guest. Its only caller is `0x1E556F0` from the flow update `0x1789B60`,
  which skips it while SprjRemo (`*0x5540058 +8 → +0x168 bit 0`) plays; `0x1789B60` itself runs
  from the in-game net update `0x1947310` and three load-step callers. Candidates: a remo playing
  on arrival, or the in-game net update not running after a non-summoned host-world load
  (GSM +0x1521 was 0). The status line now logs the flow timer (+0x98, advanced by every
  `0x1789B60`), +0x1521 and SprjRemo; D4 runs the update itself if it stays stalled.
* Host death (K8/K9, untested live): the host keeps its member tasks, respawns at its lamp through
  the funnel (B1 captures HostDeath and broadcasts it), the guests drop HostDead_1 and follow the
  travel event with B2.
* Boss clear (K10 + veto, live in 6.7): the boss-clear Lua states leave at 0x13858F5 /
  0x1385A95 (`e8 a6 ae b4 00` / `e8 06 ad b4 00` → 0x1ED07A0) on **both roles** and send home at
  0x13856F4 / 0x13859DF (guest); with B2 the leaves are dropped on host and guest and the guest's
  send-home is vetoed, so the party stays in the cleared area (rewards unchanged).
* Early loading screen (E1, untested live): what the request byte does while the guest is still
  in the world (input, pause) is unknown; the guest's own load clears it at InGameStart.
* Loading-screen count: host 1, guest 1 (its own load starts when the host's snapshot arrives,
  i.e. after the host's load; ~host load + guest load in wall time). Starting the guest's load in
  parallel needs the snapshot taken from the destination before the host is there - not possible;
  today the guest stands in the old map while it waits (flow 1); planned: raise its loading screen
  at the TRAVEL event (NowLoading 0x176D380) so the player sees one continuous screen.

* [I] GameData remote rows surviving a host load (MoveMapStep ctor touches only the local row) -
  check +0x10 of each row after the load in the two-instance run.
* [I] EnterleaveDirector entries: STEP_CreateWait waits on a presentation entry; after a load it
  is created again (`0x19700A0` is find-or-create) - the summon effect may replay on re-creation.
* [I] WorldChrMan +0xA40 net-sync object per world: if it survives, `0x18F0DB0` double-registers
  the peer (it is a list push).
* Host death / host Hunter's Mark: the task path is fine, but ChrIns_OnDeath (role 3) requests the
  return of every human task (`0x1E54DB0`) - B2 death needs that branch suppressed (not done).
* Boss clear / BlockClear2 send-home, Dream gate, 4 players: unchanged from B1; with B2 a member
  that does go home must leave cleanly (the K hooks only act with role 3/6).
* Different load speeds: the guest's state 1..3 window is limited to 60 s by the flow timer
  (`0x55640E0`); slow guests need +0x108 reset each tick (cheap).
* Packets 0x06 from a guest still loading reach a host whose task re-created the chr only after
  "arrived" - correct order; the reverse (guest early) is handled by D2 only after state 4.
