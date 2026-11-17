"""Co-op: copies of boss phase events keep the party's host-only rule where the map has a listed id to give them
(hostonly.py). The mirror of the engine's list is checked against the engine header in this repository; the planner's
worlds over several seeds are built (helpers.apply_stem, then generate.patched_events) and every copied event is
checked against the list. Run: python -I -m unittest discover -s tools/randomizer/tests. Reads (never writes) the game
at BB_GAME."""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import arenas
import hostonly
from containers import dcx_read
from emevd import LABEL_BANK, Emevd

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")
HEADER = os.path.join(HERE, "..", "..", "..", "engine", "gpu", "shim", "party", "party_bossfog_rules.h")
SEEDS = range(1, 5)
SETS = {"swap": {"boss_swap": 1},
        "mixall": {"boss_swap": 1, "witch_mix": 1, "shadows_mix": 1, "emissary_mix": 1, "failures_mix": 1, "bell_maidens": 2, "difficult_enemies": 1, "second_stage": 1},
}


def stem_of(event):
    """The map an event id of a main map belongs to (12414707 -> m24_01)."""
    return "m%02d_%02d" % (event // 100000 % 100, event // 10000 % 10)


class Game:
    _files, _events, _variants = None, {}, {}

    @classmethod
    def files(cls):
        if cls._files is None:
            from gamedata import GameFiles
            cls._files = GameFiles(GAME)
        return cls._files

    @classmethod
    def event_data(cls, stem):
        if stem not in cls._events:
            cls._events[stem] = dcx_read(cls.files().read("event/%s_00_00.emevd.dcx" % stem))[0]
        return cls._events[stem]

    @classmethod
    def variants(cls, stem):
        if stem not in cls._variants:
            from gamedata import map_variants
            cls._variants[stem] = {v: dcx_read(cls.files().read("map/mapstudio/%s.msb.dcx" % v))[0]
                                   for v in map_variants(cls.files(), stem)}
        return cls._variants[stem]


def derived_host_only(cache):
    """The [hostOnlyEvents] section of a party game-data cache (party-tables-cli GAME_DIR OUT_FILE): the engine derives
    the list from the player's own game (events named ヒートアップ), since visy 0.7.9 there is no fixed list in the header."""
    lines = open(cache, encoding="utf-8", errors="replace").read().split("\n")
    at = next(i for i, line in enumerate(lines) if line.startswith("[hostOnlyEvents]"))
    count = int(lines[at].split()[1])
    return frozenset(int(x) for x in lines[at + 1:at + 1 + count])


class Mirror(unittest.TestCase):
    @unittest.skipUnless(os.path.isfile(HEADER), "no engine header")
    def test_the_engine_reads_the_list_from_the_game(self):
        text = open(HEADER, encoding="utf-8").read()
        found = re.search(r"bool HostOnlyBossEvent\(std::uint32_t id\) \{(.*?)\n\}", text, re.S)
        self.assertIsNotNone(found, "HostOnlyBossEvent not found")
        self.assertIn("gamedata::Get().host_only_events", found.group(1))  # derived, no fixed list in the engine
        self.assertNotRegex(found.group(1), r"\d{8}u")

    @unittest.skipUnless(os.path.isfile(os.environ.get("BB_PARTY_TABLES", "")),
                         "BB_PARTY_TABLES is not a cache (party-tables-cli GAME_DIR OUT_FILE)")
    def test_the_list_mirrors_what_the_engine_derives_from_the_game(self):
        self.assertEqual(hostonly.HOST_ONLY, derived_host_only(os.environ["BB_PARTY_TABLES"]))

    @needs_game
    def test_owners_are_the_arenas_whose_bodies_the_vanilla_events_name(self):
        main = sorted(e for e in hostonly.HOST_ONLY if not 12900000 <= e < 13000000)  # m29: chalice maps, never edited
        self.assertEqual(sorted(hostonly.OWNER), main)
        for event in main:
            events = Emevd(Game.event_data(stem_of(event)))
            keys = {arenas.arena_of(w).key for _, _, words in events.instructions(event) for w in words
                    if arenas.arena_of(w) is not None}
            self.assertEqual(keys, {hostonly.OWNER[event]}, event)


def read_party_list(text):
    """party_host_only.txt as the engine's scripts/mods.py reads a mod's list (visy PR #33: utf-8-sig, lines, '#'
    comments, spaces/tabs/CR stripped, decimal 1..4294967295, any other line: the whole list is refused)."""
    ids = []
    for line in text.lstrip("﻿").splitlines():
        t = line.split("#", 1)[0].strip(" \t\r")
        if t and not (t.isascii() and t.isdigit() and 0 < int(t) <= 0xFFFFFFFF):
            raise ValueError(line)
        if t:
            ids.append(int(t))
    return ids


class PartyList(unittest.TestCase):
    """The world's party_host_only.txt: the uncovered copies of maps that list ids, for the host's engine."""

    def test_listed_maps_give_every_copy_and_other_maps_the_copies_of_listed_events(self):
        reports = {"m24_01": {"listed": 1, "free": 0, "covered": [], "uncovered": [[12414895, 12424980, 2410811, 0],
                                                                                  [12414894, 13404835, 2410811, 1]]},
                   # m35 lists no id: the copy of the Blood-starved Beast's listed 12304807 is host-only at the boss's
                   # home, so it is listed; the copy of Ludwig's unlisted 13404835 is host-only neither at home nor here
                   "m35_00": {"listed": 0, "free": 0, "covered": [], "uncovered": [[13504899, 12304807, 3500800, 0],
                                                                                  [13504900, 13404835, 3500800, 1],
                                                                                  [13504901, 12414809, 3500800, 2]]},
                   "m24_02": {"listed": 1, "free": 1, "covered": [[12424890, 12424980, 12304807, 2420800, 0]],
                              "uncovered": [[12414894, 1, 2, 2]]},  # the same id twice: once in the list
                   "m21_00": {}, "m22_00": None}
        self.assertEqual(hostonly.party_ids(reports), [12414894, 12414895, 13504899])
        self.assertEqual(hostonly.party_ids({"m35_00": reports["m35_00"], "m21_00": {}}), [13504899])
        m26 = {"listed": 0, "uncovered": [[12604890, 12414808, 2600800, 1], [12604891, 12414809, 2600800, 2]]}
        self.assertEqual(hostonly.party_ids({"m26_00": m26}), [])

    def test_the_text_is_what_the_engine_reads_and_nothing_for_no_ids(self):
        self.assertIsNone(hostonly.party_text([]))
        text = hostonly.party_text([12414894, 12414895])
        self.assertTrue(text.startswith("#") and text.endswith("\n") and "\r" not in text and text.isascii())
        self.assertEqual(read_party_list(text), [12414894, 12414895])
        self.assertEqual(hostonly.PARTY_FILE, "party_host_only.txt")

    def test_bounded_by_the_engines_limit(self):
        many = {"m24_01": {"listed": 1, "uncovered": [[20000000 + i, 0, 0, 0] for i in range(hostonly.PARTY_MAX)]}}
        self.assertEqual(len(hostonly.party_ids(many)), hostonly.PARTY_MAX)
        many["m24_01"]["uncovered"].append([30000000, 0, 0, 0])
        with self.assertRaises(RuntimeError):
            hostonly.party_ids(many)

    @unittest.skipUnless(os.path.isfile(os.path.join(HERE, "..", "..", "..", "engine", "start", "host_only.h")),
                         "no bb-start header")
    def test_the_limit_is_bb_starts(self):
        text = open(os.path.join(HERE, "..", "..", "..", "engine", "start", "host_only.h"), encoding="utf-8").read()
        self.assertEqual(int(re.search(r"kMax\s*=\s*(\d+)", text).group(1)), hostonly.PARTY_MAX)
        self.assertIn('kFile = "%s"' % hostonly.PARTY_FILE, text)


@needs_game
class SwapIds(unittest.TestCase):
    def test_a_swap_is_a_pure_relabelling(self):
        data = Game.event_data("m24_01")
        events = Emevd(data)
        a, b = 12414707, 12414808
        body_a, body_b = events.instructions(a), events.instructions(b)
        events.swap_ids(a, b)
        again = Emevd(events.bytes())
        self.assertEqual(again.event_ids(), sorted(again.event_ids()))
        self.assertEqual(set(again.event_ids()), set(Emevd(data).event_ids()))
        swap = {a: b, b: a}
        self.assertEqual(again.instructions(b), [(k, i, tuple(swap.get(w, w) for w in ws)) for k, i, ws in body_a])
        self.assertEqual(again.instructions(a), [(k, i, tuple(swap.get(w, w) for w in ws)) for k, i, ws in body_b])
        vanilla = Emevd(data)
        for event in vanilla.event_ids():
            if event not in swap:
                self.assertEqual(again.instructions(event), [(k, i, tuple(swap.get(w, w) for w in ws))
                                                             for k, i, ws in vanilla.instructions(event)], event)
        again.swap_ids(a, b)
        self.assertEqual(again.bytes(), bytes(data))

    def test_the_cleric_beast_arena_gives_its_listed_id_to_the_blood_starved_beasts_first_phase(self):
        """The co-op breaker's world (5 Nov): the Blood-starved Beast on the Cleric Beast's body, its phases along. The
        copy of 12304807 runs as 12414707, the id the party keeps host-only; the copy of 12304808 (no listed id is left
        in m24_01) keeps its new id and waits on 12414707."""
        import boss_events
        events = Emevd(Game.event_data("m24_01"))
        (_, old, added), = boss_events.move_phases(events, {2410800: 2300800},
                                                   lambda e: Emevd(Game.event_data("m23_00")))
        self.assertEqual(old, [12414707])
        first, second = added
        report = hostonly.cover(events)
        self.assertEqual(report["covered"], [[first, 12414707, 12304807, 2410800, 0]])
        self.assertEqual(report["uncovered"], [[second, 12304808, 2410800, 0]])
        self.assertIn((4, 2), [(k, i) for k, i, _ in events.instructions(12414707)])
        self.assertEqual(events.instructions(first), [(LABEL_BANK, 9, ())])  # the silenced vanilla event, under the new id
        self.assertIn(12414707, [w for _, _, ws in events.instructions(second) for w in ws])
        self.assertIn((2000, 0, (0, 12414707, 0)), events.instructions(0))


@needs_game
class HelperPhases(unittest.TestCase):
    def test_a_travelling_orphans_handover_survives_phases_and_takes_the_listed_id(self):
        """The Orphan on Amygdala's body (phases always follow): its handover copy (of 13604820, listed) tests the new
        body's HP like a phase of that body, but it is the Orphan's own and must not be silenced with Amygdala's
        phases (it was, before 5 Nov: phase two never came); it runs as 13304807, Amygdala's emptied listed id."""
        import helpers
        from generate import patched_events
        orphan, amygdala = 3600800, 3300800
        fighter = {"key": "orphan", "model": "c4540", "think": 1, "npc": 1, "origin": orphan, "home": None,
                   "extras": (), "extra_models": ()}
        homes = {orphan: "m36_00", amygdala: "m33_00"}
        origin_events = lambda e: Emevd(Game.event_data(homes[e]))
        origin_msb = lambda e: Game.variants(homes[e])[sorted(Game.variants(homes[e]))[0]]
        events = Emevd(Game.event_data("m33_00"))
        _, report = helpers.apply_stem({amygdala: fighter}, events, Game.variants("m33_00"), origin_events, origin_msb)
        handover = dict(report["travelled"][0]["events"])[13604820]
        patched, _, done = patched_events(events.bytes(), "m33_00", {amygdala: "orphan"}, {"orphan": fighter}, {},
                                          {"c4540": type("Every", (), {"__contains__": lambda s, a: True})()},
                                          origin_events, events.sources)
        built = Emevd(patched)
        self.assertNotIn(13304807, [e for _, old, _ in done["phases"] for e in old])  # the report names final ids
        # every id the report lists as silenced or added names that event in the built file (the spoiler is final)
        for _, old, added in done["phases"]:
            self.assertTrue(all(hostonly.emptied(built, e) for e in old), old)
            self.assertFalse(any(hostonly.emptied(built, e) for e in added), added)
        self.assertEqual(done["host_only"]["covered"][0][:3], [handover, 13304807, 13604820])
        body = built.instructions(13304807)
        self.assertFalse(hostonly.emptied(built, 13304807))
        self.assertIn((4, 2), [(k, i) for k, i, _ in body])
        self.assertIn(amygdala, [w for _, _, ws in body for w in ws])


@needs_game
class Worlds(unittest.TestCase):
    """The planner's worlds: every copy that needs a listed id has one, unless its map has none left."""

    @classmethod
    def setUpClass(cls):
        import chalice
        import difficult
        import generate
        from gamedata import Census
        from neighbours import group_limits, load_groups
        cls.census = Census(Game.files())
        cls.groups = load_groups()
        cls.limits = group_limits(cls.census, cls.groups)
        cls.home = {s["entity"]: stem for stem in cls.census.slots for s in cls.census.slots[stem]}
        cls.difficult = difficult.candidates(cls.census)
        cls.generate = generate

    def origin_events(self, entity):
        return Emevd(Game.event_data(self.home[entity]))

    def origin_msb(self, entity):
        variants = Game.variants(self.home[entity])
        return variants[sorted(variants)[0]]

    def worlds(self):
        import helpers
        import planner
        import seedcode
        c = self.census
        for name, extra in sorted(SETS.items()):
            for seed in SEEDS:
                options = seedcode.normalize({**seedcode.defaults(), **extra})
                maidens = seedcode.normalize(options)["bell_maidens"]
                slots = {stem: [planner.escort_slot(s, maidens) if s["entity"] in arenas.ESCORTS else s for s in v]
                         for stem, v in c.slots.items()}
                result = planner.plan(slots, c.candidates, c.sizes, self.groups, self.limits, seed, options,
                                      c.animations, self.difficult)
                fighters = result["fighters"]
                for stem in sorted({s for s, _ in result["bosses"].values()}):
                    placed = {e: result["boss_fighters"][e] for e, (s, _) in result["bosses"].items() if s == stem}
                    modes = {k: m for k, m in result["arena_modes"].items() if arenas.BY_KEY[k].stem == stem}
                    events = Emevd(Game.event_data(stem))
                    helpers.apply_stem({e: fighters[k] for e, k in placed.items()}, events, Game.variants(stem),
                                       self.origin_events, self.origin_msb)
                    patched, _, report = self.generate.patched_events(
                        events.bytes(), stem, placed, fighters, modes, c.animations, self.origin_events,
                        events.sources)
                    built = Emevd(patched if patched is not None else events.bytes())
                    yield (name, seed, stem), built, report.get("host_only") or {}

    def test_every_copy_takes_a_listed_id_while_the_map_has_one(self):
        covered, uncovered, by_arena = 0, 0, {}
        for where, built, report in self.worlds():
            stem = where[2]
            vanilla = Emevd(Game.event_data(stem))
            listed = sorted(e for e in hostonly.HOST_ONLY if e in vanilla.index)
            # never another map's listed id, and every listed id of this map is still an event here
            self.assertEqual(sorted(e for e in hostonly.HOST_ONLY if e in built.index), listed, where)
            holds = {c[1]: c for c in report.get("covered", ())}
            for event in listed:
                if event in holds:
                    self.assertTrue(hostonly.emptied(vanilla, event) or vanilla.instructions(event)
                                    != built.instructions(event), where)
                    self.assertFalse(hostonly.emptied(built, event), (where, event))
                    # the copy is started where the generator starts its copies (event 0), slot 0
                    self.assertIn((2000, 0, (0, event, 0)), built.instructions(0), (where, event))
                    self.assertFalse(built.params(event), (where, event))
                # a listed event the generator left running keeps its body (no copy took it)
                elif not hostonly.emptied(built, event):
                    self.assertNotIn(event, [c[1] for c in report.get("covered", ())], where)
            free_left = [e for e in listed if hostonly.emptied(built, e)]
            if report.get("uncovered"):
                # the rule: a copy goes without only when its map has no emptied listed id left
                self.assertEqual(free_left, [], (where, report))
            # the host's list: every uncovered copy where the map lists ids; where it lists none, the copies of listed
            # events only (host-only at the boss's home); each an event here
            party = hostonly.party_ids({stem: report})
            self.assertEqual(party, sorted({c[0] for c in report.get("uncovered", ()) if listed or c[3] == 0}), where)
            # with the list in place, every copy of a listed event is host-only on the host, in every map
            for c in report.get("uncovered", ()):
                if c[1] in hostonly.HOST_ONLY:
                    self.assertIn(c[0], party, (where, c))
            for event in party:
                self.assertIn(event, built.index, (where, event))
                self.assertNotIn(event, hostonly.HOST_ONLY, (where, event))
            for c in report.get("covered", ()):
                by_arena.setdefault(arenas.arena_of(c[3]).key, [0, 0])[0] += 1
            for c in report.get("uncovered", ()):
                by_arena.setdefault(arenas.arena_of(c[2]).key, [0, 0])[1] += 1
            covered += len(report.get("covered", ()))
            uncovered += len(report.get("uncovered", ()))
        self.assertGreater(covered, 0)
        print("\nhost-only copies over %d seeds x %d sets: covered %d, uncovered %d; per arena %s"
              % (len(SEEDS), len(SETS), covered, uncovered, sorted(by_arena.items())), file=sys.stderr)


if __name__ == "__main__":
    unittest.main()
