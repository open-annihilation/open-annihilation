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

#include "cards.hpp"
#include "developer.hpp"
#include "downloads_page.hpp"
#include "geometry.hpp"
#include "language_page.hpp"
#include "registered.hpp"
#include "section_icons.hpp"
#include "updates_page.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
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
    header.height = layout::header_height;
    // The close mark keeps the header's right. While a shared game is still
    // running, that note is the line the player needs, and the version gives
    // way so the note is not cut.
    if (dialog.locks.shared_game) {
        header.note = std::string(layout::shown_text(layout::shared_game_text));
        header.version_width = 0;
    } else {
        header.version = std::string(layout::shown_text(dialog.version));
        header.version_width = layout::version_width;
        // Already translated by the host. Empty leaves the header as it was.
        if (!dialog.header_note.empty())
            header.note = dialog.header_note;
    }
    return header;
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

/// Appends a picture of the dialog's own, scaled to fill a rectangle.
///
/// @param[in,out] list the display list
/// @param rect where
/// @param picture the picture; it lives as long as the program
/// @param clip what it is drawn clipped to; empty for the whole dialog
void picture(
    kit::DisplayList& list,
    const SourceRect& rect,
    const renderer::RgbaPicture& picture,
    const SourceRect& clip
) {
    item(list, kit::Role::picture, rect, clip).look = kit::PictureLook{picture};
}

/// Adds the section list: its face and each entry's ground, marker and focus
/// ring, then each entry's icon and caption, the rules between its groups and
/// registered pages' owners, and a control for each entry, named for its
/// section. Entries the list's room does not show wholly are not drawn, and
/// keep their controls, so that Tab still reaches them.
///
/// @param[in,out] building the list
/// @param dialog the dialog
void add_nav(Building& building, const Dialog& dialog) {
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    const layout::NavLayout layout = layout::nav_layout(dialog, &sized);
    const int32_t scroll = std::clamp(dialog.nav_scroll, int32_t{0}, layout.limit);
    const bool scrolls = layout.limit > 0;
    kit::NavLook nav;
    nav.area = layout.area;
    nav.rule_column = sized.list_rule_column;

    struct Shown {
        SourceRect rect{};
        const layout::NavPiece* piece{};
        bool lit{};
    };

    std::vector<Shown> shown;
    for (const layout::NavPiece& piece : layout.pieces) {
        if (piece.kind != layout::NavPiece::Kind::entry)
            continue;
        SourceRect drawn = piece.rect;
        drawn.y -= scroll;
        const bool selected = piece.registered ? dialog.page == Page::registered &&
                                                     dialog.registered_page == piece.index
                                               : dialog.page == piece.page;
        const bool hovered = dialog.hovered == piece.control || dialog.pressed == piece.control;
        std::string name = piece.registered
                               ? named("nav." + dialog.registered_pages[piece.index].id)
                               : named("nav." + std::string(layout::page_word(piece.page)));
        kit::Control added =
            control_of(piece.control, drawn, std::move(name), kit::ControlKind::tab);
        if (scrolls) {
            added.clip = layout.viewport;
            added.group = registered_nav_group;
        }
        added.checked = selected;
        added.text = piece.text;
        building.nav.push_back(std::move(added));
        if (!inside_rect(drawn, layout.viewport))
            continue;
        // The kit draws the entry's ground, its marker and its focus ring;
        // the icon and the caption follow, right of the marker.
        kit::NavEntry entry;
        entry.rect = drawn;
        entry.selected = selected;
        entry.hovered = hovered;
        entry.focused = dialog.focused == piece.control;
        nav.entries.push_back(std::move(entry));
        shown.push_back({drawn, &piece, selected || hovered});
    }
    item(building.list, kit::Role::nav, nav.area, {}).look = std::move(nav);
    const layout::NavMetrics& metrics = sized.nav;
    for (const Shown& entry : shown) {
        // Updates' badge stands at the entry's right where the caption keeps
        // its whole width left of it; a caption that needs the room keeps it.
        int32_t badge_width = 0;
        const int32_t caption_room =
            entry.rect.width - metrics.text_left - layout::list_text_margin;
        const auto small = width_in(building.fonts, DialogFont::small);
        const auto regular = width_in(building.fonts, DialogFont::regular);
        if (!entry.piece->registered && entry.piece->page == Page::updates &&
            !dialog.updates_badge.empty() &&
            regular(entry.piece->text) + layout::list_text_margin + small(dialog.updates_badge) <=
                caption_room) {
            badge_width = small(dialog.updates_badge);
            text(
                building.list,
                DialogFont::small,
                dialog.updates_badge,
                {entry.rect.x + entry.rect.width - layout::list_text_margin - badge_width,
                 entry.rect.y,
                 badge_width,
                 entry.rect.height},
                kit::Align::right,
                kit::colour::accent_light,
                layout.area
            );
            badge_width += layout::list_text_margin;
        }
        // No icons where a section's name needs their room (fitted_sizes).
        if (metrics.icon_side > 0) {
            const SourceRect icon{
                entry.rect.x + metrics.icon_left,
                entry.rect.y + (entry.rect.height - metrics.icon_side) / 2,
                metrics.icon_side,
                metrics.icon_side,
            };
            picture(
                building.list,
                icon,
                icons::section_icon(
                    entry.piece->registered ? Page::registered : entry.piece->page,
                    entry.lit ? icons::Tone::lit : icons::Tone::quiet
                ),
                layout.area
            );
        }
        text(
            building.list,
            DialogFont::regular,
            entry.piece->text,
            {entry.rect.x + metrics.text_left,
             entry.rect.y,
             entry.rect.width - metrics.text_left - layout::list_text_margin - badge_width,
             entry.rect.height},
            kit::Align::left,
            entry.lit ? kit::colour::text : kit::colour::list_text,
            layout.area
        );
    }
    for (const layout::NavPiece& piece : layout.pieces) {
        if (piece.kind == layout::NavPiece::Kind::entry)
            continue;
        SourceRect drawn = piece.rect;
        drawn.y -= scroll;
        if (!inside_rect(drawn, layout.viewport))
            continue;
        if (piece.kind == layout::NavPiece::Kind::rule) {
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

/// Appends a card's face: the list's surface inside the control border,
/// rounded as a card is.
///
/// @param[in,out] list the display list
/// @param rect the card
/// @param clip what it is drawn clipped to
void card_face(kit::DisplayList& list, const SourceRect& rect, const SourceRect& clip) {
    kit::ButtonLook face;
    face.style = kit::ButtonStyle::inset;
    button(list, rect, std::move(face), no_control, clip);
}

/// Appends a card's lines of text.
///
/// @param[in,out] list the display list
/// @param lines the lines
/// @param clip what they are drawn clipped to
void card_lines(
    kit::DisplayList& list, const std::vector<cards::Line>& lines, const SourceRect& clip
) {
    for (const cards::Line& line : lines)
        text(list, line.font, line.text, line.rect, line.align, line.colour, clip);
}

/// Appends one of a card's buttons and its control, which a press reaches
/// where the view shows it.
///
/// @param[in,out] building the list
/// @param dialog the dialog
/// @param rect the button
/// @param caption its caption, as shown
/// @param style its look
/// @param control its control
/// @param name its automation name
/// @param enabled it takes a press
/// @param card where its control lies for the arrows (kit::focus_toward): the
///     card's height, across the button's columns or, for the card's own row
///     control, the section's
void card_button(
    Building& building,
    const Dialog& dialog,
    const SourceRect& rect,
    const std::string& caption,
    kit::ButtonStyle style,
    int32_t control,
    std::string name,
    bool enabled,
    const SourceRect& card
) {
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    const bool hovered = enabled && dialog.hovered == control;
    kit::ButtonLook look{
        caption, style, hovered, enabled && hovered && dialog.pressed == control, enabled
    };
    look.font = DialogFont::regular;
    button(building.list, rect, std::move(look), control, sized.view_clip);
    if (enabled && dialog.focused == control)
        focus_ring(building.list, rect, true, sized.view_clip);
    kit::Control added = control_of(control, card, std::move(name), kit::ControlKind::button);
    // A press reaches the button where the view shows it. Scrolled out of
    // the view, the reach is a point beside the card, which no press inside
    // the control's place meets.
    const int32_t shown_top = std::max(rect.y, sized.view.y);
    const int32_t shown_bottom = std::min(rect.y + rect.height, sized.view.y + sized.view.height);
    added.clip = shown_bottom > shown_top
                     ? SourceRect{rect.x, shown_top, rect.width, shown_bottom - shown_top}
                     : SourceRect{rect.x, card.y - 1, 1, 1};
    added.group = layout::section_group;
    added.enabled = enabled;
    added.text = caption;
    building.rows.push_back(std::move(added));
}

/// Adds Controls' key layout card in its row's place: the keyboard tile,
/// the layout's name and the count of the player's own changes, where it
/// applies, Change layout… where the Library opens over the dialog, and
/// Edit keys…, the row's own control.
///
/// @param[in,out] building the list
/// @param dialog the dialog
/// @param row the card's row, placed and scrolled
/// @param[out] tab the card's controls, in Tab's order
void add_key_card(
    Building& building,
    const Dialog& dialog,
    const kit::PlacedRow& row,
    std::vector<kit::ControlId>& tab
) {
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    const auto regular = width_in(building.fonts, DialogFont::regular);
    const auto small = width_in(building.fonts, DialogFont::small);
    const int32_t margin = kit::metrics_of(sized.size_class).space * 2;
    const cards::KeyCard card = cards::key_card(dialog, sized, row.top + margin, regular, small);
    card_face(building.list, card.card, sized.view_clip);
    picture(building.list, card.tile, icons::key_layout_tile(), sized.view_clip);
    card_lines(building.list, card.lines, sized.view_clip);
    if (card.badge.width > 0) {
        kit::Item& badge = item(building.list, kit::Role::pill, card.badge, sized.view_clip);
        badge.text = card.badge_text;
        badge.look = kit::PillLook{card.badge_text, kit::PillState::update};
    }
    if (card.change.width > 0) {
        card_button(
            building,
            dialog,
            card.change,
            card.change_caption,
            kit::ButtonStyle::plain,
            change_layout_control,
            named("key-layout.change"),
            true,
            {card.change.x, card.card.y, card.change.width, card.card.height}
        );
        tab.push_back(change_layout_control);
    }
    card_button(
        building,
        dialog,
        card.edit,
        card.edit_caption,
        kit::ButtonStyle::accent,
        row.control,
        named("key-layout.edit"),
        true,
        card.card
    );
    tab.push_back(row.control);
}

/// Adds Accessibility's practice pad: the eyebrow and what to try, the
/// keycaps held or latched in their well, what the last key did, Hold to
/// confirm's bar while that helper is on, and Turn every helper off.
///
/// @param[in,out] building the list
/// @param dialog the dialog
/// @param place the pad's place, scrolled with the rows (layout::Rows::pad)
void add_practice_pad(Building& building, const Dialog& dialog, const SourceRect& place) {
    const layout::Sizes& sized = dialog_sizes(building, dialog);
    const cards::Pad pad = cards::practice_pad(
        dialog,
        sized,
        place.x,
        place.y,
        place.width,
        width_in(building.fonts, DialogFont::regular),
        width_in(building.fonts, DialogFont::small)
    );
    card_face(building.list, pad.card, sized.view_clip);
    kit::ButtonLook well;
    well.enabled = false;
    button(building.list, pad.well, std::move(well), no_control, sized.view_clip);
    for (const cards::Pad::Cap& cap : pad.caps) {
        kit::KeycapLook look;
        look.label = cap.label;
        look.held = cap.down;
        look.modifier = cap.label.size() > 1;
        kit::Item& added = item(building.list, kit::Role::keycap, cap.rect, sized.view_clip);
        added.text = cap.label;
        added.look = std::move(look);
    }
    card_lines(building.list, pad.lines, sized.view_clip);
    if (pad.bar.width > 0) {
        kit::Item& bar = item(building.list, kit::Role::progress, pad.bar, sized.view_clip);
        bar.look = kit::ProgressLook{pad.bar_done, pad.bar_whole, false};
    }
    card_button(
        building,
        dialog,
        pad.off,
        pad.off_caption,
        kit::ButtonStyle::plain,
        helpers_off_control,
        named("practice.helpers-off"),
        pad.off_enabled,
        pad.off
    );
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
    // Key layout's row is its card: the kit draws every other row.
    kit::PlacedRows drawn_rows = placed;
    std::optional<kit::PlacedRow> key_row;
    for (auto at = drawn_rows.rows.begin(); at != drawn_rows.rows.end(); ++at) {
        if (at->view.id != "key-layout")
            continue;
        key_row = *at;
        drawn_rows.rows.erase(at);
        break;
    }
    kit::DisplayList rows;
    kit::add_rows(rows, drawn_rows, state);
    for (kit::Item& added : rows.items)
        list.items.push_back(std::move(added));
    std::vector<kit::ControlId> card_tab;
    if (key_row.has_value())
        add_key_card(building, dialog, *key_row, card_tab);
    if (open.rows.pad.width > 0)
        add_practice_pad(building, dialog, open.rows.pad);
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
    // Tab stops on the key layout card's buttons, then on the rows that take
    // a change, top to bottom, then on the practice pad's button.
    building.section_tab = card_tab;
    building.section_tab.insert(
        building.section_tab.end(), rows.tab_order.begin(), rows.tab_order.end()
    );
    if (open.rows.pad.width > 0 && cards::any_helper_on(dialog.chosen))
        building.section_tab.push_back(helpers_off_control);
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

/// Adds the footer: its band under the footer's line, Restore defaults at
/// its left, then how many settings the dialog shows changed, then Cancel
/// and OK, in Kit 2's sizes.
///
/// @param[in,out] building the list
/// @param dialog the dialog
void add_footer(Building& building, const Dialog& dialog) {
    struct FooterButton {
        int32_t control{};
        std::string_view caption{};
        std::string_view name{};
    };

    const layout::Sizes& sized = dialog_sizes(building, dialog);
    const int32_t inner = sized.dialog_width - 2 * layout::edge;
    kit::Item& rule =
        item(building.list, kit::Role::rule, {layout::edge, sized.footer_rule_row, inner, 1}, {});
    rule.colour = kit::colour::rule;
    fill(
        building.list,
        {layout::edge, sized.footer_top, inner, sized.footer_height},
        kit::colour::band,
        {}
    );
    const std::array<FooterButton, 3> buttons{{
        {restore_control, layout::restore_text, "restore-defaults"},
        {cancel_control, layout::cancel_text, "cancel"},
        {ok_control, layout::ok_text, "ok"},
    }};
    for (const FooterButton& footer : buttons) {
        const SourceRect rect = footer.control == restore_control  ? sized.restore_button
                                : footer.control == cancel_control ? sized.cancel_button
                                                                   : sized.ok_button;
        const bool held = dialog.pressed == footer.control && dialog.hovered == footer.control;
        const bool hovered = dialog.hovered == footer.control;
        const std::string caption(layout::shown_text(footer.caption));
        kit::ButtonLook look{
            caption,
            footer.control == ok_control ? kit::ButtonStyle::accent : kit::ButtonStyle::plain,
            hovered,
            held,
            true,
        };
        // Restore defaults keeps the tracked capitals of a window's own
        // action, in the small font; Cancel and OK read in the regular one.
        look.font = footer.control == restore_control ? DialogFont::small : DialogFont::regular;
        button(building.list, rect, std::move(look), footer.control, {});
        if (dialog.focused == footer.control)
            focus_ring(building.list, rect, true, {});
        kit::Control added =
            control_of(footer.control, rect, named(footer.name), kit::ControlKind::button);
        added.text = caption;
        building.footer.push_back(std::move(added));
    }
    // How many settings changed, beside Cancel and OK; nothing while none did.
    const std::vector<std::string> count = layout::changed_count_lines(
        dialog,
        building.fonts != nullptr ? width_in(building.fonts, DialogFont::small)
                                  : width_in(nullptr, DialogFont::small),
        sized.changed_count.width
    );
    const int32_t line = oa::ui::kit::compact_metrics.small_line;
    const int32_t lines = static_cast<int32_t>(count.size());
    for (int32_t at = 0; at < lines; ++at)
        text(
            building.list,
            DialogFont::small,
            count[static_cast<std::size_t>(at)],
            {sized.changed_count.x,
             sized.changed_count.y + (sized.changed_count.height - lines * line) / 2 + at * line,
             sized.changed_count.width,
             line},
            kit::Align::right,
            kit::colour::text_secondary,
            {}
        );
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
        // Updates is not a setting, so its row carries none. Without the
        // same test Downloads uses, the drop-down test below skips it and
        // a click on a listed item lands on whatever is under the list.
        const bool downloads_choice = row.downloads >= 0 && downloads::is_choice(row);
        const bool updates_choice = row.updates >= 0 && updates::is_choice(row);
        if (row.control != dialog.open_list || row.lock != Lock::none)
            continue;
        if (!downloads_choice && !updates_choice &&
            layout::kind_of(row.setting) != kit::RowKind::choice)
            continue;
        const bool page_choice = downloads_choice || updates_choice;
        const auto& spec =
            page_choice ? layout::row_spec(Setting::language) : layout::row_spec(row.setting);
        const layout::SettingsModel model = layout::reading(dialog.chosen, dialog);
        const int32_t choices = downloads_choice ? downloads::choice_count()
                                : updates_choice ? updates::choice_count()
                                                 : kit::row_count(spec, model);
        const SourceRect menu = layout::choice_list(
            row.control_area, static_cast<std::size_t>(choices), dialog.size_class
        );
        const int32_t shown = layout::shown_choices(static_cast<std::size_t>(choices));
        kit::ChoiceMenuLook look;
        look.first = dialog.list_first;
        look.total = choices;
        look.chosen = downloads_choice ? downloads::choice_index(dialog)
                      : updates_choice ? updates::choice_index(dialog)
                                       : kit::row_index(spec, model);
        look.marked = dialog.list_marked;
        look.focus_shown = dialog.focused != no_control;
        layout::fill_language_menu(dialog, row.setting, look);
        for (int32_t place = 0; place < shown; ++place) {
            const int32_t choice = dialog.list_first + place;
            if (choice >= choices)
                break;
            const std::string caption =
                downloads_choice ? downloads::choice_caption(choice)
                : updates_choice
                    ? updates::choice_caption(choice)
                    : std::string(layout::shown_text(kit::row_caption(spec, model, choice)));
            const std::string word = downloads_choice ? downloads::choice_id(choice)
                                     : updates_choice ? updates::choice_id(choice)
                                                      : layout::choice_word(spec, model, choice);
            look.shown.push_back(page_choice ? std::string(layout::shown_text(caption)) : caption);
            // A divider is a heading: it is not a control and has no name.
            if (!page_choice && static_cast<std::size_t>(choice) < look.items.size() &&
                look.items[static_cast<std::size_t>(choice)].divider)
                continue;
            const std::string stem = downloads_choice ? std::string("content-updates")
                                     : updates_choice
                                         ? std::string("update-check")
                                         : std::string(layout::setting_name(row.setting));
            kit::Control added = control_of(
                layout::menu_item_control(choice),
                layout::choice_item(menu, place),
                named(stem + "." + word),
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
    add_nav(building, dialog);
    const ScrolledRows open = open_rows(
        dialog,
        fonts != nullptr ? &small : nullptr,
        &building.fitted,
        fonts != nullptr ? &regular : nullptr
    );
    add_section(building, dialog, open);
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
