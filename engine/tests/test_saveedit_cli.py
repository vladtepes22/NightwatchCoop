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

    def test_show_weapon_hand(self):
        for hands, expected in (({22000300: "left"}, "left"), ({22000000: "right"}, "right"),
                                ({22000000: "left"}, "left"), ({}, "right")):
            with self.subTest(hands=hands), patch.object(names.Catalogue, "load", return_value=names.Catalogue(
                    {"weapon_hands": hands})):
                code, shown = self.call("show")
                self.assertEqual(code, 0)
                self.assertEqual(shown["items"][0]["hand"], expected)
                self.assertNotIn("hand", shown["items"][1])
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_flag_labels_without_ids(self):
        rows = cli.flag_rows(names.Catalogue())
        self.assertTrue(rows)
        for flag, kind, group, label in rows:
            self.assertNotIn(f"[{flag}]", label)
        self.assertEqual([(f, g, label) for f, k, g, label in rows if k == "ending"],
                         cli.flagsdata.endings())

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

    def test_clear_boss_flag(self):
        m = model.SaveModel(self.original)
        m.set_flag(12411700, True)
        self.file.write_bytes(m.to_bytes())
        revision = self.call("show")[1]["revision"]
        code, result = self.call("apply", dict(revision=revision, flags=[dict(id=12411700, on=False)]))
        self.assertEqual(code, 0, result)
        self.assertFalse(model.SaveModel(self.file.read_bytes()).flag(12411700))
        self.assertFalse(next(f for f in self.call("show")[1]["flags"] if f["id"] == 12411700)["on"])

    def test_ludwig_group_sets_and_clears(self):
        for on in (True, False):
            revision = self.call("show")[1]["revision"]
            code, result = self.call("apply", dict(revision=revision, flags=[dict(id=13401800, on=on)]))
            self.assertEqual(code, 0, result)
            m = model.SaveModel(self.file.read_bytes())
            self.assertEqual((m.flag(13401800), m.flag(9471)), (on, on))
            self.assertEqual(next(f for f in self.call("show")[1]["flags"] if f["id"] == 13401800)["on"], on)

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
            # A copy is a new loose gem or rune with the source's record; the source is unchanged.
            revision = self.call("show")[1]["revision"]
            self.assertEqual(self.call("apply", dict(revision=revision, copy_gems=[0xDEAD]))[0], 1)
            code, reply = self.call("apply", dict(revision=revision, copy_gems=[0xC0800001, 0xC0800003]))
            self.assertEqual(code, 0, reply)
            items = self.call("show")[1]["items"]
            copies = [i for i in items if i.get("gem", {}).get("handle", 0) > 0xC0800010]
            self.assertEqual([(c["gem"]["type"], c["gem"]["shape"], c["section"]) for c in copies], [(1, 1, 0), (2, 63, 0)])
            self.assertEqual([e["id"] for e in copies[0]["gem"]["effects"]], [13105, 28101])
            self.assertEqual([e["id"] for e in copies[1]["gem"]["effects"]], [600, 601])
            self.assertEqual(self.call("slots")[1][0]["name"], "Tester")

    def test_add_gems_and_runes_only_from_the_games_own_pool(self):
        d = bytearray(self.original)
        n = test_saveedit.NAME_AT + model.NORMAL_BEHIND_NAME
        none = (instances.NO_SLOT, 0)
        struct.pack_into("<15I", d, 0x54, 0x80800010, 22000300, 0, 0, 0, *none * 5)
        struct.pack_into("<8I", d, 0x54 + 60, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF)
        self.file.write_bytes(bytes(d))
        catalogue = names.Catalogue({
            "names": {"effect": {13105: "Add blood ATK +2.5", 13106: "Cut effect text", 700: "Caryll effect",
                                 701: "Cut rune", 28101: "ATK DOWN -0.3%", 2107001: "Oath effect"},
                      "gem": {1000005: "Test Gemstone", 11000016: "Test Rune", 12000004: "Test Oath"}},
            "ids": {"weapon": [22000300]}, "icons": {},
            "effects": {13105: [0, 5, 5, 0], 13106: [0, 5, 5, 0], 28101: [1, 0, 0, 0], 700: [0, 216, 16, 1],
                        701: [0, 0, 0, 1], 2107001: [0, 1013, 4, 2]},
            "gemgen": {127000: 0, 127002: 0, 100301: 10000000, 400: 10000000, 2100301: 10000000, 100401: 10000000, 200000: 10000000},
            # id -> [name offset, shape bits, fixed effect]
            "gemrows": {127000: [0, 1, 13105], 127002: [0, 2, 13105], 100301: [10000000, 1, 700],
                        400: [10000000, 1, 700], 2100301: [10000000, 1, 700], 100401: [10000000, 1, 701],
                        200000: [10000000, 2, 2107001, 2107002, 2107003]},  # an oath rune with three fixed effects
            "reach": [13105, 700, 701, 28101]})  # 13106 has a name but no generator reaches it
        with patch.object(names.Catalogue, "load", return_value=catalogue):
            self.assertEqual(self.call("catalog", edit=dict(category=8, text=""))[1],
                             [dict(number=700, name="Test Rune", icon=None, shape=1),
                              dict(number=2107001, name="Test Oath", icon=None, shape=2)])  # 701 has no name; 400 and 2100301 are other tables
            revision = self.call("show")[1]["revision"]
            for bad in ([dict(type=2, effect=701)], [dict(type=2, effect=99)], [dict(type=1, shape=1, effects=[13106])],
                        [dict(type=1, shape=4, effects=[13105])], [dict(type=1, shape=1, effects=[])],
                        [dict(type=1, shape=1, effects=[13105] * 4)], [dict(type=3)], [dict(type=2, effect=700, shape=1)]):
                self.assertEqual(self.call("apply", dict(revision=revision, add_gems=bad))[0], 1, bad)
            good = [dict(type=1, shape=2, effects=[13105, 28101]), dict(type=2, effect=700), dict(type=2, effect=2107001)]
            code, reply = self.call("apply", dict(revision=revision, add_gems=good))
            self.assertEqual(code, 0, reply)
            gems = [i["gem"] for i in self.call("show")[1]["items"] if i.get("gem")]
            self.assertEqual([(g["type"], g["shape"], g["gen"], [e["id"] for e in g["effects"]]) for g in gems],
                             [(1, 2, 127002, [13105, 28101]), (2, 1, 100301, [700]), (2, 2, 200000, [2107001, 2107002, 2107003])])
            # An effect the game never makes cannot be set on a gem, but one the gem already has stays.
            handle = gems[0]["handle"]
            revision = self.call("show")[1]["revision"]
            self.assertEqual(self.call("apply", dict(revision=revision, gems=[dict(handle=handle, effects=[13106])]))[0], 1)
            self.assertEqual(self.call("apply", dict(revision=revision, gems=[dict(handle=handle, effects=[28101, 13105])]))[0], 0)

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

    def worker(self, requests, running=lambda: []):
        lines = "".join(r if isinstance(r, str) else json.dumps(r) + "\n" for r in requests)
        output = io.StringIO()
        code = cli.main(["worker", "--user", str(self.user), "--kind", "solo"], io.StringIO(lines), output, running)
        self.assertEqual(code, 0)
        return [json.loads(line) for line in output.getvalue().splitlines()]

    def test_worker_answers_each_request_with_the_same_checks(self):
        shown = self.call("show")[1]
        slots, show, bad, unknown, no_slot, other, applied, stale = self.worker([
            dict(command="slots"), dict(command="show", slot=0), "not json\n", dict(command="worker"), dict(command="show"),
            dict(command="slots", kind="nonsense"),
            dict(command="apply", slot=0, edit=dict(revision=shown["revision"], echoes=1234)),
            dict(command="apply", slot=0, edit=dict(revision=shown["revision"], echoes=99)),
        ])
        self.assertEqual(slots["result"][0]["name"], "Tester")
        self.assertEqual(show["result"], shown)
        self.assertIn("error", bad)
        self.assertEqual(unknown["error"], "unknown command")
        self.assertIn("slot", no_slot["error"])
        self.assertEqual(other["error"], "unknown save type")
        self.assertTrue(applied["result"]["backup"])
        self.assertEqual(stale["type"], "EditConflict")
        self.assertEqual(self.call("show")[1]["character"]["echoes"], 1234)

    def test_worker_refuses_a_write_while_the_game_runs(self):
        shown = self.call("show")[1]
        reply = self.worker([dict(command="apply", slot=0, edit=dict(revision=shown["revision"], echoes=7))], running=lambda: ["game"])[0]
        self.assertEqual(reply["type"], "GameRunning")
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_worker_keeps_the_catalogue(self):
        with patch.object(cli.names.Catalogue, "load", wraps=cli.names.Catalogue.load) as load:
            self.worker([dict(command="show", slot=0), dict(command="show", slot=0)])
        self.assertEqual(load.call_count, 1)

    def catalogue(self):
        return names.Catalogue({"names": {"goods": {1000: "Blood Vial", 1100: "Quicksilver Bullet"},
                                          "weapon": {4000000: "Hunter Axe"}, "armor": {40000: "Hunter Cap"}},
                                "ids": {"goods": [1000, 1100, 5], "weapon": [4000000, 4000100], "armor": [40000]}})

    def test_catalog_lists_named_items_only(self):
        with patch.object(names.Catalogue, "load", return_value=self.catalogue()):
            code, rows = self.call("catalog", dict(category=4, text="vial"))
            self.assertEqual((code, [r["number"] for r in rows]), (0, [1000]))
            self.assertEqual([r["name"] for r in self.call("catalog", dict(category=0))[1]], ["Hunter Axe"])
            self.assertEqual(self.call("catalog", dict(category=2))[0], 1)
            self.assertEqual(self.call("catalog", dict(text="x"))[0], 1)

    def test_add_items_through_apply(self):
        with patch.object(names.Catalogue, "load", return_value=self.catalogue()):
            revision = self.call("show")[1]["revision"]
            code, reply = self.call("apply", dict(revision=revision, add=[
                dict(category=4, number=1100, count=30), dict(category=0, number=4000000),
                dict(category=1, number=40000)]))
            self.assertEqual(code, 0, reply)
            rows = [r for r in self.call("show")[1]["items"] if r["section"] == 0]
        self.assertEqual([(r["kind"], r["number"], r["count"]) for r in rows[-3:]],
                         [("goods", 1100, 30), ("weapon", 4000000, 1), ("armor", 40000, 1)])
        self.assertEqual(self.call("slots")[1][0]["name"], "Tester")

    def test_add_items_refuses_what_is_not_offered(self):
        bad = ([dict(category=4, number=999)], [dict(category=4, number=1000, count=100)],
               [dict(category=4, number=1000, count=0)], [dict(category=0, number=4000100)],
               [dict(category=8, number=1)], [dict(category=4, number=100)], [dict(category=4, number=5)],
               [dict(category=4, number="1000")], [dict(category=4, number=1000, extra=1)], "x")
        with patch.object(names.Catalogue, "load", return_value=self.catalogue()):
            revision = self.call("show")[1]["revision"]
            for add in bad:
                with self.subTest(add=add):
                    self.assertEqual(self.call("apply", dict(revision=revision, add=add))[0], 1)
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_cut_content_is_named_but_never_added(self):
        cat = names.Catalogue({"names": {"goods": {1000: "Blood Vial"}}, "ids": {"armor": [40000, 902000], "goods": [1000]}})
        self.assertEqual(cat.name(1, 902000), "[CUT] Arms")
        self.assertIsNone(cat.name(1, 40000))
        with patch.object(names.Catalogue, "load", return_value=cat):
            self.assertEqual(self.call("catalog", dict(category=1))[1], [])
            revision = self.call("show")[1]["revision"]
            self.assertEqual(self.call("apply", dict(revision=revision, add=[dict(category=1, number=902000)]))[0], 1)
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_gem_preview_uses_the_saved_gems_rule(self):
        class Fake:
            effects = None

            def gem(self, gen, shape, effects):
                Fake.effects = (gen, shape, effects)
                return "Gold Radial Blood Gem", 3007, True

        with patch.object(cli.names.Catalogue, "load", return_value=Fake()):
            reply = self.worker([dict(command="gem_preview", edit=dict(gen=5, shape=1, effects=[7, 8]))])[0]
            bad = self.worker([dict(command="gem_preview", edit=dict(shape=1))])[0]
        self.assertEqual(Fake.effects, (5, 1, [7, 8]))
        self.assertEqual(reply["result"], dict(name="Gold Radial Blood Gem", icon=None, cursed=True))
        self.assertIn("error", bad)
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_the_rune_list_is_never_cut_so_the_oath_runes_at_its_end_show(self):
        class Runes:  # the game has 67 Caryll rows before the six oath runes
            def caryll_runes(self):
                return [(100000 + k, 1100000 + k, 1) for k in range(67)] + [(200000 + 10 * k, 2100000 + k, 2) for k in range(6)]

            def gem(self, gen, shape, effects):
                return (f"Rune {effects[0]}", None, None)
        rows = cli.rune_catalog(Runes(), "", None, None)
        self.assertEqual(len(rows), 73)
        self.assertEqual([r["shape"] for r in rows].count(2), 6)


if __name__ == "__main__":
    unittest.main()
