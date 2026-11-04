"""src/runtime_logdrain.c: log lines reach a reader that stops for a while, and the game thread is not held up."""
from paths import build_hint, test_exe
import os
import re
import subprocess
import time
import unittest

EXE = test_exe('logdrain-test')


def run(lines, stall_s, async_on):
    env = dict(os.environ, BB_LOG_ASYNC='1' if async_on else '0')
    p = subprocess.Popen([str(EXE.resolve()), str(lines)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
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


@unittest.skipUnless(EXE.exists(), build_hint('logdrain-test'))
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


if __name__ == '__main__':
    unittest.main()
