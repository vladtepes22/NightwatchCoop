# Phantom restrictions: what stops a guest from being a full player, and how to lift it

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA of
`smoketest/out/eboot.elf`; XML `Address` = ours + 0x400000; bbhost = ours + 0x400000). Byte
strings were read from that ELF; patched instructions were disassembled with capstone. Game data
was read from `E:\games\bloodborne\dvdroot_ps4` (gameparam, paramdef, `event/*.emevd`,
`script/*.luabnd`). Decompiles: Ghidra (`tools/re/decomp.py`). Status labels: **[C]** confirmed
statically (code read or data read), **[I]** inferred (needs the runtime probe listed with it).

Not repeated here (agent A6 / earlier docs): boss-kill session end, EMEVD dispatch filter,
map-reload retarget, HP 0.7 -> 1.0 (SpEffect 9006/9026), bell/Insight gates, PLAY ONLINE
(`seamless_rules.md`); travel, lamp `+0x49` gate, Dream area table, send-home callers
(`travel.md`); item rewards and flag sync (`event_flags.md`, other agent).

## 0. Summary

| # | Restriction (vanilla) | Gate | Lift | Risk | Status |
|---|---|---|---|---|---|
| 1a | Item use by a phantom | `EquipParamGoods.enable_white` (byte 0x44 bit 2), tested in goods availability 0x157F200 at 0x157F99B | Nothing needed for vials, bullets, Insight items, Coldbloods (already enabled). Optional: param write per row, or byte patch 0x157F99F | low | C |
| 1b | Vials/bullets refill | Auto-replenish 0x14DACE0, run only from MoveMapStep 0x1939470 for respawn-type loads or loads into the Dream | Director calls 0x14DACE0 on the guest (own storage) at chosen moments; optional patch 0x19398D7 | low | C (guard values I) |
| 2a | Boss fog: guest first | EMEVD "guest enters boss room" needs host-entered flag (e.g. 12414800) and chr type 1 | Lifted (0.6.1, B2): the client passes the host-entered condition, the host sets its flag on the guest's `world_action` `fog` report (ERSC_PARITY B2) | — | C |
| 2b | Multiplayer confinement walls (18 walls on block boundaries) | common event 7600, enabled while multiplayer | Index skip `7600@6`, `7600@7` together with travel B1 | medium (map-block crossing) | C |
| 2c | Lamps / Dream warps / Doll / shops for guests | EMEVD `1003[6] End if Client` at instruction 0 (7200, 7300, 12107000, 12107100, 12105043, ...) | Keep (travel is host-led, `travel.md`) | — | C |
| 2d | Doors, levers, elevators, ladders | No phantom gate found; clients' ObjAct starts are replicated (packet 0x2d) | Nothing; test | low | I |
| 2e | Item pickup / lot award | MapItemMan 0x17D6780 and award 0x17DDC50 skip guests in another world (+0x1592 == 0); chr-type table byte 2 | Other agent (C3) | — | C (gate located) |
| 3a | Guest death -> sent home | Death dispatch 0x13813D0 -> PartyGhostDeath_2 0x13824C0 -> call 0x13CDE30 at 0x1382663 | Redirect the warp to the host's lamp (0x13CDF30) in the travel hook, then auto-rejoin | low-medium | C |
| 3b | Revive in place | No native revive for a live phantom | Not recommended (see 3.3) | high | I |
| 3c | Host death | SoloPlayDeath_2 0x1381DE0; guests HostDead_1 | travel B1 (follow) | — | C (travel.md) |
| 4a | Phantom tint, name colour | Chr type 1 (draw), session type descriptor +0x24 (phantom draw param) | Cosmetic; leave, or override the descriptor value after summon | low | I |
| 4b | Summon timer | None: SpEffect 9003/9005/9006/9025/9026 `effectEndurance` = -1 | — | — | C |
| 4c | Distance leash | None found | — | — | I |
| 5a | Insight items (Madman's Knowledge, ...) | enable_white = 1 | Nothing | — | C |
| 5b | Insight on boss kill / first encounter | common event 9350 (`End if Client`), called only from host branches; a white phantom gets a native **+1** per boss kill (BlockClear2 0x13849A0, SpEffect 4680 at 0x13852B1) | Director mirrors the host's boss-kill N (parity: N - 1 on top of the native +1); non-boss 9350 starts and the bare SpEffect 4680 grants of 9422 (kidnap) and the m29 chalice boss events (12901588/589/591/708/712/718/722, all `End if Client` or chr type 0) are counted per event and sent whole (DirectInsight) | low | C (gate, +1 site), I (4680 = +1, runtime) |
| 5c | Boss echoes | BlockClear handler 0x13849A0 pays PlayerGameData+0xF8 to chr types 0, 8 **and 1** | Nothing | — | C |
| 5d | Kill echoes | Phantom SpEffects have `soulRate` 1.0 | Nothing (verify distribution) | — | C (data), I (who gets the kill) |

## 1. Blood Vials, Quicksilver Bullets and other consumables

### 1.1 Use [C]

`FUN_0157F200(goodsId, PlayerIns*, ?, chrType, weaponR, weaponL, flag)` is the general goods
availability check (the bell gates in `seamless_rules.md` live in it as well). Callers pass
`ecx = PlayerIns+0x78` (e.g. 0x18C3683 `mov ecx,[r12+0x78]` before `call 0x157F200` at
0x18C369A). The enable bit is picked by chr type:

```
157f978: cmp eax,8 ; je   -> shr [goods+0x42],0x11   (enable_gray,  byte 0x44 bit 1)
157f97d: cmp eax,2 ; jne  -> shr ...,0x13            (enable_black, bit 3)
157f996: cmp eax,1 ; jne 0x157f9a5
157f99b: 45 8b 65 42        mov r12d,[r13+0x42]
157f99f: 49 c1 ec 12        shr r12,0x12              (enable_white, bit 2)
157f9a3: eb 04              jmp 0x157f9a9
157f9a5: 45 8a 65 44        mov r12b,[r13+0x44]       (enable_live,  bit 0)
157f9a9: 41 80 e4 01        and r12b,1                -> required to be 1
```

Other terms of the final AND that matter for co-op: `enable_multi` (bit 4) must be set when
the presentation member count > 1 or an SOS is active; `disable_offline` (bit 5).

Data (EquipParamGoods, 1.09) - `L G W B M dO` = enable_live/gray/white/black/multi,
disable_offline:

| Goods | L G W B M | Notes |
|---|---|---|
| 1000 Blood Vial | 1 1 1 1 1 | isAutoReplenish 1, useHpCureMaxNum 1, isGuestDrop 1, max 20 |
| 900 Quicksilver Bullets | 0 0 0 0 0 | not a "used" item (consumed by guns); isAutoReplenish 1, useBulletMaxNum 1, isGuestDrop 1 |
| 1500/1501 Madman's Knowledge / Great One's Wisdom | 1 1 1 1 1 | canMultiUse |
| 1510-1594 Coldbloods | 1 1 1 1 1 | |
| 100 Hunter's Mark, 1400 Bold Hunter's Mark | 1 1 0 0 0 | not for phantoms |
| 200 Beckoning Bell | 1 1 0 0 1 | |
| 4320-4323 Umbilical Cords | 1 1 0 0 1 | Insight +, story flags: keep host-only |

**So a guest already heals with its own vials and uses its own bullets, Insight and echo
items.** The guest's PlayerGameData (inventory, Insight +0x84, echoes) is its own and is not
overlaid in the host's world (only event flags are, `event_flags.md` section 3), so what the
guest spends or gains persists in its save [I: confirm by save diff after a session].

Optional lifts:
- **Per item (recommended)**: runtime param write in `seamless_rules.cpp` (same mechanism as
  9006/9026): set bit 2 of byte 0x44 of the chosen EquipParamGoods rows. Candidate: none needed
  for the core loop; maybe 100 Hunter's Mark (see 3.2).
- **Global byte patch** (not recommended): `0x157F99F 49 c1 ec 12 -> 49 c1 ec 10` (white uses the
  `enable_live` bit). That would also enable the Beckoning Bell and the Umbilical Cords for
  guests. Risk: medium (story items in someone else's world).

### 1.2 Refill (auto-replenish) [C, guard semantics I]

`FUN_014DACE0()` walks the local PlayerGameData inventory (`*(GameDataMan+8)+0x5B0`), and for
every goods row with `isAutoReplenish` (`*(uint48*)(goods+0x42) & 0x10000000000`, byte 0x47
bit 0) tops it up from storage (`0x14D9A80` on PlayerGameData+0x328, `0x14DA750`). Max counts
come from `0x1F1E7E0` (useBulletMaxNum -> goods 900 maxNum, useHpCureMaxNum -> goods 1000 maxNum,
runtime overrides at 0x5593034/0x5593038). It is called only from the MoveMapStep step
`0x1939470`, together with `0x14DBB70` (resets goods 0x40000385 = 901 Blood Bullets):

```
19398b1: 41 80 be e4 00 00 00 00   cmp byte [r14+0xe4],0     ; step flag (respawn-type load?) [I]
19398b9: 74 0a                     je  19398c5
19398bb: 41 83 be a4 00 00 00 04   cmp dword [r14+0xa4],4    ; load type 4 excluded [I: summoned reload]
19398c3: 75 14                     jne 19398d9               ; -> replenish
19398c5: 41 81 7e 58 00 00 00 15   cmp dword [r14+0x58],0x15000000   ; loading into m21_00 (Dream)
19398cd: 74 0a                     je  19398d9               ; -> replenish
19398cf: 41 80 be ed 00 00 00 00   cmp byte [r14+0xed],0
19398d7: 74 0a                     je  19398e3               ; skip
19398d9: e8 92 22 ba ff            call 0x14dbb70
19398de: e8 fd 13 ba ff            call 0x14dace0
```

(The whole block runs only when the local PlayerIns was re-used, `bVar3`.) Answers:
- A guest's vials come from **its own** inventory and refill from **its own** storage; nothing is
  taken from the host.
- In vanilla a guest refills when its own world reloads after it is sent home if that load is
  respawn-type, and whenever it loads into the Dream. Whether the summoned map reload (type 4?)
  refills: probe R1.
- Host lamp use does not refill anyone directly; the following load (respawn/Dream) does.

Lift options:
- **Director call (recommended)**: on the guest, main thread, in the world and not loading, call
  `void 0x14DACE0(void)` (and optionally `0x14DBB70`) when the host rests at a lamp (TRAVEL
  kind lamp, `travel.md` 3.1) or after a guest respawn. Pure local inventory operation on the
  guest's own data; nothing is replicated. Risk: low.
- Patch (blunt): `0x19398D7 74 0a -> 90 90` refills on every successful world load (also the
  summoned reload and every host warp). Risk: low technically, but it makes vials free on any
  load.

Probe R1: break on 0x14DACE0; log `[r14+0xa4]`, `[r14+0xe4]`, `[r14+0xed]`, `[r14+0x58]` for
death, lamp, Dream, summoned reload and sent-home loads.

## 2. Interacting as a phantom

### 2.1 Boss fog [C]

Per boss, the map EMEVD has `ホストがボス部屋入場` (host enters: needs chr type 0, writes
`120x48y0`, e.g. 12414800 for Gascoigne), `ゲストがボス部屋入場` (guest enters: 2000[2]
network-synced, needs `12414800` ON, chr type 1, action button 3[24] on the fog) and
`時間差入場ゲスト用対処処理` (late-guest fixup, `1003[6] [0,0]` = end if host). Native side: Lua
`Lua_MultiWall 0x138B720` / `Lua_InvalidMultiWall 0x138B890` toggle the fog object
(`0x13CABD0(entity,1|0)`, `0x13CA580`). So **guests can always enter after the host has**;
host-first is consistent with "boss fight starts on the host". Lifting it would require the
guest to start the fight in its overlay world: not recommended.

### 2.2 Multiplayer confinement walls (area limit) [C]

common event **7600 `マルチ閉じ込め壁_XX`** (multiplayer confinement wall), args (object, SFX):

```
0 2000[2] network sync
1 2005[3] object enable (X0, 0)        ; off
2 2006[1] map SFX delete (X1)
3 3[6] mp state 3   4 3[6] mp state 2  ; OR group
5 0[0] wait
6 2005[3] object enable (X0, 1)        ; ON while in multiplayer
7 2006[2] map SFX create (X1)
8..10 wait until neither state
11 1000[4] end (restart)
```

18 instances, all on block boundaries: m22 2201999, m23 2301999, m24_00 2401995-2401999,
m24_01 2411997-2411999, m24_02 2421998/2421999, m27 2701998/2701999, m28 2801998/2801999,
m32 3201999, m35 3501990. These walls are the vanilla "phantom area limit": they stop a session
from crossing streamed map blocks, which would end it (host stage-UID check 0x193C657,
`travel.md` 1.0 / probe P2).

Lift: skip only instructions 6 and 7 of event 7600 (the A6 filter knows the index at
`event+0xA0`; the rule syntax needs an index: `BB_PARTY_EMEVD_SKIP="7600:2005:3@6,7600:2006:2@7"`).
Do not skip by `(event, bank, id)` alone: 2005[3] at index 1 is the "off" write. Enable it only
together with travel B1/B2 handling of a block change, otherwise crossing ends the session.
Risk: medium (session end on crossing; no softlock: walls are cosmetic objects plus collision).

### 2.3 Lamps, Dream travel, Doll, Messenger shops [C]

EMEVD guards `1003[6] [0,1]` (end if client) at instruction 0: 7200 lamp -> Dream warp, 7300
Dream -> lamp warp, m21 12107000/12107100 (headstones), 12105043 (Insight shop messenger),
12105064 (dress-up messenger), 12105300 (chalice unlocks), 9191/9192 (time-of-day). The lamp
prompt itself has the `+0x49` multiplayer gate (0x12F8315, `travel.md`). Keep as is: travel is
host-led (B1), and guest-side lamp/menu use would warp the guest out of the room (0x13CDE30
leaves when `+0x1592 == 0`). A guest that wants to level/shop does it between sessions or after
following the host into the Dream (each player in its own world after B1). Menu flag
`+0x2E` set in 0x17589E9 (`multiplayer || !own world`) probably greys some menu entries [I].

### 2.4 Doors, levers, elevators, shortcuts, ladders [I]

- No chr-type, role or own-world test was found in the ObjAct managers (26 functions referencing
  SprjWorldObjActMan 0x553B0F0) nor in the action-button region system (0x12FABE0, 0x12FD880,
  0x12FF9D0). ObjActParam has only key/SpEffect qualifications (`spQualifiedType` 1/2).
- 0x13AE600 (ObjAct state update) sends packet **0x2D** (ObjAct kick, `{block, idx, entity}`)
  to all session peers whenever the local player kicks one, and keeps a client-side (role 6)
  pending entry with a 10 s timeout; role 3 sends 0x2F. So the engine supports a client
  starting an ObjAct and replicates it.
- Door/lever EMEVD events (115 events with 5[2] "ObjAct executed") carry no multiplayer or
  chr-type guard; 6 are network-synced.
- Ladders: native (SprjPlayerLadderModule), no gate found.

Expected: guests can open doors / pull levers / ride elevators, and the result appears in the
host's world. Effects on the guest's **own** save (shortcut flags) need C2 progress sync
(`event_flags.md`). Probe R2: as a guest, open a lever door; log 0x13AE600 role-6 branch and the
door flag on both sides.

### 2.5 Item pickups and item lots (location only) [C]

- `MapItemMan_RemoveEntryLocal 0x17D6780`: the event/flag bookkeeping after a pickup runs only
  when `(role != 6 && slots+0x98 == -1) || GSM+0x1592 != 0`.
- `AwardItemLotWithClientPolicy 0x17DDC50` (EMEVD 2003[4]/[36], boss rewards): at
  0x17DDCD9..0x17DDCFB the same test sends a summoned guest to the "not eligible" path
  (`xor eax,eax` at 0x17DDD2A) unless `param_3` (2003[36] "client also gets item"); otherwise
  eligibility is byte 2 of the chr-type table below.
- Floor-treasure visibility per chr type: `0x17E3C20` reads lot flags at `+0x15F`
  (0x20/0x40 table byte 0, 0x80 type 1, 0x100 type 2) - `isGuestDrop` vials/bullets.

ChrType attribute table at **0x47348A0** (12-byte rows, types 0..13, default row 0x4734948):

| type | b0 | b1 | b2 | b3 | b4 | b5 | b6 | b7 | dword |
|---|---|---|---|---|---|---|---|---|---|
| 0 host body | 0 | 0 | 1 | 0 | 0 | 1 | 0 | 1 | 1 |
| 1 white | 1 | 0 | 1 | 0 | 0 | 1 | 1 | 0 | 2 |
| 2 black | 1 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 3 |
| 8 grey | 1 | 0 | 1 | 0 | 0 | 1 | 0 | 1 | 4 |
| 12 | 1 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 13 |

Readers: b2 item lots (0x17DC300, 0x17DD0B0, 0x17DDC50); b6 PlayerIns (0x18F3AF0, 0x18F4DB0,
0x18F84F0); b7 position history 0x18F7FB0; b0/b1 damage/team code (0x1A216B0, 0x1A249D0). Do
not edit this table for co-op: it is global and also decides how the host treats the guest.

### 2.6 NPC dialogue [I]

No native talk gate was located. Quest NPC state lives in event flags that, for a guest, are
the host's overlay copy (discarded). Recommendation: let guests talk (harmless in the overlay),
but treat NPC quest progress as host-authoritative (C2).

## 3. Death

### 3.1 Vanilla [C]

`OnEvent_4000 0x13813D0` (death dispatch) reads `PlayerIns+0x78`:

```
13814b6: mov eax,[rax+0x78] ; sete bl (0) ; cmp 8 ; cmp 1 -> [rbp-0x34] ; cmp 2 ; cmp 0xc
```

types 1/2/0xC -> Lua state `PartyGhostDeath` (0x1381FD0) -> `_wait` (0x1382070) ->
`_1` (0x1382130, death message, summon-type dependent) -> `_2` **0x13824C0**:
- clears GSM+0x1690/+0x16B0 snapshots;
- blood mark (echo drop) only for types 2/0xC (`(int)plVar3[0xf] == 2 || == 0xc`): **a white
  guest loses no echoes** (bbhost agrees);
- `GameDataMan+0x70 = 1` (RequestFullRecover: full HP/stamina/durability after the next load,
  consumed in 0x1939470 -> 0x18FD460);
- `GSM+0x1520 = 1` (forced placement = pre-summon spot), selector 0x1332BC0, then
  `0x1382663: e8 c8 b7 04 00  call 0x13CDE30` -> the guest leaves the room immediately
  (`+0x1592 == 0`) and loads its own world.

### 3.2 Respawn at the host's lamp (recommended) [C sites, I timing]

Use the B1 hook on `0x13CDE30` (`travel.md` 3.2): when the caller is **0x1382663**
(PartyGhostDeath_2) and party mode is on, clear `GSM+0x1520` and go through `0x13CDF30(id)`
with the host's last-lamp id (low dword of the host's `GSM+0x1528`, sent in PROGRESS) instead
of the pre-summon spot. The guest then loads its own world at the host's lamp, the director
rings the Small Resonant Bell and the host's director re-summons it (auto-rejoin, A6). With 1.2
the director can refill the guest's vials after the load.

No byte patch: a byte retarget of 0x1382663 would lose the lamp id argument. Risk: low-medium
(two loading screens, ~10-15 s out of the fight; no desync: the host's game already treats it as
a leave).

Guest-used Hunter's Mark (goods 100 is not white-enabled; if enabled through 1.1): it reaches
`OnReviveMagic 0x1389D20` (type 1: notify 0xFD7, then BeginLeaveSession) -> `OnReviveMagic_1`
-> `0x13CDE30` from 0x1389F20; the same redirect turns it into "return to the host's lamp".

### 3.3 Revive in place (not recommended) [I]

Lua `RevivePlayer` (0x1332B20) -> `0x1900710` (PlayerIns revive reset: chr type := 0, HP to
max) exists, but is used for the local body at load time (InGameStart, `IsReviveWait` =
`SprjEventState+0x26`). For a live phantom, the host's NetworkManipulator copy has already
played death, the session member state is "dead" and slot bookkeeping (0x15BE2A0 death variant)
ran; nothing re-inserts a remote character without a reload (`travel.md` 4, "very high"). It
would also set chr type 0 on the guest (host-only EMEVD branches then fire on the guest).
Desync and softlock risk: high.

### 3.4 Host death [C]

`SoloPlayDeath_2 0x1381DE0` (host reloads at its last lamp, room closes during the load), guests
get `HostDead_1 0x1381A60` (caller 0x1381A93 of 0x13CDE30). B1 covers it (TRAVEL kind host
death, guests warp to `+0x1528`). `ChrIns_OnDeath 0x18C49F0` role-3 branch requests the return
of every task; nothing extra to patch.

## 4. Presentation, timers, leash, area changes

- **Summon timer** [C]: none. SpEffect 9003/9005/9006/9025/9026 have `effectEndurance` -1.0
  (permanent). The only timers are the SOS sign lifetime (55 s / 180 s, A6 patches 30 s) and the
  NPC task return timer (20 s, presentation only).
- **Tint / name** [I]: phantom drawing follows chr type 1 (and PlayerIns draw flags at +0x1E1;
  `0x1006` mask types 1/2/12 start with the event-draw bit off) plus the session-type descriptor
  `+0x24` "phantom parameter" written on summon (`npc_peer.md` 1.3/1.4; table 0x553D750 is
  built at runtime). A cosmetic override is possible by rewriting that descriptor field after the
  table is built; dump rows first (probe R3). Chr type itself must stay 1: it drives the host-
  vs-guest EMEVD branches (`4[3] [x,10000,0]` host-only, `[x,10000,1]` guest-only) and the
  death dispatch.
- **Distance from the host** [I]: no leash code found (no distance check against the host in
  the session/cooperator paths read for this and the earlier docs).
- **Host changes area** [C]: streamed block change -> confinement walls (2.2) prevent it during
  co-op; loading transitions end the session (`travel.md`), B1 brings guests along.

## 5. Insight and echoes

- **Insight consumables** [C]: Madman's Knowledge 1500, Great One's Wisdom 1501 are
  white-enabled with `canMultiUse`; the Insight goes to the guest's own PlayerGameData+0x84.
- **Insight from bosses, resolved** [C]: both earlier statements hold, for different grants.
  (1) The *host's* boss Insight is common event 9350 (below): host-only, guests never run it.
  (2) A white phantom gets its own **+1 per boss kill** natively: BlockClear2 `0x13849A0`, branch
  `PlayerIns+0x78 == 1` (`0x1384CF4`), after the clear bonus is paid (`0x13851EE..0x138525D`):
  `0x138527C..0x13852B1` `mov esi,0x1248` (4680), `rdx = rdi = PlayerIns`, `ecx = r8 = r9 = 0`,
  `[rsp] = 0`, `xmm0..4 = 1.0f` (`0x4925B88`), `call [vtbl+0x3F0]` (the ApplySpEffect vfunc; same
  convention as MultiDoping `0x138BD48`). Condition: `[0x553B108] != 0` and a local PlayerIns
  (also the whole state is skipped when `[[0x553B108]+0x60]+0x40 > 0`, meaning not traced [I]).
  So vanilla: host +N (2-5, from 9350), cooperator +1. Data (all `2000[0] (slot, 9350, N)` calls,
  `flag_tool` EMEVD reader): every boss-kill event (`ボス撃破_*`, `*_ボス撃破`) has a boss-defeat
  instruction `2003[12]`/`[53]` (or `[15]`) before its 9350 start; first-encounter, area and story
  starts have none. Implemented (`gpu/shim/party/party_phantom.cpp`): the host's EMEVD filter pairs
  `2003[12/15/53]` with the `2000[0](_, 9350, N)` of the same event (120 s window) and sends EVENT
  `phantom {insight, n}`; a guest that was a client in the last 90 s applies SpEffect 4680
  `GuestInsightGrant(N)` times (parity default: N - 1, so it ends at N like the host; `full`: N),
  15 frames apart (9350 waits 10 frames between applications). Probe R5 still open.
- **Insight from bosses (host)** [C]: common event **9350 `SAN値獲得`** applies SpEffect 4680 N times
  (labels 1..9) and starts with `1003[6] [0,1]` (end if client). It is started (2000[0]) from
  host-only branches: e.g. Gascoigne kill 12411800 jumps to its client label at index 27
  (`1003[105] [1,1]`) before index 29 `2000[0] [0,9350,2]`; first entry 12411802 needs chr type
  0. 63 call sites. Guests therefore get no boss Insight. Lift (recommended): the director
  mirrors it - when the host's party flag sync reports a boss-defeat / first-encounter event that
  started 9350 with N, the guest applies SpEffect 4680 N times to itself (2004[8] equivalent:
  native SetSpEffect on the local PlayerIns) or writes Insight via `coop::WritePlayerInsight`.
  Do not skip the client jump in the boss events: it would also run the item award, trophy and
  flag writes on the guest. Risk: low.
- **Boss echoes** [C]: the boss-clear handler `0x13849A0` takes `PlayerGameData+0xF8`, zeroes it
  and adds it through vfunc +0x3D0 for chr types 0, 8 and 1 (type 1 branch also awards
  cooperator lots 5900/5920 (Vermin, when the PlayerIns has SpEffect 6142, or 6140 + 6150 -
  presumably the League rune [I]) and 5930/5950) - guests already get boss echoes.
- **Kill echoes** [C data, I distribution]: phantom SpEffects keep `soulRate` 1.0 /
  `haveSoulRate` 1.0. Who receives an enemy's echoes (killer only vs. all players) was not
  traced; probe R4: kill an enemy as host and as guest, compare both echo counters
  (PlayerGameData souls).
- **HelpWhiteGhost counter**: `WhiteReviveCount` Lua binding (0x138B8E0) increments
  GameDataMan+0x78 when role != 6 (statistic only).

## 6. Patch / rule list (byte-verified against out/eboot.elf)

| Name | Site | Original | Replacement | Default | Risk |
|---|---|---|---|---|---|
| (opt) White uses enable_live | 0x157F99F | `49 c1 ec 12` | `49 c1 ec 10` | off | medium |
| (opt) Refill on every load | 0x19398D7 | `74 0a` | `90 90` | off | low |
| Confinement walls off (EMEVD index skip) | event 7600 instr 6 `2005[3]`, instr 7 `2006[2]` | — | skip | off, with B1 | medium |
| Guest death -> host lamp | 0x13CDE30 hook, caller 0x1382663 (`e8 c8 b7 04 00`) | — | `0x13CDF30(host lamp id)` | on in party mode | low-medium |
| Guest vial refill | call `0x14DACE0()` (+`0x14DBB70()`) on the guest, main thread | — | runtime | on lamp/respawn | low |
| Guest boss Insight | SpEffect 4680 x (N - 1) on the guest (vfunc +0x3F0) | — | runtime | on (parity) | low |
| (opt) Hunter's Mark for guests | EquipParamGoods 100 / 1400 byte 0x44 | `0x43` / `0xC3` | `0x57` / `0xD7` (+ white, multi) | off | low-medium |
| Guest Hunter's Mark -> host lamp | tail jump 0x1389F20 (`e9 0b 3f 04 00`) retargeted | — | `0x13CDF30(host lamp id)` | on in party mode | low-medium |
| Per-goods white enable | EquipParamGoods row byte 0x44 bit 2 | 0 | 1 | none needed | low |

## 6a. Implementation (party_phantom, branch agent/phantom)

`gpu/shim/party/party_phantom.{h,cpp}` with small extensions in `party_travel` (guest redirect),
`seamless_rules` (rule syntax, observer, Hunter's Mark param rules), `coop_hooks` (`HookTailJump`),
`party_director` / `party_runtime` (EVENT `phantom`). Env switches are listed in
`party_phantom.h`. Status: built, unit-tested (`party-phantom-test`); not yet run in a live
2-instance session (the test harness cannot leave the title screen without input).

| Item | Where | Notes |
|---|---|---|
| Guest death -> host lamp | `party_travel` FunnelHook, guest branch, return address 0x1382668 | pending travel destination first (`RetargetSendHome`), else `0x13CDF30(host lamp)` with `+0x1520` cleared; `GameDataMan+0x70` (full recover) kept |
| Guest Hunter's Mark -> host lamp | `HookTailJump(0x1389F20)` | same choice; only reachable with `BB_PARTY_GUEST_MARK=1` |
| Host lamp id | host polls `+0x1528` (own world) once a second, EVENT `phantom {lamp}` on change or membership change; any TRAVEL's `last_lamp` updates it too | guest: `SetGuestDeathRedirect` |
| Auto-rejoin | the director's Small Resonant Bell (idle in own world, link up) | no new code |
| Refill | guest tick: `0x14DBB70()` then `0x14DACE0()` after a steady world (30 frames, no load / stage request / join) | triggers: EVENT `phantom {rested}` (host travel into the Dream, host death, host Mark), own redirected respawn; waits for the follow load (45 s cap) |
| Confinement walls | built-in rules `7600:2005:3@6`, `7600:2006:2@7` | only while `TravelEnabled()`; `BB_PARTY_OPEN_WORLD=0` drops them |
| Rule syntax | `event:bank:id[@index]`, index = `event+0xA0` | `BB_PARTY_EMEVD_SKIP` |

## 7. Probes

- R1 replenish guard values per load kind (1.2).
- R2 guest ObjAct (lever door): packet 0x2D path in 0x13AE600, door flag on host and guest (2.4).
- R3 dump SessionTypeDesc rows (0x553D750, stride 0x80) for the human white type; field +0x24
  (4).
- R4 kill-echo distribution (5).
- R5 confirm SpEffect 4680 = +1 Insight per application (apply once offline, read +0x84):
  `BB_PARTY_PHANTOM_PROBE=insight` (with `BB_PARTY_DIRECTOR_TEST=log_state`) applies it once 5 s
  after the world is up; the director logs `Insight a -> b`. `BB_PARTY_PHANTOM_PROBE=refill` runs
  the refill once.
- R7 guest death redirect: kill the guest; expect `Party travel: guest_died at +0x1382663: to the
  host's last lamp ...`, one load into the guest's world at that lamp, `refill (guest respawn)`,
  then the bell and a rejoin.
- R8 open world: walk the host across a confinement-wall boundary (e.g. m24_01 2411997-2411999)
  with a guest; the wall must not appear (`EMEVD ... skipped` count grows) and observe whether
  the stage-UID stop 0x193C657 ends the session (travel.md P2).
- R6 after a session, diff the guest save: inventory, Insight, echoes persist; flags do not.

## 8. Nothing waits for "home" (seamless play check, 2026-10-10)

Checked for B2 (guests summoned nearly all the time): vial / bullet refills (`RefillQueue`, gate
`ReplayAllowedNow`: world up, no load, not joining / leaving - role 6 passes; the host-rested
request falls back to `kLoadWait` when no follow load comes), boss Insight (`InsightDrip`, same
gate, SpEffect 4680 on the own PlayerIns; Insight lives in the own PlayerGameData), the host-lamp
announcement and the death redirect: none needs the own world. Echoes, Insight and inventory are
the guest's own PlayerGameData; they persist through the save requests that summoned C2/C3/C4
writes now trigger (items.md 10).
