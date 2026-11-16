// SPDX-License-Identifier: GPL-3.0-or-later
#include "bbport_osk.h"

#include <cmath>

namespace BbOsk {
namespace {

int RowStart(const View& v, int row) {
    int first = 0;
    for (int r = 0; r < row; ++r) first += v.rows[std::size_t(r)].keys;
    return first;
}
double WideCentre(const Layout& l, const View& v, int i) {
    const double n = double(l.wide.size());
    return v.width * (2 * i + 1) / (2 * n);
}
// The letter key in `row` whose centre is nearest `x` (in keys; the leftmost of two as near).
int NearestIn(const View& v, int row, double x) {
    const int first = RowStart(v, row);
    int best = first;
    double best_d = 1e9;
    for (int c = 0; c < v.rows[std::size_t(row)].keys; ++c) {
        const double d = std::abs(c + v.rows[std::size_t(row)].shift + 0.5 - x);
        if (d < best_d) best_d = d, best = first + c;
    }
    return best;
}
// The wide key whose centre is nearest `x` (the right one of two as near).
int NearestWide(const Layout& l, const View& v, double x) {
    int best = 0;
    double best_d = 1e9;
    for (int i = 0; i < int(l.wide.size()); ++i) {
        const double d = std::abs(WideCentre(l, v, i) - x);
        if (d <= best_d) best_d = d, best = i;
    }
    return l.Letters() + best;
}

}  // namespace

int Layout::Wide(const std::string& name) const {
    for (std::size_t i = 0; i < wide.size(); ++i)
        if (wide[i] == name) return Letters() + int(i);
    return -1;
}

int Moved(const Layout& l, int view, int key, PadKey move) {
    if (view < 0 || view >= int(l.views.size()) || key < 0 || key >= l.Keys()) return 0;
    const View& v = l.views[std::size_t(view)];
    const int rows = int(v.rows.size()), letters = l.Letters(), wide = int(l.wide.size());
    if (key >= letters) {
        const int i = key - letters;
        const double x = WideCentre(l, v, i);
        switch (move) {
        case PadKey::kLeft: return letters + (i + wide - 1) % wide;
        case PadKey::kRight: return letters + (i + 1) % wide;
        case PadKey::kUp: return NearestIn(v, rows - 1, x);
        case PadKey::kDown: return NearestIn(v, 0, x);
        default: return key;
        }
    }
    int row = 0;
    while (row < rows - 1 && key >= RowStart(v, row + 1)) ++row;
    const int first = RowStart(v, row), column = key - first, n = v.rows[std::size_t(row)].keys;
    const double x = column + v.rows[std::size_t(row)].shift + 0.5;
    switch (move) {
    case PadKey::kLeft: return first + (column + n - 1) % n;
    case PadKey::kRight: return first + (column + 1) % n;
    case PadKey::kUp: return row == 0 ? NearestWide(l, v, x) : NearestIn(v, row - 1, x);
    case PadKey::kDown: return row == rows - 1 ? NearestWide(l, v, x) : NearestIn(v, row + 1, x);
    default: return key;
    }
}

int InView(const Layout& l, int from, int to, int key) {
    if (key >= l.Letters() || from < 0 || to < 0 || from >= int(l.views.size()) || to >= int(l.views.size()))
        return key;
    const std::size_t at = l.views[std::size_t(to)].letters.find(l.views[std::size_t(from)].letters[std::size_t(key)]);
    return at == std::string::npos ? 0 : int(at);
}

const Layout& GlyphLayout() {
    static const Layout layout{
        {{"Grid", "abcdefghijklmnopqrstuvwxyz0123456789", {{6, 0}, {6, 0}, {6, 0}, {6, 0}, {6, 0}, {6, 0}}, 6},
         {"Qwerty", "1234567890qwertyuiopasdfghjklzxcvbnm", {{10, 0}, {10, 0}, {9, 0.5}, {7, 1.5}}, 10}},
        {"Delete", "Done"}};
    return layout;
}

const Layout& NameLayout() {
    static const Layout layout{
        {{"Grid", "abcdefghijklmnopqrstuvwxyz0123456789-'.",
          {{6, 0}, {6, 0}, {6, 0}, {6, 0}, {6, 0}, {6, 0}, {3, 1.5}}, 6},
         {"Qwerty", "1234567890qwertyuiopasdfghjkl'zxcvbnm-.", {{10, 0}, {10, 0}, {10, 0.5}, {9, 1}}, 10.5}},
        {"Shift", "Space", "Delete", "Done"}};
    return layout;
}

}  // namespace BbOsk
