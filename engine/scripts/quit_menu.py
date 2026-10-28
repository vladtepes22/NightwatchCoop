#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The code segment's last page and the machine-code helper the port's own patch code uses.

Only this part of the upstream port's quit_menu.py (Quit to Desktop) is here: cloth.py,
cloth_time.py, foliage_time.py and air_drag.py import it by that name, and keeping their files as
upstream has them keeps later merges clean. Quit to Desktop itself is not part of this build, so
its own range of the page (0x50D9700-0x50D9826) stays empty here.

The code segment of the 01.09 eboot ends at CODE_END; the rest of its 4 KiB page is mapped with it
(executable) and holds nothing, so the patch code that does not fit in place lives there
(src/probe.c lets patches write it; tests/test_code_caves.py keeps each script's range apart).
"""
import struct

EBOOT_BASE = 0x400000
# Image offsets (vaddr - EBOOT_BASE) in the 01.09 eboot.
CODE_END = 0x50D96DC         # end of the code segment; up to its 4 KiB page end nothing is there
CAVE = 0x50D9700
CAVE_END = (CODE_END + 0xFFF) & ~0xFFF


def rel32(at, end, target):
    return struct.pack('<i', target - (at + end))


class Code:
    """Machine code placed at an image offset, with rip-relative references."""

    def __init__(self, offset):
        self.offset, self.data = offset, bytearray()

    @property
    def here(self):
        return self.offset + len(self.data)

    def raw(self, hexbytes):
        self.data += bytes.fromhex(hexbytes)

    def call(self, target):
        self.data += b'\xe8' + struct.pack('<i', target - (self.here + 5))

    def jmp(self, target):
        self.data += b'\xe9' + struct.pack('<i', target - (self.here + 5))

    def lea(self, prefix, target):  # prefix: '488d3d' (rdi), '488d05' (rax)
        self.data += bytes.fromhex(prefix) + struct.pack('<i', target - (self.here + 7))


def check_segments(segments):
    """segments: [(start, end)] of the eboot's PT_LOADs. Raises unless the code segment ends at
    CODE_END and nothing else lies in the rest of its page."""
    if not any(end == CODE_END for _, end in segments) or \
            any(start < CAVE_END and end > CODE_END for start, end in segments if end != CODE_END):
        raise ValueError('Patch code: unexpected code segment layout')
