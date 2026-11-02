"""tools/mp/smoke_party.py: the oath rune amounts analysis of phase cvm (0.7.8 covnpc078), synthetic log lines, no game needed."""
from paths import ROOT
import importlib.util
import sys
import unittest

MP = ROOT / "tools" / "mp"
sys.path.insert(0, str(MP))
spec = importlib.util.spec_from_file_location("mp_smoke_cov_amounts", MP / "smoke_party.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


def line(hp, st, fx="Hunter(6120),HunterStamina(180020)", stmax=100, hpmax=700):
    return ("[coop] Party: test: oath: covenant effects on the local player: %s (role 6, map 0x15000000) "
            "vitals HP %d/%d stamina %d/%d" % (fx, hp, hpmax, st, stmax))


class VitalsTests(unittest.TestCase):
    def rows(self, samples, fx="Hunter(6120),HunterStamina(180020)"):
        return [(0.5 * i, line(hp, st, fx)) for i, (hp, st) in enumerate(samples)]

    def test_parses_the_vitals_and_the_effects(self):
        s = smoke.oath_vitals(self.rows([(700, 100), (699, 80)]))
        self.assertEqual([(x[1], x[3], x[4]) for x in s], [(700, 100, 100), (699, 80, 100)])
        self.assertIn("HunterStamina(180020)", s[0][5])

    def test_no_effect_line_is_parsed_too(self):
        s = smoke.oath_vitals([(0.0, "x test: oath: covenant effects on the local player: none (role 0, map 0x15000000) vitals HP 5/7 stamina 1/2")])
        self.assertEqual(s[0][5], "none")
        self.assertEqual(s[0][1:5], (5, 7, 1, 2))

    def test_old_line_without_vitals_is_skipped(self):
        self.assertEqual(smoke.oath_vitals([(0.0, "test: oath: covenant effects on the local player: none (role 0, map 0x15000000)")]), [])

    def test_stamina_regen_slope_after_the_lowest_point(self):
        # drained to 10, then +8 per sample of 0.5 s = 16 per second, full at 100
        st = [100, 60, 10, 10, 18, 26, 34, 42, 50, 58, 66, 74, 82, 90, 100, 100]
        r = smoke.stamina_regen(smoke.oath_vitals(self.rows([(700, s) for s in st])))
        self.assertAlmostEqual(r, 16.0, places=3)

    def test_stamina_regen_none_without_a_rise(self):
        self.assertIsNone(smoke.stamina_regen(smoke.oath_vitals(self.rows([(700, 100)] * 10))))

    def test_hunter_member_is_faster_than_a_baseline_member(self):
        fast = smoke.oath_vitals(self.rows([(700, s) for s in [100, 10, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]]))
        slow = smoke.oath_vitals(self.rows([(700, s) for s in [100, 10, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100]]))
        self.assertGreater(smoke.stamina_regen(fast), smoke.stamina_regen(slow) + 5)

    def test_hp_drift_one_per_two_seconds(self):
        hp = [700 - i // 4 for i in range(80)]  # one HP every 4 samples of 0.5 s = every 2 s
        d = smoke.hp_drift(smoke.oath_vitals(self.rows([(h, 100) for h in hp])), 0, 100)
        self.assertAlmostEqual(d, -0.5, places=1)

    def test_hp_drift_flat_and_short(self):
        self.assertAlmostEqual(smoke.hp_drift(smoke.oath_vitals(self.rows([(700, 100)] * 30)), 0, 100), 0.0)
        self.assertIsNone(smoke.hp_drift(smoke.oath_vitals(self.rows([(700, 100)] * 2)), 0, 100))


if __name__ == "__main__":
    unittest.main()
