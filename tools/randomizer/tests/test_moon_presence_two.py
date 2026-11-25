"""Option moon_presence_two (option 53): Nocturne v6.1.0's Moon Presence phase two (c5420, entity 2100860 in m21_00, parked
under the Moon Presence 2100810) comes along wherever the Moon Presence fights (helpers.MOON_PRESENCE_TWO): held at load,
started with the fight, handed over at half her bar (lines of the defeat event 12101850, host-only by the mod's list), her
death ending the fight. Off, the Moon Presence fights alone away from home (helpers.ALONE). Either way another boss on the
Moon Presence's slot fights without her: her lines at home are dropped and the fight ends on the slot body's death.
Found in the inputs (helpers.found_mod_fights), never assumed: without her (the game alone, Nocturne before v6.1.0) the
option does nothing.
Run: python -I -m unittest discover -s tools/randomizer/tests. Reads the game at BB_GAME and the newest Nocturne (noc.py,
BB_NOCTURNE); the cases that need her skip without a v6.1.0+."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import arenas
import helpers
from noc import NOCTURNE

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
MOON, TWO, KEY, OPTION = 2100810, 2100860, "moon_presence", "moon_presence_two"
SETUP, DEFEAT, GAZE = 12104852, 12101850, 12104871
has_game = os.path.isdir(os.path.join(GAME, "dvdroot_ps4"))
needs_game = unittest.skipUnless(has_game, "no game files")


def _has_her():
    path = os.path.join(NOCTURNE or "", "dvdroot_ps4", "map", "MapStudio", "m21_00_00_00.msb.dcx")
    if not os.path.isfile(path):
        return False
    from containers import dcx_read
    with open(path, "rb") as f:
        data = dcx_read(f.read())[0]
    return TWO.to_bytes(4, "little") in data and "c5420".encode("utf-16-le") in data


HAS_HER = bool(NOCTURNE) and _has_her()
needs_her = unittest.skipUnless(has_game and HAS_HER, "no game files or no Nocturne v6.1.0+")


class Inputs:
    """The game alone and the game with the newest Nocturne, each read once."""
    _cache = {}

    @classmethod
    def census(cls, mods):
        key = tuple(mods)
        if key not in cls._cache:
            from gamedata import Census, GameFiles
            cls._cache[key] = (Census(GameFiles(GAME, list(mods))), GameFiles(GAME, list(mods)))
        c = cls._cache[key][0]
        arenas.set_phases(c.phases, c.found_helpers)  # another test's census may have read other inputs since
        return c

    @classmethod
    def files(cls, mods):
        cls.census(mods)
        return cls._cache[tuple(mods)][1]

    @classmethod
    def plan(cls, mods, seed, options):
        import planner
        from neighbours import group_limits, load_groups
        c = cls.census(mods)
        groups = load_groups()
        limits = group_limits(c, groups)
        result = planner.plan(c.slots, c.candidates, c.sizes, groups, limits, seed, options, c.animations)
        return c, result, planner.check(result, c.slots, c.candidates, c.sizes, groups, limits, c.animations)


_SAVED = {}


def setUpModule():
    """The census here sets the module-wide phase and helper tables (arenas.set_phases) for its inputs: the other test
    modules read theirs once and rely on them, so they come back as they were afterwards."""
    _SAVED.update(phases=dict(arenas.PHASES), helpers=dict(arenas.FOUND_HELPERS))


def tearDownModule():
    arenas.set_phases(_SAVED["phases"], _SAVED["helpers"])


def _lines(ev, event):
    return [(b, d, tuple(w)) for b, d, w in ev.instructions(event)]


class Table(unittest.TestCase):
    def test_the_option_is_53_and_late(self):
        import seedcode
        self.assertEqual(seedcode._BY_ID[53][1:], (OPTION, "int", 0))
        self.assertEqual(seedcode.BOOL_OPTIONS_R2[-1], OPTION)  # appended: older codes keep their bits
        self.assertIn(53, seedcode.EXTRAS)
        self.assertEqual(seedcode.LATE_OPTIONS[OPTION], 0)
        a, b = {"boss_swap": 1}, {"boss_swap": 1, OPTION: 0}
        self.assertEqual(seedcode.options_hash(seedcode.normalize(a)), seedcode.options_hash(seedcode.normalize(b)))
        self.assertEqual(seedcode.encode(7, a), seedcode.encode(7, b))
        self.assertEqual(seedcode.decode(seedcode.encode(7, {"boss_swap": 1, OPTION: 1}))[1][OPTION], 1)

    def test_the_fight(self):
        fight = helpers.MOON_PRESENCE_TWO
        self.assertEqual((fight.key, fight.boss, fight.travel, fight.bar, fight.end_with, fight.disable),
                         (KEY, MOON, (TWO,), TWO, TWO, (TWO,)))
        self.assertEqual((fight.moved, fight.park, fight.handover), ((GAZE,), (GAZE,), (helpers.MADE, 1)))
        self.assertEqual(fight.made_from, (None, DEFEAT))  # the handover copy keeps the mod's host-only rule
        self.assertTrue(fight.gate)  # her events wait for the fight's start
        self.assertEqual(helpers.ALONE[MOON].bar, TWO)  # alone: the name of her home bar, which sits on phase two
        self.assertEqual(helpers.ALONE[MOON].travel, ())

    def test_found_only_with_every_line(self):
        enemies = {"m21_00": {TWO: {"model": "c5420"}}}
        self.assertEqual(helpers.found_mod_fights({"m21_00": {}}, enemies), {})
        self.assertEqual(helpers.found_mod_fights({}, {"m21_00": {TWO: {"model": "c5400"}}}), {})

    def test_a_mod_listed_event_stays_host_only_in_its_copy(self):
        import hostonly
        from emevd import Emevd  # noqa: F401  (the module the copies' sources live in)
        with tempfile.TemporaryDirectory() as mod:
            with open(os.path.join(mod, hostonly.PARTY_FILE), "w", encoding="ascii", newline="\n") as f:
                f.write("# a comment\n12101850  # the handover\n\nnot a number\n")
            listed = hostonly.mod_listed([mod, os.path.join(mod, "missing")])
        self.assertEqual(listed, frozenset((DEFEAT,)))

        class Events:
            sources = {12604869: (DEFEAT, 2600800, "helper")}

            def instructions(self, event):
                return []
        self.assertIsNone(hostonly.rank(Events(), 12604869))
        self.assertEqual(hostonly.rank(Events(), 12604869, listed), 0)


@needs_game
class WithoutHer(unittest.TestCase):
    def test_the_option_does_nothing(self):
        mods = (NOCTURNE,) if NOCTURNE and os.path.isdir(NOCTURNE) and not HAS_HER else ()
        c = Inputs.census(mods)
        self.assertNotIn(TWO, {s["entity"] for slots in c.slots.values() for s in slots})
        self.assertIsNot(helpers.FIGHTS.get(MOON), helpers.MOON_PRESENCE_TWO)
        self.assertEqual(arenas.BY_KEY[KEY].extras, ())
        for seed in (1, 2):
            _, off, bad = Inputs.plan(mods, seed, {"boss_swap": 1})
            self.assertEqual(bad, [])
            _, on, bad = Inputs.plan(mods, seed, {"boss_swap": 1, OPTION: 1})
            self.assertEqual(bad, [])
            self.assertEqual((on["changes"], on["bosses"]), (off["changes"], off["bosses"]))  # the world of it off
            self.assertIn(OPTION, on["report"]["skipped_options"])
            self.assertFalse(on["fighters"].get(KEY, {}).get("extras"))


@needs_her
class WithHer(unittest.TestCase):
    def test_she_is_found_and_kept(self):
        import boss_events
        from containers import dcx_read
        from emevd import Emevd
        c = Inputs.census((NOCTURNE,))
        index = {s["entity"]: s for slots in c.slots.values() for s in slots}
        self.assertEqual((index[TWO]["model"], index[TWO]["kept"], index[TWO]["role"], index[TWO]["arena"]),
                         ("c5420", "boss", "extra", KEY))
        self.assertNotIn("c5420", c.candidates)
        self.assertIs(helpers.FIGHTS[MOON], helpers.MOON_PRESENCE_TWO)
        self.assertEqual(arenas.BY_KEY[KEY].extras, (TWO,))  # her slot is the fight's, never an ordinary enemy's
        events = Emevd(dcx_read(Inputs.files((NOCTURNE,)).read("event/m21_00_00_00.emevd.dcx"))[0])
        # her defeat event tests the Moon Presence's HP and forces her fall: still no phase of hers (DEFEATS)
        self.assertNotIn(DEFEAT, boss_events.phase_events(events, MOON))

    def test_the_plan_both_ways(self):
        away = 0
        for seed in range(1, 7):
            _, on, bad = Inputs.plan((NOCTURNE,), seed, {"boss_swap": 1, OPTION: 1})
            self.assertEqual(bad, [], seed)
            self.assertNotIn("skipped_options", on["report"])
            if KEY in on["fighters"]:  # (the fighters that got an arena)
                f = on["fighters"][KEY]
                self.assertEqual((f["extras"], f["extra_models"], f["needs_end"], f.get("alone")),
                                 ([TWO], ["c5420"], True, None))
                away += 1
            _, off, bad = Inputs.plan((NOCTURNE,), seed, {"boss_swap": 1})
            self.assertEqual(bad, [], seed)
            if KEY in off["fighters"]:
                f = off["fighters"][KEY]
                self.assertEqual((f["extras"], f["extra_models"], f["needs_end"], f["alone"]), ([], [], False, True))
                self.assertIn(KEY, off["report"]["alone"])
        self.assertGreaterEqual(away, 4)

    def test_new_bodies_and_events_of_m21_pass_her_by(self):
        """Her entity and event ids sit in the range helpers hands out for new m21 bodies (PART_GAP): Logarius's sword
        and bullet owner in Gehrman's arena take the next free ones."""
        from containers import dcx_read
        from emevd import Emevd
        from gamedata import map_variants
        files = Inputs.files((NOCTURNE,))
        events = Emevd(dcx_read(files.read("event/m21_00_00_00.emevd.dcx"))[0])
        msb = lambda stem: dcx_read(files.read("map/mapstudio/%s.msb.dcx" % map_variants(files, stem)[0]))[0]
        logarius = {"key": "logarius", "origin": 2500800}
        bodies = helpers.plan(2100800, logarius, msb("m25_00"), [msb("m21_00")], events)
        self.assertEqual(len(bodies), 2)
        self.assertTrue(all(b["entity"] not in (TWO, MOON) and 2100860 < b["entity"] < 2100900 for b in bodies))
        ids = helpers.free_event_ids(events, 2100800, 12)
        self.assertNotIn(GAZE, ids)

    def _world(self, on):
        """A generated world where the Moon Presence fights in another map: (folder, spoiler, arena key, slot)."""
        import generate
        import seedcode
        files, c = Inputs.files((NOCTURNE,)), Inputs.census((NOCTURNE,))
        options = {"boss_swap": 1, OPTION: 1} if on else {"boss_swap": 1}
        for seed in range(1, 30):
            plan = generate.world_plan(files, c, seed, seedcode.normalize(options))[0]
            where = [e for e, k in plan["boss_fighters"].items() if k == KEY and arenas.arena_of(e).stem != "m21_00"]
            if where:
                break
        else:
            self.fail("no seed sends her out of m21")
        out = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, out, True)
        folder, spoiler = generate.generate(GAME, [NOCTURNE], out, seedcode.encode(seed, options), "world")
        arena = arenas.arena_of(where[0])
        self.assertEqual(spoiler["report"]["arenas"][arena.key]["bodies"][str(where[0])], KEY)
        return folder, spoiler, arena, where[0]

    def _check_home(self, home):
        """Another boss on her slot: her lines dropped, the bar back on the slot body, the end on its death, her gaze
        parked, phase two disabled."""
        setup = _lines(home, SETUP)
        self.assertFalse([line for line in setup if TWO in line[2]])
        self.assertTrue([line for line in setup if line[:2] == (2003, 11) and line[2][1] == MOON])
        defeat = _lines(home, DEFEAT)
        self.assertFalse([line for line in defeat if TWO in line[2]])
        self.assertIn((4, 0), [line[:2] for line in defeat if line[2][1:2] == (MOON,)])
        self.assertFalse([line for line in defeat if line[:2] in ((2003, 18), (2004, 42))])  # no fall, no warp
        self.assertEqual(_lines(home, GAZE), [(3, 0, (256, DEFEAT))])
        self.assertTrue(any((b, d, w) == (2004, 5, (TWO, 0)) for e in home.event_ids() for b, d, w in _lines(home, e)))

    def test_a_world_where_she_comes_along(self):
        from containers import dcx_read
        from emevd import Emevd
        from msb import Msb
        folder, spoiler, arena, slot = self._world(True)
        root = os.path.join(folder, "dvdroot_ps4")

        def events(stem):
            with open(os.path.join(root, "event", "%s_00_00.emevd.dcx" % stem), "rb") as f:
                return Emevd(dcx_read(f.read())[0])
        there = events(arena.stem)
        twos = set()
        for name in os.listdir(os.path.join(root, "map", "MapStudio")):
            if name.startswith(arena.stem):
                with open(os.path.join(root, "map", "MapStudio", name), "rb") as f:
                    twos |= {p["entity"] for p in Msb(dcx_read(f.read())[0]).enemies() if p["model"] == "c5420"}
        self.assertEqual(len(twos), 1)  # her part in the new map, in every variant
        two = twos.pop()
        self.assertEqual(two // 100, slot // 100)
        every = [(e, b, d, tuple(w)) for e in there.event_ids() for b, d, w in there.instructions(e)]
        has = lambda b, d, w: any(line[1:] == (b, d, tuple(w)) for line in every)
        self.assertTrue(has(2004, 33, (slot, two)))  # the Moon Presence's damage goes to her (the shared bar)
        self.assertTrue(has(2004, 8, (two, 7635, 1)))  # her HP raised as at home
        self.assertTrue(has(2004, 42, (two, 2, slot, helpers.BOSS_POINT, slot)))  # the handover warp onto the slot body
        self.assertTrue(has(2003, 18, (slot, 2200, 0)) and has(2004, 1, (two, 1)))  # its fall, her AI on
        self.assertTrue(has(2004, 28, (two, 4095)))  # co-op authority
        self.assertTrue([line for line in every if line[1:3] == (2004, 8) and line[3][:2] in ((two, 7500), (two, 7501))])
        self.assertTrue(has(4, 8, (0, two, 10, 1)))  # her Doom Gaze on her message 10
        bars = [line[3] for line in every if line[1:3] == (2003, 11) and line[3][1] in (slot, two)]
        self.assertTrue([b for b in bars if b[1] == two])  # the bar on her ...
        self.assertEqual({b[3] for b in bars if b[1] == two}, {540000})  # ... with her name
        self.assertFalse([b for b in bars if b[1] == slot])  # (the slot body's damage goes to her)
        handover = next(e for e, b, d, w in every if (b, d, w) == (2004, 42, (two, 2, slot, helpers.BOSS_POINT, slot)))
        defeat = _lines(there, arena.flags[0])
        self.assertIn((4, 0), [line[:2] for line in defeat if line[2][1:2] == (two,)])  # her death ends the fight ...
        self.assertTrue([line for line in defeat if line[:2] == (3, 0) and line[2][1] == handover])  # ... once handed over
        # host-only like the home event (the mod's party_host_only.txt): a listed id of the map or the world's list
        host = spoiler["boss_events"][arena.stem]["host_only"]
        listed = [r for r in host.get("covered", []) + host.get("uncovered", []) if DEFEAT in r]
        self.assertEqual(len(listed), 1)
        if host.get("uncovered"):
            with open(os.path.join(folder, "party_host_only.txt"), encoding="ascii") as f:
                self.assertIn(str(handover), f.read().split())
        self._check_home(events("m21_00"))

    def test_a_world_where_she_stays_home(self):
        from containers import dcx_read
        from emevd import Emevd
        from msb import Msb
        folder, spoiler, arena, slot = self._world(False)
        root = os.path.join(folder, "dvdroot_ps4")
        self.assertIn(KEY, spoiler["report"]["alone"])
        for name in os.listdir(os.path.join(root, "map", "MapStudio")):
            if name.startswith(arena.stem):
                with open(os.path.join(root, "map", "MapStudio", name), "rb") as f:
                    self.assertNotIn("c5420", {p["model"] for p in Msb(dcx_read(f.read())[0]).enemies()})
        with open(os.path.join(root, "event", "%s_00_00.emevd.dcx" % arena.stem), "rb") as f:
            there = Emevd(dcx_read(f.read())[0])
        bars = [tuple(w) for e in there.event_ids() for b, d, w in there.instructions(e)
                if (b, d) == (2003, 11) and w[1] == slot]
        self.assertTrue(bars)
        self.assertTrue(all(b[3] == 540000 for b in bars))  # her name, from the bar her home shows on phase two
        with open(os.path.join(root, "event", "m21_00_00_00.emevd.dcx"), "rb") as f:
            self._check_home(Emevd(dcx_read(f.read())[0]))


if __name__ == "__main__":
    unittest.main()
