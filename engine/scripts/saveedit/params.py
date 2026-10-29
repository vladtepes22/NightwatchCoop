"""Read rows of the game's own params (param/gameparam) by field name, using its paramdefs.

Layout (checked on CUSA03173 1.09): paramdef: u16 field count @0x08, u16 field size @0x0A, u64 fields
offset @0x30; per field the display type @+0x40, the internal type @+0x70 and the internal name @+0x90
("name", "name:bits" or "name[n]"). Bit fields share one storage unit of their type's size. Param: u16 row
count @0x0A, rows at 0x40 + i * 0x18 {i32 id, ..., i64 data offset @+8}.
"""
import re
import struct

from .names import _bnd4, _dcx

_SIZE = {"s8": 1, "u8": 1, "s16": 2, "u16": 2, "s32": 4, "u32": 4, "f32": 4, "dummy8": 1, "fixstr": 1, "fixstrW": 2}
_FMT = {"s8": "b", "u8": "B", "s16": "h", "u16": "H", "s32": "i", "u32": "I", "f32": "f"}


def _cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def layout(paramdef):
    """{field name: (struct format, byte offset, bit count or None, bit shift)}."""
    count, fsize = struct.unpack_from("<HH", paramdef, 8)
    at, = struct.unpack_from("<Q", paramdef, 0x30)
    out, off, bit, unit = {}, 0, 0, 0  # unit: byte size of the open bit-field storage unit (0 = none)
    for i in range(count):
        o = at + i * fsize
        kind = _cstr(paramdef[o + 0x70:o + 0x90])
        if kind not in _SIZE:
            kind = _cstr(paramdef[o + 0x40:o + 0x48])
        m = re.match(r"(\w+)(?::(\d+))?(?:\[(\d+)\])?", _cstr(paramdef[o + 0x90:o + 0xB0]))
        name, bits, arr = m.group(1), m.group(2), m.group(3)
        size = _SIZE.get(kind, 1)
        if bits:
            bits = int(bits)
            if unit and (unit != size or bit + bits > 8 * unit):
                off, bit, unit = off + unit, 0, 0
            unit = size
            if kind in _FMT:
                out[name] = (_FMT[kind], off, bits, bit)
            bit += bits
            continue
        if unit:
            off, bit, unit = off + unit, 0, 0
        if kind in _FMT and not arr:
            out[name] = (_FMT[kind], off, None, 0)
        off += size * (int(arr) if arr else 1)
    return out


class Params:
    """The game's gameparam and paramdef files, read once; table(name, fields) -> {row id: (values...)}."""

    def __init__(self, game_dir):
        def short(k):
            return k.replace("/", "\\").split("\\")[-1].rsplit(".", 1)[0]
        self._defs = {short(k): v for k, v in
                      _bnd4(_dcx((game_dir / "paramdef" / "paramdef.paramdefbnd.dcx").read_bytes())).items()}
        self._params = {short(k): v for k, v in
                        _bnd4(_dcx((game_dir / "param" / "gameparam" / "gameparam.parambnd.dcx").read_bytes())).items()}

    def table(self, name, fields):
        spec = layout(self._defs[name])
        cols = [spec[f] for f in fields]
        b = self._params[name]
        out = {}
        for i in range(struct.unpack_from("<H", b, 0x0A)[0]):
            rid, = struct.unpack_from("<i", b, 0x40 + i * 0x18)
            data, = struct.unpack_from("<q", b, 0x40 + i * 0x18 + 8)
            row = []
            for fmt, off, bits, shift in cols:
                v, = struct.unpack_from("<" + fmt, b, data + off)
                row.append((v >> shift) & ((1 << bits) - 1) if bits else v)
            out[rid] = tuple(row)
        return out
