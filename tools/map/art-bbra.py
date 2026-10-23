"""The map's pictures for the engine: a PNG to BBRA ("BBRA", u32 LE width and height, RGBA).

The in-game map (engine/gpu/shim/map/map_vector.cpp, map_art.cpp) reads BBRA, so the engine needs
no PNG decoder. The pictures in art/ (parchment, icons, background) are Hunter's Atlas's
source/art (GPL-2.0-or-later).

    python art-bbra.py <picture.png> <out.bbra>
"""
import struct
import sys
from pathlib import Path

from PIL import Image

MAX_SIDE = 4096  # map_vector.cpp's limit


def convert(png: Path, out: Path) -> tuple[int, int]:
    with Image.open(png) as image:
        rgba = image.convert("RGBA")
        w, h = rgba.size
        if not (1 <= w <= MAX_SIDE and 1 <= h <= MAX_SIDE):
            raise ValueError(f"{png}: {w}x{h}, the engine takes up to {MAX_SIDE} per side")
        data = b"BBRA" + struct.pack("<II", w, h) + rgba.tobytes()
    partial = out.with_name(out.name + ".partial")
    partial.write_bytes(data)
    partial.replace(out)
    return w, h


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    width, height = convert(Path(sys.argv[1]), Path(sys.argv[2]))
    print(f"{Path(sys.argv[1]).name}: {width}x{height} -> {sys.argv[2]}")
