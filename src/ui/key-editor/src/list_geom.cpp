// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "list_geom.hpp"

namespace oa::ui::key_editor {

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
        heading.text = category.title;
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
