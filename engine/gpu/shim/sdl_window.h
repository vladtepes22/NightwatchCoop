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
    /// A choice with a `tab` name (the community Chalice list: Blood gems, Equipment...) is shown under that tab;
    /// with two or more tabs, Left / Right (keyboard) or L1 / R1 (controller) switch tab, D-pad Left / Right and
    /// PageUp / PageDown jump ten rows.
    struct TextChoice {
        std::string value, label, tab;
    };
    /// `quiet`: the box takes the keys but draws nothing (no prompt window, no title): the caller shows the text.
    /// `limit`: the most characters (0: any).
    void BeginTextInput(const std::string& initial, const std::string& prompt, const std::vector<TextChoice>& choices = {},
                        bool quiet = false, std::size_t limit = 0);
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
    bool text_quiet{};  ///< BeginTextInput's quiet: nothing drawn
    std::size_t text_limit{};  ///< BeginTextInput's limit: the most characters (0: any)
    void AppendText(const std::string& more);  ///< `more` added up to the limit (text_mutex held)
    /// A quiet box's controller keyboard: what a press typed applied to the text (text_mutex held).
    void PadTyped(int typed);
    int pad_repeat_button{-1};      ///< the held button that types again (South: the key, West: delete), or -1
    std::uint64_t pad_repeat_at{};  ///< when it types next (SDL ticks, ms)
    int text_state{};
    std::string text, text_prompt, base_title;
    std::vector<std::pair<std::string, std::string>> text_choices;  ///< the shown tab's rows (all rows without tabs)
    std::vector<TextChoice> text_all;
    std::vector<std::string> text_tabs;  ///< tab names in order of appearance (empty: no tabs)
    int text_tab{0};
    void SelectTab(int tab);  ///< text_mutex held
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
