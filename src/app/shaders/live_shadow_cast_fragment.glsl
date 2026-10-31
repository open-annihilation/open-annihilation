// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's units' shadows, cast: fragment stage
// (renderer_live_gpu.cpp). Each texel a cast covers takes its light,
// multiplied into the shadows' map, unless the corner's height above the
// model's base does not pass the cast's: the part of a unit under the
// water, or of a digger under the ground, casts none.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

layout(location = 0) in float in_height;
layout(location = 1) flat in vec2 in_cast;

layout(location = 0) out vec4 out_light;

void main() {
    if (in_height <= in_cast.x)
        discard;
    out_light = vec4(in_cast.y);
}
