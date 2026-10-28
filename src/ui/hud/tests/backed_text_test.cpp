// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "check.hpp"

#include "oa/present/game_text.hpp"
#include "oa/ui/hud/backed_text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::ui::hud;

namespace {

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 96;
constexpr std::array<uint8_t, 3> kFigureColour{80, 220, 80};
constexpr std::array<uint8_t, 3> kStatusColour{255, 255, 255};

/// A palette of black, a grey ramp and the lines' colours, in entries of
/// four bytes.
std::vector<uint8_t> test_palette() {
    std::vector<uint8_t> palette(256 * 4, 0);
    for (std::size_t entry = 1; entry < 250; ++entry)
        for (std::size_t channel = 0; channel < 3; ++channel)
            palette[entry * 4 + channel] = static_cast<uint8_t>(entry);
    const auto set = [&](std::size_t entry, std::array<uint8_t, 3> colour) {
        for (std::size_t channel = 0; channel < 3; ++channel)
            palette[entry * 4 + channel] = colour[channel];
    };
    set(250, kFigureColour);
    set(251, kStatusColour);
    set(252, {120, 60, 30});
    set(253, {30, 90, 160});
    return palette;
}

/// Letters as blocks: columns from `first` to `last` covered whole on every
/// row, and the column after them half covered.
present::TextMask block_letters(int32_t width, int32_t first, int32_t last) {
    present::TextMask mask;
    mask.width = width;
    mask.height = 9;
    mask.baseline = 8;
    mask.origin = 0;
    mask.advance = width;
    mask.alpha.assign(static_cast<std::size_t>(mask.width * mask.height), 0);
    for (int32_t row = 0; row < mask.height; ++row) {
        for (int32_t column = first; column <= last; ++column)
            mask.alpha[static_cast<std::size_t>(row * width + column)] = 255;
        if (last + 1 < width)
            mask.alpha[static_cast<std::size_t>(row * width + last + 1)] = 128;
    }
    return mask;
}

/// A line with an outline, a shadow and a background box.
present::TextLayers backed_line(const present::TextMask& mask) {
    present::TextStyle style;
    style.background = true;
    return present::build_text_layers(mask, style, 1);
}

/// RGB pixels of the battlefield's mixed colours, which no line lays.
std::vector<uint8_t> ground() {
    std::vector<uint8_t> rgb(static_cast<std::size_t>(kWidth * kHeight * 3));
    for (std::size_t at = 0; at < rgb.size(); ++at)
        rgb[at] = static_cast<uint8_t>(90 + (at * 7) % 61);
    return rgb;
}

/// Counts the runs a canvas hands to the picture under it.
void count_run(void* user, const present::TextWorldRun&) {
    ++*static_cast<int*>(user);
}

// A line's backing laid alone and then the line without it make the pixels
// lay_text makes of the whole line: on RGB pixels with and without a
// palette, on 8-bit pixels, on a canvas the picture under it shows through,
// and cut by the clip rectangle.
void backing_then_letters_make_the_whole_line() {
    const auto palette = test_palette();
    const auto line = backed_line(block_letters(12, 2, 6));
    CHECK(line.background && line.background_top == 0 && line.background_bottom == 11);
    const auto bare = without_backing(line);
    CHECK(!bare.background && bare.background_top == 0 && bare.background_bottom == 11);
    CHECK(bare.fill == line.fill && bare.outline == line.outline && bare.shadow == line.shadow);
    for (const bool reduced : {false, true}) {
        for (const bool see_through : {false, true}) {
            for (const bool clipped : {false, true}) {
                auto whole = ground();
                auto parts = ground();
                // The picture under the canvas shows through a band of pixels.
                for (std::size_t at = 0; see_through && at < whole.size() / 2; at += 3)
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        whole[at + channel] = parts[at + channel] = channel == 1 ? 255 : 0;
                const std::span<const uint8_t> used =
                    reduced ? std::span<const uint8_t>(palette) : std::span<const uint8_t>{};
                auto whole_canvas = present::rgb_canvas(whole, kWidth, kHeight, used);
                auto parts_canvas = present::rgb_canvas(parts, kWidth, kHeight, used);
                int whole_runs = 0;
                int parts_runs = 0;
                for (auto* canvas : {&whole_canvas, &parts_canvas}) {
                    if (see_through) {
                        canvas->see_through = std::array<uint8_t, 3>{0, 255, 0};
                        canvas->world_run = count_run;
                    }
                    if (clipped) {
                        canvas->clip_left = 14;
                        canvas->clip_top = 22;
                        canvas->clip_right = 40;
                    }
                }
                whole_canvas.world_user = &whole_runs;
                parts_canvas.world_user = &parts_runs;
                for (const int32_t x : {10, 30, 58}) {
                    present::lay_text(whole_canvas, line, x, 30, kStatusColour);
                    lay_text_backing(parts_canvas, line, x, 30);
                    present::lay_text(parts_canvas, bare, x, 30, kStatusColour);
                }
                CHECK(whole == parts);
                CHECK(whole_runs == parts_runs);
            }
        }
    }
    // On 8-bit pixels the box is the palette's black.
    std::vector<uint8_t> whole(static_cast<std::size_t>(kWidth * kHeight), 7);
    std::vector<uint8_t> parts = whole;
    present::TextCanvas whole_canvas;
    whole_canvas.pixels = whole.data();
    whole_canvas.width = kWidth;
    whole_canvas.height = kHeight;
    whole_canvas.pitch = kWidth;
    whole_canvas.indexed = true;
    whole_canvas.clip_right = kWidth - 1;
    whole_canvas.clip_bottom = kHeight - 1;
    whole_canvas.palette = palette;
    auto parts_canvas = whole_canvas;
    parts_canvas.pixels = parts.data();
    present::lay_text(whole_canvas, line, 20, 30, kFigureColour);
    lay_text_backing(parts_canvas, line, 20, 30);
    present::lay_text(parts_canvas, bare, 20, 30, kFigureColour);
    CHECK(whole == parts);
    CHECK(parts[static_cast<std::size_t>(21 * kWidth + 19)] == 0);
    // A line without a box lays nothing; nor does an 8-bit canvas without a
    // palette.
    const auto before = parts;
    lay_text_backing(parts_canvas, bare, 40, 60);
    parts_canvas.palette = {};
    lay_text_backing(parts_canvas, line, 40, 60);
    CHECK(parts == before);
}

/// The lines of the bottom bar's case: the rate figures, then the status
/// above them, whose box reaches down over the top rows of the figures'
/// letters; and two figures side by side on a lower row, the second's box
/// starting a column left of its pen, over the last column of the first's
/// letters.
std::vector<LaidLine> bottom_bar_lines() {
    std::vector<LaidLine> lines;
    // "+0.0": letters in columns 10 to 21, rows 32 to 40.
    lines.push_back({backed_line(block_letters(12, 0, 11)), 10, 40, kFigureColour, true});
    // "Ready": its letters well right of the figures', its box over all of
    // its columns and down to row 33.
    lines.push_back({backed_line(block_letters(24, 14, 19)), 9, 31, kStatusColour, true});
    // A sign and its figures on one row, the figures pulled toward the sign
    // a column closer than its advance, as the clock line sets them.
    lines.push_back({backed_line(block_letters(12, 0, 11)), 10, 70, kFigureColour, true});
    lines.push_back({backed_line(block_letters(12, 2, 6)), 22, 70, kStatusColour, true});
    return lines;
}

// Each line laid whole in turn, as before: the status's box covers the top
// rows of the figures', and the second figure's box the first's last
// column.
void lines_laid_whole_cover_each_other() {
    const auto palette = test_palette();
    const auto lines = bottom_bar_lines();
    auto rgb = ground();
    auto canvas = present::rgb_canvas(rgb, kWidth, kHeight, palette);
    for (const LaidLine& line : lines)
        present::lay_text(canvas, line.layers, line.x, line.baseline_y, line.color);
    const auto found = find_laid_lines(rgb, kWidth, kHeight, palette, lines);
    CHECK(found.letters_under_later_backings == 2 * 12 + 9);
    CHECK(found.covered_letters == found.letters_under_later_backings);
    CHECK(found.backing_pixels > 0 && found.backing_pixels_shown == found.backing_pixels);
    CHECK(found.bare_backings == 0);
}

// The same lines in a group that holds: every backing is laid where its
// line is drawn and every line after them, so each line's letters show
// whole and every backing is still there.
void held_lines_lie_over_every_backing() {
    const auto palette = test_palette();
    const auto lines = bottom_bar_lines();
    auto rgb = ground();
    auto canvas = present::rgb_canvas(rgb, kWidth, kHeight, palette);
    TextHold hold;
    hold.open(true);
    for (const LaidLine& line : lines) {
        CHECK(hold.holding());
        lay_text_backing(canvas, line.layers, line.x, line.baseline_y);
        hold.hold([&canvas, &line, bare = without_backing(line.layers)] {
            present::lay_text(canvas, bare, line.x, line.baseline_y, line.color);
        });
    }
    CHECK(hold.held() == lines.size());
    hold.close();
    CHECK(hold.held() == 0 && !hold.holding());
    const auto found = find_laid_lines(rgb, kWidth, kHeight, palette, lines);
    CHECK(found.letter_pixels > 0);
    CHECK(found.letters_under_later_backings == 2 * 12 + 9);
    CHECK(found.covered_letters == 0);
    CHECK(found.backing_pixels > 0 && found.backing_pixels_shown == found.backing_pixels);
    CHECK(found.bare_backings == 0);
    // A backing left out shows as bare.
    auto unbacked = lines;
    unbacked[1].layers = without_backing(unbacked[1].layers);
    auto plain = ground();
    auto plain_canvas = present::rgb_canvas(plain, kWidth, kHeight, palette);
    for (const LaidLine& line : unbacked)
        present::lay_text(plain_canvas, line.layers, line.x, line.baseline_y, line.color);
    const auto bare_found = find_laid_lines(plain, kWidth, kHeight, palette, lines);
    CHECK(bare_found.bare_backings == 1);
}

// Groups opened inside a group join it; the outermost decides whether
// lines are held, and its close lays them in the order they were held, with
// nothing holding. A pause lays lines at once; a drop lets them go.
void groups_join_and_close_in_order() {
    std::string laid;
    const auto line = [&laid](char name) { return [&laid, name] { laid.push_back(name); }; };
    TextHold hold;
    hold.hold(line('a'));
    CHECK(laid == "a" && !hold.holding());
    hold.open(false);
    hold.hold(line('b'));
    hold.open(true);
    CHECK(!hold.holding());
    hold.close();
    hold.close();
    CHECK(laid == "ab");
    hold.open(true);
    hold.hold(line('c'));
    hold.open(false);
    CHECK(hold.holding());
    hold.hold(line('d'));
    hold.close();
    CHECK(laid == "ab" && hold.held() == 2);
    {
        const TextHold::Pause pause(hold);
        CHECK(!hold.holding());
        hold.hold(line('e'));
        CHECK(laid == "abe");
    }
    CHECK(hold.holding());
    hold.hold([&] {
        CHECK(!hold.holding());
        hold.hold(line('g'));
        laid.push_back('f');
    });
    hold.close();
    CHECK(laid == "abecdgf" && hold.held() == 0);
    hold.close();
    hold.open(true);
    hold.hold(line('x'));
    hold.drop();
    CHECK(!hold.holding() && hold.held() == 0);
    hold.close();
    CHECK(laid == "abecdgf");
}

} // namespace

int main() {
    backing_then_letters_make_the_whole_line();
    lines_laid_whole_cover_each_other();
    held_lines_lie_over_every_backing();
    groups_join_and_close_in_order();
    return 0;
}
