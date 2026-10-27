// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kit's controls, pinned to literal colours, the OA button's attention
// dot, and paint against a direct draw. The geometry literals were computed
// with the settings dialog's functions at ecf703fb, the commit this ticket
// started from.

#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/paint/painter.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;
namespace renderer = oa::ui::frontend_renderer;

renderer::Surface black(int32_t width, int32_t height) {
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    return surface;
}

/// The synthetic font from the settings pixel test: glyph b is 2 + b % 4
/// columns by 8 rows, set where (x + y + byte) % 3 is not 0.
kit::Fonts block_fonts() {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 0);
        for (uint16_t y = 0; y < height; ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                if ((static_cast<int>(x) + y + byte) % 3 != 0)
                    coverage[static_cast<std::size_t>(y) * width + x] = 1;
            }
        }
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

kit::Canvas canvas_on(renderer::Surface& surface, int32_t scale = 1) {
    return {&surface, {0, 0, scale, {}}, &fonts(), {}};
}

void expect(
    const renderer::Surface& surface, int32_t x, int32_t y, uint8_t red, uint8_t green, uint8_t blue
) {
    const auto width = static_cast<int32_t>(surface.width);
    const auto height = static_cast<int32_t>(surface.height);
    if (x < 0 || y < 0 || x >= width || y >= height) {
        std::fprintf(stderr, "pixel %d,%d is outside %dx%d\n", x, y, width, height);
        OA_CHECK(false);
        return;
    }
    const std::size_t at =
        (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
    const uint8_t got_red = surface.rgb[at];
    const uint8_t got_green = surface.rgb[at + 1];
    const uint8_t got_blue = surface.rgb[at + 2];
    if (got_red != red || got_green != green || got_blue != blue) {
        std::fprintf(
            stderr,
            "pixel %d,%d is #%02x%02x%02x, expected #%02x%02x%02x\n",
            x,
            y,
            got_red,
            got_green,
            got_blue,
            red,
            green,
            blue
        );
        OA_CHECK(false);
    }
}

void expect(const renderer::Surface& surface, int32_t x, int32_t y, kit::Colour colour) {
    expect(surface, x, y, colour.r, colour.g, colour.b);
}

void same(const renderer::Surface& left, const renderer::Surface& right) {
    if (left.width != right.width || left.height != right.height || left.rgb != right.rgb) {
        const uint32_t width = left.width < right.width ? left.width : right.width;
        const uint32_t height = left.height < right.height ? left.height : right.height;
        bool reported = false;
        for (uint32_t y = 0; y < height && !reported; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * left.width + x) * 3;
                const std::size_t other = (static_cast<std::size_t>(y) * right.width + x) * 3;
                if (at + 2 >= left.rgb.size() || other + 2 >= right.rgb.size() ||
                    left.rgb[at] != right.rgb[other] || left.rgb[at + 1] != right.rgb[other + 1] ||
                    left.rgb[at + 2] != right.rgb[other + 2]) {
                    std::fprintf(stderr, "surfaces differ at %u,%u\n", x, y);
                    reported = true;
                    break;
                }
            }
        }
        if (!reported)
            std::fprintf(
                stderr,
                "surfaces differ in size %ux%u and %ux%u\n",
                left.width,
                left.height,
                right.width,
                right.height
            );
    }
    OA_CHECK(left.width == right.width && left.height == right.height && left.rgb == right.rgb);
}

kit::ButtonLook
button(std::string caption, kit::ButtonStyle style, bool hovered, bool held, bool enabled = true) {
    return {std::move(caption), style, hovered, held, enabled};
}

constexpr kit::Rect kButtonArea{10, 10, 40, 17};

/// Draws a button at kButtonArea and checks its rounded corner left clear,
/// its border on its top edge, its face inside, and its caption.
///
/// @param look the button
/// @param border the border's colour; black where it has none
/// @param face the face's colour; black where it has none
/// @param text the caption's colour
void pin_button(
    const kit::ButtonLook& look, kit::Colour border, kit::Colour face, kit::Colour text
) {
    renderer::Surface surface = black(80, 40);
    kit::draw_button(canvas_on(surface), kButtonArea, look);
    // A 17-point button's corners are 3 points round (control_radius_for).
    OA_CHECK(kit::control_radius_for(kButtonArea.height) == 3);
    expect(surface, 10, 10, 0, 0, 0);
    expect(surface, 30, 10, border);
    expect(surface, 13, 20, face);
    expect(surface, 28, 13, text);
}

void buttons_match_the_six_looks() {
    constexpr kit::Colour none{0, 0, 0};
    namespace colour = kit::colour;
    // The primary button: lime, lighter under the pointer, darker held.
    pin_button(
        button("A", kit::ButtonStyle::accent, false, false),
        colour::accent,
        colour::accent,
        colour::on_accent
    );
    pin_button(
        button("A", kit::ButtonStyle::accent, true, false),
        colour::accent_light,
        colour::accent_light,
        colour::on_accent
    );
    pin_button(
        button("A", kit::ButtonStyle::accent, true, true),
        colour::accent_held,
        colour::accent_held,
        colour::on_accent
    );
    // The secondary button: raised, with the strong edge.
    pin_button(
        button("A", kit::ButtonStyle::plain, false, false),
        colour::strong_edge,
        colour::hover,
        colour::text
    );
    pin_button(
        button("A", kit::ButtonStyle::plain, true, false),
        colour::control_hover,
        colour::list_selected,
        colour::text
    );
    pin_button(
        button("A", kit::ButtonStyle::plain, false, true),
        colour::strong_edge,
        colour::panel,
        colour::text
    );
    pin_button(
        button("A", kit::ButtonStyle::plain, true, true, false),
        colour::control_border,
        none,
        colour::switch_idle
    );
    // The quiet button: its lime caption alone at rest.
    pin_button(button("A", kit::ButtonStyle::quiet, false, false), none, none, colour::accent);
    pin_button(
        button("A", kit::ButtonStyle::quiet, true, false),
        colour::hover,
        colour::hover,
        colour::accent_light
    );
    pin_button(
        button("A", kit::ButtonStyle::quiet, true, true),
        colour::list_selected,
        colour::list_selected,
        colour::accent_light
    );
    pin_button(
        button("A", kit::ButtonStyle::inset, false, false),
        colour::control_border,
        colour::list,
        colour::text
    );
    pin_button(
        button("A", kit::ButtonStyle::inset, true, false),
        colour::control_hover,
        colour::hover,
        colour::text
    );
    pin_button(
        button("A", kit::ButtonStyle::inset, true, true),
        colour::control_hover,
        colour::band,
        colour::text
    );
    // The danger button: outlined in red, tinted under the pointer.
    pin_button(
        button("A", kit::ButtonStyle::danger, false, false), colour::danger, none, colour::danger
    );
    pin_button(
        button("A", kit::ButtonStyle::danger, true, false),
        colour::danger,
        colour::danger_tint,
        colour::danger
    );
    pin_button(
        button("A", kit::ButtonStyle::danger, true, true, false),
        colour::control_border,
        none,
        colour::switch_idle
    );
}

void buttons_match_the_filled_look() {
    // The deep green face, the held accent's edge and white text at rest;
    // a lighter face and the accent's edge under the pointer; a darker face
    // while held; the plain look's disabled colours when it cannot be
    // pressed. Every one keeps its rounded corners: a quarter of the
    // shorter side, 4 points on a 17-point button, so the corner pixel
    // stays as it was.
    OA_CHECK(kit::filled_radius(kButtonArea) == 4);
    OA_CHECK(kit::filled_radius({0, 0, 180, 30}) == 7);
    OA_CHECK(kit::filled_radius({0, 0, 8, 6}) == 2);

    struct Pin {
        bool hovered;
        bool held;
        std::array<uint8_t, 3> edge;
        std::array<uint8_t, 3> face;
    };

    const Pin pins[] = {
        {false, false, {0x8a, 0xb8, 0x30}, {0x3e, 0x52, 0x18}},
        {true, false, {0x9c, 0xcc, 0x3c}, {0x49, 0x60, 0x1c}},
        {true, true, {0x9c, 0xcc, 0x3c}, {0x35, 0x45, 0x14}},
    };
    for (const Pin& pin : pins) {
        renderer::Surface surface = black(80, 40);
        kit::draw_button(
            canvas_on(surface),
            kButtonArea,
            button("A", kit::ButtonStyle::filled, pin.hovered, pin.held)
        );
        expect(surface, 10, 10, 0, 0, 0);
        expect(surface, 49, 26, 0, 0, 0);
        expect(surface, 30, 10, pin.edge[0], pin.edge[1], pin.edge[2]);
        expect(surface, 10, 18, pin.edge[0], pin.edge[1], pin.edge[2]);
        expect(surface, 12, 18, pin.face[0], pin.face[1], pin.face[2]);
        expect(surface, 28, 13, 0xe7, 0xe8, 0xdf);
    }
    renderer::Surface disabled = black(80, 40);
    kit::draw_button(
        canvas_on(disabled), kButtonArea, button("A", kit::ButtonStyle::filled, false, false, false)
    );
    expect(disabled, 10, 10, 0, 0, 0);
    expect(disabled, 30, 10, 0x3a, 0x40, 0x34);
    expect(disabled, 12, 18, 0, 0, 0);
}

void badge_button_places_its_parts() {
    // A fifth of the height pads the left, the gap after the badge and the right.
    const kit::ButtonMarkParts parts = kit::button_mark_parts({10, 10, 120, 20});
    OA_CHECK(parts.mark.x == 14 && parts.mark.y == 14);
    OA_CHECK(parts.mark.width == 12 && parts.mark.height == 12);
    OA_CHECK(parts.caption.x == 30 && parts.caption.y == 10);
    OA_CHECK(parts.caption.width == 96 && parts.caption.height == 20);
    const kit::ButtonMarkParts tall = kit::button_mark_parts({0, 0, 180, 30});
    OA_CHECK(tall.mark.x == 6 && tall.mark.width == 18 && tall.caption.x == 30);
    OA_CHECK(tall.caption.x + tall.caption.width == 174);
    // A button too narrow for the badge and a gap places nothing.
    const kit::ButtonMarkParts narrow = kit::button_mark_parts({0, 0, 20, 20});
    OA_CHECK(narrow.mark.width == 0 && narrow.caption.width == 0);

    // The badge is drawn in its own colours at the left of the filled face.
    renderer::Surface surface = black(160, 40);
    kit::ButtonLook look = button("Mutators", kit::ButtonStyle::filled, false, false);
    look.oa_mark = true;
    kit::draw_button(canvas_on(surface), {10, 10, 120, 20}, look);
    expect(surface, 11, 20, 0x3e, 0x52, 0x18);
    expect(surface, 128, 20, 0x3e, 0x52, 0x18);
    const std::size_t centre = (static_cast<std::size_t>(20) * surface.width + 20) * 3;
    OA_CHECK(
        surface.rgb[centre] != 0x3e || surface.rgb[centre + 1] != 0x52 ||
        surface.rgb[centre + 2] != 0x18
    );
}

void switch_levels_and_slider_match() {
    namespace colour = kit::colour;
    // A 52 by 16 area holds a 42 by 16 track at its right: x 20 to 61.
    OA_CHECK(kit::switch_track({10, 10, 52, 16}).x == 20);
    OA_CHECK(kit::switch_track({10, 10, 52, 16}).width == 42);
    OA_CHECK(kit::switch_track({10, 10, 52, 16}).height == 16);
    OA_CHECK(kit::switch_track({0, 0, 60, 40}).height == 24);
    OA_CHECK(kit::switch_track({0, 0, 60, 40}).y == 8);
    renderer::Surface off = black(80, 40);
    kit::draw_switch(canvas_on(off), {10, 10, 52, 16}, {false, false, false, "OFF", "ON"});
    expect(off, 10, 10, 0, 0, 0);
    expect(off, 20, 10, 0, 0, 0);
    expect(off, 40, 10, colour::control_border);
    expect(off, 40, 12, colour::off_selected);
    // The knob, 12 points round, two points in from the left.
    expect(off, 28, 18, colour::text_secondary);
    expect(off, 54, 18, colour::off_selected);

    renderer::Surface on = black(80, 40);
    kit::draw_switch(canvas_on(on), {10, 10, 52, 16}, {true, false, false, "OFF", "ON"});
    expect(on, 26, 18, colour::accent);
    expect(on, 40, 10, colour::accent);
    expect(on, 54, 18, colour::on_accent);

    renderer::Surface hot = black(80, 40);
    kit::draw_switch(canvas_on(hot), {10, 10, 52, 16}, {true, true, false, "OFF", "ON"});
    expect(hot, 26, 18, colour::accent_light);

    renderer::Surface locked = black(80, 40);
    kit::draw_switch(canvas_on(locked), {10, 10, 52, 16}, {true, true, true, "OFF", "ON"});
    expect(locked, 26, 18, colour::control_hover);
    expect(locked, 54, 18, colour::on_accent);

    const kit::Rect strip{30, 30, 71, 16};
    kit::LevelsLook levels{{"", "", ""}, 23, 0, 2, false, false};
    renderer::Surface levels_surface = black(120, 60);
    kit::draw_levels(canvas_on(levels_surface), strip, levels);
    expect(levels_surface, 30, 30, 0, 0, 0);
    expect(levels_surface, 60, 30, colour::control_border);
    expect(levels_surface, 40, 38, colour::accent);
    expect(levels_surface, 60, 38, colour::well);

    levels.hovered = true;
    renderer::Surface hovered_strip = black(120, 60);
    kit::draw_levels(canvas_on(hovered_strip), strip, levels);
    expect(hovered_strip, 60, 30, colour::control_hover);

    levels.hovered = false;
    levels.locked = true;
    renderer::Surface locked_strip = black(120, 60);
    kit::draw_levels(canvas_on(locked_strip), strip, levels);
    expect(locked_strip, 40, 38, colour::control_hover);

    const kit::Rect wide{10, 40, 100, 14};
    renderer::Surface slider = black(130, 110);
    kit::draw_slider(canvas_on(slider), wide, {2, 5, false, false});
    // The knob at stop 2 of 5: its edge in the dark ink, its face in the text colour.
    expect(slider, 58, 40, colour::on_accent);
    expect(slider, 58, 46, colour::text);
    expect(slider, 13, 52, colour::control_border);
    // The track, filled in the accent to the knob and in the border after it.
    expect(slider, 20, 45, colour::accent);
    expect(slider, 90, 45, colour::control_border);

    renderer::Surface narrow = black(40, 110);
    kit::draw_slider(canvas_on(narrow), {10, 80, 20, 14}, {0, 5, false, false});
    expect(narrow, 13, 92, 0x00, 0x00, 0x00);

    renderer::Surface slider_locked = black(130, 70);
    kit::draw_slider(canvas_on(slider_locked), wide, {2, 5, true, true});
    expect(slider_locked, 58, 46, colour::control_hover);
    expect(slider_locked, 20, 45, colour::control_hover);

    renderer::Surface slider_hot = black(130, 70);
    kit::draw_slider(canvas_on(slider_hot), wide, {2, 5, false, true});
    expect(slider_hot, 58, 46, colour::accent_light);
}

void choice_lock_and_scroll_match() {
    namespace colour = kit::colour;
    const kit::Rect field{40, 20, 80, 16};
    renderer::Surface closed = black(140, 50);
    kit::draw_choice(canvas_on(closed), field, {"Language", false, false});
    // Rounded: its corner left clear, its border on its edge.
    expect(closed, 40, 20, 0, 0, 0);
    expect(closed, 80, 20, colour::control_border);
    expect(closed, 44, 33, colour::well);
    expect(closed, 108, 26, colour::text_secondary);

    renderer::Surface open = black(140, 50);
    kit::draw_choice(canvas_on(open), field, {"Language", false, true});
    expect(open, 80, 20, colour::accent);
    expect(open, 108, 26, colour::accent);

    renderer::Surface hot = black(140, 50);
    kit::draw_choice(canvas_on(hot), field, {"Language", true, false});
    expect(hot, 80, 20, colour::control_hover);

    kit::ChoiceMenuLook menu;
    menu.shown = {"a", "b", "c", "d", "e", "f", "g", "h"};
    menu.first = 2;
    menu.total = 10;
    menu.chosen = 2;
    menu.marked = 4;
    menu.focus_shown = true;
    const kit::Rect menu_rect{20, 46, 80, 130};
    renderer::Surface menu_surface = black(120, 200);
    kit::draw_choice_menu(canvas_on(menu_surface), menu_rect, menu);
    expect(menu_surface, 20, 46, 0, 0, 0);
    expect(menu_surface, 60, 46, colour::control_hover);
    expect(menu_surface, 26, 51, colour::accent);
    expect(menu_surface, 22, 64, colour::list);
    // The marked item's focus ring, two points thick inside it.
    expect(menu_surface, 40, 79, colour::focus);
    expect(menu_surface, 40, 80, colour::focus);
    expect(menu_surface, 40, 82, colour::hover);
    expect(menu_surface, 97, 73, colour::control_hover);

    menu.focus_shown = false;
    renderer::Surface unfocused = black(120, 200);
    kit::draw_choice_menu(canvas_on(unfocused), menu_rect, menu);
    expect(unfocused, 40, 79, colour::hover);

    renderer::Surface lock = black(100, 40);
    kit::draw_lock(canvas_on(lock), {10, 10, 80, 16}, {"LOCK"});
    expect(lock, 65, 14, 0x00, 0x00, 0x00);
    expect(lock, 66, 14, 0xe0, 0xb0, 0x4f);
    expect(lock, 73, 13, 0xe0, 0xb0, 0x4f);

    // The scroll bar: a rounded well and a rounded thumb.
    const kit::Rect well{10, 10, 7, 40};
    const kit::Rect thumb{12, 14, 5, 20};
    renderer::Surface bar = black(30, 60);
    kit::draw_scroll_bar(canvas_on(bar), well, thumb, false);
    expect(bar, 10, 10, 0, 0, 0);
    expect(bar, 13, 40, colour::well);
    expect(bar, 14, 24, colour::control_hover);
    expect(bar, 12, 14, colour::well);

    renderer::Surface hot_bar = black(30, 60);
    kit::draw_scroll_bar(canvas_on(hot_bar), well, thumb, true);
    expect(hot_bar, 14, 24, colour::text_tertiary);
}

renderer::RgbaPicture red_icon() {
    static const std::array<uint8_t, 16> pixels{
        255,
        0,
        0,
        255,
        255,
        0,
        0,
        255,
        255,
        0,
        0,
        255,
        255,
        0,
        0,
        255,
    };
    return {2, 2, pixels};
}

void oa_button_and_mark_match() {
    renderer::Surface idle = black(40, 40);
    kit::draw_oa_button(canvas_on(idle), 32, kit::OaButtonState::idle);
    expect(idle, 0, 0, kit::colour::edge_light);
    expect(idle, 1, 1, kit::colour::panel);
    expect(idle, 6, 6, kit::colour::brand);
    expect(idle, 11, 12, kit::colour::brand);

    renderer::Surface pressed = black(40, 40);
    kit::draw_oa_button(canvas_on(pressed), 32, kit::OaButtonState::pressed);
    expect(pressed, 0, 0, 0x08, 0x09, 0x07);
    expect(pressed, 11, 12, kit::colour::brand_light);

    renderer::Surface small = black(30, 30);
    kit::draw_oa_button(canvas_on(small), 24, kit::OaButtonState::idle);
    expect(small, 4, 4, kit::colour::brand);
    expect(small, 7, 8, kit::colour::panel);
    expect(small, 8, 8, kit::colour::brand);

    renderer::Surface small_hot = black(30, 30);
    kit::draw_oa_button(canvas_on(small_hot), 24, kit::OaButtonState::hovered);
    expect(small_hot, 8, 8, kit::colour::brand_light);
    expect(small_hot, 1, 1, kit::colour::hover);

    renderer::Surface small_held = black(30, 30);
    kit::draw_oa_button(canvas_on(small_held), 24, kit::OaButtonState::pressed);
    expect(small_held, 0, 0, 0x08, 0x09, 0x07);

    kit::Canvas with_icon = canvas_on(idle);
    with_icon.icon = red_icon();
    renderer::Surface icon_idle = black(40, 40);
    with_icon.surface = &icon_idle;
    kit::draw_oa_button(with_icon, 32, kit::OaButtonState::idle);
    expect(icon_idle, 3, 3, 0xff, 0x00, 0x00);

    renderer::Surface icon_hot = black(40, 40);
    with_icon.surface = &icon_hot;
    kit::draw_oa_button(with_icon, 32, kit::OaButtonState::hovered);
    expect(icon_hot, 1, 1, 0x9c, 0xcc, 0x3c);
    expect(icon_hot, 2, 2, kit::colour::hover);
    expect(icon_hot, 3, 3, 0xff, 0x00, 0x00);

    renderer::Surface icon_held = black(40, 40);
    with_icon.surface = &icon_held;
    kit::draw_oa_button(with_icon, 32, kit::OaButtonState::pressed);
    expect(icon_held, 0, 0, 0x08, 0x09, 0x07);
    expect(icon_held, 3, 3, kit::colour::band);
    expect(icon_held, 4, 4, 0xff, 0x00, 0x00);

    renderer::Surface icon_small = black(30, 30);
    with_icon.surface = &icon_small;
    kit::draw_oa_button(with_icon, 24, kit::OaButtonState::hovered);
    expect(icon_small, 1, 1, 0x9c, 0xcc, 0x3c);
    expect(icon_small, 3, 3, 0xff, 0x00, 0x00);

    renderer::Surface mark = black(40, 40);
    kit::draw_oa_mark(canvas_on(mark), 32);
    expect(mark, 0, 0, 0x00, 0x00, 0x00);
    expect(mark, 6, 6, kit::colour::brand);

    renderer::Surface mark_icon = black(40, 40);
    with_icon.surface = &mark_icon;
    kit::draw_oa_mark(with_icon, 32);
    expect(mark_icon, 0, 0, 0xff, 0x00, 0x00);

    renderer::Surface nothing = black(8, 8);
    kit::draw_oa_mark(canvas_on(nothing), 0);
    expect(nothing, 0, 0, 0x00, 0x00, 0x00);
}

/// Tells whether two surfaces of one size differ only inside a rectangle,
/// and do differ there.
///
/// @param left one surface
/// @param right the other
/// @param inside the rectangle, in pixels
/// @return true when every differing pixel lies inside it and at least one does
bool differ_only_inside(
    const renderer::Surface& left, const renderer::Surface& right, const kit::Rect& inside
) {
    if (left.width != right.width || left.height != right.height)
        return false;
    bool differs = false;
    for (uint32_t y = 0; y < left.height; ++y)
        for (uint32_t x = 0; x < left.width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * left.width + x) * 3;
            if (left.rgb[at] == right.rgb[at] && left.rgb[at + 1] == right.rgb[at + 1] &&
                left.rgb[at + 2] == right.rgb[at + 2])
                continue;
            const auto column = static_cast<int32_t>(x);
            const auto row = static_cast<int32_t>(y);
            if (column < inside.x || row < inside.y || column >= inside.x + inside.width ||
                row >= inside.y + inside.height)
                return false;
            differs = true;
        }
    return differs;
}

void oa_button_wears_the_dot() {
    // The dot is 7 points across, 2 in from the top and right edges.
    OA_CHECK(kit::oa_button_dot(32).x == 23 && kit::oa_button_dot(32).y == 2);
    OA_CHECK(kit::oa_button_dot(24).x == 15 && kit::oa_button_dot(24).y == 2);
    OA_CHECK(kit::oa_button_dot(24).width == 7 && kit::oa_button_dot(24).height == 7);
    for (const int32_t side : {24, 32}) {
        for (const auto state :
             {kit::OaButtonState::idle, kit::OaButtonState::hovered, kit::OaButtonState::pressed}) {
            for (const int32_t scale : {1, 2}) {
                for (const bool with_icon : {false, true}) {
                    renderer::Surface plain = black(40 * scale, 40 * scale);
                    renderer::Surface without = black(40 * scale, 40 * scale);
                    renderer::Surface dotted = black(40 * scale, 40 * scale);
                    kit::Canvas canvas = canvas_on(plain, scale);
                    if (with_icon)
                        canvas.icon = red_icon();
                    // Without the dot the button is drawn as it always was.
                    kit::draw_oa_button(canvas, side, state);
                    canvas.surface = &without;
                    kit::draw_oa_button(canvas, side, state, false);
                    same(plain, without);
                    canvas.surface = &dotted;
                    kit::draw_oa_button(canvas, side, state, true);
                    const kit::Rect dot = kit::oa_button_dot(side);
                    OA_CHECK(differ_only_inside(
                        plain,
                        dotted,
                        {dot.x * scale, dot.y * scale, dot.width * scale, dot.height * scale}
                    ));
                    // The ring's top point and the face's middle, at the scale.
                    expect(dotted, (dot.x + 3) * scale, dot.y * scale, 0x3a, 0x40, 0x34);
                    expect(dotted, (dot.x + 1) * scale, (dot.y + 1) * scale, 0x3a, 0x40, 0x34);
                    expect(dotted, (dot.x + 3) * scale, (dot.y + 3) * scale, 0xe0, 0xb0, 0x4f);
                    expect(
                        dotted,
                        (dot.x + 3) * scale + scale - 1,
                        (dot.y + 1) * scale,
                        0xe0,
                        0xb0,
                        0x4f
                    );
                }
            }
        }
    }
    // The display list's look draws the same dot.
    renderer::Surface direct = black(48, 48);
    renderer::Surface painted = black(48, 48);
    kit::Canvas shifted = canvas_on(direct, 1);
    shifted.placement.x = 4;
    shifted.placement.y = 6;
    kit::draw_oa_button(shifted, 32, kit::OaButtonState::hovered, true);
    kit::Item oa;
    oa.role = kit::Role::oa_button;
    oa.rect = {4, 6, 32, 32};
    oa.look = kit::OaButtonLook{32, kit::OaButtonState::hovered, true};
    kit::DisplayList list;
    list.items.push_back(oa);
    kit::paint(canvas_on(painted, 1), list);
    same(direct, painted);
}

void oa_button_wears_the_count() {
    OA_CHECK(kit::oa_button_count_text(7) == "7");
    OA_CHECK(kit::oa_button_count_text(123) == "123");
    OA_CHECK(kit::oa_button_count_text(999) == "999");
    OA_CHECK(kit::oa_button_count_text(1000) == "999+");
    OA_CHECK(kit::oa_button_count_text(10000) == "999+");

    renderer::Surface sample = black(48, 48);
    kit::Canvas measuring = canvas_on(sample, 1);
    const int32_t seven_columns = kit::measure_text(measuring, kit::FontRole::small, "7");
    const int32_t many_columns = kit::measure_text(measuring, kit::FontRole::small, "123");
    const int32_t capped_columns = kit::measure_text(measuring, kit::FontRole::small, "999+");
    const kit::Rect seven = kit::oa_button_badge(48, seven_columns);
    const kit::Rect many = kit::oa_button_badge(48, many_columns);
    const kit::Rect capped = kit::oa_button_badge(48, capped_columns);
    const kit::Rect spinner = kit::oa_button_badge(48, 0);
    OA_CHECK(
        seven.width == kit::oa_button_badge_height && seven.height == kit::oa_button_badge_height
    );
    OA_CHECK(seven.x == 48 - kit::oa_button_badge_inset - seven.width);
    OA_CHECK(seven.y == 48 - kit::oa_button_badge_inset - seven.height);
    OA_CHECK(many.width > seven.width && capped.width >= many.width);
    OA_CHECK(spinner.x == seven.x && spinner.y == seven.y && spinner.width == seven.width);
    OA_CHECK(kit::oa_button_badge(10, 0).width == 0);

    const auto mark_of = [](kit::OaButtonCorner corner, std::size_t count, uint32_t turn) {
        kit::OaButtonMark mark;
        mark.corner = corner;
        mark.count = count;
        mark.turn = turn;
        return mark;
    };
    const auto painted = [&](int32_t scale, const kit::OaButtonMark& mark) {
        renderer::Surface surface = black(64 * scale, 64 * scale);
        kit::Canvas canvas = canvas_on(surface, scale);
        kit::draw_oa_button(canvas, 48, kit::OaButtonState::idle, mark);
        return surface;
    };
    for (const int32_t scale : {1, 2}) {
        const renderer::Surface plain = painted(scale, {});
        const renderer::Surface seven_face =
            painted(scale, mark_of(kit::OaButtonCorner::count, 7, 0));
        const renderer::Surface many_face =
            painted(scale, mark_of(kit::OaButtonCorner::count, 123, 0));
        // Every count above 999 reads "999+" and draws alike.
        const renderer::Surface capped_face =
            painted(scale, mark_of(kit::OaButtonCorner::count, 10000, 0));
        const renderer::Surface over = painted(scale, mark_of(kit::OaButtonCorner::count, 1000, 0));
        const kit::Rect seven_pixels{
            seven.x * scale, seven.y * scale, seven.width * scale, seven.height * scale
        };
        const kit::Rect many_pixels{
            many.x * scale, many.y * scale, many.width * scale, many.height * scale
        };
        OA_CHECK(differ_only_inside(plain, seven_face, seven_pixels));
        OA_CHECK(differ_only_inside(seven_face, many_face, many_pixels));
        OA_CHECK(over.rgb == capped_face.rgb);

        bool saw_band = false;
        bool saw_text = false;
        for (int32_t row = seven_pixels.y; row < seven_pixels.y + seven_pixels.height; ++row) {
            for (int32_t column = seven_pixels.x; column < seven_pixels.x + seven_pixels.width;
                 ++column) {
                const std::size_t at = (static_cast<std::size_t>(row) * seven_face.width +
                                        static_cast<std::size_t>(column)) *
                                       3;
                const uint8_t red = seven_face.rgb[at];
                const uint8_t green = seven_face.rgb[at + 1];
                const uint8_t blue = seven_face.rgb[at + 2];
                if (red == kit::colour::band.r && green == kit::colour::band.g &&
                    blue == kit::colour::band.b)
                    saw_band = true;
                if (red == 0xe7 && green == 0xe8 && blue == 0xdf)
                    saw_text = true;
            }
        }
        OA_CHECK(saw_band && saw_text);

        const renderer::Surface turned =
            painted(scale, mark_of(kit::OaButtonCorner::spinner, 0, 0));
        const renderer::Surface next = painted(scale, mark_of(kit::OaButtonCorner::spinner, 0, 1));
        const kit::Rect spinner_pixels{
            spinner.x * scale, spinner.y * scale, spinner.width * scale, spinner.height * scale
        };
        OA_CHECK(differ_only_inside(plain, turned, spinner_pixels));
        OA_CHECK(differ_only_inside(turned, next, spinner_pixels));

        kit::OaButtonMark dotted = mark_of(kit::OaButtonCorner::count, 7, 0);
        dotted.dot = true;
        const renderer::Surface with_dot = painted(scale, dotted);
        const kit::Rect dot = kit::oa_button_dot(48);
        OA_CHECK(differ_only_inside(
            seven_face,
            with_dot,
            {dot.x * scale, dot.y * scale, dot.width * scale, dot.height * scale}
        ));
    }

    renderer::Surface direct = black(64, 64);
    renderer::Surface listed = black(64, 64);
    kit::draw_oa_button(
        canvas_on(direct, 1),
        48,
        kit::OaButtonState::idle,
        mark_of(kit::OaButtonCorner::count, 123, 0)
    );
    kit::Item oa;
    oa.role = kit::Role::oa_button;
    oa.rect = {0, 0, 48, 48};
    oa.look =
        kit::OaButtonLook{48, kit::OaButtonState::idle, false, kit::OaButtonCorner::count, 123, 0};
    kit::DisplayList list;
    list.items.push_back(oa);
    kit::paint(canvas_on(listed, 1), list);
    same(direct, listed);
}

void clip_hides_what_it_misses() {
    const kit::ButtonLook look = button("A", kit::ButtonStyle::accent, false, false);
    renderer::Surface missed = black(80, 40);
    kit::Canvas canvas = canvas_on(missed);
    kit::draw_button(kit::clipped(canvas, {0, 0, 5, 5}), kButtonArea, look);
    expect(missed, 10, 10, 0x00, 0x00, 0x00);

    // A clip of one point on the button's top edge draws that point alone.
    renderer::Surface edge = black(80, 40);
    kit::draw_button(kit::clipped(canvas_on(edge), {30, 10, 1, 1}), kButtonArea, look);
    expect(edge, 30, 10, 0x9c, 0xcc, 0x3c);
    expect(edge, 31, 11, 0x00, 0x00, 0x00);
}

void paint_matches_the_direct_draw(int32_t scale) {
    const int32_t side = 256 * scale;
    const kit::ButtonLook look = button("A", kit::ButtonStyle::accent, true, false);
    const kit::Rect area = kButtonArea;
    renderer::Surface direct = black(side, side);
    renderer::Surface painted = black(side, side);
    kit::draw_button(canvas_on(direct, scale), area, look);
    kit::DisplayList list;
    kit::add_button(list, area, look, 3, "ok");
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);

    OA_CHECK(list.controls.size() == 1);
    OA_CHECK(list.controls[0].name == "ok");
    OA_CHECK(list.controls[0].kind == kit::ControlKind::button);
    OA_CHECK(list.controls[0].steps == false);
    OA_CHECK(list.controls[0].checked == false);
    OA_CHECK(list.controls[0].enabled == true);

    kit::DisplayList clipped_list;
    kit::add_button(clipped_list, area, look, 3, "ok");
    clipped_list.items[0].clip = {10, 10, 1, 1};
    renderer::Surface clipped_direct = black(side, side);
    renderer::Surface clipped_paint = black(side, side);
    kit::draw_button(kit::clipped(canvas_on(clipped_direct, scale), {10, 10, 1, 1}), area, look);
    kit::paint(canvas_on(clipped_paint, scale), clipped_list);
    same(clipped_direct, clipped_paint);
}

void paint_matches_each_control(int32_t scale) {
    const int32_t side = 400 * scale;
    const kit::SwitchLook toggle{true, false, false, "OFF", "ON"};
    const kit::Rect toggle_rect{10, 40, 52, 16};
    renderer::Surface direct = black(side, side);
    renderer::Surface painted = black(side, side);
    kit::draw_switch(canvas_on(direct, scale), toggle_rect, toggle);
    kit::DisplayList list;
    kit::add_switch(list, toggle_rect, toggle, 4, "sound");
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);
    OA_CHECK(list.controls[0].name == "sound");
    OA_CHECK(list.controls[0].kind == kit::ControlKind::toggle);
    OA_CHECK(list.controls[0].steps == true);
    OA_CHECK(list.controls[0].checked == true);
    OA_CHECK(list.controls[0].text == "ON");

    const kit::LevelsLook levels{{"Off", "2x"}, 23, 0, 2, false, true};
    const kit::Rect strip{30, 70, 48, 16};
    direct = black(side, side);
    painted = black(side, side);
    list = {};
    kit::draw_levels(canvas_on(direct, scale), strip, levels);
    kit::add_levels(list, strip, levels, 5, "alias");
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);
    OA_CHECK(list.controls[0].kind == kit::ControlKind::levels);
    OA_CHECK(list.controls[0].steps == true);
    OA_CHECK(list.controls[0].checked == false);
    OA_CHECK(list.controls[0].enabled == false);
    OA_CHECK(list.controls[0].text == "Off");
    OA_CHECK(list.controls[0].name == "alias");

    const kit::SliderLook slider{2, 5, false, true};
    const kit::Rect track{10, 100, 100, 14};
    direct = black(side, side);
    painted = black(side, side);
    list = {};
    kit::draw_slider(canvas_on(direct, scale), track, slider);
    kit::add_slider(list, track, slider, 6, "volume");
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);
    OA_CHECK(list.controls[0].kind == kit::ControlKind::slider);
    OA_CHECK(list.controls[0].steps == true);
    OA_CHECK(list.controls[0].text == "2");
    OA_CHECK(list.controls[0].name == "volume");

    const kit::ChoiceLook choice{"English", true, false};
    const kit::Rect field{40, 130, 80, 16};
    direct = black(side, side);
    painted = black(side, side);
    list = {};
    kit::draw_choice(canvas_on(direct, scale), field, choice);
    kit::add_choice(list, field, choice, 7, "language");
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);
    OA_CHECK(list.controls[0].kind == kit::ControlKind::choice);
    OA_CHECK(list.controls[0].steps == true);
    OA_CHECK(list.controls[0].checked == false);
    OA_CHECK(list.controls[0].text == "English");
    OA_CHECK(list.controls[0].name == "language");

    const kit::Rect ring{10, 10, 40, 17};
    direct = black(side, side);
    painted = black(side, side);
    list = {};
    kit::draw_focus_ring(canvas_on(direct, scale), ring, true);
    kit::add_focus_ring(list, ring, true);
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);
    OA_CHECK(list.controls.empty());

    const kit::LockLook lock{"LOCK"};
    const kit::Rect lock_area{10, 10, 80, 16};
    direct = black(side, side);
    painted = black(side, side);
    list = {};
    kit::draw_lock(canvas_on(direct, scale), lock_area, lock);
    kit::add_lock(list, lock_area, lock);
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);
    OA_CHECK(list.controls.empty());

    direct = black(side, side);
    painted = black(side, side);
    list = {};
    kit::draw_mark(canvas_on(direct, scale), kit::Mark::padlock, {12, 14}, kit::colour::lock);
    kit::add_mark(list, {12, 14}, kit::Mark::padlock, kit::colour::lock);
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);
    OA_CHECK(list.controls.empty());

    kit::ChoiceMenuLook menu_look;
    menu_look.shown = {"a", "b", "c", "d", "e", "f", "g", "h"};
    menu_look.first = 2;
    menu_look.total = 10;
    menu_look.chosen = 2;
    menu_look.marked = 4;
    menu_look.focus_shown = true;
    const kit::Rect menu{20, 46, 80, 130};
    direct = black(side, side);
    painted = black(side, side);
    kit::Item menu_item;
    menu_item.role = kit::Role::choice_menu;
    menu_item.rect = menu;
    menu_item.look = menu_look;
    list = {};
    list.items.push_back(menu_item);
    kit::draw_choice_menu(canvas_on(direct, scale), menu, menu_look);
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);

    const kit::Rect well{10, 10, 7, 40};
    const kit::Rect thumb{12, 14, 5, 20};
    direct = black(side, side);
    painted = black(side, side);
    kit::Item bar;
    bar.role = kit::Role::scroll_bar;
    bar.rect = well;
    bar.look = kit::ScrollBarLook{thumb, true};
    list = {};
    list.items.push_back(bar);
    kit::draw_scroll_bar(canvas_on(direct, scale), well, thumb, true);
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);

    const kit::Point origin{4, 6};
    direct = black(side, side);
    painted = black(side, side);
    kit::Canvas shifted = canvas_on(direct, scale);
    shifted.placement.x = origin.x * scale;
    shifted.placement.y = origin.y * scale;
    kit::draw_oa_button(shifted, 32, kit::OaButtonState::pressed);
    kit::Item oa;
    oa.role = kit::Role::oa_button;
    oa.rect = {origin.x, origin.y, 32, 32};
    oa.look = kit::OaButtonLook{32, kit::OaButtonState::pressed};
    list = {};
    list.items.push_back(oa);
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);

    direct = black(side, side);
    painted = black(side, side);
    shifted = canvas_on(direct, scale);
    shifted.placement.x = origin.x * scale;
    shifted.placement.y = origin.y * scale;
    shifted.icon = red_icon();
    kit::draw_oa_mark(shifted, 24);
    kit::Item mark;
    mark.role = kit::Role::oa_mark;
    mark.rect = {origin.x, origin.y, 24, 24};
    list = {};
    list.items.push_back(mark);
    kit::Canvas marked = canvas_on(painted, scale);
    marked.icon = red_icon();
    kit::paint(marked, list);
    same(direct, painted);
}

void later_items_cover_earlier_ones() {
    kit::DisplayList list;
    kit::Item first;
    first.role = kit::Role::fill;
    first.rect = {2, 2, 4, 4};
    first.colour = kit::colour::accent;
    kit::Item second = first;
    second.colour = kit::colour::text;
    list.items.push_back(first);
    list.items.push_back(second);
    renderer::Surface surface = black(16, 16);
    kit::paint(canvas_on(surface), list);
    expect(surface, 3, 3, 0xe7, 0xe8, 0xdf);

    list.items = {second, first};
    surface = black(16, 16);
    kit::paint(canvas_on(surface), list);
    expect(surface, 3, 3, 0x9c, 0xcc, 0x3c);
}

void geometry_matches_the_start_commit() {
    const kit::Rect track{10, 20, 100, 14};
    OA_CHECK(kit::knob_column(track, 0, 1) == 13);
    OA_CHECK(kit::knob_column(track, 4, 1) == 13);
    OA_CHECK(kit::knob_column(track, 0, 2) == 13);
    OA_CHECK(kit::knob_column(track, 1, 2) == 106);
    OA_CHECK(kit::knob_column(track, 0, 26) == 13);
    OA_CHECK(kit::knob_column(track, 25, 26) == 106);
    OA_CHECK(kit::knob_column(track, 1, 26) == 17);

    OA_CHECK(kit::stop_at(track, 13, 26) == 0);
    OA_CHECK(kit::stop_at(track, 106, 26) == 25);
    OA_CHECK(kit::stop_at(track, 17, 26) == 1);
    OA_CHECK(kit::stop_at(track, 0, 26) == 0);
    OA_CHECK(kit::stop_at(track, 1000, 26) == 25);
    OA_CHECK(kit::stop_at(track, 50, 1) == 0);
    OA_CHECK(kit::stop_at({10, 20, 7, 14}, 40, 5) == 0);

    const kit::Rect strip{100, 40, 71, 16};
    OA_CHECK(kit::level_at(strip, 3, 23, 100) == 0);
    OA_CHECK(kit::level_at(strip, 3, 23, 101) == 0);
    OA_CHECK(kit::level_at(strip, 3, 23, 124) == 1);
    OA_CHECK(kit::level_at(strip, 3, 23, 147) == 2);
    OA_CHECK(kit::level_at(strip, 3, 23, 200) == 2);
    OA_CHECK(kit::level_at(strip, 3, 23, 0) == 0);
    OA_CHECK(kit::level_at(strip, 0, 23, 124) == 0);

    OA_CHECK(kit::shown_choices(3) == 3);
    OA_CHECK(kit::shown_choices(12) == 8);
    OA_CHECK(kit::shown_choices(0) == 0);
    OA_CHECK(kit::shown_choices(8) == 8);

    const kit::Rect high = kit::choice_menu({200, 80, 200, 16}, 6);
    OA_CHECK(high.x == 200 && high.y == 96 && high.width == 200 && high.height == 98);
    const kit::Rect low = kit::choice_menu({200, 250, 200, 16}, 12);
    OA_CHECK(low.x == 200 && low.y == 120 && low.width == 200 && low.height == 130);
    // A menu over its field stops under the 36-point header and its rule.
    const kit::Rect clamped = kit::choice_menu({200, 150, 200, 16}, 12);
    OA_CHECK(clamped.x == 200 && clamped.y == 38 && clamped.width == 200 && clamped.height == 130);
    // A larger dialog's lower footer line leaves room under the field.
    OA_CHECK(kit::compact_footer_line == 274);
    const kit::Rect roomy = kit::choice_menu({200, 250, 200, 16}, 12, 452);
    OA_CHECK(roomy.x == 200 && roomy.y == 266 && roomy.width == 200 && roomy.height == 130);
    const kit::Rect deep = kit::choice_menu({200, 400, 200, 16}, 12, 452);
    OA_CHECK(deep.y == 270 && deep.height == 130);

    const kit::Rect opened{200, 96, 200, 98};
    const kit::Rect first = kit::choice_item(opened, 0);
    OA_CHECK(first.x == 201 && first.y == 97 && first.width == 198 && first.height == 16);
    const kit::Rect third = kit::choice_item(opened, 2);
    OA_CHECK(third.x == 201 && third.y == 129 && third.width == 198 && third.height == 16);
}

/// Tells whether a rectangle holds a pixel of one colour.
bool region_has(
    const renderer::Surface& surface,
    const kit::Rect& rect,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            if (x < 0 || y < 0 || x >= static_cast<int32_t>(surface.width) ||
                y >= static_cast<int32_t>(surface.height))
                continue;
            const std::size_t at =
                (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
            if (surface.rgb[at] == red && surface.rgb[at + 1] == green &&
                surface.rgb[at + 2] == blue)
                return true;
        }
    }
    return false;
}

/// A divider, an aside and a greyed row are drawn and hit, the focus skips
/// the divider, and the aside stops short of the label.
void a_menu_draws_a_divider_an_aside_and_a_greyed_row() {
    kit::ChoiceMenuLook menu;
    menu.shown = {"English", "AVAILABLE", "Simplified", "Off"};
    menu.items = {
        {},
        {.divider = true},
        {.aside = "4.0 MB", .id = "zh-hans"},
        {.aside = "downloads off", .id = "xx", .choosable = false},
    };
    menu.total = 4;
    menu.chosen = 0;
    menu.marked = 1;
    menu.focus_shown = true;
    const kit::Rect menu_rect{8, 8, 180, 4 * 16 + 2};
    renderer::Surface surface = black(220, 90);
    kit::draw_choice_menu(canvas_on(surface), menu_rect, menu);

    const kit::Rect divider = kit::choice_item(menu_rect, 1);
    OA_CHECK(kit::choice_item_at(menu_rect, divider.x + 20, divider.y + 8) == 1);
    const kit::Rect offered = kit::choice_item(menu_rect, 2);
    OA_CHECK(kit::choice_item_at(menu_rect, offered.x + offered.width - 6, offered.y + 8) == 2);
    const kit::Rect grey = kit::choice_item(menu_rect, 3);
    OA_CHECK(kit::choice_item_at(menu_rect, grey.x + 16, grey.y + 8) == 3);
    OA_CHECK(kit::choice_item_at(menu_rect, menu_rect.x - 1, menu_rect.y + 8) == -1);
    // Down and Up from either side of the divider land on a choice.
    OA_CHECK(kit::choice_mark_toward(menu, 0, 1) == 2);
    OA_CHECK(kit::choice_mark_toward(menu, 2, -1) == 0);
    // The divider is a heading: quiet ink, and no focus ring though it is marked.
    OA_CHECK(region_has(surface, divider, 0x95, 0x9c, 0x8b));
    OA_CHECK(!region_has(surface, divider, 0xb6, 0xe0, 0x5a));
    // A greyed row's text is quiet, including the point a press would take.
    OA_CHECK(region_has(surface, grey, 0x95, 0x9c, 0x8b));
    OA_CHECK(!menu.items[3].choosable);

    kit::ChoiceMenuLook crowded;
    crowded.shown = {"WWWWWWWWWWWWWWWW"};
    crowded.items = {{.aside = "MMMMMMMM"}};
    crowded.total = 1;
    crowded.chosen = -1;
    crowded.marked = -1;
    const kit::Rect narrow{4, 4, 80, 18};
    renderer::Surface clipped_surface = black(100, 30);
    kit::draw_choice_menu(canvas_on(clipped_surface), narrow, crowded);
    const kit::Rect row = kit::choice_item(narrow, 0);
    const int32_t inner_left = row.x + kit::compact_metrics.choice_item_text_inset;
    const int32_t inner_right = row.x + row.width - 4;
    int32_t aside_width = kit::text_width(fonts(), kit::FontRole::regular, "MMMMMMMM");
    constexpr int32_t gap = 8;
    constexpr int32_t min_label = 8;
    int32_t room = inner_right - inner_left - min_label - gap;
    if (room < 1)
        room = 1;
    if (aside_width > room)
        aside_width = room;
    const int32_t aside_left = inner_right - aside_width;
    const int32_t gap_x = aside_left - gap / 2;
    bool gap_clear = true;
    for (int32_t y = row.y; y < row.y + row.height; ++y) {
        const std::size_t at = (static_cast<std::size_t>(y) * clipped_surface.width +
                                static_cast<std::size_t>(gap_x)) *
                               3;
        if (clipped_surface.rgb[at] != 0x17 || clipped_surface.rgb[at + 1] != 0x1a ||
            clipped_surface.rgb[at + 2] != 0x15)
            gap_clear = false;
    }
    OA_CHECK(gap_clear);
    OA_CHECK(region_has(
        clipped_surface,
        {inner_left, row.y, std::max(gap_x - inner_left, int32_t{1}), row.height},
        0xb9,
        0xbf,
        0xae
    ));
    OA_CHECK(
        region_has(clipped_surface, {aside_left, row.y, aside_width, row.height}, 0xb9, 0xbf, 0xae)
    );
}

/// Tells whether a surface holds the accent check. The stroke is thinner than
/// a pixel, so a covered pixel is a share of the accent on black rather than
/// the accent itself. The accent is much greener than the grey arrow.
///
/// @param surface the picture
/// @return true when some pixel is the accent and not the arrow
bool shows_accent(const renderer::Surface& surface) {
    for (std::size_t at = 0; at + 2 < surface.rgb.size(); at += 3) {
        const int red = surface.rgb[at];
        const int green = surface.rgb[at + 1];
        const int blue = surface.rgb[at + 2];
        if (green > red + 16 && green > blue)
            return true;
    }
    return false;
}

/// The refresh mark turns, stays visible while it is disabled and turning,
/// and changes its face while the pointer is over it. Each eighth of the
/// arrow differs, the check is the accent, and the frames between them are
/// neither the arrow nor the check.
void refresh_mark_turns() {
    const auto draw =
        [](uint8_t turn, bool enabled, bool turning, bool hovered, uint16_t check_ms = 0) {
            kit::ButtonLook look;
            look.mark = kit::ButtonMark::refresh;
            look.style = kit::ButtonStyle::quiet;
            look.enabled = enabled;
            look.turning = turning;
            look.turn = turn;
            look.hovered = hovered;
            look.check_ms = check_ms;
            renderer::Surface surface = black(40, 40);
            kit::draw_button(canvas_on(surface), {8, 8, 17, 17}, look);
            return surface;
        };
    const renderer::Surface rest = draw(0, true, false, false);
    std::array<renderer::Surface, 8> places;
    for (uint8_t eighth = 0; eighth < 8; ++eighth)
        places[eighth] = draw(eighth, true, true, false);
    for (uint8_t eighth = 0; eighth < 8; ++eighth)
        OA_CHECK(places[eighth].rgb != places[static_cast<uint8_t>((eighth + 1u) % 8u)].rgb);
    const renderer::Surface turned = draw(4, true, false, false);
    OA_CHECK(rest.rgb != turned.rgb);
    const renderer::Surface idle = draw(0, false, false, false);
    const renderer::Surface spinning = draw(0, false, true, false);
    OA_CHECK(idle.rgb != spinning.rgb);
    const renderer::Surface hover = draw(0, true, false, true);
    OA_CHECK(rest.rgb != hover.rgb);

    // Halfway through the ease, then the held check. The check is the accent.
    const renderer::Surface between = draw(0, true, false, false, 80);
    const renderer::Surface done = draw(0, true, false, false, 400);
    OA_CHECK(between.rgb != rest.rgb);
    OA_CHECK(between.rgb != done.rgb);
    OA_CHECK(done.rgb != rest.rgb);
    OA_CHECK(shows_accent(done));
    OA_CHECK(!shows_accent(rest));
}

/// The close mark is an × on the refresh mark's square face. Hover changes
/// the face. The × is not the refresh arrow.
void close_mark_draws_a_cross() {
    const auto draw = [](kit::ButtonMark mark, bool hovered) {
        kit::ButtonLook look;
        look.mark = mark;
        look.style = kit::ButtonStyle::quiet;
        look.hovered = hovered;
        renderer::Surface surface = black(40, 40);
        kit::draw_button(canvas_on(surface), {8, 8, 17, 17}, look);
        return surface;
    };
    const renderer::Surface rest = draw(kit::ButtonMark::close, false);
    const renderer::Surface hover = draw(kit::ButtonMark::close, true);
    const renderer::Surface refresh = draw(kit::ButtonMark::refresh, false);
    OA_CHECK(rest.rgb != hover.rgb);
    OA_CHECK(rest.rgb != refresh.rgb);
    OA_CHECK(rest.rgb != black(40, 40).rgb);
}

/// The eighth after 0.8 seconds is one place, whether the clock was stepped
/// at 30, 60 or 120 frames a second. The place is the time, not the count of
/// steps. 0.8 seconds is half of one turn, the fourth eighth.
void refresh_turn_follows_time() {
    const auto place_after = [](uint32_t frame_ms) {
        uint32_t elapsed = 0;
        uint8_t place = 0;
        while (elapsed < 800u) {
            const uint32_t step_ms = frame_ms < 800u - elapsed ? frame_ms : 800u - elapsed;
            elapsed += step_ms;
            place = kit::refresh_eighth(elapsed);
        }
        return place;
    };
    const uint8_t at_thirty = place_after(1000u / 30u);
    const uint8_t at_sixty = place_after(1000u / 60u);
    const uint8_t at_one_twenty = place_after(1000u / 120u);
    OA_CHECK(at_thirty == at_sixty);
    OA_CHECK(at_sixty == at_one_twenty);
    OA_CHECK(at_one_twenty == kit::refresh_eighth(800));
    OA_CHECK(kit::refresh_eighth(800) == 4);
    OA_CHECK(kit::refresh_eighth(0) == 0);
    OA_CHECK(kit::refresh_eighth(kit::refresh_revolution_ms - 1u) == 7);
    OA_CHECK(kit::refresh_eighth(kit::refresh_revolution_ms) == 0);
    // The main menu's clock is 30 units a second. 0.8 seconds is 24 units,
    // and one place is 6 of them: the same fourth eighth.
    constexpr uint32_t units_per_place = kit::refresh_revolution_ms * 30u / 1000u / 8u;
    OA_CHECK(units_per_place == 6u);
    OA_CHECK(static_cast<uint8_t>((24u / units_per_place) % 8u) == kit::refresh_eighth(800));
    OA_CHECK(kit::refresh_check_ease(80) != 0);
    OA_CHECK(kit::refresh_check_ease(80) != 255);
    OA_CHECK(kit::refresh_check_ease(400) == 255);
    OA_CHECK(kit::refresh_check_ease(kit::refresh_check_ms) == 0);
}

/// The main menu's window-pixel button is the icon: a clear corner stays
/// clear, the centre is the icon's colour, and hover draws the accent ring
/// along the top edge without filling the corner.
void menu_icon_button_is_the_icon() {
    constexpr int icon_side = 64;
    constexpr int canvas_side = 72;
    std::vector<uint8_t> icon(static_cast<std::size_t>(icon_side * icon_side * 4), 0);
    for (int row = 0; row < icon_side; ++row) {
        for (int column = 0; column < icon_side; ++column) {
            if (column < 8 || row < 8 || column >= icon_side - 8 || row >= icon_side - 8)
                continue;
            uint8_t* pixel =
                icon.data() + (static_cast<std::size_t>(row * icon_side + column) * 4U);
            pixel[0] = 20;
            pixel[1] = 180;
            pixel[2] = 40;
            pixel[3] = 255;
        }
    }
    const auto draw = [&icon](kit::OaButtonState state) {
        paint::Canvas pixels = paint::make_canvas(canvas_side, canvas_side);
        paint::Painter painter(pixels);
        kit::Canvas canvas;
        canvas.smooth.painter = &painter;
        canvas.smooth.scale_percent = 150;
        canvas.icon = {static_cast<uint32_t>(icon_side), static_cast<uint32_t>(icon_side), icon};
        kit::draw_menu_icon_button(canvas, 48, state, false, {});
        return pixels;
    };
    const paint::Canvas resting = draw(kit::OaButtonState::idle);
    const paint::Rgba corner = paint::pixel_at(resting, 0, 0);
    OA_CHECK(corner.a == 0);
    const paint::Rgba centre = paint::pixel_at(resting, canvas_side / 2, canvas_side / 2);
    OA_CHECK(centre.r == 20 && centre.g == 180 && centre.b == 40 && centre.a == 255);
    const paint::Canvas hovered = draw(kit::OaButtonState::hovered);
    const paint::Rgba hovered_corner = paint::pixel_at(hovered, 0, 0);
    OA_CHECK(hovered_corner.a == 0);
    const paint::Rgba ring = paint::pixel_at(hovered, canvas_side / 2, 1);
    OA_CHECK(
        ring.r == kit::colour::accent.r && ring.g == kit::colour::accent.g &&
        ring.b == kit::colour::accent.b && ring.a == 255
    );

    // The 640 picture has no painter. The same icon is filtered into the
    // 48-pixel square and a clear corner stays the picture behind it.
    const auto draw_crisp = [&icon](kit::OaButtonState state, bool focused) {
        renderer::Surface surface = black(48, 48);
        kit::Canvas canvas;
        canvas.surface = &surface;
        canvas.placement = {0, 0, 1};
        canvas.icon = {static_cast<uint32_t>(icon_side), static_cast<uint32_t>(icon_side), icon};
        kit::draw_menu_icon_button(canvas, 48, state, focused, {});
        return surface;
    };
    const renderer::Surface crisp = draw_crisp(kit::OaButtonState::idle, false);
    expect(crisp, 0, 0, 0, 0, 0);
    expect(crisp, 24, 24, 20, 180, 40);
    const renderer::Surface crisp_hover = draw_crisp(kit::OaButtonState::hovered, false);
    expect(crisp_hover, 0, 0, 0, 0, 0);
    expect(crisp_hover, 24, 1, kit::colour::accent.r, kit::colour::accent.g, kit::colour::accent.b);
    const renderer::Surface crisp_focus = draw_crisp(kit::OaButtonState::idle, true);
    expect(crisp_focus, 24, 1, kit::colour::accent.r, kit::colour::accent.g, kit::colour::accent.b);
    const renderer::Surface crisp_held = draw_crisp(kit::OaButtonState::pressed, false);
    expect(
        crisp_held,
        24,
        1,
        kit::colour::accent_held.r,
        kit::colour::accent_held.g,
        kit::colour::accent_held.b
    );
}

} // namespace

int main() {
    buttons_match_the_six_looks();
    buttons_match_the_filled_look();
    badge_button_places_its_parts();
    refresh_mark_turns();
    close_mark_draws_a_cross();
    refresh_turn_follows_time();
    switch_levels_and_slider_match();
    choice_lock_and_scroll_match();
    a_menu_draws_a_divider_an_aside_and_a_greyed_row();
    oa_button_and_mark_match();
    oa_button_wears_the_dot();
    oa_button_wears_the_count();
    clip_hides_what_it_misses();
    paint_matches_the_direct_draw(1);
    paint_matches_the_direct_draw(2);
    paint_matches_each_control(1);
    paint_matches_each_control(2);
    later_items_cover_earlier_ones();
    geometry_matches_the_start_commit();
    menu_icon_button_is_the_icon();
    return oa::test::check_exit_status();
}
