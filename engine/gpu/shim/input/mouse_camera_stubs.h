// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from bbhost db5457cbd447f4ef129b58df0a78e0a8819c7033.
#pragma once

// The asm stubs of the mouse camera's hooks on the game's follow camera
// (engine/mouse_camera_stubs.cpp).

#include <cstdint>

// The stubs' asm reaches their data PC-relative: hidden in the Linux shared library (lld refuses
// that for symbols the library exports).
#ifdef _WIN32
#define BB_STUB_DATA
#else
#define BB_STUB_DATA __attribute__((visibility("hidden")))
#endif

extern "C" {
// What the store stub calls - thunk_wrap(free_camera_hook) in the game, a
// test's own function in the kit - as (cam, &the update's input byte,
// stick pitch, stick yaw), and where both stubs go on: the guest's 0x183ce67.
extern BB_STUB_DATA void* g_mouse_camera_host;
extern BB_STUB_DATA std::uint64_t g_mouse_camera_resume;
// Set by the fast turn's stub (mouse_camera_step.h).
extern BB_STUB_DATA std::uint8_t bb_mouse_camera_free_now;
// At the free camera's store, 0x183ce54 (19 bytes, a jmp [rip+0] there): the
// two stores it displaced, then the host, then 0x183ce67.
void bb_mouse_camera_stub();
// At each of the fast turn's exits, 0x183f9bc and 0x183fabf (14 bytes): the
// pitch store it displaced, the free camera noted as run, then 0x183ce67.
void bb_mouse_camera_ramp_stub();
}
