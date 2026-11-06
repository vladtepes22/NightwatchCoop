# NPC hunter fights with the whole party (ERSC parity D11)

0.7.3. User decision 2026-10-22: **the whole party fights.** A scripted NPC hunter appears in sync
for everyone, anyone can hurt it, and its death and the quest step count for all, like a boss.
Covenant outcomes stay **per player** (user decision; the covenants work owns those flags).

Code: `gpu/shim/party/party_npcfight_rules.h` (the decisions, unit test `party-npcfight-test`),
`party_npcfight.{h,cpp}` (the game side), the rule `party_npc_fights`. Smoke: phase `nf`, group
`npc_fights` (`tools/mp/smoke_party.py --only npc_fights`).

## 1. What the game data says (EMEVD scan of every map, 1.09)

Instructions searched in every `.emevd` (`flag_tool.py`'s parser, the events expanded per
InitializeEvent call): the AI-player summon / invasion instructions `2003[25]` (AI-player SOS),
`2003[51]` (NPC summon, type 0 cooperator / 1 invader), `2003[54]` (send an NPC home), plus every
team switch `2004[2]` / `2004[44]` and the multiplayer gates (`3[6]`, `1003[5/6/105]`, `3[29]`,
`1003[9/10/14-16/109]`, `3[22]`, `4[3]` on the local player).

| Instruction | Where | What it is |
|---|---|---|
| `2003[25]` | nowhere | unused |
| `2003[51]` type 0 | the Beckoning-Bell templates `xx04410` of m23, m24_00/01/02, m27, m28, m32, m34, m35, the Chalice `12906966` | NPC hunter **signs** (Old Hunter Bell): the only story use of the summon slots |
| `2003[51]` type 1 | m29 `12904634` | the Chalice Dungeons' NPC invader; it ends on a client (`1003[6]`) |
| `2003[54]` | m24_01 `12414480` | sends a summoned NPC home |

**The story's NPC hunter fights are not AI-player summons.** They are c0000 map chrs that the
map's events enable, warp, re-team and dress:

(The per-fight table of map entities and event ids is not kept here: it is read from the player's EMEVD and MSB files.)

* **Eileen with Henryk, Tomb of Oedon** (m24_01): the init event, Henryk's appearance, Eileen joining as the ally when
  Henryk's AI fights, the kill (solo / together variants), Eileen turning hostile and Henryk's death. Henryk is team 23
  (enemy, MSB); Eileen is 28 (ally) with a phantom-look SpEffect.
* **Eileen at the Grand Cathedral and the blood-drunk Eileen** (m24_00): init, the falling to the blood (**host only:
  `4[3]`**), then hostile; team 27.
* **Alfred (Cathedral Ward)** (m24_00): init, then **hostile (host only: `4[3]`)**; team 26 / 27.
* **Alfred executing Annalise, Alfred in Cainhurst** (m25): init (team 27 after the `1003[6]` end-if-client), the hostile
  event (`1003[6]`), his death; the queen's own event sets her state; team 27.
* **Valtr** (m27): init, **hostile (host only: `4[3]`)**; team 8 / 27.
* **Djura** (m23): the events 12300700-12300705; team 26 / 23; its gates accept chr type 0 **or 1**: it already runs on a guest.
* **Hunter's Nightmare hunters and the Research Hall hunter** (m34, m35): init, **hostile (host only)**; team 27.
* **Brador (Research Hall) and the Fishing Hamlet assassins** (m35, m36): omen, appear, forced return, repelled; hidden:
  an invisibility SpEffect, invincible, no hit; fighting: another SpEffect.
* **Yahar'gul's hunters, Patches** (m28, m32): ordinary enemies of team 23.

Covenant-defining quest states (per player, never written from here): Annalise 1040-1059 (1042 the
flesh: Alfred's execution, set by 12500010 when Alfred's 1348 is on), Alfred 1340-1359, Valtr
1790-1809 (`CovenantOutcomeFlag`; the covenants work makes them `covenant_outcome` in
the derived flag table).

## 2. What a party saw before (live, nf runs 2-3, 2026-10-22)

Host and guest at the Tomb of Oedon with Eileen's state 1363 (default test save, Gascoigne dead):

- **Spawn:** Henryk and Eileen are there on both games (the same chr handles, 10014006 / 10014004).
  No slot is involved, so the party's size does not matter.
- **Sync:** the host's game owns them. Henryk's HP and position follow the host on the guest's game;
  a HP 0 written on the guest's game alone was back to the host's 1606 within a second.
- **A guest's hits:** R1 from the guest (locked on, team 23 = an enemy for a phantom) took Henryk
  1716 -> 1690 -> 1664 -> 1636 -> 1606 **on both games**.
- **Death and quest:** after the guest's killing blow the host's game killed him (12410810 = 1,
  12410804: 1367, the drop roll and the Echoes on both); the host's flags went to the guest's own
  pool (`guest applied 4 flags in load mode 1`, save requested).
- So the Tomb fight already worked. What a guest **misses** are the transitions that only the
  host's copy of an event runs (the bold events above): Alfred turning hostile (in Cainhurst even
  at every load), the blood-drunk Eileen, Valtr, the DLC hunters: the guest keeps a friendly or
  unloaded chr it cannot lock on to or hurt. Brador's appearance waits for the host's own position
  and goods, so a guest's copy never shows him (he stays hidden and invincible there).

## 3. The rule and what it does

`party_npc_fights` (the host's; System > Party rules: World *NPC hunters fight all*, the launcher's Party
Rules tab *NPC hunters fight the whole party*, bbport.ini `party_npc_fights`,
`BB_PARTY_NPC_FIGHTS` at start; default on; guests follow the host's, sent with the `balance` rules).

- **Guest copies of the host-only transitions** (`kGuestRuns`, the EMEVD dispatch filter's second
  skip predicate): on a guest in the host's world, each listed event's gate is passed (`4[3]`: the
  chr type it waits for becomes the guest's own; `1003[6]`: the client state becomes one the game
  never reports), so the guest's copy re-teams / shows the chr at the same moment as the host's
  (the HP condition reads the host's HP, which the game syncs). Its flag writes and save requests
  (`2003[2/9/17/22/31/32/41]`, `2000[5]`) are skipped: the host's copy writes them and C2 brings
  them into the guest's save, so no progress is written twice and no covenant outcome is written
  from here. On the host, solo or with the rule off, the gate's argument is the data's own again.
- **A foe a phantom can hit** (`kGuestFoeTeam`, `TeamFor`): the hostile team 27 is no enemy for a
  cooperator. In the relation table (`0x5127B90`, lock-on) and the SpEffect target table
  (`0x4731560`, an attack's effects) row 19 (the host's body) has 1 at column 27, row 20 (a white
  phantom) has 0 in both (read from the 1.09 ELF); team 23 (the map enemies, Henryk) has 1 in both
  rows. Live nfa run 2: with 27 on its game the guest pressed R3 three times at 0.9 m (no lock-on)
  and its eight R1 left Alfred at 3031. So every `2004[2]` team 27 of a listed event and of the inits
  that run on every game (`kTeamEvents`: 12400650, 12700901, 13400950) becomes 23 in a guest's copy
  (its game only: the host's game keeps 27, its AI and events; another team in the data, e.g.
  Valtr's 8, is untouched); every other dispatch writes the data's 27 back.
- **Assassins** (`kAssassins`): a guest shows Brador / the Hamlet assassin (drops SpEffect 5560,
  adds 8060, clears its invincibility) while the host-world flag its appear event sets is on, and
  hides it again when the host's forced return clears it. **Live (0.7.9 `nfb` / `nfh`, section 4)**: both come out on team 23 on both games and
  their flags reach the guest, so this tick found nothing to do in the runs; the "no hit" state (`2004[39]`) has no known field yet.
- **Slots** (`AiSummonFits`): only the Bell signs and the Chalice NPC invader use the 5-entry slot
  table, under the game's four-player limit. The party's default of 3 players leaves one; a 4-player
  party has none (as in vanilla with three cooperators). Nothing is patched for it.

## 4. Live evidence (smoke phase `nf`, agents/npcfights/smoke/run2-run10, 2026-10-22)

| Case | Before (vanilla + party) | After (rule on) |
|---|---|---|
| Henryk + Eileen, Tomb of Oedon | Spawn on both games (same handles), Eileen joins as the ally (1365), the guest's R1s take Henryk 1716 -> 1488 on both games, the guest's killing blow kills him on the host ~5 s later, 12410810 / 1367 on the host and `1367=1/1` in the guest's own save at the next load (runs 3-7) | unchanged (no gate involved; runs 4-7 pass the same rows) |
| Alfred hostile, Cainhurst (state 1349; run 10 from the first lamp 2502950, read from the chr dumps at ~290 m) | host team 27, **guest team 26** (friendly: no lock-on, no hits; runs 5-10) | after a load the guest's copy of 12500050 passes its `1003[6]`: team 27 on the guest too (`guest copy of event 12500050 runs as the host's`, runs 4-10; run 10 17/17, the smoke as committed) |
| Blood-drunk Eileen, Grand Cathedral (1374 + 9802) | the guest that arrives after the host already gets her through the init event (12400650 label 1, no gate): team 27 on both (run 8) | same; the gate matters for a transition while the guest stands in the map. Her death from the guest's blow reached the host (1368, 1705) and the guest's own save (`1368=1/1`, run 8) |

### Alfred in the Queen's Chamber (smoke phase `nfa`, agents/nfproof/smoke/run1-run3, 2026-10-22)

The void at the Queen's Chamber lamp (2502951) was the fixture: event 12500810 (the Crown of
Illusions reveal) disables the chamber's map parts 2506000-2506017 / 2506070-2506073, objects and hit
parts 2506037 / 2506038 until its own flag is on. `nfa` sets 12500810 (and 12500000, the carriage
arrival scene; Logarius dead 12501800/02/03; Alfred's talk flags 72500350-72500359 cleared, else
12500051 moves him to 1351): the chamber draws on both games, Alfred stands at y 130.

Run 3 (22/22, the smoke as committed), Alfred friendly (1348) in both saves:

| Step | Host | Guest |
|---|---|---|
| Before | team 26 | team 26; R3 at 2 m: no lock-on |
| The host's 3 R1 (the guest 2 m from him) | 3100 -> 3031, 12500053: 1349, team 27 at +56.5 s | its copy of 12500053 (gate passed) fires at +56.5 s with the host's flag: `team 27 -> 23` (a foe), team 23 at +57.4 s |
| The guest locks on, 8 R1 | HP 3031 -> 3020 -> 3009 -> ... -> 2941 (11 per hit) | the same HP; lock-on 1; damage number 11 on screen |
| The guest's HP 0 (`guest_kill :hp`) at +82.4 s | death 5 s later: 1350, 12500054, drop roll, Echoes 0 -> 5250 at +87.5 s | Echoes 0 -> 5250, the same drop roll seed |
| The guest's next load | | `1348=1/0 1349=0/0 1350=0/1`: Alfred's states are covenant outcomes, its own save keeps 1348 (per player) |

Runs 1-2 (before the team fix): the in-map switch worked the same (team 27 on the guest 0.6-0.9 s
after the host's), but the guest never locked on and its R1 did nothing (run 2: HP 3031 unchanged).
A HP written by the test aids (`:hp85`, `guest_kill :hp` on an unhittable chr) drifted 30 per second
on both games afterwards; with real hits (run 3) there was no drift.

Seen in run 3 (not D11): on the host's screen Alfred stands ~5 s after the guest's killing blow
(the guest's game plays his death at once; the Tomb fight shows the same ~5 s), and his death line
shows as "[Hunter1 talking to Alfred]" on the host (the shared NPC talk takes the death line for a
conversation).

### The blood-drunk Eileen and Alfred in the Cathedral Ward (smoke phases `nfe`, `nfw`, 0.7.7 npchunt077)

Both loopback, 2 players, groups `npc_eileen` / `npc_alfred_ward` (`smoke_party.py --only npc_eileen`).

- **Event 12400656 never runs.** The 1.09 map script has no InitializeEvent for it (EMEVD trace, both games: no
  dispatch); the night's common event 9723 sets 1372 itself. The `kGuestRuns` row stays harmless. The live path of
  the blood-drunk Eileen is the init 12400650 at a load (team 27, 1374 + night 9802).
- `nfe` (Eileen 2400902, fixture 1374 + 9802): both games have her (team 27 host, team 23 guest through
  `kTeamEvents`), the guest locks on and hits her (HP falls identically on both games, 62 -> 36 -> 10 -> 0 after the
  host set her to 2 %, because her AI dodges), **the guest's real killing blow** (no HP aid): 1368 on the host, her
  HP 0 on the guest's game, the shared drop roll, Echoes on the host. A guest-side HP 0 aid does NOT kill her (the
  host's HP overwrites it within 2 s), so the phase uses real hits. Quirk: "[Hunter talking to Eileen]" shows during
  the fight (her shared talk lines).
- `nfw` (Alfred 2400763, Cathedral Ward, state 1344 friendly): the host's 3 hits trip 12400592 -> 12400593 slot 1
  (the `4[3]` gate in `kGuestRuns`) with the guest standing beside him: host team 26 -> 27 and 1345, guest team 26 ->
  23 in the same second, lock-on, the guest's R1s take his HP down on both games (about 36 each), his HP 0 and drop roll on the host
  and HP 0 on the guest. Alfred heals from low HP (1 % came back to full within 3 s), so the end is the host's
  HP 0 (the game's own death), not a guest blow.

### An NPC hunter that arrives as an invader (smoke phase `npi`, 0.7.7 npcinv077)

Group `npc_invader` (`smoke_party.py --only npc_invader`), loopback, 2 players, ~2.5 min.

- **No retail trigger exists.** The only `2003[51]` type-1 (invader) event, 12904634, is called with
  its NPC entities (2900160-2900163) only by the Chalice prototype layout m29_05_00_05; the retail dungeons
  (m29_00 and the generated layouts) never call it (a scan of every EMEVD's InitializeEvent calls; docs/party/chalice.md calls m29_05 a prototype layout). The Bell
  events are type 0 / 5 / 6 / 7 (cooperator session types). So the case is built with the Beckoning Bell
  fixture (`BB_PARTY_TEST_NPC=2410740:28`: the Tomb of Oedon sign NPC, session type 28 = the EMEVD type 1
  mapping, `desc[28]`: capability 0x100, team 2, net chr type 8 = an invader, "NOT counted as cooperator"),
  summoned by the host 12 s after it stands in Central Yharnam (lamp 2412951).
- **Arrival.** The request passes the SOS filter; the NPC walks in from its map spot (team 20, chr type 5,
  disabled until the game's summon presentation, ~15 s), then it is **team 21, chr type 2** on both games.
  The guest has it too (the map chr both games hold; its team, type and HP follow the host's), at the same place: chr dump
  positions of the two games 0.0 m apart (run 1).
- **Fight.** The guest locks on to it (team 21 is a foe for a phantom, nothing to convert: no `kTeamEvents`
  entry is needed) and its real R1s take its HP down identically on both games (1342 -> 1298 -> 1254 -> ...
  -> 930, the same series; damage numbers and its HP bar on the guest's screen). Like Alfred it heals from
  a low HP (host HP 26 came back to 1099 in 11 s), so the end is the host's HP 0 (`host_kill :hp`, the game's
  own death): the host rolls the drop (`enemy drop roll on this game ... handle 0x10014001`), the guest's chr
  dump shows HP 0 at the same second. The NPC then returns home (disabled, HP restored: the task's Return step).
- No party-side bug was found: the case works with the code as it is. The fixture is the only way to trigger it.

### Every story hunter, live (five of six reliable; smoke phases `nfn`, `nfr`, `nfv`, `nfd`, `nfb`, `nfh`, 0.7.9 hunters079)

Loopback, 2 players, one phase per hunter, each ~5-8 min (`smoke_party.py --only <group>`). The three provoked
hunters share one shape (`nfh`-style fixture of `phase_nfh`, like `nfw`): the NPC stands friendly in its quest
state (set by the fixture, the rest of the state range cleared), the host stands in front of it and hits it, the
guest stands beside it; the host's "turns hostile" event fires (HP at 90 % or the provoke flag, host-only gate), the
guest's copy re-teams it 8 -> 23 on its game within the second, the guest locks on, its real R1s take the HP down
identically on both games (the same series), the host's HP 0 (the game's own death) reaches the guest's chr dump
and the quest state moves to its death state on the host AND in the guest's world view.

| Phase | Hunter | Result |
|---|---|---|
| `nfn` | Hunter's Nightmare hunter (`kGuestRuns` 13400920, state 1720 -> hostile 1725 -> dead 1710) | 16/16: host team 27, guest team 23 after the host's blows, lock-on, 15 R1 on both games, death + state 1710 on both |
| `nfr` | Research Hall hunter (13500920, 1722 -> 1727 -> 1712) | 16/16, same rows |
| `nfv` | Valtr (12700905, 1800 -> 1805 -> 1790) | 16/16; his states are covenant outcomes (per player by decision), so only the host's states are required to change; the guest's chr dies with HP 0 |
| `nfb` | Brador, the Research Hall's church assassin (3500936) | the host walks to the omen spot with the goods his event checks (one of the host's own, given by the fixture), he appears on both games (team 23 on both: no conversion needed), the guest locks on and hurts him on both games, his HP 0 reaches the guest, the done flag 13505912 on both |
| `nfh` | A Fishing Hamlet assassin (3600900) | same rows; the omen fires when the host stands beside the waiting chr; done flag 13605970 on both |
| `nfd` | Djura (Old Yharnam, 2300710) | Green (3 first-try greens in a row with the fixed phase, plus one run green after a check fix and retries). The guest's copy of his events already runs (gates accept chr type 0 or 1). Cause of the old flakiness was the phase, not the game: Djura stands beside a ledge (an 18 m drop) and the phase put the host and the guest 1.5 / 1.2 m east of him; his blows or the player's own R1 lunge pushed them off, a fall death sent the guest to the lamp 149 m away (the old "Djura 149 m away", no lock-on, 0 R1 on him) and the host's falls ended the fight early. Now both stand on his south side (90 deg, the safe floor). His hostile state is 1322 or 1323 run to run (both accepted). Two shared-machine runs were cut by an outside kill after 90 s; not a game fault |

What these showed: the three provoked hunters needed no code change (the existing `kGuestRuns` / `TeamFor`
rewrite does what it says for every listed event), and neither did the assassins: Brador and the Hamlet assassin
come out on team 23 (a foe for a phantom) and the flags their events set reach the guest's world, so the guest
fights them without the guest-side show (`kAssassins`' tick found nothing to do: its log line is absent). Not
proven: a guest-only trigger (the assassin events need the host's chr type and goods, so a guest alone near the
omen spot gets no assassin), the other two Hamlet assassins and the Hamlet assassin's real kill by the guest (all
phases end with the host's HP 0 after the guest's blows), more than one guest, simulated-internet runs.

Not verified live (after 0.7.9):
- a guest's real killing blow on any of them (every phase ends with the host's HP 0 after the guest's blows; `nfe`
  alone used a guest's last real hit);
- a guest-only trigger of Brador / the Hamlet assassins, the other two Hamlet assassins, the second Hunter's
  Nightmare hunter (3400903);
- more than one guest, simulated-internet runs (BB_NET_SIM) of the hunter phases;
- the Chalice NPC invader event 12904634 itself (prototype layout only: not reachable in the retail game; `npi` runs the same session-type-28 summon through the Bell fixture);
- AI-player summons (Bell signs) with a full 4-player party.
