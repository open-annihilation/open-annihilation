// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Engine Limits' weapon-type and side rows, at the main menu and in a
// two-slot skirmish. With no mod profile they show 256 and 5. With a
// profile they show its limits at the menu, and how many of each the match
// loaded once a game is playing.

#include "oa/app/runtime.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;

/// The text a limit row shows: plain digits, or "N of LIMIT" in a mod's match.
std::string count_text(uint32_t limit, bool mod, const std::optional<uint32_t>& loaded) {
    if (!mod || !loaded)
        return std::to_string(limit);
    return std::to_string(*loaded) + " of " + std::to_string(limit);
}

/// Tells whether the dialog's layout shows a text.
bool shows_text(const std::vector<settings::LayoutPart>& parts, std::string_view text) {
    for (const settings::LayoutPart& part : parts)
        if (part.text == text)
            return true;
    return false;
}

} // namespace

void Runtime::check_mod_limits() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("mod limits check: " + what);
    };
    require(sdl_.renderer != nullptr && sdl_.window != nullptr, "needs the SDL renderer");
    load(Screen::main_menu);

    // Opens Engine Limits scrolled to its end and reads the two rows.
    const auto read_rows = [this, &require] {
        settings::Dialog& dialog = open_engine_settings_dialog(settings::DialogKind::engine);
        dialog.page = settings::Page::common_tweaks;
        const std::size_t page = static_cast<std::size_t>(settings::Page::common_tweaks);
        dialog.scroll[page] = settings::scroll_limit(dialog);
        const settings::ModLimitsShown limits = dialog.mod_limits;
        const std::string weapons =
            count_text(limits.weapon_types, limits.mod, limits.loaded_weapon_types);
        const std::string sides = count_text(limits.sides, limits.mod, limits.loaded_sides);
        const std::vector<settings::LayoutPart> parts = settings::dialog_layout(dialog);
        require(shows_text(parts, "Weapon types"), "Weapon types is not shown");
        require(shows_text(parts, "Sides"), "Sides is not shown");
        require(shows_text(parts, weapons), "weapon types show " + weapons + " nowhere");
        require(shows_text(parts, sides), "sides show " + sides + " nowhere");
        require(
            take_engine_settings_action(settings::DialogAction::cancelled),
            "Cancel did not close Settings"
        );
        return std::pair<std::string, std::string>{weapons, sides};
    };

    const auto menu = read_rows();
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    state_.player_count = 2;
    require(skirmish_settings_.slots.size() >= 2, "the skirmish has fewer than two slots");
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    require(screen_ == Screen::match && match_ != nullptr, "Start did not enter a match");

    const auto playing = read_rows();
    std::cout << "mod limits check: menu " << menu.first << ", " << menu.second << "; match "
              << playing.first << ", " << playing.second << '\n';
}

} // namespace oa::app
