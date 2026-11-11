/* Havok behaviour event of a freed animation: skip the event instead of crashing (crashfix081,
 * CL-078659; analysis in research/restab081.md section 1).
 *
 * A Havok worker (HavokWorkerThread) runs a chr's behaviour graph; an animation event goes to
 * 0x1a27fb0, which finds the animation object in the chr's behaviour module table and calls
 * 0x1a21100(rdi = listener, rsi = animation object) to play its annotations. When the chr's
 * animation data was released while the worker still ran its graph (a dying boss part on a guest),
 * the object's +0x18 holds garbage: 0x1a21100 faulted at +0x1a21134 reading address 1.
 *
 * The guard runs in the crash handler only (no cost while playing): a fault inside 0x1a21100's
 * body, or inside the annotation walker 0xf5bc30 called from it, returns from 0x1a21100 as if the
 * animation had no annotations (its own early-out at +0x1a2139c). The caller then records the event
 * in the module's ring as it always does. BB_HAVOK_EVENT_GUARD=0 turns it off (crash as before).
 *
 * Guest 1.09 offsets (raw ELF VA). Every layout fact below was checked against the disassembly:
 *   0x1a21100: push rbp; mov rbp,rsp; push r15,r14,r12,rbx; sub rsp,0xe0  -> body rsp = rbp-0x100
 *              r12 (stack-guard pointer) is set at +0x1a21118, before the first possible fault
 *              (+0x1a21127 reads the object). +0x1a2139c: the guard check, add rsp,0xe0, pops, ret.
 *   0xf5bc30:  push rbp,r15,r14,rbx (no frame pointer, no other rsp change); body
 *              +0xf5bc36..+0xf5bdcc; it never touches r12/r13. Called from 0x1a21100 at the six
 *              sites below (return addresses = site + 5).
 * Header-only C: probe.c uses it, tests/test_havok_event_guard.c tests it on a fake stack. */
#ifndef BB_HAVOK_EVENT_GUARD_H
#define BB_HAVOK_EVENT_GUARD_H

#include <stdint.h>
#include <string.h>

enum {
    HEG_PLAY = 0x1a21100,      /* annotation playback */
    HEG_BODY_FROM = 0x1a21127, /* first read of the animation object */
    HEG_EXIT = 0x1a2139c,      /* its early-out: stack guard check and epilogue */
    HEG_WALK = 0xf5bc30,       /* annotation walker */
    HEG_WALK_BODY_FROM = 0xf5bc36,
    HEG_WALK_BODY_TO = 0xf5bdcc,
};

/* The call sites of the walker inside 0x1a21100 (each `call 0xf5bc30`). */
static const uint32_t heg_walk_calls[6] = {0x1a211f0, 0x1a21232, 0x1a212bb,
                                           0x1a212f2, 0x1a21356, 0x1a21392};

/* Bytes the guard relies on, checked once against the loaded image (another game version or a
 * patch there: no guard). */
typedef struct {
    uint32_t off;
    uint8_t len;
    uint8_t bytes[12];
} heg_check;
static const heg_check heg_checks[] = {
    /* push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; sub rsp,0xe0 */
    {0x1a21100, 12, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48}},
    /* mov r12,[rip+..] at +0x1a21118 (the stack-guard pointer, before the object is read) */
    {0x1a21118, 3, {0x4c, 0x8b, 0x25}},
    /* mov rax,[r14+0x18]; test rax,rax; je; cmp dword [rax],0 (the crash) */
    {0x1a21127, 10, {0x49, 0x8b, 0x46, 0x18, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x68}},
    /* mov rax,[r12]; cmp rax,[rbp-0x28]; jne; add rsp,0xe0; pop rbx */
    {0x1a2139c, 12, {0x49, 0x8b, 0x04, 0x24, 0x48, 0x3b, 0x45, 0xd8, 0x75, 0x10, 0x48, 0x81}},
    /* push rbp; push r15; push r14; push rbx; xor eax,eax */
    {0xf5bc30, 8, {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x31, 0xc0}},
    /* pop rbx; pop r14; pop r15; pop rbp; ret */
    {0xf5bdcc, 7, {0x5b, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3}},
};

/* 1: the image holds the code the guard was written for. */
static inline int heg_image_matches(const uint8_t *image, uint64_t image_size) {
    if (!image) return 0;
    for (size_t i = 0; i < sizeof heg_checks / sizeof heg_checks[0]; ++i) {
        const heg_check *c = &heg_checks[i];
        if (c->off + c->len > image_size || memcmp(image + c->off, c->bytes, c->len) != 0) return 0;
    }
    for (size_t i = 0; i < 6; ++i) {
        const uint32_t at = heg_walk_calls[i];
        int32_t rel;
        if (at + 5 > image_size || image[at] != 0xe8) return 0;
        memcpy(&rel, image + at + 1, 4);
        if ((int64_t)at + 5 + rel != HEG_WALK) return 0;
    }
    return 1;
}

/* The registers the recovery reads and changes. rip is a guest image offset in and out. */
typedef struct {
    uint64_t rip, rsp, rbp, rbx, r14, r15;
} heg_regs;

enum { HEG_NONE = 0, HEG_IN_PLAY = 1, HEG_IN_WALK = 2 };

static inline int heg_is_walk_return(uint64_t ret_off) {
    for (size_t i = 0; i < 6; ++i)
        if (ret_off == heg_walk_calls[i] + 5u) return 1;
    return 0;
}

/* Where a fault at `r->rip` is (stack = host view of the guest stack at r->rsp, 5 qwords readable
 * when rip is in the walker's body; image_base: guest address of offset 0, to read the walker's
 * return address as an offset). */
static inline int heg_classify(const heg_regs *r, const uint64_t *stack, uint64_t image_base) {
    if (r->rip >= HEG_BODY_FROM && r->rip < HEG_EXIT) return HEG_IN_PLAY;
    if (r->rip >= HEG_WALK_BODY_FROM && r->rip < HEG_WALK_BODY_TO && stack &&
        stack[4] >= image_base && heg_is_walk_return(stack[4] - image_base))
        return HEG_IN_WALK;
    return HEG_NONE;
}

/* The registers after the recovery: 0x1a21100 resumes at its early-out with its own frame.
 * Returns the classification (HEG_NONE: nothing changed). */
static inline int heg_recover(heg_regs *r, const uint64_t *stack, uint64_t image_base) {
    const int where = heg_classify(r, stack, image_base);
    if (where == HEG_IN_WALK) {
        /* The walker's pushes, in pop order: rbx, r14, r15, rbp; then the return address. */
        r->rbx = stack[0];
        r->r14 = stack[1];
        r->r15 = stack[2];
        r->rbp = stack[3];
    }
    if (where != HEG_NONE) {
        r->rsp = r->rbp - 0x100; /* 0x1a21100's body stack: 4 pushes after rbp, then 0xe0 */
        r->rip = HEG_EXIT;
    }
    return where;
}

#endif
