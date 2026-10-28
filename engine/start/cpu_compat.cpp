// SPDX-License-Identifier: GPL-2.0-or-later
// Native port of scripts/cpu_compat.py patch_image / patch_libc / enabled (visy's
// bloodborne_pc_windows_port 0.7.6; from Supermedo/bloodborne_pc e750af88, 38e75888, 844a60b5,
// a87b3b4b, the CPU part of T0ug's Ivy Bridge PR #10 there). Same plan, same guards, same
// trampolines: tests/test_cpu_compat_native.py checks that both give the same bytes.
// One difference: a site whose guard fails is skipped and counted instead of refusing the whole
// image (a refused image would leave the old CPU with no translations at all).
#include "cpu_compat.h"

#include <cpuid.h>

#include <algorithm>
#include <cctype>
#include <set>

namespace start {
namespace {

constexpr std::uint64_t kMaxImage = 512ull << 20;
const char* const kSupported[] = {"andn", "bextr", "blsr", "blsi", "blsmsk", "tzcnt", "lzcnt", "movbe"};

// D015, the allocator's four count spans: a closed whitelist, never a general facility.
const char* const kCountSpanModule = "d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9";
constexpr std::uint64_t kCountSpanStart = 0x26addc0, kCountSpanEnd = 0x26adf49;
const char* const kCountSpanFunction = "16729bb23b2f814ef311ef0a3f3aac2d91f16110a206885eae694f2b17f38133";
const char* const kCountSpanEvidence = "docs/evidence/TASK-001/allocator-span-entries.json";
const char* const kCountSpanEvidenceSha = "c665159c757c6638e1edbf4fc20269720ca9431213a4ba05f7619f4738d30b7b";
struct CountSpan {
    std::uint64_t address;
    const char *bytes, *instruction, *body_sha256;
};
const CountSpan kCountSpans[] = {
    {0x26adde5, "f30fbdc0b920000000", "lzcnt eax, eax", "071663082920f0142ded5530ea5f039c945cf16e24f8c464b51b042db5e50284"},
    {0x26addfe, "f30fbcc883f910", "tzcnt ecx, eax", "e977e8446d3c70387ad8b12424546ebc5c9a0c1d84037ab57f69fb64e150634e"},
    {0x26adebe, "f30fbdfaba1f000000", "lzcnt edi, edx", "165fa7548268a439b5ff621df11cff11578f1ae8fe76ea2d0087c08193f40f53"},
    {0x26adf12, "f30fbdf1b91f000000", "lzcnt esi, ecx", "922a7f4f61bdd53b9cab783faebbd114691319f3c223cc857fe54f920f531745"},
};

// xor eax,eax; loop: cmp byte [rdi+rax],0; je done; inc rax; jmp loop; done: ret
const char* const kStrlenBaseline = "31c0803c0700740548ffc0ebf5c3";

using json = nlohmann::json;

bool IsInt(const json& value) { return value.is_number_integer(); }

std::uint64_t Uint(const json& value, const char* label) {
    if (!IsInt(value) || (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw Error(std::string(label) + " must be a nonnegative integer");
    return value.get<std::uint64_t>();
}

Bytes HexBytes(const json& value, const char* label, std::size_t minimum, std::size_t maximum) {
    if (!value.is_string()) throw Error(std::string(label) + " must contain hexadecimal bytes");
    const std::string text = value.get<std::string>();
    if (text.empty() || text.size() % 2 ||
        !std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }))
        throw Error(std::string(label) + " must contain hexadecimal bytes");
    Bytes data = FromHex(text);
    if (data.size() < minimum || data.size() > maximum) throw Error(std::string(label) + " has an invalid length");
    return data;
}

void Relative(Bytes& out, std::size_t at, std::int64_t target, std::int64_t next) {
    const std::int64_t displacement = target - next;
    if (displacement < INT32_MIN || displacement > INT32_MAX) throw Error("relative displacement is out of range");
    Store<std::int32_t>(out.data() + at, std::int32_t(displacement));
}

std::set<std::string> Keys(const json& object) {
    std::set<std::string> keys;
    for (auto it = object.begin(); it != object.end(); ++it) keys.insert(it.key());
    return keys;
}

}

std::vector<std::string> MissingCpuExtensions() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    const unsigned top = __get_cpuid_max(0, nullptr), extended = __get_cpuid_max(0x80000000u, nullptr);
    std::vector<std::string> missing;
    b = 0;
    if (top < 7 || !__get_cpuid_count(7, 0, &a, &b, &c, &d) || !(b & bit_BMI)) missing.push_back("BMI1");
    c = 0;
    if (extended < 0x80000001u || !__get_cpuid(0x80000001u, &a, &b, &c, &d) || !(c & bit_LZCNT))
        missing.push_back("LZCNT");
    c = 0;
    if (top < 1 || !__get_cpuid(1, &a, &b, &c, &d) || !(c & bit_MOVBE)) missing.push_back("MOVBE");
    return missing;
}

bool LegacyCpuActive(std::string& reason) {
    if (const auto value = Env("BB_LEGACY_CPU")) {
        reason = "BB_LEGACY_CPU=" + *value + " (legacy_cpu in bbport.ini or the environment)";
        return *value == "1";
    }
    std::string missing;
    for (const std::string& name : MissingCpuExtensions()) missing += (missing.empty() ? "" : ", ") + name;
    reason = missing.empty() ? "this CPU has BMI1, LZCNT and MOVBE" : "this CPU lacks " + missing;
    return !missing.empty();
}

fs::path LegacyCpuPlanPath(const fs::path& engine) {
    if (const auto path = Env("BB_LEGACY_CPU_PLAN")) return Path(*path);
    std::error_code error;
    const fs::path packaged = engine / "patches" / "legacy-cpu-plan.json";
    return fs::is_regular_file(packaged, error) ? packaged : engine / "tools" / "legacy-cpu-plan.json";
}

bool CpuCompatClear(const CpuCompatResult& result, const Bytes& image, std::uint64_t offset, std::uint64_t length) {
    const std::uint64_t from = offset >= 14 ? offset - 14 : 0, to = offset + length;
    // The sites are sorted and never overlap; the longest is 40 bytes.
    auto it = std::lower_bound(result.sites.begin(), result.sites.end(), from >= 40 ? from - 40 : 0,
                               [](const CpuCompatSite& s, std::uint64_t at) { return s.start < at; });
    for (; it != result.sites.end() && it->start < to; ++it)
        for (std::uint64_t i = 0; i < it->original.size(); ++i) {
            const std::uint64_t at = it->start + i;
            if (at >= from && at < to && at < image.size() && image[at] != it->original[i]) return false;
        }
    return true;
}

std::string ReplaceLibcStrlen(Bytes& image, std::uint64_t base, std::uint64_t value, std::uint64_t size) {
    static const Bytes andn = {0xc4, 0xe2, 0xf8, 0xf2, 0xc6};
    if (value != 0x2f970 || size != 224 || base + value + size > image.size() ||
        !std::equal(andn.begin(), andn.end(), image.begin() + std::ptrdiff_t(base + value + 24)))
        return "strlen signature does not match the inspected implementation";
    const Bytes baseline = FromHex(kStrlenBaseline);
    std::uint8_t* at = image.data() + base + value;
    std::copy(baseline.begin(), baseline.end(), at);
    std::fill(at + baseline.size(), at + size, 0x90);
    return {};
}

CpuCompatResult ApplyCpuCompat(Bytes& image, std::vector<std::array<std::uint64_t, 3>>& segments,
                               const CpuCompatInput& input, const json& plan) {
    CpuCompatResult result;
    if (image.empty() || image.size() > kMaxImage) throw Error("input image has an invalid size");
    if (!plan.is_object() || !plan.contains("version") || !IsInt(plan["version"]))
        throw Error("plan version must be 1, 2, 3 or 4");
    const std::int64_t version = plan["version"].get<std::int64_t>();
    if (version < 1 || version > 4) throw Error("plan version must be 1, 2, 3 or 4");
    if (!plan.contains("modules") || !plan["modules"].is_object() || !plan.contains("sites") ||
        !plan["sites"].is_array() || !plan.contains("unhandled") || !plan["unhandled"].is_array())
        throw Error("plan requires modules, sites and an unhandled list");

    for (const auto& s : segments)
        if (!s[1] || s[0] + s[1] > image.size()) throw Error("segment is outside the image");
    const auto mapped = [&](std::uint64_t start, std::uint64_t size, bool executable) {
        return std::any_of(segments.begin(), segments.end(), [&](const auto& s) {
            return s[0] <= start && start + size <= s[0] + s[1] && (!executable || (s[2] & 1));
        });
    };

    struct Location {
        std::uint64_t base, size;
        std::string sha256;
    };
    std::map<std::string, Location> locations;
    std::uint64_t main_size = image.size();
    for (const CpuCompatModule& m : input.modules) {
        if (m.file == "eboot.bin" || locations.count(m.file)) throw Error("duplicate module " + m.file);
        if (!m.size || m.base + m.size > image.size()) throw Error(m.file + ": module is outside the image");
        locations[m.file] = {m.base, m.size, m.sha256};
        main_size = std::min(main_size, m.base);
    }
    locations["eboot.bin"] = {0, main_size, input.eboot_sha256};
    if (plan["modules"].size() != locations.size()) throw Error("the plan's module set does not match the linked image");
    for (const auto& [name, location] : locations) {
        if (!plan["modules"].contains(name) || plan["modules"][name] != location.sha256)
            throw Error(name + ": its SHA-256 does not match the plan (another game version or a modified file)");
    }

    const std::uint64_t arena = (image.size() + 65535) & ~65535ull;
    std::uint64_t cursor = arena;
    struct Operation {
        std::uint64_t start, end, target;
        Bytes body, jump;
    };
    std::vector<Operation> operations;

    for (const json& site : plan["sites"]) {
        std::string label = "?";
        try {
            if (!site.is_object() || !site.contains("module") || !site["module"].is_string() ||
                !locations.count(site["module"].get<std::string>()))
                throw Error("site references an unknown module");
            const std::string name = site["module"].get<std::string>();
            const std::uint64_t address = Uint(site.value("address", json()), "site address");
            label = name + "+" + Hex(address);
            const std::string kind = site.value("kind", std::string("instruction"));
            const Bytes original = HexBytes(site.value("bytes", json()), "original bytes", 5, kind == "short_span" ? 40 : 15);
            Bytes body = HexBytes(site.value("body", json()), "body", 1, 4096);
            const json instruction_json = site.value("instruction", json());
            if (!instruction_json.is_string()) throw Error("unsupported instruction description");
            const std::string instruction = instruction_json.get<std::string>();
            const std::string mnemonic = instruction.substr(0, instruction.find(' '));
            if (std::none_of(std::begin(kSupported), std::end(kSupported), [&](const char* s) { return mnemonic == s; }))
                throw Error("unsupported instruction description");
            if (kind != "instruction" && kind != "count_span" && kind != "short_span") throw Error("unsupported site kind");
            const bool is_span = kind != "instruction";
            if (kind == "count_span" && version != 3 && version != 4) throw Error("count spans require plan version 3");
            if (kind == "short_span" && version != 4) throw Error("short spans require plan version 4");
            const bool short_count_prefix = original[0] == 0xf3 && original[1] == 0x0f &&
                                            (original[2] == 0xbc || original[2] == 0xbd) && (original[3] & 0xc0) == 0xc0;
            if (!is_span && (site.contains("span") || short_count_prefix))
                throw Error("count span cannot be treated as a single instruction");
            const Location& location = locations[name];
            const std::uint64_t start = location.base + address, end = start + original.size();
            if (address + original.size() > location.size || !mapped(start, original.size(), true))
                throw Error("site is outside its executable module segment");
            if (!std::equal(original.begin(), original.end(), image.begin() + std::ptrdiff_t(start)))
                throw Error("original instruction bytes do not match");
            const auto near = std::lower_bound(input.reloc_targets.begin(), input.reloc_targets.end(),
                                               start >= 7 ? start - 7 : 0);
            if (near != input.reloc_targets.end() && *near < end) throw Error("site overlaps a relocation");
            const json fixups = site.value("rip_fixups", json::array());

            if (kind == "count_span") {
                static const std::set<std::string> fields = {"kind", "module", "address", "bytes", "body",
                                                             "instruction", "rip_fixups", "span"};
                if (Keys(site) != fields) throw Error("count span site has missing or unknown fields");
                const CountSpan* spec = nullptr;
                for (const CountSpan& s : kCountSpans)
                    if (s.address == address) spec = &s;
                if (name != "eboot.bin" || !spec || ToHex(original.data(), original.size()) != spec->bytes ||
                    instruction != spec->instruction || location.sha256 != kCountSpanModule)
                    throw Error("count span is outside the closed whitelist");
                if (Sha256Hex(body) != spec->body_sha256) throw Error("count span body is not the reviewed translation");
                const json& span = site["span"];
                static const std::set<std::string> span_fields = {"count_length", "tail_bytes", "instruction_lengths",
                                                                  "function_start", "function_end", "function_sha256",
                                                                  "entry_evidence"};
                if (!span.is_object() || Keys(span) != span_fields) throw Error("count span metadata is incomplete");
                const json lengths = json::array({4, original.size() - 4});
                if (!IsInt(span["count_length"]) || span["count_length"] != 4 || !IsInt(span["function_start"]) ||
                    span["function_start"] != kCountSpanStart || !IsInt(span["function_end"]) ||
                    span["function_end"] != kCountSpanEnd || span["function_sha256"] != kCountSpanFunction ||
                    span["tail_bytes"] != ToHex(original.data() + 4, original.size() - 4) ||
                    !span["instruction_lengths"].is_array() || span["instruction_lengths"] != lengths)
                    throw Error("count span sequence/context metadata changed");
                if (!(kCountSpanStart <= address && address + original.size() <= kCountSpanEnd) ||
                    !mapped(kCountSpanStart, kCountSpanEnd - kCountSpanStart, true) ||
                    Sha256Hex(image.data() + kCountSpanStart, kCountSpanEnd - kCountSpanStart) != kCountSpanFunction)
                    throw Error("count span function signature does not match");
                if (fixups != json::array()) throw Error("count spans cannot contain RIP fixups");
                const json& evidence = span["entry_evidence"];
                static const std::set<std::string> evidence_fields = {"reference", "sha256", "method",
                                                                      "global_indirect_proof", "known_entries",
                                                                      "unresolved_candidates"};
                if (!evidence.is_object() || Keys(evidence) != evidence_fields ||
                    evidence["reference"] != kCountSpanEvidence || evidence["sha256"] != kCountSpanEvidenceSha ||
                    evidence["method"] != "allocator_count_spans_v1" || evidence["global_indirect_proof"] != false ||
                    !evidence["known_entries"].is_array() || !evidence["unresolved_candidates"].is_array())
                    throw Error("count span entry evidence does not match its reviewed manifest");
                if (!evidence["unresolved_candidates"].empty()) throw Error("count span has unresolved entry candidates");
                const auto entry = [&](std::int64_t target) {
                    if (target < 0 || std::uint64_t(target) > kMaxImage) throw Error("count span entry is outside the image");
                    if (address < std::uint64_t(target) && std::uint64_t(target) < address + original.size())
                        throw Error("count span has a known interior entry");
                };
                static const std::set<std::string> entry_kinds = {"symbol", "function", "direct_branch",
                                                                  "relocation", "jump_table", "landing_pad"};
                for (const json& known : evidence["known_entries"]) {
                    if (!known.is_object() || Keys(known) != std::set<std::string>{"kind", "address"} ||
                        !known["kind"].is_string() || !entry_kinds.count(known["kind"].get<std::string>()))
                        throw Error("count span known-entry record is invalid");
                    entry(std::int64_t(Uint(known["address"], "known entry address")));
                }
                for (const std::uint64_t target : input.resolved_relocs) entry(std::int64_t(target));
                for (const std::uint64_t value : input.eboot_symbols) entry(std::int64_t(value));
                for (const std::int64_t value : input.eboot_reloc_entries) entry(value);
                body.insert(body.end(), original.begin() + 4, original.end());
            } else if (kind == "short_span") {
                const json span = site.value("span", json());
                if (!span.is_object() || Keys(span) != std::set<std::string>{"pieces", "function", "entry_audit"})
                    throw Error("short span metadata is incomplete or has unknown fields");
                const json& pieces = span["pieces"];
                bool fine = pieces.is_array() && pieces.size() >= 2 && pieces.size() <= 4;
                bool translate = false;
                std::uint64_t total = 0;
                if (fine)
                    for (const json& p : pieces) {
                        if (!p.is_array() || p.size() != 4 || !p[0].is_string() ||
                            (p[0] != "translate" && p[0] != "move") || !IsInt(p[1]) || !IsInt(p[2])) {
                            fine = false;
                            break;
                        }
                        translate = translate || p[0] == "translate";
                        total += p[2].get<std::uint64_t>();
                    }
                if (!fine || !translate || total != end - start || pieces[0][1] != address)
                    throw Error("short span pieces do not describe its bytes");
                const json& audit = span["entry_audit"];
                if (!audit.is_object() || audit.value("method", json()) != "short_span_entries_v1" ||
                    audit.value("global_indirect_proof", json()) != false ||
                    audit.value("known_entries", json()) != json::array() ||
                    audit.value("unresolved_candidates", json()) != json::array())
                    throw Error("short span entry audit is missing or found entries");
                if (fixups != json::array()) throw Error("short spans cannot contain RIP fixups");
                const auto entry = [&](std::uint64_t target) {
                    if (start < target && target < end) throw Error("short span has a known interior entry");
                };
                for (const std::uint64_t target : input.resolved_relocs) entry(target);
                if (name == "eboot.bin")
                    for (const std::uint64_t value : input.eboot_symbols) entry(location.base + value);
            }

            std::uint64_t here = (cursor + 15) & ~15ull;
            if (!fixups.is_array() || (!fixups.empty() && version < 2)) throw Error("RIP fixups require plan version 2");
            std::set<std::uint64_t> fixed;
            for (const json& fixup : fixups) {
                if (!fixup.is_object()) throw Error("invalid RIP fixup");
                const std::uint64_t offset = Uint(fixup.value("offset", json()), "RIP displacement offset");
                const std::uint64_t following = Uint(fixup.value("next_offset", json()), "RIP next instruction offset");
                const std::uint64_t target = Uint(fixup.value("target", json()), "RIP target");
                if (!(offset + 4 <= following && following <= body.size()) || offset + 4 > body.size())
                    throw Error("RIP fixup is outside the body");
                if (target >= location.size || !mapped(location.base + target, 1, false))
                    throw Error("RIP target is outside mapped module data");
                for (std::uint64_t i = offset; i < offset + 4; ++i)
                    if (!fixed.insert(i).second) throw Error("RIP fixups overlap");
                Relative(body, offset, std::int64_t(location.base + target), std::int64_t(here + following));
            }
            Bytes jump(original.size(), 0x90);
            jump[0] = 0xe9;
            Relative(jump, 1, std::int64_t(here), std::int64_t(start + 5));
            body.push_back(0xe9);
            body.insert(body.end(), 4, 0);
            Relative(body, body.size() - 4, std::int64_t(end), std::int64_t(here + body.size()));
            if (here + body.size() > kMaxImage) throw Error("translated image exceeds the probe limit");
            cursor = here + body.size();
            operations.push_back({start, end, here, std::move(body), std::move(jump)});
            ++result.counts[mnemonic];
            result.spans += is_span;
        } catch (const std::exception& error) {
            ++result.skipped;
            if (result.skipped_sites.size() < 20) result.skipped_sites.push_back(label + ": " + error.what());
        }
    }

    std::vector<const Operation*> by_address;
    for (const Operation& op : operations) by_address.push_back(&op);
    std::sort(by_address.begin(), by_address.end(), [](auto* a, auto* b) { return a->start < b->start; });
    for (std::size_t i = 1; i < by_address.size(); ++i)
        if (by_address[i - 1]->end > by_address[i]->start) throw Error("instruction sites overlap or are duplicated");
    std::size_t semantic_risk = 0;
    for (const json& item : plan["unhandled"]) {
        if (!item.is_object() || !item.contains("module") || !item["module"].is_string() ||
            !locations.count(item["module"].get<std::string>()) || !item.contains("reason") ||
            !item["reason"].is_string() || !item.contains("instruction") || !item["instruction"].is_string())
            throw Error("invalid unhandled instruction report");
        const std::string text = item["instruction"].get<std::string>();
        const std::string mnemonic = text.substr(0, text.find(' '));
        semantic_risk += mnemonic == "lzcnt" || mnemonic == "tzcnt";
    }
    const bool partial = Env("BB_LEGACY_CPU_ALLOW_PARTIAL", "1") == "1";
    if (result.spans && !partial)
        throw Error("count spans are experimental (BB_LEGACY_CPU_ALLOW_PARTIAL=0 refuses them)");
    if (semantic_risk && !partial)
        throw Error("the plan leaves LZCNT/TZCNT sites (BB_LEGACY_CPU_ALLOW_PARTIAL=0 refuses that)");

    result.patched = int(operations.size());
    result.arena = arena;
    result.trampoline_bytes = cursor - arena;
    result.end = image.size();
    if (operations.empty()) return result;
    const std::uint64_t padded_end = (cursor + 4095) & ~4095ull;
    if (padded_end > kMaxImage) throw Error("aligned image exceeds the probe limit");
    image.resize(padded_end, 0);
    for (const Operation* op : by_address)
        result.sites.push_back({op->start, Bytes(image.begin() + std::ptrdiff_t(op->start), image.begin() + std::ptrdiff_t(op->end))});
    for (const Operation& op : operations) {
        std::copy(op.jump.begin(), op.jump.end(), image.begin() + std::ptrdiff_t(op.start));
        std::copy(op.body.begin(), op.body.end(), image.begin() + std::ptrdiff_t(op.target));
    }
    segments.push_back({arena, padded_end - arena, 5});
    result.end = padded_end;
    return result;
}

}
