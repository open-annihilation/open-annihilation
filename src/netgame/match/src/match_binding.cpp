// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/match/match_binding.hpp"

#include "oa/netgame/match/route_delta.hpp"
#include "oa/netgame/match/sim_records.hpp"
#include "oa/netgame/player_slots.hpp"

#include "oa/sim/air/host.hpp"
#include "oa/sim/feature_runtime.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/sim/unit_health.hpp"

#include <algorithm>
#include <exception>
#include <iterator>

namespace oa::netgame::match {
namespace {

constexpr uint32_t economy_period_ticks = 0x1e;
// The death kind of a unit still in a slot that a 0x09 record reuses: its own
// health decides its Killed percentage and wreck, and no statistics count it.
constexpr uint8_t replaced_unit_death_kind = 0;

MatchBinding& self(void* context) {
    return *static_cast<MatchBinding*>(context);
}

bool owned_here(const World* world, const Unit& unit) {
    const auto* owner = unit.owner != 0 && unit.owner <= OA_PLAYER_COUNT
                            ? &world->game.players[unit.owner - 1]
                            : nullptr;
    return owner != nullptr && owner->in_use != 0 &&
           (owner->status == OA_PLAYER_STATUS_LOCAL || owner->status == OA_PLAYER_STATUS_COMPUTER);
}

sim::unit_spawn::Slot* slot_of(MatchBinding& b, uint16_t index) {
    auto& slots = b.match->world().slots;
    return index != 0 && index < slots.size() ? &slots[index] : nullptr;
}

/// Returns a unit's replication movement record, choosing its driver class on first use.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit Unit whose record is wanted.
/// @return The record, marked remote unless the unit's owner plays on this machine; null past the table.
MovementRecord* movement(void* context, World* world, Unit* unit) {
    auto& b = self(context);
    const auto slot = world_unit_slot(world, unit);
    if (slot >= b.movement.size())
        return nullptr;
    auto& m = b.movement[slot];
    if (m.driver == MovementClass::none && unit->type_index < world->unit_def_count)
        m.driver = movement_class_for_def(world->unit_defs[unit->type_index]);
    m.remote_driver = !owned_here(world, *unit);
    return &m;
}

/// Creates the unit a 0x09 record names in its requested slot.
///
/// A unit still in the slot dies first as the kill handler kills it (death
/// kind 0, by its own health: its Killed percentage, explosion and wreck), and
/// the slot's movement record is reset; under the recorder's rules a create
/// that repeats the unit in its slot is dropped instead. A building whose type
/// may turn (units.build-rotation) faces as the record's heading turns it.
/// Refused and failed creates are counted, those with a def index past the
/// unit table separately.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param owner_index Player slot of the sender, which owns the unit.
/// @param record The record; unit index 0 and owners without a unit range are ignored.
void create_unit(
    void* context, World* world, uint8_t owner_index, const UnitCreatedRecord& record
) {
    auto& b = self(context);
    if (record.unit_index == 0 || owner_index >= OA_PLAYER_COUNT)
        return;
    const auto& owner = world->game.players[owner_index];
    if (owner.first_unit == 0)
        return;
    auto* existing = slot_of(b, record.unit_index);
    if (existing == nullptr) {
        ++b.refused_creates;
        return;
    }
    const bool past_table = record.unit_def_index >= world->unit_def_count;
    // A recorder peer may send the records of its first game frame twice
    // in that frame: under the recorder's rules a create that repeats the
    // unit already in its slot (same owner, type and ground position) is
    // dropped, so the owner keeps its unit and no departure is announced.
    const auto& present = existing->unit->record;
    if (b.net != nullptr && b.net->rules.recorder_protocol != recorder_protocol_plain &&
        present.type_index != 0 && present.type_index == record.unit_def_index &&
        present.owner_index == owner_index && present.position.x == record.position[0] &&
        present.position.z == record.position[2])
        return;
    try {
        if (existing->unit->record.type_index != 0)
            b.match->kill_unit(record.unit_index, replaced_unit_death_kind);
        b.movement[record.unit_index] = MovementRecord{};
        sim::unit_spawn::Request request{};
        request.player = owner_index;
        request.type = record.unit_def_index;
        request.position = {
            static_cast<uint32_t>(record.position[0]),
            static_cast<uint32_t>(record.position[1]),
            static_cast<uint32_t>(record.position[2])
        };
        request.requested_slot = record.unit_index;
        // A building faces as the heading the owner sent turns it (units.build-rotation).
        request.facing = b.match->build_facing_of_heading(
            request.type, static_cast<uint16_t>(record.bank_heading >> 16)
        );
        if (b.match->create(request) != nullptr)
            ++b.created_remote;
        else
            ++(past_table ? b.creates_past_table : b.refused_creates);
    } catch (const std::exception&) {
        ++(past_table ? b.creates_past_table : b.refused_creates);
    }
}

/// Attaches a unit to a carrier, or detaches it, as a 0x0a record or a full unit record says.
///
/// The link applies without being shared again (Match::apply_carry_link).
///
/// @param context The MatchBinding.
/// @param world Match world (unused).
/// @param record The link; linked_unit_index 0 detaches.
void link_unit(void* context, World* /*world*/, const UnitLinkRecord& record) {
    auto& b = self(context);
    if (slot_of(b, record.unit_index) == nullptr)
        return;
    try {
        b.match->apply_carry_link(
            record.unit_index, record.linked_unit_index, record.attach_piece, record.occupancy
        );
    } catch (const std::exception&) {
    }
}

/// Finishes the copy a 0x12 record names, as the builder link of the machine that simulates it did.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param source The unit whose work finished it (the unit itself for a building created finished); null
///        is ignored.
/// @param subject The finished unit; null is ignored.
void link_builder(void* context, World* world, Unit* source, Unit* subject) {
    auto& b = self(context);
    if (source == nullptr || subject == nullptr)
        return;
    try {
        b.match->finish_unit(
            static_cast<uint16_t>(world_unit_slot(world, subject)),
            static_cast<uint16_t>(world_unit_slot(world, source))
        );
    } catch (const std::exception&) {
    }
}

/// Creates the unit a 0x14 record hands to a player simulated here, as its machine carried it over.
///
/// The copy takes the record's build progress, health, orientation and
/// stockpiles (Match::transfer_unit); the unit's own machine kills the old
/// unit and shares its death.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit The unit changing owner, live here.
/// @param new_owner The receiving player, local or computer.
/// @param record The transfer.
void transfer_unit(
    void* context, World* world, Unit* unit, Player* new_owner, const UnitTransferRecord& record
) {
    auto& b = self(context);
    if (unit == nullptr || new_owner == nullptr || new_owner->index >= OA_PLAYER_COUNT)
        return;
    sim::match_runtime::TransferredUnit carried{};
    carried.build_remaining = static_cast<float>(record.build_remaining);
    carried.health = static_cast<int16_t>(record.health);
    carried.bank = static_cast<int16_t>(record.bank_heading & 0xffffu);
    carried.heading = static_cast<uint16_t>(record.bank_heading >> 16);
    carried.pitch = static_cast<int16_t>(record.pitch);
    for (std::size_t slot = 0;
         slot < carried.stockpiles.size() && slot < std::size(record.weapon_stockpiles);
         ++slot)
        carried.stockpiles[slot] = record.weapon_stockpiles[slot];
    try {
        b.match->transfer_unit(
            static_cast<uint16_t>(world_unit_slot(world, unit)), new_owner->index, &carried
        );
    } catch (const std::exception&) {
    }
}

/// Applies a 0x0b damage event to its target unit.
///
/// @param context The MatchBinding.
/// @param world Match world (unused).
/// @param record Target, source, amount, kind and direction byte of the event.
void apply_damage(void* context, World* /*world*/, const UnitDamageRecord& record) {
    auto& b = self(context);
    auto* target = slot_of(b, record.target_unit_index);
    if (target == nullptr)
        return;
    auto* source = slot_of(b, record.source_unit_index);
    try {
        b.match->apply_damage_event(
            *target, source, static_cast<int16_t>(record.amount), record.kind, record.direction
        );
    } catch (const std::exception&) {
    }
}

/// Applies a 0x0c record: the unit dies here as it died on the machine that
/// simulates it, credited to the record's attacker.
///
/// The record's death kind, Killed percentage and wreck level decide its
/// Killed pieces, explosion and wreck; a unit no longer alive here is left
/// alone.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param record The kill: unit, attacker and its owner's player id, Killed percentage, kind and wreck level.
void apply_kill(void* context, World* world, const UnitKilledRecord& record) {
    auto& b = self(context);
    if (slot_of(b, record.unit_index) == nullptr)
        return;
    sim::match_runtime::KillOutcome outcome{};
    outcome.kind = static_cast<sim::match_runtime::DeathKind>(
        record.kind_and_wreck_level >> unit_killed_kind_shift
    );
    outcome.killed_percent = record.killed_percent;
    outcome.wreck_level =
        static_cast<uint8_t>(record.kind_and_wreck_level & unit_killed_wreck_level_mask);
    try {
        b.match->apply_kill(
            record.unit_index,
            outcome,
            record.attacker_unit_index,
            player_slot_of(world->game, record.attacker_owner_id)
        );
    } catch (const std::exception&) {
    }
}

uint16_t unit_slot(const World* world, const Unit* unit) {
    return static_cast<uint16_t>(world_unit_slot(world, unit));
}

/// Moves a unit to the position a detached full unit record carries and reports it to the probe.
///
/// Falls back to storing the position when the match cannot place the unit.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param[in,out] unit Unit to place.
/// @param to New position, 16.16 fixed-point x, y, z.
/// @param occupancy The record's two occupancy bits.
void place_unit(void* context, World* world, Unit* unit, const FixedVec3& to, uint8_t occupancy) {
    auto& b = self(context);
    const auto before = unit->position;
    try {
        b.match->place_unit_at(unit_slot(world, unit), to.x, to.y, to.z, occupancy);
    } catch (const std::exception&) {
        unit->position = to;
    }
    if (b.full_record_probe.placed != nullptr)
        b.full_record_probe.placed(b.full_record_probe.context, *unit, before, to);
}

/// Hands a full unit record to the recorder's view of the units (net_match_note_full_record).
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit The unit the record is for.
/// @param record The record.
void full_record_read(void* context, World* world, Unit* unit, const FullUnitRecord& record) {
    auto& b = self(context);
    if (b.net != nullptr && unit != nullptr)
        net_match_note_full_record(b.net, world_unit_slot(world, unit), record);
}

/// Tells every recorder that the local player's commander is placed (net_match_warp_done).
///
/// @param context The MatchBinding.
void commander_placed(void* context) {
    auto& b = self(context);
    if (b.net != nullptr)
        net_match_warp_done(b.net);
}

/// Steps a sender's unit once per 0x2c record received from it.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit Unit to step.
void movement_tick(void* context, World* world, Unit* unit) {
    auto& b = self(context);
    try {
        b.match->step_mirrored_movement(unit_slot(world, unit));
    } catch (const std::exception&) {
    }
}

/// Refreshes a sender unit's height and sight after its movement step.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit Unit to refresh.
void sight_update(void* context, World* world, Unit* unit) {
    auto& b = self(context);
    try {
        b.match->refresh_mirrored_height(unit_slot(world, unit));
    } catch (const std::exception&) {
    }
}

/// Sets or clears unit state flags for a full record or a 0x11 record, running the flags' scripts.
///
/// A unit owned elsewhere never resends the change. Falls back to changing
/// the bits directly when the match cannot.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param[in,out] unit Unit to change.
/// @param mask Flags to change.
/// @param on True to set, false to clear.
void toggle_state_flags(void* context, World* world, Unit* unit, uint8_t mask, bool on) {
    auto& b = self(context);
    try {
        b.match->set_unit_state_flags(unit_slot(world, unit), mask, on);
    } catch (const std::exception&) {
        unit->state_flags =
            static_cast<uint8_t>(on ? unit->state_flags | mask : unit->state_flags & ~mask);
    }
}

/// Stores a received route head and blocked bit in a unit's mirrored navigator.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit Unit the delta is for; ignored unless its ground driver is mirrored.
/// @param delta Blocked bit and up to three route points.
void apply_ground_delta(void* context, World* world, Unit* unit, const WaypointDelta& delta) {
    auto& b = self(context);
    auto* ground = b.match->ground_runtime(unit_slot(world, unit));
    if (ground == nullptr || !ground->mirrored_driver)
        return;
    RouteDelta route{};
    route.blocked = delta.flag;
    route.count = std::min<uint8_t>(delta.count, static_cast<uint8_t>(route.points.size()));
    for (uint8_t i = 0; i < route.count; ++i)
        route.points[i] = {delta.points[i][0], delta.points[i][1]};
    apply_route_delta(ground->mirrored_navigation, ground->movement, route);
}

/// Builds the goal an air delta carries, flown by the receiving aircraft with no order behind it.
///
/// A target goal's arrival radius, altitude and bearing are zero when its
/// flags leave them out. An unflagged query point, target point or seek
/// heading takes a fresh goal's default. The wire carries no stand-off, and
/// 3.1c gives a received stand-off goal no defined distance; unlike 3.1c, a
/// stand-off goal here takes the follow distance the owner would use (the
/// first weapon's range, else 100).
///
/// @param world Match world.
/// @param unit The receiving aircraft.
/// @param delta The air delta.
/// @param[out] out The goal; written only when the delta carries one.
/// @return True for a target or seek goal, false for no goal.
bool received_air_goal(World* world, Unit* unit, const AirDelta& delta, sim::air::AirGoal* out) {
    sim::air::AirGoal goal{};
    goal.unit = unit;
    if (delta.goal_tag == air_goal_tag_target) {
        const auto& wire = delta.target;
        goal.kind = sim::air::AirGoalKind::target;
        goal.flags = wire.flags;
        if ((wire.flags & air_target_has_unit) != 0) {
            goal.query_point = wire.query_point;
            goal.target = wire.target_unit != 0 ? world_unit_at(world, wire.target_unit) : nullptr;
        }
        if ((wire.flags & air_target_has_arrival_radius) != 0)
            goal.arrival_radius = wire.arrival_radius;
        if ((wire.flags & air_target_has_altitude) != 0)
            goal.altitude = wire.altitude;
        if ((wire.flags & air_target_has_bearing) != 0)
            goal.bearing = static_cast<uint16_t>(wire.bearing);
        if ((wire.flags & air_target_has_position) != 0)
            goal.point = {wire.position[0], wire.position[1], wire.position[2]};
        if ((goal.flags & (sim::air::goal_stand_off | sim::air::goal_face_target)) ==
            sim::air::goal_stand_off) {
            sim::air::AirHost host{};
            host.world = world;
            goal.stand_off =
                sim::air::air_goal_follow_unit(host, nullptr, unit, goal.target).stand_off;
        }
    } else if (delta.goal_tag == air_goal_tag_seek) {
        const auto& wire = delta.seek;
        goal.kind = sim::air::AirGoalKind::seek;
        goal.flags = wire.flag ? sim::air::seek_turn_to_heading : uint16_t{};
        goal.point = {wire.point[0], wire.point[1], wire.point[2]};
        goal.step = {wire.step[0], wire.step[1], wire.step[2]};
        goal.heading = wire.flag ? wire.heading : uint16_t{};
    } else
        return false;
    *out = goal;
    return true;
}

/// Replaces a mirrored air driver's goal with the one an air delta carries, or clears it.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit Unit the delta is for.
/// @param delta The air delta.
void set_air_goal(void* context, World* world, Unit* unit, const AirDelta& delta) {
    auto& b = self(context);
    sim::air::AirGoal goal{};
    const bool has_goal = received_air_goal(world, unit, delta, &goal);
    try {
        b.match->set_mirrored_air_goal(unit_slot(world, unit), has_goal ? &goal : nullptr);
    } catch (const std::exception&) {
    }
}

/// Applies an air delta's movement substate to a mirrored air driver.
///
/// Only a mirrored air driver reads the substate; the owner's read is empty.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit Unit the delta is for.
/// @param substate Movement substate, 0..3.
void set_movement_substate(void* context, World* world, Unit* unit, uint8_t substate) {
    auto& b = self(context);
    const auto slot = unit_slot(world, unit);
    const auto* driver = b.match->air_driver(slot);
    if (driver == nullptr || driver->local)
        return;
    try {
        b.match->set_movement_layer(slot, substate);
    } catch (const std::exception&) {
    }
}

/// Starts a feature's die (or, flagged, reclamate) sequence where its owner saw it end.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param x Plot column; plots off the map are ignored.
/// @param y Plot row.
/// @param flagged True for the reclamate sequence (action 0xff).
void queue_feature_event(void* context, World* world, uint16_t x, uint16_t y, bool flagged) {
    auto& b = self(context);
    if (x >= world->game.map_width || y >= world->game.map_height)
        return;
    sim::feature_runtime::start_feature_sequence(*world, b.match->feature_host(), x, y, flagged);
}

/// Sets the feature on a plot alight because it caught fire on its owner's machine.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param x Plot column; plots off the map are ignored.
/// @param y Plot row.
void tree_burn(void* context, World* world, uint16_t x, uint16_t y) {
    auto& b = self(context);
    if (x >= world->game.map_width || y >= world->game.map_height)
        return;
    sim::feature_runtime::ignite_feature(*world, b.match->feature_host(), x, y, true);
}

/// Damages the feature on a plot with the weapon that hit it on the sender's machine.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param weapon Game.weapon_defs index; unknown weapons are ignored.
/// @param x Plot column; plots off the map are ignored.
/// @param y Plot row.
void feature_event(void* context, World* world, uint8_t weapon, uint16_t x, uint16_t y) {
    auto& b = self(context);
    const auto* def = world_weapon_def(world, oa_ref_from_index(weapon));
    if (def == nullptr || x >= world->game.map_width || y >= world->game.map_height)
        return;
    const auto plot =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(world->game.map_width) + x;
    sim::feature_runtime::damage_feature(*world, b.match->feature_host(), plot, x, y, *def);
}

/// Starts a unit's script function by its COB index without stepping it.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit Unit whose script runs; ignored without a script.
/// @param record Function index (negative is ignored), argument count and the four locals.
void cob_start(void* context, World* world, Unit* unit, const CobStartRecord& record) {
    auto& b = self(context);
    auto* instance = b.match->instance(unit_slot(world, unit));
    auto* script = instance != nullptr ? instance->script() : nullptr;
    if (script == nullptr || record.function_index < 0)
        return;
    std::array<int32_t, 4> locals{};
    for (std::size_t i = 0; i < locals.size(); ++i)
        locals[i] = static_cast<int32_t>(record.args[i]);
    (void)script->vm().start_parameterized(
        static_cast<uint32_t>(record.function_index), locals, record.argument_count
    );
}

/// Blows up the first shot aimed at the record's point with its weapon, as an interceptor's blast did on
/// the owner's machine.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param record Target point and weapon id of the intercepted shot.
void detonate_projectile(void* context, World* world, const ProjectileInterceptedRecord& record) {
    auto& b = self(context);
    const int32_t projectile_count = world->game.projectile_count;
    const auto count = std::min<int32_t>(projectile_count, OA_PROJECTILE_CAPACITY);
    for (int32_t i = 0; i < count; ++i) {
        auto& shot = world->projectiles[i];
        const auto* weapon = world_weapon_def(world, shot.def);
        if (weapon == nullptr || weapon->weapon_id != record.weapon_id ||
            shot.target.x != record.target[0] || shot.target.y != record.target[1] ||
            shot.target.z != record.target[2])
            continue;
        try {
            b.match->detonate(shot, nullptr);
        } catch (const std::exception&) {
        }
        return;
    }
}

/// Launches a shot fired on the sender's machine here as well.
///
/// @param context The MatchBinding.
/// @param world Match world (unused).
/// @param sender The sender's player record (unused).
/// @param record The shot.
void weapon_fire(
    void* context, World* /*world*/, Player* /*sender*/, const WeaponFireRecord& record
) {
    auto& b = self(context);
    sim::match_runtime::ShotEvent shot{};
    shot.start = {record.start[0], record.start[1], record.start[2]};
    shot.target = {record.target[0], record.target[1], record.target[2]};
    shot.weapon_id = record.weapon_id;
    shot.interceptor = (record.flags & weapon_fire_interceptor) != 0;
    shot.aim_heading = record.aim_heading;
    shot.aim_pitch = record.aim_pitch;
    shot.target_unit = record.target_unit_index;
    shot.source_unit = record.source_unit_index;
    shot.slot = record.slot;
    try {
        b.match->apply_shot(shot);
    } catch (const std::exception&) {
    }
}

/// Sets the ground movement speed a full record carries.
///
/// @param context The MatchBinding.
/// @param world Match world.
/// @param unit Unit the record is for.
/// @param speed Movement speed word as sent.
void set_movement_speed(void* context, World* world, Unit* unit, uint32_t speed) {
    auto& b = self(context);
    if (auto* ground = b.match->ground_runtime(unit_slot(world, unit)))
        ground->movement.speed = static_cast<sim::unit_movement::Fixed>(speed);
}

/// Copies a local air driver, its goal and the movement layer into the replication record.
///
/// @param b Binding holding the match.
/// @param index Unit slot.
/// @param movement_flags The unit's movement flags.
/// @param[in,out] m The unit's replication record.
void capture_air(MatchBinding& b, uint16_t index, uint8_t movement_flags, MovementRecord& m) {
    const auto* driver = b.match->air_driver(index);
    if (driver == nullptr)
        return;
    m.flags = movement_flags;
    m.air = {};
    m.air.flags = driver->flags;
    const auto* goal = sim::air::air_driver_sent_goal(driver);
    if (goal == nullptr)
        return;
    if (goal->kind == sim::air::AirGoalKind::target) {
        m.air.goal_kind = air_goal_kind_target;
        auto& wire = m.air.target;
        wire.flags = static_cast<uint8_t>(goal->flags);
        wire.query_point = goal->query_point;
        wire.target_unit = goal->target != nullptr ? goal->target->id : uint16_t{};
        wire.arrival_radius = goal->arrival_radius;
        wire.altitude = goal->altitude;
        wire.bearing = static_cast<int16_t>(goal->bearing);
        wire.position[0] = goal->point.x;
        wire.position[1] = goal->point.y;
        wire.position[2] = goal->point.z;
    } else if (goal->kind == sim::air::AirGoalKind::seek) {
        m.air.goal_kind = air_goal_kind_seek;
        auto& wire = m.air.seek;
        wire.flag = (goal->flags & sim::air::seek_turn_to_heading) != 0;
        wire.point[0] = goal->point.x;
        wire.point[1] = goal->point.y;
        wire.point[2] = goal->point.z;
        wire.step[0] = goal->step.x;
        wire.step[1] = goal->step.y;
        wire.step[2] = goal->step.z;
        wire.heading = goal->heading;
    }
}

/// Copies a local driver into its replication record; the full record carries every mobile unit's speed.
///
/// @param b Binding holding the match.
/// @param index Unit slot.
/// @param[in,out] m The unit's replication record.
void capture_route(MatchBinding& b, uint16_t index, MovementRecord& m) {
    const auto* ground = b.match->ground_runtime(index);
    if (ground == nullptr)
        return;
    m.speed = static_cast<uint32_t>(ground->movement.speed);
    if (m.driver == MovementClass::air)
        capture_air(b, index, ground->movement.flags, m);
    if (m.driver != MovementClass::ground)
        return;
    const auto& nav = ground->navigation;
    m.flags = ground->movement.flags;
    m.ground.flags = nav.flags;
    const auto count = std::min<uint32_t>(nav.count, ground_driver_path_capacity);
    m.ground.path_count = static_cast<int32_t>(count);
    for (uint32_t i = 0; i < count && i < nav.points.size(); ++i) {
        m.ground.path[i][0] = nav.points[i][0];
        m.ground.path[i][1] = nav.points[i][1];
    }
}

/// Copies the sent and resend bits a delta capture updated back into the unit's local driver.
///
/// @param b Binding holding the match.
/// @param index Unit slot.
/// @param m The unit's replication record after the capture.
void release_route(MatchBinding& b, uint16_t index, const MovementRecord& m) {
    auto* ground = b.match->ground_runtime(index);
    if (ground != nullptr && m.driver == MovementClass::ground)
        ground->navigation.flags = m.ground.flags;
    if (auto* driver = b.match->air_driver(index);
        driver != nullptr && m.driver == MovementClass::air)
        driver->flags = m.air.flags;
}

/// Sends a local player's 0x2c record after its tick.
///
/// Mirrors every mobile unit's driver into its replication record, sends the
/// record, then copies the sent state back into the drivers.
///
/// @param context The MatchBinding.
/// @param player The local player that ticked.
void local_player_ticked(void* context, Player& player) {
    auto& b = self(context);
    auto* world = b.net->world;
    uint32_t count = 0;
    Unit* first = world_player_units(world, &player, &count);
    for (uint32_t i = 0; i < count; ++i) {
        auto& unit = first[i];
        if (unit.type_index == 0 || unit.movement == 0)
            continue;
        if (auto* m = movement(&b, world, &unit))
            capture_route(b, unit.id, *m);
    }
    net_match_send_player_state(b.net, &player);
    for (uint32_t i = 0; i < count; ++i) {
        auto& unit = first[i];
        if (unit.type_index != 0 && unit.movement != 0 && unit.id < b.movement.size())
            release_route(b, unit.id, b.movement[unit.id]);
    }
}

/// Sends the local player's status after it changed.
///
/// @param context The MatchBinding.
void player_status_changed(void* context) {
    net_match_send_player_status(self(context).net, false);
}

/// Resets a new unit's replication record and announces the unit with a 0x09 record.
///
/// @param context The MatchBinding.
/// @param index Unit slot.
void unit_created(void* context, uint16_t index) {
    auto& b = self(context);
    auto* world = b.net->world;
    if (index < b.movement.size())
        b.movement[index] = MovementRecord{};
    if (auto* unit = world_unit_at(world, index))
        net_match_send_unit_created(b.net, unit);
}

/// Broadcasts a unit's new state flags as a 0x11 record from the unit's owner.
///
/// @param context The MatchBinding.
/// @param index Unit slot.
/// @param flags New state flags.
void unit_flags_changed(void* context, uint16_t index, uint8_t flags) {
    auto& b = self(context);
    auto* world = b.net->world;
    const auto record = encode_unit_flags_record(index, flags);
    uint32_t route = net_match_local_route(b.net);
    if (auto* unit = world_unit_at(world, index))
        if (const auto* owner = world_player_ref(world, unit->owner))
            route = owner->player_id;
    net_match_send(b.net, route, broadcast_destination_id, record.data(), record.size());
}

/// Has every other player start the same script: a 0x10 record from the unit's owner to all players.
///
/// @param context The MatchBinding.
/// @param index Unit slot; units without an owner are ignored.
/// @param function COB script function index.
/// @param argument_count How many of the locals are arguments.
/// @param locals The four script locals.
void script_started(
    void* context,
    uint16_t index,
    int16_t function,
    uint8_t argument_count,
    const std::array<uint32_t, 4>& locals
) {
    auto& b = self(context);
    auto* world = b.net->world;
    const auto* unit = world_unit_at(world, index);
    const auto* owner = unit != nullptr ? world_player_ref(world, unit->owner) : nullptr;
    if (owner == nullptr)
        return;
    CobStartRecord record{};
    record.unit_index = index;
    record.function_index = function;
    record.argument_count = argument_count;
    for (std::size_t i = 0; i < locals.size(); ++i)
        record.args[i] = locals[i];
    uint8_t bytes[24];
    std::size_t written = 0;
    if (encode_record(record, bytes, sizeof bytes, &written) == WireError::ok)
        (void)net_match_send(b.net, owner->player_id, broadcast_destination_id, bytes, written);
}

/// Broadcasts a shot as a 0x0d record from the firing unit's owner, or a meteor's from the local route.
///
/// @param context The MatchBinding.
/// @param shot The shot fired.
void shot_fired(void* context, const sim::match_runtime::ShotEvent& shot) {
    auto& b = self(context);
    auto* world = b.net->world;
    WeaponFireRecord record{};
    record.start[0] = shot.start.x;
    record.start[1] = shot.start.y;
    record.start[2] = shot.start.z;
    record.target[0] = shot.target.x;
    record.target[1] = shot.target.y;
    record.target[2] = shot.target.z;
    // 3.1c's record holds one byte, so a weapon past 255 goes out as its low byte.
    record.weapon_id = static_cast<uint8_t>(shot.weapon_id);
    record.flags = shot.interceptor ? weapon_fire_interceptor : uint8_t{};
    record.aim_heading = shot.aim_heading;
    record.aim_pitch = shot.aim_pitch;
    record.target_unit_index = shot.target_unit;
    record.source_unit_index = shot.source_unit;
    record.slot = shot.slot;
    uint32_t route = net_match_local_route(b.net);
    if (const auto* unit = shot.source_unit != 0 ? world_unit_at(world, shot.source_unit) : nullptr)
        if (const auto* owner = world_player_ref(world, unit->owner))
            route = owner->player_id;
    uint8_t bytes[36];
    std::size_t written = 0;
    if (encode_record(record, bytes, sizeof bytes, &written) == WireError::ok)
        (void)net_match_send(b.net, route, broadcast_destination_id, bytes, written);
}

/// Broadcasts the death of a unit simulated here as a 0x0c record from the unit's owner.
///
/// @param context The MatchBinding.
/// @param index Unit slot; a unit without an owner is ignored.
/// @param outcome The death kind, Killed percentage and wreck level it dies with.
void unit_killed(void* context, uint16_t index, const sim::match_runtime::KillOutcome& outcome) {
    auto& b = self(context);
    auto* world = b.net->world;
    const auto* unit = world_unit_at(world, index);
    const auto* owner = unit != nullptr ? world_player_ref(world, unit->owner) : nullptr;
    if (owner == nullptr)
        return;
    const auto record = encode_unit_killed_record(
        *world,
        index,
        static_cast<uint8_t>(outcome.kind),
        outcome.killed_percent,
        outcome.wreck_level
    );
    (void)net_match_send(
        b.net, owner->player_id, broadcast_destination_id, record.data(), record.size()
    );
}

/// Sends a feature record: a weapon hit to the host, anything else to every player, from the player in the
/// sender slot, or from the first player on this machine.
///
/// @param context The MatchBinding.
/// @param record The 0x0f record.
/// @param to_host True for a hit handed to the host.
/// @param sender Player slot the record goes out from; no_player_slot for the first player on this machine.
void send_feature_record(
    void* context, const uint8_t record[feature_record_bytes], bool to_host, uint8_t sender
) {
    auto& b = self(context);
    const uint32_t from = sender != no_player_slot ? player_slot_id(b.net->world->game, sender)
                                                   : net_match_local_route(b.net);
    const uint32_t to = to_host ? net_match_host_id(b.net) : broadcast_destination_id;
    (void)net_match_send(b.net, from, to, record, feature_record_bytes);
}

/// Tells whether the session is live (Game.session_flags kNetFlagLive).
///
/// @param context The MatchBinding.
/// @return True in a live network game.
bool session_live(void* context) {
    return (self(context).net->world->game.session_flags & kNetFlagLive) != 0;
}

/// Hands a weapon hit on a feature to the host through the binding's FeatureRecordLink, as the match's hook.
///
/// @param context The MatchBinding.
/// @param weapon_id The hitting weapon's id.
/// @param cell_x Hit plot column.
/// @param cell_z Hit plot row.
/// @return True when the hit went to the host and is not applied here.
bool feature_hit_hook(void* context, uint16_t weapon_id, int32_t cell_x, int32_t cell_z) {
    return feature_hit_elsewhere(&self(context).features, weapon_id, cell_x, cell_z);
}

/// Shares a feature change settled here through the binding's FeatureRecordLink, as the match's hook.
///
/// @param context The MatchBinding.
/// @param change What happened to the feature.
/// @param cell_x Feature plot column.
/// @param cell_z Feature plot row.
/// @param reclaimer Unit slot of the unit that finished reclaiming or resurrecting it, 0 for a fire or a
///        destruction.
void feature_changed_hook(
    void* context,
    sim::feature_runtime::FeatureChange change,
    int32_t cell_x,
    int32_t cell_z,
    uint16_t reclaimer
) {
    auto& b = self(context);
    const auto* unit = reclaimer != 0 ? world_unit_at(b.features.world, reclaimer) : nullptr;
    feature_changed(&b.features, change, cell_x, cell_z, unit);
}

/// Encodes and sends one record to every player.
///
/// @param b Binding holding the match.
/// @param from Player id the record goes out from.
/// @param record The record.
template <class R>
void broadcast(MatchBinding& b, uint32_t from, const R& record) {
    uint8_t bytes[record_length_table[static_cast<uint8_t>(R::type)]];
    std::size_t written = 0;
    if (encode_record(record, bytes, sizeof bytes, &written) == WireError::ok)
        (void)net_match_send(b.net, from, broadcast_destination_id, bytes, written);
}

/// Shares a shot an interceptor's blast set off here: two 0x0e records, the shot's target point and weapon
/// id, then the interceptor's, both to every player from the owner of the interceptor's source unit.
///
/// @param context The MatchBinding.
/// @param shot The shot the blast set off.
/// @param interceptor The interceptor's shot.
void shot_intercepted(void* context, const Projectile& shot, const Projectile& interceptor) {
    auto& b = self(context);
    auto* world = b.net->world;
    const auto record_of = [world](const Projectile& projectile) {
        ProjectileInterceptedRecord record{};
        record.target[0] = projectile.target.x;
        record.target[1] = projectile.target.y;
        record.target[2] = projectile.target.z;
        if (const auto* weapon = world_weapon_def(world, projectile.def))
            record.weapon_id = weapon->weapon_id;
        return record;
    };
    const auto* source = world_unit(world, interceptor.source);
    const auto* owner = source != nullptr ? world_player_ref(world, source->owner) : nullptr;
    const uint32_t from = owner != nullptr ? owner->player_id : net_match_local_route(b.net);
    broadcast(b, from, record_of(shot));
    broadcast(b, from, record_of(interceptor));
}

/// Shares a carry link just before it applies here: a 0x0a record to every player from the first player on
/// this machine, whichever machine simulates the two units.
///
/// @param context The MatchBinding.
/// @param unit Carried unit slot.
/// @param carrier Carrier slot, 0 when the unit is set down.
/// @param piece COB piece of the carrier, -1 for none.
/// @param mode Movement layer the carried unit takes.
void carry_link_changed(
    void* context, uint16_t unit, uint16_t carrier, int8_t piece, uint8_t mode
) {
    auto& b = self(context);
    UnitLinkRecord record{};
    record.unit_index = unit;
    record.linked_unit_index = carrier;
    record.attach_piece = piece;
    record.occupancy = mode;
    broadcast(b, net_match_local_route(b.net), record);
}

/// Shares that a unit simulated here is finished: a 0x12 record {unit, builder} to every player from the
/// builder's owner.
///
/// @param context The MatchBinding.
/// @param unit Finished unit slot.
/// @param builder Slot of the unit whose work finished it; `unit` for a building created finished.
void unit_finished(void* context, uint16_t unit, uint16_t builder) {
    auto& b = self(context);
    auto* world = b.net->world;
    net_match_send_builder_link(b.net, world_unit_at(world, builder), world_unit_at(world, unit));
}

/// Hands a unit simulated here to a player another machine simulates: a 0x14 record to every player from
/// the unit's owner, just before the unit dies here as captured.
///
/// @param context The MatchBinding.
/// @param unit Unit slot, still live and owned by its old owner; a unit without an owner is ignored.
/// @param new_owner Player slot of the receiving player.
void unit_transferred(void* context, uint16_t unit, uint8_t new_owner) {
    auto& b = self(context);
    auto* world = b.net->world;
    const auto* given = world_unit_at(world, unit);
    const auto* owner = given != nullptr ? world_player_ref(world, given->owner) : nullptr;
    if (owner == nullptr || new_owner >= OA_PLAYER_COUNT)
        return;
    broadcast(
        b, owner->player_id, unit_transfer_record(*given, world->game.players[new_owner].player_id)
    );
}

/// Returns the route a damage event goes out on: the source unit's owner, else the local route.
///
/// @param context The MatchBinding.
/// @param source Unit slot of the damage source; 0 for none.
/// @return The owner's player id, or the local route.
sim::unit_health::RouteIdentity health_route(void* context, uint16_t source) {
    auto& b = self(context);
    auto* world = b.net->world;
    if (source != 0)
        if (auto* unit = world_unit_at(world, source))
            if (const auto* owner = world_player_ref(world, unit->owner))
                return owner->player_id;
    return net_match_local_route(b.net);
}

/// Sends a damage event to the other players as a 0x0b record.
///
/// @param context The MatchBinding.
/// @param route Player id the record goes out from.
/// @param event Target, source, amount, direction and kind of the damage.
void health_shared(
    void* context, sim::unit_health::RouteIdentity route, const sim::unit_health::HealthEvent& event
) {
    auto& b = self(context);
    UnitDamageRecord record{};
    record.target_unit_index = event.target;
    record.source_unit_index = event.source;
    record.amount = static_cast<uint16_t>(event.amount);
    record.direction = event.direction;
    record.kind = event.kind;
    net_match_send_damage(b.net, static_cast<uint32_t>(route), record);
}

/// Credits a shared resource through credit_player_resource.
///
/// @param context Unused.
/// @param[in,out] world Match world.
/// @param to Receiving player slot.
/// @param metal True for metal, false for energy.
/// @param amount Amount received.
void credit_hook(void* /*context*/, World* world, uint8_t to, bool metal, float amount) {
    credit_player_resource(world, to, metal, amount);
}

/// Debits a shared resource through debit_player_resource.
///
/// @param context Unused.
/// @param[in,out] world Match world.
/// @param from Giving player slot.
/// @param metal True for metal, false for energy.
/// @param amount Amount given.
/// @return True when the store covered the amount.
bool debit_hook(void* /*context*/, World* world, uint8_t from, bool metal, float amount) {
    return debit_player_resource(world, from, metal, amount);
}

/// Destroys a departing player's units through match_binding_destroy_player_units.
///
/// @param context The MatchBinding.
/// @param world Match world (unused).
/// @param slot Player slot of the departing player.
void destroy_player_units_hook(void* context, World* /*world*/, uint8_t slot) {
    match_binding_destroy_player_units(&self(context), slot);
}

/// Ends the local game through match_binding_end_local_game.
///
/// @param context The MatchBinding.
void end_local_game_hook(void* context) {
    match_binding_end_local_game(&self(context));
}

/// Tells whether the local player has won, through match_binding_local_player_won.
///
/// @param context The MatchBinding.
/// @return True once the local player's game is won.
bool local_player_won_hook(void* context) {
    return match_binding_local_player_won(&self(context));
}

/// Lets the match follow a player's alliance row through match_binding_follow_alliances.
///
/// @param context The MatchBinding.
/// @param slot Player slot whose row changed.
void alliance_changed_hook(void* context, World* /*world*/, uint8_t slot) {
    match_binding_follow_alliances(&self(context), slot);
}

/// Shares a player's map with another through match_binding_share_sight.
///
/// @param context The MatchBinding.
/// @param from Player slot sharing its map.
/// @param to Player slot receiving it.
void share_sight_hook(void* context, World* /*world*/, uint8_t from, uint8_t to) {
    match_binding_share_sight(&self(context), from, to);
}

} // namespace

void match_binding_init(MatchBinding* b, sim::match_runtime::Match* match, NetMatch* net) {
    b->match = match;
    b->net = net;
    b->movement.assign(match->state().unit_slot_count, MovementRecord{});
    b->created_remote = 0;
    b->refused_creates = 0;
    b->creates_past_table = 0;
    match_assign_unit_ranges(&match->state());
    match->rebind_player_ranges();
}

ReplicationSim match_binding_sim(MatchBinding* b) noexcept {
    ReplicationSim sim{};
    sim.context = b;
    sim.movement = movement;
    sim.create_unit = create_unit;
    sim.link_unit = link_unit;
    sim.apply_damage = apply_damage;
    sim.apply_kill = apply_kill;
    sim.toggle_state_flags = toggle_state_flags;
    sim.place_unit = place_unit;
    sim.apply_ground_delta = apply_ground_delta;
    sim.set_movement_speed = set_movement_speed;
    sim.set_air_goal = set_air_goal;
    sim.set_movement_substate = set_movement_substate;
    sim.queue_feature_event = queue_feature_event;
    sim.tree_burn = tree_burn;
    sim.feature_event = feature_event;
    sim.cob_start = cob_start;
    sim.weapon_fire = weapon_fire;
    sim.detonate_projectile = detonate_projectile;
    sim.link_builder = link_builder;
    sim.transfer_unit = transfer_unit;
    sim.full_record_read = full_record_read;
    sim.movement_tick = movement_tick;
    sim.sight_update = sight_update;
    return sim;
}

NetMatchHooks match_binding_hooks(MatchBinding* b) noexcept {
    NetMatchHooks hooks{};
    hooks.context = b;
    hooks.credit = credit_hook;
    hooks.debit = debit_hook;
    hooks.destroy_player_units = destroy_player_units_hook;
    hooks.end_local_game = end_local_game_hook;
    hooks.local_player_won = local_player_won_hook;
    hooks.alliance_changed = alliance_changed_hook;
    hooks.share_sight = share_sight_hook;
    return hooks;
}

void match_binding_install(MatchBinding* b) noexcept {
    sim::match_runtime::MultiplayerHooks hooks{};
    hooks.context = b;
    hooks.local_player_ticked = local_player_ticked;
    hooks.player_status_changed = player_status_changed;
    hooks.unit_created = unit_created;
    hooks.unit_flags_changed = unit_flags_changed;
    hooks.health_route = health_route;
    hooks.health_shared = health_shared;
    hooks.script_started = script_started;
    hooks.shot_fired = shot_fired;
    hooks.unit_killed = unit_killed;
    hooks.feature_hit_elsewhere = feature_hit_hook;
    hooks.feature_changed = feature_changed_hook;
    hooks.shot_intercepted = shot_intercepted;
    hooks.carry_link_changed = carry_link_changed;
    hooks.unit_finished = unit_finished;
    hooks.unit_transferred = unit_transferred;
    hooks.commander_placed = commander_placed;
    b->features = FeatureRecordLink{};
    b->features.world = &b->match->state();
    b->features.context = b;
    b->features.send = send_feature_record;
    b->features.networked = session_live;
    b->match->multiplayer = hooks;
}

void match_binding_tick(MatchBinding* b) {
    auto& game = b->match->state().game;
    // The records the pump applies refuse what does not fit the match, as
    // they did when the match threw for it; a fault the match noted for them,
    // or for a call made between ticks, is not the tick's, so it is cleared.
    const auto pump = [b] {
        (void)net_match_pump(b->net);
        b->match->clear_fault();
    };
    if ((game.sim_run_flags & run_flag_paused) != 0) {
        pump();
        return;
    }
    // The pass that ends the match's load runs at the load's tick, before
    // the clock and the other machines' records reach the first tick.
    b->match->run_load_pass();
    ++game.tick;
    pump();
    b->match->tick();
    if (game.tick % economy_period_ticks == 0)
        net_match_economy_period(b->net);
    net_match_after_tick(b->net);
}

void match_binding_destroy_player_units(MatchBinding* b, uint8_t slot) noexcept {
    if (b->match == nullptr || slot >= OA_PLAYER_COUNT)
        return;
    try {
        b->match->destroy_player_units(slot);
    } catch (const std::exception&) {
    }
}

void match_binding_end_local_game(MatchBinding* b) noexcept {
    if (b->match != nullptr)
        b->match->end_local_game();
}

void match_binding_follow_alliances(MatchBinding* b, uint8_t slot) noexcept {
    if (b->match != nullptr && slot < OA_PLAYER_COUNT)
        b->match->follow_player_alliances(slot);
}

void match_binding_share_sight(MatchBinding* b, uint8_t from, uint8_t to) noexcept {
    if (b->match != nullptr)
        b->match->share_mapped_area(from, to);
}

bool match_binding_local_player_won(const MatchBinding* b) noexcept {
    return b->match != nullptr && b->match->outcome() == sim::scenario::Outcome::victory;
}

void credit_player_resource(World* world, uint8_t player, bool metal, float amount) noexcept {
    if (player >= OA_PLAYER_COUNT)
        return;
    auto& p = world->game.players[player];
    auto* staging = world_player_economy(world, &p);
    if (staging == nullptr)
        return;
    const auto* owner = world_player_ref(world, staging->player);
    const bool present = owner != nullptr && owner->in_use != 0;
    const auto status = owner != nullptr ? owner->status : uint8_t{0};
    if (metal)
        (void)sim::unit_health::credit_metal(
            staging->metal.produced, amount, present, status, world->game.difficulty
        );
    else
        (void)sim::unit_health::credit_energy(
            staging->energy.produced, amount, present, status, world->game.difficulty
        );
}

bool debit_player_resource(World* world, uint8_t player, bool metal, float amount) noexcept {
    if (player >= OA_PLAYER_COUNT)
        return false;
    auto& p = world->game.players[player];
    auto* staging = world_player_economy(world, &p);
    if (staging == nullptr)
        return false;
    return metal ? sim::unit_spawn::player_pay_metal(p, staging->metal.requested, amount)
                 : sim::unit_spawn::player_pay_energy(p, staging->energy.requested, amount);
}

} // namespace oa::netgame::match
