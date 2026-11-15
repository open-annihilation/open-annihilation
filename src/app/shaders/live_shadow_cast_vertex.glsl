// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's units' shadows, cast: vertex stage
// (renderer_live_gpu.cpp). A model's triangle corner from its type's
// corners and its posed vertex with its instance's origin, laid in the shadows'
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
    uint root_prefix_poses;
    uint root_prefix_points;
    uint unused_3;
    vec4 shadow_map;
    uint words[];
}
frame;

// Root-prefix products use two 32-bit words, low word first. Four 16-bit
// partial products keep every operation in Vulkan 1.0 and Metal 2.1 integers.
uvec2 prefix_product(uint first, uint second) {
    const uint half_word_bits = 16u;
    const uint half_word_mask = 0xffffu;
    const uint low_low = (first & half_word_mask) * (second & half_word_mask);
    const uint low_high = (first & half_word_mask) * (second >> half_word_bits);
    const uint high_low = (first >> half_word_bits) * (second & half_word_mask);
    const uint high_high = (first >> half_word_bits) * (second >> half_word_bits);
    const uint middle = (low_low >> half_word_bits) + (low_high & half_word_mask) +
                        (high_low & half_word_mask);
    return uvec2(
        (middle << half_word_bits) | (low_low & half_word_mask),
        high_high + (low_high >> half_word_bits) + (high_low >> half_word_bits) +
            (middle >> half_word_bits)
    );
}

uvec2 prefix_negate(uvec2 value) {
    return uvec2(0u - value.x, ~value.y + (value.x == 0u ? 1u : 0u));
}

uvec2 prefix_add(uvec2 first, uvec2 second) {
    const uint low = first.x + second.x;
    return uvec2(low, first.y + second.y + (low < first.x ? 1u : 0u));
}

uvec2 prefix_signed_product(uint first, uint second) {
    const uint sign_bit = 0x80000000u;
    const uint first_magnitude = (first & sign_bit) != 0u ? 0u - first : first;
    const uint second_magnitude = (second & sign_bit) != 0u ? 0u - second : second;
    const uvec2 product = prefix_product(first_magnitude, second_magnitude);
    return ((first ^ second) & sign_bit) != 0u ? prefix_negate(product) : product;
}

// Round the signed Q30 pair to nearest, ties to even, keeping the low word.
uint prefix_rounded(uvec2 value) {
    const uint sign_bit = 0x80000000u;
    const uint fraction_bits = 30u;
    const uint word_bits = 32u;
    const uint fraction_mask = (1u << fraction_bits) - 1u;
    const uint half_fraction = 1u << (fraction_bits - 1u);
    const bool negative = (value.y & sign_bit) != 0u;
    const uvec2 magnitude = negative ? prefix_negate(value) : value;
    uint rounded = (magnitude.x >> fraction_bits) |
                   (magnitude.y << (word_bits - fraction_bits));
    const uint remainder = magnitude.x & fraction_mask;
    if (remainder > half_fraction || (remainder == half_fraction && (rounded & 1u) != 0u))
        ++rounded;
    return negative ? 0u - rounded : rounded;
}

// Build the binary32 result of signed-integer conversion followed by /65536.
// Explicit mantissa rounding avoids backend-dependent integer-to-float rounding.
float prefix_world_coordinate(uint value) {
    const uint sign_bit = 0x80000000u;
    const uint mantissa_bits = 23u;
    const uint mantissa_mask = (1u << mantissa_bits) - 1u;
    const uint exponent_bias = 127u;
    const uint fixed_fraction_bits = 16u;
    const uint sign = value & sign_bit;
    const uint magnitude = sign != 0u ? 0u - value : value;
    if (magnitude == 0u)
        return 0.0;
    uint leading = uint(findMSB(magnitude));
    uint significand;
    if (leading <= mantissa_bits) {
        significand = magnitude << (mantissa_bits - leading);
    } else {
        const uint shift = leading - mantissa_bits;
        significand = magnitude >> shift;
        const uint remainder = magnitude & ((1u << shift) - 1u);
        const uint half_fraction = 1u << (shift - 1u);
        if (remainder > half_fraction ||
            (remainder == half_fraction && (significand & 1u) != 0u))
            ++significand;
        if (significand == (1u << (mantissa_bits + 1u))) {
            significand >>= 1u;
            ++leading;
        }
    }
    const uint exponent = leading + exponent_bias - fixed_fraction_bits;
    return uintBitsToFloat(sign | (exponent << mantissa_bits) | (significand & mantissa_mask));
}

// A descriptor has seven words: first point, count, Q30 sine and cosine,
// then signed fixed root placement. Its points are already unplaced.
vec3 model_local_position(uint instance, uint local_point) {
    const uint prefix = frame.words[instance + 8u];
    if (prefix == 0u) {
        const uint vertex = frame.positions + (frame.words[instance + 1u] + local_point) * 3u;
        return vec3(
            uintBitsToFloat(frame.words[vertex]),
            uintBitsToFloat(frame.words[vertex + 1u]),
            uintBitsToFloat(frame.words[vertex + 2u])
        );
    }
    const uint pose_words = 7u;
    const uint pose = frame.root_prefix_poses + (prefix - 1u) * pose_words;
    const uint vertex = frame.root_prefix_points + (frame.words[pose] + local_point) * 3u;
    const uvec3 point = uvec3(
        frame.words[vertex], frame.words[vertex + 1u], frame.words[vertex + 2u]
    );
    const uint sine = frame.words[pose + 2u];
    const uint cosine = frame.words[pose + 3u];
    const uint x = prefix_rounded(prefix_add(
        prefix_signed_product(point.x, cosine), prefix_negate(prefix_signed_product(point.z, sine))
    ));
    const uint z = prefix_rounded(prefix_add(
        prefix_signed_product(point.x, sine), prefix_signed_product(point.z, cosine)
    ));
    const uvec3 placed = uvec3(x, point.y, z) + uvec3(
        frame.words[pose + 4u], frame.words[pose + 5u], frame.words[pose + 6u]
    );
    return vec3(
        prefix_world_coordinate(placed.x),
        prefix_world_coordinate(placed.y),
        -prefix_world_coordinate(placed.z)
    );
}


layout(location = 0) out float out_height;
layout(location = 1) flat out vec2 out_cast;

void main() {
    // The game lays a shadow this far right of its model.
    const float shadow_offset_x = 5.0;
    const uint index = uint(gl_VertexIndex);
    const uint record = frame.casts + (index >> 16) * 9u;
    const uint instance = frame.instances + frame.words[record] * 9u;
    const uint kind = frame.words[record + 1u];
    const float base = uintBitsToFloat(frame.words[record + 2u]);
    const float ground_half = uintBitsToFloat(frame.words[record + 3u]);
    const uint place = corners[(frame.words[instance] + (index & 0xffffu)) * 2u];
    const vec3 origin = vec3(
        uintBitsToFloat(frame.words[instance + 5u]),
        uintBitsToFloat(frame.words[instance + 6u]),
        uintBitsToFloat(frame.words[instance + 7u])
    );
    precise vec3 world = origin + model_local_position(instance, place & 0xffffu);
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
