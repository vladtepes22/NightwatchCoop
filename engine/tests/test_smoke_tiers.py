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

    def test_fast_tier_is_short_and_exists(self):
        names = smoke.phase_list_for(args(tier="fast"))
        self.assertEqual(names, smoke.FAST_PHASES.split(","))
        self.assertFalse([p for p in names if p not in smoke.PHASES])
        self.assertEqual(len(names), len(set(names)))
        seed = json.loads(smoke.SEED_TIMES.read_text())
        # the point of the tier: under an hour on the seed times (measured 0.7.8 walls), no phase over 5 min
        self.assertLess(sum(seed[p] for p in names), 3600)
        self.assertLess(max(seed[p] for p in names), 300)

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


class StuckTests(unittest.TestCase):
    def test_every_phase_has_a_stuck_limit_inside_its_cap(self):
        for name, mk in smoke.PHASES.items():
            try:
                ph = mk(set())
            except AssertionError as e:
                # Nightwatch: our System menu pages differ from visy's (rules walk RULES_PAGES); his smoke_party.py is
                # his gate, not ours, so a phase that walks his menu layout is left out here.
                if "RULES_PAGES out of step" in str(e):
                    continue
                raise
            self.assertTrue(0 < smoke.stuck_seconds(ph) <= max(ph["seconds"], 150), name)

    def test_long_chain_delay_raises_the_limit(self):
        ph = dict(name="x", seconds=900, env=[], ienv=["0:BB_PARTY_DIRECTOR_TEST=host_press=0@3,host_press=0@200"])
        self.assertEqual(smoke.stuck_seconds(ph), 1.5 * 200 + 90)
        ph["ienv"] = ["0:BB_PARTY_DIRECTOR_TEST=host_press=0@3"]
        self.assertEqual(smoke.stuck_seconds(ph), 150)

    def test_instances_run_uses_the_stuck_watch_under_its_own_name(self):
        # 0.7.9: `watch` is the crash plan's loop variable in run(); a clash crashed every scripted run (s2, twice)
        import re
        src = (MP / "instances.py").read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"^\s+if watch:", src, re.M))
        self.assertNotIn("watch.feed(", src.replace("stuck_watch.feed(", ""))

    def test_watch_progress_dead_and_stuck(self):
        import instances
        w = instances.StuckWatch(2, 100, 50)
        w.feed(0, "[+ 1.0] [coop] Party: test: target pos: entity 1 at (0,0,0)\n")
        w.at -= 120
        self.assertIn("STUCK", w.verdict())  # polling lines are no progress
        w.feed(0, "[+ 1.0] [coop] Party: test: target pos: entity 1 at (0,0,0)\n[+ 2.0] x test: travel step 2/5: guest_press0 (world up)\n")
        self.assertIsNone(w.verdict())
        w.feed(1, "[+ 3.0] x test: travel step 3 (guest_press) waits: the own chr is dead\n")
        w.dead_at -= 60
        self.assertIn("DEAD", w.verdict())
        w.feed(1, "[+ 3.0] x test: travel step 3 (guest_press) waits: the own chr is dead\n[+ 4.0] Party: loading screen up\n")  # a load is progress: the wait is over
        self.assertIsNone(w.verdict())


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

    def test_harness_change_runs_the_fast_tier(self):
        names, _ = self.run_diff(["tools/mp/smoke_party.py"])
        self.assertEqual(set(names), set(smoke.FAST_PHASES.split(",")))


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
        self.alive_mock = self.alive.start()

    def tearDown(self):
        self.alive.stop()
        gq.QUEUE_FILE = self.old
        self.tmp.cleanup()

    PARENT = 424242  # fake BB_MP_PARENT_PID: game_slots leaves its wait loop when this pid is "dead"

    def got(self, memguard, free):
        """Run game_slots(3) in a thread against the queue file and report whether it was granted.

        No sleeps decide the result. The thread's time.sleep is replaced by a shim that signals the end of one
        full queue evaluation (the grant decision is made before the sleep), so "not granted" is concluded after
        a few complete passes; "granted" waits on an event. A waiter that is not granted is released through the
        parent-pid exit of game_slots and the thread is joined, so no daemon thread outlives the test and keeps
        polling the next test's queue file (that leak was the flake: with the _alive / QUEUE_FILE patches of the
        next test not yet in place, the old waiter pruned the next test's holder and took its slot)."""
        granted, passes, stop = threading.Event(), threading.Semaphore(0), threading.Event()
        real = gq.time

        def fake_sleep(_s):
            passes.release()
            stop.wait(0.005)

        shim = types.SimpleNamespace(time=real.time, strftime=real.strftime, sleep=fake_sleep)
        self.alive_mock.side_effect = lambda pid: not (pid == self.PARENT and stop.is_set())

        def run():
            try:
                with gq.game_slots(3, "root"):
                    granted.set()
                    passes.release()
            except BaseException:
                pass
            finally:
                passes.release()  # also wakes the test when the thread ended without a grant

        env = {"BB_GAME_QUEUE_MEMGUARD": "1" if memguard else "0", "BB_MP_PARENT_PID": str(self.PARENT)}
        with mock.patch.dict(gq.os.environ, env), mock.patch.object(gq, "free_ram_mb", return_value=free),                 mock.patch.object(gq, "time", shim):
            t = threading.Thread(target=run, daemon=True)
            t.start()
            try:
                for _ in range(3):  # three complete evaluations, or the grant / the end of the thread
                    if not passes.acquire(timeout=30) or granted.is_set():
                        break
                ok = granted.is_set()
            finally:
                stop.set()
                t.join(30)
        self.assertFalse(t.is_alive(), "game_slots thread did not end")
        return ok

    def test_guarded_job_starts_beside_a_holder_with_room(self):
        self.assertTrue(self.got(True, 40000))

    def test_guarded_job_waits_without_room(self):
        self.assertFalse(self.got(True, 20000))

    def test_unguarded_job_waits_behind_the_holder(self):
        self.assertFalse(self.got(False, 40000))


if __name__ == "__main__":
    unittest.main()


def test_only_runs_every_phase_of_its_groups(monkeypatch):
    """--only <group> runs all of that group's phases, also ones the ordered list in main() does not name."""
    import smoke_party as sp
    seen = {}

    def capture(a):
        seen["phase"] = a.phase
        raise SystemExit(0)

    monkeypatch.setattr(sp, "phase_list_for", capture)
    for group in ("six_join", "lift_walk", "invade_two"):
        seen.clear()
        try:
            monkeypatch.setattr(sys, "argv", ["smoke_party.py", "--only", group, "--no-shots", "--game", "E:/nonexistent_game"])
            sp.main()
        except SystemExit:
            pass
        assert set(seen.get("phase", "").split(",")) >= set(sp.GROUPS[group].split(",")), group
