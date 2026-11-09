// SPDX-License-Identifier: GPL-3.0-or-later
// The crash guards' pure rules (gpu/shim/bbport_scaleform_logic.h, bbport_fmod_logic.h): visy's
// test_stability_guards.cpp, run here (his runs it from test_stability_logic.cpp, which our tree does not have).
// ninja -C out/gpu crash-guards-test && out/gpu/crash-guards-test.exe
#include <cstdio>

int RunGuardTests(); // tests/test_stability_guards.cpp

int main() {
    const int failures = RunGuardTests();
    std::printf("crash-guards-test: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
