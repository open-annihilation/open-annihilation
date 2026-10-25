// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Records a replayed game's timeline from the world: begin reads the map's
// water from its cells, the match's event hooks add events as the replay
// runs, and after_tick samples the mobile units and the players. Every value is an integer taken from the canonical records,
// so the same replay gives the same timeline on every platform.

#include "oa/media/director/timeline_recorder.hpp"

#include "oa/core/world.h"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/simulation_state.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace oa::media::director {
namespace {

using oa::sim::match_runtime::KillOutcome;
using oa::sim::match_runtime::ShotSource;

/// Bits of a 16.16 world coordinate below the whole map pixel.
constexpr int32_t fraction_bits = 16;
/// Build progress still to go when a unit is not started, in 65536ths.
constexpr float build_left_scale = 65536.0f;
/// The largest build_left: a unit not started.
constexpr uint32_t build_left_full = 65536;

/// Returns a world point in whole map pixels.
///
/// @param position a 16.16 world position
/// @return each coordinate shifted right by 16 bits, rounding toward
///         negative infinity
WorldPoint point_of(const oa::FixedVec3& position) noexcept {
    return WorldPoint{
        position.x >> fraction_bits,
        position.y >> fraction_bits,
        position.z >> fraction_bits,
    };
}

/// The side of a map cell, in map pixels.
/// Percent: the whole of something.
constexpr uint32_t whole_percent = 100;

/// Returns the water squares that cover a length of the map.
///
/// @param pixels the length, map pixels
/// @return ceil(pixels / water_square_pixels), 0 for none
int64_t ceil_squares(int32_t pixels) noexcept {
    return pixels > 0 ? (int64_t{pixels} + water_square_pixels - 1) / water_square_pixels : 0;
}

/// Returns a player index as the timeline keeps it.
///
/// @param index a player index from a record
/// @return the index, or no_player when it names none of the ten players
uint8_t player_of(uint8_t index) noexcept {
    return index < OA_PLAYER_COUNT ? index : no_player;
}

/// Returns a float truncated to a whole number of at least zero.
///
/// @param value the value
/// @return the value truncated toward zero, held within [0, UINT32_MAX];
///         not-a-number gives 0
uint32_t whole_units(float value) noexcept {
    constexpr float limit{4294967296.0f};
    if (!(value > 0.0f))
        return 0;
    if (value >= limit)
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(value);
}

/// Returns a float truncated to a whole signed number.
///
/// @param value the value
/// @return the value truncated toward zero, held within int32_t;
///         not-a-number gives 0
int32_t whole_signed(float value) noexcept {
    constexpr float high{2147483648.0f};
    constexpr float low{-2147483648.0f};
    if (!(value == value))
        return 0;
    if (value >= high)
        return std::numeric_limits<int32_t>::max();
    if (value <= low)
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}

/// Returns a count held at zero or more.
///
/// @param count a signed count from a record
/// @return the count, or 0 when it is negative
uint32_t count_of(int32_t count) noexcept {
    return count > 0 ? static_cast<uint32_t>(count) : 0u;
}

/// Returns a unit's build progress still to go.
///
/// @param build_remaining Unit.build_remaining, 1 unfinished to 0 finished
/// @return 65536ths, 0 finished to 65536 not started, truncated
uint32_t build_left_of(float build_remaining) noexcept {
    if (!(build_remaining > 0.0f))
        return 0;
    if (build_remaining >= 1.0f)
        return build_left_full;
    return static_cast<uint32_t>(build_remaining * build_left_scale);
}

/// Returns the weapon id a weapon-type reference names.
///
/// @param world the world
/// @param reference a WeaponDef reference; 0 is none
/// @return the weapon's id, or 0 for none or one past the weapon table
uint16_t weapon_id_of(const oa::World& world, oa_ref32 reference) noexcept {
    const uint32_t count{oa::world_weapon_def_count(&world)};
    if (reference == 0 || static_cast<uint32_t>(reference) > count)
        return 0;
    const uint32_t id{reference - 1u};
    return id <= std::numeric_limits<uint16_t>::max() ? static_cast<uint16_t>(id) : uint16_t{0};
}

/// Returns the unit slot a unit reference names.
///
/// @param world the world
/// @param reference a Unit reference; 0 is none
/// @return the slot, or 0 for none or one past the unit table
uint16_t slot_of(const oa::World& world, oa_ref32 reference) noexcept {
    const uint32_t slot{oa::oa_unit_slot_from_ref(reference)};
    return slot < world.unit_slot_count && slot <= std::numeric_limits<uint16_t>::max()
               ? static_cast<uint16_t>(slot)
               : uint16_t{0};
}

/// Returns a unit slot's record when the slot holds a unit.
///
/// @param world the world
/// @param slot the slot
/// @return the unit, or null for slot 0, a slot past the table or an empty one
const oa::Unit* unit_in(const oa::World& world, uint32_t slot) noexcept {
    if (slot == 0)
        return nullptr;
    const oa::Unit* unit{oa::world_unit_at(&world, slot)};
    return unit != nullptr && unit->type_index != 0 ? unit : nullptr;
}

/// Returns a character array's text up to its first zero byte.
///
/// @param text the array
/// @param capacity its size in bytes
/// @return the text
std::string text_of(const char* text, size_t capacity) {
    const void* end{std::memchr(text, 0, capacity)};
    const size_t length{
        end != nullptr ? static_cast<size_t>(static_cast<const char*>(end) - text) : capacity
    };
    return std::string(text, length);
}

/// Adds a weapon to the timeline's weapons unless it is there already.
///
/// @param[in,out] timeline the timeline
/// @param world the world
/// @param weapon_id the weapon's id; 0 adds nothing
void note_weapon(Timeline& timeline, const oa::World& world, uint16_t weapon_id) {
    if (weapon_id == 0)
        return;
    auto& weapons{timeline.header.weapons};
    const auto at{std::lower_bound(
        weapons.begin(), weapons.end(), weapon_id, [](const TimelineWeapon& weapon, uint16_t id) {
            return weapon.weapon_id < id;
        }
    )};
    if (at != weapons.end() && at->weapon_id == weapon_id)
        return;
    const oa::WeaponDef* definition{oa::world_weapon_def(&world, oa::oa_ref_from_index(weapon_id))};
    TimelineWeapon weapon{};
    weapon.weapon_id = weapon_id;
    if (definition != nullptr) {
        weapon.name = text_of(definition->name, sizeof definition->name);
        weapon.range = definition->range;
        weapon.flags = definition->flags;
        weapon.area_of_effect = definition->area_of_effect;
        weapon.damage = definition->damage_default;
        weapon.shake_magnitude = definition->shake_magnitude;
    }
    weapons.insert(at, std::move(weapon));
}

/// Adds a unit type, and the weapons it carries, to the timeline unless it
/// is there already.
///
/// @param[in,out] timeline the timeline
/// @param world the world
/// @param unit a unit of the type
void note_unit_type(Timeline& timeline, const oa::World& world, const oa::Unit& unit) {
    const uint16_t type_index{unit.type_index};
    if (type_index == 0)
        return;
    auto& types{timeline.header.unit_types};
    const auto at{std::lower_bound(
        types.begin(), types.end(), type_index, [](const TimelineUnitType& type, uint16_t index) {
            return type.type_index < index;
        }
    )};
    if (at != types.end() && at->type_index == type_index)
        return;
    const oa::UnitDef* definition{oa::world_unit_def_of(&world, &unit)};
    TimelineUnitType type{};
    type.type_index = type_index;
    if (definition != nullptr) {
        type.unit_name = text_of(definition->unit_name, sizeof definition->unit_name);
        type.mobile = definition->bm_code != 0;
        type.can_fly = (definition->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
        type.builder = (definition->flags & OA_UNIT_DEF_FLAG_BUILDER) != 0;
        type.metal_cost = whole_units(definition->build_cost_metal);
        type.energy_cost = whole_units(definition->build_cost_energy);
        type.max_damage = definition->max_damage;
        type.build_distance = count_of(definition->build_distance);
        type.weapons = {
            weapon_id_of(world, definition->weapon1),
            weapon_id_of(world, definition->weapon2),
            weapon_id_of(world, definition->weapon3),
        };
    }
    const std::array<uint16_t, unit_weapon_slots> weapons{type.weapons};
    types.insert(at, std::move(type));
    for (const uint16_t weapon_id : weapons)
        note_weapon(timeline, world, weapon_id);
}

/// Returns a player's allies as a bit set.
///
/// @param player the player
/// @param index the player's index
/// @return bit n set when the player is allied with player n, itself left out
uint16_t allies_of(const oa::Player& player, uint8_t index) noexcept {
    uint16_t allies{};
    for (uint8_t other{}; other < OA_PLAYER_COUNT; ++other)
        if (other != index && player.alliance[other] != 0)
            allies = static_cast<uint16_t>(allies | (1u << other));
    return allies;
}

/// Returns the timeline's entry for a player, adding it from the world when
/// it is not there yet.
///
/// @param[in,out] timeline the timeline
/// @param world the world
/// @param index the player's index, below OA_PLAYER_COUNT
/// @return the entry
TimelinePlayer& note_player(Timeline& timeline, const oa::World& world, uint8_t index) {
    auto& players{timeline.header.players};
    const auto at{std::lower_bound(
        players.begin(), players.end(), index, [](const TimelinePlayer& player, uint8_t wanted) {
            return player.index < wanted;
        }
    )};
    if (at != players.end() && at->index == index)
        return *at;
    const oa::Player& record{world.game.players[index]};
    const oa::PlayerSetupInfo* info{oa::world_player_info(&world, &record)};
    TimelinePlayer player{};
    player.index = index;
    player.name = text_of(record.name, sizeof record.name);
    player.allies = allies_of(record, index);
    player.viewer = index == timeline.header.viewer_player;
    if (info != nullptr) {
        player.side = info->side;
        player.color = info->color;
        player.watcher = (info->options & OA_SETUP_OPTION_WATCHER) != 0;
    }
    return *players.insert(at, std::move(player));
}

/// Adds a created event for a unit and notes its type and owner.
///
/// The first unit a player is seen with gives the player's start position.
///
/// @param[in,out] timeline the timeline
/// @param world the world
/// @param slot the unit's slot
void record_created(Timeline& timeline, const oa::World& world, uint16_t slot) {
    const oa::Unit* unit{unit_in(world, slot)};
    if (unit == nullptr)
        return;
    note_unit_type(timeline, world, *unit);
    TimelineEvent event{};
    event.tick = world.game.tick;
    event.kind = EventKind::created;
    event.unit = slot;
    event.owner = player_of(unit->owner_index);
    event.unit_type = unit->type_index;
    event.build_left = build_left_of(unit->build_remaining);
    event.at = point_of(unit->position);
    if (oa::sim::match_runtime::side_commander(world, *unit))
        event.flags = static_cast<uint8_t>(event.flags | event_flag::commander);
    if (event.owner != no_player) {
        TimelinePlayer& player{note_player(timeline, world, event.owner)};
        if (!player.owned_units) {
            player.owned_units = true;
            player.start = event.at;
        }
    }
    timeline.events.push_back(event);
}

/// Fills the header's water map from the map's cells and the sea level.
///
/// @param[in,out] header the header; its map bounds are set
/// @param world the world; a world without cells gives a map of no water
void record_water(TimelineHeader& header, const oa::World& world) {
    const int64_t columns{ceil_squares(header.map_width)};
    const int64_t rows{ceil_squares(header.map_height)};
    header.water_columns = static_cast<int32_t>(columns);
    header.water_rows = static_cast<int32_t>(rows);
    const size_t squares{static_cast<size_t>(columns * rows)};
    std::vector<uint32_t> under(squares, 0);
    std::vector<uint32_t> cells(squares, 0);
    const int32_t sea{world.game.sea_level};
    for (int32_t z{}; z < world.game.map_height; ++z)
        for (int32_t x{}; x < world.game.map_width; ++x) {
            const oa::MapPlot* plot{oa::world_plot(&world, x, z)};
            if (plot == nullptr)
                continue;
            const WorldPoint middle{
                x * OA_MAP_CELL_PIXELS + OA_MAP_CELL_PIXELS / 2,
                plot->height,
                z * OA_MAP_CELL_PIXELS + OA_MAP_CELL_PIXELS / 2,
            };
            const int64_t column{int64_t{middle.x} / water_square_pixels};
            const int32_t row_pixels{ground_row(middle)};
            if (row_pixels < 0 || column >= columns)
                continue;
            const int64_t row{int64_t{row_pixels} / water_square_pixels};
            if (row >= rows)
                continue;
            const size_t square{static_cast<size_t>(row * columns + column)};
            ++cells[square];
            if (int32_t{plot->height} <= sea)
                ++under[square];
        }
    header.water.assign(squares, 0);
    for (size_t square{}; square < squares; ++square)
        if (cells[square] != 0)
            header.water[square] =
                static_cast<uint8_t>(under[square] * whole_percent / cells[square]);
}

/// Returns the recorder an event hook's context names.
///
/// @param context EventHooks::context
/// @return the recorder
TimelineRecorder& recorder_of(void* context) noexcept {
    return *static_cast<TimelineRecorder*>(context);
}

} // namespace

void TimelineRecorder::begin(
    const oa::World& world, uint8_t viewer_player, std::string_view map_name
) {
    timeline_ = Timeline{};
    sampled_tick_ = 0;
    sampled_ = false;
    failure_ = nullptr;
    TimelineHeader& header{timeline_.header};
    header.map_name = std::string(map_name);
    header.map_width = world.game.map_pixel_width;
    header.map_height = world.game.map_pixel_height;
    header.first_tick = world.game.tick;
    header.last_tick = world.game.tick;
    header.viewer_player = player_of(viewer_player);
    record_water(header, world);
    for (uint8_t index{}; index < OA_PLAYER_COUNT; ++index)
        if (oa::sim::simulation_state::player_slot_active(index, world.game.players[index]))
            note_player(timeline_, world, index);
    // Units already in the world are recorded as created on the first tick,
    // commanders first, so that a player's start is its commander's place.
    for (const bool commanders : {true, false})
        for (uint32_t slot{1}; slot < world.unit_slot_count; ++slot) {
            const oa::Unit* unit{unit_in(world, slot)};
            if (unit == nullptr || (unit->flags & OA_UNIT_FLAG_LIVE) == 0 ||
                slot > std::numeric_limits<uint16_t>::max() ||
                oa::sim::match_runtime::side_commander(world, *unit) != commanders)
                continue;
            record_created(timeline_, world, static_cast<uint16_t>(slot));
        }
}

oa::sim::match_runtime::EventHooks TimelineRecorder::event_hooks() noexcept {
    oa::sim::match_runtime::EventHooks hooks{};
    hooks.context = this;
    hooks.unit_created = &TimelineRecorder::unit_created;
    hooks.unit_finished = &TimelineRecorder::unit_finished;
    hooks.shot_placed = &TimelineRecorder::shot_placed;
    hooks.shot_detonated = &TimelineRecorder::shot_detonated;
    hooks.unit_damaged = &TimelineRecorder::unit_damaged;
    hooks.unit_died = &TimelineRecorder::unit_died;
    return hooks;
}

void TimelineRecorder::after_tick(const oa::World& world, const UnitVisibilityHooks& visibility) {
    const uint32_t tick{world.game.tick};
    timeline_.header.last_tick = tick;
    if (sampled_ && tick <= sampled_tick_)
        return;
    const uint32_t first{timeline_.header.first_tick};
    if (tick % sample_period_ticks == first % sample_period_ticks) {
        sampled_ = true;
        sampled_tick_ = tick;
        for (uint32_t slot{1};
             slot < world.unit_slot_count && slot <= std::numeric_limits<uint16_t>::max();
             ++slot) {
            const oa::Unit* unit{unit_in(world, slot)};
            if (unit == nullptr || (unit->flags & OA_UNIT_FLAG_LIVE) == 0)
                continue;
            const oa::UnitDef* definition{oa::world_unit_def_of(&world, unit)};
            if (definition == nullptr || definition->bm_code == 0)
                continue;
            note_unit_type(timeline_, world, *unit);
            UnitSample sample{};
            sample.tick = tick;
            sample.unit = static_cast<uint16_t>(slot);
            sample.unit_type = unit->type_index;
            sample.owner = player_of(unit->owner_index);
            sample.heading = unit->heading;
            sample.health = unit->health;
            sample.at = point_of(unit->position);
            const bool visible{
                visibility.visible == nullptr ||
                visibility.visible(visibility.context, static_cast<uint16_t>(slot))
            };
            uint8_t flags{};
            if (visible)
                flags = static_cast<uint8_t>(flags | sample_flag::visible);
            if ((unit->state_flags & OA_UNIT_STATE_CLOAKED) != 0)
                flags = static_cast<uint8_t>(flags | sample_flag::cloaked);
            if (unit->attach_parent != 0)
                flags = static_cast<uint8_t>(flags | sample_flag::carried);
            if (unit->build_remaining != 0.0f)
                flags = static_cast<uint8_t>(flags | sample_flag::unfinished);
            sample.flags = flags;
            timeline_.samples.push_back(sample);
        }
    }
    if (tick % stats_period_ticks == first % stats_period_ticks)
        for (uint8_t index{}; index < OA_PLAYER_COUNT; ++index) {
            const oa::Player& player{world.game.players[index]};
            if (!oa::sim::simulation_state::player_slot_active(index, player))
                continue;
            PlayerStats stats{};
            stats.tick = tick;
            stats.player = index;
            stats.kills = count_of(player.kills);
            stats.losses = count_of(player.losses);
            stats.commanders_killed = count_of(player.commanders_killed);
            stats.commanders_lost = count_of(player.commanders_lost);
            stats.unit_count = player.unit_count;
            stats.metal = whole_signed(player.metal);
            stats.energy = whole_signed(player.energy);
            timeline_.stats.push_back(stats);
        }
}

void TimelineRecorder::finish(const oa::World& world, const ReplayVerdict& verdict) {
    if (failure_)
        std::rethrow_exception(failure_);
    TimelineHeader& header{timeline_.header};
    // Copied out: std::max takes references, and the packed tick field cannot
    // be bound to one.
    const uint32_t tick{world.game.tick};
    header.last_tick = std::max(header.first_tick, tick);
    timeline_.verdict = verdict;
    const uint32_t after_last{
        header.last_tick < std::numeric_limits<uint32_t>::max() ? header.last_tick + 1u
                                                                : header.last_tick
    };
    uint32_t usable_end{after_last};
    if (verdict.first_error_tick != 0 && verdict.first_error_tick < usable_end)
        usable_end = std::max(verdict.first_error_tick, header.first_tick);
    timeline_.usable_end_tick = usable_end;
}

Timeline TimelineRecorder::take() {
    if (failure_)
        std::rethrow_exception(failure_);
    Timeline timeline{std::move(timeline_)};
    timeline_ = Timeline{};
    sampled_tick_ = 0;
    sampled_ = false;
    return timeline;
}

void TimelineRecorder::unit_created(void* context, const oa::World& world, uint16_t unit) {
    auto& recorder{recorder_of(context)};
    try {
        record_created(recorder.timeline_, world, unit);
    } catch (...) {
        if (!recorder.failure_)
            recorder.failure_ = std::current_exception();
    }
}

void TimelineRecorder::unit_finished(
    void* context, const oa::World& world, uint16_t unit, uint16_t builder
) {
    auto& recorder{recorder_of(context)};
    try {
        const oa::Unit* record{unit_in(world, unit)};
        if (record == nullptr)
            return;
        note_unit_type(recorder.timeline_, world, *record);
        TimelineEvent event{};
        event.tick = world.game.tick;
        event.kind = EventKind::finished;
        event.unit = unit;
        event.owner = player_of(record->owner_index);
        event.unit_type = record->type_index;
        event.other_unit = builder;
        event.at = point_of(record->position);
        recorder.timeline_.events.push_back(event);
    } catch (...) {
        if (!recorder.failure_)
            recorder.failure_ = std::current_exception();
    }
}

void TimelineRecorder::shot_placed(
    void* context,
    const oa::World& world,
    const oa::Projectile& shot,
    ShotSource source,
    const oa::FixedVec3* aim,
    uint16_t target_unit
) {
    auto& recorder{recorder_of(context)};
    try {
        TimelineEvent event{};
        event.tick = world.game.tick;
        event.kind = EventKind::shot;
        event.unit = source == ShotSource::meteor ? uint16_t{0} : slot_of(world, shot.source);
        event.owner = player_of(shot.owner_index);
        event.weapon = weapon_id_of(world, shot.def);
        event.other_unit = target_unit;
        event.at = point_of(shot.origin);
        if (aim != nullptr) {
            event.target = point_of(*aim);
            event.flags = static_cast<uint8_t>(event.flags | event_flag::has_target);
        }
        if (source == ShotSource::burst)
            event.flags = static_cast<uint8_t>(event.flags | event_flag::burst);
        note_weapon(recorder.timeline_, world, event.weapon);
        recorder.timeline_.events.push_back(event);
    } catch (...) {
        if (!recorder.failure_)
            recorder.failure_ = std::current_exception();
    }
}

void TimelineRecorder::shot_detonated(
    void* context, const oa::World& world, const oa::Projectile& shot, uint16_t direct_unit
) {
    auto& recorder{recorder_of(context)};
    try {
        TimelineEvent event{};
        event.tick = world.game.tick;
        event.kind = EventKind::detonation;
        event.unit = slot_of(world, shot.source);
        event.owner = player_of(shot.owner_index);
        event.weapon = weapon_id_of(world, shot.def);
        event.other_unit = direct_unit;
        event.at = point_of(shot.position);
        note_weapon(recorder.timeline_, world, event.weapon);
        recorder.timeline_.events.push_back(event);
    } catch (...) {
        if (!recorder.failure_)
            recorder.failure_ = std::current_exception();
    }
}

void TimelineRecorder::unit_damaged(
    void* context,
    const oa::World& world,
    uint16_t target,
    uint16_t source,
    int16_t amount,
    uint8_t kind
) {
    auto& recorder{recorder_of(context)};
    try {
        const oa::Unit* record{unit_in(world, target)};
        if (record == nullptr)
            return;
        note_unit_type(recorder.timeline_, world, *record);
        TimelineEvent event{};
        event.tick = world.game.tick;
        event.kind = EventKind::damage;
        event.unit = target;
        event.owner = player_of(record->owner_index);
        event.unit_type = record->type_index;
        event.other_unit = source;
        if (const oa::Unit* attacker{unit_in(world, source)}; attacker != nullptr)
            event.other_owner = player_of(attacker->owner_index);
        event.amount = amount;
        event.damage_kind = kind;
        event.at = point_of(record->position);
        recorder.timeline_.events.push_back(event);
    } catch (...) {
        if (!recorder.failure_)
            recorder.failure_ = std::current_exception();
    }
}

void TimelineRecorder::unit_died(
    void* context,
    const oa::World& world,
    uint16_t unit,
    const KillOutcome& outcome,
    bool settled_elsewhere
) {
    auto& recorder{recorder_of(context)};
    try {
        const oa::Unit* record{unit_in(world, unit)};
        if (record == nullptr)
            return;
        note_unit_type(recorder.timeline_, world, *record);
        TimelineEvent event{};
        event.tick = world.game.tick;
        event.kind = EventKind::death;
        event.unit = unit;
        event.owner = player_of(record->owner_index);
        event.unit_type = record->type_index;
        event.at = point_of(record->position);
        const uint32_t attacker{record->last_attacker_id};
        if (attacker != 0 && attacker < world.unit_slot_count &&
            attacker <= std::numeric_limits<uint16_t>::max())
            event.other_unit = static_cast<uint16_t>(attacker);
        event.other_owner = player_of(record->last_attacker_owner);
        event.damage_kind = static_cast<uint8_t>(outcome.kind);
        if (oa::sim::match_runtime::side_commander(world, *record))
            event.flags = static_cast<uint8_t>(event.flags | event_flag::commander);
        if (settled_elsewhere)
            event.flags = static_cast<uint8_t>(event.flags | event_flag::settled_elsewhere);
        recorder.timeline_.events.push_back(event);
    } catch (...) {
        if (!recorder.failure_)
            recorder.failure_ = std::current_exception();
    }
}

} // namespace oa::media::director
