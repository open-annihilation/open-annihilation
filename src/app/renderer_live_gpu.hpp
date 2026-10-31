// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "renderer_proof_gpu.hpp"
#include "renderer_water.hpp"

#include <array>
#include <cstddef>
#include <functional>
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
        terrain_size{}; ///< -7 filtered map feature, -6 nearest map feature, -5 submerged effect, -4 splash, -3 reflection, -2 water, -1 filtered, 0 nearest, 32, 16, 8 or 4 terrain at that many pixels a tile
    float terrain_known{1};          ///< terrain visibility in [0,1]
    float terrain_grey{};            ///< blend toward the adjacent grey mip chain in [0,1]
    float red{1}, green{1}, blue{1}; ///< material RGB modulation in [0,1]
};

using Result = renderer_proof_gpu::Result;

/// The widest and tallest picture render() draws, and atlas it takes, in
/// pixels.
inline constexpr uint32_t largest_edge = 4096;

/// The most vertices render() draws in a frame.
inline constexpr std::size_t largest_vertex_count = 3 * 1024 * 1024;

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
    /// The samples a pixel of the last frame that succeeded, before its
    /// supersampling: 4 where the card's multisampling took a 2x2 of them,
    /// else 1; zero before success.
    uint32_t actual_multisample{};
    /// Whether render() may take a 2x2 of the samples it is asked for as
    /// the card's 4x multisampling; false supersamples them all, as on a
    /// card without it.
    bool multisampling_allowed{true};
    /// SDL made no graphics device the renderer can draw with: none on
    /// Metal or Vulkan. render() answers unavailable from then on without
    /// trying again.
    bool device_unavailable{};
    /// Allocate the state without opening a device or window.
    Context();
    /// Release pending commands and GPU resources before the device.
    ~Context();
};

/// A rectangle of the atlas, in texels.
struct AtlasRegion {
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
};

/// Describe changed atlas rows since a known revision; end_row is exclusive.
/// An empty range preserves all texels. A missing or stale base revision uploads the full atlas.
struct AtlasUpdate {
    uint64_t previous_revision{};
    uint32_t first_row{};
    uint32_t end_row{};
    /// A second range of changed rows, uploaded apart from the first so that
    /// the rows between them are not: the rows the frame's own materials
    /// take, at the atlas's foot. Empty when the two are equal.
    uint32_t frame_first_row{};
    uint32_t frame_end_row{};
    /// Changed rectangles beside those rows, each uploaded alone rather than
    /// with the whole of every row it crosses: materials packed into room
    /// given back anywhere in the atlas.
    std::vector<AtlasRegion> regions{};
};

/// The ground at the start of a frame's vertices that a scene keeps from
/// frame to frame. render() keeps it in a buffer of its own on the card,
/// checked and uploaded only when its revision changes.
struct KeptGround {
    std::size_t vertex_count{}; ///< the frame's first vertices that are the ground
    /// Changes whenever those vertices may have changed, and is never the
    /// same for two different grounds in any scene; zero for ground that is
    /// not kept, checked and uploaded with the frame's other vertices.
    uint64_t revision{};
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
///        enlarged target fits 4096 pixels per axis. Above 1, where the card multisamples
///        the formats 4 times (and Context::multisampling_allowed), a 2x2 of it is the
///        card's multisampling, which needs no enlarged target: the factor drawn is then
///        half that, at least 1, while the samples drawn fit 4096 by 4096. Multisampled
///        pixels are averaged by the card and an enlarged target GPU box-filtered before
///        readback
/// @param atlas_update optional changed rows and rectangles; ignored after creation, resizing
///        or a revision gap
/// @param water optional water animation, sea level and distance attenuation
/// @param ground optional kept ground at the start of vertices, a whole number of
///        triangles; vertices must hold the same ground while its revision is unchanged
/// @param while_drawing optional work for the caller while the card draws: called once,
///        when the frame has been submitted and before render() waits for it
/// @return success; unavailable where SDL makes no device on Metal or Vulkan; or failed for
///         invalid input or GPU errors
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
    const WaterEffects* water = nullptr,
    const KeptGround* ground = nullptr,
    const std::function<void()>& while_drawing = {}
);

} // namespace oa::app::renderer_live_gpu
