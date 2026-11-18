"""A game with its update kept beside it (CUSAxxxxx-patch) reads, and hashes, like one with the update copied in: the
engine lays the update over the game, so the generator must read through it too (6 Nov: a guest whose update sat beside
the game built its world from 19 unpatched files and never matched the host)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from gamedata import GameFiles  # noqa: E402


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


if __name__ == "__main__":
    unittest.main()
