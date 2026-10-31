// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's lights' surface, fragment stage
// (renderer_live_gpu.cpp): the scene's triangles drawn again, a sample a
// pixel at the picture's size and after the scene's vertex stage, keeping
// under each pixel's centre the nearest face opaque to the lights, its
// camera depth and visibility, for the lights pass to light.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec2 in_attenuation;
layout(location = 5) in vec3 in_world;

// The atlas, read by its nearest texel.
layout(set = 2, binding = 0) uniform sampler2D atlas_nearest;

// The scene's vertex stage's block, as renderer_live_gpu.cpp fills it.
layout(set = 3, binding = 0, std140) uniform Camera {
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

// The face's camera depth, larger nearer, and its visibility in [0,1].
layout(location = 0) out vec2 out_surface;

void main() {
    // A face whose opacity, and its atlas texel's alpha (0 to 255) times
    // it, reach one half is opaque to the lights; any other leaves what
    // lies behind it, and its depth hides nothing.
    const float opacity = in_attenuation.y;
    const float alpha = round(textureLod(atlas_nearest, in_uv, 0.0).a * 255.0);
    if (opacity * 255.0 < 128.0 || alpha * opacity < 128.0)
        discard;
    out_surface = vec2(dot(camera.depth, vec4(in_world, 1.0)), clamp(in_attenuation.x, 0.0, 1.0));
}
