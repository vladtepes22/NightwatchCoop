"""Father Gascoigne, Ludwig and the Orphan of Kos: one boss each whose second body travels with it; option second_stage
puts another boss in that second body. The fight-can-end checker (test_fight_ends) runs every arena; here: the fights,
the plan, the handovers.
Run: python -I -m unittest discover -s tools/randomizer/tests. Reads (never writes) the game at BB_GAME."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import arenas
import helpers
import seedcode
from emevd import LABEL_BANK

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")
AMELIA, BSB = 2400800, 2300800
GASCOIGNE, BEAST = 2410810, 2410811
LUDWIG, HOLY_BLADE = 3400800, 3400801
ORPHAN, ORPHAN_TWO, DUMMY = 3600800, 3600801, 3600803


def plain(body):
    return [(b, d, tuple(w)) for b, d, w in body if b != LABEL_BANK]


class Option(unittest.TestCase):
    def test_default_off_and_left_out_of_codes(self):
        self.assertEqual(seedcode.defaults()["second_stage"], 0)
        self.assertEqual(seedcode.LATE_OPTIONS["second_stage"], 0)
        self.assertEqual(seedcode.encode(5, {"boss_swap": 1}), seedcode.encode(5, {"boss_swap": 1, "second_stage": 0}))
        self.assertNotEqual(seedcode.encode(5, {"boss_swap": 1}), seedcode.encode(5, {"boss_swap": 1, "second_stage": 1}))
        self.assertEqual(seedcode.decode(seedcode.encode(5, {"second_stage": 1}))[1]["second_stage"], 1)

    def test_stage_bosses(self):
        self.assertEqual(helpers.stage_bosses(), {GASCOIGNE, LUDWIG, ORPHAN})

    def test_the_arenas(self):
        one = arenas.BY_KEY
        for key, first, second in (("gascoigne", GASCOIGNE, BEAST), ("ludwig", LUDWIG, HOLY_BLADE),
                                   ("orphan", ORPHAN, ORPHAN_TWO)):
            # the second phase is a pool fighter of its own too (user, 8 Nov)
            self.assertEqual((one[key].kind, one[key].bodies, one[key].fighters), (arenas.SOLO, (first,), (first, second)), key)
            self.assertIn(second, one[key].extras)

    def test_helper_fights(self):
        one = helpers.FIGHTS
        self.assertEqual((one[GASCOIGNE].travel, one[GASCOIGNE].end_with, one[GASCOIGNE].same_hp), ((BEAST,), BEAST, (BEAST,)))
        self.assertEqual((one[LUDWIG].travel, one[LUDWIG].end_with, one[LUDWIG].same_hp),
                         ((HOLY_BLADE,), HOLY_BLADE, (HOLY_BLADE,)))
        self.assertEqual(one[ORPHAN].travel, (ORPHAN_TWO, DUMMY))
        self.assertEqual((helpers.ALONE[ORPHAN].travel, helpers.ALONE[ORPHAN].moved), ((), (13604840,)))
        # the handovers are never phases of one body
        import boss_events
        for event in (12414807, 13404824, 13404825, 13604820):
            self.assertIn(event, boss_events.NOT_PHASES)


@needs_game
class Plan(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from gamedata import Census, GameFiles
        from neighbours import group_limits, load_groups
        import planner
        cls.planner = planner
        cls.census = Census(GameFiles(GAME))
        cls.groups = load_groups()
        cls.limits = group_limits(cls.census, cls.groups)
        cls.index = {s["entity"]: (stem, s) for stem, v in cls.census.slots.items() for s in v}

    def plan(self, seed, options):
        c = self.census
        result = self.planner.plan(c.slots, c.candidates, c.sizes, self.groups, self.limits, seed, options, c.animations)
        self.assertEqual(self.planner.check(result, c.slots, c.candidates, c.sizes, self.groups, self.limits, c.animations), [])
        return result

    def test_units_and_fighters(self):
        index = self.index
        fighters = {f["origin"]: f for f in self.planner.boss_fighters(self.census.slots, index, set())}
        for options in ({}, {"second_stage": 1}):
            units = {a.key: us for a, _, us in self.planner.arena_units(seedcode.normalize(options), index)}
            for key, first, second in (("gascoigne", GASCOIGNE, BEAST), ("ludwig", LUDWIG, HOLY_BLADE),
                                       ("orphan", ORPHAN, ORPHAN_TWO)):
                self.assertEqual(units[key], [[first]])
                self.assertIn(second, fighters[first]["extras"])
                self.assertTrue(fighters[first]["needs_end"])  # its second body ends the fight elsewhere
                # a pool fighter of its own (user, 8 Nov): its own rows, no extras, keyed <arena>_2
                self.assertEqual((fighters[second]["key"], fighters[second]["extras"]), (key + "_2", []))
        self.assertEqual(fighters[GASCOIGNE]["extra_models"], ["c2720"])  # the beast's model counts too
        self.assertEqual(fighters[LUDWIG]["extra_models"], [])  # the Holy Blade form is c4510 as well
        self.assertEqual(fighters[GASCOIGNE]["key"], "gascoigne")

    def test_every_body_gets_a_boss(self):
        for options in ({"boss_swap": 1}, {"boss_swap": 1, "second_stage": 1}):
            for seed in range(4):
                result = self.plan(seed, options)
                bodies = {e for a, _, us in self.planner.arena_units(result["options"], self.index) for u in us for e in u}
                self.assertEqual(set(result["bosses"]), bodies, (seed, options))
                self.assertFalse({BEAST, HOLY_BLADE, ORPHAN_TWO} & bodies)
                for e, key in result["boss_fighters"].items():  # a boss whose second body ends the fight needs a body
                    f = result["fighters"][key]  # whose death ends its new fight
                    if f["needs_end"]:
                        self.assertTrue(arenas.ends_on_death(e), (seed, e, key))


@needs_game
class Handovers(unittest.TestCase):
    """Built the way generate builds a map (test_fight_ends.build)."""

    def build(self, stem, placed):
        import test_fight_ends as t
        return t.build(stem, placed, {})

    def test_gascoigne_travels_with_his_beast_form(self):
        events, vanilla, travellers, report = self.build("m24_00", {AMELIA: GASCOIGNE})
        beast = travellers[AMELIA][BEAST]
        moved = copies(report)
        self.assertEqual(sorted(moved), [12414807, 12414809])
        handover = plain(events.instructions(moved[12414807]))
        self.assertIn((2004, 42, (beast, 2, AMELIA, 203, AMELIA)), handover)  # to his dummy poly 203, as at home
        self.assertIn((4, 8, (2, AMELIA, 30, 1)), handover)  # his own AI's message 30, as vanilla
        self.assertIn((2003, 11, (1, beast, 0, 272000)), handover)  # the beast's bar
        self.assertIn((2004, 5, (AMELIA, 0)), handover)
        self.assertIn((2004, 1, (beast, 1)), handover)
        self.assertTrue([w for b, d, w in plain(events.instructions(12401800)) if (b, d) == (4, 0) and w[1:] == (beast, 1)])  # its death ends the fight too
        self.assertFalse([w for e in moved.values() for _, _, ws in events.instructions(e) for w in ws
                          if 12410000 <= w < 12420000 or 2410000 <= w < 2420000])

    def test_ludwig_travels_with_his_holy_blade_form(self):
        events, vanilla, travellers, report = self.build("m24_00", {AMELIA: LUDWIG})
        blade = travellers[AMELIA][HOLY_BLADE]
        moved = copies(report)
        self.assertEqual(sorted(moved), [13404824, 13404825, 13404835, 13404841])
        handover = plain(events.instructions(moved[13404825]))
        self.assertIn((2004, 42, (blade, 2, AMELIA, 203, AMELIA)), handover)  # m34's region 3402806: to the boss instead
        self.assertIn((2003, 11, (1, blade, 0, 451005)), handover)
        self.assertIn((2003, 11, (0, AMELIA, 0, 451000)), handover)
        self.assertFalse([b for b, _, _ in events.instructions(moved[13404825]) if b == 2002])  # the scene is off
        for copy in moved.values():  # 9471 (Ludwig defeated, set by m34's defeat event) became this arena's defeat
            words = [w for _, _, ws in events.instructions(copy) for w in ws]
            self.assertNotIn(9471, words)
            self.assertFalse([w for w in words if 13400000 <= w < 13410000 or 3400000 <= w < 3410000])
        self.assertTrue([w for b, d, w in plain(events.instructions(12401800)) if (b, d) == (4, 0) and w[1:] == (blade, 1)])

    def test_one_boss_arenas_lose_their_second_body(self):
        for stem, first, second, handover, link in (("m24_01", GASCOIGNE, BEAST, (12414807, 12414809), 12414802),
                                                    ("m34_00", LUDWIG, HOLY_BLADE, (13404824, 13404825, 13404835, 13404841), 13404802),
                                                    ("m36_00", ORPHAN, ORPHAN_TWO, (13604820, 13604830, 13604840), 13604802)):
            events, vanilla, travellers, report = self.build(stem, {first: AMELIA})
            defeat = arenas.arena_of(first).flags[0]
            # a listed host-only id the park empties may carry a copy of the new boss's own phase (hostonly.cover)
            reused = {listed for _, listed, *_ in report.get("host_only", {}).get("covered", ())}
            for event in handover:
                if event not in reused:
                    self.assertEqual(plain(events.instructions(event)), [(3, 0, (256, defeat))], (stem, event))  # parked
            self.assertNotIn((2004, 33, (first, second)), plain(events.instructions(link)), stem)
            disables = [e for e in events.event_ids() if e not in vanilla.index
                        and (2004, 5, (second, 0)) in plain(events.instructions(e))]
            self.assertTrue(disables, stem)


@needs_game
class SecondStage(unittest.TestCase):
    """Option second_stage: the travelling second body holds another boss (built as generate builds a map)."""

    def build(self, stem, placed, stages):
        import test_fight_ends as t
        return t.build(stem, placed, {}, stages)

    def test_gascoigne_hands_over_to_the_stage_boss(self):
        events, vanilla, travellers, report = self.build("m24_00", {AMELIA: GASCOIGNE}, {GASCOIGNE: BSB})
        second = travellers[AMELIA][BEAST]
        moved = copies(report)
        self.assertEqual(sorted(moved), [12414807])  # the handover; the beast's music box stays home
        handover = plain(events.instructions(moved[12414807]))
        self.assertIn((2004, 1, (second, 1)), handover)
        bars = [w for b, d, w in handover if (b, d) == (2003, 11) and w[1] == second]
        self.assertEqual(len(bars), 1)
        import test_fight_ends as t
        self.assertEqual(bars[0][3], helpers._bar_name(t.Emevd(t.Game.event_data("m23_00")), BSB))  # the stage's name
        travelled, = report["helper_report"]["travelled"]
        self.assertEqual(travelled["stage"], str(BSB))
        setup = plain(events.instructions(travelled["init"]))
        self.assertFalse([w for b, d, w in setup if (b, d) == (2004, 33)])  # own HP: no damage link
        body, = report["helper_report"]["bodies"][AMELIA]
        self.assertTrue(body["own_row"])

    def test_the_orphans_stage_needs_no_message_and_gets_a_bar(self):
        events, vanilla, travellers, report = self.build("m24_00", {AMELIA: ORPHAN}, {ORPHAN: LUDWIG})
        second = travellers[AMELIA][ORPHAN_TWO]
        self.assertNotIn(DUMMY, travellers[AMELIA])  # its bullet dummy stays home
        moved = copies(report)
        self.assertNotIn(13604830, moved)
        handover = plain(events.instructions(moved[13604820]))
        self.assertNotIn((4, 8, (0, second, 50, 1)), handover)
        at = handover.index((2004, 1, (second, 1)))
        self.assertEqual(handover[at + 1][:2], (2003, 11))
        self.assertEqual(handover[at + 1][2][:3], (1, second, 0))

    def test_without_a_stage_nothing_changes(self):
        events, vanilla, travellers, report = self.build("m24_00", {AMELIA: GASCOIGNE}, {})
        self.assertEqual(sorted(copies(report)), [12414807, 12414809])


def copies(report):
    """{home event: its copy's id in the built file} of the helpers' copies (after the co-op host-only id swaps)."""
    swapped = {c: listed for c, listed, *_ in report.get("host_only", {}).get("covered", ())}
    return {home: swapped.get(new, new) for t in report["helper_report"]["travelled"] for home, new in t["events"]}


if __name__ == "__main__":
    unittest.main()
