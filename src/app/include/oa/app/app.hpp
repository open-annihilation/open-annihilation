// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Shared application options, screen identifiers and layout constants for oa-game.
#pragma once

#include "oa/app/command_line.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_state/game_entry.hpp"
#include "oa/ui/frontend_state/initialization.hpp"
#include "oa/ui/frontend_state/main_menu.hpp"
#include "oa/ui/frontend_state/map_selection.hpp"
#include "oa/sim/scenario/outcome.hpp"
#include "oa/ui/frontend_state/skirmish_ui.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/screen_registry.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace oa::data::mod_profile {
struct ModProfile;
} // namespace oa::data::mod_profile

namespace oa::app {

namespace fs = std::filesystem;
namespace renderer = oa::ui::frontend_renderer;
namespace frontend = oa::ui::frontend_state;
namespace menu = oa::ui::frontend_state::main_menu;
namespace entry = oa::ui::frontend_state::game_entry;
namespace init = oa::ui::frontend_state::initialization;
namespace skirmish = oa::ui::frontend_state::skirmish_ui;
namespace map_modal = oa::ui::frontend_state::map_selection;
namespace scenario = oa::sim::scenario;

constexpr int kCanvasWidth = 640;
constexpr int kCanvasHeight = 480;
constexpr int kBattlefieldLeft = 128;
constexpr int kBattlefieldTop = 32;
constexpr int kBattlefieldBottom = 32;
constexpr int kBattlefieldWidth = kCanvasWidth - kBattlefieldLeft;
constexpr int kBattlefieldHeight = kCanvasHeight - kBattlefieldTop - kBattlefieldBottom;
constexpr std::size_t kDefaultCampaignTicks = 3000; // headless --mission without --match-ticks
constexpr uint16_t kSkirmishUnitsPerPlayer = 250;
/// The zoom floor of Maximum zoom out's Automatic while the processor draws
/// the battlefield (the Off and Basic tiers).
constexpr float kMinBattlefieldZoom = 0.5F;
/// The zoom floor of Maximum zoom out's Automatic while the graphics card
/// draws the battlefield (the Full tier): three times as far out as the
/// processor's floor.
constexpr float kMinFullBattlefieldZoom = kMinBattlefieldZoom / 3.0F;
/// The closest any view zooms in, and Maximum zoom in's closest choice.
constexpr float kMaxBattlefieldZoom = 4.0F;
constexpr float kDefaultBattlefieldZoom = 1.0F;
constexpr float kZoomWheelFactor = 1.15F;
constexpr float kZoomLerpHz = 12.0F;
/// The camera a terrain cache notes before it holds a fill: one no view
/// reaches, so that the first fill is never mistaken for the cache's.
constexpr int32_t kUncachedTerrainCamera = INT32_MIN;
/// Bytes the model bridge takes for each map pixel a frame shows: its 8-bit
/// picture and the indices it captured.
constexpr uint64_t kModelBridgeBytesPerMapPixel = 2;
/// The share of the machine's physical memory the models' drawing may take
/// past the drawing's floor, where it grows with the map shown: an eighth.
constexpr uint64_t kRenderedUnitsMemoryShare = 8;
/// The memory the models' drawing may take past the drawing's floor where
/// the machine does not say how much it has: 512 MiB.
constexpr uint64_t kRenderedUnitsUnknownBudget = uint64_t{512} << 20U;
/// Screen pixels past the battlefield's edges a frame still draws what
/// stands there at least, so that a large unit or its shadow reaching into
/// the view is drawn.
constexpr int kLeastCullMargin = 256;
/// Map pixels past the battlefield's edges a zoomed-in frame draws what
/// stands there, in screen pixels at the zoom where that is more than
/// kLeastCullMargin.
constexpr double kCullMarginMapPixels = 128.0;
/// Map pixels past the battlefield's edges a frame past the tier's floor
/// draws what stands there: room for the largest unit and its shadow.
constexpr double kFarCullMarginMapPixels = 512.0;
/// The most map pixels from the camera a unit drawn as a model may stand:
/// the model routines place each piece by its 16-bit offset from the
/// camera.
constexpr double kMostModelOffset = 32767.0;
constexpr uint8_t kPaletteGreen = 250;   // PALETTE.PAL RGB(0,255,0)
constexpr std::size_t kUiColorText = 15; // Game.ui_colors slot of message and clock text
constexpr int kDefaultWindowWidth = 1920;
constexpr int kDefaultWindowHeight = 1080;
// Headless and composed-frame checks run on a clock that advances only with
// the simulation (at 30 ticks a second) and seed the match with rand()'s
// initial seed, so two runs draw the same frames.
constexpr uint32_t kFixedClockMsPerTick = 1000 / 30;
constexpr uint32_t kFixedRandomSeed = 1;
// The most frames a second the application loop draws unless --max-fps says
// otherwise; the simulation keeps its 30 ticks a second whatever the rate.
constexpr uint32_t kDefaultMaxFramesPerSecond = 120;
// The lowest limit --max-fps takes: the simulation's 30 ticks a second, so
// that each tick is drawn before the next runs. Above it, frames on time are
// less than a clock unit apart, and no frame's clock step at normal speed runs
// two ticks, even as the match clock rounds to whole milliseconds. At it, each
// frame is due at the middle of a clock unit (frame_pacing::end_paced_frame)
// and runs exactly one tick at normal speed, which it shows whole. Below it,
// some frames would run two.
constexpr uint32_t kLowestMaxFramesPerSecond = 30;
// The highest --max-fps and --frame-rate take.
constexpr uint32_t kHighestFrameRate = 1000;
constexpr uintptr_t kFrontendMenuHandle = 1;
constexpr uintptr_t kMessageTargetHandle = 1;

enum class Screen : ScreenId {
    main_menu,
    single_player,
    skirmish,
    map_selection,
    loading,
    match,
    options,
    sound,
    visuals,
    speeds,
    music,
    new_campaign,
    any_mission,
    load_game,
    campaign_end,
    briefing
};

/// Returns the registry id of a built-in screen.
///
/// @param screen built-in screen
/// @return the ScreenId the screen registers under, the enumerator's value
[[nodiscard]] constexpr ScreenId screen_id(Screen screen) {
    return static_cast<ScreenId>(screen);
}

// The scripted showcases --showcase plays (runtime_showcase.cpp).
enum class Showcase {
    none,
    // From the main menu into the first Arm mission, whose units are sent to
    // the Galactic Gate, and through its victory to the score screen.
    arm_first_mission,
    // From the main menu into a two-player skirmish whose armies fight for a
    // minute, reporting the ticks and frames a second it played at.
    skirmish_battle
};

/// The part of the navigation check --check-navigation runs. Each group
/// starts from the main menu and sets up the menus and the match it needs,
/// so it passes run alone; all runs every group's checks in one sequence.
enum class NavigationGroup : uint8_t {
    /// every check below, one after another
    all,
    /// the menus' screens and services, the Options screen's gamma, and a
    /// skirmish's first frames, console commands, speed messages and orders
    screens,
    /// a builder's orders, build placement, the selection's visuals, the
    /// Pause key, the team panels and the match panels' keys
    orders,
    /// a skirmish's victory, a deathmatch's respawn, the D-gun and attack
    /// orders, and a turret drawn as it turns
    outcomes,
    /// the battlefield's zoom and follow, with the zoom's limits on the
    /// game's own screen, and presses on the far view
    zoom,
    /// the zoom's limits on a window of 1366x768
    zoom_1366x768,
    /// the zoom's limits on a window of 1920x1080
    zoom_1920x1080,
    /// the zoom's limits on a window of 2560x1440
    zoom_2560x1440,
    /// the launch services, the map selection, a campaign mission through
    /// its end screen and saves, the campaign screens and the dialogs
    campaign
};

/// A renderer failure --render-fault forces in --check-renderer-ladder.
enum class RenderFaultPoint : uint8_t {
    /// every render driver but software refuses at start-up
    create,
    /// a present fails, on a menu frame, a match frame or a loading frame;
    /// a texture cannot be made
    present,
    /// the device is reset, as the event dispatch, the loading pump, a drain
    /// of input or a movie's hook takes it
    reset,
    /// the device is lost, or says it is lost and is then reset
    lost,
    /// presents take over 2 s, on steady frames and on frames that are not
    stall,
    /// the floating-point settings change before a present
    float_state,
    /// the memory guard refuses the accelerated tier's buffers, then drops
    /// it; named alone, never part of the run of every case
    memory,
    /// a call only the Full tier makes fails on a Full match frame, which
    /// drops Full to Basic with a strike
    card,
    /// the memory guard refuses the Full tier's pages, then drops Full and
    /// then Basic; named alone, never part of the run of every case
    full_memory,
};

/// The failure --render-fault forces and when.
struct RenderFault {
    RenderFaultPoint point{};
    /// The presented frame of its case the failure comes at, from 1; unset
    /// for the case's own.
    std::optional<uint32_t> frame{};
};

enum class MatchCommand {
    none,
    move,
    attack,
    dgun,
    build,
    patrol,
    repair,
    reclaim,
    capture,
    load,
    unload,
    guard
};

/// Where the Game files screen's check goes (--game-files-route).
enum class GameFilesRoute : uint8_t {
    folder,        ///< CHOOSE FOLDER: a folder copied in (default)
    demo,          ///< CHOOSE INSTALLER: the demo's installer
    copy_yourself, ///< I HAVE COPIED IT: files the player copied into the game folder's parent
    manage,        ///< the management state over an installed folder
};

/// What the Game files screen's check expects (--game-files-expect).
enum class GameFilesExpect : uint8_t {
    main_menu,    ///< the route ends at the main menu (default)
    stopped_kept, ///< STOP, KEEP WHAT WAS COPIED, then quit (with --game-files-stop-after)
    resumed,      ///< the continue banner, the same folder chosen, files skipped, main menu
    not_a_game,   ///< the problem "This folder does not hold a Total Annihilation installation"
    short_space,  ///< the problem "Not enough space"
    next_start,   ///< the manage route's change applied by the next start's recovery
};

struct Options {
    // Empty until main() resolves it when --game-dir is not passed.
    fs::path game_dir;
    // Opens the folder dialog even when a usable folder is remembered.
    bool choose_game_dir = false;
    // The folder the dialog chose, which the runtime stores in the
    // preferences; empty when it came from elsewhere. It differs from
    // game_dir when it held the installer of the Total Annihilation demo
    // (1997), whose unpacked archive's folder is game_dir.
    fs::path remember_game_dir;
    /// The main menu's notice of where the game folder was found, shown once
    /// (found_install_notice, Runtime::tell_found_install); empty when the folder was not
    /// found on this machine.
    std::string found_install_notice{};
    // Where data the engine unpacks is kept (--data-dir); unset: the
    // platform's per-user data folder.
    std::optional<fs::path> data_dir;
    std::vector<fs::path> archives;
    // A mod profile (oamod.yaml) resolved instead of the game folder's
    // (--mod); empty for none.
    fs::path mod_file;
    // A mod folder layered over the game folder (--mod-dir); empty to play
    // the one the preferences remember, if any.
    fs::path mod_dir;
    // Plays the game folder without the remembered mod folder (--base-game).
    bool base_game = false;
    // The folders loose files come from, highest precedence first: the mod
    // folder, then game_dir. main() fills it; empty means game_dir alone.
    std::vector<fs::path> game_folders;
    // The mod profile the game plays, resolved from --mod or the folders'
    // own oamod.yaml before any archive is mounted: its limits size the
    // game's tables and its rules reach every match. Null for base 3.1c.
    std::shared_ptr<const oa::data::mod_profile::ModProfile> mod_profile;
    // Prints the resolved profile of --mod, or of the --game-dir folder,
    // with its hashes, then exits (--print-profile).
    bool print_profile = false;
    // Resolves a profile that turns on hacks this build does not implement
    // yet, warning about each instead of refusing it, for development
    // (--accept-unimplemented-hacks).
    bool accept_unimplemented_hacks = false;
    fs::path snapshot;
    std::optional<fs::path> preferences_file;
    // The player's own folder, which holds the saved games, screenshots,
    // films, recordings and mods (--user-folder); unset: the preferences'
    // key, else "Open Annihilation" in the Documents folder, or beside a
    // named --preferences-file.
    std::optional<fs::path> user_folder;
    // A folder the run's log is written into, whatever kind of run it is
    // (--log-dir): a program that cannot read the game's output, such as
    // Windows 95's command prompt, reads the file instead.
    std::optional<fs::path> log_dir;
    std::optional<std::size_t> frame_limit;
    std::optional<std::size_t> benchmark_frames;
    // Headless in-match run: ticks to simulate, output size, per-side army.
    std::optional<std::size_t> match_ticks;
    // Headless campaign run: the campaign's name and the mission index that
    // --headless-check starts through the briefing and ticks for match_ticks.
    std::string campaign;
    std::optional<std::size_t> campaign_mission;
    bool campaign_past_outcome = false;
    // Restarts the campaign mission from the exit menu at this tick and
    // checks the restart creates the units the first start did.
    std::optional<std::size_t> campaign_restart_tick;
    int match_width = 640;
    int match_height = 480;
    // --resolution was given: a run with a window opens it at match_width by
    // match_height instead of kDefaultWindowWidth by kDefaultWindowHeight.
    bool window_resolution = false;
    float match_zoom = kDefaultBattlefieldZoom;
    std::size_t combat_units = 0;
    // --busy-combat: the --combat armies also bring missile trucks, the
    // local player a kbot lab building peewees, an air transport loading a
    // peewee, and its army starts selected with selection boxes shown, so
    // that the run's frames draw every kind of battlefield draw.
    bool busy_combat = false;
    // --stage FILE: after the headless skirmish starts (and any --combat
    // armies), the actions the file lists, one a line: "unit PLAYER TYPE DX
    // DZ [FROM]" places a finished unit of a player DX, DZ map pixels from
    // where the first unit of player FROM (the owner by default) stood when
    // the stage began, "build TYPE DX DZ" queues the last unit placed to
    // build a type DX, DZ map pixels from it, "stockpile ROUNDS"
    // has the last unit placed build rounds for its first weapon, and
    // "console LINE" enters a chat line as the local player. "place PLAYER
    // TYPE X Z [FACING]" places a finished unit at a map pixel, "group NAME"
    // gathers the units placed after it, "move GROUP X Z", "patrol GROUP X Z",
    // "attack GROUP TARGETS", "attack-ground GROUP X Z" and "guard GROUP
    // GUARDED" order a group's live units, and "at TICK"
    // before any action runs it before that match tick instead
    // (src/app/README.md).
    fs::path stage_file;
    bool reclaim_check = false;
    // Headless camera placement: map pixel at the view's top-left.
    std::optional<std::pair<int, int>> camera;
    // Headless savegames: save once the tick reaches save_after (to save_file),
    // or start from load_file instead of a fresh skirmish.
    std::optional<std::size_t> save_after;
    fs::path save_file;
    fs::path load_file;
    // A fresh headless savegame run of a skirmish first gives a factory
    // queue, a building, a patrol, a guard and a move. A headless campaign
    // run gives the player's units orders towards the mission's victory, at
    // its start and every 300 of its ticks, and plays on through the end
    // screen and, after a victory, into the next mission; a savegame run of
    // a campaign mission gives those orders once, at its start. Two ticks
    // before the save, a savegame run also sets a feature burning, starts a
    // die and a reclamate sequence and clears a feature away.
    bool give_orders = false;
    bool skip_intro = false;
    bool headless_check = false;
    bool mute = false;
    bool check_navigation = false;
    // The part of the navigation check to run: a group name after
    // --check-navigation, all without one.
    NavigationGroup navigation_group = NavigationGroup::all;
    // Opens HELP.GUI over a live match through the SDL presenter.
    bool check_match_dialogs = false;
    // Opens LOADGAME.GUI from Single Player and, through the SDL presenter,
    // as the save and load dialogs over a paused match, checking where each
    // panel sits, its backdrop against the bitmap in the palette below and
    // the panel it darkens (<report dir>/native-loadsave-*.ppm).
    bool check_load_save = false;
    // Clicks every option of the skirmish, campaign and options setup
    // screens through the SDL presenter and checks each click changes the
    // setting and what the screen shows for it (<stem>-*.ppm beside
    // --snapshot).
    bool check_frontend_controls = false;
    // Drives the scroll bars of the options, the map and mission lists and
    // the match's preferences through the SDL presenter and checks what they draw and
    // set, and where the preferences' sub-panel shows in a window taller
    // than the chrome (<report dir>/native-scroll-bars-*.ppm).
    bool check_scroll_bars = false;
    // Opens the first mission's briefing through NEWGAME.GUI with sound on and
    // checks its narration plays exactly while SHUTUP is on and stops as the
    // briefing is left by its buttons or keys (<stem>-*.ppm beside
    // --snapshot).
    bool check_briefing_narration = false;
    // Checks the presented match frame against the CPU composition.
    bool check_match_layers = false;
    // Switches the accelerated presentation on over the main menu and a
    // skirmish, or the campaign mission --campaign and --mission name, and
    // checks what it presents against the processor's composition and the
    // card's references, at zooms and window sizes of its own; skips (exit
    // code 77) under 2 GiB of memory or on a renderer the probe finds not
    // capable (<report dir>/native-render-tiers-*.png).
    bool check_render_tiers = false;
    // Clicks the order page's standing order and toggle buttons through the
    // SDL presenter and checks what they show and what the units do.
    bool check_match_orders = false;
    // Gives a Kbot Lab a move through the SDL presenter and checks the unit it
    // builds carries it out.
    bool check_factory_orders = false;
    // Selects the commander and sends it to open ground through the SDL
    // presenter, in two skirmishes one after the other, and checks the
    // select and order lines it says.
    bool check_unit_speech = false;
    // Opens a download page through the SDL presenter and builds a download
    // unit from it.
    bool check_download_builds = false;
    // Selects every unit that stockpiles through the SDL presenter, queues
    // and removes rounds on its weapon page, builds a round and keeps it
    // through a save and a load.
    bool check_stockpile_builds = false;
    // Turns the commander's, a lab's and a PeeWee's order panels through the
    // SDL presenter and checks each unit keeps its page through reselection
    // and a save and a load, and that two selected show the general page.
    bool check_unit_page_memory = false;
    // Walks the commander's build pages on windows of several sizes through
    // the SDL presenter and checks each fits the side column.
    bool check_side_column = false;
    // Starts a skirmish on each of two sides and checks, on windows of
    // several shapes, that the top and bottom bars reach the window's right
    // edge with the side's art where the game places it.
    bool check_match_bars = false;
    // Opens the pages of the unit types named, as MODE:TYPE,TYPE,..., on
    // windows of several sizes through the SDL presenter and checks each is
    // drawn as its file places it, at the side column's one scale, and each
    // control lies inside the column and takes a click, as the radar and
    // the battlefield's edge do: "whole" requires the column to keep the
    // interface's width and scale, "scaled" requires it narrowed as a whole
    // so that the game's tallest unit page ends on the window's last row;
    // empty for no check.
    std::string check_unit_pages;
    // Presses F4 in a skirmish and checks the kills board at the top right.
    bool check_kill_board = false;
    // Pauses a skirmish with the Pause key, saves it from the in-game menu,
    // loads the save and checks the loaded game runs, and that Pause and the
    // in-game menu still hold it and let it go.
    bool check_paused_save = false;
    // Reads the simulation hash with no mod and under a mod, then after a
    // Developer Mode override that changes the simulation, without a new start.
    bool check_simulation_hash = false;
    // Starts a skirmish and checks, through the SDL presenter, that the
    // build menu, the bottom bar and the unit panel show units' names and
    // descriptions in the language this BCP-47 tag names, as the unit files
    // give them there; empty for no check.
    std::string check_unit_language;
    // Changes the language in the settings over the main menu and over a
    // skirmish's in-game menu, from Simplified Chinese to English, German
    // and back, and checks each screen under them shows it as it shows
    // opened again in it.
    bool check_language_switch = false;
    // Sends a construction kbot on PATROL through the SDL presenter with the
    // metal store low and checks it reclaims a feature on its way.
    bool check_patrol_reclaim = false;
    // Moves the pointer of a selected commander over trees, another
    // reclaimable feature and a wreck through the SDL presenter and checks
    // the cursor it shows.
    bool check_reclaim_cursor = false;
    // Places a tower under the build cursor in every tier and checks the
    // building drawn there (ui.build-preview): its pulse, the same at every
    // zoom and frame rate, and that it is drawn the right way up.
    bool check_build_preview = false;
    // Drives both interface types' pointer buttons through the SDL presenter:
    // clicks and right presses, shift cancels, radar scrolls, mouse look and a
    // factory build button's right click.
    bool check_pointer_interfaces = false;
    /// --check-megamap-clicks: the megamap's clicks (ui.megamap) against the
    /// battlefield's in both interface types, on a skirmish.
    bool check_megamap_clicks = false;
    /// --check-radar-orders: presses on the minimap (the radar) in both
    /// interface types, for every armed command and the default order, on a
    /// skirmish.
    bool check_radar_orders = false;
    /// --check-touch-controls: the touch controls, driven by finger events, on a skirmish.
    bool check_touch_controls = false;
    /// --check-pad-controls: the gamepad controls, driven by SDL virtual pads, on a skirmish.
    bool check_pad_controls = false;
    /// --check-running-while-inactive: the frame hook keeps being called while the
    /// window is inactive with the request held, and the loop waits once it is released.
    bool check_running_while_inactive = false;
    /// --touch-controls: touch controls on with no touch screen, to check their layout.
    bool touch_controls = false;
    /// --check-game-files: the Game files screen driven through a route by scripted hooks.
    bool check_game_files = false;
    /// --no-game-files-screen: with no usable game folder, the notice with its look-again
    /// button instead of the Game files screen, where the platform offers both.
    bool no_game_files_screen = false;
    /// --game-files-route folder|demo|copy-yourself|manage: the check's route.
    GameFilesRoute game_files_route{};
    /// --game-files-expect main-menu|stopped-kept|resumed|not-a-game|short-space|next-start:
    /// what the check expects the route to reach.
    GameFilesExpect game_files_expect{};
    /// --game-files-source PATH: the folder, or the installer, the check's picker answers.
    fs::path game_files_source{};
    /// --game-files-free-bytes N: the free space the check's hooks report.
    std::optional<uint64_t> game_files_free_bytes{};
    /// --game-files-copy-rate BYTES: the bytes a second the check's copy is held to.
    std::optional<uint64_t> game_files_copy_rate{};
    /// --game-files-stop-after BYTES: the check presses STOP once this much was copied.
    std::optional<uint64_t> game_files_stop_after{};
    // Clicks MULTI on the main menu through the SDL presenter and checks
    // what it reaches: network play's check of the multiplayer screens (the
    // check_multiplayer_menu hook); over game data with no multiplayer map,
    // the missing-content notice, closed by OK and by Enter.
    bool check_multiplayer_menu = false;
    // Plays the headless skirmish's first ticks in director mode, drawn and
    // again undrawn, and checks the two reach one world, that director frames
    // show the battlefield alone and move by a fraction of a map pixel, and
    // that the match's sounds reach the director's sound hooks. Implies
    // --headless-check and --skip-intro.
    bool check_director_view = false;
    // Renders a small director script over the headless skirmish's first
    // ticks, with encoding off, twice (all of it, then its second chunk
    // alone), and checks the files it writes, that neither the sound of a
    // chunk nor the world depends on the chunks drawn before it, and that
    // the render reaches the world the generator's undrawn replay of the
    // same ticks reaches. Implies --headless-check and --skip-intro.
    bool check_director_render = false;
    // Draws frames between ticks over the headless skirmish's fight and checks
    // that they show units, pieces and projectiles part of the way from one
    // tick to the next, that frames of a whole tick and the world are as
    // without them, and that units move evenly from frame to frame. Implies
    // --headless-check and --skip-intro.
    bool check_interpolation = false;
    // Draws the headless skirmish with the other player taken as another
    // machine's, whose records arrive in bursts, and checks that its units
    // move evenly on their playout, on whole ticks too, that the tracking
    // camera and the pointer's pick follow where they are drawn, and that
    // the local units, the world and, with no such player, every frame are
    // as without the playout. Implies --headless-check and --skip-intro.
    bool check_unit_playout = false;
    // Drives the Open Annihilation settings through the SDL presenter: the OA
    // button on the main menu and in the in-game menu, the dialog over each,
    // its sections, controls, keys, OK, Cancel and Restore defaults, the
    // preferences it writes, and each setting taking effect.
    bool check_engine_settings = false;
    // Drives the player's own folder through the SDL presenter: the saved
    // games moved once from beside the preferences file, the main menu's
    // notice of the move shown once and closed, and the settings' Your files
    // buttons opening its folders through a recorded opener.
    bool check_user_folder = false;
    // Switches the mod ten times through the settings' Mods page, between
    // No Mod and two test profiles in the player's own Mods folder, each a
    // soft restart on the same window, and requires the working set to
    // stay level.
    bool check_mod_switch = false;
    // Switches to a made-up mod whose unit files are missing and to one
    // that plays whole, each a soft restart, and requires the first's
    // warning once over the main menu and over a refused Skirmish start,
    // and none for the second.
    bool check_mod_warning = false;
    // --install-mod FILE, repeated, and the bare arguments that name a
    // .oamod file, any case: mod packages to install once the main menu
    // shows, each asked about first; absolute, in the order given.
    std::vector<fs::path> install_mods;
    // Installs made-up mod packages through each of the main menu's
    // questions, by a dropped file and the command line, rolls a mod back on
    // the Mods page, and replaces and rolls back the mod played across soft
    // restarts, checking the player's Mods folder after each.
    bool check_mod_install = false;
    // The soft restarts the process has made before this run: 0 for the
    // first; each switch of the mod from the settings adds one.
    uint32_t restarts = 0;
    // Forces each renderer failure the game handles while it runs, on the
    // renderer the start made, and checks that it goes on presenting: the
    // walk of the render drivers, a present error on a menu, match or
    // loading frame, a device reset or loss, present stalls, a device that
    // waits to be reset, a changed floating-point setting, and textures
    // beyond the renderer's limit.
    bool check_renderer_ladder = false;
    // --render-fault POINT[@FRAME]: --check-renderer-ladder forces only that
    // failure, at that presented frame of its case; unset for every case.
    std::optional<RenderFault> render_fault;
    // --generate-script RECORDING: the recording a director script is
    // generated from; empty for none. Implies --headless-check and
    // --skip-intro.
    fs::path generate_script;
    // --render-script FILE: the director script (.oascript) or bundle
    // (.oamovie) to render; empty for none. Implies --headless-check and
    // --skip-intro.
    fs::path render_script;
    // --output PATH: where --generate-script writes (a .oascript or
    // .oamovie) or --render-script renders (a directory); empty for the
    // default beside the input.
    fs::path director_output;
    // --chunks A-B (or A): the chunks --render-script draws and encodes,
    // counted from 0, both included; unset for all.
    std::optional<std::pair<uint32_t, uint32_t>> director_chunks;
    // --stills F[,F...]: the frames --render-script also writes as lossless
    // pictures (director_output.hpp), counted from 0, in increasing order;
    // empty for none.
    std::vector<uint64_t> director_stills;
    bool trace_input = false;
    // Writes every game file and listing the run looks up, one a line, to
    // this file (--trace-lookups); empty for none.
    fs::path trace_lookups;
    bool debug_order_lines = false;
    // The most frames a second the application loop draws (--max-fps); 0
    // for no limit. While nothing moves on its own and no input comes, the
    // loop draws fewer (frame_pacing.hpp).
    uint32_t max_frames_per_second = kDefaultMaxFramesPerSecond;
    // --max-fps was given: its rate holds for the run whatever the settings
    // say, and is never saved.
    bool max_frames_per_second_given = false;
    // --hardware-acceleration=off|basic|full, --hardware-acceleration (Full)
    // or --no-hardware-acceleration (Off): decides the Hardware acceleration
    // setting for the run whatever the settings say, and is never saved;
    // empty when no flag was given.
    std::optional<oa::ui::engine_settings::HardwareAcceleration> hardware_acceleration;
    // --force-capable, which only --check-render-tiers, --check-build-preview,
    // --check-engine-settings and --check-kill-board take: the renderer
    // counts as one the graphics card could scale the frames on, so that the
    // accelerated tier runs on SDL's software renderer, and neither the
    // environment's render driver nor SDL's software renderer locks Hardware
    // acceleration or Vertical sync. It never lifts the 2 GiB rule.
    bool force_capable = false;
    // --display-modes MODES, which only unattended runs take: a made-up
    // monitor in place of the display's own report, for checks on SDL's
    // dummy video driver, whose display reports no modes. MODES lists them
    // separated by commas, each WIDTHxHEIGHT, optionally followed by @RATE
    // and /DENSITY, the first also the desktop's, or is "none" for a monitor
    // that reports nothing (oa::platform::display_modes::report_from_text).
    // Empty for the display's own.
    std::string display_modes;
    // --native-density: for this run the window opens at the display's own
    // pixel density whatever the Native pixel density setting and the rule
    // for it say (render_policy::decide_native_density), except under 2 GiB
    // of memory or with a flag that names Off; it is never saved. With
    // --check-render-tiers the check runs its density case.
    bool native_density = false;
    // The platform the game is built for opens every window at the
    // display's own pixel density (the OA_NATIVE_DENSITY_WINDOWS build
    // option, which main sets before the run starts); false on the desktop.
    // The Native pixel density setting is then always on.
    bool native_density_windows = false;
    // --frame-rate FPS: the headless match run (--match-ticks) draws every
    // frame of a loop running at FPS frames a second on a clock that
    // advances a frame at a time, each frame between two ticks as the
    // application loop draws it; unset for a tick at a time.
    std::optional<uint32_t> frame_rate;
    // --frame-log FILE: that run writes one line a frame: the time, the
    // tick, the fraction of a tick shown, the camera and a unit it follows.
    fs::path frame_log;
    // --scroll-camera: that run holds the camera's scroll, as the arrow keys
    // would, sweeping it right and back over the army it starts on.
    bool scroll_camera = false;
    // --march: that run orders the local player's army (--combat) to march
    // south at its start.
    bool march = false;
    // --follow: that run's camera tracks the unit its frame log follows, as
    // the T key does.
    bool follow = false;
    // --frame-clock MS: that run's clock starts MS milliseconds into the
    // steady clock instead of at 0, so that a run can cross the moment the
    // match clock's reading turns over (2^32 milliseconds); unset for 0.
    std::optional<uint64_t> frame_clock_ms;
    // Set by the checks above: no wall-clock input reaches the match or the frame.
    bool fixed_clock = false;
    // Scripted runs (fixed clock, navigation and menu checks, benchmarks,
    // frame limits, snapshots, and the extension's): nobody is there to answer
    // a dialog.
    bool unattended = false;
    // An extension's option lets a program on this machine control the run
    // (option_effect::remote_controlled): the main loop runs every frame
    // while the window is inactive, and the battle room says the run can be
    // remote-controlled.
    bool remote_controlled = false;
    // On Windows, a player's run without the -d switch opens its window full
    // screen; unattended runs and video captures keep a window.
    bool start_full_screen = false;
    // Trace stream (oa/sim/match_runtime/match_trace.hpp) of each match the run starts,
    // and its optional per-slot unit dump; a new match rewrites both files.
    fs::path trace_digest;
    fs::path trace_units;
    // Argument for the generator seeder in place of mission start's
    // performance-counter sum, so two runs start from the same state.
    std::optional<uint32_t> seed;
    // Threads the per-row drawing passes run on, the drawing thread
    // included (--draw-threads N, else the OA_DRAW_THREADS environment
    // variable); unset for the job pool's default for the machine. Every
    // count draws the same frames.
    std::optional<uint32_t> draw_threads;
    // The MP4 a video capture of the run makes (video_capture.hpp); empty
    // for none.
    fs::path capture_video;
    // The scripted showcase the run plays in place of a player.
    Showcase showcase = Showcase::none;
    // The game switches: every argument that is not one of the options above.
    oa::app::command_line::Switches launch{};
};

struct Extension;

/// Parses the command line into the run options.
///
/// Long options the engine knows set their fields; any other long option goes
/// to `extension`, and every other argument joins the game switch line, which
/// fills Options::launch. --help and a bare -h print the usage and exit. After
/// the loop the combinations are checked, OA_DEBUG_ORDER_LINES is read,
/// OA_DRAW_THREADS stands in for an absent --draw-threads, and fixed_clock
/// and unattended follow from the checks and runs asked for.
/// Throws std::runtime_error on an unknown option, a missing or malformed
/// value, a refused or over-long switch line, or options that cannot be used
/// together.
///
/// @param argc argument count, the program name included
/// @param argv arguments; argv[0] is skipped
/// @param extension extension that takes the long options, switches and usage
///     text the engine does not know
/// @return the parsed options
[[nodiscard]] Options parse_options(int argc, char** argv, const Extension& extension);

/// Returns the level of hardware acceleration the run asks for: a flag
/// decides, else the Hardware acceleration setting does.
///
/// @param options the parsed command line (Options::hardware_acceleration)
/// @param setting the setting in effect
/// @return Off, Basic or Full
[[nodiscard]] oa::ui::engine_settings::HardwareAcceleration hardware_acceleration_asked(
    const Options& options, oa::ui::engine_settings::HardwareAcceleration setting
) noexcept;

/// Parses a frame or tick count option's value.
///
/// Throws std::runtime_error unless the whole text is a decimal integer from 0
/// through 10'000'000.
///
/// @param text option value
/// @return the count
[[nodiscard]] std::size_t parse_count(std::string_view text);

/// Writes an RGB surface as a binary PPM (P6) file.
///
/// Throws std::runtime_error when the file cannot be created or written.
///
/// @param path file to create or truncate
/// @param surface frame to write, 3 bytes per pixel
void write_ppm(const fs::path& path, const renderer::Surface& surface);

// A rectangle of a composed frame, in canvas pixels, that a headless check
// compares between renders.
struct CanvasRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// Pixels a typed or posted line must change in the frame.
constexpr std::size_t kTextMinPixels = 40;

/// Copies the RGB bytes of a rectangle of a frame, clipped to the frame, row by row.
///
/// @param frame composed RGB frame
/// @param rect canvas rectangle; parts outside the frame are skipped
/// @return 3 bytes per copied pixel, rows top to bottom
[[nodiscard]] std::vector<uint8_t> copy_rect(const renderer::Surface& frame, CanvasRect rect);

/// Counts the pixels whose colour differs between two copies of the same rectangle.
///
/// @param before earlier copy_rect() result
/// @param after later copy_rect() result of the same rectangle
/// @return differing pixels, over the shorter of the two copies
[[nodiscard]] std::size_t
changed_pixels(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after);

} // namespace oa::app
