# Chalice layer travel (0.7.7, CL-48ee07)

In vanilla a Chalice Dungeon lamp (talk script t290110 + n, lamp 2902950 + n, object 2901950 + n) offers one thing:
"Return to the Hunter's Dream?" (the same generic dialog `10010713` as the world lamps). Travel between the dungeon's
lamps is the ritual altar's "Select location to awaken in" in the Dream. This adds the world lamps' own travel list
(docs/party/travel.md, `lamp_travel.cpp`) to the dungeon lamps: the Hunter's Dream and every other lamp of the dungeon
that is unlocked (the layers), and the choice takes the party along.

| Setting | Where | Default |
|---|---|---|
| `party_layer_travel` (env `BB_PARTY_LAYER_TRAVEL=0/1` wins at start) | System > Party rules: PvP & lamps > Chalice layer travel (host); launcher Party Rules tab; bbport.ini | off |

The host's rule goes to the guests with the other host rules (`party_balance` Rules `layer_travel`, `HostRule::LayerTravel`;
a host from before it sends none: off); a guest sees a read-only line. Pure rules: `lamp_travel_rules.h`
(`BuildChaliceList`, `ChaliceLampUnlocked`, `NearestChaliceLamp`, `ChaliceListOffered`; unit test `lamp-travel-test`).

## How it works

1. The dungeon lamp's talk runs the Dream confirmation command; lamp travel's talk-command replacement (the same one the
   world lamps use, `BB_LAMP_TRAVEL` on) sees it in area 29. With the rule off, or for a guest (a guest's own dungeon
   lamp stays the vanilla question and the party's lamp request), it does nothing: vanilla.
2. "Here" is the nearest lamp object 2901950 + n within 8 m (a generated dungeon has no table spots). The list is the
   Dream first, then lamp n = 0 (return point 2902950, "Chamber of the Seal": the start and the altar's first awaken point,
   always) and every lamp whose lit flag 12907810 + 20 n or registration flag 12907800 + 20 n is set, ascending, never the
   one the player stands at, never one whose "cannot travel" flag 70002900 + n is set. Only the Dream (or nothing else): the
   vanilla question stays.
3. The entries go through the game's own talk commands (ClearWarpList, AddWarpEntry, OpenWarpMenu), so the menu is the
   game's. Live finding: AddWarpEntry takes an entry only if its third argument (the lit flag) is set, so an unlocked lamp
   without its own lit flag (lamp 0) is added with the standing lamp's flag.
4. A chosen lamp runs the game's lamp warp `0x13CDF30(2902950 + n)` as the player (never a computed point), recorded as
   this player's own lamp use (`split::NoteLocalUse`). The Dream runs the lamp's own flag 72900100 + n (the vanilla
   path: event 12907020 warps to the Dream's altar spot).
5. In a party the lamp warp is the host's own lamp use: `split` classifies a warp to a dungeon lamp as an awaken point
   (`lamp N (host): a Chalice Dungeon awaken point: the host enters alone, the party is summoned there`), so there is no
   "Travel with the party?" question and no vote (as for the altar's awaken list, chalice.md 8). The host loads; what a
   guest does is the next section.

## Guests and the gray loop (what was proven, what is not)

A guest never replays the warp into a dungeon of its own: `GuestSkipsChaliceTravel` (chalice.md 4) only lets a guest replay
a travel into a Chalice Dungeon when its current dungeon is the host's, and a warp to a dungeon lamp is not a replayed
travel at all (above). The 0.7.5 gray loop was a guest in m29 without its dungeon loaded while the fall recovery warped
it to a dungeon lamp; layer travel never warps a guest to a lamp. Two cases, both run live (2 games, about 20 s of
loading each, nothing stuck, no gray loop, screenshots of both looked at):

- **`cy`, guest with its own other dungeon in slot 1** (`chalice_hintertomb_ritual`): its current dungeon is not the
  host's, so `HostInOtherChalice` lets the send-home through, the guest loads home and the summon quick-joins it into the
  HOST's dungeon beside the host. Layer 1 start and back to the boss room: both ends 0.8 to 0.9 m apart each way, about
  20 s after the host's own load.
- **`cyn`, guest with no dungeon at all** (the default save): the quick join wrote the host's dungeon into its slot 0.
  What its game reads as its current dungeon varies: 0 (`HostInOtherChalice`: the send-home goes through, home + summon
  as in `cy`, leg 1 live) or the host's id (the send-home is dropped as a split travel and it would stay behind). For the
  second case `LayerFollowRefusal` (lamp_travel_rules.h, unit tested) lets the guest run the same lamp warp itself
  (`party_b2 LayerFollowTick`: `RunLampWarpPartyNotRest(2902950 + n)`, log `guest: layer travel: lamp warp N`) when:
  the host's rule is on, the lamp is 2902950..58, its own current dungeon id equals the host's (both logged), it is in
  the dungeon map, not staying home, not loading / dead / in a boss fight or cutscene. Anything else keeps the notice
  "Hunter0 went on to another area". Live (cyn, leg 2): the guest's own warp, one load, then the session re-joined it
  (a second load): host and guest 0.9 m apart in the boss room, about 18 s after the host's load, no gray loop.
  Not proven: the guard's refusals live (unit tests only).

A guest that is apart (at home, *Stay in my world*, no bell) stays where it is in both cases.

## Not done / limits

- Only the host (or a solo game) gets the list; a guest's lamp in a dungeon stays the vanilla Dream question.
- The list shows the dungeon's lamps, not named "layers": the entry names are the game's EventText of the name flag
  70002900 + n.
- No screen text of its own; no change to the altar's awaken list.

## Tests

`lamp-travel-test` (list rules), `game-menu-pages-test` / `game-menu-rows-test` (the row on PvP & lamps, Russian names
within the limit), `party-balance-test`, `settings-test`, `tests/test_launcher_party.py`; smoke group `chalice_layers`
(`smoke_party.py --phase cy` / `cyn`; fixtures `chalice_pthumeru_inside` for the host, `chalice_hintertomb_ritual` or the
default save for the guest; test aid `BB_LAMP_TRAVEL_TEST=members,lamp=2902952,to=2902950,back`).
