"""Bodies that travel with their boss, and arenas whose helpers leave (helpers.py, msb.add_enemy, emevd.splice).
Run: python -I -m unittest discover -s tools/randomizer/tests. Reads (never writes) the game at BB_GAME."""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from containers import dcx_read
from emevd import LABEL_BANK, Emevd
from msb import EVENTS, MODELS, PARTS, Msb, _i32, _i64, _utf16z, sections

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")
AMELIA, LOGARIUS, NURSE, ORPHAN, ONE_REBORN = 2400800, 2500800, 2600800, 3600800, 2800800
EBRIETAS = 2420800


def stem_of(entity):
    return "m%02d_%02d" % (entity // 100000, entity // 10000 % 10)


class Game:
    _files = None

    @classmethod
    def files(cls):
        if cls._files is None:
            from gamedata import GameFiles
            cls._files = GameFiles(GAME)
        return cls._files

    @classmethod
    def events(cls, stem):
        return Emevd(dcx_read(cls.files().read("event/%s_00_00.emevd.dcx" % stem))[0])

    @classmethod
    def variants(cls, stem):
        from gamedata import map_variants
        return {v: dcx_read(cls.files().read("map/mapstudio/%s.msb.dcx" % v))[0] for v in map_variants(cls.files(), stem)}

    @classmethod
    def msb(cls, entity):
        variants = cls.variants(stem_of(entity))
        return variants[sorted(variants)[0]]


def part_refs(msb, skip=None):
    """Names of the parts each part-index word of the file points at, in file order; words inside part `skip`
    (a new part) left out."""
    names = msb.part_names()
    table = sections(msb.data)[PARTS][2]
    lo = hi = -1
    if skip is not None:
        k = names.index(skip)
        lo, hi = table[k], table[k + 1]
    return [names[_i32(msb.data, w)] if 0 <= _i32(msb.data, w) < len(names) else None
            for w in msb._part_index_words() if not lo <= w < hi]


def records(data, section):
    table = sections(data)[section][2]
    return [bytes(data[a:b]) for a, b in zip(table[:-1], table[1:])] if section != PARTS else None


def new_words(edited, vanilla):
    old = {w for e in vanilla.event_ids() for _, _, ws in vanilla.instructions(e) for w in ws}
    return {w for e in edited.event_ids() for _, _, ws in edited.instructions(e) for w in ws} - old


def conditions_on(events, event):
    """Map entities that the event's character conditions test (the player, 10000, left out)."""
    return [w[1] for b, i, w in events.instructions(event) if b == 4 and len(w) > 1 and 1000000 <= w[1] < 10000000]


class Splice(unittest.TestCase):
    @needs_game
    def test_a_spliced_instruction_keeps_the_skips_around_it(self):
        events = Game.events("m25_00")
        body = events.instructions(12501800)
        events.splice(12501800, 13, [(4, 0, [0xff, 2500800, 1]), (4, 0, [0xff, 2500801, 1]), (0, 0, [0xff0100])])
        again = Emevd(events.bytes()).instructions(12501800)
        self.assertEqual(len(again), len(body) + 2)
        self.assertEqual(again[:13], body[:13])
        self.assertEqual(again[16:], body[14:])
        # a skip before the spliced line that jumps over it grows by two
        ev = Game.events("m25_00")
        ev.add_event(12504890, 0, [(1003, 1, [2, 12501800]), (1014, 0, []), (4, 0, [0, 2500800, 1]), (1014, 1, [])])
        ev.splice(12504890, 2, [(1014, 2, []), (1014, 3, []), (1014, 4, [])])
        self.assertEqual(ev.instructions(12504890)[0], (1003, 1, (4, 12501800)))


class AddEnemy(unittest.TestCase):
    @needs_game
    def test_a_new_enemy_part_never_takes_the_templates_talk_script(self):
        # DATA: Father Gascoigne's part (2410810) names talk script 241330; a body copied from it gets none, or its own
        gascoigne = 2410810
        msb = Msb(Game.msb(gascoigne))
        boss = next(e for e in msb.enemies() if e["entity"] == gascoigne)
        self.assertEqual(boss["talk"], 241330)
        msb.add_enemy(boss["part"], "c5072_0000", 2410865, "c5072", 507200, 507200, (1.0, 2.0, 3.0))
        msb.add_enemy(boss["part"], "c5072_0001", 2410866, "c5072", 507200, 507200, (1.0, 2.0, 3.0), talk=123)
        talks = {e["part"]: e["talk"] for e in Msb(msb.bytes()).enemies()}
        self.assertEqual((talks["c5072_0000"], talks["c5072_0001"], talks["c2710_0000"]), (0, 123, 241330))

    @needs_game
    def test_a_new_enemy_part_reads_back_and_every_other_record_is_unchanged(self):
        data = Game.msb(AMELIA)
        before = Msb(data)
        refs_before = part_refs(before)
        enemies_before = [{k: v for k, v in e.items() if k != "at"} for e in before.enemies()]
        boss = next(e for e in before.enemies() if e["entity"] == AMELIA)
        msb = Msb(data)
        msb.add_enemy(boss["part"], "c2321_0000", 2400860, "c2321", 232100, 232100, (1.0, 2.0, 3.0), (0.0, 45.0, 0.0))
        again = Msb(msb.bytes())
        new = [e for e in again.enemies() if e["part"] == "c2321_0000"]
        self.assertEqual([(e["entity"], e["model"], e["think"], e["npc"], e["anim"]) for e in new],
                         [(2400860, "c2321", 232100, 232100, -1)])
        self.assertEqual(struct.unpack_from("<6f", again.data, new[0]["at"] + 0x28), (1.0, 2.0, 3.0, 0.0, 45.0, 0.0))
        # the other enemies keep everything, in order; the new one is the last enemy, parts stay grouped by type
        self.assertEqual([{k: v for k, v in e.items() if k != "at"} for e in again.enemies()][:-1], enemies_before)
        types = [_i32(again.data, a + 0x14) for a in sections(again.data)[PARTS][2][:-1]]
        self.assertEqual(types, sorted(types, key=lambda t: [0, 1, 2, 4, 5, 8, 9, 10, 11].index(t) if t >= 0 else 99))
        # every part index still names the same part (collisions, treasure, generators, groups, object actions)
        self.assertEqual(part_refs(again, "c2321_0000"), refs_before)
        # models: one new record, the others unchanged but for c2321's instance count
        self.assertIn("c2321", again.model_names())
        self.assertEqual([n for n in again.model_names() if n != "c2321"], before.model_names())
        # events and regions: byte for byte, but for the part indices that moved
        for section in (EVENTS, "POINT_PARAM_ST"):
            a, b = records(data, section), records(again.data, section)
            self.assertEqual(len(a), len(b))
            changed = sum(x != y for x, y in zip(a, b))
            if section != EVENTS:
                self.assertEqual(changed, 0)
        # the boss's part is untouched, and the copy uses its collision
        old_boss = next(e for e in again.enemies() if e["entity"] == AMELIA)
        kind = _i64(again.data, old_boss["at"] + 0xb8)
        kind_new = _i64(again.data, new[0]["at"] + 0xb8)
        self.assertEqual(_i32(again.data, old_boss["at"] + kind + 0x1c), _i32(again.data, new[0]["at"] + kind_new + 0x1c))


class Fights(unittest.TestCase):
    def _travel(self, origin, target):
        import helpers
        stem = stem_of(target)
        events, vanilla = Game.events(stem), Game.events(stem)
        variants = Game.variants(stem)
        fighter = {"key": "x", "origin": origin, "model": None}
        out, report = helpers.apply_stem({target: fighter}, events, variants,
                                         lambda e: Game.events(stem_of(e)), lambda e: Game.msb(e))
        again = Emevd(events.bytes())
        return again, vanilla, {v: Msb(b) for v, b in out.items()}, report

    def assertNoDangling(self, edited, vanilla, msbs, origin):
        """Every entity-sized word the edit brought into the events exists in each variant of the map, and no flag of
        the boss's home map came along."""
        import helpers
        home_flags = helpers.FIGHTS[origin].defeat // 10000
        for word in new_words(edited, vanilla):
            self.assertFalse(10000000 <= word < 100000000 and word // 10000 == home_flags, word)
            if 1000000 <= word < 10000000:
                for name, msb in msbs.items():
                    self.assertIn(struct.pack("<i", word), bytes(msb.data), "%d not in %s" % (word, name))

    @needs_game
    def test_logarius_takes_his_sword_and_bullet_owner_to_amelias_arena(self):
        events, vanilla, msbs, report = self._travel(LOGARIUS, AMELIA)
        bodies = report["bodies"][AMELIA]
        self.assertEqual([(b["home"], b["model"], b["think"], b["npc"]) for b in bodies],
                         [(2500801, "c2321", 232100, 232100), (2500802, "c9010", 232000, 232000)])
        sword, owner = (b["entity"] for b in bodies)
        for msb in msbs.values():
            got = {e["entity"]: (e["model"], e["think"], e["npc"]) for e in msb.enemies()}
            self.assertEqual(got[sword], ("c2321", 232100, 232100))
            self.assertEqual(got[owner], ("c9010", 232000, 232000))
        travelled, = report["travelled"]
        moved = dict(travelled["events"])
        self.assertEqual(sorted(moved), [12504806, 12504807, 12504808])
        summon = events.instructions(moved[12504806])
        self.assertIn((2004, 42, (sword, 2, AMELIA, 100, AMELIA)), summon)  # warped to the boss's dummy poly 100
        self.assertIn((2004, 5, (sword, 1)), summon)
        self.assertIn((1003, 2, (256, 12401800)), summon)  # ends on Amelia's arena's defeat flag
        self.assertIn((2003, 5, (owner, AMELIA, 6, 223200590, 0, 0, 0)), events.instructions(moved[12504807]))
        init = events.instructions(travelled["init"])
        self.assertIn((2004, 7, (owner,)), init)
        self.assertIn((2004, 5, (sword, 0)), init)
        runs = [w[1] for b, i, w in events.instructions(0) if (b, i) == (2000, 0)]
        for event in list(moved.values()) + [travelled["init"]]:
            self.assertIn(event, runs)
        self.assertEqual(report["scripts"], [("c2321", 232100, 232100), ("c9010", 232000, 232000)])
        self.assertEqual(conditions_on(events, 12401800), conditions_on(vanilla, 12401800))  # her end: the boss's death
        self.assertNoDangling(events, vanilla, msbs, LOGARIUS)

    @needs_game
    def test_logarius_arena_with_another_boss_ends_on_that_boss_alone(self):
        import helpers
        events = Game.events("m25_00")
        report = helpers.dismiss(events, "logarius")
        self.assertEqual(report["parked"], [12504804, 12504806, 12504807, 12504808])
        again = Emevd(events.bytes())
        for event in report["parked"]:
            self.assertEqual(again.instructions(event), [(3, 0, (256, 12501800))])
        self.assertEqual(set(conditions_on(again, 12501800)), {LOGARIUS})
        # nothing left enables a helper: the AI hold disables both, no other event turns them on
        on = [(e, w) for e in again.event_ids() for b, i, w in again.instructions(e)
              if (b, i) == (2004, 5) and w[0] in (2500801, 2500802) and w[1] == 1]
        self.assertEqual(on, [])

    @needs_game
    def test_found_helpers_ebrietas_takes_her_bullet_owner_to_amelias_arena(self):
        """User 23 Nov: "what about Martyr Logarius' helpers? that's another example". The found rule (helpers.found_helpers)
        over the game: Logarius's are listed (above), and of the unlisted bosses only Ebrietas has one: DATA m24_02 12424990
        shoots bullet 225100310 from the invisible owner 2420801 (c9010, a bullet owner by event 0) while she has SpEffect
        5650. Elsewhere it comes along; at home under another boss it is parked."""
        import arenas
        import helpers
        from gamedata import Census
        census = Census(Game.files())
        arenas.set_phases(census.phases, census.found_helpers)
        self.assertEqual(sorted(census.found_helpers), [EBRIETAS])
        spec = census.found_helpers[EBRIETAS]
        self.assertEqual((spec["travel"], spec["moved"], spec["init"]), ((2420801,), (12424990,), ((2004, 7, (2420801,)),)))
        self.assertEqual(helpers.extras_of(census.slots, EBRIETAS), ((2420801,), ("c9010",)))
        events, vanilla, msbs, report = self._travel(EBRIETAS, AMELIA)
        (owner,) = (b["entity"] for b in report["bodies"][AMELIA])
        travelled, = report["travelled"]
        moved = dict(travelled["events"])
        self.assertEqual(sorted(moved), [12424990])
        shot = events.instructions(moved[12424990])
        self.assertIn((2003, 5, (owner, AMELIA, 6, 225100310, 0, 0, 0)), shot)
        self.assertEqual(shot[0], (3, 0, (256, 12404800)))  # waits for Amelia's arena's fight
        self.assertIn((2004, 7, (owner,)), events.instructions(travelled["init"]))
        self.assertNoDangling(events, vanilla, msbs, EBRIETAS)
        home = Game.events("m24_02")
        self.assertEqual(helpers.dismiss(home, "ebrietas")["parked"], [12424990])

    @needs_game
    def test_wet_nurse_arena_with_another_boss_has_its_bar_and_end_on_the_body(self):
        import helpers
        events, vanilla = Game.events("m26_00"), Game.events("m26_00")
        report = helpers.dismiss(events, "wet_nurse")
        again = Emevd(events.bytes())
        self.assertEqual(set(conditions_on(again, 12601800)), {NURSE})
        bars = [w for e in again.event_ids() for b, i, w in again.instructions(e) if (b, i) == (2003, 11)]
        self.assertIn((1, NURSE, 0, 551000), bars)
        for e in again.event_ids():
            if e in report["parked"]:
                continue
            for b, i, w in again.instructions(e):
                self.assertFalse(b == 4 and len(w) > 1 and w[1] in (2600801, 2600802), (e, b, i, w))
                self.assertNotIn((b, i, tuple(w)), [(2004, 12, (NURSE, 1)), (2004, 33, (NURSE, 2600802))])
        self.assertIn((2004, 5, (2600802, 0)), again.instructions(report["event"]))
        self.assertIn(report["event"], [w[1] for b, i, w in again.instructions(0) if (b, i) == (2000, 0)])
        # the teleport template's six runs stay, the template itself only waits for the end
        self.assertEqual(again.instructions(12604830), [(3, 0, (256, 12601800))])
        self.assertEqual(again.params(12604830), {})

    @needs_game
    def test_wet_nurse_takes_her_clone_to_amelias_arena(self):
        events, vanilla, msbs, report = self._travel(NURSE, AMELIA)
        (clone,) = report["bodies"][AMELIA]
        self.assertEqual((clone["home"], clone["model"], clone["npc"]), (2600801, "c5510", 551000))
        travelled, = report["travelled"]
        moved = dict(travelled["events"])
        self.assertEqual(sorted(moved), [12604810, 12604840])
        appear = events.instructions(moved[12604840])
        self.assertIn((2004, 41, (clone["entity"], 2, AMELIA, 203)), appear)  # the m26 anchor object is gone
        self.assertNotIn(2601857, [w for _, _, ws in appear for w in ws])
        self.assertIn((2004, 33, (clone["entity"], AMELIA)), events.instructions(travelled["init"]))
        self.assertEqual(travelled["bar"][3], 551000)
        self.assertNoDangling(events, vanilla, msbs, NURSE)

    @needs_game
    def test_ludwigs_handover_warps_his_second_body_onto_the_boss(self):
        # NW-f1c2c6c (LIVE 13 Nov): a character warp with dummy poly -1 does nothing; the parked body fell to its death
        events, vanilla, msbs, report = self._travel(3400800, AMELIA)
        travelled, = report["travelled"]
        body2 = next(b["entity"] for b in report["bodies"][AMELIA] if b["home"] == 3400801)
        handover = events.instructions(travelled["handover"])
        self.assertIn((2004, 42, (body2, 2, AMELIA, 203, AMELIA)), handover)

    @needs_game
    def test_every_boss_model_has_the_warp_point(self):
        import arenas
        import helpers
        from containers import bnd4_read
        bodies = {e for a in arenas.ARENAS_OF_GAME for e in a.bodies}
        models = {e["model"] for stem in {a.stem for a in arenas.ARENAS_OF_GAME}
                  for e in Msb(min(Game.variants(stem).items())[1]).enemies() if e["entity"] in bodies}
        self.assertGreater(len(models), 20)
        for model in sorted(models):
            data = Game.files().read("chr/%s.chrbnd.dcx" % model)
            flver = next(e[3] for e in bnd4_read(dcx_read(data)[0])["entries"] if e[3].startswith(b"FLVER"))
            count = struct.unpack_from("<i", flver, 0x14)[0]
            points = {struct.unpack_from("<h", flver, 0x80 + k * 0x40 + 0x1c)[0] for k in range(count)}
            self.assertIn(helpers.BOSS_POINT, points, model)

    @needs_game
    def test_orphan_travels_with_both_phases_and_the_handover(self):
        events, vanilla, msbs, report = self._travel(ORPHAN, AMELIA)
        bodies = {b["home"]: b for b in report["bodies"][AMELIA]}
        p2, dummy = bodies[3600801]["entity"], bodies[3600803]["entity"]
        for msb in msbs.values():
            got = {e["entity"]: (e["model"], e["think"], e["npc"]) for e in msb.enemies()}
            self.assertEqual(got[p2], ("c4541", 454100, 454100))
            self.assertEqual(got[dummy], ("c4543", 454300, 454300))
            self.assertIn("c4541", msb.model_names())
        travelled, = report["travelled"]
        moved = dict(travelled["events"])
        handover = events.instructions(moved[13604820])
        for instruction in [(4, 2, (0, AMELIA, 3, 1056964608)), (2004, 5, (AMELIA, 0)), (2004, 10, (p2, 1)),
                            (2004, 42, (p2, 2, AMELIA, 203, AMELIA)), (2003, 18, (p2, 3031, 0)), (2004, 1, (p2, 1))]:
            self.assertIn(instruction, handover)
        self.assertIn((2004, 17, (dummy, 10, 0)), events.instructions(moved[13604830]))
        self.assertIn((2004, 33, (AMELIA, p2)), events.instructions(travelled["init"]))
        # the bar sits on phase two with the Orphan's name; the end also waits on phase two's death
        self.assertEqual(travelled["bar"][2:], (p2, 453000))
        defeat = events.instructions(12401800)
        dead = [w for b, i, w in defeat if (b, i) == (4, 0)]
        self.assertEqual(sorted(w[1] for w in dead), sorted([AMELIA, p2]))
        # her death in an or-group; phase two's in an and-group with the handover's own flag, which feeds the or-group
        mine = next(w[0] for w in dead if w[1] == AMELIA)
        theirs = next(w[0] for w in dead if w[1] == p2)
        self.assertGreater(mine, 0x7f)
        self.assertLess(theirs, 8)
        self.assertIn((3, 0, (theirs | 1 << 8, moved[13604820])), defeat)
        self.assertIn((0, 0, (mine | 1 << 8 | theirs << 16,)), defeat)
        self.assertIn((0, 0, (0 | 1 << 8 | mine << 16,)), defeat)
        self.assertEqual(travelled["handover"], moved[13604820])
        # the handover waits for her fight to start before its first test
        self.assertEqual(handover[handover.index((4, 2, (0, AMELIA, 3, 1056964608))) - 1], (3, 0, (256, 12404800)))
        self.assertNoDangling(events, vanilla, msbs, ORPHAN)

    @needs_game
    def test_orphan_arena_with_another_boss_wakes_that_boss_and_ends_on_it(self):
        import helpers
        events = Game.events("m36_00")
        report = helpers.dismiss(events, "orphan")
        again = Emevd(events.bytes())
        self.assertEqual(again.instructions(13604820), [(3, 0, (256, 13601800))])
        hold = again.instructions(13604802)
        self.assertNotIn((1003, 1, (260, 13604820)), hold)  # the AI-on branch is always phase one's: the new boss
        self.assertIn((2004, 1, (ORPHAN, 1)), hold)
        self.assertIn((2003, 11, (1, ORPHAN, 0, 453000)), hold)
        self.assertIn(ORPHAN, conditions_on(again, 13601800))
        self.assertEqual(set(report["disabled"]), {3600801, 3600803})
        self.assertIn((2004, 10, (3600801, 0)), again.instructions(report["event"]))  # parked below the arena, not falling

    @needs_game
    def test_two_orphans_in_one_map_carry_their_own_body_rows(self):
        import helpers
        events, variants = Game.events("m27_00"), Game.variants("m27_00")
        fighter = {"key": "orphan", "origin": ORPHAN, "model": None}
        rows = {(2700800, 3600801): 111111, 3600801: 222222}  # per-body key first, the plain key for the others
        out, report = helpers.apply_stem({2700800: fighter, 2700801: fighter}, events, variants,
                                         lambda e: Game.events(stem_of(e)), lambda e: Game.msb(e), rows)
        phase_two = {body: next(b["entity"] for b in bodies if b["home"] == 3600801)
                     for body, bodies in report["bodies"].items()}
        for data in out.values():
            got = {e["entity"]: e["npc"] for e in Msb(data).enemies()}
            self.assertEqual(got[phase_two[2700800]], 111111)
            self.assertEqual(got[phase_two[2700801]], 222222)

    @needs_game
    def test_extras_for_the_planner(self):
        import helpers
        from gamedata import Census
        census = Census(Game.files())
        self.assertEqual(helpers.extras_of(census.slots, LOGARIUS), ((2500801, 2500802), ("c2321", "c9010")))
        self.assertEqual(helpers.extras_of(census.slots, NURSE), ((2600801,), ()))
        self.assertEqual(helpers.extras_of(census.slots, ORPHAN), ((3600801, 3600803), ("c4541", "c4543")))
        self.assertEqual(helpers.extras_of(census.slots, ONE_REBORN), ((2800801, 2800802), ("c5071", "c5072")))
        self.assertEqual(helpers.extras_of(census.slots, AMELIA), ((), ()))

    def _one_reborn_into(self, target):
        import helpers
        events, vanilla, msbs, report = self._travel(ONE_REBORN, target)
        defeat, started = helpers.arena_flags(target)
        bodies = report["bodies"][target]
        self.assertEqual([(b["home"], b["model"], b["think"], b["npc"]) for b in bodies],
                         [(2800801, "c5071", 0, 507100), (2800802, "c5072", 507200, 507200)])
        upper, caster = (b["entity"] for b in bodies)
        for msb in msbs.values():
            got = {e["entity"]: (e["model"], e["npc"]) for e in msb.enemies()}
            self.assertEqual((got[upper], got[caster]), (("c5071", 507100), ("c5072", 507200)))
            self.assertNotIn(2800803, got)  # the HP pool stays home
            self.assertTrue({"c5071", "c5072"} <= set(msb.model_names()))
        travelled = next(t for t in report["travelled"] if t["origin"] == ONE_REBORN)
        moved = dict(travelled["events"])
        self.assertEqual(sorted(moved), [12804806, 12804830, 12804831, 12804832, 12804834, 12804835, 12804836,
                                         12804837, 12804838, 12804870, 12804871])
        self.assertIn((2004, 40, (upper, 2, target, 100, upper)), events.instructions(moved[12804806]))
        cast = events.instructions(moved[12804831])
        self.assertIn((1003, 2, (256, defeat)), cast)
        self.assertIn((3, 0, (257, started)), cast)  # the AI hold's end reads as the arena's started flag
        self.assertIn((2004, 41, (caster, 2, target, 100)), cast)
        # the AI phases test the boss's own HP; the bell maidens' commands are gone
        for event, ratio in ((12804870, 1065353216), (12804871, 1056964608)):
            body = events.instructions(moved[event])
            self.assertIn(ratio, [w[3] for b, i, w in body if (b, i) == (4, 2) and w[1] == target])
            self.assertNotIn(2800803, [w for _, _, ws in body for w in ws])
        self.assertIn((1003, 1, (258, moved[12804871])), events.instructions(moved[12804870]))
        self.assertNotIn(12804840, moved)
        init = events.instructions(travelled["init"])
        for instruction in [(2004, 33, (upper, target)), (2004, 11, (upper, target)), (2004, 12, (upper, 1)),
                            (2004, 10, (caster, 0)), (2004, 1, (upper, 0))]:
            self.assertIn(instruction, init)
        self.assertEqual(init[-3:], [(3, 0, (256, started)), (2004, 1, (upper, 1)), (2004, 1, (caster, 1))])
        self.assertEqual(travelled["bar"][2:], (target, 507000))  # the bar stays on the boss, with its own name
        self.assertEqual(report["scripts"], [("c5071", 0, 507100), ("c5072", 507200, 507200)])
        self.assertNoDangling(events, vanilla, msbs, ONE_REBORN)
        self.assertEqual([w for w in new_words(events, vanilla) if w // 10000 in (280, 1280)], [])
        return report

    @needs_game
    def test_one_reborn_takes_its_upper_body_and_caster_to_amelias_arena(self):
        report = self._one_reborn_into(AMELIA)
        self.assertEqual(report["dismissed"], [])

    @needs_game
    def test_one_reborn_takes_its_bodies_to_logarius_arena_whose_helpers_leave(self):
        report = self._one_reborn_into(LOGARIUS)
        self.assertEqual([d["arena"] for d in report["dismissed"]], [LOGARIUS])

    @needs_game
    def test_one_reborn_home_arena_is_not_dismissed_here(self):
        import helpers
        self.assertIsNone(helpers.dismiss(Game.events("m28_00"), "one_reborn"))
        self.assertEqual(helpers.OWNED[ONE_REBORN], frozenset(helpers.FIGHTS[ONE_REBORN].moved))


if __name__ == "__main__":
    unittest.main()
