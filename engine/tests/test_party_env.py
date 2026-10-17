"""The shared BB_PARTY on/off rule: scripts/party_env.py, its users in scripts/patches.py and the
run.bat block that clears an off value before the patch compiler, the game and the crash-restart
loop read it (gpu/party_env.h is the C/C++ side, party-env-test)."""
from paths import ROOT
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import party_env
import patches

OFF = [None, '', ' ', '\t', '0', ' 0 ', 'off', 'OFF', 'Off', 'false', 'FALSE', 'no', 'No', ' no ']
ON = ['host', 'join', 'guest', '1', 'HOST', ' join ', 'BBP1-ABCDEF', '127.0.0.1:47000', 'yes', 'on', 'true']


def env_with(value):
    env = {'PATH': os.environ.get('PATH', '')}
    if value is not None:
        env['BB_PARTY'] = value
    return env


class PartyEnvRule(unittest.TestCase):
    def test_values(self):
        for v in OFF:
            self.assertFalse(party_env.party_value_on(v), repr(v))
            self.assertFalse(party_env.party_enabled(env_with(v)), repr(v))
        for v in ON:
            self.assertTrue(party_env.party_value_on(v), repr(v))
            self.assertTrue(party_env.party_enabled(env_with(v)), repr(v))

    def test_default_on_switch(self):
        self.assertTrue(party_env.party_switch_on({}, 'BB_PARTY_FAST_JOIN'))
        self.assertTrue(party_env.party_switch_on({'BB_PARTY_FAST_JOIN': ' '}, 'BB_PARTY_FAST_JOIN'))
        self.assertTrue(party_env.party_switch_on({'BB_PARTY_FAST_JOIN': '1'}, 'BB_PARTY_FAST_JOIN'))
        for v in ('0', 'off', ' No ', 'FALSE'):
            self.assertFalse(party_env.party_switch_on({'BB_PARTY_FAST_JOIN': v}, 'BB_PARTY_FAST_JOIN'), v)

    def test_same_as_the_c_header(self):
        # The C header lists the same off words.
        header = (ROOT / 'gpu' / 'party_env.h').read_text(encoding='utf-8')
        for word in party_env.OFF_VALUES[1:]:
            self.assertIn(f'"{word}"', header)


class PatchesFollowTheRule(unittest.TestCase):
    def test_party_mode_and_patches(self):
        for v in OFF:
            env = env_with(v)
            self.assertFalse(patches.party_mode(env), repr(v))
            self.assertEqual(patches.party_patches(env), [], repr(v))
            # Off: the offline title skip, never the online one.
            self.assertEqual(patches.network_choice_patch(env), patches.SKIP_NETWORK_CHOICE, repr(v))
        for v in ON:
            env = env_with(v)
            self.assertTrue(patches.party_mode(env), repr(v))
            self.assertIn('Party: SOS sign timeout 30s', patches.party_patches(env))
            self.assertEqual(patches.network_choice_patch(env), patches.SKIP_NETWORK_CHOICE_ONLINE, repr(v))


@unittest.skipUnless(sys.platform == 'win32', 'run.bat needs cmd.exe')
class RunBatClearsOffValues(unittest.TestCase):
    def block(self):
        lines = (ROOT / 'run.bat').read_text(encoding='utf-8').splitlines()
        start = lines.index('if not defined BB_PARTY goto party_env_done')
        end = lines.index(':party_env_done')
        # Before anything reads BB_PARTY (the patch compiler, the game, the restart loop).
        self.assertLess(end, next(i for i, l in enumerate(lines) if 'scripts\\run_windows.py' in l and l.startswith('"%BB_PYTHON%"')))
        return lines[start:end + 1]

    def run_block(self, value):
        with tempfile.TemporaryDirectory() as d:
            bat = Path(d) / 't.bat'
            bat.write_text('\r\n'.join(['@echo off', 'setlocal', *self.block(),
                                        'if defined BB_PARTY (echo [%BB_PARTY%]) else echo UNSET']) + '\r\n',
                           encoding='utf-8')
            env = dict(os.environ)
            env.pop('BB_PARTY', None)
            if value is not None:
                env['BB_PARTY'] = value
            r = subprocess.run(['cmd', '/d', '/c', str(bat)], capture_output=True, text=True, env=env)
            return r.stdout.strip()

    def test_off_values_are_cleared(self):
        for v in [None, '0', 'off', 'OFF', 'false', 'False', 'no', 'NO', ' off ', '   ']:
            self.assertEqual(self.run_block(v), 'UNSET', repr(v))

    def test_on_values_stay(self):
        for v in ['host', 'join', '1', 'BBP1-ABCDEF']:
            self.assertEqual(self.run_block(v), f'[{v}]', repr(v))


if __name__ == '__main__':
    unittest.main()
