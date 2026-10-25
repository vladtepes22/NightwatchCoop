// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport HDR: replacement for Bloodborne's tonemap pixel shader (guest ps 0x5c02eaf8, first draw of
// the pass that writes the 1920x1080 UI target). Its interface (inputs, descriptor bindings, push
// constants) is the one bbport's recompiler gives the original, so the pipeline layout is
// unchanged; the C++ side only substitutes it when the original's SPIR-V is the expected one.
//
// The game's math, as recovered from the original:
//   x     = exposure tint from img0 (1x1), neutralized by the constants (x = 1)
//   v     = x * scene * c[616]                          scene = img1 (HDR, RGBA16F)
//   T     = y * (c[833] * (y - 1) + 1)^2, y = 1 - exp(-max(v, 2^-14) * c[832])
//   U     = clamp(c[617] * (bloom * x + B))             bloom = img2, B: second neutralized path
//   S     = T + U - T * U                               (screen blend)
//   out   = pow(M * S, c[808]) + c[809] * grain + c[810]   M = 3x4 matrix c[840..851], BGR order
//   alpha = clamp(dot(c[620..623], scene))
//
// Specialization constant 0 (mode): 0 = the game's tonemap, 1 = the same with the left half of the
// screen tinted (to check that the replacement is active).

#version 450
#extension GL_EXT_fragment_shader_barycentric : require

layout(constant_id = 0) const int BB_TONEMAP_MODE = 0;

layout(set = 0, binding = 0, std430) readonly buffer ssbo_1 {
    uint data[];
} ssbo_1_1;

// The recompiler's push constants (shader_recompiler/resource.h, PushData) at the C++ struct's
// offsets (checked in vk_rasterizer.cpp); bb_hdr is bbport's, set per draw (vk_rasterizer.cpp).
layout(push_constant, std430) uniform AuxData {
    layout(offset = 0) float xoffset;
    layout(offset = 4) float yoffset;
    layout(offset = 8) float xscale;
    layout(offset = 12) float yscale;
    layout(offset = 80) uvec4 buf_offsets0;
    layout(offset = 96) uvec4 buf_offsets1;
    layout(offset = 112) uvec2 buf_offsets2;
    layout(offset = 120) uint motion_param;
    layout(offset = 124) uint scene_size;
    layout(offset = 128) uvec4 bb_hdr;
} push_data;

layout(set = 0, binding = 1) uniform texture2D fs_img0; // exposure tint, 1x1
layout(set = 0, binding = 2) uniform texture2D fs_img1; // scene, HDR
layout(set = 0, binding = 3) uniform texture2D fs_img2; // bloom
layout(set = 0, binding = 4) uniform texture2D fs_img3; // grain, 4x4
layout(set = 0, binding = 5) uniform sampler fs_samp0;
layout(set = 0, binding = 6) uniform sampler fs_samp1;
layout(set = 0, binding = 7) uniform sampler fs_samp2;
layout(set = 0, binding = 8) uniform sampler fs_samp3;

layout(location = 0) pervertexEXT in vec4 fs_in_attr0_p[3];
layout(location = 1) pervertexEXT in vec4 fs_in_attr1_p[3];
layout(location = 3) pervertexEXT in vec4 fs_in_attr2_p[3];
layout(location = 4) pervertexEXT in vec4 fs_in_attr3_p[3];
layout(location = 0) out vec4 frag_color0;

uint buf_off;

float c(uint i) {
    return uintBitsToFloat(ssbo_1_1.data[i + buf_off]);
}

// The game's texture coordinates: attribute interpolated with the barycentrics, as recompiled.
vec2 interp(vec4 a0, vec4 a1, vec4 a2) {
    vec3 b = gl_BaryCoordEXT;
    return vec2(fma(a2.x - a0.x, b.z, fma(a1.x - a0.x, b.y, a0.x)),
                fma(a2.y - a0.y, b.z, fma(a1.y - a0.y, b.y, a0.y)));
}

// Scale and bias, then saturation around the mean of the three channels.
vec3 tint(vec3 t, uint scale, uint bias, uint saturation) {
    vec3 s = t * c(scale) + c(bias);
    float mean = (s.r + s.g + s.b) * 0.3333333432674407958984375;
    return c(saturation) * (s - mean) + mean;
}

void main() {
    buf_off = bitfieldExtract(push_data.buf_offsets0.x, 0, 8) >> 2u;

    vec3 exposure = texture(sampler2D(fs_img0, fs_samp0),
                            interp(fs_in_attr2_p[0], fs_in_attr2_p[1], fs_in_attr2_p[2])).rgb;
    vec4 scene = textureLod(sampler2D(fs_img1, fs_samp1),
                            interp(fs_in_attr0_p[0], fs_in_attr0_p[1], fs_in_attr0_p[2]), 0.0);
    vec3 bloom = textureLod(sampler2D(fs_img2, fs_samp2),
                            interp(fs_in_attr1_p[0], fs_in_attr1_p[1], fs_in_attr1_p[2]), 0.0).rgb;
    float grain = textureLod(sampler2D(fs_img3, fs_samp3),
                             interp(fs_in_attr3_p[0], fs_in_attr3_p[1], fs_in_attr3_p[2]), 0.0).x;

    vec3 x = tint(exposure, 752u, 753u, 756u) * c(754u) + c(755u);
    vec3 b = clamp(tint(exposure, 744u, 745u, 748u), 0.0, 1.0) * c(746u) + c(747u);

    vec3 v = x * (scene.rgb * c(616u));
    vec3 y = 1.0 - exp2(1.44269502162933349609375 * (-max(vec3(6.103515625e-05), v) * c(832u)));
    vec3 knee = c(833u) * (y - 1.0) + 1.0;
    vec3 tonemapped = clamp(knee * (y * knee), 0.0, 1.0);

    vec3 glow = clamp(c(617u) * (bloom * x + b), 0.0, 1.0);
    vec3 screen = tonemapped + glow - tonemapped * glow;

    // Color matrix (rows 840, 844, 848 applied to RGB); the game writes the rows out in reverse
    // order (row 848 to the first channel).
    vec3 graded = vec3(dot(vec3(c(848u), c(849u), c(850u)), screen) + c(851u),
                       dot(vec3(c(844u), c(845u), c(846u)), screen) + c(847u),
                       dot(vec3(c(840u), c(841u), c(842u)), screen) + c(843u));
    vec3 encoded = exp2(c(808u) * log2(clamp(graded + 2.384185791015625e-07, 0.0, 1.0)));
    vec3 color = c(809u) * grain + encoded + c(810u);

    if (BB_TONEMAP_MODE == 1 && gl_FragCoord.x < 960.0) {
        color *= vec3(1.0, 0.6, 1.0); // check: magenta tint on the left half
    }

    frag_color0 = vec4(color,
                       clamp(dot(vec4(c(620u), c(621u), c(622u), c(623u)), scene), 0.0, 1.0));
}
