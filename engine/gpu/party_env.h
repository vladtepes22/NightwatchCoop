/* SPDX-License-Identifier: GPL-3.0-or-later
 * The one rule for "is party mode on" (BB_PARTY), shared by the C runtime (src/) and the party
 * library (gpu/shim/): unset, empty (or blanks only), "0", "off", "false", "no" (any case,
 * surrounding blanks ignored) = off; any other value (host, join, guest, 1, a party code) = on.
 * scripts/party_env.py is the same rule for the Python side (patches.py, run settings) and run.bat
 * clears an off value before anything reads it. tests: party-env-test, tests/test_party_env.py. */
#ifndef BB_PARTY_ENV_H
#define BB_PARTY_ENV_H

#include <stdlib.h>

/* 1 when `value` (BB_PARTY or another party on/off switch) means on. */
static inline int bb_party_value_on(const char *value) {
    static const char *const off[] = {"0", "off", "false", "no"};
    const char *b = value, *e;
    unsigned i;
    if (!value) return 0;
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') ++b;
    e = b;
    while (*e) ++e;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    if (e == b) return 0;
    for (i = 0; i < sizeof(off) / sizeof(off[0]); ++i) {
        const char *w = off[i], *p = b;
        while (p < e && *w) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != *w) break;
            ++p;
            ++w;
        }
        if (p == e && !*w) return 0;
    }
    return 1;
}

/* 1 when BB_PARTY turns party mode on for this process. */
static inline int bb_party_enabled(void) { return bb_party_value_on(getenv("BB_PARTY")); }

/* BB_PARTY when it turns party mode on, else NULL (an off value reads as unset). */
static inline const char *bb_party_value(void) {
    const char *v = getenv("BB_PARTY");
    return bb_party_value_on(v) ? v : (const char *)0;
}

/* A party switch that is on by default (BB_PARTY_FAST_JOIN, ...): unset or blank = on, otherwise
 * the same off words as above. */
static inline int bb_party_switch_on(const char *name) {
    const char *v = getenv(name), *p = v;
    if (!v) return 1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return *p ? bb_party_value_on(v) : 1;
}

#endif
