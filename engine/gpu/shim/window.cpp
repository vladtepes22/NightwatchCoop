// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <algorithm>
#include "bbport_headless.h"
// bbport: SDL3 window for the Vulkan swapchain (X11, Wayland or Win32).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "bbport_glyph_screen.h"
#include "bbport_name_entry.h"
#include "bbport_settings.h"
#include "input/input.h"
#include "lantern_pacing.h"

namespace Frontend {

#ifdef _WIN32
static HICON LoadBloodborneWindowIcon(bool small) {
    wchar_t exe_path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (!len || len >= MAX_PATH) {
        return nullptr;
    }
    const std::filesystem::path icon_path =
        std::filesystem::path(exe_path).parent_path() / L"bloodborne.ico";
    const int cx = GetSystemMetrics(small ? SM_CXSMICON : SM_CXICON);
    const int cy = GetSystemMetrics(small ? SM_CYSMICON : SM_CYICON);
    return static_cast<HICON>(LoadImageW(nullptr, icon_path.c_str(), IMAGE_ICON, cx, cy,
                                         LR_LOADFROMFILE | LR_DEFAULTCOLOR));
}

static void ApplyBloodborneWindowIcon(void* hwnd_ptr, void*& big_storage, void*& small_storage) {
    const HWND hwnd = static_cast<HWND>(hwnd_ptr);
    if (!hwnd) {
        return;
    }
    HICON big = LoadBloodborneWindowIcon(false);
    HICON small = LoadBloodborneWindowIcon(true);
    if (!big && !small) {
        return;
    }
    if (big) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        SetClassLongPtrW(hwnd, GCLP_HICON, reinterpret_cast<LONG_PTR>(big));
        big_storage = big;
    }
    if (small) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
        SetClassLongPtrW(hwnd, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small));
        small_storage = small;
    }
}
#endif

namespace {
std::atomic<u32> display_refresh_hz{0};

void UpdateDisplayRefresh(SDL_Window* window) {
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
    if (mode && mode->refresh_rate > 1.0f) {
        display_refresh_hz.store(u32(mode->refresh_rate + 0.5f), std::memory_order_relaxed);
    }
#ifdef _WIN32
    const HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (const HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
        monitor && GetMonitorInfoW(monitor, &info)) {
        LanternPacing::WatchVblank(info.szDevice);
    }
#endif
}
}

static void UpdateDisplayPeak(SDL_Window* window) {
    float peak = 0.0f;
#ifdef _WIN32
    const HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    const HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    static const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = dxgi ? reinterpret_cast<CreateFactory>(
                                   reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                             : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (monitor && create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; peak == 0.0f && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            IDXGIOutput* output = nullptr;
            for (UINT o = 0; peak == 0.0f && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
                IDXGIOutput6* output6 = nullptr;
                DXGI_OUTPUT_DESC1 desc{};
                if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6)))) {
                    if (SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor) {
                        peak = desc.MaxLuminance;
                    }
                    output6->Release();
                }
                output->Release();
            }
            adapter->Release();
        }
        factory->Release();
    }
#else
    (void)window;
#endif
    auto& settings = BbSettings::Get();
    if (peak != settings.hdr_display_peak.exchange(peak, std::memory_order_relaxed) && peak > 0.0f) {
        std::printf("HDR output: the display reports a peak of %.0f nits%s\n", peak,
                    settings.hdr_peak_auto.load() ? " (used: peak from the display)" : "");
    }
}

u32 DisplayRefreshHz() {
    return display_refresh_hz.load(std::memory_order_relaxed);
}

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY, "1");
    // DirectInput's device scan, run on every plug or unplug, waits on each HID device; one that
    // stops answering froze this thread and the game (dinput8 -> hid.dll). XInput and HIDAPI pads
    // don't need it; SDL_JOYSTICK_DIRECTINPUT=1 in the environment brings it back.
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_SetHint(SDL_HINT_WINDOW_ALLOW_TOPMOST, "0");
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    if (const char* selection = std::getenv("BB_MONITOR_BOUNDS"); selection && *selection) {
        int x, y, w, h;
        char trailing;
        if (SDL_sscanf(selection, "%d,%d,%d,%d%c", &x, &y, &w, &h, &trailing) == 4 &&
            x >= -100000 && x <= 100000 && y >= -100000 && y <= 100000 &&
            w > 0 && w <= 100000 && h > 0 && h <= 100000) {
            SDL_Point point{x + w / 2, y + h / 2};
            const SDL_DisplayID selected = SDL_GetDisplayForPoint(&point);
            SDL_Rect bounds{};
            if (selected && SDL_GetDisplayBounds(selected, &bounds) &&
                bounds.x == x && bounds.y == y && bounds.w == w && bounds.h == h) {
                SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(selected));
                SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(selected));
                const char* label = std::getenv("BB_MONITOR_LABEL");
                SDL_Log("Monitor selection: %s; SDL display %u (%d,%d %dx%d)",
                        label ? label : "selected monitor", static_cast<unsigned>(selected), x, y, w, h);
            } else {
                SDL_Log("Monitor selection: display disconnected; using primary monitor");
            }
        } else {
            SDL_Log("Monitor selection: invalid bounds; using primary monitor");
        }
    }
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                           fullscreen ? fullscreen[0] == '1' : BbSettings::Get().fullscreen.load());
    base_title = title;
    if (BbHeadless::On()) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN,"0");
        SDL_SetBooleanProperty(props,SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,false);
        SDL_SetBooleanProperty(props,SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN,true);
        SDL_SetBooleanProperty(props,SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN,false);
    }
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    if (BbHeadless::On()) {
        ASSERT_MSG(SDL_GetWindowFlags(window)&SDL_WINDOW_HIDDEN,"Headless window became visible");
        std::puts("Headless: hidden, unfocusable surface; graphics draws and presentation disabled");
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        ApplyBloodborneWindowIcon(window_info.render_surface, win_icon_big, win_icon_small);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        if (!SDL_SyncWindow(window)) {
            std::printf("Display: the Wayland compositor has not configured the window yet: %s\n", SDL_GetError());
        }
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    UpdateDisplayRefresh(window);
    UpdateDisplayPeak(window);
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
#ifdef _WIN32
    if (win_icon_big) {
        DestroyIcon(static_cast<HICON>(win_icon_big));
    }
    if (win_icon_small) {
        DestroyIcon(static_cast<HICON>(win_icon_small));
    }
#endif
}

namespace {
std::atomic<bool> window_unfocused{false}, window_minimized{false};
}

bool WindowInBackground() {
    return window_unfocused.load(std::memory_order_relaxed) ||
           window_minimized.load(std::memory_order_relaxed);
}

u32 BackgroundFrameLimit() {
    static const u32 limit = [] {
        const char* online = std::getenv("BB_ONLINE");
        if (online && online[0] == '1') return 0u;
        const char* env = std::getenv("BB_BACKGROUND_FPS");
        return env && env[0] ? u32(std::clamp(std::atoi(env), 0, 240)) : 30u;
    }();
    return limit;
}

void WindowSDL::SelectTab(int tab) {
    text_tab = tab;
    text_choices.clear();
    for (const TextChoice& c : text_all) {
        if (text_tabs.empty() || c.tab == text_tabs[size_t(tab)]) text_choices.emplace_back(c.value, c.label);
    }
    text_sel = text_choices.empty() ? -1 : 0;
    if (text_sel == 0) text = text_choices[0].first;
    if (!text_tabs.empty()) {
        std::printf("[coop] Party: chalice: glyph list tab -> %s (%zu rows)\n", text_tabs[size_t(tab)].c_str(), text_choices.size());
        std::fflush(stdout);
    }
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt, const std::vector<TextChoice>& choices,
                               bool quiet, std::size_t limit) {
    std::string keys;
    {
        std::scoped_lock lock{text_mutex};
        text_quiet = quiet;
        text_limit = limit;
        text.clear();
        AppendText(initial);
        text_selected = !quiet && choices.empty() && !initial.empty();  // quiet: nothing shows a selection
        text_prompt = prompt;
        text_state = 0;
        text_all = choices;
        text_tabs.clear();
        for (const TextChoice& c : choices) {
            if (!c.tab.empty() && std::find(text_tabs.begin(), text_tabs.end(), c.tab) == text_tabs.end()) text_tabs.push_back(c.tab);
        }
        for (const TextChoice& c : choices) {  // a row without a tab joins a tab of its own at the front
            if (c.tab.empty() && !text_tabs.empty()) {
                text_tabs.insert(text_tabs.begin(), "Party");
                for (TextChoice& a : text_all) a.tab = a.tab.empty() ? "Party" : a.tab;
                break;
            }
        }
        if (text_tabs.size() < 2) text_tabs.clear();  // one tab is no tab
        text_choices.clear();
        text_tab = 0;
        SelectTab(0);  // the first choice is the answer until the player moves or types
        text_requested = true;
        keys.swap(text_test_keys);
    }
    if (!keys.empty()) TestType(keys);
}

// A held key on a controller keyboard: types again after this pause, then at this rate (about a keyboard's own).
constexpr std::uint64_t kPadRepeatDelayMs = 500, kPadRepeatRateMs = 50;

void WindowSDL::AppendText(const std::string& more) {
    if (!text_limit) {
        text += more;
        return;
    }
    std::size_t count = 0;  // characters (UTF-8 lead bytes)
    for (const unsigned char c : text) count += (c & 0xc0) != 0x80;
    for (std::size_t i = 0; i < more.size();) {
        std::size_t n = 1;
        while (i + n < more.size() && (static_cast<unsigned char>(more[i + n]) & 0xc0) == 0x80) ++n;
        if (count >= text_limit) break;
        text.append(more, i, n);
        ++count;
        i += n;
    }
}

void WindowSDL::QueueTestKeys(const std::string& keys) {
    bool now = false;
    {
        std::scoped_lock lock{text_mutex};
        now = text_active;  // a box that is open already takes them at once (a slow pick)
        if (!now) text_test_keys += keys;
    }
    if (now) TestType(keys);
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    if (text_quiet) {  // the caller shows the text (the Chalice glyph screen's search bar)
        SDL_SetWindowTitle(window, base_title.c_str());
        BbOverlay::SetTextPrompt(false, text_prompt, text);
        return;
    }
    std::string prompt_lower=text_prompt;
    std::transform(prompt_lower.begin(),prompt_lower.end(),prompt_lower.begin(),[](unsigned char c){ return char(std::tolower(c)); });
    const std::string body=prompt_lower.find("password")!=std::string::npos ? std::string(text.size(),'*') : text;
    const std::string shown = text_selected ? "[" + body + "]" : body + "_";
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + shown + "  (Enter = OK, Esc = cancel, Ctrl+V = paste)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextPrompt(text_active, text_prompt, shown);
    UpdateChoices();
}

void WindowSDL::UpdateChoices() {
    std::vector<std::string> lines;
    if (text_active) {
        for (const auto& c : text_choices) lines.push_back(c.first + "   " + c.second);
    }
    BbOverlay::SetTextChoices(lines, text_sel, text_active ? text_tabs : std::vector<std::string>{}, text_tab);
}

bool WindowSDL::IsTextInputActive() {
    std::scoped_lock guard{text_mutex};
    return text_requested || text_active;
}

void WindowSDL::TestType(const std::string& keys) {
    static const auto ascii=[] {
        std::array<std::array<char,2>,128> chars{};
        for (int i=0;i<128;++i) chars[i][0]=char(i);
        return chars;
    }();
    for (unsigned char c:keys) {
        SDL_Event event{};
        if (c=='\n' || c=='\b' || c==27 || c==1 || c==2) {
            event.type=SDL_EVENT_KEY_DOWN;
            event.key.windowID=SDL_GetWindowID(window);
            event.key.key=c=='\n' ? SDLK_RETURN : c=='\b' ? SDLK_BACKSPACE : c==1 ? SDLK_UP : c==2 ? SDLK_DOWN : SDLK_ESCAPE;
            event.key.down=true;
        } else if(c>=32 && c<127) {
            event.type=SDL_EVENT_TEXT_INPUT;
            event.text.windowID=SDL_GetWindowID(window);
            event.text.text=ascii[c].data();
        } else continue;
        SDL_PushEvent(&event);
    }
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) {
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    char search_letter = 0;  // the letter that started a search (already its text: its text event, if any, is skipped)
    while (SDL_PollEvent(&event)) {
        // The Chalice glyph screen: a letter or digit starts its search with that letter, before the game sees the key.
        if (!text_active && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) && !BbOverlay::CapturesInput()) {
            const SDL_Keycode k = event.key.key;
            const char c = k >= SDLK_A && k <= SDLK_Z   ? char('a' + (k - SDLK_A))
                           : k >= SDLK_0 && k <= SDLK_9 ? char('0' + (k - SDLK_0))
                                                        : 0;
            if (c && BbGlyphScreen::TypeStartsSearch(int(event.key.scancode))) {
                BeginTextInput(BbGlyphScreen::SearchText() + c, "Search", {}, true);
                std::scoped_lock lock{text_mutex};
                text_requested = false;
                text_active = true;
                SDL_StartTextInput(window);
                UpdateTextTitle();
                search_letter = c;
                continue;
            }
        }
        BbInput::Event(event,text_active || BbOverlay::CapturesInput());
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            last_mouse_motion_ms = SDL_GetTicks();
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            if (text_quiet && BbNameEntry::Typing()) BbNameEntry::KeyboardKeyTyped();  // the name keyboard hides
            else if (text_quiet) BbGlyphScreen::KeyboardKeyTyped();  // the glyph screen's controller keyboard hides
            std::scoped_lock lock{text_mutex};
            const bool ctrl = event.type == SDL_EVENT_KEY_DOWN && (event.key.mod & SDL_KMOD_CTRL);
            if (event.type == SDL_EVENT_TEXT_INPUT && search_letter &&
                std::tolower(static_cast<unsigned char>(event.text.text[0])) == search_letter && !event.text.text[1]) {
                search_letter = 0;
            } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                if (text_selected || text_sel >= 0) text.clear();  // the list's answer gives way to typing
                text_selected = false;
                text_sel = -1;
                AppendText(event.text.text);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !text_choices.empty() && event.key.key == SDLK_UP) {
                text_sel = text_sel <= 0 ? int(text_choices.size()) - 1 : text_sel - 1;
                text = text_choices[size_t(text_sel)].first;
                text_selected = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !text_choices.empty() && event.key.key == SDLK_DOWN) {
                text_sel = text_sel + 1 >= int(text_choices.size()) ? 0 : text_sel + 1;
                text = text_choices[size_t(text_sel)].first;
                text_selected = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !text_tabs.empty() &&
                       (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT)) {
                const int n = int(text_tabs.size());
                SelectTab((text_tab + (event.key.key == SDLK_LEFT ? n - 1 : 1)) % n);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !text_choices.empty() &&
                       (event.key.key == SDLK_PAGEUP || event.key.key == SDLK_PAGEDOWN)) {
                const int n = int(text_choices.size());  // a long list (the community glyphs): ten rows at a time
                text_sel = event.key.key == SDLK_PAGEUP ? std::max(0, (text_sel < 0 ? 0 : text_sel) - 10)
                                                         : std::min(n - 1, (text_sel < 0 ? 0 : text_sel) + 10);
                text = text_choices[size_t(text_sel)].first;
                text_selected = false;
            } else if (ctrl && event.key.key == SDLK_V) {
                if (char* clip = SDL_GetClipboardText()) {
                    std::string pasted;
                    for (const char* c = clip; *c; ++c) {
                        if (*c != '\r' && *c != '\n' && *c != '\t') pasted += *c;
                    }
                    SDL_free(clip);
                    if (text_selected || text_sel >= 0) text.clear();
                    text_selected = false;
                    text_sel = -1;
                    AppendText(pasted);
                }
            } else if (ctrl && event.key.key == SDLK_A) {
                text_selected = !text.empty();
            } else if (event.key.key == SDLK_BACKSPACE && text_selected) {
                text.clear();
                text_selected = false;
                text_sel = -1;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                text_sel = -1;  // edit the picked glyph by hand
                size_t cut = text.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // A pick list (Chalice glyphs): the d-pad moves the selection (a controller has no arrow keys here).
        if (text_active && !text_choices.empty() && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP || event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) {
            std::scoped_lock lock{text_mutex};
            const int n = int(text_choices.size());
            const bool shoulder = event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
            const bool side = event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
            const bool left = event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT;
            if (!text_tabs.empty() && shoulder) {  // tabs: L1 / R1 switch the tab
                const int t = int(text_tabs.size());
                SelectTab((text_tab + (left ? t - 1 : 1)) % t);
                UpdateTextTitle();
                continue;
            }
            if (side && text_tabs.empty()) continue;  // no tabs: Left / Right do nothing
            if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) text_sel = text_sel <= 0 ? n - 1 : text_sel - 1;
            else if (left) text_sel = std::max(0, (text_sel < 0 ? 0 : text_sel) - 10);  // L1 / D-pad Left: ten rows up
            else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER || event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
                text_sel = std::min(n - 1, (text_sel < 0 ? 0 : text_sel) + 10);
            else text_sel = text_sel + 1 >= n ? 0 : text_sel + 1;
            text = text_choices[size_t(text_sel)].first;
            text_selected = false;
            UpdateTextTitle();
            continue;
        }
        // A quiet box (the Chalice glyph screen's search): the controller types on the screen's keyboard (the d-pad
        // moves on it, South presses the key, West deletes, Start is Done, East cancels, North switches its view).
        if (text_active && text_quiet && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            using BbGlyphScreen::PadKey;
            const auto b = event.gbutton.button;
            const PadKey key = b == SDL_GAMEPAD_BUTTON_DPAD_UP      ? PadKey::kUp
                               : b == SDL_GAMEPAD_BUTTON_DPAD_DOWN  ? PadKey::kDown
                               : b == SDL_GAMEPAD_BUTTON_DPAD_LEFT  ? PadKey::kLeft
                               : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? PadKey::kRight
                               : b == SDL_GAMEPAD_BUTTON_SOUTH      ? PadKey::kPress
                               : b == SDL_GAMEPAD_BUTTON_WEST       ? PadKey::kDelete
                               : b == SDL_GAMEPAD_BUTTON_START      ? PadKey::kDone
                               : b == SDL_GAMEPAD_BUTTON_EAST       ? PadKey::kCancel
                               : b == SDL_GAMEPAD_BUTTON_NORTH      ? PadKey::kToggle
                                                                    : PadKey::kNone;
            const int typed = BbNameEntry::Typing() ? BbNameEntry::KeyboardPress(key) : BbGlyphScreen::KeyboardPress(key);
            std::scoped_lock lock{text_mutex};
            PadTyped(typed);
            // held, a press that typed or deleted repeats (as a held key on a keyboard); any other press stops it
            const bool repeats = (key == PadKey::kPress || key == PadKey::kDelete) &&
                                 (typed > 0 || typed == BbGlyphScreen::kTypedDelete);
            pad_repeat_button = repeats ? int(b) : -1;
            pad_repeat_at = SDL_GetTicks() + kPadRepeatDelayMs;
            UpdateTextTitle();
            continue;
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP && int(event.gbutton.button) == pad_repeat_button) {
            pad_repeat_button = -1;
        }
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            window_unfocused.store(true, std::memory_order_relaxed);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            window_unfocused.store(false, std::memory_order_relaxed);
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            window_minimized.store(true, std::memory_order_relaxed);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            window_minimized.store(false, std::memory_order_relaxed);
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            UpdateDisplayRefresh(window);
            UpdateDisplayPeak(window);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_F11 && !event.key.repeat) {
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            }
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        bool unfocused = !(flags & SDL_WINDOW_INPUT_FOCUS);
        bool minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
#ifdef _WIN32
        if (const auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(
                SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))) {
            unfocused = GetForegroundWindow() != hwnd;
            minimized = IsIconic(hwnd) != FALSE;
        }
#endif
        window_unfocused.store(unfocused, std::memory_order_relaxed);
        window_minimized.store(minimized, std::memory_order_relaxed);
    }
    // A held Cross / Square on a controller keyboard: the key again, after a pause, then at a steady rate.
    if (pad_repeat_button >= 0 && (!text_active || !text_quiet)) pad_repeat_button = -1;
    if (pad_repeat_button >= 0 && SDL_GetTicks() >= pad_repeat_at) {
        pad_repeat_at = SDL_GetTicks() + kPadRepeatRateMs;
        using BbGlyphScreen::PadKey;
        const PadKey key = pad_repeat_button == SDL_GAMEPAD_BUTTON_SOUTH ? PadKey::kPress : PadKey::kDelete;
        const int typed = BbNameEntry::Typing() ? BbNameEntry::KeyboardPress(key) : BbGlyphScreen::KeyboardPress(key);
        std::scoped_lock lock{text_mutex};
        if (typed > 0 || typed == BbGlyphScreen::kTypedDelete) PadTyped(typed);
        else pad_repeat_button = -1;  // the key under the cursor no longer types (cannot happen: the cursor holds)
        UpdateTextTitle();
    }
    BbInput::Pump(window,text_active);
    UpdateCursor();
    return is_open;
}

void WindowSDL::PadTyped(int typed) {
    if (typed > 0) {
        AppendText(std::string(1, char(typed)));
    } else if (typed == BbGlyphScreen::kTypedDelete && !text.empty()) {
        size_t cut = text.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.erase(cut);
    } else if (typed == BbGlyphScreen::kTypedDone || typed == BbGlyphScreen::kTypedCancel) {
        text_state = typed == BbGlyphScreen::kTypedDone ? 1 : 2;
        text_active = false;
        pad_repeat_button = -1;
        SDL_StopTextInput(window);
    }
}

void WindowSDL::UpdateCursor() {
    if (SDL_GetWindowRelativeMouseMode(window)) {
        cursor_hidden = false;
        return;
    }
    const bool hide = BbSettings::Get().hide_cursor && !BbOverlay::MenuOpen() &&
                      SDL_GetTicks() - last_mouse_motion_ms > 2000;
    if (hide != cursor_hidden) {
        cursor_hidden = hide;
        hide ? SDL_HideCursor() : SDL_ShowCursor();
    }
}

}
