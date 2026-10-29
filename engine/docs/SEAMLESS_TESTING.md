# Nightwatch Co-op test build

Open **Bloodborne Launcher.exe** in this package. Its first-run defaults select
the game at E:\games\bloodborne seamless\gamedir\CUSA03173 and Host mode for two
players. The engine uses the sibling CUSA03173-patch update when present.

1. Open the **Co-op** tab.
2. Choose Host, a unique player name and the party size (under Party options).
   Pick how friends join, then use **Copy invite** to share the host code (it is
   there before the game starts).
3. On the other PC, use the same package, game version, gameplay mods, party
   size and optional password. Select Join and paste the host code.
4. Complete each new character's prologue. Continue the party save on both PCs;
   the guest should join the host's world automatically.

Use the LAN code when both PCs share a router. UPnP is enabled by default.
The guest connection code and **Add copied guest code** provide the reverse
connection path when needed. DHT rendezvous is an opt-in on both PCs.

The launcher shows live members and supports leave, rejoin and host member
removal. In-game System pages contain Party, Party mode, Members, Rules, Pings
and scaling controls. The base engine's PC Options remain available.

An ordinary launch leaves the title screen under your control. Automatic Continue
runs for an explicit in-game Continue request or a crash restart managed by the
launcher. A leftover session marker can restore the party identity without
pressing buttons. Stop requests orderly shutdown before its timeout fallback.

## Saves and recovery

This package uses its own user folder; seamless play writes savedata_party.
The existing installations' saves remain in their original folders. Fresh
characters are the default for testing. The Co-op page can copy single-player
progress from this package's profile into party saves after confirmation.

Every party session starts with a snapshot of both save folders. Saves are
committed atomically; automatic checks run when the game exits. Stop the game
before using **Check party saves**, **Copy progress** or **Undo last party
session**. Undo backs up the current saves before restoring the last snapshot.

## Tests to try

- Join, leave and rejoin; death/respawn; lamps and travel.
- Shared items, boss rewards and progression; NPC choices and cutscenes.
- Chalice creation/glyphs, dungeon travel and layer progression.
- Graphics, controller bindings, upscaling, frame pacing and long sessions.

Local headless tests verified host/guest world entry, synchronization, reconnect,
save checks and a package start without development tools. Internet multiplayer,
actual chalice dungeon gameplay and visible rendering need testing on real PCs.
Five players is experimental.

## CPU regression check

From the source checkout, run the existing isolated world save headlessly and muted:

```powershell
$env:BB_PARTY_AUTOCONTINUE = '1'
python tools/smoke-game.py --name host --seconds 60 --max-party-tick-ms 1
```

The check requires three final in-world profiling windows with party update p99
at or below 1 ms, plus silent audio, hidden presentation and a clean exit. It
fails when the world has not loaded. Use an isolated character created by this
harness; the test never selects the installed user's save folder. Logs go to
D:\project-artifacts\BloodborneSeamless\smoke-host. Profiling is enabled only
for this test process. Rendered performance comparisons use the same scene,
mods, model detail, resolution, upscaler and frame cap in both installations.

This build uses Awakenchan's published engine source and its complete available
optimization settings. The newer upstream binary's additional game-edition and
modified-executable support is not included. Use the clean CUSA03173 1.09 dump.

Launcher configuration: %APPDATA%\bloodborne-seamless. Session logs are under
this package's logs; large crash/capture artifacts are directed to
D:\project-artifacts\BloodborneSeamless\user-tests on the configured PC.

The launcher log prints **Runtime log:** with the native engine log's path.
With the configured artifact folder, it is
D:\project-artifacts\BloodborneSeamless\user-tests\native_session.log;
previous_native_session.log preserves the preceding run.
For hitch diagnosis, enable **Settings → Frame statistics in the log** and add
`BB_PARTY_PERF=1 BB_PARTY_PERF_SECONDS=5` under **Extra variables** for the
reproduction. Party reports include maxima for the collision hook and registry
scans, including calls outside the party update. Remove those extra variables
and disable frame statistics after capturing the test.

## Two-instance co-op regression

`python tools/smoke-coop.py --label collision --seconds 240` starts a host and
guest over loopback, using copies of the isolated smoke-host and smoke-guest
party saves. Both instances run headlessly with silent audio. The result JSON
under D:\project-artifacts\BloodborneSeamless records actual world entry on
both sides, the longest continuous joined interval, process exit codes and
native log paths. Success requires at least 15 seconds joined and clean exits.
`--steps` accepts a director travel script; `--require-boundary` also requires
a logged stage-record boundary crossing with a remote character present and
at least 15 seconds joined on both instances afterward.
`--probe` selects a preserved native binary for a before/after comparison.
`--seed-host` and `--seed-guest` select existing smoke-test fixture names to
start both characters in the region under test.

The `party-collision-test` native target checks that local/NPC characters do
not access the collision registry, valid remote hits remain intact, and stale
remote hit pointers are cleared without dereferencing the freed records.
