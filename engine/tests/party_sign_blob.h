// SPDX-License-Identifier: GPL-3.0-or-later
// A SummonData blob (0xE0 bytes, docs/party/from_api_schema.md 5) as a member's game builds it,
// for the FromApi sign tests (the same layout as tools/party/simguest.py summon_data).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace party_test {

inline std::string sign_blob(const std::string& online_id, std::uint32_t area = 0x18010000u, int level = 50,
                             std::uint8_t sign_type = 7, std::uint8_t nat = 2) {
    std::string b(0xE0, '\0');
    auto put32 = [&](std::size_t o, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b[o + i] = static_cast<char>(v >> (8 * i));
    };
    auto putf = [&](std::size_t o, float f) {
        std::uint32_t v;
        std::memcpy(&v, &f, sizeof v);
        put32(o, v);
    };
    const std::uint32_t equip[13] = {100000, 1000000, 22000000, 6000, 270000, 271000, 272000, 273000,
                                     0xffffffffu, 0xffffffffu, 0xffffffffu, 1, 1};
    for (int i = 0; i < 13; ++i) put32(4 * i, equip[i]);
    put32(0x34, 0xff80c8ffu);
    b[0x38] = 1;
    for (int i = 0; i < 5; ++i) b[0x39 + i] = 100;
    std::memcpy(&b[0x40], online_id.data(), online_id.size() < 16 ? online_id.size() : 16);
    put32(0x50, 12345);
    put32(0x58, area);
    putf(0x5C, 10.5f);
    putf(0x60, -3.25f);
    putf(0x64, 200.0f);
    putf(0x68, 1.0f);
    put32(0x6C, 1);
    b[0x70] = static_cast<char>(level & 0xff);
    b[0x71] = static_cast<char>((level >> 8) & 0xff);
    b[0x76] = static_cast<char>(sign_type);
    b[0x78] = 30;
    b[0x7A] = 0x25;
    b[0x7C] = 2;
    std::memcpy(&b[0x7D], online_id.data(), online_id.size() < 16 ? online_id.size() : 16);
    b[0xCC] = static_cast<char>(nat);
    put32(0xD0, 0xffffffffu);
    put32(0xD8, 1);
    return b;
}

}  // namespace party_test
