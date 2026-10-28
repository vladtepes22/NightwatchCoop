// SPDX-License-Identifier: GPL-2.0-or-later
// Legacy CPU: the boot image's BMI1, LZCNT and MOVBE instructions replaced by baseline x86-64
// code, for CPUs without them (AMD FX, Intel 2nd/3rd generation Core). A native port of
// scripts/cpu_compat.py (visy's bloodborne_pc_windows_port, from Supermedo/bloodborne_pc and the
// CPU part of T0ug's Ivy Bridge PR there) applying the same plan, tools/legacy-cpu-plan.json.
#pragma once
#include "common.h"

#include <nlohmann/json.hpp>

#include <array>

namespace start {

struct CpuCompatModule {
    std::string file;
    std::uint64_t base, size;
    std::string sha256; // of the module's file as the game ships it
};

struct CpuCompatInput {
    std::vector<CpuCompatModule> modules;          // without eboot.bin (it is at 0)
    std::string eboot_sha256;
    std::vector<std::uint64_t> reloc_targets;      // image offsets the loader writes, sorted
    std::vector<std::uint64_t> resolved_relocs;    // kind 0 relocations: value + addend
    std::vector<std::uint64_t> eboot_symbols;      // eboot symbols with a section: their value
    std::vector<std::int64_t> eboot_reloc_entries; // eboot ELF relocations' internal targets
};

struct CpuCompatSite {
    std::uint64_t start;
    Bytes original; // the bytes before the jump replaced them
};

struct CpuCompatResult {
    int patched = 0, skipped = 0, spans = 0;
    std::uint64_t arena = 0, trampoline_bytes = 0, end = 0;
    std::map<std::string, int> counts;
    std::vector<std::string> skipped_sites; // "module+address: reason", the first ones
    // Every rewritten site, by address: the red-zone sites near them stay as they are.
    std::vector<CpuCompatSite> sites;
};

/// False when a red-zone site at `offset` (its `length` bytes, and one instruction before it) reads
/// a byte the translations changed. visy's protect_image leaves such a site to the translation.
bool CpuCompatClear(const CpuCompatResult& result, const Bytes& image, std::uint64_t offset, std::uint64_t length);

/// Where the plan is: BB_LEGACY_CPU_PLAN, else engine\patches (package), else engine\tools (source tree).
fs::path LegacyCpuPlanPath(const fs::path& engine);

/// The extensions the plan translates that this CPU lacks ("BMI1", "LZCNT", "MOVBE").
std::vector<std::string> MissingCpuExtensions();

/// BB_LEGACY_CPU=1/0 (bb-start sets it from bbport.ini's legacy_cpu) decides; otherwise on when
/// the CPU lacks one of the extensions. `reason` says which.
bool LegacyCpuActive(std::string& reason);

/// Replaces libc's BMI1 strlen (libc at `base` in `image`) with a baseline loop. Empty on success,
/// else why it was left alone. visy's patch_libc.
std::string ReplaceLibcStrlen(Bytes& image, std::uint64_t base, std::uint64_t value, std::uint64_t size);

/// Applies the plan to the linked image: every site is guarded (its bytes, its function's hash, no
/// relocation and no known entry inside it) and skipped when a guard fails. Trampolines go to a new
/// executable segment at the next 64 KiB boundary. Throws Error when the plan does not describe
/// this game's modules (other SHA-256) or two sites overlap; the image is then unchanged.
CpuCompatResult ApplyCpuCompat(Bytes& image, std::vector<std::array<std::uint64_t, 3>>& segments,
                               const CpuCompatInput& input, const nlohmann::json& plan);

}
