"""The generator's own build id (buildid.py): part of every world's input hash, so another generator build never shares a
world folder name or a lobby check with this one. Pure tests: no game files."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
sys.path.insert(0, TOOL)
import buildid


def _copy_sources(to):
    for name in os.listdir(TOOL):
        if name.endswith(".py") and os.path.isfile(os.path.join(TOOL, name)):
            shutil.copyfile(os.path.join(TOOL, name), os.path.join(to, name))


class BuildId(unittest.TestCase):
    def test_counted_modules(self):
        names = buildid.modules()
        self.assertIn("generate.py", names)
        self.assertIn("buildid.py", names)
        self.assertIn("randomizer.py", names)
        self.assertEqual(names, sorted(names))
        self.assertFalse([n for n in names if n.startswith("_")])

    def test_stable_and_changes_with_any_module(self):
        with tempfile.TemporaryDirectory() as tmp:
            _copy_sources(tmp)
            first = buildid.compute(tmp)
            self.assertRegex(first, "^[0-9a-f]{64}$")
            self.assertEqual(buildid.compute(tmp), first)
            self.assertEqual(buildid.compute(tmp), buildid.compute(TOOL))  # the same files anywhere: the same id
            with open(os.path.join(tmp, "planner.py"), "ab") as f:
                f.write(b"# a comment\n")
            changed = buildid.compute(tmp)
            self.assertNotEqual(changed, first)
            os.remove(os.path.join(tmp, "planner.py"))
            self.assertNotIn(buildid.compute(tmp), (first, changed))  # a module gone
            _copy_sources(tmp)
            self.assertEqual(buildid.compute(tmp), first)
            with open(os.path.join(tmp, "extra.py"), "wb") as f:
                f.write(b"X = 1\n")
            self.assertNotEqual(buildid.compute(tmp), first)  # a module added
            os.remove(os.path.join(tmp, "extra.py"))
            shutil.copyfile(os.path.join(TOOL, "planner.py"), os.path.join(tmp, "renamed.py"))
            os.remove(os.path.join(tmp, "planner.py"))
            self.assertNotEqual(buildid.compute(tmp), first)  # names count, not only contents

    def test_left_out_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            _copy_sources(tmp)
            first = buildid.compute(tmp)
            for name in ("_notes.py", "_scratch.py"):
                with open(os.path.join(tmp, name), "w", encoding="ascii") as f:
                    f.write("KEY = b'x'\n")
            os.makedirs(os.path.join(tmp, "tests"))
            with open(os.path.join(tmp, "tests", "test_more.py"), "w", encoding="ascii") as f:
                f.write("pass\n")
            os.makedirs(os.path.join(tmp, "__pycache__"))
            with open(os.path.join(tmp, "notes.md"), "w", encoding="ascii") as f:
                f.write("# build\n")
            self.assertEqual(buildid.compute(tmp), first)

    def test_crlf_checkout_gives_the_same_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            _copy_sources(tmp)
            first = buildid.compute(tmp)
            for name in buildid.modules(tmp):
                path = os.path.join(tmp, name)
                with open(path, "rb") as f:
                    data = f.read()
                with open(path, "wb") as f:
                    f.write(data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
            self.assertEqual(buildid.compute(tmp), first)

    def test_no_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(buildid.compute(tmp))
            self.assertIsNone(buildid.compute(os.path.join(tmp, "missing")))

    def test_build_id_command(self):
        """`randomizer.py build-id` prints the id of the sources it runs from (what the launcher asks a package for)."""
        with tempfile.TemporaryDirectory() as tmp:
            _copy_sources(tmp)
            run = subprocess.run([sys.executable, "-I", "-B", os.path.join(tmp, "randomizer.py"), "build-id"],
                                 capture_output=True, text=True, cwd=tmp, timeout=120)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(run.stdout.strip(), buildid.compute(TOOL))


if __name__ == "__main__":
    unittest.main()
