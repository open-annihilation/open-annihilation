// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards' command line in oa-game: the long options of its checks,
// their usage text and cross-checks, the "-c" switch, which names the online
// module and clears the launch block, and the "-y" switch, whose setup
// request network play's frontend states follow. A "-c" that names a launch
// file is left for network play.
#pragma once

#include "boneyards_launch_check.hpp"

#include "oa/app/command_line.hpp"
#include "oa/app/extension.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace oa::app {

// The Main Frame consoles --boneyards-open opens with their buttons.
enum class BoneyardsConsole : uint8_t { recon, chat, battle, news, ladders, profile, help };

/// What --check-boneyards-entry asks the main menu to do.
enum class BoneyardsEntryCheck : uint8_t {
    none,    ///< the flag was not given
    install, ///< the expansion is fetched, then the Boneyards open with it in use
    update,  ///< an offered update is noted, the latest package is in use, and the Boneyards open
};

/// Which way in --check-boneyards-menu-modes expects the mod's main menu to show.
enum class MenuModesCheck : uint8_t {
    strip,  ///< a menu on the base game's art: the strip, which takes every click on its rows
    button, ///< a menu with art of its own: the strip's ENTER plate, the menu's own buttons kept
};
inline constexpr const char* kBoneyardsConsoleNames[] = {
    "recon", "chat", "battle", "news", "ladders", "profile", "help"
};

struct BoneyardsOptions {
    // "-c <module>": the online module named, at most 63 characters; empty
    // when no "-c" named one, or when "-c" named a launch file and was left
    // for network play. No module is loaded natively.
    std::string online_module;
    // "-y": the main menu's first update enters the Boneyards; with
    // enter_boneyards_ticks the entry check waits for it instead of pressing
    // Enter Boneyards.
    bool setup_requested = false;
    // Logs into Central Command with OA_BY_USER / OA_BY_PASSWORD, loads the
    // galactic map and exits; implies headless_check and skip_intro.
    bool check_boneyards_login = false;
    // Recorded Rover replies that stand in for Central Command (offline
    // checks); installed before the Boneyards client is activated.
    std::filesystem::path boneyards_replay;
    // Server list used instead of <game-dir>/boneyards/serv_sav.byd, or the
    // host of a yard master to list alone.
    std::filesystem::path boneyards_server;
    // Headless: presses Enter Boneyards (or, with setup_requested, waits for
    // the main menu's first update to enter), runs this many idle passes one
    // frontend tick apart and writes --snapshot. Never logs in, unless
    // boneyards_login is set: then the account (OA_BY_USER / OA_BY_PASSWORD,
    // or a guest under --boneyards-replay) is typed into the login console and
    // the passes run on to the galactic map, with each composed frame written
    // to boneyards_frames when it is set, and the war map checked for
    // boneyards_planets planets when that is given.
    std::optional<std::size_t> enter_boneyards_ticks;
    bool boneyards_login = false;
    std::filesystem::path boneyards_frames;
    std::optional<std::size_t> boneyards_planets;
    // With boneyards_login, once on the galactic map: the planet with this
    // area id is hovered and pressed through SDL input and its census
    // awaited; the frame is written beside --snapshot as <stem>-pick.ppm.
    std::optional<int32_t> boneyards_pick;
    // Then this console's Main Frame button is clicked, the console written
    // as <stem>-<console>.ppm once shown, and Escape hands the map back.
    std::optional<BoneyardsConsole> boneyards_open;
    // With boneyards_open set to chat: this line is typed into the console
    // and sent with Enter before Escape, a reply must reach the chat log and
    // the console is written as <stem>-say.ppm.
    std::string boneyards_say;
    // In place of boneyards_pick and boneyards_open: the planet with this
    // area id is picked and CHAT opened on it; the entered frequency's rows
    // and log are checked against the replay, boneyards_say is said there,
    // the other planetary default is entered with a double click on its row
    // and Release leaves it (<stem>-chat.ppm, <stem>-chat-switched.ppm).
    std::optional<int32_t> boneyards_chat;
    // With boneyards_chat, before Release: CREATE opens the create-frequency
    // pane, this name is typed into it and GO pressed; the request must reach
    // Rover and the frequency the replay makes be entered
    // (<stem>-chat-create.ppm with the name typed).
    std::string boneyards_create;
    // A battle the Boneyards launches, played by this side in the running
    // game under --boneyards-replay (register_boneyards_launch_check); the
    // run needs --frames, which bounds it.
    std::optional<LaunchCheckRole> boneyards_launch;
    // The Phase 3 screens clicked through as this account in the running
    // game under --boneyards-replay (register_boneyards_features_check); the
    // run needs --frames, which bounds it.
    std::optional<FeaturesCheckRole> boneyards_features;
    // Headless: APPLY a unit limit from the Profile and leave it in the
    // engine's unit limit preference. The value is 21 through 500.
    std::optional<int32_t> check_boneyards_unit_limit;
    // Shows the main menu and checks the status strip is on the presented
    // frame at this tier (register_boneyards_overlay_check). The run is
    // unattended and skips the intro, and it keeps its window. The tier
    // flag itself is the engine's --hardware-acceleration.
    std::optional<OverlayCheckTier> boneyards_overlay;
    // Enters as a quiet run: brings the expansion up to date, opens the
    // Boneyards and checks that they read the installed expansion, then ends
    // the run. The run is unattended and skips the intro, and it keeps its
    // window. install fetches the expansion with both online checks off.
    // update is offered a newer game and goes on without it.
    BoneyardsEntryCheck boneyards_entry = BoneyardsEntryCheck::none;
    // Headless: pointer clicks on the strip's SINGLE, MULTI and EXIT, and
    // on the old MULTI column. Implies headless_check and skip_intro.
    bool check_strip_buttons = false;
    // Headless: the way in a mod's main menu shows, and what clicks on it
    // do. Implies headless_check and skip_intro.
    std::optional<MenuModesCheck> check_menu_modes;
    // Shows the main menu the way a windowed start does. It skips the intro
    // and is neither a headless check nor an unattended run, so a finished
    // welcome still asks the update check. With "-y", the login opens with
    // no input and leaving it ends the run.
    bool check_boneyards_shortcut = false;
    // Switches a made-up mod off, then enters, across soft restarts. It
    // skips the intro and is neither a headless check nor an unattended run,
    // so the question is shown and answered by the check. It runs on its own.
    bool check_boneyards_mod_entry = false;
};

// The Boneyards extension's option context: what the command line gave it
// and the handler of the switch it takes.
struct BoneyardsOptionContext {
    BoneyardsOptions options{};
    oa::app::command_line::SwitchHandler switches{};
};

/// Returns the process's Boneyards option context, which oa_extension_init_boneyards hands the table.
///
/// @return the one context, created empty on first use
[[nodiscard]] BoneyardsOptionContext& boneyards_option_context();

/// Returns the long options the Boneyards took from the command line.
///
/// @return boneyards_option_context().options
[[nodiscard]] inline const BoneyardsOptions& boneyards_options() {
    return boneyards_option_context().options;
}

/// Fills the Boneyards' extension hooks that need no running app.
///
/// take_option takes the Boneyards options: --check-boneyards-login and
/// --enter-boneyards imply a headless check that skips the intro,
/// --check-boneyards-launch, --check-boneyards-features,
/// --check-boneyards-overlay and --check-boneyards-entry skip the intro of
/// an unattended run, --check-strip-buttons and --check-boneyards-menu-modes
/// are headless checks that skip the intro, --check-boneyards-shortcut and
/// --check-boneyards-mod-entry skip the intro of a windowed run and are
/// neither headless nor unattended, and a missing or malformed value throws
/// std::runtime_error. check_options
/// refuses the options that come without the ones they need, each with its
/// own message. The switch handler takes "-y", and "-c" when it names the
/// online module. "-c" with no word after it skips the intro and clears
/// nothing. "-c <module>" skips the intro and, with a module named (the
/// text attached to the letter, else the next token), clears the launch
/// block (oa::app::netgame::extension_api::launch_block), as the online
/// module's loader does first, and records the module name, cut to 63
/// characters; switches after it still write the block. A "-c" whose word
/// is a launch file is declined, and that word is left for the handler
/// that follows: a path (a slash, a backslash, a colon, a space or a tab),
/// a name ending in .oalaunch or .ini, "-" (a launch read from standard
/// input) or a name that begins with "-". "-y" skips the intro and asks
/// network play's main menu for its setup step
/// (oa::app::netgame::extension_api::request_setup). text gives the
/// options' usage among the check options, and nothing else.
///
/// @param[in,out] table extension table; context, take_option,
///        check_options, switch_handler and text are set
/// @param[in,out] context context the hooks read and write; becomes `table.context`
void fill_boneyards_option_hooks(Extension& table, BoneyardsOptionContext& context);

} // namespace oa::app
