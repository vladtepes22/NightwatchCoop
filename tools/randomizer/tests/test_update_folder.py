"""A game with its update kept beside it (CUSAxxxxx-patch) reads, and hashes, like one with the update copied in: the
engine lays the update over the game, so the generator must read through it too (6 Nov: a guest whose update sat beside
the game built its world from 19 unpatched files and never matched the host)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from gamedata import GameFiles, OldGame, UPDATE_MARK  # noqa: E402


def _write(folder, rel, data):
    path = os.path.join(folder, "dvdroot_ps4", *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


class UpdateFolder(unittest.TestCase):
    def test_update_beside_the_game_reads_like_one_copied_in(self):
        with tempfile.TemporaryDirectory() as tmp:
            base, update, merged, mod = (os.path.join(tmp, n) for n in ("CUSA03173", "CUSA03173-patch", "merged", "mod"))
            _write(base, "event/m21.emevd.dcx", b"old")
            _write(base, "param/a.bin", b"same")
            _write(update, "event/m21.emevd.dcx", b"patched")
            _write(update, "sfx/new.ffxbnd.dcx", b"only in the update")
            _write(merged, "event/m21.emevd.dcx", b"patched")
            _write(merged, "param/a.bin", b"same")
            _write(merged, "sfx/new.ffxbnd.dcx", b"only in the update")
            _write(mod, "event/m21.emevd.dcx", b"mod")

            beside, copied = GameFiles(base, [], update), GameFiles(merged)
            for files in (beside, copied):
                self.assertEqual(files.read("event/m21.emevd.dcx"), b"patched")
                files.read("param/a.bin")
                self.assertEqual(files.listdir("sfx"), ["new.ffxbnd.dcx"])
                files.read("sfx/new.ffxbnd.dcx")
            self.assertEqual(beside.read_hashes, copied.read_hashes)
            self.assertEqual(beside.read_from, copied.read_from)  # the update's files count as the game's (root 0)
            self.assertEqual(beside.root_input(0), copied.root_input(0))
            self.assertEqual(beside.input_hash(), copied.input_hash())

            # a mod still wins over the update, and counts as the mod's
            with_mod = GameFiles(base, [mod], update)
            self.assertEqual(with_mod.read("event/m21.emevd.dcx"), b"mod")
            self.assertEqual(with_mod.read_from["event/m21.emevd.dcx"], 1)

            # without the update the base is read: the old bytes, another hash
            alone = GameFiles(base)
            self.assertEqual(alone.read("event/m21.emevd.dcx"), b"old")

    def test_a_game_without_its_update_is_refused_plainly(self):
        """NW-bbc8683: a disc game read without its 1.09 files failed deep in an arena edit with a raw ValueError."""
        with tempfile.TemporaryDirectory() as tmp:
            base, update = (os.path.join(tmp, n) for n in ("CUSA00900", "CUSA00900-patch"))
            _write(base, "event/m24_02_00_00.emevd.dcx", b"base")
            _write(update, UPDATE_MARK, b"1.09")
            with self.assertRaises(OldGame) as refused:
                GameFiles(base).check_updated()
            self.assertIn("CUSA00900-patch", str(refused.exception))
            GameFiles(base, [], update).check_updated()  # the update beside the game counts
            _write(base, UPDATE_MARK, b"1.09")
            GameFiles(base).check_updated()  # and copied in


if __name__ == "__main__":
    unittest.main()
