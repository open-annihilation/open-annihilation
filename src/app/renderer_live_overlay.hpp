// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the processor paints over the turned 3D view while the picture stays
// on the graphics card (renderer_live_gpu.cpp): the overlay canvas, cleared
// to a key colour and painted as the battlefield is painted, and, where the
// canvas holds its key, the operations the painters would have made on the
// picture had they painted on it, in the order they made them. The card adds
// both to its picture, as the processor would have painted them, and the
// processor can add them to the picture read back (compose); the tiles each
// operation reaches are laid out for the card here (pack).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::app::renderer_live_overlay {

/// What an operation does to a pixel of the picture under the canvas.
enum class Kind : uint32_t {
    /// frontend_renderer::blend_rect: the colour at `amount` 256ths, at most
    /// the whole, rounded to nearest.
    blend = 0,
    /// lay_text's shadow: toward oa::present::text_shade_color at
    /// oa::present::text_shadow_alpha.
    shadow = 1,
    /// lay_text's outline: oa::present::text_dark_outline_color over a pixel
    /// darker than oa::present::text_outline_color, else that colour.
    outline = 2,
    /// lay_text's letters: the colour at `amount` 255ths of coverage.
    letter = 3,
};

/// An operation on a rectangle of the picture under the canvas, in the
/// canvas's pixels.
struct Operation {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
    Kind kind{Kind::blend};
    std::array<uint8_t, 3> colour{}; ///< blend's and the letters' colour
    uint32_t amount{};               ///< blend's opacity or the letters' coverage
    /// The line of text a shadow, outline or letter belongs to, numbered
    /// from 1 in the order the lines were laid; 0 for blend. A pixel a
    /// line's operations reach is reduced to the palette once they have all
    /// been made on it, as lay_text reduces the pixels it lays.
    uint32_t line{};

    friend bool operator==(const Operation&, const Operation&) = default;
};

/// Bytes of an entry of the palette lines are reduced to: red, green, blue
/// and one more, as oa::present::nearest_palette_index takes it.
inline constexpr std::size_t palette_entry_bytes = 4;

/// Makes one operation on a pixel, as its painter would have painted it.
///
/// @param operation the operation
/// @param pixel the pixel's red, green and blue
/// @return the pixel after it, before any reduction to the palette
[[nodiscard]] std::array<uint8_t, 3>
apply(const Operation& operation, std::array<uint8_t, 3> pixel) noexcept;

/// Gives the palette's colour nearest a colour, as lay_text reduces it.
///
/// @param palette palette_entry_bytes an entry, at most 256 entries read
/// @param colour the colour
/// @return the entry's red, green and blue; the colour where the palette is
///     empty
[[nodiscard]] std::array<uint8_t, 3>
reduce(std::span<const uint8_t> palette, std::array<uint8_t, 3> colour) noexcept;

/// Adds the overlay to a picture, as the painters would have painted it
/// there: each pixel the canvas paints is the canvas's; every other takes
/// the operations that reach it, in their order, each line of text reduced
/// to the palette once its operations have been made.
///
/// @param[in,out] picture red, green and blue, `width` * `height` pixels
/// @param width pixels in a row
/// @param height rows
/// @param canvas the overlay canvas, the picture's size
/// @param key the colour the canvas was cleared to
/// @param operations the operations, in the order made
/// @param palette the palette lines are reduced to; empty for none
void compose(
    std::span<uint8_t> picture,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> canvas,
    std::array<uint8_t, 3> key,
    std::span<const Operation> operations,
    std::span<const uint8_t> palette
);

/// The edge of the square tiles of the picture each of which lists the
/// operations reaching it, in pixels.
inline constexpr uint32_t tile_edge = 16;

/// RGBA texels of 32-bit words an operation takes in the operations' table.
inline constexpr uint32_t texels_per_operation = 2;

/// Operations a row of the operations' table holds.
inline constexpr uint32_t operations_per_row = 1024;

/// Words a row of the lists' table holds.
inline constexpr uint32_t list_words_per_row = 4096;

/// The operations laid out as the card reads them.
struct Packed {
    /// texels_per_operation RGBA texels an operation, operations_per_row a
    /// row, in their order: its first column and row and its last column
    /// and row, within the picture; its kind, its colour as red | green << 8
    /// | blue << 16, its amount and its line.
    std::vector<uint32_t> operations{};
    uint32_t count{}; ///< operations in the table, those that reach the picture
    uint32_t tiles_across{};
    uint32_t tiles_down{};
    /// list_words_per_row words a row: where each tile's list starts, a word
    /// a tile, row after row of tiles, and where the last ends; then the
    /// lists, each the numbers of the operations reaching its tile, in
    /// their order.
    std::vector<uint32_t> lists{};
};

/// Lays out the operations for the card: those reaching the picture, cut to
/// it, and for each tile those that reach it.
///
/// @param operations the operations, in the order made
/// @param width the picture's width, from 1 through 16384
/// @param height the picture's height, from 1 through 16384
/// @param[out] packed the layout; no operation and no tile where the
///     picture's size is out of range
void pack(std::span<const Operation> operations, uint32_t width, uint32_t height, Packed& packed);

} // namespace oa::app::renderer_live_overlay
