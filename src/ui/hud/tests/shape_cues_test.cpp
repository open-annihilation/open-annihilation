// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "check.hpp"

#include "oa/ui/hud/health_bar.hpp"
#include "oa/ui/hud/shape_cues.hpp"

#include <array>
#include <cstdint>
#include <vector>

using namespace oa::ui::hud;

namespace {

struct Dot {
    int32_t x{};
    int32_t y{};
    bool operator==(const Dot&) const = default;
};

std::vector<Dot> pixels_of(int player, MarkerPixel kind) {
    std::vector<Dot> found;
    for (int32_t y = 0; y < kPlayerMarkerHeight; ++y)
        for (int32_t x = 0; x < kPlayerMarkerWidth; ++x)
            if (player_marker_pixel(player, x, y) == kind)
                found.push_back({x, y});
    return found;
}

void each_marker_has_a_shape_an_outline_and_a_number() {
    for (int player = 0; player < 10; ++player) {
        CHECK(!pixels_of(player, MarkerPixel::shape).empty());
        CHECK(!pixels_of(player, MarkerPixel::outline).empty());
        CHECK(!pixels_of(player, MarkerPixel::numeral).empty());
    }
    CHECK(player_marker_pixel(-1, 6, 6) == MarkerPixel::clear);
    CHECK(player_marker_pixel(10, 6, 6) == MarkerPixel::clear);
    CHECK(player_marker_pixel(0, -1, 0) == MarkerPixel::clear);
}

void filled_and_hollow_shapes_differ() {
    for (int player = 0; player < 5; ++player) {
        CHECK(
            player_marker_pixel(player, kPlayerMarkerCentreColumn, kPlayerMarkerCentreRow) ==
            MarkerPixel::shape
        );
        CHECK(
            player_marker_pixel(player + 5, kPlayerMarkerCentreColumn, kPlayerMarkerCentreRow) ==
            MarkerPixel::clear
        );
        const auto filled = pixels_of(player, MarkerPixel::shape);
        const auto hollow = pixels_of(player + 5, MarkerPixel::shape);
        bool shared = false;
        for (const Dot& pixel : hollow)
            for (const Dot& other : filled)
                shared = shared || (pixel.x == other.x && pixel.y == other.y);
        CHECK(!shared);
    }
}

void the_five_shapes_differ() {
    std::array<std::vector<Dot>, 5> shapes{};
    for (int player = 0; player < 5; ++player)
        shapes[static_cast<std::size_t>(player)] = pixels_of(player, MarkerPixel::shape);
    for (int left = 0; left < 5; ++left) {
        for (int right = left + 1; right < 5; ++right) {
            CHECK(
                shapes[static_cast<std::size_t>(left)] != shapes[static_cast<std::size_t>(right)]
            );
        }
    }
}

void the_numbers_differ() {
    std::array<std::vector<Dot>, 9> numbers{};
    for (int player = 0; player < 9; ++player)
        numbers[static_cast<std::size_t>(player)] = pixels_of(player, MarkerPixel::numeral);
    for (int left = 0; left < 9; ++left)
        for (int right = left + 1; right < 9; ++right)
            CHECK(
                numbers[static_cast<std::size_t>(left)] != numbers[static_cast<std::size_t>(right)]
            );
    const auto one = pixels_of(0, MarkerPixel::numeral);
    const auto ten = pixels_of(9, MarkerPixel::numeral);
    CHECK(ten.size() > one.size());
    const auto span = [](const std::vector<Dot>& dots) {
        int32_t left = dots.front().x;
        int32_t right = dots.front().x;
        for (const Dot& dot : dots) {
            if (dot.x < left)
                left = dot.x;
            if (dot.x > right)
                right = dot.x;
        }
        return right - left;
    };
    CHECK(span(ten) > span(one));
}

void a_light_ground_takes_a_black_number() {
    const auto on_white = numeral_ink(255, 255, 255);
    CHECK(on_white[0] == 0 && on_white[1] == 0 && on_white[2] == 0);
    const auto on_blue = numeral_ink(23, 71, 231);
    CHECK(on_blue[0] == 255 && on_blue[1] == 255 && on_blue[2] == 255);
}

void stamping_writes_the_shape_and_clips() {
    constexpr int32_t width = 40;
    constexpr int32_t height = 30;
    std::array<uint8_t, width * height * 3> picture{};
    picture.fill(7);
    stamp_player_marker(picture.data(), width, height, 20, 15, 0, 10, 20, 30);
    const auto at = [](int32_t x, int32_t y) {
        return (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 3U;
    };
    const std::size_t centre = at(20, 15);
    CHECK(picture[centre] == 10 && picture[centre + 1] == 20 && picture[centre + 2] == 30);
    CHECK(picture[0] == 7 && picture[1] == 7 && picture[2] == 7);
    // A dark fill is outlined in white. This pixel is the circle's top edge.
    const std::size_t edge = at(20, 11);
    CHECK(picture[edge] == 255 && picture[edge + 1] == 255 && picture[edge + 2] == 255);
    bool outline = false;
    bool numeral = false;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const std::size_t pixel = at(x, y);
            outline = outline ||
                      (picture[pixel] == 0 && picture[pixel + 1] == 0 && picture[pixel + 2] == 0);
            numeral = numeral || (picture[pixel] == 255 && picture[pixel + 1] == 255 &&
                                  picture[pixel + 2] == 255);
        }
    }
    CHECK(outline && numeral);
    std::array<uint8_t, width * height * 3> light{};
    stamp_player_marker(light.data(), width, height, 20, 15, 0, 255, 255, 255);
    CHECK(light[edge] == 0 && light[edge + 1] == 0 && light[edge + 2] == 0);
    std::array<uint8_t, width * height * 3> scaled{};
    scaled.fill(7);
    stamp_player_marker(scaled.data(), width, height, 20, 15, 0, 10, 20, 30, 2, true);
    const std::size_t block = at(19, 14);
    CHECK(scaled[block] == 10 && scaled[block + 1] == 20 && scaled[block + 2] == 30);
    const std::size_t plate = at(5, 15);
    CHECK(scaled[plate] == 0 && scaled[plate + 1] == 0 && scaled[plate + 2] == 0);
    stamp_player_marker(picture.data(), width, height, -100, -100, 0, 1, 2, 3);
    CHECK(picture[0] == 7);
    stamp_player_marker(nullptr, width, height, 0, 0, 0, 1, 2, 3);
}

void compact_markers_are_the_shape_alone() {
    for (int player = 0; player < 10; ++player) {
        bool shape = false;
        bool outline = false;
        for (int32_t y = 0; y < kCompactMarkerSide; ++y) {
            for (int32_t x = 0; x < kCompactMarkerSide; ++x) {
                const MarkerPixel kind = compact_marker_pixel(player, x, y);
                CHECK(kind != MarkerPixel::numeral);
                CHECK(kind != MarkerPixel::halo);
                shape = shape || kind == MarkerPixel::shape;
                outline = outline || kind == MarkerPixel::outline;
            }
        }
        CHECK(shape && outline);
        const MarkerPixel centre =
            compact_marker_pixel(player, kCompactMarkerCentre, kCompactMarkerCentre);
        CHECK(centre == (player < 5 ? MarkerPixel::shape : MarkerPixel::clear));
        int colour_pixels = 0;
        for (int32_t y = 0; y < kCompactMarkerSide; ++y) {
            for (int32_t x = 0; x < kCompactMarkerSide; ++x) {
                CHECK(
                    compact_marker_pixel(player, x, y) ==
                    compact_marker_pixel(player, kCompactMarkerSide - 1 - x, y)
                );
                if (compact_marker_pixel(player, x, y) == MarkerPixel::shape)
                    colour_pixels += 1;
            }
        }
        // A filled marker keeps at least the diamond's five colour pixels.
        if (player < 5)
            CHECK(colour_pixels >= 5);
    }
    std::array<std::vector<MarkerPixel>, 10> markers{};
    for (int player = 0; player < 10; ++player) {
        for (int32_t y = 0; y < kCompactMarkerSide; ++y)
            for (int32_t x = 0; x < kCompactMarkerSide; ++x)
                markers[static_cast<std::size_t>(player)].push_back(
                    compact_marker_pixel(player, x, y)
                );
    }
    for (int left = 0; left < 10; ++left)
        for (int right = left + 1; right < 10; ++right)
            CHECK(
                markers[static_cast<std::size_t>(left)] != markers[static_cast<std::size_t>(right)]
            );
    CHECK(compact_marker_pixel(-1, 3, 3) == MarkerPixel::clear);
    CHECK(compact_marker_pixel(0, -1, 0) == MarkerPixel::clear);
}

void a_compact_marker_stays_inside_the_picture() {
    constexpr int32_t width = 40;
    constexpr int32_t height = 30;
    std::array<uint8_t, width * height * 3> shifted{};
    std::array<uint8_t, width * height * 3> placed{};
    stamp_compact_marker(shifted.data(), width, height, 0, 0, 0, 10, 20, 30);
    stamp_compact_marker(
        placed.data(), width, height, kCompactMarkerCentre, kCompactMarkerCentre, 0, 10, 20, 30
    );
    CHECK(shifted == placed);
    std::array<uint8_t, width * height * 3> corner{};
    std::array<uint8_t, width * height * 3> fitted{};
    stamp_compact_marker(corner.data(), width, height, width - 1, height - 1, 4, 1, 2, 3);
    stamp_compact_marker(
        fitted.data(),
        width,
        height,
        width - 1 - (kCompactMarkerSide - 1 - kCompactMarkerCentre),
        height - 1 - (kCompactMarkerSide - 1 - kCompactMarkerCentre),
        4,
        1,
        2,
        3
    );
    CHECK(corner == fitted);
    stamp_compact_marker(nullptr, width, height, 0, 0, 0, 1, 2, 3);
}

void notches_sit_at_one_third_and_two_thirds() {
    HealthBar bar{};
    bar.trough = {83, 48, 117, 52};
    bar.trough_color = 0x80;
    bar.fill = {84, 49, 116, 51};
    bar.fill_color = 0x8a;
    const HealthBar before = bar;
    const BarNotches marks = health_bar_notches(bar);
    CHECK(bar.trough.x1 == before.trough.x1 && bar.trough.y1 == before.trough.y1);
    CHECK(bar.trough.x2 == before.trough.x2 && bar.trough.y2 == before.trough.y2);
    CHECK(bar.trough_color == before.trough_color && bar.fill_color == before.fill_color);
    CHECK(bar.fill.x1 == before.fill.x1 && bar.fill.x2 == before.fill.x2);
    CHECK(marks.one_third.x == 95 && marks.one_third.y == 49);
    CHECK(marks.one_third.width == 1 && marks.one_third.height == 3);
    CHECK(marks.two_thirds.x == 106 && marks.two_thirds.y == 49);
    CHECK(marks.two_thirds.width == 1 && marks.two_thirds.height == 3);
    HealthBar narrow{};
    narrow.trough = {0, 0, 2, 4};
    const BarNotches empty = health_bar_notches(narrow);
    CHECK(empty.one_third.width == 0 && empty.two_thirds.width == 0);
}

} // namespace

int main() {
    each_marker_has_a_shape_an_outline_and_a_number();
    filled_and_hollow_shapes_differ();
    the_five_shapes_differ();
    the_numbers_differ();
    a_light_ground_takes_a_black_number();
    stamping_writes_the_shape_and_clips();
    compact_markers_are_the_shape_alone();
    a_compact_marker_stays_inside_the_picture();
    notches_sit_at_one_third_and_two_thirds();
    return 0;
}
