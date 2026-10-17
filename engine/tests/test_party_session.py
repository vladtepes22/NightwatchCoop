"""scripts/party_session.py: Undo last party session and the check after a session (save safety)."""
import paths  # noqa: F401  (puts scripts/ on sys.path)
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import party_session as ps

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools" / "party"))
import flag_tool as ft  # noqa: E402
import save_decode as sd  # noqa: E402

SAVE = "1/CUSA03173/SPRJ0005"
TEST_SAVE = Path(r"E:\games\bbport\test_saves\cathedral_day\savedata\1\CUSA00207\SPRJ0005\userdata0000")


def synthetic_slot(ng=0, ending=0, play_ms=3_600_000, name="Hunter", level=40, flags=()):
    """A character slot with the save header the game writes (version 0x41, NG block, flag section)."""
    d = bytearray(ps.SLOT_SIZE)
    ng_off, pool = 0x1000, 0x2000
    struct.pack_into("<III", d, 0, ps.SAVE_VERSION, 0x18000000, play_ms)
    struct.pack_into("<IIII", d, 0x2C, ng_off, 8, pool, ps.POOL_BYTES + 1)
    struct.pack_into("<II", d, ng_off, ng, ending)
    n = 0x400
    struct.pack_into("<I", d, n - 0x18, level)
    d[n:n + 2 * len(name) + 2] = name.encode("utf-16le") + b"\0\0"
    s = ps.CharacterSave(bytes(d))
    for f in flags:
        set_flag(d, s.pool, f, True)
    return bytes(d)


def set_flag(d, pool, fid, on):
    k = ps._group_key(fid)
    i = pool + k * 125 + (fid % 1000) // 8
    bit = 0x80 >> (fid % 8)
    d[i] = (d[i] | bit) if on else (d[i] & ~bit)


def write(path: Path, data: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def tree(folder: Path) -> dict:
    return {p.relative_to(folder).as_posix(): p.read_bytes() for p in folder.rglob("*") if p.is_file()}


class PartySessionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.user = Path(self.tmp.name)
        self.slot0 = synthetic_slot(flags=(12401800, 72400200, 72400201))
        write(self.user / "savedata_party" / SAVE / "userdata0000", self.slot0)
        write(self.user / "savedata_party" / SAVE / "userdata0001", bytes(ps.SLOT_SIZE))
        write(self.user / "savedata_party" / SAVE / "userdata0010", b"\3" * ps.SYSTEM_SIZE)
        write(self.user / "savedata_party" / (SAVE + ".sce_sys") / "param.bin", b"P" * ps.PARAM_SIZE)
        write(self.user / "savedata" / SAVE / "userdata0000", synthetic_slot(name="Solo"))

    def tearDown(self):
        self.tmp.cleanup()

    def start_session(self, role="host"):
        """What the game does at a party start (runtime_savepolicy_session_snapshot): same layout."""
        path = ps.snapshot_now(self.user, kind="session-start", role=role)
        return ps.last_session(self.user), path

    def test_snapshot_layout_and_label(self):
        s, path = self.start_session("guest")
        self.assertEqual(s.path, path)
        self.assertEqual(s.role, "guest")
        self.assertIn("start of a party session as guest", s.label)
        self.assertEqual(tree(path / "savedata_party"), tree(self.user / "savedata_party"))
        self.assertEqual(tree(path / "savedata"), tree(self.user / "savedata"))

    def test_completed_session_records_success_and_damage(self):
        self.start_session()
        self.assertIsNotNone(ps.needs_check(self.user))
        self.assertEqual(ps.check_completed_session(self.user), [])
        self.assertIsNone(ps.needs_check(self.user))
        self.start_session()
        write(self.user / "savedata_party" / SAVE / "userdata0000", b"broken")
        self.assertTrue(ps.check_completed_session(self.user))
        self.assertIsNone(ps.needs_check(self.user))
        report = ps.last_session(self.user).path / ps.CHECKED
        self.assertIn("result=problems", report.read_text())

    def test_undo_restores_byte_identical_and_is_undoable(self):
        before_party = tree(self.user / "savedata_party")
        before_solo = tree(self.user / "savedata")
        s, _ = self.start_session()
        # The session: the character progresses, a second character is made, a stray file appears.
        d = bytearray(synthetic_slot(play_ms=7_200_000, level=45, flags=(12401800, 72400200, 72400201, 13201800)))
        write(self.user / "savedata_party" / SAVE / "userdata0000", bytes(d))
        write(self.user / "savedata_party" / SAVE / "userdata0001", synthetic_slot(name="Second"))
        write(self.user / "savedata_party" / SAVE / "extra.tmp", b"x")
        after = tree(self.user / "savedata_party")
        lines = ps.describe(self.user)
        self.assertTrue(any("savedata_party: 3 files differ" in ln for ln in lines), lines)
        self.assertTrue(any("slot 1: now Hunter level 45, 2 h 00 min played -> back to Hunter level 40, 1 h 00 min" in ln
                            for ln in lines), lines)
        self.assertTrue(any("slot 2: now Second" in ln and "back to empty" in ln for ln in lines), lines)
        self.assertIn("savedata: unchanged since then", lines)
        restored, backup = ps.undo(self.user)
        self.assertEqual(restored.path, s.path)
        self.assertEqual(tree(self.user / "savedata_party"), before_party)
        self.assertEqual(tree(self.user / "savedata"), before_solo)
        self.assertFalse(any(p.name.endswith((".undo", ".undo-old")) for p in self.user.iterdir()))
        # The undo is itself undoable: the newest snapshot is the state before it.
        last = ps.last_session(self.user)
        self.assertEqual(last.path, backup)
        self.assertEqual(last.kind, "before-undo")
        self.assertIn("before an undo", last.label)
        self.assertIsNone(ps.needs_check(self.user))
        ps.undo(self.user)
        self.assertEqual(tree(self.user / "savedata_party"), after)

    def test_launcher_snapshots_are_pruned(self):
        for _ in range(ps.KEEP + 3):
            ps.snapshot_now(self.user, kind="before-undo")
        self.assertEqual(len(ps.sessions(self.user)), ps.KEEP)

    def test_undo_without_snapshot(self):
        with self.assertRaises(FileNotFoundError):
            ps.undo(self.user)
        self.assertEqual(ps.describe(self.user), [])

    def test_undo_of_a_first_session_empties_the_party_folder(self):
        import shutil
        shutil.rmtree(self.user / "savedata_party")
        self.start_session()
        write(self.user / "savedata_party" / SAVE / "userdata0000", self.slot0)
        ps.undo(self.user)
        self.assertEqual(tree(self.user / "savedata_party"), {})

    def test_check_clean_session(self):
        self.start_session()
        write(self.user / "savedata_party" / SAVE / "userdata0000",
              synthetic_slot(play_ms=4_000_000, level=41, flags=(12401800, 72400200, 72400201, 12411800)))
        s = ps.needs_check(self.user)
        self.assertIsNotNone(s)
        self.assertEqual(ps.check(self.user, s), [])
        ps.mark_checked(self.user, s, [])
        self.assertIsNone(ps.needs_check(self.user))

    def test_check_finds_damage_and_lost_progress(self):
        self.start_session()
        save = self.user / "savedata_party" / SAVE
        # lost boss + lamp, level and play time down
        write(save / "userdata0000", synthetic_slot(play_ms=1_000_000, level=30, flags=(72400200,)))
        problems = ps.check(self.user)
        text = "\n".join(m for _, m in problems)
        self.assertIn("1 boss kill from before the session are undone (flags 12401800)", text)
        self.assertIn("1 lamp lit before the session are unlit again (flags 72400201)", text)
        self.assertIn("level went down from 40 to 30", text)
        self.assertIn("play time went back", text)
        self.assertTrue(all(level == "error" for level, _ in problems))
        # unreadable save, damaged game data, vanished character
        write(save / "userdata0000", b"\1" * 1000)
        write(save / "userdata0010", bytes(ps.SYSTEM_SIZE))
        text = "\n".join(m for _, m in ps.check(self.user))
        self.assertIn("slot 1: the save does not load", text)
        self.assertIn("game data file is damaged", text)
        write(save / "userdata0000", bytes(ps.SLOT_SIZE))
        self.assertIn("slot 1: the character that was there before the session is gone",
                      "\n".join(m for _, m in ps.check(self.user)))

    def test_check_ng_cycle(self):
        self.start_session()
        save = self.user / "savedata_party" / SAVE / "userdata0000"
        # an ending played: NG+1 with the ending type / flags is fine, and the old cycle's bosses are gone
        write(save, synthetic_slot(ng=1, ending=2, play_ms=4_000_000, flags=(6601, 6603)))
        self.assertEqual(ps.check(self.user), [])
        # NG+1 without an ending: asked about (warning)
        write(save, synthetic_slot(ng=1, play_ms=4_000_000, flags=(12401800, 72400200, 72400201)))
        self.assertEqual([lv for lv, _ in ps.check(self.user)], ["warning"])
        # NG+2 in one session, or NG going back: wrong
        write(save, synthetic_slot(ng=2, ending=2, play_ms=4_000_000))
        self.assertIn("NG+0 to NG+2", ps.check(self.user)[0][1])
        self.start_session()
        write(save, synthetic_slot(ng=0, play_ms=5_000_000))
        self.assertIn("went back from NG+2 to NG+0", ps.check(self.user)[0][1])

    def test_new_character_in_a_slot_is_not_compared(self):
        self.start_session()
        write(self.user / "savedata_party" / SAVE / "userdata0000", synthetic_slot(name="Other", level=4, play_ms=10))
        self.assertEqual(ps.check(self.user), [])

    def test_progress_flags_match_the_party_flag_table(self):
        rows = ft.load_inc(REPO / "gpu" / "shim" / "party" / "party_flags.inc")
        for f in ps.BOSS_FLAGS:
            self.assertEqual(ft.categorize(rows, f)[0], "boss_defeated", f)
        for f in ps.LAMP_FLAGS:
            self.assertEqual(ft.categorize(rows, f)[0], "lamp_unlocked", f)
        for f in ps.BOSS_FLAGS + ps.LAMP_FLAGS + ps.ENDING_FLAGS:
            self.assertEqual(ps._group_key(f), sd.group_key(f))

    @unittest.skipUnless(TEST_SAVE.is_file(), "no test save on this PC")
    def test_real_save(self):
        data = TEST_SAVE.read_bytes()
        s = ps.CharacterSave(data)
        ref = sd.Save(data)
        self.assertEqual(s.pool, ref.pool)
        self.assertEqual((s.ng_cycle, s.ending_type), (0, 0))
        self.assertEqual(s.character()["level"], ref.character()["level"])
        for f in ps.BOSS_FLAGS + ps.LAMP_FLAGS:
            self.assertEqual(s.flag(f), ref.flag(f))
        self.assertTrue(s.flag(12401800) and s.flag(72400200))


if __name__ == "__main__":
    unittest.main()
