# Awaken Seamless

Bloodborne on Windows with **seamless co-op**: Awakenchan's **Lantern Engine 0.9.34 beta 9.8**
(performance, upscalers, PC menus and controls) with **visy's seamless co-op v0.6.9**, building on
Artificial-Sweetener's integration of the two.

Play the whole campaign together: no bells, no summon signs, no fog walls. Deaths, boss kills and
lamp travel don't end the session, and progress counts for everyone.

**No game files are included.** You need your own decrypted Bloodborne dump (CUSA03173, or another
retail release) with update 1.09.

> **Test build.** Back up your saves.

**Discord:** [discord.gg/sBK9FKKeup](https://discord.gg/sBK9FKKeup): help, finding players, bug reports.

**Guides:** [Install](#install) · [Single player](#play-single-player) · [Host a game](#host-an-online-game) ·
[Join a game](#join-a-friends-game) · [Play on the same network](#play-on-the-same-network-lan) ·
[If friends can't connect](#if-friends-cant-connect) · [Host without an open port (Radmin VPN)](#host-without-an-open-port-radmin-vpn) ·
[Saves](#where-the-saves-are) · [How co-op works](#how-co-op-works) · [Reporting a bug](#reporting-a-bug) ·
[Credits](#credits)

## Install

1. Download `AwakenSeamless-v0.2.0-beta.6-win64.zip` from
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

New characters play their own prologue first (the clinic, the first death, the first Hunter's
Dream visit). Once a player has both starting weapons, the bells are added and the party joins up
by itself; until then a joining player shows as "joining".

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

## If friends can't connect

Only the **host** needs a reachable port; guests never do. Try these in order:

1. **Check the Network line** on the host's Seamless page while the game runs: it says whether
   the port is open.
2. **Open the port:** in the host's router, allow UPnP for the host PC, or forward **9307, TCP and
   UDP**, to it. On a FRITZ!Box: *Internet → Freigaben → Portfreigaben → Gerät für Freigaben
   hinzufügen* → pick the PC → tick *Selbstständige Portfreigaben für dieses Gerät erlauben*
   (or add port 9307 for TCP and for UDP), then restart the game.
3. **Let someone else host:** a player whose port is open can host for everyone.
4. **Guest code:** the guest copies its **guest code** (`BBG1-…`) from its Seamless page and sends
   it to the host, who clicks **Add copied guest code** while the guest is trying to join. This
   works when the host's router is a "cone" NAT (the Network line says which).
5. **Radmin VPN:** see the next section. It works even when the host cannot open a port at all.

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
1. Seamless page → **Mode: Join** → **Host code or address** = `<host's Radmin IP>:9307`.
   Leave **Host public address** empty.
2. The host's password (or none) and the same party size → **Play**.

To check the connection, right-click the host in Radmin VPN → **Ping**. Tailscale, ZeroTier,
Hamachi and other VPNs work the same way: the guest joins the host's VPN address. Everyone needs
beta 4 or later; with an older build the host has to set **Host public address** =
`<VPN IP>:9307`.

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

## Reporting a bug

Write down what happened and when, then send `logs\last_session.log` from **every** player's
`AwakenSeamless` folder (the previous session is in `logs\previous_session.log`). Post them on the
[Discord](https://discord.gg/sBK9FKKeup) or open an
[issue](https://github.com/vladtepes22/LanternEngine/issues).

## Credits

| | |
|---|---|
| **Lantern Engine** | by **Awaken**: the launcher, and the engine reworked from its starting point (native shader compiler, direct draw calls, draw pipeline and memory performance work, PC menus and controls, upscaler integration, online through The Hunter's Dream, fixes) |
| Seamless co-op | [visy/bloodborne_pc_windows_port](https://github.com/visy/bloodborne_pc_windows_port), v0.6.9 |
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
