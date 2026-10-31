// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's units' shadows, cast: vertex stage
// (renderer_live_gpu.cpp). A model's triangle corner from its type's
// corners and its vertex as the processor placed it, laid in the shadows'
// map, which spans the map as the game paints it (a row down is half a
// pixel up): shadow_offset_x right of the model and at the ground's height
// under it, as the game lays the model's picture, or sheared a quarter
// across and up for a building's silhouette. Its corners keep the whole
// pixels from the model's place that the game's picture takes, so that a
// part thinner than a pixel casts no more than the picture shows of it.
// Each cast draws at a depth of its own, so that its triangles darken a
// texel once.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

// Every type's triangle corners (renderer_live_gpu.hpp's ModelDraws::corners).
layout(set = 0, binding = 0, std430) readonly buffer Corners {
    uint corners[];
};

// The frame's model draws (live_model_vertex.glsl), its casts nine words
// each: the instance, the kind (0 the picture, 1 the sheared silhouette, 2
// the picture taken out of a silhouette), the height of the model's base,
// half the ground's height under it, the height above the base a corner
// must pass to cast, the light the cast leaves, its depth and the model's
// place across and down the map.
layout(set = 0, binding = 1, std430) readonly buffer Frame {
    vec4 horizontal;
    vec4 vertical;
    vec4 depth;
    vec4 viewport;
    uint instances;
    uint positions;
    uint piece_frames;
    uint textures;
    uint casts;
    uint unused_1;
    uint unused_2;
    uint unused_3;
    vec4 shadow_map;
    uint words[];
}
frame;

layout(location = 0) out float out_height;
layout(location = 1) flat out vec2 out_cast;

void main() {
    // The game lays a shadow this far right of its model.
    const float shadow_offset_x = 5.0;
    const uint index = uint(gl_VertexIndex);
    const uint record = frame.casts + (index >> 16) * 9u;
    const uint instance = frame.instances + frame.words[record] * 8u;
    const uint kind = frame.words[record + 1u];
    const float base = uintBitsToFloat(frame.words[record + 2u]);
    const float ground_half = uintBitsToFloat(frame.words[record + 3u]);
    const uint place = corners[(frame.words[instance] + (index & 0xffffu)) * 2u];
    const uint vertex = frame.positions + (frame.words[instance + 1u] + (place & 0xffffu)) * 3u;
    const vec3 world = vec3(
        uintBitsToFloat(frame.words[vertex]),
        uintBitsToFloat(frame.words[vertex + 1u]),
        uintBitsToFloat(frame.words[vertex + 2u])
    );
    // The corner's whole pixels from the model's place, as the game's
    // picture takes them, and its height's half and quarter, rounded down.
    const vec3 model_place = vec3(
        uintBitsToFloat(frame.words[record + 7u]), base, uintBitsToFloat(frame.words[record + 8u])
    );
    const vec3 whole = floor(world - model_place);
    const float half_height = floor(whole.y * 0.5);
    const float quarter_height = floor(whole.y * 0.25);
    // A building's picture is taken out of its silhouette where it stands.
    const vec2 across_down = model_place.xz + vec2(0.0, -ground_half);
    const vec2 laid = kind == 1u ? across_down + vec2(shadow_offset_x + whole.x + quarter_height,
                                                      whole.z - quarter_height)
                      : kind == 2u ? across_down + vec2(whole.x, whole.z - half_height)
                                   : across_down + vec2(shadow_offset_x + whole.x, whole.z - half_height);
    out_height = whole.y;
    out_cast = vec2(uintBitsToFloat(frame.words[record + 4u]), uintBitsToFloat(frame.words[record + 5u]));
    // The map's left and top, in map pixels, and how much of the frame's
    // texels of the shadows' map a map pixel is, across and down; the
    // viewport is the frame's texels.
    const vec2 texel = (laid - frame.shadow_map.xy) * frame.shadow_map.zw;
    gl_Position =
        vec4(texel.x * 2.0 - 1.0, 1.0 - texel.y * 2.0, uintBitsToFloat(frame.words[record + 6u]), 1.0);
}
