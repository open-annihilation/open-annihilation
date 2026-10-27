// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kit's list rows, chips, fields, tabs, cards, hover cards and links:
// their pixels in the kit's tokens, their geometry, their controls and
// names, and paint against their direct draws.
//
// The synthetic font's glyph for byte b is 2 + b % 4 columns by 8 rows, set
// where (x + y + b) % 3 is not 0, so a glyph's top-left pixel is set unless
// b % 3 is 0. Text in a box 12 rows high starts one row down, in a box 16
// rows high three rows down. The widths below are sums of those columns.

#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
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

/// The synthetic font of the settings pixel test, for both roles.
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

/// The colour a surface holds at a pixel.
kit::Colour pixel(const renderer::Surface& surface, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
    return {surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]};
}

/// Checks that a pixel holds a colour, and says which when it does not.
void expect(const renderer::Surface& surface, int32_t x, int32_t y, kit::Colour colour) {
    const auto width = static_cast<int32_t>(surface.width);
    const auto height = static_cast<int32_t>(surface.height);
    if (x < 0 || y < 0 || x >= width || y >= height) {
        std::fprintf(stderr, "pixel %d,%d is outside %dx%d\n", x, y, width, height);
        OA_CHECK(false);
        return;
    }
    const kit::Colour got = pixel(surface, x, y);
    if (got != colour) {
        std::fprintf(
            stderr,
            "pixel %d,%d is #%02x%02x%02x, expected #%02x%02x%02x\n",
            x,
            y,
            got.r,
            got.g,
            got.b,
            colour.r,
            colour.g,
            colour.b
        );
        OA_CHECK(false);
    }
}

constexpr kit::Colour kBlack{0, 0, 0};

/// The colour a blend of a token over another colour leaves.
kit::Colour blended(kit::Colour under, kit::Colour over, uint32_t opacity) {
    renderer::Surface one = black(1, 1);
    renderer::fill_source_rect(one, {0, 0, 1, {}}, {0, 0, 1, 1}, kit::rgb(under));
    renderer::blend_source_rect(one, {0, 0, 1, {}}, {0, 0, 1, 1}, kit::rgb(over), opacity);
    return pixel(one, 0, 0);
}

void same(const renderer::Surface& left, const renderer::Surface& right) {
    if (left.rgb != right.rgb) {
        for (uint32_t y = 0; y < left.height; ++y) {
            for (uint32_t x = 0; x < left.width; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * left.width + x) * 3;
                if (left.rgb[at] != right.rgb[at] || left.rgb[at + 1] != right.rgb[at + 1] ||
                    left.rgb[at + 2] != right.rgb[at + 2]) {
                    std::fprintf(stderr, "surfaces differ at %u,%u\n", x, y);
                    y = left.height;
                    break;
                }
            }
        }
    }
    OA_CHECK(left.width == right.width && left.height == right.height && left.rgb == right.rgb);
}

/// A picture of one colour, two by two, whose pixels live as long as it does.
struct OneColourPicture {
    std::array<uint8_t, 16> pixels{};

    explicit OneColourPicture(kit::Colour colour) {
        for (std::size_t at = 0; at < pixels.size(); at += 4) {
            pixels[at] = colour.r;
            pixels[at + 1] = colour.g;
            pixels[at + 2] = colour.b;
            pixels[at + 3] = 255;
        }
    }

    [[nodiscard]] renderer::RgbaPicture picture() const { return {2, 2, pixels}; }
};

// ---- List rows ----

/// The short form: a 20-point badge with its letters, the name over its
/// line, and a chip; the Mods row's places.
void a_short_row_lies_as_the_mods_row() {
    kit::ListRowLook look;
    look.badge_text = "RI";
    look.badge_colour = kit::colour::lock;
    look.title = "Ridge";
    look.lines = {"4.8 rev 3 - 41.0 MB"};
    look.chip = kit::ChipLook{"UPDATE", kit::ChipState::update};
    const kit::Rect rect{10, 10, 300, kit::compact_metrics.short_list_row_height};
    renderer::Surface surface = black(320, 50);
    kit::draw_list_row(canvas_on(surface), rect, look);

    // Nothing under the row is painted while it is neither hovered nor selected.
    expect(surface, 12, 12, kBlack);
    // The badge: 20 square at the inset, on its colour, its letters centred.
    expect(surface, 14, 14, kit::colour::lock);
    expect(surface, 33, 33, kit::colour::lock);
    expect(surface, 34, 33, kBlack);
    expect(surface, 20, 19, kit::colour::on_accent);
    // The title at the Mods row's title line, its line at row 15.
    expect(surface, 38, 14, kit::colour::text);
    expect(surface, 38, 26, kit::colour::list_text);
    // The chip: UPDATE is 15 columns and its padding, at the right inset.
    const kit::Rect chip = kit::list_row_chip_rect(fonts(), rect, *look.chip);
    OA_CHECK(chip.x == 283 && chip.y == 18 && chip.width == 23 && chip.height == 12);
    // A pill of the caution tint.
    expect(surface, 283, 18, kBlack);
    expect(surface, 294, 18, kit::colour::caution_tint);
    expect(surface, 283, 24, kit::colour::caution_tint);
}

/// The full form: a 28-point badge with a picture, the title and its byline,
/// a cut summary, a two-line aside and a chip; selected.
void a_full_row_cuts_its_summary_before_the_aside() {
    const OneColourPicture badge(kit::colour::online);
    kit::ListRowLook look;
    look.badge = badge.picture();
    look.badge_text = "RI";
    look.title = "Ridge";
    look.by = "by the Ridge team";
    look.lines = {
        "Balance overhaul for 3.1c with every unit of both sides rebalanced, new maps to play "
        "and a campaign of its own"
    };
    look.aside = {"4.8 rev 3", "41.0 MB"};
    look.chip = kit::ChipLook{"GET", kit::ChipState::get};
    look.selected = true;
    const kit::Rect rect{10, 50, 400, kit::compact_metrics.list_row_height};
    renderer::Surface surface = black(420, 100);
    kit::draw_list_row(canvas_on(surface), rect, look);

    // Selected: the accent outline, rounded as a control is, and the
    // accent's tint over the face.
    expect(surface, 10, 50, kBlack);
    expect(surface, 200, 50, kit::colour::accent);
    expect(surface, 409, 68, kit::colour::accent);
    expect(surface, 12, 52, blended(kBlack, kit::colour::accent, kit::selected_tint));
    // The picture wins over the letters, outlined, 28 square.
    expect(surface, 14, 54, kit::colour::control_border);
    expect(surface, 20, 60, kit::colour::online);
    expect(surface, 41, 81, kit::colour::control_border);
    // The title, then its byline six columns after it in the hint colour.
    expect(surface, 46, 58, kit::colour::text);
    expect(surface, 69, 58, kit::colour::hint);
    // The aside, right-aligned at 354 to 381, one entry a line.
    expect(surface, 354, 58, kit::colour::text_secondary);
    expect(surface, 362, 70, kit::colour::text_secondary);
    // The chip at the right inset: a pill's left end.
    expect(surface, 388, 68, kit::colour::control_hover);
    // The summary, cut so that nothing of it reaches the aside's gap.
    expect(surface, 47, 70, kit::colour::list_text);
    bool reaches_end = false;
    for (int32_t y = 70; y < 78; ++y) {
        for (int32_t x = 330; x < 348; ++x)
            reaches_end = reaches_end || pixel(surface, x, y) == kit::colour::list_text;
        for (int32_t x = 348; x < 354; ++x)
            OA_CHECK(pixel(surface, x, y) != kit::colour::list_text);
    }
    OA_CHECK(reaches_end);
    const std::string cut =
        kit::cut_to_width(fonts(), kit::FontRole::small, look.lines[0], 348 - 46);
    OA_CHECK(cut.size() > 3 && cut.substr(cut.size() - 3) == "...");
    OA_CHECK(kit::text_width(fonts(), kit::FontRole::small, cut) <= 302);
    OA_CHECK(kit::cut_to_width(fonts(), kit::FontRole::small, "Ridge", 17) == "Ridge");
    OA_CHECK(kit::cut_to_width(fonts(), kit::FontRole::small, "Ridge", 16) == "R...");
    OA_CHECK(kit::cut_to_width(fonts(), kit::FontRole::small, "Ridge", 15) == "...");
    OA_CHECK(kit::cut_to_width(fonts(), kit::FontRole::small, "Ridge", 2) == "...");
}

/// A byline the title leaves too little room for is left out, never drawn as
/// "..." past its box: across the widths of a row whose whole title fits,
/// down to a gap of a column or two after it, no text is wider than its box.
void a_byline_without_room_is_left_out() {
    kit::ListRowLook look;
    look.badge_text = "RI";
    look.title = "Ridge";
    look.by = "by the Ridge team";
    kit::set_text_fit_screen("list row");
    bool shown = false;
    bool left_out = false;
    // The title starts 36 columns into the row and is 17 wide; the row's
    // inset keeps 4 at its right. Below 57 columns the title itself is cut.
    for (int32_t width = 57; width <= 120; ++width) {
        renderer::Surface surface = black(200, 40);
        kit::begin_text_fit_frame();
        kit::draw_list_row(
            canvas_on(surface), {0, 0, width, kit::compact_metrics.list_row_height}, look
        );
        for (const kit::TextFitRecord& noted : kit::text_fit_records()) {
            if (noted.wider)
                std::fprintf(
                    stderr,
                    "a row %d wide draws \"%s\" %d wide in a box of %d\n",
                    width,
                    noted.text.c_str(),
                    noted.text_width,
                    noted.box_width
                );
            OA_CHECK(!noted.wider);
        }
        // Whether any of the byline's hint colour is drawn after the title.
        bool hint = false;
        for (int32_t y = 0; y < 30; ++y)
            for (int32_t x = 59; x < width; ++x)
                hint = hint || pixel(surface, x, y) == kit::colour::hint;
        shown = shown || hint;
        left_out = left_out || !hint;
    }
    kit::begin_text_fit_frame();
    // The narrow rows leave it out, and the wide ones still show it.
    OA_CHECK(left_out);
    OA_CHECK(shown);
}

/// A row with neither picture nor letters draws the blank dashed badge; a
/// taller row puts its tags on the line after its last; hover is the hover face.
void a_blank_badge_and_tags() {
    kit::ListRowLook look;
    look.title = "Ridge";
    look.hovered = true;
    renderer::Surface surface = black(420, 200);
    kit::draw_list_row(canvas_on(surface), {10, 100, 300, 28}, look);
    expect(surface, 12, 102, kit::colour::hover);
    expect(surface, 14, 104, kit::colour::control_border);
    expect(surface, 15, 104, kit::colour::control_border);
    expect(surface, 16, 104, kit::colour::hover);
    expect(surface, 17, 104, kit::colour::control_border);
    expect(surface, 14, 106, kit::colour::hover);
    expect(surface, 14, 107, kit::colour::control_border);
    expect(surface, 16, 123, kit::colour::hover);
    expect(surface, 17, 123, kit::colour::control_border);
    expect(surface, 33, 106, kit::colour::hover);
    expect(surface, 33, 107, kit::colour::control_border);
    expect(surface, 20, 110, kit::colour::hover);

    kit::ListRowLook tagged;
    tagged.badge_text = "RI";
    tagged.badge_colour = kit::colour::accent;
    tagged.title = "Ridge";
    tagged.lines = {"Balance overhaul"};
    tagged.tags = {
        kit::ChipLook{"SIM", kit::ChipState::filter},
        kit::ChipLook{"VIEW", kit::ChipState::filter, true},
    };
    kit::draw_list_row(canvas_on(surface), {10, 140, 400, 48}, tagged);
    // The text is centred: title at 145, its line at 159, the tags at 171,
    // each tag a pill.
    expect(surface, 46, 148, kit::colour::text);
    expect(surface, 47, 160, kit::colour::list_text);
    expect(surface, 46, 171, kBlack);
    expect(surface, 46, 177, kit::colour::control_border);
    expect(surface, 55, 182, kit::colour::control_border);
    expect(surface, 69, 177, kit::colour::accent);
    expect(surface, 70, 177, kit::colour::list_selected);
    // The badge stays 28 in a taller row, centred down it.
    expect(surface, 14, 150, kit::colour::accent);
    expect(surface, 14, 149, kBlack);
}

// ---- Chips ----

/// Draws a GET-wide chip of a state and checks that it is a pill, its
/// corners clear, its border at its top and its left end, its face inside,
/// and its text.
void pin_chip(const kit::ChipLook& look, kit::Colour border, kit::Colour face, kit::Colour text) {
    renderer::Surface surface = black(40, 30);
    kit::draw_chip(canvas_on(surface), {10, 10, 18, 12}, look);
    expect(surface, 10, 10, kBlack);
    expect(surface, 27, 21, kBlack);
    expect(surface, 19, 10, border);
    expect(surface, 10, 16, border);
    expect(surface, 11, 16, face);
    expect(surface, 14, 11, text);
}

void chips_take_every_state() {
    OA_CHECK(kit::chip_width(fonts(), {"GET"}) == 18);
    OA_CHECK(kit::chip_width(fonts(), {"UPDATE"}) == 23);
    OA_CHECK(kit::chip_width(fonts(), {""}) == 8);

    using State = kit::ChipState;
    namespace colour = kit::colour;
    pin_chip({"GET", State::get}, colour::control_hover, kBlack, colour::text_secondary);
    // A state's chip is its colour's tinted fill under words in that colour.
    pin_chip({"GET", State::installed}, colour::ok_tint, colour::ok_tint, colour::accent_light);
    pin_chip({"GET", State::update}, colour::caution_tint, colour::caution_tint, colour::lock);
    pin_chip({"GET", State::playing}, colour::accent, colour::accent, colour::on_accent);
    pin_chip(
        {"GET", State::online},
        colour::information_tint,
        colour::information_tint,
        colour::information
    );
    pin_chip({"GET", State::problem}, colour::danger_tint, colour::danger_tint, colour::danger);
    pin_chip({"GET", State::filter}, colour::control_border, kBlack, colour::text_secondary);
    pin_chip(
        {"GET", State::filter, false, true},
        colour::control_hover,
        colour::hover,
        colour::text_secondary
    );
    pin_chip({"GET", State::filter, true}, colour::accent, colour::list_selected, colour::text);
    pin_chip(
        {"GET", State::get, false, true},
        colour::control_hover,
        colour::hover,
        colour::text_secondary
    );
    pin_chip(
        {"GET", State::installed, false, true},
        colour::control_hover,
        colour::ok_tint,
        colour::accent_light
    );
    pin_chip(
        {"GET", State::playing, false, true}, colour::accent, colour::accent, colour::on_accent
    );
    for (const State state :
         {State::get,
          State::installed,
          State::update,
          State::playing,
          State::online,
          State::problem,
          State::filter}) {
        pin_chip(
            {"GET", state, true, true, true}, colour::control_border, kBlack, colour::switch_idle
        );
    }
}

// ---- Pills ----

/// A pill's corner at 1x is a step, and its cap and its straight edge are
/// its ground: a state's tinted fill, the accent, or the selected surface.
void pills_are_rounded_and_stepped_at_1x() {
    OA_CHECK(kit::pill_width(fonts(), {"GET"}) == 30);
    OA_CHECK(kit::pill_width(fonts(), {""}) == 12);
    kit::PillLook with_icon;
    with_icon.text = "GET";
    with_icon.icon = kit::StateIcon::download_arrow;
    OA_CHECK(kit::pill_width(fonts(), with_icon) == 42);
    OA_CHECK(kit::pill_height() == 12);

    const kit::Rect pill{10, 10, 40, 12};
    const auto pin = [&](kit::PillState state, kit::Colour ground) {
        renderer::Surface surface = black(60, 32);
        kit::draw_pill(canvas_on(surface), pill, {"", state});
        expect(surface, 10, 10, kBlack);
        expect(surface, 10, 16, ground);
        expect(surface, 30, 10, ground);
        expect(surface, 30, 16, ground);
    };
    pin(kit::PillState::playing, kit::colour::accent);
    pin(kit::PillState::installed, kit::colour::ok_tint);
    pin(kit::PillState::neutral, kit::colour::list_selected);
    pin(kit::PillState::update, kit::colour::caution_tint);
    pin(kit::PillState::problem, kit::colour::danger_tint);
    pin(kit::PillState::information, kit::colour::information_tint);
    pin(kit::PillState::brand, kit::colour::brand_tint);

    // A chip of the same words is a pill too.
    renderer::Surface chip = black(60, 32);
    kit::draw_chip(canvas_on(chip), pill, {"", kit::ChipState::playing});
    expect(chip, 10, 10, kBlack);
    expect(chip, 10, 16, kit::colour::accent);
}

/// A selectable pill: active matches Playing, inactive matches Built in,
/// hover shows on the capsule, focus is the focus ring round it, and
/// disabled ignores both.
void a_selectable_pill_shows_its_states() {
    const kit::Rect pill{10, 10, 40, 12};
    renderer::Surface active = black(60, 32);
    kit::draw_pill(canvas_on(active), pill, {"", kit::PillState::active});
    expect(active, 10, 10, kBlack);
    expect(active, 10, 16, kit::colour::accent);
    expect(active, 30, 16, kit::colour::accent);

    renderer::Surface inactive = black(60, 32);
    kit::draw_pill(canvas_on(inactive), pill, {"", kit::PillState::inactive});
    expect(inactive, 10, 10, kBlack);
    expect(inactive, 10, 16, kit::colour::list_selected);
    expect(inactive, 30, 16, kit::colour::list_selected);

    kit::PillLook hovered{"", kit::PillState::active};
    hovered.hovered = true;
    renderer::Surface hot = black(60, 32);
    kit::draw_pill(canvas_on(hot), pill, hovered);
    expect(hot, 10, 16, kit::colour::accent_light);
    expect(hot, 30, 16, kit::colour::accent_light);

    kit::PillLook muted{"", kit::PillState::inactive};
    muted.hovered = true;
    renderer::Surface outlined = black(60, 32);
    kit::draw_pill(canvas_on(outlined), pill, muted);
    expect(outlined, 10, 16, kit::colour::control_hover);
    expect(outlined, 30, 16, kit::colour::list_selected);

    // The focus ring stands a point clear of the pill, a point thick.
    kit::PillLook focused = hovered;
    focused.focused = true;
    renderer::Surface both = black(60, 32);
    kit::draw_pill(canvas_on(both), pill, focused);
    expect(both, 10, 16, kit::colour::accent_light);
    expect(both, 9, 16, kBlack);
    expect(both, 8, 16, kit::colour::focus);
    expect(both, 7, 16, kBlack);
    expect(both, 30, 8, kit::colour::focus);
    expect(both, 30, 16, kit::colour::accent_light);

    kit::PillLook idle_focus{"", kit::PillState::inactive};
    idle_focus.focused = true;
    renderer::Surface focus = black(60, 32);
    kit::draw_pill(canvas_on(focus), pill, idle_focus);
    expect(focus, 10, 16, kit::colour::list_selected);
    expect(focus, 8, 16, kit::colour::focus);
    expect(focus, 30, 16, kit::colour::list_selected);

    kit::PillLook off{"GET", kit::PillState::active};
    off.disabled = true;
    off.hovered = true;
    off.focused = true;
    renderer::Surface disabled = black(60, 32);
    kit::draw_pill(canvas_on(disabled), pill, off);
    expect(disabled, 10, 16, kit::colour::hover);
    expect(disabled, 8, 16, kBlack);
    // The word sits in the middle; the ground is the capsule beside it.
    expect(disabled, 18, 16, kit::colour::hover);
    renderer::Surface enabled = black(60, 32);
    kit::draw_pill(canvas_on(enabled), pill, {"GET", kit::PillState::active});
    bool saw_word = false;
    for (int32_t y = 10; y < 22; ++y) {
        for (int32_t x = 10; x < 50; ++x) {
            if (pixel(enabled, x, y) != kit::colour::on_accent)
                continue;
            expect(disabled, x, y, kit::colour::switch_idle);
            saw_word = true;
        }
    }
    OA_CHECK(saw_word);

    kit::DisplayList list;
    kit::add_pill(list, pill, {"All (11)", kit::PillState::active}, 7, "library.filter.all");
    OA_CHECK(list.controls.size() == 1 && list.controls[0].checked && list.controls[0].enabled);
    kit::PillLook disabled_look{"All (11)", kit::PillState::inactive};
    disabled_look.disabled = true;
    kit::add_pill(list, {0, 20, 40, 12}, disabled_look, 8, "library.filter.new");
    OA_CHECK(!list.controls[1].checked && !list.controls[1].enabled);
}

/// A name with spaces at either end is drawn trimmed, and the maker starts
/// list_row_gap after that drawn name.
void a_trimmed_name_keeps_a_gap_before_the_maker() {
    const kit::Rect rect{0, 0, 320, kit::compact_metrics.list_row_height};
    const auto draw = [&](std::string title) {
        kit::ListRowLook look;
        look.badge_text = "AL";
        look.badge_colour = kit::colour::lock;
        look.title = std::move(title);
        look.by = "by Cavedog Entertainment";
        renderer::Surface surface = black(360, 80);
        kit::Canvas canvas = canvas_on(surface);
        kit::draw_list_row(canvas, rect, look);
        return surface;
    };
    const renderer::Surface spaced = draw("  Lake  ");
    const renderer::Surface plain = draw("Lake");
    same(spaced, plain);

    const int32_t inset = kit::compact_metrics.list_row_inset;
    const int32_t side = std::min(rect.height - 2 * inset, kit::compact_metrics.list_row_badge);
    const int32_t text_left = rect.x + inset + side + inset;
    renderer::Surface scratch = black(1, 1);
    kit::Canvas canvas = canvas_on(scratch);
    const int32_t drawn = kit::measure_text(canvas, kit::FontRole::regular, "Lake");
    const int32_t by_left = text_left + drawn + kit::compact_metrics.list_row_gap;
    bool saw_maker = false;
    for (int32_t y = 0; y < static_cast<int32_t>(plain.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(plain.width); ++x) {
            const kit::Colour ink = pixel(plain, x, y);
            if (ink == kit::colour::text)
                OA_CHECK(x < by_left);
            if (ink == kit::colour::hint) {
                OA_CHECK(x >= by_left);
                saw_maker = true;
            }
        }
    }
    OA_CHECK(saw_maker);

    const renderer::Surface blank = draw(" \t ");
    bool maker_at_the_name = false;
    for (int32_t y = 0; y < static_cast<int32_t>(blank.height); ++y)
        for (int32_t x = text_left; x < text_left + 8; ++x)
            maker_at_the_name = maker_at_the_name || pixel(blank, x, y) == kit::colour::hint;
    OA_CHECK(maker_at_the_name);
}

// ---- State icons ----

/// Tells whether a surface holds black everywhere outside a rectangle.
///
/// @param surface the surface
/// @param inside the rectangle, in pixels
/// @return true when nothing outside it was drawn
bool black_outside(const renderer::Surface& surface, const kit::Rect& inside) {
    for (int32_t y = 0; y < static_cast<int32_t>(surface.height); ++y)
        for (int32_t x = 0; x < static_cast<int32_t>(surface.width); ++x) {
            const bool in = x >= inside.x && x < inside.x + inside.width && y >= inside.y &&
                            y < inside.y + inside.height;
            if (!in && pixel(surface, x, y) != kBlack)
                return false;
        }
    return true;
}

/// Each state icon is worked out at the canvas's own pixels: at 1x and at 2x
/// it stays inside its square; at 2x it is no 1x drawing with each pixel
/// doubled, some of its pixels are wholly its ink and some, its smoothed
/// edges, are mixed with what is under them. The circled icons draw their
/// ring and the download arrow its shaft where the design puts them.
void state_icons_are_drawn_at_their_own_pixels() {
    using Icon = kit::StateIcon;
    const kit::Colour ink = kit::colour::accent;
    for (const Icon icon :
         {Icon::circled_check, Icon::circled_up_arrow, Icon::download_arrow, Icon::key}) {
        renderer::Surface one = black(30, 30);
        kit::draw_state_icon(canvas_on(one), {3, 3, 14, 14}, icon, ink);
        OA_CHECK(black_outside(one, {3, 3, 14, 14}));
        OA_CHECK(!black_outside(one, {0, 0, 0, 0}));

        renderer::Surface two = black(40, 40);
        kit::draw_state_icon(canvas_on(two, 2), {3, 3, 14, 14}, icon, ink);
        OA_CHECK(black_outside(two, {6, 6, 28, 28}));
        bool whole = false;
        bool mixed = false;
        bool finer = false;
        for (int32_t y = 6; y < 34; ++y)
            for (int32_t x = 6; x < 34; ++x) {
                const kit::Colour got = pixel(two, x, y);
                whole = whole || got == ink;
                mixed = mixed || (got != ink && got != kBlack);
                const int32_t left = x - (x - 6) % 2;
                const int32_t top = y - (y - 6) % 2;
                finer = finer || got != pixel(two, left, top);
            }
        OA_CHECK(whole && mixed && finer);
        // The ring's left side, a unit and a half in, middle row; the shaft
        // of the download arrow, in the middle column above the middle.
        if (icon == Icon::download_arrow) {
            expect(two, 6 + 2, 6 + 13, kBlack);
            expect(two, 6 + 13, 6 + 10, ink);
        } else if (icon == Icon::circled_check || icon == Icon::circled_up_arrow) {
            expect(two, 6 + 2, 6 + 13, ink);
        }
    }
    // A wider rectangle centres the icon's square along its length.
    renderer::Surface wide = black(40, 30);
    kit::draw_state_icon(canvas_on(wide), {3, 3, 30, 14}, Icon::circled_check, ink);
    OA_CHECK(black_outside(wide, {11, 3, 14, 14}));
}

/// A chip with a state icon is the icon's side square, wide and high, and
/// draws only the icon, in its state's colour: the hint for get, the accent
/// for installed and playing, the lock colour for update, the hovered
/// border's grey while disabled, on the hover face while hovered. A row
/// places it at its right inset, centred down the row.
void chips_show_state_icons() {
    using State = kit::ChipState;
    using Icon = kit::StateIcon;
    namespace colour = kit::colour;
    const int32_t side = kit::compact_metrics.state_icon_side;
    const kit::ChipLook update{
        "UPDATE", State::update, false, false, false, Icon::circled_up_arrow
    };
    OA_CHECK(kit::chip_width(fonts(), update) == side);
    const kit::Rect row{10, 10, 300, kit::compact_metrics.short_list_row_height};
    const kit::Rect at = kit::list_row_chip_rect(fonts(), row, update);
    OA_CHECK(at.x == 10 + 300 - kit::compact_metrics.list_row_inset - side);
    OA_CHECK(at.y == 10 + (row.height - side) / 2 && at.width == side && at.height == side);

    const auto pin = [&](const kit::ChipLook& look, Icon icon, kit::Colour ink, kit::Colour face) {
        renderer::Surface chip = black(30, 30);
        kit::draw_chip(canvas_on(chip, 2), {1, 1, side, side}, look);
        renderer::Surface direct = black(30, 30);
        if (face != kBlack)
            kit::fill_round_rect(canvas_on(direct, 2), {1, 1, side, side}, side / 2, face);
        kit::draw_state_icon(canvas_on(direct, 2), {1, 1, side, side}, icon, ink);
        same(chip, direct);
        // No outline, and a hovered face is a disc: the square's corner keeps
        // what is under the chip.
        expect(chip, 2, 2, kBlack);
    };
    pin(update, Icon::circled_up_arrow, colour::lock, kBlack);
    pin({"GET", State::get, false, false, false, Icon::download_arrow},
        Icon::download_arrow,
        colour::hint,
        kBlack);
    pin({"INSTALLED", State::installed, false, false, false, Icon::circled_check},
        Icon::circled_check,
        colour::accent,
        kBlack);
    pin({"PLAYING", State::playing, false, false, false, Icon::circled_check},
        Icon::circled_check,
        colour::accent,
        kBlack);
    pin({"GET", State::get, false, false, true, Icon::download_arrow},
        Icon::download_arrow,
        colour::control_hover,
        kBlack);
    pin({"GET", State::get, false, true, false, Icon::download_arrow},
        Icon::download_arrow,
        colour::hint,
        colour::hover);
    // A row draws its chip's icon where list_row_chip_rect puts it.
    kit::ListRowLook look;
    look.title = "Ridge";
    look.chip = update;
    renderer::Surface drawn = black(320, 50);
    kit::draw_list_row(canvas_on(drawn), row, look);
    renderer::Surface icon_only = black(320, 50);
    kit::draw_state_icon(canvas_on(icon_only), at, Icon::circled_up_arrow, colour::lock);
    for (int32_t y = at.y; y < at.y + at.height; ++y)
        for (int32_t x = at.x; x < at.x + at.width; ++x)
            OA_CHECK(pixel(drawn, x, y) == pixel(icon_only, x, y));
}

/// The aside's column is as wide as its widest entry: a version with its
/// revision is drawn whole, never cut.
void the_aside_keeps_a_whole_version() {
    kit::ListRowLook look;
    look.title = "Ridge";
    look.aside = {"1 rev 3", "3.4 MB"};
    look.chip = kit::ChipLook{
        "UPDATE", kit::ChipState::update, false, false, false, kit::StateIcon::circled_up_arrow
    };
    const kit::Rect rect{0, 0, 300, kit::compact_metrics.list_row_height};
    kit::DisplayList list;
    kit::add_list_row(list, rect, look, 1, "row");
    renderer::Surface drawn = black(300, 40);
    kit::paint(canvas_on(drawn), list);
    const int32_t version = kit::text_width(fonts(), kit::FontRole::small, look.aside[0]);
    const int32_t right = 300 - kit::compact_metrics.list_row_inset -
                          kit::compact_metrics.state_icon_side - kit::compact_metrics.list_row_gap;
    // The version's first glyph starts where the whole text, right-aligned, starts.
    bool first_column = false;
    for (int32_t y = 0; y < 40; ++y)
        first_column =
            first_column || pixel(drawn, right - version, y) == kit::colour::text_secondary;
    OA_CHECK(first_column);
}

// ---- Text and search fields ----

void a_search_field_shows_its_placeholder_and_magnifier() {
    kit::SearchLook look;
    look.placeholder = "Search mods";
    renderer::Surface surface = black(140, 40);
    const kit::Rect rect{10, 10, 120, kit::compact_metrics.field_height};
    kit::draw_search(canvas_on(surface), rect, look);
    // A 16-point field's corners are 3 points round.
    expect(surface, 10, 10, kBlack);
    expect(surface, 70, 10, kit::colour::control_border);
    expect(surface, 11, 18, kit::colour::well);
    // The magnifier 4 points in, centred down the field.
    expect(surface, 14, 14, kit::colour::well);
    expect(surface, 15, 14, kit::colour::hint);
    expect(surface, 18, 15, kit::colour::hint);
    expect(surface, 20, 20, kit::colour::hint);
    // The placeholder after the mark.
    const kit::Rect text = kit::field_text_rect(rect, look);
    OA_CHECK(text.x == 25 && text.y == 10 && text.width == 101 && text.height == 16);
    expect(surface, 25, 13, kit::colour::hint);

    look.hovered = true;
    surface = black(140, 40);
    kit::draw_search(canvas_on(surface), rect, look);
    expect(surface, 70, 10, kit::colour::control_hover);

    // The plain text field: no magnifier, the placeholder 4 points in.
    look.magnifier = false;
    look.hovered = false;
    look.placeholder.clear();
    surface = black(140, 40);
    kit::draw_search(canvas_on(surface), rect, look);
    OA_CHECK(kit::field_text_rect(rect, look).x == 14);
    expect(surface, 15, 14, kit::colour::well);
    expect(surface, 20, 20, kit::colour::well);
    look.placeholder = "Search mods";
    kit::draw_search(canvas_on(surface), rect, look);
    expect(surface, 14, 13, kit::colour::hint);
}

void a_focused_field_shows_its_text_and_caret() {
    kit::SearchLook look;
    look.placeholder = "Search mods";
    look.field = {"ridge", 5};
    look.focused = true;
    renderer::Surface surface = black(140, 40);
    const kit::Rect rect{10, 10, 120, 16};
    kit::draw_search(canvas_on(surface), rect, look);
    // Focused: the accent border, and the soft ring two points outside it.
    expect(surface, 70, 10, kit::colour::accent);
    expect(surface, 70, 9, kit::colour::field_ring);
    expect(surface, 70, 8, kit::colour::field_ring);
    expect(surface, 70, 7, kBlack);
    expect(surface, 10, 10, kit::colour::field_ring);
    // r's first column is empty; its second is set.
    expect(surface, 25, 13, kit::colour::well);
    expect(surface, 26, 13, kit::colour::text);
    // The caret after the 17 columns of "ridge", over the capitals' 8 rows.
    expect(surface, 42, 13, kit::colour::well);
    expect(surface, 42, 14, kit::colour::text);
    expect(surface, 42, 21, kit::colour::text);
    expect(surface, 42, 22, kit::colour::well);
    expect(surface, 43, 14, kit::colour::well);

    // Without the focus there is no caret, and no ring.
    look.focused = false;
    surface = black(140, 40);
    kit::draw_search(canvas_on(surface), rect, look);
    expect(surface, 42, 14, kit::colour::well);
    expect(surface, 70, 9, kBlack);

    // Focused on an empty field: the placeholder, and the caret at the start.
    look.field = {};
    look.focused = true;
    surface = black(140, 40);
    kit::draw_search(canvas_on(surface), rect, look);
    expect(surface, 25, 14, kit::colour::text);
    expect(surface, 25, 13, kit::colour::hint);
}

void a_long_text_scrolls_so_its_caret_shows() {
    kit::SearchLook look;
    look.magnifier = false;
    look.focused = true;
    look.field = {"abcdefghijklmnopqrstuvwxyzabcdefghij", 36};
    const kit::Rect rect{10, 40, 60, 16};
    // 126 columns of text in 52, less the caret's column: moved 75 left.
    OA_CHECK(kit::field_scroll(fonts(), rect, look) == 75);
    renderer::Surface surface = black(80, 80);
    kit::draw_search(canvas_on(surface), rect, look);
    expect(surface, 65, 44, kit::colour::text);
    expect(surface, 65, 51, kit::colour::text);
    // Nothing of the text is drawn left of its room.
    for (int32_t y = 43; y < 51; ++y)
        for (int32_t x = 11; x < 14; ++x)
            expect(surface, x, y, kit::colour::well);

    look.field.caret = 0;
    OA_CHECK(kit::field_scroll(fonts(), rect, look) == 0);
    // A caret inside a character counts from that character's start.
    look.field = {"ab\xe4\xb8\xad", 3};
    OA_CHECK(kit::field_scroll(fonts(), rect, look) == 0);
}

void typing_moves_the_caret_drawn() {
    kit::SearchLook look;
    look.placeholder = "Search mods";
    look.focused = true;
    OA_CHECK(kit::insert_text(look.field, "ridge"));
    OA_CHECK(kit::edit_text(look.field, kit::Key::left));
    OA_CHECK(kit::edit_text(look.field, kit::Key::left));
    OA_CHECK(kit::insert_text(look.field, "X"));
    OA_CHECK(look.field.text == "ridXge" && look.field.caret == 4);
    renderer::Surface surface = black(140, 100);
    kit::draw_search(canvas_on(surface), {10, 70, 120, 16}, look);
    // The caret after "ridX", 11 columns from the text's left at 25.
    expect(surface, 36, 74, kit::colour::text);
    expect(surface, 37, 74, kit::colour::well);
    OA_CHECK(kit::edit_text(look.field, kit::Key::backspace));
    OA_CHECK(look.field.text == "ridge" && look.field.caret == 3);
}

// ---- Tabs ----

void tabs_draw_with_a_count_and_with_none() {
    kit::TabsLook look;
    look.captions = {"MODS", "MAPS", "UPDATES"};
    look.counts = {0, 0, 2};
    const kit::Rect strip{10, 10, 300, kit::compact_metrics.tab_height};
    const std::vector<kit::Rect> rects = kit::tab_rects(fonts(), strip, look);
    OA_CHECK(rects.size() == 3u);
    // MODS is 15 columns and three of tracking, and 10 either side.
    OA_CHECK(rects[0].x == 10 && rects[0].y == 10 && rects[0].width == 38 && rects[0].height == 18);
    OA_CHECK(rects[1].x == 48 && rects[1].width == 36);
    // UPDATES and its count: 26, 4 apart, and a pill 12 wide.
    OA_CHECK(rects[2].x == 84 && rects[2].width == 62);

    renderer::Surface surface = black(320, 40);
    kit::draw_tabs(canvas_on(surface), strip, look);
    // The selected tab: its caption in the text colour over the accent rule.
    expect(surface, 20, 13, kit::colour::text);
    expect(surface, 10, 26, kit::colour::accent);
    expect(surface, 47, 27, kit::colour::accent);
    expect(surface, 10, 25, kBlack);
    // The others: secondary captions over the strip's hairline.
    expect(surface, 58, 13, kit::colour::text_secondary);
    expect(surface, 50, 27, kit::colour::rule);
    expect(surface, 50, 26, kBlack);
    expect(surface, 94, 13, kit::colour::text_secondary);
    expect(surface, 300, 27, kit::colour::rule);
    // The count: a pill of the lock colour with its digit in on_accent.
    expect(surface, 124, 12, kBlack);
    expect(surface, 130, 12, kit::colour::lock);
    expect(surface, 130, 23, kit::colour::lock);
    expect(surface, 128, 13, kit::colour::on_accent);
    expect(surface, 123, 12, kBlack);

    // Hovered, a tab's caption is in the text colour.
    look.hovered = 1;
    surface = black(320, 40);
    kit::draw_tabs(canvas_on(surface), strip, look);
    expect(surface, 58, 13, kit::colour::text);
    expect(surface, 94, 13, kit::colour::text_secondary);

    // No count draws no pill; a count of 0 none either.
    look.counts = {};
    look.selected = 2;
    look.hovered.reset();
    OA_CHECK(kit::tab_rects(fonts(), strip, look)[2].width == 46);
    surface = black(320, 40);
    kit::draw_tabs(canvas_on(surface), strip, look);
    expect(surface, 130, 12, kBlack);
    expect(surface, 84, 26, kit::colour::accent);
    expect(surface, 129, 27, kit::colour::accent);
    expect(surface, 130, 27, kit::colour::rule);
    expect(surface, 20, 13, kit::colour::text_secondary);
}

/// Words draw a count of 0, skip a negative count, and read the look's text.
void tabs_in_words_keep_a_count_of_zero() {
    kit::TabsLook words;
    words.count_style = kit::TabCountStyle::words;
    words.captions = {"Mods", "Active", "Updates"};
    words.counts = {0, -1, 4};
    words.count_amber = {0, 0, 1};
    words.readings = {"Mods 0", {}, "Updates 4"};
    const kit::Rect strip{0, 0, 400, kit::compact_metrics.tab_height};
    const std::vector<kit::Rect> with_counts = kit::tab_rects(fonts(), strip, words);
    kit::TabsLook bare = words;
    bare.counts = {-1, -1, -1};
    const std::vector<kit::Rect> plain = kit::tab_rects(fonts(), strip, bare);
    OA_CHECK(with_counts[0].width > plain[0].width);
    OA_CHECK(with_counts[1].width == plain[1].width);
    OA_CHECK(with_counts[2].width > plain[2].width);

    kit::DisplayList list;
    const std::array<kit::ControlId, 3> ids{1, 2, 3};
    const std::array<std::string, 3> names{"mods", "active", "updates"};
    kit::add_tabs(list, fonts(), strip, words, ids, names);
    const kit::Control* mods = kit::control_of(list, 1);
    const kit::Control* active = kit::control_of(list, 2);
    const kit::Control* updates = kit::control_of(list, 3);
    OA_CHECK(mods != nullptr && mods->text == "Mods 0");
    OA_CHECK(active != nullptr && active->text == "Active");
    OA_CHECK(updates != nullptr && updates->text == "Updates 4");
}

/// A strip that need not keep its open tab in view scrolls from its first
/// tab to its last by steps or by points, with the open tab out of view,
/// and a press can drag it between its arrows. A strip the tabs fit has no
/// such area and stays at 0.
void a_free_strip_scrolls_to_either_end() {
    kit::TabsLook look;
    look.captions = {"MODS", "MAPS", "UPDATES", "MODS", "MAPS", "UPDATES"};
    look.counts = {0, 0, 2, 0, 0, 2};
    look.selected = 1;
    look.keep_selected_in_view = false;
    const kit::Rect strip{10, 10, 160, kit::compact_metrics.tab_height};
    const std::array<kit::ControlId, 6> ids{1, 2, 3, 4, 5, 6};
    const std::array<std::string, 6> names{"a", "b", "c", "d", "e", "f"};

    kit::DisplayList list;
    kit::add_tabs(list, fonts(), strip, look, ids, names, 7, "scroll-left", 8, "scroll-right");
    const std::optional<kit::Rect> area = kit::tab_drag_area(list);
    OA_CHECK(area.has_value());
    if (area) {
        OA_CHECK(area->x == strip.x && area->width < strip.width);
        OA_CHECK(area->y == strip.y && area->height == strip.height);
    }
    OA_CHECK(kit::tab_scroll_by(list, 0, -40) == 0);

    // Steps reach the last tab, past the open one.
    int32_t scroll = 0;
    for (int32_t step = 0; step < 12; ++step) {
        kit::DisplayList stepped;
        look.scroll = scroll;
        kit::add_tabs(stepped, fonts(), strip, look, ids, names, 7, "l", 8, "r");
        scroll = kit::tab_scroll_step(stepped, 1);
    }
    look.scroll = scroll;
    const kit::TabStrip end = kit::place_tabs(fonts(), strip, look);
    OA_CHECK(end.scroll == scroll && end.right_arrow.width == 0 && end.left_arrow.width > 0);
    OA_CHECK(kit::wholly_in(end.tabs.back(), end.view));
    OA_CHECK(!kit::wholly_in(end.tabs[look.selected], end.view));

    // Points stop at the same end, and at 0.
    OA_CHECK(kit::tab_scroll_by(list, 0, 10000) == end.scroll);
    const int32_t some = kit::tab_scroll_by(list, 0, 25);
    OA_CHECK(some == 25);
    OA_CHECK(kit::tab_scroll_by(list, end.scroll, -10000) == 0);

    // The same scroll, kept for the open tab, comes back to it.
    look.keep_selected_in_view = true;
    const kit::TabStrip kept = kit::place_tabs(fonts(), strip, look);
    OA_CHECK(kit::wholly_in(kept.tabs[look.selected], kept.view));

    const kit::Rect wide{10, 10, 800, kit::compact_metrics.tab_height};
    kit::DisplayList fitting;
    look.scroll = 0;
    kit::add_tabs(fitting, fonts(), wide, look, ids, names, 7, "l", 8, "r");
    OA_CHECK(!kit::tab_drag_area(fitting).has_value());
    OA_CHECK(kit::tab_scroll_by(fitting, 0, 50) == 0);
}

/// A strip the tabs fit draws no arrows and does not shift them. A narrow
/// one keeps the open tab in view, shows an arrow only toward hidden tabs,
/// and moves one tab at a time.
void a_narrow_strip_scrolls_one_tab_at_a_time() {
    kit::TabsLook look;
    look.captions = {"MODS", "MAPS", "UPDATES", "MODS", "MAPS", "UPDATES"};
    look.counts = {0, 0, 2, 0, 0, 2};
    look.selected = 2;
    const kit::Rect wide{10, 10, 800, kit::compact_metrics.tab_height};
    const kit::TabStrip fitting = kit::place_tabs(fonts(), wide, look);
    const std::vector<kit::Rect> plain = kit::tab_rects(fonts(), wide, look);
    OA_CHECK(!fitting.overflows);
    OA_CHECK(fitting.scroll == 0);
    OA_CHECK(fitting.left_arrow.width == 0 && fitting.right_arrow.width == 0);
    OA_CHECK(fitting.tabs.size() == plain.size());
    for (std::size_t index = 0; index < fitting.tabs.size(); ++index)
        OA_CHECK(
            fitting.tabs[index].x == plain[index].x &&
            fitting.tabs[index].width == plain[index].width
        );

    const kit::Rect strip{10, 10, 160, kit::compact_metrics.tab_height};
    const kit::TabStrip placed = kit::place_tabs(fonts(), strip, look);
    OA_CHECK(placed.overflows);
    OA_CHECK(placed.selected < placed.tabs.size());
    OA_CHECK(kit::wholly_in(placed.tabs[placed.selected], placed.view));
    OA_CHECK((placed.scroll > 0) == (placed.left_arrow.width > 0));
    const int32_t total = plain.back().x + plain.back().width - wide.x;
    OA_CHECK((placed.right_arrow.width > 0) == (placed.scroll + placed.view.width < total));

    kit::DisplayList list;
    const std::array<kit::ControlId, 6> ids{1, 2, 3, 4, 5, 6};
    const std::array<std::string, 6> names{"a", "b", "c", "d", "e", "f"};
    kit::add_tabs(list, fonts(), strip, look, ids, names, 7, "scroll-left", 8, "scroll-right");
    const int32_t forward = kit::tab_scroll_step(list, 1);
    const int32_t back = kit::tab_scroll_step(list, -1);
    OA_CHECK(forward != placed.scroll || back != placed.scroll);
    kit::TabsLook moved = look;
    moved.scroll = forward;
    const kit::TabStrip after = kit::place_tabs(fonts(), strip, moved);
    OA_CHECK(kit::wholly_in(after.tabs[after.selected], after.view));
    moved.scroll = back;
    const kit::TabStrip before = kit::place_tabs(fonts(), strip, moved);
    OA_CHECK(kit::wholly_in(before.tabs[before.selected], before.view));

    look.selected = 0;
    look.scroll = 0;
    const kit::TabStrip start = kit::place_tabs(fonts(), strip, look);
    OA_CHECK(start.scroll == 0 && start.left_arrow.width == 0 && start.right_arrow.width > 0);
    look.selected = look.captions.size() - 1;
    const kit::TabStrip end = kit::place_tabs(fonts(), strip, look);
    OA_CHECK(end.scroll > 0 && end.left_arrow.width > 0 && end.right_arrow.width == 0);
    OA_CHECK(kit::wholly_in(end.tabs.back(), end.view));

    renderer::Surface surface = black(200, 40);
    kit::draw_tabs(canvas_on(surface), strip, look);
    OA_CHECK(end.left_arrow.width > 0);
    const int32_t mark_x = end.left_arrow.x + 4;
    // The arrow is centred down the strip.
    const int32_t mark_y = end.left_arrow.y + end.left_arrow.height / 2 - 2;
    expect(surface, mark_x, mark_y, kit::colour::hint);
    expect(surface, 20, 13, kBlack);
}

// ---- Cards ----

/// Checks a cell's place and size.
bool cell_is(const kit::Rect& cell, int32_t x, int32_t y, int32_t width, int32_t height) {
    return cell.x == x && cell.y == y && cell.width == width && cell.height == height;
}

void cards_lie_in_a_grid() {
    OA_CHECK(kit::card_height(120) == 152);
    OA_CHECK(kit::card_height(124) == 156);
    OA_CHECK(kit::card_height(8) == 40);
    OA_CHECK(kit::card_height(0) == 40);

    // As many as fit at 120, at three widths.
    std::vector<kit::Rect> cells = kit::card_cells({0, 0, 640, 400}, 7);
    OA_CHECK(cells.size() == 7u);
    OA_CHECK(cell_is(cells[0], 0, 0, 124, 156));
    OA_CHECK(cell_is(cells[1], 130, 0, 123, 155));
    OA_CHECK(cell_is(cells[4], 517, 0, 123, 155));
    OA_CHECK(cell_is(cells[5], 0, 162, 124, 156));
    OA_CHECK(cell_is(cells[6], 130, 162, 123, 155));
    cells = kit::card_cells({10, 20, 250, 400}, 3);
    OA_CHECK(cells.size() == 3u);
    OA_CHECK(cell_is(cells[0], 10, 20, 122, 154));
    OA_CHECK(cell_is(cells[1], 138, 20, 122, 154));
    OA_CHECK(cell_is(cells[2], 10, 180, 122, 154));
    cells = kit::card_cells({0, 0, 100, 400}, 2);
    OA_CHECK(cells.size() == 2u);
    OA_CHECK(cell_is(cells[0], 0, 0, 100, 132));
    OA_CHECK(cell_is(cells[1], 0, 138, 100, 132));
    OA_CHECK(kit::card_cells({0, 0, 640, 400}, 0).empty());

    // Exactly 3, 4 and 6 columns.
    cells = kit::card_cells({0, 0, 640, 600}, 4, 3);
    OA_CHECK(cells.size() == 4u);
    OA_CHECK(cell_is(cells[0], 0, 0, 210, 242));
    OA_CHECK(cell_is(cells[1], 216, 0, 209, 241));
    OA_CHECK(cell_is(cells[2], 431, 0, 209, 241));
    OA_CHECK(cell_is(cells[3], 0, 248, 210, 242));
    cells = kit::card_cells({0, 0, 640, 600}, 8, 4);
    OA_CHECK(cells.size() == 8u);
    OA_CHECK(cell_is(cells[0], 0, 0, 156, 188));
    OA_CHECK(cell_is(cells[2], 324, 0, 155, 187));
    OA_CHECK(cell_is(cells[3], 485, 0, 155, 187));
    OA_CHECK(cell_is(cells[7], 485, 194, 155, 187));
    cells = kit::card_cells({0, 0, 640, 600}, 7, 6);
    OA_CHECK(cells.size() == 7u);
    OA_CHECK(cell_is(cells[0], 0, 0, 102, 134));
    OA_CHECK(cell_is(cells[3], 324, 0, 102, 134));
    OA_CHECK(cell_is(cells[4], 432, 0, 101, 133));
    OA_CHECK(cell_is(cells[5], 539, 0, 101, 133));
    OA_CHECK(cell_is(cells[6], 0, 140, 102, 134));
    // Fewer cards than columns keep the columns' widths.
    cells = kit::card_cells({0, 0, 640, 600}, 2, 6);
    OA_CHECK(cells.size() == 2u);
    OA_CHECK(cell_is(cells[1], 108, 0, 102, 134));
    // More columns than the area has room for still place every card.
    cells = kit::card_cells({0, 0, 10, 600}, 3, static_cast<std::size_t>(-1));
    OA_CHECK(cells.size() == 3u);
    cells = kit::card_cells({0, 0, 0, 600}, 2);
    OA_CHECK(cells.size() == 2u);
    OA_CHECK(cell_is(cells[0], 0, 0, 0, 40));
}

void a_card_shows_its_placeholder_chips_and_lines() {
    kit::CardLook look;
    look.placeholder = "Loading preview";
    look.title = "Isle of Ashes";
    look.subtitle = "4 - 16x16";
    look.chips = {
        kit::ChipLook{"BASE", kit::ChipState::get},
        kit::ChipLook{"RIDGE", kit::ChipState::installed},
    };
    const kit::Rect rect{10, 10, 120, kit::card_height(120)};
    renderer::Surface surface = black(140, 170);
    kit::draw_card(canvas_on(surface), rect, look);
    // A card's corners are the card radius round, 8 points at Compact.
    expect(surface, 10, 10, kBlack);
    expect(surface, 70, 10, kit::colour::control_border);
    expect(surface, 11, 60, kit::colour::hover);
    // The preview, 112 square, 4 in: the well and the placeholder centred.
    expect(surface, 14, 14, kit::colour::well);
    expect(surface, 13, 14, kit::colour::hover);
    expect(surface, 125, 125, kit::colour::well);
    expect(surface, 126, 126, kit::colour::hover);
    expect(surface, 45, 65, kit::colour::hint);
    // The chips at the preview's bottom left, 4 in and 4 apart, each a pill.
    expect(surface, 18, 110, kit::colour::well);
    expect(surface, 24, 110, kit::colour::control_hover);
    expect(surface, 29, 121, kit::colour::control_hover);
    expect(surface, 51, 110, kit::colour::ok_tint);
    expect(surface, 17, 110, kit::colour::well);
    // The title and the subtitle under the preview.
    expect(surface, 14, 133, kit::colour::text);
    expect(surface, 14, 147, kit::colour::hint);

    look.selected = true;
    kit::draw_card(canvas_on(surface), rect, look);
    expect(surface, 70, 10, kit::colour::accent);
    look.selected = false;
    look.hovered = true;
    kit::draw_card(canvas_on(surface), rect, look);
    expect(surface, 70, 10, kit::colour::control_hover);
    expect(surface, 11, 60, kit::colour::list_selected);

    // A picture fills the preview instead of the placeholder.
    const OneColourPicture preview(kit::colour::online);
    look.picture = preview.picture();
    kit::draw_card(canvas_on(surface), rect, look);
    expect(surface, 45, 65, kit::colour::online);
    expect(surface, 14, 14, kit::colour::online);
}

void a_greyed_card_fades_into_the_panel() {
    kit::CardLook look;
    look.placeholder = "Loading preview";
    look.title = "Isle of Ashes";
    look.chips = {kit::ChipLook{"BASE", kit::ChipState::get}};
    const kit::Rect rect{10, 10, 120, kit::card_height(120)};
    renderer::Surface plain = black(140, 170);
    kit::draw_card(canvas_on(plain), rect, look);
    renderer::Surface unfaded = plain;
    kit::blend_corner_rect(
        canvas_on(plain),
        rect,
        kit::compact_metrics.card_radius,
        kit::colour::panel,
        kit::locked_fade
    );
    look.greyed = true;
    renderer::Surface greyed = black(140, 170);
    kit::draw_card(canvas_on(greyed), rect, look);
    same(plain, greyed);
    // The fade covers the card's face and leaves its rounded corner clear.
    OA_CHECK(pixel(greyed, 11, 60) != pixel(unfaded, 11, 60));
    expect(greyed, 10, 10, kBlack);

    // A greyed card still takes a press.
    kit::DisplayList list;
    kit::add_card(list, rect, look, 3, "maps.card.isle");
    OA_CHECK(list.controls.size() == 1u && list.controls[0].enabled);
    OA_CHECK(kit::hit(list, {20, 20}) == 3);
}

void the_arrows_move_through_a_grid_of_cards() {
    const std::vector<kit::Rect> cells = kit::card_cells({0, 0, 400, 400}, 6, 3);
    OA_CHECK(cells.size() == 6u);
    kit::DisplayList list;
    for (std::size_t index = 0; index < cells.size(); ++index) {
        kit::CardLook look;
        look.title = "Map " + std::to_string(index + 1);
        const auto id = static_cast<kit::ControlId>(index + 1);
        kit::add_card(list, cells[index], look, id, "maps.card-" + std::to_string(index + 1));
        list.tab_order.push_back(id);
    }
    OA_CHECK(kit::name_problem(list).empty());
    OA_CHECK(kit::focus_toward(list, 1, kit::Direction::right) == 2);
    OA_CHECK(kit::focus_toward(list, 2, kit::Direction::right) == 3);
    OA_CHECK(kit::focus_toward(list, 3, kit::Direction::right) == kit::no_control);
    OA_CHECK(kit::focus_toward(list, 1, kit::Direction::down) == 4);
    OA_CHECK(kit::focus_toward(list, 2, kit::Direction::down) == 5);
    OA_CHECK(kit::focus_toward(list, 6, kit::Direction::up) == 3);
    OA_CHECK(kit::focus_toward(list, 6, kit::Direction::left) == 5);
    OA_CHECK(kit::focus_toward(list, 4, kit::Direction::left) == kit::no_control);
    OA_CHECK(kit::focus_toward(list, 4, kit::Direction::down) == kit::no_control);

    // Left and Right move the focus between cards; Enter goes to the card.
    kit::Interaction interaction;
    interaction.focus_shown = true;
    interaction.focused = 5;
    const kit::KeyOutcome right = kit::key(interaction, list, kit::Key::right);
    OA_CHECK(right.result == kit::KeyResult::redraw && interaction.focused == 6);
    const kit::KeyOutcome up = kit::key(interaction, list, kit::Key::up);
    OA_CHECK(up.result == kit::KeyResult::redraw && interaction.focused == 3);
    const kit::KeyOutcome enter = kit::key(interaction, list, kit::Key::enter);
    OA_CHECK(enter.result == kit::KeyResult::to_control && enter.control == 3);
}

// ---- Hover cards ----

kit::HoverCardLook presence_card() {
    kit::HoverCardLook look;
    look.mark = true;
    look.title = "Open Annihilation 0.8.0";
    look.aside = "Windows x64";
    look.rows = {
        {"MOD", "Ridge 4.8 rev 3", kit::colour::text},
        {"RULES", "Differ from the host's", kit::colour::lock},
        {"", "Dev mode off", kit::colour::accent},
    };
    look.block = {"economy.wind", "ai.income"};
    return look;
}

void a_hover_card_shows_its_header_rows_and_block() {
    kit::HoverCardLook look = presence_card();
    // The header is the widest line: 9 + 6 + 70 + 6 + 39, 6 either side and
    // a point more at the right, where letters may reach past their width.
    // Six lines of 12, and 6 over and under.
    const kit::Point size = kit::hover_card_size(fonts(), look);
    OA_CHECK(size.x == 143 && size.y == 84);
    const kit::Rect rect{40, 40, size.x, size.y};
    renderer::Surface surface = black(240, 180);
    kit::draw_hover_card(canvas_on(surface), rect, look);
    // Rounded as a card is, outlined in the strong edge.
    expect(surface, 40, 40, kBlack);
    expect(surface, 100, 40, kit::colour::strong_edge);
    expect(surface, 182, 80, kit::colour::strong_edge);
    expect(surface, 41, 100, kit::colour::band);
    // The small OA mark, the title after it and the aside at the right.
    expect(surface, 46, 48, kit::colour::band);
    expect(surface, 47, 48, kit::colour::brand);
    expect(surface, 61, 47, kit::colour::text);
    expect(surface, 138, 47, kit::colour::hint);
    // Labels in a column 23 wide, values after it in their colours.
    expect(surface, 46, 59, kit::colour::hint);
    expect(surface, 69, 59, kit::colour::text);
    expect(surface, 46, 71, kit::colour::hint);
    expect(surface, 69, 71, kit::colour::lock);
    // A row without a label draws its value from the left.
    expect(surface, 46, 83, kit::colour::accent);
    // The block in the quiet colour.
    expect(surface, 46, 95, kit::colour::quiet);
    expect(surface, 46, 107, kit::colour::quiet);
    // No arrow.
    expect(surface, 60, 39, kBlack);

    // A fixed width is kept.
    look.width = 100;
    OA_CHECK(kit::hover_card_size(fonts(), look).x == 100);
    OA_CHECK(kit::hover_card_size(fonts(), look).y == 84);
    // No header: the lines alone.
    kit::HoverCardLook bare;
    bare.block = {"economy.wind"};
    const kit::Point bare_size = kit::hover_card_size(fonts(), bare);
    OA_CHECK(bare_size.x == 59 && bare_size.y == 24);
    // The room after the block's last letter is the room before its first
    // and the point letters may reach: the line ends 7 points in from the
    // right, as it starts 6 in from the left.
    renderer::Surface bare_surface = black(80, 40);
    kit::draw_hover_card(canvas_on(bare_surface), {0, 0, bare_size.x, bare_size.y}, bare);
    int32_t first = -1;
    int32_t last = -1;
    for (int32_t x = 0; x < bare_size.x; ++x)
        for (int32_t y = 6; y < 18; ++y)
            if (pixel(bare_surface, x, y) == kit::colour::quiet) {
                if (first < 0)
                    first = x;
                last = x;
            }
    OA_CHECK(first == 6);
    OA_CHECK(last == bare_size.x - 1 - 7);
}

void a_hover_card_points_its_arrow_from_each_side() {
    kit::HoverCardLook look = presence_card();
    const kit::Rect rect{40, 40, 142, 84};

    look.arrow = kit::Side::top;
    look.arrow_at = 20;
    renderer::Surface surface = black(240, 180);
    kit::draw_hover_card(canvas_on(surface), rect, look);
    expect(surface, 60, 35, kit::colour::strong_edge);
    expect(surface, 59, 35, kBlack);
    expect(surface, 61, 35, kBlack);
    expect(surface, 60, 34, kBlack);
    expect(surface, 56, 39, kit::colour::strong_edge);
    expect(surface, 55, 39, kBlack);
    expect(surface, 64, 39, kit::colour::strong_edge);
    expect(surface, 65, 39, kBlack);

    look.arrow = kit::Side::bottom;
    surface = black(240, 180);
    kit::draw_hover_card(canvas_on(surface), rect, look);
    expect(surface, 60, 124, kit::colour::strong_edge);
    expect(surface, 56, 124, kit::colour::strong_edge);
    expect(surface, 55, 124, kBlack);
    expect(surface, 60, 128, kit::colour::strong_edge);
    expect(surface, 59, 128, kBlack);
    expect(surface, 60, 129, kBlack);

    look.arrow = kit::Side::left;
    look.arrow_at = 10;
    surface = black(240, 180);
    kit::draw_hover_card(canvas_on(surface), rect, look);
    expect(surface, 35, 50, kit::colour::strong_edge);
    expect(surface, 35, 49, kBlack);
    expect(surface, 34, 50, kBlack);
    expect(surface, 39, 46, kit::colour::strong_edge);
    expect(surface, 39, 45, kBlack);
    expect(surface, 39, 54, kit::colour::strong_edge);
    expect(surface, 39, 55, kBlack);

    look.arrow = kit::Side::right;
    surface = black(240, 180);
    kit::draw_hover_card(canvas_on(surface), rect, look);
    expect(surface, 182, 46, kit::colour::strong_edge);
    expect(surface, 186, 50, kit::colour::strong_edge);
    expect(surface, 186, 49, kBlack);
    expect(surface, 187, 50, kBlack);
}

/// Checks a rectangle's place and size.
bool rect_is(const kit::Rect& rect, int32_t x, int32_t y, int32_t width, int32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

void a_hover_card_stays_inside_its_frame() {
    const kit::Rect frame{0, 0, 640, 480};
    const kit::Point size{100, 60};
    // Below the anchor, the arrow's depth under it.
    OA_CHECK(rect_is(kit::hover_card_rect({100, 100, 20, 20}, frame, size), 100, 125, 100, 60));
    // At the bottom edge it goes over the anchor.
    OA_CHECK(rect_is(kit::hover_card_rect({100, 440, 20, 20}, frame, size), 100, 375, 100, 60));
    // At the right edge it moves left; at the left edge right.
    OA_CHECK(rect_is(kit::hover_card_rect({600, 100, 20, 20}, frame, size), 540, 125, 100, 60));
    OA_CHECK(
        rect_is(kit::hover_card_rect({10, 100, 20, 20}, {50, 0, 590, 480}, size), 50, 125, 100, 60)
    );
    // At the top edge, an anchor over the frame puts it at the frame's top.
    OA_CHECK(rect_is(kit::hover_card_rect({100, -40, 20, 20}, frame, size), 100, 0, 100, 60));
    // No room below or above: as low as the frame allows.
    OA_CHECK(
        rect_is(kit::hover_card_rect({100, 40, 20, 20}, {0, 0, 640, 100}, size), 100, 40, 100, 60)
    );
    // Larger than the frame: at its top left.
    OA_CHECK(rect_is(kit::hover_card_rect({100, 100, 20, 20}, frame, {700, 500}), 0, 0, 700, 500));
}

void a_hover_card_waits_for_the_pointer_to_settle() {
    OA_CHECK(kit::hover_card_delay_ms == 400);
    kit::HoverTimer timer;
    OA_CHECK(!kit::hover_card_due(timer, 5, 1000));
    OA_CHECK(timer.over == 5 && timer.since_ms == 1000);
    OA_CHECK(!kit::hover_card_due(timer, 5, 1399));
    OA_CHECK(kit::hover_card_due(timer, 5, 1400));
    OA_CHECK(kit::hover_card_due(timer, 5, 2000));
    // Another control starts the wait again.
    OA_CHECK(!kit::hover_card_due(timer, 6, 2100));
    OA_CHECK(!kit::hover_card_due(timer, 6, 2499));
    OA_CHECK(kit::hover_card_due(timer, 6, 2500));
    // No control ends it, and coming back waits again.
    OA_CHECK(!kit::hover_card_due(timer, kit::no_control, 2600));
    OA_CHECK(timer.over == kit::no_control);
    OA_CHECK(!kit::hover_card_due(timer, 6, 2700));
    OA_CHECK(!kit::hover_card_due(timer, 6, 3099));
    OA_CHECK(kit::hover_card_due(timer, 6, 3100));
    // Two timers keep their own waits.
    kit::HoverTimer other;
    OA_CHECK(!kit::hover_card_due(other, 6, 3100));
    OA_CHECK(kit::hover_card_due(timer, 6, 3101));
    OA_CHECK(!kit::hover_card_due(other, 6, 3101));
}

// ---- Links ----

void a_link_in_each_state() {
    kit::LinkLook look{"MORE MAPS..."};
    OA_CHECK(kit::link_width(fonts(), look) == 42);
    const kit::Rect rect{10, 10, 42, kit::compact_metrics.small_line};
    renderer::Surface surface = black(60, 30);
    kit::draw_link(canvas_on(surface), rect, look);
    expect(surface, 10, 11, kit::colour::accent);
    look.hovered = true;
    kit::draw_link(canvas_on(surface), rect, look);
    expect(surface, 10, 11, kit::colour::accent_light);
    look.enabled = false;
    kit::draw_link(canvas_on(surface), rect, look);
    expect(surface, 10, 11, kit::colour::switch_idle);
}

// ---- Display lists ----

/// Every add_ names its controls with what automation reads, and paint of
/// the list draws what the direct draws draw.
void paint_matches_each_direct_draw(int32_t scale) {
    const int32_t width = 460 * scale;
    const int32_t height = 300 * scale;
    renderer::Surface direct = black(width, height);
    renderer::Surface painted = black(width, height);
    kit::DisplayList list;
    const kit::Canvas on_direct = canvas_on(direct, scale);

    const OneColourPicture badge(kit::colour::online);
    kit::ListRowLook row;
    row.badge = badge.picture();
    row.title = "Ridge";
    row.by = "by the Ridge team";
    row.lines = {"Balance overhaul"};
    row.aside = {"4.8 rev 3", "41.0 MB"};
    row.chip = kit::ChipLook{"UPDATE", kit::ChipState::update};
    row.tags = {kit::ChipLook{"SIM", kit::ChipState::filter}};
    row.selected = true;
    const kit::Rect row_rect{4, 4, 400, 48};
    kit::draw_list_row(on_direct, row_rect, row);
    kit::add_list_row(list, row_rect, row, 1, "library.row.ridge");

    const kit::ChipLook chip{"ALL", kit::ChipState::filter, true};
    const kit::Rect chip_rect{4, 56, kit::chip_width(fonts(), chip), 12};
    kit::draw_chip(on_direct, chip_rect, chip);
    kit::add_chip(list, chip_rect, chip, 2, "library.filter.all");
    const kit::ChipLook inside{"BASE", kit::ChipState::get};
    const kit::Rect inside_rect{60, 56, kit::chip_width(fonts(), inside), 12};
    kit::draw_chip(on_direct, inside_rect, inside);
    kit::add_chip(list, inside_rect, inside, kit::no_control, "");

    kit::SearchLook search;
    search.placeholder = "Search mods";
    search.field = {"ridge", 2};
    search.focused = true;
    const kit::Rect search_rect{4, 72, 160, 16};
    kit::draw_search(on_direct, search_rect, search);
    kit::add_search(list, search_rect, search, 3, "library.search");

    kit::TabsLook tabs;
    tabs.captions = {"MODS", "MAPS", "UPDATES"};
    tabs.counts = {0, 0, 2};
    tabs.selected = 1;
    tabs.hovered = 2;
    const kit::Rect strip{4, 92, 300, 18};
    kit::draw_tabs(on_direct, strip, tabs);
    const std::array<kit::ControlId, 3> tab_ids{4, 5, 6};
    const std::array<std::string, 3> tab_names{
        "library.tab.mods", "library.tab.maps", "library.tab.updates"
    };
    kit::add_tabs(list, fonts(), strip, tabs, tab_ids, tab_names);

    kit::CardLook card;
    card.placeholder = "Loading preview";
    card.title = "Isle of Ashes";
    card.subtitle = "4 - 16x16";
    card.chips = {kit::ChipLook{"BASE", kit::ChipState::get}};
    card.greyed = true;
    const kit::Rect card_rect{4, 114, 120, kit::card_height(120)};
    kit::draw_card(on_direct, card_rect, card);
    kit::add_card(list, card_rect, card, 7, "maps.card.isle");

    kit::LinkLook link{"MORE MAPS...", true, true};
    const kit::Rect link_rect{140, 114, kit::link_width(fonts(), link), 12};
    kit::draw_link(on_direct, link_rect, link);
    kit::add_link(list, link_rect, link, 8, "library.more-maps");

    kit::HoverCardLook hover = presence_card();
    hover.arrow = kit::Side::left;
    hover.arrow_at = 12;
    const kit::Point size = kit::hover_card_size(fonts(), hover);
    const kit::Rect hover_rect{200, 140, size.x, size.y};
    kit::draw_hover_card(on_direct, hover_rect, hover);
    kit::add_hover_card(list, hover_rect, hover);

    kit::paint(canvas_on(painted, scale), list);
    same(direct, painted);

    OA_CHECK(list.items.size() == 8u);
    OA_CHECK(list.controls.size() == 8u);
    OA_CHECK(kit::name_problem(list).empty());

    const kit::Control* control = kit::control_of(list, 1);
    OA_CHECK(control != nullptr && control->name == "library.row.ridge");
    OA_CHECK(control->kind == kit::ControlKind::list_item && !control->steps);
    OA_CHECK(control->checked && control->enabled && control->text == "Ridge");

    control = kit::control_of(list, 2);
    OA_CHECK(control != nullptr && control->name == "library.filter.all");
    OA_CHECK(control->kind == kit::ControlKind::button && !control->steps);
    OA_CHECK(control->checked && control->enabled && control->text == "ALL");

    control = kit::control_of(list, 3);
    OA_CHECK(control != nullptr && control->name == "library.search");
    OA_CHECK(control->kind == kit::ControlKind::text_field && control->steps);
    OA_CHECK(!control->checked && control->text == "ridge");

    const std::array<std::string, 3> tab_texts{"MODS", "MAPS", "UPDATES 2"};
    const std::vector<kit::Rect> tab_places = kit::tab_rects(fonts(), strip, tabs);
    for (std::size_t index = 0; index < 3; ++index) {
        control = kit::control_of(list, tab_ids[index]);
        OA_CHECK(control != nullptr && control->name == tab_names[index]);
        OA_CHECK(control->kind == kit::ControlKind::tab && !control->steps);
        OA_CHECK(control->checked == (index == 1));
        OA_CHECK(control->text == tab_texts[index]);
        OA_CHECK(rect_is(
            control->rect,
            tab_places[index].x,
            tab_places[index].y,
            tab_places[index].width,
            tab_places[index].height
        ));
    }

    control = kit::control_of(list, 7);
    OA_CHECK(control != nullptr && control->name == "maps.card.isle");
    OA_CHECK(control->kind == kit::ControlKind::list_item && !control->steps);
    OA_CHECK(control->enabled && control->text == "Isle of Ashes");

    control = kit::control_of(list, 8);
    OA_CHECK(control != nullptr && control->name == "library.more-maps");
    OA_CHECK(control->kind == kit::ControlKind::link && !control->steps);
    OA_CHECK(control->enabled && control->text == "MORE MAPS...");

    // The items carry their roles in the order they were added.
    const std::array<kit::Role, 8> roles{
        kit::Role::list_row,
        kit::Role::chip,
        kit::Role::chip,
        kit::Role::field,
        kit::Role::tabs,
        kit::Role::card,
        kit::Role::link,
        kit::Role::hover_card,
    };
    for (std::size_t index = 0; index < roles.size(); ++index)
        OA_CHECK(list.items[index].role == roles[index]);
}

/// The OA badge and the bone stay inside their square, ignore the ink they
/// are passed, and at 2x are finer than a doubled bitmap. On a row the mark
/// sits under the state icon without moving a row that has no mark.
void maker_marks_sit_under_the_icon() {
    constexpr kit::Colour kPlate{0x3c, 0x40, 0x38};
    constexpr kit::Colour kGold = kit::colour::brand;
    constexpr kit::Colour kOutline{0x0c, 0x0a, 0x08};
    constexpr kit::Colour kBone{0xf4, 0xef, 0xe0};
    const auto count_ink =
        [](const renderer::Surface& surface, const kit::Rect& box, kit::Colour ink) {
            int found = 0;
            for (int32_t y = box.y; y < box.y + box.height; ++y)
                for (int32_t x = box.x; x < box.x + box.width; ++x)
                    found += pixel(surface, x, y) == ink ? 1 : 0;
            return found;
        };

    renderer::Surface badge = black(20, 20);
    kit::draw_state_icon(canvas_on(badge), {1, 1, 14, 14}, kit::StateIcon::oa, kit::colour::danger);
    OA_CHECK(black_outside(badge, {1, 1, 14, 14}));
    OA_CHECK(count_ink(badge, {1, 1, 14, 14}, kPlate) > 0);
    OA_CHECK(count_ink(badge, {1, 1, 14, 14}, kit::colour::danger) == 0);
    OA_CHECK(pixel(badge, 1, 1) != kPlate);

    renderer::Surface bone = black(20, 20);
    kit::draw_state_icon(
        canvas_on(bone), {1, 1, 14, 14}, kit::StateIcon::bone, kit::colour::danger
    );
    OA_CHECK(black_outside(bone, {1, 1, 14, 14}));
    OA_CHECK(count_ink(bone, {1, 1, 14, 14}, kit::colour::danger) == 0);

    for (const kit::StateIcon icon : {kit::StateIcon::oa, kit::StateIcon::bone}) {
        renderer::Surface two = black(40, 40);
        kit::draw_state_icon(canvas_on(two, 2), {3, 3, 14, 14}, icon, kit::colour::danger);
        OA_CHECK(black_outside(two, {6, 6, 28, 28}));
        bool mixed = false;
        bool finer = false;
        for (int32_t y = 6; y < 34; ++y)
            for (int32_t x = 6; x < 34; ++x) {
                const kit::Colour got = pixel(two, x, y);
                mixed = mixed || (got != kBlack && got != kPlate && got != kGold && got != kBone &&
                                  got != kOutline);
                const int32_t left = x - (x - 6) % 2;
                const int32_t top = y - (y - 6) % 2;
                finer = finer || got != pixel(two, left, top);
            }
        OA_CHECK(mixed && finer);
        if (icon == kit::StateIcon::oa) {
            OA_CHECK(count_ink(two, {6, 6, 28, 28}, kPlate) > 0);
            OA_CHECK(count_ink(two, {6, 6, 28, 28}, kGold) > 0);
        } else {
            OA_CHECK(count_ink(two, {6, 6, 28, 28}, kBone) > 0);
            OA_CHECK(count_ink(two, {6, 6, 28, 28}, kOutline) > 0);
        }
    }

    const int32_t side = kit::compact_metrics.state_icon_side;
    const kit::Rect row{10, 10, 300, kit::compact_metrics.list_row_height};
    kit::ListRowLook look;
    look.title = "Ridge";
    look.chip = kit::ChipLook{
        "UPDATE", kit::ChipState::update, false, false, false, kit::StateIcon::circled_up_arrow
    };
    look.maker = kit::MakerMark::open_annihilation;
    const kit::Rect icon = kit::list_row_icon_rect(fonts(), row, look);
    const kit::Rect mark = kit::list_row_maker_rect(row, look);
    const int32_t offset = (row.height - 2 * side) / 2;
    OA_CHECK(icon.y == row.y + offset && icon.width == side && icon.height == side);
    OA_CHECK(mark.x == icon.x && mark.y == icon.y + side && mark.width == side);
    const kit::Rect centred = kit::list_row_chip_rect(fonts(), row, *look.chip);
    OA_CHECK(centred.y == row.y + (row.height - side) / 2);

    renderer::Surface drawn = black(320, 60);
    kit::draw_list_row(canvas_on(drawn), row, look);
    renderer::Surface direct = black(320, 60);
    kit::draw_state_icon(
        canvas_on(direct), icon, kit::StateIcon::circled_up_arrow, kit::colour::lock
    );
    kit::draw_state_icon(canvas_on(direct), mark, kit::StateIcon::oa, kit::colour::text);
    for (const kit::Rect& box : {icon, mark})
        for (int32_t y = box.y; y < box.y + box.height; ++y)
            for (int32_t x = box.x; x < box.x + box.width; ++x)
                OA_CHECK(pixel(drawn, x, y) == pixel(direct, x, y));

    kit::ListRowLook bare;
    bare.maker = kit::MakerMark::bone;
    const kit::Rect alone = kit::list_row_maker_rect(row, bare);
    OA_CHECK(alone.y == row.y + (row.height - side) / 2 && alone.width == side);

    const kit::Rect short_row{0, 0, 200, side};
    kit::ListRowLook cramped = look;
    OA_CHECK(kit::list_row_maker_rect(short_row, cramped).width == 0);
    OA_CHECK(
        kit::list_row_icon_rect(fonts(), short_row, cramped).y ==
        kit::list_row_chip_rect(fonts(), short_row, *cramped.chip).y
    );
}

/// A disabled chip takes no press, and a disabled link none either.
void a_disabled_control_takes_no_press() {
    kit::DisplayList list;
    kit::add_chip(list, {0, 0, 30, 12}, {"GET", kit::ChipState::get, false, false, true}, 1, "c");
    kit::add_link(list, {0, 20, 30, 12}, {"BACK", false, false}, 2, "back");
    OA_CHECK(!list.controls[0].enabled && !list.controls[1].enabled);
    OA_CHECK(kit::hit(list, {5, 5}) == kit::no_control);
    OA_CHECK(kit::hit(list, {5, 25}) == kit::no_control);
}

} // namespace

int main() {
    a_short_row_lies_as_the_mods_row();
    a_full_row_cuts_its_summary_before_the_aside();
    a_byline_without_room_is_left_out();
    a_blank_badge_and_tags();
    chips_take_every_state();
    pills_are_rounded_and_stepped_at_1x();
    a_selectable_pill_shows_its_states();
    a_trimmed_name_keeps_a_gap_before_the_maker();
    state_icons_are_drawn_at_their_own_pixels();
    chips_show_state_icons();
    the_aside_keeps_a_whole_version();
    a_search_field_shows_its_placeholder_and_magnifier();
    a_focused_field_shows_its_text_and_caret();
    a_long_text_scrolls_so_its_caret_shows();
    typing_moves_the_caret_drawn();
    tabs_draw_with_a_count_and_with_none();
    tabs_in_words_keep_a_count_of_zero();
    a_narrow_strip_scrolls_one_tab_at_a_time();
    a_free_strip_scrolls_to_either_end();
    cards_lie_in_a_grid();
    a_card_shows_its_placeholder_chips_and_lines();
    a_greyed_card_fades_into_the_panel();
    the_arrows_move_through_a_grid_of_cards();
    a_hover_card_shows_its_header_rows_and_block();
    a_hover_card_points_its_arrow_from_each_side();
    a_hover_card_stays_inside_its_frame();
    a_hover_card_waits_for_the_pointer_to_settle();
    a_link_in_each_state();
    paint_matches_each_direct_draw(1);
    paint_matches_each_direct_draw(2);
    a_disabled_control_takes_no_press();
    maker_marks_sit_under_the_icon();
    return oa::test::check_exit_status();
}
