// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Keycaps, chord chips, keyboard and mouse pictures, and the hold ring.
// Shapes are coverage at the canvas's own pixels, not pictures or one-bit
// marks. Labels are the caller's, except the add form's caption, which the
// look draws itself.

#include "oa/ui/kit/keys.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::kit {
namespace {

/// The add form's caption.
constexpr std::string_view kAddCaption = "+ ADD KEY";

/// Samples across, and down, each pixel of a mark or a ring.
constexpr int32_t kSamples = 4;

/// The most pixels across or down a mark or a ring is drawn at.
constexpr int32_t kMostPixels = 512;

/// A full ring, in 65536ths of a turn.
constexpr int32_t kRingFull = 65536;

/// Sine of 0 to 90 degrees, in 256ths.
constexpr std::array<int16_t, 91> kSin256{
    0,   4,   9,   13,  18,  22,  27,  31,  36,  40,  44,  49,  53,  58,  62,  66,  71,  75,  79,
    83,  88,  92,  96,  100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 139, 143, 147, 150, 154,
    158, 161, 165, 168, 171, 175, 178, 181, 184, 187, 190, 193, 196, 199, 202, 204, 207, 210, 212,
    215, 217, 219, 222, 224, 226, 228, 230, 232, 234, 236, 237, 239, 241, 242, 243, 245, 246, 247,
    248, 249, 250, 251, 252, 253, 254, 254, 255, 255, 255, 256, 256, 256, 256,
};

/// Darkens a colour to about three fifths.
///
/// @param ink the colour
/// @return the darker colour
Colour darken(Colour ink) noexcept {
    return Colour{
        static_cast<uint8_t>(static_cast<int32_t>(ink.r) * 3 / 5),
        static_cast<uint8_t>(static_cast<int32_t>(ink.g) * 3 / 5),
        static_cast<uint8_t>(static_cast<int32_t>(ink.b) * 3 / 5),
        ink.a,
    };
}

/// Appends a button control that takes no steps.
///
/// @param[in,out] list the display list
/// @param rect the control, in points
/// @param id its number
/// @param name its automation name
/// @param text its caption
/// @param enabled false while a press does not reach it
void add_button(
    DisplayList& list,
    const Rect& rect,
    ControlId id,
    std::string name,
    std::string text,
    bool enabled
) {
    Control control;
    control.id = id;
    control.rect = rect;
    control.enabled = enabled;
    control.name = std::move(name);
    control.kind = ControlKind::button;
    control.steps = false;
    control.focusable = true;
    control.text = std::move(text);
    list.controls.push_back(std::move(control));
}

/// Joins a picture name and a word. The word alone when the name is empty.
///
/// @param name the picture's name
/// @param word the word
/// @return the automation name
std::string hang_name(const std::string& name, std::string_view word) {
    if (name.empty())
        return std::string(word);
    return name + "." + std::string(word);
}

/// Dashes the inside edge of a rectangle, as a blank badge does.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param ink the dashes' colour
void dash_border(const Canvas& canvas, const Rect& rect, Colour ink) {
    const int32_t hair = compact_metrics.hairline;
    const int32_t period = compact_metrics.badge_dash + compact_metrics.badge_dash_gap;
    if (rect.width <= 0 || rect.height <= 0 || hair <= 0 || period <= 0)
        return;
    for (int32_t along = 0; along < rect.width; along += period) {
        const int32_t dash = std::min(compact_metrics.badge_dash, rect.width - along);
        fill_rect(canvas, Rect{rect.x + along, rect.y, dash, hair}, ink);
        fill_rect(canvas, Rect{rect.x + along, rect.y + rect.height - hair, dash, hair}, ink);
    }
    for (int32_t along = 0; along < rect.height; along += period) {
        const int32_t dash = std::min(compact_metrics.badge_dash, rect.height - along);
        fill_rect(canvas, Rect{rect.x, rect.y + along, hair, dash}, ink);
        fill_rect(canvas, Rect{rect.x + rect.width - hair, rect.y + along, hair, dash}, ink);
    }
}

/// Which corner mark is drawn.
enum class MarkShape : uint8_t { diamond, triangle, ring, square };

/// Tells whether a point of a 0-to-1024 square lies in a mark.
///
/// @param shape which mark
/// @param px the column, 0 to 1024
/// @param py the row, 0 to 1024
/// @return true when the mark covers it
bool mark_covers(MarkShape shape, int64_t px, int64_t py) noexcept {
    const int64_t dx = px > 512 ? px - 512 : 512 - px;
    const int64_t dy = py > 512 ? py - 512 : 512 - py;
    if (shape == MarkShape::diamond)
        return dx + dy <= 430;
    if (shape == MarkShape::triangle) {
        constexpr int64_t x1 = 512;
        constexpr int64_t y1 = 80;
        constexpr int64_t x2 = 80;
        constexpr int64_t y2 = 940;
        constexpr int64_t x3 = 944;
        constexpr int64_t y3 = 940;
        constexpr int64_t denom = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3);
        const int64_t wa = (y2 - y3) * (px - x3) + (x3 - x2) * (py - y3);
        const int64_t wb = (y3 - y1) * (px - x3) + (x1 - x3) * (py - y3);
        return wa >= 0 && wb >= 0 && wa + wb <= denom;
    }
    if (shape == MarkShape::ring) {
        const int64_t dist = dx * dx + dy * dy;
        return dist >= 280 * 280 && dist <= 470 * 470;
    }
    const bool outer = px >= 160 && px <= 860 && py >= 160 && py <= 860;
    const bool inner = px >= 300 && px <= 720 && py >= 300 && py <= 720;
    return outer && !inner;
}

/// Blends a mark into a rectangle at the canvas's own pixels.
///
/// @param canvas where it is drawn
/// @param box the mark's rectangle, in points
/// @param shape which mark
/// @param ink its colour
void blend_mark(const Canvas& canvas, const Rect& box, MarkShape shape, Colour ink) {
    const Point pixels = covered_pixels(canvas, box);
    if (pixels.x <= 0 || pixels.y <= 0 || pixels.x > kMostPixels || pixels.y > kMostPixels)
        return;
    const std::size_t count =
        static_cast<std::size_t>(pixels.x) * static_cast<std::size_t>(pixels.y);
    std::vector<uint8_t> coverage(count, 0);
    constexpr int64_t samples = static_cast<int64_t>(kSamples) * kSamples;
    for (int32_t row = 0; row < pixels.y; ++row) {
        for (int32_t column = 0; column < pixels.x; ++column) {
            int64_t inside = 0;
            for (int32_t down = 0; down < kSamples; ++down) {
                for (int32_t across = 0; across < kSamples; ++across) {
                    const int64_t sample_x = static_cast<int64_t>(8) * column + 2 * across + 1;
                    const int64_t sample_y = static_cast<int64_t>(8) * row + 2 * down + 1;
                    const int64_t px = sample_x * 1024 / (static_cast<int64_t>(pixels.x) * 8);
                    const int64_t py = sample_y * 1024 / (static_cast<int64_t>(pixels.y) * 8);
                    if (mark_covers(shape, px, py))
                        ++inside;
                }
            }
            coverage
                [static_cast<std::size_t>(row) * static_cast<std::size_t>(pixels.x) +
                 static_cast<std::size_t>(column)] =
                    static_cast<uint8_t>((inside * 255 + samples / 2) / samples);
        }
    }
    blend_coverage(canvas, box, coverage, ink);
}

/// Returns the font a keycap's label is drawn in.
///
/// @param look the keycap
/// @return the font
FontRole label_role(const KeycapLook& look) noexcept {
    if (look.small || !look.sub.empty())
        return FontRole::small;
    return FontRole::regular;
}

/// Draws a line fitted to a box with the kit's ellipsis.
///
/// @param canvas where it is drawn
/// @param role the font
/// @param text the line, in UTF-8
/// @param box the box, in points
/// @param align where the line sits
/// @param ink the colour
void draw_fitted(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Align align,
    Colour ink
) {
    if (text.empty() || box.width <= 0 || box.height <= 0)
        return;
    const std::string fitted = fit(text, box.width, [&](std::string_view piece) {
        return measure_text(canvas, role, piece);
    });
    if (!fitted.empty())
        box_text(canvas, role, fitted, box, align, ink);
}

/// Sine of a degree, in 256ths. The angle wraps to a full turn.
///
/// @param degrees the angle, in degrees
/// @return the sine
int32_t sin_deg(int32_t degrees) noexcept {
    int32_t wrapped = degrees % 360;
    if (wrapped < 0)
        wrapped += 360;
    if (wrapped <= 90)
        return kSin256[static_cast<std::size_t>(wrapped)];
    if (wrapped <= 180)
        return kSin256[static_cast<std::size_t>(180 - wrapped)];
    if (wrapped <= 270)
        return -kSin256[static_cast<std::size_t>(wrapped - 180)];
    return -kSin256[static_cast<std::size_t>(360 - wrapped)];
}

/// Cosine of a degree, in 256ths.
///
/// @param degrees the angle, in degrees
/// @return the cosine
int32_t cos_deg(int32_t degrees) noexcept {
    return sin_deg(degrees + 90);
}

/// Returns how far a hold ring fills, in 65536ths of a turn.
///
/// Zero is empty and kRingFull is a whole turn. Reduced motion snaps down
/// to a quarter.
///
/// @param look the ring
/// @return the fraction, 0 to kRingFull
int32_t ring_fraction(const HoldRingLook& look) noexcept {
    if (look.whole <= 0 || look.done <= 0)
        return 0;
    if (look.done >= look.whole)
        return kRingFull;
    if (look.reduced_motion) {
        const int64_t quarters =
            std::clamp(static_cast<int64_t>(look.done) * 4 / look.whole, int64_t{0}, int64_t{4});
        return static_cast<int32_t>(quarters * kRingFull / 4);
    }
    const int64_t scaled = static_cast<int64_t>(look.done) * kRingFull / look.whole;
    return static_cast<int32_t>(std::clamp(scaled, int64_t{0}, static_cast<int64_t>(kRingFull)));
}

/// Tells whether a point lies in the clockwise sector from the top.
///
/// @param dx the column from the centre
/// @param dy the row from the centre, down positive
/// @param degrees the sector, in degrees clockwise from the top
/// @return true when the point is inside the sector
bool in_sector(int64_t dx, int64_t dy, int32_t degrees) noexcept {
    if (degrees <= 0)
        return false;
    if (degrees >= 360)
        return true;
    const int32_t end_x = sin_deg(degrees);
    const int32_t end_y = -cos_deg(degrees);
    const int64_t from_top = dx;
    const int64_t before_end = dx * end_y - dy * end_x;
    if (degrees <= 180)
        return from_top >= 0 && before_end >= 0;
    return from_top >= 0 || before_end >= 0;
}

/// Appends one key of a keyboard shape.
///
/// @param[in,out] out the places
/// @param code the W3C code
/// @param label the geometry's printing
/// @param x the left, in quarter units
/// @param y the top, in quarter units
/// @param w the width, in quarter units
/// @param h the height, in quarter units
void put_key(
    std::vector<KeyPlace>& out,
    std::string_view code,
    std::string_view label,
    int x,
    int y,
    int w,
    int h
) {
    out.push_back(
        KeyPlace{
            code,
            label,
            static_cast<int16_t>(x),
            static_cast<int16_t>(y),
            static_cast<int16_t>(w),
            static_cast<int16_t>(h),
        }
    );
}

/// Builds one shape's places, in reading order.
///
/// @param shape which keyboard
/// @return the places
std::vector<KeyPlace> build_places(KeyboardShape shape) {
    const bool iso = shape == KeyboardShape::iso;
    const bool jis = shape == KeyboardShape::jis;
    std::vector<KeyPlace> out;
    out.reserve(112);

    put_key(out, "Escape", "Esc", 0, 0, 4, 4);
    put_key(out, "F1", "F1", 8, 0, 4, 4);
    put_key(out, "F2", "F2", 12, 0, 4, 4);
    put_key(out, "F3", "F3", 16, 0, 4, 4);
    put_key(out, "F4", "F4", 20, 0, 4, 4);
    put_key(out, "F5", "F5", 26, 0, 4, 4);
    put_key(out, "F6", "F6", 30, 0, 4, 4);
    put_key(out, "F7", "F7", 34, 0, 4, 4);
    put_key(out, "F8", "F8", 38, 0, 4, 4);
    put_key(out, "F9", "F9", 44, 0, 4, 4);
    put_key(out, "F10", "F10", 48, 0, 4, 4);
    put_key(out, "F11", "F11", 52, 0, 4, 4);
    put_key(out, "F12", "F12", 56, 0, 4, 4);
    put_key(out, "PrintScreen", "Prt", 61, 0, 4, 4);
    put_key(out, "ScrollLock", "Scr", 65, 0, 4, 4);
    put_key(out, "Pause", "Pau", 69, 0, 4, 4);

    put_key(out, "Backquote", "`", 0, 5, 4, 4);
    put_key(out, "Digit1", "1", 4, 5, 4, 4);
    put_key(out, "Digit2", "2", 8, 5, 4, 4);
    put_key(out, "Digit3", "3", 12, 5, 4, 4);
    put_key(out, "Digit4", "4", 16, 5, 4, 4);
    put_key(out, "Digit5", "5", 20, 5, 4, 4);
    put_key(out, "Digit6", "6", 24, 5, 4, 4);
    put_key(out, "Digit7", "7", 28, 5, 4, 4);
    put_key(out, "Digit8", "8", 32, 5, 4, 4);
    put_key(out, "Digit9", "9", 36, 5, 4, 4);
    put_key(out, "Digit0", "0", 40, 5, 4, 4);
    put_key(out, "Minus", "-", 44, 5, 4, 4);
    put_key(out, "Equal", "=", 48, 5, 4, 4);
    if (jis) {
        put_key(out, "IntlYen", "Yn", 52, 5, 4, 4);
        put_key(out, "Backspace", "Bksp", 56, 5, 4, 4);
    } else {
        put_key(out, "Backspace", "Bksp", 52, 5, 8, 4);
    }
    put_key(out, "Insert", "Ins", 61, 5, 4, 4);
    put_key(out, "Home", "Home", 65, 5, 4, 4);
    put_key(out, "PageUp", "PgUp", 69, 5, 4, 4);
    put_key(out, "NumLock", "Num", 75, 5, 4, 4);
    put_key(out, "NumpadDivide", "/", 79, 5, 4, 4);
    put_key(out, "NumpadMultiply", "*", 83, 5, 4, 4);
    put_key(out, "NumpadSubtract", "-", 87, 5, 4, 4);

    put_key(out, "Tab", "Tab", 0, 9, 6, 4);
    put_key(out, "KeyQ", "Q", 6, 9, 4, 4);
    put_key(out, "KeyW", "W", 10, 9, 4, 4);
    put_key(out, "KeyE", "E", 14, 9, 4, 4);
    put_key(out, "KeyR", "R", 18, 9, 4, 4);
    put_key(out, "KeyT", "T", 22, 9, 4, 4);
    put_key(out, "KeyY", "Y", 26, 9, 4, 4);
    put_key(out, "KeyU", "U", 30, 9, 4, 4);
    put_key(out, "KeyI", "I", 34, 9, 4, 4);
    put_key(out, "KeyO", "O", 38, 9, 4, 4);
    put_key(out, "KeyP", "P", 42, 9, 4, 4);
    put_key(out, "BracketLeft", "[", 46, 9, 4, 4);
    put_key(out, "BracketRight", "]", 50, 9, 4, 4);
    if (iso)
        put_key(out, "Enter", "Ent", 54, 9, 6, 8);
    else
        put_key(out, "Backslash", "\\", 54, 9, 6, 4);
    put_key(out, "Delete", "Del", 61, 9, 4, 4);
    put_key(out, "End", "End", 65, 9, 4, 4);
    put_key(out, "PageDown", "PgDn", 69, 9, 4, 4);
    put_key(out, "Numpad7", "7", 75, 9, 4, 4);
    put_key(out, "Numpad8", "8", 79, 9, 4, 4);
    put_key(out, "Numpad9", "9", 83, 9, 4, 4);
    put_key(out, "NumpadAdd", "+", 87, 9, 4, 8);

    put_key(out, "CapsLock", "Caps", 0, 13, 7, 4);
    put_key(out, "KeyA", "A", 7, 13, 4, 4);
    put_key(out, "KeyS", "S", 11, 13, 4, 4);
    put_key(out, "KeyD", "D", 15, 13, 4, 4);
    put_key(out, "KeyF", "F", 19, 13, 4, 4);
    put_key(out, "KeyG", "G", 23, 13, 4, 4);
    put_key(out, "KeyH", "H", 27, 13, 4, 4);
    put_key(out, "KeyJ", "J", 31, 13, 4, 4);
    put_key(out, "KeyK", "K", 35, 13, 4, 4);
    put_key(out, "KeyL", "L", 39, 13, 4, 4);
    put_key(out, "Semicolon", ";", 43, 13, 4, 4);
    put_key(out, "Quote", "'", 47, 13, 4, 4);
    if (!iso)
        put_key(out, "Enter", "Ent", 51, 13, 9, 4);
    put_key(out, "Numpad4", "4", 75, 13, 4, 4);
    put_key(out, "Numpad5", "5", 79, 13, 4, 4);
    put_key(out, "Numpad6", "6", 83, 13, 4, 4);

    if (iso)
        put_key(out, "ShiftLeft", "Sh", 0, 17, 5, 4);
    else if (jis)
        put_key(out, "ShiftLeft", "Sh", 0, 17, 5, 4);
    else
        put_key(out, "ShiftLeft", "Sh", 0, 17, 9, 4);
    if (iso)
        put_key(out, "IntlBackslash", "<", 5, 17, 4, 4);
    if (jis)
        put_key(out, "IntlRo", "Ro", 5, 17, 4, 4);
    put_key(out, "KeyZ", "Z", 9, 17, 4, 4);
    put_key(out, "KeyX", "X", 13, 17, 4, 4);
    put_key(out, "KeyC", "C", 17, 17, 4, 4);
    put_key(out, "KeyV", "V", 21, 17, 4, 4);
    put_key(out, "KeyB", "B", 25, 17, 4, 4);
    put_key(out, "KeyN", "N", 29, 17, 4, 4);
    put_key(out, "KeyM", "M", 33, 17, 4, 4);
    put_key(out, "Comma", ",", 37, 17, 4, 4);
    put_key(out, "Period", ".", 41, 17, 4, 4);
    put_key(out, "Slash", "/", 45, 17, 4, 4);
    put_key(out, "ShiftRight", "Sh", 49, 17, 11, 4);
    put_key(out, "ArrowUp", "Up", 65, 17, 4, 4);
    put_key(out, "Numpad1", "1", 75, 17, 4, 4);
    put_key(out, "Numpad2", "2", 79, 17, 4, 4);
    put_key(out, "Numpad3", "3", 83, 17, 4, 4);
    put_key(out, "NumpadEnter", "Ent", 87, 17, 4, 8);

    if (jis) {
        put_key(out, "ControlLeft", "Ctl", 0, 21, 5, 4);
        put_key(out, "MetaLeft", "Win", 5, 21, 5, 4);
        put_key(out, "AltLeft", "Alt", 10, 21, 5, 4);
        put_key(out, "NonConvert", "Ncv", 15, 21, 5, 4);
        put_key(out, "Space", "", 20, 21, 15, 4);
        put_key(out, "Convert", "Cnv", 35, 21, 5, 4);
        put_key(out, "KanaMode", "Kana", 40, 21, 5, 4);
        put_key(out, "AltRight", "Alt", 45, 21, 5, 4);
        put_key(out, "MetaRight", "Win", 50, 21, 5, 4);
        put_key(out, "ControlRight", "Ctl", 55, 21, 5, 4);
    } else {
        put_key(out, "ControlLeft", "Ctl", 0, 21, 5, 4);
        put_key(out, "MetaLeft", "Win", 5, 21, 5, 4);
        put_key(out, "AltLeft", "Alt", 10, 21, 5, 4);
        put_key(out, "Space", "", 15, 21, 25, 4);
        put_key(out, "AltRight", "Alt", 40, 21, 5, 4);
        put_key(out, "MetaRight", "Win", 45, 21, 5, 4);
        put_key(out, "ContextMenu", "Menu", 50, 21, 5, 4);
        put_key(out, "ControlRight", "Ctl", 55, 21, 5, 4);
    }
    put_key(out, "ArrowLeft", "Lt", 61, 21, 4, 4);
    put_key(out, "ArrowDown", "Dn", 65, 21, 4, 4);
    put_key(out, "ArrowRight", "Rt", 69, 21, 4, 4);
    put_key(out, "Numpad0", "0", 75, 21, 8, 4);
    put_key(out, "NumpadDecimal", ".", 83, 21, 4, 4);
    return out;
}

/// Where a keyboard's quarter grid lands in a rectangle.
struct Board {
    int32_t quarter{1}; ///< points per quarter unit
    int32_t origin_x{}; ///< the grid's left, in points
    int32_t origin_y{}; ///< the grid's top, in points
};

/// Returns the quarter grid that fits a rectangle in whole points.
///
/// @param rect the picture, in points
/// @return the grid
Board board_of(const Rect& rect) noexcept {
    int32_t quarter = 1;
    if (rect.width >= 91 && rect.height >= 25)
        quarter = std::min(rect.width / 91, rect.height / 25);
    const int32_t board_w = 91 * quarter;
    const int32_t board_h = 25 * quarter;
    Board board;
    board.quarter = std::max(quarter, int32_t{1});
    board.origin_x = rect.x + std::max(rect.width - board_w, int32_t{0}) / 2;
    board.origin_y = rect.y + std::max(rect.height - board_h, int32_t{0}) / 2;
    return board;
}

/// Returns a key's rectangle on a grid, inset so neighbours do not touch
/// when the unit is wide enough.
///
/// @param place the key
/// @param board the grid
/// @return the rectangle, in points
Rect key_rect(const KeyPlace& place, const Board& board) noexcept {
    int32_t inset = 0;
    if (board.quarter >= 8)
        inset = 2;
    else if (board.quarter >= 4)
        inset = 1;
    const int32_t width = static_cast<int32_t>(place.w) * board.quarter;
    const int32_t height = static_cast<int32_t>(place.h) * board.quarter;
    int32_t inset_x = inset * 2 < width ? inset : 0;
    int32_t inset_y = inset * 2 < height ? inset : 0;
    return Rect{
        board.origin_x + static_cast<int32_t>(place.x) * board.quarter + inset_x,
        board.origin_y + static_cast<int32_t>(place.y) * board.quarter + inset_y,
        width - 2 * inset_x,
        height - 2 * inset_y,
    };
}

/// Returns the cap a key draws: the look's entry, or a free key.
///
/// @param place the key
/// @param look the picture
/// @return the cap
KeycapLook shown_cap(const KeyPlace& place, const KeyboardLook& look) {
    KeycapLook cap;
    cap.label = std::string(place.label);
    for (const KeyboardKey& key : look.keys) {
        if (key.code != place.code)
            continue;
        cap = key.cap;
        if (cap.label.empty())
            cap.label = std::string(place.label);
        return cap;
    }
    return cap;
}

/// The mouse picture's design, in points, before it is scaled.
constexpr int32_t kMouseWide = 40;
constexpr int32_t kMouseHigh = 64;

/// One button of the mouse picture, in design points.
struct MouseSlot {
    const char* word{};   ///< the automation word
    MouseButton button{}; ///< which look
    bool extra{};         ///< drawn only when the extra buttons show
    bool wheel{};         ///< the wheel, split by its two looks
    int32_t x{};          ///< the left, in design points
    int32_t y{};          ///< the top, in design points
    int32_t w{};          ///< the width, in design points
    int32_t h{};          ///< the height, in design points
};

/// The buttons, in the order their controls are numbered.
constexpr std::array<MouseSlot, 9> kMouseSlots{{
    {"mouse-left", MouseButton::left, false, false, 8, 4, 10, 16},
    {"mouse-right", MouseButton::right, false, false, 22, 4, 10, 16},
    {"mouse-middle", MouseButton::middle, false, false, 14, 22, 12, 8},
    {"wheel", MouseButton::wheel_up, false, true, 18, 6, 4, 12},
    {"mouse-back", MouseButton::back, false, false, 0, 18, 6, 10},
    {"mouse-forward", MouseButton::forward, false, false, 0, 30, 6, 10},
    {"mouse-6", MouseButton::button6, true, false, 34, 16, 6, 8},
    {"mouse-7", MouseButton::button7, true, false, 34, 26, 6, 8},
    {"mouse-8", MouseButton::button8, true, false, 34, 36, 6, 8},
}};

/// Where the mouse's design lands in a rectangle.
struct MouseBoard {
    int32_t unit{1};    ///< points per design point
    int32_t origin_x{}; ///< the design's left, in points
    int32_t origin_y{}; ///< the design's top, in points
};

/// Returns the mouse design that fits a rectangle.
///
/// @param rect the picture, in points
/// @return the design; unit is 0 when the rectangle cannot hold it
MouseBoard mouse_board(const Rect& rect) noexcept {
    MouseBoard board;
    if (rect.width < kMouseWide || rect.height < kMouseHigh)
        board.unit = 0;
    else
        board.unit = std::min(rect.width / kMouseWide, rect.height / kMouseHigh);
    const int32_t wide = kMouseWide * board.unit;
    const int32_t high = kMouseHigh * board.unit;
    board.origin_x = rect.x + std::max(rect.width - wide, int32_t{0}) / 2;
    board.origin_y = rect.y + std::max(rect.height - high, int32_t{0}) / 2;
    return board;
}

/// Returns a design rectangle on the mouse picture.
///
/// @param board the design
/// @param x the left, in design points
/// @param y the top, in design points
/// @param w the width, in design points
/// @param h the height, in design points
/// @return the rectangle, in points
Rect mouse_rect(const MouseBoard& board, int32_t x, int32_t y, int32_t w, int32_t h) noexcept {
    return Rect{
        board.origin_x + x * board.unit,
        board.origin_y + y * board.unit,
        w * board.unit,
        h * board.unit,
    };
}

/// Returns a button's look.
///
/// @param look the picture
/// @param button which button
/// @return the look
const KeycapLook& button_look(const MouseLook& look, MouseButton button) noexcept {
    return look.buttons[static_cast<std::size_t>(button)];
}

} // namespace

Colour tint_face(KeyTint tint) noexcept {
    switch (tint) {
    case KeyTint::orders:
        return colour::category_orders;
    case KeyTint::pointer:
        return colour::category_pointer;
    case KeyTint::selection:
        return colour::category_selection;
    case KeyTint::groups:
        return colour::category_groups;
    case KeyTint::camera:
        return colour::category_camera;
    case KeyTint::building:
        return colour::category_building;
    case KeyTint::game:
        return colour::category_game;
    case KeyTint::chat:
        return colour::category_chat;
    case KeyTint::window:
        return colour::category_window;
    case KeyTint::hacks:
        return colour::category_hacks;
    case KeyTint::mod:
        return colour::category_mod;
    case KeyTint::developer:
        return colour::category_developer;
    case KeyTint::none:
        return colour::list;
    }
    return colour::list;
}

Colour tint_edge(KeyTint tint) noexcept {
    return darken(tint_face(tint));
}

Colour tint_ink(KeyTint tint) noexcept {
    if (tint == KeyTint::none)
        return colour::text;
    return colour::on_accent;
}

int32_t keycap_width(const Fonts& fonts, const KeycapLook& look) {
    const int32_t least = look.small ? 9 : keycap_least_width;
    const int32_t pad = look.small ? 4 : 8;
    int32_t extra = 0;
    if (!look.small && !look.letter.empty())
        extra = 8;
    const int32_t text = text_width(fonts, label_role(look), look.label);
    return std::max(least, text + pad + extra);
}

void draw_keycap(const Canvas& canvas, const Rect& rect, const KeycapLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const Canvas local = clipped(canvas, rect);

    Colour face = tint_face(look.tint);
    Colour ink = tint_ink(look.tint);
    Colour edge = tint_edge(look.tint);
    if (look.modifier && look.tint == KeyTint::none)
        ink = colour::button_text;
    if (look.small) {
        face = colour::on_accent;
        ink = colour::accent;
        edge = colour::accent_held;
    }
    if (look.held) {
        face = colour::accent;
        ink = colour::on_accent;
        edge = colour::accent_held;
    }
    if (look.disabled) {
        face = colour::well;
        ink = colour::quiet;
        edge = colour::control_border;
    }

    int32_t radius = look.small ? 1 : 2;
    const int32_t shorter = std::min(rect.width, rect.height);
    if (radius * 2 > shorter)
        radius = std::max(shorter / 2, int32_t{0});
    fill_corner_rect(local, rect, radius, face);

    const int32_t edge_band = look.small ? 0 : std::max(int32_t{1}, rect.height / 8);
    if (edge_band > 0 && edge_band < rect.height)
        fill_rect(
            local, Rect{rect.x, rect.y + rect.height - edge_band, rect.width, edge_band}, edge
        );

    const bool letter =
        !look.small && !look.letter.empty() && rect.width >= 12 && rect.height >= 12;
    const int32_t letter_side =
        letter ? std::min(int32_t{8}, std::min(rect.width, rect.height) / 2) : 0;
    const bool marked = look.mark != KeyMark::none && rect.width >= 8 && rect.height >= 8;
    const int32_t mark_side =
        marked ? std::min(int32_t{7}, std::min(rect.width, rect.height) / 2) : 0;

    if (letter_side > 0) {
        const Rect square{
            rect.x + rect.width - letter_side - 1,
            rect.y + rect.height - edge_band - letter_side - 1,
            letter_side,
            letter_side,
        };
        fill_corner_rect(local, square, 1, edge);
        box_text(local, FontRole::small, look.letter, square, Align::centre, ink);
    }

    const int32_t text_right = rect.x + rect.width - (letter_side > 0 ? letter_side + 2 : 2);
    const int32_t text_width_room = std::max(text_right - (rect.x + 2), int32_t{0});
    if (!look.label.empty() && text_width_room > 0) {
        Rect label_box{
            rect.x + 2, rect.y + 1, text_width_room, std::max(rect.height - 2, int32_t{1})
        };
        if (look.small)
            label_box = Rect{rect.x, rect.y, rect.width, rect.height};
        else if (!look.sub.empty())
            label_box.height = std::max((rect.height - edge_band) / 2, int32_t{1});
        const Align align = look.small ? Align::centre : Align::left;
        draw_fitted(local, label_role(look), look.label, label_box, align, ink);
    }
    if (!look.small && !look.sub.empty()) {
        const int32_t sub_top = rect.y + std::max((rect.height - edge_band) / 2, int32_t{1});
        const int32_t sub_height = std::max(rect.y + rect.height - edge_band - sub_top, int32_t{0});
        draw_fitted(
            local,
            FontRole::small,
            look.sub,
            Rect{rect.x + 2, sub_top, text_width_room, sub_height},
            Align::left,
            ink
        );
    }

    if (mark_side > 0) {
        const Rect mark_box{rect.x + rect.width - mark_side - 1, rect.y + 1, mark_side, mark_side};
        if (look.mark == KeyMark::changed)
            blend_mark(local, mark_box, MarkShape::diamond, colour::accent);
        else if (look.mark == KeyMark::clash)
            blend_mark(local, mark_box, MarkShape::triangle, colour::danger);
        else if (look.mark == KeyMark::unbound)
            blend_mark(local, mark_box, MarkShape::ring, colour::quiet);
        else if (look.mark == KeyMark::fixed)
            blend_mark(local, mark_box, MarkShape::square, colour::text);
    }

    if (look.mark == KeyMark::clash)
        dash_border(local, rect, colour::danger);
    else if (look.mark == KeyMark::fixed)
        dash_border(local, rect, colour::control_border);
    else if (look.hovered && !look.disabled)
        outline_rect(local, rect, colour::control_hover);
    if (look.focused)
        outline_rect(local, rect, colour::accent);
}

void add_keycap(
    DisplayList& list, const Rect& rect, const KeycapLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::keycap;
    item.rect = rect;
    item.text = look.label;
    item.control = id;
    item.state.hovered = look.hovered;
    item.state.pressed = look.held;
    item.state.focused = look.focused;
    item.state.disabled = look.disabled;
    item.look = look;
    list.items.push_back(std::move(item));
    if (id == no_control)
        return;
    add_button(list, rect, id, std::move(name), look.label, !look.disabled);
}

int32_t chord_chip_width(const Fonts& fonts, const ChordChipLook& look) {
    if (look.add && look.keys.empty()) {
        int32_t width = text_width(fonts, FontRole::small, kAddCaption) + 8;
        if (!look.none_text.empty())
            width += 4 + text_width(fonts, FontRole::small, look.none_text);
        return std::max(keycap_least_width, width);
    }
    if (look.keys.empty())
        return std::max(keycap_least_width, text_width(fonts, FontRole::small, look.none_text) + 8);
    int32_t width = 0;
    const int32_t plus = text_width(fonts, FontRole::small, "+") + 4;
    for (std::size_t index = 0; index < look.keys.size(); ++index) {
        if (index > 0)
            width += plus;
        KeycapLook cap;
        cap.label = look.keys[index];
        width += keycap_width(fonts, cap);
    }
    if (!look.gesture.empty())
        width += 4 + text_width(fonts, FontRole::small, look.gesture);
    return std::max(keycap_least_width, width);
}

void draw_chord_chip(const Canvas& canvas, const Rect& rect, const ChordChipLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const Canvas local = clipped(canvas, rect);
    if (look.add && look.keys.empty()) {
        dash_border(local, rect, colour::control_border);
        const int32_t caption =
            canvas.fonts != nullptr ? text_width(*canvas.fonts, FontRole::small, kAddCaption) : 0;
        box_text(
            local,
            FontRole::small,
            kAddCaption,
            Rect{rect.x + 4, rect.y, std::max(caption, int32_t{1}), rect.height},
            Align::left,
            colour::button_text
        );
        if (!look.none_text.empty()) {
            const int32_t rest = rect.x + 8 + caption;
            draw_fitted(
                local,
                FontRole::small,
                look.none_text,
                Rect{rest, rect.y, std::max(rect.x + rect.width - rest, int32_t{0}), rect.height},
                Align::left,
                colour::quiet
            );
        }
        return;
    }
    if (look.keys.empty()) {
        draw_fitted(local, FontRole::small, look.none_text, rect, Align::left, colour::quiet);
        return;
    }
    int32_t cursor = rect.x;
    const int32_t plus =
        canvas.fonts != nullptr ? text_width(*canvas.fonts, FontRole::small, "+") + 4 : 8;
    for (std::size_t index = 0; index < look.keys.size(); ++index) {
        if (index > 0) {
            box_text(
                local,
                FontRole::small,
                "+",
                Rect{cursor, rect.y, plus, rect.height},
                Align::centre,
                colour::button_text
            );
            cursor += plus;
        }
        KeycapLook cap;
        cap.label = look.keys[index];
        if (index + 1 == look.keys.size())
            cap.mark = look.mark;
        int32_t width = keycap_least_width;
        if (canvas.fonts != nullptr)
            width = keycap_width(*canvas.fonts, cap);
        if (cursor >= rect.x + rect.width)
            break;
        draw_keycap(local, Rect{cursor, rect.y, width, rect.height}, cap);
        cursor += width;
    }
    if (!look.gesture.empty()) {
        cursor += 4;
        draw_fitted(
            local,
            FontRole::small,
            look.gesture,
            Rect{cursor, rect.y, std::max(rect.x + rect.width - cursor, int32_t{0}), rect.height},
            Align::left,
            colour::hint
        );
    }
    if (look.selected)
        outline_rect(local, rect, colour::accent);
    else if (look.hovered && look.mark != KeyMark::clash)
        outline_rect(local, rect, colour::control_hover);
}

void add_chord_chip(
    DisplayList& list, const Rect& rect, const ChordChipLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::chord_chip;
    item.rect = rect;
    item.control = id;
    item.state.hovered = look.hovered;
    item.state.selected = look.selected;
    std::string text;
    if (look.add && look.keys.empty())
        text = std::string(kAddCaption);
    else if (look.keys.empty())
        text = look.none_text;
    else {
        for (std::size_t index = 0; index < look.keys.size(); ++index) {
            if (index > 0)
                text += "+";
            text += look.keys[index];
        }
        if (!look.gesture.empty()) {
            text += " ";
            text += look.gesture;
        }
    }
    item.text = text;
    item.look = look;
    list.items.push_back(std::move(item));
    if (id == no_control)
        return;
    add_button(list, rect, id, std::move(name), std::move(text), true);
}

std::span<const KeyPlace> keyboard_places(KeyboardShape shape) {
    static const std::vector<KeyPlace> ansi = build_places(KeyboardShape::ansi);
    static const std::vector<KeyPlace> iso = build_places(KeyboardShape::iso);
    static const std::vector<KeyPlace> jis = build_places(KeyboardShape::jis);
    switch (shape) {
    case KeyboardShape::iso:
        return iso;
    case KeyboardShape::jis:
        return jis;
    case KeyboardShape::ansi:
        return ansi;
    }
    return ansi;
}

Point keyboard_size() noexcept {
    return Point{91, 25};
}

void draw_keyboard(const Canvas& canvas, const Rect& rect, const KeyboardLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const Canvas local = clipped(canvas, rect);
    const Board board = board_of(rect);
    for (const KeyPlace& place : keyboard_places(look.shape))
        draw_keycap(local, key_rect(place, board), shown_cap(place, look));
}

void add_keyboard(
    DisplayList& list, const Rect& rect, const KeyboardLook& look, ControlId first, std::string name
) {
    Item item;
    item.role = Role::keyboard;
    item.rect = rect;
    item.look = look;
    list.items.push_back(std::move(item));
    const Board board = board_of(rect);
    const std::span<const KeyPlace> places = keyboard_places(look.shape);
    for (std::size_t index = 0; index < places.size(); ++index) {
        const KeyPlace& place = places[index];
        const KeycapLook cap = shown_cap(place, look);
        const Rect key = key_rect(place, board);
        if (key.width <= 0 || key.height <= 0)
            continue;
        add_button(
            list,
            key,
            first + static_cast<ControlId>(index),
            hang_name(name, code_word(place.code)),
            cap.label,
            !cap.disabled
        );
    }
}

void draw_mouse(const Canvas& canvas, const Rect& rect, const MouseLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const MouseBoard board = mouse_board(rect);
    if (board.unit <= 0)
        return;
    const Canvas local = clipped(canvas, rect);
    const Rect body = mouse_rect(board, 6, 2, 28, 60);
    const int32_t radius = std::max(board.unit * 6, int32_t{1});
    fill_corner_rect(local, body, radius, colour::list);
    outline_corner_rect(local, body, radius, colour::control_border);
    for (const MouseSlot& slot : kMouseSlots) {
        if (slot.extra && !look.extra_buttons)
            continue;
        const Rect area = mouse_rect(board, slot.x, slot.y, slot.w, slot.h);
        if (slot.wheel) {
            const int32_t half = area.height / 2;
            KeycapLook upper = button_look(look, MouseButton::wheel_up);
            KeycapLook lower = button_look(look, MouseButton::wheel_down);
            if (upper.label.empty())
                upper.label = "+";
            if (lower.label.empty())
                lower.label = "-";
            draw_keycap(local, Rect{area.x, area.y, area.width, std::max(half, int32_t{1})}, upper);
            draw_keycap(
                local,
                Rect{area.x, area.y + half, area.width, std::max(area.height - half, int32_t{1})},
                lower
            );
            continue;
        }
        draw_keycap(local, area, button_look(look, slot.button));
    }
}

void add_mouse(
    DisplayList& list, const Rect& rect, const MouseLook& look, ControlId first, std::string name
) {
    Item item;
    item.role = Role::mouse_picture;
    item.rect = rect;
    item.look = look;
    list.items.push_back(std::move(item));
    const MouseBoard board = mouse_board(rect);
    if (board.unit <= 0)
        return;
    ControlId next = first;
    for (const MouseSlot& slot : kMouseSlots) {
        if (slot.extra && !look.extra_buttons)
            continue;
        const Rect area = mouse_rect(board, slot.x, slot.y, slot.w, slot.h);
        if (area.width <= 0 || area.height <= 0)
            continue;
        const KeycapLook& cap = button_look(look, slot.button);
        bool enabled = !cap.disabled;
        if (slot.wheel)
            enabled = !button_look(look, MouseButton::wheel_up).disabled ||
                      !button_look(look, MouseButton::wheel_down).disabled;
        add_button(list, area, next, hang_name(name, slot.word), cap.label, enabled);
        ++next;
    }
}

void draw_hold_ring(const Canvas& canvas, const Rect& rect, const HoldRingLook& look) {
    if (!can_draw(canvas) || rect.width <= 0 || rect.height <= 0)
        return;
    const Point pixels = covered_pixels(canvas, rect);
    if (pixels.x <= 0 || pixels.y <= 0 || pixels.x > kMostPixels || pixels.y > kMostPixels)
        return;
    const int32_t fraction = ring_fraction(look);
    const int32_t degrees =
        static_cast<int32_t>((static_cast<int64_t>(fraction) * 360) / kRingFull);
    const int64_t side = std::min(pixels.x, pixels.y);
    const int64_t outer = side * 8 * 46 / 100;
    const int64_t inner = side * 8 * 30 / 100;
    const int64_t centre_x = static_cast<int64_t>(pixels.x) * 4;
    const int64_t centre_y = static_cast<int64_t>(pixels.y) * 4;
    const std::size_t count =
        static_cast<std::size_t>(pixels.x) * static_cast<std::size_t>(pixels.y);
    std::vector<uint8_t> track(count, 0);
    std::vector<uint8_t> sector(count, 0);
    constexpr int64_t samples = static_cast<int64_t>(kSamples) * kSamples;
    for (int32_t row = 0; row < pixels.y; ++row) {
        for (int32_t column = 0; column < pixels.x; ++column) {
            int64_t in_ring = 0;
            int64_t in_fill = 0;
            for (int32_t down = 0; down < kSamples; ++down) {
                for (int32_t across = 0; across < kSamples; ++across) {
                    const int64_t sample_x = static_cast<int64_t>(8) * column + 2 * across + 1;
                    const int64_t sample_y = static_cast<int64_t>(8) * row + 2 * down + 1;
                    const int64_t dx = sample_x - centre_x;
                    const int64_t dy = sample_y - centre_y;
                    const int64_t dist = dx * dx + dy * dy;
                    if (dist < inner * inner || dist > outer * outer)
                        continue;
                    ++in_ring;
                    if (in_sector(dx, dy, degrees))
                        ++in_fill;
                }
            }
            const std::size_t at =
                static_cast<std::size_t>(row) * static_cast<std::size_t>(pixels.x) +
                static_cast<std::size_t>(column);
            track[at] = static_cast<uint8_t>((in_ring * 255 + samples / 2) / samples);
            sector[at] = static_cast<uint8_t>((in_fill * 255 + samples / 2) / samples);
        }
    }
    const Canvas local = clipped(canvas, rect);
    blend_coverage(local, rect, track, colour::control_border);
    if (fraction > 0)
        blend_coverage(local, rect, sector, colour::accent);
}

void add_hold_ring(DisplayList& list, const Rect& rect, const HoldRingLook& look) {
    Item item;
    item.role = Role::hold_ring;
    item.rect = rect;
    item.look = look;
    list.items.push_back(std::move(item));
}

std::string code_word(std::string_view code) {
    std::string out;
    int32_t uppers = 0;
    for (std::size_t index = 0; index < code.size(); ++index) {
        const auto character = static_cast<unsigned char>(code[index]);
        if (character >= 'A' && character <= 'Z') {
            bool hyphen = false;
            if (index > 0) {
                const auto previous = static_cast<unsigned char>(code[index - 1]);
                const bool previous_lower = previous >= 'a' && previous <= 'z';
                const bool previous_digit = previous >= '0' && previous <= '9';
                if (previous_lower || previous_digit) {
                    hyphen = true;
                } else if (previous >= 'A' && previous <= 'Z' && index + 1 < code.size()) {
                    const auto next = static_cast<unsigned char>(code[index + 1]);
                    if (next >= 'a' && next <= 'z')
                        hyphen = true;
                }
            }
            if (hyphen && !out.empty() && out.back() != '-')
                out.push_back('-');
            out.push_back(static_cast<char>(character - 'A' + 'a'));
            uppers = hyphen ? 1 : uppers + 1;
        } else if (character >= '0' && character <= '9') {
            if (index > 0) {
                const auto previous = static_cast<unsigned char>(code[index - 1]);
                const bool previous_digit = previous >= '0' && previous <= '9';
                if (!previous_digit && uppers != 1 && !out.empty() && out.back() != '-')
                    out.push_back('-');
            }
            out.push_back(static_cast<char>(character));
            uppers = 0;
        } else if (character >= 'a' && character <= 'z') {
            out.push_back(static_cast<char>(character));
            uppers = 0;
        } else {
            uppers = 0;
        }
    }
    return out;
}

} // namespace oa::ui::kit
