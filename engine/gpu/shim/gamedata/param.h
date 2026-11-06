// SPDX-License-Identifier: GPL-3.0-or-later
// PARAM tables decoded through their PARAMDEF (Bloodborne / DS3 64-bit layout), read from the player's own
// parambnd + paramdefbnd (mods that replace them are respected: the files are the ones the game reads).
#pragma once

#include "bytes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bbdata {

class ParamTable;
ParamTable ParseParam(Span param, Span paramdef);

/// All rows of one param. Integer fields (bitfields included) hold their value; an f32 field holds its bit
/// pattern; dummy / string / unknown-typed fields are absent. A field the paramdef does not have reads as
/// `dflt` (the party tooling's r.get(name, 0)).
class ParamTable {
public:
    std::vector<std::int32_t> ids;  // row ids in file order

    std::size_t Rows() const { return ids.size(); }
    /// Column index of `name` (the last field of that name), -1 when absent.
    int Col(const std::string& name) const {
        const auto it = index_.find(name);
        return it == index_.end() ? -1 : it->second;
    }
    std::int64_t At(std::size_t row, int col, std::int64_t dflt = 0) const {
        return col < 0 ? dflt : cells_[row * names_.size() + static_cast<std::size_t>(col)];
    }
    std::int64_t Get(std::size_t row, const std::string& name, std::int64_t dflt = 0) const {
        return At(row, Col(name), dflt);
    }

private:
    friend ParamTable ParseParam(Span param, Span paramdef);
    std::vector<std::string> names_;
    std::unordered_map<std::string, int> index_;
    std::vector<std::int64_t> cells_;  // row major
};

}  // namespace bbdata
