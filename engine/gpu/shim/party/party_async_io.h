// SPDX-License-Identifier: GPL-3.0-or-later
// File writes the game's main thread hands off (party_story.json, BBPF flag dumps): one background
// writer thread, so a slow disk, an antivirus scan or a full pipe never stalls a frame. Jobs run
// in queue order; a queued replace of a path that is queued again is dropped (the newer text wins:
// the same file as writing both in turn). A normal exit drains the queue (atexit, up to 2 s).
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace coop::asyncio {

/// Start of every co-op log line (Print adds it; direct printf sites write it themselves).
inline const std::string kLogPrefix = "[coop] ";

/// Replaces `path` with `data` the way the callers did on the main thread: writes `path`.tmp, then
/// removes `path` and renames the .tmp over it. A later Replace of the same path while this one
/// still waits supersedes it.
void Replace(const std::string& path, std::string data);

/// Runs `job` on the writer thread (after everything queued before it).
void Run(std::function<void()> job);

/// Writes `text` to stdout (then flushes) on the writer thread, in call order with every other
/// Print: the party's log lines. A stalled stdout (a full pipe, a slow disk) never blocks the
/// caller; past kMaxQueuedLines waiting lines new ones are dropped and counted (reported with the
/// next line that fits).
void Print(std::string text);
/// Party logging (party_log.h, BB_PARTY_LOG=verbose): rewrites each Print's text on the caller's
/// thread (the correlation tag), before it is queued. nullptr = none (the default).
using LineTagger = std::string (*)(const std::string& text);
void SetLineTagger(LineTagger fn);
constexpr std::size_t kMaxQueuedLines = 20000;

/// Waits until the queue is empty (tests, shutdown); false on timeout.
bool Drain(int timeout_ms);

} // namespace coop::asyncio
