# Live FPS clock

Ported on 3 Nov 2026 from visy's 0.7.8 (18907808, 13a35bbb), which took it from Mrsuss60's Windows port
(a7cbfae6, 83a168ee, the live-FPS part of dcdb834f; GPL-2.0-or-later). On by default, as in visy's tree.

## What runs

| | Native frame rate (before) | Live FPS clock |
|---|---|---|
| Patches | the FPS++ lists written by the engine (`engine/frame_rate.cpp`) or the FPS++ patches | `60 FPS++ (Deltatime)` (`patches/Bloodborne.xml`, appended after the Party: patches) |
| Step | fixed per rate, data sites retuned while the game holds a lower rate | the measured frame time, clamped to [1/240 s, 1/`max_dt_hz`]; +-1.5% jitter locked to the target with the rest carried |
| Code | `gpu/shim/engine/frame_rate.cpp` | `gpu/shim/engine/live_clock.cpp`, rules in `live_clock_rules.h` (`live-clock-test`) |
| Steady cloth, plant sway, jump drag (`start/frame_time_code.inc`) | on | left out: the Deltatime base handles the same code (it rewrites the drag site too) |
| Plants' wind force | from the frame-time code | fixed at 1/30 (`GrassWind`, visy 0.7.8 af45a20a: the Deltatime base used the live step as the force, so plants shook); the wave time keeps the live step. The 1/30 lives in the stub page: the Deltatime patch sets the game's own 1/30 (0x4926364) to 1/60 |

The start (`start/patches.cpp`) picks it when Live FPS is wanted (`BB_LIVE_FPS` over bbport.ini `live_fps`, never
at 30 FPS or `BB_FPS_PATCH=0`), the engine is on, and every site of the base matches the 1.09 executable
(`Bloodborne-1.09-sites.json`). It then sets `BB_LIVE_FPS_ACTIVE=1`, leaves out the FPS++ / Sprint Fix /
Performance patches (and external patches named fps++, no deltatime or performance patch), and does not set
`LANTERN_FRAMERATE_FROM_ENGINE`. Otherwise the usual frame rate, with a log line.

In the game, `LiveClock::Install` checks the step block at `0x2018e3d` byte for byte, replaces it with a call to
`LiveClockTick` (registers, flags and xmm0-15 kept), hooks the pacer's mode (`0x203493d`, `0x2034d96`) so the pacer
waits for the target, and lowers the pacer's four frame time constants to 1/240 s. `frame_rate.cpp` stands aside
but keeps counting steps from the global step (`0x535a908`), so `FrameRate::Steps/GameSeconds/LastDt` (lift watch,
world actions) work as before.

## Switches

| Switch | Meaning |
|---|---|
| `live_fps` (bbport.ini, default 1), `BB_LIVE_FPS` (wins) | on / off at the next start. Launcher: Graphics > Frame rate > Live FPS clock. Insert menu: Live FPS |
| `target_fps` (bbport.ini, 30..239, `uncap`) | the target; the launcher's frame rate sets it at start, the Insert menu changes it at once (the vblank of the start's preset still limits it) |
| `max_dt_hz`, `BB_MAX_DT_HZ` | the step's upper clamp (30 = 1/30 s, as the game) |

## Party

Frame rate stays local: the base is cosmetic (`start/party.cpp` kCosmetic), so it is not in the party patch hash, and
the clock installs no party hook. "Party: Fast rejoin" writes four of the base's bytes; the party patch wins there
(`partywins`), as with the FPS++ patches.

## Not taken from visy

The presenter change in `videoout/driver.cpp` (our presenter keeps the start's limit; the pacer holds the target);
his `run.bat` changes (ours is `bb-start.exe`; Live FPS on / off takes effect at the next start); the hooks mode (deadinside's
`bbport_frame_rate.cpp`), which our tree never had.
