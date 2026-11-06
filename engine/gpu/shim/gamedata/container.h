// SPDX-License-Identifier: GPL-3.0-or-later
// Containers of the player's own game files: DCX (Bloodborne: DCP / DFLT, a zlib stream) and BND4. Read-only,
// bounds checked, nothing is cached or written. See docs/party/gamedata.md.
#pragma once

#include "bytes.h"

#include <string>
#include <vector>

namespace bbdata {

/// The whole file. Throws FormatError (with the path) when it cannot be read.
Bytes ReadFile(const std::string& path);

/// A DCX file's decompressed bytes (header "DCX\0", "DCP\0" + "DFLT" at 0x24 / 0x28, big endian
/// uncompressed / compressed sizes at 0x1c / 0x20, the zlib stream at 0x4c); any other bytes are returned
/// unchanged. Another DCX compression, a damaged stream or a size mismatch throws FormatError.
Bytes DecodeMaybeDcx(Bytes raw);

/// ReadFile + DecodeMaybeDcx.
Bytes ReadMaybeDcx(const std::string& path);

/// Test aid: a DFLT DCX around `data`.
Bytes MakeDcx(Span data);

struct BndEntry {
    std::string name;  // as stored (UTF-8 for a unicode BND4)
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

/// The entries of a BND4 (the games' parambnd / paramdefbnd layout: entries of `entry size` bytes from 0x40,
/// the size at +8, data offset + id + name offset at +0x18, names UTF-16 when byte 0x30 is set). Entry data is
/// `b.Sub(offset, size)`.
std::vector<BndEntry> ParseBnd4(Span b);

/// The entry whose name ends with `\<name>` after '/' became '\' (Python party tooling: the param bnd names
/// are paths like "N:\...\param\gameparam\ItemLotParam.param"). Null when absent.
const BndEntry* FindBndEntry(const std::vector<BndEntry>& entries, const std::string& name_with_ext);

}  // namespace bbdata
