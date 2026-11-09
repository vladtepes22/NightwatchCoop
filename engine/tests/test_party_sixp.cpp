// SPDX-License-Identifier: GPL-3.0-or-later
// Six players (gpu/shim/party/party_sixp.h), the parts without the game: the switch, the site tables.
// Build and run: ninja -C out/gpu party-sixp-test && out/gpu/party-sixp-test.exe
#include "party/party_sixp.h"

#include <cstdio>
#include <cstdlib>

using namespace coop::sixp;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static void set_env(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

int main() {
    // off by default; both conditions are needed
    set_env("BB_PARTY_LARGE", nullptr);
    set_env("BB_PARTY_MAX", nullptr);
    CHECK(!EnabledFromEnv());
    set_env("BB_PARTY_MAX", "6");
    CHECK(!EnabledFromEnv());
    set_env("BB_PARTY_LARGE", "1");  // 5 seats: nothing patched
    CHECK(!EnabledFromEnv());
    set_env("BB_PARTY_LARGE", "2");
    CHECK(EnabledFromEnv());
    set_env("BB_PARTY_MAX", "5");
    CHECK(!EnabledFromEnv());
    set_env("BB_PARTY_MAX", "6");
    set_env("BB_PARTY_LARGE", "22");
    CHECK(!EnabledFromEnv());

    // the site tables
    std::string why;
    CHECK(SitesSane(AllSites(), &why));
    CHECK(!AllSites().empty() && AllSites().size() >= Stage1Sites().size());
    for (const Site& s : AllSites()) {
        CHECK(s.from.size() == s.to.size() && !s.from.empty() && s.from != s.to);
        CHECK(s.what && s.what[0]);
    }
    // the checker catches the mistakes it exists for
    std::vector<Site> bad = Stage1Sites();
    bad.push_back({bad[0].off + 1, {0x90, 0x90}, {0x91, 0x91}, "overlap"});
    CHECK(!SitesSane(bad, &why) && why.find("overlap") == 0);
    bad = Stage1Sites();
    bad.push_back({0x1000, {0x90}, {0x90}, "same"});
    CHECK(!SitesSane(bad));
    bad = Stage1Sites();
    bad.push_back({0x1000, {0x90}, {0x91, 0x92}, "length"});
    CHECK(!SitesSane(bad));
    // stage 1 holds the ChrSet's three immediates: 0x118 -> 0x150 twice, 5 -> 6 once
    const std::vector<Site> s1 = Stage1Sites();
    CHECK(s1.size() == 3 && s1[0].to[1] == 0x50 && s1[2].to[1] == 0x50 && s1[1].to[3] == 6 && s1[1].from[3] == 5);
    // stage 2: the GameDataMan rows. Every compare site is `cmp reg, imm8` (83 /7 ib, optional REX) with the immediate 3 -> 4 or 4 -> 5.
    const std::vector<Site> s2 = Stage2Sites();
    CHECK(s2.size() >= 70 && AllSites(1).size() == s1.size() && AllSites(2).size() == s1.size() + s2.size());
    int cmps = 0, imm32 = 0;
    for (const Site& s : s2) {
        const std::size_t n = s.from.size();
        if (n == 3 || n == 4) {
            const std::size_t k = n - 3;
            CHECK(k == 0 || (s.from[0] >= 0x40 && s.from[0] < 0x50));
            CHECK(s.from[k] == 0x83 && ((s.from[k + 1] >> 3) & 7) == 7);
            const int a = s.from[n - 1], b = s.to[n - 1];
            CHECK((a == 3 || a == 4) && b == a + 1);
            for (std::size_t i = 0; i + 1 < n; ++i) CHECK(s.from[i] == s.to[i]);
            ++cmps;
        } else {
            ++imm32;
        }
    }
    CHECK(cmps >= 65 && imm32 == 4);
    set_env("BB_PARTY_SIX_STAGE", nullptr);
    CHECK(StageFromEnv() == kDefaultStage);
    set_env("BB_PARTY_SIX_STAGE", "1");
    CHECK(StageFromEnv() == 1);
    set_env("BB_PARTY_SIX_STAGE", "0");
    CHECK(StageFromEnv() == kDefaultStage);
    // stage 3: the presentation object. The tracked-handle moves add 0x30 to a disp32 of 0x80..0x90; the bounds add one slot.
    const std::vector<Site> s3 = Stage3Sites();
    CHECK(s3.size() >= 60 && AllSites(3).size() == s1.size() + s2.size() + s3.size() && AllSites().size() == AllSites(3).size());
    int moves = 0, bounds = 0, allocs = 0;
    for (const Site& s : s3) {
        CHECK(s.from.size() == s.to.size() && s.from != s.to);
        std::size_t diff = 0, at = 0;
        for (std::size_t i = 0; i < s.from.size(); ++i) {
            if (s.from[i] != s.to[i]) {
                ++diff;
                at = i;
            }
        }
        if (s.off == 0x156783e) {
            ++allocs;
            CHECK(s.from[1] == 0xb0 && s.to[1] == 0xc8);
        } else if (std::string(s.what).find("tracked handles +0x30") != std::string::npos) {
            ++moves;
            CHECK(diff == 1 && s.to[at] == s.from[at] + 0x30 && s.from[at] >= 0x80 && s.from[at] <= 0x90);
        } else {
            ++bounds;
            CHECK(diff >= 1 && diff <= 1);  // one byte: the immediate
            const int a = s.from[at], b = s.to[at];
            CHECK((a == 4 || a == 5 || a == 0x64 || a == 0x80) && b == (a == 0x64 ? 0x78 : a == 0x80 ? 0x94 : a + 1));
        }
    }
    CHECK(allocs == 1 && moves == 45 && bounds >= 20);
    CHECK(std::string(kRulesTag) == "6p:v1");
    std::printf("party-sixp-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
