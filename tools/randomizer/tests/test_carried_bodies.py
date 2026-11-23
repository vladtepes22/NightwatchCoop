"""Bodies a boss brings into another arena (helpers.py: second bodies, second stages, summoned helpers) wait held until
their fight hands over to them, and only then can they end it (bug 8 Nov, zip 030a, world R2-5CX5W-...-8A3: Ludwig in
Moon Presence's slot with the Coiled Host as his second stage; the copied handover ran at the first Dream load, warped
the Coiled Host onto the disabled Moon Presence slot, he fell and died, and his death won Moon Presence's fight).

Built as generate builds them (planner.plan, helpers.apply_stem, generate.patched_events) over many seeds with boss swaps,
second stages, chalice bosses and (on Nocturne v2.0.0+) the Coiled Host, then checked in the target map's final events
and MSB:
- held at load: disabled, or AI off and gravity off; a second stage also invincible;
- every line that lets it go (enabled, AI on, gravity on, invincible off) runs only after a wait on the arena's started
  flag in the same event (the copied handovers, summons and clones wait for the fight);
- placed in the target arena: within a few metres of the arena body across, at most a vanilla park's depth (104 m) under
  or above it;
- the arena's defeat event tests only its own bodies, and a carried body's death only together with its handover's flag;
- no added event takes an id that is the own flag of a template run in a slot above 0 (event id + slot): in that world
  the copied handover mark had 12104861, the flag of Moon Presence's part template 12104860 run in slot 1, which the
  generator had silenced, so it ended at load.
Run: python -I -m unittest discover -s tools/randomizer/tests. Reads (never writes) the game at BB_GAME and Nocturne at
BB_NOCTURNE (without Nocturne: the game alone, the Coiled Host option then does nothing)."""
import math
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import arenas
import helpers
import planner
from containers import dcx_read
from emevd import Emevd
from msb import Msb

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
# the newest installed Nocturne (v2.0.0+ has the Coiled Host); a fixed v2.0.0 path silently dropped Nocturne once
# v3.0.0 replaced it
from noc import NOCTURNE  # the newest installed Nocturne (BB_NOCTURNE overrides)
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")
# the user's world was generated on Nocturne: without it the same code draws another world
needs_nocturne = unittest.skipUnless(NOCTURNE and os.path.isdir(NOCTURNE), "no Nocturne v2.0.0+")
SEEDS = range(1, 41)
OPTIONS = {"boss_swap": 1, "second_stage": 1, "coiled_host": 1, "chalice_bosses": 1}
# metres from the arena body: across (the helpers' home offsets are small; the parks are straight under or over) and
# up or down (vanilla's deepest park is Ludwig's second body, 104 under him; the Wet Nurse's HP pool stays home)
ACROSS, DEPTH = 15.0, 105.0
ENABLE, AI, GRAVITY, INVINCIBLE = (2004, 5), (2004, 1), (2004, 10), (2004, 15)
DEAD, HP_RATIO, IF_FLAG, GROUP = (4, 0), (4, 2), (3, 0), (0, 0)
CALL = (2000, 0)
SLOT_FLAGS = helpers.slot_flags  # the check's own reading, whatever a test patches into helpers
# The user's world (STATUS 8 Nov ~18:50): Ludwig on Moon Presence's slot, the Coiled Host his second stage
USER_CODE = "R2-5CX5W-F2QNZ-02M5R-5301H-M08V0-4E027-813W0-J8096-04KG2-A8158-0JP09-C04PG-2BK45-W1300-8A3"


def stem_of(entity):
    return "m%02d_%02d" % (entity // 100000, entity // 10000 % 10)


class Inputs:
    """The game (with Nocturne when present), read once."""
    _files = _census = None
    _events, _variants = {}, {}

    @classmethod
    def files(cls):
        if cls._files is None:
            from gamedata import GameFiles
            cls._files = GameFiles(GAME, [NOCTURNE] if os.path.isdir(NOCTURNE) else [])
        return cls._files

    @classmethod
    def census(cls):
        if cls._census is None:
            from gamedata import Census
            cls._census = Census(cls.files())
        arenas.set_phases(cls._census.phases, cls._census.phase_helpers)
        return cls._census

    @classmethod
    def event_data(cls, stem):
        if stem not in cls._events:
            cls._events[stem] = dcx_read(cls.files().read("event/%s_00_00.emevd.dcx" % stem))[0]
        return cls._events[stem]

    @classmethod
    def origin_events(cls, entity):
        return Emevd(cls.event_data(stem_of(entity)))

    @classmethod
    def variants(cls, stem):
        if stem not in cls._variants:
            from gamedata import map_variants
            cls._variants[stem] = {v: dcx_read(cls.files().read("map/mapstudio/%s.msb.dcx" % v))[0]
                                   for v in map_variants(cls.files(), stem)}
        return cls._variants[stem]

    @classmethod
    def origin_msb(cls, entity):
        variants = cls.variants(stem_of(entity))
        return variants[sorted(variants)[0]]


def plan(seed, options):
    """The world's plan as generate makes it (generate.world_plan). Options that add to the census (chalice enemies)
    get a census of their own."""
    from generate import world_plan
    from seedcode import normalize
    census = Inputs.census()
    if normalize(options)["chalice_enemies"]:
        from gamedata import Census
        census = Census(Inputs.files())
        arenas.set_phases(census.phases, census.phase_helpers)
    return world_plan(Inputs.files(), census, seed, options)[0]


def build(stem, result):
    """(final Emevd, {variant: Msb}, helpers report) of one map of the plan, as generate builds its events and parts."""
    from generate import patched_events
    fighters = result["fighters"]
    placed = {e: fighters[k] for e, k in result["boss_fighters"].items() if stem_of(e) == stem}
    events = Emevd(Inputs.event_data(stem))
    variants, report = helpers.apply_stem(placed, events, Inputs.variants(stem), Inputs.origin_events,
                                          Inputs.origin_msb)
    models = {b["entity"]: b["model"] for bodies in report["bodies"].values() for b in bodies}
    patched, _, done = patched_events(events.bytes(), stem, {e: f["key"] for e, f in placed.items()}, fighters,
                                   result.get("arena_modes", {}), Inputs.census().animations, Inputs.origin_events,
                                   events.sources, models)
    final = Emevd(patched if patched is not None else events.bytes())
    # host-only copies trade ids with emptied listed ones last (hostonly.cover): the report's ids follow
    for copy, listed, *_ in done.get("host_only", {}).get("covered", ()):
        from generate import _swapped
        report = _swapped(report, copy, listed)
    return final, {v: Msb(d) for v, d in variants.items()}, report


def position(msb, entity):
    for e in msb.enemies():
        if e["entity"] == entity:
            return struct.unpack_from("<3f", msb.data, e["at"] + 0x28)
    raise KeyError(entity)


def waits_started(body, k, started):
    """Line k of an event body comes after a wait on the started flag (any condition group: 3[00] state on)."""
    return any((b, d) == IF_FLAG and len(w) > 1 and w[1] == started and (w[0] >> 8) & 0xff == 1 for b, d, w in body[:k])


def problems(stem, result):
    """Every broken rule of one map's carried bodies: [str]."""
    events, msbs, report = build(stem, result)
    out = []
    # no added event takes the own flag of a template's run in a slot above 0 (that run's end turns it on)
    added = set(events.event_ids()) - set(Emevd(Inputs.event_data(stem)).event_ids())
    for event in sorted(added & SLOT_FLAGS(events)):
        out.append("added event %d is the own flag of a slotted run" % event)
    carried = {}  # {arena key: {carried body: its handover event}}
    for travelled in report["travelled"]:
        arena_body = travelled["arena"]
        arena = arenas.arena_of(arena_body)
        defeat, started = helpers.arena_flags(arena_body)
        stage = travelled["stage"]
        fight = helpers.FIGHTS[travelled["origin"]]
        name = "%s on %d" % (fight.key, arena_body)
        for home, entity in travelled["bodies"].items():
            label = "%s body %d (%d)" % (name, entity, home)
            lines = [(e, k, b, d, tuple(w)) for e in events.event_ids()
                     for k, (b, d, w) in enumerate(events.instructions(e)) if w and w[0] == entity]
            held = {(b, d, w[1:2]) for _, _, b, d, w in lines if len(w) > 1}
            disabled = (ENABLE[0], ENABLE[1], (0,)) in held
            if not disabled and not {(AI[0], AI[1], (0,)), (GRAVITY[0], GRAVITY[1], (0,))} <= held:
                out.append("%s: not held at load (neither disabled nor AI and gravity off)" % label)
            is_stage = stage is not None and home == fight.stage
            if is_stage and (INVINCIBLE[0], INVINCIBLE[1], (1,)) not in held:
                out.append("%s: a second stage not invincible while it waits" % label)
            for e, k, b, d, w in lines:
                # a body held disabled is let go by enabling it (its AI may be set while it is off, as vanilla does)
                lets_go = len(w) > 1 and w[1] == 1 and (b, d) in ((ENABLE,) if disabled else (ENABLE, AI, GRAVITY))
                lets_go = lets_go or (len(w) > 1 and w[1] == 0 and (b, d) == INVINCIBLE)
                if not lets_go:
                    continue
                body = events.instructions(e)
                # the re-entry branch of a handover already done (its own flag on) runs at load by design
                reentry = e == travelled["handover"] and k < next(
                    (j for j, (bb, _, _) in enumerate(body) if bb == 1014), len(body))
                if not reentry and not waits_started(body, k, started):
                    out.append("%s: event %d line %d lets it go %r before the fight starts" % (label, e, k, (b, d, w)))
            for v, msb in msbs.items():
                x, y, z = position(msb, entity)
                ax, ay, az = position(msb, arena_body)
                if math.hypot(x - ax, z - az) > ACROSS or abs(y - ay) > DEPTH:
                    out.append("%s: %s at %r, arena body at %r" % (label, v, (x, y, z), (ax, ay, az)))
        carried.setdefault(arena.key, {}).update({e: travelled["handover"] for e in travelled["bodies"].values()})
    # the defeat: only this fight's bodies, a carried one only with its handover's flag
    for key, bodies in sorted(carried.items()):
        arena = arenas.BY_KEY[key]
        defeat = arena.flags[0]
        own = set(arena.bodies) | {arena.main} | set(arena.extras) | ({arena.pool} if arena.pool else set())
        own |= {e for e in arenas.PHASES if arenas.arena_of(e) is arena}
        body = events.instructions(defeat)
        for k, (b, d, w) in enumerate(body):
            if (b, d) not in (DEAD, HP_RATIO) or len(w) < 2:
                continue
            if w[1] not in own | set(bodies):
                out.append("%s: defeat %d tests %d, not of this fight" % (key, defeat, w[1]))
            if w[1] in bodies:
                group = w[0] & 0xff
                flag = [x for bb, dd, x in body if (bb, dd) == IF_FLAG and x[0] & 0xff == group]
                if bodies[w[1]] is None or group >= 0x80 or group == 0 or [x[1] for x in flag] != [bodies[w[1]]]:
                    out.append("%s: defeat %d counts %d's death without its handover's flag" % (key, defeat, w[1]))
    return out


@needs_game
class CarriedBodies(unittest.TestCase):
    def tearDown(self):
        arenas.set_phases({})

    def test_every_carried_body_waits_held_in_its_arena_and_ends_the_fight_only_after_its_handover(self):
        failures, seen, stages = [], 0, set()
        for seed in SEEDS:
            result = plan(seed, OPTIONS)
            stems = sorted({stem_of(e) for e, k in result["boss_fighters"].items()
                            if helpers.fight_of(result["fighters"][k]) is not None
                            and result["fighters"][k]["origin"] != e})
            for stem in stems:
                failures += ["seed %d %s: %s" % (seed, stem, p) for p in problems(stem, result)]
                seen += 1
            stages |= set(result["report"].get("stages", {}).values())
        self.assertEqual(failures, [])
        self.assertGreater(seen, len(SEEDS))  # most seeds carry bodies into more than one map
        self.assertGreater(len(stages), 5)    # many kinds of second stage were drawn

    def test_no_added_event_owns_a_slotted_runs_flag_in_any_boss_map(self):
        """Every map with a new boss (boss_events' copies, hostonly's id trades), every arena mixed: no added event id is
        the own flag of a template run in a slot above 0."""
        failures = []
        options = dict(OPTIONS, witch_mix=1, shadows_mix=1, emissary_mix=1, failures_mix=1)
        for seed in range(1, 7):
            result = plan(seed, options)
            for stem in sorted({stem_of(e) for e in result["boss_fighters"]}):
                events = build(stem, result)[0]
                added = set(events.event_ids()) - set(Emevd(Inputs.event_data(stem)).event_ids())
                failures += ["seed %d %s: %d" % (seed, stem, e) for e in sorted(added & SLOT_FLAGS(events))]
        self.assertEqual(failures, [])

    @staticmethod
    def users_world():
        """R2-5CX5W-...-8A3 (zip 030a) as that build drew it: Ludwig on Moon Presence's slot with the Coiled Host as his
        second stage (each boss once per run, 10 Nov, now leaves him alone there: his own second phase and every other
        boss have arenas of their own)."""
        from seedcode import decode
        result = plan(*decode(USER_CODE))
        census = Inputs.census()
        index = planner._index(census.slots)
        pool = {f["key"]: f for f in planner.boss_fighters(census.slots, index, set(), phases=planner.phase_entities(index))}
        result["fighters"]["ludwig"] = helpers.with_stage(pool["ludwig"], pool["shadows_phase"], census.slots)
        return result

    @needs_nocturne
    def test_the_users_world(self):
        """R2-5CX5W-...-8A3 (zip 030a): Ludwig on Moon Presence's slot, a second stage (the Coiled Host on Nocturne)."""
        result = self.users_world()
        self.assertEqual(result["fighters"][result["boss_fighters"][2100810]]["origin"], 3400800)  # Ludwig
        self.assertEqual(problems("m21_00", result), [])

    @needs_nocturne
    def test_the_checks_catch_a_handover_at_load_and_an_unguarded_death(self):
        """The 030a build's shape (no gate, the death or-ed in alone) fails both rules."""
        result = self.users_world()
        gate, or_death, slot_flags = helpers.gate, helpers._or_death, helpers.slot_flags
        try:
            helpers.gate = lambda body, started: list(body)
            helpers._or_death = lambda events, event, arena, other, after=None: or_death(events, event, arena, other)
            found = problems("m21_00", result)
            helpers.slot_flags = lambda events: set()
            ids = problems("m21_00", result)  # the 030a ids (the check reads the flags with SLOT_FLAGS)
        finally:
            helpers.gate, helpers._or_death, helpers.slot_flags = gate, or_death, slot_flags
        self.assertTrue(any("before the fight starts" in p for p in found), found)
        self.assertTrue(any("without its handover's flag" in p for p in found), found)
        self.assertIn("added event 12104861 is the own flag of a slotted run", ids)


if __name__ == "__main__":
    unittest.main()
