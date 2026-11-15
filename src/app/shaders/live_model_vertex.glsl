// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's models, vertex stage (renderer_live_gpu.cpp): a unit's
// or feature's triangle corner from its type's corners, which stay on the
// card, its posed vertex with its instance's origin and its texture's atlas place,
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
// camera's first four rows, where each part begins and the shadows' map,
// then the instances, nine words each, the vertices, the pieces' frames,
// the textures and the shadows cast (live_shadow_cast_vertex.glsl).
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


layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec2 out_attenuation;
layout(location = 2) out vec3 out_emission;
layout(location = 3) out vec3 out_terrain;
layout(location = 4) out vec2 out_terrain_fog;
layout(location = 5) out vec3 out_world;
layout(location = 6) out vec3 out_modulation;
layout(location = 7) flat out uint out_model_flags;

void main() {
    const float depth_limit = 65536.0;
    // The index names the instance above its corner bits; the run's first
    // instance comes as the draw's base.
    const uint index = uint(gl_VertexIndex);
    const uint instance = frame.instances + (index >> 16) * 9u;
    const uint corner_at = (frame.words[instance] + (index & 0xffffu)) * 2u;
    const uint place = corners[corner_at];
    const uint drawn = corners[corner_at + 1u];
    const vec3 origin = vec3(
        uintBitsToFloat(frame.words[instance + 5u]),
        uintBitsToFloat(frame.words[instance + 6u]),
        uintBitsToFloat(frame.words[instance + 7u])
    );
    precise vec3 world = origin + model_local_position(instance, place & 0xffffu);
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
    const uint model_cull_back_faces = 1u;
    out_model_flags = frame.words[instance + 8u] != 0u ? model_cull_back_faces : 0u;
    const vec4 point = vec4(world, 1.0);
    vec2 pixel = vec2(dot(frame.horizontal, point), dot(frame.vertical, point));
    gl_Position = vec4(
        pixel.x * 2.0 / frame.viewport.x - 1.0,
        1.0 - pixel.y * 2.0 / frame.viewport.y,
        (depth_limit - dot(frame.depth, point)) / (2.0 * depth_limit),
        1.0
    );
}
