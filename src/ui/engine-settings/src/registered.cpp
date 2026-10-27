// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registered settings pages. The section list flows from the top, the
// engine's sections first and then each owner's pages, and scrolls when
// that is taller than the list. A registered page's rows are the kit's
// row specs, bound to preference keys.

#include "registered.hpp"

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/rows.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings {

namespace {

namespace kit = oa::ui::kit;

/// Tells whether a rectangle lies wholly inside another.
///
/// @param inner the inner rectangle
/// @param outer the outer rectangle
/// @return true when inner has room and none of it falls outside outer
bool wholly_in(const geometry::SourceRect& inner, const geometry::SourceRect& outer) noexcept {
    return inner.width > 0 && inner.height > 0 && inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Returns the open registered page, or null when another section is open
/// or the page has no rows.
///
/// @param dialog the dialog
/// @return the page
const RegisteredPage* open_page(const Dialog& dialog) noexcept {
    if (dialog.page != Page::registered || dialog.registered_page >= dialog.registered_pages.size())
        return nullptr;
    const RegisteredPage& page = dialog.registered_pages[dialog.registered_page];
    if (page.rows == nullptr || page.row_count == 0)
        return nullptr;
    return &page;
}

/// Returns the row of the open registered page a control names, or -1.
///
/// @param dialog the dialog
/// @param control the control
/// @return the row, from 0
int32_t row_index_of(const Dialog& dialog, int32_t control) noexcept {
    const RegisteredPage* page = open_page(dialog);
    if (page == nullptr || control < first_row_control)
        return -1;
    const int32_t index = control - first_row_control;
    if (index < 0 || static_cast<std::size_t>(index) >= page->row_count)
        return -1;
    return index;
}

/// The stored text of a key, or empty when the values have none.
///
/// @param values the values
/// @param key the key
/// @return the text
std::string stored(const oa::platform::preferences::Values& values, std::string_view key) {
    const auto found = values.find(std::string(key));
    return found == values.end() ? std::string{} : found->second;
}

} // namespace

bool registered_nav(const Dialog& dialog) noexcept {
    return dialog.kind == DialogKind::engine && !dialog.registered_pages.empty();
}

int32_t registered_nav_index(const Dialog& dialog, int32_t control) noexcept {
    const int32_t index = first_registered_nav_control - control;
    if (index < 0 || static_cast<std::size_t>(index) >= dialog.registered_pages.size())
        return -1;
    return index;
}

RegisteredContent registered_content(const Dialog& dialog, const geometry::Sizes* sizes) {
    RegisteredContent content;
    const geometry::Sizes& sized = sizes != nullptr ? *sizes : geometry::sizes_of(dialog);
    content.scroll.view = sized.view;
    const RegisteredPage* page = open_page(dialog);
    if (page == nullptr)
        return content;
    std::vector<kit::RowView> views;
    views.reserve(page->row_count);
    for (std::size_t index = 0; index < page->row_count; ++index)
        views.push_back(
            kit::view_of(page->rows[index], dialog.registered_values, &geometry::shown_text)
        );
    const bool tall = oa::data::languages::interface_language().needs !=
                      oa::data::languages::TextNeeds::game_fonts;
    kit::RowPlacement placement;
    placement.left = sized.content_left;
    placement.right = sized.content_right;
    placement.first_top = geometry::first_row_top;
    placement.row_padding = geometry::row_padding;
    placement.tall = tall;
    placement.first_control = first_row_control;
    kit::PlacedRows placed = kit::place_rows(views, placement);
    content.scroll = kit::rows_scroll(
        placed,
        sized.view,
        sized.scroll_well,
        sized.scroll_hit,
        sized.page_step,
        geometry::end_gap,
        tall
    );
    const auto slot = static_cast<std::size_t>(Page::registered);
    content.offset = std::clamp(dialog.scroll[slot], int32_t{0}, content.scroll.limit);
    kit::scroll(placed, content.offset);
    content.placed = std::move(placed);
    return content;
}

std::string registered_heading(const Dialog& dialog) {
    if (dialog.page == Page::registered && dialog.registered_page < dialog.registered_pages.size())
        return dialog.registered_pages[dialog.registered_page].title;
    return {};
}

void append_registered_content(
    const Dialog& dialog, std::vector<LayoutPart>& parts, const geometry::Sizes* sizes
) {
    const RegisteredContent content = registered_content(dialog, sizes);
    const geometry::Sizes& sized = sizes != nullptr ? *sizes : geometry::sizes_of(dialog);
    const auto keep = [&](LayoutPart part) {
        if (wholly_in(part.rect, sized.view))
            parts.push_back(std::move(part));
    };
    for (const kit::PlacedRow& row : content.placed.rows) {
        const kit::RowView& view = row.view;
        keep(LayoutPart{row.label, view.label, DialogFont::regular, 0, no_control});
        for (std::size_t line = 0; line < row.hints.size() && line < view.hints.size(); ++line) {
            if (!view.hints[line].empty())
                keep(
                    LayoutPart{row.hints[line], view.hints[line], DialogFont::small, 0, no_control}
                );
        }
        const geometry::SourceRect area = row.control_area;
        switch (view.kind) {
        case kit::RowKind::toggle:
            if (area.width > 0 && view.captions.size() >= 2) {
                const int32_t half = (area.width - 2) / 2;
                for (std::size_t side = 0; side < 2; ++side)
                    keep(
                        LayoutPart{
                            {area.x + 1 + static_cast<int32_t>(side) * half,
                             area.y + 1,
                             half,
                             area.height - 2},
                            view.captions[side],
                            DialogFont::small,
                            0,
                            row.control,
                        }
                    );
            }
            break;
        case kit::RowKind::choice:
            keep(LayoutPart{area, view.value, DialogFont::regular, 0, row.control});
            break;
        case kit::RowKind::slider:
            keep(LayoutPart{area, {}, DialogFont::regular, 0, row.control});
            keep(LayoutPart{row.value, view.value, DialogFont::regular, 0, no_control});
            break;
        case kit::RowKind::levels:
            for (std::size_t level = 0; level < view.captions.size(); ++level)
                keep(
                    LayoutPart{
                        {area.x + 1 + static_cast<int32_t>(level) * view.control_width,
                         area.y + 1,
                         view.control_width,
                         area.height - 2},
                        view.captions[level],
                        DialogFont::small,
                        0,
                        row.control,
                    }
                );
            break;
        case kit::RowKind::text_field:
            keep(LayoutPart{area, view.text, DialogFont::regular, 0, row.control});
            break;
        case kit::RowKind::value_and_button:
        case kit::RowKind::buttons:
        case kit::RowKind::link:
        case kit::RowKind::text:
            break;
        }
    }
}

std::optional<DialogAction> activate_registered(Dialog& dialog, int32_t control) {
    if (const int32_t index = registered_nav_index(dialog, control); index >= 0) {
        dialog.registered_page = static_cast<std::size_t>(index);
        dialog.open_list = no_control;
        dialog.list_pressed = -1;
        dialog.page = Page::registered;
        dialog.wheel_rows = 0.0F;
        if (dialog.focused >= first_row_control ||
            registered_nav_index(dialog, dialog.focused) >= 0)
            dialog.focused = registered_nav_control(dialog.registered_page);
        return DialogAction::redraw;
    }
    const int32_t index = row_index_of(dialog, control);
    if (index < 0)
        return std::nullopt;
    const RegisteredPage& page = dialog.registered_pages[dialog.registered_page];
    const kit::RowResult result = kit::activate(page.rows[index], dialog.registered_values, 0);
    if (result.event == kit::RowEvent::changed)
        return DialogAction::changed;
    return DialogAction::redraw;
}

std::optional<DialogAction>
press_registered_row(Dialog& dialog, int32_t control, kit::Point point) {
    const int32_t index = row_index_of(dialog, control);
    if (index < 0)
        return std::nullopt;
    const RegisteredContent content = registered_content(dialog);
    if (static_cast<std::size_t>(index) >= content.placed.rows.size())
        return std::nullopt;
    const RegisteredPage& page = dialog.registered_pages[dialog.registered_page];
    const kit::RowResult result =
        kit::press(page.rows[index], dialog.registered_values, content.placed.rows[index], point);
    if (result.event == kit::RowEvent::changed)
        return DialogAction::changed;
    return DialogAction::redraw;
}

std::optional<DialogAction> step_registered_row(Dialog& dialog, int32_t control, bool up) {
    const int32_t index = row_index_of(dialog, control);
    if (index < 0)
        return std::nullopt;
    const RegisteredPage& page = dialog.registered_pages[dialog.registered_page];
    const kit::RowResult result = kit::step(page.rows[index], dialog.registered_values, up);
    if (result.event == kit::RowEvent::changed)
        return DialogAction::changed;
    return DialogAction::none;
}

std::optional<DialogAction> wheel_registered_content(Dialog& dialog, float notches) {
    if (open_page(dialog) == nullptr)
        return std::nullopt;
    const RegisteredContent content = registered_content(dialog);
    if (content.scroll.limit <= 0)
        return std::nullopt;
    kit::WheelCarry carry{dialog.wheel_rows};
    const int32_t next = kit::wheel_offset(
        carry, notches, geometry::wheel_step, content.offset, content.scroll.limit
    );
    dialog.wheel_rows = carry.rows;
    const auto slot = static_cast<std::size_t>(Page::registered);
    if (next == dialog.scroll[slot])
        return DialogAction::none;
    dialog.scroll[slot] = next;
    return DialogAction::redraw;
}

void restore_registered_page(Dialog& dialog) {
    const RegisteredPage* page = open_page(dialog);
    if (page == nullptr)
        return;
    for (std::size_t index = 0; index < page->row_count; ++index) {
        const kit::PreferenceRow& spec = page->rows[index];
        if (spec.key.empty())
            continue;
        dialog.registered_values.values[std::string(spec.key)] = std::string(spec.default_text);
    }
}

void set_registered_pages(
    Dialog& dialog,
    std::span<const RegisteredPage> pages,
    const oa::platform::preferences::Values& values
) {
    dialog.registered_pages.clear();
    dialog.registered_values.values.clear();
    dialog.registered_opened.values.clear();
    dialog.registered_page = 0;
    dialog.nav_scroll = 0;
    dialog.nav_wheel_rows = 0.0F;
    if (dialog.kind != DialogKind::engine)
        return;
    dialog.registered_pages.assign(pages.begin(), pages.end());
    dialog.registered_values.values = values;
    dialog.registered_opened.values = values;
}

std::vector<RegisteredWrite> registered_writes(const Dialog& dialog) {
    std::vector<RegisteredWrite> writes;
    for (const RegisteredPage& page : dialog.registered_pages) {
        if (page.rows == nullptr)
            continue;
        for (std::size_t index = 0; index < page.row_count; ++index) {
            const kit::PreferenceRow& spec = page.rows[index];
            if (spec.key.empty())
                continue;
            const std::string key(spec.key);
            const std::string now = stored(dialog.registered_values.values, key);
            const std::string opened = stored(dialog.registered_opened.values, key);
            if (now == opened)
                continue;
            writes.push_back(RegisteredWrite{key, now, page.applied, page.state});
        }
    }
    return writes;
}

} // namespace oa::ui::engine_settings
