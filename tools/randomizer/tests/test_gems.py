"""Gem roulette (gems.py, options gem_roulette 44 and cursed_gems 45). Run from tools/randomizer:
python -I -m unittest discover -s tests. Pure tests need nothing; `needs_game` ones read (never write) the game at BB_GAME."""
import os
import sys
import tempfile
import unittest
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import gems  # noqa: E402
import seedcode  # noqa: E402
from seedcode import Stream  # noqa: E402

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")


def fake_found():
    """Every kind of the table with made-up effect ids (kind index * 100 + rank) and name offsets."""
    kinds = {}
    for n, key in enumerate(sorted(gems.KINDS)):
        kinds[key] = {"category": n, "effects": {r: n * 100 + r for r in range(1, 21)},
                      "names": {r: n * 10 for r in range(1, 21)}}
    return {"kinds": kinds, "name_offset": 0}


def score(gem):
    return sum(gems.WEIGHT[gems.KINDS[k][0]] * r for k, r in zip(gem["kinds"], gem["ranks"]))


class Options(unittest.TestCase):
    def test_codes_round_trip_and_default_hash(self):
        for options in ({"gem_roulette": 1}, {"gem_roulette": 1, "cursed_gems": 1}):
            seed, back = seedcode.decode(seedcode.encode(7, options))
            self.assertEqual((seed, back["gem_roulette"], back["cursed_gems"]), (7, 1, options.get("cursed_gems", 0)))
        self.assertEqual(seedcode.options_hash({"gem_roulette": 0, "cursed_gems": 0}), seedcode.options_hash({}))
        self.assertNotEqual(seedcode.options_hash({"gem_roulette": 1}), seedcode.options_hash({}))
        with self.assertRaises(ValueError):
            seedcode.normalize({"cursed_gems": 2})


class Draw(unittest.TestCase):
    def test_every_gem_keeps_the_rules(self):
        """Effect 1 is family A, no exclude group twice, at most one element, ranks in the tier's band and the score in
        its window; uncursed gems carry no curse."""
        found = fake_found()
        for tier, (low, high) in gems.BANDS.items():
            for n in range(150):
                gem = gems.draw(found, Stream(1, "h", "t", tier, n), tier)
                self.assertEqual(gems.KINDS[gem["kinds"][0]][0], "A")
                self.assertEqual(len({k[0] for k in gem["kinds"]}), 3)
                self.assertLessEqual(len({gems.KINDS[k][1] for k in gem["kinds"]} - {None}), 1)
                self.assertTrue(all(low <= r <= high for r in gem["ranks"]), gem)
                self.assertTrue(gems.FLOOR * low <= score(gem) <= gems.CAP * high, gem)
                self.assertFalse(gem["cursed"] or any(gems.KINDS[k][0] == "X" for k in gem["kinds"]))
                self.assertEqual(gem["effects"], [found["kinds"][k]["effects"][r] for k, r in zip(gem["kinds"], gem["ranks"])])

    def test_curses_take_a_side_effect_and_raise_the_others(self):
        found = fake_found()
        cursed = 0
        for n in range(2000):
            tier = 1 + n % 7
            low, high = gems.BANDS[tier]
            plain = gems.draw(found, Stream(2, "h", n), tier)
            gem = gems.draw(found, Stream(2, "h", n), tier, cursed=True)
            if not gem["cursed"]:
                self.assertEqual(gem, plain)  # the same draw up to the curse roll
                continue
            cursed += 1
            bad = [i for i, k in enumerate(gem["kinds"]) if gems.KINDS[k][0] == "X"]
            self.assertEqual(len(bad), 1)
            self.assertIn(bad[0], (1, 2))  # never the effect that names the gem
            self.assertEqual(len({k[0] for k in gem["kinds"]}), 3)
            for i in range(3):
                if i != bad[0]:
                    self.assertEqual(gem["ranks"][i], min(plain["ranks"][i] + gems.CURSE_BOOST, gems.RANK_CAP.get(gem["kinds"][i], 20)))
            self.assertTrue(low <= gem["ranks"][bad[0]] <= high)
            self.assertEqual(gem["name"] // 100000 % 10, 1)  # the cursed name
        self.assertTrue(200 <= cursed <= 400, cursed)  # about 15 %

    def test_an_element_can_be_asked_for(self):
        found = fake_found()
        for element in gems.GUARANTEED:
            for n in range(30):
                gem = gems.draw(found, Stream(3, "h", element, n), 4, element)
                self.assertEqual((gems.KINDS[gem["kinds"][0]][1], gem["element"]), (element, element))


@needs_game
class OnGame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from gamedata import Census, GameFiles
        cls.files = GameFiles(GAME)
        cls.found = gems.census(cls.files)
        cls.census = Census(cls.files)
        cls.pickups = gems.pickup_stems(cls.files)
        cls.drops = gems.drop_stems(cls.census, {}, {}, [])

    def plan(self, seed=1, pickups=None, cursed=False):
        return gems.plan(self.found, seed, seedcode.options_hash({"gem_roulette": 1}), pickups or self.pickups, self.drops, cursed)

    def test_census_reads_kinds_template_and_bitfields(self):
        """DATA: every kind of the table has a grade-4 category whose 20 ranks exist; the template is a droplet weapon gem;
        rune rows (Body bit) are no blood gems."""
        self.assertEqual(set(self.found["kinds"]), set(gems.KINDS))
        layout = self.found["gen_layout"]
        rates = [layout.get(self.found["template"], "slotTypeRate" + c) > 0 for c in "ABCDEF"]
        self.assertEqual(rates, [False] * 5 + [True])
        self.assertEqual((layout.get(self.found["template"], "equipableSegmentWeapon"),
                          layout.get(self.found["template"], "equipableSegmentBody")), (1, 0))
        self.assertIn(126000, self.found["blood"])  # Nightmare of Mensis droplet gem
        self.assertNotIn(102001, self.found["blood"])  # a Caryll rune
        self.assertNotIn(200000, self.found["blood"])  # an oath rune
        self.assertEqual(self.found["kinds"][(1700, 0)]["category"], 171400)

    def test_every_gem_has_its_name_and_effect_texts(self):
        from containers import bnd4_read, dcx_read, fmg_read
        import readable
        menu = {e[1]: fmg_read(e[3])["texts"] for e in bnd4_read(dcx_read(self.files.peek(readable.MENU_TEXT))[0])["entries"]}
        for key, kind in self.found["kinds"].items():
            for rank in range(1, 21):
                self.assertTrue(menu[readable.GEM_EFFECTS].get(kind["effects"][rank]), (key, rank))
                if gems.KINDS[key][0] == "A":
                    for curse in (0, 100000):
                        name = gems.SHAPE_DIGIT * 1000000 + curse + kind["names"][rank]
                        self.assertTrue(menu[readable.GEM_NAMES].get(name), (key, rank, curse))

    def test_pickups_blood_gems_and_half_the_small_ones(self):
        """Every blood gem pickup entry (no rune) and half of the 1-2 Blood Vial / Quicksilver Bullet pickups."""
        from shops import chain
        layout, lots = self.found["lot_layout"], self.found["lots"]
        blood, runes, small = set(), set(), []
        for lot in self.pickups:
            rows = chain(lots, lot)
            if not rows:  # a map names a lot no row holds
                continue
            for r in rows:
                for k in range(1, 9):
                    if layout.get(lots[r], "lotItemCategory%02d" % k) == gems.GEM and layout.get(lots[r], "lotItemId%02d" % k) > 0:
                        (blood if layout.get(lots[r], "lotItemId%02d" % k) in self.found["blood"] else runes).add((r, k))
            items = [k for k in range(1, 9) if layout.get(lots[lot], "lotItemId%02d" % k) > 0]
            if (len(rows) == 1 and len(items) == 1 and layout.get(lots[lot], "lotItemCategory%02d" % items[0]) == gems.GOODS
                    and layout.get(lots[lot], "lotItemId%02d" % items[0]) in (1000, 900)
                    and layout.get(lots[lot], "lotItemNum%02d" % items[0]) <= 2):
                small.append(lot)
        # DATA (base game): 35 blood gem and 24 rune pickup entries; 27 small vial/bullet pickups
        self.assertEqual((len(blood), len(runes), len(small)), (35, 24, 27))
        result = self.plan()
        picked = {(r, k) for gem_id, (kind, r, _) in result["where"].items() if kind == "pickup"
                  for k, entry in result["lots"][r].items() if entry[0][0] == gem_id}
        self.assertTrue(blood <= picked)
        self.assertFalse(runes & picked)
        self.assertEqual(len(picked - blood), len(small) // 2)
        elements = Counter(result["gems"][g]["element"] for g, w in result["where"].items() if w[0] == "pickup")
        self.assertTrue(all(elements[e] for e in gems.GUARANTEED), elements)

    def test_the_tier_follows_where_a_pickup_ends_up(self):
        """With treasure shuffle a lot's gem has the tier of the map it lands in: a Central Yharnam gem moved to the
        Fishing Hamlet is a tier 7 gem."""
        made = self.plan()["where"].values()
        early = next(r for kind, r, stems in sorted(made) if kind == "pickup" and stems == ["m24_01"])
        late = next(lot for lot, stem in sorted(self.pickups.items()) if stem == "m36_00")
        moved = gems.pickup_stems(self.files, {late: early, early: late})
        self.assertEqual((moved[early], moved[late]), ("m36_00", "m24_01"))
        result = self.plan(pickups=moved)
        tiers = {r: result["gems"][g]["tier"] for g, (kind, r, _) in result["where"].items() if kind == "pickup"}
        self.assertEqual(tiers[early], 7)

    def test_drop_rows_keep_their_weights_and_flags(self):
        result = self.plan(cursed=True)
        rows = gems.lot_rows(self.found, result)
        layout, lots = self.found["lot_layout"], self.found["lots"]
        drops = {r for g, (kind, r, _) in result["where"].items() if kind == "drop"}
        self.assertGreater(len(drops), 40)
        for r in drops:
            before = [(layout.get(lots[r], "lotItemId%02d" % k), layout.get(lots[r], "lotItemCategory%02d" % k),
                       layout.get(lots[r], "lotItemBasePoint%02d" % k)) for k in range(1, 9)]
            after = [(layout.get(rows[r], "lotItemId%02d" % k), layout.get(rows[r], "lotItemCategory%02d" % k),
                      layout.get(rows[r], "lotItemBasePoint%02d" % k)) for k in range(1, 9)]
            self.assertEqual(sum(p for _, _, p in before), sum(p for _, _, p in after), r)
            for (item, cat, points), now in zip(before, after):
                if not (cat == gems.GEM and item in self.found["blood"]) and (item > 0 or points > 0):
                    self.assertEqual(now, (item, cat, points), r)  # nothing, vials and runes stay as they are
            self.assertFalse(any(cat == gems.GEM and item in self.found["blood"] for item, cat, _ in after))
            self.assertEqual(layout.get(rows[r], "getItemFlagId"), layout.get(lots[r], "getItemFlagId"))

    def test_a_world_holds_the_gems(self):
        """A generated world: one GemGen row per gem (droplet, three fixed effects), every converted lot points at one,
        and the spoiler names every gem."""
        import generate
        from containers import Layout, Param, bnd4_find, bnd4_read, dcx_read
        from gamedata import GameFiles
        code = seedcode.encode(808, {"gem_roulette": 1, "cursed_gems": 1, "treasure": 1, "drops": 1})
        with tempfile.TemporaryDirectory() as out:
            folder, spoiler = generate.generate(GAME, [], out, code, "world")
            world = GameFiles(GAME, [folder])
            bnd = bnd4_read(dcx_read(world.read("param/gameparam/gameparam.parambnd.dcx"))[0])
        defs = bnd4_read(dcx_read(self.files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
        gen_layout = Layout(bnd4_find(defs, "\\GemGenParam.paramdef")[3])
        lot_layout = Layout(bnd4_find(defs, "\\ItemLotParam.paramdef")[3])
        gen = Param(bnd4_find(bnd, "\\GemGenParam.param")[3])
        ids = [r[0] for r in gen.rows]
        self.assertEqual(ids, sorted(set(ids)))
        made = {int(i) for i in spoiler["gems"]}
        self.assertEqual(len(ids), len(self.found["taken"]) + len(made))
        rows = gen.first()
        for gem_id in made:
            row = rows[gem_id]
            self.assertEqual([gen_layout.get(row, "gemeffectGenParamType_%d" % i) for i in range(6)], [1, 1, 1, 0, 0, 0])
            self.assertEqual([gen_layout.get(row, "gemeffectGenParam_%d" % i) for i in range(3)], spoiler["gems"][str(gem_id)]["effects"])
            self.assertEqual([gen_layout.get(row, "manifestRate_%d" % i) for i in range(3)], [100.0] * 3)
        lots = Param(bnd4_find(bnd, "\\ItemLotParam.param")[3]).first()
        pointed = {lot_layout.get(row, "lotItemId%02d" % k) for row in lots.values() for k in range(1, 9)
                   if lot_layout.get(row, "lotItemCategory%02d" % k) == gems.GEM}
        self.assertTrue(made <= pointed)
        from shops import chain
        for lot in self.pickups:  # no main-map pickup hands out a vanilla blood gem any more
            for r in chain(lots, lot):
                for k in range(1, 9):
                    if lot_layout.get(lots[r], "lotItemCategory%02d" % k) == gems.GEM:
                        self.assertNotIn(lot_layout.get(lots[r], "lotItemId%02d" % k), self.found["blood"], r)
        readable = spoiler["readable"]
        self.assertNotIn("error", readable)
        self.assertEqual(len(readable["gems"]), len(made))
        self.assertFalse([line for line in readable["gems"] if line[1].startswith("#") or "#" in line[1]])
        self.assertEqual(sum(line[2] for line in readable["gems"]), sum(g["cursed"] for g in spoiler["gems"].values()))


if __name__ == "__main__":
    unittest.main()
