/* src/havok_event_guard.h: the recovery of a fault in a Havok behaviour event of a freed animation
 * (CL-078659), on a fake guest stack and a synthetic image. Built as out/havok-event-guard-test,
 * run by test_havok_event_guard.py. */
#include "havok_event_guard.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static const uint64_t kImage = 0x800000000ULL; /* guest address of image offset 0 */

/* A synthetic image holding just the bytes the guard checks. */
static uint8_t *make_image(uint64_t size) {
    uint8_t *img = calloc(1, size);
    assert(img);
    for (size_t i = 0; i < sizeof heg_checks / sizeof heg_checks[0]; ++i)
        memcpy(img + heg_checks[i].off, heg_checks[i].bytes, heg_checks[i].len);
    for (size_t i = 0; i < 6; ++i) {
        const uint32_t at = heg_walk_calls[i];
        const int32_t rel = (int32_t)((int64_t)HEG_WALK - (at + 5));
        img[at] = 0xe8;
        memcpy(img + at + 1, &rel, 4);
    }
    return img;
}

int main(void) {
    /* 1. The byte check: the real layout passes; any changed byte fails. */
    const uint64_t size = 0x1a30000;
    uint8_t *img = make_image(size);
    assert(heg_image_matches(img, size));
    assert(!heg_image_matches(NULL, size));
    assert(!heg_image_matches(img, 0x1a21000)); /* too small */
    img[0x1a2112a] ^= 0xff; /* the +0x18 of the checked object read */
    assert(!heg_image_matches(img, size));
    img[0x1a2112a] ^= 0xff;
    assert(heg_image_matches(img, size));
    img[0x1a21392 + 1] ^= 1; /* a walker call that goes elsewhere */
    assert(!heg_image_matches(img, size));
    img[0x1a21392 + 1] ^= 1;
    img[0xf5bdd2] = 0x90; /* the walker's ret */
    assert(!heg_image_matches(img, size));
    img[0xf5bdd2] = 0xc3;
    assert(heg_image_matches(img, size));
    free(img);

    /* 2. The crash of the dump: in 0x1a21100's body (cmp dword [rax],0 with rax = 1). */
    const uint64_t rbp = 0x43058f910ULL;
    heg_regs r = {0x1a21134, rbp - 0x100, rbp, 0x1013e6e270ULL, 0x10159d1b88ULL, 0x1013e6e190ULL};
    assert(heg_classify(&r, NULL, kImage) == HEG_IN_PLAY);
    assert(heg_recover(&r, NULL, kImage) == HEG_IN_PLAY);
    assert(r.rip == HEG_EXIT && r.rsp == rbp - 0x100 && r.rbp == rbp);
    assert(r.rbx == 0x1013e6e270ULL && r.r14 == 0x10159d1b88ULL && r.r15 == 0x1013e6e190ULL);

    /* The body ends at the early-out itself: a fault there or before the object read is not ours. */
    heg_regs edge = {HEG_EXIT, 0, rbp, 0, 0, 0};
    assert(heg_recover(&edge, NULL, kImage) == HEG_NONE && edge.rip == HEG_EXIT && edge.rsp == 0);
    edge.rip = HEG_BODY_FROM - 1;
    assert(heg_classify(&edge, NULL, kImage) == HEG_NONE);
    edge.rip = HEG_PLAY;
    assert(heg_classify(&edge, NULL, kImage) == HEG_NONE);

    /* 3. A fault in the walker called from 0x1a21100: its four pushes are popped, the walker's own
     *    use of rbp as a scratch register does not leak, and 0x1a21100 resumes at its exit. */
    for (size_t i = 0; i < 6; ++i) {
        uint64_t stack[5] = {0x1111, 0x2222, 0x3333, rbp, kImage + heg_walk_calls[i] + 5};
        heg_regs w = {0xf5bd6b, 0x43058f7f0ULL, /* rbp as scratch: */ 0x1, 0xaaaa, 0xbbbb, 0xcccc};
        assert(heg_recover(&w, stack, kImage) == HEG_IN_WALK);
        assert(w.rip == HEG_EXIT && w.rbp == rbp && w.rsp == rbp - 0x100);
        assert(w.rbx == 0x1111 && w.r14 == 0x2222 && w.r15 == 0x3333);
    }

    /* 4. The walker called from anyone else (0x12e76a0, 0x1a213c0, 0x1fce3b0) is not touched. */
    {
        uint64_t stack[5] = {1, 2, 3, rbp, kImage + 0x12e7795};
        heg_regs w = {0xf5bd6b, 0x1000, 0x7, 4, 5, 6};
        assert(heg_recover(&w, stack, kImage) == HEG_NONE);
        assert(w.rip == 0xf5bd6b && w.rsp == 0x1000 && w.rbp == 0x7 && w.rbx == 4);
        stack[4] = 0x5; /* a return address below the image */
        assert(heg_classify(&w, stack, kImage) == HEG_NONE);
        /* Its prologue (the pushes not done yet) and its epilogue are not the body. */
        stack[4] = kImage + heg_walk_calls[0] + 5;
        w.rip = HEG_WALK;
        assert(heg_classify(&w, stack, kImage) == HEG_NONE);
        w.rip = HEG_WALK_BODY_TO;
        assert(heg_classify(&w, stack, kImage) == HEG_NONE);
        w.rip = HEG_WALK_BODY_FROM;
        assert(heg_classify(&w, stack, kImage) == HEG_IN_WALK);
        assert(heg_classify(&w, NULL, kImage) == HEG_NONE);
    }

    /* 5. Elsewhere in the game: nothing. */
    heg_regs other = {0x28ce9b5, 0x1000, 0x2000, 0, 0, 0};
    assert(heg_recover(&other, NULL, kImage) == HEG_NONE && other.rip == 0x28ce9b5);

    printf("havok-event-guard-test: OK\n");
    return 0;
}
