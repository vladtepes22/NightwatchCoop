"""The batch placement of new param rows gives exactly the order of placing them one by one."""
import os
import random
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import synth  # noqa: E402,F401  (sets the merger's import path)
from merge_param import insert_all_new, insert_new  # noqa: E402


class InsertOrder(unittest.TestCase):
    def test_same_as_one_by_one(self):
        rng = random.Random(7)
        for _ in range(400):
            # unsorted tables with repeated ids, like the game's 17 unsorted params
            rows = [(rng.randrange(0, 60), b"v", i) for i in range(rng.randrange(0, 25))]
            existing = {}
            for row_id, _, _ in rows:
                existing[row_id] = existing.get(row_id, 0) + 1
            keys = set()
            for _ in range(rng.randrange(0, 15)):
                row_id = rng.randrange(-5, 70)
                keys.add((row_id, existing.get(row_id, 0) + rng.randrange(0, 2)))
            news = [(k, b"n", "t%d_%d" % k) for k in sorted(keys)]
            slow = list(rows)
            for key, body, text in news:
                insert_new(slow, key, body, text)
            fast = list(rows)
            insert_all_new(fast, news)
            self.assertEqual(fast, slow, (rows, news))

    def test_many_new_rows_fast(self):
        rows = [(i * 10, b"v", None) for i in range(5000)]
        news = [((i * 10 + 5, 0), b"n", None) for i in range(65000)]
        start = time.monotonic()
        insert_all_new(rows, news)
        self.assertEqual(len(rows), 70000)
        self.assertLess(time.monotonic() - start, 5)


if __name__ == "__main__":
    unittest.main()
