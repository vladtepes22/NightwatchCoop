"""Mod weapons with their own clip group (Nightwatch 0.3.0 weapon_clip_groups) in a world: Nocturne v3.0.0's Reverie,
Yorukage and Vesper Blades (and their Uncanny / Lost) are drawn as starters, moved as world loot and sold in shop rows,
and a starter copy of one keeps its moveset through the world's own weapon_clip_groups row (user, 9 Nov: "make sure they
are all supported (all appear as starter weapons, world loot, shop rows)").
Run: python -I -m unittest discover -s tools/randomizer/tests. Reads (never writes) the game at BB_GAME and Nocturne
v3.0.0 at BB_NOCTURNE_V3; those cases skip without them."""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import seedcode  # noqa: E402
import starters  # noqa: E402

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
NOCTURNE_V3 = os.environ.get("BB_NOCTURNE_V3") or next(
    (p for p in (r"C:\Games\NightwatchCoop\Mods\Nocturne v3.0.0",) if os.path.isdir(p)), "")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")
needs_v3 = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")) and NOCTURNE_V3 and
                               os.path.isfile(os.path.join(NOCTURNE_V3, "nightwatch", "weapon_clip_groups.json")),
                               "no game files or no Nocturne v3.0.0")
# DATA, Nocturne v3.0.0 nightwatch/weapon_clip_groups.json
GROUPS = {9100000: 162, 9110000: 162, 9120000: 162, 27400000: 163, 27410000: 163, 27420000: 163,
          27330000: 154, 27340000: 154, 27350000: 154}
PICKUPS = {3400470: 9100000, 3401110: 9100000, 2600470: 27400000, 2800660: 27330000}  # their world lots (DATA)
FLAME_BOLT_PAPER, FLAME_BOLT_LOTS = 2152, (2300480, 2300290)


def _mod(root, name, manifest=None, table=None):
    folder = os.path.join(root, name)
    os.makedirs(os.path.join(folder, "dvdroot_ps4", "param"))
    if manifest is not None:
        with open(os.path.join(folder, "nightwatch.json"), "w", encoding="utf-8") as f:
            f.write(manifest if isinstance(manifest, str) else json.dumps(manifest))
    if table is not None:
        os.makedirs(os.path.join(folder, "nightwatch"))
        with open(os.path.join(folder, "nightwatch", "weapon_clip_groups.json"), "w", encoding="utf-8") as f:
            f.write(table if isinstance(table, str) else json.dumps(table))
    return folder


def _table(*pairs):
    return {"version": 1, "rows": [{"weapon": w, "clip_group": g} for w, g in pairs]}


class Tables(unittest.TestCase):
    def test_mod_tables_merge_like_the_engine(self):
        """Load order, a later mod's row wins; a mod counts only when its manifest lists the feature; a bad table counts
        not at all (features.h ParseWeaponClipGroups); every file read counts in the input hash as that mod's input."""
        from gamedata import GameFiles
        uses = {"id": "x", "requires": "0.3.0", "features": ["weapon_clip_groups"]}
        with tempfile.TemporaryDirectory() as root:
            game = _mod(root, "game")
            mods = [_mod(root, "a", uses, _table((27330000, 154), (9100000, 162))),
                    _mod(root, "b", uses, _table((27330000, 161))),
                    _mod(root, "c", {"id": "c", "requires": "0.3.0", "features": ["hit_effects"]}, _table((4000000, 7))),
                    _mod(root, "d", uses, _table((5000000, 9), (5000000, 9))),       # a weapon twice: dropped whole
                    _mod(root, "e", uses, _table((6000100, 9))),                     # not a base id: dropped whole
                    _mod(root, "f", None, _table((7000000, 9))),                     # no manifest
                    _mod(root, "g", "not json", _table((8000000, 9))),
                    _mod(root, "h")]
            files = GameFiles(game, mods)
            before = files.input_hash()
            self.assertEqual(starters.clip_groups(files), {27330000: 161, 9100000: 162})
            self.assertNotEqual(files.input_hash(), before)
            self.assertEqual([m["files"] for m in files.mod_inputs()], [2, 2, 1, 2, 2, 0, 1, 0])

    def test_copies_of_clip_group_weapons_get_their_origin_group_in_the_world_files(self):
        copies = {27330000: 27500000, 9100000: 9500000, 12000000: 12500000}
        rows = starters.copy_clip_groups({27330000: 154, 9100000: 162, 4000000: 1}, copies)
        self.assertEqual(rows, [{"weapon": 9500000, "clip_group": 162}, {"weapon": 27500000, "clip_group": 154}])
        out = starters.clip_files(rows, "Randomizer R2-X")
        self.assertEqual(sorted(out), ["nightwatch.json", "nightwatch/weapon_clip_groups.json"])
        manifest = json.loads(out["nightwatch.json"])
        self.assertEqual((manifest["requires"], manifest["features"]), ("0.3.0", ["weapon_clip_groups"]))
        self.assertTrue(manifest["id"])
        self.assertEqual(starters._clip_rows(json.loads(out["nightwatch/weapon_clip_groups.json"])),
                         {9500000: 162, 27500000: 154})  # a table the engine takes
        self.assertEqual(starters.clip_files([], "x"), {})  # no clip-group copy: no Nightwatch files


@needs_v3
class NocturneV3(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from gamedata import GameFiles
        cls.files = GameFiles(GAME, [NOCTURNE_V3])

    def test_the_census_takes_every_new_weapon_and_its_lots_and_rows(self):
        import shops
        import treasure
        self.assertEqual(starters.clip_groups(self.files), GROUPS)
        found = starters.census(self.files)
        self.assertLessEqual(set(GROUPS), set(found["melee"]))
        lots = treasure.census(self.files, treasure.chalice_materials(self.files))
        self.assertLessEqual(set(PICKUPS) | set(FLAME_BOLT_LOTS), set(lots["movable"]))
        sold = shops.census(self.files, frozenset(), treasure.chalice_materials(self.files))
        rows = {k: sold["items"][k] for k in sold["rows"].get(shops.WEAPON, [])}
        self.assertEqual({w for w in rows.values() if w in GROUPS}, set(GROUPS))
        self.assertEqual(sum(w in GROUPS for w in rows.values()), 21)  # Bath 9, two chalice shops x 6
        self.assertTrue({FLAME_BOLT_PAPER} & {i for i, _, _ in sold["deck_items"].values()})
        self.assertLessEqual({2600470, 2800660}, set(sold["pickups"][shops.WEAPON]))

    def test_copies_never_take_a_mod_id_and_keep_the_origin_group(self):
        weapons = starters._tables(self.files)("EquipParamWeapon")[1]
        gens = starters._tables(self.files)("WeaponGenParam")[1]
        copies = starters.copy_ids(weapons, gens, list(GROUPS) + [9000000, 27000000])
        for base, copy in copies.items():
            self.assertEqual(copy // starters.WEAPON_NUMBER, base // starters.WEAPON_NUMBER)
            self.assertGreaterEqual(copy // starters.LEVEL % 100, 50)
            self.assertFalse(set(starters._levels(weapons, base)) and
                             any(copy + i - base in weapons for i in starters._levels(weapons, base)), base)
        rows = starters.copy_clip_groups(GROUPS, copies)
        self.assertEqual({r["weapon"]: r["clip_group"] for r in rows}, {copies[w]: g for w, g in GROUPS.items()})

    def test_every_new_weapon_appears_as_a_starter_world_loot_and_in_a_shop_row(self):
        import shops
        import treasure
        found = starters.census(self.files)
        mats = treasure.chalice_materials(self.files)
        lots = treasure.census(self.files, mats)
        sold = shops.census(self.files, frozenset(), mats)
        seen = {"starter": set(), "loot": set(), "shop": set()}
        moved = set()
        for seed in range(1, 41):
            options = seedcode.normalize(seedcode.decode(seedcode.encode(seed, {"starters": 1, "treasure": 1, "shops": 1,
                                                                               "starter_melee": 5}))[1])
            h = seedcode.options_hash(options)
            seen["starter"] |= set(starters.plan(found, seed, h, 5, options["starter_guns"]).values())
            placed = treasure.plan(lots, seed, h)
            moved |= {lot for spot, lot in placed.items() if spot != lot}
            trades = shops.plan(sold, seed, h)
            for key, lot in trades.items():
                seen["loot"].add(sold["items"][key])            # the pickup now gives the row's weapon
                seen["shop"].add(sold["pickup_items"][lot])     # the row now sells the pickup's weapon
            for key, origin in shops.deal(sold, seed, h, trades).items():
                if key != origin:
                    seen["shop"].add(sold["deck_items"][origin][0])
        for kind, items in seen.items():
            self.assertLessEqual(set(GROUPS), items, kind)
        self.assertLessEqual(set(PICKUPS) | set(FLAME_BOLT_LOTS), moved)  # their own pickups move too

    def test_a_world_with_clip_group_starters_ships_their_rows(self):
        """The first seed whose five melee rows offer two of them (no enemies, so it builds fast)."""
        import generate
        from containers import Layout, Param, bnd4_find, bnd4_read, dcx_read
        from gamedata import GameFiles
        options = {"starters": 1, "treasure": 1, "shops": 1, "starter_melee": 5, "enemy_chance": 0}
        h = seedcode.options_hash(seedcode.decode(seedcode.encode(1, options))[1])
        found = starters.census(self.files)
        seed = next(s for s in range(1, 500) if sum(w in GROUPS for w in starters.plan(found, s, h, 5, 2).values()) >= 2)
        code = seedcode.encode(seed, options)
        with tempfile.TemporaryDirectory() as out:
            folder, spoiler = generate.generate(GAME, [NOCTURNE_V3], out, code, "world", base_label="nocturne@3.0.0")
            copies = {int(a): b for a, b in spoiler["starter_copies"].items()}
            want = {copies[w]: g for w, g in GROUPS.items() if w in copies}
            self.assertTrue(want)
            with open(os.path.join(folder, "nightwatch.json"), encoding="utf-8") as f:
                manifest = json.load(f)
            self.assertEqual((manifest["requires"], manifest["features"]), ("0.3.0", ["weapon_clip_groups"]))
            with open(os.path.join(folder, "nightwatch", "weapon_clip_groups.json"), "rb") as f:
                self.assertEqual(starters._clip_rows(json.loads(f.read())), want)
            self.assertEqual({int(a): b for a, b in spoiler["starter_clip_groups"].items()}, want)
            with open(os.path.join(folder, "randomizer_manifest.json"), encoding="utf-8") as f:
                record = json.load(f)
            self.assertEqual(sorted(record["nightwatch"]), ["nightwatch.json", "nightwatch/weapon_clip_groups.json"])
            self.assertIn("mod 1/nightwatch/weapon_clip_groups.json", record["inputs"])
            world = GameFiles(GAME, [NOCTURNE_V3, folder])
            bnd = bnd4_read(dcx_read(world.read("param/gameparam/gameparam.parambnd.dcx"))[0])
            defs = bnd4_read(dcx_read(world.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
            layout = Layout(bnd4_find(defs, "\\EquipParamWeapon.paramdef")[3])
            weapons = Param(bnd4_find(bnd, "\\EquipParamWeapon.param")[3]).first()
            for base, copy in copies.items():  # same category and the SpEffects the hks tests (2733, 2740)
                for f in ("wepmotionCategory", "residentSpEffectId", "behaviorVariationId"):
                    self.assertEqual(layout.get(weapons[copy], f), layout.get(weapons[base], f), (copy, f))


@needs_game
class GameAlone(unittest.TestCase):
    def test_a_world_without_clip_group_starters_has_no_nightwatch_files(self):
        import generate
        code = seedcode.encode(3, {"starters": 1, "enemy_chance": 0})
        with tempfile.TemporaryDirectory() as out:
            folder, spoiler = generate.generate(GAME, [], out, code, "world")
            self.assertTrue(spoiler["starter_copies"])
            self.assertFalse(os.path.exists(os.path.join(folder, "nightwatch.json")))
            self.assertFalse(os.path.exists(os.path.join(folder, "nightwatch")))
            self.assertEqual(spoiler["starter_clip_groups"], {})


if __name__ == "__main__":
    unittest.main()
