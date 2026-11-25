"""Bosses whose fight needs bodies besides the boss: Martyr Logarius, Mergo's Wet Nurse, the Orphan of Kos, The One
Reborn.

When another boss takes such an arena, the bodies that served the old boss leave the fight (`dismiss`): their events
are parked, they are disabled at load, and the fight's health bar and end wait only on the arena's boss body. When
such a boss goes to another arena, its bodies come along (`plan` + `add_parts` + `travel`): each becomes a new enemy
part of the target map (a copy of the target boss's own part record, so it loads with the arena), and the events that
drive it are copied into the target map's events with every entity, flag and event id renamed for that arena.

Survey (DATA, vanilla MSB comments, NpcParam and map events; scratch `helpers_survey.json`):
- Logarius (m25, 2500800 c2320, the bar 232000): 2500801 c2321 "the reaper's sword", a body his AI summons (event
  message 100: warped to his dummy poly 100, enabled with AI for up to 40 s, gone when hit); 2500802 c9010, an
  invisible bullet owner that shoots bullet 223200590 at him every second while he has SpEffect 5633 (enraged).
  The fight ends on his death alone (12501800 waits IfCharDead 2500800).
- Wet Nurse (m26): 2600800 c5510 "1st body", the one the player fights (immortal, its damage linked to 2600802);
  2600801 c5510 "2nd body", a clone that appears near the player on message 20 while she has SpEffect 5631 and leaves
  when hit; 2600802 c5510 "HP keeper", placed 3,000 units above the arena with gravity and AI off, the bar's entity;
  the fight ends when 2600802's HP reaches 0. Teleports (12604820/12604830) need six regions of m26.
- Orphan (m36): 3600800 c4540 phase one; 3600801 c4541 phase two, placed 82 units under the arena, gravity and AI
  off, the bar's entity, phase one's damage linked to it (equal HP rows, 2200 each); at half HP of phase one, event
  13604820 disables phase one and warps phase two to its dummy poly 203. 3600803 c4543 is a bullet dummy commanded
  by phase two (13604830). The fight ends on the death of either body. 3600802 c4550 (no AI) is not part of the fight.
- Gascoigne (m24_01): 2410811 c2720 the beast form, 31 units under him with AI and gravity off, his
  damage linked to it (equal HP rows, 1440 each); 12414807 hands over below 34 % HP once his AI answers command 40 with
  message 30 (body 1 disabled, the beast warped to his dummy poly 203, its bar shown); 12414809 is the beast's reaction
  to the music box. The fight ends on either body's death (12411800).
- Ludwig (m34): 3400801 c4510 the Holy Blade form, 104 units under him, AI off, his damage linked to
  it; 13404824 marks the handover below 50 % HP, 13404825 plays it (scene, body 1 disabled and its bar hidden, body 2
  warped to region 3402806 and its bar shown); 13404835 the Holy Blade's own HP steps, 13404841 its replan on message
  300. Every event ends on global flag 9471 (set by the defeat). The fight ends on either body's death (13401800).
- One Reborn (m28): 2800800 c5070 the body; 2800801 c5071 its upper body (immortal, gravity off, held on the body's
  dummy poly 100 by 12804806); 2800802 c5072 an invisible caster that the upper body's messages 10-100 warp and command
  (12804830-12804837; 12804838: the upper body's animation on the body's messages); 2800803 c1050 the HP pool 100 units
  below, the bar's entity, both bodies' damage linked to it (12804802); 12804870/12804871 set its AI phases from the
  pool's HP. 12804840 (the bell maidens) belongs to the arena. Its own arena under another boss is boss_events' work.

Held until the handover (bug 8 Nov, zip 030a): a carried body waits as vanilla parks it (disabled, or AI and gravity off
at its park under or over the arena body), a second stage also invincible; the copied events that let it go wait for
the arena's started flag before their first test (`gate`), and the fight's end counts its death only once the handover
event has run (`_or_death` after=). In that world Ludwig's copied handover ran at the first Hunter's Dream load (LIKELY:
its mark's copy had id 12104861, the own flag of Moon Presence's silenced part template 12104860 run in slot 1, which
ends at load; new ids now skip such flags, `slot_flags`), warped his second stage onto that slot, gravity on, and its
fall death won Moon Presence's fight.

Order in the generator: `dismiss` and `travel` run before boss_events (move_names reads the bar entity these
functions re-point); helper parts are added after Msb.swap_enemies."""
import math
import struct

import arenas
from emevd import LABEL_BANK, SKIPS
from msb import Msb

CALL = (2000, 0)
ENABLE = (2004, 5)
GRAVITY = (2004, 10)
BOSS_BAR = (2003, 11)
CHAR_DEAD = (4, 0)
HP_RATIO = (4, 2)
COND_GROUP = (0, 0)
SHORT_WARP = (2004, 41)
WARPS = (SHORT_WARP, (2004, 42))  # [entity, kind (1 region, 2 character, ...), target, dummy poly(, angle from)]
# The dummy poly a warp onto a boss names (DATA: every boss model has it; the vanilla handovers of Gascoigne, the Orphan
# and the Emissary warp onto 203). A character warp with -1 does nothing (LIVE 13 Nov, NW-f1c2c6c: the stage body stayed
# at its park and fell to its death).
BOSS_POINT = 203
IF_FLAG = (3, 0)  # [group | state << 8, flag]; group 0 waits in place (vanilla: 12504802 i10 [256, 12504800])
CONDITION_BANKS = (0, 1, 3, 4)
AI_SWITCH = (2004, 1)
INVINCIBLE = (2004, 15)
GENERATOR = (2003, 3)  # [generator, 1 on / 0 off]
KILL = (2004, 4)       # [character, 0]
AUTHORITY = (2004, 28)  # [entity, 4095]: the body's state is synced to the guests (every set-up event of a Nocturne boss makes it)
FORCE_ANIMATION = (2003, 18)  # [entity, animation, loop | wait << 8]


class Fight:
    """What the helpers module knows of one fight beyond arenas.py (whose entry, by key, gives the stem, the flags, the
    travelling bodies (`extras`) and the HP pool that stays home)."""

    def __init__(self, key, init, moved, park=(), repoint=(), labels=(), disable=(), bar=None, end_with=None, warps=(),
                 same_hp=(), wake=(), hold=None, home=True, boss=None, travel=None, stage=None,
                 stage_events=(), stage_extra=(), stage_labels=(), stage_bar_after=None, handover=None, made=(),
                 runs=None, as_started=(), models=None, made_from=()):
        arena = arenas.BY_KEY[key]
        self.key, self.stem = key, arena.stem
        self.defeat, self.started = arena.flags
        self.ends_on = arena.ends_on  # a global flag every event of the fight ends on (Ludwig: 9471)
        self.boss = arena.main if boss is None else boss
        self.travel = arena.extras if travel is None else tuple(travel)  # bodies that come along to another arena
        self.stay = (arena.pool,) if arena.pool else ()  # bodies that only made sense in the home map
        self.init = init          # instructions run at load in the new arena, written with the home entities
        self.moved = moved        # events that drive the bodies (or the boss's own handling of them): copied along
        self.park = park          # events that stop when another boss takes the home arena
        self.repoint = repoint    # (event, bank, id, words, word index, new value): home arena, another boss
        self.labels = labels      # (event, bank, id, words): instructions made labels in the home arena
        self.disable = disable    # home bodies disabled at load when another boss takes the arena (with gravity off)
        self.bar = bar            # the entity the home bar sits on, when not the boss
        self.end_with = end_with  # a travelling body whose death also ends the fight
        self.warps = warps        # (moved event, home-map object or region a warp goes to): warp to the boss instead
        self.same_hp = same_hp    # travelling bodies whose NPC row must carry the boss's HP (linked damage)
        self.wake = wake          # travelling bodies whose AI turns on when the fight starts (the arena's started flag)
        self.hold = hold          # the home event that holds the AI until the fight starts: its flag reads as started
        self.home = home          # False: the home arena under another boss is boss_events' work, not dismiss's
        # Option second_stage (user, 5 Nov): the body the boss hands over to holds another boss wherever it fights
        self.stage = stage                    # the home body of the second phase (None: no second phase)
        self.stage_events = stage_events      # moved events of the second phase's own boss: left home under a stage boss
        self.stage_extra = stage_extra        # travelling bodies only that boss used: left home too
        self.stage_labels = stage_labels      # (event, bank, id, words): copied lines only that boss answers, made labels
        self.stage_bar_after = stage_bar_after  # (event, bank, id, words): the stage boss's bar is shown after this line
        # The moved event that hands the fight over to the second body (turns its gravity back on, warps it in, wakes its
        # AI). Its own flag is on once it has run (vanilla's re-entry branch reads it): the second body's death ends the
        # fight only then (bug 8 Nov, zip 030a: a carried second stage handed over at the first Dream load, fell and won
        # Moon Presence's fight). MADE + k: the handover is the fight's k-th `made` event (Nocturne's Moon Presence: lines
        # of her defeat event).
        self.handover = handover
        # Copied events wait for the arena's started flag before their first test (gate): nothing a carried body does
        # (handover, summon, clone) can happen before the fight. Not for a fight whose bodies' set-up runs from load
        # (the One Reborn: 12804806 keeps the upper body on the body; its AI waits on the started flag, `wake`).
        self.gate = hold is None
        # Events the home map holds only as lines of a longer event (so they cannot be copied whole): written here with the
        # home entities and made events of their own in the new arena, each waiting for the started flag first.
        self.made = made
        # the home event each `made` event's lines come from (None: several), recorded as its source: a copy of an event a
        # mod lists host-only stays host-only (hostonly.rank)
        self.made_from = tuple(made_from) + (None,) * (len(made) - len(made_from))
        # Moved events that are templates: {event: [(slot, argument words)]}, the home map's runs of each. The copy keeps
        # its parameters and is run once per home run, with the arguments renamed (a phase body's helpers, found_helpers)
        self.runs = dict(runs or {})
        self.as_started = tuple(as_started)  # home flags the copies read as the new arena's started flag
        self.models = dict(models or {})     # {travelling body: model} when the census has no slot for it


_BASE_FIGHTS = {
    # DATA m25 12504802 (AI hold): sword AI off, small bar off, immortal, disabled; bullet owner set and disabled.
    2500800: Fight("logarius",
                   init=((2004, 1, (2500801, 0)), (2004, 30, (2500801, 0)), (2004, 12, (2500801, 1)),
                         (2004, 5, (2500801, 0)), (2004, 7, (2500802,)), (2004, 5, (2500802, 0))),
                   # sword summon on message 100; enraged bullet; end of enrage on message 10 (forces his anim 2100)
                   moved=(12504806, 12504807, 12504808),
                   park=(12504804, 12504806, 12504807, 12504808),  # 12504804: lock camera of area 25 on message 100
                   repoint=(), labels=(), disable=()),            # both stay disabled by 12504802; end = his death
    # DATA m26 12604806 (at load: clone disabled, no gravity, no map hit), 12604802 (immortal, AI, small bar), and
    # its damage goes to the HP keeper; in a one-body arena the boss holds the HP, so the clone's goes to the boss.
    2600800: Fight("wet_nurse",
                   init=((2004, 5, (2600801, 0)), (2004, 10, (2600801, 0)), (2004, 31, (2600801, 1)),
                         (2004, 1, (2600801, 1)), (2004, 30, (2600801, 0)), (2004, 12, (2600801, 1)),
                         (2004, 33, (2600801, 2600800))),
                   moved=(12604810, 12604840),  # SpEffect 5631 and AI 100/110 while the player has 5630; the clone
                   park=(12604810, 12604820, 12604830, 12604840),  # + her teleports (message 10, six m26 regions)
                   repoint=((12601800, 4, 2, (0, 2600802, 5, 0), 1, 2600800),       # the end: HP keeper -> body
                            (12601801, 4, 2, (2, 2600802, 5, 0), 1, 2600800),       # a sound at HP 0
                            (12604803, 4, 2, (0, 2600802, 3, 1060320051), 1, 2600800),  # music at 70 % HP
                            (12604802, 2003, 11, (1, 2600802, 0, 551000), 1, 2600800)),  # the bar
                   labels=((12604802, 2004, 12, (2600800, 1)), (12604802, 2004, 33, (2600800, 2600802)),
                           (12604802, 2004, 33, (2600801, 2600802))),
                   disable=(2600802,), bar=2600802,
                   warps=((12604840, 2601857),)),  # the clone warps to an m26 anchor object: to the boss instead
    # DATA m36 13604802 (AI and small bar off, phase one's damage linked to phase two, bar on phase two), 13604820
    # (gravity off for phase two at load, the handover at half HP), 13601800 (ends on either death).
    3600800: Fight("orphan",
                   init=((2004, 1, (3600801, 0)), (2004, 30, (3600801, 0)), (2004, 10, (3600801, 0)),
                         (2004, 33, (3600800, 3600801))),
                   moved=(13604820, 13604830, 13604840),  # handover; bullet dummy AI; player effect on message 30
                   park=(13604820, 13604830, 13604840, 13604850),  # + 13604850: area 36 lock camera on phase two
                   repoint=((13604802, 2003, 11, (1, 3600801, 0, 453000), 1, 3600800),),
                   # phase two's link; the AI-on branch and the camera both read 13604820's flag: phase one always
                   labels=((13604802, 2004, 33, (3600800, 3600801)), (13604802, 1003, 1, (260, 13604820)),
                           (13604804, 1003, 101, (256, 13604820))),
                   disable=(3600801, 3600803), bar=3600801, end_with=3600801, same_hp=(3600801,), handover=13604820,
                   # a stage boss: no bullet dummy; the handover waits for phase two's message 50 (after its entry
                   # animation) and shows no bar of its own (vanilla's sat on phase two from the start)
                   stage=3600801, stage_events=(13604830,), stage_extra=(3600803,),
                   stage_labels=((13604820, 4, 8, (0, 3600801, 50, 1)),),
                   stage_bar_after=(13604820, 2004, 1, (3600801, 1))),
    # DATA m28 12804802 (AI hold): AI, small bar and gravity off for both, the upper body immortal, both bodies' damage
    # linked to the pool and their event target the pool; AI on when the started flag 12804800 is on. In a one-body
    # arena the boss holds the HP: the pool reads as the boss in everything that travels (the upper body's damage, the
    # phase events' HP tests, the bar's name 507000).
    2800800: Fight("one_reborn",
                   init=((2004, 1, (2800801, 0)), (2004, 1, (2800802, 0)), (2004, 30, (2800801, 0)),
                         (2004, 30, (2800802, 0)), (2004, 10, (2800801, 0)), (2004, 10, (2800802, 0)),
                         (2004, 12, (2800801, 1)), (2004, 33, (2800801, 2800803)), (2004, 11, (2800801, 2800803))),
                   # upper body on dummy poly 100; the caster's seven casts (no 12804833); upper body animations; the AI phases
                   moved=(12804806, 12804830, 12804831, 12804832, 12804834, 12804835, 12804836, 12804837, 12804838,
                          12804870, 12804871),
                   bar=2800803, wake=(2800801, 2800802), hold=12804802, home=False),
}
# The fights with a second body that travels with the boss (arenas.py: SOLO with extras).
# DATA m24_01 12414802 (AI and small bar off for both, body 1's damage linked to body 2, bar on body 1 unless 12414807's
# flag is on), 12414807 (gravity off for body 2 at load, the handover), 12411800 (ends on either death).
_GASCOIGNE_ONE = Fight("gascoigne",
                       init=((2004, 1, (2410811, 0)), (2004, 30, (2410811, 0)), (2004, 10, (2410811, 0)),
                             (2004, 33, (2410810, 2410811))),
                       moved=(12414807, 12414809),  # the handover; the beast's music box reaction
                       park=(12414807, 12414809),
                       # the link; the bar branch reads 12414807's flag (parked: the body-1 branch always)
                       labels=((12414802, 2004, 33, (2410810, 2410811)), (12414802, 1003, 101, (261, 12414807))),
                       disable=(2410811,), end_with=2410811, same_hp=(2410811,), boss=2410810, travel=(2410811,),
                       handover=12414807,
                       stage=2410811, stage_events=(12414809,))
# DATA m34 13404802 (AI and small bar off for both, body 1's damage linked to body 2, bar on body 1 unless 13404825's flag
# is on), 13404825 (gravity and map hit off for body 2 at load; the handover warps body 2 to region 3402806 of m34: to the
# boss instead), 13401800 (ends on either death).
_LUDWIG_ONE = Fight("ludwig",
                    init=((2004, 1, (3400801, 0)), (2004, 30, (3400801, 0)), (2004, 10, (3400801, 0)),
                          (2004, 33, (3400800, 3400801))),
                    moved=(13404824, 13404825, 13404835, 13404841),  # handover mark, handover, body 2's steps and replan
                    park=(13404824, 13404825, 13404835, 13404841),
                    # the link; the AI and bar branch reads 13404825's flag (parked: the body-1 branch always)
                    labels=((13404802, 2004, 33, (3400800, 3400801)), (13404802, 1003, 1, (261, 13404825))),
                    disable=(3400801,), end_with=3400801, same_hp=(3400801,), warps=((13404825, 3402806),),
                    handover=13404825,
                    boss=3400800, travel=(3400801,), stage=3400801, stage_events=(13404835, 13404841))
# DATA Nocturne v5.0.0 m23: 12304822 (hold: AI, small bar and gravity off for the head, immortal; after the started flag 2004[28]
# and the doping for both; the head's damage linked to the body), 12304823 (the carry loop: the head onto the body's dummy poly
# 100), 12301820 (ends on the body's death; kills the head). The head's death with him and its breath at half HP are lines
# of 12301820 and the tail of 12304822 (home events): written out as `made` events. No bar of the head's own.
_LESSER_DEMON = Fight("lesser_demon",
                      init=((2004, 1, (2300821, 0)), (2004, 30, (2300821, 0)), (2004, 10, (2300821, 0)),
                            (2004, 12, (2300821, 1)), (2004, 33, (2300821, 2300820)), (2004, 11, (2300821, 2300820))),
                      moved=(12304823,),
                      park=(12304823,),
                      disable=(2300821,), hold=12304822, wake=(2300821,),
                      made=(((4, 0, (0, 2300820, 1)), (2004, 43, (2300821, 1)), (2004, 4, (2300821, 0))),
                            ((4, 2, (0, 2300820, 5, 1056964608)), (2004, 8, (2300821, 5500010, 0)))))
FIGHTS = dict(sorted({**_BASE_FIGHTS, 2410810: _GASCOIGNE_ONE, 3400800: _LUDWIG_ONE, 2300820: _LESSER_DEMON}.items()))  # {home body: Fight}
LISTED = dict(FIGHTS)
MADE = "made"  # Fight.handover (MADE, k): the k-th made event hands over

# Nocturne v6.1.0's Moon Presence phase two (option moon_presence_two; user 25 Nov: "add a toggle for her", "make sure the
# randomizer fixes her when shes transported into other areas"). DATA, the mod's m21 (bb-modding mods\moon-presence-kos):
# c5420 2100860 (row 542000) waits 82 m under the Moon Presence 2100810. 12100800 i0-3 hold her at load (AI and small bar
# off, invincible, disabled); 12104852 at the fight's start enables her parked (gravity, AI and small bar off, not
# invincible), links the Moon Presence's damage to her, raises her HP (SpEffect 7635, 2004[53]), makes her authority call
# (i9), dopes her for co-op beside the Moon Presence (i29 / i32, i36 / i39) and shows the bar on her (i43). The handover is
# lines 5-28 of the defeat event 12101850 (her bar at half or the Moon Presence at 4 %: the Moon Presence falls (2200),
# she is warped onto its dummy poly 203, the Moon Presence disabled, her AI on); the tail waits for her death (i29) and gives
# the arena's rewards. 12104871 is her Doom Gaze (12104870 on her message 10). Away from the Dream she travels with the
# Moon Presence as the Orphan's phase two does: held at load, started with the fight (made event 0, the lines of 12104852),
# handed over by made event 1 (the lines of 12101850, host-only there: party_host_only.txt), her death ending the fight
# once it ran; at home another boss's fight drops her lines and waits on the slot body's death again. Found in the inputs
# (found_mod_fights), never assumed: without her the fight is the game's.
MOON, MOON_TWO = 2100810, 2100860
_MOON_SETUP, _MOON_DEFEAT, _MOON_STARTED = 12104852, 12101850, 12104850
_MOON_START = ((2004, 5, (MOON_TWO, 1)), (2004, 10, (MOON_TWO, 0)), (2004, 1, (MOON_TWO, 0)), (2004, 30, (MOON_TWO, 0)),
               (2004, 15, (MOON_TWO, 0)), (2004, 33, (MOON, MOON_TWO)), (2004, 8, (MOON_TWO, 7635, 1)))
_MOON_FILL = (2004, 53, (MOON_TWO,))
_MOON_HANDOVER = (
    (4, 2, (1, MOON_TWO, 3, 1056964608)), (4, 2, (2, MOON, 3, 1025758986)),  # her HP <= 0.5, the Moon Presence's <= 0.04
    (0, 0, (66047,)), (0, 0, (131583,)), (0, 0, (16711936,)),                # either
    (2004, 1, (MOON, 0)), (2004, 15, (MOON, 1)), (2004, 8, (MOON_TWO, 7636, 0)), (2004, 8, (MOON, 7636, 0)),
    (2004, 43, (MOON, 0)), (2003, 18, (MOON, 2200, 0)), (1001, 0, (1085905306,)), (2004, 10, (MOON_TWO, 1)),
    (2004, 34, (MOON, 65281)), (2004, 34, (MOON_TWO, 1)), (2004, 42, (MOON_TWO, 2, MOON, BOSS_POINT, MOON)),
    (2004, 5, (MOON, 0)), (2003, 18, (MOON_TWO, 7001, 0)), (1001, 0, (1065353216,)), (2003, 18, (MOON_TWO, 7000, 0)),
    (2004, 11, (MOON_TWO, 10000)), (2004, 1, (MOON_TWO, 1)), (1001, 0, (1081585172,)), (2004, 21, (MOON_TWO, 7636)))
_MOON_END_IF_CLEARED = (1003, 2, (256, _MOON_DEFEAT))  # 12104852 i0: nothing once the fight is won
MOON_PRESENCE_TWO = Fight(
    "moon_presence",
    init=((2004, 1, (MOON_TWO, 0)), (2004, 30, (MOON_TWO, 0)), (2004, 15, (MOON_TWO, 1)), (2004, 5, (MOON_TWO, 0)),
          (2004, 10, (MOON_TWO, 0))),  # 12100800 i0-3, and gravity off at her park
    moved=(12104871,), park=(12104871,),
    repoint=((_MOON_SETUP, 2003, 11, (1, MOON_TWO, 0, 540000), 1, MOON),     # the bar back on the slot body
             (_MOON_DEFEAT, 4, 0, (0, MOON_TWO, 1), 1, MOON)),               # the end on the slot body's death
    labels=tuple((_MOON_SETUP,) + line for line in ((2004, 28, (MOON_TWO, 4095)),) + _MOON_START + (_MOON_FILL,)
                 + ((2004, 8, (MOON_TWO, 7500, 1)), _MOON_FILL, (2004, 8, (MOON_TWO, 7501, 1)), _MOON_FILL))
    + tuple((_MOON_DEFEAT,) + line for line in _MOON_HANDOVER),
    disable=(MOON_TWO,), bar=MOON_TWO, end_with=MOON_TWO, travel=(MOON_TWO,), boss=MOON, handover=(MADE, 1),
    made=((_MOON_END_IF_CLEARED, (3, 0, (256, _MOON_STARTED))) + _MOON_START + ((1001, 1, (1,)), _MOON_FILL),
          (_MOON_END_IF_CLEARED,) + _MOON_HANDOVER),
    made_from=(None, _MOON_DEFEAT))
MOD_FIGHTS = {"moon_presence_two": MOON_PRESENCE_TWO}  # found_mod_fights' names
MOD_MODELS = {"moon_presence_two": (MOON_TWO, "m21_00", "c5420")}  # the body a mod adds: entity, map, model


def _holds(lines, wanted):
    """Whether every (bank, id, words) of `wanted` is among `lines`, as often as `wanted` names it."""
    left = [(b, i, tuple(w)) for b, i, w in lines]
    for bank, iid, words in wanted:
        line = (bank, iid, tuple(words))
        if line not in left:
            return False
        left.remove(line)
    return True


def found_mod_fights(events, enemies):
    """{home body: {"fight": name}} of the listed fights a base mod adds a body to (MOD_FIGHTS), found in the inputs: the
    body is an enemy part of the map with its model, and every line the fight edits or copies is in its events (a mod
    version with other events counts as none, so no edit lands on a line it lacks). events, enemies: as found_helpers."""
    out = {}
    for name, fight in sorted(MOD_FIGHTS.items()):
        entity, stem, model = MOD_MODELS[name]
        part = enemies.get(stem, {}).get(entity)
        own = events.get(stem, {})
        if part is None or part["model"] != model or any(e not in own for e in fight.moved):
            continue
        wanted = {}
        for event, bank, iid, words in fight.labels:
            wanted.setdefault(event, []).append((bank, iid, words))
        for event, bank, iid, words, _, _ in fight.repoint:
            wanted.setdefault(event, []).append((bank, iid, words))
        if all(event in own and _holds(own[event][0], lines) for event, lines in wanted.items()):
            out[fight.boss] = {"fight": name}
    return out


# A boss's map-side helpers (arenas.FOUND_HELPERS; user 23 Nov: "the Coiled Host's helpers travel with him", "what about
# Martyr Logarius' helpers? that's another example"). Found, never listed, for the phase bodies and the one-body arenas'
# bosses without a listed Fight (the group fights' adds stay arena parts: arenas.py notes). DATA Nocturne v6.0.0 m27: the
# Coiled Host's dip (3035) has no hit of its own; at 0.15 s his AI sends message 20, on which 12704813 (run from event 0
# once per snake ball 2700803-805, c5033 row 503300) enables a ball with its AI; 12704830 (one run per ball) kills the
# balls once his HP is 0; his pools (the user's "puddles"): 12704851 shoots the pool bullet 2770001 from the invisible
# producers 2701968-970 (c9010, owner 2701966) once 12704808 is on (the defeat event 12701800 sets it as he wakes),
# 12704852 renews their look (effect 651207, resident) every 5 s while he lives. DATA m24_02: Ebrietas's 12424990 shoots
# bullet 225100310 from the invisible owner 2420801 (c9010, made a bullet owner by event 0) every 0.5 s while she has
# SpEffect 5650; Logarius's enraged bullet (12504807, owner 2500802) is the same shape (listed above). The rule, over every
# event of the home map but event 0 / 50, the defeat event, the intro and the body's hold (2004[01] [body, 0]) that names
# the body in its own lines: a helper is an enemy part of the map, no boss body, that such an event makes act (enables,
# wakes its AI, shoots from or at it; a template's lines with each run's arguments put in). Every such event that names a
# helper (in its lines or its runs' arguments) travels, with the parts it names; the lines of event 0 that set a helper up
# (its bullet owner call) run at load in the new arena. A flag of the home block that the defeat event turns on and such
# an event reads is the fight's start. Its own fight never names a helper it shares with an arena boss: Logarius's sword
# and owner are his arena's extras, Gehrman's and Maria's swapped-out bodies are only disabled, Rom's spiders and the
# Cleric Beast's patient never act on the boss's events.
SHOOT = (2003, 5)  # [owner, source, dummy poly, bullet, ...]


def _runs(events):
    """{template: [(slot, argument words)]} of the 2000[00] calls in one map's events (_events format)."""
    out = {}
    for instructions, _ in events.values():
        for bank, iid, words in instructions:
            if (bank, iid) == CALL and len(words) > 1 and words[1] in events:
                out.setdefault(words[1], []).append((words[0], tuple(words[2:])))
    return out


def _acting(lines, params, runs):
    """The entities lines make act (enable, AI on, shoot from or at), once with the words as written and once per run
    with its arguments put in (params: {line: {target byte offset: source byte offset}})."""
    out = set()
    for args in [None] + [a for _, a in runs]:
        for i, (bank, iid, words) in enumerate(lines):
            words = list(words)
            if args is not None:
                for target, source in params.get(i, {}).items():
                    if target % 4 == 0 and source % 4 == 0 and target // 4 < len(words) and source // 4 < len(args):
                        words[target // 4] = args[source // 4]
            if (bank, iid) in (ENABLE, AI_SWITCH) and len(words) > 1 and words[1] == 1:
                out.add(words[0])
            elif (bank, iid) == SHOOT and len(words) > 1:
                out.update(words[:2])
    return out


def found_helpers(events, enemies, bodies):
    """{body: {"travel", "moved", "runs", "as_started", "models", "init"}} for the bodies with helpers (the rule above).
    events: {stem: {event: (instructions, params)}} (gamedata._events); enemies: {stem: {entity: MSB enemy part}};
    bodies: {entity: arena key} (the phase bodies, arenas.PHASES, and the one-body arenas' unlisted bosses)."""
    out = {}
    bosses = set(arenas.BY_ENTITY) | set(bodies)
    for body, key in sorted(bodies.items()):
        arena = arenas.BY_KEY[key]
        own, parts = events.get(arena.stem, {}), enemies.get(arena.stem, {})
        runs = _runs(own)
        holds = {e for e, (lines, _) in own.items() if any((b, i) == AI_SWITCH and tuple(w[:2]) == (body, 0) for b, i, w in lines)}
        skip = {0, 50, arena.flags[0], arena.intro} | holds
        helper = lambda x: x > 0 and x in parts and x not in bosses
        naming = [e for e in sorted(own) if e not in skip and any(body in w for _, _, w in own[e][0])]
        acting = set()
        for event in naming:
            acting |= {x for x in _acting(own[event][0], own[event][1], runs.get(event, ())) if helper(x)}
        moved, travel = [], set()
        for event in naming:
            named = {x for _, _, w in own[event][0] for x in w} | {x for _, args in runs.get(event, ()) for x in args}
            if named & acting:
                moved.append(event)
                travel |= {x for x in named if helper(x)}
        if not moved:
            continue
        block = arena.flags[0] // 10000
        read = {x for e in moved for b, _, w in own[e][0] if b in (3, 1003) for x in w}
        starts = sorted({w[0] for b, i, w in own.get(arena.flags[0], ((), {}))[0]
                         if (b, i) == (2003, 2) and len(w) > 1 and w[1] == 1 and w[0] // 10000 == block and w[0] in read})
        init = tuple((b, i, tuple(w)) for b, i, w in own.get(0, ((), {}))[0]
                     if b >= 2000 and (b, i) != CALL and any(x in travel for x in w))
        out[body] = {"travel": tuple(sorted(travel)), "moved": tuple(moved),
                     "runs": {e: list(runs[e]) for e in moved if e in runs and own[e][1]},
                     "as_started": tuple(starts), "models": {e: parts[e]["model"] for e in sorted(travel)}, "init": init}
    return out


def _sync_phase_fights():
    """FIGHTS, BY_KEY and OWNED with the found helpers (arenas.FOUND_HELPERS) beside the listed fights. A phase body's
    fight never dismisses at home (its home arena is the Shadows' own business); a one-body arena's boss parks its
    helper events at home when another boss takes the arena (they would act on that boss's SpEffects and messages)."""
    FIGHTS.clear()
    FIGHTS.update(LISTED)
    for body, spec in sorted(arenas.FOUND_HELPERS.items()):
        if "fight" in spec:  # a listed fight a base mod adds a body to (found_mod_fights)
            FIGHTS[body] = MOD_FIGHTS[spec["fight"]]
            continue
        phase = body in arenas.PHASES
        key = arenas.PHASES[body] if phase else arenas.BY_ENTITY[body].key
        FIGHTS[body] = Fight(key, init=spec["init"], moved=spec["moved"], park=() if phase else spec["moved"], boss=body,
                             travel=spec["travel"], home=not phase, runs=spec["runs"], as_started=spec["as_started"],
                             models=spec["models"])
    BY_KEY.clear()
    BY_KEY.update({f.key: f for f in FIGHTS.values()})
    OWNED.clear()
    OWNED.update({boss: frozenset(f.moved) | frozenset(f.park) for boss, f in FIGHTS.items()})


# Phase one alone (planner.second_bodies, user 10 Nov: each boss once per run): a Gascoigne, Ludwig or Orphan whose
# second body holds nobody travels as phase one alone, its fight ending on its own death. Gascoigne and Ludwig bring
# nothing (no handover, no link); the Orphan brings his player effect on message 30 (DATA m36 13604840).
ALONE = {2410810: None, 3400800: None,
         3600800: Fight("orphan", init=(), moved=(13604840,), park=(13604840,), bar=3600801, boss=3600800, travel=()),
         # option moon_presence_two off with her phase two in the inputs: the Moon Presence alone, with the bar name her
         # home bar shows on her phase two
         MOON: Fight("moon_presence", init=(), moved=(), bar=MOON_TWO, boss=MOON, travel=())}


def fight_of(fighter):
    """The Fight that travels with this fighter (planner dict, or a home body): FIGHTS', or for a fighter marked
    "alone" its phase one's (ALONE); None for a boss that brings nothing."""
    origin = fighter["origin"] if isinstance(fighter, dict) else fighter
    if isinstance(fighter, dict) and fighter.get("alone"):
        return ALONE[origin]
    return FIGHTS.get(origin)


# Events owned here, per home body: boss_events must neither silence nor copy them as phase or part events of these
# bosses.
OWNED = {boss: frozenset(f.moved) | frozenset(f.park) for boss, f in FIGHTS.items()}
BY_KEY = {f.key: f for f in FIGHTS.values()}
arenas.ON_PHASES.append(_sync_phase_fights)
PART_GAP = 60  # new entities and event ids from base + 60 to base + 99, apart from boss_events' + 90 range


def arena_flags(body):
    """(defeat flag, fight-started flag) of the arena a boss body belongs to (arenas.py)."""
    return arenas.arena_of(body).flags


def extras_of(slots, origin):
    """(extras, extra_models) for the fighter dict of the boss whose home body is `origin` (planner side). extras are
    the bodies that travel with it; extra_models the models they add besides the boss's own, which count against the
    new map's budget. slots: census.slots. ((), ()) for any other boss."""
    if origin not in FIGHTS:
        return (), ()
    fight = FIGHTS[origin]
    by_entity = {s["entity"]: s for s in slots[fight.stem]}
    model = lambda e: fight.models[e] if e in fight.models else by_entity[e]["model"]
    return tuple(fight.travel), tuple(sorted({model(e) for e in fight.travel} - {model(origin) if origin in fight.models
                                                                                    else by_entity[origin]["model"]}))


def stage_bosses():
    """The home bodies whose fight hands over to a second body (option second_stage): Gascoigne, Ludwig, the Orphan."""
    return frozenset(boss for boss, f in FIGHTS.items() if f.stage is not None)


def with_stage(fighter, stage, slots):
    """The fighter (planner dict of a boss in stage_bosses) with `stage` (another fighter dict) in its second body: the
    stage's model, rows, origin and bar name ride along as fighter["stage"], and the models it brings change (the
    stage's model in place of the second phase's, none of the second phase's own extras). A stage brings no bodies of
    its own: no chain."""
    fight = FIGHTS[fighter["origin"]]
    by_entity = {s["entity"]: s for s in slots[fight.stem]}
    keep = [e for e in fight.travel if e != fight.stage and e not in fight.stage_extra]
    out = dict(fighter)
    out["stage"] = {k: stage.get(k) for k in ("key", "model", "think", "npc", "origin", "bar_name")}
    out["extras"] = [e for e in fighter["extras"] if e not in fight.stage_extra]
    out["extra_models"] = sorted(({by_entity[e]["model"] for e in keep} | {stage["model"]}) - {fighter["model"]})
    return out


def _staged(fight, fighter):
    """The fighter's stage dict when this fight's second body holds a stage boss, else None."""
    stage = fighter.get("stage") if isinstance(fighter, dict) else None
    return stage if stage is not None and fight.stage is not None else None


# ---------------------------------------------------------------- events

def _find(events, event, bank, iid, words):
    """Index of the instruction (bank, iid, words) in event; ValueError when the map's events differ from vanilla's."""
    for i, (b, d, w) in enumerate(events.instructions(event)):
        if (b, d) == (bank, iid) and tuple(w) == tuple(words):
            return i
    raise ValueError("event %d has no %d[%02d] %r" % (event, bank, iid, words))


def _new_event(events, caller, event, body):
    events.add_event(event, 0, body)
    events.append_instruction(caller, CALL[0], CALL[1], [0, event, 0])


def _caller(events, arena):
    """The event that starts the arena's AI-switch event (event 0 in every main-map arena, DATA): where the new
    events are started too."""
    ids = events.event_ids()
    starter = next(e for e in ids if any((b, i) == AI_SWITCH and w and w[0] == arena for b, i, w in events.instructions(e)))
    return next(e for e in ids if any((b, i) == CALL and len(w) > 1 and w[1] == starter for b, i, w in events.instructions(e)))


def park(events, event, flag):
    """The event waits for `flag` (the arena's defeat flag) and ends: it never finishes during the fight, so an event
    that reads its flag keeps its first branch (silencing would end it at once and turn its flag on)."""
    events.set_body(event, [(IF_FLAG[0], IF_FLAG[1], [256, flag])])


def dismiss(events, arena_key):
    """Another boss has taken the arena `arena_key` ("logarius", "wet_nurse", "orphan", "gascoigne", "ludwig" in their
    default form; any other key, and "one_reborn", whose home arena boss_events handles: None), or the body of the Fight
    given instead of a key: the old boss's other bodies leave the fight, the bar and the end wait only on the slot body
    (arenas.py `main`). Call it before boss_events' passes. Returns a report."""
    fight = arena_key if isinstance(arena_key, Fight) else BY_KEY.get(arena_key)
    if fight is None or not fight.home:
        return None
    arena = fight.boss
    report = {"arena": arena, "parked": [], "repointed": [], "labels": [], "disabled": list(fight.disable), "event": None}
    for event in fight.park:
        park(events, event, fight.defeat)
        report["parked"].append(event)
    for event, bank, iid, words, k, value in fight.repoint:
        i = _find(events, event, bank, iid, words)
        events.set_word(event, i, k, value)
        report["repointed"].append((event, i, words[k], value))
    for event, bank, iid, words in fight.labels:
        i = _find(events, event, bank, iid, words)
        events.neutralize(event, i)
        report["labels"].append((event, i))
    if fight.disable:
        body = []
        for entity in fight.disable:
            body += [(GRAVITY[0], GRAVITY[1], [entity, 0]), (ENABLE[0], ENABLE[1], [entity, 0])]
        report["event"] = free_event_ids(events, arena, 1)[0]
        _new_event(events, _caller(events, arena), report["event"], body)
    return report


def slot_flags(events):
    """The flags of the runs of template events in slots above 0: run n of event X keeps its own flag X + n (LIKELY: the
    flag type "this event slot" of 1003[101] and the vanilla habit of leaving X+1..X+n unused as event ids after such runs;
    m21 runs Moon Presence's part template 12104860 in slots 0-4). A new event must never take such an id: its flag is
    turned on whenever that run ends (bug 8 Nov, zip 030a: generated m21 had silenced 12104860, whose five runs end at
    load, and gave its copied Ludwig handover the id 12104862 and the handover mark 12104861; the handover waits on the
    mark's flag)."""
    out = set()
    for event in events.event_ids():
        for bank, iid, words in events.instructions(event):
            if (bank, iid) == CALL and len(words) > 1 and words[0] > 0:
                out.update(range(words[1] + 1, words[1] + words[0] + 1))
    return out


def free_event_ids(events, arena, count, blocks=None):
    """Event ids from the arena's started flag block + PART_GAP that no event here uses and no word here names; when
    those 30 run out (one draw for every body of a group: two or three bosses with bodies of their own travel into one
    arena, the One Reborn alone needs 12), the rest of the block's thousand from X4100, outside the X4800-X4899 hundred,
    that no flag range here covers either (boss_events' rule)."""
    named = set(events.event_ids()) | slot_flags(events)
    for event in events.event_ids():
        for _, _, words in events.instructions(event):
            named.update(words)
    base = arena_flags(arena)[1] // 100 * 100
    free = [c for c in range(base + PART_GAP, base + 90) if c not in named]
    if len(free) < count or blocks is not None:
        import boss_events  # here: boss_events imports this module
        taken = boss_events._taken(events)
        block = base // 1000 * 1000
        free += [c for c in range(block + 100, block + 800) if not base <= c < base + 100 and c not in taken]
    if blocks is not None:  # templates run in slots 0..n-1: id and the n-1 flags after it all free (slot_flags)
        left, out = set(free), []
        for size in blocks:
            start = next((c for c in free if all(c + k in left for k in range(size))), None)
            if start is None:
                raise ValueError("no %d free event ids in a row near %d" % (size, base))
            left -= set(range(start, start + size))
            out.append(start)
        return out
    if len(free) < count:
        raise ValueError("no %d free event ids near %d" % (count, base))
    return free[:count]


def _old_map(word, origin, defeat):
    """A seven-digit id of the boss's home map (region, object, part) or an eight-digit flag of its flag block."""
    return (1000000 <= word < 10000000 and word // 10000 == origin // 10000) or \
        (10000000 <= word < 100000000 and word // 10000 == defeat // 10000)


def rewrite(body, renames, origin, defeat):
    """Instructions for the new arena: words renamed; an instruction still naming something of the home map becomes
    a label (it only made sense there). Every such label takes the same id, one no jump of the event uses: a jump
    goes to the next label of its own id, so a repeated unused id is never a target."""
    used = {iid for bank, iid, _ in body if bank == LABEL_BANK}
    free = [i for i in reversed(range(10)) if i not in used]
    out = []
    for bank, iid, words in body:
        if any(w not in renames and _old_map(w, origin, defeat) for w in words):
            if not free:
                raise ValueError("no free label in a moved event")
            out.append((LABEL_BANK, free[0], []))
        else:
            out.append((bank, iid, [renames.get(w, w) for w in words]))
    return out


def _or_death(events, event, arena, other, after=None):
    """Each condition of `event` on the arena boss's death or HP is or-ed with `other`'s death, in a free or-group.
    after: a flag (the handover event's own): `other`'s death counts only with it on, in a free and-group (a carried
    body ends the fight only once its handover has let it in)."""
    body = events.instructions(event)
    used = set()
    for bank, _, words in body:
        if (bank in CONDITION_BANKS or bank == 1000) and words:
            used.update(((words[0] >> s) & 0xff) for s in (0, 8, 16))
    groups = [g for g in range(0xff, 0xf8, -1) if g not in used]
    ands = [g for g in range(7, 0, -1) if g not in used]
    done = 0
    i = 0
    while i < len(body):
        bank, iid, words = body[i]
        if (bank, iid) in (CHAR_DEAD, HP_RATIO) and len(words) > 1 and words[1] == arena:
            if not groups or (after is not None and not ands):
                raise ValueError("no free condition group in event %d" % event)
            group, o = words[0] & 0xff, groups.pop(0)
            if after is None:
                death = [(CHAR_DEAD[0], CHAR_DEAD[1], [o, other, 1])]
            else:
                a = ands.pop(0)
                death = [(CHAR_DEAD[0], CHAR_DEAD[1], [a, other, 1]), (IF_FLAG[0], IF_FLAG[1], [a | 1 << 8, after]),
                         (COND_GROUP[0], COND_GROUP[1], [o | 1 << 8 | a << 16])]
            new = [(bank, iid, [o] + list(words[1:]))] + death + [(COND_GROUP[0], COND_GROUP[1], [group | 1 << 8 | o << 16])]
            events.splice(event, i, new)
            body = events.instructions(event)
            i += len(new)
            done += 1
        else:
            i += 1
    return done


def insert_line(body, k, line):
    """body (a list of (bank, id, words)) with `line` put before line k; skips that jump over the place grow by one."""
    out = []
    for j, (bank, iid, words) in enumerate(body[:k]):
        words = list(words)
        if (bank, iid) in SKIPS and words and j < k <= j + (words[0] & 0xff):
            if (words[0] & 0xff) == 0xff:
                raise ValueError("skip too long")
            words[0] += 1
        out.append((bank, iid, words))
    return out + [line] + [(b, d, list(w)) for b, d, w in body[k:]]


def gate(body, started):
    """A copied event's lines with a wait on the arena's started flag put before its first test: what it does to the
    bodies it drives (handover, summon, clone) waits for the fight. Its load lines before that test (the re-entry
    branch, gravity off for a parked body) still run at load. Unchanged when the event has no test or already waits
    for that flag first."""
    line = (IF_FLAG[0], IF_FLAG[1], [256, started])
    k = next((i for i, (bank, _, _) in enumerate(body) if bank < 1000), None)
    if k is None or (body[k][0], body[k][1], list(body[k][2])) == line:
        return list(body)
    return insert_line(body, k, line)


def _template_copy(origin_events, event, home, body, runs, renames, origin, defeat):
    """A moved template (Fight.runs): (body, parameters, calls [(slot, renamed arguments)]). A line fed by an argument
    that some run gives as a thing of the home map this arena lacks (Nocturne's snake-ball generators 2705001-003) becomes a
    label like the lines rewrite drops, and that argument is 0 in every call; a label drops its parameters."""
    params = origin_events.param_entries(event)
    stale = {k * 4 for _, args in runs for k, w in enumerate(args) if w not in renames and _old_map(w, origin, defeat)}
    body = list(body)
    used = {iid for bank, iid, _ in home if bank == LABEL_BANK}
    label = next(i for i in reversed(range(10)) if i not in used)
    for i, _, src, _ in params:
        if src in stale and body[i][0] != LABEL_BANK:
            body[i] = (LABEL_BANK, label, [])
    # a body its home generator brought back (2003[03] on its line, the generator left home) is disabled where the
    # template killed it: the next run enables it again (DATA Nocturne m27: 12704813 kills each snake ball after its
    # attack and its generator 2705001-003 respawns it on the next dip; LIVE 23 Nov, killed: one ball per dip only once)
    if any((home[i][0], home[i][1]) == GENERATOR and src in stale for i, _, src, _ in params):
        for i, _, src, _ in params:
            if (body[i][0], body[i][1]) == KILL:
                body[i] = (ENABLE[0], ENABLE[1], [0] + list(body[i][2][1:]))
    params = [p for p in params if body[p[0]][0] != LABEL_BANK]
    calls = [(slot, tuple(0 if k * 4 in stale else renames.get(w, w) for k, w in enumerate(args))) for slot, args in runs]
    return body, params, calls


def travel(events, arena, fighter, origin_events, new_entities):
    """The fighter (planner fighter dict; its "origin" names the home body) now fights on `arena` (a boss body of this
    map). origin_events: the home map's Emevd (or a function entity -> Emevd). new_entities: {home body: its new
    entity here} (from `plan`; the parts are added by `add_parts`). Copies the moved events (new ids, started from the
    arena's caller), adds the bodies' set-up event, moves the bar or adds a death to the fight's end where the fight
    needs it. None for a fighter without bodies here. Returns a report."""
    origin = fighter["origin"] if isinstance(fighter, dict) else fighter
    fight = fight_of(fighter)
    if fight is None:
        return None
    stage = _staged(fight, fighter)
    if stage and stage.get("bar_name") is None and stage.get("origin") is not None and callable(origin_events):
        import boss_events  # here: boss_events imports this module
        stage = dict(stage, bar_name=boss_events.origin_name(origin_events, stage["origin"]))
    if callable(origin_events):
        origin_events = origin_events(origin)
    moved_events = tuple(e for e in fight.moved if not (stage and e in fight.stage_events))
    travelling = tuple(e for e in fight.travel if not (stage and e in fight.stage_extra))
    init_lines = fight.init
    if stage:  # the stage boss has its own HP: no damage link from the boss to it
        init_lines = tuple(line for line in init_lines
                           if not (tuple(line[:2]) == (2004, 33) and tuple(line[2]) == (origin, fight.stage)))
        # and nothing to keep: it waits invincible until the handover lets it go (_stage_lines), so neither a fall nor a
        # hit can kill it before its turn
        init_lines += ((INVINCIBLE[0], INVINCIBLE[1], (fight.stage, 1)),)
    defeat, started = arena_flags(arena)
    caller = _caller(events, arena)
    setup = bool(travelling or init_lines)  # a fight with nothing to set up at load (only events) gets no set-up event
    sizes = [1 + max((slot for slot, _ in fight.runs.get(e, ())), default=0) for e in moved_events]
    sizes += [1] * (len(fight.made) + setup)
    ids = free_event_ids(events, arena, sum(sizes), blocks=sizes if fight.runs else None)
    moved = dict(zip(moved_events, ids))
    made_ids = ids[len(moved_events):len(moved_events) + len(fight.made)]
    renames = {origin: arena, fight.defeat: defeat, fight.started: started}
    if fight.ends_on is not None:  # Ludwig's events end on global 9471: the copies end on this arena's defeat
        renames[fight.ends_on] = defeat
    renames.update({pool: arena for pool in fight.stay})  # the boss holds the HP here
    if fight.hold is not None:
        renames[fight.hold] = started
    renames.update({flag: started for flag in fight.as_started})
    renames.update(new_entities)
    renames.update(moved)
    report = {"arena": arena, "origin": origin, "bodies": dict(new_entities), "events": [],
              "init": ids[-1] if setup else None, "bar": None, "end": 0, "doped": [],
              "stage": stage["key"] if stage else None, "handover": None}
    for event in moved_events:
        home = list(origin_events.instructions(event))
        for warp_event, anchor in fight.warps:  # a warp to an object or region of the home map: to the boss instead
            if warp_event == event:
                for i, (bank, iid, words) in enumerate(home):
                    if (bank, iid) in WARPS and len(words) > 2 and words[2] == anchor:
                        home[i] = (bank, iid, (words[0], 2, origin, BOSS_POINT) + tuple(words[4:]))
        if stage:
            home = _stage_lines(fight, event, home, stage)
        body = rewrite(home, renames, origin, fight.defeat)
        if event in fight.runs:
            body, params, calls = _template_copy(origin_events, event, home, body, fight.runs[event], renames, origin,
                                                 fight.defeat)
        else:
            params, calls = (), [(0, ())]
        if fight.gate:
            gated = gate(body, started)
            if len(gated) > len(body):  # the wait went in before line k: parameters of later lines move down one
                k = next(i for i, (bank, _, _) in enumerate(body) if bank < 1000)
                params = [(i + (i >= k), a, src, n) for i, a, src, n in params]
            body = gated
        events.add_event(moved[event], origin_events.restart(event), body, params=params, source=(event, arena, "helper"))
        for slot, args in calls:
            events.append_instruction(caller, CALL[0], CALL[1], [slot, moved[event]] + (list(args) or [0]))
        report["events"].append((event, moved[event]))
    for event, lines, source in zip(made_ids, fight.made, fight.made_from):
        body = gate(rewrite(list(lines), renames, origin, fight.defeat), started)
        events.add_event(event, 0, body, source=(source, arena, "helper"))
        events.append_instruction(caller, CALL[0], CALL[1], [0, event, 0])
        report["events"].append((None, event))
    if setup:
        init = [(b, d, [renames.get(w, w) for w in words]) for b, d, words in init_lines]
        # a cleared arena: the bodies stay off
        off = [(ENABLE[0], ENABLE[1], [new_entities[e], 0]) for e in travelling]
        init = [(1003, 1, [len(off) + 1, defeat])] + off + [(1000, 4, [0])] + init
        # co-op: the network authority call (2004[28]) the home map makes on a travelling body once the fight starts is
        # made for its new entity too: a body without it is not synced to the guests (vanilla gives every parked second
        # body one; player report 24 Nov: in co-op a stage boss died and the fight never ended)
        authority = [(e, w) for e in travelling for w in (_authority(origin_events, e),) if w is not None]
        if fight.wake or authority:
            init += [(IF_FLAG[0], IF_FLAG[1], [256, started])] + [(AI_SWITCH[0], AI_SWITCH[1], [new_entities[e], 1])
                                                                   for e in fight.wake]
            init += [(AUTHORITY[0], AUTHORITY[1], [new_entities[e], w]) for e, w in authority]
        _new_event(events, caller, ids[-1], init)
    # co-op doping: the home fight's set-up event dopes these bodies (it stays home); here they are doped with the slot
    # body, in this arena's set-up event (STATUS 5 Nov, co-op audit risk 1)
    home_doped = [new_entities[e] for e in travelling if doped(origin_events, e)]
    if home_doped:
        anchor = dope_anchor(events, arena)
        if anchor is None:
            raise ValueError("arena %d: no doping calls to copy" % arena)
        report["doped"] = dope_like(events, anchor, home_doped)
    name = _bar_name(origin_events, fight.bar or origin)
    bar_body = None if stage else fight.bar  # the boss keeps its own bar until it hands over to a stage boss
    for event in events.event_ids():
        params = events.params(event)
        for i, (bank, iid, words) in enumerate(events.instructions(event)):
            if (bank, iid) == BOSS_BAR and len(words) > 3 and words[1] == arena and 4 not in params.get(i, {}):
                if bar_body in new_entities:
                    events.set_word(event, i, 1, new_entities[bar_body])
                if name is not None and 12 not in params.get(i, {}):
                    events.set_word(event, i, 3, name)
                report["bar"] = (event, i, new_entities.get(bar_body, arena), name)
    if fight.end_with is not None:
        if isinstance(fight.handover, tuple) and fight.handover[0] == MADE:
            report["handover"] = made_ids[fight.handover[1]]
        elif fight.handover in moved:
            report["handover"] = moved[fight.handover]
        else:
            raise ValueError("arena %d: %s's second body ends the fight but no handover travels" % (arena, fight.key))
        report["end"] = _or_death(events, defeat, arena, new_entities[fight.end_with], after=report["handover"])
        if not report["end"]:
            raise ValueError("arena %d: no death condition in event %d" % (arena, defeat))
    return report


# ---------------------------------------------------------------- co-op doping

# DATA (every main-map event file, vanilla): a fight's set-up event dopes each of its bodies for co-op once the fight
# starts: 1003[109] jumps by the number of cooperators (labels 1-3), then 2004[8] [body, 7500 (one) / 7501 (two), k]
# for every doped body, a one-frame wait (1001[1]) and 2004[53] [body] for each (LIKELY: HP up to the doped maximum).
# 2004[53] and 1003[109] appear in no other event; no doping event has parameters. The engine leaves a doped chr out of
# its normal-enemy effect 7503 (party_balance.cpp). A copied body is doped only by calls that name its new entity.
DOPE = (2004, 8)
DOPE_FILL = (2004, 53)
DOPE_EFFECTS = frozenset((7500, 7501, 7502))


def _dope_line(bank, iid, words, entity):
    return ((bank, iid) == DOPE and len(words) > 1 and words[0] == entity and words[1] in DOPE_EFFECTS) or \
        ((bank, iid) == DOPE_FILL and tuple(words) == (entity,))


def doped(events, entity):
    """[(event, instruction)] of the doping calls (2004[8] SpEffect 7500-7502) on the entity."""
    return [(e, i) for e in events.event_ids() for i, (b, d, w) in enumerate(events.instructions(e))
            if (b, d) == DOPE and len(w) > 1 and w[0] == entity and w[1] in DOPE_EFFECTS]


def dope_anchor(events, body):
    """The entity whose doping calls a new boss on `body` (or a body travelling with it) takes: the body itself when this
    map dopes it, else its arena's `dope_as` (arenas.py: the Emissary's and the Failures' bodies). None when neither."""
    if doped(events, body):
        return body
    arena = arenas.arena_of(body)
    if arena is not None and arena.dope_as is not None and doped(events, arena.dope_as):
        return arena.dope_as
    if body in arenas.PHASES and doped(events, arena.main):  # a phase body is doped with the main (boss_events._phase_body)
        return arena.main
    return None


def dope_like(events, anchor, entities):
    """Every doping call on `anchor` (2004[8] 7500-7502 and the 2004[53] after it, in each event that dopes it) gets the
    same call for each entity right after it, in the same event: the entities are doped with the anchor, by the same
    count of cooperators, at the same moment. Entities already doped here are left alone. Returns [(event, entity)]."""
    entities = [e for e in entities if e != anchor and not doped(events, e)]
    if not entities:
        return []  # nothing to add: leave the file as it is (a splice of the same line still rewrites the event)
    done = []
    for event in sorted({e for e, _ in doped(events, anchor)}):
        body = events.instructions(event)
        for i in reversed(range(len(body))):
            bank, iid, words = body[i]
            if _dope_line(bank, iid, words, anchor):
                events.splice(event, i, [(bank, iid, list(words))] + [(bank, iid, [e] + list(words[1:])) for e in entities])
        done += [(event, e) for e in entities]
    return done


def dope_path(events, anchor, entities, started):
    """Instructions that dope the entities as the anchor's set-up event dopes the anchor, for the end of an event that
    runs later (bodies that only exist once spawned): the anchor event's path from its wait on the started flag to the
    end of its doping, its conditions, jumps and labels kept, its doping calls made for the entities, every other action
    a label (a label id no jump uses). [] when no event waits on `started` before doping the anchor."""
    for event in sorted({e for e, _ in doped(events, anchor)}):
        body = events.instructions(event)
        wait = next((i for i, (b, d, w) in enumerate(body) if (b, d) == IF_FLAG and tuple(w) == (256, started)), None)
        lines = [i for i, (b, d, w) in enumerate(body) if _dope_line(b, d, w, anchor)]
        if wait is None or not lines or wait > lines[0]:
            continue
        end = lines[-1] + 1
        # other bodies' doping calls, then the jump out and the label it lands on
        while end < len(body) and (body[end][0] in (1000, 1001, LABEL_BANK) or body[end][:2] in (DOPE, DOPE_FILL)):
            end += 1
        used = {iid for bank, iid, _ in body if bank == LABEL_BANK}
        free = next(i for i in reversed(range(10)) if i not in used)
        chunks = []
        for bank, iid, words in body[wait + 1:end]:
            if _dope_line(bank, iid, words, anchor):
                chunks.append([(bank, iid, [e] + list(words[1:])) for e in entities] or [(LABEL_BANK, free, [])])
            elif (bank, iid) in (DOPE, DOPE_FILL) or bank >= 2000:
                chunks.append([(LABEL_BANK, free, [])])
            else:
                chunks.append([(bank, iid, list(words))])
        for k, chunk in enumerate(chunks):  # a skip counts lines: it now skips what its lines became
            bank, iid, words = chunk[0]
            if (bank, iid) in SKIPS and words:
                count = words[0] & 0xff
                grown = sum(len(c) for c in chunks[k + 1:k + 1 + count])
                if grown > 0xff:
                    raise ValueError("skip too long")
                chunk[0] = (bank, iid, [words[0] - count + grown] + words[1:])
        return [line for chunk in chunks for line in chunk]
    return []


def _stage_lines(fight, event, home, stage):
    """A moved event's home lines with a stage boss in the second body: lines only the second phase's boss answers
    become labels, the second body's bars show the stage boss's name, and the stage boss's bar comes up where vanilla
    showed none (stage_bar_after)."""
    home = list(home)
    name = stage.get("bar_name")
    for k, (bank, iid, words) in enumerate(home):
        if (event, bank, iid, tuple(words)) in fight.stage_labels:
            home[k] = (LABEL_BANK, _free_label(home), ())
        elif (bank, iid) == FORCE_ANIMATION and words and words[0] == fight.stage:
            # the home second phase's entry animation (Gascoigne's beast 3030, the Orphan's rise 3031, Ludwig's 7000):
            # on the stage boss's model that id is one of its own moves (DATA: c5070 and c5080 have both 3030 and
            # 3031), so it plays none
            home[k] = (LABEL_BANK, _free_label(home), ())
        elif (bank, iid) == BOSS_BAR and len(words) > 3 and words[1] == fight.stage and name is not None:
            home[k] = (bank, iid, tuple(words[:3]) + (name,) + tuple(words[4:]))
    if fight.stage_bar_after and fight.stage_bar_after[0] == event and name is not None:
        _, bank, iid, words = fight.stage_bar_after
        at = next(k for k, (b, d, w) in enumerate(home) if (b, d) == (bank, iid) and tuple(w) == tuple(words))
        home = insert_line(home, at + 1, (BOSS_BAR[0], BOSS_BAR[1], (1, fight.stage, 0, name)))
    if event == fight.handover:  # the stage boss waited invincible (travel's set-up): it is let go with its gravity
        at = next(k for k, (b, d, w) in enumerate(home) if (b, d) == GRAVITY and tuple(w) == (fight.stage, 1))
        home = insert_line(home, at + 1, (INVINCIBLE[0], INVINCIBLE[1], (fight.stage, 0)))
        # the branch for a handover already done (its own flag on: body 1 off, the set-up event wakes body 2): the same
        first_label = next((k for k, (b, _, _) in enumerate(home) if b == LABEL_BANK), len(home))
        end = next((k for k, (b, d, _) in enumerate(home[:first_label]) if (b, d) == (1000, 4)), None)
        if end is not None:
            home = insert_line(home, end, (INVINCIBLE[0], INVINCIBLE[1], (fight.stage, 0)))
    return home


def _authority(events, entity):
    """The second word of the home map's 2004[28] call on the entity, None when it has none."""
    for event in events.event_ids():
        for bank, iid, words in events.instructions(event):
            if (bank, iid) == AUTHORITY and words[0] == entity:
                return words[1]
    return None


def _free_label(body):
    used = {iid for bank, iid, _ in body if bank == LABEL_BANK}
    return next(i for i in reversed(range(10)) if i not in used)


def _bar_name(events, entity):
    for event in events.event_ids():
        for bank, iid, words in events.instructions(event):
            if (bank, iid) == BOSS_BAR and len(words) > 3 and words[1] == entity:
                return words[3]
    return None


# ---------------------------------------------------------------- map parts

def _pose(msb, entity):
    for e in msb.enemies():
        if e["entity"] == entity:
            at = e["at"]
            return e, struct.unpack_from("<3f", msb.data, at + 0x28), struct.unpack_from("<3f", msb.data, at + 0x34)
    raise KeyError(entity)


def plan(arena, fighter, origin_msb, target_variants, events, taken=()):
    """The new bodies for the fighter going to `arena` (a boss body of this map): [{home, entity, name, model, think,
    npc, talk, offset, turn, home_yaw}], [] for a fighter without bodies. origin_msb: the home map's first variant (bytes or
    Msb); target_variants: every variant of this map (bytes or Msb; entity ids and part names are free in all of
    them); events: this map's Emevd; taken: entities and names already handed out in this map (several travellers)."""
    fight = fight_of(fighter)
    if fight is None or not fight.travel:
        return []
    stage = _staged(fight, fighter)
    origin_msb = origin_msb if isinstance(origin_msb, Msb) else Msb(origin_msb)
    msbs = [m if isinstance(m, Msb) else Msb(m) for m in target_variants]
    _, home_pos, home_rot = _pose(origin_msb, fight.boss)
    named = set(taken)
    for event in events.event_ids():
        for _, _, words in events.instructions(event):
            named.update(words)
    blobs = [bytes(m.data) for m in msbs]
    names = set().union(*(m.part_names() for m in msbs)) | set(taken)
    base = arena // 100 * 100
    out = []
    for home in fight.travel:
        if stage and home in fight.stage_extra:
            continue
        part, pos, rot = _pose(origin_msb, home)
        points = origin_msb.generator_points(part["part"])
        if points:  # its generator places it at one of its spawn points; the copies have none (user 25 Nov: the Coiled
            # Host's three snake balls rose stacked from one spot): the point nearest the boss, so they spread as at home
            pos = min(points, key=lambda p: math.hypot(p[0] - home_pos[0], p[2] - home_pos[2]))
        if stage and home == fight.stage:  # the stage boss stands where the second phase waits, with its own rows
            part = dict(part, model=stage["model"], think=stage["think"], npc=stage["npc"], talk=0)
        entity = next(c for c in range(base + PART_GAP, base + 100)
                      if c not in named and not any(struct.pack("<i", c) in b for b in blobs))
        named.add(entity)
        n = next(k for k in range(10000) if "%s_%04d" % (part["model"], k) not in names)
        name = "%s_%04d" % (part["model"], n)
        names.add(name)
        offset = home_offset(pos, home_pos)
        out.append({"home": home, "entity": entity, "name": name, "model": part["model"], "think": part["think"],
                    "npc": part["npc"], "talk": part["talk"], "own_row": bool(stage) and home == fight.stage, "offset": offset,
                    "turn": tuple(r - h for r, h in zip(rot, home_rot)), "home_yaw": home_rot[1]})
    return out


def home_offset(pos, home_pos):
    """A body's offset from its boss at home, as it is used elsewhere: on the boss when it stood far across its home arena
    (it may be no arena there: Ebrietas's bullet owner stands 31 m from her); never below the boss when it stood within
    FLOOR under him (user 25 Nov, the Coiled Host's puddles invisible in Amygdala's arena: his producers stand 0.39 m
    under his spawn at home, LIVE headless 25 Nov 0.25-0.29 m under the Scorched Beast's floor, and they have no gravity;
    a boss stands on or just above its floor). Parts parked far under or over an arena keep their height."""
    offset = tuple(p - h for p, h in zip(pos, home_pos))
    if math.hypot(offset[0], offset[2]) > NEAR:
        offset = (0.0, offset[1], 0.0)
    if -FLOOR < offset[1] < 0.0:
        offset = (offset[0], 0.0, offset[2])
    return offset


FLOOR = 1.0  # metres: a body this close under its boss stands on the boss's floor at home
NEAR = 15.0  # metres: a travelling body farther than this from its boss at home is placed on the boss (its bullets come
# from the boss's dummy polys, the owner only owns them: DATA 2003[05] [owner, source, dummy poly, bullet])


def _place(pos, rot, body):
    """Where a body goes: its home offset from its boss, turned by the difference of the two bosses' yaw."""
    dx, dy, dz = body["offset"]
    turn = math.radians(rot[1] - body["home_yaw"])
    c, s = math.cos(turn), math.sin(turn)
    where = (pos[0] + dx * c + dz * s, pos[1] + dy, pos[2] - dx * s + dz * c)
    return where, tuple(r + t for r, t in zip(rot, body["turn"]))


def add_parts(msb, arena, bodies, npc_rows=None):
    """Adds the planned bodies to one variant of this map (bytes or Msb), next to the arena's boss part, whose record
    is the template (collision, groups, lighting of this map). Call it after Msb.swap_enemies. npc_rows: {(boss body,
    home entity) or home entity: NPC row} to use instead of the home rows (e.g. a clone carrying the slot's HP for
    `same_hp` bodies); the per-body key wins, so two Orphans in one map each carry their own body's HP.
    Returns the edited bytes when given bytes, the part names added when given an Msb (edited in place)."""
    given = msb
    msb = msb if isinstance(msb, Msb) else Msb(msb)
    target, pos, rot = _pose(msb, arena)
    added = []
    for body in bodies:
        where, facing = _place(pos, rot, body)
        rows = npc_rows or {}
        # a stage boss keeps its own row (its own HP and strength), never the second phase's copy
        npc = body["npc"] if body.get("own_row") else rows.get((arena, body["home"]), rows.get(body["home"], body["npc"]))
        msb.add_enemy(target["part"], body["name"], body["entity"], body["model"], body["think"], npc, where, facing,
                      talk=body.get("talk", 0))
        added.append(body["name"])
    return added if given is msb else msb.bytes()


def apply_stem(placed, events, variants, origin_events, origin_msb, npc_rows=None):
    """One map's whole helper work. placed: {boss body of this map: fighter dict} (the bodies that got a boss);
    events: this map's Emevd (edited in place); variants: {variant: MSB bytes}; origin_events(entity) /
    origin_msb(entity): a boss's home Emevd / first-variant MSB bytes. Returns (variants
    edited, report {"dismissed", "travelled", "bodies": {body: plan}, "scripts": [(model, think, npc)]})."""
    report = {"dismissed": [], "travelled": [], "bodies": {}, "scripts": []}
    for fight in sorted(FIGHTS.values(), key=lambda f: (f.key, f.boss)):
        if fight.boss in placed and placed[fight.boss].get("origin") != fight.boss:
            done = dismiss(events, fight)
            if done is not None:
                report["dismissed"].append(done)
    taken = set()
    out = dict(variants)
    for body in sorted(placed):
        fighter = placed[body]
        if fighter.get("origin") == body or fight_of(fighter) is None:
            continue
        bodies = plan(body, fighter, origin_msb(fighter["origin"]), [out[v] for v in sorted(out)], events, taken)
        taken |= {b["entity"] for b in bodies} | {b["name"] for b in bodies}
        report["bodies"][body] = bodies
        report["travelled"].append(travel(events, body, fighter, origin_events, {b["home"]: b["entity"] for b in bodies}))
        for v in sorted(out):
            out[v] = add_parts(out[v], body, bodies, npc_rows)
        report["scripts"] += script_needs(bodies)
    return out, report


def script_needs(bodies):
    """(model, think, npc) of the added bodies: the target map's AI archive needs their battle and logic scripts
    (feed them to generate.merged_archive with the slot changes)."""
    return sorted({(b["model"], b["think"], b["npc"]) for b in bodies})
