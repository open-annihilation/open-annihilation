// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_live_overlay.hpp"

#include "oa/present/game_text.hpp"

#include <algorithm>
#include <cstddef>

namespace oa::app::renderer_live_overlay {
namespace {

constexpr uint32_t max_dimension = 16384;
/// frontend_renderer::blend_opaque: a blend's whole, in 256ths.
constexpr uint32_t blend_whole = 256;

/// Tells whether a colour is darker than another by the sum of its
/// channels, as lay_text tells it of the outline.
bool darker(const std::array<uint8_t, 3>& colour, const std::array<uint8_t, 3>& than) noexcept {
    return colour[0] + colour[1] + colour[2] < than[0] + than[1] + than[2];
}

/// An operation's rectangle within the picture, its end exclusive.
struct Cut {
    uint32_t left{};
    uint32_t top{};
    uint32_t right{};
    uint32_t bottom{};
};

/// Cuts an operation's rectangle to the picture.
///
/// @return false where nothing of it lies in the picture
bool cut(const Operation& operation, uint32_t width, uint32_t height, Cut& within) noexcept {
    const int64_t left = std::max<int64_t>(operation.x, 0);
    const int64_t top = std::max<int64_t>(operation.y, 0);
    const int64_t right =
        std::min<int64_t>(int64_t{operation.x} + std::max(operation.width, 0), width);
    const int64_t bottom =
        std::min<int64_t>(int64_t{operation.y} + std::max(operation.height, 0), height);
    if (left >= right || top >= bottom)
        return false;
    within = {
        static_cast<uint32_t>(left),
        static_cast<uint32_t>(top),
        static_cast<uint32_t>(right),
        static_cast<uint32_t>(bottom)
    };
    return true;
}

} // namespace

std::array<uint8_t, 3> apply(const Operation& operation, std::array<uint8_t, 3> pixel) noexcept {
    switch (operation.kind) {
    case Kind::blend: {
        const uint32_t share = std::min(operation.amount, blend_whole);
        const uint32_t keep = blend_whole - share;
        for (std::size_t channel = 0; channel < pixel.size(); ++channel)
            pixel[channel] = static_cast<uint8_t>(
                (pixel[channel] * keep + operation.colour[channel] * share + blend_whole / 2) /
                blend_whole
            );
        return pixel;
    }
    case Kind::shadow:
        return oa::present::blend_color(
            pixel, oa::present::text_shade_color, oa::present::text_shadow_alpha
        );
    case Kind::outline:
        return darker(pixel, oa::present::text_outline_color) ? oa::present::text_dark_outline_color
                                                              : oa::present::text_outline_color;
    case Kind::letter:
        return oa::present::blend_color(
            pixel, operation.colour, static_cast<uint8_t>(std::min<uint32_t>(operation.amount, 255))
        );
    }
    return pixel;
}

std::array<uint8_t, 3>
reduce(std::span<const uint8_t> palette, std::array<uint8_t, 3> colour) noexcept {
    if (palette.size() < palette_entry_bytes)
        return colour;
    const auto index = oa::present::nearest_palette_index(palette, palette_entry_bytes, colour);
    const std::size_t at = std::size_t{index} * palette_entry_bytes;
    return {palette[at], palette[at + 1], palette[at + 2]};
}

void compose(
    std::span<uint8_t> picture,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> canvas,
    std::array<uint8_t, 3> key,
    std::span<const Operation> operations,
    std::span<const uint8_t> palette
) {
    const std::size_t pixels = std::size_t{width} * height;
    if (picture.size() < pixels * 3 || canvas.size() < pixels * 3)
        return;
    const auto keyed = [&](std::size_t pixel) {
        return std::equal(key.begin(), key.end(), canvas.begin() + std::ptrdiff_t(pixel * 3));
    };
    // The pixels a line's operations reached, to reduce once they are all
    // made: a line's operations come one after another.
    std::vector<uint8_t> reached;
    uint32_t line = 0;
    Cut line_area{};
    const auto end_line = [&] {
        if (line == 0)
            return;
        for (uint32_t y = line_area.top; y < line_area.bottom; ++y)
            for (uint32_t x = line_area.left; x < line_area.right; ++x) {
                const std::size_t pixel = std::size_t{y} * width + x;
                if (reached[pixel] == 0)
                    continue;
                reached[pixel] = 0;
                uint8_t* at = picture.data() + pixel * 3;
                const auto reduced = reduce(palette, {at[0], at[1], at[2]});
                std::copy(reduced.begin(), reduced.end(), at);
            }
        line = 0;
    };
    for (const Operation& operation : operations) {
        if (operation.line != line)
            end_line();
        Cut within{};
        if (!cut(operation, width, height, within))
            continue;
        if (operation.line != 0 && line == 0) {
            if (reached.size() != pixels)
                reached.assign(pixels, 0);
            line = operation.line;
            line_area = within;
        } else if (line != 0) {
            line_area.left = std::min(line_area.left, within.left);
            line_area.top = std::min(line_area.top, within.top);
            line_area.right = std::max(line_area.right, within.right);
            line_area.bottom = std::max(line_area.bottom, within.bottom);
        }
        for (uint32_t y = within.top; y < within.bottom; ++y)
            for (uint32_t x = within.left; x < within.right; ++x) {
                const std::size_t pixel = std::size_t{y} * width + x;
                if (!keyed(pixel))
                    continue;
                uint8_t* at = picture.data() + pixel * 3;
                const auto made = apply(operation, {at[0], at[1], at[2]});
                std::copy(made.begin(), made.end(), at);
                if (line != 0)
                    reached[pixel] = 1;
            }
    }
    end_line();
    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
        if (!keyed(pixel))
            std::copy_n(
                canvas.begin() + std::ptrdiff_t(pixel * 3),
                3,
                picture.begin() + std::ptrdiff_t(pixel * 3)
            );
}

void pack(std::span<const Operation> operations, uint32_t width, uint32_t height, Packed& packed) {
    packed.operations.clear();
    packed.lists.clear();
    packed.count = 0;
    packed.tiles_across = 0;
    packed.tiles_down = 0;
    if (width == 0 || height == 0 || width > max_dimension || height > max_dimension)
        return;
    packed.tiles_across = (width + tile_edge - 1) / tile_edge;
    packed.tiles_down = (height + tile_edge - 1) / tile_edge;
    const std::size_t tiles = std::size_t{packed.tiles_across} * packed.tiles_down;
    // Each operation's tiles, counted first, so that the lists are laid one
    // after another in a single pass.
    std::vector<Cut> cuts;
    cuts.reserve(operations.size());
    std::vector<uint32_t> counts(tiles + 1, 0);
    for (const Operation& operation : operations) {
        Cut within{};
        if (!cut(operation, width, height, within))
            continue;
        cuts.push_back(within);
        packed.operations.insert(
            packed.operations.end(),
            {within.left,
             within.top,
             within.right - 1,
             within.bottom - 1,
             static_cast<uint32_t>(operation.kind),
             uint32_t{operation.colour[0]} | uint32_t{operation.colour[1]} << 8 |
                 uint32_t{operation.colour[2]} << 16,
             operation.amount,
             operation.line}
        );
        for (uint32_t row = within.top / tile_edge; row <= (within.bottom - 1) / tile_edge; ++row)
            for (uint32_t column = within.left / tile_edge;
                 column <= (within.right - 1) / tile_edge;
                 ++column)
                ++counts[std::size_t{row} * packed.tiles_across + column];
    }
    packed.count = static_cast<uint32_t>(cuts.size());
    // Where each tile's list starts, after the starts themselves.
    const std::size_t starts = tiles + 1;
    packed.lists.resize(starts);
    uint32_t next = static_cast<uint32_t>(starts);
    for (std::size_t tile = 0; tile < tiles; ++tile) {
        packed.lists[tile] = next;
        next += counts[tile];
        counts[tile] = packed.lists[tile];
    }
    packed.lists[tiles] = next;
    packed.lists.resize(next);
    for (uint32_t index = 0; index < packed.count; ++index) {
        const Cut& within = cuts[index];
        for (uint32_t row = within.top / tile_edge; row <= (within.bottom - 1) / tile_edge; ++row)
            for (uint32_t column = within.left / tile_edge;
                 column <= (within.right - 1) / tile_edge;
                 ++column)
                packed.lists[counts[std::size_t{row} * packed.tiles_across + column]++] = index;
    }
}

} // namespace oa::app::renderer_live_overlay
