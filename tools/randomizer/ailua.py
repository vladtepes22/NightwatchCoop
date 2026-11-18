"""A map's AI archive: adds the battle and logic scripts of enemies new to the map (LIVE: an enemy without
its battle script is nearly inert). `luagnl` = q offsets, q 0, UTF-16 global names, padding.
`luainfo` = LUAI, int 1, int count, int 0, rows of int goal id, int kind, q name, q second name, strings."""
import struct


def _utf16z(data, offset):
    end = offset
    while data[end:end + 2] != b"\0\0":
        end += 2
    return data[offset:end].decode("utf-16-le")


def gnl_read(data):
    names, i = [], 0
    while True:
        offset, = struct.unpack_from("<q", data, i * 8)
        if offset == 0:
            return names
        names.append(_utf16z(data, offset))
        i += 1


def gnl_write(names):
    base = 8 * (len(names) + 1)
    table, blob = [], bytearray()
    for name in names:
        table.append(base + len(blob))
        blob += name.encode("utf-16-le") + b"\0\0"
    return struct.pack("<%dq" % (len(names) + 1), *table, 0) + bytes(blob)


def gnl_padding(data):
    return data[len(gnl_write(gnl_read(data))):]


def info_read(data):
    if data[:4] != b"LUAI":
        raise ValueError("not a luainfo file")
    version, count, zero = struct.unpack_from("<iii", data, 4)
    rows = []
    for i in range(count):
        goal, kind, name, second = struct.unpack_from("<iiqq", data, 16 + 24 * i)
        rows.append((goal, kind, _utf16z(data, name) if name else None, _utf16z(data, second) if second else None))
    return version, zero, rows


def info_write(version, zero, rows):
    base = 16 + 24 * len(rows)
    head, blob = bytearray(b"LUAI" + struct.pack("<iii", version, len(rows), zero)), bytearray()
    for goal, kind, first, second in rows:
        offsets = []
        for text in (first, second):
            if text is None:
                offsets.append(0)
            else:
                offsets.append(base + len(blob))
                blob += text.encode("utf-16-le") + b"\0\0"
        head += struct.pack("<iiqq", goal, kind, *offsets)
    return bytes(head + blob)


def info_padding(data):
    version, zero, rows = info_read(data)
    return data[len(info_write(version, zero, rows)):]


def script_name(goal, kind):
    return "%06d_%s.lua" % (goal, kind)


def _short(entry):
    return entry[2].replace("/", "\\").rsplit("\\", 1)[1]


class Archive:
    """One parsed luabnd: {'bnd', 'files' short name -> entry, 'gnl', 'info' entries}."""

    def __init__(self, stem, bnd):
        self.stem, self.bnd = stem, bnd
        self.files = {_short(e): e for e in bnd["entries"]}
        self.gnl = self.files[stem + ".luagnl"]
        self.info = self.files[stem + ".luainfo"]

    def has(self, file_name):
        return file_name in self.files

    def info_row(self, goal, kind_code):
        for row in info_read(self.info[3])[2]:
            if row[0] == goal and row[1] == kind_code:
                return row
        return None

    def globals_of(self, script):
        """The names of this archive's globals that one of its scripts mentions."""
        return [n for n in gnl_read(self.gnl[3]) if n.encode("ascii") in script]

    def add_scripts(self, additions):
        """[(file name, bytes, info row or None, globals)]: files go before the index files, ids after the last script."""
        at = self.bnd["entries"].index(self.gnl)
        folder = self.bnd["entries"][at - 1][2].replace("/", "\\").rsplit("\\", 1)[0]
        flags = self.bnd["entries"][at - 1][0]
        next_id = max(e[1] for e in self.bnd["entries"][:at]) + 1
        used = {e[1] for e in self.bnd["entries"]}
        version, zero, rows = info_read(self.info[3])
        info_pad = info_padding(self.info[3])
        names = gnl_read(self.gnl[3])
        padding = gnl_padding(self.gnl[3])
        have = set(names)
        for file_name, body, row, script_globals in additions:
            if next_id in used:
                raise ValueError("no free id in " + self.stem)
            entry = [flags, next_id, folder + "\\" + file_name, body]
            self.bnd["entries"].insert(at, entry)
            self.files[file_name] = entry
            used.add(next_id)
            at += 1
            next_id += 1
            if row:
                rows.append(row)
            for name in script_globals:
                if name not in have:
                    have.add(name)
                    names.append(name)
        self.gnl[3] = gnl_write(names) + padding
        self.info[3] = info_write(version, zero, rows) + info_pad
