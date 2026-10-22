// SPDX-License-Identifier: GPL-3.0-or-later
// Byte-verified detours into the game's code for the party co-op layer (A5 and later: A6
// session persistence, B travel, C progress sync). Built like bbport_stability.cpp: every site
// is compared byte for byte with the 1.09 code before anything is written; stubs live in pages
// next to the image (runtime_low_map, rel32 reach) and are made read+execute after each install.
//
// Offsets are "our offsets": guest address - image base (= raw ELF VA of eboot.elf; bbhost
// addresses - 0x400000). Handlers are System V (the game's ABI): declare them BB_COOP_SYSV.
//
// Install hooks while the game is not running yet (bbgpu_patch_image) when possible: a site is
// written with a plain 5-byte store, which another thread could execute half-written.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#define BB_COOP_SYSV __attribute__((sysv_abi))

namespace coop {

using u8 = std::uint8_t;
using u64 = std::uint64_t;

/// Every install so far ("<name> @+0x<off>", in order; failed ones are not listed). The party
/// version check (A9, party_runtime) hashes the non-diagnostic ones: peers must run the same set.
std::vector<std::string> InstalledHooks();
/// bbgpu_patch_image calls this once every install at image patch time is done. The party version
/// check (A9) waits for it before hashing InstalledHooks(): a host whose party link started while
/// the hooks were still being installed sent an empty hook set and refused every guest (testinfra
/// 0.7.3: "party patches 000000000000... (0 hooks)" 0.1 s before the first install).
void MarkImageHooksDone();
/// Waits up to `ms` for MarkImageHooksDone; true once done. Never waits on the thread that patches
/// the image (it would only time out there).
bool WaitImageHooksDone(int ms);

/// Binds the hook helper to the loaded image (bbgpu_patch_image). Idempotent; false without an image.
bool HooksInit(unsigned char* image, u64 size);
unsigned char* Image();
u64 ImageSize();
/// The address of our offset `off` in this process (0 before HooksInit or out of the image).
u64 Guest(u64 off);

/// The image holds `n` bytes `bytes` at `off`.
bool Matches(u64 off, const u8* bytes, std::size_t n);
inline bool Matches(u64 off, std::initializer_list<u8> bytes) { return Matches(off, bytes.begin(), bytes.size()); }
/// Like Matches, but against the image as it was before our prologue hooks (HookPrologue /
/// ReplacePrologue) replaced function entries with jumps: code checks of functions we call
/// still pass once another feature hooked them.
bool MatchesOriginal(u64 off, const u8* bytes, std::size_t n);
inline bool MatchesOriginal(u64 off, std::initializer_list<u8> bytes) {
    return MatchesOriginal(off, bytes.begin(), bytes.size());
}
/// A `call rel32` (e8) at `off` to offset `target`.
bool CallsTo(u64 off, u64 target);

/// Called on function entry with the first six integer arguments (rdi, rsi, rdx, rcx, r8, r9).
/// The caller-saved registers and xmm0-7 are preserved around it; the function then runs as usual.
using PreHandler = void(BB_COOP_SYSV*)(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

/// Runs `handler` before the function at `off`. `prologue` is the function's first bytes as
/// whole, position-independent instructions (no rip-relative operands, no branches), at least
/// 5 bytes: they are compared, relocated into the stub and replaced by a jmp. False (and a log
/// line naming `name`) when the bytes differ or there is no stub memory; nothing is written then.
bool HookPrologue(u64 off, std::initializer_list<u8> prologue, PreHandler handler, const char* name);

/// Replaces the function at `off` by `handler` (same signature, System V). *original receives a
/// trampoline that runs the game's function (the relocated `prologue`, then the rest), which the
/// handler may call. Same `prologue` rules as HookPrologue.
bool ReplacePrologue(u64 off, std::initializer_list<u8> prologue, const void* handler, void** original,
                     const char* name);

/// Retargets the `call rel32` at `off` (which must call `target`) to `handler` (same signature as
/// `target`, System V); the handler may call Guest(target) itself.
bool HookCallSite(u64 off, u64 target, const void* handler, const char* name);

/// Retargets the tail jump `jmp rel32` (e9) at `off` (which must jump to `target`) to `handler`
/// (same signature as `target`, System V). The handler is entered with the original caller's
/// return address on the stack (a tail call); it may call Guest(target) itself.
bool HookTailJump(u64 off, u64 target, const void* handler, const char* name);

/// A value filter inside a function: `original` (whole, position-independent instructions, at
/// least 5 bytes, compared first) at `off` is replaced by a jmp to a stub that runs `original`,
/// saves rax..r11 and xmm0-7, runs `load_args` (sets rdi / rsi from the registers, e.g.
/// `mov rdi, r15; mov esi, eax`), calls `fn` and continues at `resume` with rax = fn's result and
/// every other register as `original` left it. The flags are not preserved.
using ValueFilter = u64(BB_COOP_SYSV*)(u64 a0, u64 a1);
bool HookValueFilter(u64 off, std::initializer_list<u8> original, std::initializer_list<u8> load_args, ValueFilter fn,
                     u64 resume, const char* name);

/// Writes `patched` over `original` at `off` (same length). True when the image now holds
/// `patched` (written, or already there); false (and a log line naming `name`) when it holds
/// neither: nothing is written then.
bool PatchBytes(u64 off, std::initializer_list<u8> original, std::initializer_list<u8> patched, const char* name);

} // namespace coop
