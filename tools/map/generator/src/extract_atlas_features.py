"""Read local Bloodborne data for Atlas features using only the standard library.

The packaged preparation step runs this after collision generation. No game data
is shipped; this module never writes to the game directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import zlib
from pathlib import Path

SCHEMA = 1
SUPPORTED_EBOOT = 'd65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9'
AREA_BOSSES = {
    'm22_00_00_00': [12201800], 'm23_00_00_00': [12301800],
    'm24_00_00_00': [12401800], 'm24_01_00_00': [12411800],
    'm24_02_00_00': [12421700, 12421800], 'm25_00_00_00': [12501800],
    'm26_00_00_00': [12601800], 'm27_00_00_00': [12701800],
    'm28_00_00_00': [12801800], 'm32_00_00_00': [13201800],
    'm33_00_00_00': [13301800], 'm34_00_00_00': [13401800],
    'm35_00_00_00': [13501800, 13501850], 'm36_00_00_00': [13601800],
}
TALK_HASHES = {
    't241319.esd': '29eed47fcc4df17de89ee812c5b56b99312f6802470736402efa7e7238ad917d',
    't241325.esd': 'd1cdf488809659cf7b3736c1c0671e61d70d286614532e14cabb186fadc17262',
    't240325.esd': 'db17ce48f3b712c33928d450058c3cc88aa29f969fc90f6c0a49e26a85d05549',
}


def isflag(flag, on=True):
    return ['==', ['flag', flag], int(on)]


def combine(op, *values):
    if not values:
        return 1 if op == 'and' else 0
    value = values[0]
    for other in values[1:]:
        value = [op, value, other]
    return value


def npc_rules(mid, root):
    """Reviewed completion conditions from the local ESD and quest events.

    Deliberately omit stages whose live site or prerequisite is unverified.
    Repeat-only branches have no new-dialogue rule.
    """
    rules = {}
    if mid == 'm24_01_00_00':
        clauses = [
            combine('and', isflag(1240), isflag(72410500, False)),
            combine('and', isflag(1240), isflag(72410500), isflag(72410501, False)),
            combine('and', isflag(1240), isflag(72410501), isflag(72410511), isflag(72410502, False)),
            combine('and', isflag(1241), isflag(72410500, False)),
            combine('and', isflag(1241), isflag(72410500), isflag(72410503, False)),
            combine('and', isflag(1242), isflag(72410500, False)),
            combine('and', isflag(1242), isflag(72410500), isflag(72410504, False)),
            combine('and', isflag(1243), isflag(72410505, False)),
        ]
        rules['2410780'] = combine('and', isflag(1439, False), combine('or', *clauses))
        rules['2410900'] = combine('or',
            combine('and', isflag(1360), isflag(72410540, False)),
            combine('and', isflag(1361), isflag(72410541, False)))
    if mid == 'm24_00_00_00':
        rules['2400900'] = combine('or',
            combine('and', isflag(1362), isflag(72400520, False)),
            combine('and', isflag(1363), isflag(72400521, False)))
    if rules:
        talks = binder(root / 'script/talk' / (mid + '.talkesdbnd.dcx'))
        scripts = {'2410780': 't241319.esd', '2410900': 't241325.esd', '2400900': 't240325.esd'}
        rules = {entity: rule for entity, rule in rules.items()
                 if scripts[entity] in talks and hashlib.sha256(talks[scripts[entity]][1]).hexdigest() == TALK_HASHES[scripts[entity]]}
    return rules


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def raw(path):
    b = Path(path).read_bytes()
    if b[:4] == b'DCX\0':
        at = b.index(b'DCA\0') + 8
        b = zlib.decompress(b[at:])
    return b


def text(b, at, unicode=True):
    if at < 0 or at >= len(b):
        raise ValueError('Invalid string offset')
    if not unicode:
        return b[at:b.index(b'\0', at)].decode('shift-jis')
    end = at
    while end + 2 <= len(b) and b[end:end + 2] != b'\0\0':
        end += 2
    if end + 2 > len(b):
        raise ValueError('Unterminated string')
    return b[at:end].decode('utf-16-le')


def binder(path):
    b = raw(path)
    if b[:4] != b'BND4' or b[9] != 0:
        raise ValueError('Unsupported binder')
    count = struct.unpack_from('<I', b, 12)[0]
    step = struct.unpack_from('<Q', b, 32)[0]
    flags = b[49]
    if b[10]:
        flags = int(f'{flags:08b}'[::-1], 2)
    if count > 100000 or step not in (36, 40):
        raise ValueError('Unsupported binder entries')
    out = {}
    for i in range(count):
        at = 64 + i * step
        size = struct.unpack_from('<Q', b, at + 8)[0]
        cursor = at + (24 if flags & 32 else 16)
        fmt = '<Q' if flags & 16 else '<I'
        offset = struct.unpack_from(fmt, b, cursor)[0]
        cursor += 8 if flags & 16 else 4
        eid = struct.unpack_from('<i', b, cursor)[0] if flags & 2 else i
        cursor += 4 if flags & 2 else 0
        no = struct.unpack_from('<I', b, cursor)[0]
        name = text(b, no, bool(b[48])).replace('\\', '/').rsplit('/', 1)[-1]
        if offset + size > len(b):
            raise ValueError('Truncated binder entry')
        data = b[offset:offset + size]
        entry_flags = b[at] if not b[10] else int(f'{b[at]:08b}'[::-1], 2)
        if entry_flags & 1:
            data = zlib.decompress(data)
        out[name] = (eid, data)
    return out


def params(b):
    if b[44:46] != b'\0\x04':
        raise ValueError('Unsupported parameter row layout')
    count = struct.unpack_from('<H', b, 10)[0]
    headers = [struct.unpack_from('<iiqq', b, 64 + 24 * i) for i in range(count)]
    starts = sorted({h[2] for h in headers} | {len(b)})
    return {rid: b[off:next(x for x in starts if x > off)] for rid, _, off, _ in headers}


def fmg(b):
    if b[2] != 2 or b[1]:
        raise ValueError('Unsupported FMG layout')
    nr, ns = struct.unpack_from('<II', b, 12)
    offsets_at = struct.unpack_from('<Q', b, 24)[0]
    if nr > 100000 or ns > 1000000:
        raise ValueError('FMG limits exceeded')
    offsets = struct.unpack_from('<' + str(ns) + 'Q', b, offsets_at)
    out = {}
    for i in range(nr):
        index, first, last = struct.unpack_from('<3i', b, 40 + 16 * i)
        for rid in range(first, last + 1):
            at = offsets[index + rid - first]
            out[rid] = text(b, at) if at else ''
    return out


def msb(path):
    b = raw(path)
    lists = {}
    off = 16
    visited = set()
    while off:
        if off in visited:
            raise ValueError('Cyclic MSB sections')
        visited.add(off)
        _, count, no = struct.unpack_from('<iiq', b, off)
        offsets = struct.unpack_from('<' + str(count) + 'q', b, off + 16)
        lists[text(b, no)] = offsets[:-1]
        off = offsets[-1]
    models = [text(b, o + struct.unpack_from('<q', b, o)[0]) for o in lists['MODEL_PARAM_ST']]
    parts = []
    for o in lists['PARTS_PARAM_ST']:
        desc, name = struct.unpack_from('<qq', b, o)
        kind, model = struct.unpack_from('<i', b, o + 20)[0], struct.unpack_from('<i', b, o + 28)[0]
        common, sub = struct.unpack_from('<qq', b, o + 176)
        p = dict(name=text(b, o + name), description=text(b, o + desc), kind=kind,
                 model=models[model] if model >= 0 else '', pos=list(struct.unpack_from('<3f', b, o + 40)),
                 entity=struct.unpack_from('<i', b, o + common)[0])
        if kind in (2, 10):
            p['talk_id'] = struct.unpack_from('<i', b, o + sub + 16)[0]
        parts.append(p)
    treasures = []
    for o in lists.get('EVENT_PARAM_ST', []):
        if struct.unpack_from('<i', b, o + 12)[0] != 4:
            continue
        sub = o + struct.unpack_from('<q', b, o + 32)[0]
        part = struct.unpack_from('<i', b, sub + 8)[0]
        if 0 <= part < len(parts):
            treasures.append(dict(part=parts[part], lots=list(struct.unpack_from('<3i', b, sub + 16)),
                                  hidden=bool(b[sub + 65]), chest=bool(b[sub + 64])))
    for o in lists.get('POINT_PARAM_ST', []):
        common = struct.unpack_from('<q', b, o + 72)[0]
        entity = struct.unpack_from('<i', b, o + common)[0]
        parts.append(dict(kind=-2, entity=entity, pos=list(struct.unpack_from('<3f', b, o + 20)),
                          name=text(b, o + struct.unpack_from('<q', b, o)[0]), description='', model=''))
    for o in lists.get('EVENT_PARAM_ST', []):
        if struct.unpack_from('<i', b, o + 12)[0] != 8:
            continue
        common, sub = struct.unpack_from('<qq', b, o + 24)
        region = struct.unpack_from('<i', b, o + sub)[0]
        if 0 <= region < len(lists['POINT_PARAM_ST']):
            p = lists['POINT_PARAM_ST'][region]
            parts.append(dict(kind=-3, entity=struct.unpack_from('<i', b, o + common + 8)[0],
                              pos=list(struct.unpack_from('<3f', b, p + 20)),
                              name=text(b, o + struct.unpack_from('<q', b, o)[0]), description='', model=''))
    return parts, treasures


def events(path):
    b = raw(path)
    h = struct.unpack_from('<16Q', b, 16)
    out = {}
    for j in range(h[0]):
        eid, count, local, nrepl, repl, _ = struct.unpack_from('<5QI', b, h[1] + 48 * j)
        replacements = [struct.unpack_from('<3Qii', b, h[9] + repl + 32 * k) for k in range(nrepl)]
        rows = []
        for line in range(count):
            bank, index, size, arg, _ = struct.unpack_from('<IIQqq', b, h[3] + local + line * 32)
            rows.append(dict(bank=bank, index=index, data=b[h[13] + arg:h[13] + arg + size],
                             replacements=[v[1:4] for v in replacements if v[0] == line]))
        out[eid] = rows
    return out


def event_calls(event_map):
    result, seen = [], set()
    def walk(eid, args=b'', depth=0):
        if (eid, args) in seen or depth > 12:
            return
        seen.add((eid, args))
        for row in event_map.get(eid, []):
            data = bytearray(row['data'])
            if any(source + size > len(args) for dest, source, size in row['replacements']):
                continue
            for dest, source, size in row['replacements']:
                data[dest:dest + size] = args[source:source + size]
            if (row['bank'], row['index']) == (2000, 0) and len(data) >= 8:
                slot, target = struct.unpack_from('<II', data)
                result.append((target, slot, bytes(data[8:])))
                walk(target, bytes(data[8:]), depth + 1)
    walk(0)
    walk(50)
    return result


def esd(b):
    if b[:4] != b'fsSL':
        raise ValueError('Unsupported talk layout')
    d = b[108:]
    machines, n = struct.unpack_from('<qq', d, 24)
    def commands(off, count):
        result = []
        for j in range(count):
            bank, idx, args, argc = struct.unpack_from('<iiqq', d, off + 24 * j)
            values = []
            for k in range(argc):
                at, size = struct.unpack_from('<qq', d, args + 16 * k)
                values.append(d[at:at + size])
            result.append((bank, idx, values))
        return result
    def condition(off, depth=0):
        if depth > 64:
            raise ValueError('Talk condition nesting exceeds limit')
        v = struct.unpack_from('<7q', d, off)
        return dict(next=struct.unpack_from('<q', d, v[0])[0] if v[0] > 0 else -1,
                    expr=d[v[5]:v[5] + v[6]], commands=commands(v[1], v[2]),
                    children=[condition(struct.unpack_from('<q', d, v[3] + 8 * k)[0], depth + 1) for k in range(v[4])])
    result = {}
    for i in range(n):
        mid, off, count, _ = struct.unpack_from('<4q', d, machines + i * 32)
        states = {}
        for j in range(count):
            v = struct.unpack_from('<9q', d, off + j * 72)
            states[v[0]] = dict(conditions=[condition(struct.unpack_from('<q', d, v[1] + k * 8)[0]) for k in range(v[2])],
                               enter=commands(v[3], v[4]), exit=commands(v[5], v[6]), ongoing=commands(v[7], v[8]))
        result[mid] = states
    return result


def constant(expr):
    stack, i = [], 0
    try:
        while i < len(expr):
            c = expr[i]
            i += 1
            if c < 128:
                stack.append(c - 64)
            elif c in (0x80, 0x81, 0x82):
                fmt, size = {0x80: ('<f', 4), 0x81: ('<d', 8), 0x82: ('<i', 4)}[c]
                stack.append(struct.unpack_from(fmt, expr, i)[0])
                i += size
            elif c in (0x8c, 0x8e, 0x8f):
                right, left = stack.pop(), stack.pop()
                stack.append(left + right if c == 0x8c else left - right if c == 0x8e else left * right)
            elif c == 0xa1:
                break
            else:
                return None
        return stack[-1] if len(stack) == 1 else None
    except (IndexError, struct.error):
        return None


def expression(b):
    """Decode EZL to data, never execute a game's expression or command."""
    stack, registers, i = [], {}, 0
    ops = {0x8c: '+', 0x8e: '-', 0x8f: '*', 0x90: '/', 0x91: '<=', 0x92: '>=',
           0x93: '<', 0x94: '>', 0x95: '==', 0x96: '!=', 0x98: 'and', 0x99: 'or'}
    while i < len(b):
        c = b[i]
        i += 1
        if c < 128:
            stack.append(c - 64)
        elif c in (0x80, 0x81, 0x82):
            fmt, n = {0x80: ('<f', 4), 0x81: ('<d', 8), 0x82: ('<i', 4)}[c]
            stack.append(struct.unpack_from(fmt, b, i)[0])
            i += n
        elif 0x84 <= c <= 0x8a:
            n = c - 0x84
            args = stack[-n:] if n else []
            if n:
                del stack[-n:]
            function = stack.pop()
            stack.append(['flag', *args] if function == 15 else ['function', function, *args])
        elif c in ops:
            right, left = stack.pop(), stack.pop()
            stack.append([ops[c], left, right])
        elif c == 0x8d:
            stack.append(['neg', stack.pop()])
        elif c == 0xa5:
            value = text(b, i)
            i += len(value.encode('utf-16-le')) + 2
            stack.append(value)
        elif 0xa7 <= c <= 0xae:
            registers[c - 0xa7] = stack[-1]
        elif 0xaf <= c <= 0xb6:
            stack.append(registers.get(c - 0xaf, ['register', c - 0xaf]))
        elif c == 0xb8:
            stack.append(['arg', stack.pop()])
        elif c == 0xb9:
            stack.append(['call_status'])
        elif c == 0xba:
            stack.append(['ongoing'])
        elif c not in (0xa1, 0xa6, 0xb7):
            raise ValueError(f'Unsupported EZL opcode {c:02x}')
    if len(stack) != 1:
        raise ValueError('Unbalanced EZL expression')
    return stack[0]


def walk_commands(talk):
    def conditions(cs):
        for c in cs:
            yield from c['commands']
            yield from conditions(c['children'])
    for states in talk.values():
        for state in states.values():
            yield from state['enter']
            yield from state['exit']
            yield from state['ongoing']
            yield from conditions(state['conditions'])


def jsonwrite(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n', encoding='utf-8')


def extract(game, maps):
    game, maps = Path(game).resolve(), Path(maps).resolve()
    if not (game / 'dvdroot_ps4').is_dir():
        game = game / 'Image0'
    if sha(game / 'eboot.bin') != SUPPORTED_EBOOT:
        raise ValueError('Unsupported executable for Atlas features')
    if game == maps or game in maps.parents or maps in game.parents:
        raise ValueError('Feature output must be outside game files')
    root = game / 'dvdroot_ps4'
    param_bnd = binder(root / 'param/gameparam/gameparam.parambnd.dcx')
    return_points = params(param_bnd['ReturnPointParam.param'][1])
    lots = params(param_bnd['ItemLotParam.param'][1])
    texts = binder(root / 'msg/engus/item.msgbnd.dcx')
    tables = {eid: fmg(data) for _, (eid, data) in texts.items() if data[:1] == b'\0'}
    menu_texts = binder(root / 'msg/engus/menu.msgbnd.dcx')
    event_text = next(fmg(data) for _, (eid, data) in menu_texts.items() if eid == 30)
    item_names = {kind: tables.get(eid, {}) for kind, eid in [(0, 11), (1, 12), (2, 13), (4, 10)]}
    menu_lamps = {}
    dream_talk = binder(root / 'script/talk/m21_00_00_00.talkesdbnd.dcx')
    for name, (_, data) in dream_talk.items():
        for bank, idx, args in walk_commands(esd(data)):
            if (bank, idx) != (1, 19) or len(args) != 3:
                continue
            entry, label, required = map(constant, args)
            if isinstance(label, int) and 70000000 <= label < 70100000 and isinstance(required, int) and 12000000 <= required < 14000000:
                menu_lamps[required] = dict(text_id=label, name=event_text.get(label, 'Lamp'), script=name)
    result = dict(schema=SCHEMA, game_eboot_sha256=SUPPORTED_EBOOT, areas={}, coverage={})
    atlas = json.loads((maps / 'atlas.json').read_text(encoding='utf-8'))
    catalog = json.loads((maps / 'catalog.json').read_text(encoding='utf-8'))
    for entry in atlas['maps']:
        mid = entry['id']
        parts, treasures = msb(root / 'map/mapstudio' / (mid + '.msb.dcx'))
        event_map = events(root / 'event' / (mid + '.emevd.dcx'))
        calls = event_calls(event_map)
        dynamic_objects = {struct.unpack_from('<i', row['data'])[0]
                           for rows in event_map.values() for row in rows
                           if (row['bank'], row['index']) in ((2005, 3), (2005, 4))
                           and len(row['data']) >= 4 and not row['replacements']
                           and struct.unpack_from('<i', row['data'])[0] > 0}
        byentity = {p['entity']: p for p in parts if p['entity'] > 0}
        geometry = json.loads((maps / (mid + '.json')).read_text(encoding='utf-8'))
        lamps, pickups = [], []
        for target, slot, args in calls:
            if target != 7000 or len(args) != 16:
                continue
            character, obj, appearance, base = struct.unpack('<4i', args)
            required = base + 10
            authored = byentity.get(obj)
            rid = obj + 1000
            if not authored or rid not in return_points or required not in menu_lamps:
                continue
            rp = return_points[rid]
            area, block, entity = struct.unpack_from('<hhi', rp)
            if area != int(mid[1:3]) or block != int(mid[4:6]) or entity != rid:
                raise ValueError('Lamp return-point mismatch')
            drawn = min(geometry.get('lamps', []), key=lambda l: math.dist(l.get('source_pos', l['pos']), authored['pos']), default=None)
            position = drawn['pos'] if drawn and math.dist(drawn.get('source_pos', drawn['pos']), authored['pos']) < 5 else authored['pos']
            lamps.append(dict(id=f'{mid}:{obj}', name=menu_lamps[required]['name'], pos=position, return_point=rid,
                              arrival_pos=byentity.get(rid, authored)['pos'],
                              unlock_flag=required, available_on=[] if appearance == 999 else [appearance], available_off=[],
                              source=dict(event=7000, slot=slot, menu_script=menu_lamps[required]['script'], text_id=menu_lamps[required]['text_id'])))
        # The Hypogean Gaol destination is removed from the native travel list
        # after the Blood Moon, even if its lit flag was set earlier.
        for lamp in lamps:
            if lamp['name'] == 'Hypogean Gaol':
                lamp['available_off'].append(9802)
        if mid == 'm21_00_00_00':
            dream_pos=byentity.get(2102950, {'pos':[0.0,0.0,0.0]})['pos']
            lamps.append(dict(id='hunters-dream', name="Hunter's Dream", pos=dream_pos, arrival_pos=dream_pos,
                              return_point=2102950, unlock_flag=0, available_on=[9401], available_off=[]))
        for treasure in treasures:
            p = treasure['part']
            if p['kind'] not in (1, 9) or treasure['hidden'] or p['entity'] in dynamic_objects:
                continue
            contents = []
            lot_ids = []
            for first in treasure['lots']:
                for offset in range(64):
                    if first + offset not in lots:
                        break
                    lot_ids.append(first + offset)
            for lot in dict.fromkeys(lot_ids):
                row = lots.get(lot)
                if row is None or len(row) < 146:
                    continue
                collected = struct.unpack_from('<i', row, 128)[0]
                if collected <= 0:
                    continue
                for slot in range(8):
                    iid = struct.unpack_from('<i', row, slot * 4)[0]
                    kind = struct.unpack_from('<i', row, 32 + slot * 4)[0]
                    chance = struct.unpack_from('<H', row, 64 + slot * 2)[0]
                    qty = row[138 + slot]
                    if iid <= 0 or not chance or not qty:
                        continue
                    name = item_names.get(kind, {}).get(iid, '')
                    category = ('equipment' if kind in (0, 1) else 'healing' if name == 'Blood Vial' else
                                'insight' if name in ("Madman's Knowledge", "Great One's Wisdom") else
                                'echoes' if kind == 4 and name.startswith(('Coldblood Dew (', 'Thick Coldblood (',
                                    'Frenzied Coldblood (', 'Kin Coldblood (', 'Great One Coldblood',
                                    "Great One's Coldblood", 'Revered Great One Coldblood')) else None)
                    if category:
                        contents.append(dict(name=name or f'Equipment {iid}', item_id=iid, category=category,
                                             quantity=qty, collected_flag=collected, lot_id=lot))
            if contents:
                pickups.append(dict(id=f'{mid}:{p["name"]}', pos=p['pos'], contents=contents, chest=treasure['chest'],
                                    source_part=p['name'], available_on=[], available_off=[]))
        reveal = []
        if mid == 'm24_00_00_00':
            for region in catalog[mid].get('regions', []):
                if region['name'] == 'Central Yharnam':
                    reveal.append(dict(name=region['name'], min_y=region['min_y'], max_y=region['max_y'],
                                       triangles=region['triangles'], boss_flags=[12411800]))
        if mid == 'm32_00_00_00':
            # The bossless Lecture Building shares a map with Rom's districts.
            # Its authored floors are spatially separate; reveal only after one
            # of its own lamps is unlocked, independently of Rom's defeat.
            regions = [r for r in catalog[mid].get('regions', []) if r['name'] == 'Lecture Building']
            triangles = [t for r in regions for t in r['triangles']]
            if triangles:
                xs = [t[i] for t in triangles for i in (0, 2, 4)]
                zs = [t[i] for t in triangles for i in (1, 3, 5)]
                reveal.append(dict(name='Lecture Building', bounds=[min(xs)-3, min(zs)-3, max(xs)+3, max(zs)+3],
                                   min_y=min(r['min_y'] for r in regions)-5,
                                   max_y=max(r['max_y'] for r in regions)+5, boss_flags=[],
                                   reveal_when=combine('or', isflag(13207830), isflag(13207870))))
        reveal.append(dict(name=geometry['name'], boss_flags=AREA_BOSSES.get(mid, [])))
        result['areas'][mid] = dict(lamps=lamps, pickups=pickups, npc_rules=npc_rules(mid, root),
                                    reveal_regions=reveal, always_visible=mid in ('m21_00_00_00', 'm21_01_00_00'))
    result['coverage'] = dict(lamps=sum(len(a['lamps']) for a in result['areas'].values()),
                              pickups=sum(len(a['pickups']) for a in result['areas'].values()),
                              npc_supported=['Gilbert before transformation', 'Eileen initial Central Yharnam conversations',
                                             'Eileen outside Oedon Chapel before Henryk'],
                              npc_limitation='Henryk aftermath, wounded Eileen and other NPCs are not yet certified and are hidden in new-dialogue mode.',
                              boss_reveal='Upper Cathedral Ward and Research Hall require both local bosses; overlapping Central Yharnam uses its own boss flag; the bossless Lecture Building uses its own unlocked lamps.')
    jsonwrite(maps / 'features.json', result)
    receipt_path = maps / 'generated-assets.json'
    if receipt_path.exists():
        receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
        receipt['feature_schema'] = SCHEMA
        receipt['files']['maps/features.json'] = sha(maps / 'features.json')
        jsonwrite(receipt_path, receipt)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game-dir', type=Path, required=True)
    p.add_argument('--maps-dir', type=Path, required=True)
    args = p.parse_args()
    result = extract(args.game_dir, args.maps_dir)
    print(json.dumps(result['coverage']))


if __name__ == '__main__':
    main()
