// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Strategic markers' four CPU-placed corners, kept once each in the frame's
// storage buffer and drawn as the same two triangles as the scene's vertices.
// The scene's and surface's fragment stages read the same attributes.
// tools/make_live_shaders.py compiles this for each graphics interface.
#version 450

// The first four rows of the scene's camera block, with the same layout.
layout(set = 1, binding = 0, std140) uniform Camera {
    vec4 horizontal;
    vec4 vertical;
    vec4 depth;
    vec4 viewport;
}
camera;

// The marker packets' first word in the complete model-frame buffer, and
// their shared white texel. A packet has sixteen words: four x/height/z
// corners, brightness, then red, green and blue.
layout(set = 1, binding = 1, std140) uniform Marker {
    uint first_word;
    float u;
    float v;
    uint pad;
}
marker;

layout(set = 0, binding = 0, std430) readonly buffer Frame {
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
    // gl_VertexIndex includes the draw's first vertex, so each run keeps
    // its place among the packets without changing their first word.
    const uint index = uint(gl_VertexIndex);
    const uint fan = index % 6u;
    const uint corner_index = fan == 3u ? 0u : (fan > 3u ? fan - 2u : fan);
    const uint packet = marker.first_word + (index / 6u) * 16u;
    const uint corner = packet + corner_index * 3u;
    const vec3 in_world = vec3(
        uintBitsToFloat(frame.words[corner]),
        uintBitsToFloat(frame.words[corner + 1u]),
        uintBitsToFloat(frame.words[corner + 2u])
    );
    vec4 world = vec4(in_world, 1.0);
    vec2 pixel = vec2(dot(camera.horizontal, world), dot(camera.vertical, world));
    gl_Position = vec4(
        pixel.x * 2.0 / camera.viewport.x - 1.0,
        1.0 - pixel.y * 2.0 / camera.viewport.y,
        (depth_limit - dot(camera.depth, world)) / (2.0 * depth_limit),
        1.0
    );
    out_uv = vec2(marker.u, marker.v);
    out_attenuation = vec2(uintBitsToFloat(frame.words[packet + 12u]), 1.0);
    out_emission = vec3(0.0);
    out_terrain = vec3(0.0);
    out_terrain_fog = vec2(1.0, 0.0);
    out_world = in_world;
    out_modulation = vec3(
        uintBitsToFloat(frame.words[packet + 13u]),
        uintBitsToFloat(frame.words[packet + 14u]),
        uintBitsToFloat(frame.words[packet + 15u])
    );
}
