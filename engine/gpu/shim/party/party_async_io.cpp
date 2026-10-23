// SPDX-License-Identifier: GPL-3.0-or-later
// Background file writer for the party layer (see party_async_io.h).
#include "party_async_io.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

namespace coop::asyncio {
namespace {

struct Job {
    bool print = false; // Print: `data` goes to stdout
    std::string path;   // Replace: the target; empty for Run / Print
    std::shared_ptr<std::string> data;
    std::function<void()> fn;
};

struct Queue {
    std::mutex mu;
    std::condition_variable cv, idle;
    std::deque<Job> q;
    bool busy = false;
    bool started = false;
    std::size_t lines = 0;   // queued Print jobs
    std::size_t dropped = 0; // Print jobs refused since the last one that fit
};

Queue& Q() {
    static Queue* q = new Queue; // never destroyed: the writer may outlive static destruction
    return *q;
}

void WriteReplace(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << data;
    }
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}

void WriterMain() {
    Queue& q = Q();
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(q.mu);
            q.busy = false;
            if (q.q.empty()) {
                q.idle.notify_all();
            }
            q.cv.wait(lk, [&] { return !q.q.empty(); });
            j = std::move(q.q.front());
            q.q.pop_front();
            q.busy = true;
            if (j.print) {
                --q.lines;
            }
        }
        if (j.print) {
            std::fputs(j.data->c_str(), stdout);
            std::fflush(stdout);
        } else if (j.fn) {
            j.fn();
        } else {
            WriteReplace(j.path, *j.data);
        }
    }
}

void Push(Job j) {
    Queue& q = Q();
    {
        std::lock_guard<std::mutex> lk(q.mu);
        if (!q.started) {
            q.started = true;
            std::thread(WriterMain).detach();
            std::atexit([] { Drain(2000); });
        }
        if (j.print) {
            if (q.lines >= kMaxQueuedLines) {
                ++q.dropped;
                return;
            }
            if (q.dropped) {
                Job d;
                d.print = true;
                d.data = std::make_shared<std::string>("Party: " + std::to_string(q.dropped) +
                                                       " log lines dropped (stdout stalled)\n");
                q.dropped = 0;
                q.q.push_back(std::move(d));
                ++q.lines;
            }
            ++q.lines;
        } else if (!j.path.empty()) {
            for (Job& w : q.q) {
                if (!w.fn && !w.print && w.path == j.path) {
                    w.data = std::move(j.data); // still waiting: write the newer text instead
                    return;
                }
            }
        }
        q.q.push_back(std::move(j));
    }
    q.cv.notify_one();
}

} // namespace

void Replace(const std::string& path, std::string data) {
    Job j;
    j.path = path;
    j.data = std::make_shared<std::string>(std::move(data));
    Push(std::move(j));
}

std::atomic<LineTagger> g_tagger{nullptr};

void SetLineTagger(LineTagger fn) { g_tagger.store(fn); }

void Print(std::string text) {
    if (LineTagger tag = g_tagger.load(std::memory_order_relaxed)) {
        text = tag(text);
    }
    // Every co-op line starts with kLogPrefix, so forks and readers tell it from the base port's lines.
    if (text.compare(0, kLogPrefix.size(), kLogPrefix) != 0) text.insert(0, kLogPrefix);
    Job j;
    j.print = true;
    j.data = std::make_shared<std::string>(std::move(text));
    Push(std::move(j));
}

void Run(std::function<void()> job) {
    Job j;
    j.fn = std::move(job);
    Push(std::move(j));
}

bool Drain(int timeout_ms) {
    Queue& q = Q();
    std::unique_lock<std::mutex> lk(q.mu);
    if (!q.started) {
        return true;
    }
    return q.idle.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return q.q.empty() && !q.busy; });
}

} // namespace coop::asyncio
