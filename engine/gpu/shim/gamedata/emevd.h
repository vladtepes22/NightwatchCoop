// SPDX-License-Identifier: GPL-3.0-or-later
// EMEVD (event scripts; Bloodborne: little endian, 64-bit, version 0xCC), EMEDF (instruction definitions,
// common.emedf) and EMELD (event names) of the player's own game, plus the expansion of parameterised events
// into concrete instances (what the game runs). Bounds checked, read only.
#pragma once

#include "bytes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bbdata {

struct EvInstr {
    std::uint32_t bank = 0, id = 0;
    std::uint32_t off = 0, len = 0;  // the argument bytes inside Emevd::buf
};
struct EvParam {
    std::int64_t instr = 0, target = 0, source = 0;
    std::int32_t count = 0;
};
struct EvEvent {
    std::int64_t id = 0;
    std::uint32_t rest = 0;
    std::vector<EvInstr> ins;
    std::vector<EvParam> params;
};
struct Emevd {
    Bytes buf;  // the decompressed file
    std::vector<EvEvent> events;
    Span Args(const EvInstr& i) const { return Span(buf.data() + i.off, i.len); }
};

/// Parses a decompressed EMEVD. Throws FormatError for anything but a Bloodborne EMEVD.
Emevd ParseEmevd(Bytes data);

struct EmedfArg {
    std::string name;  // Japanese
    std::uint32_t type = 0;
};
struct EmedfEntry {
    std::vector<EmedfArg> args;
};
class Emedf {
public:
    const EmedfEntry* Find(std::uint32_t bank, std::uint32_t id) const {
        const auto it = map_.find(Key(bank, id));
        return it == map_.end() ? nullptr : &it->second;
    }
    static std::uint64_t Key(std::uint64_t bank, std::uint64_t id) { return bank << 32 | (id & 0xffffffffu); }
    std::unordered_map<std::uint64_t, EmedfEntry> map_;
};
Emedf ParseEmedf(Span b);

/// {event id -> name} of an EMELD; empty when the bytes are not one.
std::unordered_map<std::int64_t, std::string> ParseEmeld(Span b);

/// Argument values of one instruction (EMEVD packing: each value aligned to its own size). Stops at the first
/// value that does not fit. f32 values hold their bit pattern.
std::vector<std::int64_t> UnpackArgs(const EmedfEntry& def, Span raw);

}  // namespace bbdata
