# Party items and rewards for guests (Phase C3)

Bloodborne 1.09, our port. **All addresses are our offsets** (raw ELF VA of `smoketest/out/eboot.elf`;
bbhost source = ours + 0x400000). Decompiles: Ghidra (`tools/re/decomp.py`); disassembly and byte checks:
capstone over the ELF. Data: `dvdroot_ps4/param/gameparam` (ItemLotParam, EquipParamGoods/Weapon/Protector,
SpEffectParam, GameAreaParam), every EMEVD + EMELD (FROM's event names), `msg/engus/item.msgbnd` (names),
read with `tools/party/flag_tool.py` helpers.

Markers: **[static]** proven from code/data here, **[inferred]** from names/shape, needs one runtime check
(probes in section 8).

"Guest" = a summoned white phantom in the host's world. In code that state is
`isClient = (SprjSessionManager+0x124 == 6) || (NetworkFlow slots(+0x16F8)+0x98 != -1)`, usually combined with
`GSM(0x5556678)+0x1592 == 0` ("I am in someone else's world", see travel.md 1.0).

## 0. Short answers

| Question | Answer |
|---|---|
| Can a guest pick up world treasure (corpse loot)? | **No, it is never created on a guest.** MSB treasure spawn `0x17DC300` returns at `0x17DC3D6` (role 6) / `0x17DC3E3` (slot +0x98 != -1). [static] |
| Enemy drops? | Yes, rolled locally on the guest's machine, but on a client only items with `isGuestDrop = 1` survive: in the data that is only **Blood Vial (1000)** and **Quicksilver Bullets (900)**. Filter at `0x17DD75D`. [static] |
| Boss drops / key items / remembrance-type items (Oedon Tomb Key, Gold Pendant, badges, chalices, umbilical cords, keys)? | **No.** Boss events award with EMEVD `2003[4]`, which a client skips twice: the script jumps past it (`1003[105]` "jump if client") and `0x17DDC50` refuses `hostOnly` lots on a client (`0x17DDCD9..0x17DDD3E`). Keys that are world treasure (Hunter Chief Emblem, Orphanage Key, Iron Door Key, Lunarium Key, Upper Cathedral Key and so on) are not spawned on the guest. [static] |
| NPC quest rewards? | No for EMEVD gifts (`common 9040/9100/9110`, `2003[4]`, host only). Talk-script (ESD) gifts were not analysed. [static for EMEVD] |
| Blood Echoes | Enemy kills: **50 %** for a client (`0x18FC93D`). Boss clear bonus (GameAreaParam): **50 %** for a phantom (`0x1335578..0x1335591`). Black/other phantoms (chr type 2) get no kill echoes (`0x18C5179`). [static] |
| Insight | A white phantom gets **+1 Insight** when the host's boss dies: `BlockClear2` applies SpEffect **4680** (`heroPointDamage = -1`) at `0x13852B1`. The host pays 1 Insight per Beckoning Bell (from_api_schema.md 8). [static; that -1 means +1 is inferred from the name] |
| Bell-related items | Covenant/Vermin lots in `BlockClear2` (5900/5920/5930 = Vermin, 5910/5940/5950 empty) are given with `hostOnly = 0`, so clients get them too. Bells themselves come from per-player Messenger gifts (lot 10010, flag 6622, personal block 6000-8999; the Beckoning Bell is the director's grant). The Old Hunter Bell (lot 10050, flag 6670) is replayed from the host since 0.7.3 (10.8). [static] |
| Shops, Doll, level-up | Not while summoned. Phantoms cannot use lamps (lamp gate `0x12F8315`, travel.md), and the Dream is host-only for clients (m21 `1003[6]` guards). Shop stock flags are per-player (6000-8999, kept in the own pool in load mode 1). Level, echoes, insight and inventory live in the guest's own `PlayerGameData` (`[[0x553B130]+8]`), so they persist [inferred: not overlaid like flags]. |

## 1. Globals and functions

| Symbol | Address | Notes |
|---|---|---|
| MapItemMan slot | `0x553D6E0` | `*(u64*)slot` is passed as `rdi` to every lot function (`0x17C17F8`, `0x132E484`) |
| SprjGaitem slot | `0x553E990` | instance handles for weapons/armour/gems (`0x1A876D0`, `0x1A88130`) |
| GameDataMan slot | `0x553B130` | `+8` PlayerGameData: `+0x70` stat fed to the roll as a float (luck-like, [inferred]), `+0x84` Insight, `+0x90` level, `+0x94` echoes (clamped to 999,999,999 in `0x131F980`), `+0xA4` remembered chr type, `+0xF8` float boss-clear-bonus accumulator, `+0x328` inventory |
| Chr-type capability table | `0x47348A0` | 14 × 12 bytes indexed by `PlayerGameData+0xA4`; byte **[2]** = "may receive item lots" (types 0, 1, 8 = 1; 2, 3, 4, 5…13 = 0). Users of byte [2]: only `0x17DC300`, `0x17DD0B0`, `0x17DDC50` [static] |
| ItemLot roll | `0x17CEAF0(LotReq*, char checkGlobal, ctx)` | reads ItemLotParam, skips items whose flag is set, rolls with the game RNG (`GSM+0xA78/+0xA80`) |
| Lot result builder | `0x1A8B080(LotReq*)` | the plan's "MapItemMan lot→items": calls the roll, then builds `{flag, rarity, n, {gaitemHandle, count}×n}` records (0x38 each) at `LotReq+0x88`, count at `+0x80`. It does **not** give anything |
| **Award lot to inventory** | **`0x17DDC50(MapItemMan*, int lot, char hostOnly)`** | the give function (section 2.3) |
| Give item list | `0x17D89F0(MapItemMan*, ItemList*)` | inventory add, overflow to storage `0x14D9A80/0x14DA0A0` or dropped on the ground `0x17DA960`, "item get" popup `0x13107B0(1, …)` |
| MSB treasure spawn | `0x17DC300(MapItemMan*, entityHandle, MsbEvent*, pos*, …)` | only for MSB event type 4 (`[rdx+0xC] == 4`, `0x17DC32B`) [inferred: 4 = Treasure as in the DS1 MSB] |
| Enemy-death drop | `0x17DD0B0(MapItemMan*, chrHandle, npc*)` | from `0x18C4F90` (chr death) |
| Hawk-girl drop (EMEVD 2003[34], DS leftover) | `0x17DB8C0` | spawns a lot as a map item |
| Pick up map item | `0x17D8570(MapItemMan*, &handle)` | from the action-button handler `0x12FD880` (vtable slot `0x531CC70`) |
| Remove map item (+ set its pickup flag) | `0x17D6780(MapItemMan*, &handle, char setFlag)` | |
| Echoes for a kill | `0x18C5070` → `0x18FC860(float soul, PlayerIns*, enemy, bool bonus, bool isClient)` | pays through `PlayerIns->vtbl+0x3D0 (AddSoul)` |
| Boss clear bonus | `0x1335400` (Lua "SetClearBonus") fills `+0xF8`; `0x13356C0` (Lua `AddBlockClearBonus`) pays it | |
| Lua `BlockClear2` (boss killed, every player) | `0x13849A0` | then `BlockClear2_1 0x1385470` (guests sent home, travel.md) |
| Lua `GetRateItem` / `GetRateItem_IgnoreMultiPlay` | `0x132E480` (hostOnly = 1) / `0x132E4A0` (hostOnly = 0) | both `→ 0x17DDC50` |

Lua names come from the registration code (`lea rax, fn` followed by the name `lea rdx`): `0x133D337`
(GetRateItem), `0x133D37D`, `0x138E729` (BlockClear2), `0x13421F2` (AddBlockClearBonus).

## 2. From a lot id to the inventory

### 2.1 ItemLotParam row (BB layout, matches the code in 0x17CEAF0) [static]

| Offset | Field |
|---|---|
| `+0x00..0x1F` | `lotItemId01..08` (s32) |
| `+0x20..0x3F` | `lotItemCategory01..08` (0 weapon, 1 protector, 2 accessory/rune, 4 goods, 8 gem) |
| `+0x40..0x4F` | `lotItemBasePoint01..08` (u16 weights) |
| `+0x50..0x5F` | `cumulateLotPoint01..08` |
| `+0x60..0x7F` | `getItemFlagId01..08` (0 = use the lot-wide flag) |
| `+0x80` | **`getItemFlagId`** (lot-wide pickup flag) |
| `+0x84 / +0x88` | `cumulateNumFlagId` / `cumulateNumMax` (drop-counter value) |
| `+0x89` | rarity |
| `+0x8A..0x91` | `lotItemNum01..08` (u8 counts) |
| `+0x92` | u16 bits: `enableLuck01..08` (bits 0-7), `cumulateReset01..08` (bits 8-15) |

**Chains:** the roll keeps going with `lotId+1, +2…` while rows exist (`0x17CEC00`), so a set such as
2110010/2110011 (Gehrman's cap and garb) is one award. **Flag mapping:** item flag =
`getItemFlagIdNN != 0 ? getItemFlagIdNN : getItemFlagId`, plus the request's byte `LotReq+9` (0 for awards,
an NPC-param offset for enemy drops). In the shipped data `getItemFlagId01..08` is always 0, so **the
lot-wide `getItemFlagId` is the flag**. Treasure lots follow `flag = 50,000,000 + lotId` (784 lots; chains
share the first row's flag; all 811 treasure rows are deterministic: one item per row with weight 100).
Overall 1,377 distinct flags over 7,187 lots that have a flag; 50 flags are shared by alternative lots (NPC
NG+ variants such as 34000/34030/34035 → 50001800), so **flag → lot is not unique** outside treasure.
Lots with `getItemFlagId = -1` (boss drops such as Oedon Tomb Key 31000 and Gold Pendant 50000001) have no
pickup flag at all; vanilla relies on the awarding event's own completion flag.

### 2.2 LotReq (stack struct built by every caller) [static]

`+0x00 lotId`, `+0x04 float` (`0x1581830(PlayerGameData+0x70 as float, +0x670)`), `+0x08 u8 checkFlags = 1`,
`+0x09 u8 flagOffset`, `+0x0A u8 rank`, `+0x0C` out count / `+0x10` out records (0x50 each:
`+0 flag, +4 rarity, +5 n, {+8 itemId, +0xC category, +0x10 count, +0x11 slot} × n`), `+0x20..` area
lot-modifier data (`0x553B148` table), `+0x80/+0x88` resolved records (see 1). Freed with `0x1A8AF00`.

The roll (`0x17CEAF0`) has one global gate: if byte `0x553D6D8 != 0` and `checkGlobal == 1` it produces
nothing (meaning unknown; probably a debug switch).

### 2.3 `0x17DDC50(MapItemMan*, int lot, char hostOnly)` — the award [static]

```c
pgd = GameDataMan->+8; if (!pgd) return;
st  = GSM->+0x16F8;
if ((role == 6 || st->+0x98 != -1) && GSM->+0x1592 == 0)  allowed = 0;     // 0x17DDCD9/0x17DDCE2/0x17DDCF4
else allowed = table_0x47348A0[pgd->+0xA4].byte[2];                          // 0x17DDD21
if (!allowed && hostOnly) return;                                            // 0x17DDD3A xor r14b,1 ; 0x17DDD3E je
LotReq r = {lot, luck, checkFlags=1, …};  0x1A8B080(&r);
for each resolved record:
    list = {n, {gaitem, itemId|category<<28, count, -1} × n};
    0x17D89F0(MapItemMan, &list);                       // inventory + popup
    if (record.flag >= 0) tree_bit(record.flag) |= 1;   // direct OR into the *current* flag tree, no SetEventFlag
0x1A8AF00(&r);
```
Callers: EMEVD `2003[4] アイテム取得` (`0x17C17F8`, hostOnly = 1); EMEVD `2003[36]
クライアントでもアイテム取得` "award even on a client" (hostOnly = 0); Lua `GetRateItem` (1) /
`GetRateItem_IgnoreMultiPlay` (0); `DeadInfoMsg_Host 0x1382A50` (lots 5510/5610/5710/5810, 0),
`DeadInfoMsg_ForceJoinBlack 0x1382ED0` (5500/5600/5700/5800, 1), `TextEffectEnd_PK_Success 0x1383AF0`
(16581, 1), `TextEffectEnd_NPCPK_Success 0x1383BD0` (5020, 0; 16581, 1), `BlockClear2 0x13849A0`
(5910/5930/5940/5950, 0).

Note: the flag is written into whatever tree is live. On a guest in load mode 1 that is the host-world
overlay, which is thrown away on return (event_flags.md 3); the item stays. That is how vanilla
`2003[36]` awards (Bloodied Arm Bands and so on) can be obtained twice.

### 2.4 Map items (treasure, enemy drops, shared drops)

* Record kinds (`+0x20 & 0xF`): 1 hawk drop, 2 enemy drop (`0x17D5320`), 3 treasure / object (`0x17D4FB0`);
  `+0x24` Lua event id, `+0x28` pickup flag, `+0x48` u16 flags (`0x40` takeable, `0x80` network-shared),
  `+0x54` n items, entries from `+0x58` (16 bytes each).
* **Treasure** (`0x17DC300`): gate `0x17DC3CF cmp [role],6 / 0x17DC3D6 je`, `0x17DC3DC cmp [st+0x98],-1 /
  0x17DC3E3 jne`, then chr-type byte [2] (`0x17DC411`). A guest never gets the object, so it cannot loot it.
* **Pickup** `0x17D8570`: unshared item → `0x17D6780(…, 1)` (remove; sets `+0x28` flag and fires the `+0x24`
  Lua event only when `!isClient || GSM+0x1592 != 0`, `0x17D6818..0x17D6831`) then `0x17D89F0` (give).
  Shared item (`0x80`) on a client (`0x17D85F4`) → packet `0x2C {1, 5, handle}` to the host via
  `0x1789A40(*0x553D6D0, 0x2C, buf, 8)`; on the host `0x17DE6E0` + give unless role 6 (`0x17D88BB`). Spawning a
  shared item on a client goes the same way (`0x17D8370`, packet 0x2C subtype 3). [static; the host's reply
  path for 0x2C is not traced]
* **Pickup animation** (live 2026-10-20, agent pickupanim): the action-button handler `0x12FD880` calls the pickup, then
  `0x132E960(luaEv, 4010, 1|2)`: Lua event 4010 locally and, with `+0x8B` set, packet `0xE {0xC, 4010, arg, 0, playId}` to
  every other member (playId = own PlayerGameData `+0x10`). `global_event.lua` `OnEvent_4010_1/2` ->
  `Luafunc_PlaySynchroAnimation`: a net message plays `ANIMEID_PICK` (7520; TimeAct 101140) on entity
  `NET_PLAYER (10001) + GetPlayID()`, which `0x13C97A0` resolves to the first player chr (WorldChrMan `+0x38`, the
  local one included) whose `+0x3B8` equals the id. Remote chrs get `+0x3B8` from their GameData row `+0x10`
  (`0x1E50090` -> `0x18DB500` -> `0x18D8100`). The title join and the A5 arrival wrote a fixed `+0x10 = 2`, so with
  two or more guests they shared id 2 and one guest's pickup knelt the other guest's own character (and, on the
  host, the first chr with id 2). Fixed: the own room member id (`SummonedPlayId`, party_b2.cpp; host 1, guests
  2, 3, 4, what a vanilla join of a second guest left: 3). `BB_PARTY_ANIM_WATCH=1` logs ids and animations.
* **Enemy drops** (`0x17DD0B0`): byte [2] gate only (`0x17DD1C3`; types 0/1/8 pass), then
  `isClient` is stored at `0x17DD193` and used once at `0x17DD75D`: a client keeps an item only if its
  equip param has `isGuestDrop` (goods byte 0x47 bit 3, weapon 0x104 bit 4, protector 0xDF bit 0; gems
  0x8C bit 3). Only goods 900 and 1000 have it.

## 3. Phantom/host checks found (one table)

| Site | Check | Effect for a guest |
|---|---|---|
| EMEVD scripts | `1003[105] (label 1, state 1 = client)` before boss awards; `1003[6] (end, client)` in many NPC/treasure events | event skips the award on clients |
| `0x17DDCD9..0x17DDD3E` | isClient && in host world → hostOnly lots refused | no `2003[4]` / `GetRateItem` lots |
| `0x17DC3CF..0x17DC418` | role 6 / slot +0x98 / chr-type byte [2] | no MSB treasure objects |
| `0x17DD186..0x17DD193`, `0x17DD75D` | isClient → `isGuestDrop` filter | only vials/bullets from enemies |
| `0x17D6818..0x17D6831` | isClient && not own world → no pickup flag / Lua event | (unreachable for treasure on a guest) |
| `0x17D85F4`, `0x17D88BB`, `0x17D8370` | role 6 → shared items go through the host (packet 0x2C) | |
| `0x18C5179` | `PlayerIns+0x78 == 2` (invader) → no kill echoes | |
| `0x18C5216/0x18C5220` → `0x18FC93D` | isClient → kill echoes × 0.5 | |
| `0x1335578..0x1335591` | session has members and own chr type not 0/8 → boss clear bonus × 0.5 (`0x4925B1C` = 0.5) | |
| `0x13849A0` (BlockClear2) | branches on `PlayerIns+0x78`: 0 host, 8 own-world grey, 1 white phantom, 2/0xC invaders (leave) | type 1: bonus paid, SpEffect 4680, flag 6006 cleared, Vermin lots |

## 4. What a guest gets in vanilla (summary)

| Source | Host | White phantom (guest) |
|---|---|---|
| Enemy kill echoes | 100 % | 50 % |
| Enemy item drops | full lot | Blood Vials / Quicksilver Bullets only |
| World treasure | yes | not spawned |
| Boss kill: echoes from the boss's NpcParam | 100 % | 50 % |
| Boss kill: GameAreaParam clear bonus (`bonusSoul_*`, for example Gascoigne 2410800 → 4000) | 100 % | 50 % |
| Boss kill: Insight | (host's own boss scripts) | +1 (SpEffect 4680) |
| Boss kill: item lots (keys, badges, chalices, Madman's Knowledge, cords) | yes | none |
| NPC gifts (EMEVD) | yes | none |
| Covenant/Vermin lots from BlockClear2 / PK texts | per rules | yes (hostOnly 0) |
| Flags of anything above | own save | overlay only (lost) |

## 5. Echoes, Insight, levels (Q4)

* Kill echoes: `echoes = soul × (isClient ? 0.5 : 1.0) × (npcFlag ? 1.2 : 1.0) × Π SpEffect soul rates`,
  rounded (constants `0x4927EBC = 1.0`, `0x4927EC0 = 0.5`, `0x4927EC4 = 1.2`) [static].
* Boss bonus: `0x1335400` reads the first u32 of the area row (`0x1F1B4C0`, GameAreaParam `bonusSoul_single`
  [inferred field]), adds the boss's NpcParam soul when the boss has the bonus bit, halves it for phantoms,
  applies SpEffect soul rates, stores it in `PlayerGameData+0xF8`; `BlockClear2` pays it via `AddSoul`
  (`0x13850A0` own world, `0x138525D` phantom).
* Insight: phantom +1 at `0x138528E mov esi,0x1248` / `0x13852B1 call [vtbl+0x3F0]` (SpEffect 4680:
  `heroPointDamage = -1`, `stateInfo = 119`). Bells: Beckoning costs the host 1 (EquipParamGoods 200 +0x38).
* Levels: only at the Doll in one's own Dream; a guest levels between sessions (B1 sends guests to their own
  world on every travel). Nothing for C3 to do.
* Optional parity patches (all byte-verified, only for a "guests earn like the host" party option):

| Patch | Address | Original | Replacement | Effect |
|---|---|---|---|---|
| Full kill echoes for clients | `0x18FC93F` | `75 06` (jne → 0.5) | `90 90` | multiplier always 1.0 |
| Full boss clear bonus for phantoms | `0x1335582` | `74 12` (je skip-halving) | `EB 12` | never halves |
| Full enemy drops for clients | `0x17DD191` | `08 C1` (or cl,al) | `30 C9` (xor cl,cl) | isClient = 0 for the guest-drop filter only (the byte at `[rbp-0x208]` has one reader, `0x17DD75D`) |

## 6. C3 design: host-authoritative lot replay

Goal: every member's own save receives every item lot the host's world hands out (key items, boss drops,
treasure the host picks up, NPC gifts), exactly once, even across crashes and rejoins.

### 6.1 What triggers a replay (host side)

1. **Flagged lots via the C2 flag diff** (covers treasure, keys, chalices, cords, NPC gifts, and ESD gifts
   that were not analysed): every host flag that flips 0→1 in category `item_lot_picked`
   (party_flags.inc; T5 `5AABZnnn` and global `5000xxxx`) is mapped to its lot:
   `lot = flag - 50,000,000` when `ItemLotParam[lot].getItemFlagId == flag` (all treasure), otherwise the
   generated reverse table (`flag → [lots]`; for the 50 shared flags take the lot the hook in 2 captured).
   **C2 must not sync these flags by itself** (syncing without the item loses it, event_flags.md 9); C3 sets
   them by giving.
2. **Hook the entry of `0x17DDC50`** on the host (cheap: all EMEVD/Lua awards): record `(lot, hostOnly)`.
   Broadcast only `hostOnly == 1` lots (clients already get the `0` ones). This is the only source for
   flagless lots (`getItemFlagId == -1`): Oedon Tomb Key 31000, Gold Pendant 50000001, Old Hunter Badge
   15000, Spark/Sword Hunter Badges 50800000/50000010, Mensis Cage 21000, Madman's Knowledge 75002405,
   25700005, 3401802, 3401852, Bold Hunter's Marks 15005/50800005, Bloodshot Eyeballs 21002950, Yellow
   Backbone 50700000, Kin Coldblood 51001900, 25700000/75002400 (item names missing from the FMG), 22502610,
   Tear Stone 14000 (and 9040 NPC slots with flag -1).
3. Deny list (never replay; per-player or repeatable): lots whose flag is in 6000-8999 (Messenger gifts
   10000/10010; 10050 the Old Hunter Bell is replayed since 0.7.3, 10.8, DLC/costume 6670-6678; 10040 the Eye of a Blood-drunk Hunter is replayed since
   2026-10-14, quest_audit.md F3), covenant gems `common 9440`, rune/gem use `9500`,
   repeatables `common 9100` / `13501940` (何度でも), DLC messenger hats `12105064`, PvP/covenant lots
   5500-5950, 16581, 5020, chalice-dungeon lots (area 29, flag -1 boss lots 1100xxxxx).

Message: `PARTY_CMD ITEM_LOT {seq, lot, flag (-1 if none), ledgerIdx (-1 if flagged), source}`.

### 6.2 Duplicate prevention

* Flagged lots: the lot's own flag in the guest's **saved pool** is the "already given" bit. The roll
  itself skips set flags (`0x17CEAF0`), so a replay of a lot the guest already has gives nothing.
* Flagless lots: a C3 **ledger** of event flags `60009000 + idx` (T6 global zone 9). That group is in the
  saved pool (key `3 + 9×5 = 48`), not in the vanilla join snapshot, and no EMEVD/param references any id in
  60000000-60009999 [static over all EMEVD + params; ESD unchecked]. `idx` = position in a generated
  table of the flagless host-only lots in 6.1 item 2 (about 30 entries). Set the bit right after the give.
* Host state for catch-up: the host derives its own "awarded" set from its save (pickup flags + the
  completion flags of the awarding events, for example 12411800 for lot 31000). On every (re)join the host
  sends the full set; the guest applies what it lacks. Live messages are only an optimisation; resending is
  always safe.
* Optional symmetry: members' own solo pickups can be pushed to the others the same way (union policy).
* **Bug fixed 2026-10-10 (blocking "Unable to acquire Sword Hunter Badge due to inventory or storage
  max" dialog on a guest).** Root cause: for a flagless lot the guest checked only its **ledger bit**
  `60009000 + idx`. That bit exists only for C3 replays; a lot the guest's save earned by itself (solo play
  before the party, or here two test saves that started as the same copy) has the awarding event's
  completion flag set but no ledger bit, so the host's `items_full` list re-awarded it. Live evidence (run A):
  `gave ... lot 50000010 (ledger 36 = flag 60009036, full)` (goods 4114, Sword Hunter Badge, `maxNum` 1, no
  storage) although the guest's own flag 12411700 (ledger 36's event) was set; likewise lots 29000, 30000,
  31000, 50000001 (done flags 9066, 9067, 12411800, 12401800). The game then refuses the unique item with a
  modal dialog over the reward list. Fixes in `Apply` (party_items.cpp): (1) a ledger lot whose
  `ItemLedgerDoneFlag` is set in the guest's own save only gets its ledger bit; (2) before any award the lot's
  slots are read from the loaded ItemLotParam (`GameParamRow`, lotItemId +0x00, category +0x20, base point
  +0x40, num +0x8A) and every goods slot is checked against the inventory (`0x14D9E80`) and EquipParamGoods
  `maxNum` (+0x36) / `maxRepositoryNum` (+0x49): a unique item already held (`Owned`) or a count that would
  pass the max with no storage room (`Full`) skips the lot and sets its flag / ledger bit instead
  (`ClassifyLotFit`, unit-tested); (3) popups: 8 s between two real awards (skips take no time) and none
  within 15 s of an HP loss (ChrIns +0x3B0 -> +0x20 -> +0xF8). The own-world gate (load mode 0, own-world
  byte 1, session idle or hosting) was re-checked: no award while summoned. Not done: suppressing the
  reward popup itself (`0x17D89F0` opens it; no flag found yet) and a menu-open check.
  Live (2026-10-10): run C1 skipped the five lots above through rule (1) (`this save's own event flag
  12411700 says it was awarded here`, no award, no dialog); run C3's `BB_PARTY_ITEMS_TEST=10010,50000010`
  probe read the ItemLotParam / EquipParamGoods rows and the inventory on both instances: `goods 200: held
  1 + 1 > max 1, no storage` and `goods 4114: held 1 + 1 > max 1` -> `Owned` (the param file type is
  `ITEMLOT_PARAM_ST`, not `ITEM_LOT_PARAM_ST`).
  **Rule (1) replaced (livesync, 2026-10-10):** checking `ItemLedgerDoneFlag` at apply time also skips
  every boss reward the guest never got, because C2 writes the host's boss / event flags into the guest's
  save first (join snapshot at the first steady world, 30 frames, before the 90-frame item gate; live
  flags while summoned): a boss killed together sets 12401800 in the guest's save, then lot 50000001 was
  skipped. Now the save's own history is recorded once as ledger bits before C2 touches it (ledger
  seeding, below) and own-world ledger awards set their bit in the award hook; rules (2) and (3) stay.

### 6.3 Applying on a guest (main thread)

Run from the A5 tick (`SprjFlipper::Update 0x2034770`). Apply only when **in the own world, load mode 0**:
`*(u32*)(EventFlagMan+0x80) == 0`, `GSM+0x1592 == 1`, `SprjSessionManager+0x124` is 0 or 3 (not 4/6/7),
`WorldChrMan(0x553E878)+0x60 != 0`, `GameDataMan+8 != 0`, `*(u64*)0x553D6E0 != 0`, NowLoading byte
`0x556286B == 0`, not inside the save writer. Queue everything that arrives while summoned (mode 1) and apply
it at the next own-world load (B1 brings guests home on every travel, death and boss clear). Reason: in mode 1
the roll reads the host-world overlay, where the host's flag is already set, so the award would be empty, and
any flag it writes is discarded.

```c
// SysV (guest code is sysv_abi), main thread only
typedef void (*AwardLotFn)(void *mapItemMan, int32_t lot, uint8_t hostOnly);
#define AwardLot ((AwardLotFn)(0x800000000ull + 0x17DDC50))
void *mim = *(void **)(0x800000000ull + 0x553D6E0);

if (flag >= 0 ? IsEventFlag(flag) : ledger_bit(idx)) drop();      // 0x13CFC00 / 0x13CFD80
AwardLot(mim, lot, 0);           // rolls with the guest's own flags, gives, shows the popup,
                                 // sets the lot flag in the live tree (= saved pool in mode 0)
if (flag < 0) SetEventFlag(*(void**)(0x800000000ull+0x553B100), 60009000 + idx, 1);   // 0x13CFCC0
```
* `hostOnly = 0` because the caller does the gating; in mode 0 the gate would pass anyway.
* One lot per tick (each award opens a popup), and the next one only after the popup queue drains.
* Inventory full: `0x17D89F0` sends the overflow to storage, then drops it on the ground (`0x17DA960`); the
  flag is set either way (same as vanilla).
* RNG: the roll consumes the game RNG (`GSM+0xA80`) even for single-item lots. That is harmless on a guest in
  its own world.
* Superseded by section 10 (direct give + own pool while summoned). Originally: not recommended: giving while summoned (calling `0x17D89F0` with a hand-built list plus writing the pool bit).
  It needs gaitem handles for weapons/armour (`0x1A876D0`, `0x1A88130`) and racing the overlay.

### 6.4 Interplay with C2 and C4

* Boss-defeated flags synced by C2 make the guest's own boss event never award (its completion flag is set)
  and are why flagless boss lots must come from C3. Order on a guest: C3 apply, then the C2 boss flags (or the
  same tick).
* NPC quests (C2 `npc_quest`, whole blocks later): their gifts arrive through C3 as soon as the host gets them,
  even if the guest's NPC block is not synced yet.
* Endings and NG+ (C4): NG+ variant lots (34000/34030, 43800/43802…) depend on the cycle; the host's lot id is
  used as is.

### 6.5 Peer validation (bbport security pass)

A guest takes a host row (`FilterPeerItems`, `ItemGrantFromPeerAllowed`) only when it is a host source
(award / flag / full), the lot is not denied (6.1 item 3, Chalice), and the tables agree: the lot's
`ITEM_LOT_FLAG` is the row's flag (or the flag's lot is the row's lot), or a flagless row names the ledger
index of its own lot. **A lot outside the tables is refused**, whatever flag comes with it. Earlier builds
took such a "captured" lot once; that is no longer needed and was a hole:

* an honest host never sends one: `ClassifyAward` takes the flag from `ITEM_LOT_FLAG(lot)` (a flagless lot
  outside the ledger is not offered at all), and `ClassifyFlag` maps flags through the same table or the
  host's captures, which only hold table lots. `party_items.inc` lists *every* flagged ItemLotParam row
  outside the Chalice Dungeons, so a flagged row the game can award is always in it;
* the pair is written into the guest's own save: an arbitrary lot award plus an arbitrary event flag set to
  ON (the "done" flag), i.e. a host could flip any story / boss / NPC flag of a guest.

Host and guests run the same game data (1.09) and the same generated tables; a mismatched build loses only
the rows its tables do not know (logged as rejected), never more.

## 7. Patches considered and rejected for C3

| Site | Original | Would do | Why not |
|---|---|---|---|
| `0x17DDD3E` | `0F 84 8F 07 00 00` | `2003[4]` gives on clients | client EMEVD already skips the instruction (`1003[105]`), and the flag lands in the overlay, so items duplicate |
| `0x17DC3D6` / `0x17DC3E3` | `0F 84 A5 0C 00 00` / `0F 85 98 0C 00 00` | spawns treasure on guests | pickups on a guest set no flag (`0x17D6831`), giving infinite loot and a desync with the host's world |

## 8. Probes

* **I1** log `0x17DDC50(lot, hostOnly)` and `0x17D6780` (handle, kind, `+0x28`) on host and guest during a boss
  kill (Gascoigne) and a treasure pickup; confirm the guest gets no call for 31000 and gets 4680 + bonus.
* **I2** guest replay: in its own world call `0x17DDC50(mim, 2400450, 0)` (Hunter Chief Emblem) twice and
  check one item, flag 52400450 set, popup shown.
* **I3** ledger: set 60009000, save, reload, read it back (pool block present in mode 0 and mode 1).
* **I4** echoes: compare a guest's `PlayerGameData+0x94` delta for one enemy with and without patch
  `0x18FC93F`; `+0x84` before/after a boss as a phantom.
* **I5** what flag 6006 (cleared for a phantom in BlockClear2 at `0x13852EE..0x1385339`) and the global byte
  `0x553D6D8` mean; the host-side handler of packet 0x2C.

## 9. Implementation (gpu/shim/party/party_items.{h,cpp})

* Tables: `party_items.inc`, generated by `tools/party/item_tool.py gen-inc GAME`: `ITEM_LOT_FLAG` (1133
  lots with a pickup flag, Chalice Dungeons left out) and `ITEM_LEDGER` (43 flagless host-only `2003[4]`
  lots, the list of 6.1 item 2, with the awarding event's completion flag = event id + slot, for
  example 31000 → 12411800, common 9040 slot 5 → 9045). Ledger indices are append-only (the generator
  keeps the indices of an existing file).
* Ledger range check: `item_tool.py check-ledger GAME` scans every EMEVD (decoded operands and raw bytes),
  the param bnd, the talk ESD and the Lua bnds for any u32/f32/f64 in 60009000..60009999: 0 hits.
* Host: hook at the entry of `0x17DDC50`; `hostOnly == 1` lots in the own world become grants (flagged
  lot → key = flag, ledger lot → key = 60009000 + idx); `OnHostFlagSet(flag)` (C2's
  `progress::SetItemFlagObserver`, registered by `InstallItemsPatches`) maps flags to lots, preferring
  the lot the hook captured for that flag. Every key is sent once (EVENT `items`); a member that
  (re)joins gets EVENT `items_full`, built on the main thread from the host's flags (lot flags and
  ledger done flags) once the host's own world is up.
* Guest: grants are applied one at a time (1.5 s apart) once the gate held 90 frames: world up with a
  known map, no loading screen, flag store load mode 0, `GSM+0x1592 == 1`, session role 0 or 3, game data
  and MapItemMan present. Skip if the done flag is set; else `0x17DDC50(mim, lot, 0)`, then the ledger
  flag. `0x17D89F0` is hooked only to log what our own award adds to the inventory. A skip or a mark
  opens no popup and does not hold the next grant (a rejoin's ~130-lot full list is almost all skips;
  with the 1.5 s spacing on skips too a new lot behind it waited ~200 s - live run, 2026-10-10).
* Ledger seeding (live run 2026-10-10: a guest on its first party session was given lots 29000, 30000,
  31000, 50000001, 50000010 again, which its own save had earned solo): once per save (marker flag
  60009999, the last ledger bit), at the first steady own world and **before** C2 applies any host
  flag (C2's before-apply hook, `progress::SetBeforeApplyHook`), every ledger lot whose done flag the
  save holds gets its ledger bit. Afterwards an own-world `hostOnly` award of a ledger lot (any role)
  sets its bit in the award hook. A save that took C2 host flags before seeding existed may hold host
  boss flags and so skip a ledger lot it never got (dev saves only).
* Guest "mark": a `hostOnly == 0` lot with a flag that the guest receives while summoned (its flag only
  reaches the overlay) is queued as a mark; at home its flag is set (no item) before any replay, so the
  host's later flag report does not duplicate it. The mark lives in memory: a crash before going home
  loses it (one duplicate possible) [known gap].
* Deviation from 6.1 item 3: Ludwig's and Laurence's boss lots 3401800 (flag 6674) and 3401850 (6673)
  are replayed even though their flags are in 6000-8999: a phantom never gets them (`2003[4]` skipped on
  clients) and the flag is the guest's own, so the replay is exact. All other lots with personal flags
  stay denied.
* Parity patches of section 5: written at start in party mode (`BB_PARTY` set) unless
  `BB_PARTY_FULL_REWARDS=0`, each byte-verified.
* Debug: `BB_PARTY_ITEMS_TEST=<lot>[,...]` applies lots once in the own world in any role (it also turns
  on the party layer's tick). Single-instance runs (Continue via `BB_PAD_FILE`, Hunter's Dream, save
  level 37): `2300090` gave goods 1000 (Blood Vial) x1, flag 52300090 set; ledger lot `24020` gave goods
  1110 x2, flag 60009008 set (log of the `0x17D89F0` list). After a restart the first lot of a run is
  skipped ("already given"), ledger or treasure alike; the second one, awarded 1.5 s later, is given again
  together with its vanilla treasure flag: the game saved once after the first item and the test run was
  killed (BB_TIMEOUT) before the next save, so item and flag share the save. The ledger bit is set before
  the award call (cleared again when the award adds nothing) so it is in the same save as the item.
  With `BB_PARTY` set the three parity patches log `applied`.
* Open: whether the 9040-slot completion flags (9045...) are set exactly when the gift is given
  [inferred from the event shape]; NG+ handling of the ledger flags (T6 global zone 9) [unchecked]; the
  `items_full` list also carries `hostOnly == 0` flagged lots, which a guest that was present in an earlier
  session (mark lost) gets a second time.

## 10. Awarding while summoned (seamless play, 2026-10-10/11)

With B2 / fast join a guest stays summoned (role 6, flag store load mode 1) nearly all the time,
so the own-world replay of 6.3 never ran. Now `ItemsTick` has two gates (`ItemApplyModeNow`):
**Home** (6.3, unchanged) and **Summoned** (`world up, no load, load mode 1, own-world byte 0,
role 6, game data`). In Summoned mode a grant is applied without `0x17DDC50` (whose roll reads
the overlay, where the host's flag is set) and without any overlay write:

* "already given": the done flag (lot flag or ledger bit) read from the **own pool**
  (`progress::ReadFlagOwnSave`), never the overlay.
* items: `PlanSummonedLot` over the lot's ItemLotParam chain (`GameParamRow`, rows lot, lot+1...
  as `0x17CEAF0` walks them; per-row flags already in the own pool are skipped). A row with one
  weighted slot (base or cumulate point) is deterministic (1169 of 1176 table lots). The host
  sends what its roll **really** gave: the award hook now **replaces** `0x17DDC50`
  (`ReplacePrologue`), so `GiveListEntry` captures the host's `0x17D89F0` list during the call
  (`[category, id, count]`, EVENT "items" row element 6). Random lots reported by flag or in
  the full list are rolled **once on the host** (`RollLotChain`) so all guests get the same.
  A guest uses the host's list only when every item is a weighted slot of a different chain row
  with a count <= lotItemNum (security: a host cannot hand out other items); else the rows decide.
* give: `GiveItemDirect 0x131CB70(0, category << 28, id, count)` (weapon, armor, rune, goods).
  **Blood gems** (category 8): the gem the host's game generated is recreated on the guest
  (10.1). Only a gem lot without the host's gem data (a random gem lot reported by flag, the
  rejoin full list) still waits for the own world (`GuestItems::Defer`, logged "waits for the
  own world"; the rest of the queue goes on).
* the native **item-get dialog**: `0x16D8850(*0x553B1C0, &inventory gaitem handle, type bits, id,
  count, 0)` - the entry the lot award's give list queues at `0x17D9348` (plus sound 300); the
  handle is the inventory entry's `+0`. Our overlay toast stays only as a fallback when that
  call cannot be made.
* flags: the done flag plus each giving row's flag into the own pool (`WriteFlagOwnSave`,
  C2's mode-1 pool write), written **before** the give.
* the fit rule (unique / full -> flag only) and the popup rules (8 s spacing, no HP loss for
  15 s) are the same as at home. Marks (hostOnly 0 lots received while summoned) are applied at
  once into the own pool (no more "lost on a crash before going home").
* a save request after each give (see below) so items and flags reach the save together.

Save request: the Lua binding **SaveRequest `0x1336DA0`** (registered at `0x1342F89`: if GSM
`+0x1578` "save allowed" then `+0x1548 = 1`, `+0x1547`, `+0x1549 = 1`) asks the game's save task
to write; `GameSaveTick` runs pending requests (C2 mode-1 applies, C3 gives, C4 story writes,
the late-joiner catch-up) at most once per 15 s. Without it the runs below lost everything at
exit (the game had not saved since the join).

Fixtures: `BB_PARTY_ITEMS_HOST_TEST=<lot>,...` (host: awards through the game's `0x17DDC50`,
hostOnly 1, 20 s after a cooperator appeared, 10 s apart), `BB_PARTY_ITEMS_HOST_TEST_FLAGS=<id>,...`
(host: SetEventFlag with the first award), `BB_PARTY_PROGRESS_TEST_READ=<id>,...` (each steady
load: own save / visible values).

Live (2 loopback instances, guest summoned in the host's Dream the whole time):
* host awards 2700120 (weapon 15000000), 2700150 (armor set, chain), 2700110 (goods 1210 x8),
  2700180 (blood gem), 22502610 (random ledger lot): guest `gave ... while summoned (host's item
  list): weapon 15000000 x1 (held 0 -> 1)`, `armor 290000/291000/112000/293000`, `goods 1210 x8
  (held 0 -> 8)`, `goods 1510 x1` (the host's roll), all "own-save flag(s) ... set (pool, load
  mode 1), overlay untouched"; 2700180 "not while summoned (blood gem...); waits for the own world".
* restart (run 4): the guest's first own-world load reads `52700120=1 52700150=1 52700110=1
  60009032=1 12700110=1 12707800=1` (the last two are the host's C2 flags, written while summoned)
  and the full list skips `lot 2700120 ... already given (own-save flag set; 0:15000000 held 1)`,
  `2700150 ... held 1` x4, `2700110 ... 4:1210 held 8` - nothing given twice. New lots in the same
  run: `goods 1000 x2 (held 15 -> 17, item-get dialog)`, `armor 31000/32000/33000 (item-get dialog)`.

### 10.1 Blood gems while summoned (2026-10-11)

How a lot makes a gem [static]: the lot result builder `0x1A8B080` calls, for a category-8
slot, `0x1A88130(out handle, SprjGaitem, LotReq+0x18, LotReq+0x28, LotReq+0x38, slot id)`.
`LotReq+0x18/+0x28/+0x38` are the **area context** `0x17DDC50` fills from the current block
(`*0x553B148`, row `+0x20`: `+0x48` byte, `+0x4C` / `+0x50` ids, `+0x20 -> +0x38` nine id
pairs); `0x1A88130` sums the pairs' rank offsets onto the slot id, looks the GemGenParam row up
(`0x1F1D630`), rolls the effects (`0x1A8F400`, game RNG) and creates the instance
(`0x1A87E20` -> `0x1A87590(out, SprjGaitem, gem id)`: a new SprjGaitem slot, handle
`0xC0800000 | index`, instance `+8` handle, `+0xC` `0x80000000 | gem id`). `0x1A87E20` then
writes the instance data: `+0x18` flags (two bits of the GemGenParam row `+0x8C`), `+0x1C` rank,
`+0x20..+0x34` six GemeffectParam ids (-1 = empty). The handle goes through the give list
`0x17D89F0`, which adds it to the inventory (`0x14CDA70`), queues the item-get dialog entry
`0x16D8850` with that handle and plays sound 300. Instances: `*(0x553E990) + 8 + index * 8`;
references `+0x8008/+0x800C + index * 8` (`0x1A8BAE0` +1, `0x1A8BC00` -1, frees at 0).

So the guest gets **the host's gem**, not a new roll:
* host: `GiveListEntry` (the `0x17D89F0` hook) reads each category-8 entry's instance
  (`ReadGem`: handle check, id, `+0x18..+0x34`) into the grant's item (`GrantItem::gem`). Awards
  (`0x17DDC50`, captured during the call) and pickups (`0x17D8570 -> 0x17D89F0`, host in its own
  world: kept for 120 s and attached when the treasure's flag reaches `OnHostFlagSet`, if it
  fits the lot's rows) both carry it. A random gem lot is never rolled by the host (no instance
  data); EVENT `items` element `[8, id, 1, [flags, rank, e1..e6]]`.
* guest (`PlanSummonedLot`): a gem matches a weighted category-8 slot of an unused row with
  `slot id <= gem id <= slot id + 99` (`kMaxGemIdOffset`; 1.09 steps by 1..3), count 1, gem data
  plausible (flags 0..3, rank 0..1000, effects -1 or 0..99,999,999; `ItemsFromJson` refuses
  anything else). At give time (`GiveGem`): the GemGenParam row of the id and the GemeffectParam
  row of every effect must exist (`GameParamRow`), the three functions are byte-checked, then
  `0x1A87590(&h, SprjGaitem, id)`, the instance data copied to `+0x18`, and the give list
  `0x17D89F0(MapItemMan, {1, h, 0x80000000 | id, 1, -1})` - inventory (or storage / ground), the
  native item-get dialog and its sound, exactly as the host's award; then our reference is
  released (`0x1A8BC00`).

Live (r1, 2 loopback instances, guest summoned in the host's Dream, `BB_PARTY_ITEMS_HOST_TEST=
2700180,2700110`): host `host award #0 lot 2700180 (flag 52700180, award) queued for the party;
items [8:102001x1 [gem 102001 (flags 2, rank 1, effects 1125000 -1 -1 -1 -1 -1)]]`; guest
`inventory add 1/1: item 102001 category 8 count 1 (gaitem 0xc0800062)` and `gave ... lot 2700180
... while summoned (host's item list): blood gem 102001 (flags 2, rank 1, effects 1125000 -1 -1
-1 -1 -1) (inventory add, item-get dialog); own-save flag(s) 52700180 set (pool, load mode 1),
overlay untouched`. Unit tests: `test_gems` (party-items-test).

### 10.3 Item spots hidden by their own event flag (playtest 2026-10-20)

Some host-only (`2003[4]`) map awards sit at an item spot whose visibility is **not** the lot's
pickup flag but the awarding event's own completion flag: Old Yharnam's altar, event 12300130
(`祭壇を調べる`: `3[24]` action button 2300000 on object 2301500, `2003[4]` lot 2300170 Ritual
Blood (1) x2, `1003[2]` ends it once 12300130 is set). Its flag is an `item_lot_picked` flag
(party_flags.inc "chest / item award"), so it is instanced (world_actions.md 4): the host's 12300130
is skipped on a guest (`guest applied 0 flags ... skipped 1`), and the guest's own copy set by its
event run stays "kept local". The guest got the lot from C3 (its own pool: 52300170), but nothing
put 12300130 into its own pool; at the next load the overlay's item bits were rebuilt from the own
pool (`GuestItemOverlaySync`), the altar was offered again, and the guest's press awarded the lot
with hostOnly 1 on a client: refused (`0x17DDD3E`), no item box, nothing given (playtest guest log,
mark 3: 19:58:37 shared, 20:00:19 the host's death respawn, 20:04:19 the empty press).

Fix: `item_tool.py gen-inc` writes `ITEM_AWARD_SPOT(lot, spot flag, note)`: the flagged lots a
map's EMEVD `2003[4]` gives whose event flag (event id + slot) is an `item_lot_picked` flag of
party_flags.inc (27 rows: the altar, chests, NPC gift spots; not area 29, not restart-on-reload
events, not per-player 6000-8999 lots; flagless ledger lots keep their ledger bit only, see the
ledger seeding in 9). A guest that is given such a lot (summoned: `ApplySummoned`, the flags go
into the own pool before the give; at home: `Apply`) also gets the spot flag, and a lot found
"already given" gets a missing spot flag too (a save from before the fix is repaired at the next
item list). The overlay follows the own pool, so the spot is gone for the guest at once and after
every load; no item is given twice (the lot's flag still decides the give). `BB_PARTY_ITEM_SPOTS=0`
turns it off. Unit test: `ItemAwardSpotFlags` (party-items-test).
Live (2 loopback instances, both saves with 12300130 / 52300170 cleared, the host awards lot
2300170 through `0x17DDC50` with 12300130 (`BB_PARTY_ITEMS_HOST_TEST(_FLAGS)`), party travel to
Old Yharnam, the host's death respawn, the guest warped to the altar and Cross): before the fix the
guest's own pool had 52300170 but not 12300130, the altar showed "Inspect" after the respawn and the
press logged `award lot 2300170 (hostOnly 1, flag 52300170) in another's world, role guest` with
nothing given (the playtest exactly); with it `gave ... own-save flag(s) 52300170,12300130 set`,
12300130 stayed 1 through both loads, no prompt at the same spot and no award (three tries).

Not a party bug (same playtest, mark 4): ~30 pickups of a Pebble (goods 1230: maxNum 20,
maxRepositoryNum 99 at EquipParamGoods +0x48) that each opened a box. The give list `0x17D89F0`
puts what fits in neither the inventory nor the storage on the ground as a kind-1 map item
(`0x17DA960`, prompt 10010103 "Pick Up Item") and shows "Unable to acquire <item> due to inventory
or storage max" (`0x13107B0`, menu texts 506000 / 506001); each try takes it, refuses it and drops
it again. Live: a summoned guest with 20 held and the storage filled to 99 reproduced the box and
the re-drop on every press; storage itself works for a summoned guest (room for 5 with 20 held).
Vanilla behaviour of a player at the item's maximum.
Side note: on a client the enemy drop and ground item inserts (`0x17D5320` / `0x17D4C40`) give
every record the bare kind as its handle (0x2 / 0x1; the host numbers them, role 6 / session slot
gates at `0x17D5642` / `0x17D4F54`), so the pickup's lookup `0x17D81A0` takes the newest record of
that kind, not the nearest one (live: two drop records on a guest, both 0x2). **Known issue, open**
(later): a guest at one corpse can loot another (newer) one; the items are right, the corpses swap.

### 10.4 Storage limit offset and the summoned give past the maximum (2026-10-21)

`FillGoodsLimits` read EquipParamGoods `maxRepositoryNum` at +0x49; the game's storage cap reads
+0x48 (`0x15813A0`: `*(short*)(row + 0x48)`; Pebble 20 / 99, Antidote 10 / 99, Blood Vial 20 / 600).
The s16 at +0x49 is 0 for nearly every goods, so `ClassifyLotFit` called a lot "Full" whenever the
inventory was at its maximum and set the lot's flag without the item, although the storage had
room (an item loss). Now `GoodsLimitsFromRow` reads +0x48 (unit test `test_goods_limits`: five
rows from the 1.09 gameparam, a Pebble at 20 held fits).
That alone would have moved the loss: the summoned direct give `GiveItemDirect 0x131CB70` only
adds to the inventory, and past `maxNum` the rest is dropped (its overflow goes through
`0x17DA830` with a count of 0). Goods that would pass the maximum are now given through the
award's own give list `0x17D89F0` (`GiveGoodsViaList`, ItemList entry `{0xB0000000 | id,
0x40000000 | id, n, -1}`): inventory first, the rest into the storage, else on the ground with the
game's box, and the game's own item-get dialog (log: `past the maximum: the game's give list,
storage room A -> B`).

### 10.2 The item-get dialog (2026-10-11)

How it works [static]: the give list `0x17D89F0` (and our `ShowItemGet`) pushes an entry with
`0x16D8850(*0x553B1C0, &handle, type bits, id, count, stored)` (8 pooled nodes at `+0x10`, the
live list at `+8`; sound 300). The ItemGetMenuMan tick `0x16D86E0` (from the in-game menu update
`0x17692F0`) sets **`*0x5562878 + 0x130 = 1`** ("dialog up") while the list is not empty and
clears the list only when the action-button check `0x1CD91F0(0, ActionButtonMan, 0xB, 0x1130,
...)` reports the confirm press (gated by player flag `WorldChrMan+0x60 -> +0x58 -> +0x38 -> +0x41`
bit 7 clear and `*0x5540050`). It is modal: the entries stay until the player confirms. The HUD
tick `0x1BFB2A0` (from `0x1BF9E10`, HUD object `rdi`) copies up to 5 entries a frame into the HUD
model (`*(HUD+0xA88) + 0x1238`, 0x48 per row, count `+0x13A8`; item name text via `0x1C5B540`),
and on the rows' 0 -> n edge with `*0x5578A38 < 1`, `menu+0x84 == 0`, `GSM+0x1544 == 0` sets
`menu+0x25C = 1` and asks for a save (GSM `+0x1547..+0x1549`). The movie itself is drawn by the
Scaleform HUD (the `FrpgMenuDlgItemGet` class names have no direct references).

Live (`BB_PARTY_ITEMGET_PROBE=1`, runs r1-r3 and r6, 2 loopback instances, guest summoned in the
host's Dream, `BB_PARTY_ITEMS_HOST_TEST=2700180,2700110`): on **both** instances, at the host's
vanilla award and at the guest's replay alike: `1 entry in the dialog list, dialog UP, confirm
check armed` -> `2 entries ..., dialog UP`; `HUD model rows 1` -> `2`; open gate `*0x5578A38 0,
menu+0x84 0, GSM+0x1544 0, menu+0x25C 1`. The guest's state is the host's, entry for entry (the
gem through the award's own give list, the other categories through `ShowItemGet`), and it stays
up (nobody presses confirm in the test). Screenshots (`tools/mp/shot.py --watch`, PrintWindow of
the background windows, 0.5 / 3 / 8 / 15 s after the give) still show **no box on either
instance**, including the host's vanilla one: every game-side condition of the dialog holds, so
what is missing is the Scaleform HUD draw of that element in these windowed background test
instances (a port / renderer matter, not party state). Not fixed here; the probe is the tool for
the next step (a foreground run with input to see whether the element ever draws in the port).
Note for shot.py: the watcher only reads log lines written while it runs (start it with the run,
`--seconds` longer than any queue wait).

Bisect (2026-10-11, coordinator's list):
* Window focus: the game cannot see it (`sceSystemServiceGetStatus` always reports foreground;
  `BB_WINDOW_BACKGROUND` only makes the SDL window non-focusable).
* Party patches (fast messages / hidden banners): ruled out - a **solo** run without `BB_PARTY`
  (patches: Skip Online/Offline Choice, Skip Intro, Intel Black Tonemap Fix only; native award via
  `BB_PARTY_ITEMS_TEST`) shows the same: list, dialog up, HUD rows, and no box (r9).
* Upscaler / UI composition: `BB_UPSCALER=none` (no UI composition) - no box either (r12).
* The empty-name movie loads at boot (`open(/app0/dvdroot_ps4/menu/.gfx) -> not found`, x3) are
  the **Skip Intro** patch: it zeroes the logo movie names `Logo_Fromsoft` / `Logo_Jstudio` /
  `Logo_SCE` (0x4999138 / 0x4999154 / 0x499916E, loaded by name through 0x1F604F0 from 0x1F6052D);
  unrelated to the item-get box.
* The box is the `GetItemList` clip of `menu/fe.gfx` (root depth, placed at stage (954, 785),
  bottom centre; sprite 240 = five rows `Item_0_0..Item_4_0`, sprite 239 = one row: background,
  RepositoryIcon, ItemIcon, ItemNum, ItemName; one frame, no animation). The HUD update
  `0x1BF7C20` binds it every frame (`GetItemList`, row template `Item_%d_0`, binder `0x1B16D10`)
  to the HUD model rows through the functor at vtable `0x5344970`, whose row method `0x1C02290`
  sets a row visible with icon, `ItemName` text and `ItemNum` when the row's count (+0x40) > 0.
  Probe (r14, solo): the ingameTop update runs ~30x/s and calls the row method for rows 0..4
  (5 per update) while the model holds the award - the game's whole data path to Scaleform is
  live, and still nothing is drawn at the bottom centre. Next: a frame capture (RenderDoc) of a
  foreground run with the list up, to see whether the row draws are issued and what they render.

**Resolved (2026-10-11): the box was always drawn; the screenshots were cropped.** A full-resolution
dump of the presented frame (`BB_FINAL_DUMP_TRIGGER`, r17, solo) shows the dialog at the bottom
centre: `Poison Knife 8` and the `OK` prompt. `tools/mp/shot.py` ran DPI-unaware: on this 200 %
display `GetClientRect` gave the logical size (416x234) of an 833x468 client while `PrintWindow`
drew physical pixels, so every shot was only the top-left quarter of the frame (the HUD gauges -
which is why nothing looked wrong - but never the bottom-centre box or the top-right Insight /
echoes counter). Fix: `shot.py` makes itself per-monitor DPI aware before enumerating windows.
Frame capture (`BB_CAPTURE_TRIGGER`, r16) agrees: after the give the UI pass gains the row
background (masked by a Scaleform stencil pass) and 4 text draws from the glyph cache.
Live (r18, 2 instances, guest summoned, fixed shot.py, 2 s after each give): host and guest show
the same dialog - `Clear Deep Sea 1` (the host's blood gem, recreated on the guest), then
`Clear Deep Sea 1` + `Poison Knife 8` with `OK`. No game or port change was needed; the
bisect notes above stay as the record of what was ruled out.

### 10.3 Key items while summoned (2026-10-20)

Why they used to wait for the own world (0.6.2 - 0.6.9): a summoned guest's key items (goodsType
1) and unique goods (maxNum 1, no storage) read 0 in its live inventory whatever its save holds
(verify_c6: the Oedon Tomb Key read 0 after every host-world load and 1 at home; the 0.6.1
regression run: Amelia's 4001 read 0 summoned, 1 at home). So (a) a give there cannot tell an
item the guest already has (the game then refuses the second copy with the blocking "Unable to
acquire ... inventory or storage max" box, seen live in 0.6.1 for lot 50000001), and (b) the
playtest guest's real save (0.6.0) did not keep a key item given while summoned (4114 "held 0 -> 1",
gone an hour later): what the summoned inventory shows is not all the save keeps.

Opt-in in 0.7.0 (`BB_PARTY_KEY_ITEMS_SUMMONED=1`; off, see 10.6 for why; on by default since 0.7.1
with the own key section in view, 10.7): a key-item grant whose done flag (lot flag / ledger
bit) is not in the guest's own save - so the save never got this lot - is given at once while
summoned (direct give, its flags into the own pool, the item-get dialog, a save request) and is
**also** owed (`party_owed_lots.txt`, per save slot) and noted as a summoned give
(`party_summoned_gives.txt`). In the next own world `SummonedGivesTick` (3 s) and `KeyCatchupTick`
(4 s) read the real held count and give it only if the summoned copy did not stay; never a second
copy (`KeyGiveAllowedHeld`). A grant whose done flag is set but whose unique items read 0
(`LedgerSkipSuspicious`) still waits for the own world (it is most likely held). `=0`: the old rule.

What the summoned view is [live, r1-r3 2026-10-20/21, `BB_PARTY_GESTURE_TEST` logs the inventory's
two sections]: the inventory (PlayerGameData `+0x328`) keeps key items in its first section (index
below `+0x24` = 64, entries at `+0x58`) and everything else in the second (`+0x48`). At home the
first section held the test save's 7 key items; summoned it held **0** (the second section was the
same 35 entries). A summoned give lands in that emptied section (`section 1 1 used`), and going home
the game puts the saved key section back without it: "goods given while summoned are missing in
the own world; given again: goods 4118 x1 (held 0 -> 1)". So the summoned copy is for the session
in the host's world, the home check is what keeps it.

Live (r3, 2 instances, guest summoned in the host's Dream, host `BB_PARTY_ITEMS_HOST_TEST=50800000`,
Spark Hunter Badge 4118, not in the test save): guest `lot 50800000 ...: key item / unique goods
4118: given now while summoned; also owed to the own world`, `goods 4118 x1 (held 0 -> 1, item-get
dialog)`, the item box "Spark Hunter Badge 1" on the guest's screen; Leave -> own world: `given
again: goods 4118 x1 (held 0 -> 1)`, then `owed lot 50800000: goods 4118 held 1 already: not given
again`. Both saves on disk afterwards (`save_decode.py`, `user\savedata_party`): 4118 x1, flag
60009038 set; the guest's save written after the rejoin (summoned again) still has 4118 x1.

### 10.5 Used-up items never come back (2026-10-21)

Report (0.6.9 playtest 2): "the host can have key items brought back after using them (a gold
blood gem)". The item is Amelia's **Gold Pendant** (goods 4001, ledger lot 50000001, done flag
12401800): goodsType 0, maxNum 1, no storage, and usable from the menu (refId 3392, use anim 29:
it becomes a blood gem). So it was `KeyGoods` / `LedgerSkipSuspicious`, and the 0.6.2 rule
"ledger bit and boss flag in the save, held 0 -> it was lost" (`KeyCatchupTick`, every own-world
load, any role) gave it back after every use. The same rule ran in `Apply` (#16, a rejoin's full
list at home) and, on master, in the owed check after a summoned give and in `SummonedGivesTick`
(held below the count right after a summoned give -> given again, for any goods).

Mechanism [live, run A 2026-10-21, master = 0.6.9 code here, default test save, which holds the
ledger seed sources but no pendant]: host, first own world: `key item catch-up: gave goods 4001 x1
(lot 50000001, ledger 35 = flag 60009035 set with boss flag 12401800 in this save; held 0 -> 1)`.

Rule now: a held count of 0 means "lost" only for KeyGoods that cannot be used up
(`KeyRegiveAllowed`: EquipParamGoods refId < 0 and goodsUseAnim 0, read into
`LotSlotState::usable` - keys, badges, workshop tools, chalices). For everything else the grant
decides, never the held count:
* `KeyCatchupNeeded` / `Apply` #16 / the summoned "checked in the own world" defer: only lots whose
  every slot is `KeyRegiveAllowed` (Gold Pendant, Tear Stone 4301, Iosefka's vial: skipped as given).
* summoned key give (10.3): only when every KeyGoods slot is `KeyRegiveAllowed`; a usable unique
  item waits for the own world as in 0.6.2 - 0.6.9 (`SummonedKeyGiveNow`).
* owed lots (`party_owed_lots.txt`, now `LOT SLOT GIVEN`; an old two-field line counts as given):
  given while summoned -> only a `KeyRegiveAllowed` item reading 0; deferred -> once, only when the
  lot's done flag is not in the save, and the give sets it (`OwedGiveAllowed`), so a rejoin's full
  list never gives it again after it was used.
* `SummonedGivesTick` re-gives (and `party_summoned_gives.txt` records) only `KeyRegiveAllowed`
  goods: the second inventory section is the same summoned and at home, so anything else held below
  the count after the give was used, dropped or sold.

* `KeyGoodsWatchTick`: a `KeyRegiveAllowed` key item whose count rises while summoned (an NPC's
  gift in the host's world: playtest 2 guest logs, Tonsil Stone 4310 `0 -> 1` from the talk, `x0`
  after the next load) is noted like a party give, so the own world gives it back if the save lost it.

Unit tests: `test_key_catchup` (party-items-test). Test aid: `host_use=GOODS:take@S`.

Live (run B, fixed build, 2 instances, `BB_PARTY_ITEMS_HOST_TEST=50800000,50000001`, host chain
`host_use=4001:take`, `host_warp` x2): host first own world `4001 x0`, no catch-up; the award test
gives the pendant (`4001 0 -> 1`), used (`held 1 -> 0`), after both warps `4001 x0`, nothing given.
Guest (summoned): `skip ... lot 50000001 ...: already given (own-save flag 60009035 set; 4:4001
held 0)` instead of the old "checked in the own world" (which the catch-up then gave).

The guest's "all key items gone after an area load" (issue 1) is the game's summoned view, not a
loss [live, run B, `save_decode.py` on the guest's `savedata_party` written every 15 s]: summoned,
`key goods held` reads 4000 / 4114 `x0` after every host-world load and `x1` again at home; every
save written during the session (Dream, Tomb of Oedon, Cathedral Ward, home) lists all 7 key items
of the test save. A key item given while summoned (4118) is in the save written before the next
load, gone from the one after it (the game rebuilds the key section from the saved one), and given
again at home (`goods given while summoned are missing in the own world; given again: goods 4118
x1`). Note: a save written while summoned holds the saved key section plus the summoned additions,
so a summoned give of a key item the save already holds would add a second stack; a player's save
(not a party test save) showed `4001 x1` twice in its key list after 0.1 h of play (the copy 0.1 h
older had one). Its build and logs are not ours, so the path that added it is not known; the
summoned give is guarded by the lot's done flag, and usable unique goods are no longer given while
summoned at all.

### 10.6 Putting a summoned guest's key items back after each load (2026-10-21, removed in 0.7.1)

Request: a summoned guest's key section is empty after every host-world load (10.3); give its key
items back silently right after the load. `SummonedKeyRestoreTick` (party_items.cpp): 0.3 / 1 / 3 s
after each load (load mode 1, client role) every known key item that is never used up
(`SummonedRestoreAllowed` = `KeyRegiveAllowed` and held 0) is given with GiveItemDirect (no box, no
sound); known = the own world's key section (`KeySectionItems`, recorded 5 s into each own world in
`user\party_key_items.txt` per save slot) plus what reached the guest while summoned, or, for a
character never seen at home, the ledger / shared-key lots whose done flag its save holds.

Live (run D, 2 instances, guest summoned in the Dream, then `host_warp=2412953`): after the warp
`summoned key items put back after the load (pass 1, 0.3 s after it, ...): 4000, 4114`, and the key
item view on the guest's screen shows the Sword Hunter Badge while summoned. **But the save gets the
keys twice**: `save_decode.py` on the guest's `savedata_party` written while summoned after the
restore lists `4000 x1` and `4114 x1` twice (the 7 saved keys, then the restored ones), and so does
the save written back home after the Leave (`+98 s`, "C3 goods given while summoned, kept at
home"). A save written while summoned holds the hidden key section plus the summoned view (also seen
in run B with 4118), so any give of a key the save already holds makes a second stack - most likely
how a player's save got the Gold Pendant twice. The in-memory check (`ItemStacks`, "DUPLICATE
STACK") did not see it: only the written save has the two stacks. So it is **off by default**
(`BB_PARTY_KEY_RESTORE=1` for tests). Doing it right needs the place the game keeps the hidden key
section while summoned (show or copy that, instead of a give): found, 10.7; `SummonedKeyRestoreTick`
and `BB_PARTY_KEY_RESTORE` are gone.

### 10.7 The two key sections, and showing the own one while summoned (H4, 0.7.1)

Where the hidden key section is [static, 1.09; live run 2026-10-21 below]. The inventory
(`EquipInventoryData`, PlayerGameData `+0x328`; vtable `0x536F320`, debug menu `0x14DBFF0`) holds an
item container at `+0x10`, built by `0x1AC5340(container, 0x800, 0x40)`. Offsets from the inventory:

| offset | what |
|---|---|
| `+0x18` / `+0x20` | normal section: 1984 entries / used count |
| `+0x24` | key section size (64) - the split every lookup uses |
| `+0x28` / `+0x30` | **the key section the character keeps** (the save's): 64 entries / count |
| `+0x38` / `+0x40` | **the summoned key section**: 64 entries / count |
| `+0x48` / `+0x50` | view: normal entries / pointer to its count |
| `+0x58` / `+0x60` | **view: key entries / pointer to its count** (`+0x28` or `+0x38`) |
| `+0x68`, `+0x78` | id hash (0x7E1 buckets) and chain, rebuilt from the views by `0x1AC5720` |
| `+0x88` | tail index |

How the summoned view is built: the in-game step's start `0x1937570` reads the game mode
(`*(0x553D6D0) + 0x84`). Mode 3 (a summoned load) calls `0x1AC5600(inventory + 0x10, 1)` at
`0x1937A97`: views = normal + **summoned** section, every summoned entry cleared (gaitem handles
released), its count 0, then `0x1AC5720` rebuilds the hash. Any other mode sets the views to normal +
**own** section inline (`+0x48 = +0x18`, `+0x58 = +0x28`) and rebuilds the hash. `0x1AC5600(c, 0)`
does the same as that inline code. So nothing is ever copied: the own section stays untouched in
`+0x28` while summoned, out of view; a give (GiveItemDirect, the give list) lands in the summoned
one.

The save writer (`0x14D8F60` -> `0x1AC5D10`) writes the three sections in turn, each as a u32 count
plus all its entries: normal (1984), own key (64), summoned key (64), then the tail index. That is
the "hidden key section plus the summoned view" of 10.6: a key put into the summoned section while
the own one holds it is two stacks in the file. `save_decode.py` now finds the two key sections
(`key section (saved)` / `(summoned)`) and `--check-dup` exits 1 on a key in both.

Live (vanilla call, 2 instances, guest summoned into the host's Dream): `inventory +0x28 =
0x100806F730, +0x30 = 7` (the test save's 7 keys, 4114 / 4312 / 4110 / 4000 / 4103 / 4011 / 4311,
read from that address), `+0x38 = 0x100806FB50, +0x40 = 0`, `+0x58 = 0x100806FB50` (the summoned
one in view), while the guest logged `key goods held: ... 4000 x0 ... 4114 x0 ... load mode 1`;
back home `4000 x1 ... 4114 x1`. The guest's save afterwards: own section 7, summoned 0.

0.7.1 (`BB_PARTY_KEY_VIEW`, on; `0` = the game's view): a call-site hook on `0x1937A97` runs the
game's `0x1AC5600(c, 1)` (the summoned section emptied as before, so an old save's leftovers there
go too) and then `0x1AC5600(c, 0)`: the own section is in view with the game's own index. A summoned
guest then reads its real held counts, and a give while summoned lands in the section the save
keeps: one stack, kept at home. Following from that, the summoned key give (10.3) is on by default
(`BB_PARTY_KEY_ITEMS_SUMMONED=0` off; a key already held counts as given, the fit rule), and
`KeyGoodsWatchTick` no longer notes a key that reached a summoned guest for the home check (it stays).
`ItemStacks` (the in-memory `DUPLICATE STACK` check) counts the key section out of view too.
Not changed: the host (never mode 3); the own-world path.
The plaza gate's `KeyItemGate` (party_worldact.cpp) inverted the emblem check for a summoned guest
with the pickup flag, because its emblem read 0; with the own section shown it reads 1, so the
inversion would have taken the "need the emblem" branch. It now inverts only while the held count
reads 0 (`KeyItemGateInvert(..., held)`). GoodsCount there runs inside the game's EMEVD dispatch
(0x17B93A0, from SprjEmkEventIns::Update) right before the game's own 3[4] handler 0x17AD240 reads
the same inventory hash the same way (0x14D9F60 / 0x14D9E80, read-only, no locks); the hash is only
rebuilt at a load (0x1AC5720 from 0x1937570).

Live (run8 2026-10-21, 2 instances): the host's emblem taken away (`goods=4011:0`), 12400160 and
Amelia's 12401800 cleared on both, the pickup flag 52400450 set in the host's world; the guest
summoned to the Cathedral Ward with its own emblem (`key goods held: ... 4011 x1 ... load mode 1`),
`key item gate: ... as in the data (summoned 1, own pickup flag 52400450 1, host world's 1, held 1)`,
at the gate's action point (2401220, 1.2 m, 180 deg - the plaza side; 0 deg is the far side, where
"Closed by a device." shows for anyone) Cross: "A special emblem opened the main gate" on **both**
screens, the guest `opened key-item gate (flag 12400160) with its own key: the host's far-side ObjAct
kick goes up; own save set`, the host `host flag 12400160 = 1 (world_state)`; the guest back home
reads `12400160=1/1` (own save / visible). Both end with the gate open.

Key Items tab while summoned (run10, 2026-10-21): the guest summoned at the host's Central Yharnam
lamp, a scripted walk `options/cross/right/right` (the pause menu opens on Inventory, Consumables tab;
Right twice = the key tab; R1 / L1 only move the item cursor, the touchpad opens the sort box) shows
"Key Items" with the guest's own Oedon Tomb Key, Hunter Chief Emblem, Eye of a Blood-drunk Hunter,
Blood Gem Workshop Tool, Saw / Sword Hunter Badge, Old Hunter Bell; the director's
`watch_text` logged `text shows: "Oedon Tomb Key"` (and the other names). `up/right/right` lands
there too. Smoke s2 (`keyitems`) runs that walk on guest 1 4 s after its arrival at m24_01
(`BB_TEST_MENU_FILE` line `m24_01 4 ...`: a map instead of title / world / joined waits for that
map, together) and needs the key name in the log.

### 10.8 Early items for every player (0.7.3)

Report: in a party at the start of the game, (1) after one player took the starting weapons from the
Dream's messengers the other could not get any, (2) only the player who talked to Iosefka got her
vial, (3) the Old Hunter Bell reached one player only.

**Starting weapons [static, live].** No item lot: the messengers 2100215 / 2100220 (right hand) and
2100216 / 2100221 (left) open ShopLineupParam rows 2000-2002 (weapons 7000000, 5000000, 22000000;
stock flags 72101000-72101002) and 2010 / 2011 (14000000 / 6000000; 72101011 / 72101010), price 0.
m21 event 12101020 (12101021 likewise): `1003[101]` to label 0 once its own flag is set (the
messengers are taken out of the back read: gone), else wait for the local chr type 0 within 6 m,
rise (12105050), wait for any of its stock flags, set 12101020. The done flags were `npc_quest`
(D8, Follow): the host's pick went into every guest's save (snapshot and live), so a guest that had
not picked yet found its messengers gone for good in its own Dream, without a weapon. The shop
stock flags were per player already (`ShopStockFlag`). Fix: `flag_tool.py` override rows
12101020-12101021 -> `never_sync` (party_flags.inc regenerated): not synced, never a world action.
A save that got the host's flag from an older build (done flag set, none of its own stock flags) is
repaired in its own world (load mode 0, session idle / hosting; `StartWeaponFlagsToClear`,
party_start.h; the director clears the done flag, logged `start: starting weapon flag N was set
without a pick of this player's own ...`); the messengers come back at the next Dream load. A
summoned guest still cannot pick in the host's Dream (the event needs chr type 0, and the host's
picks hide the messengers there): it picks in its own Dream (before joining, or after *Leave*).
With *Party starts* = after the prologue (default) every player has its weapons before any summon.

**Iosefka's Blood Vial [static, live].** Talk ESD t241313 sets the trigger 72410310 (common 9100
slot 1 awards lot 23000, goods 703, maxNum 1) in three branches, each gated by `72410301 == 0`,
goods 703 held 0 and `72410311 == 0`, then sets 72410301; m24_01 event 12410736 clears 72410301 at
the next clinic load in the own world (she gives another one when you have none). 72410301 is
`npc_quest` (shared), so after one player's talk she gave nothing to the others. A host's talk
already reached the guests (ledger 5, given while summoned with *Key items at once*); a guest's
talk gave only the guest (P2, `ItemSource::Repeat`) and moved Iosefka on for the host. Now
(`SharedRepeatGiftLot`): the guest's own repeat grant stays, and the guest also reports the lot up
(EVENT gift_up with its ledger bit 60009005 as the flag, `GuestGiftShareable`); the host's world
awards it unless its ledger bit is set or it holds one, and C3 gives it to every member once (the
talker holds its copy: "unique item held already", ledger bit set). The rejoin full list also sends
ledger lots whose ledger bit is set on the host (the vial's 9040 done flag 9046 is set by no script).

**Old Hunter Bell [static, live].** m21 event 12101026: needs 12101022 (Notebook) and 12100105,
waits for the talk flag 12101027 (`t210720`, world_state: a guest's talk in the host's Dream goes
up as a world action), then `2003[4]` lot 10050 only for chr type 0 (skipped on a client). So a
summoned guest's talk gave the HOST the bell and the guest nothing. Lot 10050 is no longer denied
(`ItemLotDenied`): the host's award and the rejoin full list (host flag 6670) give it to every
member once; 6670 is in the personal block, so each player's own flag decides.

Live (2 loopback instances, default save, `BB_PARTY_START=immediate`):
* r2: guest with its weapon picks and done flags cleared (`BB_PARTY_TEST_PROLOGUE`, own world): the
  repair line for 12101020 (set without a pick); summoned in the host's Dream (host picks set):
  `test read (load mode 1, own save / visible): 12101020=0/1 12101021=0/1`; after *Leave*, own world:
  `12101020=0/0 12101021=0/0 72101000=0/0 ...` (no stock flag of the host's either). Vial: the
  guest's `guest_flag=72410310` (what the talk writes): `gave ... lot 23000 ... goods 703 x1 (held 0
  -> 1, item-get dialog)` and `gift: lot 23000 (ledger flag 60009005) reported to the host`; host
  `gift from slot 1: lot 23000 (flag 60009005) awarded in the host's world`, `own award of ledger lot
  23000: ledger flag 60009005 set`, `host award #0 lot 23000 ... items [4:703x1]`; guest `skip ...
  lot 23000 (ledger 5 ...) while summoned: unique item held already ...; own-save flag(s) 60009005 set
  instead`. Both screens: "Iosefka's Blood Vial 1" (`evidence/r2/guestvial_3s_inst0/1.png`).
* r3: the host's test award of lot 23000 (`BB_PARTY_ITEMS_HOST_TEST`): guest `gave ... lot 23000
  (ledger 5 = flag 60009005, award) while summoned (host's item list): goods 703 x1 (held 0 -> 1,
  item-get dialog)`, "Iosefka's Blood Vial 1" on the guest's screen (`evidence/r3/vial_2s_inst1.png`).
  The rejoin full list now carries lot 10050 (flag 6670) to a guest whose 6670 was cleared:
  `lot 10050 (flag 6670, full): key item / unique goods 4312: given now while summoned`, then skipped by
  the fit rule because the test had not yet taken the guest's bell away (`held 1`): the replay path is
  open (before, the host never offered it); the item box itself was not seen live.
* r4: the host's own messengers with its picks cleared (`12101020:0`, stock flags 0) after a Dream
  reload: they rose (12105050 = 1; screenshots `evidence/r4/rise_*_inst0.png`: the messengers on the
  stairs holding the weapons), and **2.8 s later 12101020 = 1 on the host with none of its stock
  flags set**, 57 ms after the same flag on the summoned guest (whose own save has its picks): the
  guest's copy of event 12101020 ended and the event state packet 0x31 (guest_services.md 2.1)
  ended the host's copy, so the host's messengers sank before it picked - the second way one
  player's pick took another's messengers [live; mechanism inferred from the order of the two lines
  and the bath messengers' 0x31 sync]. Fix: `PerPlayerMessengerEvent` also drops the 0x31 packets of
  the gift messengers' events 12101020, 12101021, 12101022 (Notebook), 12101024 (Beckoning Bell),
  12101026 (Old Hunter Bell), 12101028 (Eye): each ends on its own player's pick / personal flag
  (the same path may have ended a host's Old Hunter Bell event with a guest's). Not re-run live (the
  run budget was spent): the next smoke in the Dream should show the host's messengers staying up
  next to a guest with picks, `Messengers: event 12101020's state from another player not applied`.
* r5 (smoke group `items_every`, phase ie, after the merge): the host's messengers rose
  (`12101020=0 12105050=1`, picks cleared) and **stayed up**: 12101020 never went to 1 on the host
  (r4: 2.8 s after the rise); the host logged `Messengers: event 12101026's state from another player
  not applied` (the guest's copy of the Old Hunter Bell event, its end dropped; no packet of 12101020
  came this time). The bell's item box was not reached: the join's full list had already skipped lot
  10050 (the guest still held its bell then) and a guest takes each flag's grant once per session, so
  the host's later award was not applied - a fixture fault, fixed in the phase (the host's 6670 is
  cleared too, so the full list has no lot 10050). The scripted weapon pick did not open the talk
  (a warp 1.5 m from messenger 2100215 showed no prompt): left out of the phase.

Unit tests: party-items-test (`test_deny`, gift_up tests), party-start-test
(`test_weapon_repair`), party-worldact-test (categories, world actions).

### 10.9 Key-item and questline treasure goes to everyone (0.7.5)

Report (a player, 2026-10-23): Gascoigne's daughter was killed by the pig in the sewer, the host took
the Red Jeweled Brooch and the Red Messenger Ribbon, the friends never got them. Host log: `host flag
52410920: map treasure, per player (guests pick up their own)`. Root cause: `InstancedTreasureFlag`
(D4) makes every host pickup of a map-treasure flag 5AABZnnn (area != 0, not captured by the award
hook) per player, except a hand list of 8 progression keys. But these spots only exist after
host-world state (the daughter's death, a brainsucker's drop), so a guest's own world never offers them.
An audit of ItemLotParam against EquipParamGoods found 21 more key / quest treasure lots per player.

Fix: `item_tool.py gen-inc` writes `ITEM_SHARED_TREASURE(lot, catchup, note)` rows (party_items.inc):
every non-Chalice map-treasure lot with a goods slot of goodsType 1 (key item, unique, maxNum 1)
plus `SHARED_QUEST_LOTS` (the daughter's 2410900 White Messenger Ribbon, 2410910 Red Messenger Ribbon,
2410920 Red Jeweled Brooch: goodsType differs). 30 lots; `SharedKeyTreasureLot` reads the table, so
`InstancedTreasureFlag`, `GuestGiftShareable` (a guest's pickup of one goes up to the host and out to
the others) and the full list on a rejoin all treat them as global. The badges (Saw / Old / Radiant
Sword / Cosmic Eye Watcher), messenger decorations, Orphanage Key, Laurence's Skull, Astral Clocktower
Key, Queenly Flesh and the like are shared too: they are unique, so sharing is harmless, and
sharing keeps the shop unlocks and quest gates equal for everyone. Duplicates: 6.2 applies as before
(the lot's flag in the guest's own pool, a unique item held = `Owned`, skipped with the flag set), so
the guest's own spot disappears after the grant; a second lot of the same goods (Old Hunter Badge
2100500 / 2110015) is skipped as held. `ItemGrantFromPeerAllowed` accepts the rows because the lots
are in ITEM_LOT_FLAG. The key catch-up (`KeyCatchupLots`: flag set, key missing in the own save ->
given again) stays on the 8 progression keys (`catchup` 1): quest items can be handed in, and are
not re-given. `BB_PARTY_SHARED_TREASURE=0` (testing): the old per-player rule. Ordinary treasure
stays per player. Test: tests/test_party_items.cpp, smoke phase `ie` (host flags 52420900 (Orphanage Key) shared,
52410140 per player).

**Lunarium Key (0.7.6, lunariumkey).** Goods 4013 / lot 3200810 / flag 53200810 is not a map treasure: m32_00 event
13200400 (MSB object 3201010 "terrace key" at (-444.9, -164.2, 392.6), the lab counter up the ladder, action button
3200010) awards it with `2003[4]` (hostOnly) after `4[3]` chr type 0 + `1000[2]`, and ends at once on 53200810
(`1003[2]`). It therefore needs the host's own-world body and shows nothing (no glow, no prompt) in a save that
holds the flag without the key. Live (smoke group `lunarium_key`, phase `lk`): party travel Dream -> lamp 3202950,
host at the object presses Cross -> `award lot 3200810`, guest `gave ... goods 4013` ("Lunarium Key" box), both hold
4013; the spot works in a party. Phase `lkc`: flag 53200810 set, no key -> `key item catch-up: gave goods 4013`.
Lot 3200810 is `catchup` 1 (the key is never used up), so a save that has the mark and lost the key gets it back.
A guest cannot take it (vanilla chr type 0 gate); the host presses. Reporter's own cause unproven (host log: it stood
about 7 m from the counter, never at it).

## 11. Gestures (2026-10-20)

How the game keeps them [static, checked on the test save]: gestures are no inventory items and no
flags of their own. `GestureGameData` (PlayerGameData `+0x5B8`, built in `0x14F1510`; its 0x78 bytes
read / written with the character by `0x14EDD10` / `0x14EDD40`) holds 30 u32 entries at `+0x10`:
`display index << 16 | param id << 1 | learned`. Param id p = gesture index + 1 indexes the static
gesture table `0x55526D0` (filled by `0x14EC980`): {goods 800 + index (the EquipParamGoods row for
the name / icon), message 301000 + index, index, motion}. The test save's entries (`userdata0000
+0x19120`) have bit 0 on for goods 800..803, 808, 809, 812, 813, 821; no goods 800..822 entry is in
its inventory. A talk script teaches a gesture with ESD command `0x41` (the item-get notice of goods
800 + i with a null handle, `0x16D8850`) and command `0x42` (index): bit 0 of that entry
(`0x203FFD0` case 0x42; `0x14EDF10` does the same and has no caller), plus its "taught" flag, an
argument of the teaching state: 6740 + i (804..820; League Oath 822 -> 6770), all in the personal
block 6000-8999. Lots 2010150 (800) and 9906160 / 9906165 (801) are the only item lots with gesture
goods; 800..803, 808, 812 and 821 are a new character's.

What went wrong in a party: the taught flag stays in the guest's own pool when summoned, the
learned bit did not reach the other members at all, and a guest's own bit learned in the host's
world could be missing at home (the user's 0.6.9 report) while the flag said "taught", so the NPC
never taught it again.

`party_gestures.{h,cpp}`:
* every member reads its learned bits every second in a steady world (own or summoned) and sends
  them (EVENT `gestures` `{"goods":[...]}`) when they change and every 60 s: a guest to the host,
  the host to everyone. The host learns a guest's new gesture at once, so its list is the party's.
* a peer's gesture this save lacks is owed (`user\party_gestures.txt`, `owed SLOT GOODS SUMMONED`,
  per save slot) and learned at once, summoned or at home: bit 0, the taught flag (own pool), the
  item-get notice as the talk shows it, a save request.
* a gesture learned while summoned (a party member's, or the guest's own NPC talk there) stays owed
  with SUMMONED 1 until the own world reads its bit; missing there, it is learned again.
* at home a taught flag without its bit (lost by an older build) is learned again.
* peer lists: only goods 800..822, at most 64 (fuzzed: party-fuzz peerev).

Live (r3, 2 instances, guest summoned in the host's Dream; `BB_PARTY_GESTURE_TEST`: the host learns
League Oath 822 at home, the guest Make Contact 820 while summoned): guest `test (summoned): gesture
820 (Make Contact) learned (item-get notice); taught flag 6760 set (own pool); load mode 1`; host
1 s later `gesture 820 (Make Contact) from a party member: owed ...`, `a party member's gesture (own
world): gesture 820 ... learned`; guest `a party member's gesture (summoned): gesture 822 (League
Oath) learned ...; load mode 1`. Item boxes "Make Contact 1 / League Oath 1" on both screens. Leave
-> the guest's own world: `learned 800 801 802 803 808 809 812 813 820 821 822`, `gesture 822 ...:
in this save (own world, learned), the one learned while summoned kept; no longer owed`; rejoined:
the same list summoned. On disk (both saves, GestureGameData found by its entry pattern): bits of
800..803, 808, 809, 812, 813, 820, 821, 822; flags 6760 and 6770 set. r2 found a stale owed entry
(SUMMONED 1, bit 0 after a fresh save) waiting forever while summoned: a missing bit is now learned
in any world. r4 (same fixtures, screenshots of the menus while summoned): the gesture menu's list
(touchpad, Triangle) ends with "Make Contact" and "League Oath" on the guest and on the host; the
guest's Inventory > Key Items tab shows "Spark Hunter Badge", held 1 / 1, while it is summoned.
Unit tests: `party-gestures-test` (ids and taught flags, JSON, entries, the owed store, the decision).

## 11. Key and unique items only (0.7.5)

Policy (user decision 2026-10-23, after the "items replaced" reports): the party shares **key items and
unique items** only. A lot is shared when one of its rows (the lot and its continuation rows lot+1, +2 ...)
gives a goods item with `goodsType` 1 or `maxNum` 1, or a weapon / armor / ring. Everything else
(consumables, repeatable stacks, blood gems) is per player: never broadcast, never in the catch-up list,
never taken from the host.

* `tools/party/item_tool.py gen-inc` writes `ITEM_UNSHARED(lot)` (929 lots: flagged lots and flagless awards
  that give nothing shareable; `ItemLotUnshared`, folded into `ItemLotDenied`). The 30 `ITEM_SHARED_TREASURE`
  lots stay shared whatever the rule says. The guest also refuses an unshared lot from an older host (`GuestItems::Offer`).
* Ledger indices never move (the guest's bit is `60009000 + idx`). A dropped lot loses its `ITEM_LEDGER` row and
  the table keeps `// retired ledger lot L (index I stays reserved)`; the generator reads those comments back, so the
  index is never reused. Retired: 15005 22000 24000-24050 26000-26030 27000-27060 29000 32000 35000 3401802 3401852
  21002950 22502610 25700000 25700005 50700000 50800005 51001900 75002400 75002405. Kept (unique goods, armor):
  14000 15000 21000 23000 (Iosefka's Blood Vial, maxNum 1) 30000 31000 39000 50000001 50000010 50800000.
* The host's catch-up list (`full`) given while summoned is **quiet**: no spacing, no popup hold, no item-get box per
  item (the 8 s spacing of `GuestItems::kSpacing` only exists so one native box is open at a time), and one notice
  "Received N items from the party" 1.5 s after the last. At home the native award still shows its box, so the
  spacing stays there.

### 11.1 Messenger shop gates

`ShopLineupParam.qwcId` is an event flag. `tools/party/shop_gates.py` (output: `docs/party/shop_gates.md`) lists every
gate with what sets it. Classes: **progress** (Gascoigne's / Henryk's / Djura's attire 5910-5914, the blood stones
5901-5904, the NPC weapon shops 12101000-12101009, the Chalice boss unlocks 5090 Bone Ash / 5091 Madman): C2 shares them
(`world_state` set-only, `npc_quest` follow);
**item** (the owner picked it up: chalice-lot pickup flags 5000-5282, "owns the Uncanny / Lost weapon" 5500-5521,
the personal block 6000-8999, the chalice root unlocks): per player. The 5500-5521 flags are the completion flags of
common event 5500+slot (param = the weapon id, `rest` 1): the event ends at once when the player's chr type is not 0,
and the completion write set all 22 in the guest's host-world overlay (the guest logs list them as "local flag
differences held back as the baseline"). `ShopGateTick` (party_items.cpp) makes the overlay show the own save's value
while summoned (like D4 does for the item_lot_picked bits).


**0.7.6 (Bone Ash set only on the host)**: 5090 / 5091 are set by map-wide event 12906824 ("dedicated flag on a boss
defeat", m29.emevd, called with the boss flag and the shop flag by m29_20_90_00 / m29_31_90_00, the Chalice layer
bosses). They lie in the global item-lot range 5000-5299 (`item_lot_picked`), which only C3's lot replay delivers,
and no ItemLotParam row carries them, so the guest never got them. `flag_tool.py gen-inc` now emits a `world_state`
row for every `ShopLineupParam.qwcId` that has no lot, is not 5500-5521 / 6000-8999 and is not a chalice runtime
flag (type 9), so a future gate is covered by regenerating. `shop_gates.py` resolves calls across EMEVD files
(`expand_all(cross_map=True)`; not used for the flag table, it would re-file the chalice layer flags), prints
"NOT SYNCED" for a `progress` gate whose category C2 does not share and `--check` makes that an exit code. The
Sinister Root Chalice gates 92905385-92905389 are engine-set per dungeon (chalice type 9 groups, never written from
outside) and stay out of scope of the flag sync; see "Sinister Root Chalice rows for a guest with its own dungeon" below.

**0.7.6 (chaliceshop): the Sinister Root Chalice rows in a host's Chalice Dungeon.** ShopLineupParam rows 5N0011-5N0015
(N = 1..4, the Bath Messenger t290300's four shops, same rows in each) sell goods 6302 / 6312 / 6502 / 6522 / 6532, each
gated by qwcId 92905385 .. 92905389 and sold out by the per-player flags 70009200 .. 70009240. The gate is the dungeon's
feature "case" flag 92905300 + DungeonFeatureParam.eventflagId: feature 85 (-> 92905385, Sinister Lower Pthumeru) comes
from the fixed sub-feature lot 998500000 of the Lower Pthumeru Chalice (6309), 86 from 6319, 87 from 6509, 88 from 6529,
89 (-> 92905389) from 6539 (HolygrailExParam fixedSf_*, DungeonSubFeatLotParam). Type-9 flags are not in the load-mode-1
overlay: they live in the member's own pool, in the group of the CURRENT chalice slot (the remap 0x13b89b0). A quick join
writes the host's dungeon's flags into the quick-joined slot's group (slot 0: `94005300 + n`), so a guest without a
dungeon of its own (current slot 0) read the host's flags and saw the row, while a guest whose own ritual dungeon is the
current slot (1..6) read ITS dungeon's flags (Hintertomb: 92905301, 310, 345, 378) and never the host's 92905385: no
Sinister Lower Pthumeru row (player report 2026-10-25, layer 4 of a host's Lower Pthumerian Labyrinth). Fix
(`party_chalice.cpp`, `CaseFlagRedirect`): while the host's dungeon is quick-joined / carried, IsEventFlag (0x13cfc00) and
GetEventFlagValue (0x13cfd80) read ids 92900000-92999999 from the quick-joined slot's group
(`94000000 + slot * 100000 + id % 100000`), nothing is written; `BB_PARTY_CHALICE_CASEFLAGS=0` off. The sold-out flags stay
per player: a guest that already bought that chalice in its own game does not see the row again (as in the original game).
Diagnostics: `BB_PARTY_SHOP_GATE_LOG=1` logs `chalice case flags (role, load mode, map): <92905300..399 set>; slot k group:
...; region flags 129072xx: ...` when the set changes. Smoke `chalice_shop`: csh (host `chalice_isz_ritual`, guest the default
save with 30000 echoes: the guest's Bath Messenger lists the Sinister Isz Root Chalice in its Chalices tab and the
guest buys it, 20000 of its own echoes, Isz 6532 x1) and csh2 (guest `chalice_hintertomb_ritual`: its flags are the host's
92905300,302,345,378,389, never its own 301,310). Not proven: the shop UI itself for a guest with a dungeon of its own (in
the test a warp to the messenger leaves its region events silent: no talk prompt; with the redirect or without), and a
real Lower Pthumeru dungeon (no fixture; the Isz dungeon uses the same mechanism, feature 89).

### 11.2 Boss item drops for the guests (0.7.6, bossdrops; user decision 2026-10-25)

Player report: a guest got no Kin Coldblood from Rom. Root cause: Rom's event 13201800 awards lot 51001900 with
`2003[4]` host-only; a guest client refuses hostOnly lots (2.3), the lot is a consumable so the 0.7.5 policy (11)
retired it from the ledger and denies it (`ITEM_UNSHARED`), and the Chalice path (G1) only covers area 29. Echoes and
Insight were paid, the item not.

Fix, same shape as the Chalice awards: `item_tool.py gen-inc` writes `ITEM_BOSS_DROP(lot, map area, note)`, 25 lots, every
`item_lot` (`2003[4]`) of an event with a boss / mid-boss defeat marker outside m29 (base game and the DLC maps m33-m36).
On the host, `AwardEntry` takes a hostOnly award of such a lot when `ClassifyAward` shares nothing (the key / unique lots
keep their flag / ledger path), the rule `party_boss_drops` is on and the world is the host's own: an
`ItemSource::BossDrop` grant (`"boss"` on the wire) whose items are what the host's roll really gave (`AwardReplace`
captures the give list), no flag, no ledger, key `kBossDropKeyBase + n` (one per kill). A guest takes it only while
summoned (load mode 1) in the map area of the lot (`BossDropAccepted`: the guests at the kill) and the host's rule is on;
elsewhere it is dropped, never carried home. `ApplySummoned` gives exactly the host's items through the usual refusal
rule (a unique item already held, no room = counts as given) and the usual item box timing (alive, no cutscene, banner
over). Key goods in the list are left out (their own rules share them). Switches: host rule `HostRule::BossDrops`
(Rules `boss_drops`, ini `party_boss_drops`, `BB_PARTY_BOSS_DROPS`).
Not covered: bosses whose drop is not a `2003[4]` in their defeat event (an enemy-param drop rolls locally on a client).
Unit: party-items-test `test_boss_drop`. Smoke groups `boss_drops` (phase bd: the party at the Moonside Lake lamp, the host awards Rom's lot 51001900 through the game's award function with hostOnly 1 (`BB_PARTY_ITEMS_HOST_TEST`, which waits for the lot's map area); the guest's item box shows Kin Coldblood, held 0 -> 1, one give) and `boss_drops_off` (bdo: `BB_PARTY_BOSS_DROPS=0` on the host, no grant, no give). Rom's real fight is not started by a test walk (HP 0 on her entity ends nothing), so her real defeat event is unproven live.

## 12. Shared rewards and pickups: the audit, the Living String, the notice (0.7.7, sharing077)

Owner's rule (kept): key items, unique items, weapons and Caryll / world-boss runes are shared both ways; ordinary
pickups (vials, materials, most treasure, armor and rings from corpses) stay per player. Every shared item must reach
every member exactly once, whoever picks it up or lands the kill, also members who join later.

| Shared | Per player |
|---|---|
| key items (goodsType 1), unique goods (maxNum 1: hunter tools, badges), weapons, Caryll runes (2xxxxx), the world bosses' reward runes, the party-earned kill reward Living String | consumables and stacks, blood gems, armor / rings found as treasure, Messenger gifts (personal flags 6000-8999), covenant gems, rune / gem use, repeatable NPC gifts (common 9100, 13501940), DLC hats, PvP texts, enemy drops |

**The audit** (`tools/party/item_tool.py gen-inc`, output `docs/party/items_audit.md` and the `ITEM_REQUIRE_SHARED` /
`ITEM_PER_PLAYER` rows of party_items.inc): every ItemLotParam row that gives a key / unique item, a weapon or a rune,
with where it is awarded (EMEVD 2003[4]/[36] events) and its verdict: `shared` (the path: map treasure replay, flag replay,
ledger, boss drop), `per-player` (the owner's reason) or `excluded` (Chalice Dungeon rows have their own grants; area 99
rows and unreferenced rows no script gives). The generator exits 1 on a `GAP` row; `party-items-test` `test_audit` asks
`ItemLotSharePath` (the shipped tables) for every row; `tests/test_item_audit.py` checks the document against the inc.
Gaps the audit found (before 0.7.7): 59 map-treasure lots (weapons 25, unique hunter tools 23, Caryll runes 5, the armor
rows of Gascoigne's set 4, a boss reward rune 1, the Living String 1) that `ITEM_SHARED_TREASURE` (key goods only until then) left per player, and the Living String lot 2605010
(ITEM_UNSHARED: goods 7200 is an ordinary material by type). The treasure rule is per pickup flag: a corpse's flag is
shared when any row that carries it gives a shared item (Gascoigne's set: armor rows + a Caryll rune), `TreasureFlagToHide`
now hides the lot's own flag (2420231 hid 52420231 before). Armor and rings from treasure stay per player.

**Living String / Brain of Mensis (NW-4ae26be).** m26 event 12600029 ("邪眼_殺害で儀式素材入手") awards lot 2605010 (goods
7200) with 2003[4] and sets 5913, when the Evil Eye (entity 2600129) has HP 0 **and** EMEVD 3[23] "damage to it by entity 10000"
holds. A guest's blows are credited to entity 10000 on the host through `DamageRecordHook` (0.7.5), but a kill that leaves no
such record (a last hit by a guest, or any kill the damage step did not record) never fires the event, and nobody got the
material. It is the only award in the game's scripts behind a 3[23] attacker-10000 condition next to an `item_lot` (scan of
every EMEVD). Fix: `KillAwardTick` (party_items.cpp, rules in party_kill_award_rules.h, rows `ITEM_KILL_AWARD`): the host,
in its own world with a party, watches the entity (HP 0 for 2.5 s) and, if the lot's flag 52605010 is still unset, awards
the lot through 0x17DDC50 (hostOnly 1) and sets 5913; the award hook shares it like the event's own award (the lot is
no longer unshared), and a rejoin gets it in the full list. The flag is the done bit: the event and the fallback never
both give it. `BB_PARTY_KILL_AWARD=0` switches the fallback off. Unproven: whether the Eye in the Wulfgar session died to a guest's
blow the damage hook missed or to something else (the log has no per-hit lines).

**Caryll runes while summoned (runenow077, CL-139f81).** A rune is a category-8 gem like a blood gem (ids 2xxxxx; `GemGenParam` rows 200000..200050 and 210000..210012). It is given to a summoned guest at once by the blood-gem path (10.1): the host's `GiveListEntry` reads the gem instance, the guest `GiveGem` recreates it into its own inventory (second section, kept in the save; the `owed` store of a lot that also holds a key item skips non-goods slots, so the rune is never given again at home). Proven live (smoke rwt, lot 17000: rune 200010 and key item 4117 given while summoned, the guest's `savedata_party` written while summoned holds the gem once, as the host's). The earlier "waits for the own world" of sharing077 came from the test, not the rule: lot 2700620 (and 2420234, 2800530, 3200510, 3200520, 29xxxxx) name gems 224021, 227000, 228001, 232000, ... that have **no GemGenParam row**, so `0x1A88130` makes no instance and the award's give list holds the empty record (`gaitem 0, item -1`); no member can ever get an item from those lots in this game data. `GiveListEntry` now treats that record as "rolled nothing" (not a read error) and the award hook sends no "shared with the party" notice for it.

**Notice** (`party_item_notice_rules.h`, `ShareNotice`): queued where a shared grant is given or queued for the party
(the host's award hook and flag report, the home / summoned apply, a guest's pickup report), never for catch-up lists
(`Full`), marks, tests, Chalice grants or lots without a share path; text `<item>: shared with the party` with the game's
own item name (runes: "Caryll rune"); at most one every 2.5 s, the same text not again within 20 s, 4 waiting at most;
shown through the party notice corner, so it follows the party notices (`party_toasts`) and the planned on-screen-messages
setting; own switch `party_item_notices` (System > Party members, launcher My Party, bbport.ini; default on).
Test aids: `BB_PARTY_GUEST_PICKUP_TEST=lot,...` (a guest "picks up" shared treasure 20 s after its world is steady: the
D4 pickup path's flag write and `ReportGuestKeyTreasure`), smoke groups `reward_guest_kill` (rw), `reward_eye` (rwe),
`reward_treasure` (rwt).
