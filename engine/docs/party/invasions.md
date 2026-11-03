# Invasions into open public games (0.7.8, CL-4cc63c)

A player can join a listed game as a **hostile phantom** (a red phantom's rules): the host and its guests are the invader's
enemies, the map's enemies are not. Owner decision (community poll, 88 %): invasions are **host-controlled**. The host's switch
**Allow invaders** is off by default, only the host's value counts, and a host that has it off is never offered for invasions.

Code: `gpu/shim/party/party_invade_rules.h` (pure: allow / deny table, rate limit, block book, how an invasion ends, the wire;
unit test `party-invade-test`), `party_invade.{h,cpp}` (game side: the host's gate and tick, notices, the invader going home),
`party_link.cpp` (the invader admission path, role limits; `party-link-test` "invasions"), `party_pvp.cpp` (the invader's
team), `net/party_vpnlobby.*` and `net/party_discovery.*` (the list flag and "where the host is"), `launcher.py` (Invade button),
`tools/vpn/lobby/nwlobby.py` (the lobby server), `scripts/party_where.py` (area / phase / NG words).

## Switches

| Where | What |
|---|---|
| bbport.ini `party_invaders=1` / env `BB_PARTY_INVADERS` | the host's **Allow invaders** (live, off by default) |
| System > Party access > **Allow invaders** | the host's row (a guest sees it hidden; its page is the "Host's access" line) |
| Launcher Party tab > **Allow invaders (listed games)** | host only |
| System > Party > **Block the invader** | host, only while an invader is there; press twice |
| Launcher Join page > **Invade** | next to **Join**, on the Parties found (VPN / LAN) list and the Nightwatch VPN games list; enabled for a row whose host allows invaders and has room |
| env `BB_PARTY_INVADE=1` | what the launcher sets for that one start when you press Invade (never saved); the game joins the address in `BB_PARTY_CODE` as an invader |

## How an invader finds a target

By hand, from the lists (the "manual pick list" first step): a host that **allows invaders and has a free slot** shows an
**Invade** button on its row, with the line *Currently in: Yahar'gul (Blood Moon), NG+1* under the list. The lists show the host's
**character name** only, never an address (the address stays in the row for joining). The game's own Sinister Bell use does not
start matchmaking yet (it would need a hook in the game's invasion flow); the bell is not used.

## Admission (host side, `party_link.cpp` + `Gate`)

An invader is **not** a guest: its HELLO carries `role = 1` (a trailing byte; an older host reads no more than the names). It is
never given the party key, so:

* **No password.** The invader never sends the password or any hash of it. The AUTH proof is made under a public *invader key*
  (`BLAKE2b-256("bbport invader key v1")`) over this connection's fresh host and guest nonces: nothing to replay, and no secret to
  steal. What the key gives is the **role**: a proof under it admits an invader only, and a cooperator's party key never passes
  that path (so the invader path cannot be used to become a cooperator, and a cooperator cannot present as an invader).
* **The normal identity check first**: bbport build, eboot, game params, patches, mods, party hooks, party rules. An incompatible
  invader gets the usual named *Mismatch* refusal ("bbport build differs (host ..., yours ...)", ...), never the gate's.
* **The gate** (`LinkConfig::invader_gate`, asked at the HELLO where the attempt is counted, and again at AUTH) answers with a
  named reason, sent as REJECT `Invasion` (code 9). In this order: *the host does not allow invaders*, *blocked for this session*,
  *too many invasion attempts* (an address may start 3 invasions per 10 minutes and knock 20 times; refusals do not count against the 3), *Stay in my world is on*, *not in a world you can enter now* (title,
  prologue), *a loading screen*, *a cutscene*, *a boss fight is on* (fog), *the host is dead*, *the party is travelling*,
  *another invader is already in*, *no free slot (4 players)*.
* **Slots.** The game has four player slots (the host and three phantoms); an invader is one of them. With the host and two
  guests there is room; with three guests there is none and the invader is refused cleanly (`Capacity`, unit tested, and live with
  Max players 2). One invader at a time (`kMaxInvaders`).
* **Before it stands in the world** the same checks run every frame; a boss fight, cutscene, loading screen or travel that begins
  while the invader is on its way turns it away with the reason (`Party: invade: invasion of slot N ends: invasion refused: ...`).
  An invader that is not in after 120 s is sent away.

## The role limits (enforced in the link, not in the game's modules)

The host's link drops, on both sides:

| Direction | Allowed for an invader | Dropped |
|---|---|---|
| invader -> host | EVENT `invade_fell`, EVENT `party_chat` (masked and rate limited like every member's), pong, roster, vitals | every other EVENT (progress, flags, items, give items, gather, travel votes, stay home, world actions ...), PARTY_CMD, PROGRESS, every RPC except the game's own session API (`http`, the room and signaling calls: the bell and the summon need them; `kick_member` and anything else is answered "not allowed for an invader") |
| host -> invader | EVENT `invade`, `party_chat`, `host_session_end`; roster; vitals | every other EVENT, PARTY_CMD and PROGRESS: it never receives the party's shared progress, flags or items |

So an invader cannot write the host's world progress or items, and its own save is not changed by the party.
An invader's slot is never kept: a lost link releases it at once (no 130 s resume), and the invader's link does not reconnect
(it goes home).

## The invasion

1. The launcher's **Invade** starts the game with `BB_PARTY_INVADE=1`, the host's address and no password.
2. The link is admitted as above; the invader is a roster member in state *Home*, and the host's director rings for it like for
   any waiting member (Beckoning Bell); the invader rings the Small Resonant Bell. The host and every guest read *An invader is
   coming: NAME*.
3. When the invader stands in the host's world: *Invader NAME has entered the world: fight!* on every member, and *You invade
   HOST's world* on the invader. From then on:
   * The invader's chr (its own on its game, a remote copy on the others) is moved to a **spare team** whose three table rows
     and columns are a copy of team 23's (`party_pvp.cpp`, `InvaderOpen`): an enemy of the host's team 19 and the guests' 20, an
     ally of the map's enemies, in both directions. No friendly-fire rule is needed; the host's PvP switch is unrelated. The
     cells and the chr go back when the invasion ends.
   * It does not follow the host's travel and takes no part in gathers or votes (it receives none).
4. It **ends** (`CheckEnd`): the invader dies (it goes home through the game's own send-home, not a revive at a lamp), the host
   dies, the host's world changes (any loading screen: a lamp, a respawn load), a boss fight starts, the host switches **Allow
   invaders** off, the host blocks it, or the link drops. The host sends `invade {"s":0,"w":<reason>}` to everyone and ends the
   link with a REJECT/BYE `Invasion` carrying the invader's reason: *You were defeated in HOST's world: back home*, *You defeated
   HOST: back home* ... The invader's game goes home (`host_lost`); a refused or ended invasion plays on solo.

Abuse limits: the rate limit above (per address), **Block the invader** (the name and the address are refused for the rest of
the session, the link also refuses the name), at most one invader, hateful names are masked everywhere they are shown
(`bbname::Display`; the log names no one), and the usual Argon2/AUTH throttles are not involved (an invader has no password to
guess).

## Rewards and win / lose

Winning or losing an invasion gives **no Echoes, Insight or items**. The session is a co-op session, not the game's invasion
session, and the vanilla result flow (Blood Echoes / Insight for the winning invader, the Sinister Bell consumed) is not
implemented: a kill in a party is a normal death (see pvp.md). The win / lose text is the notice on both sides.
Not done: the **Sinister Resonant Bell / Bell-Ringing Woman** items are not used or consumed; the invader's maximum health is the
phantom's (the host's *Phantom health cut* applies to it only where that rule reaches its game).

## The public lists: the invader flag and where the host is

Wire formats are additive; old clients and old servers keep working. **Never an address, a password or a coordinate.**

### Nightwatch VPN lobby (UDP 9310, `tools/vpn/lobby/nwlobby.py`; version 1 packets unchanged)

* **type 7 announce extension** (host -> lobby, a datagram of its own right after every type 1, every 10 s): body 7 bytes:
  `flags u8` (bit 0: the host allows invaders and has room, bit 1: a boss fight is on), `area u32` (the host's map id
  `0xAABBCCDD`, 0 unknown), `phase u8` (0 unknown, 1 day, 2 evening, 3 night, 4 Blood Moon), `ng u8` (0 = NG, 1..7 = NG+n,
  255 unknown). The lobby stores it only for a sender that has a live type 1 entry and only for the entry's TTL (30 s).
* **type 8 list2** (client -> lobby): `offset u16`, like type 2, asks for the extended reply. **type 9 reply2**: header as type 3
  (`total u16, offset u16, count u8`), then per entry: `ipv4 4, port u16, flags u8, players u8, max u8, area u32, phase u8,
  ng u8, name u8 len + UTF-8, build u8 len + ASCII`. Flags: bit 0 password, bit 1 your own entry, **bit 2 allows invaders**,
  bit 3 boss fight.
* **Compatibility.** Type 3 replies (what old clients ask for) never carry bit 2 / bit 3, so their strict parser accepts them;
  an old server drops types 7 and 8 (its unknown-type path), so a new client's type 8 gets no answer and it **falls back to
  type 2** at once (`launcher.py lobby_fetch`); a host announcing to an old server is listed as before and not invadable.
* **Invadable = flag bit 2** = the host sent a type 7 with bit 0 within the TTL. The host sends bit 0 only while its switch is on
  *and* it has a free slot and no invader; with the switch off it sends bit 0 = 0 (or nothing), so it is never offered.
* **Deploy (the owner; OpenVPN is never restarted).** Only the `nwlobby` systemd service changes, and only its Python file.
  1. From a checkout of this branch: `git archive HEAD tools/vpn/lobby scripts/namefilter.py scripts/namefilter_data.py | ssh visy@<vpn-host> 'tar xf - -C ~'`
  (the installer finds the name filter files at `~/scripts` through the repo layout). 2. `ssh visy@<vpn-host>` then
  `sudo sh ~/tools/vpn/lobby/install.sh`: idempotent, it re-copies the code to `/opt/nwlobby`, leaves the firewall rules as they
  are (they exist) and runs `systemctl restart nwlobby.service` (the tunnel, OpenVPN and the other services are not touched;
  the lobby's list is in memory and refills within 10 s as hosts announce). 3. Check: `systemctl status nwlobby`, then from a
  VPN client the launcher's *Nightwatch VPN games* list (rows get an Area; a host with *Allow invaders* on gets Invade). 4. The
  order does not matter: 0.7.8 hosts announce the extension datagram (an old server drops it), 0.7.7 clients keep asking
  type 2. Rollback: `git archive` the old commit's `tools/vpn/lobby` the same way and run `install.sh` again.
* **For the Nightwatch launcher's author**: ask `type 8` first (fall back to `type 2` when there is no answer within about
  a second), parse `type 9` as above, show **Area** from `area` / `phase` (see the table below) and an **Invade** button next to
  **Join** for rows with flag bit 2; Invade needs no password: connect to the row's address and port with the PartyLink HELLO
  below. The row never shows the address.

### LAN / VPN discovery (UDP 9308, `net/party_discovery.*`, `launcher.py DiscoveryBook`)

The announcement (version 1) is unchanged. After it, the host sends a second datagram, the **extension**:
`0xfa 'I' 1, id u32, flags u8 (bit 0 allows invaders, bit 1 boss fight), area u32, phase u8, ng u8` (14 bytes). An old listener
drops it as junk (wrong second byte); a new one pairs it with the announcement of the same `id`, for the TTL of the announcement.
The `Area` column shows the area and phase when the extension is there, the announcement's own area name otherwise.

### The words (`scripts/party_where.py`, the same table for the Nightwatch launcher)

*Currently in: AREA (PHASE), NG[+n][, boss fight]* e.g. *Currently in: Yahar'gul (Blood Moon), NG+1*. Area = the map id's top byte
(21 Hunter's Dream, 22 Hemwick Charnel Lane, 23 Old Yharnam, 24 Cathedral Ward, 25 Cainhurst Castle, 26 Nightmare of Mensis,
27 Forbidden Woods, 28 Yahar'gul, 29 and 30 Chalice Dungeon, 32 Byrgenwerth, 33 Nightmare Frontier, 34 Hunter's Nightmare,
35 Research Hall, 36 Fishing Hamlet; anything else: no area words). Phase: day, evening, night, Blood Moon (the game's
time-of-day flags 9800-9802, read by the host on its main thread). NG: `NG`, `NG+1` .. `NG+7`.

## The PartyLink handshake for another client (the Nightwatch launcher / tools)

HELLO is the usual one (`party_link.cpp on_connected`) with **one more trailing byte `role`** (0 cooperator, 1 invader) after the
hook names. For `role = 1` the host asks its gate (and the identity check first), replies CHALLENGE with a fresh 24-byte host
nonce, and expects `AUTH = guest nonce || BLAKE2b-256(key = invader key, "bbp-auth" || host nonce || guest nonce)`; the session
keys are derived the same way as for a cooperator with `invader key` in place of the party key. Refusals are REJECT with
`RejectCode 9 (Invasion)` and the named reason (or `2 Mismatch` with the usual identity reason).

## The red phantom look (0.7.8)

The look is a SpEffect, not the chr type. The chr's effect update plays, for every SpEffect with `useSpEffectEffect`, the
SpEffectVfxParam row named by its `stateInfo`: the cooperator SpEffect 9006 has stateInfo 272 (row 272: midst SFX 211, the
blue phantom glow), and the game's invader SpEffect 9026 (the Sinister Bell's 9025 + 1, same 0.7 max-HP cut) has stateInfo 276
(row 276: SFX 212). (Writing the chr type, tried first, changed nothing; the 0x1b27 / 0x1b28 the cooperator code writes at the
chr's registration are idle-animation ids, not a look.) The invader's chr is a cooperator (9006) on the other games, so per chr
and per frame (`party_pvp.cpp` `InvaderTick`): 9006 is taken off with the game's `RemoveSpEffect` 0x17E1D90 and 9026 applied
through the chr's own `ApplySpEffect` (vfunc +0x3f0); the invader's own chr just gets 9026. At the end of the invasion 9026 is
removed and 9006 put back (only what this code changed). The look is the game's own and nothing more: BB has no PhantomParam table (no such param in gameparam / paramdef, no body tint, alpha or edge parameter), the cooperator and the invader effects are the same particle effect, FXR 211 and 212 in `frpg_sfxbnd_commoneffects` (17648 bytes each, 63 bytes differ: only the colours, 211 = (0.37, 0.79, 1.0) blue, 212 = (0.78, 0.25, 0.38) red), attached to dummy poly 203 of the chr. So there is no separate red body material to apply; the red glow around the body is the vanilla hostile phantom look. Not compared with a live vanilla invader (none available offline). Pure mapping: `LookNow` / `LookEnd` in `party_invade_rules.h`. Switch:
`BB_PARTY_INVADE_LOOK=0` turns it off (default on, as a part of the invasion feature, which is off by default).
The "Companions without glow" setting edits the same rows 272 / 276, so with it on (the default) the SFX is removed and the
red look is not visible either (the SpEffect is still on); turn the setting off to see the red / blue glows.

## Rewards and costs (the game's own rules)

From `global_event.lua` (eventcommon.luabnd, `OnDeadEvent_HostDead`, `OnDeadEvent_NormalBlackDead`; the game's SOUL_RATE_S 0.1,
SOUL_RATE_L 0.5) and `EquipParamGoods`:

| | Rule in the game's data |
|---|---|
| Cost to invade | None. The Sinister Resonant Bell (goods 225, SpEffect 9025) has `consumeHeroPoint` 0 and `isConsume` 0: it is not used up and costs no Insight (the Beckoning Bell, 200, costs 1 Insight; the party layer zeroes that). It is usable alive or grey, not in a session (`enable_multi` 0). |
| The invader kills the host | The invader: Blood Echoes by `CalcExcuteMultiBonus(0.1, ...)` and **+1 Insight**. |
| The host (and guests) kill the invader | The host: Blood Echoes at rate **0.5**, no Insight. Each guest: rate 0.5 * 0.5 = **0.25**, no Insight. (The Lua also drops a bloodstain of the invader, `CreateHeroBloodStain`: not reproduced.) |
| Result banners (the game's text effects, `SetTextEffect` = native 0x13360B0, a queue the HUD shows; texts checked live) | The invader that defeated the host reads **PREY SLAUGHTERED** (type 5, `TEXT_TYPE_TargetClear`). The host and every guest read **ADVERSARY EXPUNGED** (type 7, `TEXT_TYPE_BlackClear`) when the invader falls (vanilla shows it to the killer only; the party shows it to all of them). The host that is killed reads the game's own YOU DIED; the host side never gets PREY SLAUGHTERED for an invasion (the smoke fails if it does). Same `RewardOnce` as the payouts. Other types seen live: 1 PREY SLAUGHTERED (boss kills, EMEVD 2007[2]), 4 BLOOD ECHOES RETRIEVED. |
| Anything else (travel, boss fog, block, switch off, link lost) | nothing |
| Bell-ringing Woman | no rule of its own in the data: she only gives the bells (the Lua has no invasion branch for her) |

`CalcExcuteMultiBonus` (native 0x131D470) is the game's routine: echoes = rate * the level-up cost curve at the receiver's level,
added through the player chr (shown on screen), and Insight + N (capped at 99). It is called on **the receiver's own game**
with the receiver's own record, so the invader's reward is in the invader's own save and the host's and guests' in theirs; no
reward goes through the party's progress mirror (the invader's progress events are dropped by the host, and the host's echo
payouts are local), so nothing leaks into the host world or back. One payout per outcome: `RewardOnce` (same invader name and
ending within 120 s) and one pending outcome per game; the invader is paid once its own world is up after the send-home.
`BB_PARTY_INVADE_REWARDS=0` turns the payouts off. Pure table: `RewardFor` in `party_invade_rules.h` (unit test).

## Tests

* `party-invade-test`: the allow / deny table (every row, the order), the 4-player table (host + 2 guests: room; 3: none; party
  of 2), listed-as-invadable, the rate limit, the block book, how an invasion ends, the wire.
* `party-link-test` "invasions": an invader joins a password host with no password and is the invader; the host sends it only
  `invade` / chat; it can send only `invade_fell` / chat (everything else, party commands, progress and RPCs other than the session API are dropped); a
  second invader is `Full`; a gate refusal is named; an incompatible build gets the Mismatch reason; a kick is a session block;
  a lost invader is released at once; a host without a gate refuses.
* `party-net-test`: the lobby's type 7 datagram and the discovery extension (strict parser, an old listener drops it).
* `tests/test_nwlobby.py` (types 7-9, the unchanged type 3 reply, the shipped 0.7.7 server ignoring the new datagrams, the flag
  never set without a live extension), `tests/test_launcher_invade.py` (words, type 9 parser against the server, the fetch
  against a new and an old server, discovery extension, Invade enablement).
* Smoke groups `invade` (phase `iv`), `invade_refuse` (`ivx`), `invade_sim` (`ivn`: `iv` under `BB_NET_SIM=dsl` on all three games): the results below.

## Live results (2026-11-03, loopback, three / two instances, `tools/mp/smoke_party.py --only invade | invade_refuse | invade_sim`)

* **`iv`** (host with Allow invaders on, a cooperating guest, an invader without a password; 31 of 31 rows): the invader's first dial
  at its title is refused (*the host is not in a world you can enter now*), the second from its own world is admitted, stands in
  the world about 17 s later; its chr is on the invader team on all three games (the host's and the guest's copy of it moved from
  team 20 / 21 to 22, its own from 19 / 20); the host's and the guest's blows hurt it (503 -> 442 -> ... -> 121) and its blows hurt
  the host (719 -> 658); no friendly-fire cell opens between host and guest. It then dies (`ForceDead`): it tells the host, goes
  home through the game's own send-home (`OnLeave_Limit`, no lamp revive), the host and the guest read *The invader NAME was
  defeated and goes home*; it asks again, is admitted again, and when the **host** dies the invader reads *You defeated HOST: back
  home* and goes home; the cells and the chr are put back. No crash, no *Session lost*.
* **`ivx`** (host and invader; 11 of 11): refused with the named reason while Allow invaders is off, in a boss fight, in a
  cutscene and with 4 players in the world (the host's view forced by the smoke script `BB_PARTY_INVADE_TEST`, see DEBUG_SWITCHES.md;
  a real fourth player needs five games, the table is unit tested), admitted when nothing is in the way.
* Screenshots of the fight (host, guest) show the invader as a normal hunter with a name tag and a party-list entry: **the red
  phantom look is not done** (see below). The notices are on the party panel's last-event line and in the log; they were not
  caught by the 10-second screenshots.

## Not done / unproven

* **Real networks beyond what is listed below, and the human bloodstain.** The game's Lua also has the host call CreateHeroBloodStain when a red invader falls (a bloodstain of the invader); it is not reproduced (see Rewards).
* **Health.** The invader's health is the phantom's (the host's *Phantom health cut* applies where that rule reaches its game).
* **Boss fog / cutscene / loading** are proven by the forced view (the gate and the pending checks are the same code as for the
  real flags: `BossBarShown`, `CutscenePlaying`, `loading`); a real fog walk with an invader on the way was not run. An invader
  that is already in when a fog is entered is sent home (`CheckEnd`, unit tested; not run live).
* **Real networks.** Only loopback and `BB_NET_SIM` (below); never the VPN.
