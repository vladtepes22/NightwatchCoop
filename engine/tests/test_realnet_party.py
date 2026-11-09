"""tools/mp/realnet_party.py: phase table, IPv4 masking, public-path checks on synthetic logs. No game, no network."""
from paths import ROOT
import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path

MP = ROOT / "tools" / "mp"
sys.path.insert(0, str(MP))
spec = importlib.util.spec_from_file_location("mp_realnet_party", MP / "realnet_party.py")
rn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rn)

A = types.SimpleNamespace(guest2_headless=False)


class PhaseTests(unittest.TestCase):
    def test_registered(self):
        self.assertEqual(list(rn.CHECKS), ["direct", "relay", "invade", "invade2"])

    def test_invade_is_two_rendered_games_with_a_limit(self):
        ph = rn.phase_invade(A, 5000)
        self.assertEqual((ph["count"], ph["headless"], ph["ports"]), (2, 0, (5000, 5001)))
        self.assertTrue(0 < ph["seconds"] <= 900)
        self.assertIn("1:BB_PARTY_INVADE=1", ph["ienv"])
        self.assertIn("0:BB_PARTY_INVADERS=1", ph["ienv"])
        self.assertIn("1:BB_PARTY_LAN_DISCOVERY=0", ph["ienv"])  # the VPN guest never takes the LAN
        self.assertTrue(ph["until"] and ph["until_hold"])

    def test_invade2_needs_max_invaders_2_and_a_lan_invader(self):
        ph = rn.phase_invade2(A, 5000)
        self.assertEqual((ph["count"], ph["headless"]), (3, 0))
        self.assertIn("0:BB_PARTY_MAX_INVADERS=2", ph["ienv"])
        self.assertIn("2:BB_PARTY_INVADE=1", ph["ienv"])
        self.assertNotIn("2:BB_PARTY_LAN_DISCOVERY=0", ph["ienv"])
        self.assertEqual(rn.phase_invade2(types.SimpleNamespace(guest2_headless=True), 1)["headless"], 1)


class MaskTests(unittest.TestCase):
    def test_mask(self):
        m = rn.mask_ipv4
        self.assertEqual(m("seen at 203.0.113.9:4000, x 10.1.2.3"), "seen at 203.0.x.x:4000, x 10.1.x.x")
        self.assertEqual(m("rate 0.50, v1.2.3"), "rate 0.50, v1.2.3")
        self.assertEqual(m(None), None)

    def test_masked_out_and_verdict(self):
        import io
        buf = io.StringIO()
        rn.MaskedOut(buf).write("addr 198.51.100.7\n")
        self.assertEqual(buf.getvalue(), "addr 198.51.x.x\n")
        v = rn.MaskedVerdict()
        v.step("t", True, "from 198.51.100.7")
        self.assertEqual(v.rows[0][2], "from 198.51.x.x")

    def test_mask_files(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "harness.log").write_text("a 192.0.2.44 b", encoding="utf-8")
            rn.mask_files(d)
            self.assertEqual((Path(d) / "harness.log").read_text(encoding="utf-8"), "a 192.0.x.x b")

    def test_private(self):
        for ip in ("192.168.1.5", "10.0.0.1", "172.20.1.1", "127.0.0.1", "169.254.1.1"):
            self.assertTrue(rn.private_ipv4(ip), ip)
        for ip in ("203.0.113.9", "172.32.0.1", "8.8.8.8"):
            self.assertFalse(rn.private_ipv4(ip), ip)


def logs(host, g1, g2=()):
    d = tempfile.mkdtemp()
    for i, ls in enumerate((host, g1, g2)):
        (Path(d) / ("inst%d.log" % i)).write_text("\n".join("[+ %.1f] %s" % (k + 1.0, l) for k, l in enumerate(ls)), encoding="utf-8")
    return rn.sp.Logs(Path(d))


class PathTests(unittest.TestCase):
    HOST = ["Party: public address 203.0.113.9:4000 (stun x)"]

    def run_checks(self, g1):
        L = logs(self.HOST, g1)
        v = rn.MaskedVerdict()
        rn.public_path_checks(v, L, "invade")
        return {r[0].split(" ", 1)[1]: r[1] for r in v.rows}

    def test_public_path_passes(self):
        r = self.run_checks(["Party: connected to the host as slot 1 (we are seen at 198.51.100.7:5000)",
                             "Party: network path: party link over UDP (the host's NAT let us in or punched towards us)"])
        self.assertTrue(all(r.values()), r)

    def test_lan_path_fails(self):
        r = self.run_checks(["Party: LAN fallback: found host at 192.168.1.2:4000 (x)",
                             "Party: connected to the host as slot 1 (we are seen at 192.168.1.9:5000)",
                             "Party: network path: party link over UDP (the host's NAT let us in or punched towards us)"])
        self.assertFalse(r["invader: no LAN fallback (it did not find the host on the LAN)"])
        self.assertFalse(r["invader: the host's view of it is a public address (not LAN / loopback)"])

    def test_relay_path_fails(self):
        r = self.run_checks(["Party: connected to the host as slot 1 (we are seen at 198.51.100.7:5000)",
                             "Party: network path: party link over UDP through Hunter2's relay (x): the host cannot be reached directly"])
        self.assertFalse(r["invader: no member relay on its path"])


class CrashRowTests(unittest.TestCase):
    def test_crash_rx_catches_host_fault_and_exit(self):
        import re
        sys.path.insert(0, str(MP))
        import smoke_party as sp
        rx = re.compile(sp.CRASH_RX)
        self.assertTrue(rx.search("Fault at RIP 00000001403d2ba8, address ffffffffffffffff"))
        self.assertTrue(rx.search("Party: the game exited with 139 (a crash); restart 1 of 5"))
        self.assertFalse(rx.search("Party: the game exited with 0"))
        self.assertFalse(rx.search("invader: its cooperator look is back"))


class CheckOnlyTests(unittest.TestCase):
    def test_check_only_sets_up_and_launches_nothing(self):
        from unittest import mock
        boom = mock.Mock(side_effect=AssertionError("must not be called"))
        seen = []

        def fake_check(v, L):
            seen.append(L)
            v.step("fake", True, "ok")

        with tempfile.TemporaryDirectory() as d:
            root, out = Path(d) / "root", Path(d) / "out"
            (out / "relay").mkdir(parents=True)
            argv = ["realnet_party.py", "--check-only", "--phase", "relay", "--root", str(root),
                    "--out", str(out), "--game", str(Path(d) / "game")]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(rn.subprocess, "run", boom),                     mock.patch.object(rn, "phase_direct", boom), mock.patch.object(rn.sp, "run_phase", boom),                     mock.patch.object(rn.instances, "stun_public_ipv4", boom), mock.patch.object(rn, "upnp", boom),                     mock.patch.object(rn, "VpnGuard", boom), mock.patch.object(rn, "mask_files", boom),                     mock.patch.dict(rn.CHECKS, {"relay": fake_check}),                     mock.patch.object(sys, "stdout", sys.stdout), mock.patch.object(sys, "stderr", sys.stderr):
                with self.assertRaises(SystemExit) as cm:
                    rn.main()
            self.assertEqual(cm.exception.code, 0)
            self.assertEqual(len(seen), 1)
            self.assertFalse(root.exists())  # no instance root was created
            self.assertIn("PASS  fake", (out / "verdict.txt").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
