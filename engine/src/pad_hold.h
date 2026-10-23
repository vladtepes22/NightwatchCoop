/* SPDX-License-Identifier: GPL-2.0-or-later
 * A controller button with a second use when held (runtime_pad.c: the map on Options/Start). A tap
 * stays the game's: the press is held back and sent when the button is let go of before the hold
 * time, for PAD_HOLD_TAP_US so the game sees it. A hold of hold_us fires once and the game never
 * sees that press. hold_us 0: the button does nothing in the game, so a press fires at once.
 * Pure, so tests/test_pad_hold.c covers it. */
#ifndef PAD_HOLD_H
#define PAD_HOLD_H

#include <stdint.h>

#define PAD_HOLD_TAP_US 100000u

enum { PAD_HOLD_IDLE, PAD_HOLD_PRESSED, PAD_HOLD_FIRED };
typedef struct {
    int state;
    uint64_t since, tap_until;
} PadHold;

/* What the game sees of the button this sample. */
enum { PAD_GAME_ASIS, PAD_GAME_HIDE, PAD_GAME_PRESS };

/* After the game's input was held back (a menu or the map over it): a button still held is the one
 * that closed it, never a new press. */
static inline void pad_hold_reset(PadHold *h, int down) {
    h->state = down ? PAD_HOLD_FIRED : PAD_HOLD_IDLE;
    h->tap_until = 0;
}

/* One sample: down = the button held now. Returns 1 when the hold fires; *game says what the game
 * gets of the button. */
static inline int pad_hold_step(PadHold *h, int down, uint64_t now, uint64_t hold_us, int *game) {
    *game = PAD_GAME_HIDE;
    switch (h->state) {
    case PAD_HOLD_IDLE:
        if (!down) {
            if (h->tap_until && now < h->tap_until) *game = PAD_GAME_PRESS;
            else { h->tap_until = 0; *game = PAD_GAME_ASIS; }
            return 0;
        }
        h->state = PAD_HOLD_PRESSED;
        h->since = now;
        h->tap_until = 0;
        /* fall through */
    case PAD_HOLD_PRESSED:
        if (!down) {
            h->state = PAD_HOLD_IDLE;
            h->tap_until = now + PAD_HOLD_TAP_US;
            *game = PAD_GAME_PRESS;
            return 0;
        }
        if (now - h->since >= hold_us) {
            h->state = PAD_HOLD_FIRED;
            return 1;
        }
        return 0;
    default:
        if (!down) h->state = PAD_HOLD_IDLE;
        return 0;
    }
}

#endif
