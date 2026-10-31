# Four-player co-op (host + 3 cooperators) — RE result (Bloodborne 1.09)

Addresses are **our offsets** (raw ELF VA of `out/eboot.elf`; bbhost = ours + 0x400000; bbhost
`.hpp` `Rva{}` = ours). Confidence: **C** confirmed in disassembly/decompile, **L** likely, **G** guess.
Tools: `tools/re/decomp.py`, `tools/re/refs.py`, capstone (`scratchpad/re/bb.py`), EMEVD/EMEDF and
LuaPlus parsers (bbhost `tools/luabnd.py`, `tools/bbparam.py`) over the 1.09 dump.

## Verdict: **feasible** (cap = 3 cooperators, then at most 1 invader)

Every engine table is already sized for host + 4 remote players, and the summon selector's own
"max cooperators" field is **3** in vanilla. The only thing that holds co-op at 2 is a 4-slot
"session slot summary" with two cooperator slots (0x186fe40) and its readers. One post-hook on
that function lifts the cap to 3; the selector then stops at 3 by itself. Boss scaling needs two
small hooks plus a 1-byte patch (unused SpEffect rows 7502/7503 already exist in the params).
A 4th cooperator is **not** possible without rewriting fixed-size tables (5-slot presentation
table, 4 GameDataMan rows, player ChrSet capacity 5, Matching2 room size 5).

## 1. Every cap on cooperators

| # | Where | What | Value (vanilla) | Conf. |
|---|---|---|---|---|
| 1 | **0x186fe40** SessionSlotSummary(out, selector, debug) | Builds 4 records `{kind, status, -1}` at out+0/+0xc/+0x18/+0x24 (status at **out+4, +0x10, +0x1c, +0x28**). Init status 3 (unavailable); 0 free, 2 used. Cooperators (members counted by `0x18796c0(sel, kind)` for kinds 7, 1, 0x15/0x16/0x17) fill **rec1 (out+0x10)** then **rec0 (out+4)**; invaders (8, 0x12) fill rec3 (out+0x28) then rec2 (out+0x1c). Byte out+0x33 = total counted. | **2 coop slots** | C |
| 2 | **0x18708f0** IsKindSlotFree(summary, kind) | For coop kinds (7, 0x13, 0x15–0x1a, 0x1d–0x21): `if (out+4 != 0 && out+0x10 != 0) return 0`; invader kinds (8, 0x12, 0x1c) test out+0x1c/+0x28. Callers: 0x1872775 (sign selection state 0x1872360, bbhost sub_1c72360) and 0x1877052 (world sign display 0x1876f30). | from #1 | C |
| 3 | **0x1878d90** create summon/sign request | Calls #1 (0x187935e); if both coop slots used clears capability bits `0x80 / 0x80000 / 0xe00000` of the kind's mask (desc table +4), both invader slots → `0x100 / 0x40000`; then runs the filter #4 with `param3=1`. | from #1 | C |
| 4 | **0x1874710** SOS filter (bbhost sub_1c74710) | (a) `slots+0x14 (all members incl. host) < room max *(0x553d6d0)+0xc`; (b) `coop+inv+other (slots+8/+0xc/+0x10) + outgoing (sel+0x1c8) < 4` (literal 4); (c) desc team (0x553d760+kind*0x80) == 1: reject if `sel+0x1fc <= pending_coop + slots+8` (cmp at **0x1874c2b** `3b b2 fc 01 00 00`, `jge` 0x1874c31); team 2: `pending_inv + slots+0xc < sel+0x200`. | room max **5**, guests < **4**, max coop **3**, max inv **3** | C |
| 5 | **0x1870c50** selector ctor (`*(*(SprjEventMan 0x553b108+0x60)+0x30)`) | `0x1871259: 41 c7 86 fc 01 00 00 03 00 00 00` (max coop = 3), `0x1871264: 41 c7 86 00 02 00 00 03 00 00 00` (max inv = 3). **No other writer** anywhere (all +0x1fc/+0x200 accesses reached from SprjEventMan scanned: only reads at 0x1874c2b, 0x1876b10, 0x18c5f55/0x18c5fbc, 0x1e4119c/0x1e417e9). | 3 / 3 | C |
| 6 | **0x1876060** selector update | Starts a session only while `slots+0x18 (connecting) < room max`; at 0x1876b10 `cmp ecx,[r13+0x1fc]; jl` stops inviting once `pending+coop >= 3`; team 0xc: `room max - 1 <= pending + slots+0x10`. Room created by 0x1788ed0 → `0x1ecfcd0(..., SprjSessionManager, room+0xc)` = Matching2 maxSlot (5, bbhost log). | 3 / 5 | C |
| 7 | **0x15bc590** add presentation member | `slots+0x14 < room max`, 5 slots (`+0x1c + i*0x14`, i ≤ 4). Recount 0x15bce10: +8 = members with ChrType (ChrIns+0x78) 1, +0xc ChrType 2, +0x10 ChrType 12, +0x14 all (host = ChrType 0, counted only in +0x14). | 5 | C |
| 8 | **0x1e946e0** `summon_messenger/get` builder | `CoopOrNaturalEnemyRecruitNum = 2 - count(0x15)-count(0x16)-count(0x17)-count(7)`: **0x1e97770 `b9 02 00 00 00`** (mov ecx,2). Only told to the server; our in-process FromApi ignores it. | 2 | C |
| 9 | Lua `SetPartyRestrictNum(4)` in `eventcommon.luabnd`/`global_event.lua` `g_Initialize` (pc 565) | Binding registered at 0x1341a7c (struct 0x5548f60, vtable 0x531f530) → native **0x1334e30 = `ret`** (DS1 relic, no-op). No map script calls it. | none | C |
| 10 | EMEVD `3[29]` "client count by type" (0x14510a0 / 0x1450fe0 → 0x15bdc20 coop {ChrType-invade types 1,5,7,19} / 0x15bde10 invaders {8}) | Coop uses: `01000302`/`02000302` (**coop < 2**) in the NPC-summon sign events xx04400–xx04406 (22 events: m23, m24_00/01/02, m27, m28, m29 12906962, m32, m34 ×6, m35) gated on goods 4312 + multiplayer state; common 9181 (`>=1` → flag 9185); 9220 / xx04700 (`>=1`, Insight/maiden). Invader uses: maidens xx04710/xx04720 (bbhost five-players rewrite). | NPC signs hidden at 2 coop | C |
| 11 | EMEVD `1003[109]` "label jump by co-op client count" (handler 0x17bedd0 case 0x6d, count call **0x17bf187 `e8 94 ea df ff`** → 0x15bdc20) | 28 boss events (xx04802/xx04702/…): `01000000` (==0 → label 1, no doping), `02000100` (==1 → 7500), `03000200` (==2 → 7501). **3 cooperators fall through to label 1 = no HP scaling.** | 0/1/2 only | C |
| 12 | Native Lua_MultiDoping **0x138bc70** (bbhost: compiled callback; bytecode twin in global_event.lua) | `0x138bcbf: 83 f8 02 75 32` (==2 → `mov esi,0x1d4d` 0x138bcef = 7501), `0x138bcf6: 83 f8 01 75 54` (==1 → 7500 at 0x138bd22); 3+ → nothing. Used by EMEVD `2009[4]` (m29 chalice, 6 sites) and Lua. | 0/1/2 only | C |

Other limits that are **not** cooperator caps but bound the total: GameDataMan 4 remote rows
(`*(GameDataMan+0x10) + slot*0x6a0`, alloc 0x1a90), presentation table 5 slots, tracked handles 4
(slots+0x80..0x8c, 0x15bcd30), player ChrSet capacity 5 (WorldChrMan, bbhost), Matching2 room 5,
"guests < 4" in the filter. Host + 3 coop + 1 invader = 5 = every table full.

## 2. What a 3rd cooperator uses

- **Presentation slot**: next free of the 5 (`flow 0x5556678 → +0x16f8`, `+0x1c + i*0x14`:
  handle, member type 0 Local/1 Net/2 AI, lifecycle state, init/end flags); host is one of them.
- **GameDataMan remote row**: one of 4 (`slot*0x6a0`, occupancy bytes at `*(GameDataMan+0x18)`),
  STEP_Create 0x1e50090 also checks `slots+0x14 < room max`.
- **PlayerIns**: player ChrSet index 1..4; `PlayerIns+0x3b8` player number (0x18f1700),
  `+0x3c0` game data row, `+0x3d8` peer ObjectRef. Handles resolve through WorldChrMan+0x850.
- **ChrType** (ChrIns+0x78) **1** (white phantom), **team** (ChrIns+0x88) from the session
  descriptor table 0x553d750 (0x22 × 0x80; +0x10 category 1 = coop, 2 = invader, 0xc = other;
  +0x4 capability mask; +0x15 invade type), invade type (vfunc +0x558) 7 = NormalCoop (1/5/19
  other friendly kinds). Same descriptor as the 1st/2nd cooperator.
- **Phantom look**: set in 0x15bc590 for ChrType 1: `phantom id 0x1b27` (6951), or `0x1b28` when
  vfunc +0x598 returns 3 — identical for every cooperator, so no new colour is needed.
- **HP rule**: SpEffect 9006/9026 (maxHpRate 0.7) as for any phantom; SpEffect 9200 (0x23f0) level
  sync in 0x17e46e0 scans all 4 rows + 0x28 session records.
- **Network**: Matching2 member id (room of 5), vport P2P framing per member — PartyLink already
  relays N guests. Name plates/"summoned" messages come from the descriptor message ids
  (+0x28/+0x2c/+0x34) per member — nothing per-slot-index found. Lua `GetWhiteGhostCount`
  (0x1337970) returns slots+8 (will report 3).

## 3. What bbhost changed for "five players"

Not more cooperators: **host + 2 cooperators + 2 invaders** in the late areas. `src/engine/
five_players.cpp` + `maiden_events.cpp` rewrite the dump's EMEVD (DCX→EMEVD→DCX, SHA-256 in/out
checked, served from an overlay root `<data>/bbhost/invasion-assets`): in the Chime Maiden events
xx04710 (ring) / xx04720 (stop) of m26, m33, m34, m35, m36 the `3[29]` argument
`[group][client type 1 = invader][cmp][n]` goes `01 01 00 00` (==0) → `01 01 03 02` (<2) and
`ff 01 04 01` (>=1) → `ff 01 04 02` (>=2), plus two entity/flag typo fixes (m34 3400701→3400791,
m35 12604712→13504712). Every peer needs it. No code patch; the selector's invader max is already
3 and the chalice m29 12907400 already uses `<2`. Room maxSlot stays 5.

## 4. Enemy scaling

- **Normal enemies**: none for co-op (0x17e46e0 only multiplies SpEffect maxHpRate, NG+ table
  0x4731ed0 by ClearCount, and the 9200 level-sync).
- **Bosses** (28 EMEVD events, `1003[109]` → `2004[8]` SetSpEffect + `2004[53]` HP refresh) and
  chalice/Lua bodies (`2009[4]` / `Lua_MultiDoping`, refresh key 4070 `ForceUpdate`):

| SpEffect | maxHpRate | status resist rate | superArmor | used |
|---|---|---|---|---|
| 7500 | 1.5 | 1.5 | +30 | 1 cooperator |
| 7501 | 2.0 | 2.0 | +60 | 2 cooperators |
| **7502** | **2.5** | 2.5 | +90 | unused, ready for 3 |
| 7503 | 3.0 | 3.0 | +120 | unused |

Extension: count 3 → 7502 (see hooks H2–H4).

## 5. Patch / hook list for "max cooperators = 3" (switch `BB_PARTY_MAX_COOP=3`, ini `party.max_coop`)

All with `coop::` helpers (coop_hooks.h), installed in bbgpu_patch_image only when the switch is
3; every peer must run the same set (add it to the PartyLink patch hash).

| Id | Site | Verify bytes | Change |
|---|---|---|---|
| **H1** (required) | 0x186fe40 SessionSlotSummary | `55 48 89 e5 41 57 41 56 41 55 41 54 53` (13, PIC) | `ReplacePrologue`: call original(out, sel, dbg); then if `sel && out+4 == 2` and `Σ 0x18796c0(sel,k), k∈{7,1,0x15,0x16,0x17} < 3` → `*(int*)(out+4) = 0`. Leaves status 3 (blocked area / client / not host) alone. Fixes #2, #3 and every other reader (0x130b960, 0x157f200, 0x15800f0, 0x17b21a0, 0x1872360, 0x1876f30, 0x1879b80). The filter (#4c, max 3) then stops a 4th. |
| H2 (scaling) | 0x17bf187 (1003[109] count) | `e8 94 ea df ff` (call 0x15bdc20) | `HookCallSite` → `min(0x15bdc20(slots), 2)`, so 3 cooperators take the 7501 branch. |
| H3 (scaling) | 0x138bcc2 (native MultiDoping) | `83 f8 02 75 32` at 0x138bcbf | `75` → `7c` (jne → jl): count ≥ 2 → 7501 branch. |
| H4 (scaling) | 0x18c6db0 ApplySpEffect wrapper (vfunc +0x3f0 for chr; MultiDoping calls `[r10+0x3f0]` at 0x138bd48 — C; 2004[8] (0x17ba6a0 case 8 → LAB_017bcac8) goes through the same vfunc — L, check before relying on it) | `55 48 89 e5 41 57 41 56 41 55 41 54 53` | asm stub: `cmp esi,7501; jne orig;` save regs + xmm0–4 (stack arg untouched), `call coop_count_ge3` (0x15bdc20 ≥ 3), restore, `mov esi,7502`, `jmp orig`. Optional: without H4, 3 coop get 2.0×. |
| P5 (optional) | 0x1e97770 | `b9 02 00 00 00` | `b9 03 00 00 00` (RecruitNum; cosmetic for serverless). |
| E6 (optional) | EMEVD overlay (bbhost five-players method) | `3[29]` `01000302`/`02000302` in the 22 NPC-sign events | `…0303` (coop < 3) so NPC summon signs still show with 2 players. |
| — | 0x1871259 / 0x1871264 | `41 c7 86 fc 01 00 00 03 00 00 00` / `…00 02 00 00 03…` | **no change** (already 3). For a vanilla-2 build with H1 on, write `02` here instead of hooking. |
| — | 0x1874c2b, room max, "<4" | as above | no change. |

Byte verification above is from the 1.09 eboot (capstone over `out/eboot.elf`); `Matches()` must
guard each write.

## 6. Risks

- **Desync**: boss HP scaling and EMEVD branches run on every peer; a guest counts itself (state 6
  path of 0x15bdc20) so all peers see 3 — only if all run H2–H4. Mixed builds → different boss HP.
  Enforce via the PartyLink HELLO patch hash. Unknown: any EMEVD that keys on "exactly 2
  cooperators" beyond the scanned 3[29]/1003[9,10,14,15,16,109] set (scan found none).
- **Invasions**: with 3 cooperators only 1 invader fits (room 5, "<4 guests", ChrSet 5). bbhost's
  five-player maiden rewrite becomes "up to 2 invaders while free slots allow" — fine, the filter
  refuses the 5th guest. Hunter of Hunters/Bounty (kinds 0x12/0x15–0x17) share the coop slots
  for 0x15–0x17: H1's count includes them.
- **HUD / name plates**: per-ChrIns floating plates; no fixed 2-entry party HUD found, but untested
  with 3 (bbhost notes plate positioning at 720p). Join/leave text uses descriptor messages.
- **Lock-on / targeting**: phantoms are not lock-on targets of each other; enemy AI target
  selection over more players is untested (L: generic).
- **Boss arenas / fog**: fog-entry and boss-start events are per-player and count-agnostic in the
  scanned EMEVD; Lua `Failed_BossAreaMission` paths unchanged. The 2.0× vs 2.5× choice is balance.
- **NPC summons** (2003[51], ChrType 1) take a coop slot: with 2 players + NPC, a 3rd player can
  still join (cap 3 counts NPCs).
- **Performance / bandwidth**: 5-peer full mesh already exists in chalice dungeons (2 invaders).
- **Lua bytecode** `Lua_MultiDoping` in global_event.lua has the same ==1/==2 branches; if the
  engine ever runs the bytecode instead of 0x138bc70 (L: it runs the native one), H3 does not
  cover it — H4 still turns 7501 into 7502 but 3 coop would hit "No Doping".

## 7. Implementation (gpu/shim/party/party_fourp.{h,cpp})

`coop::fourp::FourpInit()` (PartyInit, after HooksInit and SeamlessRulesInit, before the game
runs) and `FourpTick()` (CoopTick). Installed whenever `BB_PARTY` is set and `BB_PARTY_FOURP` is
not `0`; every site is compared with the 1.09 bytes first and logged (`Party 4p: ...`,
`Coop hooks: 4p ...`).

| Id | How | Gate at run time |
|---|---|---|
| H1 | `ReplacePrologue(0x186fe40, 55 48 89 e5 41 57 41 56 41 55 41 54 53)`; after the original, `out+4 == 2` and Σ `0x18796c0(sel, k)` (k = 7, 1, 0x15, 0x16, 0x17) < max players - 1 → `out+4 = 0` | max players in force >= 4 |
| H2 | `HookCallSite(0x17bf187 → 0x15bdc20)`: `min(count, 2)` | none (identity with <= 2 cooperators) |
| H3 | `PatchBytes(0x138bcbf, 83 f8 02 75 32 → 83 f8 02 7c 32)` | none (identity with <= 2) |
| H4 | `ReplacePrologue(0x18c6db0, same 13 bytes)` → asm stub `bb_fourp_spfx_stub`: esi 7501 and `0x15bdc20(slots) >= 3` (asked only with WorldChrMan, its player, SprjSessionManager and the slot table present) → esi 7502; rdi..r11 and xmm0-7 saved, stack argument untouched, tail jump to the trampoline | none (>= 3 cooperators only exist through H1) |
| E6 | EMEVD dispatch filter (seamless_rules `SetEmevdRewriter` + `EnsureEmevdFilter`, installed even with `BB_PARTY_SEAMLESS=0`): 3[29] args `[g][0][3][2]` in events `xx04400..xx04406` / 12906962 → count byte 3; reverted (only bytes it wrote) when max players drops below 4 | max players >= 4 |
| P5 | `PatchBytes(0x1e97770, b9 02 00 00 00 → b9 03 00 00 00)` | local `BB_PARTY_MAX >= 4` at boot (cosmetic, server only) |

Switches: `BB_PARTY_FOURP=0` (nothing), `BB_PARTY_FOURP_SCALING=0` (no H2-H4),
`BB_PARTY_FOURP_NPC_SIGNS=0` (no E6), `BB_PARTY_FOURP_RECRUIT=0` (no P5).

**Max players and the version check.** `party_runtime` sets `LinkConfig::rules` =
`"max <BB_PARTY_MAX> players, <rule tag>"` (tag `4p:off` / `4p:v2:H1,H5,H2,H3,H4,E6`), sent last in
HELLO and compared by `identity_mismatch` next to eboot / gameplay patches / gameplay mods: a
guest with another max players or rule set gets a Mismatch REJECT "party rules differ (host max 4
players, 4p:v1:...; yours max 3 players, ...)". So every player sets the same Max players (the
launcher tooltip says so). The host's value also arrives in WELCOME and `FourpTick` applies it on
a connected guest (`EffectiveMaxPlayers`), so H1/E6 follow the host; WELCOME arrives at the title
screen (the network starts there), before a world loads.

**Network layers with 3 guests.** PartyLink slots 1..3 (max players clamped 2..4), the host
service's Matching2 room cap `min(game MaxMembers 5, BB_PARTY_MAX)` = 4, per-guest event pumps,
signaling and the vport tables are maps without fixed sizes (tests: `party-link-test`
"four players", `party-host-test` run with `BB_PARTY_MAX=4`). Known gap: a room of 4 leaves no
slot for an invader (host + 3 + invader needs a cap of 5). Guest-to-guest traffic is direct per
pair with a per-peer fallback through the host relay (PeerPaths, PARTY_COOP_PLAN.md
"Guest-to-guest paths"; party-soak runs 3 guests, `nat-pair` forces one pair onto the relay).

**Live check (2026-10-10, 4 loopback instances, `instances.py run --count 4 --party --env
BB_PARTY_MAX=4`, ports 5317x).** All four install H1-H4/E6/P5 (`Coop hooks: 4p ...` lines),
log `party rules: max 4 players, 4p:v1:H1,H2,H3,H4,E6`, and the host's roster reaches
`Hunter0..Hunter3 (slots 0..3, title)` 19 s in. Not yet checked live: a 3rd cooperator summoned
in the world (the harness instances stay at the title without input - the director's automatic
continue runs only after a crash restart - so the automatic summon flow does not run there yet),
H2/H4 boss scaling, E6 NPC signs. Memory: four instances plus other work hit the Windows commit
limit once (error 1455 creating the 7 GB pool; the restart loop recovered).

**Summon order with 2-3 signs (review, 2026-10-10, agent/live4p).** The host's director rings
the Beckoning Bell every `BB_PARTY_RING_EVERY` s (30) while `HostMayRing`: itself ready,
cooperators < max players - 1 and a member Home/Joining. Each bell is one summon: the host's game
polls `summon_messenger/get`, picks a sign, `summon_messenger/request` -> `guest_invite` to that
member, who joins and deletes its sign; the next bell 30 s later takes the next one. Fixed:
- the bell was gated on session role *idle*, so a host with one cooperator in (role 3 *host*)
  never rang again: no 2nd/3rd cooperator ever. `SessionMayRing`: the host rings idle or hosting,
  a guest only idle.
- a member whose link is lost (crash) or gone keeps no sign on the board (`FromApi::drop_signs`
  from `on_member_left`), so the host's game cannot pick a crashed member's sign while the others
  wait (signs are offered oldest first; a summoned guest's game deletes its own sign, seen live).
  Signs of members already in a room are *not* hidden: a crashed member's room membership can be
  held (loading) while its restarted game puts up a new sign. The
  game's own pick among several valid signs is not known (0x14baec0); with every offered sign
  valid it does not matter which one it takes.
- the 4th sign: at max 4 with 3 cooperators the director stops ringing and H1 no longer lifts
  (coop < 3 fails), the room cap (BB_PARTY_MAX) refuses a 5th member and PartyLink refuses a 4th
  guest (REJECT full); at max 3 the game's own 2-cooperator cap applies (no H1), PartyLink refuses
  a 3rd guest.
`tools/party/test_simguest.py` runs this against the real host code (party-host-harness, no
game): max 4 - three guests in one room on slots 1-3, SimC crashes and is back in its kept slot
~15 s later while SimA/SimB keep their links and heartbeats, SimC's sign taken off the board, a
4th guest refused (full); max 3 - two guests in, the 3rd refused (full).

**Live, 3 instances (2026-10-10, agent/live4p, loopback, `BB_PARTY_RING_EVERY=10`).**
- Max 3 (`--crash-instance 2 --crash-when "-> client \(6"`): Hunter1 summoned (`cooperators
  0 -> 1`), the host rang again while hosting and summoned Hunter2 into the same room 1001;
  Hunter2 was killed as it entered the host's world, the host logged `Hunter2 (slot 2) lost;
  slot kept`, Hunter2's restart auto-continued, signed (sign 3), was summoned again and the host
  reached `cooperators 1 -> 2` 132 s into the run. Hunter1 stayed `client (6)` throughout (no
  session change), its path to Hunter2 went `through the host relay` while Hunter2 was dead and
  back to direct (`punch Hunter2 ... peer heard`) after the restart; Hunter2 punched both peers.
- Max 4: both guests summoned (`cooperators 1 -> 2` after 76 s). H1 shape: with 1 cooperator
  `status rec0..3 2 2 0 0, counted 2; kinds 7:2`, with 2 `counted 3; kinds 7:3` - the per-kind
  sum is one more than the phantoms in the world (0x18796c0 also counts the selector's own
  session entries, sel+0x1c0), so the old H1 rule (sum < max players - 1) would never have
  lifted at 2 cooperators: the 3rd cooperator could not have come in. H1 now uses the game's
  cooperator count (0x15bdc20, FourpTick, twice a second).
- With 1 cooperator rec0/rec1 are already both "used" by that count, yet in the max 3 run (H1
  not active below max 4) the 2nd summon happened: the summary is not what limits a bell summon
  of the 2nd cooperator; whether it limits the 3rd needs the 4-client run (step 3 below).

### A7: four players, live with 4 real clients (2026-10-11, agent/sixp)

**Scope.** The party is capped at 4 (host + 3) by the user's decision (2026-10-11; ERSC allows 6).
One constant holds it: `party::kMaxPartyPlayers = 4` (party_link.h; `coop::fourp::kMaxPlayers`
mirrors it). Everything party-side follows it: BB_PARTY_MAX clamp 2..4 (PartyLink, director,
fourp), WELCOME slot / max checks, VITALS (`kMaxVitals`), party HUD rows (`kMaxCompanions`), the
Party members page (`MaxMembers`). Raising it later needs the engine work of section 1 ("A 4th
cooperator is not possible without rewriting fixed-size tables") and nothing else hard-coded.

**Changes.**
- System menu: Party members has one row per member (`MaxMembers = party::kMaxPartyPlayers`), then
  the notices, party HUD and guest lamps switches and the last event. The game shows the first six
  visible rows, so a full party of four shows the four members, notices and party HUD; guest lamps
  (also on the overlay's Party tab) and the last event give way. A separate page is not possible: the
  System menu is at its 12 pages (game-menu-pages-test: a full party of four, the six rows shown,
  kick on the 4th member's row).
- Placement (B2 / pre-ring spacing): `coop::fourp::SpotOffset` / `SpotForTry` (pure, tested):
  upstream's 0.5 m line (slot 1 right, slot 2 left, slot 3 2x right, nothing behind), each slot
  tries its own spot, then the mirrored one, then 3 / 1.
- Version check: see version_check.md (A9); the 4 live instances all logged the same build,
  params and party patch hashes.

**Live run (4 real clients, loopback, `instances.py run --count 4 --party --env BB_PARTY_MAX=4
--env BB_PARTY_RING_EVERY=10`, 1792 MiB VRAM each, lowest settings).** Evidence (host log,
t = seconds since the host's start):
- every instance: `party rules: max 4 players, 4p:v1:H1,H2,H3,H4,E6`, `build 0.5 (0.5-590-g5bd14843cf),
  params 581e28302a23..., party patches da32cddaed85... (35 hooks)` - identical, all admitted.
- roster Hunter0..Hunter3 on slots 0..3; every guest auto-continued into its own world, rang.
- `from api: Hunter0 summons Hunter1` -> `cooperators 0 -> 1 (t 35.3 s)`; `summons Hunter2` ->
  `cooperators 1 -> 2 (t 50.1 s)`; `Party 4p: H1: a cooperator slot opened (2 cooperators in the
  world ...)` -> `summons Hunter3` -> **`cooperators 2 -> 3 (t 158.8 s)`**. Hunter3 logged
  `joined room 1001 as member 4 with 3 peers`, `session trying-to-join -> client (6)`.
- every client then sees the same world: B2 status on each shows `cooperators 3`, `presence count 4
  (h 3 ...)`, four chrs with handles 10044000..10044003 in stage 0x15000000 (Hunter's Dream).
- placement (host's world, metres): host (-10.8, -6.9, -20.8); guests (-11.5, -7.0, -19.8),
  (-10.3, -7.0, -21.5), (-11.6, -7.0, -20.7): same height as the host (no wall / floor clip), 0.6 to
  1.1 m away. Pre-ring spacing put them at side 1 / 2 / 3 (0.5 m right, 0.5 m left, 1.0 m right of
  the start spot), each `side try 0 OK`.
- no link lost, no crash; the run ended by the queue's lease (240 s) with all four in the world.
- second run (same settings): `cooperators 0 -> 1 (t 32.3 s)`, `1 -> 2 (t 44.3 s)`, `2 -> 3 (t 101.3 s)`.
  Screenshots 4 s and 12 s after `2 -> 3` (`tools/mp/shot.py`, one per window; kept in
  `agents/sixp/shots/c3_12s_inst*.png`, not committed): every window shows the four hunters together on
  the Hunter's Dream path (host + 3 cooperators, "Co-operator Hunter1..3" / "Beckoner Hunter0" plates),
  nobody in a wall or under the floor, the party HUD with three rows at 0.9-1.2 m. They stand as a loose
  group rather than a clean line: one cooperator stands about 0.5 m behind the host.

- third run (merged build 0.5.1 (0.51-613-g77c74f5281), all four identical, 36 party hooks):
  `0 -> 1 (t 25.6 s)`, `1 -> 2 (t 43.6 s)`; Hunter3 was invited twice and dropped both times as
  below, then the queue's 240 s lease (12 runs waiting) ended the run before a third invite. So the
  4th player got in on 2 of 3 runs, 57 s and 108 s after the 2nd cooperator. Pre-ring spacing put the
  guests at side 1 / 2 / 3, 0.5 m apart along the right axis, each `side try 0 OK`.

**Open: the 3rd cooperator's summon is slow and not reliable.** Hunter3's first two invites (host `summons Hunter3`
right after `cooperators 1 -> 2`, and ~30 s later) reached its game (`guest_invite`, signaling up,
about 8 small vport-40 packets) but its game never called `JoinRoom`: the exchange stopped and
the connection was deactivated (host side `member 65282`: never a room member). The third invite, ~100 s after Hunter2's arrival, went through (16 / 9 / 38 / 267-byte
exchange, then `JoinRoom`). Hunter1 and Hunter2 joined on their first invite. Candidates: a host-
side cap evaluated in the session handshake that H1 does not cover (the SOS filter 0x1874710 (b)
counts `sel+0x1c8` outgoing requests; the selector update 0x1876b10 `pending + coop >= 3`), or a
timer after the 2nd arrival. Next: trace 0x1874710's verdict on the host during the 3rd summon.

### The 4th player's dropped invites: what is known (2026-10-11, agent/sixp; open)

- **Not the SOS filter.** `0x1874710(sel, req, flag)` refuses a cooperator request when
  `sel+0x1fc (max coop, 3) <= pending + cooperators` - for the 3rd cooperator 1 + 2 = 3 - and
  `0x1876b10` stops at the same field. H5 (party_fourp: `sel+0x1fc` = party size while max players
  >= 4, `SelectorMaxCoop`, rules tag `4p:v2:H1,H5,...`) lifts that, but a probe run
  (`BB_PARTY_FOURP_PROBE=1`) showed the host's filter returning a request for **every** call,
  Hunter3's two included (`slots coop 2 ... all 3, room max 5; max coop 4; pending 0`). The guests
  never call it. H5 is kept (it is the game's own limit for 3 cooperators) but does not fix the drop:
  two H5 runs still dropped Hunter3's (and once Hunter2's) first invites.
- **Where it stops.** The summon handshake on vport 40 (guest side): recv 15 / send 24 / recv 62 /
  signaling up / send 53 / recv 17 / send 17-26 / send 54 / recv 18 / recv 54 / send 9, then on a
  good join **recv 267** and the guest's game calls `sceNpMatching2JoinRoom`. On a dropped invite the
  267-byte message never comes (once it came and the guest still did not join); ~10 s later both
  sides deactivate the connection (`member 65282`: never a room member). The room is not full (the
  game was told max 5 at CreateJoinRoom; 3 members). So the host's game decides after the guest's
  54-byte reply, outside the filter.
- **Next:** `BB_PARTY_FOURP_PROBE=1` also logs every vport-40 datagram's first 32 bytes and, on a send,
  the game's return addresses (bbnet `set_packet_trace`): one run with 4 clients shows which state
  machine sends (or does not send) the 267-byte message and what it checks. Not run: the user
  limited the machine to 2 game clients (2026-10-11), so 3-4 client runs are paused.

### 4 players with title join and headless guests (2026-10-12, agent/sixp, 0.5.6)

Since 0.5.6 guests join from the title (B2 title join, `BB_PARTY_TITLE_JOIN`), not by the bell,
so the bell path's dropped 3rd-cooperator invite above no longer decides a 4-player join. Runs with
2 rendering + 2 headless instances (`instances.py --headless 2`, `--fresh-saves`; headless-lite:
`BB_HEADLESS`, DEBUG_SWITCHES.md):
- all 4 started together: once all three counted within 5 s (27.1 / 29.7 / 32.0 s); once the host's
  game never finished one guest's first sync (it re-sent the data request 0xA 38 times; cooperators
  stayed at 2) and that guest's window stayed on the loading screen;
- headless guests started 20 s after the first join (`--headless-delay-after-join 20`): all three
  counted (24.2 / 104.7 / 118.7 s), each title join on the first try, but every headless guest only
  ~48 s after its own load ended (the host's first-sync request repeated ~33 times);
- screenshots: the host shows all four side by side with plates on the right heads; the first
  guest (rendering) is in the world, not stuck.
- **Bug (title join, B2 owner):** a title-joined guest has chrs only for the players already in the
  world when it arrived. Run 2's remote chr lists (B2 `remote chrs:` / `presence count`): host 3
  guests; Hunter1 only the host; Hunter2 and Hunter3 the host + Hunter1, not each other. Signaling
  works (every pair `peer heard`), so the newcomer's chr is not announced to the guests already in
  the world (its first sync goes to the host only). Bell joins (the 4-client runs of 2026-10-11) gave
  every guest every chr. This is the user's "guest 1 didn't see the third player".
- reproduced with only the host rendering and the guests started one at a time
  (`--headless 3 --join-sequential --stagger 10`, run 5): joins at 24.8 / 48.2 / 84.7 s, each on the
  first try; `presence count` per peer: host 4, Hunter1 2, Hunter2 3, Hunter3 4 - every guest misses
  exactly the guests that came after it. Host screenshot: all four together, three plates and HUD
  rows at 0.9-1.0 m (Hunter3's plate drawn higher than the others: the user saw the last joiner
  spawn on top of Hunter1 - its spot (2x right, 0.5 m beyond slot 1's) was 0.36 m from Hunter1, who
  had moved).
- **Placement fix (a20af90):** `BesideHost` takes a free spot (`coop::fourp::PickFreeSpot`: the own
  spot, else right / left at 1-3 spacings; free = 0.8 m from every other member's fresh VITALS
  position; unit-tested). Run 6 (same setup): joins 19.3 / 42.6 / 79.2 s, the host's screenshot shows
  the four in a clean line, none on another - but the free-spot choice did not trigger there (a
  title joiner at its title may not have the members' VITALS yet), so the fix itself is not proven
  live. The late-guest visibility bug reproduced again (presence count 4 / 2 / 3 / 4).

### Five players (2026-10-12, agent/sixp; test setting, not the default)

`party::kMaxPartyPlayers` = 5 (BB_PARTY_MAX still defaults to 3; the launcher offers 2-4). With
BB_PARTY_MAX=5 every peer installs **H6** (`0x1874bbe cmp esi,4` -> 5: the SOS filter's "cooperators +
invaders + others + outgoing < 4", rules tag `...,H6`) and H5 writes the selector's max cooperators
5 (the field counts the pending request: 5 = 4 cooperators). Host + 4 fills every engine table
(presentation 5, remote data rows 4, ChrSet 5, room 5). Runs: host rendering + 4 headless
(`--count 5 --headless 4 --join-sequential --fresh-saves`, queue max_solo 5):
- run 1 (before flowlive's e214b5b): when the 3rd guest's first sync never completed, the host's game
  left its own session (`BeginLeaveSession` from the session event handler +0x178980d) and every
  guest dropped (cooperators 2 -> 0, rejoins, again);
- run 2 (with e214b5b): 3 guests in, every peer built every other chr (presence count 4 everywhere);
  the 4th guest was admitted (room member 5 of 5) but its game stalled before its first frame
  ("== Stall during rendering at flush 1": 5 x 1792 MiB VRAM on an 8 GB GPU) and exited;
- run 3 (headless 1024 MiB VRAM): **all four guests in: cooperators 0 -> 1 -> 2 -> 3 -> 4 at 20.7 / 42.4 /
  74.0 / 117.9 s**, each on the first try; every guest's presence count 5 (all five chrs on every
  guest). Host screenshot 5 s after the 4th: five hunters, but slots 3 and 4 stand almost on slots 1
  and 2 (plates overlap in pairs: the 0.5 m line puts 2x right / 2x left 0.5 m past them, and the
  free-spot choice had no member positions yet), and one overlay name tag ("Hunter2") floats high.
So 5 players work end to end in one run; open: placement of slots 3-4, the floating tag (name-plate
owner), and repeat runs before 5 becomes a user setting.
- Placement check: run 3's final positions show the pairs ~0.8 m apart (not stacked; everyone shifted
  after the spawn, the host too, and from behind the host the outer guests line up with the inner
  ones in depth, so the plates overlap). b9d9eeb: the free spot now also counts the chrs already in
  this world (title join re-places the guest on arrival) and keeps 1.0 m. The green "Hunter2" tag is
  the E2 overhead tag, drawn 2.45 m up on purpose (above the game's plate; `TagVisible` hides it
  without a live chr here): not a fault.
- Repeat runs (b9d9eeb): run 4 - the 5th game stalled again before its first frame ("Stall during
  rendering at flush 1") and exited, the 4th guest never joined (the other three in, each first
  try); run 5 - **all four guests in** (17.1 / 33.0 / 62.1 / 99.6 s), every guest's presence count 5.
  So the party code takes 5 players; what fails 1 run in 3 is the 5th game process on this machine
  (its first frame stalls with five games running). 5 stays a test setting (BB_PARTY_MAX=5), not in
  the launcher.

### "Joined players' join animation replays when a new player joins" (user, 2026-10-12)

- **No chr is rebuilt and no slot is mixed up.** In the 5-player run 5 and a 3-player run, every
  peer's B2 `remote chrs:` list keeps each existing member's PlayerIns address unchanged across every
  join (host: 1 -> 2 -> 3 -> 4 chrs, only appended; guests likewise), with draw alpha 1.00 throughout
  (no ChrFadeIn of an existing chr at the 1 s log granularity).
- **The effect is real, at the newcomer's arrival.** 3-player run (host rendering, 2 headless,
  sequential), host screenshots every 3 s after Hunter2 entered the room: 12 s in, before the host
  counts it, the host and Hunter1 both glow translucent blue (the summon / arrival look) for a moment;
  before and after they are normal. In the host's log that window holds only the new first-sync data
  requests (`first sync: a member's task waits at FirstSyncCompleteWait: data request (0xA) #1-#3`,
  e214b5b, flowlive) and the newcomer's chr appearing.
- Next (flowlive): an A/B with those every-peer 0xA requests off, host screenshots at 0.5 s steps
  across the newcomer's arrival; and the same moment in a bell join (vanilla) to see whether the game
  itself flashes existing phantoms when a new one arrives.

### Host does not draw a guest / its plate floats away (user, 2026-10-11)

Seen with 2 and 3 players (user screenshots; split24 and a hostdeath run): on the host the guest's
plate ("Co-operator Hunter1") hangs far away at the Central Yharnam lamp arrival point and no guest
model is near, while the party HUD (VITALS) puts the guest 0.9 m away and the guest sees both
players correctly. The party code's local warps of the guest (B2 BesideHost / split travel's in-world
warp, `WarpLocal`: chr vfunc +0x5F8 on the local player) are not replicated to the host's copy of the
guest's chr; the splittravel agent owns that fix. Not a per-slot bug: the 4-client runs here
(summons into the Dream, guests placed in their own world before the summon, no warp after the
arrival) showed every chr where its player stood, on every screen. Detection: the party HUD now logs
`GAP <name> (slot N): this game draws its chr at (...), it reports (...): d m apart` when a drawn chr
and the member's own reported position differ by more than 2 m (always on, every 5 s at most per
member); `BB_PARTY_HUD_LOG=1000` traces both positions every second on every peer.

### Live test plan for 3 and 4 clients (when the machine allows 3+ game clients, or on two PCs)

Setup: `instances.py run --count 3|4 --party --env BB_PARTY_MAX=3|4 --vram 1536 --fps 30
--seconds 600` (one save copied to every instance; each guest needs a character past its
prologue: `start ... READY`). Logs: `instances.py logs --grep "Party|from api|4p"`.
1. Boot / rules: every instance logs `party rules: max N players, 4p:v1:...` and (N = 4)
   `Party 4p: rules ...: H1 on ...`; a guest with another BB_PARTY_MAX is refused (Mismatch).
2. Roster: the host's roster reaches Hunter0..HunterN-1 at the title; every instance
   auto-continues into its world (`auto-continue: world up`).
3. Summons (host log): `ringing the Beckoning Bell` -> `from api: Hunter0 summons HunterK` ->
   `cooperators 0 -> 1`; ~30 s later `ringing ...` again with session `host (3, ...)` ->
   `summons` the other guest -> `cooperators 1 -> 2`. N = 4: `Party 4p: H1: a cooperator slot
   opened (2 cooperators in the world, ...)` -> third `summons` -> `cooperators 2 -> 3`. Each
   guest: `session trying-to-join -> client (6)` and roster `in_host_world`. Signs: after each
   summon the host's `asks for signs` count drops by one (the summoned guest deleted its sign).
4. Cap: N = 3 with 3 guests running (BB_PARTY_MAX=3 but `--count 4`): the 3rd guest's link is
   refused full; N = 4: no 4th summon, no bell at 3 cooperators (no `ringing` after `2 -> 3`).
5. Guest crash (`instances.py crash --instance 2` with 2-3 guests in): host `Hunter2 (slot 2)
   lost; slot kept` + `Hunter2's link is gone: 1 sign(s) taken off the board`; `cooperators
   3 -> 2` only; the other guests stay `client (6)` (no session change, no world reload in their
   logs); Hunter2 restarts, auto-continues, rings, is summoned again (`is back (slot 2)`,
   `cooperators 2 -> 3`) within ~60-90 s.
6. Guest-to-guest P2P: each guest's status line / log lists every other guest as `direct`
   (loopback) - force `BB_PARTY_FORCE_RELAY=1` on one run: pairs listed `relay`, host log
   `udp ... relayed N` grows, phantoms still move for every viewer. All three/four see the same
   phantoms (screenshots of each window, or the cooperator count in each log).
7. Boss scaling (N = 4, 3 cooperators, any boss fog): host log `H2: boss scaling sees 3
   cooperators as 2` and `H4: ... SpEffect 7501 -> 7502`; NPC signs (E6) only where the event
   ran (`E6: NPC sign event ...`).
8. Leave: one guest leaves (overlay Leave) -> its phantom vanishes on every screen, the others
   stay, the director rings for it again when it is Home.

## 8. The floating phantom name plate (agent/balance findings, 2026-10-11)

User report (3 players, Hunter's Dream, host view): the game's own blue "Co-operator / Hunter1"
plate with its HP bar floats over the stairs ~10 m away with no character under it, while the
party HUD says Hunter1 is 0.9 m away; "2 players side by side on the host, one healthbar far
away, 3 players on both guests".

* The plate is the game's (`FrpgMenuDlgFloatingPCGauge`, string 0x4930683; the enemy bars are
  built in 0x167c460 next to it), not the port's overlay tag. It is drawn over the HOST's own
  copy of the guest's PlayerIns.
* The party HUD's 0.9 m came from VITALS (the guest's own game), because the matching in
  party_hud_game only pairs a chr here with a member whose report is within 8 m: the host's copy
  of Hunter1 was more than 8 m from where Hunter1 really was, i.e. the host's copy of that guest
  stopped getting position updates (and is not drawn there: no model under the plate). The guests'
  copies are right. So the anchor is the copy's own (stale) position; the fix belongs in the
  host's per-slot position path for the third member (the four-player agent is on it), not in
  the plate.
* Diagnostics added: party_hud_game logs `stale remote chr? one at (x y z) HP h/m is D m from the
  nearest member report (slot S)` (once a minute) and, with `BB_PARTY_HUD_POSDIAG=1` +
  `BB_PARTY_HUD_LOG=1`, every position-like quad in each remote PlayerIns and the objects its first
  0x400 bytes point to (to find a second, stale anchor if the copy's physics position is right).
* The port's own overhead tags (E2) now draw only for a chr read this tick, alive, on our map, from
  a fresh board (`party::hud::TagVisible`), so they never float; `Party HUD: tag X hidden: why`.

### Title join: every peer builds every chr (2026-10-12, flowlive)

The late-joiner bug above: a title joiner's first sync (its data, packet 0xC and the 0x168 record,
sent only in reply to a data request 0xA and only with its GameData +0x10 set) reached the host
only, because only the host's title-join code asked for it. Now every peer in the world (host or
guest) asks any member whose task waits at FirstSyncCompleteWait (step 3, no chr) for its data
(0xA, after 1 s, every 2 s), and the title joiner sets its GameData (+0x10 2, +0xA4 1, +0x5D8 7) as
soon as its load is in the host's world (flow 4) instead of at the end of its loading screen.
Live (host rendering + 2 headless, `--join-sequential --fresh-saves --stagger 10`, root
flowlive/mp3, saves from flowlive/saves3): every peer's member tasks at Update with a chr, two
remote chrs each (host: guests 1 and 2; guest 1: host + guest 2; guest 2: host + guest 1;
`presence count 3` on all three); host `cooperators 1 -> 2` at 46.3 s for a guest whose own
loading screen ended at 51.8 s (no longer ~48 s after it).
Note: `--saves` must be a save set with a character and the system data; a missing directory gives
new saves, and the game then waits on its first-launch brightness screen.

### Late joiner re-check on 0.6.0+ (2026-10-13, a7)

Same setup (host rendering, 2 headless title-joined guests, `--join-sequential --stagger 10
--fresh-saves`, root agents/a7/mp1 + mp2): both runs give every peer `presence count 3` and two
remote chrs (guest 1: host + guest 2 from 43.5 s), host `cooperators 1 -> 2` at 38.7 s against
guest 2's `in the world` at 40.2 s - the late-joiner bug above stays fixed and the ~48 s first-sync
lag is gone. The host's screenshots (shot.py PrintWindow) came out all black in both runs, so this
check is log-only; 4 players were not re-run.

## 9. A 4th player's rejoin crashes the headless guests (parab3 a7t1, 2026-10-15) - fixed (parab4)

Run: `instances.py run --count 4 --headless 2 --join-sequential --stagger 10 --fresh-saves --party
--env BB_PARTY_MAX=4 --env BB_PARTY_TEST_START_LAMP=2412951` (title join and direct summon on, the
current defaults); the rendered guest (inst1) `BB_PARTY_STATUS_TEST=guest:leave@105,guest:rejoin@120`.

* Title joins: all three on the first try, host `cooperators 0 -> 1 -> 2 -> 3` at 28.4 / 55.4 /
  82.5 s; every guest logs 3 remote chrs.
* inst1 Leave at 121.7 s (`client -> leaving -> idle` 0.4 s, its own world), Rejoin at 136.7 s
  (`starting a fresh link`, direct summon joined 1.5 s after the request, `cooperators 3 -> 4` on
  inst2 / inst3 at 144.1 / 144.3 s; on inst2 the rejoined chr fades in at its old address).
* 146.3 s inst2 and 151.5 s inst3 crash (both headless-lite), in the game's chr threads
  (CSChrThread2 / CSChrThread1), reading a peer's type-6 packet queue: 0x1A6D390 (a remote chr's
  update, `0x1789200(flow, peer, 6, buf, 0x63F8)`) -> 0x17910F0 (pop the peer's packet: memcpy, then
  free). inst2: null memcpy source (fault address 0, host RIP in the CRT); inst3: the free's heap
  lookup failed ("Heap not found", 0x20B55B0 from 0x17911A6). The entry's buffer was freed or
  replaced while the chr thread read it.
* After their restarts both title joins failed (`no snapshot within 20 s`) and the host's game kicked
  the rejoined inst1 at 190.5 s (`sceNpMatching2KickoutRoomMember`).

Not seen on the rendered host or the rendered rejoiner. Suspects: two chr threads popping the same
peer's queue (two remote chrs keyed to one peer id after the rejoin: the old entry's peer match,
`FUN_00CA1C70`), or the per-peer queue freed by the member's leave while a chr still pointed at it.
Next: the same run with all guests rendering-lite off (2 windows + 1 headless) to tell headless from
4-player, and a log of the flow's per-peer list (+0x28) at the rejoin.

### 9.1 Root cause and fix (parab4, 2026-10-15)

* The queues: SprjNetMan (`*0x553D6D0`) +0x28 is a list of peer entries {+8 PlayerId, +0x30 one queue
  per packet type (0x44)}. A received packet is pushed by 0x178D510 -> 0x1790C20, popped by 0x1789200
  -> 0x17910F0 (memcpy, free, unlink). Neither takes a lock; the remote player chr updates
  (0x1A6D390, type 6) run on the chr worker threads (a different thread almost every frame), so the
  game relies on each queue having ONE reader. The entry is found by 0x1789380: the first entry whose
  PlayerId equals the chr's (0xCA1C70: equal by the member, not by the object).
* The bug is not 4-player specific and not headless specific: it is the **bystander**. When a guest
  leaves, the host's game removes its player (cooperators 2 -> 1). The other guests' games end its
  remote player task and drop its peer entry, but keep its PlayerIns in the ChrSet (status lines:
  `[2 h 10044002 kind 1 st 3 ... chr ok net 7]` stays, cooperators stay 2, presence entry stays).
  When it rejoins, the kept PlayerIns matches the member again and reads its queue; the game also
  makes a new one. Two chrs pop one type-6 queue from two chr threads: the null memcpy source and
  the "heap not found" free of a7t1. Live with the diagnostics (a7b, 3 players, the headless
  bystander inst2): `A7: two chrs read peer entry ...` after the first rejoin, 3 readers of one entry
  after the second, `cooperators 2 -> 3` in a 3-player party, and a crash in a task step
  (0x1E50090 from the MultiPlay task update 0x1E556F0) on the third.
* Fix (party_b2 `StaleMemberChrTick`): a guest removes, 2 s after it saw it, a player chr whose
  PlayerId has no peer entry any more and that no task holds, the way the B2 split removal does it
  (EnterleaveDirector leave, presentation slot release 0x15BC080, WorldChrMan remove 0x19186E0).
  Two safety nets stay on (party_fourp): every push and pop of a peer queue runs under one lock, and
  when two chrs read one peer entry the newer keeps the updates (`BB_PARTY_QUEUE_GUARD=0` turns the
  lock and the guard off).
* Live a7e (3 players, host + rendered guest + headless bystander, three leave / rejoin cycles in
  one session): on the bystander `split: remote chr ... removed (A7: its member left the session ...)`
  2.5 s after each leave, `cooperators 2 -> 1`, and `1 -> 2` at each rejoin (never 3); no
  `two chrs read`, no same-queue overlap, no crash; screenshots show the three together after every
  rejoin. The host's Leave at the end ended the session on both guests in 0.4 s.

## Five players (visy 0.7.8, CL-de2e94; hand-ported for Nightwatch 0.2.6 beta 3)

Ported from visy `574c7c25`: the host's direct-summon cap `HostView::max_cooperators` stayed 3, so a 4th guest's rejoin fell to
the bell sign path (`dsummon::MaxCooperators`); the entry wait no longer runs while the guest's own world loads
(`dsummon::HoldWhileLoading`); the Party members page had seven rows for six slots, so *Last event* steps aside at five.
Not ported: his smoke phase `fp5`. Ours is the row `fiverejoin` in `tools/qa_scenarios.json`, not a release row: it starts five
games, about 40 GB of free commit by our QA notes. visy's run passed with five clients on one 32 GB PC (about 20 GB of
working set, free memory down to 0.4 GB). Not tried by him: five real PCs, the late areas, invaders, Chalice Dungeons.

## Five players (0.7.8, CL-de2e94, smoke `fp5`)

Host + 4 guests (`BB_PARTY_MAX=5`) passes the smoke phase `fp5` (`--only fivep`, 30 and 60 FPS; two
windows drawn, three headless-lite): four title joins (`cooperators ... -> 4`), the party lamp travel,
the Cleric Beast with all guests in the arena (`H2: boss scaling sees 4 cooperators as 2`: the scaling
stays at the three-player row 7502), a guest's death and arena revive, the boss kill, a guest's Leave
and Rejoin (host `cooperators 4 -> 3 -> 4`). Both drawn windows show four party rows. Bugs found at 5:
the host's direct-summon cap `HostView::max_cooperators` stayed 3 (the 4th guest's rejoin fell to the
bell sign path); the Party members page had seven rows for six slots (the Connection warnings switch was
dropped; *Last event* now steps aside at five). Memory on a 32 GB PC: five clients hold about 20 GB of
working set together (host 4.3 GB, guests 3.8 GB, private bytes about 14 GB); the free physical memory
fell to 0.4 GB and the drawn windows ran 14-16 FPS with a 60 FPS cap: five on one PC is a stress
test, not a play setup. Not tried: five real PCs, the late areas, invaders, chalice dungeons.
