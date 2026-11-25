"""Static proof that every boss fight can still end, for every arena, every mode and the fighters with bodies of their
own (helpers), on event files built from vanilla data the way the generator builds them (helpers.apply_stem, then
generate.patched_events). The checker reads the map's events as the game would run them, without running them:

- templates are resolved run by run (each run's arguments put into the template's parameter slots);
- what an event can reach is followed through its jumps and skips, with the flags that event 0 sets before anything
  else known to be on (the second-attempt start, D9);
- a body is present unless an event the edits added disables it at load and nothing enables it again, or it only
  ever comes from a generator that nothing turns on; an HP pool is alive only while a present body's damage link to it
  is kept (or something kills it);
- the defeat event (id = the arena's defeat flag) must be started from event 0 and every wait of its fight path must be
  satisfiable: a death or HP-0 test needs a present body that can die (not left immortal unless something kills it),
  any other test of a body needs it present; and/or groups are evaluated as the game does.
Also checked per case: no event of the map waits (in its main path) on a body that left or on a parked helper event;
every bar of a body with a new boss shows its boss's name and no bar is shown on a body that left; the arena opens as on
a second attempt (intro flag first in event 0, intro reduced to setting its flag) except the Hunter's Dream; no scene
is left in an event naming a body with a new boss.
Run: python -I -m unittest discover -s tools/randomizer/tests. Reads (never writes) the game at BB_GAME."""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import arenas
from containers import dcx_read
from emevd import LABEL_BANK, SKIPS, Emevd

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")

PLAYER = 10000
CALL = (2000, 0)
ENABLE, KILL, IMMORTAL, INVINCIBLE, LINK = (2004, 5), (2004, 4), (2004, 12), (2004, 15), (2004, 33)
GENERATOR, SET_FLAG, BAR = (2003, 3), (2003, 2), (2003, 11)
DEAD, HP_RATIO = (4, 0), (4, 2)
GROUP = (0, 0)
CONDITION_BANKS = (0, 1, 2, 3, 4)
# DATA (m35, survey 4 Nov): of the Living Failures only 3500851 exists at load; the others only come from their
# generators (13505662 turns a generator on for 3 s when its Failure is dead or idle).
SPAWN_ONLY = {3500852: 3503815, 3500853: 3503816, 3500854: 3503817}


def _signed(byte):
    return byte - 256 if byte >= 128 else byte


def resolved(events):
    """{event: [body]}: a plain event has its one body, a template one body per run with the run's arguments in its
    parameter slots (no runs: no body)."""
    runs = {}
    for event in events.event_ids():
        for bank, iid, words in events.instructions(event):
            if (bank, iid) == CALL and len(words) >= 2 and words[1] in events.index:
                runs.setdefault(words[1], []).append(tuple(words[2:]))
    out = {}
    for event in events.event_ids():
        body = events.instructions(event)
        entries = events.param_entries(event)
        if not entries:
            out[event] = [body]
            continue
        bodies = []
        for args in runs.get(event, ()):
            source = struct.pack("<%di" % len(args), *args)
            new = []
            for i, (bank, iid, words) in enumerate(body):
                raw = bytearray(struct.pack("<%di" % len(words), *words))
                for at_i, at, src, size in entries:
                    if at_i == i and at + size <= len(raw) and src + size <= len(source):
                        raw[at:at + size] = source[src:src + size]
                new.append((bank, iid, struct.unpack("<%di" % (len(raw) // 4), bytes(raw))))
            bodies.append(new)
        out[event] = bodies
    return out


def _label_after(body, label, i):
    for j in range(i + 1, len(body)):
        if body[j][0] == LABEL_BANK and body[j][1] == label:
            return j
    return len(body)


def reachable(body, known, own=None):
    """Indices of the instructions the event can run, its jumps and skips followed; known: {flag: 0/1} (flags whose
    state is certain); a test of any other flag can go either way. own: the event's id (a flag test of type 1 reads
    the event's own flag, LIKELY)."""
    seen, todo = set(), [0]
    while todo:
        i = todo.pop()
        while 0 <= i < len(body) and i not in seen:
            seen.add(i)
            bank, iid, words = body[i]
            packed = words[0] if words else 0
            b0, b1, b2 = packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff
            flag = words[1] if b2 == 0 and len(words) > 1 else own if b2 == 1 else None
            state = known.get(flag)
            if (bank, iid) == (1000, 4):
                break
            if (bank, iid) == (1003, 2):  # end if a flag is in a state
                if state is not None and state == b1:
                    break
                i += 1
            elif (bank, iid) == (1003, 101):  # go to a label if a flag is in a state
                target = _label_after(body, b0, i)
                if state is None:
                    todo.append(target)
                    i += 1
                else:
                    i = target if state == b1 else i + 1
            elif (bank, iid) == (1000, 103):  # go to a label
                i = _label_after(body, b0, i)
            elif (bank, iid) == (1000, 3):  # skip lines
                i += b0 + 1
            elif (bank, iid) == (1003, 1):  # skip lines if a flag is in a state
                if state is None:
                    todo.append(i + b0 + 1)
                    i += 1
                else:
                    i = i + b0 + 1 if state == b1 else i + 1
            elif bank in (1000, 1003) and iid >= 100:  # other conditional jumps to a label
                todo.append(_label_after(body, b0, i))
                i += 1
            elif (bank, iid) in SKIPS:  # other conditional skips
                todo.append(i + b0 + 1)
                i += 1
            else:
                i += 1
    return sorted(seen)


def first_flags(events):
    """Flags event 0 turns on before anything else (D9's intro flags): known to be on."""
    out = {}
    for bank, iid, words in events.instructions(0):
        if (bank, iid) != SET_FLAG:
            break
        out[words[0]] = words[1]
    return out


def is_parked(body, defeat):
    return [tuple(x[:2]) + (tuple(x[2]),) for x in body] == [(3, 0, (256, defeat))]


def is_silent(body):
    return len(body) == 1 and body[0][0] == LABEL_BANK


class World:
    """What the map's events can run, read once. defeat: the fight's defeat flag, off while the fight is on (so every
    branch for a cleared arena is left out)."""

    def __init__(self, events, vanilla, defeat):
        self.events = events
        self.known = dict(first_flags(events))
        self.known[defeat] = 0
        self.bodies = resolved(events)
        self.new = frozenset(e for e in events.event_ids() if e not in vanilla.index)
        self.calls = {w[1] for e in events.event_ids() for b, i, w in events.instructions(e) if (b, i) == CALL and len(w) > 1}
        self.reach = {e: [[(i,) + tuple(body[i]) for i in reachable(body, self.known, e)] for body in bodies]
                      for e, bodies in self.bodies.items()}
        self.at_load_off = set()
        for event in self.new:
            for body in self.bodies[event]:
                for bank, iid, words in body:
                    if bank in CONDITION_BANKS or bank in (1000, 1003, LABEL_BANK):
                        break
                    if (bank, iid) == ENABLE and words[1] == 0:
                        self.at_load_off.add(words[0])


CERTAIN, MAYBE, NEVER = 2, 1, 0


class Fight:
    """One arena's fight in one built map. holders: entities holding a boss (placed bodies, vanilla bodies that kept
    theirs, the bodies that travelled with a boss). What the events do is read only up to the first wait of each
    event that cannot come true, and that is read again until the bodies present stop changing."""

    def __init__(self, world, arena, holders, travellers=()):
        self.w, self.arena = world, arena
        self.defeat = arena.flags[0]
        self.travellers = set(travellers)
        self.own = set(arena.bodies) | set(arena.extras) | ({arena.pool} if arena.pool else set()) | self.travellers
        self.holders = set(holders)
        self.parked = {e for e in world.events.event_ids() if is_parked(world.events.instructions(e), self.defeat)}
        self.present, self.effects, self.dropped = set(self.own), {}, set()
        for _ in range(8):
            self.effects, self.dropped = self._effects()
            present = self._present()
            if present == self.present:
                break
            self.present = present

    def _effects(self):
        """{(bank, id, first word): [(event, index, words, mid)]} of what the events can do (mid: after a test, not at
        the unconditional start of the event), and the bodies disabled in the middle of a fight path."""
        out, dropped = {}, set()
        for event, bodies in self.w.reach.items():
            if event in self.parked:
                continue
            for body in bodies:
                cut = min([i for i, _ in self.walk(body, self._blocked)] + [body[-1][0] + 1 if body else 0])
                mid, off, tested = False, set(), False
                for i, bank, iid, words in body:
                    if i >= cut:
                        break
                    if bank in CONDITION_BANKS or bank in (1000, 1003):
                        mid = True
                    if bank == 4 and len(words) > 1 and words[1] in self.own:
                        tested = True  # a test of a fight body: what follows is the fight's (a handover), not a story state
                    if bank in (2003, 2004) and words:
                        out.setdefault((bank, iid, words[0]), []).append((event, i, tuple(words), mid))
                        if (bank, iid) == ENABLE and len(words) > 1 and tested and event != self.defeat:
                            (off.add if words[1] == 0 else off.discard)(words[0])
                dropped |= off  # disabled on the way and not brought back by the same event (a handover)
        return out, dropped

    def did(self, what, entity, value=None, skip_event=None):
        return any(value is None or words[1] == value
                   for event, _, words, _ in self.effects.get(what + (entity,), ()) if event != skip_event)

    def _present(self):
        out = set()
        for e in self.own:
            if e in self.w.at_load_off and not self.did(ENABLE, e, 1):
                continue
            if e in SPAWN_ONLY and not self.did(GENERATOR, SPAWN_ONLY[e], 1):
                continue
            out.add(e)
        pool = self.arena.pool
        if pool is not None and pool in out:
            linked = any(self.did(LINK, b, pool) for b in out if b != pool)
            if not linked and not self.did(KILL, pool, skip_event=self.defeat):
                out.discard(pool)
        return out

    def _blocked(self, bank, iid, words, killed):
        return self.chance(bank, iid, words, killed) == NEVER

    def death(self, entity, killed=()):
        """How sure the body's death is: NEVER (not present, or immortal and nothing kills it), MAYBE (an event that
        tests a fight body then disables it and does not enable it again, as a handover does), CERTAIN."""
        if entity not in self.own:
            return CERTAIN
        if entity not in self.present:
            return NEVER
        locked = (self.did(IMMORTAL, entity, 1) and not self.did(IMMORTAL, entity, 0)) or \
                 (self.did(INVINCIBLE, entity, 1) and not self.did(INVINCIBLE, entity, 0))
        if locked and entity not in killed and not self.did(KILL, entity, skip_event=self.defeat):
            return NEVER
        if entity in self.dropped:
            return MAYBE
        return CERTAIN

    def chance(self, bank, iid, words, killed):
        """Whether one condition can come true: CERTAIN, MAYBE or NEVER."""
        if bank != 4 or len(words) < 2 or words[1] not in self.own:
            return CERTAIN
        entity = words[1]
        if (bank, iid) == DEAD and len(words) > 2 and words[2] == 1:
            return self.death(entity, killed)
        if (bank, iid) == HP_RATIO and len(words) > 3:
            zero = struct.unpack("<f", struct.pack("<i", words[3]))[0] == 0 and words[2] in (0, 5)
            if zero:
                return self.death(entity, killed)
        return CERTAIN if entity in self.present else NEVER

    def walk(self, body, blocked, sure=None):
        """Main-path waits of one reachable body that cannot come true: [(index, words)]. blocked(bank, iid, words,
        killed) -> True when that condition can never be met; or sure(...) -> CERTAIN / MAYBE / NEVER, and then a main
        wait less than CERTAIN counts. And/or groups: an and-group is as sure as its least sure test, an or-group as its
        surest."""
        groups, killed, out = {}, set(), []
        for i, bank, iid, words in body:
            if (bank, iid) == (1000, 4):  # a branch ends here (a cleared branch's kills are not the fight's)
                killed = set()
            if (bank, iid) == KILL and words:
                killed.add(words[0])
            if bank not in CONDITION_BANKS or not words:
                continue
            group = _signed(words[0] & 0xff)
            if (bank, iid) == GROUP:
                state, target = (words[0] >> 8) & 0xff, _signed((words[0] >> 16) & 0xff)
                values = groups.pop(target, [CERTAIN])
                value = (min(values) if target > 0 else max(values)) if state == 1 else CERTAIN
            elif sure is not None:
                value = sure(bank, iid, words, killed)
            else:
                value = NEVER if blocked(bank, iid, words, killed) else CERTAIN
            if group == 0:
                if value != CERTAIN:
                    out.append((i, tuple(words)))
            else:
                groups.setdefault(group, []).append(value)
        return out

    def problems(self):
        """Why the fight could not end: [] when it can."""
        w, out = self.w, []
        if self.defeat not in w.events.index:
            return ["defeat event %d missing" % self.defeat]
        if self.defeat not in w.calls:
            out.append("defeat event %d never started" % self.defeat)
        for body in w.reach[self.defeat]:
            if not body or is_silent([b[1:] for b in body]):
                out.append("defeat event %d silenced" % self.defeat)
            for i, words in self.walk(body, None, self.chance):
                out.append("defeat %d i%d waits on %r, which may never happen" % (self.defeat, i, words))
        return out

    @staticmethod
    def _releases(bank, iid, words, live, flags):
        if not words:
            return False
        if (bank, iid) in (ENABLE, (2004, 1)) and len(words) > 1:
            return words[0] in live and words[1] == 1
        if (bank, iid) in (IMMORTAL, INVINCIBLE) and len(words) > 1:
            return words[0] in live and words[1] == 0
        if (bank, iid) == BAR and len(words) > 1:
            return words[0] == 1 and words[1] in live
        if (bank, iid) == SET_FLAG and len(words) > 1:
            return words[0] in flags and words[1] == 1
        return False

    def gone(self):
        return self.own - self.present

    def dead_waits(self, parked=None):
        """Events of the map (beside the defeat, parked and silenced ones) whose main path waits on a body that left
        the fight (a positive test: dead, HP, message, effect, target, AI state) or on a parked event, and which also
        act on what is still in the fight (a present body of the arena, or its defeat or started flag): such a wait
        would hold back the fight. Waits of events that only serve bodies that left are harmless (they never run).
        "Acts" means what a fight waits for: after the wait the event turns on a present body, its AI or its bar, takes
        its immortality away, or sets the arena's defeat or started flag. Returns ([(event, index, words)] harmful,
        count of harmless ones)."""
        parked = self.parked if parked is None else parked
        gone = self.gone()
        live = (self.present | self.holders) - gone
        flags = set(self.arena.flags)

        def blocked(bank, iid, words, killed):
            if bank == 4 and len(words) > 1 and words[1] in gone:
                if iid in (2, 14):
                    return True
                return iid in (0, 4, 5, 8, 9) and words[-1] != 0
            if (bank, iid) == (3, 0) and len(words) > 1 and words[1] in parked and (words[0] >> 8) & 0xff == 1:
                return True
            return False
        harmful, harmless = [], 0
        for event, bodies in self.w.reach.items():
            if event == self.defeat or event in parked:
                continue
            for body in bodies:
                if is_silent([b[1:] for b in body]):
                    continue
                waits = self.walk(body, blocked)
                if not waits:
                    continue
                first = min(i for i, _ in waits)
                acts = any(self._releases(bank, iid, words, live, flags) for i, bank, iid, words in body if i > first)
                if acts:
                    harmful += [(event, i, words) for i, words in waits]
                else:
                    harmless += len(waits)
        return harmful, harmless


# ---------------------------------------------------------------------------------------------------- building cases

AMELIA, BSB, CLERIC, LOGARIUS, NURSE, ORPHAN, ONE_REBORN = 2400800, 2300800, 2410800, 2500800, 2600800, 3600800, 2800800
GASCOIGNE, LUDWIG, ORPHAN_TWO = 2410810, 3400800, 3600801
HELPED = (LOGARIUS, NURSE, ORPHAN, ONE_REBORN)
CHALICE_NAME = 509000


class Every:
    def __contains__(self, animation):
        return True


def stem_of(entity):
    return "m%02d_%02d" % (entity // 100000, entity // 10000 % 10)


_SAVED = {}


def setUpModule():
    """These fights read the game alone, while the module-wide tables (arenas.set_phases) hold whatever inputs an earlier
    test module's census read: a base mod's fight (helpers.found_mod_fights, Nocturne v6.1.0's Moon Presence phase two)
    has no body here, so it is left out for this module and comes back afterwards."""
    _SAVED.update(phases=dict(arenas.PHASES), helpers=dict(arenas.FOUND_HELPERS))
    arenas.set_phases(_SAVED["phases"], {e: s for e, s in _SAVED["helpers"].items() if "fight" not in s})


def tearDownModule():
    arenas.set_phases(_SAVED["phases"], _SAVED["helpers"])


class Game:
    _files, _events, _msb, _variants = None, {}, {}, {}

    @classmethod
    def files(cls):
        if cls._files is None:
            from gamedata import GameFiles
            cls._files = GameFiles(GAME)
        return cls._files

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


def fighter(origin):
    out = {"key": "chalice" if origin is None else str(origin), "model": "c0000", "think": 1, "npc": 1,
           "origin": origin, "home": None, "extras": (), "extra_models": ()}
    if origin is None:
        out["bar_name"] = CHALICE_NAME
    return out


def build(stem, placed, modes, stages=None):
    """The map's events as generate builds them. placed: {body: origin entity or None (chalice)}. Returns (patched Emevd, vanilla Emevd, travellers {body: {home entity:
    new entity}}, report). stages: {origin: stage origin}: that fighter's second body holds the stage boss (option
    second_stage; model c0000 like every fighter here)."""
    import helpers
    from generate import patched_events
    fighters = {str(o) if o is not None else "chalice": fighter(o) for o in placed.values()}
    for origin, stage in (stages or {}).items():
        fighters[str(origin)]["stage"] = {k: fighter(stage).get(k) for k in ("key", "model", "think", "npc", "origin", "bar_name")}
    by_body = {e: fighters[str(o) if o is not None else "chalice"] for e, o in placed.items()}
    data = Game.event_data(stem)
    events = Emevd(data)
    _, helper_report = helpers.apply_stem(by_body, events, Game.variants(stem), Game.origin_events, Game.origin_msb)
    patched, _, report = patched_events(events.bytes(), stem, {e: f["key"] for e, f in by_body.items()}, fighters, modes,
                                        {"c0000": Every()}, Game.origin_events, events.sources)
    travellers = {body: {b["home"]: b["entity"] for b in bodies} for body, bodies in helper_report["bodies"].items()}
    report["helper_report"] = helper_report  # read by test_second_bodies
    return Emevd(patched if patched is not None else events.bytes()), Emevd(data), travellers, report


def cases():
    """(name, arena, mode, placed {body: origin}) for every arena but Micolash's, every mode its kind has, a plain boss,
    a chalice boss, each boss with bodies or events of its own (helpers.FIGHTS) and each second phase of the pool on
    every body it may take."""
    import helpers
    plain = [AMELIA, BSB, CLERIC]
    helped = {e: f for e, f in helpers.FIGHTS.items() if arenas.arena_of(e).option is None}  # the game's own fights
    for arena in arenas.ARENAS_OF_GAME:
        if arena.kind == arenas.OUT:
            continue
        modes = {arenas.GROUP: ("normal", "mix"), arenas.ARENA_ONLY: ("normal", "mix")}.get(arena.kind, ("swap",))
        for mode in modes:
            bodies = [arena.main] if mode == "normal" else list(arena.bodies)
            others = [o for o in plain if arenas.arena_of(o) is not arena]
            def fill(first):
                return {e: (first if n == 0 else others[n % len(others)]) for n, e in enumerate(bodies)}
            yield "%s %s plain" % (arena.key, mode), arena, mode, fill(others[0])
            yield "%s %s chalice" % (arena.key, mode), arena, mode, fill(None)
            for origin in sorted(helped):
                if arenas.arena_of(origin) is arena:
                    continue
                for target in bodies:
                    if helped[origin].end_with is not None and not arenas.ends_on_death(target):
                        continue  # the planner never puts such a boss there (planner needs_end)
                    placed = {**fill(others[0]), target: origin}
                    yield "%s %s %d on %d" % (arena.key, mode, origin, target), arena, mode, placed
            # the second phases in the pool (user, 8 Nov): a boss of their own, events by their origin
            for origin in [e for a in arenas.ARENAS_OF_GAME if a.kind == arenas.SOLO and a is not arena for e in a.fighters[1:]]:
                for target in bodies:
                    yield ("%s %s phase %d on %d" % (arena.key, mode, origin, target), arena, mode,
                           {**fill(others[0]), target: origin})


def check_case(arena, placed, events, vanilla, travellers):
    """Every problem of one built arena: [str]."""
    world = World(events, vanilla, arena.flags[0])
    moved = {e for es in travellers.values() for e in es.values()}
    holders = set(placed) | moved | {e for e in arena.bodies if e not in placed}
    fight = Fight(world, arena, holders, moved)
    out = fight.problems()
    harmful, _ = fight.dead_waits()
    for event, i, words in harmful:
        out.append("event %d i%d waits on %r, gone" % (event, i, words))
    gone = fight.gone()
    for (bank, iid, _), done in fight.effects.items():
        for event, i, words, _ in done:
            if (bank, iid) == BAR and words[0] == 1 and words[1] in gone:
                out.append("event %d i%d shows a bar on %d, gone" % (event, i, words[1]))
    return out


def expected_name(origin):
    import boss_events
    return CHALICE_NAME if origin is None else boss_events.origin_name(Game.origin_events, origin)


@needs_game
class FightEnds(unittest.TestCase):
    """The core proof: for every case, the fight can end and nothing waits on what left."""

    def test_vanilla_fights_pass_the_checker(self):
        for arena in arenas.ARENAS_OF_GAME:
            events = Emevd(Game.event_data(arena.stem))
            world = World(events, events, arena.flags[0])
            fight = Fight(world, arena, set(arena.bodies) | set(arena.extras))
            self.assertEqual(fight.problems(), [], arena.key)
            self.assertEqual(fight.gone(), set(), arena.key)

    def test_every_arena_every_mode_can_end(self):
        failures = []
        count = 0
        for name, arena, mode, placed in cases():
            events, vanilla, travellers, report = build(arena.stem, placed, {arena.key: mode})
            problems = check_case(arena, placed, events, vanilla, travellers)
            count += 1
            failures += ["%s: %s" % (name, p) for p in problems]
        self.assertGreater(count, 160)
        self.assertEqual(failures, [])

    def test_with_phases_every_arena_can_end(self):
        failures = []
        for name, arena, mode, placed in cases():
            if "plain" not in name and arena.key not in ("gascoigne", "ludwig", "orphan") and \
                    not any(" %d on " % o in name for o in (GASCOIGNE, LUDWIG, ORPHAN, ORPHAN_TWO)):
                continue
            events, vanilla, travellers, report = build(arena.stem, placed, {arena.key: mode})
            failures += ["%s: %s" % (name, p) for p in check_case(arena, placed, events, vanilla, travellers)]
        self.assertEqual(failures, [])

    def test_bars_starts_and_scenes_per_body(self):
        """Every bar of a body with a new boss shows that boss's name (a pool's bar the main body's boss's); the arena
        opens as on a second attempt unless it is a Hunter's Dream fight; no scene is left in an event naming a body
        with a new boss."""
        import helpers
        failures = []
        for name, arena, mode, placed in cases():
            events, vanilla, travellers, report = build(arena.stem, placed, {arena.key: mode})
            want = {e: expected_name(o) for e, o in placed.items()}
            for body, homes in travellers.items():  # a bar that travelled onto a body of the boss (Orphan's phase two)
                bar = helpers.FIGHTS[placed[body]].bar
                if bar in homes:
                    want[homes[bar]] = want[body]
            if arena.pool is not None and arena.main in placed and mode != "mix":
                want[arena.pool] = want[arena.main]
            for e in events.event_ids():
                params = events.params(e)
                for i, (bank, iid, words) in enumerate(events.instructions(e)):
                    if (bank, iid) == BAR and words[0] == 1 and words[1] in want and 12 not in params.get(i, {}):
                        if words[3] != want[words[1]]:
                            failures.append("%s: bar %d i%d on %d shows %d, not %d" % (name, e, i, words[1], words[3],
                                                                                    want[words[1]]))
                    if bank == 2002 and any(b in w for b in placed for _, _, w in events.instructions(e)):
                        failures.append("%s: scene left in %d i%d" % (name, e, i))
            flags = first_flags(events)
            if arena.story_start or arena.intro is None:
                if arena.intro in flags:
                    failures.append("%s: a story fight got a second-attempt start" % name)
                continue
            if flags.get(arena.intro) != 1:
                failures.append("%s: intro flag %d not set first in event 0" % (name, arena.intro))
            intro = events.instructions(arena.intro)
            if intro[-1] != (2003, 2, (arena.intro, 1)) or any(b == 2002 for b, _, _ in intro):
                failures.append("%s: intro %d not reduced to its flag" % (name, arena.intro))
        self.assertEqual(failures, [])


def doping_problems(name, placed, events, vanilla, travellers):
    """Co-op doping (STATUS 5 Nov, co-op audit risks 1 and 3): every body that holds a boss is doped for one and for two
    cooperators (2004[8] SpEffect 7500 and 7501, each with its 2004[53]), and so is every body that travelled with its
    boss whose home fight dopes it, by the same calls in the same event as what its slot body was doped with before the
    arena edits (helpers.dope_anchor on vanilla); a travelling body its home fight does not dope (the Orphan's bullet
    dummy) is not. [str]."""
    import helpers
    calls = {}
    for e in events.event_ids():
        for bank, iid, words in events.instructions(e):
            if (bank, iid) == helpers.DOPE and words[1] in helpers.DOPE_EFFECTS:
                calls.setdefault(words[0], []).append((e, str(words[1])))
            if (bank, iid) == helpers.DOPE_FILL:
                calls.setdefault(words[0], []).append((e, "fill"))
    out = []
    want = {body: ["7500", "7501"] for body in placed}
    for body, homes in travellers.items():
        home_events = Game.origin_events(placed[body])
        anchor = helpers.dope_anchor(vanilla, body)
        for home, new in homes.items():
            want[new] = ["7500", "7501"] if helpers.doped(home_events, home) else []
            if want[new] and sorted(calls.get(new, ())) != sorted(calls.get(anchor, ())):
                out.append("%s: %d (home %d) doped %r, its slot's anchor %d %r" % (
                    name, new, home, calls.get(new), anchor, calls.get(anchor)))
    for entity, effects in want.items():
        got = sorted({x for _, x in calls.get(entity, ()) if x != "fill"})
        fills = sum(1 for _, x in calls.get(entity, ()) if x == "fill")
        if got != effects or fills != len(effects):
            out.append("%s: %d doped %r with %d fills, want %r" % (name, entity, got, fills, effects))
    return out


@needs_game
class Doping(unittest.TestCase):
    def test_every_boss_body_and_doped_helper_is_doped(self):
        failures = []
        for name, arena, mode, placed in cases():
            events, vanilla, travellers, report = build(arena.stem, placed, {arena.key: mode})
            failures += doping_problems(name, placed, events, vanilla, travellers)
        self.assertEqual(failures, [])

    def test_a_helper_left_undoped_is_caught(self):
        import helpers
        real = helpers.dope_like
        try:
            helpers.dope_like = lambda events, anchor, entities: []
            placed = {AMELIA: ORPHAN}
            events, vanilla, travellers, report = build("m24_00", placed, {})
        finally:
            helpers.dope_like = real
        problems = doping_problems("orphan on amelia", placed, events, vanilla, travellers)
        p2 = travellers[AMELIA][3600801]
        self.assertTrue(problems)
        for problem in problems:  # phase two only: the bullet dummy is not doped at home either
            self.assertIn(": %d " % p2, problem)

    def test_orphan_phase_two_is_doped_with_the_slot_and_the_dummy_is_not(self):
        placed = {AMELIA: ORPHAN}
        events, vanilla, travellers, report = build("m24_00", placed, {})
        p2, dummy = travellers[AMELIA][3600801], travellers[AMELIA][3600803]
        body, home = events.instructions(12404802), vanilla.instructions(12404802)
        self.assertEqual(len(body), len(home) + 4)
        for effect in (7500, 7501):
            k = body.index((2004, 8, (AMELIA, effect, 1)))
            self.assertEqual(body[k + 1], (2004, 8, (p2, effect, 1)))
        fills = [k for k, x in enumerate(body) if x == (2004, 53, (AMELIA,))]
        self.assertEqual([body[k + 1] for k in fills], [(2004, 53, (p2,))] * 2)
        self.assertFalse([x for e in events.event_ids() for x in events.instructions(e) if dummy in x[2] and x[:2] in ((2004, 8), (2004, 53))])


@needs_game
class Micolash(unittest.TestCase):
    def test_micolash_is_untouched_whatever_takes_the_wet_nurse(self):
        """User rule: Micolash (2600850) stays; his arena gets no boss and his events stay vanilla."""
        import planner
        self.assertNotIn(2600850, arenas.fighter_entities())
        self.assertFalse([u for a, _, us in planner.arena_units({}, {2600850: ("m26_00", {})}) for u in us])
        for origin in (AMELIA, ORPHAN, ONE_REBORN, LOGARIUS, None):
            events, vanilla, _, _ = build("m26_00", {NURSE: origin}, {})
            for event in vanilla.event_ids():
                if event and any(x == 2600850 or 12601850 <= x <= 12601859 for _, _, w in vanilla.instructions(event) for x in w):
                    self.assertEqual(events.instructions(event), vanilla.instructions(event), (origin, event))


@needs_game
class CheckerCatches(unittest.TestCase):
    """The checker itself: it finds a fight that cannot end."""

    def test_a_disabled_defeat_body_is_caught(self):
        arena = arenas.BY_KEY["amelia"]
        placed = {AMELIA: BSB}
        events, vanilla, travellers, _ = build(arena.stem, placed, {})
        self.assertEqual(check_case(arena, placed, events, vanilla, travellers), [])
        events.add_event(12404899, 0, [(2004, 12, [AMELIA, 1])])
        events.append_instruction(0, 2000, 0, [0, 12404899, 0])
        self.assertTrue(check_case(arena, placed, Emevd(events.bytes()), vanilla, travellers))

    def test_a_removed_pool_link_is_caught(self):
        arena = arenas.BY_KEY["failures"]
        placed = {3500851: AMELIA}
        events, vanilla, travellers, _ = build(arena.stem, placed, {"failures": "normal"})
        self.assertEqual(check_case(arena, placed, events, vanilla, travellers), [])
        i = [k for k, (b, d, w) in enumerate(events.instructions(13504852)) if (b, d, tuple(w)) == (2004, 33, (3500851, 3500850))][0]
        events.neutralize(13504852, i)
        self.assertTrue(check_case(arena, placed, Emevd(events.bytes()), vanilla, travellers))

    def test_an_orphan_whose_phase_two_does_not_end_the_fight_is_caught(self):
        import helpers
        arena = arenas.BY_KEY["amelia"]
        placed = {AMELIA: ORPHAN}
        events, vanilla, travellers, _ = build(arena.stem, placed, {})
        self.assertEqual(check_case(arena, placed, events, vanilla, travellers), [])
        fight = helpers.FIGHTS[ORPHAN]
        keep, fight.end_with = fight.end_with, None
        try:
            events, vanilla, travellers, _ = build(arena.stem, placed, {})
        finally:
            fight.end_with = keep
        problems = check_case(arena, placed, events, vanilla, travellers)
        self.assertEqual(len(problems), 1)
        self.assertIn("12401800", problems[0])  # phase one leaves at the handover: its death may never come

    def test_failures_never_spawned_are_caught(self):
        arena = arenas.BY_KEY["failures"]
        placed = {e: AMELIA for e in arena.bodies}
        events, vanilla, travellers, _ = build(arena.stem, placed, {"failures": "mix"})
        self.assertEqual(check_case(arena, placed, events, vanilla, travellers), [])
        spawn = [e for e in events.event_ids() if e not in vanilla.index
                 and (2003, 3, (3503815, 1)) in [(b, d, tuple(w)) for b, d, w in events.instructions(e)]]
        for i, (b, d, w) in enumerate(events.instructions(spawn[0])):
            if (b, d) == (2003, 3) and w[1] == 1:
                events.neutralize(spawn[0], i)
        self.assertTrue(check_case(arena, placed, Emevd(events.bytes()), vanilla, travellers))

    def test_a_silenced_defeat_event_is_caught(self):
        arena = arenas.BY_KEY["shadows"]
        placed = {arena.main: AMELIA}
        events, vanilla, travellers, _ = build(arena.stem, placed, {"shadows": "normal"})
        events.silence(12701800)
        self.assertTrue(check_case(arena, placed, Emevd(events.bytes()), vanilla, travellers))


if __name__ == "__main__":
    unittest.main()
