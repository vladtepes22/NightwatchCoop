// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
#include "pad.h"
union SDL_Event;
struct SDL_Window;

enum class InputDevice { KeyboardMouse, Pad };

InputDevice host_input_device();

void host_log(const char* format, ...) __attribute__((format(printf, 1, 2)));

namespace BbInput {

bool Install(uint8_t* image, size_t size);
bool Enabled();

void Event(const SDL_Event& event, bool blocked);

void Pump(SDL_Window* window, bool text_active);

void Tick();

bool MouseIsMenuPointer();
bool PointerOnStage(float& x, float& y);
bool TextEntryOpen();

PadState LastPad();
/// Nightwatch: our item-list screens (Settings, the glyph browser), each frame while one is up (a call lasts a
/// quarter second):
/// - the left stick moves the rows: tilted up / down past half way it is the d-pad's Up / Down, in LastPad and for
///   the game (the stick itself rests: the hunter does not walk);
/// - the mouse wheel never moves the cursor: its turns are kept for TakeWheelLines (the view scrolls, nothing else);
/// - the game gets L1 / R1 as the d-pad's Left / Right (the book switches tabs with those).
struct ScreenInput {
    /// The screen's own rows (Settings): the stick's sideways tilt is Left / Right too (in LastPad); the game never
    /// gets the real Left / Right.
    bool own_rows = false;
    /// The game gets no buttons and rests the sticks (a pop-up takes every button).
    bool all = false;
};
void HoldScreenInput(const ScreenInput& how);
/// The wheel's turns while HoldScreenInput holds, since the last call, in lines (down positive).
int TakeWheelLines();

}

extern "C" {
#endif

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2, touch_count;
    struct { uint16_t x, y; uint8_t id; } touch[2];
} BbPcPad;

int bbgpu_pc_input(BbPcPad* pad, const bool* keys, int nkeys, int gamepad_present);
/* After it returned 0 (the PC controls are off): our screens' share of the pad (LastPad, HoldScreenInput). */
void bbgpu_screen_input(BbPcPad* pad);

int bbgpu_menu_buttons(uint32_t* confirm, uint32_t* back);

int runtime_pad_type(void);
int runtime_pad_button(uint32_t ps_button, int right_side);
/* Nightwatch: the controller button a game button has by default (menus always use those), or -1. */
int runtime_pad_default_button(uint32_t ps_button, int right_side);

/* Nightwatch: the Settings screen's Controller Buttons (src/runtime_pad.c). A row per game button (Cross, Circle,
 * Square, Triangle, L1, R1, L2, R2, L3, R3, Options, Touchpad (left), Touchpad (right), D-pad Up, Down, Left,
 * Right, Ping, Gather): the physical buttons
 * that press it. A capture takes the next pressed button for a row and swaps it with the row that had it; meanwhile
 * (and until everything is let go) the game and the keys get nothing. Escape or 8 s keeps the row as it was. */
int runtime_pad_remap_rows(void);
/* A row's physical buttons (SDL_GamepadButton; the triggers SDL_GAMEPAD_BUTTON_COUNT, + 1): how many (up to max in out). */
int runtime_pad_remap_buttons(int row, int* out, int max);
void runtime_pad_remap_describe(int row, char* out, size_t size);
void runtime_pad_remap_capture(int row); /* -1 cancels */
int runtime_pad_remap_capturing(void);   /* the row, or -1 */
int runtime_pad_remap_blocking(void);    /* capturing, or the press that ended it still held */
void runtime_pad_remap_defaults(void);
uint64_t runtime_pad_remap_serial(void);
/* bbport.ini's pad.<button>= line for a row (0: none to write, the rows never changed from the defaults). */
int runtime_pad_remap_ini(int row, char* key, size_t key_size, char* value, size_t value_size);

#ifdef __cplusplus
}
#endif
