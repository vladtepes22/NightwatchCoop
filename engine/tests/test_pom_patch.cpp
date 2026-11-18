// SPDX-License-Identifier: GPL-3.0-or-later
// The deeper-parallax SPIR-V patch (renderer_vulkan/vk_pom_patch.h): it declines every module that is not
// exactly the one it was written for (and then leaves the words alone), and on a synthetic module that has
// the shapes it looks for it moves the ray start, scales the step count and shades the cavity.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "video_core/renderer_vulkan/vk_pom_patch.h"

namespace {

namespace P = Vulkan::Pom;
using Words = std::vector<uint32_t>;

int g_failed = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
        std::printf("pom patch: FAILED %s\n", what);
    }
}

void Emit(Words& m, std::initializer_list<uint32_t> words) {
    m.push_back(P::Word(static_cast<uint32_t>(*words.begin()), static_cast<uint32_t>(words.size())));
    m.insert(m.end(), words.begin() + 1, words.end());
}

uint32_t Pack(const char* text, int at) {
    uint32_t word = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = text[at + i];
        word |= uint32_t(uint8_t(c)) << (8 * i);
        if (!c) break;
    }
    return word;
}

// A module with exactly the ids the table row of the second target (0x505acc58) names, and nothing else.
// `with_convert` = false leaves out the step-count conversion the patch needs (a shape it must decline).
Words Synthetic(const P::PomShader& s, const P::Cavity& c, bool with_convert) {
    Words m{0x07230203u, 0x10500u, 0u, 1500u, 0u};
    constexpr uint32_t f32 = 2, u32 = 3, vec4 = 4, ptr = 5, zero = 6, glsl = 20, steps_f = 50;
    Emit(m, {P::kOpExtInstImport, glsl, Pack("GLSL.std.450", 0), Pack("GLSL.std.450", 4),
             Pack("GLSL.std.450", 8), 0});
    Emit(m, {P::kOpEntryPoint, 4, 1, Pack("main", 0), 0});
    Emit(m, {P::kOpDecorate, 1000, 30, 0});
    Emit(m, {P::kOpTypeFloat, f32, 32});
    Emit(m, {P::kOpTypeInt, u32, 32, 0});
    Emit(m, {P::kOpTypeVector, vec4, f32, 4});
    Emit(m, {P::kOpTypePointer, ptr, 1, vec4});
    Emit(m, {P::kOpConstant, f32, zero, 0});
    Emit(m, {P::kOpFunction, 7, 8, 0, 9});
    Emit(m, {P::kOpLabel, 100});
    if (with_convert) {
        Emit(m, {P::kOpConvertFToS, u32, s.steps_convert, steps_f});
    }
    Emit(m, {P::kOpBranch, s.preheader});
    Emit(m, {P::kOpLabel, s.preheader});
    Emit(m, {P::kOpBranch, 121});
    Emit(m, {P::kOpLabel, 121});
    Emit(m, {P::kOpPhi, u32, s.layer_phi, 600, s.preheader, 601, 122});
    for (size_t k = 0; k < 4; ++k) {
        if (s.uv_phi[k]) {
            Emit(m, {P::kOpPhi, f32, s.uv_phi[k], uint32_t(610 + 2 * k), s.preheader, uint32_t(611 + 2 * k), 122});
        }
    }
    Emit(m, {P::kOpLabel, 122});
    Emit(m, {P::kOpBranch, c.merge_b});
    Emit(m, {P::kOpLabel, c.merge_b});
    Emit(m, {P::kOpPhi, f32, 900, c.depth, c.hit_b, zero, c.skip_b});
    for (const uint32_t value : c.values) {
        Emit(m, {62, 60, value});
    }
    Emit(m, {253});
    Emit(m, {56});
    return m;
}

size_t Find(const Words& m, uint32_t op, uint32_t result_word2 = 0, size_t from = 5) {
    for (size_t i = from; i < m.size(); i += m[i] >> 16) {
        if ((m[i] & 0xffff) == op && (!result_word2 || m[i + 2] == result_word2)) return i;
    }
    return 0;
}

size_t Count(const Words& m, uint32_t op) {
    size_t n = 0;
    for (size_t i = 5; i < m.size(); i += m[i] >> 16) n += (m[i] & 0xffff) == op;
    return n;
}

void TestTargets() {
    for (const auto& s : P::kShaders) Check(P::Targets(s.hash), "each table hash is a target");
    Check(!P::Targets(0) && !P::Targets(0x053bbf3a) && !P::Targets(0x1053bbf3b), "other hashes are not");
    Check(P::kShaders.size() == P::kCavities.size(), "every shader has a cavity row");
    for (size_t i = 0; i < P::kShaders.size(); ++i) {
        Check(P::kShaders[i].hash == P::kCavities[i].hash, "cavity rows follow the shader rows");
    }
}

void TestDeclines() {
    const auto& row = P::kShaders[1];
    Words words(row.words, 0u);
    Check(P::PatchPom(row.hash, false, words) == P::Outcome::NotTarget, "a vertex stage is declined");
    Check(P::PatchPom(0x12345678, true, words) == P::Outcome::NotTarget, "another hash is declined");
    // Right hash and size, wrong words: the checksum declines before anything is read as SPIR-V.
    const Words before = words;
    Check(P::PatchPom(row.hash, true, words) == P::Outcome::WrongModule, "wrong checksum is declined");
    Check(words == before, "a declined module is left as it was");
    Words other_size(row.words + 1, 0u);
    Check(P::PatchPom(row.hash, true, other_size) == P::Outcome::WrongModule, "wrong size is declined");
    Check(other_size.size() == row.words + 1, "a wrong-size module keeps its size");
    // The first target's size with the second target's hash is also wrong.
    Words swapped(P::kShaders[0].words, 0u);
    Check(P::PatchPom(row.hash, true, swapped) == P::Outcome::WrongModule, "a size of another row is declined");
}

void TestSyntheticPatch() {
    const auto& row = P::kShaders[1];
    const auto& cav = P::kCavities[1];
    const Words original = Synthetic(row, cav, true);
    Words m = original;
    const auto outcome = P::PatchShader(row, m);
    Check(outcome == P::Outcome::Patched, "a module with the shapes is patched, cavity included");
    Check(m.size() > original.size() && m[3] > original[3], "it grows and takes new ids");

    // The step count is scaled: the conversion reads a new product, made from the old step count.
    const size_t convert = Find(m, P::kOpConvertFToS, row.steps_convert);
    Check(convert && m[convert + 3] != 50, "the step conversion reads the scaled count");
    const size_t scale = Find(m, P::kOpFMul);
    Check(scale && m[scale + 3] == 50 && m[scale + 2] == m[convert + 3], "scaled count = old count times a constant");
    Check(scale < convert, "the scaling comes before the conversion");

    // The first layer and the start of the ray move: the preheader inputs of the three phis are new ids.
    const size_t layer = Find(m, P::kOpPhi, row.layer_phi);
    Check(layer && m[layer + 3] != 600 && m[layer + 4] == row.preheader, "layer phi starts from the jittered value");
    for (size_t k = 0; k < 4; ++k) {
        if (!row.uv_phi[k]) continue;
        const size_t uv = Find(m, P::kOpPhi, row.uv_phi[k]);
        Check(uv && m[uv + 3] != 610 + 2 * k, "uv phi starts from the moved start");
        Check(uv && m[uv + 5] == 611 + 2 * k, "the loop-carried input of a uv phi is untouched");
    }
    Check(m[Find(m, P::kOpPhi, row.layer_phi) + 5] == 601, "the loop-carried input of the layer phi is untouched");

    // Screen position input: a new Input variable, decorated FragCoord, named by the entry point.
    const size_t entry = Find(m, P::kOpEntryPoint, 0);
    Check((m[entry] >> 16) == 6, "the entry point lists one more interface id");
    const size_t variable = Find(m, P::kOpVariable);
    Check(variable && m[variable + 3] == 1, "a new Input variable");
    Check(m[entry + 5] == m[variable + 2], "...is the one the entry point lists");
    bool frag_coord = false;
    for (size_t i = 5; i < m.size(); i += m[i] >> 16) {
        frag_coord |= (m[i] & 0xffff) == P::kOpDecorate && m[i + 1] == m[variable + 2] && m[i + 2] == 11 && m[i + 3] == 15;
    }
    Check(frag_coord, "...decorated BuiltIn FragCoord");
    Check(variable < Find(m, P::kOpFunction), "the variable is a global, before the first function");

    // Cavity: each stored value is now a product of the old value and one factor; two phis in the merge block.
    size_t stores = 0;
    for (size_t i = 5; i < m.size(); i += m[i] >> 16) {
        if ((m[i] & 0xffff) != 62) continue;
        const size_t mul = Find(m, P::kOpFMul, m[i + 2]);
        Check(mul && m[mul + 3] == cav.values[stores], "a stored value is the old value times the cavity factor");
        ++stores;
    }
    Check(stores == cav.values.size(), "all six stores are kept");
    bool clamp = false;
    for (size_t i = 5; i < m.size(); i += m[i] >> 16) {
        clamp |= (m[i] & 0xffff) == P::kOpExtInst && m[i + 4] == 43;
    }
    Check(clamp, "the cavity factor uses a clamp");
    Check(Count(m, P::kOpPhi) == Count(original, P::kOpPhi) + 1, "one phi carries the depth through the merge block");
}

void TestSyntheticDecline() {
    const auto& row = P::kShaders[1];
    const auto& cav = P::kCavities[1];
    Words no_convert = Synthetic(row, cav, false);
    Check(P::PatchShader(row, no_convert) == P::Outcome::NoMatch, "a module without the step conversion is declined");

    Words bad_preheader = Synthetic(row, cav, true);
    const size_t layer = Find(bad_preheader, P::kOpPhi, row.layer_phi);
    bad_preheader[layer + 4] = 999; // no incoming value from the preheader
    Check(P::PatchShader(row, bad_preheader) == P::Outcome::NoMatch, "a phi without the preheader input is declined");

    // Stores that are not the cavity values: the ray start part still applies, the cavity part is left out.
    Words no_stores = Synthetic(row, cav, true);
    for (size_t i = 5; i < no_stores.size(); i += no_stores[i] >> 16) {
        if ((no_stores[i] & 0xffff) == 62) no_stores[i + 2] = 5;
    }
    Check(P::PatchShader(row, no_stores) == P::Outcome::PatchedNoCavity, "stores that are not the cavity values: no cavity");
}

} // namespace

int main() {
    TestTargets();
    TestDeclines();
    TestSyntheticPatch();
    TestSyntheticDecline();
    if (g_failed) {
        std::printf("pom patch: %d checks failed\n", g_failed);
        return 1;
    }
    std::printf("pom patch: all checks passed\n");
    return 0;
}
