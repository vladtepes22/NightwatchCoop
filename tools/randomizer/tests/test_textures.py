"""Models that draw with another model's textures (textures.py; LIVE 23 Nov, beta 3: the Orphan's phase two c4541 alone
in the Witch's arena was invisible at range and black up close). The source model counts in every budget with them
(planner.with_sources) and the generator gives the map a hidden part of it (generate.texture_parts).
Run: python -I -m unittest discover -s tools/randomizer/tests. Reads (never writes) the game at BB_GAME."""
import os
import struct
import sys
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

GAME = os.environ.get("BB_GAME", r"C:\Games\CUSA03173")
needs_game = unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files")


def _tpf(names):
    """A PS4 TPF holding only names (UTF-16), the layout textures.tpf_names reads."""
    head = bytearray(b"TPF\0" + struct.pack("<ii", 0, len(names)) + bytes((4, 3, 1, 0)))
    table = bytearray(0x24 * len(names))
    blob = bytearray()
    start = 0x10 + len(table)
    for i, name in enumerate(names):
        struct.pack_into("<i", table, i * 0x24 + 0x18, start + len(blob))
        blob += name.encode("utf-16-le") + b"\0\0"
    return bytes(head + table + blob)


class Reading(unittest.TestCase):
    def test_tpf_and_flver_names(self):
        import textures
        self.assertEqual(textures.tpf_names(_tpf(["c4541_wing_a", "C4541_Wing_N"])), {"c4541_wing_a", "c4541_wing_n"})
        flver = "N:\\SPRJ\\data\\Model\\chr\\c4540\\tex\\c4540_body_a.tga".encode("utf-16-le") + b"\0\0"
        flver += "c4541_wing_a.tga".encode("utf-16-le")
        self.assertEqual(textures.flver_textures(b"\1\2" + flver), {("c4540", "c4540_body_a"), (None, "c4541_wing_a")})

    def test_sources_count_in_the_budget_transitively(self):
        import planner
        try:
            planner.TEXTURE_SOURCES.update({"c2712": ["c2711"], "c2711": ["c2710"]})
            self.assertEqual(planner.with_sources({"c2712", "c1000"}), {"c2712", "c2711", "c2710", "c1000"})
            sizes = {"c2712": 1, "c2711": 1, "c2710": 1}
            limits = types.SimpleNamespace()
            self.assertFalse(planner._fits({"c2712"}, sizes, limits, 2, 100))  # three models with its sources
            self.assertTrue(planner._fits({"c2712"}, sizes, limits, 3, 3))
        finally:
            planner.TEXTURE_SOURCES.clear()


@needs_game
class Game(unittest.TestCase):
    _files = None

    @classmethod
    def files(cls):
        if cls._files is None:
            from gamedata import GameFiles
            cls._files = GameFiles(GAME, [])
        return cls._files

    def test_the_orphans_phase_two_needs_phase_one(self):
        """DATA: c4541's FLVER names c4540's body and blade textures, which only c4540.chrbnd holds; c1110 needs only its
        family archive c1119 (the game loads it with it), so it has no source."""
        import textures
        found = textures.sources(self.files(), ["c4541", "c4540", "c1110", "c2100"])
        self.assertEqual(found, {"c4541": ["c4540"]})

    def test_a_map_gets_a_hidden_part_of_a_missing_source(self):
        """The Witch's arena with c4541 on her body (the world of the report): c4540 is added as a hidden part parked
        under her, held at load in event 0; a map that already loads the source gets nothing."""
        from containers import dcx_read
        from emevd import Emevd
        from generate import texture_parts
        from msb import Msb
        files = self.files()
        msb = Msb(dcx_read(files.read("map/mapstudio/m22_00_00_00.msb.dcx"))[0])
        witch = next(e for e in msb.enemies() if e["entity"] == 2200800)
        self.assertEqual(msb.swap_enemies({witch["part"]: ("c4541", 454100, 454100)}), {witch["part"]})
        census = types.SimpleNamespace(texture_sources={"c4541": ["c4540"]}, candidates={},
                                       slots={"m36_00": [{"model": "c4540", "think": 454000, "npc": 454000}]})
        events = Emevd(dcx_read(files.read("event/m22_00_00_00.emevd.dcx"))[0])
        before = len(events.instructions(0))
        out, hidden = texture_parts(census, {"m22_00_00_00": msb.bytes()}, events)
        self.assertEqual([(h["model"], h["for"]) for h in hidden], [("c4540", ["c4541"])])
        entity = hidden[0]["entity"]
        result = Msb(out["m22_00_00_00"])
        added = [e for e in result.enemies() if e["entity"] == entity]
        self.assertEqual([(e["model"], e["think"], e["npc"]) for e in added], [("c4540", 454000, 454000)])
        at = lambda e: struct.unpack_from("<3f", result.data, e["at"] + 0x28)
        user = next(e for e in result.enemies() if e["entity"] == 2200800)
        self.assertAlmostEqual(at(user)[1] - at(added[0])[1], 80.0, places=3)  # parked under the body that needs it
        self.assertEqual(at(user)[0::2], at(added[0])[0::2])
        # held like a parked body, never disabled (a disabled part's model never loads: LIVE 23 Nov)
        self.assertEqual([(b, i, tuple(w)) for b, i, w in events.instructions(0)[before:]],
                         [(2004, 1, (entity, 0)), (2004, 10, (entity, 0)), (2004, 30, (entity, 0)), (2004, 15, (entity, 1))])
        again, none = texture_parts(census, out, Emevd(dcx_read(files.read("event/m22_00_00_00.emevd.dcx"))[0]))
        self.assertEqual(none, [])


if __name__ == "__main__":
    unittest.main()
