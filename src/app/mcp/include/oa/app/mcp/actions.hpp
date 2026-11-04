// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// game.speed, game.pause, the camera tools, the clock tools, orders and
// selection, and the queue that holds an act until after_pump (actions.cpp).
// A call is checked when it arrives. A speed or a pause that would change a
// replay, a watcher, or no running match is refused then, and so is an order
// for those and for a finished match, and a stepped clock in a shared match.
// A camera move needs a running match and is refused for the director's view;
// a replay and a watcher may still point the view, because it is this
// machine's picture. One that can be made waits, and is applied after the
// pump and before the match clock. The answer names the tick the change
// landed on. A stepped clock holds an order until a step runs it. camera.get
// only reads, and answers at once.
#pragma once

#include "oa/app/mcp/session.hpp"
#include "oa/app/play_host.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oa::app::mcp {

/// What an act tool asks the match to do.
enum class PlayKind : uint8_t {
    speed,               ///< set the absolute speed
    pause,               ///< pause the match
    resume,              ///< resume the match
    order,               ///< give an order
    state,               ///< set a standing order
    select,              ///< select units, or clear the selection
    camera_set,          ///< put the view's corner at a map pixel
    camera_center_unit,  ///< centre the view on a unit
    camera_center_point, ///< centre the view on a point of the map
    camera_zoom,         ///< zoom the view
    clock_mode,          ///< set the wall clock or the stepped clock
    clock_step,          ///< run a number of ticks
    clock_run,           ///< run until a tick or the match ends
};

/// What clock.run stops for, besides its max_ticks.
enum class ClockUntil : uint8_t {
    none,  ///< clock.step, or a tool that is not a run
    event, ///< stop when the match ends
    tick,  ///< stop at a named tick
};

/// One request, as the tool read it.
///
/// speed is the absolute speed for PlayKind::speed. The order, state,
/// select and camera fields are the call's own. units and type_name are
/// owned here; the host is handed pointers into them only while it runs.
struct PlayRequest {
    PlayKind kind{};                 ///< what the call asked for
    int64_t speed{};                 ///< the absolute speed, for PlayKind::speed
    oa::app::PlayOrderKind order{};  ///< the order, for PlayKind::order
    bool append{};                   ///< true adds the order behind the ones the unit has
    std::vector<uint16_t> units;     ///< the units
    uint16_t target{};               ///< the other unit, or 0
    bool has_point{};                ///< point is set
    oa::app::PlayOrderPoint point{}; ///< the map point, when has_point
    std::string type_name;           ///< the product's unit name, for a build or a factory
    int32_t count{1};                ///< units to build, or the change in a factory's queue
    uint8_t facing{};                ///< quarter turns from south a building is placed facing
    oa::app::PlayStateKind state{};  ///< the standing order, for PlayKind::state
    int32_t state_value{};           ///< the fire or move order, 0, 1 or 2
    bool state_on{};                 ///< true when cloak is being switched on
    bool select_add{};               ///< true keeps the units already selected
    int32_t place_x{};               ///< the map column, for a camera place or a point
    int32_t place_z{};               ///< the map row, for a camera place or a point
    uint16_t unit{};                 ///< the unit's id, for PlayKind::camera_center_unit
    float zoom_factor{};             ///< the zoom's multiplier, when absolute_zoom is false
    float zoom_target{};             ///< the zoom itself, when absolute_zoom is true
    float focus_x{};                 ///< the canvas column the zoom keeps, when focus_given
    float focus_y{};                 ///< the canvas row the zoom keeps, when focus_given
    bool absolute_zoom{};            ///< zoom_target names the zoom, and zoom_factor is not used
    bool focus_given{};              ///< focus_x and focus_y name the point the zoom keeps
    int64_t ticks{};                 ///< clock.step's count, or clock.run's max_ticks
    int64_t until_tick{};            ///< the tick clock.run stops at, when until is tick
    ClockUntil until{};              ///< what clock.run stops for
    uint8_t mode{};                  ///< 0 is the wall clock and 1 is stepped, for clock.mode
};

/// How many simulation ticks one frame of clock.step or clock.run may run.
///
/// A larger request stays held and continues on later frames, so a call
/// never blocks the frame. A request of this many or fewer finishes in the
/// frame it is applied, when the host runs the whole batch.
inline constexpr uint32_t kClockTicksPerFrame = 120;

/// A change that was applied, so the game can log the tick it landed on.
struct AppliedAct {
    uint32_t tick{};    ///< the match's tick when the change was made
    std::string detail; ///< the tool and what it set, for the log line
};

/// The changes waiting for the frame's after_pump, and their results until the calls take them.
class ActionQueue {
  public:

    /// Checks a request and does not queue it.
    ///
    /// The session's first poll uses this. The session has not yet copied the
    /// arguments, so nothing is queued here. A replay, a watcher, a speed
    /// outside the speeds the match allows, a finished match's pause, or no
    /// running match is refused. A camera move is refused when no match is
    /// running or the view is the director's. A request that can be made is held.
    ///
    /// @param host the host of this frame; a host with no extension_state is no match
    /// @param request what the call asked for
    /// @return a refusal, or a held answer
    [[nodiscard]] ToolReply consider(const PlayHost& host, const PlayRequest& request) const;

    /// Queues a request, or returns the result of one already queued under token.
    ///
    /// The session's later polls use this. The first of them queues the
    /// request under token. Token is a member of the session's copy of the
    /// arguments: the session moves that copy at the end of every pump, and
    /// a member of its tree keeps its address until the call finishes. A
    /// poll after the change has been applied returns the result and drops
    /// it. A poll before that stays held.
    ///
    /// @param host the host of this frame
    /// @param request what the call asked for
    /// @param token a member of the call's arguments; null is refused
    /// @param poll how many times the session has polled this call; 1 queues it
    /// @return a refusal, a held answer, or the result
    [[nodiscard]] ToolReply
    follow(const PlayHost& host, const PlayRequest& request, const void* token, int64_t poll);

    /// Applies every change that is waiting.
    ///
    /// Each change goes through the host's set_speed, pause, resume, camera,
    /// set_clock_mode, run_ticks, issue_orders, set_state or select_units, in
    /// the order it was queued. A clock run longer than one frame stays held
    /// and continues on the next frame. A stepped clock holds an order or a
    /// standing order until a step runs it, and that step's tick is the one
    /// the order lands on. A camera move is applied in that same pass and does
    /// not wait for a step. A change the host makes is remembered with the tick
    /// it landed on, and named in the returned list so the game can log it. A
    /// change the host refuses is remembered as a failure and is not listed.
    /// A host with no entry for the change fails it and does not crash.
    ///
    /// @param host the host of this frame
    /// @return the changes that were made, in the order they were queued
    [[nodiscard]] std::vector<AppliedAct> apply(const PlayHost& host);

    /// Tells whether a change is waiting to be applied.
    ///
    /// @return true when apply has work to do
    [[nodiscard]] bool waiting() const;

  private:

    /// One change waiting to be applied, or its result until the call takes it.
    struct Slot {
        const void* token{};   ///< the session's copy of the arguments
        PlayRequest request{}; ///< what the call asked for
        bool done{};           ///< apply has finished this change
        bool ok{};             ///< the change was made
        std::string text;      ///< the result, or why it was refused
        uint32_t ran{};        ///< ticks a clock run has already completed
        uint32_t total{};      ///< ticks clock.step or clock.run asked for
        bool game_over{};      ///< the match ended during the run
        oa::app::ClockStop stop{oa::app::ClockStop::ran}; ///< why a clock run last returned
    };

    /// How many changes may wait at once, the same bound as the session's held calls.
    static constexpr std::size_t kQueueLimit = 8;

    /// Finds the change queued under token.
    ///
    /// @param token the call's arguments
    /// @return its place, or nothing when it was not queued
    [[nodiscard]] std::optional<std::size_t> find_token(const void* token) const;

    /// Returns a waiting change's held answer, or a finished one's result, and drops that result.
    ///
    /// @param index the change's place
    /// @return the answer
    [[nodiscard]] ToolReply take(std::size_t index);

    std::vector<Slot> slots_;
};

/// The host and the queue the act tools share.
///
/// host_of is left as the caller set it. The game points it at the running
/// match; a test points it at its own host. add_play_tools does not replace it.
struct PlayBridge {
    /// Returns the host for this frame. Null until the caller sets it.
    PlayHost (*host_of)(void* context){};
    /// Passed to host_of.
    void* context{};
    /// The changes waiting for after_pump.
    ActionQueue queue{};
};

/// Adds game.speed, game.pause, the camera tools, the clock tools and the order tools.
///
/// camera.get only reads, so a session that lists tools that read still
/// offers it. The others are acts. The bridge outlives the session's calls.
/// host_of is not changed.
///
/// @param[in,out] session the session
/// @param[in,out] play the host and the queue; it outlives the session's calls
void add_play_tools(Session& session, PlayBridge& play);

/// Adds orders.issue, orders.state and units.select.
///
/// They are acts, queued with game.speed and game.pause and applied at
/// after_pump. add_play_tools calls this. The bridge outlives the session's calls.
///
/// @param[in,out] session the session
/// @param[in,out] play the host and the queue; it outlives the session's calls
void add_order_tools(Session& session, PlayBridge& play);

} // namespace oa::app::mcp
