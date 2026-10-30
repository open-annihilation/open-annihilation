// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's input: the pointer and a finger, the keys through the kit's
// abstract keys, typed text, the wheel, and the Library's own keys and
// controller buttons. What it knows of the screen it reads from the display
// list the screen shows, and every rule it applies is the model's.
#include "oa/ui/library/screen.hpp"

#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/use_question.hpp"
#include "screen_controls.hpp"
#include "visible.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::ui::library {

namespace {

/// Turns a pill press into what the host is asked for.
///
/// @param pressed what the model did
/// @return the screen's answer; empty when the press did nothing
ScreenResult content_press_result(const ContentPress& pressed) {
    ScreenResult answered;
    answered.expansion = pressed.expansion;
    answered.registry = pressed.registry;
    switch (pressed.kind) {
    case ContentPressKind::none:
        return {};
    case ContentPressKind::redraw:
        answered.redraw = true;
        return answered;
    case ContentPressKind::ask:
        answered.action = Action::ask_expansion_off;
        answered.redraw = true;
        return answered;
    case ContentPressKind::add:
        answered.action = Action::add_expansion;
        return answered;
    case ContentPressKind::download:
        answered.action = Action::get;
        answered.redraw = true;
        return answered;
    case ContentPressKind::cancel_download:
        answered.action = Action::cancel;
        answered.redraw = true;
        return answered;
    case ContentPressKind::retry_download:
        answered.action = Action::retry;
        answered.redraw = true;
        return answered;
    }
    return {};
}

/// How many tabs there are, in Tab's order.
constexpr auto tab_controls = static_cast<int32_t>(tab_total);
/// How many filters there are, in Filter's order.
constexpr auto filter_controls = static_cast<int32_t>(filter_total);
/// How far a notch of the wheel scrolls the details, in points: a full list row.
constexpr int32_t details_step = kit::compact_metrics.list_row_height;
/// How far a mouse moves sideways, in points, before a press on the tab strip
/// becomes a drag.
constexpr int32_t mouse_drag_points = 3;

/// The part of a control a press reaches.
///
/// @param control the control
/// @return its rectangle, within its clip when it has one
kit::Rect shown_area(const kit::Control& control) {
    if (control.clip.width > 0 && control.clip.height > 0)
        return kit::intersect(control.rect, control.clip);
    return control.rect;
}

/// Tells whether the pointer rests on a disabled UNLOAD MOD. hit passes
/// over a disabled control, and the button's hint still belongs to it.
///
/// @param list the display list
/// @param at the pointer
/// @return true when it rests on that button and the button cannot act
bool over_disabled_unload(const kit::DisplayList& list, kit::Point at) {
    for (const kit::Control& control : list.controls) {
        if (control.id != controls::active_unload)
            continue;
        return !control.enabled && kit::contains(shown_area(control), at);
    }
    return false;
}

/// A pane's scroll, as the display list shows it.
struct PaneScroll {
    kit::Rect view{}; ///< what the pane is seen through
    int32_t offset{}; ///< how far it is scrolled, in points
    int32_t limit{};  ///< the greatest offset, in points
    int32_t step{};   ///< how far a notch of the wheel scrolls it, in points
};

/// Returns a result that only redraws.
///
/// @return the result
ScreenResult redrawn() {
    ScreenResult result;
    result.redraw = true;
    return result;
}

/// Returns the height of the list's rows, from a row the list shows.
///
/// @param list the display list
/// @return the height, in points; none while no row shows
std::optional<int32_t> row_height_of(const kit::DisplayList& list) {
    for (const kit::Control& control : list.controls)
        if (controls::row_control(control.id) && control.rect.height > 0)
            return control.rect.height;
    return std::nullopt;
}

/// Returns the list's scroll as the display list shows it.
///
/// @param library the model
/// @param state the screen's state
/// @param list the display list
/// @return the scroll; none while the list shows no row
std::optional<PaneScroll>
list_scroll_of(const Library& library, const ScreenState& state, const kit::DisplayList& list) {
    const kit::Control* pane = kit::control_of(list, controls::list);
    const std::optional<int32_t> height = row_height_of(list);
    if (pane == nullptr || !height)
        return std::nullopt;
    PaneScroll scroll;
    scroll.view = pane->rect;
    scroll.step = controls::list_row_step(*height);
    const auto content = static_cast<int32_t>(library.visible.size()) * scroll.step;
    scroll.limit = kit::ScrollArea::limit_for(content, scroll.view.height);
    scroll.offset = std::clamp(state.list_scroll, int32_t{0}, scroll.limit);
    return scroll;
}

/// Returns the details' scroll as the display list shows it: their view
/// clips every control in them, and their parts run from the view's top
/// less the offset.
///
/// @param list the display list
/// @return the scroll; none while no details show
std::optional<PaneScroll> details_scroll_of(const kit::DisplayList& list) {
    const kit::Control* inside = nullptr;
    for (const kit::Control& control : list.controls)
        if (control.group == controls::details_group && control.clip.width > 0 &&
            control.clip.height > 0) {
            inside = &control;
            break;
        }
    if (inside == nullptr)
        return std::nullopt;
    const kit::Rect view = inside->clip;
    const std::optional<controls::Extent> extent = controls::clipped_extent(list, view, 0);
    if (!extent)
        return std::nullopt;
    PaneScroll scroll;
    scroll.view = view;
    scroll.step = details_step;
    scroll.offset = std::max(view.y - extent->top, int32_t{0});
    scroll.limit = kit::ScrollArea::limit_for(extent->bottom - extent->top, view.height);
    return scroll;
}

/// Returns the Active tab's scroll as the display list shows it. A map row's
/// clip is the view. When no row is on screen yet, the scroll bar's clip is
/// that same view, so the page can still be scrolled to the maps.
///
/// @param list the display list
/// @return the scroll; none while nothing on the Active tab scrolls
std::optional<PaneScroll> maps_scroll_of(const kit::DisplayList& list) {
    kit::Rect pane{};
    bool found = false;
    for (const kit::Control& control : list.controls) {
        if (control.group == controls::maps_group && control.clip.width > 0 &&
            control.clip.height > 0) {
            pane = control.clip;
            found = true;
            break;
        }
    }
    if (!found) {
        for (const kit::Item& item : list.items) {
            if (item.role != kit::Role::scroll_bar || item.clip.width <= 0 || item.clip.height <= 0)
                continue;
            pane = item.clip;
            found = true;
            break;
        }
    }
    if (!found)
        return std::nullopt;
    const std::optional<controls::Extent> extent = controls::clipped_extent(list, pane, 0);
    if (!extent)
        return std::nullopt;
    PaneScroll scroll;
    scroll.view = pane;
    scroll.step = details_step;
    scroll.offset = std::max(pane.y - extent->top, int32_t{0});
    scroll.limit = kit::ScrollArea::limit_for(extent->bottom - extent->top, pane.height);
    return scroll;
}

/// Returns Large's filter pane's scroll as the display list shows it: its
/// view clips every entry and heading, and they run from the view's top less
/// the offset.
///
/// @param list the display list
/// @return the scroll; none while the pane does not show
std::optional<PaneScroll> filters_scroll_of(const kit::DisplayList& list) {
    const kit::Control* inside = nullptr;
    for (const kit::Control& control : list.controls)
        if (control.group == controls::filters_group && control.clip.width > 0 &&
            control.clip.height > 0) {
            inside = &control;
            break;
        }
    if (inside == nullptr)
        return std::nullopt;
    const kit::Rect view = inside->clip;
    const std::optional<controls::Extent> extent = controls::clipped_extent(list, view, 0);
    if (!extent)
        return std::nullopt;
    PaneScroll scroll;
    scroll.view = view;
    scroll.step = inside->rect.height + kit::compact_metrics.hairline;
    scroll.offset = std::max(view.y - extent->top, int32_t{0});
    scroll.limit = kit::ScrollArea::limit_for(extent->bottom - extent->top, view.height);
    return scroll;
}

/// Returns the offset nearest another that shows a part of a pane whole,
/// or its top when it is taller than the view.
///
/// @param offset the offset now
/// @param top the part's top when the pane is not scrolled, from the view's top
/// @param height the part's height
/// @param view_height the view's height
/// @param limit the greatest offset
/// @return the offset
int32_t
offset_showing(int32_t offset, int32_t top, int32_t height, int32_t view_height, int32_t limit) {
    const int32_t lowest = top + height - view_height;
    return std::clamp(
        std::min(std::max(offset, lowest), top), int32_t{0}, std::max(limit, int32_t{0})
    );
}

/// Returns the selected entry's place among the visible entries.
///
/// @param library the model
/// @return the place; none with nothing selected
std::optional<std::size_t> selected_place(const Library& library) {
    if (!library.selected)
        return std::nullopt;
    for (std::size_t place = 0; place < library.visible.size(); ++place)
        if (library.entries[library.visible[place]].id == *library.selected)
            return place;
    return std::nullopt;
}

/// Scrolls the list so the selected row shows whole.
///
/// @param library the model
/// @param[in,out] state the screen's state; its list scroll changes
/// @param list the display list, which says the rows' height and the view
void show_selection(const Library& library, ScreenState& state, const kit::DisplayList& list) {
    const std::optional<PaneScroll> scroll = list_scroll_of(library, state, list);
    const std::optional<std::size_t> place = selected_place(library);
    if (!scroll || !place)
        return;
    state.list_scroll = offset_showing(
        scroll->offset,
        static_cast<int32_t>(*place) * scroll->step,
        scroll->step,
        scroll->view.height,
        scroll->limit
    );
}

/// Scrolls the details so a control in them shows whole.
///
/// @param[in,out] state the screen's state; its details scroll changes
/// @param list the display list
/// @param id the control
void show_in_details(ScreenState& state, const kit::DisplayList& list, kit::ControlId id) {
    const kit::Control* control = kit::control_of(list, id);
    if (control == nullptr || control->group != controls::details_group)
        return;
    const std::optional<PaneScroll> scroll = details_scroll_of(list);
    if (!scroll)
        return;
    const int32_t top = control->rect.y + scroll->offset - scroll->view.y;
    state.details_scroll = offset_showing(
        scroll->offset, top, control->rect.height, scroll->view.height, scroll->limit
    );
}

/// Scrolls Large's filter pane so an entry in it shows whole.
///
/// @param[in,out] state the screen's state; its filter pane's scroll changes
/// @param list the display list
/// @param id the entry
void show_in_filters(ScreenState& state, const kit::DisplayList& list, kit::ControlId id) {
    const kit::Control* control = kit::control_of(list, id);
    if (control == nullptr || control->group != controls::filters_group)
        return;
    const std::optional<PaneScroll> scroll = filters_scroll_of(list);
    if (!scroll)
        return;
    const int32_t top = control->rect.y + scroll->offset - scroll->view.y;
    state.filters_scroll = offset_showing(
        scroll->offset, top, control->rect.height, scroll->view.height, scroll->limit
    );
}

/// Moves the focus among Large's filter pane's entries with Up, Down, Home,
/// End, Page Up and Page Down while one of them has it, those out of view
/// among them, and scrolls the pane to show the one it reaches. Up from the
/// first entry and Down from the last go on as the arrows do elsewhere.
///
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param pressed the key
/// @return what the host is to do; none when the key goes on to the screen
std::optional<ScreenResult>
filter_pane_key(ScreenState& state, const kit::DisplayList& list, kit::Key pressed) {
    if (!state.interaction.focus_shown)
        return std::nullopt;
    const kit::Control* focused = kit::control_of(list, state.interaction.focused);
    if (focused == nullptr || focused->group != controls::filters_group)
        return std::nullopt;
    std::vector<kit::ControlId> entries;
    for (const kit::Control& control : list.controls)
        if (control.group == controls::filters_group && control.enabled)
            entries.push_back(control.id);
    const auto at = std::find(entries.begin(), entries.end(), focused->id);
    if (at == entries.end())
        return std::nullopt;
    const auto place = static_cast<int32_t>(at - entries.begin());
    const auto last = static_cast<int32_t>(entries.size()) - 1;
    const std::optional<PaneScroll> scroll = filters_scroll_of(list);
    const int32_t page = scroll && scroll->step > 0
                             ? std::max(scroll->view.height / scroll->step - 1, int32_t{1})
                             : 1;
    int32_t next = place;
    switch (pressed) {
    case kit::Key::up:
        if (place == 0)
            return std::nullopt;
        next = place - 1;
        break;
    case kit::Key::down:
        if (place == last)
            return std::nullopt;
        next = place + 1;
        break;
    case kit::Key::home:
        next = 0;
        break;
    case kit::Key::end:
        next = last;
        break;
    case kit::Key::page_up:
        next = std::max(place - page, int32_t{0});
        break;
    case kit::Key::page_down:
        next = std::min(place + page, last);
        break;
    default:
        return std::nullopt;
    }
    state.interaction.focused = entries[static_cast<std::size_t>(next)];
    show_in_filters(state, list, state.interaction.focused);
    return redrawn();
}

/// Scrolls the Active tab's maps so a control in them shows whole.
///
/// @param[in,out] state the screen's state; its maps scroll changes
/// @param list the display list
/// @param id the control
void show_in_maps(ScreenState& state, const kit::DisplayList& list, kit::ControlId id) {
    const kit::Control* control = kit::control_of(list, id);
    if (control == nullptr)
        return;
    const bool in_maps = control->group == controls::maps_group;
    const bool search = id == controls::active_search;
    if (!in_maps && !search)
        return;
    const std::optional<PaneScroll> scroll = maps_scroll_of(list);
    if (!scroll)
        return;
    // The search sits in the scrolled view only while the page scrolls as
    // one. Above the maps pane it stays put, and focusing it leaves the
    // pane's scroll as it is.
    if (search &&
        (control->clip.x != scroll->view.x || control->clip.y != scroll->view.y ||
         control->clip.width != scroll->view.width || control->clip.height != scroll->view.height))
        return;
    const int32_t top = control->rect.y + scroll->offset - scroll->view.y;
    state.maps_scroll = offset_showing(
        scroll->offset, top, control->rect.height, scroll->view.height, scroll->limit
    );
}

/// Tells whether the Compact details page shows: its BACK button is in the list.
///
/// @param list the display list
/// @return true while it shows
bool page_shows(const kit::DisplayList& list) {
    return kit::control_of(list, controls::back) != nullptr;
}

/// Tells whether the Compact list page shows: its DETAILS is in the list.
///
/// @param list the display list
/// @return true while it shows
bool compact_list_shows(const kit::DisplayList& list) {
    return kit::control_of(list, controls::details) != nullptr;
}

/// Tells whether the Active tab shows: its mod's line is in the list. Its
/// maps scroll on their own, unless the tiles leave them fewer than four
/// rows: then the page, tiles and maps, scrolls as one.
///
/// @param list the display list
/// @return true while it shows
bool active_shows(const kit::DisplayList& list) {
    return kit::control_of(list, controls::active_mod) != nullptr;
}

/// Tells whether the Registries tab shows: its note is in the list. Its
/// rows scroll as the details do.
///
/// @param list the display list
/// @return true while it shows
bool registries_shows(const kit::DisplayList& list) {
    return kit::control_of(list, controls::registries_note) != nullptr;
}

/// Tells whether the page keys scroll the details: the details page or the
/// Registries tab. The Active tab's page keys scroll its maps, or the whole
/// page when that page scrolls as one.
///
/// @param list the display list
/// @return true while they do
bool body_scrolls(const kit::DisplayList& list) {
    return page_shows(list) || registries_shows(list);
}

/// Returns the drop-down field a menu opens from.
///
/// @param menu the menu
/// @return its control
kit::ControlId field_of(MenuKind menu) {
    switch (menu) {
    case MenuKind::filter:
        return controls::filter_menu;
    case MenuKind::tags:
        return controls::tags_menu;
    case MenuKind::more:
        return controls::more_menu;
    case MenuKind::none:
        break;
    }
    return kit::no_control;
}

/// Tells whether a control is an open menu's item.
///
/// @param id the control
/// @return true for an item
bool menu_item_control(kit::ControlId id) {
    return id >= controls::first_menu_item && id < controls::first_active_map;
}

/// Returns the open drop-down's items.
///
/// @param library the model
/// @param state the screen's state
/// @param list the display list, which says which tags have their own chip
/// @return the items
std::vector<controls::MenuItem>
open_items(const Library& library, const ScreenState& state, const kit::DisplayList& list) {
    const std::size_t tags = tags_in_tab(library).size();
    return controls::menu_items(library, state.menu, controls::tags_shown(list, tags));
}

/// Moves the drop-down's mark, keeping it among the items shown. A heading
/// is never marked: the mark goes on past it the way it was moving, or back
/// when no item lies past it.
///
/// @param[in,out] state the screen's state
/// @param items the drop-down's items
/// @param marked the item to mark
void mark_item(ScreenState& state, const std::vector<controls::MenuItem>& items, int32_t marked) {
    const auto count = static_cast<int32_t>(items.size());
    if (count <= 0)
        return;
    const int32_t shown_count = kit::shown_choices(static_cast<std::size_t>(count));
    const int32_t way = marked < state.menu_marked ? -1 : 1;
    marked = std::clamp(marked, int32_t{0}, count - 1);
    const auto heading = [&items](int32_t place) {
        return items[static_cast<std::size_t>(place)].heading;
    };
    int32_t choice = marked;
    while (choice >= 0 && choice < count && heading(choice))
        choice += way;
    if (choice < 0 || choice >= count) {
        choice = marked;
        while (choice >= 0 && choice < count && heading(choice))
            choice -= way;
    }
    if (choice < 0 || choice >= count)
        return;
    state.menu_marked = choice;
    state.menu_first =
        std::clamp(state.menu_first, state.menu_marked - shown_count + 1, state.menu_marked);
    state.menu_first =
        std::clamp(state.menu_first, int32_t{0}, std::max(count - shown_count, int32_t{0}));
}

/// Brings the screen up to date after the model shows other entries: the
/// drop-down closes, the list shows the selection and the details their top.
///
/// @param library the model
/// @param[in,out] state the screen's state
/// @param list the display list
void after_browsing(const Library& library, ScreenState& state, const kit::DisplayList& list) {
    state.menu = MenuKind::none;
    state.list_scroll = 0;
    state.details_scroll = 0;
    show_selection(library, state, list);
}

/// Searches for the search field's text, keeping the field's text the
/// model's query, at most max_query_bytes.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list
void search_for_field(Library& library, ScreenState& state, const kit::DisplayList& list) {
    set_query(library, state.search.text);
    if (state.search.text != library.query) {
        state.search.text = library.query;
        state.search.caret = std::min(state.search.caret, state.search.text.size());
    }
    after_browsing(library, state, list);
}

/// Gives the search field the focus.
///
/// @param[in,out] state the screen's state
void focus_search(ScreenState& state) {
    state.menu = MenuKind::none;
    state.interaction.focused = controls::search;
    state.interaction.focus_shown = true;
}

/// Gives the Active tab's maps search the focus.
///
/// @param[in,out] state the screen's state
void focus_maps_search(ScreenState& state) {
    state.menu = MenuKind::none;
    state.interaction.focused = controls::active_search;
    state.interaction.focus_shown = true;
}

/// Returns the first map link the pane shows, top to bottom and left to right.
///
/// @param list the display list
/// @return the link; none while no link shows
kit::ControlId first_map_link(const kit::DisplayList& list) {
    const kit::Control* best = nullptr;
    for (const kit::Control& control : list.controls) {
        if (!controls::active_from_control(control.id) || !control.focusable || !control.enabled)
            continue;
        if (shown_area(control).height <= 0 || shown_area(control).width <= 0)
            continue;
        if (best == nullptr || control.rect.y < best->rect.y ||
            (control.rect.y == best->rect.y && control.rect.x < best->rect.x))
            best = &control;
    }
    return best == nullptr ? kit::no_control : best->id;
}

/// Cuts the maps search to max_query_bytes.
///
/// @param[in,out] field the field
void cap_maps_search(kit::TextField& field) {
    if (field.text.size() <= max_query_bytes)
        return;
    field.text.assign(capped_query(field.text));
    field.caret = std::min(field.caret, field.text.size());
}

/// Returns the first enabled action of the selected entry.
///
/// @param library the model
/// @return its button's control; none without one
std::optional<kit::ControlId> first_enabled_action(const Library& library) {
    const Entry* entry = selected_entry(library);
    if (entry == nullptr)
        return std::nullopt;
    for (const ActionButton& button : entry_actions(library, *entry))
        if (button.enabled)
            return controls::action_control(button.action);
    return std::nullopt;
}

/// Opens the selected entry's details page at Compact.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @return what the host is to do
ScreenResult open_page(Library& library, ScreenState& state) {
    if (selected_entry(library) == nullptr)
        return {};
    open_details(library);
    state.details_page = library.details_open;
    state.details_scroll = 0;
    state.menu = MenuKind::none;
    if (state.interaction.focus_shown)
        state.interaction.focused = first_enabled_action(library).value_or(controls::back);
    return redrawn();
}

/// Leaves the details page for the list.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @return what the host is to do
ScreenResult close_page(Library& library, ScreenState& state) {
    close_details(library);
    state.details_page = false;
    state.menu = MenuKind::none;
    if (state.interaction.focus_shown)
        state.interaction.focused = controls::list;
    return redrawn();
}

/// Does what Enter does on the list: opens the details page at Compact,
/// and at Regular and Large moves the focus to the first enabled action.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @return what the host is to do
ScreenResult enter_list(Library& library, ScreenState& state, const kit::DisplayList& list) {
    if (compact_list_shows(list))
        return open_page(library, state);
    const std::optional<kit::ControlId> action = first_enabled_action(library);
    if (!action || kit::control_of(list, *action) == nullptr)
        return {};
    state.interaction.focused = *action;
    state.interaction.focus_shown = true;
    show_in_details(state, list, *action);
    return redrawn();
}

/// Moves the selection through the list and keeps it in view, the details
/// of the entry it reaches from their top.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param delta how many rows; negative moves up
/// @return true when the selection moved
bool move_in_list(Library& library, ScreenState& state, const kit::DisplayList& list, int delta) {
    const std::optional<std::size_t> before = selected_place(library);
    move_selection(library, delta);
    show_selection(library, state, list);
    if (selected_place(library) == before)
        return false;
    // Another entry's details start at their top.
    state.details_scroll = 0;
    return true;
}

/// Returns how many rows the list shows.
///
/// @param library the model
/// @param state the screen's state
/// @param list the display list
/// @return the rows, at least 1
int rows_shown(const Library& library, const ScreenState& state, const kit::DisplayList& list) {
    const std::optional<PaneScroll> scroll = list_scroll_of(library, state, list);
    if (!scroll || scroll->step <= 0)
        return 1;
    return std::max(scroll->view.height / scroll->step, int32_t{1});
}

/// Does what choosing a drop-down's item asks: shows a filter or a tag, or
/// asks for the selected entry's action.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param place the item's place among the drop-down's items
/// @return what the host is to do
ScreenResult
choose_item(Library& library, ScreenState& state, const kit::DisplayList& list, int32_t place) {
    const std::vector<controls::MenuItem> items = open_items(library, state, list);
    // A heading chooses nothing; the drop-down stays open.
    if (place >= 0 && static_cast<std::size_t>(place) < items.size() &&
        items[static_cast<std::size_t>(place)].heading)
        return {};
    state.menu = MenuKind::none;
    if (place < 0 || static_cast<std::size_t>(place) >= items.size())
        return redrawn();
    const controls::MenuItem& item = items[static_cast<std::size_t>(place)];
    if (item.action) {
        ScreenResult result = redrawn();
        result.action = *item.action;
        if (library.selected)
            result.entry = *library.selected;
        return result;
    }
    if (item.filter) {
        set_tag(library, {});
        set_filter(library, *item.filter);
    } else if (item.tag) {
        const std::vector<std::pair<std::string, std::size_t>> tags = tags_in_tab(library);
        if (*item.tag < tags.size())
            set_tag(library, tags[*item.tag].first);
    }
    after_browsing(library, state, list);
    return redrawn();
}

/// Opens a drop-down, or closes it when it is open, marking its chosen item.
///
/// @param library the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param menu the drop-down
/// @return what the host is to do
ScreenResult toggle_menu(
    const Library& library, ScreenState& state, const kit::DisplayList& list, MenuKind menu
) {
    if (state.menu == menu) {
        state.menu = MenuKind::none;
        return redrawn();
    }
    state.menu = menu;
    state.menu_first = 0;
    const std::vector<controls::MenuItem> items = open_items(library, state, list);
    const int32_t chosen = controls::chosen_item(library, menu, items);
    state.menu_marked = 0;
    mark_item(state, items, std::max(chosen, int32_t{0}));
    return redrawn();
}

/// Shows a tab. The text focus goes to its search, unless a key on the tab
/// strip opened it: the keys walking the strip stay on it.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param tab the tab
/// @param by_key a key opened it, rather than the pointer, a finger or a command
/// @return what the host is to do
ScreenResult
show_tab(Library& library, ScreenState& state, const kit::DisplayList& list, Tab tab, bool by_key) {
    const bool tab_focused = by_key && state.interaction.focus_shown &&
                             state.interaction.focused >= controls::first_tab &&
                             state.interaction.focused < controls::first_tab + tab_controls;
    if (state.details_page) {
        close_details(library);
        state.details_page = false;
    }
    set_tab(library, tab);
    after_browsing(library, state, list);
    // The keys walking the tab strip stay on it. Otherwise the text focus
    // goes to the tab's search, so the player can type at once; on the
    // Registries tab, which has none, no focus shows until a key asks.
    if (tab_focused)
        state.interaction.focused = controls::tab_control(tab);
    else if (tab == Tab::registries)
        state.interaction.focus_shown = false;
    else
        focus_tab_search(library, state);
    return redrawn();
}

/// Returns what a Registries-tab control asks for.
///
/// A switch's left half and Left turn it off, its right half and Right turn
/// it on, and Space or Enter toggles it. Nothing is asked when that is
/// already its state. REFRESH names the registry and asks for it while it
/// is on and not already being asked. The key icon asks for nothing. REMOVE
/// and ADD MIRROR… name the registry; ADD REGISTRY… does not. During a match
/// ADD REGISTRY… and ADD MIRROR… do nothing.
///
/// @param library the model
/// @param list the display list
/// @param id the control
/// @param key the key, when one was pressed
/// @param point the point, when the pointer let go on it
/// @return what the host is to do; none when the control is not one of these
std::optional<ScreenResult> registry_press(
    const Library& library,
    const kit::DisplayList& list,
    kit::ControlId id,
    std::optional<kit::Key> key,
    std::optional<kit::Point> point
) {
    const bool on_switch = controls::registry_switch_control(id);
    const bool on_remove = controls::registry_remove_control(id);
    const bool on_mirror = controls::registry_mirror_control(id);
    const bool on_refresh = controls::registry_refresh_control(id);
    const bool on_key = controls::registry_key_control(id);
    if (!on_switch && !on_remove && !on_mirror && !on_refresh && !on_key &&
        id != controls::add_registry)
        return std::nullopt;
    if (id == controls::add_registry) {
        if (library.inputs.in_match)
            return ScreenResult{};
        ScreenResult result = redrawn();
        result.action = Action::add_registry;
        return result;
    }
    if (on_key)
        return redrawn();
    const std::size_t index = controls::registry_index(id);
    if (index >= library.inputs.registries.size())
        return ScreenResult{};
    const Registry& registry = library.inputs.registries[index];
    if (on_refresh) {
        if (!registry.on || registry.refreshing)
            return ScreenResult{};
        ScreenResult result = redrawn();
        result.action = Action::refresh_registry;
        result.registry = registry.id;
        return result;
    }
    if (on_switch) {
        bool next = registry.on;
        if (key) {
            if (*key == kit::Key::left)
                next = false;
            else if (*key == kit::Key::right)
                next = true;
            else if (*key == kit::Key::space || *key == kit::Key::enter)
                next = !registry.on;
            else
                return ScreenResult{};
        } else if (point) {
            const kit::Control* control = kit::control_of(list, id);
            if (control == nullptr)
                return ScreenResult{};
            next = point->x >= control->rect.x + control->rect.width / 2;
        } else {
            next = !registry.on;
        }
        if (next == registry.on)
            return ScreenResult{};
        ScreenResult result = redrawn();
        result.action = Action::set_registry;
        result.registry = registry.id;
        result.registry_on = next;
        return result;
    }
    if (registry.built_in)
        return ScreenResult{};
    if (on_mirror && library.inputs.in_match)
        return ScreenResult{};
    ScreenResult result = redrawn();
    result.action = on_remove ? Action::remove_registry : Action::add_mirror;
    result.registry = registry.id;
    return result;
}

/// Opens the Keys tab on one layout, with that row in view. The list is
/// laid out on the next frame, so the scroll is left for reveal_row.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param id the layout
/// @return a redraw, and nothing for the host to do
ScreenResult open_keys_on(Library& library, ScreenState& state, const EntryId& id) {
    set_tab(library, Tab::keys);
    set_filter(library, Filter::all);
    set_query(library, {});
    set_tag(library, {});
    state.search.text.clear();
    state.search.caret = 0;
    close_details(library);
    state.details_page = false;
    state.menu = MenuKind::none;
    state.details_scroll = 0;
    state.list_scroll = 0;
    select(library, id);
    state.reveal_row = -1;
    if (library.selected) {
        for (std::size_t place = 0; place < library.visible.size(); ++place) {
            if (library.entries[library.visible[place]].id == *library.selected) {
                state.reveal_row = static_cast<int32_t>(place);
                break;
            }
        }
    }
    if (state.interaction.focus_shown)
        state.interaction.focused = controls::list;
    return redrawn();
}

/// Follows a mod's Key layouts fact onto that layout.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param id the fact's link
/// @return a redraw; nothing when the fact does not open a layout
ScreenResult open_key_fact(Library& library, ScreenState& state, kit::ControlId id) {
    const Entry* entry = selected_entry(library);
    if (entry == nullptr)
        return {};
    const auto index = static_cast<std::size_t>(id - controls::first_fact_link);
    const std::vector<Fact> shown = facts(library, *entry);
    if (index >= shown.size() || !shown[index].opens)
        return {};
    return open_keys_on(library, state, *shown[index].opens);
}

/// Opens USE's question for a layout and puts the focus on USE.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param entry the layout
/// @return a redraw, and nothing for the host to do yet
ScreenResult begin_key_use(Library& library, ScreenState& state, const Entry& entry) {
    open_key_use(library, entry);
    state.menu = MenuKind::none;
    state.interaction.focused = controls::use_confirm;
    state.interaction.focus_shown = true;
    return redrawn();
}

/// Answers a press while USE's question is open. Other controls are covered.
///
/// USE returns Action::use_layout and leaves the question open, so the host
/// can read key_use_result. CANCEL and Escape close it and write nothing.
///
/// @param[in,out] library the model
/// @param list the display list
/// @param id the control
/// @param key the key, when one was pressed on the control
/// @param point the point, when the pointer let go on the control
/// @return what the host is to do
ScreenResult key_use_press(
    Library& library,
    const kit::DisplayList& list,
    kit::ControlId id,
    std::optional<kit::Key> key,
    std::optional<kit::Point> point
) {
    if (id == controls::use_blocker)
        return {};
    if (id == controls::use_cancel) {
        close_key_use(library);
        return redrawn();
    }
    if (id == controls::use_confirm) {
        ScreenResult result = redrawn();
        result.action = Action::use_layout;
        result.entry = library.key_use.layout;
        return result;
    }
    if (id == controls::use_keep) {
        set_key_use_keep(library, true);
        return redrawn();
    }
    if (id == controls::use_aside) {
        set_key_use_keep(library, false);
        return redrawn();
    }
    if (id == controls::use_for_game) {
        set_key_use_scope(library, false);
        return redrawn();
    }
    if (id == controls::use_for_mod) {
        set_key_use_scope(library, true);
        return redrawn();
    }
    if (controls::use_helper_control(id)) {
        const auto index = static_cast<std::size_t>(id - controls::first_use_helper);
        if (!library.key_use.open || index >= library.key_use.helpers.size())
            return {};
        bool next = !library.key_use.helpers[index].on;
        if (key) {
            if (*key == kit::Key::left)
                next = false;
            else if (*key == kit::Key::right)
                next = true;
            else if (*key != kit::Key::space && *key != kit::Key::enter)
                return {};
        } else if (point) {
            const kit::Control* control = kit::control_of(list, id);
            if (control == nullptr)
                return {};
            next = point->x >= control->rect.x + control->rect.width / 2;
        }
        if (next == library.key_use.helpers[index].on)
            return {};
        set_key_use_helper(library, index, next);
        return redrawn();
    }
    return {};
}

/// Opens the Maps tab on the pack a From link names, with that pack's row
/// in view. The link is followed before the Maps list is laid out, so the
/// scroll is left for the next layout (reveal_row) rather than read from
/// the Active tab's list.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param id the link
/// @return a redraw, and nothing for the host to do
ScreenResult open_map_pack(Library& library, ScreenState& state, kit::ControlId id) {
    const auto index = static_cast<std::size_t>(id - controls::first_active_from);
    const ActiveSetup setup = active_setup(library);
    if (index >= setup.maps.size())
        return {};
    const EntryId pack = setup.maps[index].pack_id;
    // The filter written is the tab that is open, so Maps is open first.
    set_tab(library, Tab::maps);
    set_filter(library, Filter::all);
    set_query(library, {});
    set_tag(library, {});
    state.search.text.clear();
    state.search.caret = 0;
    close_details(library);
    state.details_page = false;
    state.menu = MenuKind::none;
    state.details_scroll = 0;
    state.list_scroll = 0;
    select(library, pack);
    state.reveal_row = -1;
    if (library.selected) {
        for (std::size_t place = 0; place < library.visible.size(); ++place) {
            if (library.entries[library.visible[place]].id == *library.selected) {
                state.reveal_row = static_cast<int32_t>(place);
                break;
            }
        }
    }
    if (state.interaction.focus_shown)
        state.interaction.focused = controls::list;
    return redrawn();
}

/// Does what a control does when it is pressed, or takes Space or Enter.
///
/// No press on a row gets, updates or installs anything: it selects the
/// row, and at Compact opens its details page. A row's state icon that
/// takes a press selects its row and asks for its GET or UPDATE, as that
/// button would, while the button allows it. A Registries-tab switch reads
/// the key or the point so each half sets its own state.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param id the control
/// @param key the key, when one was pressed on the control
/// @param point the point, when the pointer let go on the control
/// @return what the host is to do
ScreenResult activate(
    Library& library,
    ScreenState& state,
    const kit::DisplayList& list,
    kit::ControlId id,
    std::optional<kit::Key> key,
    std::optional<kit::Point> point
) {
    const kit::Control* control = kit::control_of(list, id);
    if (control == nullptr || !control->enabled)
        return {};
    if (library.key_use.open)
        return key_use_press(library, list, id, key, point);
    if (const std::optional<ScreenResult> registry = registry_press(library, list, id, key, point))
        return *registry;
    if (menu_item_control(id))
        return choose_item(library, state, list, id - controls::first_menu_item);
    state.menu = id == field_of(state.menu) ? state.menu : MenuKind::none;
    if (id == controls::tab_scroll_left || id == controls::tab_scroll_right) {
        const int32_t steps = id == controls::tab_scroll_right ? 1 : -1;
        state.tab_scroll = kit::tab_scroll_step(list, steps);
        state.tab_scrolled = true;
        return redrawn();
    }
    if (id >= controls::first_tab && id < controls::first_tab + tab_controls)
        return show_tab(
            library, state, list, static_cast<Tab>(id - controls::first_tab), key.has_value()
        );
    if (id >= controls::first_filter && id < controls::first_filter + filter_controls) {
        set_filter(library, static_cast<Filter>(id - controls::first_filter));
        after_browsing(library, state, list);
        return redrawn();
    }
    if (id >= controls::first_action && id < controls::first_tag) {
        const Action action = static_cast<Action>(id - controls::first_action);
        if (action == Action::use_layout) {
            const Entry* entry = selected_entry(library);
            if (entry == nullptr)
                return {};
            return begin_key_use(library, state, *entry);
        }
        ScreenResult result = redrawn();
        result.action = action;
        if (library.selected)
            result.entry = *library.selected;
        return result;
    }
    if (id >= controls::first_tag && id < controls::first_menu_item) {
        const std::vector<std::pair<std::string, std::size_t>> tags = tags_in_tab(library);
        const auto index = static_cast<std::size_t>(id - controls::first_tag);
        if (index < tags.size())
            set_tag(library, library.tag == tags[index].first ? std::string() : tags[index].first);
        after_browsing(library, state, list);
        return redrawn();
    }
    if (controls::detail_tag_control(id)) {
        const Entry* entry = selected_entry(library);
        if (entry == nullptr || entry->listing == nullptr)
            return {};
        const auto index = static_cast<std::size_t>(id - controls::first_detail_tag);
        const std::vector<std::string>& package_tags = entry->listing->tags;
        if (index < package_tags.size())
            set_tag(
                library, library.tag == package_tags[index] ? std::string() : package_tags[index]
            );
        after_browsing(library, state, list);
        return redrawn();
    }
    if (controls::row_icon_control(id)) {
        const auto place = static_cast<std::size_t>(id - controls::first_row_icon);
        if (place >= library.visible.size())
            return {};
        const EntryId entry_id = library.entries[library.visible[place]].id;
        const Entry* entry = find_entry(library, entry_id);
        const std::optional<Action> asked =
            entry != nullptr ? controls::row_icon_action(library, *entry) : std::nullopt;
        if (!asked)
            return {};
        if (library.selected != entry_id)
            state.details_scroll = 0;
        select(library, entry_id);
        ScreenResult result = redrawn();
        result.action = *asked;
        result.entry = entry_id;
        return result;
    }
    if (controls::row_control(id)) {
        const auto place = static_cast<std::size_t>(id - controls::first_row);
        if (place >= library.visible.size())
            return {};
        select(library, library.entries[library.visible[place]].id);
        state.details_scroll = 0;
        if (compact_list_shows(list))
            return open_page(library, state);
        return redrawn();
    }
    if (controls::active_from_control(id))
        return open_map_pack(library, state, id);
    if (id == controls::active_keys_change)
        return open_keys_on(library, state, active_setup(library).keys_id);
    if (controls::fact_link_control(id))
        return open_key_fact(library, state, id);
    if (controls::expansion_control(id)) {
        const ActiveContent face = active_content(library);
        const auto index = static_cast<std::size_t>(id - controls::first_expansion);
        if (index >= face.pills.size())
            return {};
        const ExpansionPill& pill = face.pills[index];
        // The download arrow is drawn inside the pill. GET and RETRY take a
        // press only while that arrow is on. A copied pill has no arrow.
        const bool pressable = pill.arrow ? pill.arrow_enabled : pill.enabled;
        if (!pressable)
            return {};
        return content_press_result(press_expansion(library, pill.id));
    }
    ScreenResult result = redrawn();
    switch (id) {
    case controls::search:
        focus_search(state);
        return result;
    case controls::active_search:
        focus_maps_search(state);
        return result;
    case controls::filter_menu:
        return toggle_menu(library, state, list, MenuKind::filter);
    case controls::tags_menu:
        return toggle_menu(library, state, list, MenuKind::tags);
    case controls::more_menu:
        return toggle_menu(library, state, list, MenuKind::more);
    case controls::update_all:
        result.action = Action::update_all;
        return result;
    case controls::details:
        return open_page(library, state);
    case controls::back:
        return close_page(library, state);
    case controls::refresh:
        if (!can_check_now(library))
            return {};
        result.action = Action::check_now;
        return result;
    case controls::settings:
        result.action = Action::settings;
        return result;
    case controls::close:
    case controls::close_x:
        result.action = Action::close;
        return result;
    case controls::active_change:
        if (!active_content(library).change_enabled)
            return {};
        result.action = Action::change_base_game;
        return result;
    case controls::base_cancel:
        close_base_chooser(library);
        return result;
    case controls::base_use: {
        const BaseUse used = use_marked_base_game(library);
        if (used.kind == BaseUseKind::needs_source) {
            result.action = Action::copy_base_game;
            result.base_game = used.id;
        }
        return result;
    }
    case controls::base_blocker:
        return {};
    case controls::active_unload:
        // The same action SWITCH OFF returns. The host asks switch_off_question
        // and, on SWITCH, plays no mod. The entry is the mod being played,
        // which the host requires to be an installed one; the list's selection
        // may be another entry, or none, while this tab shows.
        result.action = Action::switch_off;
        for (const Entry& entry : library.entries) {
            if (entry.id.kind == Kind::mod && entry.installed != nullptr &&
                entry.installed->playing) {
                result.entry = entry.id;
                break;
            }
        }
        return result;
    default:
        if (controls::base_row_control(id)) {
            const auto index = static_cast<std::size_t>(id - controls::first_base_row);
            if (!library.chooser.open || index >= library.chooser.rows.size())
                return {};
            mark_base_game(library, library.chooser.rows[index].id);
            return result;
        }
        break;
    }
    return {};
}

/// Takes a key while a drop-down is open: Up, Down, Page Up, Page Down,
/// Home and End move its mark, Enter and Space choose the marked item and
/// Escape closes it. Tab and Shift+Tab close it and move on.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param pressed the key
/// @return what the host is to do; none when the key goes on to the screen
std::optional<ScreenResult>
menu_key(Library& library, ScreenState& state, const kit::DisplayList& list, kit::Key pressed) {
    const std::vector<controls::MenuItem> items = open_items(library, state, list);
    const auto count = static_cast<int32_t>(items.size());
    const int32_t page = kit::shown_choices(static_cast<std::size_t>(std::max(count, int32_t{0})));
    switch (pressed) {
    case kit::Key::escape:
        state.menu = MenuKind::none;
        return redrawn();
    case kit::Key::up:
        mark_item(state, items, state.menu_marked - 1);
        return redrawn();
    case kit::Key::down:
        mark_item(state, items, state.menu_marked + 1);
        return redrawn();
    case kit::Key::page_up:
        mark_item(state, items, state.menu_marked - page);
        return redrawn();
    case kit::Key::page_down:
        mark_item(state, items, state.menu_marked + page);
        return redrawn();
    case kit::Key::home:
        state.menu_marked = 0;
        mark_item(state, items, 0);
        return redrawn();
    case kit::Key::end:
        state.menu_marked = count - 1;
        mark_item(state, items, count - 1);
        return redrawn();
    case kit::Key::enter:
    case kit::Key::space:
        return choose_item(library, state, list, state.menu_marked);
    case kit::Key::tab:
    case kit::Key::back_tab:
        state.menu = MenuKind::none;
        return std::nullopt;
    default:
        return ScreenResult{};
    }
}

/// Returns the Active tab's scroll bar, when that tab shows one.
///
/// @param list the display list
/// @return the bar; none while the maps fit or another tab shows
const kit::Item* maps_bar_item(const kit::DisplayList& list) {
    if (!active_shows(list))
        return nullptr;
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::scroll_bar)
            return &item;
    return nullptr;
}

/// Returns the pane a point lies in.
///
/// On the Active tab the maps pane and its scroll bar scroll. When the page
/// scrolls as one, that view is the whole page, so the tiles scroll too.
///
/// @param library the model
/// @param state the screen's state
/// @param list the display list
/// @param at the point
/// @return the list, the details, the maps or none
Pane pane_at(
    const Library& library, const ScreenState& state, const kit::DisplayList& list, kit::Point at
) {
    if (const std::optional<PaneScroll> scroll = list_scroll_of(library, state, list);
        scroll && kit::contains(scroll->view, at))
        return Pane::list;
    if (const std::optional<PaneScroll> scroll = filters_scroll_of(list);
        scroll && kit::contains(scroll->view, at))
        return Pane::filters;
    if (active_shows(list)) {
        if (const std::optional<PaneScroll> scroll = maps_scroll_of(list);
            scroll && kit::contains(scroll->view, at))
            return Pane::maps;
        if (const kit::Item* bar = maps_bar_item(list);
            bar != nullptr && kit::contains(bar->rect, at))
            return Pane::maps;
        return Pane::none;
    }
    if (const std::optional<PaneScroll> scroll = details_scroll_of(list);
        scroll && kit::contains(scroll->view, at))
        return Pane::details;
    return Pane::none;
}

/// Returns a pane's scroll.
///
/// @param library the model
/// @param state the screen's state
/// @param list the display list
/// @param pane the pane
/// @return its scroll; none while it does not show
std::optional<PaneScroll> scroll_of(
    const Library& library, const ScreenState& state, const kit::DisplayList& list, Pane pane
) {
    if (pane == Pane::list)
        return list_scroll_of(library, state, list);
    if (pane == Pane::details)
        return details_scroll_of(list);
    if (pane == Pane::maps)
        return maps_scroll_of(list);
    if (pane == Pane::filters)
        return filters_scroll_of(list);
    return std::nullopt;
}

/// Tells whether a rectangle lies wholly inside another.
///
/// @param inner the rectangle
/// @param outer the one it may lie in
/// @return true when it does
bool lies_in(const kit::Rect& inner, const kit::Rect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Returns a pane's scroll area, as its scroll bar and its content show it.
/// The Active tab's bar is the maps'. On the other tabs a pane's bar is the
/// one in its view, in the column at its right: the list's, the details',
/// the Registries tab's or the Compact details page's, the last two the
/// details'.
///
/// @param library the model
/// @param state the screen's state
/// @param list the display list
/// @param pane the pane
/// @return the area; none while the pane shows no bar
std::optional<kit::ScrollArea> bar_area(
    const Library& library, const ScreenState& state, const kit::DisplayList& list, Pane pane
) {
    const std::optional<PaneScroll> scroll = scroll_of(library, state, list, pane);
    if (!scroll || scroll->limit <= 0)
        return std::nullopt;
    const kit::Item* bar = nullptr;
    if (pane == Pane::maps) {
        bar = maps_bar_item(list);
    } else if (!active_shows(list)) {
        for (const kit::Item& item : list.items) {
            if (item.role == kit::Role::scroll_bar && lies_in(item.rect, scroll->view)) {
                bar = &item;
                break;
            }
        }
    }
    if (bar == nullptr)
        return std::nullopt;
    kit::ScrollArea area;
    area.view = scroll->view;
    area.well = bar->rect;
    area.content_height = scroll->limit + scroll->view.height;
    area.limit = scroll->limit;
    area.offset = scroll->offset;
    return area;
}

/// Scrolls a pane to an offset.
///
/// @param[in,out] state the screen's state
/// @param pane the pane
/// @param offset the offset, in points
void scroll_to(ScreenState& state, Pane pane, int32_t offset) {
    if (pane == Pane::list)
        state.list_scroll = offset;
    else if (pane == Pane::details)
        state.details_scroll = offset;
    else if (pane == Pane::maps)
        state.maps_scroll = offset;
    else if (pane == Pane::filters)
        state.filters_scroll = offset;
}

/// Turns a pane by the wheel's notches.
///
/// @param library the model
/// @param[in,out] state the screen's state
/// @param list the display list
/// @param pane the pane
/// @param notches the turn; positive towards the top
/// @return what the host is to do
ScreenResult turn_pane(
    const Library& library,
    ScreenState& state,
    const kit::DisplayList& list,
    Pane pane,
    float notches
) {
    const std::optional<PaneScroll> scroll = scroll_of(library, state, list, pane);
    if (!scroll)
        return {};
    kit::WheelCarry& carry = pane == Pane::list      ? state.list_wheel
                             : pane == Pane::maps    ? state.maps_wheel
                             : pane == Pane::filters ? state.filters_wheel
                                                     : state.details_wheel;
    const int32_t next =
        kit::wheel_offset(carry, notches, scroll->step, scroll->offset, scroll->limit);
    if (next == scroll->offset)
        return {};
    scroll_to(state, pane, next);
    return redrawn();
}

/// Returns the pane that holds the focus: the details while a control of
/// theirs has it, the maps while one of theirs has it or the Active tab
/// shows, else the list.
///
/// @param state the screen's state
/// @param list the display list
/// @return the pane
Pane focused_pane(const ScreenState& state, const kit::DisplayList& list) {
    const kit::Control* control = kit::control_of(list, state.interaction.focused);
    if (state.interaction.focus_shown && control != nullptr) {
        if (control->group == controls::details_group)
            return Pane::details;
        if (control->group == controls::maps_group)
            return Pane::maps;
        if (control->group == controls::filters_group)
            return Pane::filters;
    }
    if (active_shows(list))
        return Pane::maps;
    return body_scrolls(list) ? Pane::details : Pane::list;
}

} // namespace

void focus_tab_search(const Library& library, ScreenState& state) noexcept {
    if (library.tab == Tab::registries)
        return;
    if (library.tab == Tab::active)
        focus_maps_search(state);
    else
        focus_search(state);
}

std::optional<kit::Rect>
focused_field(const ScreenState& state, const kit::DisplayList& list) noexcept {
    if (!state.interaction.focus_shown)
        return std::nullopt;
    const kit::ControlId id = state.interaction.focused;
    if (id != controls::search && id != controls::active_search)
        return std::nullopt;
    const kit::Control* control = kit::control_of(list, id);
    if (control == nullptr)
        return std::nullopt;
    return control->rect;
}

ScreenResult library_pointer(
    Library& library,
    ScreenState& state,
    const kit::DisplayList& list,
    PointerKind kind,
    uint8_t button,
    kit::Point at,
    int32_t finger_reach
) {
    kit::Interaction& interaction = state.interaction;
    // The chooser and USE's question cover the Library. A press reaches
    // their rows and buttons, or the shade, and never the tabs under them.
    if (library.chooser.open || library.key_use.open) {
        switch (kind) {
        case PointerKind::move: {
            const kit::PointerOutcome outcome = kit::pointer_move(interaction, list, at);
            ScreenResult result;
            result.redraw = outcome.result != kit::PointerResult::none;
            return result;
        }
        case PointerKind::down: {
            if (button != 1)
                return {};
            state.drag = {};
            state.tab_drag = {};
            state.bar_held = Pane::none;
            const kit::PointerOutcome outcome = kit::pointer_down(interaction, list, at);
            ScreenResult result;
            result.redraw = outcome.result != kit::PointerResult::none;
            return result;
        }
        case PointerKind::up: {
            if (button != 1)
                return {};
            const kit::PointerOutcome outcome = kit::pointer_up(interaction, list, at);
            if (outcome.result == kit::PointerResult::activated)
                return activate(library, state, list, outcome.control, std::nullopt, at);
            ScreenResult result;
            result.redraw = outcome.result != kit::PointerResult::none;
            return result;
        }
        }
        return {};
    }
    switch (kind) {
    case PointerKind::move: {
        if (state.bar_held != Pane::none) {
            const std::optional<kit::ScrollArea> area =
                bar_area(library, state, list, state.bar_held);
            if (!area)
                return {};
            const int32_t next =
                area->offset_at(at.y - state.bar_grab, kit::compact_metrics.least_thumb_height);
            if (next == area->offset)
                return {};
            scroll_to(state, state.bar_held, next);
            return redrawn();
        }
        if (state.tab_drag.held) {
            const int32_t across = at.x - state.tab_drag.from_x;
            const int32_t start = finger_reach > 0 ? finger_reach + 1 : mouse_drag_points;
            if (!state.tab_drag.scrolling && std::abs(across) >= start) {
                // The press becomes a drag: it scrolls the strip, and presses nothing.
                state.tab_drag.scrolling = true;
                interaction.pressed = kit::no_control;
                interaction.finger_shift = {};
            }
            if (state.tab_drag.scrolling) {
                const int32_t next = kit::tab_scroll_by(list, state.tab_drag.scroll, -across);
                if (next == state.tab_scroll)
                    return {};
                state.tab_scroll = next;
                state.tab_scrolled = true;
                return redrawn();
            }
        }
        if (state.drag.pane != Pane::none) {
            const int32_t across = at.x - state.drag.from.x;
            const int32_t along = at.y - state.drag.from.y;
            if (!state.drag.scrolling &&
                (std::abs(along) > finger_reach || std::abs(across) > finger_reach)) {
                // The press becomes a drag: it scrolls, and presses nothing.
                state.drag.scrolling = true;
                interaction.pressed = kit::no_control;
                interaction.finger_shift = {};
            }
            if (state.drag.scrolling) {
                const std::optional<PaneScroll> scroll =
                    scroll_of(library, state, list, state.drag.pane);
                if (!scroll)
                    return {};
                const int32_t next =
                    std::clamp(state.drag.offset - along, int32_t{0}, scroll->limit);
                if (next == scroll->offset)
                    return {};
                scroll_to(state, state.drag.pane, next);
                return redrawn();
            }
        }
        const kit::PointerOutcome outcome = kit::pointer_move(interaction, list, at);
        ScreenResult result;
        result.redraw = outcome.result != kit::PointerResult::none;
        if (over_disabled_unload(list, at) && interaction.hovered != controls::active_unload) {
            interaction.hovered = controls::active_unload;
            result.redraw = true;
        }
        if (state.menu != MenuKind::none && menu_item_control(outcome.control)) {
            const int32_t marked = outcome.control - controls::first_menu_item;
            if (marked != state.menu_marked) {
                state.menu_marked = marked;
                result.redraw = true;
            }
        }
        return result;
    }
    case PointerKind::down: {
        if (button != 1)
            return {};
        state.drag = {};
        state.tab_drag = {};
        state.bar_held = Pane::none;
        if (state.menu != MenuKind::none) {
            const kit::ControlId under =
                finger_reach > 0 ? kit::reach(list, at, finger_reach).control : kit::hit(list, at);
            if (!menu_item_control(under) && under != field_of(state.menu)) {
                // A press outside an open drop-down only closes it.
                state.menu = MenuKind::none;
                return redrawn();
            }
        }
        for (const Pane pane : {Pane::maps, Pane::details, Pane::list, Pane::filters}) {
            const std::optional<kit::ScrollArea> area = bar_area(library, state, list, pane);
            if (!area || !kit::contains(area->well, at))
                continue;
            // On the thumb, the press grabs it at the row pressed; on the
            // well, the thumb's middle jumps to the pointer and the drag
            // starts there.
            const kit::Rect thumb = area->thumb(kit::compact_metrics.least_thumb_height);
            state.bar_held = pane;
            if (kit::contains(thumb, at)) {
                state.bar_grab = at.y - thumb.y;
            } else {
                state.bar_grab = thumb.height / 2;
                scroll_to(
                    state,
                    pane,
                    area->offset_at(at.y - state.bar_grab, kit::compact_metrics.least_thumb_height)
                );
            }
            interaction.pressed = kit::no_control;
            return redrawn();
        }
        const kit::PointerOutcome outcome =
            finger_reach > 0 ? kit::finger_down(interaction, list, at, finger_reach)
                             : kit::pointer_down(interaction, list, at);
        if (const std::optional<kit::Rect> strip = kit::tab_drag_area(list);
            strip && kit::contains(*strip, at)) {
            // A press on tabs that do not fit can become a drag that scrolls
            // them sideways; let go without moving, it opens a tab as before.
            state.tab_drag.held = true;
            state.tab_drag.from_x = at.x;
            state.tab_drag.scroll = state.tab_scroll;
        } else if (finger_reach > 0) {
            const Pane pane = pane_at(library, state, list, at);
            if (const std::optional<PaneScroll> scroll = scroll_of(library, state, list, pane)) {
                state.drag.pane = pane;
                state.drag.from = at;
                state.drag.offset = scroll->offset;
            }
        }
        ScreenResult result;
        result.redraw = outcome.result != kit::PointerResult::none;
        return result;
    }
    case PointerKind::up: {
        if (button != 1)
            return {};
        if (state.bar_held != Pane::none) {
            state.bar_held = Pane::none;
            state.drag = {};
            interaction.pressed = kit::no_control;
            return redrawn();
        }
        const bool dragged = state.drag.scrolling || state.tab_drag.scrolling;
        state.drag = {};
        state.tab_drag = {};
        if (dragged) {
            interaction.pressed = kit::no_control;
            interaction.finger_shift = {};
            return redrawn();
        }
        const kit::PointerOutcome outcome = kit::pointer_up(interaction, list, at);
        if (outcome.result == kit::PointerResult::activated)
            return activate(library, state, list, outcome.control, std::nullopt, at);
        ScreenResult result;
        result.redraw = outcome.result != kit::PointerResult::none;
        return result;
    }
    }
    return {};
}

ScreenResult
library_key(Library& library, ScreenState& state, const kit::DisplayList& list, kit::Key pressed) {
    kit::Interaction& interaction = state.interaction;
    if (library.key_use.open) {
        if (pressed == kit::Key::escape) {
            close_key_use(library);
            return redrawn();
        }
        // Enter answers USE wherever the focus sits. The other keys stay
        // with the question's own controls.
        if (pressed == kit::Key::enter)
            return activate(library, state, list, controls::use_confirm, pressed, std::nullopt);
        const kit::KeyOutcome outcome = kit::key(interaction, list, pressed);
        switch (outcome.result) {
        case kit::KeyResult::none:
            return {};
        case kit::KeyResult::redraw:
            return redrawn();
        case kit::KeyResult::to_control:
            return activate(library, state, list, outcome.control, pressed, std::nullopt);
        case kit::KeyResult::accept:
        case kit::KeyResult::scroll:
        case kit::KeyResult::cancel:
            return {};
        }
        return {};
    }
    if (library.chooser.open) {
        if (pressed == kit::Key::escape) {
            close_base_chooser(library);
            return redrawn();
        }
        const kit::KeyOutcome outcome = kit::key(interaction, list, pressed);
        switch (outcome.result) {
        case kit::KeyResult::none:
            return {};
        case kit::KeyResult::redraw:
            return redrawn();
        case kit::KeyResult::to_control:
            return activate(library, state, list, outcome.control, pressed, std::nullopt);
        case kit::KeyResult::accept:
        case kit::KeyResult::scroll:
        case kit::KeyResult::cancel:
            return {};
        }
        return {};
    }
    // A focus the screen no longer shows moves to the list, to the maps
    // search when a map link scrolled away, or to the first stop.
    if (interaction.focus_shown && kit::control_of(list, interaction.focused) == nullptr) {
        const bool map_link = controls::active_from_control(interaction.focused);
        if (kit::control_of(list, controls::list) != nullptr)
            interaction.focused = controls::list;
        else if (map_link && kit::control_of(list, controls::active_search) != nullptr)
            interaction.focused = controls::active_search;
        else
            interaction.focused = list.tab_order.empty() ? kit::no_control : list.tab_order.front();
    }
    if (state.menu != MenuKind::none) {
        if (const std::optional<ScreenResult> taken = menu_key(library, state, list, pressed))
            return *taken;
    }
    const kit::ControlId focus = interaction.focus_shown ? interaction.focused : kit::no_control;
    const bool on_search = focus == controls::search;
    const bool on_maps_search = focus == controls::active_search;
    const bool on_list = focus == controls::list;
    const bool on_drop_down = focus == controls::filter_menu || focus == controls::tags_menu ||
                              focus == controls::more_menu;
    // On the details page the page keys scroll its body, never the list it
    // hides; on the Registries tab they scroll those cards. The Active tab's
    // page keys scroll its maps, or the whole page when that page scrolls.
    const bool in_details = body_scrolls(list) || (focus != kit::no_control && !on_list &&
                                                   focused_pane(state, list) == Pane::details);
    if (const std::optional<ScreenResult> moved = filter_pane_key(state, list, pressed))
        return *moved;

    switch (pressed) {
    case kit::Key::escape:
        if (on_search && !state.search.text.empty()) {
            state.search = {};
            search_for_field(library, state, list);
            return redrawn();
        }
        if (on_maps_search && !state.maps_search.text.empty()) {
            state.maps_search = {};
            state.maps_scroll = 0;
            return redrawn();
        }
        if (page_shows(list))
            return close_page(library, state);
        {
            ScreenResult result;
            result.action = Action::close;
            return result;
        }
    case kit::Key::enter:
        if (on_search) {
            // The search is done: the focus goes to what it found.
            if (kit::control_of(list, controls::list) != nullptr)
                interaction.focused = controls::list;
            return redrawn();
        }
        if (on_maps_search) {
            const kit::ControlId link = first_map_link(list);
            if (link != kit::no_control) {
                interaction.focused = link;
                show_in_maps(state, list, link);
            }
            return redrawn();
        }
        if (on_list || focus == kit::no_control)
            return enter_list(library, state, list);
        if (on_drop_down)
            return activate(library, state, list, focus, std::nullopt, std::nullopt);
        // Enter on a switch toggles it. The kit sends Enter only to a button,
        // a link, a list row or a tab.
        if (controls::registry_switch_control(focus))
            return activate(library, state, list, focus, pressed, std::nullopt);
        break;
    case kit::Key::space:
        if (on_list)
            return enter_list(library, state, list);
        if (on_drop_down)
            return activate(library, state, list, focus, std::nullopt, std::nullopt);
        break;
    case kit::Key::up:
    case kit::Key::down:
        // On the list the arrows move the selection; past either end the focus leaves it.
        if (on_list && move_in_list(library, state, list, pressed == kit::Key::up ? -1 : 1))
            return redrawn();
        break;
    case kit::Key::left:
    case kit::Key::right:
        if (on_search || on_maps_search) {
            kit::TextField& field = on_search ? state.search : state.maps_search;
            if (kit::edit_text(field, pressed))
                return redrawn();
            // At the text's end the arrow moves the focus on.
            const kit::ControlId next = kit::focus_toward(
                list,
                focus,
                pressed == kit::Key::left ? kit::Direction::left : kit::Direction::right
            );
            if (next == kit::no_control)
                return {};
            interaction.focused = next;
            show_in_details(state, list, next);
            show_in_maps(state, list, next);
            return redrawn();
        }
        break;
    case kit::Key::backspace:
    case kit::Key::delete_forward:
        if (on_search && kit::edit_text(state.search, pressed)) {
            search_for_field(library, state, list);
            return redrawn();
        }
        if (on_maps_search && kit::edit_text(state.maps_search, pressed)) {
            cap_maps_search(state.maps_search);
            state.maps_scroll = 0;
            return redrawn();
        }
        return {};
    case kit::Key::home:
    case kit::Key::end:
        if (on_search)
            return kit::edit_text(state.search, pressed) ? redrawn() : ScreenResult{};
        if (on_maps_search)
            return kit::edit_text(state.maps_search, pressed) ? redrawn() : ScreenResult{};
        if (active_shows(list)) {
            const std::optional<PaneScroll> scroll = maps_scroll_of(list);
            if (!scroll)
                return {};
            const int32_t next = pressed == kit::Key::home ? 0 : scroll->limit;
            if (next == scroll->offset)
                return {};
            state.maps_scroll = next;
            return redrawn();
        }
        if (in_details) {
            const std::optional<PaneScroll> scroll = details_scroll_of(list);
            if (!scroll)
                return {};
            const int32_t next = pressed == kit::Key::home ? 0 : scroll->limit;
            if (next == scroll->offset)
                return {};
            state.details_scroll = next;
            return redrawn();
        }
        if (move_in_list(
                library,
                state,
                list,
                pressed == kit::Key::home ? -static_cast<int>(library.visible.size())
                                          : static_cast<int>(library.visible.size())
            ))
            return redrawn();
        return {};
    case kit::Key::page_up:
    case kit::Key::page_down: {
        if (active_shows(list)) {
            const std::optional<PaneScroll> scroll = maps_scroll_of(list);
            if (!scroll)
                return {};
            const int32_t page =
                std::max(scroll->view.height - kit::compact_metrics.small_line, int32_t{1});
            const int32_t next = std::clamp(
                scroll->offset + (pressed == kit::Key::page_up ? -page : page),
                int32_t{0},
                scroll->limit
            );
            if (next == scroll->offset)
                return {};
            state.maps_scroll = next;
            return redrawn();
        }
        if (in_details) {
            const std::optional<PaneScroll> scroll = details_scroll_of(list);
            if (!scroll)
                return {};
            const int32_t page =
                std::max(scroll->view.height - kit::compact_metrics.small_line, int32_t{1});
            const int32_t next = std::clamp(
                scroll->offset + (pressed == kit::Key::page_up ? -page : page),
                int32_t{0},
                scroll->limit
            );
            if (next == scroll->offset)
                return {};
            state.details_scroll = next;
            return redrawn();
        }
        const int rows = rows_shown(library, state, list);
        if (move_in_list(library, state, list, pressed == kit::Key::page_up ? -rows : rows))
            return redrawn();
        return {};
    }
    case kit::Key::yes:
    case kit::Key::no:
    case kit::Key::info:
        return {};
    case kit::Key::tab:
    case kit::Key::back_tab:
        break;
    }

    // The kit's keys: Tab's declared order, the arrows by where controls
    // sit, and Space or Enter on a focused control.
    const kit::KeyOutcome outcome = kit::key(interaction, list, pressed);
    switch (outcome.result) {
    case kit::KeyResult::none:
        return {};
    case kit::KeyResult::redraw:
        show_in_details(state, list, interaction.focused);
        show_in_maps(state, list, interaction.focused);
        show_in_filters(state, list, interaction.focused);
        return redrawn();
    case kit::KeyResult::to_control:
        return activate(library, state, list, outcome.control, pressed, std::nullopt);
    case kit::KeyResult::accept:
        return enter_list(library, state, list);
    case kit::KeyResult::scroll:
    case kit::KeyResult::cancel:
        return {};
    }
    return {};
}

ScreenResult library_text(
    Library& library, ScreenState& state, const kit::DisplayList& list, std::string_view utf8
) {
    if (library.chooser.open || library.key_use.open)
        return {};
    if (state.interaction.focus_shown && state.interaction.focused == controls::active_search) {
        if (kit::control_of(list, controls::active_search) == nullptr ||
            !kit::insert_text(state.maps_search, utf8))
            return {};
        cap_maps_search(state.maps_search);
        state.maps_scroll = 0;
        return redrawn();
    }
    if (!focused_field(state, list) || !kit::insert_text(state.search, utf8))
        return {};
    search_for_field(library, state, list);
    return redrawn();
}

ScreenResult library_wheel(
    Library& library, ScreenState& state, const kit::DisplayList& list, kit::Point at, float notches
) {
    if (library.chooser.open || library.key_use.open)
        return {};
    if (state.menu != MenuKind::none) {
        for (const kit::Item& item : list.items) {
            const auto* look = std::get_if<kit::ChoiceMenuLook>(&item.look);
            if (item.role != kit::Role::choice_menu || look == nullptr ||
                !kit::contains(item.rect, at))
                continue;
            if (!std::isfinite(notches))
                return {};
            const auto count = static_cast<int32_t>(open_items(library, state, list).size());
            const int32_t shown_count =
                kit::shown_choices(static_cast<std::size_t>(std::max(count, int32_t{0})));
            const int32_t first = std::clamp(
                state.menu_first - static_cast<int32_t>(std::lround(notches)),
                int32_t{0},
                std::max(count - shown_count, int32_t{0})
            );
            if (first == state.menu_first)
                return {};
            state.menu_first = first;
            return redrawn();
        }
    }
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::tabs || !kit::contains(item.rect, at))
            continue;
        if (!std::isfinite(notches))
            return {};
        const int32_t steps = -static_cast<int32_t>(std::lround(notches));
        if (steps == 0)
            return {};
        const int32_t next = kit::tab_scroll_step(list, steps);
        if (next == state.tab_scroll)
            return {};
        state.tab_scroll = next;
        state.tab_scrolled = true;
        return redrawn();
    }
    return turn_pane(library, state, list, pane_at(library, state, list, at), notches);
}

ScreenResult library_command(
    Library& library, ScreenState& state, const kit::DisplayList& list, ScreenCommand command
) {
    if (library.key_use.open) {
        if (command == ScreenCommand::back) {
            close_key_use(library);
            return redrawn();
        }
        return {};
    }
    if (library.chooser.open) {
        if (command == ScreenCommand::back) {
            close_base_chooser(library);
            return redrawn();
        }
        return {};
    }
    switch (command) {
    case ScreenCommand::find:
        if (page_shows(list)) {
            close_details(library);
            state.details_page = false;
        }
        if (active_shows(list))
            focus_maps_search(state);
        else
            focus_search(state);
        return redrawn();
    case ScreenCommand::next_tab:
    case ScreenCommand::previous_tab: {
        const int32_t step = command == ScreenCommand::next_tab ? 1 : tab_controls - 1;
        const auto tab =
            static_cast<Tab>((static_cast<int32_t>(library.tab) + step) % tab_controls);
        return show_tab(library, state, list, tab, false);
    }
    case ScreenCommand::back:
        if (state.menu != MenuKind::none) {
            state.menu = MenuKind::none;
            return redrawn();
        }
        if (page_shows(list))
            return close_page(library, state);
        {
            ScreenResult result;
            result.action = Action::close;
            return result;
        }
    case ScreenCommand::first_action: {
        const Entry* entry = selected_entry(library);
        if (entry == nullptr)
            return {};
        const std::vector<ActionButton> actions = entry_actions(library, *entry);
        if (actions.empty() || !actions.front().enabled)
            return {};
        const Action action = actions.front().action;
        if (action != Action::get && action != Action::update && action != Action::cancel &&
            action != Action::retry)
            return {};
        ScreenResult result;
        result.action = action;
        result.entry = entry->id;
        return result;
    }
    case ScreenCommand::refresh:
        if (!can_check_now(library))
            return {};
        {
            ScreenResult asked;
            asked.action = Action::check_now;
            return asked;
        }
    }
    return {};
}

ScreenResult
library_stick(Library& library, ScreenState& state, const kit::DisplayList& list, float notches) {
    if (library.chooser.open || library.key_use.open)
        return {};
    return turn_pane(library, state, list, focused_pane(state, list), notches);
}

} // namespace oa::ui::library
