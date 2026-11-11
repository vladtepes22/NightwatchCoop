// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Faster black fades before and after a map's loading screen (visy 0.8.1). On by default; bbport.ini fast_fades=0 or
// BB_FAST_FADES=0 off. Read at start.
namespace BbFastFades {
void Install();
}
