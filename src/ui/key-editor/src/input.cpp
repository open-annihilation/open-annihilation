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
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/keys.hpp"
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

void focus_on(ScreenState& state, kit::ControlId id);

/// Leaves the editor, asking first while the draft differs from what was
/// opened: the close question offers Save, Discard and Keep editing, Save
/// left out while a clash holds it back. Its focus starts on Save, or on
/// Keep editing when Save is left out.
///
/// @param editor the model
/// @param[in,out] state the prompt
/// @return cancel, or the question shown
EditorResult leave(const Editor& editor, ScreenState& state) {
    if (editor.unsaved() == 0)
        return asked(EditorAction::cancel, {}, false);
    state.menu = Menu::none;
    state.prompt = Prompt::leave;
    focus_on(state, editor.can_save() ? controls::leave_save : controls::leave_keep);
    return redrawn();
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
    // Find by key's rows start at find_row_first. A menu item stays below that.
    return id >= controls::menu_first && id < controls::find_row_first;
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
/// @return true for Latch, Hold, Reset, a chord, + Add a key, More options or Settle…
bool detail_control(kit::ControlId id) {
    return id == controls::detail_latch || id == controls::detail_confirm ||
           id == controls::detail_reset || id == controls::detail_add ||
           id == controls::detail_find || id == controls::detail_more ||
           id == controls::card_settle || chord_remove(id) || chord_row(id);
}

void close_prompt(ScreenState& state);
void open_share(Editor& editor, ScreenState& state);
void open_import(ScreenState& state);
void focus_finished(const Editor& editor, ScreenState& state);
void start_capture(const Editor& editor, ScreenState& state, std::optional<std::size_t> replacing);
void start_finding(ScreenState& state);
EditorResult confirm_clash(Editor& editor, ScreenState& state);
EditorResult open_settle(Editor& editor, ScreenState& state);
EditorResult choose_found(Editor& editor, ScreenState& state, std::size_t index);
void show_view(Editor& editor, ScreenState& state, View shown);
void find_on_keyboard(const Editor& editor, ScreenState& state);
EditorResult press_picture_key(Editor& editor, ScreenState& state, std::string code);
EditorResult press_card_button(Editor& editor, ScreenState& state, bool use);
EditorResult press_mouse_button(Editor& editor, ScreenState& state, int index);
EditorResult press_mouse_row(Editor& editor, ScreenState& state, int index);
EditorResult move_key(ScreenState& state, const kit::DisplayList& list, kit::Key pressed);
kit::ControlId key_control(const ScreenState& state, std::string_view code);
std::string code_of_key(const ScreenState& state, kit::ControlId id);
bool in_keyboard_well(const kit::DisplayList& list, kit::Point at);
int32_t keyboard_scroll_limit(const kit::DisplayList& list);
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
    if (entry.kind == MenuKind::share) {
        close_menu(state);
        open_share(editor, state);
        return redrawn();
    }
    if (entry.kind == MenuKind::paste) {
        close_menu(state);
        return asked(EditorAction::read_clipboard, {}, true);
    }
    if (entry.kind == MenuKind::import) {
        close_menu(state);
        open_import(state);
        return asked(EditorAction::prepare_import, {}, true);
    }
    if (entry.kind == MenuKind::shape) {
        if (entry.id == "system") {
            state.shape = state.system_shape;
            state.shape_from_system = true;
        } else if (entry.id == "iso") {
            state.shape = kit::KeyboardShape::iso;
            state.shape_from_system = false;
        } else if (entry.id == "jis") {
            state.shape = kit::KeyboardShape::jis;
            state.shape_from_system = false;
        } else {
            state.shape = kit::KeyboardShape::ansi;
            state.shape_from_system = false;
        }
        close_menu(state);
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
    if (menu == Menu::shape)
        return id == controls::keyboard_shape;
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
    if (id == controls::view_list || id == controls::view_keyboard || id == controls::view_mouse) {
        View shown = View::list;
        if (id == controls::view_keyboard)
            shown = View::keyboard;
        else if (id == controls::view_mouse)
            shown = View::mouse;
        show_view(editor, state, shown);
        return redrawn();
    }
    if (id == controls::layer_none || id == controls::layer_shift || id == controls::layer_ctrl ||
        id == controls::layer_alt || id == controls::layer_cmd) {
        uint8_t layer = 0;
        if (id == controls::layer_shift)
            layer = keys::Modifier::shift;
        else if (id == controls::layer_ctrl)
            layer = keys::Modifier::ctrl;
        else if (id == controls::layer_alt)
            layer = keys::Modifier::alt;
        else if (id == controls::layer_cmd)
            layer = keys::Modifier::meta;
        state.layer = layer;
        state.menu = Menu::none;
        focus_on(state, id);
        return redrawn();
    }
    if (id == controls::keyboard_shape) {
        toggle_menu(state, editor, list, Menu::shape);
        return redrawn();
    }
    if (id == controls::scheme_right || id == controls::scheme_left) {
        editor.set_scheme(
            id == controls::scheme_left ? keys::PointerScheme::left_click
                                        : keys::PointerScheme::right_click
        );
        focus_on(state, id);
        return redrawn();
    }
    if (id == controls::detail_find) {
        find_on_keyboard(editor, state);
        return redrawn();
    }
    if (id == controls::find) {
        start_finding(state);
        return redrawn();
    }
    if (id == controls::find_close) {
        close_prompt(state);
        return redrawn();
    }
    if (id >= controls::find_row_first && id < controls::keyboard_first)
        return choose_found(editor, state, static_cast<std::size_t>(id - controls::find_row_first));
    if (id == controls::undo) {
        if (!editor.undo())
            return {};
        state.details_scroll = 0;
        return redrawn();
    }
    if (id == controls::detail_more) {
        state.details_more = !state.details_more;
        return redrawn();
    }
    if (id == controls::capture_more) {
        state.capture_more = !state.capture_more;
        return redrawn();
    }
    if (id == controls::show_as_list) {
        state.keys_as_list = !state.keys_as_list;
        state.key_list_scroll = 0;
        focus_on(state, controls::show_as_list);
        return redrawn();
    }
    if (id == controls::card_change || id == controls::card_use)
        return press_card_button(editor, state, id == controls::card_use);
    if (id >= controls::keyboard_first && id < controls::mouse_first) {
        const std::string code = code_of_key(state, id);
        if (code.empty())
            return {};
        return press_picture_key(editor, state, code);
    }
    if (id >= controls::mouse_first && id < controls::mouse_table_first)
        return press_mouse_button(editor, state, static_cast<int>(id - controls::mouse_first));
    if (id >= controls::mouse_table_first)
        return press_mouse_row(editor, state, static_cast<int>(id - controls::mouse_table_first));
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
        // The pills are one Tab stop: the one in use keeps it.
        state.interaction.focused = id;
        return redrawn();
    }
    if (id >= controls::nav_first && id < controls::row_first) {
        const std::vector<CategoryEntry> categories = editor.categories();
        const auto index = static_cast<std::size_t>(id - controls::nav_first);
        if (index < categories.size() && categories[index].id.rfind("heading.", 0) != 0) {
            state.category = categories[index].id;
            state.list_scroll = 0;
            // The nav is one Tab stop: the category in use keeps it.
            state.interaction.focused = id;
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
    if (id == controls::import_keys) {
        open_import(state);
        return asked(EditorAction::prepare_import, {}, true);
    }
    if (id == controls::share_keys) {
        open_share(editor, state);
        return redrawn();
    }
    if (id == controls::paste_keys)
        return asked(EditorAction::read_clipboard, {}, true);
    if (id == controls::share_name) {
        focus_on(state, controls::share_name);
        return redrawn();
    }
    if (id == controls::share_copy)
        return asked(EditorAction::copy_text, {}, false);
    if (id == controls::share_save) {
        const std::optional<std::string> folded = id_from_name(editor.share_name());
        if (!folded)
            return {};
        return asked(EditorAction::save_share, *folded, false);
    }
    if (id == controls::share_open)
        return asked(EditorAction::open_share_folder, {}, false);
    if (id == controls::share_close || id == controls::paste_cancel ||
        id == controls::import_cancel) {
        close_prompt(state);
        return redrawn();
    }
    if (id == controls::paste_apply) {
        if (state.paste.changes == 0)
            return {};
        apply_paste(editor, state.paste);
        close_prompt(state);
        return redrawn();
    }
    if (id == controls::import_apply) {
        if (state.importing.differ == 0)
            return {};
        state.importing.date = state.import_date;
        apply_import(editor, state.importing);
        close_prompt(state);
        return redrawn();
    }
    if (id == controls::import_eye || id == controls::import_tdraw ||
        id == controls::import_ddraw || id == controls::import_choose) {
        std::string which = "choose";
        if (id == controls::import_eye)
            which = "eye";
        else if (id == controls::import_tdraw)
            which = "tdraw";
        else if (id == controls::import_ddraw)
            which = "ddraw";
        return asked(EditorAction::read_import, std::move(which), true);
    }
    if (id == controls::back) {
        state.details_page = false;
        return redrawn();
    }
    if (id == controls::cancel || id == controls::close_mark)
        return leave(editor, state);
    if (id == controls::leave_save) {
        // Save writes the draft and closes; a failed write leaves the editor
        // open under the host's notice.
        if (!editor.can_save())
            return {};
        close_prompt(state);
        return asked(EditorAction::save, {}, true);
    }
    if (id == controls::leave_discard) {
        close_prompt(state);
        return asked(EditorAction::cancel, {}, false);
    }
    if (id == controls::leave_keep) {
        close_prompt(state);
        return redrawn();
    }
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
    // An outcome is only marked. The confirm button applies it.
    if (id == controls::clash_replace || id == controls::card_here) {
        state.outcome = 0;
        focus_on(state, id);
        return redrawn();
    }
    if (id == controls::clash_swap || id == controls::card_there) {
        state.outcome = 1;
        focus_on(state, id);
        return redrawn();
    }
    if (id == controls::clash_confirm)
        return confirm_clash(editor, state);
    if (id == controls::clash_another) {
        if (state.settling) {
            const std::string here = state.pending_clash.a;
            const keys::Chord chord = state.pending_clash.chord;
            close_prompt(state);
            state.selected = here;
            const std::optional<std::size_t> at = index_of_chord(editor, here, chord);
            if (!at)
                return redrawn();
            start_capture(editor, state, at);
            return redrawn();
        }
        // Back to the capture: the same action, listening again.
        state.prompt = Prompt::capture;
        state.pending_clash = {};
        state.outcome = 0;
        state.capture.chord.reset();
        state.capture.state = CaptureState::waiting;
        state.capture.escape_down_ms = 0;
        state.capture.held_modifiers = 0;
        focus_on(state, controls::capture_cancel);
        return redrawn();
    }
    if (id == controls::capture_means) {
        set_means(
            state.capture,
            editor,
            state.capture.means == acts::By::place ? acts::By::letter : acts::By::place
        );
        focus_on(state, controls::capture_means);
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
    if (id == controls::card_settle)
        return open_settle(editor, state);
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
    return leave(editor, state);
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
        if (state.view == View::keyboard &&
            (!code_of_key(state, under).empty() || in_keyboard_well(list, at))) {
            state.finger = true;
            state.finger_origin = at;
            state.finger_at = at;
            state.finger_moved = false;
            state.finger_in_details = false;
            state.finger_scroll = state.keyboard_scroll;
            state.finger_row = code_of_key(state, under);
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
        if (state.view == View::keyboard && in_keyboard_well(list, state.finger_origin)) {
            const int32_t next = std::clamp(
                state.finger_scroll + (state.finger_origin.x - at.x),
                int32_t{0},
                keyboard_scroll_limit(list)
            );
            if (next == state.keyboard_scroll)
                return {};
            state.keyboard_scroll = next;
            return redrawn();
        }
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
        if (state.view == View::keyboard && in_keyboard_well(list, state.finger_origin)) {
            const bool key_tap = !state.finger_moved;
            const std::string pressed_key = state.finger_row;
            state.finger = false;
            state.finger_moved = false;
            state.finger_row.clear();
            if (!key_tap) {
                state.finger_armed = false;
                return redrawn();
            }
            const std::string now = code_of_key(state, kit::hit(list, at));
            const std::string code = now.empty() ? pressed_key : now;
            if (code.empty())
                return {};
            if (state.finger_armed && state.finger_key == code)
                return press_picture_key(editor, state, code);
            state.finger_armed = true;
            state.finger_key = code;
            state.focus_code = code;
            state.card_code = code;
            const kit::ControlId key_id = key_control(state, code);
            if (key_id != kit::no_control)
                focus_on(state, key_id);
            return redrawn();
        }
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

/// Closes a prompt without writing the draft.
///
/// @param[in,out] state the prompt
void close_prompt(ScreenState& state) {
    state.prompt = Prompt::none;
    state.capture = Capture{};
    state.pending_clash = {};
    state.settling = false;
    state.outcome = 0;
    state.capture_more = false;
    state.finding = Capture{};
    state.exchange_scroll = 0;
}

/// Opens SHARE… on an empty name. A built-in layout does not start named.
///
/// @param[in,out] editor the share name
/// @param[in,out] state the prompt
void open_share(Editor& editor, ScreenState& state) {
    state.menu = Menu::none;
    state.prompt = Prompt::share;
    state.exchange_scroll = 0;
    state.share_field = {};
    state.share_place.clear();
    editor.set_share_name("");
    focus_on(state, controls::share_name);
}

/// Opens IMPORT… before the host has filled the offers.
///
/// @param[in,out] state the prompt
void open_import(ScreenState& state) {
    state.menu = Menu::none;
    state.prompt = Prompt::import;
    state.exchange_scroll = 0;
    state.importing = {};
    state.import_chosen = -1;
    focus_on(state, controls::import_cancel);
}

/// Moves the focus to OK when it can be pressed, otherwise to CANCEL.
///
/// @param editor the line under the well
/// @param[in,out] state the focus
void focus_finished(const Editor& editor, ScreenState& state) {
    static_cast<void>(editor);
    if (state.prompt == Prompt::find) {
        focus_on(state, controls::find_close);
        return;
    }
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

/// The control of one keyboard key.
///
/// @param state the shape
/// @param code the W3C code
/// @return the control, or none
kit::ControlId key_control(const ScreenState& state, std::string_view code) {
    const std::span<const kit::KeyPlace> places = kit::keyboard_places(state.shape);
    for (std::size_t index = 0; index < places.size(); ++index) {
        if (places[index].code == code)
            return controls::keyboard_first + static_cast<kit::ControlId>(index);
    }
    return kit::no_control;
}

/// The W3C code of one keyboard control.
///
/// @param state the shape
/// @param id the control
/// @return the code, or empty
std::string code_of_key(const ScreenState& state, kit::ControlId id) {
    if (id < controls::keyboard_first || id >= controls::mouse_first)
        return {};
    const std::span<const kit::KeyPlace> places = kit::keyboard_places(state.shape);
    const auto index = static_cast<std::size_t>(id - controls::keyboard_first);
    if (index >= places.size())
        return {};
    return std::string(places[index].code);
}

/// True when a point lies in the keyboard's well.
///
/// @param list the display list
/// @param at the point
/// @return true when the well holds it
bool in_keyboard_well(const kit::DisplayList& list, kit::Point at) {
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::keyboard)
            continue;
        const bool clipped = item.clip.width > 0 && item.clip.height > 0;
        return kit::contains(clipped ? item.clip : item.rect, at);
    }
    return false;
}

/// How far the compact keyboard can scroll.
///
/// @param list the display list
/// @return the furthest scroll, in points
int32_t keyboard_scroll_limit(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::keyboard)
            continue;
        const int32_t clip_w = item.clip.width > 0 ? item.clip.width : item.rect.width;
        return std::max(item.rect.width - clip_w, int32_t{0});
    }
    return 0;
}

/// The modifier bits a new chord stores for the layer being shown.
///
/// Ctrl stays Ctrl. On macOS the Cmd layer stores Primary.
///
/// @param layer the layer's bit; 0 is Nothing
/// @param platform the platform
/// @return the bits to store
uint8_t binding_modifiers(uint8_t layer, const keys::Platform& platform) noexcept {
    if (layer == keys::Modifier::meta && platform.primary_is_meta)
        return keys::Modifier::primary;
    return layer;
}

/// The first action a key's card would select.
///
/// While playing wins. A fixed line, such as Escape on the chat line, does not.
///
/// @param editor the card
/// @param code the W3C code
/// @param layer the layer's bit
/// @return the action id, or empty when the key is free
std::string face_action(const Editor& editor, std::string_view code, uint8_t layer) {
    const std::vector<CardLine> lines = key_card(editor, code, layer);
    const std::string playing = show(editor.text(), words::card_while_playing);
    for (const CardLine& line : lines) {
        if (!line.fixed && line.context == playing && !line.id.empty())
            return line.id;
    }
    for (const CardLine& line : lines) {
        if (!line.fixed && !line.id.empty())
            return line.id;
    }
    return {};
}

/// The chord one key of the picture makes in a layer, for an action that
/// reads a letter or a place.
///
/// @param editor the keyboard and the platform
/// @param layer the layer's bit
/// @param code the W3C code
/// @param by whether the action reads the letter or the key's place
/// @return the chord, or nothing for a key no chord can name
std::optional<keys::Chord>
chord_of_key(const Editor& editor, uint8_t layer, std::string_view code, acts::By by) {
    const std::optional<uint16_t> scan = keys::scancode_of(code);
    keys::Chord chord;
    chord.modifiers = binding_modifiers(layer, editor.platform());
    chord.gesture = keys::Gesture::press;
    char32_t printed = 0;
    if (scan)
        printed = editor.keyboard().printed(*scan);
    char32_t upper = printed;
    if (upper >= U'a' && upper <= U'z')
        upper = upper - U'a' + U'A';
    const bool letter = by == acts::By::letter && upper >= U'A' && upper <= U'Z';
    if (letter) {
        chord.kind = keys::TriggerKind::character;
        chord.character = upper;
        if (scan)
            chord.scancode = *scan;
        return chord;
    }
    if (scan) {
        for (const keys::NamedKeySpec& spec : keys::named_keys()) {
            if (spec.scancode != *scan)
                continue;
            chord.kind = keys::TriggerKind::named;
            chord.named = static_cast<uint8_t>(spec.id);
            chord.scancode = spec.scancode;
            return chord;
        }
        chord.kind = keys::TriggerKind::place;
        chord.scancode = *scan;
        return chord;
    }
    return std::nullopt;
}

/// Fills a capture for the action chosen in the list with one key of the
/// picture, and leaves its primary button to bind it.
///
/// @param[in,out] editor the draft
/// @param[in,out] state the prompt
/// @param code the W3C code
/// @return true when the prompt is showing the chord
bool complete_key(Editor& editor, ScreenState& state, std::string_view code) {
    if (keys::position_refused(code) || state.selected.empty())
        return false;
    const acts::Action* action = acts::find(state.selected);
    if (action == nullptr)
        return false;
    std::optional<keys::Chord> chord = chord_of_key(editor, state.layer, code, action->by);
    if (!chord)
        return false;
    start_capture(editor, state, std::nullopt);
    state.capture.means =
        chord->kind == keys::TriggerKind::character ? acts::By::letter : acts::By::place;
    state.capture.chord = std::move(chord);
    state.capture.held_modifiers = state.capture.chord->modifiers;
    set_gesture(state.capture, editor, keys::Gesture::press);
    focus_finished(editor, state);
    return true;
}

/// Sets the capture's state from the chord, including a wheel.
///
/// @param[in,out] editor the draft
/// @param[in,out] state the prompt
/// @param chord the mouse button or the wheel
void finish_device(Editor& editor, ScreenState& state, keys::Chord chord) {
    const bool wheel = chord.kind == keys::TriggerKind::wheel;
    state.capture.chord = std::move(chord);
    state.capture.held_modifiers = state.capture.chord->modifiers;
    state.capture.means = acts::By::none;
    state.capture.starts = keys::Gesture::press;
    if (wheel) {
        const CaptureStatus status = capture_status(editor, state.capture);
        if (status.refused)
            state.capture.state = CaptureState::refused;
        else if (status.clash)
            state.capture.state = CaptureState::clash;
        else
            state.capture.state = CaptureState::complete;
    } else {
        set_gesture(state.capture, editor, keys::Gesture::press);
    }
    focus_finished(editor, state);
}

/// The word a mouse control shares with the table.
///
/// @param index the control's place, from 0
/// @return the English word
std::string_view mouse_word(int index) noexcept {
    switch (index) {
    case 0:
        return words::button_left;
    case 1:
        return words::button_right;
    case 2:
        return words::button_middle;
    case 3:
        return words::button_wheel;
    case 4:
        return words::card_back;
    case 5:
        return words::button_forward;
    case 6:
        return words::button_6;
    case 7:
        return words::button_7;
    case 8:
        return words::button_8;
    default:
        return {};
    }
}

/// The chord's button number for one mouse control. The wheel is 0.
///
/// @param index the control's place, from 0
/// @return the button, 1 to 8
uint8_t mouse_button_number(int index) noexcept {
    if (index >= 0 && index <= 2)
        return static_cast<uint8_t>(index + 1);
    if (index >= 4 && index <= 8)
        return static_cast<uint8_t>(index);
    return 0;
}

/// The playing role drawn on one mouse button.
///
/// @param editor the table
/// @param word the button's English word
/// @return the action id, or empty when the button is free
std::string playing_role(const Editor& editor, std::string_view word) {
    if (word.empty())
        return {};
    const std::string shown = show(editor.text(), word);
    for (const RoleLine& row : mouse_table(editor, editor.scheme())) {
        if (row.place_id == "playing" && row.button == shown && !row.action.empty())
            return row.action;
    }
    return {};
}

/// Presses one key of the picture: the inspector shows everything on it.
/// Nothing is changed; the inspector's buttons change a key.
///
/// @param[in,out] editor the draft
/// @param[in,out] state the screen's state
/// @param code the W3C code
/// @return what the host is to do
EditorResult press_picture_key(Editor& editor, ScreenState& state, std::string code) {
    static_cast<void>(editor);
    state.focus_code = code;
    state.card_code = code;
    state.finger_armed = false;
    const kit::ControlId id = key_control(state, code);
    if (id != kit::no_control)
        focus_on(state, id);
    return redrawn();
}

/// The inspector's buttons. Change {action}'s key… opens the capture for the
/// chord the key holds; Use {key} for {action}… fills the capture for the
/// action chosen in the list with this key.
///
/// @param[in,out] editor the draft
/// @param[in,out] state the screen's state
/// @param use true for Use {key} for {action}…
/// @return what the host is to do
EditorResult press_card_button(Editor& editor, ScreenState& state, bool use) {
    const std::string code = state.card_code.empty() ? state.focus_code : state.card_code;
    if (code.empty())
        return {};
    if (use)
        return complete_key(editor, state, code) ? redrawn() : EditorResult{};
    const std::string bound = face_action(editor, code, state.layer);
    if (bound.empty())
        return {};
    state.selected = bound;
    state.details_scroll = 0;
    const acts::Action* action = acts::find(bound);
    const std::optional<keys::Chord> chord =
        chord_of_key(editor, state.layer, code, action != nullptr ? action->by : acts::By::letter);
    const Details details = editor.details(bound);
    std::optional<std::size_t> at;
    for (std::size_t index = 0; chord && index < details.chords.size() && !at; ++index) {
        if (chords_match(details.chords[index].chord, *chord, editor.keyboard(), editor.platform()))
            at = index;
    }
    if (details.read_only || (at && details.chords[*at].fixed))
        return redrawn();
    if (!at && details.chords.size() >= 4)
        return redrawn();
    start_capture(editor, state, at);
    return redrawn();
}

/// Opens the capture for a mouse button, or fills it when the button is free.
///
/// @param[in,out] editor the draft
/// @param[in,out] state the prompt
/// @param index the control's place, from 0
/// @return what the host is to do
EditorResult press_mouse_button(Editor& editor, ScreenState& state, int index) {
    const std::string role = playing_role(editor, mouse_word(index));
    if (!role.empty()) {
        state.selected = role;
        state.details_scroll = 0;
        start_capture(editor, state, std::nullopt);
        return redrawn();
    }
    if (state.selected.empty())
        return {};
    start_capture(editor, state, std::nullopt);
    keys::Chord chord;
    chord.gesture = keys::Gesture::press;
    if (index == 3) {
        chord.kind = keys::TriggerKind::wheel;
        chord.wheel = 1;
    } else {
        chord.kind = keys::TriggerKind::mouse;
        chord.button = mouse_button_number(index);
    }
    finish_device(editor, state, std::move(chord));
    return redrawn();
}

/// Opens the capture for one row of the mouse table.
///
/// @param[in,out] editor the draft
/// @param[in,out] state the prompt
/// @param index the row, from 0
/// @return what the host is to do
EditorResult press_mouse_row(Editor& editor, ScreenState& state, int index) {
    const std::vector<RoleLine> rows = mouse_table(editor, editor.scheme());
    if (index < 0 || static_cast<std::size_t>(index) >= rows.size())
        return {};
    const RoleLine& row = rows[static_cast<std::size_t>(index)];
    if (!row.free && !row.action.empty()) {
        state.selected = row.action;
        state.details_scroll = 0;
    } else if (state.selected.empty()) {
        return {};
    }
    start_capture(editor, state, std::nullopt);
    return redrawn();
}

/// The control of one layer pill.
///
/// @param layer the layer's bit
/// @return the control
kit::ControlId layer_control(uint8_t layer) noexcept {
    if (layer == keys::Modifier::shift)
        return controls::layer_shift;
    if (layer == keys::Modifier::ctrl)
        return controls::layer_ctrl;
    if (layer == keys::Modifier::alt)
        return controls::layer_alt;
    if (layer == keys::Modifier::meta)
        return controls::layer_cmd;
    return controls::layer_none;
}

/// The layer Left or Right reaches from the one showing.
///
/// @param layer the layer's bit
/// @param forward true for Right
/// @param command true when the Cmd layer is shown
/// @return the next layer's bit
uint8_t next_layer(uint8_t layer, bool forward, bool command) noexcept {
    const uint8_t bits[] = {
        0,
        keys::Modifier::shift,
        keys::Modifier::ctrl,
        keys::Modifier::alt,
        keys::Modifier::meta,
    };
    const int count = command ? 5 : 4;
    int index = 0;
    for (int place = 0; place < count; ++place) {
        if (bits[place] == layer)
            index = place;
    }
    if (forward)
        index = (index + 1) % count;
    else
        index = (index + count - 1) % count;
    return bits[index];
}

/// The filter pills' controls, in the order they show.
constexpr kit::ControlId filter_controls[] = {
    controls::filter_all,
    controls::filter_changed,
    controls::filter_clashes,
    controls::filter_unbound,
    controls::filter_latchable,
};

/// The filters, in the pills' order.
constexpr Filter filter_order[] = {
    Filter::all, Filter::changed, Filter::clashes, Filter::unbound, Filter::latchable
};

/// A filter pill's place among the pills.
///
/// @param id the control
/// @return its place from 0, or -1 when it is not a pill
int filter_place(kit::ControlId id) noexcept {
    for (int place = 0; place < 5; ++place) {
        if (filter_controls[place] == id)
            return place;
    }
    return -1;
}

/// Chooses the category above or below the one in use in the nav, skipping
/// its headings, and keeps the focus on it. The nav is one Tab stop, so the
/// arrows move within it; at an end the category stays.
///
/// @param editor the categories
/// @param[in,out] state the category, the scroll and the focus
/// @param list the display list, for the entries it shows
/// @param down true for Down
/// @return what the host is to do
EditorResult
step_category(const Editor& editor, ScreenState& state, const kit::DisplayList& list, bool down) {
    const std::vector<CategoryEntry> categories = editor.categories();
    const auto count = static_cast<int>(categories.size());
    int at = -1;
    for (int place = 0; place < count; ++place) {
        if (categories[static_cast<std::size_t>(place)].id == state.category)
            at = place;
    }
    const int step = down ? 1 : -1;
    for (int place = at + step; place >= 0 && place < count; place += step) {
        const CategoryEntry& entry = categories[static_cast<std::size_t>(place)];
        const kit::ControlId id = controls::nav_first + static_cast<kit::ControlId>(place);
        if (entry.id.rfind("heading.", 0) == 0 || kit::control_of(list, id) == nullptr)
            continue;
        state.category = entry.id;
        state.list_scroll = 0;
        focus_on(state, id);
        return redrawn();
    }
    return {};
}

/// Chooses the filter left or right of the one in use, round from the last
/// to the first, and keeps the focus on its pill. The pills are one Tab
/// stop, so the arrows move within them.
///
/// @param[in,out] state the filter, the scroll and the focus
/// @param list the display list, for the pills it shows
/// @param right true for Right
/// @return what the host is to do
EditorResult step_filter(ScreenState& state, const kit::DisplayList& list, bool right) {
    int at = 0;
    for (int place = 0; place < 5; ++place) {
        if (filter_order[place] == state.filter)
            at = place;
    }
    for (int tried = 1; tried < 5; ++tried) {
        const int place = (at + (right ? tried : 5 - tried)) % 5;
        if (kit::control_of(list, filter_controls[place]) == nullptr)
            continue;
        state.filter = filter_order[place];
        state.list_scroll = 0;
        focus_on(state, filter_controls[place]);
        return redrawn();
    }
    return {};
}

/// Shows one picture and puts the focus where that picture's keys start.
///
/// @param[in,out] editor the scheme, for the mouse
/// @param[in,out] state the picture
/// @param shown which picture
void show_view(Editor& editor, ScreenState& state, View shown) {
    state.view = shown;
    state.details_page = false;
    state.menu = Menu::none;
    state.card_code.clear();
    if (shown == View::mouse) {
        const bool left = editor.scheme() == keys::PointerScheme::left_click;
        focus_on(state, left ? controls::scheme_left : controls::scheme_right);
        return;
    }
    if (shown != View::keyboard) {
        focus_on(state, controls::view_list);
        return;
    }
    if (state.focus_code.empty()) {
        const std::span<const kit::KeyPlace> places = kit::keyboard_places(state.shape);
        if (!places.empty())
            state.focus_code = std::string(places.front().code);
    }
    const kit::ControlId id = key_control(state, state.focus_code);
    if (id != kit::no_control)
        focus_on(state, id);
    else
        focus_on(state, controls::view_keyboard);
}

/// Moves the focus to the nearest key in one direction, and scrolls it into the well.
///
/// @param[in,out] state the focus and the scroll
/// @param list the display list
/// @param pressed the arrow
/// @return what the host is to do
EditorResult move_key(ScreenState& state, const kit::DisplayList& list, kit::Key pressed) {
    const std::span<const kit::KeyPlace> places = kit::keyboard_places(state.shape);
    if (places.empty())
        return {};
    std::size_t from = 0;
    bool found = false;
    for (std::size_t index = 0; index < places.size(); ++index) {
        if (places[index].code == state.focus_code) {
            from = index;
            found = true;
        }
    }
    if (!found) {
        const std::string code = code_of_key(state, state.interaction.focused);
        for (std::size_t index = 0; index < places.size(); ++index) {
            if (places[index].code == code) {
                from = index;
                found = true;
            }
        }
    }
    if (!found)
        return {};
    const int32_t from_x =
        static_cast<int32_t>(places[from].x) + static_cast<int32_t>(places[from].w) / 2;
    const int32_t from_y =
        static_cast<int32_t>(places[from].y) + static_cast<int32_t>(places[from].h) / 2;
    const int32_t x_sign = pressed == kit::Key::right ? 1 : pressed == kit::Key::left ? -1 : 0;
    const int32_t y_sign = pressed == kit::Key::down ? 1 : pressed == kit::Key::up ? -1 : 0;
    std::size_t best = from;
    bool any = false;
    int64_t best_score = 0;
    for (std::size_t index = 0; index < places.size(); ++index) {
        if (index == from)
            continue;
        const int32_t at_x =
            static_cast<int32_t>(places[index].x) + static_cast<int32_t>(places[index].w) / 2;
        const int32_t at_y =
            static_cast<int32_t>(places[index].y) + static_cast<int32_t>(places[index].h) / 2;
        const int32_t dx = at_x - from_x;
        const int32_t dy = at_y - from_y;
        const int32_t primary = x_sign != 0 ? dx * x_sign : dy * y_sign;
        int32_t off = x_sign != 0 ? dy : dx;
        if (off < 0)
            off = -off;
        if (primary <= 0 || off > primary * 2 + 4)
            continue;
        const int64_t score =
            static_cast<int64_t>(primary) * primary + static_cast<int64_t>(off) * off * 4;
        if (!any || score < best_score) {
            best = index;
            best_score = score;
            any = true;
        }
    }
    if (!any)
        return {};
    const std::string code(places[best].code);
    const kit::ControlId id = controls::keyboard_first + static_cast<kit::ControlId>(best);
    const kit::Control* key = kit::control_of(list, id);
    if (key != nullptr) {
        for (const kit::Item& item : list.items) {
            if (item.role != kit::Role::keyboard)
                continue;
            const bool clipped = item.clip.width > 0 && item.clip.height > 0;
            const kit::Rect& clip = clipped ? item.clip : item.rect;
            int32_t scroll = state.keyboard_scroll;
            if (key->rect.x < clip.x)
                scroll -= clip.x - key->rect.x;
            else if (key->rect.x + key->rect.width > clip.x + clip.width)
                scroll += key->rect.x + key->rect.width - (clip.x + clip.width);
            if (scroll < 0)
                scroll = 0;
            const int32_t limit = keyboard_scroll_limit(list);
            if (scroll > limit)
                scroll = limit;
            state.keyboard_scroll = scroll;
        }
    }
    state.focus_code = code;
    state.card_code.clear();
    focus_on(state, id);
    return redrawn();
}

/// Outlines the chosen action on the keyboard.
///
/// @param editor the action's keys
/// @param[in,out] state the picture
void find_on_keyboard(const Editor& editor, ScreenState& state) {
    state.outline = state.selected;
    state.view = View::keyboard;
    state.details_page = false;
    state.menu = Menu::none;
    state.card_code.clear();
    const std::vector<std::string> codes = keys_of(editor, state.selected);
    if (!codes.empty())
        state.focus_code = codes.front();
    const kit::ControlId id = key_control(state, state.focus_code);
    if (id != kit::no_control)
        focus_on(state, id);
    else
        focus_on(state, controls::view_keyboard);
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

/// The capture's primary button: a free chord is bound, a chord in use opens
/// the clash question with its first outcome marked and nothing changed.
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
        state.settling = false;
        state.outcome = 0;
        focus_on(state, controls::clash_confirm);
        return redrawn();
    }
    if (state.capture.state != CaptureState::complete)
        return {};
    return finish_apply(editor, state, ClashChoice::replace);
}

/// The clash question's confirm button: applies the marked outcome.
///
/// @param[in,out] editor the draft
/// @param[in,out] state the question
/// @return what the host is to do
EditorResult confirm_clash(Editor& editor, ScreenState& state) {
    if (state.prompt != Prompt::clash)
        return {};
    if (state.settling) {
        const keys::Clash clash = state.pending_clash;
        if (!settle(editor, clash, state.outcome == 0 ? Settle::keep_here : Settle::keep_there))
            return {};
        state.selected = clash.a;
        state.details_scroll = 0;
        close_prompt(state);
        return redrawn();
    }
    const bool swap = state.outcome == 1 && state.capture.replacing.has_value();
    return finish_apply(editor, state, swap ? ClashChoice::swap : ClashChoice::replace);
}

/// Settle… on a draft clash: the clash question, its first outcome marked.
///
/// @param editor the draft clash
/// @param[in,out] state the question
/// @return what the host is to do
EditorResult open_settle(Editor& editor, ScreenState& state) {
    const std::optional<keys::Clash> clash = editor.draft_clash(state.selected);
    if (!clash)
        return {};
    state.menu = Menu::none;
    state.prompt = Prompt::clash;
    state.pending_clash = *clash;
    state.settling = true;
    state.outcome = 0;
    focus_on(state, controls::clash_confirm);
    return redrawn();
}

/// Opens Find by key, listening for a chord.
///
/// @param[in,out] state the prompt
void start_finding(ScreenState& state) {
    state.menu = Menu::none;
    state.prompt = Prompt::find;
    state.finding = Capture{};
    focus_on(state, controls::find_close);
}

/// A Find by key row's Open ›: chooses that action in the list and closes.
/// The list shows the action's own category when the open one lacks it.
///
/// @param editor the rows
/// @param[in,out] state the list
/// @param index the row's place, from 0
/// @return what the host is to do
EditorResult choose_found(Editor& editor, ScreenState& state, std::size_t index) {
    if (state.prompt != Prompt::find || !state.finding.chord)
        return {};
    const std::vector<ChordUse> uses = find_by_chord(editor, *state.finding.chord);
    if (index >= uses.size())
        return {};
    const std::string id = uses[index].id;
    close_prompt(state);
    state.view = View::list;
    state.details_page = false;
    state.selected = id;
    state.details_scroll = 0;
    bool listed = false;
    for (const Row& row : editor.rows(state.category, state.filter, state.search.text)) {
        if (row.id == id)
            listed = true;
    }
    if (!listed) {
        state.category = "all";
        state.filter = Filter::all;
        state.search = {};
    }
    state.list_scroll = 0;
    focus_on(state, controls::list);
    return redrawn();
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
    const kit::Control* find_well = kit::control_of(list, controls::find_well);
    const bool in_find_well = find_well != nullptr && kit::contains(find_well->rect, at);
    if (kind == PointerKind::down && state.prompt == Prompt::find && in_find_well && button >= 1 &&
        button <= 8) {
        // A button in the well is the chord looked up, or listens again once
        // one is showing.
        if (!state.finding.chord) {
            keys::InputEvent event;
            event.kind = keys::InputKind::button;
            event.down = true;
            event.button = stored_button(button);
            capture_button(state.finding, editor, event);
            return redrawn();
        }
        state.finding = Capture{};
        return redrawn();
    }
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
    if (state.prompt == Prompt::share && state.interaction.focused == controls::share_name) {
        const kit::Control* field = kit::control_of(list, controls::share_name);
        if (field == nullptr)
            return std::nullopt;
        return field->rect;
    }
    if (state.prompt != Prompt::none)
        return std::nullopt;
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
        bool card_changed = false;
        if (state.view == View::keyboard) {
            const std::string code = code_of_key(state, kit::hit(list, at));
            if (code != state.card_code) {
                state.card_code = code;
                card_changed = true;
            }
        }
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
        if (moved.result == kit::PointerResult::redraw || card_changed)
            return redrawn();
        return {};
    }
    if (kind == PointerKind::down) {
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
        if (state.prompt == Prompt::share && state.interaction.focused == controls::share_name &&
            (pressed == kit::Key::left || pressed == kit::Key::right || pressed == kit::Key::home ||
             pressed == kit::Key::end || pressed == kit::Key::backspace ||
             pressed == kit::Key::delete_forward)) {
            if (kit::edit_text(state.share_field, pressed)) {
                editor.set_share_name(state.share_field.text);
                return redrawn();
            }
            return {};
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

    const kit::ControlId focused = state.interaction.focused;
    const bool on_view = focused == controls::view_list || focused == controls::view_keyboard ||
                         focused == controls::view_mouse;
    const bool on_layer = focused == controls::layer_none || focused == controls::layer_shift ||
                          focused == controls::layer_ctrl || focused == controls::layer_alt ||
                          focused == controls::layer_cmd;
    const bool on_scheme = focused == controls::scheme_right || focused == controls::scheme_left;
    const bool on_nav = focused >= controls::nav_first && focused < controls::row_first;
    const bool on_pill = filter_place(focused) >= 0;
    const bool on_key = focused >= controls::keyboard_first && focused < controls::mouse_first;
    if (on_nav && (pressed == kit::Key::up || pressed == kit::Key::down))
        return step_category(editor, state, list, pressed == kit::Key::down);
    if (on_pill && (pressed == kit::Key::left || pressed == kit::Key::right))
        return step_filter(state, list, pressed == kit::Key::right);
    if (on_view && (pressed == kit::Key::left || pressed == kit::Key::right)) {
        View shown = View::list;
        if (state.view == View::keyboard)
            shown = View::mouse;
        else if (state.view == View::mouse)
            shown = pressed == kit::Key::right ? View::list : View::keyboard;
        else
            shown = pressed == kit::Key::right ? View::keyboard : View::mouse;
        if (state.view == View::list && pressed == kit::Key::left)
            shown = View::mouse;
        if (state.view == View::keyboard && pressed == kit::Key::left)
            shown = View::list;
        if (state.view == View::keyboard && pressed == kit::Key::right)
            shown = View::mouse;
        show_view(editor, state, shown);
        return redrawn();
    }
    if (on_layer && (pressed == kit::Key::left || pressed == kit::Key::right)) {
        const uint8_t layer =
            next_layer(state.layer, pressed == kit::Key::right, editor.platform().primary_is_meta);
        state.layer = layer;
        focus_on(state, layer_control(layer));
        return redrawn();
    }
    if (on_scheme && (pressed == kit::Key::left || pressed == kit::Key::right)) {
        if (editor.scheme() == keys::PointerScheme::left_click) {
            editor.set_scheme(keys::PointerScheme::right_click);
            focus_on(state, controls::scheme_right);
        } else {
            editor.set_scheme(keys::PointerScheme::left_click);
            focus_on(state, controls::scheme_left);
        }
        return redrawn();
    }
    if (on_key && (pressed == kit::Key::left || pressed == kit::Key::right ||
                   pressed == kit::Key::up || pressed == kit::Key::down))
        return move_key(state, list, pressed);
    if (on_key && (pressed == kit::Key::enter || pressed == kit::Key::space)) {
        const std::string code = code_of_key(state, focused);
        if (!code.empty())
            return press_picture_key(editor, state, code);
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
        return leave(editor, state);
    return {};
}

EditorResult editor_text(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, std::string_view utf8
) {
    static_cast<void>(list);
    if (state.prompt == Prompt::share && state.interaction.focused == controls::share_name) {
        if (!kit::insert_text(state.share_field, utf8))
            return {};
        editor.set_share_name(state.share_field.text);
        return redrawn();
    }
    if (state.prompt != Prompt::none)
        return {};
    if (state.interaction.focused != controls::search)
        return {};
    if (!kit::insert_text(state.search, utf8))
        return {};
    state.list_scroll = 0;
    return redrawn();
}

EditorResult editor_wheel(
    Editor& editor, ScreenState& state, const kit::DisplayList& list, kit::Point at, float notches
) {
    if (state.prompt != Prompt::none) {
        if (state.prompt == Prompt::share || state.prompt == Prompt::paste ||
            state.prompt == Prompt::import) {
            const kit::Control* bar = kit::control_of(list, controls::exchange_bar);
            kit::ControlId block_id = controls::share_text;
            if (state.prompt == Prompt::paste)
                block_id = controls::paste_list;
            else if (state.prompt == Prompt::import)
                block_id = controls::import_list;
            const kit::Control* block = kit::control_of(list, block_id);
            const bool over_block = block != nullptr && kit::contains(block->rect, at);
            const bool over_bar = bar != nullptr && kit::contains(bar->rect, at);
            if (bar != nullptr && block != nullptr && (over_block || over_bar)) {
                const int32_t limit = std::max(bar->group - block->rect.height, int32_t{0});
                const int32_t step =
                    std::max(kit::metrics_of(size_of(list)).small_line, int32_t{1});
                const int32_t delta = notches > 0.0F ? -step : step;
                const int32_t next = std::clamp(state.exchange_scroll + delta, int32_t{0}, limit);
                if (next != state.exchange_scroll) {
                    state.exchange_scroll = next;
                    return redrawn();
                }
            }
        }
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
    if (state.view == View::keyboard && state.keys_as_list) {
        const kit::Control* keys_list = kit::control_of(list, controls::key_list);
        if (keys_list != nullptr && kit::contains(keys_list->rect, at)) {
            const int32_t line = std::max(kit::metrics_of(size_of(list)).small_line, int32_t{1});
            int32_t rows = 0;
            for (const kit::KeyPlace& place : kit::keyboard_places(state.shape))
                rows += static_cast<int32_t>(key_card(editor, place.code, state.layer).size());
            const int32_t limit = std::max(rows * line - keys_list->rect.height / 2, int32_t{0});
            const int32_t delta = notches > 0 ? -line * 3 : line * 3;
            const int32_t next = std::clamp(state.key_list_scroll + delta, int32_t{0}, limit);
            if (next == state.key_list_scroll)
                return {};
            state.key_list_scroll = next;
            return redrawn();
        }
    }
    if (state.view == View::keyboard && in_keyboard_well(list, at)) {
        const int32_t step = 24;
        const int32_t delta = notches > 0 ? -step : step;
        const int32_t next =
            std::clamp(state.keyboard_scroll + delta, int32_t{0}, keyboard_scroll_limit(list));
        if (next == state.keyboard_scroll)
            return {};
        state.keyboard_scroll = next;
        return redrawn();
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
    if (state.prompt != Prompt::none) {
        if (command == EditorCommand::back) {
            close_prompt(state);
            return redrawn();
        }
        // Menu on the close question is its Save.
        if (command == EditorCommand::save && state.prompt == Prompt::leave)
            return activate(editor, state, list, controls::leave_save);
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
        return leave(editor, state);
    }
    if (command == EditorCommand::save) {
        if (!editor.can_save())
            return {};
        return asked(EditorAction::save, {}, false);
    }
    if (command == EditorCommand::undo) {
        if (!editor.undo())
            return {};
        state.details_scroll = 0;
        return redrawn();
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
    if (state.prompt == Prompt::find) {
        // Find by key listens as the capture does, for an action of its own.
        // A finished chord is listed; a tap of Escape closes.
        if (state.finding.chord)
            return {};
        if (event.kind == keys::InputKind::key) {
            const CaptureEvent happened = capture_key(state.finding, editor, event);
            if (happened == CaptureEvent::cancelled) {
                close_prompt(state);
                return redrawn();
            }
            return happened == CaptureEvent::changed ? redrawn() : EditorResult{};
        }
        if (event.kind == keys::InputKind::button)
            capture_button(state.finding, editor, event);
        else if (event.kind == keys::InputKind::wheel)
            capture_wheel(state.finding, editor, event);
        return state.finding.chord ? redrawn() : EditorResult{};
    }
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
    if (state.prompt == Prompt::find) {
        if (state.finding.chord || state.finding.escape_down_ms == 0)
            return {};
        capture_tick(state.finding, editor, now_ms);
        return state.finding.chord ? redrawn() : EditorResult{};
    }
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
