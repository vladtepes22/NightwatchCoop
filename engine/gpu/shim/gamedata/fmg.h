// SPDX-License-Identifier: GPL-3.0-or-later
// FMG message files (msg/<language>/*.msgbnd.dcx) of the player's own game: the names the game shows for items,
// read at run time instead of being typed into the code.
#pragma once

#include "bytes.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace bbdata {

/// The strings of an FMG (Bloodborne / DS3 64-bit layout: header 0x28 bytes, id groups of 16 bytes, 8 byte string
/// offsets, UTF-16 strings) as {id -> UTF-8 text}. Entries without a string are left out. Throws FormatError.
std::unordered_map<std::int32_t, std::string> ParseFmg(Span b);

/// The item names (the "アイテム名" FMG of msg/<lang>/item.msgbnd.dcx; goods ids) of the game under `game_root`
/// (the dvdroot_ps4 folder). Tries the English folders first, then any language. Empty when no message file can be
/// read.
std::unordered_map<std::int32_t, std::string> ReadGoodsNames(const std::string& game_root);

}  // namespace bbdata
