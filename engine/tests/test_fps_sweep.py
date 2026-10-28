"""Frame-rate sweep of the party harness (tools/mp/instances.py --fps, tools/mp/smoke_party.py --fps): no game needed."""
from paths import ROOT
import importlib.util
import sys
import unittest

MP = ROOT / "tools" / "mp"
sys.path.insert(0, str(MP))


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, MP / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


instances = load("mp_instances_fps", "instances.py")
smoke = load("mp_smoke_fps", "smoke_party.py")


class ParseFpsTests(unittest.TestCase):
    def test_single_value_for_every_instance(self):
        self.assertEqual(instances.parse_fps("60", 3), ["60", "60", "60"])

    def test_host_guest_mix(self):
        self.assertEqual(instances.parse_fps("60/30", 3), ["60", "30", "30"])

    def test_per_instance_last_repeats(self):
        self.assertEqual(instances.parse_fps("60/30/90", 4), ["60", "30", "90", "90"])
        self.assertEqual(instances.parse_fps("60/30/90", 2), ["60", "30"])

    def test_uncap_and_bad_values(self):
        self.assertEqual(instances.parse_fps("30/UNCAP", 2), ["30", "uncap"])
        for bad in ("", "abc", "60/x", "0", "-5"):
            with self.assertRaises(ValueError):
                instances.parse_fps(bad, 2)

    def test_sweep_entries(self):
        self.assertEqual(smoke.parse_fps_sweep("30, 60,90,60/30"), ["30", "60", "90", "60/30"])
        with self.assertRaises(ValueError):
            smoke.parse_fps_sweep("30,abc")
        with self.assertRaises(ValueError):
            smoke.parse_fps_sweep(" , ")


class SweepNamingTests(unittest.TestCase):
    def test_dir_names(self):
        self.assertEqual(smoke.fps_dir_name("60"), "fps60")
        self.assertEqual(smoke.fps_dir_name("60/30"), "fps60x30")
        self.assertEqual(smoke.fps_dir_name("60/30/90"), "fps60x30x90")

    def test_child_args_drop_fps_and_out(self):
        argv = ["--only", "elevator", "--fps", "30,60", "--out", "X", "--every", "5", "--fps=1", "--out=Y"]
        self.assertEqual(smoke.sweep_args(argv), ["--only", "elevator", "--every", "5"])

    def test_physics_groups_exist(self):
        for g in smoke.PHYSICS_GROUPS:
            self.assertIn(g, smoke.GROUPS)
        for g in ("lifts", "elevator", "ladder", "fall", "frontier_fall"):
            self.assertIn(g, smoke.PHYSICS_GROUPS)


class PhaseFpsTests(unittest.TestCase):
    def test_no_sweep_keeps_phase_key(self):
        self.assertEqual(smoke.phase_fps({"fps": 60}), "60")
        self.assertIsNone(smoke.phase_fps({}))

    def test_sweep_overrides_unless_fixed(self):
        self.assertEqual(smoke.phase_fps({"fps": 60}, "30"), "30")
        self.assertEqual(smoke.phase_fps({}, "60/30"), "60/30")
        self.assertEqual(smoke.phase_fps({"fps": 60, "fps_fixed": True}, "30"), "60")


class SummaryTests(unittest.TestCase):
    def test_sensitive_detection(self):
        results = {"30": {"el": [5, 0], "lf": [4, 0], "fl": [3, 1]},
                   "60": {"el": [5, 0], "lf": [2, 2], "fl": [3, 1]},
                   "60/30": {"el": [4, 1], "lf": [4, 0], "fl": [3, 1]}}
        text, sens = smoke.fps_summary(results, ["30", "60", "60/30"])
        self.assertEqual(sens, ["el", "lf"])  # fl fails everywhere: broken, not frame-rate sensitive
        self.assertIn("FPS-sensitive (pass at some rates, fail at others): el, lf", text)
        self.assertIn("PASS 5/5", text)
        self.assertIn("FAIL 2/4", text)

    def test_all_pass_none_sensitive(self):
        text, sens = smoke.fps_summary({"30": {"el": [5, 0]}, "60": {"el": [5, 0]}}, ["30", "60"])
        self.assertEqual(sens, [])
        self.assertIn("FPS-sensitive (pass at some rates, fail at others): none", text)

    def test_missing_phase_in_a_rate_is_a_dash(self):
        text, sens = smoke.fps_summary({"30": {"el": [5, 0]}, "60": {}}, ["30", "60"])
        self.assertEqual(sens, [])
        self.assertIn("-", text.splitlines()[1])


class FrameGenerationTests(unittest.TestCase):
    def test_suffix_split(self):
        self.assertEqual(instances.split_fg("60fg"), ("60", "fsr"))
        self.assertEqual(instances.split_fg("120FG"), ("120", "fsr"))
        self.assertEqual(instances.split_fg("60fgdlss"), ("60", "dlss"))
        self.assertEqual(instances.split_fg("uncapfg"), ("uncap", "fsr"))
        self.assertEqual(instances.split_fg("90"), ("90", None))

    def test_parse_fps_strips_fg(self):
        self.assertEqual(instances.parse_fps("60fg", 2), ["60", "60"])
        self.assertEqual(instances.parse_fps("60fg/30", 3), ["60", "30", "30"])
        self.assertEqual(instances.parse_fps("uncapfg", 1), ["uncap"])
        for bad in ("fg", "xfg", "60fx", "0fg"):
            with self.assertRaises(ValueError):
                instances.parse_fps(bad, 2)

    def test_parse_fg_per_instance(self):
        self.assertEqual(instances.parse_fg("60fg", 2), ["fsr", "fsr"])
        self.assertEqual(instances.parse_fg("60fg/30", 3), ["fsr", None, None])
        self.assertEqual(instances.parse_fg("30/60fgdlss", 3), [None, "dlss", "dlss"])
        self.assertEqual(instances.parse_fg("120", 2), [None, None])

    def test_env(self):
        e = instances.fps_env("60", "fsr")
        self.assertEqual((e["BB_FPS"], e["BB_FRAME_GENERATION"], e["BB_FG_BACKEND"], e["BB_FG_TIMING"]),
                         ("60", "2x", "fsr", "1"))
        e = instances.fps_env("120", None)
        # 120 is no patch preset: uncapped with the launcher's frame rate limit (as players set it)
        self.assertEqual((e["BB_FPS"], e["BB_FPS_LIMIT"], e["BB_FRAME_GENERATION"]), ("uncap", "120", "off"))
        self.assertNotIn("BB_FPS_LIMIT", instances.fps_env("90", None))
        self.assertNotIn("BB_FG_BACKEND", e)
        self.assertEqual(instances.fps_env("60", "fsr", headless=True)["BB_FRAME_GENERATION"], "off")

    def test_sweep_entries_and_dirs(self):
        self.assertEqual(smoke.parse_fps_sweep("30,60,90,120,60fg,60/30"), ["30", "60", "90", "120", "60fg", "60/30"])
        self.assertEqual(smoke.fps_dir_name("60fg"), "fps60fg")
        self.assertEqual(smoke.fps_dir_name("60fg/30"), "fps60fgx30")

    def test_measured_rates(self):
        lines = ["Frame generation: FSR 2x on (presented frames per rendered frame; presentation queue independent)"]
        for k in range(8):
            lines.append("[+ %d.0] Frame rate: %d.0 s real, 600 steps (%.1f per s), game time 1.000 s per real s, "
                         "last dt 0.01667 s; effects time 1.000 s per real s" % (k, 10 * k, 59.5 + (k % 2)))
            lines.append("FG pacing: source %.2f presented %.2f generated 60 skipped 0" % (60.0, 119.0 + k % 2))
        r = smoke.measured_rates(lines)
        self.assertEqual(r["fg"], "FSR 2x on")
        self.assertAlmostEqual(r["steps"], 60.5)
        self.assertEqual(r["source"], 60.0)
        self.assertGreater(r["presented"], 118)
        r = smoke.measured_rates(["Frame generation: FSR unavailable (no derivatives); presenting normally"])
        self.assertEqual(r["fg"], "FSR unavailable")
        self.assertIsNone(r["steps"])

    def test_measured_summary_flags_fg_not_running(self):
        import json, tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            ph = Path(d) / "el"
            ph.mkdir()
            (ph / "instances.json").write_text(json.dumps({"count": 2, "headless": 0}))
            (ph / "inst0.log").write_text("Frame generation: FSR 2x on (x)\n"
                                          "FG pacing: source 60.00 presented 119.50 generated 1 skipped 0\n")
            (ph / "inst1.log").write_text("Frame generation: FSR unavailable (no); presenting normally\n")
            lines, problems = smoke.measured_summary(d, "60fg")
            self.assertIn("frame generation FSR 2x on (rendered 60.0, presented 119.5 per s)", lines[0])
            self.assertEqual(len(problems), 1)
            self.assertIn("inst1", problems[0])
            # the same logs at a plain rate ask for nothing
            self.assertEqual(smoke.measured_summary(d, "60")[1], [])
            # a headless instance runs no frame generation: not a problem
            (ph / "instances.json").write_text(json.dumps({"count": 2, "headless": 1}))
            self.assertEqual(smoke.measured_summary(d, "60fg")[1], [])


class MaidDressTests(unittest.TestCase):
    """The md phase's frame analyzer check (Cainhurst maid c2150: dress and cloth meshes drawn with her body)."""
    REC = ("presented buffer 0x1\n\nmesh draws (BB_CAPTURE_MESH_COUNTS): 4\n"
           "  mesh pass #2 draw 0: indices 12027 instances 1 vs 0000000000000001 ps 0000000000000002\n"
           "    vertex stream 0 at 0x10 size 3000 stride 20: 150 elements, NaN 0, box (0.000 0.000 0.000) .. (0.000 0.000 0.000), gpu-written yes, cpu-modified no\n"
           "  mesh pass #2 draw 1: indices 14805 instances 1 vs 0000000000000001 ps 0000000000000002\n"
           "  mesh pass #2 draw 2: indices 657 instances 1 vs 0000000000000003 ps 0000000000000002\n"
           "    vertex stream 0 at 0x20 size 3000 stride 20: 150 elements, NaN 0, box (-1.000 0.000 0.000) .. (1.000 2.000 0.500), gpu-written no, cpu-modified no\n"
           "    vertex stream 1 at 0x30 size 1717128 stride 20: 85856 elements, NaN 3, box (0.000 0.000 0.000) .. (0.000 0.000 0.000)\n"
           "  mesh pass #2 draw 3: indices 534 instances 1 vs 0000000000000003 ps 0000000000000002\n")

    def test_record(self):
        draws, cloth = smoke.md_record(self.REC)
        self.assertEqual(draws, {("body", 1): 1, ("dress", 1): 1, ("BD_cloth", 1): 1, ("AM_cloth", 1): 1})
        self.assertEqual(cloth, [(150, True, True), (150, False, False)])  # the shared heap stream is not a cloth stream

    def test_missing(self):
        draws, _ = smoke.md_record(self.REC)
        self.assertEqual(smoke.md_missing(draws), [("BD_cloth_e", 1, 0, 1)])
        draws[("BD_cloth_e", 1)] = 1
        self.assertEqual(smoke.md_missing(draws), [])
        draws[("body", 1)] = 2  # a second maid drawn without her dress
        self.assertIn(("dress", 1, 1, 2), smoke.md_missing(draws))

    def test_md_is_a_physics_group(self):
        self.assertIn("cainhurst_maid", smoke.PHYSICS_GROUPS)
        self.assertEqual(smoke.GROUPS["cainhurst_maid"], "md")


if __name__ == "__main__":
    unittest.main()
