// SPDX-License-Identifier: GPL-3.0-or-later
// Nightwatch 0.3.0 feature sounds: start/features.h (the json, merge, out file) and gpu/shim/party/sounds_rules.h
// (the engine's reading of that file, the event key and the gains).
// Build and run: ninja -C out/gpu sounds-test && out/gpu/sounds-test.exe
#include "start/features.h"
#include "gpu/shim/party/sounds_rules.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace nwfeatures;
namespace snd = coop::sounds;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
        }                                                                                                              \
    } while (0)

static void Json() {
    std::vector<SoundRow> rows;
    std::string error;
    CHECK(ParseSounds(R"({"version":1,"rows":[{"event":"s000252531","file":"sound\\xbow.wav","volume":0.5,"replace":true},
                                               {"event":"c000000911","file":"a/b.wav"}]})",
                      &rows, &error));
    CHECK(rows.size() == 2 && rows[0].file == "sound/xbow.wav" && rows[0].volume == 0.5 && rows[0].replace);
    CHECK(rows[1].volume == 1.0 && !rows[1].replace);
    CHECK(ParseSounds(R"({"version":1,"rows":[]})", &rows, &error) && rows.empty());
    CHECK(!ParseSounds(R"({"version":2,"rows":[]})", &rows, &error));
    const char* bad[] = {
        R"({"version":1,"rows":[{"event":"s00025253","file":"a.wav"}]})",          // 8 digits
        R"({"version":1,"rows":[{"event":"q000252531","file":"a.wav"}]})",         // not a type letter
        R"({"version":1,"rows":[{"event":"s000252531","file":"../a.wav"}]})",      // leaves the mod
        R"({"version":1,"rows":[{"event":"s000252531","file":"C:/a.wav"}]})",      // a drive
        R"({"version":1,"rows":[{"event":"s000252531","file":"/a.wav"}]})",        // absolute
        R"({"version":1,"rows":[{"event":"s000252531","file":"a//b.wav"}]})",      // empty part
        R"({"version":1,"rows":[{"event":"s000252531","file":"a.wav","volume":5}]})",
        R"({"version":1,"rows":[{"event":"s000252531","file":"a.wav","replace":1}]})",
        R"({"version":1,"rows":[{"event":"s000252531","file":"a.wav"},{"event":"s000252531","file":"b.wav"}]})",
    };
    for (const char* b : bad) {
        CHECK(!ParseSounds(b, &rows, &error) && rows.empty() && !error.empty());
    }
    Manifest m;
    CHECK(ParseManifest(R"({"id":"xbow","requires":"0.3.0","features":["sounds"]})", &m, &error) && m.Uses(kSounds));
    CHECK(Refusal(m, "0.3.0-beta.2").empty());
}

static void MergeAndFile() {
    SoundMerged merged;
    AddSounds(&merged, "a", {{"s000000002", "C:/m/a/two.wav", 1.0, false}, {"s000000001", "C:/m/a/one.wav", 0.25, true}});
    AddSounds(&merged, "b", {{"s000000002", "C:/m/b two/2.wav", 2.0, false}});
    CHECK(merged.rows.size() == 2 && merged.rows[1].file == "C:/m/b two/2.wav");
    const std::string text = SoundsFileText(merged);
    std::vector<snd::Row> rows;
    std::size_t bad = 0;
    CHECK(snd::ParseTable(text, &rows, &bad));
    CHECK(rows.size() == 2);
    std::uint64_t key = 0;
    CHECK(snd::EventKey("s000000001", 0x100, &key) && key == ((5ull << 32) | 1));
    const snd::Row* one = snd::Find(rows, key);
    CHECK(one && one->replace && std::fabs(one->volume - 0.25f) < 1e-6f && one->file == "C:/m/a/one.wav");
    CHECK(snd::EventKey("s000000002", 0x100, &key) && snd::Find(rows, key)->file == "C:/m/b two/2.wav");
    CHECK(SoundsFileText(SoundMerged{}).empty());
    CHECK(!snd::ParseTable("s000000001 1.000 2 x.wav\n", &rows, &bad) && bad == 1 && rows.empty());
    CHECK(!snd::ParseTable("# c\ns00000001 1.000 0 x.wav\n", &rows, &bad) && bad == 2);
    CHECK(!snd::ParseTable("s000000001 9 0 x.wav\n", &rows, &bad));
}

static void Keys() {
    std::uint64_t key = 0;
    char buf[0x100] = "z000000911";
    CHECK(snd::EventKey(buf, sizeof buf, &key) && key == ((12ull << 32) | 911));
    CHECK(!snd::EventKey("s0002525310", 0x100, &key));  // 10 digits
    CHECK(!snd::EventKey("s00025253", 0x100, &key));    // 8 digits, then NUL
    CHECK(!snd::EventKey(nullptr, 0x100, &key));
}

static void Gains() {
    // Camera at the origin looking down +z (identity view), screen right = +x.
    const float view[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    float l = 0, r = 0;
    const float ahead[3] = {0, 0, 2};
    CHECK(snd::Gains(true, view, 1.0f, ahead, 1.0f, &l, &r) && std::fabs(l - r) < 1e-5f && std::fabs(l - 0.7071f) < 1e-3f);
    const float right[3] = {6, 0, 0};
    CHECK(snd::Gains(true, view, 1.0f, right, 1.0f, &l, &r) && l < 1e-5f && std::fabs(r - 0.5f) < 1e-4f);  // 3/6 at 6 m
    CHECK(snd::Gains(true, view, -1.0f, right, 1.0f, &l, &r) && r < 1e-5f && l > 0.49f);  // mirrored projection
    const float far[3] = {0, 0, 61};
    CHECK(!snd::Gains(true, view, 1.0f, far, 1.0f, &l, &r));
    CHECK(snd::Gains(false, view, 1.0f, far, 2.0f, &l, &r) && std::fabs(l - r) < 1e-5f && l > 1.4f);  // no camera: centred
    // Camera moved to x = 10 (view translation -10): a sound at x = 10, z = 3 is straight ahead.
    const float moved[12] = {1, 0, 0, -10, 0, 1, 0, 0, 0, 0, 1, 0};
    const float there[3] = {10, 0, 3};
    CHECK(snd::Gains(true, moved, 1.0f, there, 1.0f, &l, &r) && std::fabs(l - r) < 1e-5f);
    CHECK(!snd::Gains(true, view, 1.0f, ahead, 0.0f, &l, &r));
}

static void Sfx() {
    CHECK(snd::SfxGain(1.0f, 1.0f) == 1.0f);
    CHECK(std::fabs(snd::SfxGain(0.5f, 0.8f) - 0.4f) < 1e-6f);  // option 5/10, master fader 0.8
    CHECK(snd::SfxGain(0.0f, 1.0f) == 0.0f);                    // option 0: silent, like the game's sounds
    CHECK(snd::SfxGain(3.0f, -1.0f) == 0.0f);                   // clamped
    CHECK(snd::SfxGain(std::nanf(""), 0.3f) == 0.3f);           // unreadable: 1
}

int main() {
    Json();
    MergeAndFile();
    Keys();
    Gains();
    Sfx();
    std::printf("sounds-test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
