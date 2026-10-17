# Party version check (ERSC parity A9)

ERSC needs the same mod and game version on every machine. bbport's PartyLink checks this in the
handshake: the guest's HELLO carries its identity, the host compares it with its own
(`identity_mismatch`, `gpu/shim/party/party_link.cpp`) and refuses a guest whose game differs with a
Mismatch REJECT. The reason names what differs and what to do about it. The guest's party UI
shows it: the status board's "host refused: ..." (System > Party, the Party page's status row and
the last event on Party members) and the overlay.

Since 0.6.2 the refusal always names **both bbport versions**: "version mismatch with the host
(host bbport 0.6.2 (...), yours 0.6.1 (...)): ..." when something else differs, or the build clause
itself ("bbport build differs (host ..., yours ...)") when the builds differ. The launcher also says
when a newer release is out (below), so players can update before they meet.

## What is compared

| Item | Source (party_runtime.cpp) | Mismatch clause tells the player |
|---|---|---|
| PartyLink protocol (`kLinkProtocolVersion`) | compiled in | "different party protocol version ...: update the port" (Version REJECT, checked first) |
| bbport build | `bbport_build_id()`: `"<VERSION> (<git describe --always --dirty>)"`, generated at build time by `gpu/cmake/bbport_build_id.cmake` (only that file recompiles after a commit) | "bbport build differs (host X, yours Y): install the same bbport release as the host" |
| eboot.bin | sha256 of `<game>/eboot.bin` | "another game version or dump; the port needs 1.09" |
| Game params | sha256 of `<game>/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx` | "game params differ ...: use the same game dump (1.09) as the host" |
| Gameplay patches | `out/party_patch_hash.txt` (scripts/patches.py) | names only one side has: "turn on the same patches as the host (launcher Patches tab)" |
| Gameplay mods | `out/party_mods.txt` (scripts/mods.py) | names: "enable the same mods as the host (launcher Mods tab)" |
| Party patches | every game hook the party code installed (`coop::InstalledHooks()`: "<name> @+<offset>", from coop_hooks' install functions), diagnostics left out (`diagnostic_hook_name`: log / probe / trace / observer / fixture / test), sorted, BLAKE2b (`hook_set_hash`) | names only one side has: "use the same bbport build and party options (BB_PARTY_* switches) as the host" |
| Party rules | "max N players, 4p:..." | "every player needs the same Max players setting" |

The party patch set reflects what really runs: a party option that installs a hook (for example
guest lamps / headstones, the 4-player hooks, B2) changes it, so two players with different
gameplay options cannot meet in a half-patched session. Diagnostic hooks (`BB_PARTY_TRACE_*`, item
probes, the NPC fixture) do not count. The set is taken when the link starts (at the title), after
`PartyInit` installed everything.

Wire: four trailing HELLO fields after the party rules (str build id, params[32], hooks[32], str hook
names). An older build sends none of them; the host then reports "unknown: an older build". The
protocol number stays 2: the Version REJECT already covers older layouts.

Overrides (host side, where the comparison runs): `BB_PARTY_CHECK_BUILD=0` skips the build id (two
builds of the same party code from different commits); `BB_PARTY_HASH_PATCHES=0` skips the gameplay
patches (as before). Params, eboot, mods and party patches are always compared.

`<user dir>/party_identity.txt` (written at link start) holds this game's whole identity (no password
or secret). `tools/party/simguest.py --identity FILE` sends it, so a simulated member passes a real
game host's check; `--build-id` / `--rules` set single fields (the harness takes `--build-id`,
`--params-hash`, `--hooks-hash`, `--rules`).

## Launcher update notice (0.6.2)

At start the launcher asks GitHub's public API for the latest release
(`https://api.github.com/repos/visy/bloodborne_pc_windows_port/releases/latest`, no token, 3 s
timeout, `scripts/update_check.py`) and, when it is newer than the launcher's own version, shows
"Version X is out - every PC in the party needs the same version" under the title with a link to
the release page (and a `[UPDATE]` log line). The answer is cached for 6 hours in
`launcher_update_check.json` next to the launcher (a failed check for 30 minutes), so starting the
launcher again does not ask again; offline, a timeout, a rate limit, a draft / pre-release or an
unexpected answer show nothing. Only a `https://github.com/` link is ever shown. Off: *Advanced /
Debug > Support > Tell me when a new version is out*. Tests: `tests/test_update_check.py` (mocked
answers: newer / same / older, pre-release, HTML, offline, timeout, rate limit, foreign link, the
6 h and 30 min caches, a broken or unwritable cache).

## Tests

- `party-link-test` "version check": both versions named when only patches / params differ, the build
  clause naming them once when the builds differ, every clause and its advice, the build override, an older
  build's empty fields, `hook_set_hash` (order / duplicates / empty set), the diagnostic-name
  classifier over the real hook names; over the wire a guest with another build and one hook fewer
  is refused with both clauses and nothing else.
- `tools/party/test_simguest.py`: "other bbport build -> REJECT mismatch (A9)" against the real host
  code (party-host-harness): `bbport build differs (host unknown, yours 0.4 (...)): install the same
  bbport release as the host`.
- Live, match: in the 4-client runs (four_players.md, "A7") every instance logged the same
  `build 0.5 (0.5-590-g5bd14843cf), params 581e28302a23..., party patches da32cddaed85... (35 hooks)`
  and all were admitted.
- Live, mismatch (2026-10-11, 2 clients, `--ienv 1:BB_PARTY_GUEST_LAMPS=0 --ienv
  1:BB_PARTY_HEADSTONES=0`): the guest logged `party patches dc9c9f68ba35... (32 hooks)` and was refused
  12 s into the run, at the title: `rejected: version mismatch with the host: party patches differ
  (host only: lamp multiplayer gate +0x49 (party lamps) @+0x12f831c, talk IsMultiplayerInProgress
  (party headstones) @+0x203c564; yours only: -): use the same bbport build and party options
  (BB_PARTY_* switches) as the host`. The status board turns that into the party UI's "host refused:
  ..." (RejectText, party-status-bridge-test). Not tried live: two different builds (needs a second
  build; covered by party-link-test and test_simguest).
