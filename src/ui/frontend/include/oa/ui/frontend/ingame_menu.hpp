// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// In-game menus: the ARMOPT options panel, the exit menu and its yes/no
// confirmation, the mission restart dialog, the "continue watching" prompt
// and the read-only game settings sheet.
#pragma once

#include "oa/data/match_rules.hpp"
#include "oa/ui/frontend_state/dispatcher.hpp"
#include "oa/ui/frontend/options.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace oa::ui::frontend {

// Campaign-object state the menus branch on.
using SessionKind = oa::data::campaign::SessionKind;

// What the exit confirmation leads to.
enum class ExitKind : int32_t { main_menu = 0, labelled_return = 1, leave_game = 2 };

// Room for a return label, its terminator included, and the longest label
// the menus show (IngameContext::return_label).
inline constexpr std::size_t kReturnLabelBytes = 0x20;
inline constexpr std::size_t kMaxReturnLabelLength = 9;

struct IngameHost {
    void* context = nullptr;
    void (*play_sound)(void* context, const char* name) = nullptr;
    void (*set_cd_playback)(void* context, bool playing) = nullptr;
    void (*release_lightbar)(void* context) = nullptr;
    bool (*disc_present)(void* context) = nullptr; // null: no disc check
    void (*refresh_archives)(void* context) = nullptr;
    void (*show_message)(void* context, const char* text, int32_t width) = nullptr;
    // Word-wraps `text` to `width` pixels with '\n' breaks; returns the length.
    std::size_t (*wrap_text)(
        void* context, const char* text, int32_t width, char* out, std::size_t capacity
    ) = nullptr;
};

struct IngameContext {
    IngameHost host{};
    prefs::Preferences* preferences = nullptr;
    SessionKind session = SessionKind::none;
    ExitKind exit_kind = ExitKind::main_menu;
    bool in_game = false;          // Game.session_flags bit 2
    bool multiplayer_link = false; // Game.session_flags bit 0: a multiplayer session is open
    bool hold_game = false;        // Game.sim_run_flags bit 0
    bool realtime_panels = false;  // Game.frame_flags bit 0
    bool spectating = false;       // Game.gui_flags bit 4
    // The label the game's return names in place of the main menu,
    // zero-terminated; empty for none.
    std::array<char, kReturnLabelBytes> return_label{};
    // The question asked before leaving the game in place of
    // kLeaveGameTitle, shown as written whatever the language
    // (strings.message.exit-confirm); empty asks kLeaveGameTitle.
    std::string leave_question;
    uint8_t quit_flags = 0;         // Game.outcome_flags
    bool restart_requested = false; // Game.restart_requested
    // The game data holds LOADGAME.GUI, the dialog SAVEGAME and LOADGAME open.
    bool saved_games_offered = true;
    // Which of 3.1c's names each difficulty carries (ai.difficulty-names).
    data::match_rules::AiDifficultyNames difficulty_names{};
};

namespace quit_flag {
inline constexpr uint8_t leave_application = 4; // Game.outcome_flags bit 2
inline constexpr uint8_t out_of_game = 0x10;    // Game.outcome_flags bit 4
} // namespace quit_flag

enum class IngameAction : uint8_t {
    none,
    closed, // the panel was dismissed
    open_load_game,
    open_save_game,
    open_options,
    open_help,
    open_briefing,
    open_game_settings,
    open_exit_menu,
    open_exit_confirm,
    open_restart,
    restart_mission,
    return_to_main_menu,
    leave_game,
    keep_watching,
};

/// Sets up the ARMOPT in-game options panel.
///
/// SAVEGAME and LOADGAME are greyed in multiplayer and when the game data has
/// no dialog for them, MISSION reads "Settings" in skirmish and multiplayer,
/// the game is held outside multiplayer and CD music resumes.
///
/// @param[in,out] panel The loaded ARMOPT panel.
/// @param[in,out] context Session and host; hold_game is set outside multiplayer.
void ingame_enter_options(Panel& panel, IngameContext& context) noexcept;

/// Handles a click on ARMOPT: LOADGAME, SAVEGAME, PREFS, HELP, MISSION, EXIT and OK.
///
/// Each button plays "Options"; MISSION opens the briefing in a campaign and
/// the game settings sheet otherwise. Closing the panel releases the
/// lightbar, lets an in-game single-player game run again, clears the
/// real-time panels flag and pauses CD music.
///
/// @param[in,out] panel The loaded ARMOPT panel; an unknown selection is cleared.
/// @param[in,out] context Session, flags and host.
/// @return The panel or action the caller opens next.
IngameAction ingame_on_options_click(Panel& panel, IngameContext& context) noexcept;

/// Sets up EXITMENU.
///
/// Campaign and skirmish show RESTART. Otherwise, for a spectator or a game
/// with a return label, one of MAINMENU (a spectator without a label) or
/// EXITGAME (a game with a label) is hidden and the other takes the return
/// label when it has 1 to kMaxReturnLabelLength characters.
///
/// @param[in,out] panel The loaded EXITMENU panel.
/// @param context Session, spectator and return label.
void ingame_enter_exit_menu(Panel& panel, IngameContext& context) noexcept;

/// Handles a click on EXITMENU.
///
/// Every click plays "Options"; MAINMENU and EXITGAME record the exit kind
/// and ask for confirmation, RESTART opens the restart dialog, CANCEL closes.
///
/// @param[in,out] panel The loaded EXITMENU panel; an unknown selection is cleared.
/// @param[in,out] context Receives the exit kind.
/// @return The dialog to open next, closed or none.
IngameAction ingame_on_exit_menu_click(Panel& panel, IngameContext& context) noexcept;

/// The exit confirmation's answer to Enter and to Escape: its Enter and Escape
/// default, which also takes the focus as it opens. The confirmation a close
/// request opens answers Enter with kCloseConfirmAccept instead.
inline constexpr std::string_view kExitConfirmDefault = "CHOICE2";

/// The answer to Enter on the exit confirmation a request to close the window
/// or quit the program opens: Yes, which also takes the focus as it opens.
/// Escape still answers kExitConfirmDefault.
inline constexpr std::string_view kCloseConfirmAccept = "CHOICE1";

/// Returns the exit confirmation's answer to Enter, which also takes the focus
/// as it opens.
///
/// @param close_request whether a request to close the window or quit the
///     program opened the confirmation
/// @return kCloseConfirmAccept for a close request; otherwise
///     kExitConfirmDefault, as in 3.1c
[[nodiscard]] std::string_view ingame_exit_confirm_accept(bool close_request) noexcept;

/// The exit confirmation's title when the player leaves the game. 3.1c's
/// wording names Windows as the place the player exits to; the engine runs
/// on several systems, so it says "the system" instead.
inline constexpr std::string_view kLeaveGameTitle = "Surrender this battle and exit to the system?";

/// Sets up YESORNO as the exit confirmation, titled from the exit kind.
///
/// Main menu asks "Surrender this battle and return to main menu?" (the
/// return label instead of "main menu" when it fits); leaving asks the
/// context's leave_question, else kLeaveGameTitle, or "Exit the Battle" for a
/// spectator. Escape answers kExitConfirmDefault, and Enter answers
/// ingame_exit_confirm_accept()'s choice.
/// CHOICE1 and CHOICE2 read "Yes" and "No" in the language shown, and each
/// takes as its quick key the first letter of its caption that the other's
/// key does not hold: Y and N in English, J and N in German, as in 3.1c.
///
/// @param[in,out] panel The loaded YESORNO panel.
/// @param context Exit kind, spectator and return label.
void ingame_enter_exit_confirm(Panel& panel, const IngameContext& context) noexcept;

/// Opens the exit confirmation directly for leaving the game.
///
/// @param[in,out] panel The loaded YESORNO panel.
/// @param[in,out] context Its exit kind becomes leave_game.
void ingame_open_leave_confirm(Panel& panel, IngameContext& context) noexcept;

/// Handles the exit confirmation's answer.
///
/// Every click plays "Exit". CHOICE1 returns to the main menu, or for
/// leave_game raises quit_flag::leave_application and leaves; CHOICE2 closes.
///
/// @param[in,out] panel The loaded YESORNO panel; an unknown selection is cleared.
/// @param[in,out] context Exit kind; quit_flags gains leave_application.
/// @return What the caller does next.
IngameAction ingame_on_exit_confirm_click(Panel& panel, IngameContext& context) noexcept;

/// Sets up the RESTART dialog.
///
/// The mission name is wrapped to MISSIONNAME's width and its first two lines
/// go to MISSIONNAME and MISSIONNAME1 (leading breaks skipped); the stored
/// difficulty shows on the Difficulty button.
///
/// @param[in,out] panel The loaded RESTART panel.
/// @param context Preferences and the wrap service.
/// @param mission Mission name (up to 255 characters are used).
void ingame_enter_restart(Panel& panel, IngameContext& context, std::string_view mission) noexcept;

/// Handles a click on the RESTART dialog.
///
/// Every click plays "Options" (Difficulty plays it twice). RESTART, only in
/// a campaign or skirmish, needs the campaign or multiplayer disc, refreshes
/// the archives, stores the chosen difficulty in the preferences and requests
/// the restart. CANCEL closes.
///
/// @param[in,out] panel The loaded RESTART panel; other selections are cleared.
/// @param[in,out] context Preferences and host; restart_requested is set.
/// @return restart_mission, closed or none.
IngameAction ingame_on_restart_click(Panel& panel, IngameContext& context) noexcept;

enum class RestartPath : uint8_t { none, campaign, skirmish };

// Services a restart calls, in this order.
// Null members act as absent services.
struct RestartHost {
    void* context = nullptr;
    // CampaignFile.mission_index: the mission the campaign has bound.
    int32_t (*bound_mission)(void* context) = nullptr;
    // Tears the game session down and redraws the frame.
    void (*end_session)(void* context) = nullptr;
    // Loads the campaign file again under its own name.
    void (*reload_campaign)(void* context) = nullptr;
    // Binds campaign mission `index`; false when it cannot.
    bool (*bind_mission)(void* context, int32_t index) = nullptr;
    // Selects the skirmish map again for Game.player_count players.
    void (*select_skirmish_map)(void* context) = nullptr;
    // Fills the player slots from the skirmish roster.
    void (*apply_roster)(void* context) = nullptr;
    // Enters the frontend mode (app mode 2); `in_game` (Game.session_flags bit 2) has
    // it load the bound game instead of staying in the menus.
    void (*enter_frontend)(void* context, bool in_game) = nullptr;
};

/// Restarts the mission once RESTART raised the request.
///
/// A campaign tears the session down, reads the mission it had bound, reloads
/// its file (which binds mission 0) and rebinds that mission, raising the
/// single-player flag on success. Any other session keeps Game.player_count across the
/// teardown, selects its map again and refills the player slots from the
/// roster. Both then enter the frontend mode, which loads the game unless the
/// rebind failed.
///
/// @param context Session and the restart request.
/// @param[in,out] app Frontend state: session_flags and player_count.
/// @param host Teardown, campaign, map and frontend services, called in that order.
/// @return Which branch ran; none without a restart request.
RestartPath ingame_run_restart(
    const IngameContext& context, ui::frontend_state::State& app, const RestartHost& host
) noexcept;

/// Sets up YESORNO for "You're out!  Continue Watching?".
///
/// CHOICE1 and CHOICE2 read "Yes" and "No" in the language shown, with the
/// quick keys their captions give them, as ingame_enter_exit_confirm() gives them.
///
/// @param[in,out] panel The loaded YESORNO panel.
void ingame_enter_continue_watching(Panel& panel) noexcept;

/// Handles the continue-watching answer.
///
/// Every click plays "BigButton". CHOICE1 keeps spectating; CHOICE2 raises
/// quit_flag::leave_application and leaves. Both clear quit_flag::out_of_game.
///
/// @param[in,out] panel The loaded YESORNO panel; an unknown selection is cleared.
/// @param[in,out] context Its quit_flags change.
/// @return keep_watching, leave_game or none.
IngameAction ingame_on_continue_watching_click(Panel& panel, IngameContext& context) noexcept;

// Values the game settings sheet shows, resolved by the caller.
struct GameSettingsView {
    SessionKind session = SessionKind::none;
    uint32_t commander_rule = 0; // Game.session_rules: continues, ends, deathmatch
    uint32_t fixed_locations = 0;
    uint16_t mapping_flags =
        0; // Game.visibility_flags: bit 0 unmapped, bit 1 LOS on, bit 2 circular
    bool cheats_allowed = false;
    bool watching_allowed = false;
    uint32_t difficulty = 0; // Game.difficulty
    std::string_view map_name;
    uint32_t starting_metal = 0;
    uint32_t starting_energy = 0;
    uint16_t max_units = 0; // Game.units_per_player
    // Which of 3.1c's names each difficulty carries (ai.difficulty-names).
    data::match_rules::AiDifficultyNames difficulty_names{};
};

struct SettingsEntry {
    std::array<char, kControlTextBytes> text{};
    int16_t x = 0;
    uint16_t y = 0;
    int32_t width = 0;
};

inline constexpr std::size_t kSettingsEntries = 20;

struct GameSettingsSheet {
    std::array<SettingsEntry, kSettingsEntries> entries{};
    std::size_t count = 0;
};

// Translates a label; identity when null.
using TranslateFn = const char* (*)(void* context, const char* text);

/// Builds the GAMEOPTIONS label/value rows for the running game.
///
/// Rows, one kSettingsRowStep apart: commander death, starting locations,
/// mapping mode, line of sight, then cheat codes and watching in multiplayer
/// or difficulty otherwise, map, starting metal, starting energy and max
/// units. Labels and word values are translated; numbers are not.
///
/// @param view Values of the running game.
/// @param[out] sheet Rows built; reset first.
/// @param translate Label lookup; null keeps the English text.
/// @param translate_context Context passed to `translate`.
void ingame_build_game_settings(
    const GameSettingsView& view,
    GameSettingsSheet& sheet,
    TranslateFn translate = nullptr,
    void* translate_context = nullptr
) noexcept;

/// Handles a click on GAMEOPTIONS: OK plays the options sound and closes; anything else deselects.
///
/// @param[in,out] panel The loaded GAMEOPTIONS panel.
/// @param context Host for the sound.
/// @return closed for OK, otherwise none.
IngameAction ingame_on_game_settings_click(Panel& panel, IngameContext& context) noexcept;

} // namespace oa::ui::frontend
