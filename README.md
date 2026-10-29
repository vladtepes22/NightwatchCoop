# Nightwatch Co-op

**Play the whole of Bloodborne together with friends, on PC.** Seamless co-op for up to 4 hunters:
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
- **Mods that stay optional.** Game-data mods and community additions, each one switched on and off
  from the launcher.

## Coming next

- Permadeath mode
- More optional community mods, like RenoDX HDR
- Auto-hiding HUD
- NPC forgiveness (for a price)
- Save editor
- And more to come

> **Unofficial fan project.** Nightwatch Co-op is made by vladtepes22 and is not made, supported or
> endorsed by FromSoftware or Sony. Please don't ask Awaken, FromSoftware or Sony for help with
> this build: use the Discord above.
>
> There is no public online play. Co-op runs directly between the players' PCs; this build never
> connects to The Hunter's Dream or any other online server.
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

> **Beta.** Back up your saves (the launcher also keeps automatic
> backups of the co-op saves). See [Known issues](#known-issues).

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

1. Download `NightwatchCoop-v0.2.6-beta.1-win64.zip` from
   [Releases](https://github.com/vladtepes22/NightwatchCoop/releases).
   Read the selected release's requirements and known issues; choose its zip asset, not the
   source-code archive. Everyone in your party must install the same release.
2. Right-click the zip → **Extract All…** → choose a normal folder, for example `C:\Games`.
   Not `Program Files` or OneDrive: the game writes about 100 MB of generated files into its
   folder on every start.
3. Open the extracted `NightwatchCoop` folder and run **`Nightwatch Co-op.exe`**.
   If Windows shows "Windows protected your PC", click **More info → Run anyway** (the files are
   not code-signed; only run a zip from this repository's Releases page). (`Bloodborne.exe` starts
   the game straight away with your launcher settings, e.g. from Steam.)
4. Choose your game folder: the one with `eboot.bin` in it.

### Quick FAQ

- **Does this include the game?** No. Supply your own decrypted dump with update 1.09.
- **Does it use official servers?** No. Co-op connects players' PCs.
- **Can I keep my solo character?** Solo and co-op saves are separate; read [Saves](#where-the-saves-are) before copying characters.
- **Why haven't we joined yet?** New characters finish their own prologue first; see [Host a game](#host-a-game).
- **Why can't we connect?** Check matching builds and gameplay mods, then [connection troubleshooting](#if-friends-cant-connect).
- **Why am I limited to 30 FPS?** See [Stuck at 30 FPS](#stuck-at-30-fps).

### Moving from an older version

From beta 9.2 or later, the launcher's update button installs the new version. **Close and
start the launcher again right before you press Update**: launchers before beta 10.1 hold two
engine files open once the Controls tab has been opened, and the update then fails with "being
used by another process" (fixed in beta 10.1). From an older
beta, download the zip: close the old launcher and the game, extract the new zip to a new
folder, then copy these from the old `NightwatchCoop` folder into the new one (replace when asked):

- the `user` folder: your saves (single player and party), save backups, party code and identity
- the `Mods` folder, if you have one
- `bbport.ini`: your graphics, controls and game settings

The launcher's own settings (game folder, co-op mode, name) are kept in `%APPDATA%` and carry
over by themselves. Start the new `Nightwatch Co-op.exe` once everything works, then delete the
old folder.

## Play single player

1. In `Nightwatch Co-op.exe`, open the **Co-op** tab and set **Mode** to **Single player**.
2. Press **Play** on the main page.

Single-player uses your normal saves; co-op never touches them.

## The map

**Experimental.** Turn it on in the launcher: **Display › Map › In-game map (experimental)** (off by default). The first time, the
launcher makes the maps from your own game files (a few minutes; the Play page shows the progress).
You can play meanwhile: the map turns on in the game by itself when it is ready, no restart needed.

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
have no map, and the map has no fast travel.

## Host a game

1. Open the **Co-op** tab:
   - **Mode:** Host
   - **Your name:** any name (1-16 letters, numbers, `_` or `-`)
   - **Party size:** how many players (host included); everyone uses the same number
   - **Password:** optional; it keeps strangers out and makes the code shorter
   - Leave **UPnP** on: it asks your router to open the party port (9307) for you
2. Press **Play** on the main page. Allow the Windows Firewall prompt the first time.
   If friends still cannot connect, press **Fix firewall issues** on the Co-op tab: it shows the
   PowerShell lines first, then runs them as administrator or copies them for you to paste.
3. In the game, start a **New Game** (your first party character) or continue one.
4. Copy your code: **Copy codes** on the launcher's Co-op tab, or in the game
   **System → Party → Copy Internet code**.
5. Send your friends the **Internet code** (`BBP3-…`) and the password if you set one.

The code stays the same in later sessions. Next time: Mode Host → Play → continue your character.

New characters play their own prologue first (the clinic, the first death, the first Hunter's
Dream visit). Once a player has both starting weapons, the bells are added and the party joins up
by itself; until then a joining player shows as "joining".

## Join a friend's game

1. Install this same release (everyone needs exactly the same build and gameplay mods).
2. Open the **Co-op** tab:
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

## If friends can't connect

Only the **host** needs a reachable port; guests never do. Try these in order:

1. **Radmin VPN:** see the next section. It works even when the host cannot open a port at all.
2. **Check the Network line** on the host's Co-op page while the game runs: it says whether
   the port is open, and why not.
3. **Open the port:** in the host's router, allow UPnP for the host PC, or forward **9307, TCP and
   UDP**, to it. Then restart the game.
4. **Let someone else host:** a player whose port is open can host for everyone.

**"Your internet provider uses a shared NAT (CGNAT)"** on the Network line means port forwarding
can never work for that host, however it is set up: the provider does not give the router its own
internet address (the router's outside address starts with `100.64`-`100.127`). Use Radmin VPN,
let someone else host, or ask the provider for a public IPv4 address. **"Your router is behind
another router"** means the same port has to be forwarded on the first router too (or that router
set to bridge mode).

"Wrong password" means the connection works: clear the password on every PC (or type the same one
everywhere) and restart the host's game. A guest whose build, game files, patches or mods differ
from the host's is refused; the message says what differs.

## Host without an open port (Radmin VPN)

For a host whose router can't open a port (no UPnP, no port forwarding, mobile or carrier NAT, a
"symmetric NAT" on the Network line). [Radmin VPN](https://www.radmin-vpn.com) (free, Windows)
puts the PCs in one virtual network, as if they were on the same router.

**Everyone, once:**
1. Install Radmin VPN and start it.
2. The host clicks **Network → Create network**, picks a network name and password, and sends
   both to the friends. Each friend clicks **Network → Join network** and enters them.
3. Everyone appears in the host's network list with a green dot when connected.

**Host:**
1. Note your **Radmin IP**: shown under your PC's name at the top of Radmin VPN, it starts with
   `26.` (for example `26.12.34.56`).
2. **Mode: Host** → **Play**. Leave **Host public address** empty: since beta 4 the guests send
   game traffic to the address they reached you at.
3. Send your friends your Radmin IP with the port: `<your Radmin IP>:9307`.

**Guest:**
1. Co-op page → **Mode: Join** → **Host code or address** = `<host's Radmin IP>:9307`.
   Leave **Host public address** empty.
2. The host's password (or none) and the same party size → **Play**.

To check the connection, right-click the host in Radmin VPN → **Ping**. Tailscale, ZeroTier,
Hamachi and other VPNs work the same way: the guest joins the host's VPN address. Everyone needs
beta 4 or later; with an older build the host has to set **Host public address** =
`<VPN IP>:9307`.

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

## How co-op works

Everyone plays in the **host's world**; each player keeps their own character (level, items, build,
Echoes). Each player's own save only gains progress, it never loses any.

- **Progress for everyone:** bosses, lamps, shortcuts, story steps and key items go into every
  player's own save. A guest who is behind catches up; a guest who is ahead loses nothing (in the
  session it sees the host's world).
- **Travel:** lamps ask "Travel with the party?" (yes: everyone travels together; no: only you).
- **Deaths:** a guest gets up at its own lamp after a short loading screen; a host death respawns the whole party at the
  host's lamp; in a boss fight the dead watch the others until the boss dies or everyone is dead.
- **Loot:** world items once per player; one shared roll per enemy, everyone gets their own copy.
- **NPC quests are shared:** the first choice anyone makes applies to every player's save. If a
  questline matters to you, back up your saves first.
- **NG cycle:** the session runs at the host's cycle; your own save keeps yours.
- **Rot (off by default):** with it on, each death stacks a small penalty until you rest at a lamp,
  kill the boss, kill your killer or pick up your Blood Echoes; the host turns it on under the
  game's **System → Rot** or the launcher's Co-op tab.
- **Lamps:** the host's world keeps its own lamps; anyone in the party can light one there. A
  host can let guests bring the lamps of their own world (**Guests bring their lamps**, off by
  default).

Inside the game: **System → Party** (status, codes, rejoin, leave) and **System → Party mode**
(switch between host, join and single player; the game restarts).

## Known issues

Being worked on (visy's list for the next co-op version):

- After a revive at a far lamp, lock-on can be lost until you lock on again.
- While spectating, name tags can stack.
- Key items given while summoned can show up twice in the list.
- A boss killed in the moment the host dies may not give the host the arena revive.
- Enemies in a neighbouring area can respawn after the host walks away and back.

Good to know:

- **Use the same frame rate setting on every PC.** Very high or different frame rates make
  elevators and moving platforms skip, and players can float or fall through floors.
- A guest whose build, game files, patches or mods differ from the host's is refused; the message
  says what differs.

### Stuck at 30 FPS

The game runs at 30 FPS (alone, or the whole party as soon as someone joins) although the frame
rate is set to 60: the `eboot.bin` is a pre-patched one (a 60 FPS package or similar), not the
clean 1.09. The log says so near the top:

```
Game check: eboot.bin is a pre-patched 1.09 (12 of 1390 64 KiB pieces changed, e.g. a 60 FPS package). ...
Patches: "60 FPS++" left out: this game executable is not the standard 1.09 ...
```

(Older releases word the first line `eboot.bin is 1.09 with 12 of 1390 64 KiB pieces changed ...
starting anyway`, and say it only on the first start after the eboot changed.) Fix: close the game,
copy `eboot.bin` from your dumped **1.09 update** (for example `CUSA03173-patch`) over the one in
the game folder, and start again; set the frame rate in the launcher, not with a patched eboot.
In co-op, check every player: one pre-patched eboot is enough to hold the party at 30.

## Reporting a bug

[Open a bug report on GitHub](https://github.com/vladtepes22/NightwatchCoop/issues/new/choose)
(choose **Bug report** if offered). Include your release, hardware, host/guest role, connection
type, reproduction steps and approximate event time.

Include reviewed, redacted copies of **both** `logs\last_session.log` and
`logs\previous_session.log` from each affected player who agrees to share them. After an automatic
restart, the crash may be in the previous log. If logs are unavailable, explain why.

Easiest: the launcher's title-bar logs menu → **Copy last 2 logs** puts both log files on the
clipboard (addresses, user folder, player name and passwords hidden); paste them into the issue or
Discord. Look through them before posting. Remove remaining addresses, player names, passwords and connection codes.
Never attach saves or game files. For setup help, use [Discord](https://discord.gg/sBK9FKKeup).

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

The combined package is GPL-3.0-or-later (see [LICENSE](LICENSE)). Lantern Engine's own code (its
engine code, launcher and built-in game patches) is under the GPL v3 or later with additional terms
(GPL-3.0 section 7, see [NOTICE](NOTICE)): its attribution "Lantern Engine by Awaken
(https://github.com/Awakenchan/LanternEngine)" stays with every copy, and Nightwatch Co-op is a
modified version of it, not an official Lantern Engine release. visy's co-op is GPL-3.0-or-later;
code from shadPS4, bbport and bloodborne_pc keeps its GPL-2.0-or-later notices (a copy of that
license is in [engine/licenses/GPL-2.0.txt](engine/licenses/GPL-2.0.txt)). Third-party components
retain their own licenses in the package's `licenses/` folder.
