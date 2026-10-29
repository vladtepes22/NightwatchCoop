"""The JSON entry point, using synthetic saves only."""
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import struct

import test_saveedit
from test_saveedit import make_save
import saveedit_cli as cli
from saveedit import instances, model, names, store


class SaveEditorCliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.user = Path(self.temp.name)
        self.file = self.user / "savedata" / "1" / "TEST" / "SPRJ0005" / "userdata0000"
        self.file.parent.mkdir(parents=True)
        self.original = make_save(name="Tester")
        self.file.write_bytes(self.original)

    def call(self, command, edit=None, running=lambda: [], **options):
        argv = [command, "--user", str(self.user), "--kind", "solo"]
        if command in ("show", "apply"):
            argv += ["--slot", "0"]
        for key, value in options.items():
            argv += ["--" + key, str(value)]
        output = io.StringIO()
        code = cli.main(argv, io.StringIO(json.dumps(edit)), output, running)
        return code, json.loads(output.getvalue())

    def test_slots_and_show(self):
        code, rows = self.call("slots")
        self.assertEqual(code, 0)
        self.assertEqual(rows, [dict(slot=0, name="Tester", level=25, echoes=500)])
        code, row = self.call("show")
        self.assertEqual(code, 0)
        self.assertEqual(row["character"]["stats"]["vigor"], 10)
        self.assertEqual(row["character"]["ng_cycle"], 0)
        self.assertEqual(row["items"][0]["level"], 3)
        self.assertEqual(row["items"][1]["count"], 5)
        with patch.object(names.Catalogue, "load", return_value=names.Catalogue(
                {"names": {"goods": {100: "Test item"}}, "ids": {}})):
            self.assertEqual(self.call("show")[1]["items"][1]["name"], "Test item")

    def test_apply_backup_restore(self):
        revision = self.call("show")[1]["revision"]
        edit = dict(revision=revision, name="Edited", stats={"vigor": 20}, echoes=1234,
                    insight=12, ng_cycle=2, items=[dict(section=0, slot=1, count=8),
                                                 dict(section=0, slot=2, remove=True),
                                                 dict(section=0, slot=0, level=5)])
        catalogue = names.Catalogue({"names": {}, "ids": {"weapon": [22000500]}})
        with patch.object(names.Catalogue, "load", return_value=catalogue):
            code, result = self.call("apply", edit)
        self.assertEqual(code, 0, result)
        backup = Path(result["backup"])
        self.assertEqual((backup / "1/TEST/SPRJ0005/userdata0000").read_bytes(), self.original)
        m = model.SaveModel(self.file.read_bytes())
        self.assertEqual((m.name, m.level, m.echoes, m.insight, m.ng_cycle), ("Edited", 35, 1234, 12, 2))
        self.assertEqual([(i.slot, i.count, i.level) for i in m.items((0,))], [(0, 1, 5), (1, 8, None)])
        self.assertEqual(self.call("backups")[1][0]["name"], backup.name)
        code, result = self.call("restore", backup=backup.name)
        self.assertEqual(code, 0, result)
        self.assertTrue(Path(result["backup"]).is_dir())
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_progress_findings_and_repairs(self):
        self.file.write_bytes(make_save(name="Tester", phantom=1))
        catalogue = names.Catalogue({"names": {}, "ids": {"weapon": [22000000, 22000300, 22000500, 22001000]}})
        with patch.object(names.Catalogue, "load", return_value=catalogue):
            shown = self.call("show")[1]
            boss = next(f for f in shown["flags"] if f["kind"] == "boss")
            self.assertFalse(boss["on"])
            self.assertTrue(shown["findings"]["phantom"])
            self.assertEqual(shown["items"][0]["max_level"], 10)
            self.assertIsNone(shown["items"][1]["max_level"])
            edit = dict(revision=shown["revision"], flags=[dict(id=boss["id"], on=True)], repair=["phantom"])
            self.assertEqual(self.call("apply", edit)[0], 0)
            after = self.call("show")[1]
            self.assertTrue(next(f for f in after["flags"] if f["id"] == boss["id"])["on"])
            self.assertFalse(after["findings"]["phantom"])
            for bad in (dict(flags=[dict(id=1, on=True)]), dict(flags=[dict(id=boss["id"], on=1)]),
                        dict(repair=["everything"])):
                code, result = self.call("apply", dict(revision=after["revision"], **bad))
                self.assertEqual(code, 1, bad)
            self.assertEqual(self.call("show")[1]["revision"], after["revision"])

    def test_refuses_running_and_conflict(self):
        revision = self.call("show")[1]["revision"]
        edit = dict(revision=revision, echoes=1234)
        code, error = self.call("apply", edit, running=lambda: [1])
        self.assertEqual((code, error["type"]), (1, "GameRunning"))
        self.assertEqual(self.file.read_bytes(), self.original)
        self.assertEqual(self.call("backups")[1], [])
        self.file.write_bytes(make_save(echoes=600))
        code, error = self.call("apply", edit)
        self.assertEqual((code, error["type"]), (1, "EditConflict"))
        self.assertEqual(self.call("backups")[1], [])

    def test_store_conflict_after_open(self):
        revision = self.call("show")[1]["revision"]
        def changed():
            self.file.write_bytes(make_save(echoes=900))
            return []
        code, error = self.call("apply", dict(revision=revision, echoes=1), running=changed)
        self.assertEqual((code, error["type"]), (1, "EditConflict"))
        self.assertEqual(self.call("backups")[1], [])

    def test_invalid_edit_and_backup_failure_do_not_write(self):
        revision = self.call("show")[1]["revision"]
        for edit in ({"revision": revision, "insight": -1}, {"echoes": 1}):
            self.assertEqual(self.call("apply", edit)[0], 1)
        with patch.object(store, "make_backup", side_effect=OSError("backup failed")):
            self.assertEqual(self.call("apply", dict(revision=revision, echoes=1))[0], 1)
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_restore_refuses_running_and_unknown_backup(self):
        revision = self.call("show")[1]["revision"]
        backup = self.call("apply", dict(revision=revision, echoes=1))[1]["backup"]
        edited = self.file.read_bytes()
        code, error = self.call("restore", backup=Path(backup).name, running=lambda: [1])
        self.assertEqual((code, error["type"]), (1, "GameRunning"))
        self.assertEqual(self.call("restore", backup="../elsewhere")[0], 1)
        self.assertEqual(self.file.read_bytes(), edited)

    def test_gems_sockets_icons_and_placeholders(self):
        d = bytearray(self.original)
        n = test_saveedit.NAME_AT + model.NORMAL_BEHIND_NAME
        # instance table: the inventory weapon with one filled slot, the Memory Altar, a loose gem, a rune
        none = (instances.NO_SLOT, 0)
        struct.pack_into("<15I", d, 0x54, 0x80800010, 22000300, 0, 0, 0, 1, 0xC0800002, *none * 4)
        struct.pack_into("<15I", d, 0x54 + 60, 0x80800011, 1000, 0, 0, 0, *none * 5)
        struct.pack_into("<8I", d, 0x54 + 120, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF)
        gem = 0x54 + 152
        struct.pack_into("<10I", d, gem, 0xC0800001, 0x80000000 | 124001, 1, 1, 13105, *[0xFFFFFFFF] * 5)
        struct.pack_into("<10I", d, gem + 40, 0xC0800002, 0x80000000 | 124001, 1, 2, 13105, *[0xFFFFFFFF] * 5)
        struct.pack_into("<10I", d, gem + 80, 0xC0800003, 0x80000000 | 129001, 2, 63, 600, 601, *[0xFFFFFFFF] * 4)
        test_saveedit.put_item(d, n, 3, 0xC0800001, 0x80000000 | 124001, 1, 8)
        test_saveedit.put_item(d, n, 4, 0x80800011, 1000, 1, 9)
        test_saveedit.put_item(d, n, 5, 0xC0800003, 0x80000000 | 129001, 1, 10)
        struct.pack_into("<I", d, n, 6)
        self.file.write_bytes(bytes(d))
        catalogue = names.Catalogue({
            "names": {"weapon": {22000000: "Test Cleaver"}, "effect": {13105: "Add blood ATK +2.5",
                      28101: "ATK DOWN -0.3%", 600: "Rune effect", 700: "Caryll effect"},
                      "gem": {1000005: "Test Gemstone", 2000005: "Triangle Gemstone", 1100005: "Cursed Gemstone",
                              16000016: "Test Rune"}},
            "ids": {"weapon": [22000300]}, "icons": {"weapon": {22000000: 1001}},
            "effects": {13105: [0, 5, 5, 0], 28101: [1, 0, 0, 0], 600: [0, 216, 16, 1], 601: [0, 0, 0, 1],
                        700: [0, 0, 0, 1]},
            "gemgen": {124001: 0, 129001: 10000000}})
        with patch.object(names.Catalogue, "load", return_value=catalogue):
            shown = self.call("show")[1]
            items = shown["items"]
            self.assertNotIn(1000, [i["number"] for i in items if i["kind"] == "weapon"])
            weapon = next(i for i in items if i["kind"] == "weapon")
            self.assertEqual(weapon["sockets"], [dict(shape=1, gem="Triangle Gemstone")])
            loose = next(i for i in items if i["section"] == 0 and i["kind"] == "gem" and i["gem"]["type"] == 1)
            self.assertEqual((loose["name"], loose["gem"]["cursed"]), ("Test Gemstone", False))
            rune = next(i for i in items if i["kind"] == "gem" and i["gem"]["type"] == 2)
            self.assertEqual((rune["name"], rune["gem"]["sort"]), ("Test Rune", 1))
            socketed = next(i for i in items if i["section"] == cli.SOCKETED)
            self.assertTrue(socketed["locked"])
            self.assertEqual(socketed["gem"]["socketed_in"], dict(number=22000300, name="Test Cleaver"))
            self.assertIsNone(weapon["icon"])  # no --game: no icon files
            self.assertEqual(self.call("effects")[1], [dict(id=13105, text="Add blood ATK +2.5"),
                                                       dict(id=28101, text="ATK DOWN -0.3%")])
            revision = shown["revision"]
            for bad in ([], [13105] * 4, [600], [99], ["13105"]):
                edit = dict(revision=revision, gems=[dict(handle=loose["gem"]["handle"], effects=bad)])
                self.assertEqual(self.call("apply", edit)[0], 1, bad)
            for bad in (dict(gems=[dict(handle=rune["gem"]["handle"], effects=[700])]),
                        dict(items=[dict(section=0, slot=4, remove=True)])):
                self.assertEqual(self.call("apply", dict(revision=revision, **bad))[0], 1, bad)
            before = self.file.read_bytes()
            edit = dict(revision=revision, gems=[dict(handle=loose["gem"]["handle"], effects=[13105, 28101])])
            self.assertEqual(self.call("apply", edit)[0], 0)
            after = self.file.read_bytes()
            self.assertEqual([i for i in range(len(after)) if after[i] != before[i]], [gem + 20, gem + 21, gem + 22, gem + 23])
            again = next(i for i in self.call("show")[1]["items"] if i.get("gem", {}).get("handle") == 0xC0800001)
            self.assertEqual((again["name"], again["gem"]["cursed"]), ("Cursed Gemstone", True))

    def test_subprocess_json_contract(self):
        script = Path(cli.__file__)
        for command, expected in (("slots", 0), ("invalid", 1)):
            result = subprocess.run([sys.executable, str(script), command, "--user", str(self.user),
                                     "--kind", "solo"], capture_output=True, text=True)
            self.assertEqual(result.returncode, expected)
            json.loads(result.stdout)
            self.assertEqual(result.stderr, "")

    @unittest.skipUnless(os.name == "nt", "Windows process inspection")
    def test_windows_process_inspection(self):
        self.assertIsInstance(cli.running_games(), list)


if __name__ == "__main__":
    unittest.main()
