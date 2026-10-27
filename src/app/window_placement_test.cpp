// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Window size choice, the places of the windows that move (their keys
// and values in the preferences, keeping on screen, snapping, a drag, a
// finger's hold and a key's move), and the in-match Settings measured on
// the whole window, against the resolution table.

#include "window_placement.hpp"

#include "oa/test/check.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/kit/layout.hpp"

#include <map>
#include <stdint.h>
#include <string>

namespace {

namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;
namespace places = oa::app::window_placement;
using places::WindowSize;

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

/// Tells whether a match rectangle is the one named by its edges' place and size.
///
/// @param rect the rectangle
/// @param x its left column
/// @param y its top row
/// @param width its columns
/// @param height its rows
/// @return true when every field matches
bool rect_is(layout::Rect rect, int32_t x, int32_t y, int32_t width, int32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

/// Tells whether a point is the one named.
///
/// @param point the point
/// @param x its column
/// @param y its row
/// @return true when both match
bool point_is(kit::Point point, int32_t x, int32_t y) {
    return point.x == x && point.y == y;
}

/// Checks the Window size's words, the class Auto serves for each, and the
/// class a room takes under each.
void the_window_size_overrides_the_class() {
    OA_CHECK(places::window_size_of("compact") == WindowSize::compact);
    OA_CHECK(places::window_size_of("regular") == WindowSize::regular);
    OA_CHECK(places::window_size_of("large") == WindowSize::large);
    OA_CHECK(places::window_size_of("auto") == WindowSize::automatic);
    OA_CHECK(places::window_size_of("Large") == WindowSize::automatic);
    OA_CHECK(places::window_size_of("") == WindowSize::automatic);
    for (const WindowSize size :
         {WindowSize::automatic, WindowSize::compact, WindowSize::regular, WindowSize::large})
        OA_CHECK(places::window_size_of(places::window_size_word(size)) == size);
    OA_CHECK(places::window_size_word(WindowSize::automatic) == "auto");
    OA_CHECK(places::window_size_key == "open-annihilation.ui.window-size");

    std::map<std::string, std::string> preferences;
    OA_CHECK(places::stored_window_size(preferences) == WindowSize::automatic);
    preferences["open-annihilation.ui.window-size"] = "regular";
    OA_CHECK(places::stored_window_size(preferences) == WindowSize::regular);

    OA_CHECK(places::wanted_class(WindowSize::automatic) == kit::SizeClass::large);
    OA_CHECK(places::wanted_class(WindowSize::compact) == kit::SizeClass::compact);
    OA_CHECK(places::wanted_class(WindowSize::regular) == kit::SizeClass::regular);
    OA_CHECK(places::wanted_class(WindowSize::large) == kit::SizeClass::large);

    // Auto keeps the room's class; a chosen class shows where its dialog fits.
    using kit::SizeClass;
    OA_CHECK(
        places::class_under(WindowSize::automatic, SizeClass::regular, 800, 600) ==
        SizeClass::regular
    );
    OA_CHECK(
        places::class_under(WindowSize::compact, SizeClass::large, 1920, 1080) == SizeClass::compact
    );
    OA_CHECK(
        places::class_under(WindowSize::large, SizeClass::regular, 1000, 640) == SizeClass::large
    );
    OA_CHECK(
        places::class_under(WindowSize::large, SizeClass::regular, 800, 600) == SizeClass::regular
    );
    OA_CHECK(
        places::class_under(WindowSize::regular, SizeClass::compact, 640, 480) == SizeClass::compact
    );
}

/// Checks the place keys, their values, remembering, forgetting and which
/// screens move.
void a_place_is_kept_per_window_and_class() {
    OA_CHECK(
        places::place_key("settings", kit::SizeClass::large) ==
        "open-annihilation.window.settings.large"
    );
    OA_CHECK(
        places::place_key("key-editor", kit::SizeClass::compact) ==
        "open-annihilation.window.key-editor.compact"
    );
    OA_CHECK(places::class_word(kit::SizeClass::regular) == "regular");

    OA_CHECK(point_is(places::offset_of("-120,48").value_or(kit::Point{}), -120, 48));
    OA_CHECK(point_is(places::offset_of("+3,-4").value_or(kit::Point{}), 3, -4));
    OA_CHECK(point_is(places::offset_of("0,0").value_or(kit::Point{1, 1}), 0, 0));
    for (const char* bad :
         {"",
          "1",
          "1,",
          ",2",
          "a,b",
          "1,2,3",
          "1 ,2",
          " 1,2",
          "1,2 ",
          "2000000,0",
          "0,-1000001",
          "++1,2",
          "1.5,2"})
        OA_CHECK(!places::offset_of(bad).has_value());
    OA_CHECK(places::offset_text({-120, 48}) == "-120,48");

    std::map<std::string, std::string> preferences{
        {"open-annihilation.window-place", "kept"},
        {"open-annihilation.window-frame", "kept"},
        {"open-annihilation.windowed", "kept"},
    };
    OA_CHECK(point_is(places::stored_offset(preferences, "settings", kit::SizeClass::large), 0, 0));
    OA_CHECK(places::store_offset(preferences, "settings", kit::SizeClass::large, {-120, 48}));
    OA_CHECK(preferences["open-annihilation.window.settings.large"] == "-120,48");
    OA_CHECK(!places::store_offset(preferences, "settings", kit::SizeClass::large, {-120, 48}));
    OA_CHECK(
        point_is(places::stored_offset(preferences, "settings", kit::SizeClass::large), -120, 48)
    );
    // Each class keeps its own place.
    OA_CHECK(
        point_is(places::stored_offset(preferences, "settings", kit::SizeClass::regular), 0, 0)
    );
    OA_CHECK(places::store_offset(preferences, "library", kit::SizeClass::compact, {5, 6}));
    // A value that cannot be read is the centre.
    preferences["open-annihilation.window.map-browser.large"] = "left";
    OA_CHECK(
        point_is(places::stored_offset(preferences, "map-browser", kit::SizeClass::large), 0, 0)
    );
    // The centre erases the key.
    OA_CHECK(places::store_offset(preferences, "library", kit::SizeClass::compact, {0, 0}));
    OA_CHECK(preferences.count("open-annihilation.window.library.compact") == 0);
    OA_CHECK(!places::store_offset(preferences, "library", kit::SizeClass::compact, {0, 0}));
    // Forgetting takes every place, and nothing else.
    OA_CHECK(places::forget_places(preferences));
    OA_CHECK(preferences.size() == 3);
    OA_CHECK(preferences.count("open-annihilation.window-place") == 1);
    OA_CHECK(preferences.count("open-annihilation.window-frame") == 1);
    OA_CHECK(preferences.count("open-annihilation.windowed") == 1);
    OA_CHECK(!places::forget_places(preferences));

    for (const char* window :
         {"settings", "key-editor", "library", "map-browser", "mutator-picker", "brain-picker"})
        OA_CHECK(places::moves(window));
    for (const char* other : {"notice", "question", "gallery", "work", "first-run", ""})
        OA_CHECK(!places::moves(other));
}

/// Checks the rounding between points and window pixels.
void points_and_pixels_round_to_the_nearest() {
    OA_CHECK(places::pixels_of(8, 100) == 8);
    OA_CHECK(places::pixels_of(8, 200) == 16);
    OA_CHECK(places::pixels_of(3, 150) == 5);
    OA_CHECK(places::pixels_of(-3, 150) == -5);
    OA_CHECK(places::pixels_of(64, 0) == 64);
    OA_CHECK(places::points_of(5, 150) == 3);
    OA_CHECK(places::points_of(-401, 200) == -201);
    OA_CHECK(places::points_of(16, 200) == 8);
}

/// Returns a 1000 by 800 room at 1x and a 600 by 600 window centred in it,
/// a little high.
///
/// @return the room
places::WindowRoom desktop_room() {
    places::WindowRoom room;
    room.room = {0, 0, 1000, 800};
    room.centred = {200, 100, 600, 600};
    room.scale_percent = 100;
    return room;
}

/// Checks that a window shows exactly unmoved at no offset, moves by its
/// offset, and keeps 64 points on screen and its title in the room.
void a_moved_window_stays_on_screen() {
    const places::WindowRoom room = desktop_room();
    OA_CHECK(rect_is(places::shown_at(room, {0, 0}), 200, 100, 600, 600));
    OA_CHECK(rect_is(places::shown_at(room, {-50, 30}), 150, 130, 600, 600));
    OA_CHECK(rect_is(places::shown_at(room, {-1000, 0}), -536, 100, 600, 600));
    OA_CHECK(rect_is(places::shown_at(room, {1000, 0}), 936, 100, 600, 600));
    OA_CHECK(rect_is(places::shown_at(room, {0, -500}), 200, 0, 600, 600));
    OA_CHECK(rect_is(places::shown_at(room, {0, 1000}), 200, 736, 600, 600));
    OA_CHECK(point_is(places::offset_at(room, places::shown_at(room, {-1000, 0})), -736, 0));

    // At 2x the 64 points are 128 pixels, and an edge less than a point from
    // a safe edge lies on it.
    places::WindowRoom doubled;
    doubled.room = {0, 0, 2000, 1600};
    doubled.centred = {401, 201, 1200, 1200};
    doubled.scale_percent = 200;
    OA_CHECK(rect_is(places::shown_at(doubled, {-1000, 0}), -1072, 201, 1200, 1200));
    OA_CHECK(rect_is(places::shown_at(doubled, {-200, 0}), 0, 201, 1200, 1200));
    OA_CHECK(rect_is(places::shown_at(doubled, {200, 0}), 800, 201, 1200, 1200));
    OA_CHECK(rect_is(places::shown_at(doubled, {0, -100}), 401, 0, 1200, 1200));
    OA_CHECK(point_is(places::offset_at(doubled, {0, 201, 1200, 1200}), -201, 0));
    OA_CHECK(rect_is(places::shown_at(doubled, {-201, 0}), 0, 201, 1200, 1200));

    // An unmoved place past the bounds stays reachable: a window taller than
    // its room keeps its own top while it moves across.
    places::WindowRoom tall;
    tall.room = {0, 0, 1000, 500};
    tall.centred = {200, -50, 600, 600};
    tall.scale_percent = 100;
    OA_CHECK(rect_is(places::shown_at(tall, {40, 0}), 240, -50, 600, 600));
    OA_CHECK(rect_is(places::shown_at(tall, {0, -40}), 200, -50, 600, 600));
}

/// Checks snapping to the unmoved place, the room's centre and the safe edges.
void a_dragged_window_snaps() {
    const places::WindowRoom room = desktop_room();
    OA_CHECK(rect_is(places::snapped(room, {205, 300, 600, 600}), 200, 300, 600, 600));
    OA_CHECK(rect_is(places::snapped(room, {-10, 190, 600, 600}), 0, 200, 600, 600));
    OA_CHECK(rect_is(places::snapped(room, {412, 13, 600, 600}), 400, 13, 600, 600));
    OA_CHECK(rect_is(places::snapped(room, {413, 12, 600, 600}), 413, 0, 600, 600));
    // Beside a match's side column the unmoved place is not the centre;
    // near the room's centre the window snaps there.
    places::WindowRoom beside = room;
    beside.centred = {300, 100, 600, 600};
    OA_CHECK(rect_is(places::snapped(beside, {195, 100, 600, 600}), 200, 100, 600, 600));
    OA_CHECK(rect_is(places::snapped(beside, {290, 100, 600, 600}), 300, 100, 600, 600));
    // At 2x the 12 points are 24 pixels.
    places::WindowRoom doubled;
    doubled.room = {0, 0, 2000, 1600};
    doubled.centred = {400, 200, 1200, 1200};
    doubled.scale_percent = 200;
    OA_CHECK(rect_is(places::snapped(doubled, {24, 500, 1200, 1200}), 0, 500, 1200, 1200));
    OA_CHECK(rect_is(places::snapped(doubled, {25, 500, 1200, 1200}), 25, 500, 1200, 1200));
}

/// Checks a drag's offset and a key's move.
void a_drag_and_a_key_move_the_window() {
    const places::WindowRoom room = desktop_room();
    OA_CHECK(point_is(places::dragged_offset(room, {0, 0}, {300, 0}), 300, 0));
    OA_CHECK(point_is(places::dragged_offset(room, {0, 0}, {205, 0}), 200, 0));
    OA_CHECK(point_is(places::dragged_offset(room, {0, 0}, {5, -3}), 0, 0));
    OA_CHECK(point_is(places::dragged_offset(room, {0, 0}, {-2000, 2000}), -736, 636));
    OA_CHECK(point_is(places::dragged_offset(room, {100, 0}, {50, 0}), 150, 0));

    OA_CHECK(point_is(places::moved_by_key(room, {0, 0}, 1, 0, false), 8, 0));
    OA_CHECK(point_is(places::moved_by_key(room, {0, 0}, 1, 0, true), 32, 0));
    OA_CHECK(point_is(places::moved_by_key(room, {0, 0}, 0, -1, false), 0, -8));
    OA_CHECK(point_is(places::moved_by_key(room, {0, -100}, 0, -1, false), 0, -100));
    // An offset past the bounds moves from where the window shows.
    OA_CHECK(point_is(places::moved_by_key(room, {0, -400}, 0, 1, true), 0, -68));
    places::WindowRoom doubled;
    doubled.room = {0, 0, 2000, 1600};
    doubled.centred = {400, 200, 1200, 1200};
    doubled.scale_percent = 200;
    OA_CHECK(rect_is(
        places::shown_at(doubled, places::moved_by_key(doubled, {0, 0}, -1, 1, false)),
        384,
        216,
        1200,
        1200
    ));
}

/// Checks a mouse's drag, a finger's hold, and a finger that strays first.
void a_finger_holds_the_title_before_it_drags() {
    const places::WindowRoom room = desktop_room();
    places::TitleDrag mouse;
    mouse.window = "settings";
    mouse.grab = {300, 50};
    mouse.moving = true;
    OA_CHECK(places::drag_move(mouse, room, {310, 60}, 0) == places::DragStep::moves);
    OA_CHECK(point_is(mouse.offset, 0, 0));
    OA_CHECK(places::drag_move(mouse, room, {400, 50}, 0) == places::DragStep::moves);
    OA_CHECK(point_is(mouse.offset, 100, 0));

    places::TitleDrag finger;
    finger.window = "settings";
    finger.grab = {300, 50};
    finger.finger = true;
    finger.pressed_ms = 1000;
    OA_CHECK(places::drag_move(finger, room, {303, 52}, 1100) == places::DragStep::holds);
    OA_CHECK(!finger.moving && point_is(finger.offset, 0, 0));
    OA_CHECK(places::drag_move(finger, room, {400, 50}, 1400) == places::DragStep::moves);
    OA_CHECK(finger.moving && point_is(finger.offset, 100, 0));

    places::TitleDrag strays = finger;
    strays.moving = false;
    strays.offset = {};
    OA_CHECK(places::drag_move(strays, room, {320, 50}, 1200) == places::DragStep::cancelled);
    OA_CHECK(places::finger_hold_ms == 400);
}

/// One window of the resolution table in a match: where the in-match
/// Settings shows unmoved, its class and its scale.
struct MatchRow {
    int32_t width{};         ///< canvas pixels
    int32_t height{};        ///< canvas pixels
    bool offer_fractional{}; ///< 150% is among Auto's steps
    int32_t scale_percent{}; ///< the dialog's scale
    kit::SizeClass size_class{};
    int32_t x{}; ///< the dialog's left column
    int32_t y{}; ///< the dialog's top row
};

/// The resolution table, in a match with HUD scaling On: the dialog
/// measures the whole window and covers the side column only where the
/// area beside it is too narrow (800 by 600, 1024 by 768).
constexpr MatchRow match_rows[] = {
    {640, 480, false, 100, kit::SizeClass::compact, 144, 78},
    {800, 600, false, 100, kit::SizeClass::regular, 40, 57},
    {1024, 768, false, 100, kit::SizeClass::large, 32, 84},
    {1280, 720, false, 100, kit::SizeClass::large, 256, 60},
    {1366, 768, false, 100, kit::SizeClass::large, 305, 84},
    {1280, 800, false, 100, kit::SizeClass::large, 266, 100},
    {1920, 1080, false, 100, kit::SizeClass::large, 480, 240},
    {1920, 1080, true, 150, kit::SizeClass::large, 368, 90},
    {2560, 1440, false, 200, kit::SizeClass::large, 320, 120},
    // The side column's scale, 2, holds 3x down.
    {3840, 2160, false, 200, kit::SizeClass::large, 960, 480},
};

/// Checks the in-match Settings against the resolution table, a Window size
/// chosen, HUD scaling Off, a full-screen MacBook and a phone.
void the_match_measures_the_whole_window() {
    for (const MatchRow& row : match_rows) {
        const layout::MatchLayout laid = layout::make_match_layout(row.width, row.height);
        const int32_t percent = row.offer_fractional ? kit::auto_with_fraction_percent : 0;
        const oa::app::MatchDialogPlace place =
            places::match_window(laid, percent, WindowSize::automatic, false);
        const kit::Metrics& sized = kit::metrics_of(row.size_class);
        OA_CHECK(place.scale_percent == row.scale_percent);
        OA_CHECK(place.size_class == row.size_class);
        OA_CHECK(rect_is(
            place.shown,
            row.x,
            row.y,
            sized.dialog_width * row.scale_percent / 100,
            sized.dialog_height * row.scale_percent / 100
        ));
    }
    // A Window size chosen: Regular and Compact keep 1920 by 1080 at 2x;
    // Large is held to Regular where its dialog does not fit.
    const layout::MatchLayout full_hd = layout::make_match_layout(1920, 1080);
    const auto regular = places::match_window(full_hd, 0, WindowSize::regular, false);
    OA_CHECK(regular.scale_percent == 200 && regular.size_class == kit::SizeClass::regular);
    OA_CHECK(rect_is(regular.shown, 368, 54, 1440, 972));
    const auto compact = places::match_window(full_hd, 0, WindowSize::compact, false);
    OA_CHECK(compact.scale_percent == 200 && compact.size_class == kit::SizeClass::compact);
    OA_CHECK(rect_is(compact.shown, 480, 216, 960, 648));
    const auto held =
        places::match_window(layout::make_match_layout(800, 600), 0, WindowSize::large, false);
    OA_CHECK(held.size_class == kit::SizeClass::regular);
    // HUD scaling Off caps the scale at the side column's 1x.
    const layout::MatchLayout unscaled = layout::make_match_layout(1920, 1080, 1.0);
    const auto off = places::match_window(
        unscaled, kit::auto_with_fraction_percent, WindowSize::automatic, false
    );
    OA_CHECK(off.scale_percent == 100 && off.size_class == kit::SizeClass::large);
    OA_CHECK(rect_is(off.shown, 480, 240, 960, 600));
    // A MacBook's full screen at two pixels a point: 3x, held to the side
    // column's 2x, leaves 1512 by 982 points.
    layout::MatchLayout macbook = layout::make_match_layout(3024, 1964);
    macbook.px_per_point = 2.0;
    const auto retina = places::match_window(macbook, 0, WindowSize::automatic, false);
    OA_CHECK(retina.scale_percent == 200 && retina.size_class == kit::SizeClass::large);
    OA_CHECK(kit::frame_of(places::match_viewport(macbook, 0)).area.width == 1512);
    // A phone fits Compact to its safe area, with no scale of its own.
    const layout::MatchLayout phone = layout::make_phone_layout(1704, 786, 2.0, {118, 0, 118, 42});
    const auto fitted = places::match_window(phone, 0, WindowSize::large, true);
    OA_CHECK(fitted.scale_percent == 0 && fitted.size_class == kit::SizeClass::compact);
    OA_CHECK(fitted.shown.height == 744 && fitted.shown.y == 0);
    OA_CHECK(fitted.shown.x >= 118 && fitted.shown.x + fitted.shown.width <= 118 + 1468);
    OA_CHECK(rect_is(places::match_safe_area(phone), 118, 0, 1468, 744));
}

} // namespace

int main() {
    the_window_size_overrides_the_class();
    a_place_is_kept_per_window_and_class();
    points_and_pixels_round_to_the_nearest();
    a_moved_window_stays_on_screen();
    a_dragged_window_snaps();
    a_drag_and_a_key_move_the_window();
    a_finger_holds_the_title_before_it_drags();
    the_match_measures_the_whole_window();
    return oa::test::check_exit_status();
}
