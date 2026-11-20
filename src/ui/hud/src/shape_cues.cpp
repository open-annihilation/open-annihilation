// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/shape_cues.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace oa::ui::hud {
namespace {

constexpr int32_t kMarkerWidth = kPlayerMarkerWidth;
constexpr int32_t kMarkerHeight = kPlayerMarkerHeight;
constexpr int32_t kCentreColumn = kPlayerMarkerCentreColumn;
constexpr int32_t kCentreRow = kPlayerMarkerCentreRow;

/// A digit, 3 columns by 5 rows. Bit 2 is the left column.
constexpr uint8_t kDigits[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111},
    {0b010, 0b110, 0b010, 0b010, 0b111},
    {0b111, 0b001, 0b111, 0b100, 0b111},
    {0b111, 0b001, 0b111, 0b001, 0b111},
    {0b101, 0b101, 0b111, 0b001, 0b001},
    {0b111, 0b100, 0b111, 0b001, 0b111},
    {0b111, 0b100, 0b111, 0b101, 0b111},
    {0b111, 0b001, 0b010, 0b010, 0b010},
    {0b111, 0b101, 0b111, 0b101, 0b111},
    {0b111, 0b101, 0b111, 0b001, 0b111},
};

struct Point {
    int32_t x{};
    int32_t y{};
};

/// The triangle's corners, relative to the shape's centre.
constexpr Point kTriangle[3] = {{0, -5}, {5, 4}, {-5, 4}};
/// The star's corners, relative to the shape's centre.
constexpr Point kStar[10] = {
    {0, -5},
    {1, -2},
    {5, -2},
    {2, 1},
    {3, 4},
    {0, 2},
    {-3, 4},
    {-2, 1},
    {-5, -2},
    {-1, -2},
};

/// Tells whether a point is inside a polygon, by the even-odd rule.
///
/// @param x the point's column, relative to the shape's centre
/// @param y the point's row, relative to the shape's centre
/// @param corners the polygon's corners, in order
/// @param count how many corners
/// @return true when the point is inside
bool inside_polygon(int32_t x, int32_t y, const Point* corners, int32_t count) noexcept {
    bool inside = false;
    for (int32_t index = 0; index < count; ++index) {
        const Point from = corners[index];
        const Point to = corners[index + 1 == count ? 0 : index + 1];
        if ((from.y > y) == (to.y > y))
            continue;
        const int64_t rise = static_cast<int64_t>(to.y) - from.y;
        const int64_t cross =
            (static_cast<int64_t>(x) - from.x) * rise -
            (static_cast<int64_t>(y) - from.y) * (static_cast<int64_t>(to.x) - from.x);
        if ((rise > 0 && cross < 0) || (rise < 0 && cross > 0))
            inside = !inside;
    }
    return inside;
}

/// Tells whether a pixel belongs to one of the five shapes.
///
/// @param kind 0 circle, 1 square, 2 triangle, 3 diamond, 4 star
/// @param x column inside the marker
/// @param y row inside the marker
/// @return true when the pixel is part of the filled shape
bool shape_covers(int kind, int32_t x, int32_t y) noexcept {
    const int32_t dx = x - kCentreColumn;
    const int32_t dy = y - kCentreRow;
    const int32_t adx = dx < 0 ? -dx : dx;
    const int32_t ady = dy < 0 ? -dy : dy;
    switch (kind) {
    case 0:
        return dx * dx + dy * dy <= 16;
    case 1:
        return adx <= 4 && ady <= 4;
    case 2:
        return inside_polygon(dx, dy, kTriangle, 3);
    case 3:
        return adx * 5 + ady * 4 <= 20;
    default:
        return inside_polygon(dx, dy, kStar, 10);
    }
}

/// Tells whether a covered pixel touches the outside of the shape.
///
/// @param covered the filled shape, row by row
/// @param x column inside the marker
/// @param y row inside the marker
/// @return true when a side neighbour is outside the shape or the marker
bool on_edge(const bool covered[kMarkerHeight][kMarkerWidth], int32_t x, int32_t y) noexcept {
    if (!covered[y][x])
        return false;
    constexpr int32_t side[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& step : side) {
        const int32_t nx = x + step[0];
        const int32_t ny = y + step[1];
        if (nx < 0 || ny < 0 || nx >= kMarkerWidth || ny >= kMarkerHeight || !covered[ny][nx])
            return true;
    }
    return false;
}

/// Writes one digit into the number's mask.
///
/// @param[in,out] number the number's pixels
/// @param digit the digit, 0 to 9
/// @param left the digit's left column
void place_digit(bool number[kMarkerHeight][kMarkerWidth], int digit, int32_t left) noexcept {
    for (int32_t row = 0; row < 5; ++row) {
        const uint8_t bits = kDigits[digit][row];
        for (int32_t column = 0; column < 3; ++column) {
            const uint8_t bit = static_cast<uint8_t>(1U << static_cast<unsigned>(2 - column));
            if ((bits & bit) != 0)
                number[4 + row][left + column] = true;
        }
    }
}

/// Classifies every pixel of one player's marker.
///
/// @param player the player's seat, 0 to 9; anything else is all clear
/// @return the marker, row by row
std::array<MarkerPixel, kMarkerWidth * kMarkerHeight> marker_pixels(int player) noexcept {
    std::array<MarkerPixel, kMarkerWidth * kMarkerHeight> pixels{};
    if (player < 0 || player > 9)
        return pixels;
    bool covered[kMarkerHeight][kMarkerWidth]{};
    const int kind = player % 5;
    for (int32_t y = 0; y < kMarkerHeight; ++y)
        for (int32_t x = 0; x < kMarkerWidth; ++x)
            covered[y][x] = shape_covers(kind, x, y);
    const bool hollow = player >= 5;
    constexpr int32_t side[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int32_t y = 0; y < kMarkerHeight; ++y) {
        for (int32_t x = 0; x < kMarkerWidth; ++x) {
            MarkerPixel kind_of = MarkerPixel::clear;
            if (covered[y][x]) {
                const bool edge = on_edge(covered, x, y);
                if (hollow)
                    kind_of = edge ? MarkerPixel::shape : MarkerPixel::clear;
                else
                    kind_of = edge ? MarkerPixel::outline : MarkerPixel::shape;
            } else if (hollow) {
                for (const auto& step : side) {
                    const int32_t nx = x + step[0];
                    const int32_t ny = y + step[1];
                    if (nx >= 0 && ny >= 0 && nx < kMarkerWidth && ny < kMarkerHeight &&
                        covered[ny][nx])
                        kind_of = MarkerPixel::outline;
                }
            }
            pixels[static_cast<std::size_t>(y * kMarkerWidth + x)] = kind_of;
        }
    }
    bool number[kMarkerHeight][kMarkerWidth]{};
    if (player == 9) {
        place_digit(number, 1, 12);
        place_digit(number, 0, 16);
    } else {
        place_digit(number, player + 1, 12);
    }
    for (int32_t y = 0; y < kMarkerHeight; ++y) {
        for (int32_t x = 0; x < kMarkerWidth; ++x) {
            if (!number[y][x])
                continue;
            for (const auto& step : side) {
                const int32_t nx = x + step[0];
                const int32_t ny = y + step[1];
                if (nx < 0 || ny < 0 || nx >= kMarkerWidth || ny >= kMarkerHeight || number[ny][nx])
                    continue;
                const std::size_t at = static_cast<std::size_t>(ny * kMarkerWidth + nx);
                if (pixels[at] == MarkerPixel::clear)
                    pixels[at] = MarkerPixel::halo;
            }
        }
    }
    for (int32_t y = 0; y < kMarkerHeight; ++y)
        for (int32_t x = 0; x < kMarkerWidth; ++x)
            if (number[y][x])
                pixels[static_cast<std::size_t>(y * kMarkerWidth + x)] = MarkerPixel::numeral;
    return pixels;
}

void write_block(
    uint8_t* rgb,
    int32_t width,
    int32_t height,
    int32_t column,
    int32_t row,
    int32_t scale,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) noexcept {
    for (int32_t dy = 0; dy < scale; ++dy) {
        for (int32_t dx = 0; dx < scale; ++dx) {
            const int32_t x = column + dx;
            const int32_t y = row + dy;
            if (x < 0 || y < 0 || x >= width || y >= height)
                continue;
            uint8_t* out = rgb + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x)) *
                                     3U;
            out[0] = red;
            out[1] = green;
            out[2] = blue;
        }
    }
}

/// The compact triangle's corners, at twice the pixel scale so the half-pixel
/// corners (0, -3.5), (3.5, 3.5) and (-3.5, 3.5) stay integers. The apex is
/// the top row and the base is the bottom row.
constexpr Point kCompactTriangle[3] = {{0, -7}, {7, 7}, {-7, 7}};
/// The compact star, one bit a column and bit 6 the left column. Five points
/// reach the box's edges, and the rows mirror about the centre column.
constexpr uint8_t kCompactStarRows[kCompactMarkerSide] = {
    0b0001000,
    0b0011100,
    0b1111111,
    0b0111110,
    0b0011100,
    0b0110110,
    0b0100010,
};

/// Tells whether a pixel belongs to one of the five compact shapes.
///
/// @param kind 0 circle, 1 square, 2 triangle, 3 diamond, 4 star
/// @param x column inside the compact marker
/// @param y row inside the compact marker
/// @return true when the pixel is part of the filled shape
bool compact_covers(int kind, int32_t x, int32_t y) noexcept {
    const int32_t dx = x - kCompactMarkerCentre;
    const int32_t dy = y - kCompactMarkerCentre;
    const int32_t adx = dx < 0 ? -dx : dx;
    const int32_t ady = dy < 0 ? -dy : dy;
    switch (kind) {
    case 0:
        // Squared radius 10 keeps the filled circle's interior round inside the 7x7.
        return dx * dx + dy * dy <= 10;
    case 1:
        return adx <= 2 && ady <= 2;
    case 2:
        return inside_polygon(dx * 2, dy * 2, kCompactTriangle, 3);
    case 3:
        return adx + ady <= 2;
    default:
        return y >= 0 && y < kCompactMarkerSide && x >= 0 && x < kCompactMarkerSide &&
               (kCompactStarRows[y] & (1U << (kCompactMarkerSide - 1 - x))) != 0;
    }
}

/// Classifies every pixel of one player's compact marker.
///
/// @param player the player's seat, 0 to 9; anything else is all clear
/// @return the marker, row by row
std::array<MarkerPixel, kCompactMarkerSide * kCompactMarkerSide>
compact_pixels(int player) noexcept {
    constexpr int32_t side = kCompactMarkerSide;
    std::array<MarkerPixel, side * side> pixels{};
    if (player < 0 || player > 9)
        return pixels;
    bool covered[side][side]{};
    const int kind = player % 5;
    for (int32_t y = 0; y < side; ++y)
        for (int32_t x = 0; x < side; ++x)
            covered[y][x] = compact_covers(kind, x, y);
    const bool hollow = player >= 5;
    constexpr int32_t step[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int32_t y = 0; y < side; ++y) {
        for (int32_t x = 0; x < side; ++x) {
            MarkerPixel kind_of = MarkerPixel::clear;
            const bool edge = [&] {
                if (!covered[y][x])
                    return false;
                for (const auto& neighbor : step) {
                    const int32_t nx = x + neighbor[0];
                    const int32_t ny = y + neighbor[1];
                    if (nx < 0 || ny < 0 || nx >= side || ny >= side || !covered[ny][nx])
                        return true;
                }
                return false;
            }();
            if (covered[y][x]) {
                if (hollow)
                    kind_of = edge ? MarkerPixel::shape : MarkerPixel::clear;
                else
                    kind_of = edge ? MarkerPixel::outline : MarkerPixel::shape;
            } else if (hollow) {
                for (const auto& neighbor : step) {
                    const int32_t nx = x + neighbor[0];
                    const int32_t ny = y + neighbor[1];
                    if (nx >= 0 && ny >= 0 && nx < side && ny < side && covered[ny][nx])
                        kind_of = MarkerPixel::outline;
                }
            }
            pixels[static_cast<std::size_t>(y * side + x)] = kind_of;
        }
    }
    return pixels;
}

} // namespace

MarkerPixel player_marker_pixel(int player, int32_t x, int32_t y) noexcept {
    if (x < 0 || y < 0 || x >= kMarkerWidth || y >= kMarkerHeight)
        return MarkerPixel::clear;
    const auto pixels = marker_pixels(player);
    return pixels[static_cast<std::size_t>(y * kMarkerWidth + x)];
}

void stamp_player_marker(
    uint8_t* rgb,
    int32_t width,
    int32_t height,
    int32_t centre_x,
    int32_t centre_y,
    int player,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    int32_t scale,
    bool plate
) noexcept {
    if (rgb == nullptr || width <= 0 || height <= 0 || scale < 1 || player < 0 || player > 9)
        return;
    const auto pixels = marker_pixels(player);
    const auto ink = numeral_ink(red, green, blue);
    const int32_t origin_x = centre_x - scale / 2 - kCentreColumn * scale;
    const int32_t origin_y = centre_y - scale / 2 - kCentreRow * scale;
    if (plate) {
        const int32_t left = origin_x - scale;
        const int32_t top = origin_y - scale;
        const int32_t plate_width = (kMarkerWidth + 2) * scale;
        const int32_t plate_height = (kMarkerHeight + 2) * scale;
        for (int32_t y = 0; y < plate_height; ++y) {
            for (int32_t x = 0; x < plate_width; ++x) {
                const int32_t column = left + x;
                const int32_t row = top + y;
                if (column < 0 || row < 0 || column >= width || row >= height)
                    continue;
                uint8_t* out =
                    rgb + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(column)) *
                              3U;
                out[0] = 0;
                out[1] = 0;
                out[2] = 0;
            }
        }
    }
    for (int32_t y = 0; y < kMarkerHeight; ++y) {
        for (int32_t x = 0; x < kMarkerWidth; ++x) {
            const MarkerPixel kind = pixels[static_cast<std::size_t>(y * kMarkerWidth + x)];
            if (kind == MarkerPixel::clear)
                continue;
            uint8_t out_red = 0;
            uint8_t out_green = 0;
            uint8_t out_blue = 0;
            if (kind == MarkerPixel::shape) {
                out_red = red;
                out_green = green;
                out_blue = blue;
            } else if (kind == MarkerPixel::numeral) {
                out_red = 255;
                out_green = 255;
                out_blue = 255;
            } else if (kind == MarkerPixel::outline) {
                out_red = ink[0];
                out_green = ink[1];
                out_blue = ink[2];
            }
            write_block(
                rgb,
                width,
                height,
                origin_x + x * scale,
                origin_y + y * scale,
                scale,
                out_red,
                out_green,
                out_blue
            );
        }
    }
}

MarkerPixel compact_marker_pixel(int player, int32_t x, int32_t y) noexcept {
    if (x < 0 || y < 0 || x >= kCompactMarkerSide || y >= kCompactMarkerSide)
        return MarkerPixel::clear;
    const auto pixels = compact_pixels(player);
    return pixels[static_cast<std::size_t>(y * kCompactMarkerSide + x)];
}

CompactMarkerPlace
compact_marker_place(int32_t width, int32_t height, int32_t centre_x, int32_t centre_y) noexcept {
    CompactMarkerPlace place{centre_x - kCompactMarkerCentre, centre_y - kCompactMarkerCentre};
    if (width >= kCompactMarkerSide) {
        if (place.left < 0)
            place.left = 0;
        if (place.left > width - kCompactMarkerSide)
            place.left = width - kCompactMarkerSide;
    }
    if (height >= kCompactMarkerSide) {
        if (place.top < 0)
            place.top = 0;
        if (place.top > height - kCompactMarkerSide)
            place.top = height - kCompactMarkerSide;
    }
    return place;
}

void stamp_compact_marker(
    uint8_t* rgb,
    int32_t width,
    int32_t height,
    int32_t centre_x,
    int32_t centre_y,
    int player,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) noexcept {
    if (rgb == nullptr || width <= 0 || height <= 0 || player < 0 || player > 9)
        return;
    const auto pixels = compact_pixels(player);
    const auto ink = numeral_ink(red, green, blue);
    const CompactMarkerPlace place = compact_marker_place(width, height, centre_x, centre_y);
    const int32_t left = place.left;
    const int32_t top = place.top;
    for (int32_t y = 0; y < kCompactMarkerSide; ++y) {
        for (int32_t x = 0; x < kCompactMarkerSide; ++x) {
            const MarkerPixel kind = pixels[static_cast<std::size_t>(y * kCompactMarkerSide + x)];
            if (kind == MarkerPixel::clear)
                continue;
            const int32_t column = left + x;
            const int32_t row = top + y;
            if (column < 0 || row < 0 || column >= width || row >= height)
                continue;
            uint8_t* out = rgb + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(column)) *
                                     3U;
            if (kind == MarkerPixel::shape) {
                out[0] = red;
                out[1] = green;
                out[2] = blue;
            } else {
                out[0] = ink[0];
                out[1] = ink[1];
                out[2] = ink[2];
            }
        }
    }
}

std::array<uint8_t, 3> numeral_ink(uint8_t red, uint8_t green, uint8_t blue) noexcept {
    const int weight =
        299 * static_cast<int>(red) + 587 * static_cast<int>(green) + 114 * static_cast<int>(blue);
    const uint8_t channel = weight > 140000 ? uint8_t{0} : uint8_t{255};
    return {channel, channel, channel};
}

BarNotches health_bar_notches(const HealthBar& bar) noexcept {
    BarNotches notches;
    const int32_t inside_left = bar.trough.x1 + 1;
    const int32_t inside_right = bar.trough.x2 - 1;
    const int32_t inside_width = inside_right - inside_left + 1;
    const int32_t inside_top = bar.trough.y1 + 1;
    const int32_t inside_bottom = bar.trough.y2 - 1;
    const int32_t inside_height = inside_bottom - inside_top + 1;
    if (inside_width < 3 || inside_height <= 0)
        return notches;
    const auto place = [&](int32_t third) {
        BarNotch notch;
        notch.x = inside_left + (inside_width * third) / 3;
        notch.y = inside_top;
        notch.width = 1;
        notch.height = inside_height;
        return notch;
    };
    notches.one_third = place(1);
    notches.two_thirds = place(2);
    return notches;
}

} // namespace oa::ui::hud
