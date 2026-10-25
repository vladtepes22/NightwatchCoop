# Party co-op (playing together)

> **Status (0.7.6):** party co-op has been played for real: on one PC with several game copies, on
> a LAN between two PCs, and over the Internet in two long sessions between homes, plus many
> reports from players with 3 and 4 player parties and VPNs. Every report so far was fixed in the
> next release. It is still young software: keep the save backups (below) and report problems
> with the logs. What is still open is listed under [Known limitations](#known-limitations).

Party co-op lets 2 to 4 players play the Bloodborne campaign together, from character creation
to the ending. One player **hosts**: their game acts as the server, so there is no account, no
PSN and no central server. The others **join** with a short **party code** the host sends them.

Every player keeps their own character and save. Guests are brought into the host's world
automatically, stay through boss kills and area changes, travel with the host, and the party's
story progress, key items, rewards and cutscenes are carried over into each player's own save.

## Contents

1. [What you need](#what-you-need)
2. [Starting a party game in the game (no launcher)](#starting-a-party-game-in-the-game-no-launcher)
3. [Hosting](#hosting)
4. [Joining](#joining)
5. [The party code and the password](#the-party-code-and-the-password)
6. [Ports, UPnP, firewall, VPN](#ports-upnp-firewall-vpn)
7. [What happens in the game](#what-happens-in-the-game)
8. [Deaths, wipes and limbo](#deaths-wipes-and-limbo)
9. [What is shared, and what is not](#what-is-shared-and-what-is-not)
10. [After an ending: New Game+](#after-an-ending-new-game)
11. [Settings (Party rules and My party)](#settings-party-rules-and-my-party)
12. [Saves and backups](#saves-and-backups)
13. [Crashes and disconnects](#crashes-and-disconnects)
14. [Four players](#four-players)
15. [Troubleshooting](#troubleshooting)
16. [Known limitations](#known-limitations)

## What you need

- Every player: this port, set up and working single-player, with the **1.09** game files.
- The **same build** of the port (the newest release). An older build is refused with a message
  that says so.
- The **same gameplay patches and gameplay mods** on every PC. Graphics, FPS, resolution and
  other cosmetic patches and texture / sound mods do not matter; anything that changes gameplay
  must be the same, or joining is refused (the message says which patch or mod differs).
- The **same Max players** setting on every PC (Party tab).
- The Old Hunters DLC on all PCs or on none (the port turns it on automatically when the game
  files contain it, so this normally matches).

## Starting a party game in the game (no launcher)

The launcher's Party tab is optional: a party game can be started, joined and continued from the
game's own title screen. The title menu has a **Party** item right after *New Game* (the same page
as *System > Party*). Rows are pressed with left/right; the ones that restart the game need a
second press.

**The first session (fresh characters for everyone):**

- **Host**: *Party > New party game*. The game restarts by itself (a few seconds) into the party
  game: the party saves (`user\savedata_party`), the online title, hosting. Choose **New Game** and
  make your hunter. The party code is under *System > Party > Copy party code* (and in
  `user\party_code.txt`); send it to your friends. It stays the same in later sessions and
  launches until the host makes a new one (*System > Party mode > New party code*, or **New code** in
  the launcher's Party tab while the game is closed); setting or removing a password also gives a
  new code once.
- **Friends**: *Party > Host address* (type the host's party code; **Ctrl+V** pastes; Enter
  confirms), *Password* if the host set one (the host sets it on the same page before *New party
  game*), then *Join party game*. The game restarts, joined to the host: choose **New Game** and
  make your hunter.
- Everyone plays the prologue on their own, then the party gathers by itself (below). If you are
  short of time, the host can turn on *Skip the prologue* (below).

**Later sessions:** host and friends alike: *Party > Continue party game*. The game restarts, loads
your party character by itself and hosts or joins (your last role) with the saved address and
password; the summons follow automatically.

**In the world** (party game), *System > Party* also has **Host party** / **Join party** (switch
role: a quick restart, your character continues) and **Single player** (back to the
single-player game and saves). With a single-player character loaded there are no party actions:
party games always start from the title screen, so co-op can never write into a single-player save.

The title's *Continue* / *Load Game* / *New Game* are always the single-player game. The settings
are kept in `user\party.json`: your party name, last role, the host address, the password (stored
protected with Windows DPAPI: only your Windows user on this PC can read it back), whether you
have a party character. Details: [party/ingame_flow.md](party/ingame_flow.md).

## Hosting

1. Start the launcher (`launch_gui.bat`) and open the **Party** tab.
2. **Mode: Host a party.** Enter **Your name** (1-16 letters, digits, `_` or `-`; the others see
   it in the game).
3. Pick **Max players** (3 is the game's normal co-op size; 4 is experimental).
4. Leave **Automatic summoning** and **Seamless party rules** on (*Separate party saves* is always on).
5. Optional: a **Password** (every player must then enter the same one; see
   [the password rule](#the-party-code-and-the-password)).
6. Click **Launch Bloodborne**. Allow the Windows Firewall prompt (see
   [firewall](#ports-upnp-firewall-vpn)). Party play always uses its own saves; the first time the
   launcher offers to copy your single-player save into the party save folder (or start with an
   empty one for a new character).
7. Once the game runs, your two **party codes** appear (the launcher log shows them too):
   **Internet code** for friends elsewhere and **LAN code** for players on your own network (or on
   your VPN, see below). Click **Copy** next to one and send it (chat, mail ...).
8. Play. In party mode the game confirms **Continue** on the title screen by itself a few
   seconds after the party is up, so your last character loads without input (with an empty
   party save, create a character as usual). The others are brought into your world
   automatically when they are ready.

The code stays the same when the game restarts after a crash and from one launch to the next; it
changes when your public address or port changes, when you make a new code, or once when you set
or remove a password.

**Skip the prologue.** The host rule *Skip the prologue* (*System > Party rules: Phantoms*, or the
launcher's Party Rules tab) lets a brand-new character start at the Central Yharnam lamp with the
Saw Cleaver, the Hunter Pistol and Iosefka's Blood Vial, ready to be summoned. A character that
already left the clinic is never touched.

## Joining

1. **Party** tab, **Mode: Join a party**, enter **Your name** (must differ from the other
   players' names).
2. Paste the host's code into **Party code** (the **Paste** button takes it from the clipboard).
   On a VPN or LAN you can also pick the host from the **Parties found** list and press **Join
   selected** (see [party discovery](#ports-upnp-firewall-vpn)).
3. Set **Max players** (and the **Password**, if the host uses one) to the same values as the host.
4. Start the game **after the host's game is running**. Once connected, the game confirms
   **Continue** on the title screen by itself (with an empty party save, create a character).
5. Play the prologue in your own world (see below). Once you are ready, the host's game brings
   you in; you appear beside the host.

The launcher shows a check mark or a cross under the code field if the name, port or code look
wrong.

In the game, the game's own menu has two party pages under **System** (on the title
screen and in the pause menu, after *Environment* (Screen/Sound)): **Party** (the state and your role, *Copy party
code* to the clipboard, *Rejoin* / *Summon again*, *Leave the party*, *Gather the party* for the host, plus the party flow's
rows to start or join a party game) and **Party members** (each hunter with their state and ping and
the last event; the host kicks a hunter by pressing left/right twice on their row; the
connection warnings switch is here too). *Leave* also asks for a second press. The
bottom line of each row explains it. The overlay menu (**Insert**, or **L3 + R3** if enabled) has
the same **Party** tab. Players joining and leaving are shown as short messages.

**Party HUD.** In party mode a small panel in the bottom left corner lists your companions: name,
a thin HP bar (stamina under it), their ping in ms (green under 80, yellow under 160, red above)
and, while they are on the same map, how far away they are and an arrow pointing their way.
Companions who are not in your world (at home, loading, in their prologue, disconnected) are
greyed and say where they are. Companions in your world also get a name tag above their head
(with level, health, deaths or ping, as you choose). Switch it in the game (**System > My party:
Display > Party HUD**, and *Name tags*), the overlay's Party tab or the launcher's My Party tab
(on by default; never shown in single player).

**Hotkeys.** A **ping** (a marker everyone in your world sees through walls for 10 seconds): hold
the touchpad and press R3 (Xbox pads: hold Back, press R3; keyboard: `V`). **Gather the party**
(host): hold the touchpad and press L3 (Xbox pads: Back + L3; keyboard: `H`).

## The party code and the password

A code looks like `BBP3-NJR0-E43N-WWN9-2C2P` (Internet code of a host with a password) or
`BBP3-PAJY-EAMH-7TD` (LAN code). It holds the host's address, the port (only when it is not
9307) and a secret; the secret is what lets a guest in, so share the code only with the people
you play with. Letters are not case-sensitive, I/L count as 1 and O as 0, and a checksum catches a
mistyped character. The LAN code carries only the last number of the host's address: the guest
takes the first three from its own address (same home network), or searches the LAN when it has
none. Older `BBP2-...` and `BBP1-...` codes still work.

| Code (port 9307) | With a password | Without |
|---|---|---|
| Internet | 16 characters after `BBP3-` | 19 |
| LAN | 11 | 14 |

Another port adds 3 characters.

**The password rule.**

- **Host without a password:** only the **party code** gets a guest in. A plain `address:port`
  is refused (*"this host has no password: join with its party code"*), so the code is the only
  key. Guests who use a code ignore a password saved in their launcher.
- **Host with a password:** the guest needs the **password**; with it, a plain `address:port`
  works too (this is how the *Parties found* list and VPN addresses join). The code is shorter,
  because the password does the securing: the party is exactly as safe as its password, so pick
  one that is not easy to guess. Setting or removing a password gives the host a new code once,
  so send the new one.
- Five wrong passwords within a minute: the host ignores that address for a while.

**Two codes.** The host has two codes (`user\party_code.txt`, the launcher, *System > Party* and
the overlay's Party tab):

| Code | Holds | Use it for |
|---|---|---|
| **Internet code** | Your public address (found automatically) | Friends elsewhere |
| **LAN code** | Your address on your home network, or your VPN address when you host over a VPN | Players on the same network (same router) or the same VPN |

Most home routers cannot reach their own public address from inside. A guest on your network who
uses the Internet code therefore searches the LAN by itself when the public address does not
answer (adds about 2 s). The search sends only a hash made from the code's secret, never the
secret.

The code's secret is new for each hosting session, except when the game restarts after a crash or
the party is started again from the title screen's *Party* page.

## Ports, UPnP, firewall, VPN

- The party uses **one port on the host, UDP and TCP, default 9307** (Party tab, *Port*). Guests
  connect to it; guests do not need any open port.
- **UPnP** (on by default) asks the host's router to open that port while the game runs. Many
  routers allow this; if yours does not, the log says *"UPnP unavailable: forward UDP+TCP port
  9307 to this PC in your router"*. Then forward the port by hand in the router's settings
  (UDP and TCP, to the host PC's LAN address).
- **Windows Firewall**: the first time you host, Windows may ask whether `bbport.exe` may use the
  network. Click **Allow** (at least on *Private* networks; also *Public* if Windows calls your
  home network public). If you clicked *Cancel*, allow the program in *Windows Security > Firewall
  > Allow an app through firewall*. Every player does this, host and guest: the host needs it for
  guests to get in, the guest for the host's replies. The game never changes your firewall; since
  0.7.6 it checks the rules at start and shows a **connection warning** with the shortest fix if
  there is no rule or it is blocked.
- **Public address**: normally found automatically (STUN server `stun.l.google.com:19302`, or
  UPnP). For a fixed IP, a DNS name or a VPN address, enter it in *Public address* (host only).
  *STUN server* `off` turns the lookup off (LAN / VPN only).
- **Which NAT do I have?** The *Party* page's *Network* line (and the log's `NAT:` line) says it.
  A "cone" NAT can be punched. A **strict NAT** (the game says *"a strict router or your ISP's
  carrier NAT"*) cannot: a strict router is fixed with UPnP or by forwarding UDP+TCP 9307; a
  provider's carrier NAT cannot be fixed on your side. To tell them apart, compare the router's WAN
  address (in the router's status page) with your public address (any "what is my IP" site): if
  they differ, or the router's address starts with `100.64` to `100.127`, it is the provider.
- **Carrier-grade NAT / no port forwarding possible**: use a VPN that puts all players in one
  virtual network (Radmin VPN, Hamachi, ZeroTier, Tailscale), or let a player with an open port
  host.
- **No UPnP and no port forward: guest codes.** Each guest's overlay *Party* page shows *Your
  guest code* (`BBG1-XXXX-XXXX-XXXX`, its public UDP address; also `user/party_guest_code.txt`).
  The guest copies it and sends it to the host; the host copies it and presses *Add a guest code
  (paste)*. The host then punches a path through its router towards that guest, and the party
  link switches to UDP when TCP gets no answer. Works with most home routers; not with a strict
  NAT. Details: [party/network_robustness.md](party/network_robustness.md).
  This can also happen **by itself**, opt-in: with *Find each other through the public DHT* on
  (launcher *Party* tab or *System > Party network*; **both** host and guest must turn it on), a
  guest that cannot reach the host leaves its address in the public BitTorrent DHT under a hash
  only the party can compute, and the host finds it there and opens the way. It is **off by
  default**: DHT nodes see your IP address, as with any torrent client.
- **IPv6** (off by default; *Party* tab or *System > Party network*, both PCs): the party tries it
  before any relay, and a host without an open port then has a longer code that contains its IPv6
  address.
- **Through a member**: if the host cannot be reached but another member can, a guest can join
  through that member (*Join through a member* on its *Party* page; on by default).
- Guests talk to each other directly when they can; when a direct path does not work, their game
  traffic goes through the host automatically.

### Hosting over a VPN (Radmin VPN, Hamachi, ZeroTier, Tailscale)

1. Everyone installs the same VPN and joins the same VPN network.
2. **Host:** start the game as usual. With *Host over a VPN* on **Auto** (default; launcher
   *Party* tab, *System > Party network*), the game finds the VPN adapter by itself (Radmin VPN,
   Hamachi, ZeroTier, Tailscale; commercial VPNs such as NordVPN are never picked in Auto, only
   by name). The launcher shows *Hosting over Radmin VPN: 26.x.y.z* and a **VPN address** line
   with **Copy** (always `address:port`). The **LAN code** now holds the VPN address.
3. **Guests** in the same VPN paste the VPN address or the LAN code into **Party code**. A bare
   address without `:port` gets the party port (9307) added. Without a host password, use the
   host's LAN code, not the bare address.
4. Everyone allows the game in the Windows Firewall (above).

If the game keeps using your home address instead of the VPN one (the log says so; `net_doctor`
reports "a VPN running but not used"), force the address with `BB_PARTY_LOCAL_IP`, on **both**
the host and the guests, each with their **own** VPN address (shown in the VPN app):

1. Press the **Windows key**, type `cmd` and press Enter.
2. Type `setx BB_PARTY_LOCAL_IP 26.12.34.56` (your own VPN address) and press Enter. It says
   *SUCCESS*.
3. **Close that window.**
4. **Close the launcher and the game completely**, then start the launcher again. Windows hands a
   new setting only to programs started afterwards; if unsure, restart the PC.
5. Check: open a new `cmd` window and type `echo %BB_PARTY_LOCAL_IP%`. It must show your address.

To remove it again: open `cmd` and run `reg delete HKCU\Environment /v BB_PARTY_LOCAL_IP /f`,
then restart the launcher.

### Party discovery on a VPN or LAN

While a game hosts, it announces the party every second or two by broadcast on your Radmin /
Hamachi / ZeroTier adapters and on home-network adapters, never over the Internet, a commercial
VPN or a virtual switch (Hyper-V, WSL, Docker). It carries the host's name, level, players,
area, whether there is a password, and the version. The password and the party code are never
sent. The launcher's **Parties found** list (Party tab) shows what it hears (greyed out when the
host runs another version); **Join selected** (or double-click) fills in the address, asks for
the password only if the party has one, and starts the game as a guest. Tailscale passes no
broadcasts: with Tailscale up, its online friends are listed from the `tailscale` command (when it
is installed) as "maybe hosting". Hosting without a password over Radmin VPN or Hamachi shows a
warning in the launcher, because strangers can be in those networks. Switch it off with *Party
discovery* (launcher, or *System > My party: Look & notices*).

### Connection doctor (`net_doctor.bat`)

When players cannot join or keep dropping, collect the logs of **one session** from the host and
every guest (the game's `logs\last_session.log`, or `previous_session.log` if you restarted) and
**drop them all onto `net_doctor.bat`** in the port's folder. It explains in plain words what is
wrong and what each player should do, and copies the reply to the clipboard, ready to paste into
Discord. It recognises, among others: different game files or versions, a refusal and its
reason, a phone hotspot, no open port, a strict NAT, a VPN running but not used, a game talking
to its own address, game traffic that never reaches the host, dropouts and high ping, a player
stuck in the prologue, and different New Game+ cycles.

Before you post the reply publicly, run it with `--redactnames` (names become Host / Guest 1 ...)
and `--redactips` (public addresses hidden, VPN and LAN ones shortened to their first number),
for example from a command window: `net_doctor.bat --redactnames --redactips host.log guest.log`.

## What happens in the game

1. **Prologue, each player alone** (unless the host skips it). Everyone creates their character
   (or loads one) and plays the short prologue in their own world: the opening, the clinic, the
   first visit to the Hunter's Dream and taking a weapon. Until then the others see *(prologue)*
   next to your name. The party gives each player the Beckoning Bell and the Small Resonant Bell
   when the prologue is done.
2. **Joining.** There are no bell, beckoner or summon-sign waits: when a guest is ready, it stands
   beside the host within seconds (about 5 s in the same area; a join that stalls restarts by
   itself). *Continue party game*, or joining from another map (the Hunter's Dream, say), takes
   one loading screen. No Insight is needed or spent, and it works in areas where the normal game
   forbids co-op, including after the area's boss is dead. If the host is in a boss fight or a
   cutscene, your join waits in your own world until it is over (the Party status says why).
3. **Together.** There are no multiplayer walls and no leash: the party can split up, and each can
   play on in another part of the city for minutes. Players in different maps see only those in
   their own map; meeting again shows them at once. Nobody is sent home when a boss dies; the
   session never ends by itself. Every player can use lamps (the normal game disables them in
   co-op), and **lamps are shared** (below).
4. **Travel.** Lamps, headstones and the Hunter's Mark ask **"Travel with the party?"**: *Yes*
   moves the whole party with one loading screen, *No* moves only you. A lamp offers the Hunter's
   Dream and every lit lamp, straight from any lamp to any lamp. A host's trip inside its current
   map is an instant warp. The host can turn on **voting** (*Party votes on travel*): the party
   then travels only when everyone agrees, and one *No* or no answer within 20 seconds cancels it.
   **Resting** at a lamp resets the world for the party (enemies respawn; everyone's HP, vials
   and bullets refill).
5. **Gather the party.** The host's *System > Party > Gather the party* (or the hotkey above)
   brings every guest to the host after the guest says *Yes*: no loading screen in the host's map,
   one from another map. A guest who is dead, in a menu, in a cutscene or in a boss fight is not
   moved, and the host cannot gather in its own boss fight or cutscene, on a ladder, in mid-air,
   or more than once in 30 s. The host can turn gathering off.
6. **Bosses.** Anyone may walk through a boss fog first, a first fight included: the fight starts
   for everyone and the others follow through the fog (a first fight plays the intro cutscene on
   every screen). A few bosses start only with what the host does in their room (Ebrietas, Lady
   Maria, Laurence); a guest at that fog is told what starts it. A guest who travelled alone
   cannot start a boss fight where the host is not.
7. **Cutscenes.** Story cutscenes play for everyone, also one a guest triggers (the Blood Moon,
   the Cathedral Ward door, Gehrman's scenes, the ending). After the Blood Moon the host is asked
   "Travel with the party?" and everyone goes to Yahar'gul together. A Snatcher's kidnap, host or
   guest, wakes the whole party up together in the Hypogean Gaol (the host can turn this off in
   *Party rules: Deaths*).
8. **Any level can play together.** Nobody is turned away for their level. As in the game's own
   co-op, a hunter far above the host's level is scaled down in the host's world (*Level sync*),
   and everyone who joins has **30 % less maximum health** (*Phantom health cut*). The host can
   turn both off. Enemies and bosses scale up with the number of hunters.
9. **Chalice Dungeons together.** Guests join the host's dungeon; levers, doors, the layer boss and
   the lifts work for the whole party, layer after layer. Any player can perform the ritual: the
   host's altar then offers *Enter the party's dungeon*. Chalices are shared between all players
   (Root Chalices, which a ritual uses up, stay each player's own). The dungeon's lamps work with a
   party. A guest who dies there gets up beside the party; one pulled out by a death or a forced trip
   is brought back in automatically, one who chose to leave returns with *Party > Rejoin*.
   Gathering inside a dungeon moves a member in place beside the host.
10. **NPCs.** Guests can talk to NPCs in the host's world (Gehrman too), and guests' blows hurt the
    NPCs the host could hurt. NPC hunters fight the whole party. The first quest choice anyone
    makes counts for everyone (the covenant quests stay each player's own).
11. **Appearance.** The workshop mirror in the Hunter's Dream opens the appearance editor, in
    single player and in a party.

## Deaths, wipes and limbo

| Who dies | What happens |
|---|---|
| A guest | Gets up again at its own lamp, with no loading screen when that lamp is in the loaded area (otherwise a short respawn loading screen); its Blood Echoes wait in a bloodstain where it fell; the others keep playing. After a **fall** off a ledge or out of the world it gets up at its lamp or the host's, never in the Hunter's Dream, and its echoes wait on the last solid ground before the fall |
| The host | Gets up at its lamp with no loading screen while the guests play on (a lamp in another area: the host loads it alone); its echoes stay in a bloodstain. A death that takes the host somewhere by the story (a Snatcher's kidnap) takes the whole party along |
| Anyone, during a boss fight ("limbo") | Watches a living party member fight (the right stick turns the camera; press Cross for the next one) until the boss dies, everyone dies, or someone travels. When the boss dies, everyone who died in the fight gets up right there in the arena, with their echoes still in their bloodstain (host rule *Up after boss kill*) |
| Everyone (a wipe) | Everyone respawns together at the host's lamp with one loading screen; the session stays |
| In a Chalice Dungeon | A guest's Blood Echoes stay in a bloodstain that belongs to the dungeon and can be picked up whenever the party is in that dungeon again. After a wipe in the host's dungeon, the dead guests get up with the host |

A death brings no enemies back; a rest at a lamp (anyone's) or a party wipe resets them. An enemy
killed by anyone in the party stays dead for everyone.

**Optional rot.** With "rot" on (host rule, off by default), each death stacks a penalty, up to 5,
cleared by a lamp, levelling up, a boss kill or recovering your echoes. **Friendly fire / PvP** is
also a host rule, off by default.

## What is shared, and what is not

**Shared between all players (into each player's own save):**

- **Progress**: bosses killed, lamps lit, shortcuts, doors, elevators and ladders opened, key
  story events, in both directions: a door or lever anyone opens is open for everyone, and a
  player who is behind catches up. Shop unlocks (Gascoigne's, Henryk's, Chalice boss attire sets
  and the like) come with it.
- **Key items and unique items** the host or a guest receives (boss drops, keys, tools, quest
  items found lying around, chalices, umbilical cords), once per player; weapons, armour and
  rings found by one player are shared too. Consumables and stacks (Pebbles, Blood Vials from the
  world, bullets, blood gems and so on) stay with whoever found them. A key item you already hold
  or used up is never handed out twice. Gestures learned by anyone are learned by everyone.
- **Rewards**: full Blood Echoes for every kill, whoever made it; each player rolls their own
  copy of each enemy's drop; the same Insight for bosses as the host.
- **Chalice progress comes home with the guest** (0.7.6): the dungeon a guest played in the host's
  world is added to a free dungeon slot of the guest's own altar, with the lamps and shortcuts the
  party reached, and nothing is ever taken away. Chalice boss kills and shop unlocks are shared
  progress.
- **Cutscenes, time of day and endings**: each cutscene the host triggers is also shown to every
  guest, live in the host's world or later in the guest's own world. The time of day (evening,
  night, Blood Moon) follows the host.
- **NPC quests**: shared (My party: *Also NPC quests*), except the covenant quests of Annalise,
  Alfred and Valtr, which stay each player's own.
- **Lamps**: a lamp anyone lights in the host's world is lit for everyone. A guest's lamps from its
  own world stay there unless the host turns on *Guests bring their lamps*. The host can turn
  sharing off (*Shared lamps*).

**Per player (not shared):**

- Character level, stats, inventory, equipment, Blood Echoes and Insight you already have.
- **World treasure**: every player picks up their own copy into their own save. **Chalice weapon
  pickups** are per player too.
- **Shop stock** is per player: one-of-a-kind items (armour sets) bought by one player stay in
  stock for the others. The Doll, Hunter's Dream upgrades, runes and storage are your own.
- Repeatable NPC gifts go to whoever asked; one-time gifts go to every player.

## After an ending: New Game+

When the host reaches an ending, every guest is asked first: *Yes* plays that ending in your own
save (end credits, then New Game+), *No* takes you home with your save as it was. A guest who
used three Umbilical Cords gets Childhood's Beginning when the host refused Gehrman. One party
ending moves a guest's save forward exactly one New Game+ cycle.

**Mixed cycles.** In the host's world everyone plays at the host's New Game+ cycle; your own save
keeps its own. If your save and the host's world are in different cycles, the game tells you when
you are brought in (*"Your save is in NG+2, the host's world is NG+1: you play the host's world at
its NG+1; your own save keeps NG+2"*). This is normal, not a bug. To play at the same difficulty,
everyone should finish the same endings together. In a new cycle, the host's first Hunter's Dream
visit counts as over once the host is out of the Dream, so the party can summon again right away.

## Settings (Party rules and My party)

The launcher's **Party** tab is for hosting, joining and the connection. The party's settings
are on two more tabs, and on the same pages of the game's *System* menu:

- **Party rules** (*System > Party rules: ...*, launcher *Party Rules* tab): what the **host**
  decides for the whole party. They apply when you host; as a guest you follow the host's and the
  game shows them **read-only**, so the whole party plays one way. A few rules are read only when
  the game starts and must match the host's, otherwise the host refuses the join with *"set ...
  like the host"*.
- **My party** (*System > My party: ...*, launcher *My Party* tab): what **each player** chooses
  for itself.

The defaults are the recommended settings; hover any option in the launcher for details.

**Party rules pages (the host's):**

| Page | What it holds |
|---|---|
| World | Enemies the party killed stay dead, gathering, NPCs as in the host's world, covenants per player, voting on travel, NPC hunters fighting everyone |
| PvP & lamps | Friendly fire, shared lamps, guests bring their lamps |
| Deaths | Guests respawn at the host's lamp, refill vials and bullets, up after a boss kill, anyone can be abducted |
| Travel | Lamps and Hunter's Mark for guests, open world (no co-op walls), stay in the host's Chalice Dungeon, cutscenes live with the host |
| Rewards | Full echoes and drops for guests, boss Insight (same as the host / full bonus / game's own) |
| Phantoms | Level sync, phantom health cut, skip the prologue |
| Rot | Death penalties and what clears them (off by default) |
| Enemies | How much bosses and enemies scale with the party |

**My party pages (yours):**

| Page | What it holds |
|---|---|
| Your save | What the party brings into your own character: story progress, NPC quests, cutscenes and endings |
| Your items | Item rewards from the host, the bells, your own key items as a guest, key items at once |
| Display | Party HUD, name tags, pings, show the party through walls, bloodstain marker |
| Look & notices | Companion glow, join / leave notices, ping in the party list, NPC dialogue range, party discovery |

Also on your side: **Party starts** (*After the prologue*, or *Right away*, experimental; keep it
the same for everyone), **Back up every (min)** (10) and **Backups to keep** (10), **Restart the
game after a crash** (on), **Connection warnings** (*System > Party members*, on). Party tab
options: **Automatic summoning**, **Seamless party rules** (off: the normal game's co-op rules; the
host's setting counts), **Separate party saves** (always on), **Max players**, and the connection
settings. The environment variables behind each option are listed in
[party/DEBUG_SWITCHES.md](party/DEBUG_SWITCHES.md).

## Saves and backups

- **Party saves, always**: every party session (launcher or in-game) plays from
  `user\savedata_party`; your single-player save in `user\savedata` is never written by it, so the
  party's progress and item sync can never reach a single-player character. The launcher offers
  to copy your single-player save the first time, and the **Copy single-player save to party
  save** button does it any time (an existing party save is backed up first).
- Progress the party shares is written into **each player's own save**, so after a session your
  character's world has the bosses, lamps and shortcuts of the party's progress.
- **Backups**: during every party session the game copies the save folder to
  `user\save_backups\savedata_party\` at the start, after a crash restart and
  every 10 minutes. The newest 10 are kept; a copy identical to the last one is skipped. The game
  checks the save at every start (*"Saves: check OK"* in the log); a backup that fails the check
  is named `...-suspect` and never pushes good backups out.
- **Undo last party session** (launcher *Party* tab): right before every party session
  (host or guest; not a crash restart, which continues the same session) the game copies
  `user\savedata` and `user\savedata_party` to
  `user\save_backups\party_sessions\<date>-<time>-host` (or `-guest`); the newest 5 are kept.
  The button shows what it will replace (per folder, per character: name, level, NG cycle, play
  time now and then), copies your current saves to a `...-before-undo` snapshot (pressing the
  button again brings them back) and restores the snapshot byte for byte. After a party session
  (when the game closes, or at the next launcher start) the launcher checks the party save: every
  character loads, its NG cycle only changes when an ending was played, no boss kill or lit lamp
  from before the session is gone, level and play time did not go down, no character vanished. On
  a problem it says what and offers the undo. Details: [party/save_safety.md](party/save_safety.md).
- **Save editors**: a save the game made while you were a guest in someone else's world cannot be
  read by save editors. The launcher's *Party* tab has **Export for save editor** and **Import
  from save editor** for that (your save is backed up first).
- **Restoring a backup**:
  1. Close the game.
  2. Rename the save folder, e.g. `user\savedata_party` to `user\savedata_party.broken` (keep it
     until the restored save works).
  3. Create an empty `user\savedata_party` and copy everything from the backup folder into it
     **except** `BACKUP_INFO.txt` (that is the numbered folder, e.g. `1\CUSA03173\...`).
  4. Start the game; the log line *"Saves: check OK"* confirms the restored files.

  `user\save_backups\README.txt` has the same steps.

## Crashes and disconnects

- **Your game crashes**: it restarts by itself after 3 seconds, presses *Continue* on the title
  screen, loads your save and rejoins the party next to the host. The host keeps your place for 60
  seconds. At most 5 restarts in 10 minutes (My party: *Restart the game after a crash*).
- **The host's game crashes**: guests go to their own world and wait ("Waiting for the host to
  come back"). The host's game restarts with the **same party code** and the same member list, and
  the guests are brought back. After 3 minutes a guest plays on alone.
- **Network drops**: an outage of up to about 2 minutes keeps the session; everyone carries on
  where they stood once the network is back. After a longer one a guest goes home and is
  brought back by itself (it reconnects after 1, 2, 4, 8 seconds, then every 8 seconds).
- Progress made while someone was away is caught up when they are back.
- *System > Party* (or the overlay's Party tab) has **Rejoin** (try again now) and **Leave** (stop;
  the game no longer reconnects). A kicked player cannot rejoin that session.

## Four players

**Max players 4** (host + 3 guests) is experimental: the game was made for at most 3 players.
The party patches the game to allow a third helper and to scale boss HP for it. **Every player
must set Max players to 4**, the host and everyone joining; a different value is refused with
*"party rules differ"*. With 4 players there is no free slot for an invader. Players have run 3 and
4 player parties and sent reports, which were fixed; expect more rough edges than with 2 or 3.

## Troubleshooting

**First, run [`net_doctor.bat`](#connection-doctor-net_doctorbat)** on the logs of the host and the
guests: it names the cause and the fix for most connection problems. The game also shows a
**connection warning** (in the middle of the screen, with the shortest fix) when the firewall,
the router or a wrong address keeps the party from working. The launcher log (and `launcher.log`)
shows the game's `Party:` lines; *System > Party* shows the state and the last error.

When the host refuses a guest (wrong or missing password, old code, full party, different
version), the guest gets one message box saying why and what to do, and its game stays online
alone in its own world. Fix the cause, then *System > Party > Rejoin the host*: it sends the
password currently set in *System > Party > Password*. A changed party code or address still
needs *Join party* (restart).

| Message / symptom | Cause | Fix |
|---|---|---|
| *version mismatch with the host: eboot.bin differs* | Another game version or dump | Everyone needs the 1.09 `eboot.bin`. |
| *... gameplay patches differ (host only: X; yours only: Y)* | Different XML patches that change gameplay | Enable the same patches (Game tab, *All XML Patches...*). Cosmetic patches do not count. |
| *... gameplay mods differ* | Different mods in `mods\` | Use the same gameplay mods, or turn mods off on all PCs. |
| *... party rules differ (host max 4 players ...; yours max 3 ...)* | Different *Max players* | Set the same Max players everywhere. |
| *set ... like the host* | A start-time rule differs from the host's | Open the named rule in Party rules / launcher and match the host. |
| *different party protocol version ...: update the port* | Different builds of the port | Everyone updates to the same build. |
| *this host has no password: join with its party code* | You entered a plain `address:port` and the host has no password | Use the host's party code (or LAN code on a VPN), or ask the host to set a password. |
| *This party needs a password* | The host set a password; yours is empty | Enter the host's password (launcher *Password*, or *Party > Password* on the title screen). |
| *Wrong party password (or an old party code ...)* / *Wrong or missing party password* | Password differs, or the code is from an earlier hosting session | Same password on all sides; ask for the current code. After 5 wrong tries in a minute the host ignores you for a while. |
| *Old party code: ask the host for the current one* | The host made a new code, or set / removed a password | Ask the host for the current code. |
| *the name X is already in the party* / *invalid name* | Duplicate or invalid name | Pick another name (1-16 of A-Z a-z 0-9 _ -). |
| *the party is full* | Max players reached | Raise Max players (on everyone's PC) or wait. |
| *kicked by the host* | The host kicked you | Ask the host. |
| Guest stays at *Connecting* | The host's port is not reachable | Host: check UPnP in the log, forward UDP+TCP 9307, allow the firewall prompt on both PCs; strict router / provider NAT: use a VPN or let another player host. |
| Connection warning: Windows Firewall has no rule / blocks the game | The firewall stops the party port | Allow `bbport.exe` for UDP and TCP on the network type you use (*Windows Security > Firewall > Allow an app through firewall*), on host and guests. |
| Connection warning: nobody reached your party after 3 minutes | Host: no open port and no VPN in use | Forward UDP+TCP 9307 on the router, or host over a VPN. |
| Connection warning: a guest is in the party but its game is not heard | Firewall on one side, or the VPN address is not the one used | Allow the game in the firewall on both PCs; on a VPN both set `BB_PARTY_LOCAL_IP` to their own VPN address ([steps](#hosting-over-a-vpn-radmin-vpn-hamachi-zerotier-tailscale)). |
| Connection warning: you cannot reach the host | Wrong address, host not hosting, different VPN network, or the host's firewall | Check the host is running, the same VPN network, the address or code, and the host's firewall. |
| *strict NAT (a strict router or your ISP's carrier NAT)* | Hole punching cannot reach you | Turn on UPnP or forward UDP+TCP 9307 on the router; if the router's WAN address differs from your public address, it is the provider: use a VPN or let another player host. |
| *UDP to the guest is blocked* / *UDP to the host is blocked* | The guest's game has no public UDP address yet | Nothing at first: it retries every 40 s and the join then starts by itself. If it stays blocked, restart the guest's game, or use a VPN. |
| *Failed to acquire Bloodborne server information* when joining a host on the same network with the Internet code | The router cannot loop back to its own public address | Use the host's **LAN code**, or update (the guest then finds the host on the LAN by itself). |
| Friends on a VPN cannot join, or see each other frozen | The game uses the home address instead of the VPN one | Update to 0.7.6, or set `BB_PARTY_LOCAL_IP` ([steps](#hosting-over-a-vpn-radmin-vpn-hamachi-zerotier-tailscale)). |
| Connected, but never brought in | The guest is still in the prologue, in a menu, or the host is in a loading screen / boss fight / cutscene | Finish the prologue (or host turns on *Skip the prologue*); wait; use *Rejoin* (*System > Party*). |
| Brought in, but the worlds look different ("Your save is in NG+..." notice) | Players are in different New Game+ cycles | Normal: you play the host's world at its cycle; see [New Game+](#after-an-ending-new-game). |
| No code under *Internet code* / *LAN code* | The game has not started hosting yet | Wait until the game is at the title screen; check the log for `Party:` errors. |
| Save looks wrong after a session | - | Use *Undo last party session*, or restore a backup ([above](#saves-and-backups)), and report it with the logs. |

When reporting a problem, include the net_doctor reply (with `--redactnames --redactips` if you
post publicly), and the logs: launcher *Advanced / Debug* > **Save support bundle** on every
player's PC (passwords, codes and addresses are removed), and say who was the host and which
step each player reached. Before playing, *Party logging: Verbose* helps; press **F9** at the
moment it happens to mark the logs.

## Known limitations

- **Four players is experimental.** The game was made for three; there is no slot for invaders,
  and it has had less testing than 2 and 3 players.
- **Chalice Dungeons**: the fixed Pthumeru dungeons have been played together; **Root (randomly
  generated) and glyph dungeons use the host's dungeon the same way but have not been checked in
  a real session yet.** Bringing a dungeon home to a guest's altar is proven for the first layers; deeper
  layers and a guest whose altar is full (six dungeons) are not.
- **Mixed New Game+ cycles**: in the host's world everyone plays at the host's cycle, while each
  save keeps its own. The game tells you; it is not something to fix.
- **Strict NAT / carrier NAT**: two players behind strict or provider NATs, with no IPv6 and no
  reachable member, cannot connect directly. Use a VPN (Radmin VPN, Hamachi, ZeroTier,
  Tailscale) or let a player with an open port host. The Windows Firewall must allow the game on
  every PC; the game warns but never changes your firewall.
- **Real-network testing is limited.** Two long Internet sessions and a growing number of player
  reports; the fixes since the last long session were tested on one PC and on LANs. Please send
  support bundles when something goes wrong.
- A first boss fight with Ebrietas, Lady Maria or Laurence starts only with what the host does in
  their room. Brador and the Fishing Hamlet's hunters are not tried in a real session.
- Travel is "follow and rejoin"-style: a trip takes a loading screen, and guests that load
  separately arrive a moment later. A shared seamless loading screen is not planned.
- The Bath Messengers can pop in and out for a player watching while another browses their shop.
- The online play of the original game (PSN messages, bloodstains of strangers, invasions) is not
  available; the party only connects the players who have the code.

For developers: the design and status are in [PARTY_COOP_PLAN.md](PARTY_COOP_PLAN.md) and
[party/](party); test and debug switches in [party/DEBUG_SWITCHES.md](party/DEBUG_SWITCHES.md).
