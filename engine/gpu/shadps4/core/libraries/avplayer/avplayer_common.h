// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <queue>

#include "core/libraries/avplayer/avplayer.h"

#define AVPLAYER_IS_ERROR(x) ((x) < 0)

namespace Libraries::AvPlayer {

enum class AvState {
    Unknown,
    Initial,
    AddingSource,
    Ready,
    Play,
    Stop,
    EndOfFile,
    Pause,
    C0x08,
    Jump,
    TrickMode,
    C0x0B,
    Buffering,
    Starting,
    Error,
};

enum class AvEventType {
    ChangeFlowState = 21,
    WarningId = 22,
    RevertState = 30,
    AddSource = 40,
    Error = 255,
};

union AvPlayerEventData {
    u32 num_frames; // 20
    AvState state;  // AvEventType::ChangeFlowState
    s32 error;      // AvEventType::WarningId
    u32 attempt;    // AvEventType::AddSource
};

struct AvPlayerEvent {
    AvEventType event;
    AvPlayerEventData payload;
};

template <class T>
class AvPlayerQueue {
public:
    // bbport: every access holds the lock. Pop checked Size() before locking, so a Clear() from
    // the player stopping could empty the queue in between and front() read an empty deque (movie
    // audio thread crash, NW-1a98eff); Front() handed out a reference Clear() could destroy.
    size_t Size() {
        std::lock_guard guard(m_mutex);
        return m_queue.size();
    }

    void Push(T&& value) {
        std::lock_guard guard(m_mutex);
        m_queue.emplace(std::forward<T>(value));
    }

    // What fn reads from the front element, or nullopt when the queue is empty.
    template <class Fn>
    auto PeekFront(Fn&& fn) -> std::optional<decltype(fn(std::declval<const T&>()))> {
        std::lock_guard guard(m_mutex);
        if (m_queue.empty()) {
            return std::nullopt;
        }
        return fn(std::as_const(m_queue.front()));
    }

    std::optional<T> Pop() {
        std::lock_guard guard(m_mutex);
        if (m_queue.empty()) {
            return std::nullopt;
        }
        auto result = std::move(m_queue.front());
        m_queue.pop();
        return result;
    }

    void Clear() {
        std::lock_guard guard(m_mutex);
        m_queue = {};
    }

private:
    std::mutex m_mutex{};
    std::queue<T> m_queue{};
};

AvPlayerSourceType GetSourceType(std::string_view path);

} // namespace Libraries::AvPlayer
