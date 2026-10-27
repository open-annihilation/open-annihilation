// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "match_state.hpp"
#include "oa/sim/air/goal.hpp"
#include "oa/sim/air/host.hpp"
#include "oa/sim/match_runtime/command.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include "oa/sim/ballistics.hpp"
#include "oa/sim/unit_health.hpp"
#include "oa/sim/unit_movement/movement.hpp"
#include "oa/sim/weapon_execution.hpp"
#include "oa/sim/weapon_execution/retaliation.hpp"
#include "oa/sim/weapon_execution/weapon_launch.hpp"
#include "oa/base/game_math.hpp"
#include "oa/sim/spatial_state/cell_classifier.hpp"
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace oa::sim::match_runtime::tick_detail {
using base::game_math::truncate_low32;

// The scheduler result of a mission handler that met a fault and noted it:
// the order scheduler removes the order.
inline constexpr uint32_t mission_fault_result = 8;

// Overlay/queue walks must stop at a cycle. Patrol clone and a bad
// queue-tail insert can loop the primary queue; a 100000-step budget still
// freezes Shift-held drawing (visit_primary_queue pushes that many path
// points).
inline constexpr std::size_t overlay_order_budget = 256;

// SelfDestruct keeps its countdown in the order's second parameter with this
// marker in the top nibble once counting has started.
inline constexpr uint32_t self_destruct_countdown_marker = 0xf0000000u;

/// Queues an order after the one carrying the queue-tail mark, moving the
/// mark to it, or at the end of the primary queue when no order carries it.
///
/// A cycle ends the walk with the order linked after the repeated one; an
/// order the match does not own ends it with the order linked in its place.
/// At most overlay_order_budget orders are walked.
///
/// @param[in,out] unit Unit whose primary queue receives the order.
/// @param[in,out] record Order to queue; it takes the queue-tail mark.
/// @param extra Maps an order to its match-side fields (command flags), or
///     to null for an order the match does not own.
template <typename Extra>
inline void insert_after_queue_tail(
    sim::simulation_state::Unit& unit, sim::simulation_state::Order& record, Extra&& extra
) {
    if (auto* own = extra(&record))
        own->command_flags |= command_queue_tail;
    record.next = nullptr;
    auto** link = &unit.primary;
    sim::simulation_state::Order* seen[overlay_order_budget]{};
    std::size_t n = 0;
    while (*link && n < overlay_order_budget) {
        auto* current = *link;
        if (current == &record)
            break;
        for (std::size_t i = 0; i < n; ++i) {
            if (seen[i] == current) {
                current->next = &record;
                record.next = nullptr;
                return;
            }
        }
        seen[n++] = current;
        auto* current_extra = extra(current);
        if (!current_extra) {
            // current is not in orders_ — do not read current->next (use-after-free).
            record.next = nullptr;
            *link = &record;
            return;
        }
        if (current_extra->command_flags & command_queue_tail) {
            current_extra->command_flags &= static_cast<uint8_t>(~command_queue_tail);
            auto* next = current->next;
            if (next == &record || next == current)
                next = nullptr;
            else {
                for (std::size_t i = 0; i < n; ++i)
                    if (seen[i] == next) {
                        next = nullptr;
                        break;
                    }
            }
            record.next = next;
            current->next = &record;
            return;
        }
        link = &current->next;
    }
    record.next = nullptr;
    *link = &record;
}

/// Calls a function on each order of a queue from its head, stopping at the
/// first order seen twice.
///
/// At most overlay_order_budget orders are visited.
///
/// @param head first order of the queue, or null for an empty queue
/// @param fn called with each order in queue order
template <typename Fn>
inline void for_each_primary_uncycled(sim::simulation_state::Order* head, Fn&& fn) {
    sim::simulation_state::Order* seen[overlay_order_budget]{};
    std::size_t n = 0;
    for (auto* order = head; order && n < overlay_order_budget; order = order->next) {
        for (std::size_t i = 0; i < n; ++i)
            if (seen[i] == order)
                return;
        seen[n++] = order;
        fn(order);
    }
}

/// Returns the low 16 bits of a word read as signed.
///
/// @param n the word
/// @return its low half, -32768..32767
inline int16_t signed_half(uint32_t n) {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(n));
}

/// Returns the high 16 bits of a word read as signed: the whole part of a
/// 16.16 value.
///
/// @param n the word
/// @return its high half, -32768..32767
inline int16_t high_word(uint32_t n) {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(n >> 16));
}

/// Reads a 32-bit word as signed.
///
/// @param n the word
/// @return the same bits as a signed value
inline int32_t signed_word(uint32_t n) {
    return std::bit_cast<int32_t>(n);
}

/// Adds two signed 16.16 values, wrapping at 32 bits as 3.1c does.
///
/// @param a first value
/// @param b second value
/// @return the low 32 bits of the sum
inline int32_t wrapping_add(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) + std::bit_cast<uint32_t>(b));
}

/// Subtracts one signed 16.16 value from another, wrapping at 32 bits.
///
/// @param a value subtracted from
/// @param b value subtracted
/// @return the low 32 bits of the difference
inline int32_t wrapping_sub(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) - std::bit_cast<uint32_t>(b));
}

/// Shifts a word right by 20 bits with its sign, keeping the low 16 bits: the
/// cell of a 16.16 coordinate measured in 16-unit cells.
///
/// @param value the word, read as signed
/// @return the shifted value's low half, read as signed
inline int16_t arithmetic_shift20(uint32_t value) {
    const auto high = value >> 20;
    const auto bits = (value & 0x80000000u) ? high | 0xfffff000u : high;
    return std::bit_cast<int16_t>(static_cast<uint16_t>(bits));
}

/// Returns the footprint-origin cell of one signed 16.16 axis.
///
/// snap_build_position chooses this cell, then places the footprint's centre
/// on it.
///
/// @param world signed 16.16 coordinate
/// @param footprint footprint extent on that axis, in cells
/// @return the cell
inline int32_t footprint_origin_cell(int32_t world, int16_t footprint) {
    return static_cast<int32_t>(arithmetic_shift20(
        std::bit_cast<uint32_t>(world) - static_cast<uint32_t>(footprint) * 0x80000u + 0x80000u
    ));
}

/// Returns the high 32 bits of a value's 64-bit square.
///
/// @param value the value, read as signed
/// @return bits 32..63 of value * value
inline uint32_t square_high(int32_t value) {
    return static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(value) * value) >> 32);
}

/// Wraps a unit position as a weapon point.
///
/// @param p signed 16.16 x, y, z bit patterns
/// @return the same words as a weapon point
inline sim::weapon_execution::Point as_point(const std::array<uint32_t, 3>& p) {
    return {{p[0], p[1], p[2]}};
}

/// Unwraps a weapon point as a unit position.
///
/// @param p the weapon point
/// @return its signed 16.16 x, y, z bit patterns
inline std::array<uint32_t, 3> as_position(const sim::weapon_execution::Point& p) {
    return {p.fixed[0], p.fixed[1], p.fixed[2]};
}

inline constexpr std::array<const char*, 3> aim_script_names{
    "AimPrimary", "AimSecondary", "AimTertiary"
};
inline constexpr uint32_t live_unit_flag = OA_UNIT_FLAG_LIVE;
inline constexpr uint32_t building_unit_flag = OA_UNIT_FLAG_BUILDING; // bm_code==0
inline constexpr uint32_t attached_without_piece = OA_UNIT_FLAG_ATTACHED_WITHOUT_PIECE;
inline constexpr uint32_t floater_type_flag = OA_UNIT_DEF_FLAG_FLOATER;
inline constexpr uint32_t death_pending_flag = OA_UNIT_FLAG_DEATH_PENDING;
// The selection bits the kill handler clears.
inline constexpr uint32_t death_clear_mask = OA_UNIT_FLAG_SELECTED | OA_UNIT_FLAG_SELECTABLE;
inline constexpr uint8_t damage_script_countdown = 0xf0; // Unit.damage_countdown at a hit
inline constexpr uint8_t healing_kind = 10;
inline constexpr uint8_t paralyze_kind = 2;
inline constexpr uint8_t paralyze_order_kind = 27; // Paralyze
inline constexpr uint8_t non_relayed_kind = 11;
// Self-destruct damage; at sim::unit_health::damage_scaling_limit, so armour never scales it.
inline constexpr int32_t self_destruct_damage = 30000;
// What kills a carried unit with its transport, a captured unit and a
// cancelled nanoframe, before the damage path's veteran scaling.
inline constexpr int32_t lethal_damage = 30000;
// Aircraft variants the command resolver picks for CAN_FLY units.
inline constexpr uint8_t vtol_follow_kind = 49;
inline constexpr uint8_t vtol_pickup_kind = 57;
inline constexpr uint8_t vtol_unload_kind = 65;
inline constexpr uint8_t weapon_hit_kind = 1;
// Scale of the HitByWeapon cosine and sine arguments.
inline constexpr int32_t hit_script_scale = 400;
inline constexpr uint32_t dummy_type_index = 0; // the reserved type of Game.unit_defs
// Unit.flags bits read by the economy tick.
inline constexpr uint32_t cloak_running_flag = OA_UNIT_FLAG_CLOAK_RUNNING; // cloak active state
inline constexpr uint32_t cloak_locked_flag = OA_UNIT_FLAG_CLOAK_LOCKED;   // no auto-deactivate
inline constexpr uint32_t moving_rate_mask = OA_UNIT_FLAG_MOVE_RATE_MASK;  // the move rate
inline constexpr uint32_t moving_rate_shift = 2;
inline constexpr uint8_t order_building_flag = 0x40;              // order flags; StartBuilding ran
inline constexpr uint8_t active_state_bit = OA_UNIT_STATE_ACTIVE; // Unit.state_flags
// Words of Unit.economy, an energy then a metal ResourceAccumulator: produced,
// requested, accepted, gate, last produced and last requested of each.
inline constexpr std::size_t produced_word = 0;
inline constexpr std::size_t requested_word = 1;
inline constexpr std::size_t accepted_word = 2;
inline constexpr std::size_t gate_word = 3;
inline constexpr std::size_t previous_produced_word = 4;
inline constexpr std::size_t previous_requested_word = 5;
inline constexpr std::size_t metal_block_word = 6; // the first word of the metal accumulator
inline constexpr std::size_t metal_requested_word = 7;
inline constexpr std::size_t metal_accepted_word = 8;
inline constexpr std::size_t metal_gate_word = 9;
inline constexpr std::size_t metal_previous_produced_word = 10;
inline constexpr std::size_t metal_previous_requested_word = 11;

/// Compares a name with an upper-case name, folding the name's ASCII letters to
/// upper case.
///
/// @param candidate name in any case
/// @param want name in upper case
/// @return true when they hold the same letters
inline bool name_equals_upper(std::string_view candidate, std::string_view want) {
    if (candidate.size() != want.size())
        return false;
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        auto a = candidate[i];
        if (a >= 'a' && a <= 'z')
            a = static_cast<char>(a - 'a' + 'A');
        if (a != want[i])
            return false;
    }
    return true;
}

/// Finds a loaded unit type by name, ignoring the case of the loaded names.
///
/// @param input the match's inputs, whose loaded types are searched from index 1
/// @param want unit name in upper case
/// @return the type index, or 0 when no loaded type has the name
inline uint16_t find_loaded_type(const OfflineInputs& input, std::string_view want) {
    for (std::size_t i = 1; i < input.loaded.size(); ++i) {
        if (name_equals_upper(input.loaded[i].unit_name, want))
            return static_cast<uint16_t>(i);
    }
    return 0;
}

/// Returns the first unit a side's starting factory builds.
///
/// @param name factory unit name, in any case
/// @return the product's unit name; empty for any other unit
inline std::string_view factory_product(std::string_view name) {
    if (name_equals_upper(name, "ARMLAB"))
        return "ARMPW";
    if (name_equals_upper(name, "CORLAB"))
        return "CORAK";
    if (name_equals_upper(name, "ARMVP"))
        return "ARMFLASH";
    if (name_equals_upper(name, "CORVP"))
        return "CORGATOR";
    if (name_equals_upper(name, "ARMAP"))
        return "ARMFIG";
    if (name_equals_upper(name, "CORAP"))
        return "CORVENG";
    if (name_equals_upper(name, "ARMSY"))
        return "ARMPT";
    if (name_equals_upper(name, "CORSY"))
        return "CORPT";
    return {};
}

} // namespace oa::sim::match_runtime::tick_detail

namespace oa::sim::match_runtime {

using namespace tick_detail;

/// Returns the rate signal_move_rate keeps in the unit's
/// OA_UNIT_FLAG_MOVE_RATE_MASK bits.
///
/// @param movement The unit's movement object after the tick.
/// @param unit The moving unit.
/// @param def Its type.
/// @return 0 for a carried unit, a movement object whose flags hold the
///     rate-held bit (0x04), or no speed and no turn; otherwise 1, 2 past
///     moverate1 and 3 past moverate2 (signed compares).
inline uint32_t movement_rate(
    const sim::unit_movement::Movement& movement, const oa::Unit& unit, const oa::UnitDef& def
) noexcept {
    constexpr uint8_t rate_held = 0x04; // Movement.flags
    if ((movement.flags & rate_held) != 0 || unit.attach_parent != 0 ||
        (movement.speed == 0 && movement.turn == 0))
        return 0;
    if (!(def.move_rate1 < movement.speed))
        return 1;
    return def.move_rate2 < movement.speed ? 3 : 2;
}

class TickHost final : public sim::simulation_state::Host,
                       public sim::ground_orders::Host,
                       public sim::unit_movement::Host {
    Match& match;
    std::array<uint8_t, 3>* dispatched_flags{};
    sim::unit_spawn::Slot* dispatched_slot{};

    /// Writes the weapon flags a mission handler holds into its unit's record
    /// before a callback that reads them.
    void before_callback();

    /// Reads the dispatched unit's weapon flags back after a callback, and projects
    /// its record into its movement object when it has one.
    void after_callback();

    /// Returns the pool slot of a unit view.
    ///
    /// @param u unit view
    /// @return its slot
    sim::unit_spawn::Slot& slot(sim::simulation_state::Unit& u);

    /// Returns the unit view of a unit record.
    ///
    /// @param record unit record of this match's world
    /// @return its view
    sim::simulation_state::Unit& unit_view(oa::Unit& record);

    /// Returns a unit's movement object.
    ///
    /// @param u unit view
    /// @return the movement object; for a unit without one, which is noted, a
    ///     fresh spare bound to no slot
    sim::ground_orders::GroundRuntime& ground(sim::simulation_state::Unit& u);

    /// Returns the match's record of an order.
    ///
    /// @param order an order
    /// @return its record; for an order the match does not own, which is noted,
    ///     the cleared spare order
    Match::RuntimeOrder& owned(sim::simulation_state::Order& order);

    /// Copies a unit's three weapon slot flags.
    ///
    /// @param s the unit
    /// @return UnitWeapon.flags of slots 0..2
    std::array<uint8_t, 3> weapon_flags(sim::unit_spawn::Slot& s);

    /// Writes a unit's three weapon slot flags.
    ///
    /// @param[out] s the unit
    /// @param flags UnitWeapon.flags of slots 0..2
    void write_flags(sim::unit_spawn::Slot& s, const std::array<uint8_t, 3>& flags);

    /// Brings every stood-down weapon slot back inside a mission handler that holds
    /// a copy of the weapon flags.
    ///
    /// The copy is written first and reloaded after, so the handler's final write
    /// keeps the cleared bits.
    ///
    /// @param[in,out] s the unit
    /// @param[in,out] flags the handler's copy of the weapon flags
    void release_tracked_weapons(sim::unit_spawn::Slot& s, std::array<uint8_t, 3>& flags);

    /// Builds the ground-order view of a unit: its record, movement object state,
    /// carried state and weapon flags.
    ///
    /// @param s the unit
    /// @param g its movement object
    /// @param[in,out] flags the weapon flags the view's handlers change
    /// @return the view, which refers to its arguments
    sim::ground_orders::UnitView view(
        sim::unit_spawn::Slot& s,
        sim::ground_orders::GroundRuntime& g,
        std::array<uint8_t, 3>& flags
    );

    /// Leaves the ground for an order: weapons back on, off any carrier and
    /// active; a landed unit also climbs to half its cruise altitude over
    /// where it stands, and the order waits for the climb.
    ///
    /// @param s Unit taking off.
    /// @param[in,out] order Order it takes off for; its air goal and wait
    ///     events change when the unit was landed.
    void take_off(sim::unit_spawn::Slot& s, sim::simulation_state::Order& order);

    /// Replaces an order's aircraft goal with a point goal, or clears it.
    ///
    /// Circle goals carry no altitude. A unit without a movement object is left
    /// alone.
    ///
    /// @param s flying unit
    /// @param[in,out] order order that owns the goal
    /// @param point signed 16.16 goal point, or null to clear the goal
    /// @param arrival_radius arrival radius in world units; 0 means the unit's cell
    void set_aircraft_goal(
        sim::unit_spawn::Slot& s,
        sim::simulation_state::Order& order,
        const sim::ground_orders::Point* point,
        int32_t arrival_radius
    );

    /// Replaces an order's construction target, keeping the target observer
    /// chain in step.
    ///
    /// @param[in,out] entry Order whose target changes.
    /// @param unit New target, or null for none.
    void retarget(Match::RuntimeOrder& entry, sim::simulation_state::Unit* unit);

    /// Hands a copy of `goal` to the unit's air driver as the order's goal,
    /// dropping the order's previous air goal first.
    ///
    /// @param s Flying unit.
    /// @param[in,out] order Order that owns the goal; its goal events are
    ///     cleared when a goal is installed.
    /// @param goal Goal to copy, or null to leave the driver without one.
    void install_air_goal(
        sim::unit_spawn::Slot& s, sim::simulation_state::Order& order, const sim::air::AirGoal* goal
    );

    /// Starts an engine function's script on a unit with no arguments,
    /// stepping it at once; nothing happens without a script.
    ///
    /// @param s the unit
    /// @param function engine function whose exact COB name is started
    void script(sim::unit_spawn::Slot& s, ScriptFunction function);

    /// UnitDef.abilities of the unit, zero when it has no type.
    uint32_t unit_abilities(const oa::Unit& unit) const;

    /// Returns the pool slot of a movement unit.
    ///
    /// @param u movement-side unit, whose id is its slot
    /// @return its slot; reserved slot 0 for an id outside the pool, which is noted
    sim::unit_spawn::Slot& movement_slot(const sim::unit_movement::Unit& u);

    class AttackAdapter;

    class ConstructionAdapter;

    class PatrolAdapter;

    class HealthHost;

    // Mission handler families dispatched through the mission table. Each
    // returns true and stores the handler result when it owns order.kind.
    class GroundMissions;    // tick_missions_ground.cpp
    class VtolMissions;      // tick_missions_vtol.cpp
    class VtolBuildMissions; // tick_missions_vtol_build.cpp
    class AirAttackMissions; // tick_missions_air.cpp

    /// Runs the ground mission handler for an order's kind, when it is one.
    ///
    /// @param s the unit
    /// @param[in,out] order order to step
    /// @param events events that woke the order
    /// @param[out] result the handler's step result, when it ran
    /// @return true when a ground handler owns the kind
    bool dispatch_ground_mission(
        sim::unit_spawn::Slot& s,
        sim::simulation_state::Order& order,
        uint32_t events,
        uint32_t& result
    );

    /// Runs the aircraft mission handler for an order's kind, when it is one.
    ///
    /// @param s the unit
    /// @param[in,out] order order to step
    /// @param events events that woke the order
    /// @param[out] result the handler's step result, when it ran
    /// @return true when an aircraft handler owns the kind
    bool dispatch_vtol_mission(
        sim::unit_spawn::Slot& s,
        sim::simulation_state::Order& order,
        uint32_t events,
        uint32_t& result
    );

    /// Runs the aircraft construction handler for an order's kind, when it is one.
    ///
    /// @param s the unit
    /// @param[in,out] order order to step
    /// @param events events that woke the order
    /// @param[out] result the handler's step result, when it ran
    /// @return true when an aircraft construction handler owns the kind
    bool dispatch_vtol_build_mission(
        sim::unit_spawn::Slot& s,
        sim::simulation_state::Order& order,
        uint32_t events,
        uint32_t& result
    );

    /// Runs the aircraft attack handler for an order's kind, when it is one.
    ///
    /// @param s the unit
    /// @param[in,out] order order to step
    /// @param events events that woke the order
    /// @param[out] result the handler's step result, when it ran
    /// @return true when an aircraft attack handler owns the kind
    bool dispatch_air_attack_mission(
        sim::unit_spawn::Slot& s,
        sim::simulation_state::Order& order,
        uint32_t events,
        uint32_t& result
    );

  public:

    /// Binds the host to a match for one simulation pass and hands the
    /// match's rules to the systems it serves.
    ///
    /// @param m the match
    explicit TickHost(Match& m) : match(m) {
        sim::simulation_state::Host::rules = m.rules_view();
        sim::simulation_state::Host::order_ends = {&m, &Match::report_order_end};
        sim::ground_orders::Host::rules = m.rules_view();
    }

    /// Moves a queued build onto a new site. Match::relocate_queued_build.
    ///
    /// @param unit Builder slot.
    /// @param position Index of the order in the primary queue, head 0.
    /// @param destination Signed 16.16 point the site is taken from.
    /// @return False when nothing was moved.
    bool relocate_queued_build(
        uint16_t unit, std::size_t position, const sim::ground_orders::Point& destination
    );

    /// Returns the world queries the air goals and the air driver make, over this
    /// match.
    ///
    /// @return the air host, which refers to the match
    sim::air::AirHost air_host();

    /// Hands an order's goal to the unit's movement object: its ground navigator
    /// for a ground goal, its air driver for an air goal.
    ///
    /// A ground goal for a unit without a movement object is left alone.
    ///
    /// @param s the unit
    /// @param record the order's record
    void install_order_goal(sim::unit_spawn::Slot& s, Match::RuntimeOrder& record);

    /// Runs StopBuilding when the order started building, shares the start
    /// with the other players and clears the order's building bit
    /// (order_building_flag); nothing happens for an order that is not
    /// building.
    ///
    /// @param s Builder.
    /// @param[in,out] order Its order.
    void stop_building(sim::unit_spawn::Slot& s, sim::simulation_state::Order& order);

    /// Sends a unit to a point ahead of its orders, as the build-site
    /// kickout sends a unit off a site (Match::send_ahead_of_orders).
    ///
    /// @param s the unit, one with a movement object
    /// @param point signed 16.16 point; its whole x, y and z are the move's
    void send_ahead(sim::unit_spawn::Slot& s, const sim::ground_orders::Point& point);

    /// Returns how the ground steering treats a unit faster than any stock
    /// unit: held to its route in a match that applied mutators (it holds a
    /// mutator digest), as 3.1c steers it in any other.
    ///
    /// @return the steering for this match's fast units
    [[nodiscard]] sim::ground_orders::FastUnitSteering fast_unit_steering() const noexcept {
        return match.mutator_digest_held() ? sim::ground_orders::FastUnitSteering::held_to_route
                                           : sim::ground_orders::FastUnitSteering::as_3_1c;
    }

    /// Moves the unit's movement-rate bits (OA_UNIT_FLAG_MOVE_RATE_MASK) to its
    /// movement_rate and, on a change, runs StopMoving (rate 0), or
    /// StartMoving when the old rate was 0, then MoveRate1..3.
    ///
    /// @param s Moving unit.
    /// @param movement Its movement object after the tick.
    void signal_move_rate(sim::unit_spawn::Slot& s, const sim::unit_movement::Movement& movement);

    /// Draws from the match's shared random stream.
    ///
    /// @param n exclusive upper limit
    /// @return a value below n, or 0 for a limit below 2
    uint32_t random_bounded(uint32_t n) override;

    /// Draws from the match's shared random stream, as random_bounded does.
    ///
    /// @param n exclusive upper limit
    /// @return a value below n, or 0 for a limit below 2
    uint32_t random(uint32_t n) override { return random_bounded(n); }

    /// Steps a unit's script contexts.
    ///
    /// @param record the unit
    /// @param steps clock time since the last step
    void tick_script(oa::Unit& record, uint32_t steps) override;

    /// Hands a wind generator the new wind when it changed this tick:
    /// SetDirection(direction) then SetSpeed(speed << 4).
    ///
    /// @param record Unit about to tick; only a wind generator is affected,
    ///     and one without a script is noted.
    void update_wind_generator(oa::Unit& record) override;

    /// Runs one tick of a unit's three weapon slots: aim, reload and fire.
    ///
    /// @param record the unit
    void tick_weapon_aim(oa::Unit& record) override;

    /// Clears a weapon slot's target and runs TargetCleared, keeping a running
    /// handler's weapon flags in step.
    ///
    /// @param u the unit
    /// @param index weapon slot 0..2
    void wake_weapon(sim::simulation_state::Unit& u, uint32_t index) override;

    /// Clears a weapon slot's target, as wake_weapon does.
    ///
    /// @param record the unit
    /// @param index weapon slot 0..2
    void clear_weapon_target(oa::Unit& record, uint32_t index) override {
        wake_weapon(unit_view(record), index);
    }

    /// Queues a unit's type's default mission when the match runs its handler.
    ///
    /// A unit without a movement object gets only GetBuilt.
    ///
    /// @param record the unit, whose queue is empty
    void queue_default_mission(oa::World&, oa::Unit& record) override;

    /// Runs one natural repair step of a builder on a unit, paying energy for it.
    ///
    /// The energy is requested in the repairer's economy block (Unit.economy),
    /// so the repairer's owner pays and the repairer's gate decides whether the
    /// step is afforded, whoever owns the patient. A repair pad pays for the
    /// aircraft it repairs.
    ///
    /// @param repairer_slot repairing unit
    /// @param patient repaired unit
    /// @return true when the step was paid for; false, which is noted, when either
    ///     unit or its definition is missing
    bool nano_repair(sim::unit_spawn::Slot& repairer_slot, sim::simulation_state::Unit& patient);

    /// Runs the handler the mission table holds for the order's kind.
    ///
    /// @param world Canonical world (unused; the match's own is used).
    /// @param record Unit the order belongs to.
    /// @param[in,out] order Order to step; its phase and wait fields change.
    /// @param events Events that woke the order; tearing the order down
    ///     passes event 2.
    /// @return The handler's step result (see the ground:: result codes).
    uint32_t dispatch_mission(
        oa::World& world, oa::Unit& record, sim::simulation_state::Order& order, uint32_t events
    ) override;

    /// Tears an order down: its cleanup event when it waits for one,
    /// StopBuilding, the movement and air goals it owns, the stand-down of
    /// every weapon slot unless the order keeps them (flags bit 0), and its
    /// link in the target's observer chain; then frees it.
    ///
    /// @param record Unit the order belongs to.
    /// @param order Order to destroy; it is freed.
    void destroy_order(oa::Unit& record, sim::simulation_state::Order& order) override;

    /// Returns the terrain height under a unit.
    ///
    /// @param record the unit
    /// @return height in whole world units
    int32_t terrain_height_under(oa::Unit& record) override;

    /// Fits a unit with a movement object to the ground under its model.
    ///
    /// A unit without a model, or a hovering one without the platform clock, is
    /// noted and left as it is.
    ///
    /// @param record the unit
    void settle_on_ground(oa::Unit& record) override;

    /// Runs one movement tick of a unit: its air driver, ground or flight
    /// steering, its position (following the carrier's piece when carried),
    /// then the move-rate and setSFXoccupy scripts.
    ///
    /// @param record Unit to move; one without a movement object is skipped.
    void movement_tick(oa::Unit& record) override;

    /// Drops the path search job a navigator owns.
    ///
    /// @param navigation the navigator
    void cancel_search(sim::ground_orders::Navigation& navigation) override;

    /// Plays a unit's speech category through the application's command sound.
    ///
    /// @param u speaking unit
    /// @param category speech category
    void play_sound(sim::simulation_state::Unit& u, uint32_t category) override;

    /// Plays a unit's speech captioned with its order's own text.
    ///
    /// Without a caption, or without the application's speak hook, the
    /// category plays alone as play_sound(u, category) does.
    ///
    /// @param u Speaking unit.
    /// @param category Speech category.
    /// @param caption The order's caption, or null for the category's own.
    void play_sound(sim::simulation_state::Unit& u, uint32_t category, const char* caption);

    /// Searches an automatic target for a unit firing at will.
    ///
    /// @param unit the unit
    /// @return the target, or null
    sim::simulation_state::Unit* find_target(sim::simulation_state::Unit& unit) override;

    /// Sends a unit after another on its own account.
    ///
    /// @param from attacker
    /// @param to target
    /// @return true when an attack order was committed
    bool issue_attack(sim::simulation_state::Unit& from, sim::simulation_state::Unit& to) override;

    /// Tests whether a moving unit fits a cell on an occupancy layer.
    ///
    /// @param u moving unit
    /// @param cell footprint cell x and z
    /// @param mode occupancy layer
    /// @return true when it fits; false, which is noted, without the type's
    ///     metadata or the collision plots
    bool can_occupy(
        const sim::unit_movement::Unit& u, std::array<int16_t, 2> cell, uint8_t mode
    ) override;

    /// Takes a moving unit's footprint off the map; a removal the spatial state
    /// rejects is noted.
    ///
    /// @param u moving unit
    void remove_occupancy(sim::unit_movement::Unit& u) override;

    /// Puts a moving unit's footprint back on the map.
    ///
    /// @param u moving unit
    void insert_occupancy(sim::unit_movement::Unit& u) override;

    /// Moves a moving unit's sight stamp to where it now stands.
    ///
    /// @param u moving unit
    void update_spatial_membership(sim::unit_movement::Unit& u) override;

    /// Scales damage by the target's armour and veteran level and applies it,
    /// sharing it when the target is simulated elsewhere.
    ///
    /// @param source Attacker, or null for none.
    /// @param target Unit that takes the damage.
    /// @param amount Damage before scaling, in health points.
    /// @param kind Damage kind (1 weapon, 2 paralyze, 10 healing, or a
    ///     DeathKind for a kill).
    void scaled_damage(
        sim::unit_spawn::Slot* source, sim::unit_spawn::Slot& target, int32_t amount, uint32_t kind
    );

    /// Deals damage to a unit with no attacker, scaled as scaled_damage scales it.
    ///
    /// @param record unit that takes the damage
    /// @param amount damage before scaling, in health points
    /// @param kind damage kind
    void apply_scaled_damage(oa::Unit& record, int32_t amount, uint32_t kind) override;

    /// Runs one natural regeneration step of a unit on itself.
    ///
    /// @param record the unit
    void regenerate_health(oa::Unit& record) override;

    /// Works out a live unit's Killed percentage and wreck level, drops a
    /// last attacker whose player's slot is free, shares the death through
    /// MultiplayerHooks::unit_killed when the unit is simulated here, clears
    /// its orders and hands them to the kill handler, then runs the
    /// commander rule's sweep when the dead commander was simulated here.
    ///
    /// The Killed percentage divides by the type's maximum health; a type with
    /// none (MaxDamage 0), for which 3.1c has no percentage, takes a quotient
    /// of 0, so its unit dies as any other on its first hit.
    ///
    /// @param record Dying unit; one no longer live is skipped.
    /// @param kind DeathKind value of the death, or 0 for a unit whose slot
    ///     another player's new unit takes.
    /// @quirk An unfinished unit leaves no wreck whatever Killed returns.
    void kill_unit(oa::Unit& record, uint8_t kind) override;

    /// Makes every live unit of the player not already dying self-destruct:
    /// one simulated here takes the self-destruct damage from itself, any
    /// other explodes and dies at once.
    ///
    /// @param owner Player index 0..9; nothing happens once the player has
    ///     no units.
    void destroy_player_units(uint8_t owner);

    /// Tells the other players' machines that a player simulated here has ticked;
    /// without a multiplayer handler this is noted.
    void local_player_ticked(oa::Player&) override;

    /// Answers the unit sweep's key-state test: the observer's Shift state for the
    /// Shift code, held for any other.
    ///
    /// @return true when the key counts as held
    bool is_key_down(uint32_t) override;

    /// Selects the viewpoint player's next unit, as the periodic follow does.
    void select_next_viewpoint_unit() override;

    /// Moves the view to the next selected unit, or the previous one for a nonzero
    /// argument.
    void follow_next_selected(uint32_t) override;
};

class TickHost::AttackAdapter final : public AttackOrderHost {
    TickHost& host;
    sim::unit_spawn::Slot& source;
    Match::RuntimeOrder& record;

    /// Replaces the order's ground goal, detaching the old one from the navigator
    /// first.
    ///
    /// @param goal new goal, or null to leave the order without one
    void set_goal(std::unique_ptr<sim::ground_orders::Goal> goal);

  public:

    /// Binds the adapter to an attacking unit and its order.
    ///
    /// @param h the tick host
    /// @param s attacking unit
    /// @param r its order's record
    AttackAdapter(TickHost& h, sim::unit_spawn::Slot& s, Match::RuntimeOrder& r)
        : host(h), source(s), record(r) {}

    /// Plays the acknowledgement of an order given without the queue key, once.
    ///
    /// @param caption the order's caption
    void announce(const char* caption) override;

    /// Returns the weapon slot an attack uses when the order names none.
    ///
    /// @return The first enabled slot, 0 or 1; otherwise the third slot's
    ///     enabled bit itself, 2 when set and 0 when no slot is enabled.
    uint8_t selected_weapon() override;

    /// Brings the stood-down weapon slots back.
    ///
    /// @param i weapon slot 0..2, or 3 for all three; another is noted
    void release_weapon_targets(uint32_t i) override;

    /// Aims a weapon slot at a unit.
    ///
    /// @param target the target
    /// @param index weapon slot 0..2; another is noted
    void assign_target(sim::simulation_state::Unit& target, int32_t index) override;

    /// Stands every enabled weapon slot down and drops its target.
    void reset_weapons() override;

    /// Tests whether a weapon slot reaches a unit.
    ///
    /// @param target the target
    /// @param index weapon slot 0..2
    /// @return true when in reach
    bool can_reach(sim::simulation_state::Unit& target, uint8_t index) override;

    /// Returns the range of the unit's weapon in a slot (WeaponDef.range).
    ///
    /// @param index Weapon slot 0..2.
    /// @return Range in world units.
    int32_t range(uint8_t index) override;

    /// Leaves the order without a ground goal.
    void clear_goal() override;

    /// Gives the order a ground goal around a point.
    ///
    /// @param point signed 16.16 centre
    /// @param radius goal radius in world units
    void circle_goal(const AttackPoint& point, int32_t radius) override;

    /// Draws from the match's shared random stream.
    ///
    /// @param limit exclusive upper limit
    /// @return a value below limit, or 0 for a limit below 2
    uint32_t random(uint32_t limit) override;

    /// Resolves the attack command again for the unit and a target, as the
    /// command resolver does.
    ///
    /// @param target the target, or null for open ground
    /// @return the mission kind the command now gives, or 0 for none
    uint8_t morph_attack_command(sim::simulation_state::Unit* target) override;

    /// Aims a weapon slot at a point.
    ///
    /// @param point signed 16.16 point
    /// @param slot weapon slot 0..2; another is noted
    void assign_ground(const AttackPoint& point, int32_t slot) override;
};

class TickHost::ConstructionAdapter {
    TickHost& host;
    sim::unit_spawn::Slot& source;
    Match::RuntimeOrder& record;

    /// Returns the health fields a build step reads of a unit's type.
    ///
    /// @param s the unit
    /// @return armour, energy and metal cost, build time and maximum health
    sim::unit_health::UnitType health_type(sim::unit_spawn::Slot& s) const;

  public:

    /// Binds the adapter to a building unit and its order.
    ///
    /// @param h the tick host
    /// @param s building unit
    /// @param r its order's record
    ConstructionAdapter(TickHost& h, sim::unit_spawn::Slot& s, Match::RuntimeOrder& r)
        : host(h), source(s), record(r) {}

    /// Flags the build panel for a redraw when the viewpoint player has the unit
    /// selected.
    void refresh_selected();

    /// Tests the site of the order's type at its snapped destination.
    ///
    /// @return true when the type can be placed there
    bool site_clear();

    /// Snaps a building's site to the footprint grid and sets its height to
    /// the one its yard takes there; other types are left alone.
    ///
    /// Changes the order's destination.
    void snap_build_height();

    /// Creates an unfinished unit of the order's type at its destination.
    ///
    /// @return the frame, or null when it could not be created
    sim::simulation_state::Unit* spawn_nanoframe();

    /// Puts a GetBuilt order aimed at the builder at the head of a frame's queue.
    ///
    /// @param nanoframe the unfinished unit
    void issue_get_built(sim::simulation_state::Unit& nanoframe);

    /// Starts StartBuilding(heading) on the builder's script, shares the
    /// start with the other players (share_named_script_start, one argument:
    /// the heading zero-extended from 16 bits) and marks the order as
    /// building (order_building_flag).
    ///
    /// A builder whose script cannot start it raises its own build stance.
    ///
    /// @param heading Direction to the site in 65536ths of a turn.
    void start_building(int16_t heading);

    /// Adds worker time to an unfinished unit, or takes it away when negative,
    /// without the nano spray.
    ///
    /// @param nanoframe the unfinished unit
    /// @param rate worker time this step
    /// @return true when the builder could pay for the step
    bool build_progress(sim::simulation_state::Unit& nanoframe, float rate);

    /// Runs build_progress, with the build nano spray when the step was paid for.
    ///
    /// @param nanoframe the unfinished unit
    /// @param rate worker time this step
    /// @return true when the builder could pay for the step
    bool construct(sim::simulation_state::Unit& nanoframe, float rate);

    /// Marks a frame finished by the builder and releases it from the pad, when
    /// both units are live.
    ///
    /// Its build-list test on the source holds for any factory; the match leaves
    /// UnitDef.build_ids unset.
    ///
    /// @param nanoframe the finished unit
    void link_built(sim::simulation_state::Unit& nanoframe);

    /// Returns the rate at which an unattended frame of the source's type decays.
    ///
    /// @param ticks ticks the decay takes
    /// @return a negative worker-time rate
    float build_decay_rate(int32_t ticks) const;

    /// Returns the facing the order's building is placed in: the order's
    /// (ConstructionOrderState::facing) when its type may take it
    /// (Match::build_facing), else south.
    ///
    /// @return quarter turns from south, 0 to 3
    uint8_t facing() const;

    /// Returns the footprint width of the order's type, facing as it is
    /// placed: its depth facing east or west.
    ///
    /// @return width in cells; 0, which is noted, for a type that is not loaded
    int16_t footprint_x() const;

    /// Returns the footprint depth of the order's type, facing as it is
    /// placed: its width facing east or west.
    ///
    /// @return depth in cells; 0, which is noted, for a type that is not loaded
    int16_t footprint_z() const;

    /// Runs QueryBuildInfo for the build pad's position, keeping the pad piece
    /// on the order.
    ///
    /// @param[out] pad signed 16.16 world position of the pad piece; the unit's
    ///     own when its script has none or answers below zero
    /// @return true
    bool query_build_pad(sim::ground_orders::Point& pad);

    /// Carries a unit on the factory pad of the order's owner.
    ///
    /// @param child carried unit
    /// @param piece QueryBuildInfo piece
    /// @param mode carry mode; 1 for BuildingBuild
    void attach_child(sim::simulation_state::Unit& child, int8_t piece, uint8_t mode);

    /// Sets or clears state flags, raising the build stance a unit without a
    /// script never raises itself.
    ///
    /// @param mask state flag bits
    /// @param enabled true sets them, false clears them
    void set_activation_mask(uint8_t mask, bool enabled);
};

class TickHost::PatrolAdapter {
    TickHost& host;
    sim::unit_spawn::Slot& source;
    Match::RuntimeOrder& record;

  public:

    /// Binds the adapter to a patrolling unit and its order.
    ///
    /// @param h the tick host
    /// @param s patrolling unit
    /// @param r its order's record
    PatrolAdapter(TickHost& h, sim::unit_spawn::Slot& s, Match::RuntimeOrder& r)
        : host(h), source(s), record(r) {}

    /// Queues a copy of the patrol at the unit's position unless an order in
    /// the queue already carries the clone mark (command flags bit 0x80),
    /// then marks this order.
    ///
    /// @quirk The copy takes its command flags from the kind's descriptor, so
    ///     it is left unmarked.
    void clone_patrol();
};

class TickHost::HealthHost final : public sim::unit_health::ConstructionHost {
    Match& match_;
    sim::unit_health::EconomyDebit debit_;
    sim::unit_health::EconomyDebit metal_;
    oa::UnitEconomy& economy_;
    sim::unit_health::Unit* stand_in_{}; // the projection bind() names; null for none

    /// Returns the pool slot of a health projection.
    ///
    /// @param u projection, whose identity is its slot
    /// @return its slot
    sim::unit_spawn::Slot& slot(const sim::unit_health::Unit& u);

    /// Copies the bound projection's health, flags, events and build fraction
    /// into its unit's record; any other projection is left alone.
    ///
    /// @param u projection the host is about to act on
    void write_stand_in(const sim::unit_health::Unit& u);

    /// Reads the bound projection's health, flags, events and build fraction
    /// back from its unit's record; any other projection is left alone.
    ///
    /// @param[in,out] u projection the host has just acted on
    void read_stand_in(sim::unit_health::Unit& u);

  public:

    /// Binds the host to a unit's economy block for one health or build step.
    ///
    /// @param match the match
    /// @param index the paying unit's slot
    HealthHost(Match& match, uint16_t index)
        : match_(match), economy_(match_unit(match, index).economy) {
        rules = match.rules_view();
        repair_remainders = match.repair_remainders();
        debit_ = {economy_.energy.requested, economy_.energy.accepted, economy_.energy.gate};
        metal_ = {economy_.metal.requested, economy_.metal.accepted, economy_.metal.gate};
    }

    /// Writes the energy and metal debits back into the unit's economy block.
    void store();

    /// Lets a projection stand in for its unit's record while a build step
    /// changes it.
    ///
    /// A health event or a completion the step raises acts on the record
    /// itself, so the host first writes the projection's health, flags,
    /// events and build fraction into the record, as the step has already
    /// changed them, and afterwards reads them back; the caller then stores
    /// what the event left, a death included.
    ///
    /// @param[in,out] projection projection of a unit record; it outlives the
    ///        host's use of it
    void bind(sim::unit_health::Unit& projection) noexcept { stand_in_ = &projection; }

    /// Returns the energy debit of the bound unit's economy block.
    ///
    /// @return the debit, written back by store
    sim::unit_health::EconomyDebit& energy_debit(sim::unit_health::Unit&) override;

    /// Returns the metal debit of the bound unit's economy block.
    ///
    /// @return the debit, written back by store
    sim::unit_health::EconomyDebit& metal_debit(sim::unit_health::Unit&) override;

    /// Credits metal to a unit's owner.
    ///
    /// @param target unit whose owner is credited
    /// @param amount metal credited
    void refund_metal(sim::unit_health::Unit& target, float amount) override;

    /// Marks a frame finished by its builder, through the bound projection.
    ///
    /// @param target the finished unit
    void complete_construction(sim::unit_health::Unit&, sim::unit_health::Unit& target) override;

    /// Tests whether a unit can take damage.
    ///
    /// @param u projection of the unit
    /// @return true for a live target
    bool target_is_live(const sim::unit_health::Unit& u) override;

    /// Applies a health event to a unit, through the bound projection.
    ///
    /// @param target unit hit
    /// @param source unit responsible, or null
    /// @param event amount, kind and direction
    void apply_health_event(
        sim::unit_health::Unit& target,
        const sim::unit_health::Unit* source,
        const sim::unit_health::HealthEvent& event
    ) override;

    /// Tests whether a unit's owner is present.
    ///
    /// @param u projection of the unit
    /// @return true when it has an owner in use
    bool target_owner_present(const sim::unit_health::Unit& u) override;

    /// Returns the status of a unit's owner.
    ///
    /// @param u projection of the unit
    /// @return Player.status, or 0 without an owner
    uint8_t target_owner_status(const sim::unit_health::Unit& u) override;

    /// Returns the route a health event takes to the attacker's owner.
    ///
    /// @return the multiplayer handler's route; none, which is noted, without one
    sim::unit_health::RouteIdentity source_owner_route(const sim::unit_health::Unit&) override;

    /// Returns the route a health event with no attacker takes.
    ///
    /// @return the multiplayer handler's route; none, which is noted, without one
    sim::unit_health::RouteIdentity fallback_route() override;

    /// Shares a health event with the machine that simulates its target; without
    /// a multiplayer handler this is noted.
    void share_health_event(
        sim::unit_health::RouteIdentity, const sim::unit_health::HealthEvent&
    ) override;
};

} // namespace oa::sim::match_runtime
