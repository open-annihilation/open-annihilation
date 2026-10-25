// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's scene, vertex stage (renderer_live_gpu.cpp): world
// vertices through the camera's affine rows into clip space, every
// attribute handed on to the fragment stage. tools/make_live_shaders.py
// compiles it for every graphics interface the renderer draws with.
#version 450

layout(location = 0) in vec3 in_world;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec2 in_attenuation;
layout(location = 3) in vec3 in_emission;
layout(location = 4) in vec3 in_terrain;
layout(location = 5) in vec2 in_terrain_fog;
layout(location = 6) in vec3 in_modulation;

// The camera's rows and the scene's settings, as renderer_live_gpu.cpp
// fills them; the fragment stage reads the same block.
layout(set = 1, binding = 0, std140) uniform Camera {
    vec4 horizontal;
    vec4 vertical;
    vec4 depth;
    vec4 viewport;
    vec4 water;
    vec4 water_scene;
    vec4 water_wind;
    vec4 water_drift;
    vec4 appearance0;
    vec4 appearance1;
    vec4 appearance2;
    vec4 appearance3;
    vec4 appearance4;
    vec4 appearance5;
}
camera;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec2 out_attenuation;
layout(location = 2) out vec3 out_emission;
layout(location = 3) out vec3 out_terrain;
layout(location = 4) out vec2 out_terrain_fog;
layout(location = 5) out vec3 out_world;
layout(location = 6) out vec3 out_modulation;

void main() {
    const float depth_limit = 65536.0;
    vec4 world = vec4(in_world, 1.0);
    vec2 pixel = vec2(dot(camera.horizontal, world), dot(camera.vertical, world));
    gl_Position = vec4(
        pixel.x * 2.0 / camera.viewport.x - 1.0,
        1.0 - pixel.y * 2.0 / camera.viewport.y,
        (depth_limit - dot(camera.depth, world)) / (2.0 * depth_limit),
        1.0
    );
    out_uv = in_uv;
    out_attenuation = in_attenuation;
    out_emission = in_emission;
    out_modulation = in_modulation;
    out_terrain = in_terrain;
    out_terrain_fog = in_terrain_fog;
    out_world = in_world;
}
