// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How the settings dialog and the OA button are drawn, and the fonts they
// draw their texts in: a dark gunmetal panel with one-pixel raised edges,
// hairline rules, light text with muted hints, and one green accent for
// what is selected. The header and the button show the Open Annihilation
// icon, or the green OA mark when the host has no icon to give. The dialog
// is one display list, its items in the order they are drawn and its
// controls named; draw_dialog paints it.

#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/rows.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mod_profile/registry.hpp"

#include "developer.hpp"
#include "downloads_page.hpp"
#include "geometry.hpp"
#include "language_page.hpp"
#include "registered.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings {

namespace renderer = oa::ui::frontend_renderer;
namespace layout = geometry;
namespace kit = oa::ui::kit;

namespace {

using renderer::SourceRect;

/// The word each of the dialog's controls' names starts with.
constexpr std::string_view kNamePrefix = "settings";

/// Returns a control's name: the dialog's word, then the words given.
///
/// @param words the words after it, joined by dots
/// @return the name
std::string named(std::string_view words) {
    return std::string(kNamePrefix) + "." + std::string(words);
}

/// The pieces of the list the dialog's parts go into: the items in the
/// order they are drawn, and the controls grouped by the order a press
/// tries them.
struct Building {
    kit::DisplayList list;                ///< the items, in drawing order
    std::vector<kit::Control> menu;       ///< an open drop-down list's items
    std::vector<kit::Control> developer;  ///< Developer's list's rows
    std::vector<kit::Control> footer_own; ///< Show Active Only and Restore profile values
    std::vector<kit::Control> rows;       ///< the open section's rows
    std::vector<kit::Control> scroll;     ///< the scroll bar
    std::vector<kit::Control> footer;     ///< Restore defaults, Cancel and OK
    std::vector<kit::Control> nav;        ///< the sections' entries
    std::vector<kit::Control> header;     ///< the title bar's close mark
    /// The open section's controls Tab stops on, in the declared order: its
    /// rows that take a change, then on Developer its list's rows that take
    /// input, Show Active Only and Restore profile values while it is
    /// enabled. The footer's buttons and the sections' entries follow them.
    std::vector<kit::ControlId> section_tab;
    kit::Canvas measure{};      ///< what texts are measured in
    const DialogFonts* fonts{}; ///< the fonts; null to estimate widths
    /// The dialog's sizes, widened for a real font. Set before the parts are added.
    layout::Sizes fitted{};
    bool have_fitted = false; ///< fitted was set for this list
};

/// Returns the sizes the list is built from.
///
/// @param building the list
/// @param dialog the dialog
/// @return the fitted sizes, or the class's own
const layout::Sizes& dialog_sizes(const Building& building, const Dialog& dialog) {
    return building.have_fitted ? building.fitted : layout::sizes_of(dialog);
}

/// Returns how wide a text is in one of the dialog's fonts, or at the
/// estimated width without them.
///
/// @param fonts the fonts; null to estimate
/// @param font which of them
/// @return the measure, in source pixels
std::function<int32_t(std::string_view)> width_in(const DialogFonts* fonts, DialogFont font) {
    return [fonts, font](std::string_view text) {
        if (fonts != nullptr)
            return dialog_text_width(*fonts, font, text);
        return static_cast<int32_t>(kit::character_count(text)) * estimated_character_width;
    };
}

/// Appends an item.
///
/// @param[in,out] list the display list
/// @param role what it draws
/// @param rect where
/// @param clip what it is drawn clipped to; empty for the whole dialog
/// @return the item, to fill in
kit::Item&
item(kit::DisplayList& list, kit::Role role, const SourceRect& rect, const SourceRect& clip) {
    kit::Item& added = list.items.emplace_back();
    added.role = role;
    added.rect = rect;
    added.clip = clip;
    return added;
}

/// Appends a flat rectangle.
void fill(
    kit::DisplayList& list, const SourceRect& rect, kit::Colour colour, const SourceRect& clip
) {
    item(list, kit::Role::fill, rect, clip).colour = colour;
}

/// Appends a text in a box.
void text(
    kit::DisplayList& list,
    DialogFont font,
    std::string_view words,
    const SourceRect& box,
    kit::Align align,
    kit::Colour colour,
    const SourceRect& clip
) {
    kit::Item& added = item(list, kit::Role::text, box, clip);
    added.text = std::string(words);
    added.font = font;
    added.align = align;
    added.colour = colour;
}

/// Appends a one-bit mark at a point.
void mark(
    kit::DisplayList& list,
    kit::Mark which,
    kit::Point at,
    kit::Colour colour,
    const SourceRect& clip
) {
    kit::Item& added = item(list, kit::Role::mark, {at.x, at.y, 0, 0}, clip);
    added.colour = colour;
    added.look = kit::MarkLook{which, colour};
}

/// Appends a button.
void button(
    kit::DisplayList& list,
    const SourceRect& rect,
    kit::ButtonLook look,
    int32_t control,
    const SourceRect& clip
) {
    kit::Item& added = item(list, kit::Role::button, rect, clip);
    added.text = look.caption;
    added.control = control;
    added.state.hovered = look.hovered;
    added.state.pressed = look.held;
    added.state.disabled = !look.enabled;
    added.look = std::move(look);
}

/// Appends an Off/On switch.
void toggle(
    kit::DisplayList& list,
    const SourceRect& rect,
    kit::SwitchLook look,
    int32_t control,
    const SourceRect& clip
) {
    kit::Item& added = item(list, kit::Role::toggle, rect, clip);
    added.text = look.on ? look.on_caption : look.off_caption;
    added.control = control;
    added.state.hovered = look.hovered;
    added.state.on = look.on;
    added.state.locked = look.locked;
    added.look = std::move(look);
}

/// Appends the keyboard focus outline.
void focus_ring(
    kit::DisplayList& list, const SourceRect& rect, bool around, const SourceRect& clip
) {
    item(list, kit::Role::focus_ring, rect, clip).look = kit::FocusRingLook{around};
}

/// Appends a locked row's fade over a rectangle.
void fade(kit::DisplayList& list, const SourceRect& rect, const SourceRect& clip) {
    item(list, kit::Role::locked_fade, rect, clip);
}

/// Appends a scroll bar.
void scroll_bar(kit::DisplayList& list, const SourceRect& well, const SourceRect& thumb, bool hot) {
    item(list, kit::Role::scroll_bar, well, {}).look = kit::ScrollBarLook{thumb, hot};
}

/// Returns a control.
///
/// @param id its number
/// @param rect where it lies
/// @param name its automation name
/// @param kind what it is
/// @return the control, to fill in further
kit::Control
control_of(int32_t id, const SourceRect& rect, std::string name, kit::ControlKind kind) {
    kit::Control control;
    control.id = id;
    control.rect = rect;
    control.name = std::move(name);
    control.kind = kind;
    return control;
}

/// An Off/On switch's look, its captions already looked up.
kit::SwitchLook switch_look(bool on, bool hovered, bool locked) {
    return {
        on,
        hovered,
        locked,
        std::string(layout::shown_text(layout::off_text)),
        std::string(layout::shown_text(layout::on_text)),
    };
}

/// The header the dialog shows: its title, the second word, the version and,
/// while a shared game keeps running, the note.
///
/// @param dialog the dialog
/// @return the header's look
kit::HeaderLook dialog_header(const Dialog& dialog) {
    kit::HeaderLook header;
    header.width = layout::sizes_of(dialog).dialog_width;
    header.title = std::string(layout::shown_text(layout::title_text));
    header.title_width = layout::title_width;
    header.tracking = layout::heading_tracking;
    header.second = std::string(layout::shown_text(layout::title_suffix_text));
    header.second_width = layout::title_suffix_width;
    header.right_inset = oa::ui::kit::compact_metrics.button_height + layout::header_gap;
    // The close mark keeps the header's right. While a shared game is still
    // running, that note is the line the player needs, and the version gives
    // way so the note is not cut.
    if (dialog.locks.shared_game) {
        header.note = std::string(layout::shown_text(layout::shared_game_text));
        header.version_width = 0;
    } else {
        header.version = std::string(layout::shown_text(dialog.version));
        header.version_width = layout::version_width;
    }
    return header;
}

/// Tells whether a rectangle lies wholly inside another.
///
/// @param inner the inner rectangle
/// @param outer the outer rectangle
/// @return true when inner has room and none of it falls outside outer
bool inside_rect(const SourceRect& inner, const SourceRect& outer) noexcept;

/// Adds the section list: one item that draws it, and a control for each
/// entry, named for its section.
///
/// @param[in,out] building the list
/// @param dialog the dialog
void add_nav(Building& building, const Dialog& dialog) {
    kit::NavLook nav;
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    const int32_t body_height = sized.footer_rule_row - layout::body_top;
    nav.area = {layout::edge, layout::body_top, sized.list_width, body_height};
    nav.rule_column = sized.list_rule_column;
    if (dialog.kind == DialogKind::engine) {
        SourceRect divider = layout::list_divider(dialog.size_class);
        divider.width = sized.list_item_width - 2 * layout::list_divider_inset;
        nav.divider = divider;
    }
    const auto pages =
        dialog_pages(dialog.kind, dialog.touch, dialog.game_files, dialog.controller);
    const layout::EngineNav engine = layout::engine_nav(pages, dialog.size_class);
    const bool clip_nav =
        dialog.kind == DialogKind::engine && dialog.registered_pages.empty() && engine.scrolls;
    for (const Page page : pages) {
        const int32_t control = page_control(page);
        kit::NavEntry entry;
        entry.rect = layout::dialog_list_item(dialog, page);
        entry.rect.width = sized.list_item_width;
        entry.caption = std::string(layout::shown_text(layout::page_name(page)));
        entry.selected = page == dialog.page;
        entry.hovered = dialog.hovered == control || dialog.pressed == control;
        entry.focused = dialog.focused == control;
        kit::Control added = control_of(
            control,
            entry.rect,
            named("nav." + std::string(layout::page_word(page))),
            kit::ControlKind::tab
        );
        if (clip_nav && page != Page::developer)
            added.clip = engine.viewport;
        added.checked = entry.selected;
        added.text = entry.caption;
        building.nav.push_back(std::move(added));
        // Developer stays pinned. An entry that is not wholly in the room
        // above the divider stays in the tab order and is not drawn.
        if (clip_nav && page != Page::developer && !inside_rect(entry.rect, engine.viewport))
            continue;
        nav.entries.push_back(std::move(entry));
    }
    item(building.list, kit::Role::nav, nav.area, {}).look = std::move(nav);
}

/// Tells whether a rectangle lies wholly inside another.
///
/// @param inner the inner rectangle
/// @param outer the outer rectangle
/// @return true when inner has room and none of it falls outside outer
bool inside_rect(const SourceRect& inner, const SourceRect& outer) noexcept {
    return inner.width > 0 && inner.height > 0 && inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Adds the section list while registered pages are listed: the engine's
/// sections, then each owner's name and pages, scrolled as one list.
///
/// @param[in,out] building the list
/// @param dialog the dialog
void add_registered_nav(Building& building, const Dialog& dialog) {
    const NavLayout layout =
        layout_registered_nav(dialog, building.have_fitted ? &building.fitted : nullptr);
    const int32_t scroll = std::clamp(dialog.nav_scroll, int32_t{0}, layout.limit);
    kit::NavLook nav;
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    nav.area = layout.area;
    nav.rule_column = sized.list_rule_column;
    for (const NavPiece& piece : layout.pieces) {
        SourceRect drawn = piece.rect;
        drawn.y -= scroll;
        if (piece.kind != NavPiece::Kind::entry)
            continue;
        const bool selected = piece.registered ? dialog.page == Page::registered &&
                                                     dialog.registered_page == piece.index
                                               : dialog.page == piece.page;
        std::string name = piece.registered
                               ? named("nav." + dialog.registered_pages[piece.index].id)
                               : named("nav." + std::string(layout::page_word(piece.page)));
        kit::Control added =
            control_of(piece.control, drawn, std::move(name), kit::ControlKind::tab);
        added.clip = layout.viewport;
        added.group = registered_nav_group;
        added.checked = selected;
        added.text = piece.text;
        building.nav.push_back(std::move(added));
        if (!inside_rect(drawn, layout.viewport))
            continue;
        kit::NavEntry entry;
        entry.rect = drawn;
        entry.caption = piece.text;
        entry.selected = selected;
        entry.hovered = dialog.hovered == piece.control || dialog.pressed == piece.control;
        entry.focused = dialog.focused == piece.control;
        nav.entries.push_back(std::move(entry));
    }
    // The list's face and its entries. Rules and owner names follow, so they
    // paint over the face and stay clipped to it.
    item(building.list, kit::Role::nav, nav.area, {}).look = std::move(nav);
    for (const NavPiece& piece : layout.pieces) {
        if (piece.kind == NavPiece::Kind::entry)
            continue;
        SourceRect drawn = piece.rect;
        drawn.y -= scroll;
        if (piece.kind == NavPiece::Kind::rule) {
            kit::Item& rule = item(building.list, kit::Role::rule, drawn, layout.area);
            rule.colour = kit::colour::rule;
            continue;
        }
        text(
            building.list,
            DialogFont::small,
            piece.text,
            drawn,
            kit::Align::left,
            kit::colour::hint,
            layout.area
        );
    }
}

/// Adds the open registered page's rows from their specs.
///
/// @param[in,out] building the list
/// @param dialog the dialog
void add_registered_rows(Building& building, const Dialog& dialog) {
    const RegisteredContent content =
        registered_content(dialog, building.have_fitted ? &building.fitted : nullptr);
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    if (dialog.registered_page >= dialog.registered_pages.size())
        return;
    const RegisteredPage& page = dialog.registered_pages[dialog.registered_page];
    kit::RowsState state;
    state.name_prefix = std::string(kNamePrefix) + "." + page.id;
    state.hovered = dialog.hovered;
    state.pressed = dialog.pressed;
    state.focused = dialog.focused;
    state.group = layout::section_group;
    state.clip = sized.view_clip;
    kit::DisplayList rows;
    kit::add_rows(rows, content.placed, state);
    for (kit::Item& added : rows.items)
        building.list.items.push_back(std::move(added));
    building.section_tab = rows.tab_order;
    for (kit::Control& added : rows.controls) {
        const SourceRect control = added.rect;
        added.rect = {sized.content_left, control.y, sized.content_width, control.height};
        added.clip = {control.x, sized.view.y, control.width, sized.view.height};
        building.rows.push_back(std::move(added));
    }
}

/// Returns a row of Developer's list's name: its area's, its hack's, or its
/// hack's and its parameter's, with a set's value or a list's item.
///
/// @param row the row
/// @return the name
std::string list_row_name(const layout::ListRow& row) {
    namespace registry = oa::data::mod_profile::registry;
    if (row.kind == layout::ListRowKind::area) {
        const auto areas = developer_areas();
        const std::string_view area = row.area < areas.size() ? areas[row.area].name : "area";
        return named("hack-area." + layout::word_form(area, true));
    }
    const auto hacks = oa::data::mod_profile::standard_hacks();
    if (row.hack >= hacks.size())
        return named("hack");
    const registry::Entry& entry = *hacks[row.hack];
    std::string name = named("hack." + layout::word_form(entry.id, true));
    if (row.kind == layout::ListRowKind::hack)
        return name;
    const auto parameters = registry::parameters_of(entry);
    if (row.parameter < 0 || static_cast<std::size_t>(row.parameter) >= parameters.size())
        return name;
    const registry::Parameter& parameter = parameters[static_cast<std::size_t>(row.parameter)];
    name += "." + layout::word_form(parameter.name, false);
    if (row.item == layout::list_length)
        return name + ".length";
    if (row.item == layout::whole_parameter)
        return name;
    // A set's value is named by its word; a list's item by its number from 1.
    if (row.kind == layout::ListRowKind::toggle) {
        const auto words = registry::enum_values_of(parameter.value);
        const auto at = static_cast<std::size_t>(row.item);
        return name + "." +
               (at < words.size() ? layout::word_form(words[at], false) : std::to_string(at + 1));
    }
    return name + "." + std::to_string(row.item + 1);
}

/// Returns what Developer's list is drawn clipped to: its view, wider on each
/// side by a focus outline (Sizes::developer_view_clip at the view's place).
///
/// @param view the list's view
/// @return the clip
SourceRect developer_clip(const SourceRect& view) noexcept {
    return {
        view.x - layout::focus_inset, view.y, view.width + 2 * layout::focus_inset, view.height
    };
}

/// Adds one row of Developer's list: an area's or a hack's header, a line
/// under a hack, or a parameter's control.
///
/// @param[in,out] building the list
/// @param dialog the dialog
/// @param row the row
/// @param first the row is the list's first, which the line over the list tops
/// @param view the list's view (layout::developer_list_scroll)
void add_list_row(
    Building& building,
    const Dialog& dialog,
    const layout::ListRow& row,
    bool first,
    const SourceRect& view
) {
    kit::DisplayList& list = building.list;
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    const SourceRect clip = developer_clip(view);
    const bool header =
        row.kind == layout::ListRowKind::area || row.kind == layout::ListRowKind::hack;
    const bool takes = header || !row.locked;
    const bool hovered = takes && row.control != no_control &&
                         (dialog.hovered == row.control || dialog.pressed == row.control);
    const bool focused = takes && row.control != no_control && dialog.focused == row.control;
    if (row.kind == layout::ListRowKind::area && !first)
        fill(list, {sized.content_left, row.top, sized.content_width, 1}, kit::colour::rule, clip);
    if (header && hovered)
        fill(
            list,
            {row.control_area.x,
             row.control_area.y + 1,
             row.control_area.width,
             row.control_area.height - 1},
            kit::colour::hover,
            clip
        );
    const auto line = [&](DialogFont font,
                          std::string_view words,
                          const SourceRect& box,
                          kit::Align align,
                          kit::Colour colour) {
        text(list, font, layout::shown_text(words), box, align, colour, clip);
    };
    switch (row.kind) {
    case layout::ListRowKind::area:
        mark(
            list,
            row.open ? kit::Mark::arrow_open : kit::Mark::arrow_closed,
            {row.arrow.x, row.arrow.y},
            hovered ? kit::colour::text : kit::colour::hint,
            clip
        );
        line(DialogFont::regular, row.text, row.label, kit::Align::left, kit::colour::text);
        line(DialogFont::small, row.shown, row.value, kit::Align::right, kit::colour::hint);
        break;
    case layout::ListRowKind::hack:
        mark(
            list,
            row.open ? kit::Mark::arrow_open : kit::Mark::arrow_closed,
            {row.arrow.x, row.arrow.y},
            hovered ? kit::colour::text : kit::colour::hint,
            clip
        );
        line(
            DialogFont::small,
            row.text,
            row.label,
            kit::Align::left,
            row.on ? kit::colour::text : kit::colour::hint
        );
        toggle(
            list,
            row.toggle,
            switch_look(row.on, hovered && !row.locked, row.locked),
            row.locked ? no_control : row.control,
            clip
        );
        break;
    case layout::ListRowKind::id:
        line(DialogFont::small, row.text, row.label, kit::Align::left, kit::colour::quiet);
        break;
    case layout::ListRowKind::text:
        line(DialogFont::small, row.text, row.label, kit::Align::left, kit::colour::hint);
        break;
    case layout::ListRowKind::scope:
        line(DialogFont::small, row.text, row.label, kit::Align::left, kit::colour::lock);
        break;
    case layout::ListRowKind::heading:
        line(DialogFont::small, row.text, row.label, kit::Align::left, kit::colour::text);
        break;
    case layout::ListRowKind::toggle:
        line(DialogFont::small, row.text, row.label, kit::Align::left, kit::colour::text);
        toggle(
            list,
            row.control_area,
            switch_look(row.on, hovered, row.locked),
            takes ? row.control : no_control,
            clip
        );
        break;
    case layout::ListRowKind::slider: {
        line(DialogFont::small, row.text, row.label, kit::Align::left, kit::colour::text);
        line(DialogFont::small, row.shown, row.value, kit::Align::right, kit::colour::text);
        kit::Item& slider = item(list, kit::Role::slider, row.control_area, clip);
        slider.control = takes ? row.control : no_control;
        slider.text = row.shown;
        slider.state.hovered = hovered;
        slider.state.locked = row.locked;
        slider.look = kit::SliderLook{row.stop, row.stops, row.locked, hovered};
        break;
    }
    }
    // A control that takes no change, while Developer Mode is off, fades
    // as a locked row's does; its text keeps its strength.
    if (row.locked &&
        (row.kind == layout::ListRowKind::hack || row.kind == layout::ListRowKind::toggle ||
         row.kind == layout::ListRowKind::slider))
        fade(list, row.kind == layout::ListRowKind::hack ? row.toggle : row.control_area, clip);
    if (focused)
        focus_ring(list, row.control_area, !header, clip);
    if (!layout::list_row_takes_input(row))
        return;
    kit::ControlKind kind = kit::ControlKind::list_item;
    if (row.kind == layout::ListRowKind::toggle)
        kind = kit::ControlKind::toggle;
    else if (row.kind == layout::ListRowKind::slider)
        kind = kit::ControlKind::slider;
    kit::Control added = control_of(row.control, row.control_area, list_row_name(row), kind);
    added.clip = view;
    added.steps = true;
    added.group = layout::developer_list_group;
    added.checked = row.on;
    added.text = row.kind == layout::ListRowKind::slider ? row.shown : row.text;
    building.developer.push_back(std::move(added));
    building.section_tab.push_back(row.control);
}

/// Adds what lies under Developer's rows: its list clipped to the list's
/// view with the list's scroll bar while it scrolls, and the list's footer
/// with Show Active Only and Restore profile values.
///
/// @param[in,out] building the list
/// @param dialog the dialog
/// @param open Developer's rows and list (layout::open_rows)
void add_developer(Building& building, const Dialog& dialog, const layout::ScrolledRows& open) {
    kit::DisplayList& list = building.list;
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    const auto hot = [&](int32_t control) {
        return dialog.hovered == control || dialog.pressed == control;
    };
    // A view with no height has an empty clip, and an empty clip draws the
    // whole dialog: the list would paint across the rows and the footer.
    if (open.area.view.height > 0) {
        for (std::size_t index = 0; index < open.list.rows.size(); ++index)
            add_list_row(building, dialog, open.list.rows[index], index == 0, open.area.view);
    }
    // Scrolled from its top, the view's first row keeps a line.
    if (open.scroll > 0)
        fill(
            list,
            {sized.content_left, open.area.view.y, sized.content_width, 1},
            kit::colour::rule,
            developer_clip(open.area.view)
        );
    if (open.limit > 0)
        scroll_bar(
            list,
            open.area.well,
            layout::scroll_thumb(open.area, open.scroll, open.limit, open.content_height),
            hot(scroll_bar_control)
        );
    fill(
        list,
        {sized.content_left, sized.developer_footer_rule, sized.content_width, 1},
        kit::colour::rule,
        {}
    );
    const std::string label(
        layout::shown_text(
            layout::active_only_text(
                active_hack_count(dialog), oa::data::mod_profile::standard_hacks().size()
            )
        )
    );
    text(
        list,
        DialogFont::regular,
        label,
        sized.active_only_label,
        kit::Align::left,
        kit::colour::text,
        {}
    );
    const kit::SwitchLook active =
        switch_look(dialog.developer.active_only, hot(active_only_control), false);
    toggle(list, sized.active_only_switch, active, active_only_control, {});
    if (dialog.focused == active_only_control)
        focus_ring(list, sized.active_only_switch, true, {});
    kit::Control active_only = control_of(
        active_only_control,
        sized.active_only_switch,
        named("active-only"),
        kit::ControlKind::toggle
    );
    active_only.steps = true;
    active_only.checked = dialog.developer.active_only;
    active_only.text = dialog.developer.active_only ? active.on_caption : active.off_caption;
    building.footer_own.push_back(std::move(active_only));
    building.section_tab.push_back(active_only_control);
    // Restore profile values takes a press only while Developer Mode is on.
    // It lights while the pointer is over it or a press on it is held.
    const SourceRect& restore = sized.restore_profile_button;
    const bool enabled = developer::restore_profile_enabled(dialog);
    const std::string caption(layout::shown_text(layout::restore_profile_text));
    button(
        list,
        restore,
        {caption,
         kit::ButtonStyle::plain,
         enabled && hot(restore_profile_control),
         enabled && dialog.pressed == restore_profile_control,
         enabled},
        restore_profile_control,
        {}
    );
    if (enabled && dialog.focused == restore_profile_control)
        focus_ring(list, restore, true, {});
    kit::Control restore_profile = control_of(
        restore_profile_control, restore, named("restore-profile-values"), kit::ControlKind::button
    );
    restore_profile.enabled = enabled;
    restore_profile.text = caption;
    building.footer_own.push_back(std::move(restore_profile));
    if (enabled)
        building.section_tab.push_back(restore_profile_control);
}

/// Adds the open section: its heading, its rows clipped to the view they
/// scroll in, and its scroll bar while they scroll; on Developer, its list
/// and the list's footer under its rows.
///
/// @param[in,out] building the list
/// @param dialog the dialog
/// @param open the open section's rows (layout::open_rows)
void add_section(Building& building, const Dialog& dialog, const layout::ScrolledRows& open) {
    kit::DisplayList& list = building.list;
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    const std::string heading =
        dialog.page == Page::registered
            ? registered_heading(dialog)
            : std::string(layout::shown_text(layout::page_heading(dialog.page)));
    item(list, kit::Role::heading, sized.heading, {}).look = kit::HeadingLook{heading};
    if (dialog.page == Page::registered) {
        add_registered_rows(building, dialog);
        return;
    }
    // The rows as their specs show them, through the kit; a row the view
    // cuts shows the part inside it, its text and its focus outline
    // included. Developer's rows stay at its top while its list scrolls.
    kit::PlacedRows placed;
    const auto regular_width = width_in(building.fonts, DialogFont::regular);
    static_cast<void>(layout::place_section(
        dialog,
        dialog.page,
        layout::shown_locks(dialog),
        dialog.section_hooks,
        width_in(building.fonts, DialogFont::small),
        &placed,
        &sized,
        building.fonts != nullptr ? &regular_width : nullptr
    ));
    kit::scroll(placed, layout::developer_page(dialog) ? 0 : open.scroll);
    kit::RowsState state;
    state.name_prefix = std::string(kNamePrefix);
    state.hovered = dialog.hovered;
    state.pressed = dialog.pressed;
    state.focused = dialog.focused;
    state.open_menu = dialog.open_list;
    state.marked_button = static_cast<std::size_t>(dialog.folder_marked);
    state.hovered_button = dialog.folder_hovered;
    state.group = layout::section_group;
    state.clip = sized.view_clip;
    kit::DisplayList rows;
    kit::add_rows(rows, placed, state);
    for (kit::Item& added : rows.items)
        list.items.push_back(std::move(added));
    // A bundle being copied draws its bar in the row's own padding, so the
    // next row is not pushed down and the page can be drawn while the copy runs.
    if (dialog.page == Page::game_content) {
        for (std::size_t index = 0; index < dialog.game_content.size(); ++index) {
            const GameContentRow& bundle = dialog.game_content[index];
            if (bundle.bar_done < 0)
                continue;
            const std::size_t row_index = index + 1;
            if (row_index >= placed.rows.size())
                continue;
            const kit::PlacedRow& placed_row = placed.rows[row_index];
            const int32_t bar_width = std::min(int32_t{220}, sized.content_width);
            kit::add_progress(
                list,
                {sized.content_left, placed_row.top + placed_row.height - 8, bar_width, 6},
                bundle.bar_done,
                bundle.bar_whole > 0 ? bundle.bar_whole : 1
            );
            list.items.back().clip = sized.view_clip;
        }
    }
    // Tab stops on the rows that take a change, top to bottom.
    building.section_tab = rows.tab_order;
    // A row's control lies across its row, at the control's own line, so
    // that the arrows keep to the column of rows wherever on its line each
    // control sits. A press reaches the control itself where the view shows
    // it: the clip is the view narrowed to the control's columns.
    for (kit::Control& added : rows.controls) {
        const SourceRect control = added.rect;
        added.rect = {sized.content_left, control.y, sized.content_width, control.height};
        added.clip = {control.x, sized.view.y, control.width, sized.view.height};
        building.rows.push_back(std::move(added));
    }
    if (layout::developer_page(dialog)) {
        add_developer(building, dialog, open);
        return;
    }
    if (open.limit == 0)
        return;
    // Scrolled from its top, the view's first row keeps a line, so that the
    // cut there is the same hairline as a row's own.
    if (open.scroll > 0) {
        kit::Item& rule = item(
            list,
            kit::Role::rule,
            {sized.content_left, sized.view.y, sized.content_width, 1},
            sized.view_clip
        );
        rule.colour = kit::colour::rule;
    }
    scroll_bar(
        list,
        open.area.well,
        layout::scroll_thumb(open.area, open.scroll, open.limit, open.content_height),
        dialog.hovered == scroll_bar_control || dialog.pressed == scroll_bar_control
    );
}

/// Adds the title bar's close mark, at the top right. It asks for the same
/// close as Cancel.
///
/// @param[in,out] building the list
/// @param dialog the dialog
void add_header_close(Building& building, const Dialog& dialog) {
    const SourceRect rect = layout::header_close_button(layout::sizes_of(dialog));
    const bool held = dialog.pressed == close_x_control && dialog.hovered == close_x_control;
    const bool hovered = dialog.hovered == close_x_control;
    const std::string caption(layout::shown_text(layout::close_x_text));
    kit::ButtonLook look;
    look.caption = caption;
    look.style = kit::ButtonStyle::quiet;
    look.hovered = hovered;
    look.held = held;
    look.mark = kit::ButtonMark::close;
    button(building.list, rect, look, close_x_control, {});
    if (dialog.focused == close_x_control)
        focus_ring(building.list, rect, true, {});
    kit::Control added =
        control_of(close_x_control, rect, named("close-x"), kit::ControlKind::button);
    added.text = caption;
    building.header.push_back(std::move(added));
}

/// Adds Restore defaults, Cancel and OK.
///
/// @param[in,out] building the list
/// @param dialog the dialog
void add_footer(Building& building, const Dialog& dialog) {
    struct FooterButton {
        int32_t control{};
        std::string_view caption{};
        std::string_view name{};
    };

    const std::array<FooterButton, 3> buttons{{
        {restore_control, layout::restore_text, "restore-defaults"},
        {cancel_control, layout::cancel_text, "cancel"},
        {ok_control, layout::ok_text, "ok"},
    }};
    for (const FooterButton& footer : buttons) {
        const layout::Sizes& sized = dialog_sizes(building, dialog);
        const SourceRect rect = footer.control == restore_control  ? sized.restore_button
                                : footer.control == cancel_control ? sized.cancel_button
                                                                   : sized.ok_button;
        const bool held = dialog.pressed == footer.control && dialog.hovered == footer.control;
        const bool hovered = dialog.hovered == footer.control;
        const std::string caption(layout::shown_text(footer.caption));
        button(
            building.list,
            rect,
            {caption,
             footer.control == ok_control ? kit::ButtonStyle::accent : kit::ButtonStyle::plain,
             hovered,
             held,
             true},
            footer.control,
            {}
        );
        if (dialog.focused == footer.control)
            focus_ring(building.list, rect, true, {});
        kit::Control added =
            control_of(footer.control, rect, named(footer.name), kit::ControlKind::button);
        added.text = caption;
        building.footer.push_back(std::move(added));
    }
}

/// Adds an open drop-down's menu over the dialog, and a control for each
/// item it shows.
///
/// @param[in,out] building the list
/// @param dialog the dialog
/// @param open the open section's rows
void add_open_menu(Building& building, const Dialog& dialog, const layout::ScrolledRows& open) {
    if (dialog.open_list == no_control)
        return;
    for (const layout::Row& row : open.rows.rows) {
        const bool downloads_choice = row.downloads >= 0 && downloads::is_choice(row);
        if (row.control != dialog.open_list || row.lock != Lock::none)
            continue;
        if (!downloads_choice && layout::kind_of(row.setting) != kit::RowKind::choice)
            continue;
        const auto& spec =
            downloads_choice ? layout::row_spec(Setting::language) : layout::row_spec(row.setting);
        const layout::SettingsModel model = layout::reading(dialog.chosen, dialog);
        const int32_t choices =
            downloads_choice ? downloads::choice_count() : kit::row_count(spec, model);
        const SourceRect menu = layout::choice_list(
            row.control_area, static_cast<std::size_t>(choices), dialog.size_class
        );
        const int32_t shown = layout::shown_choices(static_cast<std::size_t>(choices));
        kit::ChoiceMenuLook look;
        look.first = dialog.list_first;
        look.total = choices;
        look.chosen =
            downloads_choice ? downloads::choice_index(dialog) : kit::row_index(spec, model);
        look.marked = dialog.list_marked;
        look.focus_shown = dialog.focused != no_control;
        layout::fill_language_menu(dialog, row.setting, look);
        for (int32_t place = 0; place < shown; ++place) {
            const int32_t choice = dialog.list_first + place;
            if (choice >= choices)
                break;
            const std::string caption =
                downloads_choice
                    ? downloads::choice_caption(choice)
                    : std::string(layout::shown_text(kit::row_caption(spec, model, choice)));
            const std::string word = downloads_choice ? downloads::choice_id(choice)
                                                      : layout::choice_word(spec, model, choice);
            look.shown.push_back(
                downloads_choice ? std::string(layout::shown_text(caption)) : caption
            );
            // A divider is a heading: it is not a control and has no name.
            if (!downloads_choice && static_cast<std::size_t>(choice) < look.items.size() &&
                look.items[static_cast<std::size_t>(choice)].divider)
                continue;
            kit::Control added = control_of(
                layout::menu_item_control(choice),
                layout::choice_item(menu, place),
                named(
                    (downloads_choice ? std::string("content-updates")
                                      : std::string(layout::setting_name(row.setting))) +
                    "." + word
                ),
                kit::ControlKind::list_item
            );
            added.checked = choice == look.chosen;
            added.text = look.shown.back();
            building.menu.push_back(std::move(added));
        }
        item(building.list, kit::Role::choice_menu, menu, {}).look = std::move(look);
        return;
    }
}

} // namespace

namespace geometry {

kit::DisplayList dialog_list(const Dialog& dialog, const DialogFonts* fonts) {
    Building building;
    building.fonts = fonts;
    const auto regular = width_in(fonts, DialogFont::regular);
    const auto small = width_in(fonts, DialogFont::small);
    building.fitted = fonts != nullptr ? fitted_sizes(dialog, &regular, &small) : sizes_of(dialog);
    building.have_fitted = true;
    const Sizes& sized = building.fitted;
    const SourceRect whole{0, 0, sized.dialog_width, sized.dialog_height};
    fill(building.list, whole, kit::colour::panel, {});
    kit::HeaderLook header = dialog_header(dialog);
    if (fonts != nullptr) {
        const int32_t tracked =
            kit::tracked_width(*fonts, kit::FontRole::regular, header.second, header.tracking);
        if (tracked > header.second_width)
            header.second_width = tracked;
    }
    item(building.list, kit::Role::header, {0, 0, sized.dialog_width, header_rule_row + 1}, {})
        .look = std::move(header);
    add_header_close(building, dialog);
    if (registered_nav(dialog))
        add_registered_nav(building, dialog);
    else
        add_nav(building, dialog);
    const ScrolledRows open = open_rows(
        dialog,
        fonts != nullptr ? &small : nullptr,
        &building.fitted,
        fonts != nullptr ? &regular : nullptr
    );
    add_section(building, dialog, open);
    item(
        building.list,
        kit::Role::footer_band,
        {0, sized.footer_rule_row, sized.dialog_width, footer_height + 1},
        {}
    )
        .look = kit::FooterBandLook{sized.dialog_width, sized.footer_rule_row};
    add_footer(building, dialog);
    if (open.limit > 0) {
        kit::Control bar = control_of(
            scroll_bar_control, open.area.hit, named("scroll-bar"), kit::ControlKind::scroll_bar
        );
        bar.focusable = false;
        building.scroll.push_back(std::move(bar));
    }
    item(building.list, kit::Role::bevel, whole, {}).colour = kit::colour::edge_light;
    // An open drop-down list lies over everything else.
    add_open_menu(building, dialog, open);
    kit::DisplayList list = std::move(building.list);
    // Tab: the open section's controls, the footer's buttons left to right,
    // then the sections' entries in the list's order.
    list.tab_order = std::move(building.section_tab);
    // Tab still reaches the rows, the footer and the list first. The close
    // mark is the title bar's only stop, so it is last in that order, and
    // last overall.
    for (const auto* group : {&building.footer, &building.nav, &building.header})
        for (const kit::Control& control : *group)
            list.tab_order.push_back(control.id);
    for (auto* group :
         {&building.menu,
          &building.developer,
          &building.footer_own,
          &building.rows,
          &building.scroll,
          &building.header,
          &building.footer,
          &building.nav})
        for (kit::Control& control : *group)
            list.controls.push_back(std::move(control));
    return list;
}

} // namespace geometry

DialogFonts load_dialog_fonts(oa::AssetStore& assets) {
    return kit::load_game_fonts(assets);
}

int32_t dialog_text_width(const DialogFonts& fonts, DialogFont font, std::string_view text) {
    return kit::text_width(fonts, font, text);
}

void draw_dialog(
    renderer::Surface& target,
    const renderer::Placement& placement,
    const Dialog& dialog,
    const DialogFonts& fonts,
    const renderer::RgbaPicture& icon
) {
    kit::paint({&target, placement, &fonts, icon}, layout::dialog_list(dialog, &fonts));
}

void draw_dialog(const kit::Canvas& canvas, const Dialog& dialog) {
    if (canvas.fonts == nullptr)
        return;
    kit::paint(canvas, layout::dialog_list(dialog, canvas.fonts));
}

void draw_oa_button(
    renderer::Surface& target,
    const renderer::Placement& placement,
    int32_t side,
    ButtonLook look,
    const DialogFonts& fonts,
    const renderer::RgbaPicture& icon
) {
    kit::draw_oa_button(
        {&target, placement, &fonts, icon}, side, static_cast<kit::OaButtonState>(look)
    );
}

void draw_oa_mark(
    renderer::Surface& target,
    const renderer::Placement& placement,
    int32_t side,
    const renderer::RgbaPicture& icon
) {
    kit::draw_oa_mark({&target, placement, nullptr, icon}, side);
}

} // namespace oa::ui::engine_settings
