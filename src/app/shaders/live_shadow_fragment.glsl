// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's units' shadows, laid on the ground: fragment stage
// (renderer_live_gpu.cpp). The ground darkened by the light the shadows'
// map leaves where it lies, blended over what is drawn as the game's
// shadow decals are. Multisampled, each sample the ground covers takes the
// map at its own place, as the decals' samples take their texels, and the
// samples in no shadow are left out. tools/make_live_shaders.py compiles it
// for every graphics interface the renderer draws with.
#version 450

layout(location = 0) in vec2 in_painted;

// The shadows' map; the scene's atlases keep the first two places.
layout(set = 2, binding = 2) uniform sampler2D shadow_map;

// The map's left and top in map pixels and the texels a map pixel is; then
// the samples a pixel and the frame's texels across and down, at the
// texture's top left.
layout(set = 3, binding = 1, std140) uniform Shadows {
    vec4 place;
    vec4 drawing;
}
shadows;

layout(location = 0) out vec4 out_colour;

// Where a multisampled pixel's four samples lie in it, as
// live_scene_fragment.glsl's.
const vec2 sample_positions[4] =
    vec2[4](vec2(0.375, 0.125), vec2(0.875, 0.375), vec2(0.125, 0.625), vec2(0.625, 0.875));

// The light the map leaves at a texel of the frame's; full light past the
// frame's texels. Taken by the texel, so that the same texel is taken
// however large the texture has been made.
float light_at(vec2 texel) {
    if (any(lessThan(texel, vec2(0.0))) || any(greaterThanEqual(texel, shadows.drawing.yz)))
        return 1.0;
    return texelFetch(shadow_map, ivec2(texel), 0).r;
}

void main() {
    const vec2 texel = (in_painted - shadows.place.xy) * shadows.place.z;
    if (shadows.drawing.x > 1.0) {
        const vec2 texel_x = dFdx(texel);
        const vec2 texel_y = dFdy(texel);
        int coverage = 0;
        float shade = 0.0;
        for (int sample_index = 0; sample_index < 4; ++sample_index) {
            if ((gl_SampleMaskIn[0] & (1 << sample_index)) == 0)
                continue;
            const vec2 offset = sample_positions[sample_index] - 0.5;
            const float light = light_at(texel + texel_x * offset.x + texel_y * offset.y);
            if (light < 1.0) {
                coverage |= 1 << sample_index;
                shade += 1.0 - light;
            }
        }
        if (coverage == 0)
            discard;
        gl_SampleMask[0] = coverage;
        out_colour = vec4(0.0, 0.0, 0.0, shade / float(bitCount(coverage)));
        return;
    }
    const float light = light_at(texel);
    if (light >= 1.0)
        discard;
    // Every sample, as the stage writes the samples it keeps.
    gl_SampleMask[0] = -1;
    out_colour = vec4(0.0, 0.0, 0.0, 1.0 - light);
}
