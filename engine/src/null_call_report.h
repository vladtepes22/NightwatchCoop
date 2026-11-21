/* A call through a null (or wild) function pointer: RIP is the bad target, so no stack walk can start and a crash
 * report named only the crash handler's own frames (NW-bbc8683: "executing address 0000000000000000", host #0 in
 * bb-probe). A `call` pushed its return address first, so at the fault [rsp] is the return address into the caller
 * (for a `jmp` tail call it is the caller's caller). probe.c reads a copy of the top stack words and prints [rsp]
 * plus the older words that point into the game image just after a call instruction (candidates for the callers).
 *
 * Header-only C, no Windows calls: probe.c gives it the stack copy and the code bytes; tests/test_null_call_report.c
 * tests it on a fake stack and a synthetic image. */
#ifndef BB_NULL_CALL_REPORT_H
#define BB_NULL_CALL_REPORT_H

#include <stddef.h>
#include <stdint.h>

enum {
    NCR_LOW_LIMIT = 0x10000, /* below 64 KiB nothing is ever mapped on Windows */
    NCR_STACK_WORDS = 32,    /* how far up the stack the candidate scan looks */
    NCR_MAX_CANDIDATES = 4,
    NCR_CALL_BYTES = 7,      /* code bytes before a return address that ncr_follows_call looks at */
};

/* Report it: an execution fault whose target is where RIP is, at a tiny address or outside every loaded module. */
static inline int ncr_wanted(int executing, uint64_t rip, uint64_t target, int target_in_module) {
    return executing && rip == target && (target < NCR_LOW_LIMIT || !target_in_module);
}

/* The NCR_CALL_BYTES bytes just before `ret` end in a call: E8 rel32, or FF /2 (call r/m64, with or without a REX
 * prefix) of 2, 3, 4, 6 or 7 bytes. Loose on purpose (it only ranks stack words as candidates). */
static inline int ncr_follows_call(const uint8_t before[NCR_CALL_BYTES]) {
    const uint8_t *end = before + NCR_CALL_BYTES;
    if (end[-5] == 0xe8) return 1;
    static const int lengths[] = {2, 3, 4, 6, 7};
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        const uint8_t *at = end - lengths[i];
        if (at[0] != 0xff || ((at[1] >> 3) & 7) != 2) continue;
        const unsigned mod = at[1] >> 6, rm = at[1] & 7;
        int length = 2 + (mod != 3 && rm == 4) + (mod == 1 ? 1 : mod == 2 ? 4 : mod == 0 && rm == 5 ? 4 : 0);
        if (mod == 0 && rm == 4 && lengths[i] > 2 && (at[2] & 7) == 5) length += 4; /* SIB with no base: disp32 */
        if (length == lengths[i]) return 1;
    }
    return 0;
}

/* Words 1..count-1 of the stack copy that lie in [base, base + size) and, when `code` is given, follow a call
 * (`code(address, bytes)` fills the NCR_CALL_BYTES before address, returns 0 if unreadable). Duplicates and copies of
 * words[0] are skipped. Returns how many were written to out (at most max). */
typedef int (*ncr_read_code)(uint64_t address, uint8_t bytes[NCR_CALL_BYTES]);
static inline size_t ncr_candidates(const uint64_t *words, size_t count, uint64_t base, uint64_t size,
                                    ncr_read_code code, uint64_t *out, size_t max) {
    size_t found = 0;
    for (size_t i = 1; i < count && found < max; ++i) {
        const uint64_t w = words[i];
        if (w - base >= size || w - base < NCR_CALL_BYTES || w == words[0]) continue;
        int seen = 0;
        for (size_t j = 0; j < found && !seen; ++j) seen = out[j] == w;
        if (seen) continue;
        uint8_t bytes[NCR_CALL_BYTES];
        if (code && !(code(w, bytes) && ncr_follows_call(bytes))) continue;
        out[found++] = w;
    }
    return found;
}

#endif
