// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The table through which the MCP server (src/app/mcp) changes a running
// match the way a player does: its speed, pausing it, where the view looks,
// its clock, giving orders and selecting units. The server gets the table
// with play_host() from a hook that is given the runtime, and uses it on
// the thread that runs main(), while the runtime lives. The automation
// endpoint does not use it, so --fark stays read-only.
//
// The header names only the standard library's types and declares the
// engine's it names, so the server needs nothing more of the engine's to
// read it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::app {

class Runtime;

/// What the server can see of the running match before it changes the speed or the pause.
struct PlayExtensionState {
    /// A match is on screen. False on the menus, and while a match is loading.
    bool in_match{};
    /// The match replays a recording. Its speed and pause stay as the recording left them.
    bool replay{};
    /// The local player is only watching a shared match, and cannot change it.
    bool watcher{};
    /// The match is played with other machines. A change is sent on, as the keys send it.
    bool shared_match{};
    /// The match is paused.
    bool paused{};
    /// The match has finished.
    bool finished{};
    /// The speed the match is set to. 10 is the ordinary speed; 0 holds the
    /// simulation without pausing.
    int32_t speed{};
    /// The slowest speed the match allows. A mod may set this, down to 0.
    int32_t slowest{};
    /// The fastest speed the match allows. A mod may set this.
    int32_t fastest{};
    /// The match's tick. 0 when no match is running.
    uint32_t tick{};
    /// The view is the director's, so a camera move is refused. The speed and the pause are not.
    bool director{};
    /// The clock runs only when clock.step or clock.run asks. False on the wall clock.
    bool stepped{};
};

/// The view the next frame draws, after the view's limits have held it.
struct PlayView {
    /// A match is on screen. False on the menus, and while a match is loading.
    bool in_match{};
    /// The camera's column, in map pixels: the left edge of the battlefield.
    int32_t x{};
    /// The camera's row, in map pixels: the top edge of the battlefield.
    int32_t z{};
    /// How large the map is drawn. 1 draws one map pixel as one canvas pixel.
    float zoom{};
};

/// No tick to stop clock.run at. A real tick never has this value.
inline constexpr uint32_t clock_no_target = 0xffffffffu;

/// How a granted run of the stepped clock stopped.
enum class ClockStop : uint8_t {
    ran,      ///< the ticks it was given were run, or the simulation stopped short of them
    tick,     ///< the named tick was reached
    event,    ///< the match's outcome left ongoing
    paused,   ///< the match is paused, or a menu holds its clock
    finished, ///< the match had already finished
    refused,  ///< this match cannot step, or the clock is not stepped
};

/// One granted run of the stepped clock.
struct ClockRun {
    ClockStop stop{ClockStop::refused}; ///< why the run returned
    uint32_t ran{};                     ///< how many simulation ticks ran
    uint32_t tick{};                    ///< the match's tick after the run
    bool game_over{};                   ///< the match's outcome left ongoing
};

/// The order orders.issue asks for.
enum class PlayOrderKind : uint8_t {
    move,           ///< move to a point
    patrol,         ///< patrol to a point
    attack_unit,    ///< attack a unit
    attack_ground,  ///< attack a point on the ground
    guard,          ///< guard a unit
    repair,         ///< repair a unit, or help finish one that is being built
    reclaim_unit,   ///< reclaim a unit
    reclaim_ground, ///< reclaim the feature under a point
    mobile_build,   ///< a mobile builder places a building
    building_build, ///< a factory builds a count of a unit
    factory,        ///< add to, or take from, a factory's queued count
    stop,           ///< stop, replacing the unit's orders
};

/// A map point in whole map pixels. The host turns it into the fixed point orders take.
struct PlayOrderPoint {
    int32_t x{}; ///< map pixel
    int32_t y{}; ///< map pixel; 0 when the caller gave only x and z
    int32_t z{}; ///< map pixel
};

/// One order for one or more units.
///
/// `units` points at `unit_count` slots and is read only during the call.
struct PlayOrderRequest {
    PlayOrderKind kind{};     ///< which order
    bool append{};            ///< true adds the order behind the ones the unit already has
    const uint16_t* units{};  ///< the units to order; slot 0 is never a unit
    std::size_t unit_count{}; ///< how many units
    uint16_t target{};        ///< the other unit, or 0 when the order has none
    bool has_point{};         ///< point is set
    PlayOrderPoint point{};   ///< the map point, when has_point
    const char* type_name{};  ///< the product's unit name, or null; read only during the call
    int32_t count{1};         ///< units to build, or the change in a factory's queue
    uint8_t facing{};         ///< quarter turns from south a building is placed facing
};

/// The standing order orders.state asks for, the order panel's fire, move, on, off and cloak.
enum class PlayStateKind : uint8_t {
    fire,  ///< the standing fire order; value is 0, 1 or 2
    move,  ///< the standing move order; value is 0, 1 or 2
    on,    ///< switch on
    off,   ///< switch off
    cloak, ///< cloak; on says whether it is on
};

/// A standing order for one or more units. `units` is read only during the call.
struct PlayStateRequest {
    PlayStateKind kind{};     ///< which standing order
    int32_t value{};          ///< the fire or move order, 0, 1 or 2
    bool on{};                ///< true when cloak is being switched on
    const uint16_t* units{};  ///< the units
    std::size_t unit_count{}; ///< how many units
};

/// A selection. `units` is read only during the call. An empty list clears the selection
/// when add is false, and leaves it when add is true.
struct PlaySelectRequest {
    const uint16_t* units{};  ///< the units to select
    std::size_t unit_count{}; ///< how many units
    bool add{};               ///< true keeps the units already selected
};

/// One order copied back after a change, in the same words units.orders uses.
struct PlayOrderLine {
    std::string kind;      ///< the mission's name
    uint16_t target{};     ///< the target unit, or 0
    int32_t x{};           ///< map pixel
    int32_t y{};           ///< map pixel
    int32_t z{};           ///< map pixel
    int32_t parameter_1{}; ///< the order's first parameter
    int32_t parameter_2{}; ///< the order's second parameter
    uint8_t phase{};       ///< how far the order has got
    uint32_t issue_tick{}; ///< the tick the order was given
    uint8_t facing{};      ///< quarter turns from south
    bool build_started{};  ///< the order already has the unit it is building
    int32_t seen_x{};      ///< where the order last saw its target, map pixel
    int32_t seen_z{};      ///< map pixel
    std::string build;     ///< the product's unit name, for a build; empty otherwise
};

/// One unit's queues after an order.
struct PlayUnitOrders {
    uint16_t id{};                        ///< the unit
    std::vector<PlayOrderLine> primary;   ///< the primary queue, head first
    std::vector<PlayOrderLine> secondary; ///< the secondary queue, head first
    bool primary_truncated{};             ///< the primary queue was longer than the copy
    bool secondary_truncated{};           ///< the secondary queue was longer than the copy
};

/// What a change did. ok is false when nothing was given; detail then says why.
/// pending is set while a stepped clock is holding the order for a step.
struct PlayCommandResult {
    bool ok{};                          ///< the change was made
    bool pending{};                     ///< a stepped clock holds it until a step runs it
    uint32_t tick{};                    ///< the match's tick when it was made
    std::string detail;                 ///< why it was refused, when ok is false
    std::vector<PlayUnitOrders> queues; ///< each ordered unit's queues, when an order was given
    std::vector<uint16_t> selected;     ///< the units selected afterwards, for a selection
};

// What the MCP server may change of the running match, and what it may read
// before it does. play_host() sets every entry, and context, which every
// entry is passed back, is the runtime.
struct PlayHost {
    void* context{};
    /// Sets the match's speed to this absolute speed, one step at a time as
    /// the + and - keys do, so the change is announced and the other
    /// machines of a shared match stay in step. Changes nothing, and returns
    /// false, when no match is running, the match is a replay, the local
    /// player is watching, or the speed is outside the speeds the match
    /// allows. Returns true when the match is already at that speed, without
    /// stepping.
    ///
    /// @param context PlayHost::context
    /// @param speed the absolute speed, the number the match reports
    /// @return true when the match is at that speed
    bool (*set_speed)(void* context, uint16_t speed){};
    /// Pauses the match as the Pause key does. Returns true when the match
    /// is already paused, without resuming it. Changes nothing, and returns
    /// false, when no match is running, the match is a replay, the local
    /// player is watching, or the match has finished.
    ///
    /// @param context PlayHost::context
    /// @return true when the match is paused
    bool (*pause)(void* context){};
    /// Resumes the match as the Pause key does. Returns true when the match
    /// is already running, without pausing it. Refuses the same matches pause refuses.
    ///
    /// @param context PlayHost::context
    /// @return true when the match is running
    bool (*resume)(void* context){};
    /// Reads whether a match is running and what kind it is, its pause, its
    /// speed and the speeds it allows, and its tick. Changes nothing.
    ///
    /// @param context PlayHost::context
    /// @return the match as the server sees it
    PlayExtensionState (*extension_state)(void* context){};
    /// Reads the view the next frame draws. Changes nothing. The position and
    /// the zoom are the ones the view's limits hold, which is what the frame
    /// shows. Returns false when `shown` is null.
    ///
    /// @param context PlayHost::context
    /// @param shown the view; unchanged when this returns false
    /// @return true when `shown` was filled in
    bool (*read_view)(void* context, PlayView* shown){};
    /// Puts the view's top-left corner at a map pixel, as set_camera_position
    /// does, and the view's limits hold it. A follow of a unit ends, as a
    /// scroll ends it, so the view stays. Returns false, and changes nothing,
    /// when no match is running or the view is the director's.
    ///
    /// @param context PlayHost::context
    /// @param x the column, in map pixels
    /// @param z the row, in map pixels
    /// @return true when the view was placed
    bool (*set_camera)(void* context, int32_t x, int32_t z){};
    /// Centres the view on a unit, as center_camera_on_unit does, held within
    /// the view's limits. A follow ends, so the view stays on the place rather
    /// than chasing the unit. Returns false, and changes nothing, when no
    /// match is running, the view is the director's, or the unit is not on the map.
    ///
    /// @param context PlayHost::context
    /// @param unit the unit's id
    /// @return true when the view was centred on the unit
    bool (*center_unit)(void* context, uint16_t unit){};
    /// Centres the view on a point of the map, as centre_view_on does, held
    /// within the view's limits. The point is a map pixel. A place past the
    /// coordinates the view can name is brought in to the furthest one, and
    /// the limits then hold the view. A follow ends. Returns false, and
    /// changes nothing, when no match is running or the view is the director's.
    ///
    /// @param context PlayHost::context
    /// @param x the point's column, in map pixels
    /// @param z the point's row, in map pixels
    /// @return true when the view was centred on the point
    bool (*center_point)(void* context, int32_t x, int32_t z){};
    /// Zooms the view by a factor about a canvas point, as zoom_match_about
    /// does, and the zoom's limits hold the result. With no point, the zoom
    /// is about the middle of the battlefield. A follow of a unit goes on, as
    /// a pinch keeps it. Returns false, and changes nothing, when no match is
    /// running, the view is the director's, or the factor is not positive.
    ///
    /// @param context PlayHost::context
    /// @param factor the zoom now over the zoom before; above 0
    /// @param x canvas column of the point, when `focus_given` is true
    /// @param y canvas row of the point, when `focus_given` is true
    /// @param focus_given true when `x` and `y` name the point
    /// @return true when the zoom was set
    bool (*zoom_about)(void* context, float factor, float x, float y, bool focus_given){};
    /// Sets the clock to the wall clock or to the stepped clock. 0 is
    /// realtime and 1 is stepped; any other value changes nothing and
    /// returns false. A stepped clock runs the match only when run_ticks
    /// asks, and frames keep drawing. The time the clock was held is not
    /// caught up when it returns to the wall clock. Changes nothing, and
    /// returns false, when no match is running, the match is a replay, the
    /// local player is watching, or the match is shared with other machines.
    /// A finished single-player match may still switch. Returns true when
    /// the clock is already in that mode.
    ///
    /// @param context PlayHost::context
    /// @param mode 0 for the wall clock, 1 for the stepped clock
    /// @return true when the clock is in that mode
    bool (*set_clock_mode)(void* context, uint8_t mode){};
    /// Runs simulation ticks while the clock is stepped. count is how many
    /// ticks this call may run, and the match's speed does not scale it.
    /// until_tick stops the run when the match's tick reaches it;
    /// clock_no_target does not stop on a tick. until_event is 0, or 1 when
    /// the caller is clock.run until an event: the run stops when the
    /// match's outcome leaves ongoing either way. Returns refused when the
    /// clock is not stepped, no match is running, the match is a replay, the
    /// local player is watching, or the match is shared. Returns finished
    /// when the match has already finished, and paused when the pause is on
    /// or a menu holds the clock, without running a tick.
    ///
    /// @param context PlayHost::context
    /// @param count how many ticks this call may run
    /// @param until_tick the tick to stop at, or clock_no_target
    /// @param until_event 1 when the caller stops on the match ending
    /// @return how far the run got
    ClockRun (*run_ticks)(
        void* context, uint32_t count, uint32_t until_tick, uint8_t until_event
    ){};
    /// Starts this match on the stepped clock when mode is 1, once. A later
    /// call in the same match changes nothing, so a clock the player switched
    /// stays switched. Mode 0 leaves the wall clock and likewise sticks for
    /// the match. A shared match, a replay and a watcher are left on the wall
    /// clock. While no match is on screen the choice is forgotten, and the
    /// clock returns to the wall clock, so the next match starts again from
    /// mode. Does nothing when the entry is null.
    ///
    /// @param context PlayHost::context
    /// @param mode 1 to start the match stepped, 0 to start it on the wall clock
    void (*offer_startup_clock)(void* context, uint8_t mode){};
    /// Gives an order to each named unit, through the match's own order calls.
    ///
    /// A move or a patrol of several units keeps the group's shape, as the
    /// order panel does. A repair of a unit that is still being built helps
    /// finish it. Nothing is given, and ok is false, when no match is
    /// running, the match is a replay, the local player is watching, the
    /// match has finished, or any named unit is not one this machine plays.
    /// The queues are the units' queues after the order.
    ///
    /// @param context PlayHost::context
    /// @param request the order and the units
    /// @return the tick and the queues, or why nothing was given
    PlayCommandResult (*issue_orders)(void* context, const PlayOrderRequest& request){};
    /// Sets a standing order on each named unit, through the order panel's path.
    ///
    /// Fire, move, on, off and cloak are the panel's own orders. A unit the
    /// order does not reach is left as it is. Nothing is set when every named
    /// unit is left, or when issue_orders would refuse the units.
    ///
    /// @param context PlayHost::context
    /// @param request the standing order and the units
    /// @return the tick and the queues, or why nothing was set
    PlayCommandResult (*set_state)(void* context, const PlayStateRequest& request){};
    /// Selects units, as a click does, or clears the selection.
    ///
    /// add keeps the units already selected. An empty list with add false
    /// clears the selection. A unit that cannot be selected, and a unit
    /// issue_orders would refuse, refuses the whole call and leaves the
    /// selection as it was.
    ///
    /// @param context PlayHost::context
    /// @param request the units
    /// @return the tick and the units selected afterwards, or why nothing changed
    PlayCommandResult (*select_units)(void* context, const PlaySelectRequest& request){};
};

/// Returns the table through which the MCP server changes the running match.
///
/// @param runtime the running game, which the table's entries change
/// @return the table, every entry set and bound to `runtime`
[[nodiscard]] PlayHost play_host(Runtime& runtime);

} // namespace oa::app
