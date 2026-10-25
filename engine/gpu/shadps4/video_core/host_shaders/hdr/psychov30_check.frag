// SPDX-License-Identifier: MIT
// bbport: compile check of hdr/psychov30.glsl (both entry points, every option path). Not part of
// the game; compiled by .github/workflows/shader-check.yml. The numerical comparison with the
// HLSL original is docs/hdr-tools/psychov30_check (run on a PC, see its README).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "psychov30.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D tex;
layout(push_constant) uniform P {
    float peak;
    int mode;
    int boundary;
    float compression;
} pc;

void main() {
    vec3 c = texture(tex, uv).rgb * 8.0;
    vec3 a = rd_psychov30(c, pc.peak);
    vec3 b = rd_psychov30(c, pc.peak, 1.5, 1.2, 0.9, 1.1, 0.8, 1.0, vec3(0.18), vec3(0.2), 0.5,
                          pc.mode, pc.compression, pc.boundary);
    color = vec4(a + b, 1.0);
}
