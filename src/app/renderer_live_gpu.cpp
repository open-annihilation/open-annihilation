// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_live_gpu.hpp"
#include "bench_timers.hpp"
#include "renderer_live_lights.hpp"
#include "renderer_water_appearance.hpp"
#include "oa/present/game_text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

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
    /// The device is Context::shared_device, which another owns.
    bool device_shared{};
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
    SDL_GPUBuffer* next_ground{};     ///< the next kept ground's, sent ahead of it
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
        if (next_ground)
            SDL_ReleaseGPUBuffer(device, next_ground);
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
        if (!device_shared)
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

/// The card's part of the model draws (ModelDraws): the models' vertex
/// stage and its pipelines into the scene and into the lights' surface, the
/// types' corners it keeps, and each frame's instances, vertices, textures
/// and indices.
/// The card's part of the units' shadows (ModelDraws::casts): the stages
/// and pipelines that cast them into the shadows' map and lay the map on
/// the frame's ground, and the map, with the depth its casts draw at.
struct ShadowResources {
    SDL_GPUDevice* device{}; ///< the device they are made on
    SDL_GPUShader* cast_vertex{};
    SDL_GPUShader* cast_fragment{};
    SDL_GPUShader* ground_vertex{};
    SDL_GPUShader* ground_fragment{};
    SDL_GPUGraphicsPipeline* cast_pipeline{}; ///< into the map, one sample a texel
    /// The ground drawn again, darkened by the map, into the scene as its
    /// pipeline draws, without writing depth.
    SDL_GPUGraphicsPipeline* ground_pipeline{};
    SDL_GPUTexture* map{};       ///< the light each texel's shadows leave
    SDL_GPUTexture* map_depth{}; ///< the casts' depths
    uint32_t width{};            ///< the map's texels across
    uint32_t height{};           ///< and down

    ShadowResources() = default;
    ShadowResources(const ShadowResources&) = delete;
    ShadowResources& operator=(const ShadowResources&) = delete;

    /// Release the handles before the device they were made on.
    ~ShadowResources() {
        if (!device)
            return;
        for (SDL_GPUGraphicsPipeline* made : {cast_pipeline, ground_pipeline})
            if (made)
                SDL_ReleaseGPUGraphicsPipeline(device, made);
        for (SDL_GPUShader* made : {cast_vertex, cast_fragment, ground_vertex, ground_fragment})
            if (made)
                SDL_ReleaseGPUShader(device, made);
        for (SDL_GPUTexture* made : {map, map_depth})
            if (made)
                SDL_ReleaseGPUTexture(device, made);
    }
};

struct ModelResources {
    SDL_GPUDevice* device{}; ///< the device they are made on
    SDL_GPUShader* vertex_shader{};
    /// The models' vertex stage and the scene's fragment stage, drawn as the
    /// scene's pipeline draws.
    SDL_GPUGraphicsPipeline* pipeline{};
    /// The models' vertex stage and the surface's fragment stage, drawn as
    /// the surface's pipeline draws; null where the card draws no surface.
    SDL_GPUGraphicsPipeline* surface_pipeline{};
    SDL_GPUBuffer* corners{}; ///< every type's triangle corners
    SDL_GPUBuffer* frame{};   ///< the camera's rows, then the frame's draws
    SDL_GPUBuffer* indices{}; ///< the frame's triangle corners drawn
    SDL_GPUTransferBuffer* upload{};
    uint32_t corner_capacity{}; ///< bytes
    uint32_t frame_capacity{};
    uint32_t index_capacity{};
    uint32_t upload_capacity{};
    /// The corners on the card: their revision and how many words of them;
    /// no revision before any are.
    uint64_t corners_revision{};
    std::size_t corner_words{};
    /// This frame's upload: the corners from corner_from, the frame's draws
    /// and its indices, one after another.
    std::size_t corner_from{};
    uint32_t corner_bytes{};
    uint32_t frame_bytes{};
    uint32_t index_bytes{};
    /// The units' shadows, released before the rest.
    ShadowResources shadows;

    ModelResources() = default;
    ModelResources(const ModelResources&) = delete;
    ModelResources& operator=(const ModelResources&) = delete;

    /// Release the handles before the device they were made on.
    ~ModelResources() {
        if (!device)
            return;
        for (SDL_GPUGraphicsPipeline* made : {pipeline, surface_pipeline})
            if (made)
                SDL_ReleaseGPUGraphicsPipeline(device, made);
        if (vertex_shader)
            SDL_ReleaseGPUShader(device, vertex_shader);
        for (SDL_GPUBuffer* made : {corners, frame, indices})
            if (made)
                SDL_ReleaseGPUBuffer(device, made);
        if (upload)
            SDL_ReleaseGPUTransferBuffer(device, upload);
    }
};

/// The words before the frame's draws in ModelResources::frame: the scene
/// camera's first four rows, where each part of the draws begins, three
/// unused words and the shadows' map's place.
constexpr uint32_t model_frame_header_words = 28;
/// The words of a shadow cast in ModelResources::frame: its instance and
/// kind, its base, half its ground, its least height, its light, its depth
/// and its model's place across and down.
constexpr uint32_t shadow_cast_words = 9;
/// The format of the shadows' map: the light left, from 0 to 1.
constexpr SDL_GPUTextureFormat shadow_map_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
/// The shadows' map is made this many texels larger at a time.
constexpr uint32_t shadow_map_step = 512;
/// The words of an instance in ModelResources::frame: its type's corners,
/// its vertices, pieces and textures, its brightness, and room.
constexpr uint32_t model_instance_words = 8;
/// A triangle corner's vertex, and a texture, in ModelDraws::corners; an
/// index's corner in ModelDraws::indices.
constexpr uint32_t model_vertex_mask = (1U << model_piece_shift) - 1;
constexpr uint32_t model_texture_mask = (1U << model_texture_bits) - 1;
static_assert(model_corner_bits == model_piece_shift);
/// The most corners, vertices and pieces an instance's type has, and
/// textures it draws with.
constexpr uint32_t model_field_limit = 1U << 16;
constexpr uint32_t model_texture_limit = 1U << model_texture_bits;

/// Whether a frame's model draws hold together: every instance's ranges in
/// their lists and within the fields' limits, every corner of its type
/// naming a vertex, piece and texture it has, every index an instance of
/// its run and a corner of its type, the runs in the frame's order among
/// its vertices after the kept ground, and every value within its range.
///
/// @param models the model draws
/// @param ground_count the frame's first vertices, the kept ground
/// @param vertex_count the frame's vertices
/// @return true where render() may draw them
bool valid_model_draws(
    const ModelDraws& models, std::size_t ground_count, std::size_t vertex_count
) {
    if (models.corners.size() % 2 != 0 || models.positions.size() % 3 != 0 ||
        models.textures.size() % 4 != 0 || models.instances.size() > largest_model_instance_count ||
        models.indices.size() > max_vertices || (models.corners.empty() && !models.runs.empty()))
        return false;
    const std::size_t corner_count = models.corners.size() / 2;
    const std::size_t position_count = models.positions.size() / 3;
    const std::size_t texture_count = models.textures.size() / 4;
    const auto within = [](std::size_t first, std::size_t count, std::size_t size) {
        return first <= size && count <= size - first;
    };

    // Each type's corners are read once, for the most each names.
    struct TypeReach {
        uint32_t corners{};
        uint32_t corner_count{};
        uint32_t vertices{};
        uint32_t pieces{};
        uint32_t textures{};
    };

    std::vector<TypeReach> types;
    for (const auto& instance : models.instances) {
        if (!within(instance.corners, instance.corner_count, corner_count) ||
            !within(instance.positions, instance.position_count, position_count) ||
            !within(instance.pieces, instance.piece_count, models.piece_frames.size()) ||
            !within(instance.textures, instance.texture_count, texture_count) ||
            instance.corner_count > model_field_limit ||
            instance.position_count > model_field_limit ||
            instance.piece_count > model_field_limit ||
            instance.texture_count > model_texture_limit ||
            !(instance.brightness >= 0 && instance.brightness <= 1))
            return false;
        auto type = std::find_if(types.begin(), types.end(), [&](const TypeReach& reach) {
            return reach.corners == instance.corners && reach.corner_count == instance.corner_count;
        });
        if (type == types.end()) {
            TypeReach reach{instance.corners, instance.corner_count};
            for (uint32_t corner = 0; corner < instance.corner_count; ++corner) {
                const uint32_t place = models.corners[2 * (size_t{instance.corners} + corner)];
                const uint32_t drawn = models.corners[2 * (size_t{instance.corners} + corner) + 1];
                reach.vertices = std::max(reach.vertices, (place & model_vertex_mask) + 1);
                reach.pieces = std::max(reach.pieces, (place >> model_piece_shift) + 1);
                reach.textures = std::max(
                    {reach.textures,
                     (drawn & model_texture_mask) + 1,
                     ((drawn >> model_texture_bits) & model_texture_mask) + 1}
                );
            }
            type = types.insert(types.end(), reach);
        }
        if (type->vertices > instance.position_count || type->pieces > instance.piece_count ||
            type->textures > instance.texture_count)
            return false;
    }
    // Branch-free tests, as the frame's vertices are checked.
    const auto inside = [](float value, float low, float high) -> uint32_t {
        return static_cast<uint32_t>(value >= low) & static_cast<uint32_t>(value <= high);
    };
    uint32_t valid = 1;
    for (const float value : models.positions)
        valid &= inside(value, -coordinate_limit, coordinate_limit);
    for (const float value : models.textures)
        valid &= inside(value, 0, 1);
    for (const uint32_t frame : models.piece_frames)
        valid &= static_cast<uint32_t>(frame <= 1);
    std::size_t vertex = ground_count;
    for (const auto& run : models.runs) {
        if (run.vertex < vertex || run.vertex > vertex_count ||
            !within(run.first_index, run.index_count, models.indices.size()) ||
            run.index_count % vertices_per_triangle != 0 ||
            (run.index_count != 0 && run.first_instance >= models.instances.size()))
            return false;
        vertex = run.vertex;
        const std::size_t instances = models.instances.size() - run.first_instance;
        for (const uint32_t index : models.indices.subspan(run.first_index, run.index_count)) {
            const std::size_t instance = index >> model_corner_bits;
            if (instance >= std::min<std::size_t>(instances, model_run_instances))
                return false;
            valid &= static_cast<uint32_t>(
                (index & model_vertex_mask) <
                models.instances[run.first_instance + instance].corner_count
            );
        }
    }
    // The shadows: each cast an instance's, of a known kind, its values in
    // their ranges; each index a cast of its run and a corner of its
    // instance's type; the map within the largest texture, and the ground
    // and the shadows' place among the frame's vertices.
    if (models.casts.size() > largest_model_instance_count * 2 ||
        models.cast_indices.size() > max_vertices)
        return false;
    for (const auto& cast : models.casts) {
        if (cast.instance >= models.instances.size() || cast.kind > shadow_cast_cut ||
            !std::isfinite(cast.base) || !std::isfinite(cast.ground_half) ||
            !std::isfinite(cast.least_height) || !std::isfinite(cast.across) ||
            !std::isfinite(cast.down))
            return false;
        valid &= inside(cast.light, 0, 1) & inside(cast.depth, 0, 1);
    }
    for (const auto& run : models.cast_runs) {
        if (!within(run.first_index, run.index_count, models.cast_indices.size()) ||
            run.index_count % vertices_per_triangle != 0 ||
            (run.index_count != 0 && run.first_instance >= models.casts.size()))
            return false;
        const std::size_t casts = models.casts.size() - run.first_instance;
        for (const uint32_t index : models.cast_indices.subspan(run.first_index, run.index_count)) {
            const std::size_t cast = index >> model_corner_bits;
            if (cast >= std::min<std::size_t>(casts, model_run_instances))
                return false;
            valid &= static_cast<uint32_t>(
                (index & model_vertex_mask) <
                models.instances[models.casts[run.first_instance + cast].instance].corner_count
            );
        }
    }
    const auto& map = models.shadow_map;
    if (!models.casts.empty() &&
        (!std::isfinite(map.left) || !std::isfinite(map.top) ||
         !(map.scale == 1 || map.scale == 2 || map.scale == 4 || map.scale == 8) ||
         map.width == 0 || map.height == 0 || map.width > max_dimension ||
         map.height > max_dimension || models.shadow_ground > vertex_count ||
         models.shadow_vertex < ground_count || models.shadow_vertex > vertex_count))
        return false;
    return valid != 0;
}

/// Makes the models' pipelines from the scene's and the surface's, in place
/// of any made before: the models' vertex stage with no vertex buffer,
/// everything else as theirs; none into the surface where it has none.
///
/// @param scene the scene's pipeline, as made
/// @param surface_shader the surface's fragment stage; null for no surface
/// @return false where the models' vertex stage or pipeline cannot be made
bool make_model_pipelines(
    ModelResources& models,
    SDL_GPUDevice* device,
    const SDL_GPUGraphicsPipelineCreateInfo& scene,
    SDL_GPUShader* surface_shader
) {
    models.device = device;
    for (SDL_GPUGraphicsPipeline** made : {&models.pipeline, &models.surface_pipeline}) {
        if (*made)
            SDL_ReleaseGPUGraphicsPipeline(device, *made);
        *made = nullptr;
    }
    if (!models.vertex_shader) {
        SDL_GPUShaderCreateInfo info{};
        stage_code(device, info, live_model_vertex_spirv, live_model_vertex_msl);
        info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        info.num_storage_buffers = 2;
        models.vertex_shader = SDL_CreateGPUShader(device, &info);
        if (!models.vertex_shader)
            return false;
    }
    auto info = scene;
    info.vertex_shader = models.vertex_shader;
    info.vertex_input_state = SDL_GPUVertexInputState{};
    models.pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!models.pipeline)
        return false;
    // The shadows laid on the ground: the scene's vertices, samples and
    // blending, the shadows' stages, and no depth written.
    auto& shadows = models.shadows;
    shadows.device = device;
    if (shadows.ground_pipeline)
        SDL_ReleaseGPUGraphicsPipeline(device, shadows.ground_pipeline);
    shadows.ground_pipeline = nullptr;
    if (!shadows.ground_vertex) {
        SDL_GPUShaderCreateInfo stage{};
        stage_code(device, stage, live_shadow_vertex_spirv, live_shadow_vertex_msl);
        stage.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        stage.num_uniform_buffers = 1;
        shadows.ground_vertex = SDL_CreateGPUShader(device, &stage);
    }
    if (!shadows.ground_fragment) {
        SDL_GPUShaderCreateInfo stage{};
        stage_code(device, stage, live_shadow_fragment_spirv, live_shadow_fragment_msl);
        stage.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        stage.num_samplers = 3;
        stage.num_uniform_buffers = 2;
        shadows.ground_fragment = SDL_CreateGPUShader(device, &stage);
    }
    if (!shadows.ground_vertex || !shadows.ground_fragment)
        return false;
    auto ground = scene;
    ground.vertex_shader = shadows.ground_vertex;
    ground.fragment_shader = shadows.ground_fragment;
    ground.depth_stencil_state.enable_depth_write = false;
    shadows.ground_pipeline = SDL_CreateGPUGraphicsPipeline(device, &ground);
    if (!shadows.ground_pipeline)
        return false;
    if (surface_shader) {
        // As make_light_surface_pipeline makes the surface's.
        SDL_GPUColorTargetDescription target{};
        target.format = light_surface_format;
        info.fragment_shader = surface_shader;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        info.multisample_state = SDL_GPUMultisampleState{};
        models.surface_pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (!models.surface_pipeline)
            return false;
    }
    return true;
}

/// Makes what the units' shadows need for a map of the texels a frame's
/// shadows ask for: the casts' stages and pipeline, once, and the map and
/// its depth, made larger, a step at a time, when the frame needs more.
///
/// @return false where any of it cannot be made
bool prepare_shadows(ShadowResources& shadows, const ShadowMap& map) {
    SDL_GPUDevice* const device = shadows.device;
    if (!shadows.cast_pipeline) {
        if (!shadows.cast_vertex) {
            SDL_GPUShaderCreateInfo info{};
            stage_code(device, info, live_shadow_cast_vertex_spirv, live_shadow_cast_vertex_msl);
            info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
            info.num_storage_buffers = 2;
            shadows.cast_vertex = SDL_CreateGPUShader(device, &info);
        }
        if (!shadows.cast_fragment) {
            SDL_GPUShaderCreateInfo info{};
            stage_code(
                device, info, live_shadow_cast_fragment_spirv, live_shadow_cast_fragment_msl
            );
            info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
            shadows.cast_fragment = SDL_CreateGPUShader(device, &info);
        }
        if (!shadows.cast_vertex || !shadows.cast_fragment)
            return false;
        // Each texel a cast covers takes its light, multiplied in.
        SDL_GPUColorTargetDescription target{};
        target.format = shadow_map_format;
        auto& blend = target.blend_state;
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_COLOR;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = shadows.cast_vertex;
        info.fragment_shader = shadows.cast_fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        // The game fills a polygon of its picture or silhouette only where
        // its corners run clockwise, as laid; one facing the other way, as
        // a model's underside does, it leaves out.
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
        info.target_info.has_depth_stencil_target = true;
        shadows.cast_pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (!shadows.cast_pipeline)
            return false;
    }
    const auto stepped = [](uint32_t texels) {
        return std::min(
            max_dimension, (texels + shadow_map_step - 1) / shadow_map_step * shadow_map_step
        );
    };
    if (map.width > shadows.width || map.height > shadows.height) {
        const uint32_t width = stepped(std::max(map.width, shadows.width));
        const uint32_t height = stepped(std::max(map.height, shadows.height));
        for (SDL_GPUTexture** made : {&shadows.map, &shadows.map_depth}) {
            if (*made)
                SDL_ReleaseGPUTexture(device, *made);
            *made = nullptr;
        }
        shadows.width = shadows.height = 0;
        shadows.map = create_texture(
            device,
            width,
            height,
            shadow_map_format,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
        );
        shadows.map_depth = create_texture(
            device,
            width,
            height,
            SDL_GPU_TEXTUREFORMAT_D16_UNORM,
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
        );
        if (!shadows.map || !shadows.map_depth)
            return false;
        shadows.width = width;
        shadows.height = height;
    }
    return true;
}

/// The shadows' map's place, as the casts' vertex stage reads it: its left
/// and top in map pixels, and how much of the frame's texels a map pixel
/// is, across and down. The frame's texels, not the whole texture, which
/// an earlier frame may have made larger, so that a frame's shadows fall on
/// the same texels whatever frames came before it.
std::array<float, 4> shadow_map_place(const ShadowMap& map) {
    return {
        map.left,
        map.top,
        1.0F / (map.scale * float(map.width)),
        1.0F / (map.scale * float(map.height))
    };
}

/// Casts the frame's units' shadows into the shadows' map, cleared to full
/// light, each cast's triangles at its own depth, into the frame's texels
/// at its top left.
///
/// @return false where the pass cannot begin
bool cast_shadows(
    const ModelResources& models, const ModelDraws& draws, SDL_GPUCommandBuffer* command
) {
    const auto& shadows = models.shadows;
    SDL_GPUColorTargetInfo target{};
    target.texture = shadows.map;
    target.clear_color = {1, 1, 1, 1};
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = shadows.map_depth;
    depth.clear_depth = 1;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, &depth);
    if (!pass)
        return false;
    SDL_BindGPUGraphicsPipeline(pass, shadows.cast_pipeline);
    // The frame's texels, at the texture's top left.
    const SDL_GPUViewport viewport{
        0, 0, float(draws.shadow_map.width), float(draws.shadow_map.height), 0, 1
    };
    SDL_SetGPUViewport(pass, &viewport);
    const std::array<SDL_GPUBuffer*, 2> storage{models.corners, models.frame};
    SDL_BindGPUVertexStorageBuffers(pass, 0, storage.data(), uint32_t(storage.size()));
    const SDL_GPUBufferBinding indices{models.indices, 0};
    SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    // The casts' indices follow the models'.
    const auto first = uint32_t(draws.indices.size());
    for (const auto& run : draws.cast_runs)
        if (run.index_count != 0)
            SDL_DrawGPUIndexedPrimitives(
                pass,
                run.index_count,
                1,
                first + run.first_index,
                int32_t(run.first_instance << model_corner_bits),
                0
            );
    SDL_EndGPURenderPass(pass);
    return true;
}

/// Draws the frame's ground again, darkened by the shadows' map, into the
/// scene's pass; its uniform block was pushed before the pass began.
///
/// @param ground the buffer of the kept ground, or of the frame's vertices
///     where the ground is not kept
/// @param count the ground's vertices at its start
void lay_shadows(
    SDL_GPURenderPass* pass,
    const ShadowResources& shadows,
    SDL_GPUSampler* sampler,
    SDL_GPUBuffer* ground,
    std::size_t count
) {
    if (count == 0)
        return;
    SDL_BindGPUGraphicsPipeline(pass, shadows.ground_pipeline);
    // The scene's atlases keep the first two places.
    const SDL_GPUTextureSamplerBinding map{shadows.map, sampler};
    SDL_BindGPUFragmentSamplers(pass, 2, &map, 1);
    const SDL_GPUBufferBinding binding{ground, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, uint32_t(count), 1, 0, 0);
}

/// Makes the buffers a frame's model draws need and copies into the upload
/// the corners the card lacks, the camera's rows and the draws, and the
/// indices.
///
/// @param camera the scene's uniform block, whose first rows the models'
///     vertex stage reads from the frame's draws
/// @return false where any of it cannot be made
bool prepare_models(
    ModelResources& models, const ModelDraws& draws, std::span<const float> camera
) {
    SDL_GPUDevice* const device = models.device;
    const auto grown = [&](SDL_GPUBuffer*& buffer,
                           uint32_t& capacity,
                           uint32_t bytes,
                           SDL_GPUBufferUsageFlags usage) {
        if (bytes <= capacity)
            return true;
        if (buffer)
            SDL_ReleaseGPUBuffer(device, buffer);
        capacity = 0;
        SDL_GPUBufferCreateInfo info{};
        info.usage = usage;
        info.size = bytes;
        buffer = SDL_CreateGPUBuffer(device, &info);
        if (!buffer)
            return false;
        capacity = bytes;
        return true;
    };
    // The corners already on the card stay while their revision does, and
    // their buffer holds them; others go up whole.
    const auto corners_bytes = uint32_t(draws.corners.size_bytes());
    if (corners_bytes > models.corner_capacity)
        models.corner_words = 0;
    if (!grown(
            models.corners,
            models.corner_capacity,
            corners_bytes,
            SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ
        ))
        return false;
    const bool kept = models.corners_revision == draws.corners_revision &&
                      models.corner_words != 0 && models.corner_words <= draws.corners.size();
    models.corner_from = kept ? models.corner_words : 0;
    models.corner_bytes = uint32_t((draws.corners.size() - models.corner_from) * sizeof(uint32_t));
    const auto words = [](std::size_t count) { return uint32_t(count * sizeof(uint32_t)); };
    const uint32_t instance_words = uint32_t(draws.instances.size()) * model_instance_words;
    models.frame_bytes = words(
        model_frame_header_words + instance_words + draws.positions.size() +
        draws.piece_frames.size() + draws.textures.size() + draws.casts.size() * shadow_cast_words
    );
    models.index_bytes = uint32_t(draws.indices.size_bytes() + draws.cast_indices.size_bytes());
    if (!draws.casts.empty() && !prepare_shadows(models.shadows, draws.shadow_map))
        return false;
    if (!grown(
            models.frame,
            models.frame_capacity,
            models.frame_bytes,
            SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ
        ) ||
        !grown(
            models.indices, models.index_capacity, models.index_bytes, SDL_GPU_BUFFERUSAGE_INDEX
        ))
        return false;
    const uint32_t upload_bytes = models.corner_bytes + models.frame_bytes + models.index_bytes;
    if (upload_bytes > models.upload_capacity) {
        if (models.upload)
            SDL_ReleaseGPUTransferBuffer(device, models.upload);
        models.upload_capacity = 0;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = upload_bytes;
        models.upload = SDL_CreateGPUTransferBuffer(device, &info);
        if (!models.upload)
            return false;
        models.upload_capacity = upload_bytes;
    }
    auto* upload = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, models.upload, true));
    if (!upload)
        return false;
    std::memcpy(upload, draws.corners.data() + models.corner_from, models.corner_bytes);
    auto* frame = reinterpret_cast<uint32_t*>(upload + models.corner_bytes);
    // The camera's horizontal, vertical, depth and viewport rows.
    constexpr std::size_t camera_rows = 16;
    std::memcpy(frame, camera.data(), camera_rows * sizeof(float));
    const uint32_t positions = instance_words;
    const uint32_t piece_frames = positions + uint32_t(draws.positions.size());
    const uint32_t textures = piece_frames + uint32_t(draws.piece_frames.size());
    const uint32_t casts = textures + uint32_t(draws.textures.size());
    frame[camera_rows] = 0;
    frame[camera_rows + 1] = positions;
    frame[camera_rows + 2] = piece_frames;
    frame[camera_rows + 3] = textures;
    frame[camera_rows + 4] = casts;
    frame[camera_rows + 5] = frame[camera_rows + 6] = frame[camera_rows + 7] = 0;
    const auto place =
        draws.casts.empty() ? std::array<float, 4>{} : shadow_map_place(draws.shadow_map);
    std::memcpy(frame + camera_rows + 8, place.data(), sizeof(place));
    uint32_t* at = frame + model_frame_header_words;
    for (const auto& instance : draws.instances) {
        const std::array<uint32_t, model_instance_words> record{
            instance.corners,
            instance.positions,
            instance.pieces,
            instance.textures,
            std::bit_cast<uint32_t>(instance.brightness)
        };
        at = std::copy(record.begin(), record.end(), at);
    }
    std::memcpy(at, draws.positions.data(), draws.positions.size_bytes());
    at += draws.positions.size();
    std::memcpy(at, draws.piece_frames.data(), draws.piece_frames.size_bytes());
    at += draws.piece_frames.size();
    std::memcpy(at, draws.textures.data(), draws.textures.size_bytes());
    at += draws.textures.size();
    for (const auto& cast : draws.casts) {
        const std::array<uint32_t, shadow_cast_words> record{
            cast.instance,
            cast.kind,
            std::bit_cast<uint32_t>(cast.base),
            std::bit_cast<uint32_t>(cast.ground_half),
            std::bit_cast<uint32_t>(cast.least_height),
            std::bit_cast<uint32_t>(cast.light),
            std::bit_cast<uint32_t>(cast.depth),
            std::bit_cast<uint32_t>(cast.across),
            std::bit_cast<uint32_t>(cast.down)
        };
        at = std::copy(record.begin(), record.end(), at);
    }
    std::memcpy(
        upload + models.corner_bytes + models.frame_bytes,
        draws.indices.data(),
        draws.indices.size_bytes()
    );
    std::memcpy(
        upload + models.corner_bytes + models.frame_bytes + draws.indices.size_bytes(),
        draws.cast_indices.data(),
        draws.cast_indices.size_bytes()
    );
    SDL_UnmapGPUTransferBuffer(device, models.upload);
    return true;
}

/// Uploads what prepare_models() copied, in the frame's copy pass, and notes
/// the corners the card then holds.
void upload_models(ModelResources& models, const ModelDraws& draws, SDL_GPUCopyPass* copy) {
    if (models.corner_bytes != 0) {
        const SDL_GPUTransferBufferLocation source{models.upload, 0};
        const SDL_GPUBufferRegion destination{
            models.corners, uint32_t(models.corner_from * sizeof(uint32_t)), models.corner_bytes
        };
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    }
    // The frame's draws and indices are written whole: cycled, since a
    // frame left on the card may still be reading the frame before's. The
    // corners above are added to, and stay.
    const SDL_GPUTransferBufferLocation frame_source{models.upload, models.corner_bytes};
    const SDL_GPUBufferRegion frame_destination{models.frame, 0, models.frame_bytes};
    SDL_UploadToGPUBuffer(copy, &frame_source, &frame_destination, true);
    if (models.index_bytes != 0) {
        const SDL_GPUTransferBufferLocation index_source{
            models.upload, models.corner_bytes + models.frame_bytes
        };
        const SDL_GPUBufferRegion index_destination{models.indices, 0, models.index_bytes};
        SDL_UploadToGPUBuffer(copy, &index_source, &index_destination, true);
    }
    models.corners_revision = draws.corners_revision;
    models.corner_words = draws.corners.size();
}

/// Draws the frame's vertices after the kept ground, in their order, with
/// each run of model draws before its vertex.
///
/// @param pass the render pass, its fragment samplers and uniforms bound
/// @param pipeline the frame's vertices' pipeline
/// @param frame_vertices the buffer of the frame's vertices after the ground
/// @param frame_count those vertices
/// @param ground_count the kept ground's vertices before them
/// @param models the models' resources, uploaded; null for no model draws
/// @param model_pipeline the models' pipeline into the same pass
/// @param runs the model draws' runs
void draw_frame_and_models(
    SDL_GPURenderPass* pass,
    SDL_GPUGraphicsPipeline* pipeline,
    SDL_GPUBuffer* frame_vertices,
    std::size_t frame_count,
    std::size_t ground_count,
    const ModelResources* models,
    SDL_GPUGraphicsPipeline* model_pipeline,
    std::span<const ModelRun> runs,
    const std::function<void()>& shadows = {},
    std::size_t shadow_vertex = 0
) {
    const SDL_GPUBufferBinding binding{frame_vertices, 0};
    std::size_t drawn = 0;
    const auto draw_vertices = [&](std::size_t end) {
        if (end <= drawn)
            return;
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, uint32_t(end - drawn), 1, uint32_t(drawn), 0);
        drawn = end;
    };
    // The shadows are laid before the vertex they are given, ahead of any
    // run drawn before it.
    if (shadows) {
        draw_vertices(std::max(shadow_vertex, ground_count) - ground_count);
        shadows();
    }
    for (const auto& run : runs) {
        if (!models || run.index_count == 0)
            continue;
        draw_vertices(run.vertex - ground_count);
        SDL_BindGPUGraphicsPipeline(pass, model_pipeline);
        const std::array<SDL_GPUBuffer*, 2> storage{models->corners, models->frame};
        SDL_BindGPUVertexStorageBuffers(pass, 0, storage.data(), uint32_t(storage.size()));
        const SDL_GPUBufferBinding indices{models->indices, 0};
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(
            pass,
            run.index_count,
            1,
            run.first_index,
            int32_t(run.first_instance << model_corner_bits),
            0
        );
    }
    draw_vertices(frame_count);
}

/// Draws the surface the lights reach: the frame's triangles again, the
/// kept ground and then the rest with the model draws among them, one
/// sample a pixel whatever the scene's samples, so that each pixel holds
/// the face under its centre.
///
/// @param camera the scene's uniform block
/// @param models the models' resources, uploaded; null for no model draws
/// @param runs the model draws' runs
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
    std::size_t frame_count,
    const ModelResources* models,
    std::span<const ModelRun> runs
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
    draw_frame_and_models(
        pass,
        lights.surface_pipeline,
        frame_vertices,
        frame_count,
        ground_count,
        models && models->surface_pipeline ? models : nullptr,
        models ? models->surface_pipeline : nullptr,
        runs
    );
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

/// The overlay's texels: the canvas's colours as ARGB8888 words, the
/// operations' and the lists' words, and the palette and the gamma.
constexpr SDL_GPUTextureFormat overlay_canvas_format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
constexpr SDL_GPUTextureFormat overlay_operations_format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT;
constexpr SDL_GPUTextureFormat overlay_words_format = SDL_GPU_TEXTUREFORMAT_R32_UINT;
/// The palette's and the gamma's texels a row, and their rows.
constexpr uint32_t overlay_colour_entries = 256;
constexpr uint32_t overlay_colour_rows = 2;

/// The card's part of the overlay (renderer_live_overlay.hpp): the canvas,
/// the operations and their tiles' lists, the palette and the gamma, and the
/// picture with the overlay added, which the window presents.
struct OverlayResources {
    SDL_GPUDevice* device{};        ///< the device they are made on
    SDL_GPUShader* vertex_shader{}; ///< the resolve's triangle
    SDL_GPUShader* fragment_shader{};
    SDL_GPUGraphicsPipeline* pipeline{};
    SDL_GPUTexture* canvas{};
    SDL_GPUTexture* operations{};
    SDL_GPUTexture* lists{};
    SDL_GPUTexture* colours{};
    SDL_GPUTexture* composed{};
    SDL_GPUTransferBuffer* upload{};
    uint32_t upload_capacity{};
    uint32_t width{}; ///< the canvas's and the composed picture's size
    uint32_t height{};
    uint32_t operation_rows{}; ///< rows of the operations' table
    uint32_t list_rows{};      ///< rows of the lists' table
    /// For each band of the canvas, whether the texture holds an opaque
    /// pixel there from an earlier overlay; empty until the whole canvas has
    /// been sent once.
    std::vector<uint8_t> held_bands{};
    /// Whether the card has the formats the overlay takes, asked once.
    bool formats_known{};
    bool formats_supported{};

    OverlayResources() = default;
    OverlayResources(const OverlayResources&) = delete;
    OverlayResources& operator=(const OverlayResources&) = delete;

    /// Release the handles before the device they were made on.
    ~OverlayResources() {
        if (!device)
            return;
        if (pipeline)
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        for (SDL_GPUShader* made : {vertex_shader, fragment_shader})
            if (made)
                SDL_ReleaseGPUShader(device, made);
        for (SDL_GPUTexture* made : {canvas, operations, lists, colours, composed})
            if (made)
                SDL_ReleaseGPUTexture(device, made);
        if (upload)
            SDL_ReleaseGPUTransferBuffer(device, upload);
    }
};

/// Whether the card has the formats the overlay takes: its canvas, its
/// tables' words to read, and the composed picture to draw and read.
bool overlay_formats(OverlayResources& overlay, SDL_GPUDevice* device) {
    if (!overlay.formats_known) {
        const auto sampled = [&](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
            return SDL_GPUTextureSupportsFormat(device, format, SDL_GPU_TEXTURETYPE_2D, usage);
        };
        overlay.formats_supported =
            sampled(overlay_canvas_format, SDL_GPU_TEXTUREUSAGE_SAMPLER) &&
            sampled(overlay_operations_format, SDL_GPU_TEXTUREUSAGE_SAMPLER) &&
            sampled(overlay_words_format, SDL_GPU_TEXTUREUSAGE_SAMPLER) &&
            sampled(
                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
            );
        overlay.formats_known = true;
    }
    return overlay.formats_supported;
}

/// Makes a table texture of words afresh, where the one held has fewer rows.
///
/// @return false where it cannot be made
bool ensure_table(
    SDL_GPUDevice* device,
    SDL_GPUTexture*& table,
    uint32_t& rows_held,
    SDL_GPUTextureFormat format,
    uint32_t width,
    uint32_t rows
) {
    rows = std::max<uint32_t>(rows, 1);
    if (table && rows <= rows_held)
        return true;
    if (table)
        SDL_ReleaseGPUTexture(device, table);
    rows_held = 0;
    table = create_texture(device, width, rows, format, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!table)
        return false;
    rows_held = rows;
    return true;
}

/// Lets go of a fence the card has signalled, and of the command buffer it
/// was submitted with.
///
/// SDL's Metal driver lets a submitted command buffer go, with what it holds
/// (the cycled copies of a transfer buffer it read, its uniform buffers),
/// only when a later submission finds the buffer's fence signalled. A fence
/// let go before then goes back to SDL's pool and the next submission takes
/// it again, so the old command buffer looks busy for ever: a picture read
/// back every frame kept a copy of the frame's upload each frame, about
/// 110 MB on the bench's battle. Waiting for the device to be idle, once the
/// fence has signalled, waits for nothing and lets every finished command
/// buffer go.
///
/// @param device the device the fence belongs to
/// @param fence the signalled fence; null once let go
void let_go_of_fence(SDL_GPUDevice* device, SDL_GPUFence*& fence) {
    SDL_ReleaseGPUFence(device, fence);
    fence = nullptr;
    SDL_WaitForGPUIdle(device);
}

/// Downloads a texture of the picture's size into the download buffer and
/// waits for the card to finish everything before it.
///
/// @param[out] output_rgba packed RGBA8 pixels
/// @return false with the reason where a GPU call fails
bool download_texture(
    SDL_GPUDevice* device,
    SDL_GPUTransferBuffer* download,
    SDL_GPUTexture* texture,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& output_rgba,
    std::string& error
) {
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    if (!command) {
        error = std::string("Acquire live GPU readback: ") + SDL_GetError();
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    if (!copy) {
        error = std::string("Begin live GPU readback: ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(command);
        return false;
    }
    const SDL_GPUTextureRegion region{texture, 0, 0, 0, 0, 0, width, height, 1};
    const SDL_GPUTextureTransferInfo destination{download, 0, width, height};
    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (!fence) {
        error = std::string("Submit live GPU readback: ") + SDL_GetError();
        return false;
    }
    if (!SDL_WaitForGPUFences(device, true, &fence, 1)) {
        error = std::string("Complete live GPU readback: ") + SDL_GetError();
        SDL_ReleaseGPUFence(device, fence);
        return false;
    }
    let_go_of_fence(device, fence);
    const auto* mapped =
        static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(device, download, false));
    if (!mapped) {
        error = std::string("Map live GPU readback: ") + SDL_GetError();
        return false;
    }
    output_rgba.assign(mapped, mapped + std::size_t{width} * height * channels);
    SDL_UnmapGPUTransferBuffer(device, download);
    return true;
}

/// Tells whether vertices are ones render() can draw: every value within
/// its range, a known terrain square size, and a terrain square inside the
/// atlas. Ordered bounds also reject NaN and infinities. Each test gives 0
/// or 1, and a vertex's tests are combined with & rather than &&, so that a
/// frame's million vertices are checked without a branch on each test.
///
/// @param vertices the vertices
/// @param texture_width the atlas's width
/// @param texture_height its height
/// @return 1 when every one can be drawn, else 0
uint32_t
vertices_valid(std::span<const Vertex> vertices, uint32_t texture_width, uint32_t texture_height) {
    const auto within = [](float value, float low, float high) -> uint32_t {
        return static_cast<uint32_t>(value >= low) & static_cast<uint32_t>(value <= high);
    };
    const auto unit_range = [&](float value) { return within(value, 0, 1); };
    const auto atlas_width = static_cast<float>(texture_width);
    const auto atlas_height = static_cast<float>(texture_height);
    uint32_t all_valid = 1;
    for (const auto& vertex : vertices) {
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
        all_valid &=
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
    }
    return all_valid;
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
    /// The next kept ground sent ahead (KeptGround::next) into its buffer:
    /// its revision, its vertices, those sent so far, the atlas size its
    /// slices were checked against and whether every one passed; no
    /// revision while none is being sent.
    uint32_t next_ground_capacity{};
    uint64_t next_ground_revision{};
    std::size_t next_ground_count{};
    std::size_t next_ground_sent{};
    uint32_t next_ground_atlas_width{};
    uint32_t next_ground_atlas_height{};
    bool next_ground_valid{};
    /// The lights' resources, released before the device in resources.
    LightResources lights;
    /// The overlay's resources, released before the device in resources.
    OverlayResources overlay;
    /// The picture the last frame that succeeded drew, which it left on the
    /// card or read back; null before one.
    SDL_GPUTexture* picture{};
    /// The models' resources, released before the device in resources.
    ModelResources models;
};

Context::Context() : state(std::make_unique<State>()) {
}

Context::~Context() = default;

namespace {

/// Opens the context's device where it is not yet open: the shared device
/// where there is one, else one of its own.
///
/// @return false with the reason where SDL makes none
bool open_device(Context& context, std::string& error) {
    auto& resources = context.state->resources;
    if (resources.device)
        return true;
    if (context.device_unavailable) {
        error = "Live GPU unavailable on this system";
        return false;
    }
    if (context.shared_device) {
        resources.device = context.shared_device;
        resources.device_shared = true;
        return true;
    }
    // SDL picks the graphics interface: Metal on Apple systems, Vulkan
    // elsewhere, or SDL_GPU_DRIVER's.
    resources.device = SDL_CreateGPUDevice(shader_formats, false, nullptr);
    if (!resources.device) {
        error = std::string("Live GPU unavailable: ") + SDL_GetError();
        context.device_unavailable = true;
        return false;
    }
    return true;
}

} // namespace

SDL_GPUDevice* device(Context& context) {
    std::string error;
    return open_device(context, error) ? context.state->resources.device : nullptr;
}

bool draws_models(const Context& context) noexcept {
    return context.models_allowed && !context.device_unavailable && context.state &&
           context.state->resources.device != nullptr;
}

bool draws_lights(const Context& context) noexcept {
    return context.lights_allowed && context.state && context.state->resources.device != nullptr &&
           context.state->lights.surface_pipeline != nullptr;
}

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
    const Lights* lights,
    const ModelDraws* models
) {
    output_rgba.clear();
    error.clear();
    context.kept_on_card = false;
    context.state->picture = nullptr;
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
    auto& state = *context.state;
    // A next kept ground sent whole, every slice checked against this
    // atlas, becomes the kept ground's buffer in the frame that takes it.
    if (ground_count != 0 && state.ground_revision != ground->revision &&
        state.next_ground_revision == ground->revision && state.next_ground_valid &&
        state.next_ground_count == ground_count && state.next_ground_sent == ground_count &&
        state.next_ground_atlas_width == texture_width &&
        state.next_ground_atlas_height == texture_height) {
        std::swap(state.resources.ground, state.resources.next_ground);
        std::swap(state.ground_capacity, state.next_ground_capacity);
        state.ground_revision = ground->revision;
        state.ground_vertex_count = ground_count;
        state.ground_atlas_width = texture_width;
        state.ground_atlas_height = texture_height;
        state.next_ground_revision = 0;
    }
    const auto& held = state;
    const bool ground_held = ground_count != 0 && held.ground_revision == ground->revision &&
                             held.ground_vertex_count == ground_count &&
                             held.ground_atlas_width == texture_width &&
                             held.ground_atlas_height == texture_height;
    bench_timers::Span validating(bench_timers::Stage::gpu_validate);
    const uint32_t frame_valid = vertices_valid(
        vertices.subspan(ground_held ? ground_count : 0), texture_width, texture_height
    );
    // The share of the next kept ground sent this frame, checked as it goes:
    // in order from its first vertex, or from the first again for a new one.
    // It counts as sent once the frame's upload holds it.
    std::span<const Vertex> next_slice;
    bool next_fresh = false;
    uint32_t next_slice_valid = 1;
    if (ground && ground->next_revision != 0 && ground->next_count != 0 &&
        ground->next_first + ground->next_count <= ground->next.size() &&
        ground->next.size() % vertices_per_triangle == 0) {
        next_fresh = state.next_ground_revision != ground->next_revision ||
                     state.next_ground_atlas_width != texture_width ||
                     state.next_ground_atlas_height != texture_height;
        if (ground->next_first == (next_fresh ? 0 : state.next_ground_sent)) {
            next_slice = ground->next.subspan(ground->next_first, ground->next_count);
            next_slice_valid = vertices_valid(next_slice, texture_width, texture_height);
        }
    }
    // The model draws, where there are any, and only where the card may
    // draw them.
    const bool modelled = models && (!models->runs.empty() || !models->casts.empty());
    if (modelled &&
        (!context.models_allowed || !valid_model_draws(*models, ground_count, vertices.size()))) {
        validating.stop();
        error = "Invalid live GPU model draws";
        return Result::failed;
    }
    validating.stop();
    if (frame_valid == 0) {
        error = "Invalid live GPU vertex coordinates";
        return Result::failed;
    }
    if (!open_device(context, error))
        return Result::unavailable;
    auto& resources = state.resources;
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
        if (!make_model_pipelines(
                state.models, resources.device, pipeline_info, state.lights.surface_shader
            ))
            return fail("Create live GPU model pipelines");
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
    const auto next_bytes = uint32_t(next_slice.size_bytes());
    const auto next_whole_bytes = uint32_t(next_slice.empty() ? 0 : ground->next.size_bytes());
    if (next_fresh && next_whole_bytes > state.next_ground_capacity) {
        if (resources.next_ground)
            SDL_ReleaseGPUBuffer(resources.device, resources.next_ground);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = next_whole_bytes;
        resources.next_ground = SDL_CreateGPUBuffer(resources.device, &info);
        state.next_ground_capacity = resources.next_ground ? next_whole_bytes : 0;
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
    // The next kept ground's share goes last.
    const uint32_t next_offset = vertex_bytes + ground_bytes + atlas_upload_bytes + region_bytes;
    const uint32_t upload_bytes = next_offset + next_bytes;
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
        (next_bytes != 0 && !resources.next_ground) || (upload_bytes != 0 && !resources.upload) ||
        !resources.download || !resources.atlas || !resources.colour || !resources.depth ||
        !resources.resolved || (resolves_twice && !resources.multisample_resolved))
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
    // The picture stays on the card unless its lights are the caller's to
    // add, which takes it back.
    const bool keep = context.keep_on_card && (lights == nullptr || lights_on_card);
    bench_timers::count_atlas_upload(
        upload_rows + frame_upload_rows, upload_rows == texture_height
    );
    bench_timers::Span uploading(bench_timers::Stage::gpu_upload);
    // The model draws, which read the camera's rows from their own buffer.
    auto& models_held = state.models;
    if (modelled) {
        const std::array camera_rows{
            projection[0],
            projection[1],
            projection[2],
            projection[3],
            projection[4],
            projection[5],
            projection[6],
            projection[7],
            projection[8],
            projection[9],
            projection[10],
            projection[11],
            float(output_width),
            float(output_height),
            float(supersample),
            float(samples)
        };
        if (!models_held.pipeline || !prepare_models(models_held, *models, camera_rows))
            return fail("Create live GPU model resources");
    }
    // The upload holds the frame's vertices, then the kept ground when it
    // has changed, then the atlas rows that have, then the frame's own rows,
    // then the rectangles.
    if (upload_bytes != 0) {
        // Cycled, since a frame left on the card may still be reading the
        // upload of the frame before.
        auto* upload = static_cast<uint8_t*>(
            SDL_MapGPUTransferBuffer(resources.device, resources.upload, true)
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
        if (next_bytes != 0)
            std::memcpy(upload + next_offset, next_slice.data(), next_bytes);
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
        let_go_of_fence(resources.device, resources.fence);
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
    if (next_bytes != 0) {
        const SDL_GPUTransferBufferLocation next_source{resources.upload, next_offset};
        const SDL_GPUBufferRegion next_destination{
            resources.next_ground, uint32_t(ground->next_first * sizeof(Vertex)), next_bytes
        };
        SDL_UploadToGPUBuffer(copy, &next_source, &next_destination, false);
        if (next_fresh) {
            state.next_ground_revision = ground->next_revision;
            state.next_ground_count = ground->next.size();
            state.next_ground_sent = 0;
            state.next_ground_atlas_width = texture_width;
            state.next_ground_atlas_height = texture_height;
            state.next_ground_valid = true;
        }
        state.next_ground_valid = state.next_ground_valid && next_slice_valid != 0;
        state.next_ground_sent += next_slice.size();
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
    if (modelled)
        upload_models(models_held, *models, copy);
    SDL_EndGPUCopyPass(copy);
    // The units' shadows cast into their map, which the scene lays on its
    // ground where the shadows lie among its vertices.
    const bool shadowed = modelled && !models->casts.empty();
    if (shadowed && !cast_shadows(models_held, *models, resources.command))
        return fail("Begin live GPU shadows");
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
    if (shadowed) {
        // The map's left and top and its texels a map pixel; the samples a
        // pixel and the frame's texels across and down.
        const auto& map = models->shadow_map;
        const std::array<float, 8> shadow_block{
            map.left,
            map.top,
            1.0F / map.scale,
            0,
            float(samples),
            float(map.width),
            float(map.height),
            0
        };
        SDL_PushGPUFragmentUniformData(
            resources.command, 1, shadow_block.data(), sizeof(shadow_block)
        );
    }
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
    // The ground first and then the rest, in the frame's order, the model
    // draws among them.
    if (ground_count != 0) {
        const SDL_GPUBufferBinding binding{resources.ground, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, uint32_t(ground_count), 1, 0, 0);
    }
    const std::span<const ModelRun> runs = modelled ? models->runs : std::span<const ModelRun>{};
    std::function<void()> lay;
    if (shadowed)
        lay = [&] {
            lay_shadows(
                pass,
                models_held.shadows,
                resources.sampler,
                ground_count != 0 ? resources.ground : resources.vertices,
                ground_count != 0 ? ground_count : models->shadow_ground
            );
        };
    draw_frame_and_models(
        pass,
        resources.pipeline,
        resources.vertices,
        frame_vertices.size(),
        ground_count,
        modelled ? &models_held : nullptr,
        models_held.pipeline,
        runs,
        lay,
        shadowed ? models->shadow_vertex : 0
    );
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
                   frame_vertices.size(),
                   modelled ? &models_held : nullptr,
                   runs
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
    const auto succeed = [&] {
        state.picture = lit ? lights_held.lit : picture;
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
    };
    if (keep) {
        // Submitted with nothing waited for: what presents the picture is
        // submitted after it, and the card draws them in that order.
        const bool submitted = SDL_SubmitGPUCommandBuffer(resources.command);
        resources.command = nullptr;
        if (!submitted)
            return fail("Submit live GPU render");
        waiting.stop();
        if (while_drawing)
            while_drawing();
        context.kept_on_card = true;
        return succeed();
    }
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
    let_go_of_fence(resources.device, resources.fence);
    return succeed();
}

Result compose_overlay(Context& context, const Overlay& overlay, std::string& error) {
    error.clear();
    auto& state = *context.state;
    auto& resources = state.resources;
    auto& held = state.overlay;
    const uint32_t width = state.resolved_width;
    const uint32_t height = state.resolved_height;
    const auto* packed = overlay.operations;
    const uint32_t band_rows = std::max<uint32_t>(overlay.band_rows, 1);
    const uint32_t bands = (height + band_rows - 1) / band_rows;
    if (!resources.device || !state.picture || !context.kept_on_card) {
        error = "No live GPU picture on the card to add the overlay to";
        return Result::failed;
    }
    if (overlay.canvas.size() != std::size_t{width} * height * channels ||
        overlay.opaque_bands.size() < bands || packed == nullptr ||
        packed->tiles_across !=
            (width + renderer_live_overlay::tile_edge - 1) / renderer_live_overlay::tile_edge ||
        packed->tiles_down !=
            (height + renderer_live_overlay::tile_edge - 1) / renderer_live_overlay::tile_edge ||
        packed->operations.size() !=
            std::size_t{packed->count} * renderer_live_overlay::texels_per_operation * 4 ||
        packed->lists.size() <= std::size_t{packed->tiles_across} * packed->tiles_down ||
        (!overlay.palette.empty() &&
         overlay.palette.size() < renderer_live_overlay::palette_entry_bytes)) {
        error = "Invalid live GPU overlay";
        return Result::failed;
    }
    SDL_GPUDevice* const device = resources.device;
    held.device = device;
    if (!overlay_formats(held, device)) {
        error = "The graphics card lacks a format the overlay takes";
        return Result::failed;
    }
    if (!held.pipeline) {
        held.vertex_shader = create_shader(device, true, true);
        SDL_GPUShaderCreateInfo info{};
        stage_code(device, info, live_overlay_fragment_spirv, live_overlay_fragment_msl);
        info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = 5;
        info.num_uniform_buffers = 1;
        held.fragment_shader = SDL_CreateGPUShader(device, &info);
        if (!held.vertex_shader || !held.fragment_shader) {
            error = std::string("Compile live GPU overlay shaders: ") + SDL_GetError();
            return Result::failed;
        }
        SDL_GPUColorTargetDescription target{};
        target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.vertex_shader = held.vertex_shader;
        pipeline_info.fragment_shader = held.fragment_shader;
        pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline_info.target_info.color_target_descriptions = &target;
        pipeline_info.target_info.num_color_targets = 1;
        held.pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_info);
        if (!held.pipeline) {
            error = std::string("Create live GPU overlay pipeline: ") + SDL_GetError();
            return Result::failed;
        }
    }
    if (width != held.width || height != held.height) {
        for (SDL_GPUTexture** made : {&held.canvas, &held.composed}) {
            if (*made)
                SDL_ReleaseGPUTexture(device, *made);
            *made = nullptr;
        }
        held.width = held.height = 0;
        held.held_bands.clear();
        held.canvas = create_texture(
            device, width, height, overlay_canvas_format, SDL_GPU_TEXTUREUSAGE_SAMPLER
        );
        // Drawn here and sampled by whatever presents it.
        held.composed = create_texture(
            device,
            width,
            height,
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
        );
        if (!held.canvas || !held.composed) {
            error = std::string("Create live GPU overlay textures: ") + SDL_GetError();
            return Result::failed;
        }
        held.width = width;
        held.height = height;
    }
    namespace overlay_layout = renderer_live_overlay;
    const uint32_t operation_rows = (packed->count + overlay_layout::operations_per_row - 1) /
                                    overlay_layout::operations_per_row;
    const auto list_words = static_cast<uint32_t>(packed->lists.size());
    const uint32_t list_rows =
        (list_words + overlay_layout::list_words_per_row - 1) / overlay_layout::list_words_per_row;
    uint32_t colour_rows_held = held.colours ? overlay_colour_rows : 0;
    if (!ensure_table(
            device,
            held.operations,
            held.operation_rows,
            overlay_operations_format,
            overlay_layout::operations_per_row * overlay_layout::texels_per_operation,
            operation_rows
        ) ||
        !ensure_table(
            device,
            held.lists,
            held.list_rows,
            overlay_words_format,
            overlay_layout::list_words_per_row,
            list_rows
        ) ||
        !ensure_table(
            device,
            held.colours,
            colour_rows_held,
            overlay_words_format,
            overlay_colour_entries,
            overlay_colour_rows
        )) {
        error = std::string("Create live GPU overlay tables: ") + SDL_GetError();
        return Result::failed;
    }
    // The canvas's bands sent: each that holds an opaque pixel now or held
    // one at the last overlay, so that the texture holds this canvas; every
    // band once after the texture is made.
    const bool whole = held.held_bands.size() != bands;
    const auto sent = [&](uint32_t band) {
        return whole || overlay.opaque_bands[band] != 0 || held.held_bands[band] != 0;
    };
    const uint32_t row_bytes = width * channels;
    uint32_t canvas_bytes = 0;
    for (uint32_t band = 0; band < bands; ++band)
        if (sent(band))
            canvas_bytes +=
                (std::min(height, (band + 1) * band_rows) - band * band_rows) * row_bytes;
    constexpr uint32_t word_bytes = sizeof(uint32_t);
    const uint32_t operation_bytes = operation_rows * overlay_layout::operations_per_row *
                                     overlay_layout::texels_per_operation * 4 * word_bytes;
    const uint32_t list_bytes = list_rows * overlay_layout::list_words_per_row * word_bytes;
    const uint32_t colour_bytes = overlay_colour_entries * overlay_colour_rows * word_bytes;
    const uint32_t upload_bytes = canvas_bytes + operation_bytes + list_bytes + colour_bytes;
    if (upload_bytes > held.upload_capacity) {
        if (held.upload)
            SDL_ReleaseGPUTransferBuffer(device, held.upload);
        held.upload_capacity = 0;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = upload_bytes;
        held.upload = SDL_CreateGPUTransferBuffer(device, &info);
        if (!held.upload) {
            error = std::string("Create live GPU overlay upload: ") + SDL_GetError();
            return Result::failed;
        }
        held.upload_capacity = upload_bytes;
    }
    // Cycled, since the card may still be reading the last overlay's.
    auto* upload = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, held.upload, true));
    if (!upload) {
        error = std::string("Map live GPU overlay upload: ") + SDL_GetError();
        return Result::failed;
    }
    // The canvas's bands, then the operations, the lists, the palette and
    // the gamma; the tables' last rows past their words are left as they are.
    uint32_t offset = 0;
    for (uint32_t band = 0; band < bands; ++band) {
        if (!sent(band))
            continue;
        const uint32_t first = band * band_rows;
        const uint32_t bytes = (std::min(height, first + band_rows) - first) * row_bytes;
        std::memcpy(upload + offset, overlay.canvas.data() + std::size_t{first} * row_bytes, bytes);
        offset += bytes;
    }
    std::memcpy(upload + offset, packed->operations.data(), packed->operations.size() * word_bytes);
    offset += operation_bytes;
    std::memcpy(upload + offset, packed->lists.data(), packed->lists.size() * word_bytes);
    offset += list_bytes;
    std::array<uint32_t, overlay_colour_entries * overlay_colour_rows> colours{};
    const std::size_t entries = std::min<std::size_t>(
        overlay.palette.size() / overlay_layout::palette_entry_bytes, overlay_colour_entries
    );
    for (std::size_t entry = 0; entry < entries; ++entry) {
        const auto* at = overlay.palette.data() + entry * overlay_layout::palette_entry_bytes;
        colours[entry] = uint32_t{at[0]} | uint32_t{at[1]} << 8 | uint32_t{at[2]} << 16;
    }
    for (uint32_t level = 0; level < overlay_colour_entries; ++level)
        colours[overlay_colour_entries + level] = overlay.gamma ? (*overlay.gamma)[level] : level;
    std::memcpy(upload + offset, colours.data(), colour_bytes);
    SDL_UnmapGPUTransferBuffer(device, held.upload);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    if (!command) {
        error = std::string("Acquire live GPU overlay: ") + SDL_GetError();
        return Result::failed;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    if (!copy) {
        error = std::string("Begin live GPU overlay upload: ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(command);
        return Result::failed;
    }
    // A run of neighbouring bands goes up at once.
    offset = 0;
    for (uint32_t band = 0; band < bands;) {
        if (!sent(band)) {
            ++band;
            continue;
        }
        uint32_t end = band + 1;
        while (end < bands && sent(end))
            ++end;
        const uint32_t first = band * band_rows;
        const uint32_t rows = std::min(height, end * band_rows) - first;
        const SDL_GPUTextureTransferInfo source{held.upload, offset, width, rows};
        const SDL_GPUTextureRegion region{held.canvas, 0, 0, 0, first, 0, width, rows, 1};
        SDL_UploadToGPUTexture(copy, &source, &region, false);
        offset += rows * row_bytes;
        band = end;
    }
    const auto upload_table =
        [&](SDL_GPUTexture* table, uint32_t texels, uint32_t rows, uint32_t bytes) {
            if (rows != 0) {
                const SDL_GPUTextureTransferInfo source{held.upload, offset, texels, rows};
                const SDL_GPUTextureRegion region{table, 0, 0, 0, 0, 0, texels, rows, 1};
                SDL_UploadToGPUTexture(copy, &source, &region, false);
            }
            offset += bytes;
        };
    upload_table(
        held.operations,
        overlay_layout::operations_per_row * overlay_layout::texels_per_operation,
        operation_rows,
        operation_bytes
    );
    upload_table(held.lists, overlay_layout::list_words_per_row, list_rows, list_bytes);
    upload_table(held.colours, overlay_colour_entries, overlay_colour_rows, colour_bytes);
    SDL_EndGPUCopyPass(copy);
    namespace text = oa::present;
    const std::array<uint32_t, 16> parameters{
        packed->tiles_across,
        overlay_layout::tile_edge,
        static_cast<uint32_t>(entries),
        overlay_layout::operations_per_row,
        text::text_shade_color[0],
        text::text_shade_color[1],
        text::text_shade_color[2],
        text::text_shadow_alpha,
        text::text_outline_color[0],
        text::text_outline_color[1],
        text::text_outline_color[2],
        0,
        text::text_dark_outline_color[0],
        text::text_dark_outline_color[1],
        text::text_dark_outline_color[2],
        0
    };
    SDL_PushGPUFragmentUniformData(command, 0, parameters.data(), sizeof(parameters));
    SDL_GPUColorTargetInfo target{};
    target.texture = held.composed;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target, 1, nullptr);
    if (!pass) {
        error = std::string("Begin live GPU overlay: ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(command);
        return Result::failed;
    }
    SDL_BindGPUGraphicsPipeline(pass, held.pipeline);
    const std::array<SDL_GPUTextureSamplerBinding, 5> sources{
        {{state.picture, resources.sampler},
         {held.canvas, resources.sampler},
         {held.operations, resources.sampler},
         {held.lists, resources.sampler},
         {held.colours, resources.sampler}}
    };
    SDL_BindGPUFragmentSamplers(pass, 0, sources.data(), static_cast<Uint32>(sources.size()));
    SDL_DrawGPUPrimitives(pass, vertices_per_triangle, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(command)) {
        error = std::string("Submit live GPU overlay: ") + SDL_GetError();
        return Result::failed;
    }
    held.held_bands.assign(overlay.opaque_bands.begin(), overlay.opaque_bands.begin() + bands);
    return Result::success;
}

SDL_GPUTexture* composed(const Context& context) {
    const auto& held = context.state->overlay;
    return held.width != 0 ? held.composed : nullptr;
}

Result read_picture(Context& context, std::vector<uint8_t>& output_rgba, std::string& error) {
    output_rgba.clear();
    error.clear();
    auto& state = *context.state;
    if (!state.resources.device || !state.picture || !state.resources.download) {
        error = "No live GPU picture to read back";
        return Result::failed;
    }
    return download_texture(
               state.resources.device,
               state.resources.download,
               state.picture,
               state.resolved_width,
               state.resolved_height,
               output_rgba,
               error
           )
               ? Result::success
               : Result::failed;
}

Result read_composed(Context& context, std::vector<uint8_t>& output_rgba, std::string& error) {
    output_rgba.clear();
    error.clear();
    auto& state = *context.state;
    const auto& held = state.overlay;
    if (!state.resources.device || !held.composed || held.width != state.resolved_width ||
        held.height != state.resolved_height || !state.resources.download) {
        error = "No live GPU overlay to read back";
        return Result::failed;
    }
    return download_texture(
               state.resources.device,
               state.resources.download,
               held.composed,
               held.width,
               held.height,
               output_rgba,
               error
           )
               ? Result::success
               : Result::failed;
}

} // namespace oa::app::renderer_live_gpu
