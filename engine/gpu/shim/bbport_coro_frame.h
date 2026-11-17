#pragma once

// bbport: owner of a nested coroutine frame in the GPU command walk (liverpool.cpp). A command
// task's final_suspend is suspend_always, so a finished frame lives until handle.destroy(). The
// nested indirect-buffer tasks (IndirectBuffer, IndirectBufferConst) were run to completion and
// then dropped without it: one leaked frame per nested buffer, every frame. The owner destroys
// the frame when its scope ends: after the run loop, and also when the outer task's frame is
// destroyed while suspended inside that loop. From Lantern v0.9.43 (Pm4Walk::OwnedFrame),
// written for our tree without its census and mutant hooks.

namespace BbCoro {

template <typename Handle>
class OwnedFrame {
public:
    explicit OwnedFrame(Handle handle) noexcept : handle_(handle) {}
    OwnedFrame(const OwnedFrame&) = delete;
    OwnedFrame& operator=(const OwnedFrame&) = delete;
    ~OwnedFrame() {
        Free();
    }

    /// Destroys the frame now; true if there was one. Later calls (and the destructor) do nothing.
    bool Free() noexcept {
        if (!handle_) {
            return false;
        }
        handle_.destroy();
        handle_ = Handle{};
        return true;
    }

private:
    Handle handle_;
};

} // namespace BbCoro
