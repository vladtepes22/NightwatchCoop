"""tools/party/save_check.py: the H3 save-consistency check (synthetic snapshots and logs)."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "party"))
import flag_tool as ft  # noqa: E402
import save_check as sc  # noqa: E402

BOSS = 12401800  # boss_defeated (party_flags.inc)
BOSS2 = 12401802  # cutscene_seen


def snap(path, label, ms, mode, flags, blocks=(12401,)):
    s = ft.Snapshot()
    s.label, s.unix_ms, s.load_mode = label, ms, mode
    for b in blocks:
        s.blocks[b] = (1, bytes(125))
    for f in flags:
        s.set(f, True)
    path.parent.mkdir(parents=True, exist_ok=True)
    s.write(path)


class SaveCheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.rows = ft.load_inc(ROOT / "gpu" / "shim" / "party" / "party_flags.inc")

    def tearDown(self):
        self.tmp.cleanup()

    def folder(self, name, log=""):
        d = self.root / name
        (d / "user" / "party").mkdir(parents=True, exist_ok=True)
        (d / "run.log").write_text(log, encoding="utf-8")
        return d

    def test_flags_consistent_and_lost(self):
        h, g = self.folder("inst0"), self.folder("inst1")
        snap(h / "user/party/flags_host_1.bbpf", "host", 1000, 0, [BOSS, BOSS2])
        snap(g / "user/party/flags_guest-pool_1.bbpf", "guest-pool", 2000, 1, [BOSS, BOSS2])
        r = sc.check_flags(sc.snapshots(h), sc.snapshots(g), self.rows)
        self.assertEqual(r["status"], "ok")
        snap(g / "user/party/flags_guest-pool_2.bbpf", "guest-pool", 3000, 1, [BOSS])
        r = sc.check_flags(sc.snapshots(h), sc.snapshots(g), self.rows)
        self.assertEqual(r["status"], "LOST")
        self.assertEqual(r["missing"], [(BOSS2, "cutscene_seen")])

    def test_host_dump_after_the_guest_is_not_used(self):
        h, g = self.folder("inst0"), self.folder("inst1")
        snap(h / "user/party/flags_host_1.bbpf", "host", 1000, 0, [BOSS])
        snap(h / "user/party/flags_host_2.bbpf", "host", 5000, 0, [BOSS, BOSS2])  # after the guest left
        snap(g / "user/party/flags_guest-pool_1.bbpf", "guest-pool", 2000, 1, [BOSS])
        self.assertEqual(sc.check_flags(sc.snapshots(h), sc.snapshots(g), self.rows)["status"], "ok")

    def test_items_doubled_and_missing(self):
        host = ["cooperators 0 -> 1", "award lot 31000 (hostOnly 1, flag 0) in host world, role host",
                "award lot 42000 (hostOnly 1, flag 0) in host world, role host",
                "cooperators 1 -> 0", "award lot 55000 (hostOnly 1, flag 0) in host world, role host"]
        guest = ["gave #1 lot 31000 (ledger 25 = flag 60009025, award): ok", "Boot: launching",
                 "skip #1 lot 31000 (ledger 25 = flag 60009025, award): already given (flag 1 set)"]
        r = sc.check_items(host, guest)
        self.assertEqual(r["missing"], {42000: 3})  # 55000 came while alone
        self.assertEqual(r["doubled"], {})
        r = sc.check_items(host[:2], guest + ["gave #2 lot 31000 (ledger 25 = flag 60009025, award): ok"])
        self.assertEqual(r["status"], "DOUBLED")

    def test_echo_drop_after_restart(self):
        ok = ["Echoes 48", "guest died alone", "Echoes 0", "Boot: launching", "Echoes 0"]
        self.assertEqual(sc.check_echoes(ok)["status"], "ok")
        lost = ["Echoes 96", "Boot: launching", "Echoes 48"]
        r = sc.check_echoes(lost)
        self.assertEqual(r["status"], "LOST")
        self.assertEqual(r["drops"][0][:2], (96, 48))


    def test_guest_save_reads_in_a_save_editor_after_the_export(self):
        sys.path.insert(0, str(ROOT / "tests"))
        from test_save_editor import make_save  # noqa: E402
        g = self.folder("inst1")
        slot = g / "user/savedata_party/1/CUSA00207/SPRJ0005"
        slot.mkdir(parents=True)
        (slot / "userdata0001").write_bytes(bytes(0x1000))  # an empty slot: not counted
        (slot / "userdata0000").write_bytes(make_save(guest=True))  # written in the host's world
        r = sc.check_saves(g)
        self.assertEqual((r["status"], r["checked"], r["guest_written"], r["bad"]), ("ok", 1, ["userdata0000"], []))
        (slot / "userdata0000").write_bytes(make_save()[:0x100000])  # a damaged save: the export cannot help
        self.assertEqual(sc.check_saves(g)["status"], "EDITOR")
        self.assertEqual(sc.check_saves(self.folder("inst2"))["status"], "no saves")


if __name__ == "__main__":
    unittest.main()
