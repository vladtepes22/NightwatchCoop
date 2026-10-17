# Friendly fire / PvP (I1)

ERSC parity row I1 (ERSC: the Judicator's Rulebook). The host switches friendly fire on and off;
party members can then hurt each other, both ways, with the game's own hit reactions. Off by
default. Code: `gpu/shim/party/party_pvp.cpp` (game side), `party_pvp_rules.h` (pure, unit test
`party-pvp-test`), the rule in `coop::balance::Rules::friendly_fire` (EVENT `balance`).

## How the game keeps phantoms friendly (1.09, our offsets)

- Every `ChrIns` has a team type at **+0x88** (chr type at +0x78). Measured live in a party:
  the host's body **19**, a cooperator (white phantom) **20**, on both games (each game sees the
  same teams); map enemies 23. `0x18C2910` resolves the effective team (vtable `0x5318790`, team
  at +8): SpEffect stateInfo 132 turns it into 29 (indiscriminate), an `0x4028` override can too.
- "Is team b an enemy of team a" is a byte in 32 x 32 tables, index `a * 32 + b`:
  - the **relation table**, reached through the pointer `0x53E4560` -> `0x5127B90` (data):
    read by lock-on / nearest-enemy search (`0x18B90F0`), the damage number display
    (`0x1A2EA90`), `0x1575BF0` and about 15 other functions;
  - the **SpEffect target table** `0x4731560` (rodata; "friend" twin `0x4731960`): an attack's,
    bullet's or area's effects on "enemies" (`0x1A29C40`, `0x1575D30`, `0x17E28D0`).
  - In 1.09 rows 19 and 20 have 0 at columns 19 and 20 in both tables (allies), 1 at column 23.
- bbhost (droogie, read-only reference) names `0x18C2910` "resolve effective team" and the
  `0x4731560` table "opposition matrix"; nothing there turns friendly fire on.

## What friendly fire does

On every party member's game (the attacker's game detects the hit, the victim's applies it), in a
party session (a host with cooperators, or a guest in the host's world) with the rule on, the cells
between the teams of the party's player chrs are set to 1 in both tables (`TeamPairs`: every
ordered pair of two different player chrs; two guests share team 20, so 20 -> 20 too). The
original bytes are kept and put back when the rule goes off, the session ends or a load starts.
Enemy, NPC and boss rows are never touched; the log names any non-player chr sharing a party team
(`0 other chr(s) on a party team` in the live run). Which of the two tables gates the hit itself
was not isolated; both are opened, so attacks and their effects (bleed, poison, bullets) behave
as against an invader.

- **The rule**: the host's setting (`party_friendly_fire`, System > Friendly fire, the overlay's
  Party tab, the launcher Party tab's host-only checkbox, env `BB_PARTY_FRIENDLY_FIRE=0/1`). It
  travels in the host's `balance` rules (on change at once, every 15 s for late joiners); a guest
  uses only the host's value (its own setting never counts; before the host's rules arrive: off).
  Guests see the host's value read-only (System > Friendly fire shows "On"/"Off" with "the host
  decides"; the overlay shows a note instead of the switch). Hosts without the field (older
  builds) mean off.
- **Toast** on every member when it changes: "Friendly fire ON: party members can hurt each
  other" (guests: "... (host's rule)"), "Friendly fire OFF"; a guest joining a party with it on
  gets the ON toast once.
- **Lock-on** follows the relation table, so with friendly fire on the lock-on can pick a party
  member, as it picks an invader in vanilla.

## Decisions

- **A kill is a normal party death.** The victim's game sees an ordinary death (`HP -> 0`): a
  guest goes through B2 (member_down, revived at its lamp with no load, or the party's rules for a
  wipe / boss fight), the host through its normal death plan (one party respawn load). Nothing in
  the death / wipe / respawn logic looks at the killer, so there is no special case to confuse it.
  The vanilla phantom-death banner is hidden as for any guest death (`banner 103100 hidden`).
- **Rot applies as for any death** (C4): the death debuff does not know the killer. A host who
  wants duels without it turns *Rot on death* off (same page group, also a party rule).
- **No Echoes / Insight from party kills.** The session stays a co-op session (no invasion
  result), and the party's kill-echo payer only pays for map enemies (team 23). Checked live: no
  Echoes or Insight change on the killer after the kill (the pvp module logs every change while
  active); if a later build ever saw one, it would show as `own Echoes a -> b` right after a kill.
  The victim drops its echoes in a bloodstain as for any death.
- No summon-sign, invasion messages or covenant results are involved: the session stays a co-op
  session; only the two team tables change.

## Live verification (2026-10-14, one 2-instance session, Hunter's Dream)

`instances.py run --count 2 --party --fresh-saves` with `BB_PARTY_PVP_TEST` (host: `log@15,
face@5,hit=3@1,on@4,log@3,face@2,hit=4@1,log@12,face@8,hit=3@1,log@20,off@5,log@5,face@3,hit=2@1,
log@15`; guest: `log@5,face@3,hit=3@1,log@30,face@2,hit=4@1,hp=30@6,log@30`). `face` warps the
local chr 1.1 m in front of the other player (physics positions), `hit=N` taps R1 N times.

| Phase | Result (logs `Party balance: hit:` / `Party: pvp:`) |
|---|---|
| Off (default), guest 3 x R1 on the host, host 3 x R1 on the guest | no HP change on either game |
| Host switches on | `opened relation[19][20] effect[19][20] relation[20][19] effect[20][19]` on both games 0.02 s apart; toast on both (screenshots) |
| On, host 4 x R1 | guest HP 719 -> 658 -> 594 -> 527 -> 464 (61-67 per hit), hit reactions and blood on screen |
| On, guest R1 on the host | host HP 719 -> 658, 659 -> 534 |
| On, guest at 30 HP (test aid), host R1 | guest `HP 30 -> 0 (dead)`, "YOU DIED", member_down, rot stack, "no bloodstain (echoes 0)", revived at its lamp with no load 20 s later; host: no Echoes / Insight change |
| Host switches off | `closed 4 table cell(s)` on both, toast; host 2 x R1 on the guest at 0.2-0.4 m: no HP change |

Unit tests: `party-pvp-test` (session gate, guest rule, team pairs, test steps, the rule's JSON),
`party-balance-test`, `game-menu-pages-test` (the new page), `tests/test_launcher_party.py`.

## Live verification, part 2 (2026-10-16): 3 players, a host killed by a guest, bullets, poison

Test steps added for this (`party_pvp_rules.h`): `face=T` (the first other party player on team T:
20 is a guest, so a guest can face the other guest), `shoot=N` (N taps of L2, the firearm),
`item=GOODS` (test aid: that goods topped up to 10 and put in quick slot 0, selected; the game's
own quick-item record, `SetQuickItem` in game_state.h) and `use=N` (N taps of Square).

**pvp4**: `instances.py run --count 3 --party --fresh-saves --headless 1 --timestamps` (host and
guest 1 rendered, guest 2 headless), Hunter's Dream. Host: `log@10,on@10,log@40,hp=30@40,log@45,
face@20,shoot=3@1,log@15,off@5,log@5`; guest 2: `log@30,face=20@5,hit=3@1,face=20@2,shoot=2@1,
item=1210@3,face=20@1,use=5@1,log@15,face=19@40,hit=3@1,log@40`; guest 1: `log@60,face=20@50,hit=2@1,log@30`.

| What | Result (logs `Party: pvp:`, run clock) |
|---|---|
| Host switches on | all three games `opened relation[19][20] effect[19][20] relation[20][19] effect[20][19] relation[20][20] effect[20][20]` at +47.0 s (6 cells: guest-guest 20 -> 20 too), toast on all three |
| Guest 2 3 x R1 on guest 1 (guest-guest) | guest 1 HP 719 -> 658 -> 594 -> 527 (+71.2..73.8 s) |
| Guest 2 2 x L2 on guest 1 (bullets, guest-guest) | guest 1 HP 528 -> 500 -> 498, 498 -> 468 (+76.6..78.0 s) |
| Guest 1 2 x R1 back on guest 2 | guest 2 HP 719 -> 645 (+147.4 s) |
| Host at 30 HP (test aid), guest 2 3 x R1 on the host | host `HP 35 -> 0 (dead)` (+144.6 s), then the host's normal death plan: `host death: the respawn load for the whole party at lamp 2102961 (vanilla loading screen)`; the cells closed for the load (`closed 6 table cell(s) (loading)`) and opened again after it on all three (+173 s); no Echoes / Insight change on the killer |
| Host 3 x L2 on guest 1 (bullets, host -> guest) | guest 1 HP 719 -> 711 -> 705 -> 702 -> 700 (long range) |
| Host switches off | `closed 6 table cell(s) (the rule is off)` on all three, toast |

**pvp5** (status effect, 2 players, host throws Poison Knives at the guest): host `log@10,on@10,
face@15,item=1210@2,face@1,use=8@1,log@40,off@5,log@5`. Guest HP: the knife hits 719 -> 688 ->
646, 634 -> 603, 561, 518 (30-43 a knife, +64.2..69.5 s), then **poison**: 12 HP every second
from +66.5 s to +105.5 s (39 ticks, 591 -> 86), as an invader's poison would; host `closed 4 table
cell(s)` at off.

Not covered live: 4 players (one more team-20 chr: the same table cells), the poison icon on screen
(the run's screenshots did not start in time; the HP log shows the 1 s ticks).
