"""PARAM merge: rows keyed (id, nth occurrence), each field merged on its own against the game's table.

A field is the merge unit, a bitfield counts as a field of its own: two mods that change different bits of one byte both
keep their change, two mods that change one field to different values are a conflict (the later mod's value). Field
names and widths come from the game's paramdefs (paramdef\\paramdef.paramdefbnd.dcx), read at merge time; a table whose
paramdef is missing or does not add up to its row size is merged per 4-byte word instead, and the log names the bytes.
"""
import bisect
import re
import struct

from common import Report, containers, show_float, show_text

_SIZE = {"s8": 1, "u8": 1, "s16": 2, "u16": 2, "s32": 4, "u32": 4, "f32": 4}
_FORMAT = {"s8": "<b", "u8": "<B", "s16": "<h", "u16": "<H", "s32": "<i", "u32": "<I", "f32": "<f"}
_BY_SIZE = {1: "u8", 2: "u16", 4: "u32"}


class Unit:
    """One mergeable place of a row: `bit` and `width` in bits from the row start (little endian), `kind` how to show a
    value: a numeric type, "bits", "raw" (padding, arrays), "fixstr" or "fixstrW"."""
    __slots__ = ("name", "bit", "width", "kind", "mask")

    def __init__(self, name, bit, width, kind):
        self.name, self.bit, self.width, self.kind = name, bit, width, kind
        self.mask = ((1 << width) - 1) << bit

    def value(self, row_int):
        return (row_int >> self.bit) & ((1 << self.width) - 1)

    def show(self, raw):
        if self.kind in _FORMAT:
            value = struct.unpack(_FORMAT[self.kind], raw.to_bytes(self.width // 8, "little"))[0]
            return show_float(value) if self.kind == "f32" else str(value)
        if self.kind == "bits":
            return str(raw)
        data = raw.to_bytes(self.width // 8, "little")
        if self.kind == "fixstr":
            return show_text(data.split(b"\0")[0].decode("shift_jis", "replace"))
        if self.kind == "fixstrW":
            text = data.decode("utf-16-le", "replace")
            return show_text(text.split("\0")[0])
        return data.hex()


class Layout:
    """The units of one paramdef, in row order. `exact`: the paramdef adds up to the row size (else the word fallback)."""

    def __init__(self, units, row_size, exact, type_name):
        self.units, self.row_size, self.exact, self.type_name = units, row_size, exact, type_name
        self.by_byte = [[] for _ in range(row_size)]
        for index, unit in enumerate(units):
            for byte in range(unit.bit // 8, (unit.bit + unit.width - 1) // 8 + 1):
                if byte < row_size:
                    self.by_byte[byte].append(index)

    def touched(self, diff_bytes):
        """Indices of the units that overlap a non-zero byte of `diff_bytes`, in row order."""
        seen = []
        last = -1
        for at, byte in enumerate(diff_bytes):
            if byte:
                for index in self.by_byte[at]:
                    if index > last:
                        seen.append(index)
                        last = index
        return seen


def paramdef_type(data):
    return data[0x0c:0x2c].split(b"\0")[0].decode("latin-1")


def param_type(data):
    return data[0x0c:0x2c].split(b"\0")[0].decode("latin-1")


def paramdef_units(data):
    """(type name, [Unit], size in bytes) of one paramdef (field records of 0xd0 bytes from 0x38: byte size at +0x64,
    type at +0x70, name at +0x90). Bitfields pack like containers.Layout: a run of bitfields of one container size
    shares a container until it is full."""
    count, = struct.unpack_from("<H", data, 8)
    units, offset, bit, bit_type = [], 0, 0, None
    names = {}
    for i in range(count):
        at = 0x38 + i * 0xd0
        kind = data[at + 0x70:at + 0x90].split(b"\0")[0].decode("latin-1")
        name = data[at + 0x90:at + 0xb0].split(b"\0")[0].decode("latin-1").strip()
        size, = struct.unpack_from("<I", data, at + 0x64)
        known = kind if kind in _SIZE else (None if kind.startswith(("dummy", "fixstr")) else _BY_SIZE.get(size))
        bits = re.match(r"(\w+)\s*:\s*(\d+)$", name)
        if bits:
            base = known or "u8"
            width = int(bits.group(2))
            if bit_type is None or _SIZE[bit_type] != _SIZE[base] or bit + width > _SIZE[base] * 8:
                if bit_type is not None:
                    offset += _SIZE[bit_type]
                bit_type, bit = base, 0
            units.append([bits.group(1), offset * 8 + bit, width, "bits"])
            bit += width
            continue
        if bit_type is not None:
            offset += _SIZE[bit_type]
            bit_type, bit = None, 0
        if size:
            if known and _SIZE[known] == size:
                shown = known
            elif kind.startswith("fixstrW"):
                shown = "fixstrW"
            elif kind.startswith("fixstr"):
                shown = "fixstr"
            else:
                shown = "raw"
            units.append([name, offset * 8, size * 8, shown])
        offset += size
    size = offset + (_SIZE[bit_type] if bit_type else 0)
    for unit in units:
        names[unit[0]] = names.get(unit[0], 0) + 1
    out = []
    for name, bit, width, kind in units:
        shown = name if names[name] == 1 else "%s@0x%X" % (name, bit // 8) + ("" if kind != "bits" else ".%d" % (bit % 8))
        out.append(Unit(shown, bit, width, kind))
    return paramdef_type(data), out, size


def word_units(row_size):
    """The fallback without a fitting paramdef: 4-byte words (the tail byte by byte), named by their offset."""
    units, at = [], 0
    while at < row_size:
        width = 4 if at + 4 <= row_size else 1
        units.append(Unit("byte +0x%X" % at, at * 8, width * 8, "u32" if width == 4 else "u8"))
        at += width
    return units


class Paramdefs:
    """The game's paramdefs by PARAM type string (the string at 0x0c of both files)."""

    def __init__(self, paramdefbnd_payload=None):
        self.defs = {}
        if paramdefbnd_payload:
            for _, _, _, body in containers.bnd4_read(paramdefbnd_payload)["entries"]:
                try:
                    type_name, units, size = paramdef_units(body)
                except (struct.error, ValueError, IndexError):
                    continue
                self.defs.setdefault(type_name, (units, size))

    def layout(self, type_name, row_size):
        found = self.defs.get(type_name)
        if found and found[1] == row_size:
            return Layout(found[0], row_size, True, type_name)
        return Layout(word_units(row_size), row_size, False, type_name)


def read_param(data):
    """containers.Param, or MergeError text when it cannot be read as one fixed-size, contiguous table."""
    if len(data) < 0x40:
        raise ValueError("too short for a PARAM")
    count, = struct.unpack_from("<H", data, 0x0a)
    if count == 0:
        raise ValueError("no rows")
    return containers.Param(data)


def row_name(param, ref):
    """The name text of a row (its name offset relative to the names block); "" for none."""
    if not isinstance(ref, int):
        return ref or ""
    names = param.names
    if ref < 0 or ref >= len(names):
        return ""
    end = ref
    while end + 1 < len(names) and names[end:end + 2] != b"\0\0":
        end += 2
    return names[ref:end].decode("utf-16-le", "replace")


def keyed_rows(rows, vanilla_bodies=None):
    """[(key, id, body, name ref)] with key (id, nth). With `vanilla_bodies` ({id: [bodies in order]}): a mod row of an
    id whose count differs from the game's is first paired with an equal game row of that id, so one deleted or added
    duplicate does not shift every later occurrence."""
    by_id = {}
    for index, (row_id, body, name) in enumerate(rows):
        by_id.setdefault(row_id, []).append(index)
    nth_of = [0] * len(rows)
    for row_id, indices in by_id.items():
        base = (vanilla_bodies or {}).get(row_id)
        if not base or len(base) == len(indices):
            for n, index in enumerate(indices):
                nth_of[index] = n
            continue
        free = list(range(len(base)))
        taken = {}
        for index in indices:
            for n in free:
                if base[n] == rows[index][1]:
                    taken[index] = n
                    free.remove(n)
                    break
        extra = len(base)
        for index in indices:
            if index in taken:
                continue
            if free:
                taken[index] = free.pop(0)
            else:
                taken[index] = extra
                extra += 1
        for index in indices:
            nth_of[index] = taken[index]
    return [((row_id, nth_of[i]), row_id, body, name) for i, (row_id, body, name) in enumerate(rows)]


def place_of(table, key, item_name):
    row_id, nth = key
    text = "%s row %d" % (table, row_id) + (" #%d" % (nth + 1) if nth else "")
    if item_name:
        text += " (%s)" % item_name
    return text


def merge_param(label_table, vanilla, mods, report, paramdefs, item_name=None):
    """`vanilla`: the game's PARAM bytes; `mods`: [(mod name, PARAM bytes)] in merge order, each different from the
    game's. Returns (PARAM bytes, rows taken from mods). Conflicts go to `report`. Raises ValueError when the game's
    table cannot be read (the caller falls back to the later mod's whole file)."""
    base = read_param(vanilla)
    parsed = []
    for name, data in mods:
        try:
            param = read_param(data)
        except (ValueError, struct.error, IndexError) as error:
            report.not_merged(label_table, [m for m, _ in mods], mods[-1][0],
                              "%s's table cannot be read: %s" % (name, error))
            return mods[-1][1], 0
        if param.size != base.size:
            report.not_merged(label_table, [m for m, _ in mods], mods[-1][0],
                              "row size 0x%X in %s, the game's is 0x%X" % (param.size, name, base.size))
            return mods[-1][1], 0
        parsed.append((name, param))
    layout = paramdefs.layout(param_type(vanilla), base.size)
    lookup = item_name or (lambda table, row_id: None)

    base_rows = keyed_rows(base.rows)
    base_bodies = {}
    for key, row_id, body, ref in base_rows:
        base_bodies.setdefault(row_id, []).append(body)
    base_index = {key: i for i, (key, _, _, _) in enumerate(base_rows)}

    order = {name: i for i, (name, _) in enumerate(parsed)}
    # Per game row: changes[key][unit index or "name"] = [(mod, value)] of the mods that changed it, in merge order;
    # deleted[key] = the mods that ship the table without that row.
    changes, deleted = {}, {}
    new_rows, new_order = {}, []  # key -> [(mod, body, name text)]
    for mod, param in parsed:
        seen = set()
        for key, row_id, body, ref in keyed_rows(param.rows, base_bodies):
            seen.add(key)
            at = base_index.get(key)
            if at is None:
                if key not in new_rows:
                    new_rows[key] = []
                    new_order.append(key)
                new_rows[key].append((mod, body, row_name(param, ref)))
                continue
            _, _, vbody, vref = base_rows[at]
            if body != vbody:
                diff = bytes(a ^ b for a, b in zip(body, vbody))
                row_int, vint = int.from_bytes(body, "little"), int.from_bytes(vbody, "little")
                for index in layout.touched(diff):
                    unit = layout.units[index]
                    if unit.value(row_int) != unit.value(vint):
                        changes.setdefault(key, {}).setdefault(index, []).append((mod, unit.value(row_int)))
            text = row_name(param, ref)
            if text != row_name(base, vref):
                changes.setdefault(key, {}).setdefault("name", []).append((mod, text))
        for key, _, _, _ in base_rows:
            if key not in seen:
                deleted.setdefault(key, []).append(mod)

    out_rows = []
    taken = 0
    # Rows whose name two mods set differently (often only a translation): the game does not use row names, so they
    # get one summary line per table instead of a CONFLICT line each.
    name_clashes = []
    for key, row_id, vbody, vref in base_rows:
        units = changes.get(key, {})
        deleters = deleted.get(key, [])
        if not units and not deleters:
            out_rows.append((row_id, vbody, vref))
            continue
        taken += 1
        place = place_of(label_table, key, lookup(label_table, row_id) or row_name(base, vref) or None)
        if deleters:
            changers = sorted({m for values in units.values() for m, _ in values}, key=order.get)
            last = max(deleters + changers, key=order.get)
            if changers:
                values = sorted([(m, "changes it") for m in changers] + [(m, "deletes it") for m in deleters],
                                key=lambda mv: order[mv[0]])
                report.conflict("%s (row deleted by one mod, changed by another)" % place, values, last)
            if last in deleters:
                continue
        row_int = int.from_bytes(vbody, "little")
        ref = vref
        for index in sorted(units, key=lambda i: (i == "name", i if i != "name" else 0)):
            values = units[index]
            final = values[-1][1]
            if index == "name":
                if len({v for _, v in values}) > 1:
                    name_clashes.append(key)
                ref = final
                continue
            unit = layout.units[index]
            if len({v for _, v in values}) > 1:
                report.conflict("%s %s" % (place, unit.name), [(m, unit.show(v)) for m, v in values], values[-1][0])
            row_int = (row_int & ~unit.mask) | (final << unit.bit)
        out_rows.append((row_id, row_int.to_bytes(len(vbody), "little"), ref))

    placed = []
    for key in sorted(new_order):
        versions = new_rows[key]
        mod, body, text = versions[-1]
        distinct = []
        for version in versions:
            if not any(version[1:] == d[1:] for d in distinct):
                distinct.append(version)
        if any(t != distinct[0][2] for _, _, t in distinct):
            name_clashes.append(key)
        if len({d[1] for d in distinct}) > 1:
            place = place_of(label_table, key, lookup(label_table, key[0]) or text or None)
            fields = []
            first = int.from_bytes(distinct[0][1], "little")
            for _, other_body, _ in distinct[1:]:
                other = int.from_bytes(other_body, "little")
                fields += [u for u in layout.units if u.value(first) != u.value(other) and u not in fields]
            what = ", ".join(u.name for u in fields[:6])
            if len(fields) > 6:
                what += " and %d more" % (len(fields) - 6)
            if len(fields) <= 3:  # few fields: each mod's values of them
                shown = []
                for m, body, _ in versions:
                    value = int.from_bytes(body, "little")
                    shown.append((m, " ".join("%s=%s" % (u.name, u.show(u.value(value))) for u in fields)))
            else:
                shown = [(m, "adds it") for m, _, _ in versions]
            report.conflict("%s (a new row; the versions differ in %s)" % (place, what), shown, mod)
        taken += 1
        placed.append((key, body, text))
    insert_all_new(out_rows, placed)
    if name_clashes:
        shown = ", ".join("%d" % k[0] + (" #%d" % (k[1] + 1) if k[1] else "") for k in name_clashes[:8])
        report.info("%s: %d row name%s set differently by two or more mods (the game does not use row names; the later "
                    "mod's name is kept): row%s %s%s" % (
                        label_table, len(name_clashes), "" if len(name_clashes) == 1 else "s",
                        "" if len(name_clashes) == 1 else "s", shown,
                        " and %d more" % (len(name_clashes) - 8) if len(name_clashes) > 8 else ""))
    base.rows = out_rows
    return base.write(), taken


def insert_new(rows, key, body, text):
    """Inserts a new row after the last row of the highest id at or below its own (the game's order stays; an id below
    every row goes first). `rows` = [(id, body, name ref)]. One row at a time; `insert_all_new` places many at once."""
    row_id = key[0]
    best, at = None, 0
    for index, (other, _, _) in enumerate(rows):
        if other <= row_id and (best is None or other >= best):
            best, at = other, index + 1
    rows.insert(at, (row_id, body, text))


def insert_all_new(rows, news):
    """Same result as `insert_new` for each of `news` ([(key, body, text)] in ascending key order), in O(n log n):
    every new row lands after the last existing row of the highest existing id at or below its own, and new rows that
    share such an anchor follow each other in key order (a later new row's best match is the earlier new row there)."""
    last_at = {}
    for index, (row_id, _, _) in enumerate(rows):
        last_at[row_id] = index
    ids = sorted(last_at)
    buckets = {}
    for key, body, text in news:
        found = bisect.bisect_right(ids, key[0]) - 1
        anchor = last_at[ids[found]] + 1 if found >= 0 else 0
        buckets.setdefault(anchor, []).append((key[0], body, text))
    out = list(buckets.get(0, ()))
    for index, row in enumerate(rows):
        out.append(row)
        out.extend(buckets.get(index + 1, ()))
    rows[:] = out
