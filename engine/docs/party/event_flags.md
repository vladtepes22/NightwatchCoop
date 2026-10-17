# Event flags: store, vanilla sync, progress categories (party co-op C2)

Bloodborne 1.09, our native port. Addresses are **our offsets** (raw ELF VA; bbhost source =
ours + 0x400000; bbhost `.hpp` `Rva{}` = ours). Sources: Ghidra decompiles (`tools/re/decomp.py`),
bbhost `src/decomp/events/flag_store.*`, `include/bbhost/engine/sprj/event_flag.hpp`, and the
game's own data: every EMEVD script with FROM's debug event names (`event/*.emeld`), the
instruction definitions (`event/common.emedf`), and ItemLotParam / ShopLineupParam / ObjActParam.
Tool: `tools/party/flag_tool.py`. Table: the `flagRows` section of the party tables (`<user dir>/party_tables.txt`,
read by `party_gamedata.h`; derivation in section 7 and `docs/party/gamedata.md`); nothing is committed.

## 1. Functions and globals

| What | Our offset | Notes |
|---|---|---|
| SprjEventFlagMan singleton slot | 0x553b100 | `*(u64*)slot` = manager |
| SprjEventState singleton slot | 0x553b0d8 | owns the save serialization of the flags |
| IsEventFlag(man, id, &exists) | 0x13cfc00 | |
| SetEventFlag(man, id, on) | 0x13cfcc0 | plain read-modify-write, no lock, no event |
| GetEventFlagValue(man, first, bits) | 0x13cfd80 | last flag = bit 0 |
| SetEventFlagValue(man, first, bits, v) | 0x13d0060 | |
| clear bit range | 0x13d0430 | |
| FD4VirtualMemoryFlag ctor(this, heap, 5, 3) | 0x13cf0e0 | 10^3 = 1000 bits/block, stride (1000+7)>>3 = 125 |
| SprjEventFlagMan ctor | 0x13b5910 | pool of 0x4b0 = 1200 groups, zeroed |
| resolve_group_key(type, area, block, zone) | 0x13bc710 | pure function, tables below |
| build tree, own world (mode 0) | 0x13b6540 | + chalice remap 0x13b89b0(n), 0x13bc3a0 |
| SetLoadMode(man, mode) | 0x13bb590 | callers MoveMapStep ctor 0x1937570 (x2), 0x19d4d70 |
| clear chalice groups | 0x13bdc00 | from chalice code 0x1aa8410 / 0x1aa9500 |
| save: SprjEventState serialize | 0x1399de0 | called by the save writer 0x156aa50 |
| load: SprjEventState deserialize | 0x1399e90 -> 0x13c0dd0 | also 0x156b970 -> 0x13c0dd0 |
| snapshot serialize ("INFE") | 0x13be3c0 | group append 0x13beac0 |
| snapshot send (packet 0x10) | 0x1786050 | from 0x178b5e0 |
| snapshot receive + apply | 0x1783160 -> 0x13beca0 | client only |
| Lua `CompleteEvent` = "BroadcastSetFlag" | 0x132aad0 | registered at 0x133bea7 (lea fn; name "CompleteEvent") |
| broadcast id only (no local set) | 0x1785380 | packet 0x0f to every peer |
| packet 0x0f receive (main thread) | 0x1339b50 | SprjLuaEventMan update, via 0x1312ff0 |
| EMEVD event completion write | 0x12ed100 | SprjEmkEventIns::EndOrRestart(this, restart): sets flag (event id +0x28) + (slot s16 +0x2c) when > 98, set-only, both paths (0x16ed100 in earlier notes is not an instruction boundary) |
| WorldChrMan slot / packed map id | 0x553e878 | `*(*(slot)+0x60)+0x3f8`: area<<24 \| block<<16 \| ... |
| SprjSessionManager slot (+0x124 role, +0xf8..+0x100 peer list) | 0x5540290 | |

## 2. Store layout (SprjEventFlagMan, 0x553b100)

```
+0x00 vtable 0x5320b60 (FD4VirtualMemoryFlag)
+0x08 heap
+0x10 u32 5        digits of a group key space (10^5 at +0x14)
+0x18 u32 3        digits of a block (10^3)
+0x1c u32 1000     bits per block
+0x20 u32 125      stride: bytes per block
+0x24 u32 1200     pool block count
+0x28 u8* pool     1200 x 125 bytes = 150,000 bytes   <- this is what the save holds
+0x30/+0x38        std::map<u32 block, {s32 kind, u64 storage}> head (tree nodes: left +0,
                   parent +8, right +0x10, is_nil +0x19, block +0x20, kind +0x28, storage +0x30)
                   kind 1: storage = pool index (data = pool + index*125)
                   kind 2: storage = pointer (overlay / dynamic buffer)
+0x80 s32 load mode (0 own world, 1 another world's snapshot)
+0x88/+0x90        std::map<u32 group key, u8* buffer>  overlay for the host world snapshot
+0xb0/+0xb8        std::map<u32 group key, ...> dynamic groups with key >= 1200 (area 99), not saved
```

A flag id is decimal **TAABZnnn**: `T` type digit (1e7), `AA` area (1e5), `B` map block
(1e4), `Z` zone (1e3), `nnn` bit. `block = id / 1000`, `bit = id % 1000`; the bit lives at
`data[bit >> 3] & (0x80 >> (bit & 7))` (first flag of a byte = MSB). A "value"
(GetEventFlagValue) is `bits` consecutive flags with the last one as bit 0, continuing into the
next block if needed.

### Group keys (0x13bc710) -> pool index

Tables in the eboot: `0x4730bf0 = {0,1,5,6,7}` (global types), `0x4730c10 = {1,5,6,7,9}` (map
types), `0x47314d0` = 17 `{area, block}` maps: 21_00 21_01 22_00 23_00 24_00 24_01 24_02 25_00
26_00 27_00 28_00 29_00 32_00 33_00 34_00 35_00 36_00 (map index 0..16).

- **Global** (area 0, block 0; ids 0-9999 and T0000Znnn for T in 1,5,6,7):
  `key = typeIdx{0,1,5,6,7} + zone*5` -> keys 0..49.
- **Map** (T in 1,5,6,7,9): `key = zone*115 + 50 + 23*typeIdx{1,5,6,7,9} + mapIdx`, mapIdx from
  the 17-map table, area 40 -> 11, areas 41..46 (chalice instances) -> 17..22 -> keys 50..1199.
- **Area 99** (block 0): `key = 0x4b0 + typeIdx + zone*5` (>= 1200): not in the pool, kept in the
  +0xb0 tree; chalice dungeons whose map is not in the table use these.

Mode 0 (0x13b6540) maps blocks 0..9 (ids 0-9999), the T1/5/6/7 global groups and every
`{map, type, zone}` above to pool indices (kind 1). Chalice slot: flags 9020..9026 are a
7-bit one-hot value; `0x13b89b0(7 - bitlength)` remaps the chalice instance groups (areas
41..47 - n), and the load / mode-1 code re-derives it. **Never write 9020-9026 from outside.**

### Saved vs runtime

- Saved: exactly the pool, `count(+0x24) * stride(+0x20)` = 150,000 bytes, plus one byte
  (SprjEventState +0x26 revive-wait) - 0x1399de0. Load 0x13c0dd0 reads the pool back (save
  version < 0x41 skips the groups of areas 34..36, the DLC) and re-runs the chalice remap.
- Not saved: the overlay (+0x88) and the area-99 groups (+0xb0).
- In the saved pool but runtime in practice: EMEVD completion flags of events with rest
  behaviour 1 (restart on reload), enemy/AI zones 1AAB5nnn and 1AAB6nnn, boss-fight state in
  1AAB4nnn (bbhost: 1AAB4800 transient, but 1AAB4223-style post-cutscene flags persist), session
  flags 2000-2022 rewritten every frame by the session code (bbhost event_flags.cpp), lamp warp
  requests 7AAB01xx, map-constructor-derived globals (e.g. 2400-2407 re-set by m24_00 event 0).
- Event completion flags: an EMEVD event's id + slot **is** a flag in the same store (e.g. boss
  event 12401800 -> flag 12401800). The interpreter writes it directly (0x12ed100), not through
  SetEventFlag. Restart (rest=1) events reset theirs.

## 3. Load modes: why phantoms' saves never progress

SetLoadMode(1) (MoveMapStep, when entering another player's world) re-points the block tree to
kind-2 overlay buffers from +0x88: global blocks 0-5 and 9 (ids 0-5999, 9000-9999), the T1/5/6/7
global groups, and **every map group of types 1,5,6,7**; blocks 6, 7, 8 (ids **6000-8999**) stay in
the player's own pool (personal: runes, messenger shop, Doll, chalice unlocks). It masks
9020-9026 and sets the host's chalice slot bit. SetLoadMode(0) rebuilds the own-world tree.
So while a guest is in the host's world, every Get/SetEventFlag (events, Lua, talk) reads and
writes the overlay copy, which is discarded on return - the guest's saved pool is untouched
except 6000-8999.

**C2 consequence:** to make a guest's save keep up, write the guest's **pool** directly
(`pool + key*125`, key from the 0x13bc710 formulas above), not via SetEventFlag, whenever the
guest is in mode 1; and write the visible tree (SetEventFlag) when it is in mode 0. On the host
(always mode 0) SetEventFlag is correct.

**Shop stock (per player).** The one-time ShopLineupParam rows' stock flags (`flag_tool.py params`, sellQuantity 1) live
partly in block 6 (own pool) and partly in the global T7 zone 9 and map T7 zone 1 groups (the Bath Messengers' armor, key
emblems, the Insight shop's attire, the Sinister chalices, the Dream's starting weapons). The T7 groups are overlay groups
in mode 1: the guest's shop reads and writes the host-world copy. Player report 2026-10-20: the host's purchase reached the
guest through the world mirror and showed sold out there, and the guest's own purchases stayed in the discarded overlay
(`kept local (not a world action)`). Now (`ShopStockFlag`, party_progress): the mirror never writes these bits
(`WorldMirrorMask` without them, so D3 never reports them either), and in mode 1 the guest merges the overlay's stock bits
with its own pool every frame, set-only both ways (`guest shop stock: purchase N ... saved to the own save`, `... own
purchase(s) shown in the host's world`), then requests a game save. Live 2026-10-21 (two loopback instances, default save,
the upper Insight messenger): before (`BB_PARTY_SHOP_STOCK=0` on the guest) the host's purchase reached the guest's overlay.
After: the host's Attire tab lists only what it has not bought, the guest's lists all; the guest bought one piece (saved to
the own save, Insight spent), was killed 8 s later and rejoined: the purchase was in its own pool and shown in the host's
world, and its next buy took the next piece, not the same one again.

## 4. Vanilla multiplayer flag channels

1. **Join snapshot (host -> client), packet 0x10.** 0x1786050 serializes with 0x13be3c0 into an
   "INFE" v1 blob (`{magic 'INFE', u16 1, u32 12 groups, u32 0x14 table, u32 0xa4 data}`; 12
   entries `{u32 group key, u32 data offset, u32 125}`), deflates it (level 9) and sends it with
   a 16-byte `'MNSE', 1, sizes` header. Groups shared: **global zones 0,1,5,9 (ids 0-1999,
   5000-5999, 9000-9999)**, **current map T1 zones 0,1,4,5,6,7** (1AAB0nnn, 1AAB1nnn,
   1AAB4nnn-1AAB7nnn) and **T6 zones 0,1** (area 99 for a map outside the table). The client
   (0x1783160, session role 6, WorldSessionObjectMan+0x84 == 1) inflates it, 0x13beca0 rebuilds
   the overlay (all overlay groups allocated zeroed, the 12 copied) and +0x84 becomes 2
   (EventFlagsSynced). Not shared: 2000-4999 (boss-cleared/area gates), 6000-8999, T5 item-lot
   flags, T7 map NPC/lamp flags, other maps.
2. **Live, packet 0x0f (any peer -> all peers).** Lua `CompleteEvent(id)` (0x132aad0) sets the
   bit locally and sends the u32 id to every session peer (0x1785380 sends without the local set).
   Receivers drain it every frame in 0x1339b50 (SprjLuaEventMan update, main thread) and OR the
   bit into the current tree - set-only, never cleared.
3. EMEVD events marked network-synced (2000[2]) exchange event state through packet 0x0e (event
   action state, 0x14 bytes, also drained in 0x1339b50) - not raw flags (open question).

## 5. Where flags change (hook points)

- SetEventFlag 0x13cfcc0 / SetEventFlagValue 0x13d0060 (bbhost compared ~82k and ~700 calls a
  session): EMEVD 2003[2]/[22]/[17]/[9]/[31]/[32]/[41]..[43], Lua, talk (ESD) scripts.
- Direct writers that bypass them: EMEVD completion (0x12ed100), 0x0f receive (0x1339b50),
  CompleteEvent (0x132aad0), table-driven setter 0x177b2a0, the snapshot apply, load, SetLoadMode
  and the chalice clear. Many more functions walk the tree inline (Ghidra lists ~70 references to
  the "SprjEventFlagMan" singleton name), so **setter hooks are not complete**: the host side must
  also diff snapshots (cheap: 150 KB pool) at safe points; bbhost saw ~4,700 flags flip in one
  title frame.

## 6. Safe points to apply flags

- **Main thread only.** The store has no lock; SetEventFlag is a byte RMW and the tree is a
  std::map that SetLoadMode/MoveMapStep rebuilds. Our tick (SprjFlipper::Update 0x2034770) and
  0x1339b50 are main-thread.
- Do not touch the store while NowLoading (byte 0x556286b set by 0x176d380, cleared by
  0x176d3c0), during MoveMapStep / SetLoadMode, during the save writer (0x156aa50; the save task
  may run off the main thread - verify) or load (0x13c0dd0). Re-resolve block pointers every time
  (never cache `data` across frames: mode switches swap them).
- Best moments: right after a map finished loading in mode 0 (world, before the first EMEVD tick
  on the new map if possible), at lamp rest / warp (WarpNextStage(_Bonfire) 0x132e010 /
  0x132e050), and live for flags that running events poll (boss dead -> fog walls, lamp lit). Door
  / shortcut / object visuals are applied by "reproduce result" events at map load, so they become
  visible after the next load.
- Writes to an id whose block is absent are no-ops in the game's code (and in ours).

## 7. Categories (the `flagRows` table)

Row format `first, last, category, note`, first match wins; ids no row covers: never_sync.
The table is not committed: it is derived from the player's game data (the EMEVD scripts and params of the game
folder; `flag_tool.py table GAME` prints it, `flag_tool.category_rows` is the reference derivation that the engine's
reader port follows, see "Derivation" below). It holds specific rows (from the scripts and params) first, then structural
rows (globals, then per map `T x zone` defaults). Categories: item_lot_picked, shortcut_door, session_runtime, npc_quest,
lamp_unlocked, boss_defeated, never_sync, cutscene_seen, key_event (plus boss_area, world_state, unclassified in
`flag_tool.py classify`, which covers every id the data writes/registers/awards).

Key findings behind the rows:
- **Boss defeated**: each boss-defeat event (2003[12]/[15]/[53]) is `1AAB1800`/`1AAB1700`-style;
  its completion flag is the persistent "boss dead" flag; it also sets the global area bit
  `2AAx`/`3AAx` (one per area, e.g. 2400-2402 for Yharnam) and a boss-count bit in the 9452-9471 range.
- **2410** = m24_01 event 12411899 "multiplayer on/off control, Central Yharnam", set when both the
  Gascoigne event and its sibling are done. ON **blocks** the host bell /
  summons in the area (area table 0x47304B0, validator 0x131D7B0; see
  campaign_start.md 2.2) - it is the flag behind the boss-cleared summon rejection NOPed at
  0x18749E8. Every gate flag of the two area tables (0x47304B0 xxx0..xxx3, 0x47301B0 xxx5..xxx8,
  resonant/responder) is **never_sync** (override rows in flag_tool.py `OVERRIDE_ROWS`).
- **Lamps**: lamp lit = `7AAB02xx` (awaited by common event 7100+slot "warp OBJ activate",
  cleared by the DLC-removal events); lamp object = `1AAB78x0` (2009[5] registration in common
  event 7000+slot); `7AAB01xx` and m21 `7210xxxx` are warp requests (runtime).
- **Doors/shortcuts**: rest=0 events in zones 0/1 named door/gate/lever/elevator/ladder (EMELD),
  their completion flags and the flags they set; ladders from 2009[0].
- **Item pickups**: ItemLotParam getItemFlagId(01..08) - map treasure in `5AABZnnn`, global key
  items in the 5000-5299 range; chest events. ShopLineupParam stock flags (7AAB1/9nnn, 66xx) are
  per-player purchases -> never_sync.
- **NPC quests**: 1000-1999 (20 flags per NPC), a few ranges in 9xxx, `7AAB0nnn`
  (per-map NPC state) and NPC-named map events.
- **Per-player, never sync**: 6000-8999 (kept in the own pool by mode 1: runes, chalice unlocks,
  messenger shop, Doll), the covenant rune flags, item use, umbilical cords, insight return.
  Endings, never synced silently (they trigger the staff roll / NG+;
  only through the C4 replay, cutscenes_endings.md): the ending flags and events of the m21 map.
- **9180** = "a cutscene is playing" (set around every story remo) -> never_sync (it was
  misread as "first death cutscene"). **9800-9802** = time of day (9800 evening, 9801 night,
  9802 Blood Moon; written by the cutscene end callback 0x1CECEB0) -> own category
  **time_of_day**, applied only through the cutscene replay path, never by the silent sync.
- **Covenant outcomes (0.7.3, own category covenant_outcome; user decision 2026-10-22: per
  player)**: the COVENANT_ROWS of `flag_tool.py` (emitted first, so they win over every other
  row) cover the state flags of Annalise (Vilebloods), Alfred and Valtr (the League), their map
  events and their T7 damage / kneel / revival counters. While the host's rule *Covenants per player* is on (default): C2 never
  writes them into a guest's save, a guest's change of them in the host's world is never a world action, and (0.7.7,
  covenant2077) the overlay shows the guest's OWN values (`GuestCovenantTick`: the covenant bits are out of the mirror, the
  overlay is the own pool's, and what the guest's talk changes in them is written back to the own pool; the log lines `guest
  covenant overlay:`), so a guest talks to Valtr / Alfred / Annalise on its own state after the host joined. Off: the base
  category (`BaseCategoryOf`: npc_quest, world_state, cutscene_seen). The oath itself is the equipped rune (the player's
  record), the rune flags and the personal block were per player already; the NPCs' item awards stay C3's; Eileen stays shared.
  **Rune effects per member (0.7.7, covfx077, smoke `covenant_runes` cvr)**: the oath rune's effect is its marker
  SpEffect on the player (the game's EMEVDs test these). Host and guest memorized different oath runes at the Memory Altar in
  the host's Dream (test aid `log_oath`): each game carried only its own covenant's rows, steadily, summoned and after the
  guest's Leave.
  **Chalice Dungeon (coven077, smoke `covenant_chalice` cvc)**: host and guest memorize their own oath rune at the Dream's
  Memory Altar, the host takes the dungeon lamp's party travel back to its dungeon: `log_oath` shows each member's own marker
  on its own chr in the Dream and in the dungeon, never the other's; screenshots of both games clean.
  **Numbers (covleft077)**: `log_oath` also dumps, once per effect, the SpEffectParam row the player's own active effect node
  points at (node +0x48 = the param table's row of that id): each member's chr holds its own covenant's row, never the other's.
  The oath MARKER rows carry no number (all rates 1.0, no stat field): the heal near death, stamina recovery and vial HP are
  engine / EMEVD code keyed on the marker being on the chr, so the marker on the right chr is the whole party-side proof.
  **Annalise's kneel (covleft077, 0.7.8)**: not a gesture (see 0.7.9 below). The kneel AREA is a 1 m sphere region (event
  sets a flag while the player is in it); all 23 gesture-list rows were swept there and none set the kneel flag.
  **Alfred** is no oath rune NPC: his gifts are triggers of common event 9040; the oath runes are Eileen's talk (Hunter),
  Annalise's (Corruption), Valtr's (Impurity) and the Radiance rune only in the Chalice. Smoke `covenant_alfred` cvf: host then
  guest talk to Alfred in one session, both get his full menu and lines, no rune lot reaches either. **Rune amounts**: each
  oath gem has a second SpEffect row holding the numbers (blood vial heal x1.02 for Radiance, a drain at very low HP for
  Corruption, a stamina recovery bonus for Hunter; Impurity none). `log_oath` dumps them and logs HP / stamina every 0.5 s
  (smoke `covenant_measure` cvm): both rows are on their own chr only; the effects themselves are gated or below the sampling
  resolution, reported, not asserted.
  **0.7.9 (covenant079, CL-95dee9) Annalise's kneel PROVEN, also for a guest.** The kneel is NOT a gesture: her ESD talk
  machine runs a prompt machine whose state returns 1 when the ACTION BUTTON (Cross) is pressed inside the 1 m cylinder
  region after the greeting; that enters the machine that sets the kneel flag. Smoke `cvk`: host Cross in the cylinder -> the
  kneel flag is set, the player anim plays, the choices "Swear Oath to the Vilebloods" / "Swear Vileblood Oath" appear and the
  rune is granted. Her region events all start with 1003[06] End if Client, so a GUEST never had the cylinder / box flags or
  the clears. `GuestAnnaliseRegionTick` (party_progress.cpp, kill switch `BB_PARTY_GUEST_ANNALISE=0`, covenant per player
  only) does them for the guest's own position (physics position, `balance::ChrPosition`; the +0xC0 spot stays at the lamp for
  a guest), clears the blood-anim flags after 3 s, and shows the rune's flag clear once per overlay while the own save lacks the
  rune, so the guest's talk SETS it. The rune's lot is now an OwnTalkGiftLot (the guest's own grant; the host's rune award
  still reaches a guest as a shared gift first: then its own talk logs "already given"). Not done: the guest's own kneel
  animation (the event plays it on the host only).
  **Rune amounts (0.7.9)**: `BB_PARTY_TEST_SET_HP=PCT:DELAY_S` sets the HP once; smoke `cvm` with `CVM_HP=10:100`: both members
  rise, i.e. a natural regeneration swamps any rune effect and no drain is seen; the stamina bonus is below the sampling
  resolution; the Radiance vial heal was not measured.
- **Session/runtime**: 2000-2099, 7000-7399 and 7600-7699 (common event completions), 9200-9299,
  zones 1AAB5/6/7nnn, restart events. **boss_area** (1AAB4nnn: fight state, fog, NPC summons) is
  not synced live; the vanilla session already carries it.

C2 policy (implemented in gpu/shim/party/party_progress.*): host -> guests, set-only, for
boss_defeated, lamp_unlocked, shortcut_door, key_event, cutscene_seen, world_state, minus the
area gates / endings / time of day above (never_sync / time_of_day rows) and ids the game
randomises or counts; item_lot_picked never by C2 (the host hands item-pickup flags to C3's
observer, party_items); npc_quest only per whole 20-flag NPC block and later; never the rest.
Elevators toggle (12400147/148 style) - sync their current state, not only sets. C3's ledger
flags 60009000+ are reserved.

## 8. Snapshot file (BB_PARTY_FLAG_DUMP=1)

The game side writes `BBPF` v1 (spec in `tools/party/flag_tool.py` docstring): 64-byte header
(bits per block, block count, packed map id, load mode, session role, unix ms, frame, label)
then per block `{u32 block, u16 125, u8 kind, u8 0, data[125]}` in ascending block order, bytes
exactly as the game holds them. Dump every block reachable through the tree (+0x38) - on the
host that is the saved pool; on a guest in mode 1 it is the host-world overlay plus 6000-8999 -
and, separately, the raw pool (label suffix `-pool`, blocks numbered by `id/1000` through the
0x13bc710 formulas) so the test can check the guest's save. Suggested triggers: on demand
(overlay hotkey), after each map load completes, after lamp rest, on exit; file
`user/party/flags_<label>_<frame>.bbpf`. `flag_tool.py diff A B --tables <user dir>/party_tables.txt
--sync-only` exits 1 when synced categories differ.

## 9. Risks

- Writing a flag under a running event that already evaluated it does nothing until reload;
  writing one a running event waits on triggers that event on the guest (double cutscene, item
  award, boss death handling run locally) - prefer applying at load boundaries for anything that
  is an event trigger.
- Event completion flags are written outside SetEventFlag; a hook-only design misses them.
- Mode 1 overlay vs own pool: applying through SetEventFlag on a summoned guest changes only the
  overlay; applying to the pool while the guest is in mode 1 is not visible until mode 0.
- 9020-9026 and the chalice groups change the store's own layout - never sync.
- Constructor-derived flags (2400-2407 etc.) are recomputed each load; syncing them is harmless
  but pointless; boss-fight state (1AAB48xx) synced mid-fight breaks the fog/boss scripts.
- NPC quest blocks are state machines (several flags, value ops); partial sync corrupts them.
- Items: syncing pickup flags without giving the item loses it (C3); shop stock is per player.
- DLC: saves without DLC clear DLC lamp/area flags (DLC削除 events) - host and guests must agree
  on DLC.
- Save writes: applying during the save writer could tear the 150 KB copy - gate on it.

## 10. Open questions

- Exact native writer of the lamp-lit flags 7AAB02xx and of 1AAB78x0 (lamp object code). The
  Hunter's Dream headstones list the lamps from the overlay's flags (a guest's list was empty until
  the host-world mirror, section 12, filled 72xx02xx).
- Packet 0x0e (network-synced EMEVD events) semantics and whether the host's EMEVD completion of
  network-synced events reaches guests.
- Which thread runs the save writer 0x156aa50 / its task.
- The full list of inline tree walkers (to know every flag writer) - a ring of snapshot diffs is
  the fallback.
- Talk (ESD) scripts set NPC flags too; not analysed (no ESD parser here) - npc_quest rows come
  from EMEVD/EMELD only.
- Chalice dungeons (area 29 blocks, areas 41-47, area 99) are out of scope.
- 70000030/70000050 (type-7 global) and 72400512 (set by the first three boss kills) meaning.

## 11. Silent catch-up: boss flags of the current map (2026-10-11)

A join snapshot applied in load mode 0 wrote the host's boss flags into a guest standing in that
boss's map (a fresh guest in the clinic, m24_01: Cleric Beast 12411800). The map's boss event then
sees the kill and plays it: "PREY SLAUGHTERED", the music change, its reward branch - without the
guest having been there. `GuestApplier::Apply(store, policy, allow_mode0, map_id)` now holds back
every `boss_defeated` flag whose id names the map the guest stands in (`HoldBossFlagInMode0`:
TAABZnnn area AA / block B == map id area / block) and writes it later: in load mode 1 (the own
pool, which the running events never see) or after a load into another map. The log says
"N boss flag(s) of this map held back". Live: the fresh guest's join snapshot in the clinic held
2 boss flags; after the summon 12411800 was 1 in the own pool, and after a restart the own save
read `12401800=1 12411800=1`. Rewards for such kills come only through C3. A banner is still
shown when the guest **witnessed** the kill (it is in the host's world; the host's event plays).
C2 mode-1 applies now also request a save (seamless guests may not save for a long time).

## 12. Host-world mirror: the guest's overlay as a copy of the host's world (2026-10-11)

**Gap.** In load mode 1 every Get/SetEventFlag of a guest reads the overlay (+0x88), and the only
thing that fills it is the vanilla packet 0x10 apply (0x13BECA0: all ~900 overlay groups allocated
zeroed, 12 copied, section 4). A/B live run (save `savedata_20261010`, host in the Dream, bell join,
then host lamp warp to Central Yharnam = B2; `BB_PARTY_FLAG_DUMP=1`, host pool dump vs guest visible
dump after the guest's m24_01 load, `BB_PARTY_WORLD_MIRROR=0`): of the 638 mirror groups the host
had some non-zero groups and the guest fewer; **33 groups had host bits the guest lacked**: area gates (block 2), T1
doors/shortcuts/boss state of other maps, every item pickup of the maps visited, T7 lamps lit and NPC state, plus a few
globals. So the guest saw unlit lamps (headstone list empty -> crash), un-picked items, closed gates of other maps.

**Fix** (`party_progress` "world mirror"; `WorldMirrorKey`, `HostWorldTracker`, `GuestWorldMirror`):

* Mirror groups (638): global types 0,1,5,6,7 minus blocks 6-8 (own pool in mode 1) and 60009xxx
  (C3's ledger); map types 1,5,6,7 of the 16 non-chalice table maps minus **T1 zones 4-6** (boss-fight
  state, enemy AI: the vanilla snapshot carries the current map's; a live copy would break fog and
  boss scripts). Bits never written: 2000-2099 (session flags the guest's own session code keeps)
  and 9020-9026 (chalice slot, set by SetLoadMode(1)). Not type 9, not area 29 / 40-47 / 99.
* Host: at the end of each scan round (BB_PARTY_PROGRESS_SCAN_MS, 1 s) one 150 KB read of its pool,
  diffed per mirror group. EVENT **`world_groups`** `{v, epoch, full, g:[[key, hex125]...]}`: a full
  image (non-zero groups, `full=1`: absent = zero) to each newly connected member right before
  `flag_snapshot`, then the changed groups (absolute bytes) to everyone.
* Guest: keeps the image; writes the overlay buffers directly (map +0x90, node key +0x20, buffer
  +0x28; key = GroupKey) - never the own pool, never through SetEventFlag:
  * **full copy** right after every vanilla apply (party_b2 K11 call site 0x17851CE calls
    `progress::OnVanillaSnapshotApplied()` after 0x13BECA0, before MoveMapStep's SetLoadMode(1) and
    the host-world load), again after the B2 overlay guard, and whenever the overlay's identity
    (head, root, leftmost buffer) changes: every masked bit becomes the host's;
  * **live**: each frame (also during loads) the bits the host changed since the last apply; the
    guest's own runtime bits in the overlay (its events in the host's world) stay.
* `BB_PARTY_WORLD_MIRROR=0` (guest) turns it off; `BB_PARTY_PROGRESS_MIRROR` (set-only C2 flags
  into the overlay) is unrelated and stays off.

**Live result** (same run with the mirror on): first join `full copy (right after the vanilla
snapshot apply, load mode 0)` wrote a few dozen overlay groups and a few hundred bits; B2 a similar count; live copies of
1 group / a few bits within ~1 s of host changes. Dump after the guest's map load: host and guest non-zero mirror groups equal,
**host-only bits: 0** except the session flags (masked by design). Guest-only bits (guest's own runtime, kept): the weapon-owner
and talk flags the guest's game sets at its load, and its own map event completions.

Live door flags: the host set a door's open flag in the Dream, then warped; the guest logged `world mirror: blocks changed by
the host` in load mode 1 within 0.3 s and its next dump had the flag in the visible (overlay) tree. (A door's model follows at
the next load of that map: "reproduce" events.)

**Visual check (2026-10-11):**
* **Boss (D1/D2): passes.** Test: the host has the Cleric Beast dead, the guest's own save has it alive; both stand in the
  arena: no boss, no boss bar, no fog on the guest's screen. Control (boss alive for both): the boss bar appears on the guest's
  screen within 6 s.
* **Doors: passes.** The host sets a door's open flag in the Dream, then warps; in the door's map the guest sees the double
  door **open**. Control (no flag): the same spot shows it **shut**. Earlier door picks were inconclusive (doors whose open
  flag is the event's own slot flag, or already open in the base save); a door that is visibly shut until opened still needs
  picking.

**Effects seen:** the guest's Hunter's Dream headstone lists the host's lamps and no longer crashes; choosing a lamp moves the party (guest_services.md
3.3). The world lamp prompt is **not** a flag question: see guest_services.md 3.3 (lamp type).

**Boss lamps and shared lamps (bosslamp079, CL-b457f5, 2026-11-05).** A lamp a boss's death lights reaches a guest's own
save from the host's world only together with that boss's defeat flag: all boss-defeat events' completion flags are
`boss_defeated` rows (set-only C2, host -> guest, the join snapshot carries them) and the lamp flags go in by C2 and the
guest's own lamp tick. Smoke `bq` (group `boss_lamp`): the host has the boss dead and the lamp lit, the guest's save has the
boss alive and the lamp unlit; afterwards the guest's party save holds the defeat flag and the lamp flags, so its own world has
the boss dead. The guest's own lamps in the other direction stay held by `HeldForBoss` (only with "Guests bring their lamps").
