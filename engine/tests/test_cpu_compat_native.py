"""Legacy CPU in bb-start (start/cpu_compat.cpp) against visy's scripts/cpu_compat.py.

Runs out/bb-start.exe --test-prepare on the game in BB_TEST_GAME_DIR (a 1.09 CUSA03173 folder);
skipped without it. Checks that the off state leaves the boot image as it was, that the native
translation gives the same bytes as the Python reference (patch_libc's strlen, patch_image, then
the red-zone sites clear of the translations, as visy's link_modules orders them), and that the
guards skip a changed site or the whole plan for other game files.
"""
from paths import ROOT
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import cpu_compat
from link_modules import module, protect_red_zones

START = ROOT / 'out' / 'bb-start.exe'
PLAN = ROOT / 'tools' / 'legacy-cpu-plan.json'
GAME = Path(os.environ['BB_TEST_GAME_DIR']) if os.environ.get('BB_TEST_GAME_DIR') else None


def parse(path):
    raw = Path(path).read_bytes()
    magic, size, entry, ns, nr, ni, flags = struct.unpack_from('<8s6Q', raw, 0)
    assert magic == b'BBPROBE5'
    pos = 56 + 8 + 32
    nt, = struct.unpack_from('<Q', raw, pos)
    tables = struct.unpack_from(f'<{7 * nt}Q', raw, pos + 8)
    pos += 8 + nt * 56
    nb, = struct.unpack_from('<Q', raw, pos)
    pos += 8 + nb * 24
    head = raw[56:pos]
    segments = [struct.unpack_from('<3Q', raw, pos + i * 24) for i in range(ns)]
    pos += ns * 24
    names = raw[pos:pos + ni * 128]
    pos += ni * 128
    relocs = [struct.unpack_from('<QQqq', raw, pos + i * 32) for i in range(nr)]
    rest = raw[pos:pos + nr * 32]
    pos += nr * 32
    return dict(entry=entry, flags=flags, head=head, names=names, rest=rest, segments=segments,
                relocs=relocs, image=bytearray(raw[pos:pos + size]),
                modules=[tables[i * 7:i * 7 + 2] for i in range(nt)])


@unittest.skipUnless(GAME and (GAME / 'eboot.bin').is_file() and START.is_file() and os.name == 'nt',
                     'needs BB_TEST_GAME_DIR (a 1.09 game folder) and out/bb-start.exe')
class NativeLegacyCpuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        # An engine folder without the red-zone sites: the image the Python reference starts from.
        bare = cls.root / 'bare-engine'
        shutil.copytree(ROOT / 'data', bare / 'data')
        (bare / 'patches').mkdir()
        cls.bare = bare

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def prepare(self, name, legacy, engine=ROOT, plan=PLAN):
        out = self.root / name
        env = {k: v for k, v in os.environ.items() if not k.startswith('BB_LEGACY_CPU')}
        if legacy is not None:
            env['BB_LEGACY_CPU'] = legacy
        env['BB_LEGACY_CPU_PLAN'] = str(plan)
        result = subprocess.run([str(START), '--test-prepare', str(GAME), str(out), str(engine)],
                                capture_output=True, text=True, env=env, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return out, result.stdout + result.stderr

    def test_off_leaves_the_image_as_it_was(self):
        if cpu_compat.missing_host_extensions():
            self.skipTest('this CPU lacks BMI1/LZCNT/MOVBE: unset means on here')
        unset, log = self.prepare('unset', None)
        off, _ = self.prepare('off', '0')
        self.assertNotIn('Legacy CPU', log)
        self.assertEqual((unset / 'boot-linked.bin').read_bytes(), (off / 'boot-linked.bin').read_bytes())
        self.assertNotIn('cpu_compat', json.loads((off / 'prepared.json').read_text()))
        self.assertNotEqual(json.loads((off / 'prepared.json').read_text())['key'],
                            json.loads((self.prepare('on-key', '1')[0] / 'prepared.json').read_text())['key'])

    def reference(self, plan=None):
        """visy's order on the image without red-zone sites: strlen, patch_image, red-zone sites
        clear of the translations (laid out as prepare.cpp lays them out)."""
        base, _ = self.prepare('bare-off', '0', engine=self.bare)
        boot = parse(base / 'boot-linked.bin')
        image = boot['image']
        libc_base = boot['modules'][0][0]
        libc = module(GAME / 'sce_module' / 'libc.prx')
        strlen = [s for s in libc['symbols'] if s['section'] and s['type'] == 2
                  and s['name'].split('#')[0] == cpu_compat.nid('strlen')]
        self.assertEqual(len(strlen), 1)
        at = libc_base + strlen[0]['value']
        image[at:at + strlen[0]['size']] = (cpu_compat.STRLEN_BASELINE +
                                            b'\x90' * (strlen[0]['size'] - len(cpu_compat.STRLEN_BASELINE)))
        main = module(GAME / 'eboot.bin')
        names = ('libc.prx', 'libSceFios2.prx')
        modules = [dict(file=n, base=b, size=s, sha256=hashlib.sha256((GAME / 'sce_module' / n).read_bytes()).hexdigest())
                   for n, (b, s) in zip(names, boot['modules'])]
        untranslated = bytes(image)
        image, segments, report = cpu_compat.patch_image(image, boot['segments'], main, modules, boot['relocs'],
                                                         plan=plan, active=True)
        sites = json.loads((ROOT / 'patches' / 'redzone.json').read_text())['sites']
        clear = [s for s in sites
                 if image[max(0, s[0] - 14):s[0] + len(s[1]) // 2] == untranslated[max(0, s[0] - 14):s[0] + len(s[1]) // 2]]
        red_base = (len(image) + 65535) & ~65535
        code, applied = protect_red_zones(image, clear, red_base, sorted(r[0] for r in boot['relocs']))
        if code:
            image = image + bytes(red_base - len(image)) + code
            segments.append((red_base, len(code), 5))
        return boot, image, segments, report, len(sites) - len(clear)

    def test_native_translation_matches_the_python_reference(self):
        out, log = self.prepare('on', '1')
        native = parse(out / 'boot-linked.bin')
        boot, image, segments, report, left = self.reference()
        self.assertEqual(report['patched_sites'], 6562)
        self.assertIn(f"translated {report['patched_sites']} instruction sites", log)
        self.assertIn('0 skipped', log)
        self.assertIn(f'{left} red-zone sites left to the translations', log)
        self.assertEqual(native['segments'], [tuple(s) for s in segments])
        self.assertEqual(len(native['image']), len(image))
        self.assertTrue(native['image'] == image, 'the native image differs from the Python reference')
        for key in ('entry', 'flags', 'head', 'names', 'rest'):
            self.assertEqual(native[key], boot[key], key)
        self.assertTrue(json.loads((out / 'prepared.json').read_text())['cpu_compat'])

    def test_a_changed_site_is_skipped_alone(self):
        plan = json.loads(PLAN.read_text())
        site = next(s for s in plan['sites'] if s['module'] == 'eboot.bin' and s.get('kind', 'instruction') == 'instruction')
        original = bytes.fromhex(site['bytes'])
        site['bytes'] = (bytes([original[0] ^ 1]) + original[1:]).hex()
        path = self.root / 'changed-plan.json'
        path.write_text(json.dumps(plan))
        out, log = self.prepare('changed', '1', plan=path)
        self.assertIn('translated 6561 instruction sites', log)
        self.assertIn('1 skipped', log)
        self.assertIn(f"skipped eboot.bin+{site['address']:#x}: original instruction bytes do not match", log)
        image = parse(out / 'boot-linked.bin')['image']
        self.assertEqual(bytes(image[site['address']:site['address'] + len(original)]), original)

    def test_other_game_files_leave_it_off(self):
        plan = json.loads(PLAN.read_text())
        plan['modules']['libSceFios2.prx'] = '0' * 64
        path = self.root / 'other-plan.json'
        path.write_text(json.dumps(plan))
        out, log = self.prepare('other', '1', plan=path)
        off, _ = self.prepare('off-2', '0')
        self.assertIn('not applied: the plan was made for other game files (libSceFios2.prx differ)', log)
        self.assertEqual((out / 'boot-linked.bin').read_bytes(), (off / 'boot-linked.bin').read_bytes())
        self.assertNotIn('cpu_compat', json.loads((out / 'prepared.json').read_text()))


if __name__ == '__main__':
    unittest.main()
