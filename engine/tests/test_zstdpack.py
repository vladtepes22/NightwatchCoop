"""bb-zstdpack (tools/zstdpack) on a synthetic dvdroot_ps4 made at test time: a DCX "DFLT" container with a zlib stream
and the layout the converter reads, a file with another layout, a loose file. No game data.

The test needs the built converter (out/bb-zstdpack.exe, or BB_ZSTDPACK); without it the tests are skipped.
"""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

from paths import ROOT

ZSTD_MAGIC = bytes.fromhex('28b52ffd')


def tool():
    candidates = [os.environ.get('BB_ZSTDPACK', '')]
    for name in ('bb-zstdpack.exe', 'bb-zstdpack'):
        candidates += [str(ROOT / 'out' / name), str(ROOT / 'out' / 'gpu' / name)]
    for c in candidates:
        if c and Path(c).is_file():
            return c
    return None


def dcx(payload: bytes, level: int = 9) -> bytes:
    """The DCX "DFLT" container of the dump: a 0x4c byte header, then one zlib stream."""
    z = zlib.compress(payload, level)
    head = (b'DCX\0' + struct.pack('>IIII', 0x10000, 0x18, 0x24, 0x44) + struct.pack('>I', 0x4c) + b'DCS\0' +
            struct.pack('>II', len(payload), len(z)) + b'DCP\0DFLT' + struct.pack('>I', 0x20) + bytes([9, 0, 0, 0]) +
            bytes(12) + bytes([0, 1, 1, 0]) + b'DCA\0' + struct.pack('>I', 8))
    assert len(head) == 0x4c, len(head)
    return head + z


def sample(n: int, seed: int) -> bytes:
    out = bytearray()
    x = seed * 2654435761 + 1
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7fffffff
        run = 1 + x % 200
        word = b'bloodborne%d ' % (x % 97)
        out += (word * (run // len(word) + 1))[:run] if x % 3 else bytes((x >> 3) & 255 for _ in range(run // 4 + 1))
    return bytes(out[:n])


def digest(root: Path) -> dict:
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob('*')) if p.is_file()}


@unittest.skipUnless(tool(), 'bb-zstdpack is not built')
class ZstdPackTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        base = Path(self._tmp.name)
        self.game, self.out = base / 'game', base / 'out'
        dv = self.game / 'dvdroot_ps4'
        (dv / 'chr').mkdir(parents=True)
        (dv / 'map' / 'm10').mkdir(parents=True)
        self.files = {'chr/c1000.chrbnd.dcx': sample(300000, 1), 'map/m10/m10.msb.dcx': sample(5000, 2),
                      'chr/empty.dcx': b'', 'chr/Upper.DCX': sample(70000, 3)}
        for rel, payload in self.files.items():
            (dv / rel).write_bytes(dcx(payload))
        # a header variant of the dump: the DCP / DCA offsets at +0x10 / +0x14 are 0x24 / 0x2c
        variant = bytearray(dcx(sample(40000, 5)))
        variant[0x10:0x18] = struct.pack('>II', 0x24, 0x2c)
        (dv / 'chr' / 'variant.dcx').write_bytes(bytes(variant))
        self.files['chr/variant.dcx'] = sample(40000, 5)
        # another layout (a different method tag) and a loose file: both stay in the game folder
        odd = bytearray(dcx(sample(1000, 4)))
        odd[0x28:0x2c] = b'KRAK'
        (dv / 'chr' / 'odd.dcx').write_bytes(bytes(odd))
        (dv / 'chr' / 'readme.txt').write_bytes(b'not a dcx')
        self.before = digest(self.game)

    def run_tool(self, *args, check=True):
        p = subprocess.run([tool(), *args], capture_output=True, text=True, timeout=300)
        if check:
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p

    def convert(self, *extra, **kw):
        return self.run_tool('convert', '--game', str(self.game), '--out', str(self.out), '--threads', '2', *extra, **kw)

    def test_convert_layout_and_manifest(self):
        p = self.convert()
        self.assertIn('free space', p.stdout)  # the free space check comes first
        self.assertEqual(digest(self.game), self.before, 'the game folder was modified')
        converted = self.out / 'dvdroot_ps4'
        for rel, payload in self.files.items():
            src = (self.game / 'dvdroot_ps4' / rel).read_bytes()
            dst = (converted / rel).read_bytes()
            self.assertEqual(dst[:0x20], src[:0x20])
            self.assertEqual(dst[0x24:0x4c], src[0x24:0x4c], 'the DCP / DCA block is kept')
            usize, csize = struct.unpack('>II', dst[0x1c:0x24])
            self.assertEqual(usize, len(payload))
            self.assertEqual(0x4c + csize, len(dst), 'the compressed size field matches the file')
            self.assertEqual(dst[0x4c:0x50], ZSTD_MAGIC)
        self.assertFalse((converted / 'chr' / 'odd.dcx').exists())
        self.assertFalse((converted / 'chr' / 'readme.txt').exists())
        manifest = (self.out / 'bbzstd.manifest').read_text().splitlines()
        self.assertTrue(manifest[0].startswith('bbzstd 1 '))
        entries = [m.split('\t') for m in manifest if m.startswith('F\t')]
        self.assertEqual(sorted(e[1] for e in entries), sorted(self.files))
        for e in entries:
            src = self.game / 'dvdroot_ps4' / e[1]
            self.assertEqual(int(e[2]), src.stat().st_size)
            self.assertEqual(int(e[3]), int(src.stat().st_mtime))
            self.assertEqual(int(e[4]), (converted / e[1]).stat().st_size)
        self.assertTrue(manifest[-1].startswith('DONE'))
        # the decompressed content equals the original's (the tool verifies; so do we, when zstandard is installed)
        try:
            import zstandard
        except ImportError:
            return
        for rel, payload in self.files.items():
            frame = (converted / rel).read_bytes()[0x4c:]
            self.assertEqual(zstandard.ZstdDecompressor().decompress(frame, max_output_size=len(payload) + 1), payload)

    def test_verify_passes_and_catches_damage(self):
        self.convert()
        self.run_tool('verify', '--game', str(self.game), '--out', str(self.out), '--threads', '2')
        victim = self.out / 'dvdroot_ps4' / 'chr' / 'c1000.chrbnd.dcx'
        data = bytearray(victim.read_bytes())
        for i in range(len(data) // 2, len(data) // 2 + 64):
            data[i] ^= 0x5a
        victim.write_bytes(bytes(data))
        p = self.run_tool('verify', '--game', str(self.game), '--out', str(self.out), '--threads', '2', check=False)
        self.assertEqual(p.returncode, 1)
        self.assertIn('VERIFY FAILED', p.stderr)

    def test_resume_skips_done_files_and_redoes_changed_ones(self):
        self.convert()
        converted = self.out / 'dvdroot_ps4'
        stamp = {rel: (converted / rel).stat().st_mtime_ns for rel in self.files}
        p = self.convert()
        self.assertIn('already done', p.stdout)
        self.assertEqual({rel: (converted / rel).stat().st_mtime_ns for rel in self.files}, stamp)
        # an original that changed (size and time) is converted again
        rel = 'map/m10/m10.msb.dcx'
        new = sample(9000, 9)
        (self.game / 'dvdroot_ps4' / rel).write_bytes(dcx(new))
        self.convert()
        self.run_tool('verify', '--game', str(self.game), '--out', str(self.out), '--threads', '2')
        entries = {m.split('\t')[1]: m.split('\t') for m in (self.out / 'bbzstd.manifest').read_text().splitlines()
                   if m.startswith('F\t')}
        self.assertEqual(int(entries[rel][2]), (self.game / 'dvdroot_ps4' / rel).stat().st_size)
        self.assertEqual(struct.unpack('>I', (converted / rel).read_bytes()[0x1c:0x20])[0], len(new))

    def test_refuses_the_game_folder_as_output_and_a_missing_game(self):
        p = self.run_tool('convert', '--game', str(self.game), '--out', str(self.game), check=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(digest(self.game), self.before)
        p = self.run_tool('convert', '--game', str(self.game / 'nothing'), '--out', str(self.out), check=False)
        self.assertNotEqual(p.returncode, 0)

    def test_corrupt_original_is_reported_not_converted(self):
        victim = self.game / 'dvdroot_ps4' / 'chr' / 'Upper.DCX'
        data = bytearray(victim.read_bytes())
        data[0x4c + 40] ^= 0xff
        victim.write_bytes(bytes(data))
        p = self.convert(check=False)
        self.assertEqual(p.returncode, 1)
        self.assertIn('FAILED', p.stderr)
        self.assertFalse((self.out / 'dvdroot_ps4' / 'chr' / 'Upper.DCX').exists())
        self.assertNotIn('DONE', (self.out / 'bbzstd.manifest').read_text())


if __name__ == '__main__':
    unittest.main()
