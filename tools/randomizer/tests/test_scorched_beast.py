"""Option scorched_beast (option 52): Nocturne v5.0.0's Scorched Beast (the Lesser Demon c5500, entity 2300820, with his second
head c5501, 2300821, in the Old Yharnam plaza of m23_00) is a boss of the pool and his plaza draws a boss. He is a base mod's
fight (arenas.Arena option / model): neither an arena nor a fighter without the option, and nothing without Nocturne v5.0.0.
His head rides his neck wherever he fights (helpers.FIGHTS[2300820]). His rewards are the arena's, like every boss's: no code
of its own.
Run: python -I -m unittest discover -s tools/randomizer/tests. Reads the game at BB_GAME and the newest Nocturne (noc.py,
BB_NOCTURNE); the Nocturne cases skip without a v5.0.0+."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import arenas
from noc import NOCTURNE

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
BODY, HEAD, KEY = 2300820, 2300821, "lesser_demon"
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")


def _has_beast():
    return bool(NOCTURNE) and os.path.isfile(os.path.join(NOCTURNE, "dvdroot_ps4", "map", "MapStudio", "m23_00_00_00.msb.dcx"))


needs_nocturne = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")) and _has_beast(),
                                     "no game files or no Nocturne v5.0.0+")


class Inputs:
    """The game alone and the game with the newest Nocturne, each read once."""
    _cache = {}

    @classmethod
    def census(cls, mods):
        key = tuple(mods)
        if key not in cls._cache:
            from gamedata import Census, GameFiles
            cls._cache[key] = Census(GameFiles(GAME, list(mods)))
        arenas.set_phases(cls._cache[key].phases, cls._cache[key].found_helpers)  # another test's census may have read other inputs since
        return cls._cache[key]

    @classmethod
    def plan(cls, mods, seed, options):
        import planner
        from neighbours import group_limits, load_groups
        c = cls.census(mods)
        groups = load_groups()
        limits = group_limits(c, groups)
        result = planner.plan(c.slots, c.candidates, c.sizes, groups, limits, seed, options, c.animations)
        return c, result, planner.check(result, c.slots, c.candidates, c.sizes, groups, limits, c.animations)


class Table(unittest.TestCase):
    def test_the_row_is_a_mod_fight_of_one_body_and_his_head(self):
        a = arenas.BY_KEY[KEY]
        self.assertEqual((a.stem, a.kind, a.bodies, a.extras, a.flags, a.intro, a.option, a.model),
                         ("m23_00", arenas.SOLO, (BODY,), (HEAD,), (12301820, 12304820), None, "scorched_beast", "c5500"))
        self.assertNotIn(a, arenas.ARENAS_OF_GAME)
        self.assertIn(KEY, arenas.ENEMY_OUT)  # his head is a separate part: never a plain enemy

    def test_the_option_is_52_and_late(self):
        import seedcode
        self.assertEqual(seedcode._BY_ID[52][1:], ("scorched_beast", "int", 0))
        self.assertEqual(seedcode.BOOL_OPTIONS_R2[-1], "scorched_beast")  # appended: older codes keep their bits
        self.assertEqual(seedcode.LATE_OPTIONS["scorched_beast"], 0)
        a, b = {"boss_swap": 1}, {"boss_swap": 1, "scorched_beast": 0}
        self.assertEqual(seedcode.options_hash(seedcode.normalize(a)), seedcode.options_hash(seedcode.normalize(b)))
        code = seedcode.encode(7, {"boss_swap": 1, "scorched_beast": 1})
        self.assertEqual(seedcode.decode(code)[1]["scorched_beast"], 1)
        self.assertEqual(seedcode.decode(seedcode.encode(7, a))[1]["scorched_beast"], 0)

    def test_the_fight_in_helpers(self):
        import helpers
        fight = helpers.FIGHTS[BODY]
        self.assertEqual((fight.key, fight.boss, fight.travel, fight.hold, fight.wake, fight.authority, fight.disable),
                         (KEY, BODY, (HEAD,), 12304822, (HEAD,), (HEAD,), (HEAD,)))
        self.assertEqual(fight.moved, (12304823,))
        self.assertEqual(len(fight.made), 2)  # the head's death with him, its breath at half HP


@needs_game
class OnTheGameAlone(unittest.TestCase):
    def test_he_is_not_there_and_the_option_does_nothing(self):
        c = Inputs.census(())
        self.assertNotIn(BODY, {s["entity"] for slots in c.slots.values() for s in slots})
        for seed in (1, 2, 3):
            _, off, bad = Inputs.plan((), seed, {"boss_swap": 1})
            self.assertEqual(bad, [])
            _, on, bad = Inputs.plan((), seed, {"boss_swap": 1, "scorched_beast": 1})
            self.assertEqual(bad, [])
            self.assertEqual(on["changes"], off["changes"])  # the world of it off
            self.assertEqual(on["bosses"], off["bosses"])
            self.assertIn("scorched_beast", on["report"]["skipped_options"])
            self.assertNotIn(KEY, on["fighters"])
            self.assertNotIn(KEY, on["report"]["arenas"])


@needs_nocturne
class OnNocturne(unittest.TestCase):
    def test_he_is_found_and_his_head_is_kept(self):
        c = Inputs.census((NOCTURNE,))
        index = {s["entity"]: s for slots in c.slots.values() for s in slots}
        self.assertEqual((index[BODY]["model"], index[BODY]["kept"], index[BODY]["role"], index[BODY]["arena"]),
                         ("c5500", "boss", "body", KEY))
        self.assertEqual((index[HEAD]["model"], index[HEAD]["kept"], index[HEAD]["role"]), ("c5501", "boss", "extra"))
        self.assertNotIn("c5500", c.candidates)
        self.assertNotIn("c5501", c.candidates)  # no world puts his head on an enemy's neck

    def test_off_he_stays_and_nothing_draws_for_him(self):
        for seed in range(1, 6):
            _, result, bad = Inputs.plan((NOCTURNE,), seed, {"boss_swap": 1, "bosses_as_enemies": 1})
            self.assertEqual(bad, [])
            self.assertNotIn(BODY, result["bosses"])
            self.assertNotIn(KEY, result["fighters"])
            self.assertNotIn(KEY, result["report"]["arenas"])
            self.assertNotIn("skipped_options", result["report"])

    def test_both_directions_in_the_plan(self):
        """On: his plaza draws a boss and he fights elsewhere, never as a plain enemy; the plan passes its own checks."""
        away = 0
        for seed in range(1, 13):
            _, result, bad = Inputs.plan((NOCTURNE,), seed, {"boss_swap": 1, "scorched_beast": 1, "bosses_as_enemies": 1})
            self.assertEqual(bad, [], seed)
            self.assertIn(BODY, result["bosses"], seed)  # his plaza has a boss
            self.assertEqual(result["report"]["arenas"][KEY]["mode"], "swap")
            self.assertNotIn(KEY, [k for k in result.get("enemy_bosses", ())])
            elsewhere = [e for e, k in result["boss_fighters"].items() if k == KEY]
            away += bool(elsewhere)
            if elsewhere:
                fighter = result["fighters"][KEY]
                self.assertEqual((fighter["model"], fighter["origin"], fighter["extras"], fighter["extra_models"]),
                                 ("c5500", BODY, [HEAD], ["c5501"]))
            self.assertTrue(all(arenas.arena_of(e).key != KEY for e in elsewhere))
        self.assertGreaterEqual(away, 8)  # of 12 draws

    def test_the_world_of_his_option_off_is_the_world_without_him(self):
        """Nocturne v5.0.0 with the option off draws the same enemy world as the code without the key (the hash)."""
        import seedcode
        self.assertEqual(seedcode.encode(5, {"boss_swap": 1}), seedcode.encode(5, {"boss_swap": 1, "scorched_beast": 0}))

    def test_a_world_where_he_fights_elsewhere_carries_his_head(self):
        """One generated world (seed 1): his body in another arena, the head a part of that map, set up like his own fight
        (hold, authority, doping, link), dying with him and breathing at half HP; his plaza holds the new boss with the head
        disabled and its carry loop parked."""
        import generate
        import seedcode
        from containers import dcx_read
        from emevd import Emevd
        from msb import Msb
        code = seedcode.encode(1, {"boss_swap": 1, "scorched_beast": 1})
        with tempfile.TemporaryDirectory() as out:
            folder, spoiler = generate.generate(GAME, [NOCTURNE], out, code, "world")
            arena_key = next(k for k, a in spoiler["report"]["arenas"].items() if KEY in a["bodies"].values())
            target = arenas.BY_KEY[arena_key]
            slot = next(int(e) for e, v in spoiler["report"]["arenas"][arena_key]["bodies"].items() if v == KEY)

            def events(stem):
                with open(os.path.join(folder, "dvdroot_ps4", "event", "%s_00_00.emevd.dcx" % stem), "rb") as f:
                    return Emevd(dcx_read(f.read())[0])

            def lines(ev, bank, iid):
                return [(e, w) for e in ev.event_ids() for b, d, w in ev.instructions(e) if (b, d) == (bank, iid)]

            there = events(target.stem)
            kills = [w for _, w in lines(there, 2004, 43) if w[1] == 1 and w[0] // 100 == slot // 100 and w[0] % 100 >= 60]
            heads = {w[0] for w in kills}
            self.assertEqual(len(heads), 1)  # the head's death with him: waits for the slot body's death
            head = heads.pop()
            self.assertNotIn(head, (BODY, HEAD))
            self.assertIn(head // 10000, (slot // 10000,))
            self.assertTrue(any(w[:2] == [head, 4095] or list(w)[:2] == [head, 4095] for _, w in lines(there, 2004, 28)))
            self.assertTrue([w for _, w in lines(there, 2004, 8) if w[0] == head and w[1] in (7500, 7501)])  # doped
            self.assertIn([head, 5500010, 0], [list(w) for _, w in lines(there, 2004, 8)])  # the phase-two breath
            self.assertIn([head, slot], [list(w) for _, w in lines(there, 2004, 33)])  # its damage goes to him
            self.assertTrue([w for _, w in lines(there, 2004, 40) if w[0] == head and w[2] == slot and w[3] == 100])  # the carry
            # the head is a part of the target map
            stem_msb = [n for n in os.listdir(os.path.join(folder, "dvdroot_ps4", "map", "MapStudio"))
                        if n.startswith(target.stem) and n.endswith(".msb.dcx")]
            self.assertTrue(stem_msb)
            for name in stem_msb:
                with open(os.path.join(folder, "dvdroot_ps4", "map", "MapStudio", name), "rb") as f:
                    parts = Msb(dcx_read(f.read())[0]).enemies()
                self.assertEqual([p["model"] for p in parts if p["entity"] == head], ["c5501"])
            # the head's fireballs are effects of Nocturne's m23 archive only: they come along (17 Nov: invisible in
            # Maria's arena, the head being no slot change)
            from containers import bnd4_read
            with open(os.path.join(folder, "dvdroot_ps4", "sfx", "frpg_sfxbnd_%s.ffxbnd.dcx" % target.stem[:3]), "rb") as f:
                effects = {e[2].split("\\")[-1] for e in bnd4_read(dcx_read(f.read())[0])["entries"]}
            self.assertTrue({"f000655001.fxr", "f000655002.fxr", "f000655011.fxr", "f000655012.fxr", "f000655140.fxr",
                             "f000017509.fxr"} <= effects)
            # at home: the carry loop is parked, the head disabled
            home = events("m23_00")
            carry = home.instructions(12304823)
            self.assertEqual([(b, d) for b, d, _ in carry], [(3, 0)])
            self.assertIn([HEAD, 0], [list(w) for _, w in lines(home, 2004, 5)])


if __name__ == "__main__":
    unittest.main()
