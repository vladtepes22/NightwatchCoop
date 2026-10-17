# Seamless party rules (plan phase A6)

What the party layer changes in the game so a party session survives boss kills, map reloads and
bells rung anywhere. Two parts:

- **Byte patches** `Party: ...` in `patches/Bloodborne.xml` (`isEnabled="false"`), enabled by
  `scripts/patches.py` when `BB_PARTY` is set and `BB_PARTY_SEAMLESS` is not `0`. The loader
  writes them before the game runs. Each `<Line>` carries `Original="..."` (the 1.09 bytes);
  patches.py compares it with `out/eboot.elf` and leaves the whole patch out on a mismatch.
- **Runtime rules** in `gpu/shim/party/seamless_rules.{h,cpp}`: `SeamlessRulesInit()` (called
  from `PartyInit`, after `HooksInit`) and `SeamlessRulesTick()` (called from `CoopTick`, the
  main-thread tick). Both are no-ops unless `BB_PARTY` is set and `BB_PARTY_SEAMLESS` != 0.

Addresses are our offsets (raw ELF VA); XML `Address` = our offset + 0x400000. The bell, boss
and map-reload patches come from Wozzardman/shadp2p (GPL-2.0-or-later,
`documents/bloodborne-seamless-re.md`, `src/core/bloodborne_re.cpp`
`InstallSeamlessCoopPatches`, enabled there with `SHADPS4_BLOODBORNE_SEAMLESS_COOP`), where
they were validated in live 2-client sessions on 1.09. Every original byte string below was
compared with our `eboot.elf` (vaddr 0 at file offset 0x4000) and every patched instruction
disassembled.

## Byte patches

### Party: Bells anywhere

| Site | 1.09 | Patched | What |
|---|---|---|---|
| 0x157F8C8 | `0f 87 90 00 00 00` ja | `0f 8f 90 00 00 00` jg | Beckoning availability (0x157F200, goods 200 at 0x157F855): `area > 999999` unsigned -> signed, so negative SOS areas pass |
| 0x157F960 | `34 01` xor al,1 | `31 c0` xor eax,eax | Beckoning area-flag result (validator 0x131D7B0) -> allowed |
| 0x157F6D1 | `88 c3` mov bl,al | `b1 01` mov cl,1 | Small/Sinister (goods 205/225, branch 0x157F686) result -> allowed |
| 0x157F6D3 | `4c 89 f7 e8 f5 9f 34 00` | `e9 e9 09 00 00 90 90 90` | jmp 0x15800C1, the function's canary-checked epilogue (returns cl) |
| 0x15068BB | `77 4a` ja | `7f 4a` jg | active-bell updater 0x1506820: range check signed |
| 0x15068FC | `88 c3 80 f3 01` | `b3 01 90 90 90` | active-bell area flag -> normal searching state (effect 9003, not the X 9004) |
| 0x191A8C3 | `18 c9 20 c1 eb 02` sbb/and | `7d 04 88 c1 eb 02` jge fail; mov cl,al | responder search (0x191A750): signed limit, keeps the native table result |
| 0x18700D3 | `84 c0 41 bd ff ff ff ff 44 0f 44 eb 41 c1 ed 1f` | `45 31 ed` + 13 x `90` | SOS status producer 0x186FE40: area-derived restriction (validator 0x131D7B0 / negative area) -> 0 |

The area validator 0x131D7B0 itself is not patched: it takes |area| and looks up the area's
event flag (e.g. 280050 -> 2800); its callers' reductions are patched above and below.

### Party: Bells after boss defeated

| Site | 1.09 | Patched | What |
|---|---|---|---|
| 0x18749E8 | `0f 85 48 01 00 00` jne | 6 x `90` | summon builder (0x1874710): rejection when 0x131D7B0 reports the area's boss-cleared flag (2410 for Great Bridge) |
| 0x18749F0 | `0f 88 40 01 00 00` js | 6 x `90` | rejection of the table's negative event id (-1 sentinel of negative areas) |
| 0x14B714A | `0f 85 c0 03 00 00` jne | 6 x `90` | summon candidate: responder area != own area (cross-map candidates) |
| 0x1874B79 | `74 0d` je | `eb 0d` jmp | builder: area comparison -> always the allowed path |

### Party: Keep session on map reload

| Site | 1.09 | Patched |
|---|---|---|
| 0x19471B1 | `e8 ea 95 58 00` call 0x1ED07A0 (CSMultiPlayMan::Stop) | `e8 ea 8e 58 00` call 0x1ED00A0 |

0x19471B1 is in the stage task payload destructor 0x19470F0; with the summon-reload phase
(`[0x553D6D0]+0x84`) != 3 it stopped multiplayer (reason 0xFF000023 through wrapper 0xC8ED70)
during the guest's summoned map reload. 0x1ED00A0 is `add rdi,0x18; jmp 0xC8F570`, the class's
"matching controller exists" predicate: the destructor keeps its success branch, the room stays.
shadp2p found a second owner of the same policy (SprjSessionManager::OnMatchingCheck 0x13808B0,
stop at return 0x13809BC, when WorldChrMan cannot find the remote character while the world is
rebuilt) and deliberately left it, as it is also the real-disconnect path; ours too (A6 handles
it with the director's auto-rejoin).

### Party: SOS sign timeout 30s

| Site | 1.09 | Patched |
|---|---|---|
| 0x4927A5C (rodata float) | `00 00 34 43` 180.0 | `00 00 f0 41` 30.0 |

Read only at 0x18729ED in the SOS status update 0x1872360: a sign entry whose elapsed time
(`entry+4`) reaches it is dropped. The float before it (0x4927A58, 55.0) is the limit for
entries with `+0xc == 0`; unchanged. With 30 s, all entries end at 30 s.

### Party: Bells without Insight

Enabled with the others unless `BB_PARTY_BELL_NO_INSIGHT=0` (which also turns off the param
write below).

| Site | 1.09 | Patched | What |
|---|---|---|---|
| 0x157FA5C | `0f 9f c0` setg al | `30 c0 90` xor al,al | availability 0x157F200: `cost (goods+0x38) > Insight (PlayerGameData+0x84)` no longer greys the item out |
| 0x18C92BB | `41 0f b6 47 38 f7 d8` movzx eax,[r15+0x38]; neg eax | `31 c0 0f 1f 44 00 00` xor eax,eax | goods use 0x18C9160: the Insight delta (-cost) becomes 0 |

Applies to every goods item with an Insight cost (in 1.09 that is goods 200 / 201, the
Beckoning Bell; the resonant bells 205/225 cost 0). The runtime param write (below) zeroes the
Beckoning Bell row's cost as well, so either alone suffices.

### Party: Skip Online/Offline Choice (Online)

`BB_SKIP_NETWORK_CHOICE=online` (the launcher sets it in party mode). Same construction as the
offline `Skip Online/Offline Choice`: the dialog builder 0x1B39030 builds a functor on the stack
and returns its item's `operator()`. The builder adds two items: text 0x61E73 with functor
vtable 0x533BE80 and text 0x61E72 with vtable 0x533BE30 (PLAY OFFLINE, operator() 0x1B49CD0).
From the eboot's relocations (vtables are filled by R_X86_64_RELATIVE), vtable 0x533BE80 slot 2
is 0x1B49EB0, identical to 0x1B49CD0 except it stores 1 (online) instead of 0 in the step
argument it builds (vtable 0x533BD90). Patch at 0x1B39030 (46 bytes):
`push rbp; mov rbp,rsp; push rbx; sub rsp,0x18; mov rbx,rdi; lea rax,[0x533BE80]; mov [rsp],rax;
mov [rsp+8],rsi; mov rsi,rsp; call 0x1B49EB0; mov rax,rbx; add rsp,0x18; pop rbx; pop rbp; ret`.
Equivalent alternative (A0, docs/party/from_api_schema.md): the two operator() bodies differ
only in that flag byte (and rip-relative displacements; compared byte by byte), so flipping
0x1B49D04 `00` -> `01` in the offline one has the same effect; the XML variant calls the game's
own online functor instead and leaves the offline one intact. The online flag sets
FrpgNetMan+0xa = 1 and starts the NP chain 0x1C37B30; NetCtl/NP must report success or the step
error handler 0x1C3F330 drops back offline. Needs a live check that the online path then signs
in through the party NP layer.

patches.py: `BB_SKIP_NETWORK_CHOICE` `0` -> no patch (the title asks); `online` -> this patch
(none, with a warning, if the XML lacks it); anything else -> the offline patch. With `BB_PARTY`
set, the community `Disable HTTP Requests` patch is never applied (built-in or external).

## Runtime rules (seamless_rules.cpp)

### Patch report (init)

For each XML party site: `applied` (patched bytes in the image), `off` (1.09 bytes) or
`MISMATCH`, one line per patch: `Party seamless: Party: Bells anywhere: applied (8 sites ...)`.

### Param writes (tick)

The game's param repository: slot 0x5540340 (bbhost 0x5940340, SoloParamRepository_ptr),
holders at +0x70 (stride 0x48: `u32 count`, `caps[8]`), cap +0x70 -> FD4ParamResCap, +0x70 ->
the param file; cap name: wide string at cap+0x18 (pointer when capacity at cap+0x30 >= 8). Files
are format 4 / flags 7 (checked in `gameparam.parambnd.dcx`): records at +0x40, 0x18 bytes
`{u32 id, pad, u64 data offset}`. Row layouts from bbhost `params.hpp` (SP_EFFECT_PARAM_ST 0x210
bytes, maxHpRate +0x10; EQUIP_PARAM_GOODS_ST 0x6c bytes, consumeHeroPoint +0x38). The values
below were read from the 1.09 gameparam offline.

| Rule | Row | Field | 1.09 | Set | Off switch |
|---|---|---|---|---|---|
| Full HP, cooperator | SpEffectParam 9006 | maxHpRate +0x10 (f32) | 0.7 | 1.0 | `BB_PARTY_FULL_HP=0` |
| Full HP, invader phantom | SpEffectParam 9026 | maxHpRate +0x10 (f32) | 0.7 | 1.0 | `BB_PARTY_FULL_HP=0` |
| Bells without Insight | EquipParamGoods 200 (Beckoning Bell) | consumeHeroPoint +0x38 (u8) | 1 | 0 | `BB_PARTY_BELL_NO_INSIGHT=0` |

Polled every 0.5 s until the params exist, then re-checked every 2 s (a reload restores them; a
re-write is logged). A row with another layout or an unexpected value is left alone (logged).

**Insight.** Goods 200 has `consumeHeroPoint` 1 (205/225, the resonant bells: 0). The inventory
availability function 0x157F200 compares it with the player's Insight at 0x157FA51:
`movzx ecx, byte [goods+0x38]; cmp ecx, [PlayerGameData+0x84]; setg al` -> blocked (greyed out)
when the cost exceeds Insight. The Lua side (`eventcommon.luabnd`, extracted and read):
`OnEvent_Call_SOS` only does `LuaCallStartPlus(..., Call_WhiteSos, GetLocalPlayerId())` and
`Call_WhiteSos` calls `RecallMenuEvent`: no Insight call in the Lua, the check is native, on the
goods row. The use path reads the same field for the cost (UseGoods 0x18C92BB: `movzx eax,[goods+0x38];
neg eax`, located by the A0 RE, docs/party/from_api_schema.md), so writing the row's cost to 0
removes both the gate and the cost; the byte patch `Party: Bells without Insight` does the same
in code. Fallback, should a live test still show Insight needed or spent: the director writes
Insight 1 before raising `OnEvent_Call_SOS` and restores the old value after
(`coop::WritePlayerInsight`, `GameSnapshot::insight`).

### EMEVD instruction dispatch filter (0x17B93A0)

`SprjEmkInstructionDispatcher::Dispatch(this, SprjEmkEventIns* event, float dt) -> bool`
(bbhost `emk_system.hpp`); one caller, `SprjEmkEventIns::Update` 0x12ECDC0, which ignores the
result and advances the event by `instruction_delta` (1 by default) - a skipped instruction is
passed. Event: +0x28 event id, +0x68 map id, +0xA0 instruction index, +0xB0 instruction record
`{i32 bank, i32 id, u32 argument size, pad, i64 argument offset}`, +0xB8 copied arguments (else
EVD base `*(*(event+0xA8)+8)` + `*(base+0x78)` + offset). Replaced with `ReplacePrologue`
(prologue `55 48 89 e5 53 50`, position-independent), only when tracing or a rule exists:

- `BB_PARTY_EMEVD_TRACE=1`: logs each new `(event, bank, instruction)` (and its 1000th and
  100000th call) with map, index and up to 24 argument bytes; a summary line every 60 s.
- `BB_PARTY_EMEVD_SKIP="event:bank:id,..."` (`*` = any event): those instructions are skipped.
- Built-in table `kBuiltinSkips`: empty. The "send phantoms home" instructions (boss kill:
  common events, e.g. the 1003[6] "end if client" family in event 7200 per shadp2p) are to be
  identified with the trace in a live session; then skip only the exact (event, bank, id).

## What still needs a live 2-instance session

- Bells anywhere / after boss: guest's Small Resonant Bell in a negative or boss-cleared area
  and the host's Beckoning Bell there produce a summon (shadp2p validated this on its build).
- Map reload retarget: the guest's summoned map reload keeps the room (no Session Lost); the
  remaining stop at 0x13809BC (OnMatchingCheck) is expected on real disconnects.
- SOS timeout: the guest's sign disappears after 30 s and the director's re-ring recreates it.
- Full HP: a summoned phantom's max HP equals its own world max HP (both instances apply it).
- Insight: the host with 0 Insight can ring (inventory and director) and keeps its Insight.
- EMEVD: trace a boss kill with a phantom, pick the instructions that send phantoms home.
- Online title skip: the game reaches the main menu online through the party NP layer.
