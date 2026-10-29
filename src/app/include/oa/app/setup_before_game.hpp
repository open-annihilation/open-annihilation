// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Steps 1 to 3 of the setup, before the game is mounted: language, where the
// game is, and the copy with its check. A player who already has the game
// does not see them. The menu welcome continues at online features when this
// start copied the game and the welcome is not finished.

#pragma once

#include "oa/app/app.hpp"
#include "oa/ui/first_run/first_run.hpp"

#include <SDL3/SDL.h>
#include <filesystem>
#include <string>

namespace oa::app {

class RendererHost;

/// How the setup reaches the game's window, which the caller opens first.
struct SetupDisplay {
    SDL_Window* window{};     ///< the game's window
    SDL_Renderer* renderer{}; ///< its renderer
    RendererHost* host{};     ///< what made the renderer; may be null
};

/// How the pre-game setup ended.
enum class SetupBefore : uint8_t {
    not_run, ///< this start does not show the game steps
    skipped, ///< Skip: the silent copy may still run
    copied,  ///< the copy finished on the copying step
    quit,    ///< the player left before the game was copied
};

/// Reports whether this start should show language, the game and copying
/// before the game is mounted. A named --game-dir, --check-first-run, and an
/// unattended start do not. --check-setup-window does: the window copies the
/// check's install, and the menu still plays the named folder.
///
/// @param options the parsed command line
/// @param mount_copy true when this start plays the player's copy
/// @return true when the caller should open the window and show the steps
[[nodiscard]] bool setup_before_game_due(const Options& options, bool mount_copy);

/// Puts the language the preferences store in effect, before a line is drawn.
///
/// @param options the parsed command line
void apply_stored_interface_language(const Options& options);

/// SHOW AGAIN walks language, online features and the buttons. It does not
/// continue a five-step walk that already copied the game on this start.
void setup_welcome_again() noexcept;

/// Shows language, the game and copying when this start has no managed copy
/// and did not name --game-dir. --check-first-run walks those steps with the
/// menu instead, so a player who named a folder never sees them here.
///
/// @param options the parsed command line
/// @param display the window, opened by the caller
/// @param mount_copy true when this start plays the player's copy
/// @param found_install a folder already found, listed with the other sources
/// @return how it ended
[[nodiscard]] SetupBefore run_setup_before_game(
    const Options& options,
    SetupDisplay display,
    bool mount_copy,
    const std::filesystem::path& found_install
);

/// The step the menu welcome opens on after this function's copy.
///
/// @return online when the five-step walk continues; language otherwise
[[nodiscard]] oa::ui::first_run::Step setup_menu_step() noexcept;

/// The walk the menu welcome continues after this function's copy.
///
/// @return setup when the five-step walk continues; welcome otherwise
[[nodiscard]] oa::ui::first_run::Walk setup_menu_walk() noexcept;

/// The left button the menu's continuation of this walk shows.
///
/// A first start that copied here keeps Quit. A re-run keeps Skip. When this
/// window did not run, the five-step walk says Quit.
///
/// @return the word for every step of that continuation
[[nodiscard]] oa::ui::first_run::Leave setup_menu_leave() noexcept;

/// Reports whether this start already copied on the copying step.
///
/// @return true when the silent copy must not run again
[[nodiscard]] bool setup_copied_this_start() noexcept;

/// Puts the check's small install into the model, under the player's folder.
///
/// @param user_folder the player's folder
/// @param[in,out] model the game step's model
void setup_install_check_source(
    const std::filesystem::path& user_folder, oa::ui::first_run::Model& model
);

/// Starts the copy of the model's chosen source on a side thread.
///
/// @param user_folder the player's folder
/// @param model the game step, with a source chosen
/// @return false when nothing was chosen or the source cannot be opened
[[nodiscard]] bool
setup_begin_copy(const std::filesystem::path& user_folder, const oa::ui::first_run::Model& model);

/// Pauses the copy, or lets it continue.
///
/// @param paused true to pause
void setup_set_paused(bool paused);

/// Asks the copy to stop and return to the game step.
void setup_request_cancel();

/// Copies the copying step's progress into `view`.
///
/// @param[out] view the progress
/// @return true when it changed since the last pull
[[nodiscard]] bool setup_pull_copy(oa::ui::first_run::CopyView& view);

/// Waits for a copy that is still running, after the setup has closed.
void setup_stop_join();

/// Asks for another place and adds it to the game step when it holds the game.
///
/// The system's folder picker is used. On a platform that brings game files
/// in, that picker's hook is used instead, so the system's sheet is the one
/// the player already knows. A cancel leaves the model as it was.
///
/// @param window the window the picker is attached to; may be null
/// @param user_folder the player's folder, so a place already inside the copy is not listed
/// @param[in,out] model the game step
/// @return true when a place was added
[[nodiscard]] bool setup_locate_source(
    SDL_Window* window, const std::filesystem::path& user_folder, oa::ui::first_run::Model& model
);

/// Ends access a system picker gave, after a copy no longer reads the place.
void setup_release_located();

/// Tells whether the game step offers Download from GOG…: this system can
/// reach GOG.
///
/// @return true when the button shows
[[nodiscard]] bool setup_gog_offered() noexcept;

/// Answers the game step's GOG asks. gog_open opens GOG's sign-in page in
/// the browser and asks for the address it ends on; gog_paste reads that
/// address from the clipboard and starts the download; gog_cancel stops a
/// download, which a later try resumes, and closes the question.
///
/// @param ask gog_open, gog_paste or gog_cancel; any other is ignored
/// @param window the game's window, which GOG's page shows inside
/// @param user_folder the player's folder
/// @param[in,out] model the game step
void setup_gog_ask(
    oa::ui::first_run::Ask ask,
    SDL_Window* window,
    const std::filesystem::path& user_folder,
    oa::ui::first_run::Model& model
);

/// What setup_gog_tick did.
enum class GogTick : uint8_t {
    none,    ///< nothing the step shows changed
    changed, ///< the question changed
    ready,   ///< the installer is listed and chosen, and the copy can begin
};

/// Follows the GOG download: starts it when the clipboard comes to hold
/// GOG's address while its page is open, moves its progress into the
/// question, and when the installer is whole lists it on the game step,
/// chosen, and closes the question.
///
/// @param user_folder the player's folder
/// @param sources_ready the machine's own places are on the step, so a new row is kept
/// @param[in,out] model the game step
/// @return what changed
[[nodiscard]] GogTick setup_gog_tick(
    const std::filesystem::path& user_folder, bool sources_ready, oa::ui::first_run::Model& model
);

/// Stops a GOG download that is still running and waits for its thread.
void setup_gog_stop();

} // namespace oa::app
