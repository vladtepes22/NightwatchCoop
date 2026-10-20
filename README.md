# Awaken Seamless

Bloodborne on Windows with **seamless co-op**: Awakenchan's **Lantern Engine 0.9.34 beta 9.5**
(performance, upscalers, PC menus and controls) with **visy's seamless co-op v0.6.8**, building on
Artificial-Sweetener's integration of the two.

Play the whole campaign together: no bells, no summon signs, no fog walls. Deaths, boss kills and
lamp travel don't end the session, and progress counts for everyone.

**No game files are included.** You need your own decrypted Bloodborne dump (CUSA03173, or another
retail release) with update 1.09.

> **Test build.** Co-op has passed automated tests and local two-player sessions on one PC; a real
> session between two PCs over the Internet is still being tested. Back up your saves.

**Guides:** [Install](#install) · [Single player](#play-single-player) · [Host a game](#host-an-online-game) ·
[Join a game](#join-a-friends-game) · [Play on the same network](#play-on-the-same-network-lan) ·
[Test co-op alone on one PC](#test-co-op-alone-on-one-pc) · [If friends can't connect](#if-friends-cant-connect) ·
[Saves](#where-the-saves-are) · [How co-op works](#how-co-op-works) · [Known issues](#known-issues) ·
[Reporting a bug](#reporting-a-bug) · [Building from source](#building-from-source) · [Credits](#credits)

## Install

1. Download `AwakenSeamless-v0.2.0-beta.1-win64.zip` from
   [Releases](https://github.com/vladtepes22/LanternEngine/releases).
2. Right-click the zip → **Extract All…** → choose a normal folder, for example `C:\Games`.
   Not `Program Files` or OneDrive: the game writes about 100 MB of generated files into its
   folder on every start.
3. Open the extracted `AwakenSeamless` folder and run **`Lantern Engine.exe`**.
   If Windows shows "Windows protected your PC", click **More info → Run anyway** (the files are
   not code-signed).
4. Choose your game folder: the one with `eboot.bin` in it.

Coming from Lantern Engine? Your settings can be copied: `bbport.ini` from your Lantern folder into
this one (graphics and key binds), and `%APPDATA%\bbport-launcher\settings.json` into
`%APPDATA%\bloodborne-seamless\` (launcher settings; keep `port_dir` empty).

## Play single player

1. In `Lantern Engine.exe`, open **More → Seamless** and set **Mode** to **Single player**.
2. Press **Play** on the main page.

Single-player uses your normal saves; co-op never touches them.

## Host an online game

1. Open **More → Seamless**:
   - **Mode:** Host
   - **Your name:** any name (1-16 letters, numbers, `_` or `-`)
   - **Party size:** how many players (host included); everyone uses the same number
   - **Password:** optional; it keeps strangers out and makes the code shorter
   - Leave **UPnP** on: it asks your router to open the party port (9307) for you
2. Press **Play** on the main page. Allow the Windows Firewall prompt the first time.
3. In the game, start a **New Game** (your first party character) or continue one.
4. Copy your code: **Copy codes** on the launcher's Seamless page, or in the game
   **System → Party → Copy Internet code**.
5. Send your friends the **Internet code** (`BBP3-…`) and the password if you set one.

The code stays the same in later sessions. Next time: Mode Host → Play → continue your character.

## Join a friend's game

1. Install this same release (everyone needs exactly the same build and gameplay mods).
2. Open **More → Seamless**:
   - **Mode:** Join
   - **Host code or address:** paste the host's code
   - **Password:** the host's, if they set one
   - **Party size:** the same number as the host
3. Press **Play** while the host's game is running. The first time, make a new party character;
   you then appear next to the host.

You can also do all of this inside the game: title screen → **Party** → **Host address** /
**Password** → **Join party game**. Later sessions: title → **Party** → **Continue party game**.

## Play on the same network (LAN)

Same as above, but the host sends the **LAN code** instead of the Internet code. Use it whenever
the players are on the same router: most routers cannot connect a PC to their own Internet address.

## Test co-op alone on one PC

Two copies of the game, a host and a guest, in small windows side by side, each with its own test
profile (your own settings are copied, never changed; your saves are not used). It needs about
11 GB of RAM per copy.

**Two launchers (the real flow):**
1. Copy `tools\Two-Launchers.cmd` and `tools\two-launchers.ps1` into your `AwakenSeamless` folder
   (beside `Lantern Engine.exe`), and start `Lantern Engine.exe` once.
2. Run **`Two-Launchers.cmd`**. The first time it copies the install to `AwakenSeamless-Guest`
   (a second game cannot run from the same folder). Two launchers open: the one with
   **Mode: Host** on its Seamless page is the host, **Mode: Join** the guest.
3. Press **Play** in the host launcher and wait until that game is in the world.
4. Press **Play** in the guest launcher. It is already set to join `127.0.0.1:9307`. To try the
   real party code instead, copy the host's **LAN code** (host launcher → Seamless → Copy codes)
   into the guest's **Host code or address** first.

**Two games without launchers:** `tools\Play-Two.cmd` (with `play-two.ps1`) starts both games
directly; the guest joins by itself.

Keyboard and controller go to the game window you clicked last. Logs: each install's
`logs\last_session.log`.

## If friends can't connect

Try these in order:

1. **Check the Network line** on the host's Seamless page while the game runs: it says whether
   the port is reachable.
2. **Forward the port:** in your router, forward **9307, TCP and UDP**, to the host PC (needed
   when UPnP is off or not supported).
3. **Guest code:** the guest copies its **guest code** (`BBG1-…`) from its Seamless page and sends
   it to the host, who clicks **Add copied guest code**. The game then connects through both routers.
4. **Tailscale (works almost always):** everyone installs [Tailscale](https://tailscale.com) and
   signs in to the same account or network. The guest enters `<host's Tailscale IP>:9307` as the host
   address.
5. **DHT rendezvous:** switch it on on **every** PC to find each other without codes (your IP is
   visible to the public BitTorrent DHT, as with any torrent client).

A guest whose build, game files, patches or mods differ from the host's is refused; the message
says what differs.

## Where the saves are

In your `AwakenSeamless` folder:

| What | Folder |
|---|---|
| Single-player saves | `user\savedata\` |
| Co-op (party) saves | `user\savedata_party\` |
| Automatic party save backups | `user\save_backups\` |

To keep playing a character from Lantern Engine, copy its `user\savedata` folder into this one's
`user\`. The Seamless page can copy your single-player saves into the party saves (the old party
saves are backed up first), check the party saves after a session, and undo the last session.

## How co-op works

Everyone plays in the **host's world**; each player keeps their own character (level, items, build,
Echoes). Each player's own save only gains progress, it never loses any.

- **Progress for everyone:** bosses, lamps, shortcuts, story steps and key items go into every
  player's own save. A guest who is behind catches up; a guest who is ahead loses nothing (in the
  session it sees the host's world).
- **Travel:** lamps ask "Travel with the party?" (yes: everyone travels together; no: only you).
- **Deaths:** a guest gets up at its own lamp; a host death respawns the whole party at the
  host's lamp; in a boss fight the dead watch the others until the boss dies or everyone is dead.
- **Loot:** world items once per player; one shared roll per enemy, everyone gets their own copy.
- **NPC quests are shared:** the first choice anyone makes applies to every player's save. If a
  questline matters to you, back up your saves first.
- **NG cycle:** the session runs at the host's cycle; your own save keeps yours.
- **Rot (on by default):** each death stacks a small penalty until you rest at a lamp; the host can
  turn it off under the game's **System → Party rules**.

Inside the game: **System → Party** (status, codes, rejoin, leave) and **System → Party mode**
(switch between host, join and single player; the game restarts).

## Known issues

- **A guest can't be damaged after one kind of respawn.** A guest that dies outside a boss fight
  and is revived at its own lamp without a loading screen can stay invulnerable until the next load.
- **A guest can see an NPC the host doesn't.** When a fresh guest joins right after its own
  prologue, its game may show clinic NPCs (Iosefka) from its own story state. Don't talk to an NPC
  only one player sees: the choice would be shared.
- **"userdata0010: the game data file is damaged"** in the log after a party session quit at the
  title screen. The file is only empty at the title; it is not damaged.
- **Two copies on one PC** need about 11 GB of RAM each.
- **Steam Deck / Linux:** not supported. The co-op layer is Windows-only; running through Proton
  is untested.

## Reporting a bug

Write down what happened and when, then send `logs\last_session.log` from **every** player's
`AwakenSeamless` folder (the previous session is in `logs\previous_session.log`). Open an
[issue](https://github.com/vladtepes22/LanternEngine/issues).

## Building from source

Windows, with [MSYS2](https://www.msys2.org) in `C:\msys64` and the CLANG64 packages from
`engine/.github/workflows/windows-offline-build.yml`, plus the .NET 8 SDK or newer.

1. `git clone --recursive https://github.com/vladtepes22/LanternEngine.git` (branch `awaken-seamless`)
2. In an MSYS2 **CLANG64** shell: `cd engine && bash build.sh` (`BB_LTO=OFF` for faster test builds).
   Build from a path without your user name in it (e.g. a `subst` drive) if you want to publish the
   binaries: debug line tables keep the build folder.
3. Tests: `ninja -C out/gpu seamless-tests && (cd out/gpu && ctest -R "party|save|seamless|game-menu|atomic")`.
4. Package (PowerShell), with an official Lantern Engine release extracted as the base:
   `tools\package-seamless.ps1 -Base <extracted>\BloodbornePC -Release`.

Source versions: Lantern Engine 0.9.34 beta 9.5 (Awakenchan, the engine patch applied to
DarkIzuku/bloodborne_pc v0.4.0-darkizuku-preview.1) with visy's co-op v0.6.8. The co-op start
(party patches, the version check of patches and mods, the restart after a crash or a role change)
is ported to Lantern's native start `bb-start.exe` (`engine/start/party.cpp`). See
[INTEGRATION.md](INTEGRATION.md) for the original integration notes.

## Credits

| | |
|---|---|
| **Lantern Engine** | by **Awaken**: the launcher, and the engine reworked from its starting point (native shader compiler, direct draw calls, draw pipeline and memory performance work, PC menus and controls, upscaler integration, online through The Hunter's Dream, fixes) |
| Seamless co-op | [visy/bloodborne_pc_windows_port](https://github.com/visy/bloodborne_pc_windows_port), v0.6.8 |
| First integration of Lantern and visy's co-op | [Artificial-Sweetener/BloodBorneButPc](https://github.com/Artificial-Sweetener/BloodBorneButPc) |
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

The combined package is GPL-3.0-or-later (see [LICENSE](LICENSE)). Lantern Engine's engine and
visy's co-op are GPL-3.0-or-later; the launcher and the base engine retain their GPL-2.0-or-later
notices, a copy of that license is in [engine/licenses/GPL-2.0.txt](engine/licenses/GPL-2.0.txt).
Third-party components retain their own licenses in the package's `licenses/` folder.
