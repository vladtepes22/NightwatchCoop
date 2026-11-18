"""Boss round 2 arena events per body and per kind of fight (boss_events.arena_events, generate.patched_events), on
vanilla data. Run: python -I -m unittest discover -s tools/randomizer/tests. Reads (never writes) the game at BB_GAME."""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import arenas
from containers import dcx_read
from emevd import LABEL_BANK, Emevd
from msb import EVENTS, PARTS, _i32, _i64, sections

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")
POINTS = "POINT_PARAM_ST"


class Every:
    """A model that has every animation."""
    def __contains__(self, animation):
        return True


ANIMATIONS = {"full": Every(), "none": frozenset()}
# fighters by their home body (bar names DATA, vanilla: test_randomizer's bar test)
AMELIA, BSB, LUDWIG, BEAST_GASCOIGNE, CLERIC = 2400800, 2300800, 3400800, 2410811, 2410800
NAMES = {AMELIA: 502000, BSB: 209000, LUDWIG: 451000, BEAST_GASCOIGNE: 272000, CLERIC: 500000}
CHALICE_NAME = 509000  # Bloodletting Beast's bar (survey3)


def stem_of(entity):
    return "m%02d_%02d" % (entity // 100000, entity // 10000 % 10)


def fighter(origin, model="full"):
    key = "chalice" if origin is None else str(origin)
    out = {"key": key, "model": model, "think": 1, "npc": 1, "origin": origin, "home": None, "extras": (), "extra_models": ()}
    if origin is None:
        out["bar_name"] = CHALICE_NAME
    return out


class Game:
    _files, _raw = None, {}

    @classmethod
    def files(cls):
        if cls._files is None:
            from gamedata import GameFiles
            cls._files = GameFiles(GAME)
        return cls._files

    @classmethod
    def data(cls, stem):
        if stem not in cls._raw:
            cls._raw[stem] = dcx_read(cls.files().read("event/%s_00_00.emevd.dcx" % stem))[0]
        return cls._raw[stem]

    @classmethod
    def events(cls, stem):
        return Emevd(cls.data(stem))

    @classmethod
    def origin_events(cls, entity):
        return cls.events(stem_of(entity))

    @classmethod
    def entities(cls, stem):
        """Entity ids the map's MSB holds: parts (any type), regions and MSB events."""
        data = dcx_read(cls.files().read("map/mapstudio/%s_00_00.msb.dcx" % stem))[0]
        table = sections(data)
        out = set()
        for at in table[PARTS][2][:-1]:
            out.add(_i32(data, at + _i64(data, at + 0xb0)))
        for at in table[POINTS][2][:-1]:
            ent = _i64(data, at + 0x48)
            if ent > 0:
                out.add(_i32(data, at + ent))
        for at in table[EVENTS][2][:-1]:
            base = _i64(data, at + 0x18)
            if base > 0:
                out.add(_i32(data, at + base + 8))
        return out


def body_of(events, event):
    return [(b, i, tuple(w)) for b, i, w in events.instructions(event)]


@needs_game
class ArenaEvents(unittest.TestCase):
    def build(self, stem, placed, fighters, modes=None):
        """patched_events on vanilla data; checks the file re-parses and names no entity its map does not hold.
        Returns (vanilla Emevd, patched Emevd, report)."""
        from gamedata import _events
        from generate import patched_events
        data = Game.data(stem)
        patched, _, report = patched_events(data, stem, placed, {f["key"]: f for f in fighters}, modes or {}, ANIMATIONS,
                                            Game.origin_events)
        self.assertIsNotNone(patched)
        vanilla, again = Emevd(data), Emevd(patched)
        parsed = _events(patched)
        self.assertEqual(sorted(parsed), again.event_ids())
        for event in again.event_ids():
            self.assertEqual([tuple(x) for x in parsed[event][0]], [tuple(x) for x in again.instructions(event)], event)
            for i in again.params(event):
                self.assertLess(i, len(again.instructions(event)), event)
        self.assertEqual(struct.unpack_from("<I", patched, 0xc)[0], len(patched))
        # refcheck: every seven-digit id the edits brought in is an entity of this map
        # (9999999 is a part HP in Amelia's and Ludwig's part templates, 2004[22], not an entity)
        words = lambda ev: {w for e in ev.event_ids() for _, _, ws in ev.instructions(e) for w in ws if 1000000 <= w < 9999999}
        self.assertEqual(sorted(words(again) - words(vanilla) - Game.entities(stem)), [])
        # the co-op id swaps (hostonly.cover, test_hostonly) undone in the file and in the report (which names the ids
        # the file ends with), so both read as the arena edits wrote them
        from generate import _swapped
        covered = report["host_only"].get("covered", ()) if report.get("host_only") else ()
        for copy, listed, *_ in reversed(covered):
            again.swap_ids(copy, listed)
            report = dict(_swapped({k: v for k, v in report.items() if k != "host_only"}, copy, listed),
                          host_only=report["host_only"])
        return vanilla, again, report

    def assertLabel(self, events, event, i):
        self.assertEqual(events.instructions(event)[i][0], LABEL_BANK, (event, i))

    def assertDoped(self, events, event, anchor, entities):
        """Each doping call on the anchor in the event is followed by the same call for each entity, in order."""
        body = events.instructions(event)
        lines = [k for k, (b, i, w) in enumerate(body) if ((b, i) == (2004, 8) and w[0] == anchor and w[1] in (7500, 7501))
                 or ((b, i) == (2004, 53) and w == (anchor,))]
        self.assertEqual(len(lines), 4, event)
        for k in lines:
            bank, iid, words = body[k]
            self.assertEqual(body[k + 1:k + 1 + len(entities)], [(bank, iid, (e,) + words[1:]) for e in entities], k)

    def bar(self, events, event, i):
        bank, iid, words = events.instructions(event)[i]
        self.assertEqual((bank, iid), (2003, 11), (event, i))
        return words[1], words[3]

    def new_events(self, vanilla, again):
        return [e for e in again.event_ids() if e not in vanilla.index]

    # ------------------------------------------------------------ Ludwig elsewhere
    def test_ludwig_elsewhere(self):
        # Ludwig in Amelia's arena: his copies end on her defeat, not on 9471, and his teleport comes along
        vanilla, ev, report = self.build("m24_00", {2400800: str(LUDWIG)}, [fighter(LUDWIG)])
        added = [e for _, _, ids in report["parts"] + report["phases"] for e in ids]
        self.assertEqual(len(added), 1 + 1 + 4)  # part template 13404830, teleport 13404840, phases 13404820-823
        words = [w for e in added for _, _, ws in ev.instructions(e) for w in ws]
        self.assertNotIn(9471, words)
        self.assertNotIn(3400800, words)
        self.assertNotIn(3400801, words)
        self.assertIn(12401800, words)
        self.assertIn(2400800, words)
        self.assertEqual(self.bar(ev, 12404802, 29), (2400800, NAMES[LUDWIG]))

    def test_body_events_travel_and_stay_home_silenced(self):
        """BODY_EVENTS follow their boss (user, 8 Nov: the Moon Presence's Doom Gaze one-shot elsewhere: m21 12104870
        keeps the player alive): a copy naming the new body in the other map; at home, another boss silences them."""
        import boss_events
        for origin, events in boss_events.BODY_EVENTS.items():
            vanilla, ev, _ = self.build("m24_00", {AMELIA: str(origin)}, [fighter(origin)])
            source = Game.origin_events(origin)
            renames = boss_events._renames(origin, AMELIA)  # the boss and its fight flags
            for event in events:
                want = [(b, d, tuple(renames.get(w, w) for w in words))
                        for b, d, words in source.instructions(event)]
                copies = [e for e in self.new_events(vanilla, ev)
                          if [(b, d, tuple(w)) for b, d, w in ev.instructions(e)] == want]
                self.assertEqual(len(copies), 1, (origin, event))
                self.assertIn((0, copies[0], 0), [tuple(w) for b, d, w in ev.instructions(0) if (b, d) == (2000, 0)])
        vanilla, ev, _ = self.build("m21_00", {2100810: str(AMELIA)}, [fighter(AMELIA)])
        self.assertEqual([b for b, _, _ in ev.instructions(12104870)], [LABEL_BANK])

    def test_arena_animations_never_play_on_a_moved_boss(self):
        """NW-b6df2d1 (9 Nov): Maria in Amelia's arena held Amelia's retry-start animation 7002 (12404802 line 30), since
        her model has a 7002 of its own: no damage, no fight. The arena's own forced animations go on a moved boss even
        when its model has the id; its own copied events keep theirs (Ludwig's teleport 13404840 forces 3017)."""
        maria = 3500800
        vanilla, ev, _ = self.build("m24_00", {AMELIA: str(maria)}, [fighter(maria)])
        self.assertEqual(vanilla.instructions(12404802)[30], (2003, 18, (AMELIA, 7002, 0)))
        self.assertLabel(ev, 12404802, 30)
        for event in vanilla.event_ids():
            for b, d, w in ev.instructions(event):
                if (b, d) == (2003, 18) and w and w[0] == AMELIA:
                    self.assertEqual(w[1], -1, event)  # only "stop what plays" stays
        vanilla, ev, _ = self.build("m24_00", {AMELIA: str(LUDWIG)}, [fighter(LUDWIG)])
        teleports = [e for e in self.new_events(vanilla, ev)
                     if any((b, d) == (2003, 18) and w[:2] == (AMELIA, 3017) for b, d, w in ev.instructions(e))]
        self.assertEqual(len(teleports), 1)

    def test_moved_amygdala_takes_damage_on_every_part(self):
        """User, 8 Nov ("so you can damage her again"): Amygdala in another arena brings her part templates, but no run
        gives an event part a damage correction under 1.0 (her tail and legs are 0.2 at home, m33 13304830 runs
        3308-3310; LIVE, Cleric Beast's arena: 1 HP a hit with them, 30-36 without). Every other argument of every run is
        as at home (renamed), and her home file keeps its 0.2."""
        import struct
        import boss_events
        amygdala = 3300800
        as_float = lambda w: struct.unpack("<f", struct.pack("<i", w))[0]
        vanilla, ev, _ = self.build("m24_01", {CLERIC: str(amygdala)}, [fighter(amygdala)])
        source = Game.origin_events(amygdala)
        home_runs = [tuple(w) for b, d, w in source.instructions(0) if (b, d) == (2000, 0) and w[1] == 13304830]
        self.assertEqual(len(home_runs), 10)
        self.assertEqual(sorted(round(as_float(w[8]), 2) for w in home_runs), [0.2] * 3 + [1.0] * 7)
        renames = boss_events._renames(amygdala, CLERIC)
        copies = [e for e in self.new_events(vanilla, ev)
                  if [(b, d, tuple(w)) for b, d, w in ev.instructions(e)]
                  == [(b, d, tuple(renames.get(x, x) for x in w)) for b, d, w in source.instructions(13304830)]]
        self.assertEqual(len(copies), 1)
        runs = [tuple(w) for b, d, w in ev.instructions(0) if (b, d) == (2000, 0) and w[1] == copies[0]]
        self.assertEqual(len(runs), 10)
        for home, run in zip(home_runs, runs):
            self.assertEqual(run[2:8], home[2:8])  # part, group, effect, HP, animation as at home
            self.assertEqual([as_float(w) for w in run[8:10]],
                             [max(1.0, round(as_float(w), 2)) for w in home[8:10]], home[2])
        # the weapon-arm parts (13304820, literal corrections 0: her clubs after SpEffect 5402) still come along
        masks = [e for e in self.new_events(vanilla, ev)
                 if [(b, d) for b, d, _ in ev.instructions(e)] == [(b, d) for b, d, _ in source.instructions(13304820)]]
        self.assertEqual(len(masks), 1)
        self.assertEqual(boss_events.ONE, struct.unpack("<i", struct.pack("<f", 1.0))[0])

    # ------------------------------------------------------------ GROUP
    def test_witch_mix_off_is_one_boss(self):
        """Mix off (user, 8 Nov): the boss on body 1 alone; body 2 disabled and dead at load, its bar and its enable gone,
        the defeat on body 1's death alone, the pair's immortality template silenced."""
        vanilla, ev, report = self.build("m22_00", {2200800: str(AMELIA)}, [fighter(AMELIA, "none")], {"witch": "normal"})
        self.assertEqual(self.bar(ev, 12204802, 27), (2200800, NAMES[AMELIA]))
        self.assertLabel(ev, 12204807, 10)  # body 2's bar
        self.assertLabel(ev, 12201800, 14)  # the defeat's wait on body 2's death
        self.assertEqual(ev.instructions(12201800)[13], (4, 0, (1, 2200800, 1)))
        self.assertLabel(ev, 12204810, 26)  # body 2's enable at body 1's half HP
        self.assertEqual([b for b, _, _ in ev.instructions(12204812)], [LABEL_BANK])
        at_load = [e for e in self.new_events(vanilla, ev)
                   if (2004, 4, (2200801, 0)) in [(b, d, tuple(w)) for b, d, w in ev.instructions(e)]]
        self.assertEqual(len(at_load), 1)
        self.assertIn((2004, 5, (2200801, 0)), [(b, d, tuple(w)) for b, d, w in ev.instructions(at_load[0])])

    def test_witch_mix_on(self):
        for placed, names in (({2200800: str(AMELIA), 2200801: str(BSB)}, (NAMES[AMELIA], NAMES[BSB])),):
            fighters = [fighter(AMELIA, "none"), fighter(BSB)]
            vanilla, ev, report = self.build("m22_00", placed, fighters, {"witch": "mix"})
            self.assertEqual(self.bar(ev, 12204802, 27), (2200800, names[0]))
            self.assertEqual(self.bar(ev, 12204807, 10), (2200801, names[1]))
            self.assertLabel(ev, 12204811, 2)  # the think switch to the Witch's
            self.assertLabel(ev, 12204811, 7)
            self.assertLabel(ev, 12204812, 13)  # her 7000/7001, fed by the template's runs: the new model has none
            self.assertLabel(ev, 12204812, 15)
            # the group event stays, its forced 3011 aside: the Witch's move, off on every boss in her bodies (NW-b6df2d1)
            self.assertEqual([x for x in body_of(ev, 12204839) if x[0] != LABEL_BANK],
                             [x for x in body_of(vanilla, 12204839) if x[0] != LABEL_BANK and not (x[:2] == (2003, 18) and x[2][0] in placed)])
            self.assertEqual(len(ev.instructions(12204812)), len(vanilla.instructions(12204812)))  # both immortal till both at HP 1
            self.assertEqual(ev.instructions(0)[0], (2003, 2, (12201802, 1)))

    def test_shadows_with_a_chalice_boss_and_their_intro(self):
        placed = {2700800: "chalice", 2700801: str(AMELIA), 2700802: str(BSB)}
        vanilla, ev, report = self.build("m27_00", placed, [fighter(None), fighter(AMELIA), fighter(BSB)], {"shadows": "mix"})
        self.assertEqual([self.bar(ev, 12704802, i) for i in (55, 56, 57)],
                         [(2700800, CHALICE_NAME), (2700801, NAMES[AMELIA]), (2700802, NAMES[BSB])])
        self.assertEqual(report["starts"], [(2700800, 12701802)])  # arenas.py's intro, not the detected one
        intro = body_of(vanilla, 12701802)
        self.assertEqual(body_of(ev, 12701802), intro[2:6] + [(2003, 2, (12701802, 1))])  # the three warp to their places
        self.assertEqual(ev.instructions(0)[0], (2003, 2, (12701802, 1)))
        self.assertEqual(body_of(ev, 12701800), body_of(vanilla, 12701800))  # ends when all three are dead
        self.assertEqual(body_of(ev, 12704806), body_of(vanilla, 12704806))
        chalice_moved = [ids for body, _, ids in report["parts"] + report["phases"] if body == 2700800]
        self.assertEqual(chalice_moved, [[], []])  # a chalice boss brings no events

    # ------------------------------------------------------------ Celestial Emissary
    SMALLS = (2420711, 2420712, 2420713, 2420716, 2420717, 2420719, 2420720)

    def setup_event(self, vanilla, ev):
        new = [e for e in self.new_events(vanilla, ev) if any(b == 2004 and i == 5 for b, i, _ in ev.instructions(e))]
        self.assertEqual(len(new), 1)
        return body_of(ev, new[0])

    def test_emissary_normal(self):
        vanilla, ev, report = self.build("m24_02", {2420811: str(AMELIA)}, [fighter(AMELIA)], {"emissary": "normal"})
        growth = body_of(ev, 12424790)
        self.assertEqual(growth[7], (3, 0, (257, 12424700)))  # grows when the fight starts
        for i in (4, 11, 12, 13):  # no damage link, no AI message
            self.assertEqual(growth[i][0], LABEL_BANK, i)
        self.assertIn((2004, 5, (2420810, 0)), growth)  # the small form goes at the start
        self.assertEqual(self.bar(ev, 12424790, 21), (2420811, NAMES[AMELIA]))
        self.assertLabel(ev, 12424702, 46 + 4)  # the small form's bar (after 2420811's four doping calls)
        self.assertDoped(ev, 12424702, 2800800, [2420811])
        setup = self.setup_event(vanilla, ev)
        self.assertEqual({w[0] for b, i, w in setup if (b, i) == (2004, 5)}, set(self.SMALLS) | {2420750, 2420751})
        self.assertEqual({w for b, i, w in setup if (b, i) == (2003, 3)}, {(e + 3000, 0) for e in self.SMALLS})
        self.assertFalse([1 for e in ev.event_ids() for b, i, w in ev.instructions(e)
                          if (b, i) == (2004, 5) and w[0] in self.SMALLS and w[1] == 1])
        self.assertIn((2004, 5, (2420810, 1)), body_of(ev, 12421703))  # the second-attempt start still brings 2420810
        self.assertEqual(len(ev.instructions(12424791)), 1)
        self.assertEqual(len(ev.instructions(12424792)), 1)
        self.assertEqual(body_of(ev, 12421700), body_of(vanilla, 12421700))  # the end: 2420811's death
        calls = [w for b, i, w in ev.instructions(0) if (b, i) == (2000, 0)]
        self.assertFalse([w for w in calls if w[1] == 12424787 or w[1:3] == (12424785, 2420811)])

    def test_emissary_mix(self):
        placed = {2420811: str(AMELIA), **{e: str(BSB) for e in self.SMALLS}}
        vanilla, ev, report = self.build("m24_02", placed, [fighter(AMELIA), fighter(BSB)], {"emissary": "mix"})
        self.assertEqual(body_of(ev, 12424790)[7], (3, 0, (257, 12424700)))
        setup = self.setup_event(vanilla, ev)
        self.assertEqual({w[0] for b, i, w in setup if (b, i) == (2004, 5)}, {2420750, 2420751})
        self.assertEqual({w for b, i, w in setup if (b, i) == (2003, 3)}, {(e + 3000, 0) for e in self.SMALLS})  # spawned once
        self.assertEqual({w[0] for b, i, w in body_of(ev, 12421703) if (b, i) == (2004, 5)}, set(self.SMALLS) | {2420810})
        bars = [(e, w) for e in ev.event_ids() for b, i, w in ev.instructions(e) if (b, i) == (2003, 11) and w[0] == 1]
        self.assertEqual([w[1] for _, w in bars if w[1] in self.SMALLS + (2420810, 2420811)], [2420811])
        self.assertDoped(ev, 12424702, 2800800, [2420811] + list(self.SMALLS))  # every body with a boss, at the start

    # ------------------------------------------------------------ Living Failures
    FAILURES = (3500851, 3500852, 3500853, 3500854)

    def test_failures_normal(self):
        vanilla, ev, report = self.build("m35_00", {3500851: str(AMELIA)}, [fighter(AMELIA)], {"failures": "normal"})
        for i in (5, 11, 17, 23):  # no respawns
            self.assertLabel(ev, 13505662, i)
        self.assertEqual(ev.instructions(13504852)[13], (2004, 33, (3500851, 3500850)))  # damage to the pool
        self.assertEqual(self.bar(ev, 13504852, 52 + 4), (3500850, NAMES[AMELIA]))  # after 3500851's doping calls
        self.assertDoped(ev, 13504852, 3500850, [3500851])
        setup = self.setup_event(vanilla, ev)
        self.assertIn((2004, 12, (3500851, 1)), setup)
        self.assertEqual({w[0] for b, i, w in setup if (b, i) == (2004, 5)}, {3500852, 3500853, 3500854})
        self.assertEqual(body_of(ev, 13501850), body_of(vanilla, 13501850))  # ends on the pool's HP 0
        self.assertFalse([e for e in self.new_events(vanilla, ev) if any(b == 2004 and i == 4 for b, i, _ in ev.instructions(e))])

    def test_failures_mix_ends_when_the_four_present_are_dead(self):
        placed = {3500851: str(AMELIA), 3500852: str(BSB), 3500853: str(LUDWIG), 3500854: str(BSB)}
        vanilla, ev, report = self.build("m35_00", placed, [fighter(AMELIA), fighter(BSB), fighter(LUDWIG)],
                                         {"failures": "mix"})
        for i in (13, 14, 15, 16):
            self.assertLabel(ev, 13504852, i)
        self.assertEqual(self.bar(ev, 13504852, 52 + 4), (3500851, NAMES[AMELIA]))
        self.assertDoped(ev, 13504852, 3500850, [3500851])  # the one there at load: with the pool
        end = [e for e in self.new_events(vanilla, ev) if any((b, i) == (2004, 4) for b, i, _ in ev.instructions(e))]
        self.assertEqual(len(end), 1)
        body = body_of(ev, end[0])
        self.assertEqual({w[1] for b, i, w in body if (b, i) == (4, 0)}, set(placed))
        self.assertEqual(body[-2:], [(0, 0, (65792,)), (2004, 4, (3500850, 0))])
        spawn = [e for e in self.new_events(vanilla, ev) if any((b, i, tuple(w)) == (2003, 3, (3503815, 1))
                                                                for b, i, w in ev.instructions(e))]
        self.assertEqual(len(spawn), 1)
        spawned = body_of(ev, spawn[0])
        self.assertIn((3, 0, (256, 13504858)), spawned)
        # bars of the next two once they are there (three slots at most), each with its boss's name
        bars = [(b, i, w) for b, i, w in spawned if (b, i) == (2003, 11)]
        self.assertEqual(bars, [(2003, 11, (1, 3500852, 1, NAMES[BSB])), (2003, 11, (1, 3500853, 2, NAMES[LUDWIG]))])
        # then the three spawned ones are doped by the pool's doping path (they did not exist when the pool was doped)
        path = spawned[spawned.index(bars[-1]) + 1:]
        pool = body_of(vanilla, 13504852)
        self.assertEqual(path[:2], [pool[19], pool[20]])  # the set-up event's checks after its wait, as they are
        for effect in (7500, 7501):
            self.assertEqual([w[0] for b, i, w in path if (b, i) == (2004, 8) and w[1] == effect], [3500852, 3500853, 3500854])
        self.assertEqual([w[0] for b, i, w in path if (b, i) == (2004, 53)], [3500852, 3500853, 3500854] * 2)
        self.assertEqual([w for b, i, w in path if (b, i) == (1003, 109)], [w for b, i, w in pool if (b, i) == (1003, 109)])
        self.assertFalse([w for b, i, w in path if b == 2003 or (b == 2004 and w[0] == 3500850)])
        self.assertEqual(path[-1], (LABEL_BANK, 4, ()))  # ends on the label its jumps land on
        # the defeat: the pool's HP 0 or all four dead, the rest as vanilla
        either = [(4, 2, (255, 3500850, 0, 0))] + [(4, 0, (1, e, 1)) for e in self.FAILURES] +                  [(0, 0, (0xff | 1 << 8 | 1 << 16,)), (0, 0, (1 << 8 | 0xff << 16,))]
        defeat = body_of(vanilla, 13501850)
        self.assertEqual(body_of(ev, 13501850), defeat[:15] + either + defeat[16:])

    # ------------------------------------------------------------ One Reborn
    def test_one_reborn_keeps_the_pool(self):
        vanilla, ev, report = self.build("m28_00", {2800800: str(AMELIA)}, [fighter(AMELIA)])
        self.assertEqual(ev.instructions(12804802)[18], (2004, 12, (2800800, 1)))
        self.assertEqual(ev.instructions(12804802)[71], (2004, 33, (2800800, 2800803)))
        self.assertEqual(self.bar(ev, 12804802, 70), (2800803, NAMES[AMELIA]))
        self.assertLabel(ev, 12801803, 5)
        self.assertEqual(len(ev.instructions(12804806)), 1)
        self.assertEqual(len(ev.instructions(12804840)), 1)
        setup = self.setup_event(vanilla, ev)
        self.assertEqual({w[0] for b, i, w in setup if (b, i) == (2004, 5)}, {2800801, 2800802})
        self.assertEqual(ev.instructions(0)[0], (2003, 2, (12801802, 1)))

    # ------------------------------------------------------------ second attempts (D9)
    def test_laurence_and_ludwig_intro_flags_are_set_first(self):
        vanilla, ev, report = self.build("m34_00", {3400850: str(AMELIA), 3400800: str(BSB)},
                                         [fighter(AMELIA), fighter(BSB)])
        n = len(vanilla.instructions(0))
        self.assertEqual(sorted(ev.instructions(0)[:2]), [(2003, 2, (13401801, 1)), (2003, 2, (13401851, 1))])
        self.assertEqual(ev.instructions(0)[2:n + 2], vanilla.instructions(0))  # then vanilla, then the new calls
        self.assertEqual(sorted(report["first"]), [("laurence", 13401851), ("ludwig", 13401801)])
        # it now finds the flag on and lets her go; its 7002 loop, Laurence's own, is off on the new boss (NW-b6df2d1)
        loop = [i for i, x in enumerate(body_of(vanilla, 13404861)) if x == (2003, 18, (3400850, 7002, 1))]
        self.assertEqual(len(loop), 1)
        self.assertLabel(ev, 13404861, loop[0])
        self.assertEqual([x for i, x in enumerate(body_of(ev, 13404861)) if i != loop[0]],
                         [x for i, x in enumerate(body_of(vanilla, 13404861)) if i != loop[0]])

    def test_new_event_ids_are_free(self):
        vanilla, ev, report = self.build("m24_01", {2410800: str(LUDWIG), 2410810: str(AMELIA)},
                                         [fighter(LUDWIG), fighter(AMELIA)])
        new = self.new_events(vanilla, ev)
        self.assertTrue(new)
        named = {w for e in vanilla.event_ids() for _, _, ws in vanilla.instructions(e) for w in ws}
        self.assertFalse(set(new) & named)
        self.assertTrue(all(e // 1000 == 12414 for e in new))


if __name__ == "__main__":
    unittest.main()
