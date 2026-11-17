"""Connected maps (neighbours.py) and the planner rule that keeps their replacement models apart (user, 4 Nov 2026: the
next map holds fewer enemies of the last). Run from tools/randomizer: python -m unittest discover -s tests.
Reads (never writes) the game at BB_GAME."""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import neighbours  # noqa: E402
import planner  # noqa: E402
from gamedata import STEMS  # noqa: E402

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")


class Table(unittest.TestCase):
    def test_pairs_are_main_maps_sorted_and_hold_every_loaded_together_pair(self):
        pairs = neighbours.connected_pairs()
        self.assertEqual(pairs, sorted(set(pairs)))
        for a, b in pairs:
            self.assertLess(a, b)
            self.assertIn(a, STEMS)
            self.assertIn(b, STEMS)
        for group in neighbours.LOADED_TOGETHER:
            for a in group:
                for b in group:
                    if a < b:
                        self.assertIn((a, b), pairs)
        links = neighbours.connections()
        self.assertEqual(sum(len(v) for v in links.values()), 2 * len(pairs))
        self.assertNotIn("m21_00", links)  # the Hunter's Dream is a hub: every lamp leads there
        self.assertEqual(len(links["m24_00"]), 8)  # Cathedral Ward: the hub of the walked maps


def _shared(result, pairs):
    boss_parts = {(stem, part) for stem, part in result["bosses"].values()}
    new = {stem: {c[0] for part, c in ch.items() if (stem, part) not in boss_parts}
           for stem, ch in result["changes"].items()}
    return {p: len(new[p[0]] & new[p[1]]) for p in pairs}


@needs_game
class OnGameFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import difficult
        from gamedata import Census, GameFiles
        cls.files = GameFiles(GAME)
        cls.census = Census(cls.files)
        cls.groups = neighbours.load_groups()
        cls.limits = neighbours.group_limits(cls.census, cls.groups)
        cls.hard = difficult.candidates(cls.census)

    def plan(self, seed, options, connected=None):
        c = self.census
        return planner.plan(c.slots, c.candidates, c.sizes, self.groups, self.limits, seed, options, c.animations,
                            self.hard, connected=connected)

    def test_walked_pairs_are_the_connect_collisions_of_the_map_files(self):
        from containers import dcx_read
        from gamedata import map_variants
        from msb import PARTS, Msb, _i32, _i64
        found = set()
        for stem in STEMS:
            for variant in map_variants(self.files, stem):
                msb = Msb(dcx_read(self.files.read("map/mapstudio/%s.msb.dcx" % variant))[0])
                for at in msb._entries(PARTS):
                    if _i32(msb.data, at + 0x14) != 11:  # connect collision
                        continue
                    area, block = msb.data[at + _i64(msb.data, at + 0xb8) + 4:][:2]
                    other = [s for s in STEMS if int(s[1:3]) == area and (block == 255 or int(s[4:6]) == block)]
                    self.assertLessEqual(len(other), 1)
                    if other and other[0] != stem:
                        found.add(tuple(sorted((stem, other[0]))))
        self.assertEqual(found, set(neighbours.WALKED))

    def test_warped_pairs_are_the_fixed_lamp_warps_of_the_map_events(self):
        from gamedata import _load_events
        found = set()
        for stem in STEMS:
            for instructions, params in _load_events(self.files, stem + "_00_00").values():
                for i, (bank, iid, words) in enumerate(instructions):
                    if (bank, iid) == (2003, 49) and i not in params:  # warp to a lamp, not fed by a parameter
                        lamp = words[0]
                        other = "m%02d_%02d" % (lamp // 100000, lamp // 10000 % 10)
                        if other in STEMS and other != stem and other != "m21_00":
                            found.add(tuple(sorted((stem, other))))
                    self.assertNotEqual((bank, iid), (2003, 14))  # no map move request in the main maps
        self.assertEqual(found, set(neighbours.WARPED))

    def test_connected_maps_share_fewer_models_and_nothing_stays_more(self):
        pairs = neighbours.connected_pairs()
        for options in ({"headroom": 150}, {"headroom": 100, "difficult_enemies": 1}):
            totals = {}
            for label, connected in (("on", None), ("off", {})):
                shared = {p: 0 for p in pairs}
                no_room = no_animation = hard = 0
                for seed in range(1, 6):
                    result = self.plan(seed, options, connected)
                    c = self.census
                    self.assertEqual(planner.check(result, c.slots, c.candidates, c.sizes, self.groups, self.limits,
                                                   c.animations), [])
                    for p, n in _shared(result, pairs).items():
                        shared[p] += n
                    no_room += result["report"]["stayed_no_room"]
                    no_animation += result["report"]["stayed_no_animation"]
                    hard += result["report"].get("difficult", 0)
                totals[label] = (shared, no_room, no_animation, hard)
            on, off = totals["on"], totals["off"]
            # with the NPC and arena slots pinned (5 Nov) seeds 1-20 give 0.695 of the shared count at h150. The difficult
            # pool ignores neighbouring maps (user, 7 Nov: its order is the seed's shuffle alone), so no bound there.
            if not options.get("difficult_enemies"):
                self.assertLess(sum(on[0].values()), 0.85 * sum(off[0].values()), options)
            self.assertLessEqual(on[1], off[1], options)
            self.assertLessEqual(on[2], off[2], options)
            self.assertGreaterEqual(on[3], off[3], options)
            if "difficult_enemies" not in options:
                # Byrgenwerth's lamp warp to the Nightmare Frontier: planned in a row, they shared most models
                self.assertLess(on[0][("m32_00", "m33_00")] * 4, off[0][("m32_00", "m33_00")])

    def test_forced_animations_never_leave_a_slot_for_want_of_a_model_that_fits(self):
        """The fill's repair: of R2-13WGN's options, only slots whose animations no pool model has stay vanilla."""
        from seedcode import decode
        seed, options = decode("R2-0ZWGN-JA9NZ-02M4E")
        result = self.plan(seed, options)
        c = self.census
        anim = c.animations
        pool = [m for m in c.candidates if c.sizes.get(m, 0) > 0]
        hopeless = 0
        for stem in STEMS:
            for slot in c.slots[stem]:
                if slot["kept"] is None and slot.get("needs"):
                    if not any(all(a in anim.get(m, ()) for a in slot["needs"]) for m in pool):
                        hopeless += 1
        self.assertLessEqual(result["report"]["stayed_no_animation"], hopeless)

    def test_free_poses_lets_any_model_take_a_posed_slot(self):
        """Option free_poses (user 6 Nov): a slot's poses are no need, so its draw is wider; its other forced animations
        still are (the plan's own check), and no more slots stay vanilla than without it."""
        c = self.census
        loose = {stem: [planner.posed_slot(s) for s in v] for stem, v in c.slots.items()}
        posed = {(stem, s["part"]) for stem in STEMS for s in c.slots[stem] if s["kept"] is None and s.get("poses")}
        self.assertGreater(len(posed), 300)
        self.assertTrue(all(set(s["poses"]) <= set(s["needs"]) for v in c.slots.values() for s in v))
        for seed in (1, 7):
            off = self.plan(seed, {})
            on = planner.plan(loose, c.candidates, c.sizes, self.groups, self.limits, seed, {"free_poses": 1},
                              c.animations, self.hard)
            self.assertEqual(planner.check(on, loose, c.candidates, c.sizes, self.groups, self.limits, c.animations), [])
            kinds = lambda r: {ch[0] for stem, v in r["changes"].items() for part, ch in v.items() if (stem, part) in posed}
            self.assertGreater(len(kinds(on)), len(kinds(off)))
            self.assertLessEqual(on["report"]["stayed_no_animation"], off["report"]["stayed_no_animation"])

    def test_away_from_home_draws_the_maps_own_kinds_only_where_nothing_else_serves(self):
        """Option away_from_home (user, 6 Nov): a replaced enemy is not one of its map's vanilla kinds, where another fits;
        no more slots stay vanilla than without it."""
        c = self.census

        def at_home(result):
            boss_parts = {(stem, part) for stem, part in result["bosses"].values()}
            needs = {(stem, s["part"]): s.get("needs") for stem in STEMS for s in c.slots[stem]}
            return [needs[stem, part] for stem, ch in result["changes"].items() for part, change in ch.items()
                    if (stem, part) not in boss_parts and change[0] in {s["model"] for s in c.slots[stem]}]

        for seed in (1, 7):
            off, on = self.plan(seed, {}), self.plan(seed, {"away_from_home": 1})
            self.assertNotIn("home_kinds", off["report"])
            home = at_home(on)
            self.assertEqual(len(home), on["report"]["home_kinds"])
            self.assertLess(len(home) * 5, len(at_home(off)))
            self.assertTrue(all(home), "only slots with forced animations draw their map's own kinds")
            for key in ("stayed_no_room", "stayed_no_animation"):
                self.assertLessEqual(on["report"][key], off["report"][key], key)

    def test_same_inputs_same_plan(self):
        a = self.plan(7, {"headroom": 200})
        b = self.plan(7, {"headroom": 200})
        self.assertEqual(a["changes"], b["changes"])


if __name__ == "__main__":
    unittest.main()
