# Nightwatch Co-op

**Play the whole of Bloodborne, and all Chalice Dungeons, together with friends, on PC.** Seamless co-op for up to 5 hunters:
no bells, no summon signs, no fog walls. Deaths, boss kills and lamp travel don't end the session,
and everyone's save keeps the progress. Free and open source. Needs your own game dump (update
1.09); no game files included.

[Download a release](https://github.com/vladtepes22/NightwatchCoop/releases) · [Install](#install) ·
[Watch gameplay](https://www.youtube.com/watch?v=bYavrI0ca94) · [Report a problem](#reporting-a-bug) ·
[Discord](https://discord.gg/sBK9FKKeup)

**See it in action:** [Nightwatch Co-op on YouTube](https://www.youtube.com/watch?v=bYavrI0ca94)
(thanks to Zarok for helping make it).

## Features

- **Seamless co-op for up to 4 hunters.** Play through the whole game together, all the way
  through NG+ and beyond.
- **Your party, your rules.** Enemy health scaling by party size, party voting on lamp travel,
  rot, shared or personal lamps and more.
- **Easy to connect.** Over a VPN, UPnP, IPv6 or a forwarded port, with one code to share. No
  server in between: co-op runs directly between your PCs.
- **Made for PC.** 120 FPS and beyond, ultrawide, popular community patches, DLSS and FSR upscaling,
  frame generation, and cheats you can switch on or off.
- **Your controls.** Remap every key and gamepad button.
- **Chalice Dungeons together.** Explore every Chalice Dungeon that ships with the game as a party.
- **An in-game map**, made from your own game files (see [The map](#the-map)).
- **Splitscreen** on one PC through Nucleus Co-op.
- **Mods that stay optional.** Game-data mods available soon on Nexus Mods, that ship never seen before 
  new hunter tools and weapons that do not replace any original items in the game.

## Coming soon

- Randomizer with custom settings
- Permadeath mode
- More optional community mods, like RenoDX HDR
- Auto-hiding HUD
- NPC forgiveness
- Save editor
- And more to come

> **Unofficial fan project.** Nightwatch Co-op is made by vladtepes22 and is not made, supported or
> endorsed by FromSoftware or Sony.
>
> Bloodborne is a trademark of Sony Interactive Entertainment; the game is by FromSoftware. This
> project ships no game files, art, music or other content from the game: you need your own copy.

## What you need

- **Windows 10 or 11, 64-bit**, and a GPU supporting **Vulkan 1.3**, with current drivers.
  Vulkan support alone does not guarantee playable performance.
- **Your own decrypted Bloodborne dump**: the `CUSA03173` folder (or another retail release) with
  **update 1.09 copied over the base game**. No game files are included; the base game alone
  (1.00) does not start.
- **The clean `eboot.bin` from that 1.09 update**, exactly as dumped. **Pre-patched eboots are not
  supported** (a "60 FPS" / "1080p 60 FPS" package, or any eboot.bin already patched by another
  tool): Nightwatch makes the 60 FPS, the resolution and every co-op change itself, and each of
  them is made for the clean 1.09 bytes. On a pre-patched eboot they are left out where it
  differs: the game runs at **30 FPS** and a co-op party is held at 30. The launcher's game folder
  line warns about it after the first start ([Stuck at 30 FPS](#stuck-at-30-fps)).
- For co-op: **everyone on the same Nightwatch release** and the same mods.

## Co-op in four steps

1. Everyone installs this release ([Install](#install)) and starts `Nightwatch Co-op.exe`.
2. **Host:** Co-op tab → **Mode: Host**, pick the **Party size** → **Play**. While the game runs,
   click **Copy codes** on the Co-op tab and send your friends the **Internet code** (`BBP3-…`).
3. **Friends:** Co-op tab → **Mode: Join** → paste the code, the same **Party size** → **Play**.
4. Continue a character and you appear next to the host (a new character plays its own prologue
   first, then joins by itself).

Can't connect? The host's **Network line** (Co-op tab) says why: see
[If friends can't connect](#if-friends-cant-connect).

**Need help?** [Report a problem](#reporting-a-bug) or find players on [Discord](https://discord.gg/sBK9FKKeup).

**Guides:** [Install](#install) · [Single player](#play-single-player) · [The map](#the-map) · [Host a game](#host-a-game) ·
[Join a game](#join-a-friends-game) · [Play on the same network](#play-on-the-same-network-lan) ·
[If friends can't connect](#if-friends-cant-connect) · [Host without an open port (Radmin VPN)](#host-without-an-open-port-radmin-vpn) ·
[Saves](#where-the-saves-are) · [How co-op works](#how-co-op-works) · [Known issues](#known-issues) · [Stuck at 30 FPS](#stuck-at-30-fps) · [Reporting a bug](#reporting-a-bug) ·
[My party settings](engine/docs/party/my_party_settings.md) · [Credits](#credits)

## Install

1. Download the zip file the latest release at:
   [Releases](https://github.com/vladtepes22/NightwatchCoop/releases).
2. Right-click the zip → **Extract All…** → choose a normal folder, for example `C:\Games`.
   Not `Program Files` or OneDrive.
3. Open the extracted `NightwatchCoop` folder and run **`Nightwatch Co-op.exe`**.
   If Windows shows "Windows protected your PC", click **More info → Run anyway** (the files are
   not code-signed; only run a zip from this repository's Releases page). (`Bloodborne.exe` starts
   the game straight away with your launcher settings, e.g. from Steam.)
4. Choose your game folder: the one with `eboot.bin` in it.

### Quick FAQ

- **Does this include the game?** No. Supply your own decrypted dump with update 1.09.
- **Does it use official servers?** No. Co-op connects players' PCs Peer to Peer.
- **Can I keep my solo character?** Solo and co-op saves are separate; read [Saves](#where-the-saves-are) before copying characters.
- **I connected, but it says "In Prologue"** New characters finish their own prologue first; see [Host a game](#host-a-game).
- **Why can't we connect?** Check matching builds and gameplay mods, then [connection troubleshooting](#if-friends-cant-connect).

## The map

In the game, **hold Options / Start** (a tap still opens the game's menu) or press **M**. The map
shows where you stand and which way you face, the place you are in, lit lamps, ladders, gates, the
NPCs that are there now and your party.

| | Controller | Keyboard and mouse |
| --- | --- | --- |
| Pan | Right stick | Drag, arrow keys |
| Zoom | L2 / R2 | Wheel, - / + |
| Look at your lit lamps (any area) | L1 / R1 | Q / E |
| Back to you | Triangle | C |
| Height shading | Square | H |
| Point at an icon for its name | Left stick | Mouse |
| Close | Circle, Options / Start | Esc, M |

Optional, under Display › Map: **pickups** still lying in the world (Blood Vials, Insight items,
weapons and attire, Coldblood) and **boss reveal** (an area's map stays hidden until its boss is
dead). Change the map button under **Controls** (the undo arrow puts it back). Chalice Dungeons
have no map.

## Host a game

1. Open the **Co-op** tab:
   - **Mode:** Host
   - **Your name:** any name (1-16 letters, numbers, `_` or `-`)
   - **Party size:** how many players.
   - **Password:** it keeps strangers out.
   - Leave **UPnP** on: it asks your router to open the party port (9307) for you, if you want to play over internet.
2. Press **Play** on the main page. Allow the Windows Firewall prompt the first time.
   If friends still cannot connect, press **Fix firewall issues** on the Co-op tab.
3. Load a save, or start one, as usual.
4. If playing over VPN, you do not have to do anything, your lobby shows up automatically on guests join page.
5. Copy your code: **Copy codes** on the launcher's Co-op tab, or in the game
   **System → Party → Copy Internet code** if playing over Internet.
6. Send your friends the **Internet code** (`BBP3-…`) and the password if you set one.

The code stays the same in later sessions.

New characters play their own prologue first (the clinic, the first death, the first Hunter's
Dream visit). Once a player has both starting weapons and teleports to any lamp, the party joins up
by itself; until then a joining player shows as "joining" or "Prologue".

## Join a friend's game

1. Install this same release (everyone needs exactly the same build and gameplay mods).
2. Open the **Co-op** tab:
   - **Mode:** Join
   - **If playing over VPN, or LAN:** select your friends Lobby, it will ask for a password and you will join automatically.
   - **Host code or address:** paste the host's code, or select
   - **Password:** the host's, if they set one
3. Press **Play** while the host's game is running. You can load an old save, or create a new character (and finish the prologue)

## If friends can't connect

Only the **host** needs a reachable port; guests never do. Try these in order:

1. **VPN:** see the next section. It works even when the host cannot open a port at all.
2. **Check the Network line** on the host's Co-op page while the game runs: it says whether
   the port is open, and why not.
3. **Open the port:** in the host's router, allow UPnP for the host PC, or forward **9307, TCP and
   UDP**, to it. Then restart the game.
4. **Let someone else host:** a player whose port is open can host for everyone.

## Host without an open port (Using VPN)

For a host whose router can't open a port (no UPnP, no port forwarding, mobile or carrier NAT, a
"symmetric NAT" on the Network line). An easy to setup VPN is [Radmin VPN](https://www.radmin-vpn.com) (free, Windows)
puts the PCs in one virtual network, as if they were on the same router. Any VPN works.

**Everyone, once:**
1. Install Radmin VPN and start it.
2. The host clicks **Network → Create network**, picks a network name and password, and sends
   both to the friends. Each friend clicks **Network → Join network** and enters them.

**Guest:**
1. Co-op page → **Mode: Join** → the Hosts game shows up as a joinable Lobby automatically.

## Where the saves are

In your `NightwatchCoop` folder:

| What | Folder |
|---|---|
| Single-player saves | `user\savedata\` |
| Co-op (party) saves | `user\savedata_party\` |
| Automatic party save backups | `user\save_backups\` |

To keep playing a character from Lantern Engine, copy its `user\savedata` folder into this one's
`user\`. The Co-op page can copy your single-player saves into the party saves (the old party
saves are backed up first), check the party saves after a session, and undo the last session.

### Stuck at 30 FPS

The game runs at 30 FPS (alone, or the whole party as soon as someone joins) although the frame
rate is set to 60: the `eboot.bin` is a pre-patched one (a 60 FPS package or similar), not the
clean 1.09. The log says so near the top:

## Reporting a bug

Easiest: the launcher's title-bar logs menu → **Copy last 2 logs** puts both log files on the
clipboard (addresses, user folder, player name and passwords hidden); paste them into our Discord Server, our Bot instantly fetches your logs, makes a bug report then deletes them from the server.

## Credits

Nightwatch Co-op is a modified version of **Lantern Engine by Awaken (https://github.com/Awakenchan/LanternEngine)**
0.9.38, not an official Lantern Engine release, combined with visy's seamless co-op.

| | |
|---|---|
| Seamless co-op | [visy/bloodborne_pc_windows_port](https://github.com/visy/bloodborne_pc_windows_port), v0.7.6+a53d3dd2 |
| **Lantern Engine by Awaken (https://github.com/Awakenchan/LanternEngine)** | the launcher, the built-in game patches, and the engine reworked from its starting point (native shader compiler, direct draw calls, draw pipeline and memory performance work, PC menus and controls, upscaler integration, cheats, occlusion culling, trophies, fixes) |
| The map: its generator, drawing, art and controls | [Hunter's Atlas](https://github.com/MikeTat1988/HuntersAtlas) by MikeTat1988 |
| First integration of Lantern and visy's co-op | [Artificial-Sweetener/BloodBorneButPc](https://github.com/Artificial-Sweetener/BloodBorneButPc) |
| Engine starting point | [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc) (built on [bbport](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4)) |
| Older CPU support (AMD FX, Intel 2nd/3rd generation Core) | the translation plan and its checks from [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc), with the CPU part of T0ug's Ivy Bridge work there, as integrated by visy (GPL-2.0-or-later) |
| Renderer crash fixes | adapted from [AwesomeObserver](https://github.com/AwesomeObserver/bloodborne_pc) and [deadinside28](https://github.com/deadinside28/bloodborne_pc) |
| Online backend (in the engine, not used: this build plays offline) | [droogie/bbhost](https://github.com/droogie/bbhost) |
| FSR 3.1 and FSR 4 on Vulkan | [FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK |
| FSR 4.1.1 | [fsr4vk](https://github.com/dvj5411/fsr4vk), with the AMD driver fix from [0xCydral](https://github.com/0xCydral/bloodborne_pc) |
| DLSS | NVIDIA DLSS SDK |
| Sound | [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) |
| In-game menu | [Dear ImGui](https://github.com/ocornut/imgui) |
| Controller mappings | [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) |
| UI library | [WPF-UI](https://github.com/lepoco/wpfui) |
| Playtesting | yorochi, Mal1216, Zarok, Banana U Pidžami and visy: thank you! |
| Video | Zarok, for helping make the [YouTube video](https://www.youtube.com/watch?v=bYavrI0ca94) |

## License

The combined package is GPL-3.0-or-later (see [LICENSE](LICENSE)). Lantern Engine's own code is under the GPL v3 or later with additional terms
(GPL-3.0 section 7, see [NOTICE](NOTICE)): its attribution "Lantern Engine by Awaken
(https://github.com/Awakenchan/LanternEngine)" stays with every copy, and Nightwatch Co-op is a
modified version of it, not an official Lantern Engine release. visy's co-op is GPL-3.0-or-later;
code from shadPS4, bbport and bloodborne_pc keeps its GPL-2.0-or-later notices (a copy of that
license is in [engine/licenses/GPL-2.0.txt](engine/licenses/GPL-2.0.txt)). Third-party components
retain their own licenses in the package's `licenses/` folder.
