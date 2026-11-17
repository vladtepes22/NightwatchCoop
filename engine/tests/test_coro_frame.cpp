// bbport: the nested command-task frames of the GPU command walk are freed exactly once
// (shim/bbport_coro_frame.h). The task type has the shape of Liverpool::Task: suspended at start
// and at the end, so a finished frame lives until it is destroyed. Frames are counted through the
// promise's constructor and destructor (allocation counts can be elided by the optimiser).

#include <cassert>
#include <coroutine>
#include <cstdio>

#include "bbport_coro_frame.h"

namespace {

int g_created = 0;
int g_destroyed = 0;

struct Task {
    struct promise_type {
        promise_type() {
            ++g_created;
        }
        ~promise_type() {
            ++g_destroyed;
        }
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept {
            return {};
        }
        std::suspend_always final_suspend() noexcept {
            return {};
        }
        void unhandled_exception() {}
        void return_void() {}
        struct empty {};
        std::suspend_always yield_value(empty&&) {
            return {};
        }
    };
    using Handle = std::coroutine_handle<promise_type>;
    Handle handle;
};

// A nested buffer: yields a few times, then ends.
Task Nested(int yields) {
    for (int i = 0; i < yields; ++i) {
        co_yield {};
    }
}

// The walk's pattern (liverpool.cpp IndirectBuffer): run the nested task, yielding while it does.
Task Outer(int buffers, int yields) {
    for (int b = 0; b < buffers; ++b) {
        auto task = Nested(yields);
        BbCoro::OwnedFrame nested{task.handle};
        task.handle.resume();
        while (!task.handle.done()) {
            co_yield {};
            task.handle.resume();
        }
    }
}

void Reset() {
    g_created = 0;
    g_destroyed = 0;
}

int Live() {
    return g_created - g_destroyed;
}

int failures = 0;
void Expect(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

} // namespace

int main() {
    // 1. Normal completion: the outer task and its 3 nested ones, all freed.
    Reset();
    {
        auto outer = Outer(3, 2);
        while (!outer.handle.done()) {
            outer.handle.resume();
        }
        Expect(g_created == 4 && Live() == 1, "nested frames freed when each run ends");
        outer.handle.destroy();
        Expect(Live() == 0, "normal completion: every frame freed");
    }

    // 2. Early exit: the outer frame destroyed while suspended inside the run loop.
    Reset();
    {
        auto outer = Outer(3, 5);
        outer.handle.resume(); // in the first nested buffer's loop
        outer.handle.resume();
        Expect(g_created == 2 && Live() == 2, "suspended inside the loop: outer + nested live");
        outer.handle.destroy();
        Expect(Live() == 0, "early exit: the nested frame goes with the outer one");
    }

    // 3. Freed by hand, then the owner's destructor: one destroy only.
    Reset();
    {
        auto task = Nested(0);
        BbCoro::OwnedFrame nested{task.handle};
        task.handle.resume();
        Expect(nested.Free(), "Free() destroys the frame");
        Expect(!nested.Free(), "a second Free() does nothing");
    }
    Expect(g_created == 1 && g_destroyed == 1, "explicit free + destructor: destroyed exactly once");

    // 4. An owner of no frame does nothing.
    Reset();
    {
        BbCoro::OwnedFrame<Task::Handle> none{Task::Handle{}};
    }
    Expect(g_destroyed == 0, "empty owner destroys nothing");

    std::printf(failures == 0 ? "coro-frame-test: all passed\n" : "coro-frame-test: %d failed\n",
                failures);
    return failures == 0 ? 0 : 1;
}
