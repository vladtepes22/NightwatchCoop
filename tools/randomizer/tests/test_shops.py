"""Shop trades with world pickups (shops.py) on the vanilla tables, and their spoiler lines.
Run from tools/randomizer: python -m unittest discover -s tests. Reads (never writes) the game at BB_GAME."""
import json
import os
import re
import sys
import tempfile
import unittest
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import seedcode  # noqa: E402
import shops  # noqa: E402
import treasure  # noqa: E402
from containers import Layout, Param, bnd4_find, bnd4_read, dcx_read  # noqa: E402

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")
INC = os.path.join(HERE, "..", "..", "..", "engine", "gpu", "shim", "party", "party_items.inc")
YHARNAM_HUNTER = (60000, 61000, 62000, 63000)
RITUAL = {7000: 5000, 7001: 5005, 7002: 5007, 7003: 5053, 7004: 6603, 7020: 5004, 7021: 5005, 7022: 5007, 7023: 5053, 7024: 6603}


def _params(files):
    bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
    defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])

    def param(name):
        return Param(bytes(bnd4_find(bnd, "\\%s.param" % name)[3])), Layout(bnd4_find(defs, "\\%s.paramdef" % name)[3])
    return param


@needs_game
class Trades(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from gamedata import GameFiles
        cls.files = GameFiles(GAME)
        cls.found = shops.census(cls.files)
        cls.materials = treasure.chalice_materials(cls.files)
        cls.joined = shops.census(cls.files, cls.materials)
        cls.param = staticmethod(_params(cls.files))

    def traded(self, found, trades):
        shop, layout = self.param("ShopLineupParam")
        lots, lot_layout = self.param("ItemLotParam")
        before_shop, before_lots = shop.first(), lots.first()
        shops.apply(shop, layout, found, trades)
        shops.apply_lots(lots, lot_layout, found, trades)
        return layout, before_shop, Param(shop.write()).first(), lot_layout, before_lots, Param(lots.write()).first()

    def test_weapon_and_armour_rows_and_buy_once_goods_trade_and_keys_stay(self):
        """User 7 Nov: every shop row but Blood Vials and Quicksilver Bullets is randomized: weapon rows (all unlimited)
        trade like armour; goods rows that trade are buy-once (the unlimited ones are dealt)."""
        layout, rows = shops._tables(self.files)
        listed = [k for kinds in self.found["rows"].values() for k in kinds]
        for first, index in self.found["rows"][3] + list(self.found["pinned"]):
            row = rows[first * shops.BLOCK + index]
            self.assertEqual(layout.get(row, "sellQuantity"), 1)
            self.assertGreater(layout.get(row, "eventFlag"), 0)
        # DATA: in the vanilla shops every buy-once row is armour, a key item, a chalice or a bell; weapons are unlimited
        self.assertEqual(sorted(self.found["rows"]), [0, 1, 3])
        self.assertEqual(len(self.found["rows"][1]), 44)
        self.assertEqual(len(self.found["rows"][0]), 136)
        self.assertTrue(all(layout.get(rows[f * shops.BLOCK + i], "sellQuantity") == -1 for f, i in self.found["rows"][0]))
        self.assertEqual([self.found["items"][k] for k in self.found["rows"][3]], [225])  # Sinister Resonant Bell
        self.assertEqual({(10, i) for i in range(4, 8)} & set(listed), {(10, i) for i in range(4, 8)})  # Yharnam Hunter set
        pinned_items = {layout.get(rows[f * shops.BLOCK + i], "equipId") for f, i in self.found["pinned"]}
        self.assertLessEqual({4011, 4105, 6302, 6312, 6502, 6522, 6532, 205}, pinned_items)
        self.assertFalse({k for k in listed if k[0] not in [s[0] for s in shops.SHOPS]})  # no starter row, no block 90

    def test_pickups_are_unique_single_items_that_nothing_else_gives(self):
        lots, lot_layout = self.param("ItemLotParam")
        rows = lots.first()
        shop_layout, shop_rows = shops._tables(self.files)
        held = Counter((c, i) for r in rows.values() for c, i, _ in shops.entries(lot_layout, r))
        sold = {(shops.TYPES.get(shop_layout.get(r, "equipType")), shop_layout.get(r, "equipId")) for r in shop_rows.values()}
        npcs, npc_layout = self.param("NpcParam")
        npc_lots = {npc_layout.get(r, f) for r in npcs.first().values() for f in shops.NPC_LOTS}
        awarded = treasure._event_values(self.files)[0]
        why = treasure.lot_pins(self.files)
        every = [r for kind in self.found["pickups"].values() for r in kind]
        self.assertEqual(len(every), len(set(every)))
        # a weapon some shop also sells counts as a pickup (its shop rows move too): 18 weapons, not 2
        self.assertEqual({k: len(v) for k, v in self.found["pickups"].items()}, {0: 18, 1: 59, 3: 5})
        for kind, picks in self.found["pickups"].items():
            for r in picks:
                (category, item, count), = shops.entries(lot_layout, rows[r])
                self.assertEqual((category, count), (shops.TYPES[kind], 1))
                self.assertEqual(held[(category, item)], 1)
                if kind != 0:
                    self.assertNotIn((category, item), sold)
                self.assertNotIn(r, npc_lots)
                self.assertNotIn(r, awarded)
                stem, lot = self.found["spots"][r]
                self.assertIn(r, shops.chain(rows, lot))
                self.assertIsNone(why(lot))
        # boss drops, kill awards, award spots and key catch-up lots of the engine's item lists are never a pickup to trade
        if os.path.exists(INC):
            with open(INC, encoding="utf-8") as f:
                text = f.read()
            known = {int(m) for m in re.findall(r"^ITEM_(?:BOSS_DROP|KILL_AWARD|AWARD_SPOT)\((\d+)", text, re.M)}
            known |= {int(m) for m in re.findall(r"^ITEM_SHARED_TREASURE\((\d+), 1,", text, re.M)}
            reached = {r for lot in known for r in shops.chain(rows, lot)}
            self.assertGreater(len(known), 20)
            self.assertFalse(reached & set(every))

    def test_each_trade_swaps_the_items_and_nothing_else_changes(self):
        trades = shops.plan(self.found, 7, "h")
        self.assertEqual(shops.plan(self.found, 7, "h"), trades)  # same code, same trades
        self.assertNotEqual(shops.plan(self.found, 8, "h"), trades)
        self.assertEqual(len(set(trades.values())), len(trades))
        self.assertEqual(len(trades), 63)  # 44 armour rows for 59 pickups, 18 of 136 weapon rows, the bell for 5 goods
        layout, before, after, lot_layout, lots_before, lots_after = self.traded(self.found, trades)
        self.assertEqual(list(after), list(before))
        self.assertEqual(list(lots_after), list(lots_before))
        changed_rows = set()
        for blocks in shops.SHOPS:
            for (f, i), lot in trades.items():
                if f == blocks[0]:
                    changed_rows.update(b * shops.BLOCK + i for b in blocks)
        for rid, row in before.items():
            if rid not in changed_rows:
                self.assertEqual(after[rid], row)  # unlimited rows, pinned rows, starters: byte for byte
                continue
            first = next(blocks[0] for blocks in shops.SHOPS if rid // shops.BLOCK in blocks)
            lot = trades[(first, rid % shops.BLOCK)]
            for field in ("qwcId", "eventFlag", "sellQuantity", "shopType", "equipType", "mtrlId", "value_SAN"):
                self.assertEqual(layout.get(after[rid], field), layout.get(row, field), field)
            self.assertEqual(layout.get(after[rid], "equipId"), self.found["pickup_items"][lot])
            kind = layout.get(row, "equipType")
            fit = self.found["fits"][rid // shops.BLOCK][kind]
            self.assertTrue(fit[2] <= layout.get(after[rid], "value") <= fit[3])
            self.assertEqual(layout.get(after[rid], "value"),
                             shops.price(self.found, rid // shops.BLOCK, kind, self.found["pickup_items"][lot], layout.get(row, "value")))
        given = shops.traded_lots(self.found, trades)
        for r, row in lots_before.items():
            if r not in given:
                self.assertEqual(lots_after[r], row)  # key items, boss drops, quest rewards and every other lot: untouched
                continue
            (category, item, count), = shops.entries(lot_layout, lots_after[r])
            self.assertEqual((item, count), (given[r], 1))
            self.assertEqual(category, shops.entries(lot_layout, row)[0][0])
            stripped = [bytearray(row), bytearray(lots_after[r])]
            for body in stripped:
                for k in range(1, 9):
                    lot_layout.set(body, "lotItemId%02d" % k, 0)
            self.assertEqual(stripped[0], stripped[1])  # flag, count, chance: the pickup's own

    def test_prices_fit_the_new_item(self):
        # Bath Messengers' armour: the median buy-once row is Cainhurst Armor, 44,000 for a sellValue of 30,800
        self.assertEqual(self.found["fits"][10][1], (44000, 30800, 500, 60000))
        self.assertEqual(shops._round(Fraction_of(25200 * 44000, 30800)), 36000)  # Knight's Garb
        self.assertEqual(shops.price(self.found, 10, 1, 81000, 500), 36000)
        self.assertEqual(shops.price(self.found, 10, 1, 320000, 27000), 500)  # a cheap hat stops at the shop's lowest price
        # the insight shop prices in insight: small numbers, at least 1
        self.assertTrue(all(1 <= shops.price(self.found, 20, 1, i, 3) <= 5 for i in self.found["values"][1]))
        self.assertEqual(shops.price(self.found, 10, 3, 4011, 777), 777)  # no value known: the row keeps its price

    def test_chalice_materials_off_keep_their_rows_and_gates_on_join(self):
        layout, rows = shops._tables(self.files)
        for item, gate in RITUAL.items():
            self.assertEqual([(layout.get(r, "qwcId"), layout.get(r, "sellQuantity")) for rid, r in rows.items()
                              if rid // shops.BLOCK == 10 and layout.get(r, "equipType") == 3 and layout.get(r, "equipId") == item],
                             [(gate, -1)])  # DATA: unlimited rows
        listed = lambda found: {found["items"][k] for k in found["rows"].get(3, [])}
        self.assertFalse(listed(self.found) & self.materials)
        trades = shops.plan(self.found, 3, "h")
        _, before, after, *_ = self.traded(self.found, trades)
        for rid, row in before.items():
            if layout.get(row, "equipType") == 3 and layout.get(row, "equipId") in self.materials:
                self.assertEqual(after[rid], row)
        # on: the material rows join the goods trades (the one kind of unlimited row that trades), five goods pickups to share
        self.assertLessEqual(set(RITUAL), listed(self.joined))
        joined = shops.plan(self.joined, 3, "h")
        goods = [k for k in joined if k in self.joined["rows"][3]]
        self.assertEqual(len(goods), 5)
        _, before, after, *_ = self.traded(self.joined, joined)
        for first, index in goods:
            for b in next(s for s in shops.SHOPS if s[0] == first):
                r = b * shops.BLOCK + index
                self.assertEqual(layout.get(after[r], "qwcId"), layout.get(before[r], "qwcId"))
                self.assertEqual(layout.get(after[r], "sellQuantity"), layout.get(before[r], "sellQuantity"))
                self.assertEqual(layout.get(after[r], "equipId"), self.joined["pickup_items"][joined[(first, index)]])

    def test_the_deal_moves_every_other_row_and_loses_no_item(self):
        """User 7 Nov: weapon rows no trade takes are dealt within their shop, consumables across the shops; no shop sells
        one item from two rows; Blood Vials and Quicksilver Bullets stay; every weapon is still sold or found."""
        layout, rows = shops._tables(self.files)
        for seed in (1, 2, 3):
            trades = shops.plan(self.found, seed, "h")
            dealt = shops.deal(self.found, seed, "h", trades)
            self.assertEqual(dealt, shops.deal(self.found, seed, "h", trades))
            self.assertFalse(set(dealt) & set(trades))
            weapons = [k for k in self.found["rows"][0]]
            self.assertEqual(set(weapons), {k for k in weapons if k in trades} | {k for k in weapons if k in dealt})
            self.assertTrue(all(o[0] == k[0] for k, o in dealt.items() if self.found["deck_items"][k][2] == 0))
            sold = Counter((k[0], self.found["pickup_items"][lot]) for k, lot in trades.items())
            sold.update((k[0], self.found["deck_items"][o][0]) for k, o in dealt.items())
            self.assertEqual([k for k, n in sold.items() if n > 1], [])
            same = [k for k, o in dealt.items() if self.found["deck_items"][k][0] == self.found["deck_items"][o][0]]
            self.assertLessEqual(len(same), 3)  # only a consumable several shops sell can come back to its own row
            before = Counter(self.found["deck_items"][k][0] for k in weapons)
            before.update(self.found["pickup_items"][r] for r in self.found["pickups"][0])
            after = Counter(self.found["deck_items"][o][0] for k, o in dealt.items() if self.found["deck_items"][k][2] == 0)
            after.update(self.found["pickup_items"][trades[k]] for k in weapons if k in trades)  # sold now
            after.update(self.found["items"][k] for k in weapons if k in trades)  # in the world now
            self.assertEqual(set(after), set(before))  # every weapon still sold or found somewhere
        staples = [rid for rid, r in rows.items() if layout.get(r, "equipType") == 3 and layout.get(r, "equipId") in shops.STAPLES]
        self.assertTrue(staples)
        self.assertFalse({(rid // shops.BLOCK, rid % shops.BLOCK) for rid in staples} & set(dealt))

    def test_blood_stone_floor_rows_stay_out_of_the_deal_and_the_insight_shop_sells_no_stone_twice(self):
        """User 9 Nov: the Insight shop's stone rows are a fixed floor (1/2/5/10 insight after Cleric Beast, Shadows, One
        Reborn, Wet Nurse); the Bath Messengers' stone rows are dealt as before."""
        self.assertEqual(self.found["stones"], {(20, 8): 3000, (20, 10): 3010, (20, 24): 3020, (20, 25): 3030})
        bath = {k for k, (item, _, _) in self.found["deck_items"].items() if k[0] == 10 and item in shops.STONES}
        self.assertEqual(bath, {(10, 708), (10, 1003)})  # DATA: Shard and Twin Shards, still dealt
        moved = 0
        for seed in range(1, 41):
            trades = shops.plan(self.found, seed, "h")
            dealt = shops.deal(self.found, seed, "h", trades)
            self.assertFalse(set(dealt) & set(self.found["stones"]))
            sold = Counter((k[0], self.found["deck_items"][o][0]) for k, o in dealt.items())
            sold.update((first, item) for (first, _), item in self.found["stones"].items())
            self.assertEqual([k for k, n in sold.items() if n > 1], [], seed)
            moved += sum(1 for k in bath if self.found["deck_items"][dealt[k]][0] not in shops.STONES)
        self.assertGreater(moved, 40)  # the Bath rows really are shuffled

    def test_apply_stones_sets_every_copy_and_nothing_else(self):
        shop, layout = self.param("ShopLineupParam")
        before = shop.first()
        shops.apply_stones(shop, layout, self.found["stones"])
        after = Param(shop.write()).first()
        self.assertEqual(list(after), list(before))
        want = {3000: (1, 12411700), 3010: (2, 12701800), 3020: (5, 12801800), 3030: (10, 12601800)}
        changed = set()
        for block in (20, 21, 22, 23, 24):
            for (_, index), item in self.found["stones"].items():
                rid = block * shops.BLOCK + index
                changed.add(rid)
                row = after[rid]
                self.assertEqual(layout.get(row, "equipId"), item)
                self.assertEqual((layout.get(row, "value"), layout.get(row, "qwcId")), want[item], rid)
                self.assertEqual(layout.get(row, "sellQuantity"), -1)
        for rid, row in before.items():
            if rid not in changed:
                self.assertEqual(after[rid], row)

    def test_yharnam_hunter_set_leaves_the_shop(self):
        for seed in range(1, 6):
            trades = shops.plan(self.found, seed, "h")
            self.assertTrue(all((10, i) in trades for i in range(4, 8)))


def Fraction_of(a, b):
    from fractions import Fraction
    return Fraction(a, b)


@needs_game
class World(unittest.TestCase):
    def test_a_world_with_shops_treasure_and_materials_shows_every_trade(self):
        import generate
        from gamedata import GameFiles
        code = seedcode.encode(77, {"treasure": 1, "shops": 1, "chalice_materials": 1, "enemy_chance": 0})
        with tempfile.TemporaryDirectory() as out:
            folder, spoiler = generate.generate(GAME, [], out, code, "world")
            world = GameFiles(GAME, [folder])
            param = _params(world)
            shop, layout = param("ShopLineupParam")
            lots, lot_layout = param("ItemLotParam")
            shop_rows, lot_rows = shop.first(), lots.first()
            tables = {k: set(param(n)[0].first()) for k, n in shops.VALUES.items()}
            for rid, row in shop_rows.items():  # every row sells an item that exists
                if layout.get(row, "equipType") in tables and rid // shops.BLOCK not in (0, 90):
                    self.assertIn(layout.get(row, "equipId"), tables[layout.get(row, "equipType")], rid)
        vanilla = _params(GameFiles(GAME))
        base_shop, _ = vanilla("ShopLineupParam")
        base_lots, _ = vanilla("ItemLotParam")
        base_shop, base_lots = base_shop.first(), base_lots.first()
        trades = {(int(f), int(i)): lot for f, m in spoiler["shops"].items() for i, lot in m.items()}
        self.assertGreater(len(trades), 40)
        for (first, index), lot in trades.items():
            rid = first * shops.BLOCK + index
            self.assertEqual(layout.get(shop_rows[rid], "equipId"), shops.entries(lot_layout, base_lots[lot])[0][1])
            self.assertEqual(shops.entries(lot_layout, lot_rows[lot])[0][1], layout.get(base_shop[rid], "equipId"))
        # starters stay out of it
        for rid in (2000, 2001, 2002, 2010, 2011):
            self.assertEqual(shop_rows[rid], base_shop[rid])
        readable = spoiler["readable"]
        self.assertNotIn("error", readable)
        dealt = {tuple(map(int, a.split(":"))): tuple(map(int, b.split(":"))) for a, b in spoiler["shop_goods"].items()}
        self.assertGreater(len(dealt), 30)
        self.assertFalse(set(dealt) & set(trades))
        # the consumables are dealt again: each dealt row sells its origin row's vanilla item, in every copy
        for (first, index), origin in dealt.items():
            want = layout.get(base_shop[origin[0] * shops.BLOCK + origin[1]], "equipId")
            for blocks in shops.SHOPS:
                if blocks[0] == first:
                    for block in blocks:
                        row = shop_rows[block * shops.BLOCK + index]
                        self.assertEqual(layout.get(row, "equipId"), want)
                        self.assertGreater(layout.get(row, "value"), 0)
                        self.assertEqual(layout.get(row, "sellQuantity"), -1)
        for item in shops.STAPLES:  # Blood Vials and Quicksilver Bullets stay where they are
            for rid, row in base_shop.items():
                if layout.get(row, "equipId") == item and layout.get(row, "equipType") == shops.GOODS:
                    self.assertEqual(shop_rows[rid], row)
        same = sum(1 for (f, i), o in dealt.items()
                   if layout.get(base_shop[o[0] * shops.BLOCK + o[1]], "equipId") == layout.get(base_shop[f * shops.BLOCK + i], "equipId"))
        self.assertEqual(len(readable["shops"]), len(trades) + len(dealt) - same)
        self.assertTrue(all(line[1] != line[2] for line in readable["shops"]))
        # each trade's pickup spot is listed in the treasure lines, now giving the shop's old item
        was = Counter(line[1] for line in readable["shops"] if line[5])
        shown = " | ".join(now for _, _, now in readable["treasure"])
        for item in was:
            self.assertIn(item, shown)
        self.assertEqual(json.loads(json.dumps(spoiler["shops"])), spoiler["shops"])
        for block in (20, 21, 22, 23, 24):  # the blood stone floor
            self.assertEqual([(layout.get(shop_rows[block * shops.BLOCK + i], "equipId"), layout.get(shop_rows[block * shops.BLOCK + i], "qwcId"))
                              for i in (8, 10, 24, 25)], [(3000, 12411700), (3010, 12701800), (3020, 12801800), (3030, 12601800)])

    def test_a_treasure_only_world_has_the_stone_floor_and_an_enemy_world_does_not(self):
        import generate
        from gamedata import GameFiles
        for options, floor in (({"treasure": 1, "enemy_chance": 0}, True), ({"enemy_chance": 0}, False)):
            with tempfile.TemporaryDirectory() as out:
                folder, _ = generate.generate(GAME, [], out, seedcode.encode(5, options), "world")
                shop, layout = _params(GameFiles(GAME, [folder]))("ShopLineupParam")
                gate = layout.get(shop.first()[200008], "qwcId")
                self.assertEqual(gate == 12411700, floor, options)


if __name__ == "__main__":
    unittest.main()
