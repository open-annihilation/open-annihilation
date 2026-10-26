// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {

namespace {
namespace vtol {

// Mission-table indices the aircraft movement handlers issue or compare.
constexpr uint8_t reclaim_kind = 32;
constexpr uint8_t vtol_help_build_kind = 51;
constexpr uint8_t vtol_mobile_build_kind = 54;
constexpr uint8_t vtol_reclaim_kind = 58;
constexpr uint8_t vtol_reclaim_unit_kind = 59;
constexpr uint8_t vtol_repair_unit_kind = 61;
constexpr uint8_t vtol_seek_guard_kind = 63;

constexpr uint32_t order_destroyed_event = 0x02; // dispatched when the order is torn down
constexpr uint32_t target_lost_event = 0x08;     // ? order target unlinked
constexpr uint32_t guard_alert_event = 0x10;     // the guarded unit took a hit
constexpr uint32_t arrived_event = sim::ground_orders::arrived_event;
constexpr uint32_t path_failed_event = sim::ground_orders::path_failed_event;
constexpr uint32_t goal_events = sim::ground_orders::arrived_event |
                                 sim::ground_orders::path_failed_event |
                                 sim::ground_orders::goal_replaced_event;
constexpr uint32_t weapon_wake_event = 0x10000;
constexpr uint32_t pickup_abort_events = weapon_wake_event | path_failed_event | target_lost_event;
constexpr uint32_t pickup_wait = goal_events | target_lost_event | weapon_wake_event;

constexpr uint32_t speech_order = 5;
constexpr uint32_t speech_order_done = 6;
constexpr uint32_t speech_failed = 7;
constexpr uint32_t speech_cargo_loaded = 0x0c;   // the load chatter
constexpr uint32_t speech_cargo_unloaded = 0x0d; // the unload chatter

// Command flags bits a new order sets when it carries a target or a point.
constexpr uint8_t order_has_target = 0x02;
constexpr uint8_t order_has_point = 0x04;
constexpr uint8_t order_announce = 0x20;
// Order flags bit of the descriptors a guarding builder may join.
constexpr uint8_t order_assistable = 0x10;

// Goal arrival radii (world units) and offsets (16.16).
constexpr int32_t point_arrival = 0;
constexpr int32_t patrol_arrival = 0x150;
constexpr int32_t patrol_lead = 0x1400000;
constexpr int32_t guard_arrival = 0x80;
constexpr int32_t guard_orbit = 0x1400000;
constexpr int32_t guard_weapon_margin = 0xa0;
constexpr uint32_t guard_turn = 0x4000;
constexpr uint32_t guard_turn_jitter = 0x2000;
constexpr int32_t return_arrival = 0x80;
constexpr int32_t return_step = 0x3200000;
constexpr int32_t pad_orbit_arrival = 0x80;
constexpr uint32_t pad_orbit_turn = 0x4000;
constexpr int32_t pad_approach_arrival = 0xa0;
constexpr int32_t pad_arrival = 0x30;
constexpr int16_t cargo_arrival = 0x30;
constexpr int16_t unload_arrival = 0x140;
constexpr uint32_t heading_range = 0x10000;
constexpr int32_t no_piece = -1;
// Movement layer a carry link leaves the unit in (the low two bits of
// Movement.flags).
constexpr uint8_t carried_mode = 0;
constexpr uint8_t ground_mode = 1;

constexpr uint32_t guard_wait = 0x1e;
constexpr uint32_t patrol_wait = 0x1e;
constexpr uint32_t pad_settle_wait = 0x0f;

// Landing search: the random box's side and half offset (world units) grow
// after each rejected probe until the side passes the limit.
constexpr uint32_t landing_search_span = 0x81;
constexpr uint32_t landing_search_half = 0x40;
constexpr uint32_t landing_search_span_step = 0x20;
constexpr uint32_t landing_search_half_step = 0x10;
constexpr uint32_t landing_search_span_limit = 0x200;
constexpr int32_t landing_circle_radius = 0xa00000;
constexpr uint32_t landing_circle_turn = 0x5555; // a third of a turn back per goal event
constexpr int32_t landing_circle_arrival = 0x40;

sim::ground_orders::Point position_of(const oa::Unit& unit) {
    return {unit.position.x, unit.position.y, unit.position.z};
}

// 16.16 sum without signed overflow.
int32_t add_fixed(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) + std::bit_cast<uint32_t>(b));
}

int32_t sub_fixed(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) - std::bit_cast<uint32_t>(b));
}

/// Returns the heading from `to` back toward `from`.
///
/// @param from Signed 16.16 point the heading points at.
/// @param to Signed 16.16 point the heading starts from.
/// @return Heading in 65536ths of a turn.
uint16_t
bearing_between(const sim::ground_orders::Point& from, const sim::ground_orders::Point& to) {
    return base::game_math::direction(sub_fixed(from[0], to[0]), sub_fixed(from[2], to[2]));
}

// `centre` stepped back along `heading` by `distance`; Y is `centre`'s.
sim::ground_orders::Point
behind(const sim::ground_orders::Point& centre, uint16_t heading, int32_t distance) {
    return {
        sub_fixed(centre[0], sim::unit_movement::sine_scaled(heading, distance)),
        centre[1],
        sub_fixed(centre[2], sim::unit_movement::cosine_scaled(heading, distance))
    };
}

bool below(int16_t health, uint32_t limit) {
    return static_cast<uint32_t>(static_cast<int32_t>(health)) < limit;
}

} // namespace vtol
} // namespace

class TickHost::VtolMissions {
    TickHost& host;
    sim::unit_spawn::Slot& s;
    sim::simulation_state::Order& order;
    Match::RuntimeOrder& record;
    uint32_t events;

    Match& match() { return host.match; }

    const oa::UnitDef& def_of(const oa::Unit& unit) { return match_unit_def(host.match, unit); }

    bool can_fly() { return (def_of(s.record).flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0; }

    bool ready_to_fly() { return s.unit->object_present && can_fly(); }

    /// Returns the order's target unit, or null once its unit slot is
    /// released (a zero type index).
    ///
    /// @return The target, or null for none.
    sim::simulation_state::Unit* target() {
        auto* unit = record.construction.target ? record.construction.target : record.attack.target;
        return unit && unit->record.type_index ? unit : nullptr;
    }

    void set_target(sim::simulation_state::Unit* unit) { host.retarget(record, unit); }

    sim::simulation_state::Unit* unit_at(uint32_t slot) {
        if (slot == 0 || slot >= match().slots_.size())
            return nullptr;
        auto* unit = match().slots_[slot].unit;
        return unit && unit->record.type_index ? unit : nullptr;
    }

    /// Plays a speech category for the unit.
    ///
    /// @param category Speech category (see the vtol::speech_* values).
    void speak(uint32_t category) { host.play_sound(*s.unit, category); }

    /// Plays a speech category for the unit, captioned with the order's text.
    ///
    /// @param category Speech category (see the vtol::speech_* values).
    /// @param caption The order's caption in place of the category's own.
    void speak(uint32_t category, const char* caption) {
        host.play_sound(*s.unit, category, caption);
    }

    /// Returns the speech category of a landing that finds no free pad,
    /// as the match's display rules set it (cannot comply in 3.1c).
    ///
    /// @return the speech category
    uint32_t landing_fail_voice() { return match().display_rules().landing_fail_voice; }

    /// Plays the order's acknowledgement once, clearing its announce bit.
    ///
    /// @param caption The order's caption, or null for none.
    void announce(const char* caption = nullptr) {
        if (record.extra.command_flags & vtol::order_announce) {
            record.extra.command_flags &= static_cast<uint8_t>(~vtol::order_announce);
            speak(vtol::speech_order, caption);
        }
    }

    void reset_weapons() { AttackAdapter(host, s, record).reset_weapons(); }

    void wait_ticks(uint32_t ticks) {
        order.wait_events |= 1;
        order.wake_tick = match().simulation_.tick + ticks;
    }

    void goal_at(const sim::ground_orders::Point& point, int32_t arrival) {
        host.set_aircraft_goal(s, order, &point, arrival);
    }

    void clear_goal() { host.set_aircraft_goal(s, order, nullptr, 0); }

    void run_script(ScriptFunction function, bool immediate) {
        auto* instance = match().instance(s.unit_index);
        if (instance && instance->script())
            instance->script()->call_no_arguments(function, immediate);
    }

    /// Queues a new order at the head of its queue, built as order creation and
    /// the head insert build it.
    ///
    /// @param kind Mission kind.
    /// @param order_target The new order's target unit, or null for none.
    /// @param point Signed 16.16 destination, or null for none.
    /// @return The new order.
    sim::simulation_state::Order& push_front(
        uint8_t kind,
        sim::simulation_state::Unit* order_target,
        const sim::ground_orders::Point* point
    ) {
        return match().insert_ground_order(
            s.unit_index,
            kind,
            point ? std::optional<sim::ground_orders::Point>(*point) : std::nullopt,
            0,
            order_target != nullptr ? host.slot(*order_target).unit_index : uint16_t{0}
        );
    }

    /// Queues a new order behind the last order of its queue.
    ///
    /// The tail insert takes no queue flag (command flags bit 0x40) from a
    /// successor.
    ///
    /// @param kind Mission kind.
    /// @param order_target The new order's target unit, or null for none.
    /// @param point Signed 16.16 destination.
    void append(
        uint8_t kind,
        sim::simulation_state::Unit* order_target,
        const sim::ground_orders::Point& point
    ) {
        auto& appended = push_front(kind, order_target, &point);
        // The tail insert propagates no queue flag from a successor.
        host.owned(appended).extra.command_flags &= static_cast<uint8_t>(~command_overlay);
        auto& head = (appended.flags & 4) ? s.unit->secondary : s.unit->primary;
        if (&appended != head || !appended.next)
            return;
        head = appended.next;
        auto* tail = head;
        std::size_t budget = overlay_order_budget;
        while (tail->next && budget-- > 0)
            tail = tail->next;
        tail->next = &appended;
        appended.next = nullptr;
    }

    int16_t weapon_range(uint32_t slot) {
        const auto* weapon = match().weapons_[s.unit_index].definitions.at(slot);
        return weapon ? static_cast<int16_t>(weapon->range_world_units) : int16_t{0};
    }

    bool can_repair(const sim::simulation_state::Unit* patient) {
        if (!patient)
            return false;
        const auto& source = def_of(s.record);
        const auto& target_def = def_of(patient->record);
        const RepairEligibility repair{
            static_cast<uint8_t>(source.abilities >> 8),
            source.flags,
            match().fields(s).definition->max_water_depth,
            patient->record.health,
            static_cast<int32_t>(target_def.max_damage),
            static_cast<uint8_t>(patient->record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK),
            static_cast<int16_t>(target_def.model_height >> 16),
            static_cast<int16_t>(patient->record.position.y >> 16),
            match().state().game.sea_level
        };
        return can_repair_target(repair);
    }

    // The unit a weapon slot is locked on.
    sim::simulation_state::Unit* weapon_target(uint32_t slot) {
        const auto* target = sim::weapon_execution::slot_target_unit(
            match().state(), s.record, static_cast<uint8_t>(slot)
        );
        return target ? unit_at(target->id) : nullptr;
    }

    // Whether `other`'s owner counts this unit's owner as an ally.
    bool allied_to_owner(const oa::Unit& other) {
        const auto& rows = match().player_alliances_;
        if (other.owner_index >= rows.size() || s.record.owner_index >= rows.size())
            return false;
        const auto& row = rows[other.owner_index];
        return row && (*row)[s.record.owner_index] != 0;
    }

    /// Flies back toward the map centre when the unit sits in the off-map
    /// bucket.
    ///
    /// @return True when the unit was off the map and a goal toward the
    ///     centre was installed.
    bool return_to_map() {
        const auto& projected = match().project_spatial(s);
        if (!projected.bucket_linked || projected.bucket)
            return false;
        const auto& game = match().state().game;
        const sim::ground_orders::Point centre{
            (game.map_pixel_width / 2) << 16, 0, (game.map_pixel_height / 2) << 16
        };
        const auto here = vtol::position_of(s.record);
        const auto heading = vtol::bearing_between(here, centre);
        goal_at(vtol::behind(here, heading, vtol::return_step), vtol::return_arrival);
        order.wait_events |= vtol::goal_events;
        return true;
    }

    /// Queues VTOL_Landing on a random pad of those the owner's pad list
    /// offers within 0xf00 world units (Match::repair_pads_in_reach).
    ///
    /// @return True when a pad was found and the landing queued.
    bool land_on_nearby_pad() {
        const auto pads = match().repair_pads_in_reach(s.record);
        if (pads == 0)
            return false;
        clear_goal();
        auto* pad = match().repair_pad_in_reach(s.record, host.random(pads));
        (void)push_front(sim::ground_orders::vtol_landing_kind, &host.unit_view(*pad), nullptr);
        order.wait_events = 0;
        return true;
    }

    /// Tells whether a landing point of a pad is free to land on.
    ///
    /// @param pad Air pad.
    /// @param piece COB piece of the landing point, -1 for the pad's origin.
    /// @return False when the pad is itself carried or a unit it carries
    ///     sits on the piece.
    bool pad_piece_free(const oa::Unit& pad, int32_t piece) {
        if (pad.attach_parent)
            return false;
        std::size_t budget = overlay_order_budget;
        for (auto child = link_first_child(pad); child && budget-- > 0;) {
            const auto& carried = match_unit(match(), child);
            if (static_cast<int8_t>(carried.attach_piece) == piece)
                return false;
            child = link_next(carried);
        }
        return true;
    }

    /// Picks a free landing point on a pad: the preferred one when free, else
    /// the first free piece QueryLandingPad returns.
    ///
    /// @param pad Air pad.
    /// @param preferred COB piece to try first, or -1 for none.
    /// @return The piece, or -1 when none is free.
    int32_t free_pad_piece(sim::simulation_state::Unit& pad, int32_t preferred) {
        if (preferred != vtol::no_piece && pad_piece_free(pad.record, preferred))
            return preferred;
        std::array<int32_t, 4> pieces{
            vtol::no_piece, vtol::no_piece, vtol::no_piece, vtol::no_piece
        };
        auto& pad_slot = host.slot(pad);
        if (auto* instance = match().instance(pad_slot.unit_index); instance && instance->script())
            (void)instance->script()->query(ScriptFunction::query_landing_pad, pieces);
        for (const auto piece : pieces)
            if (piece != vtol::no_piece && pad_piece_free(pad.record, piece))
                return piece;
        return vtol::no_piece;
    }

    /// Tells whether the unit may set down with its footprint centred on a
    /// point; a cell its owner has not seen always passes.
    ///
    /// @param projected The unit's spatial projection.
    /// @param world_x Signed 16.16 x of the footprint centre.
    /// @param world_z Signed 16.16 z of the footprint centre.
    /// @return True when it may land there.
    bool can_land_at(const sim::spatial_state::Unit& projected, int32_t world_x, int32_t world_z) {
        const auto* definition = match().fields(s).definition;
        const auto type_flags =
            definition ? oa::data::unit_definitions::pack_unit_flags(*definition) : 0u;
        // An aircraft that is not amphibious may not set down under the sea.
        const bool flies_not_amphibious = (type_flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0 &&
                                          (type_flags & OA_UNIT_DEF_FLAG_AMPHIBIOUS) == 0;
        const auto footprint_x = static_cast<int32_t>(projected.footprint[0]);
        const auto cell_x = static_cast<int32_t>(
            static_cast<uint32_t>(world_x + footprint_x * -0x80000 + 0x80000) >> 20
        );
        const auto cell_z = static_cast<int32_t>(
            static_cast<uint32_t>(
                world_z + static_cast<int32_t>(projected.footprint[1]) * -0x80000 + 0x80000
            ) >>
            20
        );
        const auto stride = static_cast<int32_t>(match().spatial().terrain_width) >> 1;
        const auto index =
            (cell_x >> 1) + (footprint_x >> 2) + ((cell_z >> 1) + (footprint_x >> 2)) * stride;
        bool seen = false;
        const auto& bits = match().sight().player_bits;
        if (index >= 0 && static_cast<size_t>(index) < bits.size()) {
            const auto owner = s.unit->owner ? s.unit->owner->record.index : 0;
            seen = (bits[static_cast<size_t>(index)] & (1u << (owner & 31))) != 0;
        }
        return sim::spatial_state::can_unload_at(
            projected, world_x, world_z, seen, flies_not_amphibious, match().spatial()
        );
    }

    // VTOL_LandIfCan phase 1: settle where the unit is, else on the first
    // clear probe of a widening random box, else fly the next leg of a
    // circle round the destination and search again from there.
    uint32_t seek_landing_site() {
        const auto here = vtol::position_of(s.record);
        const auto& projected = match().project_spatial(s);
        if (can_land_at(projected, here[0], here[2])) {
            run_script(ScriptFunction::end_transport, true);
            const int32_t sea = match().state().game.sea_level;
            const auto terrain = match().sample_terrain_height(
                static_cast<uint32_t>(here[0]), static_cast<uint32_t>(here[2])
            );
            // Over water the goal sits on the bed rather than the surface.
            const auto altitude = terrain > sea ? 0 : terrain - sea;
            install_with_altitude(
                sim::air::air_goal_at_point(&order.raised_events, &s.record, s.record.position),
                static_cast<int16_t>(altitude)
            );
            order.wait_events = vtol::goal_events;
            match().set_unit_state_flags(s.unit_index, 1, false);
            return 1;
        }
        for (uint32_t span = vtol::landing_search_span, half = vtol::landing_search_half;
             span <= vtol::landing_search_span_limit;
             span += vtol::landing_search_span_step, half += vtol::landing_search_half_step) {
            auto probe = here;
            probe[0] =
                vtol::add_fixed(probe[0], std::bit_cast<int32_t>((host.random(span) - half) << 16));
            probe[2] =
                vtol::add_fixed(probe[2], std::bit_cast<int32_t>((host.random(span) - half) << 16));
            sim::ground_orders::snap_to_footprint_centre(
                probe, s.record.footprint_x, s.record.footprint_z
            );
            if (can_land_at(projected, probe[0], probe[2])) {
                goal_at(probe, vtol::point_arrival);
                order.wait_events = vtol::goal_events;
                return 2;
            }
        }
        auto& heading = record.extra.tolerance;
        if (events & vtol::goal_events)
            heading = std::bit_cast<int32_t>(
                std::bit_cast<uint32_t>(heading) - vtol::landing_circle_turn
            );
        goal_at(
            vtol::behind(
                record.extra.destination,
                static_cast<uint16_t>(heading),
                vtol::landing_circle_radius
            ),
            vtol::landing_circle_arrival
        );
        order.wait_events |= vtol::goal_events;
        return 2;
    }

    // Guard reaction to whoever last hit the guarded unit.
    bool defend(sim::simulation_state::Unit& guarded) {
        auto* attacker = unit_at(guarded.record.last_attacker_id);
        if (!attacker || allied_to_owner(attacker->record) || !(events & vtol::guard_alert_event))
            return false;
        const auto* masks = match().fields(s).target_masks;
        if (masks && masks->no_chase.contains(attacker->record.type_index))
            return false;
        // air.guard-respects-hold-position: a guard holding position lets the
        // attacker be.
        if (match().rules().air.guard_respects_hold_position.enabled &&
            (s.record.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) == 0)
            return false;
        const auto attacker_slot = host.slot(*attacker).unit_index;
        if (match().issue_attack(s.unit_index, attacker_slot, true)) {
            order.wait_events = 0;
            return true;
        }
        if (!(s.record.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK))
            return false;
        AttackAdapter weapons(host, s, record);
        for (uint32_t slot = 0; slot < 3; ++slot) {
            const auto flags = s.record.weapons[slot].flags;
            if (!(flags & OA_UNIT_WEAPON_ENABLED) || !(flags & OA_UNIT_WEAPON_RETALIATE))
                continue;
            const auto* weapon = match().weapons_[s.unit_index].definitions[slot];
            if (weapon && (weapon->flags & OA_WEAPON_FLAG_COMMAND_FIRE))
                continue;
            if (auto* current = weapon_target(slot)) {
                const auto slot_byte = static_cast<uint8_t>(slot);
                const bool bad = masks && (slot == 0   ? masks->primary_bad
                                           : slot == 1 ? masks->secondary_bad
                                                       : masks->special_bad)
                                              .contains(current->record.type_index);
                if (weapons.can_reach(*current, slot_byte) && !bad)
                    continue;
            }
            weapons.assign_target(*attacker, static_cast<int32_t>(slot));
        }
        return false;
    }

    /// Returns the order kind the command resolver picks for the repair
    /// command (8) against the guarded unit.
    ///
    /// @param patient Guarded unit.
    /// @return RepairUnit for a finished unit, HelpBuild for an unfinished
    ///     one (their VTOL forms for an aircraft), or 0 when it is not live
    ///     or may not be repaired.
    uint8_t assist_kind(const sim::simulation_state::Unit& patient) {
        if (!(patient.record.flags & OA_UNIT_FLAG_LIVE) || !can_repair(&patient))
            return 0;
        if (patient.record.build_remaining == 0.0F)
            return can_fly() ? vtol::vtol_repair_unit_kind : repair_unit_kind;
        return can_fly() ? vtol::vtol_help_build_kind : help_build_kind;
    }

    // Join the build, repair or reclaim order the guarded builder is running.
    bool assist_builder(sim::simulation_state::Unit& guarded) {
        auto* primary = guarded.primary;
        if (!primary || primary->kind == 0)
            return false;
        if (!(def_of(s.record).flags & OA_UNIT_DEF_FLAG_BUILDER))
            return false;
        auto& job = host.owned(*primary);
        auto* job_target = job.construction.target ? job.construction.target : job.attack.target;
        if (!can_repair(job_target) || !(def_of(guarded.record).flags & OA_UNIT_DEF_FLAG_BUILDER) ||
            !(primary->flags & vtol::order_assistable) || job_target == s.unit)
            return false;
        const auto kind = primary->kind;
        const bool building = kind == mobile_build_kind || kind == building_build_kind ||
                              kind == vtol::vtol_mobile_build_kind;
        uint8_t joined = vtol::vtol_help_build_kind;
        if (!building) {
            const auto flags = job.extra.command_flags;
            const bool aimed =
                ((flags & vtol::order_has_target) && job_target) || (flags & vtol::order_has_point);
            if (!aimed)
                return false;
            joined = kind;
            if (joined == repair_unit_kind)
                joined = vtol::vtol_repair_unit_kind;
            if (joined == vtol::reclaim_kind)
                joined = vtol::vtol_reclaim_kind;
            if (joined == reclaim_unit_kind)
                joined = vtol::vtol_reclaim_unit_kind;
            if (joined == help_build_kind)
                joined = vtol::vtol_help_build_kind;
        } else if (!job_target)
            return false;
        clear_goal();
        const auto point = job.extra.destination;
        (void)push_front(joined, job_target, &point);
        order.wait_events = 0;
        return true;
    }

    // air.gunships-hover-to-strafe: a guarding type that hovers to attack goes
    // after an enemy its weapons can reach, as a patrolling aircraft does,
    // instead of circling past it; guarding resumes once that attack ends.
    bool engage_in_reach() {
        const auto& rule = match().rules().air.gunships_hover_to_strafe;
        if (!rule.enabled || !rule.guard_engagements ||
            !(def_of(s.record).flags & OA_UNIT_DEF_FLAG_HOVER_ATTACK))
            return false;
        auto* enemy = match().find_automatic_target(*s.unit);
        if (!enemy || !match().issue_automatic_attack(*s.unit, *enemy))
            return false;
        order.wait_events = 0;
        return true;
    }

    // Orbit the guarded unit: weapon range plus a margin, or a fixed radius.
    uint32_t orbit(const sim::simulation_state::Unit& guarded) {
        auto& angle = record.extra.tolerance;
        if (events & vtol::goal_events)
            angle = std::bit_cast<int32_t>(
                std::bit_cast<uint32_t>(angle) - vtol::guard_turn -
                host.random(vtol::guard_turn_jitter)
            );
        const auto heading = static_cast<uint16_t>(angle);
        const auto radius = (s.record.flags & OA_UNIT_FLAG_HAS_WEAPONS)
                                ? (int32_t{weapon_range(0)} + vtol::guard_weapon_margin) << 16
                                : vtol::guard_orbit;
        goal_at(
            vtol::behind(vtol::position_of(guarded.record), heading, radius), vtol::guard_arrival
        );
        wait_ticks(vtol::guard_wait);
        order.wait_events |= vtol::goal_events | vtol::target_lost_event | vtol::guard_alert_event;
        return 2;
    }

  public:

    VtolMissions(
        TickHost& h, sim::unit_spawn::Slot& slot, sim::simulation_state::Order& o, uint32_t e
    )
        : host(h), s(slot), order(o), record(h.owned(o)), events(e) {}

    /// Runs one step of VTOL_Move: takes off and flies to the destination
    /// snapped to the footprint centre, saying so on arrival when no order
    /// follows.
    ///
    /// @return 1 (next phase), 5 (done) or 7 (invalid: the unit cannot fly).
    uint32_t move() {
        switch (order.phase) {
        case 0:
            if (!ready_to_fly())
                return 7;
            host.take_off(s, order);
            return 1;
        case 1:
            announce();
            reset_weapons();
            sim::ground_orders::snap_to_footprint_centre(
                record.extra.destination, s.record.footprint_x, s.record.footprint_z
            );
            goal_at(record.extra.destination, vtol::point_arrival);
            order.wait_events = vtol::goal_events;
            return 1;
        case 2:
            if (!order.next)
                speak(vtol::speech_order_done);
            return 5;
        default:
            return 7;
        }
    }

    /// Runs one step of VTOL_Patrol: flies at a point short of the waypoint,
    /// attacking on the way, and puts down on a nearby pad below three
    /// quarters health.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: a landing was
    ///     queued), 3 (retry later: an attack was queued), 6 (rotate:
    ///     arrived) or 7 (invalid).
    uint32_t patrol() {
        switch (order.phase) {
        case 0: {
            if (!ready_to_fly())
                return 7;
            PatrolAdapter patrol(host, s, record);
            patrol.clone_patrol();
            announce("Patrolling");
            host.take_off(s, order);
            reset_weapons();
            return 1;
        }
        case 1:
            order.raised_events &= ~vtol::goal_events;
            return 1;
        case 2: {
            if (events & vtol::goal_events)
                return 6;
            const auto& waypoint = record.extra.destination;
            const auto heading = vtol::bearing_between(vtol::position_of(s.record), waypoint);
            goal_at(vtol::behind(waypoint, heading, vtol::patrol_lead), vtol::patrol_arrival);
            order.wait_events |= vtol::goal_events;
            const auto& type = def_of(s.record);
            if (vtol::below(s.record.health, (type.max_damage >> 2) * 3) &&
                !never_retreats_to_repair(match().rules(), type.abilities) && land_on_nearby_pad())
                return 0;
            if (auto* target = match().find_automatic_target(*s.unit);
                target && match().issue_automatic_attack(*s.unit, *target)) {
                order.wait_events = 0;
                return 3;
            }
            wait_ticks(vtol::patrol_wait);
            return 2;
        }
        default:
            return 7;
        }
    }

    /// Runs one step of VTOL_Follow: guards a unit, defending it, repairing
    /// it or joining its builder orders, and orbiting it otherwise; under
    /// air.gunships-hover-to-strafe a gunship attacks an enemy in reach
    /// before it orbits.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 3 (retry later: an attack,
    ///     repair or assist was queued), 5 (done: the guarded unit is gone or
    ///     lost, or no path; VTOL_SeekGuard follows when nothing else is
    ///     queued) or 7 (invalid).
    uint32_t follow() {
        auto* guarded = target();
        if (!guarded || (events & (vtol::target_lost_event | vtol::path_failed_event))) {
            if (!order.next)
                append(vtol::vtol_seek_guard_kind, guarded, record.extra.destination);
            return 5;
        }
        if (return_to_map())
            return 2;
        record.extra.destination = vtol::position_of(guarded->record);
        switch (order.phase) {
        case 0:
            if (!ready_to_fly())
                return 7;
            announce("Guarding");
            host.take_off(s, order);
            record.extra.tolerance = static_cast<int32_t>(host.random(vtol::heading_range));
            record.attack.retry = record.extra.tolerance & 1;
            return 1;
        case 1:
            reset_weapons();
            return 1;
        case 2: {
            if (defend(*guarded))
                return 3;
            if (can_repair(guarded)) {
                if (const auto kind = assist_kind(*guarded)) {
                    clear_goal();
                    (void)push_front(kind, guarded, nullptr);
                    order.wait_events = 0;
                    return 3;
                }
            }
            if (assist_builder(*guarded))
                return 3;
            if (engage_in_reach())
                return 3;
            return orbit(*guarded);
        }
        default:
            return 7;
        }
    }

    // A goal of this order whose altitude is fixed at `altitude` world units.
    void install_with_altitude(sim::air::AirGoal goal, int16_t altitude) {
        const auto air = host.air_host();
        sim::air::air_goal_set_altitude(&goal, air, altitude);
        host.install_air_goal(s, order, &goal);
    }

    int16_t cruise_altitude() { return def_of(s.record).cruise_alt; }

    /// Returns a goal over one of a pad's pieces that follows the pad and
    /// matches its heading.
    ///
    /// @param pad The pad.
    /// @param piece COB piece of the pad; -1 is the pad's own position.
    /// @return The goal, not yet installed.
    sim::air::AirGoal pad_goal(sim::simulation_state::Unit& pad, int32_t piece) {
        return sim::air::air_goal_over_unit(
            &order.raised_events, &s.record, &pad.record, static_cast<int16_t>(piece)
        );
    }

    /// Installs a goal over one of a pad's pieces that arrives within a
    /// radius.
    ///
    /// @param pad The pad.
    /// @param piece COB piece of the pad; -1 is the pad's own position.
    /// @param arrival Arrival radius in world units.
    void approach_pad(sim::simulation_state::Unit& pad, int32_t piece, int16_t arrival) {
        auto goal = pad_goal(pad, piece);
        sim::air::air_goal_set_arrival_radius(&goal, arrival);
        host.install_air_goal(s, order, &goal);
    }

    /// Returns the offset of one of this unit's pieces from its origin at the
    /// unit's attitude.
    ///
    /// @param piece COB piece index; a negative or unknown one gives zero.
    /// @return Signed 16.16 offset.
    formats::objects3d::FixedVector3 piece_offset(int32_t piece) {
        auto* instance = match().instance(s.unit_index);
        if (!instance || piece < 0 ||
            static_cast<size_t>(piece) >= instance->model().pieces().size())
            return {};
        return instance->model().attachment_position(
            static_cast<uint32_t>(piece),
            {s.record.bank, std::bit_cast<int16_t>(s.record.heading), s.record.pitch}
        );
    }

    /// Runs one step of VTOL_Pickup: flies to the cargo, hovers so the
    /// transport piece meets it and attaches it; the order ends on the next
    /// goal event while loaded.
    ///
    /// @return 1 (next phase), 5 (done), 8 (failed: no cargo, an abort
    ///     event, cargo under the sea, already loaded, cargo too wide or the
    ///     hover failed) or 7 (invalid).
    /// @quirk After attaching, the hover goal stays on the cargo: no
    ///     cruise-altitude goal is installed, as in 3.1c.
    uint32_t pickup() {
        auto* cargo = target();
        const auto sea_level = int32_t{match().state().game.sea_level} << 16;
        if (!cargo || (events & vtol::pickup_abort_events) ||
            vtol::add_fixed(def_of(cargo->record).model_height, cargo->record.position.y) <=
                sea_level) {
            speak(vtol::speech_failed, "Transport mission failed");
            return 8;
        }
        if (s.record.attach_first_child)
            return 8;
        switch (order.phase) {
        case 0: {
            if (!ready_to_fly())
                return 7;
            const auto size = static_cast<uint8_t>(def_of(s.record).transport_size);
            if (cargo->record.footprint_x > int16_t{size}) {
                speak(vtol::speech_failed, "Unit is too heavy to transport");
                return 8;
            }
            announce("Loading");
            host.take_off(s, order);
            return 1;
        }
        case 1: {
            auto goal = sim::air::air_goal_follow_unit(
                host.air_host(), &order.raised_events, &s.record, &cargo->record
            );
            sim::air::air_goal_set_arrival_radius(&goal, vtol::cargo_arrival);
            install_with_altitude(goal, cruise_altitude());
            order.wait_events = vtol::pickup_wait;
            return 1;
        }
        case 2:
            // Never heard: phase 0's acknowledgement already cleared the
            // announce bit, as in 3.1c.
            announce("Preparing for transport");
            record.extra.tolerance = vtol::no_piece;
            if (auto* instance = match().instance(s.unit_index); instance && instance->script()) {
                std::array<int32_t, 4> query{vtol::no_piece, 0, 0, 0};
                if (instance->script()->query(ScriptFunction::query_transport, query))
                    record.extra.tolerance = query[0];
            }
            order.wait_events = vtol::pickup_wait;
            return 1;
        case 3: {
            const auto height = def_of(cargo->record).model_height;
            if (auto* instance = match().instance(s.unit_index); instance && instance->script())
                instance->script()->call(
                    ScriptFunction::begin_transport, std::span(&height, 1), true
                );
            match().share_named_script_start(
                s.unit_index, "BeginTransport", 1, {static_cast<uint32_t>(height), 0, 0, 0}
            );
            const auto offset = piece_offset(record.extra.tolerance);
            install_with_altitude(
                sim::air::air_goal_over_unit(
                    &order.raised_events, &s.record, &cargo->record, vtol::no_piece
                ),
                static_cast<int16_t>(-static_cast<int16_t>(offset.y >> 16))
            );
            order.wait_events = vtol::pickup_wait | vtol::order_destroyed_event;
            return 1;
        }
        case 4:
            if (events & (vtol::order_destroyed_event | vtol::path_failed_event)) {
                run_script(ScriptFunction::end_transport, false);
                return 8;
            }
            match().set_carry_link(
                host.slot(*cargo).unit_index,
                s.unit_index,
                static_cast<int8_t>(record.extra.tolerance),
                vtol::carried_mode
            );
            speak(vtol::speech_cargo_loaded);
            // No cruise-altitude goal is installed here, as in 3.1c, so the
            // hover goal of phase 3 stays on the cargo.
            order.wait_events |= vtol::goal_events;
            return 1;
        case 5:
            return 5;
        default:
            return 7;
        }
    }

    /// Runs one step of VTOL_LandIfCan: takes off, finds clear ground near
    /// the unit, descends onto it and switches to the ground layer; the
    /// order yields to any order queued behind it.
    ///
    /// @return 1 (next phase), 2 (keep waiting: returning to the map or
    ///     still searching), 5 (done: landed, another order queued or no
    ///     path), 8 (failed: the descent did not arrive) or 7 (invalid).
    uint32_t land_if_can() {
        if (order.next || (events & vtol::path_failed_event))
            return 5;
        if (return_to_map())
            return 2;
        switch (order.phase) {
        case 0: {
            if (!ready_to_fly())
                return 7;
            auto& destination = record.extra.destination;
            if (destination[0] == 0 && destination[2] == 0 && destination[1] == 0)
                destination = vtol::position_of(s.record);
            record.extra.tolerance = static_cast<int32_t>(host.random(vtol::heading_range));
            record.attack.retry = record.extra.tolerance & 1;
            host.take_off(s, order);
            return 1;
        }
        case 1:
            return seek_landing_site();
        case 2:
            if (!(events & vtol::arrived_event))
                return 8;
            match().set_movement_layer(s.unit_index, sim::air::layer_ground);
            return 5;
        default:
            return 7;
        }
    }

    // Whether the carried unit fits on the ground at the order destination.
    bool cargo_fits_at_destination(const sim::simulation_state::Unit& cargo) {
        const auto& destination = record.extra.destination;
        const auto cell = [](int32_t position, int16_t footprint) {
            const auto shifted =
                vtol::add_fixed(vtol::sub_fixed(position, int32_t{footprint} * 0x80000), 0x80000);
            return static_cast<int32_t>(static_cast<int16_t>(shifted >> 20));
        };
        return match().site_clear_for(
            cargo.record.type_index,
            cell(destination[0], cargo.record.footprint_x),
            cell(destination[2], cargo.record.footprint_z),
            0,
            1
        );
    }

    FixedVec3 destination() {
        const auto& point = record.extra.destination;
        return {point[0], point[1], point[2]};
    }

    /// Runs one step of VTOL_Unload: flies to the destination, descends until
    /// the first carried unit stands on the ground there, releases it and
    /// climbs away.
    ///
    /// Losing the cargo target mid-order is noted and ends the order (8).
    ///
    /// @return 1 (next phase), 5 (done: nothing carried, or unloaded),
    ///     9 (retry: the cargo does not fit there, or no path),
    ///     7 (invalid) or 8 (the cargo target was lost).
    uint32_t unload() {
        const auto carried = link_first_child(s.record);
        if (!carried)
            return 5;
        switch (order.phase) {
        case 0: {
            if (!ready_to_fly())
                return 7;
            announce("Unloading");
            set_target(unit_at(carried));
            auto goal = sim::air::air_goal_at_point(&order.raised_events, &s.record, destination());
            sim::air::air_goal_set_arrival_radius(&goal, vtol::unload_arrival);
            install_with_altitude(goal, cruise_altitude());
            order.wait_events = vtol::goal_events | vtol::target_lost_event;
            return 1;
        }
        case 1: {
            auto* cargo = target();
            if (!cargo) {
                match().fault_.note("VTOL_Unload without a cargo target");
                return mission_fault_result;
            }
            if (!cargo_fits_at_destination(*cargo)) {
                speak(vtol::speech_failed, "Unable to unload unit");
                return 9;
            }
            const auto& carried_def = def_of(match_unit(match(), carried));
            install_with_altitude(
                sim::air::air_goal_at_point(&order.raised_events, &s.record, destination()),
                static_cast<int16_t>(carried_def.model_height >> 16)
            );
            order.wait_events = vtol::goal_events | vtol::target_lost_event;
            return 1;
        }
        case 2: {
            if (events & vtol::path_failed_event)
                return 9;
            auto* cargo = target();
            if (!cargo) {
                match().fault_.note("VTOL_Unload without a cargo target");
                return mission_fault_result;
            }
            if (!cargo_fits_at_destination(*cargo)) {
                speak(vtol::speech_failed, "Unable to unload unit");
                return 9;
            }
            run_script(ScriptFunction::end_transport, false);
            match().set_carry_link(carried, 0, vtol::no_piece, vtol::ground_mode);
            install_with_altitude(
                sim::air::air_goal_at_point(&order.raised_events, &s.record, s.record.position),
                cruise_altitude()
            );
            order.wait_events = vtol::goal_events;
            return 1;
        }
        case 3:
            speak(vtol::speech_cargo_unloaded);
            return 5;
        default:
            return 7;
        }
    }

    /// Runs one step of VTOL_Landing: circles the pad until a landing point
    /// is free, descends onto it and attaches; a damaged unit on a finished
    /// air base queues SelfRepair.
    ///
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: no free
    ///     landing point), 5 (done: attached), 8 (failed: no pad, or no
    ///     path) or 7 (invalid).
    uint32_t landing() {
        auto* pad = target();
        if (!pad) {
            speak(vtol::speech_failed, "Landing aborted");
            return 8;
        }
        auto& piece = record.extra.tolerance;
        switch (order.phase) {
        case 0:
            if (!ready_to_fly())
                return 7;
            announce("Landing");
            host.take_off(s, order);
            piece = static_cast<int32_t>(host.random(vtol::heading_range));
            return 1;
        case 1: {
            if (free_pad_piece(*pad, free_pad_piece(*pad, vtol::no_piece)) != vtol::no_piece) {
                order.phase = 2;
                return 2;
            }
            const auto heading = static_cast<uint16_t>(piece);
            const auto radius = int32_t{weapon_range(0)} << 16;
            const auto circle = vtol::behind(vtol::position_of(pad->record), heading, radius);
            piece = std::bit_cast<int32_t>(std::bit_cast<uint32_t>(piece) + vtol::pad_orbit_turn);
            goal_at(circle, vtol::pad_orbit_arrival);
            order.wait_events = vtol::goal_events | vtol::target_lost_event;
            order.phase = 1;
            return 2;
        }
        case 2:
            // The approach and the descent follow the pad (a carrier's deck
            // moves) and take its heading.
            approach_pad(*pad, vtol::no_piece, vtol::pad_approach_arrival);
            order.wait_events = vtol::goal_events | vtol::target_lost_event;
            return 1;
        case 3:
            piece = free_pad_piece(*pad, vtol::no_piece);
            if (piece == vtol::no_piece) {
                speak(landing_fail_voice(), "Landing failed");
                return 0;
            }
            approach_pad(*pad, piece, vtol::pad_arrival);
            order.wait_events = vtol::goal_events | vtol::target_lost_event;
            return 1;
        case 4:
            return 1;
        case 5: {
            if (events & vtol::arrived_event)
                return 1;
            piece = free_pad_piece(*pad, piece);
            if (piece == vtol::no_piece) {
                speak(landing_fail_voice(), "Landing aborted: all pads are occupied");
                return 0;
            }
            // Settle on the piece with the pad's heading, a loaded transport
            // holding its cargo's height above it.
            auto goal = pad_goal(*pad, piece);
            const auto carried = link_first_child(s.record);
            const auto altitude =
                carried
                    ? static_cast<int16_t>(def_of(match_unit(match(), carried)).model_height >> 16)
                    : int16_t{0};
            sim::air::air_goal_set_altitude(&goal, host.air_host(), altitude);
            run_script(ScriptFunction::end_transport, true);
            host.install_air_goal(s, order, &goal);
            wait_ticks(vtol::pad_settle_wait);
            order.wait_events |= vtol::goal_events | vtol::target_lost_event;
            order.phase = 5;
            return 2;
        }
        case 6: {
            if (events & vtol::path_failed_event)
                return 8;
            if (!pad_piece_free(pad->record, piece)) {
                speak(landing_fail_voice(), "Landing aborted: no pads available");
                return 0;
            }
            const auto pad_slot = host.slot(*pad).unit_index;
            const auto seat = static_cast<int8_t>(piece);
            if (const auto carried = link_first_child(s.record)) {
                run_script(ScriptFunction::end_transport, false);
                match().set_carry_link(carried, pad_slot, seat, 0);
                return 5;
            }
            match().set_carry_link(s.unit_index, pad_slot, seat, 0);
            const auto& base = def_of(pad->record);
            if (vtol::below(s.record.health, def_of(s.record).max_damage) &&
                (base.flags & OA_UNIT_DEF_FLAG_IS_AIRBASE) &&
                (base.flags & OA_UNIT_DEF_FLAG_BUILDER) && pad->record.build_remaining == 0.0F) {
                clear_goal();
                (void)push_front(self_repair_kind, pad, nullptr);
            }
            return 5;
        }
        default:
            return 7;
        }
    }
};

void TickHost::set_aircraft_goal(
    sim::unit_spawn::Slot& s,
    sim::simulation_state::Order& order,
    const sim::ground_orders::Point* point,
    int32_t arrival_radius
) {
    auto* g = match.ground_runtime(s.unit_index);
    if (!g)
        return;
    // Flying units hand a point goal to their air driver.
    if (match.air_drivers_.at(s.unit_index).unit != nullptr) {
        if (!point) {
            install_air_goal(s, order, nullptr);
            return;
        }
        auto goal = sim::air::air_goal_at_point(
            &order.raised_events, &s.record, {(*point)[0], (*point)[1], (*point)[2]}
        );
        if (arrival_radius != 0)
            sim::air::air_goal_set_arrival_radius(&goal, static_cast<int16_t>(arrival_radius));
        install_air_goal(s, order, &goal);
        return;
    }
    auto& extra = owned(order).extra;
    g->project_slot();
    auto flags = weapon_flags(s);
    if (extra.goal) {
        sim::ground_orders::install_goal(
            view(s, *g, flags), nullptr, match.simulation_.tick, *this
        );
        extra.goal.reset();
    }
    if (point) {
        auto goal = std::make_unique<sim::ground_orders::Goal>(
            sim::ground_orders::make_goal(order, g->geometry, *point, arrival_radius)
        );
        order.raised_events &= ~sim::ground_orders::goal_event_mask;
        sim::ground_orders::install_goal(
            view(s, *g, flags), goal.get(), match.simulation_.tick, *this
        );
        extra.goal = std::move(goal);
    }
    write_flags(s, flags);
}

void TickHost::install_air_goal(
    sim::unit_spawn::Slot& s, sim::simulation_state::Order& order, const sim::air::AirGoal* goal
) {
    auto& driver = match.air_drivers_.at(s.unit_index);
    auto& stored = owned(order).air_goal;
    if (stored.kind != sim::air::AirGoalKind::none) {
        sim::air::air_driver_set_goal(&driver, nullptr);
        stored = {};
    }
    if (!goal)
        return;
    order.raised_events &= ~sim::ground_orders::goal_event_mask;
    stored = *goal;
    stored.order_events = &order.raised_events;
    sim::air::air_driver_set_goal(&driver, &stored);
}

bool TickHost::dispatch_vtol_mission(
    sim::unit_spawn::Slot& s, sim::simulation_state::Order& order, uint32_t events, uint32_t& result
) {
    switch (order.kind) {
    case sim::ground_orders::vtol_move_kind:
        result = VtolMissions(*this, s, order, events).move();
        return true;
    case vtol_patrol_kind:
        result = VtolMissions(*this, s, order, events).patrol();
        return true;
    case vtol_follow_kind:
        result = VtolMissions(*this, s, order, events).follow();
        return true;
    case vtol_pickup_kind:
        result = VtolMissions(*this, s, order, events).pickup();
        return true;
    case vtol_unload_kind:
        result = VtolMissions(*this, s, order, events).unload();
        return true;
    case sim::ground_orders::vtol_landing_kind:
        result = VtolMissions(*this, s, order, events).landing();
        return true;
    case sim::ground_orders::vtol_land_if_can_kind:
        result = VtolMissions(*this, s, order, events).land_if_can();
        return true;
    default:
        return false;
    }
}

} // namespace oa::sim::match_runtime
