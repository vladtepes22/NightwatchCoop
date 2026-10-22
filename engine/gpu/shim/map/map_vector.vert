// SPDX-License-Identifier: GPL-2.0-or-later
// The map's vertex shader (Hunter's Atlas source/compact/vector.vert). Vertices are ImDrawVert
// shaped: world (x, -z), an auxiliary pair, a colour. data.w picks the kind:
//   0  flat colour (paint)
//   1  floors: parchment UVs from the world position, shaded by height (aux.x) within paint.xy
//   2  strokes: extruded along aux by params.z pixels; colour.r / .g add per-stroke wear to alpha
#version 450
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 auxiliary;
layout(location = 2) in vec4 colour;
layout(location = 0) out vec4 outColour;
layout(location = 1) out vec2 outUV;
layout(push_constant) uniform Constants {
    vec4 view;    // world -> clip: scale xy, offset zw
    vec4 data;    // area centre xz, parchment span, kind
    vec4 paint;
    vec4 params;  // clip units per pixel xy, stroke width, wear channel
} p;
void main() {
    vec2 projected = position * p.view.xy + p.view.zw;
    if (p.data.w > 1.5) {
        float distance = colour.a < 0.5 ? p.params.z * 0.5 + 0.5 : max(0.0, p.params.z * 0.5 - 0.5);
        projected += auxiliary * p.params.xy * distance;
        float alpha = p.paint.a + (p.params.w > 1.5 ? colour.g : (p.params.w > 0.5 ? colour.r : 0.0));
        outColour = vec4(p.paint.rgb, alpha * colour.a);
        outUV = vec2(0.5);
    } else if (p.data.w > 0.5) {
        float t = p.paint.w > 0.5 ? clamp((auxiliary.x - p.paint.x) / max(0.01, p.paint.y - p.paint.x), 0.0, 1.0) : 0.72;
        float shade = 118.0 + float(int(120.0 * t));
        outColour = vec4((vec3(shade) + vec3(4, 2, 0)) / 255.0, p.paint.z);
        outUV = clamp(vec2(0.5) + (vec2(position.x, -position.y) - p.data.xy) / p.data.z, vec2(0), vec2(1));
    } else {
        outColour = p.paint;
        outUV = vec2(0.5);
    }
    gl_Position = vec4(projected, 0, 1);
}
