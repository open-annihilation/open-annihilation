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
    // The triangle has no black edge: past the groove nothing is black.
    bool black = false;
    for (int32_t y = notch.y; y < notch.y + notch.height; ++y)
        for (int32_t x = notch.x + 2; x < notch.x + notch.width; ++x)
            black = black || (covered(canvas, x, y) && at(canvas, x, y) == gray(0));
    CHECK(!black);
    renderer::RoomCanvas held = renderer::room_canvas({0, 0, 80, 30}, nullptr);
    renderer::draw_room_notch(held, art, notch, true, true);
    CHECK(at(held, 56, 10) == gray(17));
    CHECK(at(held, 57, 10) == gray(0));
}

/// A picture twice `shown` on a side. The top half is a dark field and the
/// bottom half is gold, both opaque, and the last two columns are clear, so
/// a stamp of `shown` pixels keeps one clear column at its right.
renderer::RgbaPicture split_mark(std::vector<uint8_t>& pixels, uint32_t shown) {
    const uint32_t side = shown * 2U;
    pixels.assign(static_cast<std::size_t>(side) * side * renderer::picture_pixel_bytes, 0);
    const renderer::Rgb dark{18, 20, 16};
    const renderer::Rgb gold{214, 167, 71};
    for (uint32_t row = 0; row < side; ++row) {
        const renderer::Rgb colour = row < shown ? dark : gold;
        for (uint32_t column = 0; column < side; ++column) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * side + column) * renderer::picture_pixel_bytes;
            pixels[at] = colour[0];
            pixels[at + 1] = colour[1];
            pixels[at + 2] = colour[2];
            pixels[at + 3] = column + 2U < side ? uint8_t{255} : uint8_t{0};
        }
    }
    return {side, side, pixels};
}

void the_stamp_draws_the_mark_or_the_letters() {
    renderer::RoomArt art{nullptr, gray_palette()};
    const renderer::Rgb dark{18, 20, 16};
    const renderer::Rgb gold{214, 167, 71};
    const auto drawn = [&](int32_t side, int32_t origin_x, int32_t origin_y) {
        std::vector<uint8_t> pixels;
        const renderer::RgbaPicture icon = split_mark(pixels, static_cast<uint32_t>(side));
        renderer::RoomCanvas canvas =
            renderer::room_canvas({origin_x, origin_y, side + 4, side + 4}, nullptr);
        const renderer::SourceRect stamp{origin_x + 1, origin_y + 1, side, side};
        renderer::draw_room_stamp(canvas, art, stamp, icon);
        // The top of the square is the icon's dark field, the bottom its gold.
        CHECK(at(canvas, stamp.x + side / 2, stamp.y + 1) == dark);
        CHECK(covered(canvas, stamp.x + side / 2, stamp.y + 1));
        CHECK(at(canvas, stamp.x + side / 2, stamp.y + side - 2) == gold);
        CHECK(covered(canvas, stamp.x + side / 2, stamp.y + side - 2));
        // The icon's clear column leaves the canvas, and so does the room beside the stamp.
        CHECK(!covered(canvas, stamp.x + side - 1, stamp.y + side / 2));
        CHECK(at(canvas, stamp.x + side - 1, stamp.y + side / 2) == gray(0));
        CHECK(!covered(canvas, stamp.x - 1, stamp.y));
    };
    drawn(12, 0, 0);
    drawn(14, 3, 5);

    // Without the icon the stamp is the engraved letters on the dark plate.
    const renderer::SourceRect letters = renderer::room_stamp_rect({0, 0, 80, 20});
    renderer::RoomCanvas plain = renderer::room_canvas({0, 0, 80, 20}, nullptr);
    renderer::draw_room_stamp(plain, art, letters, {});
    CHECK(at(plain, letters.x + 1, letters.y + 1) == gray(30));
    CHECK(covered(plain, letters.x + letters.width / 2, letters.y + letters.height / 2));
    CHECK(at(plain, letters.x + letters.width / 2, letters.y + letters.height / 2) != gold);

    // A button 24 pixels high holds a stamp 14 high, still as wide as the thin letters.
    const renderer::SourceRect tall = renderer::room_stamp_rect({0, 0, 96, 24});
    CHECK(tall.height == 14 && tall.width == renderer::oa_mark_thin.width + 6);
    CHECK(tall.x == 5 && tall.y == 5);
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

void a_blinking_lamp_is_red_then_dark() {
    using renderer::RoomLamp;
    constexpr uint64_t turn = renderer::room_lamp_blink_ms;
    CHECK(turn == 500);
    CHECK(renderer::room_lamp_shown(RoomLamp::red_blinking, 0) == RoomLamp::red);
    CHECK(renderer::room_lamp_shown(RoomLamp::red_blinking, turn - 1) == RoomLamp::red);
    CHECK(renderer::room_lamp_shown(RoomLamp::red_blinking, turn) == RoomLamp::unlit);
    CHECK(renderer::room_lamp_shown(RoomLamp::red_blinking, 2 * turn - 1) == RoomLamp::unlit);
    CHECK(renderer::room_lamp_shown(RoomLamp::red_blinking, 2 * turn) == RoomLamp::red);
    CHECK(renderer::room_lamp_shown(RoomLamp::red_blinking, 7 * turn + 3) == RoomLamp::unlit);
    // A steady lamp stays as it is.
    for (const RoomLamp steady : {RoomLamp::red, RoomLamp::green, RoomLamp::unlit})
        for (const uint64_t time : {uint64_t{0}, turn, 3 * turn + 7})
            CHECK(renderer::room_lamp_shown(steady, time) == steady);
    // Drawn alone, a blinking lamp is the red one.
    renderer::RoomArt art{nullptr, gray_palette()};
    renderer::RoomCanvas red = renderer::room_canvas({0, 0, 6, 12}, nullptr);
    renderer::draw_room_lamp(red, art, {0, 0, 6, 12}, RoomLamp::red);
    renderer::RoomCanvas blinking = renderer::room_canvas({0, 0, 6, 12}, nullptr);
    renderer::draw_room_lamp(blinking, art, {0, 0, 6, 12}, RoomLamp::red_blinking);
    CHECK(red.surface.rgb == blinking.surface.rgb);
}

void a_picture_of_the_lamps_reads_back() {
    using renderer::RoomLamp;
    // Two lamps side by side, 8 by 4 pixels, in a 10 by 6 picture: the red
    // one on the left, the green on the right.
    const auto picture = [](renderer::Rgb left, renderer::Rgb right) {
        std::vector<uint8_t> rgb(10U * 6U * 3U, uint8_t{90});
        for (int32_t y = 1; y < 5; ++y)
            for (int32_t x = 1; x < 9; ++x) {
                const renderer::Rgb colour = x < 5 ? left : right;
                const std::size_t at =
                    (static_cast<std::size_t>(y) * 10U + static_cast<std::size_t>(x)) * 3U;
                rgb[at] = colour[0];
                rgb[at + 1] = colour[1];
                rgb[at + 2] = colour[2];
            }
        return rgb;
    };
    const renderer::SourceRect lamps{1, 1, 8, 4};
    const renderer::Rgb dark_red{90, 30, 30};
    const renderer::Rgb dark_green{30, 90, 30};
    CHECK(renderer::room_lamp_seen(picture({240, 40, 30}, dark_green), 10, lamps) == RoomLamp::red);
    CHECK(renderer::room_lamp_seen(picture(dark_red, {40, 240, 40}), 10, lamps) == RoomLamp::green);
    CHECK(renderer::room_lamp_seen(picture(dark_red, dark_green), 10, lamps) == RoomLamp::unlit);
    // Lamps off the picture, or none, read as unlit.
    CHECK(
        renderer::room_lamp_seen(picture({240, 40, 30}, dark_green), 10, {4, 1, 8, 4}) ==
        RoomLamp::unlit
    );
    CHECK(
        renderer::room_lamp_seen(picture({240, 40, 30}, dark_green), 10, {1, 1, 0, 0}) ==
        RoomLamp::unlit
    );
}

void a_bar_fills_from_its_left() {
    renderer::RoomArt art{nullptr, gray_palette()};
    renderer::RoomCanvas canvas = renderer::room_canvas({0, 0, 22, 8}, nullptr);
    renderer::draw_room_bar(canvas, art, {0, 0, 22, 8}, 1, 2);
    // A sunk well, filled across half its inside.
    CHECK(at(canvas, 0, 0) == gray(0));
    CHECK(at(canvas, 21, 7) == gray(17));
    CHECK(at(canvas, 5, 2)[0] > 0);
    CHECK(at(canvas, 15, 2) == gray(0));
    renderer::RoomCanvas empty = renderer::room_canvas({0, 0, 22, 8}, nullptr);
    renderer::draw_room_bar(empty, art, {0, 0, 22, 8}, 0, 2);
    CHECK(at(empty, 5, 2) == gray(0));
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
        // The picture reads back the lamp drawn: the canvas is the whole frame.
        CHECK(
            renderer::room_lamp_seen(canvas.surface.rgb, canvas.area.width, lamp.lamp) ==
            lamps[index]
        );
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
    the_stamp_draws_the_mark_or_the_letters();
    the_stamp_cuts_the_mark_into_a_dark_plate();
    a_lamp_is_lit_from_its_top();
    a_blinking_lamp_is_red_then_dark();
    a_picture_of_the_lamps_reads_back();
    a_bar_fills_from_its_left();
    a_list_lays_out_its_rows();
    if (failures != 0)
        return 1;
    std::cout << "room parts: ok\n";
    return 0;
}
