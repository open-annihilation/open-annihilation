// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The places of the overlays Arrange overlays moves: their keys and values
// in the preferences, kept for each size class, the 8-point grid measured
// from the nearest edge or the centre, a drag, an arrow, the handles, Reset,
// and the overlays' own places at the window sizes the size classes come
// from, none off the overlays' area nor over the side column.

#include "oa_layer_overlays.hpp"

#include "oa/app/family_ring.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <stdint.h>
#include <string>
#include <vector>

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

/// Tells whether the ring painted a pixel.
///
/// @param pixels the ring's pixels
/// @param x the column
/// @param y the row
/// @return true when that pixel is one of them
bool ring_has(const std::vector<oa::app::FamilyRingPixel>& pixels, int32_t x, int32_t y) {
    for (const oa::app::FamilyRingPixel& pixel : pixels)
        if (pixel.x == x && pixel.y == y)
            return true;
    return false;
}

/// Fills an RGB buffer with one colour.
///
/// @param[out] rgb the buffer, three bytes a pixel
/// @param red the red channel
/// @param green the green channel
/// @param blue the blue channel
void fill_rgb(std::vector<uint8_t>& rgb, uint8_t red, uint8_t green, uint8_t blue) {
    for (std::size_t at = 0; at + 2 < rgb.size(); at += 3) {
        rgb[at] = red;
        rgb[at + 1] = green;
        rgb[at + 2] = blue;
    }
}

/// The point size the ring uses for a window rectangle at a scale.
///
/// @param window_w the control's width, in the window's pixels
/// @param window_h the control's height, in the window's pixels
/// @param percent the scale, in percent
/// @param[out] point_w the width, in points
/// @param[out] point_h the height, in points
void ring_points(
    int32_t window_w, int32_t window_h, int32_t percent, int32_t& point_w, int32_t& point_h
) {
    if (percent % 100 == 0) {
        const int32_t scale = std::max(percent / 100, int32_t{1});
        point_w = std::max(window_w / scale, int32_t{1});
        point_h = std::max(window_h / scale, int32_t{1});
        return;
    }
    point_w = std::max(
        static_cast<int32_t>(
            std::lround(static_cast<double>(window_w) * 100.0 / static_cast<double>(percent))
        ),
        int32_t{1}
    );
    point_h = std::max(
        static_cast<int32_t>(
            std::lround(static_cast<double>(window_h) * 100.0 / static_cast<double>(percent))
        ),
        int32_t{1}
    );
}

/// Tells whether blending the family's pixels matches the kit's ring on one ground.
///
/// @param sheet_w the sheet's columns
/// @param sheet_h the sheet's rows
/// @param kit_rgb what the kit painted, three bytes a pixel
/// @param family_rgb the ground with the family's pixels blended on
/// @param percent the scale, named in a failure
/// @return true when every pixel matches
bool sheets_match(
    int32_t sheet_w,
    int32_t sheet_h,
    const std::vector<uint8_t>& kit_rgb,
    const std::vector<uint8_t>& family_rgb,
    int32_t percent
) {
    int mismatches = 0;
    int32_t first_x = 0;
    int32_t first_y = 0;
    uint8_t kit_pixel[3]{};
    uint8_t family_pixel[3]{};
    for (int32_t row = 0; row < sheet_h; ++row) {
        for (int32_t column = 0; column < sheet_w; ++column) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(sheet_w) +
                 static_cast<std::size_t>(column)) *
                3U;
            if (kit_rgb[at] == family_rgb[at] && kit_rgb[at + 1] == family_rgb[at + 1] &&
                kit_rgb[at + 2] == family_rgb[at + 2])
                continue;
            if (mismatches == 0) {
                first_x = column;
                first_y = row;
                kit_pixel[0] = kit_rgb[at];
                kit_pixel[1] = kit_rgb[at + 1];
                kit_pixel[2] = kit_rgb[at + 2];
                family_pixel[0] = family_rgb[at];
                family_pixel[1] = family_rgb[at + 1];
                family_pixel[2] = family_rgb[at + 2];
            }
            ++mismatches;
        }
    }
    if (mismatches != 0) {
        std::fprintf(
            stderr,
            "ring at %d%%: %d pixels differ, first (%d,%d) kit %02x%02x%02x family %02x%02x%02x\n",
            static_cast<int>(percent),
            mismatches,
            static_cast<int>(first_x),
            static_cast<int>(first_y),
            kit_pixel[0],
            kit_pixel[1],
            kit_pixel[2],
            family_pixel[0],
            family_pixel[1],
            family_pixel[2]
        );
    }
    return mismatches == 0;
}

/// Blends the family's ring onto a ground and compares it with the kit's.
///
/// @param ground the ground, three bytes a pixel, replaced by the blend
/// @param sheet_w the sheet's columns
/// @param sheet_h the sheet's rows
/// @param pixels the family's ring, in the window's pixels
/// @param origin_x the sheet's left column, in the window's pixels
/// @param origin_y the sheet's top row, in the window's pixels
/// @param crisp true at a whole scale
/// @return false when a pixel falls outside the sheet
bool blend_onto_ground(
    std::vector<uint8_t>& ground,
    int32_t sheet_w,
    int32_t sheet_h,
    const std::vector<oa::app::FamilyRingPixel>& pixels,
    int32_t origin_x,
    int32_t origin_y,
    bool crisp
) {
    for (const oa::app::FamilyRingPixel& pixel : pixels) {
        const int32_t column = pixel.x - origin_x;
        const int32_t row = pixel.y - origin_y;
        if (column < 0 || row < 0 || column >= sheet_w || row >= sheet_h)
            return false;
        const std::size_t at = (static_cast<std::size_t>(row) * static_cast<std::size_t>(sheet_w) +
                                static_cast<std::size_t>(column)) *
                               3U;
        uint8_t red = ground[at];
        uint8_t green = ground[at + 1];
        uint8_t blue = ground[at + 2];
        oa::app::blend_family_coverage(red, green, blue, pixel.coverage, crisp);
        ground[at] = red;
        ground[at + 1] = green;
        ground[at + 2] = blue;
    }
    return true;
}

/// The family's ring over a plain ground equals the kit's, at one whole scale.
///
/// @param percent 100 or 200
/// @return true when the pixels match
bool crisp_ring_matches(int32_t percent) {
    constexpr int32_t window_x = 12;
    constexpr int32_t window_y = 8;
    constexpr int32_t window_w = 80;
    constexpr int32_t window_h = 24;
    const int32_t scale = percent / 100;
    int32_t point_w = 0;
    int32_t point_h = 0;
    ring_points(window_w * scale, window_h * scale, percent, point_w, point_h);
    const int32_t inset = kit::compact_metrics.focus_inset;
    const int32_t sheet_w = (point_w + 2 * inset) * scale;
    const int32_t sheet_h = (point_h + 2 * inset) * scale;
    oa::ui::frontend_renderer::Surface sheet;
    sheet.width = static_cast<uint32_t>(sheet_w);
    sheet.height = static_cast<uint32_t>(sheet_h);
    sheet.rgb.assign(static_cast<std::size_t>(sheet_w) * static_cast<std::size_t>(sheet_h) * 3U, 0);
    fill_rgb(sheet.rgb, 40, 50, 60);
    std::vector<uint8_t> family = sheet.rgb;
    kit::Canvas canvas{};
    canvas.surface = &sheet;
    canvas.placement = {0, 0, scale, {}};
    kit::draw_focus_ring(canvas, {inset, inset, point_w, point_h}, true);
    const std::vector<oa::app::FamilyRingPixel> pixels = oa::app::family_ring_pixels(
        window_x, window_y, window_w * scale, window_h * scale, percent
    );
    if (!blend_onto_ground(
            family,
            sheet_w,
            sheet_h,
            pixels,
            window_x - inset * scale,
            window_y - inset * scale,
            true
        ))
        return false;
    return sheets_match(sheet_w, sheet_h, sheet.rgb, family, percent);
}

/// The family's ring over a plain ground equals the kit's, at 150%.
///
/// @return true when the pixels match
bool smooth_ring_matches() {
    constexpr int32_t percent = 150;
    constexpr int32_t window_x = 12;
    constexpr int32_t window_y = 8;
    constexpr int32_t window_w = 60;
    constexpr int32_t window_h = 24;
    int32_t point_w = 0;
    int32_t point_h = 0;
    ring_points(window_w, window_h, percent, point_w, point_h);
    const int32_t inset = kit::compact_metrics.focus_inset;
    const kit::Rect control{inset, inset, point_w, point_h};
    const kit::Rect sheet_points{0, 0, point_w + 2 * inset, point_h + 2 * inset};
    const kit::Rect sheet_px = kit::to_canvas(sheet_points, percent);
    const kit::Rect control_px = kit::to_canvas(control, percent);
    oa::ui::paint::Canvas picture = oa::ui::paint::make_canvas(sheet_px.width, sheet_px.height);
    for (std::size_t at = 0; at + 3 < picture.rgba.size(); at += 4) {
        picture.rgba[at] = 40;
        picture.rgba[at + 1] = 50;
        picture.rgba[at + 2] = 60;
        picture.rgba[at + 3] = 255;
    }
    std::vector<uint8_t> kit_rgb(
        static_cast<std::size_t>(sheet_px.width) * static_cast<std::size_t>(sheet_px.height) * 3U
    );
    oa::ui::paint::Painter painter(picture);
    kit::Canvas canvas{};
    canvas.smooth.painter = &painter;
    canvas.smooth.scale_percent = percent;
    kit::draw_focus_ring(canvas, control, true);
    for (int32_t row = 0; row < sheet_px.height; ++row) {
        for (int32_t column = 0; column < sheet_px.width; ++column) {
            const std::size_t pixel =
                static_cast<std::size_t>(row) * static_cast<std::size_t>(sheet_px.width) +
                static_cast<std::size_t>(column);
            const std::size_t from = pixel * 4U;
            const std::size_t to = pixel * 3U;
            kit_rgb[to] = picture.rgba[from];
            kit_rgb[to + 1] = picture.rgba[from + 1];
            kit_rgb[to + 2] = picture.rgba[from + 2];
        }
    }
    std::vector<uint8_t> family = kit_rgb;
    fill_rgb(family, 40, 50, 60);
    const std::vector<oa::app::FamilyRingPixel> pixels =
        oa::app::family_ring_pixels(window_x, window_y, window_w, window_h, percent);
    if (!blend_onto_ground(
            family,
            sheet_px.width,
            sheet_px.height,
            pixels,
            window_x - control_px.x,
            window_y - control_px.y,
            false
        ))
        return false;
    return sheets_match(sheet_px.width, sheet_px.height, kit_rgb, family, percent);
}

/// Over a plain ground, the family's pixels are the kit's at 100, 150 and 200.
void check_ring_on_ground() {
    OA_CHECK(crisp_ring_matches(100));
    OA_CHECK(smooth_ring_matches());
    OA_CHECK(crisp_ring_matches(200));
}

/// A 3.1c control's canvas rectangle and an extension window's own pixels.
void check_control_window_rect() {
    // The 640 by 480 picture in a 1280 by 720 window: 160 pixels of letterbox
    // on each side, and the picture at one and a half times.
    oa::app::FamilyWindowMap map;
    map.identity = false;
    map.origin_x = 160.0F;
    map.origin_y = 0.0F;
    map.scale_x = 1.5F;
    map.scale_y = 1.5F;
    map.density = 1.0F;
    oa::app::FamilyControlBox canvas;
    canvas.x = 0;
    canvas.y = 10;
    canvas.width = 80;
    canvas.height = 24;
    int32_t left = 0;
    int32_t top = 0;
    int32_t width = 0;
    int32_t height = 0;
    OA_CHECK(oa::app::control_window_rect(canvas, map, left, top, width, height));
    OA_CHECK(left == 160);
    OA_CHECK(top == 15);
    OA_CHECK(width == 120);
    OA_CHECK(height == 36);
    const std::vector<oa::app::FamilyRingPixel> canvas_ring =
        oa::app::family_ring_pixels(left, top, width, height, 100);
    OA_CHECK(ring_has(canvas_ring, left + width / 2, top - 2));
    OA_CHECK(!ring_has(canvas_ring, left + width / 2, top));

    oa::app::FamilyControlBox window;
    window.window_pixels = true;
    window.x = 100;
    window.y = 40;
    window.width = 60;
    window.height = 18;
    OA_CHECK(oa::app::control_window_rect(window, map, left, top, width, height));
    OA_CHECK(left == 100);
    OA_CHECK(top == 40);
    OA_CHECK(width == 60);
    OA_CHECK(height == 18);
    const std::vector<oa::app::FamilyRingPixel> window_ring =
        oa::app::family_ring_pixels(left, top, width, height, 100);
    OA_CHECK(ring_has(window_ring, 130, 38));
    OA_CHECK(!ring_has(window_ring, 130, 40));
}

/// The kit's ring around a gadget's rectangle and around an extension window's.
void check_family_ring() {
    // A gadget 80 by 24 at (10, 20). The ring's top edge is two pixels above it.
    const std::vector<oa::app::FamilyRingPixel> gadget =
        oa::app::family_ring_pixels(10, 20, 80, 24, 100);
    OA_CHECK(ring_has(gadget, 50, 18));
    OA_CHECK(!ring_has(gadget, 50, 20));
    // An extension window 60 by 18 at (100, 40).
    const std::vector<oa::app::FamilyRingPixel> window =
        oa::app::family_ring_pixels(100, 40, 60, 18, 100);
    OA_CHECK(ring_has(window, 130, 38));
    OA_CHECK(!ring_has(window, 130, 40));
    OA_CHECK(oa::app::family_ring_pixels(0, 0, 0, 24, 100).empty());
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
    check_family_ring();
    check_ring_on_ground();
    check_control_window_rect();
    return oa::test::check_exit_status();
}
