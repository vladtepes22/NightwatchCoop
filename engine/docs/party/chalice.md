# Chalice Dungeons in party co-op

Bloodborne 1.09, our offsets (raw ELF VA of `eboot.elf`; bbhost = ours + 0x400000). Confidence:
**C** confirmed (code read / test), **L** seen live, **I** inferred, **G** guess.
Code: `gpu/shim/net/from_api_channel.{h,cpp}` (the channel board), `gpu/shim/net/from_api.cpp`
(ChannelId sign matching), `gpu/shim/party/party_chalice.{h,cpp}` (the local dungeon),
`gpu/shim/party/party_travel.*` (the chalice travel guard). Tests: `party-host-test`
(`test_chalice_channels`), `party-travel-test` (`test_chalice`).

## 1. What a Chalice Dungeon is, in the game's data

* **Chalices** are goods whose id is a `HolygrailExParam` row (C): 6109 Pthumeru Chalice (ritualLv 1,
  `requiredMaterialId` 109), 6209 Central Pthumeru, 6219 ..., `holygrailTypeId` 0..3 is the
  family (Pthumeru / Hintertomb / Loran / Isz), `mapBaseId` the map set, `fixedSf_* / freeSf_*` the
  sub-feature (rite) lots. `RitualRequiredMatParam[requiredMaterialId]` lists the ritual's materials
  and echoes: row 109 = 1000 echoes + Ritual Blood (1) (goods 7000) x2 (C, param dump).
* **Maps**: area 29 (`m29_BB_RR_00`, 300+ pieces); a dungeon is assembled at load time from pieces
  according to the slot's layout parameters (I). Area-29 AreaIds in signs are ordinary packed maps
  (`0x1Dbbrrii`), all in `party_ids.inc` (C).
* **Slots** (C, 0x1ab2970 / 0x1ab2b40 / 0x1ab2f70 / 0x1ab34d0 and their callers): a player has 7
  dungeon slots. The one-hot flags 9020..9026 select the current slot (`GetEventFlagValue(9020, 7)`,
  slot = 7 - bit length; event_flags.md 2). Slot k's **record lives in the event flag store**, in
  the type-9 group of area 40+k (ids `94000000 + k*100000`, block 0 zone 0, spilling into zone 1):

  | bits | content | writer |
  |---|---|---|
  | +32 / +64 | ChannelId low / high dword (`0x8000000000000000`: never uploaded) | 0x1ab2970 (helper +0xd8) |
  | +96..+127 | 4 bytes, stored high byte first (helper +0xe0) | 0x1ab2b40 |
  | +128 | 32 bits (helper +0xe8) | `SetEventFlagValue(+128, 32)` |
  | +160..+735 | 9 x {u32, u32} (helper +0xf8: the layout parameters, I) | 0x1ab2f70 |
  | +736 | 16 bytes (helper +0x140) | `0x13bcdf0` (byte writer) |
  | +992.. | 0x30 bytes (helper +0x158: `{2, 0x30, ...}`, an online id serialization, I) | `0x13bcdf0` |

  The glyph string (helper +0x150 -> object +0x180, wide) is normalised by 0x1ab34d0 (fullwidth
  `#%<>?\` and yen to ASCII) and stored too. The chalice groups (areas 40..47 / 99) are separate
  from the main world's (event_flags.md 2: the remap 0x13b89b0, the clear 0x13bdc00), so nothing
  dungeon-side touches main-world progress.
* **Singletons** (C): `SprjHolygrail` slot **0x553e9e8** = `{u32 count = 7, ..., +8 -> obj[7]}`,
  created on save load (0x156bc67 -> 0x1ac3ca0); each `obj[k]` (0x6a8 bytes, vtable 0x5336570,
  ctor 0x1ab4370, slot index at +0x100) holds a **CSDungeonRitualHelper** at **+0x108** (ctor
  0x1aa3110, step table 0x557e248).

## 2. The ritual helper (CSDungeonRitualHelper) — C

Steps (names at 0x49955ee.., functions registered at 0x1aaa978..0x1aaae0a): 0 Init 0x1aa64a0,
1 Wait_forRequest 0x1aa64c0, 2 Exec_forSuspend, 3 Exec_forResume, 4 Wait_forResume,
5 ExecUpload_forResume, 6 WaitUpload_forResume, 7 Finish_forResume 0x1aa76b0, 8
Exec_forSetupHolygrail 0x1aa79c0, 9 Exec_forJoin 0x1aa7c00, 10 Exec_forRandomJoin 0x1aa7e40, 11
Wait_forRandomJoin 0x1aa7e60, 12 Finish_forRandomJoin 0x1aa7f80, 13 Exec_forQuickJoin 0x1aa81d0,
14 Exec_forCancel 0x1aa8410 (the slot record reset to empty),
15..21 PresentOffering (offer materials: ChannelAddMaterial), 22..27 ChangeShareLevel, 28
ErrorRecovery, 29 StateSwitch 0x1aaa3d0, 30 Finish.

* Requests: `helper+0x260` = 1 Suspend, 2 Resume, 3 SetupHolygrail (the ritual), 4 Join, 5
  RandomJoin, 6 QuickJoin, 7 Cancel, 8 PresentOffering, 9 ChangeShareLevel, 10 ErrorRecovery;
  Wait_forRequest maps them to steps 2/3/8/9/10/13/14/15/22/28. `+0x264` = request in progress,
  `+0x25c` = state (1 idle, StateSwitch sets 2/3/4/5/6).
* Requesters: **Join** 0x1aa4a10(helper, list, index) takes a channel object from a search
  result list (refcounted, state at +0x110, ChannelId at +0x10, glyph wstring at +0x180);
  **RandomJoin** 0x1aa4cb0(helper) needs nothing else: it reads the player's unlocked depth per
  chalice family from flags 6221+k+7i, i = 1..9 (family k = 0..3), creates the random-join request object
  with 0x1e65580 (which sends **ChannelRandomJoin**) and starts step 10. Both refuse unless
  online (FrpgNetMan +8 set, +0x9f6/+0x9f8/+0xa50 clear), idle (+0x25c == 1) and no request
  pending. Callers: the altar menu (0x1abea30, menu event 2000 = decide; 0x1c0da40 functor).
* Join / random join end in 0x1aa7f80 / 0x1aa81d0: the channel object's data (0x1aa3b90 ->
  0x1a9c490: ChannelId, layout, glyph, creator) is written into **the helper's slot** record
  (above), then StateSwitch. So a joined dungeon becomes the slot's dungeon: generated locally from
  the record, exactly as the uploader's game generates it.

## 3. The server side: /channel/* — C (eboot), our answers

Builders and readers (from_api_channel.h has the summary). Request keys (C):

| API | path | request keys | reply the game reads |
|---|---|---|---|
| 12 Upload (0x1e9af40) | `/channel/upload` | HolyGrailTypeId, FormDataVersion (0), Status, FixedOrGeneral, FormData (base64), CharaId, ShareLevel, SubFeatureFlag, WishMaterialList [{WishMaterialMainTypeId, WishMaterialSubTypeId, WishMaterialCount, SpendMaterialCount}], RitualLevel | `ChannelId` (int), `DiscernmentWord` (string) (handler 0x1e804be) |
| 13 Share (0x1e9c0a0) | `/channel/share` | CharaId, ChannelId, ShareLevel | ResKind |
| 14 Search (0x1e9ee50) | `/channel/search` | HolyGrailTypeId / SubFeatureFlag / RitualLevel (null = any), FormDataVersion, Status, FixedOrGeneralList [{FixedOrGeneral}], GetCount | `ChannelList` [entry] |
| 15 WordSearch (0x1ea0420) | `/channel/word_search` | SearchWord, FormDataVersion | **the root object is one entry** (0x1e81c1c) |
| 16 GetDetailsInfo (0x1e9d5b0) | `/channel/get_details_info` | ChannelIdList [{ChannelId}] | `ChannelList` [entry] |
| 17 GetInfo (0x1e9cf60) | `/channel/get_info` | ChannelIdList [{ChannelId}] | `ChannelInfoList` [{ChannelId, Status, ShareLevel, TurnoutLevel}]; ResKind 0x1040a -> `LostChannelIdList` [{LostChannelId}] |
| 18 RandomJoin (0x1e9dbf0) | `/channel/random_join` | RandomJoinTargetList [{HolyGrailTypeId 0..3, RitualLevel}], CharaId, FormDataVersion | **the root object is one entry** |
| 19 AddMaterial (0x1e9c4d0) | `/channel/add_material` | ChannelId, MaterialList [{MaterialMainTypeId, MaterialSubTypeId, MaterialCount}] | `CompleteFlag` |
| 20 AddMaterialCompleteNotify | `/channel/notify_add_material_complete` | | ResKind |

An **entry** (reader 0x1e89da0, 0xE0-byte object): ChannelId (+0), ShareLevel (+8), Status (+0xc),
FormData (base64, decoded only when `FormDataVersion` == 0; +0x10 data, +0x18 size),
DiscernmentWord (+0x20 wstring), RitualLevel (+0x58), HolyGrailTypeId (+0x5c), FixedOrGeneral
(+0x60 = != 0, +0x61 = == 2), SubFeatureFlag (+0x64), CreateUserId (+0x70), CreateCharaId (+0x78),
CreateDate / LastPlayDate (`YYYY-MM-DDTHH:MM:SS`, +0x80 / +0x90), TurnoutLevel (+0xb0),
WishMaterialList (+0xb8). The bells (`summon_messenger/create|get`), tomb / blood messages and
wandering ghosts all carry a `ChannelId` too: the server keys everything in a dungeon by it.

Before this work the party answered `word_search` / `random_join` with `{"ResKind":0,
"ChannelList":[]}`, i.e. an entry with ChannelId 0 and no FormData — a "join" into nothing.

**Our board** (`ChannelBoard`, the party host's process; party-only by construction):
* `upload`: ChannelId = 52-bit FNV-1a of the FormData text, glyph = 8 chars of `[a-z0-9]` from
  another hash of it. The same dungeon (same FormData) keeps its id and glyph, also across host
  restarts and when a joined member's game re-uploads it; a different FormData under an existing
  id (collision / forgery) is refused. FormData must be strict base64, <= 256 KiB text,
  FormDataVersion 0. 64 channels max, 16 per member (least recently used dropped).
* `word_search`: by glyph (case-insensitive); unknown -> ResKind 0x1040a.
* `search`: the other members' dungeons matching the non-null criteria, newest first.
* `get_details_info` / `get_info`: by id; unknown ids are left out (never reported lost: a "lost"
  answer would make a game drop a dungeon it uploaded in an earlier session).
* `random_join`: the dungeon another member is in **now** (the ChannelId its bells last carried in
  area 29), else the newest other member's upload; never the caller's own; the target list's
  family / depth limits are ignored (the party joins its own dungeons).
* `share`: only the uploader changes the share level. `add_material`: `CompleteFlag` 0.
* **Shared fixed search** (live, the altar's ritual with a chalice): the game does not upload a
  "fixed" chalice's dungeon; it searches `FixedOrGeneralList [{2}]` with the chalice's
  HolyGrailTypeId / RitualLevel / SubFeatureFlag and **must** find one (an empty list -> 0x80000005
  -> msg 0x11f9 "Failed to connect to Bloodborne server" and back to the title). The board answers
  with a synthetic channel keyed `fixed:grail:level:rites` (same id for every member, also for the
  caller's own ritual) without FormData; the game then fetches it (get_details_info), writes the id
  into its slot record and generates the fixed layout locally. A later `upload` with
  FixedOrGeneral 2 reuses that id.
* **FormData** (reader 0x1a9c490; version 5 = 0x8C bytes, 4 = 0x7C bytes without the name):
  `+0 u32 version, +4 u32 size, +8 u32 (helper +0xe0), +0xc u32 (helper +0xe8), +0x10 18 u32 (9
  layout pairs, helper +0xf8), +0x58 16 bytes (helper +0x140), +0x68 creator name (v5)`. The slot
  record (event flags 94000000 + k*100000, written by 0x13bcdf0 as 32-bit chunks) holds the same:
  ChannelId +32/+64, +96, +128, +160..+735 the pairs, +736 the 16 bytes, +992 the name. The host's
  `party_chalice::Tick` rebuilds a version-4 FormData from its current slot's record and puts it on
  the board for the synthetic channel (`FormData from slot S's record (124 bytes) on the board`):
  otherwise the summoned guest's quick join fetched an entry with no layout and loaded an empty map
  (live run V: it fell to y -1500 and died).

## 4. Co-op inside a dungeon

* Vanilla matching is server-side (from_api_schema.md 5: no client-side area / password check on
  sign intake). The client sends `AreaId` and `ChannelId` with every sign and sign query.
* **The summoned game does not need the dungeon beforehand** (C, 0x14ba340 / 0x14bb200): the
  host's game sends its own summon message (the type-1 item over P2P, np_session.h) to the sign's
  owner; it carries the host's room data and, at +0x2c, the **host's ChannelId**. The guest's
  handler 0x14ba340 stores the room data in its sign entry (+0xb8, state +0xc8 = 2) and, for a
  ChannelId other than `0x8000000000000000`, creates a channel request by id (0x1e65290 ->
  **get_details_info**) in +0xc0. When the session join proceeds (0x1872360 -> 0x14bb200), the
  guest's game hands that channel to **slot 0's helper** (`SprjHolygrail.obj[0] + 0x108`,
  0x1ac3ef0 -> request 6 0x1aa5280 -> STEP_Exec_forQuickJoin 0x1aa81d0): the FormData is parsed
  (0x1aa3b90 -> 0x1a9c490) into the helper and written into the slot-0 record, then StateSwitch
  (0x1aaa3d0) sets the helper state (+0x25c): **5** when the channel object's +0x54 == 1 (or 3
  with +0x10c bit 0), else 3. 0x1ac3ef0 succeeds only with state 5; with 2..4 it requests 7
  (STEP_Exec_forCancel 0x1aa8410: the slot record reset to empty) and the summon goes on without a
  dungeon. The map load (0x1928680) reads the current slot's record (+96 = the dungeon map
  0x1dxxxxxx) and otherwise loads m255 (an empty world: the guest falls and dies).
  The channel object's +0x54 is the entry's `Status`, copied only when `ShareLevel` != 0
  (0x1e6dea2); `CompleteFlag` of add_material sets it too. Slot 0 is the network / joined-dungeon
  slot; a member's own ritual dungeons use the others (I: the requesters always pick obj[0]).
* **What the board answers for it** (live runs V..#13): the entry must carry the dungeon's
  **FormData** (the host's game never uploads a shared fixed dungeon; party_chalice puts a
  version-4 FormData built from the host's live ritual helper on the board, `FormData from slot
  S's record (124 bytes) on the board`), and a **ShareLevel != 0 with Status 1** for the summoned
  member. The member performing the ritual of that dungeon (its fixed search within 120 s) gets
  the stored ShareLevel 0: with 1 its own ritual generated a local dungeon (ChannelId with the high
  bit set) instead (run #12).
* **Load mode 1**: SetLoadMode(1) (0x13bb590) moves the flag groups onto other storage and only
  carries the slot bit 9020+k. party_chalice replaces it to also carry the quick-joined slot
  record (snapshot before, written after; only within 180 s of a quick join; transient storage,
  never the member's save): `load mode 1: slot 0's record (dungeon N, map 0x1d0a5a00) carried
  over`. The map load itself already happened in mode 0 (the record is there then); this keeps the
  record readable for later loads in the host's world.
* **The current slot is not always slot 0.** The quick join writes slot 0's record, but the map
  load reads the record of the current slot (`GetEventFlagValue(9020, 7)` at 0x192876A, only for
  an area-29 map). A member whose own ritual dungeon was selected (slot 1..) loaded m255 and was
  sent home (live: `slot 1's record holds no dungeon`). party_chalice retargets that one read to
  the quick-joined slot while its record holds a dungeon (`map load: the quick-joined slot 0's
  dungeon, not the current slot 1's`); the member's save keeps its own selection. The retarget
  stays on for the later loads in the host's world (load mode 1 with the dungeon carried), and an
  own-world load (load mode 0) ends it, also after a quick join that never loaded. Nothing is
  written under the current slot: the slot records are not part of the load-mode-1 storage, and a
  copy written there replaced the member's own slot-1 dungeon in its save after a summoned save
  (live test); only the quick-joined slot's own record is carried.
* **The in-game "current dungeon" read too** (0.7.3): the in-game step 0x193ac10 writes FrpgNetMan
  +0xa88 (the ChannelId signs, messages and summon messages carry, read e.g. by 0x145a250) every
  frame from the current slot's record (online, not 9f6/9f8/a50, area-29 map:
  `GetEventFlagValue(9020, 7)` at 0x1941B05 -> 0x1ab2a40), and the map load step 0x1937570 sets
  FrpgNetMan +0xb14 bit 1 from the current slot helper's channel object (+0x1d8 +0x10c bit 0; read
  at 0x1937E7B). After the map load retarget a summoned member with its own slot 1.. stood in the
  host's dungeon with its OWN ChannelId in +0xa88. Both reads are retargeted the same way
  (`InDungeonSlotHook`, only while the host's dungeon is carried; logged once per change: `in the
  host's dungeon: the game's current dungeon (FrpgNetMan +0xa88) is the quick-joined slot 0's`).
  No flag is written; a load mode 0 load ends it and +0xa88 follows the own current slot again on
  the next frame. Not retargeted: 0x13d2910 (GameDataMan +0x30, the saved position's slot),
  InGameStart 0x193a3e0 (compares that record's slot with the current one), the ritual helper
  steps (0x1aa9500, 0x1b038b0).
* So what the party needs is that **get_details_info finds the host's dungeon**: the host's game
  uploads it when it performs the ritual / resumes it online (helper steps 5-6, ChannelUpload), and
  the board keeps it. **Our rule** (FromApi::sign_get): a host whose sign query is in area 29 gets
  the members' signs (wherever they rang: the Hunter's Dream, the world, another dungeon) only while
  its ChannelId is on the board; otherwise none ("held back" in the log) — the summoned game would
  build its own slot's dungeon. Outside the dungeons nothing changes. The board notes each
  member's current ChannelId from these calls (`random_join` uses it).
* **The summon message carries the ChannelId only for a waiting guest** (C): the host's game fills
  +0x2c from FrpgNetMan +0xa88 only in area 29, and the guest's 0x14ba340 takes it only with
  FrpgNetMan +0xb14 bit 0 set (from +0xb9c bit 0 = SpEffect stateInfo 269 = **SpEffect 9900**, the
  vanilla Dream bell's "chalice wait"). The party rings with its own bell patch (9005), so the
  director also applies 9900 to a guest that rings while the host's roster map is in area 29, and
  to a guest whose sign is already out when the host enters its dungeon (run #10: the guest rang
  ~10 s before, the summon then carried no quick join) (live: `summon message type 1, host map
  0x1d0a5a00, ChannelId 0x3f22b458bd799` -> two get_details_info -> quick join).
* **The direct summon carries the ChannelId** (2026-10-21, after PR #8): the party's direct summon
  (party_b2 `DeliverSummon`, from bbhost) builds the type-1 message on the guest itself, and it
  always wrote "none" at +0x28 (buffer +0x2c). Live (host from the Dream entrance, lamp 2902950;
  guest without the dungeon, sent home by PR #8): `summon message type 1, host map 0x1d0a5a00,
  ChannelId 0x8000000000000000` -> no quick join -> map "-" (m255), y -581 -> -1500, HP 0. The
  runs that worked used other paths: run #13/#14 the bell sign, where the host's own game sends the
  message with +0x2c from FrpgNetMan +0xa88; g1m/g1b3 a guest whose own slot held the same
  dungeon (the travel replay / title join built its own identical copy). Now the host's
  `dsum_room` carries `channel` (FrpgNetMan +0xa88 in area 29) and `BuildPayload` writes it; the
  host refuses with the wait word `chalice` (the guest asks again, as for `settle`) while +0xa88 is
  still 0 (live ~1.5 s after the arrival) or not on the board; the guest never hands a dungeon room
  without one to its game (`RoomLacksDungeon`: stays home, notice) and sets FrpgNetMan +0xb14 bit 0
  for the call (the summon probe does the same for the bell path). A title joiner is refused into a
  dungeon unless its `title_req` names the host's ChannelId (its own slot's). Last net:
  `ChaliceEmptyWorldTick` - a guest in the session whose map stays unknown for 3 s while the
  host's roster map is in area 29 leaves the session (home load) with a notice.
* **No gather into a dungeon**: GatherGuest skips the gather while the host's map or lamp is in
  area 29 (it warped the guest to the host's dungeon lamp = its own slot's dungeon).
* **The automatic flow**: host and guests are in the Hunter's Dream (B1 travel follows the host
  there); the host performs the ritual / enters its dungeon at the altar (the upload reaches the
  board); the guests stay (below) and keep ringing the Small Resonant Bell (PartyDirector); the
  host's director rings the Beckoning Bell inside the dungeon; the host's game summons; each guest's
  game quick-joins the host's dungeon by ChannelId and loads in next to the host.
* **Travel** (party_travel, B1): a host warp into area 29 (packed map, exact-position map, lamp
  `29xxxxx`, bonfire entity, a host death / Mark respawning at a dungeon lamp) carries the host's
  ChannelId (`"chalice"` in the travel event). A guest replays it only when its own current slot
  holds that ChannelId (`ChaliceReplayAllowed`); otherwise it stays where it is and is summoned
  (above): a warp into area 29 would generate the guest's own slot's dungeon (or none) — a
  different layout. The guest death / Mark redirect to the host's last lamp skips a dungeon lamp of
  a dungeon the guest does not have. `Party: chalice: slot S, dungeon N, load mode M` logs each
  game's current dungeon (party_chalice.h reads it from the flags).
* **4-player / scaling**: the m29 scripts already use the multiplayer doping (four_players.md,
  `Lua_MultiDoping`, 7501/7502).

## 5. Progress and rewards

* **Chalices for everyone** (party_chalice.h): the fixed chalices (goods 6109..6539 ending in 9:
  HolygrailExParam rows with isGenericHolygrail 0; isOnlyOne, never consumed by a ritual) any member owns go to every member
  that lacks them (`chalice_goods` event, every change and every 60 s): a guest sends its list to
  the host, the host gives itself the missing ones and broadcasts its (now merged) list, and each
  guest gives itself the missing ones in its own world (load mode 0; GiveItemDirect). Whichever
  member's dungeon the party clears, every member can perform the next ritual itself, and the host
  can open the altar menu for a dungeon a guest made (the altar refuses everything with "A Holy
  Chalice is required to conduct a ritual." when the player owns no chalice, run c16). A peer's
  list is filtered to the 10 fixed chalice ids (no other goods can be given this way). Root Chalices
  (isGenericHolygrail 1, the ids not ending in 9) are used up by their ritual and are not shared (0.7.1:
  before, another member's copy gave a used one back); each player gets its own from the boss (ItemSource).
* Dungeon flags (layer doors, boss kills, treasure in area 29, 1100xxxxx / 2001xxxxx lots) live in
  the **slot's own groups** (areas 40..47 remapped from area 29 by the slot), so a summoned guest
  (load mode 1, the host's overlay) never progresses its own copy, exactly like vanilla. They are
  never written from outside (party_progress `NeverWritable`, items.md: Chalice lots excluded).
* Insight (parg, 0.6.2): the dungeon's boss Insight is not common 9350 but m29's own events
  (12901718/722 first encounter, 12901708/712 kill table, 12901588/589/591 the layer boss's +2),
  bare SpEffect 4680 that end on a client; the host's are counted and sent to the guests
  (party_phantom DirectInsight; live by the `host4680` probe, not yet with a real boss).
* What a guest gets as a cooperator: boss kill echoes / Insight (and the C3 parity patches' full
  rewards), the drops it picks up itself, and (0.6.2) **the host's boss rewards**. Boss reward lots
  in the dungeons (Ritual materials, the chalice drops) are host-only `2003[4]` awards, flagless and
  repeatable per dungeon, so C3's flag / ledger replay cannot give them. Instead the host's award hook
  turns each one into an `ItemSource::Chalice` grant (party_items `MakeChaliceGrant`): exactly the
  items the host's roll gave, no flag, no ledger bit, its own key per kill; it goes out in the
  normal `items` event, and a guest takes it only while it is summoned in a Chalice Dungeon (load
  mode 1, map area 29: `ChaliceGrantAccepted`), i.e. the guests present at the kill. The guest
  gives it in the dungeon (GiveItemDirect, the native item-get box), writes **no flag** (dungeon
  flags are the slot's own), leaves key goods out (a chalice reaches everyone through the chalice
  share above) and never carries it home (a give that cannot happen in the dungeon is dropped).
  Once per kill: the host's event awards once; the guest's own copy of the event runs its client
  branch as in vanilla (its `2003[4]` gives nothing on a client; the `2003[36]` "client too" lots of
  12901596 / 12901693, e.g. 110001000, still give the guest its own consolation roll, as before). `BB_PARTY_CHALICE_AWARDS=0` turns it off. Unit: party-items-test `test_chalice_award`.
* **Why no flag replay into the guest's slot**: the guest's quick join cleared its slot-0 groups and
  rebuilt the record; writing the host's area-29 flags there would need the guest's slot mapping
  (9020-9026 one-hot, remapped by 0x13b89b0 on every load) and would bypass the game's own slot
  bookkeeping (0x13bdc00). Too risky for a save; not done. Main-world flags are never touched by
  any of this: the chalice groups are separate (event_flags.md 2).

* **Vanilla side effect, kept (decision 2026-10-15, stats audit finding 8)**: the quick join
  rewrites the summoned game's **slot-0** record and clears the current chalice groups (0x1aa8410 ->
  0x13bdc00): a member summoned into a chalice dungeon loses the progress of the dungeon it had in
  slot 0, exactly as in the original game. Slot 0 is the network / joined-dungeon slot; a player's
  own ritual dungeons live in slots 1.. (the fixture `chalice_pthumeru_ritual` is slot 1), which the
  join does not touch, so what can be lost is only an earlier *joined* dungeon's layer progress,
  as in vanilla. Protecting it would mean saving and restoring the slot-0 groups around the join
  (0x13bdc00's bookkeeping, 9020-9026 one-hot slot mapping): a risk to the save for little gain, so
  not done; revisit if players report it.

## 6. The altar objects (for automation later) — C

* `SprjHolygrail.obj[k]` is a **SprjDungeonGateIns** (the per-slot altar object, step table
  0x557e840, steps registered at 0x1abf510: Init 0x1ab5370, ChannelLoad, ModeSwitch 0x1ab5490,
  DispError, NoHolygrail 0x1ab5750 (the altar menu: its decide handler 0x1abea30 calls the random
  join), LocalRitual 0x1ab71c0, ServerRitual 0x1ab7740, LocalDungeon 0x1ab7a50, ServerDungeon
  0x1ab7d00, Finish). `+0x364` = mode (0..6 -> steps 1/8/11/14/17/20/5), `+0x36c` = helper busy,
  `+0x368` = the request it forwards to its helper (`+0x108`).
* The ritual request 0x1aa47a0(helper, ritual) takes a ritual descriptor the altar menu builds
  (0x1ab9270 / 0x1b038b0; chalice id at +0x10) and the creator's name (PlayerGameData +0x5d0).
* The test fixtures used to get there: `BB_PARTY_DIRECTOR_TEST=goods=6109:1,goods=7000:2,
  echoes=5000,log_pos,place=MAP:X:Y:Z:YAW,warp=MAP:POINT` (party_director.h). The Ritual Altar
  part (`アイテム_祭壇`, entity 2111101) stands at (136.19, -19.17, 139.77) in m21_01; its action
  region 2112999 (box 2.2 x 8 x 1) at (131.77, -19.96, 140.54).

## 7. Live evidence and open points

* **Run #13** (2 instances, `chalice PARTY #13`): the host (Hunter0) performs the Pthumeru
  Chalice ritual at the First Ritual Altar (MenuScript), its search makes the shared fixed channel
  1110692594440089 (glyph wmqbhonw), it enters m29_00 and the board gets the FormData from its
  helper. The guest (Hunter1, Hunter's Dream) rings with 9900, is summoned: `summon message type
  1 ... ChannelId 0x3f22b458bd799` -> `quick join request` -> `STEP_Exec_forQuickJoin` -> `slot 0,
  dungeon 1110692594440089 ... gate mode 5` -> `load mode 1: slot 0's record ... carried over` ->
  map m29_00_00_00, position (40.4, 31.1, -91.0) steady, cooperators 1, roster in_host_world in
  the host's dungeon until the run ended. **Run #14** reproduced it with screenshots: both games in the
  Pthumerian Labyrinth, the host sees the guest phantom (Hunter1) in the corridor ahead.
* Before the fixes: run V/W the guest fell through an empty map (m255: no FormData, then the
  quick join cancelled by state 3), run #10 the guest loaded the host's Dream copy (no 9900).
* A guest that ends in its own copy of the dungeon (e.g. it entered the same glyph itself) is not
  synchronised to the host's progress there (section 5).
* The vanilla quick join overwrites the summoned member's slot-0 record (section 5).
* The exact meaning of ShareLevel / TurnoutLevel values, FixedOrGeneral 1 vs 2; uploaded (root /
  "general") dungeons with pending rite materials (Status 0) are not joinable by a summon (as in
  vanilla: +0x54 stays 0).
* **Full clear of a layer, 2026-10-11 (runs c11-c15, 2 instances, 1792 MiB each)**: host ritual at
  the First Ritual Altar (MenuScript), guest summoned (bell_host@29 / bell_guest@29); lever
  12900192 and the layer door by host_press; layer boss 2900100 by host_kill: PREY SLAUGHTERED on
  both, `K10: boss-clear leave suppressed` for host and guest, the guest's send-home at +0x13859df
  dropped; guest award lots 131302160 / 110001000 "in another's world" (item-get on screen).
  The post-boss lift 2901004 (EMEVD 12900354 / 12900351, regions 2903020 top y 31 / 2903021 bottom
  y 10): host on the car at (-65, 31.6, -91), guest at (-64.6, 31.6, -90.4) -> both ride 31.5 ->
  10.5 in 6 s (log_pos, c15). Warping 114 m straight from the lamp into the empty shaft (c13) fell
  to death (the car rests at the bottom until called); the call lever 2901005 did not react to a
  Cross from (-58.5, -90.6). host_lamp=2102961 -> both in the Dream with one load each, the guest
  re-joined (cooperators 1).
* **A ritual by a guest** (user decision: anyone performs the ritual, the party enters together).
  The guest performs the ritual in its own Dream; the host enters that dungeon from its own altar
  with one menu choice, and the guests are summoned as for a host ritual. Pieces:
  - the chalices go both ways (section 5): the altar refuses everything without one (c16:
    "A Holy Chalice is required to conduct a ritual.");
  - the board remembers each member's last ritual (`latest_ritual`: its fixed search or upload);
    while another member's dungeon is newer than an hour and not in the host's slot, the host's
    altar item 262002 "Search by Chalice Glyph" reads **"Enter the party's dungeon"** (help: "Enter
    the Chalice Dungeon <member> made (glyph ...) with the party.") through a text override on the
    game's message lookup, and its 8-character glyph box is confirmed at once with that glyph
    (`bbgpu_ime_preset`; logs `party dungeon at the altar: ...`, `the altar's glyph box gets the
    party dungeon's glyph ...`). No typing;
  - the guest sends its slot's FormData (`chalice_form` event; a shared fixed dungeon is never
    uploaded): without it the host's join did nothing (c18);
  - the glyph answer carries a ShareLevel (as get_details_info does): with the stored 0, "Join the
    selected Chalice Dungeon?" -> YES returned to the altar without a load (c19);
  - a guest outside a dungeon no longer follows the host's warp into its own copy of the same
    dungeon (it was then summoned out of it again: two loads, c20); it waits for the summon;
  - the IME buffer holds maxTextLength + 1 units (the glyph lost its 8th character, c17).
  **Run c20** (2026-10-11): guest ritual -> host altar "Enter the party's dungeon" -> OK -> the
  result -> YES -> altar -> "Select location to awaken in" -> m29; guest summoned by quick join;
  boss kill (K10 both), guest's own lots, the lift down with the host (31.5 -> 10.5). c23 repeated
  the entry up to the join. Still more presses than needed: the search result, YES and the
  awaken prompt could be taken automatically.
* **A dungeon from an earlier session** (c22): the board is new each session, so a host entering a
  dungeon of an earlier ritual had no ChannelId on it and the members' signs were held back. The
  host's game now adds it (`... is in chalice dungeon N (not on the board yet): added`); the guest
  was summoned (c22, host started from a post-ritual test save).
* **The lift call lever** (c22): the guest, in the host's world at the top, pulled 2901005 from
  (-58.5, 31.6, -90.1) facing +z while the car was at the bottom with the host: EMEVD 12900351 ran
  and the car came up to the guest. The earlier "did nothing" (c13/c15) was a press while the car
  was already at the top (the car starts there) or from 0.5 m further back.
* **Layer 2**: the next layer boss 2900200 (MSB (56.3, -59.7, -35.2)) is not loaded at the bottom of
  the first lift (host_kill: NOT loaded); a real descent through layer 2 needs its route (not done).
  Lamps 2902950..2902959 are the dungeon's ReturnPoints; the lamp objects are **2901950 + n** (checked
  live, chalamp probe: log_obj): 2901950 (40, 31.5, -54) layer 1 start, 2901951 (37, 31.1, -92.2),
  2901952 (-48.2, 30.6, -93.4) layer 1 boss room (return point 2902952, talk t290112), 2901953 (-106,
  10.1, -89.8), 2901954 (-132.7, 17.4, -15.5), 2901955 (-128, -10.9, 53.2), 2901956 (24.5, -17.5, 49.5),
  2901957 (98.7, -59.9, 50), 2901958 (95, -66.5, -115.5) (Pthumeru Chalice layout, chalice_pthumeru_inside).
* **Since 0.5.2 work (2026-10-11, runs c24-c26)**:
  - a guest's current dungeon (from any session) reaches the host's board through its FormData event
    and becomes the party's dungeon at the host's altar (`dungeon N of Hunter1 (slot 1): FormData
    added to the board`, `party dungeon at the altar: ...`, c24b/c24c: verified up to the altar;
    both runs were ended early by the central idle reaper before the host's entry);
  - "one press": `BB_PARTY_ALTAR_AUTO=1` answers the prompts after the glyph box (OK, the result,
    YES, the altar, the first awaken point). **Off by default: not verified live** (no run reached
    it before the 0.5.2 freeze);
  - the bottom lift lever 2901006 (c26b, from (-71.5, 10.6, -91.9) facing -z and (-71.5, 10.6,
    -93.5) facing +z) showed no lift call; the guest's top-lever call (c22, c25) did not repeat in
    c26b (the guest stood at z -90.5 after the warp, 0.4 m further back than in c22). Open;
  - a guest outside a dungeon that holds its own copy no longer follows the host into it (one load
    instead of two): code in place, not confirmed live (c24c ended before the entry);
  - screenshots c26_* (entry, boss, lift, both instances): item-get boxes on both, the boss-clear
    banner fading on the guest; no floating bars or tags, no ghost characters.
  - test harness: the central reaper (tools/mp/reaper.py) ends runs whose party log lines do not
    change for 10 s; a `<root>/noreap` file with seconds lets the long ritual / altar phases pass.


* **0.6.2 live, one-press entry with the guest in the host's Dream (parfin2 g1d-g1m, 2026-10-15)**. Fixtures:
  the guest `chalice_pthumeru_ritual`, the host the default save (docs/party/test_saves.md). Since seamless
  joins the guest stands in the host's Dream before the dungeon, which needed four fixes found live:
  1. the guest title-joins and never stands in its own world, so it never sent its FormData: now it also
     sends it while summoned from its own ritual slot (1..; slot 0 in load mode 1 is the host's joined dungeon);
  2. the altars' talk scripts t210050..t210110 park in an empty machine while IsMultiplayerInProgress is 1,
     for the host too (no "First Ritual Altar" prompt with a guest present): answered 0 for the host
     (`BB_PARTY_HOST_ALTARS=0`: as vanilla); guests keep the vanilla block;
  3. the director's test menu walk held the pad at 0 every frame and released the altar autopilot's Cross;
  4. the awaken point's warp 2902950 runs from the altar's EMEVD without a lamp flag of the host's, so the
     split rule dropped it as another member's lamp: a host's area-29 awaken point is its own warp now (no
     "Travel with the party?" question; the guests come by the party travel / summon).
  g1m: altar item "Enter the party's dungeon" -> the autopilot (glyph box, OK, Search Results, YES, the
  altar again, "Select location to awaken in", Chamber of the Seal; the awaken menu's first Cross is often
  swallowed, so it presses up to three) -> one load for the host; the guest followed in one load and stayed a
  cooperator; layer boss 2900100 killed: PREY SLAUGHTERED on both, guest Insight 6 -> 9, each its own loot.
  The bottom lift lever 2901006 showed no prompt from the c26b spots, and the layer 2 boss 2900200 was not
  loaded at (56.3, -59.7, -35.2): both stay open.

* **0.6.2 live, the lift levers, layer 2 and the host-only award (parfin3 g1b3-g1e, 2026-10-15)**. Each run is
  one short session: the host's party save stays inside the dungeon (fixture `chalice_pthumeru_inside`), and the
  guest title-joins straight into it (g1c: beside the host, one load).
  - **The lift levers** (EMEVD trace of 12900347 / 351 / 354 / 357 on both games): the post-boss lift is car
    2901004 with call levers 2901005 (top, (-58.5, 31.5, -89.3)) and 2901006 (bottom, (-71.5, 10.5, -92.7)), flags
    12900506 (car down) / 12900507 (moving), regions 2903020 / 2903021, ObjAct events 12900000 / 12900001. The
    init 12900347 and every ride enable only the lever that can call the car (2005[6] ObjAct 9902 on / off), so
    the bottom lever shows no "Operate Device" while the car is down: the earlier no-prompt runs (c26b, g1m) had
    the car at the bottom. With the car up, a Cross at the bottom lever from (-71.5, 10.5, -91.7) ran 12900354
    (car down) on the host and the same on the guest's copy 0.05 s later; the top lever then ran 12900351 (up),
    the bottom lever 12900354 again. The other two lifts of this layout: 2901010 (levers 2901011 / 12, flags
    12900512 / 13) and 2901024 (levers 2901025 / 26, 42 m, 12900353 / 356).
  - **Layer bosses of this layout** (the generator's ids, not the MSB template's 2900200): 2900100 layer 1 (HP
    3175, (-39, 30.5, -91)), 2900107 layer 2 (HP 3792, (6.5, -17.6, 52.0)), 2900112 layer 3 (HP 4057, (97.5,
    -64.5, -97.5)); `host_kill=ID` with an unknown id lists the strongest loaded enemies. All 174 enemies of
    the three layers are loaded at once (one map, m29_00_00_00).
  - **Layer 2's boss** (g1d, HP 0: the game's own death and defeat): boss clear on both games (2612 Echoes
    each), the kill table 12901708 (+2, relayed) and 12901693's lots: the host's `2003[4]` lot 80109010 (hostOnly
    1, flag 5001: the Central Pthumeru Chalice 6209; corrected by parlast) went to the guest as an owed key item for its own world (and to the
    chalice share), the `2003[36]` lots gave each player its own roll (host 150100000 / 110001010, guest 150102000 /
    110001010). Both layer bosses pay through 12901693; 12901596 / 12901598 did not run in this dungeon.
  - **The host-only area-29 path** (g1e): `BB_PARTY_ITEMS_HOST_TEST=131300160` inside the dungeon (the game's
    award function with hostOnly 1, a flagless dungeon lot): the host's `chalice` grant, the guest `chalice award
    lot 131300160 from the host: queued for this guest (in the dungeon at the kill)`, the same Adept Blood
    Gemstone given in the dungeon (item-get box on both screens).
  - The test walk to the First Ritual Altar: once `prompt=` showed after a facing tap, the approach no longer
    turns the player again (that turn lost the prompt in g1a / g1b / g1b2; the altar opened at the first Cross in
    g1b3).

* **0.6.2 live, the walked lift route, the natural host-only lot and the floor-boss events (parlast g1w-g1z,
  2026-10-15)**.
  - **12901596 / 12901598 are not in the retail game.** A byte scan of every EMEVD finds them (and their family
    12901594 / 595 / 597 / 730) called only from the debug / prototype scripts m29_01_*, m29_02_00_*, m29_03_20_*,
    m29_05_00_* and m29_05_10_*, with empty placeholder lots (30401000, 30401300, 30401600, 75001000, 75001500:
    category -1). Each chalice's `HolygrailExParam.mapBaseId` picks its script folder (109 -> m29_10_90_00 for the
    Pthumeru Chalice: the live dungeon's lots match it), and every reachable script pays its bosses through
    **12901693** (boss flag, A..F): both roles get `2003[36]` A/B (host) or C/D (client, D under the Curse flag
    92905370) and the depth consolation lot 1100x1000 / 1100x1010; the host-only `2003[4]` E / F depend on the
    region the host stands in at the kill (12907210+N = the host in 290296N; case 92905378: 2902961 -> E,
    2902962 -> F; case 92905385: 2902962 -> E, 2902963 -> F; the two case flags look engine-set,
    92905300 + DungeonFeatureParam.eventflagId). For the Pthumeru Chalice E = 80109000 (Pthumeru Root Chalice
    6100, flag 5020), F = 80109010 (Central Pthumeru Chalice 6209, flag 5001: the lot g1d saw; earlier notes
    called it the Root Chalice). No retail boss pays a flagless host-only lot: the `ItemSource::Chalice` path
    keeps its test-aid evidence (g1e).
  - **The E lot live** (g1x, g1z): the boss trio of 12901801 (2900101 / 2900103 at (-134, 17.3, -21) / (-125, 17.3,
    -16.5), 2900105 at (-130, 17.3, -27.5), event 12901707) killed with the host at (-127, 17.6, -28): `award lot
    80109000 (hostOnly 1, flag 5020)`, the Pthumeru Root Chalice in the host's item box, the guest `lot 80109000
    ... key item / unique goods 6100: not given while summoned; waits for the own world`.
  - **Walked down the post-boss lift together** (g1x): from the layer 1 boss room both walked (route 0 hops, then a
    stick walk onto the car at (-65, 31.6, -91)), the car took both down (31.5 -> 10.5), cooperators 1 throughout.
    A member who arrives after the car left walks into the open shaft and falls (g1y: the guest died; vanilla).
  - **K10c**: the guest's boss-clear Lua state scheduled the farewell message ("The final battle has ended. It is
    time to bid farewell.", event 0xFF9 `RecallMenuEvent_MisssionClear_forNormalCoop`, call 0x13855DB) although
    the party stays; it is skipped while the party is on (`BB_PARTY_BOSS_FAREWELL=1`: vanilla). g1z: no box.

## 8. The dungeon lamps in a party (chalamp, 2026-10-21)

A tester's report (a derivative build, 2 players): the lamps could be lit but did nothing else; a guest that
died was sent to the Hunter's Dream and could not get back in until the host restarted; the host died, got up
"at the first lamp" and could not leave. All three reproduced from the logs and fixed on master:

* **The lamps (C, logs + live)**: a dungeon lamp is talk **t290110 + n** (lamp 2902950 + n; ids in
  `m29_00_00_00.talkesdbnd`), not one of the world lamps' tAAB000..003 (`LampTalk`). Its "Return to the
  Hunter's Dream" sets 72900100 + n; m29's event 12907020 then warps to the Dream's altar spot **2102954 +
  slot** (one per slot flag 9020..9026), not 2102950. Its talk tests IsMultiplayerInProgress / IsClientPlayer
  shut the lamp while anyone is summoned (the tester's host log: `talk t290114 asks IsMultiplayerInProgress
  (left as is)`). party_guestsvc `ChaliceLampTalk` answers both 0 in party mode, for host and guests
  (`... (Chalice Dungeon lamp: answered 0)`); no setting (an off would bring the softlock back; a
  `BB_PARTY_CHALICE_LAMPS=0` escape hatch for tests only). The lamp's warp then takes the party's usual
  path (split question / host-led travel; a guest's becomes its lamp request). Lamp travel's warp menu does
  not apply in area 29 (no table lamp within 15 m: the vanilla question), so `BB_LAMP_TRAVEL` makes no
  difference here. Travel between two dungeon lamps is the altar's "Select location to awaken in" (vanilla).
* **The host's revive spot (C, live)**: party_ids' ReturnSpot table comes from the m29 template MSB; a
  generated dungeon puts its lamps elsewhere (tester: lamp 2902952 lit in the boss room at (-48, 30.5, -93),
  the revive put the host at the table's (-127.2, 20.4, -20.0)). `deathrules::LampTableSpotTrusted` keeps
  area-29 lamps out of the table; `LampSpotTick` records the spot where the host stood when its last-lamp
  record changed to a dungeon lamp in the world (`lamp spot: Chalice Dungeon lamp N lit / used ...`), and a
  host death with no seen spot takes the host's lone load to the lamp (the game's own placement; the guests
  stay and are re-joined, live cl2).
* **A guest's death (C, live)**: outside a boss fight a dead guest went to its OWN last lamp (the Dream:
  an alone load) and stayed apart; a dungeon is entered only by the summon, its game's send-home is dropped
  while apart, and the host's travels into the dungeon are "not followed", so nothing brought it back. Now
  (bbport.ini `party_chalice_rejoin`, System > Party rules: Travel, launcher; default on) it gets up beside the
  nearest living member, no load (`guest: died in the host's Chalice Dungeon ...: up again beside the nearest
  living member`); and `ChaliceApartTick`: a guest still in the session but apart in a map outside area 29
  while the host's roster map is in area 29. **User decision (option b)**, `deathrules::PlanChaliceApart`:
  after a death or a forced trip (a death respawn, a scripted warp / kidnap, a host travel it could not
  follow) it leaves the session after 6 s (`chalice: this guest is apart ...`) and its bell's summon
  quick-joins it in again; after its OWN choice (party_split `Carry` alone at a lamp / headstone, its own
  Hunter's Mark: `NoteChosenApart`, `chalice: the guest's own choice ...`) it stays out with a one-time
  notice "The host is in a Chalice Dungeon: Party > Rejoin to go back". Party > Rejoin then leaves the
  session first (`ChaliceRejoinPressed`, `deathrules::ChaliceRejoinLeavesFirst`: still a session client,
  apart, host in a dungeon), and the bell's summon brings it in. A death clears the choice; so does the
  host leaving its dungeon (a later host trip into one is then a forced trip) or the guest back in it.
  Party > Leave was already a choice (out of the session, no bell until Rejoin).
* **Smoke `chalamp`** (phase cl, host fixture `chalice_pthumeru_inside`, guest the default save; cl5 16/16;
  with option b's case cl8 23/23: guest 1's lamp to the Dream answered alone (`BB_PARTY_TRAVEL_CHOICE=alone`
  on it) -> stays out with the notice 20 s, no send-home -> `guest_rejoin:now` (new: the press while still
  in the session) -> home first -> summoned into the dungeon):
  light 2901952, guest death (up beside the host), host death (revive at (-49.4, 30.5, -93.4)), the lamp to the
  Dream (both in m21_00, party travel), `host_lamp=2902952` from the Dream (the altar's warp: host in alone,
  the guest apart -> home -> summoned back, both in m29 at the lamp). Not run live: a guest's own use of a
  dungeon lamp (its talk answers 0, `t290112 asks IsClientPlayer (Chalice Dungeon lamp: answered 0)`).
* **chalicelamp (0.7.3, two guest reports, derivative-build logs)**:
  - **A guest's own dungeon lamp use** (cause confirmed from the data): the lamp menu's "Return to the Hunter's
    Dream" set 72900104 three times on the guest and nothing followed. m29's 12907020 starts with `1003[6]` (end
    the event on a client, state 1), the same guard as the world lamps' 7200; `ChaliceLampSkipRules`
    (`12907020:1003:6@0`, with the guest lamps and the dungeon lamps on) skips it as `LampSkipRules` does for
    7200, so the guest's 2003[49] runs and goes through `GuestLampIntercept` (split question / lamp request).
  - **The fall revive "above the dungeon"** (confirmed from the log): `GuestFallSafeRevive` ran before the
    dungeon rule and took the host's last lamp 2902954 from party_ids' table: the m29 template's spot (52.8,
    -1.0, 21.9), 180 m from that lamp's object (2901954 at (-132.7, 17.4, -15.5)) and in the air; the guest
    fell 54 m and died again, twice. `ids::ReturnSpot` now has no spot for area 29 (as documented), the fall
    rule gets `FallRespawn::BesideMember` in a dungeon (party_chalice_rejoin, a living member here, not a
    boss fight), and the guest-lamp distance tests use the lamp object 2901950 + n (`LampSpotNow`).
  - **Dungeon lifts** (cause inferred: the guest's chr ended 5 m under the car at the 21 m lift's bottom with
    the game's fall-death state; no ride sample in its height history): the lift table has no m29 lift. The
    game side learns the cars from the rides' `2003[18]` in m29's lift events (`liftrules::ChaliceLiftCar`,
    `kChaliceLiftEvents`), cleared at every load; the on-a-moving-car test takes a learned car within 4 m
    and 3 m of height, and `liftrules::CarRiderTick` (the chr's height against the car object's, no ride
    bit needed) puts a rider that drops through a moving car back on it.
  - **Smoke `clg`** (`--only chalice_guest`, host `chalice_pthumeru_inside`, guest `chalice_hintertomb_ritual`;
    r3 16/16): the guest's 31 m fall in the boss room -> up beside the host (no second death); both ride lift
    2901004 down, the guest's game learns the car, no fall; the guest lights lamp 2901953 (the host's last lamp
    follows), uses it -> 72900103, the warp request 2102955 -> both in the Dream together. Not reproduced live:
    a drop through a dungeon car (the rider guard is unit-tested only) and the report's own lift death.
* **A boss paid once (0.7.1, cl9)**: cl6 / cl8 showed every re-summon into the cleared dungeon paying the
  layer boss again (`boss clear on this game (BlockClear2): boss 2900100, clear bonus 1995`, Insight 6 -> 7).
  Cause (verified): `ChaliceLeaveSession` used a bare BeginLeave, so an apart guest stayed in its
  host-world-mode alone load (load mode 1, no chalice slot bit: `slot -1`); the next summon's SetLoadMode(1)
  found mode 1 already and carried no slot record, the area-29 flags read as none, the boss came up alive,
  died at once and its defeat ran on the guest. Fix: the game's own leave with the load home
  (`coop::LeaveGameSession`, OnLeave_Limit): cl9 shows `load mode 0` at home, `slot 0's record ... carried
  over` at each re-summon, `kill seen: entity 2900100 ... (paid 0)`, no clear. Guard behind it:
  BlockClear2 (0x13849A0) returns at once on a summoned guest within 15 s of its arrival
  (`deathrules::StaleBossClear`; a guest never arrives into a running boss fight), log `boss clear of boss N
  skipped on this guest`; it did not need to fire in cl9.

## 9. A guest's echoes and the party's respawn in the host's dungeon (chaliceechoes, 0.7.6)

Two reports (flagless logs, 2026-10-24): a guest's Blood Echoes lost in the host's dungeon, and the dead
guest staying dead until the host went back to the Dream.

- **The party stain knows its dungeon.** `gueststain::Stain.dungeon` is the game's current ChannelId
  (FrpgNetMan +0xa88: a summoned guest's is the host's, `chalice::CurrentDungeon`) when the guest dies in a
  dungeon map; party_stain.txt keeps it as a `#<id>` token after the echoes. `gueststain::InMap` (area 29)
  matches the area and the dungeon id, not the block: the same ChannelId is the same layout, so the spot is
  valid in every visit of the party to that dungeon. Until then the stain waits (`Fate::Keep`); the next death
  of the guest replaces it (the vanilla rule for a lost stain: the guest's own death elsewhere, not a party
  flow). A stain from before 0.7.6 (area 29, no id) cannot be placed: it is paid back at once
  (`Fate::Refund`, log `given back`). Both the walk onto it and the enemy hold (echodup / echohold) work
  as in the normal world (`GuestStainTick`).
- **A host travel inside the dungeon the guest stands in is followed.** `GuestDungeon()` (party_travel.cpp)
  is the game's current dungeon while the guest's map is area 29 (the host's when summoned in), else its own
  slot's. The respawn after a wipe (`host_death` to a dungeon lamp) used to be "not followed: this game has
  <own slot>", so a guest dead in a boss fight's limbo waited for the host's Dream trip. The replay is a B2
  re-join whose K11 step sets load mode 0 and 1 again; `chalice::KeepDungeonCarry` keeps the carried
  quick-join record through it (else the map load read the guest's own slot and built the guest's own dungeon
  under the host's lamp: clw run 2). Smoke: `chalice_guest` (clg: fall death, party stain, walk onto it),
  `chalice_wipe` (clw: guest down, host down, the respawn load into the host's dungeon).
- A death in a boss fight stays the ERSC rule (limbo, spectating, until the boss is gone, a wipe or a rest;
  ERSC_PARITY C5): not changed.

## 9. Progress comes home with the guest (0.7.6)

Player report: a guest that played the host's dungeons had all the chalices (section 5) but, in its own
world, the dungeons were not made at the altar and the layer lamps / shortcuts were missing.

**Where a save keeps a Chalice Dungeon** (C, decoded from the fixtures with `tools/party/save_decode.py`):
only in the event flag pool (150,000 bytes of the save), nothing else. Slot k (0..6) = area 40 + k groups
(pool group index: area 40 is the same group as area 29, areas 41..46 are their own, keys in
`resolve_group_key`):
* the record: T9 ids `94000000 + k * 100000 + bit` (section 1): ChannelId `+32/+64`, the map `+96` (`0x1d......`,
  all ones = empty), layout `+160..`; an empty slot reads `{0, 0x80000000, 0xffffffff, 0xffffffff}`;
* the dungeon's progress: the area-29 ids (`1 29 0 Znnn` events, `7 29 0 02nn` lamp-lit) while it is the current
  slot, stored in slot k's own groups (`1 4k 0 Znnn`, `7 4k 0 02nn`: `SlotFlagId`). In the Pthumeru fixture slot 1
  holds e.g. 14101555..566, 14107000..007 (events), no lamps lit;
* **a lamp n (return point 2902950 + n, n 0..8)** is four flags: the object's registration `12907800 + 20n`, lit
  `+10` and `+11`, and lamp-lit `72900200 + n` (the layout of every map's lamps, `lampshare::LitFlags` with block 290).
  Lit 2902952 gives 14107840 / 14107850 / 14107851 / 74100202 in slot 1 (live, `cpr`);
* the slot selector `9020..9026` (one-hot); the altar lists the dungeon of a slot whose record holds a map even when
  slot 0 is selected (live: the guest's altar read "Pthumerian Labyrinth: Chamber of the Seal, Layer One: Heart").

**Root of the report**: a summoned guest plays in load mode 1 (the host's flags in an overlay, discarded on the way
home) and the quick join writes only the transient network slot 0; its own slots 1..6 never learn the dungeon, and the
flags the party reached were in the overlay.

**The change** (`party_chalice.cpp` `BringDungeonHome`, pure rules in `party_chalice_merge.h`): the quick-joined slot
record is kept when the carry happens (`g_carried_rec`); on the load-mode 1 -> 0 switch (the guest leaving the host's
world, any path) the lamp / shortcut flags that read set in the overlay are collected (`ReadReached`: the lamp
categories and shortcut doors of area 29 block 0 from `party_flags.inc`, plus the four flags of every lamp whose lit
flag is set: the host's world syncs only some of them), the switch runs, and then in the guest's own store:
1. `PlanMerge`: the dungeon already in slots 1..6 (same ChannelId) -> `Existing`: nothing is added; else the first free
   slot 1..6 (record holds no map) -> `Added`; none free or no ChannelId -> nothing. Slot 0 is never a target and a
   slot holding a dungeon is never written;
2. `Added`: the 43 record dwords are written with `SetEventFlagValue` at `94000000 + k * 100000`;
3. every reached flag is set in slot k's groups with `SetEventFlag(SlotFlagId(id, k))` if not set (OR; never cleared).
Boss-defeat flags (category boss_defeated of area 29 block 0, e.g. 12901686 for the layer 1 boss) are copied too (2026-10-25 decision; no loot is given, the guest already got its drop in the host's world). Item pickups and chest flags are not copied (Chalice weapon pickups and shop gates stay per player, and a
boss the guest has not killed pays its own loot). Gate: the host's **Shared lamps** rule (`party::status::ShareLamps`,
guests follow the host); no toggle of its own. Log: `chalice: progress home: dungeon N added to the altar in slot K,
M of M lamp / shortcut flags set`, `... already in slot K ...`, `... not added (why)`.
Both ways: the host's own copy of a dungeon a guest made is the host's slot (the game's join), so host progress in a
guest's dungeon merges into the guest's slot the same way (`Existing`).

**Evidence**: unit `party-chalice-merge-test` (free slot, dedupe, own dungeons kept, all slots full, ids). Live smoke
`chalice_progress` (cpr: host `chalice_pthumeru_inside` lights lamp 2901952, guest with no dungeon leaves; its saved
file holds the record in slot 1 and the four lamp flags, lamp 2902950 unlit), `chalamp` (cl: the guest owns a
Hintertomb dungeon in slot 1; the host's lands in slot 2, once, over several re-summons and the final leave) and
`chalice_altar` (cpa: the fixture `chalice_guest_brought`, screenshots of the altar's list).
Not proven: a title-joined guest whose own slot already holds the same dungeon (no quick join, so no carry: its
progress is not merged); a guest killed / crashed in the host's world (no mode 0 switch before the crash); all six
slots full (unit only); layers deeper than 1 (only lamp 2902952 reached in the fixture).
