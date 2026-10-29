"""BC7 block decoder (pure Python, no dependencies) for the save editor's item icons.

Bit layout from the public Direct3D 11 BC7 format reference (modes 0-7, partition and anchor tables).
decode_block(16 bytes) -> 64 bytes RGBA (row-major 4x4). Checked against texture2ddecoder on the game's
own icon sheets by tests/test_saveedit_icons.py (with BB_GAME_DIR set and that module installed).
"""

_P2 = [
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00,
    0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C,
    0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8,
    0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
    0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
]

_P3 = [
    "0011001102212222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122",
    "0011001100220022", "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222",
    "0000111122222222", "0012001200120012", "0112011201120112", "0122012201220122", "0011011211221222",
    "0011200122002220", "0001001101121122", "0111001120012200", "0000112211221122", "0022002200221111",
    "0111011102220222", "0001000122212221", "0000001101220122", "0000110022102210", "0122012200110000",
    "0012001211222222", "0110122112210110", "0000011012211221", "0022110211020022", "0110011020022222",
    "0011012201220011", "0000200022112221", "0000000211221222", "0222002200120011", "0011001200220222",
    "0120012001200120", "0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011",
    "0011112222000011", "0101010122222222", "0000000021212121", "0022112200221122", "0022001100220011",
    "0220122102201221", "0101222222220101", "0000212121212121", "0101010101012222", "0222011102220111",
    "0002111200021112", "0000211221122112", "0222011101110222", "0002111211120002", "0110011001102222",
    "0000000021122112", "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
    "0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220",
]

_A2 = [15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
       15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15]
_A3B = [3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
        8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3]
_A3C = [15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
        15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8]

_W = {2: (0, 21, 43, 64), 3: (0, 9, 18, 27, 37, 46, 55, 64),
      4: (0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64)}

# mode: subsets, partition bits, rotation bits, index-select bits, color bits, alpha bits, endpoint pbits,
#       shared pbits, index bits, second index bits
_MODES = [(3, 4, 0, 0, 4, 0, 1, 0, 3, 0), (2, 6, 0, 0, 6, 0, 0, 1, 3, 0), (3, 6, 0, 0, 5, 0, 0, 0, 2, 0),
          (2, 6, 0, 0, 7, 0, 1, 0, 2, 0), (1, 0, 2, 1, 5, 6, 0, 0, 2, 3), (1, 0, 2, 0, 7, 8, 0, 0, 2, 2),
          (1, 0, 0, 0, 7, 7, 1, 0, 4, 0), (2, 6, 0, 0, 5, 5, 1, 0, 2, 0)]

_SUBSET = [[[0] * 16 for _ in range(64)],
           [[(m >> i) & 1 for i in range(16)] for m in _P2],
           [[int(c) for c in s] for s in _P3]]
_ANCHORS = [[(0,)] * 64, [(0, a) for a in _A2], [(0, b, c) for b, c in zip(_A3B, _A3C)]]
_BLACK = bytes(64)


def decode_block(block):
    v = int.from_bytes(block, "little")
    mode = 0
    while mode < 8 and not (v >> mode) & 1:
        mode += 1
    if mode == 8:
        return _BLACK
    ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2 = _MODES[mode]
    pos = mode + 1

    def take(n):
        nonlocal pos
        r = (v >> pos) & ((1 << n) - 1)
        pos += n
        return r

    part = take(pb)
    rot = take(rb)
    isel = take(isb)
    ne = 2 * ns
    ep = [[0, 0, 0, 255] for _ in range(ne)]
    for ch in range(3):
        for e in range(ne):
            ep[e][ch] = take(cb)
    if ab:
        for e in range(ne):
            ep[e][3] = take(ab)
    bits_c, bits_a = cb, ab
    if epb:
        for e in range(ne):
            p = take(1)
            for ch in range(4 if ab else 3):
                ep[e][ch] = (ep[e][ch] << 1) | p
        bits_c += 1
        bits_a += 1 if ab else 0
    elif spb:
        for s in range(ns):
            p = take(1)
            for e in (2 * s, 2 * s + 1):
                for ch in range(3):
                    ep[e][ch] = (ep[e][ch] << 1) | p
        bits_c += 1
    for e in ep:
        for ch in range(3):
            x = e[ch] << (8 - bits_c)
            e[ch] = x | (x >> bits_c)
        if ab:
            x = e[3] << (8 - bits_a)
            e[3] = x | (x >> bits_a)
    subset = _SUBSET[ns - 1][part]
    anchors = _ANCHORS[ns - 1][part]
    idx = [take(ib - (i in anchors)) for i in range(16)]
    idx2 = [take(ib2 - (i == 0)) for i in range(16)] if ib2 else None
    out = bytearray(64)
    if idx2 is None:
        w = _W[ib]
        for i in range(16):
            s = subset[i]
            a, b = ep[2 * s], ep[2 * s + 1]
            t = w[idx[i]]
            u = 64 - t
            o = 4 * i
            out[o] = (u * a[0] + t * b[0] + 32) >> 6
            out[o + 1] = (u * a[1] + t * b[1] + 32) >> 6
            out[o + 2] = (u * a[2] + t * b[2] + 32) >> 6
            out[o + 3] = (u * a[3] + t * b[3] + 32) >> 6
    else:
        ci, ai, cw, aw = (idx2, idx, _W[ib2], _W[ib]) if isel else (idx, idx2, _W[ib], _W[ib2])
        a, b = ep[0], ep[1]
        for i in range(16):
            t, ta = cw[ci[i]], aw[ai[i]]
            o = 4 * i
            px = [(( 64 - t) * a[0] + t * b[0] + 32) >> 6, ((64 - t) * a[1] + t * b[1] + 32) >> 6,
                  ((64 - t) * a[2] + t * b[2] + 32) >> 6, ((64 - ta) * a[3] + ta * b[3] + 32) >> 6]
            if rot:
                px[3], px[rot - 1] = px[rot - 1], px[3]
            out[o:o + 4] = bytes(px)
    return bytes(out)
