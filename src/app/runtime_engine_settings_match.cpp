// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The settings dialog opened in a match, beside the darkened in-game menu's
// column, as a screen of the OA layer (oa_layer.hpp), which also holds the
// in-game menu's OA button; and the locks the game shown puts on the settings.

#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <cstdint>
#include <string_view>

namespace oa::app {

namespace settings = oa::ui::engine_settings;

namespace {

/// The sound opening the dialog plays, as on the main menu.
constexpr std::string_view kOpenSound = "Options";

} // namespace

void Runtime::open_engine_settings_in_match(settings::DialogKind kind) {
    if (screen_ != Screen::match || !match_ || match_finished_ ||
        engine_settings_dialog() != nullptr || engine_settings_fonts() == nullptr)
        return;
    if (kind == settings::DialogKind::mod_options && !ui_rules().options_dialog.enabled)
        return;
    if (!ingame_menu_column_shown()) {
        // From play only: a panel the menu opened, a team panel or a message
        // box keeps the column.
        if (pause_menu_shown() || oa::ui::frontend_dialogs::dialog_count() != 0)
            return;
        show_match_pause_menu();
        if (!ingame_menu_column_shown())
            return;
    }
    // The dialog is drawn and fed from engine_settings_dialog(), as the OA
    // layer's settings screen. A settings screen whose dialog closed outside
    // its events goes first.
    if (oa_layer().find("settings") != nullptr) {
        oa_layer().close_above("settings");
        oa_layer().close_top();
    }
    open_engine_settings_dialog(kind);
    play_ui_sound(kOpenSound, 0);
    push_settings_screen(true);
}

settings::Locks Runtime::engine_settings_locks() const {
    settings::GameState state{};
    state.in_game = static_cast<bool>(match_);
    const uint32_t extension = current_extension_state();
    state.shared_game = (extension & extension_state::shared_match) != 0;
    state.replay = (extension & extension_state::replay) != 0;
    state.frame_rate_from_command_line = options_.max_frames_per_second_given;
    state.renderer_from_command_line = options_.hardware_acceleration.has_value();
    const auto report = acceleration_report();
    state.acceleration_unavailable = report.acceleration_unavailable;
    state.vertical_sync_unavailable = report.vertical_sync_unavailable;
    state.language_from_command_line =
        oa::app::command_line::launch_language(options_.launch) != nullptr;
    state.native_density_windows = options_.native_density_windows;
    state.native_density_from_command_line = options_.native_density;
    auto locks = settings::settings_locks(state);
    if (!state.in_game) {
        if ((extension & extension_state::multiplayer) != 0)
            locks.enhanced_mode = settings::Lock::set_by_host;
        else if (options_.enhanced_mode.has_value())
            locks.enhanced_mode = settings::Lock::command_line;
    }
    return locks;
}

} // namespace oa::app
