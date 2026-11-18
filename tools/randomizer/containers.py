"""Game containers: DCX (DFLT), BND4 (format 0x74, no hash table), PARAM (64-bit) and the paramdef
layout. Readers keep what the writers need, so an unchanged file writes back to the same bytes."""
import re
import struct
import zlib


def dcx_read(raw):
    """(payload, header template)"""
    if raw[:4] != b"DCX\0" or raw[0x28:0x2c] != b"DFLT":
        raise ValueError("not a DFLT DCX file")
    size, packed = struct.unpack_from(">II", raw, 0x1c)
    data = zlib.decompress(raw[0x4c:0x4c + packed])
    if len(data) != size:
        raise ValueError("DCX size mismatch")
    return data, raw[:0x4c]


def dcx_write(payload, header):
    packed = zlib.compress(payload, 9)
    head = bytearray(header)
    struct.pack_into(">II", head, 0x1c, len(payload), len(packed))
    return bytes(head) + packed


def dcx_size(first_bytes):
    """A DCX file's uncompressed size, from its first 0x24 bytes."""
    return struct.unpack_from(">I", first_bytes, 0x1c)[0]


def _utf16z(data, offset):
    end = offset
    while data[end:end + 2] != b"\0\0":
        end += 2
    return data[offset:end].decode("utf-16-le")


def bnd4_read(data):
    """{'header': first 0x40 bytes, 'entries': [[flags, id, name, bytes]]}"""
    if data[:4] != b"BND4" or data[0x30:0x33] != b"\x01\x74\x00":
        raise ValueError("unsupported BND4")
    count, = struct.unpack_from("<I", data, 0x0c)
    entries = []
    for i in range(count):
        at = 0x40 + i * 0x24
        size, = struct.unpack_from("<q", data, at + 8)
        offset, file_id, name_at = struct.unpack_from("<IiI", data, at + 0x18)
        entries.append([data[at:at + 4], file_id, _utf16z(data, name_at), data[offset:offset + size]])
    return {"header": data[:0x40], "entries": entries}


def bnd4_write(bnd):
    entries = bnd["entries"]
    names, name_at = bytearray(), []
    base = 0x40 + len(entries) * 0x24
    for entry in entries:
        name_at.append(base + len(names))
        names += entry[2].encode("utf-16-le") + b"\0\0"
    data_start = base + len(names)
    head = bytearray(bnd["header"])
    struct.pack_into("<I", head, 0x0c, len(entries))
    struct.pack_into("<Q", head, 0x28, data_start)
    table, blob, pos = bytearray(), bytearray(), (data_start + 15) & ~15
    for (flags, file_id, _, body), name in zip(entries, name_at):
        table += flags + struct.pack("<iqqIiI", -1, len(body), len(body), pos, file_id, name)
        blob += b"\0" * (pos - data_start - len(blob)) + body
        pos = (data_start + len(blob) + 15) & ~15
    return bytes(head) + bytes(table) + bytes(names) + bytes(blob)


def bnd4_find(bnd, suffix):
    """The entry whose name ends in `suffix`, folder separators and case ignored."""
    suffix = suffix.lower()
    for entry in bnd["entries"]:
        if entry[2].replace("/", "\\").lower().endswith(suffix):
            return entry
    raise KeyError(suffix)


class Param:
    """A PARAM file as a row list. Vanilla has duplicate ids and is not fully sorted, so order is kept."""

    def __init__(self, data):
        count, = struct.unpack_from("<H", data, 0x0a)
        names_start, = struct.unpack_from("<I", data, 0)
        data_start, = struct.unpack_from("<Q", data, 0x30)
        self.header = data[:0x40]
        self.size = (names_start - data_start) // count
        self.names = data[names_start:]
        self.rows = []
        for i in range(count):
            row_id, _, at, name_at = struct.unpack_from("<iiqq", data, 0x40 + i * 0x18)
            if at != data_start + i * self.size:
                raise ValueError("param rows are not contiguous")
            self.rows.append((row_id, data[at:at + self.size], name_at - names_start))

    def first(self):
        """{id: bytes}, the first row of each id (the one a lookup finds)."""
        out = {}
        for row_id, body, _ in self.rows:
            out.setdefault(row_id, body)
        return out

    def append_sorted(self, new_rows, names=None):
        """Adds rows whose ids all lie above every existing id, in id order. `names` = {id: text}, else empty names."""
        top = max(r[0] for r in self.rows)
        ids = [r[0] for r in new_rows]
        if ids != sorted(set(ids)) or (ids and ids[0] <= top):
            raise ValueError("new rows must be sorted, unique and above the existing ids")
        for row_id, body in new_rows:
            if len(body) != self.size:
                raise ValueError("wrong row size")
            self.rows.append((row_id, body, (names or {}).get(row_id)))

    def insert_sorted(self, new_rows, names=None):
        """Adds rows with new ids, each before the first row with a higher id (the game looks rows up by sorted id)."""
        taken = {r[0] for r in self.rows}
        for row_id, body in sorted(new_rows):
            if row_id in taken or len(body) != self.size:
                raise ValueError("row %d: id taken or wrong size" % row_id)
            at = next((i for i, r in enumerate(self.rows) if r[0] > row_id), len(self.rows))
            self.rows.insert(at, (row_id, body, (names or {}).get(row_id)))
            taken.add(row_id)

    def write(self):
        count = len(self.rows)
        if count > 0xffff:
            raise ValueError("too many param rows")
        data_start = 0x40 + count * 0x18
        names_start = data_start + count * self.size
        head = bytearray(self.header)
        struct.pack_into("<I", head, 0, names_start)
        struct.pack_into("<H", head, 0x0a, count)
        struct.pack_into("<Q", head, 0x30, data_start)
        names, table, data = bytearray(self.names), bytearray(), bytearray()
        for i, (row_id, body, name_ref) in enumerate(self.rows):
            if not isinstance(name_ref, int):  # a new row: its own name, empty when it has none
                if len(names) % 2:
                    names += b"\0"
                text = name_ref
                name_ref = len(names)
                names += (text or "").encode("utf-16-le") + b"\0\0"
            table += struct.pack("<iiqq", row_id, 0, data_start + i * self.size, names_start + name_ref)
            data += body
        return bytes(head) + bytes(table) + bytes(data) + bytes(names)


_FORMAT = {"s8": "b", "u8": "B", "s16": "h", "u16": "H", "s32": "i", "u32": "I", "f32": "f"}
_SIZE = {"s8": 1, "u8": 1, "s16": 2, "u16": 2, "s32": 4, "u32": 4, "f32": 4}
_BY_SIZE = {1: "u8", 2: "u16", 4: "u32"}


class Layout:
    """Field offsets of one paramdef. Plain fields by name; bitfields (`isNegative:1`) by their name without the width,
    as (offset of their container, its format, first bit, width), lowest bit first."""

    def __init__(self, data):
        count, = struct.unpack_from("<H", data, 8)
        self.fields, self.bits, offset, bit, bit_type = {}, {}, 0, 0, None
        for i in range(count):
            at = 0x38 + i * 0xd0
            kind = data[at + 0x70:at + 0x90].split(b"\0")[0].decode("latin-1")
            name = data[at + 0x90:at + 0xb0].split(b"\0")[0].decode("latin-1").strip()
            size, = struct.unpack_from("<I", data, at + 0x64)
            known = kind if kind in _SIZE else (None if kind.startswith(("dummy", "fixstr")) else _BY_SIZE.get(size))
            bits = re.match(r"\w+\s*:\s*(\d+)$", name)
            if bits:
                base = known or "u8"
                width = int(bits.group(1))
                if bit_type is None or _SIZE[bit_type] != _SIZE[base] or bit + width > _SIZE[base] * 8:
                    if bit_type is not None:
                        offset += _SIZE[bit_type]
                    bit_type, bit = base, 0
                self.bits[name.split(":")[0].strip()] = (offset, "<" + _FORMAT[base], bit, width)
                bit += width
                continue
            if bit_type is not None:
                offset += _SIZE[bit_type]
                bit_type, bit = None, 0
            if known:
                self.fields[name] = (offset, "<" + _FORMAT[known])
            offset += size
        self.size = offset + (_SIZE[bit_type] if bit_type else 0)

    def get(self, row, name):
        if name in self.fields:
            at, fmt = self.fields[name]
            return struct.unpack_from(fmt, row, at)[0]
        at, fmt, bit, width = self.bits[name]
        return struct.unpack_from(fmt, row, at)[0] >> bit & ((1 << width) - 1)

    def set(self, row, name, value):
        if name in self.fields:
            at, fmt = self.fields[name]
            struct.pack_into(fmt, row, at, value)
            return
        at, fmt, bit, width = self.bits[name]
        mask = ((1 << width) - 1) << bit
        word = struct.unpack_from(fmt, row, at)[0]
        struct.pack_into(fmt, row, at, (word & ~mask) | (value << bit & mask))


def fmg_read(data):
    """FMG v2 (64-bit): {'header': first 0x28 bytes, 'texts': {id: str or None}, 'ranges': [(first, last)], 'size'}."""
    if data[2] != 2:
        raise ValueError("unsupported FMG")
    groups, = struct.unpack_from("<i", data, 0x0c)
    strings, = struct.unpack_from("<q", data, 0x18)
    texts, ranges = {}, []
    for g in range(groups):
        index, first, last = struct.unpack_from("<iii", data, 0x28 + g * 0x10)
        ranges.append((first, last))
        for k in range(last - first + 1):
            at, = struct.unpack_from("<q", data, strings + (index + k) * 8)
            if at <= 0:
                texts[first + k] = None
                continue
            end = at
            while data[end:end + 2] != b"\0\0":
                end += 2
            texts[first + k] = data[at:end].decode("utf-16-le")
    return {"header": data[:0x28], "texts": texts, "ranges": ranges, "size": len(data)}


def fmg_write(fmg):
    """The original groups stay; an id outside them gets a group of its own. Vanilla files end in zero padding with no
    single alignment rule, so the original size is kept while the content fits, else the size is aligned to 16."""
    ranges = list(fmg["ranges"])
    covered = {i for a, b in ranges for i in range(a, b + 1)}
    ranges = sorted(ranges + [(i, i) for i in fmg["texts"] if i not in covered])
    ids, groups = [], []
    for a, b in ranges:
        groups.append((len(ids), a, b))
        ids += range(a, b + 1)
    offsets_at = 0x28 + len(groups) * 0x10
    text_at = offsets_at + len(ids) * 8
    strings, offsets = bytearray(), []
    for i in ids:
        text = fmg["texts"].get(i)
        offsets.append(0 if text is None else text_at + len(strings))
        if text is not None:
            strings += text.encode("utf-16-le") + b"\0\0"
    total = text_at + len(strings)
    total = fmg["size"] if fmg["size"] >= total else (total + 15) & ~15
    head = bytearray(fmg["header"])
    struct.pack_into("<i", head, 0x04, total)
    struct.pack_into("<ii", head, 0x0c, len(groups), len(ids))
    struct.pack_into("<q", head, 0x18, offsets_at)
    out = head + b"".join(struct.pack("<iiii", index, a, b, 0) for index, a, b in groups)
    out += b"".join(struct.pack("<q", o) for o in offsets)
    return bytes(out) + bytes(strings) + b"\0" * (total - text_at - len(strings))
