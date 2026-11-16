// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: pure rules of three renderer fixes ported from deadinside28/bloodborne_pc 0fc2921c
// (GPL-2.0-or-later), kept free of Vulkan so tests/test_gpu_logic.cpp can run them:
//  - which layer of an array / cube image a layer-sized byte range is (the point lights' shadow
//    cube is cleared one face at a time: wedges of light),
//  - which of the game's occlusion queries are open (samples are counted only inside them),
//  - when a CPU mark on a memory block is stale (FPS drop after switching the save).
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace BbGpuLogic {

/// The layer `size` bytes at `address` are exactly, in an image of `layers` layers (one mip)
/// at `image_address` of `image_size` bytes; -1 when they are no single layer.
inline int LayerOfRange(std::uint64_t address, std::uint64_t size, std::uint64_t image_address,
                        std::uint64_t image_size, std::uint32_t layers) {
    if (layers < 2 || image_size % layers != 0 || address < image_address) {
        return -1;
    }
    const std::uint64_t layer_size = image_size / layers;
    const std::uint64_t offset = address - image_address;
    if (size != layer_size || offset % layer_size != 0 || offset / layer_size >= layers) {
        return -1;
    }
    return int(offset / layer_size);
}

/// The game's open occlusion queries: a ZPASS_DONE at a 16-byte aligned address begins one (its
/// counters), the same query ends with an event 8 bytes after. Events that do not pair up turn
/// the tracking off (samples counted all the time, as before).
class OcclusionIntervals {
public:
    explicit OcclusionIntervals(bool enabled = true) : enabled_(enabled) {}
    bool Counting() const {
        return !enabled_ || !open_.empty();
    }
    bool Enabled() const {
        return enabled_;
    }
    /// False when the event did not fit the begin/end layout (tracking is then off).
    bool Note(std::uint64_t address) {
        if (!enabled_) {
            return true;
        }
        if (address % 16 == 0 && open_.size() < 256) {
            open_.push_back(address);
            return true;
        }
        if (address % 16 == 8) {
            if (const auto it = std::find(open_.begin(), open_.end(), address - 8);
                it != open_.end()) {
                open_.erase(it);
                return true;
            }
        }
        enabled_ = false;
        open_.clear();
        return false;
    }

private:
    bool enabled_;
    std::vector<std::uint64_t> open_;
};

/// A CPU mark on a memory block is dropped when a whole GPU write meets it and it is neither
/// recent, nor on a block the CPU keeps writing (hot), nor dynamic by the CPU's writes, nor watched.
inline bool CpuMarkStale(std::uint32_t now, std::uint32_t marked, std::uint32_t seconds, bool hot,
                         bool dynamic, bool dynamic_by_gpu_aged, bool watched) {
    return now - marked >= seconds && !hot && !(dynamic && !dynamic_by_gpu_aged) && !watched;
}

} // namespace BbGpuLogic
