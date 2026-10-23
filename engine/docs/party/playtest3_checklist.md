# Playtest 3 checklist (0.7.3, two PCs)

Goal: real two-PC evidence for the ERSC parity rows that failed in an earlier real session and are
capped at 84-90 % because their fixes were only proven on one PC (docs/party/ERSC_PARITY.md,
"Evidence and ERSC parity"). Each passed item lifts its row toward the two-PC cap; each failure is a
bug report with its log.

Setup: both players on 0.7.3, launcher *Advanced / Debug > Diagnostics*: party logging on. After the
session, both send `out\run.log` (or the launcher's support bundle). Host and one guest are enough;
a second guest helps for C2 / H1.

| Row | Try this | Pass when |
|---|---|---|
| A5 | Guest joins from the title and from its own world while the host is at a lit lamp, then while the host is out in an area | The guest arrives beside the host or at a lit lamp, on solid ground, one loading screen |
| B2 | Enter a boss fog together (Gascoigne, then an action-started fight: Ebrietas, Laurence or Lady Maria) | Both are in the fight; the fight starts once; a wipe returns both to the lamp with the boss reset |
| B6 | Use a headstone and the Hunter's Mark; take a story warp (Lecture Building, Nightmare grab) | One "Travel with the party?" question; YES moves both with one loading screen |
| C1 | Guest dies to an enemy, to a fall, and on a ladder | The guest gets up (no load, or at the host's lamp as set), the camera follows, walking works |
| C2 | Host dies alone with the guest alive; then both die (wipe) | One loading screen for the party at the host's lamp; nobody is stuck in "Host dead" |
| C3 | Wipe in a boss fight, walk back through the fog | The boss is reset and starts only at the fog |
| C5 | Guest dies in a boss fight, the host kills the boss | The guest gets up in the arena after the kill, gets the kill and the lamp |
| D2 | Talk to NPCs the host has met (Gascoigne's daughter, Alfred) as the guest | The guest sees the host's NPC state; nothing changes in the guest's own world |
| D3 | Ride lifts together: Cathedral Ward, Research Hall 3F-5F, Cainhurst | Both arrive at the same end; nobody falls through or is carried back |
| D4 | Pick up a key item and items in the host's world; open the Old Hunter Bell / Iosefka's vial | Each player gets its own copy once (items for every player) |
| D5 | Join Valtr's League as the guest; Alfred's Annalise step as the host | The guest gets the Impurity rune; each player keeps its own covenant outcome |
| D10 | Watch a cutscene together (Cathedral Ward door, a boss intro); host skips one | The guest sees it with the host; a host skip ends it for both |
| D11 | Fight a hunter that turns hostile with both present (Alfred in Cainhurst or Cathedral Ward, blood-drunk Eileen) | The guest can lock on and hurt it; its death counts for both |
| E1 | Talk to Gehrman, buy from the Dream messengers as both | Each player's own shop stock; no "already bought" from the other |
| F1 | Play 30+ min with both, including a disconnect and rejoin | No session timeout; the rejoin lands beside the host |
| H1 | Guest dies away from the host in several areas | The no-load revive works each time; no stuck camera or ladder state |
| H3 | Use gestures, the shop and key items as the guest | Gestures and items behave as in the guest's own world; nothing lost |

Also worth a minute each (0.7.3 additions): abduction of the guest by a Snatcher (the party goes to
the Gaol, everyone sees the scene); a Chalice Dungeon lamp used by the guest; *Gather the party* in Old
Yharnam; the travel vote (turn it on in *Party rules*).
