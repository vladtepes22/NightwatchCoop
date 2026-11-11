"""No unmasked network address in a log (0.8.1, NW-019b146).

The rule (scripts/ipmask.py, src/runtime_ipmask.c): 1.2.3.4 -> 1.2.x.x, IPv6 -> its first two groups.
Allowed unmasked: loopback (127.x, ::1), 0.0.0.0, 255.255.255.255, and nothing else. The smoke harness
runs every instance on 127.0.0.1 / 127.0.0.2, so a smoke run has no real peer address to hide; any other
address in its logs is a leak. (A real-net run, tools/mp/realnet_party.py, sets BB_LOG_RAW_IPS=1 on purpose
and is not scanned.)

The scan test reads the logs of the newest smoke run it finds (or the folders in BB_IPSCAN_DIRS, a
os.pathsep list) and skips when there is none.
"""
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

from paths import ROOT, build_hint, test_exe

sys.path.insert(0, str(ROOT / "scripts"))
import ipmask  # noqa: E402
import net_doctor  # noqa: E402

VECTORS = ROOT / "tests" / "ipmask_vectors.txt"
C_EXE = test_exe("ipmask-test")


def vectors():
    rows = []
    for line in VECTORS.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#") and "\t" in line:
            rows.append(tuple(line.split("\t", 1)))
    return rows


class MaskRule(unittest.TestCase):
    def test_vectors(self):
        for src, want in vectors():
            self.assertEqual(ipmask.mask_ips(src), want, src)
            self.assertEqual(ipmask.mask_ips(want), want, "idempotent: " + want)

    def test_unmasked_ips_lists_only_real_addresses(self):
        self.assertEqual(ipmask.unmasked_ips("a 127.0.0.2:9307 b ::1 c 0.0.0.0 d 19:49:49"), [])
        self.assertEqual(ipmask.unmasked_ips("x 192.168.4.5:1 y 2001:db8::7"), ["192.168.4.5", "2001:db8::7"])

    @unittest.skipUnless(C_EXE.exists(), build_hint("ipmask-test"))
    def test_c_rule_is_the_same(self):
        r = subprocess.run([str(C_EXE), str(VECTORS)], capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("ipmask-test: OK", r.stdout)


class NetDoctorMasks(unittest.TestCase):
    LOG = (
        "Nightwatch Co-op v0.2.6 beta 4, 2026-11-01 12:28:15\n"
        "[coop] Net: party networking on (guest, as Gx, port 9307)\n"
        "[coop] Net: local address 192.168.7.20 on Ethernet; candidates: 26.4.5.6 Radmin VPN / Radmin\n"
        "[coop] Party: joining 203.0.113.9:9307 as Gx (Internet code)\n"
        "[coop] Net: send 4 -> 198.51.100.4:9307 vport 1->2: 40 bytes -> 40\n"
        "[coop] Net: this PC is [2001:db8:1:2::5]:9307\n"
    )

    def run_doctor(self, tmp_path, *flags):
        p = Path(tmp_path) / "last_session.log"
        p.write_text(self.LOG, encoding="utf-8")
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            net_doctor.main(list(flags) + ["--evidence", str(p)])
        return buf.getvalue()

    def test_redactips_output_has_no_address(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            for flags in (("--redactips",), ("--redactnames", "--redactips")):
                out = self.run_doctor(tmp, *flags)
                self.assertEqual(ipmask.unmasked_ips(out), [], out)
                self.assertNotIn("192.168.7.20", out)

    def test_plain_output_is_the_players_own(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn("192.168.7.20", self.run_doctor(tmp))  # the local run keeps the real address for the advice

    def test_masked_logs_still_classify(self):
        self.assertTrue(net_doctor.is_vpn_ip("26.4.x.x"))
        self.assertTrue(net_doctor.is_vpn_ip("100.100.x.x"))
        self.assertTrue(net_doctor.is_public("203.0.x.x"))
        self.assertFalse(net_doctor.is_public("192.168.x.x"))
        self.assertEqual(net_doctor.vpn_kind("25.1.x.x"), "Hamachi")


class LauncherMasks(unittest.TestCase):
    def test_launcher_lines_pass_the_mask(self):
        import importlib.util
        try:
            spec = importlib.util.spec_from_file_location("bb_launcher_ipmask", ROOT / "launcher.py")
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
        except Exception as ex:  # no tkinter here
            self.skipTest("launcher.py not importable here: %s" % ex)

        class Var:
            def __init__(self, v):
                self.v = v

            def get(self):
                return self.v

        class Fake:
            feat_log_raw_ips = Var(False)

        lines = ["send 4 -> 10.9.8.7:9307", "peer [2001:db8:a:b::1]:9307 and 127.0.0.1"]
        got = mod.BloodborneLauncher.mask_lines(Fake(), lines) if hasattr(mod, "BloodborneLauncher") else None
        if got is None:
            self.skipTest("launcher class not found")
        self.assertEqual(got, ["send 4 -> 10.9.x.x:9307", "peer [2001:db8:x:x:x:x:x:x]:9307 and 127.0.0.1"])
        Fake.feat_log_raw_ips = Var(True)
        self.assertEqual(mod.BloodborneLauncher.mask_lines(Fake(), lines), lines)


def smoke_log_files():
    dirs = os.environ.get("BB_IPSCAN_DIRS")
    if dirs:
        roots = [Path(d) for d in dirs.split(os.pathsep) if d]
    else:
        # the harness's run folders: <agent root>/smoke_runs/<stamp>/<phase>/ (newest stamp), inst*/run.log
        agent = ROOT.parent
        runs = sorted((agent / "smoke_runs").glob("*")) if (agent / "smoke_runs").is_dir() else []
        roots = runs[-1:]
    files = []
    for r in roots:
        for pat in ("**/run.log", "**/inst*.log", "**/*.log"):
            files.extend(p for p in r.glob(pat) if p.is_file())
    return sorted(set(files))


class SmokeLogsHoldNoAddress(unittest.TestCase):
    def test_scan(self):
        files = smoke_log_files()
        if not files:
            self.skipTest("no smoke run logs here (set BB_IPSCAN_DIRS to a smoke run folder)")
        leaks = []
        for p in files:
            if "realnet" in str(p).lower():
                continue  # BB_LOG_RAW_IPS=1 by design, the logs stay on that PC
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                for n, line in enumerate(f, 1):
                    bad = ipmask.unmasked_ips(line)
                    if bad:
                        leaks.append("%s:%d: %s" % (p, n, ", ".join(ipmask.mask_ips(b) + " (unmasked)" for b in bad)))
                        if len(leaks) > 20:
                            break
            if len(leaks) > 20:
                break
        self.assertEqual(leaks, [], "unmasked addresses in smoke logs:\n" + "\n".join(leaks[:20]))


if __name__ == "__main__":
    unittest.main()
