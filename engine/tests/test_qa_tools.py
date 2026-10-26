"""The QA harness itself (tools/qa.py, tools/smoke-coop.py, tools/qa_scenarios.json): layer choice from
touched paths, ctest parsing, ordered per-role log expects, the crash-line scan and the scenario rows."""
import importlib.util
import json
import re
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ beside the repo's tools/
TOOLS = Path(__file__).resolve().parents[2] / 'tools'


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, TOOLS / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


qa = load('qa', 'qa.py')
coop = load('smoke_coop', 'smoke-coop.py')


class LayerChoice(unittest.TestCase):
    def test_party_change_needs_native_and_coop_only(self):
        self.assertEqual(qa.layers_for(['engine/gpu/shim/party/party_b2.cpp']), ['native', 'coop'])
        self.assertEqual(qa.layers_for(['engine/gpu/shim/net/party_link.cpp']), ['native', 'coop'])

    def test_other_engine_change_needs_the_solo_smoke(self):
        self.assertEqual(qa.layers_for(['engine/gpu/shim/bbgpu.cpp']), ['native', 'smoke'])
        self.assertEqual(qa.layers_for(['engine/src/probe.c']), ['native', 'smoke'])
        self.assertEqual(qa.layers_for(['engine/start/party.cpp']), ['native', 'smoke'])

    def test_tests_launcher_scripts_docs(self):
        self.assertEqual(qa.layers_for(['engine/tests/test_lamp_travel.cpp']), ['native'])
        self.assertEqual(qa.layers_for(['engine/gpu/cmake/SeamlessTests.cmake']), ['native'])
        self.assertEqual(qa.layers_for(['launcher/windows/BloodborneLauncher/Views/SeamlessPage.cs']), ['launcher'])
        self.assertEqual(qa.layers_for(['engine/scripts/patches.py', 'engine/tests/test_content.py']), ['python'])
        self.assertEqual(qa.layers_for(['tools/package-seamless.ps1']), ['smoke'])
        self.assertEqual(qa.layers_for(['engine/docs/party/items.md', 'README.md']), [])

    def test_layers_come_in_run_order(self):
        layers = qa.layers_for(['launcher/x.cs', 'engine/gpu/shim/party/x.cpp', 'engine/scripts/x.py'])
        self.assertEqual(layers, ['native', 'python', 'launcher', 'coop'])


class Ctest(unittest.TestCase):
    def test_all_passed(self):
        self.assertEqual(qa.parse_ctest('...\n100% tests passed out of 65\n'), (True, 'ctest 65/65'))

    def test_failures_are_named(self):
        out = ('96% tests passed, 2 tests failed out of 65\n\nThe following tests FAILED:\n'
               '\t  4 - party-link-test (Failed)\n\t 12 - map-page-test (Timeout)\n')
        self.assertEqual(qa.parse_ctest(out), (True, 'ctest 63/65: party-link-test, map-page-test'))

    def test_no_summary(self):
        self.assertEqual(qa.parse_ctest('ninja: error: loading build.ninja'), (False, 'no ctest summary'))


class Expects(unittest.TestCase):
    LOGS = {'host': 'cooperators 0 -> 1\ncooperators 1 -> 0\ncooperators 0 -> 1\n',
            'guest': 'guest_leave: Party > Leave pressed\nguest_rejoin: Party > Rejoin pressed\n'}

    def test_in_order_per_role(self):
        rows = coop.check_expects([('host', 'cooperators 0 -> 1'), ('guest', 'Leave pressed'),
                                   ('host', 'cooperators 1 -> 0'), ('host', 'cooperators 0 -> 1')], self.LOGS)
        self.assertTrue(all(r['passed'] for r in rows))
        self.assertEqual(rows[1]['line'], 'guest_leave: Party > Leave pressed')

    def test_a_line_is_not_matched_twice(self):
        rows = coop.check_expects([('host', 'cooperators 0 -> 1')] * 3, self.LOGS)
        self.assertEqual([r['passed'] for r in rows], [True, True, False])

    def test_order_matters_within_a_role(self):
        rows = coop.check_expects([('guest', 'Rejoin pressed'), ('guest', 'Leave pressed')], self.LOGS)
        self.assertEqual([r['passed'] for r in rows], [True, False])

    def test_missing_role_log(self):
        self.assertFalse(coop.check_expects([('guest', 'x')], {'host': 'x'})[0]['passed'])

    def test_expect_argument(self):
        self.assertEqual(coop.parse_expect('host:a:b'), ('host', 'a:b'))
        with self.assertRaises(Exception):
            coop.parse_expect('spectator:x')


class CrashLines(unittest.TestCase):
    def test_engine_faults_fail(self):
        for line in ['Guest fault (signal 11) at guest offset 0xebc0cb, address 0x28',
                     'uncaught C++ exception: std::bad_alloc', 'C++ exception, thread 4242:',
                     'vkQueueSubmit: VK_ERROR_DEVICE_LOST', 'STOP: guest raised debug exception code=0x1',
                     'Party: Session lost', '[WATCHDOG] Boot stalled for 20 s']:
            self.assertTrue(re.search(coop.CRASH_RX, line), line)
            self.assertTrue(re.search(qa.CRASH_RX, line), line)

    def test_benign_lines_pass(self):
        for line in ["[coop] Party: host gone: waiting for it to come back (its crash restart) up to 180 s",
                     'Crash reports: another module replaced the unhandled exception filter (an overlay?); set again',
                     'BB_LOG_CXX_EXCEPTIONS=1']:
            self.assertIsNone(re.search(coop.CRASH_RX, line), line)


class Scenarios(unittest.TestCase):
    def test_rows_are_well_formed(self):
        rows = json.loads((TOOLS / 'qa_scenarios.json').read_text(encoding='utf-8'))
        self.assertTrue({'join', 'core'} <= set(rows))
        for name, row in rows.items():
            self.assertTrue(row.get('about'), name)
            self.assertTrue(row['expect'], name)
            for role, pattern in row['expect']:
                self.assertIn(role, ('host', 'guest'), name)
                re.compile(pattern)
            for role in ('host', 'guest'):
                self.assertIsInstance(row.get(role, ''), str)

    def test_release_rows_exist(self):
        rows = json.loads((TOOLS / 'qa_scenarios.json').read_text(encoding='utf-8'))
        for name in qa.RELEASE_SCENARIOS:
            self.assertIn(name, rows)


if __name__ == '__main__':
    unittest.main()
