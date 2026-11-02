#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

bool WindowInBackground();
u32 BackgroundFrameLimit();
u32 DisplayRefreshHz();

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    bool PollEvents();
    /// `choices`: a pick list (value, label) shown above the box - Up / Down (keyboard, d-pad) move the
    /// selection, which sets the text, Enter / Cross confirm; typing or Backspace leaves the list for the
    /// keyboard. Empty: the plain box. (Chalice glyphs, docs/party/chalice.md 11.)
    void BeginTextInput(const std::string& initial, const std::string& prompt,
                        const std::vector<std::pair<std::string, std::string>>& choices = {});
    /// Test aid (any thread): keys (TestType's) the next text box takes as soon as it opens.
    void QueueTestKeys(const std::string& keys);
    int PollTextInput(std::string& text);
    bool IsTextInputActive();
    void TestType(const std::string& keys);

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    bool text_selected{};
    int text_state{};
    std::string text, text_prompt, base_title;
    std::vector<std::pair<std::string, std::string>> text_choices;
    int text_sel{-1};  ///< the selected choice, -1: typing
    std::string text_test_keys;
    void UpdateChoices();
    void UpdateTextTitle();
    void UpdateCursor();
    u64 last_mouse_motion_ms{};
    bool cursor_hidden{};
    SDL_Window* window{};
    WindowSystemInfo window_info{};
#ifdef _WIN32
    void* win_icon_big{};
    void* win_icon_small{};
#endif
};

}
