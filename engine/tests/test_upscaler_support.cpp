// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <chrono>
#include <filesystem>
#include <string>
#include "bbport_settings.h"

namespace {
// setenv / unsetenv on both platforms (MinGW has neither; _putenv_s with "" removes the variable).
void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
void UnsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}
} // namespace

int main() {
    using namespace BbSettings;
    const std::string file = (std::filesystem::temp_directory_path() /
                              ("bbport-upscaler-test-" +
                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                                 .string();
    std::fclose(std::fopen(file.c_str(), "w"));
    const char* path = file.c_str();
    SetEnv("BB_CONFIG", path);
    UnsetEnv("BB_UPSCALER");
    UnsetEnv("BB_UPSCALE_PRESET");
    UnsetEnv("BB_RENDER_RES");
    auto& s = Get();
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);
    for (bool fsr4 : {false, true}) {
        for (bool fsr411 : {false, true}) {
            for (int requested = 0; requested < UpscalerCount; ++requested) {
                FILE* config = std::fopen(path, "w");
                assert(config);
                std::fprintf(config, "upscaler=%s\npreset=3\noutput_res=2560x1440\n",
                             UpscalerName(requested));
                std::fclose(config);
                s.fsr4_problem = nullptr;
                Load();
                assert(s.upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411); // no DLSS, no FSR SDK files
                // The fallbacks (ConfigureUpscalerSupport): FSR (SDK) and DLSS to FSR 3.1, FSR 4.1.1 to
                // FSR 4 where FSR 4 runs, else any FSR 4 to FSR 3.1.
                const auto fallback = [&](int r) {
                    if (r == UpscalerFsr || r == UpscalerDlss) return int(UpscalerFsr3);
                    if (r == UpscalerFsr411 && !(fsr4 && fsr411)) return int(fsr4 ? UpscalerFsr4 : UpscalerFsr3);
                    if (r == UpscalerFsr4 && !fsr4) return int(UpscalerFsr3);
                    return r;
                };
                const int expected = fallback(requested);
                assert(s.upscaler == expected);
                assert(s.fsr4_supported == fsr4);
                assert(s.fsr411_supported == (fsr4 && fsr411));
                assert(bool(s.fsr4_problem.load()) == (expected != requested));
                // 2560x1440 is a listed size (its index, not OutputCustom), wherever the list puts it.
                assert(s.preset == Performance && s.output_res < OutputAuto &&
                       OutputWidths[s.output_res] == 2560 && OutputHeights[s.output_res] == 1440);
                assert(s.startup_upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411); // a second call keeps the fallback
                assert(s.upscaler == expected);
            }
        }
    }
    s.startup_preset = Quality;
    s.startup_upscaler = UpscalerFsr3;
    s.upscaler = UpscalerFsr3;
    s.preset = Performance;
    assert(RenderPreset() == Performance && !ResolutionNeedsRestart());
    SetEnv("BB_RENDER_RES", "1706x960");
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerTaa;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const int output = s.startup_output_res;
    s.output_res = (output + 1) % OutputCount;
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    SetEnv("BB_RENDER_RES", "");
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    UnsetEnv("BB_RENDER_RES");
    std::remove(path);
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
}
