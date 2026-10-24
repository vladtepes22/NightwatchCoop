# My party settings (launcher)

The launcher's Seamless page has a **My party** list. These switches only change your own game: your
screen, what you get as a guest, and story sync. The host's rules are in the **Host** list
(see `seamless_rules.md`). Each row below is the launcher label, the `bbport.ini` key it writes,
the default, and what the game says in its own menu (System > My party). The launcher and the game
read the same `bbport.ini` values, so a change in either place applies to both.

Notes that apply to several rows:
- "Next start" in the game's text means the change takes effect the next time the game starts.
- "While you are a guest" means the row only changes things when you join someone else's world.
- A few rows depend on another row, noted in their description.

## Screen

| Launcher label | `bbport.ini` key | Launcher default | What it does |
|---|---|---|---|
| Party HUD | `party_hud` | on | Shows companions' names, health and where they are, in the bottom left corner. Party mode only. |
| Party name tags | `party_name_tags` | on | On: our name plates above companions, never hidden by walls. Off: the game's own plate. |
| Pings | `party_pings` | on | Touchpad + R3, or the Ping key (default B, under Controls), marks where you look. Everyone sees the mark for 10 s. |
| Show party through walls | `party_through_walls` | off | A small marker with the distance on each companion on your map, also behind walls. |
| Companions without glow | `party_glow_off` | on | On: other hunters look like normal hunters on your screen. Off: the game's blue phantom glow. |
| Join, leave and travel notices | `party_toasts` | on | A short notice in a top corner when a hunter joins, leaves or travels. |
| Name tag detail (0-4) | `party_overhead` | 3 | What shows above companions in view, only for you: nothing, name, name and level, name and health, name and deaths, or name and ping. |

## Dialogue and lamps

| Launcher label | `bbport.ini` key | Launcher default | What it does |
|---|---|---|---|
| Share NPC dialogue | `party_npc_talk` | on | See and hear the NPC conversations of companions near you (subtitles, choices, gifts). |
| NPC dialogue range (m) | `party_npc_talk_range` | 25 | How close a companion has to be for you to see their NPC conversation. |
| Guests use lamps | `party_guest_lamps` | on | Guests may use lamps and headstones; the whole party travels with them. Needs a restart. |

## Guest rules (what you get when you join someone)

These are the game's co-op switches. They live in `bbport.ini`, so the game's own menu and the
launcher both change them. The launcher's labels are shown first; the game's wording follows.

| Launcher label | `bbport.ini` key | Launcher default | What it does |
|---|---|---|---|
| Grant co-op bells | `party_grant_bells` | on | Gives the Beckoning Bell and Small Resonant Bell after the prologue, for ringing by hand. Next start. |
| Remove multiplayer barriers | `party_open_world` | on | No co-op walls on area borders: the party walks anywhere. Keep it the same for everyone. Next start. |
| Guest respawn | `party_guest_respawn` | on | While you are a guest, a death brings you back at the host's lamp, not at home. Next start. |
| Refill guest supplies | `party_guest_refill` | on | While you are a guest, vials and bullets refill when the host rests, dies or travels. Next start. |
| Stay in Chalice Dungeons | `party_chalice_rejoin` | on | As a guest, if a death or forced trip takes you out of the host's dungeon, you are summoned back. Leaving by choice: use Rejoin. |
| Guests can use Hunter's Mark | `party_guest_mark` | on | A guest's Hunter's Mark asks: travel alone or with the party. Off: as in the game. Next start. |
| Full Blood Echo rewards | `party_full_rewards` | on | While you are a guest, full Blood Echoes and enemy drops, like the host. Off: the game's co-op amounts. Next start. |
| Items for every player | `party_items` | on | Boss drops, key items and gifts the host gets are given to each guest too, once. Next start. |
| Own key items as a guest | `party_key_view` | on | While you are a guest, your own keys and badges show in Key Items. Off: the game's empty list. Next start. |
| Key items at once | `party_key_items_summoned` | on | While you are a guest, a key the host's world gives you is yours at once. Needs **Own key items as a guest**. Off: the key waits until you are back in your own world. |
| Shared progression | `party_progress` | on | Bosses, lamps, shortcuts and key events are copied into every guest's save. Host and guests. Next start. |
| Shared NPC quest choices | `party_progress_npc` | on | The first NPC quest choice anyone makes applies to every save. Needs Shared progression. Next start. |
| Shared story and endings | `party_story` | on | Guests see the host's cutscenes, time of day and ending. Host and guests. Next start. |
| Cutscenes for every player | `party_story_mirror` | on | A guest in the host's world watches a cutscene with the host. Off: the guest watches it later, in its own world. Next start. |

## Notes for testers

- The launcher's **Key items at once** row is the one that shows the log line
  "not given while summoned (Key items at once is off)" in `session.log`.
- The Host list (what the host decides for everyone) is documented in `seamless_rules.md`.
- Defaults are the values the launcher's `SeamlessPage.cs` passes to `IniToggles` and `IniNumbers`,
  including `true` (on) for every `GameSwitches` row.
- When a key is missing, `bbport_settings.cpp` keeps its `Values` initializer from
  `bbport_settings.h`, which matches the launcher default for every row above unless an
  environment override applies.

## Where this is defined

- Launcher rows: `launcher/windows/BloodborneLauncher/Views/SeamlessPage.cs` (My party section),
  `Core/SeamlessSettings.cs` (`GameSwitches`).
- Game menu text: `engine/gpu/shim/bbport_game_menu_pages.cpp` (My party pages).
- Game defaults: `engine/gpu/shim/bbport_settings.h` (`Values` initializers).
- Read and written by: `engine/gpu/shim/bbport_settings.cpp`.
