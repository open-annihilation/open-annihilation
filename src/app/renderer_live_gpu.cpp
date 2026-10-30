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
constexpr uint32_t max_dimension = largest_edge;
constexpr size_t max_vertices = largest_vertex_count;
constexpr float coordinate_limit = 32768.0f;
constexpr float projection_limit = 131072.0f;
constexpr uint32_t vertices_per_triangle = 3;
constexpr uint32_t max_supersample = 16;

/// Check dimensions before calculating the byte size of a bounded RGBA image.
bool valid_dimensions(uint32_t width, uint32_t height) {
    return width != 0 && height != 0 && width <= max_dimension && height <= max_dimension;
}

// Every stage, as SPIR-V and as Metal shading language, made from
// src/app/shaders by tools/make_live_shaders.py.
#include "renderer_live_shaders.inc"

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
    SDL_GPUSampler* sampler{};        ///< the atlas's nearest texel
    SDL_GPUSampler* linear_sampler{}; ///< the atlas filtered
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
        if (linear_sampler)
            SDL_ReleaseGPUSampler(device, linear_sampler);
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

/// The shader formats the renderer carries every stage in.
constexpr SDL_GPUShaderFormat shader_formats =
    SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL;

/// Creates a stage in the form the device takes: Metal shading language
/// where the device runs on Metal, SPIR-V where it runs on Vulkan.
SDL_GPUShader* create_shader(SDL_GPUDevice* device, bool vertex, bool resolve = false) {
    SDL_GPUShaderCreateInfo info{};
    const auto stage = [&](const auto& spirv, const auto& msl) {
        if ((SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_MSL) != 0) {
            info.code = reinterpret_cast<const Uint8*>(msl);
            info.code_size = sizeof(msl) - 1;
            info.entrypoint = "main0";
            info.format = SDL_GPU_SHADERFORMAT_MSL;
        } else {
            info.code = spirv;
            info.code_size = sizeof(spirv);
            info.entrypoint = "main";
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        }
    };
    if (resolve && vertex)
        stage(live_resolve_vertex_spirv, live_resolve_vertex_msl);
    else if (resolve)
        stage(live_resolve_fragment_spirv, live_resolve_fragment_msl);
    else if (vertex)
        stage(live_scene_vertex_spirv, live_scene_vertex_msl);
    else
        stage(live_scene_fragment_spirv, live_scene_fragment_msl);
    info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    // The scene's stages each read the camera's block; the resolve's
    // fragment stage reads its factor.
    info.num_uniform_buffers = resolve && vertex ? 0 : 1;
    info.num_samplers = vertex ? 0 : (resolve ? 1 : 2);
    return SDL_CreateGPUShader(device, &info);
}

} // namespace

struct Context::State {
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
    // Ordered bounds also reject NaN and infinities.
    const auto unit_range = [](float value) { return value >= 0 && value <= 1; };
    bool vertices_valid = true;
    for (const auto& vertex : vertices) {
        const bool known_terrain_size =
            vertex.terrain_size == -7 || vertex.terrain_size == -6 || vertex.terrain_size == -5 ||
            vertex.terrain_size == -4 || vertex.terrain_size == -3 || vertex.terrain_size == -2 ||
            vertex.terrain_size == -1 || vertex.terrain_size == 0 || vertex.terrain_size == 32 ||
            vertex.terrain_size == 16 || vertex.terrain_size == 8 || vertex.terrain_size == 4;
        const bool valid =
            std::abs(vertex.x) <= coordinate_limit && std::abs(vertex.height) <= coordinate_limit &&
            std::abs(vertex.z) <= coordinate_limit && unit_range(vertex.u) &&
            unit_range(vertex.v) && unit_range(vertex.brightness) && unit_range(vertex.opacity) &&
            unit_range(vertex.red) && unit_range(vertex.green) && unit_range(vertex.blue) &&
            unit_range(vertex.emission_r) && unit_range(vertex.emission_g) &&
            unit_range(vertex.emission_b) && vertex.terrain_x >= 0 &&
            vertex.terrain_x <= (vertex.terrain_size <= -2 ? coordinate_limit : texture_width) &&
            vertex.terrain_y >= 0 && vertex.terrain_y <= texture_height && known_terrain_size &&
            unit_range(vertex.terrain_grey) && unit_range(vertex.terrain_known) &&
            (vertex.terrain_size <= 0 ||
             (vertex.terrain_x + 2 * vertex.terrain_size <= texture_width &&
              vertex.terrain_y + 2 * vertex.terrain_size <= texture_height));
        vertices_valid = vertices_valid && valid;
    }
    if (!vertices_valid) {
        error = "Invalid live GPU vertex coordinates";
        return Result::failed;
    }
    uint32_t supersample = requested_supersample;
    while (output_width > max_dimension / supersample ||
           output_height > max_dimension / supersample)
        supersample /= 2;
    const uint32_t render_width = output_width * supersample;
    const uint32_t render_height = output_height * supersample;
    auto& state = *context.state;
    auto& resources = state.resources;
    if (context.device_unavailable) {
        error = "Live GPU unavailable on this system";
        return Result::unavailable;
    }
    if (!resources.device) {
        // SDL picks the graphics interface: Metal on Apple systems, Vulkan
        // elsewhere, or SDL_GPU_DRIVER's.
        resources.device = SDL_CreateGPUDevice(shader_formats, false, nullptr);
        if (!resources.device) {
            error = std::string("Live GPU unavailable: ") + SDL_GetError();
            context.device_unavailable = true;
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
        sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
        sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
        resources.linear_sampler = SDL_CreateGPUSampler(resources.device, &sampler_info);
        if (!resources.sampler || !resources.linear_sampler)
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
    SDL_PushGPUFragmentUniformData(resources.command, 0, camera.data(), sizeof(camera));
    SDL_GPURenderPass* pass =
        SDL_BeginGPURenderPass(resources.command, &colour_target, 1, &depth_target);
    if (!pass)
        return fail("Begin live GPU render pass");
    SDL_BindGPUGraphicsPipeline(pass, resources.pipeline);
    const SDL_GPUBufferBinding binding{resources.vertices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    const std::array<SDL_GPUTextureSamplerBinding, 2> sampler_bindings{
        {{resources.atlas, resources.sampler}, {resources.atlas, resources.linear_sampler}}
    };
    SDL_BindGPUFragmentSamplers(
        pass, 0, sampler_bindings.data(), static_cast<Uint32>(sampler_bindings.size())
    );
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
}

} // namespace oa::app::renderer_live_gpu
