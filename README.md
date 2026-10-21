# Nightwatch Co-op

**Play the whole of Bloodborne together with friends, on PC.** No bells, no summon signs, no fog
walls: deaths, boss kills and lamp travel don't end the session, and everyone's save keeps the
progress. Up to 4 hunters. Free and open source.

Built on the open-source **Lantern Engine 0.9.36** (Bloodborne running natively on Windows, with
upscalers and PC controls) and **visy's seamless co-op v0.7.0**.

> **Unofficial fan project.** Nightwatch Co-op is made by vladtepes22. It is not made, supported or
> endorsed by Awaken / Lantern Engine, The Hunter's Dream, FromSoftware or Sony. Please don't
> ask them for help with this build: use the Discord below.
>
> There is no public online play. Co-op runs directly between the players' PCs; this build never
> connects to The Hunter's Dream or any other online server.
>
> Bloodborne is a trademark of Sony Interactive Entertainment; the game is by FromSoftware. This
> project ships no game files, art, music or other content from the game: you need your own copy.

## What you need

- **Windows 10 or 11, 64-bit**, and a graphics card with **Vulkan 1.3** (any recent NVIDIA, AMD or
  Intel GPU with current drivers).
- **Your own decrypted Bloodborne dump**: the `CUSA03173` folder (or another retail release) with
  **update 1.09 copied over the base game**. No game files are included; the base game alone
  (1.00) does not start.
- For co-op: **everyone on the same Nightwatch release** and the same mods.

> **Beta.** It is very playable, but back up your saves (the launcher also keeps automatic
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

**Discord:** [discord.gg/sBK9FKKeup](https://discord.gg/sBK9FKKeup): help, finding players, bug reports.

**Guides:** [Install](#install) · [Single player](#play-single-player) · [Host a game](#host-a-game) ·
[Join a game](#join-a-friends-game) · [Play on the same network](#play-on-the-same-network-lan) ·
[If friends can't connect](#if-friends-cant-connect) · [Host without an open port (Radmin VPN)](#host-without-an-open-port-radmin-vpn) ·
[Saves](#where-the-saves-are) · [How co-op works](#how-co-op-works) · [Known issues](#known-issues) · [Reporting a bug](#reporting-a-bug) ·
[Credits](#credits)

## Install

1. Download `NightwatchCoop-v0.2.0-beta.9.2-win64.zip` from
   [Releases](https://github.com/vladtepes22/NightwatchCoop/releases).
2. Right-click the zip → **Extract All…** → choose a normal folder, for example `C:\Games`.
   Not `Program Files` or OneDrive: the game writes about 100 MB of generated files into its
   folder on every start.
3. Open the extracted `NightwatchCoop` folder and run **`Nightwatch Co-op.exe`**.
   If Windows shows "Windows protected your PC", click **More info → Run anyway** (the files are
   not code-signed). (`Bloodborne.exe` starts the game straight away with your launcher settings,
   e.g. from Steam.)
4. Choose your game folder: the one with `eboot.bin` in it.

### Moving from an older version

Beta 9.2 is a fresh download. Close the old launcher and the game, extract the new zip to a new
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

## Host a game

1. Open the **Co-op** tab:
   - **Mode:** Host
   - **Your name:** any name (1-16 letters, numbers, `_` or `-`)
   - **Party size:** how many players (host included); everyone uses the same number
   - **Password:** optional; it keeps strangers out and makes the code shorter
   - Leave **UPnP** on: it asks your router to open the party port (9307) for you
2. Press **Play** on the main page. Allow the Windows Firewall prompt the first time.
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
- **Rot (on by default):** each death stacks a small penalty until you rest at a lamp; the host can
  turn it off under the game's **System → Party rules**.

Inside the game: **System → Party** (status, codes, rejoin, leave) and **System → Party mode**
(switch between host, join and single player; the game restarts).

## Known issues

Being worked on (visy's list for the next co-op version):

- An elevator door can misbehave in co-op.
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

## Reporting a bug

Write down what happened and when, then send `logs\last_session.log` from **every** player's
`NightwatchCoop` folder (the previous session is in `logs\previous_session.log`). Post them on the
[Discord](https://discord.gg/sBK9FKKeup) or open an
[issue](https://github.com/vladtepes22/NightwatchCoop/issues).

## Credits

| | |
|---|---|
| **Lantern Engine** | by **Awaken** (not involved in this project): the launcher, and the engine reworked from its starting point (native shader compiler, direct draw calls, draw pipeline and memory performance work, PC menus and controls, upscaler integration, fixes) |
| Seamless co-op | [visy/bloodborne_pc_windows_port](https://github.com/visy/bloodborne_pc_windows_port), v0.7.0 |
| First integration of Lantern and visy's co-op | [Artificial-Sweetener/BloodBorneButPc](https://github.com/Artificial-Sweetener/BloodBorneButPc) |
| Engine starting point | [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc) (built on [bbport](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4)) |
| VRAM cleanup and frame pacing | adapted from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) |
| Crash and controller fixes | adapted from [AwesomeObserver](https://github.com/AwesomeObserver/bloodborne_pc), [Supermedo](https://github.com/Supermedo/bloodborne_pc) and [deadinside28](https://github.com/deadinside28/bloodborne_pc) |
| Cheats, online sign-in, occlusion culling and trophy research | [droogie/bbhost](https://github.com/droogie/bbhost) |
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
