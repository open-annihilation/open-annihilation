// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Network play's extension, filled by oa_extension_init_netgame: the hooks
// through which oa-game runs the multiplayer screens, networked matches, .tad
// playback (a --play-demo run's, and a recording the engine hands over to
// replay one tick at a time) and the network checks. The extension owns
// network play's state for each runtime its hooks are called for
// (NetworkPlay, network_play.hpp), and RuntimeExtension turns each hook into
// work on that state. The screen host and services, which
// stay the same while the runtime lives, are kept from an overlay registered
// for them: a request to close the window and the leaves it leads to end the
// program through them. An extension built on network play reaches it
// through oa/app/netgame/extension_api.hpp, whose hooks this extension calls.
#include "oa/app/runtime.hpp"
#include "oa/app/game_directory.hpp"
#include "battle_lines.hpp"
#include "close_handlers.hpp"
#include "demo_state.hpp"
#include "launch_binding.hpp"
#include "multiplayer_menu_check.hpp"
#include "net_options.hpp"
#include "net_state.hpp"
#include "network_play.hpp"
#include "traffic_overlay.hpp"

#include "oa/app/check_host.hpp"
#include "oa/netgame/frontend/multiplayer_states.hpp"
#include "oa/netgame/messages/departures.hpp"
#include "oa/session/demo.hpp"
#include "oa/sim/messages.hpp"
#include "oa/formats/tad.hpp"
#include "oa/ui/hud/team_panels.hpp"
#include "oa/ui/frontend_multiplayer/connect.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The version of the engine's extension table these hooks follow. A build
// against a table of another version stops here until the engine's change
// has been read and the hooks follow it. Version 14 adds functions an
// extension calls rather than hooks (extension.hpp lists them).
// These hooks call none of them.
constexpr uint32_t kExtensionApiVersionFollowed = 14;
static_assert(
    oa::app::extension_api_version == kExtensionApiVersionFollowed,
    "the engine's extension table changed: follow its change, then raise "
    "kExtensionApiVersionFollowed"
);

namespace oa::app {

namespace {

namespace mp = oa::ui::frontend_multiplayer;
namespace report_event = oa::app::netgame::extension_api::report_event;
namespace demo = oa::session::demo;

// ScreenContext::host and ::services, kept from the overlay registered for
// them; null until the runtime creates its overlays.
struct KeptServices {
    void* host{};
    const ScreenServices* services{};
};

KeptServices g_services;

// The handler a request to end the program runs, as the game keeps it
// installed.
CloseHandlers g_close;

// What --check-host-not-found has seen: whether the game list has begun to
// wait for the host, and when it first showed "Host not found.  Exiting...".
struct HostNotFoundCheck {
    bool waited{};
    std::optional<uint32_t> exiting_shown; ///< host_not_found_clock_ms() when it first showed
};

HostNotFoundCheck g_host_check;

// The simulated time of --check-host-not-found, and the step it takes each
// frame: one frame at 30 frames a second.
uint32_t g_host_check_clock_ms = 0;
constexpr uint32_t kHostCheckFrameMs = 1000 / 30;

/// Tells whether --check-host-not-found's launch has ended (LaunchBinding::launch_ended).
///
/// @return false until the game list begins to wait for the host, true from then on
bool host_wait_began() {
    if (mp::multiplayer_connect().host_waiting)
        g_host_check.waited = true;
    return g_host_check.waited;
}

/// Runs --check-host-not-found's part of a frame.
///
/// Until a multiplayer screen shows, each frame asks for a frontend pass, as
/// the game's frontend runs one every frame, so the "-n1" launch opens
/// multiplayer without input. The frame at which the game list first shows
/// "Host not found.  Exiting..." is reported.
void host_not_found_frame() {
    g_host_check_clock_ms += kHostCheckFrameMs;
    if (!mp::multiplayer_showing() && !g_host_check.waited && g_services.services != nullptr &&
        g_services.services->run_frontend != nullptr)
        g_services.services->run_frontend(g_services.host);
    if (g_host_check.exiting_shown || mp::multiplayer_message() != mp::kHostNotFoundExitingText)
        return;
    g_host_check.exiting_shown = g_host_check_clock_ms;
    std::cout << "host-not-found check: the game list shows \"" << mp::kHostNotFoundExitingText
              << "\"\n";
}

/// Reports, for --check-host-not-found, the end of the run the game asks for.
///
/// @param reason the text shown first; null for none
/// @param exit_code the program's exit status
void report_host_not_found_quit(const char* reason, int exit_code) {
    std::cout << "host-not-found check: ";
    if (g_host_check.exiting_shown)
        std::cout << g_host_check_clock_ms - *g_host_check.exiting_shown << " ms later ";
    std::cout << "the game leaves with ";
    if (reason != nullptr)
        std::cout << "the reason \"" << reason << '"';
    else
        std::cout << "no reason";
    std::cout << " and exit status " << exit_code << '\n';
}

// What the direct game's lines (--host, --join) have reported: how far it
// had got when last reported, whether a multiplayer screen has shown yet,
// and the battle room's players as last listed.
struct DirectGameReport {
    mp::DirectGameStep reported = mp::DirectGameStep::connecting;
    bool shown{};
    uint32_t frames{}; // frames run before a multiplayer screen first showed
    std::string players;
};

// The most frames that ask for a frontend pass while no multiplayer screen
// has shown yet; game data that offers no multiplayer stays on the main
// menu.
constexpr uint32_t kDirectGameOpeningFrames = 300;

DirectGameReport g_direct_report;

/// Names the players seated in the battle room, in slot order.
///
/// @param lobby the multiplayer screens' lobby
/// @return the names joined by ", "; empty when the lobby has no game
std::string battle_room_players(mp::Lobby& lobby) {
    std::string names;
    if (lobby.game == nullptr)
        return names;
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        const auto& player = mp::slot_player(lobby, slot);
        const std::string name(player.name, ::strnlen(player.name, sizeof player.name));
        if (player.in_use == 0 || name.empty())
            continue;
        if (!names.empty())
            names += ", ";
        names += name;
    }
    return names;
}

/// Runs the part of a frame that follows the game --host or --join asked for.
///
/// Until a multiplayer screen first shows, each frame asks for a frontend
/// pass, so multiplayer opens without input, for kDirectGameOpeningFrames
/// frames at most. Standard output reports, each
/// line once and flushed at once: the battle room opening, with the game's
/// name and the local player's; the players the battle room lists, whenever
/// they change while it shows; and a join or a game the screens refused,
/// with the notice they showed.
void direct_game_frame() {
    const auto& game = net_options().direct_game;
    if (game.kind == mp::DirectGame::Kind::none)
        return;
    auto& report = g_direct_report;
    if (mp::multiplayer_showing())
        report.shown = true;
    if (!report.shown && report.frames < kDirectGameOpeningFrames &&
        g_services.services != nullptr && g_services.services->run_frontend != nullptr) {
        ++report.frames;
        g_services.services->run_frontend(g_services.host);
    }
    const auto& progress = mp::multiplayer_direct_game_progress();
    if (progress.reached != report.reported) {
        report.reported = progress.reached;
        if (progress.reached == mp::DirectGameStep::battle_room)
            std::cout << "multiplayer: in the battle room of \"" << progress.game_name << "\" as "
                      << progress.player_name << std::endl;
        else if (
            progress.reached == mp::DirectGameStep::failed &&
            game.kind == mp::DirectGame::Kind::join
        )
            std::cout << "multiplayer: could not join " << game.address << ": " << progress.notice
                      << std::endl;
        else if (progress.reached == mp::DirectGameStep::failed)
            std::cout << "multiplayer: could not host the game: " << progress.notice << std::endl;
    }
    // A local player the host refused is on its way out, and lists no one.
    auto& lobby = mp::multiplayer_lobby();
    if (report.reported != mp::DirectGameStep::battle_room || !mp::multiplayer_showing() ||
        lobby.game == nullptr || mp::local_player(lobby).reject_reason != 0)
        return;
    auto players = battle_room_players(lobby);
    if (players.empty() || players == report.players)
        return;
    report.players = std::move(players);
    std::cout << "multiplayer: battle room players: " << report.players << std::endl;
}

/// Keeps the screen host and services an overlay's create is given.
///
/// @param ctx the overlay's context
/// @param state unused
void keep_services(ScreenContext* ctx, void* /*state*/) {
    g_services.host = ctx->host;
    g_services.services = ctx->services;
}

/// Ends the run through the kept screen services (ScreenServices::quit).
///
/// @param reason text shown first; null for none
/// @param exit_code the program's exit status
void quit_through_services(const char* reason, int exit_code) {
    if (net_options().check_host_not_found)
        report_host_not_found_quit(reason, exit_code);
    if (g_services.services != nullptr && g_services.services->quit != nullptr)
        g_services.services->quit(g_services.host, reason, exit_code);
}

/// Tells whether the kept screen services can end the run (ScreenServices::quit).
///
/// @return false until the overlay that keeps them is created
bool can_quit() {
    return g_services.services != nullptr && g_services.services->quit != nullptr;
}

/// Opens SELPROV.GUI, the multiplayer provider list (the setup step's service select).
///
/// @param ctx Screen context of the dispatcher step.
/// @param user Step data (unused).
void step_setup_service_select(ScreenContext* ctx, void* /*user*/) {
    screen_request(ctx, mp::kScreenProviders);
}

/// Reloads the unit headers through the frontend Game block the lobby reads.
///
/// @param ctx Screen context of the dispatcher step.
/// @param user Step data (unused).
void step_reload_unit_overrides(ScreenContext* ctx, void* /*user*/) {
    mp::multiplayer_reload_unit_headers(ctx);
}

/// Returns where a launch lands in the running game, without the leave, which needs the runtime.
///
/// With --check-host-not-found a launch ends as the game list begins to wait
/// for the host (host_wait_began).
///
/// @return the launch block and the switches, the pending flag and
///         mode of the frontend states and the close handlers
LaunchBinding netgame_launch_binding() {
    auto& netgame = netgame_context();
    LaunchBinding launch{};
    launch.block = &netgame.launch.block;
    launch.switches = &netgame.launch;
    launch.launch_pending = &netgame.frontend.state.launch_pending;
    launch.app_mode_pending = &netgame.frontend.state.pending_app_mode;
    launch.close_handlers = &g_close;
    if (net_options().check_host_not_found)
        launch.launch_ended = host_wait_began;
    return launch;
}

/// Registers network play's screens, dispatcher steps and queries (Extension::register_screens).
///
/// The multiplayer screens register first, then the overlay that keeps the
/// screen services, the steps that reload the unit overrides and open the
/// setup's provider list, and the answers to the frontend's questions about
/// joining and leaving a session. A launch lands in the launch block
/// the switches write, which the multiplayer screens, the frontend states and
/// the frontend's entry read. With --check-host-not-found the "-n1" launch
/// counts as active from here until the game list begins to wait for the
/// host.
///
/// @param context Extension context (unused).
/// @param[in,out] registry Screen registry of the app.
void register_screens(void* /*context*/, ScreenRegistry* registry) {
    register_multiplayer_screens(registry);
    OverlayDesc services{};
    services.name = "netgame-screen-services";
    services.screen = kScreenAny;
    services.create = keep_services;
    (void)overlay_register(registry, &services);
    auto& netgame = netgame_context();
    bind_launch(netgame_launch_binding());
    if (net_options().check_host_not_found)
        set_launch_active(true);
    mp::multiplayer_bind_launch_link(launch_link());
    mp::multiplayer_bind_direct_game(net_options().direct_game);
    netgame.launch_active = launch_active;
    netgame.frontend.host.connection_type = launch_connection_type;
    register_frontend_state_queries(registry);
    (void)step_register(
        registry,
        oa::ui::frontend_state::Step::reload_unit_overrides,
        step_reload_unit_overrides,
        nullptr
    );
    (void)step_register(
        registry,
        oa::netgame::frontend::step::setup_service_select,
        step_setup_service_select,
        nullptr
    );
}

// What the replay's status reports once the replay has ended under the
// engine: its match is no longer the running one.
constexpr const char* kReplayEnded = "the replay ended: its match is no longer the running one";
// What the replay's status reports when it could not be read.
constexpr const char* kReplayStatusUnread = "the replay's status could not be read";

/// Reports on standard error an error a hook that must not throw caught
/// itself (extension.hpp's hooks that must not throw).
///
/// @param hook the hook's name
/// @param message what was thrown
void report_caught(const char* hook, const char* message) noexcept {
    std::fprintf(stderr, "network play: %s: %s\n", hook, message);
}

// --check-recording-hook's inputs that the hook must decline or refuse: a
// text that is no recording, and a recording's header chunk that claims
// more bytes than follow its magic.
constexpr std::string_view kCheckOtherBytes = "A director script names the recording it shows.\n";
constexpr const char* kCheckOtherName = "script.txt";
constexpr const char* kCheckTruncatedName = "truncated.tad";

/// Returns a recording's header cut short after its magic.
///
/// @return The header chunk's length field, which counts the whole fixed
///         header, and the magic; nothing else.
std::vector<uint8_t> truncated_recording_header() {
    constexpr std::size_t claimed =
        oa::formats::tad::layout::chunk_length_bytes + oa::formats::tad::layout::header_fixed_bytes;
    std::vector<uint8_t> bytes{static_cast<uint8_t>(claimed), static_cast<uint8_t>(claimed >> 8)};
    bytes.insert(bytes.end(), oa::formats::tad::magic.begin(), oa::formats::tad::magic.end());
    return bytes;
}

/// Reads a whole file.
///
/// @param path The file.
/// @return Its bytes; throws std::runtime_error when it cannot be opened.
std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path_to_utf8(path));
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Leads MULTI into the multiplayer screens through the main menu's own step (Extension::select_multiplayer).
///
/// @param context Extension context (unused).
/// @param runtime The running app (unused).
/// @return Always frontend.
MultiplayerSelection select_multiplayer(void* /*context*/, Runtime& /*runtime*/) {
    return MultiplayerSelection::frontend;
}

/// Returns the frontend's Game block, which the multiplayer screens keep (Extension::frontend_game).
///
/// The screens replace their block when they reset, so it is looked up on
/// every call.
///
/// @param context Extension context (unused).
/// @return The multiplayer screens' Game block; never null.
oa::Game* frontend_game(void* /*context*/) {
    return &mp::multiplayer_game();
}

/// Returns the end-of-game screen's text for a player's disconnect reason (Extension::disconnect_text).
///
/// @param context Extension context (unused).
/// @param reason Reject reason of the player record.
/// @return The multiplayer screens' text for the reason.
const char* disconnect_text(void* /*context*/, uint8_t reason) {
    return mp::reject_reason_text(reason);
}

} // namespace

oa::netgame::sock::HostClock host_not_found_clock() noexcept {
    oa::netgame::sock::HostClock clock;
    clock.now_ms = [](void*) { return g_host_check_clock_ms; };
    clock.advance = [](void*, uint32_t milliseconds) { g_host_check_clock_ms += milliseconds; };
    return clock;
}

uint32_t host_not_found_clock_ms() noexcept {
    return g_host_check_clock_ms;
}

namespace {

/// Returns network play's state of every runtime its hooks have been called for.
///
/// Never destroyed: a run that ends without destroying its runtime leaves
/// that runtime's state as it is, sessions included.
///
/// @return The states, one for each such runtime not yet destroyed.
std::vector<std::unique_ptr<NetworkPlay>>& network_plays() {
    static auto* const plays = new std::vector<std::unique_ptr<NetworkPlay>>();
    return *plays;
}

} // namespace

NetworkPlay::NetworkPlay(Runtime& runtime) noexcept : runtime_(runtime) {
}

NetworkPlay::~NetworkPlay() = default;

NetworkPlay& NetworkPlay::of(Runtime& runtime) {
    if (auto* play = find(runtime))
        return *play;
    return *network_plays().emplace_back(std::make_unique<NetworkPlay>(runtime));
}

NetworkPlay* NetworkPlay::find(const Runtime& runtime) noexcept {
    for (const auto& play : network_plays())
        if (&play->runtime_ == &runtime)
            return play.get();
    return nullptr;
}

void NetworkPlay::free_for(const Runtime& runtime) noexcept {
    auto& plays = network_plays();
    const auto gone = std::find_if(plays.begin(), plays.end(), [&runtime](const auto& play) {
        return &play->runtime_ == &runtime;
    });
    if (gone == plays.end())
        return;
    // Taken out of the list before it is freed, so that nothing freeing it
    // finds it.
    const std::unique_ptr<NetworkPlay> play = std::move(*gone);
    plays.erase(gone);
}

std::optional<std::size_t> NetworkPlay::match_ticks() const {
    return runtime_options(runtime_).match_ticks;
}

void NetworkPlay::post_departure(oa::World& world, uint32_t player_id) {
    oa::netgame::messages::post_departure(world, player_id, runtime_.message_hooks());
}

struct RuntimeExtension {
    /// Returns the network session of a runtime's network play.
    ///
    /// @param runtime The running app.
    /// @return The session, or null when network play has none for it.
    static NetState* net_of(const Runtime& runtime) noexcept {
        const auto* play = NetworkPlay::find(runtime);
        return play != nullptr ? play->net_.get() : nullptr;
    }

    /// Posts a line to the running network match's message log (bind_battle_lines).
    ///
    /// @param context Network play's state for the running app.
    /// @param line The line.
    /// @return False when no network match runs.
    static bool post_to_match(void* context, const char* line) {
        auto& play = *static_cast<NetworkPlay*>(context);
        auto* state = play.net_.get();
        oa::World* world = play.reporter_world();
        if (state == nullptr || !state->active || state->console_host == nullptr ||
            state->console_host->post_message == nullptr || world == nullptr)
            return false;
        post_wrapped_line(
            line,
            [](void* host, const char* part) {
                const auto* console = static_cast<const oa::ui::console::ConsoleHost*>(host);
                console->post_message(
                    console->context,
                    part,
                    oa::sim::messages::kind_notice,
                    oa::sim::messages::sender_none
                );
            },
            const_cast<oa::ui::console::ConsoleHost*>(state->console_host),
            world->game
        );
        return true;
    }

    /// Binds the multiplayer screens to the network session once every screen exists (Extension::ready).
    ///
    /// The session then follows a launch: a launched host admits joiners
    /// into a closed game, and the lines posted to the battle reach the
    /// running match's message log. The launch's leave is bound to the
    /// runtime's network play.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    static void ready(void* /*context*/, Runtime& runtime) {
        auto& play = NetworkPlay::of(runtime);
        play.net_bind_multiplayer();
        if (auto* state = play.net_.get())
            state->connection.launch_active = launch_active;
        LaunchBinding launch = netgame_launch_binding();
        launch.leave_context = &play;
        launch.leave_game = leave_game;
        bind_launch(launch);
        bind_battle_lines(&play, post_to_match);
    }

    /// Leaves the game and ends the program with exit status 0, as the game does when a request to
    /// close the window or its answer leaves.
    ///
    /// The local player's disconnect reason, with `with_reason`, is read
    /// first. Without it a running score report hears outcome event 8
    /// (session_closed), as the exit confirmation's first choice reports it.
    /// A network match's session closes as a finished one does; outside a
    /// match the multiplayer screens' session closes, which the other
    /// machines see as this machine's players departing. No close handler is
    /// left installed, the frontend Game's leaving bit rises, then the run
    /// ends through ScreenServices::quit, with the reason shown first when
    /// there is one.
    ///
    /// @param context Network play's state for the running app.
    /// @param with_reason Whether the disconnect reason shows first.
    static void leave_game(void* context, bool with_reason) {
        auto& play = *static_cast<NetworkPlay*>(context);
        const char* reason = with_reason ? leave_reason(mp::multiplayer_game()) : nullptr;
        if (!with_reason)
            play.close_reporter();
        play.net_leave();
        if (auto* state = play.net_.get(); state != nullptr && state->connected) {
            const auto net = oa::netgame::match::session_lobby_net(&state->connection);
            net.close(net.context);
        }
        g_close.installed = CloseHandler::none;
        oa::Game& game = mp::multiplayer_game();
        game.outcome_flags = static_cast<uint16_t>(game.outcome_flags | mp::kOutcomeLeaving);
        quit_through_services(reason, 0);
    }

    /// Follows the game's application mode into the installed close handler.
    ///
    /// A network match counts from its launch, a match played on this
    /// machine while its screen shows; the mode is otherwise the frontend
    /// Game's, which the engine sets.
    ///
    /// @param runtime The running app.
    static void observe_close_handlers(const Runtime& runtime) {
        close_handlers_observe(g_close, close_observation(runtime));
    }

    /// Returns what the installed close handler follows, as the game stands.
    ///
    /// @param runtime The running app.
    /// @return The frontend Game's mode and whether a match runs (a network
    ///         match from its launch, a match played on this machine while
    ///         its screen shows).
    static CloseObservation close_observation(const Runtime& runtime) {
        CloseObservation now{};
        now.frontend_mode = mp::multiplayer_game().mode;
        const auto* play = NetworkPlay::find(runtime);
        now.match_runs = (play != nullptr && play->net_match_active()) || runtime.match_running();
        return now;
    }

    /// Installs the close handler of the application mode the frontend set (Extension::app_mode_set).
    ///
    /// Every mode set installs its handler, a mode set again to the one the
    /// game runs included (close_handlers_mode_set), as each application mode
    /// the game sets does.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param mode The application mode set.
    static void app_mode_set(void* /*context*/, Runtime& runtime, int32_t mode) noexcept {
        try {
            close_handlers_mode_set(g_close, mode, close_observation(runtime));
        } catch (const std::exception& error) {
            report_caught("app_mode_set", error.what());
        }
    }

    /// Answers a request to end the program with the installed close handler (Extension::close_requested).
    ///
    /// An extension built on network play is asked first and may answer it
    /// itself. The exit confirmation is the engine's surrender confirmation
    /// in a match, and opens over the multiplayer screens elsewhere; anywhere
    /// else the engine ends the run.
    /// The leave has the game leave (leave_game) while a multiplayer session
    /// is open or a disconnect reason is to be shown; with neither there is
    /// nothing to leave, and the engine's end of the run, at once, is the
    /// leave. With no handler, or before the screen services that end the
    /// run are kept, the engine ends the run at once too.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @return False when the engine answers.
    static bool close_requested(void* /*context*/, Runtime& runtime) {
        observe_close_handlers(runtime);
        switch (g_close.installed) {
        case CloseHandler::none:
            return false;
        case CloseHandler::exit_confirm:
            return !g_close.in_match && mp::multiplayer_request_exit_confirm();
        case CloseHandler::leave:
            break;
        }
        auto& play = NetworkPlay::of(runtime);
        const bool something_to_leave = play.net_session_open() || play.net_match_active() ||
                                        leave_reason(mp::multiplayer_game()) != nullptr;
        if (!something_to_leave || !can_quit())
            return false;
        leave_game(&play, true);
        return true;
    }

    /// Runs network play's own modes (Extension::run_mode).
    ///
    /// First of the headless runs --net-loopback-check; then
    /// --check-recording-hook, or else a headless --play-demo (--match-ticks
    /// ticks, kDefaultDemoTicks without it), followed by a preferences flush.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param phase Point of Runtime::run offering the run.
    /// @param[out] exit_code Receives the mode's exit code when one ran.
    /// @return True when a mode ran, which ends Runtime::run.
    static bool run_mode(void* /*context*/, Runtime& runtime, RunPhase phase, int& exit_code) {
        const auto& options = net_options();
        switch (phase) {
        case RunPhase::start:
            return false;
        case RunPhase::headless_first:
            if (!options.net_loopback_ticks)
                return false;
            exit_code =
                NetworkPlay::of(runtime).run_net_loopback_check(*options.net_loopback_ticks);
            return true;
        case RunPhase::headless: {
            if (!options.check_recording_hook && options.play_demo.empty())
                return false;
            auto& play = NetworkPlay::of(runtime);
            const auto ticks = play.match_ticks().value_or(kDefaultDemoTicks);
            exit_code = options.check_recording_hook ? check_recording_hook(runtime, ticks)
                                                     : play.run_headless_demo(ticks);
            const auto host = check_host(runtime);
            host.write_preferences(host.context);
            return true;
        }
        }
        return false;
    }

    /// Opens a TA Demo recording the engine hands over and starts its match (Extension::open_recording).
    ///
    /// Bytes without the recording's magic are declined. A recording is
    /// staged for start_demo_playback, which loads it in place of the
    /// --play-demo file and refuses, when input.strict is set, a unit table
    /// that differs from the installed one; the match is then the watcher's,
    /// as a --play-demo run's. The replay's ticks, status and close act on
    /// the runtime's playback.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app, with no match running.
    /// @param input The recording's name and bytes.
    /// @param[out] replay Receives the replay's hooks, whose context is the runtime's network play.
    /// @param[out] info Receives what the recording holds.
    /// @return False when the bytes are not a TA Demo recording; throws
    ///         std::runtime_error, naming the recording, for one that cannot be
    ///         replayed, and when a recording is already being replayed.
    static bool open_recording(
        void* /*context*/,
        Runtime& runtime,
        const RecordingInput& input,
        ReplayHooks& replay,
        RecordingInfo& info
    ) {
        if (input.bytes == nullptr || !demo::demo_recognised({input.bytes, input.byte_count}))
            return false;
        const std::string name = input.name != nullptr ? input.name : "";
        auto& play = NetworkPlay::of(runtime);
        if (play.demo_)
            throw std::runtime_error(name + ": a recording is already being replayed");
        netgame_context().staged_recording = StagedRecording{
            name, std::vector<uint8_t>(input.bytes, input.bytes + input.byte_count), input.strict
        };
        try {
            play.start_demo_playback();
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(name + ": " + error.what());
        }
        const auto& session = *play.demo_;
        // The end stays unknown (0): the packet layer spreads the last
        // frame's records over up to 30 ticks after it is due, behind those
        // still queued, so a recording finishes some tens of ticks after its
        // last frame is due, by an amount only the replay tells.
        info.expected_end_tick = 0;
        info.duration_ms = demo::demo_duration_ms(session.playback);
        // A viewer with no slot of its own looks through the first recorded player's.
        info.viewer_player = session.watcher_slot < OA_PLAYER_COUNT ? session.watcher_slot : 0;
        info.player_count = static_cast<uint8_t>(session.playback.demo.players.size());
        info.content_differs = play.demo_unit_table_differs_;
        // Filled by position: ReplayHooks::step shares its name with a
        // private member of Runtime, whose uses here may not grow.
        replay = ReplayHooks{&play, replay_tick, replay_status, replay_close};
        return true;
    }

    /// Runs the replay's next tick (ReplayHooks::step).
    ///
    /// A tick that throws counts as a failed tick: in the replay's errors,
    /// with its message as the last error, and the replay goes on.
    ///
    /// @param context Network play's state for the running app.
    /// @return False once the replay has ended: its match is no longer the running one.
    static bool replay_tick(void* context) noexcept {
        auto& play = *static_cast<NetworkPlay*>(context);
        try {
            play.step_demo_frame();
        } catch (const std::exception& error) {
            if (play.demo_) {
                ++play.demo_->tick_errors;
                try {
                    play.demo_->last_error = error.what();
                } catch (...) {
                    play.demo_->last_error.clear();
                }
            }
            report_caught("replay step", error.what());
        }
        return play.demo_ != nullptr;
    }

    /// Tells where the replay stands (ReplayHooks::status).
    ///
    /// Errors count the records that failed, the ticks that failed, the
    /// creates refused and, while the unit table is the recording's, the
    /// creates past the table; the last error is the last failed tick's.
    ///
    /// @param context Network play's state for the running app.
    /// @param[out] status Receives the replay's state; once the replay has
    ///             ended, only a last error that says so, and when the state
    ///             cannot be read, only a last error that says that.
    static void replay_status(void* context, RecordingStatus& status) noexcept {
        try {
            read_replay_status(*static_cast<const NetworkPlay*>(context), status);
        } catch (const std::exception& error) {
            status = {};
            status.last_error = kReplayStatusUnread;
            report_caught("replay status", error.what());
        }
    }

    /// Reads where the replay stands (replay_status).
    ///
    /// @param play Network play's state for the running app.
    /// @param[out] status Receives the replay's state.
    static void read_replay_status(const NetworkPlay& play, RecordingStatus& status) {
        const auto* session = play.demo_.get();
        if (session == nullptr || session->match == nullptr) {
            status.last_error = kReplayEnded;
            return;
        }
        const bool differs = play.demo_unit_table_differs_;
        const auto verdict = demo::demo_session_verdict(*session, differs);
        status.tick = session->match->state().game.tick;
        status.finished = demo::demo_session_finished(*session);
        status.clean = verdict.clean;
        status.paced = verdict.paced;
        status.errors = session->net.record_errors + session->tick_errors +
                        session->binding.refused_creates +
                        (differs ? 0u : session->binding.creates_past_table);
        status.last_error = session->last_error.empty() ? nullptr : session->last_error.c_str();
    }

    /// Ends the replay and frees its playback; the match stays the engine's (ReplayHooks::close).
    ///
    /// @param context Network play's state for the running app.
    static void replay_close(void* context) noexcept {
        static_cast<NetworkPlay*>(context)->demo_.reset();
    }

    /// Runs --check-recording-hook: hands open_recording bytes that are no
    /// recording, which it must decline, and a recording's truncated header,
    /// which it must refuse, then with --play-demo that recording, which it
    /// replays through the replay hooks it returns.
    ///
    /// The recording is replayed until it has finished or `ticks` ran, as a
    /// headless --play-demo run replays it, with the unit table checked
    /// unless --demo-unit-table ignore is given, and closed. Standard output
    /// reports each step, and the replay's end with its digest and status.
    ///
    /// @param runtime The running app, headless, with no match running.
    /// @param ticks Most ticks to replay.
    /// @return 0 when the replay was clean and paced, or without --play-demo;
    ///         1 otherwise. A hook that answers wrongly throws std::runtime_error.
    static int check_recording_hook(Runtime& runtime, std::size_t ticks) {
        const auto open = [&runtime](
                              const char* name,
                              std::span<const uint8_t> bytes,
                              bool strict,
                              ReplayHooks& replay,
                              RecordingInfo& info
                          ) {
            const RecordingInput input{name, bytes.data(), bytes.size(), strict};
            return open_recording(&netgame_context(), runtime, input, replay, info);
        };
        const auto& play = NetworkPlay::of(runtime);
        ReplayHooks replay{};
        RecordingInfo info{};
        const std::span<const uint8_t> other{
            reinterpret_cast<const uint8_t*>(kCheckOtherBytes.data()), kCheckOtherBytes.size()
        };
        if (open(kCheckOtherName, other, true, replay, info) || play.demo_ ||
            replay.context != nullptr)
            throw std::runtime_error(
                "recording hook check: bytes that are no recording were taken"
            );
        std::cout << "recording hook: " << kCheckOtherName << " is declined\n";

        const auto truncated = truncated_recording_header();
        std::string refusal;
        try {
            (void)open(kCheckTruncatedName, truncated, true, replay, info);
        } catch (const std::runtime_error& error) {
            refusal = error.what();
        }
        if (refusal.empty() || play.demo_ || replay.context != nullptr)
            throw std::runtime_error(
                "recording hook check: a truncated recording header was not refused"
            );
        std::cout << "recording hook: " << refusal << '\n';

        const auto& options = net_options();
        if (options.play_demo.empty())
            return 0;
        const auto name = path_to_utf8(options.play_demo.filename());
        const auto bytes = read_file(options.play_demo);
        if (!open(name.c_str(), bytes, !options.demo_ignore_unit_table, replay, info))
            throw std::runtime_error("recording hook check: " + name + " was declined");
        std::printf(
            "recording hook: %s opened: %llu ms, viewer player %u, %u players%s\n",
            name.c_str(),
            static_cast<unsigned long long>(info.duration_ms),
            static_cast<unsigned>(info.viewer_player),
            static_cast<unsigned>(info.player_count),
            info.content_differs ? ", unit definitions differ" : ""
        );
        // Bound by position, as open_recording fills them.
        const auto [replay_context, run_tick, report, finish] = replay;
        RecordingStatus status{};
        report(replay_context, status);
        for (std::size_t tick = 0; tick < ticks && !status.finished; ++tick) {
            if (!run_tick(replay_context))
                break;
            status = {};
            report(replay_context, status);
        }
        const auto* world = play.demo_ ? &play.demo_->match->state() : nullptr;
        std::printf(
            "recording hook tick %u: digest %016llx, %s, %s, %s, %u errors%s%s\n",
            status.tick,
            static_cast<unsigned long long>(demo::demo_world_digest(world)),
            status.finished ? "finished" : "not finished",
            status.clean ? "clean" : "not clean",
            status.paced ? "paced" : "not paced",
            status.errors,
            status.last_error != nullptr ? "; last error: " : "",
            status.last_error != nullptr ? status.last_error : ""
        );
        std::fflush(stdout);
        finish(replay_context);
        if (play.demo_)
            throw std::runtime_error("recording hook check: the replay's close left its playback");
        return status.clean && status.paced ? 0 : 1;
    }

    /// Starts --play-demo playback as the first scene (Extension::start_scene).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @return False without --play-demo, leaving the start to the menu music.
    static bool start_scene(void* /*context*/, Runtime& runtime) {
        if (net_options().play_demo.empty())
            return false;
        NetworkPlay::of(runtime).start_demo_playback();
        return true;
    }

    /// Leaves any network match once the main loop ends (Extension::shutdown).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    static void shutdown(void* /*context*/, Runtime& runtime) {
        NetworkPlay::of(runtime).net_leave();
    }

    /// Runs network play's part of --check-multiplayer-menu (Extension::check_multiplayer_menu).
    ///
    /// An extension built on network play runs its part after it.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    static void check_multiplayer_menu(void* /*context*/, [[maybe_unused]] Runtime& runtime) {
        if constexpr (self_checks_built)
            check_multiplayer_screens(check_host(runtime), runtime_options(runtime));
    }

    /// Reports the running game's extension_state bits (Extension::state).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @return multiplayer while a session is open, shared_match while a network
    ///         match is active, replay during a demo and local_watcher when the local
    ///         player of the network match only watches.
    static uint32_t state(void* /*context*/, const Runtime& runtime) {
        uint32_t bits = 0;
        const auto* play = NetworkPlay::find(runtime);
        if (play == nullptr)
            return bits;
        if (play->net_session_open())
            bits |= extension_state::multiplayer;
        if (play->net_match_active())
            bits |= extension_state::shared_match;
        if (play->demo_)
            bits |= extension_state::replay;
        if (play->net_local_watcher())
            bits |= extension_state::local_watcher;
        return bits;
    }

    /// Runs network play's work of one frame stage (Extension::frame).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param stage pump follows the game into its close handler (and runs
    ///              --check-host-not-found's part of the frame and the part
    ///              that follows --host's and --join's game), binds the
    ///              profile's rules again once Developer Mode changes them
    ///              and Unicode chat as it is now, then runs the network
    ///              match's frame; after_pump applies
    ///              the demo's recorded speed; presented does nothing.
    static void frame(void* /*context*/, Runtime& runtime, FrameStage stage) {
        if (stage == FrameStage::pump) {
            observe_close_handlers(runtime);
            if (net_options().check_host_not_found)
                host_not_found_frame();
            direct_game_frame();
            // The rules Developer Mode lays over the profile reach the
            // multiplayer screens and the session.
            NetworkPlay::of(runtime).follow_profile_rules();
            NetworkPlay::of(runtime).follow_unicode_chat();
            NetworkPlay::of(runtime).net_frame();
        } else if (stage == FrameStage::after_pump) {
            NetworkPlay::of(runtime).demo_frame();
        }
    }

    /// Runs a pending simulation step through the network binding, or as the demo's next tick (Extension::simulation_step).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @return False when neither runs, leaving the step to the engine.
    static bool simulation_step(void* /*context*/, Runtime& runtime) {
        auto& play = NetworkPlay::of(runtime);
        if (play.net_simulation_step())
            return true;
        if (!play.demo_)
            return false;
        play.step_demo_frame();
        return true;
    }

    /// Holds a finished network game on its outcome until the final economy is settled (Extension::outcome_ready).
    ///
    /// The match clock keeps stepping a shared match meanwhile, so its ticks
    /// and their records (0x2c, 0x28, 0x16) go on through simulation_step.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @return NetworkPlay::net_final_economy_settled().
    static bool outcome_ready(void* /*context*/, Runtime& runtime) {
        return NetworkPlay::of(runtime).net_final_economy_settled();
    }

    /// Follows the match's life cycle (Extension::match_event).
    ///
    /// A finished match leaves the session once its final economy has
    /// settled, before the end-of-game screen opens: closing the transport
    /// tells the other machines, and no record announces the leave. The
    /// match report stays up for the end-of-game event the screen sends. A
    /// torn-down match leaves the same way and also ends a demo.
    /// Leaving the match or releasing its results closes the match report; reported
    /// results and a defeated player who keeps watching are reported as game
    /// events. A match that finishes, is left or is torn down ends the match
    /// mode of the close handler.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param event What happened to the match.
    static void match_event(void* /*context*/, Runtime& runtime, MatchEvent event) {
        auto& play = NetworkPlay::of(runtime);
        switch (event) {
        case MatchEvent::finished:
            // The transport closes now; the match report stays up for the
            // end-of-game event the screen sends.
            play.net_leave();
            close_handlers_match_ended(g_close);
            break;
        case MatchEvent::torn_down:
            play.net_leave();
            play.demo_.reset();
            close_handlers_match_ended(g_close);
            break;
        case MatchEvent::left:
            play.close_reporter();
            close_handlers_match_ended(g_close);
            break;
        case MatchEvent::results_released:
            play.close_reporter();
            break;
        case MatchEvent::results_reported:
            play.report_game_event(report_event::game_ended);
            break;
        case MatchEvent::watching_kept:
            play.report_game_event(report_event::player_changed);
            break;
        }
    }

    /// Gives a console resource gift in a network match or a demo (Extension::give_resources).
    ///
    /// A network match sends the gift through its session; a demo only watches,
    /// so nothing is given.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param from Giving player index.
    /// @param to Receiving player index.
    /// @param amount Amount given.
    /// @param metal True for metal, false for energy.
    /// @return True during a demo or a network match, which dealt with the gift.
    static bool give_resources(
        void* /*context*/, Runtime& runtime, uint8_t from, uint8_t to, float amount, bool metal
    ) {
        auto& play = NetworkPlay::of(runtime);
        return play.demo_ || play.net_give(from, to, metal, amount);
    }

    /// Adds network play's message-log hooks (Extension::message_hooks).
    ///
    /// Chat lines go to every player and to the match report, and an open
    /// session makes the game kind multiplayer.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app (unused; the hooks' context is the runtime).
    /// @param[in,out] hooks Message-log hooks to fill.
    static void
    message_hooks(void* /*context*/, Runtime& /*runtime*/, oa::sim::messages::Hooks& hooks) {
        hooks.share_chat = [](void* context, const char* chat) {
            NetworkPlay::of(*static_cast<Runtime*>(context)).net_send_chat(chat);
        };
        hooks.shared_chat_line =
            [](void* context, const char* chat, std::size_t index) -> const char* {
            return NetworkPlay::of(*static_cast<Runtime*>(context)).shared_chat_line(chat, index);
        };
        hooks.shared_chat_line_bytes = [](void* context) -> std::size_t {
            return NetworkPlay::of(*static_cast<Runtime*>(context)).shared_chat_line_bytes();
        };
        hooks.record_chat = [](void* context, const char* chat) {
            NetworkPlay::of(*static_cast<Runtime*>(context)).report_chat_line(chat);
        };
        hooks.game_kind = [](void* context) -> int32_t {
            const auto* play = NetworkPlay::find(*static_cast<const Runtime*>(context));
            return play != nullptr && play->net_session_open()
                       ? oa::sim::messages::game_kind_multiplayer
                       : 0;
        };
    }

    /// Announces a player leaving a network game (Extension::player_gone).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param[in,out] world Match world the departure is posted to.
    /// @param player Player that lost its last unit.
    /// @return False without an open session, leaving the announcement to the engine.
    static bool
    player_gone(void* /*context*/, Runtime& runtime, oa::World& world, const oa::Player& player) {
        auto& play = NetworkPlay::of(runtime);
        if (!play.net_session_open())
            return false;
        play.post_departure(world, player.player_id);
        return true;
    }

    /// Fills network play's part of the in-game console's host (Extension::console_host).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param[in,out] host Console host that gains the network commands.
    static void
    console_host(void* /*context*/, Runtime& runtime, oa::ui::console::ConsoleHost& host) {
        NetworkPlay::of(runtime).bind_console_network_hooks(host);
    }

    /// Runs the console network command check in --check-navigation's console check (Extension::check_console).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param enter_line Types one line into the console.
    /// @param user Context of `enter_line`.
    static void check_console(
        void* /*context*/,
        Runtime& runtime,
        void (*enter_line)(void* user, const char* line),
        void* user
    ) {
        NetworkPlay::of(runtime).check_console_network_commands(
            [enter_line, user](const char* line) { enter_line(user, line); }
        );
    }

    /// Draws a network load's player bars over the loading screen (Extension::draw_loading).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param[in,out] target Loading screen's locked surface.
    /// @param font Font of the names and the status text.
    static void draw_loading(
        void* /*context*/,
        Runtime& runtime,
        oa::Surface& target,
        const oa::present::GafSprites* font
    ) {
        NetworkPlay::of(runtime).draw_loading_players(target, font);
    }

    /// Shares the Pause key's pause with the other players (Extension::pause_changed).
    ///
    /// While a loaded network match runs, the bit the engine has just flipped
    /// goes to every player as 0x19 {kind 0, value} from the first player on
    /// this machine; a pause the other machines set arrives the same way. Before
    /// the match has loaded, or with no network match, the pause stays on this
    /// machine.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param paused The pause bit after the flip.
    static void pause_changed(void* /*context*/, Runtime& runtime, bool paused) {
        auto* state = net_of(runtime);
        if (state == nullptr || !state->active || state->loading)
            return;
        NetworkPlay::of(runtime).net_pause_key(paused);
    }

    /// Shares a speed the local player set with the other players (Extension::speed_changed).
    ///
    /// While a loaded network match runs, a speed the speed keys or the GAME
    /// slider have just set goes to every player as 0x19 {kind 1, speed}
    /// from the first player on this machine, never from a watcher's
    /// machine; a speed another machine sets arrives the same way, and the
    /// frame takes it in (NetworkPlay::net_frame). The preferences' Cancel, UNDO
    /// and RESTORE put a speed back without the engine reporting it, so it
    /// stays on this machine. Before the match has loaded, or with no
    /// network match, the speed stays on this machine too.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param speed The match's game speed now, 1 to 20.
    static void speed_changed(void* /*context*/, Runtime& runtime, uint16_t speed) noexcept {
        const auto* play = NetworkPlay::find(runtime);
        auto* state = play != nullptr ? play->net_.get() : nullptr;
        if (state == nullptr || !state->active || state->loading || play->net_local_watcher())
            return;
        oa::netgame::match::net_match_set_speed(state->net.get(), speed, true);
        state->speed_seen = speed;
    }

    /// Keeps a network launch's connection alive while its world is built (Extension::load_progress).
    ///
    /// While a launch builds its world, the rows are kept for the load
    /// progress records, and at most every 200 ms (the loading screen's pace)
    /// the connection is serviced and each local or computer player of the
    /// battle room sends a probe and its load progress, the mean of the rows
    /// (net_match_building_frame). The first such frame also sends the host's
    /// queued start record. Received frames stay queued for the match. Other
    /// loads change nothing.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app.
    /// @param rows The loading screen's rows, each 0 to 100 percent.
    /// @param row_count How many rows `rows` holds.
    static void
    load_progress(void* /*context*/, Runtime& runtime, const uint8_t* rows, size_t row_count) {
        auto* state = net_of(runtime);
        if (state == nullptr || state->building_game == nullptr || rows == nullptr)
            return;
        std::copy_n(rows, std::min(row_count, state->load_rows.size()), state->load_rows.begin());
        if (!oa::netgame::match::loading_frame_due(
                &state->loading_pace, oa::netgame::match::net_connection_time(&state->connection)
            ))
            return;
        oa::netgame::match::net_match_building_frame(
            &state->connection, *state->building_game, state->load_rows.data()
        );
    }

    /// Returns the loaded network match a team panel's change goes to.
    ///
    /// @param context The panels' host context: the running app.
    /// @return The match, or null before it has loaded or without one.
    static oa::netgame::match::NetMatch* panel_match(void* context) {
        auto* state = net_of(*static_cast<const Runtime*>(context));
        return state != nullptr && state->active && !state->loading ? state->net.get() : nullptr;
    }

    /// Fills what the in-game team panels tell the other players' machines (Extension::team_panel_host).
    ///
    /// While a loaded network match runs: an ALLIES.GUI alliance change goes
    /// to that player's machine as 0x23; a changed setup block (allied
    /// victory, watching allowed) goes out as every local player's 0x20 and
    /// 0x24; CONTROL.GUI's removals go out as 0x1b with their reason and
    /// retire the player; its OK publishes the hosted session again;
    /// SHARE.GUI's gifts of metal and energy go to the receiver's machine as
    /// 0x16 subtype 2 or 1 when both players still take part, and its map
    /// as 0x16 subtype 3. Its gift of units reaches the other machines through
    /// the match (a 0x14 for each unit). Without a loaded match, nothing is
    /// told. A game the launch marks as a tournament game offers no
    /// CONTROL.GUI.
    ///
    /// @param context Extension context (unused).
    /// @param runtime The running app (unused; the host's context is the runtime).
    /// @param[in,out] host The panels' host to fill.
    static void
    team_panel_host(void* /*context*/, Runtime& /*runtime*/, oa::ui::hud::TeamPanelHost& host) {
        namespace nm = oa::netgame::match;
        host.alliance_changed = [](void* context, uint8_t from, uint8_t to, uint8_t allied) {
            if (auto* net = panel_match(context))
                nm::net_match_send_alliance(net, from, to, allied);
        };
        host.setup_changed = [](void* context) {
            if (auto* net = panel_match(context))
                nm::net_match_send_player_status(net, false);
        };
        host.remove_player = [](void* context, uint8_t player, uint8_t reason) {
            if (auto* net = panel_match(context))
                nm::net_match_remove_player(net, player, reason);
        };
        host.game_changed = [](void* context) {
            if (auto* net = panel_match(context))
                nm::net_match_republish(net);
        };
        host.resources_given =
            [](void* context, uint8_t from, uint8_t to, bool metal, float amount) {
                if (auto* net = panel_match(context))
                    nm::net_match_send_give(net, from, to, metal, amount);
            };
        host.sight_shared = [](void* context, uint8_t from, uint8_t to) {
            if (auto* net = panel_match(context))
                nm::net_match_share_sight(net, from, to);
        };
        host.tournament_game = [](void* /*context*/) { return launch_tournament(); };
    }

    /// Frees network play's state for a runtime that is being destroyed (Extension::release_runtime).
    ///
    /// @param context Extension context (unused).
    /// @param runtime The runtime being destroyed.
    static void release_runtime(void* /*context*/, Runtime& runtime) noexcept {
        NetworkPlay::free_for(runtime);
    }

    /// Draws the traffic readouts over the battlefield (Extension::draw_match_overlay).
    ///
    /// The rates come from the connection's counters while a network match
    /// owns it, and read 0 otherwise.
    ///
    /// @param context Extension context (unused)
    /// @param runtime the running app
    /// @param overlay the battlefield and its painter
    static void
    draw_match_overlay(void* /*context*/, Runtime& runtime, const MatchOverlay& overlay) {
        auto* state = net_of(runtime);
        if (state == nullptr || !state->active || state->connection.packets == nullptr) {
            draw_traffic_overlay(overlay, nullptr, 0);
            return;
        }
        draw_traffic_overlay(
            overlay,
            &state->connection.packets->traffic,
            oa::netgame::match::net_connection_time(&state->connection)
        );
    }
};

} // namespace oa::app

/// Fills network play's extension table (its oa_add_extension INIT).
///
/// @param[out] table network play's table, zeroed on entry
void oa_extension_init_netgame(oa::app::Extension* table) {
    using oa::app::RuntimeExtension;
    *table = {};
    oa::app::fill_option_hooks(*table, oa::app::netgame_context());
    // The launch block, the switches and the close handlers exist from here:
    // an extension built on network play reaches them from its own init.
    oa::app::bind_launch(oa::app::netgame_launch_binding());
    table->register_screens = oa::app::register_screens;
    table->ready = RuntimeExtension::ready;
    table->run_mode = RuntimeExtension::run_mode;
    table->start_scene = RuntimeExtension::start_scene;
    table->shutdown = RuntimeExtension::shutdown;
    table->select_multiplayer = oa::app::select_multiplayer;
    table->frontend_game = oa::app::frontend_game;
    table->check_multiplayer_menu = RuntimeExtension::check_multiplayer_menu;
    table->state = RuntimeExtension::state;
    table->frame = RuntimeExtension::frame;
    table->simulation_step = RuntimeExtension::simulation_step;
    table->outcome_ready = RuntimeExtension::outcome_ready;
    table->match_event = RuntimeExtension::match_event;
    table->disconnect_text = oa::app::disconnect_text;
    table->give_resources = RuntimeExtension::give_resources;
    table->message_hooks = RuntimeExtension::message_hooks;
    table->player_gone = RuntimeExtension::player_gone;
    table->console_host = RuntimeExtension::console_host;
    table->check_console = RuntimeExtension::check_console;
    table->draw_loading = RuntimeExtension::draw_loading;
    table->draw_match_overlay = RuntimeExtension::draw_match_overlay;
    table->pause_changed = RuntimeExtension::pause_changed;
    table->load_progress = RuntimeExtension::load_progress;
    table->team_panel_host = RuntimeExtension::team_panel_host;
    table->close_requested = RuntimeExtension::close_requested;
    table->speed_changed = RuntimeExtension::speed_changed;
    table->app_mode_set = RuntimeExtension::app_mode_set;
    table->open_recording = RuntimeExtension::open_recording;
    table->release_runtime = RuntimeExtension::release_runtime;
}
