// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The conversion of RGB frames into the window's XRGB8888 pixels: each pixel
// packed as 0xffRRGGBB through the gamma table, and the same bytes on pools
// of every size; and into a 16-bit window's RGB565 pixels, each the one SDL
// makes of the XRGB8888 pixel, on pools of every size; a rectangle of a frame
// as the same rows and columns of the whole; the front end's frame through
// the gamma table as opaque ARGB8888 pixels of the corrected colours. Rows
// of a picture held in a wider one convert as the picture alone does,
// opaque, so they fill ARGB8888 textures; and an overlay of what was
// painted over a picture is transparent where nothing changed and the
// painted colour through the gamma table, opaque, where something did, its
// bands noted, the same on every pool. A picture fills with one colour, and
// a gamma table that keeps every level converts as no table does; the bands
// two frames differ in are found.
#include "xrgb_conversion.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check_at(bool condition, const char* expression, const char* file, int line) {
    if (condition)
        return;
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    ++failures;
}

#define CHECK(condition) check_at((condition), #condition, __FILE__, __LINE__)

namespace job_pool = oa::platform::job_pool;

/// Thread counts the conversion runs on besides the calling thread alone.
constexpr uint32_t pool_sizes[] = {2, 3, 4, 8};
/// Seed of the test's frames.
constexpr uint32_t test_seed = 1001;
/// Bytes past each converted row that the conversion must leave alone.
constexpr std::size_t row_slack = 12;
/// Value of the bytes the conversion must leave alone.
constexpr uint8_t untouched = 0x5a;

/// Converts a frame into rows `row_slack` bytes wider than its pixels.
std::vector<uint8_t> convert(
    const std::vector<uint8_t>& rgb,
    uint32_t width,
    uint32_t height,
    const std::array<uint8_t, 256>* gamma,
    job_pool::Pool* pool
) {
    const std::size_t pitch = static_cast<std::size_t>(width) * 4U + row_slack;
    std::vector<uint8_t> pixels(pitch * height, untouched);
    oa::app::convert_rgb24_xrgb(rgb.data(), width, height, pixels.data(), pitch, gamma, pool);
    return pixels;
}

void conversion_packs_each_pixel() {
    std::mt19937 random(test_seed);
    std::array<uint8_t, 256> gamma{};
    for (std::size_t level = 0; level < gamma.size(); ++level)
        gamma[level] = static_cast<uint8_t>(255U - level / 2U);
    // Widths that end on and off the four-pixel step.
    for (const uint32_t width : {1U, 3U, 4U, 7U, 640U, 1001U}) {
        const uint32_t height = 77;
        std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3U);
        for (auto& byte : rgb)
            byte = static_cast<uint8_t>(random());
        for (const bool corrected : {false, true}) {
            const auto pixels = convert(rgb, width, height, corrected ? &gamma : nullptr, nullptr);
            const std::size_t pitch = static_cast<std::size_t>(width) * 4U + row_slack;
            bool all = true;
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const uint8_t* source = &rgb[(static_cast<std::size_t>(y) * width + x) * 3U];
                    const auto channel = [&](int index) -> uint32_t {
                        return corrected ? gamma[source[index]] : source[index];
                    };
                    const uint32_t expected =
                        0xff000000u | (channel(0) << 16) | (channel(1) << 8) | channel(2);
                    uint32_t pixel = 0;
                    std::memcpy(&pixel, &pixels[y * pitch + x * 4U], 4);
                    all = all && pixel == expected;
                }
                for (std::size_t spare = width * 4U; spare < pitch; ++spare)
                    all = all && pixels[y * pitch + spare] == untouched;
            }
            CHECK(all);
        }
    }
}

void conversion_is_the_same_on_every_pool() {
    std::mt19937 random(test_seed + 1);
    std::array<uint8_t, 256> gamma{};
    for (std::size_t level = 0; level < gamma.size(); ++level)
        gamma[level] = static_cast<uint8_t>((level * level) / 255U);
    std::vector<std::unique_ptr<job_pool::Pool>> pools;
    for (const uint32_t threads : pool_sizes)
        pools.push_back(std::make_unique<job_pool::Pool>(threads));
    // Frames of one band, of a part band and of many.
    for (const auto& [width, height] : {std::pair{640U, 32U}, {1664U, 952U}, {333U, 101U}}) {
        std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3U);
        for (auto& byte : rgb)
            byte = static_cast<uint8_t>(random());
        const std::array<uint8_t, 256>* const tables[] = {nullptr, &gamma};
        for (const auto* table : tables) {
            const auto alone = convert(rgb, width, height, table, nullptr);
            for (const auto& pool : pools)
                CHECK(convert(rgb, width, height, table, pool.get()) == alone);
        }
    }
}

/// Converts a frame into RGB565 rows `row_slack` bytes wider than its pixels.
std::vector<uint8_t> convert_rgb565(
    const std::vector<uint8_t>& rgb,
    uint32_t width,
    uint32_t height,
    const std::array<uint8_t, 256>* gamma,
    job_pool::Pool* pool
) {
    const std::size_t pitch = static_cast<std::size_t>(width) * 2U + row_slack;
    std::vector<uint8_t> pixels(pitch * height, untouched);
    oa::app::convert_rgb24_rgb565(rgb.data(), width, height, pixels.data(), pitch, gamma, pool);
    return pixels;
}

void rgb565_is_what_sdl_makes_of_xrgb() {
    std::mt19937 random(test_seed + 2);
    std::array<uint8_t, 256> gamma{};
    for (std::size_t level = 0; level < gamma.size(); ++level)
        gamma[level] = static_cast<uint8_t>(255U - level / 3U);
    std::vector<std::unique_ptr<job_pool::Pool>> pools;
    for (const uint32_t threads : pool_sizes)
        pools.push_back(std::make_unique<job_pool::Pool>(threads));
    for (const auto& [width, height] : {std::pair{1U, 5U}, {7U, 33U}, {640U, 77U}, {1001U, 65U}}) {
        std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3U);
        for (auto& byte : rgb)
            byte = static_cast<uint8_t>(random());
        for (const bool corrected : {false, true}) {
            const auto* table = corrected ? &gamma : nullptr;
            const auto xrgb = convert(rgb, width, height, table, nullptr);
            const auto rgb565 = convert_rgb565(rgb, width, height, table, nullptr);
            const std::size_t xrgb_pitch = static_cast<std::size_t>(width) * 4U + row_slack;
            const std::size_t pitch = static_cast<std::size_t>(width) * 2U + row_slack;
            std::vector<uint8_t> expected(pitch * height, untouched);
            for (uint32_t y = 0; y < height; ++y)
                CHECK(SDL_ConvertPixels(
                    static_cast<int>(width),
                    1,
                    SDL_PIXELFORMAT_XRGB8888,
                    &xrgb[y * xrgb_pitch],
                    static_cast<int>(xrgb_pitch),
                    SDL_PIXELFORMAT_RGB565,
                    &expected[y * pitch],
                    static_cast<int>(pitch)
                ));
            CHECK(rgb565 == expected);
            for (const auto& pool : pools)
                CHECK(convert_rgb565(rgb, width, height, table, pool.get()) == rgb565);
        }
    }
}

/// Converts a rectangle of a frame, in XRGB8888 or RGB565, into rows
/// `row_slack` bytes wider than its pixels.
std::vector<uint8_t> convert_rect(
    const std::vector<uint8_t>& rgb,
    uint32_t frame_width,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    bool rgb565,
    const std::array<uint8_t, 256>* gamma,
    job_pool::Pool* pool
) {
    const std::size_t pixel_bytes = rgb565 ? 2U : 4U;
    const std::size_t pitch = static_cast<std::size_t>(width) * pixel_bytes + row_slack;
    std::vector<uint8_t> pixels(pitch * height, untouched);
    const std::size_t rgb_pitch = static_cast<std::size_t>(frame_width) * 3U;
    const uint8_t* first = rgb.data() + static_cast<std::size_t>(y) * rgb_pitch + x * 3U;
    const auto convert =
        rgb565 ? oa::app::convert_rgb24_rgb565_rect : oa::app::convert_rgb24_xrgb_rect;
    convert(first, rgb_pitch, width, height, pixels.data(), pitch, gamma, pool);
    return pixels;
}

void rectangle_is_that_part_of_the_whole() {
    std::mt19937 random(test_seed + 3);
    std::array<uint8_t, 256> gamma{};
    for (std::size_t level = 0; level < gamma.size(); ++level)
        gamma[level] = static_cast<uint8_t>(level ^ 0x35U);
    std::vector<std::unique_ptr<job_pool::Pool>> pools;
    for (const uint32_t threads : pool_sizes)
        pools.push_back(std::make_unique<job_pool::Pool>(threads));
    const uint32_t frame_width = 1001, frame_height = 97;
    std::vector<uint8_t> rgb(static_cast<std::size_t>(frame_width) * frame_height * 3U);
    for (auto& byte : rgb)
        byte = static_cast<uint8_t>(random());

    // Rectangles at odd offsets, of widths on and off the four-pixel step,
    // over one band and several.
    struct Rect {
        uint32_t x, y, width, height;
    };

    for (const Rect rect :
         {Rect{0, 0, frame_width, frame_height},
          Rect{1, 3, 5, 2},
          Rect{333, 7, 640, 77},
          Rect{997, 95, 4, 2},
          Rect{513, 31, 1, 33}}) {
        for (const bool rgb565 : {false, true}) {
            const std::size_t pixel_bytes = rgb565 ? 2U : 4U;
            const std::array<uint8_t, 256>* const tables[] = {nullptr, &gamma};
            for (const auto* table : tables) {
                const auto whole =
                    rgb565 ? convert_rgb565(rgb, frame_width, frame_height, table, nullptr)
                           : convert(rgb, frame_width, frame_height, table, nullptr);
                const std::size_t whole_pitch =
                    static_cast<std::size_t>(frame_width) * pixel_bytes + row_slack;
                const auto part = convert_rect(
                    rgb,
                    frame_width,
                    rect.x,
                    rect.y,
                    rect.width,
                    rect.height,
                    rgb565,
                    table,
                    nullptr
                );
                const std::size_t pitch =
                    static_cast<std::size_t>(rect.width) * pixel_bytes + row_slack;
                bool all = true;
                for (uint32_t row = 0; row < rect.height; ++row) {
                    all = all && std::memcmp(
                                     &part[row * pitch],
                                     &whole[(rect.y + row) * whole_pitch + rect.x * pixel_bytes],
                                     rect.width * pixel_bytes
                                 ) == 0;
                    for (std::size_t spare = rect.width * pixel_bytes; spare < pitch; ++spare)
                        all = all && part[row * pitch + spare] == untouched;
                }
                CHECK(all);
                for (const auto& pool : pools)
                    CHECK(
                        convert_rect(
                            rgb,
                            frame_width,
                            rect.x,
                            rect.y,
                            rect.width,
                            rect.height,
                            rgb565,
                            table,
                            pool.get()
                        ) == part
                    );
            }
        }
    }
}

void front_end_frame_serves_as_argb() {
    // The 640x480 front-end frame: today it is corrected byte by byte through
    // the gamma table and uploaded as RGB24; converted with the table, every
    // pixel holds the same colour, opaque, as SDL reads it in ARGB8888.
    const uint32_t width = 640, height = 480;
    std::mt19937 random(test_seed + 4);
    std::array<uint8_t, 256> gamma{};
    for (std::size_t level = 0; level < gamma.size(); ++level)
        gamma[level] = static_cast<uint8_t>(255U - (level * 7U) % 256U);
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3U);
    for (auto& byte : rgb)
        byte = static_cast<uint8_t>(random());
    std::vector<uint8_t> corrected = rgb;
    for (auto& byte : corrected)
        byte = gamma[byte];
    const std::size_t pitch = static_cast<std::size_t>(width) * 4U;
    std::vector<uint8_t> pixels(pitch * height);
    oa::app::convert_rgb24_xrgb_rect(
        rgb.data(),
        static_cast<std::size_t>(width) * 3U,
        width,
        height,
        pixels.data(),
        pitch,
        &gamma,
        nullptr
    );
    bool opaque = true;
    for (std::size_t pixel = 0; pixel < pixels.size(); pixel += 4U) {
        uint32_t word = 0;
        std::memcpy(&word, &pixels[pixel], 4);
        opaque = opaque && (word >> 24) == 0xffU;
    }
    CHECK(opaque);
    std::vector<uint8_t> read_back(static_cast<std::size_t>(width) * height * 3U);
    CHECK(SDL_ConvertPixels(
        static_cast<int>(width),
        static_cast<int>(height),
        SDL_PIXELFORMAT_ARGB8888,
        pixels.data(),
        static_cast<int>(pitch),
        SDL_PIXELFORMAT_RGB24,
        read_back.data(),
        static_cast<int>(width * 3U)
    ));
    CHECK(read_back == corrected);
    std::vector<std::unique_ptr<job_pool::Pool>> pools;
    for (const uint32_t threads : pool_sizes)
        pools.push_back(std::make_unique<job_pool::Pool>(threads));
    for (const auto& pool : pools) {
        std::vector<uint8_t> banded(pitch * height);
        oa::app::convert_rgb24_xrgb_rect(
            rgb.data(),
            static_cast<std::size_t>(width) * 3U,
            width,
            height,
            banded.data(),
            pitch,
            &gamma,
            pool.get()
        );
        CHECK(banded == pixels);
    }
}

void rows_of_a_wider_picture_convert_alike() {
    std::mt19937 random(test_seed + 3);
    std::array<uint8_t, 256> gamma{};
    for (std::size_t level = 0; level < gamma.size(); ++level)
        gamma[level] = static_cast<uint8_t>(level / 2U + 64U);
    // A 333x101 part of a 400-pixel-wide picture, from its 17th column.
    constexpr uint32_t whole_width = 400;
    constexpr uint32_t width = 333;
    constexpr uint32_t height = 101;
    constexpr uint32_t first_column = 17;
    std::vector<uint8_t> whole(static_cast<std::size_t>(whole_width) * height * 3U);
    for (auto& byte : whole)
        byte = static_cast<uint8_t>(random());
    std::vector<uint8_t> part(static_cast<std::size_t>(width) * height * 3U);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(
            &part[static_cast<std::size_t>(y) * width * 3U],
            &whole[(static_cast<std::size_t>(y) * whole_width + first_column) * 3U],
            width * 3U
        );
    std::vector<std::unique_ptr<job_pool::Pool>> pools;
    for (const uint32_t threads : pool_sizes)
        pools.push_back(std::make_unique<job_pool::Pool>(threads));
    const std::size_t pitch = static_cast<std::size_t>(width) * 4U + row_slack;
    const std::array<uint8_t, 256>* const tables[] = {nullptr, &gamma};
    for (const auto* table : tables) {
        const auto expected = convert(part, width, height, table, nullptr);
        for (job_pool::Pool* pool : {static_cast<job_pool::Pool*>(nullptr), pools.back().get()}) {
            std::vector<uint8_t> pixels(pitch * height, untouched);
            oa::app::convert_rgb24_xrgb_rect(
                &whole[first_column * 3U],
                static_cast<std::size_t>(whole_width) * 3U,
                width,
                height,
                pixels.data(),
                pitch,
                table,
                pool
            );
            CHECK(pixels == expected);
        }
    }
}

void overlay_holds_what_was_painted() {
    std::mt19937 random(test_seed + 4);
    std::array<uint8_t, 256> gamma{};
    for (std::size_t level = 0; level < gamma.size(); ++level)
        gamma[level] = static_cast<uint8_t>(std::min<std::size_t>(255U, level + level / 4U));
    constexpr uint32_t width = 217;
    constexpr uint32_t height = 150; // five bands, the last a part one
    const uint32_t bands = job_pool::bands_of_rows(height, oa::app::xrgb_band_rows);
    std::vector<uint8_t> base(static_cast<std::size_t>(width) * height * 3U);
    for (auto& byte : base)
        byte = static_cast<uint8_t>(random());
    // Paint a few pixels in the second and the last band, one of them with
    // the colour already under it, and nothing elsewhere.
    std::vector<uint8_t> canvas = base;
    const auto paint = [&](uint32_t x, uint32_t y, std::array<uint8_t, 3> colour) {
        std::memcpy(&canvas[(static_cast<std::size_t>(y) * width + x) * 3U], colour.data(), 3);
    };
    paint(5, 40, {1, 2, 3});
    paint(216, 63, {200, 100, 50});
    paint(0, 149, {9, 9, 9});
    paint(
        100,
        140,
        {base[(140U * width + 100U) * 3U],
         base[(140U * width + 100U) * 3U + 1],
         base[(140U * width + 100U) * 3U + 2]}
    );
    std::vector<std::unique_ptr<job_pool::Pool>> pools;
    for (const uint32_t threads : pool_sizes)
        pools.push_back(std::make_unique<job_pool::Pool>(threads));
    const std::size_t pitch = static_cast<std::size_t>(width) * 4U + row_slack;
    const std::array<uint8_t, 256>* const tables[] = {nullptr, &gamma};
    for (const auto* table : tables) {
        std::vector<uint8_t> alone(pitch * height, untouched);
        std::vector<uint8_t> alone_bands(bands, 7);
        oa::app::convert_rgb24_overlay_argb(
            canvas.data(),
            base.data(),
            width,
            height,
            alone.data(),
            pitch,
            table,
            alone_bands,
            nullptr
        );
        bool all = true;
        uint32_t opaque = 0;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const auto at = (static_cast<std::size_t>(y) * width + x) * 3U;
                const bool changed = std::memcmp(&canvas[at], &base[at], 3) != 0;
                const auto level = [&](std::size_t index) -> uint32_t {
                    return table != nullptr ? (*table)[canvas[index]] : canvas[index];
                };
                const uint32_t expected = changed ? oa::app::overlay_opaque | (level(at) << 16) |
                                                        (level(at + 1) << 8) | level(at + 2)
                                                  : 0U;
                uint32_t pixel = 0;
                std::memcpy(&pixel, &alone[y * pitch + x * 4U], 4);
                all = all && pixel == expected;
                opaque += changed ? 1U : 0U;
            }
            for (std::size_t spare = width * 4U; spare < pitch; ++spare)
                all = all && alone[y * pitch + spare] == untouched;
        }
        CHECK(all);
        CHECK(opaque == 3);
        CHECK((alone_bands == std::vector<uint8_t>{0, 1, 0, 0, 1}));
        for (const auto& pool : pools) {
            std::vector<uint8_t> pixels(pitch * height, untouched);
            std::vector<uint8_t> pool_bands(bands, 7);
            oa::app::convert_rgb24_overlay_argb(
                canvas.data(),
                base.data(),
                width,
                height,
                pixels.data(),
                pitch,
                table,
                pool_bands,
                pool.get()
            );
            CHECK(pixels == alone);
            CHECK(pool_bands == alone_bands);
        }
    }
    // Too few band entries: nothing is written.
    std::vector<uint8_t> pixels(pitch * height, untouched);
    std::vector<uint8_t> short_bands(bands - 1, 7);
    oa::app::convert_rgb24_overlay_argb(
        canvas.data(),
        base.data(),
        width,
        height,
        pixels.data(),
        pitch,
        nullptr,
        short_bands,
        nullptr
    );
    CHECK(pixels == std::vector<uint8_t>(pitch * height, untouched));
    CHECK(short_bands == std::vector<uint8_t>(bands - 1, 7));
}

void key_colour_is_the_first_outside_the_palette() {
    // Entries are 4 bytes; the fourth byte is not a colour.
    const auto entry = [](uint8_t red, uint8_t green, uint8_t blue) {
        return std::array<uint8_t, 4>{red, green, blue, 0x7f};
    };
    const auto key_of = [](const std::vector<std::array<uint8_t, 4>>& entries) {
        std::vector<uint8_t> bytes;
        for (const auto& colour : entries)
            bytes.insert(bytes.end(), colour.begin(), colour.end());
        return oa::app::overlay_key_colour(bytes);
    };
    // No palette: black.
    CHECK((key_of({}) == std::array<uint8_t, 3>{0, 0, 0}));
    // Black held, in any order and more than once: the next blue.
    CHECK(
        (key_of({entry(0, 0, 2), entry(0, 0, 0), entry(0, 0, 1), entry(0, 0, 0)}) ==
         std::array<uint8_t, 3>{0, 0, 3})
    );
    // A gap before the highest held colour is taken, whatever the fourth byte.
    CHECK((
        key_of({entry(0, 0, 0), entry(0, 0, 1), entry(0, 0, 3)}) == std::array<uint8_t, 3>{0, 0, 2}
    ));
    // Every blue of black's green: the key carries into green.
    std::vector<std::array<uint8_t, 4>> blues;
    for (int blue = 0; blue < 256; ++blue)
        blues.push_back(entry(0, 0, static_cast<uint8_t>(blue)));
    CHECK((key_of(blues) == std::array<uint8_t, 3>{0, 1, 0}));
    // The game's palette of 256 colours leaves a key at or below 0x000100.
    const auto key = key_of(blues);
    CHECK(key[0] == 0 && (key[1] == 0 || (key[1] == 1 && key[2] == 0)));
    // A trailing part entry is ignored.
    const std::vector<uint8_t> part{0, 0, 0, 0, 0, 0, 1};
    CHECK((oa::app::overlay_key_colour(part) == std::array<uint8_t, 3>{0, 0, 1}));
    // The key is never a colour the palette holds.
    std::mt19937 random(test_seed + 5);
    for (int round = 0; round < 20; ++round) {
        std::vector<uint8_t> bytes(256U * 4U);
        for (auto& byte : bytes)
            byte = static_cast<uint8_t>(random() % 4U);
        const auto found = oa::app::overlay_key_colour(bytes);
        bool held = false;
        for (std::size_t at = 0; at < bytes.size(); at += 4)
            held = held || (bytes[at] == found[0] && bytes[at + 1] == found[1] &&
                            bytes[at + 2] == found[2]);
        CHECK(!held);
    }
}

void keyed_overlay_holds_what_was_painted() {
    std::mt19937 random(test_seed + 6);
    std::array<uint8_t, 256> gamma{};
    for (std::size_t level = 0; level < gamma.size(); ++level)
        gamma[level] = static_cast<uint8_t>(std::min<std::size_t>(255U, level + level / 4U));
    constexpr uint32_t width = 203;
    constexpr uint32_t height = 130; // five bands, the last a part one
    constexpr std::array<uint8_t, 3> key{0, 0, 3};
    const uint32_t bands = job_pool::bands_of_rows(height, oa::app::xrgb_band_rows);
    // A canvas cleared to the key, with a few pixels painted in the first
    // and the fourth band, one of them a colour one byte off the key.
    std::vector<uint8_t> canvas(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t at = 0; at < canvas.size(); at += 3)
        std::memcpy(&canvas[at], key.data(), 3);
    const auto paint = [&](uint32_t x, uint32_t y, std::array<uint8_t, 3> colour) {
        std::memcpy(&canvas[(static_cast<std::size_t>(y) * width + x) * 3U], colour.data(), 3);
    };
    paint(0, 0, {1, 2, 3});
    paint(202, 31, {0, 0, 2});
    paint(77, 100, {255, 255, 255});
    paint(78, 100, {0, 0, 4});
    std::vector<std::unique_ptr<job_pool::Pool>> pools;
    for (const uint32_t threads : pool_sizes)
        pools.push_back(std::make_unique<job_pool::Pool>(threads));
    const std::size_t pitch = static_cast<std::size_t>(width) * 4U + row_slack;
    const std::array<uint8_t, 256>* const tables[] = {nullptr, &gamma};
    for (const auto* table : tables) {
        std::vector<uint8_t> alone(pitch * height, untouched);
        std::vector<uint8_t> alone_bands(bands, 7);
        oa::app::convert_rgb24_keyed_overlay_argb(
            canvas.data(), key, width, height, alone.data(), pitch, table, alone_bands, nullptr
        );
        bool all = true;
        uint32_t opaque = 0;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const auto at = (static_cast<std::size_t>(y) * width + x) * 3U;
                const bool painted = std::memcmp(&canvas[at], key.data(), 3) != 0;
                const auto level = [&](std::size_t index) -> uint32_t {
                    return table != nullptr ? (*table)[canvas[index]] : canvas[index];
                };
                const uint32_t expected = painted ? oa::app::overlay_opaque | (level(at) << 16) |
                                                        (level(at + 1) << 8) | level(at + 2)
                                                  : 0U;
                uint32_t pixel = 0;
                std::memcpy(&pixel, &alone[y * pitch + x * 4U], 4);
                all = all && pixel == expected;
                opaque += painted ? 1U : 0U;
            }
            for (std::size_t spare = width * 4U; spare < pitch; ++spare)
                all = all && alone[y * pitch + spare] == untouched;
        }
        CHECK(all);
        CHECK(opaque == 4);
        CHECK((alone_bands == std::vector<uint8_t>{1, 0, 0, 1, 0}));
        for (const auto& pool : pools) {
            std::vector<uint8_t> pixels(pitch * height, untouched);
            std::vector<uint8_t> pool_bands(bands, 7);
            oa::app::convert_rgb24_keyed_overlay_argb(
                canvas.data(),
                key,
                width,
                height,
                pixels.data(),
                pitch,
                table,
                pool_bands,
                pool.get()
            );
            CHECK(pixels == alone);
            CHECK(pool_bands == alone_bands);
        }
    }
    // The key painted over its own base converts as the base conversion
    // converts a canvas that equals its base: the same transparent words
    // and bands.
    {
        std::vector<uint8_t> base(canvas.size());
        for (std::size_t at = 0; at < base.size(); at += 3)
            std::memcpy(&base[at], key.data(), 3);
        std::vector<uint8_t> keyed(pitch * height, untouched);
        std::vector<uint8_t> keyed_bands(bands, 7);
        std::vector<uint8_t> by_difference(pitch * height, untouched);
        std::vector<uint8_t> difference_bands(bands, 7);
        oa::app::convert_rgb24_keyed_overlay_argb(
            canvas.data(), key, width, height, keyed.data(), pitch, &gamma, keyed_bands, nullptr
        );
        oa::app::convert_rgb24_overlay_argb(
            canvas.data(),
            base.data(),
            width,
            height,
            by_difference.data(),
            pitch,
            &gamma,
            difference_bands,
            nullptr
        );
        CHECK(keyed == by_difference);
        CHECK(keyed_bands == difference_bands);
    }
    // Too few band entries: nothing is written.
    std::vector<uint8_t> pixels(pitch * height, untouched);
    std::vector<uint8_t> short_bands(bands - 1, 7);
    oa::app::convert_rgb24_keyed_overlay_argb(
        canvas.data(), key, width, height, pixels.data(), pitch, nullptr, short_bands, nullptr
    );
    CHECK(pixels == std::vector<uint8_t>(pitch * height, untouched));
    CHECK(short_bands == std::vector<uint8_t>(bands - 1, 7));
}

/// Fills pictures of many sizes with one colour, each pixel that colour and
/// nothing past the picture written.
void fill_writes_every_pixel() {
    constexpr std::array<uint8_t, 3> colour{0, 1, 0};
    for (const std::size_t pixels : {0U, 1U, 2U, 5U, 640U, 16384U, 16385U, 70001U}) {
        std::vector<uint8_t> rgb(pixels * 3U + row_slack, untouched);
        oa::app::fill_rgb24(rgb.data(), pixels, colour);
        bool all = true;
        for (std::size_t pixel = 0; pixel < pixels; ++pixel)
            all = all && std::memcmp(&rgb[pixel * 3U], colour.data(), 3) == 0;
        for (std::size_t spare = pixels * 3U; spare < rgb.size(); ++spare)
            all = all && rgb[spare] == untouched;
        CHECK(all);
    }
}

/// The bands two frames differ in are found: none for equal frames, the
/// band of a changed pixel, the last band a part one, and nothing written
/// with too few entries.
void changed_bands_are_found() {
    constexpr uint32_t width = 13;
    constexpr uint32_t height = 70; // three bands, the last a part one
    const uint32_t bands = job_pool::bands_of_rows(height, oa::app::xrgb_band_rows);
    std::mt19937 random(test_seed + 8);
    std::vector<uint8_t> previous(static_cast<std::size_t>(width) * height * 3U);
    for (auto& byte : previous)
        byte = static_cast<uint8_t>(random());
    auto rgb = previous;
    std::vector<uint8_t> changed(bands, 7);
    oa::app::find_changed_rgb24_bands(rgb.data(), previous.data(), width, height, changed);
    CHECK((changed == std::vector<uint8_t>{0, 0, 0}));
    // The last byte of band 0 and the first of the last band.
    rgb[static_cast<std::size_t>(width) * 32U * 3U - 1U] ^= 1U;
    rgb[static_cast<std::size_t>(width) * 64U * 3U] ^= 0x80U;
    oa::app::find_changed_rgb24_bands(rgb.data(), previous.data(), width, height, changed);
    CHECK((changed == std::vector<uint8_t>{1, 0, 1}));
    std::vector<uint8_t> short_bands(bands - 1, 7);
    oa::app::find_changed_rgb24_bands(rgb.data(), previous.data(), width, height, short_bands);
    CHECK(short_bands == std::vector<uint8_t>(bands - 1, 7));
}

/// The overlays and the frame through a gamma table that maps every level
/// to itself are the overlays and the frame with no table: rows wholly
/// unpainted, rows painted at either end alone and rows painted throughout.
void identity_gamma_is_no_gamma() {
    std::mt19937 random(test_seed + 7);
    std::array<uint8_t, 256> identity{};
    for (std::size_t level = 0; level < identity.size(); ++level)
        identity[level] = static_cast<uint8_t>(level);
    constexpr uint32_t width = 97;
    constexpr uint32_t height = 70; // three bands, the last a part one
    constexpr std::array<uint8_t, 3> key{0, 0, 1};
    const uint32_t bands = job_pool::bands_of_rows(height, oa::app::xrgb_band_rows);
    std::vector<uint8_t> canvas(static_cast<std::size_t>(width) * height * 3U);
    oa::app::fill_rgb24(canvas.data(), static_cast<std::size_t>(width) * height, key);
    const auto paint = [&](uint32_t x, uint32_t y) {
        auto* pixel = &canvas[(static_cast<std::size_t>(y) * width + x) * 3U];
        for (int channel = 0; channel < 3; ++channel)
            pixel[channel] = static_cast<uint8_t>(random());
    };
    paint(0, 3);
    paint(width - 1, 4);
    for (uint32_t x = 0; x < width; ++x)
        paint(x, 40);
    std::vector<uint8_t> base(canvas.size());
    oa::app::fill_rgb24(base.data(), static_cast<std::size_t>(width) * height, key);
    const std::size_t pitch = static_cast<std::size_t>(width) * 4U + row_slack;
    job_pool::Pool pool(3);
    for (job_pool::Pool* threads : {static_cast<job_pool::Pool*>(nullptr), &pool}) {
        std::vector<uint8_t> plain(pitch * height, untouched);
        std::vector<uint8_t> mapped(pitch * height, untouched);
        std::vector<uint8_t> plain_bands(bands, 7);
        std::vector<uint8_t> mapped_bands(bands, 7);
        oa::app::convert_rgb24_keyed_overlay_argb(
            canvas.data(), key, width, height, plain.data(), pitch, nullptr, plain_bands, threads
        );
        oa::app::convert_rgb24_keyed_overlay_argb(
            canvas.data(),
            key,
            width,
            height,
            mapped.data(),
            pitch,
            &identity,
            mapped_bands,
            threads
        );
        CHECK(plain == mapped);
        CHECK(plain_bands == mapped_bands);
        CHECK((plain_bands == std::vector<uint8_t>{1, 1, 0}));
        std::fill(plain.begin(), plain.end(), untouched);
        std::fill(mapped.begin(), mapped.end(), untouched);
        oa::app::convert_rgb24_overlay_argb(
            canvas.data(),
            base.data(),
            width,
            height,
            plain.data(),
            pitch,
            nullptr,
            plain_bands,
            threads
        );
        oa::app::convert_rgb24_overlay_argb(
            canvas.data(),
            base.data(),
            width,
            height,
            mapped.data(),
            pitch,
            &identity,
            mapped_bands,
            threads
        );
        CHECK(plain == mapped);
        CHECK(plain_bands == mapped_bands);
        CHECK((plain_bands == std::vector<uint8_t>{1, 1, 0}));
        CHECK(
            convert(canvas, width, height, nullptr, threads) ==
            convert(canvas, width, height, &identity, threads)
        );
    }
}

} // namespace

int main() {
    conversion_packs_each_pixel();
    conversion_is_the_same_on_every_pool();
    rgb565_is_what_sdl_makes_of_xrgb();
    rectangle_is_that_part_of_the_whole();
    front_end_frame_serves_as_argb();
    rows_of_a_wider_picture_convert_alike();
    overlay_holds_what_was_painted();
    key_colour_is_the_first_outside_the_palette();
    keyed_overlay_holds_what_was_painted();
    fill_writes_every_pixel();
    identity_gamma_is_no_gamma();
    changed_bands_are_found();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::puts(
        "xrgb conversion: every pixel packed alike on every pool, RGB565 as SDL packs it, "
        "rectangles as the whole frame's, the front end as opaque ARGB8888, "
        "overlays of what was painted, over a base and over a key colour, "
        "pictures filled with one colour, a gamma of 1 as no gamma, "
        "and the bands two frames differ in"
    );
    return EXIT_SUCCESS;
}
