// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Game files screen on the game's window: its loop of events, the import's
// workers polled into the screen's model, the layout painted and presented,
// and the screen as the --check-game-files check sees it while it runs.
#pragma once

#include "oa/app/game_files_import.hpp"
#include "oa/ui/game_files.hpp"
#include "oa/ui/paint/painter.hpp"

#include <SDL3/SDL.h>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace oa::app {

// Named once, whichever app header that draws with the painter is included first.
#ifndef OA_APP_UI_PAINT
#define OA_APP_UI_PAINT
namespace paint = oa::ui::paint;
#endif

class GameFilesScreen;
class RendererHost;

/// How the screen was opened.
enum class GameFilesEntry : uint8_t {
    first_run, ///< no usable game folder at the start: S1
    manage,    ///< Settings › Game files › Manage…: S8
};

/// How the screen ended.
enum class GameFilesEnd : uint8_t {
    play, ///< PLAY: resolve the game folder again and start the game
    done, ///< DONE in the management state
    quit, ///< the window was closed or the app asked to quit
};

/// What the check sees and does each pass of the screen's loop (game_files_check.cpp); the
/// player's run leaves it empty.
struct GameFilesScreenCheckHooks {
    void* context{}; ///< passed back to every hook
    /// Runs after each pass's events, with the screen; may tap and write pictures. Null: none.
    void (*pass)(void* context, GameFilesScreen& screen){};
};

/// What the screen needs.
struct GameFilesScreenRequest {
    SDL_Window* window{};     ///< the game's window
    SDL_Renderer* renderer{}; ///< its renderer
    /// What made the renderer, whose start-up stage the screen's frames count towards
    /// (RendererHost::note_presented_frame). Null: nothing is counted.
    RendererHost* host{};
    GameFilesEntry entry{GameFilesEntry::first_run}; ///< how the screen was opened
    game_files::ImportPaths paths{};                 ///< the import's folders
    game_files::RecoveryResult recovery{};      ///< the start's recovery (copy_waiting: S7 banner)
    GameFilesNeeded needed{};                   ///< why there is no folder (S1's refused banner)
    ModChoice mod{};                            ///< the check's mod choice
    std::string version{};                      ///< "v" + the engine's version
    std::optional<fs::path> preferences_file{}; ///< where the backup and language settings live
    std::optional<fs::path> user_folder{};      ///< --user-folder; empty leaves the player's folder
    bool players_own_profile{};                 ///< the preferences are the player's own
    GameFilesScreenCheckHooks check{};          ///< the check's per-pass hooks; empty for players
};

/// The touch device the check's taps come from, which the screen and the Language
/// dialog accept as they accept a touch screen's fingers.
inline constexpr SDL_TouchID game_files_check_touch_id = 0x6766;
/// The finger the check's taps use.
inline constexpr SDL_FingerID game_files_check_finger_id = 1;

/// The screen's side of the Language dialog (game_files_dialog.cpp): how its frames
/// reach the window, and the check's per-pass hook while it is up.
struct GameFilesDialogHooks {
    void* context{}; ///< passed back to every hook
    /// Shows a canvas of the window's render output size on the window, as the screen's frame.
    void (*present)(void* context, const paint::Canvas& canvas){};
    /// Returns the viewport as it is now: the window may change while the dialog is up.
    oa::ui::game_files::Viewport (*viewport)(void* context){};
    /// Runs after each pass's events while the dialog is up (the check's hook). Null: none.
    void (*pass)(void* context){};
};

/// What the Language dialog over the Game files screen needs.
struct GameFilesLanguageRequest {
    const paint::Canvas* under{};                ///< the screen's frame the dialog lies over
    oa::platform::text_font::FontStack* fonts{}; ///< the bundled fonts the dialog's text uses
    std::string version{};                       ///< the dialog header's version text
    std::optional<fs::path> preferences_file{};  ///< --preferences-file; empty: the player's own
    std::optional<fs::path> user_folder{}; ///< --user-folder; empty leaves the player's folder
    GameFilesDialogHooks hooks{};          ///< the screen's side
};

/// Shows the settings dialog limited to Language over the Game files screen, drawn
/// wholly in the bundled fonts because the game's own are not installed yet, until OK or
/// Cancel. While it is up the game text hooks draw with the bundled fonts; the hooks in
/// place before are put back after. OK writes the settings to the preferences file and puts
/// the language chosen in effect for the interface; Cancel puts the opened one back. A quit
/// or a close request ends it as Cancel and is passed on to the screen.
///
/// @param request the screen's frame, the fonts and the preferences file
/// @return true when OK was pressed
bool run_game_files_language_dialog(const GameFilesLanguageRequest& request);

/// Puts in effect, for the interface's words, the language the preferences file chooses, with
/// the interface catalogue read from the languages folder beside the game and the player's
/// Languages folder. The Game files screen runs before the Runtime, which chooses the language
/// again once it starts.
///
/// @param preferences_file --preferences-file; empty: the player's own file
/// @param user_folder_option --user-folder; empty leaves the player's folder to the preferences
void install_game_files_language(
    const std::optional<fs::path>& preferences_file,
    const std::optional<fs::path>& user_folder_option
);

/// Returns the family the Game files screen, the folder chooser and the Language dialog
/// over them draw their text in: the interface family, Barlow, while the Look the
/// preferences file holds is Modern (Settings › User Interface › Look; Modern by default in
/// the player's own file), and the sans family, DejaVu Sans, while it is Classic, whose
/// game fonts these screens never have.
///
/// @param preferences_file --preferences-file; empty: the player's own file
/// @return the family
[[nodiscard]] oa::platform::text_font::Family
game_files_text_family(const std::optional<fs::path>& preferences_file);

/// Reads the interface catalogue and the engine's and the player's language
/// packs once, without registering them. The management screen reads them
/// so its fonts can follow the language the runtime already shows.
///
/// @param user_folder_option --user-folder; empty leaves the player's folder
/// @param preferences_file --preferences-file; empty: the player's own file
void read_game_files_language_packs(
    const std::optional<fs::path>& user_folder_option,
    const std::optional<fs::path>& preferences_file
);

/// Removes the stack's pack faces and adds the faces of the packs whose tag
/// or word is the language the interface shows.
///
/// The player's packs come first, then the engine's, each pack's fonts in
/// manifest order, a path once, and at most most_pack_faces. A face that
/// does not open is logged.
///
/// @param stack the open stack
void use_game_files_language_fonts(oa::platform::text_font::FontStack& stack);

/// Returns the header's version text: "v" and the engine's version.
///
/// @return the text, such as "v0.6.0"
[[nodiscard]] std::string game_files_version_text();

/// Runs the screen on the game's window until PLAY, DONE or a quit, then puts the renderer's
/// target, logical presentation, viewport, clip and scale back as it found them.
///
/// @param request the window, the import's folders and how the screen was opened
/// @return how the screen ended
[[nodiscard]] GameFilesEnd run_game_files_screen(const GameFilesScreenRequest& request);

/// The screen, as the check sees it while it runs.
class GameFilesScreen {
  public:

    /// The model as last updated from the import.
    ///
    /// @return the model
    [[nodiscard]] const oa::ui::game_files::Model& model() const noexcept;
    /// The layout last painted.
    ///
    /// @return the layout
    [[nodiscard]] const oa::ui::game_files::Layout& layout() const noexcept;
    /// The viewport last laid out.
    ///
    /// @return the viewport
    [[nodiscard]] const oa::ui::game_files::Viewport& viewport() const noexcept;
    /// The canvas last painted (the window's frame).
    ///
    /// @return the canvas
    [[nodiscard]] const paint::Canvas& canvas() const noexcept;
    /// The bundled fonts.
    ///
    /// @return the fonts; null when they could not be opened
    [[nodiscard]] oa::platform::text_font::FontStack* fonts() const noexcept;
    /// Pushes a finger down and up at the control's centre as SDL events (touch id of the
    /// check, window coordinates normalised), so input mapping is exercised.
    ///
    /// @param control the control to tap
    /// @return false when the control is not on the layout
    bool tap(oa::ui::game_files::Control control);
    /// Pushes a key press and release.
    ///
    /// @param key the key
    /// @param modifiers the modifiers held
    void press_key(SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE);
    /// The copy's latest snapshot, for the check's verdicts.
    ///
    /// @return the run's snapshot
    [[nodiscard]] const game_files::RunSnapshot& run() const noexcept;
    /// The scan's latest snapshot, for the check's verdicts.
    ///
    /// @return the scan's snapshot
    [[nodiscard]] const game_files::ScanSnapshot& scan() const noexcept;
    /// Asks the loop to end as a quit (the check's way out).
    void request_quit() noexcept;
    /// Tells whether the Language dialog is up over the screen; canvas() then holds
    /// the dialog's frame.
    ///
    /// @return true while the dialog is up
    [[nodiscard]] bool language_open() const noexcept;
    /// Delivers the answer of a picker the check holds back: the screen takes it as it takes
    /// the platform's answer on the main thread.
    ///
    /// @param paths the chosen paths; none for a cancel
    /// @param movable the paths are copies made for the game that the engine may move
    /// @param error why the picker failed; null when it did not
    void answer_picker(const std::vector<std::string>& paths, bool movable, const char* error);
    /// Tells whether the screen waits for a picker's answer.
    ///
    /// @return true from show_picker until its answer arrives
    [[nodiscard]] bool picker_open() const noexcept;
    /// The paths the screen still holds from the pickers (not yet released).
    ///
    /// @return how many
    [[nodiscard]] std::size_t held_sources() const noexcept;
    /// Tells whether a player's run of the loop would now wait for an event with no time
    /// limit: nothing the import does is left to poll.
    ///
    /// @return true when only an event would wake the loop
    [[nodiscard]] bool waits_for_events_only() const noexcept;

  private:

    /// The screen's loop, which makes the check's view of its state.
    friend GameFilesEnd run_game_files_screen(const GameFilesScreenRequest& request);
    struct State;
    /// Makes the check's view of a running screen.
    ///
    /// @param state the loop's state
    explicit GameFilesScreen(State& state) noexcept;
    State* state_{}; ///< the loop's state
};

} // namespace oa::app
