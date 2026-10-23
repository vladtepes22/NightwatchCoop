// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

bool HandleEvent(const SDL_Event& event);
void UpdateTextInput(SDL_Window* window);

bool Visible();

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// The menu, the map view or the text dialog is open: the game's input is held neutral.
bool CapturesInput();

/// Opens or closes the map view (the pad reader on the map button; the window thread does it for
/// the keyboard). Nothing while the map is off or the menu or text dialog is open.
void ToggleMap();

/// The settings menu is open: the window shows the system cursor over it.
bool MenuOpen();

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text);

}
