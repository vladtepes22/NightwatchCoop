"""Icon cutting and the BC7 decoder; the game comparison runs only where the game and texture2ddecoder are."""
import os
from pathlib import Path
import struct
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from saveedit import bc7, flagsdata, icons  # noqa: E402


def mode6(endpoint, pbit):
    """A BC7 mode 6 block whose eight 7-bit endpoints are all `endpoint`, all indices 0."""
    v = 1 << 6
    for k in range(8):
        v |= endpoint << (7 + 7 * k)
    v |= (pbit | pbit << 1) << 63
    return v.to_bytes(16, "little")


class IconTests(unittest.TestCase):
    def test_bc7_constant_blocks(self):
        self.assertEqual(bc7.decode_block(mode6(127, 1)), b"\xff" * 64)
        self.assertEqual(bc7.decode_block(mode6(0, 0)), bytes(64))
        self.assertEqual(bc7.decode_block(bytes(16)), bytes(64))  # reserved mode 8: black

    def test_sheet_layout(self):
        self.assertEqual(icons.sheet_cell(1001), ("MENU_Icon_01001", 80, 0))
        self.assertEqual(icons.sheet_cell(4216), ("MENU_Icon_04002", 0, 6 * 80))  # cell 72 of sheet 2
        self.assertEqual(icons._tiled(0, 0, 256), 0)
        self.assertEqual(icons._tiled(1, 1, 256), 3)
        self.assertEqual(icons._tiled(8, 0, 256), 64)
        self.assertEqual(icons._tiled(0, 8, 256), 32 * 64)

    def test_png(self):
        data = icons.png(bytes(range(16)), 2, 2)
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        idat = data.index(b"IDAT")
        size, = struct.unpack(">I", data[idat - 4:idat])
        self.assertEqual(zlib.decompress(data[idat + 4:idat + 4 + size]), b"\0" + bytes(range(8)) + b"\0" + bytes(range(8, 16)))

    def test_only_ending_seen_flags_are_offered(self):
        self.assertEqual([f for f, *_ in flagsdata.endings()], [6600, 6601, 6602, 6603])

    @unittest.skipUnless(os.environ.get("BB_GAME_DIR"), "set BB_GAME_DIR to compare with the game's sheets")
    def test_bc7_matches_reference_on_game_sheets(self):
        try:
            import texture2ddecoder
        except ImportError:
            self.skipTest("texture2ddecoder not installed")
        sheets = icons.IconSheets(Path(os.environ["BB_GAME_DIR"]))
        off, size, fmt, w, h = sheets.tex["MENU_Icon_03001"]
        bw = w // 4
        linear = b"".join(sheets.tpf[off + icons._tiled(x, y, bw) * 16:][:16] for y in range(h // 4) for x in range(bw))
        ref = texture2ddecoder.decode_bc7(linear, w, h)  # BGRA
        for y in range(0, h // 4, 16):
            for x in range(bw):
                px = bc7.decode_block(linear[(y * bw + x) * 16:][:16])
                for r in range(4):
                    o = ((y * 4 + r) * w + x * 4) * 4
                    row = ref[o:o + 16]
                    self.assertEqual(px[r * 16:r * 16 + 16], bytes(c for i in range(4) for c in (row[4 * i + 2], row[4 * i + 1], row[4 * i], row[4 * i + 3])))


if __name__ == "__main__":
    unittest.main()
