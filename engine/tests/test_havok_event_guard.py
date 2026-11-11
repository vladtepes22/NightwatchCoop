from paths import build_hint, test_exe
import subprocess
import unittest

EXE = test_exe('havok-event-guard-test')


@unittest.skipUnless(EXE.exists(), build_hint('havok-event-guard-test'))
class HavokEventGuardTests(unittest.TestCase):
    def test_freed_animation_event_recovery(self):
        r = subprocess.run([str(EXE.resolve())], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('havok-event-guard-test: OK', r.stdout)


if __name__ == '__main__':
    unittest.main()
