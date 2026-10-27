// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every colour token equals the value Kit 2's design gives it, or the value
// it keeps, written here as hex; the tinted fills are their colour laid over
// the raised surface. Each class's metrics hold Kit 2's scales (type,
// spacing, radii and control sizes) and what follows from them, the sizes
// the screens are laid out with are 0.7.3's at Compact with the design's
// table of dialog sizes, and each class's dialog and notices fit the least
// window of their class.

#include "oa/test/check.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <array>
#include <cstddef>
#include <stdint.h>

namespace {

namespace kit = oa::ui::kit;

/// Checks one token against a colour written out in the test.
///
/// @param token the token
/// @param red red, 0 to 255
/// @param green green, 0 to 255
/// @param blue blue, 0 to 255
/// @param opacity opacity, 0 to 255
void equals(kit::Colour token, uint8_t red, uint8_t green, uint8_t blue, uint8_t opacity = 255) {
    OA_CHECK(token == (kit::Colour{red, green, blue, opacity}));
}

/// Checks every token against Kit 2's value, written out in the test.
void every_token_keeps_its_value() {
    // The surface ramp, s0 to s4, and the rules and edges.
    equals(kit::colour::band, 0x12, 0x14, 0x10);
    equals(kit::colour::list, 0x17, 0x1a, 0x15);
    equals(kit::colour::panel, 0x1d, 0x21, 0x19);
    equals(kit::colour::hover, 0x25, 0x2a, 0x21);
    equals(kit::colour::list_selected, 0x2c, 0x32, 0x27);
    equals(kit::colour::rule, 0x2b, 0x30, 0x27);
    equals(kit::colour::strong_edge, 0x4f, 0x57, 0x47);
    equals(kit::colour::edge_light, 0x4f, 0x57, 0x47);
    equals(kit::colour::edge_dark, 0x08, 0x09, 0x07);
    equals(kit::colour::control_border, 0x3a, 0x40, 0x34);
    equals(kit::colour::control_hover, 0x5b, 0x63, 0x52);
    equals(kit::colour::well, 0x12, 0x14, 0x10);
    equals(kit::colour::off_selected, 0x2c, 0x32, 0x27);
    // The text tones.
    equals(kit::colour::text, 0xe7, 0xe8, 0xdf);
    equals(kit::colour::text_secondary, 0xb9, 0xbf, 0xae);
    equals(kit::colour::text_tertiary, 0x95, 0x9c, 0x8b);
    equals(kit::colour::hint, 0x95, 0x9c, 0x8b);
    equals(kit::colour::quiet, 0x95, 0x9c, 0x8b);
    equals(kit::colour::list_text, 0xb9, 0xbf, 0xae);
    equals(kit::colour::button_text, 0xb9, 0xbf, 0xae);
    equals(kit::colour::switch_idle, 0x7d, 0x84, 0x74);
    // The accent, the focus ring and the filled button.
    equals(kit::colour::accent, 0x9c, 0xcc, 0x3c);
    equals(kit::colour::accent_light, 0xb6, 0xe0, 0x5a);
    equals(kit::colour::accent_held, 0x8a, 0xb8, 0x30);
    equals(kit::colour::on_accent, 0x10, 0x12, 0x0d);
    equals(kit::colour::focus, 0xb6, 0xe0, 0x5a);
    equals(kit::colour::filled, 0x3e, 0x52, 0x18);
    equals(kit::colour::filled_light, 0x49, 0x60, 0x1c);
    equals(kit::colour::filled_held, 0x35, 0x45, 0x14);
    // Caution, the lifted red, information and the brand gold.
    equals(kit::colour::lock, 0xe0, 0xb0, 0x4f);
    equals(kit::colour::danger, 0xe8, 0x80, 0x6f);
    equals(kit::colour::information, 0x7d, 0xb3, 0xde);
    equals(kit::colour::online, 0x7d, 0xb3, 0xde);
    equals(kit::colour::brand_light, 0xf3, 0xcf, 0x76);
    equals(kit::colour::brand, 0xd6, 0xa7, 0x47);
    equals(kit::colour::brand_shade, 0x9c, 0x6f, 0x22);
    equals(kit::colour::backdrop, 5, 6, 4);
    // The tinted fills: 41/256 of the colour over the raised surface, and
    // the field's soft ring a quarter of the accent.
    OA_CHECK(kit::badge_tint == 41);
    equals(kit::colour::ok_tint, 0x31, 0x3c, 0x1f);
    equals(kit::colour::caution_tint, 0x3c, 0x38, 0x22);
    equals(kit::colour::danger_tint, 0x3e, 0x30, 0x27);
    equals(kit::colour::information_tint, 0x2c, 0x38, 0x39);
    equals(kit::colour::brand_tint, 0x3b, 0x36, 0x20);
    OA_CHECK(kit::field_ring_tint == 64);
    equals(kit::colour::field_ring, 0x3d, 0x4c, 0x22);
    equals(kit::mixed({200, 100, 0}, {0, 100, 200}, 128), 100, 100, 100);
    equals(kit::mixed({255, 255, 255}, {0, 0, 0}, 0), 0, 0, 0);
    // The category tints stay as they were.
    equals(kit::colour::category_orders, 0x9c, 0xcc, 0x3c);
    equals(kit::colour::category_selection, 0x6e, 0xa8, 0xd6);
    equals(kit::colour::category_pointer, 0x6e, 0xa8, 0xd6);
    equals(kit::colour::category_groups, 0xc4, 0x9b, 0xe0);
    equals(kit::colour::category_camera, 0x5f, 0xc0, 0xa8);
    equals(kit::colour::category_building, 0xe0, 0xb0, 0x4f);
    equals(kit::colour::category_game, 0xb9, 0xbf, 0xae);
    equals(kit::colour::category_chat, 0xe5, 0x8f, 0xb0);
    equals(kit::colour::category_window, 0xb9, 0xbf, 0xae);
    equals(kit::colour::category_hacks, 0xe0, 0x8a, 0x4f);
    equals(kit::colour::category_mod, 0x8f, 0xd0, 0xe8);
    equals(kit::colour::category_developer, 0x8a, 0x91, 0x80);

    // The Game files screen and the folder chooser take the dialog's tokens.
    OA_CHECK(kit::screen_colour::background == kit::colour::panel);
    OA_CHECK(kit::screen_colour::panel == kit::colour::hover);
    OA_CHECK(kit::screen_colour::line == kit::colour::control_border);
    OA_CHECK(kit::screen_colour::text == kit::colour::text);
    OA_CHECK(kit::screen_colour::dim == kit::colour::text_tertiary);
    // Each of the screens' tokens has a value of its own, so a layout that
    // names its colours by token keeps its names.
    const std::array<kit::Colour, 10> screen{
        kit::screen_colour::background,
        kit::screen_colour::panel,
        kit::screen_colour::line,
        kit::screen_colour::text,
        kit::screen_colour::dim,
        kit::screen_colour::green,
        kit::screen_colour::amber,
        kit::screen_colour::red,
        kit::screen_colour::ink,
        kit::screen_colour::button_text,
    };
    for (std::size_t one = 0; one < screen.size(); ++one)
        for (std::size_t other = one + 1; other < screen.size(); ++other)
            OA_CHECK(screen[one] != screen[other]);
    OA_CHECK(kit::screen_colour::green == kit::colour::accent);
    OA_CHECK(kit::screen_colour::amber == kit::colour::lock);
    OA_CHECK(kit::screen_colour::red == kit::colour::danger);
    OA_CHECK(kit::screen_colour::ink == kit::colour::on_accent);
    OA_CHECK(kit::screen_colour::button_text == kit::colour::button_text);

    equals(kit::hud_colour::panel, 0x1b, 0x1e, 0x19, 217);
    equals(kit::hud_colour::sheet, 0x14, 0x16, 0x12, 248);
    equals(kit::hud_colour::cell, 0x24, 0x28, 0x20, 230);
    equals(kit::hud_colour::pressed, 0x3b, 0x42, 0x35, 240);
    equals(kit::hud_colour::edge, 0xff, 0xff, 0xff, 34);
    equals(kit::hud_colour::lit, 0x9c, 0xcc, 0x3c);
    equals(kit::hud_colour::lit_pressed, 0xb8, 0xde, 0x66);
    equals(kit::hud_colour::ink, 0x1b, 0x1e, 0x19);
    equals(kit::hud_colour::label, 0xf2, 0xf4, 0xee);
    equals(kit::hud_colour::quiet, 0xb4, 0xb9, 0xae);
    equals(kit::hud_colour::danger, 0xc0, 0x39, 0x2b);
    equals(kit::hud_colour::danger_panel, 0x2a, 0x15, 0x12, 235);
    equals(kit::hud_colour::danger_label, 0xf0, 0x8c, 0x80);
    equals(kit::hud_colour::wedge_gap, 0x08, 0x09, 0x07);
    OA_CHECK(kit::hud_colour::greyed_opacity == 0.4F);

    OA_CHECK(kit::menu_backdrop_opacity == 159);
    OA_CHECK(kit::spotlight_opacity == 189);
    OA_CHECK(kit::ingame_backdrop_opacity == 128);
    OA_CHECK(kit::locked_fade == 115);
    OA_CHECK(kit::selected_tint == 15);

    constexpr kit::Colour with_opacity{1, 2, 3, 4};
    OA_CHECK(kit::rgb(with_opacity) == (std::array<uint8_t, 3>{1, 2, 3}));
}

/// Checks the sizes every class shares: Kit 2's focus ring, window bands,
/// switch and badge, and the sizes the screens are laid out with, 0.7.3's.
void shared_sizes() {
    for (const kit::Metrics* metrics :
         {&kit::compact_metrics, &kit::regular_metrics, &kit::large_metrics}) {
        OA_CHECK(metrics->focus_ring == 1);
        OA_CHECK(metrics->window_header_height == 36);
        OA_CHECK(metrics->window_footer_height == 48);
        OA_CHECK(metrics->switch_track_width == 42);
        OA_CHECK(metrics->switch_height == 24);
        OA_CHECK(metrics->badge_height == 22);
        // Kit 2's header holds a Compact control, and its footer every
        // class's controls, each with at least four points above and below.
        OA_CHECK(metrics->window_header_height - kit::compact_metrics.control_height >= 8);
        OA_CHECK(metrics->window_footer_height - kit::large_metrics.control_height >= 8);
        // The switch's track fits the area the screens give a switch.
        OA_CHECK(metrics->switch_track_width <= metrics->switch_width);

        OA_CHECK(metrics->edge == 1);
        OA_CHECK(metrics->header_height == 26);
        OA_CHECK(metrics->footer_height == 32);
        OA_CHECK(metrics->mark_top == 4);
        OA_CHECK(metrics->mark_side == 20);
        OA_CHECK(metrics->mark_square == 13);
        OA_CHECK(metrics->header_gap == 6);
        OA_CHECK(metrics->heading_tracking == 1);
        OA_CHECK(metrics->button_height == 17);
        OA_CHECK(metrics->button_width == 52);
        OA_CHECK(metrics->button_gap == 5);
        OA_CHECK(metrics->focus_inset == 2);
        OA_CHECK(metrics->small_line == 12);
        OA_CHECK(metrics->regular_line == 16);
        OA_CHECK(metrics->switch_width == 52);
        OA_CHECK(metrics->knob_width == 7);
        OA_CHECK(metrics->knob_height == 12);
        OA_CHECK(metrics->track_height == 4);
        OA_CHECK(metrics->track_offset == 4);
        OA_CHECK(metrics->stop_height == 2);
        OA_CHECK(metrics->stop_offset == 12);
        OA_CHECK(metrics->least_stop_spacing == 4);
        OA_CHECK(metrics->choice_text_inset == 6);
        OA_CHECK(metrics->choice_arrow_room == 16);
        OA_CHECK(metrics->choice_arrow_width == 7);
        OA_CHECK(metrics->choice_arrow_height == 4);
        OA_CHECK(metrics->choice_item_height == 16);
        OA_CHECK(metrics->most_shown_choices == 8);
        OA_CHECK(metrics->choice_item_text_inset == 12);
        OA_CHECK(metrics->padlock_width == 5);
        OA_CHECK(metrics->padlock_height == 7);
        OA_CHECK(metrics->padlock_gap == 3);
        OA_CHECK(metrics->scroll_thumb_width == 5);
        OA_CHECK(metrics->least_thumb_height == 16);
        OA_CHECK(metrics->button_icon_inset == 3);
        OA_CHECK(metrics->button_icon_press == 1);
        OA_CHECK(metrics->button_square_numerator == 20);
        OA_CHECK(metrics->button_square_denominator == 32);
        OA_CHECK(metrics->large_mark_margin == 2);
        OA_CHECK(metrics->list_row_height == 36);
        OA_CHECK(metrics->list_row_badge == 28);
        OA_CHECK(metrics->short_list_row_height == 28);
        OA_CHECK(metrics->list_row_inset == 4);
        OA_CHECK(metrics->list_row_title_top == 1);
        OA_CHECK(metrics->list_row_line_top == 15);
        OA_CHECK(metrics->list_row_gap == 6);
        OA_CHECK(metrics->hairline == 1);
        OA_CHECK(metrics->badge_dash == 2);
        OA_CHECK(metrics->badge_dash_gap == 1);
        OA_CHECK(metrics->chip_height == 12);
        OA_CHECK(metrics->chip_padding == 4);
        OA_CHECK(metrics->chip_gap == 4);
        OA_CHECK(metrics->state_icon_side == 14);
        OA_CHECK(metrics->tab_height == 18);
        OA_CHECK(metrics->tab_padding == 10);
        OA_CHECK(metrics->tab_tracking == 1);
        OA_CHECK(metrics->tab_rule == 2);
        OA_CHECK(metrics->tab_count_height == 12);
        OA_CHECK(metrics->tab_count_padding == 3);
        OA_CHECK(metrics->tab_count_gap == 4);
        OA_CHECK(metrics->field_height == 16);
        OA_CHECK(metrics->field_inset == 4);
        OA_CHECK(metrics->magnifier_side == 7);
        OA_CHECK(metrics->caret_width == 1);
        OA_CHECK(metrics->card_least_width == 120);
        OA_CHECK(metrics->card_gap == 6);
        OA_CHECK(metrics->card_inset == 4);
        OA_CHECK(metrics->hover_card_padding == 6);
        OA_CHECK(metrics->hover_card_arrow == 5);
        OA_CHECK(metrics->text_top == 38);
        OA_CHECK(metrics->paragraph_gap == 6);
        OA_CHECK(metrics->text_bottom_gap == 10);
        OA_CHECK(metrics->open_width == 96);
        OA_CHECK(metrics->prompt_button_padding == 16);
        OA_CHECK(metrics->progress_bar_height == 8);
        OA_CHECK(metrics->row_padding == 8);
        OA_CHECK(metrics->label_gap == 8);
        OA_CHECK(metrics->hint_gap == 2);
        OA_CHECK(metrics->tall_hint_line_gap == 3);
        OA_CHECK(metrics->most_hint_lines == 2);
        OA_CHECK(metrics->most_notice_lines == 4);
        OA_CHECK(metrics->slider_gap == 4);
        OA_CHECK(metrics->slider_line_height == 14);
        OA_CHECK(metrics->slider_value_width == 110);
        OA_CHECK(metrics->slider_value_gap == 10);
        OA_CHECK(metrics->lock_width == 148);
        OA_CHECK(metrics->choice_width == 200);
        OA_CHECK(metrics->wide_choice_width == 248);
        OA_CHECK(metrics->button_row_width == 76);
        OA_CHECK(metrics->folder_button_widths == (std::array<int32_t, 3>{46, 82, 44}));
        OA_CHECK(metrics->folder_button_gap == 4);
        OA_CHECK(metrics->least_notice_height == 150);
    }
}

/// One class's steps on the scales, as the design gives them, in points.
struct ClassSteps {
    const kit::Metrics* metrics{}; ///< the class's metrics
    int32_t title{};               ///< a title's size
    int32_t label{};               ///< a label's size
    int32_t body{};                ///< body text's size
    int32_t small{};               ///< small text's size
    int32_t space{};               ///< the spacing base
    int32_t keycap_radius{};       ///< a keycap's radius
    int32_t control_radius{};      ///< a control's radius
    int32_t card_radius{};         ///< a card's radius
    int32_t window_radius{};       ///< a window's radius
    int32_t dialog_radius{};       ///< a dialog's radius
    int32_t control{};             ///< a button's height
    int32_t row{};                 ///< a list row's height
    int32_t field{};               ///< a text field's height
    int32_t chip{};                ///< a filter chip's height
    int32_t tabs{};                ///< a strip of tabs' height
};

/// Kit 2's scales: Compact, Regular and Large.
const ClassSteps kClassSteps[] = {
    {&kit::compact_metrics, 17, 14, 13, 12, 4, 5, 6, 8, 10, 12, 28, 36, 30, 24, 32},
    {&kit::regular_metrics, 20, 15, 14, 12, 6, 6, 7, 9, 11, 13, 34, 40, 36, 26, 38},
    {&kit::large_metrics, 24, 16, 15, 13, 8, 6, 8, 10, 12, 14, 38, 44, 40, 28, 44},
};

/// Checks each class's steps on the type, spacing and shape scales and its
/// control sizes, and what follows from the spacing base.
void each_class_has_its_steps() {
    for (const ClassSteps& steps : kClassSteps) {
        const kit::Metrics& metrics = *steps.metrics;
        OA_CHECK(metrics.title_size == steps.title);
        OA_CHECK(metrics.label_size == steps.label);
        OA_CHECK(metrics.body_size == steps.body);
        OA_CHECK(metrics.small_size == steps.small);
        OA_CHECK(kit::text_size(metrics, kit::TextLevel::title) == steps.title);
        OA_CHECK(kit::text_size(metrics, kit::TextLevel::label) == steps.label);
        OA_CHECK(kit::text_size(metrics, kit::TextLevel::body) == steps.body);
        OA_CHECK(kit::text_size(metrics, kit::TextLevel::small) == steps.small);
        OA_CHECK(metrics.space == steps.space);
        OA_CHECK(metrics.window_padding == 3 * steps.space);
        OA_CHECK(metrics.row_gap == 3 * steps.space / 2);
        OA_CHECK(metrics.section_gap == 3 * steps.space);
        OA_CHECK(metrics.keycap_radius == steps.keycap_radius);
        OA_CHECK(metrics.control_radius == steps.control_radius);
        OA_CHECK(metrics.card_radius == steps.card_radius);
        OA_CHECK(metrics.window_radius == steps.window_radius);
        OA_CHECK(metrics.dialog_radius == steps.dialog_radius);
        OA_CHECK(metrics.control_height == steps.control);
        OA_CHECK(metrics.row_height == steps.row);
        OA_CHECK(metrics.text_field_height == steps.field);
        OA_CHECK(metrics.filter_chip_height == steps.chip);
        OA_CHECK(metrics.tab_strip_height == steps.tabs);
        // A control of the class's height takes the class's radius.
        OA_CHECK(kit::control_radius_for(steps.control) == steps.control_radius);
    }
    // Large's design: rows 12 apart, sections 24, window padding 24.
    OA_CHECK(kit::large_metrics.row_gap == 12);
    OA_CHECK(kit::large_metrics.section_gap == 24);
    OA_CHECK(kit::large_metrics.window_padding == 24);
    // A shorter control keeps Compact's radius in proportion, at least 2.
    OA_CHECK(kit::control_radius_for(17) == 3);
    OA_CHECK(kit::control_radius_for(4) == 2);
    OA_CHECK(kit::keycap_radius_for(16) == 5);
    OA_CHECK(kit::keycap_radius_for(9) == 3);
    // The classic look's two fonts.
    OA_CHECK(kit::classic_font(kit::TextLevel::title) == kit::FontRole::regular);
    OA_CHECK(kit::classic_font(kit::TextLevel::label) == kit::FontRole::regular);
    OA_CHECK(kit::classic_font(kit::TextLevel::body) == kit::FontRole::small);
    OA_CHECK(kit::classic_font(kit::TextLevel::small) == kit::FontRole::small);
}

/// One size class's row of the design's table of sizes, in points.
struct ClassSizes {
    const kit::Metrics* metrics{};    ///< the class's metrics
    int32_t dialog_width{};           ///< the settings dialog's width
    int32_t dialog_height{};          ///< the settings dialog's height
    int32_t padding{};                ///< the settings dialog's padding
    int32_t nav_width{};              ///< the nav list's width
    int32_t hint_line_characters{};   ///< a host's line's characters under a row
    int32_t notice_width{};           ///< a notice's and a prompt's width
    int32_t greatest_notice_height{}; ///< their greatest height
    kit::Point least_window{};        ///< the least window of the class, in points
};

/// The design's table of sizes: Compact, Regular and Large. Compact's least
/// window is the 640 by 480 picture; Regular's and Large's are the classes'
/// own (oa/ui/kit/layout.hpp).
const ClassSizes kClassSizes[] = {
    {&kit::compact_metrics, 480, 324, 12, 144, 50, 400, 440, {640, 480}},
    {&kit::regular_metrics, 720, 486, 16, 176, 82, 520, 500, kit::regular_from},
    {&kit::large_metrics, 960, 600, 20, 208, 114, 600, 640, kit::large_from},
};

/// Checks each class's metrics against the design's table of sizes, and
/// that each class's dialog and notices fit its least window.
void each_class_has_the_tables_sizes() {
    for (const ClassSizes& sizes : kClassSizes) {
        const kit::Metrics& metrics = *sizes.metrics;
        OA_CHECK(metrics.dialog_width == sizes.dialog_width);
        OA_CHECK(metrics.dialog_height == sizes.dialog_height);
        OA_CHECK(metrics.padding == sizes.padding);
        OA_CHECK(metrics.nav_width == sizes.nav_width);
        OA_CHECK(metrics.hint_line_characters == sizes.hint_line_characters);
        OA_CHECK(metrics.notice_width == sizes.notice_width);
        OA_CHECK(metrics.greatest_notice_height == sizes.greatest_notice_height);
        OA_CHECK(metrics.dialog_width <= sizes.least_window.x);
        OA_CHECK(metrics.dialog_height <= sizes.least_window.y);
        OA_CHECK(metrics.notice_width <= sizes.least_window.x);
        OA_CHECK(metrics.greatest_notice_height <= sizes.least_window.y);
    }
    OA_CHECK(kit::compact_metrics.language_card_width == 139);
    OA_CHECK(kit::compact_metrics.language_card_height == 182);
    OA_CHECK(kit::compact_metrics.carousel_step == 132);
    OA_CHECK(kit::regular_metrics.language_card_width == 186);
    OA_CHECK(kit::regular_metrics.language_card_height == 244);
    OA_CHECK(kit::regular_metrics.carousel_step == 177);
    OA_CHECK(kit::large_metrics.language_card_width == 232);
    OA_CHECK(kit::large_metrics.language_card_height == 304);
    OA_CHECK(kit::large_metrics.carousel_step == 236);
}

/// Checks that Regular and Large differ from Compact only in their steps on
/// Kit 2's scales, what follows from them, and the table's sizes: the
/// screens' rows, text and controls keep Compact's sizes.
void the_classes_differ_only_in_their_steps() {
    for (const kit::Metrics* metrics : {&kit::regular_metrics, &kit::large_metrics}) {
        kit::Metrics same = kit::scaled(*metrics, kit::compact_scale);
        same.dialog_width = kit::compact_metrics.dialog_width;
        same.dialog_height = kit::compact_metrics.dialog_height;
        same.padding = kit::compact_metrics.padding;
        same.nav_width = kit::compact_metrics.nav_width;
        same.hint_line_characters = kit::compact_metrics.hint_line_characters;
        same.notice_width = kit::compact_metrics.notice_width;
        same.greatest_notice_height = kit::compact_metrics.greatest_notice_height;
        same.language_card_width = kit::compact_metrics.language_card_width;
        same.language_card_height = kit::compact_metrics.language_card_height;
        same.carousel_step = kit::compact_metrics.carousel_step;
        OA_CHECK(same == kit::compact_metrics);
    }
}

} // namespace

int main() {
    every_token_keeps_its_value();
    shared_sizes();
    each_class_has_its_steps();
    each_class_has_the_tables_sizes();
    the_classes_differ_only_in_their_steps();
    return oa::test::check_exit_status();
}
