// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where Open Annihilation's overlays over a running match show once the
// player has arranged them (Arrange overlays, oa_layer_arrange.cpp): the
// keys overlay, the chat line that stands over the battlefield and the latch
// chips. Each overlay keeps one place for each size class in the
// preferences, measured from the nearest edge of the overlays' area or its
// centre on an 8-point grid, so that the place holds at another window size
// of the same class. An overlay with no place shows where it lays itself
// out. Nothing here reads the window or the runtime: the OA layer
// (oa_layer.cpp) passes what it needs and keeps the preferences.
#pragma once

#include "oa/ui/kit/layout.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace oa::app::overlay_placement {

/// The keys overlay's name (oa::ui::key_editor::name_keys_overlay).
inline constexpr std::string_view keys_overlay = "keys-overlay";
/// The chat line's name: the line being typed where it stands over the
/// battlefield (Runtime::draw_risen_chat_line).
inline constexpr std::string_view chat_line = "chat-line";
/// The latch chips' name: the latches and sticky modifiers that are on
/// (Runtime::draw_key_latch_chips).
inline constexpr std::string_view latch_chips = "latch-chips";

/// The overlays Arrange overlays moves, in the order it lists them.
inline constexpr std::array<std::string_view, 3> overlays{keys_overlay, chat_line, latch_chips};

/// The start of each overlay's place in the preferences. The key is this,
/// the overlay's name, a dot and its size class's word
/// (open-annihilation.overlay.keys-overlay.large); the value is the place
/// as place_text writes it. Absent, the overlay shows where it lays itself
/// out. Reset window positions does not touch these keys; Arrange
/// overlays' Reset forgets them.
inline constexpr std::string_view place_key_start = "open-annihilation.overlay.";

/// The grid an overlay's place keeps to, in points.
inline constexpr int32_t grid_points = 8;

/// The points an arrow moves the focused overlay, with Shift held.
inline constexpr int32_t large_step_points = 32;

/// The largest distance a place's value may hold on each axis, in points.
inline constexpr int32_t most_place_points = 1000000;

/// What an overlay's place is measured from on one axis.
enum class Anchor : uint8_t {
    start,  ///< the area's left or top edge, to the overlay's left or top edge
    centre, ///< the area's centre, to the overlay's centre; below 0 left or up
    end,    ///< the area's right or bottom edge, to the overlay's right or bottom edge
};

/// An overlay's place on one axis.
struct AxisPlace {
    Anchor anchor{Anchor::start}; ///< what it is measured from
    int32_t points{};             ///< how far from it, in points; a multiple of grid_points
    /// Tells whether two places are the same.
    ///
    /// @return true when the anchor and the distance are the same
    bool operator==(const AxisPlace&) const = default;
};

/// An overlay's place, across and down.
struct OverlayPlace {
    AxisPlace across{}; ///< the place across
    AxisPlace down{};   ///< the place down
    /// Tells whether two places are the same.
    ///
    /// @return true when both axes are the same
    bool operator==(const OverlayPlace&) const = default;
};

/// Tells whether a name is one of the overlays Arrange overlays moves.
///
/// @param overlay the name
/// @return true for keys-overlay, chat-line and latch-chips
[[nodiscard]] bool arranged(std::string_view overlay) noexcept;

/// Returns the preference key of an overlay's place at a size class.
///
/// @param overlay the overlay's name
/// @param size_class the class its places are kept for
/// @return place_key_start, the name, a dot and the class's word
[[nodiscard]] std::string place_key(std::string_view overlay, oa::ui::kit::SizeClass size_class);

/// Writes a place as a place key's value holds it: a letter for each
/// axis's anchor (l, c or r across; t, c or b down) and its distance, the
/// two with a comma between, such as "l16,b8" or "c-24,t0".
///
/// @param place the place
/// @return the text
[[nodiscard]] std::string place_text(const OverlayPlace& place);

/// Reads a place as a place key's value holds it (place_text).
///
/// @param text the value
/// @return the place; nothing for any other text, or a distance beyond
///     most_place_points
[[nodiscard]] std::optional<OverlayPlace> place_of(std::string_view text) noexcept;

/// Returns an overlay's place at a size class.
///
/// @param preferences the preferences' keys and values
/// @param overlay the overlay's name
/// @param size_class the class its places are kept for
/// @return the place; nothing when none is kept or its text cannot be read
[[nodiscard]] std::optional<OverlayPlace> stored_place(
    const std::map<std::string, std::string>& preferences,
    std::string_view overlay,
    oa::ui::kit::SizeClass size_class
);

/// Keeps an overlay's place at a size class.
///
/// @param[in,out] preferences the preferences' keys and values
/// @param overlay the overlay's name
/// @param size_class the class its places are kept for
/// @param place the place
/// @return true when the preferences changed
bool store_place(
    std::map<std::string, std::string>& preferences,
    std::string_view overlay,
    oa::ui::kit::SizeClass size_class,
    const OverlayPlace& place
);

/// Forgets an overlay's place at a size class, so that it shows where it
/// lays itself out there again.
///
/// @param[in,out] preferences the preferences' keys and values
/// @param overlay the overlay's name
/// @param size_class the class
/// @return true when a place was forgotten
bool forget_place(
    std::map<std::string, std::string>& preferences,
    std::string_view overlay,
    oa::ui::kit::SizeClass size_class
);

/// Forgets every overlay's place at every size class: every key that starts
/// with place_key_start, and no other.
///
/// @param[in,out] preferences the preferences' keys and values
/// @return true when a key was erased
bool forget_places(std::map<std::string, std::string>& preferences);

/// Returns a rectangle moved into a room: as far as it lies outside on each
/// axis, back in; one larger than the room on an axis at the room's near
/// edge. Its size is kept.
///
/// @param room the room
/// @param rect the rectangle
/// @return the rectangle inside the room
[[nodiscard]] oa::ui::kit::Rect
kept_in(const oa::ui::kit::Rect& room, const oa::ui::kit::Rect& rect) noexcept;

/// Returns where an overlay of a size shows at a place in a room: on each
/// axis its near edge the distance from the room's near edge (start), its
/// far edge the distance from the room's far edge (end), or its centre the
/// distance from the room's centre, its near edge rounded down (centre);
/// then kept in the room (kept_in).
///
/// @param room the overlays' area, in points
/// @param size the overlay's width and height, in points
/// @param place the place
/// @return the overlay's rectangle, in points
[[nodiscard]] oa::ui::kit::Rect
rect_at(const oa::ui::kit::Rect& room, oa::ui::kit::Point size, const OverlayPlace& place) noexcept;

/// Returns the place of an overlay that shows at a rectangle, on the grid:
/// on each axis measured from the near edge when the overlay's centre lies
/// in the room's first third, from the far edge in its last third and from
/// the centre between, the distance rounded to the nearest multiple of
/// grid_points (a half away from zero) and held to what the room allows,
/// so that rect_at of the place lies in the room. An overlay as large as
/// the room on an axis, or larger, takes the near edge's 0 there.
///
/// @param room the overlays' area, in points
/// @param shown where the overlay shows, in points
/// @return the place
[[nodiscard]] OverlayPlace
place_at(const oa::ui::kit::Rect& room, const oa::ui::kit::Rect& shown) noexcept;

/// Returns where an overlay shows: at its place when it has one (rect_at,
/// with its own size), else where it lays itself out, kept in the room.
///
/// @param room the overlays' area, in points
/// @param unplaced where the overlay lays itself out, in points
/// @param place its place; nothing for none
/// @return its rectangle, in points
[[nodiscard]] oa::ui::kit::Rect shown_rect(
    const oa::ui::kit::Rect& room,
    const oa::ui::kit::Rect& unplaced,
    const std::optional<OverlayPlace>& place
) noexcept;

/// Returns a handle on an overlay at least a size: on each axis where the
/// overlay is smaller, the handle grows from its near edge when its centre
/// lies in the room's first third, from its far edge in the last third and
/// from its centre between, as place_at measures it; then kept in the room.
/// A handle so grown has the place its overlay has.
///
/// @param room the overlays' area, in points
/// @param shown where the overlay shows, in points
/// @param least the handle's least width and height, in points
/// @return the handle's rectangle, in points
[[nodiscard]] oa::ui::kit::Rect handle_rect(
    const oa::ui::kit::Rect& room, const oa::ui::kit::Rect& shown, oa::ui::kit::Point least
) noexcept;

/// Returns the place a drag gives an overlay: where it showed when pressed,
/// moved by the pointer's travel and kept in the room, on the grid
/// (place_at).
///
/// @param room the overlays' area, in points
/// @param start where the overlay showed when the press landed, in points
/// @param travel how far the pointer moved since, in points
/// @return the place
[[nodiscard]] OverlayPlace dragged_place(
    const oa::ui::kit::Rect& room, const oa::ui::kit::Rect& start, oa::ui::kit::Point travel
) noexcept;

/// Returns the place an arrow gives an overlay: grid_points, or
/// large_step_points, in the arrow's direction from where it shows, kept in
/// the room, on the grid.
///
/// @param room the overlays' area, in points
/// @param shown where the overlay shows, in points
/// @param across -1 for left, 1 for right, 0 for neither
/// @param down -1 for up, 1 for down, 0 for neither
/// @param large Shift is held: the larger step
/// @return the place
[[nodiscard]] OverlayPlace moved_place(
    const oa::ui::kit::Rect& room,
    const oa::ui::kit::Rect& shown,
    int32_t across,
    int32_t down,
    bool large
) noexcept;

} // namespace oa::app::overlay_placement
