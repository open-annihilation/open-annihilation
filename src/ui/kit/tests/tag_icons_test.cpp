// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Tag icons, tool icons, package badges, drag handles and question items. The row, pill,
// nav entry and questions with no items are pinned to the pixels the kit
// drew before those parts existed.

#include "oa/base/sha256.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/tag_icon.hpp"
#include "oa/ui/paint/painter.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;
namespace renderer = oa::ui::frontend_renderer;

constexpr kit::Colour kBlack{0, 0, 0};
constexpr kit::Colour kWhite{255, 255, 255};

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

renderer::Surface black(int32_t width, int32_t height) {
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    return surface;
}

kit::Canvas canvas_on(renderer::Surface& surface, int32_t scale = 1) {
    return {&surface, {0, 0, scale, {}}, &fonts(), {}};
}

kit::Colour pixel(const renderer::Surface& surface, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
    return {surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]};
}

std::string hex_of(const renderer::Surface& surface) {
    const auto text = oa::base::sha256::to_hex(oa::base::sha256::digest_of(surface.rgb));
    return std::string(text.begin(), text.end());
}

bool inside(int32_t x, int32_t y, const kit::Rect& rect) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

kit::ListRowLook row_look() {
    kit::ListRowLook look;
    look.badge_text = "GA";
    look.badge_colour = kit::colour::hint;
    look.title = "Ridge";
    look.by = "the team";
    look.lines = {"a line"};
    return look;
}

kit::PillLook plain_pill() {
    kit::PillLook look;
    look.text = "GET";
    return look;
}

kit::NavLook plain_nav() {
    kit::NavLook look;
    look.area = {1, 20, 90, 100};
    look.rule_column = 91;
    look.divider = kit::Rect{5, 78, 80, 1};
    look.entries.push_back({{8, 28, 70, 20}, "M", true, false, false});
    look.entries.push_back({{8, 50, 70, 20}, "G", false, true, false});
    look.entries.push_back({{8, 100, 70, 20}, "S", false, false, true});
    return look;
}

kit::Question short_question() {
    kit::Question question;
    question.title = "Get Gatehouse?";
    question.paragraphs.push_back({"Install this mutator."});
    question.buttons.push_back({"GET", true, "get"});
    question.buttons.push_back({"CANCEL", false, "cancel"});
    question.cancel_button = 1;
    question.primary_button = 0;
    return question;
}

kit::Question long_question() {
    kit::Question question = short_question();
    std::string text;
    for (int count = 0; count < 24; ++count)
        text += "Ridge keeps the gate open for the team. ";
    question.paragraphs = {{text}};
    return question;
}

struct Smooth {
    paint::Canvas pixels;
    paint::Painter painter;
    kit::Canvas canvas{};

    Smooth(int width, int height, int32_t scale_percent)
        : pixels(paint::make_canvas(width, height)), painter(pixels) {
        canvas.smooth.painter = &painter;
        canvas.smooth.scale_percent = scale_percent;
    }
};

void ids_map_exactly() {
    OA_CHECK(kit::tag_icon_of("statistics") == kit::TagIcon::statistics);
    OA_CHECK(kit::tag_icon_of("economy") == kit::TagIcon::economy);
    OA_CHECK(kit::tag_icon_of("commanders") == kit::TagIcon::commanders);
    OA_CHECK(kit::tag_icon_of("new-units") == kit::TagIcon::new_units);
    OA_CHECK(kit::tag_icon_of("disable") == kit::TagIcon::disable);
    OA_CHECK(kit::tag_icon_of("gameplay") == kit::TagIcon::gameplay);
    OA_CHECK(kit::tag_icon_of("ai") == kit::TagIcon::ai);
    OA_CHECK(kit::tag_icon_of("maps") == kit::TagIcon::maps);
    OA_CHECK(!kit::tag_icon_of("STATISTICS").has_value());
    OA_CHECK(!kit::tag_icon_of("New-Units").has_value());
    OA_CHECK(!kit::tag_icon_of("warp").has_value());
    OA_CHECK(!kit::tag_icon_of("").has_value());
    for (const kit::TagIcon icon : kit::all_tag_icons)
        OA_CHECK(kit::tag_icon_of(kit::tag_id(icon)) == icon);
}

/// Draws a tag icon.
///
/// @param canvas where it is drawn
/// @param rect the rectangle
/// @param icon which icon
/// @param ink the colour
void draw_icon(
    const kit::Canvas& canvas, const kit::Rect& rect, kit::TagIcon icon, kit::Colour ink
) {
    kit::draw_tag_icon(canvas, rect, icon, ink);
}

/// Draws a tool icon.
///
/// @param canvas where it is drawn
/// @param rect the rectangle
/// @param icon which icon
/// @param ink the colour
void draw_icon(
    const kit::Canvas& canvas, const kit::Rect& rect, kit::ToolIcon icon, kit::Colour ink
) {
    kit::draw_tool_icon(canvas, rect, icon, ink);
}

template <class Icon>
void icon_on_crisp(Icon icon, int32_t side, int32_t scale) {
    const int32_t margin = 3;
    const int32_t width = (side + margin * 2) * scale;
    const int32_t height = width;
    const kit::Rect icon_rect{margin, margin, side, side};
    const kit::Rect pixels{margin * scale, margin * scale, side * scale, side * scale};
    renderer::Surface first = black(width, height);
    draw_icon(canvas_on(first, scale), icon_rect, icon, kit::colour::text);
    renderer::Surface second = black(width, height);
    draw_icon(canvas_on(second, scale), icon_rect, icon, kit::colour::text);
    OA_CHECK(first.rgb == second.rgb);
    bool covered = false;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const kit::Colour got = pixel(first, x, y);
            if (!inside(x, y, pixels))
                OA_CHECK(got == kBlack);
            covered = covered || got != kBlack;
        }
    }
    OA_CHECK(covered);
}

template <class Icon>
void two_times_is_not_one_times_doubled(Icon icon, int32_t side) {
    renderer::Surface one = black(side, side);
    draw_icon(canvas_on(one), {0, 0, side, side}, icon, kit::colour::text);
    renderer::Surface two = black(side * 2, side * 2);
    draw_icon(canvas_on(two, 2), {0, 0, side, side}, icon, kit::colour::text);
    bool doubled = true;
    for (int32_t y = 0; y < side && doubled; ++y) {
        for (int32_t x = 0; x < side && doubled; ++x) {
            const kit::Colour source = pixel(one, x, y);
            doubled = pixel(two, x * 2, y * 2) == source &&
                      pixel(two, x * 2 + 1, y * 2) == source &&
                      pixel(two, x * 2, y * 2 + 1) == source &&
                      pixel(two, x * 2 + 1, y * 2 + 1) == source;
        }
    }
    OA_CHECK(!doubled);
}

template <class Icon>
void icon_on_smooth(Icon icon, int32_t side) {
    constexpr int32_t percent = 150;
    const kit::Rect icon_rect{2, 3, side, side};
    const kit::Rect whole{0, 0, side + 6, side + 8};
    const kit::Rect pixels = kit::to_canvas(whole, percent);
    const kit::Rect box = kit::to_canvas(icon_rect, percent);
    Smooth first(pixels.width + 2, pixels.height + 2, percent);
    Smooth second(pixels.width + 2, pixels.height + 2, percent);
    draw_icon(first.canvas, icon_rect, icon, kit::colour::text);
    draw_icon(second.canvas, icon_rect, icon, kit::colour::text);
    bool covered = false;
    for (int y = 0; y < first.pixels.height; ++y) {
        for (int x = 0; x < first.pixels.width; ++x) {
            const paint::Rgba got = paint::pixel_at(first.pixels, x, y);
            const paint::Rgba again = paint::pixel_at(second.pixels, x, y);
            OA_CHECK(got == again);
            const bool in =
                x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
            if (!in)
                OA_CHECK(got.a == 0);
            covered = covered || got.a != 0;
        }
    }
    OA_CHECK(covered);
}

void icons_are_drawn_at_their_own_pixels() {
    constexpr std::array<int32_t, 5> kSides{8, 12, 20, 28, 40};
    constexpr std::array<int32_t, 3> kScales{1, 2, 3};
    for (const kit::TagIcon icon : kit::all_tag_icons) {
        for (const int32_t side : kSides) {
            for (const int32_t scale : kScales)
                icon_on_crisp(icon, side, scale);
            two_times_is_not_one_times_doubled(icon, side);
            icon_on_smooth(icon, side);
        }
    }
    for (std::size_t first = 0; first < kit::all_tag_icons.size(); ++first) {
        renderer::Surface left = black(20, 20);
        kit::draw_tag_icon(
            canvas_on(left), {0, 0, 20, 20}, kit::all_tag_icons[first], kit::colour::text
        );
        for (std::size_t second = first + 1; second < kit::all_tag_icons.size(); ++second) {
            renderer::Surface right = black(20, 20);
            kit::draw_tag_icon(
                canvas_on(right), {0, 0, 20, 20}, kit::all_tag_icons[second], kit::colour::text
            );
            OA_CHECK(left.rgb != right.rgb);
        }
    }
}

/// Counts a surface's pixels that are not black.
///
/// @param surface the surface
/// @return how many pixels hold ink
int32_t inked(const renderer::Surface& surface) {
    int32_t count = 0;
    for (uint32_t y = 0; y < surface.height; ++y)
        for (uint32_t x = 0; x < surface.width; ++x)
            if (pixel(surface, static_cast<int32_t>(x), static_cast<int32_t>(y)) != kBlack)
                ++count;
    return count;
}

void tool_icons_are_drawn_at_their_own_pixels() {
    constexpr std::array<int32_t, 5> kSides{12, 16, 18, 20, 40};
    constexpr std::array<int32_t, 3> kScales{1, 2, 3};
    for (const kit::ToolIcon icon : kit::all_tool_icons) {
        for (const int32_t side : kSides) {
            for (const int32_t scale : kScales)
                icon_on_crisp(icon, side, scale);
            two_times_is_not_one_times_doubled(icon, side);
            icon_on_smooth(icon, side);
        }
    }
    // Each icon is its own drawing at the replay bar's two sizes.
    for (const int32_t side : {18, 20}) {
        for (std::size_t first = 0; first < kit::all_tool_icons.size(); ++first) {
            renderer::Surface left = black(side, side);
            kit::draw_tool_icon(
                canvas_on(left), {0, 0, side, side}, kit::all_tool_icons[first], kit::colour::text
            );
            for (std::size_t second = first + 1; second < kit::all_tool_icons.size(); ++second) {
                renderer::Surface right = black(side, side);
                kit::draw_tool_icon(
                    canvas_on(right),
                    {0, 0, side, side},
                    kit::all_tool_icons[second],
                    kit::colour::text
                );
                OA_CHECK(left.rgb != right.rgb);
            }
        }
    }
    // The turning arrows mirror each other round the same "10": the ring's
    // far half, clear of the numerals, is the other's near half reversed.
    constexpr int32_t kSide = 20;
    renderer::Surface back = black(kSide, kSide);
    kit::draw_tool_icon(canvas_on(back), {0, 0, kSide, kSide}, kit::ToolIcon::back_ten, kWhite);
    renderer::Surface forward = black(kSide, kSide);
    kit::draw_tool_icon(
        canvas_on(forward), {0, 0, kSide, kSide}, kit::ToolIcon::forward_ten, kWhite
    );
    for (int32_t y = 0; y < kSide; ++y) {
        for (int32_t x = 0; x < 5; ++x)
            OA_CHECK(pixel(back, x, y) == pixel(forward, kSide - 1 - x, y));
    }
    // Play and pause are filled: their middles are covered whole.
    for (const kit::ToolIcon icon : {kit::ToolIcon::play, kit::ToolIcon::pause}) {
        renderer::Surface drawn = black(kSide, kSide);
        kit::draw_tool_icon(canvas_on(drawn), {0, 0, kSide, kSide}, icon, kWhite);
        const int32_t column = icon == kit::ToolIcon::play ? 9 : 7;
        OA_CHECK(pixel(drawn, column, 10) == kWhite);
    }
    // A hidden chat's bubble is struck through, and has no lines.
    renderer::Surface shown = black(kSide, kSide);
    kit::draw_tool_icon(canvas_on(shown), {0, 0, kSide, kSide}, kit::ToolIcon::chat, kWhite);
    renderer::Surface hidden = black(kSide, kSide);
    kit::draw_tool_icon(
        canvas_on(hidden), {0, 0, kSide, kSide}, kit::ToolIcon::chat_hidden, kWhite
    );
    OA_CHECK(pixel(shown, 1, 1) == kBlack);
    OA_CHECK(pixel(hidden, 2, 2) != kBlack);
    OA_CHECK(inked(shown) > 0 && inked(hidden) > 0);
    // A rectangle wider than it is high draws the icon in its middle square.
    renderer::Surface wide = black(40, 20);
    kit::draw_tool_icon(canvas_on(wide), {0, 0, 40, 20}, kit::ToolIcon::leave, kWhite);
    for (int32_t y = 0; y < 20; ++y) {
        for (int32_t x = 0; x < 10; ++x) {
            OA_CHECK(pixel(wide, x, y) == kBlack);
            OA_CHECK(pixel(wide, 39 - x, y) == kBlack);
        }
    }
}

std::vector<uint8_t> checker_pixels() {
    std::vector<uint8_t> pixels(4 * 4 * 4, 255);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const kit::Colour ink =
                ((x / 2) ^ (y / 2)) == 0 ? kit::colour::hint : kit::colour::text;
            const std::size_t at =
                (static_cast<std::size_t>(y) * 4 + static_cast<std::size_t>(x)) * 4;
            pixels[at] = ink.r;
            pixels[at + 1] = ink.g;
            pixels[at + 2] = ink.b;
        }
    }
    return pixels;
}

void badges_follow_the_order() {
    const std::vector<uint8_t> kept = checker_pixels();
    kit::BadgeLook picture;
    picture.picture = {4, 4, kept};
    picture.letters = "NO";
    picture.tag = kit::TagIcon::economy;
    renderer::Surface drawn = black(16, 16);
    kit::draw_badge(canvas_on(drawn), {0, 0, 16, 16}, picture);
    OA_CHECK(pixel(drawn, 0, 0) == kit::colour::control_border);
    OA_CHECK(pixel(drawn, 4, 4) != kBlack);
    OA_CHECK(pixel(drawn, 4, 4) != kit::colour::on_accent);

    kit::BadgeLook tagged;
    tagged.tag = kit::TagIcon::statistics;
    tagged.letters = "NO";
    tagged.face = kit::colour::hint;
    renderer::Surface icon = black(20, 20);
    kit::draw_badge(canvas_on(icon), {0, 0, 20, 20}, tagged);
    OA_CHECK(pixel(icon, 0, 0) == kit::colour::hint);
    bool ink = false;
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x)
            ink = ink || pixel(icon, x, y) == kit::colour::on_accent;
    OA_CHECK(ink);

    kit::BadgeLook letters;
    letters.letters = "GA";
    letters.face = kit::colour::hint;
    renderer::Surface words = black(28, 28);
    kit::draw_badge(canvas_on(words), {0, 0, 28, 28}, letters);
    OA_CHECK(pixel(words, 0, 0) == kit::colour::hint);
    bool letter = false;
    for (int y = 0; y < 28; ++y)
        for (int x = 0; x < 28; ++x)
            letter = letter || pixel(words, x, y) == kit::colour::on_accent;
    OA_CHECK(letter);

    renderer::Surface blank = black(12, 12);
    kit::draw_badge(canvas_on(blank), {0, 0, 12, 12}, {});
    OA_CHECK(pixel(blank, 0, 0) == kit::colour::control_border);
    OA_CHECK(pixel(blank, 2, 0) == kBlack);
    OA_CHECK(pixel(blank, 6, 6) == kBlack);
}

void unchanged_parts_keep_their_pixels() {
    const kit::Metrics& metrics = kit::metrics_of(kit::SizeClass::compact);
    renderer::Surface row = black(220, metrics.list_row_height);
    kit::draw_list_row(canvas_on(row), {0, 0, 220, metrics.list_row_height}, row_look());
    OA_CHECK(hex_of(row) == "fb0fba7f9356bd2bfdaf4db5f5b811ffdd050cd1906770bd8461d05f7d21d778");

    const kit::PillLook pill = plain_pill();
    const int32_t pill_across = kit::pill_width(fonts(), pill);
    renderer::Surface pill_surface = black(pill_across, metrics.chip_height);
    kit::draw_pill(canvas_on(pill_surface), {0, 0, pill_across, metrics.chip_height}, pill);
    OA_CHECK(pill_across == 30);
    OA_CHECK(
        hex_of(pill_surface) == "a952fceaa9c263736875cb3c1d13fb9de3be8898fc0bde45d7e141cd91ba32c1"
    );

    renderer::Surface nav = black(120, 140);
    kit::draw_nav(canvas_on(nav), plain_nav());
    OA_CHECK(hex_of(nav) == "1097701854a992b48b7a0e8d1c32ed2b2883c1cccce584125dea90a9e1bc63de");

    const kit::Question brief = short_question();
    const kit::PlacedQuestion placed_brief = kit::place_question(brief, &fonts());
    OA_CHECK(placed_brief.height == 150);
    renderer::Surface brief_surface = black(400, placed_brief.height);
    kit::draw_question(canvas_on(brief_surface), brief);
    OA_CHECK(
        hex_of(brief_surface) == "73e84c3399ce20898a2624b34076a55e8a612a6622024104d08cf24c93f1393a"
    );

    const kit::Question long_text = long_question();
    const kit::PlacedQuestion placed_long = kit::place_question(long_text, &fonts());
    OA_CHECK(placed_long.height == 178);
    renderer::Surface long_surface = black(400, placed_long.height);
    kit::draw_question(canvas_on(long_surface), long_text);
    OA_CHECK(
        hex_of(long_surface) == "7da66e918c9f4876f51565be49d1ee30783c858a30e6dda751d7e5b79afb3a22"
    );
}

void by_colour_changes_only_the_byline() {
    const kit::Metrics& metrics = kit::metrics_of(kit::SizeClass::compact);
    const kit::Rect rect{0, 0, 220, metrics.list_row_height};
    renderer::Surface before = black(rect.width, rect.height);
    kit::draw_list_row(canvas_on(before), rect, row_look());
    kit::ListRowLook coloured = row_look();
    coloured.by_colour = kit::colour::lock;
    renderer::Surface after = black(rect.width, rect.height);
    kit::draw_list_row(canvas_on(after), rect, coloured);
    OA_CHECK(before.rgb != after.rgb);
    bool recolored = false;
    for (int32_t y = 0; y < rect.height; ++y) {
        for (int32_t x = 0; x < rect.width; ++x) {
            const kit::Colour was = pixel(before, x, y);
            const kit::Colour now = pixel(after, x, y);
            if (was == kit::colour::text)
                OA_CHECK(now == kit::colour::text);
            if (was == kit::colour::hint && now == kit::colour::lock)
                recolored = true;
        }
    }
    OA_CHECK(recolored);
}

void grips_take_their_colours() {
    const kit::GripLook looks[] = {
        {},
        {.hovered = true},
        {.held = true},
        {.disabled = true},
    };
    const kit::Colour inks[] = {
        kit::colour::quiet,
        kit::colour::text,
        kit::colour::accent,
        kit::colour::control_hover,
    };
    for (std::size_t index = 0; index < 4; ++index) {
        renderer::Surface surface = black(kit::grip_width, kit::grip_height);
        kit::draw_grip(canvas_on(surface), {0, 0, kit::grip_width, kit::grip_height}, looks[index]);
        OA_CHECK(pixel(surface, 1, 1) == inks[index]);
        OA_CHECK(pixel(surface, 4, 10) == inks[index]);
        OA_CHECK(pixel(surface, 0, 0) != inks[index] || pixel(surface, 0, 0) == kBlack);
    }
    kit::DisplayList list;
    kit::add_grip(list, {1, 2, kit::grip_width, kit::grip_height}, {.held = true}, 7, "grip");
    OA_CHECK(list.items.size() == 1 && list.items[0].role == kit::Role::grip);
    OA_CHECK(list.items[0].state.pressed);
    OA_CHECK(list.controls.size() == 1 && list.controls[0].kind == kit::ControlKind::button);
    OA_CHECK(!list.controls[0].steps && list.controls[0].text.empty() && list.controls[0].enabled);
    kit::DisplayList plain;
    kit::add_grip(plain, {0, 0, kit::grip_width, kit::grip_height}, {}, kit::no_control, {});
    OA_CHECK(plain.controls.empty());
}

void questions_grow_by_their_items() {
    const kit::Question bare = long_question();
    const kit::PlacedQuestion without = kit::place_question(bare, &fonts());
    kit::Question listed = bare;
    kit::QuestionItem gatehouse;
    gatehouse.text = "Gatehouse 4.1";
    gatehouse.aside = " · from Core Prime";
    gatehouse.badge.tag = kit::TagIcon::economy;
    kit::QuestionItem keep;
    keep.text = "Gatehouse Keep 2.0";
    keep.badge.tag = kit::TagIcon::maps;
    listed.items = {gatehouse, keep};
    const kit::PlacedQuestion with = kit::place_question(listed, &fonts());
    const int32_t two_lines = kit::compact_metrics.paragraph_gap + 2 * 20 + 4;
    OA_CHECK(with.height == without.height + two_lines);
    OA_CHECK(with.items.size() == 2);
    OA_CHECK(with.items[1].badge.y == with.items[0].badge.y + 20 + 4);
    const kit::DisplayList list = kit::question_list(listed, &fonts());
    const kit::Control* body = nullptr;
    for (const kit::Control& control : list.controls)
        if (control.name == "prompt.body")
            body = &control;
    OA_CHECK(body != nullptr);
    OA_CHECK(body->text.find("Gatehouse 4.1 · from Core Prime") != std::string::npos);
    OA_CHECK(body->text.find("Gatehouse Keep 2.0") != std::string::npos);
    renderer::Surface drawn = black(400, with.height);
    renderer::Surface painted = black(400, with.height);
    kit::draw_question(canvas_on(drawn), listed);
    kit::paint(canvas_on(painted), list);
    OA_CHECK(drawn.rgb == painted.rgb);
}

} // namespace

int main() {
    ids_map_exactly();
    icons_are_drawn_at_their_own_pixels();
    tool_icons_are_drawn_at_their_own_pixels();
    badges_follow_the_order();
    unchanged_parts_keep_their_pixels();
    by_colour_changes_only_the_byline();
    grips_take_their_colours();
    questions_grow_by_their_items();
    std::puts("ui-kit-tag-icons passed");
    return 0;
}
