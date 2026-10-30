# Stay in my world (0.7.7, CL-adeb50)

A guest can pause the automatic joining into the host's world without restarting the game or leaving the party: for a
Chalice ritual of its own, shopping, a break in the Dream. Pure rules: `gpu/shim/party/party_stayhome_rules.h`
(unit test `party-stayhome-test`, `tests/test_party_stayhome.cpp`); game side `party_stayhome.cpp`.

| Setting | Where | Default |
|---|---|---|
| `party_stay` (env `BB_PARTY_STAY=0/1` wins at start) | System > Party > Stay in my world (a guest's row); launcher My Party > Start & saves; bbport.ini | off |

It is the player's own, live, and unrelated to `BB_PARTY_AUTO` (which switches the bells off for the whole process and
is a test aid). Off = joining exactly as before.

## What it does while on (guest)

- The director rings no Small Resonant Bell and runs no gather to the host's lamp; a bell already rung is withdrawn
  (log: `stay: staying in my own world: no bell, no gather`). Turned off, the next tick rings again through the normal
  join path (`stay: joining resumes`); nothing is forced mid-action.
- A title join (the restart / Continue flow) is skipped: plain Continue into its own world.
- The host's "Gather the party" is refused with the reason "staying in own world" (the host is told); the host's travel
  vote is not asked of it (it never opens the question).
- The party link, chat, the panel and the Rejoin button keep working. The guest's own bell is the only thing paused:
  "Rejoin the host" reconnects the link but rings nothing while the switch is on.
- Host travel never pulled a guest that is in its own world (`GuestFollowsHostTravel`: IgnoreOwnWorld); that stays.
- A guest that is already in the host's world when it turns the switch on stays there; it is applied the next time the
  guest is at home (it left, died back home, or used "Travel alone").

## What the host sees and does

The guest sends `EVENT stay_home {"on":0|1}` on every change and every 5 s while on; the host forgets a report older
than 15 s (a lost "off" cannot park a guest) and a guest that left. For a staying guest the host

- does not ring the Beckoning Bell and does not hold the direct summon for it (`SummonableRoster`, like an unreachable
  member);
- does not wait for its answer in a travel vote (`party_vote.cpp`: it is not a voter; log
  `stays in its own world: does not vote`);
- shows "Staying home" instead of "In own world" in the Party members rows (`party::status::StayMask`).

## Tests

- `party-stayhome-test` (wire, reporting rule, the host's expiring book, a vote that skips a staying guest),
  `party-gather-test` (the StayHome refusal), `game-menu-pages-test` (the Party row: guest only, a toggle, Russian
  strings within the limits), `tests/test_launcher_party_tabs.py` (flag and checkbox).
- Smoke group `stay_home` (`python tools/mp/smoke_party.py --only stay_home`, phase `sh`): the guest goes home with the
  switch on (`own_rule=party_stay:1`), the host travels twice, the guest follows nothing and rings nothing, the switch
  goes off and the guest is back in the host's world. The rules smoke resets `party_stay` in its ini.
