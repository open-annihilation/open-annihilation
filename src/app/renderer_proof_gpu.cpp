// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_proof_gpu.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstddef>
#include <cstring>

namespace oa::app::renderer_proof_gpu {
namespace {

constexpr uint32_t channels = 4;
constexpr uint32_t max_dimension = 4096;
constexpr size_t max_vertices = 3 * 1024 * 1024;
constexpr float coordinate_limit = 32768.0f;
constexpr uint32_t vertices_per_triangle = 3;

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
};
struct Output {
    float4 position [[position]];
    float2 uv;
};
vertex Output vertex_main(Input input [[stage_in]], constant float4& camera [[buffer(0)]]) {
    const float height_projection = 0.5;
    const float depth_limit = 65536.0;
    float2 pixel = float2(input.world.x, input.world.z - input.world.y * height_projection);
    pixel -= camera.xy;
    Output output;
    output.position = float4(pixel.x * 2.0 / camera.z - 1.0,
        1.0 - pixel.y * 2.0 / camera.w,
        (depth_limit - (input.world.y + input.world.z * height_projection)) /
            (2.0 * depth_limit), 1.0);
    output.uv = input.uv;
    return output;
}
fragment float4 fragment_main(Output input [[stage_in]],
    texture2d<float> atlas [[texture(0)]], sampler nearest [[sampler(0)]]) {
    float4 colour = atlas.sample(nearest, input.uv);
    if (colour.a == 0.0) discard_fragment();
    return colour;
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
SDL_GPUShader* create_shader(SDL_GPUDevice* device, bool vertex) {
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(shader_source);
    info.code_size = sizeof(shader_source);
    info.entrypoint = vertex ? "vertex_main" : "fragment_main";
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_uniform_buffers = vertex ? 1 : 0;
    info.num_samplers = vertex ? 0 : 1;
    return SDL_CreateGPUShader(device, &info);
}
#endif

} // namespace

Result render(
    std::span<const Vertex> vertices,
    std::span<const uint8_t> texture_rgba,
    uint32_t texture_width,
    uint32_t texture_height,
    uint32_t output_width,
    uint32_t output_height,
    float camera_x,
    float camera_y,
    std::vector<uint8_t>& output_rgba,
    std::string& error
) {
    output_rgba.clear();
    error.clear();
    if (!valid_dimensions(texture_width, texture_height) ||
        !valid_dimensions(output_width, output_height) || vertices.empty() ||
        vertices.size() > max_vertices || vertices.size() % vertices_per_triangle != 0 ||
        texture_rgba.size() != size_t(texture_width) * texture_height * channels ||
        !std::isfinite(camera_x) || !std::isfinite(camera_y) ||
        std::abs(camera_x) > coordinate_limit || std::abs(camera_y) > coordinate_limit) {
        error = "Invalid GPU proof dimensions, triangle count, texture bytes or camera";
        return Result::failed;
    }
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.height) || !std::isfinite(vertex.z) ||
            !std::isfinite(vertex.u) || !std::isfinite(vertex.v) ||
            std::abs(vertex.x) > coordinate_limit || std::abs(vertex.height) > coordinate_limit ||
            std::abs(vertex.z) > coordinate_limit || vertex.u < 0 || vertex.u > 1 || vertex.v < 0 ||
            vertex.v > 1) {
            error = "Invalid GPU proof vertex coordinates";
            return Result::failed;
        }
    }
#if !defined(__APPLE__)
    error = "GPU proof shaders currently require Metal";
    return Result::unavailable;
#else
    Resources resources;
    resources.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, "metal");
    if (!resources.device) {
        error = std::string("Metal GPU unavailable: ") + SDL_GetError();
        return Result::unavailable;
    }
    /// Attach SDL's diagnostic to the operation that failed.
    const auto fail = [&](const char* operation) {
        error = std::string(operation) + ": " + SDL_GetError();
        return Result::failed;
    };
    resources.vertex_shader = create_shader(resources.device, true);
    resources.fragment_shader = create_shader(resources.device, false);
    if (!resources.vertex_shader || !resources.fragment_shader)
        return fail("Compile GPU proof shaders");
    resources.atlas = create_texture(
        resources.device,
        texture_width,
        texture_height,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_SAMPLER
    );
    resources.colour = create_texture(
        resources.device,
        output_width,
        output_height,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
    );
    resources.depth = create_texture(
        resources.device,
        output_width,
        output_height,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
    );
    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    resources.sampler = SDL_CreateGPUSampler(resources.device, &sampler_info);
    if (!resources.atlas || !resources.colour || !resources.depth || !resources.sampler)
        return fail("Create GPU proof textures and sampler");

    SDL_GPUVertexBufferDescription vertex_description{};
    vertex_description.pitch = sizeof(Vertex);
    SDL_GPUVertexAttribute attributes[2]{};
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[1].location = 1;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[1].offset = offsetof(Vertex, u);
    SDL_GPUColorTargetDescription colour_description{};
    colour_description.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.vertex_shader = resources.vertex_shader;
    pipeline_info.fragment_shader = resources.fragment_shader;
    pipeline_info.vertex_input_state = {&vertex_description, 1, attributes, 2};
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
        return fail("Create GPU proof pipeline");

    const auto vertex_bytes = uint32_t(vertices.size_bytes());
    const auto texture_bytes = uint32_t(texture_rgba.size());
    const uint32_t output_bytes = output_width * output_height * channels;
    SDL_GPUBufferCreateInfo buffer_info{};
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer_info.size = vertex_bytes;
    resources.vertices = SDL_CreateGPUBuffer(resources.device, &buffer_info);
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = vertex_bytes + texture_bytes;
    resources.upload = SDL_CreateGPUTransferBuffer(resources.device, &transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = output_bytes;
    resources.download = SDL_CreateGPUTransferBuffer(resources.device, &transfer_info);
    if (!resources.vertices || !resources.upload || !resources.download)
        return fail("Create GPU proof buffers");
    auto* upload =
        static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(resources.device, resources.upload, false));
    if (!upload)
        return fail("Map GPU proof upload");
    std::memcpy(upload, vertices.data(), vertex_bytes);
    std::memcpy(upload + vertex_bytes, texture_rgba.data(), texture_bytes);
    SDL_UnmapGPUTransferBuffer(resources.device, resources.upload);
    resources.command = SDL_AcquireGPUCommandBuffer(resources.device);
    if (!resources.command)
        return fail("Acquire GPU proof command buffer");
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(resources.command);
    if (!copy)
        return fail("Begin GPU proof upload");
    const SDL_GPUTransferBufferLocation vertex_source{resources.upload, 0};
    const SDL_GPUBufferRegion vertex_destination{resources.vertices, 0, vertex_bytes};
    SDL_UploadToGPUBuffer(copy, &vertex_source, &vertex_destination, false);
    const SDL_GPUTextureTransferInfo texture_source{
        resources.upload, vertex_bytes, texture_width, texture_height
    };
    const SDL_GPUTextureRegion atlas_region{
        resources.atlas, 0, 0, 0, 0, 0, texture_width, texture_height, 1
    };
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
    const float camera[]{camera_x, camera_y, float(output_width), float(output_height)};
    SDL_PushGPUVertexUniformData(resources.command, 0, camera, sizeof(camera));
    SDL_GPURenderPass* pass =
        SDL_BeginGPURenderPass(resources.command, &colour_target, 1, &depth_target);
    if (!pass)
        return fail("Begin GPU proof render pass");
    SDL_BindGPUGraphicsPipeline(pass, resources.pipeline);
    const SDL_GPUBufferBinding binding{resources.vertices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    const SDL_GPUTextureSamplerBinding sampler_binding{resources.atlas, resources.sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sampler_binding, 1);
    SDL_DrawGPUPrimitives(pass, uint32_t(vertices.size()), 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    copy = SDL_BeginGPUCopyPass(resources.command);
    if (!copy)
        return fail("Begin GPU proof download");
    const SDL_GPUTextureRegion colour_region{
        resources.colour, 0, 0, 0, 0, 0, output_width, output_height, 1
    };
    const SDL_GPUTextureTransferInfo download_destination{
        resources.download, 0, output_width, output_height
    };
    SDL_DownloadFromGPUTexture(copy, &colour_region, &download_destination);
    SDL_EndGPUCopyPass(copy);
    resources.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(resources.command);
    resources.command = nullptr;
    if (!resources.fence || !SDL_WaitForGPUFences(resources.device, true, &resources.fence, 1))
        return fail("Complete GPU proof render");
    const auto* download = static_cast<const uint8_t*>(
        SDL_MapGPUTransferBuffer(resources.device, resources.download, false)
    );
    if (!download)
        return fail("Map GPU proof download");
    output_rgba.assign(download, download + output_bytes);
    SDL_UnmapGPUTransferBuffer(resources.device, resources.download);
    return Result::success;
#endif
}

} // namespace oa::app::renderer_proof_gpu
