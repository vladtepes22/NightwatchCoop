// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: direct calls, stage 2 (docs/EMULATION_REMOVAL_PLAN.md): the game's libGnm draw
// functions dcb.drawIndexAuto (0x10741b0), dcb.drawIndex (0x1074360) and dcb.drawIndexOffset
// (0x1074420) hand the GPU thread a record of the call instead of the PM4 it would be decoded from.
//
// BB_NATIVE_CALLS unset: nothing changes. =verify: the game's own functions still write their
// packets; a hook notes each call's record (by the address of the packet it wrote) and the GPU
// thread compares it with the decode: packet words, register effects, register checksum.
// =1: the functions are replaced: they reserve the same space through the command buffer's own
// reserve callback and write one type-3 NOP that carries the record (tag below). The GPU thread
// applies the record's register effects exactly as the draw packet would (same checksum, same
// prepared draws) and draws as for DRAW_INDEX_*. A call the record cannot express (a thread trace
// marker after the draw) keeps the original encoding.
//
// The original encodings (1.09 eboot), command buffer flags at cb+0x28:
//   drawIndexAuto(cb, count):          DRAW_INDEX_AUTO c0012d00|f&1, count, 2              + trailer
//   drawIndex(cb, count, addr):        DRAW_INDEX_2 c0042700|f&1, count, addr&~1, addr>>32, count, 0
//                                                                                          + trailer
//   drawIndexOffset(cb, offset, count): DRAW_INDEX_OFFSET_2 c0033500|f&1, count, offset, count, 0
//                                                                                          + trailer
//   trailer: NOP c0001000, 0; or (0x1089c40 true: thread trace) EVENT_WRITE c0004600|f&2, 0x35.
// Instance counts are not part of these calls (setNumInstances writes its own packet).
#pragma once

#include <cstdint>

namespace BbNativeCalls {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum class Mode { Off, Verify, Native };
/// BB_NATIVE_CALLS: unset/0 off, "verify", 1 native. Read once.
Mode GetMode();

enum DrawKind : u32 { DrawNone = 0, DrawAuto = 1, DrawIndex = 2, DrawIndexOffset = 3 };

struct DrawRecord {
    u32 kind = DrawNone;
    u32 predicate = 0; ///< the draw packet's predicate bit (command buffer flags bit 0)
    u32 index_count = 0;
    u32 index_offset = 0; ///< DrawIndexOffset
    u64 index_addr = 0;   ///< DrawIndex, as passed (the packet clears bit 0)
};

/// Dwords the game's function reserves and writes: the draw packet and the 2-dword trailer.
constexpr u32 ReservedDwords(u32 kind) {
    return kind == DrawAuto ? 5 : kind == DrawIndex ? 8 : kind == DrawIndexOffset ? 7 : 0;
}

/// The draw packet the game's function writes for `r` (no trailer); its length in dwords.
inline u32 EncodeDrawPacket(const DrawRecord& r, u32* out) {
    const u32 pred = r.predicate & 1;
    switch (r.kind) {
    case DrawAuto:
        out[0] = 0xc0012d00u | pred;
        out[1] = r.index_count;
        out[2] = 2; // draw initiator: auto index
        return 3;
    case DrawIndex:
        out[0] = 0xc0042700u | pred;
        out[1] = r.index_count; // max size
        out[2] = static_cast<u32>(r.index_addr) & ~1u;
        out[3] = static_cast<u32>(r.index_addr >> 32);
        out[4] = r.index_count;
        out[5] = 0;
        return 6;
    case DrawIndexOffset:
        out[0] = 0xc0033500u | pred;
        out[1] = r.index_count; // max size
        out[2] = r.index_offset;
        out[3] = r.index_count;
        out[4] = 0;
        return 5;
    default:
        return 0;
    }
}

/// NOP payload tag: 0xbb5d in the top half (no NOP the game or our driver writes starts so; the
/// observe statistics list uncovered NOPs by their payload's top nibble), kind in bits 0-7,
/// predicate in bit 8. The last payload dword is a check over the others, so data the game keeps
/// in NOPs (allocateFromCommandBuffer) cannot pass for a record.
constexpr u32 TagMagic = 0xbb5d0000u;
constexpr u32 TagMask = 0xffff0000u;

inline u32 RecordCheck(const u32* payload, u32 count) {
    u32 h = 0x5eedbb01u;
    for (u32 k = 0; k < count; ++k) {
        h = (h ^ payload[k]) * 16777619u;
        h ^= h >> 15;
    }
    return h;
}

/// The record NOP for `r`: ReservedDwords(r.kind) dwords (the space the original writes).
inline u32 EncodeRecordNop(const DrawRecord& r, u32* out) {
    const u32 n = ReservedDwords(r.kind);
    if (n == 0) {
        return 0;
    }
    const u32 payload = n - 1;
    out[0] = 0xc0001000u | ((n - 2) << 16); // type 3, NOP, count = payload - 1
    u32* p = out + 1;
    for (u32 k = 0; k < payload; ++k) {
        p[k] = 0;
    }
    p[0] = TagMagic | ((r.predicate & 1) << 8) | r.kind;
    p[1] = r.index_count;
    if (r.kind == DrawIndex) {
        p[2] = static_cast<u32>(r.index_addr);
        p[3] = static_cast<u32>(r.index_addr >> 32);
    } else if (r.kind == DrawIndexOffset) {
        p[2] = r.index_offset;
    }
    p[payload - 1] = RecordCheck(p, payload - 1);
    return n;
}

/// A record NOP (header at `packet`): fills `r`. Reads only inside the packet.
inline bool DecodeRecordNop(const u32* packet, DrawRecord& r) {
    const u32 header = packet[0];
    // type 3, opcode NOP; count field >= 3 (the smallest record has a 4-dword payload).
    if ((header >> 30) != 3 || ((header >> 8) & 0xff) != 0x10) {
        return false;
    }
    const u32 count = (header >> 16) & 0x3fff;
    if (count < 3) {
        return false;
    }
    const u32* p = packet + 1;
    if ((p[0] & TagMask) != TagMagic || (p[0] & 0xfe00u) != 0) {
        return false;
    }
    const u32 kind = p[0] & 0xff;
    const u32 n = ReservedDwords(kind);
    if (n == 0 || count != n - 2) {
        return false;
    }
    const u32 payload = n - 1;
    if (p[payload - 1] != RecordCheck(p, payload - 1)) {
        return false;
    }
    r.kind = kind;
    r.predicate = (p[0] >> 8) & 1;
    r.index_count = p[1];
    r.index_offset = kind == DrawIndexOffset ? p[2] : 0;
    r.index_addr = kind == DrawIndex ? (u64(p[3]) << 32 | p[2]) : 0;
    return true;
}

/// BB_NATIVE_CALLS=1: record NOPs can be in command buffers (otherwise none is looked for).
inline bool RecordsEnabled() {
    static const bool on = GetMode() == Mode::Native;
    return on;
}
/// A record NOP at `packet` (mode 1 only): fills `r`.
inline bool ReadRecord(const u32* packet, DrawRecord& r) {
    return RecordsEnabled() && DecodeRecordNop(packet, r);
}
inline bool IsRecordNop(const u32* packet) {
    DrawRecord r;
    return ReadRecord(packet, r);
}

// --- verify mode ---
/// Game threads (the hook after the original function): the record of the packet at `address`.
void Remember(u64 address, const DrawRecord& record);
/// GPU thread: the record noted for the draw packet at `address` (taken out of the table).
bool Take(u64 address, DrawRecord& record);

/// GPU thread: one decoded draw compared (ok: everything matched).
void NoteVerified(bool ok);
/// GPU thread: a direct draw without a record (written by another function or our driver).
void NoteUnrecorded();
/// GPU thread: the trailer was the thread trace EVENT_WRITE (mode 1 keeps those calls as PM4).
void NoteTraceTrailer();
/// GPU thread: prints a mismatch (rate-limited): what differed, the record, the decoded words.
void ReportMismatch(const char* what, u64 address, const DrawRecord& record, const u32* actual,
                    u32 words, u32 num_instances);

// --- mode 1 ---
/// Game threads: a call written as a record NOP, or in its original encoding (fallback).
void NoteWritten(bool as_record);
/// GPU thread: a record NOP drawn.
void NoteDecoded();

// --- BB_FRAME_STATS: GPU command thread cost per draw ---
bool TimingEnabled();
inline u64 Ticks() {
    return __builtin_ia32_rdtsc();
}
/// GPU thread: per type-3 packet (not IBs) its handling time; draws counted separately.
void AddPacketTicks(u64 ticks, bool is_draw);
/// GPU thread, after each direct draw: prints every 5 s ("Native calls: ...").
void MaybeReport();
} // namespace BbNativeCalls
