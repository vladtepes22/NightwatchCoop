"""Builders of small synthetic game files for the merger's tests (no game data in the repo)."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from common import containers  # noqa: E402
from merge_tpf import tpf_write  # noqa: E402


def param(type_name, size, rows):
    """A 64-bit PARAM. rows = [(id, body bytes, name)]."""
    head = bytearray(0x40)
    type_bytes = type_name.encode("latin-1")
    head[0x0c:0x0c + len(type_bytes)] = type_bytes
    head[0x2c:0x30] = b"\x00\x04\x07\x00"
    count = len(rows)
    data_start = 0x40 + count * 0x18
    names_start = data_start + count * size
    struct.pack_into("<I", head, 0, names_start)
    struct.pack_into("<H", head, 0x0a, count)
    struct.pack_into("<Q", head, 0x30, data_start)
    table, data, names = bytearray(), bytearray(), bytearray()
    for i, (row_id, body, name) in enumerate(rows):
        assert len(body) == size
        table += struct.pack("<iiqq", row_id, 0, data_start + i * size, names_start + len(names))
        data += body
        names += name.encode("utf-16-le") + b"\0\0"
    return bytes(head) + bytes(table) + bytes(data) + bytes(names)


def paramdef(type_name, fields):
    """A paramdef. fields = [(type, internal name, byte size)]; bitfields as ("u8", "flagA:1", 1)."""
    head = bytearray(0x38)
    struct.pack_into("<H", head, 8, len(fields))
    type_bytes = type_name.encode("latin-1")
    head[0x0c:0x0c + len(type_bytes)] = type_bytes
    out = bytearray(head)
    for kind, name, size in fields:
        rec = bytearray(0xd0)
        struct.pack_into("<I", rec, 0x64, size)
        rec[0x70:0x70 + len(kind)] = kind.encode("latin-1")
        rec[0x90:0x90 + len(name)] = name.encode("latin-1")
        out += rec
    return bytes(out)


def bnd(entries):
    """A BND4 (format 0x74). entries = [(id, name, bytes)]."""
    head = bytearray(0x40)
    head[0:4] = b"BND4"
    head[0x30:0x33] = b"\x01\x74\x00"
    return containers.bnd4_write({"header": bytes(head),
                                  "entries": [[b"\x40\0\0\0", i, name, body] for i, name, body in entries]})


def dcx(payload):
    head = bytearray(0x4c)
    head[0:4] = b"DCX\0"
    head[0x28:0x2c] = b"DFLT"
    return containers.dcx_write(payload, bytes(head))


def fmg(texts):
    head = bytearray(0x28)
    head[2] = 2
    return containers.fmg_write({"header": bytes(head), "texts": dict(texts), "ranges": [], "size": 0})


def tpf(textures):
    """textures = [(name, payload)]"""
    head = bytearray(b"TPF\0" + bytes(4) + bytes(4) + b"\x04\x03\x01\x00")
    entries = []
    for name, payload in textures:
        entry = bytearray(0x24)
        entry[8] = 0x66
        entries.append([name, bytes(entry), payload])
    return tpf_write({"header": bytes(head), "entries": entries, "wide": True})


def tae(anims):
    """A TAE3 file. anims = {id: [(type, start, end, args bytes)]}: each animation has its events in one group per
    type, laid out piece by piece."""
    out = bytearray(0x80)
    out[0:12] = bytes.fromhex("54414520000000ff0c000100")
    struct.pack_into("<I", out, 0x10, 0x40)
    struct.pack_into("<i", out, 0x50, 1)
    struct.pack_into("<q", out, 0x68, 0)

    def put(blob, align=16):
        out.extend(bytes((-len(out)) % align))
        at = len(out)
        out.extend(blob)
        return at

    rows = []
    for anim_id in sorted(anims):
        events = anims[anim_id]
        times = []
        for _, start, end, _ in events:
            for t in (start, end):
                if t not in times:
                    times.append(t)
        times_at = put(b"".join(struct.pack("<f", t) for t in times)) if times else 0
        args_at = [put(args) for _, _, _, args in events]
        data_at = [put(struct.pack("<iiq", kind, 0, a)) for (kind, _, _, _), a in zip(events, args_at)]
        heads = b"".join(struct.pack("<3q", times_at + 4 * times.index(s), times_at + 4 * times.index(e), d)
                         for (_, s, e, _), d in zip(events, data_at))
        eh = put(heads) if events else 0
        kinds = []
        for kind, _, _, _ in events:
            if kind not in kinds:
                kinds.append(kind)
        groups = b""
        for kind in kinds:
            members = [i for i, ev in enumerate(events) if ev[0] == kind]
            values_at = put(b"".join(struct.pack("<i", eh + 24 * i) for i in members), 4)
            type_at = put(struct.pack("<qq", kind, 0))
            groups += struct.pack("<4q", len(members), values_at, type_at, 0)
        eg = put(groups) if kinds else 0
        fo = put(struct.pack("<qq", 0, 0))
        ao = put(struct.pack("<4q4i", eh, eg, times_at, fo, len(events), len(kinds), len(times), 0))
        rows.append((anim_id, ao))
    table = put(b"".join(struct.pack("<2q", *r) for r in rows))
    ranges = []
    for k, (anim_id, _) in enumerate(rows):
        if ranges and ranges[-1][1] + 1 == anim_id:
            ranges[-1][1] = anim_id
        else:
            ranges.append([anim_id, anim_id, table + k * 16])
    range_rows = put(b"".join(struct.pack("<2iq", *g) for g in ranges))
    range_head = put(struct.pack("<2q", len(ranges), range_rows))
    struct.pack_into("<i", out, 0x54, len(rows))
    struct.pack_into("<2q", out, 0x58, table, range_head)
    struct.pack_into("<2q", out, 0x70, len(rows), rows[0][1] if rows else 0)
    struct.pack_into("<i", out, 0x0c, len(out))
    return bytes(out)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "w" if isinstance(data, str) else "wb"
    with open(path, mode, **({"encoding": "utf-8", "newline": "\n"} if mode == "w" else {})) as f:
        f.write(data)
