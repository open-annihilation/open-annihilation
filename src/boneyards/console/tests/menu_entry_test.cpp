// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How the way into the Boneyards stands on a main menu, with made-up
// pictures and layouts: the mode chosen from the share of the strip's rows a
// menu's art changes, on both sides of the limit, and the mode a profile
// names; and the place of the strip's ENTER plate: where the profile's x
// and y put it, kept on the menu, or at the default place at the top right.
#include "oa/boneyards/console/menu_entry.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/ui/frontend_renderer/main_menu_art.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace console = oa::boneyards::console;
namespace renderer = oa::ui::frontend_renderer;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

/// A 640x480 menu picture of one grey.
///
/// @param grey every channel of every pixel
/// @return the picture
oa::Image menu_picture(uint8_t grey) {
    oa::Image picture;
    picture.width = 640;
    picture.height = 480;
    picture.rgb.assign(640U * 480U * 3U, grey);
    return picture;
}

/// Repaints the first pixels of the strip's rows, row by row.
///
/// @param[in,out] picture the picture
/// @param count the pixels repainted
void repaint_strip_rows(oa::Image& picture, std::size_t count) {
    const std::size_t first = static_cast<std::size_t>(renderer::main_menu_band_top) * 640U;
    for (std::size_t pixel = first; pixel < first + count; ++pixel)
        picture.rgb[pixel * 3U] = 250;
}

/// The mode a picture gets against the base game's, with nothing named.
///
/// @param shown the menu's picture
/// @param base the base game's
/// @return the mode
console::MenuEntryMode mode_for(const oa::Image& shown, const oa::Image& base) {
    const uint32_t share =
        renderer::compare_menu_art(
            shown, base, renderer::main_menu_band_top, renderer::main_menu_band_rows
        )
            .per_mille();
    return console::choose_menu_entry_mode(console::MenuEntryAsk::automatic, share);
}

void test_mode() {
    const oa::Image base = menu_picture(90);
    const std::size_t band = 640U * 102U;
    check(
        mode_for(base, base) == console::MenuEntryMode::strip, "the base game's art shows the strip"
    );
    // A recoloured copy that changes 4% of the rows, and art that changes
    // 10% exactly, keep the strip; one pixel row more, and 17%, take the button.
    oa::Image recoloured = base;
    repaint_strip_rows(recoloured, band * 4U / 100U);
    check(mode_for(recoloured, base) == console::MenuEntryMode::strip, "4% keeps the strip");
    oa::Image tenth = base;
    repaint_strip_rows(tenth, band / 10U);
    check(mode_for(tenth, base) == console::MenuEntryMode::strip, "10% keeps the strip");
    oa::Image past = base;
    repaint_strip_rows(past, band / 10U + 640U);
    check(mode_for(past, base) == console::MenuEntryMode::button, "past 10% takes the button");
    oa::Image own = base;
    repaint_strip_rows(own, band * 17U / 100U);
    check(mode_for(own, base) == console::MenuEntryMode::button, "17% takes the button");
    // A profile's mode wins over the measure.
    check(
        console::choose_menu_entry_mode(console::MenuEntryAsk::button, 0) ==
            console::MenuEntryMode::button,
        "button is named on the base game's art"
    );
    check(
        console::choose_menu_entry_mode(console::MenuEntryAsk::strip, 1000) ==
            console::MenuEntryMode::strip,
        "strip is named on art of the mod's own"
    );
    check(
        console::choose_menu_entry_mode(console::MenuEntryAsk::none, 0) ==
            console::MenuEntryMode::none,
        "none is named"
    );
    check(console::menu_entry_ask_named("auto") == console::MenuEntryAsk::automatic, "auto");
    check(console::menu_entry_ask_named("strip") == console::MenuEntryAsk::strip, "strip");
    check(console::menu_entry_ask_named("button") == console::MenuEntryAsk::button, "button");
    check(console::menu_entry_ask_named("none") == console::MenuEntryAsk::none, "none");
    check(console::menu_entry_ask_named("Strip") == console::MenuEntryAsk::automatic, "case");
}

void test_place() {
    using renderer::SourceRect;
    constexpr SourceRect plate = console::entry_plate;
    check(plate.x == 253 && plate.y == 380, "the plate is the strip's own, from 253,380");
    check(plate.width == 135 && plate.height == 66, "the plate is 135 by 66");
    // The default place: the top right, 8 pixels in.
    constexpr SourceRect fallback = console::entry_plate_default_place;
    static_assert(fallback.x == 497 && fallback.y == 8);
    static_assert(fallback.width == 135 && fallback.height == 66);

    // The profile's place, the menu's corners included.
    struct Case {
        int32_t x, y, want_x, want_y;
    };

    for (const Case& one :
         {Case{172, 377, 172, 377},
          Case{0, 0, 0, 0},
          Case{505, 414, 505, 414},
          Case{497, 8, 497, 8}}) {
        const SourceRect place = console::entry_plate_place(one.x, one.y);
        check(
            place.x == one.want_x && place.y == one.want_y && place.width == 135 &&
                place.height == 66,
            "the plate stands where the profile puts it"
        );
    }
    // A place off the menu keeps the whole plate on it.
    for (const Case& one :
         {Case{-5, -9, 0, 0}, Case{600, 470, 505, 414}, Case{506, 415, 505, 414}}) {
        const SourceRect place = console::entry_plate_place(one.x, one.y);
        check(place.x == one.want_x && place.y == one.want_y, "a place off the menu is kept on it");
    }
    // What the plate covers, its one-pixel line included, for the log.
    const renderer::MenuPairPlaces pair =
        renderer::menu_pair_places(renderer::MenuCorner::bottom_right);
    const std::array<SourceRect, 2> oa_buttons{pair.library, pair.settings};
    check(
        !console::entry_plate_covers(fallback, oa_buttons), "the default place covers no OA button"
    );
    check(
        console::entry_plate_covers(console::entry_plate_place(400, 400), oa_buttons),
        "a plate over the pair covers it"
    );
    const std::array<SourceRect, 1> beside{SourceRect{172 + 135, 377, 20, 20}};
    check(
        console::entry_plate_covers(console::entry_plate_place(172, 377), beside),
        "the plate's line counts"
    );
    const std::array<SourceRect, 1> clear_of{SourceRect{172 + 135 + 1, 377, 20, 20}};
    check(
        !console::entry_plate_covers(console::entry_plate_place(172, 377), clear_of),
        "a pixel past the line is clear"
    );
}

} // namespace

int main() {
    test_mode();
    test_place();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("menu entry: the mode on both sides of the limit, and the plate's place");
    return 0;
}
