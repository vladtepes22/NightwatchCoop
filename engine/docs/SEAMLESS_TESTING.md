# Awaken Seamless test build

Open **Bloodborne Launcher.exe** in this package. Its first-run defaults select
the game at E:\games\bloodborne seamless\gamedir\CUSA03173 and Host mode for two
players. The engine uses the sibling CUSA03173-patch update when present.

1. Open **More → Seamless** (or Seamless co-op in the classic navigation).
2. Choose Host, a unique player name and the party size. Start the game, then
   use **Copy codes** to share the host code.
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
characters are the default for testing. The Seamless page can copy single-player
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

This build uses Awakenchan's published engine source and its complete available
optimization settings. The newer upstream binary's additional game-edition and
modified-executable support is not included. Use the clean CUSA03173 1.09 dump.

Launcher configuration: %APPDATA%\bloodborne-seamless. Session logs are under
this package's logs; large crash/capture artifacts are directed to
D:\project-artifacts\BloodborneSeamless\user-tests on the configured PC.
