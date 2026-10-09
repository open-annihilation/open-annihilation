// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's sprite stage (runtime_full.hpp) on SDL's software
// renderer over a surface: one list of sprites, blended sprites, particle
// squares, lines and selection lines drawn by the card from the sprite
// pages and by the bands on the processor (world_draws.hpp). The card's
// picture equals the processor's exactly where sprites are opaque, at
// zooms 1 and 2, and within 2 levels where the alpha table blends them;
// the lines cover the game's lines and stray no further than their width;
// the squares are exact; the stage keeps the list's order and merges its
// batches; a sprite under a cell out of sight is drawn greyed and one
// under a never-mapped cell left out; a feature's frame is cut off where
// the map the view shows ends; malformed frames are refused; a
// frame whose sprites overflow the pages draws nothing; and a digest of
// the synthetic frame is pinned. With --data, a scene of the installed
// game's GAF frames through its palette and alpha table, held to the
// processor and to a reference that blends to the true mean.
#include "runtime_full.hpp"

#include "oa/app/card/executor.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/present/model/model_library.hpp"
#include "oa/present/model/rgb_bridge.hpp"
#include "oa/present/surface.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace card = oa::app::card;
namespace full = oa::app::full;
namespace gpu = oa::present::gpu_world;
namespace model_render = oa::present::model;
namespace wr = oa::present::world_renderer;
using oa::app::WorldDrawKind;
using oa::app::WorldDrawList;
using oa::formats::gaf::RenderedFrame;

/// The synthetic scene's battlefield, in scene pixels at zoom 1, and where
/// it lies in the window; the window keeps a border around it that no draw
/// may touch.
constexpr int32_t field_width = 160;
constexpr int32_t field_height = 120;
constexpr int32_t field_left = 16;
constexpr int32_t field_top = 8;
constexpr int32_t border = 8;
/// The battlefield of the installed game's scene.
constexpr int32_t data_field_width = 320;
constexpr int32_t data_field_height = 240;
/// The colour outside the battlefield, which no draw may touch.
constexpr std::array<uint8_t, 3> outside_colour{0xff, 0x00, 0xff};
/// Most a channel of a pixel the card blends at a half may differ from the
/// mean the processor's table gives, or the reference's: the renderer
/// rounds its blend its own way.
constexpr int most_blend_difference = 2;
/// The synthetic palette: a grey ramp of multiples of 4 in its first
/// entries, so that the mean of two even entries' greys is an entry, then
/// a colour ramp, then a third ramp.
constexpr std::size_t grey_entries = 64;
constexpr std::size_t colour_entries = 64;
constexpr uint8_t grey_step = 4;
/// The transparency index every synthetic frame carries.
constexpr uint8_t transparent_index = 0;
/// The gamma a page is built at where the test applies none.
constexpr float plain_gamma = 1.0F;
/// The sprites of the installed game's scene, and how many are blended.
constexpr uint32_t data_sprites = 150;
constexpr uint32_t data_blend_every = 3;
/// The pinned digest of the synthetic scene's frame at zoom 1 (FNV-1a of
/// its batches, vertices and indices).
constexpr uint64_t synthetic_frame_digest = 0xb17c3b97a53e2622ULL;

/// An RGB picture.
struct Picture {
    uint32_t width{};
    uint32_t height{};
    std::vector<uint8_t> rgb;

    /// Makes a picture of one colour.
    ///
    /// @param w columns
    /// @param h rows
    /// @param colour the colour
    Picture(uint32_t w, uint32_t h, const std::array<uint8_t, 3>& colour)
        : width(w), height(h), rgb(std::size_t{w} * h * 3U) {
        for (std::size_t pixel = 0; pixel < std::size_t{w} * h; ++pixel)
            std::copy_n(colour.begin(), 3, rgb.begin() + static_cast<std::ptrdiff_t>(pixel * 3U));
    }

    Picture() = default;

    /// Returns a pixel's colour.
    ///
    /// @param x column
    /// @param y row
    /// @return the colour
    [[nodiscard]] std::array<uint8_t, 3> at(uint32_t x, uint32_t y) const {
        const auto offset = (std::size_t{y} * width + x) * 3U;
        return {rgb[offset], rgb[offset + 1], rgb[offset + 2]};
    }

    /// Sets a pixel's colour.
    ///
    /// @param x column
    /// @param y row
    /// @param colour the colour
    void set(uint32_t x, uint32_t y, const std::array<uint8_t, 3>& colour) {
        const auto offset = (std::size_t{y} * width + x) * 3U;
        std::copy_n(colour.begin(), 3, rgb.begin() + static_cast<std::ptrdiff_t>(offset));
    }
};

/// Returns the synthetic palette.
oa::PaletteBytes test_palette() {
    oa::PaletteBytes palette{};
    for (std::size_t index = 0; index < oa::palette_color_count; ++index) {
        uint8_t* entry = palette.data() + index * oa::palette_entry_bytes;
        if (index < grey_entries) {
            const auto grey = static_cast<uint8_t>(index * grey_step);
            entry[0] = entry[1] = entry[2] = grey;
        } else if (index < grey_entries + colour_entries) {
            const auto step = static_cast<uint8_t>((index - grey_entries) * grey_step);
            entry[0] = step;
            entry[1] = 128;
            entry[2] = static_cast<uint8_t>(255 - step);
        } else {
            entry[0] = static_cast<uint8_t>(index);
            entry[1] = static_cast<uint8_t>(255 - index);
            entry[2] = static_cast<uint8_t>(index * 3U);
        }
        entry[3] = 0;
    }
    return palette;
}

/// Returns a palette entry's colour.
///
/// @param palette the palette
/// @param index the entry
/// @return its colour
std::array<uint8_t, 3> entry_colour(const oa::PaletteBytes& palette, uint8_t index) {
    const std::size_t at = std::size_t{index} * oa::palette_entry_bytes;
    return {palette[at], palette[at + 1], palette[at + 2]};
}

/// Returns a frame covered throughout in one index.
///
/// @param width columns
/// @param height rows
/// @param origin_x hotspot column
/// @param origin_y hotspot row
/// @param index the index
/// @return the frame
RenderedFrame
block_frame(uint16_t width, uint16_t height, int16_t origin_x, int16_t origin_y, uint8_t index) {
    RenderedFrame frame;
    frame.width = width;
    frame.height = height;
    frame.origin_x = origin_x;
    frame.origin_y = origin_y;
    frame.transparency_index = transparent_index;
    frame.pixels.assign(std::size_t{width} * height, index);
    frame.coverage.assign(std::size_t{width} * height, 1);
    return frame;
}

/// Returns a frame whose border is covered in one index, with a hole
/// inside it and one covered pixel of the transparency index in the hole,
/// which is drawn in that index's colour.
///
/// @param width columns, at least 5
/// @param height rows, at least 5
/// @param origin_x hotspot column
/// @param origin_y hotspot row
/// @param index the border's index
/// @return the frame
RenderedFrame
ring_frame(uint16_t width, uint16_t height, int16_t origin_x, int16_t origin_y, uint8_t index) {
    RenderedFrame frame = block_frame(width, height, origin_x, origin_y, index);
    for (uint16_t y = 1; y + 1 < height; ++y)
        for (uint16_t x = 1; x + 1 < width; ++x)
            frame.coverage[std::size_t{y} * width + x] = 0;
    const std::size_t middle =
        static_cast<std::size_t>(height / 2) * width + static_cast<std::size_t>(width / 2);
    frame.pixels[middle] = transparent_index;
    frame.coverage[middle] = 1;
    return frame;
}

/// Returns a frame whose indices step through a range along its diagonals.
///
/// @param width columns
/// @param height rows
/// @param origin_x hotspot column
/// @param origin_y hotspot row
/// @param first the first index
/// @param count the indices stepped through
/// @return the frame
RenderedFrame gradient_frame(
    uint16_t width,
    uint16_t height,
    int16_t origin_x,
    int16_t origin_y,
    uint8_t first,
    uint8_t count
) {
    RenderedFrame frame = block_frame(width, height, origin_x, origin_y, first);
    for (uint16_t y = 0; y < height; ++y)
        for (uint16_t x = 0; x < width; ++x)
            frame.pixels[std::size_t{y} * width + x] =
                static_cast<uint8_t>(first + (x + y) % count);
    return frame;
}

/// Returns a frame with every pixel's index put through a table.
///
/// @param frame the frame
/// @param table the index each index becomes
/// @return the frame with its indices mapped
RenderedFrame
mapped_frame(RenderedFrame frame, const std::array<uint8_t, gpu::gray_table_entries>& table) {
    for (auto& index : frame.pixels)
        index = table[index];
    return frame;
}

/// Returns a background of even grey entries in blocks.
///
/// @param palette the palette
/// @param width columns
/// @param height rows
/// @return the picture
Picture grey_background(const oa::PaletteBytes& palette, uint32_t width, uint32_t height) {
    Picture picture(width, height, {});
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const auto index = static_cast<uint8_t>(2U * ((x / 8U + y / 8U) % 16U));
            picture.set(x, y, entry_colour(palette, index));
        }
    return picture;
}

/// Draws a list on the processor as the bands draw it, on one band, the
/// camera on the map's top-left corner.
///
/// @param list the list
/// @param palette the palette
/// @param display the models' display, whose alpha table blends
/// @param background the battlefield before the draws
/// @param zoom scene pixels per map pixel
/// @return the battlefield after them
Picture draw_processor(
    const WorldDrawList& list,
    const oa::PaletteBytes& palette,
    model_render::ModelDisplay& display,
    const Picture& background,
    float zoom
) {
    Picture picture = background;
    const auto width = static_cast<int32_t>(picture.width);
    const auto height = static_cast<int32_t>(picture.height);
    model_render::RgbBridge bridge;
    const model_render::RgbFrame frame{picture.rgb.data(), width, height, width * 3};
    model_render::bridge_begin(bridge, frame, {0, 0, width - 1, height - 1}, zoom, display.palette);
    oa::app::WorldFrameDraw draw;
    draw.target = {picture.rgb.data(), width, height, 0, 0, width, height, 0, height};
    draw.palette = &palette;
    draw.scale = zoom;
    draw.bridge = &bridge;
    draw.display = &display;
    if (list.shown_map_width > 0 && list.shown_map_height > 0) {
        const auto across =
            wr::shown_map_span(0, static_cast<uint32_t>(list.shown_map_width), zoom);
        const auto down = wr::shown_map_span(0, static_cast<uint32_t>(list.shown_map_height), zoom);
        draw.shown_map = {across.first, down.first, across.end, down.end};
    }
    std::vector<model_render::BridgeBand> split;
    OA_CHECK(model_render::bridge_split(bridge, 1, split) == 1);
    model_render::ModelRenderer renderer{};
    model_render::SupersampleScratch supersample{};
    std::vector<oa::formats::objects3d::FixedVector3> points;
    oa::app::draw_world_band(list, draw, split.front(), renderer, supersample, points);
    model_render::bridge_join_band(bridge, split.front());
    return picture;
}

/// SDL's software renderer over a window-sized surface, with the executor
/// and the sprite pages the card draws from.
struct CardSide {
    SDL_Surface* surface{};
    SDL_Renderer* renderer{};
    card::Executor executor;
    gpu::SpritePages pages;
    std::vector<card::PageHandle> handles;
    uint32_t window_width{};
    uint32_t window_height{};

    /// Makes the surface, the renderer and the executor.
    ///
    /// @param width the window's columns
    /// @param height its rows
    /// @param limits the pages' limits
    CardSide(uint32_t width, uint32_t height, const gpu::Limits& limits)
        : pages(limits), window_width(width), window_height(height) {
        surface = SDL_CreateSurface(
            static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_XRGB8888
        );
        renderer = surface != nullptr ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        OA_CHECK(renderer != nullptr);
        OA_CHECK(executor.open(renderer, 0));
    }

    CardSide(const CardSide&) = delete;
    CardSide& operator=(const CardSide&) = delete;

    ~CardSide() {
        executor.close();
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }

    /// Fills the window: the battlefield's picture at its place, the
    /// outside colour elsewhere.
    ///
    /// @param background the battlefield
    void fill(const Picture& background) const {
        for (uint32_t y = 0; y < window_height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(surface->pixels) +
                static_cast<std::ptrdiff_t>(y) * surface->pitch
            );
            for (uint32_t x = 0; x < window_width; ++x) {
                std::array<uint8_t, 3> colour = outside_colour;
                const auto field_x = static_cast<int64_t>(x) - field_left;
                const auto field_y = static_cast<int64_t>(y) - field_top;
                if (field_x >= 0 && field_y >= 0 && field_x < background.width &&
                    field_y < background.height)
                    colour = background.at(
                        static_cast<uint32_t>(field_x), static_cast<uint32_t>(field_y)
                    );
                row[x] = 0xff000000U | (uint32_t{colour[0]} << 16) | (uint32_t{colour[1]} << 8) |
                         colour[2];
            }
        }
    }

    /// Reads the window back.
    ///
    /// @return its picture
    [[nodiscard]] Picture read() const {
        Picture picture(window_width, window_height, {});
        SDL_Surface* shown = SDL_RenderReadPixels(renderer, nullptr);
        OA_CHECK(shown != nullptr);
        if (shown == nullptr)
            return picture;
        SDL_Surface* rgb = SDL_ConvertSurface(shown, SDL_PIXELFORMAT_RGB24);
        SDL_DestroySurface(shown);
        OA_CHECK(rgb != nullptr);
        if (rgb == nullptr)
            return picture;
        for (uint32_t y = 0; y < window_height; ++y)
            std::memcpy(
                picture.rgb.data() + std::size_t{y} * window_width * 3U,
                static_cast<const uint8_t*>(rgb->pixels) +
                    static_cast<std::ptrdiff_t>(y) * rgb->pitch,
                std::size_t{window_width} * 3U
            );
        SDL_DestroySurface(rgb);
        return picture;
    }

    /// Returns the card's page for a sprite page, making it when it is new
    /// (full::SpritePageHooks::card_page).
    ///
    /// @param context the card side
    /// @param page index into the sprite pages
    /// @return the handle
    static card::PageHandle page_for(void* context, uint32_t page) {
        auto& side = *static_cast<CardSide*>(context);
        const auto held = side.pages.pages();
        if (page >= held.size())
            return {};
        if (side.handles.size() <= page)
            side.handles.resize(std::size_t{page} + 1);
        if (side.handles[page] != card::PageHandle{} &&
            side.executor.page_alive(side.handles[page]))
            return side.handles[page];
        card::PageDescription description;
        description.width = held[page].size;
        description.height = held[page].size;
        description.level_count = 1;
        side.handles[page] = side.executor.create_page(description);
        OA_CHECK(side.handles[page] != card::PageHandle{});
        return side.handles[page];
    }

    /// Uploads every page's texels whole.
    void upload() {
        const auto held = pages.pages();
        for (uint32_t index = 0; index < handles.size() && index < held.size(); ++index) {
            if (handles[index] == card::PageHandle{} || held[index].size == 0)
                continue;
            OA_CHECK(executor.update_page(
                handles[index],
                0,
                nullptr,
                held[index].texels.data(),
                held[index].size * gpu::texel_bytes
            ));
            pages.clear_dirty(index);
        }
    }
};

/// What a card draw gives back.
struct CardDraw {
    Picture field;                    ///< the battlefield read back
    full::SpriteStageResult result{}; ///< what the stage did
    card::CardFrame frame;            ///< the frame it emitted
    bool outside_untouched{true};     ///< the window beside the battlefield kept its colour
};

/// Draws a list on the card: the stage's frame over the background, run
/// on the software renderer and read back.
///
/// @param side the card side
/// @param list the list
/// @param palette the palette
/// @param background the battlefield before the draws
/// @param zoom scene pixels per map pixel
/// @param sight the viewer's sight; none sees everything
/// @param gamma the display gamma's table; null for 1
/// @param hooks_given whether the stage is given the card's pages
/// @return the picture and what the stage did
CardDraw draw_card(
    CardSide& side,
    const WorldDrawList& list,
    const oa::PaletteBytes& palette,
    const Picture& background,
    float zoom,
    const full::SightView& sight = {},
    const std::array<uint8_t, 256>* gamma = nullptr,
    bool hooks_given = true
) {
    CardDraw drawn;
    side.fill(background);
    full::SpriteStageInputs inputs;
    inputs.list = &list;
    inputs.view.origin_x = static_cast<float>(field_left);
    inputs.view.origin_y = static_cast<float>(field_top);
    inputs.view.scale = 1.0F;
    inputs.view.zoom = zoom;
    inputs.view.width = static_cast<int32_t>(background.width);
    inputs.view.height = static_cast<int32_t>(background.height);
    inputs.sight = sight;
    inputs.palette = &palette;
    inputs.gamma = gamma;
    full::SpritePageHooks hooks;
    if (hooks_given) {
        hooks.context = &side;
        hooks.card_page = &CardSide::page_for;
    }
    drawn.result = full::emit_sprites(inputs, side.pages, hooks, drawn.frame);
    side.upload();
    const std::string malformed = card::check_frame(drawn.frame);
    if (!malformed.empty())
        std::fprintf(stderr, "the stage's frame is malformed: %s\n", malformed.c_str());
    OA_CHECK(malformed.empty());
    const bool ran = side.executor.execute(drawn.frame, nullptr);
    if (!ran)
        std::fprintf(stderr, "the executor refused the frame: %s\n", side.executor.error().c_str());
    OA_CHECK(ran);
    const Picture window = side.read();
    drawn.field = Picture(background.width, background.height, {});
    for (uint32_t y = 0; y < window.height; ++y)
        for (uint32_t x = 0; x < window.width; ++x) {
            const auto field_x = static_cast<int64_t>(x) - field_left;
            const auto field_y = static_cast<int64_t>(y) - field_top;
            if (field_x >= 0 && field_y >= 0 && field_x < background.width &&
                field_y < background.height)
                drawn.field.set(
                    static_cast<uint32_t>(field_x), static_cast<uint32_t>(field_y), window.at(x, y)
                );
            else if (window.at(x, y) != outside_colour)
                drawn.outside_untouched = false;
        }
    return drawn;
}

/// How two pictures differ, beside and under a mask.
struct Comparison {
    int most_beside{};       ///< the largest channel difference beside the mask
    int most_under{};        ///< under it
    std::size_t beside{};    ///< pixels beside the mask
    std::size_t under{};     ///< pixels under it
    std::size_t differing{}; ///< pixels beside the mask that differ at all
};

/// Compares two pictures of one size, the pixels a mask marks apart.
///
/// @param first a picture
/// @param second another
/// @param mask one byte a pixel; a set byte marks the pixel; empty marks none
/// @return the differences
Comparison compare(const Picture& first, const Picture& second, const std::vector<uint8_t>& mask) {
    Comparison comparison;
    OA_CHECK(first.width == second.width && first.height == second.height);
    if (first.width != second.width || first.height != second.height)
        return comparison;
    for (std::size_t pixel = 0; pixel < std::size_t{first.width} * first.height; ++pixel) {
        const bool marked = pixel < mask.size() && mask[pixel] != 0;
        int most = 0;
        for (std::size_t channel = 0; channel < 3; ++channel)
            most = std::max(
                most,
                std::abs(
                    int{first.rgb[pixel * 3U + channel]} - int{second.rgb[pixel * 3U + channel]}
                )
            );
        if (marked) {
            ++comparison.under;
            comparison.most_under = std::max(comparison.most_under, most);
        } else {
            ++comparison.beside;
            comparison.most_beside = std::max(comparison.most_beside, most);
            if (most != 0)
                ++comparison.differing;
        }
    }
    return comparison;
}

/// Marks a rectangle of a mask, clipped to the picture.
///
/// @param[in,out] mask one byte a pixel
/// @param width the picture's columns
/// @param height its rows
/// @param x the left column
/// @param y the top row
/// @param w columns
/// @param h rows
void mark(std::vector<uint8_t>& mask, uint32_t width, uint32_t height, int x, int y, int w, int h) {
    for (int row = std::max(y, 0); row < std::min(y + h, static_cast<int>(height)); ++row)
        for (int column = std::max(x, 0); column < std::min(x + w, static_cast<int>(width));
             ++column)
            mask[static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column)] = 1;
}

/// Returns the rectangle a sprite draw covers at a zoom, as the processor
/// draws it.
///
/// @param sprite the draw
/// @param zoom scene pixels per map pixel
/// @return left, top, columns, rows
std::array<int, 4> sprite_rect(const oa::app::SpriteDraw& sprite, float zoom) {
    const oa::app::SceneRect rect = oa::app::sprite_scene_rect(sprite, zoom);
    return {
        static_cast<int>(rect.left),
        static_cast<int>(rect.top),
        std::max(1, static_cast<int>(rect.right - rect.left)),
        std::max(1, static_cast<int>(rect.bottom - rect.top))
    };
}

/// Returns a mask of the pixels the list's blended sprites cover at a zoom.
///
/// @param list the list
/// @param width the picture's columns
/// @param height its rows
/// @param zoom scene pixels per map pixel
/// @param every whether every sprite is marked, not the blended alone
/// @return the mask
std::vector<uint8_t>
sprite_mask(const WorldDrawList& list, uint32_t width, uint32_t height, float zoom, bool every) {
    std::vector<uint8_t> mask(std::size_t{width} * height, 0);
    for (const auto& draw : list.draws) {
        if (draw.kind != WorldDrawKind::blended_sprite &&
            !(every && draw.kind == WorldDrawKind::sprite))
            continue;
        const auto& sprite = list.sprites[draw.index];
        if (sprite.frame == nullptr)
            continue;
        const auto rect = sprite_rect(sprite, zoom);
        mark(mask, width, height, rect[0], rect[1], rect[2], rect[3]);
    }
    return mask;
}

/// Returns how many blended sprites of a list cover each pixel.
///
/// @param list the list
/// @param width the picture's columns
/// @param height its rows
/// @return one count a pixel
std::vector<uint8_t> blend_counts(const WorldDrawList& list, uint32_t width, uint32_t height) {
    std::vector<uint8_t> counts(std::size_t{width} * height, 0);
    for (const auto& draw : list.draws) {
        if (draw.kind != WorldDrawKind::blended_sprite)
            continue;
        const auto& sprite = list.sprites[draw.index];
        const RenderedFrame& frame = *sprite.frame;
        for (uint16_t y = 0; y < frame.height; ++y)
            for (uint16_t x = 0; x < frame.width; ++x) {
                if (frame.coverage[std::size_t{y} * frame.width + x] == 0)
                    continue;
                const int64_t column = int64_t{sprite.place.x} - frame.origin_x + x;
                const int64_t row = int64_t{sprite.place.y} - frame.origin_y + y;
                if (column < 0 || row < 0 || column >= width || row >= height)
                    continue;
                auto& count = counts
                    [static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column)];
                if (count < 255)
                    ++count;
            }
    }
    return counts;
}

/// Returns the most a channel exceeds the blend tolerance for the blends
/// over its pixel, between two pictures: 0 when every pixel is within it.
///
/// @param first a picture
/// @param second another of the same size
/// @param counts blended sprites over each pixel
/// @return the largest excess
int most_blend_excess(
    const Picture& first, const Picture& second, const std::vector<uint8_t>& counts
) {
    int excess = 0;
    for (std::size_t pixel = 0; pixel < counts.size(); ++pixel)
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int delta = std::abs(
                int{first.rgb[pixel * 3U + channel]} - int{second.rgb[pixel * 3U + channel]}
            );
            excess = std::max(excess, delta - most_blend_difference * int{counts[pixel]});
        }
    return excess;
}

/// Adds a sprite draw to a list, placed at a map point, with the camera on
/// map pixel (0, 0); the zoom the list is drawn at lays it on the scene.
///
/// @param[in,out] list the list
/// @param frame the frame
/// @param map_x the point's map pixel column
/// @param map_y its row
/// @param blended whether the planner blends it through the alpha table
void add_sprite(
    WorldDrawList& list, const RenderedFrame& frame, int32_t map_x, int32_t map_y, bool blended
) {
    oa::app::SpriteDraw sprite;
    sprite.frame = &frame;
    sprite.place = {map_x, map_y};
    list.sprites.push_back(sprite);
    oa::app::add_world_draw(
        list,
        blended ? WorldDrawKind::blended_sprite : WorldDrawKind::sprite,
        list.sprites.size() - 1
    );
}

/// Adds a particle square to a list, clipped to the battlefield as the
/// planner clips it.
///
/// @param[in,out] list the list
/// @param x the left column, in scene pixels
/// @param y the top row
/// @param side its side
/// @param colour its colour
/// @param width the battlefield's columns
/// @param height its rows
void add_square(
    WorldDrawList& list,
    int32_t x,
    int32_t y,
    int32_t side,
    const std::array<uint8_t, 3>& colour,
    int32_t width,
    int32_t height
) {
    list.squares.push_back(
        {std::max(x, 0),
         std::max(y, 0),
         std::min(x + side, width),
         std::min(y + side, height),
         colour}
    );
    oa::app::add_world_draw(list, WorldDrawKind::pixel_square, list.squares.size() - 1);
}

/// The frames of the synthetic scene, kept together so that the list's
/// pointers stay good.
struct Frames {
    RenderedFrame colour_block = block_frame(12, 10, 6, 5, 70);
    RenderedFrame ring = ring_frame(9, 7, 4, 3, 100);
    RenderedFrame gradient = gradient_frame(16, 16, 0, 0, 128, 16);
    RenderedFrame grey_block = block_frame(14, 8, 7, 4, 40);
    RenderedFrame pale_block = block_frame(10, 12, 0, 0, 20);
    RenderedFrame bright_ring = ring_frame(11, 9, 5, 4, 60);
    RenderedFrame tiny = block_frame(1, 1, 0, 0, 90);
    RenderedFrame tall = block_frame(6, 40, 3, 20, 150);
};

/// Builds the synthetic scene at a zoom: sprites of every frame, blended
/// sprites over even greys alone, so that the processor's alpha table
/// blends to the exact mean, an opaque sprite drawn over a blended one and
/// a blended one over an opaque grey, squares between them, and sprites
/// off the battlefield's edges.
///
/// @param frames the frames
/// @param palette the palette
/// @param zoom scene pixels per map pixel
/// @param width the battlefield's columns
/// @param height its rows
/// @return the list
WorldDrawList synthetic_scene(
    const Frames& frames, const oa::PaletteBytes& palette, float zoom, int32_t width, int32_t height
) {
    WorldDrawList list;
    add_sprite(list, frames.colour_block, 30, 20, false);
    add_sprite(list, frames.grey_block, 24, 60, true);
    add_sprite(list, frames.gradient, 100, 30, false);
    // Over an opaque grey, blended; then an opaque ring over the blend.
    add_sprite(list, frames.grey_block, 70, 90, false);
    add_sprite(list, frames.pale_block, 66, 88, true);
    add_sprite(list, frames.ring, 72, 92, false);
    oa::app::add_world_draw(list, WorldDrawKind::commit, 0);
    add_sprite(list, frames.bright_ring, 120, 80, true);
    const auto side = static_cast<int32_t>(std::lround(2.0 * zoom));
    add_square(
        list,
        static_cast<int32_t>(std::lround(5.0 * zoom)),
        static_cast<int32_t>(std::lround(5.0 * zoom)),
        side,
        entry_colour(palette, 200),
        width,
        height
    );
    add_square(
        list,
        static_cast<int32_t>(std::lround(100.0 * zoom)),
        static_cast<int32_t>(std::lround(36.0 * zoom)),
        side,
        entry_colour(palette, 210),
        width,
        height
    );
    add_square(list, width - 1, height - 1, side, entry_colour(palette, 220), width, height);
    add_sprite(list, frames.tiny, 0, 0, false);
    add_sprite(list, frames.colour_block, -2, 110, false);
    add_sprite(list, frames.tall, 158, -6, false);
    add_sprite(list, frames.colour_block, 150, 115, false);
    return list;
}

/// Returns the FNV-1a digest of a frame's batches, vertices and indices.
///
/// @param frame the frame
/// @return the digest
uint64_t frame_digest(const card::CardFrame& frame) {
    uint64_t digest = 0xcbf29ce484222325ULL;
    const auto take = [&](const void* bytes, std::size_t count) {
        const auto* data = static_cast<const uint8_t*>(bytes);
        for (std::size_t index = 0; index < count; ++index) {
            digest ^= data[index];
            digest *= 0x100000001b3ULL;
        }
    };
    for (const auto& batch : frame.batches) {
        const uint32_t words[]{
            static_cast<uint32_t>(batch.operation),
            batch.target.value,
            batch.page.value,
            batch.level,
            static_cast<uint32_t>(batch.blend),
            static_cast<uint32_t>(batch.sampling),
            batch.scissored ? 1U : 0U,
            static_cast<uint32_t>(batch.scissor.x),
            static_cast<uint32_t>(batch.scissor.y),
            static_cast<uint32_t>(batch.scissor.width),
            static_cast<uint32_t>(batch.scissor.height),
            batch.first_index,
            batch.index_count
        };
        take(words, sizeof words);
    }
    for (const auto& vertex : frame.vertices)
        take(&vertex, sizeof vertex);
    for (const auto index : frame.indices)
        take(&index, sizeof index);
    return digest;
}

/// The synthetic scene at a zoom: the card's picture against the
/// processor's, exact beside the blends and within the blend tolerance
/// under them, the squares among the exact; the stage's counts and batches.
///
/// @param zoom scene pixels per map pixel, a whole number
void test_synthetic_scene(float zoom) {
    const auto palette = test_palette();
    const auto width = static_cast<int32_t>(std::lround(field_width * zoom));
    const auto height = static_cast<int32_t>(std::lround(field_height * zoom));
    const Frames frames;
    const WorldDrawList list = synthetic_scene(frames, palette, zoom, width, height);
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    const Picture background =
        grey_background(palette, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    const Picture processor = draw_processor(list, palette, display, background, zoom);
    CardSide side(
        static_cast<uint32_t>(field_left + width + border),
        static_cast<uint32_t>(field_top + height + border),
        gpu::Limits{}
    );
    side.pages.set_palette(oa::present::palette_from_bytes(palette), plain_gamma);
    const CardDraw card = draw_card(side, list, palette, background, zoom);
    OA_CHECK(card.outside_untouched);
    const auto blended =
        sprite_mask(list, static_cast<uint32_t>(width), static_cast<uint32_t>(height), zoom, false);
    const Comparison comparison = compare(card.field, processor, blended);
    std::printf(
        "synthetic scene at zoom %g: %u sprites, %u squares, %u batches; beside the blends most "
        "%d over %zu pixels (%zu differ), under them most %d over %zu\n",
        static_cast<double>(zoom),
        card.result.sprites,
        card.result.squares,
        card.result.batches,
        comparison.most_beside,
        comparison.beside,
        comparison.differing,
        comparison.most_under,
        comparison.under
    );
    OA_CHECK(comparison.beside > 0 && comparison.most_beside == 0);
    OA_CHECK(comparison.under > 0 && comparison.most_under <= most_blend_difference);
    // Every sprite drawn, the square off the corner clipped away, and the
    // sprites' batches merged about the squares'.
    OA_CHECK(card.result.sprites == 11 && card.result.refused == 0 && card.result.greyed == 0);
    OA_CHECK(card.result.squares == 3 && card.result.lines == 0);
    OA_CHECK(card.result.batches == 3);
    OA_CHECK(card.frame.batches.size() == 3);
    for (const auto& batch : card.frame.batches) {
        OA_CHECK(batch.scissored);
        OA_CHECK(batch.scissor.x == field_left && batch.scissor.y == field_top);
        OA_CHECK(batch.scissor.width == width && batch.scissor.height == height);
        OA_CHECK(batch.sampling == card::Sampling::nearest);
    }
    OA_CHECK(card.frame.batches[0].blend == card::Blend::alpha_premultiplied);
    OA_CHECK(card.frame.batches[1].blend == card::Blend::none);
    OA_CHECK(card.frame.batches[1].page == card::PageHandle{});
    // The blended sprite's drawn colour and alpha are halves; the others' whole.
    OA_CHECK(
        card.frame.vertices[4].colour.alpha == 0.5F && card.frame.vertices[4].colour.red == 0.5F
    );
    OA_CHECK(card.frame.vertices[0].colour.alpha == 1.0F);
    if (zoom == 1.0F) {
        const uint64_t digest = frame_digest(card.frame);
        std::printf("synthetic frame digest %#llx\n", static_cast<unsigned long long>(digest));
        OA_CHECK(digest == synthetic_frame_digest);
    }
}

/// A feature's frame over the map's right and bottom edges is cut off
/// where the map the view shows ends, on the card as on the processor and
/// the same on both; a feature's frame wholly past the map draws nothing;
/// a frame that is not a feature's is drawn past the edges whole.
///
/// @param zoom scene pixels per map pixel
void test_features_on_the_map(float zoom) {
    constexpr int32_t map_width = 100;
    constexpr int32_t map_height = 80;
    const auto palette = test_palette();
    const auto width = static_cast<int32_t>(std::lround(field_width * zoom));
    const auto height = static_cast<int32_t>(std::lround(field_height * zoom));
    const auto edge_x = static_cast<int32_t>(std::lround(map_width * zoom));
    const auto edge_y = static_cast<int32_t>(std::lround(map_height * zoom));
    const RenderedFrame feature = block_frame(20, 16, 10, 8, grey_entries + 5);
    const RenderedFrame other = block_frame(12, 12, 6, 6, grey_entries + 40);
    WorldDrawList list;
    list.shown_map_width = map_width;
    list.shown_map_height = map_height;
    add_sprite(list, feature, map_width - 4, map_height - 3, false);
    list.sprites.back().on_map = true;
    add_sprite(list, feature, map_width + 30, 20, false);
    list.sprites.back().on_map = true;
    add_sprite(list, other, 30, map_height, false);
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    const Picture background =
        grey_background(palette, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    const Picture processor = draw_processor(list, palette, display, background, zoom);
    CardSide side(
        static_cast<uint32_t>(field_left + width + border),
        static_cast<uint32_t>(field_top + height + border),
        gpu::Limits{}
    );
    side.pages.set_palette(oa::present::palette_from_bytes(palette), plain_gamma);
    const CardDraw card = draw_card(side, list, palette, background, zoom);
    OA_CHECK(card.outside_untouched);
    const Comparison comparison = compare(card.field, processor, {});
    OA_CHECK(comparison.beside > 0 && comparison.most_beside == 0);
    // Past the map only the other frame changes the picture; over the map
    // the feature's frame is drawn up to the edges.
    const auto feature_colour = entry_colour(palette, grey_entries + 5);
    const auto other_colour = entry_colour(palette, grey_entries + 40);
    uint32_t feature_past = 0;
    uint32_t other_past = 0;
    for (uint32_t y = 0; y < processor.height; ++y)
        for (uint32_t x = 0; x < processor.width; ++x) {
            const bool past =
                static_cast<int32_t>(x) >= edge_x || static_cast<int32_t>(y) >= edge_y;
            feature_past += past && processor.at(x, y) == feature_colour ? 1U : 0U;
            other_past += past && processor.at(x, y) == other_colour ? 1U : 0U;
        }
    OA_CHECK(feature_past == 0);
    OA_CHECK(other_past != 0);
    OA_CHECK(
        processor.at(static_cast<uint32_t>(edge_x - 1), static_cast<uint32_t>(edge_y - 1)) ==
        feature_colour
    );
    // The features' batch takes the map's part of the battlefield as its
    // scissor, which leaves out the feature past the map.
    OA_CHECK(card.result.sprites == 3 && card.result.refused == 0);
    bool map_batch = false;
    for (const auto& batch : card.frame.batches)
        map_batch = map_batch || (batch.scissor.x == field_left && batch.scissor.y == field_top &&
                                  batch.scissor.width == edge_x && batch.scissor.height == edge_y);
    OA_CHECK(map_batch);
    std::printf(
        "features on the map at zoom %g: %u sprites in %u batches, none of a feature past the "
        "edges, %u pixels of another frame there\n",
        static_cast<double>(zoom),
        card.result.sprites,
        card.result.batches,
        other_past
    );
}

/// A feature's shadow frames, drawn and blended, on the card as dark as the
/// list's shadow level: the game's own at the full level, that strength of
/// it below, within the blend tolerance of the processor's, and not drawn
/// at 0. A frame that is not a shadow is drawn whole at every level.
void test_shadow_sprites() {
    const auto palette = test_palette();
    const Frames frames;
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    const Picture background = grey_background(
        palette, static_cast<uint32_t>(field_width), static_cast<uint32_t>(field_height)
    );
    for (const uint32_t level : {model_render::shadow_full_level, 32U, 16U, 0U}) {
        WorldDrawList list;
        list.shadow_level = level;
        add_sprite(list, frames.colour_block, 30, 20, false);
        list.sprites.back().shadow = true;
        add_sprite(list, frames.grey_block, 24, 60, true);
        list.sprites.back().shadow = true;
        add_sprite(list, frames.ring, 100, 30, false);
        const Picture processor = draw_processor(list, palette, display, background, 1.0F);
        CardSide side(
            static_cast<uint32_t>(field_left + field_width + border),
            static_cast<uint32_t>(field_top + field_height + border),
            gpu::Limits{}
        );
        side.pages.set_palette(oa::present::palette_from_bytes(palette), plain_gamma);
        const CardDraw card = draw_card(side, list, palette, background, 1.0F);
        // The shadows' pixels are compared within the blend tolerance,
        // every other pixel exactly.
        std::vector<uint8_t> shadows(std::size_t{field_width} * field_height, 0);
        for (std::size_t index = 0; index < 2; ++index) {
            const auto rect = sprite_rect(list.sprites[index], 1.0F);
            mark(shadows, field_width, field_height, rect[0], rect[1], rect[2], rect[3]);
        }
        const Comparison comparison = compare(card.field, processor, shadows);
        std::printf(
            "shadow sprites at level %u: %u sprites; beside them most %d, under them most %d\n",
            level,
            card.result.sprites,
            comparison.most_beside,
            comparison.most_under
        );
        OA_CHECK(comparison.most_beside == 0);
        OA_CHECK(comparison.most_under <= most_blend_difference);
        OA_CHECK(card.result.sprites == (level == 0 ? 1U : 3U));
        if (level == 0) {
            // Nothing of the drawn shadow on either side.
            const auto rect = sprite_rect(list.sprites[0], 1.0F);
            const std::size_t at = (static_cast<std::size_t>(rect[1]) * field_width +
                                    static_cast<std::size_t>(rect[0])) *
                                   3U;
            OA_CHECK(
                std::equal(
                    processor.rgb.begin() + static_cast<std::ptrdiff_t>(at),
                    processor.rgb.begin() + static_cast<std::ptrdiff_t>(at + 3),
                    background.rgb.begin() + static_cast<std::ptrdiff_t>(at)
                )
            );
            OA_CHECK(
                std::equal(
                    card.field.rgb.begin() + static_cast<std::ptrdiff_t>(at),
                    card.field.rgb.begin() + static_cast<std::ptrdiff_t>(at + 3),
                    background.rgb.begin() + static_cast<std::ptrdiff_t>(at)
                )
            );
            continue;
        }
        // The shadows' vertex colours: the level's strength, halved for the
        // blended one; the ring's whole.
        const float strength = model_render::shadow_level_strength(level);
        OA_CHECK(card.frame.vertices.size() == 12);
        OA_CHECK(card.frame.vertices[0].colour.alpha == strength);
        OA_CHECK(card.frame.vertices[0].colour.red == strength);
        OA_CHECK(card.frame.vertices[4].colour.alpha == 0.5F * strength);
        OA_CHECK(card.frame.vertices[8].colour.alpha == 1.0F);
    }
}

/// Returns a line's pixels as the game steps them.
///
/// @param x0 the first pixel's column
/// @param y0 its row
/// @param x1 the last pixel's column
/// @param y1 its row
/// @return the pixels
std::vector<std::array<int32_t, 2>> line_pixels(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    std::vector<std::array<int32_t, 2>> pixels;
    int32_t dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    while (true) {
        pixels.push_back({x0, y0});
        if (x0 == x1 && y0 == y1)
            break;
        const auto twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
    return pixels;
}

/// A line of the lines scene.
struct SceneLine {
    int32_t x0{};
    int32_t y0{};
    int32_t x1{};
    int32_t y1{};
    bool straight{}; ///< along a row or a column
};

/// Lines and selection lines at a zoom: the card's quads cover every pixel
/// the game's lines cover and stray no further than their width from them;
/// a line along a row or a column, and a selection box, are exact at zoom
/// 1, and the selection box at zoom 2 too, where the bridge writes each map
/// pixel as a block.
///
/// @param zoom scene pixels per map pixel, a whole number
void test_lines(float zoom) {
    const auto palette = test_palette();
    const auto width = static_cast<int32_t>(std::lround(field_width * zoom));
    const auto height = static_cast<int32_t>(std::lround(field_height * zoom));
    const std::array<uint8_t, 3> line_colour{0xf0, 0x20, 0x60};
    constexpr uint8_t box_index = 200;
    const std::vector<SceneLine> lines{
        {10, 10, 60, 10, true},
        {80, 5, 80, 70, true},
        {20, 30, 60, 70, false},
        {5, 100, 120, 110, false},
        {130, 20, 140, 100, false},
        {3, 3, 3, 3, true},
        {150, 115, 100, 60, false},
    };
    WorldDrawList list;
    const auto scaled = [&](int32_t pixel) {
        return static_cast<int32_t>(std::lround(static_cast<double>(pixel) * zoom));
    };
    for (const auto& line : lines) {
        list.lines.push_back(
            {scaled(line.x0), scaled(line.y0), scaled(line.x1), scaled(line.y1), line_colour, 0}
        );
        oa::app::add_world_draw(list, WorldDrawKind::line, list.lines.size() - 1);
    }
    // The selection box, in map pixels, drawn through the bridge.
    const std::array<std::array<int32_t, 4>, 4> box{{
        {50, 50, 70, 50},
        {70, 50, 70, 66},
        {70, 66, 50, 66},
        {50, 66, 50, 50},
    }};
    for (const auto& edge : box) {
        list.lines.push_back({edge[0], edge[1], edge[2], edge[3], {}, box_index});
        oa::app::add_world_draw(list, WorldDrawKind::selection_line, list.lines.size() - 1);
    }
    oa::app::add_world_draw(list, WorldDrawKind::commit_always, 0);
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    const Picture background(
        static_cast<uint32_t>(width), static_cast<uint32_t>(height), entry_colour(palette, 10)
    );
    const Picture processor = draw_processor(list, palette, display, background, zoom);
    CardSide side(
        static_cast<uint32_t>(field_left + width + border),
        static_cast<uint32_t>(field_top + height + border),
        gpu::Limits{}
    );
    side.pages.set_palette(oa::present::palette_from_bytes(palette), plain_gamma);
    const CardDraw card = draw_card(side, list, palette, background, zoom);
    OA_CHECK(card.outside_untouched);
    OA_CHECK(card.result.lines == lines.size() + box.size());
    OA_CHECK(card.result.batches == 1);
    // Every pixel the processor drew has one the card drew in the same
    // colour within a pixel of it: SDL's software renderer truncates a
    // quad's corners to whole pixels before it fills a triangle, which
    // lands a slanted quad up to a pixel left of or above the game's line.
    const auto ground = entry_colour(palette, 10);
    const auto dilated = [&](const Picture& picture, int reach) {
        std::vector<uint8_t> near(std::size_t{picture.width} * picture.height, 0);
        for (uint32_t y = 0; y < picture.height; ++y)
            for (uint32_t x = 0; x < picture.width; ++x)
                if (picture.at(x, y) != ground)
                    mark(
                        near,
                        picture.width,
                        picture.height,
                        static_cast<int>(x) - reach,
                        static_cast<int>(y) - reach,
                        2 * reach + 1,
                        2 * reach + 1
                    );
        return near;
    };
    const auto near_card = dilated(card.field, 1);
    std::size_t drawn = 0;
    std::size_t covered = 0;
    for (uint32_t y = 0; y < processor.height; ++y)
        for (uint32_t x = 0; x < processor.width; ++x) {
            if (processor.at(x, y) == ground)
                continue;
            ++drawn;
            if (near_card[std::size_t{y} * processor.width + x] != 0)
                ++covered;
        }
    OA_CHECK(drawn > 0 && covered == drawn);
    // Every pixel the card drew lies within the lines' width of one the
    // processor drew.
    const int reach = static_cast<int>(std::ceil(std::max(1.0F, zoom)));
    const auto near_processor = dilated(processor, reach);
    std::size_t strayed = 0;
    for (uint32_t y = 0; y < card.field.height; ++y)
        for (uint32_t x = 0; x < card.field.width; ++x)
            if (card.field.at(x, y) != ground &&
                near_processor[std::size_t{y} * card.field.width + x] == 0)
                ++strayed;
    OA_CHECK(strayed == 0);
    // The straight lines at zoom 1, and the selection box at zooms 1 and
    // 2, pixel for pixel: the picture beside the slanted lines is exact.
    std::vector<uint8_t> slanted(std::size_t{processor.width} * processor.height, 0);
    for (const auto& line : lines)
        if (!line.straight || zoom != 1.0F)
            for (const auto& [x, y] :
                 line_pixels(scaled(line.x0), scaled(line.y0), scaled(line.x1), scaled(line.y1)))
                mark(
                    slanted,
                    processor.width,
                    processor.height,
                    x - reach,
                    y - reach,
                    2 * reach + 1,
                    2 * reach + 1
                );
    const Comparison comparison = compare(card.field, processor, slanted);
    std::printf(
        "lines at zoom %g: %zu pixels drawn by the processor, %zu with the card's within a pixel, "
        "%zu of the card's strayed; beside the slanted lines most %d over %zu pixels\n",
        static_cast<double>(zoom),
        drawn,
        covered,
        strayed,
        comparison.most_beside,
        comparison.beside
    );
    OA_CHECK(comparison.beside > 0 && comparison.most_beside == 0);
}

/// The fog's states: a sprite under a cell out of sight drawn from its
/// greyed cell, one under a never-mapped cell left out, one in sight and
/// one off the sight grid drawn in colour; under the dithered option
/// nothing is greyed.
void test_fog_states() {
    const auto palette = test_palette();
    const oa::Palette entries = oa::present::palette_from_bytes(palette);
    // The sight grid: 4 by 4 cells, cell (1, 1) in sight, cell (2, 2)
    // never mapped, the rest mapped but out of sight.
    constexpr int32_t cells = 4;
    std::vector<uint8_t> coverage(cells * cells, 0);
    std::vector<uint16_t> player_bits(cells * cells, 1U << 3);
    coverage[1 * cells + 1] = 2;
    player_bits[2 * cells + 2] = 0;
    full::SightView sight;
    sight.coverage = coverage;
    sight.player_bits = player_bits;
    sight.width = cells;
    sight.height = cells;
    sight.viewer_bit = 1U << 3;
    sight.line_of_sight = true;
    sight.mapping = true;
    constexpr int32_t cell = wr::fog_cell_pixels;
    OA_CHECK(full::cell_fog(sight, cell + 5, cell + 5) == full::CellFog::seen);
    OA_CHECK(full::cell_fog(sight, 5, cell + 5) == full::CellFog::unseen);
    // Out of sight counts before never mapped: the gray goes under the black.
    OA_CHECK(full::cell_fog(sight, 2 * cell + 1, 2 * cell + 1) == full::CellFog::unseen);
    OA_CHECK(full::cell_fog(sight, cells * cell + 1, 1) == full::CellFog::seen);
    OA_CHECK(full::cell_fog(sight, -1, 5) == full::CellFog::seen);
    full::SightView no_sight = sight;
    no_sight.line_of_sight = false;
    OA_CHECK(full::cell_fog(no_sight, 5, cell + 5) == full::CellFog::seen);
    OA_CHECK(full::cell_fog(no_sight, 2 * cell + 1, 2 * cell + 1) == full::CellFog::unmapped);
    OA_CHECK(full::cell_fog(no_sight, cell + 5, cell + 5) == full::CellFog::seen);
    full::SightView no_mapping = sight;
    no_mapping.mapping = false;
    OA_CHECK(full::cell_fog(no_mapping, 2 * cell + 1, 2 * cell + 1) == full::CellFog::unseen);
    OA_CHECK(full::cell_fog({}, 5, 5) == full::CellFog::seen);
    // The gray table the fog grays by: each index to the entry nearest the
    // grey of its mean channel.
    std::array<uint8_t, 256> levels{};
    wr::build_gray_levels(entries, levels);
    std::array<uint8_t, gpu::gray_table_entries> gray{};
    for (std::size_t index = 0; index < gray.size(); ++index) {
        const auto& entry = entries.entries[index];
        gray[index] = levels[(static_cast<unsigned>(entry.r) + entry.g + entry.b) / 3U];
    }
    // The sprites: one in sight, one out of sight, one on never-mapped
    // ground that is out of sight too, which the line-of-sight rule grays
    // before the black pass goes over it, one on never-mapped ground with
    // the line-of-sight rule off, drawn in colour, and one off the grid.
    const RenderedFrame seen = block_frame(10, 10, 0, 0, 70);
    const RenderedFrame unseen = gradient_frame(12, 12, 0, 0, 128, 16);
    const RenderedFrame unmapped = block_frame(10, 10, 0, 0, 100);
    const RenderedFrame off_grid = block_frame(6, 6, 0, 0, 150);
    const RenderedFrame unseen_greyed = mapped_frame(unseen, gray);
    const RenderedFrame unmapped_greyed = mapped_frame(unmapped, gray);
    const auto scene = [&](const RenderedFrame& second, const RenderedFrame& third) {
        WorldDrawList list;
        add_sprite(list, seen, cell + 4, cell + 4, false);
        add_sprite(list, second, 4, cell + 4, false);
        add_sprite(list, third, 2 * cell + 4, 2 * cell + 4, false);
        add_sprite(list, off_grid, cells * cell + 2, cell + 4, false);
        return list;
    };
    const WorldDrawList list = scene(unseen, unmapped);
    model_render::ModelDisplay display;
    model_render::build_model_display(display, entries);
    const Picture background = grey_background(palette, field_width, field_height);
    const Picture colour_expected = draw_processor(list, palette, display, background, 1.0F);
    const Picture greyed_expected =
        draw_processor(scene(unseen_greyed, unmapped_greyed), palette, display, background, 1.0F);
    CardSide side(
        field_left + field_width + border, field_top + field_height + border, gpu::Limits{}
    );
    side.pages.set_palette(entries, plain_gamma);
    // Without a gray table every sprite is drawn in colour.
    const CardDraw plain = draw_card(side, list, palette, background, 1.0F, sight);
    OA_CHECK(plain.result.sprites == 4 && plain.result.greyed == 0);
    OA_CHECK(compare(plain.field, colour_expected, {}).most_beside == 0);
    side.pages.set_gray_table(gray);
    const CardDraw greyed = draw_card(side, list, palette, background, 1.0F, sight);
    OA_CHECK(greyed.result.sprites == 4 && greyed.result.greyed == 2);
    const Comparison comparison = compare(greyed.field, greyed_expected, {});
    std::printf(
        "fog states: %u sprites, %u greyed; most %d over %zu pixels\n",
        greyed.result.sprites,
        greyed.result.greyed,
        comparison.most_beside,
        comparison.beside
    );
    OA_CHECK(comparison.most_beside == 0);
    // With the line-of-sight rule off, never-mapped ground alone grays nothing.
    const CardDraw mapping_only = draw_card(side, list, palette, background, 1.0F, no_sight);
    OA_CHECK(mapping_only.result.sprites == 4 && mapping_only.result.greyed == 0);
    OA_CHECK(compare(mapping_only.field, colour_expected, {}).most_beside == 0);
    // With the mapping rule off, the ground out of sight is grayed alone.
    const CardDraw sight_only = draw_card(side, list, palette, background, 1.0F, no_mapping);
    OA_CHECK(sight_only.result.sprites == 4 && sight_only.result.greyed == 2);
    OA_CHECK(compare(sight_only.field, greyed_expected, {}).most_beside == 0);
    // Under the dithered option the fog dithers the ground out of sight
    // instead of greying it, and every sprite is drawn in colour.
    full::SightView dithered = sight;
    dithered.dithered = true;
    const CardDraw under_dither = draw_card(side, list, palette, background, 1.0F, dithered);
    OA_CHECK(under_dither.result.sprites == 4 && under_dither.result.greyed == 0);
    OA_CHECK(compare(under_dither.field, colour_expected, {}).most_beside == 0);
}

/// A frame whose distinct sprites exceed the pages' memory draws nothing,
/// and leaves the frame as it was, or, where another stage's batches lie
/// among the sprites', empties its own; with room for them, every sprite
/// draws.
void test_pages_overflow() {
    const auto palette = test_palette();
    std::vector<RenderedFrame> frames;
    for (uint8_t index = 0; index < 5; ++index)
        frames.push_back(block_frame(20, 20, 0, 0, static_cast<uint8_t>(64 + index * 9)));
    WorldDrawList list;
    for (std::size_t index = 0; index < frames.size(); ++index)
        add_sprite(list, frames[index], static_cast<int32_t>(index * 25), 10, false);
    const Picture background = grey_background(palette, field_width, field_height);
    // One page of 64 texels a side holds four cells of 24; five frames need
    // a second page the limit refuses.
    const gpu::Limits small{64, 64, std::size_t{64} * 64 * gpu::texel_bytes};
    {
        CardSide side(field_left + field_width + border, field_top + field_height + border, small);
        side.pages.set_palette(oa::present::palette_from_bytes(palette), plain_gamma);
        card::CardFrame frame;
        card::append_quad(frame, 0, 0, 1, 1, 0, 0, 1, 1, {});
        full::SpriteStageInputs inputs;
        inputs.list = &list;
        inputs.view.width = field_width;
        inputs.view.height = field_height;
        inputs.palette = &palette;
        const full::SpritePageHooks hooks{&side, &CardSide::page_for};
        const auto result = full::emit_sprites(inputs, side.pages, hooks, frame);
        OA_CHECK(result.pages_overflowed);
        OA_CHECK(result.sprites == 0 && result.batches == 0);
        OA_CHECK(frame.batches.empty() && frame.vertices.size() == 4 && frame.indices.size() == 6);
        OA_CHECK(side.pages.statistics().evictions > 0);
    }
    {
        // Another stage's batch among the sprites': the stage's batches are
        // emptied in place and name no page, so the frame runs whatever
        // became of the pages they named.
        CardSide side(field_left + field_width + border, field_top + field_height + border, small);
        side.pages.set_palette(oa::present::palette_from_bytes(palette), plain_gamma);
        card::CardFrame frame;
        full::SpriteStageInputs inputs;
        inputs.list = &list;
        inputs.view.width = field_width;
        inputs.view.height = field_height;
        inputs.palette = &palette;
        const full::SpritePageHooks hooks{&side, &CardSide::page_for};
        full::SpriteFrame sprites(inputs, side.pages, hooks, frame);
        std::size_t foreign = 0;
        for (std::size_t index = 0; index < list.draws.size(); ++index) {
            if (index == 2) {
                const auto first = static_cast<card::Index>(frame.indices.size());
                card::append_quad(frame, 0, 0, 1, 1, 0, 0, 1, 1, {});
                card::Batch other;
                other.operation = card::Operation::draw;
                other.first_index = first;
                other.index_count = static_cast<uint32_t>(frame.indices.size()) - first;
                frame.batches.push_back(other);
                foreign = frame.batches.size() - 1;
            }
            sprites.emit(list.draws[index]);
        }
        const auto result = sprites.finish();
        OA_CHECK(result.pages_overflowed && result.sprites == 0);
        for (std::size_t position = 0; position < frame.batches.size(); ++position) {
            const card::Batch& batch = frame.batches[position];
            if (position == foreign)
                OA_CHECK(batch.index_count == 6);
            else
                OA_CHECK(batch.index_count == 0 && batch.page == card::PageHandle{});
        }
        for (const card::PageHandle page : side.handles)
            side.executor.destroy_page(page);
        OA_CHECK(side.executor.execute(frame, nullptr));
    }
    {
        const gpu::Limits enough{64, 64, std::size_t{64} * 64 * gpu::texel_bytes * 2};
        CardSide side(field_left + field_width + border, field_top + field_height + border, enough);
        side.pages.set_palette(oa::present::palette_from_bytes(palette), plain_gamma);
        const CardDraw card = draw_card(side, list, palette, background, 1.0F);
        OA_CHECK(!card.result.pages_overflowed && card.result.sprites == 5);
        // Two pages, so two batches.
        OA_CHECK(card.result.batches == 2);
        model_render::ModelDisplay display;
        model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
        const Picture processor = draw_processor(list, palette, display, background, 1.0F);
        OA_CHECK(compare(card.field, processor, {}).most_beside == 0);
    }
}

/// Sprites the pages refuse, a sprite with no frame, a draw the host gives
/// no page for, and squares with no pixels.
void test_refusals() {
    const auto palette = test_palette();
    const RenderedFrame good = block_frame(8, 8, 0, 0, 70);
    RenderedFrame malformed = block_frame(8, 8, 0, 0, 70);
    malformed.pixels.pop_back();
    const RenderedFrame empty = block_frame(0, 0, 0, 0, 70);
    WorldDrawList list;
    add_sprite(list, good, 10, 10, false);
    add_sprite(list, malformed, 30, 10, false);
    add_sprite(list, empty, 50, 10, false);
    oa::app::SpriteDraw no_frame;
    no_frame.place = {70, 10};
    list.sprites.push_back(no_frame);
    oa::app::add_world_draw(list, WorldDrawKind::sprite, list.sprites.size() - 1);
    list.squares.push_back({10, 50, 10, 60, {}});
    oa::app::add_world_draw(list, WorldDrawKind::pixel_square, list.squares.size() - 1);
    const Picture background = grey_background(palette, field_width, field_height);
    CardSide side(
        field_left + field_width + border, field_top + field_height + border, gpu::Limits{}
    );
    side.pages.set_palette(oa::present::palette_from_bytes(palette), plain_gamma);
    const CardDraw card = draw_card(side, list, palette, background, 1.0F);
    OA_CHECK(card.result.sprites == 1 && card.result.refused == 3 && card.result.squares == 0);
    OA_CHECK(card.result.batches == 1);
    const CardDraw none = draw_card(side, list, palette, background, 1.0F, {}, nullptr, false);
    OA_CHECK(none.result.sprites == 0 && none.result.refused == 4 && none.result.batches == 0);
    OA_CHECK(compare(none.field, background, {}).most_beside == 0);
    // A list with no draws, and no battlefield, emits nothing.
    const WorldDrawList bare;
    const CardDraw nothing = draw_card(side, bare, palette, background, 1.0F);
    OA_CHECK(nothing.result.batches == 0 && nothing.frame.batches.empty());
    full::SpriteStageInputs inputs;
    inputs.list = &list;
    card::CardFrame frame;
    OA_CHECK(full::emit_sprites(inputs, side.pages, {}, frame).batches == 0);
    inputs.list = nullptr;
    inputs.view.width = field_width;
    inputs.view.height = field_height;
    OA_CHECK(full::emit_sprites(inputs, side.pages, {}, frame).batches == 0);
}

/// The stage's helpers: the kinds each stage takes, the sampling by zoom,
/// the line width, a flat colour through the gamma, and a line's quad.
void test_helpers() {
    for (const auto kind :
         {WorldDrawKind::sprite,
          WorldDrawKind::blended_sprite,
          WorldDrawKind::pixel_square,
          WorldDrawKind::line,
          WorldDrawKind::selection_line})
        OA_CHECK(full::sprite_kind(kind) && !full::model_kind(kind));
    for (const auto kind :
         {WorldDrawKind::model,
          WorldDrawKind::projectile,
          WorldDrawKind::debris,
          WorldDrawKind::fragment})
        OA_CHECK(full::model_kind(kind) && !full::sprite_kind(kind));
    for (const auto kind :
         {WorldDrawKind::commit, WorldDrawKind::commit_always, WorldDrawKind::lens})
        OA_CHECK(!full::sprite_kind(kind) && !full::model_kind(kind));
    OA_CHECK(full::sprite_sampling(1.0F) == card::Sampling::nearest);
    OA_CHECK(full::sprite_sampling(2.0F) == card::Sampling::nearest);
    OA_CHECK(full::sprite_sampling(4.0F) == card::Sampling::nearest);
    OA_CHECK(full::sprite_sampling(0.5F) == card::Sampling::linear);
    OA_CHECK(full::sprite_sampling(0.75F) == card::Sampling::linear);
    OA_CHECK(full::sprite_sampling(1.37F) == card::Sampling::pixel_art);
    OA_CHECK(full::sprite_sampling(2.5F) == card::Sampling::pixel_art);
    full::SceneView view;
    view.zoom = 0.5F;
    OA_CHECK(full::line_width(view) == 1.0F);
    view.zoom = 1.0F;
    OA_CHECK(full::line_width(view) == 1.0F);
    view.zoom = 2.5F;
    OA_CHECK(full::line_width(view) == 2.5F);
    view.scale = 2.0F;
    OA_CHECK(full::line_width(view) == 5.0F);
    std::array<uint8_t, 256> gamma{};
    for (std::size_t channel = 0; channel < gamma.size(); ++channel)
        gamma[channel] = static_cast<uint8_t>(std::min<std::size_t>(255, channel * 2));
    const card::Colour plain = full::flat_colour({255, 0, 51}, nullptr);
    OA_CHECK(plain.red == 1.0F && plain.green == 0.0F && plain.alpha == 1.0F);
    OA_CHECK(std::abs(plain.blue - 51.0F / 255.0F) < 1e-6F);
    const card::Colour lit = full::flat_colour({100, 200, 0}, &gamma);
    OA_CHECK(std::abs(lit.red - 200.0F / 255.0F) < 1e-6F && lit.green == 1.0F && lit.blue == 0.0F);
    // A line along a row, one pixel wide, from the centre of pixel 10 to
    // the centre of pixel 20 on row 5: the quad from column 10 to 21 and
    // row 5 to 6.
    card::CardFrame frame;
    full::append_line_quad(frame, 10.5F, 5.5F, 20.5F, 5.5F, 1.0F, {});
    OA_CHECK(frame.vertices.size() == 4 && frame.indices.size() == 6);
    float left = 1e9F, right = -1e9F, top = 1e9F, bottom = -1e9F;
    for (const auto& vertex : frame.vertices) {
        left = std::min(left, vertex.x);
        right = std::max(right, vertex.x);
        top = std::min(top, vertex.y);
        bottom = std::max(bottom, vertex.y);
    }
    OA_CHECK(left == 10.0F && right == 21.0F && top == 5.0F && bottom == 6.0F);
    // A line of one pixel runs along its row.
    card::CardFrame dot;
    full::append_line_quad(dot, 3.5F, 3.5F, 3.5F, 3.5F, 1.0F, {});
    OA_CHECK(dot.vertices[0].x == 3.0F && dot.vertices[1].x == 4.0F);
}

/// Draws a list of sprites as the design has the card draw them, on the
/// processor: opaque sprites by their coverage, blended ones to the true
/// mean of their colour and the pixel under them, rounded half up.
///
/// @param list the list
/// @param palette the palette
/// @param background the battlefield before the draws
/// @return the battlefield after them
Picture draw_reference(
    const WorldDrawList& list, const oa::PaletteBytes& palette, const Picture& background
) {
    Picture picture = background;
    for (const auto& draw : list.draws) {
        if (draw.kind != WorldDrawKind::sprite && draw.kind != WorldDrawKind::blended_sprite)
            continue;
        const auto& sprite = list.sprites[draw.index];
        const RenderedFrame& frame = *sprite.frame;
        const bool blended = draw.kind == WorldDrawKind::blended_sprite;
        for (uint16_t y = 0; y < frame.height; ++y)
            for (uint16_t x = 0; x < frame.width; ++x) {
                const std::size_t offset = std::size_t{y} * frame.width + x;
                if (frame.coverage[offset] == 0)
                    continue;
                const int64_t column = int64_t{sprite.place.x} - frame.origin_x + x;
                const int64_t row = int64_t{sprite.place.y} - frame.origin_y + y;
                if (column < 0 || row < 0 || column >= picture.width || row >= picture.height)
                    continue;
                const auto colour = entry_colour(palette, frame.pixels[offset]);
                auto shown = colour;
                if (blended) {
                    const auto under =
                        picture.at(static_cast<uint32_t>(column), static_cast<uint32_t>(row));
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        shown[channel] = static_cast<uint8_t>(
                            (int{colour[channel]} + int{under[channel]} + 1) / 2
                        );
                }
                picture.set(static_cast<uint32_t>(column), static_cast<uint32_t>(row), shown);
            }
    }
    return picture;
}

/// A linear congruential stream for the installed scene.
struct Stream {
    uint32_t state{};

    /// Returns the next number.
    ///
    /// @return bits 16 to 30 of the next state
    uint32_t next() {
        state = state * 214013U + 2531011U;
        return (state >> 16) & 0x7fffU;
    }
};

/// A scene of the installed game's GAF frames through its palette and
/// alpha table: the card's picture equals the processor's beside the
/// blended sprites and the reference's within the blend tolerance under
/// them.
///
/// @param assets the installed game
void test_installed_scene(const oa::AssetStore& assets) {
    const auto palette_bytes = oa::test::read_game_file(assets, "palettes/PALETTE.PAL");
    OA_CHECK(palette_bytes.size() >= oa::palette_color_count * oa::palette_entry_bytes);
    if (palette_bytes.size() < oa::palette_color_count * oa::palette_entry_bytes)
        return;
    oa::PaletteBytes palette{};
    std::copy_n(palette_bytes.begin(), palette.size(), palette.begin());
    // Frames of the effects and of the first feature animations found,
    // every one the GAF reader renders, up to a few hundred.
    std::vector<RenderedFrame> frames;
    std::vector<std::string> names{"anims/FX.GAF"};
    for (const auto& name : assets.list_effective_recursive("anims", ".gaf"))
        if (names.size() < 6 && name != names.front())
            names.push_back(name);
    for (const auto& name : names) {
        const auto bytes = oa::test::read_game_file(assets, name);
        const auto parsed = oa::formats::gaf::parse(bytes);
        if (!parsed.ok())
            continue;
        for (const auto& sequence : parsed.archive->sequences)
            for (const auto& source : sequence.frames) {
                if (frames.size() >= 400)
                    break;
                auto rendered = oa::formats::gaf::render_normal(source);
                if (rendered.ok() && rendered.frame->width > 0 && rendered.frame->height > 0 &&
                    rendered.frame->width <= 128 && rendered.frame->height <= 128)
                    frames.push_back(std::move(*rendered.frame));
            }
    }
    std::printf("installed scene: %zu frames from %zu files\n", frames.size(), names.size());
    OA_CHECK(!frames.empty());
    if (frames.empty())
        return;
    Stream stream{7};
    WorldDrawList list;
    for (uint32_t index = 0; index < data_sprites; ++index) {
        const auto& frame = frames[stream.next() % frames.size()];
        const int32_t x = static_cast<int32_t>(stream.next() % (data_field_width + 40)) - 20;
        const int32_t y = static_cast<int32_t>(stream.next() % (data_field_height + 40)) - 20;
        add_sprite(list, frame, x, y, index % data_blend_every == 0);
    }
    Picture background(data_field_width, data_field_height, {});
    for (uint32_t y = 0; y < background.height; ++y)
        for (uint32_t x = 0; x < background.width; ++x)
            background.set(
                x,
                y,
                entry_colour(palette, static_cast<uint8_t>((x / 8U * 31U + y / 8U * 17U) % 256U))
            );
    model_render::ModelDisplay display;
    model_render::build_model_display(display, oa::present::palette_from_bytes(palette));
    const Picture processor = draw_processor(list, palette, display, background, 1.0F);
    const Picture reference = draw_reference(list, palette, background);
    CardSide side(
        field_left + data_field_width + border,
        field_top + data_field_height + border,
        gpu::Limits{}
    );
    side.pages.set_palette(oa::present::palette_from_bytes(palette), plain_gamma);
    const CardDraw card = draw_card(side, list, palette, background, 1.0F);
    OA_CHECK(card.outside_untouched);
    OA_CHECK(card.result.sprites == data_sprites && card.result.refused == 0);
    OA_CHECK(!card.result.pages_overflowed);
    const auto blended = sprite_mask(list, background.width, background.height, 1.0F, false);
    const Comparison against_processor = compare(card.field, processor, blended);
    const Comparison against_reference = compare(card.field, reference, blended);
    // Under the blends, within the tolerance for each blend over a pixel,
    // since each rounds its own way.
    const auto counts = blend_counts(list, background.width, background.height);
    const int excess = most_blend_excess(card.field, reference, counts);
    std::printf(
        "installed scene: %u sprites in %u batches; beside the blends most %d against the "
        "processor over %zu pixels; under them most %d against the reference over %zu pixels, "
        "exceeding the tolerance a blend by %d, and %d against the processor's alpha table\n",
        card.result.sprites,
        card.result.batches,
        against_processor.most_beside,
        against_processor.beside,
        against_reference.most_under,
        against_reference.under,
        excess,
        against_processor.most_under
    );
    OA_CHECK(against_processor.beside > 0 && against_processor.most_beside == 0);
    OA_CHECK(against_reference.most_beside == 0);
    OA_CHECK(against_reference.under > 0 && excess <= 0);
}

} // namespace

int main(int argc, char** argv) {
    if (!SDL_Init(0)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (oa::test::game_data_requested(argc, argv)) {
        const auto assets = oa::test::require_game_assets("the installed game's sprite scene");
        test_installed_scene(assets);
    } else {
        test_helpers();
        test_synthetic_scene(1.0F);
        test_synthetic_scene(2.0F);
        test_shadow_sprites();
        test_features_on_the_map(1.0F);
        test_features_on_the_map(2.0F);
        test_lines(1.0F);
        test_lines(2.0F);
        test_fog_states();
        test_pages_overflow();
        test_refusals();
    }
    SDL_Quit();
    return oa::test::check_exit_status();
}
