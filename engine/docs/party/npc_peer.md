# NPC summons as a test peer and as a "puppet co-op" fallback

Scope: Bloodborne 1.09 NPC phantom summons (AI hunters summoned with the Beckoning Bell), evaluated
(1) as a single-instance test peer for the party session work and (2) as a fallback co-op
architecture where a remote human drives an NPC phantom. Addresses are **our offsets** (raw ELF VA,
see PARTY_COOP_PLAN.md). bbhost source addresses = ours + 0x400000. Labels: **[verified]** read in
the decompilation, **[data]** read from game files, **[inferred]** a reasoned guess that still needs
a runtime check.

## TL;DR

- NPC summons are **EMEVD-driven and fully offline**. They do not use Lua bell events, Matching2,
  SprjSessionManager membership, ObjectRefs or packets. The engine calls them "AI player" summons.
  The map already holds the NPC (a `c0000` NPC `PlayerIns`, disabled). EMEVD `2003:51 NPC召喚`
  sends a summon request through the SOS selection state. The next tick creates a
  `CSMultiNPCPlayerInsTask` and puts the NPC in the NetworkFlow cooperator slot table as kind 2.
- **As a test peer:** useful, but it covers only part of the session code. It exercises
  CSMultiPlayMan (its NPC vector), the 5-entry slot table and its cap, the cooperator count, the SOS
  request filter (including the boss-cleared rejection), the summon and return presentation, the
  return triggers (boss kill, host death, map unload) and EMEVD summon flags. It cannot test the
  session state machine, room join/leave, packets, guest-side logic or `CSMultiPlayerInsTask`.
  Recipe in §5.
- **As puppet co-op:** possible on the host side only. The engine already has a manipulator
  abstraction: `ChrIns+0x60` holds the active controller (Pad / Network / NetAI / Com). An NPC
  phantom could be driven by a NetworkManipulator fed with the remote player's frame snapshots.
  The result is **one-sided**: the remote player's own game still runs its own world. Mirroring the
  host's enemies and world on their screen means reimplementing the game's enemy and world sync.
  That is exactly what the PSN-session path already provides. **Verdict: do not pursue puppet
  co-op as a co-op mode.** Keep the PSN-session path. Use NPC summons only as a test fixture (and,
  at most, an "I can see you" presence mode).

---

## 1. NPC summons end to end

### 1.1 Trigger: EMEVD, not Lua [data] [verified]

`common.emedf` (instruction names, Japanese) gives two summon instructions in bank 2003 (イベント):

| Instr | Name | Args |
|---|---|---|
| 2003:25 | AIプレイヤー召還SOS作成 ("create AI-player summon SOS") | summonType, npcEntityId, spawnPointEntityId, summonEventFlag, dismissEventFlag |
| 2003:51 | NPC召喚 ("NPC summon") | same 5 args |
| 2003:54 | (missing from emedf; the handler is there) | npcEntityId → send home |

No shipped `.emevd` uses 2003:25. 2003:51 appears in one template event per map (`1xx04410`);
2003:54 appears once (m24_01 event 12414480). The handler is the bank-2003 switch
`FUN_017c0420` (reached from the EMEVD dispatcher 0x17b93a0):
- case 0x19 / 0x33 → `SosSelection_BuildRequestFromChr` 0x1878d90. For 0x33 the summon type is
  first mapped through the u32 table at 0x4733a10: `{0:26, 1:28, 2:29, 3:30, 4:31, 5:32, 6:33, 7:27}`.
  That maps the EMEVD enum to a **session type** (index into the descriptor table at 0x553d750).
- case 0x36 → `GetChrByEntityId` 0x13c97a0 then `NetFlowSlots_ReturnNpc` 0x15be2a0(slots, handle, 0).

Template event `12414410` (m24_01). Arguments: X0 type, X1 NPC entity, X2 spawn region,
X3 summon flag (xx04420), X4 dismiss flag (xx04430), X5 "available" flag (xx04440),
X6 boss flag (xx01800/1700), X7 ActionButtonParam id (105xx):
1. If X3 (summoned) is off, `2004:05` disables X1 (the NPC sits disabled in the map).
2. If the area boss flag X6 is on, the event ends. A multiplayer-state condition (3:06) decides
   whether to force `SetNetworkUpdateAuthority(X1, 0xfff)` (2004:28).
3. The wait condition needs all of: the player owns goods 4312 (presumably the Beckoning Bell),
   X3 off, X4 off, X5 on, X6 off, and an **action-button prompt** (ActionButtonParam X7) anchored
   on the NPC entity X1. In practice: walk to the spot and press the "use bell" prompt.
4. It then plays animation 100111 on the player (ringing the bell), applies SpEffect 4682,
   **runs 2003:51(X0..X4)**, clears SpEffects 9005/9025, waits 5 s and shows message 100051.

Uses found in the shipped `.emevd` files:

The bell template `xx04410` is instantiated in most maps of the campaign (Old Yharnam, the three Cathedral / Yharnam maps, the
Forbidden Woods, Yahar'gul, Byrgenwerth, the Hunter's Nightmare and the Research Hall), each with one to six NPC entities
(summon types 0, 5, 6 and 7 appear; type 7 maps to session type 27 for a `c2710` part, a non-player model driven by its own
think row), and the Chalice dungeons use the events 12904634 / 12906966. The Hunter's Nightmare holds **six NPCs in one map**,
which is good for cap tests. The per-map entity ids are read from the player's MSB / EMEVD files, not kept here.

Lua plays no part in triggering. The SOS tick only posts the "NPC summoned"/name message through
SprjLuaEventMan (`FUN_01317110`, MsgTagMan name tag). The bell Lua events in the plan
(`OnEvent_Call_SOS`) are for human co-op only.

### 1.2 Request → scheduled work → task [verified]

1. **`FUN_01878d90(sel, sessionType, entityId, summonFlag, pos*, rot*, eventId*, dismissFlag, useNetPos)`**
   - `sel` is the event-side `SprjEventSosSelectionState`, read as `*(*(*(0x553b108)+0x60)+0x30)`
     (SprjEventMan → area state → +0x30).
   - It needs the character from `GetChrByEntityId` 0x13c97a0, and HP > 0 (data module +0xf8).
   - It allocates a 0xa8 request:
     - `descriptor_id` = the low dword of the request pointer;
     - `session_type` at +0x22;
     - `world_chr_lookup_key` = entity id at +0x88;
     - summon/dismiss flags at +0x8c/+0x90;
     - pos/yaw.
   - For an NPC `PlayerIns` (vfunc +0x1a8 IsNpcPlayer returns 1) it also:
     - writes **gameData+0x5d8 = sessionType** (the session descriptor index);
     - copies name (FMG 0x77 or 0x12, or `?NpcName?`), level, stats and colours from the
       GameData record into the request.
   - It drops the request if `capability_mask(desc[st]+0x04) & currentMask(0x186fe40)` is 0.
   - It then runs the common **SOS filter `0x1874710(sel, req, 1)`**, the same filter human signs
     use, which contains the boss-cleared rejection at 0x18749E8. Then insert/schedule
     (0x1875320 → 0x1876060 → 0x1877460).
2. **`SprjEventSosSelectionState` tick `FUN_01872360`** (called from `FUN_01947310` unless SprjRemo
   is active). For each scheduled work entry with `world_chr_lookup_key != -1`, i.e. an NPC
   (human entries wait for the 55 s/180 s timeouts at 0x4927a58/0x4927a5c instead):
   - resolves the handle with `0x191a440(WorldChrMan, entity)`;
   - **`CSMultiPlayMan_EnsureNpcTask` 0x1e54c30(*(0x5540230), &CSMultiNPCPlayerCreateInfo)**
     with `{handle=*(chr+8), session_type, pos, rot, init_flag=summonFlag, end_flag=dismissFlag, flags=0}`;
   - **`NetFlowSlots_Register` 0x15bc590(*(*(0x5556678)+0x16f8), handle, desc[st].team_type
     (0x553d760+st*0x80), kind=2, summonFlag, dismissFlag, desc[st].summonparam_type (0x553d75c+st*0x80))**;
   - sends a 16-byte message type **0x32** `{handle, summonFlag, dismissFlag, st, 1}` to every
     non-local session member through 0x17894e0 (offline there are none).
   - It also writes event flag **6009 (0x1779)** = "all multiplayer tasks are past summon wait".
3. **Guests** (`FUN_01947310`) drain message 0x32 and run the same EnsureNpcTask + Register on their
   own copy of the map NPC. They drain message 0x33 `{handle, died}` into
   `NetFlowSlots_ReturnNpcRemote` 0x15be770. So the NPC exists in every world, and the host's AI is
   replicated to guests (see §3, NetAI).

### 1.3 The task: `CSMultiNPCPlayerInsTask` (0x130 bytes, vtable 0x534f470) [verified]

Owned by CSMultiPlayMan (singleton at 0x5540230; NPC vector at +0x38/+0x40; primary human vector
at +0x10/+0x18). Steps:

| Step | Fn | Behaviour |
|---|---|---|
| 0 Init | 0x1e4b170 | If flag 0x01 (skip presentation) is set, zero both timers. |
| 1 SummonMsgWait | 0x1e4b1b0 | Count down `summon_message_timer` (runtime global 0x558eb28, 10 s default). |
| 2 SummonWait | 0x1e4b230 | Mode 1: wait for EnterleaveDirector (0x553e8b0) presentation `0x19701f0(handle, st)` and the actor's vfunc +0x78 "ready". Return-requested (0x04) jumps to step 5. |
| 3 Summon | 0x1e4b440 | Clear 0x02. **Warp**: actor vfunc +0x5f8 (`0x18cd600`, pos, rot). Vfunc +0x610(1) sets PlayerIns+0x52c bit 3. Unless flag 0x01 is set: write `desc[st]+0x24` (0x553d774) into the event module (+0x58) +0x20 and the owner's +0x20→+4 (phantom draw parameter) [inferred meaning]. |
| 4 Update | 0x1e4b620 | Idle until RETURN_REQUESTED (0x04). |
| 5 ReturnWait | 0x1e4b650 | If 0x10 is set and the session is not a client: wait for the leave presentation (`0x19704b0`) and count down `return_timer` (20 s default). |
| 6 Return | 0x1e4b830 | Free the matching NetworkFlow pending slots (+0x80..+0x8c, count +0x90). Then **`ChrIns_SetDisable` 0x18c6390(chr, 1)**, **chr_type (+0x78) = 9**, refresh, restore +0x128 (not HP: 106 / 106 on a 719 HP hunter, likely MP) and stamina to max (+0x12c→+0x128, +0x138→+0x134; HP is +0xf8 / max +0xfc, the struct at +0xf0, docs/party/balance.md). Revive if dead (vfunc +0x1a8/+0x90). **SetEventFlag(end_event_flag)**. |
| 7 Finish | 0x1e4bc00 | Teardown (no function boundary in Ghidra). |

The task never creates the actor; it reconfigures an existing map character. **Removal means
"disable + reset"**, so the NPC can be summoned again.

### 1.4 Phantom type, team and colour [verified, table contents runtime]

- **Session type** `st` (26..33; the debug path uses 27) indexes `SessionTypeDesc[0x22]` at
  0x553d750, stride 0x80. bbhost `frpg.hpp` gives the fields:
  - +0x04 capability mask;
  - +0x0c summonparam_type;
  - +0x10 team_type (stored in the slot);
  - +0x15 "net player chr type";
  - +0x24 phantom parameter written on Summon;
  - +0x40/+0x44/+0x48 enter/leave message ids;
  - +0x78 label.

  The table is **built at runtime** (zero in the ELF). Dump rows 26–33 in game.
- **Chr type** for a summoned NPC `PlayerIns` is not `ChrIns+0x78`. The cooperator logic calls
  vfunc +0x558 = `0x18fe3e0`, which returns `desc[gameData+0x5d8].field_15`. That value was set
  by 0x1878d90.
- When `Register` (0x15bc590) gets a negative summonparam_type and `ChrIns+0x78 == 1`, it writes
  phantom parameter **0x1b27 / 0x1b28** (6951/6952, chosen by vfunc +0x598) into the event module.
  These are the white NPC phantom colours [inferred].

### 1.5 Cooperator slots [verified]

The NetworkFlow slot object is `*(*(0x5556678)+0x16f8)`:
- +0x14 member count;
- **5 entries at +0x1c, stride 0x14**: `{handle, kind, state, init_flag, end_flag}`.
  `kind`: 0/1 are human peers (their NPID goes to FrpgNetMan `0x148bc70`); **2 is an NPC**.
- `Register` refuses when `count >= *(*(0x553d6d0)+0x0c)`. That is the session member cap
  [inferred name]; it is shared by humans and NPCs.
- The cooperator count `0x15bdc20(slots)` walks the 5 entries. It skips the local player unless
  the session is a client (role 6) or +0x98 != -1. It counts an entry when the actor's
  vfunc +0x558 is in `{1,5,7,19}` (mask 0x800a2). **An NPC phantom therefore counts only when its
  `desc[st].field_15` is in that set.** Verify at runtime for types 26–33.
- `NetFlowSlots_Release` 0x15bc080 frees an entry. For kind 2 it also requests the NPC return; it
  then compacts the table.

### 1.6 Sending home [verified unless marked]

| Trigger | Path |
|---|---|
| EMEVD `2003:54` / Lua binding `0x13ceac0(entity)` | `0x15be2a0(slots, handle, 0)`: leave message ("<?leaveName?>", desc +0x48), `RequestNpcReturn` 0x1e55050, release slot, message 0x33 to peers |
| Death or "leave magic" of a PlayerIns (`0x18fef50`, call at 0x18ff77a) | `0x15be2a0(slots, *(PlayerIns+0x55c), 1)`: death message variant (desc +0x40/+0x44) [inferred which actor] |
| Host (local player) death, `ChrIns_OnDeath` 0x18c49f0 | **Only when session role == 3**: request return for every human task (0x1e54db0) and every NPC task (0x1e55050) |
| Lua `PlayerKill_4030_1` binding 0x1383d90 | Request return of all tasks and post Lua event 4030 |
| Boss kill: EMEVD `2003:12` ボス撃破処理 | Lua event 4050 (`0x132eeb0(ctx,0xfd2,…)`). The return goes through Lua/flags (X6 ends the template event) [inferred] |
| Map change / reload | The NPC belongs to its map ChrSet: unloading invalidates the handle, and the EMEVD re-runs and disables the NPC again |

`RequestNpcReturn` 0x1e55050 sets lifecycle bit 0x04. It also sets 0x10 ("leave via presentation")
when role == 3, or when the slot count ≥ 2 and the local player's chr_type is 0.

### 1.7 Online at all?

No. Everything above runs in single player. The slot table, CSMultiPlayMan, EnterleaveDirector and
the SOS selection tick all exist offline. Online, the only extras are message 0x32/0x33 to peers and
NetAI replication.

---

## 2. Spawning one programmatically (main thread)

Run from the main-thread tick (SprjFlipper hook 0x2034770), only in the world: main player at
`*(WorldChrMan 0x553e878)+0x60` is non-null, and the NowLoading byte 0x556286b is 0.

**A. Faithful path** (same as EMEVD 2003:51; goes through validation, the filter and the Lua message):

```c
void *chr = GetChrByEntityId_13c97a0(entity, entity, 0);   // Lua SetDisable passes (id,id,0)
if (!chr) -> entity not in a loaded map
ChrIns_SetDisable_18c6390(chr, 0);                          // EMEVD left it disabled
void *sel = *(void**)(*(char**)(*(char**)(G(0x553b108))+0x60)+0x30);
float pos[4] = player pos; float rot[4] = {0, yaw, 0, 0};    // fn reads pos[0..2] and rot[1]
int evid = -1;
SosSel_BuildRequestFromChr_1878d90(sel, (int8)st /*26..33, e.g. 27*/, entity,
        /*summonFlag*/ -1, pos, rot, &evid, /*dismissFlag*/ -1, /*useNetPos*/ 0);
```

The next tick creates the task and the slot entry. Flags of -1 mean "no flag": Return checks `>= 0`
before setting the end flag. Ignore `useNetPos=1`, which overwrites pos from FrpgNetMan.

**B. Direct path** (skips request validation and the filter; this is what a guest does on message 0x32):

```c
CSMultiNPCPlayerCreateInfo ci = { *(u32*)(chr+8), st, pos[3], rot[3], -1, -1, 0 /*flags; 0x01 = no presentation*/ };
CSMultiPlayMan_EnsureNpcTask_1e54c30(G(0x5540230), &ci);
NetFlowSlots_Register_15bc590(*(void**)(G(0x5556678)+0x16f8), ci.handle,
        *(u32*)(0x553d760+st*0x80), 2, -1, -1, *(s32*)(0x553d75c+st*0x80));
```

**Appearance and equipment:** these come from the map part. MSB Enemy data holds
ThinkParamID, NpcParamID and **CharaInitID**. For example 2410740 uses 6153/6153/6153 and
3400921 uses 6602/6602/6602. The NPC `PlayerIns` is built at map load by the factory 0x18d85b0
from CharaInitParam:
- equipment slots, `npcPlayerFaceGenId`, `npcPlayerThinkId`, stats, `npcPlayerType`,
  `npcPlayerDrawType`, `npcPlayerSex`;
- NpcParam: team and drawing;
- NpcThinkParam: the AI.

Choices:
- pick an entity whose CharaInitParam you like;
- or patch the CharaInitParam row in memory before the map loads;
- or, after spawn, edit the actor's GameData record (`PlayerIns+0x3c0`) equipment and call the
  refresh. That last route is heavier and not mapped yet.

**Keeping it alive:**
- hook/ignore `RequestNpcReturn` 0x1e55050 and `ReturnNpc` 0x15be2a0 for the test handle;
- keep the dismiss flag at -1;
- stay out of boss areas whose flag X6 is set.

It cannot survive a map unload: the ChrSet owns it. Re-summon after every load.

**Despawn on demand:** `0x15be2a0(slots, handle, 0)` (normal) or `(…, 1)` (death message).

---

## 3. Input and control

### 3.1 The manipulator layer [verified]

`ChrIns+0x60` is the **active ChrManipulator**. bbhost calls it "ChrSync"; its type-1
"LocalPlayerChrSync" is the PadManipulator. `ChrIns_CreateManipulator` 0x18c0280(chr, type, mapRecord):

| type | class (runtime-class name) | size | vtable | notes |
|---|---|---|---|---|
| 0 | ChrManipulator (base) | 0xb0 | 0x536fac0 | |
| 1 | PadManipulator | 0x1a0 | 0x5370430 | ctor 0x1526410; update 0x1526750 (+0x48); owns the PlayerFrameSnapshot (0x1a71fb0) and sends **packet 0x06** |
| 2 | NetworkManipulator | 0x160 | 0x5370250 | ctor 0x1524430; drives remote humans from packet 0x06 frames |
| 3 | ReplayManipulator | 0x110 | 0x53707d0 | |
| 5 | NetAIManipulator | 0x180 | 0x5370070 | drives an AI actor whose AI runs on another machine |
| 6 | ComManipulator (AI) | 0x120 | 0x536fca0 | `0x18c08a0(chr, thinkId, …)`; update **0x151a720** (+0x48) |

Vfunc +0x90 returns the type. NPC `PlayerIns` (ctor 0x18f1700): **+0x3c8 = ComManipulator,
+0x3d0 = NetAIManipulator, +0x60 = +0x3c8 at start**. NpcIns (0x18e4870) keeps Com at +0x3b8. The
engine switches +0x60 between Com and NetAI by network authority (owner not mapped yet).

### 3.2 What the AI writes [verified layout, partial semantics]

`ComManipulator::Update` 0x151a720(dt, manip, chr) has `manip+0x88` = owner and `manip+0xb0` = the
AI/think object. AI object fields: +0x1d0 target position, +0x1f4 requested AI action (fallback
+0x1f8), +0x204/+0x205 hold flags. Each frame it:
- writes the movement vectors into the manipulator: +0x10 move direction, +0x20/+0x30 target
  relative/absolute, +0x60, +0xa0, and stick at +0xf0;
- maps the AI action id through the table at 0x4731160 to a bit and **ORs it into
  `SprjChrActionRequestModule` (container +0x80) +0x10 `current_requests`** (also +0x40/+0x48
  buffered/delivery masks for some actions);
- sets the lock-on target at module +0x94;
- calls the module update 0x1a0f910, which computes rising/falling/held times.

HKS (`env(ActionRequest…)`) reads that module, so attacks, dodges, item use and weapon transforms
are all request bits. PadManipulator 0x1526750 fills the same fields from the pad.

### 3.3 How to drive it with remote input

- **Option N (recommended for puppets):** set `ChrIns+0x60` to a NetworkManipulator (type 2,
  constructed in our own allocation with 0x1524430). Feed it the remote player's **packet-0x06
  frame snapshots** (PlayerFrameSnapshot 0x1d8 B, serializer 0x1a72220) taken on their side from
  their PadManipulator. This is the exact path the game uses to animate a real remote phantom:
  position and rotation interpolation, animation and TAE state, weapon state. Position correction
  is built in.
- **Option P (input injection):** keep or replace the ComManipulator. Hook 0x151a720 (or replace
  +0x60 with a PadManipulator) and write the streamed stick vector (+0x10/+0xf0), action bits
  (module+0x10) and lock-on target (+0x94). The host then simulates the puppet itself. Latency and
  divergence come back as rubber-banding: correct it with the warp vfunc +0x5f8 or physics module
  (+0x68) +0x1d0 position writes.
- Animation and weapon state: under Option N they come with the snapshot. Under Option P they
  follow from requests, but the equipped weapon, transform state and items are the **NPC's**
  (CharaInitParam), not the remote player's.

---

## 4. What a full puppet co-op would need, and verdict

On the host, the puppet is an NPC phantom: it counts as a cooperator, enemies aggro on it, and it
deals damage with its own GameData.

| Need | What it takes |
|---|---|
| Damage dealt by the remote player | Option N: real remote phantoms send damage events (packet 0x14, `0x1a341b0`/`0x1a29e40`) from **their** game, against **their** copies of the enemies. The host must accept "remote hit enemy X" messages and apply them to its own enemies, which needs a shared enemy identity. Option P: the host simulates hits with the NPC's weapon and stats, not the remote player's build. |
| Damage received | The host computes it on the puppet. The HP has to be streamed back, and the remote's game must apply it to its own player (custom message + SetHP). Death: the puppet dies on the host, but the remote must be forced to die and respawn. |
| HP, blood vials, items, weapon transform, buffs | Every consumable and SpEffect has to be mirrored each way: remote use → puppet SpEffect/anim; host effects → remote. |
| Gear and appearance | Rewrite the NPC GameData record (equipment, face, stats) from the remote save. Not mapped yet. |
| Enemy aggro and state on the **remote's** screen | **Missing.** The remote game runs its own offline world. Its enemies do not see the host, are not killed when the host kills them, and are not where the host's are. Mirroring needs: a puppet of the host in their world; every enemy switched to a NetAI-style manipulator fed from host state; HP and death sync; event flag, door, item and boss sync. That is a reimplementation of WorldChrSync, enemy authority and the EMEVD flag broadcast. |

**So puppet co-op is one-sided.** The host gets a convincing AI-slot ally. The remote player sees
their own unshared world (at best with a host ghost walking around, if we also puppet the host on
their side). That gives two parallel worlds, not co-op.

**Verdict:** the PSN-session path (bbhost-style serverless Matching2 + the real `CSMultiPlayerInsTask`
+ the game's own enemy and flag sync) already provides everything in the table natively, and the
plan's phases A–C build on it. Puppet co-op would mean rewriting the game's netcode, minus its
tested edge cases. **Feasibility for real co-op: low (months, with a fragile result). Not
recommended.** The only cheap by-product is a "presence" mode: both sides puppet the other as an
NPC phantom with Option N, for chat or exploration only, with no shared combat.

---

## 5. Using an NPC phantom as a single-instance test fixture

Proposed `BB_PARTY_TEST_NPC=<entity>[:<sessionType>]` (default type 27). Extras:
`BB_PARTY_TEST_NPC_RETURN=1` sends it home after N seconds, `BB_PARTY_TEST_NPC_LOG=1` logs the slot
table every second.

Steps in the director tick (main thread):

1. Gate: in the world, not loading, `GetChrByEntityId 0x13c97a0(id,id,0) != NULL`. Re-arm after each
   load, since the NowLoading byte goes 1→0.
2. `ChrIns_SetDisable 0x18c6390(chr,0)`, then §2 path A with `st` (path B to bypass the filter).
3. Each second log:
   - `CSMultiPlayMan` (0x5540230) NPC vector size (+0x40−+0x38)/8 and each task's `current_step`
     (+0x50) and `lifecycle_flags` (+0x128);
   - the slot table (`*(*(0x5556678)+0x16f8)`: +0x14 count, 5×0x14 at +0x1c);
   - `0x15bdc20(slots)`;
   - event flag 6009;
   - SprjSessionManager +0x124.
4. Teleport tests: `vfunc +0x5f8(chr, pos, rot)` (0x18cd600) moves the phantom.

What it can verify:

| Patch / feature | Test with the NPC fixture |
|---|---|
| Cooperator caps 3/4 (count fn 0x15bdc20, member cap `*(*(0x553d6d0)+0xc)`, 5-entry table) | In m34_00, summon 3400921..926 (six NPCs) with path B, read the count each time, and confirm where `Register` 0x15bc590 starts refusing. Patch the cap to 4 and repeat. First check that `desc[st].field_15` ∈ {1,5,7,19}, or the count will not move. |
| SOS filter / boss-cleared rejection NOP 0x18749E8 (flag 2410) | Path A after the area boss is dead: without the NOP the request is dropped inside 0x1874710; with it, the NPC appears. |
| Boss kill | Fight a boss with the NPC. Watch the task reach step 5/6 (Lua 4050 path) and check our boss-kill patches neither strand the task nor leak the slot (count must return to 0). |
| Host death | Offline, role 0: no return request (0x18c49f0 needs role 3), and the NPC goes away with the reload. With the party host role 3 active, death must drive 0x1e55050 for the NPC task. A simple regression for the role-3 death branch. |
| Map reload / warp (retarget 0x19471B1, MoveMapStep, `CSMultiPlayMan::Stop` 0x1ED07A0) | Summon, then warp or reload. Expect: no crash on the stale handle, the task finishes, the slot is released, the fixture re-summons after load. A leaked slot shows up as a stuck count. |
| Return presentation / ReturnWait gate (role 6 / +0x98) | `0x15be2a0(slots,handle,0)` and check it reaches step 7 within `return_timer`. |

Out of reach for this fixture (needs a second instance or simguest): session role transitions,
Matching2 rooms, signaling, packets (0x32/0x33 go nowhere offline), `CSMultiPlayerInsTask`
(primary vector), WorldSessionObjectMan ObjectRefs, guest-side behaviour, member-left suppression.

### 5.1 The fixture as built (`gpu/shim/party/party_npc_test.{h,cpp}`)

`BB_PARTY_TEST_NPC=<entity>[:<st>][,...] | auto` (the env starts the party layer by itself; no
`BB_PARTY` needed). The session type defaults to the one the map's EMEVD template uses for a known
entity (table 1.1 through 0x4733a10), else 27. `auto` probes every entity of table 1.1 with
`GetChrByEntityId`. Modes (`BB_PARTY_TEST_NPC_MODE`): `summon` (path A), `cap` (one NPC every 3 s,
path B unless `BB_PARTY_TEST_NPC_PATH=a`), `filter` (A/B/C, below), `log`. Extras:
`BB_PARTY_TEST_NPC_RETURN=<s>` (0x15be2a0 after s seconds), `BB_PARTY_TEST_NPC_PATCH=1` (filter
stage C patches on for the run), `BB_PARTY_TEST_NPC_WARP=<WarpParam id>` (lamp warp 0x13cdf30 first,
once; 2412951 = Central Yharnam), `BB_PARTY_TEST_NPC_DELAY`, `BB_PARTY_TEST_NPC_LOG=0`. In every
mode but `log`, a wrapper on the SOS filter 0x1874710 logs each request's fate (`SOS filter:`).
Every second: `Party NPC: state map .., session .., cooperators N, flag6009 F, tasks: human H, npc
K [npc h .. st .. step S <name> flags ..]; slots: count .. (kinds ..), cap .. [5 entries]; sel
pending .., quota team1 .. team2 ..`. `Party NPC: done` marks the end of a load's work (for
`instances.py --until`). Re-armed after each load (the warp exercises it).

Task layout confirmed from the ctor 0x1e4ae90: +0xc8 handle, +0xd0 st, +0xd4 pos, +0xe0 rot,
+0xec/+0xf0 flags, +0x128 lifecycle byte (low 5 bits). EnsureNpcTask's create info:
`{u32 handle; u8 st; f32 pos[3] @8; f32 rot[3] @0x14; i32 init @0x20; i32 end @0x24; u8 flags @0x28}`.

### 5.2 Results (1.09, one instance, save at level 37 in the Hunter's Dream, warped to m24_01)

- **Entities in m24_01** (`auto`): 2410158, 2410740, and also 2700920 / 2700921 (listed under
  m27_00 in 1.1, yet resolvable from Central Yharnam; area 270180). 2410158 is not an NPC player
  (vfunc +0x1a8 = 0, chr type 7) and never leaves SummonWait. Rows 26/27/32 of SessionTypeDesc:
  mask 0x80, summonparam -1, team 1, net chr type 7 (counted as cooperator).
- **Boss-cleared filter (mode `filter`, 2410740 st 26, area 241040, boss cleared 1):**
  A (1.09): `request NULL`: 0x1878d90 drops it *before* the filter (capability bit 0x80 cleared by
  the SOS status 0x186fe40). B (0x18749E8/0x18749F0 NOP'd): still `request NULL`. C (B +
  0x18700D3 status restriction -> 0): `ACCEPTED`, task created. So in a boss-cleared area the
  "Party: Bells after boss defeated" NOPs alone do not let NPC requests through; the status
  producer's area restriction (patched by "Party: Bells anywhere") is the first gate. Both are
  on with `BB_PARTY` set.
- **Lifecycle (path A, stage C):** step 1 SummonMsgWait (flag 6009 = 1, slot `{kind 2 state 3}`,
  cooperators 0 -> 1) -> 2 SummonWait -> 4 Update (6009 back to 0); `0x15be2a0(slots, h, 0)` at
  +40 s -> 5 ReturnWait (flags 0x14), slot released at once (count 2 -> 1, cooperators 1 -> 0) ->
  task gone within ~2 s. No leaked slot.
- **Cap, path B** (4 NPCs, 3 s apart): every Register answered `registered`; slot count 1 -> 5
  = member cap 5 (the local player holds entry 0). The cooperator count reached 3 (2410158 does
  not count: not an NPC player). A 5th NPC would be refused (count >= cap); no fifth NPC entity is
  loaded in m24_01 (m34_00 has six). Only one task at a time progressed past SummonWait (the
  others stayed in step 2 until sent home). All four returns released their slots; count back to 1.
- **Cap, path A** (with `BB_PARTY_TEST_NPC_PATCH=1`): requests 1-3 `ACCEPTED`; the 4th is
  `request NULL` again: with 3 NPCs (4 players) the SOS status clears the capability bit, i.e. the
  vanilla four-player limit is enforced in 0x186fe40, before the filter's own quota checks
  (`sel+0x1c8` pending + slot kinds < 4, team quotas `sel+0x1fc`/`+0x200` = 3/3).

---

## Address index (our offsets)

| What | Addr |
|---|---|
| EMEVD bank-2003 handler (cases 0x19, 0x33 summon; 0x36 send home; 0x0c boss kill → Lua 4050) | 0x17c0420 |
| Summon type enum → session type table | 0x4733a10 |
| SosSel_BuildRequestFromChr | 0x1878d90 |
| Event-area wrapper / debug summon (entity 2410158, st 0x1b) | 0x13deaf0 / 0x1879a60 |
| SOS selection tick / net update caller | 0x1872360 / 0x1947310 |
| SOS filter (boss-cleared rejection inside at 0x18749E8) | 0x1874710 |
| CSMultiPlayMan singleton; EnsureNpcTask; RequestNpcReturn; RequestAllReturns | 0x5540230; 0x1e54c30; 0x1e55050; 0x1e54fd0 |
| NPC task ctor / steps 0–7 | 0x1e4ae90 / 0x1e4b170, 0x1e4b1b0, 0x1e4b230, 0x1e4b440, 0x1e4b620, 0x1e4b650, 0x1e4b830, 0x1e4bc00 |
| NetworkFlow slots (`*(*(0x5556678)+0x16f8)`): Register, Release, ReturnNpc, ReturnNpcRemote, CooperatorCount | 0x15bc590, 0x15bc080, 0x15be2a0, 0x15be770, 0x15bdc20 |
| SessionTypeDesc table (runtime-built) | 0x553d750 (stride 0x80) |
| GetChrByEntityId; ChrIns_SetDisable; Lua SetDisable | 0x13c97a0; 0x18c6390; 0x1332060 |
| ChrIns_OnDeath (role-3 return-all branch) | 0x18c49f0 |
| PlayerIns vfuncs (vtable 0x538f310): +0x558 net chr type 0x18fe3e0, +0x560 desc index 0x18fe420, +0x5f8 warp 0x18cd600, +0x610 0x18fe8a0 | |
| ChrIns_CreateManipulator; ComManipulator create | 0x18c0280; 0x18c08a0 |
| ComManipulator update / PadManipulator update | 0x151a720 / 0x1526750 |
| ActionRequest module update (container +0x80) | 0x1a0f910 |
| AI action id → request bit table | 0x4731160 |
