#pragma once

// bbport: what a failed GPU queue submit means, and whether it stops the game (vk_scheduler.cpp).
// vkQueueSubmit returns VK_SUCCESS or an error: out of host memory, out of device memory, device
// lost. Only a device lost stopped the game; the others were dropped and the game went on waiting
// for work the GPU never took (a hang with nothing in the log). Now every failed submit names its
// result in one critical log line and stops the same way a device lost does.
// BB_SUBMIT_FAIL_STOP=0: only a device lost stops, as before. From Lantern v0.9.43
// (vk_submit_result.h), without its out-of-memory retry. Values are VkResult codes.

namespace BbSubmit {

constexpr int Success = 0;
constexpr int OutOfHostMemory = -1;
constexpr int OutOfDeviceMemory = -2;
constexpr int DeviceLost = -4;

/// Whether this submit result stops the game (an error code, below VK_SUCCESS).
/// all_failures: BB_SUBMIT_FAIL_STOP (default on).
constexpr bool Stops(int result, bool all_failures) {
    if (result == DeviceLost) {
        return true;
    }
    return all_failures && result < Success; // errors are negative; positive codes are not
}

/// Plain words for the log line.
constexpr const char* Meaning(int result) {
    switch (result) {
    case Success:
        return "the work was taken";
    case OutOfHostMemory:
        return "the graphics driver ran out of system memory";
    case OutOfDeviceMemory:
        return "the graphics card ran out of video memory";
    case DeviceLost:
        return "the graphics driver stopped responding (device lost)";
    default:
        return "the graphics driver refused the work";
    }
}

} // namespace BbSubmit
