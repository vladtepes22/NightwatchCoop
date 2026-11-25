"""Boss round 2, POOL part: chalice fighters, hard rows, the chalice AI archive as a script source, difficult enemies.
Run from tools/randomizer: python -m unittest discover -s tests. Reads (never writes) the game at BB_GAME."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import ailua  # noqa: E402
import chalice  # noqa: E402
import difficult  # noqa: E402
import planner  # noqa: E402
from containers import bnd4_read  # noqa: E402

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")

# survey 3 (4 Nov 2026): the difficult slots at ratio 3, as (model, think, npc) pool triples (scratchpad survey3\difficult.json)
SURVEY_DIFFICULT = {
    ("c1000", 100000, 100010), ("c1000", 100020, 100020), ("c1001", 100100, 100100), ("c1270", 127000, 127800),
    ("c1270", 127400, 127500), ("c1271", 127600, 127600), ("c2020", 202000, 202000), ("c2020", 202000, 202006),
    ("c2110", 211000, 211000), ("c2170", 217001, 217001), ("c2170", 217001, 217002),
    ("c2170", 217001, 217003), ("c2640", 264401, 264400), ("c2640", 264401, 264401), ("c2640", 264410, 264410),
    ("c2700", 270351, 270350), ("c2700", 270353, 270350), ("c2730", 273400, 273400), ("c4000", 400010, 400010),
    ("c4000", 400014, 400010), ("c4000", 400017, 400010), ("c4000", 400019, 400010), ("c4000", 400020, 400020),
    ("c4000", 400022, 400022), ("c4000", 400025, 400025), ("c4010", 401000, 401000), ("c4010", 401011, 401010),
    ("c4060", 406000, 406000), ("c4060", 406010, 406010), ("c4060", 406015, 406015),
    ("c4120", 402000, 412000), ("c4120", 402001, 412001), ("c4130", 213050, 413000),
    # the census pins the Celestial Emissary's seven small ones (boss round 2 plan): Upper Cathedral Ward's median of the
    # regular enemies falls to 53 HP and these 160 HP rows reach three times it
    ("c2700", 270200, 270200), ("c2700", 270220, 270220), ("c2700", 270250, 270250)}
BELL_MAIDENS = (2800520, 2800522, 2800524, 2800525, 2800527, 2800529)


@needs_game
class Pool(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from gamedata import Census, GameFiles
        from neighbours import group_limits, load_groups
        cls.files = GameFiles(GAME)
        cls.census = Census(cls.files)
        cls.groups = load_groups()
        cls.limits = group_limits(cls.census, cls.groups)
        cls.hard = difficult.candidates(cls.census)

    def plan(self, seed, options, slots=None, hard=True):
        c = self.census
        return planner.plan(slots or c.slots, c.candidates, c.sizes, self.groups, self.limits, seed, options, c.animations,
                            difficult=self.hard if hard else None)

    # ---- chalice
    def test_chalice_fighters_are_the_seven_with_their_deepest_rows(self):
        from gamedata import GameFiles, Census
        files = GameFiles(GAME)
        census = Census(files)
        before = files.input_hash()
        got = chalice.fighters(files, census)
        self.assertEqual([(f["model"], f["think"], f["npc"]) for f in got],
                         [("c5090", 509000, 310509006), ("c3050", 305000, 310305006), ("c3050", 305010, 310305016),
                          ("c5040", 504000, 310504006), ("c5080", 508090, 310508006), ("c3060", 306001, 210306016),
                          ("c5010", 501000, 310501006)])
        for f in got:
            self.assertIsNone(f["home"])
            self.assertIsNone(f["origin"])
            self.assertEqual(set(f) >= {"key", "model", "think", "npc", "origin", "home", "extras", "extra_models"}, True)
            self.assertGreater(census.sizes[f["model"]], 10 << 20)  # every one is a real 13-21 MB model
            self.assertIn(0, census.animations[f["model"]])
        self.assertNotEqual(files.input_hash(), before)  # the chalice models now count as inputs of this world
        self.assertFalse([k for k in files.read_hashes if "mapstudio/m29" in k or k.startswith("event/m29")])

    def test_chalice_pool_in_and_out_by_name_and_model(self):
        """The user's list (4 Nov) against the game text: each fighter's bar names it, and no row of an OUT boss
        (NpcParam nameId) shares a fighter's model; no player model."""
        names = npc_names(self.files)
        layout = self.census.npc_layout
        fighters = chalice.fighters(self.files, self.census)
        self.assertEqual([names[f["bar_name"]] for f in fighters],
                         ["Bloodletting Beast", "Pthumerian Descendant", "Pthumerian Elder", "Abhorrent Beast",
                          "Loran Darkbeast", "Yharnam, Pthumerian Queen", "Watchdog of the Old Lords"])
        self.assertEqual([f["name"] for f in fighters], [names[f["bar_name"]] for f in fighters])
        out_names = {"Undead Giant", "Maneater Boar", "Merciless Watcher", "Watcher Chieftain", "Brainsucker",
                     "Keeper of the Old Lords", "Beast-possessed Soul"}
        out_ids = {i for i, t in names.items() if t in out_names}
        self.assertEqual({names[i] for i in out_ids}, out_names)
        out_models = {"c%04d" % (layout.get(row, "behaviorVariationId") // 10) for row in self.census.npc_rows.values()
                      if layout.get(row, "nameId") in out_ids}
        self.assertTrue(out_models)
        models = {f["model"] for f in fighters}
        self.assertFalse(models & out_models, models & out_models)
        self.assertNotIn("c0000", models)
        for f in fighters:
            self.assertNotIn(layout.get(self.census.npc_rows[f["npc"]], "nameId"), out_ids, f["key"])

    def test_chalice_rows_are_the_deepest_the_dungeons_place(self):
        """Every chalice fighter and hard copy takes, of the rows the real chalices (a chalice item names the dungeon)
        place with its model and AI, the one with the strongest layer effect (spEffectID6 7300 < 7310 < 7320), and a
        Defiled one (6401 Cursed and Defiled Root, 6409 Defiled) where it has any."""
        from containers import dcx_read
        from msb import Msb
        goods = item_names(self.files, 10)
        root = os.path.join(GAME, "dvdroot_ps4", "map", "mapstudio")
        wanted = {b[2] for b in chalice.BOSSES} | {v[0] for v in chalice.HARD.values()}
        placed = {}
        for folder in sorted(os.listdir(root)):
            if not folder.startswith("m29_") or not os.path.isdir(os.path.join(root, folder)):
                continue
            for name in sorted(os.listdir(os.path.join(root, folder))):
                cup = 6000 + int(name[4:6]) * 10 + int(name[7])  # m29_AA_BB_xx (survey 3); debug maps name no chalice
                if not name.endswith(".msb.dcx") or not goods.get(cup):
                    continue
                with open(os.path.join(root, folder, name), "rb") as f:
                    for e in Msb(dcx_read(f.read())[0]).enemies():
                        if e["model"] in wanted:
                            placed.setdefault((e["model"], e["think"]), {}).setdefault(e["npc"], set()).add(cup)
        self.assertEqual(goods[6401], "Cursed and Defiled Root Chalice")
        self.assertEqual(goods[6409], "Defiled Chalice")
        layout = self.census.npc_layout
        picks = [(key, model, think, npc) for key, _, model, think, npc, _ in chalice.BOSSES]
        picks += [(opt, model, think, npc) for model, think, npc, opt in chalice.HARD.values()]
        for key, model, think, npc in picks:
            rows = placed.get((model, think), {})
            self.assertIn(npc, rows, key)
            defiled = {n for n, cups in rows.items() if cups & {6401, 6409}}
            if defiled:
                self.assertIn(npc, defiled, key)
            pool = defiled or set(rows)
            strength = {n: layout.get(self.census.npc_rows[n], "spEffectID6") for n in pool}
            self.assertEqual(strength[npc], max(strength.values()), (key, strength))

    def test_option_ids_agree_across_the_generator_the_command_line_and_the_launcher(self):
        """Options 25-53: seedcode, the CLI flags (run as the launcher runs them), WorldCode.cs and Randomizer.cs."""
        import re
        import subprocess
        import seedcode
        round2 = [o for o in seedcode.OPTIONS if 25 <= o[0] <= 53
                  and o[1] not in ("boss_enemy_chance", "boss_enemy_max", "boss_enemy_hp", "include_only")]
        self.assertEqual([o[1] for o in round2], list(seedcode.BOOL_OPTIONS_R2[:4]) + ["bell_maidens"]
                         + list(seedcode.BOOL_OPTIONS_R2[4:]))  # 25 (hard mode) was removed
        self.assertTrue({o[0] for o in round2} <= set(seedcode.EXTRAS))
        cli = os.path.join(os.path.dirname(HERE), "randomizer.py")

        def code(*flags):
            out = subprocess.run([sys.executable, "-I", cli, "code", "--seed", "9", "--bosses"] + list(flags),
                                 capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            return seedcode.decode(out.stdout.strip())[1]
        flags = {name: "--" + name.replace("_", "-") for _, name, _, _ in round2 if name != "bell_maidens"}
        for name, flag in flags.items():
            got = code(flag)
            self.assertEqual(got[name], 1, flag)
            self.assertEqual(got["bell_maidens"], 0)
        for value, word in enumerate(("off", "normal", "difficult")):
            self.assertEqual(code("--bell-maidens", word)["bell_maidens"], value)
        core = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), "launcher", "windows",
                            "BloodborneLauncher", "Core")
        world = os.path.join(core, "WorldCode.cs")
        if not os.path.exists(world):
            self.skipTest("no launcher sources beside the generator")
        with open(world, encoding="utf-8") as f:
            text = f.read()
        extras = {int(i): (n, int(d)) for i, n, d in re.findall(r"\((\d+), \"(\w+)\", '\w', (-?\d+)\)", text)}
        for option_id, name, _, default in round2:
            self.assertEqual(extras.get(option_id), (name, default), option_id)
        self.assertEqual(re.findall(r"\"(\w+)\"", re.search(r"Round2Bools = \{([^}]*)\}", text).group(1)),
                         list(seedcode.BOOL_OPTIONS_R2))
        with open(os.path.join(core, "Randomizer.cs"), encoding="utf-8") as f:
            launcher = f.read()
        for _, name, _, _ in round2:  # the launcher writes codes itself (Randomizer.ToCode, since 5 Nov), no flags
            self.assertIn('c["%s"] = ' % name, launcher, name)
        # bosses as enemies' settings (46-48, user 8 Nov): in the code, the CLI and the launcher alike
        later = [o for o in seedcode.OPTIONS if 46 <= o[0] <= 49]
        self.assertEqual([o[1] for o in later], ["boss_enemy_chance", "boss_enemy_max", "huge_bosses_as_enemies", "boss_enemy_hp"])
        for option_id, name, _, default in later:
            self.assertIn(option_id, seedcode.EXTRAS)
            self.assertEqual(extras.get(option_id), (name, default), option_id)
            self.assertIn('c["%s"] = ' % name, launcher, name)
        got = code("--bosses-as-enemies", "--boss-enemy-chance", "60", "--boss-enemy-max", "4", "--huge-bosses-as-enemies")
        self.assertEqual((got["boss_enemy_chance"], got["boss_enemy_max"], got["huge_bosses_as_enemies"]), (60, 4, 1))
        self.assertEqual(code("--bosses-as-enemies", "--boss-enemy-hp", "14")["boss_enemy_hp"], 14)

    def test_chalice_scripts_come_from_the_chalice_archive_into_a_main_map(self):
        from gamedata import GameFiles, Census
        from containers import bnd4_write
        import generate
        files = GameFiles(GAME)
        census = Census(files)
        self.assertNotIn("script/m29_00_00_00.luabnd.dcx", files.read_hashes)  # not read until a script needs it
        fighters = chalice.fighters(files, census)
        changes = {"c5100_0000": (f["model"], f["think"], f["npc"]) for f in fighters[:1]}
        changes.update({"x%d" % i: (f["model"], f["think"], f["npc"]) for i, f in enumerate(fighters)})
        bnd, missing = generate.merged_archive(census, "m32_00", changes, {})
        self.assertEqual(missing, [])
        self.assertIn("script/m29_00_00_00.luabnd.dcx", files.read_hashes)
        merged = bnd4_read(bnd)
        names = {e[2].replace("/", "\\").rsplit("\\", 1)[1]: e for e in merged["entries"]}
        info = ailua.info_read(names["m32_00_00_00.luainfo"][3])[2]
        gnl = set(ailua.gnl_read(names["m32_00_00_00.luagnl"][3]))
        for f in fighters:
            battle, logic = census.goals(f["think"])
            file_name = ailua.script_name(battle, "battle")
            self.assertIn(file_name, names)
            self.assertIn(file_name, census.chalice_script_files)
            self.assertTrue([r for r in info if r[0] == battle and r[1] == generate.BATTLE], file_name)
            source = ailua.Archive(chalice.ARCHIVE_STEM + "_00_00", chalice.archive(census))
            self.assertLessEqual(set(source.globals_of(names[file_name][3])), gnl, file_name)
            if logic > 0:  # copied, unless every map loads it already (aicommon: census.common_scripts)
                logic_name = ailua.script_name(logic, "logic")
                self.assertTrue(logic_name in names or logic_name in census.common_scripts, logic_name)
        # unchanged index files round-trip, so the archive is still well formed; the chalice archive was never touched
        self.assertEqual(bnd4_write(bnd4_read(bnd)), bnd)
        original = bnd4_read(dcx_payload(files, "script/m29_00_00_00.luabnd.dcx"))
        self.assertEqual(original["entries"], census.chalice_archive["entries"])
        self.assertNotIn("m29_00", census.archives)  # no loop over census.archives (outputs) ever sees it

    def test_a_moved_boss_brings_its_bullet_effects(self):
        """The One Reborn (no pool row) in Gascoigne's arena: its row's bullet effect f000650701 goes into m24's archive
        (7 Nov: it was missing, the projectiles were invisible)."""
        import effects
        from containers import dcx_read
        changes = {"m24_01_00_00": {"c2710_0000": ("c5070", 0, 507000)}}
        out, report = effects.plan(self.files, self.census, changes)
        names = {e[2].split("\\")[-1] for e in bnd4_read(dcx_read(out["sfx/frpg_sfxbnd_m24.ffxbnd.dcx"])[0])["entries"]}
        self.assertIn("f000650701.fxr", names)

    def test_a_moved_enemy_brings_its_weapon_trail_and_speffect_looks(self):
        """8 Nov: the chalice Keeper of the Old Lords (c2160) lost the fire on its swings away from the chalice: its TAE
        draws the trail f000621600 with SpawnFFX_Blade (type 118), only in frpg_sfxbnd_m29b. The Fishing Hamlet
        caster's row 405000 carries SpEffect 8024, whose vfx row 346 loops f000640520 (only in m36)."""
        import effects
        from containers import dcx_read
        changes = {"m24_01_00_00": {"c2500_0000": ("c2160", 216080, 10216000), "c2500_0001": ("c4050", 405000, 405000)}}
        out, report = effects.plan(self.files, self.census, changes)
        names = {e[2].split("\\")[-1] for e in bnd4_read(dcx_read(out["sfx/frpg_sfxbnd_m24.ffxbnd.dcx"])[0])["entries"]}
        self.assertTrue({"f000621600.fxr", "f000640520.fxr", "f000621620.fxr"} <= names)

    def test_hard_rows_keep_the_main_model(self):
        rows = chalice.hard_rows(self.files, self.census)
        self.assertEqual(rows, {3200800: (510090, 310510096), 3300800: (512090, 310512096), 2420800: (251090, 310251096)})
        layout = self.census.npc_layout
        by_entity = {s["entity"]: (stem, s) for stem, v in self.census.slots.items() for s in v}
        for entity, (think, npc) in rows.items():
            stem, main = by_entity[entity]
            self.assertEqual(layout.get(self.census.npc_rows[npc], "behaviorVariationId"),
                             layout.get(self.census.npc_rows[main["npc"]], "behaviorVariationId"))
            battle, _ = self.census.goals(think)
            self.assertEqual(battle, self.census.goals(main["think"])[0])  # the main game's own battle script
            self.assertIn(ailua.script_name(battle, "battle"), self.census.script_files[stem])

    # ---- difficult enemies
    def test_difficult_candidates_are_the_surveyed_list(self):
        got = {(m, t, n) for m, rows in self.hard.items() for t, n in rows}
        self.assertEqual(got, SURVEY_DIFFICULT)
        report = difficult.rows(self.census)
        self.assertTrue(all(r["hp"] >= 3 * r["median"] for r in report))
        self.assertFalse([r for r in report if r["model"] in ("c2561", "c0000")])
        self.assertEqual(difficult.list, difficult.rows)
        stricter = difficult.candidates(self.census, 3.5)
        self.assertLess(sum(map(len, stricter.values())), len(got))

    def test_difficult_option_off_changes_nothing(self):
        for seed in (1, 7, 123):
            base = planner.plan(self.census.slots, self.census.candidates, self.census.sizes, self.groups, self.limits,
                                seed, {}, self.census.animations)
            with_list = self.plan(seed, {})
            self.assertEqual(base["changes"], with_list["changes"])
            self.assertEqual(base["report"], with_list["report"])

    def test_difficult_option_on_draws_difficult_models_where_they_fit(self):
        hard_models = set(self.hard)
        for seed in (1, 7, 123):
            off = self.plan(seed, {"enemy_chance": 100})
            on = self.plan(seed, {"enemy_chance": 100, "difficult_enemies": 1})
            self.assertEqual(planner.check(on, self.census.slots, self.census.candidates, self.census.sizes, self.groups,
                                           self.limits, self.census.animations), [])
            n_on = sum(1 for v in on["changes"].values() for c in v.values() if (c[0], c[1], c[2]) in SURVEY_DIFFICULT)
            n_off = sum(1 for v in off["changes"].values() for c in v.values() if (c[0], c[1], c[2]) in SURVEY_DIFFICULT)
            changed = sum(len(v) for v in on["changes"].values())
            self.assertEqual(on["report"]["difficult"], n_on)
            self.assertGreater(n_on, changed // 2, seed)  # most replaced slots draw a difficult one
            self.assertGreater(n_on, 3 * n_off, seed)
            # a slot that did not get one: no difficult model of its map fits it (its forced animations)
            for stem, v in on["changes"].items():
                picked = {c[0] for c in v.values()} & hard_models
                by_part = {s["part"]: s for s in self.census.slots[stem]}
                for part, c in v.items():
                    if c[0] not in hard_models and picked:
                        needs = by_part[part]["needs"]
                        self.assertTrue(needs and not [m for m in picked
                                                       if all(a in self.census.animations.get(m, ()) for a in needs)],
                                        (stem, part))
            print("difficult_enemies seed %d: %d of %d replaced slots difficult (off: %d)" % (seed, n_on, changed, n_off))
        self.assertEqual(self.plan(7, {"enemy_chance": 100, "difficult_enemies": 1})["changes"],
                         self.plan(7, {"enemy_chance": 100, "difficult_enemies": 1})["changes"])
        self.assertIn("difficult", self.plan(7, {"enemy_chance": 100, "difficult_enemies": 1})["report"])

    def test_bell_maidens_from_the_difficult_pool(self):
        slots = {stem: list(v) for stem, v in self.census.slots.items()}
        for mode in (0, 1, 2):
            slots["m28_00"] = [planner.escort_slot(s, mode) if s["entity"] in BELL_MAIDENS else s
                               for s in self.census.slots["m28_00"]]
            parts = {s["part"] for s in slots["m28_00"] if s["entity"] in BELL_MAIDENS}
            self.assertEqual(len(parts), 6)
            result = self.plan(5, {"enemy_chance": 0}, slots)
            got = {p: c for p, c in result["changes"]["m28_00"].items() if p in parts}
            if mode == 0:
                self.assertEqual(got, {})
                continue
            self.assertEqual(set(got), parts)  # always replaced, even at enemy chance 0
            hard = [c for c in got.values() if tuple(c) in SURVEY_DIFFICULT]
            if mode == 2:
                self.assertEqual(len(hard), 6)
            print("bell maidens mode %d: %s" % (mode, sorted(c[0] for c in got.values())))
        self.assertEqual(self.plan(5, {"enemy_chance": 0}, slots)["changes"],
                         self.plan(5, {"enemy_chance": 0}, slots)["changes"])  # determinism


# option chalice_enemies (8 Nov 2026 survey of the vanilla game; chalice_enemies.py): the Chalice Dungeons' kinds that
# join the pool, and why some others do not
CHALICE_KINDS = {"c1010", "c1150", "c1160", "c1210", "c1230", "c1300", "c2010", "c2040", "c2050", "c2080", "c2160",
                 "c3000", "c3010", "c3020", "c3030", "c3040", "c3120"}
CHALICE_LEFT_OUT = {"c0000": "human", "c3050": "boss", "c5010": "boss", "c5040": "boss", "c5090": "boss",
                    "c2090": "home only", "c2130": "home only", "c7500": "team", "c3061": "invisible", "c1211": "scripted",
                    "c2501": "turret", "c9010": "dummy", "c2510": "main boss", "c3100": "main kind"}


@needs_game
class ChaliceEnemies(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import chalice_enemies
        from gamedata import Census, GameFiles
        from neighbours import group_limits, load_groups
        cls.files = GameFiles(GAME)
        cls.census = Census(cls.files)
        cls.read_before = set(cls.files.read_hashes)
        cls.survey = chalice_enemies.survey(cls.files, cls.census)
        cls.main_candidates = {m: list(v) for m, v in cls.census.candidates.items()}
        cls.census.add_candidates(cls.survey["rows"])
        cls.groups = load_groups()
        cls.limits = group_limits(cls.census, cls.groups)

    def test_the_maps_read_are_the_dungeons_a_chalice_opens(self):
        """chalice_enemies.maps (EquipParamGoods has the cup's row) against the game's text: every map of a cup with an
        English chalice name and an event file of its own, no other."""
        import chalice_enemies
        goods = item_names(self.files, 10)
        root = os.path.join(GAME, "dvdroot_ps4", "map", "mapstudio")
        named = set()
        for folder in sorted(os.listdir(root)):
            if not folder.startswith("m29_") or not os.path.isdir(os.path.join(root, folder)):
                continue
            for name in sorted(os.listdir(os.path.join(root, folder))):
                stem = name[:-len(".msb.dcx")]
                if goods.get(chalice_enemies.cup(name), "").endswith("Chalice") and os.path.isfile(
                        os.path.join(GAME, "dvdroot_ps4", "event", folder, stem + ".emevd.dcx")):
                    named.add((folder, stem))
        self.assertEqual(set(chalice_enemies.maps(self.files)), named)
        self.assertEqual(self.survey["maps"], len(named))
        self.assertEqual(len(named), 2310)

    def test_the_kinds_that_join_and_the_ones_left_out(self):
        rows = self.survey["rows"]
        self.assertEqual(set(rows), CHALICE_KINDS)
        for model, reason in CHALICE_LEFT_OUT.items():
            self.assertEqual(self.survey["excluded"].get(model), reason, model)
        # every chalice boss of option chalice_bosses and every hard row's model stays out (a separate, older list)
        for model in {b[2] for b in chalice.BOSSES} | {v[0] for v in chalice.HARD.values()}:
            self.assertIn(model, self.survey["excluded"], model)
            self.assertNotIn(model, rows)
        c, layout = self.census, self.census.npc_layout
        teams = {layout.get(c.npc_rows[npc], "teamType") for v in self.main_candidates.values() for _, npc in v}
        scripts = set().union(c.common_scripts, c.chalice_script_files, *c.script_files.values())
        for model, pairs in rows.items():
            self.assertNotIn(model, self.main_candidates)  # the main kinds keep their own rows
            self.assertTrue(pairs, model)
            for think, npc in pairs:
                row = c.npc_rows[npc]
                self.assertEqual(layout.get(row, "behaviorVariationId") // 10, int(model[1:]), (model, npc))
                self.assertLess(layout.get(row, "hp"), 2000)
                self.assertIn(layout.get(row, "teamType"), teams)
                self.assertFalse(6000 <= think < 7000)
                battle, logic = c.goals(think, npc)
                self.assertIn("%06d_battle.lua" % battle, scripts)
                if logic > 0:
                    self.assertIn("%06d_logic.lua" % logic, scripts)
            # the weakest layer of each think only
            strength = {}
            for think, npc in pairs:
                strength.setdefault(think, set()).add(layout.get(c.npc_rows[npc], "spEffectID6"))
            self.assertTrue(all(len(v) == 1 for v in strength.values()), model)
        # a kind with a boss variant keeps its regular rows only: the Watchers' trio rows (304090 bar) never join
        self.assertNotIn(304090, {think for think, _ in rows["c3040"]})
        bars, others = self.survey["placements"]["c3040"]
        self.assertGreater(others, bars)
        print("chalice_enemies: %d kinds, %d rows; left out %s" % (len(rows), sum(map(len, rows.values())),
                                                                   sorted(self.survey["excluded"].items())))

    def test_the_chalice_maps_count_as_inputs_only_when_read(self):
        self.assertFalse([k for k in self.read_before if "mapstudio/m29" in k or k.startswith("event/m29")])
        read = [k for k in self.files.read_hashes if k.startswith("map/mapstudio/m29_")]
        self.assertEqual(len(read), self.survey["maps"])

    def test_add_candidates_refuses_a_main_kind(self):
        with self.assertRaises(ValueError):
            self.census.add_candidates({"c1100": [(110000, 110000)]})

    def test_plans_draw_chalice_kinds_and_keep_every_rule(self):
        import generate
        c = self.census
        for seed in (1, 7, 123):
            off = planner.plan(c.slots, self.main_candidates, c.sizes, self.groups, self.limits, seed, {}, c.animations)
            on = planner.plan(c.slots, c.candidates, c.sizes, self.groups, self.limits, seed,
                              {"chalice_enemies": 1}, c.animations)
            self.assertEqual(planner.check(on, c.slots, c.candidates, c.sizes, self.groups, self.limits, c.animations),
                             [])
            drawn = [p for v in on["changes"].values() for p in v.values() if p[0] in CHALICE_KINDS]
            self.assertFalse([p for v in off["changes"].values() for p in v.values() if p[0] in CHALICE_KINDS])
            self.assertGreater(len(drawn), 50, seed)
            # every map's AI archive finds the new kinds' scripts (main maps, aicommon, else the chalice archive)
            for stem, v in on["changes"].items():
                _, missing = generate.merged_archive(c, stem, v, {})
                self.assertEqual(missing, [], stem)
            print("chalice_enemies seed %d: %d of %d replaced slots are chalice kinds (%d kinds)"
                  % (seed, len(drawn), sum(map(len, on["changes"].values())), len({p[0] for p in drawn})))
        self.assertEqual(planner.plan(c.slots, c.candidates, c.sizes, self.groups, self.limits, 7, {"chalice_enemies": 1},
                                      c.animations)["changes"],
                         planner.plan(c.slots, c.candidates, c.sizes, self.groups, self.limits, 7, {"chalice_enemies": 1},
                                      c.animations)["changes"])


def item_names(files, number):
    from containers import dcx_read, fmg_read
    entries = bnd4_read(dcx_read(files.read("msg/engus/item.msgbnd.dcx"))[0])["entries"]
    texts = {e[1]: fmg_read(e[3])["texts"] for e in entries}
    return {k: v.strip() for k, v in texts.get(number, {}).items() if v}


def npc_names(files):
    return item_names(files, 18)


def dcx_payload(files, rel):
    from containers import dcx_read
    return dcx_read(files.read(rel))[0]


if __name__ == "__main__":
    unittest.main()
