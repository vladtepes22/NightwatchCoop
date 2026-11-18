// SPDX-License-Identifier: GPL-3.0-or-later
// BC7 block decoder: one 16-byte block to 16 RGBA pixels (matches scripts/saveedit/bc7.py).
#pragma once
#include <cstdint>

namespace menu_assets {
// Decodes one BC7 block. rgba64 receives 4x4 pixels, row-major, 4 bytes (R, G, B, A) each.
// A block whose mode bits are all zero (invalid) gives 64 zero bytes.
void Bc7DecodeBlock(const std::uint8_t* block16, std::uint8_t* rgba64);
}  // namespace menu_assets
