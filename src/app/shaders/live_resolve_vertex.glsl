// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's supersampling resolve, vertex stage
// (renderer_live_gpu.cpp): one triangle that covers the target.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

void main() {
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
