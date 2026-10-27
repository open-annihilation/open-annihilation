// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-game's extension table: the hooks through which libraries linked into
// oa-game add launch options and switches, screens, per-frame work, match
// events and checks. Each such library is an extension: the project that
// builds the game registers it (oa_add_extension, cmake/OaExtensions.cmake)
// with its init function, void <init>(oa::app::Extension* table), which
// fills a zeroed table of its own. main() calls every registered init once,
// before the command line is parsed, and combines the tables into the one
// the engine calls (ExtensionList, extension_list.hpp). A library that
// includes runtime.hpp builds against oa::extension-sdk. Network play is
// one of them: the engine registers it in every build (oa-app-netgame,
// src/app/netgame), and other extensions build on it or beside it. Every
// hook no extension fills keeps the engine's own behaviour.
//
// The extensions are listed in dependency order: one whose library links
// another registered extension comes after it, and the rest keep the order
// they were registered in. An extension later in the list builds on those
// before it. When several extensions fill a hook, the combined table calls
// them by these rules:
// - every extension, in list order: check_options, startup,
//   register_screens, register_settings_pages, ready,
//   check_multiplayer_menu, frame, match_game,
//   match_event, check_console, draw_loading, draw_match_hud,
//   draw_match_overlay, pause_changed, load_progress, speed_changed and
//   app_mode_set; also message_hooks, console_host and team_panel_host,
//   whose entries are single owners (below);
// - every extension, in reverse list order: shutdown and release_runtime;
// - the first that takes it, asking the last extension in the list first:
//   take_option (the option goes to the first that takes it, with that
//   extension's effects), the switch handlers (a letter goes to the first
//   handler that takes it; every handler's reset runs), run_mode (for each
//   phase), start_scene, simulation_step, give_resources, player_gone,
//   close_requested, open_recording (each extension asked is given replay
//   and info all zero, and only the one that takes the recording fills the
//   caller's) and select_multiplayer (the first answer other than
//   unavailable);
// - the first answer, asking the last extension in the list first: the
//   first that is not null for disconnect_text and for text's usage_note
//   and register_switch (an empty text is an answer), and the first that
//   is neither null nor empty for return_label and for each field of
//   frontend_entry;
// - joined in list order: text's usage_checks, usage_runs and
//   usage_switches;
// - every extension asked, the answers combined: state (their bits OR'd),
//   keep_stored_password (true when any answers true) and outcome_ready
//   (true when every one answers true);
// - at most one extension: frontend_game and frontend_states. A second
//   extension that fills either stops the start, before the command line is
//   parsed, with a message that names both. Each entry of the hosts that
//   message_hooks, console_host and team_panel_host fill is likewise one
//   extension's: an extension that changes an entry another extension has
//   set, in that call or an earlier one, stops the call with an error
//   that names both, which is the hook's error (below).
// With one extension the combined table behaves as that extension's own.
//
// A check an extension runs from run_mode or check_multiplayer_menu drives
// the running game through the check host (check_host.hpp), which is not
// part of this table.
//
// The engine calls every hook on the thread that runs main(), and passes
// Extension::context back unchanged; the extension owns it and keeps it
// valid until the process exits. Pointers and references a hook receives
// are valid for that call only unless its documentation says otherwise.
//
// Hooks report bad input by throwing std::runtime_error, as the engine code
// around them does. The engine calls every hook, and every entry of the
// switch handler and the replay a hook returns, through one guarded call
// (call_hook, hook_call.hpp), which catches what the hook throws, whatever
// its type, before it reaches engine code. What follows is one of three
// handlings, and each hook says which is its own:
// - raised: the engine raises the hook's message as its own error at the
//   call, before it uses anything the hook answered, and the error takes
//   the path the engine's own errors take from that call. Mostly that ends
//   the game: main() prints "open-annihilation: <message>" and exits with
//   status 1. One kind of path catches it, a match start the frontend falls
//   back from: a campaign mission's start, wherever it comes from, a
//   skirmish started from its setup's Start, a saved game loaded from the
//   load dialog, and the in-game restart. The start is abandoned where it
//   stopped and the frontend runs on; the status line shows "campaign
//   start: <message>", "Skirmish start failed: <message>", "Saved game
//   start: <message>" or "Restart failed: <message>", which stderr also
//   receives. A failed skirmish start leaves no match and returns to the
//   skirmish setup, which shows the message in a message box, or the
//   warning that the mod's files are missing when they are; a failed
//   restart returns to the main menu. Other starts (a skirmish a headless
//   run starts, a saved skirmish a --load run loads) are not caught.
//   frontend_game, state, match_game, console_host, team_panel_host and
//   return_label are reached there;
// - reported: the engine reports the message and carries on as the hook
//   says, mostly as it does for that call when the hook is null:
//   simulation_step, player_gone, message_hooks, match_event, draw_loading,
//   draw_match_hud, draw_match_overlay, pause_changed and load_progress.
//   Unless the hook says otherwise the report is one line on standard
//   error (the game's log when it plays), "open-annihilation: extension
//   hook <hook>: <message>"; the same line again is printed only when its
//   count reaches a power of two, with the count;
// - must not throw: speed_changed, app_mode_set, release_runtime and the
//   hooks of ReplayHooks. One that throws all the same is reported as a reported
//   hook is, and the engine carries on as it does when the hook is null.
// Every other hook's error is raised. When several extensions fill a hook,
// those after the one that threw are not called for that call.
// docs/development/conventions.md states this rule for the extension table
// beside the rules of each layer.
#pragma once

#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/screen_registry/kit.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The version of the extension table's contract an extension is built
/// against. An extension checks oa::app::extension_api_version, its typed
/// copy below, with static_assert; the macro serves an extension that builds
/// against more than one engine and tests the version with #if.
///
/// Raise it by one in the change that alters the contract: a hook added,
/// removed or renamed; a hook's parameters, return value or the meaning of
/// leaving it null; when or in which order the engine calls it; what it may
/// keep or must free; its error behaviour; a type, enumerator or bit the
/// table uses; or a function an extension calls to read a value the engine
/// resolved. A change to wording alone keeps it. A change to the OA UI kit's
/// public headers follows the rule version 15 states below. Version 2 is the first
/// with the layered layout's names: this header is oa/app/extension.hpp and
/// the types the table names are in their modules' namespaces. Version 3
/// replaces offers_multiplayer with select_multiplayer, which MULTI calls
/// and through which the extension may take the game over. Version 4 gives
/// ConsoleHost::post_message the line's sender, calls console_host as each
/// match starts, so that the console's host is filled before the match's
/// first tick, and adds draw_match_overlay, which draws over the
/// battlefield. Version 5 adds pause_changed, load_progress and
/// team_panel_host; no simulation step is offered while the running match's
/// pause bit is set, whoever set it; a shared match's clock keeps running
/// while its in-game menu, or the preferences that menu opens, are up
/// (Runtime::match_running) and while outcome_ready holds it on its
/// outcome. Version 6 adds close_requested and the hook that named the
/// match's return, FrontendEntry::nickname, TeamPanelHost::tournament_game,
/// the ScreenServices entries quit, stop_sounds, play_sound_alternate and
/// run_frontend, and query_register; the hook that told whether the stored
/// password is kept is also asked as each match starts, and a
/// ScreenContext's host and services may be kept while the runtime lives.
/// Version 7 adds speed_changed, which the speed keys and the GAME slider
/// call, and app_mode_set, which every application mode the frontend sets
/// calls. Version 8 lets several extensions fill tables of their own, each
/// through the init function its registration names in place of the one
/// global init, and combines their hooks by the rules above; replaces those
/// two version 6 hooks with return_label, whose label alone now decides
/// what the match's menus show, and keep_stored_password, which only the
/// preferences write asks; and refuses a reserved game switch no extension
/// takes with "-<switch> is not handled by this build". Version 9 adds
/// open_recording, which hands an extension a recording's bytes to replay
/// into a match the engine steps one tick at a time, with the types
/// RecordingInput, RecordingInfo, RecordingStatus and ReplayHooks. Version
/// 10 calls every hook through a guarded call that catches what it throws:
/// the errors of simulation_step, player_gone, message_hooks, match_event,
/// draw_loading, draw_match_hud, draw_match_overlay, pause_changed and
/// load_progress are reported and the engine carries on as for a null hook;
/// the rest are raised as the engine's own errors at the call; a switch
/// handler entry that throws stops the start with its message; and the
/// hooks that must not throw, now the ReplayHooks' too, are reported when
/// they do. Version 11 adds release_runtime, through which an extension
/// frees what it keeps for a runtime as that runtime is destroyed. Version
/// 12 adds option_effect::remote_controlled, the effect of an option that
/// lets a program on this machine control the run through the extension:
/// the engine then keeps the main loop running every frame while the window
/// is inactive, and Options::remote_controlled says so. Version 13 adds
/// FrameStage::presented: the frame hook is called a third time each frame,
/// after the frame is drawn and shown. Version 14 adds functions an
/// extension calls. None is a hook: the engine does not call them, and no
/// extension fills them. player_folder(const Runtime&) reads the player's
/// own folder for the run. set_unit_limit(Runtime&, int32_t) sets the unit
/// limit preference, the configured limit a multiplayer game uses.
/// keep_running_while_inactive(Runtime&, bool) holds or releases a request
/// that the main loop keep running every frame while the window is inactive:
/// held, the frame hook keeps being called there; released, the loop waits
/// for an event, as it does otherwise. web_address_available() and
/// open_web_address(const char*) ask whether this machine can open a web
/// address in the system's browser and open an http or https address there;
/// on a Steam Deck in Game Mode the answer is no. modern_text_chain,
/// modern_text_layout, modern_text_pixels, message_log_text_size,
/// game_text_preferences_of, game_text_preferences and
/// set_focused_text_field read the modern text faces and their fallback
/// chain at a pixel size, lay a line out and draw it, take the message log's
/// face at the player's Text size, read the player's "Use modern fonts for
/// game text" and "Text size" settings, and tell the engine where the
/// focused text field is, for the input method's candidate window and the
/// on-screen keyboard. read_game_file reads one whole file from the game's
/// files: a loose file wins over a file of the same path in an archive, a
/// mod's archive among them, and a missing file returns false without
/// throwing. set_extension_window_source shows the extension's windows and
/// controls to a program on this machine that drives the game, and the
/// engine calls that source when the program asks for the screen's controls.
/// extension_clock is the clock the extension's idle work follows: in a run
/// that program controls it advances by the fixed clock's step once a frame
/// and does not read the wall clock, and while the fixed clock is on it is
/// that clock. simulation_hash reads the simulation hash the run plays
/// under: the profile in play, with every Developer Mode override that
/// changes the simulation, or the plain baseline when the game plays 3.1c,
/// folded with the running match's mutator digest when that match applied
/// mutators. With no mutators applied it is that profile hash alone. A
/// running match keeps the hash it started with. Version 15 adds the OA UI
/// kit to the contract: register_settings_pages; kit screens in
/// ScreenRegistry and open_kit_screen, show_notice and ask_prompt in
/// ScreenServices; and the kit's text, chips, progress bars and hover card in
/// MatchOverlay. From this version the kit's public headers, oa/ui/kit/*.hpp,
/// are part of the contract. Changes to them before the first release that
/// carries version 15 (0.8.0) keep 15; from that release on, any change to
/// them raises the version, while the kit is provisional too (it changes as
/// the engine's screens need). ScreenServices::read_string also reads a whole
/// preference key, as a Settings page's row binds one, when its section is
/// null. Version 16 adds run_mutators, which reads how many mutators the run
/// plays, their ids in order, and the simulation hash the run would have
/// without them. It is not a hook: the engine does not call it, and no
/// extension fills it. Version 17 adds hear_remote_seat_chat, which hands a
/// chat line from another machine to the computer seats on this machine
/// that the line reached. It is not a hook: the engine does not call it,
/// and no extension fills it. Version 18 adds the kit's keycaps, chord
/// chips, keyboard and mouse pictures, key tints and marks, and the hold
/// ring. It is not a hook: the engine does not call it, and no extension
/// fills it. Version 19 is Kit 2's look in the kit's headers: Metrics gains
/// the type, spacing and shape scales and Kit 2's control sizes, the theme
/// its new tokens, and the kit a danger button, information and brand
/// pills, a paused progress bar, a main-block keyboard picture, a header
/// height, and a question's callouts, line boxes and quiet button with the
/// update pill; and the Modern look's faces (kit::Fonts' modern member, the
/// text functions that choose a face, the text-font Style's family and two
/// weights). It is not a hook.
#define OA_EXTENSION_API_VERSION 19

namespace oa {
struct Game;
struct Player;
struct Surface;
struct World;
} // namespace oa

namespace oa::app::command_line {
struct SwitchHandler;
}

namespace oa::ui::frontend_state {
struct StateHandler;
}

namespace oa::present {
struct GafSprites;
}

namespace oa::sim::messages {
struct Hooks;
}

namespace oa::ui::console {
struct ConsoleHost;
}

namespace oa::ui::hud {
struct TeamPanelHost;
}

namespace oa::app {

// OA_EXTENSION_API_VERSION, typed.
inline constexpr uint32_t extension_api_version = OA_EXTENSION_API_VERSION;

class Runtime;
struct ScreenRegistry;

// The arguments that follow a long option.
struct OptionValues {
    void* arguments{};
    /// Takes the next argument of the command line.
    ///
    /// @param arguments OptionValues::arguments
    /// @return the argument, which lives as long as the process; throws
    ///         "<option> requires a value" when none is left or it is empty
    const char* (*next)(void* arguments){};
    /// Names the next argument without taking it, so an extension can read
    /// a value another extension takes.
    ///
    /// @param arguments OptionValues::arguments
    /// @return the argument, which lives as long as the process; null when
    ///         none is left. Empty when the argument is empty
    const char* (*following)(void* arguments){};
};

// Engine options an extension option implies (Extension::take_option).
namespace option_effect {
inline constexpr uint32_t headless_check = 1;
inline constexpr uint32_t skip_intro = 2;
inline constexpr uint32_t unattended = 4; // a scripted run: nobody answers a dialog
// A program on this machine may control the run through the extension,
// which serves it from its frame hook: the main loop runs every frame while
// the window is inactive, as it does for a live multiplayer game.
inline constexpr uint32_t remote_controlled = 8;
} // namespace option_effect

// Text an extension may word in place of the engine's (Extension::text).
enum class ExtensionText : uint8_t {
    usage_checks,    // --help: its check options, after the engine's
    usage_runs,      // --help: its run options, after the engine's
    usage_switches,  // --help: the game switches it takes, ahead of "-s"
    usage_note,      // --help: the line after the usage ("" for none)
    register_switch, // why -r stops the start
};

// Where Runtime::run offers the extension a run of its own.
enum class RunPhase : uint8_t {
    start,          // before anything else
    headless_first, // first of the headless runs, ahead of --check-navigation
    headless,       // after --check-navigation, ahead of the engine's headless runs
};

// What MULTI on the main menu does (Extension::select_multiplayer).
enum class MultiplayerSelection : uint8_t {
    unavailable, // no answer: the next extension is asked; with none, MULTI does nothing
    frontend,    // the main menu's own MULTI step into the frontend's multiplayer states
    taken,       // the extension has taken the game over; the engine does nothing more
};

// The three points of each frame where the extension works.
enum class FrameStage : uint8_t {
    pump,       // charged to the frame profile's pump bucket
    after_pump, // after that bucket closes, before the match clock runs
    presented,  // after the frame is drawn and shown in the window
};

// Extension::state bits; all clear without an extension.
namespace extension_state {
inline constexpr uint32_t multiplayer = 1;   // a multiplayer session is open
inline constexpr uint32_t shared_match = 2;  // the running match is played with other machines
inline constexpr uint32_t replay = 4;        // the running match replays a recording
inline constexpr uint32_t local_watcher = 8; // the local player only watches the shared match
} // namespace extension_state

// What happened to a match (Extension::match_event).
enum class MatchEvent : uint8_t {
    finished,         // the finished match moves to the end-of-game screen
    torn_down,        // the match, if any, is about to be destroyed
    left,             // the player leaves the running match
    results_reported, // the end-of-game screen reports the game's end
    results_released, // the end-of-game screen lets the finished match go
    watching_kept,    // a defeated local player chose to keep watching
};

// The frontend's launch values (Extension::frontend_entry).
struct FrontendEntry {
    const char* game_name{}; // preferred to the stored game name; null for none
    const char* nickname{};  // preferred to the stored nickname; null or empty for none
};

// The fonts MatchOverlay draws text in.
enum class OverlayFont : uint8_t {
    side_panel,  // the font of the side panel's readouts (SIDEDATA.TDF's font)
    message_log, // the font of the message log over the battlefield
};

// The battlefield of a drawn match frame and a painter over it
// (Extension::draw_match_overlay). Positions and sizes are in the painter's
// pixels; `scale` of them make up one pixel of the game's 640x480 screen:
// text is drawn `scale` times larger, and a readout the game draws n pixels
// from an edge of the battlefield belongs n times `scale` from that edge
// here. The OA UI kit's components (kit_text_width to draw_hover_card) are
// drawn at `scale`, one point of the kit to `scale` pixels, in the game's
// fonts, as the kit draws OA's own screens at a whole scale.
struct MatchOverlay {
    void* painter{};        // passed back to every call below
    const oa::Game* game{}; // the drawn match's Game block
    int left{};             // the battlefield's left edge
    int top{};              // the battlefield's top edge
    int bottom{};           // the battlefield's bottom edge, where the bottom bar starts
    int scale{};            // painter pixels to one 640x480 pixel; at least 1
    /// Returns a font's height: the step from one line of it to the next,
    /// as the font's own header gives it.
    ///
    /// @param painter MatchOverlay::painter
    /// @param font the font
    /// @return the height in 640x480 pixels; 0 when the font is not loaded
    uint8_t (*font_height)(void* painter, OverlayFont font){};
    /// Draws a line of text in one palette colour, clipped to the battlefield.
    ///
    /// Nothing is drawn when the font is not loaded or the colour is outside
    /// the palette.
    ///
    /// @param painter MatchOverlay::painter
    /// @param font the font to draw in
    /// @param x the text's left edge
    /// @param y its pen row; the glyphs start the font's lift above it, as
    ///        the game draws every line of text
    /// @param text the text; read at once
    /// @param palette_index the colour, a palette index
    void (*draw_text)(
        void* painter, OverlayFont font, int x, int y, const char* text, uint8_t palette_index
    ){};
    /// Fills a rectangle in one palette colour, clipped to the battlefield.
    ///
    /// @param painter MatchOverlay::painter
    /// @param x the rectangle's left edge
    /// @param y its top edge
    /// @param width its width; 0 or less fills nothing
    /// @param height its height; 0 or less fills nothing
    /// @param palette_index the colour, a palette index
    void (*fill_rect)(void* painter, int x, int y, int width, int height, uint8_t palette_index){};
    /// Returns a line of kit text's width, as draw_kit_text draws it.
    ///
    /// @param painter MatchOverlay::painter
    /// @param font the kit's font
    /// @param text the text, UTF-8; read at once; null is empty
    /// @return the width: the kit's width in points (kit::text_width) times
    ///         `scale`; 0 while the kit's fonts are not loaded
    int (*kit_text_width)(void* painter, oa::ui::kit::FontRole font, const char* text){};
    /// Draws a line of kit text in the kit's text colour
    /// (kit::colour::text), clipped to the battlefield.
    ///
    /// Nothing is drawn while the kit's fonts are not loaded.
    ///
    /// @param painter MatchOverlay::painter
    /// @param font the kit's font
    /// @param x the line's left edge
    /// @param y its top edge; the line is the font's line high (the kit's
    ///        small_line or regular_line points, times `scale`), its capitals
    ///        centred down it
    /// @param text the text, UTF-8; read at once; null draws nothing
    void (*draw_kit_text)(
        void* painter, oa::ui::kit::FontRole font, int x, int y, const char* text
    ){};
    /// Draws a kit chip (kit::draw_chip), clipped to the battlefield: as wide
    /// as kit::chip_width and kit::compact_metrics.chip_height high, in
    /// points, times `scale`.
    ///
    /// @param painter MatchOverlay::painter
    /// @param x the chip's left edge
    /// @param y its top edge
    /// @param chip its text and state; read at once; null draws nothing
    void (*draw_chip)(void* painter, int x, int y, const oa::ui::kit::ChipLook* chip){};
    /// Draws a kit progress bar (kit::draw_progress), clipped to the
    /// battlefield: a well in a border, filled in the accent from its left as
    /// far as it has come, or, busy, striped across with no fill.
    ///
    /// @param painter MatchOverlay::painter
    /// @param x the bar's left edge
    /// @param y its top edge
    /// @param width its width, rounded down to whole points (`scale` pixels
    ///        each); less than one point draws nothing
    /// @param height its height, rounded down likewise
    /// @param thousandths how far it has come, from 0 to 1000 (held to that
    ///        range), or -1 to draw it busy
    void (*draw_progress)(
        void* painter, int x, int y, int width, int height, int32_t thousandths
    ){};
    /// Draws a kit hover card (kit::draw_hover_card), clipped to the
    /// battlefield: kit::hover_card_size in points, times `scale`, its arrow
    /// outside that rectangle.
    ///
    /// @param painter MatchOverlay::painter
    /// @param x the card's left edge, its arrow not counted
    /// @param y its top edge, its arrow not counted
    /// @param card what it shows; read at once; null draws nothing
    void (*draw_hover_card)(void* painter, int x, int y, const oa::ui::kit::HoverCardLook* card){};
};

// A recording the engine asks an extension to replay: a file a director
// script names, handed over as bytes so that a bundle's entries are never
// written out.
struct RecordingInput {
    const char* name{};     // the recording's file name as the script gives it, UTF-8; read at once
    const uint8_t* bytes{}; // the recording's contents; valid for the call only
    size_t byte_count{};    // bytes at `bytes`
    bool strict{};          // refuse a recording this installation cannot replay exactly
};

// What an extension says about a recording it opened.
struct RecordingInfo {
    uint32_t expected_end_tick{}; // the tick after the recording's last; 0 when not known
    uint64_t duration_ms{};       // the recording's length by its own clock; 0 when not known
    uint8_t viewer_player{};      // the player index of the slot the replay is watched from
    uint8_t player_count{};       // players the recording holds, the viewer's slot not counted
    bool content_differs{};       // the installation's unit definitions differ from the recording's
};

// Where a replay stands (ReplayHooks::status).
struct RecordingStatus {
    uint32_t tick{};          // the running match's game tick
    bool finished{};          // everything recorded has been replayed
    bool clean{};             // no error so far: every record applied and no tick failed
    bool paced{};             // the recording's periodic records came at the spacing it states
    uint32_t errors{};        // errors so far: records refused or failed, and ticks that failed
    const char* last_error{}; // the last error's text, or null; valid until the next call
};

// A recording an extension replays into the running match, which the engine
// steps one tick at a time. The extension fills every member in
// open_recording, so none is null once it returns true, and keeps what its
// context names until close.
struct ReplayHooks {
    void* context{}; // passed back to step, status and close; owned by the extension
    /// Runs the running match's next tick from the recording.
    ///
    /// A failed tick or a record that cannot be applied does not throw: it
    /// counts in RecordingStatus::errors and the replay goes on. It must not
    /// throw; one that throws is reported (file header) and counts as a
    /// replay that can go no further.
    ///
    /// @param context ReplayHooks::context
    /// @return true when a tick ran; false when the replay can go no further
    bool (*step)(void* context){};
    /// Tells where the replay stands; it must not change the runtime. It
    /// must not throw; one that throws is reported (file header) and leaves
    /// the status all zero.
    ///
    /// @param context ReplayHooks::context
    /// @param[out] status the replay's state, all zero and false on entry
    void (*status)(void* context, RecordingStatus& status){};
    /// Ends the replay and frees what the extension kept for it. The match
    /// stays the engine's to tear down. Called once; the hooks are not used
    /// again. It must not throw; one that throws is reported (file header).
    ///
    /// @param context ReplayHooks::context
    void (*close)(void* context){};
};

struct Extension {
    // Passed back to every hook; owned by the extension.
    void* context{};

    /// Takes a long option the engine does not know.
    ///
    /// Called while the command line is parsed, before the game directory
    /// is looked up, once for each argument that starts with "--" and is not
    /// the engine's, in command-line order, until an extension takes it. An
    /// extension that does not take the option leaves its values untaken. An
    /// option no extension takes stops the start with "unknown option: <name>".
    ///
    /// @param context Extension::context
    /// @param name the option as given ("--name"); lives as long as the process
    /// @param values takes the arguments that follow the option; valid for this call
    /// @param[out] effects option_effect bits the option implies; 0 on entry
    /// @return true when the extension took the option; false when it is not
    ///         the extension's
    bool (*take_option)(
        void* context, const char* name, const OptionValues& values, uint32_t& effects
    ){};

    /// Checks the options take_option took, as a whole.
    ///
    /// Called once, when every argument and game switch is parsed and the
    /// engine has checked which of its options go together (all but
    /// --choose-game-dir's checks, which follow), before the game directory
    /// is looked up. Not called when --help, a refused option or switch, or
    /// options that do not go together end the parse first. Throws to
    /// refuse the options.
    ///
    /// @param context Extension::context
    void (*check_options)(void* context){};

    /// Returns the handler of the game switches the extension takes.
    ///
    /// Called once, after the long options, while the game switches are
    /// parsed. Each letter the engine does not handle itself is offered to
    /// the handlers until one takes it; one of the game's reserved letters
    /// (command_line::kReservedSwitches) that no handler takes stops the
    /// start with "-<switch> is not handled by this build". Null, or a null
    /// handler, takes no switch. An entry of the handler that throws stops
    /// the start with its message once the switches are parsed; the handler
    /// is not called again in that parse.
    ///
    /// @param context Extension::context
    /// @return the handler, kept by the extension; valid until the switches
    ///         are parsed, or null
    const oa::app::command_line::SwitchHandler* (*switch_handler)(void* context){};

    /// Returns the extension's wording of a text the engine prints.
    ///
    /// Called while --help prints the usage (usage_checks, usage_runs,
    /// usage_switches, usage_note in that order) and when the game switch -r
    /// stops the start (register_switch), before the runtime exists. Null,
    /// or a null text, keeps the engine's.
    ///
    /// @param context Extension::context
    /// @param which the text asked for
    /// @return the text, kept by the extension and read at once, or null
    const char* (*text)(void* context, ExtensionText which){};

    /// Starts the extension's part of the runtime.
    ///
    /// Called once while the runtime is built: after the session display
    /// starts, before the sounds load and the screens register. The runtime
    /// lives until main() returns, so the extension may keep the reference.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the runtime being built
    void (*startup)(void* context, Runtime& runtime){};

    /// Registers the extension's screens, overlays, dispatcher steps and kit
    /// screens.
    ///
    /// Called once while the runtime is built, after the engine's own
    /// screen packages (screens.inc) register. A kit screen
    /// (kit_screen_register) is opened by name through
    /// ScreenServices::open_kit_screen, with the services an overlay's or a
    /// screen's ScreenContext gives. A registration the registry rejects
    /// stops the start once this hook returns.
    ///
    /// @param context Extension::context
    /// @param[in,out] registry the runtime's registry; the state a
    ///        registration names must live as long as the runtime
    void (*register_screens)(void* context, ScreenRegistry* registry){};

    /// Registers the extension's Settings pages.
    ///
    /// Called once while each runtime is built, right after register_screens.
    /// Each page the extension takes into the registry
    /// (settings_page_register) is listed in Settings' nav after the
    /// engine's own sections, under a divider with its owner's name, and is
    /// drawn, scaled, focused, saved and restored to its defaults as the
    /// engine's own pages are. A page the registry refuses stops the start
    /// once this hook returns, with "settings page registration rejected:
    /// <reason>".
    ///
    /// @param context Extension::context
    /// @param[in,out] registry the runtime's Settings pages; it lives as long
    ///        as the runtime, and so must the rows of every page taken and
    ///        what each page's state names
    void (*register_settings_pages)(void* context, SettingsRegistry* registry){};

    /// Finishes the extension's start once every screen exists.
    ///
    /// Called once while the runtime is built, after every screen is
    /// registered and before the preferences file loads.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the runtime being built
    void (*ready)(void* context, Runtime& runtime){};

    /// Gives the frontend's launch values.
    ///
    /// Called each time the frontend loads the preferences, the first time
    /// while the runtime is built. Null leaves every value to the
    /// preferences.
    ///
    /// @param context Extension::context
    /// @param[out] entry the values, all null on entry; the engine copies
    ///        game_name and nickname (up to 16 characters each) as soon as
    ///        the hook returns
    void (*frontend_entry)(void* context, FrontendEntry& entry){};

    /// Names the frontend states the extension runs in place of the engine's.
    ///
    /// Called once while the runtime is built, after the game switches'
    /// fields are set and before the frontend's first dispatch. Null leaves
    /// every state to the engine. At most one extension may fill it.
    ///
    /// @param context Extension::context
    /// @param[out] handler the runtime's handler, empty on entry, which the
    ///        runtime keeps; what its context names must live as long as the
    ///        runtime
    void (*frontend_states)(void* context, oa::ui::frontend_state::StateHandler& handler){};

    /// Offers the extension a run of its own at a point of Runtime::run.
    ///
    /// Called with RunPhase::start first, and with headless_first then
    /// headless when --headless-check was given or implied. A run the hook
    /// takes replaces the rest of Runtime::run: shutdown is not called and
    /// the preferences are not written unless the hook writes them. Null
    /// runs nothing of the extension's.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param phase the point of Runtime::run
    /// @param[out] exit_code oa-game's exit status when the hook ran something
    /// @return true when the hook ran something, which ends Runtime::run
    bool (*run_mode)(void* context, Runtime& runtime, RunPhase phase, int& exit_code){};

    /// Starts the first scene of an interactive run.
    ///
    /// Called once, when no headless run, check or benchmark took the run,
    /// just before the main loop starts.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @return true when the extension started the scene; false, or a null
    ///         hook, starts the menu music
    bool (*start_scene)(void* context, Runtime& runtime){};

    /// Stops the extension's work once the main loop ends.
    ///
    /// Called once, after the interactive main loop ends and before the
    /// preferences are written; not after a headless run, a check or a run
    /// of run_mode's, nor when an exception ended the run.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    void (*shutdown)(void* context, Runtime& runtime){};

    /// Chooses what MULTI on the main menu does, and may take the game over.
    ///
    /// Called each time MULTI is activated on the main menu, by pointer or
    /// keyboard, before the engine does anything for it; not over game data
    /// with no multiplayer map, where MULTI shows the engine's
    /// missing-content notice. The extension may take the game over here
    /// (open its own screens, start a session) and answer taken. A null
    /// hook, or an answer the enum does not hold, counts as unavailable;
    /// when no extension answers otherwise, MULTI does nothing and the main
    /// menu stays up. Network play answers frontend.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app, on the main menu
    /// @return unavailable when the extension leads MULTI nowhere, leaving it
    ///         to the other extensions; frontend to run the main menu's own MULTI
    ///         step, which plays the button's sound and moves the frontend to
    ///         its multiplayer states (the extension's frontend_states handler
    ///         and screens drive them); taken when the extension has taken
    ///         the click over, after which the engine does nothing more for it
    MultiplayerSelection (*select_multiplayer)(void* context, Runtime& runtime){};

    /// Returns the frontend's Game block, which the extension keeps itself.
    ///
    /// Called each time the engine uses the frontend's block; it asks again
    /// for every use, so the extension may replace the block between calls.
    /// The engine keeps a block of its own only while this hook is null. At
    /// most one extension may fill it. An
    /// exception it throws takes the path of the code that asked: a match
    /// start the file header lists abandons the start, the in-game options
    /// panel reports "options panel unavailable: <message>", and elsewhere
    /// it ends oa-game.
    ///
    /// @param context Extension::context
    /// @return the block; never null
    oa::Game* (*frontend_game)(void* context){};

    /// Tells whether the preferences write keeps the stored password.
    ///
    /// Called each time the frontend writes the preferences. While it
    /// answers true the write leaves the password the preferences file
    /// holds as it is; otherwise it writes the frontend's password.
    ///
    /// @param context Extension::context
    /// @return true to keep the stored password; a null hook means false
    bool (*keep_stored_password)(void* context){};

    /// Runs --check-multiplayer-menu in place of the engine's check.
    ///
    /// Called once, with the SDL renderer up, when --check-multiplayer-menu
    /// was given over game data with a multiplayer map. Throws to fail the
    /// check. Over data with no multiplayer map the engine's check runs
    /// instead, which clicks MULTI and requires the missing-content notice;
    /// over other data, when no extension fills it, the check fails.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    void (*check_multiplayer_menu)(void* context, Runtime& runtime){};

    /// Returns the extension_state bits of the running game.
    ///
    /// Called whenever the engine needs to know what kind of session it
    /// runs, often several times a frame; it must not change the runtime.
    /// An exception it throws during a match start the file header lists
    /// abandons the start; elsewhere it ends oa-game.
    ///
    /// @param context Extension::context
    /// @param runtime the running app
    /// @return extension_state bits; a null hook means 0
    uint32_t (*state)(void* context, const Runtime& runtime){};

    /// Does the extension's work of one frame stage.
    ///
    /// Called every frame of the main loop and of the checks that step it,
    /// including every frame of a pause, once with FrameStage::pump and then
    /// once with after_pump, before the match clock runs and the frame is
    /// drawn, and once with presented after the frame is drawn and shown.
    /// A frame that ends the run before it is drawn has no presented stage;
    /// one the game does not show, as while its device is lost, has it all
    /// the same.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param stage the point of the frame
    void (*frame)(void* context, Runtime& runtime, FrameStage stage){};

    /// Runs one pending simulation step of the running match itself.
    ///
    /// Called for each step the match clock owes when the main loop, or a
    /// check that steps it, advances the match, ahead of the engine's tick;
    /// headless runs tick the match without it. Not called while the pause
    /// bit of Game.sim_run_flags is set, nor while the in-game menu, or the
    /// preferences it opens, hold a match played on this machine alone; they
    /// hold no shared match (extension_state::shared_match), which goes on
    /// beneath them (Runtime::match_running). Its error is reported: as a
    /// simulation error on standard error (the game's log when it plays) and
    /// on the console; the frame's remaining steps are dropped and the match
    /// runs on.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @return true when the extension ran the step; false, or a null hook,
    ///         lets the engine tick the match
    bool (*simulation_step)(void* context, Runtime& runtime){};

    /// Tells whether a finished match may leave its outcome for the end-of-game screen.
    ///
    /// Called each time the engine would move the match on: after every frame
    /// drawn with the victory or defeat outcome, and when the disc check a
    /// campaign asks for closes while the match is still on its outcome.
    /// While it returns false for a shared match the match clock keeps
    /// running and its steps go to simulation_step as usual; a match played
    /// on this machine alone stays held on its outcome.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @return false to keep the match on its outcome until the engine asks
    ///         again, after the next frame or disc check; true, or a null
    ///         hook, moves on
    bool (*outcome_ready)(void* context, Runtime& runtime){};

    /// Writes the launch values the extension owns into a new match's Game block.
    ///
    /// Called once as each match starts (a skirmish, a campaign mission or a
    /// loaded game), after the engine writes its defaults and before the game
    /// switches' options and the player records are applied. An exception
    /// it throws abandons a match start the file header lists; elsewhere it
    /// ends oa-game.
    ///
    /// @param context Extension::context
    /// @param[in,out] game the new match's Game block
    void (*match_game)(void* context, oa::Game& game){};

    /// Follows the life of a match.
    ///
    /// Called at each MatchEvent: finished as the match moves to the
    /// end-of-game screen, torn_down before the engine destroys the match
    /// (also when a new match starts over none, and after left), left when
    /// the player leaves a running match, results_reported when the
    /// end-of-game screen of a shared match (extension_state::shared_match)
    /// opens, results_released whenever the end-of-game screen lets the
    /// finished match go, shared or not (when the player leaves the screen
    /// for good, or a new match starts while it keeps one), and
    /// watching_kept when a defeated local player chooses to keep watching.
    /// Only results_reported is limited to shared matches. Its error is
    /// reported (file header), and the engine goes on with what the event
    /// reports: the match is torn down, left or let go all the same.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param event what happened
    void (*match_event)(void* context, Runtime& runtime, MatchEvent event){};

    /// Returns the end-of-game screen's text for the local player's disconnect reason.
    ///
    /// Called when the end-of-game screen of a shared match opens and the
    /// local player's record holds a reject reason other than watching; the
    /// text is shown translated in a message box.
    ///
    /// @param context Extension::context
    /// @param reason the reject reason of the local player's record
    /// @return the text, kept by the extension and read at once; null, or a
    ///         null hook, shows nothing
    const char* (*disconnect_text)(void* context, uint8_t reason){};

    /// Gives resources between players for a console gift.
    ///
    /// Called when a console command gives resources in a running match,
    /// before the engine moves them.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param from the giving player's index
    /// @param to the receiving player's index
    /// @param amount the amount given
    /// @param metal true for metal, false for energy
    /// @return true when the extension dealt with the gift; false, or a null
    ///         hook, lets the engine move the resources
    bool (*give_resources)(
        void* context, Runtime& runtime, uint8_t from, uint8_t to, float amount, bool metal
    ){};

    /// Adds the extension's hooks to the message log's.
    ///
    /// Called each time the engine builds the message log's hooks, after it
    /// fills its own: to post to the running match's message log (console
    /// lines, chat, unit reports, simulation errors, an elimination during
    /// a tick), to cycle the reported units or to change the game speed.
    /// Their context is the runtime. Its error is reported (file header),
    /// on standard error alone, and the line is posted with the engine's own
    /// hooks: what the hook set before it threw is dropped for that call.
    /// Each of the hooks is one extension's: an extension that changes one
    /// an earlier extension set stops the call, which is reported as the
    /// hook's error.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param[in,out] hooks the hooks being built; functions the extension
    ///        sets must accept the runtime as their context
    void (*message_hooks)(void* context, Runtime& runtime, oa::sim::messages::Hooks& hooks){};

    /// Announces a player who lost its last unit.
    ///
    /// Called during a simulation tick of the running match, in a campaign
    /// too. Its error is reported as a simulation error, as simulation_step's
    /// is, and the tick goes on as for a null hook.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param[in,out] world the match world the announcement goes to
    /// @param player the player who lost its last unit
    /// @return true when the extension announced it; false, or a null hook,
    ///         posts the engine's elimination message outside a campaign
    bool (*player_gone)(
        void* context, Runtime& runtime, oa::World& world, const oa::Player& player
    ){};

    /// Fills the extension's part of the in-game console's host.
    ///
    /// Called as each match starts, before its first tick, when the console
    /// binds to the match's world and before the console starts; and again
    /// should the console later be used for another world. The engine fills
    /// its own callbacks first, except those for group missions, path search
    /// and posters, which it sets after this hook and so keeps; the host's
    /// context is the runtime. The host keeps its address for as long as the
    /// runtime lives, so the extension may keep it and call its callbacks
    /// while a match runs: post_message, for one, posts to the running
    /// match's message log. Each entry of the host, such as extend and
    /// player_info_changed, is one extension's: an extension that changes
    /// one an earlier extension set stops the call. An exception it throws
    /// during a match start the file header lists abandons the start;
    /// elsewhere it ends oa-game.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param[in,out] host the console's host, which the runtime keeps at this
    ///        address while it lives; functions the extension sets must accept
    ///        the runtime as their context
    void (*console_host)(void* context, Runtime& runtime, oa::ui::console::ConsoleHost& host){};

    /// Checks the extension's console commands in --check-navigation's console check.
    ///
    /// Called once in that check, after the engine's option, cursor, debug,
    /// sound and display commands and before its unit commands. Throws to
    /// fail the check.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param enter_line types one line into the console and presses Enter;
    ///        valid for this call
    /// @param user the first argument of enter_line
    void (*check_console)(
        void* context,
        Runtime& runtime,
        void (*enter_line)(void* user, const char* line),
        void* user
    ){};

    /// Draws over the loading screen.
    ///
    /// Called each time the loading screen is drawn while a match loads,
    /// after the engine's progress bars, with the display surface locked.
    /// Its error is reported (file header), and the loading screen is shown
    /// with what was drawn.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param[in,out] target the locked display surface; valid for this call
    /// @param font the GUI font, or null while it is not loaded
    void (*draw_loading)(
        void* context, Runtime& runtime, oa::Surface& target, const oa::present::GafSprites* font
    ){};

    /// Draws the extension's readouts in the match HUD pass.
    ///
    /// Called each time the match HUD is drawn, after the resource readout
    /// and build captions and before the unit information and chat entry.
    /// What it draws shows only over the side panel and the top and bottom
    /// bars; draw_match_overlay draws over the battlefield. Its error is
    /// reported (file header), and the HUD pass goes on.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    void (*draw_match_hud)(void* context, Runtime& runtime){};

    /// Draws the extension's readouts over the battlefield.
    ///
    /// Called each time a match frame is drawn, after the HUD pass, the kills
    /// board and the message log, and before the profile bars and the paused
    /// or finished title; what it draws shows wherever the battlefield does.
    /// Its error is reported (file header), and the frame is drawn on.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param overlay the battlefield and its painter; valid for this call
    void (*draw_match_overlay)(void* context, Runtime& runtime, const MatchOverlay& overlay){};

    /// Reports that the Pause key toggled the running match's pause.
    ///
    /// Called during a running match each time the Pause key flips the pause
    /// bit of Game.sim_run_flags, after the flip, in any kind of game; the
    /// Pause key of a finished match flips nothing. While the bit is set,
    /// whoever set it, the match clock steps no simulation and
    /// simulation_step is not called; frame still is, every frame. Null
    /// keeps the pause on this machine. Its error is reported (file header);
    /// the pause stays as the key set it.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param paused the pause bit after the flip
    void (*pause_changed)(void* context, Runtime& runtime, bool paused){};

    /// Reports the progress of a match's loading.
    ///
    /// Called each time the loading sets a row of the loading screen while a
    /// match loads, the building of its world included, before the loading
    /// screen is drawn. No frame runs while the world is built, so an extension that
    /// must keep working through a long load does it here. Its error is
    /// reported (file header), and the load goes on.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param rows the loading screen's rows, each 0 to 100 percent; valid for
    ///        this call
    /// @param row_count how many rows `rows` holds
    void (*load_progress)(void* context, Runtime& runtime, const uint8_t* rows, size_t row_count){};

    /// Fills the extension's part of the in-game team panels' host.
    ///
    /// Called as each match starts, before its first tick, beside
    /// console_host, with every entry of the host null and its context the
    /// runtime. The team panels (TABMENU.GUI, SHARE.GUI, ALLIES.GUI and
    /// CONTROL.GUI) open only in a multiplayer game
    /// (extension_state::multiplayer); the engine makes their changes here
    /// itself and the host tells the other players' machines. The host keeps
    /// its address for as long as the runtime lives. Each entry of the host
    /// is one extension's: an extension that changes one an earlier
    /// extension set stops the call. An exception it throws during a match
    /// start the file header lists abandons the start; elsewhere it ends
    /// oa-game.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param[in,out] host the panels' host, which the runtime keeps at this
    ///        address while it lives; functions the extension sets must
    ///        accept the runtime as their context
    void (*team_panel_host)(void* context, Runtime& runtime, oa::ui::hud::TeamPanelHost& host){};

    /// Answers a request to end the program: the window's close button, or
    /// the system's quit while the window is open.
    ///
    /// Called for each such request the main loop receives, before the engine
    /// does anything for it; a request that arrives while a match loads is
    /// not offered, and the load stops at once. The extension may answer
    /// with a box of its own, or leave its session and end the run through
    /// ScreenServices::quit. When every extension declines, the engine
    /// handles it: in a running match, or a page opened over one, the surrender confirmation
    /// (YESORNO.GUI) with its second choice preselected; anywhere else the
    /// run ends at once.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @return true when the extension answered the request; false, or a null
    ///         hook, keeps the engine's handling
    bool (*close_requested)(void* context, Runtime& runtime){};

    /// Returns the label the match's return names, in place of the main menu.
    ///
    /// Called as each match starts; the runtime copies up to 31 characters
    /// at once and keeps them for that match's in-game menus and end-of-game
    /// screen. While the match has a label: the in-game exit menu hides EXIT
    /// GAME; the end-of-game screen's MAIN MENU leaves the pointer's picture
    /// as it is; and when the label holds 1 to 9 characters, the exit menu's
    /// and the end-of-game screen's MAIN MENU entries read it, and the exit
    /// confirmation asks "Surrender this battle and return to <label>?". A
    /// longer label leaves the exit menu's entry and the confirmation as they
    /// are and gives the end-of-game screen's entry "OK". An exception it
    /// throws during a match start the file header lists abandons the start;
    /// elsewhere it ends oa-game.
    ///
    /// @param context Extension::context
    /// @return the label, kept by the extension and read at once; null, an
    ///         empty label or a null hook means none
    const char* (*return_label)(void* context){};

    /// Reports a game speed the local player set with the speed keys or the
    /// GAME slider.
    ///
    /// Called during a running match each time '+' sets the speed, which it
    /// does below the fastest speed, 20, and each time '-' sets it, above
    /// the slowest, 1; and each time the in-game preferences' GAME slider
    /// sets it, whatever its value. Neither works in a watcher's game
    /// (extension_state::local_watcher), which calls it for neither. Called
    /// after the speed is clamped to 1..20, posted to the message log when
    /// it changed and set as the match's speed. The preferences' Cancel and
    /// UNDO, which put back the speed they opened with, and RESTORE, which
    /// puts back the normal speed, do not call it, nor does anything else
    /// that sets the speed. It must not throw; one that throws is reported
    /// (file header) and the speed stays set on this machine. Null keeps the
    /// speed on this machine.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param speed the match's game speed now, 1 to 20; 10 is normal
    void (*speed_changed)(void* context, Runtime& runtime, uint16_t speed){};

    /// Reports an application mode the frontend set.
    ///
    /// Called each time the engine sets the frontend Game's application mode
    /// (Game.mode, an oa::ui::frontend_state::mode_id value), after writing
    /// it, also when the mode is the one it already holds: as the frontend's
    /// states and screens move between menus, as a match is set up or a
    /// saved game loads, as the end-of-game screen opens and as its buttons
    /// leave it. A running match sets no mode here. It must not throw; one
    /// that throws is reported (file header) and the mode stays set.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app
    /// @param mode the mode set
    void (*app_mode_set)(void* context, Runtime& runtime, int32_t mode){};

    /// Opens a recording to replay into a new match, which the engine then steps.
    ///
    /// Called when --render-script or --generate-script needs the recording a
    /// director script names, in a headless run before any match starts. An
    /// extension that replays recordings of that kind starts the recording's
    /// match through the engine's own match start, with the engine's match
    /// hooks installed, fills `replay` and `info` and returns true. The
    /// engine then runs no tick of that match itself: it calls replay.step
    /// once for each tick, replay.status whenever it needs to know where the
    /// replay stands, and replay.close once, before it tears the match down.
    /// An extension that does not recognise the recording returns false and
    /// the next one is asked; when none takes it the run stops with "no
    /// extension of this build replays <name>". Throws std::runtime_error
    /// for a recording it recognises but cannot replay: one it cannot read,
    /// whose map is not installed, with no free slot to watch from, or, when
    /// input.strict is set, whose unit definitions differ from the
    /// installation's; the error ends oa-game.
    ///
    /// @param context Extension::context
    /// @param[in,out] runtime the running app, headless, with no match running
    /// @param input the recording's name and bytes, valid for this call only
    /// @param[out] replay the replay's step, status and close, all null on entry
    /// @param[out] info what the recording holds, all zero on entry
    /// @return true when the extension opened the recording
    bool (*open_recording)(
        void* context,
        Runtime& runtime,
        const RecordingInput& input,
        ReplayHooks& replay,
        RecordingInfo& info
    ){};

    /// Frees what the extension keeps for a runtime that is being destroyed.
    ///
    /// Called once for every runtime the engine creates, the game's own and
    /// any second runtime a check creates beside it, as that runtime is
    /// destroyed, also when its constructor throws after the extension's
    /// hooks were first called for it: after the runtime's state that
    /// follows its match is gone and before its match goes. The runtime is
    /// given only to name which one goes: nothing of it may be used but its
    /// address. No other hook is called for it afterwards. It must not
    /// throw; one that throws is reported (file header) and the runtime is
    /// destroyed all the same. Null keeps nothing to free.
    ///
    /// @param context Extension::context
    /// @param runtime the runtime being destroyed
    void (*release_runtime)(void* context, Runtime& runtime){};
};

/// Returns the player's own folder of a runtime: where that run keeps the
/// player's saved games, screenshots, films, recordings and mods.
///
/// It is the folder the runtime chose from its options and preferences:
/// --user-folder, else the preferences' open-annihilation.user-folder while
/// it holds an absolute path, else user_folder_name beside a named
/// --preferences-file, else that folder in the Documents folder, else
/// beside the preferences file the run reads when there is no Documents
/// folder. Empty before the choice. The choice is made after the
/// preferences load, which is after the startup, register_screens and ready
/// hooks, so a call from those hooks reads an empty folder; a hook called
/// once the runtime exists reads the folder.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param runtime the running app
/// @return the folder, absolute; empty before it is chosen. It lives as
///         long as the runtime
[[nodiscard]] const std::filesystem::path& player_folder(const Runtime& runtime) noexcept;

/// When a setting an extension changes takes effect, and whether the player
/// keeps it. A call that changes one of the player's settings takes it.
enum class SettingScope : uint8_t {
    /// Now, and kept as the player's own setting.
    immediate,
    /// For the next new game and nothing after it: a skirmish this machine
    /// starts, or a multiplayer game it hosts or joins. A joined game plays
    /// at its host's value and still ends this one. A game that brings its
    /// own value (a saved game, a campaign mission, a recording, a restart)
    /// neither uses nor ends it. The player's setting is left as it is.
    next_game,
    /// Kept as the player's own setting, which Settings shows at once; the
    /// games of the running session keep the value they have until the game
    /// next starts.
    next_restart,
};

/// Sets the unit limit a game plays at, as `scope` says.
///
/// The limit is the configured unit limit a game uses (Game.max_units_setting,
/// and the preferences' open-annihilation.unit-limit). With
/// SettingScope::immediate the next game offers it and the player's setting
/// becomes it, written to the preferences at once. With SettingScope::next_game
/// only the next new game plays at it: a game mode passes it as it starts its
/// game, a joined multiplayer game plays at its host's limit instead, and when
/// that match ends the player's own setting is the limit again. With
/// SettingScope::next_restart the player's setting becomes it, as Settings
/// shows, and the preferences are written; the games of this session keep
/// the limit they have until the game next starts. The value
/// is clamped into the range the unit limit setting keeps, as a stored
/// preference is: the lowest the game allows (20 for 3.1c) through the
/// highest the setting offers (1500, or a game's higher maximum). A value
/// between the setting's stops is kept. The preferences load after the
/// startup, register_screens and ready hooks, and that load replaces a limit
/// set from those hooks; a hook called once the runtime's preferences are
/// loaded sets the limit the game keeps.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param runtime the running app
/// @param units_per_player the limit, in units per player
/// @param scope when the limit takes effect, and whether the player keeps it
void set_unit_limit(Runtime& runtime, int32_t units_per_player, SettingScope scope);

/// Holds or releases the request that the main loop keep running while the
/// window is inactive.
///
/// While the request is held, the loop runs every frame there, as it does
/// for a live multiplayer game, and the frame hook keeps being called.
/// Released, the loop waits for an event, as it does otherwise. Holding it
/// again while it is held leaves it held; releasing it while it is released
/// leaves it released. The request starts released. A call may come from
/// any hook, and from one call to the next.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param runtime the running app
/// @param hold true to hold the request, false to release it
void keep_running_while_inactive(Runtime& runtime, bool hold);

/// Tells whether this machine can open a web address in the system's browser.
///
/// The answer is no on a Steam Deck in Game Mode, which has no browser to
/// hand an address to. A Steam Deck in its desktop session answers yes, and
/// so does every other machine, including one running Steam's Big Picture.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @return whether open_web_address can hand an address to the system's
///         browser
[[nodiscard]] bool web_address_available() noexcept;

/// Opens an http or https address in the system's browser.
///
/// Accepts an address whose scheme is http or https, in either letter case,
/// with a body after "://", and refuses a null or empty address, any other
/// scheme, a missing scheme and a body that holds an ASCII control or a
/// space. A refused address is not handed to the browser. The address that
/// is handed over is the one given, unchanged. A machine that cannot open
/// one (web_address_available is false) opens nothing. An accepted address
/// is handed to the system's browser through SDL.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
/// A call may come from any hook.
///
/// @param address the address, read for this call only; null is refused
/// @return true when the browser was asked to open the address and could
[[nodiscard]] bool open_web_address(const char* address);

/// One font of the modern text stack. The values match the font stack's
/// base faces. A line looks in them in the chain's order, which places the
/// endonym face before emoji.
enum class ModernTextFace : uint8_t {
    sans_bold, ///< Latin, Greek, Cyrillic and symbols, bold
    sans,      ///< the same scripts with more characters, regular
    endonyms,  ///< languages' own names and the notice before a pack is installed
    emoji,     ///< emoji, drawn in one colour like any character
};

/// How many fonts the modern text stack holds.
inline constexpr int modern_text_face_count = 4;

/// The pixel size a modern line is drawn at.
///
/// pixel_size is the sans faces' pixels per em. The endonym and emoji faces
/// take their own sizes from it. least_cjk_pixel_size holds an
/// ideograph-sized face to a least size, or 0 to leave it at the size
/// derived from pixel_size.
struct ModernTextSize {
    /// pixels per em of the sans faces
    int32_t pixel_size{14};
    /// bold looks in the bold sans face first; regular looks in the regular
    /// sans face first
    bool bold{true};
    /// the least pixels per em of an ideograph-sized face; 0 leaves the derived size
    int32_t least_cjk_pixel_size{};
};

/// The rows of one modern face at the pixel size a line draws it at.
struct ModernFaceMetrics {
    ModernTextFace face{};
    int32_t pixel_size{}; ///< pixels per em of this face
    int32_t ascent{};     ///< rows above the baseline
    int32_t descent{};    ///< rows below the baseline, the baseline's own row among them
};

/// The modern faces a line looks in, and the rows of that line.
///
/// count faces are filled, in fallback order. Entries at and after count are
/// zero. A bold line looks in all four faces; a regular line looks in the
/// regular sans face, then the endonym face, then emoji.
/// ascent and descent are the rows of the line the game draws, which fit
/// every face of the chain.
struct ModernTextChain {
    std::array<ModernFaceMetrics, modern_text_face_count> faces{};
    int32_t count{};
    int32_t ascent{};
    int32_t descent{};
};

/// One character of a modern line: the face that draws it and where.
struct ModernTextGlyph {
    char32_t character{};
    ModernTextFace face{};
    int32_t pen{};     ///< the pen's column, counted from the line's start
    int32_t advance{}; ///< pixels the pen moves past it
};

/// One modern line drawn as coverage, one byte a pixel.
struct ModernTextPixels {
    int32_t width{};    ///< columns
    int32_t height{};   ///< rows
    int32_t baseline{}; ///< the row of the baseline
    int32_t origin{};   ///< the column the pen starts at
    int32_t advance{};  ///< pixels the pen moved
    /// width * height bytes, top row first: 0 is untouched, 255 fully covered
    std::vector<uint8_t> coverage{};
};

/// The Text size a player who has not changed it reads at, in percent of the
/// game fonts' sizes. The engine checks it against its own setting's default.
inline constexpr int32_t default_game_text_size = 80;

/// The player's modern-text settings.
///
/// modern_fonts is "Use modern fonts for game text". text_size is "Text
/// size", in percent of the game fonts' sizes.
struct GameTextPreferences {
    bool modern_fonts{};
    int32_t text_size{default_game_text_size};
};

/// A text field in canvas pixels, and the caret's distance from its left.
///
/// The caret's distance is in the same pixels as the field. 0 is the field's
/// start, which is where the game's own fields keep the caret. An extension
/// whose caret stands further along the line passes that distance, so the
/// input method's candidates stand beside the caret.
struct TextField {
    int x{};
    int y{};
    int width{};
    int height{};
    int cursor{};
};

/// Gives the modern faces a line looks in at a size, and the line's rows.
///
/// The fonts are the ones that travel with the game, opened the first time
/// they are asked for. Any thread may call this and the other modern_text
/// calls: they take turns with the faces. A size the faces cannot draw gives
/// nothing, and so does a machine whose fonts cannot be opened.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param size the sans faces' pixel size, the weight and the least CJK size
/// @return the chain; empty when the size is out of range or the fonts
///         cannot be opened
[[nodiscard]] std::optional<ModernTextChain> modern_text_chain(const ModernTextSize& size);

/// Lays a line of modern text out without drawing it.
///
/// Characters that draw nothing are left out. The faces are modern_text_chain's,
/// and any thread may call this.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param text UTF-8 text, read for this call only
/// @param size the size modern_text_chain takes
/// @return each drawn character with its face and pen; empty when the text
///         is not UTF-8 or is too long, the size is out of range, or the
///         fonts cannot be opened
[[nodiscard]] std::optional<std::vector<ModernTextGlyph>>
modern_text_layout(std::string_view text, const ModernTextSize& size);

/// Draws a line of modern text as coverage.
///
/// The line is hinted to whole pixels, one bit a pixel, with no extra space
/// between characters, as the message log is drawn. The faces are
/// modern_text_chain's, and any thread may call this.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param text UTF-8 text, read for this call only
/// @param size the size modern_text_chain takes
/// @return the coverage; empty when the text is not UTF-8 or is too long,
///         the size is out of range, or the fonts cannot be opened
[[nodiscard]] std::optional<ModernTextPixels>
modern_text_pixels(std::string_view text, const ModernTextSize& size);

/// Gives the message log's modern face at a text size and scale.
///
/// The face is bold. Its pixel size is the message log's size at that text
/// size and scale, and no smaller than the least the modern fonts are drawn
/// at. While a Chinese, Japanese or Korean language is shown, an
/// ideograph-sized face is held to 12 px. The size is the sans faces'; the
/// endonym and emoji faces take their own sizes from it.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param text_size the player's Text size, in percent
/// @param scale screen pixels to a game pixel, 1 or more
/// @param cjk_language a Chinese, Japanese or Korean language is shown
/// @return the size modern_text_chain takes for the message log
[[nodiscard]] ModernTextSize
message_log_text_size(int32_t text_size, int32_t scale, bool cjk_language) noexcept;

/// Holds modern-text settings to the values the game keeps.
///
/// text_size is held to the sizes Text size offers. This does not read a
/// runtime, and a language that draws in the modern fonts is not applied:
/// game_text_preferences does that.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param modern_fonts Use modern fonts for game text is on
/// @param text_size Text size, in percent
/// @return the settings, with the text size held
[[nodiscard]] GameTextPreferences
game_text_preferences_of(bool modern_fonts, int32_t text_size) noexcept;

/// Returns the modern-text settings in effect for a runtime.
///
/// These are the settings the game is drawing with: Use modern fonts for
/// game text, including when the language shown turns those fonts on
/// whatever the setting, and Text size. Should reading them fail, the
/// defaults come back: modern fonts off and default_game_text_size, and the
/// reason is written to the log once a run.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param runtime the running app
/// @return the settings in effect
[[nodiscard]] GameTextPreferences game_text_preferences(const Runtime& runtime) noexcept;

/// Tells the input method where the focused text field is.
///
/// The field is in canvas pixels. The engine converts it to the window's own
/// coordinates and gives that rectangle to the system, with the caret's
/// distance from the field's left, so the input method's candidate window
/// and the on-screen keyboard can stand clear of the field and beside the
/// caret. A null field, or one whose width or height is not positive, clears
/// the rectangle. A runtime whose window is not open is left as it is. This
/// does not start or stop text input.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
/// A call may come from any hook.
///
/// @param runtime the running app
/// @param field the field, in canvas pixels, read for this call only; null
///        clears the rectangle
void set_focused_text_field(Runtime& runtime, const TextField* field);

/// Reads one whole file from the game's files into `bytes`.
///
/// The files are the runtime's store: the loose files of the folders it
/// layers, then the archives it mounted, as the game reads them. A loose
/// file wins over a file of the same path in any archive, a mod's archive
/// among them. Where several loose folders are layered, the first folder
/// that holds the path wins, and a mod's folder is layered ahead of the
/// game folder. Where only archives hold the path, the earliest mounted
/// archive wins. Mount order after the loose files is the revision archive,
/// the ccx group, the ufo group, the installation archives in the order a
/// mod names them, at most ten hpi archives, then the disc's archives.
///
/// A missing file, a null or empty path, and a path the store refuses
/// return false. `bytes` is then empty. The call does not throw.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
/// A call may come from any hook.
///
/// @param runtime the running app
/// @param path the file, with '\\' or '/' between its parts, matched
///        ignoring ASCII case; read for this call only; null is refused
/// @param[out] bytes the file's bytes; empty when the file is missing
/// @return true when the file was read
[[nodiscard]] bool
read_game_file(const Runtime& runtime, const char* path, std::vector<uint8_t>& bytes);

/// What a control of an extension's window is, as the driver names it.
enum class ExtensionControlKind : uint8_t {
    button,     ///< a push button
    check_box,  ///< a button that flips between checked and not
    list,       ///< a list of rows, one of which may be selected
    text_field, ///< a field that takes typed text
    slider,     ///< a scroll bar
    label,      ///< text that takes no pointer
    area,       ///< a surface that takes the pointer
    image,      ///< a picture that takes no pointer
};

/// One control of a window an extension shows.
///
/// Its place is on the game's canvas, in that canvas's pixels, as the
/// screen's own controls are placed.
struct ExtensionControl {
    std::string name;               ///< as the extension spells it; empty is left out
    ExtensionControlKind kind{};    ///< what it is
    int32_t x{};                    ///< its left column on the canvas
    int32_t y{};                    ///< its top row on the canvas
    int32_t width{};                ///< its width in canvas pixels
    int32_t height{};               ///< its height in canvas pixels
    bool visible{true};             ///< it is shown
    bool enabled{true};             ///< it takes a click
    bool focused{};                 ///< it holds the keyboard focus
    bool checked{};                 ///< a check box is checked
    std::string text;               ///< its caption, its label's text or a field's typed text
    std::vector<std::string> items; ///< a list's rows
    int32_t first_visible{};        ///< a list's first row shown
    int32_t rows{};                 ///< the rows a list shows at once
    int32_t row_height{};           ///< a list's row pitch in canvas pixels
    int32_t selected{-1};           ///< a list's selected row; -1 for none
};

/// One window an extension shows over the game, and the controls on it.
struct ExtensionWindow {
    std::string name;                       ///< as the extension spells it; empty is left out
    std::vector<ExtensionControl> controls; ///< its controls, in the extension's order
};

/// Lists the windows an extension shows, for a program driving the game.
///
/// The vector is empty on entry. The source appends the windows this
/// extension shows now. It must not change the runtime, and it must not
/// throw.
///
/// @param context the pointer set_extension_window_source was given
/// @param runtime the running app
/// @param[out] windows receives the windows
using ExtensionWindowSource =
    void (*)(void* context, const Runtime& runtime, std::vector<ExtensionWindow>& windows);

/// Shows an extension's windows to a program on this machine that drives the game.
///
/// The source replaces any source already registered with the same context.
/// A null source removes it. The engine calls the sources, in the order they
/// were first registered, when that program asks for the screen's controls,
/// on the thread that runs main(), and copies what they append before it
/// calls the next. At most 16 sources are kept. A call may come from any
/// hook. The windows are listed ahead of the screen's own controls, since
/// they take the pointer over the screen.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
///
/// @param runtime the running app
/// @param context identifies the source; the extension's own, passed back to it
/// @param source the list of windows; null removes the source for `context`
void set_extension_window_source(Runtime& runtime, void* context, ExtensionWindowSource source);

/// Returns the clock an extension's idle work follows, in milliseconds.
///
/// While the fixed clock is on, this is that clock: the running match's tick
/// times the fixed step, one thirtieth of a second. In a run a program on
/// this machine controls, it advances by that same step once a frame, from
/// the first frame, and does not read the wall clock. Otherwise it is the
/// steady clock, as the game's own clock is then. The low 32 bits are kept,
/// as that clock keeps them.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
/// A call may come from any hook, and from a window source.
///
/// @param runtime the running app
/// @return the clock, in milliseconds
[[nodiscard]] uint32_t extension_clock(const Runtime& runtime);

/// Returns the simulation hash the run plays under, as 64 lower-case
/// hexadecimal digits.
///
/// It is the profile in play: the mod's sim hash with every Developer Mode
/// override that changes the simulation, or the plain 3.1c baseline when
/// the game plays 3.1c, folded with the running match's mutator digest when
/// that match applied mutators. With no mutators applied it is that profile
/// hash alone. Choosing mutators, before the match builds them, leaves the
/// hash unchanged. Saves and network games are matched on it. A running
/// match keeps the profile it started with, so an override that changes the
/// simulation shows here once that match ends; with no match running it
/// shows at once. A display-only override leaves it unchanged.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
/// A call may come from any hook.
///
/// @param runtime the running app
/// @return the hash
[[nodiscard]] std::string simulation_hash(const Runtime& runtime);

/// The mutators whose digest simulation_hash folds in at this moment.
///
/// `count` is how many play, and `ids` names them in the order they play.
/// `simulation_hash_without` is the simulation hash the run would have
/// without them: 64 lower-case hexadecimal digits. When `count` is 0 it
/// equals simulation_hash. A running match keeps the mutators it started
/// with until it ends, as simulation_hash keeps that match's hash.
struct RunMutators {
    uint32_t count{};
    std::vector<std::string> ids{};
    std::string simulation_hash_without{};
};

/// Returns the mutators whose digest simulation_hash folds in now.
///
/// Choosing mutators, before the match builds them, leaves the count at 0
/// and the hash the one simulation_hash reports. Both calls read the same
/// profile and the same mutator digest, so they agree at this moment.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
/// A call may come from any hook.
///
/// @param runtime the running app
/// @return the mutators and the hash without them
[[nodiscard]] RunMutators run_mutators(const Runtime& runtime);

/// Hands a chat line from another machine to the computer seats it reached.
///
/// A null line is ignored. With no match running, nothing is handed on.
/// The line is the one this machine shows, without a further change.
///
/// It is not a hook: the engine does not call it, and no extension fills it.
/// A call may come from any hook.
///
/// @param runtime the running app
/// @param from slot of the sender
/// @param addressee slot the record was addressed to
/// @param broadcast the record went to every player
/// @param line the line as this machine shows it; null is ignored
void hear_remote_seat_chat(
    Runtime& runtime, uint8_t from, uint8_t addressee, bool broadcast, const char* line
);

} // namespace oa::app
