// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's units' shadows, laid on the ground: vertex stage
// (renderer_live_gpu.cpp). The frame's ground drawn again, as the scene's
// vertex stage draws it, the ground under the water moved by the ripples'
// swells, a twentieth of a pixel above it, as the game's decals lie; each
// corner hands on where it lies in the map as the game paints it, which
// the shadows' map spans. tools/make_live_shaders.py compiles it for every
// graphics interface the renderer draws with.
#version 450

layout(location = 0) in vec3 in_world;
layout(location = 4) in vec3 in_terrain;

// The scene's camera block, as renderer_live_gpu.cpp fills it.
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

// Water ripples' broad swells, as live_scene_vertex.glsl's.
const vec4 water_swells[3] = vec4[3](
    vec4(0.0, 71.0, 0.65, 1.6), vec4(0.66, 113.0, 0.28, 1.2), vec4(-0.51, 53.0, 0.45, 0.9)
);

layout(location = 0) out vec2 out_painted;

void main() {
    const float depth_limit = 65536.0;
    const float surface_lift = 0.05;
    vec4 world = vec4(in_world, 1.0);
    const float depth = camera.water_scene.x - in_world.y;
    if (in_terrain.z > 0.0 && depth > 0.0 && camera.water.x > 0.0 && camera.water_scene.z > 0.0) {
        const vec2 flow = camera.water_wind.xy;
        const vec2 point = vec2(in_world.x, in_world.z + depth * 0.5);
        vec2 offset = vec2(0.0);
        for (int wave = 0; wave < 3; ++wave) {
            const vec4 swell = water_swells[wave];
            const vec2 heading =
                vec2(flow.x * cos(swell.x) - flow.y * sin(swell.x),
                     flow.x * sin(swell.x) + flow.y * cos(swell.x));
            const float travel = dot(heading, point - camera.water_drift.xy * swell.z);
            offset += heading * swell.w * sin(travel * 6.283185307 / swell.y);
        }
        world.xz += offset * camera.water_wind.z * camera.water.x * camera.water_scene.z *
                    smoothstep(0.0, 12.0, depth);
    }
    world.y += surface_lift;
    vec2 pixel = vec2(dot(camera.horizontal, world), dot(camera.vertical, world));
    gl_Position = vec4(
        pixel.x * 2.0 / camera.viewport.x - 1.0,
        1.0 - pixel.y * 2.0 / camera.viewport.y,
        (depth_limit - dot(camera.depth, world)) / (2.0 * depth_limit),
        1.0
    );
    out_painted = vec2(in_world.x, in_world.z - in_world.y * 0.5);
}
