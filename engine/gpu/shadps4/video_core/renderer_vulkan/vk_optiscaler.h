// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: OptiScaler (a DLL players drop next to the executable, often as winmm.dll) hooks
// vkCreateDevice, vkGetDeviceProcAddr, vkQueueSubmit and takes over the NGX entry points: our
// DLSS runs through its own feature, which may call vkDeviceWaitIdle from the thread recording
// the evaluate (a release, or a backend change from its menu). It is allowed; while it is loaded
// a compatibility mode keeps the engine on the paths it handles. BB_OPTISCALER_COMPAT=0 turns
// the whole mode off; each part has its own switch as well.
#pragma once

#include <string>

namespace Vulkan::OptiScaler {

struct Info {
    bool loaded = false;
    std::string module;  ///< the file name it was loaded as (winmm.dll, dxgi.dll, ...)
    std::string version; ///< from its version resource, empty when it has none
};

/// Looks once for a loaded OptiScaler and prints one line about it and the compatibility mode.
const Info& Detect();
/// OptiScaler is loaded and BB_OPTISCALER_COMPAT is not 0.
bool Compat();
/// Compat() and the part's own switch (`name`, e.g. "BB_OPTISCALER_SYNC_SUBMIT") is not 0.
bool Part(const char* name);

} // namespace Vulkan::OptiScaler
