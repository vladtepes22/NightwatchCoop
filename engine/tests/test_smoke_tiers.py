"""tools/mp/smoke_party.py tiers, --changed-since mapping, progress / ETA and the game queue's memory guard: no game needed."""
from paths import ROOT
import importlib.util
import json
import sys
import tempfile
import threading
import types
import unittest
from pathlib import Path
from unittest import mock

MP = ROOT / "tools" / "mp"
sys.path.insert(0, str(MP))


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, MP / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


smoke = load("mp_smoke_tiers", "smoke_party.py")
gq = load("mp_gq_tiers", "game_queue.py")


def args(**kw):
    d = dict(phase=None, tier=None, release=False, changed_since=None)
    d.update(kw)
    return types.SimpleNamespace(**d)


class TierTests(unittest.TestCase):
    def test_default_is_the_quick_list_and_it_is_named(self):
        names = smoke.phase_list_for(args())
        self.assertEqual(names, smoke.QUICK_PHASES.split(","))
        self.assertEqual(len(names), 40)

    def test_release_flag_is_every_phase(self):
        self.assertEqual(smoke.phase_list_for(args(release=True)), list(smoke.PHASES))

    def test_release_tier_only_drops_listed_variants(self):
        rel = smoke.phase_list_for(args(tier="release"))
        self.assertEqual(set(smoke.PHASES) - set(rel), set(smoke.RELEASE_SKIP))
        for skipped in smoke.RELEASE_SKIP:
            self.assertIn(skipped, smoke.PHASES)

    def test_every_skip_names_a_phase_that_stays(self):
        for skipped, why in smoke.RELEASE_SKIP.items():
            sibling = why.split()[0]
            self.assertTrue(sibling in smoke.PHASES and sibling not in smoke.RELEASE_SKIP, (skipped, why))

    def test_explicit_phase_wins_and_unknown_is_refused(self):
        self.assertEqual(smoke.phase_list_for(args(phase="cv, da", release=True)), ["cv", "da"])
        with self.assertRaises(SystemExit):
            smoke.phase_list_for(args(phase="cv,nope"))

    def test_quick_names_all_exist(self):
        self.assertFalse([p for p in smoke.QUICK_PHASES.split(",") if p not in smoke.PHASES])

    def test_changed_map_phases_exist(self):
        for rx, phases in smoke.CHANGED_MAP:
            self.assertFalse([p for p in phases.split(",") if p not in smoke.PHASES], rx)

    def test_seed_times_cover_every_phase(self):
        seed = json.loads(smoke.SEED_TIMES.read_text())
        self.assertFalse(set(smoke.PHASES) - set(seed))


class ChangedSinceTests(unittest.TestCase):
    def run_diff(self, files):
        out = types.SimpleNamespace(returncode=0, stdout="\n".join(files), stderr="")
        with mock.patch.object(smoke.subprocess, "run", return_value=out):
            return smoke.phases_changed_since("v0")

    def test_docs_and_tests_run_nothing(self):
        names, _ = self.run_diff(["docs/RELEASE_CHECKLIST.md", "tests/test_x.py", "CHANGES.md", "launcher/a.py"])
        self.assertEqual(names, [])

    def test_mapped_file_runs_its_phases(self):
        names, notes = self.run_diff(["gpu/shim/party/party_lift_rules.h"])
        self.assertIn("lfw", names)
        self.assertNotIn("cv", names)
        self.assertTrue(notes)

    def test_unmapped_game_file_runs_the_quick_tier(self):
        names, _ = self.run_diff(["gpu/shim/party/party_rudp.cpp"])
        self.assertEqual(set(names), set(smoke.QUICK_PHASES.split(",")))

    def test_harness_change_runs_the_quick_tier(self):
        names, _ = self.run_diff(["tools/mp/smoke_party.py"])
        self.assertEqual(set(names), set(smoke.QUICK_PHASES.split(",")))


class ProgressTests(unittest.TestCase):
    def test_eta_counts_down_over_lanes(self):
        p = smoke.Progress(["a", "b", "c"], {"a": 100, "b": 200}, lanes=2)
        self.assertEqual(p.eta(), (100 + 200 + 300) / 2)  # c unknown: 300 s
        with mock.patch("builtins.print"):
            p.finish("b", 50)
        self.assertEqual(p.eta(), (100 + 300) / 2)

    def test_hms(self):
        self.assertEqual(smoke.fmt_hms(3725), "1:02:05")


class MemGuardTests(unittest.TestCase):
    """A guarded job may exceed max_clients while the free RAM / commit covers it; an unguarded one never."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.old = gq.QUEUE_FILE
        gq.QUEUE_FILE = Path(self.tmp.name) / "game_queue.json"
        gq.QUEUE_FILE.write_text(json.dumps({"max_clients": 2, "max_solo": 3, "max_mem_clients": 6,
                                            "holders": [{"id": "h", "pid": 0, "count": 3, "t": 0, "root": "r", "since": "0", "slots": [0, 1, 2]}],
                                            "queue": []}))
        self.alive = mock.patch.object(gq, "_alive", return_value=True)
        self.alive.start()

    def tearDown(self):
        self.alive.stop()
        gq.QUEUE_FILE = self.old
        self.tmp.cleanup()

    def got(self, memguard, free, timeout=3):
        done = []

        def run():
            with mock.patch.dict(gq.os.environ, {"BB_GAME_QUEUE_MEMGUARD": "1" if memguard else "0"}), \
                    mock.patch.object(gq, "free_ram_mb", return_value=free):
                try:
                    with gq.game_slots(3, "root"):
                        done.append(1)
                        return
                except BaseException:
                    return
        t = threading.Thread(target=run, daemon=True)
        t.start()
        t.join(timeout)
        return bool(done)

    def test_guarded_job_starts_beside_a_holder_with_room(self):
        self.assertTrue(self.got(True, 40000))

    def test_guarded_job_waits_without_room(self):
        self.assertFalse(self.got(True, 20000))

    def test_unguarded_job_waits_behind_the_holder(self):
        self.assertFalse(self.got(False, 40000))


if __name__ == "__main__":
    unittest.main()
