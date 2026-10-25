# Guest services: Doll, shops, workshop (and lamps) for a summoned guest

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA of
`smoketest/out/eboot.elf`; XML `Address` = ours + 0x400000). Talk scripts were decompiled from
`script/talk/m21_00_00_00.talkesdbnd.dcx` (soulstruct 2.3.2 ESP export), EMEVD with
`tools/party/flag_tool.py`'s parser, MSB `map/MapStudio/m21_00_00_00.msb.dcx` read raw. Status labels:
**[C]** confirmed statically, **[R]** confirmed at runtime (live 2-instance run), **[I]** inferred.

Goal (user requirement, like Elden Ring Seamless Co-op): a guest in the host's world can use the
Doll (level up), the Bath Messengers' shops (echoes, Insight), the workshop tools (weapon upgrade,
Blood Gem workbench, storage) and lamps, on its own character; nothing it does touches the host.

## 1. Whose data does a summoned guest change?

* **Character record** [C]: level (`PlayerGameData +0x90`), echoes (`+0x94`), Insight (`+0x84`),
  attributes, inventory (`+0x328`) and storage live in the local `PlayerGameData`
  (`[[0x553B130]+8]`). Joining another world (SetLoadMode(1)) replaces only the **event flag** tree
  with the host-world overlay (`event_flags.md` 3); the record is never overlaid or swapped. Every
  menu (level up `OpenSoul`, shops, upgrade, storage) reads and writes that local record.
* **Save** [C/R]: the game's save writer (`0x156AA50`, `event_flags.md`) serializes the whole local
  save - the record plus the flag **pool** (not the overlay). It runs while a guest is summoned
  as at home (vanilla: consumables a phantom uses stay used). So level-ups, purchases, echoes and
  Insight spent persist in the guest's own save (the party save when `BB_PARTY_SAVE=separate`).
  Runtime evidence: section 5.
* **Flags** [C]: talk-script `SetFlagState` on a summoned guest goes to the overlay (discarded on
  return), except 6000-8999, which load mode 1 keeps in the guest's own pool: exactly the
  personal Dream flags (Doll / messenger shop stock, dress-up unlocks, chalice unlocks). Shop stock
  limits therefore track the guest's own purchases. Nothing a guest's talk script does is sent to
  the host: talk flags are local (no `2000[2]` network sync, no Lua `CompleteEvent` packet).
* **Host** [C]: no menu of the guest has a network path; the host's record and save are not
  reachable from the guest's game.

## 2. Doll, shops, workshop: the gate and the lift (implemented)

### 2.1 Gate [C]

Every Hunter's Dream talk script that opens one of those menus has a "client" state machine
(`クライアント待機` = client wait; `ゲスト待機` = guest wait in the t21069x workshop scripts):

```
State_1: CALL_STATE_MACHINE[9](...)      # the normal talk / menu machine
         if IsClientPlayer() == 1: -> State_2
State_2: CALL_STATE_MACHINE[8]()         # "client wait": does nothing
         if IsClientPlayer() == 0: -> State_1
```

`IsClientPlayer` is talk test function 19. The talk environment's test-function switch is
`0x203AD60` (`call [vtbl+0x10]` = function id, `cmp eax,0x3e`, jump table at `0x20473B4`; ids 0-62,
the rest falls through to `0x203C747`). Case 19 (`0x203BA66`):

```
203ba66: mov rax,[r14] ; mov rdi,r14 ; call [rax+0x18]   ; argument count
203ba6f: cmp eax,1 ; jne 0x203c747
203ba78: 41 0f b6 47 25    movzx eax, byte [r15+0x25]   ; r15 = talk environment
203ba7d: e9 4d 34 00 00    jmp 0x203eecf                 ; [r12+8] = 2 (int), [r12] = eax
```

`env+0x25` is set in `0x203ABB0` (`0x203AC0F`) when the local player's chr type (`[[WorldChrMan]+0x60]
+0x78`) is not 0 or 8 (host body / grey): any phantom. `env+0x10` is the talk script id
(`t210304` -> 210304) [R: logged for every Dream script in the live run]. Case 52 `IsMultiplayerInProgress`
(`0x203C505`) is the session presentation count (`[[0x5556678]+0x16F8]+0x14 > 1`) or the session
peer count; only the headstones (t210000-t210003) and the chalice altar (t210100) use it - those
are blocked for the host too while a guest is present (lamp phase, section 3).

No other gate: the shop scripts t2106xx (`OpenRegularShop`) have no client test at all, the menus
themselves (`OpenSoul`, shop, upgrade, `OpenRepository`) have no role test, and the messengers'
EMEVD animation events (12105043 Insight messenger, 12105064 dress-up messenger: `1003[6]` end if
multiplayer after the first animation request) only decide whether the messenger has risen; with
the host's flag (e.g. 12100320) in the overlay the guest sees the same messenger state.

**The lower bath messengers' animation is per player [R]** (bathsync, 2026-10-21). m21 event
12105040 (売買メッセンジャーアニメ, messengers 2100211-2100213 of t210691 / t210694 / t210695) loops:
`[0-2]` 2003[18] idle in the bath (7001/7002/7041), `[3]` wait 12105041 ON, `[4-6]` rise
(7005/7006/7029), wait 29 frames, `[8-10]` at the shop (7003/7004/7043), `[11]` wait 12105041 OFF,
`[12-14]` sink (7007/7008/7047), wait 28 frames, `[16]` 1000[04] restart. 12105041 is set by the
talk script (talk command switch, return 0x2043F5C -> 0x27C1C12 -> SetEventFlag) of whoever opens
the shop, in its own game only, and cleared by it when the player leaves (return 0x27C1D3D). The
event's state is network-synced: packet 0x31, drained by
0x12F0F90 at 0x12F1744 (`{u16 kind 0, u16, u32 size, i32 event, i16 slot, state}` to every
instance with that event id and slot, vfunc +0x28). With one player in the shop (flag 1) and one
watching (flag 0) each game passed the other's wait: the shopper's `[3]` ON reached the watcher,
the watcher's `[11]` OFF reached the shopper, and the event ran rise / sink every ~2.0 s on both
games (the watcher saw the messengers pop in and out with their sound; the shopper too, behind the
menu). No SetEventFlag of 12105041 ever happened on the watcher (`BB_PARTY_WORLD_WATCH` write
trace). Now (`PerPlayerMessengerEvent`, `BB_PARTY_MESSENGER_SYNC=1` restores it) the 0x31 packets
of 12105040 are dropped on receipt: each game animates the messengers from its own talk flag (live
after: the shopper's messengers rise once, stay at 3007003 and sink once when it leaves; on the
watcher they rose once with the other player's shop opening - the animation itself reaches it
some other way - and stayed up, no loop; the rise / sink loop count on the watcher went from 16
to 0). The
Insight messenger's 12105043 is different and stays synced: its copy ends on a client
(`1003[101]`), the host's copy drives every game (live: one rise 3007034/3007035 when the host
opened t210698, one sink 3007036 when it closed; its talk script clears 12105044 on leaving).

0.7.3: the same filter drops the 0x31 packets of the Dream's gift messengers' events 12101020 /
12101021 (starting weapons), 12101022 (Notebook), 12101024 / 12101026 (bells) and 12101028 (Eye):
a guest's copy ending ended the host's (the host's weapon messengers sank before it picked; items.md 10.8).

### 2.2 Which talk scripts (MSB m21_00_00_00 entity -> talk script) [C]

The filter allows by talk script id; the entity column is where the script is placed.

| Entity | Talk | What | Guest |
|---|---|---|---|
| 2100700 | t210304 | Doll (level up; "talked to Gehrman" branches) | yes |
| 2100200 | t210693 | storage (`OpenRepository`) | yes |
| 2100201 | t210692 | workbench: weapon upgrade, Blood Gems, repair, crush materials | yes |
| 2100202 | t210696 | mirror (appearance, `OpenCharaMakeMenu`) | yes |
| 2100203 | t210699 | Memory Altar (runes; prompt 70000008), live h5 2026-10-13 | yes |
| 2100211, 2100215, 2100216 | t210691, t210694, t210695 | Bath Messengers: shops, sell | yes |
| 2100218 | t210697 | dress-up messenger (attire for flags 6071..) | yes |
| 2100219 | t210698 | Insight messenger (Insight shop) | yes |
| 2100600 | t210305 | Gehrman: ordinary lines (72100120-72100129, 72100133-72100135); last event: the ending choice | yes, outside his last event (2.7) |
| 2100800 | t210306 | (Gehrman / Doll death branch) | no |
| 2100950.. | t210000-3 | headstones (travel) | no (lamp phase) |
| 2100230, 2100231 | t210720, t210721 | Dream lamp / warp menus | no (lamp phase) |
| 2100954, 2100959, 2100960 | t210050, t210100, t210110 | chalice altars (all of t210050..t210110) | no; the host's since 0.6.2 (G1: IsMultiplayerInProgress answered 0 for the host, `BB_PARTY_HOST_ALTARS`) |

t210140 / t210141 / t210200 / t2107xx (other `OpenSoul` scripts) are not placed in the m21 MSB;
they are assigned elsewhere (NPC param / other maps) [I].

### 2.3 Lift (party_guestsvc.cpp) [C]

`HookValueFilter` (coop_hooks) replaces the 5-byte `movzx` at `0x203BA78` with a jmp to a stub
that runs it, then calls `IsClientFilter(env = r15, value = eax)` with all volatile registers
saved, and continues at `0x203EECF` with the filtered eax. The filter answers 0 only when

* the gate is open: `BB_PARTY_GUEST_SERVICES` on (default in party mode), the local player is a
  client (`SprjSessionManager+0x124 == 6`), world up, not loading, map `m21_xx` (Hunter's Dream);
* the game said 1; and
* the talk script (`env+0x10`) is in the list above (`BB_PARTY_GUEST_SERVICES_TALK=all|id,...`
  widens it for diagnostics).

Everywhere else the game's own answer stands: quest NPCs in the host's world keep refusing guests,
Gehrman's ending choice (his last event, 2.7) and the travel menus stay host-only. Nothing runs on the host (role 3: the
game already answers 0 there).

Log lines (`Party guest services:`): `open: guest in the host's m21_00_00_00; level L, echoes E,
Insight I`, `talk script tN (environment 0x...) asks IsClientPlayer: allowed|kept vanilla`,
`talk scripts asked IsClientPlayer N times (answered 0)`, `own character: level a -> b, echoes ...`.

### 2.4 The Doll on a guest (placement) [C]

User report from the first live run: the guest in the host's Dream did not see the Doll where the
host's Doll stands. What decides the Doll, per m21_00 EMEVD:

* No client guard and no character disable for the Doll (entity 2100700) anywhere in m21_00 or
  common EMEVD: the constructor only sets her update frequency (`2004[34]`), the preconstructor
  keeps the Dream state flags 1000-1003 (`1003` "Doll dead" via 12100101, 1002 / 1000 via 9462).
  Her talk script t210304 runs on the guest (the live run logged it asking IsClientPlayer), so the
  character exists on the guest.
* **12100110** `人形のヒロイン_立ち位置制御` (Doll stand position, every Dream load, both roles):
  lifeless (spot 2102304, anim 9011, sets 12105100) if flag 9401 is off or Insight is 0; else
  12100105 = 1 (awake), 9404 = 1; ends for the late states (1000 / 1002 on, 1026 / 1027, the DLC
  boss flags 13501800 / 13601800 without 72100116 / 72100117); then **re-rolls the stand spot**:
  index 24 `1003[1]` skip 2 if flag 100, index 25 `2003[22]` clear 12100500-12100509, index 26
  `2003[17]` random one of them, and short-warps her (`2004[41]`) to 2102301 (by the lamp, anim
  9009), 2102302 (in the workshop, -13.3, -1.1, 1.7) or 2102303 (lower garden, -7.4, -6.3, -25.1).
* Where the inputs come from on a guest (load mode 1, join snapshot 0x10): 9401 (global 9000-9999)
  and 100 / 1000-1003 / 1026 / 1027 (global 0-1999) are in the snapshot = the host's values;
  12100105 and 12100500-12100510 are m21 type-1 zone 0 = in the snapshot; Insight is the guest's
  own (the test guest has 6); 13501800 / 13601800 (other maps) and 72100116 / 72100117 (T7) are
  not in the snapshot (0 on the guest: only differs for a host that killed those DLC bosses).
* So a guest gets an awake Doll, but **rolls its own stand spot**: in two of three cases its Doll
  stands in the workshop or the lower garden while the host's stands by the lamp.

Fix (party mode, guest services on): client-only EMEVD skip rules `12100110:2003:22@25` and
`12100110:2003:17@26` (`EmevdSkipRule::client_only`, checked against `SprjSessionManager+0x124
== 6` in the EMEVD filter): on a client the re-roll is skipped, the host's 12100500-12100502 from
the snapshot stay, and the guest's Doll is warped to the host's spot. The host is unaffected (the
rules never match there). `BB_PARTY_GUEST_SERVICES=0` drops them.

### 2.5 The Doll as a wandering ghost on a guest [R, fixed]

Live: the guest saw a translucent Doll it could walk through and not talk to. Probe: her ChrIns
chr type (+0x78) was 5 on the host and **4 (wandering ghost)** on the guest. Chr creation
`0x18E4F94..0x18E4FDC`: when the local session role is 6 (client) or a presentation is active
(`[[0x5556678]+0x16F8]+0x98 != -1`), an NPC whose NpcParam row (`[chr+0x3C8]+8`) has
**isChangeWanderGhost** (byte 0x145 bit 7) gets `ghostModelId` (+0x10C) and chr type 4. The Doll's
row **803000** has it (1.09 byte `0x80`; 804000 too). Fix: a seamless param rule writes NpcParam
803000 byte 0x145 `0x80 -> 0x00` (row size 0x184; only clients read the bit;
`BB_PARTY_GUEST_SERVICES=0` keeps vanilla). Live after the fix: chr type 5 on the guest, solid,
her talk menu opens.

Playtest 1 (bugs 11 / 17, 2026-10-14): the same bit is on almost every talk NPC's row, so Gehrman,
Iosefka, Alfred, Eileen, the Oedon Chapel Dweller and others were wandering ghosts for a guest. The
rule is now a table (`party_npcghost.h`, 42 rows from the 1.09 NpcParam: 6040-6990, 8120-8125,
113010, 803000, 804000), each checked against its 1.09 byte (`0x80`, the Dweller `0xC0`: only
bit 7 goes) and refused on any other value. Enemy rows with the bit (109100, 111010-111012, 402xxx,
504020 the beast beggar, ...) stay vanilla. Startup log: one `... stays solid for guests (NpcParam N
isChangeWanderGhost): 128 -> 0` line per row.

### 2.6 The Doll's level-up handshake [C, R]

`t210304` StateMachine x7: State_3 "レベルアップ" -> State_22 -> State_10 sets 72100100 and waits for
**72100101 and 72100102**, then State_7 runs `OpenSoul()` (talk command 31: switch `0x203FFD0`
case at `0x2040B4E`, menu request `0x1C4A520` with the factory `0x1BB7F60` that builds the menu
named "LevelUp"). 72100101 is set by m21_00 event **12100112** (the player's kneel), 72100102 by
**12100113** (the Doll's gesture); 12100114 is the level-up SFX (flag 7500). All three start with
`4[3] [15, 10000, 0]` + `1000[2]` = end unless the local chr type is 0 (12100112 repeats the test
in each distance branch). Live trace (guest): the kneel ran once 12100112 was rewritten, but no
talk command 31 and no LevelUp factory call followed - 72100102 never came. Fix: an EMEVD
rewriter (second slot, `SetEmevdRewriter2`) sets those `4[3]` chr-type bytes (args +8) to the local
player's type on a client while they execute and back to 0 on a non-client (the EVD data is
shared) for events 12100112-12100114. The command path itself has no role test (the shop
command uses the same menu request and works, 5.).

Playtest 1 (bug 10): the handshake flags went to the host as npc_quest world actions (72100100-102
and 12100112-114 fell into the m21 NPC ranges), so the HOST knelt and its Doll gestured for the
guest's level-up. party_flags.inc now has never_sync override rows (flag_tool.py OVERRIDE_ROWS):
12100112-12100118 (kneel, gesture, SFX, bow, surprise, gesture reaction), 72100100-72100102,
72100105-72100108 (idle talk / bow / surprise triggers), 72100110 and 72100113 (talk state). Each
player levels up at its own Doll; the key-item gift (72100112 / 72100114) is unchanged.

### 2.7 Gehrman on a guest [data, R]

Live (2-player session 2026-10-20, 0.6.9): in the host's Dream the guest could not talk to Gehrman;
the host's talks played on the guest (shared lines), and each time the guest's prompt 10010200
showed for 0.4 s and went (`talk script t210305 ... asks IsClientPlayer: kept vanilla`). Talk ESD
`t210305.esd` (m21_00_00_00.talkesdbnd): StateMachine 1 runs machine 0x7FFFFFF7 while
`IsClientPlayer() == 0` and the "client wait" machine 0x7FFFFFF8 while it is 1, so the whole talk is
host-only, not only the choice. 0x7FFFFFF7 picks the last event (0x7FFFFFF5 -> 0x7FFFFFF4: Yes / No,
72100130 submit / 72100131 refuse; its "attacked" branch 0x7FFFFFF3 sets 72100131) when **1028** is
on or **1029** is on and **1030** off (m21 events 12100143 / 12100144 / 12100145); otherwise the
ordinary talk 0x7FFFFFF6 (lines 1500000-1501300, flags 72100120-72100129 / 72100133-72100135, no
choice). Fix: in the Dream gate the filter answers 0 for t210305 while the guest's flags 1028 / 1029 /
1030 (the host's: global 1000-1999, join snapshot and mirror) say his last event is not up (the tick
reads them; an unreadable flag keeps him vanilla). When the host's Dream reaches the last event the
next IsClientPlayer answer is 1 again and machine 1 goes back to the client wait. 72100130 / 72100131
stay never_sync and out of world actions. t210306 (the boss Gehrman's death lines, no menu) stays
vanilla. Log: `Gehrman (t210305): a guest may talk to him (his ordinary lines) (flags ...)`.

## 3. Lamps and headstones (implemented, opt-in `BB_PARTY_GUEST_LAMPS=1` until run live)

Bloodborne's world lamps only go to the Hunter's Dream (resting = the load into the Dream refills
and respawns), and the Dream's headstones go to a lit lamp. Seamless-style behaviour for a party:
whoever uses a lamp or headstone, **the whole party travels**, led by the host (B1 travel,
`travel.md`), so nobody is left alone in a world they cannot reach.

### 3.1 Gates [C]

* **World lamp object** `0x12F5C40`: byte `+0x49` = session peer count > 0 or presentation count
  > 1 (`0x12F8303..0x12F8323`); while it is set the action-button row is 90000 (no lamp use), for
  the host as well as for guests. That is why even the host cannot rest with guests present.
* **Common event 7200** `ワープOBJ_拠点へのワープ_XX` (lamp -> Dream, one instance per lamp): index 0
  `1003[6] [0, 1]` = end if client (multiplayer state 1); then wait for the lamp's flag, clear it,
  turn, SFX, `2003[49]` warp (-> `0x13CDF30(id)`).
* **Headstones** t210000-t210003 (and the warp menu t210721): their root state machine parks in an
  empty machine while talk test 52 `IsMultiplayerInProgress()` is 1 (case `0x203C505`: session
  presentation count > 1 or peer count > 0) - for the host too - and in "client wait" while
  `IsClientPlayer()` is 1.

### 3.2 Lift [C, runtime pending]

* `0x12F831C 85 c9 0f 9f c1 08 c1` -> `31 c9 90 90 90 90 90` (party mode): `+0x49` = 0, so lamps
  are usable with guests present (host and guests).
* EMEVD rule `7200:1003:6@0` (party mode): a guest's lamp event no longer ends at once; on the host
  `1003[6]` (end if client) never ends anything, so the rule is a no-op there.
* Value filter at `0x203C564` (`or cl, al; movzx eax, cl`, then `jmp 0x203EECF`): for the headstone
  scripts IsMultiplayerInProgress answers 0 in party mode; IsClientPlayer answers 0 for them on a
  guest in the Dream.
* **Guest warp -> host request.** `0x13CDF30` is now replaced (it was a pre-hook): our own replays
  pass through; any other call on a guest that is a client in the host's world is consumed and
  becomes EVENT `lamp_request {id, seq}` to the host (sent from the director tick).
* **Host.** `on_event` (host side) parses and checks the request (`id` must be a ReturnPointParam
  row, `TravelLampKnown`); the director tick runs `0x13CDF30(id)` itself when the host is in the
  world and not loading, at most once per 10 s. The host's own warp is captured by the travel funnel
  hook as usual (TRAVEL broadcast), every guest replays it and is summoned again: one party travel.
  No loop: the host's warp is not intercepted (host role), the guests' follow warps are our replays.
* Rest: travelling to the Dream is the rest (the host's load refills; guests get the phantom
  refill on the host's "rested" event, `phantom_limits.md` 1.2).
* Opt-in: `BB_PARTY_GUEST_LAMPS=1` (it also changes the host's lamp gate); default off until a
  live run confirms 3.3.

### 3.3 Live results and open items

* **Guest lamp -> party rest [R]** (`BB_PARTY_GUEST_LAMPS=1`, `host_warp=2412951@15`, guest test
  `lamppress;lampflag=72410101`): the host warped to Central Yharnam, the guest followed (B2,
  still a client in m24_01); the guest's lamp flag 72410101 started its own common event 7200 (the
  `7200:1003:6@0` rule lets it run on a client), whose warp `0x13CDF30(2102950)` became
  `lamp: request 2102950 sent to the host`; the host ran the same warp (`lamp: slot 1 used a lamp /
  headstone: the host warps to 2102950, the party follows`), sent "rested", and the guest refilled
  after the follow load (`refill (host rested): vials and bullets topped up`) and re-joined in the
  Dream. Not yet shown: the guest's own button press at a world lamp (the run set the lamp's flag;
  the press needs the guest standing at the lamp with its prompt up). The host's capture logged
  the warp twice (two TRAVEL / rested events for one warp: harmless, deduplicated by seq on the
  guest, but worth a look).
* **Guest headstones [R, fixed 2026-10-11]**: choosing an area crashed a guest (fault under the
  menu code 0x1BEA2D4: the warp list is built from the lamp flags of the guest's overlay, which the
  vanilla join snapshot left empty). The host-world mirror (event_flags.md 12) now copies the host's
  lamp flags (72xx02xx etc.) into the overlay. Live (`guestheadstone;keys=cross,cross,cross,cross`,
  guest in the host's Dream): the guest's "Awaken Above Ground" list shows the host's 7 lamps (1st
  Floor Sickroom ... Old Yharnam; screenshot), choosing 1st Floor Sickroom sets that lamp's flag,
  m21 event **12107000** (headstone warp, one instance per lamp: `1003[6]` end if client, wait for
  the flag, `2003[49]` warp) runs with the new client-end skip `12107000:1003:6@0` (LampSkipRules),
  its `0x13CDF30(2412950)` becomes `lamp: request 2412950 sent to the host`, the host warps (`host
  travel ... lamp 2412950`), the guest follows (B2 `replay now ... lamp 2412950`, `arrived in the
  host world`) and stands beside the host in the clinic. No crash. The guest's IsClientPlayer
  answer is lifted for t210000-t210003 whenever `BB_PARTY_GUEST_LAMPS=1` (t210721 stays excluded).
* **Host headstone with a guest present [R]** (`hostheadstone;keys=cross,cross,left,cross,cross`, host
  side): the headstone's menu opened for the host (the IsMultiplayerInProgress lift), the host
  chose a lamp and warped (`host travel ... lamp 2412950`, one TRAVEL); the guest replayed it (B2:
  host snapshot applied, `replay now ... lamp 2412950`, host-world load).
* **One TRAVEL per warp [fixed]**: the host captured a guest-requested lamp warp twice, 26 ms apart
  (two TRAVEL, two "rested"). `CaptureHost` now drops the same warp (kind, lamp, map, respawn
  record) seen again within 1 s; the next runs logged one TRAVEL.
* **World lamps in a party [R, fixed 2026-10-11]** (host: party mode, `BB_PARTY_HEADSTONES`
  default on; guest: guest lamps, on by default since 2026-10-11, `BB_PARTY_GUEST_LAMPS=0` off). How a lamp works: the lamp is a **talk script**,
  `tAAB000..tAAB003` of its map (Central Yharnam's lamp 2411951 = **t241001**; the Dream's
  headstones are t210000..t210003 of the same family). Each frame it registers its action button
  (ActionButtonParam **6100** "Light Lamp" / **6101** "Return to the Hunter's Dream", type 0xD,
  through the talk condition 0x203AD60 -> `0x1CD91F0`; `buttonprobe` saw `13:6101` 30x/s) when the
  lamp's FrpgNetMan +0xC70 entry is enabled - and that enable is the lamp object's four
  "unavailable" bytes, of which **+0x49 = session peer count > 0** (0x12F831C). So in vanilla
  the prompt disappears for everybody as soon as a session is up. On a client the script also
  asks **IsClientPlayer** and **IsMultiplayerInProgress**. (The lamp object's "type" +0x28 and
  +0x2C = 90000 + type are the flame level / SFX id: unrelated.)
  * Fix: the +0x49 patch (`31 c9 90..` at 0x12F831C) is now applied whenever the party headstones
    are on (it was only with `BB_PARTY_GUEST_LAMPS`); `MultiFilter` answers IsMultiplayerInProgress 0
    for lamp scripts too (`LampTalk`: talk / 1000 in 210..369 with block 0..2, talk % 1000 <= 3);
    for a client, `LampClientAnswer` (in the IsClientPlayer filter, outside the Dream-only gate)
    answers 0 for lamp scripts when `BB_PARTY_GUEST_LAMPS=1`.
  * Host, live (`lampprobe;lampwalk;hostlamppress`, the host lamp-warped to Central Yharnam with a
    guest in the party): prompt "Return to the Hunter's Dream" (screenshot), Cross -> the confirm
    box "Return to the Hunter's Dream? YES / NO", Left + Cross -> the lamp's flag, common event
    7200 warps (`0x13CDF30(2102950)`): `host travel ... lamp 2102950, map m21_00_00_00`, "rested"
    sent, the guest replayed it (B2 `replay now ... lamp 2102950`, `arrived in the host world`).
  * Guest, live (`BB_PARTY_GUEST_LAMPS=1`, `lamppress;lampprobe;lampwalk`): `talk t241001 asks
    IsClientPlayer (world lamp: answered 0)`, the same prompt and confirm box on the guest
    (screenshots), YES -> `lamp: the guest's warp to 2102950 becomes a request to the host`, the host
    warped (`slot 1 used a lamp / headstone: the host warps to 2102950`, one TRAVEL), the guest
    followed by B2 into the Dream.
  * **Lighting a new lamp [R]** (test fixture: the host's own save loses Central Yharnam's lamp,
    `BB_PARTY_PROGRESS_TEST_CLEAR=12417830,12417831,72410201`: the lamp object's base+10 / base+11
    of its flag base 12417820 and its lamp-lit flag 7AAB02xx). Through the mirror the guest's
    prompt there is "Light lamp" too although its own save has the lamp lit (screenshot).
    - Host lights it: 6100 registered, Cross -> `host flag 72410201 = 1`, then 12417830/831; the
      guest's overlay follows within ~1 s (`blocks changed by the host: 72410:1`, `12417:2`) and the
      guest's prompt at the lamp is "Return to the Hunter's Dream" (screenshot).
    - Guest lights it: its game writes the flags into the host-world overlay only (own pool:
      0 differing flags before / after, BBPF -pool dumps). `GuestLampLitTick` (guest, load mode 1,
      once a second) sends the lamp flags its overlay has and the host's mirror image lacks - the
      lamp's base+10 / base+11 from its FrpgNetMan +0xC70 entry and the map's 7AAB02xx - as EVENT
      **`lamp_lit`** `{ids}` (at most 16, each `LampLitFlagAllowed`); the host sets them in its world
      (`lamp: slot 1 lit a lamp in the host's world: set 72410201`) and the mirror carries the rest.
      The guest's own save never changes; the host's world is lit for the whole party.
* **Default on (2026-10-11)**: guest lamps / headstones are on in party mode; `BB_PARTY_GUEST_LAMPS=0`
  (or the switch "Guests use lamps" on the Party rules: Travel page / the overlay's party section, ini
  `party_guest_lamps`; read at start) turns them off. With them on, live: the guest's Doll level up
  still works (`LEVEL UP on the guest: level 37 -> 38`), the guest's headstone travel works (above).
* **Rest via a guest's lamp [R]**: the guest's lamp use is a request; the host makes the warp, so
  the host's load is the rest (its world's enemies respawn in the host's game, the only world
  there is) and the guest gets the "rested" refill: vials and bullets from storage, and HP (B2 no
  longer puts the pre-travel HP back after a rest travel, `RestedFromTravel`; the refill tops HP up
  too). Live (`hurt`: guest HP 53/106 before): after the follow load `guest: HP 106 after the load
  (a rest travel: the refill heals)`, `refill (host rested): vials and bullets topped up`.
  * Test steps: `lampwalk` stands the player 1 m in front of the nearest probed lamp (its map
    object's matrix: 0x13C9AF0 record +0x24C/+0x25C/+0x26C); `hostlamppress` (host) and `lamppress`
    (guest) press Cross, Left, Cross; `buttonprobe` logs every action-button registration
    (0x1CD91F0 type:id) and the ActionButtonMan gates; `talkprobe` the talk action manager 0x13ADCC0.
* Open: `+0x4B` and the lamp's chr-type range branch for a guest.

## 4. Safety

* Guest actions only change the guest's own record and own save (section 1); the host's game
  never sees a menu transaction. Talk flags: overlay (discarded) or the guest's own 6000-8999.
* Excluded talkers (Gehrman's ending, travel menus, chalice altars) keep the vanilla client wait.
* The filter reads the talker id from the environment the game just dereferenced; no allocation,
  no lock (main thread).

## 5. Runtime evidence

Live 2-instance runs (loopback party, the test save parked in the Hunter's Dream, guest summoned
into the host's Dream), guest log `Party guest services:` [R]:

* `open: guest in the host's m21_00_00_00; level 37, echoes 0, Insight 6` - the gate opens for the
  client in the host's Dream and closes during loads (`closed (role client, map m21_00_00_00,
  loading)`).
* Discovery: every Dream talk script near the guest asks IsClientPlayer through the filter, with
  its id at `env+0x10`: t210000-t210002 (headstones), t210304 (Doll), t210691 / t210694 /
  t210695 (messengers), t210721; `+0x8` holds the script table (210304, 210305, 210306, 210693 ...).
* `talk scripts asked IsClientPlayer 270-285 times (answered 0)` per second - the allowed scripts
  leave their client wait; `headstones asked IsMultiplayerInProgress ~90 times (answered 0)` with
  the lamp part on.
* **Bath Messenger purchase (t210691) [R]**: the fixture stands the guest 1.2 m in front of the
  messenger (talk table lookup, +0xC0 / +0xD0), the event-driven script talks, waits for the talk
  menu ('Purchase' 15000140 looked up), selects it, sees the shop's menu request, buys the first
  row: `PURCHASE on the guest: echoes 20000 -> 19460` (a Blood Vial for 540: vials 15 -> 16), the
  guest's game saved 3 s later, and the guest's party save (`savedata_party/.../userdata0000`)
  holds level 37 / echoes 19460 at the record (+0x86E8 / +0x86EC); the host's save keeps its
  echoes 0. Screenshots: `agents/guestsvc/evidence/bath_messenger_purchase` (the "Buy" list with
  the cursor on Blood Vial 540, the quantity box).
* **Insight shop purchase (t210698) [R]**: same script (`warp=talk:210698;buy`): the Insight
  messenger's Purchase list opened on the guest and the first row was bought: `Insight 6 -> 5`;
  after the session the guest's party save holds Insight 5 (+0x86DC), the host's save Insight 6.
* **Workbench upgrade (t210692) [R]** (`echoes=20000;shards=10;warp=talk:210692;open=15000110;
  keys=cross,left,cross,cross`): the "Workshop" prompt (text 70000001) came up at 1.0 m, the
  Workshop menu opened, "Fortify Weapon" listed the guest's weapons; Threaded Cane +3 -> +4: echoes
  20000 -> 19640, Twin Blood Stone Shards 10 -> 7 (the fixture gives goods 3000 / 3010); the guest's
  save holds echoes 19640, the host's save is unchanged. Screenshots:
  `agents/guestsvc/evidence/workbench_upgrade`.
* **Storage (t210693) [R]**: the "Storage" menu opened on the guest (prompt text 70000000 at
  1.0 m); Cross on Blood Vial, Cross on the quantity dialog: carried Blood Vials 15 -> 14 (one
  deposited) (`agents/guestsvc/evidence/storage`).
* **Mirror (t210696)**: not checked: the fixture did not find a character for t210696 in the talk
  table, so the run never placed the guest.
* **Doll level up [R]**: with 2.4-2.6 in place (`echoes=20000;warp=doll;levelup`): talk, her
  lines, "Channel the Blood Echoes", the kneel and her gesture (12100112 / 12100113 rewritten),
  then talk command 31 OpenSoul and the LevelUp menu factory 0x1BB7F60 run on the guest; the
  Level Up screen shows the guest's own record (level 37, echoes 20000, Insight 6). Down to
  Endurance, Right (15 -> 16, "Required 8634"), Cross, the dialog "Spend 8634 Blood Echoes and
  level up?" (cursor on NO: Left to YES, Cross): `LEVEL UP on the guest: level 37 -> 38, echoes
  20000 -> 11366`. After the session the guest's party save holds level 38 / echoes 11366; the
  host's save keeps level 37 / echoes 0. Screenshots: `agents/guestsvc/evidence/doll_level_up`.

## 2026-10-15 (parceh2): Caryll Rune, the editor's frame, the host's view of a new look (H5)

Live, guest in the host's Dream, the guest driven by its in-process pad file (`BB_PAD_FILE`), host
placed by `host_chr`, screenshots of both games after each step.

* **Rune memorized as a guest.** The Caryll Runes are generated gems (lot category 8, GemGen ids
  100xxx), not EquipParamAccessory rows (the fixture's old `rune=100` gave an accessory the altar
  never lists). Fixture `lot=20000;goods=4104:1`: the Rune Workshop Tool, then the game's own award
  of lot 20000 ("Eye"; the C3 gift path also hands it to the host, a test-aid side effect). Without
  the tool the altar says "The workshop tool required for this task is missing." With it: "Memorise
  a Caryll Rune to acquire its eldritch strength." -> "Select Caryll Rune to remember" lists Eye ->
  Cross: Eye is marked memorized, the stats panel shows it in slot 1 and its effect icon appears in
  the guest's HUD.
* **The editor's frame.** `mirror_frame.cpp` (ported from bbhost's change_appearance.cpp /
  menu_steps.cpp): the menu step constructor 0x1C1CCE0 is watched for "ChrMakeCommandList" (type 7);
  in the world the tick opens ChrMake_BG (0x1AD8150, descriptor as character creation fills it),
  runs its controller's update every frame and closes it (0x1AD8E60) when our reference on the
  list's step is the last. Live: the Appearance list sits in the book with the hunter's live preview
  (Gender, Age, Voice, Build, Scale Adjustment, skin colours, Face Details, favourites, Default),
  no game HUD over it; Gender -> Male (the "Changing gender will reset certain changes" box, YES)
  changed the preview.
* **The host sees the new look.** When the list closes the guest re-broadcasts its player record
  (0x1781AC0, what InGameStart runs after every load) and sends `{"op":"appearance"}`; 1.5 s later
  every other game removes that member's chr and rewinds its task to STEP_Create, which builds it
  from the new record (`remote chr ... shown as InGameStart does`). Host screenshots 1-4 s after the
  close: the guest male (no long hair down the back), no load; after the next party travel still
  male. The one-time "Wear clothing to change appearance." flag 72100202 is per player now
  (never_sync; it went to the host's world as a world action before).

## 2026-10-15 (parx): a covenant joined as a guest (H5), the Doll three times in a row (E1)

* **The oath runes are ordinary item lots** (the note in the parfin H5 evidence was wrong): GemGen
  200000 Radiance (lots 34020 / 34040, enemy drops), 200010 Corruption (17000, Annalise's talk sets
  flag 9042, common 9040 awards it), 200020 Hunter (35010, Eileen's late talk sets 9075),
  200030 Impurity (43120, Valtr: m27 event 12700906), 200040 Beast's Embrace (3401850, Laurence),
  200050 Milkweed (43020). Oath runes go into the altar's "Oath Memory" slot (slotTypeRateB 100).
* Live (parx dream, guest in the host's Dream, one session): fixture `lot=35010;goods=4104:1;
  warp=talk:210699;open;keys=left,cross,...`: the game's own award of lot 35010 on the guest only
  (its pickup flag 50001910 into the own save), the Memory Altar, Left to "Oath Memory": the list
  shows "Hunter" (Type Oath), Cross: the guest's stats panel shows Hunter in the oath slot, i.e.
  the guest joined the Hunter of Hunters with its own character in the host's world.
* Same session, before it: `levelup|levelup|levelup`: three level-ups at the Doll in a row as a
  guest (37 -> 38 -> 39 -> 40), each LevelUp menu built on the second press after her lines.

## 2026-10-22 (covenants, 0.7.3): Valtr's Impurity rune for a guest

* Valtr's two gifts are not common 9040 talk gifts: m27 event 12700906 (`DLC_NPCアイテム入手XX`, slots
  0 / 1) ends when the lot's personal flag is set (6676 / 6678), tests the player's character type
  (4[3] on 10000), waits for the talk's trigger (72700321 / 72700322) and awards lot 43120 (the
  Impurity rune, GemGen 200030) / 43130 (the League Staff, goods 131) host-only (2003[4]). A guest's
  League talk in the host's world set the trigger in the host-world overlay only (discarded at
  home) and its own copy of the event never awarded; as a world action (npc_quest) the trigger made
  the HOST's 12700906 give the host the rune (when the host lacked 6676).
* Now (`worldact::OwnTalkGiftLot`): the trigger is never a world action; the guest's own SetEventFlag
  of it while summoned queues its own grant (`items::OfferOwnGift`, ItemSource::Own, the lot's
  personal flag: once per player), given while summoned or at home like the Messenger gifts. Joining
  the League (Valtr's state, his talk block) is a covenant outcome and stays in the host's world
  (event_flags.md 7); the rune is the guest's own item.

