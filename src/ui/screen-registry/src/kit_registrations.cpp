// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registration of settings pages and kit screens. The words of the engine's
// own sections are listed here so this module does not depend on the
// settings dialog.

#include "oa/ui/screen_registry.hpp"
#include "oa/ui/screen_registry/kit.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace oa::app {
namespace {

/// The most bytes a page id or a screen name holds.
constexpr std::size_t kIdMost = 48;

/// The engine's sections' words, and the word Page::registered uses. A
/// registered page's id is none of these, so its nav name stays its own.
constexpr std::string_view kEngineSectionWords[] = {
    "mods",
    "controls",
    "engine-limits",
    // The section's word before it was engine-limits. A page may not take it.
    "common-tweaks",
    "language",
    "graphics",
    "touch",
    "controller",
    "developer",
    "game-files",
    "updates",
    "accessibility",
    "mod-keys",
    "mod-patrol",
    "mod-guard",
    "mod-tools",
    "mod-chat",
    "registered",
};

/// Returns why an id is refused, or null when it is a page id's form.
///
/// @param id the id; null is missing
/// @return a reason, or null
const char* id_problem(const char* id) noexcept {
    if (id == nullptr || id[0] == '\0')
        return "an id is missing";
    const std::string_view text(id);
    if (text.size() > kIdMost)
        return "an id is longer than 48 bytes";
    if (text.front() == '.' || text.back() == '.')
        return "an id is malformed";
    std::size_t start = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index != text.size() && text[index] != '.')
            continue;
        if (index == start || !oa::ui::kit::row_word(text.substr(start, index - start)))
            return "an id is malformed";
        start = index + 1;
    }
    return nullptr;
}

/// Tells whether an id is an engine section's word.
///
/// @param id the id
/// @return true when it is one of kEngineSectionWords
bool engine_section_word(std::string_view id) noexcept {
    for (const std::string_view word : kEngineSectionWords)
        if (word == id)
            return true;
    return false;
}

/// Returns why a row is refused, or null when it is bound.
///
/// @param row the row
/// @return a reason, or null
const char* row_problem(const oa::ui::kit::PreferenceRow& row) noexcept {
    if (!oa::ui::kit::row_word(row.id))
        return "a row id is malformed";
    if (row.key.empty())
        return "a row has no binding";
    if (const char* key = oa::ui::kit::preference_key_problem(row.key))
        return key;
    switch (row.kind) {
    case oa::ui::kit::RowKind::toggle:
    case oa::ui::kit::RowKind::text_field:
        return nullptr;
    case oa::ui::kit::RowKind::choice:
        if (row.captions.empty() || row.choice_ids.size() != row.captions.size())
            return "a row has no binding";
        for (const std::string_view choice : row.choice_ids)
            if (!oa::ui::kit::row_word(choice))
                return "a choice id is malformed";
        return nullptr;
    case oa::ui::kit::RowKind::slider:
        if (!(row.slider_step > 0.0) || row.slider_most < row.slider_least)
            return "a row has no binding";
        return nullptr;
    default:
        return "a row has no binding";
    }
}

/// Returns why a page's rows are refused, or null when each is bound.
///
/// @param page the page
/// @return a reason, or null
const char* rows_problem(const SettingsPageDesc& page) noexcept {
    if (page.rows == nullptr || page.row_count == 0)
        return "a page has no rows";
    for (std::size_t index = 0; index < page.row_count; ++index) {
        if (const char* problem = row_problem(page.rows[index]))
            return problem;
        for (std::size_t earlier = 0; earlier < index; ++earlier)
            if (page.rows[earlier].id == page.rows[index].id)
                return "a row id is taken";
    }
    return nullptr;
}

} // namespace

int settings_page_register(SettingsRegistry* registry, const SettingsPageDesc& page) {
    if (registry == nullptr)
        return 0;
    // The first refusal is kept, as ScreenRegistry keeps its first, so that
    // a page taken after it does not hide it.
    const auto refuse = [registry](const char* reason) {
        if (registry->rejected == nullptr)
            registry->rejected = reason;
        return 0;
    };
    if (registry->page_count >= kMaxSettingsPages)
        return refuse("the registry is full");
    if (const char* problem = id_problem(page.id))
        return refuse(problem);
    if (engine_section_word(page.id))
        return refuse("a page id is an engine section");
    if (page.title == nullptr || page.title[0] == '\0')
        return refuse("a title is missing");
    if (page.owner == nullptr || page.owner[0] == '\0')
        return refuse("an owner is missing");
    if (const char* problem = rows_problem(page))
        return refuse(problem);
    for (uint32_t index = 0; index < registry->page_count; ++index)
        if (registry->ids[index] == page.id)
            return refuse("a page id is taken");

    const uint32_t index = registry->page_count;
    registry->ids[index] = page.id;
    registry->titles[index] = page.title;
    registry->owners[index] = page.owner;
    SettingsPageDesc stored = page;
    stored.id = registry->ids[index].c_str();
    stored.title = registry->titles[index].c_str();
    stored.owner = registry->owners[index].c_str();
    registry->pages[index] = stored;
    ++registry->page_count;
    return 1;
}

bool kit_screen_register(ScreenRegistry* registry, const KitScreenDesc& screen) {
    if (registry == nullptr)
        return false;
    // The first refusal is kept, as the registry's other tables keep theirs.
    const auto refuse = [registry, &screen] {
        if (registry->rejected == nullptr)
            registry->rejected =
                screen.name != nullptr && screen.name[0] != '\0' ? screen.name : "(unnamed)";
        return false;
    };
    if (registry->kit_screen_count >= kMaxKitScreens || id_problem(screen.name) != nullptr ||
        screen.lay_out == nullptr || screen.act == nullptr)
        return refuse();
    for (uint32_t index = 0; index < registry->kit_screen_count; ++index) {
        const char* taken = registry->kit_screens[index].name;
        if (taken != nullptr && std::string_view(taken) == screen.name)
            return refuse();
    }
    registry->kit_screens[registry->kit_screen_count++] = screen;
    return true;
}

} // namespace oa::app
