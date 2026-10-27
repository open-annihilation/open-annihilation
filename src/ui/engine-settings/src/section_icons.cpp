// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The settings dialog's line icons, drawn from lines, circles, arcs and
// rounded rectangles on a 24-unit square into pictures the dialog's display
// list places.

#include "section_icons.hpp"

#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace oa::ui::engine_settings::icons {

namespace {

namespace kit = oa::ui::kit;
namespace renderer = oa::ui::frontend_renderer;

/// The units across an icon's square.
constexpr double kGrid = 24.0;
/// Half a line's width, in units: the design's lines are two units wide.
constexpr double kHalfLine = 1.0;
/// The bytes of one picture pixel: red, green, blue and opacity.
constexpr std::size_t kPixelBytes = 4;

/// What a shape is.
enum class Form : uint8_t {
    line,   ///< a line from (a, b) to (c, d); a point where both ends meet
    circle, ///< a circle round (a, b) of radius c
    arc,    ///< the part of a circle round (a, b) of radius c from angle d to angle e
    box,    ///< a rectangle from (a, b), c wide and d high, its corners rounded by e
};

/// One shape of an icon, in units, its angles in radians, y down.
struct Shape {
    Form form{};
    double a{};
    double b{};
    double c{};
    double d{};
    double e{};
};

/// Returns a line.
constexpr Shape line(double x0, double y0, double x1, double y1) noexcept {
    return {Form::line, x0, y0, x1, y1, 0.0};
}

/// Returns a circle.
constexpr Shape circle(double x, double y, double radius) noexcept {
    return {Form::circle, x, y, radius, 0.0, 0.0};
}

/// Returns an arc, from one angle to a larger one, y down.
constexpr Shape arc(double x, double y, double radius, double from, double to) noexcept {
    return {Form::arc, x, y, radius, from, to};
}

/// Returns a rounded rectangle.
constexpr Shape box(double x, double y, double width, double height, double radius) noexcept {
    return {Form::box, x, y, width, height, radius};
}

constexpr double kPi = std::numbers::pi;

/// Controls and a mod's keys: a keyboard with a row of keys and a space bar.
constexpr std::array kKeyboard{
    box(2, 6, 20, 12, 2),
    line(6, 10, 6, 10),
    line(10, 10, 10, 10),
    line(14, 10, 14, 10),
    line(18, 10, 18, 10),
    line(7, 14, 17, 14),
};
/// Accessibility: a person with open arms.
constexpr std::array kPerson{
    circle(12, 4.5, 1.5),
    line(5, 8, 12, 9.5),
    line(12, 9.5, 19, 8),
    line(12, 9.5, 12, 14),
    line(9, 21, 12, 14),
    line(12, 14, 15, 21),
};
/// Touch: a hand, its finger raised.
constexpr std::array kHand{
    line(9, 11, 9, 5),
    arc(10.5, 5, 1.5, kPi, 2 * kPi),
    line(12, 5, 12, 11),
    line(12, 10, 12, 4),
    arc(13.5, 4, 1.5, kPi, 2 * kPi),
    line(15, 4, 15, 11),
    arc(16.5, 10.5, 1.5, kPi, 2 * kPi),
    line(18, 10.5, 18, 15),
    arc(12, 15, 6, 0, kPi / 2),
    line(12, 21, 11, 21),
    line(11, 21, 8, 20.4),
    line(8, 20.4, 6, 18.3),
    line(6, 18.3, 3.5, 14),
    line(3.5, 14, 4.4, 12.6),
    line(4.4, 12.6, 6, 12.4),
    line(6, 12.4, 9, 15),
};
/// Controller: a gamepad with its cross and two buttons.
constexpr std::array kGamepad{
    box(2, 7, 20, 10, 5),
    line(7, 12, 11, 12),
    line(9, 10, 9, 14),
    line(15.5, 11, 15.5, 11),
    line(17.5, 13, 17.5, 13),
};
/// Graphics: a monitor on its stand.
constexpr std::array kMonitor{
    box(2, 3, 20, 14, 2),
    line(8, 21, 16, 21),
    line(12, 17, 12, 21),
};
/// User Interface and a registered page: a window with a bar and a side pane.
constexpr std::array kWindow{
    box(3, 4, 18, 16, 2),
    line(3, 9, 21, 9),
    line(9, 9, 9, 20),
};
/// The half-width of the globe's meridians, from the arcs' centres: the
/// meridians are arcs of radius 14 through the poles, 9 from the middle.
constexpr double kMeridianSpread = 10.7238;
/// The angle at which a meridian meets a pole, from its arc's centre.
constexpr double kMeridianAngle = 0.6982;
/// Language & Text: a globe.
constexpr std::array kGlobe{
    circle(12, 12, 9),
    line(3, 12, 21, 12),
    arc(12 - kMeridianSpread, 12, 14, -kMeridianAngle, kMeridianAngle),
    arc(12 + kMeridianSpread, 12, 14, kPi - kMeridianAngle, kPi + kMeridianAngle),
};
/// Game files: a folder.
constexpr std::array kFolder{
    line(3, 7, 3, 17),
    arc(5, 7, 2, kPi, 1.5 * kPi),
    line(5, 5, 9, 5),
    line(9, 5, 11, 7),
    line(11, 7, 19, 7),
    arc(19, 9, 2, 1.5 * kPi, 2 * kPi),
    line(21, 9, 21, 17),
    arc(19, 17, 2, 0, 0.5 * kPi),
    line(19, 19, 5, 19),
    arc(5, 17, 2, 0.5 * kPi, kPi),
};
/// Downloads: an arrow down onto a line.
constexpr std::array kDownload{
    line(12, 3, 12, 15),
    line(7, 10, 12, 15),
    line(12, 15, 17, 10),
    line(5, 21, 19, 21),
};
/// The angle the turning arrow's circle ends at, short of its start.
constexpr double kTurnEnd = 5.4416;
/// Updates and a mod's patrolling builders: a turning arrow.
constexpr std::array kTurn{
    arc(12, 12, 9, 0, kTurnEnd),
    line(18, 5.3, 21, 8),
    line(21, 3, 21, 8),
    line(21, 8, 16, 8),
};
/// Game Content: a disc.
constexpr std::array kDisc{
    circle(12, 12, 9),
    circle(12, 12, 2.5),
};
/// System: a box of files with its lid.
constexpr std::array kArchive{
    box(3, 4, 18, 5, 1.5),
    box(5, 9, 14, 11, 1.5),
    line(10, 13, 14, 13),
};
/// Engine Limits: three sliders.
constexpr std::array kSliders{
    line(4, 6, 14, 6),
    line(18, 6, 20, 6),
    line(4, 12, 8, 12),
    line(12, 12, 20, 12),
    line(4, 18, 16, 18),
    circle(16, 6, 2),
    circle(10, 12, 2),
    circle(18, 18, 2),
};
/// Developer: two angle brackets.
constexpr std::array kBrackets{
    line(8, 8, 4, 12),
    line(4, 12, 8, 16),
    line(16, 8, 20, 12),
    line(20, 12, 16, 16),
};
/// A mod's guarding builders: a shield.
constexpr std::array kShield{
    line(12, 3, 20, 6),
    line(20, 6, 20, 12),
    line(20, 12, 12, 21),
    line(12, 21, 4, 12),
    line(4, 12, 4, 6),
    line(4, 6, 12, 3),
};
/// A mod's build tools: a grid of build cells.
constexpr std::array kCells{
    box(4, 4, 16, 16, 2),
    line(4, 12, 20, 12),
    line(12, 4, 12, 20),
};
/// A mod's snap and chat: a speech bubble.
constexpr std::array kBubble{
    box(3, 5, 18, 12, 3),
    line(8, 17, 7, 21),
    line(7, 21, 12, 17),
};

/// Returns a section's shapes.
///
/// @param page the section
/// @return its shapes
std::span<const Shape> shapes_of(Page page) noexcept {
    switch (page) {
    case Page::controls:
    case Page::mod_keys:
        return kKeyboard;
    case Page::accessibility:
        return kPerson;
    case Page::touch:
        return kHand;
    case Page::controller:
        return kGamepad;
    case Page::graphics:
        return kMonitor;
    case Page::user_interface:
    case Page::registered:
        return kWindow;
    case Page::language:
        return kGlobe;
    case Page::game_files:
        return kFolder;
    case Page::downloads:
        return kDownload;
    case Page::updates:
    case Page::mod_patrol:
        return kTurn;
    case Page::game_content:
        return kDisc;
    case Page::system:
        return kArchive;
    case Page::common_tweaks:
        return kSliders;
    case Page::developer:
        return kBrackets;
    case Page::mod_guard:
        return kShield;
    case Page::mod_tools:
        return kCells;
    case Page::mod_chat:
        return kBubble;
    }
    return kWindow;
}

/// Returns a point's distance from a line.
double line_distance(double x, double y, const Shape& shape) noexcept {
    const double along_x = shape.c - shape.a;
    const double along_y = shape.d - shape.b;
    const double length = along_x * along_x + along_y * along_y;
    double t = 0.0;
    if (length > 0.0)
        t = std::clamp(((x - shape.a) * along_x + (y - shape.b) * along_y) / length, 0.0, 1.0);
    return std::hypot(x - (shape.a + t * along_x), y - (shape.b + t * along_y));
}

/// Returns a point's distance from an arc: from the circle where the point
/// lies within the arc's angles, else from the nearer end.
double arc_distance(double x, double y, const Shape& shape) noexcept {
    double angle = std::atan2(y - shape.b, x - shape.a);
    while (angle < shape.d)
        angle += 2 * kPi;
    if (angle <= shape.e)
        return std::abs(std::hypot(x - shape.a, y - shape.b) - shape.c);
    const double start = std::hypot(
        x - (shape.a + shape.c * std::cos(shape.d)), y - (shape.b + shape.c * std::sin(shape.d))
    );
    const double end = std::hypot(
        x - (shape.a + shape.c * std::cos(shape.e)), y - (shape.b + shape.c * std::sin(shape.e))
    );
    return std::min(start, end);
}

/// Returns a point's distance from a rounded rectangle's outline.
double box_distance(double x, double y, const Shape& shape) noexcept {
    const double half_x = shape.c / 2.0;
    const double half_y = shape.d / 2.0;
    const double qx = std::abs(x - (shape.a + half_x)) - (half_x - shape.e);
    const double qy = std::abs(y - (shape.b + half_y)) - (half_y - shape.e);
    const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
    const double inside = std::min(std::max(qx, qy), 0.0);
    return std::abs(outside + inside - shape.e);
}

/// Returns a point's distance from a shape.
double distance(double x, double y, const Shape& shape) noexcept {
    switch (shape.form) {
    case Form::line:
        return line_distance(x, y, shape);
    case Form::circle:
        return std::abs(std::hypot(x - shape.a, y - shape.b) - shape.c);
    case Form::arc:
        return arc_distance(x, y, shape);
    case Form::box:
        return box_distance(x, y, shape);
    }
    return kGrid;
}

/// Returns how much of a pixel the shapes' lines cover, 0 to 1: each pixel
/// sampled at its middle, the edge a pixel wide.
///
/// @param shapes the shapes
/// @param column the pixel's column
/// @param row the pixel's row
/// @return the cover
double cover(std::span<const Shape> shapes, uint32_t column, uint32_t row) noexcept {
    const double unit = kGrid / static_cast<double>(picture_side);
    const double x = (static_cast<double>(column) + 0.5) * unit;
    const double y = (static_cast<double>(row) + 0.5) * unit;
    double nearest = kGrid;
    for (const Shape& shape : shapes)
        nearest = std::min(nearest, distance(x, y, shape));
    return std::clamp(0.5 + (kHalfLine - nearest) / unit, 0.0, 1.0);
}

/// Returns a picture's pixels: the shapes' lines in one colour, every other
/// pixel clear, each pixel keeping the colour so that a filter blends no
/// other colour into the edges.
///
/// @param shapes the shapes
/// @param ink the lines' colour
/// @return the pixels, picture_side square
std::vector<uint8_t> drawn(std::span<const Shape> shapes, kit::Colour ink) {
    std::vector<uint8_t> pixels(
        static_cast<std::size_t>(picture_side) * picture_side * kPixelBytes
    );
    for (uint32_t row = 0; row < picture_side; ++row) {
        for (uint32_t column = 0; column < picture_side; ++column) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * picture_side + column) * kPixelBytes;
            pixels[at] = ink.r;
            pixels[at + 1] = ink.g;
            pixels[at + 2] = ink.b;
            pixels[at + 3] = static_cast<uint8_t>(std::lround(cover(shapes, column, row) * 255.0));
        }
    }
    return pixels;
}

/// The sections, in Page's order, each icon's picture is kept for.
constexpr std::size_t kPages = page_count;

/// Every section's icon in both tones, drawn once.
struct Drawn {
    std::array<std::vector<uint8_t>, kPages * 2> pixels{};
    std::array<renderer::RgbaPicture, kPages * 2> pictures{};
};

/// Returns every icon, drawing them the first time.
const Drawn& every_icon() {
    static const Drawn icons = [] {
        Drawn made;
        for (std::size_t index = 0; index < kPages; ++index) {
            const auto page = static_cast<Page>(index);
            for (const Tone tone : {Tone::quiet, Tone::lit}) {
                const std::size_t slot = index * 2 + static_cast<std::size_t>(tone);
                made.pixels[slot] = drawn(
                    shapes_of(page), tone == Tone::lit ? kit::colour::text : kit::colour::list_text
                );
                made.pictures[slot] = {picture_side, picture_side, made.pixels[slot]};
            }
        }
        return made;
    }();
    return icons;
}

} // namespace

const renderer::RgbaPicture& section_icon(Page page, Tone tone) {
    const std::size_t index = std::min(static_cast<std::size_t>(page), kPages - 1);
    return every_icon().pictures[index * 2 + static_cast<std::size_t>(tone)];
}

const renderer::RgbaPicture& key_layout_tile() {
    struct Tile {
        std::vector<uint8_t> pixels;
        renderer::RgbaPicture picture{};
    };

    static const Tile tile = [] {
        Tile made;
        // The keyboard, two thirds of the tile across, over the hover surface.
        constexpr double inset = kGrid / 6.0;
        constexpr double scale = 2.0 / 3.0;
        std::vector<Shape> keys;
        for (Shape shape : kKeyboard) {
            shape.a = inset + shape.a * scale;
            shape.b = inset + shape.b * scale;
            if (shape.form == Form::line) {
                shape.c = inset + shape.c * scale;
                shape.d = inset + shape.d * scale;
            } else {
                shape.c *= scale;
                shape.d *= scale;
                shape.e *= scale;
            }
            keys.push_back(shape);
        }
        made.pixels = drawn(keys, kit::colour::accent);
        const Shape ground = box(0, 0, kGrid, kGrid, 5);
        const double unit = kGrid / static_cast<double>(picture_side);
        for (uint32_t row = 0; row < picture_side; ++row) {
            for (uint32_t column = 0; column < picture_side; ++column) {
                const double x = (static_cast<double>(column) + 0.5) * unit;
                const double y = (static_cast<double>(row) + 0.5) * unit;
                // Inside the tile's rounded square, the ground; its edge a
                // pixel wide.
                const double half = kGrid / 2.0;
                const double qx = std::abs(x - half) - (half - ground.e);
                const double qy = std::abs(y - half) - (half - ground.e);
                const double signed_distance = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) +
                                               std::min(std::max(qx, qy), 0.0) - ground.e;
                const double inside = std::clamp(0.5 - signed_distance / unit, 0.0, 1.0);
                const std::size_t at =
                    (static_cast<std::size_t>(row) * picture_side + column) * kPixelBytes;
                const double line = made.pixels[at + 3] / 255.0;
                const auto mix = [line](uint8_t over, uint8_t under) {
                    return static_cast<uint8_t>(std::lround(over * line + under * (1.0 - line)));
                };
                made.pixels[at] = mix(kit::colour::accent.r, kit::colour::hover.r);
                made.pixels[at + 1] = mix(kit::colour::accent.g, kit::colour::hover.g);
                made.pixels[at + 2] = mix(kit::colour::accent.b, kit::colour::hover.b);
                made.pixels[at + 3] = static_cast<uint8_t>(std::lround(inside * 255.0));
            }
        }
        made.picture = {picture_side, picture_side, made.pixels};
        return made;
    }();
    return tile.picture;
}

} // namespace oa::ui::engine_settings::icons
