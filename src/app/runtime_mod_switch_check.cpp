// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-mod-switch: the Library switches the mod with a soft restart. Each
// run of the check is one turn: it checks that the run plays the mod the
// last turn chose, the Library showing it played, then chooses the next of
// no mod and two test profiles there, with PLAY NOW for a profile, or SWITCH
// OFF and the Switch Mod question's SWITCH for no mod, which ends the run;
// main() builds the next run on the same window and measures the working set
// as each starts.

#include "engine_settings_state.hpp"
#include "library_host.hpp"
#include "oa_layer.hpp"
#include "package_paths.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/mod_questions.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app {

namespace settings = oa::ui::engine_settings;
namespace library = oa::ui::library;

namespace {

/// The switches the check makes before its last run: four rounds of the
/// mods it takes turns with (main.cpp's kModSwitches, which judges them).
constexpr uint32_t kSwitches = 13;

/// One test profile of the check: its folder's name in the player's Mods
/// folder, and its oamod.yaml.
struct TestMod {
    std::string_view folder;      ///< the folder's name
    std::string_view id;          ///< the profile's id
    std::string_view description; ///< the description its oamod.yaml gives
    std::string_view profile;     ///< the oamod.yaml's text
};

/// The two test profiles, which change nothing of the game's rules.
constexpr std::array<TestMod, 2> kTestMods{{
    {"switch-check-alpha",
     "switch-check-alpha",
     "The first profile the mod switch check plays.",
     "oamod: 1\n"
     "id: switch-check-alpha\n"
     "name: Switch Check Alpha\n"
     "version: \"1.0\"\n"
     "description: The first profile the mod switch check plays.\n"
     "requires: {base: ta-3.1c, catalogue: 1}\n"
     "author: {name: unknown}\n"
     "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n"},
    {"switch-check-beta",
     "switch-check-beta",
     "The second profile the mod switch check plays.",
     "oamod: 1\n"
     "id: switch-check-beta\n"
     "name: Switch Check Beta\n"
     "version: \"2.0\"\n"
     "description: The second profile the mod switch check plays.\n"
     "requires: {base: ta-3.1c, catalogue: 1}\n"
     "author: {name: unknown}\n"
     "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n"},
}};

/// Throws when a step of the check fails.
///
/// @param ok the step passed
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("mod switch check: " + std::string(what));
}

} // namespace

void Runtime::check_mod_switch() {
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    const fs::path mods = user_folder_ / std::string(user_mods_folder_name);
    // The first run writes the test profiles afresh and lists them.
    if (options_.restarts == 0) {
        for (const auto& mod : kTestMods) {
            const fs::path folder = mods / std::string(mod.folder);
            fs::create_directories(folder);
            std::ofstream profile(folder / "oamod.yaml", std::ios::binary | std::ios::trunc);
            profile << mod.profile;
            require(static_cast<bool>(profile), "a test profile could not be written");
        }
        list_offered_mods();
    }
    // The mods take turns in this order: no mod, then the test profiles.
    std::vector<std::string> turns{std::string()};
    for (const auto& mod : kTestMods)
        turns.push_back(kept_path(mods / std::string(mod.folder)));
    auto& state = engine_settings_state();
    std::size_t playing_turn = turns.size();
    for (std::size_t turn = 0; turn < turns.size(); ++turn)
        if (turns[turn] == state.playing_mod_folder)
            playing_turn = turn;
    require(playing_turn < turns.size(), "the run plays a mod the check did not choose");
    // The run plays the profile of the mod chosen, read afresh at its start.
    const auto* profile = mod_profile();
    if (playing_turn == 0)
        require(
            profile == nullptr ||
                (profile->id != kTestMods[0].id && profile->id != kTestMods[1].id),
            "No Mod plays a test profile"
        );
    else
        require(
            profile != nullptr && profile->id == kTestMods[playing_turn - 1].id,
            "the mod chosen does not play its own profile"
        );
    std::cout << "mod switch check: run " << options_.restarts << " plays "
              << (playing_turn == 0 ? std::string("No Mod") : std::string(profile->id)) << '\n';

    // The Library, built from the run's own feed, lists both test profiles:
    // the one played as PLAYING with SWITCH OFF, the other with PLAY NOW;
    // with no mod played, neither plays.
    LibraryHost& host = library_host();
    host.rebuild(*this);
    const auto entry_of = [&host](const std::string& folder) -> const library::Entry* {
        for (const library::Entry& each : host.model.entries)
            if (each.id.kind == library::Kind::mod && each.installed != nullptr &&
                same_folder(kept_path(each.installed->folder), folder))
                return &each;
        return nullptr;
    };
    const auto offers = [&host](const library::Entry& each, library::Action wanted) {
        for (const library::ActionButton& button : library::entry_actions(host.model, each))
            if (button.action == wanted)
                return button.enabled;
        return false;
    };
    for (std::size_t turn = 1; turn < turns.size(); ++turn) {
        const library::Entry* listed = entry_of(turns[turn]);
        require(listed != nullptr, "the Library does not list a test profile");
        require(
            library::entry_summary(*listed) == kTestMods[turn - 1].description,
            "the Library's details do not show a test profile's description"
        );
        const bool played = turn == playing_turn;
        require(
            listed->installed->playing == played &&
                (listed->state == library::State::playing) == played,
            played ? "the Library does not show the mod played as playing"
                   : "the Library shows a mod playing that the run does not play"
        );
        require(
            offers(*listed, played ? library::Action::switch_off : library::Action::play_now),
            played ? "the mod played offers no SWITCH OFF" : "a mod not played offers no PLAY NOW"
        );
    }
    const library::Entry* played_entry =
        playing_turn != 0 ? entry_of(turns[playing_turn]) : nullptr;
    for (const library::Entry& each : host.model.entries)
        require(
            each.id.kind != library::Kind::mod || each.installed == nullptr ||
                each.installed->playing == (&each == played_entry),
            "the Library shows another mod playing than the one played"
        );

    if (options_.restarts >= kSwitches) {
        // The last run: the next start of the check plays No Mod again.
        preference_values_.erase(std::string(settings::key::mod_directory));
        preferences_dirty_ = true;
        std::cout << "mod switch check: " << kSwitches << " switches made\n";
        return;
    }

    // A test profile is chosen with PLAY NOW, which stores it and ends the
    // run; no mod with SWITCH OFF on the mod played, which asks nothing,
    // stores no mod and ends the run.
    const std::string& next = turns[(playing_turn + 1) % turns.size()];
    const auto question_shows = [this] {
        return dynamic_cast<const QuestionScreen*>(oa_layer().top()) != nullptr;
    };
    if (next.empty()) {
        const library::Entry* played = entry_of(turns[playing_turn]);
        require(played != nullptr, "the Library does not list the mod played");
        const library::EntryId id = played->id;
        // SWITCH OFF plays no mod at once, asking nothing.
        library_entry_action(library::Action::switch_off, id);
        require(!question_shows(), "SWITCH OFF asked a question");
    } else {
        const library::Entry* chosen = entry_of(next);
        require(chosen != nullptr, "the Library does not list the next mod");
        const library::EntryId id = chosen->id;
        library_entry_action(library::Action::play_now, id);
    }
    require(engine_settings().mod_folder == next, "the switch did not choose the mod");
    require(soft_restart_requested(), "the switch did not end the run");
    const auto stored = preference_values_.find(std::string(settings::key::mod_directory));
    require(
        next.empty() ? stored == preference_values_.end() || stored->second.empty()
                     : stored != preference_values_.end() && stored->second == next,
        "the switch did not store the mod"
    );
}

} // namespace oa::app
