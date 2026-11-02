"""src/runtime_savecommit.c: the save commit runs in the background; ordering, and what a kill leaves."""
from paths import ROOT
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

EXE = ROOT / 'out' / 'gpu' / 'savecommit-test.exe'


@unittest.skipUnless(EXE.exists(), 'savecommit-test not built: ninja -C out/gpu savecommit-test')
class SaveCommitTests(unittest.TestCase):
    def check(self, path):
        r = subprocess.run([str(EXE.resolve()), '--check', str(path)], capture_output=True, text=True, timeout=20)
        return int(r.stdout.strip())

    def test_ordering_and_waiting(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run([str(EXE.resolve()), d], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_default_runs_inline(self):
        env = {k: v for k, v in os.environ.items() if k != 'BB_SAVE_COMMIT_ASYNC'}
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run([str(EXE.resolve()), '--inline', d], capture_output=True, text=True, timeout=60, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_ini_key_turns_it_on(self):
        env = {k: v for k, v in os.environ.items() if k != 'BB_SAVE_COMMIT_ASYNC'}
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / 'bbport.ini'
            env['BB_CONFIG'] = str(ini)
            for text, code in (('gpl=1\nsave_commit_async=1\n', 0), ('save_commit_async=0\n', 1), ('gpl=1\n', 1)):
                ini.write_text(text)
                r = subprocess.run([str(EXE.resolve()), '--ini-on'], env=env, timeout=20)
                self.assertEqual(r.returncode, code, text)

    def kill_at(self, stage):
        with tempfile.TemporaryDirectory() as d:
            p = subprocess.Popen([str(EXE.resolve()), '--child', stage, d])
            try:
                marker = Path(d) / 'reached'
                for _ in range(200):
                    if marker.exists() or p.poll() is not None:
                        break
                    time.sleep(0.05)
                self.assertTrue(marker.exists(), 'the commit never reached ' + stage)
            finally:
                p.kill()
                p.wait()
            return self.check(Path(d) / 'file.dat')

    def test_kill_before_rename_keeps_the_old_file_whole(self):
        self.assertEqual(self.kill_at('before-rename'), 0xA1)

    def test_kill_after_rename_has_the_new_file_whole(self):
        self.assertEqual(self.kill_at('after-rename'), 0xB2)


if __name__ == '__main__':
    unittest.main()
