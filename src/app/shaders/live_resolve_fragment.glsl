// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's supersampling resolve, fragment stage
// (renderer_live_gpu.cpp): each output pixel the rounded mean of its
// factor by factor block of the supersampled target, in whole levels.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

layout(set = 2, binding = 0) uniform sampler2D source;

// x: the supersampling factor along each axis.
layout(set = 3, binding = 0, std140) uniform Parameters {
    uvec4 parameters;
};

layout(location = 0) out vec4 out_colour;

void main() {
    const uint factor = parameters.x;
    const uint samples = factor * factor;
    const uvec2 origin = uvec2(gl_FragCoord.xy) * factor;
    uvec4 sum = uvec4(0u);
    for (uint y = 0u; y < factor; ++y)
        for (uint x = 0u; x < factor; ++x)
            sum += uvec4(round(texelFetch(source, ivec2(origin + uvec2(x, y)), 0) * 255.0));
    out_colour = vec4((sum + samples / 2u) / samples) / 255.0;
}
