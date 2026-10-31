// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_live_gpu.hpp"
#include "bench_timers.hpp"
#include "renderer_live_lights.hpp"
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
/// The samples a pixel the card's multisampling takes: a 2x2 of
/// supersampling's.
constexpr uint32_t multisample_count = 4;

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
    /// The multisampled colour as the card averages it, at the drawn size,
    /// for the supersampling resolve to reduce: only with both.
    SDL_GPUTexture* multisample_resolved{};
    SDL_GPUTexture* resolved{};
    SDL_GPUShader* resolve_vertex{};
    SDL_GPUShader* resolve_fragment{};
    SDL_GPUGraphicsPipeline* resolve_pipeline{};
    SDL_GPUSampler* sampler{};        ///< the atlas's nearest texel
    SDL_GPUSampler* linear_sampler{}; ///< the atlas filtered
    SDL_GPUBuffer* vertices{};        ///< the frame's vertices after the kept ground
    SDL_GPUBuffer* ground{};          ///< the kept ground's vertices
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
        if (multisample_resolved)
            SDL_ReleaseGPUTexture(device, multisample_resolved);
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
        if (ground)
            SDL_ReleaseGPUBuffer(device, ground);
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

/// Create a single-level two-dimensional texture, of one sample a pixel
/// unless the multisampling count says more.
SDL_GPUTexture* create_texture(
    SDL_GPUDevice* device,
    uint32_t width,
    uint32_t height,
    SDL_GPUTextureFormat format,
    SDL_GPUTextureUsageFlags usage,
    SDL_GPUSampleCount sample_count = SDL_GPU_SAMPLECOUNT_1
) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = sample_count;
    return SDL_CreateGPUTexture(device, &info);
}

/// The shader formats the renderer carries every stage in.
constexpr SDL_GPUShaderFormat shader_formats =
    SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL;

/// Fills in a stage's code in the form the device takes: Metal shading
/// language where the device runs on Metal, SPIR-V where it runs on Vulkan.
template <std::size_t spirv_size, std::size_t msl_size>
void stage_code(
    SDL_GPUDevice* device,
    SDL_GPUShaderCreateInfo& info,
    const unsigned char (&spirv)[spirv_size],
    const char (&msl)[msl_size]
) {
    if ((SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_MSL) != 0) {
        info.code = reinterpret_cast<const Uint8*>(msl);
        info.code_size = msl_size - 1;
        info.entrypoint = "main0";
        info.format = SDL_GPU_SHADERFORMAT_MSL;
    } else {
        info.code = spirv;
        info.code_size = spirv_size;
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    }
}

/// Creates a stage in the form the device takes.
SDL_GPUShader* create_shader(SDL_GPUDevice* device, bool vertex, bool resolve = false) {
    SDL_GPUShaderCreateInfo info{};
    const auto stage = [&](const auto& spirv, const auto& msl) {
        stage_code(device, info, spirv, msl);
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

/// The card's part of the lights (renderer_live_lights.hpp): the surface
/// under each pixel's centre, which a pass of its own draws from the
/// scene's vertices, the lights' table and tiles, and the pass that adds the
/// lights to the picture.
struct LightResources {
    SDL_GPUDevice* device{}; ///< the device they are made on
    /// The scene's vertex stage and the surface's fragment stage, drawing one
    /// sample a pixel; null where the card cannot draw the surface.
    SDL_GPUGraphicsPipeline* surface_pipeline{};
    SDL_GPUShader* surface_shader{};
    SDL_GPUShader* vertex_shader{}; ///< the resolve's triangle
    SDL_GPUShader* fragment_shader{};
    SDL_GPUGraphicsPipeline* pipeline{};
    /// The surface, its depth and the picture with the lights, at the
    /// picture's size.
    SDL_GPUTexture* surface{};
    SDL_GPUTexture* surface_depth{};
    SDL_GPUTexture* lit{};
    SDL_GPUTexture* table{}; ///< a row of texels_per_light texels a light
    SDL_GPUTexture* tiles{}; ///< a texel of words_per_tile words a tile
    SDL_GPUTransferBuffer* upload{};
    uint32_t upload_capacity{};
    uint32_t width{};
    uint32_t height{};
    uint32_t tiles_across{};
    uint32_t tiles_down{};
    /// Whether the card has the formats the lights take, asked once.
    bool formats_known{};
    bool formats_supported{};
    /// The frame's lights, kept so that once grown a frame allocates nothing.
    renderer_live_lights::Table packed{};

    LightResources() = default;
    LightResources(const LightResources&) = delete;
    LightResources& operator=(const LightResources&) = delete;

    /// Release the handles before the device they were made on.
    ~LightResources() {
        if (!device)
            return;
        for (SDL_GPUGraphicsPipeline* made : {surface_pipeline, pipeline})
            if (made)
                SDL_ReleaseGPUGraphicsPipeline(device, made);
        for (SDL_GPUShader* made : {surface_shader, vertex_shader, fragment_shader})
            if (made)
                SDL_ReleaseGPUShader(device, made);
        for (SDL_GPUTexture* made : {surface, surface_depth, lit, table, tiles})
            if (made)
                SDL_ReleaseGPUTexture(device, made);
        if (upload)
            SDL_ReleaseGPUTransferBuffer(device, upload);
    }
};

/// The surface's format: its camera depth and visibility.
constexpr SDL_GPUTextureFormat light_surface_format = SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;

/// Whether the card has the formats the lights take: the surface's as a
/// target it reads back, and the table's floats and the tiles' words to read.
bool light_formats(LightResources& lights, SDL_GPUDevice* device) {
    if (!lights.formats_known) {
        lights.formats_supported =
            SDL_GPUTextureSupportsFormat(
                device,
                light_surface_format,
                SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
            ) &&
            SDL_GPUTextureSupportsFormat(
                device,
                SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
                SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREUSAGE_SAMPLER
            ) &&
            SDL_GPUTextureSupportsFormat(
                device,
                SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT,
                SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREUSAGE_SAMPLER
            );
        lights.formats_known = true;
    }
    return lights.formats_supported;
}

/// Makes the surface's pipeline from the scene's, in place of any made
/// before: the scene's vertices and vertex stage, one sample a pixel, the
/// nearest face drawn kept; none where the card cannot draw the surface.
void make_light_surface_pipeline(
    LightResources& lights, SDL_GPUDevice* device, const SDL_GPUGraphicsPipelineCreateInfo& scene
) {
    lights.device = device;
    if (lights.surface_pipeline)
        SDL_ReleaseGPUGraphicsPipeline(device, lights.surface_pipeline);
    lights.surface_pipeline = nullptr;
    if (!light_formats(lights, device))
        return;
    if (!lights.surface_shader) {
        SDL_GPUShaderCreateInfo info{};
        stage_code(device, info, live_surface_fragment_spirv, live_surface_fragment_msl);
        info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = 1;
        info.num_uniform_buffers = 1;
        lights.surface_shader = SDL_CreateGPUShader(device, &info);
        if (!lights.surface_shader)
            return;
    }
    SDL_GPUColorTargetDescription target{};
    target.format = light_surface_format;
    auto info = scene;
    info.fragment_shader = lights.surface_shader;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    // A face drawn later at the same depth leaves the first, as the
    // processor's raster does.
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    info.multisample_state = SDL_GPUMultisampleState{};
    lights.surface_pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
}

/// Makes what the frame's lights need for a picture of width by height, and
/// copies the packed lights into the upload.
///
/// @return false where any of it cannot be made
bool prepare_lights(LightResources& lights, uint32_t width, uint32_t height) {
    SDL_GPUDevice* const device = lights.device;
    if (!lights.pipeline) {
        lights.vertex_shader = create_shader(device, true, true);
        SDL_GPUShaderCreateInfo info{};
        stage_code(device, info, live_lights_fragment_spirv, live_lights_fragment_msl);
        info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = 4;
        info.num_uniform_buffers = 1;
        lights.fragment_shader = SDL_CreateGPUShader(device, &info);
        if (!lights.vertex_shader || !lights.fragment_shader)
            return false;
        SDL_GPUColorTargetDescription target{};
        target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.vertex_shader = lights.vertex_shader;
        pipeline_info.fragment_shader = lights.fragment_shader;
        pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline_info.target_info.color_target_descriptions = &target;
        pipeline_info.target_info.num_color_targets = 1;
        lights.pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_info);
        if (!lights.pipeline)
            return false;
    }
    if (!lights.table) {
        lights.table = create_texture(
            device,
            renderer_live_lights::texels_per_light,
            uint32_t(renderer_effects::max_sources),
            SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
            SDL_GPU_TEXTUREUSAGE_SAMPLER
        );
        if (!lights.table)
            return false;
    }
    if (width != lights.width || height != lights.height) {
        for (SDL_GPUTexture** made : {&lights.surface, &lights.surface_depth, &lights.lit}) {
            if (*made)
                SDL_ReleaseGPUTexture(device, *made);
            *made = nullptr;
        }
        lights.width = lights.height = 0;
        lights.surface = create_texture(
            device,
            width,
            height,
            light_surface_format,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
        );
        lights.surface_depth = create_texture(
            device,
            width,
            height,
            SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
        );
        // Read as well as drawn, so that what is drawn over it next can be
        // drawn on the card.
        lights.lit = create_texture(
            device,
            width,
            height,
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
        );
        if (!lights.surface || !lights.surface_depth || !lights.lit)
            return false;
        lights.width = width;
        lights.height = height;
    }
    const auto& packed = lights.packed;
    if (packed.tiles_across != lights.tiles_across || packed.tiles_down != lights.tiles_down) {
        if (lights.tiles)
            SDL_ReleaseGPUTexture(device, lights.tiles);
        lights.tiles_across = lights.tiles_down = 0;
        lights.tiles = create_texture(
            device,
            packed.tiles_across,
            packed.tiles_down,
            SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT,
            SDL_GPU_TEXTUREUSAGE_SAMPLER
        );
        if (!lights.tiles)
            return false;
        lights.tiles_across = packed.tiles_across;
        lights.tiles_down = packed.tiles_down;
    }
    const auto table_bytes = uint32_t(packed.lights.size() * sizeof(float));
    const auto tile_bytes = uint32_t(packed.tiles.size() * sizeof(uint32_t));
    if (table_bytes + tile_bytes > lights.upload_capacity) {
        if (lights.upload)
            SDL_ReleaseGPUTransferBuffer(device, lights.upload);
        lights.upload_capacity = 0;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = table_bytes + tile_bytes;
        lights.upload = SDL_CreateGPUTransferBuffer(device, &info);
        if (!lights.upload)
            return false;
        lights.upload_capacity = info.size;
    }
    auto* upload = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, lights.upload, true));
    if (!upload)
        return false;
    std::memcpy(upload, packed.lights.data(), table_bytes);
    std::memcpy(upload + table_bytes, packed.tiles.data(), tile_bytes);
    SDL_UnmapGPUTransferBuffer(device, lights.upload);
    return true;
}

/// Uploads the frame's lights, which prepare_lights() copied, in the frame's
/// copy pass.
void upload_lights(const LightResources& lights, SDL_GPUCopyPass* copy) {
    const auto& packed = lights.packed;
    const SDL_GPUTextureTransferInfo table_source{
        lights.upload, 0, renderer_live_lights::texels_per_light, packed.count
    };
    const SDL_GPUTextureRegion table_region{
        lights.table, 0, 0, 0, 0, 0, renderer_live_lights::texels_per_light, packed.count, 1
    };
    SDL_UploadToGPUTexture(copy, &table_source, &table_region, true);
    const SDL_GPUTextureTransferInfo tile_source{
        lights.upload,
        uint32_t(packed.lights.size() * sizeof(float)),
        packed.tiles_across,
        packed.tiles_down
    };
    const SDL_GPUTextureRegion tile_region{
        lights.tiles, 0, 0, 0, 0, 0, packed.tiles_across, packed.tiles_down, 1
    };
    SDL_UploadToGPUTexture(copy, &tile_source, &tile_region, true);
}

/// Draws the surface the lights reach: the frame's triangles again, the
/// kept ground and then the rest, one sample a pixel whatever the scene's
/// samples, so that each pixel holds the face under its centre.
///
/// @param camera the scene's uniform block
/// @return false where the pass cannot begin
bool draw_light_surface(
    const LightResources& lights,
    SDL_GPUCommandBuffer* command,
    std::span<const float> camera,
    SDL_GPUTexture* atlas,
    SDL_GPUSampler* sampler,
    SDL_GPUBuffer* ground,
    std::size_t ground_count,
    SDL_GPUBuffer* frame_vertices,
    std::size_t frame_count
) {
    SDL_GPUColorTargetInfo target{};
    target.texture = lights.surface;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = lights.surface_depth;
    depth.clear_depth = 1;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    const auto bytes = static_cast<Uint32>(camera.size_bytes());
    SDL_PushGPUVertexUniformData(command, 0, camera.data(), bytes);
    SDL_PushGPUFragmentUniformData(command, 0, camera.data(), bytes);
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, &depth);
    if (!pass)
        return false;
    SDL_BindGPUGraphicsPipeline(pass, lights.surface_pipeline);
    const SDL_GPUTextureSamplerBinding source{atlas, sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
    if (ground_count != 0) {
        const SDL_GPUBufferBinding binding{ground, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, uint32_t(ground_count), 1, 0, 0);
    }
    if (frame_count != 0) {
        const SDL_GPUBufferBinding binding{frame_vertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, uint32_t(frame_count), 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

/// Adds the frame's lights to the picture, into the lit texture: a triangle
/// over it whose every pixel takes the lights its tile lists.
///
/// @param picture the picture as the scene drew it, at the output size
/// @return false where the pass cannot begin
bool draw_lights(
    const LightResources& lights,
    SDL_GPUCommandBuffer* command,
    SDL_GPUTexture* picture,
    SDL_GPUSampler* sampler,
    uint32_t output_width,
    uint32_t output_height
) {
    SDL_GPUColorTargetInfo target{};
    target.texture = lights.lit;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    const auto parameters = renderer_live_lights::parameters(output_width, output_height);
    SDL_PushGPUFragmentUniformData(command, 0, parameters.data(), sizeof(parameters));
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
    if (!pass)
        return false;
    SDL_BindGPUGraphicsPipeline(pass, lights.pipeline);
    const std::array<SDL_GPUTextureSamplerBinding, 4> sources{
        {{picture, sampler},
         {lights.surface, sampler},
         {lights.table, sampler},
         {lights.tiles, sampler}}
    };
    SDL_BindGPUFragmentSamplers(pass, 0, sources.data(), static_cast<Uint32>(sources.size()));
    SDL_DrawGPUPrimitives(pass, vertices_per_triangle, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return true;
}

} // namespace

struct Context::State {
    Resources resources;
    uint32_t texture_width{};
    uint32_t texture_height{};
    uint32_t output_width{};
    uint32_t output_height{};
    uint32_t samples{};          ///< the samples a pixel of the drawn colour and depth
    uint32_t pipeline_samples{}; ///< the samples a pixel the pipeline draws
    /// Whether the card draws the colour and depth formats with
    /// multisample_count samples a pixel, asked once its device is made.
    bool multisample_known{};
    bool multisample_supported{};
    uint32_t resolved_width{};
    uint32_t resolved_height{};
    uint32_t vertex_capacity{};
    uint32_t upload_capacity{};
    uint64_t atlas_revision{};
    bool atlas_uploaded{};
    uint32_t ground_capacity{};
    /// The kept ground in the ground buffer: its revision, its vertices and
    /// the atlas size they were checked against; no revision before one is.
    uint64_t ground_revision{};
    std::size_t ground_vertex_count{};
    uint32_t ground_atlas_width{};
    uint32_t ground_atlas_height{};
    /// The lights' resources, released before the device in resources.
    LightResources lights;
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
    const WaterEffects* water,
    const KeptGround* ground,
    const std::function<void()>& while_drawing,
    const Lights* lights
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
                         atlas_update->end_row > texture_height ||
                         atlas_update->frame_first_row > atlas_update->frame_end_row ||
                         atlas_update->frame_end_row > texture_height ||
                         std::any_of(
                             atlas_update->regions.begin(),
                             atlas_update->regions.end(),
                             [&](const AtlasRegion& region) {
                                 return region.width == 0 || region.height == 0 ||
                                        region.width > texture_width ||
                                        region.height > texture_height ||
                                        region.x > texture_width - region.width ||
                                        region.y > texture_height - region.height;
                             }
                         ))) {
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
    // The kept ground goes in a buffer of its own, checked and uploaded only
    // when its revision changes; the frame's other vertices, each frame.
    const std::size_t ground_count = ground && ground->revision != 0 ? ground->vertex_count : 0;
    if (ground_count > vertices.size() || ground_count % vertices_per_triangle != 0) {
        error = "Invalid live GPU kept ground";
        return Result::failed;
    }
    const auto& held = *context.state;
    const bool ground_held = ground_count != 0 && held.ground_revision == ground->revision &&
                             held.ground_vertex_count == ground_count &&
                             held.ground_atlas_width == texture_width &&
                             held.ground_atlas_height == texture_height;
    // Ordered bounds also reject NaN and infinities. Each test gives 0 or 1,
    // and a vertex's tests are combined with & rather than &&, so that a
    // frame's million vertices are checked without a branch on each test.
    const auto within = [](float value, float low, float high) -> uint32_t {
        return static_cast<uint32_t>(value >= low) & static_cast<uint32_t>(value <= high);
    };
    const auto unit_range = [&](float value) { return within(value, 0, 1); };
    const auto atlas_width = static_cast<float>(texture_width);
    const auto atlas_height = static_cast<float>(texture_height);
    uint32_t vertices_valid = 1;
    bench_timers::Span validating(bench_timers::Stage::gpu_validate);
    for (const auto& vertex : vertices.subspan(ground_held ? ground_count : 0)) {
        const float size = vertex.terrain_size;
        // A whole number from -7 to 0, or a terrain square of 32, 16, 8 or 4.
        const uint32_t known_terrain_size =
            (within(size, -7, 0) & static_cast<uint32_t>(size == std::trunc(size))) |
            static_cast<uint32_t>(size == 32) | static_cast<uint32_t>(size == 16) |
            static_cast<uint32_t>(size == 8) | static_cast<uint32_t>(size == 4);
        const uint32_t square_in_atlas =
            static_cast<uint32_t>(size <= 0) |
            (static_cast<uint32_t>(vertex.terrain_x + 2 * size <= atlas_width) &
             static_cast<uint32_t>(vertex.terrain_y + 2 * size <= atlas_height));
        const uint32_t valid =
            within(vertex.x, -coordinate_limit, coordinate_limit) &
            within(vertex.height, -coordinate_limit, coordinate_limit) &
            within(vertex.z, -coordinate_limit, coordinate_limit) & unit_range(vertex.u) &
            unit_range(vertex.v) & unit_range(vertex.brightness) & unit_range(vertex.opacity) &
            unit_range(vertex.red) & unit_range(vertex.green) & unit_range(vertex.blue) &
            unit_range(vertex.emission_r) & unit_range(vertex.emission_g) &
            unit_range(vertex.emission_b) &
            within(vertex.terrain_x, 0, size <= -2 ? coordinate_limit : atlas_width) &
            within(vertex.terrain_y, 0, atlas_height) & known_terrain_size &
            unit_range(vertex.terrain_grey) & unit_range(vertex.terrain_known) & square_in_atlas;
        vertices_valid &= valid;
    }
    validating.stop();
    if (vertices_valid == 0) {
        error = "Invalid live GPU vertex coordinates";
        return Result::failed;
    }
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
    if (!state.multisample_known) {
        state.multisample_supported =
            SDL_GPUTextureSupportsSampleCount(
                resources.device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_SAMPLECOUNT_4
            ) &&
            SDL_GPUTextureSupportsSampleCount(
                resources.device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_SAMPLECOUNT_4
            );
        state.multisample_known = true;
    }
    uint32_t supersample = requested_supersample;
    while (output_width > max_dimension / supersample ||
           output_height > max_dimension / supersample)
        supersample /= 2;
    // The card's multisampling takes a 2x2 of the samples asked for where it
    // can: every triangle's edge keeps its samples while the card shades a
    // quarter of the pixels. It needs no larger target, so a frame too wide
    // to supersample still takes it, as long as the card holds no more
    // samples than the largest supersampled target.
    uint32_t samples = 1;
    if (context.multisampling_allowed && state.multisample_supported &&
        requested_supersample >= 2) {
        const uint32_t drawn = std::max<uint32_t>(1, supersample / 2);
        if (uint64_t{output_width} * drawn * output_height * drawn * multisample_count <=
            uint64_t{max_dimension} * max_dimension) {
            samples = multisample_count;
            supersample = drawn;
        }
    }
    const uint32_t render_width = output_width * supersample;
    const uint32_t render_height = output_height * supersample;
    // Multisampled and supersampled, the card averages the samples at the
    // drawn size and the resolve reduces that.
    const bool resolves_twice = samples > 1 && supersample > 1;
    /// Drop incomplete resources so another frame can retry safely.
    const auto fail = [&](const char* operation) {
        error = std::string(operation) + ": " + SDL_GetError();
        context.state = std::make_unique<Context::State>();
        return Result::failed;
    };
    if (resources.pipeline && state.pipeline_samples != samples) {
        SDL_ReleaseGPUGraphicsPipeline(resources.device, resources.pipeline);
        resources.pipeline = nullptr;
    }
    if (!resources.vertex_shader) {
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
    }
    if (!resources.pipeline) {
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
        pipeline_info.multisample_state.sample_count =
            samples > 1 ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_1;
        resources.pipeline = SDL_CreateGPUGraphicsPipeline(resources.device, &pipeline_info);
        if (!resources.pipeline)
            return fail("Create live GPU pipeline");
        state.pipeline_samples = samples;
        make_light_surface_pipeline(state.lights, resources.device, pipeline_info);
    }
    const auto frame_vertices = vertices.subspan(ground_count);
    const auto vertex_bytes = uint32_t(frame_vertices.size_bytes());
    const auto ground_bytes = uint32_t(ground_held ? 0 : ground_count * sizeof(Vertex));
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
    if (render_width != state.output_width || render_height != state.output_height ||
        samples != state.samples || resolves_twice != (resources.multisample_resolved != nullptr)) {
        if (resources.colour)
            SDL_ReleaseGPUTexture(resources.device, resources.colour);
        if (resources.depth)
            SDL_ReleaseGPUTexture(resources.device, resources.depth);
        if (resources.multisample_resolved)
            SDL_ReleaseGPUTexture(resources.device, resources.multisample_resolved);
        resources.multisample_resolved = nullptr;
        // A multisampled target is drawn and averaged, never read.
        const auto sample_count = samples > 1 ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_1;
        resources.colour = create_texture(
            resources.device,
            render_width,
            render_height,
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            samples > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
                        : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            sample_count
        );
        resources.depth = create_texture(
            resources.device,
            render_width,
            render_height,
            SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
            sample_count
        );
        if (resolves_twice)
            resources.multisample_resolved = create_texture(
                resources.device,
                render_width,
                render_height,
                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
            );
        state.output_width = render_width;
        state.output_height = render_height;
        state.samples = samples;
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
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
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
    if (ground_bytes > state.ground_capacity) {
        if (resources.ground)
            SDL_ReleaseGPUBuffer(resources.device, resources.ground);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = ground_bytes;
        resources.ground = SDL_CreateGPUBuffer(resources.device, &info);
        state.ground_capacity = ground_bytes;
    }
    const bool atlas_changed = !state.atlas_uploaded || state.atlas_revision != atlas_revision;
    const bool partial_atlas = atlas_update && state.atlas_uploaded &&
                               state.atlas_revision == atlas_update->previous_revision;
    uint32_t first_row = partial_atlas ? atlas_update->first_row : 0;
    uint32_t end_row = partial_atlas ? atlas_update->end_row : texture_height;
    // The frame's own rows, uploaded as a second region unless they touch
    // the first or it is empty, when the two are taken as one.
    uint32_t frame_first_row = 0;
    uint32_t frame_end_row = 0;
    if (partial_atlas && atlas_update->frame_first_row != atlas_update->frame_end_row) {
        frame_first_row = atlas_update->frame_first_row;
        frame_end_row = atlas_update->frame_end_row;
        if (first_row == end_row || (frame_first_row <= end_row && first_row <= frame_end_row)) {
            if (first_row != end_row) {
                frame_first_row = std::min(frame_first_row, first_row);
                frame_end_row = std::max(frame_end_row, end_row);
            }
            first_row = frame_first_row;
            end_row = frame_end_row;
            frame_first_row = frame_end_row = 0;
        }
    }
    const uint32_t upload_rows = atlas_changed ? end_row - first_row : 0;
    const uint32_t frame_upload_rows = atlas_changed ? frame_end_row - frame_first_row : 0;
    const uint32_t frame_upload_bytes = texture_width * frame_upload_rows * channels;
    const uint32_t atlas_upload_bytes = texture_width * upload_rows * channels + frame_upload_bytes;
    // The rectangles changed beside those rows, each uploaded alone after
    // them; one inside rows uploaded anyway is left out.
    std::span<const AtlasRegion> regions{};
    if (partial_atlas && atlas_changed)
        regions = atlas_update->regions;
    const auto in_rows = [&](const AtlasRegion& region) {
        const auto inside = [&](uint32_t first, uint32_t end) {
            return region.y >= first && region.y + region.height <= end;
        };
        return inside(first_row, end_row) || inside(frame_first_row, frame_end_row);
    };
    uint32_t region_bytes = 0;
    for (const auto& region : regions)
        if (!in_rows(region))
            region_bytes += region.width * region.height * channels;
    const uint32_t upload_bytes = vertex_bytes + ground_bytes + atlas_upload_bytes + region_bytes;
    if (upload_bytes > state.upload_capacity) {
        if (resources.upload)
            SDL_ReleaseGPUTransferBuffer(resources.device, resources.upload);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = upload_bytes;
        resources.upload = SDL_CreateGPUTransferBuffer(resources.device, &info);
        state.upload_capacity = upload_bytes;
    }
    if ((vertex_bytes != 0 && !resources.vertices) || (ground_count != 0 && !resources.ground) ||
        (upload_bytes != 0 && !resources.upload) || !resources.download || !resources.atlas ||
        !resources.colour || !resources.depth || !resources.resolved ||
        (resolves_twice && !resources.multisample_resolved))
        return fail("Create live GPU frame resources");
    // The card adds the lights where it can; elsewhere the caller does.
    auto& lights_held = state.lights;
    bool lights_on_card = false;
    if (lights && context.lights_allowed && lights_held.surface_pipeline) {
        renderer_live_lights::pack(
            lights->sources,
            lights->settings,
            lights->simulation_seconds,
            output_width,
            output_height,
            lights_held.packed
        );
        lights_on_card = lights_held.packed.count == 0 ||
                         prepare_lights(lights_held, output_width, output_height);
    }
    const bool lit = lights_on_card && lights_held.packed.count != 0;
    bench_timers::count_atlas_upload(
        upload_rows + frame_upload_rows, upload_rows == texture_height
    );
    bench_timers::Span uploading(bench_timers::Stage::gpu_upload);
    // The upload holds the frame's vertices, then the kept ground when it
    // has changed, then the atlas rows that have, then the frame's own rows,
    // then the rectangles.
    if (upload_bytes != 0) {
        auto* upload = static_cast<uint8_t*>(
            SDL_MapGPUTransferBuffer(resources.device, resources.upload, false)
        );
        if (!upload)
            return fail("Map live GPU upload");
        if (vertex_bytes != 0)
            std::memcpy(upload, frame_vertices.data(), vertex_bytes);
        if (ground_bytes != 0)
            std::memcpy(upload + vertex_bytes, vertices.data(), ground_bytes);
        if (atlas_upload_bytes != frame_upload_bytes)
            std::memcpy(
                upload + vertex_bytes + ground_bytes,
                texture_rgba.data() + size_t(first_row) * texture_width * channels,
                atlas_upload_bytes - frame_upload_bytes
            );
        if (frame_upload_bytes != 0)
            std::memcpy(
                upload + vertex_bytes + ground_bytes + atlas_upload_bytes - frame_upload_bytes,
                texture_rgba.data() + size_t(frame_first_row) * texture_width * channels,
                frame_upload_bytes
            );
        auto* rectangle = upload + vertex_bytes + ground_bytes + atlas_upload_bytes;
        for (const auto& region : regions) {
            if (in_rows(region))
                continue;
            const size_t row_bytes = size_t(region.width) * channels;
            for (uint32_t row = region.y; row < region.y + region.height; ++row) {
                std::memcpy(
                    rectangle,
                    texture_rgba.data() + (size_t(row) * texture_width + region.x) * channels,
                    row_bytes
                );
                rectangle += row_bytes;
            }
        }
        SDL_UnmapGPUTransferBuffer(resources.device, resources.upload);
    }
    uploading.stop();
    // The submission and the waits for the card, without the caller's work
    // while it draws, which the caller times.
    bench_timers::Span waiting(bench_timers::Stage::gpu_wait);
    // A frame whose caller's work threw while the card drew it is still
    // waiting: it is let go first.
    if (resources.fence) {
        SDL_WaitForGPUFences(resources.device, true, &resources.fence, 1);
        SDL_ReleaseGPUFence(resources.device, resources.fence);
        resources.fence = nullptr;
    }
    resources.command = SDL_AcquireGPUCommandBuffer(resources.device);
    if (!resources.command)
        return fail("Acquire live GPU command buffer");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(resources.command);
    if (!copy)
        return fail("Begin live GPU upload");
    if (vertex_bytes != 0) {
        const SDL_GPUTransferBufferLocation vertex_source{resources.upload, 0};
        const SDL_GPUBufferRegion vertex_destination{resources.vertices, 0, vertex_bytes};
        SDL_UploadToGPUBuffer(copy, &vertex_source, &vertex_destination, false);
    }
    if (ground_bytes != 0) {
        const SDL_GPUTransferBufferLocation ground_source{resources.upload, vertex_bytes};
        const SDL_GPUBufferRegion ground_destination{resources.ground, 0, ground_bytes};
        SDL_UploadToGPUBuffer(copy, &ground_source, &ground_destination, false);
    }
    const SDL_GPUTextureTransferInfo texture_source{
        resources.upload, vertex_bytes + ground_bytes, texture_width, upload_rows
    };
    const SDL_GPUTextureRegion atlas_region{
        resources.atlas, 0, 0, 0, first_row, 0, texture_width, upload_rows, 1
    };
    if (atlas_upload_bytes != frame_upload_bytes)
        SDL_UploadToGPUTexture(copy, &texture_source, &atlas_region, false);
    const SDL_GPUTextureTransferInfo frame_source{
        resources.upload,
        vertex_bytes + ground_bytes + atlas_upload_bytes - frame_upload_bytes,
        texture_width,
        frame_upload_rows
    };
    const SDL_GPUTextureRegion frame_region{
        resources.atlas, 0, 0, 0, frame_first_row, 0, texture_width, frame_upload_rows, 1
    };
    if (frame_upload_bytes != 0)
        SDL_UploadToGPUTexture(copy, &frame_source, &frame_region, false);
    uint32_t region_offset = vertex_bytes + ground_bytes + atlas_upload_bytes;
    for (const auto& region : regions) {
        if (in_rows(region))
            continue;
        const SDL_GPUTextureTransferInfo region_source{
            resources.upload, region_offset, region.width, region.height
        };
        const SDL_GPUTextureRegion region_destination{
            resources.atlas, 0, 0, region.x, region.y, 0, region.width, region.height, 1
        };
        SDL_UploadToGPUTexture(copy, &region_source, &region_destination, false);
        region_offset += region.width * region.height * channels;
    }
    if (lit)
        upload_lights(lights_held, copy);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUColorTargetInfo colour_target{};
    colour_target.texture = resources.colour;
    colour_target.clear_color.a = 1;
    colour_target.load_op = SDL_GPU_LOADOP_CLEAR;
    colour_target.store_op = SDL_GPU_STOREOP_STORE;
    if (samples > 1) {
        // The card averages each pixel's samples as the pass ends, into the
        // picture, or into the drawn size's for the supersampling resolve.
        colour_target.store_op = SDL_GPU_STOREOP_RESOLVE;
        colour_target.resolve_texture =
            resolves_twice ? resources.multisample_resolved : resources.resolved;
    }
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
    camera[15] = float(samples);
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
    const std::array<SDL_GPUTextureSamplerBinding, 2> sampler_bindings{
        {{resources.atlas, resources.sampler}, {resources.atlas, resources.linear_sampler}}
    };
    SDL_BindGPUFragmentSamplers(
        pass, 0, sampler_bindings.data(), static_cast<Uint32>(sampler_bindings.size())
    );
    // The ground first and then the rest, in the frame's order.
    if (ground_count != 0) {
        const SDL_GPUBufferBinding binding{resources.ground, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, uint32_t(ground_count), 1, 0, 0);
    }
    if (!frame_vertices.empty()) {
        const SDL_GPUBufferBinding binding{resources.vertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, uint32_t(frame_vertices.size()), 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    // The surface the lights reach, from the same vertices.
    if (lit && !draw_light_surface(
                   lights_held,
                   resources.command,
                   camera,
                   resources.atlas,
                   resources.sampler,
                   resources.ground,
                   ground_count,
                   resources.vertices,
                   frame_vertices.size()
               ))
        return fail("Begin live GPU lights' surface");
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
        const SDL_GPUTextureSamplerBinding source{
            resolves_twice ? resources.multisample_resolved : resources.colour, resources.sampler
        };
        SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
        SDL_DrawGPUPrimitives(pass, vertices_per_triangle, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    // The picture as the scene drew it, and with the lights over it.
    SDL_GPUTexture* const picture =
        supersample > 1 || samples > 1 ? resources.resolved : resources.colour;
    if (lit &&
        !draw_lights(
            lights_held, resources.command, picture, resources.sampler, output_width, output_height
        ))
        return fail("Begin live GPU lights");
    copy = SDL_BeginGPUCopyPass(resources.command);
    if (!copy)
        return fail("Begin live GPU download");
    const SDL_GPUTextureRegion colour_region{
        lit ? lights_held.lit : picture, 0, 0, 0, 0, 0, output_width, output_height, 1
    };
    const SDL_GPUTextureTransferInfo download_destination{
        resources.download, 0, output_width, output_height
    };
    SDL_DownloadFromGPUTexture(copy, &colour_region, &download_destination);
    SDL_EndGPUCopyPass(copy);
    resources.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(resources.command);
    resources.command = nullptr;
    if (!resources.fence)
        return fail("Submit live GPU render");
    // The caller's work runs while the card draws, rather than after the
    // picture comes back.
    waiting.stop();
    if (while_drawing)
        while_drawing();
    waiting.start();
    if (!SDL_WaitForGPUFences(resources.device, true, &resources.fence, 1))
        return fail("Complete live GPU render");
    waiting.stop();
    const bench_timers::Span downloading(bench_timers::Stage::gpu_download);
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
    context.actual_multisample = samples;
    bench_timers::note_multisample(samples);
    context.lights_drawn = lights_on_card;
    state.atlas_revision = atlas_revision;
    state.atlas_uploaded = true;
    if (ground_bytes != 0) {
        state.ground_revision = ground->revision;
        state.ground_vertex_count = ground_count;
        state.ground_atlas_width = texture_width;
        state.ground_atlas_height = texture_height;
    }
    return Result::success;
}

} // namespace oa::app::renderer_live_gpu
