// SPDX-License-Identifier: GPL-3.0-or-later
// When the port may show a remote player chr itself (party_b2.cpp RemoteVisibleTick), the parts
// without the game. tests/test_party_remote_show.cpp.
//
// A remote phantom is created hidden by its task's STEP_Create (chr +0x1E2 bit 4 clear) at the map
// origin with HP 1: the defaults of a chr whose member's data has not arrived yet. The member's
// first data (the owner's own chr state) brings its position, HP and flags, the draw bit with
// them, and the game's own fade-in runs. The port's shows (the 4 s safety net, the show after a
// rewind) are for chrs that have their data and still no draw bit; before the data they fired on
// a guest whose data was late (~10 s on a crash-restarted host, restartdraw), ahead of the game.
// Never showing a remote chr before its member's data keeps the game's own path in charge.
#pragma once

#include <cmath>

namespace coop::remote_show {

/// Still the defaults STEP_Create gives the chr: HP 1 (or unread, <= 1) and within 1 m of the map
/// origin on the ground plane (the height is 0.0 .. 0.6 there). A real member at 1 HP standing
/// at a map's origin is the only false match; it is shown when it moves or heals.
inline bool AwaitsFirstData(int hp, const float pos[3]) {
    return hp <= 1 && std::fabs(pos[0]) < 1.0f && std::fabs(pos[2]) < 1.0f;
}

/// The safety net (a remote chr without the draw bit for `hidden_for` s): never before the
/// member's first data, which brings the draw bit itself.
constexpr double kSafetyNetSeconds = 4.0;
inline bool SafetyNetShows(bool awaits_first_data, bool draw_bit, double hidden_for) {
    return !awaits_first_data && !draw_bit && hidden_for >= kSafetyNetSeconds;
}

}  // namespace coop::remote_show
