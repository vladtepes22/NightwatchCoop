import paths  # noqa: F401
from pathlib import Path
import tempfile
import unittest
from party_launch import apply_relaunch, run_loop


class PartyLaunchTests(unittest.TestCase):
    def test_actual_flow_format_and_removal(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'party_relaunch.bat'
            path.write_text('@rem bbport: next start\nset "BB_PARTY=join"\nset "BB_PARTY_PASSWORD="\nset "BB_PARTY_AUTOCONTINUE=1"\n')
            env = {'BB_PARTY_PASSWORD': 'private'}
            self.assertTrue(apply_relaunch(path, env))
            self.assertEqual(env, {'BB_PARTY': 'join', 'BB_PARTY_AUTOCONTINUE': '1'})
            self.assertFalse(path.exists())

    def test_rejects_whole_file_before_changing_environment(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'party_relaunch.bat'
            for unsafe in ['echo unsafe', 'set "PATH=bad"', 'set "BB_PARTY=x&calc"']:
                path.write_text('set "BB_PARTY=join"\n' + unsafe)
                env = {'BB_PARTY': 'host'}
                with self.assertRaises(ValueError):
                    apply_relaunch(path, env)
                self.assertEqual(env, {'BB_PARTY': 'host'})

    def test_role_change_is_a_new_session(self):
        with tempfile.TemporaryDirectory() as folder:
            env = {'BB_USER_DIR': folder, 'BB_PARTY': 'host', 'BB_PARTY_RESTARTED': '1'}
            Path(folder, 'party_relaunch.bat').write_text('set "BB_PARTY=join"\nset "BB_PARTY_RESTARTED="\n')
            calls = []
            def start():
                calls.append(dict(env))
                return 75 if len(calls) == 1 else 0
            self.assertEqual(run_loop(start, env), 0)
            self.assertEqual(calls[1]['BB_PARTY'], 'join')
            self.assertNotIn('BB_PARTY_RESTARTED', calls[1])

    def test_crash_recovery_keeps_session_and_is_bounded(self):
        env = {'BB_PARTY': 'host', 'BB_PARTY_RESTART_MAX': '2'}
        calls = []
        def start():
            calls.append(dict(env))
            return 23
        self.assertEqual(run_loop(start, env, sleep=lambda _: None, clock=lambda: 0), 23)
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls[1]['BB_PARTY_RESTARTED'], '1')

    def test_off_mode_does_not_restart_a_failure(self):
        for mode in ['', '0', 'off', 'FALSE']:
            self.assertEqual(run_loop(lambda: 17, {'BB_PARTY': mode}), 17)
