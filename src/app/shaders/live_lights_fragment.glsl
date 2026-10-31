// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's lights, fragment stage (renderer_live_gpu.cpp): the
// weapons', nanolathes' and wrecks' light, glow and heat added to the drawn
// picture as renderer_effects::compose adds them on the processor, each
// light reaching the surface under each pixel's centre
// (live_surface_fragment.glsl): its camera depth limits the light's reach
// and keeps glow and heat off whatever stands in front of the light, and its
// visibility lets the light through.
// tools/make_live_shaders.py compiles it for every graphics interface the
// renderer draws with.
#version 450

// The picture as the scene drew it; the surface under each pixel, its depth
// and visibility; the lights' table, four texels a light
// (renderer_live_lights.hpp); and each tile's lights, a bit a light.
layout(set = 2, binding = 0) uniform sampler2D picture;
layout(set = 2, binding = 1) uniform sampler2D surface;
layout(set = 2, binding = 2) uniform sampler2D lights;
layout(set = 2, binding = 3) uniform usampler2D tiles;

// renderer_live_lights::parameters: compose's light and glow energies, the
// glow core's squared share of the radius and the share of the depth reach
// that stands in front; the picture's last column and row, heat's ripple in
// radians a row and its widest move; the tiles' edge.
layout(set = 3, binding = 0, std140) uniform Parameters {
    vec4 energy;
    vec4 frame;
    vec4 tiling;
};

layout(location = 0) out vec4 out_colour;

// The surface under a pixel: its camera depth, larger nearer, and its
// visibility as compose takes it, 0 to 255 as a whole number; 0 where no
// face opaque to the lights lies.
struct Surface {
    float depth;
    float visibility;
};

Surface surface_at(ivec2 pixel) {
    const vec2 held = texelFetch(surface, pixel, 0).xy;
    return Surface(held.x, floor(clamp(held.y, 0.0, 1.0) * 255.0));
}

// Rounds half away from zero.
vec3 round_away(vec3 value) {
    return sign(value) * floor(abs(value) + 0.5);
}

void main() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const vec4 drawn = texelFetch(picture, pixel, 0);
    out_colour = drawn;
    const uvec4 listed = texelFetch(tiles, pixel / int(tiling.x), 0);
    if (all(equal(listed, uvec4(0u))))
        return;
    const Surface here = surface_at(pixel);
    // A light reaches no pixel without a surface or with none of it in view.
    if (here.visibility == 0.0)
        return;
    const float visibility = here.visibility / 255.0;
    const vec3 undistorted = round(drawn.rgb * 255.0);
    vec3 colour = undistorted;
    const float x = float(pixel.x);
    const float y = float(pixel.y);
    // The lights in their order, each changing the pixel as the ones before
    // it left it.
    for (int word = 0; word < 4; ++word) {
        uint bits = listed[word];
        while (bits != 0u) {
            const int light = word * 32 + findLSB(bits);
            bits &= bits - 1u;
            // Centre and depth reach; radii, heat's phase and strength;
            // colour and light; glow.
            const vec4 place = texelFetch(lights, ivec2(0, light), 0);
            const vec4 reach = texelFetch(lights, ivec2(1, light), 0);
            const vec4 tint = texelFetch(lights, ivec2(2, light), 0);
            const float glow = texelFetch(lights, ivec2(3, light), 0).x;
            const float radius = reach.x;
            const float radius_y = reach.y;
            // The rows of the light's rectangle, and on each the columns of
            // its ellipse and one more either side.
            const int top = int(max(0.0, place.y - radius_y));
            const int bottom = int(min(frame.y, place.y + radius_y));
            if (pixel.y < top || pixel.y > bottom)
                continue;
            const float dy = (y - place.y) / radius_y;
            if (dy * dy >= 1.0)
                continue;
            const float half_width = radius * sqrt(1.0 - dy * dy);
            const int left = int(max(0.0, place.x - radius));
            const int right = int(min(frame.x, place.x + radius));
            const int first = max(left, int(floor(place.x - half_width)) - 1);
            const int last = min(right, int(ceil(place.x + half_width)) + 1);
            if (pixel.x < first || pixel.x > last || place.w <= 0.0)
                continue;
            const float dx = (x - place.x) / radius;
            const float distance_squared = dx * dx + dy * dy;
            const float depth_delta = here.depth - place.z;
            const float dz = depth_delta / place.w;
            const float falloff = 1.0 - distance_squared - dz * dz;
            if (falloff <= 0.0)
                continue;
            const float attenuation = falloff * falloff * visibility;
            const float silhouette = place.w * energy.w;
            const bool foreground = depth_delta > silhouette;
            if (reach.w > 0.0 && !foreground) {
                // Heat moves the pixel's sample along its row by a fraction of
                // a pixel, both of the sample's pixels on the pixel's own
                // surface: the same visibility, and depth within the share.
                const float displacement = sin(y * frame.z + reach.z) * frame.w * reach.w;
                const float sample_x = clamp(x + displacement * attenuation, 0.0, frame.x);
                const int sample_left = int(sample_x);
                const int sample_right = min(sample_left + 1, int(frame.x));
                const float fraction = sample_x - float(sample_left);
                const Surface left_surface = surface_at(ivec2(sample_left, pixel.y));
                const Surface right_surface = surface_at(ivec2(sample_right, pixel.y));
                const bool left_same = left_surface.visibility == here.visibility &&
                                       abs(left_surface.depth - here.depth) <= silhouette;
                const bool right_same = right_surface.visibility == here.visibility &&
                                        abs(right_surface.depth - here.depth) <= silhouette;
                if (left_same && (fraction == 0.0 || right_same)) {
                    const vec3 left_sample =
                        round(texelFetch(picture, ivec2(sample_left, pixel.y), 0).rgb * 255.0);
                    const vec3 right_sample =
                        round(texelFetch(picture, ivec2(sample_right, pixel.y), 0).rgb * 255.0);
                    const vec3 difference =
                        left_sample + (right_sample - left_sample) * fraction - undistorted;
                    colour = clamp(round_away(colour + difference), 0.0, 255.0);
                }
            }
            float added = tint.w * energy.x * attenuation;
            if (glow > 0.0 && !foreground) {
                const float core = max(0.0, 1.0 - distance_squared / energy.z);
                added += glow * energy.y * core * core * visibility;
            }
            // The light's whole steps, whatever the channel held.
            colour = min(vec3(255.0), colour + floor(added * tint.rgb));
        }
    }
    out_colour = vec4(colour / 255.0, drawn.a);
}
