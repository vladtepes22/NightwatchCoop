// SPDX-License-Identifier: GPL-2.0-or-later
// Test mode: a hidden, unfocusable Vulkan surface; no presentation or graphics draws.
// Compute, guest memory, fences and game logic still run for multiplayer tests.
#pragma once
#include <cstdlib>
namespace BbHeadless {
inline bool On() {
    static const bool enabled=[] { const char* value=std::getenv("BB_HEADLESS"); return value && value[0]=='1'; }();
    return enabled;
}
}
