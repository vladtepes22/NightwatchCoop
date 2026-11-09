// SPDX-License-Identifier: GPL-3.0-or-later
// H5 look sync, the pure part: finding a player record's appearance block ("FACE" magic), hashing it, and
// deciding when the block has really changed (settled) so the party re-sends the record. Game side: party_b2.cpp
// (LookSyncTick). Unit test: tests/test_look_sync.cpp. Setting: bbport.ini party_look_sync (off by default).
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::looksync {

inline constexpr std::size_t kFaceSize = 0x120;  // the appearance block in the save and in memory
inline constexpr double kSettleSeconds = 1.5;     // a change must hold this long (the editor writes live)

/// Offset of the first 4-aligned "FACE" magic in buf[0, len) that leaves kFaceSize bytes after it, or -1.
inline long FindFace(const std::uint8_t* buf, std::size_t len) {
    if (!buf || len < kFaceSize) return -1;
    for (std::size_t i = 0; i + kFaceSize <= len; i += 4) {
        if (buf[i] == 'F' && buf[i + 1] == 'A' && buf[i + 2] == 'C' && buf[i + 3] == 'E') return static_cast<long>(i);
    }
    return -1;
}

/// FNV-1a 64 over the block.
inline std::uint64_t HashBlock(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/// Change detector. Feed it the block's hash every tick (or Invalid() when the record is not readable /
/// the world is loading). The first valid hash after a reset is the baseline (a join or load sends the
/// record anyway). Update() returns true once when a different hash has held for kSettleSeconds.
class Detector {
public:
    void Reset() {
        have_ = false;
        pending_ = false;
    }
    bool Update(bool valid, std::uint64_t hash, double now) {
        if (!valid) {
            Reset();
            return false;
        }
        if (!have_) {
            have_ = true;
            base_ = hash;
            pending_ = false;
            return false;
        }
        if (hash == base_) {
            pending_ = false;  // back to the sent look
            return false;
        }
        if (!pending_ || hash != seen_) {
            pending_ = true;
            seen_ = hash;
            since_ = now;
            return false;
        }
        if (now - since_ >= kSettleSeconds) {
            base_ = hash;
            pending_ = false;
            return true;
        }
        return false;
    }
    bool pending() const { return pending_; }

private:
    bool have_ = false, pending_ = false;
    std::uint64_t base_ = 0, seen_ = 0;
    double since_ = 0;
};

}  // namespace coop::looksync
