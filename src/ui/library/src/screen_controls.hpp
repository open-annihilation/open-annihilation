// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What layout.cpp and input.cpp share: the numbers of the screen's controls,
// its two scroll areas, and the items each drop-down holds.
#pragma once

#include "oa/ui/kit/layout.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/screen.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oa::ui::library::controls {

/// The first tab's control; the others follow in Tab's order.
inline constexpr kit::ControlId first_tab = 1;
/// The tab strip's arrow toward the earlier tabs. It is not a tab.
/// The tabs occupy 1 through tab_total. These sit after MORE ▾ (14) and
/// before the filters (20), so the ninth tab never shares a number with them.
inline constexpr kit::ControlId tab_scroll_left = 15;
/// The tab strip's arrow toward the later tabs. It is not a tab.
inline constexpr kit::ControlId tab_scroll_right = 16;
/// The search field.
inline constexpr kit::ControlId search = 11;
/// Compact's filter drop-down (library.filter).
inline constexpr kit::ControlId filter_menu = 12;
/// The drop-down of the tags that do not fit (library.tags).
inline constexpr kit::ControlId tags_menu = 13;
/// The Compact details page's MORE ▾ (library.action).
inline constexpr kit::ControlId more_menu = 14;
/// The All chip or nav entry; New, Installed and Updates follow in Filter's order.
inline constexpr kit::ControlId first_filter = 20;
/// UPDATE ALL.
inline constexpr kit::ControlId update_all = 30;
/// The list pane.
inline constexpr kit::ControlId list = 40;
/// Compact's DETAILS.
inline constexpr kit::ControlId details = 50;
/// BACK on Compact's details page. It returns to the list.
inline constexpr kit::ControlId back = 51;
/// The shortcut still asks for Settings. The footer does not place this control.
inline constexpr kit::ControlId settings = 52;
/// CLOSE.
inline constexpr kit::ControlId close = 53;
/// The header's refresh icon.
inline constexpr kit::ControlId refresh = 54;
/// The header's close mark, to the right of the refresh icon. It asks for
/// the same close as the footer's CLOSE.
inline constexpr kit::ControlId close_x = 55;
/// The status line's label.
inline constexpr kit::ControlId status = 60;
/// The first queue line's label.
inline constexpr kit::ControlId queue = 61;
/// The playing note's label.
inline constexpr kit::ControlId note = 62;
/// The From fact's label.
inline constexpr kit::ControlId from = 63;
/// The Rules fact's label.
inline constexpr kit::ControlId rules = 64;
/// The Active tab's tile for the mod being played, or for the base game.
inline constexpr kit::ControlId active_mod = 65;
/// The Active tab's tile for the language in use.
inline constexpr kit::ControlId active_language = 66;
/// UNLOAD MOD, at the bottom right of the Active tab's mod tile while a mod
/// is played. It asks for SWITCH OFF.
inline constexpr kit::ControlId active_unload = 67;
/// ADD REGISTRY… on the Registries tab. 70–73 sit between UNLOAD MOD (67)
/// and the action buttons (100); 73 is the Active tab's maps search.
inline constexpr kit::ControlId add_registry = 70;
/// The Registries tab's amber note, when a change was refused.
inline constexpr kit::ControlId registries_notice = 71;
/// The note under the Registries list.
inline constexpr kit::ControlId registries_note = 72;
/// The Active tab's maps search (library.active.search). It is not the
/// catalogue search: that one is absent on Active.
inline constexpr kit::ControlId active_search = 73;
/// CHANGE… on the base game tile. The host opens the chooser.
inline constexpr kit::ControlId active_change = 74;
/// The chooser's first base game row. The next rows follow, one per base game.
inline constexpr kit::ControlId first_base_row = 13000000;
/// The most base game rows the chooser names.
inline constexpr std::size_t most_base_rows = 8;
/// CANCEL on the chooser.
inline constexpr kit::ControlId base_cancel = 13000010;
/// USE THIS GAME on the chooser.
inline constexpr kit::ControlId base_use = 13000011;
/// The shade behind the chooser. It takes a press so the Library under it does not.
inline constexpr kit::ControlId base_blocker = 13000012;

/// Tells whether a control is one of the chooser's base game rows.
///
/// @param id the control
/// @return true when it is a row
[[nodiscard]] inline bool base_row_control(kit::ControlId id) noexcept {
    return id >= first_base_row &&
           id < first_base_row + static_cast<kit::ControlId>(most_base_rows);
}

/// The base game tile's label. It takes no press.
inline constexpr kit::ControlId active_base = 75;
/// The expansions heading's label. It takes no press.
inline constexpr kit::ControlId active_expansions = 76;
/// The label of the lock under the expansion pills, while a pill is locked.
/// It takes no press.
inline constexpr kit::ControlId active_lock = 77;
/// The Active tab's KEYS tile. It takes no press.
inline constexpr kit::ControlId active_keys = 78;
/// CHANGE… on the KEYS tile. It opens Keys with the layout in use selected.
inline constexpr kit::ControlId active_keys_change = 79;
/// An entry's action button: this and the Action's value.
inline constexpr kit::ControlId first_action = 100;
/// A tag's chip or nav entry: this and the tag's place in tags_in_tab.
inline constexpr kit::ControlId first_tag = 1000;
/// An open drop-down's item: this and the item's place among its items,
/// below first_active_map.
inline constexpr kit::ControlId first_menu_item = 3000;
/// The Active tab's line on a map: this and the map's place among the
/// tiles, from 0, below first_row. The label takes no press.
inline constexpr kit::ControlId first_active_map = 6000;
/// The most maps whose lines and From links are named; the tiles after
/// them are drawn but not named.
inline constexpr std::size_t most_active_maps = 4000;
/// The Active tab's From link: this and the map's place among the tiles,
/// from 0. Past the Registries-tab controls, so a link is never a row.
inline constexpr kit::ControlId first_active_from = 9000000;
/// A package's tag in the details: this and the tag's place in that
/// package's tags. Past the Active tab's From links, and not a filter-row
/// tag, which stays in [first_tag, first_menu_item).
inline constexpr kit::ControlId first_detail_tag = 11000000;
/// The most tags one package's details name.
inline constexpr std::size_t most_detail_tags = 256;
/// The Active tab's first expansion pill. The rest follow in the built-in
/// order of that base game's expansions.
inline constexpr kit::ControlId first_expansion = 12000000;
/// The most expansion pills named. A base game has fewer.
inline constexpr std::size_t most_expansions = 64;
/// The keyboard picture on a layout's details. An area, not a key.
inline constexpr kit::ControlId keys_picture = 14000000;
/// The first key of that picture. The keys that follow are drawn and take
/// no focus; the area above is what automation names.
inline constexpr kit::ControlId first_keys_picture_key = 14000001;
/// A fact whose value opens another entry: this and the fact's place.
inline constexpr kit::ControlId first_fact_link = 15000000;
/// The most such facts one details pane names.
inline constexpr std::size_t most_fact_links = 64;
/// USE's question. Keep them on top is marked until the player chooses
/// Start clean.
inline constexpr kit::ControlId use_keep = 16000001;
/// Start clean: the player's own changes stay with the layout they sit on.
inline constexpr kit::ControlId use_aside = 16000002;
/// Every game.
inline constexpr kit::ControlId use_for_game = 16000003;
/// Only with the mod being played.
inline constexpr kit::ControlId use_for_mod = 16000004;
/// CANCEL. Escape answers it, and it writes nothing.
inline constexpr kit::ControlId use_cancel = 16000005;
/// USE. Enter answers it.
inline constexpr kit::ControlId use_confirm = 16000006;
/// The shade behind the question. It takes a press so the Library under it does not.
inline constexpr kit::ControlId use_blocker = 16000007;
/// What moves: an area whose text names each command, its key now and afterwards.
inline constexpr kit::ControlId use_moves = 16000008;
/// The note under the own-changes choice: where those changes stay.
inline constexpr kit::ControlId use_saved = 16000009;
/// The helpers' note: they act in every game.
inline constexpr kit::ControlId use_helpers_note = 16000010;
/// A helper switch: this and the helper's place in the question.
inline constexpr kit::ControlId first_use_helper = 16000100;
/// The most helper switches one question names.
inline constexpr std::size_t most_use_helpers = 16;

/// Tells whether a control is a fact that opens a key layout.
///
/// @param id the control
/// @return true for a link the details name
[[nodiscard]] inline bool fact_link_control(kit::ControlId id) noexcept {
    return id >= first_fact_link &&
           id < first_fact_link + static_cast<kit::ControlId>(most_fact_links);
}

/// Tells whether a control is a helper switch on USE's question.
///
/// @param id the control
/// @return true for a switch the question names
[[nodiscard]] inline bool use_helper_control(kit::ControlId id) noexcept {
    return id >= first_use_helper &&
           id < first_use_helper + static_cast<kit::ControlId>(most_use_helpers);
}

/// Tells whether a control is an expansion pill.
///
/// @param id the control
/// @return true for a pill the Active tab names
[[nodiscard]] constexpr bool expansion_control(kit::ControlId id) noexcept {
    return id >= first_expansion &&
           id < first_expansion + static_cast<kit::ControlId>(most_expansions);
}

/// Tells whether a control is an Active tab From link.
///
/// @param id the control
/// @return true for a link the map tiles name
[[nodiscard]] constexpr bool active_from_control(kit::ControlId id) noexcept {
    return id >= first_active_from &&
           id < first_active_from + static_cast<kit::ControlId>(most_active_maps);
}

/// Tells whether a control is a tag in the details pane.
///
/// @param id the control
/// @return true for a tag the selected package's details name
[[nodiscard]] constexpr bool detail_tag_control(kit::ControlId id) noexcept {
    return id >= first_detail_tag &&
           id < first_detail_tag + static_cast<kit::ControlId>(most_detail_tags);
}

/// A row: this and the entry's place in the model's visible entries, below first_row_icon.
inline constexpr kit::ControlId first_row = 10000;
/// A row's state icon that is pressed as GET or UPDATE: this and the
/// entry's place in the model's visible entries, below first_registry.
inline constexpr kit::ControlId first_row_icon = 1000000;
/// A Registries-tab switch: this and the registry's place in the inputs.
/// Switches occupy 2000000 until 3000000, REMOVE 3000000 until 4000000,
/// ADD MIRROR… 4000000 until 5000000, REFRESH 5000000 until 6000000, the
/// key icon 6000000 until 7000000, the address 7000000 until 8000000 and
/// the refresh line 8000000 until 9000000, after the row icons at 1000000.
inline constexpr kit::ControlId first_registry = 2000000;
/// A Registries-tab REMOVE: this and the registry's place in the inputs.
inline constexpr kit::ControlId first_registry_remove = 3000000;
/// A Registries-tab ADD MIRROR…: this and the registry's place in the inputs.
inline constexpr kit::ControlId first_registry_mirror = 4000000;
/// A Registries-tab REFRESH: this and the registry's place in the inputs.
inline constexpr kit::ControlId first_registry_refresh = 5000000;
/// A Registries-tab key icon: this and the registry's place in the inputs.
inline constexpr kit::ControlId first_registry_key = 6000000;
/// A Registries-tab address label: this and the registry's place in the inputs.
inline constexpr kit::ControlId first_registry_address = 7000000;
/// A Registries-tab refresh-time label: this and the registry's place in the inputs.
inline constexpr kit::ControlId first_registry_refreshed = 8000000;
/// The first number after the Registries-tab controls.
inline constexpr kit::ControlId registry_controls_end = 9000000;

/// The list's scroll area, the group of its rows.
inline constexpr int32_t list_group = 0;
/// The details' scroll area, the group of the controls in them.
inline constexpr int32_t details_group = 1;
/// The Active tab's maps pane. The mod tile, the language tile and the maps
/// search are not in it: they stay put while the maps scroll.
inline constexpr int32_t maps_group = 2;

/// Large's filter pane: its filters, tags and worlds, which scroll under its
/// headings. Tab stops on one of them, and the arrows move among them.
inline constexpr int32_t filters_group = 3;

/// Returns an action's button's control.
///
/// @param action the action
/// @return its number
[[nodiscard]] constexpr kit::ControlId action_control(Action action) noexcept {
    return first_action + static_cast<kit::ControlId>(action);
}

/// Tells whether a control is a row's.
///
/// @param id the control
/// @return true from first_row up to first_row_icon
[[nodiscard]] constexpr bool row_control(kit::ControlId id) noexcept {
    return id >= first_row && id < first_row_icon;
}

/// Tells whether a control is a row's state icon.
///
/// @param id the control
/// @return true from first_row_icon up to the Registries-tab controls
[[nodiscard]] constexpr bool row_icon_control(kit::ControlId id) noexcept {
    return id >= first_row_icon && id < first_registry;
}

/// Tells whether a control is a registry's switch.
///
/// @param id the control
/// @return true for a Registries-tab switch
[[nodiscard]] constexpr bool registry_switch_control(kit::ControlId id) noexcept {
    return id >= first_registry && id < first_registry_remove;
}

/// Tells whether a control is a registry's REMOVE.
///
/// @param id the control
/// @return true for a Registries-tab REMOVE
[[nodiscard]] constexpr bool registry_remove_control(kit::ControlId id) noexcept {
    return id >= first_registry_remove && id < first_registry_mirror;
}

/// Tells whether a control is a registry's ADD MIRROR….
///
/// @param id the control
/// @return true for a Registries-tab ADD MIRROR…
[[nodiscard]] constexpr bool registry_mirror_control(kit::ControlId id) noexcept {
    return id >= first_registry_mirror && id < first_registry_refresh;
}

/// Tells whether a control is a registry's REFRESH.
///
/// @param id the control
/// @return true for a Registries-tab REFRESH
[[nodiscard]] constexpr bool registry_refresh_control(kit::ControlId id) noexcept {
    return id >= first_registry_refresh && id < first_registry_key;
}

/// Tells whether a control is a registry's key icon.
///
/// @param id the control
/// @return true for a Registries-tab key icon
[[nodiscard]] constexpr bool registry_key_control(kit::ControlId id) noexcept {
    return id >= first_registry_key && id < first_registry_address;
}

/// Returns the registry a Registries-tab control belongs to.
///
/// @param id a switch, REMOVE, ADD MIRROR…, REFRESH, key icon, address or refresh line
/// @return its place in the inputs, from 0
[[nodiscard]] constexpr std::size_t registry_index(kit::ControlId id) noexcept {
    if (id >= first_registry_refreshed)
        return static_cast<std::size_t>(id - first_registry_refreshed);
    if (id >= first_registry_address)
        return static_cast<std::size_t>(id - first_registry_address);
    if (id >= first_registry_key)
        return static_cast<std::size_t>(id - first_registry_key);
    if (id >= first_registry_refresh)
        return static_cast<std::size_t>(id - first_registry_refresh);
    if (id >= first_registry_mirror)
        return static_cast<std::size_t>(id - first_registry_mirror);
    if (id >= first_registry_remove)
        return static_cast<std::size_t>(id - first_registry_remove);
    return static_cast<std::size_t>(id - first_registry);
}

/// Returns a filter's chip's or nav entry's control.
///
/// @param filter the filter
/// @return its number
[[nodiscard]] constexpr kit::ControlId filter_control(Filter filter) noexcept {
    return first_filter + static_cast<kit::ControlId>(filter);
}

/// Returns a tab's control.
///
/// @param tab the tab
/// @return its number
[[nodiscard]] constexpr kit::ControlId tab_control(Tab tab) noexcept {
    return first_tab + static_cast<kit::ControlId>(tab);
}

/// One item of a drop-down: its caption, its automation word and what
/// choosing it does, one of a filter, a tag or an action.
struct MenuItem {
    std::string caption{};            ///< the words shown, looked up
    std::string word{};               ///< the word automation names it by
    std::optional<Filter> filter{};   ///< the filter it shows, with every tag
    std::optional<std::size_t> tag{}; ///< the tag it shows, its place in tags_in_tab
    std::optional<Action> action{};   ///< the selected entry's action it asks for
    /// A heading over the items after it, such as WORLD: not a choice, never
    /// marked, pressed or named.
    bool heading{};
};

/// Lists a drop-down's items.
///
/// Compact's filter holds All, New, Installed and Updates with their counts,
/// then every tag of the tab, a world tag as its world; the tags drop-down
/// the tags without a chip or nav entry of their own, the world tags under
/// the heading WORLD; MORE the selected entry's actions after its first
/// three.
///
/// @param library the model
/// @param menu the drop-down
/// @param tag_shown for each tag of tags_in_tab, whether it has its own chip or nav entry
/// @return the items, top to bottom
[[nodiscard]] std::vector<MenuItem>
menu_items(const Library& library, MenuKind menu, const std::vector<bool>& tag_shown);

/// Returns what a row's state icon is pressed as: GET, GET WITH or UPDATE,
/// as the entry's first button asks for it, and only while that button is
/// enabled, so the icon keeps that button's rules and blocked state. A row
/// that is queued (CANCEL), that failed (RETRY), or that is installed and
/// up to date has none.
///
/// @param library the model
/// @param entry one of its entries
/// @return the action; none while the icon takes no press
[[nodiscard]] std::optional<Action> row_icon_action(const Library& library, const Entry& entry);

/// Returns the item a drop-down shows as chosen.
///
/// @param library the model
/// @param menu the drop-down
/// @param items its items
/// @return the item's place, or -1 for none
[[nodiscard]] int32_t
chosen_item(const Library& library, MenuKind menu, const std::vector<MenuItem>& items);

/// The rows a pane's parts take.
struct Extent {
    int32_t top{};    ///< the first part's top row
    int32_t bottom{}; ///< the row under the last part
};

/// Returns the rows taken by the items of a display list that a view clips,
/// from one item on: the parts of a pane that scrolls.
///
/// @param display the display list
/// @param view the pane's view, which clips its parts
/// @param first_item the first item to look at
/// @return the rows; none when no item from there is clipped by the view
[[nodiscard]] std::optional<Extent>
clipped_extent(const kit::DisplayList& display, const kit::Rect& view, std::size_t first_item);

/// Tells, for each tag of the tab, whether a display list holds its own chip
/// or nav entry.
///
/// @param display the display list
/// @param tags how many tags the tab has
/// @return one flag per tag
[[nodiscard]] std::vector<bool> tags_shown(const kit::DisplayList& display, std::size_t tags);

} // namespace oa::ui::library::controls
