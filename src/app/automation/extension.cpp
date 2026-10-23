// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The automation endpoint's extension, filled by
// oa_extension_init_automation. Without --fark the endpoint's hooks open
// no socket and write no file. With it, the first runtime's start refuses
// the engine options that run no main loop, opens the endpoint and writes
// its address and token; the endpoint then serves its client from the
// frame hook of every runtime the process builds, so that a client stays
// connected through a switch of the mod, and sends its events from the
// frame hook, from load_progress while a match loads and no frame runs,
// and from match_event as a match is torn down. --status-stdout,
// --status-file and --quit-after-game follow the same events with no
// socket. The hooks read the game through the check host, the automation
// host and the multiplayer screens' lobby and never change the simulation:
// the hooks that could take over a simulation step, resources, a departing
// player or a close request are left null, and so is the hook that tells
// the session's kind.
#include "endpoint.hpp"
#include "options.hpp"
#include "serve_reports.hpp"
#include "status.hpp"

#include "oa/app/app.hpp"
#include "oa/app/automation_host.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/extension.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/core/world.h"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

// The version of the engine's extension table these hooks follow. A build
// against a table of another version stops here until the engine's change
// has been read and the hooks follow it. Version 14 adds functions an
// extension calls rather than hooks (extension.hpp lists them).
// These hooks call none of them. Version 15 adds the OA UI kit, which these
// hooks do not use. Version 16 adds run_mutators, which these hooks do not
// call. Version 17 adds hear_remote_seat_chat, which these hooks do not call.
constexpr uint32_t kExtensionApiVersionFollowed = 17;
static_assert(
    oa::app::extension_api_version == kExtensionApiVersionFollowed,
    "the engine's extension table changed: follow its change, then raise "
    "kExtensionApiVersionFollowed"
);

namespace oa::app::automation {
namespace {

// What the command line asked of the endpoint, and the endpoint, for the
// whole process.
struct AutomationContext {
    EndpointOptions options{};
    Endpoint endpoint;
};

/// Returns the process's automation context.
///
/// @return the one context, made on first use
AutomationContext& automation_context() {
    static AutomationContext context;
    return context;
}

/// Returns the file the endpoint's address and token go to: --fark-file's,
/// else automation.json in the folder of the preferences file.
///
/// @param options what the command line asked of the endpoint
/// @param run the run's options
/// @return the file
std::filesystem::path endpoint_file(const EndpointOptions& options, const oa::app::Options& run) {
    if (options.file)
        return *options.file;
    return preference_file(run.preferences_file).parent_path() /
           std::string(default_endpoint_file_name);
}

/// Asks the main loop to leave, as the window's close button does.
void push_quit() {
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    quit.quit.timestamp = SDL_GetTicksNS();
    (void)SDL_PushEvent(&quit);
}

/// Tells whether a match is still on screen.
///
/// Quitting then asks whether to surrender, and that question pauses the
/// match, so a game that has ended stays on screen. The score screen, and
/// a match already left, have no such question.
///
/// @param runtime the running game
/// @return true while the match screen still has its world
bool match_on_screen(Runtime& runtime) {
    const CheckHost check = oa::app::check_host(runtime);
    if (check.screen == nullptr || check.screen(check.context) != screen_id(Screen::match))
        return false;
    const AutomationHost host = oa::app::automation_host(runtime);
    return host.match_world != nullptr && host.match_world(host.context) != nullptr;
}

/// Tells whether the local player lost the connection.
///
/// reject_reason 6 is the notice "You have lost connection with the game".
/// Read while the match still exists.
///
/// @param runtime the running game
/// @return true when that notice is the local player's
bool connection_lost(Runtime& runtime) {
    const AutomationHost host = oa::app::automation_host(runtime);
    if (host.match_world == nullptr)
        return false;
    const oa::World* world = host.match_world(host.context);
    if (world == nullptr)
        return false;
    const uint8_t local = world->game.local_player_index;
    if (local >= OA_PLAYER_COUNT)
        return false;
    return world->game.players[local].reject_reason == 6;
}

/// Leaves once --quit-after-game's moment has been reached.
///
/// While the match is still on screen the quit waits: the system's quit
/// would open the surrender question and pause the match.
///
/// @param runtime the running game
void leave_if_asked(Runtime& runtime) {
    if (match_on_screen(runtime))
        return;
    if (status_take_quit())
        push_quit();
}

/// Opens the endpoint as the first runtime starts (Extension::startup).
///
/// A run whose options run no main loop, or own it, is refused with an
/// error the engine raises as its own. Status lines and --quit-after-game
/// keep the main loop running while the window is inactive, and do not
/// open the endpoint.
///
/// @param runtime the runtime being built
void startup(void* /*context*/, Runtime& runtime) {
    auto& automation = automation_context();
    if (status_following())
        keep_running_while_inactive(runtime, true);
    if (!automation.options.enabled || automation.endpoint.listening())
        return;
    const auto& run = runtime_options(runtime);
    if (const std::string_view conflict = conflicting_option(run); !conflict.empty())
        throw std::runtime_error(
            "--fark cannot be used with " + std::string(conflict) +
            ": the automation endpoint is served from the game's main loop"
        );
    automation.endpoint.open(automation.options.listen, endpoint_file(automation.options, run));
}

/// Serves the endpoint at each stage of a frame (Extension::frame).
///
/// When the endpoint is not serving, a launcher's status lines still follow
/// the frame. The endpoint's own frame work feeds those lines when it is.
///
/// @param[in,out] runtime the running game
/// @param stage the stage of the frame
void frame(void* /*context*/, Runtime& runtime, FrameStage stage) {
    auto& automation = automation_context();
    const bool serving = automation.options.enabled && automation.endpoint.listening();
    if (automation.options.enabled)
        automation.endpoint.serve(runtime, stage);
    if (!serving && stage == FrameStage::pump)
        report_status_frame(runtime);
    leave_if_asked(runtime);
}

/// Sends the events of a match's loading, while no frame runs (Extension::load_progress).
///
/// @param[in,out] runtime the running game
/// @param rows the loading screen's rows, each 0 to 100 percent
/// @param row_count how many rows `rows` holds
void load_progress(void* /*context*/, Runtime& runtime, const uint8_t* rows, size_t row_count) {
    if (rows == nullptr)
        return;
    if (!status_following() && !automation_context().options.enabled)
        return;
    const std::span<const uint8_t> progress(rows, row_count);
    auto& automation = automation_context();
    if (automation.options.enabled && automation.endpoint.listening()) {
        automation.endpoint.serve_between_frames(runtime, [progress](Endpoint& endpoint) {
            send_loading_events(endpoint, progress);
        });
        return;
    }
    if (!status_following())
        return;
    const AutomationHost host = oa::app::automation_host(runtime);
    const oa::World* match = host.match_world != nullptr ? host.match_world(host.context) : nullptr;
    status_on_loading(match, progress);
}

/// Sends the end of the running match as it is torn down (Extension::match_event).
///
/// A player who leaves before the game has ended is recorded while the
/// match still exists. --quit-after-game then leaves once that match is no
/// longer on screen, and --quit-after-game=after-score once the score
/// screen has been left.
///
/// @param[in,out] runtime the running game
/// @param event what happened to the match
void match_event(void* /*context*/, Runtime& runtime, MatchEvent event) {
    auto& automation = automation_context();
    if (status_following() && event == MatchEvent::left)
        status_note_departure(connection_lost(runtime));
    if (status_following() && event == MatchEvent::results_released)
        status_note_score_left();
    if (event == MatchEvent::torn_down) {
        if (automation.options.enabled && automation.endpoint.listening())
            automation.endpoint.serve_between_frames(runtime, send_match_gone);
        else if (status_following())
            status_on_match_gone();
    }
    leave_if_asked(runtime);
}

} // namespace
} // namespace oa::app::automation

/// Fills the automation endpoint's extension table.
///
/// @param[in,out] table the table, zeroed
void oa_extension_init_automation(oa::app::Extension* table) {
    auto& context = oa::app::automation::automation_context();
    oa::app::automation::fill_option_hooks(*table, context.options);
    table->startup = oa::app::automation::startup;
    table->frame = oa::app::automation::frame;
    table->load_progress = oa::app::automation::load_progress;
    table->match_event = oa::app::automation::match_event;
}
