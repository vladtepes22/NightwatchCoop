// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace BbGxQueryGuard {
/// bbgpu_patch_image, after coop::HooksInit, every mode: a GX occlusion query without a handle is
/// neither begun nor ended (crash 0x2169edc). BB_GX_QUERY_GUARD=0: off.
void Install();
} // namespace BbGxQueryGuard
