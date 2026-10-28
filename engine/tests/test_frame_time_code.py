# SPDX-License-Identifier: GPL-3.0-or-later
"""start/frame_time_code.inc (what bb-start applies) is what the frame-time scripts make."""
import os
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import frame_time_code  # noqa: E402
import patches  # noqa: E402
import quit_menu  # noqa: E402


class FrameTimeCodeTest(unittest.TestCase):
    def test_the_native_start_has_the_scripts_code(self):
        self.assertEqual(frame_time_code.INCLUDE.read_text(encoding='utf-8'), frame_time_code.render(),
                         'run: python scripts/frame_time_code.py --write')

    def test_every_write_is_a_checked_site_or_in_the_last_code_page(self):
        for name, module, _env, _setting, _needs, originals in frame_time_code.GROUPS:
            sites = dict(originals)
            for at, data in module.code_writes():
                if at in sites:
                    self.assertEqual(len(data), len(sites[at]), name)
                else:
                    self.assertGreaterEqual(at, quit_menu.CODE_END, name)
                    self.assertLessEqual(at + len(data), quit_menu.CAVE_END, name)

    def test_switches_leave_groups_out(self):
        def image(originals):
            # A fake eboot: an ELF header with one PT_LOAD over the code segment, holding the originals.
            size = quit_menu.CODE_END
            elf = bytearray(0x40 + 0x38)
            struct.pack_into('<Q', elf, 0x20, 0x40)
            struct.pack_into('<HH', elf, 0x36, 0x38, 1)
            struct.pack_into('<IIQQQQQQ', elf, 0x40, 1, 5, 0x1000, 0, 0, size, size, 0x1000)
            body = bytearray(size)
            for at, data in originals:
                body[at:at + len(data)] = data
            return bytes(elf) + bytes(0x1000 - len(elf)) + bytes(body)

        elf = image([o for g in frame_time_code.GROUPS for o in g[5]])
        segments = patches.eboot_segments(elf)
        names = []
        with open(os.devnull, 'w') as quiet:
            stdout, sys.stdout = sys.stdout, quiet
            try:
                all_on = patches.frame_time_writes(elf, segments, {}, names, env={})
                no_time = patches.frame_time_writes(elf, segments, {}, [], env={'BB_CLOTH_TIME': '0'})
                no_cloth = patches.frame_time_writes(elf, segments, {'steady_cloth': '0'}, [], env={})
                other = patches.frame_time_writes(image([]), segments, {}, [], env={})
            finally:
                sys.stdout = stdout
        self.assertEqual(names, [g[0] for g in frame_time_code.GROUPS])
        self.assertEqual(len(all_on), sum(len(g[1].code_writes()) for g in frame_time_code.GROUPS))
        # Steady cloth needs the steady time; the others don't.
        self.assertEqual(len(no_time), sum(len(g[1].code_writes()) for g in frame_time_code.GROUPS
                                           if not g[4] and g[2] != 'BB_CLOTH_TIME'))
        self.assertEqual(len(no_cloth), len(all_on) - len(frame_time_code.cloth.code_writes()))
        self.assertEqual(other, [])  # another eboot: nothing


if __name__ == '__main__':
    unittest.main()
