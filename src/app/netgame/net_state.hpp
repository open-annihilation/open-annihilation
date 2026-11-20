// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The network session network play keeps for a runtime from start-up
// (NetworkPlay::net_, network_play.hpp): the connection behind
// the multiplayer screens, the running match's binding and whether a match
// report runs.
#pragma once

#include "oa/app/runtime.hpp"
#include "oa/base/game_loop.hpp"
#include "oa/base/threads.hpp"

#include "oa/netgame/console/console_commands.hpp"
#include "oa/netgame/match/match_binding.hpp"
#include "oa/netgame/match/net_match.hpp"
#include "oa/netgame/match/session_lobby.hpp"
#include "oa/session/demo/recording.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "recording_partials.hpp"

#include <array>
#include <atomic>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <vector>

namespace oa::app {

/// A finished game's recording on its way into place, which a thread of its own
/// may be working on.
///
/// The recording is compressed and moved off the thread that draws, so that
/// the end of a game never stalls a frame. The destructor waits for the thread.
struct RecordingFinishing {
    RecordingFinishing() = default;

    /// Waits for the thread, so that no recording is left half finished when the session goes.
    ~RecordingFinishing() { join(); }

    RecordingFinishing(const RecordingFinishing&) = delete;
    RecordingFinishing& operator=(const RecordingFinishing&) = delete;

    /// Waits for the thread, when one runs.
    void join() noexcept {
        if (started)
            oa::base::threads::join_thread(thread);
        started = false;
    }

    /// What to finish, and how.
    PartialFinish finish{};
    /// The recording's files and the lock on them, released when the finish is done.
    std::unique_ptr<PartialRecording> partial{};
    /// The recording's stream, closed by the finish: what it still holds goes to the disk
    /// there, off the thread that draws.
    std::shared_ptr<oa::session::demo::RecordingStream> stream{};
    /// Why the stream is not whole, when a write failed; empty otherwise.
    std::string stream_problem{};
    /// How the finish went; set before the thread ends.
    oa::session::demo::FinishResult result{};
    /// The thread, once started.
    oa::base::threads::Thread thread{};
    bool started{};
    /// The finish ran, on the thread or without one; set last, so that a frame that sees it
    /// may say how it went.
    std::atomic<bool> ran{false};
};

struct NetState {
    oa::netgame::match::NetConnection connection{};
    bool connected{}; // connection storage allocated
    std::unique_ptr<oa::netgame::match::NetMatch> net =
        std::make_unique<oa::netgame::match::NetMatch>();
    oa::netgame::match::MatchBinding binding{};
    bool hosting{};
    bool loading{}; // load barrier in progress
    bool active{};  // a match owns the session
    // The battle room's players while a launch builds the match's world,
    // before the match owns the session: the engine's load_progress hook
    // keeps the connection alive for them. Null otherwise.
    const oa::Game* building_game{};
    // The host's slot table (Game.slot_table, record 0x26) as the battle
    // room had it at the launch: each of the host's slots' player id. The
    // host deals fixed starts in its own slot order, which this machine's
    // slots need not follow.
    std::array<uint32_t, OA_PLAYER_COUNT> host_slot_ids{};
    // When the loading screen's work last ran, over the build and the load
    // barrier alike.
    oa::netgame::match::LoadingPace loading_pace{};
    // The loading screen's rows as the engine last reported them; the load
    // progress records carry their mean.
    std::array<uint8_t, oa::netgame::match::load_progress_rows> load_rows{};
    uint8_t local_slot{};
    // The pause bit as this machine last saw it: the Pause key's own flips
    // update it as they are sent, so a change seen in a frame came from
    // another player's machine and is announced.
    bool pause_seen{};
    // The game speed as this machine last saw it: sent as the speed keys or
    // the GAME slider set it (the extension's speed_changed), received, or
    // put back by this machine's preferences alone. A speed another machine
    // sent, or the preferences put back, shows as a difference from it.
    uint16_t speed_seen{};
    uint32_t stalled_player{oa::netgame::match::no_player_id};
    bool finish_pending{}; // the finished game waits on the final economy
    // A match report started (NetworkPlay::start_reporter) and has not closed;
    // the extension built on network play keeps the report itself
    // (extension_api::Hooks::report_start). Used only once the end-of-game
    // screen's state exists.
    bool report_started{};
    // The console's network commands reach the app through it.
    oa::netgame::console::CommandHost console_commands{};
    // The in-game console's host, kept from the engine's console_host hook:
    // the engine fills it as each match starts and keeps it at this address
    // while the runtime lives. Chat another player sent is posted through it.
    const oa::ui::console::ConsoleHost* console_host{};
    /// The lag guard (network.lag-guard): steps held while the other players are silent.
    oa::base::game_loop::LagGuard lag_guard{};
    /// The player the last vote shown would reject; Vote Yes and Vote No answer it.
    uint32_t vote_target{oa::netgame::match::no_player_id};
    /// The recording of the running game (recorder.ta-demo-recorder, the player's setting,
    /// --net-record or --record-game), streamed to its partial file as the game goes and finished
    /// into recording_path as the game is left.
    oa::session::demo::DemoRecording recording{};
    std::filesystem::path recording_path{};
    /// The recording's files being made, with the lock that says its game is on.
    std::unique_ptr<PartialRecording> partial{};
    /// The recording is a `.oademo` (not the recorder hack's `.tad`).
    bool recording_is_replay{};
    /// A file --net-record or --record-game named: it replaces what is there instead of
    /// taking " (2)".
    bool recording_named{};
    /// The replay's description and manifest as they stood when the game began.
    oa::formats::oademo::Replay recording_start{};
    /// The match finished (MatchEvent::finished) instead of being left.
    bool game_finished{};
    /// The recording could not be started for this game: a .record typed later does not try again.
    bool recording_refused{};
    /// The last game's recording on its way into place.
    std::unique_ptr<RecordingFinishing> finishing{};
    /// The battle room's unit verdicts as the launch applied them, one 0x1a verdict record each;
    /// a recording of the game stores them beside this machine's unit checksums.
    std::vector<std::array<uint8_t, oa::formats::tad::layout::unit_check_record_bytes>>
        unit_verdicts{};
};

/// Returns the clock --check-host-not-found runs DirectPlay's timers on.
///
/// It moves on by one frame each frame (host_not_found_clock_frame) and by
/// every wait a pump would have made, so the join's 20 s wait for its host and
/// the 4 s "Host not found.  Exiting..." run in simulated time.
///
/// @return the clock, with no bytes in flight to wait for
[[nodiscard]] oa::netgame::sock::HostClock host_not_found_clock() noexcept;

/// Returns the time on the clock host_not_found_clock gives.
///
/// @return milliseconds since the run began
[[nodiscard]] uint32_t host_not_found_clock_ms() noexcept;

} // namespace oa::app
