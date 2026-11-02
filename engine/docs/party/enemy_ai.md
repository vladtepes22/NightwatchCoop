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

## Dead players in the target bank (0.7.7, bossai077: "the boss stops when the host dies")

Root cause, read from the player's screenshot (NW-ca9ed04): most likely the **host's** screen, spectating
(empty HP bar, echoes 0; smoke `bh` shows the same picture on the host), the followed guest at **0.2 m** and the Cleric Beast idle beside them. The
spectate (C5, smooth follow since 0.7.0) carries the dead host chr onto the followed member's own spot
so that the camera follows it. The enemies' target search (`SprjTargetBankManager` *0x553B098, a vector
at +0x8 / +0x10 of every chr's target entry; read by 0x12ADC50, which filters on team and entry flags,
**no HP check**) still holds the dead host chr: it is now as near to the boss as the living guest, so
the boss's target search can pick the corpse and idle over it; the once-a-second ChangeTarget only
pulled it back for a moment. Loopback runs looked fine because the boss's actions at the corpse
look the same as actions at the guest standing on it, and the scripted guest never moves away.
The same holds for a dead guest's copy on the host's game (kept at its death spot with HP 0):
the 4-player report "the boss stops attacking the last one after the 3rd death".

Fix (`DeadPlayerUntargetTick`, party_b2.cpp; rules `DeadPlayerOutOfBank` / `BankRemove` / `BankInsert`
in party_enemyai_rules.h): on the host's game (the AI owner), a player chr (its own or a member's copy)
at HP 0 while a boss bar is up or in the host's limbo is taken out of the bank vector as the chr
teardown 0x18BC110 does it (chr +0x3B0 -> +0x40 -> +0x8 is its entry) and put back at the end when it
gets up (arena revive), when the fight ends, or never when the chr is gone (a load or a member that
left took the entry with it; a load builds a new one). The game itself put the dead host's entry back once,
~4 s after the death (live `bk_fix2`), so the tick checks once a second and takes it out again. Put back only into room the vector already had
(its size at removal; capacity word +0x18 when it grew meanwhile). `BB_PARTY_DEAD_UNTARGET=0` off (A/B).
Log: `dead untarget: chr ... out of / back in the enemies' target bank`. Smoke phases `bh` (mortal
guest gets hit after the host's death) and `bk` (boss killed in limbo, host up, back in the bank),
group `boss_dead`.

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

## Late guest in a boss fight (smoke phase hg, 0.7.6, player reports "the boss only fights the host")

Not reproduced on loopback, and a player's host log (Nightwatch 0.2.4-beta.1) shows the same. Phase `hg`
(group `boss_late`, 2 players, Gascoigne, `BB_PARTY_TARGET_ANIM` / `BB_PARTY_AI_LOG`):

* The host owns the boss's AI on both orders (host game: manip Com, owner table 1; guest game: NetAI, owner table 0).
* Host walks in first, the guest 12 s later (and 0 s later, during the intro): the guest's fog walk reaches the host
  ("boss fog: slot 1 went in"), the guest's R1 hits take the boss's HP down identically on both games (3555 -> 1130),
  Gascoigne's phase 2 (anims 1000000+, the beast) fires on both games at ~55 % and the boss is killed.
* Boss target: with the host passive 16 m away and the guest beside the boss, the boss stays at the guest (host-first
  and guest-first alike); with the host beside the boss and the guest 12 m away it stays at the host: it takes
  the nearest player, both are in its target list. Mirror cases of the two reports.
* The host log of the report: both Gascoigne attempts have the guest's hits (boss HP keeps falling after the host's
  death, "boss targets the living member (ChangeTarget ok)"), the host died at 53 % both times (the phase-2 start)
  and the second attempt ended with the guest killing the boss 103.8 s after the host's death.
* `BB_HG_ORDER=guest` (guest goes in first), `BB_HG_DELAY`, `BB_HG_HP`, `BB_HG_HOSTDIST` vary the phase.

## Bosses share their attention (0.7.7, bosstarget, report "bosses only target the host")

Reproduced (smoke `hg`, `BB_HG_MORTAL=1 BB_HG_HOSTDIST=1 BB_HG_GUESTDIST=8`, host invincible beside the boss, guest mortal 8 m
away): with the rule off the boss stayed on the host for the whole fight, never came within 4 m of the guest and the guest's HP
never fell. Cause: the game's target search keeps the nearest player (the earlier `hg` check, "within 4 m of the guest, host
passive 16 m away", only showed that a nearer guest is taken). Fix: `BossAttentionTick` (party_b2.cpp, rule
`enemyai_rules::PickAttention`): on the host's game, while a boss bar is up and 2+ living players are within 30 m of the boss, the
event ChangeTarget path 0x1299CD0 hands the boss the one who was the target longest ago, every 8 s (checked twice a second).
Log: `boss attention: boss ... turns to player chr ...`. Switch `party_boss_share` / `BB_PARTY_BOSS_SHARE=0`. With the rule on
the same run: boss at the guest (39/51 samples < 4 m), guest HP to 0 while the host stood by. Not tested: 3+ players,
bosses other than Gascoigne, the 8 s / 30 m values against real play.

## Laurence and the guest (0.7.8, bossg078, report "bosses never attack the guest")

Not a bug in the logs: a player's host and guest logs (Nightwatch 0.2.6 beta 3, 2 players, Laurence) show the host-side
attention share firing every fight (alternating host / guest chr), the guest's game showing the boss bar in every fight
(also when the guest went through the fog first: the bar and "went through boss fog" lines come in either order) and
the guest taking ~40 boss hits, dying in fights 1-3; the host's copy of the guest loses HP to 0 / 1 in most fights.
The boss hits once per 8 s turn, so a guest next to the host feels ignored half of the time (by design, PickAttention).
Smoke phase `bg` (group `boss_guest`, Laurence, BR fixture, host god beside the boss, mortal guest 8 m away): the bar is up on
both games, the boss turns to the guest and its hit lands on the guest's game 4.5 s after the turn (both screens looked at).
Tune knobs: BB_BG_HOSTDIST, BB_BG_GUESTDIST, BB_BG_LAP. Not reproduced / unproven: the intermittent "stuck on the statue"
(CL-7b1d4e), the guest not starting the fight (every guest log with a fight shows the bar).

### Guest-first Gascoigne: the boss ignores a guest at some spots (0.7.8, bossg078; solved, see the root cause below)

Reproduced deterministically: smoke `hg` with `BB_HG_ORDER=guest BB_HG_MORTAL=1 BB_HG_HOSTDIST=1 BB_HG_GUESTAT=-42.1:-40.76:77.3`
(guest 7-8 m from the boss at a fixed spot; host god beside the boss): every ChangeTarget answers ok, the boss never leaves the
host (0.8 m for 140+ s) and the guest's HP stays flat. The same run with the guest 1.5 m to the east (-40.5:-40.76:77.6,
what `guest_chr=...:8` gave on most runs, 1 in ~4 runs landed on the bad spot because the boss's position varies) is adopted at
once and the guest dies within 20 s. Boss AI target list (`BB_PARTY_AI_LOG=1`, "boss target dump"): AI chr vfunc+0x160 -> +0xB0 ->
+0x560 -> entries from +0x18 (next +8, flags byte +2, kind +0x28, ref +0x48, handle at +0x50, slot ptr +0x58). One entry with
flags 0x87 is the active target; a good ChangeTarget puts the chosen chr's handle there ~1 s later. At the bad spot, and for a guest
19-20 m away anywhere, it never does. Tried and ruled out: re-asserting ChangeTarget up to 3x per turn (no effect); writing the
guest's handle/slot into the 0x87 entry every frame (it holds, the boss still does not move: that entry is a view, the AI goals do
not follow it). So the rejection is inside the AI's own goal / perception logic, position dependent (sight / path / region). Open.

### Root cause and fix (0.7.8, bossg078)

The boss's goals follow the AI's per-chr hate record, not the active entry (flags 0x87) that the event ChangeTarget moves.
A record is a kind-1 entry (flags 0x81 / 0x82) with +0x20 type, +0x38 seconds left, +0x2C, +0x40 active; the game's own "attacker of a hit"
path (0x12A9230) sets type 3, 10.0 s, +0x2C 0, +0x40 1 on the chr that hurt the boss. The host keeps a live record because it hits the
boss; a passive guest has none, so at some spots the boss never turned to it (ChangeTarget ok, active entry moved for a second or not at
all, no hits). `BossLockEntry` (party_b2.cpp) now sets the same record on the chr BossAttentionTick hands the boss to. Proof: smoke `hg`
with `BB_HG_GUESTAT=-42.1:-40.76:77.3` (the former dead spot): 0 hits in 140 s before, 6 hits after (2/2 runs); guest-first `hg` 8/8 runs
with the guest's HP falling; `bg` (Laurence) and `bv`, `bw`, `s3`, `s1` green.

## A guest stays "dead" for the enemies after a wipe (0.7.8, amelia2078, CL-2c1eef, Vicar Amelia's second try)

4-player playtest: Amelia's first try (intro) wiped the party; on the second try the host died early and she animated and
walked but never attacked the guests. Smoke `amp` (group `amelia_phase`, fixture `amelia_alive`) reproduces it with two players
on master and on 0.7.7: after the host's death on try 2, 0 attacks and 0 guest HP drops; on try 1, and on try 2 with the host
alive, she attacks everyone.

How the enemies see players (static RE, confirmed live): the perception tick 0x12AB4C0 (holder vtable 0x531B9F0 slot +0x18 ->
0x12A85C0) searches the target bank with 0x12ADC50 in mode 0, which takes only bank entries whose `[entry +0x18] +0x30 -> +0x18`
bit 0 is clear; the sighted ones are (re)marked in the AI's target list (0x12B2760: flags | 0x80), and the list update 0x12B17C0
removes every entry that was not re-marked that tick (0x12B2490). A hit (0x12A9230) or an event ChangeTarget (0x12AD1B0) only
marks for one tick: without perception such a record is gone again within 0.5 s (live: the limbo's hate record write).

The bit is the dead mark: the host's own entry goes 0 -> 1 at its death. A member's copy re-created by the wipe's re-join came
back with it still set (live: the guest's entry 0 on try 1, 1 on try 2 from its first frame), so no enemy perceived the guest: a
boss fought the host while it lived and nobody once the host was down. `LivingTargetMarkTick` (party_b2.cpp, any role, every
tick) clears the mark on every living player chr (HP > 0, not the spectating / limbo local player), logged once per chr
(`living target: chr ... still had the dead mark`). `BB_PARTY_LIVING_TARGET=0` turns it off. Not the cause (A/B): the dead
untarget, the limbo ChangeTarget, the boss room notice 2009[6], the intro's player chr +0x1E5 bit 0x10 (a remo marker only).
