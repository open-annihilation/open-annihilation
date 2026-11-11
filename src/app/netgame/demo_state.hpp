// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The .tad playback network play keeps for a runtime while it replays a
// recording (NetworkPlay::demo_): the session runtime_demo.cpp drives, which the replay
// hooks the extension hands the engine read, and the viewer's clock beside it.
#pragma once

#include "oa/app/extension.hpp"
#include "oa/session/demo.hpp"
#include "oa/session/demo/clock.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oa::app {

/// A seek that is in progress: the tick it was asked from, the tick it is
/// going to, and when it started, so the line it prints can say how long it took.
struct ReplaySeekRun {
    uint32_t from{};
    uint32_t to{};
    uint32_t origin{}; ///< the tick the skip started at, after a restore
    bool past_furthest{};
    bool viewer_paused{}; ///< the viewer's pause, put back when the jump lands
    std::chrono::steady_clock::time_point started{};
};

struct DemoState : oa::session::demo::DemoSession {
    /// The viewer's speed, pause and keyframe reel. A restore does not touch it:
    /// a keyframe holds the session's own fields, not these.
    oa::session::demo::PlaybackClock clock{};
    /// Keyframes taken while watching, by the tick they were taken at.
    std::map<uint32_t, oa::session::demo::DemoKeyframe> keyframes{};
    /// Seeks waiting for their tick, then one that is running.
    std::vector<oa::session::demo::ReplayJump> seeks{};
    std::optional<ReplaySeekRun> seek{};
    bool keyframe_refused{};  ///< a take was refused, and the log has said so
    bool speech_forced_off{}; ///< speech was forced off for a fast speed
    bool speech_was_on{};     ///< speech was on before it was forced off
    /// Takes keyframes. A headless run with no jump takes none.
    bool watch_keyframes{};
    /// The ready spans replay_state last handed out. The view points here.
    std::vector<ReplayReadySpan> state_ready{};
    /// The sentence replay_state last handed out. The view points here.
    std::string state_unavailable{};
};

} // namespace oa::app
