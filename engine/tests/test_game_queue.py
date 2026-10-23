"""tools/mp/game_queue.py: the queue file's write stays atomic and survives a busy file on Windows
(WinError 32 / 5 on the rename while another process has game_queue.json open)."""
from paths import ROOT
import importlib.util
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / "tools" / "mp"))
spec = importlib.util.spec_from_file_location("mp_game_queue", ROOT / "tools" / "mp" / "game_queue.py")
gq = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gq)


class QueueFileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.qf = Path(self.tmp.name) / "game_queue.json"
        self.old = gq.QUEUE_FILE
        gq.QUEUE_FILE = self.qf

    def tearDown(self):
        gq.QUEUE_FILE = self.old
        self.tmp.cleanup()

    def test_replace_retried_after_sharing_violations(self):
        real = os.replace
        calls = []

        def flaky(src, dst):
            calls.append(1)
            if len(calls) <= 3:
                raise PermissionError(32, "The process cannot access the file because it is being used")
            return real(src, dst)

        with mock.patch.object(gq.os, "replace", flaky), mock.patch.object(gq, "RETRY_DELAYS", (0,) * 5):
            gq._write_atomic(self.qf, '{"max_clients": 2}')
        self.assertEqual(len(calls), 4)
        self.assertEqual(json.loads(self.qf.read_text()), {"max_clients": 2})
        self.assertEqual(list(Path(self.tmp.name).glob("*.tmp")), [])  # no temp file left behind

    def test_retry_is_bounded(self):
        def always(src, dst):
            raise PermissionError(5, "Access is denied")

        self.qf.write_text('{"max_clients": 2}')
        with mock.patch.object(gq.os, "replace", always), mock.patch.object(gq, "RETRY_DELAYS", (0,) * 3):
            with self.assertRaises(PermissionError):
                gq._write_atomic(self.qf, '{"max_clients": 9}')
        # the old file is untouched (atomic: never half written) and the temp file is gone
        self.assertEqual(json.loads(self.qf.read_text()), {"max_clients": 2})
        self.assertEqual(list(Path(self.tmp.name).glob("*.tmp")), [])

    def test_busy_read_is_not_an_empty_queue(self):
        self.qf.write_text(json.dumps({"max_clients": 2, "holders": [{"pid": 1}], "queue": []}))
        real = Path.read_text
        n = []

        def flaky(p, *a, **k):
            n.append(1)
            if len(n) <= 2:
                raise PermissionError(32, "busy")
            return real(p, *a, **k)

        with mock.patch.object(gq.Path, "read_text", flaky), mock.patch.object(gq, "RETRY_DELAYS", (0,) * 5):
            st = gq._read_state(self.qf)
        self.assertEqual(st["holders"], [{"pid": 1}])
        self.assertEqual(gq._read_state(Path(self.tmp.name) / "missing.json"), {})

    @unittest.skipUnless(os.name == "nt", "Windows sharing violation")
    def test_real_open_handle_on_windows(self):
        """A reader holding the file open (Python's open has no FILE_SHARE_DELETE): the rename fails
        until it closes; the edit under the lock still goes through."""
        self.qf.write_text(json.dumps({"max_clients": 2, "holders": [], "queue": []}))
        f = open(self.qf, "rb")
        threading.Timer(0.4, f.close).start()
        t0 = time.time()
        with gq._locked("game") as st:
            st["queue"].append({"id": "x", "pid": os.getpid(), "count": 1})
        self.assertGreater(time.time() - t0, 0.3)
        self.assertEqual(json.loads(self.qf.read_text())["queue"][0]["id"], "x")
        self.assertFalse(self.qf.with_suffix(".lock").exists())


if __name__ == "__main__":
    unittest.main()
