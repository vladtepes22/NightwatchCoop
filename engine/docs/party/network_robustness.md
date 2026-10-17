# Network robustness: internet play, latency, loss, outages, NAT

Party co-op runs serverless peer-to-peer. Before 2026-10-11 every live test ran over loopback at
0 ms. This page covers the network simulator, the runs under realistic profiles and what a host
must do about NAT.

## 1. Transports

| Channel | Protocol | What it carries | Reliability |
|---|---|---|---|
| Party link (`party_link.h`) | TCP to the host's party port (default 9307); or a reliable UDP stream on the party UDP port (`party_rudp.h`) when TCP does not get through | Roster, RPC (the guest game's FROM API / matchmaking), our EVENTs: travel, `flags`, `flag_snapshot`, `world_groups`, items, story, lamp requests, `lamp_lit`... | TCP (in order, retransmitted), plus a per-stream event cursor: unacked events are replayed after a reconnect and de-duplicated. Lost = no frame for 10 s; the slot is kept 60 s; the guest reconnects with backoff 1-8 s. |
| Game P2P (`net_socket.cpp`, `party_udp.h`) | UDP on the same port number | The game's own session traffic (players, enemies, damage, the join data) | The game's own protocol over UDP. Guest-to-guest pairs hole-punch, falling back to the host relay (PeerPaths). |

So our own events are reliable and ordered by construction. The open question was whether the
game's UDP session and our loopback-tuned timers survive real latency, loss and outages.

## 2. Simulator (`BB_NET_SIM`, `gpu/shim/net/netsim.h`)

`BB_NET_SIM="lat=80,jit=20,loss=2,dup=0.5,reorder=1,seed=7"` (or a preset `lan`, `wifi`, `dsl`,
`mobile`, `bad`, optionally followed by overrides). `lat` and `jit`/`jitter` are one-way ms;
`loss`, `dup` and `reorder` are percentages.

- **UDP**: a delay line in front of every datagram the game sends (`Queue`). It delays, drops,
  duplicates and reorders. A destination's datagrams stay in order unless reordered.
- **Party link (new)**: on the receiving side, every TCP chunk waits until its due time
  (`StreamDelay`): lat ± jitter, never ahead of the previous chunk. A "lost" segment costs a
  retransmission delay of max(200 ms, 3 × lat) instead of a drop. An outage holds everything
  until it ends. `last_rx` follows delivery, so a held link is silent to the timeout logic, as a
  real one would be.
- **Per direction**: each instance applies the simulator to what it sends (UDP) and receives
  (TCP). `--ienv I:BB_NET_SIM=...` gives an asymmetric link.
- **Outages**:
  - `outage=MS,every=MS` repeats an outage periodically.
  - `outage=MS,at=MS` runs one outage that long after process start.
  - The director test steps `host_outage=MS@S` / `guest_outage=MS@S` (`netsim::outage_now`) start a
    total outage on that instance at a scripted point. They need the simulator on (`lat=0` will do).
  - `BB_NET_SIM_OUTAGE_FILE=<path>` (test aid, 2026-10-14): `outage_now(MS)` whenever that file's
    text (`MS` or `MS tag`) changes. One file shared by every instance of a local run gives a
    **symmetric** total outage on all of them at the same moment. A `host_outage` step alone holds
    only the host's received link bytes: the host then closes the silent link after 10 s and the
    guest sees that close, which a real outage would swallow.
- **Loopback only**: only 127.0.0.0/8 destinations or peers are simulated, unless `any=1`. A
  stray test setting never slows a real internet session.
- **Tests**:
  - party-net-test covers parsing, the one-shot outage, loopback-only, StreamDelay (order,
    retransmits, outage hold) and outage_now.
  - party-link-test runs a full link at 60 ms ± 20 with 5 % loss and a 1.5 s outage. The guest
    joined in 0.33 s; 300 events arrived in order, none duplicated or missing; ping was 123 ms;
    the link never dropped.

## 3. Live runs (2 instances, loopback + simulator, 2026-10-11)

Scenario (`agents/overlay/netrun.sh`):
1. The guest rings the bell in the Dream.
2. The host lamp-warps to Great Bridge (B2 party travel).
3. Both move to the Cleric Beast; the guest dies (respawns at the host's lamp in the host's world).
4. The host kills the boss.
5. The flag mirror, C2 progress and C3 items run throughout.

`BB_PARTY_FLAG_DUMP=1` gives the parity check: host pool vs guest overlay.

| Profile | Bell -> client | B2 travel (host warp -> guest arrived) | Guest death -> respawned in the host's world | Host death -> party follows | Boss clear | Link | Mirror parity at the end |
|---|---|---|---|---|---|---|---|
| loopback 0 ms (earlier runs) | ~5 s | ~15 s | - | - | ok | - | 0 host-only bits |
| `lat=60,jit=10,loss=1` | 4.9 s | 14.7 s | (death step had no effect after the boss) | - | ok, guest Insight 6 -> 9 | ping ~125-145 ms, 0 drops | - |
| `lat=150,jit=30,loss=3` | 5.8 s | 18.1 s | 23.6 s from the death (8 s death hold, then 15.6 s load) | 22 s (the host died to the boss) | ok, guest Insight +3 | 0 drops | 0 host-only bits (39 = 39 groups) |
| `lat=60,jit=10,loss=1` + **5 s total outage** on both sides mid-session (in m24_01, ~30 s after B2) | 11.2 s | 8.8 s from the snapshot (load) | 33.7 s after the 8 s death hold | - | ok, after the outage | **0 drops / reconnects**, still client, cooperators 1 | 0 host-only bits (only session flag 2010, masked) |
| `...,outage=5000,at=95000` (the outage hit during the bell join) | joined (host cooperators after 27 s instead of ~8 s) | 19.1 s from the snapshot | ok | - | - | 0 drops | - |

Findings:
- Nothing broke at 150 ms / 3 % loss. Join, B2 travel and gather, deaths, boss clear, events and
  the mirror all behaved as on loopback, only slower by the round trips.
- A 5 s total outage mid-session (UDP dropped, link held) left the game session (client role,
  cooperator count) and the link intact. Afterwards the boss clear and the guest's death respawn
  ran normally; no desync (mirror parity), no crash.
- An outage during the join slows the summon (host cooperators 0 -> 1 after 27 s instead of
  ~8 s) but completes.
- **Timeouts**: the link's 10 s lost timeout and 60 s slot keep, B2's 45 s snapshot wait and
  8 s death grace, and the host watchdog (stall 60 s, gone 25 s, wait 90 s, hard 240 s) all have
  room over 150 ms RTTs and 5 s outages. Nothing needed retuning. The fast-join patches shorten
  the game's own message intervals, not its timeouts; joins at 150 ms took 5.8 s.
- Fixed on the way: the guest-lamp diagnostics printed a line every second while a guest stood
  near a world lamp. They are now silent outside tests.

### 3a. Internet profile, current build (2026-10-15, 0.6.2)

Asymmetric profile, `BB_NET_SIM` per instance: host `lat=100,jit=30,loss=2,dup=0.5,reorder=1`,
guest `lat=150,jit=40,loss=3,dup=0.5,reorder=1` (roster ping 226-287 ms); one session each run
(`agents/parceh/runs/f1.sh`): title join, party travel to Central Yharnam, two host kills, host
weapon fight (`host_chr=0` + R1 taps), guest death, 5 s guest outage, host death, a kill, party
travel to the Dream.

| Run | Result |
|---|---|
| 1 | Join, travel, kills, guest death (revived in place), outage all fine. After the host's death load `join snapshot re-sent (0x19, flags FAILED, world FAILED ...)` (never seen at 0 ms): the guest waited 45 s for a snapshot, fell back to B1 (`BeginLeaveSession` from +0x13cdef6), the host's game left its session 6 s after the failed send (+0x178980d, the session event handler, as when a first sync never completes), the guest was summoned again 64 s after the death. |
| fix | `ResyncMember` reports the send result; a failed one is retried every 0.5 s for up to 15 s (`ResyncRetryTick`). |
| 2 | Every step with one loading screen per player: host cooperators 0 -> 1 17 s after start, travel 16 s, kills paid on both games (ForceDead + 3 real weapon kills paid by the game), guest death in place, outage, host death (guest arrived 20.8 s after it, snapshot sent first try), Dream travel, desync digests matching (21), census 0 respawned. The retry was not needed in run 2. |

### 3b. Simulated NAT, no port forward (2026-10-11, run `NAT sim: link over UDP`)

`BB_NET_SIM=lat=30,jit=5,loss=1,nat=1` on both, host has no TCP listener, the guest's code goes
to the host through `BB_PARTY_GUEST_CODE_FILE` (standing in for chat):

| Time (instance clock) | Host | Guest |
|---|---|---|
| +27.4 | | `guest code BBG1-0ZR0-000V-PPCD (UDP 127.0.0.1:47961)` |
| +28.3 | `punching towards a guest at 127.0.0.1:47961` | |
| +28.9 | `a guest connects over UDP` | `no TCP answer from the host; connecting over UDP`, `connected over UDP` (+0.07 s) |
| +29.1 | flag snapshot due | `connected: slot 1` |
| +44.5 | game P2P `punch Hunter1 ... peer heard after 500 ms` | same towards Hunter0 |
| +47.1 / +52.3 | `cooperators 0 -> 1` | `client (6)` (bell join) |
| +74.4 | | B2 travel: `arrived in the host world` |

So the link (events, RPC, mirror) and the game's own session both crossed the simulated NAT;
without the guest code the guest never connects (`test_udp_nat_link`, Bob).

Not covered here: a real NAT and a real ISP (no 2-PC test). The simulator also does not model
bandwidth limits (the party's bandwidth is small: ~180 KB/min UDP per guest in the runs above,
plus the 10 KB mirror image at join).

### 3c. Direct summon and title join under DSL + NAT (2026-10-13, 0.6.1)

The default join paths since 0.6.0 (title join and the bell's direct summon, `party_dsummon`)
had only run on loopback at 0 ms. Two runs, `BB_NET_SIM=dsl,nat=1` on both (80 ms ± 20 one-way,
2 % loss, 0.5 % dup, 1 % reorder; no TCP listener, guest code through `BB_PARTY_GUEST_CODE_FILE`),
existing characters, automated Continue:

| Run | Link (no TCP -> UDP) | Join | Host `cooperators 0 -> 1` | After |
|---|---|---|---|---|
| Title join (default) | guest `connecting over UDP` 13.0 -> `connected over UDP` 13.2 s, slot 1 at 13.5 s | Continue -> `client (6)` 20.7 s (game P2P punch heard after 500 ms) | 30.6 s | roster ping 172 ms; the run was ended by the central idle reaper at ~46 s |
| Bell, direct summon (`BB_PARTY_TITLE_JOIN=0`) | `connected over UDP` 10.1 s (0.15 s after the switch) | rang 19.54 -> host room created and sent 19.84 -> guest has it 19.95 (0.4 s) -> item delivered 21.58 (waits for the bell's own entry) -> `client (6)` 22.68: **bell -> client 3.1 s**, `dsum_done` ok at the host 22.78 | 27.41 s (**7.9 s after the bell**, guest loaded in) | **173 s stable** in the host's world (to the 200 s end): cooperators 1 throughout, roster ping 172-184 ms, 0 link losses / reconnects, 0 fallbacks to signs, clean shutdown on both |

Same as the loopback direct summon (bell -> client 3.0 s): the dsum messages ride the party link,
which is reliable (TCP or the reliable UDP stream), so a lost datagram costs a retransmit, not the
request; the 4 s answer wait (`kAnswerWait`) had 3.6 s to spare at 0.4 s, the 8 s entry/join
waits 6 s. No retry or timeout change was needed.

### 3d. No guest code: the DHT rendezvous under the NAT sim (2026-10-14, 0.6.1)

`BB_NET_SIM=dsl,nat=1` on both (no TCP listener on the host, unsolicited UDP dropped), **no
`BB_PARTY_GUEST_CODE_FILE`**, `BB_PARTY_RENDEZVOUS=1` (opt-in since 2026-10-14; the run predates
that and had it on by default), `BB_PARTY_DHT_BOOTSTRAP=127.0.0.1:46881` pointing at
`tools/mp/fake_dht.py` (one DHT node on loopback standing in for the public DHT). Title join with
existing characters, one clock for both logs:

| Time | Host | Guest |
|---|---|---|
| +17.4 | `NAT: cone (no open port)`, `rendezvous: looking for guests in the DHT every 5 s` | |
| +21.1 | | `NAT: cone; ... hole punching works (endpoint exchanged automatically)` |
| +22.6 | | `no TCP answer from the host; connecting over UDP` |
| +24.2 | | `rendezvous: announce done: ... stored at 1` |
| +27.5 | `a guest's endpoint 127.0.0.1:47601 found in the DHT`, `punching towards a guest` | |
| +27.7 | | the first UDP attempt (started before the punch) times out, retried |
| +30.2 / +30.5 | `Hunter1 rejoined (slot 1)` | `connected over UDP`, `network path: party link over UDP` |
| +32.1 | | title join: `client (6)` |
| +39.1 / +40.8 | `cooperators 0 -> 1` | `arrived 0.5 m from the host` |

Screenshots every 15 s: both hunters side by side in the host's world from 45 s to the end
(130 s), no glitch; the guest has the shared key items box open (nobody pressed OK). A first run
with the host polling every 10 s connected too (link over UDP 2.3 s after the announce), but the
guest's title screen showed *Failed to acquire Bloodborne server information* once at 20 s, while
its game waited for the link. The run then stopped at 28 s on its end markers, so the outcome of
that box was not seen. The host now polls every 5 s; in the second run the link was up 6 s after
the announce and no such box showed.
Not covered: the real DHT (public bootstrap nodes) and a real router. Both test instances stay
on 127.0.0.1.

## 4. NAT: what a host must do

- **Guests**: nothing. They connect out (TCP to the host, UDP to the host's port). Their own NAT
  keeps the mapping open, and the host answers to the observed address (the link's WELCOME and
  the STUN Binding Response at the host's port tell each guest its public address).
  Guest-to-guest game traffic hole-punches. Where that fails (symmetric NAT), it goes through the
  host relay automatically (`PeerPaths`; `BB_PARTY_FORCE_RELAY=1` forces it).
- **Host**, best case: one port reachable from the internet, **UDP and TCP, default 9307**
  (`BB_PARTY_PORT` / Party tab).
  1. **UPnP** (already implemented: `party/upnp_win.cpp`, Windows' own IUPnPNAT, on by default,
     `BB_PARTY_UPNP=0` off) asks the router to map both protocols while the game runs and removes
     the mappings on exit. UPnP itself causes no firewall prompt. When the router refuses, the log
     and the launcher say *"forward UDP+TCP 9307 to this PC"*.
  2. **Manual port forward**: if UPnP is off on the router, forward UDP and TCP 9307 to the host
     PC's LAN address.
  3. **Windows Firewall**: the first time the host listens, Windows asks once whether
     `bbport.exe` may accept connections. Allow it, on private and public networks, or the guests
     time out at *Connecting*. This prompt cannot be avoided without admin rights (a firewall
     rule needs elevation); it appears once per executable path.
  4. **The party code** carries the public address found by STUN (`BB_PARTY_STUN`, default
     Google's) or reported by the UPnP router, plus a LAN code for same-network guests. A fixed
     IP, DNS name or VPN address can be set as *Public address* (`BB_PARTY_PUBLIC_ADDR`).
- **Host without UPnP or a port forward: UDP hole punching with a guest code** (2026-10-11).
  1. The party code already carries the host's STUN-mapped public UDP endpoint.
  2. Each guest, on joining, finds its own public UDP endpoint of the party port by STUN and shows
     it as a **guest code** `BBG1-XXXX-XXXX-XXXX` (Crockford base32 of IPv4 + port + CRC-8;
     overlay Party page *Your guest code: copy*; also `user/party_guest_code.txt`).
  3. The guest sends that code to the host out of band (chat), the host presses *Add a guest
     code (paste)* (or `BB_PARTY_GUEST_CODES=...`, or a file named by `BB_PARTY_GUEST_CODE_FILE`).
  4. The host then sends small punch datagrams from its party port to the guest's endpoint every
     200 ms for 30 s. That opens the host's NAT mapping for that guest; the guest's own outbound
     packets opened its side. Both sides' NAT then pass the traffic (simultaneous open).
  5. The guest tries TCP first; after 1.5 s without an answer, or at once on *refused*, the party
     link switches to a **reliable UDP stream** on the same UDP port (`party_rudp.h`: SYN / DATA /
     cumulative ACK / FIN, 1180-byte segments, window 256, RTO from the smoothed RTT, 150 ms to
     2 s, keep-alive 1 s, 30 s idle limit). The link's frames, encryption, events and replay run
     over it unchanged. TCP simultaneous open was not used: Windows and most NATs handle it badly.
  6. The game's own UDP session uses the same port and thus the same NAT mapping, so it passes too.
  - Tests: `party-link-test` (`test_rudp`: clean, 10 % loss with 60 ms jitter, 2 x 300 KB in order;
    NAT filter without a punch fails, with a punch connects; `test_udp_nat_link`: a guest behind
    the simulated NAT connects over UDP in ~0.6 s, 200 events in order; a guest without a code
    does not). The simulator has a NAT mode (`BB_NET_SIM=...,nat=1`): inbound UDP is dropped
    unless we sent to that address:port first, and the host's TCP listener is off. Live run: see
    §3b (link over UDP 0.6 s after the punch started, bell join and B2 travel as usual).
- **No guest code needed: the automatic rendezvous (DHT), opt-in** (2026-10-14, 0.6.1,
  `party_rdv.h`, `party_traversal.h`). Without any server of ours, the guest's public UDP endpoint
  reaches the host through the public BitTorrent DHT (BEP 5 "mainline", as any torrent client
  uses). **Off by default** (user decision 2026-10-14: it posts the guest's IP on public DHT
  nodes): launcher *Party* tab *Find each other through the public DHT* or *System > Party rules*
  (`BB_PARTY_RENDEZVOUS=1/0`, which wins over bbport.ini `party_rendezvous`; unset = the ini,
  default 0). Host **and** guest need it on. Off, the host's *Network* line says "with their guest
  code (Add a guest code; automatic punching is off: turn on the DHT option on both PCs ...)" and
  the guest's line and its "no UDP answer from the host" failure say "automatic punching is off:
  send the host your guest code, or turn on the DHT option on both PCs".
  1. The DHT key is `BLAKE2b("bbport rendezvous v1" || party key)` cut to 20 bytes. Only someone
     with the code (and the password) can compute it; DHT nodes see an opaque hash.
  2. Guest: when its party link has not come up 3 s after the start (TCP failed), it does an
     iterative `get_peers` lookup for the hash and `announce_peer`s the party port's STUN mapping
     to the 8 closest nodes (the node records the guest's public IP). Repeated every 45 s while
     the link is down; read-only node (BEP 43), so nobody routes through us.
  3. Host, only without an open port (no UPnP mapping, no override, not on a public IP): a
     `get_peers` lookup every 5 s (the first from the bootstrap nodes, later ones seeded with the
     closest nodes found: one hop). Every endpoint found is punched exactly like a pasted guest
     code (at most once per 50 s per endpoint). The guest's UDP SYNs then get in (§ above).
  4. Bootstrap: `router.bittorrent.com`, `dht.transmissionbt.com`, `router.utorrent.com`,
     `dht.libtorrent.org` (`BB_PARTY_DHT_BOOTSTRAP=host:port,...` replaces them).
     The guest code works either way. Loopback runs use the DHT only with
     `BB_PARTY_RENDEZVOUS=1` **and** `BB_PARTY_DHT_BOOTSTRAP` (a fake node).
  - Exposure: the guest's public IP and the host's lookups appear on public DHT nodes under that
    hash, only when needed. A forged entry can only make the host send a few punch datagrams; the
    link still needs the party key.
  - Tests (`party-nat-test`): bencode / KRPC; a simulated 300-node Kademlia DHT with 5 % loss (the
    announce lands on the 8 closest nodes, the host's lookup finds the endpoint, a seeded re-poll
    takes 8 queries); the socket driver against fake nodes on loopback; and the whole path: a
    guest behind the simulated NAT, **no guest code**, connects over UDP ~0.8 s after the
    rendezvous starts (the host found it and punched once). Live: see §3d.
  - Not tested against the real DHT or a real router (test machines stay on 127.0.0.1).
- **NAT type and advice** (2026-10-14): host and guest ask a second STUN server
  (`BB_PARTY_STUN2`, default `stun.cloudflare.com:3478`) from the same party port. The same
  external port from both: a cone (punching works); another port per server: **symmetric**
  (punching cannot work). The log line `NAT: <type>; <advice>` and the overlay *Party* page
  (*Network:*) say what that means and what to do (forward UDP+TCP 9307, let a friend host, or a
  VPN such as Tailscale / ZeroTier). Once connected the guest logs `network path: party link over
  TCP|UDP` and the *Network* line shows it.
- **The fallback chain, verified for every pair of NAT types** (`party-nat-test`, a simulated NAT
  per RFC 4787 on each side carrying the real reliable-UDP streams, compared with the model in
  `party_nat.h`, 25 / 25 pairs agree):

  | Host NAT \ guest NAT | none | full cone | restricted | port-restricted | symmetric |
  |---|---|---|---|---|---|
  | none / open port (UPnP, forward) | TCP | TCP | TCP | TCP | TCP |
  | full cone | UDP direct | UDP direct | UDP direct | UDP direct | UDP direct |
  | restricted cone | punch | punch | punch | punch | punch |
  | port-restricted cone (most routers) | punch | punch | punch | punch | **no** |
  | symmetric | **no** | **no** | **no** | **no** | **no** |

  "punch" needs the guest's endpoint at the host: now automatic (rendezvous) or the guest code.
  "no": port forward on the host, another host, or a VPN. Guest-to-guest game traffic punches the
  same way and falls back to the host relay (`PeerPaths`).
  The network simulator has the same kinds: `BB_NET_SIM=...,nat=full|addr|port|sym` (`nat=1` =
  port); `sym` also makes the guest code / NAT check report another port per STUN server, so a
  live run detects a simulated symmetric NAT.
- **What CGNAT / hard NAT still cannot do** (no server anywhere, by design):
  - **Symmetric (endpoint-dependent) NAT** on either side - many mobile carriers and some
    carrier-grade NATs - gives the punch a different external port than STUN saw, so the guest
    code points at the wrong port and the punch fails. Two symmetric NATs can never meet without
    a relay.
  - **Port-restricted CGNAT with short mapping timeouts** may drop the mapping between STUN and
    the punch; the guest code then goes stale (the guest rejoins to get a new one).
  - **No inbound at all** (some campus / corporate firewalls block unsolicited UDP even after an
    outbound packet) cannot be punched.
  - There is no third-party relay or TURN server. **Relay through a member with an open port**
    (considered, not built): when the host cannot be reached at all but member A can, both the host
    and guest B could connect *out* to A and A would forward the link and the game's UDP between
    them (the host already relays guest-to-guest game traffic the same way, `PeerPaths`). It would
    keep the host's world, at the cost of A's upload (~3 KB/s per relayed guest) and one more hop
    of latency. Not built because the simpler remedies cover it: let A host (the world is then A's),
    or use a VPN that puts everyone on one network (Tailscale, ZeroTier, Radmin VPN) and the
    host's VPN address.
- There is no relay server: the host itself is the rendezvous and the relay. That is why only the
  host needs a port (or a guest code per guest).

## 5. Running the profiles

```
python tools/mp/instances.py run --party --count 2 --env BB_NET_SIM=lat=150,jit=30,loss=3 ...
--ienv "0:BB_PARTY_DIRECTOR_TEST=...,host_outage=5000@20%m24_01"
--ienv "1:BB_PARTY_DIRECTOR_TEST=...,guest_outage=5000@29%m24_01"
```
(the outage steps run S s after the last load / previous step, in the named map).

## 6. Security: what a stranger on the party port meets (F3, 2026-10-14)

Every byte from a peer is untrusted until the AUTH proof is checked, and still capped after it.

| Threat | Defence | Test |
|---|---|---|
| Garbage / oversized frames before the handshake | 64 KiB handshake frame cap, connection dropped; 16 pending handshakes in all, 4 per address; a handshake that never finishes times out | party-security-test `regress_link`, fuzz `linkhost` |
| Online password guessing | `AuthThrottle` (party_link.h): 5 wrong proofs per address per minute are free (typos, an old code), the 6th locks the address out for 30 s, every later wrong proof doubles it (to 15 min); a locked-out address is refused at HELLO with "too many wrong passwords from your address: try again in N s", the right password included. 4096 addresses tracked at most. **Global budget (0.6.2)**: a guesser spread over many addresses (one try each) is caught by the sum: more than 30 wrong proofs from all addresses together within a minute make the host refuse every address that has not passed AUTH before ("the host got too many wrong passwords (from several addresses) and takes no new players now: try again in N s") for 60 s, doubling to 15 min, forgotten after an hour without a wrong proof; the last 64 addresses that got in (members, a crash re-join) still pass. Offline guessing of a captured handshake pays Argon2i (8 MiB, 3 passes) per guess | `regress_auth_throttle` (pure rule incl. the global budget: a member passes, a stranger waits, 30 a minute never trips it, the second lockout doubles; a real host over loopback) |
| Replay / forged frames after WELCOME | Every frame is sealed with a per-direction session key and a counter nonce (party_crypto): a replayed, reordered or altered frame fails to open and drops the connection | fuzz `linkhost` (forged frame at the end) |
| A member flooding the host | Per-member token buckets (200 frames/s, 4 MiB/s, bursts 2000 / 48 MiB); RPCs over the limit get "rate limited"; 20000 drops in a row close the connection | `regress_link` flood |
| Malformed events | Every event body has a decoder with caps and a sanitizer (travel, story, items, phantom, lamp requests, world actions, flags, snapshots, world groups, balance, chalice goods, drop seed, direct summon bodies, gift_up, sync_digest / sync_resync); each is fuzzed with random and structure-aware inputs under UBSan (party-fuzz-ubsan) | party-fuzz / party-security-test (20 s run in the test) |
| A malicious host | The guest's decoders get the same checks (fuzz `linkguest`: mutated CHALLENGE / WELCOME / ROSTER / EVENT frames) | fuzz `linkguest` |

## 7. Desync detection and long outages (F5, 2026-10-14)

**Digests** (party_desync.h). The host sends every `BB_PARTY_DESYNC_S` (15 s) a `sync_digest`: 32
bucket hashes of its host-world mirror image and 32 of its candidate flags, with the flag stream's
seq. Each guest hashes what it holds: its mirror image (GuestWorldMirror) and a model of the host's
flags (the last snapshot plus every change after it). The link is reliable and ordered per member,
so the digest arrives after every change the host sent before it: any difference is a real desync
(a lost or misapplied event, a seq gap). The guest then asks `sync_resync` with the differing
buckets; the host answers with the world groups of those buckets (a partial `world_groups`, zero
groups included) and, for any flag bucket or a gap, a fresh `flag_snapshot` (the join catch-up
path: set-only, idempotent). Limits: a guest asks at most every 5 s and stops after 3 resyncs that
did not help ("persistent", logged once; retried after 5 min); the host serves one resync per member
per 10 s. Logs: `Party progress: desync: ...` on both sides.

**Long outages** (party-link-test `test_long_outages`, timings scaled 1:10 so the test runs in
~25 s: lost timeout 1 s for the game's 10 s, slot keep 6 s for 60 s):
- "30 s" outage (3 s): both sides call the link lost, the host keeps the slot, the guest reconnects
  by itself and resumes 3.1 s after the outage began; all 60 events posted before and during the
  outage arrive once, in order.
- "120 s" outage (12 s): past the keep the host releases the member; the guest still comes back by
  itself as a new member (not resumed). Events posted into the released slot are gone (0 of 10);
  the director's new-member path re-sends the full flag snapshot and world image, which covers C2.
- A host whose event callback stalls for 4 lost timeouts (a paused / alt-tabbed / loading host)
  keeps its member: the link thread answers pings; the 30 events sent meanwhile arrive in order.

**Live (2026-10-14, 2 clients, `BB_NET_SIM=lat=30`, `BB_PARTY_DESYNC_S=5`, guest `BB_PARTY_DESYNC_TEST=3`,
host `host_outage=30000@50`):** the first digest matched 15 s after the join; the injected
desync (mirror group 0 flipped, one host flag forgotten) was found by the next digest ("world
buckets 0, flag buckets 19"), the host sent 36 groups and a snapshot, and the following digest
said "resolved". The 30 s total outage: the host's link called the guest lost after 10 s (slot
kept); the guest's game left the session ~12 s in (not the game's timeout: our host-lost grace, see the follow-up below) and went home to its last lamp
(a 3.4 s loading screen); the link resumed as the network came back (20.2 s after it was lost);
the first direct summon was refused (the host's game still held the dead connection) and the bell
summon brought the guest back 2.4 m from the host, 41.3 s after the outage began (two loading
screens: correct, not seamless). 21 digests matched afterwards. Screenshots showed both players
together before and after and the HUD's "Connection lost" during.

**F5 follow-up: what ended the session, and a one-load recovery (2026-10-14, par_f5b).** The ~12 s
drop above was ours, not the game's: the guest's link calls the host lost after 10 s of silence
and its 2 s host-lost grace then sent the guest home (`host lost ... home now`). Fixed:
- A **silent** link (`no traffic for 10 s`: an outage, not a reset / BYE / refusal) keeps the
  host's world up to `BB_PARTY_OUTAGE_HOLD_MS` (45 s; the host keeps the slot 60 s). The host holds
  the room's 15 s heartbeat timeout while a member sends no VITALS (before its link's own 10 s).
- What then ends the session is **the game's own silence timeout: 25.0 s** (live, both games:
  `notify_multi_play_error`, then LeaveRoom; the transport config's 25000 ms). While the link
  says a peer is only temporarily lost, the game hears that peer's last small datagram again every
  2 s (`bbnet::p2p_outage_keepalive`, `BB_PARTY_OUTAGE_KEEPALIVE=0` off). Live this kept the
  **host's** game from dropping its silent guest (run 1 without it: dropped at 26.6 s), but the
  **guest's** game still ended the session at 25.0 s. A 30 s outage therefore still costs the guest
  a reload; outages under ~25 s keep the session (5 s: section 3).
- When the guest's game ends the session during the outage it goes home **at once** (OnLeave_Limit,
  one loading screen while the network is still down) instead of standing in the host's world
  without a session until the game's room-gone path (~15 s later, live run 1). Its LeaveRoom, lost
  in the outage, is retried until the host is reached.
- The host, on the member's return: one scan 1.5 s later releases a game connection to it that has
  been silent since (C6's release) and ends its sign hold either way; a direct summon asked
  meanwhile waits for that scan instead of being refused. If the host's game dropped the member
  during the outage, its room membership ends at once (`member_timed_out`), and a direct summon
  while the host's game is still ending that session is answered `"wait":"settle"` (the guest asks
  again every 1.5 s, unit-tested; not needed live).
- After a resumed link the guest checks the session: host traffic back = survived; out of the room
  = summoned back; still in the room without host traffic for 12 s = home cleanly (no half-dead
  phantom).

Live, 2 clients, `BB_NET_SIM=lat=30`, a symmetric 30 s outage 25 s after the join (outage file),
guest title join off (bell + direct summon), screenshots every 3 s:

| | before (par_f) | run 1 (hold, no keepalive) | run 2 (all of the above) |
|---|---|---|---|
| guest's game ends the session | ~12 s (our grace) | 25.0 s (its own timeout) | 25.1 s (its own timeout) |
| host's game drops the guest | - | 26.6 s | not until the guest left (keepalive) |
| guest home (loading screen) | 12 s | ~40 s (15 s without a session first) | **25.3 s, 3.3 s load, during the outage** |
| link back / direct summon | first summon refused (hold) | served, failed (the host's game left the room 4 s later), next bell 20 s later | **asked 0.8 s after the link, old connection released, room served at once, joined 3.2 s later** |
| guest back beside the host | 41.3 s after the outage began | 67.5 s | **36.4 s (6.3 s after the network returned), 1.6 m from the host** |

Run 2's screenshots: both together before; during the outage each sees the other frozen
("Connection lost" on the host's panel); the guest's loading screen home at ~25 s; the host's
"Co-operator Hunter1 returned to their own world" as the guest came back; both together again
(1.6 m) to the end of the run. Two loading screens for the guest (home, then the summon), as the
game's own 25 s session timeout cannot be outlasted yet. Next: find what the guest's game counts
as traffic for that timeout (the replayed datagram did not reset it on the guest's side). Done in
0.6.2: section 7b (outages up to 30 s keep the session, no loading screen).

### 7b. Outages up to 30 s keep the session (2026-10-15, 0.6.2, agent/outage062)

The 0.6.1 regression run still dropped a 15 s outage: the host room's heartbeat timeout removed
the member 0.6 s after its link came back ("left room 1001 (timeout)"). Restarting the heartbeat
clock there made the outage survive, but ~36 s later the host's game left the session by itself
(OnIrregularLeaveSession) and the guest stood in a dead session for 30 s. What the games count,
found by RE (`deps/ghidra-export`, decompiles below) and a hex trace of the game's P2P datagrams
(`BB_NET_HEX=N`, the first N bytes of each, both ways):

- **The game's transport** packs records into each datagram: `[channel u8][length u16 BE][body]`.
  Channel 1 is a reliable, ordered stream with TCP-like congestion control; its body starts
  `[seq u16][ack u16][type u8][window u8]` (type 3 / 7 data, +0x30 = the ack field is valid,
  0x30 a bare ack, 2 a duplicate-ack report carrying the size it saw, 1 the handshake; the window
  byte is the receiver's free buffer in 1/255). Channel 0 is unreliable state (positions etc.).
  Record handler 0x271deb0, ack / window update 0x271f650 (state +0x64: 0 slow start, 1 avoidance,
  2 fast recovery with the recovery point +0x13c; window +0x114 / +0x11c / +0x118).
- **The 25 s idle timeout** is a per-connection deadline that every channel-1 record restarts
  (0x271b900 at the top of the record handler: now + the manager's +0x24, 25000 ms). Channel-0
  datagrams do not. 0.6.1 replayed each peer's last *small* datagram during an outage: on the host
  that was the guest's bare ack (channel 1: it worked), on the guest the host's last small datagram
  was an unreliable state record (channel 0: it never counted, so the guest's game still ended the
  session at 25 s).
- **What ended the host's session after a resume**: 0x17894e0 (the session manager's send) queues a
  message the transport cannot take yet in a per-peer list (SSM +0x188); once all those lists hold
  more than 1000 messages (`cmp 0x3e9`) it raises OnIrregularLeaveSession and leaves
  (BeginLeaveSession from +0x178980d). After an outage the transport had: lost everything in
  flight, collapsed its window, and - on the guest's first datagrams after the outage, which
  reported the gap as duplicate-ack records - entered fast recovery with the whole backlog
  (~400-770 segments) as its recovery point. In fast recovery a partial ack releases one segment:
  live 10 segments/s against the game's ~30 messages/s, so the queue reached 1000 about 30 s
  later. The other irregular leave, 0x1789b60's check of SSM +0x2e0 against +0x2f0 (received
  messages left unprocessed for too long), never fired in these runs.

What the port does now (`gpu/shim/net/nr_outage.h`, used by `net_socket.cpp`; all of it off with
`BB_PARTY_OUTAGE_REPLAY=0`):
1. **Keepalive record**: while the party link says a peer is only temporarily lost, the game hears
   that peer's last reliable *data* record again every 2 s (its ack flag cleared). It is an old
   sequence number, so the transport drops it (and acks it into the outage), but its idle deadline
   starts over. Both games now keep the session through 30 s.
2. **Resume replay**: every reliable data record the game sends to a peer is kept from 3 s before
   that peer went silent (a silence of 4 s or more counts) until it is heard again; then they are
   sent once more, in order, each sequence number once, packed into datagrams of at most 1100
   bytes (live: 600-770 records in 24-30 datagrams from the host, 7-25 from the guest). The
   receiver gets the outage's segments as if they had only been late and acks the whole backlog
   with one cumulative ack.
3. **Duplicate-ack reports held back** for 3 s after a resume: the peer's reports of the gap the
   outage left (live: 8-113 of them) would put the sender into fast recovery before the replay
   has filled the gap.
4. **Oversized datagrams split**: catching up, the transport packs datagrams past its own MTU
   (0x4d0 = 1232; live up to 9178 bytes); the peer's game reads at most its MTU and the records
   past it were lost, which left a new gap and stalled the stream again. A datagram over 1232
   bytes now leaves as several, split at record boundaries.
5. **Host room heartbeat**: when the hold on a silent (or loading) member's 15 s heartbeat timeout
   ends, its clock starts over instead of the member being removed before its next heartbeat
   (`PartyHostService::tick`, party-host-test).
6. **Link error after silent game traffic**: a guest whose link ends with an error (reset, send
   failed) after the host's game traffic had already stopped for 5 s takes the outage hold too
   (live on the loopback simulator the host's link timed out first and its close reached the guest
   as "send failed"; a crashed host resets the link while its game traffic still flows).
7. **Fallback, never a dead session**: if the host's game ends the session by itself anyway
   (BeginLeaveSession from +0x178980d or +0x1789cb4, the two OnIrregularLeaveSession sites), the
   host tells every member at once (`host_session_end` event) and a guest in its world goes home
   with one loading screen, instead of standing in a session that is over until the host's game
   leaves the room (~30 s later, live).

**Live, one session** (2 clients, loopback, `BB_NET_SIM=lat=30`, symmetric outages through
`BB_NET_SIM_OUTAGE_FILE`, guest title-joined into the host's Hunter's Dream, screenshots every 3 s,
45 s between outages):

| Outage | Link | Game session | Loading screens | Host records replayed | Afterwards |
|---|---|---|---|---|---|
| 10 s | lost at 9.4 s, resumed 0.4 s after the network | survived ("outage over: the game session survived") | 0 | 767 in 30 datagrams | 45 s normal traffic, no drop |
| 20 s | lost at 9.7 s, resumed 0.1 s after | survived | 0 | 768 in 30 | 45 s normal, no drop |
| 30 s | lost at 9.4 s, resumed 0.1 s after | survived | 0 | 749 in 30 | 37 s to the end of the run, no drop; P2P traffic at its pre-outage rate |

Screenshots: both players side by side before; during the 30 s outage each sees the other frozen
in place and the host's panel says "Connection lost"; afterwards both together again (0.9 m), the
guest's game still a phantom in the host's world (type 1/7), no loading screen on either. The
first run of the sweep lost the 20 s outage to point 6 (fixed, then the re-run above); single runs
of a 20 s outage before point 4 / point 3 showed the stall described above (host leave 29-30 s
after the resume).

**Fallback, live** (same setup, one 20 s outage with `BB_PARTY_OUTAGE_REPLAY=0`, i.e. the 0.6.1
behaviour plus points 5-7): the session survived the outage itself, then the host's game left it
31.6 s after the network came back (BeginLeaveSession from +0x178980d, its send queue full). The
host sent `host_session_end` at once; the guest went home 0.06 s later (one loading screen, 3.4 s)
instead of ~25 s in a dead session; the host's game was hosting again 25.9 s later and the
guest's direct summon (asked 1.7 s after its load) joined 1.4 s after that (second loading
screen, 3.3 s), host `cooperators 0 -> 1`.

Not covered: a real network (the simulator drops UDP in both directions and holds the TCP link;
the loopback TCP close is real), 3-4 players, outages over 30 s (the guest keeps the host's world
45 s, the host its slot 60 s; the keepalive runs as long as the link calls the peer lost).

### 7c. Outages of 60, 90 and 120 s keep the session (2026-10-15, 0.6.2, agent parlast)

After the send-queue patch (parx: the session manager's limit of 1001 unsent messages, ~30 s of a
stalled link, raised to 30000) nothing in the game ended the session within 50 s; what set the
bound were our own holds: the guest kept the host's world 45 s after its link called the host lost
(10 s into the silence, so ~55 s of outage) and the host kept the slot 60 s. Measured with those
two raised (`BB_PARTY_OUTAGE_HOLD_MS=125000`, `BB_PARTY_SLOT_KEEP_MS=130000`), one session, 2
instances, loopback, `BB_NET_SIM=lat=30`, symmetric total outages through
`BB_NET_SIM_OUTAGE_FILE` 45 s apart, test save `cathedral_day` (the guest title-joined beside the
host in Cathedral Ward), screenshots every 3 s (run `combo`):

| Outage | Link | Game session | Loading screens | Host records replayed | Afterwards |
|---|---|---|---|---|---|
| 60 s | lost after 10 s, `back in the party after 50.4 s (session resumed)` | survived (`outage over: the game session survived`, the host's game traffic back 1.5 s after the link) | 0 | 743 in 29 datagrams | 45 s normal play |
| 90 s | lost after 10 s, back after 80.7 s | survived | 0 | 739 in 29 | 45 s normal play |
| 120 s | lost after 10 s, back after 110.8 s | survived | 0 | 590 in 23 | then two guest crashes and rejoins (C6) |

Screenshots: both players side by side before; during the 120 s outage the host's party panel
says "Connection lost" while each game shows the other frozen in place; afterwards both together
again, 0.8 m apart, no loading screen on either. **The new limit is ours, not the game's**, so the
defaults are now the tested values: the guest waits 125 s (an outage of up to ~135 s) and the
host keeps a silent member's slot 130 s (unit: party-link-test `slot keep env`). A real crash is
not affected: a killed game's link is reset at once (C6 / section 8), only a silent link waits.

What a real 2-PC test still has to show (playtest2_checklist.md): a cable pull / Wi-Fi off on one
PC (the simulator drops both directions at the same instant; a real outage may be one-sided and
the OS may report the TCP link down earlier than our 10 s), a NAT whose mapping expires during a
long outage (the host's UDP hole may close after 30-120 s: the link then comes back over a new
mapping, which the resume by token covers, but the game's P2P transport would need its peer's
new port), router DHCP or address changes (a new address is a new peer for the game: the session
would not resume), and 3-4 players.

## 8. Host crash: the guests wait for the host (0.6.2, agent robust1)

Before: when the host's game crashed, each guest went home at once (one loading screen) and was
summoned back by itself once the restarted host was in its world again (C6, par_c6: 33.5 s after
the crash), but nothing told the guest what was going on, and nothing ended the wait if the host
never came back.

Now (`gpu/shim/party/party_runtime.cpp`, `host_wait`): when the host is lost for real - the link
dropped and did not come back within the 2 s grace (a reset: the host's process is gone), or a
silent link outlived the 45 s outage hold, or the link came back as a **new session** (the host
restarted before the grace ran out) - the guest:

1. goes home at once if it was in the host's world (one loading screen, as before; its own world is
   the safe place to wait: nothing it does there can be lost);
2. shows **"Waiting for the host to come back (N s)..."** in the corner (refreshed every second)
   and as the Party page's status (`Reconnecting`), while the link keeps trying every ~2 s;
3. when the host's restarted game answers (`host wait: the host's game answers again ...`) it keeps
   waiting until the host's own roster entry says its world is up (or the guest is in its room);
   then `host wait: the host is back ...`, the corner says "The host is back: joining it", and the
   direct summon brings the guest beside the host with **one loading screen**;
4. after `BB_PARTY_HOST_WAIT_S` (180 s; 0 = no limit) without the host: a notice ("the host did not
   come back within 3 min - you play on alone in your own world; System > Party > Rejoin the host
   joins again") and the party is left (the guest is not pulled away later while playing alone).

A refusal, the host ending the party (BYE) or the player's own Leave never start the wait. Outages
that the session survives (F5, up to 30 s, 0 loading screens) do not reach it: the link resumes
the old session (`back in the party ... (session resumed)`) and the wait only starts for a lost
or new session. A Rejoin after Leave (the host does not know the old session either) counts its
time from the Rejoin, not from a loss long ago.

**Live (2026-10-15, two loopback instances, test save in the Hunter's Dream, `instances.py run
--count 2 --party --crash-plan "0:cooperators 0 -> 1@15"`: the host's `bbport.exe` killed 15 s
after the guest joined, `run.bat` restarts it with `BB_PARTY_RESTARTED=1`), two runs on this
build:**

| | run 2 | run 3 |
|---|---|---|
| guest leaves the dead session (crash ~0.3 s before) | t 35.6 s, home load | t 34.6 s, home load |
| `host gone: waiting ... up to 180 s` | at once | at once |
| the restarted host's game answers (its party link) | 6.0 s after the loss | 6.0 s |
| the host's world up again, `host wait: the host is back` | 18.0 s | 18.0 s |
| guest in the host's room (`client`) | 18.7 s after the crash | 18.6 s |
| loading screens for the guest | 2 (home, summon) | 2 |

The host itself: restart 3 s after the crash, Continue, world up 12 s into its new run, `cooperators
0 -> 1` 17.2 s into it. Screenshots (`shots3`): the guest in its own Hunter's Dream with "Waiting
for the host to come back (11 s)..." in the corner while the host's window shows its title screen;
14 s after "the host is back" both stand together in the host's Dream (2.3 m apart, each sees the
other's name plate). Run 1 (same build before the wait was extended past the link's return) showed
why the wait must last until the host's world is up: the link was back 6.1 s after the crash, long
before the host's world.

Not live: the 3-minute cap (unit: the notice text, `party-status-bridge-test`; the code path is the
same Leave as the Party page's), a host crash during a boss fight or a travel with the wait (C6's
earlier runs cover the rejoin itself there), a real network.

Why not without the home load: the crashed host's room is gone with its process; the restarted host
opens a new room, and the game has no path from a phantom in one room into another without loading
the own world first (B2 re-entry, b2_seamless.md, keeps the room; it cannot switch rooms). Waiting
as a phantom in the dead world would be two loads too (home, then the summon) and later.

### 8a. Guest crash: no outage replay into the restarted game (0.6.2, agent parfin3)

The F5 outage replay (section 7, `nr_outage.h`, `net_socket.cpp`) is keyed by the peer's source
address and port. A crashed guest's restarted game comes back from the same address and port, so
once it had been silent for over 4 s the host's socket replayed the dead session's reliable records
into the new game (4-player runs a7b / a7t1: `outage resume: ... heard again after 7.6 s: 14
reliable record(s) sent again` right after the restarted guest joined the room) and held back its
duplicate-ack reports; the restarted guest's title join then got no snapshot
(`title join: FAILED (no snapshot within 20 s)`), Continued into its own world and was summoned
again 35-50 s after the crash.

Now the C6 release of a crashed member's game connection (`party_b2.cpp`, "silent since the
crash") also forgets that source's replay, keepalive, last-small, dup-ack hold and last-heard
state on every game socket (`bbnet::p2p_forget_source`; `C6: its outage-replay state forgotten
(N entries): no replay into its restarted game`). An outage (F5) keeps all of it, as before. A
title join that still gets no snapshot gives up after 12 s (was 20 s), and run.bat restarts a
crashed party game after 1 s (was 3 s; the host has released the old connection 1.5 s after the
crash and holds a title joiner's room request until then).

Live (s3b, 2 instances, one session, two guest crashes then a host crash without a restart):

| | crash 1 | crash 2 |
|---|---|---|
| kill -> host `lost Hunter1 ... connection reset` | 1.8 s | 2.2 s |
| C6 release + replay state forgotten | 3.4 s | 3.7 s |
| restarted game asks the host for its room | 5.9 s | 6.3 s |
| host's room, JoinSession, snapshot applied | 7.8 s (snapshot 1.1 s after the join) | 8.1 s (1.0 s) |
| FROM sign-in over, Continue | 12.6 s | |
| host `cooperators 0 -> 1` | **16.6 s** | **16.9 s** |
| guest in the world beside the host (0.7-0.8 m, screenshots) | 26.6 s | 27.0 s |

The rest is the game's own boot, the FROM sign-in (~5 s, the open C6 item) and its ~12 s load.
Host crash with `BB_PARTY_RESTART=0` and the guest's `BB_PARTY_HOST_WAIT_S=30`: the corner counter
"Waiting for the host to come back (N s)..." (screenshots at 7 s and 28 s), then `host wait: the
host did not come back within 30 s: leaving the party` 32 s after the crash, the guest on in its
own world.

### 8b. The FROM sign-in before Continue (C6, 0.6.2, agent parlast)

What the restarted game waits for between its title and Continue, traced live (runs `g3w`, `combo`,
`g3b`; `BB_HTTP_TRACE_CALLERS=1`, `BB_PARTY_TRACE_FROM_UPDATE=N`):

* **Our side is instant.** Every HTTP answer (ss.info from the in-process FromApi on the host, the
  rest over the party link) takes 0-1 ms; NP auth (`sceNpAuthGetAuthorizationCode`,
  `sceNpAuthPollAsync`) answers at once. Capping the game's `sceHttpWaitRequest` timeout to 25 ms
  (tried) changed nothing: the waits are not the pacing.
* **The FROM client runs every frame.** SprjNetworkClientMan's update 0x1E7DF20 (its timers, then
  the HTTP client update 0xFD10D0, which sends whatever is queued, then the completions
  0x1E7F240) runs once per frame (dt 0.033 s, intervals 0.020-0.048 s). All requests of the title
  and the world go out from it (call chain fd2ac9 <- fd14ec <- 1e7e2f5 <- 103d438 ...).
* **The title's own sign-in chain sets the pace.** The title flow built by 0x1C37B30 is a chain of
  menu-step futures (0x1C1A070 = then; step class vtable 0x53474E0, created by 0x1C3C810 with a 60 s
  limit); its network stages are function objects: 0x1C48F80 (CSPlatformNetworkMan 0x1E75D10, NP
  auth "psn:s2s"), **0x1C492C0 ss.info** (0x1E8AA40), **0x1C48AA0 login** (0x1E8B040; its answer
  0x1C48CA0 marks the step done, state 4 at +0x1C), 0x1C43EE0 sync_chara_id (0x1E8BE50), 0x1C485D0
  the notice (0x1EA3CD0). Each stage starts when the previous step's future is resumed by the menu
  framework, not when the answer arrives: ~1 s per stage (ss.info -> login 1.03 s, login ->
  notice 0.90 s, live).

Live (g3b, the guest killed in the world, run.bat restart after 1 s): boot +2.4 s after the kill,
link +4.6 s, the title takes its first press +7.0 s, the ss.info stage starts +10.1 s, the sign-in
is over +11.9 s (1.8 s for three requests), Continue's loading screen +14.1 s, host `cooperators 0
-> 1` ~15 s (combo: 14 s and 15 s for two crashes), in the world beside the host (0.5 m) ~19.6 s
after the title join started.

Not shortened, on purpose: Continue while the sign-in runs leaves the game offline (that is why
AutoContinue holds it, docs/STARTUP.md), and the ~3 s from the first press to the ss.info stage and
the ~1 s per stage are the title menu's own step chain. Patching FROM's menu framework (a shared
future scheduler used by every title and in-game menu) for ~3 s on a crash rejoin is not worth the
risk; the hold itself already ends the frame the notice answer is in.
