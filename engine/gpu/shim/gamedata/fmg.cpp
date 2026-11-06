// SPDX-License-Identifier: GPL-3.0-or-later
#include "fmg.h"

#include "container.h"

#include <algorithm>
#include <filesystem>

namespace bbdata {

std::unordered_map<std::int32_t, std::string> ParseFmg(Span b) {
    std::unordered_map<std::int32_t, std::string> out;
    if (b.n < 0x28 || b.Le<std::uint8_t>(2) != 2) {
        throw FormatError("not a version 2 FMG");
    }
    const std::uint32_t groups = b.Le<std::uint32_t>(0x0c);
    const std::uint32_t strings = b.Le<std::uint32_t>(0x10);
    const std::int64_t offsets = b.Le<std::uint32_t>(0x18);
    if (groups > (1u << 20) || strings > (1u << 24)) {
        throw FormatError("FMG counts implausible");
    }
    for (std::uint32_t g = 0; g < groups; ++g) {
        const std::int64_t o = 0x28 + static_cast<std::int64_t>(g) * 16;
        const std::int32_t index = b.Le<std::int32_t>(o), first = b.Le<std::int32_t>(o + 4), last = b.Le<std::int32_t>(o + 8);
        for (std::int64_t id = first; id <= last; ++id) {
            const std::int64_t si = index + (id - first);
            if (si < 0 || si >= static_cast<std::int64_t>(strings)) continue;
            const std::int64_t so = b.Le<std::int64_t>(offsets + si * 8);
            if (so > 0) out[static_cast<std::int32_t>(id)] = Utf16z(b, so);
        }
    }
    return out;
}

std::unordered_map<std::int32_t, std::string> ReadGoodsNames(const std::string& game_root) {
    namespace fs = std::filesystem;
    std::vector<std::string> langs = {"engus", "enggb"};
    std::error_code ec;
    if (fs::is_directory(fs::path(game_root) / "msg", ec)) {
        std::vector<std::string> all;
        for (const auto& e : fs::directory_iterator(fs::path(game_root) / "msg", ec)) all.push_back(e.path().filename().string());
        std::sort(all.begin(), all.end());
        langs.insert(langs.end(), all.begin(), all.end());
    }
    for (const std::string& lang : langs) {
        const fs::path p = fs::path(game_root) / "msg" / lang / "item.msgbnd.dcx";
        if (!fs::is_regular_file(p, ec)) continue;
        try {
            const Bytes file = ReadMaybeDcx(p.string());
            for (const BndEntry& e : ParseBnd4(file)) {
                if (e.name.size() >= 4 && e.name.compare(e.name.size() - 4, 4, ".fmg") == 0 && e.name.find("アイテム名") != std::string::npos) {
                    return ParseFmg(Span(file).Sub(static_cast<std::int64_t>(e.offset), e.size));
                }
            }
        } catch (const FormatError&) {
        }
    }
    return {};
}

}  // namespace bbdata
