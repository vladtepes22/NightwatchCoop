"""src/runtime_logdrain.c: log lines reach a reader that stops for a while, and the game thread is not held up."""
from paths import ROOT
import os
import re
import subprocess
import time
import unittest

EXE = ROOT / 'out' / 'gpu' / 'logdrain-test.exe'  # ninja -C out/gpu logdrain-test


def run(lines, stall_s, async_on, mode=None):
    env = dict(os.environ, BB_LOG_ASYNC='1' if async_on else '0')
    p = subprocess.Popen([str(EXE.resolve()), str(lines)] + ([mode] if mode else []), stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    err = []
    import threading
    t = threading.Thread(target=lambda: err.append(p.stderr.read()), daemon=True)
    t.start()
    time.sleep(stall_s)  # a reader that does not read
    data = p.stdout.read()
    p.wait(timeout=30)
    t.join(10)
    ms = int(re.search(rb'printed in (\d+) ms', err[0]).group(1))
    return p.returncode, data, ms


@unittest.skipUnless(EXE.exists(), 'out/gpu/logdrain-test.exe not built: ninja -C out/gpu logdrain-test')
class LogDrainTests(unittest.TestCase):
    N = 20000  # about 2 MiB, under the drain pipe, over the 4 KiB OS pipe

    def test_printing_does_not_wait_for_the_reader(self):
        code, data, ms = run(self.N, 3, True)
        self.assertEqual(code, 0)
        self.assertLess(ms, 2000)
        self.assertEqual(data.count(b'\n'), self.N)
        self.assertTrue(data.startswith(b'line 000000 '))
        self.assertIn(b'line %06d ' % (self.N - 1), data)

    def test_off_keeps_the_direct_writes(self):
        code, data, ms = run(self.N, 3, False)
        self.assertEqual(code, 0)
        self.assertGreaterEqual(ms, 2500)  # blocked until the reader read
        self.assertEqual(data.count(b'\n'), self.N)

    def test_a_fatal_stop_hands_the_last_lines_over(self):
        # assert_fail_impl, raise_exception, hard_exit, TerminateProcessNow: flush, then _exit (no atexit)
        code, data, ms = run(self.N, 1, True, 'stop')
        self.assertEqual(code, 23)
        self.assertEqual(data.count(b'\n'), self.N + 1)
        self.assertTrue(data.endswith(b'last line before the stop\n'))

    def test_a_full_pipe_drops_lines_instead_of_waiting(self):
        lines = 7000  # 1 KiB lines, about 7 MiB: over the 4 MiB drain pipe while the reader does not read
        code, data, ms = run(lines, 3, True, 'tail')
        self.assertEqual(code, 0)
        self.assertLess(ms, 2000)
        self.assertTrue(data.startswith(b'line 000000 '))
        self.assertLess(data.count(b'\n'), lines)
        self.assertTrue(data.endswith(b'tail line\n'))  # and the log keeps working afterwards


if __name__ == '__main__':
    unittest.main()
