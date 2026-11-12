// SPDX-License-Identifier: GPL-3.0-or-later
// sounds: the one owner of the positioned-sound hook (docs/modding/sounds.md). Offsets are ours (bbhost - 0x400000),
// 1.09 eboot.
//
// The game (1.09, disassembly; bb-modding research/sound-request-path-2026-11-10.md and -2-):
// - 0x27D3A80 PlayAt(out, name, pos, a3, a4, a5, a6 on the stack) makes an FD4 sound event from an ASCII name
//   ("s000252531": a letter of "acfopsmvxbgyz" and 9 digits) at world pos (float[3]) and starts it. Every positioned
//   sound reaches it: TAE SoundIns, the hit overlay, effect (FXR) sounds, the chr helpers (16 call sites). 2D sounds
//   (menus) use other functions and are not covered.
// - With the sound system off it writes the invalid handle {[0x56C7E94], [0x56C7E98]} to `out` and returns `out`;
//   Start on that handle does nothing. A name in no bank gets the same handle. Each request comes once: an FXR sound
//   node calls Start once whatever it returned, a SoundIns with an invalid handle is freed in the same tick.
//
// So: a prologue replacement on 0x27D3A80. A name in the table queues the mod's sound (runtime_audio.c mixes it into
// the main port, panned and attenuated from the camera at that moment); "replace" then returns the invalid handle
// without the game's call, else the game's function runs as before. The mixer scales the clips by the game's own
// sound-effect level (Level below, sounds_rules.h SfxGain), read per buffer so a playing clip follows the slider.
#include "sounds.h"
#include "sounds_rules.h"

#include "../bbport_camera.h"
#include "coop_hooks.h"
#include "game_state.h"
#include "party_async_io.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" void runtime_audio_play(const float* mono48k, int frames, float left, float right);
extern "C" void runtime_audio_set_mod_level(float (*level)(void));

namespace coop::sounds {
namespace {

constexpr u64 kPlayAt = 0x27d3a80;
constexpr u64 kInvalidId = 0x56c7e94, kInvalidGen = 0x56c7e98;
constexpr int kMaxSeconds = 30;  // longer files are cut

std::vector<Row> g_rows;                 // set once before the game runs, read-only after
std::vector<std::vector<float>> g_pcm;   // g_pcm[i]: g_rows[i]'s sound, 48 kHz mono float
using PlayAtFn = u64*(BB_COOP_SYSV*)(u64*, const char*, const float*, u64, u64, u64, u64);
PlayAtFn g_play = nullptr;
bool g_verbose = false;
std::atomic<int> g_logged{0};

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("sounds: ") + line + "\n");
}

/// The game's sound-effect level (sounds_rules.h SfxGain), on the audio thread once per mixed buffer while a mod
/// sound plays: the levels the game last sent to FMOD for its SE category and its master. No manager yet: 1.
float Level() {
    u64 manager = 0;
    if (!SafeGet(Guest(kVolumeManager), &manager) || !manager) return 1.0f;
    float se = std::nanf(""), master = std::nanf("");
    SafeGet(manager + kSeSent, &se);
    SafeGet(manager + kMasterSent, &master);
    return SfxGain(se, master);
}

BB_COOP_SYSV u64* PlayAt(u64* out, const char* name, const float* pos, u64 a3, u64 a4, u64 a5, u64 a6) {
    std::uint64_t key = 0;
    const Row* row = nullptr;
    const bool event = EventKey(name, 0x100, &key);
    if (!g_rows.empty() && event) row = Find(g_rows, key);
    if (!row) {
        static std::atomic<int> asked{0};
        if (g_verbose && event && asked.fetch_add(1) < 50) {
            Log("the game asks for %.10s at (%.1f, %.1f, %.1f)", name, pos ? pos[0] : 0.0f, pos ? pos[1] : 0.0f,
                pos ? pos[2] : 0.0f);
        }
        return g_play(out, name, pos, a3, a4, a5, a6);
    }
    const std::vector<float>& pcm = g_pcm[std::size_t(row - g_rows.data())];
    const BbCamera::State cam = BbCamera::Latest(500);
    float left = 0.0f, right = 0.0f;
    const bool heard = pos && Gains(cam.valid, cam.view.data(), cam.proj[0], pos, row->volume, &left, &right);
    if (heard) runtime_audio_play(pcm.data(), int(pcm.size()), left, right);
    if (g_logged.fetch_add(1) < (g_verbose ? 400 : 8)) {
        Log("%.10s at (%.1f, %.1f, %.1f): %s (gains %.2f / %.2f%s, game SFX level %.2f)%s", name, pos ? pos[0] : 0.0f,
            pos ? pos[1] : 0.0f, pos ? pos[2] : 0.0f, heard ? "played" : "too far", left, right,
            cam.valid ? "" : ", no camera: centred", double(Level()), row->replace ? ", game's sound replaced" : "");
    }
    if (!row->replace) return g_play(out, name, pos, a3, a4, a5, a6);
    std::uint32_t id = 0, gen = 0;
    SafeGet(Guest(kInvalidId), &id);
    SafeGet(Guest(kInvalidGen), &gen);
    *out = (u64(gen) << 32) | id;
    return out;
}

std::string TablePath() {
    const char* o = std::getenv("BB_OUT_DIR");
    const char* d = std::getenv("BB_DATA_DIR");
    return (o && o[0] ? std::string(o) : std::string(d && d[0] ? d : ".") + "/out") + "/nightwatch_sounds.txt";
}

/// `file` as 48 kHz mono float, or empty (with a log line) when SDL cannot read it as a WAV.
std::vector<float> Decode(const std::string& file) {
    SDL_AudioSpec spec{};
    Uint8* data = nullptr;
    Uint32 bytes = 0;
    if (!SDL_LoadWAV(file.c_str(), &spec, &data, &bytes)) {
        Log("%s: not a WAV SDL can read (%s): row not used", file.c_str(), SDL_GetError());
        return {};
    }
    const SDL_AudioSpec want{SDL_AUDIO_F32, 1, 48000};
    Uint8* converted = nullptr;
    int converted_bytes = 0;
    const bool ok = SDL_ConvertAudioSamples(&spec, data, int(bytes), &want, &converted, &converted_bytes);
    SDL_free(data);
    if (!ok || converted_bytes <= 0) {
        Log("%s: cannot convert to 48 kHz (%s): row not used", file.c_str(), SDL_GetError());
        SDL_free(converted);
        return {};
    }
    std::size_t frames = std::size_t(converted_bytes) / sizeof(float);
    if (frames > std::size_t(kMaxSeconds) * 48000) frames = std::size_t(kMaxSeconds) * 48000;
    std::vector<float> pcm(reinterpret_cast<const float*>(converted), reinterpret_cast<const float*>(converted) + frames);
    SDL_free(converted);
    return pcm;
}

void LoadTable() {
    const char* mods = std::getenv("BB_MODS_ENABLED");
    if (mods && mods[0] == '0') return;
    const std::string path = TablePath();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    std::size_t n = 0;
    while (text.size() <= (1u << 20) && (n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    std::size_t bad = 0;
    std::vector<Row> rows;
    if (text.size() > (1u << 20) || !ParseTable(text, &rows, &bad)) {
        Log("%s line %zu is not \"<event> <volume> <replace> <file>\": no rows used", path.c_str(), bad);
        return;
    }
    for (Row& row : rows) {
        std::vector<float> pcm = Decode(row.file);
        if (pcm.empty()) continue;
        if (g_verbose) Log("%s: %.2f s", row.file.c_str(), double(pcm.size()) / 48000.0);
        g_rows.push_back(std::move(row));
        g_pcm.push_back(std::move(pcm));
    }
}

}  // namespace

void Install() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* off = std::getenv("BB_SOUNDS");
    if (off && off[0] == '0') {
        Log("feature sounds: not installed (BB_SOUNDS=0)");
        std::printf("[coop] feature sounds: not installed (BB_SOUNDS=0)\n");
        return;
    }
    const char* v = std::getenv("BB_SOUNDS_LOG");
    g_verbose = v && v[0] == '1';
    LoadTable();
    // Installed with or without rows: the party version check hashes the installed hooks, and a cosmetic feature must
    // not split players whose mods differ.
    void* original = nullptr;
    if (!ReplacePrologue(kPlayAt, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}, reinterpret_cast<const void*>(&PlayAt),
                         &original, "sounds PlayAt")) {
        std::printf("[coop] feature sounds: not installed (PlayAt 0x27d3a80 bytes differ)\n");
        Log("feature sounds: not installed (PlayAt 0x27d3a80 bytes differ)");
        g_rows.clear();
        return;
    }
    g_play = reinterpret_cast<PlayAtFn>(original);
    if (!g_rows.empty()) runtime_audio_set_mod_level(&Level);
    std::printf("[coop] feature sounds: installed (%zu rows)\n", g_rows.size());
    Log("feature sounds: installed (%zu rows from %s)%s", g_rows.size(), g_rows.empty() ? "no mod table" : TablePath().c_str(),
        g_verbose ? ", BB_SOUNDS_LOG=1" : "");
}

}  // namespace coop::sounds
