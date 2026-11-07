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
| eboot.bin | sha256 of the loaded image of `<game>/eboot.bin` (or `eboot.elf`, a raw ELF; `eboot.bin` wins when both exist): the load segments at their addresses, as `scripts/game_check.py image_sha256`, so a SELF and a raw ELF of the same game give the same hash and those players can join each other (`gpu/shim/party/eboot_image.h`, `tests/test_eboot_image.cpp`). The log line `<file> image sha256 <prefix>` names the file used | "another game version or dump; the port needs 1.09" |
| Game params | sha256 of the dump's own `<BB_GAME_DIR>/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx`; with a separate update folder (`<game>-patch` / `-update`, bb-start sets `BB_GAME_UPDATE_DIR`) the update's copy, since the base folder's is pre-1.09 (0.2.9 fix: a host on base game + `CUSA03173-patch` refused clean guests with `974a2c13` vs `581e2830`) (not the mods overlay: a mod's gameparam counts under the mods; when the mods differ the refusal names them first and the dump clause follows as a separate problem) | "game params differ ...: use the same game dump (1.09) as the host" |
| Gameplay patches | `out/party_patch_hash.txt` (scripts/patches.py) | names only one side has: "turn on the same patches as the host (launcher Patches tab)" |
| Gameplay mods | `out/party_mods.txt` (scripts/mods.py) | names: "enable the same mods as the host (launcher Mods tab)" |
| Party patches | every game hook the party code installed (`coop::InstalledHooks()`: "<name> @+<offset>", from coop_hooks' install functions), diagnostics left out (`diagnostic_hook_name`: log / probe / trace / observer / fixture / test), sorted, BLAKE2b (`hook_set_hash`) | names only one side has: "use the same bbport build and party options (BB_PARTY_* switches) as the host" |
| Party rules | "4p:..." rule set (the leading "max N players" is not compared since 0.7.8: a guest takes the host's) | "every player needs the same BB_PARTY_FOURP* settings" |

0.7.8: hooks that only the HOST's world needs are left out of the set as well (`host_world_hook_name`: the shared-progress
flag capture `progress: ...` and `story capture ...`). A guest with `BB_PARTY_PROGRESS=0` / `BB_PARTY_STORY=0` still joins a host
that has them on (it just does not receive them; player report CL-aa5a64, same build both sides). When the hook sets still differ,
the refusal names each known option in plain words with both values (`hook_option_label`: Guest lamps, Headstones, Guest
services, Shared enemy drop roll, 4-player rules): `party options differ (Guest lamps (BB_PARTY_GUEST_LAMPS): host ON, you OFF):
change it in System > Party or the launcher so it matches the host`; a difference with an unknown hook keeps the old list of names.

### Invaders (0.7.8)

The HELLO has one more trailing byte, the role (0 cooperator, 1 invader; docs/party/invasions.md). The identity check above
runs for an invader first, with the same named refusal; only then the host's invader gate (REJECT `Invasion`) is asked.

### Not compared: party text chat (0.7.8)

Party chat (`docs/party/chat.md`) adds no game hook, no link message type and no compared setting. Its lines travel as the EVENT
`party_chat`; a member that does not know it ignores it, so chat on one side never refuses a join.

### Host-owned options (0.7.8, hostopts078)

Max players (`BB_PARTY_MAX`), Guest lamps (`BB_PARTY_GUEST_LAMPS`), Headstones (`BB_PARTY_HEADSTONES`), Guest
services (`BB_PARTY_GUEST_TALK`) and the Shared enemy drop roll (`BB_PARTY_SHARED_DROP_ROLL`) belong to the host.
Most of them decide hooks installed at boot, so a running game cannot take another value; the guest therefore learns
them before its game starts, and the named refusal (`hook_option_label`) stays only as the last resort.
Pure code: `gpu/shim/party/party_options.{h,cpp}` (`HostOptions`, text `max=4 lamps=1 headstones=1 talk=1 droproll=1`).

| Where | What | Who uses it |
|---|---|---|
| Discovery announcement (`party_discovery`, UDP 9308) | flags bit 6 (0x40) "options present" + bit 2 lamps, 3 headstones, 4 services, 5 drop roll (1 = on); `max` was already in it. Option bits alone, or bit 7, are junk (strict parser, C++ and `launcher.py parse_discovery_packet`). An older host sends none: unknown, nothing is copied. | the launcher's Join list: `discovery_adopt_env` -> `launch_game` -> `apply_host_options` sets `BB_PARTY_*` for that launch only |
| Nightwatch VPN lobby list | not changed (the lobby server's format is theirs); only the slots (`max`) | the launcher copies `BB_PARTY_MAX` |
| WELCOME | Max players (as before) | `FourpTick`: applied live on a connected guest |
| Mismatch REJECT | the reason gets a prefix `[host options: max=4 lamps=1 ...] ` (`LinkConfig::options`; the guest's `PartyLink` strips it: `reject_reason()` is the plain text, `reject_options()` the options; an older guest just shows the prefix) | the running game: compares with its own (`local_options`), says in the party status what differs and to restart, writes `<user dir>/party_adopt.txt` (`BB_PARTY_*=value` lines); the launcher's next join reads it once (known keys and values only, ignored after 2 days, deleted after use) |

Behaviour. A guest's saved settings (launcher / bbport.ini) are never overwritten: the host's values are set in the
environment of that one launch (the ini keeps the guest's own for hosting later). A guest whose game already runs
with another value of a hook-deciding option is refused as before, but the message now reads e.g. *party options
differ (Headstones: host on, you off). ... restart to join - the launcher uses the host's for your next join*.
Max players never refuses (taken live). No toggle: it only removes refusals and changes nothing for a guest that
already matches; a guest who wants another value hosts or sets the variable by hand (a hand-set `BB_PARTY_*` in the
environment loses to the host's on a join from the list).

Tests: `party-link-test` "host-owned options" (text/parse strictness, prefix round trip, diff and the plain-words
line "Max players (4-player party): host 4, you 3", a refused guest reads the host's options over the wire),
`party-net-test` (announcement bits), `tests/test_launcher_discovery.py` (parser, rows, `parse_party_adopt`,
`apply_host_options`), smoke `vm` (`VM_MODE=maxadopt`: host 4, three guests 3, all join; `VM_MODE=refuse`).

### The host's mods label (0.7.9)

The VPN lobby already lists a host's mods label (`party_vpnlobby.h` `mods_label`: `BB_PARTY_MODS_LABEL` when valid, e.g.
`nocturne@1.0.1`, else `mods:` + 8 hex of the party mod hash; none without gameplay mods; 1..255 of a-z 0-9 @ . _ : + -,
`valid_mods_label`). LAN discovery and the refusal carry the same label, so a guest can tell which mods a host uses
before the join or from its refusal. The party mod hash stays the join check.

| Where | What |
|---|---|
| LAN discovery | a third datagram after the announcement and its extension, only while the host has a label: `0xfa 'M' 1, id u32, u8 len + label` (9..263 bytes, strict `parse_mods`). An old listener drops it as junk (`parse` / `parse_ext` and `launcher.py` refuse it); the announcement itself is unchanged. |
| Mismatch REJECT | when the mods hashes differ and the host has a label: a prefix `[host mods: <label>] ` after the options prefix (`reject_with_mods`). The guest's `PartyLink` strips it (`reject_mods_label()`); the game adds "(the host's mods: <label>)" to the party status and writes `<user dir>/party_host_mods.txt` (the label, one line; removed after a refusal without one). An older guest shows the prefix as part of the reason. |
| Party codes | nothing: BBP3's four flag bits are all used and the rest of the last symbol is the check, so a "mods" bit would make the code longer or break older decoders. |

Tests: `party-net-test` (the datagram: round trip, the longest label, invalid labels never sent, strict parse, old parsers
refuse it, junk), `party-link-test` (prefix round trip and forged labels; over the wire: a refused guest reads the host's
label next to its options, a refusal over other things carries none), `tests/test_launcher_discovery.py` (the launcher's
listener ignores the datagram).

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
