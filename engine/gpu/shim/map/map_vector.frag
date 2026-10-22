// SPDX-License-Identifier: GPL-2.0-or-later
// The map's fragment shader: Dear ImGui's Vulkan one (texture and sampler in sets 0 and 1, the
// backend's layouts), so ImGui texture descriptors bind as they are.
#version 450
layout(location = 0) in vec4 colour;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec4 result;
layout(set = 0, binding = 0) uniform texture2D image;
layout(set = 1, binding = 0) uniform sampler linear;
void main() {
    result = colour * texture(sampler2D(image, linear), uv);
}
