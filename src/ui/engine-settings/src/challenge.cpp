// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The download check's words, its placement at each size class, and what a
// pointer, a finger and a key do to it. The window is a notice's chrome: the
// class's notice width and height limits, and the Compact metrics for the
// text, the buttons and the footer, as a notice uses them.

#include "oa/ui/engine_settings/challenge.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings {

namespace {

namespace kit = oa::ui::kit;

/// The Compact metrics the text, the buttons and the footer are placed by.
/// The width and the height limits come from the size class.
constexpr const kit::Metrics& metrics = kit::compact_metrics;

/// The caption of CANCEL.
constexpr std::string_view cancel_caption = "Cancel";
/// The caption of OPEN THE CHECK.
constexpr std::string_view open_caption = "Open the check";
/// The caption of TRY AGAIN.
constexpr std::string_view try_again_caption = "Try again";

/// Tells whether the body offers the check on this computer.
///
/// @param window the challenge
/// @return true when a browser can be opened
bool on_this_computer(const ChallengeWindow& window) noexcept {
    return window.can_open_browser;
}

/// Tells whether OPEN THE CHECK is shown.
///
/// @param window the challenge
/// @return true while the check waits and a browser can be opened
bool offers_open(const ChallengeWindow& window) noexcept {
    return window.status == ChallengeWindow::Status::waiting && window.can_open_browser;
}

/// Tells whether TRY AGAIN is shown.
///
/// @param window the challenge
/// @return true once the check has run out
bool offers_retry(const ChallengeWindow& window) noexcept {
    return window.status == ChallengeWindow::Status::expired;
}

/// Returns a text's width in a font, or its estimated width without fonts.
///
/// @param fonts the fonts; null to estimate
/// @param role which font
/// @return the measure
kit::Measure measure_of(const kit::Fonts* fonts, kit::FontRole role) {
    if (fonts == nullptr)
        return kit::estimated_width;
    return [fonts, role](std::string_view text) { return kit::text_width(*fonts, role, text); };
}

/// Returns the width of the text inside the window.
///
/// @param width the window's width
/// @return the text's width, in points
int32_t line_width_in(int32_t width) noexcept {
    return width - 2 * metrics.padding;
}

/// Returns the width inside the window's edge.
///
/// @param width the window's width
/// @return the width, in points
int32_t inner_width_in(int32_t width) noexcept {
    return width - 2 * metrics.edge;
}

/// Returns the height of a window whose text ends at a row.
///
/// @param text_bottom the row under the text
/// @param size_class the window's size class
/// @return the height, from the class's least to its greatest notice height
int32_t height_for(int32_t text_bottom, kit::SizeClass size_class) noexcept {
    const kit::Metrics& sized = kit::metrics_of(size_class);
    return std::clamp(
        text_bottom + metrics.text_bottom_gap + 1 + metrics.footer_height + metrics.edge,
        sized.least_notice_height,
        sized.greatest_notice_height
    );
}

/// Returns the row of the line over the footer.
///
/// @param height the window's height
/// @return the row
int32_t footer_rule_of(int32_t height) noexcept {
    return height - metrics.edge - metrics.footer_height - 1;
}

/// Returns the top row of the footer buttons.
///
/// @param height the window's height
/// @return the row
int32_t button_top_of(int32_t height) noexcept {
    return footer_rule_of(height) + 1 + (metrics.footer_height - metrics.button_height) / 2;
}

/// Tells whether a rectangle sits above the footer with the gap the text keeps.
///
/// @param rect the rectangle
/// @param footer_rule the row of the line over the footer
/// @return true when it fits
bool fits_over(const kit::Rect& rect, int32_t footer_rule) noexcept {
    return rect.y + rect.height <= footer_rule - metrics.text_bottom_gap / 2;
}

/// Returns the title's place in the header.
///
/// @param width the window's width
/// @return the rectangle, in points
kit::Rect title_rect(int32_t width) noexcept {
    const int32_t title_left = kit::notice_mark.x + kit::notice_mark.width + metrics.header_gap;
    return {title_left, metrics.edge, width - metrics.padding - title_left, metrics.header_height};
}

/// One line of the body, placed.
struct BodyLine {
    std::string text{}; ///< the line
    kit::Rect rect{};   ///< where it is drawn
};

/// The parts of the window above the footer.
struct PlacedBody {
    kit::Rect heading{};           ///< the heading
    std::vector<BodyLine> lines{}; ///< the explanation, one line at a time
    kit::Rect code{};              ///< the framed code
    kit::Rect code_text{};         ///< the code's glyphs, inside the frame
    kit::Rect status{};            ///< the line under the code
    bool code_shown{};             ///< the code fits above the footer
    bool status_shown{};           ///< the status fits above the footer
    int32_t height{};              ///< the window's height
    int32_t footer_rule{};         ///< the row of the line over the footer
};

/// Places the heading, the explanation, the code and the status.
///
/// @param text the words
/// @param size_class the window's size class
/// @param fonts the fonts; null estimates each character
/// @return the parts that fit, and the window's height
PlacedBody
place_body(const ChallengeText& text, kit::SizeClass size_class, const kit::Fonts* fonts) {
    const int32_t width = challenge_width(size_class);
    const int32_t line_width = line_width_in(width);
    const kit::Measure small = measure_of(fonts, kit::FontRole::small);
    PlacedBody placed{};
    int32_t row = metrics.text_top;
    placed.heading = {metrics.padding, row, line_width, metrics.small_line};
    row += metrics.small_line + metrics.paragraph_gap;
    for (const std::string& line : kit::wrap(text.body, line_width, small)) {
        placed.lines.push_back(
            BodyLine{line, {metrics.padding, row, line_width, metrics.small_line}}
        );
        row += metrics.small_line;
    }
    row += metrics.paragraph_gap;
    const int32_t code_height = metrics.regular_line + metrics.padding;
    placed.code = {metrics.padding, row, line_width, code_height};
    placed.code_text = kit::inset(placed.code, kit::Insets{4, 4, 4, 4});
    row += code_height + metrics.paragraph_gap;
    placed.status = {metrics.padding, row, line_width, metrics.small_line};
    row += metrics.small_line;
    placed.height = height_for(row, size_class);
    placed.footer_rule = footer_rule_of(placed.height);
    std::vector<BodyLine> fitting;
    for (auto& line : placed.lines)
        if (fits_over(line.rect, placed.footer_rule))
            fitting.push_back(std::move(line));
    placed.lines = std::move(fitting);
    placed.code_shown = fits_over(placed.code, placed.footer_rule);
    placed.status_shown = fits_over(placed.status, placed.footer_rule);
    return placed;
}

/// One button, placed.
struct PlacedButton {
    kit::ControlId id{kit::no_control}; ///< its number
    std::string caption{};              ///< the caption
    std::string name{};                 ///< its automation name
    bool accent{};                      ///< drawn as the accent button
    kit::Rect rect{};                   ///< where it lies
};

/// Returns the buttons, right-aligned, CANCEL at the left of the row.
///
/// @param text the words, whose buttons are CANCEL then the other
/// @param width the window's width
/// @param height the window's height
/// @return the buttons, left to right
std::vector<PlacedButton> place_buttons(const ChallengeText& text, int32_t width, int32_t height) {
    std::vector<PlacedButton> buttons;
    buttons.reserve(text.buttons.size());
    for (const ChallengeButton& button : text.buttons) {
        PlacedButton placed{};
        placed.caption = button.caption;
        if (button.caption == open_caption) {
            placed.id = challenge_open_control;
            placed.name = "challenge.open";
            placed.accent = true;
        } else if (button.caption == try_again_caption) {
            placed.id = challenge_try_again_control;
            placed.name = "challenge.try-again";
            placed.accent = true;
        } else {
            placed.id = challenge_cancel_control;
            placed.name = "challenge.cancel";
        }
        const int32_t button_width = std::max(
            metrics.button_width,
            kit::estimated_width(button.caption) + metrics.prompt_button_padding
        );
        placed.rect = {0, button_top_of(height), button_width, metrics.button_height};
        buttons.push_back(std::move(placed));
    }
    int32_t right = width - metrics.padding;
    for (std::size_t index = buttons.size(); index-- > 0;) {
        buttons[index].rect.x = right - buttons[index].rect.width;
        right -= buttons[index].rect.width + metrics.button_gap;
    }
    return buttons;
}

/// Returns what pressing a control asks.
///
/// @param id the control
/// @return its action, or none when it is not a button
ChallengeAction action_for(kit::ControlId id) noexcept {
    if (id == challenge_open_control)
        return ChallengeAction::open;
    if (id == challenge_try_again_control)
        return ChallengeAction::try_again;
    if (id == challenge_cancel_control)
        return ChallengeAction::cancel;
    return ChallengeAction::none;
}

/// Returns what one pointer event asks.
///
/// @param outcome what the kit's pointer did
/// @return the button's action, redraw, or none
ChallengeAction from_pointer(const kit::PointerOutcome& outcome) noexcept {
    if (outcome.result == kit::PointerResult::activated)
        return action_for(outcome.control);
    if (outcome.result == kit::PointerResult::redraw)
        return ChallengeAction::redraw;
    return ChallengeAction::none;
}

/// Appends a label automation reads and a press does not reach.
///
/// @param[in,out] list the display list
/// @param id its number
/// @param rect where it lies
/// @param name its automation name
/// @param text the words it shows
void add_label(
    kit::DisplayList& list,
    kit::ControlId id,
    const kit::Rect& rect,
    std::string name,
    std::string text
) {
    kit::Control control;
    control.id = id;
    control.rect = rect;
    control.enabled = false;
    control.name = std::move(name);
    control.kind = kit::ControlKind::area;
    control.focusable = false;
    control.text = std::move(text);
    list.controls.push_back(std::move(control));
}

/// Appends a button and, when the keys mark it, its focus ring.
///
/// @param[in,out] list the display list
/// @param button the button
/// @param marked true when the keys mark it
/// @param hovered true when the pointer is over it
/// @param held true when a press is held on it
void add_button(
    kit::DisplayList& list, const PlacedButton& button, bool marked, bool hovered, bool held
) {
    kit::ButtonLook look{};
    look.caption = button.caption;
    look.style = button.accent ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
    look.hovered = hovered;
    look.held = held;
    look.enabled = true;
    kit::Item item;
    item.role = kit::Role::button;
    item.rect = button.rect;
    item.control = button.id;
    item.text = button.caption;
    item.state.hovered = hovered;
    item.state.pressed = held;
    item.state.focused = marked;
    item.look = look;
    list.items.push_back(std::move(item));
    if (marked)
        kit::add_focus_ring(list, button.rect, true);
    kit::Control control;
    control.id = button.id;
    control.rect = button.rect;
    control.name = button.name;
    control.kind = kit::ControlKind::button;
    control.text = button.caption;
    list.controls.push_back(std::move(control));
    list.tab_order.push_back(button.id);
}

} // namespace

ChallengeText challenge_text(const ChallengeWindow& window) {
    ChallengeText text;
    text.title = "Downloads";
    text.heading = "Check that you're a person";
    text.code = window.code;
    if (on_this_computer(window)) {
        text.body = "Before more downloads, " + window.registry_name +
                    " needs to check this isn't an automated program. Open the check on this "
                    "computer, or go to " +
                    window.address + " on any device and enter:";
    } else {
        text.body = "Before more downloads, " + window.registry_name +
                    " needs to check this isn't an automated program. Go to " + window.address +
                    " on another device and enter:";
    }
    if (window.status == ChallengeWindow::Status::expired)
        text.status = "The check ran out. Try again for a new code.";
    else if (window.status == ChallengeWindow::Status::waiting)
        text.status = "Waiting for the check. This updates by itself.";
    text.buttons.push_back(ChallengeButton{std::string(cancel_caption)});
    if (offers_open(window))
        text.buttons.push_back(ChallengeButton{std::string(open_caption)});
    else if (offers_retry(window))
        text.buttons.push_back(ChallengeButton{std::string(try_again_caption)});
    return text;
}

kit::ControlId challenge_focus_control(const ChallengeWindow& window) noexcept {
    if (offers_open(window))
        return challenge_open_control;
    if (offers_retry(window))
        return challenge_try_again_control;
    return challenge_cancel_control;
}

int32_t challenge_width(kit::SizeClass size_class) noexcept {
    return kit::metrics_of(size_class).notice_width;
}

int32_t challenge_height(
    const ChallengeWindow& window, kit::SizeClass size_class, const kit::Fonts* fonts
) {
    return place_body(challenge_text(window), size_class, fonts).height;
}

kit::DisplayList challenge_list(const ChallengeModel& model, const kit::Fonts* fonts) {
    const ChallengeText text = challenge_text(model.window);
    const int32_t width = challenge_width(model.size_class);
    const PlacedBody placed = place_body(text, model.size_class, fonts);
    const std::vector<PlacedButton> buttons = place_buttons(text, width, placed.height);
    kit::DisplayList list;

    kit::Item face;
    face.role = kit::Role::fill;
    face.rect = {0, 0, width, placed.height};
    face.colour = kit::colour::panel;
    list.items.push_back(std::move(face));

    kit::HeaderLook header{};
    header.width = width;
    header.title = text.title;
    header.title_width = title_rect(width).width;
    header.tracking = metrics.heading_tracking;
    kit::Item header_item;
    header_item.role = kit::Role::header;
    header_item.rect = {metrics.edge, metrics.edge, inner_width_in(width), metrics.header_height};
    header_item.text = text.title;
    header_item.look = header;
    list.items.push_back(std::move(header_item));

    kit::Item heading;
    heading.role = kit::Role::heading;
    heading.rect = placed.heading;
    heading.text = text.heading;
    heading.font = kit::FontRole::small;
    heading.look = kit::HeadingLook{text.heading};
    list.items.push_back(std::move(heading));

    for (const BodyLine& line : placed.lines) {
        kit::Item item;
        item.role = kit::Role::text;
        item.rect = line.rect;
        item.text = line.text;
        item.font = kit::FontRole::small;
        item.align = kit::Align::left;
        item.colour = kit::colour::text;
        list.items.push_back(std::move(item));
    }

    if (placed.code_shown) {
        kit::Item frame;
        frame.role = kit::Role::outline;
        frame.rect = placed.code;
        frame.colour = kit::colour::accent;
        list.items.push_back(std::move(frame));
        kit::Item code;
        code.role = kit::Role::text;
        code.rect = placed.code_text;
        code.text = text.code;
        code.font = kit::FontRole::regular;
        code.tracking = challenge_code_tracking;
        code.align = kit::Align::centre;
        code.colour = kit::colour::accent;
        code.control = challenge_code_control;
        list.items.push_back(std::move(code));
        add_label(list, challenge_code_control, placed.code, "challenge.code", text.code);
    }
    if (placed.status_shown) {
        kit::Item status;
        status.role = kit::Role::text;
        status.rect = placed.status;
        status.text = text.status;
        status.font = kit::FontRole::small;
        status.align = kit::Align::left;
        status.colour = kit::colour::text;
        status.control = challenge_status_control;
        list.items.push_back(std::move(status));
        add_label(list, challenge_status_control, placed.status, "challenge.status", text.status);
    }

    kit::Item footer;
    footer.role = kit::Role::footer_band;
    footer.rect = {
        metrics.edge, placed.footer_rule, inner_width_in(width), 1 + metrics.footer_height
    };
    footer.look = kit::FooterBandLook{width, placed.footer_rule};
    list.items.push_back(std::move(footer));

    const kit::Interaction& interaction = model.interaction;
    for (const PlacedButton& button : buttons) {
        const bool marked = interaction.focus_shown && interaction.focused == button.id;
        const bool hovered = interaction.hovered == button.id;
        const bool held = interaction.pressed == button.id && hovered;
        add_button(list, button, marked, hovered, held);
    }

    kit::Item edge;
    edge.role = kit::Role::bevel;
    edge.rect = {0, 0, width, placed.height};
    edge.colour = kit::colour::edge_light;
    list.items.push_back(std::move(edge));
    return list;
}

void challenge_settle(ChallengeModel& model) {
    model.interaction.focused = challenge_focus_control(model.window);
    model.interaction.focus_shown = true;
    model.interaction.hovered = kit::no_control;
    model.interaction.pressed = kit::no_control;
    model.interaction.finger_shift = {};
}

ChallengeAction challenge_key(ChallengeModel& model, kit::Key pressed, const kit::Fonts* fonts) {
    const kit::DisplayList list = challenge_list(model, fonts);
    const kit::KeyOutcome outcome = kit::key(model.interaction, list, pressed);
    if (outcome.result == kit::KeyResult::cancel)
        return ChallengeAction::cancel;
    if (outcome.result == kit::KeyResult::redraw)
        return ChallengeAction::redraw;
    if (outcome.result == kit::KeyResult::to_control)
        return action_for(outcome.control);
    if (outcome.result == kit::KeyResult::accept) {
        const kit::ControlId focused = model.interaction.focused != kit::no_control
                                           ? model.interaction.focused
                                           : challenge_focus_control(model.window);
        return action_for(focused);
    }
    return ChallengeAction::none;
}

ChallengeAction
challenge_pointer_move(ChallengeModel& model, kit::Point at, const kit::Fonts* fonts) {
    const kit::DisplayList list = challenge_list(model, fonts);
    return from_pointer(kit::pointer_move(model.interaction, list, at));
}

ChallengeAction
challenge_pointer_down(ChallengeModel& model, kit::Point at, const kit::Fonts* fonts) {
    const kit::DisplayList list = challenge_list(model, fonts);
    return from_pointer(kit::pointer_down(model.interaction, list, at));
}

ChallengeAction
challenge_pointer_up(ChallengeModel& model, kit::Point at, const kit::Fonts* fonts) {
    const kit::DisplayList list = challenge_list(model, fonts);
    return from_pointer(kit::pointer_up(model.interaction, list, at));
}

ChallengeAction challenge_finger_down(
    ChallengeModel& model, kit::Point at, int32_t reach, const kit::Fonts* fonts
) {
    const kit::DisplayList list = challenge_list(model, fonts);
    return from_pointer(kit::finger_down(model.interaction, list, at, reach));
}

void draw_challenge(const kit::Canvas& canvas, const ChallengeModel& model) {
    kit::DisplayList list = challenge_list(model, canvas.fonts);
    std::string code;
    kit::Rect code_box{};
    for (kit::Item& item : list.items) {
        if (item.role == kit::Role::text && item.control == challenge_code_control) {
            code = std::move(item.text);
            item.text.clear();
            code_box = item.rect;
        }
    }
    kit::paint(canvas, list);
    if (code.empty() || canvas.fonts == nullptr)
        return;
    // Through the canvas's text, so that it draws on either form.
    kit::box_text(
        canvas,
        kit::FontRole::regular,
        code,
        code_box,
        kit::Align::centre,
        kit::colour::accent,
        challenge_code_tracking
    );
}

} // namespace oa::ui::engine_settings
