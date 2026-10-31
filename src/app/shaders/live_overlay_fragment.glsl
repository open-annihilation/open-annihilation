// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's overlay, fragment stage (renderer_live_gpu.cpp): what
// the processor painted over the turned view added to the picture the card
// drew, as renderer_live_overlay::compose adds it on the processor. Each
// pixel the overlay canvas paints is the canvas's; every other takes the
// operations its tile lists that reach it, in their order, each line of
// text reduced to the palette once its operations have been made; then the
// display gamma. Every step is in whole levels, as the painters paint.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

// The picture; the canvas, transparent where it holds its key; the
// operations, two texels each (renderer_live_overlay::Packed); each tile's
// list of them; and the palette in row 0, a colour red | green << 8 |
// blue << 16 a texel, and in row 1 the level the display gamma shows for
// each.
layout(set = 2, binding = 0) uniform sampler2D picture;
layout(set = 2, binding = 1) uniform sampler2D canvas;
layout(set = 2, binding = 2) uniform usampler2D operations;
layout(set = 2, binding = 3) uniform usampler2D lists;
layout(set = 2, binding = 4) uniform usampler2D colours;

// The tiles across, their edge, the palette's entries (0 for none) and the
// operations a row of their table holds; the text's shade colour and its
// shadow's alpha; the outline's colour; the dark outline's colour.
layout(set = 3, binding = 0, std140) uniform Parameters {
    uvec4 tiling;
    uvec4 shade;
    uvec4 outline;
    uvec4 dark;
};

layout(location = 0) out vec4 out_colour;

// Words a row of the lists' table holds (renderer_live_overlay::list_words_per_row).
const uint list_words_per_row = 4096u;

uint list_word(uint at) {
    return texelFetch(lists, ivec2(int(at % list_words_per_row), int(at / list_words_per_row)), 0).x;
}

uvec3 unpacked(uint word) {
    return uvec3(word & 255u, (word >> 8) & 255u, (word >> 16) & 255u);
}

// oa::present::blend_color: each channel under + (over - under) * alpha / 255,
// rounded to nearest.
uvec3 blend_colour(uvec3 under, uvec3 over, uint alpha) {
    return (under * (255u - alpha) + over * alpha + 127u) / 255u;
}

// The palette's colour nearest a colour, the first of equal distance, as
// oa::present::nearest_palette_index finds it.
uvec3 reduced(uvec3 colour) {
    const uint entries = tiling.z;
    if (entries == 0u)
        return colour;
    uint best = 0u;
    int best_distance = 0x7fffffff;
    for (uint entry = 0u; entry < entries; ++entry) {
        const ivec3 delta =
            ivec3(unpacked(texelFetch(colours, ivec2(int(entry), 0), 0).x)) - ivec3(colour);
        const int distance = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (distance < best_distance) {
            best_distance = distance;
            best = entry;
        }
    }
    return unpacked(texelFetch(colours, ivec2(int(best), 0), 0).x);
}

void main() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const uvec2 place = uvec2(pixel);
    const vec4 painted = texelFetch(canvas, pixel, 0);
    uvec3 colour;
    if (painted.a > 0.5) {
        colour = uvec3(round(painted.rgb * 255.0));
    } else {
        colour = uvec3(round(texelFetch(picture, pixel, 0).rgb * 255.0));
        const uint edge = tiling.y;
        const uint tile = place.y / edge * tiling.x + place.x / edge;
        const uint end = list_word(tile + 1u);
        // The line whose operations last reached the pixel, not yet reduced.
        uint line = 0u;
        for (uint at = list_word(tile); at < end; ++at) {
            const uint index = list_word(at);
            const ivec2 texel = ivec2(int(index % tiling.w) * 2, int(index / tiling.w));
            const uvec4 area = texelFetch(operations, texel, 0);
            if (place.x < area.x || place.y < area.y || place.x > area.z || place.y > area.w)
                continue;
            const uvec4 operation = texelFetch(operations, texel + ivec2(1, 0), 0);
            if (line != 0u && operation.w != line)
                colour = reduced(colour);
            line = operation.w;
            const uvec3 own = unpacked(operation.y);
            if (operation.x == 0u) {
                // blend_rect: the colour at its opacity in 256ths, at most
                // the whole.
                const uint share = min(operation.z, 256u);
                colour = (colour * (256u - share) + own * share + 128u) / 256u;
            } else if (operation.x == 1u) {
                colour = blend_colour(colour, shade.rgb, shade.a);
            } else if (operation.x == 2u) {
                colour = colour.r + colour.g + colour.b < outline.r + outline.g + outline.b
                             ? dark.rgb
                             : outline.rgb;
            } else {
                colour = blend_colour(colour, own, min(operation.z, 255u));
            }
        }
        if (line != 0u)
            colour = reduced(colour);
    }
    const uvec3 shown = uvec3(
        texelFetch(colours, ivec2(int(colour.r), 1), 0).x,
        texelFetch(colours, ivec2(int(colour.g), 1), 0).x,
        texelFetch(colours, ivec2(int(colour.b), 1), 0).x
    );
    out_colour = vec4(vec3(shown) / 255.0, 1.0);
}
