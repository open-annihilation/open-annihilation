// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main menu's picture set against the base game's, with made-up
// pictures and layouts: the share of a band two pictures share, the records
// an overlay's band covers and what they keep, a button that keeps its
// place but draws nothing, and the corners Open Annihilation's buttons sit
// in with the place of a wider button beside them.
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/main_menu_art.hpp"
#include "oa/ui/frontend_renderer/menu_buttons.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace renderer = oa::ui::frontend_renderer;
namespace layout = oa::ui::gui_layout;

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

/// A 640x480 picture of one grey.
///
/// @param grey the red, green and blue of every pixel
/// @return the picture
oa::Image plain_picture(uint8_t grey) {
    oa::Image picture;
    picture.width = 640;
    picture.height = 480;
    picture.rgb.assign(640U * 480U * 3U, grey);
    return picture;
}

/// Changes the first pixels of the band, row by row.
///
/// @param[in,out] picture the picture
/// @param count how many pixels change
/// @param step how far each channel moves
void change_band(oa::Image& picture, std::size_t count, int step) {
    const std::size_t first = static_cast<std::size_t>(renderer::main_menu_band_top) * 640U;
    for (std::size_t pixel = first; pixel < first + count; ++pixel)
        for (std::size_t channel = 0; channel < 3U; ++channel)
            picture.rgb[pixel * 3U + channel] =
                static_cast<uint8_t>(static_cast<int>(picture.rgb[pixel * 3U + channel]) + step);
}

/// Checks the share of the band two pictures differ in, on both sides of a tenth.
void test_compare_menu_art() {
    const oa::Image base = plain_picture(100);
    const uint32_t band = 640U * 102U;
    CHECK(renderer::compare_menu_art(base, base, 378, 102).compared == band);
    CHECK(renderer::compare_menu_art(base, base, 378, 102).per_mille() == 0);
    // A tenth of the band, exactly, and one pixel more.
    oa::Image tenth = base;
    change_band(tenth, band / 10U, 60);
    CHECK(renderer::compare_menu_art(tenth, base, 378, 102).differing == band / 10U);
    CHECK(renderer::compare_menu_art(tenth, base, 378, 102).per_mille() == 100);
    oa::Image past = base;
    change_band(past, band / 10U + 70U, 60);
    CHECK(renderer::compare_menu_art(past, base, 378, 102).per_mille() == 101);
    // A recoloured copy within the tolerance is the same picture.
    oa::Image recoloured = base;
    change_band(recoloured, band, renderer::menu_art_tolerance);
    CHECK(renderer::compare_menu_art(recoloured, base, 378, 102).differing == 0);
    oa::Image beyond = base;
    change_band(beyond, band, renderer::menu_art_tolerance + 1);
    CHECK(renderer::compare_menu_art(beyond, base, 378, 102).per_mille() == 1000);
    // Rows above the band are not read, and pictures of other sizes are not compared.
    oa::Image above = base;
    for (std::size_t pixel = 0; pixel < 640U * 378U; ++pixel)
        above.rgb[pixel * 3U] = 0;
    CHECK(renderer::compare_menu_art(above, base, 378, 102).differing == 0);
    oa::Image small = base;
    small.width = 320;
    CHECK(renderer::compare_menu_art(small, base, 378, 102).compared == 0);
    CHECK(renderer::compare_menu_art(small, base, 378, 102).per_mille() == 1000);
}

/// A button record.
///
/// @param name its name
/// @param x its left column
/// @param y its top row
/// @param width its width
/// @param height its height
/// @param key its quick key
/// @return the record
layout::Gadget
button(std::string name, int16_t x, int16_t y, int16_t width, int16_t height, int8_t key) {
    layout::Gadget result;
    result.common.type = layout::GadgetType::button;
    result.common.name = std::move(name);
    result.common.x = x;
    result.common.y = y;
    result.common.width = width;
    result.common.height = height;
    result.common.active = 1;
    layout::ButtonFields fields;
    fields.quick_key = key;
    fields.text = "Button";
    result.fields = fields;
    return result;
}

/// The quick key of a layout's record.
///
/// @param menu the layout
/// @param index the record
/// @return its quick key
int8_t quick_key_of(const layout::Layout& menu, std::size_t index) {
    return std::get<layout::ButtonFields>(menu.gadgets[index].fields).quick_key;
}

/// Checks which records the band covers and what each keeps.
void test_cover_band() {
    layout::Layout menu;
    layout::Gadget root;
    root.common.name = "MAINMENU";
    root.common.width = 640;
    root.common.height = 480;
    menu.gadgets.push_back(root);
    menu.gadgets.push_back(button("SINGLE", 82, 393, 96, 20, 's'));   // 1: a job, under the band
    menu.gadgets.push_back(button("Hidden", 8, 452, 64, 20, 'h'));    // 2: not a job, under it
    menu.gadgets.push_back(button("Half", 300, 368, 96, 20, 'f'));    // 3: half under it
    menu.gadgets.push_back(button("Above", 300, 200, 96, 20, 'a'));   // 4: clear of it
    menu.gadgets.push_back(button("credits", 280, 440, 80, 40, 'c')); // 5: a job, by another case

    CHECK(renderer::in_main_menu_band(menu.gadgets[1]));
    CHECK(renderer::in_main_menu_band(menu.gadgets[2]));
    CHECK(!renderer::in_main_menu_band(menu.gadgets[3]));
    CHECK(!renderer::in_main_menu_band(menu.gadgets[4]));
    layout::Gadget empty = button("Empty", 10, 400, 0, 0, 0);
    CHECK(!renderer::in_main_menu_band(empty));
    layout::Gadget past_edge = button("Past", 600, 400, 64, 20, 0);
    CHECK(!renderer::in_main_menu_band(past_edge));

    for (std::size_t index = 1; index < menu.gadgets.size(); ++index)
        CHECK(layout::keyboard_focusable(menu.gadgets[index]));
    const std::array<std::string_view, 2> jobs{"SINGLE", "Credits"};
    CHECK(renderer::cover_main_menu_band(menu, jobs) == 1);
    // The hidden record is off for focus and its key; nothing else changes.
    CHECK(!layout::keyboard_focusable(menu.gadgets[2]));
    CHECK(quick_key_of(menu, 2) == 0);
    CHECK(
        (static_cast<uint32_t>(menu.gadgets[2].common.attributes) & layout::attribute::no_focus) !=
        0
    );
    CHECK(menu.gadgets[2].common.active == 1);
    CHECK(layout::keyboard_focusable(menu.gadgets[1]) && quick_key_of(menu, 1) == 's');
    CHECK(layout::keyboard_focusable(menu.gadgets[3]) && quick_key_of(menu, 3) == 'f');
    CHECK(layout::keyboard_focusable(menu.gadgets[4]) && quick_key_of(menu, 4) == 'a');
    CHECK(layout::keyboard_focusable(menu.gadgets[5]) && quick_key_of(menu, 5) == 'c');
    // A second cover finds the same record and leaves it off.
    CHECK(renderer::cover_main_menu_band(menu, jobs) == 1);
    CHECK(!layout::keyboard_focusable(menu.gadgets[2]));
}

/// Reads one pixel's red.
///
/// @param surface the surface
/// @param x the column
/// @param y the row
/// @return its red
uint8_t red_at(const renderer::Surface& surface, uint32_t x, uint32_t y) {
    return surface.rgb[(static_cast<std::size_t>(y) * surface.width + x) * 3U];
}

/// Checks that a button named undrawn keeps its place and draws nothing.
void test_undrawn_button() {
    renderer::MainMenuResources resources;
    resources.background.width = 8;
    resources.background.height = 8;
    resources.background.rgb.assign(8U * 8U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    resources.layout.gadgets.push_back(button("PATCH", 1, 1, 5, 5, 0));
    std::get<layout::ButtonFields>(resources.layout.gadgets.back().fields).text.clear();
    const auto drawn = renderer::render_main_menu(resources);
    CHECK(red_at(drawn, 3, 3) != 99);
    resources.undrawn_buttons.push_back("PATCH");
    const auto undrawn = renderer::render_main_menu(resources);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x)
            CHECK(red_at(undrawn, x, y) == 99);
    CHECK(resources.layout.gadgets.back().common.width == 5);
}

/// Checks a button's resting picture against the art under it.
void test_button_art() {
    renderer::MainMenuResources resources;
    resources.background = plain_picture(100);
    oa::PaletteBytes palette{};
    palette[5U * oa::palette_entry_bytes] = 200;
    resources.background.palette = palette;
    resources.layout.gadgets.push_back(button("Credits", 280, 440, 80, 40, 0));
    // A frame of palette entry 5, (200, 0, 0), with its last column clear.
    oa::formats::gaf::Frame frame;
    frame.width = 80;
    frame.height = 40;
    frame.pixels.assign(80U * 40U, 5);
    frame.coverage.assign(80U * 40U, 1);
    for (std::size_t row = 0; row < 40U; ++row)
        frame.coverage[row * 80U + 79U] = 0;
    oa::formats::gaf::Sequence sequence;
    sequence.name = "Credits";
    sequence.frames.push_back(frame);
    resources.sprites.sequences.push_back(sequence);

    // Art of another colour differs wherever the frame draws.
    const auto on_grey = renderer::compare_button_art(resources, "credits", resources.background);
    CHECK(on_grey.compared == 79U * 40U && on_grey.per_mille() == 1000);
    // Art holding the frame's own picture does not.
    oa::Image matching = plain_picture(100);
    for (std::size_t y = 440; y < 480; ++y)
        for (std::size_t x = 280; x < 360; ++x) {
            matching.rgb[(y * 640U + x) * 3U] = 200;
            matching.rgb[(y * 640U + x) * 3U + 1U] = 0;
            matching.rgb[(y * 640U + x) * 3U + 2U] = 0;
        }
    CHECK(renderer::compare_button_art(resources, "Credits", matching).per_mille() == 0);
    // No such button, or a picture of another size, compares nothing.
    CHECK(renderer::compare_button_art(resources, "Missing", matching).compared == 0);
    oa::Image small = matching;
    small.width = 320;
    CHECK(renderer::compare_button_art(resources, "Credits", small).compared == 0);
}

/// Checks the pair's corners and the place of a wider button beside it.
void test_menu_buttons() {
    using renderer::MenuCorner;
    static_assert(renderer::menu_pair_width == 108);
    static_assert(renderer::menu_corner_named("bottom-right") == MenuCorner::bottom_right);
    static_assert(renderer::menu_corner_named("top-left") == MenuCorner::top_left);
    static_assert(!renderer::menu_corner_named("Top-left").has_value());
    static_assert(!renderer::menu_corner_named("middle").has_value());
    // Where 0.8.14 puts the pair, and where it goes under an overlay.
    constexpr auto bottom_right = renderer::menu_pair_places(MenuCorner::bottom_right);
    static_assert(bottom_right.library.x == 520 && bottom_right.library.y == 420);
    static_assert(bottom_right.settings.x == 580 && bottom_right.settings.y == 420);
    constexpr auto under_overlay =
        renderer::menu_pair_places(renderer::menu_group_corner(MenuCorner::bottom_right, true));
    static_assert(under_overlay.library.x == 520 && under_overlay.library.y == 12);
    static_assert(under_overlay.settings.x == 580 && under_overlay.settings.y == 12);
    static_assert(
        renderer::menu_group_corner(MenuCorner::bottom_left, true) == MenuCorner::top_left
    );
    static_assert(
        renderer::menu_group_corner(MenuCorner::bottom_left, false) == MenuCorner::bottom_left
    );
    constexpr auto top_left = renderer::menu_pair_places(MenuCorner::top_left);
    static_assert(top_left.library.x == 12 && top_left.library.y == 12);
    static_assert(top_left.settings.x == 72 && top_left.settings.y == 12);
    static_assert(renderer::menu_pair_width == 108);
}

} // namespace

int main() {
    test_compare_menu_art();
    test_cover_band();
    test_undrawn_button();
    test_button_art();
    test_menu_buttons();
    if (failures != 0)
        return 1;
    std::puts(
        "main menu art: the band compared, covered records, undrawn buttons, a button's art and "
        "the corners"
    );
    return 0;
}
