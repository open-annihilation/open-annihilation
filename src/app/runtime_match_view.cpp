// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The runtime's side of the match view (match_view.hpp). The entries copy
// units, orders, types, the map, the sight grids and the radar contacts out
// of the running match. They do not change it. apply_give_orders is the one
// write, and only when the launch asked for the save/load orders on a local
// skirmish.
#include "oa/app/match_view.hpp"

#include "oa/app/extension.hpp"
#include "oa/app/runtime.hpp"
#include "oa/formats/json.hpp"
#include "oa/core/feature_def.h"
#include "oa/core/map_plot.h"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/weapon_def.h"
#include "oa/core/world.h"
#include "oa/data/mission_types.hpp"
#include "oa/platform/log_files.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/present/world_renderer/world_radar.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/detection.hpp"
#include "oa/sim/match_runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

constexpr std::size_t kQueueCopy = 32;

/// Copies a fixed character field, stopping at the first NUL.
///
/// @param bytes the field
/// @param capacity its size, counting a NUL when it has one
/// @return the text
std::string field_text(const char* bytes, std::size_t capacity) {
    std::size_t length = 0;
    while (length < capacity && bytes[length] != '\0')
        ++length;
    return std::string(bytes, length);
}

/// Returns a mission kind's registered name.
///
/// @param kind the order kind
/// @return the name; empty when the kind is not registered or is the empty name
std::string mission_name(uint8_t kind) {
    const std::span<const std::string_view> names = oa::data::mission_types::registered_names();
    if (kind >= names.size())
        return {};
    return std::string(names[kind]);
}

/// Tells whether an order's first parameter is the type it builds.
///
/// @param name the mission's name
/// @return true for a building or mobile build
bool construction_kind(std::string_view name) {
    return name == "BuildingBuild" || name == "MobileBuild" || name == "VTOL_MobileBuild";
}

/// Returns how finished a unit is, as a percent from 0 to 100.
///
/// @param remaining 1 unfinished through 0 finished
/// @return the percent
int32_t build_percent(float remaining) {
    if (!std::isfinite(remaining))
        return 0;
    const double done = (1.0 - static_cast<double>(remaining)) * 100.0;
    const long rounded = std::lround(done);
    if (rounded < 0)
        return 0;
    if (rounded > 100)
        return 100;
    return static_cast<int32_t>(rounded);
}

/// The words the player sees for a type: its description, or its name.
///
/// @param def the type
/// @return the words
std::string display_name(const oa::UnitDef& def) {
    std::string description = field_text(def.description, sizeof def.description);
    if (!description.empty())
        return description;
    return field_text(def.name, sizeof def.name);
}

/// Returns a type's unit name when the index is one the match has.
///
/// @param world the match's world
/// @param parameter the order's first parameter
/// @return the unit name; empty when the parameter is not a type index
std::string type_at(const oa::World& world, int32_t parameter) {
    if (parameter <= 0 || world.unit_defs == nullptr)
        return {};
    const auto index = static_cast<uint32_t>(parameter);
    if (index >= world.unit_def_count)
        return {};
    return field_text(world.unit_defs[index].unit_name, sizeof world.unit_defs[index].unit_name);
}

/// Copies one order record.
///
/// @param world the match's world, for a build's product name
/// @param record the match's record
/// @return the copy
OrderRecord
order_from(const oa::World& world, const sim::match_runtime::Match::OrderRecordView& record) {
    OrderRecord copied;
    copied.kind = mission_name(record.kind);
    copied.target = record.target;
    copied.x = record.point[0] >> 16;
    copied.y = record.point[1] >> 16;
    copied.z = record.point[2] >> 16;
    copied.parameter_1 = record.parameter_1;
    copied.parameter_2 = record.parameter_2;
    copied.phase = record.phase;
    copied.issue_tick = record.issue_tick;
    copied.facing = record.facing;
    copied.build_started = record.build_started;
    copied.seen_x = record.seen_x;
    copied.seen_z = record.seen_z;
    if (construction_kind(copied.kind))
        copied.build = type_at(world, record.parameter_1);
    return copied;
}

/// Copies one queue, head first, and notes when the copy stopped at its limit.
///
/// @param match the running match
/// @param world the match's world
/// @param id the unit
/// @param secondary the secondary queue
/// @param[out] truncated set when the queue was longer than the copy
/// @return the orders
std::vector<OrderRecord> copy_queue(
    sim::match_runtime::Match& match,
    const oa::World& world,
    uint16_t id,
    bool secondary,
    bool& truncated
) {
    std::array<sim::match_runtime::Match::OrderRecordView, kQueueCopy> records{};
    const std::size_t written = match.queue_records(id, secondary, records.data(), records.size());
    truncated = written == records.size();
    std::vector<OrderRecord> copied;
    copied.reserve(written);
    for (std::size_t cursor = 0; cursor < written; ++cursor)
        copied.push_back(order_from(world, records[cursor]));
    return copied;
}

/// Returns the primary queue's first order, or Standby when that queue is empty.
///
/// @param match the running match
/// @param id the unit
/// @return the mission name, or Standby
std::string current_order(sim::match_runtime::Match& match, uint16_t id) {
    sim::match_runtime::Match::OrderRecordView head{};
    const std::size_t written = match.queue_records(id, false, &head, 1);
    if (written == 0)
        return "Standby";
    std::string name = mission_name(head.kind);
    if (name.empty())
        return "Standby";
    return name;
}

/// Appends a weapon's key, or its name when it has no key.
///
/// @param world the match's world
/// @param ref the weapon reference; 0 is none
/// @param[in,out] weapons the names gathered so far
void append_weapon(const oa::World& world, oa_ref32 ref, std::vector<std::string>& weapons) {
    const oa::WeaponDef* weapon = oa::world_weapon_def(&world, ref);
    if (weapon == nullptr)
        return;
    std::string key = field_text(weapon->key, sizeof weapon->key);
    if (key.empty())
        key = field_text(weapon->name, sizeof weapon->name);
    if (!key.empty())
        weapons.push_back(std::move(key));
}

/// Appends the unit names one builder type's build list names.
///
/// The lists were stored for the computer players when the match started.
/// Reading them does not create that state.
///
/// @param world the match's world
/// @param players the computer players' lists; null leaves the list empty
/// @param type_index the builder's type
/// @param[in,out] builds the names gathered so far
void append_builds(
    const oa::World& world,
    const sim::ai::ComputerPlayers* players,
    uint32_t type_index,
    std::vector<std::string>& builds
) {
    if (players == nullptr || players->build_list_runs == nullptr || world.unit_defs == nullptr)
        return;
    const uint16_t* run = players->build_list_runs;
    const uint16_t* const end = run + players->build_list_run_length;
    for (uint32_t cursor = 0; cursor < world.unit_def_count && run < end; ++cursor) {
        const uint32_t count = *run++;
        const auto present = static_cast<uint32_t>(end - run);
        const uint32_t length = count < present ? count : present;
        if (cursor == type_index) {
            for (uint32_t item = 0; item < length; ++item) {
                const uint16_t built = run[item];
                if (built == 0 || built >= world.unit_def_count)
                    continue;
                std::string built_name = field_text(
                    world.unit_defs[built].unit_name, sizeof world.unit_defs[built].unit_name
                );
                if (!built_name.empty())
                    builds.push_back(std::move(built_name));
            }
            return;
        }
        run += length;
    }
}

/// Returns the computer players already stored for the match.
///
/// @param match the running match
/// @return the players, or null when the match has not stored them
const sim::ai::ComputerPlayers* stored_computer_players(sim::match_runtime::Match& match) {
    const std::shared_ptr<void>& slot = match.computer_player_state();
    if (!slot)
        return nullptr;
    return static_cast<const sim::ai::ComputerPlayers*>(slot.get());
}

/// Tells whether the match has a live unit of one player.
///
/// @param match the running match
/// @param player the player
/// @return true when it has
bool local_unit_is_live(const sim::match_runtime::Match& match, uint8_t player) {
    const oa::World& world = match.state();
    if (world.units == nullptr)
        return false;
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const oa::Unit& unit = world.units[slot];
        if (unit.type_index == 0 || (unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        if (unit.owner_index == player)
            return true;
    }
    return false;
}

/// The most cells one grid may hold.
constexpr int64_t kGridLimit = 512LL * 512LL;

/// The most features map info copies.
constexpr std::size_t kFeatureLimit = 2500;

/// One sight cell, in map pixels. The minimap's fog is drawn from these cells.
constexpr int32_t kSightCell = 32;

/// One plot, in map pixels.
constexpr int32_t kPlotPixels = 16;

/// Tells whether a grid of that size is small enough to copy.
///
/// @param across cells across
/// @param down cells down
/// @return true when the grid is within the limit
bool grid_fits(int32_t across, int32_t down) {
    if (across <= 0 || down <= 0)
        return false;
    return static_cast<int64_t>(across) * static_cast<int64_t>(down) <= kGridLimit;
}

/// The rules the minimap uses for one player's fog.
///
/// @param line_of_sight whether line of sight is on
/// @param mapping whether unexplored ground stays hidden
/// @param shared_mapped the allies whose explored ground this player is shown
/// @return the rules
oa::present::world_renderer::FogOptions
fog_rules(bool line_of_sight, bool mapping, uint16_t shared_mapped) {
    oa::present::world_renderer::FogOptions rules;
    rules.line_of_sight = line_of_sight;
    rules.mapping = mapping;
    rules.shared_mapped = shared_mapped;
    return rules;
}

/// Fills the sight and explored cells the minimap's fog is drawn from.
///
/// @param match the running match
/// @param player the player
/// @param rules the minimap's rules for that player
/// @param[out] explored 1 where the minimap shows the ground as explored
/// @param[out] in_sight 1 where the minimap shows the ground in sight
void native_fog(
    sim::match_runtime::Match& match,
    uint8_t player,
    oa::present::world_renderer::FogOptions rules,
    std::vector<uint8_t>& explored,
    std::vector<uint8_t>& in_sight
) {
    namespace wr = oa::present::world_renderer;
    const sim::visibility_state::PlayerSightGrid& sight = match.sight();
    const int32_t across = sight.width;
    const int32_t down = sight.height;
    const auto cells =
        static_cast<std::size_t>(std::max(across, 0)) * static_cast<std::size_t>(std::max(down, 0));
    const bool greys = wr::view_greys(rules);
    explored.assign(cells, rules.mapping ? uint8_t{0} : uint8_t{1});
    in_sight.assign(cells, greys ? uint8_t{0} : uint8_t{1});
    if ((!rules.mapping && !rules.line_of_sight) || across <= 0 || down <= 0)
        return;
    const auto own_bit = static_cast<uint16_t>(1U << (player & 0x1fU));
    const auto shown_bit = static_cast<uint16_t>(own_bit | rules.shared_mapped);
    const std::span<const uint16_t> coverage =
        rules.line_of_sight ? match.player_coverage(player) : std::span<const uint16_t>{};
    for (std::size_t index = 0; index < cells; ++index) {
        const uint16_t mapped =
            index < sight.player_bits.size() ? sight.player_bits[index] : uint16_t{0};
        if (rules.mapping && (mapped & shown_bit) != 0)
            explored[index] = 1;
        if (greys &&
            wr::cell_shown_in_sight(
                index < coverage.size() ? coverage[index] : uint16_t{0}, mapped, own_bit, rules
            ))
            in_sight[index] = 1;
    }
}

/// The minimap's fog for one layer, at the sight cells.
///
/// @param match the running match
/// @param player the player
/// @param rules the minimap's rules
/// @param layer sight or explored
/// @param[out] reading 1 where that layer is on, one per sight cell
void minimap_reading(
    sim::match_runtime::Match& match,
    uint8_t player,
    oa::present::world_renderer::FogOptions rules,
    VisibilityLayer layer,
    std::vector<uint8_t>& reading
) {
    namespace wr = oa::present::world_renderer;
    const sim::visibility_state::PlayerSightGrid& sight = match.sight();
    const int32_t across = std::max(sight.width, 0);
    const int32_t down = std::max(sight.height, 0);
    std::vector<uint16_t> explored_bits;
    std::vector<uint8_t> covered;
    const std::span<const uint16_t> coverage =
        rules.line_of_sight ? match.player_coverage(player) : std::span<const uint16_t>{};
    wr::radar_read_sight(sight, player, coverage, rules, 1, across, down, explored_bits, covered);
    const std::size_t cells = static_cast<std::size_t>(across) * static_cast<std::size_t>(down);
    reading.assign(cells, 0);
    for (std::size_t index = 0; index < cells; ++index) {
        if (layer == VisibilityLayer::explored)
            reading[index] = index < explored_bits.size() && explored_bits[index] != 0 ? 1 : 0;
        else
            reading[index] = index < covered.size() && covered[index] != 0 ? 1 : 0;
    }
}

/// Groups sight cells into the cell size a caller asked for.
///
/// A grouped cell is on when any sight cell it covers is on. A cell of 32
/// is the sight grid unchanged.
///
/// @param native one byte per sight cell, row by row
/// @param across sight cells across
/// @param down sight cells down
/// @param cell the caller's cell size, in map pixels
/// @param[out] grouped the grouped cells
/// @param[out] grouped_across cells across
/// @param[out] grouped_down cells down
/// @return false when the grouped grid is too large
bool group_cells(
    const std::vector<uint8_t>& native,
    int32_t across,
    int32_t down,
    int32_t cell,
    std::vector<uint8_t>& grouped,
    int32_t& grouped_across,
    int32_t& grouped_down
) {
    if (cell == kSightCell) {
        grouped_across = across;
        grouped_down = down;
        grouped = native;
        return grid_fits(grouped_across, grouped_down);
    }
    const int32_t span_x = across * kSightCell;
    const int32_t span_z = down * kSightCell;
    grouped_across = (span_x + cell - 1) / cell;
    grouped_down = (span_z + cell - 1) / cell;
    if (!grid_fits(grouped_across, grouped_down))
        return false;
    grouped.assign(
        static_cast<std::size_t>(grouped_across) * static_cast<std::size_t>(grouped_down), 0
    );
    for (int32_t row = 0; row < grouped_down; ++row) {
        const int32_t z0 = row * cell;
        const int32_t z1 = std::min(z0 + cell, span_z);
        const int32_t from_z = z0 / kSightCell;
        const int32_t to_z = (z1 - 1) / kSightCell;
        for (int32_t column = 0; column < grouped_across; ++column) {
            const int32_t x0 = column * cell;
            const int32_t x1 = std::min(x0 + cell, span_x);
            const int32_t from_x = x0 / kSightCell;
            const int32_t to_x = (x1 - 1) / kSightCell;
            uint8_t on = 0;
            for (int32_t sight_z = from_z; sight_z <= to_z && on == 0; ++sight_z) {
                for (int32_t sight_x = from_x; sight_x <= to_x; ++sight_x) {
                    const auto at =
                        static_cast<std::size_t>(sight_z) * static_cast<std::size_t>(across) +
                        static_cast<std::size_t>(sight_x);
                    if (at < native.size() && native[at] != 0) {
                        on = 1;
                        break;
                    }
                }
            }
            grouped
                [static_cast<std::size_t>(row) * static_cast<std::size_t>(grouped_across) +
                 static_cast<std::size_t>(column)] = on;
        }
    }
    return true;
}

/// The picture byte of one unit for a player.
///
/// The viewpoint's picture is the contact marks on the unit. Another
/// player's is the picture the match keeps, or zero when it keeps none.
///
/// @param match the running match
/// @param world the match's world
/// @param player the player
/// @param slot the unit's slot
/// @param unit the unit
/// @return the picture byte
uint8_t picture_byte(
    sim::match_runtime::Match& match,
    const oa::World& world,
    uint8_t player,
    uint32_t slot,
    const oa::Unit& unit
) {
    if (player == world.game.viewpoint_player) {
        uint8_t bits = 0;
        if ((unit.flags & oa::sim::detection::radar_contact) != 0)
            bits = static_cast<uint8_t>(bits | oa::sim::detection::picture_radar);
        if ((unit.flags & oa::sim::detection::sonar_contact) != 0)
            bits = static_cast<uint8_t>(bits | oa::sim::detection::picture_sonar);
        if ((unit.flags & oa::sim::detection::jammed) != 0)
            bits = static_cast<uint8_t>(bits | oa::sim::detection::picture_jammed);
        return bits;
    }
    const std::span<const uint8_t> kept = match.player_contact_bits(player);
    if (slot >= kept.size())
        return 0;
    return kept[slot];
}

/// Marks the sight cell a map pixel lies in.
///
/// @param pixels the map pixel
/// @param limit the sight cells along that axis
/// @return the cell, clamped onto the grid
int32_t sight_cell_of(int32_t pixels, int32_t limit) {
    if (limit <= 0)
        return 0;
    if (pixels < 0)
        pixels = 0;
    int32_t cell = pixels / kSightCell;
    if (cell >= limit)
        cell = limit - 1;
    return cell;
}

/// The sight cells that hold a radar contact, a sonar contact or a jammed unit.
///
/// @param match the running match
/// @param player the player
/// @param layer radar, sonar or jammed
/// @param across sight cells across
/// @param down sight cells down
/// @return one byte per sight cell, 1 where the layer is on
std::vector<uint8_t> contact_cells(
    sim::match_runtime::Match& match,
    uint8_t player,
    VisibilityLayer layer,
    int32_t across,
    int32_t down
) {
    std::vector<uint8_t> cells(
        static_cast<std::size_t>(across) * static_cast<std::size_t>(down), 0
    );
    const oa::World& world = match.state();
    if (world.units == nullptr)
        return cells;
    const bool viewpoint = player == world.game.viewpoint_player;
    if (!viewpoint && !match.player_contacts_kept(player))
        return cells;
    const uint8_t wanted = layer == VisibilityLayer::sonar    ? oa::sim::detection::picture_sonar
                           : layer == VisibilityLayer::jammed ? oa::sim::detection::picture_jammed
                                                              : oa::sim::detection::picture_radar;
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const oa::Unit& unit = world.units[slot];
        if (unit.type_index == 0 || (unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        if (layer != VisibilityLayer::jammed &&
            (unit.owner_index >= OA_PLAYER_COUNT || match.allied(player, unit.owner_index)))
            continue;
        const uint8_t bits = picture_byte(match, world, player, slot, unit);
        if ((bits & wanted) == 0)
            continue;
        const int32_t column = sight_cell_of(unit.position.x >> 16, across);
        const int32_t row = sight_cell_of(unit.position.z >> 16, down);
        cells
            [static_cast<std::size_t>(row) * static_cast<std::size_t>(across) +
             static_cast<std::size_t>(column)] = 1;
    }
    return cells;
}

} // namespace

// The runtime's side of the match view: the one friend through which its
// entries read the runtime.
struct MatchViewAccess {
    /// Returns the runtime a match view's context names.
    ///
    /// @param context MatchView::context
    /// @return the runtime
    static Runtime& game(void* context) { return *static_cast<Runtime*>(context); }

    /// Tells whether a match is running (MatchView::active).
    ///
    /// @param context MatchView::context
    /// @return true while a match is running
    static bool active(void* context) { return game(context).match_ != nullptr; }

    /// Returns the match's tick (MatchView::tick).
    ///
    /// @param context MatchView::context
    /// @return the tick, or 0 while no match is running
    static uint32_t tick(void* context) {
        const std::unique_ptr<sim::match_runtime::Match>& match = game(context).match_;
        if (!match)
            return 0;
        return match->state().game.tick;
    }

    /// Returns the local player (MatchView::viewer).
    ///
    /// @param context MatchView::context
    /// @return the player
    static uint8_t viewer(void* context) { return game(context).match_local_player_; }

    /// Tells whether full awareness is allowed (MatchView::full_awareness_allowed).
    ///
    /// @param context MatchView::context
    /// @return false for a shared match
    static bool full_awareness_allowed(void* context) {
        const uint32_t bits = game(context).current_extension_state();
        return (bits & extension_state::shared_match) == 0;
    }

    /// Copies every live unit (MatchView::units).
    ///
    /// @param context MatchView::context
    /// @param[out] listed replaced by the live units
    static void units(void* context, std::vector<UnitRecord>* listed) {
        listed->clear();
        Runtime& runtime = game(context);
        if (!runtime.match_)
            return;
        sim::match_runtime::Match& match = *runtime.match_;
        const oa::World& world = match.state();
        if (world.units == nullptr)
            return;
        const uint8_t player = runtime.match_local_player_;
        for (uint32_t slot = 1; slot < world.unit_slot_count && slot <= 0xffffu; ++slot) {
            const oa::Unit& unit = world.units[slot];
            if (unit.type_index == 0 || (unit.flags & OA_UNIT_FLAG_LIVE) == 0)
                continue;
            UnitRecord record;
            record.id = unit.id != 0 ? unit.id : static_cast<uint16_t>(slot);
            if (world.unit_defs != nullptr && unit.type_index < world.unit_def_count) {
                const oa::UnitDef& def = world.unit_defs[unit.type_index];
                record.type = field_text(def.unit_name, sizeof def.unit_name);
                record.name = display_name(def);
                record.side = field_text(def.side, sizeof def.side);
            }
            record.owner = unit.owner_index;
            record.x = unit.position.x >> 16;
            record.y = unit.position.y >> 16;
            record.z = unit.position.z >> 16;
            record.health = unit.health;
            record.build = build_percent(unit.build_remaining);
            record.selected = (unit.flags & OA_UNIT_FLAG_SELECTED) != 0;
            record.radar = (unit.flags & OA_UNIT_FLAG_RADAR_CONTACT) != 0;
            record.cloaked = (unit.state_flags & OA_UNIT_STATE_CLOAKED) != 0;
            record.shown = match.unit_shown(player, static_cast<uint16_t>(slot));
            record.order = current_order(match, static_cast<uint16_t>(slot));
            listed->push_back(std::move(record));
        }
    }

    /// Copies one live unit's queues (MatchView::orders).
    ///
    /// @param context MatchView::context
    /// @param id the unit's slot
    /// @param[out] queues replaced by the two queues
    /// @return false when id is not a live unit
    static bool orders(void* context, uint16_t id, OrderQueues* queues) {
        *queues = {};
        Runtime& runtime = game(context);
        if (!runtime.match_ || id == 0)
            return false;
        sim::match_runtime::Match& match = *runtime.match_;
        const oa::World& world = match.state();
        if (world.units == nullptr || id >= world.unit_slot_count)
            return false;
        const oa::Unit& unit = world.units[id];
        if (unit.type_index == 0 || (unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            return false;
        queues->primary = copy_queue(match, world, id, false, queues->primary_truncated);
        queues->secondary = copy_queue(match, world, id, true, queues->secondary_truncated);
        return true;
    }

    /// Copies every loaded type (MatchView::types).
    ///
    /// @param context MatchView::context
    /// @param[out] catalog replaced by the types
    static void types(void* context, std::vector<TypeRecord>* catalog) {
        catalog->clear();
        Runtime& runtime = game(context);
        if (!runtime.match_)
            return;
        sim::match_runtime::Match& match = *runtime.match_;
        const oa::World& world = match.state();
        if (world.unit_defs == nullptr)
            return;
        const sim::ai::ComputerPlayers* players = stored_computer_players(match);
        for (uint32_t slot = 1; slot < world.unit_def_count && slot <= 0xffffu; ++slot) {
            const oa::UnitDef& def = world.unit_defs[slot];
            TypeRecord record;
            record.index = static_cast<uint16_t>(slot);
            record.type = field_text(def.unit_name, sizeof def.unit_name);
            record.name = display_name(def);
            record.description = field_text(def.description, sizeof def.description);
            record.side = field_text(def.side, sizeof def.side);
            if (record.type.empty() && record.name.empty() && record.description.empty())
                continue;
            if ((def.flags & OA_UNIT_DEF_FLAG_BUILDER) != 0)
                append_builds(world, players, slot, record.builds);
            append_weapon(world, def.weapon1, record.weapons);
            append_weapon(world, def.weapon2, record.weapons);
            append_weapon(world, def.weapon3, record.weapons);
            record.speed = def.max_velocity;
            record.sight = def.sight_distance;
            record.radar = def.radar_distance;
            record.sonar = def.sonar_distance;
            catalog->push_back(std::move(record));
        }
    }

    /// Copies the map (MatchView::map_info).
    ///
    /// @param context MatchView::context
    /// @param cell the cell size, in map pixels
    /// @param[out] info replaced by the map
    /// @return false when the map cannot be read at that cell
    static bool map_info(void* context, int32_t cell, MapInfo* info) {
        *info = {};
        Runtime& runtime = game(context);
        if (!runtime.match_ || cell < 1)
            return false;
        sim::match_runtime::Match& match = *runtime.match_;
        const oa::World& world = match.state();
        const oa::Game& state = world.game;
        info->name = runtime.map_display_name();
        if (info->name.empty())
            info->name = runtime.selected_map_name_runtime_;
        info->width = state.map_pixel_width;
        info->height = state.map_pixel_height;
        info->plots_x = state.map_width;
        info->plots_z = state.map_height;
        info->sea = state.sea_level;
        info->cell = cell;
        if (info->width <= 0 || info->height <= 0 || info->plots_x <= 0 || info->plots_z <= 0)
            return false;
        info->cells_x = (info->width + cell - 1) / cell;
        info->cells_z = (info->height + cell - 1) / cell;
        if (!grid_fits(info->cells_x, info->cells_z))
            return false;
        const auto cells =
            static_cast<std::size_t>(info->cells_x) * static_cast<std::size_t>(info->cells_z);
        info->heights.assign(cells, 0);
        if (world.plots == nullptr)
            return true;
        for (int32_t row = 0; row < info->cells_z; ++row) {
            const int32_t z0 = row * cell;
            const int32_t z1 = std::min(z0 + cell, info->height);
            int32_t mid_z = z0 + cell / 2;
            if (mid_z >= info->height)
                mid_z = info->height - 1;
            const int32_t plot_z = std::min(mid_z / kPlotPixels, info->plots_z - 1);
            const int32_t from_z = std::min(z0 / kPlotPixels, info->plots_z - 1);
            const int32_t to_z = std::min((z1 - 1) / kPlotPixels, info->plots_z - 1);
            for (int32_t column = 0; column < info->cells_x; ++column) {
                const int32_t x0 = column * cell;
                const int32_t x1 = std::min(x0 + cell, info->width);
                int32_t mid_x = x0 + cell / 2;
                if (mid_x >= info->width)
                    mid_x = info->width - 1;
                const int32_t plot_x = std::min(mid_x / kPlotPixels, info->plots_x - 1);
                const oa::MapPlot* centre = oa::world_plot(&world, plot_x, plot_z);
                const auto at =
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(info->cells_x) +
                    static_cast<std::size_t>(column);
                if (centre != nullptr)
                    info->heights[at] = centre->height;
                const int32_t from_x = std::min(x0 / kPlotPixels, info->plots_x - 1);
                const int32_t to_x = std::min((x1 - 1) / kPlotPixels, info->plots_x - 1);
                int32_t amount = 0;
                for (int32_t metal_z = from_z; metal_z <= to_z; ++metal_z) {
                    for (int32_t metal_x = from_x; metal_x <= to_x; ++metal_x) {
                        const oa::MapPlot* plot = oa::world_plot(&world, metal_x, metal_z);
                        if (plot != nullptr)
                            amount += plot->metal;
                    }
                }
                if (amount > 0)
                    info->metal.push_back(MetalSpot{column, row, amount});
            }
        }
        info->features.reserve(std::min(kFeatureLimit, cells));
        for (int32_t plot_z = 0; plot_z < info->plots_z; ++plot_z) {
            for (int32_t plot_x = 0; plot_x < info->plots_x; ++plot_x) {
                const oa::MapPlot* plot = oa::world_plot(&world, plot_x, plot_z);
                if (plot == nullptr || plot->feature >= OA_PLOT_FEATURE_RESERVED)
                    continue;
                if (info->features.size() >= kFeatureLimit) {
                    info->features_truncated = true;
                    return true;
                }
                MapFeature feature;
                feature.x = plot_x * kPlotPixels;
                feature.z = plot_z * kPlotPixels;
                feature.footprint_x = 1;
                feature.footprint_z = 1;
                if (world.feature_defs != nullptr && plot->feature < world.feature_def_count) {
                    const oa::FeatureDef& def = world.feature_defs[plot->feature];
                    feature.name = field_text(def.name, sizeof def.name);
                    if (def.footprint_x > 0)
                        feature.footprint_x = def.footprint_x;
                    if (def.footprint_z > 0)
                        feature.footprint_z = def.footprint_z;
                }
                info->features.push_back(std::move(feature));
            }
        }
        return true;
    }

    /// Copies one visibility grid (MatchView::visibility).
    ///
    /// @param context MatchView::context
    /// @param player the player
    /// @param layer which grid
    /// @param cell the cell size, in map pixels
    /// @param[out] grid replaced by the grid
    /// @return false when it cannot be read
    static bool visibility(
        void* context, uint8_t player, VisibilityLayer layer, int32_t cell, VisibilityGrid* grid
    ) {
        *grid = {};
        Runtime& runtime = game(context);
        if (!runtime.match_ || cell < 1 || player >= OA_PLAYER_COUNT)
            return false;
        sim::match_runtime::Match& match = *runtime.match_;
        uint16_t shared_mapped = 0;
        if (match.rules().intel.allied_los_sharing.enabled)
            shared_mapped =
                oa::present::world_renderer::allied_mapped_players(match.state().game, player);
        const oa::present::world_renderer::FogOptions rules =
            fog_rules(runtime.match_line_of_sight_on(), runtime.match_mapping_on(), shared_mapped);
        const sim::visibility_state::PlayerSightGrid& sight = match.sight();
        if (sight.width <= 0 || sight.height <= 0)
            return false;
        std::vector<uint8_t> explored;
        std::vector<uint8_t> in_sight;
        native_fog(match, player, rules, explored, in_sight);
        std::vector<uint8_t> native;
        if (layer == VisibilityLayer::sight)
            native = std::move(in_sight);
        else if (layer == VisibilityLayer::explored)
            native = std::move(explored);
        else
            native = contact_cells(match, player, layer, sight.width, sight.height);
        if (!group_cells(
                native, sight.width, sight.height, cell, grid->cells, grid->width, grid->height
            ))
            return false;
        grid->cell = cell;
        if (layer == VisibilityLayer::sight || layer == VisibilityLayer::explored) {
            std::vector<uint8_t> reading;
            minimap_reading(match, player, rules, layer, reading);
            int32_t fog_across = 0;
            int32_t fog_down = 0;
            if (!group_cells(
                    reading, sight.width, sight.height, cell, grid->minimap, fog_across, fog_down
                ))
                grid->minimap.clear();
            else if (
                fog_across != grid->width || fog_down != grid->height ||
                grid->minimap.size() != grid->cells.size()
            )
                grid->minimap.clear();
        }
        return true;
    }

    /// Copies the radar and sonar contacts (MatchView::contacts).
    ///
    /// @param context MatchView::context
    /// @param player the player
    /// @param[out] listed replaced by the contacts
    /// @param[out] kept set when that player's picture is kept
    /// @return false when no match is running
    static bool
    contacts(void* context, uint8_t player, std::vector<RadarContact>* listed, bool* kept) {
        listed->clear();
        *kept = false;
        Runtime& runtime = game(context);
        if (!runtime.match_ || player >= OA_PLAYER_COUNT)
            return false;
        sim::match_runtime::Match& match = *runtime.match_;
        const oa::World& world = match.state();
        if (world.units == nullptr)
            return true;
        const bool viewpoint = player == world.game.viewpoint_player;
        *kept = viewpoint || match.player_contacts_kept(player);
        if (!*kept)
            return true;
        for (uint32_t slot = 1; slot < world.unit_slot_count && slot <= 0xffffu; ++slot) {
            const oa::Unit& unit = world.units[slot];
            if (unit.type_index == 0 || (unit.flags & OA_UNIT_FLAG_LIVE) == 0)
                continue;
            if (unit.owner_index >= OA_PLAYER_COUNT || match.allied(player, unit.owner_index))
                continue;
            const uint8_t bits = picture_byte(match, world, player, slot, unit);
            const bool radar = (bits & oa::sim::detection::picture_radar) != 0;
            const bool sonar = (bits & oa::sim::detection::picture_sonar) != 0;
            if (!radar && !sonar)
                continue;
            RadarContact contact;
            contact.x = unit.position.x >> 16;
            contact.y = unit.position.y >> 16;
            contact.z = unit.position.z >> 16;
            contact.radar = radar;
            contact.sonar = sonar;
            listed->push_back(contact);
        }
        std::sort(
            listed->begin(), listed->end(), [](const RadarContact& one, const RadarContact& other) {
                if (one.x != other.x)
                    return one.x < other.x;
                if (one.z != other.z)
                    return one.z < other.z;
                return one.y < other.y;
            }
        );
        return true;
    }

    /// Gives the save/load orders once (apply_give_orders).
    ///
    /// @param runtime the running game
    static void give_orders(Runtime& runtime) {
        if (!runtime.options_.give_orders || runtime.campaign_mission_)
            return;
        const uint32_t bits = runtime.current_extension_state();
        if ((bits & (extension_state::shared_match | extension_state::replay)) != 0)
            return;
        // The match this call has already tried. A null match clears it, so
        // the next match is tried even when it reuses the address.
        static sim::match_runtime::Match* attempted = nullptr;
        sim::match_runtime::Match* running = runtime.match_.get();
        if (running == nullptr) {
            attempted = nullptr;
            return;
        }
        if (running == attempted || !local_unit_is_live(*running, runtime.match_local_player_))
            return;
        attempted = running;
        try {
            runtime.give_saveload_orders();
        } catch (const std::exception& failure) {
            oa::platform::log_files::write_line(std::string("mcp: give-orders: ") + failure.what());
        } catch (...) {
            oa::platform::log_files::write_line("mcp: give-orders failed");
        }
    }

    /// The running match, or null when none is.
    ///
    /// @param runtime the running game
    /// @return the match
    static sim::match_runtime::Match* match_of(Runtime& runtime) { return runtime.match_.get(); }

    /// The player this machine plays.
    ///
    /// @param runtime the running game
    /// @return the player, 0 to 9
    static uint8_t local_of(Runtime& runtime) { return runtime.match_local_player_; }
};

namespace {

namespace json = oa::formats::json;
namespace hooks = oa::sim::match_runtime;

/// A health event that gives health back, which is not a unit being hurt.
constexpr uint8_t kHealingKind = 10;

/// The notes waiting to be handed on. Older ones past the keep are dropped.
struct PendingWatch {
    Runtime* runtime{};
    hooks::Match* match{};
    hooks::EventListenerId listener{};
    bool listen_failed{};
    std::deque<MatchNote> pending;
    std::vector<uint8_t> radar;
};

/// The one watch. It outlives a match, and the next match starts clean.
///
/// @return the watch
PendingWatch& pending_watch() {
    static PendingWatch held;
    return held;
}

/// Keeps one note, dropping the oldest when the keep is full.
///
/// @param tick the match's tick
/// @param kind the note's kind
/// @param fill writes the note's own members into the open object
template <typename Fill>
void remember(uint32_t tick, std::string_view kind, Fill fill) {
    try {
        json::JsonWriter writer;
        writer.begin_object();
        writer.key("event");
        writer.string(kind);
        writer.key("tick");
        writer.integer(static_cast<int64_t>(tick));
        fill(writer);
        writer.end_object();
        PendingWatch& held = pending_watch();
        if (held.pending.size() == match_note_keep)
            held.pending.pop_front();
        MatchNote note;
        note.tick = tick;
        note.kind = std::string(kind);
        note.json = writer.text();
        held.pending.push_back(std::move(note));
    } catch (const std::exception&) {
        // A note that cannot be built is dropped. The match is not changed.
    }
}

/// The id the unit tools report: the unit's own index, or the slot.
///
/// @param unit the unit
/// @param slot the slot
/// @return the id
uint16_t unit_number(const oa::Unit& unit, uint16_t slot) {
    return unit.id != 0 ? unit.id : slot;
}

/// Writes a unit's place in map pixels.
///
/// @param writer the open object
/// @param unit the unit
void write_place(json::JsonWriter& writer, const oa::Unit& unit) {
    writer.key("x");
    writer.integer(static_cast<int64_t>(unit.position.x >> 16));
    writer.key("y");
    writer.integer(static_cast<int64_t>(unit.position.y >> 16));
    writer.key("z");
    writer.integer(static_cast<int64_t>(unit.position.z >> 16));
}

/// The local player the watch reports for, or nothing when it cannot.
///
/// @param[out] player the local player
/// @return false when no match is being watched
bool local_player(uint8_t& player) {
    const PendingWatch& held = pending_watch();
    if (held.runtime == nullptr || MatchViewAccess::match_of(*held.runtime) == nullptr)
        return false;
    player = MatchViewAccess::local_of(*held.runtime);
    return true;
}

/// Tells whether a unit is the local player's or one that player is shown.
///
/// @param match the match
/// @param world the match's world
/// @param slot the unit's slot
/// @param unit the unit
/// @return true when the note may be kept
bool reportable(hooks::Match& match, const oa::World& world, uint16_t slot, const oa::Unit& unit) {
    uint8_t player = 0;
    if (!local_player(player))
        return false;
    if (unit.owner_index == player)
        return true;
    return match.unit_shown(player, slot) && world.units != nullptr;
}

/// A live unit at a slot, or null.
///
/// @param world the match's world
/// @param slot the slot
/// @return the unit, or null
const oa::Unit* live_at(const oa::World& world, uint16_t slot) {
    if (world.units == nullptr || slot == 0 || slot >= world.unit_slot_count)
        return nullptr;
    const oa::Unit& unit = world.units[slot];
    if (unit.type_index == 0 || (unit.flags & OA_UNIT_FLAG_LIVE) == 0)
        return nullptr;
    return &unit;
}

/// The running match's type name for a unit.
///
/// @param world the match's world
/// @param unit the unit
/// @return the unit name; empty when the type is not loaded
std::string unit_type_name(const oa::World& world, const oa::Unit& unit) {
    if (world.unit_defs == nullptr || unit.type_index >= world.unit_def_count)
        return {};
    return field_text(
        world.unit_defs[unit.type_index].unit_name,
        sizeof world.unit_defs[unit.type_index].unit_name
    );
}

/// Notes a unit the local player owns or is shown (EventHooks::unit_created).
void on_unit_created(void* /*context*/, const oa::World& world, uint16_t slot) {
    const PendingWatch& held = pending_watch();
    if (held.runtime == nullptr || MatchViewAccess::match_of(*held.runtime) == nullptr)
        return;
    const oa::Unit* unit = live_at(world, slot);
    if (unit == nullptr ||
        !reportable(*MatchViewAccess::match_of(*held.runtime), world, slot, *unit))
        return;
    const std::string type_name = unit_type_name(world, *unit);
    const uint16_t number = unit_number(*unit, slot);
    const uint8_t owner = unit->owner_index;
    remember(world.game.tick, "unit_created", [&](json::JsonWriter& writer) {
        writer.key("unit");
        writer.integer(number);
        writer.key("type");
        writer.string(type_name);
        writer.key("owner");
        writer.integer(owner);
        write_place(writer, *unit);
    });
}

/// Notes a finished unit (EventHooks::unit_finished).
void on_unit_finished(void* /*context*/, const oa::World& world, uint16_t slot, uint16_t builder) {
    const PendingWatch& held = pending_watch();
    if (held.runtime == nullptr || MatchViewAccess::match_of(*held.runtime) == nullptr)
        return;
    const oa::Unit* unit = live_at(world, slot);
    if (unit == nullptr ||
        !reportable(*MatchViewAccess::match_of(*held.runtime), world, slot, *unit))
        return;
    const uint16_t number = unit_number(*unit, slot);
    const uint8_t owner = unit->owner_index;
    uint16_t builder_number = builder;
    if (const oa::Unit* made_by = live_at(world, builder))
        builder_number = unit_number(*made_by, builder);
    remember(world.game.tick, "build_done", [&](json::JsonWriter& writer) {
        writer.key("unit");
        writer.integer(number);
        writer.key("owner");
        writer.integer(owner);
        writer.key("builder");
        writer.integer(builder_number);
    });
}

/// Notes a unit that was hurt (EventHooks::unit_damaged). Healing is not a note.
void on_unit_damaged(
    void* /*context*/,
    const oa::World& world,
    uint16_t slot,
    uint16_t source,
    int16_t amount,
    uint8_t damage_kind
) {
    if (damage_kind == kHealingKind || amount <= 0)
        return;
    const PendingWatch& held = pending_watch();
    if (held.runtime == nullptr || MatchViewAccess::match_of(*held.runtime) == nullptr)
        return;
    const oa::Unit* unit = live_at(world, slot);
    if (unit == nullptr ||
        !reportable(*MatchViewAccess::match_of(*held.runtime), world, slot, *unit))
        return;
    const uint16_t number = unit_number(*unit, slot);
    const uint8_t owner = unit->owner_index;
    const int32_t health = unit->health;
    uint16_t source_number = source;
    if (const oa::Unit* from = live_at(world, source))
        source_number = unit_number(*from, source);
    remember(world.game.tick, "unit_damaged", [&](json::JsonWriter& writer) {
        writer.key("unit");
        writer.integer(number);
        writer.key("owner");
        writer.integer(owner);
        writer.key("amount");
        writer.integer(static_cast<int64_t>(amount));
        writer.key("source");
        writer.integer(source_number);
        writer.key("health");
        writer.integer(static_cast<int64_t>(health));
    });
}

/// Notes a unit that died (EventHooks::unit_died).
void on_unit_died(
    void* /*context*/,
    const oa::World& world,
    uint16_t slot,
    const hooks::KillOutcome& /*outcome*/,
    bool /*settled_elsewhere*/
) {
    const PendingWatch& held = pending_watch();
    if (held.runtime == nullptr || MatchViewAccess::match_of(*held.runtime) == nullptr)
        return;
    if (world.units == nullptr || slot == 0 || slot >= world.unit_slot_count)
        return;
    const oa::Unit& unit = world.units[slot];
    if (!reportable(*MatchViewAccess::match_of(*held.runtime), world, slot, unit))
        return;
    const std::string type_name = unit_type_name(world, unit);
    const uint16_t number = unit_number(unit, slot);
    const uint8_t owner = unit.owner_index;
    remember(world.game.tick, "unit_destroyed", [&](json::JsonWriter& writer) {
        writer.key("unit");
        writer.integer(number);
        writer.key("type");
        writer.string(type_name);
        writer.key("owner");
        writer.integer(owner);
        write_place(writer, unit);
    });
}

/// Notes a unit that ran out of orders (EventHooks::unit_idle).
void on_unit_idle(void* /*context*/, const oa::World& world, uint16_t slot) {
    const PendingWatch& held = pending_watch();
    if (held.runtime == nullptr || MatchViewAccess::match_of(*held.runtime) == nullptr)
        return;
    const oa::Unit* unit = live_at(world, slot);
    if (unit == nullptr ||
        !reportable(*MatchViewAccess::match_of(*held.runtime), world, slot, *unit))
        return;
    const uint16_t number = unit_number(*unit, slot);
    const uint8_t owner = unit->owner_index;
    remember(world.game.tick, "unit_idle", [&](json::JsonWriter& writer) {
        writer.key("unit");
        writer.integer(number);
        writer.key("owner");
        writer.integer(owner);
    });
}

/// Notes an order the handler finished (EventHooks::order_ended). A failed order is not a note.
void on_order_ended(
    void* /*context*/, const oa::World& world, uint16_t slot, uint8_t order_kind, bool done
) {
    if (!done)
        return;
    const PendingWatch& held = pending_watch();
    if (held.runtime == nullptr || MatchViewAccess::match_of(*held.runtime) == nullptr)
        return;
    const oa::Unit* unit = live_at(world, slot);
    if (unit == nullptr ||
        !reportable(*MatchViewAccess::match_of(*held.runtime), world, slot, *unit))
        return;
    const uint16_t number = unit_number(*unit, slot);
    const uint8_t owner = unit->owner_index;
    const std::string order_name = mission_name(order_kind);
    remember(world.game.tick, "order_done", [&](json::JsonWriter& writer) {
        writer.key("unit");
        writer.integer(number);
        writer.key("owner");
        writer.integer(owner);
        writer.key("order");
        writer.string(order_name);
    });
}

/// Notes enemies the local player gained sight of (EventHooks::seen_changed).
void on_seen_changed(
    void* /*context*/,
    const oa::World& world,
    uint8_t player,
    std::span<const oa::sim::detection::SightedUnit> gained,
    std::span<const oa::sim::detection::SightedUnit> /*lost*/
) {
    uint8_t local = 0;
    if (!local_player(local) || player != local)
        return;
    for (const oa::sim::detection::SightedUnit& seen : gained) {
        const oa::Unit* unit = live_at(world, seen.unit);
        const uint32_t generation = seen.generation;
        const uint16_t slot = seen.unit;
        remember(world.game.tick, "enemy_seen", [&](json::JsonWriter& writer) {
            writer.key("unit");
            writer.integer(unit != nullptr ? unit_number(*unit, slot) : slot);
            writer.key("generation");
            writer.integer(static_cast<int64_t>(generation));
            if (unit != nullptr)
                write_place(writer, *unit);
        });
    }
}

/// The hooks the watch listens with. The context is not used: there is one watch.
///
/// @return the table
hooks::EventHooks watch_hooks() {
    hooks::EventHooks table;
    table.unit_created = on_unit_created;
    table.unit_finished = on_unit_finished;
    table.unit_damaged = on_unit_damaged;
    table.unit_died = on_unit_died;
    table.unit_idle = on_unit_idle;
    table.order_ended = on_order_ended;
    table.seen_changed = on_seen_changed;
    return table;
}

/// Stops listening to the match the watch holds, while that match is still there.
void forget_listener() {
    PendingWatch& held = pending_watch();
    if (held.listener != 0 && held.match != nullptr)
        held.match->remove_event_listener(held.listener);
    held.listener = 0;
    held.match = nullptr;
    held.radar.clear();
}

/// Notes radar contacts the local player has gained.
void sample_contacts() {
    PendingWatch& held = pending_watch();
    if (held.runtime == nullptr || MatchViewAccess::match_of(*held.runtime) == nullptr)
        return;
    hooks::Match& match = *MatchViewAccess::match_of(*held.runtime);
    const oa::World& world = match.state();
    if (world.units == nullptr)
        return;
    uint8_t player = 0;
    if (!local_player(player))
        return;
    if (held.radar.size() < world.unit_slot_count)
        held.radar.resize(world.unit_slot_count);
    for (uint32_t slot = 1; slot < world.unit_slot_count && slot <= 0xffffu; ++slot) {
        const oa::Unit& unit = world.units[slot];
        const bool live = unit.type_index != 0 && (unit.flags & OA_UNIT_FLAG_LIVE) != 0;
        const bool other = live && unit.owner_index != player &&
                           unit.owner_index < OA_PLAYER_COUNT &&
                           !match.allied(player, unit.owner_index);
        const bool contact = other && (picture_byte(match, world, player, slot, unit) &
                                       oa::sim::detection::picture_radar) != 0;
        const auto index = static_cast<std::size_t>(slot);
        if (!contact) {
            held.radar[index] = 0;
            continue;
        }
        if (held.radar[index] != 0)
            continue;
        held.radar[index] = 1;
        const int32_t place_x = unit.position.x >> 16;
        const int32_t place_y = unit.position.y >> 16;
        const int32_t place_z = unit.position.z >> 16;
        remember(world.game.tick, "radar_contact", [&](json::JsonWriter& writer) {
            writer.key("x");
            writer.integer(place_x);
            writer.key("y");
            writer.integer(place_y);
            writer.key("z");
            writer.integer(place_z);
        });
    }
}

/// The word a match-life value is noted with.
///
/// @param life the hook's value
/// @return the word; empty when the value is not one the hook names
std::string_view life_word(uint8_t life) {
    switch (static_cast<MatchEvent>(life)) {
    case MatchEvent::finished:
        return "finished";
    case MatchEvent::torn_down:
        return "torn_down";
    case MatchEvent::left:
        return "left";
    case MatchEvent::results_reported:
        return "results_reported";
    case MatchEvent::results_released:
        return "results_released";
    case MatchEvent::watching_kept:
        return "watching_kept";
    }
    return {};
}

} // namespace

void begin_match_watch(Runtime& runtime) {
    PendingWatch& held = pending_watch();
    held.runtime = &runtime;
    sim::match_runtime::Match* running = MatchViewAccess::match_of(runtime);
    if (running == nullptr) {
        held.listener = 0;
        held.match = nullptr;
        held.listen_failed = false;
        held.radar.clear();
        return;
    }
    if (held.match == running && (held.listener != 0 || held.listen_failed))
        return;
    // The previous match has already gone. Its listener went with it.
    held.listener = 0;
    held.radar.clear();
    held.listen_failed = false;
    held.match = running;
    const sim::match_runtime::EventListenerId added = running->add_event_listener(watch_hooks());
    if (added == 0) {
        held.listen_failed = true;
        oa::platform::log_files::write_line("mcp: the match's event watch was not added");
        return;
    }
    held.listener = added;
}

void note_match_life(Runtime& runtime, uint8_t life) {
    pending_watch().runtime = &runtime;
    sim::match_runtime::Match* running = MatchViewAccess::match_of(runtime);
    const uint32_t tick = running != nullptr ? running->state().game.tick : 0;
    const std::string_view word = life_word(life);
    if (!word.empty()) {
        remember(tick, "match", [&](json::JsonWriter& writer) {
            writer.key("change");
            writer.string(word);
        });
    }
    if (static_cast<MatchEvent>(life) == MatchEvent::torn_down)
        forget_listener();
}

void sample_match_contacts(Runtime& runtime) {
    pending_watch().runtime = &runtime;
    sample_contacts();
}

void pull_match_notes(std::vector<MatchNote>* notes) {
    notes->clear();
    PendingWatch& held = pending_watch();
    notes->assign(held.pending.begin(), held.pending.end());
    held.pending.clear();
}

namespace {

constexpr std::size_t kMatchViewEntryCount = 10;
static_assert(
    sizeof(MatchView) == sizeof(void*) * (1 + kMatchViewEntryCount),
    "the match view's entries changed: set every one of them in match_view"
);

} // namespace

MatchView match_view(Runtime& runtime) {
    MatchView table;
    table.context = &runtime;
    table.active = MatchViewAccess::active;
    table.tick = MatchViewAccess::tick;
    table.viewer = MatchViewAccess::viewer;
    table.full_awareness_allowed = MatchViewAccess::full_awareness_allowed;
    table.units = MatchViewAccess::units;
    table.orders = MatchViewAccess::orders;
    table.types = MatchViewAccess::types;
    table.map_info = MatchViewAccess::map_info;
    table.visibility = MatchViewAccess::visibility;
    table.contacts = MatchViewAccess::contacts;
    return table;
}

void apply_give_orders(Runtime& runtime) {
    MatchViewAccess::give_orders(runtime);
}

} // namespace oa::app
