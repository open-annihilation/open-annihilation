// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's presentation: the graphics card draws the battlefield
// from the card's command list, built once per presented frame from the
// last list the planner made. The terrain comes from the map's terrain
// atlas, built and uploaded as the match loads as texture pages with their
// levels, by the level rule of full_terrain.hpp; the fog's greyed pass goes
// over it from the greyed atlas, built with a palette of the gray table's
// entries, or its dithered form; the model stage's shadows, then the list's
// sprites, particles, lines, units, projectiles and debris in the planner's
// order from the sprite pages and the models' meshes (runtime_full.hpp);
// the fog's black pass over never-mapped ground; and the quads the painters
// asked for in place of shading the world themselves. Below zoom 1 the
// card draws all of it into the zoomed-out target, at twice the display's
// density on the screen pixels laid from the map's corner, moves that by
// the rest of a screen pixel at its own texels and reduces it onto the
// battlefield. With Enhanced anti-aliasing on, from zoom 1 up the card
// draws the terrain, the fog's greyed pass and the stages into a world
// target at the row's supersample factor (full_supersampling.hpp), within
// the texture limit and the memory guard, and reduces it to the window by
// exact halvings; the processor's anti-aliasing never runs. The processor
// paints the interface and, onto an overlay canvas cleared to a key colour
// outside the palette, everything the painters after the fog paint; the
// overlay is laid over the card's picture 1:1. The planner, the HUD, the
// painters and the readers that keep a picture run as in the Basic tier.
// It is reached only when the tier decided for the frame is Full.
#include "oa/app/runtime.hpp"

#include "full_fog.hpp"
#include "full_presentation.hpp"
#include "graphics_report.hpp"
#include "match_models.hpp"
#include "render_host.hpp"
#include "render_run.hpp"
#include "runtime_full.hpp"
#include "xrgb_conversion.hpp"

#include "oa/app/renderer_records.hpp"

#include "oa/base/float_precision.hpp"
#include "oa/platform/machine.hpp"
#include "oa/present/surface.hpp"
#include "oa/present/world_renderer/world_fog.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace gw = oa::present::gpu_world;
namespace ft = full_terrain;
namespace policy = render_policy;
namespace wr = oa::present::world_renderer;

/// What --render-fault card makes the card's frame fail with.
constexpr std::string_view forced_card_failure =
    "the card refused the frame: forced by --render-fault card";
/// Bytes a texel of a page or a target holds.
constexpr uint64_t bytes_per_texel = card::texel_bytes;

/// The edge of the page the Full function test draws, and of the target,
/// which holds the test's four batches: the page's level 0 at the top
/// left, the blended quad right of it, the page's level 1 enlarged below
/// it and the triangle over the rest.
constexpr uint32_t test_page_edge = 8;
constexpr uint32_t test_target_edge = 64;
/// Most a channel of the function test's blended quad may differ from the
/// blend computed exactly, and a channel of its level 1 drawn LINEAR from
/// the enlargement computed exactly: the renderer rounds its own way.
constexpr int test_blend_most_difference = 2;
/// The colour the function test clears its target to, and the alpha of the
/// quad it blends over it.
constexpr card::Colour test_clear_colour{0.2F, 0.4F, 0.6F, 1.0F};
constexpr float test_blend_alpha = 0.5F;
/// The corners of the function test's triangle, on whole pixels past the
/// page's and the blended quad's squares, with red at the first, green at
/// the second and blue at the third. The pixel at (test_centroid_x,
/// test_centroid_y) holds their centroid and is to show their mean, a
/// third of full in every channel, within test_centroid_most_difference:
/// the pixel's centre lies a sixth of a pixel from the centroid, so the
/// triangle's legs are long enough, 56 pixels, that the colours change by
/// under 2 levels over that, and a renderer that keeps whole levels, as
/// SDL's software renderer does, truncates one more at most.
constexpr std::array<std::array<float, 2>, 3> test_triangle{
    {{8.0F, 8.0F}, {64.0F, 8.0F}, {8.0F, 64.0F}}
};
constexpr uint32_t test_centroid_x = 26;
constexpr uint32_t test_centroid_y = 26;
constexpr int test_centroid_mean = 85;
constexpr int test_centroid_most_difference = 4;
/// Levels of the test page: level 0 and its exact box, level 1.
constexpr uint8_t test_page_levels = 2;
/// Channels of a texel the read-back compares: red, green and blue.
constexpr std::size_t compared_channels = 3;
/// Entries of the display gamma's table, of the gray table and of a palette.
constexpr std::size_t table_entries = 256;
/// Bytes one entry of the game's palette bytes takes.
constexpr std::size_t palette_entry_stride = palette_entry_bytes;
/// Indices a quad adds to a frame.
constexpr uint32_t quad_indices = 6;

/// Returns the nanoseconds since a moment of the steady clock.
///
/// @param since the moment
/// @return nanoseconds
[[nodiscard]] uint64_t nanoseconds_since(std::chrono::steady_clock::time_point since) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - since
    )
                                     .count());
}

/// Rounds a count up to a multiple of the target's grain.
///
/// @param pixels the count
/// @return the multiple
[[nodiscard]] uint32_t target_extent(uint32_t pixels) noexcept {
    return (pixels + full_target_grain - 1U) / full_target_grain * full_target_grain;
}

/// Appends an untextured triangle, each corner in its own colour.
///
/// @param[in,out] frame the frame
/// @param corners the corners, in pixels of the target
/// @param colours the corners' colours
void append_triangle(
    card::CardFrame& frame,
    const std::array<std::array<float, 2>, 3>& corners,
    const std::array<card::Colour, 3>& colours
) {
    const auto first = static_cast<card::Index>(frame.vertices.size());
    for (std::size_t corner = 0; corner < corners.size(); ++corner)
        frame.vertices.push_back(
            {corners[corner][0], corners[corner][1], colours[corner], 0.0F, 0.0F}
        );
    frame.indices.push_back(first);
    frame.indices.push_back(first + 1);
    frame.indices.push_back(first + 2);
}

/// Returns a channel of a small picture enlarged twice as the card's LINEAR
/// sampling enlarges it: the pixel's centre lands a quarter of a texel
/// before the texel's centre or after it, and the two texels around it are
/// weighted by that quarter, clamped at the edge, rounded to the nearest.
///
/// @param texels the picture, texel_bytes a texel, row by row
/// @param edge the picture's texels a side
/// @param x the pixel's column in the enlargement, below 2 * edge
/// @param y the pixel's row in the enlargement
/// @param channel the channel
/// @return the level
[[nodiscard]] int enlarged_twice_linear(
    const uint8_t* texels, uint32_t edge, uint32_t x, uint32_t y, std::size_t channel
) {
    const auto last = static_cast<double>(edge - 1U);
    const auto axis = [&](uint32_t pixel, uint32_t& first, uint32_t& second, double& toward) {
        const double sample = std::clamp(static_cast<double>(pixel) / 2.0 - 0.25, 0.0, last);
        first = static_cast<uint32_t>(std::floor(sample));
        second = std::min(first + 1U, edge - 1U);
        toward = sample - static_cast<double>(first);
    };
    uint32_t x0 = 0;
    uint32_t x1 = 0;
    uint32_t y0 = 0;
    uint32_t y1 = 0;
    double fx = 0.0;
    double fy = 0.0;
    axis(x, x0, x1, fx);
    axis(y, y0, y1, fy);
    const auto at = [&](uint32_t tx, uint32_t ty) {
        return static_cast<double>(texels[(ty * edge + tx) * card::texel_bytes + channel]);
    };
    const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const double bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return static_cast<int>(std::lround(top + (bottom - top) * fy));
}

/// Runs the Full function test on an executor, four batches into one
/// target read back once: (i) an opaque page of two levels drawn 1:1
/// NEAREST reads back equal to the page; (ii) the page's level 1 drawn
/// twice its size LINEAR reads back within test_blend_most_difference of
/// the enlargement computed exactly, or, on SDL's software renderer, which
/// may sample geometry NEAREST, exactly as NEAREST; (iii) a white quad at
/// alpha one half blended over a known colour reads back within
/// test_blend_most_difference of the blend; and (iv) a triangle with red,
/// green and blue corners shows their mean at its centroid pixel within
/// test_centroid_most_difference. Its page and target are destroyed and the
/// render target set back to the window when it ends.
///
/// @param[in,out] executor the executor, open
/// @param renderer the renderer the executor is open on
/// @param software_renderer the renderer is SDL's software renderer
/// @return what failed; empty when it passed
std::string
run_full_function_test(card::Executor& executor, SDL_Renderer* renderer, bool software_renderer) {
    card::PageDescription description;
    description.width = test_page_edge;
    description.height = test_page_edge;
    description.level_count = test_page_levels;
    const card::PageHandle page = executor.create_page(description);
    if (page == card::PageHandle{})
        return "the test page: " + executor.error();
    const card::TargetHandle target = executor.create_target(test_target_edge, test_target_edge, 1);
    if (target == card::TargetHandle{}) {
        executor.destroy_page(page);
        return "the test target: " + executor.error();
    }
    const auto finish = [&](std::string failure) {
        executor.destroy_target(target);
        executor.destroy_page(page);
        return failure;
    };
    // Level 0: a pattern of channels that differ in every texel; level 1 its
    // exact box.
    std::array<uint8_t, test_page_edge * test_page_edge * card::texel_bytes> level_0{};
    for (uint32_t y = 0; y < test_page_edge; ++y)
        for (uint32_t x = 0; x < test_page_edge; ++x) {
            uint8_t* texel = &level_0[(y * test_page_edge + x) * card::texel_bytes];
            texel[0] = static_cast<uint8_t>(x * 32U);
            texel[1] = static_cast<uint8_t>(y * 32U);
            texel[2] = static_cast<uint8_t>((x ^ y) * 16U + 8U);
            texel[3] = 255;
        }
    constexpr uint32_t half_edge = test_page_edge / 2;
    std::array<uint8_t, half_edge * half_edge * card::texel_bytes> level_1{};
    for (uint32_t y = 0; y < half_edge; ++y)
        for (uint32_t x = 0; x < half_edge; ++x)
            for (uint32_t channel = 0; channel < card::texel_bytes; ++channel) {
                const auto at = [&](uint32_t dx, uint32_t dy) {
                    return static_cast<uint32_t>(
                        level_0
                            [((2U * y + dy) * test_page_edge + 2U * x + dx) * card::texel_bytes +
                             channel]
                    );
                };
                level_1[(y * half_edge + x) * card::texel_bytes + channel] =
                    static_cast<uint8_t>((at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1) + 2U) / 4U);
            }
    if (!executor.update_page(
            page, 0, nullptr, level_0.data(), test_page_edge * card::texel_bytes
        ) ||
        !executor.update_page(page, 1, nullptr, level_1.data(), half_edge * card::texel_bytes))
        return finish("filling the test page: " + executor.error());
    card::CardFrame frame;
    card::Batch clear;
    clear.operation = card::Operation::clear;
    clear.target = target;
    clear.colour = test_clear_colour;
    frame.batches.push_back(clear);
    // (i) The page 1:1 NEAREST, at the top left.
    card::Batch quad;
    quad.target = target;
    quad.page = page;
    quad.level = 0;
    quad.sampling = card::Sampling::nearest;
    quad.first_index = 0;
    card::append_quad(
        frame,
        0.0F,
        0.0F,
        static_cast<float>(test_page_edge),
        static_cast<float>(test_page_edge),
        0.0F,
        0.0F,
        1.0F,
        1.0F,
        card::Colour{}
    );
    quad.index_count = static_cast<uint32_t>(frame.indices.size());
    frame.batches.push_back(quad);
    // (iii) The blended quad, at the top right.
    card::Batch blended;
    blended.target = target;
    blended.blend = card::Blend::alpha;
    blended.first_index = static_cast<card::Index>(frame.indices.size());
    card::append_quad(
        frame,
        static_cast<float>(test_page_edge),
        0.0F,
        static_cast<float>(test_page_edge),
        static_cast<float>(test_page_edge),
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        card::Colour{1.0F, 1.0F, 1.0F, test_blend_alpha}
    );
    blended.index_count = static_cast<uint32_t>(frame.indices.size()) - blended.first_index;
    frame.batches.push_back(blended);
    // (ii) Level 1 twice its size LINEAR, at the bottom left.
    card::Batch enlarged;
    enlarged.target = target;
    enlarged.page = page;
    enlarged.level = 1;
    enlarged.sampling = card::Sampling::linear;
    enlarged.first_index = static_cast<card::Index>(frame.indices.size());
    card::append_quad(
        frame,
        0.0F,
        static_cast<float>(test_page_edge),
        static_cast<float>(test_page_edge),
        static_cast<float>(test_page_edge),
        0.0F,
        0.0F,
        1.0F,
        1.0F,
        card::Colour{}
    );
    enlarged.index_count = static_cast<uint32_t>(frame.indices.size()) - enlarged.first_index;
    frame.batches.push_back(enlarged);
    // (iv) The triangle, over the rest of the target.
    card::Batch triangle;
    triangle.target = target;
    triangle.first_index = static_cast<card::Index>(frame.indices.size());
    append_triangle(
        frame,
        test_triangle,
        {card::Colour{1.0F, 0.0F, 0.0F, 1.0F},
         card::Colour{0.0F, 1.0F, 0.0F, 1.0F},
         card::Colour{0.0F, 0.0F, 1.0F, 1.0F}}
    );
    triangle.index_count = static_cast<uint32_t>(frame.indices.size()) - triangle.first_index;
    frame.batches.push_back(triangle);
    if (!executor.execute(frame, nullptr))
        return finish("the test frame: " + executor.error());
    // The target read back once: the page's texels, the enlargement, the
    // blend and the centroid.
    SDL_Texture* texture = executor.target_texture(target);
    if (texture == nullptr || !SDL_SetRenderTarget(renderer, texture))
        return finish(std::string("SDL_SetRenderTarget of the test target: ") + SDL_GetError());
    SDL_Surface* read = SDL_RenderReadPixels(renderer, nullptr);
    const bool back = SDL_SetRenderTarget(renderer, nullptr);
    SDL_Surface* pixels =
        read != nullptr ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32) : nullptr;
    SDL_DestroySurface(read);
    if (pixels == nullptr || !back) {
        SDL_DestroySurface(pixels);
        return finish(std::string("reading the test target back: ") + SDL_GetError());
    }
    std::string failure;
    const auto pixel = [&](uint32_t x, uint32_t y) {
        return static_cast<const uint8_t*>(pixels->pixels) +
               static_cast<std::ptrdiff_t>(y) * pixels->pitch +
               static_cast<std::ptrdiff_t>(x) * card::texel_bytes;
    };
    for (uint32_t y = 0; y < test_page_edge && failure.empty(); ++y)
        for (uint32_t x = 0; x < test_page_edge && failure.empty(); ++x)
            if (std::memcmp(
                    pixel(x, y),
                    &level_0[(y * test_page_edge + x) * card::texel_bytes],
                    compared_channels
                ) != 0)
                failure = "the page drawn 1:1 did not read back as its texels";
    // The enlargement: LINEAR within the tolerance, or NEAREST exactly where
    // the software renderer samples geometry so.
    bool linear_within = true;
    bool nearest_exact = true;
    for (uint32_t y = 0; y < test_page_edge; ++y)
        for (uint32_t x = 0; x < test_page_edge; ++x) {
            const uint8_t* shown = pixel(x, test_page_edge + y);
            const uint8_t* nearest = &level_1[((y / 2U) * half_edge + x / 2U) * card::texel_bytes];
            if (std::memcmp(shown, nearest, compared_channels) != 0)
                nearest_exact = false;
            for (std::size_t channel = 0; channel < compared_channels; ++channel)
                if (std::abs(
                        int{shown[channel]} -
                        enlarged_twice_linear(level_1.data(), half_edge, x, y, channel)
                    ) > test_blend_most_difference)
                    linear_within = false;
        }
    if (failure.empty() && !linear_within && !(software_renderer && nearest_exact))
        failure = "the page's level 1 drawn twice its size LINEAR did not read back as the "
                  "enlargement";
    const std::array<float, compared_channels> cleared{
        test_clear_colour.red, test_clear_colour.green, test_clear_colour.blue
    };
    for (uint32_t y = 0; y < test_page_edge && failure.empty(); ++y)
        for (uint32_t x = test_page_edge; x < 2U * test_page_edge && failure.empty(); ++x)
            for (std::size_t channel = 0; channel < compared_channels; ++channel) {
                const double expected =
                    255.0 * (test_blend_alpha + (1.0 - test_blend_alpha) * cleared[channel]);
                if (std::abs(static_cast<double>(pixel(x, y)[channel]) - expected) >
                    test_blend_most_difference) {
                    failure = "the quad at alpha one half did not read back as the blend";
                    break;
                }
            }
    if (failure.empty()) {
        const uint8_t* centroid = pixel(test_centroid_x, test_centroid_y);
        for (std::size_t channel = 0; channel < compared_channels; ++channel)
            if (std::abs(int{centroid[channel]} - test_centroid_mean) >
                test_centroid_most_difference) {
                failure = "the triangle's centroid pixel is not the mean of its corners' "
                          "colours: it read back " +
                          std::to_string(centroid[0]) + ", " + std::to_string(centroid[1]) + ", " +
                          std::to_string(centroid[2]);
                break;
            }
    }
    SDL_DestroySurface(pixels);
    return finish(failure);
}

/// Converts the overlay canvas by its key colour into ARGB8888 words, and
/// uploads the bands that hold paint now or held it at the last upload, so
/// that the texture holds this frame's overlay whatever came before.
///
/// @param canvas the painted canvas, RGB24 at the battlefield's size
/// @param key the colour the canvas was cleared to
/// @param bf_w the battlefield's width in pixels
/// @param bf_h its height
/// @param gamma the display gamma's table; null for none
/// @param pool the drawing threads
/// @param[in,out] overlay the words, the battlefield's size
/// @param[in,out] opaque_bands the bands that hold an opaque pixel now
/// @param[in,out] uploaded_bands the bands uploaded last
/// @param[in,out] uploaded the texture holds an uploaded overlay
/// @param[in,out] texture the overlay's texture, made for the size
void upload_keyed_overlay(
    const uint8_t* canvas,
    std::array<uint8_t, 3> key,
    uint32_t bf_w,
    uint32_t bf_h,
    const std::array<uint8_t, 256>* gamma,
    oa::platform::job_pool::Pool* pool,
    std::vector<uint8_t>& overlay,
    std::vector<uint8_t>& opaque_bands,
    std::vector<uint8_t>& uploaded_bands,
    bool& uploaded,
    TiledTexture& texture
) {
    const std::size_t pitch = std::size_t{bf_w} * card::texel_bytes;
    convert_rgb24_keyed_overlay_argb(
        canvas, key, bf_w, bf_h, overlay.data(), pitch, gamma, opaque_bands, pool
    );
    const auto bands = static_cast<uint32_t>(opaque_bands.size());
    const auto stale = [&](uint32_t band) {
        return !uploaded || opaque_bands[band] != 0 || uploaded_bands[band] != 0;
    };
    for (uint32_t band = 0; band < bands;) {
        if (!stale(band)) {
            ++band;
            continue;
        }
        uint32_t end = band + 1;
        while (end < bands && stale(end))
            ++end;
        texture.update(
            overlay.data(),
            static_cast<int>(pitch),
            static_cast<int>(card::texel_bytes),
            band * xrgb_band_rows,
            std::min(bf_h, end * xrgb_band_rows)
        );
        band = end;
    }
    uploaded_bands = opaque_bands;
    uploaded = true;
}

/// Returns the palette of the gray table's entries: each entry replaced by
/// the entry the fog grays it to, the palette's nearest to the grey of its
/// mean channel (build_gray_levels), so that an atlas built with it holds
/// the fog's picture of the terrain.
///
/// @param palette the match's palette, 4 bytes a colour
/// @return the greyed palette, the same layout
oa::PaletteBytes greyed_palette(const oa::PaletteBytes& palette) {
    const oa::Palette entries = oa::present::palette_from_bytes(palette);
    std::array<uint8_t, table_entries> levels{};
    wr::build_gray_levels(entries, levels);
    oa::PaletteBytes greyed = palette;
    for (std::size_t index = 0; index < OA_PALETTE_COLORS; ++index) {
        const auto& entry = entries.entries[index];
        const auto level =
            static_cast<std::size_t>((static_cast<unsigned>(entry.r) + entry.g + entry.b) / 3U);
        const std::size_t source = static_cast<std::size_t>(levels[level]) * palette_entry_stride;
        std::memcpy(&greyed[index * palette_entry_stride], &palette[source], palette_entry_stride);
    }
    return greyed;
}

/// Returns the texel bytes the pages of an atlas take at the levels the
/// Full tier uploads, which the memory guard counts before they are made.
///
/// @param atlas the atlas
/// @return the bytes
[[nodiscard]] uint64_t atlas_page_bytes(const gw::TerrainAtlas& atlas) noexcept {
    uint64_t bytes = 0;
    for (const auto& atlas_page : atlas.pages)
        for (std::size_t level = 0;
             level < std::min<std::size_t>(full_terrain_levels, atlas_page.levels.size());
             ++level)
            bytes += uint64_t{atlas_page.levels[level].width} * atlas_page.levels[level].height *
                     bytes_per_texel;
    return bytes;
}

/// Uploads levels 0 to full_terrain_levels - 1 of an atlas's pages as the
/// executor's pages, and lets each page's texels go once the card holds
/// them: the atlas keeps its grid and its pages' sizes and levels, which
/// the builder reads.
///
/// Throws FullCardError when a page cannot be made or filled.
///
/// @param[in,out] executor the executor
/// @param[in,out] atlas the atlas, whose texels are freed
/// @param[out] pages the executor's page for each page of the atlas, in order
/// @param[in,out] bytes the texel bytes uploaded, added to
/// @param what the atlas, for the error
void upload_atlas_pages(
    card::Executor& executor,
    gw::TerrainAtlas& atlas,
    std::vector<card::PageHandle>& pages,
    uint64_t& bytes,
    const char* what
) {
    for (auto& atlas_page : atlas.pages) {
        card::PageDescription description;
        description.width = atlas_page.width;
        description.height = atlas_page.height;
        description.level_count = static_cast<uint8_t>(
            std::min<std::size_t>(full_terrain_levels, atlas_page.levels.size())
        );
        const card::PageHandle page = executor.create_page(description);
        if (page == card::PageHandle{})
            throw FullCardError(
                std::string("a ") + what + " page could not be made: " + executor.error()
            );
        pages.push_back(page);
        for (uint8_t level = 0; level < description.level_count; ++level) {
            const auto& atlas_level = atlas_page.levels[level];
            const uint32_t pitch = atlas_level.width * card::texel_bytes;
            if (!executor.update_page(
                    page, level, nullptr, atlas_page.texels.data() + atlas_level.offset, pitch
                ))
                throw FullCardError(
                    std::string("a ") + what + " page could not be filled: " + executor.error()
                );
            bytes += uint64_t{pitch} * atlas_level.height;
        }
        // The card holds the page now; the processor keeps the page's
        // sizes and levels, which the builder reads, and lets its texels go.
        std::vector<uint8_t>().swap(atlas_page.texels);
    }
}

namespace supersampling = full_supersampling;

static_assert(
    policy::largest_supersample_factor == card::largest_supersampling_factor,
    "the policy's largest supersample factor is one a render target takes"
);

/// Returns the Enhanced anti-aliasing row's level that a unit
/// supersampling level is in effect for, which the Full tier reads its
/// factor from (render_policy::supersample_factor): the two name the same
/// six levels.
///
/// @param level the level units draw at outside Full
/// @return the row's level
[[nodiscard]] oa::ui::engine_settings::AntiAliasing
anti_aliasing_level_of(oa::present::model::UnitSupersampling level) noexcept {
    using oa::present::model::UnitSupersampling;
    using oa::ui::engine_settings::AntiAliasing;
    switch (level) {
    case UnitSupersampling::off:
        return AntiAliasing::off;
    case UnitSupersampling::x2:
        return AntiAliasing::x2;
    case UnitSupersampling::x4:
        return AntiAliasing::x4;
    case UnitSupersampling::x8:
        return AntiAliasing::x8;
    case UnitSupersampling::x16:
        return AntiAliasing::x16;
    }
    return AntiAliasing::off;
}

/// Returns the supersample factor a Full frame asks of the world target:
/// the Enhanced anti-aliasing row's (render_policy::supersample_factor).
///
/// @param level the row's level, as units draw at it outside Full
/// @return the factor
[[nodiscard]] uint32_t supersample_asked(oa::present::model::UnitSupersampling level) noexcept {
    return policy::supersample_factor(anti_aliasing_level_of(level));
}

/// Returns a count of bytes in whole mebibytes, for the log.
///
/// @param bytes the count
/// @return the mebibytes
[[nodiscard]] uint64_t mebibytes(uint64_t bytes) noexcept {
    return bytes / (uint64_t{1024} * 1024);
}

/// Readies the projectiles' shadow sprite for the model stage, from the
/// match's FX.GAF "shadow" frame, once per sprite.
///
/// @param[out] frame the rendered frame the stage reads
/// @param[in,out] source the sprite's pixels the frame was made from
/// @param shadow the match's sprite; no data for none
void ensure_projectile_shadow(
    oa::formats::gaf::RenderedFrame& frame, const void*& source, const oa::Sprite& shadow
) {
    if (source == shadow.data)
        return;
    frame = {};
    source = shadow.data;
    if (shadow.data == nullptr || shadow.width == 0 || shadow.height == 0)
        return;
    frame.width = shadow.width;
    frame.height = shadow.height;
    frame.origin_x = shadow.origin_x;
    frame.origin_y = shadow.origin_y;
    frame.transparency_index = shadow.key;
    const auto size = static_cast<std::size_t>(shadow.width) * shadow.height;
    const auto* pixels = static_cast<const uint8_t*>(shadow.data);
    frame.pixels.assign(pixels, pixels + size);
    frame.coverage.resize(size);
    for (std::size_t i = 0; i < size; ++i)
        frame.coverage[i] = pixels[i] != shadow.key ? 1 : 0;
}

/// Throws the error of a frame the executor did not run: FullFrameRefusedError
/// for a frame it refused before drawing anything, which no driver caused,
/// else FullCardError, its text starting with the call that failed, which
/// names the strike against the driver.
///
/// @param executor the executor
/// @param what the frame, for the error
[[noreturn]] void throw_frame_failure(const card::Executor& executor, std::string_view what) {
    if (executor.frame_refused())
        throw FullFrameRefusedError(
            "the card refused " + std::string(what) + ": " + executor.error()
        );
    throw FullCardError(executor.error() + ", running " + std::string(what));
}

/// Appends the quads the painters asked for over the world, in paint order,
/// consecutive quads of one blend in one batch.
///
/// @param[in,out] frame the frame
/// @param quads the quads, in pixels of the battlefield layer
/// @param battlefield the battlefield's rectangle in the target drawn into
/// @param target the target drawn into; none for the window
/// @return quads appended
uint32_t append_world_quads(
    card::CardFrame& frame,
    std::span<const FullWorldQuad> quads,
    const card::Rect& battlefield,
    card::TargetHandle target
) {
    card::Batch* batch = nullptr;
    for (const FullWorldQuad& quad : quads) {
        if (batch == nullptr || batch->blend != quad.blend) {
            frame.batches.emplace_back();
            batch = &frame.batches.back();
            batch->operation = card::Operation::draw;
            batch->target = target;
            batch->blend = quad.blend;
            batch->scissored = true;
            batch->scissor = battlefield;
            batch->first_index = static_cast<card::Index>(frame.indices.size());
            batch->index_count = 0;
        }
        card::append_quad(
            frame,
            static_cast<float>(battlefield.x + quad.x),
            static_cast<float>(battlefield.y + quad.y),
            static_cast<float>(quad.width),
            static_cast<float>(quad.height),
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            quad.colour
        );
        batch->index_count += quad_indices;
    }
    return static_cast<uint32_t>(quads.size());
}

} // namespace

void Runtime::destroy_full_presentation(FullPresentation* full) noexcept {
    delete full;
}

void Runtime::FullPresentation::ensure_sprite_palette(
    const oa::PaletteBytes& palette_bytes, float gamma
) {
    const oa::Palette palette = oa::present::palette_from_bytes(palette_bytes);
    sprite_pages.set_palette(palette, gamma);
    if (sprite_pages.has_gray_table() && gray_generation == sprite_pages.palette_generation())
        return;
    // The fog grays a pixel by its brightness: the entry nearest the grey
    // of its colour's mean channel, so a frame's greyed cell takes that
    // entry for each of its indices.
    std::array<uint8_t, table_entries> levels{};
    wr::build_gray_levels(palette, levels);
    std::array<uint8_t, gw::gray_table_entries> gray{};
    for (std::size_t index = 0; index < gray.size(); ++index) {
        const auto& entry = palette.entries[index];
        const auto level =
            static_cast<std::size_t>((static_cast<unsigned>(entry.r) + entry.g + entry.b) / 3U);
        gray[index] = levels[level];
    }
    sprite_pages.set_gray_table(gray);
    gray_generation = sprite_pages.palette_generation();
}

card::PageHandle Runtime::FullPresentation::card_page(uint32_t page) {
    const auto held = sprite_pages.pages();
    if (page >= held.size())
        return {};
    if (card_pages.size() <= page)
        card_pages.resize(std::size_t{page} + 1);
    FullCardPage& slot = card_pages[page];
    const uint32_t size = held[page].size;
    if (slot.handle != card::PageHandle{} && slot.size == size && executor.page_alive(slot.handle))
        return slot.handle;
    // The page at its old size may be named by batches of the frame being
    // built, so it goes once the frame has run.
    if (slot.handle != card::PageHandle{})
        executor.retire_page(slot.handle);
    slot = {};
    card::PageDescription description;
    description.width = size;
    description.height = size;
    description.level_count = 1;
    slot.handle = executor.create_page(description);
    if (slot.handle == card::PageHandle{})
        throw FullCardError(
            "a sprite page of " + std::to_string(size) +
            " texels a side could not be made: " + executor.error()
        );
    slot.size = size;
    return slot.handle;
}

card::PageHandle Runtime::FullPresentation::card_page_hook(void* context, uint32_t page) {
    return static_cast<FullPresentation*>(context)->card_page(page);
}

void Runtime::FullPresentation::upload_sprite_pages() {
    const auto held = sprite_pages.pages();
    for (uint32_t index = 0; index < card_pages.size() && index < held.size(); ++index) {
        FullCardPage& slot = card_pages[index];
        if (slot.handle == card::PageHandle{})
            continue;
        const gw::Page& page = held[index];
        if (page.size == 0 || page.size != slot.size) {
            // Batches of the frame being built may name it still.
            executor.retire_page(slot.handle);
            slot = {};
            continue;
        }
        if (slot.revision == page.revision)
            continue;
        const uint32_t pitch = page.size * card::texel_bytes;
        bool uploaded = true;
        if (slot.revision == 0) {
            uploaded = executor.update_page(slot.handle, 0, nullptr, page.texels.data(), pitch);
        } else if (!page.dirty.empty()) {
            const card::Rect part{page.dirty.x, page.dirty.y, page.dirty.width, page.dirty.height};
            const uint8_t* first =
                page.texels.data() +
                (std::size_t{page.dirty.y} * page.size + page.dirty.x) * card::texel_bytes;
            uploaded = executor.update_page(slot.handle, 0, &part, first, pitch);
        }
        if (!uploaded)
            throw FullCardError("a sprite page could not be filled: " + executor.error());
        sprite_pages.clear_dirty(index);
        slot.revision = page.revision;
    }
}

bool Runtime::FullPresentation::allow_page_growth(void* context, std::size_t bytes) {
    return static_cast<Runtime*>(context)->accelerated_buffer_fits(bytes);
}

void Runtime::FullPresentation::note_page_memory() {
    const gw::SpritePages* const sets[] = {&sprite_pages, &models.pages(), &models.bright_pages()};
    std::size_t limit = 0;
    std::size_t largest = 0;
    uint64_t held_refusals = 0;
    for (const gw::SpritePages* set : sets) {
        limit += set->limits().memory_limit;
        largest += set->limits().largest_memory_limit;
        held_refusals += set->statistics().held_refusals;
    }
    if (logged_page_memory == 0)
        logged_page_memory = limit;
    if (limit > logged_page_memory) {
        logged_page_memory = limit;
        std::cout << graphics_log_prefix << "the sprite and texture pages grew to "
                  << mebibytes(limit) << " MiB for this match\n"
                  << std::flush;
    }
    if (held_refusals == held_refusals_seen)
        return;
    held_refusals_seen = held_refusals;
    if (page_memory_full)
        return;
    page_memory_full = true;
    std::cout << graphics_log_prefix << "the sprite and texture pages hold " << mebibytes(limit)
              << " MiB, "
              << (limit >= largest ? "the most they grow to" : "as much as the memory allows")
              << "; what one frame needs past that is left out of it\n"
              << std::flush;
}

void Runtime::FullPresentation::destroy_sprite_card_pages() noexcept {
    for (FullCardPage& slot : card_pages)
        if (slot.handle != card::PageHandle{})
            executor.destroy_page(slot.handle);
    card_pages.clear();
}

void Runtime::FullPresentation::destroy_moved_targets() noexcept {
    executor.destroy_target(moved_target);
    executor.destroy_target(shifted_target);
    moved_target = {};
    shifted_target = {};
    moved_target_width = 0;
    moved_target_height = 0;
    moved_target_factor = 0;
}

void Runtime::FullPresentation::destroy_world_target() noexcept {
    executor.destroy_target(world_target);
    world_target = {};
    world_target_width = 0;
    world_target_height = 0;
    world_target_factor = 0;
    world_target_bytes = 0;
}

void Runtime::free_full_match_state() noexcept {
    if (!full_)
        return;
    free_full_match_textures();
    auto& full = *full_;
    // What the match's busiest frame took, for a log a player sends.
    if (const FullFrameVertices& busiest = full.busiest_frame; busiest.total != 0)
        std::cout << graphics_log_prefix << "the match's busiest frame held " << busiest.total
                  << " vertices: " << busiest.shadows << " of shadows, " << busiest.models
                  << " of models, " << busiest.sprites << " of sprites and the rest of the "
                  << "ground, the fog and the painters, at zoom " << busiest.zoom << '\n'
                  << std::flush;
    full.busiest_frame = {};
    full.sprite_pages.clear();
    full.sprite_pages.reset_memory_limit();
    full.logged_page_memory = 0;
    full.page_memory_full = false;
    full.gray_generation = 0;
    full.sprites = {};
    full.models_palette = {};
    full.models_gamma = 0.0F;
    full.projectile_shadow = {};
    full.projectile_shadow_source = nullptr;
    full.fog = {};
    full.canvas_drawn = false;
}

bool Runtime::full_presentation() const noexcept {
    return accelerated_presentation() && full_ && full_->on;
}

void Runtime::set_full_presentation(bool on) {
    if (!on) {
        if (full_ && full_->on) {
            full_->on = false;
            free_full_presentation();
        }
        return;
    }
    if (!full_)
        full_.reset(new FullPresentation);
    full_->on = true;
}

void Runtime::free_full_match_textures() noexcept {
    if (!full_)
        return;
    auto& full = *full_;
    for (const card::PageHandle page : full.pages)
        full.executor.destroy_page(page);
    full.pages.clear();
    for (const card::PageHandle page : full.greyed_pages)
        full.executor.destroy_page(page);
    full.greyed_pages.clear();
    full.page_bytes = 0;
    full.pages_from_load = false;
    full.executor.destroy_target(full.target);
    full.target = {};
    full.target_width = 0;
    full.target_height = 0;
    full.target_refused = false;
    full.destroy_moved_targets();
    full.refused_moved_width = 0;
    full.refused_moved_height = 0;
    full.refused_moved_factor = 0;
    full.destroy_world_target();
    full.refused_world_width = 0;
    full.refused_world_height = 0;
    full.refused_world_factor = 0;
    full.supersample = 1;
    full.drawn_plan = {};
    full.atlas = {};
    full.atlas_source = {};
    full.frame = {};
    full.overlay_texture.reset();
    full.overlay = {};
    full.opaque_bands = {};
    full.uploaded_bands = {};
    full.overlay_uploaded = false;
    // The stages' pages and targets go with the match too.
    full.destroy_sprite_card_pages();
    full.models.close(full.executor);
    full.world_quads.clear();
    full.drawn = false;
}

void Runtime::free_full_presentation() noexcept {
    if (!full_)
        return;
    free_full_match_textures();
    full_->executor.close();
    full_->function_tested = false;
}

void Runtime::drop_full(const std::string& reason, policy::FullDrop drop) {
    if (full_ && full_->on)
        std::cout << "open-annihilation: graphics: the full tier stopped (" << reason
                  << "); the basic tier draws the battlefield from now on\n"
                  << std::flush;
    free_full_presentation();
    if (full_)
        full_->on = false;
    if (!render_run_ || render_run_->host == nullptr)
        return;
    auto& host = *render_run_->host;
    auto& inputs = host.tier_inputs();
    if (inputs.full_drop == policy::FullDrop::none)
        inputs.full_drop = drop;
    // In a shared game or a replay Full stays away until the match ends.
    policy::note_match_frame(inputs.match, policy::RenderTier::accelerated, false);
    // Full's first frames, where they stood under their own sentinel, stop
    // with it; another path's stage stands.
    const auto& sentinel = host.records().sentinel();
    if (sentinel && sentinel->stage == renderer_state::SentinelStage::path &&
        sentinel->path == renderer_state::AcceleratedPath::full)
        host.end_path_stage();
}

void Runtime::take_full_failure(const std::string& error) {
    if (render_run_ && render_run_->host != nullptr) {
        auto& host = *render_run_->host;
        // A failure noted already, as the function test's is before it is
        // thrown, is not a second one; any other card call that failed is
        // struck against the driver.
        if (host.tier_inputs().full_drop == policy::FullDrop::none) {
            renderer_state::Strike failure;
            failure.stage = renderer_state::StrikeStage::card;
            failure.call = failing_call_name(error);
            std::ignore = host.note_running_failure(failure);
        }
    }
    drop_full(error, policy::FullDrop::card_failure);
}

bool Runtime::begin_full_path() {
    if (!render_run_ || render_run_->host == nullptr)
        return true;
    if (render_run_->host->begin_path(renderer_state::AcceleratedPath::full))
        return true;
    drop_full(std::string(path_trial_unwritten_reason), policy::FullDrop::trial_unwritten);
    return false;
}

void Runtime::preallocate_full_match_textures() {
    if (!render_run_ || render_run_->host == nullptr || !full_ || !full_->on ||
        sdl_.renderer == nullptr)
        return;
    const auto& gate = render_run_->host->tier_inputs().match;
    if (gate.kind == policy::MatchKind::none || !gate.full)
        return;
    auto& full = *full_;
    try {
        // The palette the atlas is built from: the game's, which the match
        // view reads again as it opens.
        match_palette_ = load_active_palette(assets_);
        if (begin_full_path()) {
            ensure_full_match_textures();
            full.pages_from_load = true;
            const auto bf_w = static_cast<uint32_t>(std::max(0, match_layout_.battlefield_width()));
            const auto bf_h =
                static_cast<uint32_t>(std::max(0, match_layout_.battlefield_height()));
            ensure_full_target(bf_w, bf_h);
            if (bf_w != 0 && bf_h != 0)
                ensure_full_world_target(
                    supersample_asked(unit_supersampling_), bf_w, bf_h, render_texture_limit()
                );
        }
    } catch (const FullCardError& error) {
        take_full_failure(error.what());
    } catch (const AccelerationError& error) {
        take_acceleration_error(error);
    }
}

void Runtime::ensure_full_target(uint32_t bf_w, uint32_t bf_h) {
    auto& full = *full_;
    if (full.target != card::TargetHandle{} || full.target_refused || bf_w == 0 || bf_h == 0)
        return;
    // The target, made once at the largest a zoom just above 1 needs, two
    // map pixels of room for every battlefield pixel, in whole map pixels at
    // every whole-number zoom; where the renderer cannot make it, the
    // terrain is drawn LINEAR straight.
    const uint32_t width = target_extent(2U * bf_w);
    const uint32_t height = target_extent(2U * bf_h);
    if (!accelerated_buffer_allowed(
            policy::AcceleratedBuffer::card_targets, uint64_t{width} * height * bytes_per_texel
        ))
        throw FullCardError("the full tier's zoom-in target: too little memory");
    full.target_width = width;
    full.target_height = height;
    full.target = full.executor.create_target(full.target_width, full.target_height, 1);
    if (full.target == card::TargetHandle{}) {
        full.target_refused = true;
        full.target_width = 0;
        full.target_height = 0;
        std::cout << "open-annihilation: graphics: the full tier's zoom-in target "
                     "could not be made ("
                  << full.executor.error() << "); the terrain between whole zooms is drawn LINEAR\n"
                  << std::flush;
    }
}

void Runtime::ensure_full_moved_target(uint32_t bf_w, uint32_t bf_h) {
    auto& full = *full_;
    // A layout pixel of room on every side, and the display's density in
    // texture pixels a layout pixel, rounded up to a factor a target takes.
    const uint32_t width = bf_w + 2U;
    const uint32_t height = bf_h + 2U;
    const double density = match_display_density();
    const uint32_t display_factor = density <= 1.0 ? 1U : density <= 2.0 ? 2U : 4U;
    const uint32_t doubled = display_factor * 2U;
    const bool doubled_refused = full.refused_moved_width == width &&
                                 full.refused_moved_height == height &&
                                 full.refused_moved_factor == doubled;
    const uint32_t wanted = doubled_refused ? display_factor : doubled;
    if (full.moved_target != card::TargetHandle{} && full.moved_target_width == width &&
        full.moved_target_height == height && full.moved_target_factor == wanted)
        return;
    full.destroy_moved_targets();
    const auto bytes_at = [&](uint32_t factor) {
        return uint64_t{width} * factor * height * factor * bytes_per_texel;
    };
    // Twice the display's density, with the shifted target, where the
    // memory and the renderer allow both: the tier draws on without them.
    if (!doubled_refused) {
        if (accelerated_buffer_fits(2U * bytes_at(doubled))) {
            full.moved_target = full.executor.create_target(width, height, doubled);
            if (full.moved_target != card::TargetHandle{})
                full.shifted_target = full.executor.create_target(width, height, doubled);
        }
        if (full.shifted_target != card::TargetHandle{}) {
            full.moved_target_width = width;
            full.moved_target_height = height;
            full.moved_target_factor = doubled;
            return;
        }
        std::cout << graphics_log_prefix << "full tier: the zoomed-out targets of " << width << "x"
                  << height << " at factor " << doubled
                  << " could not be made or would leave too little memory; the battlefield is "
                     "moved between pixels at the display's density\n"
                  << std::flush;
        full.destroy_moved_targets();
        full.refused_moved_width = width;
        full.refused_moved_height = height;
        full.refused_moved_factor = doubled;
    }
    if (!accelerated_buffer_allowed(
            policy::AcceleratedBuffer::card_targets, bytes_at(display_factor)
        ))
        throw FullCardError("the full tier's zoomed-out target: too little memory");
    full.moved_target = full.executor.create_target(width, height, display_factor);
    if (full.moved_target == card::TargetHandle{})
        throw FullCardError(
            "the full tier's zoomed-out target could not be made: " + full.executor.error()
        );
    full.moved_target_width = width;
    full.moved_target_height = height;
    full.moved_target_factor = display_factor;
}

uint32_t Runtime::full_supersample() const noexcept {
    return full_presentation() ? full_->supersample : 0;
}

void Runtime::ensure_full_world_target(
    uint32_t asked, uint32_t battlefield_width, uint32_t battlefield_height, uint32_t texture_limit
) {
    auto& full = *full_;
    // The budget is the largest target a renderer holds; below it the
    // renderer's texture limit and the memory guard decide, factor by factor.
    full.supersample_budget = policy::supersample_budget_pixels;
    const uint32_t size_width =
        supersampling::rounded_up(battlefield_width, supersampling::target_grain);
    const uint32_t size_height =
        supersampling::rounded_up(battlefield_height, supersampling::target_grain);
    uint32_t fitted = policy::fit_supersample_factor(
        asked, size_width, size_height, full.supersample_budget, texture_limit
    );
    // The samples a pixel a factor draws, as the log names them.
    const auto samples = [](uint32_t factor) {
        return std::to_string(factor) + "x" + std::to_string(factor);
    };
    const auto kept_at = [&](uint32_t factor) {
        return full.world_target != card::TargetHandle{} && full.world_target_width == size_width &&
               full.world_target_height == size_height && full.world_target_factor == factor;
    };
    // A factor the renderer or the memory guard refused at this size, and
    // every factor above it, is not asked for again.
    const auto refused_at = [&](uint32_t factor) {
        return full.refused_world_factor != 0 && full.refused_world_width == size_width &&
               full.refused_world_height == size_height && factor >= full.refused_world_factor;
    };
    const auto refuse = [&](uint32_t factor) {
        full.refused_world_width = size_width;
        full.refused_world_height = size_height;
        full.refused_world_factor = factor;
    };
    // The anti-aliasing asked for, or the largest halving of it the
    // renderer and the memory guard allow: a refusal costs a halving, never
    // the tier.
    while (fitted > 1 && !kept_at(fitted)) {
        if (refused_at(fitted)) {
            fitted /= 2;
            continue;
        }
        const uint64_t bytes =
            policy::supersample_target_pixels(size_width, size_height, fitted) * bytes_per_texel;
        if (!accelerated_buffer_fits(bytes)) {
            std::cout << graphics_log_prefix << "full tier: the world target of " << size_width
                      << "x" << size_height << " at factor " << fitted
                      << " would leave too little memory\n"
                      << std::flush;
            refuse(fitted);
            fitted /= 2;
            continue;
        }
        full.destroy_world_target();
        const uint64_t bytes_before = full.executor.counts().texture_bytes;
        full.world_target = full.executor.create_target(size_width, size_height, fitted, true);
        if (full.world_target == card::TargetHandle{}) {
            std::cout << graphics_log_prefix << "full tier: the world target of " << size_width
                      << "x" << size_height << " at factor " << fitted << " could not be made ("
                      << full.executor.error() << ")\n"
                      << std::flush;
            refuse(fitted);
            fitted /= 2;
            continue;
        }
        full.world_target_width = size_width;
        full.world_target_height = size_height;
        full.world_target_factor = fitted;
        full.world_target_bytes = full.executor.counts().texture_bytes - bytes_before;
        std::cout << graphics_log_prefix << "full tier: anti-aliasing " << samples(fitted) << " ("
                  << fitted * fitted << " samples a pixel): the world target of " << size_width
                  << "x" << size_height << " at factor " << fitted << " and its half hold "
                  << mebibytes(full.world_target_bytes) << " MiB\n"
                  << std::flush;
    }
    if (fitted == 1 && full.world_target != card::TargetHandle{})
        full.destroy_world_target();
    if (fitted < asked && (fitted != full.supersample || asked != full.supersample_asked))
        std::cout << graphics_log_prefix << "full tier: anti-aliasing "
                  << (fitted > 1 ? samples(fitted) : std::string("off"))
                  << ": the Enhanced anti-aliasing row asks for " << samples(asked)
                  << ", but the renderer's texture limit and the memory allow no larger world "
                  << "target at " << size_width << "x" << size_height << '\n'
                  << std::flush;
    full.supersample = fitted;
    full.supersample_asked = asked;
}

void Runtime::ensure_full_executor() {
    auto& full = *full_;
    if (!full.executor.is_open()) {
        if (!full.executor.open(sdl_.renderer, render_texture_limit()))
            throw FullCardError("the card could not be opened: " + full.executor.error());
        full.function_tested = false;
        // The executor's pages and targets went with its last hold on the
        // renderer.
        full.pages.clear();
        full.greyed_pages.clear();
        full.card_pages.clear();
        full.pages_from_load = false;
        full.atlas_source = {};
        full.target = {};
        full.target_refused = false;
        full.moved_target = {};
        full.shifted_target = {};
        full.moved_target_width = 0;
        full.moved_target_height = 0;
        full.moved_target_factor = 0;
        full.refused_moved_width = 0;
        full.refused_moved_height = 0;
        full.refused_moved_factor = 0;
        full.world_target = {};
        full.world_target_width = 0;
        full.world_target_height = 0;
        full.world_target_factor = 0;
        full.world_target_bytes = 0;
        full.refused_world_width = 0;
        full.refused_world_height = 0;
        full.refused_world_factor = 0;
        full.supersample = 1;
    }
    if (!full.function_tested) {
        const bool software =
            render_run_ && render_run_->host != nullptr &&
            render_run_->host->facts().renderer == oa::platform::render_probe::software_renderer;
        const std::string failure = run_full_function_test(full.executor, sdl_.renderer, software);
        oa::base::float_precision::restore_program_float_control();
        if (!failure.empty()) {
            // Not capable of Full: no strike, since the card drew nothing
            // wrong that a driver's failure would explain.
            drop_full("the Full function test failed: " + failure, policy::FullDrop::function_test);
            throw FullCardError("the Full function test failed: " + failure);
        }
        full.function_tested = true;
    }
}

void Runtime::ensure_full_terrain_pages(const oa::PaletteBytes& palette) {
    auto& full = *full_;
    if (!selected_tnt_)
        throw FullCardError("the match has no map");
    const auto& map = *selected_tnt_;
    const uint32_t limit = render_texture_limit();
    const uint32_t page_edge = gw::fit_page_edge(
        limit == render_policy::unlimited_texture_size ? gw::page_edge_limit : limit
    );
    const bool gamma = !gamma_identity_;
    const FullPresentation::AtlasSource source = FullPresentation::AtlasSource::of(map);
    const bool same_source = full.atlas_source == source && !full.pages.empty() &&
                             full.pages.size() == full.atlas.pages.size() &&
                             full.greyed_pages.size() == full.pages.size() &&
                             full.atlas_page_edge == page_edge && full.atlas_palette == palette &&
                             full.atlas_gamma == gamma &&
                             (!gamma || full.atlas_gamma_table == gamma_table_);
    if (same_source)
        return;
    for (const card::PageHandle page : full.pages)
        full.executor.destroy_page(page);
    full.pages.clear();
    for (const card::PageHandle page : full.greyed_pages)
        full.executor.destroy_page(page);
    full.greyed_pages.clear();
    full.page_bytes = 0;
    full.pages_from_load = false;
    full.atlas_source = {};
    const auto build_start = std::chrono::steady_clock::now();
    // The atlas holds the map the view shows: the mosaic less the edges the
    // game never shows, whose filler tiles the card then never draws.
    const auto [columns, rows] = shown_tile_grid();
    const gw::TerrainAtlasError error = gw::build_terrain_atlas(
        map, columns, rows, palette, gamma ? &gamma_table_ : nullptr, page_edge, full.atlas
    );
    if (error != gw::TerrainAtlasError::none)
        throw FullCardError(
            std::string("the terrain atlas could not be built: ") +
            gw::terrain_atlas_error_text(error)
        );
    // The greyed terrain: the same build with the gray table's entries,
    // whose slots and grid are the colour atlas's, since the tiles are told
    // apart by their indices; its texels go once uploaded.
    gw::TerrainAtlas greyed;
    const gw::TerrainAtlasError greyed_error = gw::build_terrain_atlas(
        map,
        columns,
        rows,
        greyed_palette(palette),
        gamma ? &gamma_table_ : nullptr,
        page_edge,
        greyed
    );
    full.atlas_build_ns = nanoseconds_since(build_start);
    if (greyed_error != gw::TerrainAtlasError::none)
        throw FullCardError(
            std::string("the greyed terrain atlas could not be built: ") +
            gw::terrain_atlas_error_text(greyed_error)
        );
    if (greyed.pages.size() != full.atlas.pages.size() || greyed.grid != full.atlas.grid)
        throw FullCardError("the greyed terrain atlas does not share the terrain's slots");
    // The pages' memory, both atlases', counted by the memory guard before
    // they are made.
    if (!accelerated_buffer_allowed(
            policy::AcceleratedBuffer::card_pages,
            atlas_page_bytes(full.atlas) + atlas_page_bytes(greyed)
        ))
        throw FullCardError("the terrain pages: too little memory");
    const auto upload_start = std::chrono::steady_clock::now();
    upload_atlas_pages(full.executor, full.atlas, full.pages, full.page_bytes, "terrain");
    upload_atlas_pages(full.executor, greyed, full.greyed_pages, full.page_bytes, "greyed terrain");
    full.page_upload_ns = nanoseconds_since(upload_start);
    full.atlas_source = source;
    full.atlas_page_edge = page_edge;
    full.atlas_palette = palette;
    full.atlas_gamma = gamma;
    full.atlas_gamma_table = gamma_table_;
}

void Runtime::make_full_match_pages() {
    if (!full_presentation() || !selected_tnt_)
        return;
    auto& full = *full_;
    try {
        // Full's first card calls of the run stand under its trial, as its
        // first frame's do; a trial that cannot be written keeps Full off
        // with nothing struck.
        if (!begin_full_path())
            return;
        ensure_full_executor();
        // The loading screen's palette is the match's: both are the game's
        // active palette, which the match view loads again as it is entered.
        ensure_full_terrain_pages(load_active_palette(assets_));
        full.pages_from_load = true;
    } catch (const FullCardError& error) {
        take_full_failure(error.what());
    }
}

void Runtime::ensure_full_match_textures() {
    auto& full = *full_;
    ensure_full_executor();
    ensure_full_terrain_pages(match_palette_);
    // The sprite pages and the model stage, at the match's palette.
    full.ensure_sprite_palette(match_palette_, display_gamma_);
    if (match_models_) {
        auto& models = match_models();
        if (full.models_gamma != display_gamma_ || full.models_palette != match_palette_) {
            full.models.set_palette(models.display.palette, display_gamma_);
            full.models_palette = match_palette_;
            full.models_gamma = display_gamma_;
        }
        ensure_projectile_shadow(
            full.projectile_shadow, full.projectile_shadow_source, models.projectile_shadow
        );
    }
    // The overlay, at the battlefield's size: the world layer's, or before
    // the first frame the match layout's.
    const uint32_t bf_w =
        match_world_cpu_.width != 0
            ? match_world_cpu_.width
            : static_cast<uint32_t>(std::max(0, match_layout_.battlefield_width()));
    const uint32_t bf_h =
        match_world_cpu_.height != 0
            ? match_world_cpu_.height
            : static_cast<uint32_t>(std::max(0, match_layout_.battlefield_height()));
    if (bf_w == 0 || bf_h == 0)
        return;
    const uint32_t limit = render_texture_limit();
    if (!full.overlay_texture.made_for(bf_w, bf_h, limit)) {
        full.overlay_texture.create(
            sdl_.renderer, bf_w, bf_h, limit, SDL_BLENDMODE_BLEND, full.counts
        );
        full.overlay_uploaded = false;
        const uint32_t bands = platform::job_pool::bands_of_rows(bf_h, xrgb_band_rows);
        full.opaque_bands.assign(bands, 0);
        full.uploaded_bands.assign(bands, 0);
        full.overlay.assign(std::size_t{bf_w} * bf_h * 4U, 0);
        // A new battlefield size needs new targets.
        full.executor.destroy_target(full.target);
        full.target = {};
        full.target_width = 0;
        full.target_height = 0;
        full.target_refused = false;
        full.destroy_moved_targets();
        full.destroy_world_target();
    }
}

bool Runtime::present_full_match_layers(bool dialogs) {
    auto& full = *full_;
    // Full's first match frame of the run stands under its own trial and
    // sentinel; where the trial cannot be written Full is dropped, with
    // nothing struck, and Basic presents the frame.
    if (!begin_full_path()) {
        full.drawn = false;
        return false;
    }
    try {
        // The front end's prescale target is not kept during a match.
        accelerated_.screen_prescale.destroy();
        const auto frame_format = opaque_layer_format();
        ensure_streaming_texture(
            match_hud_tex_,
            frame_format,
            static_cast<int>(match_hud_cpu_.width),
            static_cast<int>(match_hud_cpu_.height),
            match_hud_tex_w_,
            match_hud_tex_h_
        );
        // The HUD layer's prescale target and the layout's bookkeeping, as
        // Basic's frame makes them, so that the HUD strips are drawn by
        // sharp_draw from that target at every chrome scale as Basic draws
        // them; the magnified world's part makes nothing, since a Full frame
        // is drawn at the zoom with no split.
        ensure_accelerated_match_textures();
        ensure_full_match_textures();
        const uint32_t bf_w = match_world_cpu_.width;
        const uint32_t bf_h = match_world_cpu_.height;
        const float zoom = match_zoom();
        const uint32_t limit = render_texture_limit();
        // The frame's world layer is the overlay canvas, drawn at the zoom
        // and camera the card draws from; a frame drawn otherwise, as the
        // standard tier's scaling draws it, is Basic's to present.
        const bool ready = full.canvas_drawn && match_ && match_models_ &&
                           std::abs(full.frame_zoom - zoom) <= 1.0e-4F &&
                           !accelerated_.frame.apart &&
                           full.pages.size() == full.atlas.pages.size() && !full.pages.empty() &&
                           full.greyed_pages.size() == full.pages.size() &&
                           full.overlay_texture.made_for(bf_w, bf_h, limit);
        if (!ready) {
            full.drawn = false;
            return false;
        }
        const auto* gamma = gamma_identity_ ? nullptr : &gamma_table_;
        const auto upload_start = std::chrono::steady_clock::now();
        upload_rgb24_frame(match_hud_tex_, match_hud_cpu_);
        // What the painters painted on the canvas, by its key, uploaded in
        // the bands that hold it now or held it at the last upload, so the
        // texture holds this frame's overlay whatever came before.
        const auto overlay_start = std::chrono::steady_clock::now();
        upload_keyed_overlay(
            match_world_cpu_.rgb.data(),
            full_overlay_key(),
            bf_w,
            bf_h,
            gamma,
            draw_pool_.get(),
            full.overlay,
            full.opaque_bands,
            full.uploaded_bands,
            full.overlay_uploaded,
            full.overlay_texture
        );
        full.overlay_ns = nanoseconds_since(overlay_start);
        const auto present_start = std::chrono::steady_clock::now();
        phase_times_.upload += static_cast<int64_t>(nanoseconds_since(upload_start));
        if (!SDL_SetRenderDrawColor(sdl_.renderer, 0, 0, 0, 255) || !SDL_RenderClear(sdl_.renderer))
            throw_present_error("SDL_RenderClear");
        // In placed mode the HUD's pieces are drawn after the world
        // (finish_match_layers).
        if (!oa::ui::display_layout::placed_mode(match_layout_))
            draw_accelerated_hud_strips();

        // The card's frame: the terrain from the pages, by the level rule at
        // the zoom, and everything over it.
        const auto build_start = std::chrono::steady_clock::now();
        auto& frame = full.frame;
        frame.reset();
        const card::Rect battlefield{
            match_layout_.left,
            match_layout_.top,
            static_cast<int32_t>(bf_w),
            static_cast<int32_t>(bf_h)
        };
        // The pixel-art sampling mode where the start-up probe found it
        // scales as that filter does, which the rung's card filter carries
        // (SDL's software renderer takes the mode and draws it NEAREST),
        // and the executor sets it on the pages.
        const bool pixel_art = accelerated_.rung.card == render_policy::CardFilter::pixelart &&
                               full.executor.capabilities().pixel_art_sampling;
        full.plan = ft::plan_terrain_draw(zoom, pixel_art);
        // A view drawn between map pixels lands that far before the
        // battlefield's corner, the terrain and the fog with the stages.
        const auto& offset = accelerated_.frame_offset;
        ft::TerrainView view;
        view.camera_x = full.frame_camera_x;
        view.camera_y = full.frame_camera_y;
        view.origin_x = static_cast<float>(
            static_cast<double>(match_layout_.left) - offset.x * static_cast<double>(zoom)
        );
        view.origin_y = static_cast<float>(
            static_cast<double>(match_layout_.top) - offset.y * static_cast<double>(zoom)
        );
        view.scale = zoom;
        view.width = bf_w;
        view.height = bf_h;
        // Below the zoom the other tiers reach, a tile spans a fraction of
        // a pixel or a few: the tiles and the fog's quads meet on whole
        // pixels, which leaves no row or column between them on a renderer
        // that rounds each quad by itself.
        view.whole_pixels = zoom < kMinBattlefieldZoom;
        uint32_t quads = 0;
        uint32_t fog_quads = 0;
        uint32_t world_quads = 0;
        bool through_target = false;
        // The fog's greyed pass over the terrain, from the greyed pages at
        // the terrain's levels and sampling, into whatever the terrain was
        // drawn into. Under the dithered option the pass is the dither over
        // everything, drawn after the stages.
        const auto& fog = full.fog;
        const bool fogged = !fog.grid.tiles.empty();
        const auto append_unseen = [&](const ft::TerrainView& where,
                                       std::span<const full_fog::GreyedLevel> levels,
                                       card::TargetHandle target,
                                       const card::Rect* scissor) {
            if (!fogged || fog.dithered)
                return;
            full_fog::FogPlacement placement;
            placement.camera_x = fog.camera_x;
            placement.camera_z = fog.camera_z;
            placement.origin_x = where.origin_x;
            placement.origin_y = where.origin_y;
            placement.scale = where.scale;
            placement.whole_pixels = where.whole_pixels;
            fog_quads += full_fog::append_unseen_terrain(
                frame, fog.grid, full.atlas, full.greyed_pages, levels, placement, target, scissor
            );
        };

        // The stages, over the last list the planner built: what they read,
        // and the sight the sprite stage tells the fog's state from; none
        // when the match cannot say, which draws every sprite in colour.
        auto& models = match_models();
        full::ModelFrameInputs model_inputs;
        model_inputs.draws = &models.draws;
        model_inputs.world = &match_->world().record;
        model_inputs.library = &models.library;
        model_inputs.display = &models.display;
        model_inputs.graphics_flags = models.renderer.graphics_flags;
        model_inputs.build_pulse_tick = models.renderer.tick - models.renderer.build_pulse_lag;
        model_inputs.moving_pieces_once_built = models.renderer.moving_pieces_once_built;
        std::copy_n(
            models.renderer.team_colors, full::team_colour_players, model_inputs.team_colors.begin()
        );
        model_inputs.light = {
            models.renderer.light[0], models.renderer.light[1], models.renderer.light[2]
        };
        model_inputs.light_scale = models.renderer.light_scale;
        model_inputs.projectile_shadow =
            full.projectile_shadow.width != 0 ? &full.projectile_shadow : nullptr;
        full::SpriteStageInputs sprite_inputs;
        sprite_inputs.list = &models.draws;
        std::span<const uint8_t> coverage;
        try {
            coverage = match_->player_coverage(match_view_player());
        } catch (const std::exception&) {
            coverage = {};
        }
        const auto& sight = match_->sight();
        if (!coverage.empty() && sight.width > 0 && sight.height > 0) {
            sprite_inputs.sight.coverage = coverage;
            sprite_inputs.sight.player_bits = sight.player_bits;
            sprite_inputs.sight.width = sight.width;
            sprite_inputs.sight.height = sight.height;
            sprite_inputs.sight.viewer_bit =
                static_cast<uint16_t>(1U << (sight.viewpoint_player & 0x1fU));
            sprite_inputs.sight.line_of_sight = match_line_of_sight_on();
            sprite_inputs.sight.mapping = match_mapping_on();
            sprite_inputs.sight.dithered = fog.dithered;
        }
        sprite_inputs.palette = &match_palette_;
        sprite_inputs.gamma = gamma;
        const full::SpritePageHooks hooks{&full, &FullPresentation::card_page_hook};
        // The frame's sprites and textures stay on their pages until it has
        // run, and the pages grow where those alone fill them, as far as the
        // memory guard allows.
        const gw::GrowthHooks growth{this, &FullPresentation::allow_page_growth};
        full.sprite_pages.set_growth_hooks(growth);
        full.models.set_growth_hooks(growth);
        full.sprite_pages.begin_frame();
        full.models.begin_frame();
        full.sprites = {};
        full.frame_vertices = {};
        full.stage_ns = 0;
        // Emits the stages into the frame through a view: straight to the
        // window at the battlefield's corner, or into the world target at
        // its draw scale. The model stage's shadows first, then each draw of
        // the list to the stage of its kind, the sprite stage's batches never
        // joining the model stage's; the pages' texels the stages changed go
        // up before the frame runs.
        const auto emit_stages =
            [&](float origin_x, float origin_y, float view_scale, card::TargetHandle target) {
                const auto stage_start = std::chrono::steady_clock::now();
                full::SceneView scene;
                scene.origin_x = origin_x;
                scene.origin_y = origin_y;
                scene.scale = view_scale;
                scene.zoom = zoom;
                scene.offset = accelerated_.frame_offset;
                scene.width = static_cast<int32_t>(bf_w);
                scene.height = static_cast<int32_t>(bf_h);
                scene.camera_x = static_cast<int32_t>(full.frame_camera_x);
                scene.camera_y = static_cast<int32_t>(full.frame_camera_y);
                scene.target = target;
                sprite_inputs.view = scene;
                {
                    full::SpriteFrame sprites(sprite_inputs, full.sprite_pages, hooks, frame);
                    std::size_t before = frame.vertices.size();
                    full.models.emit_shadows(model_inputs, scene, full.executor, frame);
                    full.frame_vertices.shadows += frame.vertices.size() - before;
                    for (const WorldDraw& draw : models.draws.draws) {
                        before = frame.vertices.size();
                        if (full::sprite_kind(draw.kind)) {
                            sprites.emit(draw);
                            full.frame_vertices.sprites += frame.vertices.size() - before;
                        } else if (full::model_kind(draw.kind)) {
                            full.models.emit_draw(model_inputs, scene, draw, full.executor, frame);
                            full.frame_vertices.models += frame.vertices.size() - before;
                        }
                    }
                    full.sprites = sprites.finish();
                }
                if (full.sprites.pages_overflowed && !full.overflow_logged) {
                    full.overflow_logged = true;
                    std::cout << graphics_log_prefix
                              << "the frame's sprites do not fit the sprite pages; the card draws "
                                 "none of them this frame\n"
                              << std::flush;
                }
                if (!full.models.error().empty() && !full.stage_error_logged) {
                    full.stage_error_logged = true;
                    std::cout << graphics_log_prefix
                              << "the full tier's model stage: " << full.models.error()
                              << "; it draws on without what it could not make\n"
                              << std::flush;
                }
                full.note_page_memory();
                full.upload_sprite_pages();
                if (!full.models.upload(full.executor))
                    throw FullCardError(
                        "the model stage's pages could not be uploaded: " + full.models.error()
                    );
                full.stage_ns += nanoseconds_since(stage_start);
            };
        // Appends the fog's passes over everything the card drew into a
        // view: the fog's dither, where the option is on, since the
        // processor dithers the objects as it does the ground and the pages
        // hold no dithered sprite, and the fog's black pass over never-mapped
        // ground. Into the world target where one is drawn, before its
        // reduction, so that the target holds the whole picture the
        // battlefield shows; else straight to the window.
        const auto append_fog_over = [&](const ft::TerrainView& where,
                                         card::TargetHandle target,
                                         const card::Rect* scissor) {
            if (!fogged)
                return;
            full_fog::FogPlacement placement;
            placement.camera_x = fog.camera_x;
            placement.camera_z = fog.camera_z;
            placement.origin_x = where.origin_x;
            placement.origin_y = where.origin_y;
            placement.scale = where.scale;
            placement.whole_pixels = where.whole_pixels;
            if (fog.dithered)
                fog_quads += full_fog::append_unseen_dither(
                    frame, fog.grid, fog.dither, placement, target, scissor
                );
            fog_quads += full_fog::append_unmapped(
                frame, fog.grid, fog.unmapped, placement, target, scissor
            );
        };
        bool fog_in_target = false;
        // Below zoom 1 the battlefield is drawn into the moved target, a
        // pixel in from its corner, on the screen pixels laid from the map's
        // corner as the view's offset places it, and the card then moves the
        // target onto the battlefield by the rest of a screen pixel, at the
        // view's exact place: the fog's passes over everything and the
        // painters' quads go where the battlefield was drawn.
        bool moved = false;
        ft::TerrainView over_view = view;
        card::TargetHandle over_target{};
        const card::Rect* over_scissor = &battlefield;
        card::Rect painted = battlefield;

        // Anti-aliasing: the factor the Enhanced anti-aliasing row asks for,
        // fitted to this battlefield, and the world target at it.
        ensure_full_world_target(supersample_asked(unit_supersampling_), bf_w, bf_h, limit);
        const supersampling::WorldTargetPlan supersampled =
            supersampling::plan_world_target(zoom, full.supersample, bf_w, bf_h);
        full.drawn_plan = supersampled;
        if (supersampled.factor > 1) {
            // From zoom 1 up the terrain into the world target, level 0
            // NEAREST at the zoom, the texture holding the factor's texels a
            // window pixel; the fog's greyed pass and the stages into it at
            // the same scale; then the target reduced into the battlefield
            // by the factor's halvings. Below zoom 1 the plan makes no world
            // target: the zoomed-out target below holds two texels a display
            // pixel already.
            through_target = true;
            full.plan = ft::plan_terrain_draw(1.0F, false);
            const ft::TerrainPass pass = full.plan.passes[0];
            card::Batch clear;
            clear.operation = card::Operation::clear;
            clear.target = full.world_target;
            clear.colour = card::Colour{0.0F, 0.0F, 0.0F, 1.0F};
            frame.batches.push_back(clear);
            ft::TerrainView target_view = view;
            target_view.scale = supersampled.draw_scale;
            target_view.origin_x =
                static_cast<float>(-offset.x * static_cast<double>(target_view.scale));
            target_view.origin_y =
                static_cast<float>(-offset.y * static_cast<double>(target_view.scale));
            target_view.width = supersampled.size_width;
            target_view.height = supersampled.size_height;
            quads += ft::append_terrain_tiles(
                frame, full.atlas, full.pages, target_view, pass, full.world_target, nullptr
            );
            const std::array<full_fog::GreyedLevel, 1> levels{{{pass.level, pass.sampling, 1.0F}}};
            append_unseen(target_view, levels, full.world_target, nullptr);
            // Size pixels per layout pixel, so that the texture holds the
            // stages at the factor's pixels a window pixel.
            emit_stages(0.0F, 0.0F, supersampled.draw_scale / zoom, full.world_target);
            append_fog_over(target_view, full.world_target, nullptr);
            fog_in_target = true;
            card::Batch reduce;
            reduce.operation = card::Operation::resolve;
            reduce.source = full.world_target;
            reduce.destination = {
                battlefield.x + supersampled.destination.x,
                battlefield.y + supersampled.destination.y,
                supersampled.destination.width,
                supersampled.destination.height
            };
            reduce.scissored = true;
            reduce.scissor = battlefield;
            frame.batches.push_back(reduce);
        } else if (full.plan.through_target) {
            ensure_full_target(bf_w, bf_h);
            const uint32_t factor = full.plan.target_zoom;
            if (full.target != card::TargetHandle{} && factor != 0 &&
                full.target_width / factor * zoom >= static_cast<float>(bf_w) &&
                full.target_height / factor * zoom >= static_cast<float>(bf_h)) {
                through_target = true;
                card::Batch clear;
                clear.operation = card::Operation::clear;
                clear.target = full.target;
                clear.colour = card::Colour{0.0F, 0.0F, 0.0F, 1.0F};
                frame.batches.push_back(clear);
                ft::TerrainView target_view = view;
                target_view.scale = static_cast<float>(factor);
                target_view.origin_x =
                    static_cast<float>(-offset.x * static_cast<double>(target_view.scale));
                target_view.origin_y =
                    static_cast<float>(-offset.y * static_cast<double>(target_view.scale));
                target_view.width = full.target_width;
                target_view.height = full.target_height;
                const ft::TerrainPass& pass = full.plan.passes[0];
                quads += ft::append_terrain_tiles(
                    frame, full.atlas, full.pages, target_view, pass, full.target, nullptr
                );
                const std::array<full_fog::GreyedLevel, 1> levels{
                    {{pass.level, pass.sampling, 1.0F}}
                };
                append_unseen(target_view, levels, full.target, nullptr);
                card::Batch resolve;
                resolve.operation = card::Operation::resolve;
                resolve.source = full.target;
                resolve.scissored = true;
                resolve.scissor = battlefield;
                resolve.destination = {
                    battlefield.x,
                    battlefield.y,
                    static_cast<int32_t>(std::lround(
                        static_cast<double>(full.target_width) * zoom / static_cast<double>(factor)
                    )),
                    static_cast<int32_t>(std::lround(
                        static_cast<double>(full.target_height) * zoom / static_cast<double>(factor)
                    ))
                };
                frame.batches.push_back(resolve);
            } else {
                ft::TerrainPass pass = full.plan.passes[0];
                pass.sampling = card::Sampling::linear;
                quads += ft::append_terrain_tiles(
                    frame, full.atlas, full.pages, view, pass, card::TargetHandle{}, &battlefield
                );
                const std::array<full_fog::GreyedLevel, 1> levels{
                    {{pass.level, pass.sampling, 1.0F}}
                };
                append_unseen(view, levels, card::TargetHandle{}, &battlefield);
            }
            emit_stages(
                static_cast<float>(match_layout_.left),
                static_cast<float>(match_layout_.top),
                1.0F,
                card::TargetHandle{}
            );
        } else {
            moved = zoom < 1.0F;
            if (moved) {
                ensure_full_moved_target(bf_w, bf_h);
                // The terrain by the level rule at the target's own texels,
                // so that a level's texel lands on about one of the target's
                // and the reduction onto the battlefield, not an enlarged
                // level, makes each display pixel: the ground keeps its
                // detail. From one texel a map pixel up, level 0 alone,
                // LINEAR, which the reduction smooths as it would a larger
                // level.
                const float texel_zoom = zoom * static_cast<float>(full.moved_target_factor);
                full.plan = ft::plan_terrain_draw(std::min(texel_zoom, 1.0F), false);
                for (uint32_t index = 0; index < full.plan.pass_count; ++index)
                    full.plan.passes[index].sampling = card::Sampling::linear;
                over_target = full.moved_target;
                over_scissor = nullptr;
                painted = {1, 1, static_cast<int32_t>(bf_w), static_cast<int32_t>(bf_h)};
                // The terrain fills the room around the battlefield too,
                // which the move brings into view at an edge.
                over_view.origin_x = static_cast<float>(1.0 - offset.x * static_cast<double>(zoom));
                over_view.origin_y = static_cast<float>(1.0 - offset.y * static_cast<double>(zoom));
                over_view.width = bf_w + 2U;
                over_view.height = bf_h + 2U;
                over_view.margin = 1;
                card::Batch clear;
                clear.operation = card::Operation::clear;
                clear.target = over_target;
                clear.colour = card::Colour{0.0F, 0.0F, 0.0F, 1.0F};
                frame.batches.push_back(clear);
            }
            std::array<full_fog::GreyedLevel, ft::most_terrain_passes> levels{};
            for (uint32_t index = 0; index < full.plan.pass_count; ++index) {
                const ft::TerrainPass& pass = full.plan.passes[index];
                quads += ft::append_terrain_tiles(
                    frame, full.atlas, full.pages, over_view, pass, over_target, over_scissor
                );
                levels[index] = {pass.level, pass.sampling, 1.0F};
            }
            // The second pass is blended over the first at its alpha, so it
            // carries that share of the picture and the first the rest.
            if (full.plan.pass_count == 2) {
                levels[1].share = std::clamp(full.plan.passes[1].alpha, 0.0F, 1.0F);
                levels[0].share = 1.0F - levels[1].share;
            }
            append_unseen(
                over_view,
                std::span<const full_fog::GreyedLevel>(levels.data(), full.plan.pass_count),
                over_target,
                over_scissor
            );
            emit_stages(
                static_cast<float>(painted.x), static_cast<float>(painted.y), 1.0F, over_target
            );
        }
        // Over everything, straight to the window or into the moved target:
        // the fog's passes where no world target holds them, and the
        // painters' quads.
        if (!fog_in_target)
            append_fog_over(over_view, over_target, over_scissor);
        world_quads = append_world_quads(frame, full.world_quads, painted, over_target);
        full.build_ns = nanoseconds_since(build_start) - full.stage_ns;
        const auto execute_start = std::chrono::steady_clock::now();
        // The failure --check-renderer-ladder forces stands in for the
        // card's own.
        if (render_fault_due(RenderFaultPoint::card))
            throw FullCardError(std::string(forced_card_failure));
        if (!full.executor.execute(frame, nullptr))
            throw_frame_failure(full.executor, "the frame");
        oa::base::float_precision::restore_program_float_control();
        full.execute_ns = nanoseconds_since(execute_start);
        // The frame counts towards the stage of Full's first frames.
        if (render_run_)
            render_run_->paths_drawn = static_cast<PathSet>(
                render_run_->paths_drawn | path_bit(renderer_state::AcceleratedPath::full)
            );
        full.drawn = true;
        full.drawn_zoom = zoom;
        full.drawn_quads = quads;
        full.drawn_fog_quads = fog_quads;
        full.drawn_world_quads = world_quads;
        full.drawn_batches = static_cast<uint32_t>(frame.batches.size());
        full.drawn_through_target = through_target;
        full.frame_vertices.total = frame.vertices.size();
        full.frame_vertices.zoom = zoom;
        if (full.frame_vertices.total > full.busiest_frame.total)
            full.busiest_frame = full.frame_vertices;
        ++full.frames;

        // What the painters painted, over the card's picture, 1:1: into the
        // moved target where the battlefield was drawn into it, which the
        // card then moves onto the battlefield by the rest of a screen pixel
        // (view_shift), so that the ground, everything on it and the
        // painters' marks move together; else over the window.
        const auto& shift = accelerated_.frame_shift;
        if (moved) {
            SDL_Texture* into = full.executor.target_texture(full.moved_target);
            const SDL_FRect in_target{
                1.0F, 1.0F, static_cast<float>(bf_w), static_cast<float>(bf_h)
            };
            if (into == nullptr || !SDL_SetRenderTarget(sdl_.renderer, into))
                throw FullCardError(
                    std::string("SDL_SetRenderTarget of the zoomed-out target: ") + SDL_GetError()
                );
            draw_one_to_one(
                sdl_.renderer, full.overlay_texture, nullptr, &in_target, one_to_one_scale_mode()
            );
            if (!SDL_SetRenderTarget(sdl_.renderer, nullptr))
                throw FullCardError(
                    std::string("SDL_SetRenderTarget of the window: ") + SDL_GetError()
                );
            auto& move = full.moved_frame;
            move.reset();
            card::Batch onto;
            onto.operation = card::Operation::resolve;
            onto.source = full.moved_target;
            onto.destination = {
                battlefield.x - 1,
                battlefield.y - 1,
                static_cast<int32_t>(full.moved_target_width),
                static_cast<int32_t>(full.moved_target_height)
            };
            onto.shift_x = static_cast<float>(shift[0]);
            onto.shift_y = static_cast<float>(shift[1]);
            onto.scissored = true;
            onto.scissor = battlefield;
            if (full.shifted_target != card::TargetHandle{}) {
                // The shift at the target's own texels, where a LINEAR draw
                // blurs no more than a fraction of a texel, then the shifted
                // picture reduced in place, each display pixel the mean of
                // its texels: one LINEAR draw moving the picture by a
                // fraction of a display pixel would blur it by that fraction,
                // so that the ground sharpened each time it landed on whole
                // pixels and softened between.
                card::Batch shifted = onto;
                shifted.target = full.shifted_target;
                shifted.destination = {
                    0,
                    0,
                    static_cast<int32_t>(full.moved_target_width),
                    static_cast<int32_t>(full.moved_target_height)
                };
                shifted.scissored = false;
                shifted.scissor = {};
                move.batches.push_back(shifted);
                onto.source = full.shifted_target;
                onto.shift_x = 0.0F;
                onto.shift_y = 0.0F;
            }
            move.batches.push_back(onto);
            if (!full.executor.execute(move, nullptr))
                throw_frame_failure(full.executor, "the zoomed-out target's move");
            oa::base::float_precision::restore_program_float_control();
        } else {
            const SDL_FRect world{
                static_cast<float>(static_cast<double>(battlefield.x) + shift[0]),
                static_cast<float>(static_cast<double>(battlefield.y) + shift[1]),
                static_cast<float>(bf_w),
                static_cast<float>(bf_h)
            };
            draw_one_to_one(
                sdl_.renderer, full.overlay_texture, nullptr, &world, one_to_one_scale_mode()
            );
        }
        finish_match_layers(frame_format, dialogs, upload_start, present_start);
        return true;
    } catch (const FullFrameRefusedError& error) {
        // A frame built wrong is the engine's fault, not the driver's.
        drop_full(error.what(), policy::FullDrop::frame_refused);
        return false;
    } catch (const FullCardError& error) {
        take_full_failure(error.what());
        return false;
    }
}

} // namespace oa::app
