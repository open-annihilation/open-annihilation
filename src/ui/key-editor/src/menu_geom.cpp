// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "menu_geom.hpp"

#include "oa/ui/key_editor/text.hpp"

#include <string_view>

namespace oa::ui::key_editor {
namespace {

/// True when a control name is words the kit accepts.
///
/// @param name the name
/// @return true when it is words of a-z, 0-9 and hyphens joined by dots
bool name_ok(std::string_view name) {
    if (name.empty() || name.size() > 100)
        return false;
    bool word = false;
    for (const char letter : name) {
        if (letter == '.') {
            if (!word)
                return false;
            word = false;
            continue;
        }
        const bool ordinary = (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') ||
                              letter == '-';
        if (!ordinary)
            return false;
        word = true;
    }
    return word;
}

/// A control name, or `fallback` when `name` would be refused.
///
/// @param name the name the item would have
/// @param fallback a name that is sound
/// @return the name to give the control
std::string sound_name(std::string name, std::string fallback) {
    if (name_ok(name))
        return name;
    return fallback;
}

/// One filter's caption, with how many actions it keeps in the category.
///
/// @param editor the editor
/// @param state the category
/// @param filter which filter
/// @param word the filter's name
/// @return the caption
std::string filter_caption(
    const Editor& editor, const ScreenState& state, Filter filter, std::string_view word
) {
    const std::string label = show(editor.text(), word);
    const std::string count =
        std::to_string(editor.rows(state.category, filter, "").size());
    return fill(show(editor.text(), words::filter_count), {{"filter", label}, {"count", count}});
}

/// The layout menu: OA default, each installed layout, then Duplicate and Rename.
///
/// @param editor the editor
/// @return the items
std::vector<MenuEntry> layout_entries(const Editor& editor) {
    std::vector<MenuEntry> entries;
    auto add = [&](MenuKind kind, std::string id, std::string caption, std::string name) {
        MenuEntry entry;
        entry.kind = kind;
        entry.id = std::move(id);
        entry.caption = std::move(caption);
        entry.name = std::move(name);
        entries.push_back(std::move(entry));
    };
    add(MenuKind::layout_id,
        "oa-default",
        show(editor.text(), words::oa_default),
        std::string(name_layout) + ".oa-default");
    std::size_t place = 0;
    for (const LayoutSummary& layout : editor.installed()) {
        const std::string fallback = std::string(name_layout) + ".n" + std::to_string(place);
        add(MenuKind::layout_id,
            layout.id,
            layout.name,
            sound_name(std::string(name_layout) + "." + layout.id, fallback));
        ++place;
    }
    MenuEntry rule;
    rule.kind = MenuKind::divider;
    entries.push_back(std::move(rule));
    add(MenuKind::duplicate,
        "duplicate",
        show(editor.text(), words::duplicate),
        std::string(name_layout) + ".duplicate");
    add(MenuKind::rename,
        "rename",
        show(editor.text(), words::rename),
        std::string(name_layout) + ".rename");
    return entries;
}

/// The category menu. Compact adds the filters under a rule.
///
/// @param editor the editor
/// @param state the category, for the filters' counts
/// @param compact true when the filters are items of this menu
/// @return the items
std::vector<MenuEntry> category_entries(const Editor& editor, const ScreenState& state, bool compact) {
    std::vector<MenuEntry> entries;
    for (const CategoryEntry& category : editor.categories()) {
        MenuEntry entry;
        if (category.id.starts_with("heading.")) {
            entry.kind = MenuKind::divider;
            entry.caption = category.title;
        } else {
            entry.kind = MenuKind::category;
            entry.id = category.id;
            const std::string count = std::to_string(category.count);
            const std::string name_shown =
                category.id == "all" ? show(editor.text(), words::all_categories) : category.title;
            entry.caption = fill(
                show(editor.text(), words::category_count), {{"name", name_shown}, {"count", count}}
            );
            entry.name = sound_name(
                std::string(name_category) + "." + category.id,
                std::string(name_category) + ".n" + std::to_string(entries.size())
            );
        }
        entries.push_back(std::move(entry));
    }
    if (!compact)
        return entries;
    MenuEntry rule;
    rule.kind = MenuKind::divider;
    entries.push_back(std::move(rule));
    const Filter filters[] = {
        Filter::all, Filter::changed, Filter::clashes, Filter::unbound, Filter::latchable
    };
    const std::string_view words_of[] = {
        words::filter_all, words::filter_changed, words::filter_clashes, words::filter_unbound,
        words::filter_latchable
    };
    const std::string_view names[] = {
        name_filter_all, name_filter_changed, name_filter_clashes, name_filter_unbound,
        name_filter_latchable
    };
    const char* ids[] = {"all", "changed", "clashes", "unbound", "latchable"};
    for (int index = 0; index < 5; ++index) {
        MenuEntry entry;
        entry.kind = MenuKind::filter;
        entry.id = ids[index];
        entry.caption = filter_caption(editor, state, filters[index], words_of[index]);
        entry.name = std::string(names[index]);
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace

std::vector<MenuEntry>
menu_entries(const Editor& editor, Menu menu, const ScreenState& state, bool compact) {
    if (menu == Menu::layout)
        return layout_entries(editor);
    if (menu == Menu::category)
        return category_entries(editor, state, compact);
    if (menu == Menu::more) {
        // Compact has no LAYOUT drop-down. The layout's choices sit in MORE,
        // and Restore defaults follows them.
        std::vector<MenuEntry> entries = layout_entries(editor);
        MenuEntry rule;
        rule.kind = MenuKind::divider;
        entries.push_back(std::move(rule));
        MenuEntry entry;
        entry.kind = MenuKind::restore;
        entry.id = "restore";
        entry.caption = show(editor.text(), words::restore);
        entry.name = std::string(name_restore);
        entries.push_back(std::move(entry));
        return entries;
    }
    return {};
}

int32_t menu_chosen(
    const Editor& editor, Menu menu, const ScreenState& state, const std::vector<MenuEntry>& entries
) {
    if (menu == Menu::layout) {
        const std::string id = editor.layout_id();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].kind == MenuKind::layout_id && entries[index].id == id)
                return static_cast<int32_t>(index);
        }
        return 0;
    }
    if (menu == Menu::category) {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].kind == MenuKind::category && entries[index].id == state.category)
                return static_cast<int32_t>(index);
        }
    }
    if (menu == Menu::more) {
        const std::string id = editor.layout_id();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].kind == MenuKind::layout_id && entries[index].id == id)
                return static_cast<int32_t>(index);
        }
        return 0;
    }
    return -1;
}

} // namespace oa::ui::key_editor
