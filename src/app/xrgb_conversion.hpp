// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The conversion of the game's RGB frames into the 32-bit pixels the window
// shows (SDL_PIXELFORMAT_XRGB8888, 0xXXRRGGBB), or into the 16-bit pixels of a
// 16-bit window (SDL_PIXELFORMAT_RGB565), through the display gamma's table
// when the gamma is not 1, in bands of rows on the drawing threads; and of
// the pixels painted over a picture into an overlay that holds only them.
#pragma once

#include "oa/platform/job_pool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::app {

/// Rows of the frame each band of convert_rgb24_xrgb converts.
inline constexpr uint32_t xrgb_band_rows = 32;

/// Packs one RGB24 row into XRGB8888 words (0xffRRGGBB).
///
/// Four pixels are read as three little-endian words to keep the loop short.
///
/// @param[out] out `width` opaque words
/// @param rgb 3 bytes per pixel
/// @param width pixels in the row
void pack_rgb24_row(uint32_t* out, const uint8_t* rgb, int width) noexcept;

/// Maps packed XRGB pixels through the display gamma's per-channel table.
///
/// @param[in,out] row `width` packed pixels; the top byte is kept
/// @param width pixels in the row
/// @param table the gamma table, applied alike to red, green and blue
void gamma_xrgb_row(uint32_t* row, int width, const std::array<uint8_t, 256>& table) noexcept;

/// Converts an RGB24 frame into XRGB8888 rows, in bands of xrgb_band_rows rows.
///
/// Every row is the same whichever thread converts it.
///
/// @param rgb the frame, `width` * 3 bytes a row, rows one after another
/// @param width pixels in a row
/// @param height rows
/// @param[out] pixels the converted rows, each at least `width` * 4 bytes, 4-byte aligned
/// @param pitch bytes from one row of `pixels` to the next
/// @param gamma the display gamma's table; null when the gamma is 1
/// @param pool threads to convert the bands on; null converts them on the calling thread
void convert_rgb24_xrgb(
    const uint8_t* rgb,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    platform::job_pool::Pool* pool
) noexcept;

/// Converts a rectangle of an RGB24 frame into XRGB8888 rows, in bands of
/// xrgb_band_rows rows, exactly as convert_rgb24_xrgb converts the same rows
/// and columns of the whole frame.
///
/// Every row is the same whichever thread converts it.
///
/// @param rgb the rectangle's first pixel, 3 bytes a pixel
/// @param rgb_pitch bytes from one row of the frame to the next, at least `width` * 3
/// @param width pixels in a row of the rectangle
/// @param height rows of the rectangle
/// @param[out] pixels the converted rows, each at least `width` * 4 bytes, 4-byte aligned
/// @param pitch bytes from one row of `pixels` to the next
/// @param gamma the display gamma's table; null when the gamma is 1
/// @param pool threads to convert the bands on; null converts them on the calling thread
void convert_rgb24_xrgb_rect(
    const uint8_t* rgb,
    std::size_t rgb_pitch,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    platform::job_pool::Pool* pool
) noexcept;

/// The opaque alpha of an overlay pixel.
inline constexpr uint32_t overlay_opaque = 0xff000000U;

/// Converts what was painted over a picture into an ARGB8888 overlay, in
/// bands of xrgb_band_rows rows: 0, transparent, where the painted picture
/// (the canvas) equals the picture under it (the base), and the canvas's
/// colour through the gamma table, opaque (overlay_opaque), where it does
/// not. Notes, for each band, whether it holds an opaque pixel.
///
/// Every row is the same whichever thread converts it.
///
/// @param canvas the painted picture, `width` * 3 bytes a row, rows one after another
/// @param base the picture it was painted over, the same size
/// @param width pixels in a row
/// @param height rows
/// @param[out] pixels the overlay's rows, each at least `width` * 4 bytes, 4-byte aligned
/// @param pitch bytes from one row of `pixels` to the next
/// @param gamma the display gamma's table; null when the gamma is 1
/// @param[out] opaque_bands one entry for each band of xrgb_band_rows rows, at least
///        ceil(height / xrgb_band_rows) of them: 1 where the band holds an
///        opaque pixel, else 0
/// @param pool threads to convert the bands on; null converts them on the calling thread
void convert_rgb24_overlay_argb(
    const uint8_t* canvas,
    const uint8_t* base,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    std::span<uint8_t> opaque_bands,
    platform::job_pool::Pool* pool
) noexcept;

/// Bytes one entry of a palette takes in the game's palette bytes: red,
/// green, blue and a fourth byte the colour does not use.
inline constexpr std::size_t palette_entry_bytes = 4;

/// Finds the colour an overlay canvas is cleared to: the lowest 24-bit
/// colour, counted as 0xRRGGBB from black up, that no entry of the palette
/// holds, so that a painter, which writes palette colours, can never paint
/// it. A palette of 256 colours leaves it at or below 0x000100.
///
/// @param palette the palette's bytes, palette_entry_bytes an entry; a
///        trailing part entry, and entries past the 256th, are ignored
/// @return the key colour, red, green and blue
[[nodiscard]] std::array<uint8_t, 3> overlay_key_colour(std::span<const uint8_t> palette) noexcept;

/// Tells, for each band of xrgb_band_rows rows, whether two RGB24 frames of
/// one size differ in it, so that only the bands that changed are uploaded
/// again. Nothing is written when `changed` has too few entries.
///
/// @param rgb the frame, `width` * 3 bytes a row, rows one after another
/// @param previous the frame it is compared with, the same size
/// @param width pixels in a row
/// @param height rows
/// @param[out] changed one entry for each band, at least
///        ceil(height / xrgb_band_rows) of them: 1 where the frames differ
///        in the band, else 0
void find_changed_rgb24_bands(
    const uint8_t* rgb,
    const uint8_t* previous,
    uint32_t width,
    uint32_t height,
    std::span<uint8_t> changed
) noexcept;

/// Fills an RGB24 picture with one colour, as overlay canvases are cleared
/// to their key colour (overlay_key_colour).
///
/// @param[out] rgb the picture, 3 bytes a pixel
/// @param pixels pixels in the picture
/// @param colour the colour, red, green and blue
void fill_rgb24(uint8_t* rgb, std::size_t pixels, std::array<uint8_t, 3> colour) noexcept;

/// Converts a canvas cleared to a key colour and painted over into an
/// ARGB8888 overlay, in bands of xrgb_band_rows rows: 0, transparent, where
/// the canvas still holds the key, and the canvas's colour through the
/// gamma table, opaque (overlay_opaque), where it does not. Notes, for each
/// band, whether it holds an opaque pixel, as convert_rgb24_overlay_argb
/// does.
///
/// Every row is the same whichever thread converts it.
///
/// @param canvas the painted canvas, `width` * 3 bytes a row, rows one after another
/// @param key the colour the canvas was cleared to (overlay_key_colour)
/// @param width pixels in a row
/// @param height rows
/// @param[out] pixels the overlay's rows, each at least `width` * 4 bytes, 4-byte aligned
/// @param pitch bytes from one row of `pixels` to the next
/// @param gamma the display gamma's table; null when the gamma is 1
/// @param[out] opaque_bands one entry for each band of xrgb_band_rows rows, at least
///        ceil(height / xrgb_band_rows) of them: 1 where the band holds an
///        opaque pixel, else 0
/// @param pool threads to convert the bands on; null converts them on the calling thread
void convert_rgb24_keyed_overlay_argb(
    const uint8_t* canvas,
    std::array<uint8_t, 3> key,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    std::span<uint8_t> opaque_bands,
    platform::job_pool::Pool* pool
) noexcept;

/// Packs one RGB24 row into RGB565 words: red's top 5 bits, green's top 6 and
/// blue's top 5, from the top bit down, each channel through the gamma table
/// first when there is one; the 16-bit pixels a window of that format shows
/// for the frame's XRGB8888 ones.
///
/// @param[out] out `width` words
/// @param rgb 3 bytes per pixel
/// @param width pixels in the row
/// @param gamma the display gamma's table; null when the gamma is 1
void pack_rgb24_rgb565_row(
    uint16_t* out, const uint8_t* rgb, int width, const std::array<uint8_t, 256>* gamma
) noexcept;

/// Converts an RGB24 frame into RGB565 rows (pack_rgb24_rgb565_row), in bands
/// of xrgb_band_rows rows.
///
/// Every row is the same whichever thread converts it.
///
/// @param rgb the frame, `width` * 3 bytes a row, rows one after another
/// @param width pixels in a row
/// @param height rows
/// @param[out] pixels the converted rows, each at least `width` * 2 bytes, 2-byte aligned
/// @param pitch bytes from one row of `pixels` to the next
/// @param gamma the display gamma's table; null when the gamma is 1
/// @param pool threads to convert the bands on; null converts them on the calling thread
void convert_rgb24_rgb565(
    const uint8_t* rgb,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    platform::job_pool::Pool* pool
) noexcept;

/// Converts a rectangle of an RGB24 frame into RGB565 rows
/// (pack_rgb24_rgb565_row), in bands of xrgb_band_rows rows, exactly as
/// convert_rgb24_rgb565 converts the same rows and columns of the whole
/// frame.
///
/// Every row is the same whichever thread converts it.
///
/// @param rgb the rectangle's first pixel, 3 bytes a pixel
/// @param rgb_pitch bytes from one row of the frame to the next, at least `width` * 3
/// @param width pixels in a row of the rectangle
/// @param height rows of the rectangle
/// @param[out] pixels the converted rows, each at least `width` * 2 bytes, 2-byte aligned
/// @param pitch bytes from one row of `pixels` to the next
/// @param gamma the display gamma's table; null when the gamma is 1
/// @param pool threads to convert the bands on; null converts them on the calling thread
void convert_rgb24_rgb565_rect(
    const uint8_t* rgb,
    std::size_t rgb_pitch,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    platform::job_pool::Pool* pool
) noexcept;

} // namespace oa::app
