// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The places of the overlays Arrange overlays moves: their keys and values
// in the preferences, and where an overlay shows at a place.

#include "oa_layer_overlays.hpp"

#include "window_placement.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace oa::app::overlay_placement {

namespace {

namespace kit = oa::ui::kit;

/// Returns a quotient rounded down, for a numerator below 0 too.
///
/// @param numerator the numerator
/// @param denominator the denominator, above 0
/// @return floor(numerator / denominator)
[[nodiscard]] int64_t floor_div(int64_t numerator, int64_t denominator) noexcept {
    const int64_t quotient = numerator / denominator;
    return numerator % denominator < 0 ? quotient - 1 : quotient;
}

/// Returns a distance rounded to the nearest multiple of grid_points, a half
/// away from zero.
///
/// @param points the distance, in points
/// @return the multiple
[[nodiscard]] int64_t on_grid(int64_t points) noexcept {
    const int64_t magnitude = (points < 0 ? -points : points) + grid_points / 2;
    const int64_t rounded = magnitude / grid_points * grid_points;
    return points < 0 ? -rounded : rounded;
}

/// Returns a distance held between two bounds on the grid: the nearest
/// multiple of grid_points within them, or 0 when none is.
///
/// @param points the distance, in points
/// @param least the least it may be
/// @param most the most it may be
/// @return the distance, a multiple of grid_points
[[nodiscard]] int64_t held_on_grid(int64_t points, int64_t least, int64_t most) noexcept {
    const int64_t low = -floor_div(-least, grid_points) * grid_points;
    const int64_t high = floor_div(most, grid_points) * grid_points;
    if (high < low)
        return 0;
    return std::clamp(on_grid(points), low, high);
}

/// Returns the near edge of an overlay on one axis at a place.
///
/// @param place the place on the axis
/// @param room_start the room's near edge
/// @param room_length the room's length
/// @param length the overlay's length
/// @return the overlay's near edge, before it is kept in the room
[[nodiscard]] int64_t
near_edge_at(const AxisPlace& place, int64_t room_start, int64_t room_length, int64_t length) {
    switch (place.anchor) {
    case Anchor::end:
        return room_start + room_length - length - place.points;
    case Anchor::centre:
        return room_start + floor_div(room_length - length, 2) + place.points;
    case Anchor::start:
        break;
    }
    return room_start + place.points;
}

/// Returns an overlay's place on one axis (place_at).
///
/// @param start the overlay's near edge
/// @param length its length
/// @param room_start the room's near edge
/// @param room_length the room's length
/// @return the place on the axis
[[nodiscard]] AxisPlace
axis_place_at(int64_t start, int64_t length, int64_t room_start, int64_t room_length) noexcept {
    const int64_t spare = room_length - length;
    if (spare <= 0)
        return {Anchor::start, 0};
    const int64_t from_start = start - room_start;
    // Thrice the centre against the room's thirds, so that no third is rounded.
    const int64_t centre_thrice = (2 * from_start + length) * 3;
    if (centre_thrice < 2 * room_length)
        return {Anchor::start, static_cast<int32_t>(held_on_grid(from_start, 0, spare))};
    if (centre_thrice > 4 * room_length)
        return {Anchor::end, static_cast<int32_t>(held_on_grid(spare - from_start, 0, spare))};
    const int64_t centred = floor_div(spare, 2);
    return {
        Anchor::centre,
        static_cast<int32_t>(held_on_grid(from_start - centred, -centred, spare - centred))
    };
}

/// Reads one axis of a place's text.
///
/// @param text the axis: its anchor's letter and a signed whole number
/// @param start the letter of the near edge: l across, t down
/// @param end the letter of the far edge: r across, b down
/// @return the axis's place; nothing for any other text
[[nodiscard]] std::optional<AxisPlace>
axis_of(std::string_view text, char start, char end) noexcept {
    if (text.size() < 2)
        return std::nullopt;
    AxisPlace place;
    const char letter = text.front();
    if (letter == start)
        place.anchor = Anchor::start;
    else if (letter == 'c')
        place.anchor = Anchor::centre;
    else if (letter == end)
        place.anchor = Anchor::end;
    else
        return std::nullopt;
    text.remove_prefix(1);
    int32_t value = 0;
    const auto [stop, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || stop != text.data() + text.size())
        return std::nullopt;
    if (value > most_place_points || value < -most_place_points)
        return std::nullopt;
    place.points = value;
    return place;
}

/// Writes one axis of a place.
///
/// @param place the axis's place
/// @param start the letter of the near edge
/// @param end the letter of the far edge
/// @return the letter and the distance
[[nodiscard]] std::string axis_text(const AxisPlace& place, char start, char end) {
    char letter = start;
    if (place.anchor == Anchor::centre)
        letter = 'c';
    else if (place.anchor == Anchor::end)
        letter = end;
    return letter + std::to_string(place.points);
}

} // namespace

bool arranged(std::string_view overlay) noexcept {
    return std::find(overlays.begin(), overlays.end(), overlay) != overlays.end();
}

std::string place_key(std::string_view overlay, kit::SizeClass size_class) {
    std::string key(place_key_start);
    key += overlay;
    key += '.';
    key += window_placement::class_word(size_class);
    return key;
}

std::string place_text(const OverlayPlace& place) {
    return axis_text(place.across, 'l', 'r') + ',' + axis_text(place.down, 't', 'b');
}

std::optional<OverlayPlace> place_of(std::string_view text) noexcept {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos)
        return std::nullopt;
    const auto across = axis_of(text.substr(0, comma), 'l', 'r');
    const auto down = axis_of(text.substr(comma + 1), 't', 'b');
    if (!across || !down)
        return std::nullopt;
    return OverlayPlace{*across, *down};
}

std::optional<OverlayPlace> stored_place(
    const std::map<std::string, std::string>& preferences,
    std::string_view overlay,
    kit::SizeClass size_class
) {
    const auto found = preferences.find(place_key(overlay, size_class));
    if (found == preferences.end())
        return std::nullopt;
    return place_of(found->second);
}

bool store_place(
    std::map<std::string, std::string>& preferences,
    std::string_view overlay,
    kit::SizeClass size_class,
    const OverlayPlace& place
) {
    const std::string key = place_key(overlay, size_class);
    const std::string text = place_text(place);
    const auto found = preferences.find(key);
    if (found != preferences.end() && found->second == text)
        return false;
    preferences.insert_or_assign(key, text);
    return true;
}

bool forget_place(
    std::map<std::string, std::string>& preferences,
    std::string_view overlay,
    kit::SizeClass size_class
) {
    return preferences.erase(place_key(overlay, size_class)) != 0;
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

kit::Rect kept_in(const kit::Rect& room, const kit::Rect& rect) noexcept {
    const auto axis = [](int32_t start, int32_t length, int32_t room_start, int32_t room_length) {
        const int32_t last = room_start + std::max(room_length - length, 0);
        return std::clamp(start, room_start, last);
    };
    return {
        axis(rect.x, rect.width, room.x, room.width),
        axis(rect.y, rect.height, room.y, room.height),
        rect.width,
        rect.height
    };
}

kit::Rect rect_at(const kit::Rect& room, kit::Point size, const OverlayPlace& place) noexcept {
    const int64_t x = near_edge_at(place.across, room.x, room.width, size.x);
    const int64_t y = near_edge_at(place.down, room.y, room.height, size.y);
    const auto bounded = [](int64_t value) {
        return static_cast<int32_t>(std::clamp<int64_t>(
            value, -2 * int64_t{most_place_points}, 2 * int64_t{most_place_points}
        ));
    };
    return kept_in(room, {bounded(x), bounded(y), size.x, size.y});
}

OverlayPlace place_at(const kit::Rect& room, const kit::Rect& shown) noexcept {
    return {
        axis_place_at(shown.x, shown.width, room.x, room.width),
        axis_place_at(shown.y, shown.height, room.y, room.height)
    };
}

kit::Rect shown_rect(
    const kit::Rect& room, const kit::Rect& unplaced, const std::optional<OverlayPlace>& place
) noexcept {
    if (!place)
        return kept_in(room, unplaced);
    return rect_at(room, {unplaced.width, unplaced.height}, *place);
}

kit::Rect handle_rect(const kit::Rect& room, const kit::Rect& shown, kit::Point least) noexcept {
    const auto axis =
        [](int32_t start, int32_t length, int32_t wanted, int32_t room_start, int32_t room_length) {
            if (length >= wanted)
                return start;
            const int64_t centre_thrice = (2 * int64_t{start - room_start} + length) * 3;
            if (centre_thrice < 2 * int64_t{room_length})
                return start;
            if (centre_thrice > 4 * int64_t{room_length})
                return start + length - wanted;
            return start + static_cast<int32_t>(floor_div(int64_t{length} - wanted, 2));
        };
    const int32_t width = std::max(shown.width, least.x);
    const int32_t height = std::max(shown.height, least.y);
    return kept_in(
        room,
        {axis(shown.x, shown.width, width, room.x, room.width),
         axis(shown.y, shown.height, height, room.y, room.height),
         width,
         height}
    );
}

OverlayPlace
dragged_place(const kit::Rect& room, const kit::Rect& start, kit::Point travel) noexcept {
    const kit::Rect moved{start.x + travel.x, start.y + travel.y, start.width, start.height};
    return place_at(room, kept_in(room, moved));
}

OverlayPlace moved_place(
    const kit::Rect& room, const kit::Rect& shown, int32_t across, int32_t down, bool large
) noexcept {
    const int32_t step = large ? large_step_points : grid_points;
    const kit::Rect moved{
        shown.x + across * step, shown.y + down * step, shown.width, shown.height
    };
    return place_at(room, kept_in(room, moved));
}

} // namespace oa::app::overlay_placement
