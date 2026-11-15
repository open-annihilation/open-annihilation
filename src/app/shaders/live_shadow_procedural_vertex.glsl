// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's units' shadows, cast from mixed corner patterns.
// Each unchanged run draws its casts' triangle corners in order. Exclusive
// corner prefixes select a cast and its local corner sequence, kept in the
// frame or in the immutable patterns. A cast with no corners repeats the
// prefix before it and draws nothing. The casts' placement and whole-pixel
// rounding are the same as live_shadow_cast_vertex.glsl.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

layout(set = 0, binding = 0, std430) readonly buffer Corners {
    uint corners[];
};

// The same camera, model draws and nine-word casts as the indexed stage.
// Descriptor and dynamic corner offsets are relative to words[].
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

layout(set = 0, binding = 2, std430) readonly buffer Patterns {
    uint pattern_words[];
};

// One descriptor per cast, from one through 256: its exclusive prefix,
// source offset, source kind (0 frame, 1 retained pattern), and a zero word.
layout(set = 1, binding = 0, std140) uniform Run {
    uint descriptor_word_base;
    uint cast_count;
    uint first_cast;
    uint reserved;
}
run;

layout(location = 0) out float out_height;
layout(location = 1) flat out vec2 out_cast;

void main() {
    const uint descriptor_words = 4u;
    const uint retained_pattern_source = 1u;
    const uint model_corner_bits = 16u;
    // A procedural draw starts at vertex zero, within its unchanged run.
    const uint ordinal = uint(gl_VertexIndex);
    uint relative_cast = 0u;
    uint end = run.cast_count;
    while (relative_cast < end) {
        const uint middle = relative_cast + (end - relative_cast) / 2u;
        const uint prefix = frame.words[run.descriptor_word_base + middle * descriptor_words];
        if (prefix <= ordinal)
            relative_cast = middle + 1u;
        else
            end = middle;
    }
    const uint descriptor = run.descriptor_word_base + relative_cast * descriptor_words;
    const uint previous_end = relative_cast == 0u
                                  ? 0u
                                  : frame.words[descriptor - descriptor_words];
    const uint source = frame.words[descriptor + 1u] + ordinal - previous_end;
    const uint local_corner = frame.words[descriptor + 2u] == retained_pattern_source
                                  ? pattern_words[source]
                                  : frame.words[source];
    const uint index = ((run.first_cast + relative_cast) << model_corner_bits) | local_corner;
    // The game lays a shadow this far right of its model.
    const float shadow_offset_x = 5.0;
    const uint record = frame.casts + (index >> 16) * 9u;
    const uint instance = frame.instances + frame.words[record] * 8u;
    const uint kind = frame.words[record + 1u];
    const float base = uintBitsToFloat(frame.words[record + 2u]);
    const float ground_half = uintBitsToFloat(frame.words[record + 3u]);
    const uint place = corners[(frame.words[instance] + (index & 0xffffu)) * 2u];
    const uint vertex = frame.positions + (frame.words[instance + 1u] + (place & 0xffffu)) * 3u;
    const vec3 origin = vec3(
        uintBitsToFloat(frame.words[instance + 5u]),
        uintBitsToFloat(frame.words[instance + 6u]),
        uintBitsToFloat(frame.words[instance + 7u])
    );
    precise vec3 world = origin + vec3(
        uintBitsToFloat(frame.words[vertex]),
        uintBitsToFloat(frame.words[vertex + 1u]),
        uintBitsToFloat(frame.words[vertex + 2u])
    );
    // The corner's whole pixels from the model's place, as the game's
    // picture takes them, and its height's half and quarter, rounded down.
    const vec3 model_place = vec3(
        uintBitsToFloat(frame.words[record + 7u]), base, uintBitsToFloat(frame.words[record + 8u])
    );
    precise vec3 relative = world - model_place;
    const vec3 whole = floor(relative);
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
