# Party C4: cutscenes, story transitions, endings

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA of
`smoketest/out/eboot.elf`, vaddr 0 at file offset 0x4000; bbhost source = ours + 0x400000;
XML `Address` = ours + 0x400000). Decompiles: `tools/re/decomp.py`; disassembly: capstone.
EMEVD: every `event/*.emevd.dcx` expanded with `common.emedf` + FROM's `*.emeld` event names
(parser = `tools/party/flag_tool.py`), FMG names from `msg/engus/*.msgbnd.dcx`, talk ESD from
`script/talk/*.talkesdbnd.dcx`.

Markers: **[static]** proven from code; **[data]** proven from the game's own EMEVD/ESD/param
data; **[inferred]** name/shape based or DS1/DS3 analogy, needs the probe listed in section 7.

EMEVD notation: `bank[id]` (`2002[3]` = bank 2002 instruction 3). Map ids: m21 Hunter's Dream,
m22 Hemwick, m23 Old Yharnam, m24_00 Cathedral Ward, m24_01 Central Yharnam, m24_02 Upper
Cathedral Ward, m25 Cainhurst, m26 Nightmare of Mensis, m27 Forbidden Woods, m28 Yahar'gul,
m32 Byrgenwerth, m33 Nightmare Frontier, m34 Hunter's Nightmare, m35 Research Hall,
m36 Fishing Hamlet (from the boss-gauge NpcName ids and PlaceName FMG) [data].

---------------------------------------------------------------------------------------------

## 1. How cutscenes (remo) and movies are played

### 1.1 EMEVD instructions [data]

`common.emedf` bank 2002 ("ポリ劇" = remo):

| Instr | Japanese name | Args | Used by |
|---|---|---|---|
| 2002[1] | ポリ劇再生 | remo id, play mode | unused in 1.09 data |
| 2002[2] | ポリ劇再生＆PCワープ | id, mode, point entity, area, block | unused |
| 2002[3] | ポリ劇再生_プレイヤー指定 | id, mode, player entity | most cutscenes |
| 2002[4] | ポリ劇再生＆PCワープ_プレイヤー指定 | id, mode, point, area, block, player | boss intros with reposition |
| 2002[5] | …＆PCY軸回転ワープ | id, mode, rot X/Z, angle, Y move, player | unused |
| 2002[6] | ポリ劇再生 & 時間帯変化 & PCワープ_プレイヤー指定 | id, mode, point, area, block, player, **time-of-day** | Cathedral gate (tod 1), Willem memory (tod 2) |
| 2002[7] | ポリ劇再生 & 時間帯変化_プレイヤー指定 | id, mode, player, **time-of-day** | Rom/Blood Moon (tod 3) |
| 2002[8] | ダミーポリ劇再生＆PCワープ | point, area, block | teleport fades (m28, chalice) |

Remo id `AABBNNNN` = file `remo/sAA_BB_NNNN.remobnd.dcx` (all 42 files listed under
`dvdroot_ps4/remo`); `+1000` on NNNN is the sex variant (`1003[11]/[12]` sex branch, sex
value 1 → the `x1xxx` file, e.g. 21001010) [data].

**Play mode** (再生方法) values in the data: 0, 2, 8. Pattern [data]: every boss intro and the
DLC/Clocktower scenes run `1003[5]/[105]` on *MultiplayerState 2* and play the same id with
mode 2 in multiplayer, mode 0 otherwise; mode 8 only for the narrative first-time scenes
(char-make 24010005/24011005, first death 21000000, kidnap 28000010). Semantics [inferred, DS1
CutscenePlayMode analogy]: 0 = skippable, 2 = unskippable/no world pause (multiplayer-safe),
8 = skippable with fade. The mode is copied into the remo parameter object at `+0x20`
(`0x1CED0A0`: `*(param+0x20) = req+0x10`) [static]; its consumer was not traced (probe C1).

Other remo-related state:
* Flag **9180** = "a cutscene is playing" — every story event sets it ON one frame before the
  2002 instruction and OFF one frame after; common event 50 clears it on every load [data].
  (the flag category table (`flagRows`) row 9180 "first death → Hunter's Dream cutscene" is a misreading; it is
  the generic cutscene-in-progress flag, i.e. session/runtime, never sync.)
* **MultiplayerState** enum used by `3[6]`, `1003[5]/[6]/[105]` — `0x17B21A0` [static]:
  0 Host (role 0/2/5, or role 3, or ≥2 presentation members and own body `PlayerIns+0x78 == 0`),
  1 Client (`presentation+0x98 != -1` or role 6), 2 Multiplayer (presentation count `+0x14 > 1`),
  3 Connecting (SOS/summon pending or `+0x18 > 1`), 4 Singleplayer (count == 1).
  Presentation = `*(GSM 0x5556678 + 0x16F8)`, role = `SprjSessionManager 0x5540290 +0x124`.

### 1.2 Native path [static]

```
EMEVD dispatch 0x17B93A0  (bank 2002 → case 0x7d2)
  └ 0x17C53B0  bank-2002 handler (switch on instr id at [rec+4]; args at event+0xB8 or EVD base)
      id 1,3 → 0x131A9F0(_, remoId, mode, -1, -1, player)       call site 0x17C5585
               (instr 3: player 10000 is replaced by SprjEmkEventIns+0x60 at 0x17C5536..0x17C553F)
      id 2,4 → 0x131AD70(_, remoId, mode, &packedMap, point, 0, 0, player)  0x17C54FF / 0x17C5655
      id 5   → 0x131B540                                                    0x17C570C
      id 6   → 0x131BC10 (warp + time-of-day)                               0x17C5853
      id 7   → 0x131C170 (time-of-day)                                      0x17C596E
      id 8   → 0x131B230 (dummy remo + warp)                                0x17C5A8C
      then allocates a wait condition (vtable 0x532D570) and attaches it to event+0x30
      (0x12EC230): the event stays on this instruction until the remo ends.
```
All six are SprjLuaEventMan helpers (singleton slot `0x553B0C8`; `0x131A9F0` also has two Lua
callers, `0x1333347` / `0x1333391`). `0x131A9F0` [static]:
1. if `player != -1`: look the character up (`0x13C97A0(player, …)`) and snapshot its
   appearance into the request (`0x1894E10`, `0x14E2890(chr+0x4AC)`, `chr+0xCA`).
2. builds a request `{vtable 0x534C0E0; +0x10 mode; +0x14 remoId; +0x18 packed map
   (area<<24 | block<<16, derived from the id)}` plus a post-action object
   (vtable `0x539EF30`, `+0xC a4`, `+0x10 a5`).
3. `SprjRemo` singleton **`0x5540058`**: `*(+8)` = remo player; allocates a 0x150-byte task
   (`0x1CF41D0`, registers with `FD4RemoManager 0x54B22C0`) and queues it on `remo+0xE8`
   (`0x1CFED20`). Returns 1 if queued. `param_1` (rdi) is unused.

Remo state: `*(*(0x5540058)+8) + 0x168` bit 0 = **remo playing** (the EMEVD remo-state
condition `0x17B7700` and the main update `0x17692F0` read it), bit 1 = remo blocks
menus/inputs (`0x18208D0`, `0x18209A0`) [static].

**Time of day** (2002[6]/[7]) [static]: the request gets an extra action object, vtable
`0x539EFF0`, `+0xC = 1` (run at remo end), `+0x10 = tod`. Its end callback `0x1CECEB0`:
```
1ceced9: mov esi, 0x2648 ; mov edx, 3 ; ecx = TOD_TABLE[tod] ; call 0x13D0060   ; SetEventFlagValue(EventFlagMan 0x553B100, 9800, 3 bits, v)
1cecef7: mov byte [ [0x553B148] + 0x81 ], 1                                      ; world re-evaluates time-of-day
```
`TOD_TABLE @ 0x4925010 = {0, 4, 6, 7}` ⇒ flags **9800 = evening reached, 9801 = night,
9802 = Blood Moon** (value 7 sets all three). Every map's `時間帯変化` event and NPC/enemy
swaps read 9800–9802 [data]. These are world state, not "unclassified/never_sync" as
the flag category table (`flagRows`) row 9800–9899 says.

### 1.3 Movies [static, partly inferred]

`movie/`: `sprj_opening.mp4`, `sprj_staffroll.mp4`, `sprj_staffroll_dlc.mp4`,
`sprj_advertise.mp4`. No EMEVD instruction plays a movie. The path format `movie:/%s.mp4`
(UTF-16 at `0x49B8276`) is used only by `0x1FCB2E0` (movie open; callers `0x1FCB6B0`,
`0x1FCB760`, `0x1FCB820`, `0x1C4BF60`). The base names are not literal strings in the eboot
(built elsewhere) [inferred]. The staff roll is played by the game's ending step (1.4), not by
any script — so it cannot be triggered from EMEVD.

### 1.4 Ending → credits → NG+ [static]

* Lua `RequestEnding` (name `0x492DA92`, registered at `0x1342E6A`) = `0x1336C10`:
  `mov rax,[0x5556678]; mov byte [rax+0x1550], 1`. Accessors `0x156E7E0` set /
  `0x156E7F0` get / `0x156E800` clear; GSM ctor clears it (`0x15666CB`).
* The stage update `0x193AC10` reads global flags **21, 22, 23** inline (three inlined
  IsEventFlag tree walks over `EventFlagMan+0x38`, merging at `0x193FE8A`) and, when any is ON and `stage+0xE0 != 0`, sets
  `GSM+0x1550 = 1` (`0x193FEA8: 48 8b 05 c9 67 c1 03 c6 80 50 15 00 00 01`) and
  `GameDataMan(0x553B130)+0x6C` = **1 (flag 21), 2 (22), 3 (23)** = ending type.
* The in-game step leave decision `0x195B6A0` (called by `0x195BDA0`): if `GSM+0x1550`
  (`0x195B73A: 80 b8 50 15 00 00 00`), clear it and go to step state **8** instead of 1 —
  the ending path (class strings `EndingStep`, `SprjStepTask< EndingStep >` registered by
  `0x1957120`) [static for the branch, inferred that state 8 = EndingStep → staff roll →
  NG+ load].
* `2003[21]` (ゲームクリア周回加算) = bank-2003 handler `0x17C0420` case 0x15:
  `GameDataMan+0x68` (NG cycle) `+1`, clamped to 7 [static].
* `2003[27]` (タイトル抜け) sets `*(SprjEventMan 0x553B108 +0x10)+8 = 2` (return to title);
  only used behind debug flag 13400999 in Ludwig's kill event [static/data].
* There is **no NG+ prompt** in Bloodborne's code path: the ending step goes straight from the
  staff roll into the next cycle; common event 50 then sees flag **6604** ("ending reached,
  strip key items") on the first load, deletes the key items (goods 4000–4342, 700) and clears
  6604 [data]. 6600 (A), 6601 (B), 6602 (C), 6603 (any) are per-player shop/messenger unlocks.

---------------------------------------------------------------------------------------------

## 2. What happens to phantoms during cutscenes in vanilla

The guest (client) runs the host's map EMEVD on its own machine with the host's flag overlay;
a 2002 instruction plays only on the machine that executes it. No network replication of a
remo was found (SprjRemo has no session code; the only session references near the remo
update are the role-6 check in `0x1939970`) [static for the code read; "no packet" is
inferred, probe C2]. The scripts decide who sees what [data]:

* **Boss intros** (`ホストがボス部屋入場_初戦_*`, e.g. Gascoigne 12411802, Wet Nurse 12601802,
  Micolash 12601852, Gehrman 12101802, Ludwig 13401801, Orphan 13601801): the condition group
  requires `4[3] local player chr type == 0` (own-world body). Phantoms never run the remo.
  In multiplayer the host plays the same id with mode 2. Phantoms enter afterwards through
  `ゲストがボス部屋入場_*` (action button at the fog, sets the guest-entered flag) and
  `*_時間差入場ゲスト用対処処理` (late-entry fix-up: enable boss, mark intro done). They are
  **not sent home** by the cutscene; they just do not see it.
* **Story cutscenes with host-only guards** (`1003[6] End if Client` or chr-type 0 checks):
  Willem's memory 12401803 (tod 2), Rom → Blood Moon 13201803 (tod 3 + warp), Micolash
  post-fight 12601854, first Dream 9401, first sacrifice 9422, char-make 12410000 (mode 8),
  all three endings. Phantoms see nothing.
* **Phantom-aware scenes** (no client guard, explicit phantom branch): DLC entry grab
  12405263 (phantom plays 24000000 mode 2 then ends; host warps), Clocktower altar 13501105
  (host: warp 3502104; phantom: no warp if inside 3502102, else random 3502124–127).
* **Sent home**: not by cutscenes but by what follows them — boss kills that use `2003[12]`
  (BlockClear Lua 0x1385470/0x1385930), any host load (`2003[49]`, `2003[14]`, 2002[2]/[4]/[6]
  across maps → `0x13CDE30`, travel.md 1.0/1.4), and the ending (1.4, the host loads into the
  ending step; guests get room-gone → `OnLeave_Limit`).
  `2003[15]` (mid-boss defeat) does **not** end the session [inferred from name/DS analogy]:
  that is how the C-route keeps phantoms from Gehrman into the Moon Presence fight (4.4).

---------------------------------------------------------------------------------------------

## 3. Story transitions that end or forbid co-op (events and flags)

### 3.1 Time of day (evening → night → Blood Moon) [data]

| Step | Event | Trigger | Remo | tod | Who |
|---|---|---|---|---|---|
| day → evening | m24_00 event on a Cathedral Ward door (ObjAct 7030) | the door's ObjAct | a 2002[6] remo with warp | 1 → 9800 | local player, no client guard (host in practice: ObjAct on host world) |
| evening → night | m24_00 event of Amelia's skull ("the Chancellor's memory") | Amelia dead + action at the skull | a 2002[6] remo | 2 → 9800+9801 | host only (`1003[6]` + chr type 0) |
| night → Blood Moon | m32 event after Rom's death | Rom dead + within 12 m of Queen Yharnam at one of four regions | a 2002[7] remo | 3 → 9800–9802 | host only (chr type 0) |

After the Blood Moon remo 13201803 sets **70002802** and warps `2003[49] 2802958` (m28
respawn row) — a host load, so the session ends there in any case. Rom's kill itself
(13201800, `2003[12]`) already sends phantoms home.

Related multiplayer gates:
* common **9191/9192** (マルチ時間帯解禁フラグ制御 host/guest): host sets 6500 + (6501/6502/6503
  per 9800/9801/9802), all four in m26/m29/m33; guest sets 6400–6403 from its own world. The
  native consumer was not found (probe C3) [inferred: time-of-day matching gate].
* m28 **12800160** 初回生贄マルチ禁止 ("first sacrifice: multiplayer forbidden"): in Yahar'gul
  while 9802 is OFF, SpEffects **9120 + 9121** on the local player; removed when 9802 or leaving
  m28. Same pair as the Dream (3.3). SpEffectParam rows 9120/9121 differ from a neutral row only
  in byte `+0x13E` (161 / 160; probably `stateInfo`) [data; meaning inferred, probe C4].
* m24_01 **12411899** sets **2410** (Central Yharnam multiplayer-allowed bit) when Gascoigne
  and Cleric Beast are dead (already in event_flags.md).

### 3.2 Mergo's Wet Nurse (m26) [data]

* Boss-defeat **12601800** (`ボス撃破_レッサーデーモン（死と闇）`, gauge NPC 551000): after the
  kill `2003[12] 2600803` (phantoms home), item lot 55100000, flags **2601**, **9462**.
  No cutscene.
* **9462** = "final night" world state: Hunter's Dream m21 events branch on it (12100300
  time-of-day → burning Dream objects, 12100100 Doll, 12100143 Gehrman "wants to release you",
  m21 ev 50 enables Gehrman 2100800/Moon Presence 2100810 backread). Common ev 50 re-derives
  9462 from 12601800 on every load (`9450..9467` = per-boss "killed" mirrors).
* Micolash (12601850, `ボス撃破_悪夢の主`) → post cutscene **12601854** (26000000 or 26000005
  if 12604879), host only.

### 3.3 Hunter's Dream, Gehrman, Moon Presence, the choice [data]

Dream multiplayer gates:
* m21 ev 50: `if 12101802 OFF → SetEventFlag(2100, ON)` → area table `0x47304B0` blocks bells
  (travel.md section 2). m21 ev 0 (constructor) instr 278–280: SpEffect **9121** always,
  **9120** unless 12101802 is ON.
* **12101802** (ホストがボス戦開始_初戦_拠点老人) is set when the Gehrman fight first starts.
  It also does `SetEventFlag(2100, OFF)` itself. ⇒ **after the first Gehrman attempt has begun
  the Dream allows co-op** (no 2100, no 9120): retries use **12104810** (host: action button
  at gate 2101800, enters with turn anim) and **12104811** (guest entry), late-entry
  **12101803**. So "Gehrman unsummonable" is true for the *first* attempt only (the fight
  starts straight from the dialogue in a blocked area) [data; runtime probe C5].

The choice (talk ESD `t210305.esd` = Gehrman, the only file that writes both) [data]:
* "Submit" → **72100130** ⇒ **A ending 12100180** (Yharnam Sunrise): `1003[6] End if Client`;
  waits 72100130; 9180 ON; `2003[21]` (NG+1) *before* the remo; remo 21000010 / 21001010 (sex);
  trophy 1; flag 6604; starts 12100450/451/452 (unlock-item and covenant-rune bookkeeping);
  flags **21**, 6600, 6603.
* "Refuse" → **72100131** ⇒ **12101802**: remo 21000040 with PC warp to 2102808 (`2002[4]`),
  enable Gehrman 2100800, set 12104800, **2100 OFF**, start 9350 (boss BGM/HP setup).
* Gehrman killed → **12101800**: if **9900** OFF `2003[12] 2100800` (boss defeat: phantoms
  home), else `2003[15]` (mid-boss: session continues); item lot 15000/15005.
* **B ending 12100000** (Honoring Wishes): End if Client; waits 12101800 ON ∧ host chr type 0 ∧
  local player on any of hit parts 2103601–2103605 ∧ **9900 OFF**; 3 s; 9180; remo **21000020**
  (no sex variant); trophy 2; 12100450–452; NG+1; flags 6604, 6601, 6603, **22**.
* **C ending branch**: common **9905** ×4 (slots 4685–4688) increments value **9901** (4 bits)
  each time the local player gains a third-cord SpEffect; common **9909** sets **9900** when
  9901 ≥ 3.
* **Moon Presence 12101852**: End if Client; 12101800 ∧ host on 2103601–5 ∧ 9900; 3 s; remo
  **21000050** with warp 2102809, **mode 2 when MultiplayerState 2** — the C-route fight is
  scripted for co-op (guest entry **12104881**, late entry **12101853**); sets 12104850.
  Killed → **12101850** (`2003[12] 2100810`, phantoms home).
* **C ending 12100002** (Childhood's Beginning): End if Client; 12101850 ∧ host on 2103601–5;
  5 s; remo 21000030 / 21001030 (sex); trophy 3; 12100450–452; NG+1; flags 6604, 6602, 6603,
  **23**.
* Flags 21/22/23 → `GSM+0x1550` + `GameDataMan+0x6C` → ending step → staff roll → NG+ (1.4).
  `sprj_staffroll_dlc.mp4` presumably when the DLC is owned (6899) [inferred].

### 3.4 DLC entry and DLC gates [data]

* **12405263** (邪神投げでDLCマップへ, m24_00, rest=1): Amygdala 2400899 sends event message 710
  (grab) ∧ local player owns goods **4311** (Eye of a Blood-drunk Hunter) → invincible, 30
  frames, remo **24000000** (mode 2 if MultiplayerState 2); **if local chr type ≠ 0 → end**
  (phantom: remo only); host: flag **12401000**, `2003[49] 3402959` (Hunter's Nightmare
  respawn row, a load ⇒ session ends).
* **The grab's trigger is 12405260** (m24_00, rest=1): while the local player (chr type 0 or 1)
  stands in region **2402018** ("Event 邪神アニメ強制再生エリア", an 8 x 8 x 4 m box at
  (-20.5, -17.35, 95.1), the steps below the Oedon Chapel; floor about -15.5) it forces animation
  3000 (the grab) on 2400899, every 250 frames. The hand's hit sends message 710. Live 2026-10-15
  (parab4 b6b / b6c): a grabbed guest runs the HOST's 12405263 (the host holds the Eye): scene on
  both, then the host's question; a grabbed host the same.
* The grabbing Amygdala exists only under Blood Moon: **12400765** shows 2400899 when 9802 or
  12404001 (else sets 12405263 OFF).
* Arrival m34 **13401000**: if 12401000 → clear it, SpEffects 110–116, respawn point 3402958,
  start 9350.
* m34 **13400010** マルチ閉じ込め壁: wall 3401801 (the door at the back of Ludwig's arena) + SFX
  3403801 up while in multiplayer (state 2/3) AND Ludwig lives (9471 off); taken down only when the
  session is over AND 9471 is on (0[0] [11] + 3[0] [12], then a restart). A party stays after the
  kill, so the wall stayed (player report 2026-10-22). Party rules (`bossfog::ConfinementWalls`):
  open world on skips [7] / [8] (no wall), and party mode always skips [11] (down at the kill).
* Clocktower altar **13501105** (m35, needs goods 4017) is phantom-aware (section 2).
* **DLC ownership**: common **3500/3503** (DLC削除): if **6899** (set natively, not by EMEVD) is
  OFF: delete goods 4311, clear DLC lamp/route flags, and if in m34/35/36 warp
  `2003[49] 2102961` (Dream). Every party member must have the DLC entitlement or the DLC
  part breaks for them.

---------------------------------------------------------------------------------------------

## 4. Design: everyone sees each cutscene and reaches the same ending

Principle: vanilla already confines story cutscenes to the host machine and ends the session
on every load; we do not fight that. Instead the host **announces** every cutscene/ending it
plays, and each guest **replays it natively on its own machine** — immediately if it is still a
phantom in the host world (cosmetic), and on its own world after the session drops (state-
changing scenes and endings). Rejoin follows the B1 travel machinery.

### 4.1 Host capture (no new patch)

The A6 EMEVD dispatch filter already replaces `0x17B93A0` (`seamless_rules.cpp`). In it, when
`bank == 2002` on the host (role 3 or solo with a party), read the instruction record at
`event+0xB0` and the args (`event+0xB8`, else EVD base, exactly as `0x17C53B0` does) and send
`PARTY_CMD CUTSCENE {seq, map (event+0x68), event id (event+0x28), instr, remoId, mode,
tod (instr 6: u8 at args+0x14; instr 7: u8 at args+0xC; EMEVD args are packed with natural
alignment, see `flag_tool.unpack_args`), warp point/area/block}` before calling
the original. Also send `ENDING {type}` when the host's flag 21/22/23 is written (C2 flag hook
on `SetEventFlag 0x13CFCC0`, or poll `GSM+0x1550` / `GameDataMan+0x6C` in the tick).
Alternative hook site if the filter is off: entry of `0x17C53B0`
(`55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68`).

### 4.2 Guest live mirror (guest still a phantom in the host world) [design]

On `CUTSCENE` with a plain play (instr 1/3, or any instr whose warp stays on the same map):
from the main-thread tick (`0x2034770`), when not loading and no remo is playing, call
`0x131A9F0(rdi=0, esi=remoIdForMySex, edx=2, ecx=-1, r8d=-1, r9d=-1)` (SysV; returns al=1 when
queued) and poll `*(*(0x5540058)+8)+0x168 & 1` until it clears. Rules:
* Never use the warp/tod variants on a phantom (no `0x131AD70/0x131BC10/0x131C170`): position
  and time of day belong to the host world.
* Sex variant: if the host's id has a `+1000` sibling file, pick by the guest's own sex
  (`remo/sAA_BB_1NNN.remobnd.dcx` exists for 21001010, 21001030, 24011005, 28001040, 33001000).
* `player = -1` plays without the player snapshot [inferred]; to show the guest's own hunter
  pass its entity id (the value EMEVD substitutes for 10000 is `SprjEmkEventIns+0x60`; probe C6).
* Skip mirrors the guest already saw (per-save "seen" list keyed by remo id, see 4.3).
* Boss intros: the guest will be outside the fog; playing the intro there is purely cosmetic
  and harmless (host plays mode 2 and does not pause). Make it a setting (default on).

### 4.3 Deferred replay on the guest's own world ("story replay queue") [design]

Persist per guest (party save sidecar) a queue of `CUTSCENE`/`ENDING` items received with
`state_change = (tod != none) || ending || not-mirrored`. When the guest is in its own world
(role 0, not loading, `+0x1592 == 1`) — typically right after the host's load dropped the
session and before the B1 auto-rejoin — run each item:
1. play the remo (`0x131A9F0`, mode 0, own sex variant) and wait as in 4.2;
2. apply its state with the same natives the scripts use: tod → `SetEventFlagValue(0x13D0060)
   (EventFlagMan, 9800, 3, {0,4,6,7}[tod])` and `byte [*(0x553B148)+0x81] = 1` (exactly
   `0x1CECEB0`); story flags through the C2 allowlist (e.g. 70002802 after the Blood Moon);
3. only then let B1 rejoin. This gives every member the Willem memory, the Blood Moon, the
   Micolash scene, the DLC grab, the Clocktower altar, with their world state matching the
   host's.

C2 consequences (event_flags.md policy, documentation only here): 9800–9802 should be
`world_state` (host → guest, set-only) but applied **through the replay** (remo first, flags
at its end) rather than silently; 9180 is runtime (never sync); 21–23, 6600–6604, 9900/9901,
72100130/72100131 never via generic sync — only via 4.4.

### 4.4 Endings and credits for every member [design]

Vanilla ending events end on clients (`1003[6]`), and the host's ending step tears the room
down. Plan:
1. **Before the choice**: the party gathers in the host's Dream (needs travel.md's 2100 table
   patch for the lobby **and** handling of SpEffects 9120/9121, 3.3/5.2). Gehrman and Moon
   Presence fights are natively co-op scripted (12104811/12104881); on the C route Gehrman is a
   mid-boss (`2003[15]`), so the party stays together into the Moon Presence fight.
2. **Host**: plays its ending normally. The director sends `ENDING {type 1/2/3}` as soon as the
   flag (21/22/23) is set, before the host's room closes.
3. **Guests** (sent home by the room teardown, or leave voluntarily on `ENDING`): in their own
   world run the ending through native code, choosing one of:
   * **A (native trigger)**: TRAVEL to the own Dream (`0x13CDF30(2102961)`, the Dream's default
     respawn row used by m21 ev 0 and common ev 3503), wait for arrival, then
     `SetEventFlag(72100130, ON)`; m21's always-running **12100180** does the rest (NG+1,
     remo by sex, trophy, items, flags 21/6600/6603/6604) and flag 21 drives the native ending
     step → **staff roll → NG+** on the guest.
   * **B/C (scripted emulation)** — native triggers would need 12101800/12101850 (marks the
     guest's Gehrman/Moon Presence dead — fine story-wise) plus the 2103601–5 hit and 9900 OFF
     for B; to avoid mismatched 9900 state use direct emulation in the own Dream, mirroring the
     event bodies exactly: `SetEventFlag(12101800[,12101850])`; flag 9180 ON; remo
     (B 21000020; C 21000030/21001030 by sex) via `0x131A9F0` and wait; 9180 OFF; start
     12100450–452 is optional bookkeeping (their effects are flag sets, can be replayed via C2
     allowlist later); `GameDataMan+0x68 = min(+0x68 + 1, 7)` (= `2003[21]`); flags 6604,
     6601|6602, 6603; **last** 22|23 → native ending step → staff roll → NG+. Trophies are
     cosmetic (skip or call the trophy native later).
   * Probe C7 must confirm the stage-update condition `stage+0xE0` is true in the own Dream
     (otherwise also set `GSM+0x1550 = 1` and `GameDataMan+0x6C` directly, i.e. call the
     `RequestEnding` body `0x1336C10` after setting +0x6C).
4. **After credits**: every member is in NG+ in its own save (the ending step writes the new
   cycle; common ev 50 strips key items via 6604). The party director treats it as a C1 start:
   re-form at the first lamp. Party check: all members' `GameDataMan+0x68` equal, else warn.

Why not "let phantoms through the ending": the ending step is a full step change on the host
(`0x195B6A0` → state 8) with no notion of peers; a guest has no world to run it in while
summoned. Replaying in each own world gives each save its real ending, trophies, NG+.

### 4.5 DLC entry with a party [design]

The host's grab (12405263) is a `2003[49] 3402959` load → B1 TRAVEL (`0x13CDF30` hook already
sees id 3402959). Guest replay order: (mirror 24000000 if still summoned) → in own world
`SetEventFlag(12401000, ON)` (so m34 13401000 grants respawn 3402958 and the SpEffects) →
`0x13CDF30(3402959)` → rejoin. Requirements: guest DLC entitlement (6899) else 3503 kicks it
out; no need for goods 4311 on guests. Expect the m34 multiplayer wall (13400010) until Ludwig.

### 4.6 Other host-only scenes worth a replay entry

The scenes worth a replay entry, by event: the char-make scene of m24_01 (mode 8; none, each player has their own), the first
death scene of m21 (implies one flag), the first kidnap scene (every member sees it since 0.7.3, whoever was grabbed), the Hemwick
carriage and the Lecture Building grab (both a TRAVEL warp), the brain shutdown scene of m26 (mode 2 in MP), the Patches push of
m33 (a warp inside the map), Ludwig's kill scene (boss flags) and the Orphan of Kos scene (nothing). The remo ids and implied
flags are read from the player's EMEVD files.

---------------------------------------------------------------------------------------------

## 5. Patches

### 5.1 No code patch is required for the recommended design

All capture points are reads in hooks that already exist (A6 dispatch filter, B1 `0x13CDE30`/
`0x13CDF30` hooks, C2 flag hook) plus director calls into existing natives on the main thread.
Byte-verified reference sites (1.09 bytes from `eboot.elf`):

| Site | Bytes | Use |
|---|---|---|
| 0x17C53B0 | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68` | bank-2002 handler entry (alt. capture hook) |
| 0x131A9F0 | `55 48 89 e5 41 57 41 56 41 55 41 54` | play remo (director call) |
| 0x1CECEB0 | `55 48 89 e5 48 89 f8 83 78 0c 01 75 41` | time-of-day apply (end callback) |
| 0x1336C10 | `48 8b 05 61 fa 21 04 c6 80 50 15 00 00 01 c3` | Lua RequestEnding |
| 0x193FEA8 | `48 8b 05 c9 67 c1 03 c6 80 50 15 00 00 01` | stage update: flags 21–23 → ending request |
| 0x195B73A | `80 b8 50 15 00 00 00 74 3f c6 80 50 15 00 00 00` | ending request consumed → step 8 |

### 5.2 Dream / Yahar'gul "multiplayer forbidden" SpEffects (data, inferred)

Besides the area-table patch (travel.md: `dword 0x47304B0` 2100 → -1), the Dream lobby before
the first Gehrman fight and first-visit Yahar'gul apply SpEffects 9120/9121. Options:
* Runtime param write (like A6's SpEffect 9006/9026): SpEffectParam rows **9120, 9121**, byte
  `+0x13E` **161 → 0** and **160 → 0** (only differing field besides `+0x0A/+0x0B`,
  `+0x9E..+0xB3`, `+0x167`, which equal other marker rows) [inferred meaning; probe C4 first].
* Or EMEVD filter rule — the current `BB_PARTY_EMEVD_SKIP` keys on (event, bank, id), which in
  event 0 would also skip SpEffects 110/111; extend the filter key with map (`event+0x68`) and
  instruction index (`event+0xA0`): skip `m21 ev 0 idx 278, 280` and `m28 ev 12800160 idx 4, 5`.

### 5.3 Rejected / not needed

* Removing `1003[6]` from ending events: the ending would still require a world the guest does
  not own while summoned.
* Keeping the session through the host's ending step: no peers survive a step change (travel.md
  B2 analysis applies).
* Live remo replication over the network: not present in vanilla; the director mirror (4.2) is
  simpler and lossless.

---------------------------------------------------------------------------------------------

## 6. Quick reference

| Item | Address / id | Status |
|---|---|---|
| bank-2002 handler | 0x17C53B0 | static |
| play remo / +warp / +rotate / +warp+tod / +tod / dummy+warp | 0x131A9F0 / 0x131AD70 / 0x131B540 / 0x131BC10 / 0x131C170 / 0x131B230 | static |
| SprjRemo slot; playing bit | 0x5540058; `*(+8)+0x168` bit0 | static |
| FD4RemoManager slot | 0x54B22C0 | static |
| tod action vtable / apply / table | 0x539EFF0 / 0x1CECEB0 / 0x4925010 {0,4,6,7} | static |
| time-of-day flags | 9800 evening, 9801 night, 9802 Blood Moon | static+data |
| cutscene-playing flag | 9180 | data |
| MultiplayerState eval | 0x17B21A0 (0 Host,1 Client,2 Multi,3 Connecting,4 Solo) | static |
| ending request | GSM 0x5556678 +0x1550; Lua RequestEnding 0x1336C10 | static |
| ending flags → request | 0x193AC10 @0x193FEA8; type → GameDataMan+0x6C (1/2/3) | static |
| ending step switch | 0x195B6A0 @0x195B73A → state 8 | static (EndingStep name inferred) |
| NG cycle | GameDataMan 0x553B130 +0x68 (2003[21], clamp 7) | static |
| movie open | 0x1FCB2E0, fmt `movie:/%s.mp4` @0x49B8276 | static |
| endings A/B/C | three m21 ending events; flags 21/22/23 | data |
| choice flags | submit / refuse (t210305.esd) | data |
| Gehrman / Moon Presence | start, kill, guest, late and retry events of the two fights | data |
| C route | 9905 → 9901 (4-bit) ≥3 → 9909 → 9900 | data |
| Dream gates | 2100 (m21 ev 50 unless the Gehrman start event), SpEffect 9120/9121 (m21 ev 0) | data |
| Blood Moon, Willem memory / night, Cathedral evening | see 3.1 | data |
| DLC entry | m24_00 grab event (goods 4311, msg 710), warp to m34; arrival event in m34 | data |
| DLC ownership | 6899 (native), common 3500/3503 | data |
| ending NG+ cleanup | 6604 (common ev 50) | data |

## 7. Runtime probes (to close the [inferred] items)

* **C1** play mode semantics: log `remoParam+0x20` consumers; compare skip button and world pause
  for mode 0/2/8 (host solo vs with a phantom).
* **C2** host boss intro with a phantom present: does the phantom see anything (trace 0x131A9F0
  on the guest; packet log for an unknown type during the remo)?
* **C3** 6400–6403 / 6500–6503 consumers: hardware watch on their bits during SOS search.
* **C4** SpEffect 9120/9121 effect: in the Dream before 12101802 with the 2100 table patch, ring
  the bells with and without 9120/9121 removed.
* **C5** Gehrman retry co-op: after 12101802 is ON, host rings Beckoning in the Dream (vanilla,
  no patches) — expect success.
* **C6** `SprjEmkEventIns+0x60` value on host and guest (expected local player entity id);
  call `0x131A9F0` with -1 vs that id and compare the cut.
* **C7** own-world ending replay on a test save: set 72100130 in the Dream (A) and, separately,
  the emulated B sequence; confirm `0x193FEA8` fires (stage+0xE0), `GameDataMan+0x6C`, staff
  roll, NG+ load, `+0x68` incremented once (A sets it before the remo, the emulation must not
  double it).
* **C8** host ending with guests present: order of `ENDING` send vs room close (`0x19471B1` /
  `0x193C657`), guests' `OnLeave_Limit` path.

## 8. Implementation (gpu/shim/party/party_story.{h,cpp})

* Host capture: entry hook (`HookPrologue`) on the bank-2002 handler `0x17C53B0(this, event)`,
  independent of the A6 filter (which is only installed for trace/skip rules). Argument layouts
  read from the handler's disassembly [static]: 1 `{id, mode}`; 2 `{id, mode, point, u8 area,
  u8 block}`; 3 `{id, mode, player}`; 4 = 2 + player @0x10; 6 = 4 + u8 tod @0x14; 7 `{id, mode,
  player, u8 tod @0xC}`; 8 `{point, u8 area @4, u8 block @5}` (not captured: B1 travel). Only
  plays of the host's own world (`+0x1592 == 1`). Ending events 12100180/12100000/12100002
  become `ending {1,2,3}` at their remo instruction (before flag 21/22/23 and the room close);
  the tick also polls 21/22/23 and the 9800 3-bit value (baseline taken at each world-up).
* EVENT `story` JSON: `{seq, kind cutscene|ending|time_of_day, id, mode, instr, event, map, tod,
  warp_point, warp_map, flags[]}`.
* Guest (`GuestStory`, pure, `party-story-test`): phantom (role 6) -> live mirror, mode 2, own
  sex; then a replay queue in the own world (remo mode 0 unless mirrored, tod never backwards,
  story flags); endings A/B/C as in 4.4 (B/C: `ForceEnding` = `GameDataMan+0x6C`, `GSM+0x1550`
  only if no loading screen 8 s after the flag; A is never forced). Queue + seen ids persist in
  `<user>/party_story.json`. The director does not ring the guest bell while a replay runs.
* Sex: 1003[12] jumps to the `+1000` branch when `*(PlayerIns->vfunc[0x1C8]() + 0xCA) == 0`
  (0x17bedd0 case 0xc) [static].
* Player argument: 10000 resolves to `WorldChrMan+0x60` in `0x13C97A0` [static], so the guest's
  own hunter is snapshotted (probe C6 answered statically; `BB_PARTY_STORY_PLAYER=-1` for none).
* Runtime: `BB_PARTY_STORY_TEST=cutscene:21000040` in the Hunter's Dream (single instance):
  `0x131A9F0` queued, playing bit rose, fell after 85 s, control back, world up;
  `cutscene:21000000,mirror` (mode 2, the phantom call): played 27.6 s, control back. The sidecar
  replayed an unfinished item after a restart (persistence path).

## 9. Replays while summoned (seamless play, 2026-10-10)

The replay queue no longer waits for an own-world phase that B2 may never have. Once the guest is
a steady phantom (role 6, `kStableTicks`) and calm (no HP lost for 15 s), `GuestStory::Step`
runs the first queued item **except endings** (`summoned_replay`, `BB_PARTY_STORY_SUMMONED=0`
restores the old behaviour): a scene the guest missed plays through the phantom path (mode 2,
the live mirror's call), then the time of day and the story flags go into the **own pool**
(`progress::WriteFlagOwnSave`; 9800..9802 as three bits, no world-refresh byte - the world shown
is the host's), never into the host-world overlay, followed by a save request. `st.tod_value`
is read from the own pool while summoned ("never turn the clock back" compares the own save).
Endings run last, from the summoned state too (2026-10-11, `summoned_ending`,
`BB_PARTY_STORY_ENDING_SUMMONED=0` restores the wait for the own world): once the guest is a
steady, calm phantom with an ending queued, the program starts with the lamp warp
`0x13CDF30(2102961)` run as a travel replay (`RunLampWarpReplay`: travel capture, guest-lamp
interceptor and send-home retarget stand aside). A client's lamp warp leaves the host's room and
loads its **own** world (as every B1 travel follow), so the program's `WaitArrive` (load seen,
own world, m21, steady) is met in the guest's own Hunter's Dream and the ending runs there exactly
as at home: A sets 72100130 (m21 12100180 does the rest), B / C emulate the event (flags, remo by
sex, NG cycle +1, 6604 / 6601|6602 / 6603, last 22|23) -> native ending step -> staff roll -> next
cycle. The `TravelDream` op skips the warp only in the **own** Dream (`own_world`), not in the
host's m21. The director rings no bell while the ending runs (`StoryBusy`), so the guest rejoins
the party only from its new cycle. After our ending flag the story tick logs `ending: staff roll /
ending step t=...` every 10 s (the world is down for minutes; it also keeps the harness reaper
away) and `ending: the next cycle's world is up ...` with the NG cycle; `NG cycle N at world-up`
is logged once per world-up.

0.7.6: a guest that answers YES plays the HOST's ending type (`GuestEndingType`): until 0.7.5 B/C followed the
guest's own flag 9900 (three cords), so the ng_plus_coop session's guest without cords got B (remo 21000020)
after the host's C. The host's `ending_sent` latch resets with every NG-cycle change. Smoke phase `ef`
(group `ending_flow`): the default save's guest, `ending:3,summoned` + `BB_PARTY_ENDING_TEST=yes`: remo
2100[01]030, NG step once, flag 23, staff roll screenshots.

Test hook: `BB_PARTY_STORY_TEST=ending:<1|2|3>[,summoned]` (+ `BB_PARTY_STORY_TEST_DELAY`) queues
that ending as if the host had sent it (`summoned`: only once session role 6).

Live (copied saves only):
* r3 (2 instances, guest summoned in the host's Dream, `ending:2,summoned`, delay 75 s): guest
  `test: ... ending 2 ... queued (session client, NG cycle 0)`, `ending: warping to the own
  Hunter's Dream (2102961) - leaving the host's world (summoned)`, then in its own world (session
  idle): `flags set ON (12101800=1, 9180=1)`, `replay: remo 21000020 mode 0 -> queued`, `remo ended
  after 72.3 s`, `9180=0`, `NG cycle 0 -> 1`, `6604=1,6601=1,6603=1`, `22=1`, `world down` (the
  ending step; Mv_AudioPlayThread = the staff roll movie).
* r4 / r5 (1 instance, `ending:2`): screenshots 10 / 45 / 100 s after flag 22 show the staff roll
  (`Hidetaka Miyazaki`, `Lead Character Artist`, `Lead 2D Artist` ...); r5: `staff roll / ending
  step t=100 ... t=381 s`, then `NG cycle 1 at world-up (map m24_01_00_00 ...)` - the next cycle
  starts in Iosefka's Clinic with its opening cutscene 24011005 - and `the next cycle's world is up
  391 s after the ending started (NG cycle 1, map m24_01_00_00)`.
* r7: the same save copy booted again (Continue, no re-copy): `NG cycle 1 at world-up (map
  m24_01_00_00)` - the NG+ cycle is in the save (the backup save is NG 0).
Unit tests: `test_summoned_ending`, `test_missed_scene_replayed_while_summoned` (party-story-test).
Open: the host's own ending with B2 sessions (does its ending step close the room before the
guest's warp?) - the guest no longer depends on it, it leaves by itself.
Unit tests: `test_mirror_then_replay` (now finishes as a phantom),
`test_replay_waits_home_when_summoned_off`, `test_missed_scene_replayed_while_summoned`.
