// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/scenario/conditions.hpp"

#include "oa/core/player_setup.h"
#include "oa/data/defs/unit_records.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/formats/tdf.hpp"

#include <array>
#include <cmath>

namespace oa::sim::scenario {
namespace {

constexpr int32_t pass_reach = 2;    // cells either side of the line
constexpr int32_t counted_limit = 1; // walks stop once two units are counted
constexpr uint8_t enemy_player = 1;
constexpr uint8_t local_player = 0;

using UnitEventHandler = void (*)(
    Condition& condition, oa::World& world, const oa::Unit& unit, const ConditionHost& host
);

// The unit events one kind reacts to; null where it ignores the event. No
// kind reacts to a unit's creation.
struct UnitEventHandlers {
    Kind kind{};
    UnitEventHandler destroyed{};
    UnitEventHandler captured{};
};

/// Returns the unit type name of a unit, or null for a unit without a type.
///
/// @param world unit types
/// @param unit the unit
/// @return the type's name
const char* def_name(const oa::World& world, const oa::Unit& unit) {
    const oa::UnitDef* def = oa::world_unit_def_of(&world, &unit);
    return def != nullptr ? def->unit_name : nullptr;
}

/// Tells whether a unit's type has a name, ignoring case.
///
/// @param world unit types
/// @param unit the unit
/// @param name type name
/// @return true when the names match
bool named(const oa::World& world, const oa::Unit& unit, const char* name) {
    const char* type = def_name(world, unit);
    return type != nullptr && formats::tdf::compare_nocase(name, type) == 0;
}

/// Tells whether a unit is the commander of the side its owner plays.
///
/// @param world players, sides and unit types
/// @param unit the unit
/// @return true for the side's commander type
bool side_commander(const oa::World& world, const oa::Unit& unit) {
    const oa::Player* owner = oa::world_player_ref(&world, unit.owner);
    const oa::PlayerSetupInfo* info =
        owner != nullptr ? oa::world_player_info(&world, owner) : nullptr;
    const oa::Side* side = info != nullptr ? oa::world_side(&world, info->side) : nullptr;
    if (info == nullptr || side == nullptr)
        return false;
    return named(world, unit, side->commander);
}

/// Marks a condition met, then plays the victory cue the first time.
///
/// @param[in,out] c the condition; satisfied and celebrated are set
/// @param host victory cue
void celebrate(Condition& c, const ConditionHost& host) {
    c.satisfied = 1;
    if (c.celebrated != 0)
        return;
    if (host.victory_cue != nullptr)
        host.victory_cue(host.context);
    c.celebrated = 1;
}

/// Tells whether a condition is met.
///
/// @param c the condition
/// @return true once satisfied
bool met(const Condition& c) {
    return c.satisfied != 0;
}

/// Asks the host whether a unit holds a movement object.
///
/// @param host movement query; without one no unit has a movement object
/// @param unit the unit
/// @return true for a unit with a movement object
bool has_movement(const ConditionHost& host, const oa::Unit& unit) {
    return host.has_movement != nullptr && host.has_movement(host.context, unit);
}

/// Resolves the named type into its type index and clears the count, before a counting walk.
///
/// @param[in,out] c the condition; type_index and units_counted are written
/// @param world unit types
void resolve_type(Condition& c, const oa::World& world) {
    c.units_counted = 0;
    c.type_index =
        oa::data::defs::unit_defs_type_id(world.unit_defs, world.unit_def_count, c.type_name);
}

/// Tests one unit for UnitTypePassesX or UnitTypePassesZ.
///
/// @param[in,out] c the condition
/// @param world unit types
/// @param cell the unit's cell column or row
/// @param unit walked unit
/// @param host victory cue
/// @return true while unmet (the walk goes on)
bool unit_type_passes_unit(
    Condition& c,
    const oa::World& world,
    int16_t cell,
    const oa::Unit& unit,
    const ConditionHost& host
) {
    if (c.type_name[0] == '\0' || named(world, unit, c.type_name)) {
        if (cell_on_line(cell, c.line))
            celebrate(c, host);
    }
    return !met(c);
}

/// Runs KillEnemyCommander from the handler table.
///
/// @param[in,out] c the condition
/// @param world units, players and types
/// @param unit unit destroyed
/// @param host victory cue
void kill_enemy_commander_handler(
    Condition& c, oa::World& world, const oa::Unit& unit, const ConditionHost& host
) {
    kill_enemy_commander_destroyed(c, world, unit, host);
}

/// Runs KillUnitType from the handler table.
///
/// @param[in,out] c the condition
/// @param world units, players and types
/// @param unit unit destroyed
/// @param host victory cue
void kill_unit_type_handler(
    Condition& c, oa::World& world, const oa::Unit& unit, const ConditionHost& host
) {
    kill_unit_type_destroyed(c, world, unit, host);
}

/// Runs CommanderKilled from the handler table.
///
/// @param[in,out] c the condition
/// @param world units, players and types
/// @param unit unit destroyed
/// @param host unused: the defeat plays no cue
void commander_killed_handler(
    Condition& c, oa::World& world, const oa::Unit& unit, const ConditionHost& host
) {
    (void)host;
    commander_killed_destroyed(c, world, unit);
}

/// Runs UnitTypeKilled from the handler table.
///
/// @param[in,out] c the condition
/// @param world units, players and types
/// @param unit unit destroyed
/// @param host unused: the defeat plays no cue
void unit_type_killed_handler(
    Condition& c, oa::World& world, const oa::Unit& unit, const ConditionHost& host
) {
    (void)host;
    unit_type_killed_destroyed(c, world, unit);
}

/// Runs AllUnitsKilledOfType from the handler table.
///
/// @param[in,out] c the condition
/// @param world units, players and types
/// @param unit unit destroyed
/// @param host unused: the defeat plays no cue
void all_units_killed_of_type_handler(
    Condition& c, oa::World& world, const oa::Unit& unit, const ConditionHost& host
) {
    (void)host;
    all_units_killed_of_type_destroyed(c, world, unit);
}

/// Runs CaptureUnitType from the handler table.
///
/// @param[in,out] c the condition
/// @param world units, players and types
/// @param unit unit captured
/// @param host victory cue
void capture_unit_type_handler(
    Condition& c, oa::World& world, const oa::Unit& unit, const ConditionHost& host
) {
    capture_unit_type_captured(c, world, unit, host);
}

constexpr std::array<UnitEventHandlers, kind_count> unit_event_handlers{{
    {Kind::kill_enemy_commander, kill_enemy_commander_handler, nullptr},
    {Kind::destroy_all_units, nullptr, nullptr},
    {Kind::kill_all_mobile_units, kill_all_mobile_units_destroyed, nullptr},
    {Kind::build_unit_type, nullptr, nullptr},
    {Kind::capture_unit_type, nullptr, capture_unit_type_handler},
    {Kind::kill_all_of_type, kill_all_of_type_destroyed, nullptr},
    {Kind::kill_unit_type, kill_unit_type_handler, nullptr},
    {Kind::move_unit_to_radius, nullptr, nullptr},
    {Kind::unit_type_passes_x, nullptr, nullptr},
    {Kind::unit_type_passes_z, nullptr, nullptr},
    {Kind::victory_timer, nullptr, nullptr},
    {Kind::commander_killed, commander_killed_handler, nullptr},
    {Kind::all_units_killed, nullptr, nullptr},
    {Kind::all_units_killed_of_type, all_units_killed_of_type_handler, nullptr},
    {Kind::unit_type_killed, unit_type_killed_handler, nullptr},
    {Kind::death_timer, nullptr, nullptr},
    {Kind::any_unit_passes_x, nullptr, nullptr},
    {Kind::any_unit_passes_z, nullptr, nullptr},
}};

/// Tells whether every table entry sits at its kind's index, which handler() relies on.
///
/// @return true when the table is in Kind order
constexpr bool unit_event_handlers_in_kind_order() {
    for (std::size_t i = 0; i < unit_event_handlers.size(); ++i)
        if (static_cast<std::size_t>(unit_event_handlers[i].kind) != i)
            return false;
    return true;
}

static_assert(unit_event_handlers_in_kind_order());

/// Returns a kind's handler for a unit event.
///
/// @param kind condition kind
/// @param event unit event
/// @return the handler, or null when the kind ignores the event or either value
///         lies outside its enumeration
UnitEventHandler handler(Kind kind, Event event) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= unit_event_handlers.size())
        return nullptr;
    const UnitEventHandlers& handlers = unit_event_handlers[index];
    switch (event) {
    case Event::unit_destroyed:
        return handlers.destroyed;
    case Event::unit_captured:
        return handlers.captured;
    case Event::unit_created:
        return nullptr;
    }
    return nullptr;
}

} // namespace

bool reacts_to(Kind kind, Event event) noexcept {
    return handler(kind, event) != nullptr;
}

bool dispatch(
    Controller& controller,
    Event event,
    oa::World& world,
    const oa::Unit& unit,
    const ConditionHost& host
) {
    if (event != Event::unit_destroyed && event != Event::unit_captured &&
        event != Event::unit_created)
        return false;
    return visit_conditions(controller, [&](Condition& condition) {
        if (const UnitEventHandler run = handler(condition.kind, event))
            run(condition, world, unit, host);
    });
}

bool cell_on_line(int16_t cell, int32_t line) {
    auto distance = static_cast<int32_t>(
        static_cast<uint32_t>(static_cast<int32_t>(cell)) - static_cast<uint32_t>(line)
    );
    if (distance < 0)
        distance = static_cast<int32_t>(0u - static_cast<uint32_t>(distance));
    return distance <= pass_reach;
}

void kill_enemy_commander_destroyed(
    Condition& c, const oa::World& world, const oa::Unit& destroyed, const ConditionHost& host
) {
    if (destroyed.owner_index == enemy_player && side_commander(world, destroyed))
        celebrate(c, host);
}

void kill_all_mobile_units_destroyed(
    Condition& c, oa::World& world, const oa::Unit& destroyed, const ConditionHost& host
) {
    if (destroyed.owner_index != enemy_player || !has_movement(host, destroyed))
        return;
    c.units_counted = 0;
    scan_player_units(world, enemy_player, [&](const oa::Unit& unit) {
        return count_mobile_unit(c, has_movement(host, unit));
    });
    if (c.units_counted <= counted_limit)
        celebrate(c, host);
}

bool count_mobile_unit(Condition& c, bool unit_has_movement) {
    if (unit_has_movement)
        ++c.units_counted;
    return c.units_counted <= counted_limit;
}

bool build_unit_type_met(Condition& c, oa::World& world, const ConditionHost& host) {
    if (met(c))
        return true;
    if (c.type_index == 0)
        c.type_index =
            oa::data::defs::unit_defs_type_id(world.unit_defs, world.unit_def_count, c.type_name);
    scan_player_units(world, local_player, [&](const oa::Unit& unit) {
        return finished_unit_of_type(c, unit, host);
    });
    return met(c);
}

bool finished_unit_of_type(Condition& c, const oa::Unit& unit, const ConditionHost& host) {
    // An unordered build fraction reads as finished.
    const bool finished = unit.build_remaining == 0.0F || std::isnan(unit.build_remaining);
    if (unit.type_index == c.type_index && finished)
        celebrate(c, host);
    return !met(c);
}

void capture_unit_type_captured(
    Condition& c, const oa::World& world, const oa::Unit& captured, const ConditionHost& host
) {
    if (captured.owner_index != enemy_player)
        return;
    if (named(world, captured, c.type_name))
        celebrate(c, host);
}

void kill_all_of_type_destroyed(
    Condition& c, oa::World& world, const oa::Unit& destroyed, const ConditionHost& host
) {
    if (met(c) || destroyed.owner_index != enemy_player)
        return;
    if (!named(world, destroyed, c.type_name))
        return;
    resolve_type(c, world);
    scan_player_units(world, enemy_player, [&](const oa::Unit& unit) {
        return count_type_match(c, unit);
    });
    if (c.units_counted <= counted_limit)
        celebrate(c, host);
}

bool count_type_match(Condition& c, const oa::Unit& unit) {
    if (unit.type_index == c.type_index)
        ++c.units_counted;
    return c.units_counted <= counted_limit;
}

void kill_unit_type_destroyed(
    Condition& c, const oa::World& world, const oa::Unit& destroyed, const ConditionHost& host
) {
    if (c.kills_left <= 0 || destroyed.owner_index != enemy_player)
        return;
    if (!named(world, destroyed, c.type_name))
        return;
    --c.kills_left;
    if (c.kills_left < 1)
        celebrate(c, host);
}

void move_unit_to_radius_unit(
    Condition& c, const oa::World& world, const oa::Unit& unit, const ConditionHost& host
) {
    if (unit.owner_index != local_player)
        return;
    if (c.type_name[0] != '\0' && !named(world, unit, c.type_name))
        return;
    if (sim::simulation_state::unit_selectable(world, unit))
        celebrate(c, host);
}

bool unit_type_passes_x_met(Condition& c, oa::World& world, const ConditionHost& host) {
    if (!met(c))
        scan_player_units(world, local_player, [&](const oa::Unit& unit) {
            return unit_type_passes_x_unit(c, world, unit, host);
        });
    return met(c);
}

bool unit_type_passes_x_unit(
    Condition& c, const oa::World& world, const oa::Unit& unit, const ConditionHost& host
) {
    return unit_type_passes_unit(c, world, unit.cell_x, unit, host);
}

bool unit_type_passes_z_met(Condition& c, oa::World& world, const ConditionHost& host) {
    if (!met(c))
        scan_player_units(world, local_player, [&](const oa::Unit& unit) {
            return unit_type_passes_z_unit(c, world, unit, host);
        });
    return met(c);
}

bool unit_type_passes_z_unit(
    Condition& c, const oa::World& world, const oa::Unit& unit, const ConditionHost& host
) {
    return unit_type_passes_unit(c, world, unit.cell_z, unit, host);
}

void commander_killed_destroyed(Condition& c, const oa::World& world, const oa::Unit& destroyed) {
    if (destroyed.owner_index == local_player && side_commander(world, destroyed))
        c.satisfied = 1;
}

void unit_type_killed_destroyed(Condition& c, const oa::World& world, const oa::Unit& destroyed) {
    if (!named(world, destroyed, c.type_name))
        return;
    // A 32-bit decrement that wraps, then the signed compare.
    c.kills_left = static_cast<int32_t>(static_cast<uint32_t>(c.kills_left) - 1u);
    if (c.kills_left < 1)
        c.satisfied = 1;
}

void all_units_killed_of_type_destroyed(Condition& c, oa::World& world, const oa::Unit& destroyed) {
    if (!named(world, destroyed, c.type_name))
        return;
    resolve_type(c, world);
    const auto count = [&](const oa::Unit& unit) { return count_type_match(c, unit); };
    scan_player_units(world, local_player, count);
    scan_player_units(world, enemy_player, count);
    if (c.units_counted <= counted_limit)
        c.satisfied = 1;
}

} // namespace oa::sim::scenario
