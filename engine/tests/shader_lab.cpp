// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the shader lab. Compiles shader captures (BB_SHADER_CAPTURE=1, shader_recompiler/native/
// capture.h) offline, without the game or a GPU, and reports what each compile holds that is not
// native GPU code yet: goto variables left by the structurizer, EXEC read or written as data,
// ballots and lane operations, and bit casts. Later, the native compiler next to the current one.
//
//   bb-shader-lab <capture folder> [report.csv] [--native] [--spv-dir DIR]
//                                                 every capture, each in a child process
//   bb-shader-lab --one <file.bbs> [--native] [--spv-dir DIR]
//                                                 one capture, a CSV line on stdout
//   bb-shader-lab --export-catalog <cache folder> <catalog folder>
//                                                 a cache as a shader catalog: specializations,
//                                                 pipeline keys and draw states, without code
//   bb-shader-lab --seed-cache <catalog folder> <cache folder> <game folder> <eboot.elf>
//                                                 a catalog into a player's cache, the shader code
//                                                 from their own game files, then recompiled
//   bb-shader-lab --recompile-cache <cache folder>
//                                                 the cache recompiled with this compiler from
//                                                 its shader states (bb-start, after an update)
// --native: the native compiler's options on (Profile::uniform_constant_buffers).
// --spv-dir: each compile's SPIR-V written there (<capture>.spv) for spirv-val.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/abstract_syntax_list.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/native/capture.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include <miniz.h>
#include <set>
#include "shader_recompiler/specialization.h"
#include "common/hash.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace fs = std::filesystem;
using namespace Shader;

// The pipeline serialization the cache recompile uses links in more of the GPU library than the
// shared test stubs (tests/test_gpu_runtime_stubs.cpp) cover; the lab never reaches these.
extern "C" {
void runtime_blocked_report(void) {}
[[noreturn]] void runtime_thread_exit(void*) { std::abort(); }
int runtime_pad_type(void) { return 0; }
int runtime_pad_button(uint32_t, int) { return 0; }
}

namespace {

bool g_native = false;
fs::path g_spv_dir;

std::vector<char> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Census {
    u32 insts = 0, goto_vars = 0, exec_reads = 0, exec_writes = 0, ballots = 0, lane_ops = 0,
        bitcasts = 0, discards = 0;
    u32 ifs = 0, loops = 0, breaks = 0;
};

Census Count(const IR::Program& program) {
    Census c;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            ++c.insts;
            switch (inst.GetOpcode()) {
            case IR::Opcode::GetGotoVariable:
            case IR::Opcode::SetGotoVariable:
                ++c.goto_vars;
                break;
            case IR::Opcode::GetExec:
                ++c.exec_reads;
                break;
            case IR::Opcode::SetExec:
                ++c.exec_writes;
                break;
            case IR::Opcode::Ballot:
            case IR::Opcode::InverseBallot:
            case IR::Opcode::BallotFindLsb:
                ++c.ballots;
                break;
            case IR::Opcode::ReadLane:
            case IR::Opcode::ReadFirstLane:
            case IR::Opcode::WriteLane:
            case IR::Opcode::Shuffle:
            case IR::Opcode::ShuffleXor:
            case IR::Opcode::QuadBroadcast:
                ++c.lane_ops;
                break;
            case IR::Opcode::BitCastU16F16:
            case IR::Opcode::BitCastU32F32:
            case IR::Opcode::BitCastF16U16:
            case IR::Opcode::BitCastF32U32:
                ++c.bitcasts;
                break;
            case IR::Opcode::Discard:
            case IR::Opcode::DiscardCond:
                ++c.discards;
                break;
            default:
                break;
            }
        }
    }
    for (const auto& node : program.syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::If:
            ++c.ifs;
            break;
        case IR::AbstractSyntaxNode::Type::Loop:
            ++c.loops;
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            ++c.breaks;
            break;
        default:
            break;
        }
    }
    return c;
}

/// Where a version 5 state's copy shader trailer starts (the size when it has none).
template <class Bytes>
size_t CopyTrailerAt(const Bytes& b) {
    if (b.size() < 8) return b.size();
    u32 count = 0, magic = 0;
    std::memcpy(&count, b.data() + b.size() - 8, 4);
    std::memcpy(&magic, b.data() + b.size() - 4, 4);
    if (magic != Native::CopyTrailerMagic || size_t(count) * 4 + 8 > b.size()) return b.size();
    return b.size() - 8 - size_t(count) * 4;
}

/// Dwords below 2^44 (a V#'s address range), reused by the next capture: the heap is above it.
u32* LowWords(size_t count) {
    static u32* block = nullptr;
    static size_t capacity = 0;
    if (count <= capacity) return block;
#ifdef _WIN32
    if (block) VirtualFree(block, 0, MEM_RELEASE);
    block = nullptr;
    capacity = 0;
    const size_t bytes = (count * 4 + 0xFFFF) & ~size_t(0xFFFF);
    for (uintptr_t at = 0x10000000; at < (uintptr_t(1) << 40) && !block; at += 0x10000000) {
        block = static_cast<u32*>(VirtualAlloc(reinterpret_cast<void*>(at), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (block) capacity = bytes / 4;
#endif
    return block;
}

/// A capture file read back for a compile: its buffers stay here, the user data's fetch shader and
/// V# table pointers aimed at them, and Info::lab_flat at the captured flattened user data.
struct LoadedCapture {
    Native::CaptureHeader header{};
    // RuntimeInfo has no default constructor (its unions): the captured bytes are the object.
    alignas(RuntimeInfo) unsigned char runtime_storage[sizeof(RuntimeInfo)]{};
    std::array<u32, ShaderParams::NumShaderUserData> user_data{};
    std::vector<u32> code, fetch, flat, copy;
    u32* tess = nullptr; ///< in low memory: a V#'s address has 44 bits
    std::map<u32, std::vector<u32>> tables;

    RuntimeInfo& Runtime() {
        return *std::launder(reinterpret_cast<RuntimeInfo*>(runtime_storage));
    }
    ShaderParams Params() {
        return ShaderParams{.user_data = user_data, .code = code, .hash = header.hash};
    }

    /// Null on success, else why the file is unusable.
    const char* Load(const fs::path& file) {
        const auto bytes = ReadAll(file);
        auto& h = header;
        if (bytes.size() < sizeof(h)) {
            return "capture cut short";
        }
        std::memcpy(&h, bytes.data(), sizeof(h));
        if (std::memcmp(h.magic, Native::CaptureMagic, 4) != 0 || h.version < 2 ||
            h.version > Native::CaptureVersion || h.runtime_info_size != sizeof(RuntimeInfo)) {
            return "capture of another engine version";
        }
        const char* p = bytes.data() + sizeof(h);
        const char* const end = bytes.data() + bytes.size();
        const auto take = [&](void* out, size_t size) {
            if (end < p + size) {
                return false;
            }
            std::memcpy(out, p, size);
            p += size;
            return true;
        };
        std::vector<u32> ud(h.user_data_count);
        code.resize(h.code_dwords);
        fetch.resize(h.fetch_dwords);
        // The flattened user data the resource table walker read in the game: the compile reads
        // sharps from it, and the walker would read the game's memory, which is not here.
        flat.resize(h.flat_dwords);
        if (!take(runtime_storage, sizeof(RuntimeInfo)) || !take(ud.data(), ud.size() * 4) ||
            !take(code.data(), code.size() * 4) || !take(fetch.data(), fetch.size() * 4) ||
            !take(flat.data(), flat.size() * 4)) {
            return "capture cut short";
        }
        std::memcpy(user_data.data(), ud.data(), std::min(ud.size(), user_data.size()) * 4);
        // The vertex attributes' V#s, read through table pointers in the user data: each table
        // is rebuilt here with the captured V#s at their offsets, and its pointer aimed at it.
        u32 sharp_count = 0;
        if (h.version >= 3 && !take(&sharp_count, 4)) {
            return "capture cut short";
        }
        std::vector<Native::CaptureSharp> sharps(sharp_count);
        if (!take(sharps.data(), sharps.size() * sizeof(Native::CaptureSharp))) {
            return "capture cut short";
        }
        for (const auto& sharp : sharps) {
            auto& table = tables[sharp.sgpr_base];
            if (table.size() < sharp.dword_offset + 4) {
                table.resize(sharp.dword_offset + 4);
            }
            std::memcpy(&table[sharp.dword_offset], sharp.words, sizeof(sharp.words));
        }
        // Version 4: the tessellation constants, their V#'s address aimed at the copy here.
        Native::CaptureTess tess_info{};
        if (h.version >= 4) {
            if (!take(&tess_info, sizeof(tess_info))) {
                return "capture cut short";
            }
            tess = LowWords((tess_info.size + 3) / 4);
            if (!tess) {
                return "no low memory for the tessellation constants";
            }
            if (!take(tess, tess_info.size)) {
                return "capture cut short";
            }
        }
        if (tess_info.size) {
            AmdGpu::Buffer buffer;
            std::memcpy(&buffer, tess_info.words, sizeof(buffer));
            buffer.base_address = reinterpret_cast<uintptr_t>(tess);
            if (buffer.base_address != reinterpret_cast<uintptr_t>(tess)) {
                return "tessellation constants above the V# address range";
            }
            u32 words[4];
            std::memcpy(words, &buffer, sizeof(words));
            if (tess_info.sgpr_base == IR::NumScalarRegs) {
                if (tess_info.dword_offset + 4 <= user_data.size()) {
                    std::memcpy(&user_data[tess_info.dword_offset], words, sizeof(words));
                }
            } else {
                auto& table = tables[tess_info.sgpr_base];
                if (table.size() < tess_info.dword_offset + 4) {
                    table.resize(tess_info.dword_offset + 4);
                }
                std::memcpy(&table[tess_info.dword_offset], words, sizeof(words));
            }
        }
        for (auto& [sgpr, table] : tables) {
            if (sgpr + 1 < user_data.size()) {
                const u32* address = table.data();
                std::memcpy(&user_data[sgpr], &address, sizeof(address));
            }
        }
        if (!fetch.empty() && h.fetch_sgpr + 1 < user_data.size()) {
            // The fetch shader is read through its address in the user data: aim it at the copy.
            const u32* address = fetch.data();
            std::memcpy(&user_data[h.fetch_sgpr], &address, sizeof(address));
        }
        // Version 5: a geometry shader's copy shader, at the end of the file.
        if (const auto at = CopyTrailerAt(bytes); at != bytes.size()) {
            u32 count = 0;
            std::memcpy(&count, bytes.data() + bytes.size() - 8, 4);
            copy.resize(count);
            std::memcpy(copy.data(), bytes.data() + at, size_t(count) * 4);
        }
        if (HwStage(h.hw_stage) == HwStage::Geometry) {
            if (copy.empty()) {
                return "geometry shader without its copy shader (a state of an older engine)";
            }
            Runtime().hw.gs.vs_copy = copy;
        }
        Info::lab_flat = flat.data();
        Info::lab_flat_size = flat.size();
        return nullptr;
    }
};

const char* kHeader = "file,stage,hash,gcn_dwords,status,ir_insts,spirv_words,goto_vars,exec_reads,"
                      "exec_writes,ballots,lane_ops,bitcasts,discards,ifs,loops,breaks";

/// One capture: compile it with the current recompiler, print a CSV line. Assertions end the
/// process (the parent reports it).
int RunOne(const fs::path& file, const fs::path& profile_file) {
    const auto profile_bytes = ReadAll(profile_file);
    Profile profile{};
    if (profile_bytes.size() < 4) {
        std::printf("%s,,,,no profile.bin\n", file.filename().string().c_str());
        return 2;
    }
    // A profile captured by an engine with fewer fields: the rest stay off.
    std::memcpy(&profile, profile_bytes.data() + 4,
                std::min<size_t>(profile_bytes.size() - 4, sizeof(Profile)));
    profile.uniform_constant_buffers = g_native;

    LoadedCapture cap;
    if (const char* error = cap.Load(file)) {
        std::printf("%s,,,,%s\n", file.filename().string().c_str(), error);
        return 2;
    }
    const auto& h = cap.header;
    const auto& code = cap.code;
    RuntimeInfo& runtime_info = cap.Runtime();
    ShaderParams params = cap.Params();
    Info info(HwStage(h.hw_stage), SwStage(h.sw_stage), params);
    Pools pools;
    const auto program = TranslateProgram(code, pools, info, runtime_info, profile);
    Backend::Bindings bindings{};
    const auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
    if (!g_spv_dir.empty()) {
        std::ofstream out(g_spv_dir / (file.stem().string() + ".spv"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(spv.data()), std::streamsize(spv.size() * 4));
    }
    const Census c = Count(program);
    std::printf("%s,%u,%016llx,%zu,ok,%u,%zu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                file.filename().string().c_str(), h.hw_stage, (unsigned long long)h.hash,
                code.size(), c.insts, spv.size(), c.goto_vars, c.exec_reads, c.exec_writes,
                c.ballots, c.lane_ops, c.bitcasts, c.discards, c.ifs, c.loops, c.breaks);
    // Every IR operation of the final program, for the run's histogram (ops.csv).
    std::map<std::string, u32> ops;
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            ++ops[std::string(IR::NameOf(inst.GetOpcode()))];
        }
    }
    std::printf("OPS|");
    for (const auto& [name, n] : ops) {
        std::printf("%s=%u;", name.c_str(), n);
    }
    std::printf("\n");
    return 0;
}

#ifdef _WIN32
/// Runs this program with `args` and returns its whole stdout and exit code.
std::string RunSelf(const fs::path& self, const std::wstring& args, DWORD& exit_code) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    CreatePipe(&read, &write, &sa, 0);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring line = L"\"" + self.wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    std::string out;
    exit_code = ~0u;
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        CloseHandle(write);
        write = nullptr;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof buffer, &got, nullptr) && got) {
            out.append(buffer, got);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (write) {
        CloseHandle(write);
    }
    CloseHandle(read);
    return out;
}

/// Runs `--one file` in a child process and returns its stdout (empty when it crashed).
std::string RunChild(const fs::path& self, const fs::path& file, DWORD& exit_code, std::string& ops) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    CreatePipe(&read, &write, &sa, 0);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring line = L"\"" + self.wstring() + L"\" --one \"" + file.wstring() + L"\"";
    if (g_native) {
        line += L" --native";
    }
    if (!g_spv_dir.empty()) {
        line += L" --spv-dir \"" + g_spv_dir.wstring() + L"\"";
    }
    PROCESS_INFORMATION pi{};
    std::string out;
    exit_code = ~0u;
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        CloseHandle(write);
        write = nullptr;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof buffer, &got, nullptr) && got) {
            out.append(buffer, got);
        }
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (write) {
        CloseHandle(write);
    }
    CloseHandle(read);
    // Only the CSV line: the recompiler's own log lines go to stdout too.
    std::string csv;
    size_t at = 0;
    while (at < out.size()) {
        const size_t end = out.find('\n', at);
        std::string row = out.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        if (row.rfind(file.filename().string(), 0) == 0) {
            csv = row;
        } else if (row.rfind("OPS|", 0) == 0) {
            ops = row.substr(4);
        }
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    return csv;
}
#endif

int RunAll(const fs::path& self, const fs::path& dir, const fs::path& report) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".bbs") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::ofstream csv(report);
    csv << kHeader << '\n';
    u32 ok = 0, crashed = 0;
    Census total;
    u32 with_goto = 0, with_exec = 0, with_ballot = 0, with_lane = 0;
    std::map<u32, u32> stages;
    std::map<std::string, std::map<u32, u64>> histogram; ///< IR operation -> hardware stage -> count
    for (size_t i = 0; i < files.size(); ++i) {
#ifdef _WIN32
        DWORD code = 0;
        std::string ops;
        std::string row = RunChild(self, files[i], code, ops);
#else
        std::string row, ops;
        u32 code = 1;
#endif
        if (row.empty() || code != 0) {
            ++crashed;
            csv << files[i].filename().string() << ",,,,crashed (exit " << code << ")\n";
            continue;
        }
        csv << row << '\n';
        ++ok;
        // stage, hash, dwords, status, insts, spirv, goto, exec_r, exec_w, ballots, lane, bitcasts
        u32 v[17] = {};
        unsigned long long hash = 0;
        char status[16] = {};
        size_t comma = row.find(',');
        std::sscanf(row.c_str() + comma + 1, "%u,%llx,%u,%15[^,],%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                    &v[0], &hash, &v[1], status, &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8],
                    &v[9], &v[10], &v[11], &v[12], &v[13]);
        ++stages[v[0]];
        for (size_t at = 0; at < ops.size();) {
            const size_t end = ops.find(';', at);
            const std::string pair = ops.substr(at, end == std::string::npos ? std::string::npos : end - at);
            const size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                const u64 n = std::strtoull(pair.c_str() + eq + 1, nullptr, 10);
                histogram[pair.substr(0, eq)][v[0]] += n;
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1;
        }
        total.insts += v[2];
        total.goto_vars += v[4];
        total.exec_reads += v[5];
        total.exec_writes += v[6];
        total.ballots += v[7];
        total.lane_ops += v[8];
        total.bitcasts += v[9];
        with_goto += v[4] != 0;
        with_exec += (v[5] + v[6]) != 0;
        with_ballot += v[7] != 0;
        with_lane += v[8] != 0;
        if ((i + 1) % 200 == 0) {
            std::printf("%zu of %zu\n", i + 1, files.size());
        }
    }
    {
        // ops.csv beside the report: every IR operation, by hardware stage.
        std::ofstream out(fs::path(report).replace_filename(report.stem().string() + "-ops.csv"));
        out << "op,total,fragment,vertex,compute,other\n";
        for (const auto& [name, per_stage] : histogram) {
            unsigned long long total_n = 0, other = 0;
            for (const auto& [stage, n] : per_stage) {
                total_n += n;
                if (stage != 0 && stage != 1 && stage != 6) {
                    other += n;
                }
            }
            const auto at = [&](u32 st) {
                const auto it = per_stage.find(st);
                return it == per_stage.end() ? 0ull : (unsigned long long)it->second;
            };
            out << name << ',' << total_n << ',' << at(0) << ',' << at(1) << ',' << at(6) << ',' << other
                << '\n';
        }
    }
    std::printf("Shader lab: %zu captures, %u compiled, %u crashed the current recompiler\n",
                files.size(), ok, crashed);
    for (const auto& [stage, n] : stages) {
        std::printf("  hardware stage %u: %u\n", stage, n);
    }
    std::printf("  with goto variables: %u (%u in all)\n", with_goto, total.goto_vars);
    std::printf("  with EXEC read or written as data: %u (%u reads, %u writes)\n", with_exec,
                total.exec_reads, total.exec_writes);
    std::printf("  with ballots: %u (%u)\n", with_ballot, total.ballots);
    std::printf("  with lane operations: %u (%u)\n", with_lane, total.lane_ops);
    std::printf("  bit casts: %u in %u IR instructions\n", total.bitcasts, total.insts);
    std::printf("Per shader: %s\n", report.string().c_str());
    return 0;
}

// --- The cache recompile (bb-start runs it when the shader compiler changed) ---------------------
// The cache folder holds per permutation "<pgm>_<perm>.spv" (SPIR-V), "<perm_hash>.meta" (its
// specialization and Info) and, since 0.9.34, "<pgm>_<perm>.bbs" (its compile inputs, the shader
// state). Each permutation with a state is compiled again; it is kept when this compiler's
// specialization equals the stored one (the pipeline keys name permutations by it), and left out
// otherwise. The pipeline keys stay: the engine's warm-up builds every pipeline whose shaders are
// there before the game starts, instead of each one compiling in play.

std::vector<std::string> SpvNames(const fs::path& dir);

/// The permutations to compile: those listed in `only` (one name a line) when given, else all.
std::vector<std::string> BatchNames(const fs::path& dir, const fs::path& only) {
    if (only.empty()) {
        return SpvNames(dir);
    }
    std::vector<std::string> names;
    std::ifstream in(only);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) names.push_back(line);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> SpvNames(const fs::path& dir) {
    // The permutations with SPIR-V, and those with only a state (seeded from a catalog).
    std::set<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        const auto ext = entry.path().extension();
        if (ext == ".spv" || ext == Native::StateExtension) {
            names.insert(entry.path().stem().string());
        }
    }
    return {names.begin(), names.end()};
}

bool WriteAtomically(const fs::path& path, const void* data, size_t size) {
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), std::streamsize(size));
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    return !error;
}

fs::path MetaPath(const fs::path& dir, u64 pgm, size_t perm) {
    char name[40];
    std::snprintf(name, sizeof(name), "0x%016llx.meta", (unsigned long long)HashCombine(pgm, u64(perm)));
    return dir / name;
}

void Leave(const fs::path& dir, const std::string& name, u64 pgm, size_t perm) {
    std::error_code error;
    fs::remove(dir / (name + ".spv"), error);
    // Its state too: the permutation compiles again in play and keeps a fresh one.
    fs::remove(dir / (name + Native::StateExtension), error);
    fs::remove(MetaPath(dir, pgm, perm), error);
}

/// Child: the permutations from `start` on, one line each ("@OK i", "@OUT i why"), then "@END".
/// It only reports what to leave out: the parent removes those files once every batch is done, so
/// each batch lists the same names in the same order.
int RecompileBatch(const fs::path& dir, size_t start, const fs::path& only) {
    const auto profile_bytes = ReadAll(dir / "profile.bin");
    Profile profile{};
    if (profile_bytes.size() != sizeof(Profile)) {
        std::printf("\n@PROFILE\n"); // a profile of another engine: the cache is started again
        return 3;
    }
    std::memcpy(&profile, profile_bytes.data(), sizeof(Profile));
    const auto names = BatchNames(dir, only);
    Pools pools;
    for (size_t i = start; i < names.size(); ++i) {
        const auto& name = names[i];
        unsigned long long pgm = 0;
        size_t perm = 0;
        const auto out = [&](const char* why) {
            std::printf("\n@OUT %zu %s\n", i, why);
            std::fflush(stdout);
        };
        if (std::sscanf(name.c_str(), "0x%llx_%zu", &pgm, &perm) != 2) {
            out("name");
            continue;
        }
        const auto meta_bytes = ReadAll(MetaPath(dir, pgm, perm));
        StageSpecialization old_spec;
        size_t meta_perm = ~size_t{};
        if (meta_bytes.empty() ||
            !Vulkan::ReadShaderMetaSpec(std::vector<u8>(meta_bytes.begin(), meta_bytes.end()),
                                        old_spec, meta_perm) ||
            meta_perm != perm) {
            out("record of another engine");
            continue;
        }
        LoadedCapture cap;
        if (cap.Load(dir / (name + Native::StateExtension))) {
            out("no state");
            continue;
        }
        // Which permutation is being compiled, should the compiler end the process here.
        std::printf("\n@AT %zu %u %u\n", i, cap.header.hw_stage, cap.header.sw_stage);
        std::fflush(stdout);
        // A catalog's state has no fetch shader code (the game generates it at run time): its
        // layout comes from the specialization instead.
        Gcn::fetch_override = cap.fetch.empty() && cap.header.fetch_sgpr != ~0u && old_spec.fetch_shader_data
                                  ? &*old_spec.fetch_shader_data
                                  : nullptr;
        ShaderParams params = cap.Params();
        Info info(HwStage(cap.header.hw_stage), SwStage(cap.header.sw_stage), params);
        // The RuntimeInfo before the compile, as the engine's CompileModule has it.
        RuntimeInfo runtime_info = cap.Runtime();
        const auto program = TranslateProgram(cap.code, pools, info, runtime_info, profile);
        Backend::Bindings bindings = old_spec.start;
        auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
        const StageSpecialization new_spec(info, runtime_info, profile, old_spec.start);
        old_spec.info = &info;
        if (!(new_spec == old_spec)) {
            // What differs, for the report (BB_LAB_VERBOSE=1 lists them).
            std::string why = "specialization changed:";
            if (!(new_spec.runtime_info == old_spec.runtime_info)) why += " runtime";
            if (new_spec.vs_attribs != old_spec.vs_attribs) why += " vs_attribs";
            if (new_spec.fetch_shader_data != old_spec.fetch_shader_data) why += " fetch";
            if (new_spec.fmasks != old_spec.fmasks) why += " fmasks";
            if (new_spec.start != old_spec.start) why += " start";
            if (new_spec.buffers.size() != old_spec.buffers.size() || new_spec.buffers != old_spec.buffers) why += " buffers";
            if (new_spec.images.size() != old_spec.images.size() || new_spec.images != old_spec.images) why += " images";
            if (new_spec.samplers.size() != old_spec.samplers.size() || new_spec.samplers != old_spec.samplers) why += " samplers";
            if (new_spec.bitset != old_spec.bitset) why += " bitset";
            out(why.c_str());
            continue;
        }
        const auto meta = Vulkan::SerializeShaderMeta(info, new_spec, HashCombine(pgm, u64(perm)), perm);
        if (!WriteAtomically(MetaPath(dir, pgm, perm), meta.data(), meta.size()) ||
            !WriteAtomically(dir / (name + ".spv"), spv.data(), spv.size() * sizeof(u32))) {
            out("write failed");
            continue;
        }
        std::printf("\n@OK %zu\n", i);
        std::fflush(stdout);
    }
    std::printf("\n@END\n");
    return 0;
}

/// Parent: batches in child processes; a permutation that crashes the compiler is left out and
/// the next batch starts after it.
int RecompileCache(const fs::path& self, const fs::path& dir, const fs::path& only = {}) {
    const auto names = BatchNames(dir, only);
    size_t next = 0, kept = 0, left = 0, crashed = 0;
    std::vector<size_t> leave;
    const char* verbose_env = std::getenv("BB_LAB_VERBOSE");
    const bool verbose = verbose_env && verbose_env[0] == '1';
    std::map<std::string, u32> reasons; // why permutations were left out, for BB_LAB_VERBOSE=1
    while (next < names.size()) {
#ifdef _WIN32
        DWORD code = 0;
        const std::string output =
            RunSelf(self, L"--recompile-batch \"" + dir.wstring() + L"\" " + std::to_wstring(next) +
                              (only.empty() ? std::wstring{} : L" \"" + only.wstring() + L"\""),
                    code);
#else
        const std::string output;
        u32 code = 1;
#endif
        if (output.find("@PROFILE") != std::string::npos) {
            std::printf("Shader cache: written by an engine with another profile; started again\n");
            return 3;
        }
        long long last = -1;
        std::string at_stage;
        bool ended = false;
        size_t at = 0;
        while (at < output.size()) {
            const size_t end = output.find('\n', at);
            const std::string row = output.substr(at, end == std::string::npos ? std::string::npos : end - at);
            size_t index = 0;
            if (std::sscanf(row.c_str(), "@OK %zu", &index) == 1) {
                ++kept;
                last = (long long)index;
            } else if (std::sscanf(row.c_str(), "@OUT %zu", &index) == 1) {
                ++left;
                leave.push_back(index);
                last = (long long)index;
                const auto why = row.find(' ', 5);
                ++reasons[why == std::string::npos ? "?" : row.substr(why + 1)];
            } else if (row.rfind("@AT ", 0) == 0) {
                at_stage = row.substr(4);
            } else if (row.rfind("@END", 0) == 0) {
                ended = true;
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1;
        }
        if (ended) {
            break;
        }
        // The child stopped at the permutation after its last line: that one is left out.
        const size_t failed = last < 0 ? next : size_t(last) + 1;
        if (failed >= names.size()) {
            break;
        }
        leave.push_back(failed);
        ++crashed;
        ++reasons["compiler refused, hw/sw stage " + at_stage.substr(at_stage.find(' ') + 1)];
        next = failed + 1;
        (void)code;
    }
    for (const size_t index : leave) {
        unsigned long long pgm = 0;
        size_t perm = 0;
        std::sscanf(names[index].c_str(), "0x%llx_%zu", &pgm, &perm);
        Leave(dir, names[index], pgm, perm);
    }
    if (verbose) {
        for (const auto& [why, n] : reasons) {
            std::printf("  %u: %s\n", n, why.c_str());
        }
    }
    std::printf("Shader cache: %zu shaders recompiled for this engine, %zu left out (changed or "
                "without a state), %zu the compiler refused\n",
                kept, left, crashed);
    return 0;
}

// --- The shader catalog ----------------------------------------------------------------------------
// A cache folder without anything taken from the game: each permutation's specialization (.meta),
// the pipeline keys (.key) and the draw states (.bbs) with no shader or fetch shader code. A
// player's PC puts the code back from its own game files (the shader bundles in
// dvdroot_ps4/shader and the executable), by the hash in each program's OrbShdr footer.

std::vector<u8> ReadBytes(const fs::path& path) {
    const auto v = ReadAll(path);
    return {v.begin(), v.end()};
}

bool WriteBytes(const fs::path& path, const std::vector<u8>& bytes) {
    return WriteAtomically(path, bytes.data(), bytes.size());
}

/// The layout offsets of a state file's parts (header, RuntimeInfo, user data, code, fetch, rest).
struct StateLayout {
    Native::CaptureHeader header{};
    size_t code_at = 0, fetch_at = 0, rest_at = 0;
    bool Parse(const std::vector<u8>& b) {
        if (b.size() < sizeof(header)) return false;
        std::memcpy(&header, b.data(), sizeof(header));
        if (std::memcmp(header.magic, Native::CaptureMagic, 4) != 0) return false;
        code_at = sizeof(header) + header.runtime_info_size + size_t(header.user_data_count) * 4;
        fetch_at = code_at + size_t(header.code_dwords) * 4;
        rest_at = fetch_at + size_t(header.fetch_dwords) * 4;
        return rest_at <= b.size();
    }
};

int ExportCatalog(const fs::path& cache, const fs::path& out) {
    std::error_code error;
    fs::create_directories(out, error);
    u32 states = 0, metas = 0, keys = 0;
    for (const auto& entry : fs::directory_iterator(cache, error)) {
        const auto& path = entry.path();
        const auto ext = path.extension();
        if (ext == ".key" || ext == ".meta" || path.filename() == "profile.bin") {
            fs::copy_file(path, out / path.filename(), fs::copy_options::overwrite_existing, error);
            ext == ".key" ? ++keys : ext == ".meta" ? ++metas : 0;
        } else if (ext == Native::StateExtension) {
            auto bytes = ReadBytes(path);
            StateLayout layout;
            if (!layout.Parse(bytes)) continue;
            // Without the code and the fetch shader's code: what is left is the draw's state.
            std::vector<u8> stripped(bytes.begin(), bytes.begin() + layout.code_at);
            stripped.insert(stripped.end(), bytes.begin() + layout.rest_at, bytes.begin() + CopyTrailerAt(bytes));
            auto header = layout.header;
            header.code_dwords = 0;
            header.fetch_dwords = 0;
            std::memcpy(stripped.data(), &header, sizeof(header));
            if (WriteBytes(out / path.filename(), stripped)) ++states;
        }
    }
    std::printf("Shader catalog: %u states, %u specializations, %u pipeline keys in %s\n", states, metas,
                keys, out.string().c_str());
    return 0;
}

/// Every Gnm program in the game's files by its hash: the shader bundles and the executable.
struct CodeIndex {
    std::vector<std::vector<u8>> blobs;
    std::map<u64, std::pair<size_t, size_t>> programs; ///< hash -> blob, byte offset of the code

    static std::vector<u8> Undcx(std::vector<u8>&& data) {
        if (data.size() < 0x4c || std::memcmp(data.data(), "DCX", 4) != 0) return std::move(data);
        if (std::memcmp(data.data() + 0x28, "DFLT", 4) != 0) return {};
        const auto be32 = [&](size_t at) {
            return u32(data[at]) << 24 | u32(data[at + 1]) << 16 | u32(data[at + 2]) << 8 | data[at + 3];
        };
        const auto dca = std::search(data.begin(), data.end(), "DCA", "DCA" + 4) - data.begin();
        if (size_t(dca) + 8 > data.size()) return {};
        const size_t skip = be32(size_t(dca) + 4);
        mz_ulong size = be32(0x1c);
        std::vector<u8> out(size);
        if (mz_uncompress(out.data(), &size, data.data() + dca + skip, mz_ulong(data.size() - dca - skip)) != MZ_OK) {
            return {};
        }
        out.resize(size);
        return out;
    }

    void Add(std::vector<u8>&& blob) {
        const size_t index = blobs.size();
        static constexpr char kSig[] = "OrbShdr";
        for (size_t at = 0; (at = std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()).find(kSig, at)) != std::string_view::npos; ++at) {
            if (at + 28 > blob.size()) break;
            u64 hash;
            std::memcpy(&hash, blob.data() + at + 16, 8);
            // The program starts with s_mov_b32 vcc_hi, #imm whose immediate puts the footer at
            // code + (imm + 1) * 8 bytes (AmdGpu::SearchBinaryInfo).
            for (size_t p = at & ~size_t(3); p >= 8 && at - p < 0x40000; p -= 4) {
                u32 token, imm;
                std::memcpy(&token, blob.data() + p, 4);
                std::memcpy(&imm, blob.data() + p + 4, 4);
                if (token == 0xBEEB03FFu && p + (size_t(imm) + 1) * 8 == at) {
                    programs.try_emplace(hash, index, p);
                    break;
                }
            }
        }
        blobs.push_back(std::move(blob));
    }

    /// The program's code as the engine reads it (BinaryInfo::length bytes from its start).
    std::vector<u32> Code(u64 hash) const {
        const auto it = programs.find(hash);
        if (it == programs.end()) return {};
        const auto& blob = blobs[it->second.first];
        const size_t start = it->second.second;
        u32 imm;
        std::memcpy(&imm, blob.data() + start + 4, 4);
        const size_t footer = start + (size_t(imm) + 1) * 8;
        u32 bits;
        std::memcpy(&bits, blob.data() + footer + 8, 4);
        const size_t length = bits >> 8;
        if (start + length > blob.size()) return {};
        std::vector<u32> code(length / 4);
        std::memcpy(code.data(), blob.data() + start, code.size() * 4);
        return code;
    }
};

/// The permutation numbers the cache uses for a program.
std::set<size_t> UsedPerms(const fs::path& cache, u64 pgm) {
    std::set<size_t> used;
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "0x%016llx_", (unsigned long long)pgm);
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(cache, error)) {
        const auto name = entry.path().stem().string();
        if (name.rfind(prefix, 0) == 0) used.insert(std::strtoull(name.c_str() + std::strlen(prefix), nullptr, 10));
    }
    return used;
}

int SeedCache(const fs::path& self, const fs::path& catalog, const fs::path& cache, const fs::path& game,
              const fs::path& eboot) {
    std::error_code error;
    // Only into a cache made on a GPU with the catalog's profile: another profile gives other
    // specializations, and the engine would start a cache of another profile again anyway.
    const auto catalog_profile = ReadBytes(catalog / "profile.bin");
    const auto cache_profile = ReadBytes(cache / "profile.bin");
    if (catalog_profile.empty() || catalog_profile != cache_profile) {
        std::printf("Shader catalog: made on another kind of GPU than this one; not used\n");
        return 5;
    }
    CodeIndex index;
    for (const auto& entry : fs::directory_iterator(game / "dvdroot_ps4" / "shader", error)) {
        if (entry.path().extension() == ".dcx") index.Add(CodeIndex::Undcx(ReadBytes(entry.path())));
    }
    index.Add(ReadBytes(eboot));
    // Catalog permutation -> the number it gets here (a program's own permutations keep theirs).
    std::map<u64, u64> renamed_hashes;
    std::vector<std::string> added_names;
    u32 added = 0, missing = 0, present = 0, renumbered = 0;
    for (const auto& entry : fs::directory_iterator(catalog, error)) {
        if (entry.path().extension() != Native::StateExtension) continue;
        const auto name = entry.path().stem().string();
        unsigned long long pgm = 0;
        size_t perm = 0;
        if (std::sscanf(name.c_str(), "0x%llx_%zu", &pgm, &perm) != 2) continue;
        auto bytes = ReadBytes(entry.path());
        StateLayout layout;
        auto meta = ReadBytes(MetaPath(catalog, pgm, perm));
        if (!layout.Parse(bytes) || meta.size() < 24) continue;
        const auto code = index.Code(pgm);
        if (code.empty()) {
            ++missing; // not in this game's files (another edition)
            continue;
        }
        // A permutation this cache has already (same program, same specialization) is not added
        // again; a clash of numbers must not mix two specializations: the catalog's gets a free
        // number and its keys follow.
        const auto used = UsedPerms(cache, pgm);
        StageSpecialization catalog_spec;
        size_t catalog_perm = 0;
        if (!Vulkan::ReadShaderMetaSpec(std::vector<u8>(meta), catalog_spec, catalog_perm)) continue;
        const Info valid_marker{}; // StageSpecialization::Valid() only asks for an Info
        catalog_spec.info = &valid_marker;
        bool have = false;
        for (const size_t mine : used) {
            StageSpecialization spec;
            size_t mine_perm = 0;
            const auto mine_meta = ReadBytes(MetaPath(cache, pgm, mine));
            if (!mine_meta.empty() && Vulkan::ReadShaderMetaSpec(std::vector<u8>(mine_meta), spec, mine_perm)) {
                spec.info = &valid_marker;
                if (spec == catalog_spec) {
                    have = true;
                    // The catalog's keys name it by the catalog's number: they follow this one.
                    renamed_hashes[HashCombine(u64(pgm), u64(perm))] = HashCombine(u64(pgm), u64(mine));
                    break;
                }
            }
        }
        if (have) {
            ++present;
            continue;
        }
        size_t target = perm;
        if (used.count(perm)) {
            ++renumbered;
            target = *used.rbegin() + 1;
            while (used.count(target)) ++target;
        }
        if (target >= 4096) continue;
        const u64 old_hash = HashCombine(u64(pgm), u64(perm));
        const u64 new_hash = HashCombine(u64(pgm), u64(target));
        renamed_hashes[old_hash] = new_hash;
        // The state with the code from this PC's game files.
        auto header = layout.header;
        header.code_dwords = u32(code.size());
        std::vector<u8> full(bytes.begin(), bytes.begin() + layout.code_at);
        full.insert(full.end(), reinterpret_cast<const u8*>(code.data()),
                    reinterpret_cast<const u8*>(code.data() + code.size()));
        full.insert(full.end(), bytes.begin() + layout.code_at, bytes.end());
        std::memcpy(full.data(), &header, sizeof(header));
        // A geometry shader's copy shader, by the hash in its RuntimeInfo, from the game's files.
        if (HwStage(header.hw_stage) == HwStage::Geometry && CopyTrailerAt(bytes) == bytes.size()) {
            alignas(RuntimeInfo) unsigned char runtime_storage[sizeof(RuntimeInfo)];
            std::memcpy(runtime_storage, bytes.data() + sizeof(header), sizeof(RuntimeInfo));
            const auto& runtime = *std::launder(reinterpret_cast<const RuntimeInfo*>(runtime_storage));
            const auto copy = index.Code(runtime.hw.gs.vs_copy_hash);
            if (copy.empty()) {
                ++missing;
                continue;
            }
            const u32 trailer[2] = {u32(copy.size()), Native::CopyTrailerMagic};
            full.insert(full.end(), reinterpret_cast<const u8*>(copy.data()),
                        reinterpret_cast<const u8*>(copy.data() + copy.size()));
            full.insert(full.end(), reinterpret_cast<const u8*>(trailer), reinterpret_cast<const u8*>(trailer + 2));
        }
        char target_name[48];
        std::snprintf(target_name, sizeof(target_name), "0x%016llx_%zu", pgm, target);
        // The meta's permutation fields (hash at 8, number at 16) for the new number; the
        // recompile writes the rest of it again.
        std::memcpy(meta.data() + 8, &new_hash, 8);
        const u64 target64 = target;
        std::memcpy(meta.data() + 16, &target64, 8);
        if (WriteBytes(cache / (std::string(target_name) + Native::StateExtension), full) &&
            WriteBytes(MetaPath(cache, pgm, target), meta)) {
            ++added;
            added_names.push_back(target_name);
        }
    }
    // The pipeline keys, with the stage hashes of renumbered permutations replaced.
    u32 keys = 0;
    for (const auto& entry : fs::directory_iterator(catalog, error)) {
        if (entry.path().extension() != ".key" || fs::exists(cache / entry.path().filename(), error)) continue;
        auto key = ReadBytes(entry.path());
        for (const auto& [from, to] : renamed_hashes) {
            if (from == to) continue;
            for (size_t at = 0; at + 8 <= key.size(); at += 1) {
                u64 v;
                std::memcpy(&v, key.data() + at, 8);
                if (v == from) std::memcpy(key.data() + at, &to, 8);
            }
        }
        if (WriteBytes(cache / entry.path().filename(), key)) ++keys;
    }
    std::printf("Shader catalog: %u permutations added (%u renumbered), %u here already, %u not in this "
                "game's files, %u pipeline keys; compiling them\n",
                added, renumbered, present, missing, keys);
    if (added_names.empty()) {
        return 0;
    }
    const fs::path list = cache / "catalog-seed.txt";
    {
        std::ofstream out(list, std::ios::trunc);
        for (const auto& name : added_names) out << name << '\n';
    }
    const int code = RecompileCache(self, cache, list);
    fs::remove(list, error);
    return code;
}

} // namespace

int main(int argc, char** argv) {
    // Options anywhere after the positional arguments.
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--native") == 0) {
            g_native = true;
        } else if (std::strcmp(argv[i], "--spv-dir") == 0 && i + 1 < argc) {
            g_spv_dir = argv[++i];
            fs::create_directories(g_spv_dir);
        } else {
            args.push_back(argv[i]);
        }
    }
    argc = int(args.size());
    argv = args.data();
#ifdef _WIN32
    if (argc >= 3 && (std::strcmp(argv[1], "--recompile-batch") == 0 || std::strcmp(argv[1], "--one") == 0)) {
        // A child the compiler ends (an assertion's breakpoint, a fault) is gone at once: Windows'
        // error reporting took seconds for each, over a minute for a cache with 17 of them.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 0xC0DE);
            return EXCEPTION_EXECUTE_HANDLER;
        });
    }
#endif
    if (argc >= 4 && std::strcmp(argv[1], "--export-catalog") == 0) {
        return ExportCatalog(argv[2], argv[3]);
    }
    if (argc >= 6 && std::strcmp(argv[1], "--seed-cache") == 0) {
#ifdef _WIN32
        wchar_t seed_self[MAX_PATH];
        GetModuleFileNameW(nullptr, seed_self, MAX_PATH);
        return SeedCache(seed_self, argv[2], argv[3], argv[4], argv[5]);
#else
        return SeedCache(argv[0], argv[2], argv[3], argv[4], argv[5]);
#endif
    }
    if (argc >= 4 && std::strcmp(argv[1], "--recompile-batch") == 0) {
        return RecompileBatch(argv[2], std::strtoull(argv[3], nullptr, 10), argc >= 5 ? fs::path(argv[4]) : fs::path{});
    }
    if (argc >= 3 && std::strcmp(argv[1], "--recompile-cache") == 0) {
#ifdef _WIN32
        wchar_t self_path[MAX_PATH];
        GetModuleFileNameW(nullptr, self_path, MAX_PATH);
        return RecompileCache(self_path, argv[2]);
#else
        return RecompileCache(argv[0], argv[2]);
#endif
    }
    if (argc >= 3 && std::strcmp(argv[1], "--one") == 0) {
        const fs::path file = argv[2];
        return RunOne(file, file.parent_path() / "profile.bin");
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: bb-shader-lab <capture folder> [report.csv]\n");
        return 2;
    }
    const fs::path dir = argv[1];
    const fs::path report = argc >= 3 ? fs::path(argv[2]) : dir / "lab-report.csv";
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return RunAll(self, dir, report);
#else
    return RunAll(argv[0], dir, report);
#endif
}
