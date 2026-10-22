"""tools/mp/reaper.py: test games outside the shared game queue, and runs whose harness died."""
from paths import ROOT
import importlib.util
import sys
import unittest

sys.path.insert(0, str(ROOT / "tools" / "mp"))
spec = importlib.util.spec_from_file_location("mp_reaper", ROOT / "tools" / "mp" / "reaper.py")
reaper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reaper)

AG = "E:\\games\\bbport\\agents\\"
GAME = AG + "foo\\mptest\\inst0\\out\\bbport.exe"
SHELL = ("cmd.exe", 1, "C:\\Windows\\system32\\cmd.exe", 'cmd /c "' + AG + 'foo\\mptest\\inst0\\run.bat"')


class UnqueuedTests(unittest.TestCase):
    def procs(self, path=GAME):
        return {100: ("bbport.exe", 50, path, path), 50: SHELL}

    def test_queued_game_is_kept(self):
        state = {}
        holders = [{"root": AG + "foo\\mptest"}]
        self.assertEqual(reaper.plan_unqueued(self.procs(), holders, state, 0.0), [])
        self.assertEqual(reaper.plan_unqueued(self.procs(), holders, state, 100.0), [])
        self.assertNotIn("unq:100", state)

    def test_forward_slashes_and_case(self):
        holders = [{"root": "e:/games/bbport/AGENTS/foo/mptest/"}]
        self.assertEqual(reaper.plan_unqueued(self.procs(), holders, {}, 100.0), [])

    def test_relative_root_of_an_old_queue_copy_matches_as_a_component(self):
        state = {}
        holders = [{"root": "mptest"}]
        reaper.plan_unqueued(self.procs(), holders, state, 0.0)
        self.assertEqual(reaper.plan_unqueued(self.procs(), holders, state, 100.0), [])

    def test_orphan_game_ends_with_its_run_bat_shell_after_the_grace(self):
        # a dead harness: its instances.py's slot was pruned, the games and run.bat still run
        state = {}
        self.assertEqual(reaper.plan_unqueued(self.procs(), [], state, 0.0), [])
        self.assertEqual(reaper.plan_unqueued(self.procs(), [], state, 30.0), [])  # the 30 s grace
        kills = reaper.plan_unqueued(self.procs(), [], state, 31.0)
        # the shell too: ending the game alone lets run.bat's party crash restart boot it again
        self.assertEqual(kills, [(100, GAME, [50])])
        self.assertNotIn("unq:100", state)

    def test_other_root_holding_slots_does_not_cover_it(self):
        state = {}
        holders = [{"root": AG + "foo\\mptest2"}]  # a prefix of the name, not of the path
        reaper.plan_unqueued(self.procs(), holders, state, 0.0)
        self.assertEqual(len(reaper.plan_unqueued(self.procs(), holders, state, 31.0)), 1)

    def test_user_game_and_unknown_paths_are_never_touched(self):
        state = {}
        for path in ("E:\\games\\bloodborne\\out\\bbport.exe", ""):
            reaper.plan_unqueued(self.procs(path), [], state, 0.0)
            self.assertEqual(reaper.plan_unqueued(self.procs(path), [], state, 100.0), [])

    def test_no_shell_when_the_parent_is_not_run_bat(self):
        state = {}
        procs = {100: ("bbport.exe", 7, GAME, GAME), 7: ("python.exe", 1, "C:\\python3\\python.exe", "python x.py")}
        reaper.plan_unqueued(procs, [], state, 0.0)
        self.assertEqual(reaper.plan_unqueued(procs, [], state, 31.0), [(100, GAME, [])])

    def test_a_game_that_ended_by_itself_is_forgotten(self):
        state = {}
        reaper.plan_unqueued(self.procs(), [], state, 0.0)
        reaper.plan_unqueued({}, [], state, 10.0)
        self.assertEqual(state, {})
        # the same pid later (reused): a new grace
        self.assertEqual(reaper.plan_unqueued(self.procs(), [], state, 20.0), [])
        self.assertEqual(reaper.plan_unqueued(self.procs(), [], state, 45.0), [])

    def test_orphaned_holders(self):
        holders = [{"root": "a", "pid": 1, "parent": 11}, {"root": "b", "pid": 2, "parent": 12},
                   {"root": "c", "pid": 3}]
        gone = reaper.orphaned_holders(holders, lambda pid: pid == 12)
        self.assertEqual([e["root"] for e in gone], ["a"])  # no parent recorded: never


if __name__ == "__main__":
    unittest.main()
