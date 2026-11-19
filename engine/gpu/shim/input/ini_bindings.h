// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace BbInputConfig {
using Items = std::vector<std::pair<std::string, std::string>>;
inline void (*append_bindings)(Items& lines) = nullptr;
// Nightwatch: the controller buttons' pad.<button>= lines (the Settings screen's Controller Buttons rows).
inline void (*append_pad)(Items& lines) = nullptr;

inline void Append(Items& lines) {
    if (append_bindings) append_bindings(lines);
    if (append_pad) append_pad(lines);
}
}
