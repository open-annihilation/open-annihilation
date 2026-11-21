// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "oa/core/world.h"
#include "oa/data/enhanced_mode.hpp"
#include "oa/data/limits.hpp"
#include "oa/data/match_rules.hpp"
#include "oa/sim/script_vm.hpp"

namespace oa::formats::cob {
struct CobProgram;
}

namespace oa::sim::unit_script {

/// Every unit-script function the engine calls by name.
enum class ScriptFunction : uint8_t {
    create,
    killed,
    activate,
    deactivate,
    start_building,
    stop_building,
    start_moving,
    stop_moving,
    move_rate1,
    move_rate2,
    move_rate3,
    set_speed,
    set_direction,
    set_sfx_occupy,
    query_build_info,
    query_nano_piece,
    query_primary,
    query_secondary,
    query_tertiary,
    aim_from_primary,
    aim_from_secondary,
    aim_from_tertiary,
    aim_primary,
    aim_secondary,
    aim_tertiary,
    fire_primary,
    fire_secondary,
    fire_tertiary,
    sweet_spot,
    hit_by_weapon,
    take_damage,
    target_cleared,
    rock_unit,
    set_max_reload_time,
    transport_pickup,
    transport_drop,
    begin_transport,
    end_transport,
    query_transport,
    query_landing_pad,
};

inline constexpr std::size_t script_function_count =
    static_cast<std::size_t>(ScriptFunction::query_landing_pad) + 1;
inline constexpr uint32_t weapon_slot_count = OA_UNIT_WEAPON_COUNT;
inline constexpr int32_t no_script_index = -1;
inline constexpr uint32_t max_arguments = 4;

/// How the engine hands a function to the interpreter.
enum class CallForm : uint8_t {
    bare,      /* no stack arguments; locals keep whatever the context held */
    arguments, /* four locals written, stack pointer = argument count - 1 */
    query,     /* synchronous; four values pushed and read back */
};

/// Whether the engine steps every context (with elapsed 0) right after starting.
enum class ScriptRun : uint8_t {
    deferred = 0,
    immediate = 1,
};

/// The name and arguments 3.1c starts one function with. Where the engine
/// starts a function in more than one way, this is the common one.
struct EngineCall {
    ScriptFunction function{};
    const char* name{}; /* exact, case-sensitive COB function name */
    CallForm form{};
    uint8_t argument_count{};
    ScriptRun run{};
};

/// Returns how the engine calls one script function.
///
/// @param function engine function; an out-of-range value maps to Create
/// @return the function's COB name, call form, argument count and run mode
[[nodiscard]] const EngineCall& engine_call(ScriptFunction function) noexcept;

/// Returns the exact, case-sensitive COB name of an engine function.
///
/// @param function engine function
/// @return the name from engine_call
[[nodiscard]] inline const char* script_function_name(ScriptFunction function) noexcept {
    return engine_call(function).name;
}

/// Returns the Query function of a weapon slot.
///
/// @param slot weapon slot, 0..2 = primary..tertiary; others map to primary
/// @return QueryPrimary, QuerySecondary or QueryTertiary
[[nodiscard]] ScriptFunction weapon_query_function(uint32_t slot) noexcept;
/// Returns the AimFrom function of a weapon slot.
///
/// @param slot weapon slot, 0..2 = primary..tertiary; others map to primary
/// @return AimFromPrimary, AimFromSecondary or AimFromTertiary
[[nodiscard]] ScriptFunction weapon_aim_from_function(uint32_t slot) noexcept;
/// Returns the Aim function of a weapon slot.
///
/// @param slot weapon slot, 0..2 = primary..tertiary; others map to primary
/// @return AimPrimary, AimSecondary or AimTertiary
[[nodiscard]] ScriptFunction weapon_aim_function(uint32_t slot) noexcept;
/// Returns the Fire function of a weapon slot.
///
/// @param slot weapon slot, 0..2 = primary..tertiary; others map to primary
/// @return FirePrimary, FireSecondary or FireTertiary
[[nodiscard]] ScriptFunction weapon_fire_function(uint32_t slot) noexcept;
/// Returns the function that signals a movement rate.
///
/// @param rate movement rate, 0..3; others map to 0
/// @return StopMoving for rate 0, MoveRate1..3 for 1..3
[[nodiscard]] ScriptFunction move_rate_function(uint32_t rate) noexcept;

/// Up to four 32-bit script arguments.
struct ScriptArgs {
    int32_t values[max_arguments]{};
    uint32_t count{};
};

/// Packs up to four values as script arguments.
///
/// @param values arguments, each converted to int32_t
/// @return the arguments and their count
template <class... T>
[[nodiscard]] constexpr ScriptArgs script_args(T... values) noexcept {
    static_assert(sizeof...(T) <= max_arguments, "engine script calls take at most four arguments");
    ScriptArgs args{{static_cast<int32_t>(values)...}, sizeof...(T)};
    return args;
}

enum class ScriptCallStatus : uint8_t {
    started,         /* context claimed (and run, when immediate) */
    no_script,       /* unit has no attached script */
    no_function,     /* the script lacks this function */
    no_free_context, /* all eight contexts busy */
    fault,           /* the bounded interpreter stopped on an error */
};

/// One unit's script: the interpreter and the function indices resolved from
/// the COB name table. vm is null for an unscripted slot.
struct UnitScript {
    sim::script_vm::Vm* vm{};
    int32_t function_index[script_function_count]{};
    sim::script_vm::ErrorCode last_fault{};
};

/// Attaches a unit's interpreter and resolves every engine function name.
///
/// The side table is keyed by World and slot; it does not own the Vm and
/// leaves Unit.script (the spawn presence flag) alone. Attaching over an
/// existing interpreter replaces it.
///
/// @param world World the unit belongs to
/// @param unit unit record inside world->units; slot 0 is refused
/// @param vm interpreter to attach; must outlive the attachment
/// @param program the unit's COB program, whose name table is searched
/// @return false when the unit is outside the World, vm is null or no table can be allocated
bool unit_script_attach(
    World* world, const Unit* unit, sim::script_vm::Vm* vm, const formats::cob::CobProgram& program
);
/// Detaches a unit's interpreter; the World's table is freed with its last entry.
///
/// @param world World the unit belongs to
/// @param unit unit whose interpreter is detached
/// @param vm when not null, only this interpreter is detached, so a
///        replacement attached since stays
void unit_script_detach(World* world, const Unit* unit, const sim::script_vm::Vm* vm = nullptr);
/// Returns a unit's attached script.
///
/// @param world World the unit belongs to
/// @param unit unit record inside world->units
/// @return the script, or null when the unit has no attached interpreter
[[nodiscard]] UnitScript* unit_script_of(World* world, const Unit* unit);

/// Starts a script function with arguments.
///
/// Writes four locals and sets the stack pointer to the argument count - 1,
/// then steps all contexts at once when run is immediate.
///
/// @param world World the unit belongs to
/// @param[in,out] unit unit whose script runs
/// @param function engine function to start
/// @param args up to four arguments; a larger count is clamped to four
/// @param run whether to step every context right after starting
/// @param callback receives the RETURN value, or 0 when the function is
///        missing or no context is free
/// @return how the call went
/// @quirk The callback receives 0 on those failures, as in 3.1c.
ScriptCallStatus unit_script_call(
    World* world,
    Unit* unit,
    ScriptFunction function,
    ScriptArgs args,
    ScriptRun run = ScriptRun::deferred,
    sim::script_vm::ReturnCallback callback = {}
);
/// Starts a script function by its index in the COB table.
///
/// Calls that arrive from another player's simulation name the function by
/// index. Arguments and callback behave as in unit_script_call.
///
/// @param world World the unit belongs to
/// @param[in,out] unit unit whose script runs
/// @param script_index index in the COB function table; negative is no_function
/// @param args up to four arguments; a larger count is clamped to four
/// @param run whether to step every context right after starting
/// @param callback receives the RETURN value, or 0 on the failures unit_script_call reports that way
/// @return how the call went
ScriptCallStatus unit_script_call_index(
    World* world,
    Unit* unit,
    int32_t script_index,
    ScriptArgs args,
    ScriptRun run = ScriptRun::deferred,
    sim::script_vm::ReturnCallback callback = {}
);
/// Starts a script function without touching its stack slots (stack pointer -1).
///
/// @param world World the unit belongs to
/// @param[in,out] unit unit whose script runs
/// @param function engine function to start
/// @param run whether to step every context right after starting
/// @param callback receives the RETURN value; a failure does not invoke it
/// @return how the call went
ScriptCallStatus unit_script_call_bare(
    World* world,
    Unit* unit,
    ScriptFunction function,
    ScriptRun run = ScriptRun::deferred,
    sim::script_vm::ReturnCallback callback = {}
);
/// Runs a script function synchronously and reads four stack slots back.
///
/// Each non-null value is pushed and then replaced by the matching stack slot;
/// null entries push zero and are not written.
///
/// @param world World the unit belongs to
/// @param[in,out] unit unit whose script runs
/// @param function engine function to run
/// @param[in,out] first value for stack slot 0, or null
/// @param[in,out] second value for stack slot 1, or null
/// @param[in,out] third value for stack slot 2, or null
/// @param[in,out] fourth value for stack slot 3, or null
/// @return how the call went
ScriptCallStatus unit_script_query(
    World* world,
    Unit* unit,
    ScriptFunction function,
    int32_t* first,
    int32_t* second = nullptr,
    int32_t* third = nullptr,
    int32_t* fourth = nullptr
);
/// Advances one unit's contexts and piece motion.
///
/// @param world World the unit belongs to
/// @param[in,out] unit unit whose script runs
/// @param elapsed scheduler ticks since the previous step
/// @return started when the step ran cleanly, else no_script or fault
ScriptCallStatus unit_script_tick(World* world, Unit* unit, uint32_t elapsed);

/// Tests whether a unit with a given id rides on a unit (COB opcode 0x10044000).
///
/// @param world World the units belong to
/// @param unit carrier whose attached children are searched
/// @param unit_id Unit.id looked for
/// @return 1 when found, else 0; the walk stops after unit_slot_count steps,
///         so a cycle ends it
[[nodiscard]] uint32_t
unit_script_is_carrying(const World* world, const Unit* unit, uint32_t unit_id) noexcept;
/// Returns the id of the unit carrying a unit (COB opcode 0x10045000).
///
/// @param world World the units belong to
/// @param unit unit whose attach parent is read
/// @return the carrier's Unit.id, or 0 when nothing carries it
[[nodiscard]] uint32_t unit_script_carrier_id(const World* world, const Unit* unit) noexcept;

/// GET/SET unit value selectors (the numbering compiled into COB code).
enum class UnitValue : int32_t {
    activation = 1,
    standing_move_orders = 2,
    standing_fire_orders = 3,
    health = 4,
    in_build_stance = 5,
    busy = 6,
    piece_xz = 7,
    piece_y = 8,
    unit_xz = 9,
    unit_y = 10,
    unit_height = 11,
    xz_atan = 12,
    xz_hypot = 13,
    atan = 14,
    hypot = 15,
    ground_height = 16,
    build_percent_left = 17,
    yard_open = 18,
    bugger_off = 19,
    armored = 20,
};

/// Unit.state_flags bits a script reads or writes.
inline constexpr uint8_t state_flag_activated = 0x01;
inline constexpr uint8_t state_flag_armored = 0x02;
/// Unit.build_flags bits a script reads or writes.
inline constexpr uint8_t build_flag_in_build_stance = OA_UNIT_BUILD_IN_BUILD_STANCE;
inline constexpr uint8_t build_flag_busy = OA_UNIT_BUILD_BUSY;
inline constexpr uint8_t build_flag_yard_open = OA_UNIT_BUILD_YARD_OPEN;
inline constexpr uint8_t build_flag_bugger_off = OA_UNIT_BUILD_BUGGER_OFF;
/// Unit.events bit raised by any SET.
inline constexpr uint16_t event_script_state_changed = 0x0004; /* ? */

/// Engine services the GET/SET handlers reach outside the unit record.
struct UnitValueServices {
    void* context{};
    /// World position of a script piece (16.16 x, y, z).
    FixedVec3 (*piece_world)(void* context, World* world, Unit* unit, uint32_t piece){};
    /// 16-bit heading of the vector (dx, dz).
    uint16_t (*direction)(void* context, int32_t dx, int32_t dz){};
    /// Horizontal length of (dx, dz), truncated to an integer.
    uint32_t (*distance)(void* context, int32_t dx, int32_t dz){};
    /// Integer terrain height at the 16.16 map position.
    int32_t (*ground_height)(void* context, World* world, int32_t x, int32_t z){};
    /// Sets or clears activation (mask 1) or armored (mask 2) with its side effects.
    void (*set_state_flag)(void* context, World* world, Unit* unit, uint8_t mask, bool enabled){};
    /// Opens or closes a factory yard.
    void (*set_yard_open)(void* context, World* world, Unit* unit, int32_t open){};
    /// The rules the match plays by: the unit-script extensions a profile
    /// mounts (MatchRules::script_get, script_set, script_fidelity). Unset,
    /// nothing is mounted, as in 3.1c.
    data::match_rules::MatchRulesView rules{};
    /// The capacities the match was built with, such as the most units a
    /// player may have; null for 3.1c's.
    const data::limits::Limits* limits{};
    /// The Enhanced Mode features the match plays; with capacity, unit ids
    /// run over the participating players' allocated slots. Unset, none, as
    /// in 3.1c.
    data::enhanced_mode::FeatureSet features{};
};

/// Reads a GET unit value.
///
/// Positions packed by COB hold x in the high word and z in the low word.
/// Selectors 1..20 read the base values; any other selector reads the
/// extension mounted there (MatchRules::script_get, through
/// UnitValueServices::rules), or 0 when none is. The extensions read:
///
/// - unit.kills-x100: the caller's kills (Unit.veteran_level) times 100;
/// - unit.min-id: 1, the lowest unit id;
/// - unit.max-id: the unit limit the game recorded (Game.max_units_setting)
///   times 10; a skirmish or multiplayer game records the limit it plays at,
///   so this is the table's last id, and a campaign mission records the
///   run's unit limit; a world that records no limit reads its active limit
///   (Game.units_per_player) instead;
/// - unit.my-id: the caller's own id (Unit.id);
/// - unit.owner-of(id): the owner player index (Unit.owner_index) of slot
///   id & 0xffff;
/// - unit.build-percent-left-of(id): the BUILD_PERCENT_LEFT value of the unit
///   in slot id, the whole 32-bit argument;
/// - unit.allied-with(id): 1 when the caller's owner has allied the owner of
///   slot id & 0xffff (Player.alliance, one-way; every player allies itself),
///   0 for an owner index of 10 or more;
/// - unit.is-local(id): 1 when the owner of slot id & 0xffff is played on
///   this machine (Player.status local or computer), else 0; this differs
///   between machines by design.
///
/// Under exact fidelity owner-of, build-percent-left-of, allied-with and
/// is-local read a slot whether or not a unit lives there, so a free slot
/// gives what its record still holds. A slot past the unit table, or past
/// the highest id unit.max-id reports, reads 0. build-percent-left-of reads
/// the slot the argument times the record size (0x118 bytes), wrapped to 32
/// bits, lands on, and 0 when that is not a whole record before the table's
/// last slot. Under safe fidelity those four read 0 for any slot without a
/// live unit, and build-percent-left-of takes the argument as the slot.
///
/// @param world World the unit belongs to
/// @param unit unit whose script asks
/// @param selector UnitValue selector, or an index an extension is mounted at
/// @param first selector argument: piece index, unit id, packed x/z or dx
/// @param second second argument; dz for atan and hypot, otherwise unused
/// @param services engine services for pieces, trigonometry and terrain, and
///        the mounted extensions
/// @return the value, or 0 for an unknown selector or a dead or missing unit;
///         health is 0 for a type whose UnitDef.max_damage is zero
/// @quirk Health is Unit.health * 100 divided by UnitDef.max_damage as unsigned
///        32-bit values, as 3.1c computes it, and ignores any unit-id argument.
/// @quirk unit.max-id follows the recorded limit, which a campaign mission
///        keeps at the run's unit limit rather than its own, so there it can
///        name ids past the table, which read 0, or stop short of its last id.
[[nodiscard]] int32_t unit_script_get_value(
    World* world,
    Unit* unit,
    int32_t selector,
    int32_t first,
    int32_t second,
    const UnitValueServices& services
);
/// Writes a SET unit value.
///
/// Always raises the script-state-changed event bit, even for a selector that
/// cannot be set.
///
/// @param world World the unit belongs to
/// @param[in,out] unit unit whose script sets the value
/// @param selector UnitValue selector
/// @param value new value; flag selectors use its low bit or its truth
/// @param services engine services for activation, armour and the yard
void unit_script_set_value(
    World* world, Unit* unit, int32_t selector, int32_t value, const UnitValueServices& services
);

} // namespace oa::sim::unit_script
