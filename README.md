# Awaken Seamless

Bloodborne on Windows, combining Awakenchan's published engine and performance
options with visy's seamless co-op and native WPF launcher controls.

**No game files are included.** You need your own decrypted Bloodborne dump (CUSA03173) with update 1.09.

## Install

Run `Bloodborne Launcher.exe` from the assembled package. Choose your game folder,
then use **More → Seamless** to host or join a party.

See [test instructions](engine/docs/SEAMLESS_TESTING.md) for saves and multiplayer
setup, and [integration notes](INTEGRATION.md) for source versions, validation,
limitations and the Windows build/package commands. Updates use this fork;
automatic checks are disabled in the local test package.

## Credits

| | |
|---|---|
| Engine and WPF launcher base | [Awakenchan/BloodBorneButPc](https://github.com/Awakenchan/BloodBorneButPc) |
| Seamless co-op | [visy/bloodborne_pc_windows_port](https://github.com/visy/bloodborne_pc_windows_port), v0.6.5 |
| Engine starting point | [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc) (built on [bbport](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4)) |
| VRAM cleanup | adapted from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) |
| Crash and controller fixes | adapted from [AwesomeObserver](https://github.com/AwesomeObserver/bloodborne_pc), [Supermedo](https://github.com/Supermedo/bloodborne_pc) and [deadinside28](https://github.com/deadinside28/bloodborne_pc) |
| Cheats and occlusion culling research | [droogie/bbhost](https://github.com/droogie/bbhost) |
| Controller mappings | [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) |
| Trophy list | [pyL1nx/bloodborne_pc](https://github.com/pyL1nx/bloodborne_pc) |
| UI library | [WPF-UI](https://github.com/lepoco/wpfui) |

## License

The combined package is GPL-3.0-or-later (see [LICENSE](LICENSE)). The original
launcher and base engine retain their GPL-2.0-or-later notices; a copy of that
license is in [engine/licenses/GPL-2.0.txt](engine/licenses/GPL-2.0.txt).
Third-party components retain their own licenses in the package's `licenses/` folder.
