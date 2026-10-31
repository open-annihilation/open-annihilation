// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's models, vertex stage (renderer_live_gpu.cpp): a unit's
// or feature's triangle corner from its type's corners, which stay on the
// card, its vertex as the processor placed it and its texture's atlas place,
// handed to the scene's fragment stage as the scene's vertex stage hands on
// a model's vertex. Its camera is the scene's, through the same arithmetic.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

// Every type's triangle corners, two words each (renderer_live_gpu.hpp's
// ModelDraws::corners).
layout(set = 0, binding = 0, std430) readonly buffer Corners {
    uint corners[];
};

// The frame's model draws, as renderer_live_gpu.cpp writes them: the scene
// camera's first four rows, where each part begins, then the instances,
// eight words each, the vertices, the pieces' frames and the textures.
layout(set = 0, binding = 1, std430) readonly buffer Frame {
    vec4 horizontal;
    vec4 vertical;
    vec4 depth;
    vec4 viewport;
    uint instances;
    uint positions;
    uint piece_frames;
    uint textures;
    uint words[];
}
frame;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec2 out_attenuation;
layout(location = 2) out vec3 out_emission;
layout(location = 3) out vec3 out_terrain;
layout(location = 4) out vec2 out_terrain_fog;
layout(location = 5) out vec3 out_world;
layout(location = 6) out vec3 out_modulation;

void main() {
    const float depth_limit = 65536.0;
    // The index names the instance above its corner bits; the run's first
    // instance comes as the draw's base.
    const uint index = uint(gl_VertexIndex);
    const uint instance = frame.instances + (index >> 16) * 8u;
    const uint corner_at = (frame.words[instance] + (index & 0xffffu)) * 2u;
    const uint place = corners[corner_at];
    const uint drawn = corners[corner_at + 1u];
    const uint vertex = frame.positions + (frame.words[instance + 1u] + (place & 0xffffu)) * 3u;
    const vec3 world = vec3(
        uintBitsToFloat(frame.words[vertex]),
        uintBitsToFloat(frame.words[vertex + 1u]),
        uintBitsToFloat(frame.words[vertex + 2u])
    );
    // The piece's first frame or its running one, and the polygon's corner,
    // which takes the texture's first or second place across and down.
    const bool first = frame.words[frame.piece_frames + frame.words[instance + 2u] + (place >> 16)] != 0u;
    const uint texture = frame.textures +
                         (frame.words[instance + 3u] + (first ? drawn & 0x3fffu : (drawn >> 14) & 0x3fffu)) * 4u;
    const uint corner = (drawn >> 28) & 3u;
    out_uv = vec2(
        uintBitsToFloat(frame.words[texture + (corner == 1u || corner == 2u ? 2u : 0u)]),
        uintBitsToFloat(frame.words[texture + (corner >= 2u ? 3u : 1u)])
    );
    out_attenuation = vec2(uintBitsToFloat(frame.words[instance + 4u]), 1.0);
    out_emission = vec3(0.0);
    out_terrain = vec3(0.0);
    out_terrain_fog = vec2(1.0, 0.0);
    out_world = world;
    out_modulation = vec3(1.0);
    const vec4 point = vec4(world, 1.0);
    vec2 pixel = vec2(dot(frame.horizontal, point), dot(frame.vertical, point));
    gl_Position = vec4(
        pixel.x * 2.0 / frame.viewport.x - 1.0,
        1.0 - pixel.y * 2.0 / frame.viewport.y,
        (depth_limit - dot(frame.depth, point)) / (2.0 * depth_limit),
        1.0
    );
}
