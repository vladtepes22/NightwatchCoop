# Join / summon timing (fast join)

Where the time of an automatic party summon goes, which pauses were removed, and what is left.
Switches: `BB_PARTY_FAST_JOIN` (game pacing patches, sign polling, director pauses, room
delays) and `BB_PARTY_FAST_MESSAGES` (banners); both on by default in party mode, `0` / `off`
turns each off (gpu/party_env.h `bb_party_switch_on`, scripts/party_env.py `party_switch_on`).
Addresses are our offsets (XML `Address` = offset + 0x400000).

## Measuring

`tools/mp/instances.py run --timestamps` prefixes every log line of every instance with
`[+SSS.mmm]` (seconds since the harness started the first instance, one clock for all).
`tools/mp/join_timeline.py --root <root>` prints the milestones of the summon in time order
(`--from 1:"Launching Bloodborne":2` measures a crash rejoin from the guest's restart). The
director logs the game's sign polling state when it rings (`Party: bell: sign polling: ...`, and
again 2.5 s later); `BB_PARTY_TRACE_BELL=1` adds entry traces of the sign chain (0x1900500,
0x130cde0, 0x1901010, SendSign, CreateSign, SSM tick, the summon_messenger builders).

Runs: loopback, 2 instances, the same save, guest started 8 s after the host (harness stagger).

## Automatic summon: before / after

Both players in their world -> the guest in the host's world (host `cooperators 0 -> 1`).

| Step | Before (fast off) | After | What decides it |
|---|---|---|---|
| guest in world -> guest rings | 5.0 s | 1.0 s | director: `ring_delay` (`BB_PARTY_RING_DELAY`) |
| guest rings -> sign created (`summon_messenger/create`) | 5.1 s | 5.0 s | game, open (see below) |
| sign on the board -> host's poll lists it | 1.6 s (up to 5) | 1.0 s (up to 1) | ss.info `SummonDataGetListInterval` 5 -> 1 |
| poll -> `summon_messenger/request` / `guest_invite` | 0.6 s | 0.4 s | game (room creation) |
| invite -> guest `JoinRoom` (summon data received) | 3.3 s | 0.8-1.3 s | game: host summon-message retry 3.0 -> 0.5 s |
| `JoinRoom` -> session client (6) | 3.7 s | 3.4 s | game transport (first connect retried after ~3 s), open |
| client (6) -> guest loading screen | 5.7 s | 1.2 s | game: guest self-prologue 5.0 -> 0.5 s |
| loading the host's world | 3.3 s | 3.3 s | real load (kept) |
| **both in world -> cooperators 1** | **28.7 s** | **16.9 s** | |

Evidence (`E:\games\bbport\agents\joinfast\mp`, `join_timeline.py`):

- before (`BB_PARTY_FAST_JOIN` off build, `base_inst*.log`): guest in world 38.174, guest rings
  43.196, create 48.304, host poll 49.952, request / invite 50.550, JoinRoom 53.835, client 57.571,
  loading 63.290 -> 66.609, host cooperators 1 at 66.927.
- after (increment 1): guest in world 34.536, rings 35.568, create 40.598, poll 41.575 (`sign
  polling: interval 1 s`), request 41.974, JoinRoom 43.300, client 46.733, loading 47.953 ->
  51.270, cooperators 1 at 51.412.

## What was changed

### Our code

| Item | Before | Now | Where |
|---|---|---|---|
| First bell after the world is steady (and the bell grant) | 5 s | 1 s | `party_director.cpp` `ring_delay` |
| Title: settle before the first Continue / between presses | 4 s / 2.5 s | 1 s / 1 s | `party_director.cpp` `AutoContinue` |
| ss.info `SummonDataGetListInterval` / `SummonDataCreateInterval` | 5 / 5 | 1 / 1 | `from_api_formats.inc` `kSsFastJoinValues` |
| Matching2 room answer / signaling ESTABLISHED after create / join / member joined | 300 / 800 / 500 / 300+800 ms | 100 / 300 / 200 / 100+300 ms | `np_matching2.cpp` `join_ms` |
| Party link reconnect backoff cap (a crashed host comes back) | 8 s | 2 s | `party_link.h` `backoff_max_ms` |

Not changed: 0 for the polling interval (the get-list builder 0x1e946e0 has no in-flight guard:
a request per frame); the sign lifetime (30 s patch) and the host-lost grace (2 s): they guard
correctness.

### Game patches (patches/Bloodborne.xml, byte-verified, reported by seamless_rules.cpp)

`Party: Fast join` (`BB_PARTY_FAST_JOIN`):

| Site | 1.09 | Patched | What |
|---|---|---|---|
| 0x196d3a8 | `c7051e77c0030000a040` | `...0000003f` | CSEnterleaveDirectionStep static init: guest self-prologue 0x5574ad0 5.0 -> 0.5 s (STEP_C2H_Wait_forSelfPrologue 0x196b500 gates the guest's load via STEP_C2H_Init_forSelfLoading 0x196b580 / +0x134) |
| 0x196d3b2 | `c7051c77c0030000a040` | `...0000803f` | others-epilogue cap 0x5574ad8 5.0 -> 1.0 s (0x196be90 ends earlier when the effect is done) |
| 0x196d3c6 | `c7051077c0030000a040` | `...0000803f` | return-home wait 0x5574ae0 5.0 -> 1.0 s (STEP_H2C_Wait_forEffect 0x196c2d0) |
| 0x196d3d0 | `c7050e77c00300000040` | `...0000003f` | its fade 0x5574ae8 2.0 -> 0.5 s (`alpha -= dt / x`) |
| 0x14b80c4 | `41c7451c00004040` | `41c7451c0000003f` | SummonStepManager tick: the host re-checks whether the summoned guest is a connected peer before it sends the summon data every 0.5 s instead of 3.0 s |
| 0x14b4975 | `41c744241801000500` | `...01001e00` | that queue entry's tries 5 -> 30 (the same 15 s window) |
| 0x14bb067 | `c7431801000500` | `c7431801001e00` | the same for the second creator 0x14baec0 |

`Party: Fast multiplayer messages` / `Party: Hide bell searching messages`
(`BB_PARTY_FAST_MESSAGES`): the multiplayer banner is one Scaleform dialog (SummonMessage,
`menu/summonmessage.gfx`, update 0x1bff910, FMG EventText 100001..200000). Its fades are the
movie's own (FadeIn 5 frames, FadeOut 7 frames at 30 fps: ~0.17 / 0.23 s); the hold is code:

| Site | 1.09 | Patched | What |
|---|---|---|---|
| 0x1bfface | `41c745000000a040` | `41c745000000c03f` | hold of a new message 5.0 -> 1.5 s |
| 0x4928a98 | `0000a040` | `0000c03f` | rodata 5.0 read only at 0x1bffa7d (re-show after a cutscene) |
| 0x1389cfc | `e97f618700` | `c390909090` | host "Searching for the chime of a Beckoning Bell..." (tail jmp to ShowSummonMessage 0x1bffe80) -> ret |
| 0x17c3cea | `e891c14300` | `0f1f440000` | guest "Searching for the chime of a Small Resonant Bell..." call (result unused) -> nop |

All three are `Party="cosmetic"` (left out of the party version check): they change only local
pacing, so a host and a guest with different settings still match.

## Kept (correctness)

`[0x553d6d0]+0xb8` summon / room timeouts (55 / 85 s), pending-summon lifetimes in 0x1872360
(55 s, 180 -> 30 s), the sign lifetime SSM+0x194 (30 s), CSMultiPlayerInsTask
FirstSyncCompleteWait / CreateWait (network flags), SprjMoveMapListStep::Wait_forData,
Wait_forSelfEpilogue (effect state), the world load itself, the 2 s host-lost grace.

## Round 2: ringing and finding the peer

Measured with `join_timeline.py` (`E:\games\bbport\agents\joinfast\mp\r2i*_inst*.log`,
`r2k*_inst*.log`). Both in their worlds -> host `cooperators 1`:

| Step | Increment 1 | Round 2 | What |
|---|---|---|---|
| guest in world -> rings | 1.0 s | 0.3 s | `ring_delay` 0.3 s |
| rings -> sign created | 5.0 s | 2.0 s | `Party: Fast bell sign` |
| sign -> host's poll lists it | 1.0 s | 0.04 s | the host director zeroes the game's poll countdown when a sign lands on our board |
| request -> guest JoinRoom | 1.3 s | 0.8 s | signaling ESTABLISHED / ACTIVE +50 / +100 ms |
| JoinRoom -> client (6) | 3.4 s | 1.7 s | NexusRevolution first retransmission timeout 3000 -> 1000 ms |
| client (6) -> loading screen | 1.2 s | 1.2 s | (round 1: 0.5 s prologue) |
| loading the host's world | 3.3 s | 3.3 s | real load |
| **both in world -> cooperators 1** | **16.9 s** | **~10 s** (33.73 -> 43.63, 33.40 -> 43.76) | |

**Bell -> sign: 5.0 s.** The traced bisect (`BB_PARTY_TRACE_BELL=1`) showed that SendSign 0x1901320
is not reached through 0x1900500 / 0x130cde0 / 0x1901010 but from the bell sign sender, a small
object (vtable 0x532f390, ctor 0x18a9050). Its update 0x18a9350 counts `+0x28` down while
`+0x20` bit 0 is set. At 0 it sends the sign of kind `+8` (0x18a9900: type 7 for the Small
Resonant Bell, or a natural-enemy type by the ss.info percentage) and re-arms 30 s for the
resend. Every start writes 5.0: the functions 0x18a9250 / 0x18a9280 (0x18a9264 / 0x18a9324) and
ten inlined starts, one per sign kind 1..10, at 0x189a095 + n*0x110.

`Party: Fast bell sign` sets the timer to 2.0 s. It is on with fast join; `BB_PARTY_FAST_SIGN=0`
turns it off alone. 0.5 s was tried and rejected: in 2 of 6 runs the guest was summoned while its
bell was barely up, and the join stalled (the host never sent the summon data, or the guest
stopped after client (6)). 2.0 s passed every run.

**Host bell while a summon runs.** The host director no longer rings within 20 s after its game
asked to summon someone (`summon_messenger/request`). A bell in that window restarted the search,
and the summon failed into the game's 55 s summon timeout.

**Kept:** the guest's 1.2 s from client (6) to the loading screen (0.5 s prologue plus the effect
start) and the 3.3 s load.

## Guest crash rejoin: before / after

| | Baseline (fast off) | Round 2 |
|---|---|---|
| guest restart -> host cooperators 1 | 104.9 s (62.40 -> 167.33) | 30.4 s (46.10 -> 76.51; 45.10 -> 75.48) |
| crash -> cooperators 1 | 108.6 s | 34.0 s |
| failed summons before the good one | 2 (host never sends the summon data) | 0 |

Why it failed (static RE, confirmed live). The host's game reuses what it had for the crashed
guest's online id and address:

- **Stale peer reservation.** The peer pool 0x147b350 (SSM+0x48) keeps a peer reserved for that
  online id. 0x147af20 renews and reuses it on every summon retry, even when its connection
  belongs to the dead process.
- **Stale transport connection.** The NexusRevolution connection to the same address/port lives
  until the transport's own timeout (~25 s, the defaults at 0xcdb309 / 0xcdb314). A summon in that
  window sends the summon data over the old connection; the restarted game waits and gives up.

Fixes:

1. `PartyHostService::context_start`: a member registering a new context while it is still in a
   room is a restarted game. Its old membership ends at once (MEMBER_LEFT + signaling DEAD on the
   host instead of the 15 s heartbeat timeout), and it is reported to the host director.
2. Host director `ClearStalePeers`: once the host's game has left the session with the dead peer,
   the peers reserved for that online id lose the id (first byte `#`) and the keep flag. The next
   summon reserves a fresh peer.
3. `Party: Fast rejoin`: the peer keep 30 -> 10 s (0x147afe4, 0x147b0a7, 0x1480cf7, 0x1480e14).
4. `FromApi::hold_signs`: a member that was in, joining or loading into the host's world when its
   link dropped has its new signs held for 26 s after the drop, so the host's game cannot summon
   it over the old connection. In the runs the hold ended 2-5 s after the restarted guest's sign
   went up, so it costs about that much. Lowering the transport timeout would remove it, but would
   also drop players on a real network hiccup that short.

### Profile and what was tried (2026-10-11, C6)

Run `crash rejoin` (loopback, 2 instances, guest killed right after it reached the host's world,
`join_timeline.py --from '1:Launching Bloodborne:2'`):

| Step | Time | Note |
|---|---|---|
| crash -> host link drop | ~0.6 s | TCP RST from the dead process |
| guest restart -> party link | 2.5-2.9 s | game boot |
| -> Continue pressed | +1.0 s | |
| -> world up -> load done | +9.0 +2.2 s | the guest's own save loads |
| -> bell rung, sign up | +1.3 +2.0 s | same as a normal join |
| sign up -> hold ends | 2-5 s | **the 26 s sign hold** (from the drop) |
| summon -> client -> in the host's world | +0.4 +2.1 +4.5 s | same as a normal join |
| **total, restart -> cooperators** | **~30 s** (crash -> cooperators ~34 s) | world up -> cooperators ~15 s vs ~4.5 s for a normal join |

So everything but the hold is the normal join plus the guest's own boot and load. Tried:

1. **Another UDP port for the restarted guest** (so the host's old transport connection would not
   match): failed. The host's game sends the summon data to the address it keeps for that online
   id (`send 3 -> 127.0.0.1:47961` after the guest moved to 47965); the restarted game never got
   it and the join took 60 s. Reverted.
2. **Expire the dead connection early**: when the link ends with *connection reset* (a crash,
   not a network hiccup), lower the live copies of the transport config's idle timeouts
   (25000 / 20000 ms, pattern `0x4d0, 3000, 5000, 6000, 25000, 20000, 200`) to 4 / 3 s for 8 s
   and shorten the hold to 5 s from then. The copies (mapped heap, e.g. `0x110dcbxxxx`) were found
   in 2 of 3 runs by a 35 s background scan of the resident memory, none in the third, and no
   run showed the connection actually expiring early (in the run where the patch came in time the
   25 s timeout ended at the same moment). Kept **off by default** (`BB_PARTY_NR_FAST_EXPIRE=1`).
   The hold is dynamic now: 26 s at the drop, shortened only once such a patch is in.

Result: crash rejoin stays ~30 s after the restart (~34 s after the crash). The floor without
the hold is ~22 s after the crash (boot + own load + a normal join).

### Transport RE so far (2026-10-11, experimental, all behind `BB_PARTY_NR_FAST_EXPIRE=1`)

- A call-site hook on 0xCDAC45 (`FUN_00CDABE0` -> `FUN_00CDB250`, the transport create) gives each
  transport as it is made: one at boot, one per hosted session (e.g. owner `0x110dbb8b40`,
  transport `owner+8` = `0x110dcb9380`). The socket states are named by 0xCD84E0
  (`SocketState_Disconnected` ... `Connected`, `Disconnect`, `Disconnecting`).
- The "config copies" the memory scan found (the 0x4d0 / 3000 / 5000 / 6000 / 25000 / 20000 / 200
  run) are **not live**: they show up only after the session ends (at `transport + 0x7c` of the
  freed session transport), and hardware data breakpoints on them (`BB_PARTY_NR_WATCH=1`) were
  hit only by our own scanner, never by game code. So lowering them could not work, which explains
  the earlier runs.
- Next: find where the live transport keeps the idle timeout or the per-connection last-receive
  time (the 0x4d0 run is not in the session transport's first 16 KiB; a search of the transport,
  owner and factory objects for the single values is built in but was not run), then the
  transport's vtable (factory `param_3->vtable[0x10]`).

### 2026-10-12: live transport fields, title join after a crash

- The session transport (owner + 8 from the create hook) keeps the timing block at
  **+0x84..+0x94** (5000, 6000, **25000**, 20000, 200) and again at +0x254c..+0x255c; its
  connection array is at +0xd0 (count +0x6c, 0xc0 bytes each) and every element points back to the
  transport. FUN_027163B0 (the transport reset) writes those defaults. A data breakpoint on +0x8c
  during a session and through a guest crash saw **no game read** until that reset, so the
  per-tick idle check does not read this field: lowering it does nothing (still experimental, off).
- **Regression found and fixed:** 0.5.5's title join let a crashed guest's restarted game ask for
  the host's room at its title while the host's game still held the dead connection: the join
  failed after 20 s and the guest fell back to Continue (crash rejoin **41 s** after the restart).
  The host now answers `title_wait` while any sign hold runs (`FromApi::hold_left_ms`) and sends
  the room when it ends. Live, 2 runs: restart -> in the host's world **28.4 / 28.5 s** (crash ->
  joined ~32 s); the guest goes straight from its title into the host's world (no own-world load).
  Screenshots of both games at the crash and the rejoin: no frozen character or stray HP bar after
  the game's own drop (~6 s after the crash).
- Tried and kept **off** (`BB_PARTY_CRASH_LEAVE=1`): ending the crashed member's room membership at
  the TCP reset (removes its frozen character at once); the host's game froze ~20 s later in both
  runs.

### 2026-10-12: the host releases the crashed guest's game connection (H2)

The wait was the host's game: after the crash it kept sending to the dead guest over its
transport connection until the connection's own idle check (~8 s to `cooperators 1 -> 0`, the
session left ~17 s after the crash) and the 26 s sign hold kept the restarted guest at its title
(restart -> Matching2 JoinRoom **20.5 s**).

Static RE (Ghidra) of the NexusRevolution transport (0x138 bytes, made by FUN_027157e0; vtable
0x52c51e0 udpp2p, 0x52c5160 udp): a pre-allocated connection pool at +0xd0 (count +0x6c, 0xc0
bytes each, set up by FUN_02716010), a hash of in-use connections at +0x100 (buckets +0x108), the
game socket at +0x120. vtable +0x20 (FUN_02716930) takes a connection for an address; vtable +0x28
(FUN_02716a40) **releases** one: unhashes it, resets it (FUN_0271a280) and calls the owner's close
callback (conn +0x40 with +0x50). A connection is in use while byte +0x2c is set; its key (live
dump): +0x20 IPv4 address and +0x28 port (host order), +0x2a vport.

Fix (`party_nr_drop.h`, `coop::b2::NrDropSilentPeers`): when a member's party link ends with
*connection reset* (a crash), 1.5 s later the host, on the game's own network thread (the shim's
`sceNetEpollWait` / P2P receive; never from a send, which runs inside the transport's update),
releases every in-use connection whose source has been silent since then (the other members
keep sending; the restarted game needs >3 s to send again), and ends the sign hold 1 s later.
`BB_PARTY_NR_DROP=0` turns it off.

Live, crash run each (loopback, guest killed just after it reached the host's world,
`join_timeline.py --from '1:Launching Bloodborne:2'`):

| | before | after |
|---|---|---|
| crash -> host's game drops the guest (`cooperators 1 -> 0`) | 8.0 s | 3.5 s (released 2.0 s after the crash) |
| guest restart -> title join (Matching2 JoinRoom) | 22.6 s | 2.8 s |
| **guest restart -> host cooperators 1** | **28.4 s** | **12.4 s** (crash -> cooperators ~16.5 s) |

What is left is the guest's own boot (~2 s), the session join (~1 s) and the ~7 s from its
Continue to the host-world loading screen, then the load.

## Open

- Done 2026-10-12 another way (above): the host releases the dead connection instead of
  shortening its idle timeout.
- A lower transport idle timeout (0xcdb309 25000 / 0xcdb314 20000 ms) would end the crash hold
  sooner, and the host would notice a crashed guest sooner too (today ~16 s). Not done: it is also
  the tolerance for real network hiccups.
- Host crash rejoin was not measured; the guest's link reconnect cap is now 2 s.

## 2026-10-11 profile (flowlive, after the pre-ring spacing fix)

`tools/mp/flow_live.py world --count 3` (both start straight in the party game, pre-made
characters, lowest-cost settings, 3 games on the machine), `join_timeline.py`:

| Step | Time |
|---|---|
| guest in world (load done) -> rings | 1.2 s (pre-ring spacing 0.8 s per side try + 0.3 s; was 6.9 s with 30 ticks per try at a low frame rate) |
| rings -> `summon_messenger/create` | 2.0 s (`Party: Fast bell sign`) |
| create -> `guest_invite` | 0.5 s |
| invite -> `JoinRoom` | 0.8 s |
| `JoinRoom` -> client (6) | 1.8 s |
| client (6) -> loading screen | 1.2 s |
| loading the host's world | 4.3 s |
| **guest in world -> in the host's world** | **11.8 s** (50.53 -> 62.30) |

What is left for "instant" joining: the 2 s bell timer (0.5 s broke 2 of 6 joins, above), the
transport's first connect (1.8 s), the guest's prologue / effect (1.2 s) and the load of the host's
world. A guest already in the host's map still loads it (split travel removes same-map loads,
docs/party/split_travel.md when it lands); Continue still loads the guest's own world first (the
sign is put up from there).

## 2026-10-11 (later): shorter transport retry and prologue

`Party: Fast join` now sets the NexusRevolution first retransmission timeout to 300 ms (was 1000;
0xcdb2e8) and the guest's self-prologue to 0.1 s (was 0.5; 0x196d3a8). Live (flowlive, 2 players):
`JoinRoom` -> client (6) 0.93 s (was 1.8), client (6) -> loading screen 0.80 s (was 1.2). In the
same run the bell -> `summon_messenger/create` took 7.6 s instead of 2.0: the waiting guest had the
item catch-up's key-item cards on screen (the game's full-screen card for a badge / key item), which
the sign sender waits behind - the items catch-up (party_items) should give key items without the
card, or before the bell.

Not done yet: Continue straight into the host's world (one load from the title), and the 0.5 s
bell timer race (2 of 6 joins stalled before; not reproduced or understood yet).

Also: a guest that is not in the host's world no longer follows the host's death or a host travel
within the map it already stands in (party_travel.cpp; live: a waiting joiner loaded for the host's
lamp warp and death in its own m24_01). Two-player Leave / Join party: the host had no load at the
guest's join, leave or re-join (one load in the whole run).

## 2026-10-11 (flowlive): the "item cards" and the failed join

* The full-screen item cards seen on guests (Hand Lantern, Monocular, Sword Hunter Badge, Bastard
  of Loran, Blood Gem Workshop Tool) are **the game's loading-screen item tips**, not the item
  catch-up: they show exactly while `Party: loading screen up` -> `gone`, a different item each
  load, and the item-get probe (`BB_PARTY_ITEMGET_PROBE=1`) had 0 entries in the dialog list at the
  same moments; the catch-up of that run only skipped lots (`already given`). Nothing to fix. (The
  bell -> sign delay of one run coincided with a loading screen of the waiting guest, which the
  party_travel fix below removed.)
* The failed join (guest client -> leaving at once, BeginLeaveSession from +0x178C118 = the session
  event handler 0x178B5E0's event 0x1E: Lua `OnBeKickOut` / `OnJoinClearedRoom`) followed a load
  of the *waiting* guest for the host's death travel. With that load gone (a guest outside the
  host's world no longer follows the host's death or a same-map host travel) the same scenario
  (host killed by the boss, then the join) joined cleanly (`trying-to-join -> client`, host
  `cooperators 0 -> 1`). The B2 leave trace now also logs flow +0x84, GSM +0x1592 and flow +0xF4.

## Continue straight into the host's world (one load): design (prototype below)

What the engine needs for a host-world load (b2_seamless.md 1.3, split_travel.md 1): the network
flow in state 3 (`+0x84`), which the MoveMapStep ctor 0x1937570 turns into host-world mode
(GSM +0x1592 = 0), a session (room + NexusRevolution session client, role 6) and the host's
packets 0x10 / 0x11 for the overlay and the enemy state. B2 re-joins get there from inside a
session (state 4 -> 1 -> 3); a fresh join gets there only through the summon: the guest's sign
(posted by its bell from its own world) -> the host's summon data -> JoinRoom -> client -> state 1.
A title-time join would have to (1) create the guest's sign / summon request without a PlayerIns
(the sign sender object 0x18A9050 and SendSign 0x1901320 read the local player), (2) take the
host's summon data and JoinRoom at the title (SprjSessionManager runs at the title in online mode,
but the summon step machine 0x14B5E10 is a world task), (3) enter state 1, take the snapshot, then
let the title's Continue load run as the state-3 load. Step (1) and (2) are the unknowns; the
cheaper path is to keep the own-world load short (it is the save's own load, 4 s here) and ring at
once (0.3 s after the world is steady today). Left for a dedicated RE pass.

## 2026-10-11 (flowlive): title join RE pass and prototype (`BB_PARTY_TITLE_JOIN=1`, off by default)

RE (our offsets):

* The guest's join is not tied to the sign once the room data is in hand. The summon message
  handler 0x14BA340 (FrpgNetMan +0xC50 = the summon manager; own signs in its +0x158 list) only
  stores the host's room data in the sign entry (+0xB8, state +0xC8); the in-world net task
  0x1947310 -> 0x1872360 -> 0x14BB200 picks that entry up, reads `[u32 -> GSM +0x16F0]
  [u32, low byte -> GSM +0x16F4 (presentation slot)]` and the session descriptor (0xC8FBF0 ctor,
  0xC90DD0 read; +0x28 name, +0x60 key, +0x68/+0x70 blob), then calls **0x1789010(flow, room)** ->
  SprjSessionManager 0x1ED0670 (role 4, JoinRoom). It also writes the return spot (GSM
  +0x14A0..+0x14C0 = the sign spot) and the summoned placement (+0x14C4 host map, +0x14D0 pos,
  +0x14E0 rot) and FrpgNetMan +0xA98.
* The host writes the same descriptor from its own session (0xC8F500(SessionMan+0x18, +0x38, 1),
  copy 0xC90270, 0xC90B00 into a DLMemoryWriter: vtable 0x53B2C80, +0x10 buffer, +0x18 capacity,
  +0x30 end).
* SendSign 0x1901320 / the sign sender 0x18A9900 / 0x14B9180 need the PlayerIns (vfunc +0x1C8,
  chr +0x3F8, position): a sign without a world is not practical; the room data does not need it.
* The flow update 0x1789B60 (float dt, flow) runs the SessionManager update 0x1ED0B20 and the
  packet handler 0x1783160; it is called only by the in-game net task 0x1947310 and three load
  steps (0x1962940, 0x1B39480, 0x195B920) - not at the title, so a title join must run it itself.
* The host-world switch is the MoveMapStep ctor 0x1937570 (built by step 0x1929220): flow +0x84 == 3
  -> GSM +0x1592 = 0, FrpgNetMan +0xB14 |= 8, GameData +0x5D8, SetLoadMode(1) (overlay kept); any
  other state clears the overlay groups (0x13CF9E0). The guest's Lua `SummonReloadStart`
  (0x1389440 / 0x1389620) runs the own-world serializer 0x131E5C0 (+0x84 = 3) before its warp.

Prototype (party_b2.cpp, "Title join"): the guest's AutoContinue holds Continue after the FROM
sign-in and sends b2 `title_req`; the host answers (once it has a room) with the serialized
descriptor (head = slot, session-type table 0x553D75C + slot * 0x80) and its stage / position; the
guest reads it, joins (0x1789010), runs 0x1789B60 at the title while the game does not, and once
the host's snapshot is applied (flow 2) writes flow 3 and the summoned placement, then presses
Continue so that the save's first load is the host-world load. Any failure (no answer 45 s, no
snapshot 20 s, refused join) leaves the session and continues as before (own world, then the bell).
`BB_PARTY_TITLE_JOIN_PROBE=1` logs the host's descriptor at an ordinary bell join next to the
guest's `summon message ... (head ...)` line for comparison.

Open (why it stays off): not yet verified live (the 2-client run was still queued at the 0.5.2
wrap-up); unknowns are whether the Continue save load keeps the overlay applied at the title,
the own-world snapshot / return spot (none at the title: going home loads the save's own spot),
and the GameData rows of the host member across the save load.

Cleric Beast at the lamp after the host's respawn (screenshot of the a11_mortal run): a test
artefact. The A11 fixture lit lamp 2412952 (Great Bridge, (-124.8, -26.9, 64.3)) and cleared the
boss flags (`BB_PARTY_PROGRESS_TEST_CLEAR`); that lamp stands in the Cleric Beast's arena
((-136.5, -26.6, 52.8), 16 m away), so the respawn put the host next to a live boss. In the game the
lamp only exists after the boss is dead. No bug.

## 2026-10-12 (flowlive): title join live (2 clients, mp2 runs 2-12), still off

* Setup: the "path not found" exits came from a setup run with another Python (msys) on PATH: the
  root path hashed to another port range, so the guest's saved host address pointed nowhere. Runs
  now use a fresh root and `BB_MP_BASE_PORT`.
* The host has no room until it summons a sign (the bell alone opens none): on a title request it
  now creates one (0x1ECFCD0(float [0x49271C8], SessionMan, flow +0xC), as the flow's event 0x1E).
* The descriptor head is `ffffffff 07000000` (type 7, a co-op summon; the guest's summon message
  of an ordinary join has the same head and the same 209 bytes).
* 0x1789010 panics at the title (SprjLuaEventMan does not exist there): the guest calls 0x1ED0670
  directly.
* Result: title -> JoinRoom -> client in 0.9 s, the host's snapshot in 0.03 s more, **one load**
  into the host's world (loading screen 5 s; host reply -> in the world 7.5 s), the guest beside the
  host (in-world warp after the load; with GSM +0x1521 set the summoned placement worked too), the
  guest sees the host's chr (guest `cooperators 0 -> 1`, session type 7 written after the first sync).
* **Blocker: the host never builds the guest's chr.** Its task for the guest stays at step 3
  FirstSyncCompleteWait (no packet 0xC from the guest; once, run 7, it got through); the host
  watchdog then ends the session after 60 s. The guest then shows "Session lost" and stays in the
  host-world copy (no own world to return to: GSM +0x1592 stays 0), so its bell posts no sign and it
  never re-joins (the run the user saw). Next: the guest's 0xA / 0xC exchange after a title join
  (0x178D9C0 sends 0xA to the flow's pending members at the load end) and a home load when a title
  join's session ends. Until then `BB_PARTY_TITLE_JOIN` stays off.

## 2026-10-12 (flowlive, later): title join finished, on by default

* **The host's chr of the guest** (task stuck at FirstSyncCompleteWait): the guest sends its
  first-sync packet 0xC (and the 0x168 record) only with its own GameData +0x10 set; a summon
  load leaves +0x10 2, +0xA4 1 (white phantom), +0x5D8 7 (session type) (read live from a vanilla
  join). The title-joined guest writes them at its arrival; the host asks for its data (packet 0xA
  to the member, 0x178D9C0's packet) while the task waits. Result: host `cooperators 0 -> 1`,
  "Co-operator" / "Beckoner" tags, "Entered the world of ..." banner.
* K5 (Lua OnMatchingCheck: a member's chr missing) left the session 0.1 s after the arrival in one
  run: quiet for 15 s after a title join's arrival.
* **Home**: the guest's own world was never loaded (no own-world snapshot); the game's return load
  fell back to the title ("Failed to acquire Bloodborne server information"). A title-joined
  guest's send-home (OnLeave_Limit 0x138A51C, its Leave or a lost session) now leaves the session
  and takes it home with a lamp warp to its own last lamp (GSM +0x1528, with GSM +0x1592 = 1 and
  the same-map no-load shortcut off): an ordinary own-world load (`apply gate open: own world`).
  The return spot GSM +0x14A0 / +0x14B0 / +0x14C0 is that lamp too.
* Handshake: the host answers `title_ack` at once; a guest without it within 6 s (an older host,
  or `BB_PARTY_TITLE_JOIN=0` there) presses Continue the old way.
* Live (2 clients, mp2, 2026-10-12, `BB_PARTY_TITLE_JOIN=1` + guest Leave 13 s after its world
  came up): 4 runs in a row: title -> client 0.9 s, host reply -> in the host's world 7.6-7.8 s
  (one loading screen), host `cooperators 0 -> 1`, both screenshots show the two hunters side by
  side with tags, Leave -> one load into the guest's own world (Iosefka's Clinic lamp). The host
  had only its own first load. The default-on build (switch unset, with the ack) is not run live
  yet (the game queue was not available to this agent at the end).

## 2026-10-12 (flowlive, 0.5.4 follow-up): Continue party game, default on again

The 0.5.4 default-build check went through the title's *Continue party game* (flow_live
`continue`): there the test new-game driver (`BB_TEST_NEWGAME=party`) presses the first Cross,
not AutoContinue, so the title join never ran and the guest took the old bell path (the run ended
before that join). The driver now waits for the title join too (a `BB_PARTY=join` process). Live,
absolute root `flowlive/mp2`: 3 runs in a row with `BB_PARTY_TITLE_JOIN=1` and one with the switch
unset: each time one loading screen into the host's world (asked -> in the world 7.9-8.2 s), host
`cooperators 0 -> 1`, screenshots 8 s after the arrival show both hunters side by side with the
Co-operator / Beckoner tags (in one run the guest's window still showed the loading tip at 8 FPS
while the host already saw it). Default on again; `BB_PARTY_TITLE_JOIN=0` turns it off.

## 2026-10-12: a host that is not up yet

A guest whose title join gets no room within 10 s (the host still at its title or loading) sends
`title_cancel` (the host drops its request) and Continues the usual way: own world, then the bell
join once the host is up. Live (host Continue held 30 s with the test switch
`BB_PARTY_CONTINUE_SETTLE=30`): guest asked at 5 s, fell back at 15 s, own world loading at 16 s;
the host loaded at 35 s; bell join `trying-to-join -> client` at 50 s, host `cooperators 0 -> 1`.

## 2026-10-12: title join from another map (Dream -> Central Yharnam), own save spot

* Continue loads the map of the save's spawn **record** (GSM +0x15B0 pos, +0x15C0 rot, +0x15E0
  map), not the forced placement: with only the forced placement set, a guest whose save was in
  the Dream loaded the Dream in host-world mode (hostdeath agent's finding: then it left and
  gathered). The title join now writes, every frame between Continue and the load start, the
  record and the forced placement (+0x14F0 / +0x1500 / +0x1510, +0x1520) to the host's map beside
  the host, and GSM +0x1521 = 1 so the summoned placement (+0x14C4 / +0x14D0 / +0x14E0) applies
  (without it the guest stood at its Dream coordinates inside the host's map).
* The save's own spot (the record's value before it is overwritten; the forced placement holds
  none at the title) is kept and written back at the arrival into the forced placement and the
  return spot, so the guest's save keeps its own spawn.
* A host still at its title: the host sends `title_wait` every 2 s (at its title, or continuing /
  loading / in the world); the guest waits 10 s for a host idle at its title, 40 s for one on its
  way (the host's AutoContinue), then Continues the usual way.
* Live (2 clients, mp2): guest save in the Dream (m21_00, (-9.2, -6.8, -19.4)), host in Central
  Yharnam (Iosefka's Clinic lamp): one loading screen each, the guest in m24_01 at the host's
  side (type 1/7), "Entered the world of vp797-rz2z" and the Co-operator / Beckoner tags on the two
  screenshots; host `cooperators 0 -> 1`. Next run without title join: the guest's Continue
  started in the Dream (its own spot).

## 2026-10-12: first 2-PC LAN session, 63 s bell summon (0.5.7)

Host log (`launcher.log`, session 18:41:58): host -> `host` at t 78.0 s, `cooperators 0 -> 1` at
t 141.2 s. Where the time went (host side; the guest log was not available):

| Attempt | Host | Result |
|---|---|---|
| 1 (t ~78) | the guest's sign was already up; summon at once, signaling resolved, vport 40 handshake complete | the host's game never sent the summon data (no 267/271-byte message); it gave up after its 15 s connected-peer window (`DeactivateConnection`, member 0xff00 = never joined) |
| 2 (~30 s later) | the guest's 30 s sign resend, summoned at once | same failure |
| 3 (~60 s after 1) | same | summon data sent, JoinRoom, `cooperators 1` ~3 s later |

So ~60 of the 63 s were two failed attempts, each costing the 15 s window plus the wait for the
guest's next sign (the host's game summons a sign id only once). The failure itself (host sees the
guest as not connected although the transport handshake went through; the crossing 54-byte
messages differ from the good attempt) is not understood yet and needs a guest log.

Fix (both host side, loopback-tested; the failure path itself could not be reproduced locally):

- `np_signaling.cpp` `sig_deactivate` -> `FromApi::renew_signs`: when the host's game drops a
  peer it was summoning that never joined, that member's signs on the board get new ids and the
  board change zeroes the game's poll countdown: the next attempt starts ~0.5 s after the failure
  instead of at the guest's next sign (up to 30 s).
- `Party: Fast join` tries 30 -> 16 (0x14b4975, 0x14bb067): the connected-peer window is 8 s
  instead of 15 s (a good LAN/loopback summon passes it in < 1 s; relay fallback is 2.5 s).

Expected for the logged session: 2 failures cost ~2 x 8.5 s instead of ~60 s (summon ~20 s after
hosting). Local 2-instance run after the change (Continue party, title join): host `host` t 13.5 s,
`cooperators 1` t 18.2 s (unchanged, no regression).

## 2026-10-13: direct summon delivery (`BB_PARTY_DIRECT_SUMMON`, on by default since 0.6.0)

Retest (no menu walks, existing characters): bell 35.36 s, room joined 3.0 s after the bell, host cooperators 0 -> 1 at 47.14 s (11.8 s after the bell); the host stayed connected ~77 s and shut down cleanly (no crash or hang dump). Turned on by default.

Under simulated DSL + NAT (`BB_NET_SIM=dsl,nat=1`: 80 ms ± 20 one-way, 2 % loss, link over the
reliable UDP stream after a guest-code punch, 2026-10-13): rang 19.54 -> room answer 19.95 (0.4 s)
-> item delivered 21.58 -> client 22.68 (**bell -> client 3.1 s**, as on loopback) -> host
cooperators 1 at 27.41 (7.9 s after the bell); 173 s stable after, 0 drops, no sign fallback.
Title join on the same profile: link up 13.5 s, client 20.7 s, host cooperators 1 30.6 s.
Details: network_robustness.md §3c.


Ported from droogie/bbhost (`engine/summon_invite.cpp`, GPL-3.0+): `party_dsummon.{h,cpp}` (wire
format, type-1 payload, decisions; `party-dsummon-test`) and the game side in `party_b2.cpp`.
The guest sends `dsum_req` when its director rings the Small Resonant Bell; the host (in its
world, not full, its game not summoning a sign in the last 20 s, no crash hold) creates its room
if it has none (0x1ECFCD0, as the title join), serializes it (`SerializeRoom`, head type 7, 209
bytes) and answers `dsum_room` with FrpgNetMan +0xa7c area / +0xaac position, holding the guest's
signs (20 s) so its game does not summon the same guest by sign. The guest, on the game thread,
waits for the bell's own passive entry (SummonStepManager [FrpgNetMan+0xc50] +0x158, entry
+0xc8 == 0), builds the item with the game's wrapper ctor 0x1486a10 (vtable 0x5322570, verified:
23 sites `lea 0x5322560; add 0x10`) and calls handleIncomingItem 0x14ba340 (checked as the
manager's vtable +0x10). `dsum_done` ends the hold (a failure renews the signs at once).
bbhost's level field (FrpgNetMan +0xaa8) holds a float on our 1.09; level is sent as 0.

Live (one session, loopback, `continue`, both in m24_01, title join off): guest rings 69.73 ->
host room 69.94 -> item delivered 71.77 (the entry appeared with the 2 s bell sign timer; state
1) -> `trying-to-join` 71.80 -> client (6) 72.73 -> loading screen 73.53: **bell -> client 3.0 s**
(sign path: rings -> create 2.0 + create -> invite 0.5 + invite -> JoinRoom 0.8 + JoinRoom ->
client 0.9 = ~4.2 s). Then the host's process ended at 73.5 s (no log line; the guest showed
"Connection to the host lost"), so the arrival / host `cooperators 1` was not seen and it is not
known whether the host crashed or the run was stopped. Kept off until a run shows the arrival;
the default (sign) path is unchanged.

## 2026-10-13 (a5): direct summon from another map does not load the host's map

One session + one re-run, loopback, save `save_backup/savedata_20261010` (Dream) for both, guest
title join off (`--ienv 1:BB_PARTY_TITLE_JOIN=0`), guest started 50 s after the host.

* Run 1 (host in the Dream too): guest world steady -> bell 1.4 s, `dsum_req` -> room 0.2 s ->
  client 72.9 s, loading screen at once (one load); catch-up: own save past the prologue.
* Run 2 (host moved to Central Yharnam by `BB_PARTY_TEST_START_LAMP=2412951`, guest rings from the
  Dream without the gather: `BB_PARTY_GATHER_DSUM=1`): `no gather: the direct summon loads this
  guest into the host's map`, bell -> room 0.3 s -> type-1 message (host map 0x18010000) ->
  client 3.1 s after the request, but no load: 10 s later the guest is still in m21_00_00_00 at
  its own spot, `apart 1`, the host's chr not in its world (run ended there). The game's summon
  load evidently needs the joiner to be in the host's area (vanilla summons always are). So the
  gather (own-world lamp warp to the host's lamp, then the bell) stays the default: two loads.
* Next: after such a cross-map client join, start the B2 arrival load into the host's map (the
  split travel's follow path) so the joiner has one load; or keep the title join (already one load)
  as the way to join from another map. Screens black (desktop locked): verified by logs.

## 2026-10-15 (parab): one-load cross-map join (A5), default on

Guest save in the Dream (title join off), host in Central Yharnam (`BB_PARTY_TEST_START_LAMP=2412951`).
The direct summon joins the host's room from the guest's own map (`BB_PARTY_GATHER_DSUM`, now on),
then the B2 re-join loads it into the host's map (`AloneWhy::Join`, the host's lamp nearest the
host, then the join-beside warp):

| Step | a5b | a5c |
|---|---|---|
| guest world steady -> bell / `dsum_req` | 0.3 s | 0.7 s |
| request -> client (6) | 3.0 s | 3.1 s |
| client -> arrival re-join | 1.0 s | 1.0 s |
| the one loading screen | 6.2 s | 6.1 s |
| world steady -> beside the host (0.5 m) | **12.3 s** | **12.8 s** |

Pitfalls found live and fixed the same day:

* The game's own summon load (Lua OnBeJoinStart, K6) can start 0.8 s after the client role when
  the host answers fast (a11e, 1.1 s): it builds the guest's OWN map as the host's world (the
  Dream in host-world mode). It is held by K6 from the join until the arrival re-join starts.
* Starting the arrival in the join's own frame (a5d, a11f, a11g) loaded the Dream too, with the
  join's queued snapshot packets dropped or kept: the arrival waits 1.0 s (the game takes the
  join's snapshot first), as in a5b / a5c.
* A guest that joined from its own world never got the summoned placement, so its GameData kept
  +0x5D8 0 (chr type 1/0): the host counted 0 cooperators and its watchdog ended the session 60 s
  later (a5c, a5e, a11h). After the arrival the guest writes the summoned guest's values
  (+0x10 2, +0xA4 1, +0x5D8 7) and re-broadcasts its record: a11i `cooperators 0 -> 1` 0.1 s
  after the arrival.
* A direct summon refused only to wait (A11: boss, cutscene) no longer makes the next attempt
  gather with a loading screen first.

## 2026-10-15 (parab2): the bell sign timer below 2 s, live (A4)

The direct summon hands the guest's game the host's room as soon as it is there (0.25 s after the
ring), but the game takes it only into its bell's own passive summon entry, which the sign sender
creates when its first-sign timer fires (2.0 s, `Party: Fast bell sign`). So the timer, not the
transport, is most of request -> client (3.0 s). Test switch `BB_PARTY_SIGN_DELAY=S` rewrites the
patched float at the 12 sites; below 2.0 s the host's sign board (`from_api` sign_get) holds a sign
back until 2.0 s after the ring, so the sign path keeps the timing it was proven with.

| Run (one session each, guest_home x7: leave, ring, join again; same map) | Joins | Ring -> client (6) |
|---|---|---|
| a4ds: direct summon, timer 0.5 s | 4 of 5 in 1.5-1.6 s (`joined the host's room (1.5 s after the request)`); the 5th stalled after client (6): no host-world load for 6 s (the test's next leave hit it in that state, the guest then sat in flow 2 for 55 s) | 1.5 s (was 3.0 s) |
| a4ds2: the same with 12 s between joins (the host's game long out of the last session) | 1 of 2: the 2nd stalled again after client (6) (flow 1 -> 2, no load for 12 s), then three attempts failed (`the bell's own summon entry did not come up, not even after a re-ring`) | 1.9-2.4 s |
| a4sp: sign path only (`BB_PARTY_DIRECT_SUMMON=0`), timer 0.5 s + the board hold | 7 of 7 (`1 too new: shown 1500 ms after it went up`, then `summons Hunter1 into room 100N` 1.9 s later) | 3.7-4.3 s (as with the 2 s timer) |

The stall is the symptom of 2026-10-11's 0.5 s sign-path runs ("the guest stopped after client
(6)"), now without the host's sign polling in the loop: a join that reaches the guest's game well
before ~2 s after its ring can stall; delivered >= 2 s after it, every live join so far went
through. The race is on the guest's side (its bell is "barely up"), not the sign board's. The
default stays 2.0 s; the switch and the board hold stay for the RE of the guest's flow state 1.

## 2026-10-15 (parab3): the 0.5 s bell timer by default, the flow-2 watchdog, the empty-room timeout (A4)

**What the guest's game does after client (6)** (Ghidra, `BB_PARTY_JOIN_TRACE=1` live): the
packet handler 0x1783160 applies the host's snapshot (0x19 stage, 0x11 world, 0x10 flags) in flow
state 1 and sets flow +0x84 = 2, then calls 0x1317780, which raises the Lua event
`OnBeJoinStart_<type>` chosen by the summon type at GSM +0x16F0 (-1 White, ..., 0 = no event at
all; the room head's first u32, written by the summon message 0x1872360). The Lua's
`SummonReloadStart` (0x1389440 / 0x1389620) loads the host's world (own-world serializer
0x131E5C0: flow 3, then the MoveMapStep) only when the presentation table (GSM +0x16F8) counts at
least 2 and the own chr is alive; otherwise it **clears +0x16F0 and returns**: the join sits in
flow 2, no load, until the session times out - the stall parab2 saw with the 0.5 s timer
(`client (6)`, flow 1 -> 2, no load). In every traced join the presentation count is 1 at the
client role and 2 thirty milliseconds later at the Lua start, so a snapshot that arrives before
the host's presentation entry is the likely trigger (not reproduced naturally in 24 joins).

**Fixes.**
* Flow-2 watchdog (guest, `party_b2` Tick, `BB_PARTY_JOIN_RESTART=0` off): flow 2, no load, 1.5 s
  after the game's Lua join start, presentation count >= 2 and the own chr alive: the summon type
  is put back and the game's own join start 0x1317780 runs again (twice at most per join).
  Live (a4t3, `BB_PARTY_TEST_JOIN_STALL=2` swallows the 2nd join start and clears the type, as
  SummonReloadStart does): `join trace +0.27 s: flow 2, type 0`, `A4: the join sits in flow 2 with
  no load 1.5 s after the Lua join start (type 0, presentation count 2 now): the game's join start
  again (1), type -1`, then flow 3 at +1.60 s, the load, flow 4 at +3.46 s: the join went through.
* Host empty-room timeout (a4t2, live): a host left alone counts its room down (flow +0xB8; at 0
  0x1789B60 raises `OnRoomTimeOut` and the game leaves the room, ~10 s after the last member left).
  A room served 27 ms before that was gone when the guest's JoinRoom came (`np: join_room failed:
  Room not found`), an 8 s failure and the bell's 30 s fallback. A served room now gets 10 s more
  (`direct summon: this host's empty room had N s left ...`; not hit again live).
* The bell's first-sign timer is 0.5 s by default (`BB_PARTY_SIGN_DELAY=2` restores 2.0 s); the
  sign board's hold keeps the sign path at 2 s from the ring.

| Run (one session each; same map, title join off; guest_home: leave, home load, ring, join) | Joins | Request -> client (6) | Client -> load / in the world |
|---|---|---|---|
| a4t1: timer 0.5 s, guest_home@8 x8 | 8 of 8 | 1.5 s every time | load 0.8 s, flow 4 at +2.1 s, load gone +5.1-5.6 s |
| a4t2: timer 0.5 s, guest_home@1-20 mixed x8 | 7 of 8 (the miss: the host's empty-room timeout above, not the timer) | 1.5-1.8 s | the same |
| a4t3: timer 0.5 s, guest_home@4 x8, 2nd join start swallowed | 8 of 8 (the forced stall recovered by the watchdog) | 1.5-1.6 s | load gone +3.7-3.8 s |

So a guest's ring -> beside the host in the same map is now ~5.3 s (1.5 s to the client role,
~3.8 s to the end of the load), was ~7 s with the 2 s timer.
