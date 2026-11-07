// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The MCP server's extension, filled by oa_extension_init_mcp. Without
// --mcp the hooks do not read standard input or write standard output.
// Without --mcp-http they open no socket. With either, startup refuses a
// run whose options own the main loop. The pump reads the client and
// pushes input. The presented stage finishes a picture of that frame.
// The hooks reach the game through the automation host, the play host and
// the match view. The unit tools only read. With --give-orders, the pump
// gives a skirmish the save/load orders once before those tools read.
// game.speed, game.pause, the camera moves, the clock tools, orders and
// selection are applied at after_pump, through the play host, so they land
// before the match clock and the keys' hooks run. --mcp-clock stepped
// offers that clock once when a match starts. Both transports share it, and
// a step from either client runs it. A stepped clock holds an order until a
// step runs it. game.quit asks the automation host to leave. game.status and
// camera.get only read. The screen tools drive input and do not touch the
// simulation.
#include "oa/app/mcp/http.hpp"
#include "oa/app/mcp/options.hpp"
#include "oa/app/mcp/session.hpp"
#include "oa/app/mcp_badge.hpp"
#include "event_feed.hpp"
#include "stdio.hpp"
#include "map.hpp"
#include "tools.hpp"
#include "units.hpp"

#include "oa/app/app.hpp"
#include "oa/app/automation_host.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/extension.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/match_view.hpp"
#include "oa/app/play_host.hpp"
#include "oa/app/presence_facts.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/log_files.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the engine's version, which initialize gives"
#endif

// The version of the engine's extension table these hooks follow. A build
// against a table of another version stops here until the engine's change
// has been read and the hooks follow it. Version 19 is Kit 2's look in the
// kit's headers, which these hooks do not use. Version 20 adds
// ExtensionControl's masked, text_length, scroll_up and scroll_down members,
// and the calls answer_multiplayer_entry, defer_inactive_loop_release,
// post_extension_note and take_extension_notes. None is a hook. Version 21
// adds accessibility fields on controls. It is not a hook, and these hooks
// do not fill them.
// Version 21 also adds the replay viewer's eight hooks, which these hooks
// do not fill. Version 21 also adds the rectangle where an extension
// window's art takes the press. It is not a hook, and these hooks do not
// state one.
constexpr uint32_t kExtensionApiVersionFollowed = 21;
static_assert(
    oa::app::extension_api_version == kExtensionApiVersionFollowed,
    "the engine's extension table changed: follow its change, then raise "
    "kExtensionApiVersionFollowed"
);

namespace oa::app::mcp {
namespace {

/// What this process serves, for every runtime it builds.
struct McpContext {
    McpOptions options{};
    Stdio stdio{};
    HttpServer http{};
    Serve serve{};
    UnitHost units{};
    PlayBridge bridge{};
    EventFeed feed{};
    std::optional<Session> session{};
    std::optional<Session> http_session{};
    bool quit_sent{};
};

/// Returns the process's MCP context.
///
/// @return the one context, made on first use
McpContext& mcp_context() {
    static McpContext context;
    return context;
}

/// Tells whether an act is logged when it lands, with the tick, rather than when it is called.
///
/// @param name the tool
/// @return true for a tool the queue applies
bool logged_when_it_lands(std::string_view name) {
    return name == "game.speed" || name == "game.pause" || name == "camera.set" ||
           name == "camera.center" || name == "camera.zoom" || name == "orders.issue" ||
           name == "orders.state" || name == "units.select" || name == "clock.mode" ||
           name == "clock.step" || name == "clock.run";
}

/// Writes an act to the log. A tool that only reads is not an act.
///
/// @param name the tool
/// @param reads true when the tool only reads
void log_act(void* /*context*/, std::string_view name, bool reads) {
    // game.speed, game.pause, the camera moves, orders, selection and the
    // clock tools are logged when they land, with the tick.
    if (reads || logged_when_it_lands(name))
        return;
    oa::platform::log_files::write_line(std::string("mcp: ") + std::string(name));
}

/// The session both transports start from.
///
/// @param mcp the context
/// @return the configuration
SessionConfig session_config(const McpContext& mcp) {
    SessionConfig config;
    config.server_name = "open-annihilation";
    config.server_version = OA_ENGINE_VERSION;
    config.observe = mcp.options.observe;
    config.instructions =
        "game.status reads the running match. game.info reads the engine version, the mod, the "
        "hacks in force, the map and the rules. game.speed sets its speed and game.pause pauses "
        "it, both as the keys do; a replay and a watcher are refused, and the answer names the "
        "tick the change landed on. clock.mode sets a realtime or stepped clock. clock.step and "
        "clock.run advance a stepped clock, and a shared match refuses them. Both transports "
        "share that one clock. orders.issue gives orders, orders.state sets standing orders and "
        "units.select selects, each for this machine's units only; a replay and a watcher are "
        "refused, and the answer names the tick. A stepped clock holds an order until a step "
        "runs it. units.list, units.get, units.orders and units.types read "
        "the units, their orders and their types. map.info reads the map. "
        "map.visibility reads one player's sight, explored ground, radar, sonar and jammed cells. "
        "map.radar reads that player's contacts, without identity. camera.get reads the view. "
        "camera.set, camera.center and camera.zoom move it, and the view's limits hold the place "
        "and the zoom; a replay and a watcher may move the view. game.quit leaves the game. "
        "ui.screen and ui.tree read the screen and its controls by name. "
        "ui.click, ui.select, ui.type, ui.key, ui.pointer, ui.scroll and ui.drag "
        "drive a control by its name. ui.wait waits for a screen, a control, a text "
        "or an event, and does not stop the frame. capture.screenshot returns a PNG "
        "of the frame. events.subscribe chooses the events this session is sent, and "
        "events.read returns the last 4096, which oa://events also holds. A note's "
        "tick is the match's. With --mcp=observe only tools that read are listed.";
    config.on_call = log_act;
    return config;
}

/// Asks the game to leave, once.
///
/// @param runtime the running game
/// @param[in,out] mcp the context
void request_quit(Runtime& runtime, McpContext& mcp) {
    if (mcp.quit_sent)
        return;
    mcp.quit_sent = true;
    const AutomationHost host = automation_host(runtime);
    if (host.quit != nullptr)
        host.quit(host.context);
}

/// Opens one session and gives it the game, unit, map, screen and event tools.
///
/// The game tools include the camera and the clock. Both sessions are given
/// the one play bridge, so they share one queue and one clock, and the one
/// event feed.
///
/// @param[in,out] session the session
/// @param[in,out] mcp the context
/// @param http true when this is the HTTP session
void open_session(Session& session, McpContext& mcp, bool http) {
    add_game_tools(session, mcp.serve, mcp.bridge);
    add_unit_tools(session, mcp.units);
    add_map_tools(session, mcp.units);
    add_ui_tools(session, mcp.serve);
    add_event_tools(session, mcp.feed, http);
}

/// Takes the match's notes and the screen's events into the feed.
///
/// The match watch is listening before the notes are pulled, and radar
/// contacts gained since the last sample are noted at the match's tick.
///
/// @param runtime the running game
/// @param[in,out] mcp the context
void collect_events(Runtime& runtime, McpContext& mcp) {
    begin_match_watch(runtime);
    sample_match_contacts(runtime);
    std::vector<MatchNote> notes;
    pull_match_notes(&notes);
    mcp.feed.take_notes(notes);
    const MatchView table = match_view(runtime);
    const uint32_t tick = table.tick != nullptr ? table.tick(table.context) : 0;
    const CheckHost check = check_host(runtime);
    const AutomationHost automation = automation_host(runtime);
    mcp.feed.gather_screen(check, automation, mcp.serve.frame, tick);
    mcp.feed.flush_resource();
}

/// Opens the sessions as a runtime starts (Extension::startup).
///
/// A run whose options run no main loop, or own it, is refused. Each
/// session then lives as long as the process, so a client stays connected
/// through a switch of the mod. --mcp-http opens its listener here and
/// nowhere else. Closing that client does not leave the game.
///
/// @param runtime the runtime being built
void startup(void* /*context*/, Runtime& runtime) {
    McpContext& mcp = mcp_context();
    const bool stdio = mcp.options.enabled;
    const bool http = mcp.options.http;
    if (!stdio && !http)
        return;
    keep_running_while_inactive(runtime, true);
    const bool need_stdio = stdio && !mcp.session;
    const bool need_http = http && !mcp.http.listening();
    if (!need_stdio && !need_http)
        return;
    if (const std::string_view conflict = conflicting_option(runtime_options(runtime));
        !conflict.empty()) {
        const char* flag = stdio ? "--mcp" : "--mcp-http";
        throw std::runtime_error(
            std::string(flag) + " cannot be used with " + std::string(conflict) +
            ": the MCP server is served from the game's main loop"
        );
    }
    if (need_stdio) {
        if (!stdio_captured())
            throw std::runtime_error("--mcp could not keep standard input and output");
        mcp.session.emplace(session_config(mcp));
        open_session(*mcp.session, mcp, false);
        oa::platform::log_files::write_line(
            mcp.options.observe ? "mcp: serving on standard input and output, tools that read only"
                                : "mcp: serving on standard input and output"
        );
    }
    if (need_http) {
        const auto& run = runtime_options(runtime);
        const std::filesystem::path file =
            preference_file(run.preferences_file).parent_path() / std::string(token_file_name);
        mcp.http.open(mcp.options.http_listen.family, mcp.options.http_listen.port, file);
        mcp.http_session.emplace(session_config(mcp));
        open_session(*mcp.http_session, mcp, true);
        const std::string url(mcp.http.url());
        oa::platform::log_files::write_line(
            mcp.options.observe ? "mcp: serving on " + url + ", tools that read only"
                                : "mcp: serving on " + url
        );
        oa::platform::log_files::write_line(
            "mcp: the address and token are in " + oa::platform::path_to_utf8(file)
        );
    }
}

/// Offers --mcp-clock once for the match on screen, before queued changes run.
///
/// The host is read and the runtime pointer cleared before the offer, which
/// keeps its own pointer. A realtime command line still calls, so a clock
/// left stepped by a match that has ended returns to the wall clock.
///
/// @param[in,out] runtime the running game
/// @param[in,out] mcp the context
void offer_startup_clock(Runtime& runtime, McpContext& mcp) {
    mcp.serve.runtime = &runtime;
    PlayHost host;
    if (mcp.bridge.host_of != nullptr)
        host = mcp.bridge.host_of(mcp.bridge.context);
    mcp.serve.runtime = nullptr;
    if (host.offer_startup_clock == nullptr)
        return;
    const uint8_t mode =
        mcp.options.clock == McpClock::stepped ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    host.offer_startup_clock(host.context, mode);
}

/// Applies the queued speed, pause, camera, clock, order and selection changes, and logs each one with its tick.
///
/// The runtime is set for the host and cleared before the log lines, which
/// do not need it. Nothing is done when the queue is empty, so a frame that
/// only reads does not ask the host.
///
/// @param[in,out] runtime the running game
/// @param[in,out] mcp the context
void apply_queued(Runtime& runtime, McpContext& mcp) {
    if (!mcp.bridge.queue.waiting())
        return;
    mcp.serve.runtime = &runtime;
    PlayHost host;
    if (mcp.bridge.host_of != nullptr)
        host = mcp.bridge.host_of(mcp.bridge.context);
    const std::vector<AppliedAct> landed = mcp.bridge.queue.apply(host);
    mcp.serve.runtime = nullptr;
    for (const AppliedAct& act : landed)
        oa::platform::log_files::write_line("mcp: " + std::to_string(act.tick) + " " + act.detail);
}

/// Copies the engine version, the mod, the hacks and the mutators onto the tools.
///
/// The tools read these copies, so a unit test of the tools does not link
/// the game. game_type is left unset: a running match has no launcher mode name.
///
/// @param[in,out] serve the tools' game
/// @param runtime the running game
void note_profile(Serve& serve, const Runtime& runtime) {
    serve.reading.engine_version = OA_ENGINE_VERSION;
    serve.reading.profile = match_profile_reading(runtime);
}

/// Copies the match view's full-awareness rule onto the tools.
///
/// @param[in,out] serve the tools' game
/// @param table the view published for this frame
void note_awareness(Serve& serve, const MatchView& table) {
    serve.reading.full_awareness_allowed = table.full_awareness_allowed;
    serve.reading.awareness_context = table.context;
}

/// Serves the sessions at the pump and once the frame has been presented.
/// after_pump offers --mcp-clock, then applies the queued speed, pause,
/// camera, clock, order and selection changes (Extension::frame).
///
/// The pump counts the frame, records what the screen tools wait on, gives
/// the save/load orders, reads the match view, then reads each client. The
/// presented stage only polls what is already held, so a picture of the
/// frame just shown can finish, and it delivers on both transports. The
/// clock is held in the match step, after this hook's after_pump. An error
/// is written to the log. The hook does not throw: a throw would abandon
/// the frame.
///
/// @param[in,out] runtime the running game
/// @param stage the stage of the frame
void frame(void* /*context*/, Runtime& runtime, FrameStage stage) {
    McpContext& mcp = mcp_context();
    if (!mcp.session && !mcp.http_session)
        return;
    try {
        if (stage == FrameStage::after_pump) {
            offer_startup_clock(runtime, mcp);
            apply_queued(runtime, mcp);
            return;
        }
        if (stage != FrameStage::pump && stage != FrameStage::presented)
            return;
        // Cleared when the pump returns, including when it throws: a tool
        // must not read the runtime after this frame.
        mcp.serve.runtime = &runtime;

        struct Release {
            McpContext& mcp;

            ~Release() {
                mcp.units.table = {};
                mcp.serve.reading.full_awareness_allowed = nullptr;
                mcp.serve.reading.awareness_context = nullptr;
                mcp.serve.runtime = nullptr;
                mcp.serve.stage = ServeStage::idle;
            }
        } release{mcp};

        note_profile(mcp.serve, runtime);

        if (stage == FrameStage::pump) {
            mcp.serve.stage = ServeStage::pump;
            ++mcp.serve.frame;
            collect_events(runtime, mcp);
            ui_before_pump(mcp.serve);
            // Orders first, so a tool served by this pump sees the factory queue.
            apply_give_orders(runtime);
            mcp.units.table = match_view(runtime);
            note_awareness(mcp.serve, mcp.units.table);
            // A DELETE answered on an earlier pump starts a new HTTP session
            // before this one accepts another client.
            if (mcp.http.take_session_reset()) {
                mcp.feed.http_slot().session = nullptr;
                mcp.http_session.emplace(session_config(mcp));
                open_session(*mcp.http_session, mcp, true);
            }
            if (mcp.session)
                mcp.stdio.pump(*mcp.session);
            if (mcp.http_session)
                mcp.http.pump(*mcp.http_session);
            const bool stdio_gone = mcp.session && mcp.stdio.client_gone();
            const bool leave = mcp.serve.quit || stdio_gone;
            const bool pending =
                (mcp.session && mcp.stdio.response_pending()) || mcp.http.response_pending();
            if (leave && !pending)
                request_quit(runtime, mcp);
        } else {
            mcp.serve.stage = ServeStage::presented;
            collect_events(runtime, mcp);
            mcp.units.table = match_view(runtime);
            note_awareness(mcp.serve, mcp.units.table);
            if (mcp.session)
                mcp.stdio.deliver(*mcp.session);
            if (mcp.http_session)
                mcp.http.deliver(*mcp.http_session);
        }
    } catch (const std::exception& failure) {
        mcp.serve.runtime = nullptr;
        mcp.serve.stage = ServeStage::idle;
        oa::platform::log_files::write_line(std::string("mcp: ") + failure.what());
    } catch (...) {
        mcp.serve.runtime = nullptr;
        mcp.serve.stage = ServeStage::idle;
        oa::platform::log_files::write_line("mcp: the frame could not be served");
    }
}

/// Samples radar contacts once per tick, then lets the engine run the tick.
///
/// With neither transport on, it does nothing. It never runs the tick itself.
///
/// @param runtime the running game
/// @return false, so the engine ticks the match
bool simulation_step(void* /*context*/, Runtime& runtime) {
    McpContext& mcp = mcp_context();
    if (!mcp.session && !mcp.http_session)
        return false;
    try {
        begin_match_watch(runtime);
        sample_match_contacts(runtime);
    } catch (const std::exception& failure) {
        oa::platform::log_files::write_line(std::string("mcp: ") + failure.what());
    } catch (...) {
        oa::platform::log_files::write_line("mcp: the match's contacts could not be read");
    }
    return false;
}

/// Notes the match's life (Extension::match_event).
///
/// @param runtime the running game
/// @param event what happened
void match_event(void* /*context*/, Runtime& runtime, MatchEvent life) {
    McpContext& mcp = mcp_context();
    if (!mcp.session && !mcp.http_session)
        return;
    try {
        note_match_life(runtime, static_cast<uint8_t>(life));
    } catch (const std::exception& failure) {
        oa::platform::log_files::write_line(std::string("mcp: ") + failure.what());
    } catch (...) {
        oa::platform::log_files::write_line("mcp: the match's life could not be noted");
    }
}

} // namespace

std::string_view connected_badge(bool match_on_screen, bool match_stepped) noexcept {
    const McpContext& mcp = mcp_context();
    const bool stdio_up =
        mcp.session.has_value() && mcp.session->initialized() && !mcp.stdio.client_gone();
    const bool http_up = mcp.http_session.has_value() && mcp.http_session->initialized() &&
                         mcp.http.client_connected();
    if (!stdio_up && !http_up)
        return {};
    // On a menu the option is the clock a match will be offered. In a match
    // the badge follows the clock that match accepted, including a later
    // clock.mode and a shared match that stayed on the wall clock.
    const bool stepped = match_on_screen ? match_stepped : mcp.options.clock == McpClock::stepped;
    return stepped ? "MCP · stepped" : "MCP · realtime";
}

} // namespace oa::app::mcp

/// Fills the MCP server's extension table.
///
/// @param[in,out] table the table, zeroed
void oa_extension_init_mcp(oa::app::Extension* table) {
    auto& context = oa::app::mcp::mcp_context();
    oa::app::mcp::fill_option_hooks(*table, context.options);
    table->startup = oa::app::mcp::startup;
    table->frame = oa::app::mcp::frame;
    table->simulation_step = oa::app::mcp::simulation_step;
    table->match_event = oa::app::mcp::match_event;
}
