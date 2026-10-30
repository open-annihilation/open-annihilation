// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The warning that the mod played lacks files: its folder lacks the unit
// definitions its profile reads, or a side's commander is not among them,
// which keeps its games from starting; or a side's interface art or font is
// missing, with or without a profile, which its games show without. The mod
// still plays its menus; the main menu shows the warning once from each
// start, and a start of a skirmish, a shared game, a campaign mission or a
// saved game that cannot start shows it in place of the game, on the screen
// the start was asked from. It is a notice of the player's own folder
// (UserFolderState::show_notice, runtime_user_folder.cpp) on the OA layer,
// whose button shows the mod's folder.

#include "user_folder_state.hpp"

#include "oa/app/asset_files.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/mutators_memory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/sides.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/defs/unit_records.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_state/app_modes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace defs = oa::data::defs;

/// Frames in a row the main menu shows before the warning: a start that
/// passes the main menu at its first update shows none.
constexpr uint32_t kWarningMenuFrames = 2;
/// Wrap width of the message box that stands for the warning when its fonts
/// cannot be drawn, and that tells of another failed start.
constexpr int32_t kMessageWidth = 0x140;

/// Returns a fixed field's text: up to its first NUL, or all of it.
///
/// @param field the field
/// @param capacity its bytes
/// @return the text
std::string field_text(const char* field, std::size_t capacity) {
    return {field, ::strnlen(field, capacity)};
}

/// Returns the names of the unit definition files in the units folder, as
/// the match loads them.
///
/// @param files the game's files
/// @return the file names, each with its extension
std::vector<std::string> unit_files(const defs::Files& files) {
    std::vector<std::string> names;
    files.list(
        files.context,
        defs::directory_name(defs::DataDirectory::units),
        defs::unit_extension(),
        [](void* user, const char* name) {
            static_cast<std::vector<std::string>*>(user)->emplace_back(name);
        },
        &names
    );
    return names;
}

/// Returns the UnitName a unit definition file gives a unit the match's
/// catalog keeps: its header read as the match reads it, under the same
/// Version and Copyright rules.
///
/// @param files the game's files
/// @param file the file's name in the units folder
/// @return the name; empty when the file cannot be read, gives none, or the
///     catalog drops its unit
std::string kept_unit_name(const defs::Files& files, const std::string& file) {
    char path[defs::path_capacity];
    defs::build_variant_path(
        &files,
        path,
        sizeof path,
        defs::directory_name(defs::DataDirectory::units),
        file.c_str(),
        defs::unit_extension(),
        nullptr
    );
    // The weapons only feed the record's checksum, which is not wanted here.
    defs::WeaponTdfSet weapons{};
    const defs::UnitHeaderSources sources{
        "",
        &weapons,
        defs::data_layout().build_version[0],
        defs::data_layout().build_version[1],
        false,
        false,
        nullptr
    };
    UnitDef record{};
    bool refused = false;
    if (!defs::load_unit_header(&files, path, record, sources, &refused) ||
        defs::unit_def_is_unavailable(&record))
        return {};
    return field_text(record.unit_name, sizeof record.unit_name);
}

/// Tells whether every side's commander has a unit file of its own name
/// whose unit the match's catalog keeps under that name.
///
/// @param files the game's files
/// @param listed the unit files' names
/// @param sides each side and its commander
/// @return true when each commander's own file gives it
bool commanders_kept_by_file_name(
    const defs::Files& files,
    const std::vector<std::string>& listed,
    const std::vector<SideCommander>& sides
) {
    for (const auto& side : sides) {
        if (side.commander.empty())
            continue;
        const auto own =
            std::find_if(listed.begin(), listed.end(), [&side](const std::string& file) {
                const std::string stem = file.substr(0, file.rfind('.'));
                return oa::formats::tdf::compare_nocase(stem.c_str(), side.commander.c_str()) == 0;
            });
        if (own == listed.end() || oa::formats::tdf::compare_nocase(
                                       kept_unit_name(files, *own).c_str(), side.commander.c_str()
                                   ) != 0)
            return false;
    }
    return true;
}

/// Returns the notice's text as one message, for the message box that stands
/// for it.
///
/// @param notice the notice
/// @return its paragraphs, a line each
std::string message_text(const oa::ui::engine_settings::Notice& notice) {
    std::string text;
    for (const auto& paragraph : notice.paragraphs) {
        if (!text.empty())
            text += '\n';
        text += paragraph.text;
    }
    return text;
}

} // namespace

fs::path Runtime::played_mod_folder() const {
    fs::path folder;
    if (!options_.layered_mod_folder.empty())
        folder = options_.layered_mod_folder;
    else if (!options_.mod_file.empty())
        folder = options_.mod_file.parent_path();
    else if (!options_.game_folders.empty())
        folder = options_.game_folders.back();
    else
        folder = options_.game_dir;
    std::error_code error;
    const fs::path absolute = fs::absolute(folder, error);
    return (error ? folder : absolute).lexically_normal();
}

bool Runtime::plays_mod() const noexcept {
    return mod_profile() != nullptr || !options_.layered_mod_folder.empty();
}

ModStartGaps Runtime::mod_start_gaps() {
    if (!plays_mod())
        return {};
    std::vector<SideCommander> sides;
    for (uint32_t side = 0; side < side_table_.count; ++side) {
        const auto& record = *oa::data::defs::side_at(side_table_, side);
        sides.push_back(
            {field_text(record.name, sizeof record.name),
             field_text(record.commander, sizeof record.commander)}
        );
    }
    // The sides' interface art and fonts, which a game loads as it starts.
    std::vector<SideFileGap> side_files;
    for (const auto& missing : missing_side_files())
        side_files.push_back(
            {sides[missing.side].side, field_text(missing.path, sizeof missing.path)}
        );
    ModStartGaps gaps;
    // A folder without a profile plays by 3.1c's own rules, and its units
    // and commanders are not looked for.
    if (mod_profile() != nullptr) {
        const defs::Files files = asset_files(assets_);
        const std::vector<std::string> listed = unit_files(files);
        // The commanders' own files first, which are their units' names in
        // the game and its mods: a mod that plays whole reads no others.
        if (listed.empty() || !commanders_kept_by_file_name(files, listed, sides)) {
            // Otherwise every unit the catalog keeps, by the name its
            // definition gives.
            std::vector<std::string> names;
            for (const auto& file : listed)
                if (auto name = kept_unit_name(files, file); !name.empty())
                    names.push_back(std::move(name));
            gaps = find_mod_start_gaps(names, sides);
        }
    }
    gaps.missing_side_files = std::move(side_files);
    return gaps;
}

void Runtime::show_mod_warning(const ModStartGaps& gaps) {
    auto& state = user_folder_state();
    const auto* profile = mod_profile();
    const fs::path folder = played_mod_folder();
    auto notice = mod_files_missing_notice(
        profile != nullptr ? std::string_view(profile->name) : std::string_view{}, gaps, folder
    );
    state.mod_warning_due = false;
    ++state.mod_warnings_shown;
    // Without the dialog's fonts the warning is the game's message box.
    if (engine_settings_fonts() == nullptr) {
        show_frontend_message(
            message_text(notice), kMessageWidth, entry::message_show_ok, entry::message_fit_width
        );
        return;
    }
    UserFolderState::show_notice(*this, std::move(notice), folder, screen_);
}

bool Runtime::refuse_incomplete_mod_start(bool later) {
    if (mod_profile() == nullptr)
        return false;
    // Missing side files alone warn from the main menu, and games start.
    const ModStartGaps gaps = mod_start_gaps();
    if (!gaps.games_cannot_start())
        return false;
    status_ = "This mod's games cannot start until its files are added to its folder";
    std::cerr << "open-annihilation: " << status_ << ": " << path_to_utf8(played_mod_folder())
              << '\n';
    // Over a match, the loading screen or a screen package's frame, and when
    // the screen shown is about to go, the main menu shows it next.
    if (later || screen_ == Screen::match || screen_ == Screen::loading ||
        frame_owned_by_package()) {
        auto& state = user_folder_state();
        state.mod_warning_due = true;
        state.mod_warning_frames = 0;
        return true;
    }
    show_mod_warning(gaps);
    return true;
}

void Runtime::tell_incomplete_mod() {
    // Most frames have nothing to tell, and cost no more than this.
    if (!plays_mod() || (user_folder_state_ && (!user_folder_state_->mod_warning_due ||
                                                UserFolderState::notice_pending(*this))))
        return;
    auto& state = user_folder_state();
    namespace frontend_state = oa::ui::frontend_state;
    const bool settled = screen_ == Screen::main_menu && !frame_owned_by_package() &&
                         state_.state == frontend_state::state_id::main_menu &&
                         state_.pending_signal != frontend_state::signal_id::multiplayer;
    if (!settled) {
        state.mod_warning_frames = 0;
        return;
    }
    if (++state.mod_warning_frames < kWarningMenuFrames ||
        oa::ui::frontend_dialogs::dialog_count() != 0 || engine_settings_dialog() != nullptr ||
        engine_settings_fonts() == nullptr)
        return;
    // A run nobody watches leaves it waiting for one someone does.
    if (UserFolderState::unwatched(options_.unattended) && !state.check_shows_notice)
        return;
    state.mod_warning_due = false;
    if (const ModStartGaps gaps = mod_start_gaps(); gaps.any())
        show_mod_warning(gaps);
}

bool Runtime::note_failed_skirmish_start(const std::exception& failure) {
    // A start that fails leaves no match and the skirmish setup as it
    // was, with what went wrong over it.
    status_ = std::string("Skirmish start failed: ") + failure.what();
    std::cerr << "open-annihilation: " << status_ << '\n';
    if (match_)
        leave_match();
    else
        teardown_match();
    skirmish::setup(state_, skirmish_settings_, preferences_, skirmish_ui_, *this);
    if (const ModStartGaps gaps = mod_start_gaps(); gaps.games_cannot_start())
        show_mod_warning(gaps);
    else if (!offer_mutator_start_question())
        show_frontend_message(
            status_, kMessageWidth, entry::message_show_ok, entry::message_fit_width
        );
    return false;
}

bool Runtime::start_skirmish_as_chosen(const entry::Event& event) {
    try {
        return skirmish::handle_event(
            state_, skirmish_settings_, preferences_, skirmish_ui_, event, *this
        );
    } catch (const std::exception& failure) {
        return note_failed_skirmish_start(failure);
    }
}

bool Runtime::start_skirmish_from_setup(const entry::Event& event) {
    // A hold this START armed and the match never took must not seat a later one.
    bool armed_brains = false;
    try {
        // START hands the setup's chosen mutators on first, so a set that
        // cannot be read fails here as any start does, and the set stays.
        if (button_result(skirmish::MenuHandle{kFrontendMenuHandle}, skirmish::Button::start) !=
            0) {
            hand_chosen_mutators(mutators_memory::Setup::skirmish);
            cancel_brain_seats_once();
            armed_brains = true;
            hand_setup_brain_seats(oa::data::brain_library::Setup::skirmish);
        }
    } catch (const std::exception& failure) {
        if (armed_brains)
            cancel_brain_seats_once();
        return note_failed_skirmish_start(failure);
    }
    const bool started = start_skirmish_as_chosen(event);
    if (armed_brains && brain_seats_held_for_next_match())
        cancel_brain_seats_once();
    return started;
}

} // namespace oa::app
