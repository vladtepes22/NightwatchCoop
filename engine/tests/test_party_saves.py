"""scripts/party_saves.py: the launcher's copy of the single-player save into the party save folder."""
import paths  # noqa: F401  (puts scripts/ on sys.path)
import tempfile
import unittest
from pathlib import Path

import party_saves as ps

SAVE = "1/CUSA03173/SPRJ0005"


def write(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def snapshot(folder: Path) -> dict:
    return {p.relative_to(folder).as_posix(): (p.stat().st_mtime_ns, p.read_bytes())
            for p in folder.rglob("*") if p.is_file()}


class PartySaveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.user = Path(self.tmp.name)
        write(self.user / ps.SOLO / SAVE / "userdata0000", b"A" * 4096)
        write(self.user / ps.SOLO / SAVE / "userdata0010", b"B" * 1024)
        write(self.user / ps.SOLO / (SAVE + ".sce_sys") / "param.bin", b"P" * 1328)
        write(self.user / ps.SOLO / SAVE / "userdata0000.3.bbtmp", b"partial")

    def tearDown(self):
        self.tmp.cleanup()

    def test_offer_only_when_party_folder_is_empty(self):
        self.assertTrue(ps.should_offer_copy(self.user))
        (self.user / ps.PARTY).mkdir()
        self.assertTrue(ps.should_offer_copy(self.user))  # an empty folder is no save
        write(self.user / ps.PARTY / SAVE / "userdata0000", b"C")
        self.assertFalse(ps.should_offer_copy(self.user))

    def test_no_offer_without_single_player_save(self):
        empty = self.user / "other"
        empty.mkdir()
        self.assertFalse(ps.should_offer_copy(empty))
        with self.assertRaises(FileNotFoundError):
            ps.copy_solo_to_party(empty)

    def test_copy_reads_solo_only(self):
        before = snapshot(self.user / ps.SOLO)
        self.assertIsNone(ps.copy_solo_to_party(self.user))
        self.assertEqual(snapshot(self.user / ps.SOLO), before)
        party = self.user / ps.PARTY
        self.assertEqual((party / SAVE / "userdata0000").read_bytes(), b"A" * 4096)
        self.assertTrue((party / (SAVE + ".sce_sys") / "param.bin").is_file())
        self.assertFalse((party / SAVE / "userdata0000.3.bbtmp").exists())
        self.assertFalse((self.user / (ps.PARTY + ".copying")).exists())

    def test_existing_party_save_is_kept_or_backed_up(self):
        write(self.user / ps.PARTY / SAVE / "userdata0000", b"party progress")
        with self.assertRaises(FileExistsError):
            ps.copy_solo_to_party(self.user)
        self.assertEqual((self.user / ps.PARTY / SAVE / "userdata0000").read_bytes(), b"party progress")
        backup = ps.copy_solo_to_party(self.user, replace=True)
        self.assertEqual((backup / SAVE / "userdata0000").read_bytes(), b"party progress")
        self.assertEqual(backup.parent, self.user / ps.BACKUPS / ps.PARTY)
        self.assertEqual((self.user / ps.PARTY / SAVE / "userdata0000").read_bytes(), b"A" * 4096)


class LauncherEnvTests(unittest.TestCase):
    def test_party_env_save_policy(self):
        import importlib.util
        from paths import ROOT
        try:
            spec = importlib.util.spec_from_file_location("bb_launcher", ROOT / "launcher.py")
            launcher = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(launcher)
        except Exception as ex:  # no tkinter / display
            self.skipTest(f"launcher.py not importable here: {ex}")
        env = launcher.party_env({"party_mode": "host"})
        self.assertEqual(env["BB_PARTY_SAVE"], "separate")
        # The old shared choice is gone: a party session always plays from the party saves.
        env = launcher.party_env({"party_mode": "join", "party_separate_save": False})
        self.assertEqual(env["BB_PARTY_SAVE"], "separate")
        self.assertIsNone(launcher.party_env({"party_mode": ""})["BB_PARTY_SAVE"])
        self.assertEqual(launcher.party_saves_module().PARTY, "savedata_party")

    def test_party_env_rules_and_sync(self):
        import importlib.util
        from paths import ROOT
        try:
            spec = importlib.util.spec_from_file_location("bb_launcher", ROOT / "launcher.py")
            launcher = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(launcher)
        except Exception as ex:  # no tkinter / display
            self.skipTest(f"launcher.py not importable here: {ex}")
        # Defaults = the game's defaults (docs/party/DEBUG_SWITCHES.md, docs/PARTY.md)
        env = launcher.party_env({"party_mode": "host"})
        expected = {
            "BB_PARTY_START": "prologue_solo", "BB_PARTY_GUEST_INSIGHT": "parity",
            "BB_SAVE_BACKUP_MINUTES": "10", "BB_SAVE_BACKUP_KEEP": "10",
            "BB_PARTY_GRANT_BELLS": "1", "BB_PARTY_OPEN_WORLD": "1", "BB_PARTY_GUEST_RESPAWN": "1",
            "BB_PARTY_GUEST_REFILL": "1", "BB_PARTY_GUEST_MARK": "1", "BB_PARTY_FULL_REWARDS": "1",
            "BB_PARTY_ITEMS": "1", "BB_PARTY_PROGRESS": "1", "BB_PARTY_PROGRESS_NPC": "1",
            "BB_PARTY_STORY": "1", "BB_PARTY_STORY_MIRROR": "1", "BB_PARTY_RESTART": "1",
        }
        for key, value in expected.items():
            self.assertEqual(env[key], value, key)
        env = launcher.party_env({"party_mode": "join", "party_start": "immediate", "party_guest_insight": "0",
                                  "party_backup_minutes": "0", "party_backup_keep": "50",
                                  "party_progress_npc": False, "party_story_mirror": False,
                                  "party_restart": False, "party_guest_mark": True})
        self.assertEqual((env["BB_PARTY_START"], env["BB_PARTY_GUEST_INSIGHT"]), ("immediate", "0"))
        self.assertEqual((env["BB_SAVE_BACKUP_MINUTES"], env["BB_SAVE_BACKUP_KEEP"]), ("0", "50"))
        self.assertEqual((env["BB_PARTY_PROGRESS_NPC"], env["BB_PARTY_STORY_MIRROR"]), ("0", "0"))
        self.assertEqual((env["BB_PARTY_RESTART"], env["BB_PARTY_GUEST_MARK"]), ("0", "1"))
        # Bad values fall back to the defaults
        env = launcher.party_env({"party_mode": "host", "party_start": "x", "party_backup_keep": "7"})
        self.assertEqual((env["BB_PARTY_START"], env["BB_SAVE_BACKUP_KEEP"]), ("prologue_solo", "10"))
        # Party off: every party variable is unset (run.bat sees none of them)
        off = launcher.party_env({"party_mode": "", "party_restart": False})
        self.assertTrue(all(v is None for v in off.values()))
        self.assertIn("BB_PARTY_RESTART", off)
        # run.bat started without the launcher gets the same values through --write-env
        all_env = launcher.settings_env({"party_mode": "host", "party_story": False})
        self.assertEqual(all_env["BB_PARTY_STORY"], "0")
        self.assertEqual(all_env["BB_SKIP_NETWORK_CHOICE"], "online")


if __name__ == "__main__":
    unittest.main()
