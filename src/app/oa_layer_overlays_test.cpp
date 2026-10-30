// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The places of the overlays Arrange overlays moves: their keys and values
// in the preferences, kept for each size class, the 8-point grid measured
// from the nearest edge or the centre, a drag, an arrow, the handles, Reset,
// and the overlays' own places at the window sizes the size classes come
// from, none off the overlays' area nor over the side column.

#include "oa_layer_overlays.hpp"

#include "oa/test/check.hpp"
#include "oa/ui/kit/layout.hpp"

#include <array>
#include <map>
#include <optional>
#include <stdint.h>
#include <string>

namespace {

namespace kit = oa::ui::kit;
namespace places = oa::app::overlay_placement;
using places::Anchor;
using places::AxisPlace;
using places::OverlayPlace;

/// Tells whether a rectangle is the one named by its edges' place and size.
///
/// @param rect the rectangle
/// @param x its left column
/// @param y its top row
/// @param width its columns
/// @param height its rows
/// @return true when every field matches
bool rect_is(kit::Rect rect, int32_t x, int32_t y, int32_t width, int32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

/// Tells whether a rectangle lies wholly in another.
///
/// @param room the outer rectangle
/// @param rect the inner one
/// @return true when every edge of rect is inside room
bool inside(kit::Rect room, kit::Rect rect) {
    return rect.x >= room.x && rect.y >= room.y && rect.x + rect.width <= room.x + room.width &&
           rect.y + rect.height <= room.y + room.height;
}

/// Tells whether a place is the one named.
///
/// @param place the place
/// @param across the anchor across
/// @param x the distance across
/// @param down the anchor down
/// @param y the distance down
/// @return true when both axes match
bool place_is(OverlayPlace place, Anchor across, int32_t x, Anchor down, int32_t y) {
    return place.across == AxisPlace{across, x} && place.down == AxisPlace{down, y};
}

/// Checks each overlay's key, a place's text both ways, and the values that
/// are refused.
void check_keys_and_text() {
    OA_CHECK(places::arranged("keys-overlay"));
    OA_CHECK(places::arranged("chat-line"));
    OA_CHECK(places::arranged("latch-chips"));
    OA_CHECK(!places::arranged("settings"));
    OA_CHECK(
        places::place_key("keys-overlay", kit::SizeClass::large) ==
        "open-annihilation.overlay.keys-overlay.large"
    );
    OA_CHECK(
        places::place_key("chat-line", kit::SizeClass::compact) ==
        "open-annihilation.overlay.chat-line.compact"
    );
    OA_CHECK(places::place_text({{Anchor::start, 16}, {Anchor::end, 8}}) == "l16,b8");
    OA_CHECK(places::place_text({{Anchor::centre, -24}, {Anchor::start, 0}}) == "c-24,t0");
    OA_CHECK(places::place_text({{Anchor::end, 40}, {Anchor::centre, 8}}) == "r40,c8");
    const auto read = places::place_of("r40,c8");
    OA_CHECK(read && place_is(*read, Anchor::end, 40, Anchor::centre, 8));
    const auto signed_place = places::place_of("c-24,t0");
    OA_CHECK(signed_place && place_is(*signed_place, Anchor::centre, -24, Anchor::start, 0));
    for (const char* refused :
         {"", "l16", "16,8", "t16,l8", "x8,t8", "l8,b", "l8 ,t8", "l8,t8,", "l1000001,t0", "l8,tq"})
        OA_CHECK(!places::place_of(refused));
}

/// Checks that each size class keeps a place of its own, that storing the
/// same place again changes nothing, and that forgetting and Reset erase
/// only the overlays' keys.
void check_places_per_class_and_reset() {
    std::map<std::string, std::string> preferences{
        {"open-annihilation.window.settings.large", "8,8"},
        {"open-annihilation.ui.window-size", "large"},
    };
    const OverlayPlace compact{{Anchor::start, 16}, {Anchor::start, 8}};
    const OverlayPlace large{{Anchor::end, 32}, {Anchor::end, 48}};
    OA_CHECK(!places::stored_place(preferences, "keys-overlay", kit::SizeClass::compact));
    OA_CHECK(places::store_place(preferences, "keys-overlay", kit::SizeClass::compact, compact));
    OA_CHECK(!places::store_place(preferences, "keys-overlay", kit::SizeClass::compact, compact));
    OA_CHECK(places::store_place(preferences, "keys-overlay", kit::SizeClass::large, large));
    OA_CHECK(places::store_place(preferences, "chat-line", kit::SizeClass::large, compact));
    OA_CHECK(preferences.at("open-annihilation.overlay.keys-overlay.compact") == "l16,t8");
    OA_CHECK(preferences.at("open-annihilation.overlay.keys-overlay.large") == "r32,b48");
    const auto at_compact =
        places::stored_place(preferences, "keys-overlay", kit::SizeClass::compact);
    const auto at_large = places::stored_place(preferences, "keys-overlay", kit::SizeClass::large);
    OA_CHECK(at_compact && *at_compact == compact);
    OA_CHECK(at_large && *at_large == large);
    OA_CHECK(!places::stored_place(preferences, "keys-overlay", kit::SizeClass::regular));
    // A value that cannot be read is no place.
    preferences["open-annihilation.overlay.latch-chips.regular"] = "middle";
    OA_CHECK(!places::stored_place(preferences, "latch-chips", kit::SizeClass::regular));
    // One overlay at one class forgotten.
    OA_CHECK(places::forget_place(preferences, "keys-overlay", kit::SizeClass::compact));
    OA_CHECK(!places::forget_place(preferences, "keys-overlay", kit::SizeClass::compact));
    OA_CHECK(places::stored_place(preferences, "keys-overlay", kit::SizeClass::large));
    // Reset forgets every overlay at every class, and nothing else.
    OA_CHECK(places::forget_places(preferences));
    OA_CHECK(!places::forget_places(preferences));
    OA_CHECK(preferences.size() == 2);
    OA_CHECK(preferences.contains("open-annihilation.window.settings.large"));
    OA_CHECK(preferences.contains("open-annihilation.ui.window-size"));
}

/// Checks where an overlay shows at a place: from either edge or the
/// centre, and kept in the area whatever the place says.
void check_rect_at() {
    const kit::Rect room{0, 0, 640, 400};
    const kit::Point size{200, 100};
    OA_CHECK(rect_is(
        places::rect_at(room, size, {{Anchor::start, 16}, {Anchor::start, 8}}), 16, 8, 200, 100
    ));
    OA_CHECK(rect_is(
        places::rect_at(room, size, {{Anchor::end, 16}, {Anchor::end, 8}}), 424, 292, 200, 100
    ));
    OA_CHECK(rect_is(
        places::rect_at(room, size, {{Anchor::centre, 0}, {Anchor::centre, -40}}),
        220,
        110,
        200,
        100
    ));
    // A room that does not start at 0, 0.
    OA_CHECK(rect_is(
        places::rect_at({128, 32, 512, 416}, {64, 24}, {{Anchor::end, 0}, {Anchor::start, 0}}),
        576,
        32,
        64,
        24
    ));
    // Never off the area: past any edge it is kept in, and one larger than
    // the area keeps to its near edge.
    OA_CHECK(rect_is(
        places::rect_at(room, size, {{Anchor::start, 900}, {Anchor::end, 900}}), 440, 0, 200, 100
    ));
    OA_CHECK(rect_is(
        places::rect_at(room, size, {{Anchor::centre, -9000}, {Anchor::centre, 9000}}),
        0,
        300,
        200,
        100
    ));
    OA_CHECK(rect_is(
        places::rect_at(room, {700, 500}, {{Anchor::end, 0}, {Anchor::centre, 0}}), 0, 0, 700, 500
    ));
    for (const int32_t far : {-1000000, -8, 0, 8, 1000000})
        for (const Anchor anchor : {Anchor::start, Anchor::centre, Anchor::end})
            OA_CHECK(inside(room, places::rect_at(room, size, {{anchor, far}, {anchor, -far}})));
}

/// Checks the place of an overlay at a rectangle: the anchor by the third
/// its centre lies in, and the distance on the 8-point grid.
void check_place_at_and_grid() {
    const kit::Rect room{0, 0, 640, 480};
    // The first third from the near edge, the last from the far one, the
    // middle from the centre.
    OA_CHECK(
        place_is(places::place_at(room, {13, 5, 100, 60}), Anchor::start, 16, Anchor::start, 8)
    );
    OA_CHECK(
        place_is(places::place_at(room, {527, 413, 100, 60}), Anchor::end, 16, Anchor::end, 8)
    );
    OA_CHECK(
        place_is(places::place_at(room, {277, 213, 100, 60}), Anchor::centre, 8, Anchor::centre, 0)
    );
    // A half rounds away from 0; 3 points from an edge lies on it.
    OA_CHECK((places::place_at(room, {12, 3, 100, 60}).across == AxisPlace{Anchor::start, 16}));
    OA_CHECK((places::place_at(room, {12, 3, 100, 60}).down == AxisPlace{Anchor::start, 0}));
    OA_CHECK((places::place_at(room, {266, 0, 100, 60}).across == AxisPlace{Anchor::centre, -8}));
    // Every distance lies on the grid, and the rectangle at it in the room.
    for (int32_t x = -20; x < 700; x += 7)
        for (int32_t y = -20; y < 520; y += 11) {
            const kit::Rect shown = places::kept_in(room, {x, y, 100, 60});
            const OverlayPlace place = places::place_at(room, shown);
            OA_CHECK(place.across.points % places::grid_points == 0);
            OA_CHECK(place.down.points % places::grid_points == 0);
            const kit::Rect again = places::rect_at(room, {100, 60}, place);
            OA_CHECK(inside(room, again));
            OA_CHECK(again.x - shown.x <= 4 && shown.x - again.x <= 4);
            OA_CHECK(again.y - shown.y <= 4 && shown.y - again.y <= 4);
        }
    // An overlay as wide as the area takes the near edge's 0 across.
    OA_CHECK((places::place_at(room, {0, 400, 640, 40}).across == AxisPlace{Anchor::start, 0}));
    OA_CHECK((places::place_at(room, {0, 400, 640, 40}).down == AxisPlace{Anchor::end, 40}));
}

/// Checks the place a drag and an arrow give, and that a place keeps to its
/// edge at another window size of the same class.
void check_drag_and_arrows() {
    const kit::Rect room{0, 0, 640, 480};
    const kit::Rect start{8, 8, 120, 40};
    // A drag on the grid: 37 points right and 5 down lands at 48, 16.
    const OverlayPlace dragged = places::dragged_place(room, start, {37, 5});
    OA_CHECK(place_is(dragged, Anchor::start, 48, Anchor::start, 16));
    // Dragged past the far corner it lies flush in it.
    const OverlayPlace corner = places::dragged_place(room, start, {2000, 2000});
    OA_CHECK(place_is(corner, Anchor::end, 0, Anchor::end, 0));
    OA_CHECK(rect_is(places::rect_at(room, {120, 40}, corner), 520, 440, 120, 40));
    // The same place in a larger area of the same class keeps to the corner.
    OA_CHECK(rect_is(places::rect_at({0, 0, 1152, 648}, {120, 40}, corner), 1032, 608, 120, 40));
    // Arrows: a grid step, 32 with Shift, and no further than the area.
    const kit::Rect at{48, 16, 120, 40};
    OA_CHECK((places::moved_place(room, at, 1, 0, false).across == AxisPlace{Anchor::start, 56}));
    OA_CHECK((places::moved_place(room, at, 0, 1, true).down == AxisPlace{Anchor::start, 48}));
    OA_CHECK((places::moved_place(room, at, -1, 0, true).across == AxisPlace{Anchor::start, 16}));
    OA_CHECK(
        places::moved_place(room, {0, 0, 120, 40}, -1, -1, false) ==
        (OverlayPlace{{Anchor::start, 0}, {Anchor::start, 0}})
    );
    OA_CHECK(
        places::moved_place(room, {520, 440, 120, 40}, 1, 1, true) ==
        (OverlayPlace{{Anchor::end, 0}, {Anchor::end, 0}})
    );
}

/// Checks a handle's growth round a small overlay, from the edge or centre
/// its third gives, and that it keeps the overlay's place.
void check_handles() {
    const kit::Rect room{0, 0, 640, 480};
    // Near the top left: it grows right and down.
    OA_CHECK(rect_is(places::handle_rect(room, {8, 8, 40, 10}, {96, 28}), 8, 8, 96, 28));
    // At the bottom right: it grows left and up.
    OA_CHECK(rect_is(places::handle_rect(room, {592, 462, 40, 10}, {96, 28}), 536, 444, 96, 28));
    // In the middle: round its centre.
    OA_CHECK(rect_is(places::handle_rect(room, {300, 235, 40, 10}, {96, 28}), 272, 226, 96, 28));
    // A larger overlay is its own handle.
    OA_CHECK(rect_is(places::handle_rect(room, {8, 8, 300, 200}, {96, 28}), 8, 8, 300, 200));
    // The handle's place, on the grid, is the overlay's.
    const kit::Rect overlay{16, 432, 40, 16};
    const kit::Rect handle = places::handle_rect(room, overlay, {96, 28});
    const OverlayPlace place = places::place_at(room, handle);
    OA_CHECK(places::place_at(room, overlay) == place);
    OA_CHECK(rect_is(places::rect_at(room, {40, 16}, place), 16, 432, 40, 16));
}

/// Checks where each overlay shows with no place and with one, in the
/// overlays' area of each window size the size classes come from: right of
/// a 128-point side column and under the top bar. With no place it shows
/// where it lays itself out; none of them lies off the area or over the
/// column, whatever its place.
void check_default_places() {
    struct Window {
        kit::Rect battlefield{}; ///< the overlays' area, in points
        kit::SizeClass size_class{};
    };

    // 640 by 480 at Compact, 1280 by 720 at Regular's room and above, 1920 by
    // 1080 at Large: the battlefield right of the column, under the top bar
    // and over the bottom bar.
    const std::array<Window, 3> windows{{
        {{128, 32, 512, 416}, kit::SizeClass::compact},
        {{128, 32, 1152, 656}, kit::SizeClass::large},
        {{128, 32, 1792, 1016}, kit::SizeClass::large},
    }};
    const int32_t column_right = 128;
    for (const Window& window : windows) {
        const kit::Rect& room = window.battlefield;
        // The overlays as they lay themselves out: the keys overlay 8 points
        // right of the column and under the top, the chat line across the
        // bottom, the latch chips centred 6 points under the top.
        const std::array<kit::Rect, 3> laid{{
            {room.x + 8, room.y + 8, std::min(520, room.width - 16), room.height - 16},
            {room.x, room.y + room.height - 20, room.width, 20},
            {room.x + (room.width - 96) / 2, room.y + 6, 96, 16},
        }};
        for (const kit::Rect& unplaced : laid) {
            const kit::Rect shown = places::shown_rect(room, unplaced, std::nullopt);
            OA_CHECK(rect_is(shown, unplaced.x, unplaced.y, unplaced.width, unplaced.height));
            OA_CHECK(inside(room, shown));
            OA_CHECK(shown.x >= column_right);
            for (const Anchor anchor : {Anchor::start, Anchor::centre, Anchor::end}) {
                const OverlayPlace far{{anchor, -100000}, {anchor, 100000}};
                const kit::Rect placed = places::shown_rect(room, unplaced, far);
                OA_CHECK(inside(room, placed));
                OA_CHECK(placed.x >= column_right);
            }
        }
    }
}

} // namespace

int main() {
    check_keys_and_text();
    check_places_per_class_and_reset();
    check_rect_at();
    check_place_at_and_grid();
    check_drag_and_arrows();
    check_handles();
    check_default_places();
    return oa::test::check_exit_status();
}
