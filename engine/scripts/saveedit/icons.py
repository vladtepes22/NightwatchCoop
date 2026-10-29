"""Item icons from the player's OWN game files (menu/common.tpf.dcx), cut to 80x80 PNGs in the cache folder.

Nothing game-derived ships: the PNGs are made on the player's PC from their install, like names.py's text.
Layout (checked on CUSA03173 1.09): the grid icons are 80x80 cells on 1024-wide sheets named
MENU_Icon_GGSSS, 12 cells a row and 144 a sheet; icon id N lives on group N // 1000, sheet
(N % 1000) // 144 + 1, cell (N % 1000) % 144. Groups: 0 goods, 1 weapons, 2 armor, 3-4 blood gems and runes
(3000 + the gem effect's gemIconIdOffset), 5 more goods. Sheets are PS4-tiled BC7 (TPF format 0x66) or
BC1 (format 0): 8x8-block tiles, Morton order inside a tile, tiles row-major.
"""
import struct
import zlib
from pathlib import Path

from . import bc7
from .names import _dcx

CELL, PER_ROW, PER_SHEET = 80, 12, 144
GEM_ICON_BASE = 3000
CACHE_VERSION = 1


def _textures(t):
    if t[:4] != b"TPF\0":
        raise ValueError("not a TPF")
    out = {}
    for i in range(struct.unpack_from("<I", t, 8)[0]):
        h = 0x10 + i * 0x24
        off, size = struct.unpack_from("<II", t, h)
        fmt = t[h + 8]
        w, hh = struct.unpack_from("<HH", t, h + 0x0C)
        noff = struct.unpack_from("<I", t, h + 0x18)[0]
        end = noff
        while t[end:end + 2] != b"\0\0":
            end += 2
        out[t[noff:end].decode("utf-16le")] = (off, size, fmt, w, hh)
    return out


def _tiled(x, y, bw):
    """Offset in blocks of block (x, y) in a PS4-tiled image bw blocks wide."""
    k = 0
    for b in range(3):
        k |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1)
    return ((y // 8) * ((bw + 7) // 8) + x // 8) * 64 + k


def _bc1(block):
    c0, c1, bits = struct.unpack("<HHI", block)

    def rgb(c):
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255]

    a, b = rgb(c0), rgb(c1)
    if c0 > c1:
        pal = [a, b, [(2 * a[i] + b[i] + 1) // 3 for i in range(3)] + [255],
               [(a[i] + 2 * b[i] + 1) // 3 for i in range(3)] + [255]]
    else:
        pal = [a, b, [(a[i] + b[i]) // 2 for i in range(3)] + [255], [0, 0, 0, 0]]
    return bytes(x for i in range(16) for x in pal[(bits >> (2 * i)) & 3])


def png(rgba, w, h):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def sheet_cell(icon_id):
    """(sheet name, x, y) of an icon id."""
    group, idx = divmod(icon_id, 1000)
    sheet, cell = divmod(idx, PER_SHEET)
    return f"MENU_Icon_{group:02d}{sheet + 1:03d}", (cell % PER_ROW) * CELL, (cell // PER_ROW) * CELL


class IconSheets:
    """The game's icon sheets, read once; cell(icon id) -> 80x80 RGBA bytes or None."""

    def __init__(self, game_dir):
        game = Path(game_dir)
        if not (game / "menu").is_dir() and (game / "dvdroot_ps4" / "menu").is_dir():
            game = game / "dvdroot_ps4"
        self.path = game / "menu" / "common.tpf.dcx"
        self.tpf = _dcx(self.path.read_bytes())
        self.tex = _textures(self.tpf)

    def cell(self, icon_id):
        name, x0, y0 = sheet_cell(icon_id)
        if icon_id <= 0 or name not in self.tex:
            return None
        off, size, fmt, w, h = self.tex[name]
        if x0 + CELL > w or y0 + CELL > h:
            return None
        bs, decode = (16, bc7.decode_block) if fmt == 0x66 else (8, _bc1) if fmt == 0 else (0, None)
        if decode is None:
            return None
        bw, out = w // 4, bytearray(CELL * CELL * 4)
        for by in range(CELL // 4):
            for bx in range(CELL // 4):
                o = off + _tiled(x0 // 4 + bx, y0 // 4 + by, bw) * bs
                px = decode(self.tpf[o:o + bs])
                for r in range(4):
                    d = ((by * 4 + r) * CELL + bx * 4) * 4
                    out[d:d + 16] = px[r * 16:r * 16 + 16]
        return bytes(out)


def ensure(game_dir, cache_dir, icon_ids):
    """Write <cache>/icons/<id>.png for every id not cached yet (cache keyed by the sheet file's size and
    time); returns {id: path} for the ids that have an icon."""
    folder = Path(cache_dir) / "icons"
    sheets = None
    try:
        game = Path(game_dir)
        src = game / "menu" / "common.tpf.dcx"
        if not src.is_file():
            src = game / "dvdroot_ps4" / "menu" / "common.tpf.dcx"
        st = src.stat()
        stamp = f"{CACHE_VERSION}-{st.st_size}-{int(st.st_mtime)}"
    except OSError:
        return {}
    marker = folder / "stamp.txt"
    try:
        fresh = marker.read_text(encoding="ascii") == stamp
    except OSError:
        fresh = False
    folder.mkdir(parents=True, exist_ok=True)
    if not fresh:
        for old in folder.glob("*.png"):
            old.unlink(missing_ok=True)
        marker.write_text(stamp, encoding="ascii")
    out = {}
    for i in sorted({int(i) for i in icon_ids if i and int(i) > 0}):
        p = folder / f"{i}.png"
        if not p.exists():
            if sheets is None:
                sheets = IconSheets(game_dir)
            px = sheets.cell(i)
            if px is None or not any(px[3::4]):
                continue
            tmp = p.with_suffix(".tmp")
            tmp.write_bytes(png(px, CELL, CELL))
            tmp.replace(p)
        out[i] = str(p)
    return out
