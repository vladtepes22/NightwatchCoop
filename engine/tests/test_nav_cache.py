"""tools/party/nav_cache.py: the map-frame offset search (unit) and a data check of the Nightmare maps (game data)."""
import os
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "party"))
import nav_cache as nc  # noqa: E402

GAME = Path(os.environ.get("BB_NAV_TEST_GAME", r"E:\games\bloodborne\dvdroot_ps4"))


class OnMesh(unittest.TestCase):
    def test_count(self):
        verts = [(0, 0, 0), (0, 0, 10), (10, 0, 10), (10, 0, 0)]
        faces = [(0, [0, 1, 2, 3])]
        pts = [(5, 0.5, 5), (5, 3.0, 5), (15, 0, 5), (9.9, 0, 0.1)]
        self.assertEqual(nc.on_mesh_count(verts, faces, pts), 2)  # too high, and outside, do not count

    def test_shifted_mesh_found_by_spots(self):
        verts = [(100, 1500, 100), (100, 1500, 110), (110, 1500, 110), (110, 1500, 100)]
        faces = [(0, [0, 1, 2, 3])]
        spots = [(105, 1500.2, 105), (101, 1499.5, 109)]
        self.assertEqual(nc.on_mesh_count(verts, faces, spots), 2)
        local = [(v[0] - 100, v[1] - 1500, v[2] - 100) for v in verts]
        self.assertEqual(nc.on_mesh_count(local, faces, spots), 0)  # in the local frame the spots are off the mesh


@unittest.skipUnless((GAME / "map" / "m34_00_00_00").is_dir(), "game data not present")
class NightmareFrame(unittest.TestCase):
    def test_offsets_put_lamps_on_the_mesh(self):
        want = {"m34_00_00_00": (-500.0, 1500.0, -500.0, 0.0), "m35_00_00_00": (-792.2, 1489.5, -723.8, 0.0),
                "m24_01_00_00": (0.0, 0.0, 0.0, 0.0), "m27_00_00_00": (-190.31, -187.18, 312.4, -70.0)}
        for m, off in want.items():
            got, n, total, _c = nc.choose_offset(GAME, m)
            self.assertEqual(got["default"], off, m)
            self.assertGreaterEqual(n, max(2, total // 2), m)


if __name__ == "__main__":
    unittest.main()
