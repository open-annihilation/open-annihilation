// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor's pointer, keys, typed search, wheel and controller.
// The host passes what the platform produced. This file changes the model
// and the screen state, and tells the host when to ask or to close.
#include "oa/ui/key_editor/screen.hpp"

#include "list_geom.hpp"
#include "menu_geom.hpp"
#include "screen_controls.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/input.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::ui::key_editor {
namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;

namespace {

/// A result that lays the screen out again.
///
/// @return the result
EditorResult redrawn() {
    EditorResult result;
    result.redraw = true;
    return result;
}

/// Asks the host something, and lays the screen out again when a menu closed.
///
/// @param action what the host is to do
/// @param id the layout, or "duplicate" or "rename"
/// @param draw true when the screen changed as well
/// @return the result
EditorResult asked(EditorAction action, std::string id, bool draw) {
    EditorResult result;
    result.action = action;
    result.id = std::move(id);
    result.redraw = draw;
    return result;
}

/// Leaves the editor, asking first while the draft differs from what was opened.
///
/// @param editor the model
/// @return cancel, or ask_leave
EditorResult leave(const Editor& editor) {
    if (editor.unsaved() > 0)
        return asked(EditorAction::ask_leave, {}, false);
    return asked(EditorAction::cancel, {}, false);
}

/// True at Compact, where the filters live in the category menu and there is no nav.
///
/// @param list the display list
/// @return true at Compact, including its details page
bool compact_list(const kit::DisplayList& list) {
    return kit::control_of(list, controls::filter_all) == nullptr &&
           kit::control_of(list, controls::nav_first) == nullptr;
}

/// The size class the list was laid out for, read from which controls it has.
///
/// @param list the display list
/// @return the class
kit::SizeClass size_of(const kit::DisplayList& list) {
    if (kit::control_of(list, controls::nav_first) != nullptr)
        return kit::SizeClass::large;
    if (kit::control_of(list, controls::filter_all) != nullptr)
        return kit::SizeClass::regular;
    return kit::SizeClass::compact;
}

/// How far two columns or rows are, as a positive distance.
///
/// @param from one coordinate
/// @param to the other
/// @return the distance, in points
int32_t apart(int32_t from, int32_t to) {
    const int32_t delta = from - to;
    return delta < 0 ? -delta : delta;
}

/// True when a control id is one item of the open menu.
///
/// @param id the control
/// @return true for a menu item
bool menu_item(kit::ControlId id) {
    return id >= controls::menu_first;
}

/// True when a control id removes one chord in the details.
///
/// @param id the control
/// @return true for a remove control
bool chord_remove(kit::ControlId id) {
    return id >= controls::chord_first && id < controls::chord_row_first;
}

/// True when a control id is a chord Enter changes.
///
/// @param id the control
/// @return true for a chord that is not a fixed Escape
bool chord_row(kit::ControlId id) {
    return id >= controls::chord_row_first && id < controls::menu_first;
}

/// True when a control id is one of the details' own controls.
///
/// @param id the control
/// @return true for Latch, Hold, Reset, a chord, + ADD KEY or the clash card
bool detail_control(kit::ControlId id) {
    return id == controls::detail_latch || id == controls::detail_confirm ||
           id == controls::detail_reset || id == controls::detail_add ||
           id == controls::card_here || id == controls::card_there || id == controls::card_choose ||
           chord_remove(id) || chord_row(id);
}

void close_prompt(ScreenState& state);
void focus_finished(const Editor& editor, ScreenState& state);
void start_capture(const Editor& editor, ScreenState& state, std::optional<std::size_t> replacing);
EditorResult finish_apply(Editor& editor, ScreenState& state, ClashChoice choice);
EditorResult press_ok(Editor& editor, ScreenState& state);
std::optional<std::size_t>
index_of_chord(const Editor& editor, std::string_view id, const keys::Chord& chord);

/// The first details control in Tab's order.
///
/// @param list the display list
/// @return the control, or none
kit::ControlId first_detail(const kit::DisplayList& list) {
    for (const kit::ControlId id : list.tab_order) {
        if (detail_control(id))
            return id;
    }
    return kit::no_control;
}

/// The filter a menu item names.
///
/// @param id the item's id
/// @return the filter; All when the id is not one of the five
Filter filter_named(std::string_view id) {
    if (id == "changed")
        return Filter::changed;
    if (id == "clashes")
        return Filter::clashes;
    if (id == "unbound")
        return Filter::unbound;
    if (id == "latchable")
        return Filter::latchable;
    return Filter::all;
}

/// The list's scroll area, from the slots and the list control.
///
/// @param editor the editor
/// @param state the scroll
/// @param list the display list
/// @return the area; none while the list is not shown
std::optional<kit::ScrollArea>
list_area(const Editor& editor, const ScreenState& state, const kit::DisplayList& list) {
    const kit::Control* port = kit::control_of(list, controls::list);
    if (port == nullptr || port->rect.height <= 0)
        return std::nullopt;
    const std::vector<Slot> slots = list_slots(editor, state);
    const int32_t row_h = list_row_height(size_of(list));
    const int32_t content = static_cast<int32_t>(slots.size()) * row_h;
    kit::ScrollArea area;
    area.view = port->rect;
    if (const kit::Control* bar = kit::control_of(list, controls::list_bar))
        area.well = bar->rect;
    area.content_height = content;
    area.limit = std::max(content - port->rect.height, int32_t{0});
    area.offset = std::clamp(state.list_scroll, int32_t{0}, area.limit);
    return area;
}

/// The scroll bar's look, matched to the bar's rectangle.
///
/// @param list the display list
/// @param well the bar
/// @return the look, or null
const kit::ScrollBarLook* bar_look(const kit::DisplayList& list, const kit::Rect& well) {
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::scroll_bar || item.rect.x != well.x || item.rect.y != well.y ||
            item.rect.width != well.width || item.rect.height != well.height)
            continue;
        if (const auto* look = std::get_if<kit::ScrollBarLook>(&item.look))
            return look;
    }
    return nullptr;
}

/// The details' scroll area, read back from the bar the layout drew.
///
/// @param state the scroll
/// @param list the display list
/// @return the area; none while the details do not scroll
std::optional<kit::ScrollArea>
details_area(const ScreenState& state, const kit::DisplayList& list) {
    const kit::Control* bar = kit::control_of(list, controls::details_bar);
    if (bar == nullptr || bar->rect.height <= 0)
        return std::nullopt;
    const kit::ScrollBarLook* look = bar_look(list, bar->rect);
    if (look == nullptr || look->thumb.height <= 0)
        return std::nullopt;
    const int32_t least = kit::metrics_of(size_of(list)).least_thumb_height;
    const int32_t view_h = bar->rect.height;
    int32_t low = view_h;
    int32_t high = view_h * 40;
    if (high < low)
        high = low;
    while (low < high) {
        const int32_t mid = low + (high - low) / 2;
        kit::ScrollArea probe;
        probe.well = bar->rect;
        probe.view = {0, bar->rect.y, 1, view_h};
        probe.content_height = mid;
        probe.limit = std::max(mid - view_h, int32_t{0});
        probe.offset = 0;
        if (probe.thumb(least).height > look->thumb.height)
            low = mid + 1;
        else
            high = mid;
    }
    kit::ScrollArea area;
    area.well = bar->rect;
    area.view = {bar->rect.x - 1, bar->rect.y, 1, view_h};
    area.content_height = low;
    area.limit = std::max(low - view_h, int32_t{0});
    area.offset = std::clamp(state.details_scroll, int32_t{0}, area.limit);
    return area;
}

/// Scrolls a pane to an offset.
///
/// @param[in,out] state the screen's state
/// @param details true for the details
/// @param offset the offset, already within the limit
void scroll_to(ScreenState& state, bool details, int32_t offset) {
    if (details)
        state.details_scroll = offset;
    else
        state.list_scroll = offset;
}

/// Holds a scroll bar, or jumps its thumb when the press missed it.
///
/// @param[in,out] state the screen's state
/// @param editor the list's content
/// @param list the display list
/// @param details true for the details' bar
/// @param at the pointer
/// @return true when the bar is held
bool take_bar(
    ScreenState& state,
    const Editor& editor,
    const kit::DisplayList& list,
    bool details,
    kit::Point at
) {
    const std::optional<kit::ScrollArea> area =
        details ? details_area(state, list) : list_area(editor, state, list);
    if (!area || area->limit <= 0 || area->well.height <= 0)
        return false;
    const int32_t least = kit::metrics_of(size_of(list)).least_thumb_height;
    const kit::Rect thumb = area->thumb(least);
    state.bar = details ? ScreenState::Bar::details : ScreenState::Bar::list;
    state.interaction.pressed = kit::no_control;
    if (kit::contains(thumb, at)) {
        state.bar_grab = at.y - thumb.y;
    } else {
        state.bar_grab = thumb.height / 2;
        scroll_to(state, details, area->offset_at(at.y - state.bar_grab, least));
    }
    return true;
}

/// Drags the held scroll bar.
///
/// @param[in,out] state the screen's state
/// @param editor the list's content
/// @param list the display list
/// @param at the pointer
/// @return true when the offset moved
bool move_bar(
    ScreenState& state, const Editor& editor, const kit::DisplayList& list, kit::Point at
) {
    const bool details = state.bar == ScreenState::Bar::details;
    const std::optional<kit::ScrollArea> area =
        details ? details_area(state, list) : list_area(editor, state, list);
    if (!area)
        return false;
    const int32_t least = kit::metrics_of(size_of(list)).least_thumb_height;
    const int32_t next = area->offset_at(at.y - state.bar_grab, least);
    if (next == area->offset)
        return false;
    scroll_to(state, details, next);
    return true;
}

/// The row under a point.
///
/// @param editor the editor
/// @param state the list
/// @param list the display list
/// @param at the point
/// @return the action id; empty when the point misses every row
std::string row_at(
    const Editor& editor, const ScreenState& state, const kit::DisplayList& list, kit::Point at
) {
    const kit::ControlId id = kit::hit(list, at);
    if (id < controls::row_first || id >= controls::chord_first)
        return {};
    const std::vector<Slot> slots = list_slots(editor, state);
    const auto index = static_cast<std::size_t>(id - controls::row_first);
    if (index >= slots.size() || slots[index].kind != Slot::Kind::row)
        return {};
    return slots[index].id;
}

/// True when a point lies in the list's view.
///
/// @param list the display list
/// @param at the point
/// @return true when it does
bool in_list(const kit::DisplayList& list, kit::Point at) {
    const kit::Control* port = kit::control_of(list, controls::list);
    return port != nullptr && kit::contains(port->rect, at);
}

/// Puts the focus on a control and shows it.
///
/// @param[in,out] state the screen's state
/// @param id the control
void focus_on(ScreenState& state, kit::ControlId id) {
    state.interaction.focused = id;
    state.interaction.focus_shown = true;
}

/// Scrolls the list so one slot is wholly in view.
///
/// @param[in,out] state the scroll
/// @param editor the slots
/// @param list the display list
/// @param slot the slot's place, from 0
void reveal_slot(
    ScreenState& state, const Editor& editor, const kit::DisplayList& list, int32_t slot
) {
    const std::optional<kit::ScrollArea> area = list_area(editor, state, list);
    if (!area)
        return;
    const int32_t row_h = list_row_height(size_of(list));
    if (row_h <= 0)
        return;
    const int32_t top = slot * row_h;
    int32_t offset = area->offset;
    if (top < offset)
        offset = top;
    else if (top + row_h > offset + area->view.height)
        offset = top + row_h - area->view.height;
    state.list_scroll = std::clamp(offset, int32_t{0}, area->limit);
}

/// Moves the chosen row and keeps the focus on the list.
///
/// @param[in,out] state the chosen row and the scroll
/// @param editor the rows
/// @param list the display list
/// @param step how many rows to move; ignored for home and end
/// @param home true to go to the first row
/// @param end true to go to the last row
/// @return true when the chosen row or the scroll changed
bool move_row(
    ScreenState& state,
    const Editor& editor,
    const kit::DisplayList& list,
    int32_t step,
    bool home,
    bool end
) {
    const std::vector<Slot> slots = list_slots(editor, state);
    std::vector<int32_t> rows;
    int32_t current = -1;
    for (int32_t index = 0; index < static_cast<int32_t>(slots.size()); ++index) {
        if (slots[static_cast<std::size_t>(index)].kind != Slot::Kind::row)
            continue;
        if (slots[static_cast<std::size_t>(index)].id == state.selected)
            current = static_cast<int32_t>(rows.size());
        rows.push_back(index);
    }
    if (rows.empty())
        return false;
    const int32_t last = static_cast<int32_t>(rows.size()) - 1;
    int32_t next = current;
    if (home)
        next = 0;
    else if (end)
        next = last;
    else if (current < 0)
        next = step < 0 ? last : 0;
    else
        next = std::clamp(current + step, int32_t{0}, last);
    const int32_t slot = rows[static_cast<std::size_t>(next)];
    const std::string id = slots[static_cast<std::size_t>(slot)].id;
    state.selected = id;
    focus_on(state, controls::list);
    reveal_slot(state, editor, list, slot);
    return true;
}

/// How many rows the list shows, at least one.
///
/// @param editor the list
/// @param state the scroll
/// @param list the display list
/// @return the rows
int32_t rows_shown(const Editor& editor, const ScreenState& state, const kit::DisplayList& list) {
    const std::optional<kit::ScrollArea> area = list_area(editor, state, list);
    const int32_t row_h = list_row_height(size_of(list));
    if (!area || row_h <= 0)
        return 1;
    return std::max(area->view.height / row_h, int32_t{1});
}

/// Closes the open menu.
///
/// @param[in,out] state the screen's state
void close_menu(ScreenState& state) {
    state.menu = Menu::none;
    state.menu_marked = -1;
    state.menu_first = 0;
}

/// Keeps the marked menu item among the items the menu shows.
///
/// @param[in,out] state the menu
/// @param count how many items the menu has
void show_marked(ScreenState& state, std::size_t count) {
    const int32_t shown = std::max(kit::shown_choices(count), int32_t{1});
    const int32_t last_first = std::max(static_cast<int32_t>(count) - shown, int32_t{0});
    if (state.menu_marked < state.menu_first)
        state.menu_first = std::max(state.menu_marked, int32_t{0});
    if (state.menu_marked >= state.menu_first + shown)
        state.menu_first = state.menu_marked - shown + 1;
    state.menu_first = std::clamp(state.menu_first, int32_t{0}, last_first);
}

/// The choices a menu offers, skipping dividers.
///
/// @param entries the menu
/// @return each choice's place, from 0
std::vector<int32_t> choice_places(const std::vector<MenuEntry>& entries) {
    std::vector<int32_t> places;
    for (int32_t index = 0; index < static_cast<int32_t>(entries.size()); ++index) {
        if (entries[static_cast<std::size_t>(index)].kind != MenuKind::divider)
            places.push_back(index);
    }
    return places;
}

/// What choosing one menu item does.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param entry the item
/// @return what the host is to do
EditorResult choose_entry(Editor& editor, ScreenState& state, const MenuEntry& entry) {
    if (entry.kind == MenuKind::divider)
        return {};
    if (entry.kind == MenuKind::duplicate || entry.kind == MenuKind::rename) {
        close_menu(state);
        return asked(EditorAction::ask_name, entry.id, true);
    }
    if (entry.kind == MenuKind::layout_id) {
        if (entry.id != editor.layout_id() && editor.unsaved() > 0) {
            close_menu(state);
            return asked(EditorAction::ask_keep_changes, entry.id, true);
        }
        if (entry.id != editor.layout_id())
            editor.choose_layout(entry.id, KeepChanges::keep);
        close_menu(state);
        state.list_scroll = 0;
        return redrawn();
    }
    if (entry.kind == MenuKind::category) {
        if (entry.id.rfind("heading.", 0) != 0) {
            state.category = entry.id;
            state.list_scroll = 0;
        }
        close_menu(state);
        return redrawn();
    }
    if (entry.kind == MenuKind::filter) {
        state.filter = filter_named(entry.id);
        state.list_scroll = 0;
        close_menu(state);
        return redrawn();
    }
    if (entry.kind == MenuKind::restore) {
        editor.restore_defaults();
        close_menu(state);
        state.list_scroll = 0;
        return redrawn();
    }
    close_menu(state);
    return redrawn();
}

/// Chooses the menu item a control id names.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the item's control
/// @return what the host is to do
EditorResult choose_menu_id(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, kit::ControlId id
) {
    const std::vector<MenuEntry> entries =
        menu_entries(editor, state.menu, state, compact_list(list));
    const auto index = static_cast<std::size_t>(id - controls::menu_first);
    if (index >= entries.size())
        return {};
    return choose_entry(editor, state, entries[index]);
}

/// Opens a menu, or closes it when that menu is already open.
///
/// @param[in,out] state the screen's state
/// @param editor which item is chosen
/// @param list the display list
/// @param menu which menu
void toggle_menu(
    ScreenState& state, const Editor& editor, const kit::DisplayList& list, Menu menu
) {
    if (state.menu == menu) {
        close_menu(state);
        return;
    }
    state.menu = menu;
    const std::vector<MenuEntry> entries = menu_entries(editor, menu, state, compact_list(list));
    state.menu_marked = menu_chosen(editor, menu, state, entries);
    if (state.menu_marked < 0) {
        const std::vector<int32_t> places = choice_places(entries);
        state.menu_marked = places.empty() ? -1 : places.front();
    }
    state.menu_first = 0;
    show_marked(state, entries.size());
}

/// The control a press is on, when it is the field that opened the menu.
///
/// @param menu which menu
/// @param id the control
/// @return true for that field
bool menu_opener(Menu menu, kit::ControlId id) {
    if (menu == Menu::layout)
        return id == controls::layout_choice;
    if (menu == Menu::category)
        return id == controls::category;
    if (menu == Menu::more)
        return id == controls::more;
    return false;
}

/// Acts on a control the pointer or a key chose.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the control
/// @return what the host is to do
EditorResult
activate(Editor& editor, ScreenState& state, const kit::DisplayList& list, kit::ControlId id) {
    if (menu_item(id))
        return choose_menu_id(editor, state, list, id);
    if (id == controls::layout_choice) {
        toggle_menu(state, editor, list, Menu::layout);
        return redrawn();
    }
    if (id == controls::category) {
        toggle_menu(state, editor, list, Menu::category);
        return redrawn();
    }
    if (id == controls::more) {
        toggle_menu(state, editor, list, Menu::more);
        return redrawn();
    }
    if (id == controls::filter_all || id == controls::filter_changed ||
        id == controls::filter_clashes || id == controls::filter_unbound ||
        id == controls::filter_latchable) {
        if (id == controls::filter_all)
            state.filter = Filter::all;
        else if (id == controls::filter_changed)
            state.filter = Filter::changed;
        else if (id == controls::filter_clashes)
            state.filter = Filter::clashes;
        else if (id == controls::filter_unbound)
            state.filter = Filter::unbound;
        else
            state.filter = Filter::latchable;
        state.list_scroll = 0;
        return redrawn();
    }
    if (id >= controls::nav_first && id < controls::row_first) {
        const std::vector<CategoryEntry> categories = editor.categories();
        const auto index = static_cast<std::size_t>(id - controls::nav_first);
        if (index < categories.size() && categories[index].id.rfind("heading.", 0) != 0) {
            state.category = categories[index].id;
            state.list_scroll = 0;
        }
        return redrawn();
    }
    if (id == controls::number_keys) {
        editor.set_number_keys(!editor.number_keys());
        return redrawn();
    }
    if (id >= controls::row_first && id < controls::chord_first) {
        const std::vector<Slot> slots = list_slots(editor, state);
        const auto index = static_cast<std::size_t>(id - controls::row_first);
        if (index < slots.size() && slots[index].kind == Slot::Kind::row) {
            state.selected = slots[index].id;
            state.details_scroll = 0;
            focus_on(state, controls::list);
        }
        return redrawn();
    }
    if (chord_remove(id)) {
        editor.remove_chord(state.selected, static_cast<std::size_t>(id - controls::chord_first));
        return redrawn();
    }
    if (id == controls::detail_latch) {
        const Details action = editor.details(state.selected);
        if (action.latch_offered)
            editor.set_latch(state.selected, !action.latch_on);
        return redrawn();
    }
    if (id == controls::detail_confirm) {
        const Details action = editor.details(state.selected);
        if (action.confirm_offered)
            editor.set_confirm(state.selected, !action.confirm_on);
        return redrawn();
    }
    if (id == controls::detail_reset) {
        editor.reset_action(state.selected);
        return redrawn();
    }
    if (id == controls::restore) {
        editor.restore_defaults();
        state.list_scroll = 0;
        return redrawn();
    }
    if (id == controls::back) {
        state.details_page = false;
        return redrawn();
    }
    if (id == controls::cancel || id == controls::close_mark)
        return leave(editor);
    if (id == controls::save) {
        if (!editor.can_save())
            return {};
        return asked(EditorAction::save, {}, false);
    }
    if (id == controls::detail_add) {
        if (state.selected.empty() || state.prompt != Prompt::none)
            return {};
        const Details action = editor.details(state.selected);
        if (action.read_only || action.chords.size() >= 4)
            return {};
        start_capture(editor, state, std::nullopt);
        return redrawn();
    }
    if (chord_row(id)) {
        const auto at = static_cast<std::size_t>(id - controls::chord_row_first);
        const Details action = editor.details(state.selected);
        if (at >= action.chords.size() || action.chords[at].fixed)
            return {};
        start_capture(editor, state, at);
        return redrawn();
    }
    if (id == controls::capture_cancel || id == controls::clash_cancel) {
        close_prompt(state);
        return redrawn();
    }
    if (id == controls::capture_ok)
        return press_ok(editor, state);
    if (id == controls::clash_replace)
        return finish_apply(editor, state, ClashChoice::replace);
    if (id == controls::clash_swap)
        return finish_apply(editor, state, ClashChoice::swap);
    if (id == controls::capture_letter || id == controls::capture_place) {
        set_means(
            state.capture,
            editor,
            id == controls::capture_letter ? acts::By::letter : acts::By::place
        );
        focus_finished(editor, state);
        return redrawn();
    }
    if (id == controls::capture_press || id == controls::capture_double ||
        id == controls::capture_hold) {
        keys::Gesture gesture = keys::Gesture::press;
        if (id == controls::capture_double)
            gesture = keys::Gesture::double_press;
        else if (id == controls::capture_hold)
            gesture = keys::Gesture::hold;
        set_gesture(state.capture, editor, gesture);
        focus_finished(editor, state);
        return redrawn();
    }
    if (id == controls::card_here || id == controls::card_there) {
        const std::optional<keys::Clash> clash = editor.draft_clash(state.selected);
        if (!clash)
            return {};
        settle(editor, *clash, id == controls::card_here ? Settle::keep_here : Settle::keep_there);
        state.details_scroll = 0;
        return redrawn();
    }
    if (id == controls::card_choose) {
        const std::optional<keys::Clash> clash = editor.draft_clash(state.selected);
        if (!clash)
            return {};
        const std::optional<std::size_t> at = index_of_chord(editor, state.selected, clash->chord);
        if (!at)
            return {};
        start_capture(editor, state, at);
        return redrawn();
    }
    if (id == controls::search_chord) {
        state.search_chord.reset();
        state.list_scroll = 0;
        return redrawn();
    }
    if (id == controls::search) {
        focus_on(state, controls::search);
        return redrawn();
    }
    if (id == controls::list) {
        focus_on(state, controls::list);
        return redrawn();
    }
    return {};
}

/// Enter or Space on the list: Compact's details page, or the details' first control.
///
/// @param[in,out] state the page or the focus
/// @param list the display list
/// @return what the host is to do
EditorResult open_details(ScreenState& state, const kit::DisplayList& list) {
    if (compact_list(list)) {
        state.details_page = true;
        state.details_scroll = 0;
        return redrawn();
    }
    const kit::ControlId detail = first_detail(list);
    if (detail == kit::no_control)
        return {};
    focus_on(state, detail);
    return redrawn();
}

/// A key on the open menu: the arrows, Home, End, Page, Enter, Space and Escape.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param pressed the key
/// @return what the host is to do
EditorResult
menu_key(Editor& editor, ScreenState& state, const kit::DisplayList& list, kit::Key pressed) {
    if (pressed == kit::Key::escape) {
        close_menu(state);
        return redrawn();
    }
    const std::vector<MenuEntry> entries =
        menu_entries(editor, state.menu, state, compact_list(list));
    const std::vector<int32_t> places = choice_places(entries);
    if (places.empty())
        return {};
    int32_t current = 0;
    bool found = false;
    for (int32_t index = 0; index < static_cast<int32_t>(places.size()); ++index) {
        if (places[static_cast<std::size_t>(index)] == state.menu_marked) {
            current = index;
            found = true;
        }
    }
    const int32_t last = static_cast<int32_t>(places.size()) - 1;
    if (pressed == kit::Key::enter || pressed == kit::Key::space) {
        if (!found)
            return {};
        return choose_entry(
            editor,
            state,
            entries[static_cast<std::size_t>(places[static_cast<std::size_t>(current)])]
        );
    }
    int32_t next = found ? current : 0;
    if (pressed == kit::Key::home)
        next = 0;
    else if (pressed == kit::Key::end)
        next = last;
    else if (pressed == kit::Key::up)
        next = std::clamp((found ? current : 0) - 1, int32_t{0}, last);
    else if (pressed == kit::Key::down)
        next = std::clamp((found ? current : -1) + 1, int32_t{0}, last);
    else if (pressed == kit::Key::page_up || pressed == kit::Key::page_down) {
        const int32_t page = std::max(kit::shown_choices(entries.size()), int32_t{1});
        const int32_t from = found ? current : (pressed == kit::Key::page_down ? -1 : last + 1);
        const int32_t step = pressed == kit::Key::page_down ? page : -page;
        next = std::clamp(from + step, int32_t{0}, last);
    } else {
        return {};
    }
    state.menu_marked = places[static_cast<std::size_t>(next)];
    show_marked(state, entries.size());
    return redrawn();
}

/// Escape: close a menu, clear a search, leave the details page, or leave the editor.
///
/// @param[in,out] editor the model
/// @param[in,out] state the screen's state
/// @return what the host is to do
EditorResult escape_key(Editor& editor, ScreenState& state) {
    if (state.menu != Menu::none) {
        close_menu(state);
        return redrawn();
    }
    if (state.search_chord) {
        state.search_chord.reset();
        state.list_scroll = 0;
        return redrawn();
    }
    if (!state.search.text.empty()) {
        state.search.text.clear();
        state.search.caret = 0;
        state.list_scroll = 0;
        return redrawn();
    }
    if (state.details_page) {
        state.details_page = false;
        return redrawn();
    }
    return leave(editor);
}

/// True when the focused control is one of the details.
///
/// @param state the focus
/// @return true when the details should take a stick
bool details_focused(const ScreenState& state) {
    return state.details_page || detail_control(state.interaction.focused);
}

/// Scrolls one pane by the wheel.
///
/// @param[in,out] state the scroll and the carry
/// @param editor the list's content
/// @param list the display list
/// @param details true for the details
/// @param notches the turn
/// @return what the host is to do
EditorResult turn_pane(
    ScreenState& state,
    const Editor& editor,
    const kit::DisplayList& list,
    bool details,
    float notches
) {
    if (!std::isfinite(notches))
        return {};
    const std::optional<kit::ScrollArea> area =
        details ? details_area(state, list) : list_area(editor, state, list);
    if (!area || area->limit <= 0)
        return {};
    const int32_t step = details ? std::max(kit::metrics_of(size_of(list)).small_line, int32_t{1})
                                 : std::max(list_row_height(size_of(list)), int32_t{1});
    kit::WheelCarry& carry = details ? state.details_wheel : state.list_wheel;
    const int32_t next = kit::wheel_offset(carry, notches, step, area->offset, area->limit);
    if (next == area->offset)
        return {};
    scroll_to(state, details, next);
    return redrawn();
}

/// A finger pressed in a pane. A drag scrolls it. A tap chooses the row under it.
///
/// @param[in,out] editor the model
/// @param[in,out] state the finger
/// @param list the display list
/// @param kind a move, a press or a release
/// @param button the pointer's button
/// @param at the finger
/// @param reach how far a tap may move before it scrolls
/// @return what the host is to do; none when this was not the finger
EditorResult finger_pointer(
    Editor& editor,
    ScreenState& state,
    const kit::DisplayList& list,
    PointerKind kind,
    uint8_t button,
    kit::Point at,
    int32_t reach
) {
    if (kind == PointerKind::down && button == 1) {
        const kit::ControlId under = kit::hit(list, at);
        if (under == controls::list_bar || under == controls::details_bar) {
            if (take_bar(state, editor, list, under == controls::details_bar, at))
                return redrawn();
            return {};
        }
        state.finger = true;
        state.finger_origin = at;
        state.finger_at = at;
        state.finger_moved = false;
        state.finger_in_details = !in_list(list, at);
        state.finger_scroll = state.finger_in_details ? state.details_scroll : state.list_scroll;
        state.finger_row = row_at(editor, state, list, at);
        return {};
    }
    if (!state.finger)
        return {};
    if (kind == PointerKind::move) {
        state.finger_at = at;
        if (apart(at.x, state.finger_origin.x) > reach ||
            apart(at.y, state.finger_origin.y) > reach)
            state.finger_moved = true;
        if (!state.finger_moved)
            return {};
        const std::optional<kit::ScrollArea> area =
            state.finger_in_details ? details_area(state, list) : list_area(editor, state, list);
        if (!area)
            return {};
        const int32_t next = std::clamp(
            state.finger_scroll + (state.finger_origin.y - at.y), int32_t{0}, area->limit
        );
        if (next == (state.finger_in_details ? state.details_scroll : state.list_scroll))
            return {};
        scroll_to(state, state.finger_in_details, next);
        return redrawn();
    }
    if (kind == PointerKind::up) {
        const bool tap = !state.finger_moved;
        const std::string row = state.finger_row;
        state.finger = false;
        state.finger_moved = false;
        state.finger_row.clear();
        if (!tap)
            return redrawn();
        const std::string chosen = row_at(editor, state, list, at);
        const std::string id = chosen.empty() ? row : chosen;
        if (id.empty())
            return {};
        state.selected = id;
        state.details_scroll = 0;
        focus_on(state, controls::list);
        return redrawn();
    }
    return {};
}

/// Closes the capture or the clash question without writing.
///
/// @param[in,out] state the prompt
void close_prompt(ScreenState& state) {
    state.prompt = Prompt::none;
    state.capture = Capture{};
    state.pending_clash = {};
}

/// Moves the focus to OK when it can be pressed, otherwise to CANCEL.
///
/// @param editor the line under the well
/// @param[in,out] state the focus
void focus_finished(const Editor& editor, ScreenState& state) {
    static_cast<void>(editor);
    if (state.prompt != Prompt::capture)
        return;
    if (state.capture.state == CaptureState::complete || state.capture.state == CaptureState::clash)
        focus_on(state, controls::capture_ok);
    else
        focus_on(state, controls::capture_cancel);
}

/// Opens the capture for the chosen action.
///
/// @param editor the action's letter or place
/// @param[in,out] state the prompt
/// @param replacing the chord being changed; none for + ADD KEY
void start_capture(const Editor& editor, ScreenState& state, std::optional<std::size_t> replacing) {
    static_cast<void>(editor);
    state.menu = Menu::none;
    state.prompt = Prompt::capture;
    state.capture = Capture{};
    state.capture.action = state.selected;
    state.capture.replacing = replacing;
    state.capture.starts = keys::Gesture::press;
    const acts::Action* action = acts::find(state.selected);
    if (action != nullptr && action->by == acts::By::place)
        state.capture.means = acts::By::place;
    else
        state.capture.means = acts::By::letter;
    focus_on(state, controls::capture_cancel);
}

/// Writes a finished capture and closes the prompt.
///
/// @param[in,out] editor the draft
/// @param[in,out] state the prompt and the chosen row
/// @param choice replace or swap
/// @return what the host is to do
EditorResult finish_apply(Editor& editor, ScreenState& state, ClashChoice choice) {
    const ApplyResult applied = apply_capture(editor, state.capture, choice);
    if (!applied.applied)
        return {};
    if (!applied.select.empty())
        state.selected = applied.select;
    state.details_scroll = 0;
    state.list_scroll = 0;
    close_prompt(state);
    return redrawn();
}

/// OK on the capture: a free chord is bound, a clash opens the question.
///
/// @param[in,out] editor the draft
/// @param[in,out] state the prompt
/// @return what the host is to do
EditorResult press_ok(Editor& editor, ScreenState& state) {
    const CaptureStatus status = capture_status(editor, state.capture);
    if (!status.ok_enabled)
        return {};
    if (state.capture.state == CaptureState::clash && status.clash) {
        state.pending_clash = *status.clash;
        if (state.pending_clash.a != state.capture.action &&
            state.pending_clash.b == state.capture.action)
            std::swap(state.pending_clash.a, state.pending_clash.b);
        state.prompt = Prompt::clash;
        if (state.capture.replacing)
            focus_on(state, controls::clash_swap);
        else
            focus_on(state, controls::clash_replace);
        return redrawn();
    }
    if (state.capture.state != CaptureState::complete)
        return {};
    return finish_apply(editor, state, ClashChoice::replace);
}

/// The place of a chord among an action's chords.
///
/// @param editor the keyboard
/// @param id the action
/// @param chord the chord
/// @return the place, from 0, or nothing
std::optional<std::size_t>
index_of_chord(const Editor& editor, std::string_view id, const keys::Chord& chord) {
    const Details action = editor.details(id);
    const keys::KeyboardMap& board = editor.keyboard();
    for (std::size_t at = 0; at < action.chords.size(); ++at) {
        if (keys::same_key(action.chords[at].chord, chord, &board))
            return at;
        const std::optional<keys::Chord> paired = paired_chord(chord, editor.platform());
        if (paired && keys::same_key(action.chords[at].chord, *paired, &board))
            return at;
    }
    return std::nullopt;
}

/// The chord's button number for a platform button. SDL's right and middle
/// are the other way round from the numbers a chord stores.
///
/// @param button the platform button
/// @return the chord's button
uint8_t stored_button(uint8_t button) noexcept {
    if (button == 3)
        return 2;
    if (button == 2)
        return 3;
    return button;
}

/// A pointer while a prompt covers the list.
///
/// A button in the well finishes the chord, or starts it again once a chord
/// is showing. OK and CANCEL take the primary button only.
///
/// @param[in,out] editor the model
/// @param[in,out] state the prompt
/// @param list the display list
/// @param kind a move, a press or a release
/// @param button the platform button
/// @param at the pointer
/// @return what the host is to do
EditorResult prompt_pointer(
    Editor& editor,
    ScreenState& state,
    const kit::DisplayList& list,
    PointerKind kind,
    uint8_t button,
    kit::Point at
) {
    if (kind == PointerKind::move) {
        const kit::PointerOutcome moved = kit::pointer_move(state.interaction, list, at);
        if (moved.result == kit::PointerResult::redraw)
            return redrawn();
        return {};
    }
    const kit::Control* well = kit::control_of(list, controls::capture_well);
    const bool in_well = well != nullptr && kit::contains(well->rect, at);
    if (kind == PointerKind::down) {
        if (state.prompt == Prompt::capture && in_well && button >= 1 && button <= 8) {
            if (state.capture.state == CaptureState::waiting) {
                keys::InputEvent event;
                event.kind = keys::InputKind::button;
                event.down = true;
                event.button = stored_button(button);
                capture_button(state.capture, editor, event);
                focus_finished(editor, state);
                return redrawn();
            }
            state.capture.chord.reset();
            state.capture.state = CaptureState::waiting;
            state.capture.escape_down_ms = 0;
            state.capture.held_modifiers = 0;
            focus_on(state, controls::capture_cancel);
            return redrawn();
        }
        if (button != 1)
            return {};
        const kit::PointerOutcome pressed = kit::pointer_down(state.interaction, list, at);
        if (pressed.result == kit::PointerResult::redraw)
            return redrawn();
        return {};
    }
    if (button != 1)
        return {};
    const kit::PointerOutcome released = kit::pointer_up(state.interaction, list, at);
    if (released.result != kit::PointerResult::activated)
        return released.result == kit::PointerResult::redraw ? redrawn() : EditorResult{};
    return activate(editor, state, list, released.control);
}

} // namespace

std::optional<kit::Rect>
focused_field(const ScreenState& state, const kit::DisplayList& list) noexcept {
    if (state.interaction.focused != controls::search)
        return std::nullopt;
    const kit::Control* field = kit::control_of(list, controls::search);
    if (field == nullptr)
        return std::nullopt;
    return field->rect;
}

EditorResult editor_pointer(
    Editor& editor,
    ScreenState& state,
    const kit::DisplayList& list,
    PointerKind kind,
    uint8_t button,
    kit::Point at,
    int32_t finger_reach
) {
    if (state.prompt != Prompt::none)
        return prompt_pointer(editor, state, list, kind, button, at);
    if (state.bar != ScreenState::Bar::none) {
        if (kind == PointerKind::move) {
            if (move_bar(state, editor, list, at))
                return redrawn();
            return {};
        }
        if (kind == PointerKind::up) {
            state.bar = ScreenState::Bar::none;
            return redrawn();
        }
    }
    if (finger_reach > 0 || state.finger)
        return finger_pointer(editor, state, list, kind, button, at, finger_reach);

    if (kind == PointerKind::move) {
        const kit::PointerOutcome moved = kit::pointer_move(state.interaction, list, at);
        if (state.menu != Menu::none) {
            const kit::ControlId under = kit::hit(list, at);
            if (menu_item(under)) {
                const int32_t marked = under - controls::menu_first;
                if (marked != state.menu_marked) {
                    state.menu_marked = marked;
                    return redrawn();
                }
            }
        }
        if (moved.result == kit::PointerResult::redraw)
            return redrawn();
        return {};
    }
    if (kind == PointerKind::down) {
        const uint8_t numbered = stored_button(button);
        if (numbered >= 4 && state.interaction.focused == controls::search &&
            state.search.text.empty()) {
            const kit::Control* field = kit::control_of(list, controls::search);
            if (field != nullptr && kit::contains(field->rect, at)) {
                keys::InputEvent event;
                event.kind = keys::InputKind::button;
                event.down = true;
                event.button = numbered;
                if (std::optional<keys::Chord> found = chord_to_find(editor, event)) {
                    state.search_chord = std::move(*found);
                    state.search.text.clear();
                    state.search.caret = 0;
                    state.list_scroll = 0;
                    return redrawn();
                }
            }
        }
        if (button != 1)
            return {};
        const kit::ControlId under = kit::hit(list, at);
        if (state.menu != Menu::none && !menu_item(under) && !menu_opener(state.menu, under)) {
            close_menu(state);
            return redrawn();
        }
        if (under == controls::list_bar || under == controls::details_bar) {
            if (take_bar(state, editor, list, under == controls::details_bar, at))
                return redrawn();
            return {};
        }
        const kit::PointerOutcome pressed = kit::pointer_down(state.interaction, list, at);
        if (under == controls::search)
            focus_on(state, controls::search);
        if (pressed.result == kit::PointerResult::redraw || under == controls::search)
            return redrawn();
        return {};
    }
    const kit::PointerOutcome released = kit::pointer_up(state.interaction, list, at);
    if (released.result != kit::PointerResult::activated)
        return released.result == kit::PointerResult::redraw ? redrawn() : EditorResult{};
    return activate(editor, state, list, released.control);
}

EditorResult
editor_key(Editor& editor, ScreenState& state, const kit::DisplayList& list, kit::Key pressed) {
    if (state.prompt != Prompt::none) {
        if (pressed == kit::Key::escape) {
            close_prompt(state);
            return redrawn();
        }
        const kit::KeyOutcome outcome = kit::key(state.interaction, list, pressed);
        if (outcome.result == kit::KeyResult::redraw)
            return redrawn();
        if (outcome.result == kit::KeyResult::to_control)
            return activate(editor, state, list, outcome.control);
        return {};
    }
    if (state.menu != Menu::none)
        return menu_key(editor, state, list, pressed);
    if (pressed == kit::Key::escape)
        return escape_key(editor, state);

    const bool on_search = state.interaction.focused == controls::search;
    if (on_search && (pressed == kit::Key::left || pressed == kit::Key::right ||
                      pressed == kit::Key::home || pressed == kit::Key::end ||
                      pressed == kit::Key::backspace || pressed == kit::Key::delete_forward)) {
        if (kit::edit_text(state.search, pressed)) {
            state.list_scroll = 0;
            return redrawn();
        }
        return {};
    }
    if (pressed == kit::Key::delete_forward && chord_row(state.interaction.focused)) {
        editor.remove_chord(
            state.selected,
            static_cast<std::size_t>(state.interaction.focused - controls::chord_row_first)
        );
        return redrawn();
    }
    if (pressed == kit::Key::delete_forward && chord_remove(state.interaction.focused)) {
        editor.remove_chord(
            state.selected,
            static_cast<std::size_t>(state.interaction.focused - controls::chord_first)
        );
        return redrawn();
    }

    const bool on_list = state.interaction.focused == controls::list;
    if (on_list &&
        (pressed == kit::Key::up || pressed == kit::Key::down || pressed == kit::Key::page_up ||
         pressed == kit::Key::page_down || pressed == kit::Key::home || pressed == kit::Key::end)) {
        const int32_t page = rows_shown(editor, state, list);
        int32_t step = 0;
        bool home = false;
        bool end = false;
        if (pressed == kit::Key::up)
            step = -1;
        else if (pressed == kit::Key::down)
            step = 1;
        else if (pressed == kit::Key::page_up)
            step = -page;
        else if (pressed == kit::Key::page_down)
            step = page;
        else if (pressed == kit::Key::home)
            home = true;
        else
            end = true;
        move_row(state, editor, list, step, home, end);
        return redrawn();
    }
    if (on_list && (pressed == kit::Key::enter || pressed == kit::Key::space))
        return open_details(state, list);

    const kit::KeyOutcome outcome = kit::key(state.interaction, list, pressed);
    if (outcome.result == kit::KeyResult::redraw)
        return redrawn();
    if (outcome.result == kit::KeyResult::to_control) {
        if (outcome.control == controls::search) {
            if (kit::edit_text(state.search, pressed)) {
                state.list_scroll = 0;
                return redrawn();
            }
            return {};
        }
        if (outcome.control == controls::list)
            return open_details(state, list);
        return activate(editor, state, list, outcome.control);
    }
    if (outcome.result == kit::KeyResult::scroll) {
        const bool details = details_focused(state);
        const int32_t page = rows_shown(editor, state, list);
        const int32_t step =
            pressed == kit::Key::page_up || pressed == kit::Key::home ? -page : page;
        if (pressed == kit::Key::home || pressed == kit::Key::end) {
            if (details) {
                const std::optional<kit::ScrollArea> area = details_area(state, list);
                state.details_scroll = pressed == kit::Key::home || !area ? 0 : area->limit;
            } else {
                const std::optional<kit::ScrollArea> area = list_area(editor, state, list);
                state.list_scroll = pressed == kit::Key::home || !area ? 0 : area->limit;
            }
            return redrawn();
        }
        return turn_pane(
            state,
            editor,
            list,
            details,
            pressed == kit::Key::page_up ? static_cast<float>(-step) : static_cast<float>(step)
        );
    }
    if (outcome.result == kit::KeyResult::cancel)
        return leave(editor);
    return {};
}

EditorResult editor_text(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, std::string_view utf8
) {
    static_cast<void>(editor);
    static_cast<void>(list);
    if (state.prompt != Prompt::none)
        return {};
    if (state.interaction.focused != controls::search)
        return {};
    if (state.search_chord)
        state.search_chord.reset();
    if (!kit::insert_text(state.search, utf8))
        return {};
    state.list_scroll = 0;
    return redrawn();
}

EditorResult editor_wheel(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, kit::Point at, float notches
) {
    if (state.prompt != Prompt::none) {
        const kit::Control* well = kit::control_of(list, controls::capture_well);
        if (state.prompt == Prompt::capture && state.capture.state == CaptureState::waiting &&
            well != nullptr && kit::contains(well->rect, at)) {
            keys::InputEvent event;
            event.kind = keys::InputKind::wheel;
            if (notches > 0)
                event.wheel_y = 1;
            else if (notches < 0)
                event.wheel_y = -1;
            capture_wheel(state.capture, editor, event);
            focus_finished(editor, state);
            return redrawn();
        }
        return {};
    }
    const kit::Control* list_bar = kit::control_of(list, controls::list_bar);
    const kit::Control* details_bar = kit::control_of(list, controls::details_bar);
    const bool on_list =
        in_list(list, at) || (list_bar != nullptr && kit::contains(list_bar->rect, at));
    const bool on_details = details_bar != nullptr && kit::contains(details_bar->rect, at);
    if (!on_list && !on_details)
        return {};
    return turn_pane(state, editor, list, on_details && !on_list, notches);
}

EditorResult editor_command(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, EditorCommand command
) {
    static_cast<void>(list);
    if (state.prompt != Prompt::none) {
        if (command == EditorCommand::back) {
            close_prompt(state);
            return redrawn();
        }
        return {};
    }
    if (command == EditorCommand::add_key) {
        if (state.selected.empty())
            return {};
        const Details action = editor.details(state.selected);
        if (action.read_only || action.chords.size() >= 4)
            return {};
        start_capture(editor, state, std::nullopt);
        return redrawn();
    }
    if (command == EditorCommand::find) {
        focus_on(state, controls::search);
        return redrawn();
    }
    if (command == EditorCommand::back) {
        if (state.details_page) {
            state.details_page = false;
            return redrawn();
        }
        return leave(editor);
    }
    if (command == EditorCommand::save) {
        if (!editor.can_save())
            return {};
        return asked(EditorAction::save, {}, false);
    }
    return {};
}

EditorResult
editor_stick(Editor& editor, ScreenState& state, const kit::DisplayList& list, float notches) {
    if (state.prompt != Prompt::none)
        return {};
    return turn_pane(state, editor, list, details_focused(state), notches);
}

EditorResult
editor_capture_input(Editor& editor, ScreenState& state, const keys::InputEvent& event) {
    if (state.prompt != Prompt::capture || state.capture.state != CaptureState::waiting)
        return {};
    if (event.kind == keys::InputKind::key) {
        const CaptureEvent happened = capture_key(state.capture, editor, event);
        if (happened == CaptureEvent::cancelled) {
            close_prompt(state);
            return redrawn();
        }
        if (happened == CaptureEvent::changed) {
            focus_finished(editor, state);
            return redrawn();
        }
        return {};
    }
    if (event.kind == keys::InputKind::button) {
        const bool had = state.capture.chord.has_value();
        capture_button(state.capture, editor, event);
        if (!had && state.capture.chord) {
            focus_finished(editor, state);
            return redrawn();
        }
        return {};
    }
    if (event.kind == keys::InputKind::wheel) {
        const bool had = state.capture.chord.has_value();
        capture_wheel(state.capture, editor, event);
        if (!had && state.capture.chord) {
            focus_finished(editor, state);
            return redrawn();
        }
        return {};
    }
    return {};
}

EditorResult editor_capture_tick(Editor& editor, ScreenState& state, uint64_t now_ms) {
    if (state.prompt != Prompt::capture || state.capture.state != CaptureState::waiting)
        return {};
    if (state.capture.escape_down_ms == 0)
        return {};
    capture_tick(state.capture, editor, now_ms);
    if (state.capture.state == CaptureState::waiting)
        return {};
    focus_finished(editor, state);
    return redrawn();
}

} // namespace oa::ui::key_editor
