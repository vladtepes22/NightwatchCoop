// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: HDR output. The game's SDR picture (letterboxed into a swapchain-sized sRGB-encoded
// image) and the settings menu's layer (premultiplied alpha, sRGB encoded) become the HDR
// swapchain's signal: HDR10 (BT.2020 primaries, PQ) or scRGB (linear BT.709, 1.0 = 80 nits).
// SDR white is shown at paper_white. Native mode (FLAG_NATIVE): the frame's gain map
// (hdr_native.comp: how much brighter the game's own scene is where its tonemap compressed the
// highlights; 1 for midtones and UI) brightens the highlights. Expand mode, and native frames
// without a scene (menus, loading, movies): boost expands the SDR picture's highlights towards
// the peak.
// bbport: in native mode the game's UI (where the gain map is negative, and all of a frame
// without a scene) has its own white (ui_white times paper white) and colours: ui_vivid of the
// way from sRGB's (exact) to a wide-gamut SDR monitor's (the values taken as DCI-P3), which is
// how most players have seen it - next to that, exact sRGB looks washed out.

#version 450
#extension GL_GOOGLE_include_directive : require

layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D game;    // the letterboxed SDR frame, swapchain-sized
layout (binding = 1) uniform sampler2D overlay; // the menu layer (FLAG_OVERLAY)
layout (binding = 2) uniform sampler2D gain;    // the frame's gain map (FLAG_NATIVE), filtered

layout (push_constant) uniform HdrSettings {
    float paper_white; // nits of SDR white
    float peak;        // nits the display reaches
    float boost;       // 0: SDR white at paper white .. 1: SDR white at the peak
    uint flags;
    vec2 gain_offset;  // the frame's rectangle in the output: its top left corner, pixels,
    vec2 gain_scale;   // and 1 / its size: output pixels -> gain map coordinates
    float ui_white;    // native mode: the UI's white, times paper white
    float ui_vivid;    // native mode: 0 the UI in sRGB's colours .. 1 in DCI-P3's
    float black_floor;
    uint compare;
    uint tonemapper;
} hdr;

layout(binding = 3) uniform sampler2D hdrChannel;

const uint FLAG_SCRGB = 1u;   // scRGB output, else HDR10
const uint FLAG_OVERLAY = 2u; // the menu layer has something drawn
const uint FLAG_NATIVE = 4u;  // the gain map belongs to this frame
const uint FLAG_DEBUG = 8u;   // BB_HDR_DEBUG: the gain as a heat map
const uint FLAG_UI_FRAME = 16u; // native mode, a frame without a scene: all of it is UI
// 32 Native+ (16-bit) frame, 64 its signal encoded, 128 its pre-HUD snapshot bound (hdr_pass.h).
const uint FLAG_NEGATIVE_PROBE = 256u; // Native+: the 16-bit probe 6, channels below 0
const uint FLAG_COMPARE = 1024u;  // the left half as SDR (at paper white), for comparison

// Linear brightness (SDR white = 1) above which the highlights are expanded: high, so large
// bright areas (fog, sky) stay near paper white and only lamps, fire and sparks reach the peak.
const float KNEE = 0.8;

// BT.709 -> BT.2020 primaries (columns).
const mat3 BT709_TO_BT2020 = mat3(
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956);

// DCI-P3 (D65) -> BT.709 primaries (columns): BT.709 values outside [0, 1] are the wider gamut,
// which the BT.2020 and scRGB encodes below both carry.
const mat3 P3_TO_BT709 = mat3(
    1.2249, -0.0421, -0.0196,
    -0.2249, 1.0421, -0.0786,
    0.0, 0.0, 1.0983);

// The game's picture is made for SDR displays, which show it with a 2.2 power curve. The
// piecewise sRGB curve (what Windows uses for SDR content in HDR) lifts the shadows: dark
// scenes turn grey and foggy. Bloodborne is mostly shadows, so 2.2 it is.
vec3 SdrToLinear(vec3 c) {
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

// SMPTE ST 2084 inverse EOTF; y: luminance / 10000 nits.
vec3 PqEncode(vec3 y) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 ym = pow(clamp(y, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), vec3(m2));
}

// Inverse tone map of the brightest channel (hue and saturation kept): unchanged up to KNEE,
// then a curve with the same slope there that reaches `white` at SDR white.
vec3 ExpandHighlights(vec3 rgb, float white) {
    float m = max(rgb.r, max(rgb.g, rgb.b));
    if (white <= 1.0 || m <= KNEE) {
        return rgb;
    }
    float t = (m - KNEE) / (1.0 - KNEE);
    float expanded = m + (white - 1.0) * t * t;
    return rgb * (expanded / m);
}

// Debug view colours: blue (a little gain) .. cyan .. green .. yellow .. red (the peak).
vec3 Heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0),
                      1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

struct PsychoSettings {
    float gamma;
    bool hdr;
    bool srgb_input;
    float hdr_paper_white; // bbport: nits of SDR white in HDR10 output
    bool hdr_gamma22;      // bbport: decode SDR with a 2.2 power instead of the sRGB curve
    bool hdr_test_pattern; // bbport: HDR10 test pattern instead of the game
    bool hdr_channel;      // bbport: apply the HDR side channel's gains
    uint hdr_debug;        // bbport: 1 = gain heat map, 2 = pixels taken as UI in red
    float hdr_peak_nits;   // bbport: peak luminance
    uint hdr_compare;      // bbport: 0 HDR, 1 the game's SDR
    float hdr_ui_nits;     // bbport: nits of the HUD's white (pixels taken as UI)
    uint hdr_tonemapper;   // bbport: 2 Vanilla (the game's own look: the HDR preset Off), 3 tone mapped by the composite
    uint hdr_native16;     // bbport: the 16-bit chain's frame: 1 linear, 2 sRGB encoded (HDR > 1)
    float hdr_black_floor; // bbport: the game's black level taken to 0 (units of paper white)
};
PsychoSettings pp;
vec2 uv;
const uint TONEMAPPER_VANILLA = 2u;

const vec3 LUMA709 = vec3(0.2126390059, 0.7151686788, 0.0721923154);



// The game's black level (its darkest grey, lifted by its brightness curve) taken down to 0, less
// and less up to white; above white unchanged.
vec3 black_floor(vec3 v) {
    float f = clamp(pp.hdr_black_floor, 0.0, 0.5);
    if (f <= 0.0) {
        return v;
    }
    vec3 lowered = max(v - f, 0.0) / (1.0 - f);
    return mix(lowered, v, greaterThan(v, vec3(1.0)));
}

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

// bbport HDR10: Rec.709 -> Rec.2020 primaries, then SMPTE ST 2084 (PQ) encoding.
vec3 bt709_to_bt2020(vec3 c) {
    return vec3(dot(vec3(0.6274040, 0.3292820, 0.0433136), c),
                dot(vec3(0.0690970, 0.9195400, 0.0113612), c),
                dot(vec3(0.0163916, 0.0880132, 0.8955950), c));
}

vec3 pq_encode(vec3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

// Shared by the native 16-bit chain and the legacy side-channel path.
vec3 decode_sdr_signal(vec3 signal) {
    return pp.hdr_gamma22 ? pow(signal, vec3(2.2)) : degamma(signal);
}

bool sdr_reference() {
    return pp.hdr_compare == 1u ||
           pp.hdr_tonemapper == TONEMAPPER_VANILLA;
}

vec3 scene_to_nits(vec3 v) {
    return min(max(bt709_to_bt2020(v), 0.0) * pp.hdr_paper_white, vec3(pp.hdr_peak_nits));
}

// An SDR signal (sRGB-encoded, 0..1) as an SDR display shows it, in Rec.2020 nits, each Rec.709
// channel first multiplied by `gains` (the HDR side channel's; 1 for plain SDR).
vec3 sdr_to_nits(vec3 signal, vec3 gains) {
    vec3 lin = decode_sdr_signal(signal);
    return max(bt709_to_bt2020(lin * gains), 0.0) * pp.hdr_paper_white;
}

vec3 ui_to_nits(vec3 signal) {
    return sdr_to_nits(signal, vec3(1.0)) * (pp.hdr_ui_nits / pp.hdr_paper_white);
}

vec3 debug_nits(vec3 nits, vec3 tone_mapped, float reference_luma, float scene, float peak) {
    if (pp.hdr_debug == 1u) {
        float gain = dot(tone_mapped, LUMA709) / max(reference_luma, 1e-4);
        float k = clamp((gain - 1.0) / (max(peak, 1.001) - 1.0), 0.0, 1.0);
        return gain > 1.001 ? mix(vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), k) * pp.hdr_paper_white
                            : vec3(dot(nits, vec3(0.2627, 0.6780, 0.0593)) * 0.5);
    }
    if (pp.hdr_debug == 2u && scene < 0.5) {
        return mix(nits, vec3(pp.hdr_paper_white, 0.0, 0.0), 0.6);
    }
    return nits;
}

// bbport HDR test pattern, four rows from the top:
//  1. SDR grey ramp, 32 steps from black to white, through the same path as the game;
//  2. grey patches at 0.1, 1, 10, 50, 100 nits, paper white, 300, 400, 600, 800 nits;
//  3. grey patches at 1000, 1200, 1500, 2000, 2500, 3000, 4000, 6000, 8000, 10000 nits
//     (patches that look the same are above what the display can show);
//  4. SDR color bars: white, yellow, cyan, green, magenta, red, blue, black.
vec3 test_pattern_nits(vec2 p) {
    int row = int(min(floor(p.y * 4.0), 3.0));
    float fy = fract(p.y * 4.0);
    if (fy < 0.03 || fy > 0.97) {
        return vec3(0.0);
    }
    if (row == 0) {
        return sdr_to_nits(vec3(min(floor(p.x * 32.0), 31.0) / 31.0), vec3(1.0));
    }
    if (row == 3) {
        const vec3 bars[8] = vec3[](vec3(1.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 1.0),
                                    vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0),
                                    vec3(0.0, 0.0, 1.0), vec3(0.0));
        return sdr_to_nits(bars[int(min(floor(p.x * 8.0), 7.0))], vec3(1.0));
    }
    const float low[10] = float[](0.1, 1.0, 10.0, 50.0, 100.0, -1.0, 300.0, 400.0, 600.0, 800.0);
    const float high[10] = float[](1000.0, 1200.0, 1500.0, 2000.0, 2500.0, 3000.0, 4000.0,
                                   6000.0, 8000.0, 10000.0);
    int i = int(min(floor(p.x * 10.0), 9.0));
    float fx = fract(p.x * 10.0);
    if (fx < 0.04 || fx > 0.96) {
        return vec3(0.0);
    }
    float nits = row == 1 ? (low[i] < 0.0 ? pp.hdr_paper_white : low[i]) : high[i];
    return vec3(nits);
}

// bbport HDR, 16-bit chain (BB_HDR_TONEMAP=5): the game drew its frame in RGBA16F, linear, its
// white at 1 and its highlights above. The SDR part is decoded like the game's SDR (sRGB or 2.2);
// above 1 the values are the game's own, then shaped live like the side channel's.
vec3 native16_nits(vec3 v) {
    // Channels below 0 (PsychoV30 with a BT.2020 target: colors outside BT.709) are kept apart,
    // decoded with their sign and added back before the BT.2020 conversion, which drops what is
    // still negative there (scene_to_nits). The rest exactly as before, on the positive part.
    vec3 below = min(v, 0.0);
    v = max(v, 0.0);
    vec3 sdr = min(v, 1.0);
    // Encoded (2): the values are the SDR signal itself; linear (1): encoded like the game's.
    vec3 signal = pp.hdr_native16 == 2u ? sdr : clamp(gamma(sdr), 0.0, 1.0);
    // Vanilla tone mapper (or the menu's preset Off): the game's own SDR look, as Compare's SDR.
    if (sdr_reference()) {
        return sdr_to_nits(signal, vec3(1.0));
    }
    vec3 lin_sdr = decode_sdr_signal(signal);
    // Above white the chain's values are gamma encoded (the composite's output, a 2.2 power).
    vec3 lin = black_floor(lin_sdr * mix(vec3(1.0), pow(v, vec3(2.2)), greaterThan(v, vec3(1.0))));
    // The channels below 0, decoded with their sign (encoded: the chain's 2.2 power; linear: as
    // they are), after Black Floor, which lowers the positive part only.
    lin += pp.hdr_native16 == 2u ? -pow(-below, vec3(2.2)) : below;
    float peak = max(pp.hdr_peak_nits / pp.hdr_paper_white, 1.0);
    float l_sdr = dot(lin_sdr, LUMA709);
    // The composite tone mapped the scene to the peak (Vanilla+ or PsychoV-30): only encoded here.
    vec3 t = lin;
    vec3 nits = scene_to_nits(t);
    // The HUD: the pixels the UI changed since the scene's snapshot (bound as hdrChannel),
    // shown as the game's SDR at the HUD brightness, told from luminance: the brightness curve
    // comes after the snapshot, so the tolerance widens with brightness.
    float scene = 1.0;
    if (pp.hdr_channel) {
        vec3 s = max(texture(hdrChannel, uv).rgb, 0.0);
        float l_snap = dot(min(s, 1.0), LUMA709);
        float l_now = dot(signal, LUMA709);
        float tolerance = 0.04 + 0.22 * l_snap * l_snap;
        float miss = abs(l_now - l_snap);
        scene = 1.0 - smoothstep(tolerance, tolerance + 0.10, miss);
        vec3 ui = ui_to_nits(signal);
        nits = mix(ui, nits, scene);
    }
    return debug_nits(nits, t, l_sdr, scene, peak);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 game_srgb = texelFetch(game, pixel, 0).rgb;
    if ((hdr.flags & 32u) != 0u) {
        uv = (gl_FragCoord.xy - hdr.gain_offset) * hdr.gain_scale;
        // Every field native16_nits may read: neutral where this pass has no setting of its own
        // (bbport's final-pass grading; uninitialized, a tone mapper other than PsychoV30 (scene)
        // read garbage).
        pp.hdr = true;
        pp.srgb_input = false;
        pp.hdr_test_pattern = false;
        pp.gamma = 1.0;
        pp.hdr_paper_white = hdr.paper_white;
        pp.hdr_peak_nits = hdr.peak;
        pp.hdr_ui_nits = hdr.ui_white * hdr.paper_white;
        pp.hdr_gamma22 = true;
        pp.hdr_native16 = (hdr.flags & 64u) != 0u ? 2u : 1u;
        pp.hdr_channel = (hdr.flags & 128u) != 0u;
        pp.hdr_compare = hdr.compare;
        pp.hdr_tonemapper = hdr.tonemapper;
        pp.hdr_black_floor = hdr.black_floor;
        // The 16-bit probe 7 (or BB_HDR_DEBUG): the pixels taken as the HUD in red.
        pp.hdr_debug = (hdr.flags & FLAG_DEBUG) != 0u ? 2u : 0u;
        vec3 nits = native16_nits(game_srgb);
        // The 16-bit probe 6 (in-game menu, test): magenta where a BT.709 channel reaches this
        // pass below 0, a color outside BT.709 (PsychoV30 with a BT.2020 target) kept to the end.
        if ((hdr.flags & FLAG_NEGATIVE_PROBE) != 0u &&
            min(min(game_srgb.r, game_srgb.g), game_srgb.b) < -1e-3) {
            nits = vec3(hdr.paper_white, 0.0, hdr.paper_white);
        }
        if ((hdr.flags & FLAG_OVERLAY) != 0u) {
            vec4 menu = texelFetch(overlay, pixel, 0);
            vec3 menu_signal = clamp(menu.rgb + min(game_srgb, 1.0) * (1.0 - menu.a), 0.0, 1.0);
            nits = mix(nits, sdr_to_nits(menu_signal, vec3(1.0)), clamp(menu.a * 4.0, 0.0, 1.0));
        }
        if ((hdr.flags & FLAG_SCRGB) != 0u) {
            // BT.2020 nits -> linear BT.709 scRGB (one unit = 80 nits).
            const mat3 inv = mat3(1.660491, -0.1245505, -0.0181508,
                -0.587641, 1.1328999, -0.1005789, -0.0728499, -0.0083494, 1.1187297);
            color = vec4(inv * nits / 80.0, 1.0);
        } else color = vec4(pq_encode(nits), 1.0);
        return;
    }
    float peak_white = max(hdr.peak / hdr.paper_white, 1.0);
    float white = max(mix(1.0, peak_white, clamp(hdr.boost, 0.0, 1.0)), 1.0);
    vec3 sdr_linear = SdrToLinear(game_srgb);
    vec3 linear;
    float applied = 1.0; // the brightest channel's gain (debug view)
    bool ui = false;
    float ui_amount = (hdr.flags & FLAG_UI_FRAME) != 0u ? 1.0 : 0.0; // filtered: soft UI edges
    if ((hdr.flags & FLAG_NATIVE) != 0u) {
        // Outside the frame (letterbox bars) the map is not sampled: gain 1.
        vec2 uv = (gl_FragCoord.xy - hdr.gain_offset) * hdr.gain_scale;
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
            float g = textureLod(gain, uv, 0.0).r;
            ui = g < 0.0;
            ui_amount = clamp(-g, 0.0, 1.0);
            applied = max(abs(g), 1.0);
        }
        // The map keeps the brightest channel within the peak; filtering and the letterbox
        // blit can step over it by a little.
        linear = min(sdr_linear * applied, vec3(peak_white));
    } else {
        linear = ExpandHighlights(sdr_linear, white);
        float m = max(sdr_linear.r, max(sdr_linear.g, sdr_linear.b));
        applied = m > 0.0 ? max(linear.r, max(linear.g, linear.b)) / m : 1.0;
    }
    if (ui_amount > 0.0) {
        linear *= mix(1.0, hdr.ui_white, ui_amount);
        linear = mix(linear, P3_TO_BT709 * linear, ui_amount * clamp(hdr.ui_vivid, 0.0, 1.0));
        linear = min(linear, vec3(peak_white));
    }
    if ((hdr.flags & FLAG_DEBUG) != 0u) {
        // The picture in grey at a third of paper white, the gain over it from blue (a little)
        // to red (the peak), UI that native mode keeps at paper white in magenta. Top left
        // corner: green while native mode has this frame's gain map, orange while it falls back
        // to expand (or in expand mode).
        float y = dot(sdr_linear, vec3(0.2126, 0.7152, 0.0722));
        vec3 shown = vec3(y * 0.33);
        if (applied > 1.001) {
            shown = mix(shown, Heat(log2(applied) / max(log2(peak_white), 0.1)), 0.8);
        }
        if (ui) {
            shown = mix(shown, vec3(1.0, 0.0, 1.0), 0.5);
        }
        if (gl_FragCoord.x < 48.0 && gl_FragCoord.y < 48.0) {
            shown = (hdr.flags & FLAG_NATIVE) != 0u ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.5, 0.0);
        }
        linear = shown;
    }
    if ((hdr.flags & FLAG_COMPARE) != 0u && (hdr.flags & FLAG_DEBUG) == 0u) {
        // The left half as an SDR display shows the game (at paper white, sRGB's colours), a
        // thin grey line between the two.
        const float middle = 0.5 * float(textureSize(game, 0).x);
        if (gl_FragCoord.x < middle) {
            linear = sdr_linear;
        }
        if (abs(gl_FragCoord.x - middle) < 1.5) {
            linear = vec3(0.5);
        }
    }
    if ((hdr.flags & FLAG_OVERLAY) != 0u) {
        // The menu over the frame as the SDR path draws it (blended in sRGB), at paper white:
        // the menu is not expanded.
        vec4 menu = texelFetch(overlay, pixel, 0);
        if (menu.a > 0.0) {
            vec3 sdr = SdrToLinear(clamp(menu.rgb + game_srgb * (1.0 - menu.a), 0.0, 1.0));
            linear = mix(linear, sdr, clamp(menu.a * 4.0, 0.0, 1.0));
        }
    }
    if ((hdr.flags & FLAG_SCRGB) != 0u) {
        color = vec4(linear * (hdr.paper_white / 80.0), 1.0);
    } else {
        vec3 nits = max(BT709_TO_BT2020 * linear, vec3(0.0)) * hdr.paper_white;
        color = vec4(PqEncode(nits / 10000.0), 1.0);
    }
}
