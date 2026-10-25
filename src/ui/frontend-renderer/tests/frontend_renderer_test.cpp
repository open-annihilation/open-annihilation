// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/frontend_renderer/scroll_bars.hpp"
#include "oa/data/languages/translation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <tuple>
#include <memory>
#include <variant>
#include <vector>

namespace {

int failures = 0;

// Whether render times compare the drawing code. A build with the address
// sanitizer checks every memory access, which slows the grayed buttons'
// shade-table reads far more than the normal buttons' copies, so its times
// are not compared.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool render_times_compared = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool render_times_compared = false;
#else
constexpr bool render_times_compared = true;
#endif
#else
constexpr bool render_times_compared = true;
#endif

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

oa::ui::gui_layout::Gadget
button(std::string name, int16_t x, int16_t y, int16_t width, int16_t height) {
    oa::ui::gui_layout::Gadget result;
    result.common.type = oa::ui::gui_layout::GadgetType::button;
    result.common.name = std::move(name);
    result.common.x = x;
    result.common.y = y;
    result.common.width = width;
    result.common.height = height;
    result.common.active = 1;
    result.fields = oa::ui::gui_layout::ButtonFields{};
    return result;
}

uint8_t red_at(const oa::ui::frontend_renderer::Surface& surface, std::size_t x, std::size_t y) {
    return surface.rgb[(y * surface.width + x) * 3];
}

// A grayed-out art button shows the frame two past its normal one, or the
// last frame of a shorter sequence, grayed through the palette's gray table
// and darkened through shade table row 12.
void test_grayed_art_frame() {
    oa::ui::frontend_renderer::ScreenResources resources;
    resources.background.width = 4;
    resources.background.height = 4;
    resources.background.rgb.assign(4U * 4U * 3U, 99);
    // A gray ramp, each entry its own gray, but for one red entry.
    for (std::size_t index = 0; index < 256; ++index)
        for (std::size_t channel = 0; channel < 3; ++channel)
            resources.gui_palette[index * 4 + channel] = static_cast<uint8_t>(index);
    resources.gui_palette[200 * 4] = 60;
    resources.gui_palette[200 * 4 + 1] = 0;
    resources.gui_palette[200 * 4 + 2] = 0;
    // The screen's bitmap carries the palette the shade table indexes.
    resources.background.palette = resources.gui_palette;
    resources.layout.gadgets.push_back(button("ART", 1, 1, 1, 1));
    oa::formats::gaf::Sequence sequence;
    sequence.name = "ART";
    for (uint8_t value : {uint8_t{10}, uint8_t{11}, uint8_t{12}, uint8_t{13}}) {
        oa::formats::gaf::Frame frame;
        frame.width = 1;
        frame.height = 1;
        frame.pixels = {value};
        frame.coverage = {1};
        sequence.frames.push_back(frame);
    }
    resources.sprites.sequences.push_back(sequence);
    resources.shade_table.resize(32U * 256U);
    for (std::size_t row = 0; row < 32; ++row)
        for (std::size_t index = 0; index < 256; ++index)
            resources.shade_table[row * 256 + index] = static_cast<uint8_t>(index);
    resources.shade_table[12U * 256U + 12U] = 50;
    const oa::ui::frontend_renderer::ButtonPresentation grayed{
        "ART",
        oa::ui::frontend_renderer::ButtonCondition::disabled,
        std::nullopt,
        std::nullopt,
        std::nullopt
    };
    CHECK(red_at(oa::ui::frontend_renderer::render_screen(resources, {&grayed, 1}), 1, 1) == 50);
    resources.sprites.sequences.back().frames.resize(2);
    CHECK(red_at(oa::ui::frontend_renderer::render_screen(resources, {&grayed, 1}), 1, 1) == 11);
    // Drawn in the GUI palette alone, the grayed frame is darkened in the game
    // palette, and left undarkened without one.
    resources.sprites.sequences.back().frames.push_back(
        resources.sprites.sequences.back().frames[0]
    );
    resources.sprites.sequences.back().frames.back().pixels = {12};
    resources.background.palette.reset();
    resources.game_palette = resources.gui_palette;
    CHECK(red_at(oa::ui::frontend_renderer::render_screen(resources, {&grayed, 1}), 1, 1) == 50);
    resources.game_palette.reset();
    CHECK(red_at(oa::ui::frontend_renderer::render_screen(resources, {&grayed, 1}), 1, 1) == 12);
    // A coloured entry is darkened as its gray: (60, 0, 0) as entry 20.
    resources.background.palette = resources.gui_palette;
    resources.sprites.sequences.back().frames.back().pixels = {200};
    resources.shade_table[12U * 256U + 20U] = 77;
    CHECK(red_at(oa::ui::frontend_renderer::render_screen(resources, {&grayed, 1}), 1, 1) == 77);
}

// An art button whose authored rectangle is smaller than its frame, as
// NEWGAME's captionless Side0 is, draws the whole frame at its own size from
// the gadget's corner.
void test_art_keeps_frame_size() {
    oa::ui::frontend_renderer::ScreenResources resources;
    resources.background.width = 5;
    resources.background.height = 5;
    resources.background.rgb.assign(5U * 5U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    resources.background.palette = resources.gui_palette;
    auto side = button("Side0", 1, 1, 2, 2);
    side.common.attributes = static_cast<int32_t>(
        oa::ui::gui_layout::attribute::centered | oa::ui::gui_layout::attribute::text_list |
        oa::ui::gui_layout::attribute::no_focus
    );
    resources.layout.gadgets.push_back(side);
    oa::formats::gaf::Sequence sequence;
    sequence.name = "Side0";
    oa::formats::gaf::Frame frame;
    frame.width = 3;
    frame.height = 3;
    frame.pixels = {10, 11, 12, 13, 14, 15, 16, 17, 18};
    frame.coverage.assign(9, 1);
    sequence.frames.push_back(frame);
    resources.sprites.sequences.push_back(sequence);
    const auto drawn = oa::ui::frontend_renderer::render_screen(resources, {});
    CHECK(red_at(drawn, 1, 1) == 10);
    CHECK(red_at(drawn, 2, 2) == 14);
    CHECK(red_at(drawn, 3, 3) == 18);
    CHECK(red_at(drawn, 4, 4) == 99);
}

// An in-game panel holds commongui.gaf in its own archive and the side's
// interface art shared (ARMOPT's buttons): a button without art of its own
// draws BUTTONS0's frames from there. Off the pointer and under it, it shows
// its normal face, frame 0 of its size group; pressed, frame 1. The frame's
// top-left is on the record's position whatever origin the frame carries.
void test_default_art_in_panel_archive() {
    namespace renderer = oa::ui::frontend_renderer;
    renderer::ScreenResources resources;
    resources.background.width = 6;
    resources.background.height = 6;
    resources.background.rgb.assign(6U * 6U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    resources.layout.gadgets.push_back(button("OK", 2, 3, 2, 1));
    oa::formats::gaf::Sequence buttons;
    buttons.name = "BUTTONS0";
    for (uint8_t value : {uint8_t{40}, uint8_t{41}, uint8_t{42}, uint8_t{43}}) {
        oa::formats::gaf::Frame frame;
        frame.width = 2;
        frame.height = 1;
        frame.origin_x = 304;
        frame.origin_y = 207;
        frame.pixels = {value, value};
        frame.coverage = {1, 1};
        buttons.frames.push_back(frame);
    }
    resources.sprites.sequences.push_back(buttons);
    oa::formats::gaf::Sequence chrome;
    chrome.name = "PANELSIDE";
    chrome.frames.push_back(buttons.frames.front());
    resources.shared_sprites.sequences.push_back(chrome);
    const auto shown = [&](renderer::ButtonCondition condition) {
        const renderer::ButtonPresentation state{
            "OK", condition, std::nullopt, std::nullopt, std::nullopt
        };
        return renderer::render_screen(resources, {&state, 1});
    };
    const auto normal = shown(renderer::ButtonCondition::normal);
    CHECK(red_at(normal, 2, 3) == 40);
    CHECK(red_at(normal, 3, 3) == 40);
    CHECK(red_at(normal, 1, 3) == 99);
    CHECK(red_at(normal, 4, 3) == 99);
    CHECK(red_at(normal, 2, 2) == 99);
    CHECK(red_at(normal, 2, 4) == 99);
    CHECK(shown(renderer::ButtonCondition::hovered).rgb == normal.rgb);
    CHECK(red_at(shown(renderer::ButtonCondition::pressed), 2, 3) == 41);
}

// NEWGAME's Arm and Core are hold buttons (text_list) with no sequence of
// their own. They take BUTTONS0's size group: the normal frame while up,
// the next frame while down. A checkbox still has no default frame.
void test_hold_button_takes_default_art() {
    namespace renderer = oa::ui::frontend_renderer;
    renderer::ScreenResources resources;
    resources.background.width = 16;
    resources.background.height = 8;
    resources.background.rgb.assign(16U * 8U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    auto arm = button("Arm", 1, 1, 10, 4);
    arm.common.attributes = static_cast<int32_t>(
        oa::ui::gui_layout::attribute::centered | oa::ui::gui_layout::attribute::text_list
    );
    resources.layout.gadgets.push_back(arm);
    auto box = button("Check", 1, 1, 10, 4);
    box.common.attributes = static_cast<int32_t>(oa::ui::gui_layout::attribute::checkbox);
    oa::formats::gaf::Sequence buttons;
    buttons.name = "BUTTONS0";
    const auto add_group = [&](int width, int height, uint8_t normal, uint8_t pressed) {
        for (int frame_index = 0; frame_index < 4; ++frame_index) {
            oa::formats::gaf::Frame frame;
            frame.width = static_cast<uint16_t>(width);
            frame.height = static_cast<uint16_t>(height);
            const uint8_t value = frame_index == 1 ? pressed : normal;
            frame.pixels.assign(static_cast<std::size_t>(width * height), value);
            frame.coverage.assign(frame.pixels.size(), 1);
            buttons.frames.push_back(std::move(frame));
        }
    };
    add_group(2, 2, 10, 11);
    add_group(10, 4, 30, 31);
    resources.shared_sprites.sequences.push_back(buttons);
    const auto shown = [&](renderer::ButtonCondition condition) {
        const renderer::ButtonPresentation state{
            "Arm", condition, std::nullopt, std::nullopt, std::nullopt
        };
        return renderer::render_screen(resources, {&state, 1});
    };
    CHECK(red_at(shown(renderer::ButtonCondition::normal), 1, 1) == 30);
    CHECK(red_at(shown(renderer::ButtonCondition::pressed), 1, 1) == 31);
    resources.layout.gadgets.back() = box;
    const auto drawn = renderer::render_screen(resources, {});
    // The checkbox draws its own bevel, never BUTTONS0's frame.
    CHECK(red_at(drawn, 6, 3) != 30 && red_at(drawn, 6, 3) != 31);
}

// A centred caption underlines its quick key's glyph on the row below the
// text, in GUI palette entry 2, or 0 while pressed, and not while grayed;
// the focused record gets the focus marker's six rings, lit through the
// light table, corners twice, inside the root.
void test_quick_key_and_focus() {
    namespace renderer = oa::ui::frontend_renderer;
    constexpr uint8_t ground = 99;
    constexpr uint8_t glyph_colour = 7;
    constexpr uint8_t underline_entry = 2;
    constexpr uint8_t button_fill = 20;
    constexpr uint8_t grayed_fill = 19;
    renderer::ScreenResources resources;
    resources.background.width = 60;
    resources.background.height = 30;
    resources.background.rgb.assign(60U * 30U * 3U, 0);
    for (std::size_t pixel = 0; pixel < 60U * 30U; ++pixel)
        resources.background.rgb[pixel * 3] = ground;
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    resources.background.palette = resources.gui_palette;
    // Every light level adds itself to the entry, so a pixel lit twice shows it.
    resources.light_table.resize(32U * 256U);
    for (std::size_t level = 0; level < 32; ++level)
        for (std::size_t index = 0; index < 256; ++index)
            resources.light_table[level * 256 + index] = static_cast<uint8_t>(index + level);
    // Glyphs 3 pixels wide and 4 high: the text is 'I' + 2 = 6 high.
    oa::formats::fnt::Glyph glyph;
    glyph.width = 3;
    glyph.height = 4;
    glyph.pixels.assign(12, glyph_colour);
    glyph.coverage.assign(12, 1);
    for (const char character : {'I', 'Y', 'e', 's'})
        resources.font.glyphs[static_cast<unsigned char>(character)] = glyph;
    oa::ui::gui_layout::Gadget root;
    root.common.type = oa::ui::gui_layout::GadgetType::panel;
    root.common.name = "ROOT";
    root.common.y = 2;
    root.common.width = 60;
    root.common.height = 26;
    root.common.active = 1;
    auto yes = button("YES", 10, 5, 20, 10);
    yes.common.attributes = 2; // centred caption
    std::get<oa::ui::gui_layout::ButtonFields>(yes.fields).text = "Yes";
    resources.layout.gadgets = {root, yes};
    renderer::ButtonPresentation state{
        "YES", renderer::ButtonCondition::normal, std::nullopt, std::nullopt, std::nullopt
    };
    state.quick_key = 'Y';
    // "Yes" is 9 wide: x = 10 + (20 - 1 - 9) / 2 + 1 = 16,
    // y = 5 + (10 - 1 - 6) / 2 = 6, so the underline lies on row 6 + 6 - 1.
    auto drawn = renderer::render_screen(resources, {&state, 1});
    CHECK(red_at(drawn, 16, 11) == underline_entry && red_at(drawn, 18, 11) == underline_entry);
    CHECK(red_at(drawn, 15, 11) == button_fill && red_at(drawn, 19, 11) == button_fill);
    CHECK(red_at(drawn, 16, 10) == button_fill);
    state.quick_key = 's';
    drawn = renderer::render_screen(resources, {&state, 1});
    CHECK(red_at(drawn, 22, 11) == underline_entry && red_at(drawn, 16, 11) == button_fill);
    // A key the caption does not hold in that case underlines nothing.
    state.quick_key = 'y';
    drawn = renderer::render_screen(resources, {&state, 1});
    CHECK(red_at(drawn, 16, 11) == button_fill);
    state.quick_key = 'Y';
    state.condition = renderer::ButtonCondition::pressed;
    drawn = renderer::render_screen(resources, {&state, 1});
    // Pressed, the caption moves a pixel right and down with its underline.
    CHECK(red_at(drawn, 17, 12) == 0 && red_at(drawn, 19, 12) == 0);
    state.condition = renderer::ButtonCondition::disabled;
    drawn = renderer::render_screen(resources, {&state, 1});
    CHECK(red_at(drawn, 16, 11) == grayed_fill);
    CHECK(red_at(drawn, 15, 4) == ground);

    state.condition = renderer::ButtonCondition::normal;
    state.focused = true;
    drawn = renderer::render_screen(resources, {&state, 1});
    // Ring 1 (level 31) lies a pixel outside the record, ring 2 (28) two.
    CHECK(red_at(drawn, 15, 4) == ground + 31);
    CHECK(red_at(drawn, 9, 10) == ground + 31);
    CHECK(red_at(drawn, 9, 4) == ground + 2 * 31);
    CHECK(red_at(drawn, 15, 3) == ground + 28);
    CHECK(red_at(drawn, 8, 3) == ground + 2 * 28);
    // Ring 3 (24) lies on the root's top row; ring 4 above it is left out.
    CHECK(red_at(drawn, 15, 2) == ground + 24);
    CHECK(red_at(drawn, 15, 1) == ground);
    // Rings 4 to 6 are lit where the root holds them.
    CHECK(red_at(drawn, 6, 10) == ground + 19);
    CHECK(red_at(drawn, 5, 10) == ground + 13);
    CHECK(red_at(drawn, 4, 10) == ground + 6);
    CHECK(red_at(drawn, 3, 10) == ground);
}

// A side past its art draws the raised bevel and the side's name, and none
// of the sequence named after the button. Without the flag the same button
// draws that sequence's frame.
void test_caption_only_skips_art() {
    namespace renderer = oa::ui::frontend_renderer;
    constexpr uint8_t glyph_colour = 7;
    constexpr uint8_t art_frame = 40;
    constexpr uint8_t art_next = 41;
    constexpr int16_t origin = 5;
    constexpr int16_t width = 40;
    constexpr int16_t height = 16;
    renderer::ScreenResources resources;
    resources.background.width = 50;
    resources.background.height = 30;
    resources.background.rgb.assign(50U * 30U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    resources.background.palette = resources.gui_palette;
    oa::formats::fnt::Glyph glyph;
    glyph.width = 3;
    glyph.height = 4;
    glyph.pixels.assign(12, glyph_colour);
    glyph.coverage.assign(12, 1);
    for (const char character : {'R', 'i', 'd', 'g', 'e', '6'})
        resources.font.glyphs[static_cast<unsigned char>(character)] = glyph;
    auto side = button("SIDE", origin, origin, width, height);
    side.common.attributes = 2; // centred caption
    auto& fields = std::get<oa::ui::gui_layout::ButtonFields>(side.fields);
    fields.stages = 2;
    fields.text = "Ridge6";
    resources.layout.gadgets.push_back(side);
    oa::formats::gaf::Sequence sequence;
    sequence.name = "SIDE";
    for (const uint8_t value : {art_frame, art_next}) {
        oa::formats::gaf::Frame frame;
        frame.width = static_cast<uint16_t>(width);
        frame.height = static_cast<uint16_t>(height);
        frame.pixels.assign(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height), value
        );
        frame.coverage.assign(frame.pixels.size(), 1);
        sequence.frames.push_back(std::move(frame));
    }
    resources.sprites.sequences.push_back(std::move(sequence));
    renderer::ButtonPresentation state{
        "SIDE", renderer::ButtonCondition::normal, std::nullopt, std::nullopt, std::nullopt
    };
    const auto art = renderer::render_screen(resources, {&state, 1});
    CHECK(red_at(art, origin, origin) == art_frame);
    CHECK(
        red_at(
            art,
            static_cast<std::size_t>(origin + width - 1),
            static_cast<std::size_t>(origin + height - 1)
        ) == art_frame
    );
    state.caption_only = true;
    const auto plain = renderer::render_screen(resources, {&state, 1});
    // Raised bevel: map 17 on the top and left, map 0 on the bottom and right,
    // fill 20 inside. "Ridge6" is 18 wide and 6 high, so its first glyph is at
    // x = 5 + (39 - 18) / 2 + 1 = 16, y = 5 + (15 - 6) / 2 = 9.
    CHECK(red_at(plain, origin, origin) == 17);
    CHECK(
        red_at(
            plain,
            static_cast<std::size_t>(origin + width - 1),
            static_cast<std::size_t>(origin + height - 1)
        ) == 0
    );
    CHECK(red_at(plain, 6, 6) == 20);
    CHECK(red_at(plain, 16, 9) == glyph_colour);
    bool art_pixel = false;
    for (std::size_t y = 0; y < plain.height && !art_pixel; ++y) {
        for (std::size_t x = 0; x < plain.width; ++x) {
            const uint8_t red = red_at(plain, x, y);
            if (red == art_frame || red == art_next)
                art_pixel = true;
        }
    }
    CHECK(!art_pixel);
}

// A caption is placed within the button's inclusive rectangle, whose far
// edges are a pixel inside its width and height: centred, it starts
// (width - 1 - text width) / 2 + 1 across and (height - 1 - text height) / 2
// down, halves rounded toward zero; right-aligned, it ends 3 pixels before
// the far edge; left-aligned, it starts 3 pixels in, centred down.
void test_caption_placement() {
    namespace renderer = oa::ui::frontend_renderer;
    constexpr uint8_t glyph_colour = 7;
    constexpr uint8_t button_fill = 20;
    constexpr int16_t origin = 5;
    renderer::ScreenResources resources;
    resources.background.width = 40;
    resources.background.height = 40;
    resources.background.rgb.assign(40U * 40U * 3U, 0);
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    resources.background.palette = resources.gui_palette;
    // Glyphs 3 pixels wide and 4 high: "AA" is 6 wide and the text is 6 high.
    oa::formats::fnt::Glyph glyph;
    glyph.width = 3;
    glyph.height = 4;
    glyph.pixels.assign(12, glyph_colour);
    glyph.coverage.assign(12, 1);
    for (const char character : {'I', 'A'})
        resources.font.glyphs[static_cast<unsigned char>(character)] = glyph;
    const renderer::ButtonPresentation state{
        "CAPTION", renderer::ButtonCondition::normal, std::nullopt, std::nullopt, std::nullopt
    };
    // Returns whether the caption's top-left glyph pixel lies at (x, y), with
    // the button's fill left of it and above it.
    const auto caption_at = [&](int16_t width, int16_t height, uint32_t attributes, int x, int y) {
        auto caption = button("CAPTION", origin, origin, width, height);
        caption.common.attributes = static_cast<int32_t>(attributes);
        std::get<oa::ui::gui_layout::ButtonFields>(caption.fields).text = "AA";
        resources.layout.gadgets = {caption};
        const auto drawn = renderer::render_screen(resources, {&state, 1});
        const auto ux = static_cast<std::size_t>(x);
        const auto uy = static_cast<std::size_t>(y);
        return red_at(drawn, ux, uy) == glyph_colour && red_at(drawn, ux - 1, uy) == button_fill &&
               red_at(drawn, ux, uy - 1) == button_fill;
    };
    constexpr uint32_t left = 1U;
    constexpr uint32_t centred = 2U;
    constexpr uint32_t right = 4U;
    // Even differences (20 - 6): x = 5 + (19 - 6) / 2 + 1 = 12, y = 5 + 6 = 11.
    CHECK(caption_at(20, 20, centred, 12, 11));
    // Odd differences (21 - 6): x = 5 + (20 - 6) / 2 + 1 = 13, y = 5 + 7 = 12.
    CHECK(caption_at(21, 21, centred, 13, 12));
    // Right-aligned: x = 5 + 19 - 6 - 3 = 15, or 5 + 20 - 6 - 3 = 16.
    CHECK(caption_at(20, 20, right, 15, 11));
    CHECK(caption_at(21, 21, right, 16, 12));
    // Left-aligned: x = 5 + 3 whatever the width.
    CHECK(caption_at(20, 20, left, 8, 11));
    CHECK(caption_at(21, 21, left, 8, 12));
}

// A caption wider than its button less 6 pixels loses characters from its
// end until it is no wider, and is then placed as a caption that fits:
// centred, its start shows inside the button and nothing is drawn past the
// button's sides. A caption that fits is drawn whole.
void test_caption_cut_to_button() {
    namespace renderer = oa::ui::frontend_renderer;
    constexpr uint8_t ground = 99;
    constexpr uint8_t glyph_colour = 7;
    constexpr uint8_t button_fill = 20;
    constexpr int16_t left_edge = 5;
    constexpr int16_t button_width = 20;
    constexpr std::size_t screen_width = 40;
    constexpr std::size_t screen_height = 20;
    renderer::ScreenResources resources;
    resources.background.width = screen_width;
    resources.background.height = screen_height;
    resources.background.rgb.assign(screen_width * screen_height * 3U, 0);
    for (std::size_t pixel = 0; pixel < screen_width * screen_height; ++pixel)
        resources.background.rgb[pixel * 3] = ground;
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    resources.background.palette = resources.gui_palette;
    // Glyphs 3 pixels wide and 4 high: the text is 6 high.
    oa::formats::fnt::Glyph glyph;
    glyph.width = 3;
    glyph.height = 4;
    glyph.pixels.assign(12, glyph_colour);
    glyph.coverage.assign(12, 1);
    for (const char character : {'I', 'A', 'B'})
        resources.font.glyphs[static_cast<unsigned char>(character)] = glyph;
    const renderer::ButtonPresentation state{
        "CAPTION", renderer::ButtonCondition::normal, std::nullopt, std::nullopt, std::nullopt
    };
    const auto drawn_with = [&](const char* text, int16_t width) {
        auto caption = button("CAPTION", left_edge, left_edge, width, 10);
        caption.common.attributes = 2; // centred caption
        std::get<oa::ui::gui_layout::ButtonFields>(caption.fields).text = text;
        resources.layout.gadgets = {caption};
        return renderer::render_screen(resources, {&state, 1});
    };
    // A row of the glyphs, which start at 5 + (10 - 1 - 6) / 2 = 6, below the
    // button's bevel.
    constexpr std::size_t row = 7;
    // "ABABABAB" is 24 wide; cut to 14 or less it is "ABAB", 12 wide, at
    // x = 5 + (19 - 12) / 2 + 1 = 9 to 20.
    auto drawn = drawn_with("ABABABAB", button_width);
    CHECK(red_at(drawn, 8, row) == button_fill && red_at(drawn, 9, row) == glyph_colour);
    CHECK(red_at(drawn, 20, row) == glyph_colour && red_at(drawn, 21, row) == button_fill);
    for (std::size_t x = 0; x < screen_width; ++x)
        if (x < static_cast<std::size_t>(left_edge) ||
            x >= static_cast<std::size_t>(left_edge + button_width))
            CHECK(red_at(drawn, x, row) == ground);
    // A caption exactly as wide as the button less 6 is drawn whole: "ABAB"
    // in a button 18 wide, at x = 5 + (17 - 12) / 2 + 1 = 8 to 19; one more
    // character is cut off.
    drawn = drawn_with("ABAB", 18);
    CHECK(red_at(drawn, 8, row) == glyph_colour && red_at(drawn, 19, row) == glyph_colour);
    const auto cut = drawn_with("ABABA", 18);
    CHECK(cut.rgb == drawn.rgb);
}

/// What the modern fonts were last asked to draw, and at what size.
struct ModernDraws {
    std::vector<std::pair<std::string, int32_t>> asked{};
};

/// Counts a UTF-8 text's characters.
///
/// @param text the text
/// @return its characters
int32_t characters_of(std::string_view text) {
    int32_t count = 0;
    for (const char byte : text)
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U)
            ++count;
    return count;
}

// A caption the modern fonts draw that is wider than its button less 6
// pixels is drawn smaller, a step of 5 at a time, until it fits, and keeps
// every character; one that fits is drawn at the game font's size, and a
// caption the game font draws still loses characters from its end.
void test_caption_shrinks_to_button() {
    namespace renderer = oa::ui::frontend_renderer;
    ModernDraws draws;
    oa::present::GameTextHooks hooks{};
    hooks.context = &draws;
    hooks.settings = [](void*) {
        oa::present::TextSettings settings;
        settings.style = {false, false, false, false, oa::present::default_text_size};
        settings.utf8 = true;
        return settings;
    };
    // Each character is 4 pixels wide at the game font's size, 3 at 75%.
    hooks.draw = [](void* context,
                    std::string_view text,
                    oa::present::TextFace,
                    int32_t,
                    int32_t size) -> std::shared_ptr<const oa::present::TextMask> {
        static_cast<ModernDraws*>(context)->asked.emplace_back(std::string(text), size);
        auto mask = std::make_shared<oa::present::TextMask>();
        mask->advance = characters_of(text) * size / 25;
        mask->width = mask->advance;
        mask->height = 2;
        mask->baseline = 2;
        mask->alpha.assign(static_cast<std::size_t>(mask->width) * 2U, 255);
        return mask;
    };
    oa::present::set_game_text_hooks(hooks);

    renderer::ScreenResources resources;
    resources.background.width = 60;
    resources.background.height = 20;
    resources.background.rgb.assign(60U * 20U * 3U, 0);
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    resources.background.palette = resources.gui_palette;
    oa::formats::fnt::Glyph glyph;
    glyph.width = 3;
    glyph.height = 4;
    glyph.pixels.assign(12, 7);
    glyph.coverage.assign(12, 1);
    for (const char character : {'A', 'B'})
        resources.font.glyphs[static_cast<unsigned char>(character)] = glyph;
    const renderer::ButtonPresentation state{
        "CAPTION", renderer::ButtonCondition::normal, std::nullopt, std::nullopt, std::nullopt
    };
    // The modern fonts' last draw of a button 30 wide, room 24.
    const auto drawn = [&](const std::string& text) {
        auto caption = button("CAPTION", 5, 5, 30, 10);
        caption.common.attributes = 2;
        std::get<oa::ui::gui_layout::ButtonFields>(caption.fields).text = text;
        resources.layout.gadgets = {caption};
        draws.asked.clear();
        std::ignore = renderer::render_screen(resources, {&state, 1});
        return draws.asked.empty() ? std::pair<std::string, int32_t>{} : draws.asked.back();
    };
    // Six letters, 24 wide, fit at the game font's size.
    const std::string fits = "\u041a\u043d\u043e\u043f\u043a\u0430";
    CHECK(drawn(fits) == std::make_pair(fits, oa::present::game_font_text_size));
    // Eight letters, 32 wide, fit at 75%, 24 wide, whole.
    const std::string wide = fits + "\u0430\u0430";
    CHECK(drawn(wide) == std::make_pair(wide, 75));
    // Twenty letters do not fit at 50% either: they are drawn at 50% and
    // cut to the twelve that fit.
    std::string longest;
    for (int letter = 0; letter < 20; ++letter)
        longest += "\u0430";
    const auto cut = drawn(longest);
    CHECK(cut.second == oa::present::lowest_text_size && characters_of(cut.first) == 12);

    // A one-line label wider than itself is drawn smaller until it fits:
    // eight letters in a label 20 wide fit at 65%, 20 wide.
    oa::ui::gui_layout::Gadget label;
    label.common.type = oa::ui::gui_layout::GadgetType::label;
    label.common.name = "LABEL";
    label.common.x = 5;
    label.common.y = 5;
    label.common.width = 20;
    label.common.height = 4;
    label.common.active = 1;
    oa::ui::gui_layout::LabelFields label_text;
    label_text.text = wide;
    label.fields = label_text;
    resources.layout.gadgets = {label};
    draws.asked.clear();
    std::ignore = renderer::render_screen(resources, {});
    CHECK(!draws.asked.empty() && draws.asked.back() == std::make_pair(wide, 65));
    oa::present::set_game_text_hooks({});
}

// Six grayed-out 64x64 art buttons drawn in all 256 colours of a palette
// whose colours all differ, as a builder's page of empty build slots is:
// every pixel is darkened to the shade table's colour for its own gray, and
// the screen costs at most twice what it costs with the buttons drawn
// normally.
void test_grayed_art_cost() {
    namespace renderer = oa::ui::frontend_renderer;
    constexpr int screen_width = 640;
    constexpr int screen_height = 480;
    constexpr int button_side = 64;
    constexpr int button_count = 6;
    constexpr std::size_t shade_row = 12;
    renderer::ScreenResources resources;
    resources.background.width = screen_width;
    resources.background.height = screen_height;
    resources.background.rgb.assign(std::size_t{screen_width} * screen_height * 3U, 0);
    for (std::size_t index = 0; index < 256; ++index) {
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
        resources.gui_palette[index * 4 + 1] = static_cast<uint8_t>(index * 37U);
        resources.gui_palette[index * 4 + 2] = static_cast<uint8_t>(index * 101U);
    }
    resources.background.palette = resources.gui_palette;
    resources.shade_table.resize(32U * 256U);
    for (std::size_t row = 0; row < 32; ++row)
        for (std::size_t index = 0; index < 256; ++index)
            resources.shade_table[row * 256 + index] = static_cast<uint8_t>(255U - index);
    oa::formats::gaf::Frame frame;
    frame.width = button_side;
    frame.height = button_side;
    frame.pixels.resize(std::size_t{button_side} * button_side);
    frame.coverage.assign(frame.pixels.size(), 1);
    for (std::size_t pixel = 0; pixel < frame.pixels.size(); ++pixel)
        frame.pixels[pixel] = static_cast<uint8_t>(pixel * 7U);
    std::vector<std::string> names;
    for (int slot = 0; slot < button_count; ++slot)
        names.push_back("SLOT" + std::to_string(slot));
    std::vector<renderer::ButtonPresentation> grayed;
    std::vector<renderer::ButtonPresentation> normal;
    for (int slot = 0; slot < button_count; ++slot) {
        auto gadget = button(
            names[static_cast<std::size_t>(slot)],
            static_cast<int16_t>(10 + slot * (button_side + 10)),
            20,
            button_side,
            button_side
        );
        std::get<oa::ui::gui_layout::ButtonFields>(gadget.fields).text.clear();
        resources.layout.gadgets.push_back(gadget);
        grayed.push_back(
            {names[static_cast<std::size_t>(slot)],
             renderer::ButtonCondition::disabled,
             std::nullopt,
             std::nullopt,
             std::nullopt}
        );
        normal.push_back(
            {names[static_cast<std::size_t>(slot)],
             renderer::ButtonCondition::normal,
             std::nullopt,
             std::nullopt,
             std::nullopt}
        );
    }
    // Each button draws the art sequence of its own name: the same frame
    // three times, so the grayed-out frame two past the normal one is it too.
    for (const auto& name : names) {
        oa::formats::gaf::Sequence sequence;
        sequence.name = name;
        sequence.frames.assign(3, frame);
        resources.sprites.sequences.push_back(sequence);
    }

    const auto shaded = renderer::render_screen(resources, grayed);
    const auto gray = renderer::build_gray_table(resources.gui_palette);
    bool every_pixel_shaded = true;
    for (int slot = 0; slot < button_count; ++slot)
        for (int y = 0; y < button_side; ++y)
            for (int x = 0; x < button_side; ++x) {
                const auto source =
                    gray[frame.pixels[static_cast<std::size_t>(y * button_side + x)]];
                const std::size_t row = source >= 0x80 ? shade_row - 1 : shade_row;
                const auto darkened = resources.shade_table[row * 256 + source];
                const auto at = (static_cast<std::size_t>(20 + y) * screen_width +
                                 static_cast<std::size_t>(10 + slot * (button_side + 10) + x)) *
                                3U;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    if (shaded.rgb[at + channel] != resources.gui_palette[darkened * 4U + channel])
                        every_pixel_shaded = false;
            }
    CHECK(every_pixel_shaded);

    // The quickest of several interleaved renders of each, so a busy
    // machine's pauses count against neither.
    constexpr int rounds = 7;
    auto quickest_grayed = std::chrono::steady_clock::duration::max();
    auto quickest_normal = std::chrono::steady_clock::duration::max();
    for (int round = 0; round < rounds; ++round) {
        auto start = std::chrono::steady_clock::now();
        const auto grayed_surface = renderer::render_screen(resources, grayed);
        quickest_grayed = std::min(quickest_grayed, std::chrono::steady_clock::now() - start);
        start = std::chrono::steady_clock::now();
        const auto normal_surface = renderer::render_screen(resources, normal);
        quickest_normal = std::min(quickest_normal, std::chrono::steady_clock::now() - start);
        CHECK(grayed_surface.rgb.size() == normal_surface.rgb.size());
    }
    if constexpr (render_times_compared) {
        CHECK(quickest_grayed <= 2 * quickest_normal);
        if (quickest_grayed > 2 * quickest_normal)
            std::fprintf(
                stderr,
                "grayed buttons took %lld us, normal ones %lld us\n",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(quickest_grayed).count()
                ),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(quickest_normal).count()
                )
            );
    }
}

// A colour blended over a rectangle: each channel mixed by the opacity in
// 256ths and rounded to the nearest, clipped to the image; no opacity
// leaves the pixels, a whole one paints the colour.
void test_blend_rect() {
    using oa::ui::frontend_renderer::blend_opaque;
    using oa::ui::frontend_renderer::blend_rect;
    oa::ui::frontend_renderer::Surface surface;
    surface.width = 4;
    surface.height = 3;
    surface.rgb.assign(4U * 3U * 3U, 200);
    const auto at = [&](int x, int y, int channel) {
        return surface.rgb[(static_cast<std::size_t>(y) * surface.width + x) * 3 + channel];
    };
    // Three quarters black over 200: (200 * 64 + 128) / 256 = 50.
    blend_rect(surface, 1, 1, 2, 1, {0, 0, 0}, 192);
    CHECK(at(1, 1, 0) == 50 && at(2, 1, 1) == 50 && at(2, 1, 2) == 50);
    CHECK(at(0, 1, 0) == 200 && at(3, 1, 0) == 200 && at(1, 0, 0) == 200 && at(1, 2, 0) == 200);
    // Half of each channel's colour over 50: (50 * 128 + c * 128 + 128) / 256.
    blend_rect(surface, 1, 1, 1, 1, {255, 100, 0}, 128);
    CHECK(at(1, 1, 0) == 153 && at(1, 1, 1) == 75 && at(1, 1, 2) == 25);
    // No opacity leaves the pixels; a whole one, or more, paints the colour.
    const auto kept = surface.rgb;
    blend_rect(surface, 0, 0, 4, 3, {9, 9, 9}, 0);
    CHECK(surface.rgb == kept);
    blend_rect(surface, 3, 2, 1, 1, {7, 8, 9}, blend_opaque);
    CHECK(at(3, 2, 0) == 7 && at(3, 2, 1) == 8 && at(3, 2, 2) == 9);
    blend_rect(surface, 0, 2, 1, 1, {1, 2, 3}, blend_opaque + 100);
    CHECK(at(0, 2, 0) == 1 && at(0, 2, 1) == 2 && at(0, 2, 2) == 3);
    // Clipped at every edge; nothing outside the image, an empty
    // rectangle or an image whose pixels do not fill its size changes.
    blend_rect(surface, -3, -3, 4, 4, {0, 0, 0}, blend_opaque);
    CHECK(at(0, 0, 0) == 0 && at(1, 0, 0) == 200 && at(0, 1, 0) == 200);
    blend_rect(surface, 3, 1, 10, 10, {0, 0, 0}, blend_opaque);
    CHECK(at(3, 1, 0) == 0 && at(3, 2, 0) == 0 && at(2, 2, 0) == 200);
    const auto clipped = surface.rgb;
    blend_rect(surface, 4, 0, 2, 2, {0, 0, 0}, blend_opaque);
    blend_rect(surface, 0, 3, 2, 2, {0, 0, 0}, blend_opaque);
    blend_rect(surface, 1, 1, 0, 2, {0, 0, 0}, blend_opaque);
    blend_rect(surface, 1, 1, 2, -1, {0, 0, 0}, blend_opaque);
    CHECK(surface.rgb == clipped);
    surface.rgb.assign(3, 200);
    blend_rect(surface, 0, 0, 1, 1, {0, 0, 0}, blend_opaque);
    CHECK(surface.rgb[0] == 200);
}

// The panel under a shade_below panel: entries below 0x80 read shade row
// 8 (level -0x18), entries 0x80 to 0xFF the row before, as 3.1c's
// rectangle shade indexes the row as signed bytes.
void test_shade_panel_below() {
    using oa::ui::frontend_renderer::shade_below_level;
    using oa::ui::frontend_renderer::shade_panel_below;
    CHECK(shade_below_level == -0x18);
    // Entry e is (e, 255 - e, 7): every colour is its own entry.
    oa::PaletteBytes palette{};
    for (std::size_t entry = 0; entry < oa::palette_color_count; ++entry) {
        palette[entry * oa::palette_entry_bytes] = static_cast<uint8_t>(entry);
        palette[entry * oa::palette_entry_bytes + 1] = static_cast<uint8_t>(255 - entry);
        palette[entry * oa::palette_entry_bytes + 2] = 7;
    }
    // Row 8 takes entry e to e + 1, row 7 to e + 2, every other row to 0.
    std::vector<uint8_t> shade(32U * 256U, 0);
    for (std::size_t entry = 0; entry < 256; ++entry) {
        shade[8U * 256U + entry] = static_cast<uint8_t>(entry + 1);
        shade[7U * 256U + entry] = static_cast<uint8_t>(entry + 2);
    }
    const std::vector<uint8_t> entries{0x00, 0x10, 0x7f, 0x80, 0x90, 0xff};
    oa::ui::frontend_renderer::Surface surface;
    surface.width = static_cast<uint32_t>(entries.size());
    surface.height = 2;
    surface.rgb.resize(static_cast<std::size_t>(surface.width) * surface.height * 3U);
    const auto paint = [&](std::size_t pixel, uint8_t entry) {
        std::copy_n(&palette[entry * oa::palette_entry_bytes], 3, &surface.rgb[pixel * 3U]);
    };
    const auto entry_at = [&](std::size_t pixel) {
        const auto* rgb = &surface.rgb[pixel * 3U];
        return rgb[2] == 7 && rgb[0] == 255 - rgb[1] ? static_cast<int>(rgb[0]) : -1;
    };
    const auto fill = [&] {
        for (std::size_t row = 0; row < surface.height; ++row)
            for (std::size_t column = 0; column < entries.size(); ++column)
                paint(row * surface.width + column, entries[column]);
    };
    fill();
    shade_panel_below(surface, 0, 0, 6, 1, palette, shade.data());
    CHECK(entry_at(0) == 0x01);
    CHECK(entry_at(1) == 0x11);
    CHECK(entry_at(2) == 0x80);
    CHECK(entry_at(3) == 0x82);
    CHECK(entry_at(4) == 0x92);
    CHECK(entry_at(5) == 0x01);
    for (std::size_t column = 0; column < entries.size(); ++column)
        CHECK(entry_at(surface.width + column) == entries[column]);
    // Clipped to the image; pixels of the kept colour stay.
    fill();
    shade_panel_below(
        surface, 3, -1, 10, 10, palette, shade.data(), &palette[0x90 * oa::palette_entry_bytes]
    );
    for (std::size_t row = 0; row < surface.height; ++row) {
        const std::size_t first = row * surface.width;
        CHECK(entry_at(first) == 0x00 && entry_at(first + 2) == 0x7f);
        CHECK(entry_at(first + 3) == 0x82 && entry_at(first + 4) == 0x90);
        CHECK(entry_at(first + 5) == 0x01);
    }
    // Without a shade table, or with an empty rectangle, nothing changes.
    fill();
    const auto kept = surface.rgb;
    shade_panel_below(surface, 0, 0, 6, 2, palette, nullptr);
    shade_panel_below(surface, 0, 0, 0, 2, palette, shade.data());
    shade_panel_below(surface, 6, 0, 2, 2, palette, shade.data());
    CHECK(surface.rgb == kept);
}

/// Translates the German words of 3.1c's translate.tdf the tests need.
///
/// @param text the English text
/// @return its German, or null
const char* german(void*, const char* text) {
    const std::string_view english(text);
    if (english == "Easy")
        return "Leicht";
    if (english == "Medium")
        return "Mittel";
    return nullptr;
}

// A button with stages shows each stage's caption translated on its own, as
// 3.1c translates them after the whole caption; a button without stages
// keeps its caption as the GUI file's reading translated it.
void check_translated_stage_captions() {
    using oa::ui::frontend_renderer::translated_stage_caption;
    oa::data::languages::TranslationHooks hooks;
    hooks.translate = german;
    oa::data::languages::set_translation_hooks(hooks);
    oa::ui::gui_layout::ButtonFields staged;
    staged.text = "Easy|Medium|Hard";
    staged.stages = 3;
    CHECK(translated_stage_caption(staged, 0) == "Leicht");
    CHECK(translated_stage_caption(staged, 1) == "Mittel");
    CHECK(!translated_stage_caption(staged, 2));
    oa::ui::gui_layout::ButtonFields plain;
    plain.text = "Medium";
    CHECK(!translated_stage_caption(plain, 0));
    oa::data::languages::set_translation_hooks({});
    CHECK(!translated_stage_caption(staged, 1));
}

// The modern fonts draw a font's text in its letters' colour: the lightest
// entry its 'H' is drawn in, which for a GUI font is its letters' face and
// not their shading or outline, whatever the entry at foreground_index
// holds in a screen's palette, and for an FNT font foreground_index.
void test_font_ink() {
    namespace renderer = oa::ui::frontend_renderer;
    constexpr uint8_t face = 0x42;
    constexpr uint8_t shade = 0x44;
    constexpr uint8_t outline = 0x5d;
    oa::PaletteBytes palette{};
    const auto set = [&palette](uint8_t index, uint8_t red, uint8_t green, uint8_t blue) {
        palette[index * 4U] = red;
        palette[index * 4U + 1] = green;
        palette[index * 4U + 2] = blue;
    };
    set(face, 195, 195, 155);
    set(shade, 147, 147, 111);
    set(outline, 43, 43, 43);
    set(oa::formats::fnt::foreground_index, 0, 255, 0);
    oa::formats::fnt::Glyph letter;
    letter.width = 4;
    letter.height = 1;
    letter.pixels = {outline, shade, face, outline};
    letter.coverage.assign(4, 1);
    oa::formats::fnt::Font gui;
    gui.glyphs['H'] = letter;
    CHECK(renderer::fnt_font_ink(gui, palette) == face);
    letter.pixels.assign(4, oa::formats::fnt::foreground_index);
    oa::formats::fnt::Font fnt;
    fnt.glyphs['H'] = letter;
    CHECK(renderer::fnt_font_ink(fnt, palette) == oa::formats::fnt::foreground_index);
    CHECK(
        renderer::fnt_font_ink(oa::formats::fnt::Font{}, palette) ==
        oa::formats::fnt::foreground_index
    );
}

} // namespace

// A label of several lines breaks where the line breaker lets it: a line
// of Chinese between two whole characters, never among one character's
// bytes. Each of 中's three bytes has a one-pixel glyph of its own colour,
// so a line of seven pixels holds two whole characters and the third goes
// down whole.
void test_label_lines_keep_characters_whole() {
    oa::ui::frontend_renderer::ScreenResources screen;
    screen.background.width = 12;
    screen.background.height = 16;
    screen.background.rgb.assign(12U * 16U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index)
        screen.gui_palette[index * 4] = static_cast<uint8_t>(index);
    const auto glyph = [](uint8_t colour, uint16_t height) {
        oa::formats::fnt::Glyph made;
        made.width = 1;
        made.height = height;
        made.pixels.assign(height, colour);
        made.coverage.assign(height, 1);
        return made;
    };
    // The 'I' sets the line height: two rows and two more.
    screen.font.glyphs['I'] = glyph(10, 2);
    screen.font.glyphs[0xE4] = glyph(50, 1);
    screen.font.glyphs[0xB8] = glyph(51, 1);
    screen.font.glyphs[0xAD] = glyph(52, 1);
    oa::ui::gui_layout::Gadget label;
    label.common.type = oa::ui::gui_layout::GadgetType::label;
    label.common.name = "DESCRIPTION";
    label.common.x = 1;
    label.common.y = 1;
    label.common.width = 7;
    label.common.height = 12; // two line heights fit: the label wraps
    label.common.attributes = 1;
    label.common.active = 1;
    label.fields = oa::ui::gui_layout::LabelFields{"", "\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD", ""};
    screen.layout.gadgets.push_back(label);
    const auto drawn = oa::ui::frontend_renderer::render_screen(screen);
    // The first line holds 中中, its seventh pixel left; the second line,
    // a line height and two below, starts with the third 中 whole.
    CHECK(red_at(drawn, 1, 1) == 50 && red_at(drawn, 6, 1) == 52);
    CHECK(red_at(drawn, 7, 1) == 99);
    CHECK(red_at(drawn, 1, 7) == 50 && red_at(drawn, 3, 7) == 52);
}

int main() {
    check_translated_stage_captions();
    test_label_lines_keep_characters_whole();
    test_caption_shrinks_to_button();
    test_font_ink();
    using oa::ui::frontend_renderer::staged_caption;
    CHECK(staged_caption("Easy|Medium|Hard", 0) == "Easy");
    CHECK(staged_caption("Easy|Medium|Hard", 1) == "Medium");
    CHECK(staged_caption("Easy|Medium|Hard", 2) == "Hard");
    CHECK(staged_caption("Easy|Medium|Hard", 3).empty());
    CHECK(staged_caption("Select Map", 2) == "Select Map");
    CHECK(staged_caption("", 1).empty());

    oa::ui::frontend_renderer::MainMenuResources resources;
    resources.background.width = 8;
    resources.background.height = 8;
    resources.background.rgb.assign(8U * 8U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index) {
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    }
    resources.layout.gadgets.push_back(button("PLAIN", 1, 1, 5, 5));

    const auto raised = oa::ui::frontend_renderer::render_main_menu(resources);
    CHECK(red_at(raised, 1, 1) == 17);
    CHECK(red_at(raised, 5, 5) == 0);
    CHECK(red_at(raised, 3, 3) == 20);

    const oa::ui::frontend_renderer::ButtonPresentation pressed{
        "plain",
        oa::ui::frontend_renderer::ButtonCondition::pressed,
        std::nullopt,
        std::nullopt,
        std::nullopt
    };
    const auto sunken = oa::ui::frontend_renderer::render_main_menu(resources, {&pressed, 1});
    CHECK(red_at(sunken, 1, 1) == 0);
    CHECK(red_at(sunken, 5, 5) == 17);

    // A button under the pointer draws exactly as it does without it.
    const oa::ui::frontend_renderer::ButtonPresentation hovered{
        "PLAIN",
        oa::ui::frontend_renderer::ButtonCondition::hovered,
        std::nullopt,
        std::nullopt,
        std::nullopt
    };
    CHECK(oa::ui::frontend_renderer::render_main_menu(resources, {&hovered, 1}).rgb == raised.rgb);

    const oa::ui::frontend_renderer::ButtonPresentation disabled{
        "PLAIN",
        oa::ui::frontend_renderer::ButtonCondition::disabled,
        std::nullopt,
        std::nullopt,
        std::nullopt
    };
    const auto grayed = oa::ui::frontend_renderer::render_main_menu(resources, {&disabled, 1});
    CHECK(red_at(grayed, 1, 1) == 0);
    CHECK(red_at(grayed, 5, 5) == 19);
    CHECK(red_at(grayed, 3, 3) == 19);

    oa::formats::gaf::Sequence sequence;
    sequence.name = "PLAIN";
    oa::formats::gaf::Frame frame;
    frame.width = 2;
    frame.height = 1;
    frame.transparency_index = 7;
    frame.pixels = {7, 42};
    frame.coverage = {1, 0};
    sequence.frames.push_back(frame);
    resources.sprites.sequences.push_back(sequence);
    const auto sprite = oa::ui::frontend_renderer::render_main_menu(resources);
    // A covered literal equal to the transparency index must still draw, and
    // an uncovered non-transparent value must not draw.
    CHECK(red_at(sprite, 1, 1) == 7);
    CHECK(red_at(sprite, 2, 1) == 99);

    oa::ui::frontend_renderer::ScreenResources controls;
    controls.background.width = 24;
    controls.background.height = 16;
    controls.background.rgb.assign(24U * 16U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index)
        controls.gui_palette[index * 4] = static_cast<uint8_t>(index);
    oa::formats::fnt::Glyph glyph;
    glyph.width = 2;
    glyph.height = 2;
    glyph.origin_y = -1;
    glyph.pixels.assign(4, 42);
    glyph.coverage.assign(4, 1);
    controls.font.glyphs['A'] = glyph;
    controls.font.glyphs['I'] = glyph;
    controls.light_table.resize(32U * 256U);
    for (std::size_t level = 0; level < 32; ++level)
        for (std::size_t index = 0; index < 256; ++index)
            controls.light_table[level * 256 + index] = static_cast<uint8_t>(index);
    controls.light_table[0x1eU * 256U + 42U] = 77;

    oa::ui::gui_layout::Gadget label;
    label.common.type = oa::ui::gui_layout::GadgetType::label;
    label.common.name = "DESCRIPTION";
    label.common.x = 1;
    label.common.y = 1;
    label.common.width = 2; // clips the second A completely
    label.common.height = 3;
    label.common.attributes = 1;
    label.common.active = 1;
    label.fields = oa::ui::gui_layout::LabelFields{"", "AA", ""};
    controls.layout.gadgets.push_back(label);

    oa::ui::gui_layout::Gadget list;
    list.common.type = oa::ui::gui_layout::GadgetType::list_box;
    list.common.name = "MAPNAMES";
    list.common.x = 8;
    list.common.y = 1;
    list.common.width = 8;
    list.common.height = 13;
    list.common.attributes = 1;
    list.common.active = 1;
    list.fields = oa::ui::gui_layout::ListBoxFields{0};
    controls.layout.gadgets.push_back(list);
    const std::vector<std::string> names{"A", "AA"};
    const oa::ui::frontend_renderer::ListPresentation list_data{"mapnames", names, 0, 1};
    const auto controls_rendered =
        oa::ui::frontend_renderer::render_screen(controls, {}, {&list_data, 1});
    CHECK(red_at(controls_rendered, 1, 2) == 42);
    CHECK(red_at(controls_rendered, 3, 2) == 99); // label clip

    // A single-line label authored with no height is limited by width only.
    auto flat_controls = controls;
    flat_controls.layout.gadgets[0].common.height = 0;
    const auto flat_rendered =
        oa::ui::frontend_renderer::render_screen(flat_controls, {}, {&list_data, 1});
    CHECK(red_at(flat_rendered, 1, 2) == 42);
    CHECK(red_at(flat_rendered, 3, 2) == 99);
    CHECK(red_at(controls_rendered, 10, 4) == 42);
    // line_height is glyph-I height+2; default item spacing adds one.
    CHECK(red_at(controls_rendered, 10, 9) == 77); // selected row lit at level 0x1E

    // The list drawing stops when the remaining gadget height falls below one font
    // line, even if another row's top would still lie inside the rectangle.
    auto short_controls = controls;
    auto& short_list = short_controls.layout.gadgets[1];
    short_list.common.height = 8; // line height 4, item height 5: one row only
    const auto short_rendered =
        oa::ui::frontend_renderer::render_screen(short_controls, {}, {&list_data, 1});
    CHECK(red_at(short_rendered, 10, 4) == 42);
    CHECK(red_at(short_rendered, 10, 9) == 99);

    // A button or label left with an authored foreground colour (a merged
    // sub-panel's records keep theirs) draws lit through that light-table row,
    // as 3.1c draws it with a colour table; colour 0 draws it plain.
    oa::formats::gaf::Sequence transport;
    transport.name = "CDPLAY";
    oa::formats::gaf::Frame transport_frame;
    transport_frame.width = 1;
    transport_frame.height = 1;
    transport_frame.pixels = {60};
    transport_frame.coverage = {1};
    transport.frames.push_back(transport_frame);
    controls.sprites.sequences.push_back(transport);
    controls.light_table[5U * 256U + 60U] = 120;
    auto lit_button = button("CDPLAY", 20, 10, 1, 1);
    lit_button.common.foreground_color = 5;
    controls.layout.gadgets.push_back(lit_button);
    const auto lit_button_rendered = oa::ui::frontend_renderer::render_screen(controls);
    CHECK(red_at(lit_button_rendered, 20, 10) == 120);
    controls.layout.gadgets.back().common.foreground_color = 0;
    const auto plain_button_rendered = oa::ui::frontend_renderer::render_screen(controls);
    CHECK(red_at(plain_button_rendered, 20, 10) == 60);

    controls.light_table[5U * 256U + 42U] = 150;
    oa::ui::gui_layout::Gadget lit_label;
    lit_label.common.type = oa::ui::gui_layout::GadgetType::label;
    lit_label.common.name = "TRACKNUM";
    lit_label.common.x = 20;
    lit_label.common.y = 12;
    lit_label.common.width = 2;
    lit_label.common.height = 3;
    lit_label.common.attributes = 1;
    lit_label.common.foreground_color = 5;
    lit_label.common.active = 1;
    lit_label.fields = oa::ui::gui_layout::LabelFields{"", "I", ""};
    controls.layout.gadgets.push_back(lit_label);
    const auto lit_label_rendered = oa::ui::frontend_renderer::render_screen(controls);
    CHECK(red_at(lit_label_rendered, 20, 13) == 150);

    oa::ui::gui_layout::Gadget runtime_image;
    runtime_image.common.type = oa::ui::gui_layout::GadgetType::hot_surface;
    runtime_image.common.name = "Color0";
    runtime_image.common.x = 20;
    runtime_image.common.y = 1;
    runtime_image.common.active = 1;
    controls.layout.gadgets.push_back(runtime_image);
    oa::formats::gaf::Sequence logos;
    logos.name = "32xlogos";
    oa::formats::gaf::Frame logo;
    logo.width = 1;
    logo.height = 1;
    logo.pixels = {55};
    logo.coverage = {1};
    logos.frames.push_back(logo);
    controls.global_sprites.sequences.push_back(logos);
    const oa::ui::frontend_renderer::ButtonPresentation image_state{
        "Color0",
        oa::ui::frontend_renderer::ButtonCondition::normal,
        0,
        std::nullopt,
        oa::ui::frontend_renderer::SpriteOverride{
            oa::ui::frontend_renderer::SpriteArchive::global, "32xlogos"
        }
    };
    const auto image_rendered =
        oa::ui::frontend_renderer::render_screen(controls, {&image_state, 1});
    CHECK(red_at(image_rendered, 20, 1) == 55);
    // Without a frame the hot surface is left to whoever draws it.
    auto unframed_state = image_state;
    unframed_state.gaf_frame.reset();
    const auto unframed = oa::ui::frontend_renderer::render_screen(controls, {&unframed_state, 1});
    CHECK(red_at(unframed, 20, 1) != 55);

    oa::ui::frontend_renderer::ScreenResources setup_screen = controls;
    setup_screen.background.width = 80;
    setup_screen.background.height = 40;
    setup_screen.background.rgb.assign(80U * 40U * 3U, 99);
    oa::ui::gui_layout::Gadget color_patch;
    color_patch.common.type = oa::ui::gui_layout::GadgetType::hot_surface;
    color_patch.common.name = "Color1";
    color_patch.common.x = 8;
    color_patch.common.y = 8;
    color_patch.common.width = 20;
    color_patch.common.height = 20;
    color_patch.common.active = 1;
    setup_screen.layout.gadgets = {color_patch};
    oa::formats::gaf::Frame logo32;
    logo32.width = 32;
    logo32.height = 32;
    logo32.pixels.assign(32U * 32U, 77);
    logo32.coverage.assign(32U * 32U, 1);
    logos.frames[0] = std::move(logo32);
    setup_screen.global_sprites.sequences.back() = logos;
    const oa::ui::frontend_renderer::ButtonPresentation patch_state{
        "Color1",
        oa::ui::frontend_renderer::ButtonCondition::normal,
        0,
        std::nullopt,
        oa::ui::frontend_renderer::SpriteOverride{
            oa::ui::frontend_renderer::SpriteArchive::global, "32xlogos"
        }
    };
    const auto patch_rendered =
        oa::ui::frontend_renderer::render_screen(setup_screen, {&patch_state, 1});
    CHECK(red_at(patch_rendered, 8, 8) == 77);
    CHECK(red_at(patch_rendered, 27, 27) == 77);
    CHECK(red_at(patch_rendered, 28, 8) == 99);
    CHECK(red_at(patch_rendered, 8, 28) == 99);

    oa::Image circuit;
    circuit.width = oa::ui::frontend_renderer::menu_spark_width;
    circuit.height = oa::ui::frontend_renderer::menu_spark_height;
    const auto pixels = static_cast<std::size_t>(circuit.width) * circuit.height;
    circuit.rgb.assign(pixels * 3U, 0);
    circuit.indices.assign(pixels, 0);
    circuit.palette.emplace();
    (*circuit.palette)[oa::ui::frontend_renderer::menu_spark_pixel * 4] = 250;
    (*circuit.palette)[oa::ui::frontend_renderer::menu_spark_pixel * 4 + 1] = 250;
    (*circuit.palette)[oa::ui::frontend_renderer::menu_spark_pixel * 4 + 2] = 250;
    for (int x = 0; x < oa::ui::frontend_renderer::menu_spark_width; ++x)
        circuit.indices[10U * circuit.width + static_cast<std::size_t>(x)] = 0xdf;
    oa::ui::frontend_renderer::Surface spark_surface;
    spark_surface.width = circuit.width;
    spark_surface.height = circuit.height;
    spark_surface.rgb = circuit.rgb;
    oa::ui::frontend_renderer::MenuSparks sparks;
    oa::ui::frontend_renderer::reset_menu_sparks(sparks, circuit);
    CHECK(sparks.sparks.size() == oa::ui::frontend_renderer::menu_spark_count);
    bool drew = false;
    for (int step = 0; step < 400 && !drew; ++step) {
        spark_surface.rgb.assign(pixels * 3U, 0);
        oa::ui::frontend_renderer::step_menu_sparks(sparks, spark_surface, circuit);
        for (const auto& spark : sparks.sparks) {
            if (spark.active == 0)
                continue;
            CHECK(spark.x >= 0 && spark.x < oa::ui::frontend_renderer::menu_spark_width);
            CHECK(spark.y >= 0 && spark.y < oa::ui::frontend_renderer::menu_spark_height);
            if (spark.y == 10) {
                const auto offset = (static_cast<std::size_t>(spark.y) * spark_surface.width +
                                     static_cast<std::size_t>(spark.x)) *
                                    3U;
                if (spark_surface.rgb[offset] == 250)
                    drew = true;
            }
        }
    }
    CHECK(drew);

    // A spark spawned on a vertical course moves down from an even row and
    // up from an odd one; a horizontal one right from an even column.
    oa::Image traces = circuit;
    traces.indices.assign(pixels, 0xdf);
    oa::ui::frontend_renderer::MenuSparks born;
    oa::ui::frontend_renderer::reset_menu_sparks(born, traces);
    oa::ui::frontend_renderer::step_menu_sparks(born, spark_surface, traces);
    int vertical = 0;
    for (const auto& spark : born.sparks) {
        CHECK(spark.active == 1);
        if (spark.dx == 0) {
            CHECK(spark.dy == ((spark.y & 1) == 0 ? 3 : -3));
            ++vertical;
        } else {
            CHECK(spark.dx == ((spark.x & 1) == 0 ? 3 : -3));
        }
    }
    CHECK(vertical > 0);

    test_grayed_art_frame();
    test_art_keeps_frame_size();
    test_default_art_in_panel_archive();
    test_hold_button_takes_default_art();
    test_grayed_art_cost();
    test_quick_key_and_focus();
    test_caption_only_skips_art();
    test_caption_placement();
    test_caption_cut_to_button();
    test_blend_rect();
    test_shade_panel_below();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
