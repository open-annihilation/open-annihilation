// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_live_gpu.hpp"
#include "renderer_water_appearance.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace oa::app::renderer_live_gpu {
namespace {

constexpr uint32_t channels = 4;
constexpr uint32_t max_dimension = 4096;
constexpr size_t max_vertices = 3 * 1024 * 1024;
constexpr float coordinate_limit = 32768.0f;
constexpr float projection_limit = 131072.0f;
constexpr uint32_t vertices_per_triangle = 3;
constexpr uint32_t max_supersample = 16;

/// Check dimensions before calculating the byte size of a bounded RGBA image.
bool valid_dimensions(uint32_t width, uint32_t height) {
    return width != 0 && height != 0 && width <= max_dimension && height <= max_dimension;
}

#if defined(__APPLE__)
constexpr char shader_source[] = R"(
#include <metal_stdlib>
using namespace metal;
struct Input {
    float3 world [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float2 attenuation [[attribute(2)]];
    float3 emission [[attribute(3)]];
    float3 terrain [[attribute(4)]];
    float2 terrain_fog [[attribute(5)]];
    float3 modulation [[attribute(6)]];
};
struct Output {
    float4 position [[position]];
    float2 uv;
    float2 attenuation;
    float3 emission;
    float3 terrain;
    float2 terrain_fog;
    float filter_scale [[flat]];
    float3 world;
    float3 modulation;
    float4 water [[flat]];
    float4 water_scene [[flat]];
    float4 water_wind [[flat]];
    float4 water_drift [[flat]];
    float4 appearance0 [[flat]];
    float4 appearance1 [[flat]];
    float4 appearance2 [[flat]];
    float4 appearance3 [[flat]];
    float4 appearance4 [[flat]];
    float4 appearance5 [[flat]];

};
struct Camera { float4 horizontal; float4 vertical; float4 depth; float4 viewport; float4 water; float4 water_scene; float4 water_wind; float4 water_drift; float4 appearance0; float4 appearance1; float4 appearance2; float4 appearance3; float4 appearance4; float4 appearance5; };
vertex Output vertex_main(Input input [[stage_in]], constant Camera& camera [[buffer(0)]]) {
    const float depth_limit = 65536.0;
    float4 world = float4(input.world, 1.0);
    float2 pixel = float2(dot(camera.horizontal, world), dot(camera.vertical, world));
    Output output;
    output.position = float4(pixel.x * 2.0 / camera.viewport.x - 1.0,
        1.0 - pixel.y * 2.0 / camera.viewport.y,
        (depth_limit - dot(camera.depth, world)) / (2.0 * depth_limit), 1.0);
    output.uv = input.uv;
    output.attenuation = input.attenuation;
    output.emission = input.emission;
    output.modulation = input.modulation;
    output.terrain = input.terrain;
    output.terrain_fog = input.terrain_fog;
    output.filter_scale = camera.viewport.z;
    output.world = input.world;
    output.water = camera.water;
    output.water_scene = camera.water_scene;
    output.water_wind = camera.water_wind;
    output.water_drift = camera.water_drift;
    output.appearance0 = camera.appearance0;
    output.appearance1 = camera.appearance1;
    output.appearance2 = camera.appearance2;
    output.appearance3 = camera.appearance3;
    output.appearance4 = camera.appearance4;
    output.appearance5 = camera.appearance5;

    return output;
}
// Mip levels occupy vertically stacked squares in a 32 by 64 chain.
float4 terrain_level(texture2d<float> atlas, float2 origin, float2 local, uint mip) {
    constexpr sampler linear_sampler(coord::normalized, address::clamp_to_edge, filter::linear);
    const float base_size = 32.0;
    const float size = base_size / float(1u << mip);
    const float offset = mip == 0 ? 0.0 : 64.0 - 2.0 * size;
    const float2 position = origin + float2(0.0, offset) +
        clamp(local / float(1u << mip), float2(0.5), float2(size - 0.5));
    return atlas.sample(linear_sampler,
        position / float2(atlas.get_width(), atlas.get_height()));
}
float water_hash(int2 cell) {
    uint hash = uint(cell.x) * 0x8da6b343u ^ uint(cell.y) * 0xd8163841u;
    hash ^= hash >> 13;
    hash *= 0x85ebca6bu;
    hash ^= hash >> 16;
    return float(hash & 65535u) / 65535.0;
}
float water_noise(float2 point) {
    const int2 cell = int2(floor(point));
    float2 weight = fract(point);
    weight = weight * weight * (3.0 - 2.0 * weight);
    return mix(mix(water_hash(cell), water_hash(cell + int2(1, 0)), weight.x),
               mix(water_hash(cell + int2(0, 1)), water_hash(cell + int2(1, 1)), weight.x), weight.y);
}
fragment float4 fragment_main(Output input [[stage_in]],
    texture2d<float> atlas [[texture(0)]], sampler nearest [[sampler(0)]]) {
    float4 colour;
    const float time = input.water_scene.y;
    const float detail = input.water_scene.z;
    if (input.terrain.z == -4.0) {
        colour = float4(0.66, 0.85, 0.92, 1.0);
    } else if (input.terrain.z == -2.0) {
        const float depth = input.terrain.x;
        const float2 medium = input.world.xz - input.water_drift.xy * 0.65;
        const float2 long_wave = input.world.xz - input.water_drift.xy * 0.28;
        const float2 fine_point = input.world.xz - input.water_drift.xy * 1.35;
        const float broad = 0.65 * water_noise(medium / float2(34.0, 22.0)) +
            0.35 * water_noise((long_wave + float2(37.0, -19.0)) / float2(79.0, 53.0));
        const float fine = water_noise((fine_point + float2(11.0, -7.0)) / float2(8.0, 11.0));
        const float ripple = mix(smoothstep(0.48, 0.82, broad),
            smoothstep(0.48, 0.82, fine), input.water_scene.w * 0.4);
        const float wind_strength = 0.12 + 0.88 * input.water_wind.z;
        const float shore = (1.0 - smoothstep(3.0, 24.0, depth)) * smoothstep(0.0, 1.0, depth);
        const float crest = pow(max(0.0, sin(depth * 0.9 + time * 1.1)), 8.0);
        const float following = pow(max(0.0, sin(depth * 0.67 + time * 0.83 + 1.4)), 8.0);
        const float foam = shore * (crest * 0.7 + following * 0.3) * input.water.y *
            input.terrain.y * 0.090;
        const float sheen = ripple * input.water.x * 0.14 * wind_strength;
        const float visibility = input.attenuation.x;
        colour = float4(float3(0.72, 0.86, 0.90), (foam + sheen) * visibility * detail);
    } else if (input.terrain.z > 0.0) {
        const float2 local = input.uv * float2(atlas.get_width(), atlas.get_height()) -
            input.terrain.xy;
        const float footprint = max(length(dfdx(local)), length(dfdy(local))) * input.filter_scale;
        const float lod = clamp(log2(max(footprint, 1.0)), 0.0, 5.0);
        const uint lower = uint(floor(lod));
        const uint upper = min(lower + 1u, 5u);
        if (footprint <= 1.0) {
            const float2 position = input.terrain.xy + clamp(local, float2(0.5), float2(31.5));
            colour = atlas.sample(nearest,
                position / float2(atlas.get_width(), atlas.get_height()));
        } else {
            colour = mix(terrain_level(atlas, input.terrain.xy, local, lower),
                terrain_level(atlas, input.terrain.xy, local, upper), fract(lod));
        }
        if (input.terrain_fog.y > 0.0) {
            const float2 grey_origin = input.terrain.xy + float2(32.0, 0.0);
            float4 grey;
            if (footprint <= 1.0) {
                const float2 position = grey_origin + clamp(local, float2(0.5), float2(31.5));
                grey = atlas.sample(nearest,
                    position / float2(atlas.get_width(), atlas.get_height()));
            } else {
                grey = mix(terrain_level(atlas, grey_origin, local, lower),
                    terrain_level(atlas, grey_origin, local, upper), fract(lod));
            }
            colour = mix(colour, grey, input.terrain_fog.y);
        }
    } else if (input.terrain.z < 0.0) {
        constexpr sampler effect_sampler(coord::normalized, address::clamp_to_edge, filter::linear);
        colour = atlas.sample(effect_sampler, input.uv);
        // Atlas coverage is binary; filtering it gives premultiplied RGB. The pipeline
        // uses straight alpha, so recover the colour before applying vertex opacity.
        if (colour.a > 0.0) colour.rgb /= colour.a;
    } else {
        colour = atlas.sample(nearest, input.uv);
    }
    const float brightness = input.terrain.z == -2.0 ? 1.0 :
        (input.terrain.z > 0.0 ? input.terrain_fog.x : input.attenuation.x);
    colour.rgb = colour.rgb * input.modulation * brightness + input.emission;
    // Optical underwater billboards share their event depth across the complete sprite.
    // Physical models retain their actual per-fragment water intersection.
    const float depth = input.terrain.z == -5.0 ? input.terrain.x : input.water_scene.x - input.world.y;
    if (depth > 0.5 && input.terrain.z != -2.0 && input.terrain.z != -3.0 && input.water.z > 0.0 && detail > 0.0) {
        const float shallow = 1.0 - smoothstep(input.appearance3.z, input.appearance3.z + input.appearance3.w, depth);
        // Fine, restrained seabed patterns sit beneath the map's baked water art.
        // Submerged objects retain stronger highlights so their depth remains legible.
        const bool terrain = input.terrain.z > 0.0;
        const float frequency = terrain ? 3.0 : 1.0;
        const float wave = sin(input.world.x * 0.11 * frequency + time * 0.9 +
            sin(input.world.z * 0.07 * frequency - time * 0.5) * 1.6);
        const float crossing = sin(input.world.z * 0.1 * frequency - time * 0.7 +
            sin(input.world.x * 0.065 * frequency + time * 0.4));
        const float caustic = smoothstep(0.45, 0.96, max(wave, crossing));
        const float intensity = input.water.z * detail * smoothstep(0.5, input.appearance0.x, depth);
        if (input.terrain.z <= 0.0) {
            const float depth_fraction = clamp(depth / input.appearance0.y, 0.0, 1.0);
            const float tint = clamp((input.appearance0.z + input.appearance0.w * depth_fraction) *
                intensity, 0.0, input.appearance1.w);
            colour.rgb = mix(colour.rgb, colour.rgb * input.appearance1.xyz, tint);
            colour.rgb *= 1.0 - min(input.appearance2.z, (input.appearance2.x + input.appearance2.y * depth_fraction) * intensity);
        }
        const float modulation = terrain ? caustic * 0.06 - 0.01 : caustic * input.appearance3.x + input.appearance3.y;
        colour.rgb *= 1.0 + modulation * intensity * shallow;
    }
    // Actual interpolated surface intersection, confined to covered model fragments.
    // Geometry stays fixed so crossing triangles cannot displace their dry portion.
    if (depth > 0.0 && depth < input.appearance4.y && input.terrain.z <= 0.0 &&
        input.terrain.z >= -1.0 && input.water.z > 0.0 && detail > 0.0) {
        const float rim = depth < input.appearance4.x ? depth / input.appearance4.x :
            (input.appearance4.y - depth) / (input.appearance4.y - input.appearance4.x);
        colour.rgb = mix(colour.rgb, input.appearance5.xyz * brightness,
            clamp(rim * input.water.z * detail * input.appearance4.z, 0.0, 0.48));
    }
    if (input.terrain.z == -3.0) {
        const float bands = 0.82 + 0.18 * sin(input.world.z * 0.35 + time * 1.5);
        colour.a *= bands * smoothstep(0.0, 2.0, input.terrain.x);
    }
    colour.a *= input.attenuation.y;
    if (colour.a == 0.0) discard_fragment();
    return colour;
}
vertex float4 resolve_vertex(uint index [[vertex_id]]) {
    const float2 corners[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(corners[index], 0, 1);
}
fragment float4 resolve_fragment(float4 position [[position]],
    texture2d<float> source [[texture(0)]],
    constant uint4& parameters [[buffer(0)]]) {
    const uint factor = parameters.x;
    const uint samples = factor * factor;
    const uint2 origin = uint2(position.xy) * factor;
    uint4 sum = uint4(0);
    for (uint y = 0; y < factor; ++y)
        for (uint x = 0; x < factor; ++x)
            sum += uint4(round(source.read(origin + uint2(x, y)) * 255.0));
    return float4((sum + samples / 2) / samples) / 255.0;
}
)";

/// Release all resources, including an unsubmitted command buffer after an error.
struct Resources {
    SDL_GPUDevice* device{};
    SDL_GPUShader* vertex_shader{};
    SDL_GPUShader* fragment_shader{};
    SDL_GPUTexture* atlas{};
    SDL_GPUTexture* colour{};
    SDL_GPUTexture* depth{};
    SDL_GPUTexture* resolved{};
    SDL_GPUShader* resolve_vertex{};
    SDL_GPUShader* resolve_fragment{};
    SDL_GPUGraphicsPipeline* resolve_pipeline{};
    SDL_GPUSampler* sampler{};
    SDL_GPUBuffer* vertices{};
    SDL_GPUTransferBuffer* upload{};
    SDL_GPUTransferBuffer* download{};
    SDL_GPUGraphicsPipeline* pipeline{};
    SDL_GPUCommandBuffer* command{};
    SDL_GPUFence* fence{};

    /// Release GPU handles before destroying their device.
    ~Resources() {
        if (!device)
            return;
        if (command)
            SDL_CancelGPUCommandBuffer(command);
        if (fence)
            SDL_ReleaseGPUFence(device, fence);
        if (resolve_pipeline)
            SDL_ReleaseGPUGraphicsPipeline(device, resolve_pipeline);
        if (resolve_vertex)
            SDL_ReleaseGPUShader(device, resolve_vertex);
        if (resolve_fragment)
            SDL_ReleaseGPUShader(device, resolve_fragment);
        if (resolved)
            SDL_ReleaseGPUTexture(device, resolved);
        if (pipeline)
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        if (vertex_shader)
            SDL_ReleaseGPUShader(device, vertex_shader);
        if (fragment_shader)
            SDL_ReleaseGPUShader(device, fragment_shader);
        if (sampler)
            SDL_ReleaseGPUSampler(device, sampler);
        if (vertices)
            SDL_ReleaseGPUBuffer(device, vertices);
        if (upload)
            SDL_ReleaseGPUTransferBuffer(device, upload);
        if (download)
            SDL_ReleaseGPUTransferBuffer(device, download);
        if (atlas)
            SDL_ReleaseGPUTexture(device, atlas);
        if (colour)
            SDL_ReleaseGPUTexture(device, colour);
        if (depth)
            SDL_ReleaseGPUTexture(device, depth);
        SDL_DestroyGPUDevice(device);
    }
};

/// Create a single-level two-dimensional texture.
SDL_GPUTexture* create_texture(
    SDL_GPUDevice* device,
    uint32_t width,
    uint32_t height,
    SDL_GPUTextureFormat format,
    SDL_GPUTextureUsageFlags usage
) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    return SDL_CreateGPUTexture(device, &info);
}

/// Compile a stage from the shared Metal shader source.
SDL_GPUShader* create_shader(SDL_GPUDevice* device, bool vertex, bool resolve = false) {
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(shader_source);
    info.code_size = sizeof(shader_source);
    info.entrypoint = resolve ? (vertex ? "resolve_vertex" : "resolve_fragment")
                              : (vertex ? "vertex_main" : "fragment_main");
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_uniform_buffers = resolve ? (vertex ? 0 : 1) : (vertex ? 1 : 0);
    info.num_samplers = vertex ? 0 : 1;
    return SDL_CreateGPUShader(device, &info);
}
#endif

} // namespace

struct Context::State {
#if defined(__APPLE__)
    Resources resources;
    uint32_t texture_width{};
    uint32_t texture_height{};
    uint32_t output_width{};
    uint32_t output_height{};
    uint32_t resolved_width{};
    uint32_t resolved_height{};
    uint32_t vertex_capacity{};
    uint32_t upload_capacity{};
    uint64_t atlas_revision{};
    bool atlas_uploaded{};
#endif
};

Context::Context() : state(std::make_unique<State>()) {
}

Context::~Context() = default;

Result render(
    Context& context,
    std::span<const Vertex> vertices,
    std::span<const uint8_t> texture_rgba,
    uint32_t texture_width,
    uint32_t texture_height,
    uint64_t atlas_revision,
    uint32_t output_width,
    uint32_t output_height,
    const std::array<float, 12>& projection,
    std::vector<uint8_t>& output_rgba,
    std::string& error,
    uint32_t requested_supersample,
    const AtlasUpdate* atlas_update,
    const WaterEffects* water
) {
    output_rgba.clear();
    error.clear();
    if (!valid_dimensions(texture_width, texture_height) ||
        !valid_dimensions(output_width, output_height) || vertices.empty() ||
        vertices.size() > max_vertices || vertices.size() % vertices_per_triangle != 0 ||
        texture_rgba.size() != size_t(texture_width) * texture_height * channels ||
        requested_supersample == 0 || requested_supersample > max_supersample ||
        (requested_supersample & (requested_supersample - 1)) != 0) {
        error = "Invalid live GPU dimensions, triangle count, texture bytes or supersampling";
        return Result::failed;
    }
    if (atlas_update && (atlas_update->first_row > atlas_update->end_row ||
                         atlas_update->end_row > texture_height)) {
        error = "Invalid live GPU atlas update rows";
        return Result::failed;
    }
    for (float value : projection) {
        if (!std::isfinite(value) || std::abs(value) > projection_limit) {
            error = "Invalid live GPU projection";
            return Result::failed;
        }
    }
    if (water && (!std::isfinite(water->ripples) || !std::isfinite(water->coastal_waves) ||
                  !std::isfinite(water->shimmer) || !std::isfinite(water->reflections) ||
                  !std::isfinite(water->sea_level) || !std::isfinite(water->seconds) ||
                  !std::isfinite(water->fine_detail) || !std::isfinite(water->detail) ||
                  !std::isfinite(water->wind.x) || !std::isfinite(water->wind.z) ||
                  !std::isfinite(water->wind.strength) || !std::isfinite(water->wind.drift_x) ||
                  !std::isfinite(water->wind.drift_z))) {
        error = "Invalid live GPU water parameters";
        return Result::failed;
    }
    // Ordered bounds also reject NaN and infinities. Accumulate without short-circuiting
    // so independent vertex checks can run in parallel.
    bool vertices_valid = true;
    for (const auto& vertex : vertices) {
        vertices_valid &=
            uint32_t(std::abs(vertex.x) <= coordinate_limit) &
            (std::abs(vertex.height) <= coordinate_limit) &
            (std::abs(vertex.z) <= coordinate_limit) & (vertex.u >= 0) & (vertex.u <= 1) &
            (vertex.v >= 0) & (vertex.v <= 1) & (vertex.brightness >= 0) &
            (vertex.brightness <= 1) & (vertex.opacity >= 0) & (vertex.opacity <= 1) &
            (vertex.red >= 0) & (vertex.red <= 1) & (vertex.green >= 0) & (vertex.green <= 1) &
            (vertex.blue >= 0) & (vertex.blue <= 1) & (vertex.emission_r >= 0) &
            (vertex.emission_r <= 1) & (vertex.emission_g >= 0) & (vertex.emission_g <= 1) &
            (vertex.emission_b >= 0) & (vertex.emission_b <= 1) & (vertex.terrain_x >= 0) &
            (vertex.terrain_x <= (vertex.terrain_size <= -2 ? coordinate_limit : texture_width)) &
            (vertex.terrain_y >= 0) & (vertex.terrain_y <= texture_height) &
            ((vertex.terrain_size == -5) | (vertex.terrain_size == -4) |
             (vertex.terrain_size == -3) | (vertex.terrain_size == -2) |
             (vertex.terrain_size == -1) | (vertex.terrain_size == 0) |
             (vertex.terrain_size == 32)) &
            (vertex.terrain_grey >= 0) & (vertex.terrain_grey <= 1) & (vertex.terrain_known >= 0) &
            (vertex.terrain_known <= 1);
        if (vertex.terrain_size > 0)
            vertices_valid &= (vertex.terrain_x + 64 <= texture_width) &
                              (vertex.terrain_y + 64 <= texture_height);
    }
    if (!vertices_valid) {
        error = "Invalid live GPU vertex coordinates";
        return Result::failed;
    }
#if !defined(__APPLE__)
    (void)context;
    (void)atlas_revision;
    (void)water;
    error = "Live GPU shaders currently require Metal";
    return Result::unavailable;
#else
    uint32_t supersample = requested_supersample;
    while (output_width > max_dimension / supersample ||
           output_height > max_dimension / supersample)
        supersample /= 2;
    const uint32_t render_width = output_width * supersample;
    const uint32_t render_height = output_height * supersample;
    auto& state = *context.state;
    auto& resources = state.resources;
    if (!resources.device) {
        resources.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, "metal");
        if (!resources.device) {
            error = std::string("Metal GPU unavailable: ") + SDL_GetError();
            return Result::unavailable;
        }
    }
    /// Drop incomplete resources so another frame can retry safely.
    const auto fail = [&](const char* operation) {
        error = std::string(operation) + ": " + SDL_GetError();
        context.state = std::make_unique<Context::State>();
        return Result::failed;
    };
    if (!resources.pipeline) {
        resources.vertex_shader = create_shader(resources.device, true);
        resources.fragment_shader = create_shader(resources.device, false);
        if (!resources.vertex_shader || !resources.fragment_shader)
            return fail("Compile live GPU shaders");
        SDL_GPUSamplerCreateInfo sampler_info{};
        sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        resources.sampler = SDL_CreateGPUSampler(resources.device, &sampler_info);
        if (!resources.sampler)
            return fail("Create live GPU textures and sampler");

        SDL_GPUVertexBufferDescription vertex_description{};
        vertex_description.pitch = sizeof(Vertex);
        SDL_GPUVertexAttribute attributes[7]{};
        attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[1].location = 1;
        attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attributes[1].offset = offsetof(Vertex, u);
        attributes[2].location = 2;
        attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attributes[2].offset = offsetof(Vertex, brightness);
        attributes[3].location = 3;
        attributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[3].offset = offsetof(Vertex, emission_r);
        attributes[4].location = 4;
        attributes[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[4].offset = offsetof(Vertex, terrain_x);
        attributes[5].location = 5;
        attributes[5].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attributes[5].offset = offsetof(Vertex, terrain_known);
        attributes[6].location = 6;
        attributes[6].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[6].offset = offsetof(Vertex, red);
        SDL_GPUColorTargetDescription colour_description{};
        colour_description.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        auto& blend = colour_description.blend_state;
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.vertex_shader = resources.vertex_shader;
        pipeline_info.fragment_shader = resources.fragment_shader;
        pipeline_info.vertex_input_state = {&vertex_description, 1, attributes, 7};
        pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline_info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        pipeline_info.depth_stencil_state.enable_depth_test = true;
        pipeline_info.depth_stencil_state.enable_depth_write = true;
        pipeline_info.target_info.color_target_descriptions = &colour_description;
        pipeline_info.target_info.num_color_targets = 1;
        pipeline_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        pipeline_info.target_info.has_depth_stencil_target = true;
        resources.pipeline = SDL_CreateGPUGraphicsPipeline(resources.device, &pipeline_info);
        if (!resources.pipeline)
            return fail("Create live GPU pipeline");
    }
    const auto vertex_bytes = uint32_t(vertices.size_bytes());
    if (supersample > 1 && !resources.resolve_pipeline) {
        resources.resolve_vertex = create_shader(resources.device, true, true);
        resources.resolve_fragment = create_shader(resources.device, false, true);
        if (!resources.resolve_vertex || !resources.resolve_fragment)
            return fail("Compile live GPU resolve shaders");
        SDL_GPUColorTargetDescription target{};
        target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = resources.resolve_vertex;
        info.fragment_shader = resources.resolve_fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        resources.resolve_pipeline = SDL_CreateGPUGraphicsPipeline(resources.device, &info);
        if (!resources.resolve_pipeline)
            return fail("Create live GPU resolve pipeline");
    }
    const uint32_t output_bytes = output_width * output_height * channels;
    if (texture_width != state.texture_width || texture_height != state.texture_height) {
        if (resources.atlas)
            SDL_ReleaseGPUTexture(resources.device, resources.atlas);
        resources.atlas = create_texture(
            resources.device,
            texture_width,
            texture_height,
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_SAMPLER
        );
        state.texture_width = texture_width;
        state.texture_height = texture_height;
        state.atlas_uploaded = false;
    }
    if (render_width != state.output_width || render_height != state.output_height) {
        if (resources.colour)
            SDL_ReleaseGPUTexture(resources.device, resources.colour);
        if (resources.depth)
            SDL_ReleaseGPUTexture(resources.device, resources.depth);
        resources.colour = create_texture(
            resources.device,
            render_width,
            render_height,
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
        );
        resources.depth = create_texture(
            resources.device,
            render_width,
            render_height,
            SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
        );
        state.output_width = render_width;
        state.output_height = render_height;
    }
    if (output_width != state.resolved_width || output_height != state.resolved_height) {
        if (resources.resolved)
            SDL_ReleaseGPUTexture(resources.device, resources.resolved);
        if (resources.download)
            SDL_ReleaseGPUTransferBuffer(resources.device, resources.download);
        resources.resolved = create_texture(
            resources.device,
            output_width,
            output_height,
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
        );
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = output_bytes;
        resources.download = SDL_CreateGPUTransferBuffer(resources.device, &info);
        state.resolved_width = output_width;
        state.resolved_height = output_height;
    }
    if (vertex_bytes > state.vertex_capacity) {
        if (resources.vertices)
            SDL_ReleaseGPUBuffer(resources.device, resources.vertices);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = vertex_bytes;
        resources.vertices = SDL_CreateGPUBuffer(resources.device, &info);
        state.vertex_capacity = vertex_bytes;
    }
    const bool atlas_changed = !state.atlas_uploaded || state.atlas_revision != atlas_revision;
    const bool partial_atlas = atlas_update && state.atlas_uploaded &&
                               state.atlas_revision == atlas_update->previous_revision;
    const uint32_t first_row = partial_atlas ? atlas_update->first_row : 0;
    const uint32_t end_row = partial_atlas ? atlas_update->end_row : texture_height;
    const uint32_t upload_rows = atlas_changed ? end_row - first_row : 0;
    const uint32_t atlas_upload_bytes = texture_width * upload_rows * channels;
    const uint32_t upload_bytes = vertex_bytes + atlas_upload_bytes;
    if (upload_bytes > state.upload_capacity) {
        if (resources.upload)
            SDL_ReleaseGPUTransferBuffer(resources.device, resources.upload);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = upload_bytes;
        resources.upload = SDL_CreateGPUTransferBuffer(resources.device, &info);
        state.upload_capacity = upload_bytes;
    }
    if (!resources.vertices || !resources.upload || !resources.download || !resources.atlas ||
        !resources.colour || !resources.depth || !resources.resolved)
        return fail("Create live GPU frame resources");
    auto* upload =
        static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(resources.device, resources.upload, false));
    if (!upload)
        return fail("Map live GPU upload");
    std::memcpy(upload, vertices.data(), vertex_bytes);
    if (atlas_upload_bytes != 0)
        std::memcpy(
            upload + vertex_bytes,
            texture_rgba.data() + size_t(first_row) * texture_width * channels,
            atlas_upload_bytes
        );
    SDL_UnmapGPUTransferBuffer(resources.device, resources.upload);
    resources.command = SDL_AcquireGPUCommandBuffer(resources.device);
    if (!resources.command)
        return fail("Acquire live GPU command buffer");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(resources.command);
    if (!copy)
        return fail("Begin live GPU upload");
    const SDL_GPUTransferBufferLocation vertex_source{resources.upload, 0};
    const SDL_GPUBufferRegion vertex_destination{resources.vertices, 0, vertex_bytes};
    SDL_UploadToGPUBuffer(copy, &vertex_source, &vertex_destination, false);
    const SDL_GPUTextureTransferInfo texture_source{
        resources.upload, vertex_bytes, texture_width, upload_rows
    };
    const SDL_GPUTextureRegion atlas_region{
        resources.atlas, 0, 0, 0, first_row, 0, texture_width, upload_rows, 1
    };
    if (atlas_upload_bytes != 0)
        SDL_UploadToGPUTexture(copy, &texture_source, &atlas_region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUColorTargetInfo colour_target{};
    colour_target.texture = resources.colour;
    colour_target.clear_color.a = 1;
    colour_target.load_op = SDL_GPU_LOADOP_CLEAR;
    colour_target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth_target{};
    depth_target.texture = resources.depth;
    depth_target.clear_depth = 1;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    std::array<float, 56> camera{};
    namespace appearance = renderer_water_appearance;
    const std::array coefficients{
        appearance::immersion_distance,
        appearance::tint_distance,
        appearance::tint_base,
        appearance::tint_depth,
        appearance::tint_red,
        appearance::tint_green,
        appearance::tint_blue,
        appearance::tint_max,
        appearance::absorption_base,
        appearance::absorption_depth,
        appearance::absorption_max,
        0.0F,
        appearance::caustic_gain,
        appearance::caustic_bias,
        appearance::caustic_fade_start,
        appearance::caustic_fade_range,
        appearance::rim_peak_depth,
        appearance::rim_width,
        appearance::rim_strength,
        0.0F,
        appearance::rim_red,
        appearance::rim_green,
        appearance::rim_blue,
        0.0F
    };
    std::copy(coefficients.begin(), coefficients.end(), camera.begin() + 32);
    std::copy(projection.begin(), projection.end(), camera.begin());
    camera[12] = float(output_width);
    camera[13] = float(output_height);
    camera[14] = float(supersample);
    if (water) {
        camera[16] = std::clamp(water->ripples, 0.0F, 2.0F);
        camera[17] = std::clamp(water->coastal_waves, 0.0F, 2.0F);
        camera[18] = std::clamp(water->shimmer, 0.0F, 2.0F);
        camera[19] = std::clamp(water->reflections, 0.0F, 2.0F);
        camera[20] = water->sea_level;
        camera[21] = water->seconds;
        camera[22] = std::clamp(water->detail, 0.0F, 1.0F);
        camera[23] = std::clamp(water->fine_detail, 0.0F, 1.0F);
        camera[24] = water->wind.x;
        camera[25] = water->wind.z;
        camera[26] = std::clamp(water->wind.strength, 0.0F, 1.0F);
        camera[28] = water->wind.drift_x;
        camera[29] = water->wind.drift_z;
    }
    SDL_PushGPUVertexUniformData(resources.command, 0, camera.data(), sizeof(camera));
    SDL_GPURenderPass* pass =
        SDL_BeginGPURenderPass(resources.command, &colour_target, 1, &depth_target);
    if (!pass)
        return fail("Begin live GPU render pass");
    SDL_BindGPUGraphicsPipeline(pass, resources.pipeline);
    const SDL_GPUBufferBinding binding{resources.vertices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    const SDL_GPUTextureSamplerBinding sampler_binding{resources.atlas, resources.sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sampler_binding, 1);
    SDL_DrawGPUPrimitives(pass, uint32_t(vertices.size()), 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    if (supersample > 1) {
        SDL_GPUColorTargetInfo target{};
        target.texture = resources.resolved;
        target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        target.store_op = SDL_GPU_STOREOP_STORE;
        const std::array<uint32_t, 4> parameters{supersample, 0, 0, 0};
        SDL_PushGPUFragmentUniformData(resources.command, 0, parameters.data(), sizeof(parameters));
        pass = SDL_BeginGPURenderPass(resources.command, &target, 1, nullptr);
        if (!pass)
            return fail("Begin live GPU resolve");
        SDL_BindGPUGraphicsPipeline(pass, resources.resolve_pipeline);
        const SDL_GPUTextureSamplerBinding source{resources.colour, resources.sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
        SDL_DrawGPUPrimitives(pass, vertices_per_triangle, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    copy = SDL_BeginGPUCopyPass(resources.command);
    if (!copy)
        return fail("Begin live GPU download");
    const SDL_GPUTextureRegion colour_region{
        supersample > 1 ? resources.resolved : resources.colour,
        0,
        0,
        0,
        0,
        0,
        output_width,
        output_height,
        1
    };
    const SDL_GPUTextureTransferInfo download_destination{
        resources.download, 0, output_width, output_height
    };
    SDL_DownloadFromGPUTexture(copy, &colour_region, &download_destination);
    SDL_EndGPUCopyPass(copy);
    resources.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(resources.command);
    resources.command = nullptr;
    if (!resources.fence || !SDL_WaitForGPUFences(resources.device, true, &resources.fence, 1))
        return fail("Complete live GPU render");
    const auto* download = static_cast<const uint8_t*>(
        SDL_MapGPUTransferBuffer(resources.device, resources.download, false)
    );
    if (!download)
        return fail("Map live GPU download");
    output_rgba.assign(download, download + output_bytes);
    SDL_UnmapGPUTransferBuffer(resources.device, resources.download);
    SDL_ReleaseGPUFence(resources.device, resources.fence);
    resources.fence = nullptr;
    context.actual_supersample = supersample;
    state.atlas_revision = atlas_revision;
    state.atlas_uploaded = true;
    return Result::success;
#endif
}

} // namespace oa::app::renderer_live_gpu
