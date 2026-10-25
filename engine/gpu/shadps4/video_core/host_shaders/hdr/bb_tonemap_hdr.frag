// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport HDR: HDR version of Bloodborne's tonemap pixel shader (guest ps 0x5c02eaf8), for the HDR
// side channel: the tonemap draw is replayed with this shader into a bbport-owned RGBA16F target
// (docs/HDR.md, step 3b). Same interface and same math as hdr/bb_tonemap.frag, except the curve: Its interface (inputs, descriptor bindings, push
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

#version 450
#extension GL_EXT_fragment_shader_barycentric : require

//   - up to the knee (the game's output at BB_HDR_KNEE of white) the game's curve, unchanged;
//   - above, a Neutwo shoulder (hdr/renodx.glsl) with the same slope at the knee, rising to
//     BB_HDR_PEAK (peak luminance / paper white) instead of the game's white.
// Output (RGBA16F):
//   r, b, a = HDR gain of the red, green and blue channels, all the same here: luminance of the
//       HDR color / luminance of the game's color (1 below the knee);
//   g = luminance of the game's own output (gamma 2.2 encoded, no grain), so the final pass can
//       tell the scene from the UI drawn over it.
// The final pass multiplies the game's finished image (UI, AA, grain and brightness setting
// included) by the gain where the pixel is still the scene: the SDR image is kept as it is and
// only its highlights are raised, so the channel order of the targets does not matter.
// Specialization constants: 1 = BB_HDR_PEAK, 2 = BB_HDR_KNEE.

layout(constant_id = 1) const float BB_HDR_PEAK = 4.926;
layout(constant_id = 2) const float BB_HDR_KNEE = 0.6;

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
layout(location = 1) out vec4 frag_aux; // effect ratios (bb_composite_hdr.frag): none here

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

// rows dot color, for the game's 3x4 color matrix (mat3 columns hold the rows here).
vec3 screen_rows(mat3 rows, vec3 color) {
    return vec3(dot(rows[0], color), dot(rows[1], color), dot(rows[2], color));
}

// The game's curve: y = 1 - exp(-k v), T = y (c (y - 1) + 1)^2.
vec3 game_curve(vec3 v, float k, float knee_c) {
    vec3 y = 1.0 - exp2(1.44269502162933349609375 * (-max(vec3(6.103515625e-05), v) * k));
    vec3 knee = knee_c * (y - 1.0) + 1.0;
    return clamp(knee * (y * knee), 0.0, 1.0);
}

// Where the game's curve reaches `target` (< 1): the scene value and the slope there (Newton on y).
void game_curve_point(float target, float k, float knee_c, out float v, out float slope) {
    float y = target;
    for (int i = 0; i < 8; ++i) {
        float a = knee_c * y - knee_c + 1.0;
        y -= (y * a * a - target) / (a * a + 2.0 * knee_c * y * a);
    }
    y = clamp(y, 0.0, 0.999999);
    float a = knee_c * y - knee_c + 1.0;
    v = -log(1.0 - y) / k;
    slope = (a * a + 2.0 * knee_c * y * a) * k * (1.0 - y);
}

// Neutwo with peak p: p x / sqrt(x^2 + p^2) (slope 1 at 0, tends to p).
vec3 neutwo(vec3 x, float p) {
    return p * x * inversesqrt(x * x + p * p);
}

void main() {
    buf_off = bitfieldExtract(push_data.buf_offsets0.x, 0, 8) >> 2u;

    vec3 exposure = texture(sampler2D(fs_img0, fs_samp0),
                            interp(fs_in_attr2_p[0], fs_in_attr2_p[1], fs_in_attr2_p[2])).rgb;
    vec4 scene = textureLod(sampler2D(fs_img1, fs_samp1),
                            interp(fs_in_attr0_p[0], fs_in_attr0_p[1], fs_in_attr0_p[2]), 0.0);
    vec3 bloom = textureLod(sampler2D(fs_img2, fs_samp2),
                            interp(fs_in_attr1_p[0], fs_in_attr1_p[1], fs_in_attr1_p[2]), 0.0).rgb;

    vec3 x = tint(exposure, 752u, 753u, 756u) * c(754u) + c(755u);
    vec3 b = clamp(tint(exposure, 744u, 745u, 748u), 0.0, 1.0) * c(746u) + c(747u);
    vec3 glow = clamp(c(617u) * (bloom * x + b), 0.0, 1.0);

    float k = c(832u), knee_c = c(833u);
    vec3 v = x * (scene.rgb * c(616u));
    vec3 sdr = game_curve(v, k, knee_c);

    // HDR curve: the game's below the knee, a Neutwo shoulder above.
    float knee_v, knee_slope;
    game_curve_point(BB_HDR_KNEE, k, knee_c, knee_v, knee_slope);
    vec3 shoulder = BB_HDR_KNEE + neutwo(max(v - knee_v, 0.0) * knee_slope,
                                         max(BB_HDR_PEAK - BB_HDR_KNEE, 1e-3));
    vec3 hdr = mix(sdr, shoulder, step(knee_v, v));

    // Screen blend of the bloom on the SDR part only (above 1 it would subtract).
    vec3 sdr_screen = sdr + glow - sdr * glow;
    vec3 hdr_screen = hdr + glow * (1.0 - min(hdr, 1.0));

    mat3 grade_rows = mat3(vec3(c(840u), c(841u), c(842u)), vec3(c(844u), c(845u), c(846u)),
                           vec3(c(848u), c(849u), c(850u)));
    vec3 offsets = vec3(c(843u), c(847u), c(851u));
    // The game writes row 848 to the first channel (see hdr/bb_tonemap.frag).
    vec3 sdr_graded = (screen_rows(grade_rows, sdr_screen) + offsets).zyx;
    vec3 hdr_graded = (screen_rows(grade_rows, hdr_screen) + offsets).zyx;

    // The game's channel order here is BGR (row 840, mostly red, goes to the third channel).
    const vec3 luma_bgr = vec3(0.0721923154, 0.7151686788, 0.2126390059);
    vec3 sdr_encoded = exp2(c(808u) * log2(clamp(sdr_graded + 2.384185791015625e-07, 0.0, 1.0)));
    float sdr_luma = dot(max(sdr_graded, 0.0), luma_bgr);
    float hdr_luma = dot(max(hdr_graded, 0.0), luma_bgr);
    float gain = sdr_luma > 1e-4 ? max(hdr_luma / sdr_luma, 1.0) : 1.0;
    // r, b, a: the gain of each channel (the same here), g: the reference luminance.
    frag_color0 = vec4(gain, dot(sdr_encoded, luma_bgr), gain, gain);
    frag_aux = vec4(1.0);
}
