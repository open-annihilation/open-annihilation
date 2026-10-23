// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain picker's input: the pointer and a finger, the keys, the wheel,
// and the controller's own buttons. What it knows of the screen it reads
// from the display list the screen shows. A choice, a level and an option
// change only through the model.
#include "oa/ui/brain_picker/screen.hpp"

#include "screen_controls.hpp"

#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/mutator_picker/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::ui::brain_picker {
namespace {

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
    constexpr auto most = static_cast<std::size_t>(std::numeric_limits<int32_t>::max());
    if (value > most)
        return std::numeric_limits<int32_t>::max();
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

/// Returns a result that asks the host to close or to use the choice.
///
/// @param action cancel or use
/// @return the result
ScreenResult answer(PickerAction action) {
    ScreenResult answered;
    answered.action = action;
    answered.redraw = true;
    return answered;
}

/// Tells whether two rectangles are the same place.
///
/// @param one one rectangle
/// @param other the other
/// @return true when they match
bool same_rect(const kit::Rect& one, const kit::Rect& other) {
    return one.x == other.x && one.y == other.y && one.width == other.width &&
           one.height == other.height;
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

/// Returns the list's rows. The vector is named so nothing points into a
/// temporary after the call that made it.
///
/// @param picker the picker
/// @return the rows
std::vector<Row> shown_rows(const Picker& picker) {
    return rows(picker);
}

/// Returns the selected brain's details. Named so nothing points into a
/// temporary.
///
/// @param picker the picker
/// @return the details
Details shown_details(const Picker& picker) {
    return details(picker);
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

/// Returns the brain id of a row. Empty is the built-in player. Nothing
/// when the control is not a row.
///
/// @param picker the picker
/// @param id the control
/// @return the id, or nothing
std::optional<std::string> row_id_of(const Picker& picker, kit::ControlId id) {
    const int32_t place = band_place(id, controls::first_row, controls::first_option);
    if (place < 0)
        return std::nullopt;
    const std::vector<Row> shown = shown_rows(picker);
    const auto index = static_cast<std::size_t>(place);
    if (index >= shown.size())
        return std::nullopt;
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
        if (band_place(control.id, controls::first_row, controls::first_option) >= 0 &&
            control.rect.height > 0)
            return control.rect.height;
    }
    return fallback_row_height;
}

/// Grows an extent to cover one rectangle.
///
/// @param[in,out] top the top so far
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
    return scroll_geometry(list, port, kit::compact_metrics.regular_line);
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
    const PaneScroll scroll = list_scroll_of(list);
    if (!scroll.ok)
        return;
    const std::vector<Row> shown = shown_rows(picker);
    std::optional<std::size_t> place;
    for (std::size_t index = 0; index < shown.size(); ++index) {
        if (shown[index].id == picker.selected)
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

/// Selects a row through the model. A new selection closes a menu and shows
/// the details from their top. The keys scroll the list until the row shows.
/// A pointer press does not, so the control under the pointer stays where it
/// was. The list takes the focus, so the arrows keep moving the selection.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the brain's id; empty is the built-in player
/// @param reveal true when the list scrolls the row into view
void select_brain(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, std::string id, bool reveal
) {
    const std::string before = picker.selected;
    std::ignore = select(picker, id);
    if (picker.selected != before) {
        state.details_scroll = 0;
        state.menu = Menu::none;
        state.menu_option.clear();
        state.menu_marked = -1;
        if (reveal)
            show_selected(picker, state, list);
    }
    if (kit::control_of(list, controls::list) != nullptr) {
        state.interaction.focused = controls::list;
        state.interaction.focus_shown = true;
    }
}

/// Ends a finger scroll, a slider hold and a scroll-bar hold. Changes
/// nothing in the model.
///
/// @param[in,out] state the screen's state
void end_hold(ScreenState& state) {
    state.hold = {};
    state.bar = Bar::none;
    state.interaction.pressed = kit::no_control;
    state.interaction.finger_shift = {};
}

/// Tells whether a press is holding the pointer.
///
/// @param state the screen's state
/// @return true while a finger, a slider or a scroll bar is held
bool holding(const ScreenState& state) {
    return state.hold.kind != Hold::Kind::none || state.bar != Bar::none;
}

/// The option place of a control, or nothing when it is not an option.
///
/// @param picker the picker
/// @param id the control
/// @return the place in the details' options
std::optional<std::size_t> option_place(const Picker& picker, kit::ControlId id) {
    const int32_t place = band_place(id, controls::first_option, controls::first_menu_item);
    if (place < 0)
        return std::nullopt;
    const Details shown = shown_details(picker);
    const auto index = static_cast<std::size_t>(place);
    if (index >= shown.options.size())
        return std::nullopt;
    return index;
}

/// Copies one option out of the details, so nothing keeps a pointer into them.
///
/// @param picker the picker
/// @param id the control
/// @return the option; nothing when the control is not one
std::optional<mutator_picker::OptionSpec> option_spec(const Picker& picker, kit::ControlId id) {
    const std::optional<std::size_t> place = option_place(picker, id);
    if (!place)
        return std::nullopt;
    const Details shown = shown_details(picker);
    if (*place >= shown.options.size())
        return std::nullopt;
    return shown.options[*place];
}

/// The value the details show for an option.
///
/// @param picker the picker
/// @param place the option's place
/// @return the value; empty when the details name none
std::string option_value(const Picker& picker, std::size_t place) {
    const Details shown = shown_details(picker);
    if (place >= shown.values.size())
        return {};
    return shown.values[place];
}

/// Sets the chosen brain's level when the strip offers it.
///
/// @param[in,out] picker the picker
/// @param index the level's place
/// @return what the host is to do
ScreenResult set_brain_level(Picker& picker, std::size_t index) {
    const Details shown = shown_details(picker);
    if (index >= shown.levels.size())
        return redrawn();
    std::ignore = set_level(picker, shown.levels[index].id);
    return redrawn();
}

/// Sets one option of the chosen brain.
///
/// @param[in,out] picker the picker
/// @param option the option's id
/// @param value the value
/// @return what the host is to do
ScreenResult set_brain_option(Picker& picker, std::string_view option, std::string_view value) {
    std::ignore = set_option(picker, option, value);
    return redrawn();
}

/// Flips a toggle option.
///
/// @param[in,out] picker the picker
/// @param id the control
/// @return what the host is to do
ScreenResult flip_toggle(Picker& picker, kit::ControlId id) {
    const std::optional<std::size_t> place = option_place(picker, id);
    const std::optional<mutator_picker::OptionSpec> spec = option_spec(picker, id);
    if (!place || !spec || spec->values.size() < 2)
        return redrawn();
    const std::string current = option_value(picker, *place);
    const std::string& value = current == spec->values[1] ? spec->values[0] : spec->values[1];
    return set_brain_option(picker, spec->id, value);
}

/// Sets a toggle to its left or right value.
///
/// @param[in,out] picker the picker
/// @param id the control
/// @param right true for the right value
/// @return what the host is to do
ScreenResult set_toggle(Picker& picker, kit::ControlId id, bool right) {
    const std::optional<mutator_picker::OptionSpec> spec = option_spec(picker, id);
    if (!spec || spec->values.empty())
        return {};
    const std::size_t index = right && spec->values.size() > 1 ? std::size_t{1} : std::size_t{0};
    return set_brain_option(picker, spec->id, spec->values[index]);
}

/// Steps a level strip or a choice strip by one, and stops at its ends.
///
/// @param[in,out] picker the picker
/// @param list the display list
/// @param id the control
/// @param toward_right true for the next value
/// @return what the host is to do
ScreenResult
step_strip(Picker& picker, const kit::DisplayList& list, kit::ControlId id, bool toward_right) {
    const kit::Item* item = nullptr;
    for (const kit::Item& one : list.items) {
        if (one.control == id && one.role == kit::Role::levels)
            item = &one;
    }
    const auto* look = item == nullptr ? nullptr : std::get_if<kit::LevelsLook>(&item->look);
    if (look == nullptr || look->captions.empty() || look->locked)
        return {};
    const std::size_t last = look->captions.size() - 1;
    std::size_t index = look->chosen < look->captions.size() ? look->chosen : std::size_t{0};
    if (toward_right) {
        if (index >= last)
            return {};
        ++index;
    } else if (index == 0) {
        return {};
    } else {
        --index;
    }
    if (id == controls::level)
        return set_brain_level(picker, index);
    const std::optional<mutator_picker::OptionSpec> spec = option_spec(picker, id);
    if (!spec || index >= spec->values.size())
        return {};
    return set_brain_option(picker, spec->id, spec->values[index]);
}

/// Moves a slider to a stop, clamped to the stops the option offers.
///
/// @param[in,out] picker the picker
/// @param id the control
/// @param stop the stop
/// @return what the host is to do
ScreenResult set_stop(Picker& picker, kit::ControlId id, int32_t stop) {
    const std::optional<mutator_picker::OptionSpec> spec = option_spec(picker, id);
    if (!spec)
        return {};
    const std::vector<std::string> stops = mutator_picker::number_values(*spec);
    if (stops.size() < 2)
        return {};
    const int32_t last = static_cast<int32_t>(stops.size()) - 1;
    const int32_t clamped = std::clamp(stop, int32_t{0}, last);
    return set_brain_option(picker, spec->id, stops[static_cast<std::size_t>(clamped)]);
}

/// Flips "Use for every computer seat".
///
/// @param[in,out] picker the picker
/// @return what the host is to do
ScreenResult flip_every(Picker& picker) {
    std::ignore = set_every_seat(picker, !picker.every_seat);
    return redrawn();
}

/// Moves the selected row through the list.
///
/// Past either end the move is refused, so the focus can stay on the list.
/// Home, End and the page keys clamp. With nothing selected, a downward key
/// and Home select the first row, and End selects the last.
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
    for (std::size_t index = 0; index < shown.size(); ++index) {
        if (shown[index].id == picker.selected)
            place = index;
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
    select_brain(picker, state, list, shown[next].id, true);
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
    state.menu_option.clear();
    state.menu_marked = -1;
    if (state.interaction.focus_shown)
        state.interaction.focused = controls::list;
    return redrawn();
}

/// The focus the details page takes when the keys opened it.
///
/// @param picker the picker
/// @return the control
kit::ControlId page_focus(const Picker& picker) {
    const Details shown = shown_details(picker);
    if (shown.level_note.empty() && !shown.levels.empty() && !shown.levels_locked)
        return controls::level;
    if (shown.chosen) {
        for (std::size_t place = 0; place < shown.options.size(); ++place) {
            const mutator_picker::OptionSpec& spec = shown.options[place];
            if (spec.type == mutator_picker::OptionSpec::Type::number &&
                mutator_picker::number_values(spec).size() < 2)
                continue;
            return controls::first_option + static_cast<kit::ControlId>(as_int(place));
        }
    }
    if (picker.inputs.offer_every_seat)
        return controls::every_seat;
    return controls::back;
}

/// Opens Compact's details page.
///
/// @param picker the picker
/// @param[in,out] state the screen's state
/// @return what the host is to do
ScreenResult open_page(const Picker& picker, ScreenState& state) {
    state.details_page = true;
    state.details_scroll = 0;
    state.menu = Menu::none;
    state.menu_option.clear();
    state.menu_marked = -1;
    if (state.interaction.focus_shown) {
        state.interaction.focused = page_focus(picker);
        state.interaction.focus_shown = true;
    }
    return redrawn();
}

/// The first details control in the layout's Tab order.
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

/// Puts the focus back on a control the layout still has, when the one it
/// held has gone.
///
/// @param[in,out] state the screen's state
/// @param list the display list
void repair_focus(ScreenState& state, const kit::DisplayList& list) {
    if (!state.interaction.focus_shown)
        return;
    const kit::Control* control = kit::control_of(list, state.interaction.focused);
    if (control != nullptr && control->enabled && control->focusable)
        return;
    if (list.tab_order.empty()) {
        state.interaction.focused = kit::no_control;
        state.interaction.focus_shown = false;
        return;
    }
    state.interaction.focused = list.tab_order.front();
}

/// The open menu's field.
///
/// @param picker the picker
/// @param state the screen's state
/// @return the control; none when no menu is open
kit::ControlId menu_field(const Picker& picker, const ScreenState& state) {
    if (state.menu != Menu::choice)
        return kit::no_control;
    const Details shown = shown_details(picker);
    for (std::size_t place = 0; place < shown.options.size(); ++place) {
        if (shown.options[place].id == state.menu_option)
            return controls::first_option + static_cast<kit::ControlId>(as_int(place));
    }
    return kit::no_control;
}

/// How many items the open menu has.
///
/// @param picker the picker
/// @param state the screen's state
/// @return the items
int32_t menu_count(const Picker& picker, const ScreenState& state) {
    const std::optional<mutator_picker::OptionSpec> spec = [&] {
        const Details shown = shown_details(picker);
        for (const mutator_picker::OptionSpec& option : shown.options) {
            if (option.id == state.menu_option)
                return std::optional<mutator_picker::OptionSpec>(option);
        }
        return std::optional<mutator_picker::OptionSpec>{};
    }();
    if (!spec)
        return 0;
    return as_int(spec->values.size());
}

/// Marks an item of the open menu, and scrolls the menu until it shows.
///
/// @param[in,out] state the screen's state
/// @param tally how many items the menu has
/// @param marked the item
void mark_item(ScreenState& state, int32_t tally, int32_t marked) {
    if (tally <= 0) {
        state.menu_marked = -1;
        return;
    }
    const int32_t shown = kit::shown_choices(static_cast<std::size_t>(tally));
    const int32_t index = std::clamp(marked, int32_t{0}, tally - 1);
    state.menu_marked = index;
    if (index < state.menu_first)
        state.menu_first = index;
    else if (index >= state.menu_first + shown)
        state.menu_first = index - shown + 1;
    const int32_t most = std::max(tally - shown, int32_t{0});
    state.menu_first = std::clamp(state.menu_first, int32_t{0}, most);
}

/// Chooses a marked item of the open menu and closes it.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param index the item
/// @return what the host is to do
ScreenResult choose_item(Picker& picker, ScreenState& state, int32_t index) {
    const Details shown = shown_details(picker);
    const mutator_picker::OptionSpec* spec = nullptr;
    for (const mutator_picker::OptionSpec& option : shown.options) {
        if (option.id == state.menu_option)
            spec = &option;
    }
    std::string option_id;
    std::string value;
    if (spec != nullptr && index >= 0 && static_cast<std::size_t>(index) < spec->values.size()) {
        option_id = spec->id;
        value = spec->values[static_cast<std::size_t>(index)];
    }
    state.menu = Menu::none;
    state.menu_option.clear();
    state.menu_marked = -1;
    if (!option_id.empty())
        std::ignore = set_option(picker, option_id, value);
    return redrawn();
}

/// Opens or closes an option's drop-down.
///
/// @param picker the picker
/// @param[in,out] state the screen's state
/// @param option the option's id
/// @return what the host is to do
ScreenResult toggle_menu(const Picker& picker, ScreenState& state, std::string option) {
    if (state.menu == Menu::choice && state.menu_option == option) {
        state.menu = Menu::none;
        state.menu_option.clear();
        state.menu_marked = -1;
        return redrawn();
    }
    const Details shown = shown_details(picker);
    std::size_t chosen = 0;
    bool found = false;
    for (std::size_t place = 0; place < shown.options.size(); ++place) {
        if (shown.options[place].id != option)
            continue;
        found = true;
        const std::string value = place < shown.values.size() ? shown.values[place] : std::string{};
        for (std::size_t slot = 0; slot < shown.options[place].values.size(); ++slot) {
            if (shown.options[place].values[slot] == value)
                chosen = slot;
        }
    }
    if (!found)
        return {};
    state.menu = Menu::choice;
    state.menu_option = std::move(option);
    state.menu_first = 0;
    mark_item(state, menu_count(picker, state), as_int(chosen));
    return redrawn();
}

/// A key while a drop-down is open. Nothing when this key is not the menu's.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param pressed the key
/// @return what the host is to do
std::optional<ScreenResult> menu_key(Picker& picker, ScreenState& state, kit::Key pressed) {
    const int32_t tally = menu_count(picker, state);
    if (tally <= 0) {
        state.menu = Menu::none;
        return redrawn();
    }
    const int32_t shown = kit::shown_choices(static_cast<std::size_t>(tally));
    int32_t marked = state.menu_marked;
    if (marked < 0 || marked >= tally)
        marked = 0;
    switch (pressed) {
    case kit::Key::escape:
        state.menu = Menu::none;
        state.menu_option.clear();
        state.menu_marked = -1;
        return redrawn();
    case kit::Key::enter:
    case kit::Key::space:
        return choose_item(picker, state, marked);
    case kit::Key::up:
    case kit::Key::left:
        mark_item(state, tally, marked == 0 ? 0 : marked - 1);
        return redrawn();
    case kit::Key::down:
    case kit::Key::right:
        mark_item(state, tally, marked + 1 >= tally ? tally - 1 : marked + 1);
        return redrawn();
    case kit::Key::home:
        mark_item(state, tally, 0);
        return redrawn();
    case kit::Key::end:
        mark_item(state, tally, tally - 1);
        return redrawn();
    case kit::Key::page_up:
        mark_item(state, tally, std::max(marked - shown, int32_t{0}));
        return redrawn();
    case kit::Key::page_down:
        mark_item(state, tally, std::min(marked + shown, tally - 1));
        return redrawn();
    case kit::Key::tab:
    case kit::Key::back_tab:
    case kit::Key::yes:
    case kit::Key::no:
    case kit::Key::info:
    case kit::Key::backspace:
    case kit::Key::delete_forward:
        return std::nullopt;
    }
    return std::nullopt;
}

/// The scroll bar under a point.
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

/// The scroll area of a held bar.
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
/// @param at the pointer
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
    const PaneScroll detail_scroll = details_scroll_of(list);
    if (detail_scroll.ok && kit::contains(detail_scroll.port, at)) {
        in_details = true;
        return true;
    }
    if (const kit::Control* bar = kit::control_of(list, controls::details_bar);
        bar != nullptr && kit::contains(bar->rect, at)) {
        in_details = true;
        return true;
    }
    if (const kit::Control* bar = kit::control_of(list, controls::list_bar);
        bar != nullptr && kit::contains(bar->rect, at))
        return true;
    return false;
}

/// The level of a strip under a point.
///
/// @param list the display list
/// @param id the strip
/// @param at the point
/// @return the level's index; nothing when the strip is missing
std::optional<std::size_t>
level_under(const kit::DisplayList& list, kit::ControlId id, kit::Point at) {
    for (const kit::Item& item : list.items) {
        if (item.control != id || item.role != kit::Role::levels)
            continue;
        const auto* look = std::get_if<kit::LevelsLook>(&item.look);
        if (look == nullptr || look->captions.empty())
            return std::nullopt;
        return kit::level_at(item.rect, look->captions.size(), look->level_width, at.x);
    }
    return std::nullopt;
}

/// The slider item of a control.
///
/// @param list the display list
/// @param id the control
/// @return the item; null when it is not a slider
const kit::Item* slider_item(const kit::DisplayList& list, kit::ControlId id) {
    for (const kit::Item& item : list.items) {
        if (item.control == id && item.role == kit::Role::slider)
            return &item;
    }
    return nullptr;
}

/// Drags a slider's knob to the column under the pointer.
///
/// @param[in,out] picker the picker
/// @param list the display list
/// @param id the slider
/// @param column the pointer's column
/// @return what the host is to do
ScreenResult
drag_slider(Picker& picker, const kit::DisplayList& list, kit::ControlId id, int32_t column) {
    const kit::Item* item = slider_item(list, id);
    const auto* look = item == nullptr ? nullptr : std::get_if<kit::SliderLook>(&item->look);
    if (look == nullptr || look->locked || look->stops < 2)
        return {};
    return set_stop(picker, id, kit::stop_at(item->rect, column, look->stops));
}

/// What a press on a control does, once it is released over that control.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the control
/// @param at the point of the release; none for a key
/// @return what the host is to do
ScreenResult activate(
    Picker& picker,
    ScreenState& state,
    const kit::DisplayList& list,
    kit::ControlId id,
    std::optional<kit::Point> at
) {
    if (menu_item_control(id))
        return choose_item(picker, state, id - controls::first_menu_item);
    if (const std::optional<std::string> row = row_id_of(picker, id)) {
        select_brain(picker, state, list, *row, false);
        return redrawn();
    }
    if (id == controls::level) {
        if (!at)
            return redrawn();
        const std::optional<std::size_t> index = level_under(list, id, *at);
        if (!index)
            return redrawn();
        return set_brain_level(picker, *index);
    }
    if (id == controls::every_seat)
        return flip_every(picker);
    if (const std::optional<std::size_t> place = option_place(picker, id)) {
        static_cast<void>(place);
        const kit::Control* control = kit::control_of(list, id);
        if (control == nullptr)
            return redrawn();
        if (control->kind == kit::ControlKind::levels && at) {
            const std::optional<std::size_t> index = level_under(list, id, *at);
            if (!index)
                return redrawn();
            const std::optional<mutator_picker::OptionSpec> spec = option_spec(picker, id);
            if (!spec || *index >= spec->values.size())
                return redrawn();
            return set_brain_option(picker, spec->id, spec->values[*index]);
        }
        if (control->kind == kit::ControlKind::toggle)
            return flip_toggle(picker, id);
        if (control->kind == kit::ControlKind::choice) {
            std::string option;
            if (const std::optional<mutator_picker::OptionSpec> spec = option_spec(picker, id))
                option = spec->id;
            return toggle_menu(picker, state, std::move(option));
        }
        return redrawn();
    }
    if (id == controls::details)
        return open_page(picker, state);
    if (id == controls::back)
        return leave_page(state);
    if (id == controls::cancel || id == controls::close_mark)
        return answer(PickerAction::cancel);
    if (id == controls::use)
        return answer(PickerAction::use);
    return redrawn();
}

/// A key on a details control that takes steps.
///
/// @param[in,out] picker the picker
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the control
/// @param pressed the key
/// @return what the host is to do; nothing when the key is not one this handles
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
    const bool levels = control->kind == kit::ControlKind::levels || id == controls::level;
    const bool slider = control->kind == kit::ControlKind::slider;
    const bool toggle = control->kind == kit::ControlKind::toggle || id == controls::every_seat;
    if (!choice && !levels && !slider && !toggle)
        return std::nullopt;
    if (pressed == kit::Key::left || pressed == kit::Key::right) {
        const bool toward_right = pressed == kit::Key::right;
        if (id == controls::every_seat) {
            std::ignore = set_every_seat(picker, toward_right);
            return redrawn();
        }
        if (levels)
            return step_strip(picker, list, id, toward_right);
        if (slider) {
            const kit::Item* item = slider_item(list, id);
            const auto* look =
                item == nullptr ? nullptr : std::get_if<kit::SliderLook>(&item->look);
            if (look == nullptr)
                return ScreenResult{};
            return set_stop(picker, id, look->stop + (toward_right ? 1 : -1));
        }
        if (toggle)
            return set_toggle(picker, id, toward_right);
        return ScreenResult{};
    }
    if (pressed == kit::Key::space || pressed == kit::Key::enter) {
        if (id == controls::every_seat)
            return flip_every(picker);
        if (toggle)
            return flip_toggle(picker, id);
        if (choice) {
            std::string option;
            if (const std::optional<mutator_picker::OptionSpec> spec = option_spec(picker, id))
                option = spec->id;
            return toggle_menu(picker, state, std::move(option));
        }
        return ScreenResult{};
    }
    return std::nullopt;
}

/// Tells whether page keys scroll the details.
///
/// @param state the screen's state
/// @param list the display list
/// @return true when they do
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
    show_in_details(state, list, detail);
    return redrawn();
}

} // namespace

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
        if (!inside_window(list, at) && state.menu == Menu::none && !holding(state))
            return {};
        const bool cancelled = holding(state);
        end_hold(state);
        if (state.menu != Menu::none) {
            const kit::ControlId under =
                finger_reach > 0 ? kit::reach(list, at, finger_reach).control : kit::hit(list, at);
            if (!menu_item_control(under) && under != menu_field(picker, state)) {
                state.menu = Menu::none;
                state.menu_option.clear();
                state.menu_marked = -1;
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
        if (control != nullptr && control->enabled && control->kind == kit::ControlKind::slider &&
            option_place(picker, control->id)) {
            state.hold.kind = Hold::Kind::slider;
            state.hold.control = control->id;
            state.hold.from = at;
            interaction.hovered = control->id;
            interaction.pressed = control->id;
            return drag_slider(picker, list, control->id, pressed.at.x);
        }
        if (!finger) {
            if (const std::optional<std::string> row = row_id_of(picker, pressed.control))
                select_brain(picker, state, list, *row, false);
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
        answered.redraw = true;
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
        const bool finger = finger_reach > 0;
        if (state.hold.kind == Hold::Kind::finger)
            end_hold(state);
        const kit::PointerOutcome released = kit::pointer_up(interaction, list, at);
        if (released.result == kit::PointerResult::activated)
            return activate(picker, state, list, released.control, released.at);
        ScreenResult answered;
        answered.redraw = released.result != kit::PointerResult::none || finger;
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
        if (const std::optional<ScreenResult> taken = menu_key(picker, state, pressed))
            return *taken;
    }
    kit::Interaction& interaction = state.interaction;
    const kit::ControlId focus = interaction.focus_shown ? interaction.focused : kit::no_control;
    const bool on_list = focus == controls::list;
    const bool in_details = details_focused(state, list);

    switch (pressed) {
    case kit::Key::escape:
        if (page_open(state, list))
            return leave_page(state);
        return answer(PickerAction::cancel);
    case kit::Key::enter:
        if (on_list || focus == kit::no_control)
            return enter_list(picker, state, list);
        if (const std::optional<ScreenResult> stepped =
                step_key(picker, state, list, focus, pressed))
            return *stepped;
        break;
    case kit::Key::space:
        if (on_list)
            return {};
        if (const std::optional<ScreenResult> stepped =
                step_key(picker, state, list, focus, pressed))
            return *stepped;
        break;
    case kit::Key::up:
    case kit::Key::down:
        if (on_list) {
            if (move_selection(picker, state, list, pressed == kit::Key::up ? -1 : 1, false, false))
                return redrawn();
            return {};
        }
        break;
    case kit::Key::left:
    case kit::Key::right:
        if (const std::optional<ScreenResult> stepped =
                step_key(picker, state, list, focus, pressed))
            return *stepped;
        break;
    case kit::Key::home:
    case kit::Key::end:
        if (on_list) {
            const std::vector<Row> shown = shown_rows(picker);
            const int delta =
                pressed == kit::Key::home ? -as_int(shown.size()) : as_int(shown.size());
            if (move_selection(picker, state, list, delta, true, pressed == kit::Key::end))
                return redrawn();
            return {};
        }
        if (in_details)
            return scroll_details(state, list, pressed);
        break;
    case kit::Key::page_up:
    case kit::Key::page_down:
        if (on_list) {
            const int page_rows = rows_on_screen(list);
            const int delta = pressed == kit::Key::page_up ? -page_rows : page_rows;
            if (move_selection(picker, state, list, delta, true, false))
                return redrawn();
            return {};
        }
        if (in_details)
            return scroll_details(state, list, pressed);
        break;
    case kit::Key::yes:
    case kit::Key::no:
    case kit::Key::info:
    case kit::Key::backspace:
    case kit::Key::delete_forward:
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
        return activate(picker, state, list, outcome.control, std::nullopt);
    case kit::KeyResult::accept:
        return enter_list(picker, state, list);
    case kit::KeyResult::scroll:
    case kit::KeyResult::cancel:
        return {};
    }
    return {};
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
            const int32_t tally = menu_count(picker, state);
            const int32_t shown =
                kit::shown_choices(static_cast<std::size_t>(std::max(tally, int32_t{0})));
            const auto turned = static_cast<int32_t>(std::lround(notches));
            const int32_t first = std::clamp(
                state.menu_first - turned, int32_t{0}, std::max(tally - shown, int32_t{0})
            );
            if (first == state.menu_first)
                return {};
            state.menu_first = first;
            return redrawn();
        }
    }
    static_cast<void>(picker);
    bool in_details = false;
    if (!pane_at(list, at, in_details))
        return {};
    const PaneScroll scroll = in_details ? details_scroll_of(list) : list_scroll_of(list);
    return turn_pane(state, scroll, in_details, notches);
}

ScreenResult picker_command(
    Picker& picker, ScreenState& state, const kit::DisplayList& list, ScreenCommand command
) {
    static_cast<void>(picker);
    switch (command) {
    case ScreenCommand::back:
        if (holding(state)) {
            end_hold(state);
            return redrawn();
        }
        if (state.menu != Menu::none) {
            state.menu = Menu::none;
            state.menu_option.clear();
            state.menu_marked = -1;
            return redrawn();
        }
        if (page_open(state, list))
            return leave_page(state);
        return answer(PickerAction::cancel);
    case ScreenCommand::use:
        return answer(PickerAction::use);
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

} // namespace oa::ui::brain_picker
