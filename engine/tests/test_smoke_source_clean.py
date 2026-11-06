"""Regression: a raw 0x01 byte in a smoke regex stands where a backreference (backslash-1) was meant and never matches."""
from paths import ROOT
import re
import unittest

MP = ROOT / "tools" / "mp"


class SmokeSourceClean(unittest.TestCase):
    def test_no_control_bytes_in_harness_sources(self):
        for f in sorted(MP.glob("*.py")):
            for n, line in enumerate(f.read_bytes().split(b"\n"), 1):
                bad = re.findall(rb"[\x00-\x08\x0b\x0c\x0e-\x1f]", line)
                self.assertFalse(bad, "%s:%d holds a control byte %r (a backreference typed without its backslash?)" % (f.name, n, bad[:1]))

    def test_until_regexes_backreference_matches(self):
        import importlib.util, sys
        sys.path.insert(0, str(MP))
        spec = importlib.util.spec_from_file_location("mp_smoke_src", MP / "smoke_party.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        done = "0:travel step 5/5: host_press"
        self.assertTrue(re.search(mod.FDR_UNTIL, done))
        self.assertFalse(re.search(mod.FDR_UNTIL, "0:travel step 4/5: x"))


if __name__ == "__main__":
    unittest.main()
