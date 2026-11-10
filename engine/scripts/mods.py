#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merge loose-file mods through links, preserving the original game and mod files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile

WINDOWS = os.name == 'nt'

GAME_FOLDERS = {'action', 'chr', 'event', 'facegen', 'font', 'map', 'menu', 'movie', 'msg', 'mtd',
                'obj', 'other', 'param', 'paramdef', 'parts', 'remo', 'script', 'sfx', 'shader',
                'sound'}

SERIALS = ('CUSA00207', 'CUSA00208', 'CUSA00299', 'CUSA00900', 'CUSA01363', 'CUSA03014',
           'CUSA03023', 'CUSA03173', 'CUSA03179')


def child(folder, name):
    """`name` inside `folder`, matching an existing entry case-insensitively (mods made on
    Windows often differ from the game's lower-case names)."""
    folder = Path(folder)
    for entry in (folder.iterdir() if folder.is_dir() else ()):
        if entry.name.casefold() == name.casefold():
            return entry
    return folder / name


def content_root(folder):
    """(root, prefix): where a mod's files are and the game path they go to, or None.

    Accepted layouts: <mod>/dvdroot_ps4, <mod>/app0/dvdroot_ps4, <mod>/<serial>/dvdroot_ps4
    (CUSA03173 or another retail serial), one wrapper folder around any of these (an archive
    extracted into a folder of its name), and the game's folders without dvdroot_ps4 (<mod>/chr,
    <mod>/parts, ...)."""
    folder = Path(folder)
    if not folder.is_dir():
        return None
    for wrapper in ('', 'app0') + SERIALS:
        base = child(folder, wrapper) if wrapper else folder
        dvdroot = child(base, 'dvdroot_ps4')
        if dvdroot.is_dir():
            return base, ''
    entries = [e for e in folder.iterdir() if not e.name.startswith('.')]
    folders = [e for e in entries if e.is_dir()]
    if folders and all(e.name.casefold() in GAME_FOLDERS for e in folders):
        return folder, 'dvdroot_ps4'
    if len(folders) == 1 and not [e for e in entries if e.is_file() and
                                  e.suffix.casefold() not in ('.txt', '.md', '.jpg', '.png', '.ini')]:
        return content_root(folders[0])
    return None


def discover(root):
    """Named mods: folders of `root` with an accepted layout (content_root)."""
    root = Path(root)
    if not root.is_dir():
        return []
    return sorted((p.name for p in root.iterdir() if p.is_dir() and content_root(p)),
                  key=lambda name: (name.casefold(), name))


def selected(root, config):
    available = discover(root)
    if not config or not Path(config).is_file():
        return available
    settings = json.loads(Path(config).read_text())
    disabled_names = settings.get('disabled', [])
    if not isinstance(disabled_names, list) or not all(isinstance(n, str) for n in disabled_names):
        raise ValueError('Disabled mods must be a list of folder names')
    disabled = set(disabled_names)
    order = settings.get('order', [])
    if not isinstance(order, list) or not all(isinstance(n, str) for n in order):
        raise ValueError('Mod order must be a list of folder names')
    return list(dict.fromkeys(n for n in [*order, *available]
                             if n in available and n not in disabled))


def mod_files(folder):
    """(game path, source) of every file a mod replaces or adds under dvdroot_ps4."""
    layout = content_root(folder)
    if not layout:
        raise ValueError(f'{folder}: expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder')
    root, prefix = layout
    root = root.resolve()
    for directory, folders, files in os.walk(root, followlinks=False):
        directory = Path(directory)
        for name in [n for n in folders if (directory / n).is_symlink()]:
            print(f'Mods: skipped the link {directory / name} (links are not followed)', file=sys.stderr)
            folders.remove(name)
        for name in sorted(files):
            source = directory / name
            if source.is_symlink():
                print(f'Mods: skipped the link {source} (links are not followed)', file=sys.stderr)
                continue
            relative = Path(prefix) / source.relative_to(root) if prefix else source.relative_to(root)
            if relative.parts[0].casefold() != 'dvdroot_ps4':
                if relative.parts[0].casefold() in ('eboot.bin', 'sce_module', 'sce_sys'):
                    raise ValueError(f'Executable/system replacement is unsupported: {source}')
                continue
            if not source.is_file():
                raise ValueError(f'Not a regular mod file: {source}')
            yield relative, source


def app_version(folder):
    """APP_VER of a game folder's sce_sys/param.sfo, or None."""
    try:
        from prepare import sfo
        return sfo((Path(folder) / 'sce_sys' / 'param.sfo').read_bytes()).get('APP_VER')
    except Exception:
        return None


def update_folder(game):
    """The dumped game update beside the game, as shadPS4 keeps it (<game>-UPDATE or <game>-patch,
    any case), when it holds an eboot.bin and the game is not already at its version: it is layered
    over the game, which stays unchanged."""
    game = Path(game)
    for entry in (game.parent.iterdir() if game.parent.is_dir() else ()):
        if entry.is_dir() and entry.name.casefold() in (f'{game.name}-update'.casefold(),
                                                         f'{game.name}-patch'.casefold())                 and child(entry, 'eboot.bin').is_file():
            version = app_version(entry)
            if version and app_version(game) == version:
                print(f'Game update: {entry.name} is already in the game ({version})', file=sys.stderr)
                return None
            return entry
    return None


def overlay_base(game, out):
    """Where the overlay is built: `out`, or beside the game when `out` is on another drive (hard
    links work within one volume only; across volumes every linked file was copied)."""
    if WINDOWS and os.path.splitdrive(str(Path(out).resolve()))[0].casefold() !=             os.path.splitdrive(str(game))[0].casefold():
        return game.parent / '.bbport-overlays'
    return Path(out)


def update_files(folder):
    """(game path, source) of every file of a game update: executable and system files too."""
    root = Path(folder).resolve()
    for directory, folders, files in os.walk(root, followlinks=False):
        directory = Path(directory)
        for name in sorted(files):
            source = directory / name
            if source.is_file() and not source.is_symlink():
                yield source.relative_to(root), source


def is_link(path):
    return path.is_symlink() or (WINDOWS and path.is_junction())


def link(path, target):
    """`path` refers to `target` (a directory or a file) without copying the game."""
    if not WINDOWS:
        path.symlink_to(target, target_is_directory=target.is_dir())
    elif target.is_dir():
        import _winapi
        _winapi.CreateJunction(str(target), str(path))
    else:
        try:
            os.link(target, path)
        except OSError:
            shutil.copy2(target, path)


def unlink(path):
    """Removes a link made by `link` (never what it refers to)."""
    if WINDOWS and path.is_junction():
        os.rmdir(path)
        return
    try:
        path.unlink()
    except PermissionError:
        if not WINDOWS or is_link(path):
            raise
        os.chmod(path, stat.S_IWRITE)
        path.unlink()


def remove_overlay(path):
    """Deletes an overlay from build_overlay without following its links."""
    path = Path(path)
    if is_link(path) or not path.is_dir():
        if is_link(path) or path.exists():
            unlink(path)
        return
    for entry in path.iterdir():
        remove_overlay(entry)
    os.rmdir(path)


def expand(directory):
    """Materialize one directory level; never write through a directory link."""
    if is_link(directory):
        target = directory.resolve(strict=True)
        if not target.is_dir():
            raise ValueError(f'File/directory conflict at {directory.name}')
        unlink(directory)
        directory.mkdir()
        for entry in target.iterdir():
            link(directory / entry.name, entry)
    elif directory.exists() and not directory.is_dir():
        raise ValueError(f'File/directory conflict at {directory.name}')
    else:
        directory.mkdir(exist_ok=True)


def remove_stale_overlays(out):
    """Deletes the overlays earlier starts left in `out`: run_windows.py removes its own when the
    game ends, but not when the launcher's Stop (or its "still running" prompt) kills it with the
    game. One still in use (an in-game restart's previous run, stopping) is left for the next start."""
    for overlay in Path(out).glob('mod-game-*'):
        if overlay.is_dir() and not is_link(overlay):
            try:
                remove_overlay(overlay)
            except OSError:
                pass


GAMEPLAY_FOLDERS = ('dvdroot_ps4/param/', 'dvdroot_ps4/event/', 'dvdroot_ps4/script/',
                    'dvdroot_ps4/map/mapstudio/', 'dvdroot_ps4/action/')


def changes_gameplay(key):
    """A mod file the other players of a co-op session read too (parameters, map layouts, events,
    AI and talk scripts, actions, character animations): different ones on each side desync enemies."""
    return key.startswith(GAMEPLAY_FOLDERS) or (
        key.startswith('dvdroot_ps4/chr/') and ('.anibnd' in key or '.behbnd' in key))


def build_overlay(game, out, mods, update=None):
    game = Path(game).resolve(strict=True)
    out = overlay_base(game, out)
    if Path(out).is_dir():
        remove_stale_overlays(out)
    replacements = []
    owners = {}
    if update:
        files = list(update_files(update))
        for relative, source in files:
            owners[relative.as_posix().casefold()] = Path(update).name
            replacements.append((relative, source))
        print(f'Game update: {Path(update).name}: {len(files)} files over the game', file=sys.stderr)
    gameplay = 0
    for name, root in mods:
        count = 0
        for relative, source in mod_files(root):
            key = relative.as_posix().casefold()
            gameplay += changes_gameplay(key)
            if key in owners:
                print(f'Mods: {relative}: {owners[key]} -> {name}', file=sys.stderr)
            owners[key] = name
            replacements.append((relative, source))
            count += 1
        print(f'Mods: {name}: {count} files', file=sys.stderr)
    if gameplay:
        print(f'Mods: {gameplay} of these files change gameplay (parameters, map layouts, events, AI, '
              'animations): co-op partners need the same files, or enemies and items desync between you',
              file=sys.stderr)
    if not replacements:
        return game
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    overlay = Path(tempfile.mkdtemp(prefix='mod-game-', dir=out))
    try:
        for entry in game.iterdir():
            link(overlay / entry.name, entry)
        replaced = added = 0
        for relative, source in replacements:
            parent = overlay
            for part in relative.parts[:-1]:
                parent = child(parent, part)
                expand(parent)
            destination = child(parent, relative.parts[-1])
            if destination.is_symlink():
                replaced += destination.resolve().is_relative_to(game)
                destination.unlink()
            elif WINDOWS and destination.is_file():
                replaced += 1
                unlink(destination)
            elif destination.exists():
                raise ValueError(f'File/directory conflict: {relative}')
            else:
                added += 1
            link(destination, source)
        print(f'Mods: {replaced} game files replaced, {added} added', file=sys.stderr)
        return overlay
    except BaseException:
        remove_overlay(overlay)
        raise


COSMETIC_FOLDERS = {'adhoc', 'facegen', 'font', 'menu', 'movie', 'msg', 'mtd', 'parts', 'sfx', 'shader', 'sound'}
COSMETIC_SUFFIXES = ('.tpf', '.tpf.dcx', '.tpfbhd', '.tpfbdt', '.texbnd', '.texbnd.dcx', '.dds')
PARTY_MODS_FILE = 'party_mods.txt'
PARTY_MODS_CACHE = 'party_mods_cache.json'
# Generated layers (name: stamp), hashed by stamp: scripts/dream_mirror.py LAYER_NAME, STAMP.
GENERATED_STAMPS = {'Dream mirror (bbhost)': 'dream-mirror 1'}


def mod_file_is_cosmetic(relative):
    """True when the game file `relative` (dvdroot_ps4/...) changes no gameplay."""
    parts = [p.casefold() for p in Path(relative).parts]
    if len(parts) > 2 and parts[1] in COSMETIC_FOLDERS:
        return True
    return parts[-1].endswith(COSMETIC_SUFFIXES)


def file_sha256(path, cache):
    stat = path.stat()
    key = str(path)
    hit = cache.get(key)
    if hit and hit[0] == stat.st_size and hit[1] == stat.st_mtime_ns:
        return hit[2]
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    cache[key] = [stat.st_size, stat.st_mtime_ns, digest.hexdigest()]
    return cache[key][2]


def party_mod_set(layers, cache_file=None, features=''):
    """(hash hex, gameplay items, cosmetic mod names) of the mods `layers` ([(name, folder)], load
    order). The hash covers the winning gameplay files (game path, contents), not folder names, then
    `features`: the gameplay feature tables' rows (start/mods.cpp WritePartyModsFile); '' adds nothing."""
    owners = {}
    for name, root in layers:
        for relative, source in mod_files(root):
            owners[relative.as_posix().casefold()] = (name, source)
    cache = {}
    if cache_file and Path(cache_file).is_file():
        try:
            cache = json.loads(Path(cache_file).read_text(encoding='utf-8'))
        except ValueError:
            cache = {}
    digest = hashlib.sha256(b'bbparty-mods-1')
    counts, cosmetic = {}, set()
    for key in sorted(owners):
        name, source = owners[key]
        if mod_file_is_cosmetic(key):
            cosmetic.add(name)
            continue
        counts[name] = counts.get(name, 0) + 1
        # The generated mirror layout is the same file everywhere (a SHA-256-checked rewrite), but
        # its DCX bytes depend on the zlib build: it counts by its stamp.
        content = GENERATED_STAMPS.get(name) or file_sha256(source, cache)
        digest.update(b'\0file\0' + key.encode('utf-8') + b'\0' + content.encode())
    if features:
        digest.update(b'\0features\0' + features.encode('utf-8'))
    if cache_file:
        try:
            Path(cache_file).write_text(json.dumps(cache), encoding='utf-8')
        except OSError:
            pass
    items = [f'{name} ({counts[name]} files)' for name, _ in layers if name in counts]
    if not counts and not features:
        return '0' * 64, [], sorted(cosmetic - set(counts))
    return digest.hexdigest(), items, sorted(cosmetic - set(counts))


def write_party_mods_file(out, layers, features=''):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    digest, items, cosmetic = party_mod_set(layers, out / PARTY_MODS_CACHE, features)
    lines = ['# Party version check (scripts/mods.py): the enabled mods that change gameplay files. Players',
             '# with another hash are refused by the party host; texture/shader/sound-only mods do not count.',
             f'hash {digest}']
    lines += [f'mod {item}' for item in items]
    lines += [f'# cosmetic {name}' for name in cosmetic]
    (out / PARTY_MODS_FILE).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return digest, items


# ---- Mod manifests and feature tables (start/features.h, start/mods.cpp CheckManifests; docs/modding/manifest.md) ----
# Mirrors the native start: a mod's nightwatch.json beside dvdroot_ps4, its nightwatch/<feature>.json tables, merged
# in load order into out/ files of the same text format.
MANIFEST = 'nightwatch.json'
TABLES_DIR = 'nightwatch'
MAX_FILE_BYTES = 1 << 20
HIT_EFFECTS = 'hit_effects'
HIT_EFFECTS_OUT = 'nightwatch_hit_effects.txt'
MAX_HIT_ROWS = 4096
WEAPON_CLIP_GROUPS = 'weapon_clip_groups'
WEAPON_CLIP_GROUPS_OUT = 'nightwatch_weapon_clip_groups.txt'
MAX_CLIP_ROWS = 4096
SOUNDS = 'sounds'
SOUNDS_OUT = 'nightwatch_sounds.txt'
MAX_SOUND_ROWS = 256
MAX_SOUND_FILE_BYTES = 4 << 20
KNOWN_FEATURES = (HIT_EFFECTS, WEAPON_CLIP_GROUPS, SOUNDS)


def _nightwatch_version():
    """NW_VERSION as the native start has it: the repo's VERSION text (first line); the NW_VERSION
    environment variable overrides it."""
    given = os.environ.get('NW_VERSION')
    if given:
        return given
    try:
        with open(Path(__file__).resolve().parents[2] / 'VERSION', encoding='utf-8') as f:
            return f.readline().strip('\r\n')
    except OSError:
        return '0.0.0-unknown'


NW_VERSION = _nightwatch_version()


def parse_version(text):
    """(major, minor, patch) of "X.Y.Z" with an optional "-suffix" / "+meta" (ignored); also "X.Y"
    (patch 0). None for anything else (features.h ParseVersion)."""
    cut = min((i for i in (text.find('-'), text.find('+')) if i >= 0), default=len(text))
    text = text[:cut]
    parts = []
    i = 0
    while True:
        if len(parts) == 3 or i >= len(text) or not '0' <= text[i] <= '9':
            return None
        n = 0
        while i < len(text) and '0' <= text[i] <= '9':
            n = n * 10 + int(text[i])
            if n > 99999:
                return None
            i += 1
        parts.append(n)
        if i == len(text):
            break
        if text[i] != '.':
            return None
        i += 1
    if len(parts) < 2:
        return None
    return tuple(parts + [0] * (3 - len(parts)))


def _strict_json(data):
    """The parsed JSON of bytes / text, or None when it is not JSON (a BOM is skipped; NaN and
    Infinity are not JSON)."""
    if isinstance(data, bytes):
        try:
            data = data.decode('utf-8')
        except UnicodeDecodeError:
            return None
    data = data.removeprefix('﻿')

    def refuse(_):
        raise ValueError

    try:
        return json.loads(data, parse_constant=refuse)
    except (ValueError, RecursionError):
        return None


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def parse_manifest(data):
    """(manifest dict, error): nightwatch.json is an object with string `id` and `requires` (a
    version), optional strings `name`, `version`, `author` and an optional list of feature names.
    Other keys are ignored."""
    size = len(data.encode('utf-8')) if isinstance(data, str) else len(data)
    if size > MAX_FILE_BYTES:
        return None, f'{MANIFEST} is larger than 1 MiB'
    j = _strict_json(data)
    if not isinstance(j, dict):
        return None, f'{MANIFEST} is not a JSON object'
    m = {'id': '', 'name': '', 'version': '', 'author': '', 'requires': '', 'features': []}
    for key, field, required in (('id', 'id', True), ('requires', 'requires', True), ('name', 'name', False),
                                 ('version', 'version', False), ('author', 'author', False)):
        if key not in j:
            if required:
                return None, f'{MANIFEST} has no "{key}"'
            continue
        value = j[key]
        if not isinstance(value, str) or (required and not value):
            return None, f'{MANIFEST}: "{key}" is not a{" non-empty" if required else ""} string'
        m[field] = value
    if parse_version(m['requires']) is None:
        return None, f'{MANIFEST}: "requires" is not a version like 0.3.0: {m["requires"]}'
    if 'features' in j:
        if not isinstance(j['features'], list):
            return None, f'{MANIFEST}: "features" is not a list'
        for f in j['features']:
            if not isinstance(f, str) or not f:
                return None, f'{MANIFEST}: "features" holds something other than a feature name'
            m['features'].append(f)
    return m, ''


def manifest_refusal(m, nightwatch=None):
    """Why this Nightwatch cannot load the mod with manifest `m`, or '' when it can."""
    nightwatch = NW_VERSION if nightwatch is None else nightwatch
    need = parse_version(m['requires'])
    have = parse_version(nightwatch)
    if need is None:
        return f'"requires" is not a version: {m["requires"]}'
    if have is None:
        return f'this Nightwatch has no version ({nightwatch})'
    if have < need:
        return f'needs Nightwatch {m["requires"]} or newer (this is {nightwatch})'
    for f in m['features']:
        if f not in KNOWN_FEATURES:
            return f'uses the feature "{f}", which Nightwatch {nightwatch} does not have'
    return ''


def _log(text):
    print(text, file=sys.stderr)


def check_manifests(layers, manifests):
    """The layers ([(name, folder)]) without those whose nightwatch.json is bad, needs a newer
    Nightwatch or names an unknown feature (one log line each). `manifests` (name: manifest) gets
    the kept layers' manifests; a mod without one loads as before."""
    kept = []
    for name, root in layers:
        path = Path(root) / MANIFEST
        if not path.is_file():
            kept.append((name, root))
            continue
        try:
            m, why = parse_manifest(path.read_bytes())
            if m is None:
                why = f'bad manifest: {why}'
        except OSError as error:
            m, why = None, f'cannot read {MANIFEST}: {error}'
        if m is not None:
            why = manifest_refusal(m)
        if why:
            _log(f'Mods: {name}: {why}: not loaded')
            continue
        _log(f'Mods: {name}: manifest {m["id"]} {m["version"] or "-"}, requires {m["requires"]}, '
             f'features {", ".join(m["features"]) or "none"}')
        manifests[name] = m
        kept.append((name, root))
    return kept


def _table_header(data, name, what):
    """(rows list, error) of a feature table: {"version": 1, "rows": [...]}."""
    file = f'{TABLES_DIR}/{name}.json'
    if len(data) > MAX_FILE_BYTES:
        return None, f'{file} is larger than 1 MiB'
    j = _strict_json(data)
    if not isinstance(j, dict):
        return None, f'{file} is not a JSON object'
    if not _is_int(j.get('version')) or j['version'] != 1:
        return None, f'{file}: "version" is not 1'
    if not isinstance(j.get('rows'), list):
        return None, f'{file}: "rows" is not a list'
    if len(j['rows']) > what:
        return None, f'{file}: more than {what} rows'
    return j['rows'], ''


def _whole(row, key, lo=1, hi=0x7FFFFFFF):
    value = row.get(key) if isinstance(row, dict) else None
    return value if _is_int(value) and lo <= value <= hi else None


def parse_hit_effects(data):
    """(rows, error) of nightwatch/hit_effects.json. A row is (attacker_speffect, base_fx, fx,
    sp_attribute, sound): a swap {"attacker_speffect", "base_fx", "fx"} or a draw {"sp_attribute"
    (1..255, not 2 / 6), "fx", "sound"} (sp_attribute != 0), each a whole number 1..2147483647."""
    file = f'{TABLES_DIR}/{HIT_EFFECTS}.json'
    rows, error = _table_header(data, HIT_EFFECTS, MAX_HIT_ROWS)
    if rows is None:
        return None, error
    out = []
    for number, r in enumerate(rows, 1):
        if isinstance(r, dict) and 'sp_attribute' in r:
            attr, fx, sound = _whole(r, 'sp_attribute'), _whole(r, 'fx'), _whole(r, 'sound')
            if (attr is None or attr > 255 or attr in (2, 6) or fx is None or sound is None
                    or 'attacker_speffect' in r or 'base_fx' in r):
                return None, (f'{file}: row {number} (a draw row) needs sp_attribute 1..255 but not 2 or 6, and whole '
                              'numbers 1..2147483647 for fx and sound (no attacker_speffect or base_fx)')
            out.append((0, 0, fx, attr, sound))
        else:
            sp, base, fx = _whole(r, 'attacker_speffect'), _whole(r, 'base_fx'), _whole(r, 'fx')
            if sp is None or base is None or fx is None:
                return None, (f'{file}: row {number} needs whole numbers 1..2147483647 for attacker_speffect, '
                              'base_fx and fx')
            out.append((sp, base, fx, 0, 0))
    return out, ''


def _same_hit_key(a, b):
    if a[3]:
        return a[3] == b[3]
    return not b[3] and a[0] == b[0] and a[1] == b[1]


def merge_hit_effects(merged, name, own):
    """merged = {'names': [], 'rows': [(row, mod index)]}: a later mod's row replaces one with the same key."""
    if not own:
        return
    index = len(merged['names'])
    merged['names'].append(name)
    for row in own:
        merged['rows'] = [r for r in merged['rows'] if not _same_hit_key(r[0], row)]
        merged['rows'].append((row, index))


def hit_effects_file_text(merged):
    """out/nightwatch_hit_effects.txt text, the newest mod first; '' when there are no rows."""
    if not merged['rows']:
        return ''
    rows = sorted(merged['rows'], key=lambda r: -r[1])[:MAX_HIT_ROWS]  # stable
    lines = ['# hit_effects (bb-start; attacker_speffect base_fx fx | attr sp_attribute fx sound, newest mod first), '
             'from: ' + ', '.join(merged['names'])]
    for (sp, base, fx, attr, sound), _ in rows:
        lines.append(f'attr {attr} {fx} {sound}' if attr else f'{sp} {base} {fx}')
    return '\n'.join(lines) + '\n'


def parse_weapon_clip_groups(data):
    """(rows, error) of nightwatch/weapon_clip_groups.json: rows (weapon, clip_group), weapon a multiple of
    10000 (10000..2147480000), clip_group 1..999; a weapon listed twice in one table is a bad row."""
    file = f'{TABLES_DIR}/{WEAPON_CLIP_GROUPS}.json'
    rows, error = _table_header(data, WEAPON_CLIP_GROUPS, MAX_CLIP_ROWS)
    if rows is None:
        return None, error
    out = []
    for number, r in enumerate(rows, 1):
        weapon, group = _whole(r, 'weapon', 10000, 2147480000), _whole(r, 'clip_group', 1, 999)
        ok = weapon is not None and weapon % 10000 == 0 and group is not None
        twice = ok and any(w == weapon for w, _ in out)
        if not ok or twice:
            return None, f'{file}: row {number} ' + (
                'lists a weapon a second time' if twice
                else 'needs "weapon" (a multiple of 10000) and "clip_group" (1..999)')
        out.append((weapon, group))
    return out, ''


def merge_weapon_clip_groups(merged, name, own):
    """merged = {'names': [], 'rows': {weapon: group}}: a later mod's row replaces one for the same weapon."""
    if not own:
        return
    merged['names'].append(name)
    merged['rows'].update(own)
    # the native merge keeps the first 4096 rows by weapon
    for weapon in sorted(merged['rows'])[MAX_CLIP_ROWS:]:
        del merged['rows'][weapon]


def weapon_clip_groups_file_text(merged):
    """out/nightwatch_weapon_clip_groups.txt text, by weapon; '' when there are no rows."""
    if not merged['rows']:
        return ''
    lines = ['# weapon_clip_groups (bb-start; weapon clip_group), from: ' + ', '.join(merged['names'])]
    lines += [f'{weapon} {merged["rows"][weapon]}' for weapon in sorted(merged['rows'])]
    return '\n'.join(lines) + '\n'


def is_sound_event(e):
    """An event name the game builds: one of the letters "acfopsmvxbgyz" and exactly 9 digits."""
    return (isinstance(e, str) and len(e) == 10 and e[0] in 'acfopsmvxbgyz'
            and all('0' <= c <= '9' for c in e[1:]))


def sound_file_path(p):
    """A relative file path inside the mod ('\\' becomes '/'), or None: not empty, at most 200 bytes, no
    drive, no leading slash, no '.' / '..' / empty part, no control characters."""
    if not isinstance(p, str) or not p or len(p.encode('utf-8')) > 200:
        return None
    p = p.replace('\\', '/')
    if p[0] == '/' or ':' in p or any(ord(c) < 0x20 for c in p):
        return None
    if any(part in ('', '.', '..') for part in p.split('/')):
        return None
    return p


def parse_sounds(data):
    """(rows, error) of nightwatch/sounds.json: rows (event, file, volume, replace); volume 0..4 (default
    1), replace true / false (default false); an event listed twice in one table is a bad row."""
    file = f'{TABLES_DIR}/{SOUNDS}.json'
    rows, error = _table_header(data, SOUNDS, MAX_SOUND_ROWS)
    if rows is None:
        return None, error
    out = []
    for number, r in enumerate(rows, 1):
        ok = isinstance(r, dict) and is_sound_event(r.get('event'))
        path = sound_file_path(r.get('file')) if ok else None
        ok = ok and path is not None
        volume = r.get('volume', 1.0) if ok else None
        ok = ok and isinstance(volume, (int, float)) and not isinstance(volume, bool) and 0 <= volume <= 4
        replace = r.get('replace', False) if ok else None
        ok = ok and isinstance(replace, bool)
        twice = ok and any(e == r['event'] for e, _, _, _ in out)
        if not ok or twice:
            return None, f'{file}: row {number} ' + (
                'lists an event a second time' if twice
                else 'needs "event" (e.g. "s000252531"), "file" (a path inside the mod), "volume" 0..4 and '
                     '"replace" true/false')
        out.append((r['event'], path, float(volume), replace))
    return out, ''


def sounds_file_text(merged):
    """out/nightwatch_sounds.txt text, by event; '' when there are no rows."""
    if not merged['rows']:
        return ''
    lines = ['# sounds (bb-start; event volume replace file), from: ' + ', '.join(merged['names'])]
    for event in sorted(merged['rows'])[:MAX_SOUND_ROWS]:
        file, volume, replace = merged['rows'][event]
        lines.append(f'{event} {volume:.3f} {1 if replace else 0} {file}')
    return '\n'.join(lines) + '\n'


def _read_tables(layers, manifests, feature, parse, merge, merged):
    for name, root in layers:
        table = Path(root) / TABLES_DIR / f'{feature}.json'
        if not table.is_file():
            continue
        m = manifests.get(name)
        if m is None or feature not in m['features']:
            _log(f'Mods: {name}: has nightwatch/{feature}.json but its nightwatch.json does not list {feature}: '
                 'not used')
            continue
        try:
            rows, error = parse(table.read_bytes())
        except OSError as e:
            _log(f'Mods: {table}: {e}: its {feature} table is not used')
            continue
        if rows is None:
            _log(f'Mods: {name}: {error}: its {feature} table is not used')
            continue
        merge(merged, name, rows)


def _write_feature_file(out, file, text):
    path = Path(out) / file
    if not text:
        path.unlink(missing_ok=True)
        return
    path.write_text(text, encoding='utf-8', newline='\n')


def write_hit_effects_file(out, layers, manifests):
    """out/nightwatch_hit_effects.txt from the layers' nightwatch/hit_effects.json (only for a manifest
    listing the feature). A visual feature: not in the party hash."""
    merged = {'names': [], 'rows': []}
    _read_tables(layers, manifests, HIT_EFFECTS, parse_hit_effects, merge_hit_effects, merged)
    text = hit_effects_file_text(merged)
    _write_feature_file(out, HIT_EFFECTS_OUT, text)
    if text:
        _log(f'Mods: hit_effects: {len(merged["rows"])} rows from {len(merged["names"])} mods')
    return text


def write_weapon_clip_groups_file(out, layers, manifests):
    """out/nightwatch_weapon_clip_groups.txt likewise. Gameplay: returns the rows ("weapon group" lines,
    no comment line) for the party hash, '' when there are none."""
    merged = {'names': [], 'rows': {}}
    _read_tables(layers, manifests, WEAPON_CLIP_GROUPS, parse_weapon_clip_groups, merge_weapon_clip_groups, merged)
    text = weapon_clip_groups_file_text(merged)
    _write_feature_file(out, WEAPON_CLIP_GROUPS_OUT, text)
    if not text:
        return ''
    _log(f'Mods: {WEAPON_CLIP_GROUPS}: {len(merged["rows"])} rows from {len(merged["names"])} mods')
    return text[text.index('\n') + 1:]


def write_sounds_file(out, layers, manifests):
    """out/nightwatch_sounds.txt from the layers' nightwatch/sounds.json (only for a manifest listing the
    feature). A row whose file is missing or not 1 byte..4 MiB is dropped (one log line). Cosmetic: not in
    the party hash."""
    roots = dict(layers)
    merged = {'names': [], 'rows': {}}

    def merge(merged, name, own):
        found = {}
        for event, file, volume, replace in own:
            path = (Path(roots[name]) / file).resolve()
            size = path.stat().st_size if path.is_file() else 0
            if not 0 < size <= MAX_SOUND_FILE_BYTES:
                _log(f'Mods: {name}: sounds: {event}: {file} is missing or not a file of 1 byte..4 MiB: row not used')
                continue
            found[event] = (str(path), volume, replace)
        if found:
            merged['names'].append(name)
            merged['rows'].update(found)

    _read_tables(layers, manifests, SOUNDS, parse_sounds, merge, merged)
    text = sounds_file_text(merged)
    _write_feature_file(out, SOUNDS_OUT, text)
    if text:
        _log(f'Mods: {SOUNDS}: {len(merged["rows"])} rows from {len(merged["names"])} mods')
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--mods-dir', required=True, type=Path)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--enabled', choices=('0', '1'), default='1')
    args = parser.parse_args()
    layers = []
    if args.enabled == '1':
        legacy = Path(str(args.game.resolve()) + '-mods')
        if legacy.is_dir():
            layers.append((legacy.name, legacy))
        if child(args.mods_dir, 'dvdroot_ps4').is_dir():
            layers.append((args.mods_dir.name, args.mods_dir))
        else:
            for name in selected(args.mods_dir, args.config):
                layers.append((name, args.mods_dir / name))
    args.out.mkdir(parents=True, exist_ok=True)
    for stale in (HIT_EFFECTS_OUT, WEAPON_CLIP_GROUPS_OUT, SOUNDS_OUT):  # a failed start: vanilla look, clips, sounds
        (args.out / stale).unlink(missing_ok=True)
    manifests = {}
    layers = check_manifests(layers, manifests)
    print(f'Mods: Nightwatch {NW_VERSION}, features: {", ".join(KNOWN_FEATURES)}', file=sys.stderr)
    write_party_mods_file(args.out, layers, write_weapon_clip_groups_file(args.out, layers, manifests))
    view = build_overlay(args.game, args.out, layers, update_folder(args.game.resolve()))
    write_hit_effects_file(args.out, layers, manifests)
    write_sounds_file(args.out, layers, manifests)
    print(view)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError) as error:
        print(f'Mods: {error}', file=sys.stderr)
        sys.exit(1)
