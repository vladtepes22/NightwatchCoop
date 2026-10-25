# Party settings in the launcher (My party and Party rules)

The launcher's Co-op page has two settings lists. **Party rules** (shown to a host) holds what the
host decides for the whole party. **My party** holds what only changes your own game: your screen,
what you get as a guest, and story sync. Each row below is the launcher label, the `bbport.ini` key
it writes, the default, and what it does; the game's own menu (System > Party rules / My party)
shows the same values, because the launcher and the game read the same `bbport.ini`.

Defaults were checked on 25 Oct against the `Values` initializers in
`engine/gpu/shim/bbport_settings.h` and visy 0.7.5's key table (`work/handoff-0.2.4/report-W1.md`).
When a key is missing the game keeps that initializer, so the launcher default and the game default
are the same for every row.

Notes that apply to several rows:
- "Next start" means the game reads the value once, when it starts.
- "While you are a guest" means the row only changes things when you join someone else's world.
- A few rows depend on another row, noted in their description.
- Since 0.7.5 the host's rules reach a guest on connect (and again on every change and every 15 s),
  so a guest follows the host's value for a live rule. The Join tab hides Party rules for that reason.

## Party rules (the host decides)

| Launcher label | `bbport.ini` key | Default | Read | What it does |
|---|---|---|---|---|
| Remove multiplayer barriers | `party_open_world` | on | live | No co-op walls on area borders: the party walks anywhere. |
| Cutscenes for every player | `party_story_mirror` | on | live | A guest in the host's world watches a cutscene with the host. Off: the guest watches it later, in its own world. |
| Guest respawn | `party_guest_respawn` | on | live | A guest who dies comes back at the host's last lamp and is summoned again, not sent home. |
| Get up without a loading screen | `party_guest_revive_inplace` | on | live | A guest who dies gets up at the host's lamp with no loading screen (after a fall: the respawn load). Off: every death takes the respawn load. |
| Refill guest supplies | `party_guest_refill` | on | live | Guests' vials and bullets refill when the host rests, dies or travels. |
| Stay in Chalice Dungeons | `party_chalice_rejoin` | on | live | A guest taken out of the host's dungeon by a death or forced trip is summoned back. Leaving by choice: use Rejoin. |
| Level sync | `party_level_sync` | on | live | A hunter above the host's level range is scaled down in the host's world (less health, stamina, defence, resistance) and gets a blue icon by its name. Off: full stats. |
| Phantom health cut | `party_phantom_hp` | on | live | Hunters who join have 30 % less maximum health. Off: full maximum health. |
| Skip the prologue (new characters) | `party_skip_prologue` | off | new character | A character that has not finished the prologue skips it: story flags set, starting weapons given, appears at the first lamp in the city. Characters past the prologue are never touched. |
| Guests use lamps | `party_guest_lamps` | on | next start, join check | Guests may use lamps and headstones; the whole party travels with them. |
| Guests can use Hunter's Mark | `party_guest_mark` | on | next start, join check | A guest's Hunter's Mark asks: travel alone or with the party. Off: as in the game. |
| Full Blood Echo rewards | `party_full_rewards` | on | next start, join check | Guests get full Blood Echoes and enemy drops like the host. Off: the game's co-op amounts. |
| Insight for bosses | `party_guest_insight` | Same as host (`parity`) | next start, join check | The Insight a guest gets when the host kills a boss: `parity`, `full` or `0` (the game's own). |
| Party starts | launcher variable `BB_PARTY_START` (`party_start` in settings.json) | After the prologue | next start, join check | Whether each new character completes its own prologue first (`prologue_solo`) or the party starts right away (`immediate`). |

**Join check.** The five rows marked "join check" are read once at start and the host refuses a
guest whose value differs from its own ("set X like the host"). The launcher never changes them on
its own. On the Join tab they appear under **Start-up rules** with a note, and the launcher names
any that are not the default, because a host that kept its defaults will refuse the guest.

Other Party rules rows (older, unchanged): Enemies stay dead, Host can gather the party, NPCs as in
the host's world, Covenant outcomes per player, Party votes on travel, NPC hunters fight the whole
party, Anyone can be abducted, Friendly fire / PvP, Shared lamps, Guests bring their lamps, Rot on
death and its three cleanse rows, and the six boss / enemy scaling numbers. They are described by
their tooltips in the launcher and by the game's System > Party rules pages.

## My party: screen

| Launcher label | `bbport.ini` key | Default | What it does |
|---|---|---|---|
| Party HUD | `party_hud` | on | Shows companions' names, health and where they are, in the bottom left corner. Party mode only. |
| Ping in the party list | `party_hud_ping` | on | Each companion's ping, coloured by quality, in the party list. Live, local. |
| Party name tags | `party_name_tags` | on | On: our name plates above companions, never hidden by walls. Off: the game's own plate. |
| Pings | `party_pings` | on | Touchpad + R3, or the Ping key (default B, under Controls), marks where you look. Everyone sees the mark for 10 s. |
| Show party through walls | `party_through_walls` | off | A small marker with the distance on each companion on your map, also behind walls. |
| Companions without glow | `party_glow_off` | on | On: other hunters look like normal hunters on your screen. Off: the game's blue phantom glow. |
| Join, leave and travel notices | `party_toasts` | on | A short notice in a top corner when a hunter joins, leaves or travels. |
| Name tag detail (0-4) | `party_overhead` | 3 | What shows above companions in view, only for you: nothing, name, name and level, name and health, name and deaths, or name and ping. |

## My party: dialogue, keys and items

| Launcher label | `bbport.ini` key | Default | What it does |
|---|---|---|---|
| Share NPC dialogue | `party_npc_talk` | on | See and hear the NPC conversations of companions near you (subtitles, choices, gifts). |
| NPC dialogue range (m) | `party_npc_talk_range` | 25 | How close a companion has to be for you to see their NPC conversation. |
| Own key items as a guest | `party_key_view` | on | While you are a guest, your own keys and badges show in Key Items. Off: the game's empty list. Next start. |
| Key items at once | `party_key_items_summoned` | on | While you are a guest, a key the host's world gives you is yours at once. Needs **Own key items as a guest**. Off: the key waits until you are back in your own world. |
| Grant co-op bells | `party_grant_bells` | on | Gives the Beckoning Bell and Small Resonant Bell after the prologue, for ringing by hand. Next start. |
| Key items for every player | `party_items` | on | Since 0.7.5 only key items and unique items (a key, a badge, a gift that exists only once) the host picks up or is given are given to each guest too, once. Ordinary pickups (vials, bullets, materials) are not copied. Next start. |

## My party: your save

| Launcher label | `bbport.ini` key | Default | What it does |
|---|---|---|---|
| Shared progression | `party_progress` | on | Bosses, lamps, shortcuts and key events are copied into every guest's save. Host and guests. Next start. |
| Shared NPC quest choices | `party_progress_npc` | on | The first NPC quest choice anyone makes applies to every save. Needs Shared progression. Next start. |
| Shared story and endings | `party_story` | on | Guests see the host's cutscenes, time of day and ending. Host and guests. Next start. |

## Connection rows (Co-op page)

| Launcher label | Key | Default | What it does |
|---|---|---|---|
| Host over a VPN | `party_vpn` | Auto | `auto` takes a Radmin VPN, Hamachi, Tailscale or ZeroTier adapter (in that order) when there is one, `off` never, or an adapter name. A commercial VPN is only used when named. Read at start. |
| Show my party to friends | `party_discovery` | on | While hosting, the game announces name, level, players, area and build (never the password or an address) by UDP broadcast on port 9308 on private LAN, Radmin, Hamachi and ZeroTier adapters. Live. |
| Online memory watch (crash) | `BB_HEAP_WATCH_LOG` (launcher: More > Engine > Diagnostics) | off | Logs every 20 s which parts of the game hold the memory the online code uses, for "Transaction failed" crash reports. |

The **Your connection** card shows the route the party uses (the VPN adapter, the local network or
the Internet), its state and `ip:port` with a Copy button. It reads the adapters in the launcher,
off the UI thread, and the address is only shown on the user's own screen. The **Parties on your
network** list (Join tab) listens on UDP 9308 only while the Join tab is open, lists one entry per
game, and lists a full party or another version as unavailable.

## Notes for testers

- The launcher's **Key items at once** row is the one that shows the log line
  "not given while summoned (Key items at once is off)" in `session.log`.
- Defaults are the values the launcher's `SeamlessPage.cs` passes to `IniToggles` and `IniNumbers`.
- The game's in-game text for `party_items` ("Host's item rewards") may still describe the
  pre-0.7.5 meaning.

## Where this is defined

- Launcher rows: `launcher/windows/BloodborneLauncher/Views/SeamlessPage.cs`,
  `Core/SeamlessSettings.cs` (`GameSwitches`, `OwnSwitches`, `StartupRules`), `Core/NetworkRoute.cs`
  and `Core/PartyDiscovery.cs` (the discovery parser, written to the spec in
  `engine/gpu/shim/net/party_discovery.h`).
- Game menu text: `engine/gpu/shim/bbport_game_menu_pages.cpp`.
- Game defaults: `engine/gpu/shim/bbport_settings.h` (`Values` initializers).
- Read and written by: `engine/gpu/shim/bbport_settings.cpp`.
