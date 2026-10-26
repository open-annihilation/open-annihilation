// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The ground and command mission family: one instance serves a single
// dispatch of one order through the mission table.
#pragma once

#include "tick_internal.hpp"
#include "oa/base/game_math.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace oa::sim::match_runtime {
using base::game_math::truncate_low32;

namespace ground {

constexpr uint8_t attack_kamikaze_kind = 7;
constexpr uint8_t attack_unit_type_kind = 10;
constexpr uint8_t build_weapon_kind = 13;
constexpr uint8_t guard_no_move_kind = 22;
constexpr uint8_t help_build_kind = 23;
constexpr uint8_t park_kind = 28;
constexpr uint8_t reclaim_kind = 32;
constexpr uint8_t repair_patrol_kind = 34;
constexpr uint8_t repair_unit_kind = 35;
constexpr uint8_t resurrect_kind = 37;
constexpr uint8_t standby_mine_kind = 42;
constexpr uint8_t vtol_help_build_kind = 51;
constexpr uint8_t vtol_repair_unit_kind = 61;
constexpr uint8_t patrol_kind = 29;
constexpr uint8_t vtol_patrol_kind = 56;
constexpr uint8_t vtol_repair_patrol_kind = 60;

// What a builder on a repair patrol looks for (orders.con-patrol-guard-options).
constexpr uint8_t patrol_reclaim_only = 0; // skips the repair and assist search
constexpr uint8_t patrol_both = 1;         // as in 3.1c
constexpr uint8_t patrol_assist_only = 2;  // skips the reclaim search
// Where a guarding unit stands (orders.con-patrol-guard-options).
constexpr uint8_t guard_stay = 0;    // 7/20 of the guard spacing out on each axis
constexpr uint8_t guard_base = 1;    // as in 3.1c
constexpr uint8_t guard_scatter = 2; // the whole guard spacing out on each axis

/// Returns what a unit on a builder patrol looks for under its standing move
/// order, by orders.con-patrol-guard-options: patrol_reclaim_only,
/// patrol_both (3.1c's, also for a standing move order past roam) or
/// patrol_assist_only.
///
/// @param rules the match's rules
/// @param unit the patrolling unit
/// @return the choice
inline uint8_t patrol_choice(const data::match_rules::MatchRules& rules, const oa::Unit& unit) {
    const auto& options = rules.orders.con_patrol_guard_options;
    switch ((unit.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT) {
    case 0:
        return static_cast<uint8_t>(options.patrol_hold_position);
    case 1:
        return static_cast<uint8_t>(options.patrol_maneuver);
    case 2:
        return static_cast<uint8_t>(options.patrol_roam);
    default:
        return patrol_both;
    }
}

/// Returns where a guarding unit stands under its standing move order, by
/// orders.con-patrol-guard-options: guard_stay, guard_base (3.1c's, also
/// without the guard hook and for a standing move order past roam) or
/// guard_scatter.
///
/// @param rules the match's rules
/// @param unit the guarding unit
/// @return the choice
inline uint8_t guard_choice(const data::match_rules::MatchRules& rules, const oa::Unit& unit) {
    const auto& options = rules.orders.con_patrol_guard_options;
    if (!options.guard_hook)
        return guard_base;
    switch ((unit.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT) {
    case 0:
        return static_cast<uint8_t>(options.guard_hold_position);
    case 1:
        return static_cast<uint8_t>(options.guard_maneuver);
    case 2:
        return static_cast<uint8_t>(options.guard_roam);
    default:
        return guard_base;
    }
}

// Results read by the order sweep.
constexpr uint32_t restart_mission = 0;
constexpr uint32_t next_phase = 1;
constexpr uint32_t keep_waiting = 2;
constexpr uint32_t retry_later = 3;
constexpr uint32_t phase_chosen = 4; // the handler set the next phase itself
constexpr uint32_t mission_done = 5;
constexpr uint32_t rotate_mission = 6;
constexpr uint32_t mission_invalid = 7;
constexpr uint32_t mission_failed = 8;
constexpr uint32_t retry_mission = 9;

constexpr uint32_t timer_event = 0x1;
constexpr uint32_t script_value_event = 0x4; // any COB SET, such as build stance or BUSY
constexpr uint32_t target_lost_event = 0x8;
constexpr uint32_t target_lost_events = 0x10008;
constexpr uint32_t arrived_event = sim::ground_orders::arrived_event;
constexpr uint32_t path_failed_event = sim::ground_orders::path_failed_event;
constexpr uint32_t goal_events = sim::ground_orders::arrived_event |
                                 sim::ground_orders::path_failed_event |
                                 sim::ground_orders::goal_replaced_event;
constexpr uint32_t guard_fire_events = 0x7008;   // ?
constexpr uint32_t guard_hold_event = 0x4000;    // ? resets the guard give-up counter
constexpr uint32_t standby_mine_event = 0x10000; // ?
constexpr uint32_t cancel_event = 0x2;           // raised when the order is torn down
constexpr uint32_t build_abort_events = 0xa;     // cancel_event | target_lost_event
constexpr uint32_t repair_step_event = 0x8;
constexpr uint32_t target_attacked_event = 0x10; // the guarded unit took a hit
constexpr uint32_t guard_wait_events = 0x18;
constexpr uint32_t get_built_wake = 0x8000; // build progress was applied to the frame
constexpr uint32_t weapon_out_of_range_events =
    0x3000; // 0x1000: a weapon could not reach; 0x2000 ?
constexpr uint32_t chase_wake_events = 0x40e0;
constexpr uint32_t attack_wait = 0x13808;
constexpr uint32_t attack_in_range_wait = 0x148e8;
constexpr uint32_t attack_out_of_range_wait = 0x100e8;
constexpr uint32_t attack_abort_events = 0x10808;
constexpr uint32_t attack_no_move_wait = 0x11808;
constexpr uint32_t attack_ground_wait = 0x1c00;
constexpr uint32_t suppress_abort_event = 0x800;
constexpr uint32_t suppress_rotate_event = 0x400;
constexpr uint32_t wait_for_attack_events = 0x18;
constexpr uint32_t build_goal_events = 0xe8; // goal events with target_lost_event
constexpr uint32_t reclaim_goal_events = 0x100e8;

constexpr uint32_t speech_order = 5;
constexpr uint32_t speech_kamikaze = 6; // ?
constexpr uint32_t speech_failed = 7;
constexpr uint32_t speech_complete = 8;
constexpr uint32_t speech_build = 9;
constexpr uint32_t speech_repaired = 0x0a;
constexpr uint32_t speech_work_started = 0x0b;   // ?
constexpr uint32_t speech_cargo_loaded = 0x0c;   // the load chatter
constexpr uint32_t speech_cargo_unloaded = 0x0d; // the unload chatter
constexpr uint32_t speech_captured = 0x10;
constexpr uint32_t speech_countdown_zero = 0x16; // count0; count1..count5 run down from 0x15
constexpr uint32_t speech_destruct_cancelled = 0x17;

// Command flags bits a new order keeps only when it carries a target or a point.
constexpr uint8_t order_has_target = 0x02;
constexpr uint8_t order_has_point = 0x04;
constexpr uint8_t order_announce = 0x20;
constexpr uint8_t order_inherited = 0x40;
constexpr uint8_t order_patrol_cloned = 0x80;
constexpr uint8_t order_secondary = 0x04;  // order flags
constexpr uint8_t order_assistable = 0x10; // order flags: guarding builders may join in
constexpr uint8_t order_queue_tail = 0x10; // command flags: last order a queued command follows
constexpr uint8_t order_queued = 0x01;     // preserve flags
constexpr uint8_t order_at_head = 0x20;    // preserve flags: queued ahead of the others
// A kind change keeps these bits of the packed descriptor word (preserve,
// command and order flags, low byte first).
constexpr uint32_t kept_descriptor_bits = 0x600;

constexpr uint8_t build_stance_flag = 0x01; // unit build_flags
constexpr uint8_t busy_flag = 0x02;         // unit build_flags, COB BUSY
constexpr uint32_t occupancy_ground = 1;
constexpr uint32_t occupancy_air = 2;

constexpr uint32_t guard_wait = 0x1e;
constexpr uint32_t guard_search_radius = 0x280;
constexpr uint32_t guard_give_up_percent = 0x50;
constexpr uint32_t park_wait = 0x1e;
constexpr uint32_t patrol_wait = 0x3c;
constexpr int32_t patrol_arrival = 0x10;
constexpr int32_t kamikaze_min_arrival = 0x10;
constexpr uint32_t kamikaze_wait = 0x3c;
constexpr uint32_t reclaim_step = 2;
constexpr uint32_t decloak_hold = 300;
constexpr int32_t reclaim_spray_floor = 15; // Reclaim sprays while more remains
constexpr int32_t reclaim_sprays_per_step = 2;
constexpr uint32_t stockpile_limit = 200;
constexpr uint32_t stockpile_full_wait = 300;
constexpr int32_t stockpile_step = 5;
constexpr uint32_t stockpile_short_wait = 10;
constexpr uint32_t resurrect_no_slot_wait = 300;
constexpr uint32_t mine_wait = 0x1e;
constexpr uint32_t attack_type_wait = 0x5a;
constexpr uint32_t wait_retry_base = 0x96;
constexpr uint32_t wait_retry_jitter = 0x1e;
constexpr double reserve_fraction = 0.2;
constexpr double reclaim_time_scale = 0.5;
constexpr double reclaim_time_base = 15.0;
constexpr double resurrect_time_scale = 0.3;
// Transport scripts get three TransportPickup/TransportDrop attempts; a hover
// transport unloads from 1.5 of its Z extent away.
constexpr int32_t transport_attempts = 3;
constexpr uint32_t transport_script_wait = 0x0f;
constexpr double hover_unload_reach = 1.5;
constexpr int32_t worker_ticks_per_second = 30;

// Map-cell geometry: a cell is 16 world units (1 << 20 in 16.16) and the
// feature search samples every third cell.
constexpr int32_t cell_shift = 20;
constexpr int32_t half_cell_shift = 19;
constexpr int64_t feature_sample_step = 0x300000;

/// Returns a unit's position as a ground-order point.
///
/// @param unit the unit
/// @return its signed 16.16 x, y, z
inline sim::ground_orders::Point position_of(const oa::Unit& unit) {
    return {unit.position.x, unit.position.y, unit.position.z};
}

/// Subtracts one signed 16.16 value from another, wrapping at 32 bits.
///
/// @param a value subtracted from
/// @param b value subtracted
/// @return the low 32 bits of the difference
inline int32_t sub_fixed(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) - std::bit_cast<uint32_t>(b));
}

/// Returns the high 32 bits of a squared 16.16 delta: squared world units.
///
/// @param delta signed 16.16 delta
/// @return bits 32..63 of delta * delta, read as signed
inline int32_t squared_high(int32_t delta) {
    return static_cast<int32_t>((static_cast<int64_t>(delta) * delta) >> 32);
}

/// Returns the heading from `to` back toward `from`.
///
/// @param from Signed 16.16 point the heading points at.
/// @param to Signed 16.16 point the heading starts from.
/// @return Heading in 65536ths of a turn.
inline uint16_t
bearing_between(const sim::ground_orders::Point& from, const sim::ground_orders::Point& to) {
    return base::game_math::direction(sub_fixed(from[0], to[0]), sub_fixed(from[2], to[2]));
}

// A plot's feature words (MapPlot.feature) below this carry a FeatureDef index.
constexpr uint16_t first_reserved_feature = sim::spatial_state::first_reserved_feature;
constexpr uint16_t feature_continuation = sim::spatial_state::feature_continuation;

// The feature covering a map point: its origin cell and footprint.
struct FeatureSite {
    uint16_t index{};
    int16_t cell_x{}, cell_z{};
    int16_t footprint_x{}, footprint_z{};
    const oa::FeatureDef* def{};
};

} // namespace ground

class TickHost::GroundMissions {
    TickHost& host;
    sim::unit_spawn::Slot& s;
    sim::simulation_state::Order& order;
    Match::RuntimeOrder& record;
    uint32_t events;

    /// Returns the match the unit plays in.
    ///
    /// @return the match
    Match& match() { return host.match; }

    /// Returns the match's canonical world.
    ///
    /// @return the world
    oa::World& world() { return host.match.state(); }

    /// Returns a unit's type record.
    ///
    /// @param unit the unit
    /// @return its type; reserved type 0, which is noted, for a unit without one
    const oa::UnitDef& def_of(const oa::Unit& unit) { return match_unit_def(host.match, unit); }

    /// Returns the type record of the unit running the order.
    ///
    /// @return its type
    const oa::UnitDef& def() { return def_of(s.record); }

    /// Returns the current game tick.
    ///
    /// @return Game.tick
    uint32_t tick() { return world().game.tick; }

    /// Draws from the match's shared random stream.
    ///
    /// @param limit exclusive upper limit
    /// @return a value below limit, or 0 for a limit below 2
    uint32_t random(uint32_t limit) { return host.match.random_bounded(limit); }

    /// Plays a speech category for the unit.
    ///
    /// @param category Speech category (see the ground::speech_* values).
    void speak(uint32_t category) { host.play_sound(*s.unit, category); }

    /// Plays a speech category for the unit, captioned with the order's text.
    ///
    /// @param category Speech category (see the ground::speech_* values).
    /// @param caption The order's caption in place of the category's own.
    void speak(uint32_t category, const char* caption) {
        host.play_sound(*s.unit, category, caption);
    }

    /// Makes the order wait for the timer event.
    ///
    /// @param ticks ticks from now until the order wakes
    void wait_ticks(uint32_t ticks) {
        order.wait_events |= ground::timer_event;
        order.wake_tick = tick() + ticks;
    }

    /// Plays the order's acknowledgement once, clearing its announce bit.
    ///
    /// @param caption The order's caption, or null for none.
    void announce(const char* caption = nullptr) {
        if (record.extra.command_flags & ground::order_announce) {
            record.extra.command_flags &= static_cast<uint8_t>(~ground::order_announce);
            speak(ground::speech_order, caption);
        }
    }

    /// Tells whether one player counts another as an ally: the match's alliance
    /// row when it has one, else the player record's.
    ///
    /// @param owner player index whose row is read
    /// @param other player index looked up in it
    /// @return true for an ally
    bool allied(uint8_t owner, uint8_t other) {
        if (owner < host.match.player_alliances_.size() && host.match.player_alliances_[owner])
            return other < 10 && (*host.match.player_alliances_[owner])[other] != 0;
        const auto* player = oa::world_player(&world(), owner);
        return player && other < sizeof(player->alliance) && player->alliance[other] != 0;
    }

    /// Returns the order's target unit: the construction family's copy when
    /// it holds one, else the attack family's.
    ///
    /// @return The target, or null for none.
    sim::simulation_state::Unit* target() {
        return record.construction.target ? record.construction.target : record.attack.target;
    }

    /// Replaces this order's target, keeping the observer chain in step.
    ///
    /// @param unit new target, or null for none
    void set_target(sim::simulation_state::Unit* unit) { host.retarget(record, unit); }

    /// Replaces an order's target, keeping the observer chain in step.
    ///
    /// @param[in,out] entry order whose target changes
    /// @param unit new target, or null for none
    void retarget(Match::RuntimeOrder& entry, sim::simulation_state::Unit* unit) {
        host.retarget(entry, unit);
    }

    /// Returns the unit in a pool slot.
    ///
    /// @param slot unit slot
    /// @return its view, or null for slot 0 or one outside the pool
    sim::simulation_state::Unit* unit_at(int32_t slot) {
        if (slot <= 0 || static_cast<size_t>(slot) >= host.match.slots_.size())
            return nullptr;
        return host.match.slots_[static_cast<size_t>(slot)].unit;
    }

    /// Replaces the order's navigator goal; a structure has no movement object, so
    /// its goals go nowhere.
    ///
    /// @param goal new goal, or null to leave the order without one
    void install_goal(std::unique_ptr<sim::ground_orders::Goal> goal) {
        auto* movement = host.match.ground_runtime(s.unit_index);
        if (!movement)
            return;
        auto& g = *movement;
        g.project_slot();
        auto flags = host.weapon_flags(s);
        if (record.extra.goal) {
            sim::ground_orders::install_goal(host.view(s, g, flags), nullptr, tick(), host);
            record.extra.goal.reset();
        }
        if (goal) {
            order.raised_events &= ~sim::ground_orders::goal_event_mask;
            sim::ground_orders::install_goal(host.view(s, g, flags), goal.get(), tick(), host);
            record.extra.goal = std::move(goal);
        }
        host.write_flags(s, flags);
    }

    /// Returns the match-wide rules the match plays by.
    ///
    /// @return the rules; 3.1c's without a profile
    const data::match_rules::MatchRules& rules() const { return host.match.rules(); }

    /// Occupies every weapon slot for a busy state (the slots fire only at
    /// what an order gives them), or frees them all (they pick their own
    /// targets) when orders.weapons-free-while-busy lists the state.
    ///
    /// @param state the busy state the order enters
    void occupy_weapons_while_busy(data::match_rules::OrdersWeaponsFreeWhileBusyStates state) {
        AttackAdapter weapons(host, s, record);
        if (rules().orders.weapons_free_while_busy.states.contains(state))
            weapons.reset_weapons();
        else
            weapons.release_weapon_targets(3);
    }

    /// Returns what the unit looks for on a builder patrol (ground::patrol_choice).
    ///
    /// @return the choice for the unit's standing move order
    uint8_t patrol_choice() const { return ground::patrol_choice(rules(), s.record); }

    /// Returns where the unit stands when it guards (ground::guard_choice).
    ///
    /// @return the choice for the unit's standing move order
    uint8_t guard_choice() const { return ground::guard_choice(rules(), s.record); }

    /// Tells whether the unit's type can fly.
    ///
    /// @return true for OA_UNIT_DEF_FLAG_CAN_FLY
    bool can_fly() { return (def().flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0; }

    /// Tells whether the unit has a movement object (Unit.movement; bmcode 1 types
    /// only).
    ///
    /// @return true when it has one
    bool has_movement_object() const { return host.match.ground_runtime(s.unit_index) != nullptr; }

    /// Replaces the order's goal with arriving within `tolerance` world units
    /// of `point`; an aircraft or a unit without a movement object only
    /// drops its goal.
    ///
    /// @param point Signed 16.16 goal point.
    /// @param tolerance Arrival radius in world units.
    void circle_goal(const sim::ground_orders::Point& point, int32_t tolerance) {
        auto* g = host.match.ground_runtime(s.unit_index);
        if (can_fly() || !g) {
            install_goal(nullptr);
            return;
        }
        g->project_slot();
        install_goal(
            std::make_unique<sim::ground_orders::Goal>(
                sim::ground_orders::make_goal(order, g->geometry, point, tolerance)
            )
        );
    }

    /// Replaces the order's goal with standing on the border of a site; an
    /// aircraft or a unit without a movement object only drops its goal.
    ///
    /// @param site Top-left cell of the site (x, z).
    /// @param size Site size in cells (x, z).
    void outline_goal(std::array<int16_t, 2> site, std::array<int16_t, 2> size) {
        auto* g = host.match.ground_runtime(s.unit_index);
        if (can_fly() || !g) {
            install_goal(nullptr);
            return;
        }
        g->project_slot();
        install_goal(
            std::make_unique<sim::ground_orders::Goal>(
                sim::ground_orders::make_outline_goal(order, g->geometry, site, size)
            )
        );
    }

    /// Advances once the builder stands in its build stance.
    ///
    /// @param extra_events Order events to wait for besides a script value
    ///     change; they replace the order's wait events.
    /// @return 1 (next phase) in the build stance, else 2 (keep waiting).
    uint32_t wait_for_build_stance(uint32_t extra_events) {
        if (s.record.build_flags & ground::build_stance_flag)
            return ground::next_phase;
        order.wait_events = extra_events | ground::script_value_event;
        return ground::keep_waiting;
    }

    /// Starts StartBuilding with the heading from the unit to a point, relative to
    /// the unit's own heading.
    ///
    /// @param point signed 16.16 point built at
    void start_building_toward(const sim::ground_orders::Point& point) {
        const auto heading = static_cast<int16_t>(
            ground::bearing_between(ground::position_of(s.record), point) -
            static_cast<uint16_t>(s.record.heading)
        );
        ConstructionAdapter(host, s, record).start_building(heading);
    }

    /// Advances once the unit's script has cleared BUSY.
    ///
    /// @param extra_events Order events to wait for besides a script value
    ///     change; they replace the order's wait events.
    /// @return 1 (next phase) when not busy, else 2 (keep waiting).
    uint32_t wait_until_not_busy(uint32_t extra_events) {
        if (!(s.record.build_flags & ground::busy_flag))
            return ground::next_phase;
        order.wait_events = extra_events | ground::script_value_event;
        return ground::keep_waiting;
    }

    /// Tells whether the unit's type can carry units.
    ///
    /// @return true for OA_UNIT_DEF_ABILITY_CAN_LOAD
    bool can_load() { return (def().abilities & OA_UNIT_DEF_ABILITY_CAN_LOAD) != 0; }

    /// Starts a transport script with four locals, the first passed as its
    /// argument, stepping it at once; nothing happens without a script.
    ///
    /// @param function engine function whose exact COB name is started
    /// @param locals values of the four locals
    void start_transport_script(ScriptFunction function, const std::array<int32_t, 4>& locals) {
        auto* instance = host.match.instance(s.unit_index);
        if (instance && instance->script())
            (void)instance->script()->call_with_locals(function, locals, 1, true);
    }

    /// Returns the unit a weapon slot aims at.
    ///
    /// @param slot weapon slot 0..2
    /// @return the target, or null when the slot aims at none
    sim::simulation_state::Unit* weapon_target(uint32_t slot) {
        const auto* target = sim::weapon_execution::slot_target_unit(
            host.match.state(), s.record, static_cast<uint8_t>(slot)
        );
        return target ? unit_at(target->id) : nullptr;
    }

    // --- Orders -------------------------------------------------------------

    /// Gives an order a kind and the flags of the kind's descriptor.
    ///
    /// @param[in,out] entry the order
    /// @param kind mission kind
    void apply_descriptor(Match::RuntimeOrder& entry, uint8_t kind) {
        const auto descriptor = mission_descriptor_table.at(kind);
        entry.order.kind = kind;
        entry.order.preserve_flags = static_cast<uint8_t>(descriptor);
        entry.extra.command_flags = static_cast<uint8_t>(descriptor >> 8);
        entry.order.flags = static_cast<uint8_t>(descriptor >> 16);
    }

    /// Builds a new order record for this unit from the kind's descriptor,
    /// not yet queued.
    ///
    /// The three parameter words land in every order family's copy of them.
    ///
    /// @param kind Mission kind.
    /// @param aimed Target unit, kept only when the descriptor takes one;
    ///     null for none.
    /// @param point Signed 16.16 destination, or null for none.
    /// @param parameter_1 The order's first parameter.
    /// @param parameter_2 The order's second parameter.
    /// @param parameter_3 The order's third parameter.
    /// @return The new order, owned by the match.
    Match::RuntimeOrder& create_order(
        uint8_t kind,
        sim::simulation_state::Unit* aimed,
        const sim::ground_orders::Point* point,
        int32_t parameter_1,
        int32_t parameter_2,
        int32_t parameter_3
    ) {
        auto entry = std::make_unique<Match::RuntimeOrder>();
        auto& created = *entry;
        created.unit = s.unit;
        apply_descriptor(created, kind);
        created.order.wake_tick = 0xffffffffu;
        created.order.issue_tick = world().game.tick;
        if (point) {
            created.extra.destination = *point;
            created.attack.destination = {
                std::bit_cast<uint32_t>((*point)[0]),
                std::bit_cast<uint32_t>((*point)[1]),
                std::bit_cast<uint32_t>((*point)[2])
            };
        }
        created.extra.tolerance = parameter_1;
        created.attack.weapon_slot = parameter_1;
        created.attack.retry = parameter_2;
        created.attack.leash = parameter_3;
        created.construction.remaining = parameter_2;
        created.construction.blocked_retries = parameter_3;
        if (!aimed)
            created.extra.command_flags &= static_cast<uint8_t>(~ground::order_has_target);
        if (!point)
            created.extra.command_flags &= static_cast<uint8_t>(~ground::order_has_point);
        host.match.adopt_order(std::move(entry));
        if (aimed && (created.extra.command_flags & ground::order_has_target))
            retarget(created, aimed);
        return created;
    }

    /// Returns the head link of the queue an order belongs in: the secondary queue
    /// for an order whose flags carry order_secondary, else the primary.
    ///
    /// @param queued the order
    /// @return the unit's head link of that queue
    sim::simulation_state::Order*& queue_head(const sim::simulation_state::Order& queued) {
        return (queued.flags & ground::order_secondary) ? s.unit->secondary : s.unit->primary;
    }

    /// Links an order before another in its queue (primary, or secondary
    /// for an order whose flags carry order_secondary).
    ///
    /// @param[in,out] entry Order to link; it inherits the order_inherited
    ///     command flag of the order it precedes.
    /// @param before Order to link in front of, or null for the queue's end.
    void link_order(Match::RuntimeOrder& entry, sim::simulation_state::Order* before) {
        auto** link = &queue_head(entry.order);
        size_t steps = overlay_order_budget;
        while (*link && *link != before && steps-- > 0)
            link = &(*link)->next;
        *link = &entry.order;
        entry.order.next = before;
        if (before)
            entry.extra.command_flags |=
                host.owned(*before).extra.command_flags & ground::order_inherited;
    }

    /// Links an order at the head of its queue.
    ///
    /// @param[in,out] entry Order to link (see link_order).
    void push_order_front(Match::RuntimeOrder& entry) {
        link_order(entry, queue_head(entry.order));
    }

    /// Queues a copy of a patrol-style order at the unit's current position
    /// once per queue, so the route loops back to its start, then marks this
    /// order as cloned (order_patrol_cloned).
    void clone_patrol() {
        bool cloned = false;
        for_each_primary_uncycled(s.unit->primary, [&](sim::simulation_state::Order* queued) {
            if (host.owned(*queued).extra.command_flags & ground::order_patrol_cloned)
                cloned = true;
        });
        if (!cloned) {
            const auto here = ground::position_of(s.record);
            link_order(create_order(order.kind, nullptr, &here, 0, 0, 0), nullptr);
        }
        record.extra.command_flags |= ground::order_patrol_cloned;
    }

    /// Returns the order kind the command resolver picks for the repair
    /// command (8) against a unit.
    ///
    /// @param patient Unit to repair.
    /// @return RepairUnit for a finished unit, HelpBuild for an unfinished
    ///     one (their VTOL forms for an aircraft), or 0 when the repair is
    ///     not allowed or the patient is not live.
    uint8_t repair_command_kind(sim::simulation_state::Unit& patient) {
        const auto& unit = patient.record;
        if (!(unit.flags & OA_UNIT_FLAG_LIVE))
            return 0;
        const auto& source = def();
        const auto& patient_def = def_of(unit);
        const RepairEligibility eligibility{
            static_cast<uint8_t>(source.abilities >> 8),
            source.flags,
            source.max_water_depth,
            unit.health,
            static_cast<int32_t>(patient_def.max_damage),
            static_cast<uint8_t>(unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK),
            static_cast<int16_t>(patient_def.model_height >> 16),
            static_cast<int16_t>(unit.position.y >> 16),
            world().game.sea_level
        };
        if (!can_repair_target(eligibility))
            return 0;
        const bool finished = !(unit.build_remaining != 0.0F);
        if (finished)
            return can_fly() ? ground::vtol_repair_unit_kind : ground::repair_unit_kind;
        return can_fly() ? ground::vtol_help_build_kind : ground::help_build_kind;
    }

    /// Returns the order kind the command resolver picks for a move to a point.
    ///
    /// @return QMove for a unit without a movement object, the move kind, or 0
    uint8_t move_command_kind() {
        CommandSource source;
        source.can_move = (def().abilities & OA_UNIT_DEF_ABILITY_CAN_MOVE) != 0;
        source.object_present = has_movement_object();
        source.type_flags = def().flags;
        const auto name = resolve_combat_command(2, source, std::nullopt, world().game.sea_level);
        if (name == "QMove")
            return qmove_kind;
        return combat_order_kind(name);
    }

    /// Queues a repair of a unit ahead of the current orders; a unit that
    /// holds position or manoeuvres also queues the walk back to where it
    /// stands, and its repair is leashed (sight distance, or the manoeuvre
    /// leash).
    ///
    /// @param patient Unit to repair.
    /// @return False when no repair order applies or the unit's move stance
    ///     is none of hold, manoeuvre and roam.
    bool queue_repair(sim::simulation_state::Unit& patient) {
        const auto kind = repair_command_kind(patient);
        if (!kind)
            return false;
        const auto stance = s.record.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK;
        const bool holds = stance == 0;
        const bool manoeuvres = stance == (1u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT);
        const bool roams = stance == (2u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT);
        if (!holds && !manoeuvres && !roams)
            return false;
        int32_t leash = 0;
        if (holds || manoeuvres) {
            const auto here = ground::position_of(s.record);
            push_order_front(create_order(move_command_kind(), nullptr, &here, 0, 0, 0));
            leash =
                holds ? def().sight_distance : static_cast<uint16_t>(def().maneuver_leash_length);
        }
        auto& repair = create_order(kind, &patient, nullptr, 0, 0, leash);
        if (holds || manoeuvres) {
            repair.extra.anchor_x = static_cast<int16_t>(s.record.position.x >> 16);
            repair.extra.anchor_z = static_cast<int16_t>(s.record.position.z >> 16);
        }
        push_order_front(repair);
        return true;
    }

    /// Builds a SelfDestruct order whose countdown (the first parameter) is
    /// 1, so the unit detonates on the order's first dispatch.
    ///
    /// @return The new order, not yet queued.
    Match::RuntimeOrder& self_destruct_order() {
        return create_order(self_destruct_kind, nullptr, nullptr, 1, 0, 0);
    }

    // --- Unit searches -------------------------------------------------------

    /// Visits every unit of the spatial buckets around a point whose position
    /// lies within a radius of it.
    ///
    /// @param centre Signed 16.16 centre.
    /// @param radius Radius, signed 16.16.
    /// @param visit Called with each unit found.
    template <typename Fn>
    void
    for_each_unit_in_radius(const sim::ground_orders::Point& centre, int32_t radius, Fn&& visit) {
        using Visit = std::remove_reference_t<Fn>;
        host.match.for_each_unit_in_radius(
            centre,
            radius,
            [](void* context, sim::unit_spawn::Slot& slot) {
                (*static_cast<Visit*>(context))(*slot.unit);
            },
            &visit
        );
    }

    /// Collects damaged or unfinished allied units standing on the ground
    /// near the unit, except ones its own player is reclaiming.
    ///
    /// @param radius Search radius, signed 16.16.
    /// @return The candidates in bucket order.
    std::vector<sim::simulation_state::Unit*> repair_candidates(int32_t radius) {
        std::vector<sim::simulation_state::Unit*> found;
        const auto owner = s.record.owner_index;
        for_each_unit_in_radius(
            ground::position_of(s.record), radius, [&](sim::simulation_state::Unit& unit) {
                const auto& other = unit.record;
                if (&other == &s.record || !allied(owner, other.owner_index))
                    return;
                if ((other.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != ground::occupancy_ground)
                    return;
                const bool damaged = static_cast<uint32_t>(static_cast<int32_t>(other.health)) <
                                     def_of(other).max_damage;
                if (!damaged && !(other.build_remaining != 0.0F))
                    return;
                if (other.last_attacker_owner == owner &&
                    other.damage_kind == static_cast<uint8_t>(DeathKind::reclaim))
                    return;
                found.push_back(&unit);
            }
        );
        return found;
    }

    /// Lists the live units of the player's sightings within a radius.
    ///
    /// @param centre signed 16.16 centre
    /// @param radius radius in world units
    /// @return the units, in sightings order
    std::vector<sim::simulation_state::Unit*>
    known_units_near(const sim::ground_orders::Point& centre, int32_t radius) {
        struct CacheHost final : sim::combat_state::IntelligenceHost {
            Match& match;
            sim::combat_state::TargetUnit projected{};

            explicit CacheHost(Match& m) : match(m) {}

            bool unit_active(sim::combat_state::UnitIdentity id) override {
                if (id == 0 || id >= match.slots_.size())
                    return false;
                return unit_is_live_target(match.slots_[id].record.flags);
            }

            const sim::combat_state::TargetUnit*
            resolve(sim::combat_state::UnitIdentity id) override {
                const auto& unit = match.slots_[id].record;
                projected = {};
                projected.identity = id;
                projected.position = fixed_words(unit.position);
                projected.unit_flags = unit.flags;
                return &projected;
            }
        } cache_host(host.match);

        std::vector<sim::simulation_state::Unit*> found;
        const auto owner = s.record.owner_index;
        if (owner >= host.match.sightings_.size())
            return found;
        const std::array<uint32_t, 3> point{
            std::bit_cast<uint32_t>(centre[0]),
            std::bit_cast<uint32_t>(centre[1]),
            std::bit_cast<uint32_t>(centre[2])
        };
        const auto& sightings = host.match.sightings_[owner];
        std::vector<sim::combat_state::TargetUnit> known;
        sim::combat_state::gather_sightings(
            {sightings.seen, sightings.seen_count},
            {sightings.radar, sightings.radar_count},
            sightings.radar_fallback != 0,
            point,
            radius,
            cache_host,
            known
        );
        for (const auto& sighted : known)
            if (auto* unit = unit_at(static_cast<int32_t>(sighted.identity)))
                found.push_back(unit);
        return found;
    }

    // --- Map features --------------------------------------------------------

    /// Returns the index of a map plot.
    ///
    /// @param cell_x plot column
    /// @param cell_z plot row
    /// @return the row-major index, or nothing off the map
    std::optional<size_t> plot_index(int32_t cell_x, int32_t cell_z) {
        const auto& spatial = host.match.spatial_;
        if (cell_x < 0 || cell_z < 0 || static_cast<uint32_t>(cell_x) >= spatial.terrain_width ||
            static_cast<uint32_t>(cell_z) >= spatial.terrain_height)
            return std::nullopt;
        const auto index =
            static_cast<size_t>(cell_z) * spatial.terrain_width + static_cast<size_t>(cell_x);
        if (index >= spatial.plots.size())
            return std::nullopt;
        return index;
    }

    /// Returns the plot a footprint continuation cell leads back to.
    ///
    /// @param index plot index, row-major
    /// @return the origin plot's index; the plot itself when it is no continuation
    size_t feature_origin(size_t index) {
        const auto& plots = host.match.spatial_.plots;
        if (plots[index].feature_word != ground::feature_continuation)
            return index;
        const auto back =
            static_cast<size_t>(plots[index].feature_back_z) * host.match.spatial_.terrain_width +
            plots[index].feature_back_x;
        return back <= index ? index - back : index;
    }

    /// Returns the feature definition a plot's feature word names.
    ///
    /// @param word plot feature word
    /// @return the definition, or null for a reserved word
    const oa::FeatureDef* feature_def(uint16_t word) {
        if (word >= ground::first_reserved_feature)
            return nullptr;
        return oa::world_feature_def(&world(), oa::oa_ref_from_index(word));
    }

    /// Returns a plot's feature word, following a continuation cell to its
    /// origin.
    ///
    /// @param index Plot index (row-major).
    /// @return The FeatureDef index, or sim::spatial_state::no_feature.
    uint16_t origin_feature_word(size_t index) {
        const auto word = host.match.spatial_.plots[index].feature_word;
        if (word < ground::first_reserved_feature)
            return word;
        if (word != ground::feature_continuation)
            return sim::spatial_state::no_feature;
        return host.match.spatial_.plots[feature_origin(index)].feature_word;
    }

    /// Finds the feature under a point with its origin cell and footprint.
    ///
    /// @param point Signed 16.16 map point.
    /// @return The feature's site (its FeatureDef footprint when the table has
    ///     it), or nothing off the map or without a feature.
    std::optional<ground::FeatureSite> feature_site(const sim::ground_orders::Point& point) {
        auto cell_x = static_cast<int16_t>(point[0] >> ground::cell_shift);
        auto cell_z = static_cast<int16_t>(point[2] >> ground::cell_shift);
        auto index = plot_index(cell_x, cell_z);
        if (!index)
            return std::nullopt;
        const auto& plots = host.match.spatial_.plots;
        if (plots[*index].feature_word == ground::feature_continuation) {
            cell_z = static_cast<int16_t>(cell_z - plots[*index].feature_back_z);
            cell_x = static_cast<int16_t>(cell_x - plots[*index].feature_back_x);
            index = plot_index(cell_x, cell_z);
            if (!index)
                return std::nullopt;
        }
        const auto word = plots[*index].feature_word;
        if (word >= ground::first_reserved_feature)
            return std::nullopt;
        ground::FeatureSite site{
            word,
            cell_x,
            cell_z,
            plots[*index].feature_footprint_x,
            plots[*index].feature_footprint_z,
            feature_def(word)
        };
        if (site.def) {
            site.footprint_x = site.def->footprint_x;
            site.footprint_z = site.def->footprint_z;
        }
        return site;
    }

    /// Tells whether a feature site can be reclaimed.
    ///
    /// @param site the site
    /// @return true for a definition carrying OA_FEATURE_FLAG_RECLAIMABLE
    static bool reclaimable(const ground::FeatureSite& site) {
        return site.def && (site.def->flags & OA_FEATURE_FLAG_RECLAIMABLE);
    }

    /// Returns the point a worker sprays a feature at: the site's centre on the
    /// ground, raised by a random part of the feature's height.
    ///
    /// @param site the site
    /// @return signed 16.16 point
    sim::ground_orders::Point work_point(const ground::FeatureSite& site) {
        sim::ground_orders::Point point{
            (site.footprint_x + site.cell_x * 2) << ground::half_cell_shift,
            0,
            (site.footprint_z + site.cell_z * 2) << ground::half_cell_shift
        };
        const auto rise = random(static_cast<uint8_t>(site.def->height));
        const auto ground_height = host.match.map_height(
            std::bit_cast<uint32_t>(point[0]), std::bit_cast<uint32_t>(point[2])
        );
        point[1] = std::bit_cast<int32_t>((rise + static_cast<uint32_t>(ground_height)) << 16);
        return point;
    }

    /// Credits the unit with the feature under a point and starts its
    /// reclamate sequence.
    ///
    /// @param point Signed 16.16 map point.
    /// @return False when there is nothing to reclaim there.
    bool reclaim_feature_at(const sim::ground_orders::Point& point) {
        return host.match.reclaim_feature(s.record, {point[0], point[1], point[2]});
    }

    struct FeatureChoice {
        std::optional<sim::ground_orders::Point> site;
        float amount{};
    };

    struct ReclaimChoice {
        bool found{};
        FeatureChoice energy, metal;
    };

    /// Picks an energy and a metal feature the unit may reclaim on its own
    /// around a point, sampling every third cell of a square and keeping the
    /// richest of three random draws each.
    ///
    /// @param centre Signed 16.16 centre of the search.
    /// @param radius Side of the searched square, signed 16.16.
    /// @return The chosen sites and amounts; found is false when neither
    ///     kind has a candidate.
    ReclaimChoice choose_reclaim_features(const sim::ground_orders::Point& centre, int32_t radius) {
        struct Candidate {
            sim::ground_orders::Point site;
            float amount{};
        };

        std::vector<Candidate> metal, energy;
        const int64_t half = radius / 2;
        for (int64_t z = centre[2] - half; z <= centre[2] + half; z += ground::feature_sample_step)
            for (int64_t x = centre[0] - half; x <= centre[0] + half;
                 x += ground::feature_sample_step) {
                const auto index = plot_index(
                    static_cast<int32_t>(x) >> ground::cell_shift,
                    static_cast<int32_t>(z) >> ground::cell_shift
                );
                if (!index)
                    continue;
                const auto* feature = feature_def(origin_feature_word(*index));
                if (!feature || !(feature->flags & OA_FEATURE_FLAG_RECLAIMABLE) ||
                    !(feature->flags & OA_FEATURE_FLAG_AUTO_RECLAIMABLE))
                    continue;
                const sim::ground_orders::Point site{
                    static_cast<int32_t>(x), 0, static_cast<int32_t>(z)
                };
                if (feature->energy != 0.0F)
                    energy.push_back({site, feature->energy});
                if (feature->metal != 0.0F)
                    metal.push_back({site, feature->metal});
            }
        const auto pick = [&](const std::vector<Candidate>& candidates) {
            FeatureChoice choice;
            if (candidates.empty())
                return choice;
            float best = 0.0F;
            size_t chosen = 0;
            for (int draw = 0; draw < 3; ++draw) {
                const auto drawn = random(static_cast<uint32_t>(candidates.size()));
                if (!(best >= candidates[drawn].amount)) {
                    best = candidates[drawn].amount;
                    chosen = drawn;
                }
            }
            choice.site = candidates[chosen].site;
            choice.amount = candidates[chosen].amount;
            return choice;
        };
        ReclaimChoice choice;
        choice.energy = pick(energy);
        choice.metal = pick(metal);
        choice.found = choice.energy.site || choice.metal.site;
        return choice;
    }

    /// Gives a raised unit the orientation words of the placed-feature record
    /// its wreck's origin plot names, whether or not the plot holds a record.
    ///
    /// @param[in,out] raised The resurrected unit; its bank, heading and
    ///     pitch change.
    /// @param origin Index of the wreck's origin plot.
    void copy_wreck_orientation(oa::Unit& raised, size_t origin) {
        const auto* placed =
            sim::feature_runtime::feature_record(world(), world().plots[origin].feature_record);
        if (!placed)
            return;
        raised.bank = placed->orientation[0];
        raised.heading = static_cast<uint16_t>(placed->orientation[1]);
        raised.pitch = placed->orientation[2];
    }

    /// Requests energy and metal from the unit's economy block; the request
    /// always counts, and it is accepted only when neither gate is positive.
    ///
    /// @param energy Energy asked for this tick.
    /// @param metal Metal asked for this tick.
    /// @return True when the request was accepted.
    bool debit(float energy, float metal) {
        auto& economy = s.record.economy;
        sim::unit_health::EconomyDebit energy_block{
            economy.energy.requested, economy.energy.accepted, economy.energy.gate
        };
        sim::unit_health::EconomyDebit metal_block{
            economy.metal.requested, economy.metal.accepted, economy.metal.gate
        };
        const auto paid =
            sim::unit_health::debit_resources(energy_block, metal_block, energy, metal);
        economy.energy.requested = energy_block.requested;
        economy.energy.accepted = energy_block.accepted;
        economy.energy.gate = energy_block.gate;
        economy.metal.requested = metal_block.requested;
        economy.metal.accepted = metal_block.accepted;
        economy.metal.gate = metal_block.gate;
        return paid;
    }

    /// Redraws the order panel when it shows this unit.
    void refresh_panel() {
        if (host.match.selection_.panel_unit_id == s.record.id)
            host.match.services_.refresh_selected_unit(s);
    }

    /// Changes the order's kind to another mission, taking the new kind's
    /// descriptor but keeping bits 0x600 of the old one's packed descriptor
    /// word (kept_descriptor_bits).
    ///
    /// @param kind New mission kind.
    void morph(uint8_t kind) {
        const auto descriptor = mission_descriptor_table.at(kind);
        auto packed = static_cast<uint32_t>(order.preserve_flags) |
                      (static_cast<uint32_t>(record.extra.command_flags) << 8) |
                      (static_cast<uint32_t>(order.flags) << 16);
        packed = ((packed ^ descriptor) & ground::kept_descriptor_bits) ^ descriptor;
        order.kind = kind;
        order.preserve_flags = static_cast<uint8_t>(packed);
        record.extra.command_flags = static_cast<uint8_t>(packed >> 8);
        order.flags = static_cast<uint8_t>(packed >> 16);
    }

    // --- Order block helpers -------------------------------------------------

    /// Replaces the order's goal with a ring around a point; an aircraft or
    /// a unit without a movement object only drops its goal.
    ///
    /// @param point Signed 16.16 centre.
    /// @param outer Outer radius in world units.
    /// @param inner Inner radius in world units.
    void ring_goal(const sim::ground_orders::Point& point, int32_t outer, int32_t inner) {
        auto* g = host.match.ground_runtime(s.unit_index);
        if (can_fly() || !g) {
            install_goal(nullptr);
            return;
        }
        g->project_slot();
        install_goal(
            std::make_unique<sim::ground_orders::Goal>(
                sim::ground_orders::make_ring_goal(order, g->geometry, point, outer, inner)
            )
        );
    }

    /// Runs StopBuilding when the order started building (TickHost::stop_building).
    void stop_building() { host.stop_building(s, order); }

    /// Flags the build panel for a redraw when the viewpoint player has the
    /// unit selected.
    void refresh_selection() {
        if (s.record.owner_index == world().game.viewpoint_player &&
            (s.record.flags & OA_UNIT_FLAG_SELECTED))
            host.match.selection_.frame_flags |= OA_FRAME_FLAG_REFRESH_ORDER_PANEL;
    }

    /// Returns a worker's time per tick, as the build and repair steps pass it.
    ///
    /// @param worker the worker's type
    /// @return worker_time / 30, divided as integers
    float work_rate(const oa::UnitDef& worker) {
        return static_cast<float>(
            static_cast<int32_t>(static_cast<uint16_t>(worker.worker_time)) / 30
        );
    }

    /// Returns a footprint's reach: trunc(hypot(x, z) * scale).
    ///
    /// @param x footprint width in cells
    /// @param z footprint depth in cells
    /// @param scale world units per cell, signed
    /// @return the reach in world units, its low 32 bits
    static int32_t footprint_reach(int16_t x, int16_t z, double scale) {
        return truncate_low32(base::game_math::planar_length(x, z) * scale);
    }

    /// Returns the world units between this unit and a point, less both footprint
    /// reaches, as the build, capture and repair orders compare it with
    /// build_distance.
    ///
    /// @param at signed 16.16 point
    /// @param footprint_x width of the footprint at the point, in cells
    /// @param footprint_z depth of the footprint at the point, in cells
    /// @return the gap in world units
    int32_t
    build_gap(const sim::ground_orders::Point& at, int16_t footprint_x, int16_t footprint_z) {
        const auto centre = base::game_math::distance(
            ground::sub_fixed(s.record.position.x, at[0]),
            ground::sub_fixed(s.record.position.z, at[2])
        );
        const auto whole = static_cast<int16_t>(centre >> 16);
        return whole - footprint_reach(s.record.footprint_x, s.record.footprint_z, 8.0) +
               footprint_reach(footprint_x, footprint_z, -8.0);
    }

    /// Tells whether a unit lies within this unit's build distance.
    ///
    /// @param target the unit
    /// @return true when the build gap is at most UnitDef.build_distance
    bool within_build_distance(const oa::Unit& target) {
        return build_gap(ground::position_of(target), target.footprint_x, target.footprint_z) <=
               static_cast<int32_t>(static_cast<uint16_t>(def().build_distance));
    }

    /// Tells whether this unit may reclaim a unit: it can reclaim, and the
    /// other is not airborne and cannot capture.
    ///
    /// @param other Unit to reclaim.
    /// @return True when allowed.
    bool can_reclaim(const oa::Unit& other) {
        return (def().abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) &&
               (other.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != ground::occupancy_air &&
               !(def_of(other).abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE);
    }

    /// Returns the damage one reclaim bite deals, from worker time,
    /// veterancy and the target's health and metal cost (at least 10).
    ///
    /// @param other Unit being reclaimed.
    /// @param ticks Bite duration in ticks.
    /// @return Damage in health points, at least 1.
    int32_t reclaim_bite(const oa::Unit& other, uint32_t ticks) {
        constexpr float minimum_metal = 10.0F;
        constexpr float metal_scale = 300.0F;
        const auto& other_def = def_of(other);
        const auto metal =
            other_def.build_cost_metal > minimum_metal ? other_def.build_cost_metal : minimum_metal;
        const auto work = static_cast<uint32_t>(
            (static_cast<int32_t>(s.record.veteran_level) + 5) / 5 *
            static_cast<int32_t>(static_cast<uint16_t>(def().worker_time)) *
            static_cast<int32_t>(other_def.max_damage) * static_cast<int32_t>(ticks)
        );
        const auto bite =
            truncate_low32(static_cast<double>(work) / static_cast<double>(metal * metal_scale));
        return bite > 1 ? bite : 1;
    }

    /// Returns the order kind the command resolver picks for a patrol.
    ///
    /// @return QPatrol, the patrol or repair patrol kind, or 0 for a unit that
    ///     cannot patrol
    uint8_t patrol_command_kind() {
        const auto& d = def();
        if (!(d.abilities & OA_UNIT_DEF_ABILITY_CAN_PATROL))
            return 0;
        if (!has_movement_object())
            return qpatrol_kind;
        const bool flies = (d.flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
        if (!(d.abilities & OA_UNIT_DEF_ABILITY_CAN_REPAIR))
            return flies ? ground::vtol_patrol_kind : ground::patrol_kind;
        return flies ? ground::vtol_repair_patrol_kind : ground::repair_patrol_kind;
    }

    /// Queues a new order for this unit behind its queued orders, or at the head
    /// of its queue when the descriptor asks.
    ///
    /// Inherited orders ahead of a primary order are dropped first; never the
    /// order being dispatched.
    ///
    /// @param kind mission kind
    /// @param point signed 16.16 point, or null for none
    void queue_order(uint8_t kind, const sim::ground_orders::Point* point) {
        auto& entry = create_order(kind, nullptr, point, 0, 0, 0);
        const bool secondary = (entry.order.flags & ground::order_secondary) != 0;
        if (!secondary) {
            // Inherited orders ahead of it are dropped, never the order being dispatched.
            size_t steps = overlay_order_budget;
            while (s.unit->primary && s.unit->primary != &order && steps-- > 0 &&
                   (host.owned(*s.unit->primary).extra.command_flags & ground::order_inherited)) {
                auto* head = s.unit->primary;
                host.match.note_step(sim::simulation_state::remove_order(*s.unit, *head, host));
                if (s.unit->primary == head)
                    break;
            }
        }
        entry.order.preserve_flags |= ground::order_queued;
        if (!secondary && !(entry.order.preserve_flags & ground::order_at_head)) {
            entry.extra.command_flags |= ground::order_queue_tail;
            insert_after_queue_tail(
                *s.unit, entry.order, [&](sim::simulation_state::Order* queued) {
                    return host.match.owned_extra(queued);
                }
            );
            return;
        }
        push_order_front(entry);
    }

    // tick_missions_kickout.cpp (orders.build-site-kickout)
    /// Returns the order kind the move command (2) gives a unit.
    ///
    /// @param host the tick host
    /// @param slot the unit
    /// @return QMove for a unit without a movement object, the move kind, or 0
    static uint8_t move_command_kind_of(TickHost& host, sim::unit_spawn::Slot& slot);
    /// Tells whether a unit is on the move the kickout last sent it on: its
    /// first order is the move command's kind and its point's whole x, z and
    /// y are the ones kept for the unit. A unit on another move loses what
    /// was kept for it.
    ///
    /// @param host the tick host
    /// @param slot the unit
    /// @return true when it is on the kept move
    static bool on_kickout_move(TickHost& host, sim::unit_spawn::Slot& slot);
    /// Tells whether a unit on the footprint leaves it: one moving to a point
    /// in the footprint, an idle one, one whose order has no target or a
    /// finished one, one working on a frame that has taken 600 or more of
    /// its build energy, and one on a cheaper frame another of its owner's
    /// units works on from phase 2 on.
    ///
    /// @param host the tick host
    /// @param slot the unit
    /// @param site_x whole x of the site's centre
    /// @param site_z whole z of the site's centre
    /// @param footprint_x the built type's footprint width, cells
    /// @param footprint_z its depth, cells
    /// @return true when it leaves
    static bool kickout_moves(
        TickHost& host,
        sim::unit_spawn::Slot& slot,
        int32_t site_x,
        int32_t site_z,
        int16_t footprint_x,
        int16_t footprint_z
    );
    /// Finds where a unit leaving the footprint goes: the first free plot
    /// (no unit, no feature with height, a slope the unit stands) on rings
    /// around the site from the radius out to twice it, each walked from a
    /// start angle outward on both sides until half a turn. The start
    /// angle is random (one draw from the match's stream, made for every
    /// unit that leaves) for a unit the kickout already sent, an eighth of
    /// a turn off the course of a unit headed for a target, away from the
    /// site for any other unit.
    ///
    /// @param host the tick host
    /// @param slot the unit
    /// @param site_x whole x of the site's centre
    /// @param site_z whole z of the site's centre
    /// @param radius the first ring's radius, world units
    /// @return whole x and z of the spot, or none
    static std::optional<std::array<int32_t, 2>> kickout_spot(
        TickHost& host, sim::unit_spawn::Slot& slot, int32_t site_x, int32_t site_z, int32_t radius
    );
    /// Orders a unit to a point ahead of its orders and keeps where it sent
    /// it.
    ///
    /// @param host the tick host
    /// @param slot the unit
    /// @param move the signed 16.16 point, whole x, y and z
    static void send_off_site(
        TickHost& host, sim::unit_spawn::Slot& slot, const sim::ground_orders::Point& move
    );
    /// Returns the move point for a spot: its whole x and z, the unit's
    /// whole y, fractions zero.
    ///
    /// @param slot the unit
    /// @param spot whole x and z
    /// @return the signed 16.16 point
    static sim::ground_orders::Point
    whole_move_point(sim::unit_spawn::Slot& slot, std::array<int32_t, 2> spot);
    /// Queues a move to a point ahead of a unit's orders.
    ///
    /// @param host the tick host
    /// @param slot the unit
    /// @param head the unit's first order
    /// @param move_kind the move command's kind for the unit
    /// @param move the signed 16.16 point
    static void push_move_front(
        TickHost& host,
        sim::unit_spawn::Slot& slot,
        sim::simulation_state::Order& head,
        uint8_t move_kind,
        const sim::ground_orders::Point& move
    );

  public:

    /// Clears a blocked build site of the local player's own units
    /// (orders.build-site-kickout): each of its mobile units on the
    /// footprint that kickout_moves picks is sent to the spot kickout_spot
    /// finds. A builder whose frame is not started walks to its site again
    /// behind the move; an order without a point gives way to it; a unit
    /// the kickout already sent is sent on; any other order stops and is
    /// given again from its start behind the move, a frame the unit worked
    /// on as the repair command gives it. The unit's later orders are kept.
    ///
    /// @param host the tick host
    /// @param footprint_x the built type's footprint width, cells
    /// @param footprint_z its depth, cells
    /// @param site signed 16.16 centre of the site
    static void clear_build_site(
        TickHost& host,
        int16_t footprint_x,
        int16_t footprint_z,
        const sim::ground_orders::Point& site
    );
    /// Sends a unit to a point ahead of its orders, as the kickout sends a
    /// unit off a site (ui.build-tools' drag with the snap override key):
    /// its first order waits behind a move to the point's whole x, y and z,
    /// and its later orders are kept.
    ///
    /// @param host the tick host
    /// @param slot the unit
    /// @param point signed 16.16 point
    static void
    send_ahead(TickHost& host, sim::unit_spawn::Slot& slot, const sim::ground_orders::Point& point);

    /// Binds the handlers to one unit's order for one step.
    ///
    /// @param h the tick host
    /// @param slot the unit
    /// @param o the order
    /// @param e events that woke the order
    GroundMissions(
        TickHost& h, sim::unit_spawn::Slot& slot, sim::simulation_state::Order& o, uint32_t e
    )
        : host(h), s(slot), order(o), record(h.owned(o)), events(e) {}

    // tick_missions_ground.cpp
    /// Runs one step of Reclaim: walks to the map feature at the order point,
    /// sprays it for a time set by its metal and energy, then reclaims it.
    ///
    /// @quirk Stage 3 says "working" at every step and, once the time runs
    /// out, stage 4 takes one step more; display rules that say it once move
    /// to stage 4 with the first step instead.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 5 (done), 8 (failed: no
    ///     feature there, which is announced, one that cannot be reclaimed, or
    ///     no path) or 7 (invalid: the unit cannot move or reclaim).
    uint32_t reclaim();
    /// Runs one step of Resurrect: walks to the wreck at the order point, sprays
    /// it for a time set by the raised type's build time, raises the unit its
    /// name names at 1 health in the wreck's attitude, then queues its repair.
    ///
    /// @return 1 (next phase), 2 (keep waiting, also while no unit slot is
    ///     free), 5 (done), 8 (failed: no wreck, one that cannot be reclaimed,
    ///     no path or no type of that name) or 7 (invalid).
    uint32_t resurrect();
    /// Runs one step of RepairPatrol: patrols between points, repairing damaged
    /// allies seen on the way while energy stands above a fifth of storage, and
    /// reclaiming features while a store runs low.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 6 (rotate: arrived, or a
    ///     repair was queued), 3 (retry later: a repair could not be queued, or
    ///     a reclaim was) or 7 (invalid).
    uint32_t repair_patrol();
    /// Runs one step of Guard_NoMove: holds position and keeps weapon 0 on what
    /// it targets, or on a known enemy around the guarded point.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart) or 7 (invalid).
    uint32_t guard_no_move();
    /// Runs one step of AttackUType: queues an attack on the nearest enemy unit
    /// of the order's type (first parameter), each distance shortened by a
    /// random amount of up to half.
    ///
    /// @return 1 (next phase), 0 (restart once the attack is queued), 5 (done:
    ///     no such unit) or 7 (invalid: the unit cannot attack).
    uint32_t attack_unit_type();
    /// Runs one step of BuildWeapon: builds stockpiled rounds (nukes,
    /// anti-nukes) on the order's weapon slot (first parameter), paying for
    /// each in five-tick steps of its reload time, until the count (second
    /// parameter) is used up.
    ///
    /// @return 1 (next phase), 2 (keep waiting: stockpile full, short of
    ///     resources or mid-round), 0 (restart after each round), 5 (done) or
    ///     7 (invalid).
    uint32_t build_weapon();
    /// Runs one step of Standby_Mine: a mine waits for a ground unit to come in
    /// range and, unless it holds fire, self-destructs.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 5 (done: self-destruct
    ///     queued) or 7 (invalid: not a building).
    uint32_t standby_mine();
    /// Runs one step of Attack_Kamikaze: runs to the target (or the order
    /// point) and self-destructs on arrival.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 5 (done: target lost, or
    ///     self-destruct queued), 8 (failed: no path) or 7 (invalid: carried).
    uint32_t attack_kamikaze();
    /// Runs one step of Park: moves clear of the build area around the unit;
    /// an aircraft turns the order into VTOL_Move.
    ///
    /// @return 1 (next phase), 0 (restart), 5 (done: arrived, or another order
    ///     follows) or 7 (invalid).
    uint32_t park();
    /// Runs one step of Ground_Pickup: the transport script lifts the target
    /// aboard; between attempts the transport closes in on it, giving up after
    /// three.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart), 5 (done: aboard),
    ///     9 (retry: three attempts failed), 8 (failed: no target, target lost,
    ///     or too wide for the transport) or 7 (invalid).
    uint32_t pickup();
    /// Runs one step of Ground_Unload: the transport script sets the first
    /// carried unit down at the destination; between attempts the transport
    /// closes in on it, giving up after three.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart), 5 (done: nothing
    ///     carried, or the cargo is down), 9 (retry: three attempts failed),
    ///     8 (failed: target lost) or 7 (invalid).
    uint32_t unload();
    /// Runs one step of Wait: waits the time in the first parameter, or, with
    /// a search radius in the second, until a known unit comes near or the
    /// time runs out.
    ///
    /// @return 2 (keep waiting) or 5 (done); without a radius, wait_order's
    ///     result.
    uint32_t wait();

    // tick_missions_command.cpp
    /// Runs one step of Stop: drops every weapon target; an airborne aircraft
    /// queues a landing where it is.
    ///
    /// @return 5 (done).
    uint32_t stop();
    /// Runs one step of MakeSelectable: a campaign unit whose script has run
    /// becomes selectable.
    ///
    /// @return 5 (done).
    uint32_t make_selectable();
    /// Runs one step of WaitForAttack: holds until the target is lost or
    /// attacks.
    ///
    /// @return 1 (next phase), 5 (done: woken, or no target) or 7 (invalid).
    uint32_t wait_for_attack();
    /// Runs one step of SelfDestruct: counts down from the type's
    /// selfdestructcountdown, one spoken count a second, then blows the unit up;
    /// cancelling the order during the countdown says so. A type with no
    /// countdown blows up at once.
    ///
    /// Only counts of five and below are spoken; the steps above five stay
    /// silent.
    ///
    /// @return 1 (next phase) after each count, or 5 (done: blown up or
    ///     cancelled).
    uint32_t self_destruct();
    /// Runs one step of Attack_NoMove: a unit that cannot move fires its
    /// primary weapon at the target until it is lost or out of reach.
    ///
    /// @return 1 (next phase), 9 (retry), 5 (done: target lost or an abort
    ///     event) or 7 (invalid).
    uint32_t attack_no_move();
    /// Runs one step of SelfRepair: the unit stands on the builder in the order
    /// target (a repair pad) while the pad repairs it.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 5 (done), 8 (failed: no pad,
    ///     which is announced, an unfinished pad or an inactive unit) or
    ///     7 (invalid: the pad is not a builder).
    uint32_t self_repair();
    /// Runs one step of BuildingBuild: a factory builds the order's type on its
    /// pad, the order's count (second parameter) times. A cancel refunds the
    /// unbuilt part of the frame's metal and destroys it.
    ///
    /// @return 1 (next phase), 2 (keep waiting: pad blocked, no free slot or
    ///     building), 0 (restart after each unit or a lost frame), 5 (done:
    ///     cancelled or count used up) or 7 (invalid).
    uint32_t building_build();
    /// Runs one step of GetBuilt: an unfinished unit waits, then loses build
    /// progress while no builder is applying work. Once finished it takes on
    /// its factory's queued moves and patrols, standing orders and squad, or
    /// parks when there are none.
    ///
    /// The opening wait is 300 ticks and the next is 30. After that, 11 ticks
    /// with no construction wake take one step of build progress, and that
    /// 11-tick wait repeats. A construction wake waits 30 ticks instead and
    /// does not decay. Ordering a builder, and turning it into its build
    /// stance, do not raise the wake; applying build progress does, including
    /// a step that adds no progress. Reclaiming the frame, or repairing a
    /// finished unit, does not raise it either.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 5 (done: finished) or
    ///     7 (invalid).
    /// @quirk The 300-tick wait is only the opening one. Later progress
    ///     restarts the 30-tick wait, not the 300.
    uint32_t get_built();
    /// Runs one step of BeCarried: a transported unit keeps its weapons free and
    /// waits.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 5 (done: no longer carried) or
    ///     7 (invalid).
    uint32_t be_carried();
    /// Runs one step of Activate: an on/off type switches on.
    ///
    /// @return 5 (done).
    uint32_t activate();
    /// Runs one step of Deactivate: an on/off type switches off.
    ///
    /// @return 5 (done).
    uint32_t deactivate();
    /// Runs one step of Cloak_On: a cloakable type keeps its cloak running.
    ///
    /// @return 5 (done).
    uint32_t cloak_on();
    /// Runs one step of Cloak_Off: a cloakable type stops its cloak.
    ///
    /// @return 5 (done).
    uint32_t cloak_off();
    /// Runs one step of Standing_MoveOrder: sets the standing move order (hold,
    /// manoeuvre, roam) from the order's first parameter.
    ///
    /// @return 5 (done).
    uint32_t standing_move_order();
    /// Runs one step of Standing_FireOrder: sets the standing fire order from
    /// the order's first parameter; hold fire and return fire drop the targets
    /// of the weapons that pick their own.
    ///
    /// @return 5 (done).
    uint32_t standing_fire_order();
    /// Runs one step of QMove and QPatrol: a unit that cannot act on them yet
    /// (it has no movement object) passes them on every 60 ticks.
    ///
    /// @return 6 (rotate).
    uint32_t queued_move();

    // tick_missions_attack.cpp
    /// Runs one step of AttackSpecial: turns into the attack order the command
    /// resolver picks for the target, firing the special (third) weapon.
    ///
    /// @return 2 (keep waiting).
    uint32_t attack_special();
    /// Runs one step of Patrol: walks the route, attacking what it finds on the
    /// way.
    ///
    /// @return 1 (next phase), 6 (rotate: arrived), 3 (retry later: an attack
    ///     was queued), 4 (phase chosen: idle wait) or 7 (invalid: no movement
    ///     object).
    uint32_t patrol();
    /// Runs one step of Attack_Chase: closes on the target and fires, circling
    /// it at weapon range and nearer, then on rings around it, until the target
    /// is lost or the unit strays past its leash.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 4 (phase chosen), 5 (done:
    ///     target lost, an abort event or past the leash) or 7 (invalid: no
    ///     movement object, an aircraft or no weapons).
    uint32_t attack_chase();
    /// Runs one step of Suppress: fires at the ground point, moving about it
    /// within weapon range while the shots last.
    ///
    /// @return 1 (next phase), 4 (phase chosen), 6 (rotate), 9 (retry: cannot
    ///     move or out of reach), 5 (done: an abort event), 8 (failed: an
    ///     aircraft) or 7 (invalid).
    uint32_t suppress();
    /// Runs one step of Follow_Ground (guard): keeps near the guarded unit,
    /// strikes back at what attacks it, repairs it and helps with its
    /// construction.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 3 (retry later: an attack,
    ///     repair or assist was queued), 5 (done: the guarded unit is gone),
    ///     8 (failed: it flies) or 7 (invalid: carried).
    uint32_t follow();
    /// Runs one step of Teleport: every other unit inside the teleporter's
    /// bounds moves by the offset from the teleporter to the order point,
    /// trailing flame.
    ///
    /// A teleporter whose type has no bounds is noted and ends the order (8).
    /// Empty slots are passed
    /// over; 3.1c also moves the records of dead units.
    ///
    /// @return 5 (done), or 8 without type bounds.
    uint32_t teleport();

    // tick_missions_build.cpp
    /// Runs one step of MobileBuild: walks to the site, waits for it to clear,
    /// places the frame of the order's type and nanolathes it to completion.
    ///
    /// @return 1 (next phase), 2 (keep waiting: site blocked, no free slot or
    ///     building), 5 (done: cancelled or complete), 8 (failed: target lost,
    ///     unreachable site, or blocked after ten retries) or 7 (invalid).
    uint32_t mobile_build();
    /// Runs one step of HelpBuild: joins another builder's unfinished frame and
    /// nanolathes it.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 5 (done: cancelled or
    ///     finished), 8 (failed: no frame or no path) or 7 (invalid).
    uint32_t help_build();
    /// Runs one step of Capture: closes in on an enemy unit and sprays it until
    /// it changes sides, for a time set by its costs, health and veterancy.
    ///
    /// The time scales with (health + maximum health) / (2 * maximum health);
    /// a type with no maximum health (MaxDamage 0), for which 3.1c has no
    /// time, takes a quotient of 0, so such a unit is captured at the first
    /// capture step.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: out of reach or
    ///     the target moves), 5 (done: captured), 8 (failed: target lost, one
    ///     that can capture or is unfinished, or no path) or 7 (invalid).
    uint32_t capture();
    /// Runs one step of ReclaimUnit: closes in on a unit and sprays it, biting
    /// off health every 15 ticks, until it is gone.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: out of reach),
    ///     9 (retry: no path), 5 (done: target gone), 8 (failed: a target it may
    ///     not reclaim) or 7 (invalid).
    uint32_t reclaim_unit();
    /// Runs one step of RepairUnit: closes in on a finished ground unit and
    /// repairs it, giving up when the unit strays past its leash.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: the patient
    ///     moves), 5 (done: repaired, patient gone or airborne, or past the
    ///     leash), 8 (failed: no path) or 7 (invalid).
    uint32_t repair_unit();
    /// Runs one step of RepairUnitNoMove: repairs the target from where the unit
    /// stands.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 5 (done: repaired or no
    ///     patient), 8 (failed: unfinished patient or inactive unit) or
    ///     7 (invalid).
    uint32_t repair_unit_no_move();
};

} // namespace oa::sim::match_runtime
