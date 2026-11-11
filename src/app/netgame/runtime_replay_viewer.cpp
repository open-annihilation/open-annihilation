// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A --replay-viewer join prints the demo summary when the watched game ends,
// and a --headless-check join plays to that end without a window.
#include "oa/app/runtime.hpp"
#include "oa/app/check_host.hpp"
#include "network_play.hpp"
#include "net_options.hpp"
#include "net_state.hpp"

#include "oa/app/netgame/replay_viewer_end.hpp"
#include "oa/base/game_loop.hpp"
#include "oa/core/game_state.h"
#include "oa/netgame/match/net_match.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/session/demo.hpp"
#include "oa/sim/scenario/outcome.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace oa::app {
namespace {

namespace demo = oa::session::demo;
namespace mp = oa::ui::frontend_multiplayer;
namespace nm = oa::netgame::match;
namespace view = oa::app::netgame;

// How long a headless join may take to reach a session before it is given up.
constexpr std::chrono::minutes kReplayViewerJoinWait{3};

// The pause between headless frames, so the match clock can follow the wall
// clock without a busy loop.
constexpr std::chrono::milliseconds kReplayViewerFrameWait{5};

// The connection clock, the same rate the match's timeout scan counts in.
constexpr uint32_t kClockTicksPerSecond = 30;

// Milliseconds since the newest packet from another player, or since the
// timeout scan's baseline when that is later. A pause holds the baseline on
// the present, so a paused match does not count as silence.
uint32_t milliseconds_since_packet(const nm::NetMatch& match) {
    const auto now = nm::net_connection_time(match.connection);
    uint32_t since = oa::base::game_loop::scaled_clock_elapsed(now, match.timeout_baseline);
    if (match.world != nullptr) {
        for (const auto& player : match.world->game.players) {
            if (player.in_use == 0 || player.status != OA_PLAYER_STATUS_MIRRORED)
                continue;
            const auto quiet =
                oa::base::game_loop::scaled_clock_elapsed(now, player.last_update_time);
            if (quiet < since)
                since = quiet;
        }
    }
    const auto milliseconds = static_cast<uint64_t>(since) * 1000u / kClockTicksPerSecond;
    if (milliseconds > 0xffffffffu)
        return 0xffffffffu;
    return static_cast<uint32_t>(milliseconds);
}

void say(const char* line) {
    std::printf("%s\n", line);
    std::fflush(stdout);
}

// Milliseconds from one steady-clock reading to a later one, capped.
uint32_t milliseconds_between(
    std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to
) {
    if (to < from)
        return 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    if (elapsed <= 0)
        return 0;
    if (elapsed > static_cast<std::chrono::milliseconds::rep>(0xffffffffu))
        return 0xffffffffu;
    return static_cast<uint32_t>(elapsed);
}

// The local player has a seat and a session id, and the host has not refused it.
bool viewer_seated(mp::Lobby& lobby) {
    if (lobby.game == nullptr)
        return false;
    const auto& player = mp::local_player(lobby);
    return player.in_use != 0 && player.player_id != 0 && player.player_id != nm::no_player_id &&
           player.reject_reason == 0;
}

// This machine has handled the host's greeting and reported every unit type.
// The host's own view of the sync is not this.
bool viewer_units_reported(const mp::Lobby& lobby) {
    return lobby.sync.records_handled > 0 && lobby.sync.next_unit >= lobby.unit_count;
}

// The battle room's own rule: READY stays off while a guest has not built the
// host's mutator list. The host is never waiting on itself. No mutator field
// on the host means the room has no mutators.
bool viewer_awaits_mutators(mp::Lobby& lobby) noexcept {
    if (mp::lobby_hosts(lobby) || lobby.game == nullptr)
        return false;
    const uint8_t host_slot = mp::lobby_host_slot(lobby);
    if (host_slot == mp::kNoSlot)
        return false;
    const auto host_record = mp::lobby_presence_record(lobby, static_cast<int32_t>(host_slot));
    const auto host_state =
        oa::netgame::presence_mutators_state(host_record.data(), host_record.size());
    if (!host_state)
        return false;
    const auto own_record = mp::lobby_presence_record(lobby, lobby.game->local_player_index);
    const auto own_state =
        oa::netgame::presence_mutators_state(own_record.data(), own_record.size());
    if (!own_state || own_state->digest != host_state->digest)
        return true;
    return !own_state->built;
}

} // namespace

bool NetworkPlay::replay_viewer_session_lost() const {
    if (!net_ || !net_->connected || !replay_viewer_saw_session_)
        return false;
    const auto* engine = net_->connection.session.backend.engine;
    if (engine != nullptr && engine->session_lost)
        return true;
    return !net_->connection.in_session;
}

void NetworkPlay::print_replay_viewer_summary() {
    if (replay_viewer_summary_printed_ || !replay_viewer_received_ || !net_ || !net_->net)
        return;
    const demo::DemoSummary summary =
        demo::demo_summary_of_match(*net_->net, net_->binding, net_->connection);
    std::printf("%s\n", demo::demo_summary_line(summary).c_str());
    std::fflush(stdout);
    replay_viewer_summary_printed_ = true;
    replay_viewer_ended_ = true;
}

void NetworkPlay::note_replay_viewer_match() {
    if (!net_ || !net_->active || net_->loading || !net_->net || net_->net->world == nullptr)
        return;
    const auto& played = *net_->net;
    if (played.records_applied > 0 || net_->binding.created_remote > 0)
        replay_viewer_received_ = true;
    if (!replay_viewer_host_known_) {
        const uint32_t host_id = nm::net_match_host_id(&played);
        if (host_id != nm::no_player_id) {
            replay_viewer_host_id_ = host_id;
            replay_viewer_host_known_ = true;
        }
    }
}

void NetworkPlay::press_replay_viewer_ready() {
    // Only a headless join sets READY. The flag lives on the app options.
    // The bit is set again when the room clears it, at most once a second.
    auto& lobby = mp::multiplayer_lobby();
    view::ReplayViewerReadyFacts facts;
    facts.headless = runtime_options(runtime_).headless_check && net_options().replay_viewer;
    facts.in_battle_room =
        mp::multiplayer_direct_game_progress().reached == mp::DirectGameStep::battle_room;
    facts.seated = viewer_seated(lobby);
    facts.units_reported = viewer_units_reported(lobby);
    facts.host_map_known = lobby.game != nullptr && mp::lobby_has_host_map(lobby);
    facts.awaiting_mutators = viewer_awaits_mutators(lobby);
    if (lobby.game != nullptr)
        facts.ready_set = (mp::local_info(lobby).options & mp::option::ready) != 0;
    facts.sent = replay_viewer_ready_sent_;
    if (replay_viewer_ready_sent_)
        facts.milliseconds_since_send =
            milliseconds_between(replay_viewer_ready_at_, std::chrono::steady_clock::now());
    if (view::replay_viewer_ready(facts) != view::ReplayViewerReady::set || lobby.game == nullptr)
        return;
    auto& info = mp::local_info(lobby);
    info.options = static_cast<uint16_t>(info.options | mp::option::ready);
    mp::lobby_send_player_info(lobby);
    replay_viewer_ready_sent_ = true;
    replay_viewer_ready_at_ = std::chrono::steady_clock::now();
    if (replay_viewer_ready_said_)
        return;
    replay_viewer_ready_said_ = true;
    say("replay viewer: READY after the unit sync");
}

void NetworkPlay::note_replay_viewer_drop(const view::ReplayViewerWatch& watch) {
    replay_viewer_dropped_ = true;
    if (replay_viewer_reason_printed_)
        return;
    replay_viewer_reason_printed_ = true;
    const bool silent =
        watch.silence_measured && watch.milliseconds_since_packet >= view::replay_viewer_silence_ms;
    if (silent) {
        say("replay viewer: no packet from the host for 60 s; the session dropped");
        return;
    }
    if (watch.local_rejected) {
        say("replay viewer: the host refused this player before the replay arrived");
        return;
    }
    if (watch.session_lost) {
        say(watch.received ? "replay viewer: the session was lost"
                           : "replay viewer: the session was lost before the replay arrived");
        return;
    }
    if (watch.match_ended) {
        say("replay viewer: the match ended before the replay arrived");
        return;
    }
    const bool room_wait = watch.room_wait_measured &&
                           watch.milliseconds_in_battle_room >= view::replay_viewer_room_limit_ms;
    if (room_wait) {
        say("replay viewer: the replay did not start within 5 minutes of the battle room");
        return;
    }
    say("replay viewer: the session dropped before the replay arrived");
}

void NetworkPlay::note_replay_viewer() {
    if (!net_options().replay_viewer || replay_viewer_ended_ || replay_viewer_dropped_ ||
        replay_viewer_summary_printed_)
        return;
    if (net_ && net_->connected && net_->connection.in_session)
        replay_viewer_saw_session_ = true;
    note_replay_viewer_match();
    view::ReplayViewerWatch watch;
    watch.received = replay_viewer_received_;
    watch.session_lost = replay_viewer_session_lost();
    const bool match_up =
        net_ && net_->active && !net_->loading && net_->net && net_->net->world != nullptr;
    if (match_up) {
        const auto& world = *net_->net->world;
        watch.local_rejected = world.game.players[world.game.local_player_index].reject_reason != 0;
        watch.host_in_table = true;
        if (replay_viewer_host_known_) {
            watch.host_in_table = false;
            for (const auto& player : world.game.players) {
                if (player.player_id == replay_viewer_host_id_ && player.in_use != 0 &&
                    player.reject_reason == 0)
                    watch.host_in_table = true;
            }
        }
        watch.match_ended = net_->binding.match != nullptr &&
                            net_->binding.match->outcome() != oa::sim::scenario::Outcome::ongoing;
        watch.silence_measured = true;
        watch.milliseconds_since_packet = milliseconds_since_packet(*net_->net);
    } else {
        auto& lobby = mp::multiplayer_lobby();
        if (lobby.game != nullptr)
            watch.local_rejected = mp::local_player(lobby).reject_reason != 0;
        // The five minutes start when this headless run reaches the battle
        // room, and they count only while the match has not started.
        if (runtime_options(runtime_).headless_check) {
            const auto reached = mp::multiplayer_direct_game_progress().reached;
            if (reached == mp::DirectGameStep::battle_room && !replay_viewer_room_marked_) {
                replay_viewer_room_marked_ = true;
                replay_viewer_room_at_ = std::chrono::steady_clock::now();
            }
            if (replay_viewer_room_marked_) {
                watch.room_wait_measured = true;
                watch.milliseconds_in_battle_room =
                    milliseconds_between(replay_viewer_room_at_, std::chrono::steady_clock::now());
            }
        }
    }
    const view::ReplayViewerEnd decision = view::replay_viewer_end(watch);
    if (decision == view::ReplayViewerEnd::summary)
        print_replay_viewer_summary();
    else if (decision == view::ReplayViewerEnd::dropped)
        note_replay_viewer_drop(watch);
}

int NetworkPlay::run_headless_replay_viewer() {
    const auto started = std::chrono::steady_clock::now();
    while (!replay_viewer_ended_ && !replay_viewer_dropped_) {
        if (!runtime_.service_headless_replay_viewer()) {
            if (replay_viewer_ended_ && replay_viewer_summary_printed_)
                break;
            // The pump may have failed the join after this frame started.
            // That notice is the reason; a quit line is not added on top.
            report_direct_game_progress();
            if (mp::multiplayer_direct_game_progress().reached == mp::DirectGameStep::failed)
                return 1;
            if (!replay_viewer_reason_printed_) {
                say("replay viewer: the run was asked to quit");
                replay_viewer_reason_printed_ = true;
            }
            return 1;
        }
        if (replay_viewer_ended_)
            break;
        if (replay_viewer_dropped_)
            break;
        report_direct_game_progress();
        const auto& progress = mp::multiplayer_direct_game_progress();
        if (progress.reached == mp::DirectGameStep::failed)
            return 1;
        // After this frame, so a join's reset of the room cannot clear a bit
        // this call has just set.
        press_replay_viewer_ready();
        const bool waiting_to_join = !replay_viewer_saw_session_ &&
                                     progress.reached != mp::DirectGameStep::battle_room &&
                                     !net_match_active();
        if (waiting_to_join && std::chrono::steady_clock::now() - started > kReplayViewerJoinWait) {
            if (!replay_viewer_reason_printed_) {
                say("replay viewer: no session within 3 minutes");
                replay_viewer_reason_printed_ = true;
            }
            return 1;
        }
        std::this_thread::sleep_for(kReplayViewerFrameWait);
    }
    if (replay_viewer_ended_ && replay_viewer_summary_printed_)
        return 0;
    if (!replay_viewer_reason_printed_)
        say("replay viewer: the session dropped before the replay arrived");
    return 1;
}

} // namespace oa::app
