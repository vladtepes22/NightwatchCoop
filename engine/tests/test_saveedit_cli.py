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

from test_saveedit import make_save
import saveedit_cli as cli
from saveedit import model, names, store


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
