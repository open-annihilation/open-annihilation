// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "renderer_effects.hpp"
#include "renderer_live_overlay.hpp"
#include "renderer_proof_gpu.hpp"
#include "renderer_water.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct SDL_GPUDevice;
struct SDL_GPUTexture;

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

/// The lights and heat render() adds to its picture on the card, as
/// renderer_effects::compose() adds them to a picture on the processor: in
/// its order, at its strengths and switches, each light reaching the
/// surfaces the frame itself draws in its reach, which the card finds from
/// the depth, opacity and fog of each pixel's surface as it draws it.
struct Lights {
    std::span<const renderer_effects::Source> sources{}; ///< in output pixels
    renderer_effects::Settings settings{};
    double simulation_seconds{};
};

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
    /// Whether render() may add the lights it is given on the card; false
    /// leaves them to the caller, as on a card without the formats they need.
    bool lights_allowed{true};
    /// Whether the last frame that succeeded holds the lights it was given,
    /// added on the card; false when it was given none, or left them to the
    /// caller, which adds them to the picture itself.
    bool lights_drawn{};
    /// A graphics device to draw on in place of one of the context's own,
    /// such as the one the game's window presents through; the context
    /// never destroys it. Set before the first frame; null opens one.
    SDL_GPUDevice* shared_device{};
    /// Whether render() leaves its picture on the card, for compose_overlay()
    /// and whatever presents it, in place of reading it back and waiting for
    /// the card to draw it. A frame whose lights the card cannot add is read
    /// back all the same, for the caller to add them.
    bool keep_on_card{};
    /// Whether the last frame that succeeded left its picture on the card,
    /// with nothing read back.
    bool kept_on_card{};
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
/// @param lights optional lights added to the picture before readback where the card has
///        the formats they need and Context::lights_allowed; Context::lights_drawn says
///        whether they were
/// @return success; unavailable where SDL makes no device on Metal or Vulkan; or failed for
///         invalid input or GPU errors. With Context::keep_on_card, and lights the card adds
///         or none, the picture stays on the card: output_rgba is left empty, render()
///         returns once the frame is submitted and while_drawing has run, and
///         Context::kept_on_card says so
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
    const std::function<void()>& while_drawing = {},
    const Lights* lights = nullptr
);

/// Opens the graphics device render() draws on, where it is not yet open:
/// Context::shared_device where it is set.
///
/// @param[in,out] context the persistent GPU resources
/// @return the device; null where SDL makes none (Context::device_unavailable)
SDL_GPUDevice* device(Context& context);

/// What the processor painted over the turned view, for compose_overlay().
struct Overlay {
    /// The overlay canvas as ARGB8888 words, as convert_rgb24_keyed_overlay_argb
    /// writes it: 0 where the canvas holds its key, else its colour, opaque,
    /// before the display gamma. The picture's size, `width` * 4 bytes a row.
    std::span<const uint8_t> canvas{};
    /// For each band of band_rows rows, 1 where the canvas holds an opaque
    /// pixel; a band clear now and at the last overlay is not sent again.
    std::span<const uint8_t> opaque_bands{};
    uint32_t band_rows{1};
    /// The operations on the picture under the canvas, laid out for the card.
    const renderer_live_overlay::Packed* operations{};
    /// The palette lines of text are reduced to,
    /// renderer_live_overlay::palette_entry_bytes an entry; empty for none.
    std::span<const uint8_t> palette{};
    /// The display gamma's table; null for a gamma of 1.
    const std::array<uint8_t, 256>* gamma{};
};

/// Adds what the processor painted over the turned view to the picture the
/// last frame left on the card, as renderer_live_overlay::compose adds it on
/// the processor, then the display gamma, into the composed texture
/// (composed()), on the card: submitted with nothing waited for, so that
/// whatever presents the texture next draws it after.
///
/// @param[in,out] context the persistent GPU resources, whose last frame left
///        its picture on the card (Context::kept_on_card)
/// @param overlay what was painted over it
/// @param[out] error the failure's reason
/// @return success; failed where no picture is on the card, the card lacks a
///         format the overlay takes, or a GPU call fails
Result compose_overlay(Context& context, const Overlay& overlay, std::string& error);

/// Returns the texture compose_overlay() drew: RGBA8, the picture's size,
/// opaque, sampled; null before it drew one. The context keeps it, made
/// again only when the picture's size changes.
///
/// @param context the persistent GPU resources
/// @return the texture, owned by the context
[[nodiscard]] SDL_GPUTexture* composed(const Context& context);

/// Reads back the picture the last frame left on the card, as render() reads
/// it back otherwise, waiting for the card to finish it.
///
/// @param[in,out] context the persistent GPU resources
/// @param[out] output_rgba packed RGBA8 pixels, the picture's size; empty on failure
/// @param[out] error the failure's reason
/// @return success; failed where no picture is on the card or a GPU call fails
Result read_picture(Context& context, std::vector<uint8_t>& output_rgba, std::string& error);

/// Reads back the texture compose_overlay() last drew, waiting for the card.
///
/// @param[in,out] context the persistent GPU resources
/// @param[out] output_rgba packed RGBA8 pixels; empty on failure
/// @param[out] error the failure's reason
/// @return success; failed before compose_overlay() drew one or where a GPU call fails
Result read_composed(Context& context, std::vector<uint8_t>& output_rgba, std::string& error);

} // namespace oa::app::renderer_live_gpu
