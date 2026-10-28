// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Settings screen on the OA layer (settings_menu.hpp).

#include "settings_menu.hpp"
#include "menu_paint.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
using namespace menu_paint;

// The screen's measures, in rows of a window 1080 rows high, as the other
// menus' are.

/// The narrowest width, in the same rows, the measures keep their size in;
/// a narrower window shrinks them to fit its width.
constexpr double design_least_columns = 1480.0;
constexpr double content_columns = 1420.0;    ///< the panels' width together
constexpr double panels_top = 180.75;         ///< the panels' top, under the title
constexpr double panels_rows = 690.0;         ///< the panels' height
constexpr double categories_columns = 330.0;  ///< the CATEGORIES panel's width
constexpr double details_columns = 350.0;     ///< the DETAILS panel's width
constexpr double panel_gap_columns = 14.0;    ///< between two panels
constexpr double band_rows = 46.0;            ///< a panel's label band
constexpr double rim_columns = 11.0;          ///< from a panel's edge to its label's room
constexpr double label_left_columns = 10.0;   ///< from that room's left to its label
constexpr double label_top_rows = 5.0;        ///< from a panel's top to its label
constexpr double label_rows = 34.0;           ///< a label's height
constexpr double inset_columns = 18.0;        ///< inside a panel, round its content
constexpr double entry_rows = 40.0;           ///< a category's plate's height, at most
constexpr double entry_gap_rows = 6.0;        ///< between two categories' plates
constexpr double divider_rows = 22.0;         ///< the room the divider takes
constexpr double icon_columns = 26.0;         ///< a category's picture's side
constexpr double icon_gap_columns = 14.0;     ///< from a plate's left to its picture, and on to its name
constexpr double strip_rows = 34.0;           ///< a section's strip's height
constexpr double strip_gap_rows = 16.0;       ///< above every strip but the first
constexpr double strip_after_rows = 6.0;      ///< between a strip and its first row
constexpr double row_rows = 38.0;             ///< a row's arrows' side, and the row's height
constexpr double row_gap_rows = 6.0;          ///< between two rows
constexpr double row_pad_columns = 14.0;      ///< inside a row, before its label and after its control
constexpr double control_columns = 280.0;     ///< a row's control's width
constexpr double option_gap_columns = 6.0;    ///< between a selector's arrows and its value
constexpr double knob_columns = 22.0;         ///< a slider's knob's width
constexpr double value_columns = 120.0;       ///< a slider's value's width
constexpr double track_rows = 8.0;            ///< a slider's track's height
constexpr double scroll_columns = 20.0;       ///< the scroll bar's width
constexpr double scroll_gap_columns = 8.0;    ///< between the rows and the scroll bar
constexpr double least_thumb_rows = 40.0;     ///< the scroll bar's thumb's least height
constexpr double title_rows = 44.0;           ///< the details' title's line
constexpr double paragraph_gap_rows = 10.0;   ///< between the title and what it does
constexpr double default_rows = 38.0;         ///< the details' default's box
constexpr double note_rows = 30.0;            ///< the details' note on when it takes effect
constexpr double buttons_gap_rows = 18.0;     ///< between the panels and the buttons
constexpr double button_rows = 58.0;          ///< the buttons' height
constexpr double restore_columns = 330.0;     ///< Restore Defaults' width
constexpr double undo_columns = 220.0;        ///< Undo's width
constexpr double cancel_columns = 240.0;      ///< Cancel's width
constexpr double ok_columns = 240.0;          ///< OK's width
constexpr double button_gap_columns = 16.0;   ///< between two buttons of a side
/// The points a line of text needs over its font's size.
constexpr int32_t text_room = 8;
/// The most lines the details' title takes.
constexpr std::size_t most_title_lines = 2;
/// A turn of the wheel scrolls the rows by this share of a row's pitch.
constexpr double wheel_rows_per_notch = 1.5;
/// The window pixels a point takes, in percent: the game's art is drawn in
/// whole pixels of the window.
constexpr int32_t point_percent = 100;

constexpr kit::Colour chosen_colour{232, 164, 52};       ///< amber: the row pointed at or focused
constexpr kit::Colour icon_hole_colour{64, 70, 72};      ///< the hole in the System picture's gear
constexpr kit::Colour row_rule_colour{40, 46, 50};       ///< between two rows
constexpr kit::Colour lock_colour{232, 164, 52};         ///< a lock's padlock, amber
/// How strongly the row under the pointer is lit amber, in 256ths.
constexpr uint32_t row_light_opacity = 34;
/// The shade over the game's art of what cannot be changed, and how much of it.
constexpr kit::Colour grayed_colour{0, 0, 0};
constexpr uint32_t grayed_opacity = 128;
/// A category's picture on the game's button: dark, as its bevel's shade.
constexpr kit::Colour game_icon_colour{40, 44, 40};

/// What a press can reach.
enum class Part : uint8_t {
    none,        ///< nothing
    category,    ///< a category's plate, by its place
    row,         ///< a row, where it has no control
    back,        ///< a selector's left arrow
    next,        ///< a selector's right arrow
    value,       ///< a selector's value, which steps it on
    track,       ///< a slider's track and knob
    button,      ///< one of a row's buttons, by its place
    scroll_up,   ///< the scroll bar's upper arrow
    scroll_down, ///< its lower arrow
    well,        ///< its well, round its thumb
    thumb,       ///< its thumb
    restore,     ///< Restore Defaults
    undo,        ///< Undo
    cancel,      ///< Cancel
    ok,          ///< OK
};

/// A part, and which of its kind: the category, or the row's section, the
/// row and its button.
struct Hit {
    Part part{Part::none}; ///< what it is
    std::size_t section{}; ///< the row's section, or the category
    std::size_t row{};     ///< the row
    std::size_t index{};   ///< the button

    friend bool operator==(const Hit&, const Hit&) = default;
};

/// A row, by its section and its place there.
struct RowAt {
    std::size_t section{}; ///< its section
    std::size_t row{};     ///< its place there

    friend bool operator==(const RowAt&, const RowAt&) = default;
};

/// The four buttons along the bottom, left to right.
enum class Footer : uint8_t { restore, undo, cancel, ok };
/// How many they are.
constexpr std::size_t footer_count = 4;
/// Their captions, in English, in Footer's order.
constexpr std::array<std::string_view, footer_count> footer_captions{
    "RESTORE DEFAULTS", "UNDO", "CANCEL", "OK"
};
/// Their control names, for automation.
constexpr std::array<std::string_view, footer_count> footer_names{
    "restore-defaults", "undo", "cancel", "ok"
};
/// Their quick keys, the letters their captions underline.
constexpr std::array<uint32_t, footer_count> footer_keys{SDLK_R, SDLK_U, SDLK_C, SDLK_O};

/// Returns a footer button's part.
///
/// @param footer the button
/// @return its part
constexpr Part footer_part(Footer footer) noexcept {
    switch (footer) {
    case Footer::restore:
        return Part::restore;
    case Footer::undo:
        return Part::undo;
    case Footer::cancel:
        return Part::cancel;
    case Footer::ok:
        return Part::ok;
    }
    return Part::none;
}

/// Where the screen's fixed parts lie, in its points.
struct SettingsLayout {
    double unit{};                                   ///< the points a row of the 1080-row window takes
    kit::Rect categories{};                          ///< the CATEGORIES panel
    kit::Rect list{};                                ///< inside it, where the plates lie
    kit::Rect center{};                              ///< the chosen category's panel
    kit::Rect view{};                                ///< inside it, where the rows scroll
    kit::Rect scroll_bar{};                          ///< right of the rows, its arrows included
    kit::Rect details{};                             ///< the DETAILS panel
    kit::Rect about{};                               ///< inside it, where its text lies
    std::array<kit::Rect, footer_count> buttons{};   ///< the buttons, in Footer's order
    int32_t entry_height{};                          ///< a category's plate's height, at most
    int32_t entry_gap{};                             ///< between two plates
    int32_t divider{};                               ///< the room the divider takes
    int32_t icon{};                                  ///< a category's picture's side
    int32_t icon_gap{};                              ///< round a category's picture
    int32_t strip_height{};                          ///< a section's strip's height
    int32_t strip_gap{};                             ///< above every strip but the first
    int32_t strip_after{};                           ///< between a strip and its first row
    int32_t row_height{};                            ///< a row's height
    int32_t row_gap{};                               ///< between two rows
    int32_t row_pad{};                               ///< inside a row, before and after
    int32_t control{};                               ///< a row's control's width
    int32_t option_gap{};                            ///< between a selector's arrows and its value
    int32_t knob{};                                  ///< a slider's knob's width
    int32_t value{};                                 ///< a slider's value's width
    int32_t track{};                                 ///< a slider's track's height
    int32_t least_thumb{};                           ///< the thumb's least height
    int32_t title_line{};                            ///< the details' title's line
    int32_t paragraph_gap{};                         ///< under the details' title
    int32_t default_height{};                        ///< the details' default box
    int32_t note_height{};                           ///< the details' note
    int32_t inset{};                                 ///< inside a panel, round its content
    int32_t rim{};                                   ///< from a panel's edge to its label's room
    int32_t label_left{};                            ///< from that room's left to its label
    int32_t label_top{};                             ///< from a panel's top to its label
    int32_t label_height{};                          ///< a label's height
};

/// Lays the screen's fixed parts out on a screen of a size.
///
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @param largest_unit the most points a row of the 1080-row window takes
/// @return where they lie
SettingsLayout lay_out_settings_menu(int32_t width, int32_t height, double largest_unit) {
    SettingsLayout placed;
    const double columns = std::max(width, 1);
    const double rows = std::max(height, 1);
    // A point is this many rows of the 1080-row window; a window narrower
    // than design_least_columns of them takes a smaller one, centred down.
    const double unit = std::min({rows / design_rows, columns / design_least_columns, largest_unit});
    const double top = (rows - design_rows * unit) / 2.0;
    placed.unit = unit;
    const double middle = columns / 2.0;
    const auto across = [&](double design) { return nearest(design * unit); };
    const auto down = [&](double design) { return nearest(top + design * unit); };
    const int32_t regular_line = kit::smooth_text_points(kit::FontRole::regular) + text_room;
    const int32_t small_line = kit::smooth_text_points(kit::FontRole::small) + text_room;

    placed.inset = across(inset_columns);
    placed.rim = across(rim_columns);
    placed.label_left = across(label_left_columns);
    placed.label_top = across(label_top_rows);
    placed.label_height = std::max(across(label_rows), small_line);
    placed.entry_height = std::max(across(entry_rows), regular_line);
    placed.entry_gap = std::max(across(entry_gap_rows), 1);
    placed.divider = std::max(across(divider_rows), 5);
    placed.icon = std::max(across(icon_columns), 8);
    placed.icon_gap = std::max(across(icon_gap_columns), 3);
    placed.strip_height = std::max(across(strip_rows), regular_line);
    placed.strip_gap = across(strip_gap_rows);
    placed.strip_after = across(strip_after_rows);
    placed.row_height = std::max(across(row_rows), regular_line);
    placed.row_gap = std::max(across(row_gap_rows), 1);
    placed.row_pad = across(row_pad_columns);
    placed.control = across(control_columns);
    placed.option_gap = std::max(across(option_gap_columns), 2);
    placed.knob = std::max(across(knob_columns), 6);
    placed.value = std::max(across(value_columns), 24);
    placed.track = std::max(across(track_rows), 3);
    placed.least_thumb = std::max(across(least_thumb_rows), 8);
    placed.title_line = std::max(across(title_rows), regular_line);
    placed.paragraph_gap = across(paragraph_gap_rows);
    placed.default_height = std::max(across(default_rows), small_line + 4);
    placed.note_height = std::max(across(note_rows), small_line);
    const int32_t band = std::max(across(band_rows), placed.label_top + placed.label_height + 2);

    const int32_t left = nearest(middle - content_columns * unit / 2.0);
    const int32_t content = across(content_columns);
    const int32_t panels_y = down(panels_top);
    const int32_t panels_height = across(panels_rows);
    const int32_t gap = across(panel_gap_columns);
    placed.categories = {left, panels_y, across(categories_columns), panels_height};
    const int32_t details_width = across(details_columns);
    placed.details = {left + content - details_width, panels_y, details_width, panels_height};
    const int32_t center_left = placed.categories.x + placed.categories.width + gap;
    placed.center = {
        center_left, panels_y, placed.details.x - gap - center_left, panels_height
    };
    const auto inside = [&](const kit::Rect& panel) {
        return kit::Rect{
            panel.x + placed.inset,
            panel.y + band,
            panel.width - 2 * placed.inset,
            panel.height - band - placed.inset
        };
    };
    placed.list = inside(placed.categories);
    placed.about = inside(placed.details);
    const kit::Rect body = inside(placed.center);
    const int32_t bar = std::max(across(scroll_columns), 8);
    placed.scroll_bar = {body.x + body.width - bar, body.y, bar, body.height};
    placed.view = {
        body.x, body.y, body.width - bar - across(scroll_gap_columns), body.height
    };

    const double buttons_top = panels_top + panels_rows + buttons_gap_rows;
    const int32_t buttons_y = down(buttons_top);
    const int32_t button_height = across(button_rows);
    const int32_t button_gap = across(button_gap_columns);
    const int32_t restore = across(restore_columns);
    const int32_t undo = across(undo_columns);
    const int32_t cancel = across(cancel_columns);
    const int32_t ok = across(ok_columns);
    placed.buttons[static_cast<std::size_t>(Footer::restore)] = {
        left, buttons_y, restore, button_height
    };
    placed.buttons[static_cast<std::size_t>(Footer::undo)] = {
        left + restore + button_gap, buttons_y, undo, button_height
    };
    placed.buttons[static_cast<std::size_t>(Footer::ok)] = {
        left + content - ok, buttons_y, ok, button_height
    };
    placed.buttons[static_cast<std::size_t>(Footer::cancel)] = {
        left + content - ok - button_gap - cancel, buttons_y, cancel, button_height
    };
    return placed;
}

/// How many columns of each end of the game's level bar its frame takes up:
/// its rim and the narrowing round its rod. The knob stays off them.
constexpr int32_t level_end_rim = 6;

/// Gives the four buttons the shape of the game's buttons on their plates:
/// as high as they are, each as wide as its picture is for that height, the
/// first two from the left, the last two from the right.
///
/// @param[in,out] buttons the buttons, in Footer's order
/// @param art the game's buttons, in Footer's order, up and held
/// @param gap between two buttons of a side
void shape_game_buttons(
    std::array<kit::Rect, footer_count>& buttons,
    const std::array<std::array<WindowIcon, 2>, 4>& art,
    int32_t gap
) {
    const kit::Rect& first = buttons[static_cast<std::size_t>(Footer::restore)];
    const kit::Rect& last = buttons[static_cast<std::size_t>(Footer::ok)];
    const int32_t left = first.x;
    const int32_t right = last.x + last.width;
    std::array<int32_t, footer_count> widths{};
    for (std::size_t footer = 0; footer < footer_count; ++footer) {
        const WindowIcon& up = art[footer][0];
        widths[footer] = up.height == 0 ? buttons[footer].width
                                        : nearest(static_cast<double>(buttons[footer].height) *
                                                  up.width / up.height);
    }
    const auto at = [&](Footer footer) -> kit::Rect& {
        return buttons[static_cast<std::size_t>(footer)];
    };
    const auto wide = [&](Footer footer) { return widths[static_cast<std::size_t>(footer)]; };
    at(Footer::restore).x = left;
    at(Footer::restore).width = wide(Footer::restore);
    at(Footer::undo).x = left + wide(Footer::restore) + gap;
    at(Footer::undo).width = wide(Footer::undo);
    at(Footer::ok).x = right - wide(Footer::ok);
    at(Footer::ok).width = wide(Footer::ok);
    at(Footer::cancel).x = right - wide(Footer::ok) - gap - wide(Footer::cancel);
    at(Footer::cancel).width = wide(Footer::cancel);
}

/// Draws a category's picture: a small line drawing in one colour.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param icon which picture
/// @param ink its colour
void draw_icon(
    const kit::Canvas& canvas, const kit::Rect& box, SettingsMenuIcon icon, kit::Colour ink
) {
    const int32_t s = std::min(box.width, box.height);
    if (s < 6)
        return;
    const int32_t x = box.x + (box.width - s) / 2;
    const int32_t y = box.y + (box.height - s) / 2;
    const int32_t line = std::max(s / 12, 1);
    const auto at = [&](double left, double top, double wide, double high) {
        return kit::Rect{
            x + nearest(left * s),
            y + nearest(top * s),
            std::max(nearest(wide * s), 1),
            std::max(nearest(high * s), 1)
        };
    };
    const auto ring = [&](const kit::Rect& rect, int32_t radius) {
        for (int32_t step = 0; step < line; ++step)
            kit::outline_round_rect(
                canvas,
                {rect.x + step, rect.y + step, rect.width - 2 * step, rect.height - 2 * step},
                std::max(radius - step, 0),
                ink
            );
    };
    const auto frame = [&](const kit::Rect& rect) {
        for (int32_t step = 0; step < line; ++step)
            kit::outline_rect(
                canvas,
                {rect.x + step, rect.y + step, rect.width - 2 * step, rect.height - 2 * step},
                ink
            );
    };
    switch (icon) {
    case SettingsMenuIcon::sound:
        kit::fill_rect(canvas, at(0.08, 0.36, 0.18, 0.28), ink);
        kit::fill_rect(canvas, at(0.26, 0.26, 0.08, 0.48), ink);
        kit::fill_rect(canvas, at(0.34, 0.16, 0.08, 0.68), ink);
        kit::fill_rect(canvas, at(0.42, 0.08, 0.06, 0.84), ink);
        kit::fill_rect(canvas, at(0.60, 0.34, 0.07, 0.32), ink);
        kit::fill_rect(canvas, at(0.76, 0.20, 0.07, 0.60), ink);
        break;
    case SettingsMenuIcon::music:
        kit::fill_round_rect(canvas, at(0.10, 0.64, 0.30, 0.24), nearest(0.12 * s), ink);
        kit::fill_rect(canvas, at(0.32, 0.10, 0.08, 0.68), ink);
        kit::fill_round_rect(canvas, at(0.56, 0.54, 0.30, 0.24), nearest(0.12 * s), ink);
        kit::fill_rect(canvas, at(0.78, 0.02, 0.08, 0.66), ink);
        kit::fill_rect(canvas, at(0.32, 0.04, 0.54, 0.12), ink);
        break;
    case SettingsMenuIcon::graphics:
        frame(at(0.04, 0.10, 0.92, 0.62));
        kit::fill_rect(canvas, at(0.44, 0.72, 0.12, 0.12), ink);
        kit::fill_rect(canvas, at(0.24, 0.84, 0.52, 0.08), ink);
        break;
    case SettingsMenuIcon::speeds:
        ring(at(0.10, 0.16, 0.80, 0.80), nearest(0.40 * s));
        kit::fill_rect(canvas, at(0.40, 0.02, 0.20, 0.10), ink);
        kit::fill_rect(canvas, at(0.47, 0.32, 0.07, 0.26), ink);
        kit::fill_rect(canvas, at(0.47, 0.52, 0.24, 0.07), ink);
        break;
    case SettingsMenuIcon::controls:
    case SettingsMenuIcon::controller:
        ring(at(0.02, 0.24, 0.96, 0.56), nearest(0.20 * s));
        kit::fill_rect(canvas, at(0.16, 0.48, 0.24, 0.08), ink);
        kit::fill_rect(canvas, at(0.24, 0.40, 0.08, 0.24), ink);
        kit::fill_round_rect(canvas, at(0.62, 0.36, 0.12, 0.12), nearest(0.06 * s), ink);
        kit::fill_round_rect(canvas, at(0.74, 0.52, 0.12, 0.12), nearest(0.06 * s), ink);
        break;
    case SettingsMenuIcon::interface:
        frame(at(0.04, 0.10, 0.92, 0.80));
        kit::fill_rect(canvas, at(0.04, 0.10, 0.92, 0.18), ink);
        kit::fill_rect(canvas, at(0.16, 0.40, 0.30, 0.38), ink);
        kit::fill_rect(canvas, at(0.56, 0.40, 0.28, 0.08), ink);
        kit::fill_rect(canvas, at(0.56, 0.56, 0.28, 0.08), ink);
        break;
    case SettingsMenuIcon::language:
        ring(at(0.04, 0.08, 0.92, 0.62), nearest(0.18 * s));
        kit::fill_rect(canvas, at(0.22, 0.66, 0.12, 0.24), ink);
        kit::fill_rect(canvas, at(0.22, 0.28, 0.56, 0.08), ink);
        kit::fill_rect(canvas, at(0.22, 0.44, 0.40, 0.08), ink);
        break;
    case SettingsMenuIcon::limits:
        frame(at(0.20, 0.20, 0.60, 0.60));
        kit::fill_rect(canvas, at(0.36, 0.36, 0.28, 0.28), ink);
        for (const double along : {0.28, 0.46, 0.64}) {
            kit::fill_rect(canvas, at(along, 0.04, 0.08, 0.16), ink);
            kit::fill_rect(canvas, at(along, 0.80, 0.08, 0.16), ink);
            kit::fill_rect(canvas, at(0.04, along, 0.16, 0.08), ink);
            kit::fill_rect(canvas, at(0.80, along, 0.16, 0.08), ink);
        }
        break;
    case SettingsMenuIcon::touch:
        kit::fill_round_rect(canvas, at(0.36, 0.36, 0.28, 0.28), nearest(0.14 * s), ink);
        ring(at(0.14, 0.14, 0.72, 0.72), nearest(0.36 * s));
        break;
    case SettingsMenuIcon::game_files:
    case SettingsMenuIcon::page:
        frame(at(0.18, 0.04, 0.64, 0.92));
        for (const double line_top : {0.26, 0.44, 0.62})
            kit::fill_rect(canvas, at(0.30, line_top, 0.40, 0.08), ink);
        break;
    case SettingsMenuIcon::downloads:
        kit::fill_rect(canvas, at(0.44, 0.04, 0.12, 0.40), ink);
        draw_upright_arrow(
            canvas, {x + nearest(0.50 * s), y + nearest(0.70 * s)}, nearest(0.28 * s), true, ink
        );
        kit::fill_rect(canvas, at(0.08, 0.84, 0.84, 0.10), ink);
        break;
    case SettingsMenuIcon::content:
        frame(at(0.08, 0.24, 0.84, 0.68));
        kit::fill_rect(canvas, at(0.08, 0.24, 0.84, 0.16), ink);
        kit::fill_rect(canvas, at(0.40, 0.50, 0.20, 0.10), ink);
        break;
    case SettingsMenuIcon::system:
        for (const auto& tooth :
             {at(0.42, 0.02, 0.16, 0.20), at(0.42, 0.78, 0.16, 0.20), at(0.02, 0.42, 0.20, 0.16),
              at(0.78, 0.42, 0.20, 0.16)})
            kit::fill_rect(canvas, tooth, ink);
        for (const auto& tooth :
             {at(0.12, 0.12, 0.16, 0.16), at(0.72, 0.12, 0.16, 0.16), at(0.12, 0.72, 0.16, 0.16),
              at(0.72, 0.72, 0.16, 0.16)})
            kit::fill_round_rect(canvas, tooth, nearest(0.04 * s), ink);
        kit::fill_round_rect(canvas, at(0.18, 0.18, 0.64, 0.64), nearest(0.32 * s), ink);
        kit::fill_round_rect(
            canvas, at(0.38, 0.38, 0.24, 0.24), nearest(0.12 * s), icon_hole_colour
        );
        break;
    case SettingsMenuIcon::developer:
        draw_arrow(canvas, {x + nearest(0.04 * s), y + s / 2}, nearest(0.26 * s), true, ink);
        draw_arrow(canvas, {x + nearest(0.96 * s), y + s / 2}, nearest(0.26 * s), false, ink);
        kit::fill_rect(canvas, at(0.46, 0.10, 0.08, 0.80), ink);
        break;
    }
}

/// Draws a padlock: its body and its shackle.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param ink its colour
void draw_padlock(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink) {
    const int32_t s = std::min(box.width, box.height);
    if (s < 6)
        return;
    const int32_t x = box.x + (box.width - s) / 2;
    const int32_t y = box.y + (box.height - s) / 2;
    kit::outline_round_rect(
        canvas, {x + s / 4, y, s / 2, s * 3 / 5}, s / 4, ink
    );
    kit::fill_rect(canvas, {x + s / 8, y + s * 2 / 5, s * 3 / 4, s * 3 / 5}, ink);
}

/// The Settings screen as a screen of the OA layer.
///
/// Its points are window pixels, from the room's top left corner, and the
/// screen covers the room.
class SettingsMenuScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param shows tells whether the screen shows
    /// @param facts reads what the settings show now
    /// @param actions what its presses do
    /// @param game_art the game's art it is drawn with
    SettingsMenuScreen(
        std::function<bool()> shows,
        std::function<SettingsMenuFacts()> facts,
        SettingsMenuActions actions,
        SettingsMenuGameArt game_art
    )
        : shows_(std::move(shows)), facts_(std::move(facts)), actions_(std::move(actions)),
          game_buttons_(std::move(game_art.buttons)), painter_(std::move(game_art.common)) {
        facts_now_ = facts_();
    }

    /// Returns the screen's name.
    ///
    /// @return settings_menu_screen_name
    [[nodiscard]] std::string_view name() const override { return settings_menu_screen_name; }

    /// Returns where the screen shows: the whole room, at the window's own
    /// pixels, as the game's art is drawn in whole pixels of the window and
    /// scaled with the window rather than with Interface size; nowhere in a
    /// match.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (placed_on.match || placed_on.room.width <= 0 || placed_on.room.height <= 0)
            return {};
        const kit::Point size = points_of(placed_on);
        const kit::Rect at = kit::to_canvas({0, 0, size.x, size.y}, point_percent);
        return {{placed_on.room.x, placed_on.room.y, at.width, at.height}, size.x, size.y, point_percent};
    }

    /// Tells that the screen takes every input while it shows.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that the screen darkens nothing: it covers what lies under it.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Draws the game's picture and title, its pixels unsmoothed as the
    /// game's own OPTIONS, the three panels, the buttons and the version
    /// plate.
    ///
    /// @param canvas where it draws, at the layer's scale
    void draw(const kit::Canvas& canvas) const override {
        painter_.draw_background(canvas);
        draw_categories(canvas);
        draw_center(canvas);
        draw_details(canvas);
        for (std::size_t footer = 0; footer < footer_count; ++footer)
            draw_footer(canvas, static_cast<Footer>(footer));
    }

    /// Follows the view: the room's size in the layer's points.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (placed_on.match)
            return;
        const kit::Point size = points_of(placed_on);
        if (size.x == size_.x && size.y == size_.y && laid_out_)
            return;
        size_ = size;
        // The parts are laid out on the game's stage, the room round it the
        // picture's alone.
        const kit::Rect stage = game_menu_stage(size_.x, size_.y);
        layout_ = lay_out_settings_menu(
            size_.x, size_.y, std::min(stage.height / design_rows, stage.width / design_least_columns)
        );
        painter_.lay_out(size_.x, size_.y, layout_.unit);
        shape_game_buttons(
            layout_.buttons,
            game_buttons_,
            layout_.buttons[static_cast<std::size_t>(Footer::undo)].x -
                layout_.buttons[static_cast<std::size_t>(Footer::restore)].x -
                layout_.buttons[static_cast<std::size_t>(Footer::restore)].width
        );
        laid_out_ = true;
        fitted_.clear();
        place_content();
        ++revision_;
    }

    /// Follows the pointer over the parts: a press on an arrow, a value, a
    /// button or a category acts when it is released over it; a press on a
    /// slider's track moves its knob there and drags it, one on the scroll
    /// bar's thumb drags the rows, and one on its arrows or its well scrolls
    /// them at once. The right button steps a selector back.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @return redraw when the look changed; none otherwise
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        const uint64_t before = revision_;
        if (kind == ScreenInputKind::pointer_move) {
            if (dragging_ == Part::track) {
                drag_track(at);
                return LayerAnswer::redraw;
            }
            if (dragging_ == Part::thumb) {
                drag_thumb(at);
                return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
            }
            set_hovered(hit_at(at));
            return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
        }
        const bool usable_button = button == SDL_BUTTON_LEFT || button == SDL_BUTTON_RIGHT;
        if (!usable_button)
            return LayerAnswer::none;
        const Hit over = hit_at(at);
        if (kind == ScreenInputKind::pointer_down) {
            set_hovered(over);
            set_pressed(over);
            pressed_button_ = button;
            if (button != SDL_BUTTON_LEFT)
                return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
            switch (over.part) {
            case Part::track:
                dragging_ = Part::track;
                drag_row_ = {over.section, over.row};
                drag_track(at);
                break;
            case Part::thumb:
                dragging_ = Part::thumb;
                thumb_grab_ = at.y - thumb_rect().y;
                break;
            case Part::scroll_up:
            case Part::scroll_down:
                scroll_by(
                    (over.part == Part::scroll_up ? -1 : 1) *
                    (layout_.row_height + layout_.row_gap)
                );
                break;
            case Part::well:
                scroll_by((at.y < thumb_rect().y ? -1 : 1) * std::max(layout_.view.height - layout_.row_height, 1));
                break;
            default:
                break;
            }
            return LayerAnswer::redraw;
        }
        if (kind != ScreenInputKind::pointer_up || button != pressed_button_)
            return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
        const Hit held = pressed_;
        const Part dragged = dragging_;
        dragging_ = Part::none;
        set_pressed({});
        set_hovered(over);
        if (dragged == Part::none && held.part != Part::none && held == over)
            press(held, button == SDL_BUTTON_RIGHT);
        return LayerAnswer::redraw;
    }

    /// Takes the four buttons' quick keys, R, U, C and O, without a modifier.
    ///
    /// @param sdl_key the key's SDL keycode
    /// @param modifiers the modifier keys held
    /// @return redraw when it acted; pass otherwise
    LayerAnswer shortcut(uint32_t sdl_key, uint16_t modifiers) override {
        if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0)
            return LayerAnswer::pass;
        for (std::size_t footer = 0; footer < footer_count; ++footer)
            if (sdl_key == footer_keys[footer]) {
                press({footer_part(static_cast<Footer>(footer)), 0, 0, 0}, false);
                return LayerAnswer::redraw;
            }
        return LayerAnswer::pass;
    }

    /// Takes the keys: Up and Down move the focus along the rows, Left and
    /// Right step the focused row, Space steps it on or presses its first
    /// button, Tab and Page Down show the next category and Shift+Tab and
    /// Page Up the one before, Home and End scroll to the top and the end,
    /// Return is OK and Escape Cancel.
    ///
    /// @param pressed the key
    /// @return redraw when it acted; none otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        switch (pressed) {
        case kit::Key::enter:
            press({Part::ok, 0, 0, 0}, false);
            return LayerAnswer::redraw;
        case kit::Key::escape:
            press({Part::cancel, 0, 0, 0}, false);
            return LayerAnswer::redraw;
        case kit::Key::up:
        case kit::Key::down:
            move_focus(pressed == kit::Key::down);
            return LayerAnswer::redraw;
        case kit::Key::left:
        case kit::Key::right:
        case kit::Key::space: {
            const SettingsMenuRow* row = focused_row();
            if (row == nullptr || !row->enabled)
                return LayerAnswer::none;
            if (row->control == SettingsMenuControl::buttons) {
                if (pressed == kit::Key::space && !row->buttons.empty())
                    press({Part::button, focused_->section, focused_->row, 0}, false);
                return LayerAnswer::redraw;
            }
            if (row->control == SettingsMenuControl::text)
                return LayerAnswer::none;
            press(
                {pressed == kit::Key::left ? Part::back : Part::next,
                 focused_->section,
                 focused_->row,
                 0},
                false
            );
            return LayerAnswer::redraw;
        }
        case kit::Key::tab:
        case kit::Key::page_down:
        case kit::Key::back_tab:
        case kit::Key::page_up: {
            const std::size_t count = facts_now_.categories.size();
            if (count == 0)
                return LayerAnswer::none;
            const bool forward = pressed == kit::Key::tab || pressed == kit::Key::page_down;
            const std::size_t next =
                forward ? (facts_now_.chosen + 1) % count : (facts_now_.chosen + count - 1) % count;
            press({Part::category, next, 0, 0}, false);
            return LayerAnswer::redraw;
        }
        case kit::Key::home:
            scroll_to(0);
            return LayerAnswer::redraw;
        case kit::Key::end:
            scroll_to(scroll_limit());
            return LayerAnswer::redraw;
        default:
            return LayerAnswer::none;
        }
    }

    /// Scrolls the rows when the pointer is over the chosen category's panel.
    ///
    /// @param at the pointer, in the screen's points
    /// @param notches how far the wheel turned, positive away from the player
    /// @return redraw when the rows scrolled; none otherwise
    LayerAnswer wheel(kit::Point at, float notches) override {
        if (!kit::contains(layout_.center, at))
            return LayerAnswer::none;
        const int32_t before = scroll_;
        scroll_by(static_cast<int32_t>(std::lround(
            -notches * wheel_rows_per_notch * (layout_.row_height + layout_.row_gap)
        )));
        if (scroll_ == before)
            return LayerAnswer::none;
        set_hovered(hit_at(at));
        return LayerAnswer::redraw;
    }

    /// Returns the parts a press can reach, one named control each, for
    /// automation: each category as nav.<word>, each row's arrows as
    /// <word>-previous and <word>-next, a slider's track as <word>, a row's
    /// buttons as <word>.<button>, then the scroll bar's arrows and the four
    /// buttons; only the rows the view shows.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        std::set<std::string> taken;
        const auto add = [&](Hit hit, const kit::Rect& rect, std::string name, std::string text,
                             bool checked, kit::ControlKind kind) {
            std::string full = std::string(settings_menu_screen_name) + "." + std::move(name);
            if (!taken.insert(full).second) {
                for (int32_t again = 2;; ++again)
                    if (taken.insert(full + "-" + std::to_string(again)).second) {
                        full += "-" + std::to_string(again);
                        break;
                    }
            }
            kit::Control control;
            control.id = control_id(hit);
            control.rect = rect;
            control.enabled = enabled(hit);
            control.name = std::move(full);
            control.kind = kind;
            control.text = std::move(text);
            control.checked = checked;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(control_id(hit));
        };
        for (std::size_t entry = 0; entry < facts_now_.categories.size(); ++entry) {
            const SettingsMenuCategory& category = facts_now_.categories[entry];
            add({Part::category, entry, 0, 0},
                entry_rect(entry),
                "nav." + word_of(category.id),
                category.name,
                entry == facts_now_.chosen,
                kit::ControlKind::button);
        }
        for (std::size_t section = 0; section < facts_now_.sections.size(); ++section) {
            const auto& rows = facts_now_.sections[section].rows;
            for (std::size_t index = 0; index < rows.size(); ++index) {
                const SettingsMenuRow& row = rows[index];
                const kit::Rect whole = row_rect(section, index);
                if (!kit::contains(layout_.view, {whole.x, whole.y}) ||
                    !kit::contains(layout_.view, {whole.x, whole.y + whole.height - 1}))
                    continue;
                const std::string word = word_of(row.id);
                switch (row.control) {
                case SettingsMenuControl::selector:
                    add({Part::back, section, index, 0}, arrow_rect(section, index, true),
                        word + "-previous", row.value, false, kit::ControlKind::button);
                    add({Part::next, section, index, 0}, arrow_rect(section, index, false),
                        word + "-next", row.value, false, kit::ControlKind::button);
                    break;
                case SettingsMenuControl::slider:
                    add({Part::track, section, index, 0}, track_rect(section, index), word,
                        row.value, false, kit::ControlKind::slider);
                    break;
                case SettingsMenuControl::buttons:
                    for (std::size_t at = 0; at < row.buttons.size(); ++at)
                        add({Part::button, section, index, at}, button_rect(section, index, at),
                            row.buttons.size() == 1 ? word : word + "." + std::to_string(at + 1),
                            row.buttons[at], false, kit::ControlKind::button);
                    break;
                case SettingsMenuControl::text:
                    break;
                }
            }
        }
        if (scroll_limit() > 0) {
            add({Part::scroll_up, 0, 0, 0}, scroll_arrow_rect(true), "scroll-up", {}, false,
                kit::ControlKind::button);
            add({Part::scroll_down, 0, 0, 0}, scroll_arrow_rect(false), "scroll-down", {}, false,
                kit::ControlKind::button);
        }
        for (std::size_t footer = 0; footer < footer_count; ++footer)
            add({footer_part(static_cast<Footer>(footer)), 0, 0, 0},
                layout_.buttons[footer],
                std::string(footer_names[footer]),
                worded(footer_captions[footer]),
                false,
                kit::ControlKind::button);
        return list;
    }

    /// Returns the hovered and pressed parts, and the focused row's right
    /// arrow while a key moved the focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown;
        shown.hovered = hovered_.part != Part::none ? control_id(hovered_) : kit::no_control;
        shown.pressed = pressed_.part != Part::none ? control_id(pressed_) : kit::no_control;
        if (focused_) {
            shown.focused = control_id({Part::next, focused_->section, focused_->row, 0});
            shown.focus_shown = true;
        }
        return shown;
    }

    /// Closes the screen when it no longer shows, and follows what the
    /// settings show.
    ///
    /// @return close when the screen no longer shows; redraw when what it
    ///     shows changed; otherwise none
    LayerAnswer tick() override {
        if (!shows_())
            return LayerAnswer::close;
        return refresh() ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns a number that changes with what the screen draws.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that the screen draws nothing that moves on its own.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Has the settings put back, as Cancel does, when the screen leaves
    /// while it still shows.
    void close(bool /*by_key*/) override {
        if (closing_ || !shows_() || !actions_.cancel)
            return;
        closing_ = true;
        actions_.cancel();
    }

  private:

    /// Returns the room's size in the screen's points, one a window pixel.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the width and height, in points
    [[nodiscard]] static kit::Point points_of(const LayerView& placed_on) noexcept {
        return {placed_on.room.width, placed_on.room.height};
    }

    /// Returns a word fit for an automation name: lower case letters,
    /// digits and hyphens.
    ///
    /// @param text the word as given
    /// @return the word; "row" for one with none of them
    [[nodiscard]] static std::string word_of(std::string_view text) {
        std::string word;
        for (const char character : text) {
            if (character >= 'A' && character <= 'Z')
                word += static_cast<char>(character - 'A' + 'a');
            else if ((character >= 'a' && character <= 'z') ||
                     (character >= '0' && character <= '9') || character == '-')
                word += character;
            else if (!word.empty() && word.back() != '-')
                word += '-';
        }
        while (!word.empty() && word.back() == '-')
            word.pop_back();
        return word.empty() ? std::string("row") : word;
    }

    /// Returns a part's control number.
    ///
    /// @param hit the part
    /// @return its number
    [[nodiscard]] static kit::ControlId control_id(Hit hit) noexcept {
        // Each kind has a million numbers: ten thousand a section, ten a row.
        return static_cast<kit::ControlId>(
            static_cast<int32_t>(hit.part) * 1000000 +
            static_cast<int32_t>(std::min<std::size_t>(hit.section, 99)) * 10000 +
            static_cast<int32_t>(std::min<std::size_t>(hit.row, 999)) * 10 +
            static_cast<int32_t>(std::min<std::size_t>(hit.index, 9))
        );
    }

    /// Reads the facts again, and places the rows again when they changed.
    ///
    /// @return true when what the screen shows changed
    bool refresh() {
        SettingsMenuFacts now = facts_();
        if (now == facts_now_)
            return false;
        // By the category's word, not its name: a language chosen on the
        // screen renames it and keeps the view where it is.
        const auto word_of = [](const SettingsMenuFacts& facts) {
            return facts.chosen < facts.categories.size() ? facts.categories[facts.chosen].id
                                                          : std::string();
        };
        const bool other_category = now.chosen != facts_now_.chosen ||
                                    word_of(now) != word_of(facts_now_) ||
                                    now.sections.size() != facts_now_.sections.size();
        facts_now_ = std::move(now);
        if (other_category) {
            scroll_ = 0;
            focused_.reset();
            shown_row_.reset();
        }
        place_content();
        if (focused_ && row_of(*focused_) == nullptr)
            focused_.reset();
        if (shown_row_ && row_of(*shown_row_) == nullptr)
            shown_row_.reset();
        if (!enabled(hovered_))
            hovered_ = {};
        ++revision_;
        return true;
    }

    /// Places the chosen category's strips and rows from the top of the
    /// view, before it scrolls, and holds the scroll to what they need.
    void place_content() {
        strips_.clear();
        rows_.clear();
        int32_t y = 0;
        const int32_t pitch = layout_.row_height + layout_.row_gap;
        for (std::size_t section = 0; section < facts_now_.sections.size(); ++section) {
            if (section > 0)
                y += layout_.strip_gap;
            strips_.push_back({layout_.view.x, y, layout_.view.width, layout_.strip_height});
            y += layout_.strip_height + layout_.strip_after;
            auto& placed = rows_.emplace_back();
            for (std::size_t row = 0; row < facts_now_.sections[section].rows.size(); ++row) {
                placed.push_back({layout_.view.x, y, layout_.view.width, layout_.row_height});
                y += pitch;
            }
        }
        content_height_ = y;
        scroll_ = std::clamp(scroll_, 0, scroll_limit());
    }

    /// Returns how far the rows scroll at most.
    ///
    /// @return the points; 0 when they fit the view
    [[nodiscard]] int32_t scroll_limit() const noexcept {
        return std::max(content_height_ - layout_.view.height, 0);
    }

    /// Scrolls the rows to an offset, held to what they need.
    ///
    /// @param offset the offset, in points from their top
    void scroll_to(int32_t offset) {
        const int32_t held = std::clamp(offset, 0, scroll_limit());
        if (held == scroll_)
            return;
        scroll_ = held;
        ++revision_;
    }

    /// Scrolls the rows by a distance.
    ///
    /// @param distance the points, positive towards their end
    void scroll_by(int32_t distance) { scroll_to(scroll_ + distance); }

    /// Returns a row of the facts.
    ///
    /// @param at the row
    /// @return the row; null for none there
    [[nodiscard]] const SettingsMenuRow* row_of(RowAt at) const noexcept {
        if (at.section >= facts_now_.sections.size())
            return nullptr;
        const auto& rows = facts_now_.sections[at.section].rows;
        return at.row < rows.size() ? &rows[at.row] : nullptr;
    }

    /// Returns the focused row.
    ///
    /// @return the row; null while no row has the focus
    [[nodiscard]] const SettingsMenuRow* focused_row() const noexcept {
        return focused_ ? row_of(*focused_) : nullptr;
    }

    /// Moves the focus to the next or the previous row, scrolling the least
    /// that shows it whole.
    ///
    /// @param forward to the next row; otherwise the one before
    void move_focus(bool forward) {
        std::vector<RowAt> order;
        for (std::size_t section = 0; section < facts_now_.sections.size(); ++section)
            for (std::size_t row = 0; row < facts_now_.sections[section].rows.size(); ++row)
                order.push_back({section, row});
        if (order.empty())
            return;
        std::size_t next = forward ? 0 : order.size() - 1;
        if (focused_) {
            const auto found = std::find(order.begin(), order.end(), *focused_);
            if (found != order.end()) {
                const auto at = static_cast<std::size_t>(found - order.begin());
                next = forward ? std::min(at + 1, order.size() - 1) : (at > 0 ? at - 1 : 0);
            }
        }
        focused_ = order[next];
        shown_row_ = order[next];
        const kit::Rect whole = row_rect(order[next].section, order[next].row);
        if (whole.y < layout_.view.y)
            scroll_by(whole.y - layout_.view.y - (next == 0 ? layout_.strip_height + layout_.strip_after : 0));
        else if (whole.y + whole.height > layout_.view.y + layout_.view.height)
            scroll_by(whole.y + whole.height - layout_.view.y - layout_.view.height);
        ++revision_;
    }

    /// Returns a category's plate.
    ///
    /// @param entry the category, by its place
    /// @return the plate
    [[nodiscard]] kit::Rect entry_rect(std::size_t entry) const noexcept {
        const auto& categories = facts_now_.categories;
        const auto count = static_cast<int32_t>(categories.size());
        const bool divided =
            std::any_of(categories.begin(), categories.end(), [](const auto& category) {
                return category.after_divider;
            });
        const int32_t room = layout_.list.height - (divided ? layout_.divider : 0);
        const int32_t pitch = std::min(
            layout_.entry_height + layout_.entry_gap, room / std::max(count, 1)
        );
        const int32_t height = std::max(pitch - layout_.entry_gap, 1);
        int32_t y = layout_.list.y + static_cast<int32_t>(entry) * pitch;
        if (entry < categories.size() && categories[entry].after_divider)
            y += layout_.divider;
        return {layout_.list.x, y, layout_.list.width, height};
    }

    /// Returns where the divider's line lies.
    ///
    /// @return its row; none without a divider
    [[nodiscard]] std::optional<int32_t> divider_row() const noexcept {
        const auto& categories = facts_now_.categories;
        for (std::size_t entry = 0; entry < categories.size(); ++entry)
            if (categories[entry].after_divider) {
                const kit::Rect first = entry_rect(entry);
                return first.y - (layout_.divider + layout_.entry_gap) / 2;
            }
        return std::nullopt;
    }

    /// Returns a section's strip where the view shows it.
    ///
    /// @param section the section
    /// @return the strip
    [[nodiscard]] kit::Rect strip_rect(std::size_t section) const noexcept {
        if (section >= strips_.size())
            return {};
        kit::Rect strip = strips_[section];
        strip.y += layout_.view.y - scroll_;
        return strip;
    }

    /// Returns a row where the view shows it.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @return the row
    [[nodiscard]] kit::Rect row_rect(std::size_t section, std::size_t row) const noexcept {
        if (section >= rows_.size() || row >= rows_[section].size())
            return {};
        kit::Rect whole = rows_[section][row];
        whole.y += layout_.view.y - scroll_;
        return whole;
    }

    /// Returns a row's control's place, at its right end.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @return the control's place
    [[nodiscard]] kit::Rect control_rect(std::size_t section, std::size_t row) const noexcept {
        const kit::Rect whole = row_rect(section, row);
        const SettingsMenuRow* shown = row_of({section, row});
        // A row of buttons alone takes more room, so that its captions fit.
        const bool wide = shown != nullptr && shown->control == SettingsMenuControl::buttons &&
                          shown->value.empty() && shown->buttons.size() > 1;
        const int32_t width = wide ? std::min(layout_.control * 3 / 2, whole.width * 3 / 5)
                                   : std::min(layout_.control, whole.width / 2);
        return {whole.x + whole.width - layout_.row_pad / 2 - width, whole.y, width, whole.height};
    }

    /// Returns one of a selector's arrows.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @param back the left arrow; otherwise the right
    /// @return the arrow
    [[nodiscard]] kit::Rect arrow_rect(std::size_t section, std::size_t row, bool back) const noexcept {
        const kit::Rect area = control_rect(section, row);
        const int32_t side = area.height;
        return {back ? area.x : area.x + area.width - side, area.y, side, side};
    }

    /// Returns where a selector's value shows, between its arrows.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @return the value's plate
    [[nodiscard]] kit::Rect value_rect(std::size_t section, std::size_t row) const noexcept {
        const kit::Rect back = arrow_rect(section, row, true);
        const kit::Rect on = arrow_rect(section, row, false);
        const int32_t left = back.x + back.width + layout_.option_gap;
        return {left, back.y, std::max(on.x - layout_.option_gap - left, 1), back.height};
    }

    /// Returns a slider's track, which a press reaches over its whole height.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @return the track's reach
    [[nodiscard]] kit::Rect track_rect(std::size_t section, std::size_t row) const noexcept {
        const kit::Rect area = control_rect(section, row);
        return {area.x, area.y, std::max(area.width - layout_.value - layout_.option_gap, 1), area.height};
    }

    /// Returns where a slider's knob's middle goes: from its first stop, as
    /// many points on as reach its last. The game's knob stays inside the
    /// frame of the bar's two ends (level_end_rim).
    ///
    /// @param track the track (track_rect)
    /// @return the first stop's column and the columns on to the last
    [[nodiscard]] std::pair<int32_t, int32_t> knob_travel(const kit::Rect& track) const noexcept {
        const double scale = piece_scale(track.height, slider_piece(SliderPiece::level_start).height);
        const int32_t rim = nearest(scale * level_end_rim);
        const int32_t knob = piece_place({0, 0}, slider_piece(SliderPiece::level_knob), scale).width;
        return {track.x + rim + knob / 2, std::max(track.width - 2 * rim - knob, 1)};
    }

    /// Returns the column of a slider's stop along its track.
    ///
    /// @param track the track (track_rect)
    /// @param stop the stop
    /// @param count how many stops it has
    /// @return the knob's middle
    [[nodiscard]] int32_t stop_column(const kit::Rect& track, int32_t stop, int32_t count) const noexcept {
        const auto [from, reach] = knob_travel(track);
        if (count <= 1)
            return from;
        return from + nearest(static_cast<double>(reach) * std::clamp(stop, 0, count - 1) / (count - 1));
    }

    /// Returns one of a row's buttons, the row's value on their left.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @param at the button, from 0 at the left
    /// @return the button
    [[nodiscard]] kit::Rect
    button_rect(std::size_t section, std::size_t row, std::size_t at) const noexcept {
        const SettingsMenuRow* shown = row_of({section, row});
        const kit::Rect area = control_rect(section, row);
        if (shown == nullptr || shown->buttons.empty())
            return {};
        const auto count = static_cast<int32_t>(shown->buttons.size());
        // A row with a value gives its button the right half; a row of
        // buttons alone shares the whole control among them.
        const int32_t left = shown->value.empty() ? area.x : area.x + area.width / 2;
        const int32_t room = area.x + area.width - left;
        const int32_t width = (room - (count - 1) * layout_.option_gap) / count;
        return {
            left + static_cast<int32_t>(at) * (width + layout_.option_gap),
            area.y,
            std::max(width, 1),
            area.height
        };
    }

    /// Returns one of the scroll bar's arrows.
    ///
    /// @param up the upper arrow; otherwise the lower
    /// @return the arrow
    [[nodiscard]] kit::Rect scroll_arrow_rect(bool up) const noexcept {
        const kit::Rect& bar = layout_.scroll_bar;
        const int32_t side = bar.width;
        return {bar.x, up ? bar.y : bar.y + bar.height - side, side, side};
    }

    /// Returns the scroll bar's well, between its arrows.
    ///
    /// @return the well
    [[nodiscard]] kit::Rect well_rect() const noexcept {
        const kit::Rect up = scroll_arrow_rect(true);
        const kit::Rect down = scroll_arrow_rect(false);
        const int32_t top = up.y + up.height + 2;
        return {layout_.scroll_bar.x, top, layout_.scroll_bar.width, std::max(down.y - 2 - top, 1)};
    }

    /// Returns the scroll bar's thumb: as tall as the view's share of the
    /// rows, never under the least, where the scroll puts it.
    ///
    /// @return the thumb
    [[nodiscard]] kit::Rect thumb_rect() const noexcept {
        const kit::Rect well = well_rect();
        const int32_t content = std::max(content_height_, 1);
        const int32_t height = std::clamp(
            static_cast<int32_t>(static_cast<int64_t>(well.height) * layout_.view.height / content),
            std::min(layout_.least_thumb, well.height),
            well.height
        );
        const int32_t limit = scroll_limit();
        const int32_t travel = well.height - height;
        const int32_t offset = limit > 0 ? static_cast<int32_t>(static_cast<int64_t>(travel) * scroll_ / limit) : 0;
        return {well.x, well.y + offset, well.width, height};
    }

    /// Returns the part under a point.
    ///
    /// @param at the point, in the screen's points
    /// @return the part; none when nothing a press reaches is there
    [[nodiscard]] Hit hit_at(kit::Point at) const noexcept {
        for (std::size_t entry = 0; entry < facts_now_.categories.size(); ++entry)
            if (kit::contains(entry_rect(entry), at))
                return {Part::category, entry, 0, 0};
        if (scroll_limit() > 0 && kit::contains(layout_.scroll_bar, at)) {
            if (kit::contains(scroll_arrow_rect(true), at))
                return {Part::scroll_up, 0, 0, 0};
            if (kit::contains(scroll_arrow_rect(false), at))
                return {Part::scroll_down, 0, 0, 0};
            if (kit::contains(thumb_rect(), at))
                return {Part::thumb, 0, 0, 0};
            if (kit::contains(well_rect(), at))
                return {Part::well, 0, 0, 0};
            return {};
        }
        if (kit::contains(layout_.view, at)) {
            for (std::size_t section = 0; section < rows_.size(); ++section)
                for (std::size_t row = 0; row < rows_[section].size(); ++row) {
                    if (!kit::contains(row_rect(section, row), at))
                        continue;
                    const SettingsMenuRow& shown = facts_now_.sections[section].rows[row];
                    const Hit plain{Part::row, section, row, 0};
                    if (!shown.enabled)
                        return plain;
                    switch (shown.control) {
                    case SettingsMenuControl::selector:
                        if (kit::contains(arrow_rect(section, row, true), at))
                            return {Part::back, section, row, 0};
                        if (kit::contains(arrow_rect(section, row, false), at))
                            return {Part::next, section, row, 0};
                        if (kit::contains(value_rect(section, row), at))
                            return {Part::value, section, row, 0};
                        break;
                    case SettingsMenuControl::slider:
                        if (kit::contains(track_rect(section, row), at))
                            return {Part::track, section, row, 0};
                        break;
                    case SettingsMenuControl::buttons:
                        for (std::size_t index = 0; index < shown.buttons.size(); ++index)
                            if (kit::contains(button_rect(section, row, index), at))
                                return {Part::button, section, row, index};
                        break;
                    case SettingsMenuControl::text:
                        break;
                    }
                    return plain;
                }
            return {};
        }
        for (std::size_t footer = 0; footer < footer_count; ++footer)
            if (kit::contains(layout_.buttons[footer], at))
                return {footer_part(static_cast<Footer>(footer)), 0, 0, 0};
        return {};
    }

    /// Tells whether a part takes a press.
    ///
    /// @param hit the part
    /// @return true when it does
    [[nodiscard]] bool enabled(Hit hit) const noexcept {
        switch (hit.part) {
        case Part::none:
            return false;
        case Part::category:
            return hit.section < facts_now_.categories.size();
        case Part::row:
            return row_of({hit.section, hit.row}) != nullptr;
        case Part::back:
        case Part::next:
        case Part::value:
        case Part::track:
        case Part::button: {
            const SettingsMenuRow* row = row_of({hit.section, hit.row});
            if (row == nullptr || !row->enabled)
                return false;
            if (hit.part == Part::button)
                return hit.index < row->buttons.size();
            if (hit.part == Part::track)
                return row->control == SettingsMenuControl::slider;
            return row->control == SettingsMenuControl::selector;
        }
        case Part::scroll_up:
        case Part::scroll_down:
        case Part::well:
        case Part::thumb:
            return scroll_limit() > 0;
        case Part::restore:
        case Part::undo:
        case Part::cancel:
        case Part::ok:
            return true;
        }
        return false;
    }

    /// Presses a part that takes a press, and reads the facts again.
    ///
    /// @param hit the part
    /// @param right made with the right button, which steps a selector back
    void press(Hit hit, bool right) {
        if (!enabled(hit))
            return;
        switch (hit.part) {
        case Part::category:
            if (right)
                return;
            if (actions_.choose)
                actions_.choose(hit.section);
            break;
        case Part::back:
        case Part::next:
        case Part::value:
            if (actions_.step)
                actions_.step(hit.section, hit.row, hit.part == Part::back ? right : !right);
            focused_.reset();
            break;
        case Part::button:
            if (right)
                return;
            if (actions_.press)
                actions_.press(hit.section, hit.row, hit.index);
            break;
        case Part::restore:
            if (actions_.restore_defaults)
                actions_.restore_defaults();
            break;
        case Part::undo:
            if (actions_.undo)
                actions_.undo();
            break;
        case Part::cancel:
            if (right)
                return;
            closing_ = true;
            if (actions_.cancel)
                actions_.cancel();
            break;
        case Part::ok:
            if (right)
                return;
            closing_ = true;
            if (actions_.accept)
                actions_.accept();
            break;
        default:
            return;
        }
        ++revision_;
        if (shows_())
            refresh();
    }

    /// Moves the dragged slider's knob to the stop nearest the pointer.
    ///
    /// @param at the pointer, in the screen's points
    void drag_track(kit::Point at) {
        const SettingsMenuRow* row = row_of(drag_row_);
        if (row == nullptr || !row->enabled || row->count <= 0 || !actions_.set_stop)
            return;
        const auto [from, reach] = knob_travel(track_rect(drag_row_.section, drag_row_.row));
        const double share = std::clamp(static_cast<double>(at.x - from) / reach, 0.0, 1.0);
        const int32_t stop = row->count > 1 ? nearest(share * (row->count - 1)) : 0;
        if (stop == row->index)
            return;
        actions_.set_stop(drag_row_.section, drag_row_.row, stop);
        ++revision_;
        refresh();
    }

    /// Scrolls the rows so that the dragged thumb follows the pointer.
    ///
    /// @param at the pointer, in the screen's points
    void drag_thumb(kit::Point at) {
        const kit::Rect well = well_rect();
        const kit::Rect thumb = thumb_rect();
        const int32_t travel = well.height - thumb.height;
        if (travel <= 0)
            return;
        const int32_t top = std::clamp(at.y - thumb_grab_, well.y, well.y + travel);
        scroll_to(static_cast<int32_t>(
            static_cast<int64_t>(scroll_limit()) * (top - well.y) / travel
        ));
    }

    /// Sets the part under the pointer, counting a change; a row it lies in
    /// becomes the one DETAILS tells of.
    ///
    /// @param hit the part
    void set_hovered(Hit hit) {
        if (!enabled(hit))
            hit = {};
        if (hit == hovered_)
            return;
        hovered_ = hit;
        switch (hit.part) {
        case Part::row:
        case Part::back:
        case Part::next:
        case Part::value:
        case Part::track:
        case Part::button:
            shown_row_ = RowAt{hit.section, hit.row};
            break;
        default:
            break;
        }
        ++revision_;
    }

    /// Sets the part a press holds, counting a change.
    ///
    /// @param hit the part
    void set_pressed(Hit hit) {
        if (hit == pressed_)
            return;
        pressed_ = hit;
        ++revision_;
    }

    /// Tells whether the pointer is over a row, or a key gave it the focus.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @return true when it is lit
    [[nodiscard]] bool row_lit(std::size_t section, std::size_t row) const noexcept {
        const bool pointed = hovered_.part != Part::none && hovered_.part != Part::category &&
                             hovered_.part < Part::scroll_up && hovered_.section == section &&
                             hovered_.row == row;
        return pointed || (focused_ && focused_->section == section && focused_->row == row);
    }

    /// Returns how wide a text in the game's font is drawn in a box.
    ///
    /// @param box the box
    /// @param text the text
    /// @return its width, in points
    [[nodiscard]] int32_t game_text_width(const kit::Rect& box, std::string_view text) const {
        return painter_.text_width(box, text);
    }

    /// Draws a text in the game's font (GameMenuPainter::draw_text).
    ///
    /// @param canvas where it draws
    /// @param box the box
    /// @param text the text
    /// @param align where the text sits along the box's width
    /// @param ink its tint
    /// @param edge how its pixels are drawn
    void draw_game_text(
        const kit::Canvas& canvas, const kit::Rect& box, std::string_view text, kit::Align align, Ink ink,
        TextEdge edge = TextEdge::sharp
    ) const {
        painter_.draw_text(canvas, box, text, align, ink, edge);
    }

    /// Returns one of the game's slider pieces.
    ///
    /// @param piece the piece
    /// @param held the held arrow, the piece after it
    /// @return the piece
    [[nodiscard]] const WindowIcon& slider_piece(SliderPiece piece, bool held = false) const {
        return painter_.slider_piece(piece, held);
    }

    /// Returns how many points a pixel of the game's slider pieces covers
    /// where a piece is to fit a room (GameMenuPainter::piece_scale).
    ///
    /// @param room the room, in points
    /// @param pixels the piece's pixels across it
    /// @return the points a pixel
    [[nodiscard]] double piece_scale(int32_t room, uint32_t pixels) const noexcept {
        return painter_.piece_scale(room, pixels);
    }

    /// Returns where one of the game's slider pieces lies, its top left at a
    /// point.
    ///
    /// @param at the point
    /// @param piece the piece
    /// @param scale the points a pixel
    /// @return its place
    [[nodiscard]] static kit::Rect piece_place(kit::Point at, const WindowIcon& piece, double scale) noexcept {
        return GameMenuPainter::piece_place(at, piece, scale);
    }

    /// Draws the game's level slider: its track, its knob at its stop, its
    /// value, darkened when it cannot be changed.
    ///
    /// @param canvas where it draws
    /// @param section the row's section
    /// @param row its place there
    void draw_game_slider(const kit::Canvas& canvas, std::size_t section, std::size_t row) const {
        const SettingsMenuRow& shown = facts_now_.sections[section].rows[row];
        const kit::Rect reach = track_rect(section, row);
        const WindowIcon& start = slider_piece(SliderPiece::level_start);
        const WindowIcon& track = slider_piece(SliderPiece::level_track);
        const WindowIcon& end = slider_piece(SliderPiece::level_end);
        const WindowIcon& knob = slider_piece(SliderPiece::level_knob);
        const double scale = piece_scale(reach.height, start.height);
        const int32_t height = std::max(nearest(scale * start.height), 1);
        const int32_t top = reach.y + (reach.height - height) / 2;
        const kit::Rect first = piece_place({reach.x, top}, start, scale);
        kit::Rect last = piece_place({0, top}, end, scale);
        last.x = reach.x + reach.width - last.width;
        const kit::Rect between{first.x + first.width, top, last.x - first.x - first.width, height};
        const int32_t step = std::max(nearest(scale * track.width), 1);
        const kit::Canvas along = kit::clipped(canvas, between);
        for (int32_t x = between.x; x < between.x + between.width; x += step)
            fitted_.draw(along, {x, top, step, height}, track);
        fitted_.draw(canvas, first, start);
        fitted_.draw(canvas, last, end);
        kit::Rect stop = piece_place({0, 0}, knob, scale);
        stop.x = stop_column(reach, shown.index, shown.count) - stop.width / 2;
        stop.y = top + (height - stop.height) / 2;
        fitted_.draw(canvas, stop, knob);
        if (!shown.enabled)
            kit::blend_rect(canvas, {reach.x, top, reach.width, height}, grayed_colour, grayed_opacity);
        const kit::Rect area = control_rect(section, row);
        draw_game_text(
            canvas,
            {area.x + area.width - layout_.value, area.y, layout_.value, area.height},
            shown.value,
            kit::Align::right,
            shown.enabled ? Ink::plain : Ink::faded
        );
    }

    /// Draws the game's scroll bar: its arrows, its track between them and
    /// its knob where the scroll puts it.
    ///
    /// @param canvas where it draws
    void draw_game_scroll_bar(const kit::Canvas& canvas) const {
        const auto held = [this](bool up) {
            const Hit hit{up ? Part::scroll_up : Part::scroll_down, 0, 0, 0};
            return hovered_ == hit && pressed_ == hit;
        };
        painter_.draw_scroll_bar(
            canvas, layout_.scroll_bar, scroll_arrow_rect(true), scroll_arrow_rect(false), well_rect(),
            thumb_rect(), held(true), held(false)
        );
    }

    /// Draws a panel's frame: the game's, its tube along the panel's edges
    /// and its corners and bolts past them, on the game's ground.
    ///
    /// @param canvas where it draws
    /// @param panel the panel
    void draw_frame(const kit::Canvas& canvas, const kit::Rect& panel) const {
        painter_.draw_frame(canvas, panel);
    }

    /// Draws a panel's label at its top left, in the game's font,
    /// unsmoothed.
    ///
    /// @param canvas where it draws
    /// @param panel the panel
    /// @param text the label
    void draw_label(const kit::Canvas& canvas, const kit::Rect& panel, std::string_view text) const {
        const int32_t left = panel.x + layout_.rim + layout_.label_left;
        const int32_t reach = panel.x + panel.width - layout_.rim - layout_.inset;
        draw_game_text(
            canvas,
            {left, panel.y + layout_.label_top, reach - left, layout_.label_height},
            text,
            kit::Align::left,
            Ink::plain
        );
    }

    /// Draws the CATEGORIES panel: a plate for each category with its
    /// picture and its name, the chosen one marked, and the divider.
    ///
    /// @param canvas where it draws
    void draw_categories(const kit::Canvas& canvas) const {
        draw_frame(canvas, layout_.categories);
        draw_label(canvas, layout_.categories, worded("CATEGORIES"));
        // The divider is the frame's tube, repeated across from the frame's
        // left tube to its right one.
        if (const auto row = divider_row())
            painter_.draw_rule(canvas, layout_.categories, *row);
        const kit::Canvas inside = kit::clipped(canvas, layout_.list);
        for (std::size_t entry = 0; entry < facts_now_.categories.size(); ++entry) {
            const SettingsMenuCategory& category = facts_now_.categories[entry];
            const kit::Rect plate = entry_rect(entry);
            const bool chosen = entry == facts_now_.chosen;
            const Hit hit{Part::category, entry, 0, 0};
            const bool lit = hovered_ == hit;
            const bool held = lit && pressed_ == hit;
            // The game's button stands held for the chosen category; only
            // the name under the pointer turns amber. The picture stays dark
            // on the button's light stone.
            painter_.draw_plain_button(inside, plate, chosen || held ? 1 : 0);
            const int32_t icon = std::min(layout_.icon, plate.height - 6);
            const kit::Rect picture{
                plate.x + layout_.icon_gap, plate.y + (plate.height - icon) / 2, icon, icon
            };
            draw_icon(inside, picture, category.icon, game_icon_colour);
            const int32_t text_left = picture.x + picture.width + layout_.icon_gap;
            draw_game_text(
                inside,
                {text_left, plate.y, plate.x + plate.width - layout_.icon_gap - text_left, plate.height},
                category.name,
                kit::Align::left,
                lit ? Ink::lit : Ink::plain,
                TextEdge::smoothed
            );
        }
    }

    /// Draws the chosen category's panel: its label, each section's strip
    /// and rows in the view, and the scroll bar when the rows scroll.
    ///
    /// @param canvas where it draws
    void draw_center(const kit::Canvas& canvas) const {
        draw_frame(canvas, layout_.center);
        draw_label(canvas, layout_.center, facts_now_.heading);
        const kit::Canvas view = kit::clipped(canvas, layout_.view);
        if (facts_now_.sections.empty())
            kit::box_text(
                canvas, kit::FontRole::small, worded("Nothing to set here."), layout_.view,
                kit::Align::centre, caption_faded_colour
            );
        for (std::size_t section = 0; section < facts_now_.sections.size(); ++section) {
            draw_strip(view, section);
            for (std::size_t row = 0; row < facts_now_.sections[section].rows.size(); ++row) {
                const kit::Rect whole = row_rect(section, row);
                if (whole.y + whole.height < layout_.view.y ||
                    whole.y > layout_.view.y + layout_.view.height)
                    continue;
                draw_row(view, section, row);
            }
        }
        if (scroll_limit() > 0)
            draw_game_scroll_bar(canvas);
    }

    /// Draws a section's strip: the game's sunk bar with its title, and its
    /// badge in amber at its right.
    ///
    /// @param canvas where it draws, clipped to the view
    /// @param section the section
    void draw_strip(const kit::Canvas& canvas, std::size_t section) const {
        const SettingsMenuSection& shown = facts_now_.sections[section];
        const kit::Rect strip = strip_rect(section);
        painter_.draw_well(canvas, strip);
        int32_t right = strip.x + strip.width - layout_.row_pad;
        if (!shown.badge.empty()) {
            const kit::Rect badge{strip.x, strip.y, right - strip.x, strip.height};
            draw_game_text(canvas, badge, shown.badge, kit::Align::right, Ink::lit);
            right -= game_text_width(badge, shown.badge) + layout_.option_gap;
        }
        const int32_t left = strip.x + layout_.row_pad;
        draw_game_text(
            canvas, {left, strip.y, right - left, strip.height}, shown.title, kit::Align::left, Ink::plain
        );
    }

    /// Draws a row: its label, its control and, under the pointer or the
    /// focus, an amber light over it.
    ///
    /// @param canvas where it draws, clipped to the view
    /// @param section the row's section
    /// @param row its place there
    void draw_row(const kit::Canvas& canvas, std::size_t section, std::size_t row) const {
        const SettingsMenuRow& shown = facts_now_.sections[section].rows[row];
        const kit::Rect whole = row_rect(section, row);
        const bool lit = row_lit(section, row);
        if (lit)
            kit::blend_rect(canvas, whole, chosen_colour, row_light_opacity);
        kit::fill_rect(canvas, {whole.x, whole.y + whole.height + layout_.row_gap / 2, whole.width, 1}, row_rule_colour);
        const kit::Rect control = control_rect(section, row);
        const int32_t label_left = whole.x + layout_.row_pad;
        int32_t label_right = control.x - layout_.option_gap;
        if (!shown.lock.empty()) {
            const int32_t side = std::max(kit::smooth_text_points(kit::FontRole::small), 6);
            draw_padlock(
                canvas,
                {label_right - side, whole.y + (whole.height - side) / 2, side, side},
                lock_colour
            );
            label_right -= side + layout_.option_gap;
        }
        draw_game_text(
            canvas,
            {label_left, whole.y, label_right - label_left, whole.height},
            shown.label,
            kit::Align::left,
            !shown.enabled ? Ink::faded : lit ? Ink::lit : Ink::plain
        );
        switch (shown.control) {
        case SettingsMenuControl::selector:
            draw_selector(canvas, section, row);
            break;
        case SettingsMenuControl::slider:
            draw_game_slider(canvas, section, row);
            break;
        case SettingsMenuControl::buttons:
            draw_buttons(canvas, section, row);
            break;
        case SettingsMenuControl::text:
            draw_game_text(canvas, control, shown.value, kit::Align::right, Ink::plain);
            break;
        }
    }

    /// Draws the game's arrow, held while it is pressed, darkened when it
    /// cannot be pressed.
    ///
    /// @param canvas where it draws
    /// @param rect the button
    /// @param hit its part
    /// @param left the arrow points left; otherwise right
    void draw_arrow_button(
        const kit::Canvas& canvas, const kit::Rect& rect, Hit hit, bool left
    ) const {
        const bool usable = enabled(hit);
        const bool lit = usable && hovered_ == hit;
        const bool held = lit && pressed_ == hit;
        const WindowIcon& arrow = slider_piece(left ? SliderPiece::back : SliderPiece::next, held);
        const double scale = piece_scale(rect.height, arrow.height);
        kit::Rect place = piece_place({0, 0}, arrow, scale);
        place.x = rect.x + (rect.width - place.width) / 2;
        place.y = rect.y + (rect.height - place.height) / 2;
        fitted_.draw(canvas, place, arrow);
        if (!usable)
            kit::blend_rect(canvas, place, grayed_colour, grayed_opacity);
    }

    /// Draws a selector: its value in the game's sunk bar between its two
    /// arrows.
    ///
    /// @param canvas where it draws
    /// @param section the row's section
    /// @param row its place there
    void draw_selector(const kit::Canvas& canvas, std::size_t section, std::size_t row) const {
        const SettingsMenuRow& shown = facts_now_.sections[section].rows[row];
        draw_arrow_button(canvas, arrow_rect(section, row, true), {Part::back, section, row, 0}, true);
        draw_arrow_button(canvas, arrow_rect(section, row, false), {Part::next, section, row, 0}, false);
        const kit::Rect value = value_rect(section, row);
        const Hit hit{Part::value, section, row, 0};
        const bool lit = enabled(hit) && hovered_ == hit;
        painter_.draw_well(canvas, value);
        draw_game_text(
            canvas,
            {value.x + layout_.option_gap, value.y, value.width - 2 * layout_.option_gap, value.height},
            shown.value,
            kit::Align::centre,
            !shown.enabled ? Ink::faded : lit ? Ink::lit : Ink::plain
        );
    }

    /// Draws a row's buttons, and its value on their left.
    ///
    /// @param canvas where it draws
    /// @param section the row's section
    /// @param row its place there
    void draw_buttons(const kit::Canvas& canvas, std::size_t section, std::size_t row) const {
        const SettingsMenuRow& shown = facts_now_.sections[section].rows[row];
        if (!shown.value.empty()) {
            const kit::Rect area = control_rect(section, row);
            draw_game_text(
                canvas,
                {area.x, area.y, area.width / 2 - layout_.option_gap, area.height},
                shown.value,
                kit::Align::right,
                Ink::plain
            );
        }
        for (std::size_t at = 0; at < shown.buttons.size(); ++at) {
            const Hit hit{Part::button, section, row, at};
            const kit::Rect rect = button_rect(section, row, at);
            const bool usable = enabled(hit);
            const bool lit = usable && hovered_ == hit;
            const bool held = lit && pressed_ == hit;
            painter_.draw_plain_button(canvas, rect, !usable ? 2 : held ? 1 : 0);
            draw_game_text(
                canvas,
                {rect.x + layout_.option_gap, rect.y, rect.width - 2 * layout_.option_gap, rect.height},
                shown.buttons[at],
                kit::Align::centre,
                !usable ? Ink::faded : lit ? Ink::lit : Ink::plain,
                TextEdge::smoothed
            );
        }
    }

    /// Returns the row DETAILS tells of: the one under the pointer, or the
    /// focused one, or the last one either was on.
    ///
    /// @return the row; null for none
    [[nodiscard]] const SettingsMenuRow* told_row() const noexcept {
        if (focused_)
            if (const SettingsMenuRow* row = row_of(*focused_))
                return row;
        return shown_row_ ? row_of(*shown_row_) : nullptr;
    }

    /// Returns how many points each pixel of the game's font covers in a
    /// row's label: the size DETAILS writes in too.
    ///
    /// @return the points a pixel
    [[nodiscard]] int32_t game_text_square() const { return painter_.text_square(layout_.row_height); }

    /// Draws the DETAILS panel: the row's label, what it does, why it is
    /// locked, its default and when a change takes effect; without a row,
    /// the category's name and how to read about a row. It writes in the
    /// game's font, the default in the game's sunk bar.
    ///
    /// @param canvas where it draws
    void draw_details(const kit::Canvas& canvas) const {
        draw_frame(canvas, layout_.details);
        draw_label(canvas, layout_.details, worded("DETAILS"));
        const kit::Rect& about = layout_.about;
        const int32_t square = game_text_square();
        const kit::Measure game = painter_.measure(square);
        // Each line's box makes the game's font as big as in a row's label.
        const int32_t line_height = painter_.line_height(square);
        const SettingsMenuRow* row = told_row();
        std::string title;
        std::string paragraph;
        if (row != nullptr) {
            title = row->label;
            paragraph = row->detail;
        } else {
            if (facts_now_.chosen < facts_now_.categories.size())
                title = facts_now_.categories[facts_now_.chosen].name;
            paragraph = worded("Point at a setting to read what it does.");
        }
        int32_t y = about.y;
        kit::WrapRules title_rules;
        title_rules.most_lines = most_title_lines;
        const auto title_lines = kit::wrap(title, about.width, game, title_rules);
        const int32_t title_height = std::max(
            layout_.title_line, static_cast<int32_t>(title_lines.size()) * line_height
        );
        int32_t line_y = y + (title_height - static_cast<int32_t>(title_lines.size()) * line_height) / 2;
        for (const std::string& line : title_lines) {
            draw_game_text(canvas, {about.x, line_y, about.width, line_height}, line, kit::Align::left, Ink::plain);
            line_y += line_height;
        }
        y += title_height + layout_.paragraph_gap;
        const int32_t bottom =
            about.y + about.height - layout_.default_height - layout_.note_height - layout_.paragraph_gap;
        // A new line in a row's description starts a line of its own: a
        // language's facts stand one to a line.
        kit::WrapRules paragraph_rules;
        paragraph_rules.newlines = true;
        const auto lines_of = [&](std::string_view text, Ink ink) {
            for (const std::string& line : kit::wrap(text, about.width, game, paragraph_rules)) {
                if (y + line_height > bottom)
                    return;
                draw_game_text(canvas, {about.x, y, about.width, line_height}, line, kit::Align::left, ink);
                y += line_height;
            }
        };
        lines_of(paragraph, Ink::plain);
        if (row != nullptr && !row->lock.empty()) {
            y += line_height / 2;
            lines_of(filled(worded("Locked: {reason}"), "{reason}", row->lock), Ink::lit);
        }
        if (!facts_now_.notice.empty()) {
            y += line_height / 2;
            lines_of(facts_now_.notice, Ink::lit);
        }
        if (row == nullptr)
            return;
        const kit::Rect box{
            about.x, about.y + about.height - layout_.default_height - layout_.note_height,
            about.width, layout_.default_height
        };
        if (!row->default_value.empty()) {
            const std::string text = filled(worded("Default: {value}"), "{value}", row->default_value);
            const kit::Rect inner{box.x + layout_.row_pad, box.y, box.width - 2 * layout_.row_pad, box.height};
            painter_.draw_well(canvas, box);
            draw_game_text(canvas, inner, text, kit::Align::left, Ink::plain);
        }
        if (!row->takes_effect.empty()) {
            const kit::Rect note{about.x, box.y + box.height, about.width, layout_.note_height};
            draw_game_text(canvas, note, row->takes_effect, kit::Align::left, Ink::faded);
        }
    }

    /// Draws one of the four buttons as the Options screen's own, up or
    /// held. They carry their captions, and show no hover: the pointer over
    /// a button leaves it as it is.
    ///
    /// @param canvas where it draws
    /// @param footer the button
    void draw_footer(const kit::Canvas& canvas, Footer footer) const {
        const Hit hit{footer_part(footer), 0, 0, 0};
        const bool held = hovered_ == hit && pressed_ == hit;
        const auto at = static_cast<std::size_t>(footer);
        fitted_.draw(canvas, layout_.buttons[at], game_buttons_[at][held ? 1 : 0]);
    }

    std::function<bool()> shows_{};             ///< whether the screen shows
    std::function<SettingsMenuFacts()> facts_{}; ///< reads what the settings show
    SettingsMenuActions actions_{};              ///< what its presses do
    SettingsMenuFacts facts_now_{};              ///< what they show, as last read
    kit::Point size_{};                          ///< the screen's size, in points
    SettingsLayout layout_{};                    ///< where its fixed parts lie
    bool laid_out_{};                            ///< layout_ is for size_
    std::vector<kit::Rect> strips_{};            ///< each section's strip, before the scroll
    std::vector<std::vector<kit::Rect>> rows_{}; ///< each section's rows, before the scroll
    int32_t content_height_{};                   ///< the strips' and rows' height together
    int32_t scroll_{};                           ///< how far the rows are scrolled, in points
    Hit hovered_{};                              ///< the part under the pointer
    Hit pressed_{};                              ///< the part a press holds
    uint8_t pressed_button_{};                   ///< the mouse button the press is of
    Part dragging_{Part::none};                  ///< the track or the thumb a press drags
    RowAt drag_row_{};                           ///< the slider dragged
    int32_t thumb_grab_{};                       ///< where the press took the thumb, from its top
    std::optional<RowAt> focused_{};             ///< the row the keys moved the focus to
    std::optional<RowAt> shown_row_{};           ///< the row DETAILS tells of
    bool closing_{};                             ///< Cancel or OK was pressed
    /// The Options screen's buttons the four buttons are drawn as.
    std::array<std::array<WindowIcon, 2>, 4> game_buttons_{};
    GameMenuPainter painter_;                    ///< draws with the game's art
    FittedArt fitted_{};                         ///< the arrows, the slider pieces and the buttons, filtered
    uint64_t revision_{1};                       ///< counts the changes to what it draws
};

} // namespace

std::unique_ptr<LayerScreen> make_settings_menu(
    std::function<bool()> shows,
    std::function<SettingsMenuFacts()> facts,
    SettingsMenuActions actions,
    SettingsMenuGameArt game_art
) {
    return std::make_unique<SettingsMenuScreen>(
        std::move(shows), std::move(facts), std::move(actions), std::move(game_art)
    );
}

} // namespace oa::app
