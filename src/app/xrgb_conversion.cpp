// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "xrgb_conversion.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace oa::app {

void pack_rgb24_row(uint32_t* out, const uint8_t* rgb, int width) noexcept {
    static_assert(std::endian::native == std::endian::little);
    constexpr uint32_t opaque = 0xff000000u;
    int x = 0;
    for (; x + 4 <= width; x += 4, rgb += 12, out += 4) {
        uint32_t first = 0, second = 0, third = 0;
        std::memcpy(&first, rgb, 4);
        std::memcpy(&second, rgb + 4, 4);
        std::memcpy(&third, rgb + 8, 4);
        out[0] = opaque | ((first & 0xffu) << 16) | (first & 0xff00u) | ((first >> 16) & 0xffu);
        out[1] =
            opaque | ((first >> 8) & 0xff0000u) | ((second & 0xffu) << 8) | ((second >> 8) & 0xffu);
        out[2] = opaque | (second & 0xff0000u) | ((second >> 16) & 0xff00u) | (third & 0xffu);
        out[3] = opaque | ((third << 8) & 0xff0000u) | ((third >> 8) & 0xff00u) | (third >> 24);
    }
    for (; x < width; ++x, rgb += 3)
        *out++ = opaque | (static_cast<uint32_t>(rgb[0]) << 16) |
                 (static_cast<uint32_t>(rgb[1]) << 8) | static_cast<uint32_t>(rgb[2]);
}

void gamma_xrgb_row(uint32_t* row, int width, const std::array<uint8_t, 256>& table) noexcept {
    for (int x = 0; x < width; ++x) {
        const uint32_t pixel = row[x];
        row[x] = (pixel & 0xff000000u) |
                 (static_cast<uint32_t>(table[(pixel >> 16) & 0xffu]) << 16) |
                 (static_cast<uint32_t>(table[(pixel >> 8) & 0xffu]) << 8) |
                 static_cast<uint32_t>(table[pixel & 0xffu]);
    }
}

void convert_rgb24_xrgb(
    const uint8_t* rgb,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    platform::job_pool::Pool* pool
) noexcept {
    convert_rgb24_xrgb_rect(
        rgb, static_cast<std::size_t>(width) * 3U, width, height, pixels, pitch, gamma, pool
    );
}

void convert_rgb24_xrgb_rect(
    const uint8_t* rgb,
    std::size_t rgb_pitch,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    platform::job_pool::Pool* pool
) noexcept {
    const auto row_pixels = static_cast<int>(width);
    platform::job_pool::run_bands(
        pool, platform::job_pool::bands_of_rows(height, xrgb_band_rows), [&](uint32_t band) {
            const uint32_t first_row = band * xrgb_band_rows;
            const uint32_t end_row = std::min(height, first_row + xrgb_band_rows);
            for (uint32_t y = first_row; y < end_row; ++y) {
                auto* row =
                    reinterpret_cast<uint32_t*>(pixels + static_cast<std::size_t>(y) * pitch);
                pack_rgb24_row(row, rgb + static_cast<std::size_t>(y) * rgb_pitch, row_pixels);
                if (gamma != nullptr)
                    gamma_xrgb_row(row, row_pixels, *gamma);
            }
        }
    );
}

namespace {

/// Most palette entries overlay_key_colour reads.
constexpr std::size_t most_palette_entries = 256;

/// Most bytes fill_rgb24 copies at once: whole pixels, few enough that the
/// block copied from stays in the processor's cache.
constexpr std::size_t fill_rgb24_block_bytes = 3U * 16384U;

/// Converts one row of a painted canvas into ARGB8888 overlay words:
/// transparent where `transparent` says a pixel was not painted, else the
/// canvas's colour, through the gamma table when `apply_gamma` holds, opaque.
///
/// @param painted the row's first pixel, 3 bytes a pixel
/// @param at the byte offset of the row's first pixel in the canvas
/// @param width pixels in the row
/// @param[out] row the row's `width` words
/// @param table the gamma table; read only when `apply_gamma` holds
/// @param transparent tells, from a pixel's bytes and its byte offset in the
///        canvas, whether it is left transparent
/// @return true when the row holds an opaque pixel
template <bool apply_gamma, typename Transparent>
bool convert_overlay_row(
    const uint8_t* painted,
    std::size_t at,
    uint32_t width,
    uint32_t* row,
    const std::array<uint8_t, 256>* table,
    Transparent& transparent
) noexcept {
    bool opaque = false;
    for (uint32_t x = 0; x < width; ++x, painted += 3, at += 3) {
        if (transparent(painted, at)) {
            row[x] = 0;
            continue;
        }
        opaque = true;
        if constexpr (apply_gamma)
            row[x] = overlay_opaque | (static_cast<uint32_t>((*table)[painted[0]]) << 16) |
                     (static_cast<uint32_t>((*table)[painted[1]]) << 8) |
                     static_cast<uint32_t>((*table)[painted[2]]);
        else
            row[x] = overlay_opaque | (static_cast<uint32_t>(painted[0]) << 16) |
                     (static_cast<uint32_t>(painted[1]) << 8) | static_cast<uint32_t>(painted[2]);
    }
    return opaque;
}

/// Converts a painted canvas into an ARGB8888 overlay in bands: transparent
/// where `transparent` says a pixel was not painted, else the canvas's
/// colour through the gamma table, opaque; the bands that hold an opaque
/// pixel noted. A row `row_transparent` finds wholly unpainted is cleared
/// at once, without its pixels being looked at one by one.
///
/// @param canvas the painted canvas, `width` * 3 bytes a row
/// @param width pixels in a row
/// @param height rows
/// @param[out] pixels the overlay's rows
/// @param pitch bytes from one row of `pixels` to the next
/// @param gamma the display gamma's table; null when the gamma is 1
/// @param[out] opaque_bands 1 where a band holds an opaque pixel, else 0
/// @param pool threads to convert the bands on; null for the calling thread
/// @param transparent tells, from a pixel's bytes and its byte offset in the
///        canvas, whether it is left transparent
/// @param row_transparent tells, from a row's first pixel, its byte offset
///        in the canvas and its width, whether every pixel of the row is
///        left transparent
template <typename Transparent, typename RowTransparent>
void convert_overlay_bands(
    const uint8_t* canvas,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    std::span<uint8_t> opaque_bands,
    platform::job_pool::Pool* pool,
    Transparent&& transparent,
    RowTransparent&& row_transparent
) noexcept {
    const uint32_t bands = platform::job_pool::bands_of_rows(height, xrgb_band_rows);
    if (opaque_bands.size() < bands)
        return;
    platform::job_pool::run_bands(pool, bands, [&](uint32_t band) {
        const uint32_t first_row = band * xrgb_band_rows;
        const uint32_t end_row = std::min(height, first_row + xrgb_band_rows);
        bool opaque = false;
        for (uint32_t y = first_row; y < end_row; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(pixels + static_cast<std::size_t>(y) * pitch);
            const auto at = static_cast<std::size_t>(y) * width * 3U;
            const uint8_t* painted = canvas + at;
            if (row_transparent(painted, at, width)) {
                std::memset(row, 0, static_cast<std::size_t>(width) * sizeof(uint32_t));
                continue;
            }
            const bool row_opaque =
                gamma != nullptr
                    ? convert_overlay_row<true>(painted, at, width, row, gamma, transparent)
                    : convert_overlay_row<false>(painted, at, width, row, gamma, transparent);
            opaque = opaque || row_opaque;
        }
        opaque_bands[band] = opaque ? 1 : 0;
    });
}

} // namespace

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
) noexcept {
    convert_overlay_bands(
        canvas,
        width,
        height,
        pixels,
        pitch,
        gamma,
        opaque_bands,
        pool,
        [base](const uint8_t* painted, std::size_t at) {
            const uint8_t* under = base + at;
            return painted[0] == under[0] && painted[1] == under[1] && painted[2] == under[2];
        },
        [base](const uint8_t* painted, std::size_t at, uint32_t width) {
            return std::memcmp(painted, base + at, static_cast<std::size_t>(width) * 3U) == 0;
        }
    );
}

std::array<uint8_t, 3> overlay_key_colour(std::span<const uint8_t> palette) noexcept {
    // The colours the palette holds, as 0xRRGGBB, sorted; the key is the
    // first value from 0 up that the sorted colours skip.
    std::array<uint32_t, most_palette_entries> held{};
    std::size_t count = 0;
    const std::size_t entries = std::min(palette.size() / palette_entry_bytes, held.size());
    for (std::size_t entry = 0; entry < entries; ++entry) {
        const uint8_t* rgb = palette.data() + entry * palette_entry_bytes;
        held[count++] = (static_cast<uint32_t>(rgb[0]) << 16) |
                        (static_cast<uint32_t>(rgb[1]) << 8) | static_cast<uint32_t>(rgb[2]);
    }
    std::sort(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(count));
    uint32_t key = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (held[index] > key)
            break;
        if (held[index] == key)
            ++key;
    }
    return {
        static_cast<uint8_t>(key >> 16), static_cast<uint8_t>(key >> 8), static_cast<uint8_t>(key)
    };
}

void find_changed_rgb24_bands(
    const uint8_t* rgb,
    const uint8_t* previous,
    uint32_t width,
    uint32_t height,
    std::span<uint8_t> changed
) noexcept {
    const uint32_t bands = platform::job_pool::bands_of_rows(height, xrgb_band_rows);
    if (changed.size() < bands)
        return;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 3U;
    for (uint32_t band = 0; band < bands; ++band) {
        const uint32_t first_row = band * xrgb_band_rows;
        const uint32_t end_row = std::min(height, first_row + xrgb_band_rows);
        const std::size_t at = static_cast<std::size_t>(first_row) * row_bytes;
        const std::size_t bytes = static_cast<std::size_t>(end_row - first_row) * row_bytes;
        changed[band] = std::memcmp(rgb + at, previous + at, bytes) != 0 ? 1 : 0;
    }
}

void fill_rgb24(uint8_t* rgb, std::size_t pixels, std::array<uint8_t, 3> colour) noexcept {
    if (pixels == 0)
        return;
    // One pixel written, then what is filled copied after itself, doubling
    // each time, and past fill_rgb24_block_bytes the first block copied on
    // until the picture is full.
    const std::size_t total = pixels * 3U;
    std::memcpy(rgb, colour.data(), 3);
    std::size_t filled = 3;
    while (filled < total) {
        const std::size_t copied = std::min({filled, total - filled, fill_rgb24_block_bytes});
        std::memcpy(rgb + filled, rgb, copied);
        filled += copied;
    }
}

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
) noexcept {
    convert_overlay_bands(
        canvas,
        width,
        height,
        pixels,
        pitch,
        gamma,
        opaque_bands,
        pool,
        [key](const uint8_t* painted, std::size_t) {
            return painted[0] == key[0] && painted[1] == key[1] && painted[2] == key[2];
        },
        [key](const uint8_t* painted, std::size_t, uint32_t width) {
            // A row holds the key alone when its first pixel is the key and
            // each of its bytes equals the byte one pixel further on.
            return width == 0 ||
                   (std::memcmp(painted, key.data(), 3) == 0 &&
                    std::memcmp(
                        painted, painted + 3, (static_cast<std::size_t>(width) - 1U) * 3U
                    ) == 0);
        }
    );
}

void pack_rgb24_rgb565_row(
    uint16_t* out, const uint8_t* rgb, int width, const std::array<uint8_t, 256>* gamma
) noexcept {
    // Each channel's top bits and where they go in the word.
    constexpr unsigned red_drop = 3, green_drop = 2, blue_drop = 3;
    constexpr unsigned red_shift = 11, green_shift = 5;
    const auto pack = [](unsigned red, unsigned green, unsigned blue) {
        return static_cast<uint16_t>(
            ((red >> red_drop) << red_shift) | ((green >> green_drop) << green_shift) |
            (blue >> blue_drop)
        );
    };
    if (gamma == nullptr) {
        for (int x = 0; x < width; ++x, rgb += 3)
            out[x] = pack(rgb[0], rgb[1], rgb[2]);
        return;
    }
    const auto& table = *gamma;
    for (int x = 0; x < width; ++x, rgb += 3)
        out[x] = pack(table[rgb[0]], table[rgb[1]], table[rgb[2]]);
}

void convert_rgb24_rgb565(
    const uint8_t* rgb,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    platform::job_pool::Pool* pool
) noexcept {
    convert_rgb24_rgb565_rect(
        rgb, static_cast<std::size_t>(width) * 3U, width, height, pixels, pitch, gamma, pool
    );
}

void convert_rgb24_rgb565_rect(
    const uint8_t* rgb,
    std::size_t rgb_pitch,
    uint32_t width,
    uint32_t height,
    uint8_t* pixels,
    std::size_t pitch,
    const std::array<uint8_t, 256>* gamma,
    platform::job_pool::Pool* pool
) noexcept {
    const auto row_pixels = static_cast<int>(width);
    platform::job_pool::run_bands(
        pool, platform::job_pool::bands_of_rows(height, xrgb_band_rows), [&](uint32_t band) {
            const uint32_t first_row = band * xrgb_band_rows;
            const uint32_t end_row = std::min(height, first_row + xrgb_band_rows);
            for (uint32_t y = first_row; y < end_row; ++y)
                pack_rgb24_rgb565_row(
                    reinterpret_cast<uint16_t*>(pixels + static_cast<std::size_t>(y) * pitch),
                    rgb + static_cast<std::size_t>(y) * rgb_pitch,
                    row_pixels,
                    gamma
                );
        }
    );
}

} // namespace oa::app
