# Party travel (Phase B): lamp, Hunter's Dream, Hunter's Mark, transitions, death

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA; bbhost source = ours + 0x400000;
bbhost `.hpp` `Rva{}` = ours; shadp2p = ours). Decompiles are Ghidra (`tools/re/decomp.py`), disassembly is
capstone over `smoketest/out/eboot.elf`. Sources besides our own RE: bbhost `network_script_state.hpp`,
`session_manager.hpp`, `cs/session_connect_state_step.hpp`; shadp2p `documents/bloodborne-seamless-re.md`
(runtime captures, cited as [shadp2p]).

Status markers: **[static]** proven from code here, **[runtime-shadp2p]** proven by shadp2p captures,
**[inferred]** name/shape based, needs one runtime confirmation (each has a probe listed in section 5).

## 0. Globals and names used below

| Symbol | Address | Notes |
|---|---|---|
| `GameStateMan`/WorldTransitionState ptr | `0x5556678` | `+0x08` transition requested, `+0x0C` packed target map (area<<24 \| block<<16 \| region<<8 \| idx), `+0x10` warp point / bonfire id, `+0x1520` forced-placement byte, `+0x14F0/+0x1500/+0x1510` forced map/pos/rot, `+0x1524`, `+0x1528` qword last-lamp record `{id, aux}`, `+0x1538` respawn mode (1 world, 2 Dream), `+0x153C` qword respawn record `{id, aux}`, `+0x1590` online, `+0x1592` "in own world" byte (see 1.0), `+0x1690/+0x16B0/+0x16D0` reload snapshots, `+0x16F8` session/presentation state |
| `SprjSessionManager` | `0x5540290` | assert string in every user names it so. `+0x124` role (0 idle, 1 creating, 3 host, 4 joining, 6 client, 7 leaving), `+0xF8..+0x100` peer vector (stride 0x20), `+0x280` leave step |
| `CSMultiPlayMan` | `0x5540230` | separate singleton (assert in `0x19470F0`); `FUN_01E54830(CSMultiPlayMan)` resets it |
| Summon-reload phase object | `0x553D6D0` | `+0x84` phase (3 = guest map-reload into host world), `+0xF0` |
| `BeginLeaveSession` | `0x1ED07A0` | the plan's "CSMultiPlayMan::Stop". Roles 0/2/5 → false; else role=7, `0xC8ED70` → `0xC98070(ctl, 0xFF000023)` → … → `sceNpMatching2LeaveRoom` |
| Reset tracked entities | `0x15BED10(+0x16F8 state)` | called after every session-ending site; clears presentation members |
| PlayerIns | `*(WorldChrMan 0x553E878 + 0x60)`; `+0x78` chr type: 0 own-world body, 8 own-world (dead/grey), 1 white phantom, 2 black phantom, 0xC other phantom |

`0xC8ED70` (the only place that supplies reason `0xFF000023`):
```c
uVar2 = FUN_00c98070(*param_1, 0xff000023);   // enqueue matching stop task 0x0C
```
`0x1ED07A0`:
```c
if (role > 5 || ((0x25 >> role) & 1) == 0) {   // not 0/2/5
  *(int*)(this+0x124) = 7;
  if (FUN_00c8f570(this+0x18) && FUN_00c8ed70(this+0x18)) { alloc leave step at +0x280; set bit 0; return 1; }
  FUN_01ed0910(this);                            // reset to idle
}
return 0;
```

## 1. Code paths

### 1.0 The common funnel: `SessionWorldTransition 0x13CDE30`

Every stage change goes through this function (19 call sites, table in 1.6):
```c
undefined8 FUN_013cde30(void) {
  *(u8*)(GSM + 8) = 1;                                    // arm stage request
  lVar1 = *(long*)(GameDataMan + 8);
  if (lVar1) {
    *(u32*)(lVar1 + 0xa4) = PlayerIns ? PlayerIns->+0x78 : -1;   // remember chr type
    FUN_014ccd40(lVar1 + 0x1d0, *(u8*)(lVar1 + 0xca));
    if (*(u8*)(GSM + 0x1592) == 0) {                      // 0x13CDEA2: cmp byte [rax+0x1592],0 ; jne 0x13CDF1C
      st = *(GSM + 0x16f8);
      if (BeginLeaveSession(SprjSessionManager))          // 0x13CDEF6
        *(u32*)(ReloadPhase + 0xf0) = *(u32*)(GameDataMan->+8 + 0xa4);
      FUN_015bed10(st);                                   // drop presentation members
    }
    return 1;
  }
  return 0;
}
```
**What `+0x1592` means** [static]: writers are the GSM ctor (`0x1566743`, =1), the setter `0x156EEA0`
(`mov [rax+0x1592], dil`), stage update `0x1942216` (=1 after a load), and `MoveMapStep` ctor
`0x1937570`: `=0` when `ReloadPhase+0x84 == 3` (guest loading into the host's world, host event flags,
`FrpgNetMan+0xB14 |= 8`), `=1` otherwise (own world, own flags restored). So `+0x1592 == 0` means "I am a
summoned guest in someone else's world". Consequence:

* **Guest** (`+0x1592 == 0`): any warp leaves the room *immediately* inside `0x13CDE30` (`0x13CDEF6`).
* **Host/solo** (`+0x1592 == 1`): `0x13CDE30` does **not** leave. The host's room dies later, during the
  load, from one of two stage sites:
  * `0x19471B1` in the stage payload destructor `0x19470F0` — whenever `ReloadPhase+0x84 != 3`
    (that is always for a host):
    ```c
    if (*(int*)(ReloadPhase + 0x84) != 3) {
      if (BeginLeaveSession(SprjSessionManager)) *(ReloadPhase+0xf0) = GameDataMan->+8->+0xa4;  // 0x19471B1
      FUN_01e54830(CSMultiPlayMan);                         // tear down multiplayer ins tasks
    }
    ```
    [runtime-shadp2p: proven caller with reason 0xFF000023 on a guest; same branch for a host by static flow]
  * `0x193C657` in stage update `0x193AC10`: when the active stage record UID (`stage+0x28[idx*0xA0]+8`,
    `idx = stage+0x118->+0x20`) differs from `PlayerIns+0x3F8`, it caches the new UID and, **only for role 1
    or 3 (host)**, calls `BeginLeaveSession` then `0x15BED10`.
    ```
    193c537: cmp r13d, [rax+0x3f8] ; je 193c691      ; same map → nothing
    193c5de: mov eax,[rdi+0x124] ; cmp eax,3 ; je 193c61d ; cmp eax,1 ; jne 193c67b
    193c657: call 0x1ed07a0                             ; host leaves
    193c689: call 0x15bed10
    ```
  Which of the two fires first for a host warp is a runtime detail (probe P1); both end in reason
  `0xFF000023`, both must be handled for B2, neither matters for B1.

### 1.1 (a) Host uses a lamp (world lamp → Hunter's Dream)

0. **Vanilla blocks it while guests are present.** The lamp action object update `0x12F5C40` computes
   four "unavailable" bytes; byte `+0x49` is the multiplayer gate (`0x12F8315`):
   ```
   12f8303: mov rcx,[rax+0x100] ; sub rcx,[rax+0xf8] ; shr rcx,5   ; SprjSessionManager peer count
   12f8315: cmp dword [rbx+0x14],1 ; setg al                        ; presentation_count > 1
   12f831c: test ecx,ecx ; setg cl ; or cl,al
   12f8323: mov [r13+0x49], cl
   ```
   `+0x48` (lamp type 0x7A8 + event state), `+0x4A` (no lamp record `0x13C9AF0` or record `+0x270 == 0`),
   `+0x4B = FUN_0191B6E0(WorldChrMan, lampEntity)` (computed only if `+0x48/+0x49/+0x4A` are all 0: scans
   the other chr sets for a character whose current action-target entity equals the lamp and whose state
   is in table `0x5127DF0`). The "action-button row" (+0x2C; 2026-10-11: really the flame **SFX id**, see
   guest_services.md 3.3) is `r15 = 90000 + type` when all are 0, `90000` when
   `+0x49`/`+0x4B`, `90005` for `+0x48`, `-1` for `+0x4A`; the final enable is
   `FUN_0146E250(entity, all4==0, type)` at `0x12F8EBF` (writes `FrpgNetMan+0xC70` list entry `+0x48/+0x49`).
   Every interaction path also refreshes `+0x1528` (last-lamp record) from `entity + 1000` via
   `FUN_01F26B40` (same row lookup `0x13CDF30` uses).
1. Interaction sets the lamp flag (e.g. `72410100`); **common event 7200** (`common.emevd`, EVD record 31,
   instruction 0 = `1003[6] End if Client`, args at EVD `0xF55C`) waits for it and runs
   `2003[49] Warp Player to Respawn Point` → EMEVD dispatcher `0x17C0420` at `0x17C1CF0`:
   ```
   17c1cee: mov edi, dword ptr [rcx]   ; respawn/WarpParam id
   17c1cf0: call 0x13cdf30
   ```
2. `0x13CDF30(u32 id)` — the real "warp to lamp" primitive [static; sole direct caller is the EMEVD
   dispatcher, menu travel reaches it too per runtime-shadp2p]:
   ```c
   FUN_01f26b40(&row, id);                         // param row lookup
   if (row) {
     mode = row[6].lo == 0 ? 2 : 1;                // 2 = Hunter's Dream, 1 = world
     *(u32*)(GSM+0xc)    = row[0] << 24 | (row[1] & 0xff) << 16;   // area, block
     *(u32*)(GSM+0x1538) = mode;
     FUN_01f26b40(&row2, id);
     *(u64*)(GSM+0x153c) = row2 ? (row2->+4 | (u64)row2->+8 << 32) : (id | 0xffffffff00000000);
     FUN_013cde30();
   }
   ```
   Observed ids [runtime-shadp2p]: Great Bridge `2412952` (m24_01, mode 1, aux 101163), Central Yharnam
   `2412951`, Hypogean Gaol `2802952`, Dream from Great Bridge `2102950`, Dream from Gaol `2102952`
   (m21_00, mode 2, aux 101200). Timing: request ack `0x194100A` +533 ms, descriptor `0x1944D76`
   +650 ms, map resource +1033 ms, SOS area settles +4.3 s (Dream) / +6.6 s (Gaol).
3. Load: descriptor builder copies `+0x1538` → desc `+0x98`, nonzero → request type/subtype 9/2,
   `+0x153C/+0x1540` → desc `+0x80/+0x84` (`0x1944B33`, `0x1944D76`); `0x1928680` → stage manager →
   `MoveMapStep` ctor `0x1937570` (`+0x58` map, `+0xA4/+0xA8` type, `+0xD0` respawn id).
4. **Session end** (host): `0x13CDE30` skips the leave (`+0x1592 == 1`); the room is closed by
   `0x19471B1` and/or `0x193C657` → `0x1ED07A0` → `0xC8ED70` reason **`0xFF000023`**. Guests then get
   member-left/room-gone → Lua `OnRoomDisappeared 0x138A590` / `OnIrregularLeaveSession_1 0x13840E0` →
   state `OnLeave_Limit 0x138A4A0`:
   ```c
   if (phantom count > 0) {                      // PlayerIns->+0x3b0->+0x20->+0xf8
     *DAT_0553b0d8 = 2;
     *(u8*)(GSM+0x1520) = 1;                     // forced placement = my pre-summon spot (+0x14F0..)
     FUN_01332bc0(ctx, -1);                      // selector: +0x0C = +0x14F0
     FUN_013cde30();                             // guest: +0x1592==0 → BeginLeave(0xFF000023)
   }
   ```

### 1.2 (b) Host goes to the Hunter's Dream

Same as (a): world lamp → event 7200 → `0x13CDF30(21xxxxx)` (mode 2). Dream headstone travel back out
uses the warp menu (`FrpgMenuDlgWarp`, Lua `GetWarpMenuResult`/`OnWarpMenuClose`) and ends in the same
`0x13CDF30(id)` (mode 1) [runtime-shadp2p: "every observed menu travel entered through the WarpParam
handler"]. Session end identical to 1.1.4. Additionally in vanilla the Dream itself refuses multiplayer
(section 2), so nobody can rejoin there.

### 1.3 (c) Hunter's Mark

Lua handlers (`name → fn`, recovered from the registration code `0x138E000..0x1392000`):
`OnReviveMagic 0x1389D20`, `OnReviveMagic_1 0x1389E80`, `OnLeaveMagic 0x13881E0`,
`OnLeaveMenu_Yes 0x1389FB0`, `OnSpEffectRevive 0x1389F30`. Hunter's Mark is the BB Darksign; it maps to
`OnReviveMagic` [inferred, probe P3; Silencing Blank / Small Resonant "leave" map to `OnLeaveMagic`].

`OnReviveMagic 0x1389D20`:
```c
if (PlayerIns) { if (type == 2) return;           // invader can't
                 if (type == 1) FUN_0132e960(ev, 0xfd7, 1); }   // white phantom: notify
FUN_0132ed40(ev, 0xfdf, myId);
st = *(GSM + 0x16f8);
if (BeginLeaveSession(SprjSessionManager)) ...;    // 0x1389DDD  ← session ends here, both roles
FUN_015bed10(st);
FUN_0130fd40(0,0,-1,0,-1,1);
FUN_0132b1c0(..., 0xfce, "OnReviveMagic_1", ...);   // next Lua state
```
**Correction (live 2026-10-11, section 7):** the Hunter's Mark *item* (EquipParamGoods 100 -> SpEffect
2100, stateInfo 16) does not use these Lua states. The special-effect update `0x18FEF50` calls
`0x1317550(LuaEventMan, 1)` at `0x18FFA1D`; case 1 selects the last lamp (`0x1332BC0`, `+0x1520 == 0`)
and tail-jumps to the funnel at `0x13175EF`, so the funnel's return address is `0x18FFA22` (site
`0x18FFA1D`). Host and guest alike (a guest leaves at `0x13CDEF6`). `OnReviveMagic` is raised for a
stateInfo 23 effect (SpEffect 1904) and is the phantom "return home" path (Lua: `SetSosSignWarp`,
`SetDefaultMapUid`, `RevivePlayerNext`, `WarpNextStageKick`): raised on a host it warps to the forced
map `+0x14F0` (never set for a host) and the game ends on the title screen.

`OnReviveMagic_1 0x1389E80`: `*(GSM+0x1520)=1; FUN_01332bc0(ctx,-1); *(GameDataMan+0x8c)=0; FUN_013cde30();`
(forced placement path → own lamp). So **host or guest**: the leave is immediate at `0x1389DDD`,
reason `0xFF000023`, before any load. The guest goes to *its own* last lamp in its own world.

### 1.4 (d) Area transition / fog into another map

* Most of Yharnam is streamed (`m24_00/01/02` blocks) — no load and no `0x13CDE30`. The only session
  policy on map identity is the host stage-UID check `0x193C657` (1.0): it fires when the *active stage
  record* changes; whether streaming between `m24_0x` blocks changes that record is probe P2.
* Scripted loading transitions (elevators/doors/kidnapper to Gaol, Nightmare entries, Lecture Building,
  etc.) are EMEVD `2003[14] Warp Player` (`0x17C0420`, `0x17C0B80..0x17C0BED`):
  ```
  movzx r15d, byte [rcx]   ; area      movzx r14d, byte [rcx+1] ; block
  mov r12d, [rcx+4]        ; warp point (10000 → current target)
  mov [GSM+0xc], area<<24|block<<16 ; mov [GSM+0x10], r12d ; call 0x13cde30   (0x17C0BED)
  ```
  Session end: host as in 1.1.4 (load-time stop), guest immediately in `0x13CDE30`. Boss fog walls are
  not stage changes; guest-side gating there is `Lua_MultiWall 0x138B720` / `Lua_InvalidMultiWall 0x138B890`.
* Lua `WarpNextStage 0x132E010(ctx, area, block, region, index, warp_point)`,
  `WarpNextStage_Bonfire 0x132E050(ctx, id)` (area = id/100000, block = (id/10000)%10, `+0x10 = id`),
  `WarpNextStageKick 0x1332B80()`, `Lua_Warp_1 0x138C570` (plays anim `0x205C`, then same as
  `_Bonfire` with event `+0x20`) are script-driven variants of the same funnel.

### 1.5 (e) Host dies

Death dispatcher Lua `OnEvent_4000 0x13813D0` (all players): reads role `SprjSessionManager+0x124`,
`n = presentation state(+0x16F8)+0x18`, chr type `t`:
```c
own = (t == 0 || t == 8);
if (own && n > 1 && role == 3)      → state "SoloPlayDeath"   (host in co-op)
if (t == 1 || t == 2 || t == 0xc)   → state "PartyGhostDeath" (phantom died)
if (own && n < 2)                   → state "SoloPlayDeath"   (solo)
```
* Host: `SoloPlayDeath_2 0x1381DE0` clears snapshots `+0x1690/+0x16B0`, `FUN_01332BC0()` (no forced
  byte → `+0x1528 != -1` ⇒ `FUN_013CA100` = last lamp map, `+0x10=-1`, `+0x1524=1`), `0x13CDE30` (no leave,
  `+0x1592==1`) → room closes at load time (1.0), reason `0xFF000023`.
* Guests receive the host-death event; Lua `HostDead 0x1381870` (returns at once if role==3; invader gets
  the PK-success text) → `HostDead_1 0x1381A60`:
  ```c
  *DAT_0553b0d8 = 2; *(u8*)(GSM+0x1520) = 1;      // forced = my pre-summon spot
  FUN_01332bc0(ctx, -1); FUN_013cde30();         // guest: BeginLeave here (0x13CDEF6), 0xFF000023
  ```
  (`OnDeadEvent_HostDead 0x138CA40` is only the message/effect.)
* A guest's own death: `PartyGhostDeath_2 0x13824C0` → forced home + `0x13CDE30` (leave).
* Also in the same family: `BlockClear2_1 0x1385470`/`BlockClear2_3 0x1385930` (boss killed → guests home),
  `Failed_BossAreaMission_LeaveMap 0x138B4E0`, `PlayerKill_4030_1 0x1383A00`, direct
  `BeginLeaveSession` in `OnMatchingCheck 0x13808B0` (`0x13809B7`, remote chr not found in WorldChrMan),
  `OnMatchingError 0x13809F0`, `OnLeaveMenu_Yes 0x1389FB0` (`0x138A11E`).
  The pair (eventcommon.lua, event 4064 sub 3 / 4, type 0xC; fired through 0x132EAD0, which also sends
  it to every peer as packet 0x0E {0xC, 4064, sub, param, sender's player number}): a client's summon
  sends OnMatchingCheck; a non-client that gets it runs `IsMatchingMultiPlay(GetHostPlayerNo,
  GetPlayID)` (0x1338CE0: a chr with `+0x3B8` == the host's number and one with the sender's) and, when
  that fails, fires OnMatchingError with the sender's number as param 3; the player whose own number
  (`GameDataMan+8 -> +0x10`) equals it leaves (`BeginLeaveSession` at `0x1380A65`). Live sf3 / integ s2:
  a guest renumbered by PlayerNumberTick (2 -> 3) right after its load was checked by the host under 3
  while the host's copy of its chr still had 2 (rebuilt 1.5 s later): left 0.2 s after arrival. Fixed at
  the packet 7 read (0x17831AE, the host hands the joining guest its number): the guest takes its slot's
  number there, before its chr exists (`player number: packet 7 from the host gives 2; this guest (party
  slot 2) takes 3`); K5b stays as a safety net and logs a WARNING if it ever fires.

### 1.6 Call sites of `0x13CDE30` → travel kind (for the hook in 3.1)

| Return addr − 5 (call) | Caller | Kind |
|---|---|---|
| `0x13CE002` | `0x13CDF30` (EMEVD `2003[49]`) | lamp / Dream / headstone travel (respawn id in `+0x153C`) |
| `0x17C0BED` | EMEVD `2003[14]` | scripted area transition (`+0x0C/+0x10`) |
| `0x132E03E` / `0x132E0B4` / `0x1332B84` | WarpNextStage / _Bonfire / Kick | Lua stage warps |
| `0x138C5F0` | `Lua_Warp_1` | Lua bonfire warp |
| `0x1381EDE` | `SoloPlayDeath_2` | own death (host in co-op) |
| `0x1389F20` | `OnReviveMagic_1` | Hunter's Mark |
| `0x1381A93` | `HostDead_1` | guest sent home: host died |
| `0x138A51C` | `OnLeave_Limit` | guest sent home: room gone/kicked/left |
| `0x1382663` | `PartyGhostDeath_2` | guest died |
| `0x13856F4` / `0x13859DF` | `BlockClear2_1/_3` | boss cleared |
| `0x138B513` / `0x1383A5C` | `Failed_BossAreaMission_LeaveMap` / `PlayerKill_4030_1` | mission/PK |
| `0x132E11E`, `0x132E166` | `0x132E0C0` | map `0x01000000` (title/tutorial style) |
| `0x13175EF` (tail jump; the funnel sees the caller `0x18FFA1D`), `0x1317632` | `0x1317550` | **the Hunter's Mark item** (case 1: last lamp; 2-6: fixed maps for stateInfo 0xA1-0xA5), see 1.3 |

## 2. Why co-op is blocked in the Hunter's Dream, and the patch

**Not the action bit.** `UseItem` = action bit `0x80` (`SprjChrActionFlagModule` updater `0x1A0F910`;
first consumer `0x18F7516`) fires in the Dream as elsewhere [runtime-shadp2p]. The bell is rejected
earlier, by item availability `0x157F200` (the inventory greys it out), so nothing at `0x18F7516` needs
patching.

**The real gate is the area restriction table.** All 14 callers of `FUN_0131D7B0` (bell availability
`0x157F90E`, active-bell updater `0x15068F7`, SOS status `0x18700CE`, summon builder `0x18749E1`, responder
search `0x191A968/0x191AA4C`, …) use it:
```c
a = |sosArea|;  i = (a/10000)%100 - 21;  blk = (a/1000)%10;  rgn = (a/100)%10;
if (i < 16 && blk < 3 && rgn < 4) {
  f1 = TBL1[i][blk][rgn];  /* 0x47304B0 */   f2 = TBL2[i][blk][rgn]; /* 0x47307B0 */
  blocked = (f1 > 0 && GetEventFlag(f1)) || (f2 > 0 && GetEventFlag(f2));
}
```
Dumped `TBL1` rows (12 ints each = 3 blocks × 4 regions): `m21: 2100,-1…`, `m22: 2200`, `m23: 2300,2301`,
`m24: 2400,2401,2402,-1, 2410,2411,2412,-1, 2420,2421`, `m25: 2500`, `m26: 2600,2601`, `m27: 2700,2701`,
`m28: 2800`, `m29: 12901800..803`, `m30/m31: none`, `m32: 3200`, `m33: 3300`, `m34: 3400..3403`,
`m35: 3510..3513`, `m36: 3600,3601`; `TBL2` all −1 (m36 zeros). These are the "area boss defeated" flags;
the Dream's SOS area `210000` maps to flag **2100**, which the Dream keeps ON (the same mechanism as
`2800` in m28 [runtime-shadp2p]). Hence Beckoning greyed, no SOS request, Small Resonant refused.

Patches to allow a party in the Dream (all exact, byte-verifiable):

1. **Data patch** `dword [0x47304B0] = 0xFFFFFFFF` (was 2100). Removes the Dream from the table for all
   14 consumers at once. (Equivalent finer option: shadp2p's bell set `0x157F8C8/0x157F960/0x157F6D1/
   0x157F6D3/0x15068BB/0x15068FC/0x191A8C3/0x18700D3`, which A6 may already apply for boss-cleared areas.)
2. **Guest bell in the Dream**: the Small Resonant branch also fails after the multiplayer effect mask
   changes (`0x202205 → 0x202207`) in the Dream; shadp2p's `0x157F6D1` (`cl=1`) + `0x157F6D3` (jump to
   epilogue) — goods 205/225 only.
3. **Same-map is enough**: when everyone is in the Dream the area-group checks `0x14B714A` and
   `0x1874B79` pass natively; they are only needed for cross-map joins. `0x18749E8/0x18749F0` (boss-cleared
   / negative event id in the builder) are already in A6.
4. Expected leftovers to test: m21 EMEVD client guards (`1003[6]`) make the Doll/headstones/workshop
   host-only for guests (desired: travel is host-led); host headstone menu with guests present is not yet
   proven open (probe P4); Insight cost of the Beckoning Bell (A5 policy).

## 3. B1 "follow and rejoin" — design and hook points

### 3.1 Host: warp detection
* Hook **entry of `0x13CDE30`** (19 callers, one place). In the hook read the caller (return address
  − 5, table 1.6) and the already-written state: `+0x0C` packed map, `+0x10` warp point, `+0x1520`,
  `+0x14F0/+0x1500/+0x1510`, `+0x1528` (qword), `+0x1538`, `+0x153C` (qword). Also hook entry of
  **`0x13CDF30`** to capture the *key* `edi` (the id to replay; `+0x153C` stores the row's `+4` field,
  which equals the key in all observed rows — probe P5).
* Act only if role (`SprjSessionManager+0x124`) is 3 and party mode is on; send
  `PARTY_CMD TRAVEL {seq, kind, key/respawn id, mode, packed map, warp point, last-lamp record,
  forced transform}` on PartyLink **before** calling the original (the host's room survives until its
  load, ≥ 0.5 s, so guests get the message while still connected; PartyLink is independent anyway).
* Host death needs no special hook: `SoloPlayDeath_2` reaches the same entry with kind = own death,
  destination = `+0x1528` (send the record; `+0x10 = -1`).
* Kinds to broadcast: lamp/headstone (`0x13CE002`), scripted transition (`0x17C0BED`), Lua warps,
  host death (`0x1381EDE`), host Hunter's Mark (`0x1389F20`; note the host already left at `0x1389DDD`,
  so hook `0x1389D20` entry too to send TRAVEL first).

### 3.2 Guest: warp to the same place, on the main thread
Run from the A5 tick (`SprjFlipper::Update 0x2034770`). Preconditions: `WorldChrMan+0x60 != 0`,
NowLoading `0x556286B == 0`, `GSM+0x08 == 0` (no pending request), role not 4/7, no menu open.

* **Lamp / Dream / headstone / host death** — call the native respawn warp:
  `void 0x13CDF30(u32 id)` (SysV: `edi = id`; no `this`). Same function event 7200 uses; resolves map
  and mode itself (Dream = mode 2). For host death use the low dword of the host's `+0x1528`.
* **Scripted transition or Lua stage warp** — `0x132E010(ctx=0, int area, u8 block, u8 region, u8 index,
  int warp_point)` (`rsi, edx, ecx, r8b, r9d`; returns 1) or `0x132E050(0, int bonfire_id)`.
* **Exact transform** (fallback, e.g. no lamp id): native forced-placement setters, all taking a pointer:
  `0x156CF10(&u32 map)` → `+0x14F0`, `0x156CF20(&vec4 pos)` → `+0x1500`, `0x156CF40(&vec4 rot)` →
  `+0x1510`, `0x156CF60()` → `+0x1520 = 1`; then selector `0x1332BC0(0, -1)` (copies `+0x14F0` to
  `+0x0C`, ignores arg 1) and `0x13CDE30()` — exactly the `HostDead_1` sequence; shadp2p validated it
  lands at an exact host transform when no room exists.
* The guest is still a summoned phantom when it calls these: `+0x1592 == 0` → `0x13CDE30` leaves the
  room at once (that is wanted), and because `ReloadPhase+0x84 != 3` by then, `MoveMapStep` takes the
  normal path: **the guest loads into its own world with its own flags** at the host's lamp.
* **Suppress the stock "send home" warps** while a follow is pending: in the same `0x13CDE30` hook on the
  guest, if the caller is `0x138A51C` (OnLeave_Limit), `0x1381A93` (HostDead_1), `0x13856F4/0x13859DF`
  (boss clear, if A6 policy is "stay") and a TRAVEL is pending, rewrite instead of going home: clear
  `+0x1520`, then return through `0x13CDF30(id)` (re-entrancy guard on the hook). This also covers the
  race where room teardown reaches the guest before the tick runs. `OnRoomDisappeared 0x138A590` shows a
  dialog (FrpgNetMan `+0xA05` bit 0 clear) — suppress with A6's member-left suppression.
* Guest-initiated Hunter's Mark (`0x1389D20`, type 1): policy hook — either let it go home (leave party
  temporarily) or redirect to the host's last lamp (`+0x1528` from the host's PROGRESS) via the same
  rewrite.
* **Peer validation** (`SanitizePeerTravel`, bbport security pass): every id of a host's intent is looked
  up in tables generated from the game data (`gpu/shim/party/party_ids.inc`, `tools/party/ids_tool.py`):
  the 0x13CDF30 ids (`lamp`, the low dwords of `respawn` / `last_lamp`, the phantom event's lamp) must be
  **ReturnPointParam** rows (the param the lamp warp looks up: +0 area, +1 block, +4 returnPointEntityId,
  +8 returnAnimId, `isRegistDeadReturn` 0 = Hunter's Dream; rows 1 and 9902950 have area 0 and are left
  out); `map` and the transform's map must be existing maps (map folders and MSBs); a stage-warp point
  must be an entity of the destination map (int32 ids AABnnnn found in that map's MSBs, plus the EMEVD
  2003[14] / 2002 warp points), a 0x132E050 bonfire id an entity of the map it names. An unknown id
  becomes "none" (a stage warp then uses the map's default entry; no lamp id leaves the lamp warp out).

### 3.3 Auto-rejoin and timing
1. T0 host hook sends TRAVEL; host request ack ≈ T0+0.53 s; host room closes during its load
   (`0x19471B1`/`0x193C657`, reason `0xFF000023`); host SOS area settles ≈ T0+4–7 s.
2. Guests start their own load ≈ T0+RTT+1 frame (in parallel with the host).
3. "Arrived" = NowLoading clear (`0x176D3C0`), `PlayerIns_GetBloodMarkMap 0x19046C0` == TRAVEL map,
   SOS area stable, role 0 and no controller, for 20 consecutive ticks (shadp2p's guard).
4. Host rings Beckoning (`OnEvent_Call_SOS` via `0x1339870`), guests ring Small Resonant
   (`OnEvent_SendSoulSign_NormalCoop`); PartyHostService matches immediately (same map → no area
   patches needed except boss-cleared/Dream ones). Guest then does `SummonedMapReload` (second, short load
   into the host world).
5. Expected: guests visible again ≈ host load + ~1 s + guest join reload (≈ 8–15 s after the lamp press),
   two loading screens for guests, one for the host. Retry the bell once after 15 s without effect 9005
   (shadp2p) / summon result.
* Edge rules: if a guest's world lacks the destination (DLC not owned, map locked) fall back to the
  host's transform via the forced setters; if a guest is mid-load when a second TRAVEL arrives, keep only
  the newest `seq`.

## 4. B2 true seamless warp — assessment

What would have to hold, with the blocking sites:

| Requirement | Site(s) | Difficulty |
|---|---|---|
| Host can open the lamp/headstone with guests | `+0x49` at `0x12F8323` (patch to 0); second gate unresolved: candidates `+0x4B` (`0x191B6E0` — another character targeting the lamp entity, i.e. a phantom near the lamp), action-button row 90000 vs 90000+type (`0x12F83E0`), the enable consumer of `FrpgNetMan+0xC70` entries (`0x146E250` writers `0x12F5A50/0x12F5B40/0x13274C0/0x1327650`), Lua `OnEvent_Bonfire` scripts | medium (probe P4) |
| Host keeps the room through its load | retarget `0x19471B1` → `0x1ED00A0` (shadp2p), skip `0x193C657` (role-3 stage-UID stop), skip the `0x15BED10` calls after them | low |
| Guests keep the room through *their* load | `0x13CDE30` leave (`+0x1592==0`, `jne` at `0x13CDEAE` → force jump while traveling), `OnMatchingCheck 0x13809B7` (remote chr missing during rebuild), OnLeave_Limit/HostDead_1 send-home | low–medium |
| Guests load the new map **in the host world** | `MoveMapStep` ctor host-world path needs `ReloadPhase+0x84 == 3`; only the `SummonedMapReload` serializer `0x131E5C0` / setter `0x178D9A0` sets it, and it snapshots the *source* map into `+0x1690/+0x16B0/+0x16D0` (must be emptied, as `SoloPlayDeath_2` does); ordering trap: phase 3 must be set after the target descriptor exists (`0x1944F23`) [runtime-shadp2p] | high |
| Host's flags for the new map on guests | vanilla sends the flag snapshot only at join (serialize `0x13BE3C0` / apply `0x13BECA0`); needs a re-send before guests' `MoveMapStep` (C2 territory) | high |
| Remote PlayerIns re-created after load | WorldChrMan is rebuilt; remote chrs are created only by the join insertion tasks `0x1E4FCF0..0x1E50D70` (post-copy `0x1E4FF3A`), and the payload dtor calls `FUN_01E54830(CSMultiPlayMan)` which resets them; there is no native "re-insert existing member" entry — must drive insertion manually for host and every guest | very high |
| Net enemies/objects across different load speeds | net chr sync tables are per loaded map; packets referencing unloaded entities → stale pointers (DS2 crash class); needs per-peer "map ready" gating of packets | high |
| Guest event 7200 / map events | `1003[6]` client guards end events on clients; fine (guest follows our message, not the event) | low |

`SetSessionTargetMap 0x156CE80` (`+0x14C4 = *rdi`) and its route siblings `0x156CE90/0x156CEB0`
(`+0x14D0/+0x14E0`) only set the summoned-placement bank used by the join/reload path; they do not by
themselves move anyone or keep a session, so they are useful only inside the high-difficulty "host-world
load" row.

**Verdict:** B2 is research-grade (several weeks, highest crash risk, depends on C2 flag sync and on a
custom re-insertion driver). Ship **B1** (one hook on `0x13CDE30` + one on `0x13CDF30`, guest replay via
`0x13CDF30`/forced setters, send-home rewrite, auto-rejoin), plus the Dream table patch so the Dream works
as a lobby. Revisit B2 only after B1 and C2 are stable, starting with the cheap rows (host room retention,
guest leave suppression) behind a flag, and measure whether `0x1E50xxx` insertion can be re-run.

## 5. Runtime probes (to close the [inferred] items)

* **P1** log return address of every `0x1ED07A0` call and `0x193C657`/`0x19471B1` order during a host lamp
  warp with a guest present (after the `+0x49` patch) and during host death.
* **P2** walk Central Yharnam ↔ Cathedral Ward ↔ Old Yharnam as host with a guest: does `0x193C537` see a
  UID change (would end vanilla co-op)?
* **P3** use Hunter's Mark as host and as guest: confirm `0x1389D20`/`0x1389E80` fire (vs `0x1389F30`).
* **P4** at `0x12F8E8A` log `+0x48..+0x4B`, `r15` at `0x12F83FD`, and the `FrpgNetMan+0xC70` entry for the
  lamp, with and without the `+0x49` patch; read `GetEventFlagValue 0x13CFD80(2100)` in the Dream.
* **P5** at `0x13CDF30` entry log `edi` and the stored `+0x153C` qword for lamp, headstone and death
  (key vs row `+4`).

## 6. Live verification (2026-10-10, two loopback instances)

Harness: `tools/mp/instances.py run --count 2 --party`, the test saves start in the Hunter's Dream (host
level 37). The travel is triggered in-process by the director's test steps (no OS input):
`BB_PARTY_DIRECTOR_TEST=host_warp=<ReturnPointParam id>@S,host_die@S,guest_die@S[#N]` run in order, each
S seconds after that player's (re)join (host: cooperators 0 -> >= 1; guest: client(6) with the world
loaded), `#N` holding a step until the Nth join. `host_warp` calls `0x13CDF30(id)` on the host's main
thread (the lamp / warp-menu path, so the funnel hook sees caller `0x13CE002`); `host_die` / `guest_die`
call the Lua binding **ForceDead `0x13320C0(ctx, 10000)`** (entity 10000 = local player via `0x13C97A0`;
clears `ChrIns+0x1E3` bit 5, then `0x18BF9A0`: HP `+0xF8` of `[[chr+0x3B0]+0x20]` = 0 and the dead
flag), which runs the normal death (animation, `OnEvent_4000`, `SoloPlayDeath_2` / `PartyGhostDeath_2`).
With any test step set, the Party / Party travel / Party phantom lines carry wall-clock stamps
(`BB_PARTY_LOG_TIME=1` does it alone), so the two logs line up.

**Bug found and fixed: the host kept an empty room after its own travel.** The "Party: Keep session on
map reload" patch (`0x19471B1` -> `0x1ED00A0`, meant for the guest's summoned map reload) also keeps the
*host's* room through the host's lamp load, so section 1.1.4's "room dies at the load" no longer holds.
First live run: host warped 19:43:44, guest left at once (replay +1 s), host arrived in m24_01 with role
host(3) and 0 cooperators and stayed so for 56 s until the game's own timeout left (19:45:10); the
director rings only when idle, and the summon into the next room then stalled (guest invited, the host's
game never sent the world data; the same signature the host watchdog of `party_watchdog.h` was later
written for). Fix (`party_travel.cpp` `HostCloseEmptyRoom`, from `TravelTick` on the host): after a
broadcast travel, once the host has loaded and is in its new world in role host/creating with 0
cooperators, it leaves through `0x13CDDA0` (BeginLeaveSession + `0x15BED10`, the funnel's own idiom),
i.e. what vanilla does at the load; with cooperators > 0 the room is kept. The director also rings while
hosting now (upstream `SessionMayRing`), which covers a guest that died and comes back.

Timelines after the fix (host log H, guest log G; t = 0 at the step):

| Scenario | Step | Host captures / sends | Guest replays | Guest home loaded | Host room | Guest client(6) | Back (host cooperators 1) |
|---|---|---|---|---|---|---|---|
| Lamp warp Dream -> Central Yharnam `2412951` (run B) | 20:22:56.21 | +0.00 `host travel ... lamp (site +0x13ce002)` mode 1, m24_01 | +0.04 `replay ... via lamp warp 0x13CDF30` (role 6 -> 7 at once) | +8.2 m24_01 | +8.0 arrived, empty room left; host(3) again +23.8 | +27.8 | **+38.7 s** |
| Host death (ForceDead) in Central Yharnam (run B) | 20:23:49.90 | +9.1 `host_death (site +0x1381ede)`, last lamp 2412951 (death animation first) | +9.15 lamp warp 2412951 | +14.8 | host left by itself +20.6 | +32.8 | **+43.5 s** (34.4 s after the capture) |
| Lamp warp Central Yharnam -> same lamp (run C1) | 21:20:44.71 | +0.00 | +0.07 | +5.7 | +5.5 arrived, empty room left; host(3) again +21.2 | +25.2 | **+35.8 s** |
| Guest death (ForceDead) in the host's world (run C1) | G 21:21:24.93 | (none: guest-side) | +8.6 `guest_died at +0x1382663: to the host's last lamp 2412951` (`party_phantom` redirect, then refill) | +14.2 | host 1 -> 0 cooperators +14.0, game left +18.7, host(3) again +36.4 | +40.4 | **+51.3 s** |
| Lamp warp Central Yharnam -> Hunter's Dream `2102950` (run C3, fast-join build) | 22:33:23.58 | +0.00 mode 2, m21_00 | +0.04 | +8.7 m21_00 | +11.1 arrived, empty room left; host(3) again +18.6 | +24.1 | **+30.0 s** |

What the times consist of: the load itself is 5-8 s on both sides (in parallel); the rest is the bell
cycle - the host's sign search polls `summon_messenger/get` and the guest's sign goes up on its next
bell (director period 10 s in these runs, default 30 s), ~15 s from host idle to room, then 4 s join and
~10 s summon reload into the host world. A death adds the 8-9 s death animation before the funnel.
Before the fix the same lamp warp took > 4 min and did not complete. Run C3 (with the fast-join
increment 1 merged) also shows the Dream gate patches working for a rejoin in the Dream after a travel.

Not covered live: headstone travel from the Dream (warp menu), scripted transitions (`2003[14]`), Lua
stage warps, Hunter's Mark, boss clear, a 3rd/4th member; the send-home retarget (`OnLeave_Limit` /
`HostDead_1`) did not fire in these runs because the TRAVEL message always arrived (< 0.1 s) before the
room teardown, so the guest left by its own replay.

## 7. Live verification of the remaining travel cases (2026-10-11, B2 default, edgetravel)

Two loopback instances (`instances.py run --count 2 --party`, base save `savedata_20261010` in each
`savedata_party`, B2 on by default). Director test steps (BB_PARTY_DIRECTOR_TEST, no OS input):
`host_headstone=TALK:DOWNS` (stand 1.2 m in front of the talker, Cross, wait for the menu's text
lookups, Down N, Cross, Cross on the confirmations until the stage request is armed),
`host_stage=A:B:R:I:POINT` (Lua WarpNextStage `0x132E010`) or `host_stage=bonfire:ID` (`0x132E050`),
`host_mark` / `guest_mark` (the Hunter's Mark item: SpEffect 2100 on the local player; `=lua` raises
the Lua event instead), `host_walk=X:Y:Z:YAW/...` (chr warp vfunc +0x5F8 hops 1 s apart, no load);
upstream's `host_move` / `guest_move` (one hop, steady-world clock, `%MAP` filter). t = 0 at the step.

| Case | Run | Evidence | Result |
|---|---|---|---|
| 1 Headstone t210000, row 2 (host, Dream -> 1st Floor Sickroom) | 06:04 | menu +3.3 s, travel +10.1 s (three confirm presses) `host travel ... lamp 2412950 (site +0x13ce002)`; guest `host travels` -> B2 load -> `re-join done` +28.3 s; host `(re)joined #2` +28.8 s | **pass** after the fix below (headstones default on) |
| 2 Scripted transition: WarpNextStage(m21_00, point 2100980) = the EMEVD `2003[14]` args of the chalice exit (m29 events 12900700/12901710) | 05:31 | `host travel ... lua_stage_warp (site +0x132e03e) ... warp point 2100980`; guest B2 `re-join done` +10.4 s, host +10.4 s, both in the Dream | **pass** |
| 3 Host Hunter's Mark (Great Bridge) | 06:58 | `host travel ... hunters_mark (site +0x18ffa1d) ... last lamp 2412952`; guest B2 load, `re-join done` +12.1 s; host `(re)joined #3` +12.2 s | **pass** after the fix below |
| 4 Guest Hunter's Mark | 06:59 | `hunters_mark at +0x18ffa1d: to the host's last lamp 2412952` (session left at `0x13CDEF6`, as vanilla), client again +10.5 s; that join was dropped 3 s later (leave from `+0x178a6be` during its summon reload), the bells brought it back +29.9 s | **pass** (one dropped re-join, open item) |
| 5 Walk between streamed blocks, Cathedral Ward m24_00 <-> Great Bridge m24_01, guest moving along | 05:30 | `K3: stage-record change with a session (stage 0x18010000, 1 member chr(s), farthest 2.4 m): no leave`, and back (1.9 m); host role 3 / guest role 6 throughout, no load | **pass** |
| 6 Five lamp warps in a row (GB 2412952, CW 2402950, Central 2412951, Grand Cathedral 2402951, Dream 2102950) | 03:01 | guest `re-join done` +24.2 / +52.3 / +25.0 / +28.9 / +10.9 s (the 2nd: first Cathedral Ward load, host 24 s + guest 28 s); no fallback, no session loss | **pass** after the fixes below |

Bugs found and fixed:

* **Cooperators 0 after every B2 travel** (all cases): the host-world MoveMapStep writes the guest's
  PlayerGameData `+0x5D8` (session type = presentation slot, 7) from its descriptor `+0x88`, filled
  only on the summoned-placement path (GSM `+0x16F4`); our lamp / transform warp left 0. The chr
  vfunc `+0x558` the cooperator count `0x15BDC20` tests then read 0 on both sides, so the host's
  watchdog ended the session 60 s after the travel ("stuck summon"). Fixed here by a load-time restore,
  then superseded by upstream's MemberSessionTypeTick (also the guest's own player) in the merge.
* **Repeated travel**: HostTick stayed in its "re-join" wait 60 s after an arrival; a second travel in
  that window was not seen as a load (no re-send, guest fell back to B1 after 45 s). The wait now
  ends when every member task has its chr again, and a new own load restarts it.
* **Headstones with guests present**: the headstone scripts park while `IsMultiplayerInProgress`
  (the host too); the filter was only installed with `BB_PARTY_GUEST_LAMPS=1`. It is now on in party
  mode (`BB_PARTY_HEADSTONES=0` off); guests' headstones stay behind `IsClientPlayer`.
* **Host Hunter's Mark not followed**: the item reaches the funnel from `0x18FFA1D` (1.3), which was
  unlisted, so the host's travel was never broadcast and the guest stayed in the old world without
  the host. Now `HuntersMark` (host: captured; guest: redirected to the host's last lamp).

Fixed after the first report (same day):

* **Host / guest crash when the other player stands in a block this game streams out** (a guest
  walking away or left behind, the host walking on; also the guest's game with the host far
  away). A chr keeps the map hit it stands on in `+0x288` (setter `0x18BBA70`; `+0x350` a second
  one). Map chrs die with their block and the local player's block never unloads, but a *remote*
  player chr kept a hit of a block that left the hit registry (`*0x553E850`: `+0x20` blocks,
  records of 0xB8 with `+0x78` count / `+0x80` hits of 0x138; the table `0x13C9FB0` resolves
  from). The hit record's `+0x18` (its block's collision resource) was freed, and the chr physics
  pre-step `0x18CB0E0` (rdi = chr) read `[[hit+0x18]+0x88]` -> fault at `0x18CB16A`/`0x18CB17D`.
  Hit watch (live): the guest's hit dropped out of the registry ~0.7 s after the host's block
  change, the crash followed 0.25 s later. Vanilla never has a remote player there (the host
  leaves at the block change, 1.0; the confinement walls of common event 7600 keep phantoms in).
  Fix (party_b2.cpp, both roles): an entry hook on `0x18CB0E0` clears `+0x288` / `+0x350` of a
  *remote player* chr whose hit is no longer in the registry (pointer range checks only; the stale
  hit is never read). The chr then has no ground hit until it stands on loaded collision again;
  its position keeps coming from its owner. Live: host teleported 146 m into the other block
  (crashed 4/4 before): `stale map hit cleared 1 time(s)`, no crash; then the guest moved 146 m
  away from the host into the other block: the guest's game cleared the host chr's hit, no crash;
  both stayed in session (host 3 / guest 6, cooperators 1) for 2+ minutes 146 m apart.
* **Dropped re-join after a guest's Hunter's Mark**: not a stale member task (the host deleted it).
  The session event handler `0x178B5E0` arms the network flow's "host gone" timer `+0xF4` = 10 s
  (`0x178D35D`) when a member leaves a guest in the host's world; the flow update `0x1789B60` counts
  it down and leaves (`0x178A6BE`). Left on its own and summoned again within 10 s, the guest
  carried it into the new session and was dropped 3 s after joining. A (re)join now starts with
  it off (-1, the flow ctor's value).

### 7.1 Stick walk-away of a guest (2026-10-13, b1)

One session, base save `savedata_20261010`, host `host_warp=2412952@8` (Great Bridge lamp, stays
at (-124.8, -27.0, 64.3)); guest `BB_PAD_FILE` + `guest_move` to the bridge's fog end, then
`guest_stick=-70:118:2:150` (walk along the bridge: blocked at the lamp after 33 m), `guest_obj` +
`guest_stick=obj:W:10:180` / `obj:W:25:180` for the confinement walls W = 2411997 (at (1.6, -29.6,
118.9)) and 2411999 (at (-187.2, -46.7, 102.2); 2411998 was not loaded). The guest walked through
both walls by the stick (`approach: arrived` 10 m behind each); the host's remote chr log reached
140.3 m. For 5 min apart: host role 3, guest role 6, cooperators 1 on both, no `send-home`, no
leave, no K3 (these walls do not change the stage record: no block boundary was crossed by the stick,
that stays open), `where` reports fresh (< 0.8 s). There is no leash (phantom_limits.md §4).
Screenshots black (desktop locked). The walk driver now stops after 8 steps without moving.

### 7.2 Route walk across a block boundary (2026-10-15, parab2 b1a-b1c)

The straight-line stick walk of 7.1 runs into walls. **Route walker** (party_director test steps
`host_route=` / `guest_route=X:Y:Z/X:Y:Z/...`, also `host_stick=`): the left stick (pad file, closed
loop on the position, camera-relative angle corrected after every step) steers to each waypoint in
turn (reached within 2 m, no stop between waypoints); against a wall it takes 3 side steps to one
side, then the other; still blocked after two such slides it hops (chr warp) onto the waypoint
when it is within 6 m, counted and logged (`route: HOP n`). The walker is invincible while it walks
(chr +0x1E1 bit 14, cleared at the end) so enemies on the way do not end it; fights and deaths are
the steps after it. Every stage-record (block) change is logged (`route: stage record A -> B`).

**Waypoints from the game's navmesh** (`tools/party/nav_route.py`): each map's
`mAA_BB_CC_DD.nvmhktbnd.dcx` holds Havok 2014 packfiles with one hkaiNavMesh each (faces, edges,
vertices in map coordinates); faces are linked inside a file by their opposite face and across files
and maps by boundary edges lying on each other; a route is A* over face centroids, a waypoint every
4-5 m and at every map change. `--borders` lists where two maps' navmeshes meet: m24_01 (Central
Yharnam) and m24_00 (Cathedral Ward) meet at the Great Bridge's gate (-117, -27, 77) and east of the
Oedon Chapel (31, -9, 95). The navmesh is the enemies' one: closed gates are in it.

| Run | What | Result |
|---|---|---|
| b1a | Great Bridge lamp -> Cathedral Ward (host), Great Bridge -> Central Yharnam lamp (guest), straight centroids, R 1.5, no hop | host: the Great Bridge's gate to Cathedral Ward is closed in the test save (screenshot): stopped 2 m before it; guest: pinned between a cart and an enemy after 9 waypoints. Led to the hop fallback, the 2 m reach and the Oedon Chapel route |
| b1b | Host at the Oedon Chapel lamp 2402950 walks east into m24_01 (10 waypoints, 43 m); guest walks the chapel -> the Cathedral Ward stairs (16 waypoints, 77 m) | host: `stage record 0x18000000 -> 0x18010000` at (32.5, -9.6, 94.7), `K3: stage-record change with a session ... no leave`, DONE 42 s, 0 side steps, 0 hops; guest: DONE 49 s, 0 side steps, 0 hops; the guest's fight with the nearest enemy (entity 2400203, R1 taps) ended in its death: `died alone: revived at its own last lamp 2402950 in the loaded map, no load`; the host's kill in m24_01 (Echoes 96 -> 267). Apart in two blocks from 62 s to the end (239 s, ~3 min, 58-91 m): host role 3 / guest role 6, cooperators 1 throughout, no send-home, no leave, no desync digest difference (digests every 15 s), no stall (screenshots: host in the dark m24_01 corridor east of the chapel, guest on the stairs and fighting, guest revived at the chapel lamp) |
| b1c | b1b with items and a host death | both routes DONE again with 0 side steps and 0 hops (host 45 s, `stage record 0x18000000 -> 0x18010000`, K3 no leave); host kill in m24_01 (Echoes 267 -> 438), host and guest each used Madman's Knowledge in their own block (Insight 6 -> 7 on each); the guest's fight; the host died in m24_01 while the guest was being killed in m24_00 (both down: `a wipe; the party respawns together (one load)`), both at the chapel lamp, together again; the guest died alone later: revived at its lamp, no load. No digest difference, no stall |

Not shown: a host death in the same map while the guest is alive in another block: by the C2
decision (the vanilla respawn load for the whole party) it takes the party along; a host death in
another map leaves the guest where it is (B5).

Open:

* Headstone menu: three Cross presses after the row (row, then the confirmation dialog); the menu
  text ids were not identified (the script waits for fresh text lookups).

## Audit: the ng_plus_coop session's "a few wrong teleports" (0.7.6)

Host log vs the two logs of one guest (consecutive sessions), every `host travel #seq ... lamp/map` against the
guest's `replay now #seq`: 26 travels (lamp warps and host deaths) replayed with the same lamp id, map and
respawn record, none missing inside the guest's log windows, none with another destination; the post-ending
travels (#982/#983, NG+) were held by the prologue rule (#19). Arrival placements ("placed beside the host",
join arrival 15-27 m -> put beside) are the Dream's spawn spot or the host's spot. The `guest death
redirect: host's last lamp 2102961 (was 2102950/2402950)` lines are the host's own respawn record after its
death (the Dream's second lamp, same map), not a mismatch. No wrong destination found: what players saw as
wrong teleports matches the guest-to-guest link outages (the other guest `no chr`, rebuilt by the next teleport),
tracked separately. One departure by design: a guest that accepts the ending leaves the host's world for its
own Hunter's Dream.
