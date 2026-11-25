"""TPF (PS4) read / write and merge per texture name.

Layout (checked on the game's TPFs, round trip byte-identical): 0x10-byte header (TPF\\0, u32 size of the texture
data, u32 count, platform byte 4, flag byte, encoding byte at 0xe: 1 = UTF-16 names, 2 = ASCII), then 0x24-byte
entries (u32 data offset, i32 size, 16 bytes of format, u32 name offset at +0x18, 8 more bytes), then the names in
entry order, then the data from the next 16-byte boundary, each texture on a 16-byte boundary."""
import struct

from common import MergeError


def tpf_read(data):
    """{'header': first 0x10 bytes, 'entries': [[name, 0x24-byte entry, payload]], 'wide': bool}"""
    if data[:4] != b"TPF\0" or len(data) < 0x10:
        raise MergeError("not a TPF")
    count, = struct.unpack_from("<I", data, 8)
    wide = data[0xe] == 1
    entries = []
    for i in range(count):
        at = 0x10 + i * 0x24
        entry = data[at:at + 0x24]
        if len(entry) != 0x24:
            raise MergeError("TPF entry table cut short")
        offset, size = struct.unpack_from("<Ii", entry, 0)
        name_at, = struct.unpack_from("<I", entry, 0x18)
        if wide:
            end = name_at
            while data[end:end + 2] != b"\0\0":
                end += 2
                if end > len(data):
                    raise MergeError("TPF name runs past the end")
            name = data[name_at:end].decode("utf-16-le")
        else:
            end = data.index(b"\0", name_at)
            name = data[name_at:end].decode("latin-1")
        if offset + size > len(data) or size < 0:
            raise MergeError("TPF texture data outside the file")
        entries.append([name, entry, data[offset:offset + size]])
    return {"header": data[:0x10], "entries": entries, "wide": wide}


def tpf_write(tpf):
    entries = tpf["entries"]
    wide = tpf["wide"]
    table = bytearray()
    names = bytearray()
    names_at = 0x10 + len(entries) * 0x24
    name_offsets = []
    for name, _, _ in entries:
        name_offsets.append(names_at + len(names))
        names += name.encode("utf-16-le") + b"\0\0" if wide else name.encode("latin-1") + b"\0"
    data_at = (names_at + len(names) + 15) & ~15
    blob = bytearray()
    for (name, entry, payload), name_at in zip(entries, name_offsets):
        blob += b"\0" * ((-len(blob)) % 16)
        row = bytearray(entry)
        struct.pack_into("<Ii", row, 0, data_at + len(blob), len(payload))
        struct.pack_into("<I", row, 0x18, name_at)
        table += row
        blob += payload
    head = bytearray(tpf["header"])
    struct.pack_into("<II", head, 4, len(blob), len(entries))
    padding = b"\0" * (data_at - names_at - len(names))
    return bytes(head) + bytes(table) + bytes(names) + padding + bytes(blob)


def _key(entry):
    return entry[0].lower()


def _same(a, b):
    """Two entries are the same texture when everything but the offsets agrees."""
    if a is None or b is None:
        return a is b
    ea, eb = bytearray(a[1]), bytearray(b[1])
    for e in (ea, eb):
        struct.pack_into("<I", e, 0, 0)
        struct.pack_into("<I", e, 0x18, 0)
    return ea == eb and a[2] == b[2] and a[0] == b[0]


def merge_tpf(label, vanilla, mods, report):
    """`vanilla`: the game's TPF bytes or None; `mods`: [(mod, TPF bytes)]. Returns (TPF bytes, textures taken)."""
    parsed = [(mod, tpf_read(data)) for mod, data in mods]
    base = tpf_read(vanilla) if vanilla is not None else {"header": parsed[0][1]["header"], "entries": [],
                                                          "wide": parsed[0][1]["wide"]}
    if any(t["wide"] != base["wide"] for _, t in parsed):
        raise MergeError("the TPFs use different name encodings")
    base_by = {_key(e): e for e in base["entries"]}
    versions = {}  # key -> [(mod, entry or None)] of the mods that changed it
    anchors = {}  # new key -> key of the entry before it in the first mod that adds it
    order = []
    for mod, tpf in parsed:
        own = {_key(e): e for e in tpf["entries"]}
        previous = None
        for entry in tpf["entries"]:
            key = _key(entry)
            if not _same(entry, base_by.get(key)):
                versions.setdefault(key, []).append((mod, entry))
                if key not in base_by and key not in anchors:
                    anchors[key] = previous
                    order.append(key)
            previous = key
        for key in base_by:
            if key not in own:
                versions.setdefault(key, []).append((mod, None))
    out = []
    for entry in base["entries"]:
        key = _key(entry)
        final = _pick(label, key, versions.get(key), report)
        if final is not False:
            entry = final
        if entry is not None:
            out.append(entry)
    for key in order:
        final = _pick(label, key, versions[key], report)
        if final is None:
            continue
        keys = [_key(e) for e in out]
        at = keys.index(anchors[key]) + 1 if anchors[key] in keys else len(out)
        out.insert(at, final)
    merged = dict(base)
    merged["entries"] = out
    return tpf_write(merged), len(versions)


def _pick(label, key, values, report):
    """The winning entry of a texture (None = deleted), or False when no mod changed it."""
    if not values:
        return False
    distinct = []
    for _, entry in values:
        if not any(_same(entry, d) for d in distinct):
            distinct.append(entry)
    if len(distinct) > 1:
        shown = [(m, "deletes it" if e is None else "%d bytes" % len(e[2])) for m, e in values]
        report.conflict("%s texture %s" % (label, values[-1][1][0] if values[-1][1] else key), shown, values[-1][0])
    return values[-1][1]
