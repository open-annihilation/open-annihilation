// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The unit rules a mod sets that keep state of their own: the reuse delay of
// unit slots (units.id-reuse-delay), the sea occupy code units are created
// with (units.water-state-rules) and the facing buildings are placed in
// (units.build-rotation).
#include "oa/sim/match_runtime.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace oa::sim::match_runtime {
namespace {

// The largest footprint side a building may turn with.
constexpr int32_t turned_footprint_limit = 32;

// The facing a heading points: the quarter of a turn that holds the heading
// plus 0xA000, so that south spans 0x6000 to 0x9fff.
constexpr uint16_t facing_heading_offset = 0xa000;
constexpr unsigned facing_heading_shift = 14;

// A rule-state table's name for the slot reuse ticks.
constexpr const char* slot_reuse_table = "unit-slot-reuse";

/// Turns a yard by quarter turns.
///
/// @param yard the yard, row by row, `width` cells a row
/// @param width footprint width in cells
/// @param depth footprint depth in cells
/// @param quarter_turns 1 (east), 2 (north) or 3 (west)
/// @param[out] turned as many cells as the yard: the turned yard, its rows
///        `depth` cells long for an odd number of quarter turns and `width`
///        long otherwise
void turn_yard(
    std::span<const uint8_t> yard,
    int32_t width,
    int32_t depth,
    uint8_t quarter_turns,
    std::span<uint8_t> turned
) {
    const bool odd = (quarter_turns & 1) != 0;
    const int32_t rows = odd ? width : depth;
    const int32_t row_length = odd ? depth : width;
    for (int32_t row = 0; row < rows; ++row)
        for (int32_t column = 0; column < row_length; ++column) {
            int32_t from_row = 0;
            int32_t from_column = 0;
            switch (quarter_turns) {
            case 1:
                from_row = column;
                from_column = width - row - 1;
                break;
            case 2:
                from_row = depth - row - 1;
                from_column = width - column - 1;
                break;
            default:
                from_row = depth - column - 1;
                from_column = row;
                break;
            }
            turned
                [static_cast<std::size_t>(row) * static_cast<std::size_t>(row_length) +
                 static_cast<std::size_t>(column)] = yard
                    [static_cast<std::size_t>(from_row) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(from_column)];
        }
}

} // namespace

void Match::keep_unit_rules() {
    const auto& units = rules().units;
    auto& spawn_rules = state_.tables.rules;
    spawn_rules.start_submerged =
        units.water_state_rules.enabled && units.water_state_rules.start_submerged;
    if (units.id_reuse_delay.enabled && units.id_reuse_delay.ticks > 0) {
        slot_reuse_tick_count_ =
            static_cast<std::size_t>(OA_PLAYER_COUNT) * input_.per_player_limit;
        slot_reuse_ticks_ = std::make_unique<int32_t[]>(slot_reuse_tick_count_);
        spawn_rules.reuse_ticks = {slot_reuse_ticks_.get(), slot_reuse_tick_count_};
        spawn_rules.reuse_delay_ticks = units.id_reuse_delay.ticks;
        const RuleStateTable table{
            slot_reuse_table,
            this,
            [](void* context) -> std::span<const uint8_t> {
                const auto& match = *static_cast<Match*>(context);
                return {
                    reinterpret_cast<const uint8_t*>(match.slot_reuse_ticks_.get()),
                    match.slot_reuse_tick_count_ * sizeof(int32_t)
                };
            },
            [](void* context, std::span<const uint8_t> bytes) {
                auto& match = *static_cast<Match*>(context);
                if (bytes.size() != match.slot_reuse_tick_count_ * sizeof(int32_t))
                    return false;
                std::memcpy(match.slot_reuse_ticks_.get(), bytes.data(), bytes.size());
                return true;
            }
        };
        if (!add_rule_state(rule_state_, table))
            fault_.note("the unit slot reuse ticks cannot be kept");
    }
    if (!units.build_rotation.enabled)
        return;
    constexpr uint8_t turned_facings = data::match_rules::build_facing::east |
                                       data::match_rules::build_facing::north |
                                       data::match_rules::build_facing::west;
    const auto view = rules_view();
    const auto turns = [&](std::size_t type) -> std::size_t {
        if (type == 0 || type >= input_.fields.size())
            return 0;
        const auto& runtime = world_.types[type];
        const int32_t width = runtime.footprint_x;
        const int32_t depth = runtime.footprint_z;
        const auto yard = input_.fields[type].yard_mask;
        if (runtime.bm_code != 0 || (view.unit_type(type).build_facings & turned_facings) == 0 ||
            width < 1 || depth < 1 || width > turned_footprint_limit ||
            depth > turned_footprint_limit ||
            yard.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(depth))
            return 0;
        return yard.size();
    };
    turned_yard_type_count_ = world_.types.size();
    turned_yard_starts_ = std::make_unique<uint32_t[]>(turned_yard_type_count_);
    std::size_t total = 0;
    for (std::size_t type = 0; type < turned_yard_type_count_; ++type)
        total += 3 * turns(type);
    turned_yard_cells_ = std::make_unique<uint8_t[]>(total);
    std::size_t next = 0;
    for (std::size_t type = 0; type < turned_yard_type_count_; ++type) {
        const auto cells = turns(type);
        if (cells == 0)
            continue;
        turned_yard_starts_[type] = static_cast<uint32_t>(next + 1);
        for (uint8_t facing = 1; facing < 4; ++facing) {
            turn_yard(
                input_.fields[type].yard_mask,
                world_.types[type].footprint_x,
                world_.types[type].footprint_z,
                facing,
                {turned_yard_cells_.get() + next, cells}
            );
            next += cells;
        }
    }
}

uint8_t Match::build_facing(uint16_t type, uint8_t facing) const noexcept {
    facing &= 3U;
    if (facing == 0 || type >= turned_yard_type_count_ || turned_yard_starts_[type] == 0)
        return 0;
    const auto facings = rules_view().unit_type(type).build_facings;
    return (facings & (1U << facing)) != 0 ? facing : uint8_t{0};
}

uint8_t Match::build_facings(uint16_t type) const noexcept {
    uint8_t facings = data::match_rules::build_facing::south;
    for (uint8_t facing = 1; facing < 4; ++facing)
        if (build_facing(type, facing) == facing)
            facings = static_cast<uint8_t>(facings | (1U << facing));
    return facings;
}

uint8_t Match::build_facing_of_heading(uint16_t type, uint16_t heading) const noexcept {
    const auto turned = static_cast<uint16_t>(heading + facing_heading_offset);
    return build_facing(type, static_cast<uint8_t>(turned >> facing_heading_shift));
}

uint8_t Match::unit_build_facing(const oa::Unit& unit) const noexcept {
    const auto type = unit.type_index;
    const auto facing = build_facing_of_heading(type, unit.heading);
    if (facing == 0)
        return 0;
    const auto& runtime = world_.types[type];
    const bool odd = (facing & 1U) != 0;
    const auto width = odd ? runtime.footprint_z : runtime.footprint_x;
    const auto depth = odd ? runtime.footprint_x : runtime.footprint_z;
    return unit.footprint_x == width && unit.footprint_z == depth ? facing : uint8_t{0};
}

std::span<const uint8_t> Match::turned_yard(uint16_t type, uint8_t facing) const noexcept {
    const auto cells = input_.fields[type].yard_mask.size();
    return {
        turned_yard_cells_.get() + (turned_yard_starts_[type] - 1U) + (facing - 1U) * cells, cells
    };
}

std::span<const uint8_t> Match::build_yard(uint16_t type, uint8_t facing) const noexcept {
    const auto turned = build_facing(type, facing);
    if (turned != 0)
        return turned_yard(type, turned);
    if (type >= input_.fields.size())
        return {};
    return input_.fields[type].yard_mask;
}

std::span<const uint8_t> Match::unit_yard(sim::unit_spawn::Slot& slot) const {
    if (turned_yard_type_count_ != 0 && slot.unit != nullptr)
        if (const auto facing = unit_build_facing(slot.record); facing != 0)
            return turned_yard(slot.record.type_index, facing);
    return fields(slot).yard_mask;
}

bool Match::own_mobile_unit(uint16_t unit, uint8_t player) const noexcept {
    if (player == no_site_units_player || unit >= movement_.size() || unit >= slots_.size())
        return false;
    return movement_[unit] != nullptr && slots_[unit].record.owner_index == player;
}

void Match::set_build_facing(sim::simulation_state::Order& order, uint8_t facing) {
    if (auto* entry = runtime_order(&order)) {
        entry->construction.facing = static_cast<uint8_t>(facing & 3U);
        return;
    }
    fault_.note("build facing set on an order the match does not own");
}

} // namespace oa::sim::match_runtime
