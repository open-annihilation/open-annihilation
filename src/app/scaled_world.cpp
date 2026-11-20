// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Window-size textures as one texture or as tiles with gutters, and the
// accelerated tier's drawing on the graphics card.
#include "oa/app/scaled_world.hpp"

#include "xrgb_conversion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace oa::app {

void throw_present_error(const std::string& what) {
    throw PresentError(what + ": " + SDL_GetError());
}

SDL_ScaleMode direct_scale_mode(render_policy::ScaleFilter filter) noexcept {
    switch (filter) {
    case render_policy::ScaleFilter::nearest:
        return SDL_SCALEMODE_NEAREST;
    case render_policy::ScaleFilter::pixelart:
#if SDL_VERSION_ATLEAST(3, 4, 0)
        return SDL_SCALEMODE_PIXELART;
#else
        return SDL_SCALEMODE_NEAREST;
#endif
    case render_policy::ScaleFilter::sharp_bilinear:
    case render_policy::ScaleFilter::linear:
        return SDL_SCALEMODE_LINEAR;
    }
    return SDL_SCALEMODE_NEAREST;
}

namespace {

/// The first SDL release that draws a queued texture with the scale mode
/// it had when the draw was queued. An older release draws it with the
/// mode the texture has when the queued draws run, at the present or at a
/// flush.
constexpr int scale_mode_kept_per_draw_version = SDL_VERSIONNUM(3, 2, 10);

/// Runs the queued draws before a texture's scale mode changes, on an SDL
/// that would otherwise draw them with the changed mode; on a newer one the
/// queue is left to run at the present. The SDL the game runs on decides,
/// since a shared SDL library can be older than its headers.
///
/// @param renderer the renderer
/// @return false when SDL refused to run the queue
[[nodiscard]] bool run_draws_before_scale_mode_changes(SDL_Renderer* renderer) noexcept {
    if (SDL_GetVersion() >= scale_mode_kept_per_draw_version)
        return true;
    return SDL_FlushRenderer(renderer);
}

} // namespace

void draw_one_to_one(
    SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect* destination, SDL_ScaleMode mode
) {
    const bool filtered = mode != SDL_SCALEMODE_NEAREST;
    if (filtered && !SDL_SetTextureScaleMode(texture, mode))
        throw AccelerationError(std::string("SDL_SetTextureScaleMode: ") + SDL_GetError());
    const bool drawn = SDL_RenderTexture(renderer, texture, nullptr, destination);
    const bool run = !filtered || !drawn || run_draws_before_scale_mode_changes(renderer);
    if (filtered)
        std::ignore = SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    if (!drawn)
        throw_present_error("SDL_RenderTexture");
    if (!run)
        throw AccelerationError(std::string("SDL_FlushRenderer: ") + SDL_GetError());
}

void draw_one_to_one(
    SDL_Renderer* renderer,
    TiledTexture& layer,
    const SDL_FRect* source,
    const SDL_FRect* destination,
    SDL_ScaleMode mode
) {
    if (mode == SDL_SCALEMODE_NEAREST) {
        layer.draw(renderer, source, destination);
        return;
    }
    if (!layer.set_scale_mode(mode)) {
        std::ignore = layer.set_scale_mode(SDL_SCALEMODE_NEAREST);
        throw AccelerationError(std::string("SDL_SetTextureScaleMode: ") + SDL_GetError());
    }
    try {
        layer.draw(renderer, source, destination);
    } catch (...) {
        std::ignore = layer.set_scale_mode(SDL_SCALEMODE_NEAREST);
        throw;
    }
    const bool run = run_draws_before_scale_mode_changes(renderer);
    std::ignore = layer.set_scale_mode(SDL_SCALEMODE_NEAREST);
    if (!run)
        throw AccelerationError(std::string("SDL_FlushRenderer: ") + SDL_GetError());
}

bool set_frame_presentation(
    SDL_Renderer* renderer, render_policy::MenuScaling scaling, int width, int height
) {
    int output_width = 0;
    int output_height = 0;
    if (!SDL_GetRenderOutputSize(renderer, &output_width, &output_height))
        return false;
    const auto fit = render_policy::frame_fit(scaling, output_width, output_height, width, height);
    return SDL_SetRenderLogicalPresentation(
        renderer,
        width,
        height,
        fit == render_policy::FrameFit::whole_steps ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                                    : SDL_LOGICAL_PRESENTATION_LETTERBOX
    );
}

void draw_frame(SDL_Renderer* renderer, SDL_Texture* texture, SDL_ScaleMode mode) {
    const bool filtered = mode != SDL_SCALEMODE_NEAREST && SDL_SetTextureScaleMode(texture, mode);
    const bool drawn = SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    const bool run = !filtered || !drawn || run_draws_before_scale_mode_changes(renderer);
    if (filtered)
        std::ignore = SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    if (!drawn)
        throw_present_error("SDL_RenderTexture");
    if (!run)
        throw_present_error("SDL_FlushRenderer");
}

void draw_frame(
    SDL_Renderer* renderer, TiledTexture& frame, SDL_ScaleMode mode, const SDL_FRect* destination
) {
    const bool filtered = mode != SDL_SCALEMODE_NEAREST && frame.set_scale_mode(mode);
    if (!filtered) {
        // A refused mode may have reached some tiles: all go back to NEAREST.
        if (mode != SDL_SCALEMODE_NEAREST)
            std::ignore = frame.set_scale_mode(SDL_SCALEMODE_NEAREST);
        frame.draw(renderer, nullptr, destination);
        return;
    }
    try {
        frame.draw(renderer, nullptr, destination);
    } catch (...) {
        std::ignore = frame.set_scale_mode(SDL_SCALEMODE_NEAREST);
        throw;
    }
    const bool run = run_draws_before_scale_mode_changes(renderer);
    std::ignore = frame.set_scale_mode(SDL_SCALEMODE_NEAREST);
    if (!run)
        throw_present_error("SDL_FlushRenderer");
}

namespace {

namespace policy = render_policy;

/// Says whether two grids split a texture alike.
///
/// @param first one grid
/// @param second the other
/// @return true for the same size, tiles, columns and rows
bool same_grid(const policy::TileGrid& first, const policy::TileGrid& second) noexcept {
    return first.width == second.width && first.height == second.height &&
           first.tile_size == second.tile_size && first.columns == second.columns &&
           first.rows == second.rows;
}

/// The colour a new prescale target is cleared to and read back as: one no
/// picture is likely to leave in it.
constexpr std::array<uint8_t, 3> target_check_colour{0x5A, 0xA5, 0x3C};

#if SDL_VERSION_ATLEAST(3, 4, 0)
/// The pixel-art probe's source, 2x1 texels, and its target, 5x1 pixels.
constexpr int pixelart_probe_source_width = 2;
constexpr int pixelart_probe_target_width = 5;
/// The probe's black and white texels.
constexpr uint32_t pixelart_probe_black = 0xFF000000U;
constexpr uint32_t pixelart_probe_white = 0xFFFFFFFFU;
#endif

/// Throws an AccelerationError naming the call and SDL's error, after
/// setting the render target back to the final one and clearing the clip.
///
/// @param renderer the renderer
/// @param final_target the target frames are drawn into; null for the window
/// @param what the call that failed
[[noreturn]] void fail(SDL_Renderer* renderer, SDL_Texture* final_target, const char* what) {
    std::string message = std::string(what) + ": " + SDL_GetError();
    std::ignore = SDL_SetRenderTarget(renderer, final_target);
    std::ignore = SDL_SetRenderClipRect(renderer, nullptr);
    throw AccelerationError(message);
}

/// The part two texel rectangles share; empty when they do not meet.
///
/// @param first a rectangle
/// @param second another
/// @return the shared rectangle
[[nodiscard]] policy::TexelRect
shared(const policy::TexelRect& first, const policy::TexelRect& second) {
    const uint32_t left = std::max(first.x, second.x);
    const uint32_t top = std::max(first.y, second.y);
    const uint32_t right = std::min(first.x + first.width, second.x + second.width);
    const uint32_t bottom = std::min(first.y + first.height, second.y + second.height);
    if (right <= left || bottom <= top)
        return {};
    return {left, top, right - left, bottom - top};
}

/// Throws the error of a texture's tier.
///
/// @param accelerated true for a texture only the accelerated tier makes
/// @param message what failed and SDL's error
[[noreturn]] void throw_texture_error(bool accelerated, const std::string& message) {
    if (accelerated)
        throw AccelerationError(message);
    throw PresentError(message);
}

/// Draws a rectangle of a picture held in tiles into a rectangle of the
/// current render target: from each tile the rectangle covers, with the
/// tile's gutters left out, each part landing where it lands in the whole,
/// so that tiles meet with no seam and tiles drawn 1:1 at whole-pixel
/// places give the pixels one texture would. A picture of one tile is one
/// SDL_RenderTexture of the rectangles.
///
/// @param renderer the renderer
/// @param grid how the picture is split into tiles
/// @param texture_at returns a tile's texture by its column and row
/// @param region the rectangle drawn, in the picture's texels
/// @param destination where it lands, in the render target's coordinates
/// @return false when SDL refused a draw
template <typename TextureAt>
bool draw_tiles(
    SDL_Renderer* renderer,
    const policy::TileGrid& grid,
    const TextureAt& texture_at,
    const SDL_FRect& region,
    const SDL_FRect& destination
) {
    if (grid.columns == 1 && grid.rows == 1)
        return SDL_RenderTexture(renderer, texture_at(0U, 0U), &region, &destination);
    if (!(region.w > 0.0F) || !(region.h > 0.0F))
        return true;
    const double across = static_cast<double>(destination.w) / static_cast<double>(region.w);
    const double down = static_cast<double>(destination.h) / static_cast<double>(region.h);
    const double right = static_cast<double>(region.x) + static_cast<double>(region.w);
    const double bottom = static_cast<double>(region.y) + static_cast<double>(region.h);
    const auto x_at = [&](double texel) {
        return static_cast<float>(
            static_cast<double>(destination.x) + (texel - static_cast<double>(region.x)) * across
        );
    };
    const auto y_at = [&](double texel) {
        return static_cast<float>(
            static_cast<double>(destination.y) + (texel - static_cast<double>(region.y)) * down
        );
    };
    for (uint32_t row = 0; row < grid.rows; ++row)
        for (uint32_t column = 0; column < grid.columns; ++column) {
            const auto tile = policy::tile_at(grid, column, row);
            const double left = std::max<double>(tile.content.x, region.x);
            const double top = std::max<double>(tile.content.y, region.y);
            const double end_x = std::min<double>(tile.content.x + tile.content.width, right);
            const double end_y = std::min<double>(tile.content.y + tile.content.height, bottom);
            if (end_x <= left || end_y <= top)
                continue;
            const SDL_FRect source{
                static_cast<float>(left - tile.texture.x),
                static_cast<float>(top - tile.texture.y),
                static_cast<float>(end_x - left),
                static_cast<float>(end_y - top)
            };
            const float drawn_x = x_at(left);
            const float drawn_y = y_at(top);
            const SDL_FRect drawn{drawn_x, drawn_y, x_at(end_x) - drawn_x, y_at(end_y) - drawn_y};
            if (!SDL_RenderTexture(renderer, texture_at(column, row), &source, &drawn))
                return false;
        }
    return true;
}

/// A texture a picture drawn into a prescale target comes from: the whole
/// picture, or one tile of it.
struct SourcePiece {
    SDL_Texture* texture{};
    /// The picture's texels it supplies, in the whole picture's texels.
    policy::TexelRect content{};
    uint32_t origin_x{}; ///< the picture's column at the texture's left edge
    uint32_t origin_y{}; ///< the picture's row at the texture's top edge
};

/// A run of a prescale target's texels along one axis and the picture's
/// texels it shows at a whole-number factor: whole blocks of `factor`
/// texels each, or one texel's block cut by the run's ends.
struct PrescaleRun {
    uint32_t source_first{}; ///< the first picture texel
    uint32_t source_count{}; ///< picture texels
    uint32_t target_first{}; ///< the first target texel
    uint32_t target_count{}; ///< target texels
};

/// Returns the runs that make up the target texels from `start` up to
/// `end` at a factor, the picture's texels limited to those from
/// `source_first` up to `source_end`: a cut block at each end where the
/// run starts or ends inside a block, and the whole blocks between. Each
/// run draws exactly, NEAREST, whatever renderer draws it: whole blocks at
/// the factor, a cut block from one texel.
///
/// @param start the first target texel
/// @param end the target texel past the last
/// @param factor target texels per picture texel, above 1
/// @param source_first the first picture texel the runs may show
/// @param source_end the picture texel past the last they may show
/// @param[out] runs the runs, in order, from the first
/// @return the runs written, at most 3
uint32_t prescale_runs(
    uint32_t start,
    uint32_t end,
    uint32_t factor,
    uint32_t source_first,
    uint32_t source_end,
    std::array<PrescaleRun, 4>& runs
) {
    uint32_t count = 0;
    // One texel's block cut to the target texels from `first` up to `last`.
    const auto cut = [&](uint32_t texel, uint32_t first, uint32_t last) {
        if (texel >= source_first && texel < source_end && last > first)
            runs[count++] = {texel, 1, first, last - first};
    };
    if (end <= start)
        return 0;
    const uint32_t first_whole = (start + factor - 1) / factor;
    const uint32_t end_whole = end / factor;
    if (first_whole > end_whole) {
        // The run lies inside one block.
        cut(start / factor, start, end);
        return count;
    }
    if (start % factor != 0)
        cut(start / factor, start, first_whole * factor);
    // The whole blocks, within the picture texels the runs may show.
    const uint32_t low = std::max(first_whole, source_first);
    const uint32_t high = std::min(end_whole, source_end);
    if (high > low)
        runs[count++] = {low, high - low, low * factor, (high - low) * factor};
    if (end % factor != 0)
        cut(end_whole, end_whole * factor, end);
    return count;
}

/// Draws the top-left corner of a picture NEAREST into a prescale target at
/// a whole-number factor: into each of the target's tiles, gutters
/// included, from each piece of the picture that holds texels the tile
/// shows. The column and row of the target past the enlarged corner, where
/// the target holds them, take the corner's last column and row, the
/// corner's last texel where they meet: LINEAR reads them at the corner's
/// edge, and they give back what a target of the enlarged corner's own
/// size gives there. The render target is left on the target's last tile.
///
/// Throws AccelerationError, after setting the render target back to
/// `final_target`, when a call fails.
///
/// @param renderer the renderer
/// @param final_target the target frames are drawn into; null for the window
/// @param prescale the prescale target, at least the corner times the factor
/// @param pieces the picture's pieces, each at NEAREST
/// @param width columns of the corner
/// @param height rows of the corner
/// @param factor target texels per picture texel, above 1
void fill_prescale(
    SDL_Renderer* renderer,
    SDL_Texture* final_target,
    const PrescaleTarget& prescale,
    std::span<const SourcePiece> pieces,
    uint32_t width,
    uint32_t height,
    uint32_t factor
) {
    const auto& grid = prescale.grid();
    const uint32_t filled_w = width * factor;
    const uint32_t filled_h = height * factor;
    const uint32_t written_w = std::min(filled_w + 1, grid.width);
    const uint32_t written_h = std::min(filled_h + 1, grid.height);
    // The runs of the target texels from `start` up to `end` along one
    // axis: the enlarged corner's, from the picture's texels a piece holds,
    // then the texel past the corner from the corner's last texel, where
    // the piece holds that.
    const auto runs_of = [factor](
                             uint32_t start,
                             uint32_t end,
                             uint32_t filled,
                             uint32_t corner,
                             uint32_t source_first,
                             uint32_t source_end,
                             std::array<PrescaleRun, 4>& runs
                         ) {
        uint32_t count =
            prescale_runs(start, std::min(end, filled), factor, source_first, source_end, runs);
        const uint32_t last = corner - 1;
        if (end > filled && last >= source_first && last < source_end)
            runs[count++] = {last, 1, filled, 1};
        return count;
    };
    std::array<PrescaleRun, 4> across{};
    std::array<PrescaleRun, 4> down{};
    for (uint32_t row = 0; row < grid.rows; ++row)
        for (uint32_t column = 0; column < grid.columns; ++column) {
            const auto tile = policy::tile_at(grid, column, row);
            const auto filled = shared(tile.texture, {0, 0, written_w, written_h});
            if (filled.width == 0)
                continue;
            if (!SDL_SetRenderTarget(renderer, prescale.tile_texture(column, row)))
                fail(renderer, final_target, "SDL_SetRenderTarget of a prescale tile");
            for (const auto& piece : pieces) {
                const uint32_t runs_across = runs_of(
                    filled.x,
                    filled.x + filled.width,
                    filled_w,
                    width,
                    piece.content.x,
                    piece.content.x + piece.content.width,
                    across
                );
                const uint32_t runs_down = runs_of(
                    filled.y,
                    filled.y + filled.height,
                    filled_h,
                    height,
                    piece.content.y,
                    piece.content.y + piece.content.height,
                    down
                );
                for (uint32_t y = 0; y < runs_down; ++y)
                    for (uint32_t x = 0; x < runs_across; ++x) {
                        const SDL_FRect source{
                            static_cast<float>(across[x].source_first - piece.origin_x),
                            static_cast<float>(down[y].source_first - piece.origin_y),
                            static_cast<float>(across[x].source_count),
                            static_cast<float>(down[y].source_count)
                        };
                        const SDL_FRect drawn{
                            static_cast<float>(across[x].target_first - tile.texture.x),
                            static_cast<float>(down[y].target_first - tile.texture.y),
                            static_cast<float>(across[x].target_count),
                            static_cast<float>(down[y].target_count)
                        };
                        if (!SDL_RenderTexture(renderer, piece.texture, &source, &drawn))
                            fail(renderer, final_target, "SDL_RenderTexture into a prescale tile");
                    }
            }
        }
}

/// Draws a rectangle of a prescale target LINEAR into a rectangle of the
/// current render target, from each tile the rectangle covers (draw_tiles).
///
/// Throws AccelerationError, after setting the render target back to
/// `final_target`, when a draw fails.
///
/// @param renderer the renderer
/// @param final_target the target frames are drawn into; null for the window
/// @param prescale the prescale target
/// @param region the rectangle drawn, in the target's texels
/// @param destination where it lands, in the current render target's coordinates
void draw_prescaled(
    SDL_Renderer* renderer,
    SDL_Texture* final_target,
    const PrescaleTarget& prescale,
    const SDL_FRect& region,
    const SDL_FRect& destination
) {
    const auto texture_at = [&](uint32_t column, uint32_t row) {
        return prescale.tile_texture(column, row);
    };
    if (!draw_tiles(renderer, prescale.grid(), texture_at, region, destination))
        fail(renderer, final_target, "SDL_RenderTexture of the prescale target");
}

/// Bits of the 16.16 fixed point SDL's software LINEAR steps through a texture in.
constexpr int software_linear_fraction_bits = 16;
/// Bits of the weight toward a sample's second texel: 128ths of a texel.
constexpr int software_linear_weight_bits = 7;
/// The weight of a whole texel.
constexpr uint32_t software_linear_weight_one = 1U << software_linear_weight_bits;

/// One pixel's sample along an axis of SDL's software LINEAR: the two
/// texels around it and the weight toward the second.
struct LinearSample {
    uint32_t first{};
    uint32_t second{};
    uint32_t weight{}; ///< 128ths of a texel toward `second`
};

/// Returns the samples of SDL's software LINEAR along one axis.
///
/// @param texels the texels along the axis, at least 1
/// @param pixels the pixels they are drawn into, at least 1
/// @return one sample a pixel
[[nodiscard]] std::vector<LinearSample> software_linear_samples(uint32_t texels, uint32_t pixels) {
    std::vector<LinearSample> samples(pixels);
    if (texels < 2)
        return samples;
    const int64_t step = (uint64_t{texels} << software_linear_fraction_bits) / pixels;
    const int64_t half = int64_t{1} << (software_linear_fraction_bits - 1);
    const int64_t start = ((step * half + half) >> software_linear_fraction_bits) - half;
    const int64_t last_pair = int64_t{texels} - 2;
    for (uint32_t pixel = 0; pixel < pixels; ++pixel) {
        const int64_t at = start + step * pixel;
        if (at < 0)
            continue;
        const int64_t index = at >> software_linear_fraction_bits;
        if (index > last_pair) {
            samples[pixel] = {texels - 1, texels - 1, 0};
            continue;
        }
        const auto weight = static_cast<uint32_t>(
            (at >> (software_linear_fraction_bits - software_linear_weight_bits)) &
            (software_linear_weight_one - 1)
        );
        samples[pixel] = {static_cast<uint32_t>(index), static_cast<uint32_t>(index) + 1, weight};
    }
    return samples;
}

} // namespace

bool clear_streaming_texture(SDL_Texture* texture) noexcept {
    void* pixels = nullptr;
    int pitch = 0;
    if (texture == nullptr || !SDL_LockTexture(texture, nullptr, &pixels, &pitch))
        return false;
    std::memset(pixels, 0, static_cast<std::size_t>(pitch) * static_cast<std::size_t>(texture->h));
    SDL_UnlockTexture(texture);
    return true;
}

bool TiledTexture::ensure(
    SDL_Renderer* renderer,
    SDL_PixelFormat format,
    int width,
    int height,
    uint32_t limit,
    SDL_BlendMode blend
) {
    const policy::TileGrid grid = policy::plan_tiles(
        static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0)), limit
    );
    if (made() && format_ == format && blend_ == blend && same_grid(grid, grid_))
        return false;
    reset();
    if (grid.columns == 0 || grid.rows == 0)
        throw PresentError(
            "texture of " + std::to_string(width) + "x" + std::to_string(height) +
            " beyond the renderer's limit of " + std::to_string(limit)
        );
    make(renderer, format, grid, limit, blend);
    return true;
}

void TiledTexture::create(
    SDL_Renderer* renderer,
    uint32_t width,
    uint32_t height,
    uint32_t limit,
    SDL_BlendMode blend,
    ScaledWorldCounts& counts
) {
    reset();
    const policy::TileGrid grid = policy::plan_tiles(width, height, limit);
    if (grid.columns == 0 || grid.rows == 0)
        throw AccelerationError(
            "a texture cannot be split into tiles under the renderer's limit",
            AccelerationFault::engine
        );
    counts_ = &counts;
    make(renderer, SDL_PIXELFORMAT_ARGB8888, grid, limit, blend);
}

void TiledTexture::make(
    SDL_Renderer* renderer,
    SDL_PixelFormat format,
    const policy::TileGrid& grid,
    uint32_t limit,
    SDL_BlendMode blend
) {
    grid_ = grid;
    format_ = format;
    blend_ = blend;
    limit_ = limit;
    // SDL's error is taken before the tiles made by then are destroyed.
    const auto failed = [&](const char* what) {
        const bool accelerated = counts_ != nullptr;
        const std::string message = std::string(what) + ": " + SDL_GetError();
        reset();
        throw_texture_error(accelerated, message);
    };
    tiles_.reserve(static_cast<std::size_t>(grid.columns) * grid.rows);
    for (uint32_t row = 0; row < grid.rows; ++row)
        for (uint32_t column = 0; column < grid.columns; ++column) {
            Piece piece;
            piece.tile = policy::tile_at(grid, column, row);
            piece.texture = SDL_CreateTexture(
                renderer,
                format,
                SDL_TEXTUREACCESS_STREAMING,
                static_cast<int>(piece.tile.texture.width),
                static_cast<int>(piece.tile.texture.height)
            );
            if (piece.texture == nullptr)
                failed("SDL_CreateTexture");
            tiles_.push_back(piece);
            if (counts_ != nullptr)
                ++counts_->textures_created;
            // SDL blends alpha formats unless told not to.
            if ((blend != SDL_BLENDMODE_NONE || SDL_ISPIXELFORMAT_ALPHA(format)) &&
                !SDL_SetTextureBlendMode(piece.texture, blend))
                failed("SDL texture blend");
            if (!SDL_SetTextureScaleMode(piece.texture, SDL_SCALEMODE_NEAREST))
                failed("SDL texture scale");
            if (!clear_streaming_texture(piece.texture))
                failed("SDL_LockTexture");
        }
}

void TiledTexture::fail(const std::string& what) const {
    throw_texture_error(counts_ != nullptr, what + ": " + SDL_GetError());
}

void TiledTexture::update(
    const uint8_t* pixels, int pitch, int bytes_per_pixel, uint32_t first_row, uint32_t end_row
) {
    const uint32_t end = std::min(end_row, grid_.height);
    if (first_row >= end)
        return;
    const policy::TexelRect rows{0, first_row, grid_.width, end - first_row};
    for (const auto& piece : tiles_) {
        const auto part = shared(piece.tile.texture, rows);
        if (part.width == 0)
            continue;
        const SDL_Rect updated{
            static_cast<int>(part.x - piece.tile.texture.x),
            static_cast<int>(part.y - piece.tile.texture.y),
            static_cast<int>(part.width),
            static_cast<int>(part.height)
        };
        const uint8_t* first = pixels + static_cast<std::ptrdiff_t>(part.y) * pitch +
                               static_cast<std::ptrdiff_t>(part.x) * bytes_per_pixel;
        if (!SDL_UpdateTexture(piece.texture, &updated, first, pitch))
            fail("SDL_UpdateTexture");
    }
}

void TiledTexture::update_rect(
    const uint8_t* pixels, int pitch, int bytes_per_pixel, const policy::TexelRect& rect
) {
    for (const auto& piece : tiles_) {
        const auto part = shared(piece.tile.texture, rect);
        if (part.width == 0)
            continue;
        const SDL_Rect updated{
            static_cast<int>(part.x - piece.tile.texture.x),
            static_cast<int>(part.y - piece.tile.texture.y),
            static_cast<int>(part.width),
            static_cast<int>(part.height)
        };
        const uint8_t* first = pixels + static_cast<std::ptrdiff_t>(part.y - rect.y) * pitch +
                               static_cast<std::ptrdiff_t>(part.x - rect.x) * bytes_per_pixel;
        if (!SDL_UpdateTexture(piece.texture, &updated, first, pitch))
            fail("SDL_UpdateTexture");
    }
}

void TiledTexture::upload_rgb24(
    const uint8_t* rgb,
    std::size_t rgb_pitch,
    uint32_t width,
    uint32_t height,
    const std::array<uint8_t, 256>* gamma,
    platform::job_pool::Pool* pool
) {
    const uint32_t corner_width = std::min(width, grid_.width);
    const uint32_t corner_height = std::min(height, grid_.height);
    if (corner_width == 0 || corner_height == 0)
        return;
    const policy::TexelRect corner{0, 0, corner_width, corner_height};
    // The corner and the column and row past it, where the texture holds them.
    const policy::TexelRect written{
        0, 0, std::min(corner_width + 1, grid_.width), std::min(corner_height + 1, grid_.height)
    };
    for (const auto& piece : tiles_) {
        const auto part = shared(piece.tile.texture, written);
        if (part.width == 0)
            continue;
        const SDL_Rect locked{
            static_cast<int>(part.x - piece.tile.texture.x),
            static_cast<int>(part.y - piece.tile.texture.y),
            static_cast<int>(part.width),
            static_cast<int>(part.height)
        };
        void* pixels = nullptr;
        int pitch = 0;
        if (!SDL_LockTexture(piece.texture, &locked, &pixels, &pitch))
            fail("SDL_LockTexture");
        // Converts the texels of a rectangle of the texture from the
        // picture's texels from (from_x, from_y) on.
        const auto convert = [&](const policy::TexelRect& texels,
                                 uint32_t from_x,
                                 uint32_t from_y) {
            if (texels.width == 0)
                return;
            convert_rgb24_xrgb_rect(
                rgb + static_cast<std::size_t>(from_y) * rgb_pitch +
                    static_cast<std::size_t>(from_x) * 3U,
                rgb_pitch,
                texels.width,
                texels.height,
                static_cast<uint8_t*>(pixels) +
                    static_cast<std::size_t>(texels.y - part.y) * static_cast<std::size_t>(pitch) +
                    static_cast<std::size_t>(texels.x - part.x) * 4U,
                static_cast<std::size_t>(pitch),
                gamma,
                pool
            );
        };
        const auto inside = shared(part, corner);
        convert(inside, inside.x, inside.y);
        // The column past the corner from its last column, the row past it
        // from its last row, and the texel where they meet from its last.
        const uint32_t last_column = corner_width - 1;
        const uint32_t last_row = corner_height - 1;
        const auto column = shared(part, {corner_width, 0, 1, corner_height});
        convert(column, last_column, column.y);
        const auto row = shared(part, {0, corner_height, corner_width, 1});
        convert(row, row.x, last_row);
        convert(shared(part, {corner_width, corner_height, 1, 1}), last_column, last_row);
        SDL_UnlockTexture(piece.texture);
    }
}

bool TiledTexture::set_scale_mode(SDL_ScaleMode mode) noexcept {
    bool all = true;
    for (const auto& piece : tiles_)
        all = SDL_SetTextureScaleMode(piece.texture, mode) && all;
    return all;
}

void TiledTexture::draw(
    SDL_Renderer* renderer, const SDL_FRect* source, const SDL_FRect* destination
) const {
    if (tiles_.size() == 1) {
        if (!SDL_RenderTexture(renderer, tiles_.front().texture, source, destination))
            fail("SDL_RenderTexture");
        return;
    }
    const SDL_FRect whole{
        0.0F, 0.0F, static_cast<float>(grid_.width), static_cast<float>(grid_.height)
    };
    const auto texture_at = [this](uint32_t column, uint32_t row) {
        return tile_texture(column, row);
    };
    if (!draw_tiles(
            renderer,
            grid_,
            texture_at,
            source != nullptr ? *source : whole,
            destination != nullptr ? *destination : whole
        ))
        fail("SDL_RenderTexture");
}

void TiledTexture::reset() noexcept {
    for (const auto& piece : tiles_) {
        SDL_DestroyTexture(piece.texture);
        if (counts_ != nullptr)
            ++counts_->textures_destroyed;
    }
    tiles_.clear();
    grid_ = {};
    format_ = SDL_PIXELFORMAT_UNKNOWN;
    blend_ = SDL_BLENDMODE_NONE;
    limit_ = 0;
    counts_ = nullptr;
}

void PrescaleTarget::ensure(
    SDL_Renderer* renderer,
    SDL_Texture* final_target,
    uint32_t width,
    uint32_t height,
    uint32_t limit,
    ScaledWorldCounts& counts
) {
    if (made() && grid_.width == width && grid_.height == height && limit_ == limit)
        return;
    destroy();
    counts_ = &counts;
    grid_ = policy::plan_tiles(width, height, limit);
    limit_ = limit;
    if (grid_.columns == 0 || grid_.rows == 0) {
        grid_ = {};
        throw AccelerationError(
            "a prescale target cannot be split into tiles under the renderer's limit",
            AccelerationFault::engine
        );
    }
    // Each tile is made and checked in turn; a failure destroys those made.
    const auto failed = [&](const char* what) {
        std::string message = std::string(what) + ": " + SDL_GetError();
        std::ignore = SDL_SetRenderTarget(renderer, final_target);
        destroy();
        throw AccelerationError(message);
    };
    uint8_t red = 0, green = 0, blue = 0, alpha = 0;
    if (!SDL_GetRenderDrawColor(renderer, &red, &green, &blue, &alpha))
        failed("SDL_GetRenderDrawColor");
    for (uint32_t row = 0; row < grid_.rows; ++row)
        for (uint32_t column = 0; column < grid_.columns; ++column) {
            const auto tile = policy::tile_at(grid_, column, row);
            SDL_Texture* texture = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_TARGET,
                static_cast<int>(tile.texture.width),
                static_cast<int>(tile.texture.height)
            );
            if (texture == nullptr)
                failed("SDL_CreateTexture of a prescale target");
            tiles_.push_back(texture);
            ++counts.textures_created;
            if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) ||
                !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR))
                failed("SDL prescale target state");
            // A new tile is cleared and read back: a driver that fails
            // silently shows here, before the target is used.
            if (!SDL_SetRenderTarget(renderer, texture) ||
                !SDL_SetRenderDrawColor(
                    renderer,
                    target_check_colour[0],
                    target_check_colour[1],
                    target_check_colour[2],
                    255
                ) ||
                !SDL_RenderClear(renderer))
                failed("SDL clearing a prescale target");
            const SDL_Rect first_pixel{0, 0, 1, 1};
            SDL_Surface* read = SDL_RenderReadPixels(renderer, &first_pixel);
            if (!SDL_SetRenderDrawColor(renderer, red, green, blue, alpha)) {
                SDL_DestroySurface(read);
                failed("SDL_SetRenderDrawColor");
            }
            if (read == nullptr)
                failed("SDL_RenderReadPixels of a prescale target");
            uint8_t read_red = 0, read_green = 0, read_blue = 0, read_alpha = 0;
            const bool readable =
                SDL_ReadSurfacePixel(read, 0, 0, &read_red, &read_green, &read_blue, &read_alpha);
            SDL_DestroySurface(read);
            if (!readable || read_red != target_check_colour[0] ||
                read_green != target_check_colour[1] || read_blue != target_check_colour[2]) {
                std::ignore = SDL_SetRenderTarget(renderer, final_target);
                destroy();
                throw AccelerationError(
                    "a new prescale target read back another colour than it was cleared to"
                );
            }
        }
    if (!SDL_SetRenderTarget(renderer, final_target))
        failed("SDL_SetRenderTarget");
}

void PrescaleTarget::destroy() noexcept {
    for (SDL_Texture* texture : tiles_) {
        SDL_DestroyTexture(texture);
        if (counts_ != nullptr)
            ++counts_->textures_destroyed;
    }
    tiles_.clear();
    grid_ = {};
    limit_ = 0;
    drawn_revision = ~uint64_t{0};
    drawn_factor = 0;
}

SDL_Texture* PrescaleTarget::tile_texture(uint32_t column, uint32_t row) const noexcept {
    if (column >= grid_.columns || row >= grid_.rows)
        return nullptr;
    return tiles_[static_cast<std::size_t>(row) * grid_.columns + column];
}

bool probe_pixelart(SDL_Renderer* renderer, SDL_Texture* final_target) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
    SDL_Texture* source = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, pixelart_probe_source_width, 1
    );
    SDL_Texture* target = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, pixelart_probe_target_width, 1
    );
    const std::array<uint32_t, pixelart_probe_source_width> texels{
        pixelart_probe_black, pixelart_probe_white
    };
    bool works = false;
    uint8_t red = 0, green = 0, blue = 0, alpha = 0;
    if (source != nullptr && target != nullptr &&
        SDL_UpdateTexture(source, nullptr, texels.data(), static_cast<int>(sizeof texels)) &&
        SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE) &&
        SDL_SetTextureScaleMode(source, SDL_SCALEMODE_PIXELART) &&
        SDL_GetRenderDrawColor(renderer, &red, &green, &blue, &alpha) &&
        SDL_SetRenderTarget(renderer, target)) {
        const SDL_FRect drawn{0.0F, 0.0F, static_cast<float>(pixelart_probe_target_width), 1.0F};
        SDL_Surface* read = nullptr;
        if (SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) && SDL_RenderClear(renderer) &&
            SDL_RenderTexture(renderer, source, nullptr, &drawn))
            read = SDL_RenderReadPixels(renderer, nullptr);
        std::ignore = SDL_SetRenderDrawColor(renderer, red, green, blue, alpha);
        if (read != nullptr) {
            uint8_t second = 255, third = 0, unused = 0;
            const bool readable =
                SDL_ReadSurfacePixel(read, 1, 0, &second, &unused, &unused, &unused) &&
                SDL_ReadSurfacePixel(read, 2, 0, &third, &unused, &unused, &unused);
            // LINEAR greys the second pixel, NEAREST leaves the third black or white.
            works = readable && second == 0 && third > 0 && third < 255;
            SDL_DestroySurface(read);
        }
    }
    std::ignore = SDL_SetRenderTarget(renderer, final_target);
    SDL_DestroyTexture(source);
    SDL_DestroyTexture(target);
    return works;
#else
    static_cast<void>(renderer);
    static_cast<void>(final_target);
    return false;
#endif
}

void draw_scaled_world(
    SDL_Renderer* renderer,
    SDL_Texture* final_target,
    TiledTexture& scene,
    uint32_t scene_width,
    uint32_t scene_height,
    uint32_t width,
    uint32_t height,
    const SDL_FRect& destination,
    const SDL_Rect& clip,
    const CardScale& scale,
    PrescaleTarget& prescale,
    ScaledWorldCounts& counts
) {
    const SDL_FRect& landed = destination;
    if (scale.filter == policy::ScaleFilter::sharp_bilinear && scale.factor > 1) {
        const uint32_t factor = scale.factor;
        if (!prescale.made() || prescale.width() < width * factor ||
            prescale.height() < height * factor)
            throw AccelerationError(
                "the prescale target is smaller than the scene it is to hold",
                AccelerationFault::engine
            );
        // The corner and the column and row past it, for LINEAR to read,
        // where the target holds them: a view between map pixels draws one
        // more column and row than the target was sized for at a zoom.
        const uint32_t region_width = std::min({width + 1, scene_width, prescale.width() / factor});
        const uint32_t region_height =
            std::min({height + 1, scene_height, prescale.height() / factor});
        if (!scene.set_scale_mode(SDL_SCALEMODE_NEAREST))
            fail(renderer, final_target, "SDL_SetTextureScaleMode");
        const auto& grid = scene.grid();
        std::vector<SourcePiece> pieces;
        pieces.reserve(std::size_t{grid.columns} * grid.rows);
        for (uint32_t row = 0; row < grid.rows; ++row)
            for (uint32_t column = 0; column < grid.columns; ++column) {
                const auto tile = policy::tile_at(grid, column, row);
                pieces.push_back(
                    {scene.tile_texture(column, row), tile.content, tile.texture.x, tile.texture.y}
                );
            }
        fill_prescale(
            renderer, final_target, prescale, pieces, region_width, region_height, factor
        );
        ++counts.prescale_draws;
        if (!SDL_SetRenderTarget(renderer, final_target) || !SDL_SetRenderClipRect(renderer, &clip))
            fail(renderer, final_target, "SDL_SetRenderTarget");
        const SDL_FRect corner{
            0.0F, 0.0F, static_cast<float>(width * factor), static_cast<float>(height * factor)
        };
        draw_prescaled(renderer, final_target, prescale, corner, landed);
    } else {
        if (!scene.set_scale_mode(direct_scale_mode(scale.filter)))
            fail(renderer, final_target, "SDL_SetTextureScaleMode");
        if (!SDL_SetRenderClipRect(renderer, &clip))
            fail(renderer, final_target, "SDL_SetRenderClipRect");
        const SDL_FRect corner{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
        try {
            scene.draw(renderer, &corner, &landed);
        } catch (const AccelerationError&) {
            std::ignore = SDL_SetRenderClipRect(renderer, nullptr);
            throw;
        }
    }
    if (!SDL_SetRenderClipRect(renderer, nullptr))
        fail(renderer, final_target, "SDL_SetRenderClipRect");
}

void sharp_draw(
    SDL_Renderer* renderer,
    SDL_Texture* final_target,
    SDL_Texture* source,
    uint32_t source_width,
    uint32_t source_height,
    std::span<const SharpPart> parts,
    const CardScale& scale,
    uint64_t revision,
    PrescaleTarget& prescale,
    ScaledWorldCounts& counts
) {
    if (scale.filter != policy::ScaleFilter::sharp_bilinear || scale.factor <= 1) {
        const SDL_ScaleMode mode = direct_scale_mode(scale.filter);
        if (mode != SDL_SCALEMODE_NEAREST && !SDL_SetTextureScaleMode(source, mode))
            fail(renderer, final_target, "SDL_SetTextureScaleMode");
        bool drawn = true;
        for (const auto& part : parts)
            drawn = drawn && SDL_RenderTexture(renderer, source, &part.source, &part.destination);
        // The source is left at NEAREST, as the standard tier draws it, once
        // the draws keep their mode.
        const bool run = mode == SDL_SCALEMODE_NEAREST || !drawn ||
                         run_draws_before_scale_mode_changes(renderer);
        if (mode != SDL_SCALEMODE_NEAREST)
            std::ignore = SDL_SetTextureScaleMode(source, SDL_SCALEMODE_NEAREST);
        if (!drawn)
            fail(renderer, final_target, "SDL_RenderTexture");
        if (!run)
            fail(renderer, final_target, "SDL_FlushRenderer");
        return;
    }
    const uint32_t factor = scale.factor;
    if (!prescale.made() || prescale.width() < source_width * factor ||
        prescale.height() < source_height * factor)
        throw AccelerationError(
            "the prescale target is smaller than the source it is to hold",
            AccelerationFault::engine
        );
    if (prescale.drawn_revision != revision || prescale.drawn_factor != factor) {
        const SourcePiece whole{source, {0, 0, source_width, source_height}, 0, 0};
        fill_prescale(
            renderer, final_target, prescale, {&whole, 1}, source_width, source_height, factor
        );
        ++counts.prescale_draws;
        if (!SDL_SetRenderTarget(renderer, final_target))
            fail(renderer, final_target, "SDL_SetRenderTarget");
        prescale.drawn_revision = revision;
        prescale.drawn_factor = factor;
    }
    const auto n = static_cast<float>(factor);
    for (const auto& part : parts) {
        const SDL_FRect enlarged{
            part.source.x * n, part.source.y * n, part.source.w * n, part.source.h * n
        };
        draw_prescaled(renderer, final_target, prescale, enlarged, part.destination);
    }
}

void software_linear_rgb24(
    const oa::present::world_renderer::RgbSource& source,
    uint32_t factor,
    const SDL_Rect& rectangle,
    const oa::present::world_renderer::RgbTarget& target
) {
    if (source.width == 0 || source.height == 0 || factor == 0 || rectangle.w <= 0 ||
        rectangle.h <= 0)
        return;
    const auto columns =
        software_linear_samples(source.width * factor, static_cast<uint32_t>(rectangle.w));
    const auto rows =
        software_linear_samples(source.height * factor, static_cast<uint32_t>(rectangle.h));
    // A texel of the enlarged picture.
    const auto texel = [&](uint32_t column, uint32_t row, uint32_t channel) {
        return uint32_t{
            source.rgb
                [(std::size_t{row / factor} * source.stride_pixels + column / factor) * 3U +
                 channel]
        };
    };
    for (int32_t j = 0; j < rectangle.h; ++j) {
        const int64_t y = int64_t{rectangle.y} + j;
        if (y < 0 || y >= int64_t{target.height})
            continue;
        const LinearSample& down = rows[static_cast<std::size_t>(j)];
        for (int32_t i = 0; i < rectangle.w; ++i) {
            const int64_t x = int64_t{rectangle.x} + i;
            if (x < 0 || x >= int64_t{target.width})
                continue;
            const LinearSample& across = columns[static_cast<std::size_t>(i)];
            uint8_t* pixel = target.rgb + (static_cast<std::size_t>(y) * target.stride_pixels +
                                           static_cast<std::size_t>(x)) *
                                              3U;
            for (uint32_t channel = 0; channel < 3; ++channel) {
                // Rows first, each column of the pair; then the columns;
                // the sum truncated once.
                const auto vertical = [&](uint32_t column) {
                    return (software_linear_weight_one - down.weight) *
                               texel(column, down.first, channel) +
                           down.weight * texel(column, down.second, channel);
                };
                const uint32_t left = vertical(across.first);
                const uint32_t right = vertical(across.second);
                pixel[channel] = static_cast<uint8_t>(
                    ((software_linear_weight_one - across.weight) * left + across.weight * right) >>
                    (2 * software_linear_weight_bits)
                );
            }
        }
    }
}

} // namespace oa::app
