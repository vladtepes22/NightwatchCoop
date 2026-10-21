# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from online import configure_online, ONLINE_ENV


class OnlineProfileTests(unittest.TestCase):
    def test_always_offline_and_clears_inherited_online_environment(self):
        with tempfile.TemporaryDirectory() as temp:
            for settings in ({}, {'online_enabled': '0'}, {'online_enabled': '1', 'online_connected': '1'},
                             {'online_enabled': '1', 'online_pool': 'huntersdream'}):
                env = dict.fromkeys(ONLINE_ENV, 'inherited')
                env['UNRELATED'] = 'keep'
                self.assertIsNone(configure_online(settings, temp, env))
                self.assertEqual(env, {'BB_ONLINE': '0', 'UNRELATED': 'keep'})
                self.assertFalse((Path(temp) / 'network').exists())


if __name__ == '__main__': unittest.main()
