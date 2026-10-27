// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A room's own parts (room_parts.hpp): the canvas over a room's frame and
// the pixels it covers, the plain button face, a plate from BUTTONS0 drawn
// wider than its art, the notch, the stamp, a lamp and a list box's rows.
// With --data, each part in the installed game's art over the skirmish
// setup's background; --frames DIR also writes that picture as
// room-parts.png in DIR for review.

#include "oa/ui/frontend_renderer/room_parts.hpp"

#include "oa/formats/png.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition)
        return;
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
    ++failures;
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

namespace renderer = oa::ui::frontend_renderer;
namespace gaf = oa::formats::gaf;

/// A palette whose entry n is the gray n, n, n.
oa::PaletteBytes gray_palette() {
    oa::PaletteBytes palette{};
    for (std::size_t index = 0; index < oa::palette_color_count; ++index) {
        palette[index * oa::palette_entry_bytes] = static_cast<uint8_t>(index);
        palette[index * oa::palette_entry_bytes + 1] = static_cast<uint8_t>(index);
        palette[index * oa::palette_entry_bytes + 2] = static_cast<uint8_t>(index);
    }
    return palette;
}

renderer::Rgb at(const renderer::RoomCanvas& canvas, int32_t x, int32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y - canvas.area.y) * canvas.area.width +
                               static_cast<std::size_t>(x - canvas.area.x)) *
                              3U;
    return {
        canvas.surface.rgb[index], canvas.surface.rgb[index + 1], canvas.surface.rgb[index + 2]
    };
}

bool covered(const renderer::RoomCanvas& canvas, int32_t x, int32_t y) {
    return canvas.covered
               [static_cast<std::size_t>(y - canvas.area.y) * canvas.area.width +
                static_cast<std::size_t>(x - canvas.area.x)] != 0;
}

renderer::Rgb gray(uint8_t level) {
    return {level, level, level};
}

/// A frame of one palette index a pixel, every pixel covered.
gaf::Frame frame_of(uint16_t width, uint16_t height, const std::vector<uint8_t>& pixels) {
    gaf::Frame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels = pixels;
    frame.coverage.assign(pixels.size(), 1);
    return frame;
}

void the_canvas_keeps_the_frame_and_covers_nothing() {
    renderer::Surface frame{4, 3, {}};
    for (uint8_t value = 0; value < 12; ++value) {
        frame.rgb.push_back(static_cast<uint8_t>(value * 10));
        frame.rgb.push_back(1);
        frame.rgb.push_back(2);
    }
    const renderer::RoomCanvas canvas = renderer::room_canvas({1, 1, 4, 3}, &frame);
    CHECK(canvas.surface.width == 4 && canvas.surface.height == 3);
    // Frame pixel (2, 1) is the seventh; pixels past the frame are black.
    CHECK((at(canvas, 2, 1) == renderer::Rgb{60, 1, 2}));
    CHECK((at(canvas, 4, 1) == renderer::Rgb{0, 0, 0}));
    CHECK((at(canvas, 1, 3) == renderer::Rgb{0, 0, 0}));
    CHECK(std::all_of(canvas.covered.begin(), canvas.covered.end(), [](uint8_t mark) {
        return mark == 0;
    }));
    std::vector<uint8_t> rgba;
    renderer::room_canvas_rgba(canvas, rgba);
    CHECK(rgba.size() == 4U * 3U * 4U);
    CHECK(std::all_of(rgba.begin(), rgba.end(), [](uint8_t byte) { return byte == 0; }));
    CHECK(renderer::room_canvas({0, 0, 0, 3}, &frame).surface.rgb.empty());
}

void a_button_without_art_is_the_plain_face() {
    renderer::RoomArt art{nullptr, gray_palette()};
    renderer::RoomCanvas canvas = renderer::room_canvas({0, 0, 24, 12}, nullptr);
    renderer::RoomButtonLook look;
    look.rect = {2, 2, 20, 8};
    renderer::draw_room_button(canvas, art, look);
    // Filled with entry 20, light (17) above and left, black below and right.
    CHECK(at(canvas, 10, 5) == gray(20));
    CHECK(at(canvas, 2, 2) == gray(17));
    CHECK(at(canvas, 3, 3) == gray(17));
    CHECK(at(canvas, 21, 9) == gray(0));
    CHECK(covered(canvas, 2, 2) && covered(canvas, 21, 9));
    CHECK(!covered(canvas, 1, 1) && !covered(canvas, 22, 10));
    look.condition = renderer::ButtonCondition::pressed;
    renderer::draw_room_button(canvas, art, look);
    CHECK(at(canvas, 2, 2) == gray(0));
    CHECK(at(canvas, 21, 9) == gray(17));
    std::vector<uint8_t> rgba;
    renderer::room_canvas_rgba(canvas, rgba);
    CHECK(rgba[(2U * 24U + 2U) * 4U + 3U] == 255);
    CHECK(rgba[(1U * 24U + 1U) * 4U + 3U] == 0);
}

void a_plate_keeps_its_ends_and_repeats_its_middle() {
    renderer::ScreenResources resources;
    gaf::Sequence buttons;
    buttons.name = "BUTTONS0";
    // A 16 by 4 plate whose every column is its own entry: 30, 31, ... 45.
    std::vector<uint8_t> pixels;
    for (int32_t row = 0; row < 4; ++row)
        for (int32_t column = 0; column < 16; ++column)
            pixels.push_back(static_cast<uint8_t>(30 + column));
    for (int32_t state = 0; state < 4; ++state)
        buttons.frames.push_back(frame_of(16, 4, pixels));
    // A second size group, far from the button's size.
    for (int32_t state = 0; state < 4; ++state)
        buttons.frames.push_back(frame_of(2, 2, {1, 1, 1, 1}));
    resources.shared_sprites.sequences.push_back(buttons);
    CHECK(renderer::room_button_group(buttons, 30, 4) == 0);
    CHECK(renderer::room_button_group(buttons, 3, 3) == 4);
    renderer::RoomArt art{&resources, gray_palette()};
    renderer::RoomCanvas canvas = renderer::room_canvas({0, 0, 30, 4}, nullptr);
    renderer::RoomButtonLook look;
    look.rect = {0, 0, 30, 4};
    renderer::draw_room_button(canvas, art, look);
    // The first six and last six columns are the plate's ends; the middle
    // four columns of the plate repeat between them.
    CHECK(at(canvas, 0, 1) == gray(30));
    CHECK(at(canvas, 5, 1) == gray(35));
    CHECK(at(canvas, 6, 1) == gray(36));
    CHECK(at(canvas, 9, 1) == gray(39));
    CHECK(at(canvas, 10, 1) == gray(36));
    CHECK(at(canvas, 24, 1) == gray(40));
    CHECK(at(canvas, 29, 2) == gray(45));
}

void the_notch_sits_in_the_right_end() {
    const renderer::SourceRect notch = renderer::room_notch_rect({10, 4, 60, 20}, 12);
    CHECK(notch.x == 56 && notch.y == 6 && notch.width == 12 && notch.height == 16);
    renderer::RoomArt art{nullptr, gray_palette()};
    renderer::RoomCanvas canvas = renderer::room_canvas({0, 0, 80, 30}, nullptr);
    renderer::draw_room_notch(canvas, art, notch, false, false);
    CHECK(at(canvas, 56, 10) == gray(0));
    CHECK(at(canvas, 57, 10) == gray(17));
    // The triangle points down: its widest row is its first.
    int32_t first_row = -1;
    int32_t first_width = 0;
    int32_t last_width = 0;
    for (int32_t y = notch.y; y < notch.y + notch.height; ++y) {
        int32_t width = 0;
        for (int32_t x = notch.x + 2; x < notch.x + notch.width; ++x)
            if (covered(canvas, x, y) && at(canvas, x, y) != gray(0))
                ++width;
        if (width == 0)
            continue;
        if (first_row < 0) {
            first_row = y;
            first_width = width;
        }
        last_width = width;
    }
    CHECK(first_row >= 0);
    CHECK(first_width > last_width);
    renderer::RoomCanvas held = renderer::room_canvas({0, 0, 80, 30}, nullptr);
    renderer::draw_room_notch(held, art, notch, true, true);
    CHECK(at(held, 56, 10) == gray(17));
    CHECK(at(held, 57, 10) == gray(0));
}

void the_stamp_cuts_the_mark_into_a_dark_plate() {
    const renderer::SourceRect stamp = renderer::room_stamp_rect({0, 0, 80, 20});
    CHECK(stamp.height == 12 && stamp.width == renderer::oa_mark_thin.width + 6);
    CHECK(stamp.x == 5 && stamp.y == 4);
    renderer::RoomArt art{nullptr, gray_palette()};
    renderer::RoomCanvas canvas = renderer::room_canvas({0, 0, 80, 20}, nullptr);
    renderer::draw_room_stamp(canvas, art, stamp);
    for (int32_t y = stamp.y; y < stamp.y + stamp.height; ++y)
        for (int32_t x = stamp.x; x < stamp.x + stamp.width; ++x)
            CHECK(covered(canvas, x, y));
    // On a gray palette the gold is the brightest gray in the stamp.
    uint8_t brightest = 0;
    for (int32_t y = stamp.y + 1; y < stamp.y + stamp.height - 1; ++y)
        for (int32_t x = stamp.x + 1; x < stamp.x + stamp.width - 1; ++x)
            brightest = std::max(brightest, at(canvas, x, y)[0]);
    CHECK(brightest > 100);
}

void a_lamp_is_lit_from_its_top() {
    renderer::RoomArt art{nullptr, gray_palette()};
    renderer::RoomCanvas canvas = renderer::room_canvas({0, 0, 6, 12}, nullptr);
    renderer::draw_room_lamp(canvas, art, {0, 0, 6, 12}, renderer::RoomLamp::green);
    CHECK(at(canvas, 0, 0) == gray(0));
    CHECK(at(canvas, 2, 1)[0] > at(canvas, 2, 10)[0]);
    renderer::RoomCanvas unlit = renderer::room_canvas({0, 0, 6, 12}, nullptr);
    renderer::draw_room_lamp(unlit, art, {0, 0, 6, 12}, renderer::RoomLamp::unlit);
    CHECK(at(unlit, 2, 1)[0] < at(canvas, 2, 1)[0]);
}

void a_list_lays_out_its_rows() {
    renderer::RoomArt art{nullptr, gray_palette()};
    std::array<renderer::RoomListRow, 3> rows{};
    rows[0].heading = true;
    rows[2].rule_above = true;
    renderer::RoomListLook look;
    look.rect = {10, 20, 100, 60};
    look.rows = rows;
    const std::vector<renderer::SourceRect> rects = renderer::room_list_row_rects(art, look);
    CHECK(rects.size() == 3);
    const int32_t height = renderer::room_list_row_height(art);
    CHECK(height == 12);
    CHECK(rects[0].x == 15 && rects[0].y == 25 && rects[0].width == 90);
    CHECK(rects[1].y == 25 + height);
    CHECK(rects[2].y == 25 + 2 * height + renderer::room_list_rule);
    const renderer::SourceRect size = renderer::room_list_size(art, rows, 30);
    CHECK(size.height == 3 * height + renderer::room_list_rule + 2 * renderer::room_list_border);
    renderer::RoomCanvas canvas = renderer::room_canvas({0, 0, 120, 90}, nullptr);
    renderer::draw_room_list(canvas, art, look);
    // The frame is the plain face round a black well.
    CHECK(at(canvas, 10, 20) == gray(17));
    CHECK(at(canvas, 12, 40) == gray(20));
    CHECK(at(canvas, 60, 40) == gray(0));
    CHECK(covered(canvas, 60, 40) && !covered(canvas, 5, 5));
}

/// Writes a canvas over the room as a PNG file.
void write_picture(const std::filesystem::path& file, const renderer::RoomCanvas& canvas) {
    std::vector<uint8_t> bytes;
    const oa::formats::png::Header header{
        canvas.surface.width, canvas.surface.height, 8, oa::formats::png::ColorType::rgb, {}
    };
    if (!oa::formats::png::write(oa::formats::png::Image{header, {}, canvas.surface.rgb}, &bytes)) {
        std::cerr << "could not encode " << file << '\n';
        ++failures;
        return;
    }
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
}

/// Draws every part in the installed game's art over the skirmish setup.
void the_parts_draw_in_the_game_art(const std::filesystem::path& frames) {
    oa::AssetStore assets = oa::test::require_game_assets("a room's parts in the game's art");
    const renderer::ScreenResources resources = renderer::load_screen_art(
        assets,
        {"",
         "bitmaps/skirmsetup4x.pcx",
         "palettes/guipal.pal",
         "anims/skirmish.gaf",
         "anims/commongui.gaf"}
    );
    const renderer::RoomArt art{&resources, renderer::room_palette(resources)};
    renderer::Surface frame{
        resources.background.width, resources.background.height, resources.background.rgb
    };
    renderer::RoomCanvas canvas = renderer::room_canvas({0, 0, 640, 480}, &frame);

    // A button at a BUTTONS0 size keeps the plate as drawn.
    const gaf::Sequence* buttons = nullptr;
    for (const gaf::Sequence& sequence : resources.shared_sprites.sequences)
        if (sequence.name == "BUTTONS0")
            buttons = &sequence;
    CHECK(buttons != nullptr && buttons->frames.size() >= 4);
    if (buttons == nullptr || buttons->frames.empty())
        return;
    const gaf::Frame& plate = buttons->frames[0];
    const std::string caption = renderer::room_text("Mutators");
    renderer::RoomButtonLook look;
    look.rect = {20, 20, plate.width, plate.height};
    look.caption = caption;
    renderer::draw_room_button(canvas, art, look);
    CHECK(covered(canvas, 20, 20 + plate.height / 2));
    CHECK(covered(canvas, 20 + plate.width - 1, 20 + plate.height / 2));
    CHECK(!covered(canvas, 19, 20) && !covered(canvas, 20 + plate.width, 20));

    // The switch lights its lamp green, red, or not at all, at its right end.
    const renderer::RoomLampParts parts =
        renderer::room_lamp_parts(art, {20, 60, 160, plate.height});
    CHECK(parts.lamp.width > 0 && parts.lamp.height > 0);
    CHECK(parts.lamp.x > 20 + 80);
    const std::array<renderer::RoomLamp, 3> lamps{
        renderer::RoomLamp::green, renderer::RoomLamp::red, renderer::RoomLamp::unlit
    };
    std::array<int32_t, 3> red_less_green{};
    for (std::size_t index = 0; index < lamps.size(); ++index) {
        const renderer::SourceRect rect{20, 60 + static_cast<int32_t>(index) * 30, 180, 22};
        const std::string words = renderer::room_text("Mutators: 2 on");
        renderer::RoomButtonLook switch_look;
        switch_look.rect = rect;
        switch_look.caption = words;
        switch_look.inset_left = renderer::room_stamp_rect(rect).width + 6;
        renderer::draw_room_switch(canvas, art, switch_look, lamps[index]);
        renderer::draw_room_stamp(canvas, art, renderer::room_stamp_rect(rect));
        const renderer::RoomLampParts lamp = renderer::room_lamp_parts(art, rect);
        for (int32_t y = lamp.lamp.y; y < lamp.lamp.y + lamp.lamp.height; ++y)
            for (int32_t x = lamp.lamp.x; x < lamp.lamp.x + lamp.lamp.width; ++x) {
                const renderer::Rgb colour = at(canvas, x, y);
                red_less_green[index] += static_cast<int32_t>(colour[1]) - colour[0];
            }
    }
    CHECK(red_less_green[0] > 0);
    CHECK(red_less_green[1] < 0);

    // A player's button with a notch, and the list it opens.
    const std::string name = renderer::room_text("Lantern Scout");
    renderer::RoomButtonLook seat;
    seat.rect = {20, 160, 112, 20};
    seat.caption = name;
    seat.inset_right = 14;
    seat.narrower_font = true;
    renderer::draw_room_button(canvas, art, seat);
    renderer::draw_room_notch(canvas, art, renderer::room_notch_rect(seat.rect, 12), true, false);
    std::vector<renderer::RoomListRow> rows(7);
    rows[0].heading = true;
    rows[0].text = renderer::room_text("Computer player for this seat");
    rows[1].text = renderer::room_text("Computer");
    rows[1].value = renderer::room_text("Medium");
    rows[2].text = renderer::room_text("Lantern Scout");
    rows[2].value = renderer::room_text("< Steady >");
    rows[2].selected = true;
    rows[2].ticked = true;
    rows[3].text = renderer::room_text("Steady Commander");
    rows[3].value = renderer::room_text("Firm");
    rows[4].text = renderer::room_text("Ridge Raider");
    rows[4].value = renderer::room_text("Raid");
    rows[4].grey = true;
    rows[5].text = renderer::room_text("Use for every computer seat");
    rows[5].checkbox = true;
    rows[5].rule_above = true;
    rows[6].text = renderer::room_text("Get more brains...");
    renderer::RoomListLook list;
    const renderer::SourceRect size = renderer::room_list_size(art, rows, 90);
    list.rect = {20, 182, std::max(size.width, 240), size.height};
    list.rows = rows;
    list.value_width = 90;
    renderer::draw_room_list(canvas, art, list);
    const std::vector<renderer::SourceRect> rects = renderer::room_list_row_rects(art, list);
    CHECK(rects.size() == rows.size());
    // The chosen row is lit; a row that is not differs from it.
    CHECK(
        at(canvas, rects[2].x + rects[2].width - 2, rects[2].y + 1) !=
        at(canvas, rects[1].x + rects[1].width - 2, rects[1].y + 1)
    );
    renderer::draw_room_focus(canvas, art, seat.rect);
    CHECK(covered(canvas, seat.rect.x - 1, seat.rect.y + 5));

    if (!frames.empty())
        write_picture(frames / "room-parts.png", canvas);
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        std::filesystem::path frames;
        for (int index = 1; index + 1 < argc; ++index)
            if (std::string_view(argv[index]) == "--frames")
                frames = argv[index + 1];
        the_parts_draw_in_the_game_art(frames);
        if (failures != 0)
            return 1;
        std::cout << "room parts in the installed game's art: ok\n";
        return 0;
    }
    the_canvas_keeps_the_frame_and_covers_nothing();
    a_button_without_art_is_the_plain_face();
    a_plate_keeps_its_ends_and_repeats_its_middle();
    the_notch_sits_in_the_right_end();
    the_stamp_cuts_the_mark_into_a_dark_plate();
    a_lamp_is_lit_from_its_top();
    a_list_lays_out_its_rows();
    if (failures != 0)
        return 1;
    std::cout << "room parts: ok\n";
    return 0;
}
