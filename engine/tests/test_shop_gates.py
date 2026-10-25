"""tools/party/shop_gates.py + flag_tool.py gen_specific: which messenger shop gates reach party guests (0.7.6)."""
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools" / "party"))
import flag_tool as ft  # noqa: E402
import shop_gates as sg  # noqa: E402


def pf(flag, src, rid=1):
    return (flag, src, rid, "")


class ShopGateSync(unittest.TestCase):
    def test_progress_needs_a_shared_category(self):
        self.assertFalse(sg.reaches_guest("progress", 5090, "item_lot_picked"))  # the 0.7.6 bug: C3 has no lot to replay
        self.assertFalse(sg.reaches_guest("progress", 5090, "never_sync"))
        self.assertTrue(sg.reaches_guest("progress", 5090, "world_state"))
        self.assertTrue(sg.reaches_guest("progress", 12101003, "npc_quest"))
        self.assertTrue(sg.reaches_guest("progress", 5910, "boss_defeated"))

    def test_per_player_classes_have_no_requirement(self):
        self.assertTrue(sg.reaches_guest("item", 5053, "item_lot_picked"))
        self.assertTrue(sg.reaches_guest("open", -1, "-"))
        self.assertTrue(sg.reaches_guest("progress", 92905385, "never_sync"))  # chalice runtime groups: out of scope

    def test_generated_rows(self):
        """gen_specific: a lot-less progress gate in the item-lot range becomes world_state, the rest stays."""
        pflags = [
            pf(5090, "ShopLineupParam.qwcId", 200060),                  # progress, no lot -> shared
            pf(5053, "ShopLineupParam.qwcId", 100011), pf(5053, "ItemLotParam.getItemFlagId", 80509010),  # has a lot
            pf(5510, "ShopLineupParam.qwcId", 100020),                  # owns-the-weapon gate
            pf(6500, "ShopLineupParam.qwcId", 100021),                  # personal block
            pf(92905385, "ShopLineupParam.qwcId", 510011),              # chalice runtime group
            pf(70009290, "ShopLineupParam.eventFlag", 200033),          # stock row: purchases per player
        ]
        spec = ft.gen_specific({}, pflags)
        self.assertEqual(spec[5090][0], "world_state")
        self.assertEqual(spec[5053][0], "item_lot_picked")
        self.assertNotIn(5510, spec)
        self.assertNotIn(6500, spec)
        self.assertNotIn(92905385, spec)
        self.assertEqual(spec[70009290][0], "never_sync")


if __name__ == "__main__":
    unittest.main()
