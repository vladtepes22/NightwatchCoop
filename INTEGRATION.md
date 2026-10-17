# Awaken Seamless integration

This fork combines Awakenchan's published engine with visy's v0.6.5 seamless co-op.
The Windows test package targets the clean CUSA03173 game dump and its 1.09 update
at E:\games\bloodborne seamless\gamedir\CUSA03173.

## Source baselines

- Awakenchan launcher and published engine patch: f3dcca13da2c2c81d2b9904dd549277e876e19e6.
- DarkIzuku engine starting point: 8ecba9a4abdd54747e31d7fee54982e422b5f8e2
  (v0.4.0-darkizuku-preview.1), with Awakenchan's engine patch applied.
- Visy seamless implementation: ae113015554722e7e150e93c30060784694136ab (v0.6.5).

Awakenchan's published engine source is labeled 0.9.29. The installed upstream
binary is 0.9.34-beta.3. This fork retains the published renderer, optimizations,
settings, shaders and original online backend. Upstream's later binary-only
support for other game editions and modified executables is not reproduced.
The user's clean CUSA03173 target passed startup and hook validation.

## Integrated behavior

- Complete party and serverless networking modules, encrypted links and build,
  hook, parameter and gameplay-mod compatibility checks.
- Shared campaign progression, items, travel, respawn, rewards, NPC dialogue,
  chalice synchronization, pings, HUD, party rules and enemy scaling.
- One owner for the main frame hook and talk-command hook; the base engine's
  PC Options and gameplay callbacks remain active alongside seamless pages.
- WPF host/join/settings, codes, live roster, leave/rejoin, member removal,
  guest connection codes and save recovery controls, in both launcher layouts.
- Separate party saves, atomic writes, session snapshots, rotating backups,
  automatic checks after exit, and undo with a backup before restoration.
- Party role changes and bounded crash recovery restart through the launcher.
- Fork-owned updates and an isolated launcher configuration directory.
- A self-contained Windows package with native DLLs, Python and .NET runtime.

## Validation

- 189 Python tests completed successfully, with 52 platform/fixture skips.
- 39 native regression tests passed, including encrypted links, security fuzzing,
  party rules, travel/chalice logic, progression, atomic saves, launcher commands
  and menu row limits across party states.
- WPF settings tests passed; both layouts were rendered and visually inspected
  offscreen. Base graphics settings remain intact when seamless settings apply.
- Native startup verified all party patch sites and 76 gameplay hooks against
  the user's executable. New-character automation reached Central Yharnam.
- Two local headless instances joined the same world and exchanged character,
  progression and item data. Leave/rejoin succeeded.
- The self-contained package repeated host/join and leave/rejoin with development
  tools removed from PATH. Both instances used hidden windows, silent audio and
  exited normally. Their isolated party saves passed structural/progress checks.

Second-PC Internet/NAT traversal, extended campaign play, actual chalice dungeon
play and visual/performance behavior of the combined build remain user tests.
The headless runtime skips graphics draws and presentation, so these local tests
do not establish visible rendering quality or frame rate.

## Build and package

Use the explicit CLANG64 toolchain recorded in tools/build-engine.ps1:

```powershell
.\tools\build-engine.ps1 -Targets bloodborne-seamless,bb-engine-assets,bb-gpu-capabilities,seamless-tests
Copy-Item engine\out\gpu\bb-engine-assets.exe engine\out\bb-engine-assets.exe -Force
.\tools\package-seamless.ps1
```

The packaging script reuses the installed upstream shader/provider assets and
licenses, resolves native DLL imports, verifies the embedded Python signature,
publishes the launcher, and records the source commit and runtime SHA-256.
Existing game files and saves are kept in their original installations.

Every agent-run game instance must set BB_HEADLESS=1 and BB_AUDIO=none.
tools/smoke-game.py enforces these settings and checks both runtime markers.
Captures and large artifacts belong on D:\project-artifacts\BloodborneSeamless.
