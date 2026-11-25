"""Byte-identical round trip of every game file of a merged type (roundtrip.py), when the game is here.

The game folder: BB_QA_GAME, else C:\\Games\\CUSA03173; skipped when it has no dvdroot_ps4. About a minute.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import roundtrip  # noqa: E402

GAME = os.environ.get("BB_QA_GAME") or r"C:\Games\CUSA03173"


@unittest.skipUnless(os.path.isdir(os.path.join(GAME, "dvdroot_ps4")), "no game files at " + GAME)
class GameRoundTrip(unittest.TestCase):
    def test_every_merged_type_writes_back_the_same(self):
        problems, counts, files = roundtrip.run(GAME)
        self.assertEqual(problems, [])
        for kind in ("bnd", "param", "fmg", "tpf", "tae"):
            self.assertGreater(counts.get(kind, 0), 0, kind)
        print("\nround trip: %d files: %s" % (files, ", ".join("%s %d" % kv for kv in sorted(counts.items()))))


if __name__ == "__main__":
    unittest.main()
