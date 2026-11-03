"""Files written whole or not at all (scripts/safe_write.py, launcher.py update_ini / write_whole) and the
save swaps that must never leave the player without a save folder (scripts/party_saves.py,
scripts/saveedit/store.py restore_backup). Ported idea: Supermedo's launcher safe writes (72097e6e, f66abc20,
c021f461), written for this tree. Synthetic data only."""
from paths import ROOT
import importlib.util
import os
import stat
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import party_saves as ps
import safe_write
from saveedit import store

SAVE = "1/CUSA03173/SPRJ0005"


class SafeWriteTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_writes_text_and_bytes_whole(self):
        path = self.dir / "a.ini"
        safe_write.write_text(path, "x=1\n")
        self.assertEqual(path.read_text(), "x=1\n")  # line ends as write_text gave them
        safe_write.write_text(path, "x=2\r\n", newline="")
        self.assertEqual(path.read_bytes(), b"x=2\r\n")
        safe_write.write_bytes(path, b"\x00\x01")
        self.assertEqual(path.read_bytes(), b"\x00\x01")
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ["a.ini"])

    def test_failed_replace_keeps_the_old_file_and_removes_the_temporary(self):
        path = self.dir / "a.ini"
        path.write_text("old\n")
        with mock.patch("safe_write.os.replace", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                safe_write.write_text(path, "new\n")
        self.assertEqual(path.read_text(), "old\n")
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ["a.ini"])

    def test_failed_write_keeps_the_old_file(self):
        path = self.dir / "a.ini"
        path.write_text("old\n")
        with mock.patch("safe_write.os.fsync", side_effect=OSError("no space")):
            with self.assertRaises(OSError):
                safe_write.write_text(path, "new\n")
        self.assertEqual(path.read_text(), "old\n")
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ["a.ini"])

    def test_a_file_held_for_a_moment_is_tried_again(self):
        path = self.dir / "a.ini"
        real, calls = os.replace, []

        def flaky(source, target):
            calls.append(1)
            if len(calls) < 3:
                raise PermissionError("held")
            real(source, target)

        with mock.patch("safe_write.os.replace", flaky), mock.patch("safe_write.time.sleep"):
            safe_write.write_text(path, "ok\n")
        self.assertEqual((len(calls), path.read_text()), (3, "ok\n"))

    def test_a_file_held_for_good_raises_after_the_tries(self):
        path = self.dir / "a.ini"
        path.write_text("old\n")
        with mock.patch("safe_write.os.replace", side_effect=PermissionError("held")), \
                mock.patch("safe_write.time.sleep"):
            with self.assertRaises(PermissionError):
                safe_write.write_text(path, "new\n")
        self.assertEqual(path.read_text(), "old\n")
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ["a.ini"])

    @unittest.skipUnless(os.name == "nt", "a read-only file refuses os.replace on Windows only")
    def test_read_only_file_stays_and_leaves_no_temporary(self):
        path = self.dir / "a.ini"
        path.write_text("old\n")
        os.chmod(path, stat.S_IREAD)
        try:
            with mock.patch("safe_write.time.sleep"):
                with self.assertRaises(OSError):
                    safe_write.write_text(path, "new\n")
            self.assertEqual(path.read_text(), "old\n")
            self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ["a.ini"])
        finally:
            os.chmod(path, stat.S_IWRITE | stat.S_IREAD)


class LauncherIniTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            spec = importlib.util.spec_from_file_location("bb_launcher_safe_writes", ROOT / "launcher.py")
            cls.L = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.L)
        except Exception as ex:  # no tkinter here
            raise unittest.SkipTest(f"launcher.py not importable here: {ex}")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_update_ini_keeps_other_lines_and_leaves_no_temporary(self):
        ini = self.dir / "bbport.ini"
        ini.write_text("# mine\nkey.jump=Space\nsharpen=1\n", encoding="utf-8")
        self.L.update_ini(ini, {"sharpen": "0", "live_fps": "1"})
        self.assertEqual(ini.read_text(encoding="utf-8").splitlines(),
                         ["# mine", "key.jump=Space", "sharpen=0", "live_fps=1"])
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ["bbport.ini"])

    def test_update_ini_that_cannot_replace_keeps_the_old_file_clean(self):
        ini = self.dir / "bbport.ini"
        ini.write_text("sharpen=1\n", encoding="utf-8")
        with mock.patch("safe_write.os.replace", side_effect=PermissionError("locked")), \
                mock.patch("safe_write.time.sleep"):
            with self.assertRaises(OSError):
                self.L.update_ini(ini, {"sharpen": "0"})
        self.assertEqual(ini.read_text(encoding="utf-8"), "sharpen=1\n")
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ["bbport.ini"])

    def test_write_whole_is_used_for_the_settings_json(self):
        target = self.dir / "launcher_settings.json"
        target.write_text("{}", encoding="utf-8")
        with mock.patch("safe_write.os.replace", side_effect=OSError("full")):
            with self.assertRaises(OSError):
                self.L.write_whole(target, '{"fps": "60"}')
        self.assertEqual(target.read_text(encoding="utf-8"), "{}")
        self.L.write_whole(target, '{"fps": "60"}')
        self.assertEqual(target.read_text(encoding="utf-8"), '{"fps": "60"}')


def put(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class SaveSwapTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.user = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def real_rename(self):
        return Path.rename

    def test_party_copy_that_cannot_rename_in_keeps_the_party_save(self):
        put(self.user / ps.SOLO / SAVE / "userdata0000", b"solo" * 100)
        put(self.user / ps.PARTY / SAVE / "userdata0000", b"party progress")
        real = Path.rename

        def refuse_copy_in(self_, target):
            if self_.name == ps.PARTY + ".copying":
                raise PermissionError("held")
            return real(self_, target)

        with mock.patch.object(Path, "rename", refuse_copy_in):
            with self.assertRaises(OSError):
                ps.copy_solo_to_party(self.user, replace=True)
        self.assertEqual((self.user / ps.PARTY / SAVE / "userdata0000").read_bytes(), b"party progress")
        self.assertFalse((self.user / (ps.PARTY + ".copying")).exists())
        self.assertFalse((self.user / (ps.PARTY + ".replaced")).exists())

    def test_party_copy_replaces_and_leaves_no_side_folders(self):
        put(self.user / ps.SOLO / SAVE / "userdata0000", b"solo" * 100)
        put(self.user / ps.PARTY / SAVE / "userdata0000", b"party progress")
        backup = ps.copy_solo_to_party(self.user, replace=True)
        self.assertEqual((backup / SAVE / "userdata0000").read_bytes(), b"party progress")
        self.assertEqual((self.user / ps.PARTY / SAVE / "userdata0000").read_bytes(), b"solo" * 100)
        self.assertFalse((self.user / (ps.PARTY + ".replaced")).exists())

    def test_editor_restore_that_cannot_move_in_puts_the_folder_back(self):
        folder = self.user / store.KINDS["solo"]
        put(folder / SAVE / "userdata0000", b"live character")
        put(self.user / store.BACKUPS / "x-before-edit" / SAVE / "userdata0000", b"older character")
        backup = self.user / store.BACKUPS / "x-before-edit"
        real = os.replace

        def refuse_staging(source, target):
            if str(source).endswith(".restore"):
                raise PermissionError("held")
            return real(source, target)

        with mock.patch("saveedit.store.os.replace", refuse_staging):
            with self.assertRaises(OSError):
                store.restore_backup(self.user, "solo", backup, None)
        self.assertEqual((folder / SAVE / "userdata0000").read_bytes(), b"live character")
        self.assertFalse(folder.with_name(folder.name + ".restore").exists())
        self.assertFalse(folder.with_name(folder.name + ".replaced").exists())

    def test_editor_restore_puts_the_backup_in(self):
        folder = self.user / store.KINDS["solo"]
        put(folder / SAVE / "userdata0000", b"live character")
        backup = self.user / store.BACKUPS / "x-before-edit"
        put(backup / SAVE / "userdata0000", b"older character")
        store.restore_backup(self.user, "solo", backup, None)
        self.assertEqual((folder / SAVE / "userdata0000").read_bytes(), b"older character")


if __name__ == "__main__":
    unittest.main()
