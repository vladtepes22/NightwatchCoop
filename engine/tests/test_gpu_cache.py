from paths import ROOT
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest
from run_windows import gpu_cache_dir, gpu_cache_id, reset_stale_gpu_cache


class GpuCacheTests(unittest.TestCase):
    def test_another_engines_cache_is_cleared_once(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'cache'
            (cache / 'CUSA03173').mkdir(parents=True)
            (cache / 'CUSA03173' / 'a.spv').write_bytes(b'x')
            (cache / 'CUSA03173.vkcache').write_bytes(b'x')
            self.assertTrue(reset_stale_gpu_cache(cache, 'engine-2'))
            self.assertEqual(sorted(p.name for p in cache.iterdir()), ['CUSA03173.vkcache', 'engine.txt'])
            (cache / 'CUSA03173').mkdir()
            self.assertFalse(reset_stale_gpu_cache(cache, 'engine-2'))
            self.assertTrue((cache / 'CUSA03173').is_dir())
            self.assertTrue(reset_stale_gpu_cache(cache, 'engine-3'))
            self.assertFalse((cache / 'CUSA03173').exists())

    def test_no_cache_yet_and_unknown_engine(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'cache'
            self.assertFalse(reset_stale_gpu_cache(cache, ''))
            self.assertFalse(cache.exists())
            self.assertFalse(reset_stale_gpu_cache(cache, 'engine-1'))
            self.assertEqual((cache / 'engine.txt').read_text().strip(), 'engine-1')

    def test_the_cache_follows_the_shader_compiler_not_the_engine(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            self.assertEqual(gpu_cache_id(out, 'engine-1'), 'engine-1')
            (out / 'gpu-cache.id').write_text('0123456789abcdef\n', encoding='utf-8')
            self.assertEqual(gpu_cache_id(out, 'engine-1'), gpu_cache_id(out, 'engine-2'))
            self.assertEqual(gpu_cache_id(out, 'engine-2'), 'gpu 0123456789abcdef')

    def test_the_cache_is_beside_the_engine_unless_moved(self):
        self.assertEqual(gpu_cache_dir({'BB_GPU_USER_DIR': 'X:/gpu'}), Path('X:/gpu/cache'))
        self.assertEqual(gpu_cache_dir({}).parent.name, 'user')

    def test_one_catalog_per_game_of_the_gpu_vendor(self):
        import gpu_cache
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            catalogs = root / 'shader-catalog'
            for folder, vendor in ((catalogs / 'CUSA03173', '10DE'), (catalogs / 'amd' / 'CUSA03173', '1002')):
                folder.mkdir(parents=True)
                (folder / 'profile.bin').write_bytes(vendor.encode())
                (folder / 'gpu.txt').write_text(f'vendor {vendor} device 0001\n')
            lab = root / 'bb-shader-lab'
            lab.write_text('')
            seeded = []

            def run(_lab, *args):
                seeded.append(Path(args[1]).parent.name + '/' + Path(args[1]).name)
                return 0, 'Shader catalog: ok\nShader cache: ok\n'

            def start(vendors, cache_vendor=None):
                seeded.clear()
                cache = root / 'cache'
                if cache.exists():
                    import shutil
                    shutil.rmtree(cache)
                cache.mkdir()
                if cache_vendor is not None:
                    (cache / 'CUSA03173').mkdir()
                    (cache / 'CUSA03173' / 'profile.bin').write_bytes(b'x')
                    (cache / 'CUSA03173.vkcache').write_bytes(b'BPBC' + bytes(4) + cache_vendor.to_bytes(4, 'little'))
                with patch.object(gpu_cache, 'gpu_vendors', return_value=set(vendors)), \
                        patch.object(gpu_cache, 'lab_run', side_effect=run), patch.object(gpu_cache, 'log'):
                    gpu_cache.seed(cache, catalogs, root / 'game', root / 'eboot.elf', lab)
                return list(seeded), (cache / 'CUSA03173' / 'profile.bin').read_bytes() if (cache / 'CUSA03173' / 'profile.bin').exists() else None

            self.assertEqual(start({0x1002, 0x10DE}), (['shader-catalog/CUSA03173'], b'10DE'))
            self.assertEqual(start({0x1002}), (['amd/CUSA03173'], b'1002'))
            self.assertEqual(start({0x1002, 0x10DE}, cache_vendor=0x1002)[0], ['amd/CUSA03173'])
            self.assertEqual(start({0x8086}), ([], None))


LAB = ROOT / 'out' / 'bb-shader-lab.exe'
# A real shader catalog (a package's engine/shader-catalog/<vendor folder>), e.g. the Lantern base's.
CATALOG = Path(os.environ.get('BB_TEST_SHADER_CATALOG', ROOT.parent / 'work' / 'downloads' / 'lantern-0.9.38' / 'x' /
                              'BloodbornePC' / 'engine' / 'shader-catalog' / 'amd' / 'CUSA03173'))


def check_catalog(folder):
    env = dict(os.environ, PATH=os.pathsep.join([str(Path('C:/msys64/clang64/bin')), os.environ.get('PATH', '')]))
    p = subprocess.run([str(LAB), '--check-catalog', str(folder)], capture_output=True, text=True, env=env, timeout=120)
    line = next((l for l in p.stdout.splitlines() if l.startswith('Catalog check:')), '')
    return p.returncode, line


@unittest.skipUnless(LAB.exists(), 'out/bb-shader-lab.exe not built (build.sh)')
class ShaderCatalogTests(unittest.TestCase):
    """tools/package-seamless.ps1 ships the base's catalog when this engine reads it (--check-catalog),
    whatever compiler made it: the seed compiles every state again with this one."""

    def test_a_catalog_of_another_format_is_not_shipped(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'profile.bin').write_bytes(b'short')
            (folder / '0x0000000000000001_0.bbs').write_bytes(b'BBS1' + bytes(60))
            (folder / '0x0000000000000001_0.meta').write_bytes(bytes(24))
            code, line = check_catalog(folder)
            self.assertEqual(code, 4, line)
            self.assertIn('not usable', line)

    @unittest.skipUnless((CATALOG / 'profile.bin').exists(), 'no shader catalog (BB_TEST_SHADER_CATALOG)')
    def test_the_base_catalog_is_readable_by_this_engine(self):
        code, line = check_catalog(CATALOG)
        self.assertEqual(code, 0, line)
        readable = int(re.search(r'Catalog check: (\d+) of (\d+)', line).group(1))
        self.assertGreater(readable, 500, line)


if __name__ == '__main__':
    unittest.main()
