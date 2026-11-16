// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch: the on-screen keyboards a controller types with (the Chalice Glyph Browser's search bar, the Contract
// screen's name): their layouts and how the d-pad moves on them. Pure: no game code (tests/test_osk.cpp). The keys
// themselves are drawn by generated movies (tools/engine/menu_assets.cpp KeyboardBoard): Key_0 .. Key_<n>, the letter
// keys in a view's letter order, then the wide keys under them.
#pragma once

#include <string>
#include <vector>

namespace BbOsk {

/// A controller button in a quiet text box: the d-pad, Cross (press the key), Square (delete), Options (done),
/// Circle (cancel), Triangle (the other view).
enum class PadKey { kNone, kUp, kDown, kLeft, kRight, kPress, kDelete, kDone, kCancel, kToggle };
/// What a press typed: a character (> 0), nothing (0) or one of these.
constexpr int kTypedDelete = -1, kTypedDone = -2, kTypedCancel = -3;

struct Row {
    int keys;      ///< letter keys in the row
    double shift;  ///< the row's offset, in keys
};
struct View {
    std::string name;     ///< its sprite under the keyboard
    std::string letters;  ///< every view of a layout holds the same letters, in its own order
    std::vector<Row> rows;
    double width;         ///< the rows' width in keys; the wide keys share it equally
};
struct Layout {
    std::vector<View> views;
    std::vector<std::string> wide;  ///< the wide keys' names, left to right (the same in every view)
    int Letters() const { return views.empty() ? 0 : int(views[0].letters.size()); }
    int Keys() const { return Letters() + int(wide.size()); }
    /// The wide key named `name`'s index (Letters() + its place), or -1.
    int Wide(const std::string& name) const;
};

/// Left / Right wrap in a row (the wide keys are a row too); Up / Down go to the nearest key of the next row (by the
/// key centres, the leftmost of two as near; into the wide keys, the right one), wrapping past the first and the last
/// row through the wide keys.
int Moved(const Layout& layout, int view, int key, PadKey move);
/// The same key in the view `to`: a letter stays on its letter, a wide key stays.
int InView(const Layout& layout, int from, int to, int key);

/// The Chalice Glyph Browser's keyboard: the A-Z grid (a..z, 0..9 in six rows of six) and QWERTY, then Delete and Done.
const Layout& GlyphLayout();
/// The Contract screen's name keyboard: the A-Z grid (a..z, 0..9 in six rows of six, then - ' .) and QWERTY (- ' . where
/// a keyboard has them), then Shift, Space, Delete and Done.
const Layout& NameLayout();

}  // namespace BbOsk
