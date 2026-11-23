"""Option coiled_host (user, 7 Nov: "both replace bosses and get replaced"): Nocturne's Coiled Host, the Shadows of
Yharnam's second phase, is a boss of the pool, and his own place draws a boss. Found in the inputs (gamedata.phase_bodies,
arenas.PHASES), never listed; without Nocturne the option does nothing. The fight checks reuse test_fight_ends' checker.
Run: python -I -m unittest discover -s tools/randomizer/tests. Reads (never writes) the game at BB_GAME and Nocturne
v2.0.0+ at BB_NOCTURNE; the Nocturne cases skip without it."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import arenas
from containers import dcx_read
from emevd import Emevd
from test_fight_ends import AMELIA, CLERIC, GASCOIGNE, LOGARIUS, LUDWIG, NURSE, ORPHAN, Every, check_case, fighter

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
# the newest installed Nocturne; a fixed v2.0.0 path silently skipped these cases once v3.0.0 replaced it
from noc import NOCTURNE  # the newest installed Nocturne (BB_NOCTURNE overrides)
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")
needs_nocturne = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")) and NOCTURNE and os.path.isdir(NOCTURNE),
                                     "no game files or no Nocturne v2.0.0+")
HOST, SHADOWS, ONE_REBORN = 2700806, 2700800, 2800800  # DATA, Nocturne v2.0.0 m27: c5030 row 503000; body 1


def stem_of(entity):
    return "m%02d_%02d" % (entity // 100000, entity // 10000 % 10)


class Noc:
    """The game with Nocturne over it, read once."""
    _files, _census, _events, _variants = None, None, {}, {}

    @classmethod
    def files(cls):
        if cls._files is None:
            from gamedata import GameFiles
            cls._files = GameFiles(GAME, [NOCTURNE])
        return cls._files

    @classmethod
    def census(cls):
        if cls._census is None:
            from gamedata import Census
            cls._census = Census(cls.files())
        arenas.set_phases(cls._census.phases, cls._census.phase_helpers)  # another module's census may have read the game alone since
        return cls._census

    @classmethod
    def event_data(cls, stem):
        if stem not in cls._events:
            cls._events[stem] = dcx_read(cls.files().read("event/%s_00_00.emevd.dcx" % stem))[0]
        return cls._events[stem]

    @classmethod
    def origin_events(cls, entity):
        return Emevd(cls.event_data(stem_of(entity)))

    @classmethod
    def variants(cls, stem):
        if stem not in cls._variants:
            from gamedata import map_variants
            cls._variants[stem] = {v: dcx_read(cls.files().read("map/mapstudio/%s.msb.dcx" % v))[0]
                                   for v in map_variants(cls.files(), stem)}
        return cls._variants[stem]

    @classmethod
    def origin_msb(cls, entity):
        variants = cls.variants(stem_of(entity))
        return variants[sorted(variants)[0]]


def build(stem, placed, modes):
    """The map's events as generate builds them, on Nocturne (test_fight_ends.build, other inputs)."""
    import helpers
    from generate import patched_events
    fighters = {str(o) if o is not None else "chalice": fighter(o) for o in placed.values()}
    by_body = {e: fighters[str(o) if o is not None else "chalice"] for e, o in placed.items()}
    events = Emevd(Noc.event_data(stem))
    _, helper_report = helpers.apply_stem(by_body, events, Noc.variants(stem), Noc.origin_events, Noc.origin_msb)
    patched, _, report = patched_events(events.bytes(), stem, {e: f["key"] for e, f in by_body.items()}, fighters, modes,
                                        {"c0000": Every()}, Noc.origin_events, events.sources)
    travellers = {body: {b["home"]: b["entity"] for b in bodies} for body, bodies in helper_report["bodies"].items()}
    return Emevd(patched if patched is not None else events.bytes()), Emevd(Noc.event_data(stem)), travellers, report


def lines(events, event, bank, iid):
    return [tuple(w) for b, d, w in events.instructions(event) if (b, d) == (bank, iid)]


class Detection(unittest.TestCase):
    def test_phase_bodies_need_bar_death_and_reward_in_the_defeat_event(self):
        from gamedata import phase_bodies
        shadows = arenas.BY_KEY["shadows"]
        wake = [(2003, 11, (1, 2700899, 0, 1)), (4, 0, (2, 2700899, 1)), (2003, 12, (2700899,))]
        enemies = {"m27_00": {2700899: {}}}
        self.assertEqual(phase_bodies({"m27_00": {shadows.flags[0]: (wake, {})}}, enemies), {2700899: "shadows"})
        for k in range(3):  # any one missing: no phase body
            self.assertEqual(phase_bodies({"m27_00": {shadows.flags[0]: (wake[:k] + wake[k + 1:], {})}}, enemies), {})
        self.assertEqual(phase_bodies({"m27_00": {shadows.flags[0]: (wake, {})}}, {"m27_00": {}}), {})  # not placed
        # a table body is never a phase body
        own = [(2003, 11, (1, SHADOWS, 0, 1)), (4, 0, (2, SHADOWS, 1)), (2003, 12, (SHADOWS,))]
        self.assertEqual(phase_bodies({"m27_00": {shadows.flags[0]: (own, {})}}, {"m27_00": {SHADOWS: {}}}), {})

    def test_phase_helpers_are_the_parts_his_events_drive(self):
        """helpers.phase_helpers on a made-up m27: an event naming the body and a part travels with the part; a template
        brings its runs; the defeat event, the hold and an event naming only boss bodies stay; a flag the defeat event
        turns on and a moved event reads is the start."""
        import helpers
        shadows = arenas.BY_KEY["shadows"]
        defeat = shadows.flags[0]
        body, ball, owner, gen, flag = 2700899, 2700898, 2700897, 2705099, 12704899
        own = {
            0: ([(2000, 0, (0, 12704890, ball, gen)), (2000, 0, (1, 12704890, ball + 100, gen))], {}),
            defeat: ([(2003, 2, (flag, 1)), (2004, 1, (body, 1)), (2003, 12, (body,))], {}),
            12704880: ([(2004, 1, (body, 0)), (2004, 5, (owner, 0))], {}),            # his hold: stays
            12704890: ([(4, 8, (255, body, 20, 1)), (2004, 5, (0, 1))], {1: {0: 0}}),  # template, runs above
            12704891: ([(3, 0, (257, flag)), (4, 0, (1, body, 0)), (2003, 5, (owner, owner, 101, 1))], {}),
            12704892: ([(4, 0, (1, body, 0)), (2004, 1, (SHADOWS, 1))], {}),          # bosses only: stays
        }
        parts = {e: {"model": "c%04d" % (e % 10000)} for e in (body, ball, ball + 100, owner, SHADOWS)}
        out = helpers.phase_helpers({"m27_00": own}, {"m27_00": parts}, {body: "shadows"})[body]
        self.assertEqual(out["moved"], (12704890, 12704891))
        self.assertEqual(out["travel"], (owner, ball, ball + 100))
        self.assertEqual(out["runs"], {12704890: [(0, (ball, gen)), (1, (ball + 100, gen))]})
        self.assertEqual(out["as_started"], (flag,))
        self.assertEqual(helpers.phase_helpers({"m27_00": {defeat: own[defeat]}}, {"m27_00": parts}, {body: "shadows"}), {})

    @needs_game
    def test_the_game_alone_has_none_and_the_option_does_nothing(self):
        """User: without Nocturne the option does nothing: the same world as with it off."""
        import planner
        from gamedata import Census, GameFiles
        from neighbours import group_limits, load_groups
        try:
            census = Census(GameFiles(GAME))
            self.assertEqual(census.phases, {})
            self.assertFalse(any(s["role"] == "phase" for v in census.slots.values() for s in v))
            groups = load_groups()
            limits = group_limits(census, groups)
            on, off = (planner.plan(census.slots, census.candidates, census.sizes, groups, limits, 7,
                                    dict({"boss_swap": 1}, **extra), census.animations) for extra in ({"coiled_host": 1}, {}))
            self.assertEqual(on["options_hash"], off["options_hash"])
            self.assertEqual(on["changes"], off["changes"])
            self.assertEqual(on["options"]["coiled_host"], 0)
            self.assertIn("coiled_host", on["report"]["skipped_options"])
            self.assertNotIn("skipped_options", off["report"])
        finally:
            arenas.set_phases({})


@needs_nocturne
class OnNocturne(unittest.TestCase):
    def tearDown(self):
        arenas.set_phases({})

    def test_the_coiled_host_is_found_in_nocturne(self):
        census = Noc.census()
        self.assertEqual(census.phases, {HOST: "shadows"})
        slot = next(s for s in census.slots["m27_00"] if s["entity"] == HOST)
        self.assertEqual((slot["model"], slot["npc"], slot["kept"], slot["arena"], slot["role"]),
                         ("c5030", 503000, "boss", "shadows", "phase"))
        self.assertIs(arenas.arena_of(HOST), arenas.BY_KEY["shadows"])
        self.assertTrue(arenas.ends_on_death(HOST))
        self.assertTrue(census.animations["c5030"])  # forced animations he lacks elsewhere go inert (generate)
        self.assertNotIn("c5030", census.candidates)  # never an ordinary enemy

    def test_both_directions_in_the_plan(self):
        """On: his place draws a boss (never the Shadows', never his own model, never the One Reborn, whose bodies wake
        at the fight's start) and he fights in other arenas; the plan passes its own checks. Off: he stays and goes
        nowhere."""
        import planner
        from neighbours import group_limits, load_groups
        c = Noc.census()
        groups = load_groups()
        limits = group_limits(c, groups)
        away = 0
        for mix in (0, 1):
            for seed in range(1, 7):
                result = planner.plan(c.slots, c.candidates, c.sizes, groups, limits, seed,
                                      {"boss_swap": 1, "coiled_host": 1, "shadows_mix": mix}, c.animations)
                self.assertEqual(planner.check(result, c.slots, c.candidates, c.sizes, groups, limits, c.animations), [])
                key = result["boss_fighters"].get(HOST)
                self.assertIsNotNone(key, (mix, seed))
                f = result["fighters"][key]
                self.assertNotEqual(f["home"], "shadows")
                self.assertNotEqual(f["model"], "c5030")
                self.assertNotEqual(f["origin"], ONE_REBORN)
                elsewhere = [e for e, k in result["boss_fighters"].items() if k == "shadows_phase"]
                self.assertTrue(all(arenas.arena_of(e).key != "shadows" for e in elsewhere))
                away += bool(elsewhere)
                if elsewhere:
                    self.assertEqual(result["fighters"]["shadows_phase"]["origin"], HOST)
        self.assertGreaterEqual(away, 9)
        off = planner.plan(c.slots, c.candidates, c.sizes, groups, limits, 3, {"boss_swap": 1}, c.animations)
        self.assertNotIn(HOST, off["boss_fighters"])
        self.assertNotIn("shadows_phase", off["fighters"])

    def test_a_boss_in_his_place_ends_the_fight_with_the_rewards(self):
        """Every kind of boss in his place, with the Shadows one boss (mix off) or each their own (mix on), or left as
        they are: the fight can end (test_fight_ends' checker: the defeat waits on his place's death, or-ed with a
        body that travelled with that boss), the reward stays on his place, his bar shows the new boss's name, and
        his place is set up for co-op as vanilla sets up a parked second body (authority, doping)."""
        import boss_events
        Noc.census()
        shadows = arenas.BY_KEY["shadows"]
        failures = []
        for origin in (AMELIA, None, GASCOIGNE, LUDWIG, ORPHAN, LOGARIUS, NURSE):
            for mode, bodies in (("normal", {SHADOWS: CLERIC}), ("mix", {SHADOWS: CLERIC, 2700801: AMELIA, 2700802: CLERIC}),
                                 (None, {})):
                placed = {**bodies, HOST: origin}
                name = "%s %s" % (origin, mode)
                events, vanilla, travellers, report = build("m27_00", placed, {"shadows": mode} if mode else {})
                moved = {e for es in travellers.values() for e in es.values()}
                # the checker knows the table's bodies: his place joins them as one more body of the fight
                failures += ["%s: %s" % (name, p) for p in check_case(shadows, placed, events, vanilla,
                                                                       {**travellers, "phase": {HOST: HOST}})]
                if (2003, 12, (HOST,)) not in [(b, d, tuple(w)) for b, d, w in events.instructions(shadows.flags[0])]:
                    failures.append("%s: the reward left his place" % name)
                bars = lines(events, shadows.flags[0], 2003, 11)
                want = 509000 if origin is None else boss_events.origin_name(Noc.origin_events, origin)
                shown = [w for w in bars if w[0] == 1]
                if len(shown) != 1 or shown[0][3] != want or shown[0][1] not in {HOST} | moved:
                    failures.append("%s: bar %r, want %d on him" % (name, shown, want))
                for anchor_line, his in (((SHADOWS, 4095), (HOST, 4095)), ((SHADOWS, 7500, 1), (HOST, 7500, 1)),
                                         ((SHADOWS, 7501, 1), (HOST, 7501, 1))):
                    found = [e for e in events.event_ids() if any(tuple(w) == his for _, _, w in events.instructions(e))]
                    if not found:
                        failures.append("%s: %r missing" % (name, his))
                if mode is None:  # the Shadows stay as they are: their dead tests and bars too
                    dead = lines(events, shadows.flags[0], 4, 0)
                    for body in shadows.bodies:
                        if (1, body, 1) not in dead:
                            failures.append("%s: the defeat no longer waits on %d" % (name, body))
                    if len([w for w in lines(events, 12704802, 2003, 11) if w[0] == 1]) != 3:
                        failures.append("%s: a Shadow lost its bar" % name)
        self.assertEqual(failures, [])

    def test_he_fights_elsewhere_with_nothing_of_his_home_map(self):
        """In another arena he is that arena's boss: its fight can end, its bars show his name (NpcName 503000), and
        nothing of m27 is named there (his helpers come as copies under new ids: phase_helpers)."""
        Noc.census()
        failures = []
        for body in (AMELIA, LOGARIUS, 2200800):
            arena = arenas.arena_of(body)
            modes = {arena.key: "normal"} if arena.kind == arenas.GROUP else {}
            events, vanilla, travellers, report = build(arena.stem, {body: HOST}, modes)
            failures += ["%d: %s" % (body, p) for p in check_case(arena, {body: HOST}, events, vanilla, travellers)]
            names = [w[3] for e in events.event_ids() for w in lines(events, e, 2003, 11) if w[0] == 1 and w[1] == body]
            if not names or set(names) != {503000}:
                failures.append("%d: bars %r" % (body, names))
            bullets = {ws[3] for e in events.event_ids() for b, i, ws in events.instructions(e) if (b, i) == (2003, 5)}
            home = {w for e in events.event_ids() for _, _, ws in events.instructions(e) for w in ws
                    if (2700000 <= w < 2800000 or 12700000 <= w < 12800000) and w not in bullets}
            if home:
                failures.append("%d: names m27's %r" % (body, sorted(home)[:5]))
            for kind in ("parts", "phases"):
                for entity, _, added in report[kind]:
                    if entity == body and added:
                        failures.append("%d: copied %s %r" % (body, kind, added))
        self.assertEqual(failures, [])

    def test_his_helpers_travel_with_him(self):
        """User 23 Nov: his dip (3035) has no hit of its own; its message 20 releases the snake balls, so they and the
        bullet owners come along with their events (LIVE 23 Nov, beta 3: in the Blood-starved Beast's arena every dip did
        nothing). DATA Nocturne v6.0.0 m27: 12704813 (one run per ball), 12704830, 12704851 / 852; flag 12704808."""
        import helpers
        Noc.census()
        spec = arenas.PHASE_HELPERS.get(HOST)
        self.assertIsNotNone(spec)
        self.assertTrue({2700803, 2700804, 2700805, 2701966} <= set(spec["travel"]), spec["travel"])
        self.assertTrue(not set(spec["travel"]) & set(arenas.BY_ENTITY), spec["travel"])
        self.assertTrue({12704813, 12704830} <= set(spec["moved"]), spec["moved"])
        # his pools ("amygdala puddles", user 23 Nov): bullet 2770001 shot from the producers 2701968-970 (owner 2701966)
        # by 12704851, their look (effect 651207) renewed by 12704852 (snake-ball NOTES 22-23 Nov)
        self.assertTrue({2701968, 2701969, 2701970} <= set(spec["travel"]), spec["travel"])
        self.assertTrue({12704851, 12704852} <= set(spec["moved"]), spec["moved"])
        self.assertEqual(sorted(slot for slot, _ in spec["runs"][12704813]), [0, 1, 2])
        self.assertIn(12704808, spec["as_started"])
        self.assertIs(helpers.FIGHTS[HOST].home, False)  # his own arena under another boss is not dismissed
        body = 2300800  # the Blood-starved Beast's (LIVE 23 Nov)
        arena = arenas.arena_of(body)
        events, vanilla, travellers, _ = build(arena.stem, {body: HOST}, {})
        self.assertEqual(check_case(arena, {body: HOST}, events, vanilla, travellers), [])
        new = travellers[body]
        self.assertEqual(set(new), set(spec["travel"]))
        balls = sorted(new[e] for e in (2700803, 2700804, 2700805))
        calls = [tuple(w) for b, i, w in events.instructions(0) if (b, i) == (2000, 0) and len(w) > 2 and w[2] in balls]
        template = {w[1] for w in calls}
        self.assertEqual(len(template), 2, calls)  # the release and the end, each once per ball
        release = next(t for t in template if any((b, i) == (4, 8) and w[1] == body and w[2] == 20
                                                  for b, i, w in events.instructions(t)))
        self.assertEqual(sorted(w[2] for w in calls if w[1] == release), balls)
        self.assertEqual(sorted(w[0] for w in calls if w[1] == release), [0, 1, 2])
        for t in template:  # the generators stay home: the copies disable a ball where m27 killed it (its generator
            # brought it back on the next dip), and no line names a generator
            self.assertEqual([w for b, i, w in events.instructions(t) if (b, i) in ((2004, 4), (2003, 3))], [], t)
            self.assertEqual(events.restart(t), 1)
        self.assertTrue(all(w[3] == 0 for w in calls))  # the generator argument
        started = arenas.arena_of(body).flags[1]
        pools = [e for e in events.event_ids() if any((b, i) == (2003, 5) and w[3] == 2770001 and w[0] == new[2701966]
                                                      for b, i, w in events.instructions(e))]
        self.assertEqual(len(pools), 1)
        shot = sorted(w[1] for b, i, w in events.instructions(pools[0]) if (b, i) == (2003, 5))
        self.assertEqual(shot, sorted(new[e] for e in (2701968, 2701969, 2701970)))
        looks = [e for e in events.event_ids() if any((b, i) == (2006, 3) and w[3] == 651207
                                                      for b, i, w in events.instructions(e))]
        self.assertEqual(len(looks), 1)
        for t in sorted(template):
            first = next(w for b, i, w in events.instructions(t) if b < 1000)
            self.assertEqual(tuple(first), (256, started), t)

    def test_off_no_summons_also_silences_nocturnes_summon_event(self):
        """Nocturne runs its own copy of the summon event (12704813, which also waits on his message 20) in place of
        12704807: option shadows_summons off must silence that one."""
        import generate
        events = Emevd(Noc.event_data("m27_00"))
        before = {e: events.instructions(e) for e in events.event_ids()}
        generate._no_summons(events)
        after = {e: events.instructions(e) for e in events.event_ids()}
        self.assertEqual({e for e in before if before[e] != after[e]}, {12704813})
        changed = [a[2] for b, a in zip(before[12704813], after[12704813]) if a != b]
        self.assertEqual(changed, [(255, e, generate.NEVER_SENT, 1) for e in (2700800, 2700801, 2700802, HOST)])

    def test_own_fight_gets_coop_setup(self):
        """User, 8 Nov: whatever the options, the Coiled Host in his own fight gets the main Shadow's 2004[28] and co-op
        doping in the set-up event (Nocturne gives him neither); run twice (or after a drawn boss's edit) it adds nothing."""
        import boss_events, helpers
        Noc.census()
        events = Emevd(Noc.event_data("m27_00"))
        self.assertEqual(helpers.doped(events, HOST), [])
        self.assertFalse([e for e in events.event_ids() if (HOST, 4095) in lines(events, e, 2004, 28)])
        report = boss_events.phase_coop(events, "m27_00")
        self.assertEqual(set(report), {"shadows"})
        main = [e for e in events.event_ids() if (SHADOWS, 4095) in lines(events, e, 2004, 28)]
        self.assertTrue(main)
        for e in main:
            self.assertIn((HOST, 4095), lines(events, e, 2004, 28))
        self.assertEqual({e for e, _ in helpers.doped(events, HOST)}, {e for e, _ in helpers.doped(events, SHADOWS)})
        self.assertEqual(len(helpers.doped(events, HOST)), len(helpers.doped(events, SHADOWS)))
        again = {e: events.instructions(e) for e in events.event_ids()}
        self.assertEqual(boss_events.phase_coop(events, "m27_00"), {})
        self.assertEqual({e: events.instructions(e) for e in events.event_ids()}, again)
        self.assertEqual(boss_events.phase_coop(Emevd(Noc.event_data("m24_01")), "m24_01"), {})

    def test_game_alone_phase_coop_changes_nothing(self):
        import boss_events
        from gamedata import GameFiles
        events_data = dcx_read(GameFiles(GAME, []).read("event/m27_00_00_00.emevd.dcx"))[0]
        arenas.set_phases({})
        events = Emevd(events_data)
        self.assertEqual(boss_events.phase_coop(events, "m27_00"), {})
        vanilla = Emevd(events_data)
        self.assertEqual({e: events.instructions(e) for e in events.event_ids()},
                         {e: vanilla.instructions(e) for e in vanilla.event_ids()})


if __name__ == "__main__":
    unittest.main()
