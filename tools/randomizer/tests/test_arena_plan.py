"""Boss round 2, the plan side: every arena kind of arenas.py as the planner fills it, on vanilla data (BB_GAME, read only).
Run: python -I -m unittest discover -s tools/randomizer/tests"""
import os
import sys
import unittest
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import arenas
import planner

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")
SEEDS = range(40)
MIXES = {"witch_mix": 1, "shadows_mix": 1, "emissary_mix": 1, "failures_mix": 1}
MAIDENS = arenas.BY_KEY["one_reborn"].escorts
EMISSARY_SMALL = [e for e in arenas.BY_KEY["emissary"].bodies if e != 2420811]


@needs_game
class ArenaPlan(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from gamedata import Census, GameFiles
        from neighbours import group_limits, load_groups
        cls.census = Census(GameFiles(GAME))
        cls.groups = load_groups()
        cls.limits = group_limits(cls.census, cls.groups)
        cls.index = {s["entity"]: (stem, s) for stem, v in cls.census.slots.items() for s in v}
        cls.cache = {}

    def plan(self, seed, options, **extra):
        key = (seed, tuple(sorted(options.items())), repr(sorted(extra.items())))
        if key not in self.cache:
            c = self.census
            from seedcode import normalize
            maidens = normalize(options)["bell_maidens"]  # as generate applies the option
            slots = {stem: [planner.escort_slot(s, maidens) if s["entity"] in MAIDENS else s for s in v]
                     for stem, v in c.slots.items()}
            result = planner.plan(slots, c.candidates, c.sizes, self.groups, self.limits, seed, options, c.animations, **extra)
            self.assertEqual(planner.check(result, slots, c.candidates, c.sizes, self.groups, self.limits, c.animations), [],
                             (seed, options))
            self.cache[key] = result
        return self.cache[key]

    def model(self, result, entity):
        stem, part = result["bosses"][entity]
        return result["changes"][stem][part][0]

    def expected_bodies(self, options):
        from seedcode import normalize
        norm = normalize(options)
        out = set()
        for a in arenas.ARENAS_OF_GAME:
            mix = bool(a.mix_option and norm[a.mix_option])
            if a.kind in (arenas.SOLO, arenas.GROUP):
                out |= set(a.bodies[:1] if a.kind == arenas.SOLO or (a.kind == arenas.GROUP and not mix) else a.bodies)
            elif a.kind == arenas.ARENA_ONLY:
                out |= set(a.bodies) if mix else {a.main}
        return out

    def test_census_pins_every_part_of_a_fight(self):
        for entity in [e for a in arenas.ARENAS_OF_GAME for e in a.bodies + a.extras + ((a.pool,) if a.pool else ())]:
            stem, slot = self.index[entity]
            self.assertEqual(slot["kept"], "boss", entity)
            self.assertEqual(slot["arena"], arenas.arena_of(entity).key)
        for entity in MAIDENS:
            self.assertEqual(self.index[entity][1]["kept"], "escort", entity)
            self.assertEqual(self.index[entity][1]["role"], "escort")
        self.assertEqual(self.index[2420711][1]["role"], "body")
        self.assertEqual(self.index[2800803][1]["role"], "pool")
        self.assertEqual(self.index[2500801][1]["role"], "extra")
        # D11: no invisible model in the ordinary pool
        self.assertNotIn("c2561", self.census.candidates)
        self.assertFalse([m for m in self.census.candidates if 0 < self.census.sizes[m] < 4096])
        for fighter in arenas.fighter_entities():
            slot = self.index[fighter][1]
            self.assertGreater(self.census.goals(slot["think"], slot["npc"])[0], 0, fighter)  # think 0: its NpcParam's

    def test_every_boss_body_gets_another_boss_in_every_mode(self):
        for options in ({"boss_swap": 1}, {"boss_swap": 1, "headroom": 150}, dict(MIXES, boss_swap=1),
                        dict(MIXES, boss_swap=1, headroom=150), {"boss_swap": 1, "witch_mix": 1, "shadows_mix": 1, "emissary_mix": 1, "failures_mix": 1, "bell_maidens": 2, "difficult_enemies": 1, "second_stage": 1}):
            wanted = self.expected_bodies(options)
            for seed in SEEDS:
                result = self.plan(seed, options)
                self.assertEqual(set(result["bosses"]), wanted, (seed, options))
                for entity, key in result["boss_fighters"].items():
                    fighter = result["fighters"][key]
                    slot = self.index[entity][1]
                    self.assertNotEqual(fighter["home"], slot["arena"], (seed, entity))
                    self.assertNotEqual(self.model(result, entity), slot["model"], (seed, entity))
                    self.assertNotIn(fighter["origin"], (2600850, 2420811, 3500851))
                # distinct first: every fighter fights before any fights twice
                draws = Counter(result["boss_fighters"][u[0]] for _, _, us in
                                planner.arena_units(result["options"], self.index) for u in us)
                fighters = arenas.fighter_entities()
                self.assertEqual(len(draws), min(sum(draws.values()), len(fighters)), seed)
                self.assertLessEqual(max(draws.values()) - min(draws.values()), 1, seed)

    def test_second_stage_travels_and_group_one_or_one_each(self):
        for seed in SEEDS:
            off = self.plan(seed, {"boss_swap": 1})
            on = self.plan(seed, dict(MIXES, boss_swap=1))
            staged = self.plan(seed, {"boss_swap": 1, "second_stage": 1})
            for key in ("gascoigne", "ludwig", "orphan"):
                bodies = arenas.BY_KEY[key].fighters  # the boss, its second phase
                for plan in (off, staged):  # the arena never splits: one boss on body 1
                    self.assertEqual(set(bodies) & set(plan["bosses"]), {bodies[0]}, (seed, key))
                if off["fighters"].get(key, {}).get("stage"):  # option off: only when its own second phase fights elsewhere
                    own = next((k for k, f in off["fighters"].items() if f["origin"] == bodies[0] + 1), None)
                    self.assertIn(own, off["boss_fighters"].values(), (seed, key))
                if staged["fighters"].get(key, {}).get("stage"):  # placed: another boss in its second body
                    fighter = staged["fighters"][key]
                    stage = fighter["stage"]
                    self.assertNotEqual(stage["key"], key)
                    self.assertNotEqual(stage["model"], fighter["model"])
                    self.assertIn(stage["model"], fighter["extra_models"])
                    self.assertEqual(staged["report"]["stages"][key], stage["key"])
            for key in ("witch", "shadows"):
                bodies = arenas.BY_KEY[key].bodies
                # mix off: one boss on body 1, the others leave (user, 8 Nov)
                self.assertEqual(set(bodies) & set(off["boss_fighters"]), {bodies[0]}, (seed, key))
                self.assertEqual(off["arena_modes"][key], "normal")
                self.assertEqual(len({on["boss_fighters"][e] for e in bodies}), len(bodies), (seed, key))
                self.assertEqual(on["arena_modes"][key], "mix")
            self.assertEqual(off["arena_modes"]["gehrman"], "swap")

    def test_every_boss_fights_once_per_run(self):
        """User, 10 Nov: no boss twice in a world, second phases counted (they are bosses of their own): a placed
        Gascoigne, Ludwig or Orphan carries its own second phase only while that phase has no arena, else a boss nobody
        fights, else fights alone; with second stages every one of them hands over to a boss nobody else fights."""
        import helpers
        from gamedata import Census
        from generate import world_plan
        table = helpers.FIGHTS
        worlds = [(seed, {"boss_swap": 1}, self.plan(seed, {"boss_swap": 1})) for seed in SEEDS]
        # second stages: as generate plans them, the chalice bosses filling the pool (user, 10 Nov)
        for seed in range(8):
            options = {"boss_swap": 1, "second_stage": 1}
            result = world_plan(self.census.files, Census(self.census.files), seed, options)[0]
            self.assertEqual(len(result["report"]["chalice_fill"]), 2, seed)  # the Emissary and the Failures stay home
            self.assertEqual(result["report"]["alone"], [], seed)
            worlds.append((seed, options, result))
        for seed, options, result in worlds:
            fighters = result["fighters"]
            by_origin = {f["origin"]: k for k, f in fighters.items()}
            seen = list(result["boss_fighters"].values())
            for key, fighter in fighters.items():
                fight = table.get(fighter["origin"])
                if fight is None or fight.stage is None:
                    continue
                if fighter.get("alone"):
                    self.assertEqual(fighter["extras"], [], (seed, key))
                elif fighter.get("stage"):
                    seen.append(fighter["stage"]["key"])
                else:  # its own second phase: never in an arena of its own too (seen stays distinct)
                    seen.append(by_origin.get(fight.stage, "%s:second" % key))
            self.assertEqual(sorted(seen), sorted(set(seen)), (seed, options))

    def test_arena_only_normal_is_the_main_body_mix_every_body(self):
        for seed in SEEDS:
            off = self.plan(seed, {"boss_swap": 1})
            on = self.plan(seed, dict(MIXES, boss_swap=1))
            for key in ("emissary", "failures"):
                arena = arenas.BY_KEY[key]
                self.assertEqual({e for e in off["bosses"] if arenas.arena_of(e) is arena}, {arena.main})
                self.assertEqual(off["arena_modes"][key], "normal")
                self.assertEqual({e for e in on["bosses"] if arenas.arena_of(e) is arena}, set(arena.bodies))
                self.assertEqual(on["arena_modes"][key], "mix")
                self.assertFalse([k for k, f in on["fighters"].items() if f["home"] == key])  # never a fighter elsewhere
            # the Emissary's small ones are never ordinary slots, whatever the chance
            busy = self.plan(seed, {"boss_swap": 1, "enemy_chance": 100})
            plain = self.plan(seed, {"enemy_chance": 100})
            for entity in EMISSARY_SMALL + list(arenas.BY_KEY["emissary"].extras):
                stem, slot = self.index[entity]
                self.assertNotIn(slot["part"], busy["changes"][stem])
                self.assertNotIn(slot["part"], plain["changes"][stem])

    def test_one_reborn_micolash_and_the_bell_maidens(self):
        fought = 0
        for seed in SEEDS:
            result = self.plan(seed, {"boss_swap": 1, "enemy_chance": 100})
            self.assertIn(2800800, result["bosses"])
            for entity in (2800801, 2800802, 2800803, 2600850) + MAIDENS:
                stem, slot = self.index[entity]
                self.assertNotIn(slot["part"], result["changes"][stem], (seed, entity))
            self.assertFalse([f for f in result["fighters"].values() if f["home"] == "micolash"])
            # more fighters than draws since the second phases joined (user, 8 Nov): not every world has it
            fought += "one_reborn" in result["boss_fighters"].values()
            free = self.plan(seed, {"boss_swap": 1, "enemy_chance": 100, "bell_maidens": 1})
            stem = "m28_00"
            for entity in MAIDENS:
                self.assertIn(self.index[entity][1]["part"], free["changes"][stem], (seed, entity))
        self.assertGreaterEqual(fought, len(SEEDS) // 2)  # it travels with its upper body and caster
        # bell_maidens 2: the difficult pool (difficult.candidates), with the bosses planned around them
        import difficult
        pool = difficult.candidates(self.census)
        for seed in SEEDS:
            result = self.plan(seed, {"boss_swap": 1, "bell_maidens": 2}, difficult=pool)
            for entity in MAIDENS:
                model, think, npc = result["changes"]["m28_00"][self.index[entity][1]["part"]]
                self.assertIn((think, npc), pool.get(model, ()), (seed, entity))
            self.assertIn(2800800, result["bosses"])

    def test_fighters_bring_their_extras_into_the_budget(self):
        brought = placed_with_models = 0
        for seed in SEEDS:
            result = self.plan(seed, {"boss_swap": 1})
            placed = set(result["boss_fighters"].values())
            fighters = result["fighters"]
            # a second body alone brings nothing; Ludwig's own second phase is his model, a stage boss is not
            placed_with_models += len({k for k in {"gascoigne", "logarius", "orphan", "one_reborn"} & placed
                                       if not fighters[k].get("alone")} | {k for k in placed if fighters[k].get("stage")})
            for entity, key in result["boss_fighters"].items():
                fighter = result["fighters"][key]
                stem = result["bosses"][entity][0]
                self.assertLessEqual(set(fighter["extra_models"]), result["chosen"][stem])
            brought += len({k for k in result["boss_fighters"].values() if result["fighters"][k]["extra_models"]})
        by_key = {f["key"]: f for f in planner.boss_fighters(self.census.slots, self.index, set())}
        self.assertEqual(by_key["logarius"]["extras"], [2500801, 2500802])
        self.assertEqual(by_key["logarius"]["extra_models"], ["c2321", "c9010"])
        self.assertEqual(by_key["orphan"]["extra_models"], ["c4541", "c4543"])
        self.assertEqual(by_key["gascoigne"]["extras"], [2410811])  # his beast form travels with him by default
        self.assertEqual(by_key["ludwig"]["extras"], [3400801])
        self.assertEqual(by_key["ludwig"]["extra_models"], [])  # his second body is his own model
        self.assertEqual(by_key["wet_nurse"]["extra_models"], [])  # her clone is her own model
        self.assertEqual(by_key["one_reborn"]["extra_models"], ["c5071", "c5072"])
        # Gascoigne, Logarius, the Orphan and the One Reborn bring models wherever they fight (not every world has
        # every boss: more fighters than draws since the second phases joined, user 8 Nov)
        self.assertEqual(brought, placed_with_models)
        self.assertGreaterEqual(placed_with_models, len(SEEDS))

    def test_ends_on_death_matches_the_defeat_events(self):
        from containers import dcx_read
        from emevd import Emevd
        import helpers
        files = self.census.files
        for arena in arenas.ARENAS_OF_GAME:
            events = Emevd(dcx_read(files.read("event/%s_00_00.emevd.dcx" % arena.stem))[0])
            for body in arena.bodies:
                defeat = helpers.arena_flags(body)[0]
                tested = any((bank, iid) in (helpers.CHAR_DEAD, helpers.HP_RATIO) and len(words) > 1 and words[1] == body
                             for bank, iid, words in events.instructions(defeat))
                self.assertEqual(arenas.ends_on_death(body), tested, body)

    def test_a_boss_ended_by_its_second_body_takes_an_ending_body(self):
        ended = Counter()
        for seed in SEEDS:
            for options in ({"boss_swap": 1}, {"boss_swap": 1, "witch_mix": 1, "shadows_mix": 1, "emissary_mix": 1, "failures_mix": 1, "bell_maidens": 2, "difficult_enemies": 1, "second_stage": 1}):
                result = self.plan(seed, options)
                for entity, key in result["boss_fighters"].items():
                    if result["fighters"][key].get("needs_end"):
                        ended[key, "witch_mix" in options] += 1
                        self.assertTrue(arenas.ends_on_death(entity), (seed, entity, key))
        # Gascoigne, Ludwig and the Orphan end on their second body, when they carry one (each boss once per run: with
        # every fight mixed no boss is free, so they fight alone)
        self.assertEqual({k for k, mixed in ended if not mixed}, {"gascoigne", "ludwig", "orphan"})

    def test_same_seed_same_plan(self):
        for options in ({"boss_swap": 1}, {"boss_swap": 1, "witch_mix": 1, "shadows_mix": 1, "emissary_mix": 1, "failures_mix": 1, "bell_maidens": 2, "difficult_enemies": 1, "second_stage": 1}):
            c = self.census
            a = planner.plan(c.slots, c.candidates, c.sizes, self.groups, self.limits, 7, options, c.animations)
            b = planner.plan(c.slots, c.candidates, c.sizes, self.groups, self.limits, 7, options, c.animations)
            self.assertEqual(a, b)
            other = planner.plan(c.slots, c.candidates, c.sizes, self.groups, self.limits, 8, options, c.animations)
            self.assertNotEqual(a["boss_fighters"], other["boss_fighters"])

    def test_pool_hooks_chalice_fighters_and_hard_rows(self):
        stand_in = {"key": "chalice_test", "model": "c2570", "think": self.index[2420800][1]["think"],
                    "npc": self.index[2420800][1]["npc"], "origin": None, "home": None, "extras": [], "extra_models": []}
        seen = 0
        for seed in SEEDS:
            result = self.plan(seed, {"boss_swap": 1, "chalice_bosses": 1}, extra_fighters=[stand_in])
            seen += "chalice_test" in result["fighters"]
        # 24 fighters for about 20 draws (second phases in the pool, one boss per mix-off fight, user 8 Nov)
        self.assertGreaterEqual(seen, len(SEEDS) // 2)
        rom = self.index[3200800][1]
        hard = {3200800: (rom["think"], rom["npc"] + 1)}
        result = self.plan(3, {"boss_swap": 1}, hard_rows=hard)
        self.assertEqual(result["fighters"]["rom"]["npc"], rom["npc"] + 1)
        home = self.plan(3, {}, hard_rows=hard)
        self.assertEqual(home["changes"]["m32_00"][rom["part"]], (rom["model"], rom["think"], rom["npc"] + 1))
        self.assertEqual(home["bosses"], {})

    def test_boss_hp_follows_the_lead_decisions(self):
        import generate
        c = self.census
        layout = c.npc_layout

        def hp(row):
            return layout.get(c.npc_rows[row], "hp")
        for options in ({"boss_swap": 1}, dict(MIXES, boss_swap=1), dict(MIXES, boss_swap=1, scaling=1)):
            result = self.plan(5, options)
            slot_row, rows = generate.clone_rows(c, result["changes"], result["options"], generate.boss_hp_rows(c, result))
            made = dict(rows)
            scaled = result["options"]["scaling"] == 0
            for entity, (stem, part) in result["bosses"].items():
                slot = self.index[entity][1]
                fighter = result["fighters"][result["boss_fighters"][entity]]
                got = layout.get(made[slot_row[(stem, part)]], "hp")
                arena = arenas.arena_of(entity)
                if arena.kind == arenas.ARENA_ONLY and result["arena_modes"][arena.key] == "mix":
                    want = hp(slot["npc"])  # own HP whatever the scaling, the main body's too (user: each its own HP)
                elif arena.key == "emissary" and entity == arena.main:
                    want = hp(self.index[2420800][1]["npc"]) if scaled else hp(fighter["npc"])  # D3, normal mode
                else:
                    want = hp(slot["npc"]) if scaled else hp(fighter["npc"])
                self.assertEqual(got, want, (options, entity))
        self.assertEqual([hp(self.index[e][1]["npc"]) for e in arenas.BY_KEY["failures"].bodies], [320, 256, 160, 160])
        self.assertEqual([hp(self.index[e][1]["npc"]) for e in arenas.BY_KEY["emissary"].bodies], [400] + [56] * 7)

    def test_a_mix_holds_one_boss_each_where_memory_allows(self):
        """Room for one new model in Forbidden Woods beside the Shadows' own (loaded until the last body is taken): every
        Shadow still gets a boss, sharing the first draw's model (the fallback, never vanilla) until the last body frees
        the Shadows' model; with room for many: three different bosses."""
        from seedcode import normalize, options_hash
        c = self.census
        norm = normalize({"boss_swap": 1, "shadows_mix": 1})
        units = [u for u in planner.arena_units(norm, self.index) if u[0].key == "shadows"]
        bodies = arenas.BY_KEY["shadows"].bodies
        fighters = planner.boss_fighters(c.slots, self.index, set())
        base = {stem: {s["model"] for s in v if s["entity"] not in bodies} for stem, v in c.slots.items()}
        for seed in SEEDS:
            for room in (2, 20):
                limits = planner.Limits(norm, *self.limits)
                limits.map_models, limits.map_bytes = len(base["m27_00"]) + room, 1 << 40
                placed, loaded = planner._plan_bosses(c.slots, self.index, units, fighters, c.sizes, limits, [], seed,
                                                      options_hash(norm), base)
                self.assertEqual(set(placed), set(bodies), (seed, room))
                models = {self.index_fighter(fighters, k)["model"] for k in placed.values()}
                if room == 2:
                    self.assertLessEqual(len(models), 2, (seed, placed))
                    model = {e: self.index_fighter(fighters, k)["model"] for e, k in placed.items()}
                    self.assertEqual(model[bodies[0]], model[bodies[1]], (seed, placed))  # body 3 still holds c2120
                else:
                    self.assertEqual(len(set(placed.values())), 3, (seed, placed))
                self.assertLessEqual(len(loaded["m27_00"]), limits.map_models)

    @staticmethod
    def index_fighter(fighters, key):
        return next(f for f in fighters if f["key"] == key)

    def test_two_orphans_in_one_map_each_carry_their_own_hp(self):
        """The Orphan's travelling phase two takes phase one's damage: its row must carry the HP of its own boss body
        (Shadows 1425 / 900 / 800), also when two Orphans share a map."""
        import generate
        c = self.census
        layout = c.npc_layout
        orphan = next(f for f in planner.boss_fighters(c.slots, self.index, set()) if f["key"] == "orphan")
        bodies = arenas.BY_KEY["shadows"].bodies
        changes = {stem: {} for stem in c.slots}
        result = {"bosses": {}, "arena_modes": {"shadows": "mix"}, "boss_fighters": {}}
        for e in bodies:
            stem, slot = self.index[e]
            changes[stem][slot["part"]] = (orphan["model"], orphan["think"], orphan["npc"])
            result["bosses"][e] = (stem, slot["part"])
        options = __import__("seedcode").normalize({"boss_swap": 1, "shadows_mix": 1})
        slot_row, rows = generate.clone_rows(c, changes, options, generate.boss_hp_rows(c, result))
        placed = {e: "orphan" for e in bodies}
        got = generate.helper_rows(c, "m27_00", placed, {"orphan": orphan}, slot_row, rows)
        made = dict(rows)
        for e in bodies:
            want = layout.get(c.npc_rows[self.index[e][1]["npc"]], "hp")
            self.assertEqual(layout.get(made[got[(e, 3600801)]], "hp"), want, e)
        self.assertEqual(len({got[(e, 3600801)] for e in bodies}), 3)
        self.assertEqual(got[3600801], got[(min(bodies), 3600801)])  # the plain key: what helpers.add_parts reads today
        # the bullet dummy (not same_hp) travels too: its own HP, the strength of its boss body's row
        for e in bodies:
            dummy = made[got[(e, 3600803)]]
            self.assertEqual(layout.get(dummy, "hp"), layout.get(c.npc_rows[454300], "hp"))
            boss = made[slot_row[(self.index[e][0], self.index[e][1]["part"])]]
            for name in generate.SLOT_STRENGTH:
                self.assertEqual(layout.get(dummy, name), layout.get(boss, name), (e, name))

    def _travelling_rows(self, scaling):
        """Logarius on Amelia's body (m24_00): the rows of his sword and bullet owner, and of his boss body."""
        import generate
        import seedcode
        c = self.census
        logarius = next(f for f in planner.boss_fighters(c.slots, self.index, set()) if f["key"] == "logarius")
        stem, slot = self.index[2400800]
        changes = {s: {} for s in c.slots}
        changes[stem][slot["part"]] = (logarius["model"], logarius["think"], logarius["npc"])
        result = {"bosses": {2400800: (stem, slot["part"])}, "arena_modes": {}, "boss_fighters": {}}
        options = seedcode.normalize({"boss_swap": 1, "scaling": scaling})
        slot_row, rows = generate.clone_rows(c, changes, options, generate.boss_hp_rows(c, result))
        got = generate.helper_rows(c, stem, {2400800: "logarius"}, {"logarius": logarius}, slot_row, rows)
        return got, dict(rows), dict(rows)[slot_row[(stem, slot["part"])]]

    def test_travelling_bodies_take_the_strength_of_their_boss_row(self):
        """Not only same_hp bodies: every body that travels takes SLOT_STRENGTH of its boss body's new row (slot
        scaling: Amelia's area strength, not m25's), and keeps its own HP."""
        import generate
        c = self.census
        layout = c.npc_layout
        got, made, boss = self._travelling_rows(0)
        for home, npc in ((2500801, 232100), (2500802, 232000)):
            row = made[got[(2400800, home)]]
            self.assertEqual(layout.get(row, "hp"), layout.get(c.npc_rows[npc], "hp"), home)
            for name in generate.SLOT_STRENGTH:
                self.assertEqual(layout.get(row, name), layout.get(boss, name), (home, name))
        home_effect = layout.get(c.npc_rows[232100], "spEffectID6")
        self.assertNotEqual(layout.get(boss, "spEffectID6"), home_effect)  # the test means something: m24 is not m25
        self.assertEqual(got[2500801], got[(2400800, 2500801)])

    def test_travelling_bodies_keep_their_home_row_when_it_already_has_that_strength(self):
        """Own-strength scaling: the boss row brings Logarius' own strength, which his bodies' rows have: no new row."""
        got, made, boss = self._travelling_rows(1)
        self.assertEqual((got[(2400800, 2500801)], got[(2400800, 2500802)]), (232100, 232000))


if __name__ == "__main__":
    unittest.main()
