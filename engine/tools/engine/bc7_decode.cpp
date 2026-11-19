// SPDX-License-Identifier: GPL-3.0-or-later
// BC7 block decoder (modes 0-7), exact integer math; a port of scripts/saveedit/bc7.py.
#include "bc7_decode.h"
#include <cstddef>
#include <cstring>

namespace menu_assets {
namespace {
// Two-subset partitions: bit i of the mask is the subset of pixel i.
const std::uint16_t kP2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00,
    0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C,
    0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8,
    0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
    0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};

// Three-subset partitions: one digit (subset) per pixel.
const char* const kP3[64] = {
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
};

// Anchor pixels (indices with one bit fewer): second subset of 2-subset, second and third of 3-subset.
const std::uint8_t kA2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8,
                              8, 15, 2, 8, 2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
                              6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
const std::uint8_t kA3B[64] = {3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3,
                               6, 10, 5, 8, 8, 6, 8, 5, 15, 15, 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15,
                               15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
const std::uint8_t kA3C[64] = {15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8,
                               15, 8, 3, 15, 6, 10, 15, 15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6,
                               6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8};

// Interpolation weights by index bit count (2, 3, 4 bits).
const int kW2[4] = {0, 21, 43, 64};
const int kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const int kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
const int* Weights(int bits) { return bits == 2 ? kW2 : bits == 3 ? kW3 : kW4; }

struct ModeInfo {
    int subsets, partition_bits, rotation_bits, select_bits, color_bits, alpha_bits, endpoint_pbit, shared_pbit,
        index_bits, index2_bits;
};
const ModeInfo kModes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

// Reads bits least-significant first from the 128-bit block; bits past the end read as 0.
struct BitReader {
    const std::uint8_t* b;
    int pos;
    int Take(int n) {
        int r = 0;
        for (int i = 0; i < n; ++i, ++pos) {
            const int bit = pos < 128 ? (b[pos >> 3] >> (pos & 7)) & 1 : 0;
            r |= bit << i;
        }
        return r;
    }
};
}  // namespace

void Bc7DecodeBlock(const std::uint8_t* block16, std::uint8_t* rgba64) {
    std::memset(rgba64, 0, 64);
    int mode = 0;
    while (mode < 8 && !((block16[0] >> mode) & 1)) ++mode;
    if (mode == 8) return;  // no mode bit set: transparent black, as bc7.py does

    const ModeInfo& m = kModes[mode];
    BitReader br{block16, mode + 1};
    const int part = br.Take(m.partition_bits);
    const int rot = br.Take(m.rotation_bits);
    const int isel = br.Take(m.select_bits);
    const int ne = 2 * m.subsets;
    int ep[6][4];
    for (int e = 0; e < ne; ++e) ep[e][0] = ep[e][1] = ep[e][2] = 0, ep[e][3] = 255;
    for (int ch = 0; ch < 3; ++ch)
        for (int e = 0; e < ne; ++e) ep[e][ch] = br.Take(m.color_bits);
    if (m.alpha_bits)
        for (int e = 0; e < ne; ++e) ep[e][3] = br.Take(m.alpha_bits);
    int bits_c = m.color_bits, bits_a = m.alpha_bits;
    if (m.endpoint_pbit) {
        for (int e = 0; e < ne; ++e) {
            const int p = br.Take(1);
            for (int ch = 0; ch < (m.alpha_bits ? 4 : 3); ++ch) ep[e][ch] = ep[e][ch] << 1 | p;
        }
        bits_c += 1;
        if (m.alpha_bits) bits_a += 1;
    } else if (m.shared_pbit) {
        for (int s = 0; s < m.subsets; ++s) {
            const int p = br.Take(1);
            for (int e = 2 * s; e <= 2 * s + 1; ++e)
                for (int ch = 0; ch < 3; ++ch) ep[e][ch] = ep[e][ch] << 1 | p;
        }
        bits_c += 1;
    }
    // Widen endpoints to 8 bits by repeating the top bits.
    for (int e = 0; e < ne; ++e) {
        for (int ch = 0; ch < 3; ++ch) {
            const int x = ep[e][ch] << (8 - bits_c);
            ep[e][ch] = x | (x >> bits_c);
        }
        if (m.alpha_bits) {
            const int x = ep[e][3] << (8 - bits_a);
            ep[e][3] = x | (x >> bits_a);
        }
    }

    int subset[16] = {};
    int anchor[3] = {0, -1, -1};
    if (m.subsets == 2) {
        for (int i = 0; i < 16; ++i) subset[i] = (kP2[part] >> i) & 1;
        anchor[1] = kA2[part];
    } else if (m.subsets == 3) {
        for (int i = 0; i < 16; ++i) subset[i] = kP3[part][i] - '0';
        anchor[1] = kA3B[part];
        anchor[2] = kA3C[part];
    }
    int idx[16], idx2[16];
    for (int i = 0; i < 16; ++i) {
        const bool anch = i == anchor[0] || i == anchor[1] || i == anchor[2];
        idx[i] = br.Take(m.index_bits - (anch ? 1 : 0));
    }
    if (m.index2_bits)
        for (int i = 0; i < 16; ++i) idx2[i] = br.Take(m.index2_bits - (i == 0 ? 1 : 0));

    if (!m.index2_bits) {
        const int* w = Weights(m.index_bits);
        for (int i = 0; i < 16; ++i) {
            const int* a = ep[2 * subset[i]];
            const int* b = ep[2 * subset[i] + 1];
            const int t = w[idx[i]], u = 64 - t;
            for (int c = 0; c < 4; ++c) rgba64[4 * i + c] = std::uint8_t((u * a[c] + t * b[c] + 32) >> 6);
        }
    } else {
        // Modes 4 and 5: separate color and alpha indices, optional swap, then channel rotation.
        const int* ci = isel ? idx2 : idx;
        const int* ai = isel ? idx : idx2;
        const int* cw = Weights(isel ? m.index2_bits : m.index_bits);
        const int* aw = Weights(isel ? m.index_bits : m.index2_bits);
        const int* a = ep[0];
        const int* b = ep[1];
        for (int i = 0; i < 16; ++i) {
            const int t = cw[ci[i]], ta = aw[ai[i]];
            int px[4];
            for (int c = 0; c < 3; ++c) px[c] = ((64 - t) * a[c] + t * b[c] + 32) >> 6;
            px[3] = ((64 - ta) * a[3] + ta * b[3] + 32) >> 6;
            if (rot) {
                const int tmp = px[3];
                px[3] = px[rot - 1];
                px[rot - 1] = tmp;
            }
            for (int c = 0; c < 4; ++c) rgba64[4 * i + c] = std::uint8_t(px[c]);
        }
    }
}
}  // namespace menu_assets
