// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Window-size textures beyond the renderer's texture limit, on SDL's
// software renderer over a surface larger than two 1024-texel tiles each
// way: the same picture drawn as tiles under a limit of 1024 and as one
// texture with no limit reads back pixel for pixel alike in its colour
// channels (the unused byte of the target's pixels, which a blend may leave
// as it likes, is not compared), for an XRGB8888
// upload drawn 1:1 at an offset, ARGB8888 drawn with no blending, an RGBA32
// layer blended over a background with no rectangles given, and part of the
// picture drawn onto the same part of the target. Each tile is filled from the part of the picture
// it holds, so its gutters hold its neighbours' edges. Rectangles of a
// second picture uploaded over the first leave the texture as one upload of
// the picture they make together does, as tiles and as one texture.
//
// The accelerated tier's drawing on the card (scaled_world.hpp), on SDL's
// software renderer over a surface, with no window, read back and compared
// with what that renderer is asked to draw: nearest replication
// (scene_filter.hpp) and the renderer's own LINEAR, modelled on the
// processor (software_linear_rgb24). A seeded scene magnified by
// draw_scaled_world at 1.37 by sharp-bilinear and by LINEAR, and seeded
// sources drawn by sharp_draw at 0.5, 0.6, 0.75 and 1.37, keep within 2
// levels of it, and 0.5 on average; at 2 and 4 both draw NEAREST, equal to
// nearest replication. A view drawn between map pixels, one column and row
// wider and landing before the battlefield's edge, through a prescale
// target made for the view on its camera's map pixel, keeps within them
// too, placed where that renderer places it, from SDL 3.4; before it that
// renderer moves a scaled draw the battlefield clips by up to a texel, and
// such a view is held only to stay inside the battlefield. PIXELART is the
// renderer's NEAREST, as SDL's software renderer gives it and as a build of
// SDL older than 3.4, which lacks the mode, draws it, and the probe finds it
// missing. A scene split into
// tiles holds each neighbour's texels in its gutters and draws as the whole
// texture does. A prescale target split into tiles holds the enlarged
// picture, gutters included, from a scene of one tile or of several, and
// draws what one target draws. The overlay drawn over a picture keeps it
// where transparent and replaces it where opaque. sharp_draw makes no
// prescale target at a whole-number scale and does not draw its source into
// the target again while the source's revision is unchanged. A prescale
// target is checked by its read-back when made.
//
// New textures read back cleared: a streaming texture filled with a colour
// and cleared, and every texture and tile a TiledTexture makes, as one
// texture and as tiles. An upload of a scene's corner writes the column and
// row past it from the corner's last column and row, gutters included, and
// leaves the rest cleared; a prescale target larger than the enlarged
// source, and one that held a larger source first, holds the source's last
// column and row in the column and row past the enlargement, and keeps
// what it held beyond them.
//
// The menus' frame of 640x480 under each way of Menu scaling, in windows of
// the common sizes, one smaller than the frame, and ones of an odd width and
// height: the rectangle it is drawn in is whole, centred, as large as the
// window holds, or in the largest whole step, and the frame read back fills
// exactly that rectangle; a point of the frame placed in the window and
// mapped back is the same point, and the middle of each edge pixel of the
// rectangle maps to the middle of the frame's edge pixel, where SDL reckons
// half a pixel as well. PIXELART, which the standard tier asks for, draws
// the frame as NEAREST on that renderer.
#include "oa/app/scaled_world.hpp"

#include "oa/present/world_renderer/scene_filter.hpp"
#include "oa/test/check.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <vector>

namespace {

/// Window-size textures as tiles beyond the renderer's limit.
namespace layer_tiles {
using oa::app::TiledTexture;

/// The render target, larger than two 1024-texel tiles in each direction.
constexpr int kTargetWidth = 2600;
constexpr int kTargetHeight = 1500;
/// The tiled run's texture limit, in texels.
constexpr uint32_t kTiledLimit = 1024;
/// The picture's size: a window-size layer of the target's size, which
/// drawn at an offset runs off the target's right and bottom edges.
constexpr int kPictureWidth = kTargetWidth;
constexpr int kPictureHeight = kTargetHeight;
/// Where the picture is drawn on the target.
constexpr float kOffsetX = 128.0F;
constexpr float kOffsetY = 32.0F;
/// Bytes of one pixel of the 32-bit formats.
constexpr int kPixelBytes = 4;
/// Seed of the pictures.
constexpr uint32_t kSeed = 2600;

/// A picture of random 32-bit pixels.
std::vector<uint8_t> random_picture(uint32_t seed) {
    std::mt19937 random(seed);
    std::vector<uint8_t> picture(static_cast<std::size_t>(kPictureWidth) * kPictureHeight * 4U);
    for (auto& byte : picture)
        byte = static_cast<uint8_t>(random());
    return picture;
}

/// SDL's software renderer over its own surface.
struct SoftwareTarget {
    SDL_Surface* surface{};
    SDL_Renderer* renderer{};

    SoftwareTarget() {
        surface = SDL_CreateSurface(kTargetWidth, kTargetHeight, SDL_PIXELFORMAT_XRGB8888);
        if (surface != nullptr)
            renderer = SDL_CreateSoftwareRenderer(surface);
    }

    SoftwareTarget(const SoftwareTarget&) = delete;
    SoftwareTarget& operator=(const SoftwareTarget&) = delete;

    ~SoftwareTarget() {
        if (renderer != nullptr)
            SDL_DestroyRenderer(renderer);
        if (surface != nullptr)
            SDL_DestroySurface(surface);
    }

    /// Clears the target to a colour of its own.
    void clear() const {
        OA_CHECK(SDL_SetRenderDrawColor(renderer, 20, 40, 60, 255));
        OA_CHECK(SDL_RenderClear(renderer));
    }

    /// Reads the target's colour channels back, 3 bytes a pixel: the unused
    /// byte of its XRGB8888 pixels is left out, since a blend may leave it
    /// as it likes.
    std::vector<uint8_t> read_back() const {
        std::vector<uint8_t> pixels;
        SDL_Surface* read = SDL_RenderReadPixels(renderer, nullptr);
        OA_CHECK(read != nullptr);
        if (read == nullptr)
            return pixels;
        SDL_Surface* rgb = SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGB24);
        SDL_DestroySurface(read);
        OA_CHECK(rgb != nullptr);
        if (rgb == nullptr)
            return pixels;
        const auto row_bytes = static_cast<std::size_t>(rgb->w) * 3U;
        pixels.resize(row_bytes * static_cast<std::size_t>(rgb->h));
        for (int row = 0; row < rgb->h; ++row)
            std::memcpy(
                pixels.data() + static_cast<std::size_t>(row) * row_bytes,
                static_cast<const uint8_t*>(rgb->pixels) +
                    static_cast<std::ptrdiff_t>(row) * rgb->pitch,
                row_bytes
            );
        SDL_DestroySurface(rgb);
        return pixels;
    }
};

/// What one way of drawing a picture does.
enum class Drawing : uint8_t {
    locked_offset,  ///< filled through locked tiles, drawn 1:1 at an offset
    updated_argb,   ///< updated as ARGB8888, drawn with no blending
    blended_layer,  ///< an RGBA32 layer blended over a background
    part_onto_part, ///< part of the picture drawn onto the same part of the target
};

/// Draws a picture one way, under a texture limit, and reads the target back.
///
/// @param drawing the way
/// @param limit the texture limit; 0 for none
/// @param[out] tiles how many textures held the picture
/// @return the target's pixels
std::vector<uint8_t> draw(Drawing drawing, uint32_t limit, std::size_t& tiles) {
    SoftwareTarget target;
    OA_CHECK(target.renderer != nullptr);
    if (target.renderer == nullptr)
        return {};
    const auto picture = random_picture(kSeed + static_cast<uint32_t>(drawing));
    const int picture_pitch = kPictureWidth * kPixelBytes;
    target.clear();
    TiledTexture texture;
    const SDL_FRect at{
        kOffsetX, kOffsetY, static_cast<float>(kPictureWidth), static_cast<float>(kPictureHeight)
    };
    switch (drawing) {
    case Drawing::locked_offset: {
        OA_CHECK(texture.ensure(
            target.renderer,
            SDL_PIXELFORMAT_XRGB8888,
            kPictureWidth,
            kPictureHeight,
            limit,
            SDL_BLENDMODE_NONE
        ));
        texture.upload([&](const SDL_Rect& source, uint8_t* pixels, int pitch) {
            for (int row = 0; row < source.h; ++row)
                std::memcpy(
                    pixels + static_cast<std::ptrdiff_t>(row) * pitch,
                    picture.data() + static_cast<std::ptrdiff_t>(source.y + row) * picture_pitch +
                        static_cast<std::ptrdiff_t>(source.x) * kPixelBytes,
                    static_cast<std::size_t>(source.w) * kPixelBytes
                );
        });
        texture.draw(target.renderer, nullptr, &at);
        break;
    }
    case Drawing::updated_argb:
        OA_CHECK(texture.ensure(
            target.renderer,
            SDL_PIXELFORMAT_ARGB8888,
            kPictureWidth,
            kPictureHeight,
            limit,
            SDL_BLENDMODE_NONE
        ));
        texture.update(picture.data(), picture_pitch, kPixelBytes);
        texture.draw(target.renderer, nullptr, &at);
        break;
    case Drawing::blended_layer: {
        // A background of its own, as one texture in both runs, then the
        // layer blended over the whole target.
        const auto background = random_picture(kSeed);
        TiledTexture under;
        OA_CHECK(under.ensure(
            target.renderer,
            SDL_PIXELFORMAT_XRGB8888,
            kPictureWidth,
            kPictureHeight,
            0,
            SDL_BLENDMODE_NONE
        ));
        under.update(background.data(), picture_pitch, kPixelBytes);
        under.draw(target.renderer, nullptr, nullptr);
        OA_CHECK(texture.ensure(
            target.renderer,
            SDL_PIXELFORMAT_RGBA32,
            kPictureWidth,
            kPictureHeight,
            limit,
            SDL_BLENDMODE_BLEND
        ));
        texture.update(picture.data(), picture_pitch, kPixelBytes);
        texture.draw(target.renderer, nullptr, nullptr);
        break;
    }
    case Drawing::part_onto_part: {
        OA_CHECK(texture.ensure(
            target.renderer,
            SDL_PIXELFORMAT_ARGB8888,
            kPictureWidth,
            kPictureHeight,
            limit,
            SDL_BLENDMODE_NONE
        ));
        texture.update(picture.data(), picture_pitch, kPixelBytes);
        // A part that crosses a tile's edge in each direction.
        const SDL_FRect part{1000.0F, 1000.0F, 700.0F, 250.0F};
        texture.draw(target.renderer, &part, &part);
        break;
    }
    }
    tiles = texture.tile_count();
    return target.read_back();
}

void tiles_draw_as_one_texture() {
    for (const Drawing drawing :
         {Drawing::locked_offset,
          Drawing::updated_argb,
          Drawing::blended_layer,
          Drawing::part_onto_part}) {
        std::size_t whole_tiles = 0;
        std::size_t split_tiles = 0;
        const auto whole = draw(drawing, 0, whole_tiles);
        const auto split = draw(drawing, kTiledLimit, split_tiles);
        OA_CHECK(whole_tiles == 1);
        OA_CHECK(split_tiles > 1);
        OA_CHECK(!whole.empty());
        OA_CHECK(whole == split);
    }
}

void gutters_hold_neighbours_edges() {
    SoftwareTarget target;
    OA_CHECK(target.renderer != nullptr);
    if (target.renderer == nullptr)
        return;
    TiledTexture texture;
    OA_CHECK(texture.ensure(
        target.renderer,
        SDL_PIXELFORMAT_XRGB8888,
        kPictureWidth,
        kPictureHeight,
        kTiledLimit,
        SDL_BLENDMODE_NONE
    ));
    std::vector<SDL_Rect> sources;
    texture.upload([&](const SDL_Rect& source, uint8_t*, int) { sources.push_back(source); });
    OA_CHECK(sources.size() == texture.tile_count());
    // Every pair of neighbours overlaps by two texels, a gutter on each side
    // of their seam, so each gutter is filled with its neighbour's edge.
    std::size_t seams = 0;
    for (const auto& first : sources)
        for (const auto& second : sources) {
            if (first.y == second.y && first.h == second.h && second.x > first.x &&
                second.x < first.x + first.w) {
                OA_CHECK(first.x + first.w - second.x == 2);
                ++seams;
            }
            if (first.x == second.x && first.w == second.w && second.y > first.y &&
                second.y < first.y + first.h) {
                OA_CHECK(first.y + first.h - second.y == 2);
                ++seams;
            }
        }
    OA_CHECK(seams > 0);
    // Every tile within the limit, and a picture within it in one texture.
    for (const auto& source : sources)
        OA_CHECK(
            source.w <= static_cast<int>(kTiledLimit) && source.h <= static_cast<int>(kTiledLimit)
        );
    TiledTexture small;
    OA_CHECK(small.ensure(
        target.renderer, SDL_PIXELFORMAT_XRGB8888, 640, 480, kTiledLimit, SDL_BLENDMODE_NONE
    ));
    OA_CHECK(small.tile_count() == 1 && small.single() != nullptr);
    // The same request makes nothing again; another size does.
    OA_CHECK(!small.ensure(
        target.renderer, SDL_PIXELFORMAT_XRGB8888, 640, 480, kTiledLimit, SDL_BLENDMODE_NONE
    ));
    OA_CHECK(small.ensure(
        target.renderer, SDL_PIXELFORMAT_XRGB8888, 800, 600, kTiledLimit, SDL_BLENDMODE_NONE
    ));
    small.reset();
    OA_CHECK(small.tile_count() == 0 && small.width() == 0);
}

void alpha_layers_keep_their_blending() {
    SoftwareTarget target;
    OA_CHECK(target.renderer != nullptr);
    if (target.renderer == nullptr)
        return;
    TiledTexture opaque;
    OA_CHECK(
        opaque.ensure(target.renderer, SDL_PIXELFORMAT_ARGB8888, 64, 64, 0, SDL_BLENDMODE_NONE)
    );
    SDL_BlendMode mode = SDL_BLENDMODE_INVALID;
    OA_CHECK(SDL_GetTextureBlendMode(opaque.single(), &mode) && mode == SDL_BLENDMODE_NONE);
    TiledTexture layer;
    OA_CHECK(layer.ensure(target.renderer, SDL_PIXELFORMAT_RGBA32, 64, 64, 0, SDL_BLENDMODE_BLEND));
    OA_CHECK(SDL_GetTextureBlendMode(layer.single(), &mode) && mode == SDL_BLENDMODE_BLEND);
    SDL_ScaleMode scale = SDL_SCALEMODE_LINEAR;
    OA_CHECK(SDL_GetTextureScaleMode(layer.single(), &scale) && scale == SDL_SCALEMODE_NEAREST);
}

/// Draws a texture uploaded whole from one picture, then rectangles of a
/// second, under a texture limit, and reads the target back.
///
/// @param first the picture uploaded whole
/// @param second the picture the rectangles are taken from; empty for none
/// @param rects the rectangles, in the pictures' pixels
/// @param limit the texture limit; 0 for none
/// @param[out] tiles how many textures held the picture
/// @return the target's pixels
std::vector<uint8_t> draw_updated(
    const std::vector<uint8_t>& first,
    const std::vector<uint8_t>& second,
    std::span<const oa::app::render_policy::TexelRect> rects,
    uint32_t limit,
    std::size_t& tiles
) {
    SoftwareTarget target;
    OA_CHECK(target.renderer != nullptr);
    if (target.renderer == nullptr)
        return {};
    target.clear();
    const int picture_pitch = kPictureWidth * kPixelBytes;
    TiledTexture texture;
    OA_CHECK(texture.ensure(
        target.renderer,
        SDL_PIXELFORMAT_ARGB8888,
        kPictureWidth,
        kPictureHeight,
        limit,
        SDL_BLENDMODE_NONE
    ));
    texture.update(first.data(), picture_pitch, kPixelBytes);
    for (const auto& rect : rects)
        texture.update_rect(
            second.data() + (static_cast<std::size_t>(rect.y) * kPictureWidth + rect.x) *
                                static_cast<std::size_t>(kPixelBytes),
            picture_pitch,
            kPixelBytes,
            rect
        );
    texture.draw(target.renderer, nullptr, nullptr);
    tiles = texture.tile_count();
    return target.read_back();
}

/// Checks that rectangles uploaded over a whole picture, as the match's OA
/// layer uploads the part it composed again, leave the texture as one
/// upload of the picture they make together does: inside a tile, across a
/// seam each way, along a whole row and at the bottom right corner, as
/// tiles under the limit and as one texture.
void rects_update_as_the_picture_they_make() {
    const auto first = random_picture(kSeed + 10U);
    const auto second = random_picture(kSeed + 11U);
    const std::array<oa::app::render_policy::TexelRect, 5> rects{{
        {10, 20, 300, 200},
        {1000, 990, 60, 50},
        {900, 100, 1300, 40},
        {0, 700, static_cast<uint32_t>(kPictureWidth), 3},
        {2400, 1300, 200, 200},
    }};
    std::vector<uint8_t> together = first;
    const auto row_bytes = static_cast<std::size_t>(kPictureWidth) * kPixelBytes;
    for (const auto& rect : rects)
        for (uint32_t row = rect.y; row < rect.y + rect.height; ++row) {
            const std::size_t at = row * row_bytes + std::size_t{rect.x} * kPixelBytes;
            std::memcpy(
                together.data() + at, second.data() + at, std::size_t{rect.width} * kPixelBytes
            );
        }
    for (const uint32_t limit : {0U, kTiledLimit}) {
        std::size_t updated_tiles = 0;
        std::size_t whole_tiles = 0;
        const auto updated = draw_updated(first, second, rects, limit, updated_tiles);
        const auto whole = draw_updated(together, {}, {}, limit, whole_tiles);
        OA_CHECK(updated_tiles == whole_tiles);
        OA_CHECK(limit == 0 ? whole_tiles == 1 : whole_tiles > 1);
        OA_CHECK(!updated.empty());
        OA_CHECK(updated == whole);
    }
}

} // namespace layer_tiles

/// The accelerated tier's drawing on the card.
namespace card {
namespace wr = oa::present::world_renderer;
using oa::app::CardScale;
using oa::app::render_policy::ScaleFilter;

/// The surface the renderer draws on, and the battlefield on it.
constexpr int surface_width = 400;
constexpr int surface_height = 300;
constexpr SDL_Rect battlefield{10, 12, 300, 200};

/// The scene's texture: room for the scene at every zoom drawn here.
constexpr uint32_t scene_texture_width = 320;
constexpr uint32_t scene_texture_height = 240;

/// Seeds of the scene and the sources.
constexpr uint32_t seed_scene = 0x243F6A88U;
constexpr uint32_t seed_source = 0x85A308D3U;
constexpr uint32_t seed_overlay = 0x13198A2EU;

/// Most a channel of the renderer's picture may differ from the reference,
/// and the most the mean difference may be. SDL's software renderer draws
/// LINEAR exactly as software_linear_rgb24 models it on a processor with
/// SSE2 or NEON; a build of it without either truncates after each of its
/// two passes and can give one level less.
constexpr int most_difference = 2;
constexpr double most_mean_difference = 0.5;

/// The first SDL release whose software renderer draws a scaled texture
/// whose destination the clip rectangle cuts with the pixels the whole draw
/// gives inside the clip. An older one scales the part of the source it
/// works out for the clipped destination, rounded to whole texels, which
/// moves the picture by up to a texel; on it a view drawn between map
/// pixels, which starts before the battlefield, is held only to stay inside
/// the battlefield.
constexpr int clipped_draw_keeps_sampling_version = SDL_VERSIONNUM(3, 4, 0);

/// The tile limit the tiled cases plan with: tiles of 64 texels, gutters included.
constexpr uint32_t small_limit = 64;

/// The tiled prescale cases: a target split at its 65th texel across and
/// down (tiles of 66 texels, gutters included), whose edge lands on a whole
/// pixel at a reduction by 0.8, where the software renderer, which places
/// each draw at whole pixels, steps through each tile as through the whole
/// target; and a scene split in two by tiles of 40 texels.
constexpr uint32_t prescale_tile_limit = 66;
constexpr uint32_t scene_tile_limit = 40;
/// The tiled scene's texture, and the corner of it magnified 1.6 times,
/// by a factor of 2, to whole pixels.
constexpr uint32_t tiled_scene_edge = 64;
constexpr uint32_t tiled_corner_edge = 50;
constexpr int tiled_corner_landed = 80;
/// The source sharp_draw enlarges 2.4 times, by a factor of 3, through a
/// tiled target whose tile edge cuts a block of 3 texels.
constexpr uint32_t tiled_source_edge = 40;
constexpr int tiled_source_landed = 96;

/// A 32-bit xorshift sequence, the same on every platform.
struct Random {
    uint32_t state{};

    /// Returns the next number of the sequence.
    ///
    /// @return 32 bits
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
};

/// RGB pixels, rows one after another.
struct Picture {
    uint32_t width{};
    uint32_t height{};
    std::vector<uint8_t> rgb;

    /// Returns the pixels as a scene.
    ///
    /// @return a read-only view
    [[nodiscard]] wr::RgbSource source() const { return {rgb.data(), width, height, width}; }

    /// Returns the top-left corner of the pixels as a scene.
    ///
    /// @param columns the corner's columns
    /// @param rows the corner's rows
    /// @return a read-only view
    [[nodiscard]] wr::RgbSource corner(uint32_t columns, uint32_t rows) const {
        return {rgb.data(), columns, rows, width};
    }

    /// Returns the pixels as a picture.
    ///
    /// @return a writable view
    [[nodiscard]] wr::RgbTarget target() { return {rgb.data(), width, height, width}; }
};

/// Returns a picture of seeded levels.
///
/// @param seed the sequence's seed
/// @param width columns
/// @param height rows
/// @return the picture
Picture seeded(uint32_t seed, uint32_t width, uint32_t height) {
    Random random{seed};
    Picture picture{width, height, std::vector<uint8_t>(std::size_t{width} * height * 3U)};
    for (auto& byte : picture.rgb)
        byte = static_cast<uint8_t>(random.next() >> 24);
    return picture;
}

/// The software renderer over a surface, made for each case.
struct Canvas {
    SDL_Surface* surface{};
    SDL_Renderer* renderer{};

    Canvas() {
        surface = SDL_CreateSurface(surface_width, surface_height, SDL_PIXELFORMAT_XRGB8888);
        renderer = surface != nullptr ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        OA_CHECK(renderer != nullptr);
    }

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    ~Canvas() {
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }

    /// Clears the surface to black.
    void clear() const {
        OA_CHECK(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255));
        OA_CHECK(SDL_RenderClear(renderer));
    }

    /// Reads the surface back.
    ///
    /// @return its pixels
    [[nodiscard]] Picture read() const {
        Picture picture{surface_width, surface_height, {}};
        SDL_Surface* read = SDL_RenderReadPixels(renderer, nullptr);
        SDL_Surface* rgb =
            read != nullptr ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGB24) : nullptr;
        SDL_DestroySurface(read);
        OA_CHECK(rgb != nullptr);
        if (rgb == nullptr)
            return picture;
        picture.rgb.resize(std::size_t{surface_width} * surface_height * 3U);
        for (int row = 0; row < surface_height; ++row)
            std::memcpy(
                picture.rgb.data() + static_cast<std::size_t>(row) * surface_width * 3U,
                static_cast<const uint8_t*>(rgb->pixels) +
                    static_cast<std::ptrdiff_t>(row) * rgb->pitch,
                std::size_t{surface_width} * 3U
            );
        SDL_DestroySurface(rgb);
        return picture;
    }
};

/// How far a read-back is from a reference over a rectangle.
struct Difference {
    int most{};
    double mean{};
};

/// Compares a read-back with a reference over a rectangle of both.
///
/// @param read the read-back
/// @param reference the reference, the same size
/// @param area the rectangle compared
/// @return the largest and the mean difference of a channel
Difference compare(const Picture& read, const Picture& reference, const SDL_Rect& area) {
    Difference difference;
    double sum = 0.0;
    std::size_t count = 0;
    for (int y = area.y; y < area.y + area.h; ++y)
        for (int x = area.x; x < area.x + area.w; ++x)
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto at = (static_cast<std::size_t>(y) * read.width + x) * 3U + channel;
                const int delta = std::abs(int{read.rgb[at]} - int{reference.rgb[at]});
                difference.most = std::max(difference.most, delta);
                sum += delta;
                ++count;
            }
    difference.mean = count != 0 ? sum / static_cast<double>(count) : 0.0;
    return difference;
}

/// Says whether a read-back is black outside the battlefield, as the clip
/// leaves it.
///
/// @param read the read-back of the whole surface
/// @return true when every channel outside the battlefield is 0
bool black_outside_battlefield(const Picture& read) {
    for (int y = 0; y < surface_height; ++y)
        for (int x = 0; x < surface_width; ++x) {
            if (x >= battlefield.x && x < battlefield.x + battlefield.w && y >= battlefield.y &&
                y < battlefield.y + battlefield.h)
                continue;
            const auto at = (static_cast<std::size_t>(y) * surface_width + x) * 3U;
            if (read.rgb[at] != 0 || read.rgb[at + 1] != 0 || read.rgb[at + 2] != 0)
                return false;
        }
    return true;
}

/// The corner of the scene a zoom shows on the battlefield, and where it lands.
struct Corner {
    uint32_t width{};
    uint32_t height{};
    SDL_Rect destination{};

    /// Returns where the corner lands as the card is given it.
    ///
    /// @return the destination in the final target's coordinates
    [[nodiscard]] SDL_FRect landed() const {
        return {
            static_cast<float>(destination.x),
            static_cast<float>(destination.y),
            static_cast<float>(destination.w),
            static_cast<float>(destination.h)
        };
    }
};

/// Returns the corner a zoom shows: the battlefield's map pixels rounded up,
/// landing from the battlefield's corner at that many times the zoom, rounded.
///
/// @param zoom screen pixels per scene pixel
/// @return the corner
Corner corner_at(double zoom) {
    Corner corner;
    corner.width = static_cast<uint32_t>(std::ceil(battlefield.w / zoom));
    corner.height = static_cast<uint32_t>(std::ceil(battlefield.h / zoom));
    corner.destination = {
        battlefield.x,
        battlefield.y,
        static_cast<int>(std::lround(corner.width * zoom)),
        static_cast<int>(std::lround(corner.height * zoom))
    };
    return corner;
}

/// Returns the reference of a corner drawn into the surface: black, and the
/// filter's reference within the battlefield.
///
/// @param scene the scene
/// @param corner the corner and where it lands
/// @param filter the reference's filter
/// @param factor the prescale factor for sharp-bilinear
/// @return the reference picture of the surface
Picture
reference_of(const Picture& scene, const Corner& corner, ScaleFilter filter, uint32_t factor) {
    Picture reference{
        surface_width,
        surface_height,
        std::vector<uint8_t>(std::size_t{surface_width} * surface_height * 3U, 0)
    };
    Picture drawn{surface_width, surface_height, reference.rgb};
    const wr::ScenePlacement placement{
        static_cast<double>(corner.destination.w) / corner.width,
        static_cast<double>(corner.destination.h) / corner.height,
        static_cast<double>(corner.destination.x),
        static_cast<double>(corner.destination.y)
    };
    const auto source = scene.corner(corner.width, corner.height);
    switch (filter) {
    case ScaleFilter::nearest:
    case ScaleFilter::pixelart:
        wr::nearest_rgb24(source, placement, drawn.target());
        break;
    case ScaleFilter::linear:
        oa::app::software_linear_rgb24(source, 1, corner.destination, drawn.target());
        break;
    case ScaleFilter::sharp_bilinear:
        oa::app::software_linear_rgb24(source, factor, corner.destination, drawn.target());
        break;
    }
    for (int y = battlefield.y; y < battlefield.y + battlefield.h; ++y)
        std::memcpy(
            reference.rgb.data() +
                (static_cast<std::size_t>(y) * surface_width + battlefield.x) * 3U,
            drawn.rgb.data() + (static_cast<std::size_t>(y) * surface_width + battlefield.x) * 3U,
            static_cast<std::size_t>(battlefield.w) * 3U
        );
    return reference;
}

/// Uploads the scene into a tiled texture of the largest size.
///
/// @param canvas the renderer
/// @param scene the scene
/// @param limit the texture limit to tile at
/// @param counts the counts the texture adds to
/// @param[out] texture the texture
void upload_scene(
    const Canvas& canvas,
    const Picture& scene,
    uint32_t limit,
    oa::app::ScaledWorldCounts& counts,
    oa::app::TiledTexture& texture
) {
    texture.create(
        canvas.renderer,
        scene_texture_width,
        scene_texture_height,
        limit,
        SDL_BLENDMODE_NONE,
        counts
    );
    texture.upload_rgb24(
        scene.rgb.data(), scene.width * 3U, scene.width, scene.height, nullptr, nullptr
    );
}

/// Magnifies a seeded scene into the battlefield by each filter and zoom
/// and compares the read-back with the references.
void test_draw_scaled_world_matches_the_references() {
    const Picture scene = seeded(seed_scene, 240, 160);

    struct Case {
        double zoom{};
        CardScale scale{};
    };

    const Case cases[] = {
        {1.37, {ScaleFilter::sharp_bilinear, 2}},
        {1.37, {ScaleFilter::linear, 1}},
        {2.0, {ScaleFilter::nearest, 1}},
        {4.0, {ScaleFilter::nearest, 1}},
    };
    for (const auto& item : cases) {
        Canvas canvas;
        oa::app::ScaledWorldCounts counts;
        oa::app::TiledTexture texture;
        oa::app::PrescaleTarget prescale;
        upload_scene(canvas, scene, 0, counts, texture);
        const Corner corner = corner_at(item.zoom);
        if (item.scale.filter == ScaleFilter::sharp_bilinear)
            prescale.ensure(
                canvas.renderer,
                nullptr,
                (corner.width + 1) * item.scale.factor,
                (corner.height + 1) * item.scale.factor,
                0,
                counts
            );
        canvas.clear();
        oa::app::draw_scaled_world(
            canvas.renderer,
            nullptr,
            texture,
            scene.width,
            scene.height,
            corner.width,
            corner.height,
            corner.landed(),
            battlefield,
            item.scale,
            prescale,
            counts
        );
        const Picture read = canvas.read();
        const Picture reference = reference_of(scene, corner, item.scale.filter, item.scale.factor);
        // The whole surface: outside the battlefield the clip leaves black.
        const SDL_Rect whole{0, 0, surface_width, surface_height};
        const auto difference = compare(read, reference, whole);
        std::printf(
            "draw_scaled_world at %.2f by filter %d: most %d, mean %.3f\n",
            item.zoom,
            static_cast<int>(item.scale.filter),
            difference.most,
            difference.mean
        );
        if (item.scale.filter == ScaleFilter::nearest)
            OA_CHECK(difference.most == 0);
        OA_CHECK(difference.most <= most_difference);
        OA_CHECK(difference.mean <= most_mean_difference);
        OA_CHECK(
            counts.prescale_draws == (item.scale.filter == ScaleFilter::sharp_bilinear ? 1U : 0U)
        );
    }
}

/// Magnifies the corner of a view drawn between map pixels: one more column
/// and row than the view on its camera's map pixel shows, landing its
/// offset times the zoom before the battlefield's edge, through a prescale
/// target made only for that view's corner; and compares the read-back with
/// the references placed where SDL's software renderer places the corner,
/// at the whole pixel it truncates the destination to. Before SDL 3.4 that
/// renderer moves the clipped draw (clipped_draw_keeps_sampling_version),
/// and the read-back is held only to stay inside the battlefield.
void test_view_between_map_pixels() {
    const Picture scene = seeded(seed_scene, 240, 160);

    struct Case {
        double zoom{};
        double offset{}; ///< map pixels past the camera's
        CardScale scale{};
    };

    const Case cases[] = {
        {1.37, 0.5, {ScaleFilter::sharp_bilinear, 2}},
        {1.37, 0.9, {ScaleFilter::linear, 1}},
        {2.0, 0.5, {ScaleFilter::nearest, 1}},
        {4.0, 0.75, {ScaleFilter::nearest, 1}},
    };
    const bool held_to_reference = SDL_GetVersion() >= clipped_draw_keeps_sampling_version;
    for (const auto& item : cases) {
        Canvas canvas;
        oa::app::ScaledWorldCounts counts;
        oa::app::TiledTexture texture;
        oa::app::PrescaleTarget prescale;
        upload_scene(canvas, scene, 0, counts, texture);
        const Corner whole = corner_at(item.zoom);
        if (item.scale.filter == ScaleFilter::sharp_bilinear)
            prescale.ensure(
                canvas.renderer,
                nullptr,
                (whole.width + 1) * item.scale.factor,
                (whole.height + 1) * item.scale.factor,
                0,
                counts
            );
        Corner corner;
        corner.width = static_cast<uint32_t>(std::ceil(battlefield.w / item.zoom + item.offset));
        corner.height = static_cast<uint32_t>(std::ceil(battlefield.h / item.zoom + item.offset));
        OA_CHECK(corner.width == whole.width + 1 || corner.width == whole.width);
        const SDL_FRect landed{
            static_cast<float>(battlefield.x - item.offset * item.zoom),
            static_cast<float>(battlefield.y - item.offset * item.zoom),
            static_cast<float>(std::lround(corner.width * item.zoom)),
            static_cast<float>(std::lround(corner.height * item.zoom))
        };
        corner.destination = {
            static_cast<int>(landed.x),
            static_cast<int>(landed.y),
            static_cast<int>(landed.w),
            static_cast<int>(landed.h)
        };
        canvas.clear();
        oa::app::draw_scaled_world(
            canvas.renderer,
            nullptr,
            texture,
            scene.width,
            scene.height,
            corner.width,
            corner.height,
            landed,
            battlefield,
            item.scale,
            prescale,
            counts
        );
        const Picture read = canvas.read();
        const Picture reference = reference_of(scene, corner, item.scale.filter, item.scale.factor);
        const SDL_Rect surface{0, 0, surface_width, surface_height};
        const auto difference = compare(read, reference, surface);
        std::printf(
            "a view %.2f map pixels on at %.2f by filter %d: most %d, mean %.3f%s\n",
            item.offset,
            item.zoom,
            static_cast<int>(item.scale.filter),
            difference.most,
            difference.mean,
            held_to_reference ? "" : " (not held to the reference before SDL 3.4)"
        );
        OA_CHECK(black_outside_battlefield(read));
        if (!held_to_reference)
            continue;
        if (item.scale.filter == ScaleFilter::nearest)
            OA_CHECK(difference.most == 0);
        OA_CHECK(difference.most <= most_difference);
        OA_CHECK(difference.mean <= most_mean_difference);
    }
}

/// Draws a seeded source by sharp_draw at each scale, through the prescale
/// target where the scale is not whole and above 1, and compares.
void test_sharp_draw_matches_the_references() {
    const Picture source = seeded(seed_source, 160, 120);
    const double scales[] = {0.5, 0.6, 0.75, 1.37, 2.0, 4.0};
    for (const double scale : scales) {
        Canvas canvas;
        oa::app::ScaledWorldCounts counts;
        oa::app::PrescaleTarget prescale;
        SDL_Texture* texture = SDL_CreateTexture(
            canvas.renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 160, 120
        );
        OA_CHECK(texture != nullptr);
        OA_CHECK(SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST));
        OA_CHECK(SDL_UpdateTexture(texture, nullptr, source.rgb.data(), 160 * 3));
        const bool whole = std::floor(scale) == scale;
        const auto factor = static_cast<uint32_t>(std::ceil(scale));
        const CardScale card{
            whole ? ScaleFilter::nearest
                  : (factor > 1 ? ScaleFilter::sharp_bilinear : ScaleFilter::linear),
            whole ? 1U : factor
        };
        if (card.filter == ScaleFilter::sharp_bilinear)
            prescale.ensure(canvas.renderer, nullptr, 160 * factor, 120 * factor, 0, counts);
        const int width = static_cast<int>(std::lround(160 * scale));
        const int height = static_cast<int>(std::lround(120 * scale));
        const Corner corner{
            160,
            120,
            {7, 5, std::min(width, surface_width - 7), std::min(height, surface_height - 5)}
        };
        // Draw the source whole, clipped by the surface.
        const oa::app::SharpPart part{
            {0.0F, 0.0F, 160.0F, 120.0F},
            {7.0F, 5.0F, static_cast<float>(width), static_cast<float>(height)}
        };
        canvas.clear();
        oa::app::sharp_draw(
            canvas.renderer, nullptr, texture, 160, 120, {&part, 1}, card, 1, prescale, counts
        );
        const Picture read = canvas.read();
        Picture reference{
            surface_width,
            surface_height,
            std::vector<uint8_t>(std::size_t{surface_width} * surface_height * 3U, 0)
        };
        const wr::ScenePlacement placement{
            static_cast<double>(width) / 160.0, static_cast<double>(height) / 120.0, 7.0, 5.0
        };
        Picture drawn = reference;
        const SDL_Rect landed{7, 5, width, height};
        switch (card.filter) {
        case ScaleFilter::nearest:
            wr::nearest_rgb24(source.source(), placement, drawn.target());
            break;
        case ScaleFilter::linear:
            oa::app::software_linear_rgb24(source.source(), 1, landed, drawn.target());
            break;
        default:
            oa::app::software_linear_rgb24(source.source(), factor, landed, drawn.target());
            break;
        }
        for (int y = 5; y < 5 + corner.destination.h; ++y)
            std::memcpy(
                reference.rgb.data() + (static_cast<std::size_t>(y) * surface_width + 7) * 3U,
                drawn.rgb.data() + (static_cast<std::size_t>(y) * surface_width + 7) * 3U,
                static_cast<std::size_t>(corner.destination.w) * 3U
            );
        const auto difference = compare(read, reference, {0, 0, surface_width, surface_height});
        std::printf(
            "sharp_draw at %.2f: most %d, mean %.3f\n", scale, difference.most, difference.mean
        );
        if (whole)
            OA_CHECK(difference.most == 0);
        OA_CHECK(difference.most <= most_difference);
        OA_CHECK(difference.mean <= most_mean_difference);
        // A whole-number scale makes no prescale target and draws none.
        if (whole)
            OA_CHECK(!prescale.made() && counts.prescale_draws == 0);
        // The source left NEAREST, as the standard tier draws it.
        SDL_ScaleMode mode = SDL_SCALEMODE_LINEAR;
        OA_CHECK(SDL_GetTextureScaleMode(texture, &mode) && mode == SDL_SCALEMODE_NEAREST);
        SDL_DestroyTexture(texture);
    }
}

/// Checks that sharp_draw draws its source into the prescale target only
/// when the source's revision or the factor changes.
void test_sharp_draw_redraws_only_a_new_revision() {
    Canvas canvas;
    const Picture source = seeded(seed_source, 64, 48);
    oa::app::ScaledWorldCounts counts;
    oa::app::PrescaleTarget prescale;
    SDL_Texture* texture = SDL_CreateTexture(
        canvas.renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 64, 48
    );
    OA_CHECK(texture != nullptr && SDL_UpdateTexture(texture, nullptr, source.rgb.data(), 64 * 3));
    prescale.ensure(canvas.renderer, nullptr, 64 * 3, 48 * 3, 0, counts);
    OA_CHECK(counts.textures_created == 1);
    const oa::app::SharpPart part{{0.0F, 0.0F, 64.0F, 48.0F}, {0.0F, 0.0F, 160.0F, 120.0F}};
    const CardScale card{ScaleFilter::sharp_bilinear, 3};
    for (const uint64_t revision : {5U, 5U, 5U, 6U, 6U})
        oa::app::sharp_draw(
            canvas.renderer, nullptr, texture, 64, 48, {&part, 1}, card, revision, prescale, counts
        );
    OA_CHECK(counts.prescale_draws == 2);
    // A smaller factor within the target draws again.
    oa::app::sharp_draw(
        canvas.renderer,
        nullptr,
        texture,
        64,
        48,
        {&part, 1},
        {ScaleFilter::sharp_bilinear, 2},
        6,
        prescale,
        counts
    );
    OA_CHECK(counts.prescale_draws == 3);
    // Asking for the target at its size again makes nothing new.
    prescale.ensure(canvas.renderer, nullptr, 64 * 3, 48 * 3, 0, counts);
    OA_CHECK(counts.textures_created == 1 && counts.textures_destroyed == 0);
    prescale.destroy();
    OA_CHECK(counts.textures_destroyed == 1);
    SDL_DestroyTexture(texture);
}

/// Checks that PIXELART draws as NEAREST on SDL's software renderer, which
/// lacks the filter, as it does in a build of SDL older than 3.4, which
/// lacks the mode, and that the probe finds it missing there.
void test_pixelart_is_nearest_here() {
    Canvas canvas;
    OA_CHECK(!oa::app::probe_pixelart(canvas.renderer, nullptr));
    const Picture scene = seeded(seed_scene, 240, 160);
    const Corner corner = corner_at(1.37);
    std::vector<Picture> reads;
    for (const auto filter : {ScaleFilter::pixelart, ScaleFilter::nearest}) {
        oa::app::ScaledWorldCounts counts;
        oa::app::TiledTexture texture;
        oa::app::PrescaleTarget prescale;
        upload_scene(canvas, scene, 0, counts, texture);
        canvas.clear();
        oa::app::draw_scaled_world(
            canvas.renderer,
            nullptr,
            texture,
            scene.width,
            scene.height,
            corner.width,
            corner.height,
            corner.landed(),
            battlefield,
            {filter, 1},
            prescale,
            counts
        );
        reads.push_back(canvas.read());
    }
    OA_CHECK(reads[0].rgb == reads[1].rgb);
}

/// Checks a scene split into tiles: each tile holds the texels of its part
/// of the scene, gutters included, and the tiles draw what the whole
/// texture draws, at 1 and at 2.
void test_tiles_hold_their_gutters() {
    const Picture scene = seeded(seed_scene, 240, 160);
    Canvas canvas;
    oa::app::ScaledWorldCounts counts;
    oa::app::TiledTexture tiled;
    upload_scene(canvas, scene, small_limit, counts, tiled);
    const auto& grid = tiled.grid();
    OA_CHECK(grid.columns > 1 && grid.rows > 1);
    OA_CHECK(counts.textures_created == uint64_t{grid.columns} * grid.rows);
    // Each tile drawn alone 1:1 shows the scene's texels its texture rectangle covers.
    bool gutters = true;
    for (uint32_t row = 0; row < grid.rows; ++row)
        for (uint32_t column = 0; column < grid.columns; ++column) {
            const auto tile = oa::app::render_policy::tile_at(grid, column, row);
            if (tile.texture.x + tile.texture.width > scene.width ||
                tile.texture.y + tile.texture.height > scene.height)
                continue;
            canvas.clear();
            const SDL_FRect whole{
                0.0F,
                0.0F,
                static_cast<float>(tile.texture.width),
                static_cast<float>(tile.texture.height)
            };
            OA_CHECK(
                SDL_RenderTexture(canvas.renderer, tiled.tile_texture(column, row), nullptr, &whole)
            );
            const Picture read = canvas.read();
            for (uint32_t y = 0; y < tile.texture.height; ++y)
                gutters =
                    gutters &&
                    std::memcmp(
                        read.rgb.data() + std::size_t{y} * surface_width * 3U,
                        scene.rgb.data() +
                            ((std::size_t{tile.texture.y} + y) * scene.width + tile.texture.x) * 3U,
                        std::size_t{tile.texture.width} * 3U
                    ) == 0;
        }
    OA_CHECK(gutters);
    oa::app::TiledTexture whole;
    upload_scene(canvas, scene, 0, counts, whole);
    OA_CHECK(whole.grid().columns == 1 && whole.grid().rows == 1);
    for (const double zoom : {1.0, 2.0}) {
        const Corner corner = corner_at(zoom);
        std::vector<Picture> reads;
        for (auto* texture : {&tiled, &whole}) {
            oa::app::PrescaleTarget prescale;
            canvas.clear();
            oa::app::draw_scaled_world(
                canvas.renderer,
                nullptr,
                *texture,
                scene.width,
                scene.height,
                corner.width,
                corner.height,
                corner.landed(),
                battlefield,
                {ScaleFilter::nearest, 1},
                prescale,
                counts
            );
            reads.push_back(canvas.read());
        }
        OA_CHECK(reads[0].rgb == reads[1].rgb);
    }
    const uint64_t made = counts.textures_created;
    tiled.reset();
    whole.reset();
    OA_CHECK(counts.textures_destroyed == made);
}

/// Reads back what a target texture holds.
///
/// @param canvas the renderer
/// @param texture a render target of it
/// @param width the target's texels across
/// @param height its texels down
/// @return its pixels
Picture read_target(const Canvas& canvas, SDL_Texture* texture, uint32_t width, uint32_t height) {
    Picture picture{width, height, std::vector<uint8_t>(std::size_t{width} * height * 3U, 0)};
    OA_CHECK(SDL_SetRenderTarget(canvas.renderer, texture));
    SDL_Surface* read = SDL_RenderReadPixels(canvas.renderer, nullptr);
    SDL_Surface* rgb = read != nullptr ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGB24) : nullptr;
    SDL_DestroySurface(read);
    OA_CHECK(SDL_SetRenderTarget(canvas.renderer, nullptr));
    OA_CHECK(
        rgb != nullptr && rgb->w == static_cast<int>(width) && rgb->h == static_cast<int>(height)
    );
    if (rgb == nullptr || rgb->w != static_cast<int>(width) || rgb->h != static_cast<int>(height)) {
        SDL_DestroySurface(rgb);
        return picture;
    }
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(
            picture.rgb.data() + std::size_t{row} * width * 3U,
            static_cast<const uint8_t*>(rgb->pixels) +
                static_cast<std::ptrdiff_t>(row) * rgb->pitch,
            std::size_t{width} * 3U
        );
    SDL_DestroySurface(rgb);
    return picture;
}

/// A colour no picture drawn here is left at, with which a surface is
/// filled before a texture that should hold black is drawn over it.
constexpr std::array<uint8_t, 3> under_colour{90, 60, 30};

/// Fills a canvas's surface with under_colour.
///
/// @param canvas the canvas
void fill_under(const Canvas& canvas) {
    OA_CHECK(SDL_SetRenderDrawColor(
        canvas.renderer, under_colour[0], under_colour[1], under_colour[2], 255
    ));
    OA_CHECK(SDL_RenderClear(canvas.renderer));
}

/// Says whether every pixel of a rectangle of a read-back is black.
///
/// @param read the read-back
/// @param area the rectangle
/// @return true when every channel there is 0
bool black_in(const Picture& read, const SDL_Rect& area) {
    for (int y = area.y; y < area.y + area.h; ++y)
        for (int x = area.x; x < area.x + area.w; ++x)
            for (std::size_t channel = 0; channel < 3; ++channel)
                if (read.rgb[(static_cast<std::size_t>(y) * read.width + x) * 3U + channel] != 0)
                    return false;
    return true;
}

/// Checks that new textures read back cleared: a streaming texture filled
/// with a colour and then cleared, and each texture a TiledTexture makes by
/// ensure and by create, as one texture and as tiles, drawn 1:1 with no
/// blending over a surface of another colour.
void test_new_textures_read_back_cleared() {
    Canvas canvas;
    constexpr int width = 50;
    constexpr int height = 40;
    SDL_Texture* filled = SDL_CreateTexture(
        canvas.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height
    );
    OA_CHECK(filled != nullptr && SDL_SetTextureBlendMode(filled, SDL_BLENDMODE_NONE));
    void* pixels = nullptr;
    int pitch = 0;
    OA_CHECK(SDL_LockTexture(filled, nullptr, &pixels, &pitch));
    if (pixels != nullptr)
        std::memset(pixels, 0xA7, static_cast<std::size_t>(pitch) * height);
    SDL_UnlockTexture(filled);
    OA_CHECK(oa::app::clear_streaming_texture(filled));
    fill_under(canvas);
    const SDL_FRect at{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
    OA_CHECK(SDL_RenderTexture(canvas.renderer, filled, nullptr, &at));
    OA_CHECK(black_in(canvas.read(), {0, 0, width, height}));
    SDL_DestroyTexture(filled);
    OA_CHECK(!oa::app::clear_streaming_texture(nullptr));

    const SDL_FRect whole{
        0.0F,
        0.0F,
        static_cast<float>(scene_texture_width),
        static_cast<float>(scene_texture_height)
    };
    const SDL_Rect area{
        0, 0, static_cast<int>(scene_texture_width), static_cast<int>(scene_texture_height)
    };
    for (const uint32_t limit : {0U, small_limit}) {
        oa::app::ScaledWorldCounts counts;
        oa::app::TiledTexture made;
        OA_CHECK(made.ensure(
            canvas.renderer,
            SDL_PIXELFORMAT_XRGB8888,
            static_cast<int>(scene_texture_width),
            static_cast<int>(scene_texture_height),
            limit,
            SDL_BLENDMODE_NONE
        ));
        fill_under(canvas);
        made.draw(canvas.renderer, nullptr, &whole);
        OA_CHECK(black_in(canvas.read(), area));
        made.create(
            canvas.renderer,
            scene_texture_width,
            scene_texture_height,
            limit,
            SDL_BLENDMODE_NONE,
            counts
        );
        OA_CHECK((limit == 0) == (made.tile_count() == 1));
        fill_under(canvas);
        made.draw(canvas.renderer, nullptr, &whole);
        OA_CHECK(black_in(canvas.read(), area));
    }
}

/// Checks that an upload of a scene's corner into a larger texture writes
/// the column and row past the corner from the corner's last column and
/// row, the corner's last texel where they meet, and leaves every texel
/// beyond them cleared: in each tile drawn alone 1:1, gutters included, as
/// one texture and as tiles whose edges the corner and the texels past it
/// cross.
void test_upload_fills_past_the_corner() {
    const Picture scene = seeded(seed_scene, 240, 160);
    constexpr uint32_t corner_width = 124;
    constexpr uint32_t corner_height = 62;
    // The texel a LINEAR draw of the corner reads at a texel of the texture.
    const auto expected = [&](uint32_t x, uint32_t y) {
        std::array<uint8_t, 3> texel{};
        if (x > corner_width || y > corner_height)
            return texel;
        const uint8_t* at =
            scene.rgb.data() + (std::size_t{std::min(y, corner_height - 1)} * scene.width +
                                std::min(x, corner_width - 1)) *
                                   3U;
        std::copy_n(at, 3, texel.begin());
        return texel;
    };
    for (const uint32_t limit : {0U, small_limit}) {
        Canvas canvas;
        oa::app::ScaledWorldCounts counts;
        oa::app::TiledTexture texture;
        texture.create(
            canvas.renderer,
            scene_texture_width,
            scene_texture_height,
            limit,
            SDL_BLENDMODE_NONE,
            counts
        );
        texture.upload_rgb24(
            scene.rgb.data(), scene.width * 3U, corner_width, corner_height, nullptr, nullptr
        );
        const auto& grid = texture.grid();
        OA_CHECK((limit == 0) == (grid.columns == 1 && grid.rows == 1));
        bool held = true;
        for (uint32_t row = 0; row < grid.rows; ++row)
            for (uint32_t column = 0; column < grid.columns; ++column) {
                const auto tile = oa::app::render_policy::tile_at(grid, column, row);
                if (tile.texture.width > static_cast<uint32_t>(surface_width) ||
                    tile.texture.height > static_cast<uint32_t>(surface_height))
                    continue;
                fill_under(canvas);
                const SDL_FRect alone{
                    0.0F,
                    0.0F,
                    static_cast<float>(tile.texture.width),
                    static_cast<float>(tile.texture.height)
                };
                OA_CHECK(SDL_RenderTexture(
                    canvas.renderer, texture.tile_texture(column, row), nullptr, &alone
                ));
                const Picture read = canvas.read();
                for (uint32_t y = 0; y < tile.texture.height; ++y)
                    for (uint32_t x = 0; x < tile.texture.width; ++x) {
                        const auto want = expected(tile.texture.x + x, tile.texture.y + y);
                        held = held && std::memcmp(
                                           read.rgb.data() + (std::size_t{y} * read.width + x) * 3U,
                                           want.data(),
                                           3
                                       ) == 0;
                    }
            }
        OA_CHECK(held);
    }
}

/// Checks that a prescale target larger than the source enlarged into it,
/// as the HUD's is once a taller page has made it larger, holds the
/// source's last column and row in the column and row past the
/// enlargement, the source's last texel where they meet, after a larger
/// source was drawn into it first; and that every texel beyond them keeps
/// what the larger source left. As one target and in tiles.
void test_prescale_fills_past_a_smaller_source() {
    constexpr uint32_t factor = 3;
    const Picture larger = seeded(seed_overlay, 50, 40);
    const Picture smaller = seeded(seed_source, 40, 30);
    constexpr uint32_t target_width = 160;
    constexpr uint32_t target_height = 130;
    const uint32_t filled_width = smaller.width * factor;
    const uint32_t filled_height = smaller.height * factor;
    for (const uint32_t limit : {0U, prescale_tile_limit}) {
        Canvas canvas;
        oa::app::ScaledWorldCounts counts;
        oa::app::PrescaleTarget prescale;
        prescale.ensure(canvas.renderer, nullptr, target_width, target_height, limit, counts);
        const auto& grid = prescale.grid();
        OA_CHECK((limit == 0) == (grid.columns == 1 && grid.rows == 1));
        const CardScale card{ScaleFilter::sharp_bilinear, factor};
        // Each source drawn into the target by sharp_draw, a new revision each.
        uint64_t revision = 0;
        const auto draw_source = [&](const Picture& source) {
            SDL_Texture* texture = SDL_CreateTexture(
                canvas.renderer,
                SDL_PIXELFORMAT_RGB24,
                SDL_TEXTUREACCESS_STREAMING,
                static_cast<int>(source.width),
                static_cast<int>(source.height)
            );
            OA_CHECK(
                texture != nullptr && SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST) &&
                SDL_UpdateTexture(
                    texture, nullptr, source.rgb.data(), static_cast<int>(source.width) * 3
                )
            );
            const oa::app::SharpPart part{
                {0.0F, 0.0F, static_cast<float>(source.width), static_cast<float>(source.height)},
                {0.0F,
                 0.0F,
                 static_cast<float>(source.width) * 2.5F,
                 static_cast<float>(source.height) * 2.5F}
            };
            oa::app::sharp_draw(
                canvas.renderer,
                nullptr,
                texture,
                source.width,
                source.height,
                {&part, 1},
                card,
                ++revision,
                prescale,
                counts
            );
            SDL_DestroyTexture(texture);
        };
        draw_source(larger);
        std::vector<Picture> before;
        for (uint32_t row = 0; row < grid.rows; ++row)
            for (uint32_t column = 0; column < grid.columns; ++column) {
                const auto tile = oa::app::render_policy::tile_at(grid, column, row);
                before.push_back(read_target(
                    canvas,
                    prescale.tile_texture(column, row),
                    tile.texture.width,
                    tile.texture.height
                ));
            }
        draw_source(smaller);
        OA_CHECK(counts.prescale_draws == 2);
        bool held = true;
        for (uint32_t row = 0; row < grid.rows; ++row)
            for (uint32_t column = 0; column < grid.columns; ++column) {
                const auto tile = oa::app::render_policy::tile_at(grid, column, row);
                const Picture read = read_target(
                    canvas,
                    prescale.tile_texture(column, row),
                    tile.texture.width,
                    tile.texture.height
                );
                const Picture& was = before[std::size_t{row} * grid.columns + column];
                for (uint32_t y = 0; y < tile.texture.height; ++y)
                    for (uint32_t x = 0; x < tile.texture.width; ++x) {
                        const uint32_t texel_x = tile.texture.x + x;
                        const uint32_t texel_y = tile.texture.y + y;
                        const std::size_t at = (std::size_t{y} * read.width + x) * 3U;
                        const uint8_t* want = was.rgb.data() + at;
                        if (texel_x <= filled_width && texel_y <= filled_height)
                            want = smaller.rgb.data() +
                                   (std::size_t{std::min(texel_y / factor, smaller.height - 1)} *
                                        smaller.width +
                                    std::min(texel_x / factor, smaller.width - 1)) *
                                       3U;
                        held = held && std::memcmp(read.rgb.data() + at, want, 3) == 0;
                    }
            }
        OA_CHECK(held);
    }
}

/// Checks that each tile of a prescale target holds the corner of a
/// picture enlarged by nearest replication, gutters included.
///
/// @param canvas the renderer
/// @param prescale the target, its corner filled
/// @param picture the picture
/// @param factor target texels per picture texel
/// @param filled the target's texels filled across and down
void check_prescale_tiles(
    const Canvas& canvas,
    const oa::app::PrescaleTarget& prescale,
    const Picture& picture,
    uint32_t factor,
    uint32_t filled
) {
    const auto& grid = prescale.grid();
    bool held = true;
    for (uint32_t row = 0; row < grid.rows; ++row)
        for (uint32_t column = 0; column < grid.columns; ++column) {
            const auto tile = oa::app::render_policy::tile_at(grid, column, row);
            const Picture read = read_target(
                canvas, prescale.tile_texture(column, row), tile.texture.width, tile.texture.height
            );
            for (uint32_t y = 0; y < tile.texture.height; ++y)
                for (uint32_t x = 0; x < tile.texture.width; ++x) {
                    const uint32_t texel_x = tile.texture.x + x;
                    const uint32_t texel_y = tile.texture.y + y;
                    if (texel_x >= filled || texel_y >= filled)
                        continue;
                    held = held &&
                           std::memcmp(
                               read.rgb.data() + (std::size_t{y} * read.width + x) * 3U,
                               picture.rgb.data() + (std::size_t{texel_y / factor} * picture.width +
                                                     texel_x / factor) *
                                                        3U,
                               3
                           ) == 0;
                }
        }
    OA_CHECK(held);
}

/// Checks prescale targets split into tiles: a seeded scene, itself split
/// into tiles, magnified 1.6 times by sharp-bilinear through a target of
/// tiles holds the enlarged scene in every tile and draws what one target
/// draws; and a seeded source drawn 2.4 times by sharp_draw, whose tile
/// edges cut a block of the enlargement, likewise. Both are the renderer's
/// own LINEAR of the enlarged picture.
void test_prescale_tiles_draw_as_one_target() {
    {
        const Picture scene = seeded(seed_scene, tiled_scene_edge, tiled_scene_edge);
        const Corner corner{
            tiled_corner_edge,
            tiled_corner_edge,
            {battlefield.x, battlefield.y, tiled_corner_landed, tiled_corner_landed}
        };
        constexpr uint32_t factor = 2;
        const uint32_t filled = (tiled_corner_edge + 1) * factor;
        std::vector<Picture> reads;
        for (const uint32_t limit : {prescale_tile_limit, 0U}) {
            Canvas canvas;
            oa::app::ScaledWorldCounts counts;
            oa::app::TiledTexture texture;
            texture.create(
                canvas.renderer,
                tiled_scene_edge,
                tiled_scene_edge,
                scene_tile_limit,
                SDL_BLENDMODE_NONE,
                counts
            );
            OA_CHECK(texture.grid().columns == 2 && texture.grid().rows == 2);
            texture.upload_rgb24(
                scene.rgb.data(), scene.width * 3U, scene.width, scene.height, nullptr, nullptr
            );
            oa::app::PrescaleTarget prescale;
            prescale.ensure(canvas.renderer, nullptr, filled, filled, limit, counts);
            const uint32_t tiles = limit == 0 ? 1U : 2U;
            OA_CHECK(prescale.grid().columns == tiles && prescale.grid().rows == tiles);
            canvas.clear();
            oa::app::draw_scaled_world(
                canvas.renderer,
                nullptr,
                texture,
                scene.width,
                scene.height,
                corner.width,
                corner.height,
                corner.landed(),
                battlefield,
                {ScaleFilter::sharp_bilinear, factor},
                prescale,
                counts
            );
            OA_CHECK(counts.prescale_draws == 1);
            reads.push_back(canvas.read());
            check_prescale_tiles(canvas, prescale, scene, factor, filled);
        }
        OA_CHECK(reads[0].rgb == reads[1].rgb);
        const auto difference = compare(
            reads[0],
            reference_of(scene, corner, ScaleFilter::sharp_bilinear, factor),
            {0, 0, surface_width, surface_height}
        );
        std::printf(
            "draw_scaled_world through prescale tiles: most %d, mean %.3f\n",
            difference.most,
            difference.mean
        );
        OA_CHECK(difference.most <= most_difference);
        OA_CHECK(difference.mean <= most_mean_difference);
    }
    {
        const Picture source = seeded(seed_source, tiled_source_edge, tiled_source_edge);
        constexpr uint32_t factor = 3;
        const uint32_t filled = tiled_source_edge * factor;
        const SDL_Rect landed{7, 5, tiled_source_landed, tiled_source_landed};
        const oa::app::SharpPart part{
            {0.0F,
             0.0F,
             static_cast<float>(tiled_source_edge),
             static_cast<float>(tiled_source_edge)},
            {static_cast<float>(landed.x),
             static_cast<float>(landed.y),
             static_cast<float>(landed.w),
             static_cast<float>(landed.h)}
        };
        std::vector<Picture> reads;
        for (const uint32_t limit : {prescale_tile_limit, 0U}) {
            Canvas canvas;
            oa::app::ScaledWorldCounts counts;
            SDL_Texture* texture = SDL_CreateTexture(
                canvas.renderer,
                SDL_PIXELFORMAT_RGB24,
                SDL_TEXTUREACCESS_STREAMING,
                static_cast<int>(tiled_source_edge),
                static_cast<int>(tiled_source_edge)
            );
            OA_CHECK(texture != nullptr);
            OA_CHECK(SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST));
            OA_CHECK(SDL_UpdateTexture(
                texture, nullptr, source.rgb.data(), static_cast<int>(tiled_source_edge * 3U)
            ));
            oa::app::PrescaleTarget prescale;
            prescale.ensure(canvas.renderer, nullptr, filled, filled, limit, counts);
            const uint32_t tiles = limit == 0 ? 1U : 2U;
            OA_CHECK(prescale.grid().columns == tiles && prescale.grid().rows == tiles);
            canvas.clear();
            oa::app::sharp_draw(
                canvas.renderer,
                nullptr,
                texture,
                tiled_source_edge,
                tiled_source_edge,
                {&part, 1},
                {ScaleFilter::sharp_bilinear, factor},
                1,
                prescale,
                counts
            );
            reads.push_back(canvas.read());
            check_prescale_tiles(canvas, prescale, source, factor, filled);
            SDL_DestroyTexture(texture);
        }
        OA_CHECK(reads[0].rgb == reads[1].rgb);
        Picture reference{
            surface_width,
            surface_height,
            std::vector<uint8_t>(std::size_t{surface_width} * surface_height * 3U, 0)
        };
        oa::app::software_linear_rgb24(source.source(), factor, landed, reference.target());
        const auto difference = compare(reads[0], reference, {0, 0, surface_width, surface_height});
        std::printf(
            "sharp_draw through prescale tiles: most %d, mean %.3f\n",
            difference.most,
            difference.mean
        );
        OA_CHECK(difference.most <= most_difference);
        OA_CHECK(difference.mean <= most_mean_difference);
    }
}

/// Checks the overlay drawn 1:1 with blending over a picture: it replaces
/// the picture where opaque and leaves it where transparent, as the overlay
/// rule says; also split into tiles.
void test_overlay_over_a_picture() {
    const Picture picture = seeded(seed_scene, battlefield.w, battlefield.h);
    Random random{seed_overlay};
    std::vector<uint32_t> overlay(
        std::size_t{static_cast<uint32_t>(battlefield.w)} * battlefield.h, 0
    );
    for (auto& word : overlay)
        if (random.next() % 7U == 0)
            word = 0xFF000000U | (random.next() & 0xFFFFFFU);
    Picture expected = picture;
    wr::overlay_rgb24(expected.target(), overlay.data(), static_cast<uint32_t>(battlefield.w));
    for (const uint32_t limit : {0U, small_limit}) {
        Canvas canvas;
        oa::app::ScaledWorldCounts counts;
        oa::app::TiledTexture under;
        under.create(canvas.renderer, battlefield.w, battlefield.h, 0, SDL_BLENDMODE_NONE, counts);
        under.upload_rgb24(
            picture.rgb.data(), picture.width * 3U, picture.width, picture.height, nullptr, nullptr
        );
        oa::app::TiledTexture over;
        over.create(
            canvas.renderer, battlefield.w, battlefield.h, limit, SDL_BLENDMODE_BLEND, counts
        );
        // Uploaded in two runs of rows, as dirty bands are.
        over.update(reinterpret_cast<const uint8_t*>(overlay.data()), battlefield.w * 4, 4, 0, 64);
        over.update(
            reinterpret_cast<const uint8_t*>(overlay.data()),
            battlefield.w * 4,
            4,
            64,
            static_cast<uint32_t>(battlefield.h)
        );
        canvas.clear();
        const SDL_FRect landed{
            static_cast<float>(battlefield.x),
            static_cast<float>(battlefield.y),
            static_cast<float>(battlefield.w),
            static_cast<float>(battlefield.h)
        };
        under.draw(canvas.renderer, nullptr, &landed);
        over.draw(canvas.renderer, nullptr, &landed);
        const Picture read = canvas.read();
        bool same = true;
        for (int y = 0; y < battlefield.h; ++y)
            same =
                same && std::memcmp(
                            read.rgb.data() +
                                ((static_cast<std::size_t>(y) + battlefield.y) * surface_width +
                                 battlefield.x) *
                                    3U,
                            expected.rgb.data() + static_cast<std::size_t>(y) * battlefield.w * 3U,
                            static_cast<std::size_t>(battlefield.w) * 3U
                        ) == 0;
        OA_CHECK(same);
    }
}

} // namespace card

/// The menus' frame under each way of Menu scaling.
namespace frame_presentation {
using oa::app::render_policy::MenuScaling;

/// The frame's size, in its own pixels.
constexpr int kFrameWidth = 640;
constexpr int kFrameHeight = 480;
/// How close a point mapped there and back stays to itself, in frame pixels.
constexpr float kMappingTolerance = 0.002F;

/// A window size, and the largest whole step of the frame it holds; 0 for
/// one smaller than the frame.
struct Window {
    int width{};
    int height{};
    int whole_step{};
};

/// A software renderer over a surface of a window's size.
struct Output {
    SDL_Surface* surface{};
    SDL_Renderer* renderer{};

    /// Makes the surface and its renderer.
    ///
    /// @param width the surface's width
    /// @param height the surface's height
    Output(int width, int height) {
        surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_XRGB8888);
        if (surface != nullptr)
            renderer = SDL_CreateSoftwareRenderer(surface);
    }

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    ~Output() {
        if (renderer != nullptr)
            SDL_DestroyRenderer(renderer);
        if (surface != nullptr)
            SDL_DestroySurface(surface);
    }
};

/// Returns the rectangle of the output's pixels that are not black, read
/// from its surface: SDL's own read-back covers the letterbox alone.
///
/// @param output the output, drawn
/// @return the bounding rectangle; empty when every pixel is black
SDL_Rect lit_rectangle(const Output& output) {
    OA_CHECK(SDL_FlushRenderer(output.renderer));
    SDL_Surface* const read = output.surface;
    int left = read->w;
    int top = read->h;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < read->h; ++y)
        for (int x = 0; x < read->w; ++x) {
            uint8_t red = 0, green = 0, blue = 0;
            if (!SDL_ReadSurfacePixel(read, x, y, &red, &green, &blue, nullptr) ||
                (red | green | blue) == 0)
                continue;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    if (right < 0)
        return {};
    return {left, top, right - left + 1, bottom - top + 1};
}

/// Maps a window point into the frame through a mouse press, as the game
/// converts its events.
///
/// @param renderer the renderer
/// @param x the window column
/// @param y the window row
/// @return the frame point
SDL_FPoint frame_point(SDL_Renderer* renderer, float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.x = x;
    event.button.y = y;
    OA_CHECK(oa::app::convert_event_to_frame(renderer, event));
    return {event.button.x, event.button.y};
}

void test_each_way_in_each_window() {
    const Window windows[] = {
        {640, 480, 1},
        {800, 600, 1},
        {1280, 800, 1},
        {1366, 768, 1},
        {1920, 1080, 2},
        {2000, 1250, 2},
        {600, 400, 0},
        {1001, 625, 1},
        {1000, 625, 1},
        {1001, 751, 1},
    };
    // A white frame, drawn over a black output.
    std::vector<uint8_t> white(static_cast<std::size_t>(kFrameWidth) * kFrameHeight * 3U, 255);
    int halves = 0;
    for (const Window& window : windows)
        for (const MenuScaling scaling :
             {MenuScaling::sharp, MenuScaling::whole_steps, MenuScaling::unfiltered}) {
            Output output(window.width, window.height);
            OA_CHECK(output.renderer != nullptr);
            if (output.renderer == nullptr)
                continue;
            OA_CHECK(
                oa::app::set_frame_presentation(output.renderer, scaling, kFrameWidth, kFrameHeight)
            );
            // As after a frame's present: SDL before 3.4 places the draws
            // queued before the next run of its queue where the presentation
            // was before.
            OA_CHECK(SDL_FlushRenderer(output.renderer));
            SDL_FRect reckoned{};
            OA_CHECK(SDL_GetRenderLogicalPresentationRect(output.renderer, &reckoned));
            if (reckoned.x != std::floor(reckoned.x) || reckoned.y != std::floor(reckoned.y))
                ++halves;
            const SDL_Rect drawn = oa::app::drawn_frame_rect(reckoned);
            // Whole steps where they are asked for and the window holds the
            // frame; otherwise as large as the window holds, filling it
            // across or down at the frame's shape.
            if (scaling == MenuScaling::whole_steps && window.whole_step > 0) {
                OA_CHECK(drawn.w == kFrameWidth * window.whole_step);
                OA_CHECK(drawn.h == kFrameHeight * window.whole_step);
            } else {
                OA_CHECK(
                    (drawn.w == window.width && drawn.h <= window.height) ||
                    (drawn.h == window.height && drawn.w <= window.width)
                );
                OA_CHECK(std::abs(drawn.w * kFrameHeight - drawn.h * kFrameWidth) < kFrameWidth);
            }
            // Centred on whole pixels: the margins differ by a pixel at most.
            OA_CHECK(drawn.x == (window.width - drawn.w) / 2);
            OA_CHECK(drawn.y == (window.height - drawn.h) / 2);

            // The frame read back fills that rectangle exactly, drawn NEAREST
            // and as the standard tier asks for PIXELART alike.
            SDL_Texture* frame = SDL_CreateTexture(
                output.renderer,
                SDL_PIXELFORMAT_RGB24,
                SDL_TEXTUREACCESS_STATIC,
                kFrameWidth,
                kFrameHeight
            );
            OA_CHECK(frame != nullptr);
            if (frame == nullptr)
                continue;
            OA_CHECK(SDL_UpdateTexture(frame, nullptr, white.data(), kFrameWidth * 3));
            OA_CHECK(SDL_SetTextureScaleMode(frame, SDL_SCALEMODE_NEAREST));
            for (const auto filter :
                 {oa::app::render_policy::ScaleFilter::nearest,
                  oa::app::render_policy::ScaleFilter::pixelart}) {
                OA_CHECK(SDL_SetRenderDrawColor(output.renderer, 0, 0, 0, 255));
                OA_CHECK(SDL_RenderClear(output.renderer));
                oa::app::draw_frame(output.renderer, frame, oa::app::direct_scale_mode(filter));
                const SDL_Rect lit = lit_rectangle(output);
                const bool same =
                    lit.x == drawn.x && lit.y == drawn.y && lit.w == drawn.w && lit.h == drawn.h;
                if (!same)
                    std::fprintf(
                        stderr,
                        "frame in %dx%d: drawn %d,%d %dx%d, read back %d,%d %dx%d\n",
                        window.width,
                        window.height,
                        drawn.x,
                        drawn.y,
                        drawn.w,
                        drawn.h,
                        lit.x,
                        lit.y,
                        lit.w,
                        lit.h
                    );
                OA_CHECK(same);
            }
            SDL_DestroyTexture(frame);

            // A point of the frame placed in the window maps back to itself.
            for (const float x : {0.5F, 1.5F, 319.5F, 638.5F, 639.5F})
                for (const float y : {0.5F, 239.5F, 479.5F}) {
                    float window_x = 0.0F;
                    float window_y = 0.0F;
                    OA_CHECK(oa::app::frame_to_window(output.renderer, x, y, &window_x, &window_y));
                    const SDL_FPoint back = frame_point(output.renderer, window_x, window_y);
                    OA_CHECK(std::abs(back.x - x) < kMappingTolerance);
                    OA_CHECK(std::abs(back.y - y) < kMappingTolerance);
                }
            // The middle of each edge pixel of the rectangle lies on the
            // frame's edge pixel, at the place the drawn frame shows it.
            const auto expected = [](int pixel, int origin, int drawn_size, int frame_size) {
                return (static_cast<float>(pixel - origin) + 0.5F) *
                       static_cast<float>(frame_size) / static_cast<float>(drawn_size);
            };
            const int last_x = drawn.x + drawn.w - 1;
            const int last_y = drawn.y + drawn.h - 1;
            const SDL_FPoint first = frame_point(
                output.renderer,
                static_cast<float>(drawn.x) + 0.5F,
                static_cast<float>(drawn.y) + 0.5F
            );
            const SDL_FPoint last = frame_point(
                output.renderer,
                static_cast<float>(last_x) + 0.5F,
                static_cast<float>(last_y) + 0.5F
            );
            OA_CHECK(
                std::abs(first.x - expected(drawn.x, drawn.x, drawn.w, kFrameWidth)) <
                kMappingTolerance
            );
            OA_CHECK(
                std::abs(first.y - expected(drawn.y, drawn.y, drawn.h, kFrameHeight)) <
                kMappingTolerance
            );
            OA_CHECK(
                std::abs(last.x - expected(last_x, drawn.x, drawn.w, kFrameWidth)) <
                kMappingTolerance
            );
            OA_CHECK(
                std::abs(last.y - expected(last_y, drawn.y, drawn.h, kFrameHeight)) <
                kMappingTolerance
            );
            OA_CHECK(static_cast<int>(std::floor(first.x)) == 0);
            OA_CHECK(static_cast<int>(std::floor(first.y)) == 0);
            OA_CHECK(static_cast<int>(std::floor(last.x)) == kFrameWidth - 1);
            OA_CHECK(static_cast<int>(std::floor(last.y)) == kFrameHeight - 1);
        }
    // Some of these windows leave SDL's own reckoning half a pixel off.
    OA_CHECK(halves > 0);
}

} // namespace frame_presentation

} // namespace

int main() {
    layer_tiles::tiles_draw_as_one_texture();
    layer_tiles::gutters_hold_neighbours_edges();
    layer_tiles::alpha_layers_keep_their_blending();
    layer_tiles::rects_update_as_the_picture_they_make();
    if (!SDL_Init(0)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    card::test_draw_scaled_world_matches_the_references();
    card::test_view_between_map_pixels();
    card::test_sharp_draw_matches_the_references();
    card::test_sharp_draw_redraws_only_a_new_revision();
    card::test_pixelart_is_nearest_here();
    card::test_tiles_hold_their_gutters();
    card::test_prescale_tiles_draw_as_one_target();
    card::test_new_textures_read_back_cleared();
    card::test_upload_fills_past_the_corner();
    card::test_prescale_fills_past_a_smaller_source();
    card::test_overlay_over_a_picture();
    frame_presentation::test_each_way_in_each_window();
    SDL_Quit();
    const int status = oa::test::check_exit_status();
    if (status == 0)
        std::puts("scaled world: tiles beyond the limit draw as one texture would");
    return status;
}
