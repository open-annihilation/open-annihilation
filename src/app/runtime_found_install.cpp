// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main menu's notice of where the game folder was found, shown once: in
// the look of the saved games' notice, a notice screen of the OA layer over
// the darkened main menu, after that notice and never over another, with a
// button that shows the folder.
#include "user_folder_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_state/app_modes.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app {

namespace {

/// Frames in a row the main menu shows before the notice, as for the saved games' notice: a
/// start that passes the main menu at its first update shows none.
constexpr uint32_t found_notice_menu_frames = 2;

/// Returns the notice for the words found_install_notice gave: the sentence, then the folder
/// as a path, then how to choose another.
///
/// @param words the sentence and, on the next line, the folder
/// @return the notice
oa::ui::engine_settings::Notice found_notice(std::string_view words) {
    oa::ui::engine_settings::Notice notice;
    notice.title = "GAME FOLDER FOUND";
    notice.open_caption = "Open folder";
    const std::size_t line_end = words.find('\n');
    notice.paragraphs.push_back({std::string(words.substr(0, line_end)), false});
    if (line_end != std::string_view::npos && line_end + 1 < words.size())
        notice.paragraphs.push_back({std::string(words.substr(line_end + 1)), true});
    notice.paragraphs.push_back(
        {"Open Annihilation remembers it for later starts; start it with --choose-game-dir to "
         "choose another folder.",
         false}
    );
    return notice;
}

} // namespace

void Runtime::tell_found_install() {
    // Most frames, and every start whose folder was not found, have nothing to tell.
    if (options_.found_install_notice.empty())
        return;
    // The saved games' notice goes first.
    if (saves_notice_due_in(preference_values_))
        return;
    auto& state = user_folder_state();
    if (UserFolderState::notice_pending(*this)) {
        state.main_menu_frames = 0;
        return;
    }
    namespace frontend_state = oa::ui::frontend_state;
    const bool settled = screen_ == Screen::main_menu && !frame_owned_by_package() &&
                         state_.state == frontend_state::state_id::main_menu &&
                         state_.pending_signal != frontend_state::signal_id::multiplayer;
    if (!settled) {
        state.main_menu_frames = 0;
        return;
    }
    if (++state.main_menu_frames < found_notice_menu_frames ||
        oa::ui::frontend_dialogs::dialog_count() != 0 || engine_settings_dialog() != nullptr ||
        engine_settings_fonts() == nullptr)
        return;
    // A run nobody watches never shows it.
    if (UserFolderState::unwatched(options_.unattended) && !state.check_shows_notice) {
        options_.found_install_notice.clear();
        return;
    }
    const std::string words = std::move(options_.found_install_notice);
    options_.found_install_notice.clear();
    const std::size_t line_end = words.find('\n');
    UserFolderState::show_notice(
        *this,
        found_notice(words),
        line_end == std::string::npos
            ? options_.remember_game_dir
            : path_from_utf8(std::string_view(words).substr(line_end + 1)),
        Screen::main_menu
    );
    ++state.notices_shown;
}

} // namespace oa::app
