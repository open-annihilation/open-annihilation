// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_state/oa_button_place.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace oa::ui::frontend_state {
namespace {

/// Tells whether two rectangles share an interior point.
///
/// @param first a rectangle
/// @param second another
/// @return true when they meet
bool meet(const PlaceRect& first, const PlaceRect& second) noexcept {
    return first.x < second.x + second.width && second.x < first.x + first.width &&
           first.y < second.y + second.height && second.y < first.y + first.height;
}

/// Tells whether a rectangle lies wholly inside a room.
///
/// @param rect the rectangle
/// @param room the room
/// @return true when every edge is inside
bool inside(const PlaceRect& rect, const PlaceRect& room) noexcept {
    return rect.x >= room.x && rect.y >= room.y && rect.x + rect.width <= room.x + room.width &&
           rect.y + rect.height <= room.y + room.height;
}

/// Tells whether a rectangle meets any of a list.
///
/// @param rect the rectangle
/// @param others the list
/// @return true when one meets it
bool meets_any(const PlaceRect& rect, std::span<const PlaceRect> others) noexcept {
    return std::any_of(others.begin(), others.end(), [&](const PlaceRect& other) {
        return meet(rect, other);
    });
}

/// Returns the width of the button row.
///
/// No listed widths is one button of `button_width`. Several widths stand
/// `button_gap` apart.
///
/// @param sizes the sizes
/// @return the row's width, in points
int32_t row_width(const BlockSizes& sizes) noexcept {
    if (sizes.button_widths.empty())
        return sizes.button_width;
    int32_t width = 0;
    for (std::size_t index = 0; index < sizes.button_widths.size(); ++index) {
        if (index > 0)
            width += sizes.button_gap;
        width += sizes.button_widths[index];
    }
    return width;
}

/// Returns the block's height: the button, then each line a point below the part above it.
///
/// @param sizes the sizes
/// @param lines how many lines
/// @return the height, in points
int32_t block_height(const BlockSizes& sizes, std::size_t lines) noexcept {
    if (lines == 0)
        return sizes.button_height;
    return sizes.button_height + static_cast<int32_t>(lines) * (sizes.line_height + 1);
}

/// Shortens a line from the left so it ends before every rectangle it would meet.
///
/// The right edge stays. A rectangle that covers that edge leaves nothing.
/// A line shorter than the least width is left out.
///
/// @param top the line's top
/// @param right the right edge that is kept
/// @param sizes the sizes
/// @param others the rectangles
/// @return the line, or nothing when it is left out
std::optional<PlaceRect> shorten_line(
    int32_t top, int32_t right, const BlockSizes& sizes, std::span<const PlaceRect> others
) {
    int32_t left = right - sizes.line_width;
    int32_t width = sizes.line_width;
    for (int32_t guard = 0; guard < 64 && width > 0; ++guard) {
        bool moved = false;
        for (const PlaceRect& obstacle : others) {
            if (width <= 0)
                break;
            const PlaceRect line{left, top, width, sizes.line_height};
            if (!meet(line, obstacle) &&
                !(obstacle.y < top + sizes.line_height && top < obstacle.y + obstacle.height &&
                  obstacle.x < right && obstacle.x + obstacle.width > left))
                continue;
            const int32_t obstacle_right = obstacle.x + obstacle.width;
            if (obstacle.x < right && obstacle_right >= right) {
                left = right;
                width = 0;
                break;
            }
            if (obstacle_right > left && obstacle.x < right) {
                left = obstacle_right;
                width = right - left;
                moved = true;
            }
        }
        if (!moved)
            break;
    }
    if (width < sizes.least_line_width)
        return std::nullopt;
    return PlaceRect{left, top, width, sizes.line_height};
}

/// Builds the block with its top at a row, shortening each line.
///
/// @param top the button's top
/// @param lines how many lines are asked for
/// @param anchor the anchor
/// @param others the rectangles
/// @param sizes the sizes
/// @return the button and the lines that were kept
BlockPlace compose(
    int32_t top,
    std::size_t lines,
    const PlaceRect& anchor,
    std::span<const PlaceRect> others,
    const BlockSizes& sizes
) {
    BlockPlace place;
    const int32_t right = anchor.x + anchor.width;
    int32_t cursor = right - row_width(sizes);
    if (sizes.button_widths.empty()) {
        place.buttons.push_back(PlaceRect{cursor, top, sizes.button_width, sizes.button_height});
    } else {
        for (std::size_t index = 0; index < sizes.button_widths.size(); ++index) {
            const int32_t width = sizes.button_widths[index];
            place.buttons.push_back(PlaceRect{cursor, top, width, sizes.button_height});
            cursor += width + sizes.button_gap;
        }
    }
    place.button = place.buttons.back();
    int32_t line_top = place.button.y + place.button.height + 1;
    place.lines.reserve(lines);
    for (std::size_t index = 0; index < lines; ++index) {
        if (const std::optional<PlaceRect> line = shorten_line(line_top, right, sizes, others))
            place.lines.push_back(*line);
        line_top += sizes.line_height + 1;
    }
    return place;
}

/// Tells whether a built block is a whole place inside the room that meets nothing.
///
/// @param place the block
/// @param lines how many lines were asked for
/// @param others the rectangles
/// @param room the room
/// @return true when every asked line was kept and the block is clear
bool accepts(
    const BlockPlace& place,
    std::size_t lines,
    std::span<const PlaceRect> others,
    const PlaceRect& room,
    const BlockSizes& sizes
) noexcept {
    if (place.lines.size() != lines)
        return false;
    for (const PlaceRect& one : place.buttons) {
        if (!inside(one, room) || meets_any(one, others))
            return false;
    }
    if (sizes.beside > 0) {
        const PlaceRect side{
            place.button.x + place.button.width, place.button.y, sizes.beside, place.button.height
        };
        if (!inside(side, room) || meets_any(side, others))
            return false;
    }
    return std::all_of(place.lines.begin(), place.lines.end(), [&](const PlaceRect& line) {
        return inside(line, room) && !meets_any(line, others);
    });
}

/// Moves a block up until its unshortened bounds meet nothing, leaving the gap.
///
/// @param top the block's top, moved up
/// @param height the block's height
/// @param anchor the anchor
/// @param others the rectangles
/// @param sizes the sizes
/// @param lines how many lines the bounds cover
/// @return the top after the move
int32_t lift_clear(
    int32_t top,
    int32_t height,
    const PlaceRect& anchor,
    std::span<const PlaceRect> others,
    const BlockSizes& sizes,
    std::size_t lines
) noexcept {
    const int32_t buttons = row_width(sizes);
    const int32_t span = lines == 0 ? buttons : std::max(buttons, sizes.line_width);
    const int32_t right = anchor.x + anchor.width;
    for (int32_t guard = 0; guard < 64; ++guard) {
        const PlaceRect bounds{right - span, top, span + sizes.beside, height};
        int32_t next = top;
        bool hit = false;
        for (const PlaceRect& obstacle : others) {
            if (!meet(bounds, obstacle))
                continue;
            hit = true;
            const int32_t candidate = obstacle.y - sizes.gap - height;
            if (candidate < next)
                next = candidate;
        }
        if (!hit || next >= top)
            break;
        top = next;
    }
    return top;
}

/// Returns the bottom of the last rectangle over a row that shares some of a
/// run of columns, or the room's top when none does.
///
/// @param row the row the space ends on
/// @param left the run's left edge
/// @param right the run's right edge, outside the run
/// @param others the rectangles
/// @param room the room
/// @return the space's top edge
int32_t last_above(
    int32_t row,
    int32_t left,
    int32_t right,
    std::span<const PlaceRect> others,
    const PlaceRect& room
) noexcept {
    int32_t top = room.y;
    for (const PlaceRect& other : others) {
        if (other.width <= 0 || other.height <= 0 || other.y + other.height > row)
            continue;
        if (other.x >= right || other.x + other.width <= left)
            continue;
        top = std::max(top, other.y + other.height);
    }
    return top;
}

} // namespace

BlockPlace place_block(
    const PlaceRect& anchor,
    std::span<const PlaceRect> others,
    const PlaceRect& room,
    const BlockSizes& sizes
) {
    const BlockPlace below =
        compose(anchor.y + anchor.height + sizes.gap, sizes.lines, anchor, others, sizes);
    for (std::size_t lines = sizes.lines + 1; lines > 0; --lines) {
        const std::size_t count = lines - 1;
        const BlockPlace under =
            count == sizes.lines
                ? below
                : compose(anchor.y + anchor.height + sizes.gap, count, anchor, others, sizes);
        if (accepts(under, count, others, room, sizes))
            return under;
        const int32_t height = block_height(sizes, count);
        const int32_t top =
            lift_clear(anchor.y - sizes.gap - height, height, anchor, others, sizes, count);
        const BlockPlace over = compose(top, count, anchor, others, sizes);
        if (accepts(over, count, others, room, sizes))
            return over;
    }
    return below;
}

std::optional<PlaceRect>
next_below(const PlaceRect& anchor, std::span<const PlaceRect> others) noexcept {
    const int32_t bottom = anchor.y + anchor.height;
    std::optional<PlaceRect> nearest;
    for (const PlaceRect& other : others) {
        if (other.width <= 0 || other.height <= 0 || other.y < bottom)
            continue;
        if (other.x >= anchor.x + anchor.width || other.x + other.width <= anchor.x)
            continue;
        if (!nearest || other.y < nearest->y || (other.y == nearest->y && other.x < nearest->x))
            nearest = other;
    }
    return nearest;
}

std::optional<PlaceRect> place_in_column(
    const PlaceRect& anchor, std::span<const PlaceRect> others, const PlaceRect& room, int32_t gap
) {
    const std::optional<PlaceRect> model = next_below(anchor, others);
    if (!model)
        return std::nullopt;
    const int32_t top = anchor.y + anchor.height;
    const int32_t space = model->y - top;
    if (space < model->height + 2 * gap)
        return std::nullopt;
    const PlaceRect button{
        model->x, top + (space - model->height) / 2, model->width, model->height
    };
    if (!inside(button, room) || meets_any(button, others))
        return std::nullopt;
    return button;
}

PlaceRect bar_middle(const PlaceRect& left, const PlaceRect& right) noexcept {
    const int32_t bolt = left.height * 9 / 20;
    const int32_t from = left.x + left.width + bolt;
    const int32_t to = right.x - bolt;
    if (to <= from || left.height <= 0)
        return {};
    return {from, left.y, to - from, left.height};
}

std::optional<PlaceRect> place_under_middle(
    const PlaceRect& left,
    const PlaceRect& right,
    std::span<const PlaceRect> others,
    const PlaceRect& room
) {
    const PlaceRect middle = bar_middle(left, right);
    if (middle.width <= 0)
        return std::nullopt;
    const int32_t bottom = std::max(left.y + left.height, right.y + right.height);
    const int32_t bolt = left.height * 9 / 20;
    const PlaceRect button{middle.x, bottom + bolt, middle.width, middle.height};
    if (!inside(button, room) || meets_any(button, others))
        return std::nullopt;
    return button;
}

std::optional<PlaceRect> place_over(
    const PlaceRect& anchor, std::span<const PlaceRect> others, const PlaceRect& room, int32_t gap
) {
    const int32_t top = last_above(anchor.y, anchor.x, anchor.x + anchor.width, others, room);
    const int32_t space = anchor.y - top;
    if (space < anchor.height + 2 * gap)
        return std::nullopt;
    const PlaceRect button{
        anchor.x, top + (space - anchor.height) / 2, anchor.width, anchor.height
    };
    if (!inside(button, room) || meets_any(button, others))
        return std::nullopt;
    return button;
}

std::size_t focus_place(std::span<const PlaceRect> focusable, const PlaceRect& button) {
    const int64_t key = static_cast<int64_t>(button.y) * 5000 + button.x;
    std::size_t count = 0;
    for (const PlaceRect& rect : focusable) {
        const int64_t other = static_cast<int64_t>(rect.y) * 5000 + rect.x;
        if (other <= key)
            ++count;
    }
    return count;
}

int64_t point_distance_squared(int32_t x, int32_t y, const PlaceRect& rect) noexcept {
    if (rect.width <= 0 || rect.height <= 0)
        return std::numeric_limits<int64_t>::max();
    const int32_t nearest_x = std::clamp(x, rect.x, rect.x + rect.width - 1);
    const int32_t nearest_y = std::clamp(y, rect.y, rect.y + rect.height - 1);
    const int64_t across = static_cast<int64_t>(x) - static_cast<int64_t>(nearest_x);
    const int64_t down = static_cast<int64_t>(y) - static_cast<int64_t>(nearest_y);
    return across * across + down * down;
}

bool nearer_gadget(
    int32_t x, int32_t y, const PlaceRect& button, std::span<const PlaceRect> gadgets
) noexcept {
    const int64_t to_button = point_distance_squared(x, y, button);
    for (const PlaceRect& gadget : gadgets) {
        if (gadget.width <= 0 || gadget.height <= 0)
            continue;
        const int64_t to_gadget = point_distance_squared(x, y, gadget);
        // Inside a gadget the point is that gadget's, even on the button.
        if (to_gadget == 0 || to_gadget < to_button)
            return true;
    }
    return false;
}

std::optional<PlaceRect> place_beside(
    const PlaceRect& anchor,
    std::span<const PlaceRect> others,
    const PlaceRect& room,
    int32_t gap,
    int32_t height,
    int32_t least_width,
    int32_t most_width
) {
    if (height <= 0 || most_width <= 0)
        return std::nullopt;
    const int32_t x = anchor.x + anchor.width + gap;
    const int32_t y = anchor.y + (anchor.height - height) / 2;
    bool limited = false;
    int32_t first_x = 0;
    for (const PlaceRect& other : others) {
        if (other.width <= 0 || other.height <= 0)
            continue;
        const bool shares = other.y < y + height && y < other.y + other.height;
        if (!shares || other.x <= x)
            continue;
        if (!limited || other.x < first_x) {
            limited = true;
            first_x = other.x;
        }
    }
    int32_t width = most_width;
    if (limited)
        width = first_x - gap - x;
    if (width > most_width)
        width = most_width;
    if (width < least_width)
        return std::nullopt;
    const PlaceRect button{x, y, width, height};
    if (!inside(button, room) || meets_any(button, others))
        return std::nullopt;
    return button;
}

} // namespace oa::ui::frontend_state
