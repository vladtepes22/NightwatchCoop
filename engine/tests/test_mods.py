from paths import ROOT
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('bbmods', ROOT / 'scripts/mods.py')
mods = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mods)


class ModTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'CUSA03173'
        self.assets = self.game / 'dvdroot_ps4' / 'chr'
        self.assets.mkdir(parents=True)
        (self.game / 'eboot.bin').write_bytes(b'original executable')
        (self.assets / 'a.dcx').write_bytes(b'original')
        (self.assets / 'b.dcx').write_bytes(b'untouched')
        self.moddir = self.root / 'mods'

    def mod(self, name, contents=b'mod', file='a.dcx'):
        root = self.moddir / name
        folder = root / 'dvdroot_ps4' / 'chr'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / file).write_bytes(contents)
        return root

    def test_game_update_beside_the_game_is_layered_under_mods(self):
        update = self.root / 'CUSA03173-UPDATE'
        (update / 'dvdroot_ps4' / 'chr').mkdir(parents=True)
        (update / 'sce_sys').mkdir()
        (update / 'eboot.bin').write_bytes(b'1.09 executable')
        (update / 'sce_sys' / 'param.sfo').write_bytes(b'1.09 sfo')
        (update / 'dvdroot_ps4' / 'chr' / 'a.dcx').write_bytes(b'updated')
        (update / 'dvdroot_ps4' / 'chr' / 'b.dcx').write_bytes(b'updated b')
        # samefile: MinGW Python's iterdir joins with '\' while '/' builds the expected path
        self.assertTrue(mods.update_folder(self.game).samefile(update))
        a = self.mod('A')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)], mods.update_folder(self.game))
        self.assertEqual((result / 'eboot.bin').read_bytes(), b'1.09 executable')
        self.assertEqual((result / 'sce_sys' / 'param.sfo').read_bytes(), b'1.09 sfo')
        self.assertEqual((result / 'dvdroot_ps4' / 'chr' / 'a.dcx').read_bytes(), b'mod')
        self.assertEqual((result / 'dvdroot_ps4' / 'chr' / 'b.dcx').read_bytes(), b'updated b')
        self.assertEqual((self.game / 'eboot.bin').read_bytes(), b'original executable')
        self.assertIsNone(mods.update_folder(update))

    def test_merge_replacement_new_file_and_directory_listing(self):
        a = self.mod('A')
        self.mod('A', b'new', 'new.dcx')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        folder = result / 'dvdroot_ps4' / 'chr'
        self.assertEqual((folder / 'a.dcx').read_bytes(), b'mod')
        self.assertEqual((folder / 'b.dcx').read_bytes(), b'untouched')
        self.assertEqual((folder / 'new.dcx').read_bytes(), b'new')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.dcx', 'b.dcx', 'new.dcx'})
        self.assertEqual((self.assets / 'a.dcx').read_bytes(), b'original')
        self.assertFalse((self.assets / 'new.dcx').exists())
        self.assertEqual((result / 'eboot.bin').read_bytes(), b'original executable')

    def test_last_mod_wins_and_reordering_changes_winner(self):
        a, b = self.mod('A', b'A'), self.mod('B', b'B')
        for layers, winner in [([('A', a), ('B', b)], b'B'), ([('B', b), ('A', a)], b'A')]:
            result = mods.build_overlay(self.game, self.root / 'out', layers)
            self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), winner)

    def test_selection_disable_and_new_mod(self):
        for name in ['C', 'B', 'A']:
            self.mod(name)
        config = self.root / 'mods.json'
        config.write_text(json.dumps({'order': ['B', 'A', 'deleted'], 'disabled': ['A']}))
        self.assertEqual(mods.selected(self.moddir, config), ['B', 'C'])

    def test_no_mod_returns_original_game(self):
        self.assertEqual(mods.build_overlay(self.game, self.root / 'out', []), self.game)
        self.assertFalse((self.root / 'out').exists())

    def test_a_new_start_removes_overlays_left_by_earlier_ones(self):
        a = self.mod('A')
        out = self.root / 'out'
        first = mods.build_overlay(self.game, out, [('A', a)])
        second = mods.build_overlay(self.game, out, [('A', a)])
        self.assertFalse(first.exists())
        self.assertEqual([p.name for p in out.iterdir()], [second.name])
        mods.build_overlay(self.game, out, [])
        self.assertEqual(list(out.iterdir()), [])
        self.assertEqual((self.assets / 'a.dcx').read_bytes(), b'original')
        self.assertEqual((a / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'mod')

    def test_directory_conflict_does_not_modify_base(self):
        a = self.mod('A')
        (a / 'dvdroot_ps4/chr/a.dcx').unlink()
        nested = a / 'dvdroot_ps4/chr/b.dcx'
        nested.mkdir()
        (nested / 'child').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        self.assertEqual((self.assets / 'b.dcx').read_bytes(), b'untouched')
        self.assertEqual(list((self.root / 'out').iterdir()), [])

    def test_mod_symlinks_skipped_not_followed(self):
        a = self.mod('A')
        try:
            (a / 'dvdroot_ps4/escape').symlink_to(self.assets, target_is_directory=True)
        except OSError:
            self.skipTest('symbolic links need administrator rights or developer mode here')
        result = mods.build_overlay(self.game, self.root / 'out', [('A', a)])
        self.assertFalse((result / 'dvdroot_ps4/escape').exists())
        self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), b'mod')

    def test_executable_replacement_rejected(self):
        a = self.mod('A')
        (a / 'eboot.bin').write_bytes(b'unsupported')
        with self.assertRaises(ValueError):
            mods.build_overlay(self.game, self.root / 'out', [('A', a)])

    def test_shadps4_sibling_folder_and_global_disable(self):
        legacy = Path(str(self.game) + '-mods')
        folder = legacy / 'dvdroot_ps4/chr'
        folder.mkdir(parents=True)
        (folder / 'a.dcx').write_bytes(b'legacy')
        for enabled, expected in [('1', b'legacy'), ('0', b'original')]:
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/mods.py'), str(self.game),
                '--out', str(self.root / 'out'), '--mods-dir', str(self.moddir), '--enabled', enabled],
                capture_output=True, text=True, check=True)
            self.assertEqual((Path(result.stdout.strip()) / 'dvdroot_ps4/chr/a.dcx').read_bytes(), expected)

    def test_app0_wrapped_mod_discovered(self):
        self.moddir.mkdir()
        (self.moddir / 'Wrapped/app0/dvdroot_ps4').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir), ['Wrapped'])

    def test_other_case_replaces_the_game_file(self):
        root = self.moddir / 'Upper'
        (root / 'DVDROOT_PS4/Chr').mkdir(parents=True)
        (root / 'DVDROOT_PS4/Chr/A.DCX').write_bytes(b'upper')
        result = mods.build_overlay(self.game, self.root / 'out', [('Upper', root)])
        folder = result / 'dvdroot_ps4/chr'
        self.assertEqual((folder / 'a.dcx').read_bytes(), b'upper')
        self.assertEqual({p.name for p in folder.iterdir()}, {'a.dcx', 'b.dcx'})
        self.assertEqual({p.name for p in result.iterdir()}, {'dvdroot_ps4', 'eboot.bin'})

    def test_wrapped_and_bare_layouts(self):
        layouts = {'Title': 'CUSA03173/dvdroot_ps4/chr', 'Archive': 'Archive v1.2/dvdroot_ps4/chr',
                   'Bare': 'chr', 'Nested': 'Nested/app0/dvdroot_ps4/chr',
                   'Us': 'CUSA03179/dvdroot_ps4/chr'}
        for name, path in layouts.items():
            folder = self.moddir / name / path
            folder.mkdir(parents=True)
            (folder / 'a.dcx').write_bytes(name.encode())
        (self.moddir / 'Archive/readme.txt').write_text('notes')
        (self.moddir / 'Us/screenshots').mkdir()
        (self.moddir / 'Junk/stuff').mkdir(parents=True)
        self.assertEqual(mods.discover(self.moddir), ['Archive', 'Bare', 'Nested', 'Title', 'Us'])
        for name in layouts:
            with self.subTest(name=name):
                result = mods.build_overlay(self.game, self.root / 'out',
                                            [(name, self.moddir / name)])
                self.assertEqual((result / 'dvdroot_ps4/chr/a.dcx').read_bytes(), name.encode())

    def test_invalid_profile_fails(self):
        self.mod('A')
        config = self.root / 'mods.json'
        config.write_text('{"disabled":"A"}')
        with self.assertRaises(ValueError):
            mods.selected(self.moddir, config)

    @unittest.skipIf(os.name == 'nt', 'POSIX executable scripts and run.sh')
    def test_run_uses_overlay_propagates_exit_and_cleans_view(self):
        self.mod('A')
        python = self.root / 'python'
        python.write_text(f'#!{sys.executable}\nimport subprocess,sys\n'
            'if sys.argv[1] == "scripts/mods.py" or sys.argv[1] == "-c":\n'
            '    sys.exit(subprocess.call([sys.executable,*sys.argv[1:]]))\n')
        python.chmod(0o755)
        probe = self.root / 'probe'
        probe.write_text(f'#!{sys.executable}\nimport json,sys,os\nfrom pathlib import Path\n'
            'game=Path(sys.argv[sys.argv.index("--app0")+1])\n'
            'Path(os.environ["BB_DATA_DIR"],"mounted.json").write_text(json.dumps({\n'
            '"path":str(game),"content":(game/"dvdroot_ps4/chr/a.dcx").read_text()}))\n'
            'sys.exit(7)\n')
        probe.chmod(0o755)
        env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
            BB_DATA_DIR=str(self.root), BB_GAME_DIR=str(self.game),
            BB_MODS_DIR=str(self.moddir), BB_MODS_ENABLED='1', BB_MODS_CONFIG=str(self.root/'mods.json'))
        result = subprocess.run(['bash', 'run.sh'], cwd=ROOT, env=env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 7, result.stderr)
        mounted = json.loads((self.root / 'mounted.json').read_text())
        self.assertEqual(mounted['content'], 'mod')
        self.assertFalse(Path(mounted['path']).exists())
        self.assertEqual((self.assets/'a.dcx').read_bytes(), b'original')


class GameplayFiles(unittest.TestCase):
    def test_files_both_players_read_are_gameplay(self):
        for key in ('dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx',
                    'dvdroot_ps4/map/mapstudio/m24_00_00_00.msb.dcx', 'dvdroot_ps4/event/m24_00_00_00.emevd.dcx',
                    'dvdroot_ps4/script/aicommon.luabnd.dcx', 'dvdroot_ps4/chr/c2500.anibnd.dcx'):
            self.assertTrue(mods.changes_gameplay(key), key)
        for key in ('dvdroot_ps4/chr/c2500.chrbnd.dcx', 'dvdroot_ps4/parts/wp_a_6010.partsbnd.dcx',
                    'dvdroot_ps4/sfx/frpg_sfxbnd_m24.ffxbnd.dcx', 'dvdroot_ps4/map/m24/m24_9999.tpf.dcx'):
            self.assertFalse(mods.changes_gameplay(key), key)


GOLD = '{"id":"gold-bolt-paper","name":"Gold Bolt Paper","version":"1.0.0","requires":"0.3.0","features":["hit_effects"]}'
GOLD_TABLE = ('{"version":1,"rows":[{"attacker_speffect":90002,"base_fx":880,"fx":900880},'
              '{"attacker_speffect":90002,"base_fx":10880,"fx":910880}]}')


class Versions(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(mods.parse_version('0.3.0'), (0, 3, 0))
        self.assertEqual(mods.parse_version('0.3.0-alpha.1'), (0, 3, 0))
        self.assertEqual(mods.parse_version('1.2'), (1, 2, 0))
        self.assertEqual(mods.parse_version('10.20.30+meta'), (10, 20, 30))
        for bad in ('', '3', '0.3.', '0.3.0.1', 'v0.3.0', '0.x.0', '0.100000.0'):
            self.assertIsNone(mods.parse_version(bad), bad)


class Manifests(unittest.TestCase):
    def test_parse_and_refusal(self):
        m, e = mods.parse_manifest(GOLD)
        self.assertEqual((m['id'], m['name'], m['version'], m['requires'], m['features']),
                         ('gold-bolt-paper', 'Gold Bolt Paper', '1.0.0', '0.3.0', ['hit_effects']))
        for have in ('0.3.0', '0.3.0-alpha.1', '0.4.1'):
            self.assertEqual(mods.manifest_refusal(m, have), '')
        self.assertEqual(mods.manifest_refusal(m, '0.2.9-beta.1'),
                         'needs Nightwatch 0.3.0 or newer (this is 0.2.9-beta.1)')
        m, e = mods.parse_manifest(b'\xef\xbb\xbf{"id":"a","requires":"0.3","homepage":"x"}')
        self.assertEqual((m['features'], mods.manifest_refusal(m, '0.3.0')), ([], ''))
        m, e = mods.parse_manifest('{"id":"a","requires":"0.3.0","features":["hit_effects","frostbite"]}')
        self.assertEqual(mods.manifest_refusal(m, '0.3.0'),
                         'uses the feature "frostbite", which Nightwatch 0.3.0 does not have')

    def test_bad_manifests(self):
        self.assertEqual(mods.parse_manifest('not json'), (None, 'nightwatch.json is not a JSON object'))
        self.assertIsNone(mods.parse_manifest('[]')[0])
        self.assertEqual(mods.parse_manifest('{"requires":"0.3.0"}')[1], 'nightwatch.json has no "id"')
        self.assertEqual(mods.parse_manifest('{"id":"a"}')[1], 'nightwatch.json has no "requires"')
        for bad in ('{"id":"","requires":"0.3.0"}', '{"id":"a","requires":"soon"}', '{"id":"a","requires":3}',
                    '{"id":"a","requires":"0.3.0","features":"hit_effects"}',
                    '{"id":"a","requires":"0.3.0","features":[1]}', '{"id":"a","requires":"0.3.0","name":5}',
                    '{"id":"a","requires":"0.3.0","x":NaN}'):
            self.assertIsNone(mods.parse_manifest(bad)[0], bad)
        self.assertIsNone(mods.parse_manifest(b'{"id":"a\xff","requires":"0.3.0"}')[0])


class Tables(unittest.TestCase):
    def test_hit_effects(self):
        rows, e = mods.parse_hit_effects(GOLD_TABLE)
        self.assertEqual(rows, [(90002, 880, 900880, 0, 0), (90002, 10880, 910880, 0, 0)])
        self.assertEqual(mods.parse_hit_effects('{"version":1,"rows":[]}'), ([], ''))
        self.assertEqual(mods.parse_hit_effects(
            '{"version":1,"rows":[{"attacker_speffect":1,"base_fx":2,"fx":3,"note":"x"}],"x":1}')[0],
            [(1, 2, 3, 0, 0)])
        self.assertEqual(mods.parse_hit_effects('{"version":1,"rows":[{"sp_attribute":4,"fx":7,"sound":8}]}')[0],
                         [(0, 0, 7, 4, 8)])
        self.assertEqual(mods.parse_hit_effects('{"version":2,"rows":[]}')[1],
                         'nightwatch/hit_effects.json: "version" is not 1')
        for bad in ('{"rows":[]}', '{"version":1}', '{', '{"version":true,"rows":[]}',
                    '{"version":1,"rows":[{"attacker_speffect":0,"base_fx":2,"fx":3}]}',
                    '{"version":1,"rows":[{"attacker_speffect":1.0,"base_fx":2,"fx":3}]}',
                    '{"version":1,"rows":[{"attacker_speffect":1,"base_fx":2,"fx":2147483648}]}',
                    '{"version":1,"rows":[{"attacker_speffect":1,"base_fx":2}]}', '{"version":1,"rows":[5]}',
                    '{"version":1,"rows":[{"sp_attribute":2,"fx":7,"sound":8}]}',
                    '{"version":1,"rows":[{"sp_attribute":6,"fx":7,"sound":8}]}',
                    '{"version":1,"rows":[{"sp_attribute":256,"fx":7,"sound":8}]}',
                    '{"version":1,"rows":[{"sp_attribute":4,"fx":7}]}',
                    '{"version":1,"rows":[{"sp_attribute":4,"fx":7,"sound":8,"base_fx":1}]}'):
            self.assertIsNone(mods.parse_hit_effects(bad)[0], bad)
        rows, e = mods.parse_hit_effects('{"version":1,"rows":[{"attacker_speffect":1,"base_fx":2,"fx":3},{}]}')
        self.assertTrue(e.startswith('nightwatch/hit_effects.json: row 2 needs'))
        many = '{"version":1,"rows":[' + ','.join(['{"attacker_speffect":1,"base_fx":2,"fx":3}'] * 4097) + ']}'
        self.assertIsNone(mods.parse_hit_effects(many)[0])

    def test_hit_effects_merge_text(self):
        merged = {'names': [], 'rows': []}
        self.assertEqual(mods.hit_effects_file_text(merged), '')
        mods.merge_hit_effects(merged, 'A', [(90002, 880, 100, 0, 0), (7, 10880, 200, 0, 0), (0, 0, 5, 4, 6)])
        mods.merge_hit_effects(merged, 'empty', [])
        mods.merge_hit_effects(merged, 'B', [(90002, 880, 111, 0, 0), (0, 0, 9, 4, 10)])
        self.assertEqual(merged['names'], ['A', 'B'])
        self.assertEqual(mods.hit_effects_file_text(merged),
                         '# hit_effects (bb-start; attacker_speffect base_fx fx | attr sp_attribute fx sound, '
                         'newest mod first), from: A, B\n90002 880 111\nattr 4 9 10\n7 10880 200\n')

    def test_weapon_clip_groups(self):
        ok = '{"version":1,"rows":[{"weapon":10000,"clip_group":5},{"weapon":20000,"clip_group":999}]}'
        self.assertEqual(mods.parse_weapon_clip_groups(ok)[0], [(10000, 5), (20000, 999)])
        for bad in ('{"version":1,"rows":[{"weapon":10001,"clip_group":5}]}',
                    '{"version":1,"rows":[{"weapon":0,"clip_group":5}]}',
                    '{"version":1,"rows":[{"weapon":10000,"clip_group":0}]}',
                    '{"version":1,"rows":[{"weapon":10000,"clip_group":1000}]}',
                    '{"version":1,"rows":[{"weapon":2147490000,"clip_group":1}]}',
                    '{"version":1,"rows":[{"weapon":10000}]}', '{"version":0,"rows":[]}'):
            self.assertIsNone(mods.parse_weapon_clip_groups(bad)[0], bad)
        rows, e = mods.parse_weapon_clip_groups(
            '{"version":1,"rows":[{"weapon":10000,"clip_group":5},{"weapon":10000,"clip_group":6}]}')
        self.assertEqual((rows, e), (None, 'nightwatch/weapon_clip_groups.json: row 2 lists a weapon a second time'))

    def test_weapon_clip_groups_merge_text(self):
        merged = {'names': [], 'rows': {}}
        mods.merge_weapon_clip_groups(merged, 'A', [(30000, 1), (10000, 2)])
        mods.merge_weapon_clip_groups(merged, 'B', [(10000, 7)])
        self.assertEqual(mods.weapon_clip_groups_file_text(merged),
                         '# weapon_clip_groups (bb-start; weapon clip_group), from: A, B\n10000 7\n30000 1\n')


class FeatureFiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root / 'out'
        self.out.mkdir()

    def mod(self, name, manifest=None, **tables):
        folder = self.root / name
        (folder / 'dvdroot_ps4' / 'chr').mkdir(parents=True)
        if manifest is not None:
            (folder / 'nightwatch.json').write_text(manifest)
        for feature, text in tables.items():
            (folder / 'nightwatch').mkdir(exist_ok=True)
            (folder / 'nightwatch' / f'{feature}.json').write_text(text)
        return name, folder

    def check(self, layers, version='0.3.0'):
        old = mods.NW_VERSION
        mods.NW_VERSION = version
        self.addCleanup(setattr, mods, 'NW_VERSION', old)
        manifests = {}
        return mods.check_manifests(layers, manifests), manifests

    def test_manifest_gate(self):
        plain = self.mod('plain')
        gold = self.mod('gold', GOLD)
        bad = self.mod('bad', '{"id":')
        future = self.mod('future', '{"id":"f","requires":"0.4.0"}')
        unknown = self.mod('unknown', '{"id":"u","requires":"0.3.0","features":["frostbite"]}')
        kept, manifests = self.check([plain, gold, bad, future, unknown])
        self.assertEqual([n for n, _ in kept], ['plain', 'gold'])
        self.assertEqual(list(manifests), ['gold'])
        kept, _ = self.check([gold, future], '0.3.0-alpha.1')
        self.assertEqual([n for n, _ in kept], ['gold'])
        kept, _ = self.check([gold], '0.2.9')
        self.assertEqual(kept, [])

    def test_hit_effects_file(self):
        a = self.mod('a', GOLD, hit_effects=GOLD_TABLE)
        b = self.mod('b', GOLD, hit_effects='{"version":1,"rows":[{"attacker_speffect":90002,"base_fx":880,"fx":1}]}')
        unlisted = self.mod('c', '{"id":"c","requires":"0.3.0"}', hit_effects=GOLD_TABLE)
        nomanifest = self.mod('d', None, hit_effects=GOLD_TABLE)
        badtable = self.mod('e', GOLD, hit_effects='{"version":9}')
        layers, manifests = self.check([a, unlisted, nomanifest, badtable, b])
        self.assertEqual(len(layers), 5)
        mods.write_hit_effects_file(self.out, layers, manifests)
        raw = (self.out / mods.HIT_EFFECTS_OUT).read_bytes()
        self.assertEqual(raw.decode(),
                         '# hit_effects (bb-start; attacker_speffect base_fx fx | attr sp_attribute fx sound, newest '
                         'mod first), from: a, b\n90002 880 1\n90002 10880 910880\n')
        self.assertNotIn(b'\r', raw)
        mods.write_hit_effects_file(self.out, [unlisted], manifests)
        self.assertFalse((self.out / mods.HIT_EFFECTS_OUT).exists())

    def test_clip_groups_in_the_party_hash_and_hit_effects_not(self):
        clip = '{"version":1,"rows":[{"weapon":10000,"clip_group":5}]}'
        m = '{"id":"m","requires":"0.3.0","features":["weapon_clip_groups","hit_effects"]}'
        plain = self.mod('plain')
        with_tables = self.mod('tables', m, weapon_clip_groups=clip, hit_effects=GOLD_TABLE)
        layers, manifests = self.check([plain, with_tables])
        features = mods.write_weapon_clip_groups_file(self.out, layers, manifests)
        self.assertEqual(features, '10000 5\n')
        self.assertEqual((self.out / mods.WEAPON_CLIP_GROUPS_OUT).read_text(),
                         '# weapon_clip_groups (bb-start; weapon clip_group), from: tables\n10000 5\n')
        base = mods.party_mod_set(layers, None)[0]
        digest = mods.party_mod_set(layers, None, features)[0]
        self.assertNotEqual(base, digest)
        self.assertEqual(mods.party_mod_set([plain], None, '')[0], '0' * 64)  # no rows: hash unchanged
        self.assertNotEqual(mods.party_mod_set([plain], None, features)[0], '0' * 64)
        mods.write_hit_effects_file(self.out, layers, manifests)  # a visual table never changes the hash
        self.assertEqual(mods.party_mod_set(layers, None, features)[0], digest)
        self.assertEqual(mods.party_mod_set(layers, None, '')[0], base)
