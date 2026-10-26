"""tools/log_digest.py: real log markers, report folding, CLI and privacy."""
import paths  # noqa: F401 (puts scripts/ on sys.path)
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import log_digest as ld


HOST = """Nightwatch Co-op 0.2.5-beta.1: co-op visy v0.7.6, base 0.9.37, source abc123
GPU: NVIDIA GeForce RTX 3070, driver 1 (NVIDIA), Vulkan 1.3
GPU memory: Vulkan 7000 MiB allocated (VMA blocks 7100 MiB, 6900 MiB of them used); the game's data in VRAM 6000 MiB (300 MiB unused)
[coop] Net: party networking on (host, as HostA, port 9307)
[coop] Party: NAT: cone (port open); port 9307 open: guests connect directly
[coop] Party: link: hosting: port 9307, up to 4 players
[coop] Coop hooks: Example: entry hook at guest +0x123abc
"""
PROBLEMS = """Guest fault 0xc0000005 at guest+0xebc0cb
Host fault 0xc0000005 at bb-probe.exe+0x1234
GPU worker: uncaught C++ exception std::bad_alloc: bad allocation
vkQueueSubmit: VK_ERROR_DEVICE_LOST
Vulkan: device lost
Runtime: no room for 8192 KiB of host memory the game sees below 0x100000000
Runtime: committing 8192 KiB of host memory for the game failed (Windows error 1450)
[coop] Party: link: lost GuestA (slot 1): no traffic for 30 s
[coop] Party: link: reconnecting: handshake timed out; retrying in 1000 ms
[coop] Coop hooks: Example: guest +0x123abc is not the expected code (another game version or a patch); not installed
[coop] Coop hooks: Other: guest +0x234abc not writable; not written
"""


class LogDigestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8', newline='\n')
        return path

    def cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = ld.main(list(map(str, args)))
        return status, out.getvalue(), err.getvalue()

    def test_host_context_and_healthy_hooks(self):
        result = ld.digest(self.write('host.log', HOST))
        self.assertEqual(result['problems'], {})
        context = result['context']
        self.assertIn('visy v0.7.6', context['build'][0]['text'])
        self.assertIn('RTX 3070', context['gpu'][0]['text'])
        self.assertEqual(context['vram'][0]['line'], 3)
        self.assertIn('(host,', context['role'][0]['text'])
        self.assertIn('hosting:', context['link'][0]['text'])
        self.assertIn('cone (port open)', context['nat'][0]['text'])

    def test_categories_first_lines_and_counts(self):
        result = ld.digest(self.write('bad.log', HOST + PROBLEMS))['problems']
        expected = {'crash': (8, 2), 'cpp_exception': (10, 1), 'vulkan': (11, 2),
                    'memory': (13, 2), 'party_link': (15, 2), 'coop_hooks': (17, 2)}
        self.assertEqual(set(result), set(expected))
        for category, (number, count) in expected.items():
            self.assertEqual(result[category]['first_line'], number)
            self.assertEqual(result[category]['count'], count)
        self.assertEqual(result['crash']['addresses'], ['+0xebc0cb', '+0x1234'])

    def test_linux_fault_and_watchdog(self):
        result = ld.digest(self.write('linux.log',
            'Guest fault (signal 11) at guest offset 0xebc0cb, address 0x28\n'
            'Fault (signal 11) at RIP 0x1234, address 0x28\n'
            'STOP: watchdog timeout; thread stacks:\n'))['problems']
        self.assertEqual(result['crash']['count'], 3)
        self.assertEqual(result['crash']['addresses'], ['+0xebc0cb'])

    def test_guest_link_types_and_status(self):
        for transport in ("TCP (the host's port is open)",
                          "UDP via IPv6 [2001:db8::1]:9307 (no NAT in the way)",
                          "UDP through a member's relay (203.0.113.1:9307)"):
            with self.subTest(transport=transport):
                result = ld.digest(self.write('guest.log',
                    '[coop] Net: party networking on (guest, as GuestA, port 9307)\n'
                    '[coop] Party: link: connected: slot 1\n'
                    f'[coop] Party: network path: party link over {transport}\n'
                    '[coop] Party: NAT: symmetric; strict NAT\n'))
                self.assertIn('(guest,', result['context']['role'][0]['text'])
                self.assertIn(transport, result['context']['link_type'][0]['text'])
                self.assertFalse(result['problems'])

    def test_launcher_report_folding_and_hardware(self):
        report = ('Nightwatch Co-op v0.2.5 beta 1, engine x\n'
                  'Exit code 0x8B: The game stopped.\n'
                  'NVIDIA RTX 3070 (8 GB), 32 GB RAM, Windows 10.0\n'
                  'Last 120 lines of logs\\last_session.log (repeats folded):\n'
                  '```\nRuntime: no room for 8192 KiB of host memory  (x12)\n```\n')
        result = ld.digest(self.write('crash.txt', report))
        self.assertEqual(result['problems']['memory']['count'], 12)
        self.assertEqual(result['problems']['memory']['first_line'], 6)
        self.assertEqual(result['problems']['crash']['first_line'], 2)
        self.assertIn('RTX 3070', result['context']['gpu'][0]['text'])

    def test_context_is_bounded_and_keeps_latest_vram(self):
        result = ld.digest(self.write('many.log', ''.join(
            f'GPU memory: Vulkan {i} MiB allocated\n' for i in range(100))))
        readings = result['context']['vram']
        self.assertEqual([r['line'] for r in readings], [1, 2, 100])

    def test_false_positives(self):
        result = ld.digest(self.write('clean.log', HOST +
            'C++ exception, thread 123:\nCrash reports: another module replaced the filter\n'
            '[coop] Party: link: GuestA left the party\n'
            '[coop] Party: link: Lost joined (slot 1, from 203.0.113.1:9307)\n'
            '[coop] Party: link: dropped an event with an invalid name from slot 1\n'
            'Exit code 0x0: done\n'))
        # An invalid event is discarded, but the party link itself is still up.
        self.assertFalse(result['problems'])

    def test_summary_counts_files_not_hits_and_deduplicates(self):
        bad = self.write('a.log', PROBLEMS * 3)
        self.write('nested/b.LOG', 'vkPresent: ErrorDeviceLost\n')
        self.write('clean.txt', HOST)
        self.write('ignored.dmp', 'Guest fault 0xc0000005 at guest+0x1')
        status, out, err = self.cli('--summary', '--json', self.root, bad)
        self.assertEqual((status, err), (0, ''))
        result = json.loads(out)
        self.assertEqual(result['files'], 3)
        self.assertEqual(result['categories']['vulkan'], 2)
        self.assertEqual(result['categories']['crash'], 1)
        self.assertEqual(next(iter(result['categories'])), 'vulkan')
        self.assertIn('vulkan', self.cli('--summary', self.root)[1])

    def test_privacy_in_text_json_and_errors(self):
        path = self.write('privacy.log',
            'Host fault 0xc0000005 at C:\\Users\\Alice\\game\\bb-probe.exe+0x123\n'
            'GPU worker: uncaught C++ exception: c:/users/Bob/game/missing\n')
        for flags in ((), ('--json',)):
            status, out, err = self.cli(*flags, path)
            self.assertEqual((status, err), (0, ''))
            self.assertNotIn('Alice', out)
            self.assertNotIn('Bob', out)
            if flags:
                out = json.dumps(json.loads(out), ensure_ascii=False)
            self.assertIn('<user>', out)
        for flags in ((), ('--json',)):
            status, out, err = self.cli(*flags, r'C:\Users\Secret\missing.log')
            self.assertEqual(status, 1)
            self.assertNotIn('Secret', out + err)
        with contextlib.redirect_stderr(io.StringIO()) as err:
            with self.assertRaises(SystemExit):
                ld.main([r'--bad=C:\Users\Secret\missing.log', str(path)])
        self.assertNotIn('Secret', err.getvalue())

    def test_empty_bom_and_invalid_utf8(self):
        path = self.write('empty.log', '')
        self.assertIn('none detected', self.cli(path)[1])
        path.write_bytes(b'\xef\xbb\xbfGuest fault 0xc0000005 at guest+0x123\xff\n')
        result = json.loads(self.cli('--json', path)[1])['reports'][0]
        self.assertEqual(result['problems']['crash']['count'], 1)

    def test_empty_folder_and_partial_failure(self):
        self.assertEqual(self.cli(self.root)[0], 1)
        path = self.write('valid.log', PROBLEMS)
        status, out, err = self.cli('--json', path, self.root / 'missing.log')
        self.assertEqual((status, err), (1, ''))
        result = json.loads(out)
        self.assertEqual(len(result['reports']), 1)
        self.assertEqual(len(result['errors']), 1)


if __name__ == '__main__':
    unittest.main()
