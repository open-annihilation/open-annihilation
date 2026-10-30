// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kit's points, Auto scale, the Interface size's scale rule, size
// classes and the resolution table they make, arrangements, scroll areas and display list. The scroll numbers were computed with geometry::scroll_thumb,
// geometry::scroll_at and geometry::scroll_showing at c5e005df, the start of
// this branch, by a throwaway program that was not committed. The parts of a
// screen drawn in the modern fonts are measured with a made-up measure: 6
// pixels a character, 7 in bold, and lines 2 pixels taller than their size.

#include "oa/test/check.hpp"
#include "oa/ui/kit/layout.hpp"

#include <array>
#include <cstddef>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace kit = oa::ui::kit;

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

/// One row of the design's window table, and the frame it makes at density 1.
struct Window {
    int32_t width{};         ///< canvas pixels
    int32_t height{};        ///< canvas pixels
    int32_t scale{};         ///< the Auto scale
    int32_t points_width{};  ///< the frame's width, in points
    int32_t points_height{}; ///< the frame's height, in points
    kit::SizeClass size_class{kit::SizeClass::compact};
};

/// The design's six windows, at one canvas pixel a window point, at the
/// Auto scale of their height alone (kit::auto_scale), before the step down
/// kit::layer_viewport may take.
constexpr Window windows[] = {
    {640, 480, 1, 640, 480, kit::SizeClass::compact},
    {1024, 768, 1, 1024, 768, kit::SizeClass::large},
    {1280, 720, 1, 1280, 720, kit::SizeClass::large},
    {1920, 1080, 2, 960, 540, kit::SizeClass::regular},
    {2560, 1440, 2, 1280, 720, kit::SizeClass::large},
    {3840, 2160, 3, 1280, 720, kit::SizeClass::large},
};

/// Checks that each size class gives its own metrics.
void each_class_gives_its_metrics() {
    OA_CHECK(&kit::metrics_of(kit::SizeClass::compact) == &kit::compact_metrics);
    OA_CHECK(&kit::metrics_of(kit::SizeClass::regular) == &kit::regular_metrics);
    OA_CHECK(&kit::metrics_of(kit::SizeClass::large) == &kit::large_metrics);
    static_assert(kit::metrics_of(kit::SizeClass::regular).dialog_width == 720);
    static_assert(kit::metrics_of(kit::SizeClass::large).dialog_height == 600);
    // Each window of the design's table lays its dialog out at its class.
    for (const Window& window : windows) {
        const kit::Metrics& metrics = kit::metrics_of(window.size_class);
        OA_CHECK(metrics.dialog_width <= window.points_width);
        OA_CHECK(metrics.dialog_height <= window.points_height);
    }
    OA_CHECK(kit::metrics_of(kit::size_class_of(960, 540)).dialog_width == 720);
    OA_CHECK(kit::metrics_of(kit::size_class_of(1280, 720)).dialog_width == 960);
    OA_CHECK(kit::metrics_of(kit::size_class_of(640, 480)).dialog_width == 480);
}

/// Checks a point against a rectangle's inside and its edges.
void a_point_lies_inside_and_not_on_the_far_edges() {
    const kit::Rect rect{0, 0, 10, 10};
    OA_CHECK(kit::contains(rect, {0, 0}));
    OA_CHECK(kit::contains(rect, {9, 9}));
    OA_CHECK(!kit::contains(rect, {10, 5}));
    OA_CHECK(!kit::contains(rect, {5, 10}));
    OA_CHECK(!kit::contains(rect, {-1, 0}));
    OA_CHECK(rect_is(kit::intersect(rect, {8, 8, 10, 10}), 8, 8, 2, 2));
    OA_CHECK(rect_is(kit::intersect(rect, {20, 20, 4, 4}), 20, 20, 0, 0));
    OA_CHECK(kit::wholly_in({2, 2, 3, 3}, rect));
    OA_CHECK(kit::wholly_in(rect, rect));
    OA_CHECK(!kit::wholly_in({0, 0, 11, 10}, rect));
    OA_CHECK(rect_is(kit::grown({5, 5, 4, 4}, 2), 3, 3, 8, 8));
    OA_CHECK(rect_is(kit::inset({0, 0, 20, 20}, {1, 2, 3, 4}), 1, 2, 16, 14));
    const kit::Point inside = kit::nearest_point(rect, {3, 4});
    OA_CHECK(inside.x == 3 && inside.y == 4);
    const kit::Point outside = kit::nearest_point(rect, {-2, 20});
    OA_CHECK(outside.x == 0 && outside.y == 9);
    const kit::Point empty = kit::nearest_point({5, 6, 0, 4}, {100, 100});
    OA_CHECK(empty.x == 5 && empty.y == 6);
}

/// Checks the Auto scale at density 1, and where the density raises it.
/// 150% is not offered: the scale is a whole step, as a percentage.
void the_scale_follows_the_canvas_and_the_density() {
    constexpr int32_t heights[] = {480, 720, 768, 1079, 1080, 1440, 1800, 2160};
    constexpr int32_t scales[] = {1, 1, 1, 1, 2, 2, 3, 3};
    for (std::size_t index = 0; index < 8; ++index)
        OA_CHECK(kit::auto_scale(heights[index], 1.0F) == scales[index] * 100);
    // A 786-row canvas rounds to 1 from its height, and a 1179-row canvas to 2.
    OA_CHECK(kit::auto_scale(786, 2.0F) == 200);
    OA_CHECK(kit::auto_scale(786, 3.0F) == 300);
    OA_CHECK(kit::auto_scale(1179, 2.0F) == 200);
    OA_CHECK(kit::auto_scale(1179, 3.0F) == 300);
}

/// Returns a canvas at one canvas pixel a window point, with no insets.
///
/// @param width canvas pixels
/// @param height canvas pixels
/// @return the canvas
kit::Viewport desktop_canvas(int32_t width, int32_t height);

/// One canvas height and the Auto scale it gives, as a percentage, with 150%
/// not offered and with it offered.
struct AutoHeight {
    int32_t height{};     ///< canvas rows
    int32_t whole{};      ///< the scale with 150% not offered
    int32_t fractional{}; ///< the scale with 150% offered
};

/// Checks Auto's steps with 150% offered and not, the density floor, and a
/// room too small for Compact at 150%.
void auto_chooses_one_and_a_half_where_it_is_offered() {
    constexpr AutoHeight rows[] = {
        {720, 100, 100},
        {899, 100, 100},
        {900, 100, 150},
        {1077, 100, 150},
        {1080, 200, 150},
        {1259, 200, 150},
        {1260, 200, 200},
        {1440, 200, 200},
        {1799, 200, 200},
        {1800, 300, 300},
        {2160, 300, 300},
    };
    for (const AutoHeight& row : rows) {
        OA_CHECK(kit::auto_scale(row.height, 1.0F, false) == row.whole);
        OA_CHECK(kit::auto_scale(row.height, 1.0F, true) == row.fractional);
    }
    // The density floor is a whole step, so 2 and 3 never leave 150% in place.
    for (const int32_t floor_percent : {200, 300}) {
        const float density = static_cast<float>(floor_percent / 100);
        for (const AutoHeight& row : rows) {
            const int32_t whole = row.whole > floor_percent ? row.whole : floor_percent;
            const int32_t fractional =
                row.fractional > floor_percent ? row.fractional : floor_percent;
            OA_CHECK(kit::auto_scale(row.height, density, false) == whole);
            OA_CHECK(kit::auto_scale(row.height, density, true) == fractional);
        }
    }
    // 4x from the tie at 3.5, which is 2520 lines.
    OA_CHECK(kit::auto_scale(2519, 1.0F, true) == 300);
    OA_CHECK(kit::auto_scale(2520, 1.0F, true) == 400);

    // 1077 lines would be 150%. 719 columns do not hold Compact's 720 at one
    // and a half, so Auto draws 1x; 720 columns do.
    OA_CHECK(
        kit::layer_viewport(desktop_canvas(719, 1077), kit::auto_with_fraction_percent)
            .scale_percent == 100
    );
    OA_CHECK(
        kit::layer_viewport(desktop_canvas(720, 1077), kit::auto_with_fraction_percent)
            .scale_percent == 150
    );
    // 1920 by 1080 is Large at 150%, and so is 1914 by 1077, a few lines
    // short, with 1276 by 718 points. Without 150% offered, 1080 lines would
    // be Regular at 2x, so Auto steps down to Large at 1x.
    const kit::Viewport full_hd =
        kit::layer_viewport(desktop_canvas(1920, 1080), kit::auto_with_fraction_percent);
    OA_CHECK(full_hd.scale_percent == 150);
    OA_CHECK(kit::frame_of(full_hd).size_class == kit::SizeClass::large);
    const kit::Viewport short_hd =
        kit::layer_viewport(desktop_canvas(1914, 1077), kit::auto_with_fraction_percent);
    OA_CHECK(short_hd.scale_percent == 150);
    OA_CHECK(kit::frame_of(short_hd).size_class == kit::SizeClass::large);
    OA_CHECK(kit::layer_viewport(desktop_canvas(1920, 1080), 0).scale_percent == 100);
    // A phone at density 2 stays at 2x: the density floor is above 150%.
    kit::Viewport phone;
    phone.width = 1704;
    phone.height = 786;
    phone.density = 2.0F;
    phone.safe = {118, 0, 118, 42};
    OA_CHECK(kit::layer_viewport(phone, kit::auto_with_fraction_percent).scale_percent == 200);
}

/// Checks each window of the design's table, and a phone whose insets come off first.
void the_frame_matches_the_window_table() {
    for (const Window& window : windows) {
        OA_CHECK(kit::auto_scale(window.height, 1.0F) == window.scale * 100);
        kit::Viewport viewport;
        viewport.width = window.width;
        viewport.height = window.height;
        viewport.density = 1.0F;
        viewport.scale_percent = window.scale * 100;
        const kit::Frame frame = kit::frame_of(viewport);
        OA_CHECK(frame.scale_percent == window.scale * 100);
        OA_CHECK(frame.size_class == window.size_class);
        OA_CHECK(rect_is(frame.area, 0, 0, window.points_width, window.points_height));
    }
    // One side reaching a class does not.
    OA_CHECK(kit::size_class_of(1007, 648) == kit::SizeClass::regular);
    OA_CHECK(kit::size_class_of(1008, 647) == kit::SizeClass::regular);
    OA_CHECK(kit::size_class_of(767, 534) == kit::SizeClass::compact);
    OA_CHECK(kit::size_class_of(768, 533) == kit::SizeClass::compact);

    // 2880 by 1800 at density 2: the height rounds to 3, above the density.
    OA_CHECK(kit::auto_scale(1800, 2.0F) == 300);
    kit::Viewport hidpi;
    hidpi.width = 2880;
    hidpi.height = 1800;
    hidpi.density = 2.0F;
    hidpi.scale_percent = 300;
    const kit::Frame wide = kit::frame_of(hidpi);
    OA_CHECK(wide.size_class == kit::SizeClass::regular);
    OA_CHECK(rect_is(wide.area, 0, 0, 960, 600));

    // A 1704 by 786 canvas at 2 canvas pixels a window point. The insets are
    // 118, 0, 118 and 42 canvas pixels: 59 points at each side and 21 at the
    // bottom, times 2.
    OA_CHECK(kit::auto_scale(786, 2.0F) == 200);
    kit::Viewport phone;
    phone.width = 1704;
    phone.height = 786;
    phone.density = 2.0F;
    phone.scale_percent = 200;
    phone.safe = {118, 0, 118, 42};
    const kit::Frame frame = kit::frame_of(phone);
    OA_CHECK(frame.scale_percent == 200);
    OA_CHECK(frame.size_class == kit::SizeClass::compact);
    OA_CHECK(rect_is(frame.area, 59, 0, 734, 372));
}

/// One example of an Interface size on a window, at one canvas pixel a window
/// point: the scale the OA layer draws at and the class it lays out at.
struct Chosen {
    int32_t width{};          ///< canvas pixels
    int32_t height{};         ///< canvas pixels
    int32_t chosen_percent{}; ///< the Interface size, 0 for Auto
    int32_t scale{};          ///< the whole scale drawn at
    kit::SizeClass size_class{kit::SizeClass::compact};
};

/// The design's examples of Interface sizes.
constexpr Chosen chosen_sizes[] = {
    // 3x would need 1440 columns for Compact's 480.
    {1280, 720, 400, 2, kit::SizeClass::compact},
    {1920, 1080, 100, 1, kit::SizeClass::large},
    {1920, 1080, 300, 3, kit::SizeClass::compact},
    {640, 480, 200, 1, kit::SizeClass::compact},
    {3840, 2160, 400, 4, kit::SizeClass::regular},
    {3840, 2160, 0, 3, kit::SizeClass::large},
};

kit::Viewport desktop_canvas(int32_t width, int32_t height) {
    kit::Viewport canvas;
    canvas.width = width;
    canvas.height = height;
    canvas.density = 1.0F;
    return canvas;
}

/// Checks the Interface size's scale rule: the design's examples, Auto at
/// each window the OA layer's checks use, the step held to what fits
/// Compact, 150% held to where Compact fits at one and a half, and the
/// density floor and insets on a touch canvas.
void an_interface_size_takes_the_largest_step_that_fits_compact() {
    for (const Chosen& example : chosen_sizes) {
        const kit::Viewport canvas = desktop_canvas(example.width, example.height);
        const kit::Viewport laid = kit::layer_viewport(canvas, example.chosen_percent);
        OA_CHECK(laid.scale_percent == example.scale * 100);
        OA_CHECK(kit::frame_of(laid).size_class == example.size_class);
        // Nothing but the scale changes.
        OA_CHECK(laid.width == canvas.width && laid.height == canvas.height);
        OA_CHECK(laid.density == canvas.density);
    }
    // Auto at each window is checked against the resolution table
    // (compact_only_where_nothing_larger_fits). The 2560 by 1080 ultrawide
    // window the window-size check takes would be Regular at 2x with 1280 by
    // 540 points, so Auto steps down to Large at 1x.
    const kit::Viewport ultrawide = kit::layer_viewport(desktop_canvas(2560, 1080), 0);
    OA_CHECK(ultrawide.scale_percent == 100);
    OA_CHECK(kit::frame_of(ultrawide).size_class == kit::SizeClass::large);
    // The canvas's own scale is not read, and a negative choice is Auto.
    kit::Viewport scaled = desktop_canvas(1920, 1080);
    scaled.scale_percent = 400;
    OA_CHECK(kit::layer_viewport(scaled, 0).scale_percent == 100);
    OA_CHECK(kit::layer_viewport(scaled, -100).scale_percent == 100);
    // Each step up to the most that fits, and no further; never below 1x.
    const kit::Viewport full_hd = desktop_canvas(1920, 1080);
    OA_CHECK(kit::layer_viewport(full_hd, 200).scale_percent == 200);
    OA_CHECK(kit::layer_viewport(full_hd, 400).scale_percent == 300);
    OA_CHECK(kit::layer_viewport(desktop_canvas(1439, 1080), 300).scale_percent == 200);
    OA_CHECK(kit::layer_viewport(desktop_canvas(1440, 972), 300).scale_percent == 300);
    OA_CHECK(kit::layer_viewport(desktop_canvas(1440, 971), 300).scale_percent == 200);
    OA_CHECK(kit::layer_viewport(desktop_canvas(400, 300), 400).scale_percent == 100);
    OA_CHECK(kit::layer_viewport(desktop_canvas(400, 300), 0).scale_percent == 100);
    // A percent between steps takes the step below it.
    OA_CHECK(kit::layer_viewport(full_hd, 250).scale_percent == 200);
    OA_CHECK(kit::layer_viewport(full_hd, 50).scale_percent == 100);

    // 150%, held to where Compact fits at one and a half: 1920 by 1080 is
    // 1.5x with 1280 by 720 points, Large; 1280 by 720 is 1.5x with 853 by
    // 480 points, Compact; 640 by 480 would need 720 columns, so is 1x.
    OA_CHECK(kit::fractional_scale_percent == 150);
    const kit::Viewport large_fraction = kit::layer_viewport(full_hd, 150);
    OA_CHECK(large_fraction.scale_percent == 150);
    OA_CHECK(kit::frame_of(large_fraction).size_class == kit::SizeClass::large);
    OA_CHECK(rect_is(kit::frame_of(large_fraction).area, 0, 0, 1280, 720));
    const kit::Viewport compact_fraction = kit::layer_viewport(desktop_canvas(1280, 720), 150);
    OA_CHECK(compact_fraction.scale_percent == 150);
    OA_CHECK(kit::frame_of(compact_fraction).size_class == kit::SizeClass::compact);
    OA_CHECK(rect_is(kit::frame_of(compact_fraction).area, 0, 0, 853, 480));
    const kit::Viewport too_small = kit::layer_viewport(desktop_canvas(640, 480), 150);
    OA_CHECK(too_small.scale_percent == 100);
    OA_CHECK(kit::frame_of(too_small).size_class == kit::SizeClass::compact);
    // The edge: Compact's 480 by 324 points take 720 by 486 pixels.
    OA_CHECK(kit::layer_viewport(desktop_canvas(720, 486), 150).scale_percent == 150);
    OA_CHECK(kit::layer_viewport(desktop_canvas(719, 486), 150).scale_percent == 100);
    OA_CHECK(kit::layer_viewport(desktop_canvas(720, 485), 150).scale_percent == 100);

    // The 1704 by 786 phone canvas at density 2 with its insets: Auto keeps
    // the density's 2x, Compact; a chosen 100% replaces the density floor
    // and lays out the room's 1468 by 744 points at Large; 400% is held to
    // the 2x at which Compact fits 744 rows.
    kit::Viewport phone;
    phone.width = 1704;
    phone.height = 786;
    phone.density = 2.0F;
    phone.safe = {118, 0, 118, 42};
    const kit::Viewport automatic = kit::layer_viewport(phone, 0);
    OA_CHECK(automatic.scale_percent == 200);
    OA_CHECK(kit::frame_of(automatic).size_class == kit::SizeClass::compact);
    OA_CHECK(automatic.safe.left == 118 && automatic.safe.bottom == 42);
    const kit::Viewport one = kit::layer_viewport(phone, 100);
    OA_CHECK(one.scale_percent == 100);
    OA_CHECK(kit::frame_of(one).size_class == kit::SizeClass::large);
    OA_CHECK(rect_is(kit::frame_of(one).area, 118, 0, 1468, 744));
    OA_CHECK(kit::layer_viewport(phone, 400).scale_percent == 200);
    // 150% replaces the density floor too, as the room of 1468 by 744
    // pixels holds Compact at one and a half; with 260 columns of insets
    // more it does not.
    OA_CHECK(kit::layer_viewport(phone, 150).scale_percent == 150);
    kit::Viewport narrow_phone = phone;
    narrow_phone.safe.left += 760;
    OA_CHECK(kit::layer_viewport(narrow_phone, 150).scale_percent == 100);
    // The insets count: 1440 by 972 fits 3x, and not with a row inset.
    kit::Viewport inset_canvas = desktop_canvas(1440, 972);
    OA_CHECK(kit::layer_viewport(inset_canvas, 300).scale_percent == 300);
    inset_canvas.safe = {0, 1, 0, 0};
    OA_CHECK(kit::layer_viewport(inset_canvas, 300).scale_percent == 200);
}

/// One row of the resolution table "Compact only where it is needed": a
/// window, how Auto lays it out on the front end, and the room's points.
struct Resolution {
    int32_t width{};             ///< canvas pixels
    int32_t height{};            ///< canvas pixels
    float density{1.0F};         ///< canvas pixels per window point
    bool offer_fractional{};     ///< 150% is among Auto's steps: the modern fonts
    int32_t scale_percent{};     ///< the scale Auto lays the screens out at
    int32_t points_width{};      ///< the room's width at that scale, in points
    int32_t points_height{};     ///< the room's height at that scale, in points
    kit::SizeClass size_class{}; ///< the room's class
};

/// The resolution table, menus: Compact only at 640 by 480; 800 by 600
/// Regular; every larger window Large, Auto taking one step down where its
/// height's scale would leave Regular.
constexpr Resolution resolutions[] = {
    {640, 480, 1.0F, false, 100, 640, 480, kit::SizeClass::compact},
    {800, 600, 1.0F, false, 100, 800, 600, kit::SizeClass::regular},
    {1024, 768, 1.0F, false, 100, 1024, 768, kit::SizeClass::large},
    {1280, 720, 1.0F, false, 100, 1280, 720, kit::SizeClass::large},
    {1366, 768, 1.0F, false, 100, 1366, 768, kit::SizeClass::large},
    // The Steam Deck.
    {1280, 800, 1.0F, false, 100, 1280, 800, kit::SizeClass::large},
    // 150% would leave 960 by 600, Regular.
    {1440, 900, 1.0F, true, 100, 1440, 900, kit::SizeClass::large},
    {1440, 900, 1.0F, false, 100, 1440, 900, kit::SizeClass::large},
    // 2x would leave 960 by 540, Regular.
    {1920, 1080, 1.0F, false, 100, 1920, 1080, kit::SizeClass::large},
    {1920, 1080, 1.0F, true, 150, 1280, 720, kit::SizeClass::large},
    {2560, 1440, 1.0F, false, 200, 1280, 720, kit::SizeClass::large},
    {2560, 1440, 1.0F, true, 200, 1280, 720, kit::SizeClass::large},
    {3840, 2160, 1.0F, false, 300, 1280, 720, kit::SizeClass::large},
    // A MacBook's full screen, two pixels a point: 3x leaves 1008 by 654.
    {3024, 1964, 2.0F, false, 300, 1008, 654, kit::SizeClass::large},
    {3024, 1964, 2.0F, true, 300, 1008, 654, kit::SizeClass::large},
    // A 15-inch MacBook's full screen: 3x would leave 960 by 600, and 2x is
    // the density floor.
    {2880, 1800, 2.0F, false, 200, 1440, 900, kit::SizeClass::large},
    // An 11-inch iPad on its side, 1194 by 834 points.
    {2388, 1668, 2.0F, false, 200, 1194, 834, kit::SizeClass::large},
};

/// Checks the resolution table on the front end, the class edges, a chosen
/// class held to the room, and the step down serving a chosen class.
void compact_only_where_nothing_larger_fits() {
    static_assert(kit::class_margin == 24);
    static_assert(kit::regular_from.x == 768 && kit::regular_from.y == 534);
    static_assert(kit::large_from.x == 1008 && kit::large_from.y == 648);
    for (const Resolution& row : resolutions) {
        kit::Viewport canvas = desktop_canvas(row.width, row.height);
        canvas.density = row.density;
        const kit::Viewport laid =
            kit::layer_viewport(canvas, row.offer_fractional ? kit::auto_with_fraction_percent : 0);
        const kit::Frame frame = kit::frame_of(laid);
        OA_CHECK(laid.scale_percent == row.scale_percent);
        OA_CHECK(rect_is(frame.area, 0, 0, row.points_width, row.points_height));
        OA_CHECK(frame.size_class == row.size_class);
    }
    // The class edges, each side on its own.
    OA_CHECK(kit::size_class_of(1008, 648) == kit::SizeClass::large);
    OA_CHECK(kit::size_class_of(768, 534) == kit::SizeClass::regular);
    OA_CHECK(kit::size_class_of(1007, 2000) == kit::SizeClass::regular);
    OA_CHECK(kit::size_class_of(2000, 533) == kit::SizeClass::compact);

    // A chosen class shows wherever its dialog fits whole, the margin not
    // counted, and is held to the largest that does elsewhere.
    using kit::SizeClass;
    OA_CHECK(kit::size_class_within(SizeClass::large, 960, 600) == SizeClass::large);
    OA_CHECK(kit::size_class_within(SizeClass::large, 959, 600) == SizeClass::regular);
    OA_CHECK(kit::size_class_within(SizeClass::large, 960, 599) == SizeClass::regular);
    OA_CHECK(kit::size_class_within(SizeClass::large, 719, 600) == SizeClass::compact);
    OA_CHECK(kit::size_class_within(SizeClass::regular, 720, 486) == SizeClass::regular);
    OA_CHECK(kit::size_class_within(SizeClass::regular, 720, 485) == SizeClass::compact);
    OA_CHECK(kit::size_class_within(SizeClass::regular, 1920, 1080) == SizeClass::regular);
    OA_CHECK(kit::size_class_within(SizeClass::compact, 1920, 1080) == SizeClass::compact);
    OA_CHECK(kit::size_class_within(SizeClass::compact, 100, 100) == SizeClass::compact);

    // The step down serves the class wanted: 1920 by 1080 is Regular at 2x,
    // which a chosen Regular or Compact keeps. A step down that would not
    // reach the wanted class is not taken: 1000 by 1080 is 500 by 540 at 2x,
    // Compact, and Regular at 1x, so Large wanted keeps 2x and Regular wanted
    // takes 1x.
    const kit::Viewport full_hd = desktop_canvas(1920, 1080);
    OA_CHECK(kit::layer_viewport(full_hd, 0, SizeClass::regular).scale_percent == 200);
    OA_CHECK(kit::layer_viewport(full_hd, 0, SizeClass::compact).scale_percent == 200);
    OA_CHECK(kit::layer_viewport(full_hd, 0, SizeClass::large).scale_percent == 100);
    const kit::Viewport tall = desktop_canvas(1000, 1080);
    OA_CHECK(kit::layer_viewport(tall, 0, SizeClass::large).scale_percent == 200);
    OA_CHECK(kit::layer_viewport(tall, 0, SizeClass::regular).scale_percent == 100);
    // A chosen percent is never stepped down.
    OA_CHECK(kit::layer_viewport(full_hd, 200).scale_percent == 200);
    // The density floor holds: the phone at two pixels a point stays at 2x,
    // Compact, though 1x would give Large.
    kit::Viewport phone = desktop_canvas(1704, 786);
    phone.density = 2.0F;
    phone.safe = {118, 0, 118, 42};
    OA_CHECK(kit::layer_viewport(phone, 0).scale_percent == 200);
    OA_CHECK(kit::layer_viewport(phone, kit::auto_with_fraction_percent).scale_percent == 200);
    // With 150% offered the step below 2x is 150%: 1920 by 1280 is 960 by
    // 640 at 2x, Regular, and 1280 by 853 at 150%, Large. On whole steps it
    // is Large at 1x.
    const kit::Viewport tall_hd = desktop_canvas(1920, 1280);
    OA_CHECK(kit::layer_viewport(tall_hd, kit::auto_with_fraction_percent).scale_percent == 150);
    OA_CHECK(kit::layer_viewport(tall_hd, 0).scale_percent == 100);
}

/// Checks that neighbouring rectangles share an edge at each scale, that each
/// edge is rounded to the nearest pixel on its own, and that a whole scale
/// maps a rectangle back onto itself.
void canvas_pixels_tile_and_whole_scales_round_trip() {
    const kit::Rect left{0, 0, 10, 10};
    const kit::Rect right{10, 0, 10, 10};
    const kit::Rect below{0, 10, 10, 8};
    constexpr int32_t percentages[] = {100, 150, 200, 300};
    for (const int32_t percent : percentages) {
        const kit::Rect across = kit::to_canvas(left, percent);
        const kit::Rect beside = kit::to_canvas(right, percent);
        const kit::Rect under = kit::to_canvas(below, percent);
        OA_CHECK(across.x + across.width == beside.x);
        OA_CHECK(across.y == beside.y);
        OA_CHECK(across.width > 0 && beside.width > 0);
        OA_CHECK(across.y + across.height == under.y);
        OA_CHECK(across.x == under.x);
    }
    OA_CHECK(rect_is(kit::to_canvas(left, 150), 0, 0, 15, 15));
    OA_CHECK(rect_is(kit::to_canvas(right, 150), 15, 0, 15, 15));
    // At 1.5x each edge is lround(point * 1.5) on its own: a point is one or
    // two pixels, halves rounded away from zero.
    OA_CHECK(rect_is(kit::to_canvas({1, 1, 1, 1}, 150), 2, 2, 1, 1));
    OA_CHECK(rect_is(kit::to_canvas({2, 3, 1, 1}, 150), 3, 5, 2, 1));
    OA_CHECK(rect_is(kit::to_canvas({3, 5, 10, 10}, 150), 5, 8, 15, 15));
    OA_CHECK(rect_is(kit::to_canvas({-1, -3, 2, 2}, 150), -2, -5, 4, 3));

    const kit::Rect samples[] = {{0, 0, 1, 1}, {3, 5, 7, 11}, {10, 20, 30, 40}};
    constexpr int32_t whole[] = {100, 200, 300};
    for (const kit::Rect& sample : samples) {
        for (const int32_t percent : whole) {
            const kit::Rect canvas = kit::to_canvas(sample, percent);
            const kit::Point origin = kit::to_points({canvas.x, canvas.y}, percent);
            const kit::Point far =
                kit::to_points({canvas.x + canvas.width, canvas.y + canvas.height}, percent);
            OA_CHECK(origin.x == sample.x && origin.y == sample.y);
            OA_CHECK(far.x - origin.x == sample.width);
            OA_CHECK(far.y - origin.y == sample.height);
        }
    }
}

/// Checks a column, a row at each alignment, a grid and the two splits.
void arrangements_place_the_rectangles() {
    const kit::Rect area{10, 20, 100, 80};
    const int32_t heights[] = {15, 25, 5};
    const std::vector<kit::Rect> down = kit::column(area, heights, 4);
    OA_CHECK(down.size() == 3);
    OA_CHECK(rect_is(down[0], 10, 20, 100, 15));
    OA_CHECK(rect_is(down[1], 10, 39, 100, 25));
    OA_CHECK(rect_is(down[2], 10, 68, 100, 5));
    OA_CHECK(kit::column(area, {}, 4).empty());

    const kit::Rect band{0, 0, 100, 16};
    const int32_t widths[] = {10, 20, 30};
    const std::vector<kit::Rect> at_left = kit::row(band, widths, 5, kit::Align::left);
    const std::vector<kit::Rect> at_centre = kit::row(band, widths, 5, kit::Align::centre);
    const std::vector<kit::Rect> at_right = kit::row(band, widths, 5, kit::Align::right);
    OA_CHECK(at_left.size() == 3 && at_centre.size() == 3 && at_right.size() == 3);
    OA_CHECK(rect_is(at_left[0], 0, 0, 10, 16));
    OA_CHECK(rect_is(at_left[1], 15, 0, 20, 16));
    OA_CHECK(rect_is(at_left[2], 40, 0, 30, 16));
    OA_CHECK(rect_is(at_centre[0], 15, 0, 10, 16));
    OA_CHECK(rect_is(at_centre[1], 30, 0, 20, 16));
    OA_CHECK(rect_is(at_centre[2], 55, 0, 30, 16));
    OA_CHECK(rect_is(at_right[0], 30, 0, 10, 16));
    OA_CHECK(rect_is(at_right[1], 45, 0, 20, 16));
    OA_CHECK(rect_is(at_right[2], 70, 0, 30, 16));

    // Three cells in a row whose width is exactly three least widths and two gaps.
    const std::vector<kit::Rect> exact = kit::grid({0, 0, 70, 40}, 20, 10, 5, 3);
    OA_CHECK(exact.size() == 3);
    OA_CHECK(rect_is(exact[0], 0, 0, 20, 10));
    OA_CHECK(rect_is(exact[1], 25, 0, 20, 10));
    OA_CHECK(rect_is(exact[2], 50, 0, 20, 10));

    // Seven cells: three columns, the spare two points on the left-hand cells.
    const std::vector<kit::Rect> seven = kit::grid({8, 4, 100, 80}, 30, 12, 4, 7);
    OA_CHECK(seven.size() == 7);
    OA_CHECK(rect_is(seven[0], 8, 4, 31, 12));
    OA_CHECK(rect_is(seven[1], 43, 4, 31, 12));
    OA_CHECK(rect_is(seven[2], 78, 4, 30, 12));
    OA_CHECK(rect_is(seven[3], 8, 20, 31, 12));
    OA_CHECK(rect_is(seven[4], 43, 20, 31, 12));
    OA_CHECK(rect_is(seven[5], 78, 20, 30, 12));
    OA_CHECK(rect_is(seven[6], 8, 36, 31, 12));
    OA_CHECK(seven[2].x + seven[2].width == 108);
    OA_CHECK(kit::grid(area, 20, 10, 4, 0).empty());

    const kit::Split across = kit::split_across(area, 40, 6);
    OA_CHECK(rect_is(across.first, 10, 20, 40, 80));
    OA_CHECK(rect_is(across.second, 56, 20, 54, 80));
    const kit::Split down_split = kit::split_down(area, 30, 6);
    OA_CHECK(rect_is(down_split.first, 10, 20, 100, 30));
    OA_CHECK(rect_is(down_split.second, 10, 56, 100, 44));
    const kit::Split none_left = kit::split_across(area, 100, 10);
    OA_CHECK(none_left.second.width == 0 && none_left.second.height == 80);
}

/// Graphics' scroll area, and the values computed for it with the settings
/// dialog's scroll functions.
///
/// view {158, 54, 309, 236}, well {470, 54, 7, 236}, content height 749,
/// limit 513, least thumb height 16. The half offset is 256, half of 513
/// rounded down. The well's top row is 54 and its bottom row is 289.
void the_scroll_matches_the_settings_dialog() {
    kit::ScrollArea area;
    area.view = {158, 54, 309, 236};
    area.well = {470, 54, 7, 236};
    area.content_height = 749;
    area.limit = 513;
    OA_CHECK(kit::ScrollArea::limit_for(749, 236) == 513);
    OA_CHECK(kit::ScrollArea::limit_for(100, 236) == 0);
    OA_CHECK(kit::ScrollArea::limit_for(236, 236) == 0);

    area.offset = 0;
    OA_CHECK(rect_is(area.thumb(16), 471, 55, 5, 73));
    area.offset = 1;
    OA_CHECK(rect_is(area.thumb(16), 471, 55, 5, 73));
    area.offset = 256;
    OA_CHECK(rect_is(area.thumb(16), 471, 135, 5, 73));
    // The thumb's top at offset 256 reads back as 255.
    OA_CHECK(area.offset_at(135, 16) == 255);
    area.offset = 513;
    OA_CHECK(rect_is(area.thumb(16), 471, 216, 5, 73));
    OA_CHECK(area.offset_at(216, 16) == 513);
    OA_CHECK(area.offset_at(54, 16) == 0);
    OA_CHECK(area.offset_at(289, 16) == 513);

    // The limit is the area's own, here 600 rather than 749 - 236.
    area.limit = 600;
    area.offset = 300;
    OA_CHECK(rect_is(area.thumb(16), 471, 136, 5, 73));
    OA_CHECK(area.offset_at(136, 16) == 302);
    area.limit = 513;

    area.offset = 0;
    OA_CHECK(area.offset_showing(54, 28, false) == 0);
    OA_CHECK(area.offset_showing(400, 36, false) == 147);
    area.offset = 400;
    OA_CHECK(area.offset_showing(80, 20, false) == 26);
    area.offset = 100;
    OA_CHECK(area.offset_showing(700, 49, true) == 513);
    area.offset = 1;
    OA_CHECK(area.offset_showing(400, 36, false) == 147);
    area.offset = 256;
    OA_CHECK(area.offset_showing(400, 36, false) == 256);
    area.offset = 513;
    OA_CHECK(area.offset_showing(400, 36, false) == 346);
    area.offset = 200;
    OA_CHECK(area.offset_showing(400, 36, false) == 200);

    area.offset = 40;
    OA_CHECK(rect_is(area.placed({158, 400, 40, 16}), 158, 360, 40, 16));
    OA_CHECK(rect_is(area.placed({158, 400, 0, 16}), 158, 400, 0, 16));
    OA_CHECK(rect_is(area.placed({158, 400, 40, 0}), 158, 400, 40, 0));
}

/// Checks hit testing: list order, a clip, a disabled control and an empty list.
void a_point_hits_the_first_enabled_control() {
    OA_CHECK(kit::hit({}, {0, 0}) == kit::no_control);

    kit::DisplayList list;
    list.controls.push_back(kit::Control{1, {0, 0, 20, 20}, {}, true});
    list.controls.push_back(kit::Control{2, {10, 10, 20, 20}, {}, true});
    OA_CHECK(kit::hit(list, {12, 12}) == 1);
    OA_CHECK(kit::hit(list, {25, 25}) == 2);
    OA_CHECK(kit::hit(list, {30, 30}) == kit::no_control);

    kit::DisplayList clipped;
    clipped.controls.push_back(kit::Control{7, {0, 0, 40, 40}, {10, 10, 10, 10}, true});
    OA_CHECK(kit::hit(clipped, {5, 5}) == kit::no_control);
    OA_CHECK(kit::hit(clipped, {15, 15}) == 7);
    OA_CHECK(kit::hit(clipped, {19, 19}) == 7);
    OA_CHECK(kit::hit(clipped, {20, 20}) == kit::no_control);

    kit::DisplayList disabled;
    disabled.controls.push_back(kit::Control{3, {0, 0, 10, 10}, {}, false});
    disabled.controls.push_back(kit::Control{4, {0, 0, 10, 10}, {}, true});
    OA_CHECK(kit::hit(disabled, {1, 1}) == 4);
    disabled.controls.pop_back();
    OA_CHECK(kit::hit(disabled, {1, 1}) == kit::no_control);

    // An empty clip is no clip. An item is not a control, so a point on one misses.
    kit::DisplayList drawn;
    kit::Item item;
    item.role = kit::Role::fill;
    item.rect = {0, 0, 50, 50};
    item.control = 9;
    drawn.items.push_back(item);
    drawn.controls.push_back(kit::Control{5, {0, 0, 10, 10}, {100, 100, 0, 0}, true});
    OA_CHECK(kit::hit(drawn, {1, 1}) == 5);
    OA_CHECK(kit::hit(drawn, {30, 30}) == kit::no_control);
    const kit::Control* found = kit::control_of(drawn, 5);
    OA_CHECK(found != nullptr && found->id == 5);
    OA_CHECK(kit::control_of(drawn, 9) == nullptr);
}

/// Checks the rule for a disabled control a point lies in: it passes the point on to the
/// controls under it, as hit does, or blocks it and the point reaches no control; a clip that
/// hides the point from it lets the point through either way.
void a_disabled_control_passes_or_blocks_a_point() {
    kit::DisplayList list;
    list.controls.push_back(kit::Control{1, {0, 0, 20, 20}, {}, false});
    list.controls.push_back(kit::Control{3, {20, 20, 20, 20}, {20, 20, 20, 5}, false});
    list.controls.push_back(kit::Control{2, {0, 0, 40, 40}, {}, true});
    // On the disabled control.
    OA_CHECK(kit::hit(list, {5, 5}) == 2);
    OA_CHECK(kit::hit(list, {5, 5}, kit::DisabledControl::passes) == 2);
    OA_CHECK(kit::hit(list, {5, 5}, kit::DisabledControl::blocks) == kit::no_control);
    // On the part of the clipped disabled control a press reaches, and on the part it hides.
    OA_CHECK(kit::hit(list, {30, 22}, kit::DisabledControl::passes) == 2);
    OA_CHECK(kit::hit(list, {30, 22}, kit::DisabledControl::blocks) == kit::no_control);
    OA_CHECK(kit::hit(list, {30, 30}, kit::DisabledControl::blocks) == 2);
    // On the enabled control alone, and on nothing.
    OA_CHECK(kit::hit(list, {30, 5}, kit::DisabledControl::blocks) == 2);
    OA_CHECK(kit::hit(list, {50, 50}, kit::DisabledControl::blocks) == kit::no_control);
    // An enabled control over a disabled one takes the point under either rule.
    kit::DisplayList over;
    over.controls.push_back(kit::Control{4, {0, 0, 10, 10}, {}, true});
    over.controls.push_back(kit::Control{5, {0, 0, 20, 20}, {}, false});
    OA_CHECK(kit::hit(over, {5, 5}, kit::DisabledControl::blocks) == 4);
    OA_CHECK(kit::hit(over, {15, 15}, kit::DisabledControl::blocks) == kit::no_control);
    OA_CHECK(kit::hit(over, {15, 15}, kit::DisabledControl::passes) == kit::no_control);
}

/// Measures a line as the made-up font draws it: 6 pixels a character, 7 in bold.
///
/// @param text the line
/// @param bold the weight
/// @return pixels
int made_up_width(void*, std::string_view text, int, bool bold) {
    return static_cast<int>(kit::character_count(text)) * (bold ? 7 : 6);
}

/// Gives a line's height in the made-up font: 2 pixels more than its size.
///
/// @param pixel_size the size
/// @return pixels
int made_up_line(void*, int pixel_size, bool) {
    return pixel_size + 2;
}

/// Returns a typesetter with the made-up font at a scale.
///
/// @param px_per_point canvas pixels per point
/// @return the typesetter
kit::Typesetter made_up_type(float px_per_point) {
    kit::Typesetter type{};
    type.measure.width = made_up_width;
    type.measure.line_height = made_up_line;
    type.px_per_point = px_per_point;
    return type;
}

/// Points become whole canvas pixels, a half rounded away from zero, and a text's size at
/// least one pixel.
void points_round_to_canvas_pixels() {
    const kit::Typesetter twice = made_up_type(2.0F);
    OA_CHECK(kit::canvas_pixels(twice, 10.25F) == 21);
    OA_CHECK(kit::canvas_pixels(twice, -1.25F) == -3);
    OA_CHECK(kit::canvas_pixels(made_up_type(1.5F), 3.0F) == 5);
    OA_CHECK(kit::canvas_pixels(twice, 0.1F) == 0);
    OA_CHECK(kit::canvas_font(twice, 0.1F) == 1);
    OA_CHECK(kit::canvas_font(twice, 11.0F) == 22);
}

/// A text part wraps to its room, drops empty lines, keeps at most its lines with an
/// ellipsis, and sits in its room as asked.
void a_text_part_wraps_to_its_room() {
    const kit::Typesetter type = made_up_type(1.0F);
    const kit::Lettering look{10.0F, false, kit::screen_colour::dim, kit::TextStyle::lead};
    std::vector<kit::Item> parts;
    const int32_t height =
        kit::text_part(parts, type, kit::Role::text, {5, 7}, 40, "one two three four", look);
    OA_CHECK(height == 48);
    OA_CHECK(parts.size() == 1);
    const kit::Item& part = parts.front();
    OA_CHECK(part.role == kit::Role::text);
    OA_CHECK((part.lines == std::vector<std::string>{"one", "two", "three", "four"}));
    OA_CHECK(rect_is(part.rect, 5, 7, 40, 48));
    OA_CHECK(part.pixel_size == 10 && !part.bold);
    OA_CHECK(part.colour == kit::screen_colour::dim);
    OA_CHECK(part.style == kit::TextStyle::lead);
    OA_CHECK(part.control == kit::no_control);

    // At most two lines: the second takes the rest, shortened with an ellipsis.
    parts.clear();
    OA_CHECK(
        kit::text_part(
            parts,
            type,
            kit::Role::title,
            {0, 0},
            40,
            "one two three four",
            look,
            kit::TextFit::column,
            2
        ) == 24
    );
    OA_CHECK((parts.front().lines == std::vector<std::string>{"one", "two t\xE2\x80\xA6"}));
    OA_CHECK(parts.front().role == kit::Role::title);

    // Tight to the left or the right: as wide as its widest line.
    parts.clear();
    kit::text_part(
        parts, type, kit::Role::text, {5, 0}, 40, "one two three", look, kit::TextFit::tight_left
    );
    kit::text_part(
        parts, type, kit::Role::text, {5, 0}, 40, "one two three", look, kit::TextFit::tight_right
    );
    OA_CHECK(rect_is(parts[0].rect, 5, 0, 30, 36));
    OA_CHECK(rect_is(parts[1].rect, 15, 0, 30, 36));

    // A new line starts a paragraph, and an empty paragraph adds no line.
    parts.clear();
    kit::text_part(parts, type, kit::Role::text, {0, 0}, 40, "a\n\nb", look);
    OA_CHECK((parts.front().lines == std::vector<std::string>{"a", "b"}));

    // A path wider than the room breaks after a separator with path_break, and between
    // characters without it.
    parts.clear();
    kit::text_part(parts, type, kit::Role::text, {0, 0}, 40, "/home/deck/Games", look);
    kit::text_part(
        parts,
        type,
        kit::Role::text,
        {0, 0},
        40,
        "/home/deck/Games",
        look,
        kit::TextFit::column,
        0,
        kit::path_break
    );
    OA_CHECK((parts[0].lines == std::vector<std::string>{"/home/", "deck/G", "ames"}));
    OA_CHECK((parts[1].lines == std::vector<std::string>{"/home/", "deck/", "Games"}));

    // No text, or no room, adds nothing.
    parts.clear();
    OA_CHECK(kit::text_part(parts, type, kit::Role::text, {0, 0}, 40, "", look) == 0);
    OA_CHECK(kit::text_part(parts, type, kit::Role::text, {0, 0}, 0, "one", look) == 0);
    OA_CHECK(parts.empty());
}

/// A line part is shortened to its room with an ellipsis and centred on its row.
void a_line_part_is_shortened_to_its_room() {
    const kit::Typesetter type = made_up_type(1.0F);
    const kit::Lettering look{10.0F, false, kit::screen_colour::text, kit::TextStyle::row_title};
    std::vector<kit::Item> parts;
    OA_CHECK(kit::line_part(parts, type, 100, 50, 40, "abcdefghij", look) == 36);
    OA_CHECK(parts.size() == 1);
    OA_CHECK((parts.front().lines == std::vector<std::string>{"abcde\xE2\x80\xA6"}));
    OA_CHECK(rect_is(parts.front().rect, 100, 44, 36, 12));
    OA_CHECK(parts.front().role == kit::Role::text);
    OA_CHECK(parts.front().style == kit::TextStyle::row_title);
    // Ending at x, as a version or a size does; a role of the caller's.
    OA_CHECK(
        kit::line_part(
            parts, type, 100, 50, 40, "v0.6", look, kit::TextFit::tight_right, kit::Role::version
        ) == 24
    );
    OA_CHECK(rect_is(parts.back().rect, 76, 44, 24, 12));
    OA_CHECK(parts.back().role == kit::Role::version);
    // No text, no room, or a room too narrow for the ellipsis adds nothing.
    OA_CHECK(kit::line_part(parts, type, 0, 0, 40, "", look) == 0);
    OA_CHECK(kit::line_part(parts, type, 0, 0, 0, "abc", look) == 0);
    OA_CHECK(kit::line_part(parts, type, 0, 0, 4, "abc", look) == 0);
    OA_CHECK(parts.size() == 2);
}

/// A button's width counts its bold label, its mark and the gap after it, and its padding,
/// and is at least its least width.
void a_button_spans_its_label_mark_and_padding() {
    const kit::Typesetter type = made_up_type(1.0F);
    kit::ButtonSpec ok{};
    ok.label = "OK";
    OA_CHECK(kit::button_span(type, ok, 10.0F, 3.0F, 0.0F) == 20);
    OA_CHECK(kit::button_span(type, ok, 10.0F, 3.0F, 30.0F) == 30);
    // A mark the label's size, and half that size after it.
    ok.glyph = kit::Glyph::check;
    OA_CHECK(kit::button_span(type, ok, 10.0F, 3.0F, 0.0F) == 35);
    // A mark of its own size.
    ok.glyph_points = 4.0F;
    OA_CHECK(kit::button_span(type, ok, 10.0F, 3.0F, 0.0F) == 29);
    OA_CHECK(kit::button_span(made_up_type(2.0F), ok, 10.0F, 3.0F, 0.0F) == 14 + 8 + 10 + 12);
}

/// A button part's label is fitted to what its mark and margins leave, in the colour of its
/// look, and its state says whether it is the main one and whether a press reaches it.
void a_button_part_fits_its_label() {
    const kit::Typesetter type = made_up_type(1.0F);
    kit::ButtonSpec remove{};
    remove.label = "REMOVE ALL";
    remove.role = kit::Role::button_danger;
    remove.glyph = kit::Glyph::trash;
    remove.control = 7;
    remove.enabled = false;
    std::vector<kit::Item> parts;
    kit::button_part(parts, type, {0, 0, 50, 44}, remove, 10.0F);
    OA_CHECK(parts.size() == 1);
    const kit::Item& danger = parts.back();
    OA_CHECK(danger.role == kit::Role::button_danger);
    OA_CHECK(rect_is(danger.rect, 0, 0, 50, 44));
    OA_CHECK((danger.lines == std::vector<std::string>{"RE\xE2\x80\xA6"}));
    OA_CHECK(danger.colour == kit::screen_colour::red);
    OA_CHECK(danger.glyph == kit::Glyph::trash && danger.glyph_size == 0);
    OA_CHECK(danger.control == 7 && danger.state.disabled && !danger.state.on);
    OA_CHECK(danger.style == kit::TextStyle::button && danger.bold && danger.pixel_size == 10);

    kit::ButtonSpec play{};
    play.label = "PLAY";
    play.role = kit::Role::button_main;
    play.glyph = kit::Glyph::play;
    play.glyph_points = 6.0F;
    play.control = 8;
    kit::button_part(parts, made_up_type(2.0F), {0, 0, 200, 88}, play, 10.0F);
    const kit::Item& main = parts.back();
    OA_CHECK((main.lines == std::vector<std::string>{"PLAY"}));
    OA_CHECK(main.colour == kit::screen_colour::ink);
    OA_CHECK(main.glyph_size == 12 && main.pixel_size == 20);
    OA_CHECK(main.state.on && !main.state.disabled);

    kit::ButtonSpec plain{};
    plain.label = "BACK";
    kit::button_part(parts, type, {0, 0, 4, 44}, plain, 10.0F);
    OA_CHECK(parts.back().colour == kit::screen_colour::button_text);
    // Too narrow for even the ellipsis: a button with no label.
    OA_CHECK(parts.back().lines.empty());
}

/// A row of buttons keeps its groups at the room's two sides, or wraps every button from the
/// left when they do not fit one line.
void a_button_row_places_its_groups() {
    const kit::Typesetter type = made_up_type(1.0F);
    const kit::ButtonRowSizes sizes{10.0F, 2.0F, 30.0F, 4.0F, 20.0F};
    kit::ButtonSpec left{};
    left.label = "AB";
    left.control = 1;
    kit::ButtonSpec right{};
    right.label = "CD";
    right.role = kit::Role::button_main;
    right.control = 2;
    const std::array<kit::ButtonSpec, 1> lefts{left};
    const std::array<kit::ButtonSpec, 1> rights{right};
    std::vector<kit::Item> parts;
    OA_CHECK(kit::button_row(parts, type, {10, 5}, 100, lefts, rights, sizes) == 20);
    OA_CHECK(parts.size() == 2);
    OA_CHECK(rect_is(parts[0].rect, 10, 5, 18, 20));
    OA_CHECK(rect_is(parts[1].rect, 80, 5, 30, 20));
    OA_CHECK(parts[0].control == 1 && parts[1].control == 2);

    parts.clear();
    OA_CHECK(kit::button_row(parts, type, {10, 5}, 40, lefts, rights, sizes) == 44);
    OA_CHECK(rect_is(parts[0].rect, 10, 5, 18, 20));
    OA_CHECK(rect_is(parts[1].rect, 10, 29, 30, 20));

    // A button wider than the room takes the room.
    parts.clear();
    OA_CHECK(kit::button_row(parts, type, {0, 0}, 25, rights, {}, sizes) == 20);
    OA_CHECK(rect_is(parts[0].rect, 0, 0, 25, 20));

    parts.clear();
    OA_CHECK(kit::button_row(parts, type, {0, 0}, 100, {}, {}, sizes) == 0);
    OA_CHECK(parts.empty());
}

/// Icons, plain parts and parts moved from one list to another.
void icons_plain_parts_and_moves() {
    std::vector<kit::Item> parts;
    kit::icon_part(parts, {0, 0, 10, 10}, kit::Glyph::none, kit::screen_colour::green, false);
    OA_CHECK(parts.empty());
    kit::icon_part(parts, {1, 2, 10, 10}, kit::Glyph::none, kit::screen_colour::green, true);
    kit::icon_part(parts, {1, 2, 10, 10}, kit::Glyph::check, kit::screen_colour::amber, false);
    OA_CHECK(parts.size() == 2);
    OA_CHECK(parts[0].role == kit::Role::icon && parts[0].state.on);
    OA_CHECK(parts[1].glyph == kit::Glyph::check && !parts[1].state.on);
    OA_CHECK(parts[1].colour == kit::screen_colour::amber);

    kit::plain_part(parts, kit::Role::panel, {3, 4, 50, 60}, kit::screen_colour::line);
    OA_CHECK(parts.back().role == kit::Role::panel);
    OA_CHECK(rect_is(parts.back().rect, 3, 4, 50, 60));
    OA_CHECK(parts.back().colour == kit::screen_colour::line);
    OA_CHECK(parts.back().lines.empty() && parts.back().control == kit::no_control);

    parts[0].clip = {0, 0, 5, 5};
    std::vector<kit::Item> moved;
    kit::move_parts(moved, parts, 10);
    OA_CHECK(parts.empty() && moved.size() == 3);
    OA_CHECK(moved[0].rect.y == 12 && rect_is(moved[0].clip, 0, 0, 5, 5));
    OA_CHECK(moved[2].rect.y == 14);
    std::vector<kit::Item> clipped;
    kit::move_parts(clipped, moved, -2, {0, 0, 100, 100});
    OA_CHECK(moved.empty() && clipped.size() == 3);
    OA_CHECK(clipped[2].rect.y == 12);
    for (const kit::Item& part : clipped)
        OA_CHECK(rect_is(part.clip, 0, 0, 100, 100));
}

/// A column's blocks lie one after another when they fit; else the body scrolls between the
/// fixed top and bottom; else, when that leaves the body too little room, the whole column
/// scrolls.
void a_column_scrolls_its_body_or_the_whole() {
    const kit::Rect area{0, 100, 300, 200};
    const kit::ScrollColumn fits = kit::scroll_column(area, 50, 60, 40, 10, 30, 25);
    OA_CHECK(fits.top == 100 && fits.body == 150 && fits.bottom == 220);
    OA_CHECK(fits.top_clip.width == 0 && fits.body_clip.width == 0 && fits.bottom_clip.width == 0);
    OA_CHECK(fits.scroll.view.width == 0 && fits.scroll.limit == 0 && fits.scroll.offset == 0);

    const kit::ScrollColumn body = kit::scroll_column(area, 50, 200, 40, 10, 30, 25);
    OA_CHECK(rect_is(body.scroll.view, 0, 150, 300, 100));
    OA_CHECK(body.scroll.limit == 100 && body.scroll.offset == 25);
    OA_CHECK(body.scroll.content_height == 200);
    OA_CHECK(body.top == 100 && body.body == 125 && body.bottom == 260);
    OA_CHECK(rect_is(body.body_clip, 0, 150, 300, 100));
    OA_CHECK(body.top_clip.width == 0 && body.bottom_clip.width == 0);
    OA_CHECK(kit::scroll_column(area, 50, 200, 40, 10, 30, 500).scroll.offset == 100);
    OA_CHECK(kit::scroll_column(area, 50, 200, 40, 10, 30, -5).scroll.offset == 0);

    const kit::ScrollColumn whole = kit::scroll_column(area, 150, 200, 40, 10, 30, 25);
    OA_CHECK(rect_is(whole.scroll.view, 0, 100, 300, 200));
    OA_CHECK(whole.scroll.limit == 200 && whole.scroll.offset == 25);
    OA_CHECK(whole.top == 75 && whole.body == 225 && whole.bottom == 435);
    OA_CHECK(rect_is(whole.top_clip, 0, 100, 300, 200));
    OA_CHECK(rect_is(whole.body_clip, 0, 100, 300, 200));
    OA_CHECK(rect_is(whole.bottom_clip, 0, 100, 300, 200));

    // No gap without a body, and an empty body never scrolls alone.
    const kit::ScrollColumn no_body = kit::scroll_column(area, 150, 0, 80, 10, 30, 0);
    OA_CHECK(no_body.scroll.limit == 30 && no_body.bottom == 250);
    OA_CHECK(rect_is(no_body.body_clip, 0, 100, 300, 200));
}

/// The controls of a screen whose parts carry them: the parts after the last backdrop, the
/// last drawn first, and the enabled ones once each in the Tab order, in drawing order.
void parts_list_their_controls() {
    kit::DisplayList list;
    const auto add = [&list](kit::Role role, kit::ControlId control, kit::Rect rect) {
        kit::Item part{};
        part.role = role;
        part.control = control;
        part.rect = rect;
        list.items.push_back(part);
        return list.items.size() - 1;
    };
    add(kit::Role::button, 1, {0, 0, 10, 10});
    OA_CHECK(kit::first_live_part(list) == 0);
    add(kit::Role::backdrop, kit::no_control, {0, 0, 100, 100});
    OA_CHECK(kit::first_live_part(list) == 2);
    add(kit::Role::text, kit::no_control, {0, 0, 10, 10});
    const std::size_t two = add(kit::Role::button, 2, {20, 0, 10, 10});
    list.items[two].clip = {20, 0, 10, 5};
    const std::size_t three = add(kit::Role::toggle, 3, {40, 0, 10, 10});
    list.items[three].state.on = true;
    list.items[three].lines = {"OFF", "ON"};
    add(kit::Role::button_main, 2, {60, 0, 10, 10});
    const std::size_t four = add(kit::Role::button, 4, {80, 0, 10, 10});
    list.items[four].state.disabled = true;
    list.controls.push_back(kit::Control{9, {}, {}, true});
    kit::list_part_controls(list);
    OA_CHECK(list.controls.size() == 4);
    if (list.controls.size() == 4) {
        OA_CHECK(list.controls[0].id == 4 && !list.controls[0].enabled);
        OA_CHECK(list.controls[1].id == 2 && rect_is(list.controls[1].rect, 60, 0, 10, 10));
        OA_CHECK(list.controls[2].id == 3 && list.controls[2].kind == kit::ControlKind::toggle);
        OA_CHECK(list.controls[2].checked && list.controls[2].text == "OFF");
        OA_CHECK(list.controls[3].id == 2 && rect_is(list.controls[3].clip, 20, 0, 10, 5));
        OA_CHECK(list.controls[3].kind == kit::ControlKind::button);
    }
    OA_CHECK((list.tab_order == std::vector<kit::ControlId>{2, 3}));
    // The part under the backdrop takes no press.
    OA_CHECK(kit::hit(list, {5, 5}) == kit::no_control);
    OA_CHECK(kit::hit(list, {25, 2}) == 2);
    OA_CHECK(kit::hit(list, {25, 7}) == kit::no_control);
}

} // namespace

int main() {
    a_point_lies_inside_and_not_on_the_far_edges();
    the_scale_follows_the_canvas_and_the_density();
    auto_chooses_one_and_a_half_where_it_is_offered();
    the_frame_matches_the_window_table();
    an_interface_size_takes_the_largest_step_that_fits_compact();
    each_class_gives_its_metrics();
    compact_only_where_nothing_larger_fits();
    canvas_pixels_tile_and_whole_scales_round_trip();
    arrangements_place_the_rectangles();
    the_scroll_matches_the_settings_dialog();
    a_point_hits_the_first_enabled_control();
    a_disabled_control_passes_or_blocks_a_point();
    points_round_to_canvas_pixels();
    a_text_part_wraps_to_its_room();
    a_line_part_is_shortened_to_its_room();
    a_button_spans_its_label_mark_and_padding();
    a_button_part_fits_its_label();
    a_button_row_places_its_groups();
    icons_plain_parts_and_moves();
    a_column_scrolls_its_body_or_the_whole();
    parts_list_their_controls();
    return oa::test::check_exit_status();
}
