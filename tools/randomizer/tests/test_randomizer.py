"""Run: python -I -m unittest discover -s tools/randomizer/tests
Pure tests need nothing. Tests marked by `needs_game` read (never write) the game at BB_GAME."""
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)
import ailua
import planner
import seedcode
from emevd import LABEL_BANK, Emevd
from containers import Layout, Param, bnd4_find, bnd4_read, bnd4_write, dcx_read, fmg_read, fmg_write
from msb import MODELS, PARTS, Msb, sections

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")


class BossEvents(unittest.TestCase):
    def _events(self, stem):
        from gamedata import GameFiles
        return Emevd(dcx_read(GameFiles(GAME).read("event/%s_00_00.emevd.dcx" % stem))[0])

    @needs_game
    def test_growing_a_file_keeps_every_other_event(self):
        events = self._events("m24_00")
        before = {e: (events.instructions(e), events.params(e), events.restart(e)) for e in events.event_ids()}
        caller = 50
        n = len(before[caller][0])
        events.add_event(12404890, 0, [(2003, 2, [12404891, 1]), (1014, 3, [])])
        events.append_instruction(caller, 2000, 0, [0, 12404890, 0])
        events.silence(12404807)
        again = Emevd(events.bytes())
        self.assertEqual(struct.unpack_from("<I", again.bytes(), 0xc)[0], len(again.bytes()))
        self.assertEqual(again.instructions(12404890), [(2003, 2, (12404891, 1)), (1014, 3, ())])
        self.assertEqual(again.event_ids(), sorted(again.event_ids()))
        self.assertEqual(again.instructions(caller)[:n], before[caller][0])
        self.assertEqual(again.instructions(caller)[n], (2000, 0, (0, 12404890, 0)))
        self.assertEqual(len(again.instructions(12404807)), 1)
        for e, (ins, params, restart) in before.items():
            if e not in (caller, 12404807):
                self.assertEqual((again.instructions(e), again.params(e), again.restart(e)), (ins, params, restart), e)

    @needs_game
    def test_phase_events_found_in_vanilla(self):
        import boss_events
        self.assertEqual(boss_events.phase_events(self._events("m24_00"), 2400800), [12404807])
        self.assertEqual(boss_events.phase_events(self._events("m23_00"), 2300800), [12304807, 12304808])
        self.assertEqual(boss_events.phase_events(self._events("m35_00"), 3500800), [])

    @needs_game
    def test_swapped_arenas_open_as_on_a_second_attempt(self):
        import boss_events
        # the first-meeting event each fog wall waits for (DATA, vanilla); the Dream's story starts are left alone
        intros = {2300800: 12301802, 2300810: 12301702, 2400800: 12401802, 2410800: 12411702, 2420800: 12421802,
                  3300800: 13301802, 3400850: 13401851, 3500800: 13501801}
        for arena, intro in intros.items():
            stem = "m%02d_%02d" % (arena // 100000, arena // 10000 % 10)
            events = self._events(stem)
            self.assertEqual(boss_events.intro_event(events, arena), intro, arena)
            others = {e: events.instructions(e) for e in events.event_ids() if e != intro}
            self.assertEqual(boss_events.retry_starts(events, {arena}), [(arena, intro)])
            again = Emevd(events.bytes())
            self.assertEqual(again.instructions(intro), [(2003, 2, (intro, 1))])
            self.assertEqual({e: again.instructions(e) for e in again.event_ids() if e != intro}, others)
        self.assertEqual(boss_events.retry_starts(self._events("m21_00"), {2100800, 2100810}), [])
        self.assertIsNone(boss_events.intro_event(self._events("m32_00"), 3200800))  # no fog wall: a walk-in

    @needs_game
    def test_a_swapped_arenas_health_bar_names_the_new_boss(self):
        import boss_events
        # each boss's bar name in its own map (DATA, vanilla): one bar each, the name written in, not a parameter
        names = {2100800: 804000, 2100810: 540000, 2300800: 209000, 2300810: 508000, 2400800: 502000, 2410800: 500000,
                 2420800: 251000, 3200800: 510000, 3300800: 512000, 3400850: 450000, 3500800: 452000}
        stem = lambda e: "m%02d_%02d" % (e // 100000, e // 10000 % 10)
        for entity, name in names.items():
            self.assertEqual(boss_events.bar_name(self._events(stem(entity)), entity), name, entity)
        arena = self._events("m34_00")  # Laurence's arena, with Ludwig's two bars beside it
        others = {e: arena.instructions(e) for e in arena.event_ids() if e != 13404852}
        self.assertEqual(boss_events.move_names(arena, {3400850: 3300800}, lambda e: self._events(stem(e))),
                         [(3400850, 13404852, 450000, 512000)])
        again = Emevd(arena.bytes())
        self.assertIn((2003, 11, (1, 3400850, 0, 512000)), again.instructions(13404852))
        self.assertEqual({e: again.instructions(e) for e in again.event_ids() if e != 13404852}, others)

    @needs_game
    def test_part_events_follow_the_boss_with_their_parameters_and_runs(self):
        import boss_events
        source, arena = self._events("m24_01"), self._events("m24_00")
        self.assertEqual(boss_events.part_events(source, 2410800), [12414710, 12414720])
        old = boss_events.part_events(arena, 2400800)
        self.assertEqual(old, [12404810, 12404820])
        self.assertEqual(boss_events.part_events(self._events("m32_00"), 3200800), [])  # Rom's: run for her 30 spiders, the arena's
        report = boss_events.move_parts(arena, {2400800: 2410800}, lambda e: source)
        (_, silenced, added), = report
        self.assertEqual(silenced, old)
        again = Emevd(arena.bytes())
        self.assertEqual(len(again.event_ids()), len(self._events("m24_00").event_ids()) + 2)
        runs = boss_events._callers(again)
        for template, new in zip((12414710, 12414720), added):
            self.assertEqual(again.param_entries(new), source.param_entries(template))
            self.assertEqual(again.restart(new), source.restart(template))
            words = [w for _, _, ws in again.instructions(new) for w in ws]
            self.assertIn(2400800, words)
            self.assertNotIn(2410800, words)
            self.assertNotIn(12411700, words)  # the old arena's defeat flag became this arena's
            self.assertIn(12401800, words)
            self.assertEqual(sorted(w[2:] for _, _, w in runs[new]), sorted(w[2:] for _, _, w in boss_events._callers(source)[template]))
        for event in old:
            self.assertEqual(len(again.instructions(event)), 1)
        # the table grew by the copies' entries (a silenced event's entries stay unused in it, as neutralize leaves them)
        vanilla = self._events("m24_00")
        self.assertEqual(struct.unpack_from("<q", again.bytes(), 0x50)[0],
                         struct.unpack_from("<q", vanilla.bytes(), 0x50)[0] + sum(len(again.param_entries(e)) for e in added))
        for event in vanilla.event_ids():
            if event not in old and event != 0:
                self.assertEqual((again.instructions(event), again.param_entries(event)),
                                 (vanilla.instructions(event), vanilla.param_entries(event)), event)

    @needs_game
    def test_a_copied_phase_waits_on_its_sibling_copy(self):
        import boss_events
        arena = self._events("m24_01")
        (_, _, added), = boss_events.move_phases(arena, {2410800: 2300800}, lambda e: self._events("m23_00"))
        first, second = added  # the copies of 12304807 and 12304808, which waits for 12304807
        words = [w for _, _, ws in Emevd(arena.bytes()).instructions(second) for w in ws]
        self.assertIn(first, words)
        self.assertNotIn(12304807, words)
        # Rom's phase sets the m32 flags that call her spiders: those lines do not come along
        body = boss_events.bodies(self._events("m32_00"), 3200800)[0][2]
        words = [w for _, _, ws in boss_events._rewrite(body, 3200800, 2400800) for w in ws]
        self.assertNotIn(13204811, words)
        self.assertNotIn(13204812, words)

    @needs_game
    def test_moved_phase_names_only_the_new_arena(self):
        import boss_events
        body = boss_events.bodies(self._events("m32_00"), 3200800)[0][2]
        new = boss_events._rewrite(body, 3200800, 2400800)
        words = [w for _, _, ws in new for w in ws]
        self.assertIn(2400800, words)
        self.assertNotIn(3200800, words)
        self.assertNotIn(13201800, words)
        self.assertNotIn(3202806, words)  # a region of the old map
        self.assertEqual(len(new), len(body))


class Playable(unittest.TestCase):
    def test_an_animation_plays_shifted_by_the_row_offset_else_plain(self):
        """gamedata.playable: under offset 2000000 a forced 3027 plays as 2003027 (c2630's case), a plain id still plays
        (fallback); with two possible rows an id counts only when both can play it."""
        from gamedata import playable
        table = {0, 100, 3004, 2003027}
        self.assertEqual(playable(table, {0}), frozenset(table))
        self.assertLessEqual({0, 100, 3004, 3027}, playable(table, {2000000}))
        self.assertNotIn(3027, playable(table, {0, 2000000}))
        self.assertNotIn(3027, playable(table, {1000000}))


class Prng(unittest.TestCase):
    def test_splitmix_vector(self):
        self.assertEqual(seedcode.splitmix64(0)[1], 0xE220A8397B1DCDAF)

    def test_xoshiro_vector(self):
        s = seedcode.Stream(0, "x")
        s.s = [1, 2, 3, 4]
        self.assertEqual([s.next() for _ in range(4)], [11520, 0, 1509978240, 1215971899390074240])

    def test_streams_differ_and_repeat(self):
        a = seedcode.Stream(1, "h", "d", "k").next()
        self.assertEqual(a, seedcode.Stream(1, "h", "d", "k").next())
        self.assertNotEqual(a, seedcode.Stream(1, "h", "d", "k2").next())

    def test_below_range(self):
        s = seedcode.Stream(5, "h")
        for n in (1, 2, 3, 7, 1000):
            self.assertTrue(all(0 <= s.below(n) < n for _ in range(200)))
        with self.assertRaises(ValueError):
            s.below(0)


class Code(unittest.TestCase):
    def test_round_trip(self):
        cases = [(0, {}), ((1 << 32) - 1, {}), (77, {"enemy_chance": 50, "exclude": (2630, 1100), "oops_all": 1260}),
                 (9, {"scaling": 2, "fixed_effect": 7010, "rewards": 1, "area_chance": ((2401, 25), (2200, 0))}),
                 (3, {"map_models": 40, "map_mb": 500, "big_models": 5, "variety": 8}), (4, {"chalice_materials": 1})]
        for seed, options in cases:
            code = seedcode.encode(seed, options)
            back_seed, back = seedcode.decode(code)
            self.assertEqual(back_seed, seed)
            self.assertEqual(back, seedcode.normalize(options))

    def test_boss_round_options(self):
        full = dict(boss_swap=1, witch_mix=1, shadows_mix=1, emissary_mix=1, failures_mix=1, bell_maidens=2, chalice_bosses=1,
                    hard_rom=1, hard_amygdala=1, hard_ebrietas=1, difficult_enemies=1)
        self.assertEqual(seedcode.decode(seedcode.encode(3, full))[1], seedcode.normalize(full))
        # at their defaults they keep every older code's world
        self.assertEqual(seedcode.options_hash({k: 0 for k in seedcode.BOOL_OPTIONS_R2}), seedcode.options_hash({}))
        for bad in ({"bell_maidens": 3}, {"hard_rom": 2}):
            with self.assertRaises(ValueError):
                seedcode.normalize(bad)

    def test_bosses_as_enemies_option(self):
        """Option 42 bosses_as_enemies (user, 7 Nov): a world code carries it, and off it keeps every older world."""
        code = seedcode.encode(5, {"bosses_as_enemies": 1})
        self.assertEqual(seedcode.decode(code), (5, seedcode.normalize({"bosses_as_enemies": 1})))
        self.assertNotEqual(code, seedcode.encode(5, {}))
        self.assertIn(42, seedcode.EXTRAS)
        self.assertEqual(seedcode.options_hash({"bosses_as_enemies": 0}), seedcode.options_hash({}))
        for bad in ({"bosses_as_enemies": 2}, {"boss_enemy_chance": 7}, {"boss_enemy_chance": 105}, {"boss_enemy_max": 0},
                    {"boss_enemy_max": seedcode.BOSS_ENEMY_MAX + 1}, {"huge_bosses_as_enemies": 2}):
            with self.assertRaises(ValueError):
                seedcode.normalize(bad)
        every = {"bosses_as_enemies": 1, "boss_enemy_chance": 50, "boss_enemy_max": 3, "huge_bosses_as_enemies": 1}
        self.assertEqual(seedcode.decode(seedcode.encode(6, every))[1], seedcode.normalize(every))
        self.assertEqual(seedcode.options_hash({"boss_enemy_chance": 25, "boss_enemy_max": 1}), seedcode.options_hash({}))
        # option 49 boss_enemy_hp (user 8 Nov): 8-16 in steps of 2, default 8; it sets HP only, so the same enemies at any value
        for times in seedcode.BOSS_ENEMY_HP:
            o = {"bosses_as_enemies": 1, "boss_enemy_hp": times}
            self.assertEqual(seedcode.decode(seedcode.encode(7, o))[1], seedcode.normalize(o))
            self.assertEqual(seedcode.options_hash(o), seedcode.options_hash({"bosses_as_enemies": 1}))
        self.assertEqual(seedcode.encode(7, {"boss_enemy_hp": 8}), seedcode.encode(7, {}))
        self.assertNotEqual(seedcode.encode(7, {"boss_enemy_hp": 16}), seedcode.encode(7, {}))
        for bad in (0, 7, 9, 18):
            with self.assertRaises(ValueError):
                seedcode.normalize({"boss_enemy_hp": bad})

    def test_typing_mistake_is_refused(self):
        code = seedcode.encode(12345, {"enemy_chance": 50})
        for i in range(3, len(code)):
            if code[i] == "-":
                continue
            bad = code[:i] + ("0" if code[i] != "0" else "1") + code[i + 1:]
            try:
                decoded = seedcode.decode(bad)
            except ValueError:
                continue
            self.assertNotEqual(decoded, seedcode.decode(code))  # the checksum can miss 1 in 1024, never silently equal

    def test_other_schema_refused(self):
        with self.assertRaises(seedcode.CodeError) as newer:
            seedcode.decode("R9-00000-00")
        self.assertEqual(newer.exception.kind, "newer")
        with self.assertRaises(seedcode.CodeError) as old:
            seedcode.decode("R1-1M002-D98XR-60238-11R0G-Y08QE")
        self.assertEqual(old.exception.kind, "bad")

    def test_seed_over_32_bits_refused(self):
        with self.assertRaises(ValueError):
            seedcode.encode(1 << 32, {})

    def test_vectors(self):
        """tests/r2_vectors.json is shared with the launcher's decoder tests: both must agree with it."""
        with open(os.path.join(os.path.dirname(__file__), "r2_vectors.json"), encoding="utf-8") as f:
            data = json.load(f)
        self.assertEqual(data["schema"], seedcode.SCHEMA)
        self.assertEqual(data["core_bits"], seedcode.CORE_BITS)
        for v in data["vectors"]:
            with self.subTest(v["name"]):
                if "error" in v:
                    with self.assertRaises(seedcode.CodeError) as e:
                        seedcode.decode(v["code"])
                    self.assertEqual(e.exception.kind, v["error"])
                    continue
                seed, options = seedcode.decode(v["code"])
                self.assertEqual(seed, v["seed"])
                self.assertEqual(json.loads(json.dumps(options)), v["options"])
                self.assertEqual(seedcode.canonical(v["code"]), v["canonical"])
                self.assertEqual(seedcode.label_code(v["code"]), v["label"])
                self.assertEqual(seedcode.encode(seed, options), v["canonical"])

    def test_launcher_worlds_stay_short(self):
        every = {name: 1 for name in seedcode.CORE_FLAGS}
        every.update(scaling=2, fixed_effect=7010, enemy_chance=25, headroom=300)
        code = seedcode.encode((1 << 32) - 1, every)
        part = "rando%d:%s+w:%s+g:%s" % (seedcode.GENERATOR_MAJOR, code.lower(), "0" * 8, "0" * 8)
        self.assertLessEqual(len(part), 64)

    def test_bad_options_refused(self):
        for bad in ({"enemy_chance": 33}, {"nope": 1}, {"scaling": 2}, {"fixed_effect": 7}):
            with self.assertRaises(ValueError):
                seedcode.normalize(bad)

    def test_list_order_does_not_change_code(self):
        self.assertEqual(seedcode.encode(1, {"exclude": (2, 1)}), seedcode.encode(1, {"exclude": (1, 2)}))


class ModInputs(unittest.TestCase):
    def test_each_mod_is_named_with_a_hash_of_what_it_supplied(self):
        from gamedata import GameFiles
        with tempfile.TemporaryDirectory() as tmp:
            def put(folder, rel, data):
                path = os.path.join(tmp, folder, "dvdroot_ps4", rel)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(data)
            put("game", "param/a.bin", b"game a")
            put("game", "param/b.bin", b"game b")
            put("Nocturne", "param/a.bin", b"mod a")
            put("Unused", "param/c.bin", b"never read")
            files = GameFiles(os.path.join(tmp, "game"), [os.path.join(tmp, "Nocturne"), os.path.join(tmp, "Unused")])
            self.assertEqual(files.read("param/a.bin"), b"mod a")
            files.read("param/b.bin")
            mods = files.mod_inputs()
        self.assertEqual([(m["name"], m["files"]) for m in mods], [("Nocturne", 1), ("Unused", 0)])
        self.assertEqual(mods[0]["hash"], hashlib.sha256(("param/a.bin %s\n" % hashlib.sha256(b"mod a").hexdigest()).encode()).hexdigest())
        self.assertEqual(mods[1]["hash"], "")


class WorldFolder(unittest.TestCase):
    def test_the_folder_name_holds_no_code(self):
        # a code grows with its lists; in the folder name a long one put the world's files past Windows' 259 characters
        import generate
        self.assertEqual(generate.folder_name("R2-0M000-01G74-ZY", "227291f8" + "0" * 56), "Randomizer 6def8a9ecc26 227291f8")
        self.assertEqual(len(generate.folder_name("R2" + "-AD2XE" * 60, "227291f8" + "0" * 56)), 32)

    def test_a_new_build_replaces_an_old_folder_whole(self):
        import generate
        with tempfile.TemporaryDirectory() as tmp:
            final, temp = os.path.join(tmp, "Randomizer R2-X 0"), os.path.join(tmp, "Randomizer R2-X 0.tmp")
            for folder, rel, text in ((final, "randomizer_manifest.json", "old"), (final, "dvdroot_ps4/map/stray.bin", "old"),
                                      (temp, "randomizer_manifest.json", "new"), (temp, "dvdroot_ps4/event/e.bin", "new")):
                os.makedirs(os.path.dirname(os.path.join(folder, rel)), exist_ok=True)
                with open(os.path.join(folder, rel), "w") as f:
                    f.write(text)
            generate.install(temp, final)
            got = sorted(os.path.relpath(os.path.join(h, n), final).replace("\\", "/") for h, _, ns in os.walk(final) for n in ns)
            self.assertEqual(got, ["dvdroot_ps4/event/e.bin", "randomizer_manifest.json"])
            with open(os.path.join(final, "randomizer_manifest.json")) as f:
                self.assertEqual(f.read(), "new")
            self.assertEqual(sorted(os.listdir(tmp)), ["Randomizer R2-X 0"])

    def test_a_folder_that_cannot_be_moved_stays_as_it_was(self):
        import generate
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            final, temp = os.path.join(tmp, "w"), os.path.join(tmp, "w.tmp")
            for folder in (final, temp):
                os.makedirs(os.path.join(folder, "dvdroot_ps4"))
                with open(os.path.join(folder, "randomizer_manifest.json"), "w") as f:
                    f.write(folder)
            real = os.replace

            def busy(src, dst):
                if src == final:
                    raise PermissionError("in use")
                return real(src, dst)
            with mock.patch.object(generate.os, "replace", busy):
                with self.assertRaises(generate.FolderInUse):
                    generate.install(temp, final)
            with open(os.path.join(final, "randomizer_manifest.json")) as f:
                self.assertEqual(f.read(), final)
            self.assertEqual(os.listdir(tmp), ["w"])


class InputHash(unittest.TestCase):
    def test_the_check_counts_the_generator_and_ignores_case(self):
        from gamedata import GameFiles
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "game", "dvdroot_ps4", "Msg", "ENGUS"))
            with open(os.path.join(tmp, "game", "dvdroot_ps4", "Msg", "ENGUS", "item.bin"), "wb") as f:
                f.write(b"text")
            one, two = GameFiles(os.path.join(tmp, "game")), GameFiles(os.path.join(tmp, "game"))
            self.assertEqual(one.listdir("msg"), ["engus"])
            one.read("msg/engus/item.bin")
            two.read("MSG\\EngUS\\Item.bin")
            self.assertEqual(one.read_hashes, two.read_hashes)
            self.assertEqual(one.input_hash(), two.input_hash())
            here, read = one.input_hash(), one.read_hash()
            major = seedcode.GENERATOR_MAJOR
            try:
                seedcode.GENERATOR_MAJOR = major - 1
                self.assertNotEqual(one.input_hash(), here)  # an older generator's world never takes this one's name
                self.assertNotEqual(one.read_hash(), read)
            finally:
                seedcode.GENERATOR_MAJOR = major
            import buildid
            saved = buildid._cached
            try:
                buildid._cached = "0" * 64  # another generator build: another world name, the same read hash
                self.assertNotEqual(one.input_hash(), here)
                self.assertEqual(one.read_hash(), read)
            finally:
                buildid._cached = saved
            self.assertEqual(one.input_hash(), here)

    def test_the_compressor_packs_the_bytes_it_always_did(self):
        """Every .dcx the world writes is zlib level 9: a Python whose zlib packs other bytes would make another party mod
        hash for the same world (the manifest records the zlib version; the packaged generator must give these bytes too)."""
        from containers import dcx_write
        payload = b"".join(b"row %06d %s\n" % (i, bytes([65 + i % 26]) * (i % 37)) for i in range(4000))
        packed = dcx_write(payload, b"DCX\0" + bytes(0x24) + b"DFLT" + bytes(0x20))
        self.assertEqual(hashlib.sha256(packed).hexdigest(), "1aee27c665a1897255694707d0f7ced3dd30f435a8cd02c3aab6ec1ae5c881b5")


def tiny_world():
    slots = {
        "m24_01": [{"part": "p%d" % i, "model": "c1000", "kept": None} for i in range(30)] + [{"part": "boss", "model": "c5000", "kept": "boss"}],
        "m24_00": [{"part": "q%d" % i, "model": "c1100", "kept": None} for i in range(30)],
    }
    candidates = {"c1000": [(1, 10)], "c1100": [(2, 20), (3, 21)], "c1200": [(4, 30)], "c5120": [(5, 40)], "c5070": [(6, 50)], "c2510": [(7, 60)]}
    mb = 1 << 20
    sizes = {"c1000": 10 * mb, "c1100": 10 * mb, "c1200": 10 * mb, "c5000": 20 * mb, "c5120": 40 * mb, "c5070": 35 * mb, "c2510": 34 * mb}
    return slots, candidates, sizes


class Plan(unittest.TestCase):
    def run_plan(self, seed=1, options=None, limits=(100, 1 << 40), groups=()):
        slots, candidates, sizes = tiny_world()
        return planner.plan(slots, candidates, sizes, list(groups), limits, seed, options or {}), slots, candidates, sizes

    def test_same_seed_same_plan(self):
        self.assertEqual(self.run_plan()[0]["changes"], self.run_plan()[0]["changes"])

    def test_seed_changes_plan(self):
        self.assertNotEqual(self.run_plan(1)[0]["changes"], self.run_plan(2)[0]["changes"])

    def test_kept_slots_never_change(self):
        result, *_ = self.run_plan()
        self.assertNotIn("boss", result["changes"]["m24_01"])

    def test_chance_zero_and_area_override(self):
        self.assertFalse(any(self.run_plan(options={"enemy_chance": 0})[0]["changes"].values()))
        r = self.run_plan(options={"enemy_chance": 0, "area_chance": ((2401, 100),)})[0]["changes"]
        self.assertEqual(len(r["m24_01"]), 30)
        self.assertFalse(r["m24_00"])

    def test_exclude_and_oops(self):
        r = self.run_plan(options={"exclude": (1200,)})[0]["changes"]
        self.assertFalse(any(c[0] == "c1200" for m in r.values() for c in m.values()))
        r = self.run_plan(options={"oops_all": 1200})[0]["changes"]
        self.assertTrue(all(c[0] == "c1200" for m in r.values() for c in m.values()))
        with self.assertRaises(ValueError):
            self.run_plan(options={"oops_all": 9999})

    def test_no_winter_lanterns(self):
        """Option no_winter_lanterns (user 10 Nov): the Lantern's model never replaces an enemy; off, it may."""
        with mock.patch.object(planner, "WINTER_LANTERN", "c2510"):
            r = self.run_plan(options={"no_winter_lanterns": 1})[0]["changes"]
            self.assertTrue(any(m.values() for m in r.values()))
            self.assertFalse(any(c[0] == "c2510" for m in r.values() for c in m.values()))
            r = self.run_plan()[0]["changes"]
            self.assertTrue(any(c[0] == "c2510" for m in r.values() for c in m.values()))

    def test_include_only(self):
        """Option include_only (user 11 Nov): only the listed models replace an ordinary enemy, exclude still wins; the
        check refuses any other model."""
        result, slots, candidates, sizes = self.run_plan(options={"include_only": (1200, 2510)})
        models = {c[0] for m in result["changes"].values() for c in m.values()}
        self.assertTrue(models)
        self.assertLessEqual(models, {"c1200", "c2510"})
        self.assertEqual(planner.check(result, slots, candidates, sizes, [], (100, 1 << 40)), [])
        result = self.run_plan(options={"include_only": (1200, 2510), "exclude": (2510,)})[0]
        self.assertEqual({c[0] for m in result["changes"].values() for c in m.values()}, {"c1200"})
        result["options"] = seedcode.normalize(dict(result["options"], include_only=(1000,)))
        self.assertTrue(planner.check(result, slots, candidates, sizes, [], (100, 1 << 40)))

    def test_slot_takes_only_a_model_with_its_forced_animations(self):
        slots, candidates, sizes = tiny_world()
        for slot in slots["m24_01"]:
            slot["needs"] = (7000,)
        animations = {"c1200": frozenset((7000,))}
        result = planner.plan(slots, candidates, sizes, [], (100, 1 << 40), 1, {}, animations)
        self.assertTrue(result["changes"]["m24_01"])
        self.assertTrue(all(c[0] == "c1200" for c in result["changes"]["m24_01"].values()))
        result = planner.plan(slots, candidates, sizes, [], (100, 1 << 40), 1, {}, {})
        self.assertFalse(result["changes"]["m24_01"])
        self.assertEqual(result["report"]["stayed_no_animation"], 30)
        self.assertEqual(planner.check(result, slots, candidates, sizes, [], (100, 1 << 40), {}), [])

    def test_big_models_is_retired_but_still_reads(self):
        import seedcode
        self.assertFalse(hasattr(planner, "BIG_MODEL"))
        code = seedcode.encode(7, {"big_models": 6})
        self.assertEqual(seedcode.decode(code)[1]["big_models"], 6)  # old codes keep decoding
        self.assertEqual(seedcode.options_hash({"big_models": 6}), seedcode.options_hash({}))  # and make the same world
        run = subprocess.run([sys.executable, "-I", os.path.join(TOOL, "randomizer.py"), "code", "--seed", "7", "--big-models", "6"],
                             capture_output=True, text=True)
        self.assertNotEqual(run.returncode, 0)  # the CLI flag is gone

    def test_starter_counts_reach_the_code_and_pick_the_rows(self):
        import seedcode
        import starters
        run = subprocess.run([sys.executable, "-I", os.path.join(TOOL, "randomizer.py"), "code", "--seed", "7",
                              "--starter-melee", "5", "--starter-guns", "1"], capture_output=True, text=True, check=True)
        options = seedcode.decode(run.stdout.strip().splitlines()[-1])[1]
        self.assertEqual((options["starter_melee"], options["starter_guns"]), (5, 1))
        self.assertEqual(seedcode.encode(7, {"starter_melee": 3, "starter_guns": 2}), seedcode.encode(7, {}))  # defaults not written
        self.assertEqual(seedcode.options_hash({"starter_melee": 3, "starter_guns": 2}), seedcode.options_hash({}))  # old codes keep their world
        for bad in ({"starter_melee": 0}, {"starter_melee": 6}, {"starter_guns": 0}, {"starter_guns": 4}):
            with self.assertRaises(ValueError):
                seedcode.normalize(bad)
        self.assertEqual((starters.hidden_rows(3, 2), starters.added_rows(3, 2)), (set(), {}))
        self.assertEqual(starters.hidden_rows(1, 1), {2001, 2002, 2011})
        self.assertEqual(starters.added_rows(5, 3), {2003: 2000, 2004: 2000, 2012: 2010})
        found = {"melee": list(range(1, 30)), "guns": list(range(100, 110)), "rows": {2000: 1, 2001: 2, 2002: 3, 2010: 100, 2011: 101}}
        self.assertEqual(starters.plan(found, 7, "h", 5, 3)[2000], starters.plan(found, 7, "h")[2000])  # more rows, same draws
        plain = starters.plan(found, 7, "h", 5, 3, shuffle=False)
        self.assertEqual(set(plain), {2003, 2004, 2012})  # without the shuffle only the added rows are drawn
        self.assertFalse(set(plain.values()) & set(found["rows"].values()))

    @needs_game
    def test_added_starter_rows_copy_the_first_row_and_its_flag(self):
        import starters
        from gamedata import GameFiles
        files = GameFiles(GAME)
        bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
        defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
        from containers import Layout
        layout = Layout(bnd4_find(defs, "\\ShopLineupParam.paramdef")[3])
        param = Param(bnd4_find(bnd, "\\ShopLineupParam.param")[3])
        starters.add_rows(param, starters.added_rows(5, 3))
        starters.hide_rows(param, starters.hidden_rows(5, 3))
        rows = Param(param.write()).first()
        self.assertEqual([r for r in sorted(rows) if 2000 <= r < 2020], [2000, 2001, 2002, 2003, 2004, 2010, 2011, 2012])
        for new, source in ((2003, 2000), (2004, 2000), (2012, 2010)):
            self.assertEqual(layout.get(rows[new], "eventFlag"), layout.get(rows[source], "eventFlag"))
            self.assertEqual(layout.get(rows[new], "value"), 0)
        ids = [r[0] for r in param.rows if 1000 <= r[0] < 3000]  # vanilla is not sorted everywhere; here it is
        self.assertEqual(ids, sorted(ids))

    def test_map_budget_and_no_room(self):
        result, slots, candidates, sizes = self.run_plan(options={"map_mb": 25})  # 20 MB boss + 10 MB never fits m24_01
        self.assertFalse(result["changes"]["m24_01"])
        self.assertEqual(result["report"]["stayed_no_room"], 30)

    def test_group_budget(self):
        groups = [["m24_00", "m24_01"]]
        limits = (3, 200 << 20)
        result, slots, candidates, sizes = self.run_plan(limits=limits, groups=groups)
        self.assertEqual(planner.check(result, slots, candidates, sizes, groups, limits), [])
        self.assertLessEqual(len(result["chosen"]["m24_00"] | result["chosen"]["m24_01"]), 3)

    def test_headroom_scales_every_cap_and_stays_out_of_old_hashes(self):
        base = {"headroom": 100, "map_models": 24, "map_mb": 343, "big_models": 2}
        wide = planner.Limits(dict(base, headroom=150), 40, 400 << 20)
        self.assertEqual((wide.map_models, wide.map_bytes, wide.group_models, wide.group_bytes), (36, (343 << 20) * 3 // 2, 60, 600 << 20))
        self.assertEqual(seedcode.options_hash({"headroom": 100, "treasure_by_type": 0}), seedcode.options_hash({}))
        self.assertNotEqual(seedcode.options_hash({"headroom": 150}), seedcode.options_hash({}))
        # content-only options never change the draws
        drawn = {"headroom": 150, "starters": 1, "drops": 1}
        self.assertEqual(seedcode.options_hash(dict(drawn, boss_cutscenes=1)), seedcode.options_hash(drawn))
        with self.assertRaises(ValueError):
            seedcode.normalize({"headroom": 90})
        groups = [["m24_00", "m24_01"]]
        result, slots, candidates, sizes = self.run_plan(limits=(3, 200 << 20), groups=groups, options={"headroom": 200})
        self.assertEqual(planner.check(result, slots, candidates, sizes, groups, (3, 200 << 20)), [])
        self.assertLessEqual(len(result["chosen"]["m24_00"] | result["chosen"]["m24_01"]), 6)

    def test_slots_spread_evenly_over_the_picked_models(self):
        result, slots, candidates, sizes = self.run_plan()
        for stem, changes in result["changes"].items():
            counts = {}
            for model, _, _ in changes.values():
                counts[model] = counts.get(model, 0) + 1
            if len(counts) > 1:
                self.assertLessEqual(max(counts.values()) - min(counts.values()), 1, stem)

    def test_changing_an_option_only_in_one_area_is_stable_elsewhere_by_slot(self):
        # streams are keyed by slot, so slot draws do not shift when other slots are added
        a = seedcode.Stream(1, "h", "enemy-slot", "m24_01", "p1").next()
        b = seedcode.Stream(1, "h", "enemy-slot", "m24_01", "p1").next()
        self.assertEqual(a, b)


def tiny_emevd():
    """Two events: one with 3 instructions, parameters on the first and last; one with a single instruction."""
    instr = bytearray()
    args = struct.pack("<4i", 1, 2, 3, 4)
    for bank, iid, length, offset in ((2003, 18, 8, 0), (1014, 0, 0, 8), (2004, 5, 8, 8), (2003, 18, 0, 8)):
        instr += struct.pack("<iiqqq", bank, iid, length, offset, 0)
    params = bytearray()
    for i, dst, src in ((0, 0, 0), (0, 4, 4), (2, 0, 8)):
        params += struct.pack("<qqqq", i, dst, src, 0)
    events_at = 0x90
    instr_at = events_at + 0x60
    params_at = instr_at + len(instr)
    args_at = params_at + len(params)
    head = bytearray(0x90)
    struct.pack_into("<q", head, 0x10, 2)
    struct.pack_into("<q", head, 0x18, events_at)
    struct.pack_into("<q", head, 0x28, instr_at)
    struct.pack_into("<q", head, 0x58, params_at)
    struct.pack_into("<q", head, 0x78, args_at)
    events = struct.pack("<qqqqqq", 100, 3, 0, 3, 0, 0) + struct.pack("<qqqqqq", 200, 1, 0x60, 0, 0x60, 0)
    return bytes(head + events + instr + params + args)


class EventEdit(unittest.TestCase):
    def test_neutralize_changes_only_that_instruction_and_its_parameters(self):
        original = tiny_emevd()
        e = Emevd(original)
        self.assertEqual(e.bytes(), original)
        self.assertEqual(e.params(100), {0: {0: 0, 4: 4}, 2: {0: 8}})
        e.neutralize(100, 0)
        ins = e.instructions(100)
        self.assertEqual(ins[0][0], LABEL_BANK)
        self.assertEqual(ins[0][2], ())
        self.assertNotEqual(ins[0][1], 0)  # the event already holds label 0
        self.assertEqual(ins[1:], Emevd(original).instructions(100)[1:])
        self.assertEqual(e.params(100), {2: {0: 8}})
        self.assertEqual(e.instructions(200), Emevd(original).instructions(200))
        self.assertEqual(len(e.bytes()), len(original))

    def test_no_such_instruction(self):
        with self.assertRaises(IndexError):
            Emevd(tiny_emevd()).neutralize(100, 3)


class IndexFiles(unittest.TestCase):
    def test_gnl_info_round_trip(self):
        names = ["alpha", "beta"]
        self.assertEqual(ailua.gnl_read(ailua.gnl_write(names)), names)
        rows = [(5, 1, "a", None), (6, 0x100, "b", "c")]
        v, z, back = ailua.info_read(ailua.info_write(1, 0, rows))
        self.assertEqual(back, rows)


@needs_game
class OnGameFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from gamedata import Census, GameFiles
        cls.files = GameFiles(GAME)
        cls.census = Census(cls.files)

    def test_riders_go_with_a_replaced_host(self):
        """DATA, vanilla: 9 Frenzy bullet parts c2561 ride Winter Lanterns, 3 spine parts c2501 small Emissaries; a host
        that gets another model loses its rider (user, 10 Nov), one that keeps its kind keeps it."""
        import generate
        riders = {stem: v for stem, v in self.census.riders.items() if v}
        self.assertEqual(riders, {
            "m24_02": {2420731: 2420402}, "m27_00": {2700600: 2700550, 2700601: 2700552},
            "m26_00": {2600310: 2600300, 2600311: 2600301, 2600312: 2600302, 2600313: 2600303},
            "m33_00": {3300520: 3300500, 3300521: 3300501, 3300522: 3300502},
            "m36_00": {3600510: 3600500, 3600511: 3600501}})
        self.assertEqual(generate.riders_off(self.census, "m26_00", frozenset({2600301, 2600399})), [2600311])
        payload = dcx_read(self.files.read("event/m26_00_00_00.emevd.dcx"))[0]
        events = Emevd(generate.release(payload, [], riders=[2600311]))
        self.assertEqual(events.instructions(0)[-1], (2004, 5, (2600311, 0)))

    def test_readable_spoiler_names_from_the_game_text_and_reads_nothing_into_the_hash(self):
        import readable
        from gamedata import GameFiles
        files = GameFiles(GAME)
        files.read("param/gameparam/gameparam.parambnd.dcx")
        before = files.input_hash()
        texts = readable._texts(files)
        layout, rows = readable._tables(files)("ItemLotParam")
        self.assertEqual(files.input_hash(), before)  # peeks only: the spoiler never changes which world a code makes
        self.assertEqual(readable.area_name(texts, "m35_00"), "Research Hall")
        self.assertEqual(readable.area_name(texts, "m24_01"), "Central Yharnam")
        self.assertEqual(readable.lot_text(texts, layout, rows, 2200100), "Pebble x4")  # a pickup gives its item
        self.assertEqual(readable.lot_text(texts, layout, rows, 10202422), "nothing 68%, Blood Vial x2 32%")  # a drop draws by weight
        self.assertEqual(readable.lot_text(texts, layout, rows, 999999999), "#999999999")  # no such row
        self.assertEqual(readable.item_name(texts, 0, 7010000), "Uncanny Saw Cleaver")
        out = readable.build(files, self.census, {stem: {} for stem in self.census.slots}, {}, {}, {}, {}, {})
        self.assertEqual(len(out["areas"]), 16)
        self.assertNotIn("c0000", out["models"])
        self.assertEqual(out["enemies"], {})

    def test_vanilla_budget_numbers(self):
        # the plan's measured ceiling: no main map holds more than 24 models and 343 MB
        most = max(sum(self.census.sizes[m] for m in {s["model"] for s in slots}) for slots in self.census.slots.values())
        self.assertLessEqual(most >> 20, 343)
        self.assertLessEqual(max(len({s["model"] for s in slots}) for slots in self.census.slots.values()), 24)

    def test_boss_swap_keeps_every_rule_and_reads_every_arena(self):
        from gamedata import FORCED_ANIM, ONE_BODY_ARENAS
        from neighbours import group_limits, load_groups
        groups = load_groups()
        limits = group_limits(self.census, groups)
        # boss round 2: every arena's bodies are slots now; the one-body ones of round 1 still move every seed
        arena_slots = [s for v in self.census.slots.values() for s in v if s["role"] == "body"]
        self.assertLessEqual(set(ONE_BODY_ARENAS), {s["entity"] for s in arena_slots})
        self.assertTrue(all(s["think"] > 1 for s in arena_slots if s["entity"] in ONE_BODY_ARENAS))
        for seed in range(40):
            result = planner.plan(self.census.slots, self.census.candidates, self.census.sizes, groups, limits, seed, {"boss_swap": 1}, self.census.animations)
            self.assertEqual(planner.check(result, self.census.slots, self.census.candidates, self.census.sizes, groups, limits, self.census.animations), [])
            self.assertLessEqual(set(ONE_BODY_ARENAS), set(result["bosses"]), seed)  # every boss moves, every seed
            self.assertFalse([e for e, (stem, part) in result["bosses"].items()
                              if result["changes"][stem][part][0] == next(s["model"] for s in arena_slots if s["entity"] == e)])
        off = planner.plan(self.census.slots, self.census.candidates, self.census.sizes, groups, limits, 1, {}, self.census.animations)
        self.assertEqual(off["bosses"], {})
        # a model with no animations: every forced one on the boss goes inert, the file's size and other events stay
        # (the animation pass alone: patched_events also runs every arena edit, test_arena_events). Template 12304715
        # forces its runs' animation (word 7 of each call in event 0: 8000-8040) on 2300810 itself: those words go -1.
        from generate import _inert_animations
        payload = dcx_read(self.files.read("event/m23_00_00_00.emevd.dcx"))[0]
        events = Emevd(payload)
        done = _inert_animations(events, {2300810: "none"}, {"none": frozenset()})
        patched = events.bytes()
        self.assertEqual(len(patched), len(payload))
        self.assertEqual({d[1] for d in done}, {12301702, 12304715})
        before, after = Emevd(payload), Emevd(patched)
        for event in before.event_ids():
            if event not in (0, 12301702):
                self.assertEqual(before.instructions(event), after.instructions(event))
                self.assertEqual(before.params(event), after.params(event))
        self.assertFalse([1 for ins in after.instructions(12301702) if ins[:2] == FORCED_ANIM and ins[2][0] == 2300810])
        runs = lambda ev: [w for b, n, w in ev.instructions(0) if (b, n) == (2000, 0) and w[1:2] == (12304715,)]
        self.assertEqual(len(runs(before)), 5)
        self.assertEqual([w[:7] + (-1,) + w[8:] for w in runs(before)], runs(after))
        changed = [x for x, y in zip(before.instructions(0), after.instructions(0)) if x != y]
        self.assertEqual(len(changed), 5)

    def test_starter_armor_dresses_every_origin_in_one_drawn_outfit(self):
        """Option starter_armor (user 7 Nov): each slot draws from the armour the game hands out of that slot; the ten
        origin rows of both sets (2000-2009 with the hood 230000, which a new character wears, LIVE; 3000-3009 without
        it; both Foreign Garb 231000, Sullied Bandage 232000, Foreign Trousers 233000) get it, no other row changes."""
        import starters
        found = starters.armor_census(self.files)
        self.assertTrue(all(len(pool) > 10 for pool in found))
        self.assertNotIn(231000, found[1])  # the Foreign set is worn at the start only, never handed out (DATA)
        outfits = {tuple(starters.armor_plan(found, seed, "h")) for seed in range(20)}
        self.assertGreater(len(outfits), 15)
        for outfit in outfits:
            self.assertTrue(all(item in pool for item, pool in zip(outfit, found)))
        bnd = bnd4_read(dcx_read(self.files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
        defs = bnd4_read(dcx_read(self.files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
        layout = Layout(bnd4_find(defs, "\\CharaInitParam.paramdef")[3])
        param = Param(bnd4_find(bnd, "\\CharaInitParam.param")[3])
        before = list(param.rows)
        self.assertEqual({tuple(layout.get(b, f) for f in starters.ARMOR_SLOTS) for r, b, _ in before if r in starters.ORIGIN_ROWS},
                         {(230000, 231000, 232000, 233000), (-1, 231000, 232000, 233000)})
        self.assertEqual(sum(r in starters.ORIGIN_ROWS for r, _, _ in before), 20)
        outfit = starters.armor_plan(found, 3, "h")
        starters.apply_armor(param, layout, outfit)
        for (r, b, _), (r2, b2, _) in zip(before, param.rows):
            self.assertEqual(r, r2)
            if r in starters.ORIGIN_ROWS:
                self.assertEqual([layout.get(b2, f) for f in starters.ARMOR_SLOTS], outfit)
            else:
                self.assertEqual(b, b2)

    def test_a_template_animation_goes_only_for_the_run_whose_model_lacks_it(self):
        """User 7 Nov: play the animation if the new model has it, else stand. m24_01 template 12415225 forces its run's
        animation: (2410015, 3004) and (2410102, 3027) (DATA). A model lacking 3027 in 2410102 turns off only that run's
        argument (-1); the template and the other run stay as they are."""
        from generate import _inert_animations
        payload = dcx_read(self.files.read("event/m24_01_00_00.emevd.dcx"))[0]
        events = Emevd(payload)
        runs = lambda ev: [w for b, n, w in ev.instructions(0) if (b, n) == (2000, 0) and w[1:2] == (12415225,)]
        self.assertEqual(sorted(w[2:4] for w in runs(events)), [(2410015, 3004), (2410102, 3027)])
        done = _inert_animations(events, {2410102: "lacks", 2410015: "has"},
                                 {"lacks": frozenset({0}), "has": frozenset({0, 3004})})
        after = Emevd(events.bytes())
        self.assertEqual(done, [(2410102, 12415225, 10, 3027)])
        self.assertEqual(sorted(w[2:4] for w in runs(after)), [(2410015, 3004), (2410102, -1)])
        self.assertEqual(Emevd(payload).instructions(12415225), after.instructions(12415225))

    def test_required_goods_are_the_ones_named_and_what_the_game_asks_for(self):
        import treasure
        required = treasure.required_goods(self.files)
        # D20's tools, keys, an item a door asks for and items events ask for
        self.assertLessEqual({4102, 4103, 4104, 4000, 4019, 4001, 200, 701, 702, 6302, 130, 703, 4306}, required)
        self.assertNotIn(1000, required)  # a blood vial stays free
        self.assertNotIn(0, required)

    def test_treasure_moves_only_free_lots_and_keeps_every_lot(self):
        import re
        import treasure
        found = treasure.census(self.files)
        self.assertGreater(len(found["movable"]), 300)
        mapping = treasure.plan(found, 7, "h")
        self.assertEqual(sorted(mapping), sorted(mapping.values()))
        self.assertGreater(sum(a != b for a, b in mapping.items()), 280)
        self.assertFalse(set(mapping) & set(found["pinned"]))
        payload = dcx_read(self.files.read("map/mapstudio/m24_01_00_00.msb.dcx"))[0]
        traded = treasure.trade_lots(payload, mapping)
        self.assertEqual(len(traded), len(payload))
        before, after = treasure.treasure_lots(payload), treasure.treasure_lots(traded)
        self.assertEqual([a for a, _ in before], [a for a, _ in after])
        self.assertEqual([mapping.get(l, l) for _, l in before], [l for _, l in after])
        differ = [i for i in range(len(payload)) if payload[i] != traded[i]]
        words = {a + k for a, _ in before for k in range(4)}
        self.assertTrue(set(differ) <= words)
        # the engine's lists by lot: its award spots and key catch-up never move. Other shared treasure goes by lot, wherever it lies
        inc = os.path.join(os.path.dirname(__file__), "..", "..", "..", "engine", "gpu", "shim", "party", "party_items.inc")
        if os.path.exists(inc):
            with open(inc, encoding="utf-8") as f:
                text = f.read()
            known = {int(m) for m in re.findall(r"^ITEM_AWARD_SPOT\((\d+)", text, re.M)}
            known |= {int(m) for m in re.findall(r"^ITEM_SHARED_TREASURE\((\d+), 1,", text, re.M)}
            self.assertGreater(len(known), 5)
            self.assertEqual(sorted(known & set(found["movable"])), [])

    def test_treasure_pins_lots_without_an_item_row_and_can_trade_by_kind(self):
        import treasure
        found = treasure.census(self.files)
        self.assertTrue(all(found["pinned"].get(lot, "").startswith("no row for item") for lot in (2300270, 3300100)))
        self.assertEqual(sorted(found["kinds"]), sorted(found["movable"]))
        mapping = treasure.plan(found, 7, "h", True)
        self.assertEqual(sorted(mapping), sorted(mapping.values()))
        self.assertTrue(all(found["kinds"][a] == found["kinds"][b] for a, b in mapping.items()))
        self.assertGreater(len({found["kinds"][a] for a in mapping}), 2)

    def test_starting_weapons_are_drawn_from_what_the_game_gives_out(self):
        import starters
        found = starters.census(self.files)
        self.assertGreater(len(found["melee"]), 40)
        self.assertGreater(len(found["guns"]), 4)
        self.assertFalse(set(found["melee"]) & set(found["guns"]))
        # every left-hand weapon is a secondary (user, 6 Nov): Rosmarinus, Fist of Gratia (Nocturne: Vacuous Iron), Piercing Rifle
        self.assertLessEqual({18000000, 34000000, 36000000}, set(found["guns"]))
        mapping = starters.plan(found, 3, "h")
        self.assertEqual(sorted(mapping), [2000, 2001, 2002, 2010, 2011])
        self.assertEqual(len(set(mapping.values())), 5)
        self.assertTrue(all(mapping[r] in found["melee"] for r in starters.MELEE_ROWS[:3]))
        self.assertTrue(all(mapping[r] in found["guns"] for r in starters.GUN_ROWS[:2]))
        self.assertNotEqual(mapping, starters.plan(found, 4, "h"))
        from containers import Layout, Param, bnd4_find, bnd4_read
        bnd = bnd4_read(dcx_read(self.files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
        defs = bnd4_read(dcx_read(self.files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
        layout = Layout(bnd4_find(defs, "\\EquipParamWeapon.paramdef")[3])
        gem_layout = Layout(bnd4_find(defs, "\\WeaponGenParam.paramdef")[3])
        param = Param(bnd4_find(bnd, "\\EquipParamWeapon.param")[3])
        gems = Param(bnd4_find(bnd, "\\WeaponGenParam.param")[3])
        before, gems_before = param.first(), gems.first()
        copies = starters.copy_ids(before, gems_before, mapping.values())
        self.assertEqual(sorted(copies), sorted(set(mapping.values())))
        self.assertEqual({c // 1000000 for c in copies.values()}, {w // 1000000 for w in copies})  # same weapon number
        added = starters.apply_weapons(param, layout, copies)
        starters.apply_gems(gems, gem_layout, copies)
        rows, gem_rows = Param(param.write()).first(), Param(gems.write()).first()
        self.assertEqual([r[0] for r in param.rows], sorted(r[0] for r in param.rows))
        self.assertEqual({i: b for i, b in rows.items() if i in before}, before)  # the drawn weapons keep their rows
        self.assertGreaterEqual(len(added), 5 * 11)  # every upgrade level of every drawn weapon
        for base, copy in copies.items():
            for level in starters._levels(before, base):
                row = rows[level - base + copy]
                self.assertEqual([layout.get(row, f) for f in starters.REQUIREMENTS], [0, 0, 0, 0], level)
                for f in ("equipModelId", "iconId", "sortId", "wepmotionCategory", "reinforceTypeId", "attackBasePhysics"):
                    self.assertEqual(layout.get(row, f), layout.get(before[level], f), f)
                for f in starters.ORIGINS:
                    linked = layout.get(row, f)
                    self.assertFalse(linked > 0 and linked // starters.LEVEL * starters.LEVEL == base, (level, f))
        self.assertEqual({i: b for i, b in gem_rows.items() if i in gems_before}, gems_before)
        for base, copy in copies.items():
            levels = starters._levels(gems_before, base)
            self.assertTrue(levels, base)
            for level in levels:
                body, copy_row = gems_before[level], gem_rows[level - base + copy]
                self.assertEqual(gem_layout.get(copy_row, "wepParamId"), gem_layout.get(body, "wepParamId") - base + copy)
                self.assertEqual(gem_layout.get(copy_row, "gemSlotType_0"), gem_layout.get(body, "gemSlotType_0"))
        text_bnd = bnd4_read(dcx_read(self.files.read("msg/engus/item.msgbnd.dcx"))[0])
        self.assertEqual(sorted(e[1] for e in text_bnd["entries"] if e[1] in starters.TEXTS), list(starters.TEXTS))
        names = fmg_read(bnd4_find(text_bnd, "\\武器名.fmg")[3])
        starters.copy_text(names, copies)
        names = fmg_read(fmg_write(names))["texts"]
        for base, copy in copies.items():
            self.assertEqual(names[copy], names[base])
            self.assertEqual(names[copy + 1000], names[base + 1000])
        starter_shop = Param(bnd4_find(bnd, "\\ShopLineupParam.param")[3])
        shop_layout = Layout(bnd4_find(defs, "\\ShopLineupParam.paramdef")[3])
        starters.apply_shop(starter_shop, shop_layout, mapping, copies)
        shop_rows = starter_shop.first()
        self.assertEqual({r: shop_layout.get(shop_rows[r], "equipId") for r in mapping}, {r: copies[w] for r, w in mapping.items()})

    def test_chalice_materials_stay_in_map_pickups_shop_rows_and_drops(self):
        import drops
        import treasure
        import shops
        from gamedata import Census
        chalice = treasure.chalice_materials(self.files)
        # Pthumeru Chalice: Ritual Blood (1) 7000; Root: Tomb Mold (1) 7020; Hintertomb: Arcane Haze 7150; Ihyll: 7200
        self.assertLessEqual({7000, 7001, 7002, 7003, 7004, 7020, 7024, 7150, 7200}, chalice)
        self.assertFalse(chalice & {1000, 3000, 130})  # Blood Vial, Blood Stone Shard, a key: not materials
        loose = treasure.census(self.files)
        kept = treasure.census(self.files, chalice)
        self.assertTrue([r for r in kept["pinned"].values() if r.startswith("chalice material")])
        self.assertTrue(set(loose["movable"]) - set(kept["movable"]))  # some material lots move without the pin
        # enemy drops: a lot with a material stays too while the option is off (user, 7 Nov: "very literally")
        game = Census(self.files)
        loose_drops, kept_drops = drops.census(self.files, game), drops.census(self.files, game, chalice)
        self.assertFalse([r for r in loose_drops["pinned"].values() if r.startswith("chalice")])
        self.assertTrue([r for r in kept_drops["pinned"].values() if r.startswith("chalice material")])
        self.assertLess(set(kept_drops["movable"]), set(loose_drops["movable"]))
        # shop rows: a material's row trades only when the option is on (tests/test_shops.py)
        listed = lambda found: {found["items"][k] for k in found["rows"].get(3, [])}
        self.assertFalse(listed(shops.census(self.files)) & chalice)
        self.assertIn(7000, listed(shops.census(self.files, chalice)))

    def test_drops_trade_only_lots_of_normal_enemies(self):
        import re
        import drops
        from gamedata import Census, STEMS
        census = Census(self.files)
        found = drops.census(self.files, census)
        mapping = drops.plan(found, 7, "h")
        self.assertGreater(len(found["movable"]), 80)
        self.assertEqual(sorted(mapping), sorted(mapping.values()))
        self.assertFalse(set(mapping) & set(found["pinned"]))
        layout = census.npc_layout
        kept_rows = {s["npc"] for stem in STEMS for s in census.slots[stem] if s["kept"] is not None}
        free_rows = {s["npc"] for stem in STEMS for s in census.slots[stem] if s["kept"] is None}
        for row_id in kept_rows | (set(census.npc_rows) - free_rows):
            self.assertFalse({layout.get(census.npc_rows[row_id], name) for name in drops.LOT_FIELDS} & set(mapping))
        param = Param(bytes(bnd4_find(bnd4_read(dcx_read(self.files.read("param/gameparam/gameparam.parambnd.dcx"))[0]), "\\NpcParam.param")[3]))
        before = list(param.rows)
        drops.apply(param, layout, mapping)
        changed = 0
        for old, new in zip(before, param.rows):
            self.assertEqual(old[0], new[0])
            for name in drops.LOT_FIELDS:
                lot = layout.get(old[1], name)
                self.assertEqual(layout.get(new[1], name), mapping.get(lot, lot))
            stripped = [bytearray(old[1]), bytearray(new[1])]
            for body in stripped:
                for name in drops.LOT_FIELDS:
                    layout.set(body, name, 0)
            self.assertEqual(stripped[0], stripped[1])
            changed += old[1] != new[1]
        self.assertGreater(changed, 100)
        inc = os.path.join(os.path.dirname(__file__), "..", "..", "..", "engine", "gpu", "shim", "party", "party_items.inc")
        if os.path.exists(inc):
            with open(inc, encoding="utf-8") as f:
                text = f.read()
            known = {int(m) for m in re.findall(r"^ITEM_(?:BOSS_DROP|KILL_AWARD|AWARD_SPOT)\((\d+)", text, re.M)}
            self.assertEqual(sorted(known & set(found["movable"])), [])

    def test_event_files_parse_the_same_as_the_census_reader(self):
        from gamedata import _events
        payload = dcx_read(self.files.read("event/m23_00_00_00.emevd.dcx"))[0]
        e, ref = Emevd(payload), _events(payload)
        self.assertEqual(e.bytes(), payload)
        for event_id, (instructions, params) in ref.items():
            self.assertEqual(e.instructions(event_id), instructions)
            self.assertEqual(e.params(event_id), params)

    def test_slot_census(self):
        count = sum(s["kept"] is None for slots in self.census.slots.values() for s in slots)
        self.assertGreater(count, 500)
        # human hunters: only the nameless, silent, quest-free ones are free (user, 6 Nov); 17 in vanilla
        free_humans = [s for slots in self.census.slots.values() for s in slots if s["model"] == "c0000" and s["kept"] is None]
        self.assertEqual(len(free_humans), 17)

    def test_no_special_row_in_the_pool(self):
        from gamedata import SPECIAL_HP
        for rows in self.census.candidates.values():
            for _, npc in rows:
                self.assertLess(self.census.npc_layout.get(self.census.npc_rows[npc], "hp"), SPECIAL_HP)
        self.assertNotIn("c4543", self.census.candidates)

    def test_npcs_on_enemy_models_and_arena_parts_stay(self):
        """User, 2 Nov: NPC slots stay vanilla. gamedata.NPC_ENTITIES re-derived from the game files: a talk script the
        map holds, an NPC think (6000-6999) or an event naming the part that writes NPC quest flags (2003[22], 1000-1999)."""
        import re
        from gamedata import (NPC_ENTITIES, STEMS, GameFiles, _load_events, map_variants)
        fresh = GameFiles(GAME)  # the talk files read here never reach the class's input hash
        by_entity = {s["entity"]: (stem, s) for stem, slots in self.census.slots.items() for s in slots}
        found = set()
        for stem in STEMS:
            talk = fresh.read("script/talk/%s_00_00.talkesdbnd.dcx" % stem)
            scripts = {int(m.group(1)) for e in bnd4_read(dcx_read(talk)[0])["entries"]
                       for m in [re.search(r"t(\d+)\.esd$", e[2])] if m}
            msb = Msb(dcx_read(fresh.read("map/mapstudio/%s.msb.dcx" % map_variants(fresh, stem)[0]))[0])
            here = {}
            for e in msb.enemies():
                s = by_entity.get(e["entity"], (None, None))[1] if e["entity"] > 0 else None  # -1: no entity id
                if s is None or s["model"] == "c0000" or s["kept"] not in (None, "npc"):
                    continue
                here[e["entity"]] = s
                if e["talk"] in scripts or 6000 <= s["think"] < 7000:
                    found.add(e["entity"])
            for instructions, _ in _load_events(fresh, stem + "_00_00").values():
                named = set(here) & {w for _, _, words in instructions for w in words}
                if named and any((b, i) == (2003, 22) and 1000 <= w[0] < 2000 for b, i, w in instructions if w):
                    found |= named
        self.assertEqual(found, set(NPC_ENTITIES))
        for entity in NPC_ENTITIES:
            self.assertEqual(by_entity[entity][1]["kept"], "npc", entity)
        # the Fishing Hamlet Priest (c4051 3600920, talk t360507): an NPC since 6 Nov (user), his row leaves the pool
        self.assertNotIn("c4051", self.census.candidates)
        # hostile human hunters (c0000: no talk, no name, no quest event) are free; named and talking ones stay (user, 6 Nov)
        human = {s["entity"]: s["kept"] for slots in self.census.slots.values() for s in slots if s["model"] == "c0000"}
        for entity in (3400910, 3400911, 3500931, 3500933, 3500934, 3500935, 2800700):  # old hunters, Yahar'gul hunters
            self.assertIsNone(human[entity], entity)
        for entity in (2300740, 3400922, 3500940, 2400730, 2400762):  # Alfred, Valtr, Yamamura, a talker, a quest hunter
            self.assertEqual(human[entity], "npc", entity)
        self.assertNotIn("c0000", self.census.candidates)
        # kinds that never attack away from home (user, 6 Nov) are no replacement; their own slots stay free
        from gamedata import HOME_ONLY_MODELS
        for model in HOME_ONLY_MODELS:
            self.assertNotIn(model, self.census.candidates)
        self.assertIsNone(by_entity[3400650][1]["kept"])
        self.assertEqual(by_entity[2100700][1]["model"], "c8030")
        # the Shadows' summoned snakes (m27 2700803-805, c5033; enabled on a Shadow's AI message 20 by 12704807) stay
        # snakes unless option shadows_summons frees them; their row is in the pool (user, 8 Nov)
        from gamedata import SUMMON
        for entity in (2700803, 2700804, 2700805):
            self.assertEqual((by_entity[entity][1]["model"], by_entity[entity][1]["kept"]), ("c5033", SUMMON))
        self.assertIn("c5033", self.census.candidates)
        from neighbours import group_limits, load_groups
        groups = load_groups()
        result = planner.plan(self.census.slots, self.census.candidates, self.census.sizes, groups,
                              group_limits(self.census, groups), 3, {"boss_swap": 1}, self.census.animations)
        moved = {(stem, part) for stem, changes in result["changes"].items() for part in changes}
        pinned = {(by_entity[e][0], by_entity[e][1]["part"]) for e in NPC_ENTITIES}
        self.assertFalse(moved & pinned)

    def test_bosses_stand_in_ordinary_slots_as_plain_enemies(self):
        """Option bosses_as_enemies (user, 7-8 Nov): a map rolls boss_enemy_chance and then holds 1 to boss_enemy_max
        different bosses in ordinary slots, never one arenas.ENEMY_OUT names, nor a huge one unless huge_bosses_as_enemies,
        with chalice bosses when chalice_bosses is on; every boss arena stays as the option found it; the slot keeps its own
        rewards and the boss's base HP is cut to 8 times the area's median (generate.boss_enemy_hp). Off: nothing of the kind."""
        import arenas
        import boss_events
        import chalice
        import difficult
        import generate
        import helpers
        from gamedata import BOSS_ENTITIES, HOME_ONLY_MODELS
        from neighbours import group_limits, load_groups
        c = self.census
        layout = c.npc_layout
        groups = load_groups()
        limits = group_limits(c, groups)
        index = planner._index(c.slots)
        fighters = planner.boss_fighters(c.slots, index, set())
        self.assertLessEqual(set(arenas.ENEMY_OUT) - {"lesser_demon"}, {f["key"] for f in fighters})  # a mod fight is no fighter of the game alone  # every name is a fighter's
        # the reasons, from the data they cite
        self.assertIn(12104870, boss_events.BODY_EVENTS[2100810])
        self.assertIn(index[2300800][1]["model"], HOME_ONLY_MODELS)
        self.assertTrue({2500800, 2600800, 2800800} <= set(helpers.FIGHTS))
        cups = chalice.fighters(self.files, c)
        huge = generate.huge_rows(c, [f["npc"] for f in fighters + cups])
        wide = {f["key"] for f in fighters + cups if f["npc"] in huge}
        self.assertEqual(wide, {"paarl", "ebrietas", "rom", "amelia", "ludwig", "ludwig_2", "one_reborn", "amygdala",
                                "chalice:bloodletting_beast", "chalice:loran_darkbeast", "chalice:watchdog"})
        plain = {f["key"]: f for f in planner.enemy_bosses(c.slots, index, set(), cups)}
        total, many = 0, 0
        for seed in range(1, 7):
            for options in ({"bosses_as_enemies": 1},
                            {"bosses_as_enemies": 1, "boss_swap": 1, "headroom": 200, "boss_enemy_chance": 100,
                             "boss_enemy_max": 3, "chalice_bosses": 1, "huge_bosses_as_enemies": seed % 2}):
                result = planner.plan(c.slots, c.candidates, c.sizes, groups, limits, seed, options, c.animations,
                                      extra_fighters=cups if options.get("chalice_bosses") else (), huge=huge)
                self.assertEqual(planner.check(result, c.slots, c.candidates, c.sizes, groups, limits, c.animations), [])
                top = options.get("boss_enemy_max", 1)
                for stem, parts in result["boss_enemies"].items():
                    self.assertTrue(1 <= len(parts) <= top)
                    self.assertEqual(len(set(parts.values())), len(parts))  # different bosses
                    many += len(parts) > 1
                    for part, key in parts.items():
                        slot = next(s for s in c.slots[stem] if s["part"] == part)
                        self.assertTrue(planner.boss_enemy_slot(slot))
                        self.assertNotIn(slot["entity"], BOSS_ENTITIES)
                        self.assertNotEqual(slot["entity"] % 1000 // 100, 8)  # no part a boss fight spawns
                        f = plain[key]
                        self.assertEqual(result["changes"][stem][part], (f["model"], f["think"], f["npc"]))
                        if not options.get("huge_bosses_as_enemies"):
                            self.assertNotIn(key, wide)
                        if not options.get("chalice_bosses"):
                            self.assertFalse(key.startswith("chalice:"))
                        total += 1
                if not options.get("boss_swap"):  # no arena body changes: the option leaves every arena alone
                    moved = {(stem, part) for stem, v in result["changes"].items() for part in v}
                    self.assertFalse({(index[e][0], index[e][1]["part"]) for e in BOSS_ENTITIES if e in index} & moved)
                off = planner.plan(c.slots, c.candidates, c.sizes, groups, limits, seed,
                                   dict(options, bosses_as_enemies=0), c.animations, huge=huge)
                self.assertEqual(off["boss_enemies"], {})
                self.assertNotIn("boss_enemies", off["report"])
                none = planner.plan(c.slots, c.candidates, c.sizes, groups, limits, seed,
                                    dict(options, boss_enemy_chance=0), c.animations, huge=huge)
                self.assertEqual(none["boss_enemies"], {})
        self.assertGreater(total, 40)
        self.assertGreater(many, 5)
        # the cut HP and the slot's own rewards, also with rewards = 1 (a boss row gives none: its arena does)
        result = planner.plan(c.slots, c.candidates, c.sizes, groups, limits, 2,
                              {"bosses_as_enemies": 1, "rewards": 1, "boss_enemy_chance": 100}, c.animations, huge=huge)
        hp = generate.boss_enemy_hp(c, result)
        self.assertEqual(len(hp), sum(len(v) for v in result["boss_enemies"].values()))
        slot_row, made = generate.clone_rows(c, result["changes"], result["options"], {}, hp)
        made = dict(made)
        for (stem, part), base in hp.items():
            slot = next(s for s in c.slots[stem] if s["part"] == part)
            npc = result["changes"][stem][part][2]
            own = layout.get(c.npc_rows[slot["npc"]], "hp")
            row = made[slot_row[(stem, part)]]
            self.assertEqual(layout.get(row, "hp"), base)
            final = int(base * c.hp_rate(layout.get(row, "spEffectID6")))
            self.assertLessEqual(final, generate.fight_hp(c, npc))  # never above its own fight (PLAN 14 fix 4)
            if final < generate.fight_hp(c, npc) - 20:  # not capped: 8 x the area median, at least the slot's own
                self.assertEqual(base, max(own, int(8 * difficult.area_median(c, stem))))
            for name in generate.SLOT_REWARDS:
                self.assertEqual(layout.get(row, name), layout.get(c.npc_rows[slot["npc"]], name), name)

    def test_a_boss_as_an_enemy_never_has_more_hp_than_in_its_own_fight(self):
        """PLAN 14 fix 4 and option 49 boss_enemy_hp (user 8 Nov): base HP = boss_enemy_hp x the area's median ordinary
        base HP (at least the slot's own), then the slot's area rate; the FINAL HP never exceeds the boss's fight HP (its
        row's HP x its own area rate). DATA: Gascoigne 1,440 x 1.411 = 2,031 in his fight; the Hunter's Nightmare (m34_00,
        median 80) slot c1051_0000 has rate 6.911: before the fix 640 x 6.911 = 4,423, now base 293, 2,024 HP at 8x and
        16x alike. A Central Yharnam (m24_01, median 80) slot at rate 1.0: 640 at 8x, 1,280 at 16x."""
        import generate
        c = self.census
        layout = c.npc_layout
        index = planner._index(c.slots)
        fighters = {f["key"]: f for f in planner.enemy_bosses(c.slots, index, set())}
        self.assertEqual(generate.fight_hp(c, fighters["gascoigne"]["npc"]), 2031)
        self.assertEqual(generate.fight_hp(c, fighters["cleric_beast"]["npc"]), 3015)

        def hp(stem, part, key, times, **extra):
            options = seedcode.normalize(dict({"bosses_as_enemies": 1, "boss_enemy_hp": times}, **extra))
            result = {"options": options, "boss_enemies": {stem: {part: key}}, "enemy_fighters": {key: fighters[key]}}
            base = generate.boss_enemy_hp(c, result)[(stem, part)]
            f = fighters[key]
            slot_row, made = generate.clone_rows(c, {stem: {part: (f["model"], f["think"], f["npc"])}}, options, {},
                                                 {(stem, part): base})
            row = dict(made)[slot_row[(stem, part)]]
            self.assertEqual(layout.get(row, "hp"), base)
            return base, int(base * c.hp_rate(layout.get(row, "spEffectID6")))
        nightmare = {s["part"]: s for s in c.slots["m34_00"]}["c1051_0000"]
        self.assertAlmostEqual(c.hp_rate(layout.get(c.npc_rows[nightmare["npc"]], "spEffectID6")), 6.911, places=3)
        for times in seedcode.BOSS_ENEMY_HP:
            self.assertEqual(hp("m34_00", "c1051_0000", "gascoigne", times), (293, 2024))
        self.assertEqual(hp("m34_00", "c1051_0000", "cleric_beast", 8), (436, 3013))
        self.assertEqual(hp("m34_00", "c1051_0000", "orphan", 8), (640, 4423))  # his fight: 19,216
        self.assertEqual(hp("m34_00", "c1051_0000", "orphan", 16), (1280, 8846))
        self.assertEqual(hp("m24_01", "c1170_0003", "gascoigne", 8), (640, 640))
        self.assertEqual(hp("m24_01", "c1170_0003", "gascoigne", 12), (960, 960))
        self.assertEqual(hp("m24_01", "c1170_0003", "gascoigne", 16), (1280, 1280))
        # scaling 1 (the replacement's own strength): his own rate 1.411 on any slot
        self.assertEqual(hp("m24_01", "c1170_0003", "gascoigne", 16, scaling=1), (1280, int(1280 * c.hp_rate(7003))))
        self.assertEqual(hp("m34_00", "c1051_0000", "gascoigne", 8, scaling=1), (640, int(640 * c.hp_rate(7003))))
        # every boss, a slot of every area rate of five late areas, both ends of the option: never above its fight
        for key, f in fighters.items():
            most = generate.fight_hp(c, f["npc"])
            for stem in ("m24_01", "m26_00", "m32_00", "m34_00", "m35_00"):
                rates = {}
                for s in c.slots[stem]:
                    if planner.boss_enemy_slot(s):
                        rates.setdefault(layout.get(c.npc_rows[s["npc"]], "spEffectID6"), s["part"])
                for p in rates.values():
                    for times in (8, 16):
                        self.assertLessEqual(hp(stem, p, key, times)[1], most, (key, stem, p, times))

    def test_chalice_enemies_always_give_the_slot_rewards(self):
        """PLAN 14 fix 5 (user 8 Nov): a chalice enemy (a model census.add_candidates joined; here the Merciless Watcher's
        chalice row 10304000, 316 echoes, lot 30402900) takes the slot's echoes and drop lots whatever option rewards
        says, never its dungeon row's; a main kind follows the option as before."""
        import generate
        from gamedata import Census
        c = Census(self.files)
        layout = c.npc_layout
        c.add_candidates({"c3040": [(304000, 10304000)]})
        self.assertEqual(c.joined, {"c3040"})
        self.assertEqual(self.census.joined, set())
        slot = next(s for s in c.slots["m24_01"] if s["kept"] is None and layout.get(c.npc_rows[s["npc"]], "getSoul") != 316)
        main_model = next(m for m in sorted(c.candidates) if m not in c.joined and m != slot["model"])
        main_think, main_npc = c.candidates[main_model][0]
        for rewards in (0, 1):
            options = seedcode.normalize({"chalice_enemies": 1, "rewards": rewards})
            slot_row, made = generate.clone_rows(c, {"m24_01": {slot["part"]: ("c3040", 304000, 10304000)}}, options)
            row = dict(made)[slot_row[("m24_01", slot["part"])]]
            for name in generate.SLOT_REWARDS:
                self.assertEqual(layout.get(row, name), layout.get(c.npc_rows[slot["npc"]], name), (rewards, name))
            self.assertNotEqual(layout.get(row, "itemLotId_1"), 30402900)
            slot_row, made = generate.clone_rows(c, {"m24_01": {slot["part"]: (main_model, main_think, main_npc)}}, options)
            row = dict(made)[slot_row[("m24_01", slot["part"])]]
            source = c.npc_rows[main_npc] if rewards else c.npc_rows[slot["npc"]]
            for name in generate.SLOT_REWARDS:
                self.assertEqual(layout.get(row, name), layout.get(source, name), (rewards, name))

    def test_off_nothing_is_summoned_in_the_shadows_arena(self):
        """Option shadows_summons off (user, 8 Nov): the summon event waits for an AI message no script sends; the rest
        of it (the disable at load) and every other event stay as they were."""
        import generate
        from gamedata import SUMMON_EVENT, GameFiles
        events = Emevd(dcx_read(GameFiles(GAME).read("event/m27_00_00_00.emevd.dcx"))[0])
        before = {e: events.instructions(e) for e in events.event_ids()}
        generate._no_summons(events)
        after = {e: events.instructions(e) for e in events.event_ids()}
        self.assertEqual({e for e in before if before[e] != after[e]}, {SUMMON_EVENT})
        changed = [(i, a[2]) for i, (b, a) in enumerate(zip(before[SUMMON_EVENT], after[SUMMON_EVENT])) if a != b]
        self.assertEqual(changed, [(4, (255, 2700800, generate.NEVER_SENT, 1)), (5, (255, 2700801, generate.NEVER_SENT, 1)),
                                   (6, (255, 2700802, generate.NEVER_SENT, 1))])

    def test_a_summoned_snake_slot_keeps_its_summon_alive(self):
        """The Shadows' snakes are free slots (user, 8 Nov); their event kills a summon that lacks SpEffect 5546 after
        5 s, so every replacement row there carries 5546 (gamedata.SUMMONS), and every pool row has room for it."""
        import generate
        c = self.census
        layout = c.npc_layout
        snakes = {s["part"]: s for s in c.slots["m27_00"] if s["entity"] in generate.SUMMONS}
        self.assertEqual(len(snakes), 3)
        options = seedcode.normalize({})
        full = []
        for model, rows in sorted(c.candidates.items()):
            for think, npc in sorted(rows):
                changes = {"m27_00": {part: (model, think, npc) for part in snakes}}
                slot_row, made = generate.clone_rows(c, changes, options)
                made = dict(made)
                room = any(layout.get(c.npc_rows[npc], "spEffectID%d" % k) <= 0 for k in range(8) if k != 6)
                full += [] if room else [npc]
                for part in snakes:
                    row = made[slot_row[("m27_00", part)]]
                    effects = [layout.get(row, "spEffectID%d" % k) for k in range(8)]
                    self.assertEqual(5546 in effects, room, (model, npc))
        self.assertEqual(full, [273400])  # no room: its summon despawns after 5 s (generate._mark)
        # an ordinary slot's row does not
        other = next(s for s in c.slots["m27_00"] if s["kept"] is None and s["entity"] not in generate.SUMMONS)
        model, (think, npc) = next(iter(sorted(c.candidates.items())))[0], sorted(next(iter(sorted(c.candidates.items())))[1])[0]
        slot_row, made = generate.clone_rows(c, {"m27_00": {other["part"]: (model, think, npc)}}, options)
        row = dict(made)[slot_row[("m27_00", other["part"])]]
        self.assertEqual([layout.get(row, "spEffectID%d" % k) for k in range(8)],
                         [layout.get(c.npc_rows[npc], "spEffectID%d" % k) if k != 6 else layout.get(row, "spEffectID6")
                          for k in range(8)])

    def test_held_runs_are_central_yharnams_caged_dogs(self):
        """User, 8 Nov: replaced caged dogs "didnt die ... flipped upside down after dying ... t posed". The runs that hold
        a free slot (gamedata.held_runs) are, game-wide, the six caged Rabid Dogs' immortal-until-hit runs (12415420) and
        the four barking ones' AI-off-for-good runs (12410380), all in m24_01, and the 16 Mensis scholars' immortal-until-
        Micolash runs (m26_00 12604890, PLAN 14 item 7); a replacement there needs none of their animations, the vanilla
        dog's row still feeds no pool."""
        from gamedata import HOLD_CAGED, HOLD_IMMORTAL
        found = {(stem, s["entity"], run[2], run[3]) for stem, slots in self.census.slots.items() for s in slots
                 for run in s["held"] if s["kept"] is None}
        # kept parts never get another model: Fishing Hamlet's quest NPCs 3600905 / 3600920 (13600950, 13600953,
        # 13600940 make them immortal for good) have held runs no replacement meets
        self.assertTrue(all(s["kept"] for s in self.census.slots["m36_00"] if s["entity"] in (3600905, 3600920)))
        immortal = {2410271, 2410272, 2410275, 2410277, 2410278, 2410279}
        caged = {2410275, 2410277, 2410278, 2410279}
        scholars = {2600161, 2600162, 2600163, 2600164, 2600165, 2600167, 2600169, 2600170, 2600171, 2600172, 2600173,
                    2600175, 2600176, 2600177, 2600178, 2600179}
        self.assertEqual(found, {("m24_01", e, 12415420, HOLD_IMMORTAL) for e in immortal}
                         | {("m24_01", e, 12410380, HOLD_CAGED) for e in caged}
                         | {("m26_00", e, 12604890, HOLD_IMMORTAL) for e in scholars})
        mensis = {s["entity"]: s for s in self.census.slots["m26_00"]}
        for e in scholars:
            self.assertIsNone(mensis[e]["kept"])
            self.assertNotIn(7002, mensis[e]["needs"])  # the collapse 12604890 forces on Micolash's defeat
        by_entity = {s["entity"]: s for s in self.census.slots["m24_01"]}
        for e in immortal:
            self.assertIsNone(by_entity[e]["kept"])
        for e in caged:
            self.assertEqual(by_entity[e]["needs"], ())  # vanilla: (3020,), the bark 12410380 loops
            self.assertEqual(by_entity[e]["poses"], ())
        for e in immortal - caged:  # the two that break out keep 12410378 (3021 rattle, 3001 jump; AI back on)
            self.assertEqual(by_entity[e]["needs"], (3001, 3021))
        self.assertNotIn((124400, 124401), self.census.candidates.get("c1240", []))

    def test_no_replaced_enemy_is_left_immortal_or_without_ai(self):
        """Every map, read here on its own: a free slot that an event of its map makes immortal (2004[12] on, written or
        through a run's parameter), or turns AI off (2004[01] off) with nothing in the map turning it back on or
        disabling it, is one the generator releases when replaced (its runs are in the slot's `held`). No exception is
        left (the Mensis scholars' m26_00 12604890 was one until 8 Nov)."""
        from gamedata import STEMS, GameFiles, _load_events
        fresh = GameFiles(GAME)
        common = _load_events(fresh, "common")
        left, released = [], 0
        for stem in STEMS:
            free = {s["entity"]: s for s in self.census.slots[stem] if s["kept"] is None and s["entity"] > 0}
            own = _load_events(fresh, stem + "_00_00")
            every = dict(common)
            every.update(own)
            hits = {}  # entity: {(event or template, what, call site or None)}
            woken = set()

            def lines(event):
                instructions, params = every[event]
                for i, (bank, iid, words) in enumerate(instructions):
                    if words:
                        yield i, bank, iid, words, params.get(i, {}).get(0)

            calls = [(e, k, w) for e, (ins, _) in own.items() for k, (b, i, w) in enumerate(ins)
                     if b == 2000 and i in (0, 6) and len(w) >= 2]
            for event in own:
                for i, bank, iid, words, src in lines(event):
                    if src is None and words[0] in free:
                        if (bank, iid) == (2004, 12) and words[1] == 1:
                            hits.setdefault(words[0], set()).add((event, "immortal", None))
                        elif (bank, iid) == (2004, 1) and words[1] == 0:
                            hits.setdefault(words[0], set()).add((event, "ai off", None))
                        elif (bank, iid) in ((2004, 1), (2004, 5)) and words[1] == 1 or (bank, iid) == (2004, 5):
                            woken.add(words[0])
            for event, k, words in calls:
                target, first = (words[1], 2) if words[1] in every else (words[0], 1)
                if target not in every:
                    continue
                for i, bank, iid, w, src in lines(target):
                    if src is None or first + src // 4 >= len(words):
                        continue
                    entity = words[first + src // 4]
                    if (bank, iid) in ((2004, 1), (2004, 5)) and (w[1] == 1 or (bank, iid) == (2004, 5)):
                        woken.add(entity)
                    if entity not in free:
                        continue
                    if (bank, iid) == (2004, 12) and w[1] == 1:
                        hits.setdefault(entity, set()).add((target, "immortal", (event, k)))
                    elif (bank, iid) == (2004, 1) and w[1] == 0:
                        hits.setdefault(entity, set()).add((target, "ai off", (event, k)))
            for entity, found in sorted(hits.items()):
                held = {run[:2] for run in free[entity]["held"]}
                for template, what, site in sorted(found, key=str):
                    if what == "ai off" and entity in woken:
                        continue
                    if site is not None and site in held:
                        released += 1
                        continue
                    left.append((stem, entity, template, what))
        self.assertEqual(left, [])
        self.assertEqual(released, 26)

    def test_a_replaced_caged_dog_goes_without_its_runs(self):
        """generate.release: the call lines of a replaced dog's held runs become labels, nothing else in the file moves,
        and the map then holds no run on it; a dog that draws its own kind again keeps its runs."""
        import generate
        from gamedata import GameFiles, _events, _load_events, held_runs
        fresh = GameFiles(GAME)
        payload = dcx_read(fresh.read("event/m24_01_00_00.emevd.dcx"))[0]
        slots = {s["entity"]: s for s in self.census.slots["m24_01"]}
        final = {slots[e]["part"]: ("c1100", 0, 0) for e in (2410271, 2410275)}
        final[slots[2410277]["part"]] = ("c1240", 124400, 124401)
        released = generate.released_runs(self.census, "m24_01", final)
        self.assertEqual({(r[0], r[3]) for r in released},
                         {(2410271, 12415420), (2410275, 12415420), (2410275, 12410380)})
        out = generate.release(payload, released)
        self.assertEqual(len(out), len(payload))
        before, after = Emevd(payload), Emevd(out)
        sites = {(r[1], r[2]) for r in released}
        for event in before.event_ids():
            for k, (old, new) in enumerate(zip(before.instructions(event), after.instructions(event))):
                if (event, k) in sites:
                    self.assertEqual(new[0], LABEL_BANK)
                else:
                    self.assertEqual(old, new, (event, k))
        held = held_runs(_events(out), _load_events(fresh, "common"))
        self.assertNotIn(2410271, held)
        self.assertNotIn(2410275, held)
        self.assertIn(2410277, held)  # still the dog: still caged
        self.assertEqual(generate.release(payload, []), payload)

    @staticmethod
    def _ai_sets(events):
        """{entity: {(event, line, AI id or None)}}: what every Set Character AI ID line (2004[19]) sets, read here on its
        own: a written entity, or each run's argument (runs found by a template id in a call's first two words)."""
        ids = set(events.event_ids())
        out = {}
        for event in ids:
            params = events.params(event)
            for i, (bank, iid, words) in enumerate(events.instructions(event)):
                if (bank, iid) != (2004, 19):
                    continue
                fed = params.get(i, {})
                if 0 not in fed:
                    out.setdefault(words[0], set()).add((event, i, words[1] if 4 not in fed else None))
                    continue
                for caller in ids:
                    for bank2, iid2, run in events.instructions(caller):
                        if bank2 == 2000 and iid2 in (0, 6) and event in run[:2] and len(run) >= 2:
                            first = run.index(event) + 1
                            ai = run[first + fed[4] // 4] if 4 in fed else words[1]
                            out.setdefault(run[first + fed[0] // 4], set()).add((event, i, ai))
        return out

    @staticmethod
    def _gravity(events, entity):
        """[(event, line, on)] of every 2004[10] line on this entity, written or through a run."""
        ids = set(events.event_ids())
        out = []
        for event in ids:
            params = events.params(event)
            for i, (bank, iid, words) in enumerate(events.instructions(event)):
                if (bank, iid) != (2004, 10):
                    continue
                if 0 not in params.get(i, {}):
                    if words[0] == entity:
                        out.append((event, i, words[1]))
                    continue
                for caller in ids:
                    for bank2, iid2, run in events.instructions(caller):
                        if bank2 == 2000 and iid2 in (0, 6) and event in run[:2]:
                            if run[run.index(event) + 1 + params[i][0] // 4] == entity:
                                out.append((event, i, words[1]))
        return sorted(out)

    def test_no_replaced_enemy_gets_an_ai_id_from_an_event(self):
        """User 8 Nov: replaced resting Yharnamites "look at me and don't react" (m24_01 12415150 set the Yharnamite's AI
        IDs 263499 then 263450/263440 on the new enemy). Every map: with every other ordinary part replaced, no event
        line sets a replaced part's AI ID any more, while the parts that keep their kind keep exactly their vanilla AI
        lines; nothing else changes but labels and the redirected calls; a replaced part's gravity, where an event turns
        it off, an event still turns on again (as vanilla)."""
        import generate
        from gamedata import BOSS_ENTITIES, ESCORT_ARENA, STEMS, SUMMONS, GameFiles
        fresh = GameFiles(GAME)
        fight = BOSS_ENTITIES | set(ESCORT_ARENA) | set(SUMMONS)
        hit, copies = set(), 0
        for stem in STEMS:
            payload = dcx_read(fresh.read("event/%s_00_00.emevd.dcx" % stem))[0]
            before = Emevd(payload)
            vanilla = self._ai_sets(before)
            ordinary = sorted(s["entity"] for s in self.census.slots[stem] if s["entity"] > 0 and s["entity"] not in fight)
            replaced, kept = frozenset(ordinary[::2]), set(ordinary[1::2])
            report = []
            after = Emevd(generate.release(payload, [], replaced, report))
            sets = self._ai_sets(after)
            for entity in replaced:
                self.assertEqual(sets.get(entity, set()), set(), (stem, entity))
                if entity in vanilla:
                    hit.add((stem, entity))
                    gravity = self._gravity(after, entity)
                    if any(on == 0 for _, _, on in gravity):  # (m36_00: event 50 turns it off, the part's run back on)
                        self.assertTrue(any(on == 1 for _, _, on in gravity), (stem, entity))
            for entity in kept:
                self.assertEqual(sets.get(entity, set()), vanilla.get(entity, set()), (stem, entity))
            new = set(after.event_ids()) - set(before.event_ids())
            copies += len(new)
            self.assertEqual({r[4] for r in report if r[4]}, new)
            for event in before.event_ids():
                for k, (old, now) in enumerate(zip(before.instructions(event), after.instructions(event))):
                    if old != now:
                        diff = [(a, b) for a, b in zip(old[2], now[2]) if a != b]
                        self.assertTrue(now[0] == LABEL_BANK and old[:2] == (2004, 19)
                                        or old[:2] == now[:2] and old[0] == 2000 and len(diff) == 1 and diff[0][1] in new,
                                        (stem, event, k, old, now))
        self.assertTrue({("m24_01", e) for e in (2410100, 2410101, 2410103)} & hit)
        self.assertGreater(len(hit), 50)
        self.assertGreater(copies, 0)

    def test_a_replaced_resting_yharnamite_loses_its_ai_lines(self):
        """generate.release_ai on m24_01's resting Yharnamites (12415150, runs for 2410100/101/103): all three replaced,
        the template's two AI lines become labels in place; two replaced, a copy without them runs for those two and the
        third keeps the original; none replaced, the file is untouched."""
        import generate
        payload = dcx_read(self.files.read("event/m24_01_00_00.emevd.dcx"))[0]
        report = []
        out = Emevd(generate.release(payload, [], frozenset({2410100, 2410101, 2410103}), report))
        self.assertEqual({(r[0], r[1], r[2], r[4]) for r in report},
                         {(e, 12415150, line, None) for e in (2410100, 2410101, 2410103) for line in (2, 14)})
        self.assertEqual([out.instructions(12415150)[k][0] for k in (2, 14)], [LABEL_BANK, LABEL_BANK])
        self.assertEqual(out.instructions(12415150)[15], (2004, 10, (0, 1)))  # gravity back on stays
        report = []
        out = Emevd(generate.release(payload, [], frozenset({2410100, 2410101}), report))
        copy = {r[4] for r in report}
        self.assertEqual(len(copy), 1)
        copy = copy.pop()
        self.assertEqual(copy // 1000, 12415)
        self.assertEqual(out.instructions(12415150), Emevd(payload).instructions(12415150))
        self.assertEqual([out.instructions(copy)[k][0] for k in (2, 14)], [LABEL_BANK, LABEL_BANK])
        sets = self._ai_sets(out)
        self.assertEqual(sets[2410103], self._ai_sets(Emevd(payload))[2410103])
        self.assertEqual({a for _, line, a in sets[2410103] if line == 2}, {263499})  # the resting AI, vanilla's
        self.assertNotIn(2410100, sets)
        self.assertNotIn(2410101, sets)
        self.assertEqual(generate.release(payload, [], frozenset()), payload)

    def test_turrets_stay_and_are_no_replacement(self):
        """User, 5 Nov: the Old Yharnam gatling stays a gatling, its model goes nowhere else; every other part that cannot
        move on its own is kept the same way. The table is re-derived here from the game files (gamedata.TURRET_MODELS)."""
        from gamedata import GameFiles, LOCOMOTION, TURRET, TURRET_MODELS, animations_of
        import difficult
        fresh = GameFiles(GAME)  # a reader of its own: the anibnd files read here never reach the class's input hash
        own = {}
        found = set()
        for slots in self.census.slots.values():
            for s in slots:
                if s["kept"] not in (None, TURRET) or s["npc"] not in self.census.npc_rows:
                    continue
                if s["model"] not in own:
                    own[s["model"]] = animations_of(fresh, s["model"])
                ids = own[s["model"]]
                if not ids or any(lo <= a < hi for a in ids for lo, hi in LOCOMOTION):
                    continue
                think = s["think"]
                if think <= 1:
                    think = self.census.npc_layout.get(self.census.npc_rows[s["npc"]], "aiThinkId")
                if think not in self.census.think_rows:
                    continue
                goal = self.census.think_layout.get(self.census.think_rows[think], "battleGoalID")
                team = self.census.npc_layout.get(self.census.npc_rows[s["npc"]], "teamType")
                if goal != 11000 and team != 26:
                    found.add(s["model"])
        self.assertEqual(found, set(TURRET_MODELS))
        kept = sorted((stem, s["part"], s["entity"]) for stem, slots in self.census.slots.items() for s in slots
                      if s["kept"] == TURRET)
        self.assertIn(("m23_00", "c7100_0000", 2300730), kept)
        self.assertEqual(len(kept), 14)  # 1 gatling, 9 bullet parts, 3 spines, 1 brain
        self.assertEqual({s["model"] for slots in self.census.slots.values() for s in slots if s["kept"] == TURRET},
                         set(TURRET_MODELS))
        self.assertFalse(set(TURRET_MODELS) & set(self.census.candidates))
        self.assertFalse(set(TURRET_MODELS) & {r["model"] for r in difficult.rows(self.census)})
        from neighbours import group_limits, load_groups
        groups = load_groups()
        result = planner.plan(self.census.slots, self.census.candidates, self.census.sizes, groups,
                              group_limits(self.census, groups), 6, {"boss_swap": 1}, self.census.animations)
        moved = {(stem, part) for stem, changes in result["changes"].items() for part in changes}
        self.assertFalse(moved & {(stem, part) for stem, part, _ in kept})
        used = {v[0] for changes in result["changes"].values() for v in changes.values()}
        self.assertFalse(used & set(TURRET_MODELS))

    def test_param_and_bnd_round_trip(self):
        payload, _ = dcx_read(self.files.read("param/gameparam/gameparam.parambnd.dcx"))
        bnd = bnd4_read(payload)
        self.assertEqual(bnd4_write(bnd), payload)
        for name in ("\\NpcParam.param", "\\NpcThinkParam.param"):
            data = bnd4_find(bnd, name)[3]
            self.assertEqual(Param(data).write(), data)

    def test_layout_matches_row_size(self):
        self.assertEqual(self.census.npc_layout.size, self.census.npc.size)
        self.assertEqual(self.census.think_layout.size, self.census.think.size)

    def test_lua_indexes_round_trip(self):
        for stem, bnd in self.census.archives.items():
            a = ailua.Archive(stem + "_00_00", {"header": bnd["header"], "entries": [list(e) for e in bnd["entries"]]})
            v, z, rows = ailua.info_read(a.info[3])
            self.assertEqual(ailua.info_write(v, z, rows) + ailua.info_padding(a.info[3]), a.info[3], stem)
            self.assertFalse(ailua.info_padding(a.info[3]).strip(b"\0"), stem)
            self.assertEqual(ailua.gnl_write(ailua.gnl_read(a.gnl[3])) + ailua.gnl_padding(a.gnl[3]), a.gnl[3], stem)

    def test_msb_edit_changes_only_what_it_says(self):
        from gamedata import map_variants
        for stem in ("m24_01", "m27_00"):
            for variant in map_variants(self.files, stem):
                payload, _ = dcx_read(self.files.read("map/mapstudio/%s.msb.dcx" % variant))
                msb = Msb(payload)
                self.assertEqual(msb.bytes(), payload)  # nothing done, nothing changed
                before = {e["part"]: e for e in msb.enemies()}
                names_before = msb.part_names()
                secs_before = {k: len(v[2]) for k, v in sections(msb.data).items()}
                free = [s["part"] for s in self.census.slots[stem] if s["kept"] is None and s["part"] in before][:12]
                msb.swap_enemies({p: ("c1260" if stem != "m27_00" else "c4020", 777, 888) for p in free})
                after = {e["part"]: e for e in msb.enemies()}
                self.assertEqual(msb.part_names(), names_before)
                for part, old in before.items():
                    new = after[part]
                    self.assertEqual(new["entity"], old["entity"])
                    if part in free:
                        self.assertEqual((new["think"], new["npc"]), (777, 888))
                    else:
                        self.assertEqual((new["model"], new["think"], new["npc"]), (old["model"], old["think"], old["npc"]))
                secs_after = {k: len(v[2]) for k, v in sections(msb.data).items()}
                for k in secs_before:
                    self.assertEqual(secs_after[k], secs_before[k] + (k == MODELS and secs_after[MODELS] != secs_before[MODELS]))
                # every model's instance count equals the parts that use it
                counts = {}
                names = msb.model_names()
                for at in sections(msb.data)[PARTS][2][:-1]:
                    index = int.from_bytes(msb.data[at + 0x1c:at + 0x20], "little", signed=True)
                    counts[index] = counts.get(index, 0) + 1
                models = sections(msb.data)[MODELS][2][:-1]
                for i, at in enumerate(models):
                    self.assertEqual(int.from_bytes(msb.data[at + 0x18:at + 0x1c], "little", signed=True), counts.get(i, 0), names[i])

    def test_sweep_1000_seeds(self):
        from neighbours import group_limits, load_groups
        groups = load_groups()
        limits = group_limits(self.census, groups)
        presets = [{}, {"enemy_chance": 50}, {"exclude": (2630, 1100, 4020), "scaling": 1}, {"variety": 3}]
        for i in range(1000):
            options = presets[i % len(presets)]
            result = planner.plan(self.census.slots, self.census.candidates, self.census.sizes, groups, limits, i, options, self.census.animations)
            problems = planner.check(result, self.census.slots, self.census.candidates, self.census.sizes, groups, limits, self.census.animations)
            self.assertEqual(problems, [], "seed %d %r" % (i, options))
            if i == 0:
                self.assertGreater(sum(len(c) for c in result["changes"].values()), 500)

    def test_every_candidate_script_exists_somewhere(self):
        every = set().union(self.census.common_scripts, *self.census.script_files.values())
        for model, rows in self.census.candidates.items():
            for think, _ in rows:
                battle, logic = self.census.goals(think)
                self.assertIn(ailua.script_name(battle, "battle"), every, model)
                if logic > 0:
                    self.assertIn(ailua.script_name(logic, "logic"), every, "%s think %d" % (model, think))


@needs_game
class Determinism(unittest.TestCase):
    def test_golden_output(self):
        """A code's output must not drift by accident. After a change that means to alter output, run this file with
        BB_RANDOMIZER_GOLDEN=write and raise GENERATOR_MAJOR if anything has been released. Hashes only, no game data."""
        import generate
        path = os.path.join(os.path.dirname(__file__), "golden.json")
        with open(path, encoding="utf-8") as f:
            golden = json.load(f)
        code = seedcode.encode(20261102, {"boss_swap": 1, "treasure": 1, "shops": 1, "drops": 1})
        with tempfile.TemporaryDirectory() as out:
            folder, spoiler = generate.generate(GAME, [], out, code, "world")
            with open(os.path.join(folder, "randomizer_manifest.json"), encoding="utf-8") as f:
                manifest = json.load(f)
        # read_hash, not input_hash: the input hash counts the generator's own build (buildid.py), so it changes with every
        # commit to the generator; the game files depend only on the read hash and on what the generator does.
        got = {"code": code, "read_hash": spoiler["read_hash"], "files": hashlib.sha256(json.dumps(manifest["files"], sort_keys=True).encode()).hexdigest()}
        if os.environ.get("BB_RANDOMIZER_GOLDEN") == "write":
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(got, f, indent=1, sort_keys=True)
                f.write("\n")
            return
        if golden.get("read_hash") != got["read_hash"]:
            self.skipTest("another game input")
        self.assertEqual(golden, got)

    def test_manifest_and_plain_row_names(self):
        import generate
        code = seedcode.encode(77, {"enemy_chance": 100})
        with tempfile.TemporaryDirectory() as out:
            folder, spoiler = generate.generate(GAME, [], out, code.lower().replace("0", "o"), "w", "nocturne@1.0")
            # no boss moves, so no phase copies: no host-only list at all (hostonly.party_text)
            self.assertFalse(os.path.exists(os.path.join(folder, "party_host_only.txt")))
            with open(os.path.join(folder, "randomizer_manifest.json"), "rb") as f:
                raw = f.read()
            manifest = json.loads(raw)
            self.assertEqual(manifest["code"], code)  # the canonical spelling, whatever was typed
            self.assertEqual(manifest["mod_inputs"], [])
            import buildid
            self.assertEqual(manifest["generator_build"], buildid.build_id())
            self.assertEqual(spoiler["generator_build"], buildid.build_id())
            self.assertEqual(len(manifest["game_input"]), 64)
            self.assertEqual(manifest["lobby_label"], "nocturne@1.0+rando%d:%s+w:%s+g:%s+i:%s" % (
                seedcode.GENERATOR_MAJOR, code.lower(), spoiler["input_hash"][:8], buildid.build_id()[:8],
                manifest["game_input"][:8]))
            self.assertNotIn("credit", manifest)
            self.assertFalse(os.path.exists(os.path.join(folder, "randomizer.stamp")))

    @staticmethod
    def party_mods_hash(layers):
        """The party mods hash engine/start/mods.cpp WritePartyModsFile computes over these mod folders in load order
        (dvdroot_ps4 layout): each folded game path's winning file, cosmetic folders and texture files left out, no mod
        names."""
        cosmetic = {"adhoc", "facegen", "font", "menu", "movie", "msg", "mtd", "parts", "sfx", "shader", "sound"}
        suffixes = (".tpf", ".tpf.dcx", ".tpfbhd", ".tpfbdt", ".texbnd", ".texbnd.dcx", ".dds")
        owners = {}
        for layer in layers:
            root = os.path.join(layer, "dvdroot_ps4")
            for here, _, names in os.walk(root):
                for name in names:
                    full = os.path.join(here, name)
                    owners[("dvdroot_ps4/" + os.path.relpath(full, root).replace("\\", "/")).lower()] = full
        h = hashlib.sha256(b"bbparty-mods-1")
        for key in sorted(owners):
            parts = key.split("/")
            if (len(parts) > 2 and parts[1] in cosmetic) or parts[-1].endswith(suffixes):
                continue
            with open(owners[key], "rb") as f:
                digest = hashlib.sha256(f.read()).hexdigest()
            h.update(b"\0file\0" + key.encode("utf-8") + b"\0" + digest.encode("ascii"))
        return h.hexdigest()

    def test_one_code_twice_on_other_folders_and_hash_seeds_writes_the_same_world(self):
        """A world depends only on the code, the files read and the generator: two processes with other PYTHONHASHSEEDs,
        other out folders and a base mod copy under another folder name write the same folder name and the same bytes
        (every file the party mod hash covers and the rest), the same spoiler and the same manifest but for the mods' names."""
        import shutil
        code = seedcode.encode(424242, {"boss_swap": 1, "treasure": 1, "shops": 1, "drops": 1, "starters": 1,
                                        "witch_mix": 1, "difficult_enemies": 1, "chalice_bosses": 1, "enemy_chance": 75})
        runs = []
        with tempfile.TemporaryDirectory() as tmp:
            for hash_seed, out_name, mod_name in (("1", "out", "Nocturne"), ("4242", "out two é", "Nocturne v1.0.1 refresh")):
                mod = os.path.join(tmp, "mods", mod_name)
                rel = os.path.join("dvdroot_ps4", "event", "common.emevd.dcx")
                os.makedirs(os.path.dirname(os.path.join(mod, rel)))
                shutil.copyfile(os.path.join(GAME, rel), os.path.join(mod, rel))
                out = os.path.join(tmp, out_name)
                env = dict(os.environ, PYTHONHASHSEED=hash_seed)
                subprocess.run([sys.executable, os.path.join(TOOL, "randomizer.py"), "generate", "--game", GAME, "--code", code,
                                "--out", out, "--mods", mod, "--base-label", "nocturne@1.0"], check=True, env=env, stdout=subprocess.DEVNULL)
                worlds = os.listdir(out)
                self.assertEqual(len(worlds), 1)
                folder = os.path.join(out, worlds[0])
                files = {}
                for here, _, names in os.walk(folder):
                    for name in names:
                        with open(os.path.join(here, name), "rb") as f:
                            files[os.path.relpath(os.path.join(here, name), folder).replace("\\", "/")] = f.read()
                # the co-op check: base mod then world, as world mode loads them (same files, any folder names)
                runs.append((worlds[0], files, (tmp, out, mod, GAME), self.party_mods_hash([mod, folder])))
        (name_a, a, paths_a, party_a), (name_b, b, paths_b, party_b) = runs
        self.assertEqual(name_a, name_b)
        self.assertEqual(party_a, party_b)  # a guest on the same base files under another name passes the party check
        self.assertEqual(sorted(a), sorted(b))
        self.assertGreater(len([k for k in a if k.startswith("dvdroot_ps4/")]), 10)
        for rel in a:
            if rel != "randomizer_manifest.json":
                self.assertTrue(a[rel] == b[rel], rel + " differs")
        ma, mb = json.loads(a["randomizer_manifest.json"]), json.loads(b["randomizer_manifest.json"])
        # co-op: the host's list of phase copies no listed id holds (hostonly.party_ids) sits at the world's root beside
        # dvdroot_ps4 (no game file, so in no party or mods hash) and, like the spoiler, not in the
        # manifest's files; the same in both builds (compared above); each id an event of its map's written file
        import hostonly
        listed = a[hostonly.PARTY_FILE].decode("ascii")
        ids = [int(line) for line in listed.splitlines() if not line.startswith("#")]
        self.assertTrue(ids)
        self.assertEqual(ids, sorted(set(ids)))
        self.assertEqual(hostonly.party_text(ids), listed)
        self.assertFalse([k for k in a if k.endswith(hostonly.PARTY_FILE) and k != hostonly.PARTY_FILE])
        self.assertFalse([k for k in ma["files"] if k.endswith(hostonly.PARTY_FILE)])
        reports = {s: r.get("host_only") for s, r in json.loads(a["randomizer_spoiler.json"])["boss_events"].items()}
        self.assertEqual(ids, hostonly.party_ids(reports))
        ranks = {u[0]: u[3] for r in reports.values() if r for u in r.get("uncovered", ())}
        for event in ids:
            rel = "dvdroot_ps4/event/m%02d_%02d_00_00.emevd.dcx" % (event // 100000 % 100, event // 10000 % 10)
            self.assertIn(event, Emevd(dcx_read(a[rel])[0]).index, event)
            if event // 100000 % 100 in (25, 26, 35):  # maps that list no ids: copies of listed events only
                self.assertEqual(ranks[event], 0, event)
        self.assertEqual((ma["mods"], mb["mods"]), (["Nocturne"], ["Nocturne v1.0.1 refresh"]))
        self.assertEqual([m["files"] for m in ma["mod_inputs"]], [1])
        for m in (ma, mb):  # the folder names are the only difference
            del m["mods"]
            del m["mod_inputs"][0]["name"]
        self.assertEqual(ma, mb)
        for raw, paths in ((a, paths_a), (b, paths_b)):
            text = (raw["randomizer_manifest.json"] + raw["randomizer_spoiler.json"]).decode("utf-8").lower()
            for path in paths:
                for spelling in (path, path.replace("\\", "/"), json.dumps(path)[1:-1]):
                    self.assertNotIn(spelling.lower(), text)


if __name__ == "__main__":
    unittest.main()
