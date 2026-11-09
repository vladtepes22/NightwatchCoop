# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/mp/stranger.py: the helpers that decide "admitted" and the loopback-only guard (synthetic data)."""
import importlib.util
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "party"))
spec = importlib.util.spec_from_file_location("stranger", ROOT / "tools" / "mp" / "stranger.py")
st = importlib.util.module_from_spec(spec)
spec.loader.exec_module(st)


class StrangerTest(unittest.TestCase):
    def test_admitted_only_for_welcome_or_encrypted(self):
        sg = st.sg
        self.assertFalse(st.admitted(b""))
        self.assertFalse(st.admitted(st.frame(sg.CHALLENGE, b"\0" * 24)))
        self.assertFalse(st.admitted(st.frame(sg.REJECT, b"\3\0\0")))
        self.assertTrue(st.admitted(st.frame(sg.WELCOME, b"\0" * 8)))
        self.assertTrue(st.admitted(st.frame(sg.CHALLENGE, b"\0" * 24) + st.frame(sg.ENCRYPTED, b"x" * 20)))
        self.assertFalse(st.admitted(struct.pack("<IB", 0xFFFFFFFF, 1)))  # absurd length: stop parsing

    def test_reject_text(self):
        sg = st.sg
        body = bytes([3]) + struct.pack("<H", 4) + b"nope"
        self.assertEqual(st.reject_text(st.frame(sg.REJECT, body)), "auth: nope")

    def test_refuses_a_remote_address(self):
        with self.assertRaises(SystemExit):
            st.main(["--host", "203.0.113.5", "--port", "9307"])


if __name__ == "__main__":
    unittest.main()
