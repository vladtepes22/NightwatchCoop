# Who runs an enemy's AI (ERSC parity C2, split travel; 0.7.6)

Two player reports of the week of 2026-10-19/25 looked like one cause: an enemy stood still next to
a guest because the game that owns its AI did not drive it.

1. A boss idle next to a guest while the host lay dead in a boss fight (screenshot, no log).
2. A guest who travelled alone (split travel, "Travel with the party?" answered NO) into a map the
   host had not loaded: the enemies loaded but stood still and ignored the guest until the host
   loaded that map (REPORT of the Nightwatch dev, 0.2.4-beta.1 = our 0.7.5).

## The engine, as measured

`NpcIns` (every enemy, boss and NPC chr of the map) holds two manipulators: `+0x3B8` the
ComManipulator (this game runs the chr's AI) and `+0x3C0` the NetAIManipulator (the owner's game
runs it, this game replays its frames); `+0x60` is the active one. The chooser is
`NpcIns::SetManipulator` 0x18E6540 `(chr, force_local)`. With `force_local == 0` it picks Com when the
chr's handle is in the WorldChrMan `+0xA40` owner table, or when the chr's vfunc `+0x4C8` says so, and
the NetAI otherwise. A client's game asks for it **every frame for every enemy** (live: 8640 calls per
second for 240 enemies at 36 fps).

Live probe (`BB_PARTY_AI_LOG=1`, smoke phase `la`, a lone guest in the Cathedral Ward, the host in the Dream):
the guest's enemies are built on the Com manipulator and the first chooser call puts them on NetAI
two seconds after the arrival (`owner table 0, vfunc +0x4C8 0`); nothing ever sends them a frame, so
they stand still. The host's game did not have the map, so there is no AI owner at all. The same
probe shows enemies of a map the host shares with the guest move onto Com only when the owner table
holds them (a handle the host's data placed there).

## Decision (2 = lone guest): option (a), the guest's game runs the AI

`party_enemyai.cpp`: while the guest is a client (role 6) and the host is in another **area**
(`b2::MemberApart(0)`, the vitals are fresh) the replaced chooser prologue turns every
`force_local == 0` call into `force_local = 1` (the chr keeps the Com manipulator the game built it
with) and a one-second tick switches the ones already on NetAI. Rules and the take-over / hand-back
steps are the pure `party_enemyai_rules.h` (unit test `party-enemyai-rules-test`). `BB_PARTY_LONE_AI=0`
turns it off (A/B).

Why (a) and not "block lone travel into a map the host has not loaded": the split-travel decision
(users, binding) is that every player may travel alone; the enemies of a map only that player has are
fresh and simulated by that player's game anyway (split_travel.md section 2), only their AI was not
running. No new rule, no toggle: it is a fix of a broken part of an existing behaviour.

Hand back: when the host is in the guest's area again the game does the B2 re-join (the older world
loads once, the chrs are built again and the game chooses for itself); if the area is the same
without a load the tick calls the chooser with `force_local = 0` again for the chrs it switched.

Not covered (unchanged): a lone guest cannot start a **boss** fight (the fight events are host-only
copies; the fog is kept shut, split_travel.md section 1); two guests in the same map without the
host each simulate their own copy of the enemies (the same "ordinary enemies per player's own game"
rule); enemy HP / kills of a lone guest's block are the guest's own until the meet (the census
compares only maps both games hold: `the host is in m21, this game in m24: not compared`).

## Decision (1 = host dead in a boss fight)

The static finding of 0.7.4 stands: a chr leaves the AI's target bank only at its teardown, and the
target search has no HP check, so the boss may keep the dead host as its target. 0.7.4 added
`LimboBossTargetTick` (the living member nearest to the boss is forced as its target through the
event ChangeTarget path 0x1299CD0, once a second, `BB_PARTY_LIMBO_RETARGET=0` off).

Reproduced here (smoke phase `ba`, 2 players, Cleric Beast, the host frail so that the boss itself
kills it, the guest invincible beside the boss, `BB_PARTY_TARGET_ANIM=1`): **with and without** the
retarget the boss kept acting after the host's death in limbo (30-60 animation changes in 100+ s,
the attack combos 3000-3027); in one extra run (retarget off) the guest moved 22 m away after the
host's death and the boss walked to it (closest 1.6 m inside 45 s). So the idle boss of the player's screenshot is **not reproduced** on one PC and
the retarget is not shown to be needed or to hurt; it stays as it is. What differs in the real
session is unknown (no log came with the report): the build, the boss and whether the limbo path was
taken at all (a host death with no boss bar up is the whole party's respawn load, the boss idles
for the ~10 s of the death sequence by the game's own rule). The AI log switch above is for the next
report: ask for `BB_PARTY_AI_LOG=1` and `BB_PARTY_TARGET_ANIM=1` logs of both games.

## Evidence (one PC, 2 instances, loopback; agents/enemyai/smoke)

* `la9` (fix on): the Ward enemy 2400421 (182 HP, awake, on a ledge 22 m from the lamp) beside the lone
  guest: animations 0, 3011, 2603 (hit reaction), 3011, 3008, 0; the guest (frail test aid) went 340 HP -> 1
  from its hit; the guest's R1 took the enemy from 182 to 84 HP.
* `la10` (`BB_PARTY_LONE_AI=0`): the same enemy: animations 0, 3011 and then nothing, the guest never hurt.
* Not every Ward enemy is awake: 2400350-352 and 2400220 sleep (chr entry +0x10 is 0, no animation at all,
  also on a guest in a shared world with the host: `lb1`), 2400362 is a statue. The probe lists only the
  active ones (+0x10 == 4); a lone guest's active enemies animate on its own game (`la8`: the anim ring
  index of 2400421, 2400899, 2400422, 2400203 keeps advancing, manipulator Com).
* Hand back is not exercised live: when the host reaches the guest's area the B2 re-join loads the older
  world (fresh chrs); the unit test covers the take / keep / hand-back steps.
