// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's input: the pointer and a finger, the keys, typed
// text, the wheel, and the controller's own buttons. What it knows of the
// screen it reads from the display list the screen shows. A drop, a switch
// and a move go through the model, which keeps the order.
#include "oa/ui/mutator_picker/screen.hpp"

#include "internal.hpp"
#include "screen_controls.hpp"

#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::ui::mutator_picker {
namespace {

/// How far a mouse moves, in points, before a grip becomes a drag.
constexpr int32_t mouse_drag_points = 3;
/// How close to the list's edge, in points, the pointer scrolls while dragging.
constexpr int32_t scroll_edge = 16;
/// A row's height when the list shows none, in points.
constexpr int32_t fallback_row_height = 34;

using Hold = ScreenState::PointerHold;
using Bar = ScreenState::Bar;

/// A pane's scroll, as the display list shows it.
struct PaneScroll {
    kit::Rect port{}; ///< what the pane is seen through
    int32_t offset{}; ///< how far it is scrolled, in points
    int32_t limit{};  ///< the greatest offset, in points
    int32_t step{};   ///< how far one row moves it, in points
    bool ok{};        ///< the pane is on the screen
};

/// Returns |value| without calling the C library.
///
/// @param value the number
/// @return its distance from 0
int32_t absolute(int32_t value) {
    return value < 0 ? -value : value;
}

/// Returns a size as a point count, capped so the cast is exact.
///
/// @param value the size
/// @return the count
int32_t as_int(std::size_t value) {
    constexpr auto most = static_cast<std::size_t>(INT32_MAX);
    if (value > most)
        return INT32_MAX;
    return static_cast<int32_t>(value);
}

/// Returns a result that only redraws.
///
/// @return the result
ScreenResult redrawn() {
    ScreenResult answered;
    answered.redraw = true;
    return answered;
}

/// Tells whether two rectangles are the same place.
///
/// @param left one rectangle
/// @param right the other
/// @return true when they match
bool same_rect(const kit::Rect& left, const kit::Rect& right) {
    return left.x == right.x && left.y == right.y && left.width == right.width &&
           left.height == right.height;
}

/// Tells whether the pointer is inside the window. The window is the first
/// thing the layout draws.
///
/// @param list the display list
/// @param at the pointer
/// @return true when the pointer is inside the window
bool inside_window(const kit::DisplayList& list, kit::Point at) {
    if (list.items.empty())
        return false;
    return kit::contains(list.items.front().rect, at);
}

/// Returns the list's rows. The vector is named so nothing points into it
/// after the call that made it.
///
/// @param picker the picker
/// @return the rows
std::vector<Row> shown_rows(const Picker& picker) {
    return rows(picker);
}

/// Returns a row control's place, or a negative number when `id` is not one.
///
/// @param id the control
/// @param first the first id of that band
/// @param next the first id of the next band
/// @return the place
int32_t band_place(kit::ControlId id, kit::ControlId first, kit::ControlId next) {
    if (id < first || id >= next)
        return -1;
    return id - first;
}

/// Returns the mutator id of a row, a grip or a switch.
///
/// @param picker the picker
/// @param id the control
/// @return the mutator's id; empty when the control is not one of those
std::string row_id_of(const Picker& picker, kit::ControlId id) {
    int32_t place = band_place(id, controls::first_row, controls::first_grip);
    if (place < 0)
        place = band_place(id, controls::first_grip, controls::first_switch);
    if (place < 0)
        place = band_place(id, controls::first_switch, controls::first_option);
    if (place < 0)
        return {};
    const std::vector<Row> shown = shown_rows(picker);
    const auto index = static_cast<std::size_t>(place);
    if (index >= shown.size())
        return {};
    return shown[index].id;
}

/// Tells whether a control is one item of the open menu.
///
/// @param id the control
/// @return true for an item
bool menu_item_control(kit::ControlId id) {
    return id >= controls::first_menu_item;
}

/// Returns the height of a row the list shows.
///
/// @param list the display list
/// @return the height, in points
int32_t row_height_of(const kit::DisplayList& list) {
    for (const kit::Control& control : list.controls) {
        if (band_place(control.id, controls::first_row, controls::first_grip) >= 0 &&
            control.rect.height > 0)
            return control.rect.height;
    }
    return fallback_row_height;
}

/// Grows an extent to cover one rectangle.
///
/// @param[in,out] top the top so far; unchanged when `any` is false and the
///     rectangle is the first
/// @param[in,out] bottom the bottom so far
/// @param[in,out] any true once a rectangle has been seen
/// @param rect the rectangle
void grow_extent(int32_t& top, int32_t& bottom, bool& any, const kit::Rect& rect) {
    const int32_t rect_bottom = rect.y + rect.height;
    if (!any) {
        top = rect.y;
        bottom = rect_bottom;
        any = true;
        return;
    }
    if (rect.y < top)
        top = rect.y;
    if (rect_bottom > bottom)
        bottom = rect_bottom;
}

/// Returns how far a pane is scrolled, from the parts clipped to its port.
///
/// @param list the display list
/// @param port the pane
/// @param step how far one step moves, in points
/// @return the scroll
PaneScroll scroll_geometry(const kit::DisplayList& list, const kit::Rect& port, int32_t step) {
    PaneScroll scroll;
    scroll.port = port;
    scroll.step = step > 0 ? step : fallback_row_height;
    int32_t top = 0;
    int32_t bottom = 0;
    bool any = false;
    for (const kit::Item& item : list.items) {
        if (same_rect(item.clip, port))
            grow_extent(top, bottom, any, item.rect);
    }
    for (const kit::Control& control : list.controls) {
        if (same_rect(control.clip, port))
            grow_extent(top, bottom, any, control.rect);
    }
    if (!any)
        return scroll;
    scroll.ok = true;
    scroll.offset = std::max(port.y - top, int32_t{0});
    scroll.limit = kit::ScrollArea::limit_for(bottom - top, port.height);
    if (scroll.offset > scroll.limit)
        scroll.offset = scroll.limit;
    return scroll;
}

/// Returns the list's scroll.
///
/// @param list the display list
/// @return the scroll; not ok while the list is not shown
PaneScroll list_scroll_of(const kit::DisplayList& list) {
    const kit::Control* pane = kit::control_of(list, controls::list);
    if (pane == nullptr)
        return {};
    return scroll_geometry(list, pane->rect, row_height_of(list));
}

/// Returns the details' port: the clip the details share.
///
/// @param list the display list
/// @return the port; empty while the details show nothing that scrolls
kit::Rect details_port(const kit::DisplayList& list) {
    for (const kit::Control& control : list.controls) {
        if (control.group != controls::details_group ||
            control.kind == kit::ControlKind::scroll_bar)
            continue;
        if (control.clip.width > 0 && control.clip.height > 0)
            return control.clip;
    }
    return {};
}

/// Returns the details' scroll.
///
/// @param list the display list
/// @return the scroll; not ok while no details show
PaneScroll details_scroll_of(const kit::DisplayList& list) {
    const kit::Rect port = details_port(list);
    if (port.height <= 0)
        return {};
    return scroll_geometry(list, port, kit::compact_metrics.list_row_height);
}

/// Returns the offset nearest another that shows a part of a pane whole.
///
/// @param offset the offset now
/// @param top the part's top when the pane is not scrolled, from the port's top
/// @param height the part's height
/// @param port_height the port's height
/// @param limit the greatest offset
/// @return the offset
int32_t
offset_showing(int32_t offset, int32_t top, int32_t height, int32_t port_height, int32_t limit) {
    const int32_t lowest = top + height - port_height;
    return std::clamp(
        std::min(std::max(offset, lowest), top), int32_t{0}, std::max(limit, int32_t{0})
    );
}

/// Scrolls the list so the selected row shows whole.
///
/// @param picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
void show_selected(const Picker& picker, ScreenState& state, const kit::DisplayList& list) {
    if (!picker.selected)
        return;
    const PaneScroll scroll = list_scroll_of(list);
    if (!scroll.ok)
        return;
    const std::vector<Row> shown = shown_rows(picker);
    std::optional<std::size_t> place;
    for (std::size_t index = 0; index < shown.size(); ++index) {
        if (shown[index].id == *picker.selected)
            place = index;
    }
    if (!place)
        return;
    const kit::Control* row =
        kit::control_of(list, controls::first_row + static_cast<kit::ControlId>(as_int(*place)));
    if (row == nullptr)
        return;
    const int32_t top = row->rect.y + scroll.offset - scroll.port.y;
    state.list_scroll =
        offset_showing(scroll.offset, top, row->rect.height, scroll.port.height, scroll.limit);
}

/// Scrolls the details so a control in them shows whole.
///
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the control
void show_in_details(ScreenState& state, const kit::DisplayList& list, kit::ControlId id) {
    const kit::Control* control = kit::control_of(list, id);
    if (control == nullptr || control->group != controls::details_group)
        return;
    const PaneScroll scroll = details_scroll_of(list);
    if (!scroll.ok)
        return;
    const int32_t top = control->rect.y + scroll.offset - scroll.port.y;
    state.details_scroll =
        offset_showing(scroll.offset, top, control->rect.height, scroll.port.height, scroll.limit);
}

/// Selects a row. A new selection closes a menu and shows the details from
/// their top. The keys scroll the list until the row shows. A pointer press
/// does not, so the control under the pointer stays where it was.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the mutator's id
/// @param reveal true when the list scrolls the row into view
void select_row(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, std::string id, bool reveal
) {
    const bool same = picker.selected && *picker.selected == id;
    picker.selected = std::move(id);
    if (same)
        return;
    state.details_scroll = 0;
    state.menu = Menu::none;
    if (reveal)
        show_selected(picker, state, list);
}

/// Ends a drag, a finger scroll, a slider hold and a scroll-bar hold.
/// Changes nothing in the model.
///
/// @param[in,out] state the screen's state
void end_hold(ScreenState& state) {
    state.drag.reset();
    state.hold = {};
    state.bar = Bar::none;
    state.interaction.pressed = kit::no_control;
    state.interaction.finger_shift = {};
}

/// Tells whether a press is holding the pointer.
///
/// @param state the screen's state
/// @return true while a drag, a finger, a slider or a scroll bar is held
bool holding(const ScreenState& state) {
    return state.drag.has_value() || state.hold.kind != Hold::Kind::none || state.bar != Bar::none;
}

/// Returns the On ids that were not on before and are not installed.
///
/// @param picker the picker
/// @param before the On ids before the change, top to bottom
/// @return the new ones, in the On list's order
std::vector<std::string>
gained_uninstalled(const Picker& picker, const std::vector<std::string>& before) {
    std::vector<std::string> gained;
    for (const std::string& id : detail::on_ids(picker)) {
        if (detail::listed(before, id))
            continue;
        const Mutator* mutator = detail::find_mutator(picker, id);
        if (mutator != nullptr && !mutator->installed)
            gained.push_back(id);
    }
    return gained;
}

/// The question, or the mutators a switch just brought that are not installed.
///
/// @param picker the picker
/// @param before the On ids before the change
/// @param asked the question, when switching off asked one
/// @return what the host is to do
ScreenResult switched(
    const Picker& picker,
    const std::vector<std::string>& before,
    const std::optional<TurnOff>& asked
) {
    ScreenResult answered = redrawn();
    if (asked) {
        answered.action = PickerAction::ask_turn_off;
        answered.ids.push_back(asked->id);
        for (const std::string& dependant : asked->dependants)
            answered.ids.push_back(dependant);
        return answered;
    }
    answered.ids = gained_uninstalled(picker, before);
    if (!answered.ids.empty())
        answered.action = PickerAction::get;
    return answered;
}

/// Flips a row's switch through the model.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @return what the host is to do
ScreenResult flip_switch(Picker& picker, std::string_view id) {
    const std::vector<std::string> before = detail::on_ids(picker);
    const std::vector<Row> shown = shown_rows(picker);
    bool on = false;
    bool found = false;
    for (const Row& row : shown) {
        if (row.id != id)
            continue;
        on = row.on;
        found = true;
    }
    if (!found)
        return redrawn();
    if (on)
        return switched(picker, before, switch_off(picker, id));
    std::ignore = switch_on(picker, id);
    return switched(picker, before, std::nullopt);
}

/// Sets an option when the value changes, and answers check only then.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
/// @param option the option's id
/// @param value the value
/// @return what the host is to do
ScreenResult assign_option(Picker& picker, std::string id, std::string option, std::string value) {
    std::string before;
    if (const std::optional<std::size_t> at = detail::on_index(picker, id))
        before = detail::chosen_value(picker.on[*at], option);
    const bool kept = set_option(picker, id, option, value);
    std::string after;
    if (const std::optional<std::size_t> at = detail::on_index(picker, id))
        after = detail::chosen_value(picker.on[*at], option);
    ScreenResult answered = redrawn();
    if (kept && before != after) {
        answered.action = PickerAction::check;
        answered.ids.push_back(std::move(id));
    }
    return answered;
}

/// The selected mutator's option for a control.
///
/// @param picker the picker
/// @param id the control
/// @return the option; null when the control is not one, or the mutator is off
const OptionSpec* option_of(const Picker& picker, kit::ControlId id) {
    const int32_t place = band_place(id, controls::first_option, controls::first_tag_pill);
    if (place < 0 || !picker.selected)
        return nullptr;
    const Mutator* mutator = detail::find_mutator(picker, *picker.selected);
    if (mutator == nullptr || !detail::on_index(picker, mutator->id))
        return nullptr;
    const auto index = static_cast<std::size_t>(place);
    if (index >= mutator->options.size())
        return nullptr;
    return &mutator->options[index];
}

/// Returns the level strip's item.
///
/// @param list the display list
/// @param id the control
/// @return the item; null when the list draws no strip for it
const kit::Item* levels_item(const kit::DisplayList& list, kit::ControlId id) {
    for (const kit::Item& item : list.items) {
        if (item.control == id && std::holds_alternative<kit::LevelsLook>(item.look))
            return &item;
    }
    return nullptr;
}

/// Returns the slider's item.
///
/// @param list the display list
/// @param id the control
/// @return the item; null when the list draws no slider for it
const kit::Item* slider_item(const kit::DisplayList& list, kit::ControlId id) {
    for (const kit::Item& item : list.items) {
        if (item.control == id && std::holds_alternative<kit::SliderLook>(item.look))
            return &item;
    }
    return nullptr;
}

/// Sets a level strip's level. A level past the ones offered is ignored.
///
/// @param[in,out] picker the picker
/// @param list the display list
/// @param id the control
/// @param level the level
/// @return what the host is to do
ScreenResult
set_level(Picker& picker, const kit::DisplayList& list, kit::ControlId id, std::size_t level) {
    if (id == controls::clash) {
        if (!picker.selected)
            return redrawn();
        const Clash mode = level == 0 ? Clash::add : Clash::replace;
        std::optional<Clash> before;
        if (const std::optional<std::size_t> at = detail::on_index(picker, *picker.selected))
            before = picker.on[*at].clash;
        const bool kept = set_clash(picker, *picker.selected, mode);
        std::optional<Clash> after = before;
        if (const std::optional<std::size_t> at = detail::on_index(picker, *picker.selected))
            after = picker.on[*at].clash;
        ScreenResult answered = redrawn();
        if (kept && before != after) {
            answered.action = PickerAction::check;
            answered.ids.push_back(*picker.selected);
        }
        return answered;
    }
    const OptionSpec* spec = option_of(picker, id);
    const kit::Item* item = levels_item(list, id);
    if (spec == nullptr || item == nullptr || !picker.selected)
        return redrawn();
    const auto* look = std::get_if<kit::LevelsLook>(&item->look);
    if (look == nullptr || level >= look->offered || level >= spec->values.size())
        return redrawn();
    return assign_option(picker, *picker.selected, spec->id, spec->values[level]);
}

/// Steps a level strip by one level, staying inside the ones offered.
///
/// @param[in,out] picker the picker
/// @param list the display list
/// @param id the control
/// @param toward_right true for the level on the right
/// @return what the host is to do
ScreenResult
step_levels(Picker& picker, const kit::DisplayList& list, kit::ControlId id, bool toward_right) {
    const kit::Item* item = levels_item(list, id);
    if (item == nullptr)
        return {};
    const auto* look = std::get_if<kit::LevelsLook>(&item->look);
    if (look == nullptr || look->offered == 0)
        return {};
    std::size_t level = look->chosen;
    if (toward_right) {
        if (level + 1 >= look->offered)
            return {};
        ++level;
    } else if (level == 0) {
        return {};
    } else {
        --level;
    }
    return set_level(picker, list, id, level);
}

/// Sets a slider's stop.
///
/// @param[in,out] picker the picker
/// @param list the display list
/// @param id the control
/// @param stop the stop
/// @return what the host is to do
ScreenResult
set_stop(Picker& picker, const kit::DisplayList& list, kit::ControlId id, int32_t stop) {
    const OptionSpec* spec = option_of(picker, id);
    const kit::Item* item = slider_item(list, id);
    if (spec == nullptr || item == nullptr || !picker.selected)
        return redrawn();
    const auto* look = std::get_if<kit::SliderLook>(&item->look);
    if (look == nullptr || look->stops < 2)
        return redrawn();
    const std::vector<std::string> stops = number_values(*spec);
    if (stops.size() < 2)
        return redrawn();
    const int32_t last = as_int(stops.size()) - 1;
    const int32_t clamped = std::clamp(stop, int32_t{0}, last);
    return assign_option(
        picker, *picker.selected, spec->id, stops[static_cast<std::size_t>(clamped)]
    );
}

/// Moves a slider's knob to the column under the pointer.
///
/// @param[in,out] picker the picker
/// @param list the display list
/// @param id the control
/// @param column the column
/// @return what the host is to do
ScreenResult
drag_slider(Picker& picker, const kit::DisplayList& list, kit::ControlId id, int32_t column) {
    const kit::Control* control = kit::control_of(list, id);
    const kit::Item* item = slider_item(list, id);
    if (control == nullptr || item == nullptr)
        return redrawn();
    const auto* look = std::get_if<kit::SliderLook>(&item->look);
    if (look == nullptr)
        return redrawn();
    return set_stop(picker, list, id, kit::stop_at(control->rect, column, look->stops));
}

/// The level under a point on a strip.
///
/// @param list the display list
/// @param id the control
/// @param at the point
/// @return the level; none when the strip is not drawn
std::optional<std::size_t>
level_under(const kit::DisplayList& list, kit::ControlId id, kit::Point at) {
    const kit::Item* item = levels_item(list, id);
    if (item == nullptr)
        return std::nullopt;
    const auto* look = std::get_if<kit::LevelsLook>(&item->look);
    if (look == nullptr || look->captions.empty())
        return std::nullopt;
    return kit::level_at(item->rect, look->captions.size(), look->level_width, at.x);
}

/// Flips a toggle option.
///
/// @param[in,out] picker the picker
/// @param id the control
/// @return what the host is to do
ScreenResult flip_option(Picker& picker, kit::ControlId id) {
    const OptionSpec* spec = option_of(picker, id);
    if (spec == nullptr || spec->values.size() < 2 || !picker.selected)
        return redrawn();
    std::string current;
    if (const std::optional<std::size_t> at = detail::on_index(picker, *picker.selected))
        current = detail::chosen_value(picker.on[*at], spec->id);
    const std::string& value = current == spec->values[1] ? spec->values[0] : spec->values[1];
    return assign_option(picker, *picker.selected, spec->id, value);
}

/// Sets a toggle to its left or right value.
///
/// @param[in,out] picker the picker
/// @param id the control
/// @param right true for the right value
/// @return what the host is to do
ScreenResult set_toggle(Picker& picker, kit::ControlId id, bool right) {
    const OptionSpec* spec = option_of(picker, id);
    if (spec == nullptr || spec->values.empty() || !picker.selected)
        return {};
    const std::size_t index = right && spec->values.size() > 1 ? std::size_t{1} : std::size_t{0};
    return assign_option(picker, *picker.selected, spec->id, spec->values[index]);
}

/// Cycles a closed choice's value.
///
/// @param[in,out] picker the picker
/// @param id the control
/// @param toward_right true for the next value
/// @return what the host is to do
ScreenResult cycle_choice(Picker& picker, kit::ControlId id, bool toward_right) {
    const OptionSpec* spec = option_of(picker, id);
    if (spec == nullptr || spec->values.empty() || !picker.selected)
        return {};
    std::string current;
    if (const std::optional<std::size_t> at = detail::on_index(picker, *picker.selected))
        current = detail::chosen_value(picker.on[*at], spec->id);
    std::size_t index = 0;
    for (std::size_t place = 0; place < spec->values.size(); ++place) {
        if (spec->values[place] == current)
            index = place;
    }
    const std::size_t last = spec->values.size() - 1;
    if (toward_right)
        index = index == last ? 0 : index + 1;
    else
        index = index == 0 ? last : index - 1;
    return assign_option(picker, *picker.selected, spec->id, spec->values[index]);
}

/// Moves the selected row through the list.
///
/// Past either end, and upward with nothing selected, the move is refused
/// so the focus can leave the list. Home, End and the page keys clamp.
/// With nothing selected, a downward key and Home select the first row, and
/// End selects the last.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param delta how many rows; negative moves up
/// @param clamp_ends true when the move stays in the list
/// @param to_last true for End, which selects the last row when none is
/// @return true when the selection changed
bool move_selection(
    Picker& picker,
    ScreenState& state,
    const kit::DisplayList& list,
    int delta,
    bool clamp_ends,
    bool to_last
) {
    const std::vector<Row> shown = shown_rows(picker);
    if (shown.empty())
        return false;
    std::optional<std::size_t> place;
    if (picker.selected) {
        for (std::size_t index = 0; index < shown.size(); ++index) {
            if (shown[index].id == *picker.selected)
                place = index;
        }
    }
    std::size_t next = 0;
    if (!place) {
        if (to_last)
            next = shown.size() - 1;
        else if (delta < 0 && !clamp_ends)
            return false;
        else
            next = 0;
    } else {
        const int64_t wanted = static_cast<int64_t>(*place) + static_cast<int64_t>(delta);
        if (!clamp_ends && (wanted < 0 || wanted >= static_cast<int64_t>(shown.size())))
            return false;
        if (wanted < 0)
            next = 0;
        else if (wanted >= static_cast<int64_t>(shown.size()))
            next = shown.size() - 1;
        else
            next = static_cast<std::size_t>(wanted);
    }
    if (place && *place == next)
        return false;
    select_row(picker, state, list, shown[next].id, true);
    return true;
}

/// How many rows the list shows, at least one.
///
/// @param list the display list
/// @return the rows
int rows_on_screen(const kit::DisplayList& list) {
    const PaneScroll scroll = list_scroll_of(list);
    if (!scroll.ok || scroll.step <= 0)
        return 1;
    return std::max(scroll.port.height / scroll.step, int32_t{1});
}

/// Scrolls a pane to an offset.
///
/// @param[in,out] state the screen's state
/// @param in_details true for the details
/// @param offset the offset
void scroll_to(ScreenState& state, bool in_details, int32_t offset) {
    if (in_details)
        state.details_scroll = offset;
    else
        state.list_scroll = offset;
}

/// Turns a pane by the wheel.
///
/// @param[in,out] state the screen's state
/// @param scroll the pane
/// @param in_details true for the details
/// @param notches the turn; positive towards the top
/// @return what the host is to do
ScreenResult
turn_pane(ScreenState& state, const PaneScroll& scroll, bool in_details, float notches) {
    if (!scroll.ok || !std::isfinite(notches) || notches > 1000000.f || notches < -1000000.f)
        return {};
    kit::WheelCarry& carry = in_details ? state.details_wheel : state.list_wheel;
    const int32_t next =
        kit::wheel_offset(carry, notches, scroll.step, scroll.offset, scroll.limit);
    if (next == scroll.offset)
        return {};
    scroll_to(state, in_details, next);
    return redrawn();
}

/// Scrolls the details by a page, or to an end.
///
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param pressed the key
/// @return what the host is to do
ScreenResult scroll_details(ScreenState& state, const kit::DisplayList& list, kit::Key pressed) {
    const PaneScroll scroll = details_scroll_of(list);
    if (!scroll.ok)
        return {};
    int32_t next = scroll.offset;
    if (pressed == kit::Key::home) {
        next = 0;
    } else if (pressed == kit::Key::end) {
        next = scroll.limit;
    } else {
        const int32_t page =
            std::max(scroll.port.height - kit::compact_metrics.small_line, int32_t{1});
        const int32_t delta = pressed == kit::Key::page_up ? -page : page;
        next = std::clamp(scroll.offset + delta, int32_t{0}, scroll.limit);
    }
    if (next == scroll.offset)
        return {};
    state.details_scroll = next;
    return redrawn();
}

/// Tells whether the details page is showing.
///
/// @param state the screen's state
/// @param list the display list
/// @return true while it is
bool page_open(const ScreenState& state, const kit::DisplayList& list) {
    return state.details_page || kit::control_of(list, controls::back) != nullptr;
}

/// Leaves the details page.
///
/// @param[in,out] state the screen's state
/// @return what the host is to do
ScreenResult leave_page(ScreenState& state) {
    state.details_page = false;
    state.menu = Menu::none;
    if (state.interaction.focus_shown)
        state.interaction.focused = controls::list;
    return redrawn();
}

/// The focus the details page takes when the keys opened it.
///
/// @param picker the picker
/// @return the control
kit::ControlId page_focus(const Picker& picker) {
    if (picker.inputs.read_only)
        return controls::done;
    if (!picker.selected)
        return controls::back;
    const Mutator* mutator = detail::find_mutator(picker, *picker.selected);
    if (mutator != nullptr && detail::on_index(picker, mutator->id)) {
        if (!mutator->options.empty())
            return controls::first_option;
        if (mutator->brings)
            return controls::clash;
    }
    return controls::back;
}

/// Opens Compact's details page.
///
/// @param picker the picker
/// @param[in,out] state the screen's state
/// @return what the host is to do
ScreenResult open_page(const Picker& picker, ScreenState& state) {
    if (!picker.selected || detail::find_mutator(picker, *picker.selected) == nullptr)
        return {};
    state.details_page = true;
    state.details_scroll = 0;
    state.menu = Menu::none;
    if (state.interaction.focus_shown) {
        state.interaction.focused = page_focus(picker);
        state.interaction.focus_shown = true;
        state.interaction.pointer_hid_ring = false;
    }
    return redrawn();
}

/// Applies a search the field now holds: the model's query matches it, the
/// list starts at the top and then shows the selection.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
void apply_query(Picker& picker, ScreenState& state, const kit::DisplayList& list) {
    picker.query = state.search.text;
    state.menu = Menu::none;
    state.list_scroll = 0;
    show_selected(picker, state, list);
}

/// Gives the search the focus. The details page is left first, so the field
/// is on the screen the host lays out next.
///
/// @param[in,out] state the screen's state
void focus_search(ScreenState& state) {
    state.details_page = false;
    state.menu = Menu::none;
    state.interaction.focused = controls::search;
    state.interaction.focus_shown = true;
    state.interaction.pointer_hid_ring = false;
}

/// The first control the layout put in Tab's order.
///
/// @param list the display list
/// @return the control; none when the order is empty
kit::ControlId first_stop(const kit::DisplayList& list) {
    if (list.tab_order.empty())
        return kit::no_control;
    return list.tab_order.front();
}

/// The first details control in the layout's Tab order. A clipped one stays
/// in that order, so the keys can scroll it into view.
///
/// @param list the display list
/// @return the control; none when the details have none
kit::ControlId first_detail(const kit::DisplayList& list) {
    for (const kit::ControlId id : list.tab_order) {
        const kit::Control* control = kit::control_of(list, id);
        if (control != nullptr && control->group == controls::details_group)
            return id;
    }
    return kit::no_control;
}

/// Moves the focus to the list when the focused control is gone.
///
/// @param[in,out] state the screen's state
/// @param list the display list
void repair_focus(ScreenState& state, const kit::DisplayList& list) {
    if (state.interaction.focused == kit::no_control)
        return;
    if (kit::control_of(list, state.interaction.focused) != nullptr)
        return;
    if (kit::control_of(list, controls::list) != nullptr)
        state.interaction.focused = controls::list;
    else
        state.interaction.focused = first_stop(list);
}

/// How many tags the TAGS menu holds: the ones with no pill.
///
/// @param picker the picker
/// @param list the display list
/// @return the count
int32_t overflow_count(const Picker& picker, const kit::DisplayList& list) {
    const std::vector<std::string> tags = tags_shown(picker);
    int32_t count = 0;
    for (std::size_t index = 0; index < tags.size(); ++index) {
        const kit::ControlId id =
            controls::first_tag_pill + static_cast<kit::ControlId>(as_int(index));
        if (kit::control_of(list, id) == nullptr)
            ++count;
    }
    return count;
}

/// Returns one overflow tag.
///
/// @param picker the picker
/// @param list the display list
/// @param place the item's place in the menu
/// @return the tag; empty when the place is not one
std::string overflow_tag(const Picker& picker, const kit::DisplayList& list, int32_t place) {
    if (place < 0)
        return {};
    const std::vector<std::string> tags = tags_shown(picker);
    int32_t seen = 0;
    for (std::size_t index = 0; index < tags.size(); ++index) {
        const kit::ControlId id =
            controls::first_tag_pill + static_cast<kit::ControlId>(as_int(index));
        if (kit::control_of(list, id) != nullptr)
            continue;
        if (seen == place)
            return tags[index];
        ++seen;
    }
    return {};
}

/// How many items a filter or TAGS menu has. A choice is counted by the open menu.
///
/// @param picker the picker
/// @param which the menu
/// @param list the display list
/// @return the count; 0 for a choice or no menu
int32_t menu_count_of(const Picker& picker, Menu which, const kit::DisplayList& list) {
    switch (which) {
    case Menu::filter:
        return 3 + as_int(tags_shown(picker).size());
    case Menu::tags:
        return overflow_count(picker, list);
    case Menu::choice:
    case Menu::none:
        break;
    }
    return 0;
}

/// How many items the open menu has.
///
/// @param picker the picker
/// @param state the screen's state
/// @param list the display list
/// @return the count
int32_t menu_count(const Picker& picker, const ScreenState& state, const kit::DisplayList& list) {
    switch (state.menu) {
    case Menu::filter:
    case Menu::tags:
        return menu_count_of(picker, state.menu, list);
    case Menu::choice: {
        if (!picker.selected)
            return 0;
        const Mutator* mutator = detail::find_mutator(picker, *picker.selected);
        if (mutator == nullptr)
            return 0;
        for (const OptionSpec& spec : mutator->options) {
            if (spec.id == state.menu_option)
                return as_int(spec.values.size());
        }
        return 0;
    }
    case Menu::none:
        break;
    }
    return 0;
}

/// Returns the control the open menu belongs to.
///
/// @param picker the picker
/// @param state the screen's state
/// @return the control; none when no menu is open
kit::ControlId menu_field(const Picker& picker, const ScreenState& state) {
    switch (state.menu) {
    case Menu::filter:
        return controls::filter;
    case Menu::tags:
        return controls::tags;
    case Menu::choice: {
        if (!picker.selected)
            return kit::no_control;
        const Mutator* mutator = detail::find_mutator(picker, *picker.selected);
        if (mutator == nullptr)
            return kit::no_control;
        for (std::size_t index = 0; index < mutator->options.size(); ++index) {
            if (mutator->options[index].id == state.menu_option)
                return controls::first_option + static_cast<kit::ControlId>(as_int(index));
        }
        return kit::no_control;
    }
    case Menu::none:
        break;
    }
    return kit::no_control;
}

/// Moves the menu's mark, keeping it among the items shown.
///
/// @param[in,out] state the screen's state
/// @param count how many items the menu holds
/// @param marked the item to mark
void mark_item(ScreenState& state, int32_t count, int32_t marked) {
    if (count <= 0) {
        state.menu_marked = -1;
        state.menu_first = 0;
        return;
    }
    const int32_t shown = kit::shown_choices(static_cast<std::size_t>(count));
    state.menu_marked = std::clamp(marked, int32_t{0}, count - 1);
    state.menu_first =
        std::clamp(state.menu_first, state.menu_marked - shown + 1, state.menu_marked);
    state.menu_first =
        std::clamp(state.menu_first, int32_t{0}, std::max(count - shown, int32_t{0}));
}

/// Sets the filter. A status item also clears the tag. The list starts at
/// the top and then shows the selection.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param which the filter
/// @param clear_tag true when the tag is cleared
void set_filter_to(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, Filter which, bool clear_tag
) {
    const bool changed = picker.filter != which || (clear_tag && !picker.tag.empty());
    picker.filter = which;
    if (clear_tag)
        picker.tag.clear();
    state.menu = Menu::none;
    if (!changed)
        return;
    state.list_scroll = 0;
    show_selected(picker, state, list);
}

/// Sets the tag, or clears it when it is that tag already.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param tag the tag
void toggle_tag(Picker& picker, ScreenState& state, const kit::DisplayList& list, std::string tag) {
    if (tag.empty()) {
        state.menu = Menu::none;
        return;
    }
    if (picker.tag == tag)
        picker.tag.clear();
    else
        picker.tag = std::move(tag);
    state.menu = Menu::none;
    state.list_scroll = 0;
    show_selected(picker, state, list);
}

/// Does what choosing a menu item asks.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param place the item
/// @return what the host is to do
ScreenResult
choose_item(Picker& picker, ScreenState& state, const kit::DisplayList& list, int32_t place) {
    const Menu which = state.menu;
    const int32_t count = menu_count(picker, state, list);
    if (place < 0 || place >= count) {
        state.menu = Menu::none;
        return redrawn();
    }
    if (which == Menu::filter) {
        if (place < 3) {
            const Filter filters[] = {Filter::all, Filter::installed, Filter::downloads};
            set_filter_to(picker, state, list, filters[place], true);
        } else {
            const std::vector<std::string> tags = tags_shown(picker);
            const auto index = static_cast<std::size_t>(place - 3);
            if (index < tags.size())
                toggle_tag(picker, state, list, tags[index]);
            else
                state.menu = Menu::none;
        }
        return redrawn();
    }
    if (which == Menu::tags) {
        toggle_tag(picker, state, list, overflow_tag(picker, list, place));
        return redrawn();
    }
    if (which == Menu::choice && picker.selected) {
        const Mutator* mutator = detail::find_mutator(picker, *picker.selected);
        const std::string option = state.menu_option;
        state.menu = Menu::none;
        if (mutator == nullptr)
            return redrawn();
        for (const OptionSpec& spec : mutator->options) {
            if (spec.id != option)
                continue;
            const auto index = static_cast<std::size_t>(place);
            if (index >= spec.values.size())
                return redrawn();
            return assign_option(picker, *picker.selected, spec.id, spec.values[index]);
        }
    }
    state.menu = Menu::none;
    return redrawn();
}

/// The item a filter or TAGS menu would mark. The menu need not be open.
///
/// @param picker the picker
/// @param which the menu
/// @param list the display list
/// @return the item; 0 for a choice or no menu
int32_t chosen_in(const Picker& picker, Menu which, const kit::DisplayList& list) {
    if (which == Menu::filter) {
        int32_t chosen = 0;
        if (picker.filter == Filter::installed)
            chosen = 1;
        else if (picker.filter == Filter::downloads)
            chosen = 2;
        if (!picker.tag.empty()) {
            const std::vector<std::string> tags = tags_shown(picker);
            for (std::size_t index = 0; index < tags.size(); ++index) {
                if (tags[index] == picker.tag)
                    chosen = 3 + as_int(index);
            }
        }
        return chosen;
    }
    if (which == Menu::tags) {
        const std::vector<std::string> tags = tags_shown(picker);
        int32_t seen = 0;
        for (std::size_t index = 0; index < tags.size(); ++index) {
            const kit::ControlId id =
                controls::first_tag_pill + static_cast<kit::ControlId>(as_int(index));
            if (kit::control_of(list, id) != nullptr)
                continue;
            if (tags[index] == picker.tag)
                return seen;
            ++seen;
        }
    }
    return 0;
}

/// The item a menu marks when it opens.
///
/// @param picker the picker
/// @param state the screen's state
/// @param list the display list
/// @return the item
int32_t chosen_item(const Picker& picker, const ScreenState& state, const kit::DisplayList& list) {
    if (state.menu == Menu::filter || state.menu == Menu::tags)
        return chosen_in(picker, state.menu, list);
    if (state.menu == Menu::choice && picker.selected) {
        const Mutator* mutator = detail::find_mutator(picker, *picker.selected);
        if (mutator == nullptr)
            return 0;
        for (const OptionSpec& spec : mutator->options) {
            if (spec.id != state.menu_option)
                continue;
            std::string current;
            if (const std::optional<std::size_t> at = detail::on_index(picker, *picker.selected))
                current = detail::chosen_value(picker.on[*at], spec.id);
            for (std::size_t index = 0; index < spec.values.size(); ++index) {
                if (spec.values[index] == current)
                    return as_int(index);
            }
        }
    }
    return 0;
}

/// Opens a menu, or closes it when that menu is open.
///
/// @param picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param which the menu
/// @param option the option, when `which` is a choice
/// @return what the host is to do
ScreenResult toggle_menu(
    const Picker& picker,
    ScreenState& state,
    const kit::DisplayList& list,
    Menu which,
    std::string option
) {
    if (state.menu == which && (which != Menu::choice || state.menu_option == option)) {
        state.menu = Menu::none;
        return redrawn();
    }
    state.menu = which;
    state.menu_option = std::move(option);
    state.menu_first = 0;
    state.menu_marked = -1;
    mark_item(state, menu_count(picker, state, list), chosen_item(picker, state, list));
    return redrawn();
}

/// Takes a key while a menu is open. Tab closes it and moves on.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param pressed the key
/// @return what the host is to do; none when the key goes on
std::optional<ScreenResult>
menu_key(Picker& picker, ScreenState& state, const kit::DisplayList& list, kit::Key pressed) {
    const int32_t count = menu_count(picker, state, list);
    const int32_t page = kit::shown_choices(static_cast<std::size_t>(std::max(count, int32_t{0})));
    switch (pressed) {
    case kit::Key::escape:
        state.menu = Menu::none;
        return redrawn();
    case kit::Key::up:
        mark_item(state, count, state.menu_marked - 1);
        return redrawn();
    case kit::Key::down:
        mark_item(state, count, state.menu_marked + 1);
        return redrawn();
    case kit::Key::page_up:
        mark_item(state, count, state.menu_marked - page);
        return redrawn();
    case kit::Key::page_down:
        mark_item(state, count, state.menu_marked + page);
        return redrawn();
    case kit::Key::home:
        mark_item(state, count, 0);
        return redrawn();
    case kit::Key::end:
        mark_item(state, count, count - 1);
        return redrawn();
    case kit::Key::enter:
    case kit::Key::space:
        return choose_item(picker, state, list, state.menu_marked);
    case kit::Key::tab:
    case kit::Key::back_tab:
        state.menu = Menu::none;
        return std::nullopt;
    default:
        return ScreenResult{};
    }
}

/// One gap a drag can land in.
struct Gap {
    int32_t y{};    ///< the line, in window points
    Drop where{};   ///< where a drop there lands
    bool current{}; ///< the dragged row already lands here
};

/// The gaps among the On rows and the Off section.
///
/// A gap at an On row's top is that place. The gap under the last On row is
/// the end. The Off section is its heading, or the last On row's bottom
/// when the list draws no Off heading.
///
/// @param picker the picker
/// @param list the display list
/// @param dragged the mutator being dragged
/// @return the gaps
std::vector<Gap>
gaps_of(const Picker& picker, const kit::DisplayList& list, std::string_view dragged) {
    std::vector<Gap> gaps;
    const std::vector<Row> shown = shown_rows(picker);
    const PaneScroll scroll = list_scroll_of(list);
    int32_t last_bottom = 0;
    bool any_on = false;
    std::size_t on_index = 0;
    for (std::size_t place = 0; place < shown.size(); ++place) {
        if (!shown[place].on)
            continue;
        const kit::Control* control =
            kit::control_of(list, controls::first_row + static_cast<kit::ControlId>(as_int(place)));
        if (control == nullptr)
            continue;
        Gap gap;
        gap.y = control->rect.y;
        gap.where.into_on = true;
        gap.where.place = on_index;
        gaps.push_back(gap);
        last_bottom = control->rect.y + control->rect.height;
        any_on = true;
        ++on_index;
    }
    if (any_on) {
        Gap end;
        end.y = last_bottom;
        end.where.into_on = true;
        end.where.place = on_index;
        gaps.push_back(end);
    }
    int32_t off_y = any_on ? last_bottom : 0;
    bool off_found = false;
    int32_t lower_heading = 0;
    int32_t headings = 0;
    if (scroll.ok) {
        for (const kit::Item& item : list.items) {
            if (item.role != kit::Role::heading || !same_rect(item.clip, scroll.port))
                continue;
            if (headings == 0 || item.rect.y > lower_heading)
                lower_heading = item.rect.y;
            ++headings;
        }
    }
    if (headings >= 2 || (headings == 1 && !any_on)) {
        off_y = lower_heading;
        off_found = true;
    } else if (any_on) {
        off_found = true;
    }
    if (off_found) {
        Gap off;
        off.y = off_y;
        off.where.into_on = false;
        gaps.push_back(off);
    }
    const std::optional<std::size_t> on_at = detail::on_index(picker, dragged);
    for (Gap& gap : gaps) {
        if (on_at)
            gap.current = gap.where.into_on && gap.where.place == *on_at;
        else
            gap.current = !gap.where.into_on;
    }
    return gaps;
}

/// The gap nearest a row of the list.
///
/// Equal distances keep the gap the dragged row already lands in.
///
/// @param picker the picker
/// @param list the display list
/// @param dragged the mutator
/// @param pointer_y the row the pointer is on, after a scroll in this event
/// @return the gap; none when the list has nowhere to land
std::optional<Drop> nearest_gap(
    const Picker& picker, const kit::DisplayList& list, std::string_view dragged, int32_t pointer_y
) {
    const std::vector<Gap> gaps = gaps_of(picker, list, dragged);
    std::optional<Drop> nearest;
    int32_t best = 0;
    bool current = false;
    for (const Gap& gap : gaps) {
        const int32_t distance = absolute(gap.y - pointer_y);
        if (!nearest || distance < best || (distance == best && gap.current && !current)) {
            nearest = gap.where;
            best = distance;
            current = gap.current;
        }
    }
    return nearest;
}

/// Scrolls the list one row when the pointer is within 16 points of its top
/// or bottom edge.
///
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param at the pointer
/// @return how far the offset moved, in points; the list on screen has not
///     moved yet
int32_t auto_scroll(ScreenState& state, const kit::DisplayList& list, kit::Point at) {
    const PaneScroll scroll = list_scroll_of(list);
    if (!scroll.ok || scroll.step <= 0 || scroll.limit <= 0)
        return 0;
    const int32_t from_top = absolute(at.y - scroll.port.y);
    const int32_t from_bottom = absolute(at.y - (scroll.port.y + scroll.port.height));
    int32_t delta = 0;
    if (from_top <= scroll_edge && from_bottom <= scroll_edge)
        delta = from_top <= from_bottom ? -1 : 1;
    else if (from_top <= scroll_edge)
        delta = -1;
    else if (from_bottom <= scroll_edge)
        delta = 1;
    if (delta == 0)
        return 0;
    const int32_t next = std::clamp(scroll.offset + delta * scroll.step, int32_t{0}, scroll.limit);
    if (next == scroll.offset)
        return 0;
    state.list_scroll = next;
    return next - scroll.offset;
}

/// Follows a drag: the row's top, the nearest gap, and one row of scrolling.
///
/// @param[in,out] state the screen's state
/// @param picker the picker
/// @param list the display list
/// @param at the pointer
void follow_drag(
    ScreenState& state, const Picker& picker, const kit::DisplayList& list, kit::Point at
) {
    if (!state.drag)
        return;
    const std::vector<Row> shown = shown_rows(picker);
    bool found = false;
    for (const Row& row : shown) {
        if (row.id == state.drag->id)
            found = true;
    }
    if (!found) {
        end_hold(state);
        return;
    }
    state.drag->y = at.y - state.hold.grab;
    const PaneScroll scroll = list_scroll_of(list);
    if (!scroll.ok || !kit::contains(scroll.port, at)) {
        state.drag->target.reset();
        return;
    }
    const int32_t shifted = auto_scroll(state, list, at);
    state.drag->target = nearest_gap(picker, list, state.drag->id, at.y + shifted);
}

/// Drops a dragged row through the model.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param at the pointer
/// @return what the host is to do
ScreenResult
release_drag(Picker& picker, ScreenState& state, const kit::DisplayList& list, kit::Point at) {
    if (!state.drag) {
        end_hold(state);
        return redrawn();
    }
    const std::string id = state.drag->id;
    const PaneScroll scroll = list_scroll_of(list);
    const bool inside = scroll.ok && kit::contains(scroll.port, at);
    std::optional<Drop> where;
    if (inside)
        where = nearest_gap(picker, list, id, at.y);
    end_hold(state);
    if (!inside || !where)
        return redrawn();
    const std::vector<Row> shown = shown_rows(picker);
    bool found = false;
    for (const Row& row : shown) {
        if (row.id == id)
            found = true;
    }
    if (!found)
        return redrawn();
    const std::vector<std::string> before = detail::on_ids(picker);
    const std::optional<TurnOff> asked = drop(picker, id, *where);
    return switched(picker, before, asked);
}

/// The scroll bar under a point. Each bar is its named control, and that
/// control's rectangle is the well, thumb included.
///
/// @param list the display list
/// @param at the pointer
/// @return the bar; none when the point misses both
Bar bar_at(const kit::DisplayList& list, kit::Point at) {
    if (const kit::Control* bar = kit::control_of(list, controls::list_bar);
        bar != nullptr && kit::contains(bar->rect, at))
        return Bar::list;
    if (const kit::Control* bar = kit::control_of(list, controls::details_bar);
        bar != nullptr && kit::contains(bar->rect, at))
        return Bar::details;
    return Bar::none;
}

/// The scroll area of a held bar. The well is the named control's rectangle.
///
/// @param list the display list
/// @param which the bar
/// @return the area; none while that pane does not scroll
std::optional<kit::ScrollArea> bar_area(const kit::DisplayList& list, Bar which) {
    const PaneScroll scroll =
        which == Bar::details ? details_scroll_of(list) : list_scroll_of(list);
    if (!scroll.ok || scroll.limit <= 0)
        return std::nullopt;
    const kit::ControlId id = which == Bar::details ? controls::details_bar : controls::list_bar;
    const kit::Control* bar = kit::control_of(list, id);
    if (bar == nullptr)
        return std::nullopt;
    kit::ScrollArea area;
    area.view = scroll.port;
    area.well = bar->rect;
    area.content_height = scroll.limit + scroll.port.height;
    area.limit = scroll.limit;
    area.offset = scroll.offset;
    return area;
}

/// Holds a scroll bar, grabbing the thumb or jumping its middle to the pointer.
///
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param which the bar
/// @param at the pointer, before a finger's shift
/// @return what the host is to do
ScreenResult take_bar(ScreenState& state, const kit::DisplayList& list, Bar which, kit::Point at) {
    const std::optional<kit::ScrollArea> area = bar_area(list, which);
    if (!area)
        return {};
    const kit::Rect thumb = area->thumb(kit::compact_metrics.least_thumb_height);
    state.bar = which;
    state.interaction.pressed = kit::no_control;
    if (kit::contains(thumb, at)) {
        state.bar_grab = at.y - thumb.y;
    } else {
        state.bar_grab = thumb.height / 2;
        scroll_to(
            state,
            which == Bar::details,
            area->offset_at(at.y - state.bar_grab, kit::compact_metrics.least_thumb_height)
        );
    }
    return redrawn();
}

/// Moves a held scroll bar.
///
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param at the pointer
/// @return what the host is to do
ScreenResult move_bar(ScreenState& state, const kit::DisplayList& list, kit::Point at) {
    const std::optional<kit::ScrollArea> area = bar_area(list, state.bar);
    if (!area)
        return {};
    const int32_t next =
        area->offset_at(at.y - state.bar_grab, kit::compact_metrics.least_thumb_height);
    if (next == area->offset)
        return {};
    scroll_to(state, state.bar == Bar::details, next);
    return redrawn();
}

/// The point a later move uses after a finger's press shifted onto a control.
///
/// @param state the screen's state
/// @param at the pointer
/// @return the point
kit::Point shifted_point(const ScreenState& state, kit::Point at) {
    at.x += state.interaction.finger_shift.x;
    at.y += state.interaction.finger_shift.y;
    return at;
}

/// Tells whether a point lies in the list or the details.
///
/// @param list the display list
/// @param at the point
/// @param in_details set when the point is in the details
/// @return true when it lies in either pane
bool pane_at(const kit::DisplayList& list, kit::Point at, bool& in_details) {
    in_details = false;
    const PaneScroll listed = list_scroll_of(list);
    if (listed.ok && kit::contains(listed.port, at))
        return true;
    const PaneScroll details = details_scroll_of(list);
    if (details.ok && kit::contains(details.port, at)) {
        in_details = true;
        return true;
    }
    if (const kit::Control* bar = kit::control_of(list, controls::details_bar);
        bar != nullptr && kit::contains(bar->rect, at)) {
        in_details = true;
        return true;
    }
    if (const kit::Control* bar = kit::control_of(list, controls::list_bar);
        bar != nullptr && kit::contains(bar->rect, at)) {
        in_details = false;
        return true;
    }
    return false;
}

/// Starts a drag from a grip, or remembers the press until the mouse moves.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the grip
/// @param at the pointer
/// @param finger true for a finger, which drags at once
void take_grip(
    Picker& picker,
    ScreenState& state,
    const kit::DisplayList& list,
    kit::ControlId id,
    kit::Point at,
    bool finger
) {
    const std::string row_id = row_id_of(picker, id);
    if (row_id.empty())
        return;
    const kit::Control* control = kit::control_of(list, id);
    if (control == nullptr || !control->enabled)
        return;
    select_row(picker, state, list, row_id, false);
    int32_t top = at.y;
    const int32_t place = band_place(id, controls::first_grip, controls::first_switch);
    if (place >= 0) {
        const kit::Control* row =
            kit::control_of(list, controls::first_row + static_cast<kit::ControlId>(place));
        if (row != nullptr)
            top = row->rect.y;
    }
    state.hold.kind = Hold::Kind::grip;
    state.hold.from = at;
    state.hold.grab = at.y - top;
    state.hold.control = id;
    if (!finger)
        return;
    DragShown shown;
    shown.id = row_id;
    shown.y = top;
    state.drag = std::move(shown);
    follow_drag(state, picker, list, at);
}

/// What a press on a control does, once it is released over that control.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the control
/// @param at the point of the release; none for a key
/// @param finger true when a finger pressed it
/// @return what the host is to do
ScreenResult activate(
    Picker& picker,
    ScreenState& state,
    const kit::DisplayList& list,
    kit::ControlId id,
    std::optional<kit::Point> at,
    bool finger
) {
    if (menu_item_control(id))
        return choose_item(picker, state, list, id - controls::first_menu_item);
    const std::string row_id = row_id_of(picker, id);
    if (band_place(id, controls::first_row, controls::first_grip) >= 0 && !row_id.empty()) {
        select_row(picker, state, list, row_id, false);
        return redrawn();
    }
    if (band_place(id, controls::first_grip, controls::first_switch) >= 0)
        return redrawn();
    if (band_place(id, controls::first_switch, controls::first_option) >= 0 && !row_id.empty()) {
        select_row(picker, state, list, row_id, false);
        return flip_switch(picker, row_id);
    }
    if (id == controls::clash || option_of(picker, id) != nullptr) {
        const kit::Control* control = kit::control_of(list, id);
        if (control == nullptr)
            return redrawn();
        if (control->kind == kit::ControlKind::levels && at) {
            const std::optional<std::size_t> level = level_under(list, id, *at);
            if (!level)
                return redrawn();
            return set_level(picker, list, id, *level);
        }
        if (control->kind == kit::ControlKind::toggle)
            return flip_option(picker, id);
        if (control->kind == kit::ControlKind::choice) {
            std::string option;
            if (const OptionSpec* spec = option_of(picker, id))
                option = spec->id;
            return toggle_menu(picker, state, list, Menu::choice, std::move(option));
        }
        return redrawn();
    }
    if (id == controls::up || id == controls::down) {
        if (!picker.selected)
            return redrawn();
        if (id == controls::up)
            std::ignore = move_up(picker, *picker.selected);
        else
            std::ignore = move_down(picker, *picker.selected);
        show_selected(picker, state, list);
        return redrawn();
    }
    if (id == controls::details)
        return open_page(picker, state);
    if (id == controls::back)
        return leave_page(state);
    if (id == controls::cancel || id == controls::done || id == controls::close_mark) {
        ScreenResult answered;
        answered.action = PickerAction::cancel;
        answered.redraw = true;
        return answered;
    }
    if (id == controls::use) {
        const kit::Control* control = kit::control_of(list, controls::use);
        if (control == nullptr || !control->enabled)
            return redrawn();
        ScreenResult answered;
        answered.action = PickerAction::use;
        answered.redraw = true;
        return answered;
    }
    if (id == controls::filter)
        return toggle_menu(picker, state, list, Menu::filter, {});
    if (id == controls::tags)
        return toggle_menu(picker, state, list, Menu::tags, {});
    if (id == controls::filter_all)
        set_filter_to(picker, state, list, Filter::all, false);
    else if (id == controls::filter_installed)
        set_filter_to(picker, state, list, Filter::installed, false);
    else if (id == controls::filter_downloads)
        set_filter_to(picker, state, list, Filter::downloads, false);
    else if (band_place(id, controls::first_tag_pill, controls::first_menu_item) >= 0) {
        const int32_t place = id - controls::first_tag_pill;
        const std::vector<std::string> tags = tags_shown(picker);
        const auto index = static_cast<std::size_t>(place);
        if (index < tags.size())
            toggle_tag(picker, state, list, tags[index]);
    } else if (id == controls::search) {
        state.interaction.focused = controls::search;
        state.interaction.focus_shown = true;
        state.interaction.pointer_hid_ring = false;
    } else if (finger) {
        return {};
    }
    return redrawn();
}

/// A key on a details control that takes steps.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the control
/// @param pressed the key
/// @return what the host is to do; none when the key is not one this handles
std::optional<ScreenResult> step_key(
    Picker& picker,
    ScreenState& state,
    const kit::DisplayList& list,
    kit::ControlId id,
    kit::Key pressed
) {
    const kit::Control* control = kit::control_of(list, id);
    if (control == nullptr || !control->enabled)
        return std::nullopt;
    const bool choice = control->kind == kit::ControlKind::choice;
    const bool levels = control->kind == kit::ControlKind::levels || id == controls::clash;
    const bool slider = control->kind == kit::ControlKind::slider;
    const bool toggle = control->kind == kit::ControlKind::toggle;
    if (!choice && !levels && !slider && !toggle)
        return std::nullopt;
    if (pressed == kit::Key::left || pressed == kit::Key::right) {
        const bool toward_right = pressed == kit::Key::right;
        if (levels)
            return step_levels(picker, list, id, toward_right);
        if (slider) {
            const kit::Item* item = slider_item(list, id);
            const auto* look =
                item == nullptr ? nullptr : std::get_if<kit::SliderLook>(&item->look);
            if (look == nullptr)
                return ScreenResult{};
            const int32_t delta = toward_right ? 1 : -1;
            return set_stop(picker, list, id, look->stop + delta);
        }
        if (toggle)
            return set_toggle(picker, id, toward_right);
        return cycle_choice(picker, id, toward_right);
    }
    if (pressed == kit::Key::space || pressed == kit::Key::enter) {
        if (toggle)
            return flip_option(picker, id);
        if (choice) {
            std::string option;
            if (const OptionSpec* spec = option_of(picker, id))
                option = spec->id;
            return toggle_menu(picker, state, list, Menu::choice, std::move(option));
        }
        return ScreenResult{};
    }
    return std::nullopt;
}

/// Steps the filter or TAGS, so Left and Right are not trapped on a choice.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param toward_right true for the next item
/// @return what the host is to do
ScreenResult
cycle_field(Picker& picker, ScreenState& state, const kit::DisplayList& list, bool toward_right) {
    Menu which = Menu::none;
    if (state.interaction.focused == controls::filter)
        which = Menu::filter;
    else if (state.interaction.focused == controls::tags)
        which = Menu::tags;
    if (which == Menu::none)
        return {};
    const int32_t count = menu_count_of(picker, which, list);
    if (count <= 0)
        return {};
    int32_t marked = chosen_in(picker, which, list);
    if (toward_right)
        marked = marked + 1 >= count ? 0 : marked + 1;
    else
        marked = marked == 0 ? count - 1 : marked - 1;
    state.menu = which;
    state.menu_option.clear();
    return choose_item(picker, state, list, marked);
}

/// Tells whether the focus is in the details, or the details page is the screen.
///
/// @param state the screen's state
/// @param list the display list
/// @return true when page keys scroll the details
bool details_focused(const ScreenState& state, const kit::DisplayList& list) {
    if (page_open(state, list) && kit::control_of(list, controls::list) == nullptr)
        return true;
    if (!state.interaction.focus_shown)
        return false;
    const kit::Control* control = kit::control_of(list, state.interaction.focused);
    return control != nullptr && control->group == controls::details_group;
}

/// What Enter does on the list.
///
/// @param picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @return what the host is to do
ScreenResult enter_list(const Picker& picker, ScreenState& state, const kit::DisplayList& list) {
    if (kit::control_of(list, controls::details) != nullptr)
        return open_page(picker, state);
    const kit::ControlId detail = first_detail(list);
    if (detail == kit::no_control)
        return {};
    state.interaction.focused = detail;
    state.interaction.focus_shown = true;
    state.interaction.pointer_hid_ring = false;
    show_in_details(state, list, detail);
    return redrawn();
}

/// USE THESE, when the screen is offering it.
///
/// Waiting does not disable it. The host asks to get a mutator that is not
/// installed yet.
///
/// @param list the display list
/// @return what the host is to do
ScreenResult use_set(const kit::DisplayList& list) {
    const kit::Control* control = kit::control_of(list, controls::use);
    if (control == nullptr || !control->enabled)
        return {};
    ScreenResult answered;
    answered.action = PickerAction::use;
    answered.redraw = true;
    return answered;
}

} // namespace

std::optional<kit::Rect>
focused_field(const ScreenState& state, const kit::DisplayList& list) noexcept {
    if (!state.interaction.focus_shown || state.interaction.focused != controls::search)
        return std::nullopt;
    const kit::Control* control = kit::control_of(list, controls::search);
    if (control == nullptr)
        return std::nullopt;
    return control->rect;
}

ScreenResult picker_pointer(
    Picker& picker,
    ScreenState& state,
    const kit::DisplayList& list,
    PointerKind kind,
    uint8_t button,
    kit::Point at,
    int32_t finger_reach
) {
    kit::Interaction& interaction = state.interaction;
    switch (kind) {
    case PointerKind::move: {
        if (state.bar != Bar::none)
            return move_bar(state, list, at);
        if (state.hold.kind == Hold::Kind::slider) {
            interaction.hovered = state.hold.control;
            interaction.pressed = state.hold.control;
            return drag_slider(picker, list, state.hold.control, shifted_point(state, at).x);
        }
        if (state.hold.kind == Hold::Kind::grip || state.drag) {
            if (!inside_window(list, at)) {
                end_hold(state);
                return redrawn();
            }
            if (!state.drag) {
                const int32_t across = absolute(at.x - state.hold.from.x);
                const int32_t along = absolute(at.y - state.hold.from.y);
                if (std::max(across, along) >= mouse_drag_points) {
                    DragShown shown;
                    shown.id = row_id_of(picker, state.hold.control);
                    shown.y = at.y - state.hold.grab;
                    if (!shown.id.empty())
                        state.drag = std::move(shown);
                }
            }
            if (state.drag)
                follow_drag(state, picker, list, at);
            return state.drag || state.hold.kind == Hold::Kind::grip ? redrawn() : ScreenResult{};
        }
        if (state.hold.kind == Hold::Kind::finger) {
            const int32_t across = absolute(at.x - state.hold.from.x);
            const int32_t along = absolute(at.y - state.hold.from.y);
            if (!state.hold.scrolling && std::max(across, along) > finger_reach) {
                state.hold.scrolling = true;
                interaction.pressed = kit::no_control;
                interaction.finger_shift = {};
            }
            if (state.hold.scrolling) {
                const PaneScroll scroll =
                    state.hold.in_details ? details_scroll_of(list) : list_scroll_of(list);
                if (!scroll.ok)
                    return {};
                const int32_t next = std::clamp(
                    state.hold.offset - (at.y - state.hold.from.y), int32_t{0}, scroll.limit
                );
                if (next == scroll.offset)
                    return {};
                scroll_to(state, state.hold.in_details, next);
                return redrawn();
            }
        }
        const kit::PointerOutcome moved = kit::pointer_move(interaction, list, at);
        ScreenResult answered;
        answered.redraw = moved.result != kit::PointerResult::none;
        if (state.menu != Menu::none && menu_item_control(moved.control) &&
            moved.control - controls::first_menu_item != state.menu_marked) {
            state.menu_marked = moved.control - controls::first_menu_item;
            answered.redraw = true;
        }
        return answered;
    }
    case PointerKind::down: {
        if (button != 1)
            return {};
        const bool cancelled = holding(state);
        end_hold(state);
        if (state.menu != Menu::none) {
            const kit::ControlId under =
                finger_reach > 0 ? kit::reach(list, at, finger_reach).control : kit::hit(list, at);
            if (!menu_item_control(under) && under != menu_field(picker, state)) {
                state.menu = Menu::none;
                return redrawn();
            }
        }
        if (const Bar which = bar_at(list, at); which != Bar::none)
            return take_bar(state, list, which, at);
        const bool finger = finger_reach > 0;
        const kit::PointerOutcome pressed =
            finger ? kit::finger_down(interaction, list, at, finger_reach)
                   : kit::pointer_down(interaction, list, at);
        ScreenResult answered;
        answered.redraw = pressed.result != kit::PointerResult::none || cancelled;
        const kit::Control* control = kit::control_of(list, pressed.control);
        if (control != nullptr && control->enabled &&
            band_place(control->id, controls::first_grip, controls::first_switch) >= 0) {
            take_grip(picker, state, list, control->id, at, finger);
            return redrawn();
        }
        if (control != nullptr && control->enabled && control->kind == kit::ControlKind::slider &&
            option_of(picker, control->id) != nullptr) {
            state.hold.kind = Hold::Kind::slider;
            state.hold.control = control->id;
            state.hold.from = at;
            interaction.hovered = control->id;
            interaction.pressed = control->id;
            return drag_slider(picker, list, control->id, pressed.at.x);
        }
        if (!finger && !row_id_of(picker, pressed.control).empty())
            select_row(picker, state, list, row_id_of(picker, pressed.control), false);
        if (pressed.control == controls::search) {
            interaction.focused = controls::search;
            interaction.focus_shown = true;
            interaction.pointer_hid_ring = false;
            answered.redraw = true;
        }
        if (finger && !menu_item_control(pressed.control)) {
            bool in_details = false;
            if (pane_at(list, at, in_details)) {
                const PaneScroll scroll =
                    in_details ? details_scroll_of(list) : list_scroll_of(list);
                if (scroll.ok) {
                    state.hold.kind = Hold::Kind::finger;
                    state.hold.from = at;
                    state.hold.offset = scroll.offset;
                    state.hold.in_details = in_details;
                }
            }
        }
        return answered;
    }
    case PointerKind::up: {
        if (button != 1)
            return {};
        if (state.bar != Bar::none) {
            end_hold(state);
            return redrawn();
        }
        if (state.hold.kind == Hold::Kind::slider) {
            end_hold(state);
            return redrawn();
        }
        if (state.hold.scrolling) {
            end_hold(state);
            return redrawn();
        }
        if (state.drag || state.hold.kind == Hold::Kind::grip) {
            if (!state.drag) {
                end_hold(state);
                return redrawn();
            }
            return release_drag(picker, state, list, at);
        }
        const kit::PointerOutcome released = kit::pointer_up(interaction, list, at);
        if (released.result == kit::PointerResult::activated)
            return activate(picker, state, list, released.control, released.at, finger_reach > 0);
        ScreenResult answered;
        answered.redraw = released.result != kit::PointerResult::none;
        return answered;
    }
    }
    return {};
}

ScreenResult
picker_key(Picker& picker, ScreenState& state, const kit::DisplayList& list, kit::Key pressed) {
    if (holding(state)) {
        if (pressed != kit::Key::escape)
            return {};
        end_hold(state);
        return redrawn();
    }
    repair_focus(state, list);
    if (state.menu != Menu::none) {
        if (const std::optional<ScreenResult> taken = menu_key(picker, state, list, pressed))
            return *taken;
    }
    // The first stop takes the keys from the first key, with the ring hidden,
    // unless the host already opened the screen (and may have asked for the ring).
    kit::focus_opened(state.interaction, list, false);
    kit::Interaction& interaction = state.interaction;
    if (pressed != kit::Key::escape && pressed != kit::Key::yes && pressed != kit::Key::no &&
        pressed != kit::Key::info)
        kit::show_ring_for_key(interaction);
    const kit::ControlId focus = interaction.focus_shown ? interaction.focused : kit::no_control;
    const bool on_search = focus == controls::search;
    const bool on_list = focus == controls::list;
    const bool in_details = details_focused(state, list);

    switch (pressed) {
    case kit::Key::escape:
        if (on_search && !state.search.text.empty()) {
            state.search = {};
            apply_query(picker, state, list);
            return redrawn();
        }
        if (page_open(state, list))
            return leave_page(state);
        {
            ScreenResult answered;
            answered.action = PickerAction::cancel;
            return answered;
        }
    case kit::Key::enter:
        if (on_search) {
            if (kit::control_of(list, controls::list) != nullptr)
                interaction.focused = controls::list;
            return redrawn();
        }
        if (on_list || focus == kit::no_control)
            return enter_list(picker, state, list);
        if (focus == controls::filter) {
            interaction.focus_shown = false;
            return toggle_menu(picker, state, list, Menu::filter, {});
        }
        if (focus == controls::tags) {
            interaction.focus_shown = false;
            return toggle_menu(picker, state, list, Menu::tags, {});
        }
        if (const std::optional<ScreenResult> stepped =
                step_key(picker, state, list, focus, pressed)) {
            interaction.focus_shown = false;
            return *stepped;
        }
        break;
    case kit::Key::space:
        if (on_list) {
            if (!picker.selected)
                return {};
            return flip_switch(picker, *picker.selected);
        }
        if (focus == controls::filter)
            return toggle_menu(picker, state, list, Menu::filter, {});
        if (focus == controls::tags)
            return toggle_menu(picker, state, list, Menu::tags, {});
        if (const std::optional<ScreenResult> stepped =
                step_key(picker, state, list, focus, pressed))
            return *stepped;
        break;
    case kit::Key::up:
    case kit::Key::down:
        if (on_list &&
            move_selection(picker, state, list, pressed == kit::Key::up ? -1 : 1, false, false))
            return redrawn();
        break;
    case kit::Key::left:
    case kit::Key::right:
        if (on_search) {
            if (kit::edit_text(state.search, pressed))
                return redrawn();
            break;
        }
        if (focus == controls::filter || focus == controls::tags)
            return cycle_field(picker, state, list, pressed == kit::Key::right);
        if (const std::optional<ScreenResult> stepped =
                step_key(picker, state, list, focus, pressed))
            return *stepped;
        break;
    case kit::Key::backspace:
    case kit::Key::delete_forward:
        if (on_search && kit::edit_text(state.search, pressed)) {
            apply_query(picker, state, list);
            return redrawn();
        }
        return {};
    case kit::Key::home:
    case kit::Key::end:
        if (on_search)
            return kit::edit_text(state.search, pressed) ? redrawn() : ScreenResult{};
        if (in_details)
            return scroll_details(state, list, pressed);
        if (kit::control_of(list, controls::list) == nullptr)
            return {};
        {
            const std::vector<Row> shown = shown_rows(picker);
            const int delta =
                pressed == kit::Key::home ? -as_int(shown.size()) : as_int(shown.size());
            std::ignore =
                move_selection(picker, state, list, delta, true, pressed == kit::Key::end);
        }
        return redrawn();
    case kit::Key::page_up:
    case kit::Key::page_down:
        if (in_details)
            return scroll_details(state, list, pressed);
        if (kit::control_of(list, controls::list) == nullptr)
            return {};
        {
            const int page_rows = rows_on_screen(list);
            const int delta = pressed == kit::Key::page_up ? -page_rows : page_rows;
            std::ignore = move_selection(picker, state, list, delta, true, false);
        }
        return redrawn();
    case kit::Key::yes:
    case kit::Key::no:
    case kit::Key::info:
        return {};
    case kit::Key::tab:
    case kit::Key::back_tab:
        break;
    }

    const kit::KeyOutcome outcome = kit::key(interaction, list, pressed);
    switch (outcome.result) {
    case kit::KeyResult::none:
        return {};
    case kit::KeyResult::redraw:
        show_in_details(state, list, interaction.focused);
        return redrawn();
    case kit::KeyResult::to_control:
        if (const std::optional<ScreenResult> stepped =
                step_key(picker, state, list, outcome.control, pressed))
            return *stepped;
        return activate(picker, state, list, outcome.control, std::nullopt, false);
    case kit::KeyResult::accept:
        return enter_list(picker, state, list);
    case kit::KeyResult::scroll:
    case kit::KeyResult::cancel:
        return {};
    }
    return {};
}

ScreenResult picker_text(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, std::string_view utf8
) {
    if (!focused_field(state, list) || !kit::insert_text(state.search, utf8))
        return {};
    apply_query(picker, state, list);
    return redrawn();
}

ScreenResult picker_wheel(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, kit::Point at, float notches
) {
    if (state.menu != Menu::none) {
        for (const kit::Item& item : list.items) {
            if (item.role != kit::Role::choice_menu || !kit::contains(item.rect, at))
                continue;
            if (!std::isfinite(notches) || notches > 1000000.f || notches < -1000000.f)
                return {};
            const int32_t count = menu_count(picker, state, list);
            const int32_t shown =
                kit::shown_choices(static_cast<std::size_t>(std::max(count, int32_t{0})));
            const auto turned = static_cast<int32_t>(std::lround(notches));
            const int32_t first = std::clamp(
                state.menu_first - turned, int32_t{0}, std::max(count - shown, int32_t{0})
            );
            if (first == state.menu_first)
                return {};
            state.menu_first = first;
            return redrawn();
        }
    }
    bool in_details = false;
    if (!pane_at(list, at, in_details))
        return {};
    const PaneScroll scroll = in_details ? details_scroll_of(list) : list_scroll_of(list);
    return turn_pane(state, scroll, in_details, notches);
}

ScreenResult picker_command(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, ScreenCommand command
) {
    if (holding(state))
        end_hold(state);
    switch (command) {
    case ScreenCommand::find:
        focus_search(state);
        return redrawn();
    case ScreenCommand::back:
        if (state.menu != Menu::none) {
            state.menu = Menu::none;
            return redrawn();
        }
        if (page_open(state, list))
            return leave_page(state);
        {
            ScreenResult answered;
            answered.action = PickerAction::cancel;
            return answered;
        }
    case ScreenCommand::flip:
        if (!picker.selected)
            return {};
        return flip_switch(picker, *picker.selected);
    case ScreenCommand::use:
        return use_set(list);
    }
    return {};
}

ScreenResult
picker_stick(Picker& picker, ScreenState& state, const kit::DisplayList& list, float notches) {
    static_cast<void>(picker);
    if (!std::isfinite(notches))
        return {};
    const bool in_details = details_focused(state, list);
    const PaneScroll scroll = in_details ? details_scroll_of(list) : list_scroll_of(list);
    return turn_pane(state, scroll, in_details, notches);
}

} // namespace oa::ui::mutator_picker
