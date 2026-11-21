/* src/null_call_report.h: the callers of a call through a null function pointer (NW-bbc8683), from a fake stack
 * and a synthetic image. Built as out/gpu/null-call-report-test. */
#include "null_call_report.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t kBase = 0x800000000ULL, kSize = 0x10000;
static uint8_t image[0x10000];

static int read_code(uint64_t address, uint8_t bytes[NCR_CALL_BYTES]) {
    if (address - kBase < NCR_CALL_BYTES || address - kBase > kSize) return 0;
    memcpy(bytes, image + (address - kBase) - NCR_CALL_BYTES, NCR_CALL_BYTES);
    return 1;
}
/* Puts the call instruction `code` so that it ends at image offset `ret`; returns the guest address of ret. */
static uint64_t put_call(uint64_t ret, const uint8_t *code, size_t length) {
    memcpy(image + ret - length, code, length);
    return kBase + ret;
}

int main(void) {
    /* 1. When to report: only an execution fault at RIP, tiny or outside every module. */
    assert(ncr_wanted(1, 0, 0, 0));
    assert(ncr_wanted(1, 0x1234, 0x1234, 1));           /* tiny: even if something claims a module */
    assert(ncr_wanted(1, 0x7ff600000000, 0x7ff600000000, 0)); /* wild pointer into nothing */
    assert(!ncr_wanted(1, 0x7ff600000000, 0x7ff600000000, 1)); /* NX page in a module: the stack walk works */
    assert(!ncr_wanted(0, 0, 0, 0));                    /* a read of address 0 */
    assert(!ncr_wanted(1, 0x401000, 0, 0));             /* RIP is elsewhere */

    /* 2. Call forms the scan recognises, and data that only looks close. */
    static const uint8_t rel32[] = {0xe8, 1, 2, 3, 4};
    static const uint8_t reg[] = {0xff, 0xd0};               /* call rax */
    static const uint8_t rex_reg[] = {0x41, 0xff, 0xd3};     /* call r11 */
    static const uint8_t disp8[] = {0xff, 0x50, 0x18};       /* call [rax+0x18] */
    static const uint8_t sib_disp8[] = {0xff, 0x54, 0x24, 0x08}; /* call [rsp+8] */
    static const uint8_t rip_rel[] = {0xff, 0x15, 1, 2, 3, 4};  /* call [rip+x] */
    static const uint8_t disp32[] = {0xff, 0x90, 0, 1, 0, 0};   /* call [rax+0x100] */
    static const uint8_t sib_disp32[] = {0xff, 0x94, 0xc8, 0, 1, 0, 0}; /* call [rax+rcx*8+0x100] */
    const uint64_t r_rel32 = put_call(0x1005, rel32, sizeof rel32), r_reg = put_call(0x2002, reg, sizeof reg),
                   r_rex = put_call(0x3003, rex_reg, sizeof rex_reg), r_d8 = put_call(0x4003, disp8, sizeof disp8),
                   r_sib8 = put_call(0x5004, sib_disp8, sizeof sib_disp8),
                   r_rip = put_call(0x6006, rip_rel, sizeof rip_rel), r_d32 = put_call(0x7006, disp32, sizeof disp32),
                   r_sib32 = put_call(0x8007, sib_disp32, sizeof sib_disp32);
    const uint64_t all[] = {r_rel32, r_reg, r_rex, r_d8, r_sib8, r_rip, r_d32, r_sib32};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
        uint8_t bytes[NCR_CALL_BYTES];
        assert(read_code(all[i], bytes));
        if (!ncr_follows_call(bytes)) { fprintf(stderr, "call form %zu not recognised\n", i); return 1; }
    }
    uint8_t bytes[NCR_CALL_BYTES];
    assert(read_code(kBase + 0x9100, bytes) && !ncr_follows_call(bytes)); /* zeros */
    static const uint8_t jmp_reg[] = {0xff, 0xe0};             /* jmp rax: FF /4, not a call */
    assert(read_code(put_call(0x9202, jmp_reg, sizeof jmp_reg), bytes) && !ncr_follows_call(bytes));

    /* 3. The scan over a fake stack: [rsp] is the return address; later words in the image after a call, in stack
     * order, without duplicates, data pointers into the image, host addresses or small numbers. */
    const uint64_t stack[] = {
        r_rel32,            /* [rsp]: reported on its own, skipped by the scan */
        0x20,               /* a small integer */
        kBase + 0x9100,     /* a pointer to image data (no call before it) */
        r_reg,
        0x7ffa12345678ULL,  /* a host DLL address */
        r_reg,              /* duplicate */
        r_rel32,            /* the [rsp] word again */
        r_d8,
        kBase + kSize + 8,  /* just past the image */
        kBase + 3,          /* too close to the start to have a call before it */
        r_rip,
        r_sib32,
        r_d32,              /* beyond the limit of 4 */
    };
    const size_t count = sizeof stack / sizeof stack[0];
    uint64_t out[NCR_MAX_CANDIDATES];
    size_t n = ncr_candidates(stack, count, kBase, kSize, read_code, out, NCR_MAX_CANDIDATES);
    assert(n == 4);
    assert(out[0] == r_reg && out[1] == r_d8 && out[2] == r_rip && out[3] == r_sib32);
    /* Without code bytes every in-image word counts (data pointer included). */
    n = ncr_candidates(stack, count, kBase, kSize, NULL, out, NCR_MAX_CANDIDATES);
    assert(n == 4 && out[0] == kBase + 0x9100 && out[1] == r_reg && out[2] == r_d8 && out[3] == r_rip);
    /* An unreadable code page drops the word; an empty or one-word stack gives nothing. */
    assert(ncr_candidates(stack, 1, kBase, kSize, read_code, out, NCR_MAX_CANDIDATES) == 0);
    assert(ncr_candidates(stack, 0, kBase, kSize, read_code, out, NCR_MAX_CANDIDATES) == 0);

    puts("null-call-report-test: ok");
    return 0;
}
