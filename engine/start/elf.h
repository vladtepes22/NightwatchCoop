// SPDX-License-Identifier: GPL-2.0-or-later
// PS4 executables as the boot image needs them (was scripts/prepare.py parse_self/parse_elf/sfo):
// an already plaintext SELF, or an eboot.bin some tools unwrap to a plain ELF.
#pragma once
#include "common.h"

#include <map>
#include <variant>

namespace start {

struct Ph {
    std::uint32_t type, flags;
    std::uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct Executable {
    Bytes elf;                 ///< the ELF with every present segment at its file offset
    std::uint64_t entry = 0;
    std::vector<Ph> ph;
    std::vector<int> missing;  ///< program headers whose data the file lacks
};

Executable ParseSelf(const Bytes& data);

/// param.sfo: integers (format 0x404) and text.
using SfoValue = std::variant<std::uint32_t, std::string>;
std::map<std::string, SfoValue> ParseSfo(const Bytes& data);
std::string SfoText(const std::map<std::string, SfoValue>& sfo, const std::string& key, std::string_view fallback = "?");

/// The loadable segments of an ELF file (program header table read from the file).
std::vector<Ph> ProgramHeaders(const Bytes& elf);
/// The executable's bytes at image offset `offset` (zero past a segment's file data), or none.
std::optional<Bytes> OriginalBytes(const Bytes& elf, std::uint64_t offset, std::size_t length);

/// Bounds-checked view.
const std::uint8_t* Span(const Bytes& data, std::uint64_t offset, std::uint64_t size);

} // namespace start
