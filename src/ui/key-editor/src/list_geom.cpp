// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "list_geom.hpp"

#include "oa/ui/key_editor/text.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_editor {
namespace {

/// A row's place with "held" taken out, so a group heading reads
/// "Orders · while playing" rather than the held form of one action.
///
/// @param where the row's place, already looked up
/// @param held the looked-up word "held"
/// @return the place
std::string plain_where(std::string where, std::string_view held) {
    const std::string token = std::string(" · ") + std::string(held) + " ";
    const std::size_t at = where.find(token);
    if (at != std::string::npos)
        where.replace(at, token.size(), " · ");
    return where;
}

/// The heading over one category: the place most of its rows share.
///
/// @param editor the words
/// @param rows the category's rows
/// @param title the category's title, when no row names a place
/// @return the heading
std::string
group_heading(const Editor& editor, const std::vector<const Row*>& rows, std::string_view title) {
    const std::string held = show(editor.text(), words::aside_held);
    std::vector<std::string> lines;
    lines.reserve(rows.size());
    for (const Row* row : rows) {
        std::string line = plain_where(row->where, held);
        if (!line.empty())
            lines.push_back(std::move(line));
    }
    if (lines.empty())
        return std::string(title);
    std::string best = lines.front();
    std::size_t best_count = 0;
    for (const std::string& candidate : lines) {
        std::size_t count = 0;
        for (const std::string& line : lines) {
            if (line == candidate)
                ++count;
        }
        if (count > best_count) {
            best = candidate;
            best_count = count;
        }
    }
    return best;
}

} // namespace

int32_t list_row_height(kit::SizeClass size_class) noexcept {
    if (size_class == kit::SizeClass::large)
        return large_row_height;
    if (size_class == kit::SizeClass::regular)
        return regular_row_height;
    return compact_row_height;
}

std::vector<Slot> list_slots(const Editor& editor, const ScreenState& state) {
    const std::vector<Row> rows = editor.rows(state.category, state.filter, state.search.text);
    const bool every = state.category.empty() || state.category == "all";
    std::vector<Slot> slots;
    if (!every) {
        if (state.category == "groups") {
            Slot switch_slot;
            switch_slot.kind = Slot::Kind::number_keys;
            switch_slot.id = "groups";
            slots.push_back(std::move(switch_slot));
        }
        for (const Row& row : rows) {
            Slot slot;
            slot.kind = Slot::Kind::row;
            slot.id = row.id;
            slots.push_back(std::move(slot));
        }
        return slots;
    }
    const std::vector<CategoryEntry> categories = editor.categories();
    for (const CategoryEntry& category : categories) {
        if (category.id == "all" || category.id.starts_with("heading."))
            continue;
        std::vector<const Row*> in_category;
        for (const Row& row : rows) {
            if (row.category == category.id)
                in_category.push_back(&row);
        }
        const bool groups = category.id == "groups";
        if (in_category.empty() && !groups)
            continue;
        Slot heading;
        heading.kind = Slot::Kind::heading;
        heading.id = category.id;
        heading.text = group_heading(editor, in_category, category.title);
        slots.push_back(std::move(heading));
        if (groups) {
            Slot switch_slot;
            switch_slot.kind = Slot::Kind::number_keys;
            switch_slot.id = category.id;
            slots.push_back(std::move(switch_slot));
        }
        for (const Row* row : in_category) {
            Slot slot;
            slot.kind = Slot::Kind::row;
            slot.id = row->id;
            slots.push_back(std::move(slot));
        }
    }
    return slots;
}

} // namespace oa::ui::key_editor
