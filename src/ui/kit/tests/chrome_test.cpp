// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kit's window chrome, pinned to literal colours, and paint against a
// direct draw.

#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
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

kit::HeaderLook header_look() {
    kit::HeaderLook look;
    look.width = 200;
    look.title = "O";
    look.title_width = 20;
    look.tracking = 1;
    look.second = "S";
    look.second_width = 10;
    look.version = "V";
    look.version_width = 30;
    look.note = "N";
    return look;
}

void header_matches_its_parts() {
    // The header is 36 points high under the top edge, its rule at row 37;
    // the mark is 20 points square at row 9, centred down it, and the texts
    // are centred on the same middle, five rows lower than in 0.7.3's
    // 26-point header.
    renderer::Surface surface = black(210, 48);
    kit::draw_header(canvas_on(surface), header_look());
    expect(surface, 1, 2, 0x12, 0x14, 0x10);
    expect(surface, 0, 2, 0x00, 0x00, 0x00);
    expect(surface, 1, 36, 0x12, 0x14, 0x10);
    expect(surface, 1, 37, 0x2b, 0x30, 0x27);
    // The OA mark in the brand gold.
    expect(surface, 15, 12, 0xd6, 0xa7, 0x47);
    expect(surface, 17, 15, 0x12, 0x14, 0x10);
    expect(surface, 18, 15, 0xd6, 0xa7, 0x47);
    expect(surface, 38, 14, 0xe7, 0xe8, 0xdf);
    expect(surface, 64, 14, 0x95, 0x9c, 0x8b);
    expect(surface, 172, 14, 0xe0, 0xb0, 0x4f);
    expect(surface, 178, 14, 0x12, 0x14, 0x10);
    expect(surface, 183, 14, 0x95, 0x9c, 0x8b);

    // The note ends before the version starts. The second word is the
    // version's colour too, so the search starts right of it.
    int32_t note_right = -1;
    int32_t version_left = 210;
    for (int32_t x = 100; x < 210; ++x) {
        const std::size_t at =
            (static_cast<std::size_t>(14) * 210 + static_cast<std::size_t>(x)) * 3;
        const bool note =
            surface.rgb[at] == 0xe0 && surface.rgb[at + 1] == 0xb0 && surface.rgb[at + 2] == 0x4f;
        const bool version =
            surface.rgb[at] == 0x95 && surface.rgb[at + 1] == 0x9c && surface.rgb[at + 2] == 0x8b;
        OA_CHECK(!(note && version));
        if (note)
            note_right = x;
        if (version)
            version_left = std::min(version_left, x);
    }
    OA_CHECK(note_right >= 0 && version_left < 210 && note_right < version_left);

    kit::HeaderLook bare;
    bare.width = 200;
    renderer::Surface empty = black(210, 48);
    kit::draw_header(canvas_on(empty), bare);
    expect(empty, 18, 15, 0xd6, 0xa7, 0x47);
    expect(empty, 38, 14, 0x12, 0x14, 0x10);
    expect(empty, 64, 14, 0x12, 0x14, 0x10);
    expect(empty, 172, 14, 0x12, 0x14, 0x10);
    expect(empty, 183, 14, 0x12, 0x14, 0x10);

    renderer::Surface icon_surface = black(210, 48);
    kit::Canvas with_icon = canvas_on(icon_surface);
    with_icon.icon = red_icon();
    kit::draw_header(with_icon, header_look());
    expect(icon_surface, 12, 9, 0xff, 0x00, 0x00);
    expect(icon_surface, 15, 12, 0xff, 0x00, 0x00);
    expect(icon_surface, 31, 28, 0xff, 0x00, 0x00);
    expect(icon_surface, 32, 9, 0x12, 0x14, 0x10);
}

void window_and_footer_match() {
    renderer::Surface face = black(20, 12);
    const kit::Rect whole{0, 0, 20, 10};
    kit::draw_window_face(canvas_on(face), whole);
    kit::draw_window_edge(canvas_on(face), whole);
    expect(face, 0, 0, 0x4f, 0x57, 0x47);
    expect(face, 19, 0, 0x08, 0x09, 0x07);
    expect(face, 0, 9, 0x08, 0x09, 0x07);
    expect(face, 1, 1, 0x1d, 0x21, 0x19);

    // The footer's band is 48 points high under its rule.
    renderer::Surface band = black(130, 70);
    kit::draw_footer_band(canvas_on(band), 120, 10);
    expect(band, 1, 10, 0x2b, 0x30, 0x27);
    expect(band, 0, 11, 0x00, 0x00, 0x00);
    expect(band, 1, 11, 0x12, 0x14, 0x10);
    expect(band, 1, 58, 0x12, 0x14, 0x10);
    expect(band, 118, 11, 0x12, 0x14, 0x10);
    expect(band, 119, 11, 0x00, 0x00, 0x00);
    expect(band, 1, 59, 0x00, 0x00, 0x00);
}

kit::NavLook nav_look() {
    kit::NavLook look;
    look.area = {1, 20, 90, 100};
    look.rule_column = 91;
    look.divider = kit::Rect{5, 78, 80, 1};
    look.entries.push_back({{8, 28, 70, 20}, "M", true, false, false});
    look.entries.push_back({{8, 50, 70, 20}, "G", false, true, false});
    look.entries.push_back({{8, 100, 70, 20}, "S", false, false, true});
    return look;
}

void nav_matches_its_entries() {
    renderer::Surface surface = black(120, 140);
    kit::draw_nav(canvas_on(surface), nav_look());
    expect(surface, 2, 21, 0x17, 0x1a, 0x15);
    expect(surface, 91, 21, 0x2b, 0x30, 0x27);
    expect(surface, 5, 78, 0x2b, 0x30, 0x27);
    // The selected and the hovered entries are rounded: 4 points on a
    // 20-point entry, their corner pixel the list's face.
    expect(surface, 8, 28, 0x17, 0x1a, 0x15);
    expect(surface, 9, 29, 0x2c, 0x32, 0x27);
    expect(surface, 13, 34, 0x9c, 0xcc, 0x3c);
    expect(surface, 20, 33, 0xe7, 0xe8, 0xdf);
    expect(surface, 9, 51, 0x25, 0x2a, 0x21);
    expect(surface, 20, 55, 0xe7, 0xe8, 0xdf);
    // The focused entry's ring: two points of the light accent inside it.
    expect(surface, 8, 100, 0x17, 0x1a, 0x15);
    expect(surface, 40, 100, 0xb6, 0xe0, 0x5a);
    expect(surface, 40, 101, 0xb6, 0xe0, 0x5a);
    expect(surface, 40, 102, 0x17, 0x1a, 0x15);
    expect(surface, 20, 105, 0xb9, 0xbf, 0xae);
}

kit::RowFrame row_frame() {
    kit::RowFrame row;
    row.top = 10;
    row.left = 4;
    row.width = 100;
    row.label = {4, 18, 40, 16};
    row.label_text = "L";
    row.hints = {{4, 36, 80, 12}, {4, 48, 80, 12}};
    row.hint_texts = {"W", "W"};
    row.hint_notices = {false, true};
    row.hint_clips = {{}, {4, 48, 2, 12}};
    return row;
}

void row_and_fade_match() {
    renderer::Surface surface = black(120, 80);
    kit::draw_row_frame(canvas_on(surface), row_frame());
    expect(surface, 4, 10, 0x2b, 0x30, 0x27);
    expect(surface, 4, 21, 0xe7, 0xe8, 0xdf);
    expect(surface, 5, 37, 0x95, 0x9c, 0x8b);
    expect(surface, 6, 37, 0x95, 0x9c, 0x8b);
    expect(surface, 5, 49, 0xe0, 0xb0, 0x4f);
    expect(surface, 6, 49, 0x00, 0x00, 0x00);
    expect(surface, 8, 49, 0x00, 0x00, 0x00);

    renderer::Surface faded = black(40, 30);
    kit::draw_locked_fade(canvas_on(faded), {8, 8, 10, 6});
    expect(faded, 8, 8, 0x0d, 0x0f, 0x0b);
    expect(faded, 17, 13, 0x0d, 0x0f, 0x0b);
    expect(faded, 7, 8, 0x00, 0x00, 0x00);
    expect(faded, 18, 8, 0x00, 0x00, 0x00);

    renderer::Surface rule = black(30, 16);
    kit::draw_rule(canvas_on(rule), 3, 5, 12);
    expect(rule, 3, 5, 0x2b, 0x30, 0x27);
    expect(rule, 14, 5, 0x2b, 0x30, 0x27);
    expect(rule, 15, 5, 0x00, 0x00, 0x00);
}

kit::Item item_of(kit::Role role, kit::Rect rect, kit::Look look) {
    kit::Item item;
    item.role = role;
    item.rect = rect;
    item.look = std::move(look);
    return item;
}

void paint_matches_the_direct_draw(int32_t scale) {
    const int32_t side = 256 * scale;
    const kit::HeaderLook header = header_look();
    const kit::FooterBandLook band{header.width, 150};
    const kit::NavLook nav = nav_look();
    const kit::HeadingLook heading{"H"};
    const kit::Rect heading_rect{30, 160, 40, 12};
    const kit::RowFrame row = row_frame();
    const kit::Rect fade{4, 170, 8, 6};

    renderer::Surface direct = black(side, side);
    kit::Canvas canvas = canvas_on(direct, scale);
    kit::draw_header(canvas, header);
    kit::draw_footer_band(canvas, band.width, band.rule_row);
    kit::draw_nav(canvas, nav);
    kit::draw_heading(canvas, heading_rect, heading.text);
    kit::draw_row_frame(canvas, row);
    kit::draw_locked_fade(canvas, fade);

    kit::DisplayList list;
    list.items.push_back(item_of(kit::Role::header, {}, header));
    list.items.push_back(item_of(kit::Role::footer_band, {}, band));
    const std::vector<kit::ControlId> ids{1, 2, 3};
    const std::vector<std::string> names{"mods", "graphics", "sound"};
    kit::add_nav(list, nav, ids, names);
    list.items.push_back(item_of(kit::Role::heading, heading_rect, heading));
    list.items.push_back(item_of(kit::Role::row_frame, {}, row));
    list.items.push_back(item_of(kit::Role::locked_fade, fade, {}));

    renderer::Surface painted = black(side, side);
    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);

    OA_CHECK(list.controls.size() == 3);
    OA_CHECK(list.controls[0].kind == kit::ControlKind::tab);
    OA_CHECK(list.controls[0].steps == false);
    OA_CHECK(list.controls[0].checked == true);
    OA_CHECK(list.controls[0].name == "mods");
    OA_CHECK(list.controls[1].name == "graphics");
    OA_CHECK(list.controls[1].checked == false);
    OA_CHECK(list.controls[2].kind == kit::ControlKind::tab);
    OA_CHECK(list.controls[2].steps == false);
    OA_CHECK(list.controls[2].name == "sound");
    OA_CHECK(list.controls[2].text == "S");
}

} // namespace

int main() {
    header_matches_its_parts();
    window_and_footer_match();
    nav_matches_its_entries();
    row_and_fade_match();
    paint_matches_the_direct_draw(1);
    paint_matches_the_direct_draw(2);
    return oa::test::check_exit_status();
}
