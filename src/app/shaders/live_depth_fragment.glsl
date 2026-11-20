// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's depth cues, fragment stage (renderer_live_gpu.cpp):
// depth of field and haze by the camera depth the lights' surface keeps,
// drawn after the lights, over the picture, and only while a photo asks.
// A pixel with no surface in view, the void past the map or ground the
// player has never seen, keeps its colour: the fog stays black and sharp,
// as the lights leave it unlit. The blur reads an unchanged copy of the
// picture.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

layout(set = 2, binding = 0) uniform sampler2D picture;
layout(set = 2, binding = 1) uniform sampler2D surface;

// focus, the sharp range, blur per depth unit past it, and the widest blur.
// Haze amount and colour. The near depth, the depth from near to far, and
// the picture's last column and row.
layout(set = 3, binding = 0, std140) uniform Parameters {
    vec4 focus_band;
    vec4 haze;
    vec4 framing;
};

layout(location = 0) out vec4 out_colour;

// Twelve taps in the unit disk. The centre sample is taken beside them.
const vec2 kDisk[12] = vec2[12](
    vec2(0.000, 0.500),
    vec2(0.433, 0.250),
    vec2(0.433, -0.250),
    vec2(0.000, -0.500),
    vec2(-0.433, -0.250),
    vec2(-0.433, 0.250),
    vec2(0.250, 0.866),
    vec2(0.866, 0.000),
    vec2(0.250, -0.866),
    vec2(-0.500, -0.866),
    vec2(-0.866, 0.000),
    vec2(-0.500, 0.866)
);

// How far a surface's camera depth is, 0 at the near depth and 1 at the far
// end.
float farness_of(float depth) {
    return clamp((framing.x - depth) / max(framing.y, 1e-6), 0.0, 1.0);
}

// The picture, hazed by the surface under that pixel. A pixel with no
// surface in view keeps its colour.
vec3 hazed(ivec2 pixel) {
    const vec3 colour = texelFetch(picture, pixel, 0).rgb;
    const vec2 held = texelFetch(surface, pixel, 0).xy;
    if (held.y <= 0.0)
        return colour;
    return mix(colour, haze.yzw, haze.x * farness_of(held.x));
}

// Blur in pixels. A surface inside the sharp range stays sharp, and so does
// a pixel with no surface in view.
float radius_at(ivec2 pixel) {
    const vec2 held = texelFetch(surface, pixel, 0).xy;
    if (held.y <= 0.0)
        return 0.0;
    const float past = max(0.0, abs(held.x - focus_band.x) - focus_band.y);
    return min(focus_band.w, past * focus_band.z);
}

ivec2 placed(vec2 at) {
    return ivec2(clamp(round(at), vec2(0.0), framing.zw));
}

void main() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const float radius = radius_at(pixel);
    if (radius < 0.5) {
        out_colour = vec4(hazed(pixel), 1.0);
        return;
    }
    vec3 sum = hazed(pixel);
    for (int tap = 0; tap < 12; ++tap)
        sum += hazed(placed(vec2(pixel) + kDisk[tap] * radius));
    out_colour = vec4(sum / 13.0, 1.0);
}
