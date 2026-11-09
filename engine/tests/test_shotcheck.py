"""tools/mp/shotcheck.py: black-frame detector, the visual verdict and the missing-Pillow path. No game needed."""
from paths import ROOT
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / "tools" / "mp"))
import shotcheck  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402


def solid(v, size=(320, 180)):
    return Image.new("RGB", size, (v, v, v))


def loading_screen():
    img = solid(0)
    d = ImageDraw.Draw(img)
    d.rectangle((100, 150, 220, 160), fill=(200, 200, 200))  # tip text / spinner
    return img


def scene():
    img = Image.new("RGB", (320, 180))
    for x in range(320):
        for y in range(180):
            img.putpixel((x, y), ((x * 3) % 200 + 30, (y * 5) % 180 + 20, 90))
    return img


class Detector(unittest.TestCase):
    def test_all_zero_is_black(self):
        self.assertTrue(shotcheck.is_black(solid(0)))

    def test_flat_dark_fill_is_black(self):
        self.assertTrue(shotcheck.is_black(solid(8)))

    def test_loading_screen_is_not_black(self):
        self.assertFalse(shotcheck.is_black(loading_screen()))

    def test_scene_is_not_black(self):
        self.assertFalse(shotcheck.is_black(scene()))

    def test_flat_grey_is_not_black(self):  # a grey screen is the grey detector's case, not this one
        self.assertFalse(shotcheck.is_black(solid(90)))


class Verdict(unittest.TestCase):
    def test_no_shots_fails(self):
        ok, why = shotcheck.judge([])
        self.assertFalse(ok)
        self.assertIn("no screenshot", why)

    def test_all_black_fails(self):
        ok, why = shotcheck.judge([True] * 6)
        self.assertFalse(ok)
        self.assertIn("all 6", why)

    def test_loading_then_world_passes(self):
        self.assertTrue(shotcheck.judge([True, True, False, False, False, False])[0])

    def test_one_fade_passes(self):
        self.assertTrue(shotcheck.judge([False, False, True, False, False])[0])

    def test_black_after_the_first_picture_fails(self):
        ok, why = shotcheck.judge([False, True, True, True, False])
        self.assertFalse(ok)
        self.assertIn("3 of 5", why)


class Dir(unittest.TestCase):
    def phase(self, tmp, shots, count=2, headless=0):
        d = Path(tmp) / "ph"
        (d / "shots").mkdir(parents=True)
        (d / "instances.json").write_text(json.dumps({"count": count, "headless": headless}))
        for name, img in shots.items():
            img.save(d / "shots" / name)
        return d

    def test_black_instance_flagged_other_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            shots = {}
            for t in (10, 20, 30, 40):
                shots["x_%ds_inst0.png" % t] = scene()
                shots["x_%ds_inst1.png" % t] = solid(0)
            rows = shotcheck.visual_rows("x", self.phase(tmp, shots))
        self.assertEqual([r[0] for r in rows], ["x: visual check inst0", "x: visual check inst1"])
        self.assertEqual([r[1] for r in rows], [True, False])
        self.assertIn("black", rows[1][2])

    def test_missing_instance_shots_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = shotcheck.visual_rows("x", self.phase(tmp, {"x_10s_inst0.png": scene()}))
        self.assertEqual([r[1] for r in rows], [True, False])

    def test_headless_instance_not_expected(self):
        with tempfile.TemporaryDirectory() as tmp:
            rows = shotcheck.visual_rows("x", self.phase(tmp, {"x_10s_inst0.png": scene()}, count=2, headless=1))
        self.assertEqual(len(rows), 1)

    def test_no_shots_folder_is_no_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(shotcheck.visual_rows("x", Path(tmp)), [])

    def test_time_order_numeric(self):
        self.assertLess(shotcheck.shot_order(Path("a_9s_inst0.png")), shotcheck.shot_order(Path("a_10s_inst0.png")))


class MissingPillow(unittest.TestCase):
    def fake_import(self, name):
        raise ImportError("No module named 'PIL'")

    def test_require_pil_raises_a_clear_error(self):
        with self.assertRaises(shotcheck.ShotDependencyError) as cm:
            shotcheck.require_pil(self.fake_import)
        msg = str(cm.exception)
        self.assertIn("pip install pillow", msg)
        self.assertIn("--no-shots", msg)

    def test_require_pil_returns_module_when_present(self):
        self.assertTrue(hasattr(shotcheck.require_pil(), "frombytes"))

    def test_smoke_main_stops_before_any_phase(self):
        """smoke_party.main() exits with the dependency message (no phase, no game) when Pillow is missing."""
        import importlib.util
        from unittest import mock
        spec = importlib.util.spec_from_file_location("smoke_shotcheck", ROOT / "tools" / "mp" / "smoke_party.py")
        smoke = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(smoke)
        with mock.patch.object(shotcheck, "require_pil", side_effect=shotcheck.ShotDependencyError("Pillow (PIL) is missing")), \
                mock.patch.object(sys, "argv", ["smoke_party.py", "--only", "lamps"]), \
                mock.patch.object(smoke, "run_phase_retrying") as ran:
            with self.assertRaises(SystemExit) as cm:
                smoke.main()
        self.assertIn("Pillow", str(cm.exception.code))
        ran.assert_not_called()

    def test_no_shots_skips_the_check(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("smoke_shotcheck2", ROOT / "tools" / "mp" / "smoke_party.py")
        smoke = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(smoke)
        v = smoke.Verdict()
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "shots").mkdir()
            smoke.add_visual(v, "x", tmp, type("A", (), {"no_shots": True})())
            self.assertEqual(v.rows, [])
            smoke.add_visual(v, "x", tmp, type("A", (), {"no_shots": False})())
            self.assertEqual([r[1] for r in v.rows], [False])  # an empty shots folder fails inst0


if __name__ == "__main__":
    unittest.main()
