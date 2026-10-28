// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The live renderer's scene, fragment stage (renderer_live_gpu.cpp): the
// atlas's terrain levels, the water's art moved by its fine ripples,
// sprites and models, the water's shore and underwater light, lit and
// blended. tools/make_live_shaders.py
// compiles it for every graphics interface the renderer draws with.
#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec2 in_attenuation;
layout(location = 2) in vec3 in_emission;
layout(location = 3) in vec3 in_terrain;
layout(location = 4) in vec2 in_terrain_fog;
layout(location = 5) in vec3 in_world;
layout(location = 6) in vec3 in_modulation;

// The atlas, read by its nearest texel and filtered.
layout(set = 2, binding = 0) uniform sampler2D atlas_nearest;
layout(set = 2, binding = 1) uniform sampler2D atlas_linear;

// The vertex stage's block, as renderer_live_gpu.cpp fills it.
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

layout(location = 0) out vec4 out_colour;

// Mip levels occupy vertically stacked squares in a 32 by 64 chain.
vec4 terrain_level(vec2 origin, vec2 local, uint mip) {
    const float base_size = 32.0;
    const float size = base_size / float(1u << mip);
    const float offset = mip == 0u ? 0.0 : 64.0 - 2.0 * size;
    const vec2 position =
        origin + vec2(0.0, offset) + clamp(local / float(1u << mip), vec2(0.5), vec2(size - 0.5));
    return texture(atlas_linear, position / vec2(textureSize(atlas_linear, 0)));
}

// Water ripples' fine ripples (renderer_water_surface.hpp's art_waves),
// as the vertex stage's swells: heading, wavelength, speed and amplitude.
const vec4 water_ripples[2] = vec4[2](vec4(0.37, 29.0, 1.35, 0.6), vec4(-0.82, 23.0, 1.1, 0.45));

// How far the fine ripples move the water's art at a point under the
// water, in texels: the vertex stage moves the ground by the swells, which
// its corners are too far apart for.
vec2 water_fine_offset(vec3 world) {
    const float depth = camera.water_scene.x - world.y;
    const float strength = camera.water.x * camera.water_scene.z * camera.water_scene.w *
                           camera.water_wind.z * smoothstep(0.0, 12.0, depth);
    if (depth <= 0.0 || strength <= 0.0)
        return vec2(0.0);
    const vec2 flow = camera.water_wind.xy;
    const vec2 point = vec2(world.x, world.z + depth * 0.5);
    vec2 offset = vec2(0.0);
    for (int wave = 0; wave < 2; ++wave) {
        const vec4 ripple = water_ripples[wave];
        const vec2 heading = vec2(flow.x * cos(ripple.x) - flow.y * sin(ripple.x),
                                  flow.x * sin(ripple.x) + flow.y * cos(ripple.x));
        const float travel = dot(heading, point - camera.water_drift.xy * ripple.z);
        offset += heading * ripple.w * sin(travel * 6.283185307 / ripple.y);
    }
    return offset * strength;
}

void main() {
    vec4 colour;
    const float time = camera.water_scene.y;
    const float detail = camera.water_scene.z;
    const vec2 atlas_size = vec2(textureSize(atlas_nearest, 0));
    if (in_terrain.z == -4.0) {
        colour = vec4(0.66, 0.85, 0.92, 1.0);
    } else if (in_terrain.z == -2.0) {
        // Coastal waves' foam, as renderer_water_surface.hpp's foam_cover
        // and foam_alpha give it: a wash along the shore and two trains of
        // crests running in to it, gentler on sheltered water.
        const float depth = in_terrain.x;
        const float shore = (1.0 - smoothstep(3.0, 24.0, depth)) * smoothstep(0.0, 1.0, depth);
        const float crest = 0.5 + 0.5 * sin(depth * 0.8 + time * 1.2);
        const float following = 0.5 + 0.5 * sin(depth * 0.55 + time * 0.9 + 1.4);
        const float cover =
            depth < 24.0 ? shore * (0.3 + 0.7 * (0.65 * crest * crest * crest +
                                                 0.35 * following * following * following))
                         : 0.0;
        const float shelter = 0.35 + 0.65 * clamp(in_terrain.y, 0.0, 1.0);
        const float foam = min(
            0.85,
            camera.water.y * shelter * (0.12 + 0.88 * camera.water_wind.z) * cover
        );
        const float visibility = in_attenuation.x;
        // White here: the vertex's material colour below gives the foam the
        // colour of the ground beneath it.
        colour = vec4(vec3(1.0), foam * visibility * detail);
    } else if (in_terrain.z > 0.0) {
        const vec2 local = in_uv * atlas_size - in_terrain.xy;
        const float footprint =
            max(length(dFdx(local)), length(dFdy(local))) * camera.viewport.z;
        const float lod = clamp(log2(max(footprint, 1.0)), 0.0, 5.0);
        const uint lower = uint(floor(lod));
        const uint upper = min(lower + 1u, 5u);
        // The art the fine ripples move here, within its own tile.
        const vec2 art = local - water_fine_offset(in_world);
        if (footprint <= 1.0) {
            const vec2 position = in_terrain.xy + clamp(art, vec2(0.5), vec2(31.5));
            colour = texture(atlas_nearest, position / atlas_size);
        } else {
            colour = mix(
                terrain_level(in_terrain.xy, art, lower),
                terrain_level(in_terrain.xy, art, upper),
                fract(lod)
            );
        }
        if (in_terrain_fog.y > 0.0) {
            const vec2 grey_origin = in_terrain.xy + vec2(32.0, 0.0);
            vec4 grey;
            if (footprint <= 1.0) {
                const vec2 position = grey_origin + clamp(art, vec2(0.5), vec2(31.5));
                grey = texture(atlas_nearest, position / atlas_size);
            } else {
                grey = mix(
                    terrain_level(grey_origin, art, lower),
                    terrain_level(grey_origin, art, upper),
                    fract(lod)
                );
            }
            colour = mix(colour, grey, in_terrain_fog.y);
        }
    } else if (in_terrain.z == -6.0 || in_terrain.z == -7.0) {
        // Map scenery. Nearest for a feature's own pixels, filtered when the
        // frame asks for it. Where the ground is grey the same grey image is
        // mixed in; the frame is not tinted as a unit under the water.
        if (in_terrain.z == -7.0) {
            colour = texture(atlas_linear, in_uv);
            if (colour.a > 0.0)
                colour.rgb /= colour.a;
            if (in_terrain_fog.y > 0.0) {
                vec4 grey = texture(atlas_linear, in_terrain.xy);
                if (grey.a > 0.0)
                    grey.rgb /= grey.a;
                colour = mix(colour, grey, in_terrain_fog.y);
            }
        } else {
            colour = texture(atlas_nearest, in_uv);
            if (in_terrain_fog.y > 0.0) {
                vec4 grey = texture(atlas_nearest, in_terrain.xy);
                colour = mix(colour, grey, in_terrain_fog.y);
            }
        }
    } else if (in_terrain.z < 0.0) {
        colour = texture(atlas_linear, in_uv);
        // Atlas coverage is binary; filtering it gives premultiplied RGB. The
        // pipeline uses straight alpha, so recover the colour before applying
        // vertex opacity.
        if (colour.a > 0.0)
            colour.rgb /= colour.a;
    } else {
        colour = texture(atlas_nearest, in_uv);
    }
    const float brightness =
        in_terrain.z == -2.0
            ? 1.0
            : (in_terrain.z > 0.0 || in_terrain.z == -6.0 || in_terrain.z == -7.0
                   ? in_terrain_fog.x
                   : in_attenuation.x);
    colour.rgb = colour.rgb * in_modulation * brightness + in_emission;
    // Optical underwater billboards share their event depth across the
    // complete sprite. Physical models retain their actual per-fragment
    // water intersection.
    const float depth =
        in_terrain.z == -5.0 ? in_terrain.x : camera.water_scene.x - in_world.y;
    if (depth > 0.5 && in_terrain.z != -2.0 && in_terrain.z != -3.0 && in_terrain.z != -6.0 &&
        in_terrain.z != -7.0 && camera.water.z > 0.0 && detail > 0.0) {
        const float shallow = 1.0 - smoothstep(
                                        camera.appearance3.z,
                                        camera.appearance3.z + camera.appearance3.w,
                                        depth
                                    );
        // Fine, restrained seabed patterns sit beneath the map's baked water
        // art. Submerged objects retain stronger highlights so their depth
        // remains legible.
        const bool on_terrain = in_terrain.z > 0.0;
        const float frequency = on_terrain ? 3.0 : 1.0;
        const float wave = sin(
            in_world.x * 0.11 * frequency + time * 0.9 +
            sin(in_world.z * 0.07 * frequency - time * 0.5) * 1.6
        );
        const float crossing = sin(
            in_world.z * 0.1 * frequency - time * 0.7 +
            sin(in_world.x * 0.065 * frequency + time * 0.4)
        );
        const float caustic = smoothstep(0.45, 0.96, max(wave, crossing));
        const float intensity =
            camera.water.z * detail * smoothstep(0.5, camera.appearance0.x, depth);
        if (in_terrain.z <= 0.0) {
            const float depth_fraction = clamp(depth / camera.appearance0.y, 0.0, 1.0);
            const float tint = clamp(
                (camera.appearance0.z + camera.appearance0.w * depth_fraction) * intensity,
                0.0,
                camera.appearance1.w
            );
            colour.rgb = mix(colour.rgb, colour.rgb * camera.appearance1.xyz, tint);
            colour.rgb *= 1.0 - min(
                                    camera.appearance2.z,
                                    (camera.appearance2.x + camera.appearance2.y * depth_fraction) *
                                        intensity
                                );
        }
        const float modulation = on_terrain ? caustic * 0.06 - 0.01
                                            : caustic * camera.appearance3.x + camera.appearance3.y;
        colour.rgb *= 1.0 + modulation * intensity * shallow;
    }
    // Actual interpolated surface intersection, confined to covered model
    // fragments. Geometry stays fixed so crossing triangles cannot displace
    // their dry portion.
    if (depth > 0.0 && depth < camera.appearance4.y && in_terrain.z <= 0.0 &&
        in_terrain.z >= -1.0 && camera.water.z > 0.0 && detail > 0.0) {
        const float rim = depth < camera.appearance4.x
                              ? depth / camera.appearance4.x
                              : (camera.appearance4.y - depth) /
                                    (camera.appearance4.y - camera.appearance4.x);
        colour.rgb = mix(
            colour.rgb,
            camera.appearance5.xyz * brightness,
            clamp(rim * camera.water.z * detail * camera.appearance4.z, 0.0, 0.48)
        );
    }
    if (in_terrain.z == -3.0) {
        const float bands = 0.82 + 0.18 * sin(in_world.z * 0.35 + time * 1.5);
        colour.a *= bands * smoothstep(0.0, 2.0, in_terrain.x);
    }
    colour.a *= in_attenuation.y;
    if (colour.a == 0.0)
        discard;
    out_colour = colour;
}
