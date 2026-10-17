// SPDX-License-Identifier: GPL-3.0-or-later
// The shared BB_PARTY rule (gpu/party_env.h): unset, empty, 0, off, false, no = off.
// Build and run: ninja -C out/gpu party-env-test && out/gpu/party-env-test.exe
#include "../gpu/party_env.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

static void SetParty(const char* v) {
#ifdef _WIN32
    std::string s = std::string("BB_PARTY=") + (v ? v : "");
    _putenv(s.c_str()); // "BB_PARTY=" removes it
#else
    if (v) setenv("BB_PARTY", v, 1);
    else unsetenv("BB_PARTY");
#endif
}

int main() {
    for (const char* off : {(const char*)nullptr, "", " ", "\t \r\n", "0", " 0 ", "off", "OFF", "Off", "false", "FALSE",
                            "no", "No", " no\n"}) {
        CHECK(!bb_party_value_on(off));
    }
    for (const char* on : {"host", "join", "guest", "1", "HOST", " join ", "BBP1-ABCDEF", "127.0.0.1:47000", "yes",
                           "on", "true", "00", "nope", "offf", "of", "n", "o ff"}) {
        CHECK(bb_party_value_on(on));
    }
    SetParty(nullptr);
    CHECK(!bb_party_enabled() && bb_party_value() == nullptr);
    SetParty("off");
    CHECK(!bb_party_enabled() && bb_party_value() == nullptr);
    SetParty("0");
    CHECK(!bb_party_enabled());
    SetParty("host");
    CHECK(bb_party_enabled() && bb_party_value() && std::string(bb_party_value()) == "host");
    // Default-on switches (BB_PARTY_FAST_JOIN, ...): unset / blank = on, off words = off.
#ifdef _WIN32
    _putenv("BB_PARTY_FAST_JOIN=");
    CHECK(bb_party_switch_on("BB_PARTY_FAST_JOIN"));
    for (const char* v : {"BB_PARTY_FAST_JOIN=0", "BB_PARTY_FAST_JOIN=off", "BB_PARTY_FAST_JOIN= No "}) {
        _putenv(v);
        CHECK(!bb_party_switch_on("BB_PARTY_FAST_JOIN"));
    }
    for (const char* v : {"BB_PARTY_FAST_JOIN=1", "BB_PARTY_FAST_JOIN=on", "BB_PARTY_FAST_JOIN= "}) {
        _putenv(v);
        CHECK(bb_party_switch_on("BB_PARTY_FAST_JOIN"));
    }
    _putenv("BB_PARTY_FAST_JOIN=");
#endif
    std::printf("party-env-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
