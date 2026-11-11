// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Game files screen's presses and keys (game_files.hpp): what a press or
// a key does to the sheets, the switches, the focus and the scroll, and what
// it asks the app to do.
#include "game_files_internal.hpp"

#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <algorithm>
#include <cmath>

namespace oa::ui::game_files {

namespace {

namespace kit = oa::ui::kit;

/// How far an arrow key scrolls, in points.
constexpr float arrow_scroll_points = 40.0f;
/// The share of the rows' height a page key scrolls.
constexpr float page_scroll_share = 0.9f;
/// The bits of a kit control number below a control's kind, which hold its index.
constexpr int control_index_bits = 16;
/// The index's bits of a kit control number.
constexpr uint32_t control_index_mask = (uint32_t{1} << control_index_bits) - 1U;

/// Returns the control a press can reach (on a part after the last backdrop).
///
/// @param layout the layout last drawn
/// @param control the control
/// @return the kit's control; null when the layout has no such control
const kit::Control* find_control(const Layout& layout, Control control) noexcept {
    if (control.kind == ControlKind::none)
        return nullptr;
    return kit::control_of(layout.list, control_id(control));
}

/// Returns the look-only outcome.
///
/// @return Command::redraw
Outcome redraw() noexcept {
    return {Command::redraw, 0};
}

/// Opens a sheet over the step, its list at the top.
///
/// @param[in,out] model the model
/// @param sheet the sheet
/// @param part the row it is about
void open_sheet(Model& model, Sheet sheet, uint16_t part = 0) noexcept {
    model.sheet = sheet;
    model.sheet_part = part;
    model.scroll_points = 0;
}

/// Closes the sheet; the step's rows start again at the top.
///
/// @param[in,out] model the model
void close_sheet(Model& model) noexcept {
    model.sheet = Sheet::none;
    model.scroll_points = 0;
}

/// Acts on a sheet's button.
///
/// @param[in,out] model the model
/// @param control the button
/// @return what the app must do
Outcome act_on_sheet(Model& model, Control control) {
    if (control.kind != ControlKind::sheet_option)
        return {};
    const detail::SheetContent sheet = detail::sheet_content(model);
    if (control.index >= sheet.actions.size())
        return {};
    const detail::Action& action = sheet.actions[control.index];
    if (!action.enabled)
        return {};
    Outcome outcome{action.command, 0};
    if (action.command == Command::manage_remove || action.command == Command::remove_demo_data)
        outcome.index = model.sheet_part;
    close_sheet(model);
    if (outcome.command == Command::none)
        return redraw();
    return outcome;
}

/// Acts on a control as a finished press does.
///
/// @param[in,out] model the model
/// @param control the control
/// @return what the app must do
Outcome act(Model& model, Control control) {
    if (model.sheet != Sheet::none)
        return act_on_sheet(model, control);
    switch (control.kind) {
    case ControlKind::none:
    case ControlKind::sheet_option:
        return {};
    case ControlKind::language:
        if (model.management)
            return {};
        return {Command::open_language, 0};
    case ControlKind::choose_folder:
        return {Command::pick_game_folder, 0};
    case ControlKind::i_have_copied:
        return {Command::check_copied, 0};
    case ControlKind::choose_installer:
        return {Command::pick_installer, 0};
    case ControlKind::banner_action:
    case ControlKind::banner_discard: {
        const detail::BannerContent banner = detail::banner_content(model);
        for (const detail::Action& action : banner.actions)
            if (action.control == control)
                return {action.command, 0};
        return {};
    }
    case ControlKind::cancel:
        return {Command::cancel_scan, 0};
    case ControlKind::nested_choice:
        if (control.index >= model.nested.size())
            return {};
        return {Command::use_nested, control.index};
    case ControlKind::check_it:
        return {Command::check_in_place, 0};
    case ControlKind::part_switch:
        if (control.index >= model.parts.size() || !model.parts[control.index].has_switch)
            return {};
        model.parts[control.index].on = !model.parts[control.index].on;
        return redraw();
    case ControlKind::part_why:
        if (control.index >= model.parts.size())
            return {};
        open_sheet(model, Sheet::mod_errors, control.index);
        return redraw();
    case ControlKind::show_left_out:
        open_sheet(model, Sheet::left_out_list);
        return redraw();
    case ControlKind::copy:
        if (model.space_short)
            return {};
        if (model.replace) {
            open_sheet(model, Sheet::replace_confirm);
            return redraw();
        }
        return {Command::start_copy, 0};
    case ControlKind::check_space:
        return {Command::recheck_space, 0};
    case ControlKind::stop:
        open_sheet(model, Sheet::stop);
        return redraw();
    case ControlKind::play:
        return {Command::play, 0};
    case ControlKind::remove_old:
        open_sheet(model, Sheet::remove_old_confirm);
        return redraw();
    case ControlKind::problem_action: {
        const detail::ProblemContent problem = detail::problem_content(model);
        if (control.index >= problem.actions.size())
            return {};
        return {problem.actions[control.index].command, control.index};
    }
    case ControlKind::back:
        return {Command::back, 0};
    case ControlKind::manage_check:
        return {Command::manage_check, 0};
    case ControlKind::manage_add:
        if (model.pending_replacement)
            return {};
        open_sheet(model, Sheet::add_files);
        return redraw();
    case ControlKind::manage_remove:
        if (control.index >= model.parts.size() || model.pending_replacement)
            return {};
        open_sheet(model, Sheet::remove_part_confirm, control.index);
        return redraw();
    case ControlKind::manage_replace:
        if (model.pending_replacement)
            return {};
        return {Command::manage_replace, 0};
    case ControlKind::manage_remove_all:
        if (model.pending_replacement)
            return {};
        open_sheet(model, Sheet::remove_all_confirm);
        return redraw();
    case ControlKind::manage_cancel_pending:
        return {Command::manage_cancel_pending, 0};
    case ControlKind::manage_done:
        if (detail::change_waits(model)) {
            open_sheet(model, Sheet::scheduled_note);
            return redraw();
        }
        return {Command::done, 0};
    }
    return {};
}

/// Scrolls the rows so that a control's part shows whole (or its top, when taller).
///
/// @param[in,out] model the model
/// @param layout the layout last drawn
/// @param control the control
void scroll_into_view(Model& model, const Layout& layout, Control control) {
    const kit::Control* found = find_control(layout, control);
    if (found == nullptr || layout.rows.height <= 0 || found->clip.height <= 0)
        return;
    const Rect rows = layout.rows;
    const Rect box = found->rect;
    int delta = 0;
    if (box.y < rows.y || box.height > rows.height)
        delta = box.y - rows.y;
    else if (box.y + box.height > rows.y + rows.height)
        delta = box.y + box.height - (rows.y + rows.height);
    if (delta == 0)
        return;
    const float scale = layout.px_per_point > 0.0f ? layout.px_per_point : 1.0f;
    const float points = static_cast<float>(delta) / scale;
    const auto step = static_cast<int32_t>(delta > 0 ? std::ceil(points) : std::floor(points));
    model.scroll_points = std::clamp(
        model.scroll_points + step, int32_t{0}, std::max(int32_t{0}, layout.scroll_max_points)
    );
}

/// Returns the main button a Return presses when no control has the focus.
///
/// @param layout the layout last drawn
/// @return its control; none when there is none
Control main_control(const Layout& layout) noexcept {
    const std::vector<kit::Item>& parts = layout.list.items;
    for (std::size_t index = kit::first_live_part(layout.list); index < parts.size(); ++index) {
        const kit::Item& part = parts[index];
        if (part.role == kit::Role::button_main && !part.state.disabled &&
            part.control != kit::no_control)
            return screen_control(part.control);
    }
    return {};
}

/// What Esc does on a step with no sheet.
///
/// @param[in,out] model the model
/// @return what the app must do
Outcome escape_step(Model& model) {
    switch (model.step) {
    case Step::first_run:
        return model.management ? Outcome{Command::back, 0} : Outcome{};
    case Step::looking:
        return {Command::cancel_scan, 0};
    case Step::nested_offer:
    case Step::already_there:
    case Step::ready_to_copy:
    case Step::problem:
        return {Command::back, 0};
    case Step::copying:
        open_sheet(model, Sheet::stop);
        return redraw();
    case Step::checking:
        return {};
    case Step::ready_to_play:
        return model.management ? Outcome{Command::back, 0} : Outcome{};
    case Step::manage:
        return act(model, {ControlKind::manage_done});
    }
    return {};
}

} // namespace

kit::ControlId control_id(Control control) noexcept {
    if (control.kind == ControlKind::none)
        return kit::no_control;
    return static_cast<kit::ControlId>(
        (static_cast<uint32_t>(control.kind) << control_index_bits) | control.index
    );
}

Control screen_control(kit::ControlId id) noexcept {
    if (id < 0)
        return {};
    const uint32_t kind = static_cast<uint32_t>(id) >> control_index_bits;
    if (kind == 0 || kind > static_cast<uint32_t>(ControlKind::manage_done))
        return {};
    return {
        static_cast<ControlKind>(kind),
        static_cast<uint16_t>(static_cast<uint32_t>(id) & control_index_mask)
    };
}

Outcome press_down(Model& model, Interaction& interaction, const Layout& layout, Control control) {
    static_cast<void>(model);
    const kit::Control* found = find_control(layout, control);
    if (found == nullptr || !found->enabled) {
        const bool was_pressed = interaction.pressed != kit::no_control;
        interaction.pressed = kit::no_control;
        return was_pressed ? redraw() : Outcome{};
    }
    interaction.pressed = control_id(control);
    return redraw();
}

Outcome press_up(Model& model, Interaction& interaction, const Layout& layout, Control control) {
    const Control pressed = screen_control(interaction.pressed);
    interaction.pressed = kit::no_control;
    const Outcome look = pressed.kind != ControlKind::none ? redraw() : Outcome{};
    if (control.kind == ControlKind::none)
        return look;
    if (pressed.kind != ControlKind::none && !(pressed == control))
        return look;
    const kit::Control* found = find_control(layout, control);
    if (found == nullptr || !found->enabled)
        return look;
    interaction.focused = control_id(control);
    const Outcome outcome = act(model, control);
    if (outcome.command == Command::none)
        return look;
    return outcome;
}

Outcome key(Model& model, Interaction& interaction, const Layout& layout, Key key) {
    // The first stop takes the keys from the first key, with the ring hidden,
    // unless the screen already opened (and may have asked for the ring).
    kit::focus_opened(interaction, layout.list, false);
    if (key != Key::escape)
        kit::show_ring_for_key(interaction);
    switch (key) {
    case Key::tab:
    case Key::back_tab: {
        const std::vector<kit::ControlId>& order = layout.list.tab_order;
        if (order.empty())
            return {};
        // Tab always moves. A control that is not in the order lands on the
        // first stop going forward and the last going back.
        interaction.focused =
            kit::next_in_tab_order(layout.list, interaction.focused, key == Key::tab);
        interaction.focus_shown = true;
        interaction.pointer_hid_ring = false;
        scroll_into_view(model, layout, screen_control(interaction.focused));
        return redraw();
    }
    case Key::enter:
    case Key::space: {
        Control target{};
        const std::vector<kit::ControlId>& order = layout.list.tab_order;
        const bool focus_live =
            interaction.focus_shown &&
            std::find(order.begin(), order.end(), interaction.focused) != order.end();
        if (focus_live)
            target = screen_control(interaction.focused);
        else if (key == Key::enter)
            target = main_control(layout);
        if (target.kind == ControlKind::none)
            return {};
        const kit::Control* found = find_control(layout, target);
        if (found == nullptr || !found->enabled)
            return {};
        interaction.pressed = kit::no_control;
        const Outcome outcome = act(model, target);
        if (outcome.command == Command::none)
            return {};
        // Return on the control the ring shows presses it and hides the ring.
        if (key == Key::enter && focus_live)
            interaction.focus_shown = false;
        return outcome;
    }
    case Key::escape:
        interaction.pressed = kit::no_control;
        if (model.sheet != Sheet::none) {
            close_sheet(model);
            return redraw();
        }
        return escape_step(model);
    case Key::up:
        return scroll(model, layout, -arrow_scroll_points);
    case Key::down:
        return scroll(model, layout, arrow_scroll_points);
    case Key::page_up:
    case Key::page_down: {
        const float scale = layout.px_per_point > 0.0f ? layout.px_per_point : 1.0f;
        const float page = std::max(
            arrow_scroll_points, static_cast<float>(layout.rows.height) / scale * page_scroll_share
        );
        return scroll(model, layout, key == Key::page_up ? -page : page);
    }
    }
    return {};
}

Outcome scroll(Model& model, const Layout& layout, float points) {
    const int32_t most = std::max(int32_t{0}, layout.scroll_max_points);
    const auto wanted = static_cast<int64_t>(model.scroll_points) +
                        static_cast<int64_t>(std::lround(static_cast<double>(points)));
    const auto next = static_cast<int32_t>(std::clamp<int64_t>(wanted, 0, most));
    if (next == model.scroll_points)
        return {};
    model.scroll_points = next;
    return redraw();
}

} // namespace oa::ui::game_files
