// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Window size choice, the places of the windows that move, and the
// in-match Settings' place.

#include "window_placement.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <stdint.h>
#include <string>
#include <string_view>
#include <system_error>

namespace oa::app::window_placement {

namespace {

namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;

/// The screens that move by their title (moves).
constexpr std::array<std::string_view, 6> moving_windows{
    "settings", "key-editor", "library", "map-browser", "mutator-picker", "brain-picker"
};

/// The largest offset a place key's value may hold on each axis, in points.
constexpr int32_t most_offset_points = 1000000;

/// Returns the percentage a scale multiplies by. 0 or less counts as 100.
///
/// @param scale_percent the scale, in percent
/// @return the percentage, above 0
[[nodiscard]] int32_t percent_of(int32_t scale_percent) noexcept {
    return scale_percent > 0 ? scale_percent : 100;
}

/// Returns value * numerator / denominator to the nearest whole number, a
/// half away from zero. The denominator is above 0.
///
/// @param value the value
/// @param numerator the multiplier
/// @param denominator the divisor, above 0
/// @return the rounded quotient
[[nodiscard]] int32_t nearest(int32_t value, int32_t numerator, int32_t denominator) noexcept {
    const int64_t product = static_cast<int64_t>(value) * numerator;
    const int64_t magnitude = ((product < 0 ? -product : product) * 2 + denominator) /
                              (2 * static_cast<int64_t>(denominator));
    return static_cast<int32_t>(product < 0 ? -magnitude : magnitude);
}

/// Returns a quotient rounded down, for a numerator below 0 too.
///
/// @param numerator the numerator
/// @param denominator the denominator, above 0
/// @return floor(numerator / denominator)
[[nodiscard]] int32_t floor_div(int32_t numerator, int32_t denominator) noexcept {
    const int32_t quotient = numerator / denominator;
    return numerator % denominator < 0 ? quotient - 1 : quotient;
}

/// Reads one signed whole number of an offset.
///
/// @param text the number, optionally signed
/// @return the number; nothing when the text is not one within most_offset_points
[[nodiscard]] std::optional<int32_t> offset_part(std::string_view text) noexcept {
    if (!text.empty() && text.front() == '+')
        text.remove_prefix(1);
    if (text.empty())
        return std::nullopt;
    int32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    if (value > most_offset_points || value < -most_offset_points)
        return std::nullopt;
    return value;
}

/// Keeps one edge of a moved window within its bounds on one axis, unless
/// the unmoved place lies past them: the bounds widen to take it in.
///
/// @param start the window's near edge, in window pixels
/// @param unmoved the near edge of its unmoved place
/// @param least the least the near edge may be
/// @param most the most the near edge may be
/// @return the near edge kept within the bounds
[[nodiscard]] int32_t
kept_between(int32_t start, int32_t unmoved, int32_t least, int32_t most) noexcept {
    const int32_t low = std::min(least, unmoved);
    const int32_t high = std::max(most, unmoved);
    return std::clamp(start, low, std::max(low, high));
}

/// Puts an edge less than a point from a safe edge on it.
///
/// @param start the window's near edge, in window pixels
/// @param length the window's length on this axis
/// @param room_start the room's near edge on this axis
/// @param room_length the room's length on this axis
/// @param scale_percent the scale, in percent, above 0
/// @return the near edge, on a safe edge when it was that close to one
[[nodiscard]] int32_t on_safe_edge(
    int32_t start, int32_t length, int32_t room_start, int32_t room_length, int32_t scale_percent
) noexcept {
    const auto within_a_point = [scale_percent](int32_t pixels) {
        return pixels != 0 && std::abs(int64_t{pixels}) * 100 < scale_percent;
    };
    if (within_a_point(start - room_start))
        return room_start;
    const int32_t far_edge = room_start + room_length;
    if (within_a_point(start + length - far_edge))
        return far_edge - length;
    return start;
}

/// Snaps one axis of a window's place (snapped).
///
/// @param start the window's near edge, in window pixels
/// @param length the window's length on this axis
/// @param unmoved the near edge of its unmoved place
/// @param room_start the room's near edge on this axis
/// @param room_length the room's length on this axis
/// @param snap the snapping distance, in window pixels
/// @return the near edge after snapping
[[nodiscard]] int32_t snap_axis(
    int32_t start,
    int32_t length,
    int32_t unmoved,
    int32_t room_start,
    int32_t room_length,
    int32_t snap
) noexcept {
    if (std::abs(start - unmoved) <= snap)
        return unmoved;
    // Twice the centres, so that an odd length keeps its half pixel.
    const int64_t centre_twice = int64_t{start} * 2 + length;
    const int64_t room_centre_twice = int64_t{room_start} * 2 + room_length;
    if (std::abs(centre_twice - room_centre_twice) <= int64_t{snap} * 2)
        return room_start + floor_div(room_length - length, 2);
    if (std::abs(start - room_start) <= snap)
        return room_start;
    const int32_t far_edge = room_start + room_length;
    if (std::abs(start + length - far_edge) <= snap)
        return far_edge - length;
    return start;
}

} // namespace

WindowSize window_size_of(std::string_view text) noexcept {
    if (text == "compact")
        return WindowSize::compact;
    if (text == "regular")
        return WindowSize::regular;
    if (text == "large")
        return WindowSize::large;
    return WindowSize::automatic;
}

std::string_view window_size_word(WindowSize size) noexcept {
    switch (size) {
    case WindowSize::compact:
        return "compact";
    case WindowSize::regular:
        return "regular";
    case WindowSize::large:
        return "large";
    case WindowSize::automatic:
        break;
    }
    return "auto";
}

WindowSize stored_window_size(const std::map<std::string, std::string>& preferences) {
    const auto found = preferences.find(std::string(window_size_key));
    return found == preferences.end() ? WindowSize::automatic : window_size_of(found->second);
}

kit::SizeClass wanted_class(WindowSize size) noexcept {
    switch (size) {
    case WindowSize::compact:
        return kit::SizeClass::compact;
    case WindowSize::regular:
        return kit::SizeClass::regular;
    case WindowSize::large:
    case WindowSize::automatic:
        break;
    }
    return kit::SizeClass::large;
}

kit::SizeClass class_under(
    WindowSize size, kit::SizeClass room_class, int32_t width_points, int32_t height_points
) noexcept {
    if (size == WindowSize::automatic)
        return room_class;
    return kit::size_class_within(wanted_class(size), width_points, height_points);
}

std::string_view class_word(kit::SizeClass size_class) noexcept {
    switch (size_class) {
    case kit::SizeClass::regular:
        return "regular";
    case kit::SizeClass::large:
        return "large";
    case kit::SizeClass::compact:
        break;
    }
    return "compact";
}

std::string place_key(std::string_view window, kit::SizeClass size_class) {
    std::string key(place_key_start);
    key += window;
    key += '.';
    key += class_word(size_class);
    return key;
}

std::optional<kit::Point> offset_of(std::string_view text) noexcept {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos)
        return std::nullopt;
    const auto across = offset_part(text.substr(0, comma));
    const auto down = offset_part(text.substr(comma + 1));
    if (!across || !down)
        return std::nullopt;
    return kit::Point{*across, *down};
}

std::string offset_text(kit::Point offset) {
    return std::to_string(offset.x) + "," + std::to_string(offset.y);
}

kit::Point stored_offset(
    const std::map<std::string, std::string>& preferences,
    std::string_view window,
    kit::SizeClass size_class
) {
    const auto found = preferences.find(place_key(window, size_class));
    if (found == preferences.end())
        return {};
    return offset_of(found->second).value_or(kit::Point{});
}

bool store_offset(
    std::map<std::string, std::string>& preferences,
    std::string_view window,
    kit::SizeClass size_class,
    kit::Point offset
) {
    const std::string key = place_key(window, size_class);
    if (offset.x == 0 && offset.y == 0)
        return preferences.erase(key) != 0;
    const std::string text = offset_text(offset);
    const auto found = preferences.find(key);
    if (found != preferences.end() && found->second == text)
        return false;
    preferences.insert_or_assign(key, text);
    return true;
}

bool forget_places(std::map<std::string, std::string>& preferences) {
    bool erased = false;
    for (auto entry = preferences.lower_bound(std::string(place_key_start));
         entry != preferences.end() && entry->first.starts_with(place_key_start);) {
        entry = preferences.erase(entry);
        erased = true;
    }
    return erased;
}

bool moves(std::string_view screen_name) noexcept {
    return std::find(moving_windows.begin(), moving_windows.end(), screen_name) !=
           moving_windows.end();
}

int32_t pixels_of(int32_t points, int32_t scale_percent) noexcept {
    return nearest(points, percent_of(scale_percent), 100);
}

int32_t points_of(int32_t pixels, int32_t scale_percent) noexcept {
    return nearest(pixels, 100, percent_of(scale_percent));
}

kit::Rect shown_at(const WindowRoom& room, kit::Point offset) noexcept {
    const kit::Rect& unmoved = room.centred;
    if (offset.x == 0 && offset.y == 0)
        return unmoved;
    const int32_t percent = percent_of(room.scale_percent);
    const int32_t least = pixels_of(least_shown_points, percent);
    const int32_t keep_across = std::min(least, unmoved.width);
    const int32_t keep_down = std::min(least, unmoved.height);
    const kit::Rect& safe = room.room;
    kit::Rect shown = unmoved;
    shown.x = kept_between(
        unmoved.x + pixels_of(offset.x, percent),
        unmoved.x,
        safe.x + keep_across - unmoved.width,
        safe.x + safe.width - keep_across
    );
    // The title, along the top, stays in the room.
    shown.y = kept_between(
        unmoved.y + pixels_of(offset.y, percent),
        unmoved.y,
        safe.y,
        safe.y + safe.height - keep_down
    );
    shown.x = on_safe_edge(shown.x, shown.width, safe.x, safe.width, percent);
    shown.y = on_safe_edge(shown.y, shown.height, safe.y, safe.height, percent);
    return shown;
}

kit::Point offset_at(const WindowRoom& room, const kit::Rect& shown) noexcept {
    const int32_t percent = percent_of(room.scale_percent);
    return {
        points_of(shown.x - room.centred.x, percent),
        points_of(shown.y - room.centred.y, percent),
    };
}

kit::Rect snapped(const WindowRoom& room, const kit::Rect& shown) noexcept {
    const int32_t snap = pixels_of(snap_points, room.scale_percent);
    kit::Rect placed = shown;
    placed.x = snap_axis(shown.x, shown.width, room.centred.x, room.room.x, room.room.width, snap);
    placed.y =
        snap_axis(shown.y, shown.height, room.centred.y, room.room.y, room.room.height, snap);
    return placed;
}

kit::Point
dragged_offset(const WindowRoom& room, kit::Point start_offset, kit::Point travel) noexcept {
    kit::Rect moved = shown_at(room, start_offset);
    moved.x += travel.x;
    moved.y += travel.y;
    const kit::Point offset = offset_at(room, snapped(room, moved));
    // Kept on screen, and read back from where it then shows.
    return offset_at(room, shown_at(room, offset));
}

kit::Point moved_by_key(
    const WindowRoom& room, kit::Point offset, int32_t across, int32_t down, bool large
) noexcept {
    const int32_t step = large ? large_key_step_points : key_step_points;
    const kit::Rect shown = shown_at(room, offset);
    const kit::Point from = offset_at(room, shown);
    const kit::Point to{from.x + across * step, from.y + down * step};
    return offset_at(room, shown_at(room, to));
}

DragStep
drag_move(TitleDrag& drag, const WindowRoom& room, kit::Point at, uint64_t now_ms) noexcept {
    const kit::Point travel{at.x - drag.grab.x, at.y - drag.grab.y};
    if (!drag.moving) {
        if (now_ms >= drag.pressed_ms + finger_hold_ms) {
            drag.moving = true;
        } else {
            const int32_t slop = pixels_of(finger_hold_points, room.scale_percent);
            if (std::abs(travel.x) > slop || std::abs(travel.y) > slop)
                return DragStep::cancelled;
            return DragStep::holds;
        }
    }
    drag.offset = dragged_offset(room, drag.start_offset, travel);
    return DragStep::moves;
}

layout::Rect match_safe_area(const layout::MatchLayout& match_layout) noexcept {
    const auto& safe = match_layout.safe;
    return {
        safe.left,
        safe.top,
        std::max(match_layout.width - safe.left - safe.right, 0),
        std::max(match_layout.height - safe.top - safe.bottom, 0)
    };
}

kit::Viewport match_viewport(
    const layout::MatchLayout& match_layout, int32_t interface_percent, WindowSize size
) noexcept {
    kit::Viewport canvas;
    canvas.width = match_layout.width;
    canvas.height = match_layout.height;
    canvas.density = static_cast<float>(match_layout.px_per_point);
    canvas.safe = {
        match_layout.safe.left,
        match_layout.safe.top,
        match_layout.safe.right,
        match_layout.safe.bottom
    };
    int32_t percent =
        kit::layer_viewport(canvas, interface_percent, wanted_class(size)).scale_percent;
    // A hair under a whole step counts as that step.
    constexpr double whole_step_slack = 1e-9;
    if (static_cast<double>(percent) > match_layout.scale * 100.0 + whole_step_slack)
        percent =
            std::max(
                int32_t{1}, static_cast<int32_t>(std::floor(match_layout.scale + whole_step_slack))
            ) *
            100;
    canvas.scale_percent = percent;
    return canvas;
}

MatchDialogPlace match_window(
    const layout::MatchLayout& match_layout, int32_t interface_percent, WindowSize size, bool fit
) noexcept {
    MatchDialogPlace place{};
    const layout::Rect safe = match_safe_area(match_layout);
    if (fit && safe.width > 0 && safe.height > 0) {
        // Compact at 1x, fitted at the largest scale that shows the whole
        // dialog in the safe area.
        const kit::Metrics& compact = kit::compact_metrics;
        const double scale = std::min(
            static_cast<double>(safe.width) / compact.dialog_width,
            static_cast<double>(safe.height) / compact.dialog_height
        );
        const auto scaled = [scale](int32_t length) {
            return std::max(
                1, static_cast<int32_t>(std::lround(static_cast<double>(length) * scale))
            );
        };
        const int32_t width = std::min(scaled(compact.dialog_width), safe.width);
        const int32_t height = std::min(scaled(compact.dialog_height), safe.height);
        place.shown = {
            safe.x + (safe.width - width) / 2, safe.y + (safe.height - height) / 2, width, height
        };
        place.scale_percent = 0;
        return place;
    }
    // The class from the whole safe area's points at the scale.
    const kit::Viewport canvas = match_viewport(match_layout, interface_percent, size);
    const kit::Frame frame = kit::frame_of(canvas);
    place.scale_percent = canvas.scale_percent;
    place.size_class = class_under(size, frame.size_class, frame.area.width, frame.area.height);
    const kit::Metrics& sized = kit::metrics_of(place.size_class);
    const kit::Rect drawn =
        kit::to_canvas({0, 0, sized.dialog_width, sized.dialog_height}, canvas.scale_percent);
    // Centred in the safe area; clear of the side column where the safe area
    // right of it holds the dialog, centred there across instead.
    int32_t left = safe.x + floor_div(safe.width - drawn.width, 2);
    const int32_t column_right = std::max(match_layout.left, safe.x);
    const int32_t beside = safe.x + safe.width - column_right;
    if (left < column_right && beside >= drawn.width)
        left = column_right + (beside - drawn.width) / 2;
    place.shown = {
        left, safe.y + floor_div(safe.height - drawn.height, 2), drawn.width, drawn.height
    };
    return place;
}

} // namespace oa::app::window_placement
