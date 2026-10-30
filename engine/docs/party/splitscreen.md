# Splitscreen mode (0.7.7, CL-2eec68)

For several game windows on one screen (the Nucleus Co-op splitscreen mod): each window is small, so the party HUD
and the party messages are cut down. Every setting is the player's own; nothing is sent to the party.

| Setting | Where | Default |
|---|---|---|
| `party_splitscreen` (env `BB_PARTY_SPLITSCREEN`) | System > My party: Splitscreen > Splitscreen mode; launcher My Party > Splitscreen; bbport.ini | off |
| `party_messages` = `all` / `errors` / `off` (env `BB_PARTY_MESSAGES`) | System > My party: Splitscreen > Party messages; launcher My Party > Party messages; bbport.ini | all |

Both are live (no restart). The pure rules are in `gpu/shim/party/party_splitscreen_rules.h`
(unit test `party-splitscreen-test`, `tests/test_party_splitscreen.cpp`); the drawing is in `bbport_overlay.cpp`
(`PartyHud`, `PartyToast`, the alert box).

## What the switch does

- **Compass-only HUD.** The party panel (bottom left; still subject to `party_hud`) shows one row per companion that
  is on this map in this world: a direction arrow in the member's colour and the distance. No names, health and
  stamina bars, ping, state text or rot line. Companions elsewhere get no row; with none on the map, no panel.
- **No name tags** above the companions (`party_name_tags` and `party_overhead` keep the values the player set).
  Pings and the through-walls markers stay their own settings.
- **Messages:** the effective mode is `Effective(splitscreen, party_messages)`: splitscreen turns All into Errors
  only; Errors only and Off stay as picked. The mode applies without the switch too.

## Message mapping

Party messages come in two kinds, filtered where they are drawn (the Party page's "Last event" line and the logs keep
everything):

- **Alert** (a message box in the middle of the screen: host refused us, connection warnings, host did not come back,
  Chalice Dungeon cannot be entered): always an error. Shown by All and Errors only; hidden by Off.
- **Notice** (a toast in a corner, `SetLastEvent`): an error when its text contains one of: lost, failed, unstable,
  blocked, can't / cannot reach, could not, did not come back, waiting for the host, error, stuck, refused,
  kicked by, not allowed, desync, mismatch, timed out, party is full, wrong password, wrong or missing
  (`IsErrorNotice`). Everything else (joined, left, travelling, gathering, votes, shared item notices, boss and bell
  notices) is information and shows only with All.

A new notice that reports a failure must contain one of those words, or the list in the rules header grows (the unit
test lists the current examples).

## Tests

- `party-splitscreen-test` (unit: the messages parse, the effective mode, the error mapping, the HUD and tag rules),
  `game-menu-pages-test` (the My party: Splitscreen page: two rows, the list last), `tests/test_launcher_party_tabs.py`.
- Smoke group `splitscreen` (`python tools/mp/smoke_party.py --only splitscreen`): two games, the guest with
  `BB_PARTY_SPLITSCREEN=1`; the guest draws the compass only, no name tags and hides "Joined the party", the host
  keeps its panel and tag; screenshots of both are in the run's `shots/`. Group `rules` walks the new page.
