"""Merger unit tests on synthetic files.

  C:/msys64/clang64/bin/python.exe -I -B -m unittest discover -s tools/merger/tests
"""
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import synth  # noqa: E402
from common import MergeError, Report, containers  # noqa: E402
import merger  # noqa: E402
from merge_any import Context, merge_bnd  # noqa: E402
from merge_fmg import merge_fmg  # noqa: E402
from merge_hks import merge_hks, units  # noqa: E402
from merge_param import Paramdefs, merge_param  # noqa: E402
from merge_tae import merge_tae, read_anims, arg_sizes  # noqa: E402
from merge_tpf import merge_tpf, tpf_read  # noqa: E402

TYPE = "TEST_PARAM_ST"
FIELDS = [("s32", "attackBaseMagic", 4), ("f32", "speed", 4), ("u8", "flagA:1", 1), ("u8", "flagB:1", 1),
          ("dummy8", "pad:6", 0), ("u8", "level", 1), ("dummy8", "pad2[2]", 2)]
SIZE = 12


def row(magic=100, speed=1.0, flags=0, level=0):
    return struct.pack("<ifBB2x", magic, speed, flags, level)


def defs():
    return Paramdefs(synth.bnd([(0, "N:\\x\\Test.paramdef", synth.paramdef(TYPE, FIELDS))]))


def values(param_bytes):
    p = containers.Param(param_bytes)
    return [(r[0], r[1]) for r in p.rows]


def names_of(param_bytes):
    from merge_param import row_name
    p = containers.Param(param_bytes)
    return [row_name(p, r[2]) for r in p.rows]


class ParamTests(unittest.TestCase):
    def merge(self, vanilla, mods):
        report = Report("gameparam")
        out, _ = merge_param("Test", vanilla, mods, report, defs())
        return out, report

    def test_paramdef_layout(self):
        units = defs().layout(TYPE, SIZE).units
        self.assertEqual([(u.name, u.bit, u.width) for u in units],
                         [("attackBaseMagic", 0, 32), ("speed", 32, 32), ("flagA", 64, 1), ("flagB", 65, 1),
                          ("pad", 66, 6), ("level", 72, 8), ("pad2[2]", 80, 16)])

    def test_mod_equal_to_vanilla_changes_nothing(self):
        vanilla = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(), "b")])
        changed = synth.param(TYPE, SIZE, [(1, row(150), "a"), (2, row(), "b")])
        out, report = self.merge(vanilla, [("A", vanilla), ("B", changed)])
        self.assertEqual(out, changed)
        self.assertEqual(report.conflicts, 0)

    def test_field_conflict_later_wins_with_names(self):
        vanilla = synth.param(TYPE, SIZE, [(10, row(100), "x")])
        a = synth.param(TYPE, SIZE, [(10, row(180), "x")])
        b = synth.param(TYPE, SIZE, [(10, row(150), "x")])
        report = Report("param\\gameparam\\gameparam.parambnd.dcx")
        out, _ = merge_param("EquipParamWeapon", vanilla, [("Nocturne", a), ("Weapon Balance", b)], report, defs(),
                             lambda table, row_id: "Kos Parasite +10")
        self.assertEqual(values(out), [(10, row(150))])
        self.assertEqual(report.lines, ["CONFLICT param\\gameparam\\gameparam.parambnd.dcx EquipParamWeapon row 10 "
                                        "(Kos Parasite +10) attackBaseMagic: Nocturne 180, Weapon Balance 150 -> "
                                        "Weapon Balance wins"])

    def test_different_fields_of_one_row_both_kept(self):
        vanilla = synth.param(TYPE, SIZE, [(10, row(), "x")])
        a = synth.param(TYPE, SIZE, [(10, row(magic=5), "x")])
        b = synth.param(TYPE, SIZE, [(10, row(speed=2.5), "x")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual(values(out), [(10, row(5, 2.5))])
        self.assertEqual(report.conflicts, 0)

    def test_bit_merge_two_mods_change_different_bits_of_one_byte(self):
        vanilla = synth.param(TYPE, SIZE, [(10, row(flags=0), "x")])
        a = synth.param(TYPE, SIZE, [(10, row(flags=0b01), "x")])  # flagA
        b = synth.param(TYPE, SIZE, [(10, row(flags=0b10), "x")])  # flagB
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual(values(out), [(10, row(flags=0b11))])
        self.assertEqual(report.conflicts, 0)

    def test_same_bit_set_differently_is_a_conflict(self):
        vanilla = synth.param(TYPE, SIZE, [(10, row(flags=0b01), "x")])
        a = synth.param(TYPE, SIZE, [(10, row(flags=0b00), "x")])
        b = synth.param(TYPE, SIZE, [(10, row(flags=0b11), "x")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        # A clears flagA; B sets flagB (flagA unchanged): no conflict, both kept.
        self.assertEqual(values(out), [(10, row(flags=0b10))])
        self.assertEqual(report.conflicts, 0)
        c = synth.param(TYPE, SIZE, [(10, row(flags=0b00), "x")])
        d = synth.param(TYPE, SIZE, [(10, row(flags=0b00), "x")])
        out, report = self.merge(vanilla, [("C", c), ("D", d)])
        self.assertEqual(report.conflicts, 0)  # both make the same change

    def test_numeric_field_is_one_unit(self):
        """Two mods changing different bits of one number: the later mod's whole number, never a mix."""
        vanilla = synth.param(TYPE, SIZE, [(10, row(level=0b0000), "x")])
        a = synth.param(TYPE, SIZE, [(10, row(level=0b0001), "x")])
        b = synth.param(TYPE, SIZE, [(10, row(level=0b0100), "x")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual(values(out), [(10, row(level=0b0100))])
        self.assertEqual(report.conflicts, 1)
        self.assertIn("level: A 1, B 4 -> B wins", report.lines[0])

    def test_duplicate_ids_keyed_by_occurrence(self):
        vanilla = synth.param(TYPE, SIZE, [(5, row(1), "first"), (5, row(2), "second"), (6, row(3), "z")])
        a = synth.param(TYPE, SIZE, [(5, row(1), "first"), (5, row(20), "second"), (6, row(3), "z")])
        b = synth.param(TYPE, SIZE, [(5, row(10), "first"), (5, row(2), "second"), (6, row(3), "z")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual(values(out), [(5, row(10)), (5, row(20)), (6, row(3))])
        self.assertEqual(report.conflicts, 0)

    def test_deleted_duplicate_does_not_shift_the_next(self):
        vanilla = synth.param(TYPE, SIZE, [(5, row(1), "first"), (5, row(2), "second")])
        a = synth.param(TYPE, SIZE, [(5, row(2), "second")])  # deletes the first occurrence
        b = synth.param(TYPE, SIZE, [(5, row(1), "first"), (5, row(2, 9.0), "second")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual(values(out), [(5, row(2, 9.0))])
        self.assertEqual(report.conflicts, 0)

    def test_unsorted_table_keeps_order_and_inserts_after_next_lower_id(self):
        order = [(30, row(), "c"), (10, row(), "a"), (20, row(), "b"), (50, row(), "e")]
        vanilla = synth.param(TYPE, SIZE, order)
        a = synth.param(TYPE, SIZE, order + [(25, row(7), "new25")])
        b = synth.param(TYPE, SIZE, sorted(order) + [(5, row(8), "new5"), (40, row(9), "new40")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual([r[0] for r in values(out)], [5, 30, 40, 10, 20, 25, 50])
        self.assertEqual(names_of(out), ["new5", "c", "new40", "a", "b", "new25", "e"])
        self.assertEqual(report.conflicts, 0)

    def test_new_id_clash_later_wins_identical_is_no_conflict(self):
        vanilla = synth.param(TYPE, SIZE, [(1, row(), "a")])
        a = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(7), "two"), (3, row(1), "three")])
        b = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(8), "two"), (3, row(1), "three")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual(values(out), [(1, row()), (2, row(8)), (3, row(1))])
        self.assertEqual(report.conflicts, 1)
        self.assertIn("row 2 (two) (a new row; the versions differ in attackBaseMagic): A attackBaseMagic=7, "
                      "B attackBaseMagic=8 -> B wins", report.lines[0])

    def test_row_names_set_differently_are_one_summary_line(self):
        # Two mods naming rows differently (a translation) change nothing in the game: one INFO line per table, no
        # CONFLICT per row; a value conflict in the same table stays its own line.
        vanilla = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(), "b"), (3, row(), "c")])
        a = synth.param(TYPE, SIZE, [(1, row(), "Blade"), (2, row(5), "Hammer"), (3, row(), "c"), (9, row(1), "Neu")])
        b = synth.param(TYPE, SIZE, [(1, row(), "Klinge"), (2, row(6), "Hammer"), (3, row(), "c"), (9, row(1), "New")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual(names_of(out), ["Klinge", "Hammer", "c", "New"])
        self.assertEqual(values(out), [(1, row()), (2, row(6)), (3, row()), (9, row(1))])
        self.assertEqual(report.conflicts, 1)
        self.assertEqual([line for line in report.lines if line.startswith("CONFLICT")],
                         ["CONFLICT gameparam Test row 2 (b) attackBaseMagic: A 5, B 6 -> B wins"])
        self.assertIn("INFO gameparam: Test: 2 row names set differently by two or more mods (the game does not use "
                      "row names; the later mod's name is kept): rows 1, 9", report.lines)

    def test_row_name_merge(self):
        vanilla = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(), "b")])
        a = synth.param(TYPE, SIZE, [(1, row(), "renamed"), (2, row(), "b")])
        b = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(3), "b")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual(names_of(out), ["renamed", "b"])
        self.assertEqual(values(out), [(1, row()), (2, row(3))])

    def test_deleted_row(self):
        vanilla = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(), "b"), (3, row(), "c")])
        a = synth.param(TYPE, SIZE, [(1, row(), "a"), (3, row(), "c")])
        b = synth.param(TYPE, SIZE, [(1, row(4), "a"), (2, row(), "b"), (3, row(), "c")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertEqual(values(out), [(1, row(4)), (3, row())])
        self.assertEqual(report.conflicts, 0)
        c = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(9), "b"), (3, row(), "c")])
        out, report = self.merge(vanilla, [("A", a), ("C", c)])  # C changes the row A deletes: C wins, row kept
        self.assertEqual(values(out), [(1, row()), (2, row(9)), (3, row())])
        self.assertEqual(report.conflicts, 1)
        out, report = self.merge(vanilla, [("C", c), ("A", a)])  # A, later, deletes it
        self.assertEqual(values(out), [(1, row()), (3, row())])
        self.assertEqual(report.conflicts, 1)

    def test_row_size_fallback(self):
        vanilla = synth.param(TYPE, SIZE, [(1, row(), "a")])
        a = synth.param(TYPE, SIZE, [(1, row(5), "a")])
        bigger = synth.param(TYPE, SIZE + 4, [(1, row(6) + b"\0" * 4, "a")])
        out, report = self.merge(vanilla, [("A", a), ("Big", bigger)])
        self.assertEqual(out, bigger)
        self.assertEqual(len(report.lines), 1)
        self.assertTrue(report.lines[0].startswith("NOT MERGED gameparam Test: A, Big -> Big wins (row size 0x10"))

    def test_no_paramdef_falls_back_to_words(self):
        vanilla = synth.param("OTHER_ST", SIZE, [(1, row(), "a")])
        a = synth.param("OTHER_ST", SIZE, [(1, row(5), "a")])
        b = synth.param("OTHER_ST", SIZE, [(1, row(6), "a")])
        out, report = self.merge(vanilla, [("A", a), ("B", b)])
        self.assertIn("byte +0x0: A 5, B 6 -> B wins", report.lines[0])


class FmgTests(unittest.TestCase):
    def test_fmg_per_text_id(self):
        vanilla = synth.fmg({1: "one", 2: "two", 3: "three"})
        a = synth.fmg({1: "ONE", 2: "two", 3: "three", 10: "new"})
        b = synth.fmg({1: "one", 2: "TWO", 3: "three"})
        report = Report("msg")
        out, taken = merge_fmg("item.fmg", vanilla, [("A", a), ("B", b)], report)
        self.assertEqual(containers.fmg_read(out)["texts"], {1: "ONE", 2: "TWO", 3: "three", 10: "new"})
        self.assertEqual(report.conflicts, 0)

    def test_fmg_clash(self):
        vanilla = synth.fmg({1: "one"})
        a = synth.fmg({1: "uno"})
        b = synth.fmg({1: "eins"})
        report = Report("msg\\engus\\item.msgbnd.dcx")
        out, _ = merge_fmg("item.fmg", vanilla, [("A", a), ("B", b)], report)
        self.assertEqual(containers.fmg_read(out)["texts"][1], "eins")
        self.assertEqual(report.lines, ['CONFLICT msg\\engus\\item.msgbnd.dcx item.fmg text 1: A "uno", B "eins" -> '
                                        'B wins'])


class BndTests(unittest.TestCase):
    def test_inner_file_recurse(self):
        vparam = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(), "b")])
        vanilla = synth.bnd([(0, "N:\\p\\Test.param", vparam), (1, "N:\\p\\x.bin", b"xx"),
                             (2, "N:\\p\\t.fmg", synth.fmg({1: "a"}))])
        a = synth.bnd([(0, "N:\\p\\Test.param", synth.param(TYPE, SIZE, [(1, row(7), "a"), (2, row(), "b")])),
                       (1, "N:\\p\\x.bin", b"xx"), (2, "N:\\p\\t.fmg", synth.fmg({1: "A"})),
                       (3, "N:\\p\\new_a.bin", b"aa")])
        b = synth.bnd([(0, "N:\\p\\Test.param", synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(8), "b")])),
                       (1, "N:\\p\\x.bin", b"yy"), (2, "N:\\p\\t.fmg", synth.fmg({1: "a", 2: "B"}))])
        report = Report("gameparam")
        counts = {}
        out, _ = merge_bnd(report, "", vanilla, [("A", a), ("B", b)], Context(defs()), counts)
        entries = {e[2]: e[3] for e in containers.bnd4_read(out)["entries"]}
        self.assertEqual(values(entries["N:\\p\\Test.param"]), [(1, row(7)), (2, row(8))])
        self.assertEqual(entries["N:\\p\\x.bin"], b"yy")
        self.assertEqual(entries["N:\\p\\new_a.bin"], b"aa")
        self.assertEqual(containers.fmg_read(entries["N:\\p\\t.fmg"])["texts"], {1: "A", 2: "B"})
        self.assertEqual(report.conflicts, 0)
        self.assertEqual(counts["rows"], 2)

    def test_unmergeable_inner_file_clash(self):
        vanilla = synth.bnd([(0, "N:\\c\\c1000.flver", b"v")])
        a = synth.bnd([(0, "N:\\c\\c1000.flver", b"a")])
        b = synth.bnd([(0, "N:\\c\\c1000.flver", b"bb")])
        report = Report("chr\\c1000.chrbnd.dcx")
        out, _ = merge_bnd(report, "", vanilla, [("A", a), ("B", b)], Context(defs()), {})
        self.assertEqual(containers.bnd4_read(out)["entries"][0][3], b"bb")
        self.assertEqual(report.lines[0], "CONFLICT chr\\c1000.chrbnd.dcx c1000.flver: A 1 bytes, B 2 bytes -> B wins")

    def test_damaged_inner_file_falls_back_instead_of_stopping(self):
        # An ASCII-named TPF whose name has no end: the reader fails with a plain ValueError. The merge must keep the
        # later mod's file and go on, not stop the whole run.
        broken = bytearray(b"TPF\0" + bytes(4) + struct.pack("<I", 1) + b"\x04\x03\x02\x00")
        entry = bytearray(0x24)
        struct.pack_into("<I", entry, 0x18, 0x10 + 0x24)
        broken += entry + b"abc"
        vanilla = synth.bnd([(0, "N:\\m\\x.tpf", synth.tpf([("t", b"v" * 16)])),
                             (1, "N:\\m\\y.fmg", synth.fmg({1: "v"}))])
        a = synth.bnd([(0, "N:\\m\\x.tpf", synth.tpf([("t", b"a" * 16)])), (1, "N:\\m\\y.fmg", synth.fmg({1: "a"}))])
        b = synth.bnd([(0, "N:\\m\\x.tpf", bytes(broken)), (1, "N:\\m\\y.fmg", synth.fmg({1: "v", 2: "b"}))])
        report = Report("menu\\m.tpfbnd.dcx")
        out, _ = merge_bnd(report, "", vanilla, [("A", a), ("B", b)], Context(defs()), {})
        entries = containers.bnd4_read(out)["entries"]
        self.assertEqual(entries[0][3], bytes(broken))
        self.assertEqual(containers.fmg_read(entries[1][3])["texts"], {1: "a", 2: "b"})
        self.assertTrue(any("x.tpf: kept whole from B (the TPF file cannot be read" in line for line in report.lines),
                        report.lines)

    def test_inner_paths_matched_by_file_name(self):
        vanilla = synth.bnd([(10, "N:\\msg\\engGB\\names.fmg", synth.fmg({1: "colour", 2: "x"}))])
        a = synth.bnd([(10, "N:\\msg\\engUS\\names.fmg", synth.fmg({1: "color", 2: "x"}))])
        b = synth.bnd([(10, "N:\\msg\\engGB\\names.fmg", synth.fmg({1: "colour", 2: "y"}))])
        report = Report("msg\\enggb\\item.msgbnd.dcx")
        out, _ = merge_bnd(report, "", vanilla, [("A", a), ("B", b)], Context(defs()), {})
        entry = containers.bnd4_read(out)["entries"][0]
        self.assertEqual(entry[2], "N:\\msg\\engGB\\names.fmg")
        self.assertEqual(containers.fmg_read(entry[3])["texts"], {1: "color", 2: "y"})
        self.assertEqual(report.conflicts, 0)

    def test_dcx_inner_and_tpf(self):
        vanilla = synth.tpf([("tex_a", b"A" * 32), ("tex_b", b"B" * 16)])
        a = synth.tpf([("tex_a", b"a" * 32), ("tex_b", b"B" * 16), ("tex_c", b"C" * 8)])
        b = synth.tpf([("tex_a", b"A" * 32), ("tex_b", b"b" * 16)])
        report = Report("menu\\x.tpf.dcx")
        out, _ = merge_tpf("", vanilla, [("A", a), ("B", b)], report)
        self.assertEqual([(e[0], e[2]) for e in tpf_read(out)["entries"]],
                         [("tex_a", b"a" * 32), ("tex_b", b"b" * 16), ("tex_c", b"C" * 8)])
        c = synth.tpf([("tex_a", b"z" * 32), ("tex_b", b"B" * 16)])
        out, _ = merge_tpf("", vanilla, [("A", a), ("C", c)], report)
        self.assertEqual(report.lines, ["CONFLICT menu\\x.tpf.dcx texture tex_a: A 32 bytes, C 32 bytes -> C wins"])


class TaeTests(unittest.TestCase):
    def setUp(self):
        self.base = {100: [(96, 0.0, 1.0, struct.pack("<ii", 7, 8))], 200: [(1, 0.5, 0.6, b"\x01\0\0\0")],
                     300: []}

    def test_tae_per_animation(self):
        vanilla = synth.tae(self.base)
        a_anims = {**self.base, 100: [(96, 0.0, 1.0, struct.pack("<ii", 70, 8))], 400: [(2, 0, 1, b"\x05")]}
        b_anims = {**self.base, 200: [(1, 0.5, 0.9, b"\x01\0\0\0"), (5, 0, 1, b"\x09")]}
        report = Report("chr\\c0000.anibnd.dcx")
        out, taken = merge_tae("a00.tae", vanilla, [("A", synth.tae(a_anims)), ("B", synth.tae(b_anims))], report)
        self.assertEqual(report.conflicts, 0)
        self.assertEqual(taken, 3)
        sizes = arg_sizes(vanilla, [])
        want = read_anims(synth.tae({**a_anims, 200: b_anims[200]}), sizes)
        got = read_anims(out, sizes)
        self.assertEqual(sorted(got), [100, 200, 300, 400])
        for anim_id in got:
            self.assertEqual(got[anim_id].signature, want[anim_id].signature, anim_id)

    def test_tae_clash_and_delete(self):
        vanilla = synth.tae(self.base)
        a = synth.tae({**self.base, 100: [(96, 0.0, 2.0, struct.pack("<ii", 7, 8))]})
        b = synth.tae({**self.base, 100: [(96, 0.0, 3.0, struct.pack("<ii", 7, 8))]})
        no300 = {k: v for k, v in self.base.items() if k != 300}
        c = synth.tae(no300)
        report = Report("chr\\c0000.anibnd.dcx")
        out, _ = merge_tae("a00.tae", vanilla, [("A", a), ("B", b), ("C", c)], report)
        self.assertEqual(report.lines, ["CONFLICT chr\\c0000.anibnd.dcx a00.tae animation 100: A 1 events, B 1 events "
                                        "-> B wins"])
        sizes = arg_sizes(vanilla, [])
        got = read_anims(out, sizes)
        self.assertEqual(sorted(got), [100, 200])
        self.assertEqual(got[100].signature, read_anims(b, sizes)[100].signature)

    def test_tae_unchanged_is_identical(self):
        vanilla = synth.tae(self.base)
        out, taken = merge_tae("", vanilla, [], Report("x"))
        self.assertEqual(out, vanilla)
        self.assertEqual(taken, 0)


HKS_BASE = """-- header comment
function Alpha()
    if x then
        return 1
    end
end

function Beta()
    return 2
end

Gamma = "text -- not a comment"
local function Delta(t)
    local s = [[ long
    string end ]]
    return t
end
"""


class HksTests(unittest.TestCase):
    def merge(self, mods, base_text=HKS_BASE):
        report = Report("action\\script\\c0000.hks")
        out, taken = merge_hks("", "test.hks", base_text.encode("utf-8"), [(m, t.encode("utf-8")) for m, t in mods],
                               report)
        return out.decode("utf-8"), report

    def test_units(self):
        keys = [k for k, _, _ in units(HKS_BASE)]
        self.assertEqual(keys, ["(file start)", "function Alpha", "function Beta", "Gamma", "local function Delta"])
        self.assertEqual("".join(b for _, b, _ in units(HKS_BASE)), HKS_BASE)

    def test_per_function_merge(self):
        a = HKS_BASE.replace("return 1", "return 10") + "\nfunction NewA()\nend\n"
        b = HKS_BASE.replace("return 2", "return 20").replace("-- header comment", "-- other comment")
        out, report = self.merge([("A", a), ("B", b)])
        self.assertIn("return 10", out)
        self.assertIn("return 20", out)
        self.assertIn("function NewA()", out)
        self.assertEqual(report.conflicts, 0)

    def test_function_clash(self):
        a = HKS_BASE.replace("return 2", "return 21")
        b = HKS_BASE.replace("return 2", "return 22\n    -- more")
        out, report = self.merge([("A", a), ("B", b)])
        self.assertIn("return 22", out)
        self.assertNotIn("return 21", out)
        self.assertEqual(report.lines, ["CONFLICT action\\script\\c0000.hks function Beta: A 4 lines, B 5 lines -> "
                                        "B wins"])

    def test_whitespace_and_comments_are_no_change(self):
        a = HKS_BASE.replace("    return 2", "\treturn   2 -- same")
        b = HKS_BASE.replace("return 1", "return 11")
        out, report = self.merge([("A", a), ("B", b)])
        self.assertEqual(report.conflicts, 0)
        self.assertIn("return 11", out)

    def test_compiled_hks_is_refused(self):
        with self.assertRaises(MergeError):
            merge_hks("", "test.hks", None, [("A", b"\x1bLuaQ..."), ("B", b"function X()\nend\n")], Report("x"))


class EndToEndTests(unittest.TestCase):
    """The command line on a synthetic game and mods: output folder, log lines, tables, determinism."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="merger-test-")
        self.game = os.path.join(self.dir, "CUSA03173")
        root = os.path.join(self.game, "dvdroot_ps4")
        vparam = synth.param(TYPE, SIZE, [(1, row(), "a"), (2, row(), "b")])
        synth.write(os.path.join(root, "param", "gameparam", "gameparam.parambnd.dcx"),
                    synth.dcx(synth.bnd([(0, "N:\\p\\Test.param", vparam)])))
        synth.write(os.path.join(root, "paramdef", "paramdef.paramdefbnd.dcx"),
                    synth.dcx(synth.bnd([(0, "N:\\d\\Test.paramdef", synth.paramdef(TYPE, FIELDS))])))
        synth.write(os.path.join(root, "map", "mapstudio", "m10.msb.dcx"), synth.dcx(b"MSB vanilla"))
        self.mods = []
        for name, magic, extra in (("Mod A", 180, {}), ("Mod B", 150, {"only_b.txt": b"b"})):
            folder = os.path.join(self.dir, "mods", name)
            mroot = os.path.join(folder, "dvdroot_ps4")
            param = synth.param(TYPE, SIZE, [(1, row(magic), "a"), (2, row(), "b"), (3 if name == "Mod A" else 4,
                                                                                     row(1), "new")])
            synth.write(os.path.join(mroot, "param", "gameparam", "gameparam.parambnd.dcx"),
                        synth.dcx(synth.bnd([(0, "N:\\p\\Test.param", param)])))
            synth.write(os.path.join(mroot, "map", "mapstudio", "m10.msb.dcx"), synth.dcx(b"MSB " + name.encode()))
            synth.write(os.path.join(mroot, "chr", "c%d.chrbnd.dcx" % magic), b"only one mod ships this")
            manifest = {"id": name.lower().replace(" ", "-"), "requires": "0.3.%d" % (0 if magic == 180 else 3),
                        "features": ["sounds", "weapon_clip_groups"]}
            synth.write(os.path.join(folder, "nightwatch.json"), json.dumps(manifest))
            synth.write(os.path.join(folder, "nightwatch", "sounds.json"), json.dumps(
                {"version": 1, "rows": [{"event": "s000000001", "file": "sound/x.wav"},
                                        {"event": "s00000000%d" % (2 if magic == 180 else 3), "file": "sound/x.wav"}]}))
            synth.write(os.path.join(folder, "sound", "x.wav"), name.encode())
            synth.write(os.path.join(folder, "nightwatch", "weapon_clip_groups.json"), json.dumps(
                {"version": 1, "rows": [{"weapon": 10000, "clip_group": 50 if magic == 180 else 60}]}))
            for rel, data in extra.items():
                synth.write(os.path.join(folder, rel), data)
            self.mods.append(folder)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def run_merge(self, out, mods=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        args = ["merge", "--game", self.game, "--out", out, "--log", out + ".log"]
        for m in (mods or self.mods):
            args += ["--mod", m]
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = merger.main(args)
        return code, stdout.getvalue().splitlines(), stderr.getvalue()

    def test_merge_folder_and_log(self):
        out = os.path.join(self.dir, "mods", "Merged")
        code, lines, err = self.run_merge(out)
        self.assertEqual(code, 0, err)
        self.assertFalse(os.path.exists(out + ".tmp"))
        with open(out + ".log", encoding="utf-8") as f:
            self.assertEqual(f.read().splitlines(), lines)
        text = "\n".join(lines)
        self.assertIn("CONFLICT param\\gameparam\\gameparam.parambnd.dcx Test row 1 (a) attackBaseMagic: Mod A 180, Mod B "
                      "150 -> Mod B wins", text)
        self.assertIn("NOT MERGED map\\mapstudio\\m10.msb.dcx: Mod A, Mod B -> Mod B wins (map layouts are not merged)",
                      text)
        self.assertIn("COPIED chr\\c180.chrbnd.dcx from Mod A", text)
        self.assertIn("MERGED param\\gameparam\\gameparam.parambnd.dcx from 2 mods: 1 inner file changed (1 merged by "
                      "content), 3 rows merged, 1 conflict", text)
        self.assertIn("CONFLICT nightwatch\\weapon_clip_groups.json weapon 10000", text)
        self.assertIn("INFO Mod B: not copied (beside its game files): only_b.txt", text)
        # Both mods' rows for s000000001 read the same, but their sound/x.wav differ: a conflict too.
        self.assertIn('CONFLICT nightwatch\\sounds.json event s000000001: Mod A {"event": "s000000001", "file": '
                      '"sound/x.wav"}, Mod B {"event": "s000000001", "file": "sound/x.wav"} -> Mod B wins', text)
        self.assertEqual(lines[-1], "DONE 4 files, 3 conflicts")
        payload = containers.dcx_read(open(os.path.join(out, "dvdroot_ps4", "param", "gameparam",
                                                        "gameparam.parambnd.dcx"), "rb").read())[0]
        param = containers.bnd4_read(payload)["entries"][0][3]
        self.assertEqual(values(param), [(1, row(150)), (2, row()), (3, row(1)), (4, row(1))])
        manifest = json.load(open(os.path.join(out, "nightwatch.json"), encoding="utf-8"))
        self.assertEqual(manifest["requires"], "0.3.3")
        self.assertEqual(manifest["name"], "Merged")
        sounds = json.load(open(os.path.join(out, "nightwatch", "sounds.json"), encoding="utf-8"))["rows"]
        self.assertEqual(sorted(r["event"] for r in sounds), ["s000000001", "s000000002", "s000000003"])
        files = {r["event"]: r["file"] for r in sounds}
        with open(os.path.join(out, files["s000000002"]), "rb") as f:
            self.assertEqual(f.read(), b"Mod A")
        with open(os.path.join(out, files["s000000001"]), "rb") as f:
            self.assertEqual(f.read(), b"Mod B")
        # The launcher lists it: a dvdroot_ps4 folder in it.
        self.assertTrue(os.path.isdir(os.path.join(out, "dvdroot_ps4")))

    def test_party_host_only_lists_are_merged(self):
        # A boss mod's host-only events must reach the merged mod, else co-op loses them.
        synth.write(os.path.join(self.mods[0], "party_host_only.txt"),
                    b"\xef\xbb\xbf# boss\n12104900\r\n  12204900 # x\n")
        synth.write(os.path.join(self.mods[1], "party_host_only.txt"), b"12204900\n12000001\n")
        out = os.path.join(self.dir, "mods", "Merged")
        code, lines, err = self.run_merge(out)
        self.assertEqual(code, 0, err)
        with open(os.path.join(out, "party_host_only.txt"), encoding="utf-8") as f:
            ids = [line for line in f.read().splitlines() if not line.startswith("#")]
        self.assertEqual(ids, ["12000001", "12104900", "12204900"])
        self.assertIn("MERGED party_host_only.txt from 2 mods: 3 events", lines)
        self.assertFalse(any("not copied" in line and "party_host_only" in line for line in lines), lines)
        # A list the game refuses is left out, as the game leaves it out.
        out2 = os.path.join(self.dir, "mods", "Merged2")
        synth.write(os.path.join(self.mods[1], "party_host_only.txt"), b"12000001\nboss\n")
        code, lines, err = self.run_merge(out2)
        self.assertEqual(code, 0, err)
        with open(os.path.join(out2, "party_host_only.txt"), encoding="utf-8") as f:
            self.assertEqual([x for x in f.read().splitlines() if not x.startswith("#")], ["12104900", "12204900"])

    def test_sound_file_outside_the_mod_is_not_copied(self):
        synth.write(os.path.join(self.dir, "mods", "secret.wav"), b"not the mod's")
        synth.write(os.path.join(self.mods[0], "nightwatch", "sounds.json"), json.dumps(
            {"version": 1, "rows": [{"event": "s000000009", "file": "../secret.wav"}]}))
        out = os.path.join(self.dir, "o4", "deep", "Merged")
        code, lines, err = self.run_merge(out)
        self.assertEqual(code, 0, err)
        self.assertTrue(any(line.startswith("INFO nightwatch\\sounds.json of Mod A: row 1 needs") for line in lines),
                        lines)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "o4", "deep", "secret.wav")))
        self.assertFalse(any(name.endswith(".wav") and "secret" in name for name in tree(out)))

    def write_table(self, mod, feature, table):
        folder = self.mods[mod]
        path = os.path.join(folder, "nightwatch.json")
        manifest = json.load(open(path, encoding="utf-8"))
        if feature not in manifest["features"]:
            manifest["features"].append(feature)
            synth.write(path, json.dumps(manifest))
        synth.write(os.path.join(folder, "nightwatch", feature + ".json"), json.dumps(table))

    def merged_table(self, out, feature):
        with open(os.path.join(out, "nightwatch", feature + ".json"), encoding="utf-8") as f:
            return json.load(f)

    def test_bad_table_row_drops_only_that_row(self):
        # The game drops a mod's whole table for one bad row; the merged table must not carry it, or every mod's
        # rows of that feature would be lost.
        self.write_table(0, "weapon_clip_groups", {"version": 1, "rows": [
            {"weapon": 10000, "clip_group": 50}, {"weapon": 12345, "clip_group": 5}, {"weapon": 20000, "clip_group": 0},
            {"weapon": 30000, "clip_group": True}, {"weapon": 40000, "clip_group": 7}]})
        self.write_table(1, "hit_effects", {"version": 1, "rows": [
            {"attacker_speffect": 1, "base_fx": 2, "fx": 3}, {"sp_attribute": 2, "fx": 1, "sound": 1},
            {"attacker_speffect": 1.0, "base_fx": 2, "fx": 3}]})
        self.write_table(0, "hit_effects", {"version": 2, "rows": [{"attacker_speffect": 9, "base_fx": 9, "fx": 9}]})
        out = os.path.join(self.dir, "mods", "Merged")
        code, lines, err = self.run_merge(out)
        self.assertEqual(code, 0, err)
        clips = self.merged_table(out, "weapon_clip_groups")["rows"]
        self.assertEqual(sorted(r["weapon"] for r in clips), [10000, 40000])
        for number in (2, 3, 4):
            self.assertTrue(any(line.startswith("INFO nightwatch\\weapon_clip_groups.json of Mod A: row %d needs"
                                                % number) for line in lines), lines)
        self.assertEqual(self.merged_table(out, "hit_effects")["rows"],
                         [{"attacker_speffect": 1, "base_fx": 2, "fx": 3}])
        self.assertIn('INFO nightwatch\\hit_effects.json of Mod A cannot be read ("version" is not 1): left out, as '
                      'the game would leave it out', lines)

    def test_key_twice_in_one_table_keeps_the_later_row(self):
        self.write_table(0, "animated_attachments", {"attachments": [
            {"speffect": 5, "part": "hd_a_1"}, {"speffect": 6, "part": "hd_a_2"}, {"speffect": 5, "part": "HD_A_1"}]})
        self.write_table(1, "sounds", {"version": 1, "rows": [
            {"event": "s000000001", "file": "sound/x.wav", "volume": 1.0},
            {"event": "s000000001", "file": "sound/x.wav", "volume": 2.0}]})
        out = os.path.join(self.dir, "mods", "Merged")
        code, lines, err = self.run_merge(out)
        self.assertEqual(code, 0, err)
        self.assertEqual(self.merged_table(out, "animated_attachments")["attachments"],
                         [{"speffect": 6, "part": "hd_a_2"}, {"speffect": 5, "part": "HD_A_1"}])
        self.assertIn("INFO nightwatch\\animated_attachments.json of Mod A: row 3 lists speffect 5 part HD_A_1 a "
                      "second time: the later row is kept", lines)
        sounds = [r for r in self.merged_table(out, "sounds")["rows"] if r["event"] == "s000000001"]
        self.assertEqual([r["volume"] for r in sounds], [2.0])

    def test_merged_table_keeps_the_game_limits_later_mods_first(self):
        self.write_table(0, "animated_attachments", {"attachments": [
            {"speffect": i, "part": "a_%d" % i} for i in range(1, 61)]})
        self.write_table(1, "animated_attachments", {"attachments": [
            {"speffect": i, "part": "b_%d" % i} for i in range(1, 11)]})
        out = os.path.join(self.dir, "mods", "Merged")
        code, lines, err = self.run_merge(out)
        self.assertEqual(code, 0, err)
        rows = self.merged_table(out, "animated_attachments")["attachments"]
        self.assertEqual(len(rows), 64)
        self.assertEqual(rows[0], {"speffect": 7, "part": "a_7"})  # Mod A's first 6 rows left out
        self.assertEqual(rows[-10:], [{"speffect": i, "part": "b_%d" % i} for i in range(1, 11)])
        self.assertIn("INFO nightwatch\\animated_attachments.json: more than the game takes (64 rows, 1 MiB): 6 rows "
                      "of the earliest mods left out (Mod A 6)", lines)

    def test_log_inside_the_output_is_refused(self):
        out = os.path.join(self.dir, "o5", "Merged")
        stderr = io.StringIO()
        args = ["merge", "--game", self.game, "--out", out, "--log", os.path.join(out, "merge.log")]
        for m in self.mods:
            args += ["--mod", m]
        with redirect_stdout(io.StringIO()), redirect_stderr(stderr):
            code = merger.main(args)
        self.assertNotEqual(code, 0)
        self.assertTrue(stderr.getvalue().startswith("ERROR the log file cannot be inside the output folder"),
                        stderr.getvalue())
        self.assertFalse(os.path.exists(out))

    def test_existing_tmp_folder(self):
        out = os.path.join(self.dir, "o6", "Merged")
        # A folder of that name the merger did not make (a mod called "Merged.tmp") is never deleted.
        synth.write(os.path.join(out + ".tmp", "dvdroot_ps4", "keep.txt"), b"the user's")
        code, lines, err = self.run_merge(out)
        self.assertNotEqual(code, 0)
        self.assertTrue(err.startswith("ERROR ") and "was not made by the merger" in err, err)
        self.assertTrue(os.path.isfile(os.path.join(out + ".tmp", "dvdroot_ps4", "keep.txt")))
        # A stopped merge's folder (it holds the merger's mark) is cleared and the merge runs.
        shutil.rmtree(out + ".tmp")
        synth.write(os.path.join(out + ".tmp", merger.TMP_MARK), b"")
        synth.write(os.path.join(out + ".tmp", "half.bin"), b"x")
        code, lines, err = self.run_merge(out)
        self.assertEqual(code, 0, err)
        self.assertFalse(os.path.exists(out + ".tmp"))
        self.assertFalse(os.path.exists(os.path.join(out, "half.bin")))
        self.assertFalse(os.path.exists(os.path.join(out, merger.TMP_MARK)))

    def test_deterministic(self):
        first, second = os.path.join(self.dir, "o1", "Merged"), os.path.join(self.dir, "o2", "Merged")
        self.assertEqual(self.run_merge(first)[0], 0)
        self.assertEqual(self.run_merge(second)[0], 0)
        self.assertEqual(tree(first), tree(second))

    def test_refuses_existing_output(self):
        out = os.path.join(self.dir, "exists")
        os.makedirs(out)
        code, lines, err = self.run_merge(out)
        self.assertNotEqual(code, 0)
        self.assertTrue(err.startswith("ERROR the output folder exists already"), err)

    def test_failure_removes_tmp(self):
        out = os.path.join(self.dir, "o3", "Merged")
        code, lines, err = self.run_merge(out, self.mods + [os.path.join(self.dir, "no such mod")])
        self.assertNotEqual(code, 0)
        self.assertTrue(err.startswith("ERROR "), err)
        self.assertFalse(os.path.exists(out + ".tmp"))
        self.assertFalse(os.path.exists(out))


def tree(folder):
    out = {}
    for here, _, files in os.walk(folder):
        for name in files:
            path = os.path.join(here, name)
            with open(path, "rb") as f:
                out[os.path.relpath(path, folder)] = f.read()
    return out


if __name__ == "__main__":
    unittest.main()
