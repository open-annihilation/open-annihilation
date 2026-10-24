// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "renderer_proof_gpu.hpp"
#include "renderer_water.hpp"

#include <array>
#include <memory>

namespace oa::app::renderer_live_gpu {

/// Hold a world-space vertex, atlas coordinates and interpolated attenuation and emission.
struct Vertex {
    float x{};
    float height{};
    float z{};
    float u{};
    float v{};
    float brightness{1}; ///< RGB multiplier in [0,1]
    float opacity{1};    ///< alpha multiplier in [0,1]
    float emission_r{};  ///< additive red channel in [0,1]
    float emission_g{};  ///< additive green channel in [0,1]
    float emission_b{};  ///< additive blue channel in [0,1]
    float terrain_x{};   ///< terrain atlas origin, or water depth for water/reflections
    float terrain_y{};   ///< terrain mip-chain origin in atlas pixels
    float
        terrain_size{}; ///< -5 submerged effect, -4 splash, -3 reflection, -2 water, -1 filtered, 0 nearest, 32 terrain
    float terrain_known{1};          ///< terrain visibility in [0,1]
    float terrain_grey{};            ///< blend toward the adjacent grey mip chain in [0,1]
    float red{1}, green{1}, blue{1}; ///< material RGB modulation in [0,1]
};

using Result = renderer_proof_gpu::Result;

/// Shared water animation controls; zero intensities leave source artwork unchanged.
struct WaterEffects {
    float ripples{};
    float coastal_waves{};
    float shimmer{};
    float reflections{};
    float sea_level{};
    float seconds{};
    float detail{1};
    renderer_water::Wind wind{}; ///< stable map flow and integrated surface travel
    float fine_detail{};         ///< close-range ripple layer, smoothly faded at distance
};

/// Own the persistent device, pipeline and frame resources; destroy before SDL quits.
struct Context {
    struct State;
    std::unique_ptr<State> state{};
    uint32_t actual_supersample{}; ///< successful last frame axis factor; zero before success
    /// Allocate the state without opening a device or window.
    Context();
    /// Release pending commands and GPU resources before the device.
    ~Context();
};

/// Describe changed atlas rows since a known revision; end_row is exclusive.
/// An empty range preserves all texels. A missing or stale base revision uploads the full atlas.
struct AtlasUpdate {
    uint64_t previous_revision{};
    uint32_t first_row{};
    uint32_t end_row{};
};

/// Render world triangles through a pixel-space affine projection without a window.
/// Reuse shaders and buffers across frames, uploading the atlas only when its revision
/// or dimensions change. Readback waits for completion before returning RGBA8 pixels.
/// Failure clears output_rgba and reports its cause; the caller chooses a fallback.
/// @param[in,out] context persistent GPU resources; SDL video must be initialized
/// @param vertices nonempty triangle list within +/-32768 world units; UVs, brightness
///        opacity and emission channels in [0,1]
/// @param texture_rgba packed source RGBA8 pixels; filtered terrain owns a 64x64
///        rectangle: colour and grey 32x64 chains side by side. Each chain stacks
///        square mip levels 32,16,8,4,2,1 at row offsets 0,32,48,56,60,62.
///        Terrain UVs span the base colour tile edges; sprites keep nearest sampling.
/// @param texture_width atlas width in pixels, from 1 through 4096
/// @param texture_height atlas height in pixels, from 1 through 4096
/// @param atlas_revision revision changed whenever atlas pixels change
/// @param output_width target width in pixels, from 1 through 4096
/// @param output_height target height in pixels, from 1 through 4096
/// @param projection three affine rows for pixel x, pixel y and depth; world order
///        x,height,z,1; larger depth is nearer, with clipping at +/-65536
/// @param[out] output_rgba packed target pixels; background is opaque black
/// @param[out] error failure or unavailable reason
/// @param requested_supersample axis sample factor 1,2,4,8 or16; halved until the
///        enlarged target fits 4096 pixels per axis, then GPU box-filtered before readback
/// @param atlas_update optional changed rows; ignored after creation, resizing or a revision gap
/// @param water optional water animation, sea level and distance attenuation
/// @return success, unavailable without Metal, or failed for invalid input or GPU errors
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
    uint32_t requested_supersample = 1,
    const AtlasUpdate* atlas_update = nullptr,
    const WaterEffects* water = nullptr
);

} // namespace oa::app::renderer_live_gpu
