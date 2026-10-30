# Guests can gather the party (0.7.7, CL-a3a9e6)

The host's *Gather the party* (docs/party/ERSC_PARITY.md I5, `party_gather.cpp`) brings the guests to the host. This adds the
other direction: a guest pulls the host and the other guests to itself. Pure rules: `gpu/shim/party/party_gather_rules.h`
(`RequestRefusal`, `RequestHostRefusal`, `MemberAreaRefusal`, the `req` / `reqreply` wire; unit test `party-gather-test`).

| Setting | Where | Default |
|---|---|---|
| `party_guest_gather` (env `BB_PARTY_GUEST_GATHER=0/1` wins at start) | System > Party rules: Travel > Guests can gather (host); launcher Party Rules tab; bbport.ini | off |

The host's rule goes to the guests with the other host rules (`party_balance` Rules `guest_gather`, `HostRule::GuestGather`; a
host from before it sends none: off). A guest's *Gather the party* row (System > Party) is shown only while it is on; the
Travel page shows the guest a read-only line with the host's value.

## Flow

1. The guest presses the row (`party_status::RequestGather`, queued only while the rule is on). `RequestRefusal` checks it:
   rule off, *Stay in my world* on, not in the host's world, loading, dead / spectating, cutscene, boss fight, travelling,
   on a ladder or in the air, and the 30 s cooldown. The refusal is a corner notice ("Gather the party: ...").
2. EVENT `gather {"op":"req"}` (map, spot, yaw, ground flag) goes to the host. The host (`RequestHostRefusal`) checks its
   rule and the cooldown it shares with its own gather (a round still open counts as cooldown), answers `reqreply`
   (ok + number asked, or the reason) and asks every other connected guest and itself with `{"op":"ask","by":<slot>}`.
3. Each member is asked as for the host's gather (same refusals: menu, dialog, boss fight, cutscene, loading, dead,
   travelling, staying in its own world; the host is refused for its own boss fight too) with the game's YES / NO dialog,
   text "NAME gathers the party". A member in another map block than the guest answers *in another area* (the move is an
   in-world warp only: no map load, no travel to a lamp, no computed point). YES: `PutBeside` with the guest's spot
   (the ground the guest checked), the B2 spacing by slot (the host takes spot 3) and the same landing watch and side
   tries; a fall puts the member back where it stood and stops (never a load).
4. The host sums the round up in its notice ("Gather: Ann coming; Bob said no; ...").

## Not done / limits

- Members in another map are not brought (they would need a load; correct over seamless). The guest is told the number
  asked, not the individual answers (the host sees the summary).
- No hotkey for guests, only the Party row.

## Tests

`party-gather-test` (rules, wire), `game-menu-pages-test` / `game-menu-rows-test` (the Travel row, the guest's Gather row
visible only with the rule), `party-balance-test`, `settings-test`, `tests/test_launcher_party.py`; smoke group
`guest_gather` (`python tools/mp/smoke_party.py --only guest_gather`, phase `gg`) and `--only rules`.
