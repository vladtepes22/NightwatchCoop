# Party logging and support bundles

What to turn on so a co-op bug can be followed on every PC, and what to send. Code:
`gpu/shim/party/party_log.{h,cpp}` (levels, tags, marks; unit test `party-log-test`),
`gpu/shim/party/party_log_game.{h,cpp}` (snapshots, area events, link trace),
`scripts/support_bundle.py` (the zip; `tests/test_support_bundle.py`).

## For players

1. Launcher → *Advanced / Debug* → *Party logging*: **Verbose** on every PC (the same level
   everywhere; a log says when a peer's level differs).
2. When something goes wrong press **F9** (or *Party tab → Mark an issue in the logs*). Every
   player's log gets the same numbered line, and the game shows "Issue mark n written to the logs".
3. After the session: *Advanced / Debug* → **Save support bundle...** on every PC and send the zips.
   After a crash the launcher writes `logs\support_<time>.zip` by itself (switch: *Save a support
   bundle after a crash*).

The launcher keeps the last 20 session logs (`logs\launcher_*.log`, was 5: in playtest 1 a guest's
restarts pushed the logs of the session out).

## Levels and areas

| `BB_PARTY_LOG` | What it adds |
|---|---|
| unset / `normal` | Nothing (no tags, no per-frame work). Marks, the run-id line per peer and a snapshot at a mark still work |
| `verbose` | A tag on every `Party...` line, a state snapshot every 10 s, area events (below), every link event both ways with a short body for flag / item / travel / summon events |
| `trace` | Verbose plus the bodies of every event (800 chars), every Blood Echoes change |

`BB_PARTY_LOG_AREAS` (launcher checkboxes; unset = all): `net` (link state, high RTT, the event
trace), `summon` (session role changes, `b2` events), `travel` (load start / end with reason and
time, world up/down, map changes, `travel` / lamp events), `death` (deaths and revivals of every
party member from the VITALS stream, `phantom` events), `items` (item-get box, item / drop-seed
events), `flags` (flag sync: `flags`, `flag_snapshot`, `world_groups`, story and world-action
events), `dialog` (action prompts, the item-get box, the guest service menu). Per-event-name output
is rate-limited (20 lines per 10 s, the next line says how many were dropped); prompts 6 per 10 s.

`BB_PARTY_LOG_SNAPSHOT_S` changes the snapshot period (0 = none). Test aid:
`BB_PARTY_LOG_MARK_AT=S` makes one user mark S s after the world first came up with the link up.

## Lining up two logs

```
Party log: {run 149224 H0 t+4.737 pc 2.541 16:26:27.326} peer slot 1 (Hunter1): log run 16976b, level verbose, ...
Party: {run 16976b G1 t+3.208 pc 2.875 16:26:27.659} session ? (-1, sub -1) -> idle (0, sub 0) (t 1.4 s)
```

| Field | Meaning |
|---|---|
| `run 149224` | Random id of this game process (a restart gets a new one). The `peer slot N ... log run X` line names the peer's run, so you know which of its files to open |
| `H0` / `G1` | Host / guest and link slot (`-` before the link knows it) |
| `t+4.737` | Seconds since this game started (monotonic) |
| `pc 2.541` | **Party clock**: the host's link clock in seconds, estimated on guests from the pings (within ~RTT/2). Sort the host's and guests' lines by `pc` to merge them |
| `16:26:27.326` | Local wall time (PCs' clocks may differ; use `pc` across PCs) |

User marks start the line (no tag) and carry the origin's number, name and party clock on every PC:

```
=== USER MARK 1 === Hunter1 (slot 1), F9; local 16:27:27.393, party clock 62.609 s (run 16976b)
=== USER MARK 1 === Hunter1 (slot 1), from the party link; local 16:27:27.393, party clock 62.609 s (received; run 149224)
```

A guest's mark goes to the host, which passes it on to the other guests. Each mark also writes a
snapshot.

## Snapshot (every 10 s, verbose)

```
Party log: {...} snapshot (periodic): me host map m21_00_00_00 world yes loading no session role 3 echoes 0 insight 6 level 37; start ready yes prologue no; item-get box 0 entries, action prompt none, guest service menu no
Party log: {...} snapshot link: host slot 0 state hosting, rtt 0 ms, tcp/none, udp resends 0 (+0), party clock 17856 ms; roster: Hunter0(s0 home m21_00_00_00 ping 0 ms) Hunter1(s1 title - ping 0 ms)
Party log: {...} snapshot vitals: s0 hp 719/719 alive map m21_00_00_00 pos (-10.8, -7.0, -21.0) lvl 37 deaths 0 age 0 ms; s1 no vitals (age 988 ms)
```

`udp resends` counts RUDP segments sent again (packet loss); `(+N)` since the last snapshot. A
roster entry marked `LOST` is a member whose connection is gone while the host keeps its slot.

## Events (verbose)

| Line | When |
|---|---|
| `travel: load start in MAP (reason)` / `load end: A -> B after N s (reason)` | Every loading screen. Reason: the last lamp / headstone warp, the host's travel event, "after the own death", or "no party reason known" |
| `travel: world up/down`, `map A -> B (no loading screen)` | Seamless map changes too |
| `summon: session role a -> b` | 6 = in the host's world |
| `death: own player DIED / slot N revived` | From the party's VITALS (hp, map, position) |
| `items: item-get box open / +N / closed after N s` | Pickups, boss rewards, party grants |
| `dialog: action prompt up: text N / gone`, `guest service menu open/closed` | What the player was looking at |
| `net: send/recv 'NAME' -> slot (N B): body` | Every link event; refusals (a `b2` `dsum_room` with no room data, `title_wait`) show here on both sides |
| `net: link state A -> B`, `high rtt to the host` | Connection problems |

The existing party lines (refusal reasons, bloodstain changes `own bloodstain ...`, the split
question `asking "Travel with the party?"` and its result, item decisions `gave / skip ...`) are
logged at every level; verbose only tags them.

## Support bundle

`scripts/support_bundle.py` (launcher button, automatic after a crash, or by hand:
`python scripts\support_bundle.py [GAME_DIR] [-o ZIP]`) zips:

- `launcher.log` and the newest 10 `logs\launcher_*.log`
- `out\crash_*.log` (newest 10), `out\hang_dump*.log`, `out\heartbeat*.log` (newest 3), `out\summary.log`
- `bbport.ini`, `launcher_settings.json` (password / code / address keys replaced by `<redacted>`)
- `info.txt`: launcher version, build (`Party: build` line or `git describe`), OS, CPU and its
  SSE4 / AVX / AVX2 / AVX-512 support, GPUs and drivers, party mode and logging level

Every text is redacted: the launcher's party password, code and public address wherever they
appear; party codes (`BBP1-...`); `password=`, `token:`, `"secret": "..."` and similar values; IPv4 /
IPv6 addresses become `<ip-N>` / `<lan-ip-N>` / `<ip6-N>` (the same address keeps its number in
every file; loopback and 0.0.0.0 stay); `C:\Users\<name>` becomes `C:\Users\<user>`; e-mail
addresses. Never included: saves (`user\`), `eboot.bin` and game files, shader / pipeline caches.
Logs over 40 MB keep their last 40 MB.
