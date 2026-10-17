// SPDX-License-Identifier: GPL-3.0-or-later
// C5 spectate target: which living party member a dead player in a boss-fight limbo follows.
// Pure logic (unit test: tests/test_party_spectate.cpp); the game side is SpectateTick in
// party_b2.cpp.
#pragma once

/// living: the slots of the living party members in task order (n of them). current: the slot
/// followed so far (-1: none). advance: the spectator pressed Cross (switch to the next member).
/// Returns the slot to follow, or -1 when nobody is alive. The followed member stays while it
/// lives; when it dies (or leaves) or on a switch, the next living slot after it in task order
/// (wrapping); with one living member a switch keeps it.
inline int SpectateTarget(const int* living, int n, int current, bool advance) {
    if (!living || n <= 0) {
        return -1;
    }
    int at = -1;
    for (int i = 0; i < n; ++i) {
        if (living[i] == current) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        // Not followed yet or the followed member is gone: the next slot after it, else the first.
        for (int i = 0; i < n; ++i) {
            if (current >= 0 && living[i] > current) {
                return living[i];
            }
        }
        return living[0];
    }
    return advance ? living[(at + 1) % n] : current;
}
