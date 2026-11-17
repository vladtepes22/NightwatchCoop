// bbport: which failed GPU queue submits stop the game, and their words (shim/bbport_submit_result.h).

#include <cstdio>
#include <cstring>

#include "bbport_submit_result.h"

namespace {
int failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}
} // namespace

int main() {
    using namespace BbSubmit;
    // Default (BB_SUBMIT_FAIL_STOP on): every failure stops, success never does.
    Expect(!Stops(Success, true), "success goes on");
    Expect(Stops(DeviceLost, true), "device lost stops");
    Expect(Stops(OutOfDeviceMemory, true), "out of video memory stops");
    Expect(Stops(OutOfHostMemory, true), "out of system memory stops");
    Expect(Stops(-13, true), "an unexpected error stops");
    Expect(!Stops(5, true), "a positive status (not an error) goes on");
    // BB_SUBMIT_FAIL_STOP=0: only a device lost stops, as before.
    Expect(!Stops(Success, false), "off: success goes on");
    Expect(Stops(DeviceLost, false), "off: device lost still stops");
    Expect(!Stops(OutOfDeviceMemory, false), "off: out of video memory goes on (old behaviour)");
    Expect(!Stops(OutOfHostMemory, false), "off: out of system memory goes on (old behaviour)");
    // Words: one per known result, a fallback for the rest.
    Expect(std::strstr(Meaning(OutOfDeviceMemory), "video memory") != nullptr, "video memory words");
    Expect(std::strstr(Meaning(DeviceLost), "device lost") != nullptr, "device lost words");
    Expect(std::strcmp(Meaning(-13), "the graphics driver refused the work") == 0, "fallback words");
    std::printf(failures == 0 ? "submit-result-test: all passed\n" : "submit-result-test: %d failed\n",
                failures);
    return failures == 0 ? 0 : 1;
}
