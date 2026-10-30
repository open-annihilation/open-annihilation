// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Game files screen's layouts (game_files.hpp): each step in points for
// the tablet and phone forms inside the safe area, its banner and sheet, the
// scrolling rows, and the hit test, built from the kit's parts.
//
// A step is laid out as the header bar, then a column of three blocks (a
// fixed top, a body that scrolls when the column does not fit, and a fixed
// bottom), then on the first run a footer held at the foot of the safe area.
// When even the fixed blocks leave too little room for the body, the whole
// column scrolls (the kit's scroll_column). A sheet is laid out the same way
// inside its panel, over a backdrop that makes everything under it inert. The
// kit measures, fits and wraps the texts, and lays out the text parts, the
// buttons and their rows, the icons and the plain parts; the screen's own
// compositions (its header, banners, cards, option rows, part rows, panels
// and sheets) place them.
#include "game_files_internal.hpp"

#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oa::ui::game_files {

namespace {

namespace kit = oa::ui::kit;
using detail::Action;
using detail::BannerContent;
using detail::fill;
using detail::tr;
using kit::Lettering;
using kit::Role;
using kit::TextFit;
using kit::TextStyle;

/// The sizes of one form, in points.
struct Metrics {
    float header_height{};      ///< the header bar
    float header_pad{};         ///< the header's side padding
    float badge{};              ///< the OA badge's side
    float header_text{};        ///< "OPEN ANNIHILATION  GAME FILES"
    float header_gap{};         ///< between the header's pieces
    float version_text{};       ///< the version
    float header_button_text{}; ///< OA · Aa
    float header_button_mark{}; ///< OA · Aa's icon
    float max_content_width{};  ///< the widest the column grows (0: no limit)
    float side_pad{};           ///< the column's least side padding
    float content_top{};        ///< above the column
    float content_bottom{};     ///< below the column
    float title{};              ///< a step's title
    float subtitle{};           ///< the line under it
    float lead{};               ///< S1's lead
    float title_gap{};          ///< between the title and the next line
    float block_gap{};          ///< between blocks of a step
    float banner_text{};        ///< a banner's text
    float banner_pad_x{};       ///< a banner's side padding
    float banner_pad_y{};       ///< a banner's top and bottom padding
    float banner_icon{};        ///< a banner's mark
    float card_pad{};           ///< a card's padding
    float card_gap{};           ///< between cards
    float card_icon{};          ///< a card's framed icon
    float card_title{};         ///< a card's title
    float card_text{};          ///< a card's text
    float card_min_height{};    ///< a card's least height
    float card_min_width{};     ///< narrower cards become rows
    float option_row_height{};  ///< a phone row of S1
    float option_icon{};        ///< its framed icon
    float option_title{};       ///< its title
    float option_text{};        ///< its one line
    float option_gap{};         ///< between the rows
    float footer_text{};        ///< S1's footer
    float footer_pad{};         ///< above and below the footer's line
    float button_text{};        ///< a button's label
    float button_pad{};         ///< a button's side padding
    float button_min_width{};   ///< the main buttons' least width
    float button_gap{};         ///< between buttons
    float row_pad{};            ///< a part row's side padding
    float row_mark{};           ///< its mark
    float row_title{};          ///< its name
    float row_detail{};         ///< its note
    float row_hint{};           ///< its amber hint
    float row_size{};           ///< its size
    float row_size_min{};       ///< the size column's least width
    float row_height{};         ///< a row with no control
    float row_gap{};            ///< between a row's columns
    float switch_width{};       ///< an OFF/ON switch
    float switch_text{};        ///< its labels
    float small_button_text{};  ///< Why, Show, Remove
    float small_button_pad{};   ///< their side padding
    float totals_text{};        ///< S3's totals and check lines
    float note_text{};          ///< hints under a step
    float bar_height{};         ///< a progress bar
    float panel_pad{};          ///< a panel's padding (S2, S4, S5)
    float panel_text{};         ///< a panel's main line
    float sheet_width{};        ///< a sheet's width
    float sheet_pad{};          ///< its padding
    float sheet_title{};        ///< its title
    float sheet_text{};         ///< its text
    float sheet_gap{};          ///< between its pieces
    float problem_icon{};       ///< S6's mark
    float problem_title{};      ///< S6's title
    float problem_text{};       ///< S6's text
};

/// The tablet form (a 1194x834-point window).
constexpr Metrics tablet_metrics{
    .header_height = 56.0f,
    .header_pad = 24.0f,
    .badge = 26.0f,
    .header_text = 15.0f,
    .header_gap = 12.0f,
    .version_text = 12.0f,
    .header_button_text = 11.0f,
    .header_button_mark = 20.0f,
    .max_content_width = 1030.0f,
    .side_pad = 24.0f,
    .content_top = 30.0f,
    .content_bottom = 18.0f,
    .title = 28.0f,
    .subtitle = 14.0f,
    .lead = 15.0f,
    .title_gap = 8.0f,
    .block_gap = 18.0f,
    .banner_text = 13.5f,
    .banner_pad_x = 14.0f,
    .banner_pad_y = 6.0f,
    .banner_icon = 18.0f,
    .card_pad = 18.0f,
    .card_gap = 18.0f,
    .card_icon = 44.0f,
    .card_title = 16.0f,
    .card_text = 13.0f,
    .card_min_height = 226.0f,
    .card_min_width = 220.0f,
    .option_row_height = 60.0f,
    .option_icon = 40.0f,
    .option_title = 15.0f,
    .option_text = 12.5f,
    .option_gap = 8.0f,
    .footer_text = 12.5f,
    .footer_pad = 14.0f,
    .button_text = 12.5f,
    .button_pad = 16.0f,
    .button_min_width = 150.0f,
    .button_gap = 12.0f,
    .row_pad = 14.0f,
    .row_mark = 24.0f,
    .row_title = 14.5f,
    .row_detail = 12.5f,
    .row_hint = 11.5f,
    .row_size = 13.0f,
    .row_size_min = 62.0f,
    .row_height = 46.0f,
    .row_gap = 10.0f,
    .switch_width = 84.0f,
    .switch_text = 10.5f,
    .small_button_text = 11.0f,
    .small_button_pad = 10.0f,
    .totals_text = 14.0f,
    .note_text = 12.5f,
    .bar_height = 8.0f,
    .panel_pad = 18.0f,
    .panel_text = 15.0f,
    .sheet_width = 460.0f,
    .sheet_pad = 20.0f,
    .sheet_title = 18.0f,
    .sheet_text = 12.5f,
    .sheet_gap = 10.0f,
    .problem_icon = 44.0f,
    .problem_title = 24.0f,
    .problem_text = 14.0f,
};

/// The phone form (an 852x393-point window), buttons kept at 44 pt.
constexpr Metrics phone_metrics{
    .header_height = 48.0f,
    .header_pad = 14.0f,
    .badge = 22.0f,
    .header_text = 12.5f,
    .header_gap = 10.0f,
    .version_text = 11.0f,
    .header_button_text = 10.5f,
    .header_button_mark = 18.0f,
    .max_content_width = 0.0f,
    .side_pad = 20.0f,
    .content_top = 10.0f,
    .content_bottom = 6.0f,
    .title = 19.0f,
    .subtitle = 12.0f,
    .lead = 12.0f,
    .title_gap = 3.0f,
    .block_gap = 8.0f,
    .banner_text = 11.0f,
    .banner_pad_x = 8.0f,
    .banner_pad_y = 4.0f,
    .banner_icon = 14.0f,
    .card_pad = 14.0f,
    .card_gap = 12.0f,
    .card_icon = 40.0f,
    .card_title = 14.0f,
    .card_text = 11.5f,
    .card_min_height = 0.0f,
    .card_min_width = 100000.0f,
    .option_row_height = 52.0f,
    .option_icon = 36.0f,
    .option_title = 13.5f,
    .option_text = 11.0f,
    .option_gap = 6.0f,
    .footer_text = 10.5f,
    .footer_pad = 6.0f,
    .button_text = 11.0f,
    .button_pad = 12.0f,
    .button_min_width = 124.0f,
    .button_gap = 8.0f,
    .row_pad = 10.0f,
    .row_mark = 20.0f,
    .row_title = 12.5f,
    .row_detail = 10.5f,
    .row_hint = 10.0f,
    .row_size = 11.0f,
    .row_size_min = 48.0f,
    .row_height = 34.0f,
    .row_gap = 8.0f,
    .switch_width = 72.0f,
    .switch_text = 9.5f,
    .small_button_text = 10.0f,
    .small_button_pad = 8.0f,
    .totals_text = 11.5f,
    .note_text = 10.5f,
    .bar_height = 7.0f,
    .panel_pad = 12.0f,
    .panel_text = 13.0f,
    .sheet_width = 420.0f,
    .sheet_pad = 16.0f,
    .sheet_title = 16.0f,
    .sheet_text = 11.5f,
    .sheet_gap = 8.0f,
    .problem_icon = 40.0f,
    .problem_title = 19.0f,
    .problem_text = 12.5f,
};

/// A window whose shorter side is under this many points takes the compact sizes.
constexpr float compact_short_side_points = 600.0f;
/// The least height, in points, a scrolling body keeps before the whole column scrolls.
constexpr float least_body_points = 96.0f;
/// The margin, in points, a sheet keeps from the safe area's edges.
constexpr float sheet_margin_points = 12.0f;

/// The form being laid out: the model, the form's sizes and the kit's typesetter.
struct Form {
    const Model& model;     ///< what to show
    const Metrics& metrics; ///< the form's sizes
    kit::Typesetter type{}; ///< the scale and the text measure
    bool compact{};         ///< the compact sizes: the phone form, or a small window
};

/// Parts laid out in a column, their rectangles relative to the block's top.
struct Block {
    std::vector<kit::Item> items{}; ///< in painting order
    int height{};                   ///< pixels
};

/// Returns one of the screen's buttons as the kit lays a button out.
///
/// @param action the button
/// @return its label, look, mark and control
kit::ButtonSpec spec_of(const Action& action) {
    kit::ButtonSpec button{};
    button.label = action.label;
    button.role = action.role;
    button.glyph = action.glyph;
    button.glyph_points = action.glyph_points;
    button.control = control_id(action.control);
    button.enabled = action.enabled;
    return button;
}

/// Adds a row of the screen's buttons with the kit's button row at the form's sizes: a
/// button's label and padding, the main button's least width, the gap, and the least
/// height a control keeps.
///
/// @param form the form laid out
/// @param[in,out] block the block
/// @param x the row's left, pixels
/// @param y the row's top, pixels
/// @param room the row's width, pixels
/// @param left the buttons at the left, left to right
/// @param right the buttons at the right, left to right
/// @return the height taken, pixels
int button_row_of(
    const Form& form,
    Block& block,
    int x,
    int y,
    int room,
    const std::vector<Action>& left,
    const std::vector<Action>& right
) {
    std::vector<kit::ButtonSpec> lefts;
    for (const Action& action : left)
        lefts.push_back(spec_of(action));
    std::vector<kit::ButtonSpec> rights;
    for (const Action& action : right)
        rights.push_back(spec_of(action));
    const Metrics& metrics = form.metrics;
    const kit::ButtonRowSizes sizes{
        metrics.button_text,
        metrics.button_pad,
        metrics.button_min_width,
        metrics.button_gap,
        min_button_points,
    };
    return kit::button_row(block.items, form.type, {x, y}, room, lefts, rights, sizes);
}

/// Adds a banner: its mark, its text and its buttons at the right (below the text when the
/// banner is narrow).
///
/// @param form the form laid out
/// @param[in,out] block the block
/// @param x the column's left, pixels
/// @param y the banner's top, pixels
/// @param room the column's width, pixels
/// @param banner what it shows
/// @return the height taken, pixels
int add_banner(
    const Form& form, Block& block, int x, int y, int room, const BannerContent& banner
) {
    const Metrics& metrics = form.metrics;
    const int pad_x = kit::canvas_pixels(form.type, metrics.banner_pad_x);
    const int pad_y = kit::canvas_pixels(form.type, metrics.banner_pad_y);
    const int icon = kit::canvas_pixels(form.type, metrics.banner_icon);
    const int gap = kit::canvas_pixels(form.type, metrics.button_gap);
    const int button_height = kit::canvas_pixels(form.type, min_button_points);
    const Lettering style{metrics.banner_text, false, text_colour, TextStyle::lead};
    std::vector<int> widths;
    int buttons = 0;
    for (const Action& action : banner.actions) {
        const float least = action.role == Role::button_main ? metrics.button_min_width : 0.0f;
        widths.push_back(
            kit::button_span(
                form.type, spec_of(action), metrics.button_text, metrics.button_pad, least
            )
        );
        buttons += widths.back() + (widths.size() > 1 ? gap : 0);
    }
    const int text_x = x + pad_x + icon + gap;
    const int inner_right = x + room - pad_x;
    int text_room = inner_right - text_x - (buttons > 0 ? buttons + gap : 0);
    const bool below = buttons > 0 && text_room < room * 2 / 5;
    if (below)
        text_room = inner_right - text_x;
    Block inner;
    const int text_height = kit::text_part(
        inner.items, form.type, Role::text, {text_x, 0}, text_room, banner.text, style
    );
    int height = 0;
    if (below) {
        // A narrow banner puts its buttons under the text, wrapping them as a button row does.
        Block row;
        const int row_top = pad_y + std::max(text_height, icon) + gap;
        const int row_height =
            button_row_of(form, row, x + pad_x, row_top, room - 2 * pad_x, {}, banner.actions);
        height = row_top + row_height + pad_y;
        kit::plain_part(block.items, Role::banner, {x, y, room, height}, banner.colour);
        kit::icon_part(
            block.items, {x + pad_x, y + pad_y, icon, icon}, banner.glyph, banner.colour, false
        );
        kit::move_parts(block.items, inner.items, y + pad_y);
        kit::move_parts(block.items, row.items, y);
        return height;
    }
    height = pad_y + std::max({text_height, icon, buttons > 0 ? button_height : 0}) + pad_y;
    kit::plain_part(block.items, Role::banner, {x, y, room, height}, banner.colour);
    kit::icon_part(
        block.items,
        {x + pad_x, y + (height - icon) / 2, icon, icon},
        banner.glyph,
        banner.colour,
        false
    );
    kit::move_parts(block.items, inner.items, y + (height - text_height) / 2);
    int at = inner_right - buttons;
    const int button_top = y + (height - button_height) / 2;
    for (std::size_t index = 0; index < banner.actions.size(); ++index) {
        kit::button_part(
            block.items,
            form.type,
            {at, button_top, widths[index], button_height},
            spec_of(banner.actions[index]),
            metrics.button_text
        );
        at += widths[index] + gap;
    }
    return height;
}

/// Tells whether a banner belongs on a step.
///
/// @param step the step
/// @param banner the banner
/// @param pending a replacement waits (S8)
/// @return true when the step shows it
bool banner_on_step(Step step, Banner banner, bool pending) noexcept {
    switch (step) {
    case Step::first_run:
        return banner == Banner::continue_copy || banner == Banner::folder_refused ||
               banner == Banner::nothing_added;
    case Step::copying:
    case Step::checking:
    case Step::ready_to_play:
        return banner == Banner::copy_went_on || banner == Banner::copy_resumed;
    case Step::manage:
        return pending || banner == Banner::added || banner == Banner::next_start ||
               banner == Banner::continue_copy || banner == Banner::nothing_added;
    case Step::looking:
    case Step::nested_offer:
    case Step::already_there:
    case Step::ready_to_copy:
    case Step::problem:
        return false;
    }
    return false;
}

/// Adds the step's banner, when it has one, and the gap after it.
///
/// @param form the form laid out
/// @param[in,out] block the block
/// @param x the column's left, pixels
/// @param y where it goes, pixels
/// @param room the column's width, pixels
/// @return the height taken with the gap, pixels
int add_step_banner(const Form& form, Block& block, int x, int y, int room) {
    const Model& model = form.model;
    if (!banner_on_step(model.step, model.banner, model.pending_replacement))
        return 0;
    BannerContent banner = detail::banner_content(model);
    if (!banner.shown)
        return 0;
    // In the compact form the continue banner leaves Choose folder to the row just below it.
    if (form.compact && model.step == Step::first_run && model.offers_pick_folder)
        banner.actions.erase(
            std::remove_if(
                banner.actions.begin(),
                banner.actions.end(),
                [](const Action& action) {
                    return action.control.kind == ControlKind::choose_folder;
                }
            ),
            banner.actions.end()
        );
    return add_banner(form, block, x, y, room, banner) +
           kit::canvas_pixels(form.type, form.metrics.block_gap);
}

/// Adds a step's title and the line under it.
///
/// @param form the form laid out
/// @param[in,out] block the block
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @param title the title
/// @param subtitle the line under it; empty for none
/// @return the height taken, pixels, with the gap after
int add_title(
    const Form& form,
    Block& block,
    int x,
    int room,
    std::string_view title,
    std::string_view subtitle
) {
    const Metrics& metrics = form.metrics;
    int y = block.height;
    y += kit::text_part(
        block.items,
        form.type,
        Role::title,
        {x, y},
        room,
        title,
        {metrics.title, true, text_colour, TextStyle::title},
        TextFit::column,
        2
    );
    if (!subtitle.empty()) {
        y += kit::canvas_pixels(form.type, metrics.title_gap / 2.0f);
        y += kit::text_part(
            block.items,
            form.type,
            Role::text,
            {x, y},
            room,
            subtitle,
            {metrics.subtitle, false, dim_colour, TextStyle::subtitle},
            TextFit::column,
            2
        );
    }
    y += kit::canvas_pixels(form.type, metrics.block_gap);
    return y - block.height;
}

/// One way in on S1.
struct Option {
    Glyph glyph{};            ///< its icon
    std::string title{};      ///< its title
    std::string text{};       ///< the card's text
    std::string short_text{}; ///< the row's one line
    Action action{};          ///< its button
};

/// Returns S1's ways in that the platform offers, in order.
///
/// @param model the model
/// @return the options
std::vector<Option> first_run_options(const Model& model) {
    std::vector<Option> options;
    if (model.offers_pick_folder) {
        Option option{};
        option.glyph = Glyph::folder;
        option.title = tr("Choose the game folder");
        option.text = fill(
            "Pick the folder that holds totala1.hpi: {places}. It is checked, then copied in.",
            {{"places", platform_word(model, detail::word_picker_places)}}
        );
        option.short_text = fill(
            "The folder that holds totala1.hpi: {places}.",
            {{"places", platform_word(model, detail::word_picker_places_short)}}
        );
        option.action.label = tr("Choose folder");
        option.action.control = {ControlKind::choose_folder};
        option.action.command = Command::pick_game_folder;
        option.action.role = Role::button_main;
        options.push_back(std::move(option));
    }
    if (model.offers_copy_yourself) {
        Option option{};
        option.glyph = Glyph::device;
        option.title = tr("Copy it yourself");
        option.text = fill(
            "{steps} Name it Total Annihilation.",
            {{"steps", platform_word(model, detail::word_copy_yourself_steps)}}
        );
        option.short_text = fill(
            "{steps}, named Total Annihilation.",
            {{"steps", platform_word(model, detail::word_copy_yourself_short)}}
        );
        option.action.label = tr("I have copied it");
        option.action.control = {ControlKind::i_have_copied};
        option.action.command = Command::check_copied;
        option.action.role = options.empty() ? Role::button_main : Role::button;
        options.push_back(std::move(option));
    }
    if (model.offers_pick_folder) {
        Option option{};
        option.glyph = Glyph::disk;
        option.title = tr("Play the 1997 demo");
        option.text = fill(
            "Choose the installer of the Total Annihilation demo (1997). The game recognises it, "
            "never runs it, and unpacks its {size} of game data.",
            {{"size", std::string(detail::demo_data_size_text)}}
        );
        option.short_text = fill(
            "The demo's installer: recognised, never run, {size} unpacked.",
            {{"size", std::string(detail::demo_data_size_text)}}
        );
        option.action.label = tr("Choose installer");
        option.action.control = {ControlKind::choose_installer};
        option.action.command = Command::pick_installer;
        options.push_back(std::move(option));
    }
    return options;
}

/// Adds S1's ways in as cards in a row.
///
/// @param form the form laid out
/// @param[in,out] block the block
/// @param x the column's left, pixels
/// @param y the cards' top, pixels
/// @param room the column's width, pixels
/// @param options the ways in
/// @return the height taken, pixels
int add_cards(
    const Form& form, Block& block, int x, int y, int room, const std::vector<Option>& options
) {
    const Metrics& metrics = form.metrics;
    const int count = static_cast<int>(options.size());
    const int gap = kit::canvas_pixels(form.type, metrics.card_gap);
    const int card_width = (room - gap * (count - 1)) / count;
    const int pad = kit::canvas_pixels(form.type, metrics.card_pad);
    const int icon = kit::canvas_pixels(form.type, metrics.card_icon);
    const int inner = card_width - 2 * pad;
    const int button_height = kit::canvas_pixels(form.type, min_button_points);
    const int inner_gap = kit::canvas_pixels(form.type, 10.0f);
    std::vector<Block> texts(options.size());
    int height = kit::canvas_pixels(form.type, metrics.card_min_height);
    for (std::size_t index = 0; index < options.size(); ++index) {
        Block& text = texts[index];
        const int card_x = x + static_cast<int>(index) * (card_width + gap);
        int at = pad + icon + inner_gap;
        at += kit::text_part(
            text.items,
            form.type,
            Role::title,
            {card_x + pad, at},
            inner,
            options[index].title,
            {metrics.card_title, true, text_colour, TextStyle::row_title},
            TextFit::column,
            2
        );
        at += inner_gap;
        at += kit::text_part(
            text.items,
            form.type,
            Role::text,
            {card_x + pad, at},
            inner,
            options[index].text,
            {metrics.card_text, false, dim_colour, TextStyle::body}
        );
        height = std::max(height, at + inner_gap + button_height + pad);
    }
    for (std::size_t index = 0; index < options.size(); ++index) {
        const int card_x = x + static_cast<int>(index) * (card_width + gap);
        kit::plain_part(block.items, Role::panel, {card_x, y, card_width, height}, line_colour);
        kit::icon_part(
            block.items,
            {card_x + pad, y + pad, icon, icon},
            options[index].glyph,
            green_colour,
            true
        );
        kit::move_parts(block.items, texts[index].items, y);
        kit::button_part(
            block.items,
            form.type,
            {card_x + pad, y + height - pad - button_height, inner, button_height},
            spec_of(options[index].action),
            metrics.button_text
        );
    }
    return height;
}

/// Adds S1's ways in as rows: icon, title, one line and the button at the right.
///
/// @param form the form laid out
/// @param[in,out] block the block
/// @param x the column's left, pixels
/// @param y the first row's top, pixels
/// @param room the column's width, pixels
/// @param options the ways in
/// @return the height taken, pixels
int add_option_rows(
    const Form& form, Block& block, int x, int y, int room, const std::vector<Option>& options
) {
    const Metrics& metrics = form.metrics;
    const int row_height = std::max(
        kit::canvas_pixels(form.type, metrics.option_row_height),
        kit::canvas_pixels(form.type, min_button_points + 8.0f)
    );
    const int pad = kit::canvas_pixels(form.type, 10.0f);
    const int icon = kit::canvas_pixels(form.type, metrics.option_icon);
    const int gap = kit::canvas_pixels(form.type, 12.0f);
    const int button_height = kit::canvas_pixels(form.type, min_button_points);
    int widest = 0;
    for (const Option& option : options)
        widest = std::max(
            widest,
            kit::button_span(
                form.type,
                spec_of(option.action),
                metrics.button_text,
                metrics.button_pad,
                metrics.button_min_width
            )
        );
    widest = std::min(widest, room / 2);
    int at = y;
    for (const Option& option : options) {
        kit::plain_part(block.items, Role::panel, {x, at, room, row_height}, line_colour);
        kit::icon_part(
            block.items,
            {x + pad, at + (row_height - icon) / 2, icon, icon},
            option.glyph,
            green_colour,
            true
        );
        const int text_x = x + pad + icon + gap;
        const int text_room = x + room - pad - widest - gap - text_x;
        const Lettering title{metrics.option_title, true, text_colour, TextStyle::row_title};
        const Lettering line{metrics.option_text, false, dim_colour, TextStyle::row_detail};
        const int title_height =
            kit::measured_line(form.type.measure, kit::canvas_font(form.type, title.points), true);
        const int line_height =
            kit::measured_line(form.type.measure, kit::canvas_font(form.type, line.points), false);
        const int spacing = kit::canvas_pixels(form.type, 2.0f);
        const int text_top = at + (row_height - title_height - spacing - line_height) / 2;
        kit::line_part(
            block.items,
            form.type,
            text_x,
            text_top + title_height / 2,
            text_room,
            option.title,
            title
        );
        kit::line_part(
            block.items,
            form.type,
            text_x,
            text_top + title_height + spacing + line_height / 2,
            text_room,
            option.short_text,
            line
        );
        kit::button_part(
            block.items,
            form.type,
            {x + room - pad - widest, at + (row_height - button_height) / 2, widest, button_height},
            spec_of(option.action),
            metrics.button_text
        );
        at += row_height + kit::canvas_pixels(form.type, metrics.option_gap);
    }
    return at - y - kit::canvas_pixels(form.type, metrics.option_gap);
}

/// The blocks of one step's column.
struct Column {
    Block top{};    ///< fixed at the top
    Block body{};   ///< scrolls when the column does not fit
    Block bottom{}; ///< fixed at the bottom
    Block footer{}; ///< held at the foot of the safe area (S1)
};

/// Returns a row's mark and its colour.
///
/// @param row the row
/// @return the glyph and colour
std::pair<Glyph, Colour> row_mark(const PartRow& row) {
    switch (row.mark) {
    case Mark::none:
    case Mark::waiting:
        return {Glyph::none, dim_colour};
    case Mark::found:
    case Mark::done:
        return {Glyph::check, green_colour};
    case Mark::missing:
        if (row.kind == PartKind::update_31c)
            return {Glyph::warning, amber_colour};
        return {Glyph::dash, dim_colour};
    case Mark::refused:
        return {Glyph::cross, red_colour};
    case Mark::left_out:
        return {Glyph::dash, dim_colour};
    case Mark::copying:
        return {Glyph::clock, amber_colour};
    case Mark::warning:
        return {Glyph::warning, amber_colour};
    }
    return {Glyph::none, dim_colour};
}

/// Adds a panel: a card around content laid out inside it.
///
/// @param[in,out] block the block
/// @param box the panel's box, pixels
/// @param[in,out] inner the content, already placed in the block's coordinates
void add_panel(Block& block, Rect box, Block& inner) {
    kit::plain_part(block.items, Role::panel, box, line_colour);
    kit::move_parts(block.items, inner.items, 0);
}

/// Adds one part's row.
///
/// @param form the form laid out
/// @param[in,out] block the block
/// @param x the row's left, pixels
/// @param y its top, pixels
/// @param room its width, pixels
/// @param row the part
/// @param index its place in Model::parts; -1 for a row the UI added
/// @param switch_column the rows keep a column for switches
/// @return the row's height, pixels
int add_part_row(
    const Form& form,
    Block& block,
    int x,
    int y,
    int room,
    const PartRow& row,
    int index,
    bool switch_column
) {
    const Model& model = form.model;
    const Metrics& metrics = form.metrics;
    const bool choosing = model.step == Step::ready_to_copy;
    const bool managing = model.step == Step::manage;
    const bool missing = detail::part_missing(row);
    const bool has_switch = choosing && row.has_switch && !missing && index >= 0;
    std::optional<Action> button;
    if (choosing && row.kind == PartKind::mod && detail::part_refused(row) && index >= 0) {
        button = Action{};
        button->label = tr("Why");
        button->control = {ControlKind::part_why, static_cast<uint16_t>(index)};
    } else if (choosing && row.kind == PartKind::left_out) {
        button = Action{};
        button->label = tr("Show");
        button->control = {ControlKind::show_left_out};
    } else if (
        managing && index >= 0 && row.kind != PartKind::game_archives &&
        (row.removable || row.kind == PartKind::demo_data)
    ) {
        button = Action{};
        button->label = tr("Remove");
        button->control = {ControlKind::manage_remove, static_cast<uint16_t>(index)};
        button->role = Role::button_danger;
        button->enabled = !model.pending_replacement;
    }
    const int control_height = kit::canvas_pixels(form.type, min_button_points);
    const int height = has_switch || button
                           ? std::max(
                                 kit::canvas_pixels(form.type, metrics.row_height),
                                 kit::canvas_pixels(form.type, min_button_points + 4.0f)
                             )
                           : kit::canvas_pixels(form.type, metrics.row_height);
    kit::plain_part(block.items, Role::row, {x, y, room, height}, panel_colour);
    const int middle = y + height / 2;
    const int pad = kit::canvas_pixels(form.type, metrics.row_pad);
    const int gap = kit::canvas_pixels(form.type, metrics.row_gap);
    int right = x + room - pad;
    if (switch_column) {
        const int width = kit::canvas_pixels(form.type, metrics.switch_width);
        if (has_switch) {
            kit::Item part{};
            part.role = Role::toggle;
            part.rect = {right - width, middle - control_height / 2, width, control_height};
            part.lines = {tr("Off"), tr("On")};
            part.style = TextStyle::button;
            part.pixel_size = kit::canvas_font(form.type, metrics.switch_text);
            part.bold = true;
            part.colour = text_colour;
            part.control = control_id({ControlKind::part_switch, static_cast<uint16_t>(index)});
            part.state.on = row.on;
            block.items.push_back(std::move(part));
        }
        right -= width + gap;
    }
    const std::string size = detail::part_size(row);
    const Lettering size_style{
        metrics.row_size, false, oa::ui::kit::screen_colour::button_text, TextStyle::row_detail
    };
    const int size_room = std::max(
        kit::canvas_pixels(form.type, metrics.row_size_min),
        kit::measured_width(
            form.type.measure, size, kit::canvas_font(form.type, metrics.row_size), false
        )
    );
    kit::line_part(
        block.items, form.type, right, middle, size_room, size, size_style, TextFit::tight_right
    );
    right -= size_room + gap;
    if (button) {
        const int width = std::min(
            kit::button_span(
                form.type,
                spec_of(*button),
                metrics.small_button_text,
                metrics.small_button_pad,
                0.0f
            ),
            room / 3
        );
        kit::button_part(
            block.items,
            form.type,
            {right - width, middle - control_height / 2, width, control_height},
            spec_of(*button),
            metrics.small_button_text
        );
        right -= width + gap;
    }
    const auto [glyph, glyph_colour] = row_mark(row);
    const int mark = kit::canvas_pixels(form.type, metrics.row_mark);
    kit::icon_part(
        block.items, {x + pad, middle - mark / 2, mark, mark}, glyph, glyph_colour, false
    );
    const int text_x = x + pad + mark + gap;
    Colour title_colour = text_colour;
    if (row.mark == Mark::done)
        title_colour = green_colour;
    else if (row.mark == Mark::copying)
        title_colour = amber_colour;
    else if (
        row.mark == Mark::waiting || row.mark == Mark::left_out ||
        (missing && row.kind != PartKind::update_31c)
    )
        title_colour = dim_colour;
    const int taken = kit::line_part(
        block.items,
        form.type,
        text_x,
        middle,
        right - text_x,
        detail::part_name(row, managing),
        {metrics.row_title, true, title_colour, TextStyle::row_title}
    );
    int at = text_x + taken + gap;
    const std::string note = detail::part_detail(model, row);
    const Colour note_colour =
        row.kind == PartKind::update_31c && missing ? amber_colour : dim_colour;
    const Lettering note_style{metrics.row_detail, false, note_colour, TextStyle::row_detail};
    const Lettering hint_style{metrics.row_hint, false, amber_colour, TextStyle::mark_text};
    const int least = kit::canvas_pixels(form.type, 24.0f);
    if (taken > 0 && !note.empty() && right - at >= least)
        at +=
            kit::line_part(block.items, form.type, at, middle, right - at, note, note_style) + gap;
    if (taken > 0 && right - at >= least) {
        const int hint_size = kit::canvas_font(form.type, hint_style.points);
        std::string hint = detail::part_hint(model, row, false);
        if (!hint.empty() &&
            kit::measured_width(form.type.measure, hint, hint_size, false) > right - at)
            hint = detail::part_hint(model, row, true);
        if (!hint.empty() &&
            kit::measured_width(form.type.measure, hint, hint_size, false) <= right - at)
            kit::line_part(block.items, form.type, at, middle, right - at, hint, hint_style);
    }
    return height;
}

/// A row to show and its place in Model::parts (-1: added by the UI).
using ShownRow = std::pair<PartRow, int>;

/// Returns the rows a step shows: the model's parts, and on S3 and S4 the Left out row (when
/// something is left out) or the demo's row, when the model has none.
///
/// @param model the model
/// @return the rows
std::vector<ShownRow> shown_rows(const Model& model) {
    std::vector<ShownRow> rows;
    bool has_left_out = false;
    bool has_demo = false;
    for (std::size_t index = 0; index < model.parts.size(); ++index) {
        rows.emplace_back(model.parts[index], static_cast<int>(index));
        has_left_out = has_left_out || model.parts[index].kind == PartKind::left_out;
        has_demo = has_demo || model.parts[index].kind == PartKind::demo;
    }
    if (model.step != Step::ready_to_copy && model.step != Step::copying)
        return rows;
    if (model.demo) {
        if (!has_demo) {
            PartRow row{};
            row.kind = PartKind::demo;
            row.bytes = model.copy_bytes;
            row.mark = Mark::found;
            rows.emplace_back(row, -1);
        }
    } else if (!has_left_out && !model.left_out.empty()) {
        PartRow row{};
        row.kind = PartKind::left_out;
        row.bytes = model.left_out_bytes;
        row.count = static_cast<uint32_t>(model.left_out.size());
        row.mark = Mark::left_out;
        rows.emplace_back(row, -1);
    }
    return rows;
}

/// Adds the parts' rows in a panel, with dividers between them.
///
/// @param form the form laid out
/// @param[in,out] block the block
/// @param x the panel's left, pixels
/// @param y its top, pixels
/// @param room its width, pixels
/// @return the height taken, pixels
int add_rows_panel(const Form& form, Block& block, int x, int y, int room) {
    const std::vector<ShownRow> rows = shown_rows(form.model);
    if (rows.empty())
        return 0;
    bool switch_column = false;
    for (const auto& [row, index] : rows)
        switch_column =
            switch_column || (form.model.step == Step::ready_to_copy && row.has_switch &&
                              !detail::part_missing(row) && index >= 0);
    const int border = std::max(1, kit::canvas_pixels(form.type, 1.0f));
    Block inner;
    int at = y + border;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (index > 0) {
            kit::plain_part(
                inner.items, Role::divider, {x + border, at, room - 2 * border, border}, line_colour
            );
            at += border;
        }
        at += add_part_row(
            form,
            inner,
            x + border,
            at,
            room - 2 * border,
            rows[index].first,
            rows[index].second,
            switch_column
        );
    }
    at += border;
    add_panel(block, {x, y, room, at - y}, inner);
    return at - y;
}

/// Adds the header bar: the OA badge (the Open Annihilation icon), "OPEN ANNIHILATION  GAME
/// FILES", the version and, except in the management state, the OA · Aa button (the icon and
/// "Aa").
///
/// @param form the form laid out
/// @param[in,out] block the block (canvas coordinates)
/// @param safe the safe area, pixels
/// @return the header's bottom, pixels
int add_header(const Form& form, Block& block, Rect safe) {
    const Metrics& metrics = form.metrics;
    const Rect bar{
        safe.x, safe.y, safe.width, kit::canvas_pixels(form.type, metrics.header_height)
    };
    kit::plain_part(block.items, Role::header_bar, bar, line_colour);
    const int pad = kit::canvas_pixels(form.type, metrics.header_pad);
    const int gap = kit::canvas_pixels(form.type, metrics.header_gap);
    const int middle = bar.y + bar.height / 2;
    const int badge = kit::canvas_pixels(form.type, metrics.badge);
    kit::Item badge_part{};
    badge_part.role = Role::badge;
    badge_part.rect = {bar.x + pad, middle - badge / 2, badge, badge};
    badge_part.colour = green_colour;
    badge_part.glyph = Glyph::oa;
    block.items.push_back(badge_part);
    int right = bar.x + bar.width - pad;
    const int text_x = bar.x + pad + badge + gap;
    if (!form.model.management) {
        Action language{};
        language.label = tr("Aa");
        language.glyph = Glyph::oa;
        language.glyph_points = metrics.header_button_mark;
        language.control = {ControlKind::language};
        language.command = Command::open_language;
        const int height = kit::canvas_pixels(form.type, min_button_points);
        const int width = std::min(
            kit::button_span(form.type, spec_of(language), metrics.header_button_text, 12.0f, 0.0f),
            std::max(0, right - text_x)
        );
        kit::button_part(
            block.items,
            form.type,
            {right - width, middle - height / 2, width, height},
            spec_of(language),
            metrics.header_button_text
        );
        right -= width + gap;
    }
    const int version = kit::line_part(
        block.items,
        form.type,
        right,
        middle,
        std::max(0, right - text_x),
        form.model.version,
        {metrics.version_text, false, dim_colour, TextStyle::small},
        TextFit::tight_right,
        Role::version
    );
    if (version > 0)
        right -= version + gap;
    const Lettering header{metrics.header_text, true, text_colour, TextStyle::header};
    const int title = kit::line_part(
        block.items,
        form.type,
        text_x,
        middle,
        right - text_x,
        "OPEN ANNIHILATION",
        header,
        TextFit::tight_left,
        Role::header_text
    );
    const int after = text_x + title + gap;
    if (title > 0 && right - after > kit::canvas_pixels(form.type, 40.0f))
        kit::line_part(
            block.items,
            form.type,
            after,
            middle,
            right - after,
            tr("GAME FILES"),
            {metrics.header_text, false, dim_colour, TextStyle::header},
            TextFit::tight_left,
            Role::header_text
        );
    return bar.y + bar.height;
}

/// Lays out S1: the title, the lead, the banner, the ways in and the footer.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column first_run_column(const Form& form, int x, int room) {
    const Model& model = form.model;
    const Metrics& metrics = form.metrics;
    const std::string device = platform_word(model, detail::word_device);
    Column column;
    Block& top = column.top;
    int y = kit::text_part(
        top.items,
        form.type,
        Role::title,
        {x, 0},
        room,
        tr("Add your Total Annihilation files"),
        {metrics.title, true, text_colour, TextStyle::title},
        TextFit::column,
        2
    );
    y += kit::canvas_pixels(form.type, metrics.title_gap);
    const Lettering lead{metrics.lead, false, dim_colour, TextStyle::lead};
    if (form.compact)
        y += kit::text_part(
            top.items,
            form.type,
            Role::text,
            {x, y},
            room,
            fill(
                "Copied onto this {device} once from your own copy. Nothing is downloaded.",
                {{"device", device}}
            ),
            lead,
            TextFit::column,
            1
        );
    else
        y += kit::text_part(
            top.items,
            form.type,
            Role::text,
            {x, y},
            std::min(room, kit::canvas_pixels(form.type, 900.0f)),
            fill(
                "Open Annihilation plays the game from your own copy, as it does on a computer. It "
                "comes with no game data and downloads nothing: the files are copied onto this "
                "{device} once and stay here.",
                {{"device", device}}
            ),
            lead
        );
    y += kit::canvas_pixels(form.type, metrics.block_gap);
    y += add_step_banner(form, top, x, y, room);
    top.height = y;

    const std::vector<Option> options = first_run_options(model);
    Block& body = column.body;
    if (!options.empty()) {
        const int count = static_cast<int>(options.size());
        const int card_width =
            (room - kit::canvas_pixels(form.type, metrics.card_gap) * (count - 1)) / count;
        const bool cards =
            !form.compact && card_width >= kit::canvas_pixels(form.type, metrics.card_min_width);
        body.height = cards ? add_cards(form, body, x, 0, room, options)
                            : add_option_rows(form, body, x, 0, room, options);
    }
    if (!form.compact && model.offers_pick_folder) {
        const int at = body.height + kit::canvas_pixels(form.type, metrics.block_gap);
        const int height = kit::text_part(
            body.items,
            form.type,
            Role::text,
            {x, at},
            std::min(room, kit::canvas_pixels(form.type, 900.0f)),
            tr("You need the folder of an installed copy of Total Annihilation, the one that holds "
               "totala1.hpi: about 1.1 GB with both expansions and the music. Installers of the "
               "full game are Windows programs and cannot be used here; install the game on a "
               "computer and bring its folder."),
            {metrics.note_text, false, dim_colour, TextStyle::small}
        );
        if (height > 0)
            body.height = at + height;
    }

    Block& footer = column.footer;
    const Lettering small{metrics.footer_text, false, dim_colour, TextStyle::small};
    const int pad = kit::canvas_pixels(form.type, metrics.footer_pad);
    const int line_height =
        kit::measured_line(form.type.measure, kit::canvas_font(form.type, small.points), false);
    const int middle = pad + line_height / 2;
    int at = x;
    if (model.free_known)
        at += kit::line_part(
                  footer.items,
                  form.type,
                  at,
                  middle,
                  room,
                  fill(
                      "{size} free on this {device}.",
                      {{"size", size_text(model.free_bytes)}, {"device", device}}
                  ),
                  small
              ) +
              kit::canvas_pixels(form.type, 20.0f);
    const std::string hint =
        form.compact ? tr("OA settings › Game files manages them later.")
                     : tr("OA settings › Game files adds an expansion or removes the files later.");
    if (x + room - at > kit::canvas_pixels(form.type, 60.0f))
        kit::line_part(footer.items, form.type, at, middle, x + room - at, hint, small);
    footer.height = pad * 2 + line_height;
    return column;
}

/// Lays out S2 while the source is listed: the count growing, Cancel and a busy bar.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column looking_column(const Form& form, int x, int room) {
    const Model& model = form.model;
    const Metrics& metrics = form.metrics;
    Column column;
    column.top.height = add_title(
        form,
        column.top,
        x,
        room,
        model.demo ? tr("Looking at the file") : tr("Looking at the folder"),
        model.location
    );
    Block inner;
    const int pad = kit::canvas_pixels(form.type, metrics.panel_pad);
    const int inner_x = x + pad;
    const int inner_room = room - 2 * pad;
    Action cancel{};
    cancel.label = tr("Cancel");
    cancel.control = {ControlKind::cancel};
    cancel.command = Command::cancel_scan;
    const int cancel_width = std::min(
        kit::button_span(form.type, spec_of(cancel), metrics.button_text, metrics.button_pad, 0.0f),
        inner_room / 2
    );
    const int button_height = kit::canvas_pixels(form.type, min_button_points);
    kit::button_part(
        inner.items,
        form.type,
        {inner_x + inner_room - cancel_width, pad, cancel_width, button_height},
        spec_of(cancel),
        metrics.button_text
    );
    std::string count;
    if (model.listed_files == 0 && model.listed_bytes == 0)
        count = fill(
            "Looking at {location}…",
            {{"location", model.location.empty() ? tr("the folder you chose") : model.location}}
        );
    else
        count = fill(
            model.listed_files == 1 ? "1 file, {size} so far" : "{n} files, {size} so far",
            {{"n", std::to_string(model.listed_files)}, {"size", size_text(model.listed_bytes)}}
        );
    kit::line_part(
        inner.items,
        form.type,
        inner_x,
        pad + button_height / 2,
        inner_room - cancel_width - kit::canvas_pixels(form.type, metrics.button_gap),
        count,
        {metrics.panel_text, true, text_colour, TextStyle::body}
    );
    int y = pad + button_height + kit::canvas_pixels(form.type, 12.0f);
    const int bar = std::max(2, kit::canvas_pixels(form.type, metrics.bar_height));
    kit::plain_part(inner.items, Role::progress_busy, {inner_x, y, inner_room, bar}, text_colour);
    y += bar + kit::canvas_pixels(form.type, 10.0f);
    y += kit::text_part(
        inner.items,
        form.type,
        Role::text,
        {inner_x, y},
        inner_room,
        tr("Reading names and sizes only: nothing is copied or downloaded yet."),
        {metrics.note_text, false, dim_colour, TextStyle::small}
    );
    y += pad;
    add_panel(column.body, {x, 0, room, y}, inner);
    column.body.height = y;
    return column;
}

/// Shows a nested folder's relative name with the arrows the locations use.
///
/// @param relative the '/'-separated name
/// @return "Games › Total Annihilation"
std::string nested_name(std::string_view relative) {
    std::string name;
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end = std::min(relative.find('/', start), relative.size());
        if (end > start) {
            if (!name.empty())
                name += " › ";
            name.append(relative.substr(start, end - start));
        }
        if (end >= relative.size())
            break;
        start = end + 1;
    }
    return name;
}

/// Lays out S2's offer of a game folder found inside the chosen one.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column nested_column(const Form& form, int x, int room) {
    const Model& model = form.model;
    const Metrics& metrics = form.metrics;
    Column column;
    Block& top = column.top;
    int y = add_title(
        form,
        top,
        x,
        room,
        tr("A game folder inside"),
        fill(
            "You chose: {location}",
            {{"location", model.location.empty() ? tr("a folder") : model.location}}
        )
    );
    y -= kit::canvas_pixels(form.type, metrics.block_gap / 2.0f);
    const std::string offer =
        model.nested.size() == 1
            ? fill(
                  "This folder holds no game archives, but {folder} does.",
                  {{"folder", nested_name(model.nested.front())}}
              )
            : tr("This folder holds no game archives, but folders inside it do. Choose one.");
    y += kit::text_part(
        top.items,
        form.type,
        Role::text,
        {x, y},
        room,
        offer,
        {metrics.panel_text, false, text_colour, TextStyle::body}
    );
    y += kit::canvas_pixels(form.type, metrics.block_gap);
    top.height = y;

    Block& body = column.body;
    const int row_height = kit::canvas_pixels(form.type, min_button_points + 8.0f);
    const int pad = kit::canvas_pixels(form.type, 10.0f);
    const int icon = kit::canvas_pixels(form.type, metrics.row_mark);
    const int gap = kit::canvas_pixels(form.type, metrics.row_gap);
    const int button_height = kit::canvas_pixels(form.type, min_button_points);
    int at = 0;
    for (std::size_t index = 0; index < model.nested.size(); ++index) {
        Action use{};
        use.label = tr("Use that folder");
        use.control = {ControlKind::nested_choice, static_cast<uint16_t>(index)};
        use.command = Command::use_nested;
        use.role = index == 0 ? Role::button_main : Role::button;
        const int width = std::min(
            kit::button_span(
                form.type,
                spec_of(use),
                metrics.button_text,
                metrics.button_pad,
                metrics.button_min_width
            ),
            room / 2
        );
        kit::plain_part(body.items, Role::panel, {x, at, room, row_height}, line_colour);
        const int middle = at + row_height / 2;
        kit::icon_part(
            body.items, {x + pad, middle - icon / 2, icon, icon}, Glyph::folder, dim_colour, false
        );
        const int text_x = x + pad + icon + gap;
        kit::line_part(
            body.items,
            form.type,
            text_x,
            middle,
            x + room - pad - width - gap - text_x,
            nested_name(model.nested[index]),
            {metrics.row_title, true, text_colour, TextStyle::row_title}
        );
        kit::button_part(
            body.items,
            form.type,
            {x + room - pad - width, middle - button_height / 2, width, button_height},
            spec_of(use),
            metrics.button_text
        );
        at += row_height + kit::canvas_pixels(form.type, 6.0f);
    }
    body.height = at > 0 ? at - kit::canvas_pixels(form.type, 6.0f) : 0;

    Action other{};
    other.label = tr("Choose another folder");
    other.control = {ControlKind::choose_folder};
    other.command = Command::pick_game_folder;
    column.bottom.height = button_row_of(form, column.bottom, x, 0, room, {other}, {});
    return column;
}

/// Lays out S2's answer when the chosen folder is the game folder itself.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column already_there_column(const Form& form, int x, int room) {
    Column column;
    column.top.height = add_title(
        form, column.top, x, room, tr("This is already the game folder."), form.model.location
    );
    Action check{};
    check.label = tr("Check it");
    check.control = {ControlKind::check_it};
    check.command = Command::check_in_place;
    check.role = Role::button_main;
    Action other{};
    other.label = tr("Choose another folder");
    other.control = {ControlKind::choose_folder};
    other.command = Command::pick_game_folder;
    column.body.height = button_row_of(form, column.body, x, 0, room, {check, other}, {});
    return column;
}

/// Returns the short-space warning of S3: the need, the free space, what to do, and which
/// switches would make it fit.
///
/// @param model the model
/// @return the warning
std::string short_space_text(const Model& model) {
    std::string text = fill(
        "These files need {need} and this {device} has {free} free. {advice}, or turn off a part "
        "above, then tap Check again.",
        {{"need", size_text(model.need_bytes)},
         {"device", platform_word(model, detail::word_device)},
         {"free", size_text(model.free_bytes)},
         {"advice", platform_word(model, detail::word_free_space_advice)}}
    );
    std::vector<std::string> names;
    for (PartKind kind : model.fitting_off)
        names.push_back(detail::part_kind_name(kind));
    if (!names.empty())
        text = fill(
            "{warning} Turning off {parts} would make them fit.",
            {{"warning", text}, {"parts", detail::join_or(names)}}
        );
    return text;
}

/// Lays out S3: the parts, what is left out, the space and the check, and Copy.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column ready_to_copy_column(const Form& form, int x, int room) {
    const Model& model = form.model;
    const Metrics& metrics = form.metrics;
    const std::string device = platform_word(model, detail::word_device);
    Column column;
    std::string source = fill(
        "From {location}",
        {{"location", model.location.empty() ? tr("the folder you chose") : model.location}}
    );
    const std::string cloud =
        model.remote_bytes > 0 ? fill(
                                     "{size} is in {cloud} only and is downloaded while copying.",
                                     {{"size", size_text(model.remote_bytes)},
                                      {"cloud", platform_word(model, detail::word_cloud_name)}}
                                 )
                               : std::string{};
    // The compact form keeps the rows' room: the cloud line joins the source's.
    if (form.compact && !cloud.empty())
        source = fill("{source} · {cloud}", {{"source", source}, {"cloud", cloud}});
    column.top.height = add_title(form, column.top, x, room, tr("Ready to copy"), source);
    column.body.height = add_rows_panel(form, column.body, x, 0, room);

    Block& bottom = column.bottom;
    const Lettering totals{metrics.totals_text, false, dim_colour, TextStyle::body};
    const Lettering copies{metrics.totals_text, true, text_colour, TextStyle::body};
    const int size = kit::canvas_font(form.type, metrics.totals_text);
    const int line_height = kit::measured_line(form.type.measure, size, true);
    const int gap = kit::canvas_pixels(form.type, metrics.row_gap);
    std::string check;
    Colour check_colour = dim_colour;
    if (model.source_checked) {
        check = tr("Checked: the game can start from these files.");
        check_colour = green_colour;
    } else if (model.source_check_skipped) {
        check = tr("Checked fully once copied.");
    }
    const std::string copy_size = size_text(model.copy_bytes, true);
    // The demo's installer is moved in, not copied, so a demo copying nothing says nothing.
    std::string copies_text;
    if (!model.demo || model.copy_bytes > 0)
        copies_text = model.sizes_unknown ? fill("Copies at least {size}.", {{"size", copy_size}})
                                          : fill("Copies {size}.", {{"size", copy_size}});
    const std::string free_text =
        model.free_known ? fill(
                               "{size} free on this {device}.",
                               {{"size", size_text(model.free_bytes)}, {"device", device}}
                           )
                         : std::string{};
    const int icon = check.empty() || !model.source_checked ? 0 : size;
    const int check_width = check.empty()
                                ? 0
                                : icon + (icon > 0 ? gap : 0) +
                                      kit::measured_width(form.type.measure, check, size, false);
    const int totals_width = kit::measured_width(form.type.measure, copies_text, size, true) +
                             (free_text.empty() || copies_text.empty() ? 0 : gap) +
                             kit::measured_width(form.type.measure, free_text, size, false);
    int y = 0;
    const auto add_check = [&](int middle) {
        if (check.empty())
            return;
        int at = x;
        if (icon > 0) {
            kit::icon_part(
                bottom.items, {at, middle - icon / 2, icon, icon}, Glyph::check, green_colour, false
            );
            at += icon + gap;
        }
        kit::line_part(
            bottom.items,
            form.type,
            at,
            middle,
            x + room - at,
            check,
            {metrics.totals_text, false, check_colour, TextStyle::body}
        );
    };
    const auto add_totals = [&](int middle, bool right) {
        int at = right ? x + room - std::min(room, totals_width) : x;
        const int taken =
            kit::line_part(bottom.items, form.type, at, middle, x + room - at, copies_text, copies);
        if (taken > 0)
            at += taken + gap;
        if (!free_text.empty() && x + room - at > kit::canvas_pixels(form.type, 24.0f))
            kit::line_part(bottom.items, form.type, at, middle, x + room - at, free_text, totals);
    };
    if (check_width + 2 * gap + totals_width <= room) {
        add_check(line_height / 2);
        add_totals(line_height / 2, true);
        y += line_height;
    } else {
        if (!check.empty()) {
            add_check(line_height / 2);
            y += line_height + kit::canvas_pixels(form.type, 4.0f);
        }
        add_totals(y + line_height / 2, false);
        y += line_height;
    }
    if (!form.compact && !cloud.empty()) {
        y += kit::canvas_pixels(form.type, 4.0f);
        y += kit::text_part(bottom.items, form.type, Role::text, {x, y}, room, cloud, totals);
    }
    if (model.space_short) {
        y += kit::canvas_pixels(form.type, metrics.block_gap / 2.0f);
        BannerContent warning{};
        warning.shown = true;
        warning.glyph = Glyph::warning;
        warning.colour = amber_colour;
        warning.text = short_space_text(model);
        y += add_banner(form, bottom, x, y, room, warning);
    }
    y += kit::canvas_pixels(form.type, metrics.block_gap / 2.0f);
    Action other{};
    if (model.demo) {
        other.label = tr("Choose another file");
        other.control = {ControlKind::choose_installer};
        other.command = Command::pick_installer;
    } else {
        other.label = tr("Choose another folder");
        other.control = {ControlKind::choose_folder};
        other.command = Command::pick_game_folder;
    }
    std::vector<Action> right;
    if (model.space_short) {
        Action again{};
        again.label = tr("Check again");
        again.control = {ControlKind::check_space};
        again.command = Command::recheck_space;
        right.push_back(again);
    }
    Action copy{};
    copy.label = model.replace ? tr("Replace game files") : tr("Copy");
    copy.control = {ControlKind::copy};
    copy.command = Command::start_copy;
    copy.role = Role::button_main;
    copy.enabled = !model.space_short;
    right.push_back(copy);
    y += button_row_of(form, bottom, x, y, room, {other}, right);
    bottom.height = y;
    return column;
}

/// Lays out S4: the progress, Stop, the parts ticking and the hint.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column copying_column(const Form& form, int x, int room) {
    const Model& model = form.model;
    const Metrics& metrics = form.metrics;
    Column column;
    Block& top = column.top;
    int y = add_title(
        form,
        top,
        x,
        room,
        tr("Copying"),
        fill(
            "From {location}",
            {{"location", model.location.empty() ? tr("the folder you chose") : model.location}}
        )
    );
    y += add_step_banner(form, top, x, y, room);
    Block inner;
    const int pad = kit::canvas_pixels(form.type, metrics.panel_pad);
    const int inner_x = x + pad;
    const int inner_room = room - 2 * pad;
    const int button_height = kit::canvas_pixels(form.type, min_button_points);
    Action stop{};
    stop.label = tr("Stop");
    stop.control = {ControlKind::stop};
    stop.role = Role::button_danger;
    stop.glyph = Glyph::stop;
    const int stop_width = std::min(
        kit::button_span(form.type, spec_of(stop), metrics.button_text, metrics.button_pad, 0.0f),
        inner_room / 2
    );
    const int middle = y + pad + button_height / 2;
    kit::button_part(
        inner.items,
        form.type,
        {inner_x + inner_room - stop_width, y + pad, stop_width, button_height},
        spec_of(stop),
        metrics.button_text
    );
    const int text_room =
        inner_room - stop_width - kit::canvas_pixels(form.type, metrics.button_gap);
    const int taken = kit::line_part(
        inner.items,
        form.type,
        inner_x,
        middle,
        text_room,
        fill(
            "{done} of {total}",
            {{"done", size_text(model.progress.done_bytes, true)},
             {"total", size_text(model.progress.total_bytes, true)}}
        ),
        {metrics.panel_text, true, text_colour, TextStyle::body}
    );
    const int gap = kit::canvas_pixels(form.type, metrics.row_gap);
    if (taken > 0 && text_room - taken - gap > kit::canvas_pixels(form.type, 24.0f))
        kit::line_part(
            inner.items,
            form.type,
            inner_x + taken + gap,
            middle,
            text_room - taken - gap,
            time_left_text(model.progress.seconds_left),
            {metrics.note_text, false, dim_colour, TextStyle::small}
        );
    int at = y + pad + button_height + kit::canvas_pixels(form.type, 10.0f);
    const Rect track{
        inner_x, at, inner_room, std::max(2, kit::canvas_pixels(form.type, metrics.bar_height))
    };
    kit::plain_part(inner.items, Role::progress_track, track, text_colour);
    float fraction = 0.0f;
    if (model.progress.total_bytes > 0)
        fraction = static_cast<float>(std::min(
            1.0,
            static_cast<double>(model.progress.done_bytes) /
                static_cast<double>(model.progress.total_bytes)
        ));
    kit::Item filled{};
    filled.role = Role::progress_fill;
    filled.rect = track;
    filled.rect.width = static_cast<int>(std::lround(static_cast<float>(track.width) * fraction));
    filled.fraction = fraction;
    filled.colour = green_colour;
    inner.items.push_back(filled);
    at += track.height + kit::canvas_pixels(form.type, 10.0f);
    std::string current = model.progress.current;
    if (model.progress.downloading && !current.empty())
        current = fill(
            "Downloading {file} from {cloud}…",
            {{"file", current}, {"cloud", platform_word(model, detail::word_cloud_name)}}
        );
    if (!current.empty()) {
        const Lettering style{metrics.note_text, false, dim_colour, TextStyle::small};
        const int line_height =
            kit::measured_line(form.type.measure, kit::canvas_font(form.type, style.points), false);
        kit::line_part(
            inner.items, form.type, inner_x, at + line_height / 2, inner_room, current, style
        );
        at += line_height;
    }
    at += pad;
    add_panel(top, {x, y, room, at - y}, inner);
    top.height = at + kit::canvas_pixels(form.type, metrics.block_gap);

    column.body.height = add_rows_panel(form, column.body, x, 0, room);
    column.bottom.height = kit::text_part(
        column.bottom.items,
        form.type,
        Role::text,
        {x, 0},
        room,
        tr("Keep Open Annihilation open until the copy ends. If you leave it, the copy goes on for "
           "a short while, then pauses and continues when you come back."),
        {metrics.note_text, false, dim_colour, TextStyle::small}
    );
    return column;
}

/// Lays out S5 while the engine checks the copy (or unpacks the demo).
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column checking_column(const Form& form, int x, int room) {
    const Model& model = form.model;
    const Metrics& metrics = form.metrics;
    Column column;
    int y = add_title(form, column.top, x, room, tr("Checking"), {});
    y += add_step_banner(form, column.top, x, y, room);
    column.top.height = y;
    Block inner;
    const int pad = kit::canvas_pixels(form.type, metrics.panel_pad);
    int at = pad;
    at += kit::text_part(
        inner.items,
        form.type,
        Role::text,
        {x + pad, at},
        room - 2 * pad,
        model.demo ? fill(
                         "Unpacking the demo's game data ({size})…",
                         {{"size", std::string(detail::demo_data_size_text)}}
                     )
                   : tr("Checking the copied files with the game's own check…"),
        {metrics.panel_text, false, text_colour, TextStyle::body}
    );
    at += kit::canvas_pixels(form.type, 12.0f);
    const int bar = std::max(2, kit::canvas_pixels(form.type, metrics.bar_height));
    kit::plain_part(
        inner.items, Role::progress_busy, {x + pad, at, room - 2 * pad, bar}, text_colour
    );
    at += bar + pad;
    add_panel(column.body, {x, 0, room, at}, inner);
    column.body.height = at;
    return column;
}

/// Lays out S5's Ready to play: what the game will play, the space, the warnings, the backups
/// line, the folder set aside, and Play.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column ready_to_play_column(const Form& form, int x, int room) {
    const Model& model = form.model;
    const Metrics& metrics = form.metrics;
    const std::string device = platform_word(model, detail::word_device);
    Column column;
    Block& top = column.top;
    const int icon = kit::canvas_pixels(form.type, metrics.problem_icon);
    const int gap = kit::canvas_pixels(form.type, 16.0f);
    const int text_x = x + icon + gap;
    const int text_room = room - icon - gap;
    int text_height = kit::text_part(
        top.items,
        form.type,
        Role::title,
        {text_x, 0},
        text_room,
        tr("Ready to play"),
        {metrics.title, true, text_colour, TextStyle::title},
        TextFit::column,
        2
    );
    text_height += kit::canvas_pixels(form.type, metrics.title_gap / 2.0f);
    text_height += kit::text_part(
        top.items,
        form.type,
        Role::text,
        {text_x, text_height},
        text_room,
        model.ready_summary.empty() ? ready_text(model.ready_parts, model.demo)
                                    : model.ready_summary,
        {metrics.subtitle, false, text_colour, TextStyle::subtitle}
    );
    kit::icon_part(top.items, {x, 0, icon, icon}, Glyph::check, green_colour, true);
    int y = std::max(icon, text_height) + kit::canvas_pixels(form.type, metrics.block_gap);
    y += add_step_banner(form, top, x, y, room);
    top.height = y;

    Block& body = column.body;
    Block inner;
    const int pad = kit::canvas_pixels(form.type, metrics.panel_pad);
    const Lettering uses{metrics.note_text + 1.0f, false, text_colour, TextStyle::body};
    const Lettering dim{metrics.note_text, false, dim_colour, TextStyle::small};
    int at = pad;
    at += kit::text_part(
        inner.items,
        form.type,
        Role::text,
        {x + pad, at},
        room - 2 * pad,
        model.free_known ? fill(
                               "Uses {size} on this {device} · {free} free.",
                               {{"size", size_text(model.uses_bytes)},
                                {"device", device},
                                {"free", size_text(model.free_bytes)}}
                           )
                         : fill(
                               "Uses {size} on this {device}.",
                               {{"size", size_text(model.uses_bytes)}, {"device", device}}
                           ),
        uses
    );
    if (!model.backed_up)
        at += kit::canvas_pixels(form.type, 2.0f) +
              kit::text_part(
                  inner.items,
                  form.type,
                  Role::text,
                  {x + pad, at + kit::canvas_pixels(form.type, 2.0f)},
                  room - 2 * pad,
                  fill(
                      "These files are not in this {device}'s backups. Keep "
                      "your original copy.",
                      {{"device", device}}
                  ),
                  dim
              );
    at += pad;
    add_panel(body, {x, 0, room, at}, inner);
    int y_body = at;
    for (const std::string& archive : model.skipped_archives) {
        y_body += kit::canvas_pixels(form.type, metrics.block_gap / 2.0f);
        BannerContent warning{};
        warning.shown = true;
        warning.glyph = Glyph::warning;
        warning.colour = amber_colour;
        warning.text = fill(
            "{file} could not be opened, so the game plays without it: it may be damaged. Copy it "
            "again from the original.",
            {{"file", archive}}
        );
        y_body += add_banner(form, body, x, y_body, room, warning);
    }
    if (model.old_folder_bytes > 0) {
        y_body += kit::canvas_pixels(form.type, metrics.block_gap / 2.0f);
        BannerContent old{};
        old.shown = true;
        old.glyph = Glyph::folder;
        old.colour = amber_colour;
        old.text = fill(
            "The folder that was there before is kept as Total Annihilation (old), {size}.",
            {{"size", size_text(model.old_folder_bytes)}}
        );
        Action remove{};
        remove.label = tr("Remove old folder");
        remove.control = {ControlKind::remove_old};
        remove.glyph = Glyph::trash;
        old.actions.push_back(remove);
        y_body += add_banner(form, body, x, y_body, room, old);
    }
    body.height = y_body;

    Action play{};
    play.role = Role::button_main;
    if (model.management) {
        // Check again in the management state reports here; Done goes back to the list.
        play.label = tr("Done");
        play.control = {ControlKind::back};
        play.command = Command::back;
    } else {
        play.label = tr("Play");
        play.control = {ControlKind::play};
        play.command = Command::play;
        play.glyph = Glyph::play;
    }
    column.bottom.height = button_row_of(form, column.bottom, x, 0, room, {}, {play});
    return column;
}

/// Lays out S6: the problem's mark, title, texts and buttons.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column problem_column(const Form& form, int x, int room) {
    const Metrics& metrics = form.metrics;
    const detail::ProblemContent problem = detail::problem_content(form.model);
    Column column;
    Block& body = column.body;
    const int icon = kit::canvas_pixels(form.type, metrics.problem_icon);
    const int gap = kit::canvas_pixels(form.type, 16.0f);
    const int text_x = x + icon + gap;
    const int text_room = room - icon - gap;
    kit::icon_part(body.items, {x, 0, icon, icon}, problem.glyph, problem.colour, true);
    int y = kit::text_part(
        body.items,
        form.type,
        Role::title,
        {text_x, 0},
        text_room,
        problem.title,
        {metrics.problem_title, true, text_colour, TextStyle::title},
        TextFit::column,
        3
    );
    for (std::size_t index = 0; index < problem.paragraphs.size(); ++index) {
        y += kit::canvas_pixels(form.type, 6.0f);
        y += kit::text_part(
            body.items,
            form.type,
            Role::text,
            {text_x, y},
            text_room,
            problem.paragraphs[index],
            {metrics.problem_text,
             false,
             index == 0 ? text_colour : dim_colour,
             index == 0 ? TextStyle::body : TextStyle::small}
        );
    }
    y += kit::canvas_pixels(form.type, metrics.block_gap);
    y += button_row_of(form, body, text_x, y, text_room, problem.actions, {});
    body.height = std::max(icon, y);
    return column;
}

/// Lays out S8: the summary, Check again and Add files…, the banner, the parts with Remove,
/// and the foot's Replace the game files…, Remove all and Done.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column manage_column(const Form& form, int x, int room) {
    const Model& model = form.model;
    const Metrics& metrics = form.metrics;
    const std::string device = platform_word(model, detail::word_device);
    Column column;
    Block& top = column.top;
    Action check{};
    check.label = tr("Check again");
    check.control = {ControlKind::manage_check};
    check.command = Command::manage_check;
    check.glyph = Glyph::refresh;
    Action add{};
    add.label = tr("Add files…");
    add.control = {ControlKind::manage_add};
    add.role = Role::button_main;
    add.glyph = Glyph::plus;
    add.enabled = !model.pending_replacement;
    const int gap = kit::canvas_pixels(form.type, metrics.button_gap);
    const int check_width =
        kit::button_span(form.type, spec_of(check), metrics.button_text, metrics.button_pad, 0.0f);
    const int add_width =
        kit::button_span(form.type, spec_of(add), metrics.button_text, metrics.button_pad, 0.0f);
    const int buttons = check_width + gap + add_width;
    const std::string title = tr("Game files");
    const std::string subtitle =
        model.free_known ? fill(
                               "{size} on this {device} · {free} free",
                               {{"size", size_text(model.installed_bytes)},
                                {"device", device},
                                {"free", size_text(model.free_bytes)}}
                           )
                         : fill(
                               "{size} on this {device}",
                               {{"size", size_text(model.installed_bytes)}, {"device", device}}
                           );
    const int title_width = std::max(
        kit::measured_width(
            form.type.measure, title, kit::canvas_font(form.type, metrics.title), true
        ),
        kit::measured_width(
            form.type.measure, subtitle, kit::canvas_font(form.type, metrics.subtitle), false
        )
    );
    const bool beside = title_width + 2 * gap + buttons <= room;
    const int text_room = beside ? room - buttons - 2 * gap : room;
    int y = add_title(form, top, x, text_room, title, subtitle);
    const int height = kit::canvas_pixels(form.type, min_button_points);
    if (beside) {
        kit::button_part(
            top.items,
            form.type,
            {x + room - buttons, 0, check_width, height},
            spec_of(check),
            metrics.button_text
        );
        kit::button_part(
            top.items,
            form.type,
            {x + room - add_width, 0, add_width, height},
            spec_of(add),
            metrics.button_text
        );
        y = std::max(y, height + kit::canvas_pixels(form.type, metrics.block_gap));
    } else {
        y += button_row_of(form, top, x, y, room, {check, add}, {});
        y += kit::canvas_pixels(form.type, metrics.block_gap);
    }
    y += add_step_banner(form, top, x, y, room);
    top.height = y;

    column.body.height = add_rows_panel(form, column.body, x, 0, room);

    Action replace{};
    replace.label = tr("Replace the game files…");
    replace.control = {ControlKind::manage_replace};
    replace.command = Command::manage_replace;
    replace.enabled = !model.pending_replacement;
    Action remove_all{};
    remove_all.label = tr("Remove all game files…");
    remove_all.control = {ControlKind::manage_remove_all};
    remove_all.role = Role::button_danger;
    remove_all.enabled = !model.pending_replacement;
    Action done{};
    done.label = tr("Done");
    done.control = {ControlKind::manage_done};
    done.command = Command::done;
    done.role = Role::button_main;
    column.bottom.height =
        button_row_of(form, column.bottom, x, 0, room, {replace, remove_all}, {done});
    return column;
}

/// Lays out the model's step.
///
/// @param form the form laid out
/// @param x the column's left, pixels
/// @param room the column's width, pixels
/// @return the column
Column step_column(const Form& form, int x, int room) {
    switch (form.model.step) {
    case Step::first_run:
        return first_run_column(form, x, room);
    case Step::looking:
        return looking_column(form, x, room);
    case Step::nested_offer:
        return nested_column(form, x, room);
    case Step::already_there:
        return already_there_column(form, x, room);
    case Step::ready_to_copy:
        return ready_to_copy_column(form, x, room);
    case Step::copying:
        return copying_column(form, x, room);
    case Step::checking:
        return checking_column(form, x, room);
    case Step::ready_to_play:
        return ready_to_play_column(form, x, room);
    case Step::problem:
        return problem_column(form, x, room);
    case Step::manage:
        return manage_column(form, x, room);
    }
    return {};
}

/// Places a column's blocks in an area as the kit's scroll column: one after another when
/// they fit; else the body scrolls between the fixed top and bottom; else, when that leaves
/// the body less than least_body_points, the whole column scrolls.
///
/// @param form the form laid out
/// @param[out] items where the placed parts go
/// @param area the area, pixels
/// @param[in,out] column the blocks; their parts are moved out
/// @param scroll_points how far the scrolling part is scrolled, points
/// @return what scrolls: its view, empty when nothing does, and its limit, pixels
kit::ScrollArea place_blocks(
    const Form& form,
    std::vector<kit::Item>& items,
    Rect area,
    Column& column,
    int32_t scroll_points
) {
    const kit::ScrollColumn placed = kit::scroll_column(
        area,
        column.top.height,
        column.body.height,
        column.bottom.height,
        kit::canvas_pixels(form.type, form.metrics.block_gap),
        kit::canvas_pixels(form.type, least_body_points),
        kit::canvas_pixels(form.type, static_cast<float>(scroll_points))
    );
    kit::move_parts(items, column.top.items, placed.top, placed.top_clip);
    kit::move_parts(items, column.body.items, placed.body, placed.body_clip);
    kit::move_parts(items, column.bottom.items, placed.bottom, placed.bottom_clip);
    return placed.scroll;
}

/// Returns how far a scroll area scrolls, in points: its limit in pixels over the scale,
/// rounded up.
///
/// @param scroll the scroll area
/// @param scale canvas pixels per point
/// @return points; 0 when it does not scroll
int32_t scroll_limit_points(const kit::ScrollArea& scroll, float scale) noexcept {
    return scroll.limit > 0
               ? static_cast<int32_t>(std::ceil(static_cast<float>(scroll.limit) / scale))
               : 0;
}

/// Lays out the model's sheet over a backdrop: its title, text, list and buttons, the list
/// scrolling when the sheet does not fit.
///
/// @param form the form laid out
/// @param[in,out] layout the layout
/// @param viewport the canvas
/// @param safe the safe area, pixels
void add_sheet(const Form& form, Layout& layout, const Viewport& viewport, Rect safe) {
    const Metrics& metrics = form.metrics;
    const detail::SheetContent sheet = detail::sheet_content(form.model);
    kit::plain_part(
        layout.list.items, Role::backdrop, {0, 0, viewport.width, viewport.height}, text_colour
    );
    const int margin = kit::canvas_pixels(form.type, sheet_margin_points);
    const int width = std::max(
        0, std::min(kit::canvas_pixels(form.type, metrics.sheet_width), safe.width - 2 * margin)
    );
    const int pad = kit::canvas_pixels(form.type, metrics.sheet_pad);
    const int inner = width - 2 * pad;
    const int x = safe.x + (safe.width - width) / 2;
    const int inner_x = x + pad;
    const int gap = kit::canvas_pixels(form.type, metrics.sheet_gap);
    Column column;
    column.top.height = kit::text_part(
                            column.top.items,
                            form.type,
                            Role::title,
                            {inner_x, 0},
                            inner,
                            sheet.title,
                            {metrics.sheet_title, true, text_colour, TextStyle::title},
                            TextFit::column,
                            3
                        ) +
                        gap;
    Block& body = column.body;
    const Lettering text{metrics.sheet_text, false, dim_colour, TextStyle::body};
    const Lettering strong{metrics.sheet_text, false, text_colour, TextStyle::body};
    const Lettering warning{metrics.sheet_text, false, amber_colour, TextStyle::body};
    const Lettering small{metrics.sheet_text - 1.0f, false, dim_colour, TextStyle::row_detail};
    int y = 0;
    const auto paragraph = [&](std::string_view words, const Lettering& style) {
        const int height =
            kit::text_part(body.items, form.type, Role::text, {inner_x, y}, inner, words, style);
        if (height > 0)
            y += height + gap;
    };
    for (const std::string& words : sheet.paragraphs)
        paragraph(words, text);
    for (const detail::SheetRow& row : sheet.rows) {
        const int size = kit::canvas_font(form.type, strong.points);
        const int line_height = kit::measured_line(form.type.measure, size, false);
        const int size_width = kit::line_part(
            body.items,
            form.type,
            inner_x + inner,
            y + line_height / 2,
            inner / 3,
            row.right,
            text,
            TextFit::tight_right
        );
        kit::line_part(
            body.items,
            form.type,
            inner_x,
            y + line_height / 2,
            inner - size_width - (size_width > 0 ? gap : 0),
            row.text,
            strong
        );
        y += line_height;
        const int small_height =
            kit::measured_line(form.type.measure, kit::canvas_font(form.type, small.points), false);
        kit::line_part(
            body.items, form.type, inner_x, y + small_height / 2, inner, row.detail, small
        );
        y += small_height + kit::canvas_pixels(form.type, 6.0f);
    }
    if (!sheet.rows.empty())
        y += gap - kit::canvas_pixels(form.type, 6.0f);
    for (const std::string& line : sheet.lines)
        paragraph(line, strong);
    for (const std::string& note : sheet.notes)
        paragraph(note, warning);
    for (const std::string& words : sheet.after)
        paragraph(words, text);
    body.height = y > 0 ? y - gap : 0;
    Block& bottom = column.bottom;
    const int button_height = kit::canvas_pixels(form.type, min_button_points);
    int at = 0;
    for (const Action& action : sheet.actions) {
        kit::button_part(
            bottom.items,
            form.type,
            {inner_x, at, inner, button_height},
            spec_of(action),
            metrics.button_text
        );
        at += button_height + gap;
    }
    bottom.height = at > 0 ? at - gap : 0;

    const int between =
        body.height > 0 && bottom.height > 0 ? kit::canvas_pixels(form.type, metrics.block_gap) : 0;
    const int natural = column.top.height + body.height + between + bottom.height;
    const int room = std::max(1, safe.height - 2 * margin - 2 * pad);
    const int height = std::min(natural, room) + 2 * pad;
    const int y_sheet = safe.y + (safe.height - height) / 2;
    kit::plain_part(layout.list.items, Role::sheet, {x, y_sheet, width, height}, line_colour);
    const kit::ScrollArea scroll = place_blocks(
        form,
        layout.list.items,
        {x, y_sheet + pad, width, height - 2 * pad},
        column,
        form.model.scroll_points
    );
    layout.rows = scroll.view;
    layout.scroll_max_points = scroll_limit_points(scroll, form.type.px_per_point);
}

} // namespace

DeviceClass device_class(const Viewport& viewport) noexcept {
    const float scale = viewport.px_per_point > 0.0f ? viewport.px_per_point : 1.0f;
    return oa::ui::touch_hud::classify_device(
        static_cast<int>(std::lround(static_cast<float>(viewport.width) / scale)),
        static_cast<int>(std::lround(static_cast<float>(viewport.height) / scale))
    );
}

Layout lay_out(const Model& model, const Viewport& viewport, const TextMeasureHooks& measure) {
    Layout layout;
    layout.device = device_class(viewport);
    const float scale = viewport.px_per_point > 0.0f ? viewport.px_per_point : 1.0f;
    layout.px_per_point = scale;
    const Rect safe{
        viewport.safe.left,
        viewport.safe.top,
        viewport.width - viewport.safe.left - viewport.safe.right,
        viewport.height - viewport.safe.top - viewport.safe.bottom,
    };
    if (safe.width <= 0 || safe.height <= 0)
        return layout;
    // A tablet window too short for the tablet's sizes (a 640x480 window) takes the phone's
    // compact sizes; its form stays the tablet's.
    const bool compact = layout.device == DeviceClass::phone ||
                         static_cast<float>(std::min(viewport.width, viewport.height)) / scale <
                             compact_short_side_points;
    const Form form{
        model, compact ? phone_metrics : tablet_metrics, kit::Typesetter{measure, scale}, compact
    };
    const Metrics& metrics = form.metrics;

    Block header;
    const int header_bottom = add_header(form, header, safe);
    kit::move_parts(layout.list.items, header.items, 0);

    int side = kit::canvas_pixels(form.type, metrics.side_pad);
    if (metrics.max_content_width > 0.0f)
        side = std::max(
            side, (safe.width - kit::canvas_pixels(form.type, metrics.max_content_width)) / 2
        );
    side = std::min(side, safe.width / 4);
    const int x = safe.x + side;
    const int room = safe.width - 2 * side;
    Column column = step_column(form, x, room);
    const int safe_bottom = safe.y + safe.height;
    const int footer_top = safe_bottom - column.footer.height;
    const int area_top = header_bottom + kit::canvas_pixels(form.type, metrics.content_top);
    const int area_bottom = footer_top - kit::canvas_pixels(form.type, metrics.content_bottom);
    const Rect area{safe.x, area_top, safe.width, std::max(1, area_bottom - area_top)};
    const bool sheet = model.sheet != Sheet::none;
    const kit::ScrollArea scroll =
        place_blocks(form, layout.list.items, area, column, sheet ? 0 : model.scroll_points);
    kit::move_parts(layout.list.items, column.footer.items, footer_top);
    if (sheet) {
        add_sheet(form, layout, viewport, safe);
    } else {
        layout.rows = scroll.view;
        layout.scroll_max_points = scroll_limit_points(scroll, scale);
    }
    kit::list_part_controls(layout.list);
    for (const kit::ControlId id : layout.list.tab_order)
        layout.focus_order.push_back(screen_control(id));
    return layout;
}

Control hit_test(const Layout& layout, Point point, float reach_px) noexcept {
    return screen_control(kit::reach(layout.list, point, reach_px).control);
}

} // namespace oa::ui::game_files
