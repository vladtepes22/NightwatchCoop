// SPDX-License-Identifier: GPL-3.0-or-later
// start/party_wins.h: the party patches win on overlapping bytes.
// Build and run: ninja -C out/gpu party-wins-test && out/gpu/party-wins-test.exe
#include "start/party_wins.h"

#include <cassert>

using namespace partywins;

int main() {
    const Write party{100, {1, 1, 1, 1}};
    // Apart: kept, the party's write last.
    assert((PartyWins({{10, {7, 7}}, party}, {party}) == Writes{{10, {7, 7}}, party}));
    // The same float at the same address (60.0 over the party's 10.0): the other patch's bytes go.
    assert((PartyWins({{100, {9, 9, 9, 9}}, party}, {party}) == Writes{party}));
    // A write around the party's bytes keeps both ends, with the right bytes.
    assert((PartyWins({{98, {1, 2, 3, 4, 5, 6, 7, 8}}}, {party}) == Writes{{98, {1, 2}}, {104, {7, 8}}, party}));
    // Partly over one edge.
    assert((PartyWins({{102, {5, 6, 7, 8}}}, {party}) == Writes{{104, {7, 8}}, party}));
    assert(Overlaps({{103, {0}}}, {party}));
    assert(!Overlaps({{104, {0}}}, {party}));
    assert(!Overlaps({{96, {0, 0, 0, 0}}}, {party}));
    return 0;
}
