// SPDX-License-Identifier: GPL-3.0-or-later
// The on-screen keyboards' layouts and d-pad moves (gpu/shim/bbport_osk.cpp). The browser's moves are the ones its
// keyboard had before the model was shared (bbport_glyph_screen.cpp's own MovedKey / KeyInView).
#include "bbport_osk.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace BbOsk;

namespace {
int Key(const Layout& l, int view, char c) { return int(l.views[std::size_t(view)].letters.find(c)); }

void GlyphMoves() {
    const Layout& l = GlyphLayout();
    assert(l.Letters() == 36 && l.Keys() == 38);
    assert(l.Wide("Delete") == 36 && l.Wide("Done") == 37 && l.Wide("Shift") == -1);
    // the A-Z grid: rows wrap, columns go through Delete (left half) and Done (right half)
    assert(Moved(l, 0, Key(l, 0, 'a'), PadKey::kLeft) == Key(l, 0, 'f'));
    assert(Moved(l, 0, Key(l, 0, 'f'), PadKey::kRight) == Key(l, 0, 'a'));
    assert(Moved(l, 0, Key(l, 0, 'a'), PadKey::kDown) == Key(l, 0, 'g'));
    assert(Moved(l, 0, Key(l, 0, 'a'), PadKey::kUp) == 36);
    assert(Moved(l, 0, Key(l, 0, 'c'), PadKey::kUp) == 36);
    assert(Moved(l, 0, Key(l, 0, 'd'), PadKey::kUp) == 37);
    assert(Moved(l, 0, Key(l, 0, '4'), PadKey::kDown) == 36);
    assert(Moved(l, 0, Key(l, 0, '9'), PadKey::kDown) == 37);
    assert(Moved(l, 0, 36, PadKey::kRight) == 37 && Moved(l, 0, 37, PadKey::kLeft) == 36);
    assert(Moved(l, 0, 36, PadKey::kDown) == Key(l, 0, 'b'));  // under Delete's centre (1.5): b (the leftmost)
    assert(Moved(l, 0, 37, PadKey::kUp) == Key(l, 0, '8'));    // over Done's centre (4.5): 8
    // QWERTY: staggered rows go to the nearest key; g (centre 5.0) goes down to Done
    assert(Moved(l, 1, Key(l, 1, 'q'), PadKey::kDown) == Key(l, 1, 'a'));
    assert(Moved(l, 1, Key(l, 1, 'a'), PadKey::kDown) == Key(l, 1, 'z'));
    assert(Moved(l, 1, Key(l, 1, 'z'), PadKey::kUp) == Key(l, 1, 's'));
    assert(Moved(l, 1, Key(l, 1, 'g'), PadKey::kUp) == Key(l, 1, 't'));
    assert(Moved(l, 1, Key(l, 1, 'b'), PadKey::kDown) == 37);
    // the other view keeps the letter, the wide keys stay
    assert(InView(l, 0, 1, Key(l, 0, 'q')) == Key(l, 1, 'q'));
    assert(InView(l, 1, 0, 37) == 37);
}

void NameMoves() {
    const Layout& l = NameLayout();
    assert(l.Letters() == 39 && l.Keys() == 43);
    assert(l.views[0].letters.size() == l.views[1].letters.size());
    for (const char c : l.views[0].letters) assert(l.views[1].letters.find(c) != std::string::npos);
    int keys = 0;
    for (const Row& r : l.views[0].rows) keys += r.keys;
    assert(keys == 39);
    keys = 0;
    for (const Row& r : l.views[1].rows) keys += r.keys;
    assert(keys == 39);
    const int shift = l.Wide("Shift"), space = l.Wide("Space"), del = l.Wide("Delete"), done = l.Wide("Done");
    assert(shift == 39 && space == 40 && del == 41 && done == 42);
    // the grid: 4 down to the symbol row's '-' (centre 2.0 is nearer 2.0 than 3.0), the symbols down to the wide keys
    assert(Moved(l, 0, Key(l, 0, '4'), PadKey::kDown) == Key(l, 0, '-'));
    assert(Moved(l, 0, Key(l, 0, '-'), PadKey::kRight) == Key(l, 0, '\''));
    assert(Moved(l, 0, Key(l, 0, '.'), PadKey::kRight) == Key(l, 0, '-'));
    assert(Moved(l, 0, Key(l, 0, '-'), PadKey::kDown) == space);  // centre 2.0: Space (2.25) over Shift (0.75)
    assert(Moved(l, 0, Key(l, 0, '.'), PadKey::kDown) == del);    // centre 4.0: Delete (3.75)
    assert(Moved(l, 0, Key(l, 0, 'a'), PadKey::kUp) == shift);
    assert(Moved(l, 0, Key(l, 0, 'f'), PadKey::kUp) == done);
    assert(Moved(l, 0, shift, PadKey::kLeft) == done && Moved(l, 0, done, PadKey::kRight) == shift);
    assert(Moved(l, 0, space, PadKey::kUp) == Key(l, 0, '-'));
    assert(Moved(l, 0, shift, PadKey::kDown) == Key(l, 0, 'a'));
    // QWERTY: ' after l, - and . after m
    assert(Moved(l, 1, Key(l, 1, 'l'), PadKey::kRight) == Key(l, 1, '\''));
    assert(Moved(l, 1, Key(l, 1, '.'), PadKey::kRight) == Key(l, 1, 'z'));
    assert(Moved(l, 1, Key(l, 1, 'z'), PadKey::kDown) == shift);
    assert(InView(l, 0, 1, Key(l, 0, '\'')) == Key(l, 1, '\''));
    assert(InView(l, 1, 0, shift) == shift);
}
}  // namespace

int main() {
    GlyphMoves();
    NameMoves();
    std::puts("osk-test: ok");
    return 0;
}
