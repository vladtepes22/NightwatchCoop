# Awaken Seamless

Bloodborne on Windows, combining Awakenchan's Lantern Engine (0.9.34 beta 9.5) and its
performance options with visy's seamless co-op (v0.6.8) and native WPF launcher controls.

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
| **Lantern Engine** | by **Awaken**: the launcher, and the engine reworked from its starting point (native shader compiler, direct draw calls, draw pipeline and memory performance work, PC menus and controls, upscaler integration, online through The Hunter's Dream, fixes) |
| Seamless co-op | [visy/bloodborne_pc_windows_port](https://github.com/visy/bloodborne_pc_windows_port), v0.6.8 |
| Engine starting point | [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc) (built on [bbport](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4)) |
| VRAM cleanup and frame pacing | adapted from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) |
| Crash and controller fixes | adapted from [AwesomeObserver](https://github.com/AwesomeObserver/bloodborne_pc), [Supermedo](https://github.com/Supermedo/bloodborne_pc) and [deadinside28](https://github.com/deadinside28/bloodborne_pc) |
| Cheats, online sign-in, occlusion culling and trophy research | [droogie/bbhost](https://github.com/droogie/bbhost) |
| Online servers | [The Hunter's Dream](https://thehuntersdream.com) |
| FSR 3.1 and FSR 4 on Vulkan | [FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK |
| FSR 4.1.1 | [fsr4vk](https://github.com/dvj5411/fsr4vk), with the AMD driver fix from [0xCydral](https://github.com/0xCydral/bloodborne_pc) |
| DLSS | NVIDIA DLSS SDK |
| Game patches | Kyo, Lance McDonald, auser1337, illusion, emoose and other community members |
| Sound | [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) |
| In-game menu | [Dear ImGui](https://github.com/ocornut/imgui) |
| Controller mappings | [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) |
| Trophy list | [pyL1nx/bloodborne_pc](https://github.com/pyL1nx/bloodborne_pc) |
| UI library | [WPF-UI](https://github.com/lepoco/wpfui) |

## License

The combined package is GPL-3.0-or-later (see [LICENSE](LICENSE)). Lantern Engine's
engine and visy's co-op are GPL-3.0-or-later; the launcher and the base engine retain
their GPL-2.0-or-later notices, a copy of that license is in
[engine/licenses/GPL-2.0.txt](engine/licenses/GPL-2.0.txt).
Third-party components retain their own licenses in the package's `licenses/` folder.
