"""src/runtime_logdrain.c: log lines reach a reader that stops for a while, and the game thread is not held up."""
from paths import build_hint, test_exe
import os
import re
import subprocess
import time
import unittest

EXE = test_exe('logdrain-test')


def run(lines, stall_s, async_on, kill=False, width=None, mode=None):
    env = dict(os.environ, BB_LOG_ASYNC='1' if async_on else '0')
    if not async_on:
        env['BB_LOG_RAW_IPS'] = '1'  # the address mask keeps a drain thread; direct writes need it off too
    p = subprocess.Popen([str(EXE.resolve()), str(lines)] + (([mode or 'kill'] if kill else ['go']) + [str(width)] if (kill or width) else []), stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
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

    def test_fatal_stop_flushes_the_last_line(self):
        code, data, ms = run(self.N, 1, True, kill=True)
        self.assertEqual(code, 23)
        self.assertTrue(data.rstrip().endswith(b'LAST LINE before the fatal stop'))

    def test_lines_after_an_idle_pause_arrive_and_flush(self):
        # the drain polls every 10 ms after 0.1 s idle; the burst after a 1 s pause and the last line still arrive
        code, data, ms = run(2000, 0, True, kill=True, mode='idle')
        self.assertEqual(code, 23)
        self.assertEqual(data.count(b'line 0'), 2000)
        self.assertTrue(data.rstrip().endswith(b'LAST LINE before the fatal stop'))

    def test_a_full_pipe_drops_lines_instead_of_stalling(self):
        n = 8000  # 8000 lines of 1 KiB: over the 4 MiB drain pipe
        code, data, ms = run(n, 12, True, width=1000)
        self.assertEqual(code, 0)
        self.assertLess(ms, 8000)  # the reader sleeps 12 s: a blocked writer would wait for it
        self.assertLess(data.count(b'line 0'), n)
        self.assertIn(b'log lines were dropped', data)

    def test_off_keeps_the_direct_writes(self):
        code, data, ms = run(self.N, 3, False)
        self.assertEqual(code, 0)
        self.assertGreaterEqual(ms, 2500)  # blocked until the reader read
        self.assertEqual(data.count(b'\n'), self.N)


    def test_addresses_are_masked_unless_raw(self):
        masked = (b'send 4 -> 192.168.x.x:9307 vport 1->2\n' b'peer 8.8.x.x:9307 and [2001:db8:x:x:x:x:x:x]:9307 lo 127.0.0.1\n'
                  b'tail 10.8.x.x')
        raw = (b'send 4 -> 192.168.1.20:9307 vport 1->2\n' b'peer 8.8.4.4:9307 and [2001:db8:1:2::5]:9307 lo 127.0.0.1\n'
               b'tail 10.8.0.2')
        for env, want in (({}, masked), ({'BB_LOG_RAW_IPS': '1'}, raw), ({'BB_LOG_ASYNC': '0'}, masked)):
            r = subprocess.run([str(EXE.resolve()), '0', 'ips'], capture_output=True, env=dict(os.environ, **env), timeout=30)
            self.assertEqual(r.stdout.replace(b'\r\n', b'\n'), want.replace(b'\n', b'\n'), env)


if __name__ == '__main__':
    unittest.main()
