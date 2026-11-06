"""The shared-rewards audit, derived at run time from the player's own game folder (no committed table):
scripts/partydata/item_tool.py derive() must find no GAP row, and its sections agree with the audit rows.
Skipped when no game folder is at hand (BB_GAME_DIR, else the agent clone's game/). The C++ side
(party-items-test test_audit) checks that the tables the game loads give every `shared` row a path."""
import os
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "partydata"))


def game_dir():
    for c in (os.environ.get("BB_GAME_DIR"), str(ROOT.parent / "game")):
        if c and (Path(c) / "dvdroot_ps4").is_dir():
            return Path(c) / "dvdroot_ps4"
    return None


GAME = game_dir()


@unittest.skipUnless(GAME, "no game folder (BB_GAME_DIR)")
class ItemAuditTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import item_tool
        cls.sections, cls.audit = item_tool.derive(GAME)

    def rows(self, verdict):
        return [r[0] for r in self.audit if r[5] == verdict]

    def test_no_gap(self):
        self.assertEqual(self.rows("GAP"), [], "a lot gives a key / unique item, weapon or rune and no path shares it")

    def test_sections_match_audit(self):
        self.assertEqual([int(r[0]) for r in self.sections["itemRequireShared"]], self.rows("shared"))
        self.assertEqual([int(r[0]) for r in self.sections["itemPerPlayer"]], self.rows("per-player"))

    def test_owner_cases(self):
        shared = set(self.rows("shared"))
        self.assertIn(2605010, shared)  # Living String (NW-4ae26be)
        self.assertIn(2410900, shared)  # the questline
        self.assertNotIn(2410140, shared)  # an ordinary corpse stays per player
        self.assertEqual(self.sections["itemKillAward"], [["2605010", "26", "2600129", "12600029", "5913"]])

    def test_ledger_indices_stable(self):
        import item_tool
        got = {int(r[1]): int(r[0]) for r in self.sections["itemLedger"]}
        self.assertEqual(got, {lot: i for lot, i in item_tool.LEDGER_LEGACY_INDEX.items()})


if __name__ == "__main__":
    unittest.main()
