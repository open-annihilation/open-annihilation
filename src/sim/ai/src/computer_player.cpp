// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ai.hpp"

#include "computer_internal.hpp"
#include "oa/base/game_math.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/unit_movement/movement.hpp"
#include "oa/sim/simulation_state.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>

namespace oa::sim::ai {
namespace {

constexpr uint32_t strike_interval_ticks = 300;
/// Counts every new strike member, including the seed of an empty squad.
constexpr uint32_t distributed_recruits_per_dispatch = 256;
/// Spatial attack groups span four bands along their larger horizontal extent.
constexpr uint32_t strike_cohort_bands = 4;
constexpr uint32_t strike_cohort_depths = 2;
constexpr uint32_t strike_cohort_count = strike_cohort_bands * strike_cohort_depths;
/// Keeps an exactly coincident target's weighted distance responsive to load.
constexpr uint64_t strike_target_distance_floor = 1;
/// Keeps a group's target until its squared distance exceeds the nearest by 25%.
constexpr uint64_t strike_front_distance_denominator = 4;
constexpr uint64_t strike_front_distance_numerator = 5;
static_assert(
    distributed_recruits_per_dispatch > 0 && distributed_recruits_per_dispatch <= max_recruits
);
constexpr uint32_t rally_interval_ticks = 150;
constexpr uint32_t construction_interval_ticks = 90;
constexpr uint32_t structures_interval_ticks = 30;
constexpr uint32_t air_raid_base_ticks = 30;
constexpr uint32_t air_raid_jitter_ticks = 900;
constexpr uint32_t siege_base_ticks = 30;
constexpr uint32_t siege_jitter_ticks = 150;
constexpr uint32_t siege_turn_odds = 10;         // one run in this many turns the search
constexpr int32_t siege_step_length = 0x1400000; // 320 world units
constexpr int32_t siege_search_radius = 160;     // world units around the searched point
// Builder-capable units from which a capturing builder that is idle circles the base
// instead of waiting near it; ai.builder-withhold-threshold leaves it at 5.
constexpr int32_t commander_patrol_quota = 5;
constexpr int32_t builder_patrol_radius = 0x1400000;   // 320 world units
constexpr int32_t builder_nudge_radius = 0x100000;     // 16 world units
constexpr int32_t commander_patrol_radius = 0x2800000; // 640 world units
constexpr uint8_t order_preserve_busy = 0x08;          // bit of Order::preserve_flags
constexpr uint8_t order_queue_idle = 0x40;             // bit of OrderState::command_flags
// EnergyUse from which the factory tick's power toggle runs under ai.factory-tick-filter's
// energy-use-32: the float's top byte, read as a signed number, at least this (32.0 and up).
constexpr int32_t power_toggle_energy_use_top_byte = 0x42;
// EnergyUse above which a building joins the structures squad under the role-squad rules:
// the float's high 16 bits, read as a signed number, above this (58.5 and up).
constexpr int32_t role_squads_energy_use_high_word = 0x4269;
// MaxWaterDepth from which a mobile unit joins the navy under the role-squad rules.
constexpr int16_t role_squads_navy_max_water_depth = 128;

/// Returns the bits of a float.
///
/// @param value the float
/// @return its IEEE 754 single-precision bits
uint32_t float_bits(float value) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

/// Returns the AI rules the computer players play by.
///
/// @param state computer players
/// @return the match's ai.* rules; 3.1c's for a null state
const data::match_rules::AiRules& ai_rules(const ComputerPlayers* state) noexcept {
    return state != nullptr ? state->rules.rules().ai : data::match_rules::baseline_match_rules.ai;
}

int32_t word(oa_fixed value) noexcept {
    return static_cast<int16_t>(static_cast<uint32_t>(value) >> 16);
}

int32_t wrap_add(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t wrap_sub(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

int32_t wrap_mul(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

oa::Unit* unit_at(const ComputerHost& host, uint16_t slot) noexcept {
    return oa::world_unit_at(host.world, slot);
}

uint32_t tick_of(const ComputerHost& host) noexcept {
    return host.world->game.tick;
}

/// Returns the number of members of one of the player's squads.
///
/// @param host squad membership and world
/// @param ai the player's controller
/// @param squad squad to count
/// @return its member count
uint32_t squad_size(const ComputerHost& host, const ComputerPlayer& ai, Squad squad) noexcept {
    return host.squad_size(host.context, ai.player, squad);
}

/// Tests whether one of the player's squads has no members.
///
/// @param host squad membership and world
/// @param ai the player's controller
/// @param squad squad to test
/// @return true when empty
bool squad_empty(const ComputerHost& host, const ComputerPlayer& ai, Squad squad) noexcept {
    return squad_size(host, ai, squad) == 0;
}

/// Returns the base position the player's knowledge settled on.
///
/// @param ai the player's controller
/// @return 16.16 world position
oa::FixedVec3 base_position(const ComputerPlayer& ai) noexcept {
    return ai.knowledge.base_position;
}

/// Returns the player's finished units that have a build list.
///
/// @param ai the player's controller
/// @return the count from the last knowledge rebuild
int32_t builder_count(const ComputerPlayer& ai) noexcept {
    return ai.knowledge.builder_count;
}

uint16_t
squad_member(const ComputerHost& host, const ComputerPlayer& ai, Squad squad, uint32_t i) noexcept {
    return host.squad_member(host.context, ai.player, squad, i);
}

/// Averages a squad's positions from the signed high words, back in 16.16.
///
/// @param host squad membership and world
/// @param ai the player's controller
/// @param squad squad to average
/// @param[out] out receives the centroid; unchanged for an empty squad
/// @return false for an empty squad
bool squad_centroid(
    const ComputerHost& host, const ComputerPlayer& ai, Squad squad, oa::FixedVec3* out
) noexcept {
    const auto count = squad_size(host, ai, squad);
    if (count == 0)
        return false;
    int32_t x = 0, y = 0, z = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* unit = unit_at(host, squad_member(host, ai, squad, i));
        if (unit == nullptr)
            continue;
        x += word(unit->position.x);
        y += word(unit->position.y);
        z += word(unit->position.z);
    }
    const auto n = static_cast<int32_t>(count);
    out->x = wrap_mul(x / n, 0x10000);
    out->y = wrap_mul(y / n, 0x10000);
    out->z = wrap_mul(z / n, 0x10000);
    return true;
}

/// Finds the player's home: the centroid of the armed structures, else the structures, else the builders.
///
/// @param host squad membership and world
/// @param ai the player's controller
/// @param[out] out receives the home position
/// @return false when all three squads are empty
bool home_centroid(
    const ComputerHost& host, const ComputerPlayer& ai, oa::FixedVec3* out
) noexcept {
    return squad_centroid(host, ai, Squad::armed_structures, out) ||
           squad_centroid(host, ai, Squad::structures, out) ||
           squad_centroid(host, ai, Squad::builders, out);
}

/// Finds the nearest live unit of any player the controller is not allied with.
///
/// Under ai.nearest-enemy-filter's skip-submerged rules the search also passes over units
/// whose occupancy or move-rate bits hold state 2 or 3, and a task that does not recruit
/// from the navy passes over fully submerged units; the naval strike still finds them.
///
/// @param state computer players, for the rules
/// @param host alliances and world
/// @param ai the player's controller
/// @param task the task searching: its source squad says whether it recruits from the navy
/// @param at reference point, 16.16 world coordinates
/// @return the unit, or null
/// @quirk Under skip-submerged only the naval strike finds fully submerged units: every
///        other task that searches, the aircraft task among them, passes over them.
oa::Unit* nearest_enemy(
    const ComputerPlayers* state,
    const ComputerHost& host,
    const ComputerPlayer& ai,
    const ComputerTask& task,
    const oa::FixedVec3& at
) noexcept {
    uint8_t relation[OA_PLAYER_COUNT + 1]{};
    for (uint8_t i = 0; i < OA_PLAYER_COUNT; ++i)
        relation[i] = host.allied(host.context, ai.player, i) ? 1 : 0;
    relation[OA_PLAYER_COUNT] = 1;
    sim::simulation_state::CandidateFilter filter{};
    // A dying replacement would consume the strike's cooldown without a live target.
    const auto& rules =
        state != nullptr ? state->rules.rules() : data::match_rules::baseline_match_rules;
    filter.skip_dying =
        task.kind == TaskKind::strike && rules.setup.enhanced_mode.enabled &&
        (rules.setup.enhanced_mode.version == data::enhanced_mode::battle_flow_version ||
         data::enhanced_mode::uses_distributed_battle_flow(rules.setup.enhanced_mode.version));
    if (ai_rules(state).nearest_enemy_filter.rules ==
        data::match_rules::AiNearestEnemyFilterRules::skip_submerged) {
        filter.widened_flags = true;
        filter.skip_submerged = task.source_squad != Squad::navy;
    }
    return sim::simulation_state::nearest_candidate_unit(*host.world, relation, at.x, at.z, filter);
}

enum class Command : uint8_t { move, patrol, attack };

/// Orders every unit of the player in a squad, in unit-slot order.
///
/// @param host order calls and world
/// @param ai the player's controller
/// @param squad squad to order
/// @param command move, patrol or attack
/// @param queue true to queue a move or patrol behind the current orders
/// @param target unit to attack (attack only)
/// @param at destination (move and patrol)
void order_squad(
    const ComputerHost& host,
    const ComputerPlayer& ai,
    Squad squad,
    Command command,
    bool queue,
    uint16_t target,
    const oa::FixedVec3* at
) noexcept {
    const auto& player = host.world->game.players[ai.player];
    uint32_t count = 0;
    auto* units = oa::world_player_units(host.world, &player, &count);
    if (units == nullptr)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& unit = units[i];
        if (unit.type_index == 0 || unit.squad != static_cast<int32_t>(squad))
            continue;
        const auto slot = static_cast<uint16_t>(oa::world_unit_slot(host.world, &unit));
        switch (command) {
        case Command::move:
            (void)host.order_move(host.context, slot, at, queue);
            break;
        case Command::patrol:
            (void)host.order_patrol(host.context, slot, at, queue);
            break;
        case Command::attack:
            (void)host.order_attack(host.context, slot, target);
            break;
        }
    }
}

/// Holds the horizontal extent used to divide a strike into spatial groups.
struct StrikeExtent {
    int32_t least_x{std::numeric_limits<int32_t>::max()};
    int32_t greatest_x{std::numeric_limits<int32_t>::min()};
    int32_t least_z{std::numeric_limits<int32_t>::max()};
    int32_t greatest_z{std::numeric_limits<int32_t>::min()};
    bool along_x{};
};

/// Holds one spatial group's centroid sums and selected target's slot and position.
struct StrikeCohort {
    int64_t sum_x{};
    int64_t sum_z{};
    uint32_t members{};
    uint16_t target{};
    std::array<int32_t, 2> target_position{}; // selected target's whole-world X/Z
};

/// Keeps a cohort search's selected record and bounded work summary.
struct CohortChoice {
    const oa::Unit* unit{};
    uint64_t cost{};
    uint32_t eligible_count{};
};

/// Tests whether a finished live member can receive a strike's attack command.
bool strike_member(const oa::Unit& unit, uint8_t player, Squad squad) noexcept {
    return unit.type_index != 0 && unit.owner_index == player &&
           unit.squad == static_cast<int32_t>(squad) && oa::unit_is_live_target(unit.flags) &&
           unit.health > 0 && unit.build_remaining == 0;
}

/// Finds a coordinate's stable band inside an inclusive whole-world extent.
uint32_t strike_band(int32_t value, int32_t least, int32_t greatest, uint32_t bands) noexcept {
    const auto offset = static_cast<uint64_t>(static_cast<int64_t>(value) - least);
    const auto width = static_cast<uint64_t>(static_cast<int64_t>(greatest) - least) + 1;
    return static_cast<uint32_t>(offset * bands / width);
}

/// Finds a member's spatial group without depending on squad-vector ordering.
uint32_t strike_cohort(const oa::Unit& unit, const StrikeExtent& extent) noexcept {
    const auto x = word(unit.position.x);
    const auto z = word(unit.position.z);
    const auto bands = extent.along_x
                           ? strike_band(x, extent.least_x, extent.greatest_x, strike_cohort_bands)
                           : strike_band(z, extent.least_z, extent.greatest_z, strike_cohort_bands);
    const auto depth =
        extent.along_x ? strike_band(z, extent.least_z, extent.greatest_z, strike_cohort_depths)
                       : strike_band(x, extent.least_x, extent.greatest_x, strike_cohort_depths);
    return bands * strike_cohort_depths + depth;
}

/// Tests the same enemy flags and water states used by the strike search.
bool cohort_enemy_eligible(
    const oa::Unit& unit, const sim::simulation_state::CandidateFilter& filter
) noexcept {
    constexpr uint32_t occupancy_upper_states = 0x2u;
    constexpr uint32_t move_rate_upper_tiers = 0x8u;
    constexpr uint32_t widened_skip =
        occupancy_upper_states | move_rate_upper_tiers | OA_UNIT_FLAG_NOT_SELECTABLE;
    constexpr uint32_t submerged_occupy_code = 3;
    if (!oa::unit_is_live_target(unit.flags) || unit.health <= 0 ||
        (unit.state_flags & OA_UNIT_STATE_CLOAKED) != 0)
        return false;
    if (filter.widened_flags) {
        if ((unit.flags & widened_skip) != 0)
            return false;
    } else if (
        (unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == occupancy_upper_states ||
        (unit.flags & OA_UNIT_FLAG_NOT_SELECTABLE) != 0
    ) {
        return false;
    }
    if (filter.skip_submerged) {
        uint32_t occupy_code{};
        std::memcpy(&occupy_code, unit.last_occupy_code, sizeof occupy_code);
        if (occupy_code == submerged_occupy_code)
            return false;
    }
    return true;
}

/// Chooses a nearby enemy while charging for earlier groups sent to that target.
///
/// Whole-world coordinate differences are squared in 64 bits. Each previous
/// assignment adds one to the distance multiplier; at most eight groups search.
/// A lower canonical slot resolves an equal weighted distance. The search reads
/// current records, keeping no target handle across dispatches.
///
/// @param host alliances and canonical unit records
/// @param relation the controller's alliance row, including the no-player entry
/// @param filter the strike's additional water and occupancy exclusions
/// @param cohort the current group's aggregate positions and membership
/// @param earlier groups whose targets were already selected
/// @return selected record, weighted distance and eligible-candidate count
CohortChoice choose_cohort_target(
    const ComputerHost& host,
    std::span<const uint8_t> relation,
    const sim::simulation_state::CandidateFilter& filter,
    const StrikeCohort& cohort,
    std::span<const StrikeCohort> earlier
) noexcept {
    CohortChoice choice{};
    const auto center_x = cohort.sum_x / cohort.members;
    const auto center_z = cohort.sum_z / cohort.members;
    uint16_t best_slot{};
    for (uint8_t player_index = 0; player_index < OA_PLAYER_COUNT; ++player_index) {
        const auto& player = host.world->game.players[player_index];
        if (!sim::simulation_state::player_slot_active(player_index, player) ||
            relation[player.index] != 0)
            continue;
        uint32_t count{};
        const auto* units = oa::world_player_units(host.world, &player, &count);
        if (units == nullptr)
            continue;
        for (uint32_t index = 0; index < count; ++index) {
            const auto& unit = units[index];
            if (!cohort_enemy_eligible(unit, filter))
                continue;
            ++choice.eligible_count;
            const auto slot = static_cast<uint16_t>(oa::world_unit_slot(host.world, &unit));
            uint64_t assignments{};
            for (const auto& previous : earlier)
                assignments += previous.target == slot ? 1 : 0;
            const int64_t dx = center_x - word(unit.position.x);
            const int64_t dz = center_z - word(unit.position.z);
            const auto distance = static_cast<uint64_t>(dx * dx + dz * dz);
            const auto cost = (distance + strike_target_distance_floor) * (assignments + 1);
            if (choice.unit == nullptr || cost < choice.cost ||
                (cost == choice.cost && slot < best_slot)) {
                choice.unit = &unit;
                choice.cost = cost;
                best_slot = slot;
            }
        }
    }
    return choice;
}

/// Measures a member's squared whole-world distance from a selected front.
///
/// @param unit canonical member whose integer world position is compared
/// @param front selected target's integer world position
/// @return squared X/Z distance, widened before multiplication
uint64_t strike_front_distance(const oa::Unit& unit, const StrikeCohort& front) noexcept {
    const int64_t dx = static_cast<int64_t>(word(unit.position.x)) - front.target_position[0];
    const int64_t dz = static_cast<int64_t>(word(unit.position.z)) - front.target_position[1];
    return static_cast<uint64_t>(dx * dx + dz * dz);
}

/// Retains the group's target unless another selected front is materially closer.
///
/// Only the at most eight targets already selected for the strike are compared.
/// The group's squared distance must exceed the nearest front's by more than
/// 25%: four times the group's distance must exceed five times the nearest
/// front's. Equality retains the group's target; equal nearest fronts
/// choose the lower canonical slot. No enemy record is searched or reread here.
///
/// @param unit canonical member about to receive an attack callback
/// @param own the member's spatial group and its selected front
/// @param fronts the strike's selected targets and whole-world positions
/// @return canonical target slot, or zero when the group has no target
uint16_t strike_member_target(
    const oa::Unit& unit, const StrikeCohort& own, std::span<const StrikeCohort> fronts
) noexcept {
    if (own.target == 0)
        return 0;
    const auto own_distance = strike_front_distance(unit, own);
    auto best_distance = own_distance;
    auto best_target = own.target;
    for (const auto& front : fronts) {
        if (front.target == 0)
            continue;
        const auto distance = strike_front_distance(unit, front);
        if (distance < best_distance || (distance == best_distance && front.target < best_target)) {
            best_distance = distance;
            best_target = front.target;
        }
    }
    return strike_front_distance_denominator * own_distance >
                   strike_front_distance_numerator * best_distance
               ? best_target
               : own.target;
}

/// Orders live strike members toward nearby targets shared by spatial groups.
///
/// Three unit-slot walks find the extent, form at most eight centroids and
/// dispatch. Each occupied group searches the enemy records once. Existing
/// order callbacks retain their ordinary mission and same-target handling.
///
/// @param state computer players and AI exclusions
/// @param host canonical records, alliances and ordinary order callbacks
/// @param ai the controller whose strike is dispatched
/// @param task the land or naval strike task
/// @param sources selected source-slot bits; empty dispatches every member
/// @return true when at least one attack callback accepted an order
bool order_strike_cohorts(
    const ComputerPlayers* state,
    const ComputerHost& host,
    const ComputerPlayer& ai,
    const ComputerTask& task,
    std::span<const uint64_t> sources = {}
) noexcept {
    const auto& player = host.world->game.players[ai.player];
    uint32_t count{};
    auto* units = oa::world_player_units(host.world, &player, &count);
    if (units == nullptr)
        return false;
    StrikeExtent extent{};
    bool populated = false;
    for (uint32_t index = 0; index < count; ++index) {
        const auto& unit = units[index];
        if (!strike_member(unit, ai.player, task.squad))
            continue;
        const auto x = word(unit.position.x);
        const auto z = word(unit.position.z);
        extent.least_x = std::min(extent.least_x, x);
        extent.greatest_x = std::max(extent.greatest_x, x);
        extent.least_z = std::min(extent.least_z, z);
        extent.greatest_z = std::max(extent.greatest_z, z);
        populated = true;
    }
    if (!populated)
        return false;
    extent.along_x = static_cast<int64_t>(extent.greatest_x) - extent.least_x >=
                     static_cast<int64_t>(extent.greatest_z) - extent.least_z;
    std::array<StrikeCohort, strike_cohort_count> cohorts{};
    for (uint32_t index = 0; index < count; ++index) {
        const auto& unit = units[index];
        if (!strike_member(unit, ai.player, task.squad))
            continue;
        auto& cohort = cohorts[strike_cohort(unit, extent)];
        cohort.sum_x += word(unit.position.x);
        cohort.sum_z += word(unit.position.z);
        ++cohort.members;
    }
    std::array<uint8_t, OA_PLAYER_COUNT + 1> relation{};
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index)
        relation[index] = host.allied(host.context, ai.player, index) ? 1 : 0;
    relation[OA_PLAYER_COUNT] = 1;
    sim::simulation_state::CandidateFilter filter{};
    filter.skip_dying = true;
    if (ai_rules(state).nearest_enemy_filter.rules ==
        data::match_rules::AiNearestEnemyFilterRules::skip_submerged) {
        filter.widened_flags = true;
        filter.skip_submerged = task.source_squad != Squad::navy;
    }
    bool targeted = false;
    for (uint32_t index = 0; index < cohorts.size(); ++index) {
        auto& cohort = cohorts[index];
        if (cohort.members == 0)
            continue;
        const auto choice = choose_cohort_target(
            host, relation, filter, cohort, std::span<const StrikeCohort>(cohorts).first(index)
        );
        if (choice.unit != nullptr) {
            cohort.target = static_cast<uint16_t>(oa::world_unit_slot(host.world, choice.unit));
            cohort.target_position = {word(choice.unit->position.x), word(choice.unit->position.z)};
            targeted = true;
        }
    }
    if (!targeted)
        return false;
    bool issued = false;
    for (uint32_t index = 0; index < count; ++index) {
        const auto& unit = units[index];
        if (!strike_member(unit, ai.player, task.squad))
            continue;
        const auto slot = static_cast<uint16_t>(oa::world_unit_slot(host.world, &unit));
        if (!sources.empty() && (sources[slot / strike_refresh_word_bits] &
                                 (uint64_t{1} << (slot % strike_refresh_word_bits))) == 0)
            continue;
        const auto target =
            strike_member_target(unit, cohorts[strike_cohort(unit, extent)], cohorts);
        if (target != 0) {
            issued = host.order_attack(host.context, slot, target) || issued;
        }
    }
    return issued;
}

/// Pulls members of a source squad within the per-member radius into the task's squad.
///
/// Seeds an empty task squad with the source's first member, then sheds the farthest
/// stragglers back to the source until the squad fits the radius, and last recruits
/// every source member inside it.
///
/// @param host squad membership and world
/// @param ai the player's controller
/// @param task task whose squad recruits
/// @param source squad recruited from; the task's own squad does nothing
/// @param radius squared world units allowed per member
/// @param budget maximum new members including the seed; none retains the legacy joining limit
void recruit_squad(
    const ComputerHost& host,
    ComputerPlayer& ai,
    const ComputerTask& task,
    Squad source,
    int32_t radius,
    std::optional<uint32_t> budget
) noexcept {
    if (source == task.squad || (budget && *budget == 0))
        return;
    uint32_t joining_limit = budget ? std::min(*budget, max_recruits) : max_recruits;
    if (squad_size(host, ai, task.squad) == 0) {
        if (squad_size(host, ai, source) == 0)
            return;
        host.set_squad(host.context, squad_member(host, ai, source, 0), task.squad);
        if (budget)
            --joining_limit;
    }
    int32_t sum_x = 0, sum_z = 0;
    for (uint32_t i = 0; i < squad_size(host, ai, task.squad); ++i) {
        const auto* unit = unit_at(host, squad_member(host, ai, task.squad, i));
        sum_x += word(unit->position.x);
        sum_z += word(unit->position.z);
    }
    int32_t center_x = 0, center_z = 0;
    for (;;) {
        const auto count = static_cast<int32_t>(squad_size(host, ai, task.squad));
        center_x = sum_x / count;
        center_z = sum_z / count;
        if (count == 1)
            break;
        int32_t farthest = 0;
        uint16_t straggler = 0;
        for (int32_t i = 0; i < count; ++i) {
            const auto slot = squad_member(host, ai, task.squad, static_cast<uint32_t>(i));
            const auto* unit = unit_at(host, slot);
            const auto dx = word(unit->position.x) - center_x;
            const auto dz = word(unit->position.z) - center_z;
            const auto distance = wrap_add(wrap_mul(dz, dz), wrap_mul(dx, dx));
            if (farthest < distance) {
                farthest = distance;
                straggler = slot;
            }
        }
        if (farthest < wrap_mul(count, radius))
            break;
        const auto* unit = unit_at(host, straggler);
        host.set_squad(host.context, straggler, source);
        sum_x -= word(unit->position.x);
        sum_z -= word(unit->position.z);
    }
    uint16_t joining[max_recruits];
    uint32_t joining_count = 0;
    const auto own = static_cast<int32_t>(squad_size(host, ai, task.squad));
    const auto candidates = squad_size(host, ai, source);
    for (uint32_t i = 0; i < candidates && joining_count < joining_limit; ++i) {
        const auto slot = squad_member(host, ai, source, i);
        const auto* unit = unit_at(host, slot);
        const auto dx = word(unit->position.x) - center_x;
        const auto dz = word(unit->position.z) - center_z;
        if (wrap_add(wrap_mul(dz, dz), wrap_mul(dx, dx)) < wrap_mul(own, radius))
            joining[joining_count++] = slot;
    }
    for (uint32_t i = 0; i < joining_count; ++i)
        host.set_squad(host.context, joining[i], task.squad);
}

/// Runs a strike group task.
///
/// Recruits, then rallies at home until big enough, then attacks the nearest enemy
/// unit. Runs again in 300 ticks.
///
/// @param state computer players, for the rules
/// @param host squads, orders and world
/// @param ai the player's controller
/// @param[in,out] task strike task; its attacking flag and next tick are updated
void run_strike(
    const ComputerPlayers* state, const ComputerHost& host, ComputerPlayer& ai, ComputerTask& task
) noexcept {
    task.next_tick = tick_of(host) + strike_interval_ticks;
    const auto& rules =
        state != nullptr ? state->rules.rules() : data::match_rules::baseline_match_rules;
    const bool distributed =
        rules.setup.enhanced_mode.enabled &&
        data::enhanced_mode::uses_distributed_battle_flow(rules.setup.enhanced_mode.version);
    recruit_squad(
        host,
        ai,
        task,
        task.source_squad,
        task.merge_radius,
        distributed ? std::optional<uint32_t>{distributed_recruits_per_dispatch} : std::nullopt
    );
    if (squad_empty(host, ai, task.squad))
        return;
    const auto count = static_cast<int32_t>(squad_size(host, ai, task.squad));
    bool attack = false;
    if (count > task.min_size)
        attack = task.attacking != 0 || count >= task.launch_size;
    if (!attack) {
        oa::FixedVec3 home{};
        if (home_centroid(host, ai, &home)) {
            task.attacking = 0;
            order_squad(host, ai, task.squad, Command::move, false, 0, &home);
            return;
        }
    }
    task.attacking = 1;
    if (rules.setup.enhanced_mode.enabled &&
        data::enhanced_mode::uses_cohort_target_distribution(rules.setup.enhanced_mode.version)) {
        (void)order_strike_cohorts(state, host, ai, task);
        return;
    }
    oa::FixedVec3 center{};
    (void)squad_centroid(host, ai, task.squad, &center);
    const auto* target = nearest_enemy(state, host, ai, task, center);
    if (target == nullptr)
        return;
    const auto target_slot = static_cast<uint16_t>(oa::world_unit_slot(host.world, target));
    order_squad(host, ai, task.squad, Command::attack, false, target_slot, nullptr);
}

/// Runs a waiting squad's task: move to the strike group when both have members.
///
/// Runs again in 150 ticks.
///
/// @param host squads, orders and world
/// @param ai the player's controller
/// @param[in,out] task rally task
void run_rally(const ComputerHost& host, ComputerPlayer& ai, ComputerTask& task) noexcept {
    task.next_tick = tick_of(host) + rally_interval_ticks;
    if (squad_empty(host, ai, task.squad) || squad_empty(host, ai, task.source_squad))
        return;
    oa::FixedVec3 center{};
    if (squad_centroid(host, ai, task.source_squad, &center))
        order_squad(host, ai, task.squad, Command::move, false, 0, &center);
}

/// Runs the aircraft task.
///
/// Small wings scout around the base (or patrol to the nearest enemy before a base
/// exists); wings of five or more (ai.patrol-group-size) patrol to a random map edge.
/// Under ai.patrol-null-enemy-skip a small wing without a base patrols to a random map
/// edge too, drawing as a large wing does. Runs again in 30 plus a random 0..899 ticks.
///
/// @param state computer players, for the rules
/// @param host squads, orders, random stream and world
/// @param ai the player's controller
/// @param[in,out] task air raid task
void run_air_raid(
    const ComputerPlayers* state, const ComputerHost& host, ComputerPlayer& ai, ComputerTask& task
) noexcept {
    task.next_tick =
        tick_of(host) + host.random(host.context, air_raid_jitter_ticks) + air_raid_base_ticks;
    const auto& rules = ai_rules(state);
    const auto world_x = host.map_cells_x << 4;
    const auto world_z = host.map_cells_z << 4;
    const auto base = base_position(ai);
    const bool no_base =
        (static_cast<uint32_t>(base.x) >> 16 | static_cast<uint32_t>(base.z) >> 16) == 0;
    const bool small_wing =
        static_cast<int32_t>(squad_size(host, ai, task.squad)) < rules.patrol_group_size.units;
    if (small_wing && !(no_base && rules.patrol_null_enemy_skip.enabled)) {
        if (no_base) {
            oa::FixedVec3 center{};
            (void)squad_centroid(host, ai, task.squad, &center);
            const auto* target = nearest_enemy(state, host, ai, task, center);
            if (target != nullptr)
                order_squad(host, ai, task.squad, Command::patrol, true, 0, &target->position);
            return;
        }
        const auto waypoints = static_cast<int32_t>(host.random(host.context, 2)) + 2;
        const auto span_x = world_x / 8;
        const auto span_z = world_z / 8;
        const auto half_x = span_x / 2;
        const auto half_z = span_z / 2;
        for (int32_t i = 0; i < waypoints; ++i) {
            oa::FixedVec3 point{};
            const auto rx =
                static_cast<int32_t>(host.random(host.context, static_cast<uint32_t>(span_x)));
            point.x = wrap_add(wrap_mul(wrap_add(rx, wrap_mul(half_x, 0xffff)), 0x10000), base.x);
            point.y = base.y;
            const auto rz =
                static_cast<int32_t>(host.random(host.context, static_cast<uint32_t>(span_z)));
            point.z = wrap_add(wrap_mul(wrap_add(rz, wrap_mul(half_z, 0xffff)), 0x10000), base.z);
            order_squad(
                host, ai, task.squad, i == 0 ? Command::move : Command::patrol, i != 0, 0, &point
            );
        }
        return;
    }
    oa::FixedVec3 edge{};
    if (host.random(host.context, 2) != 0) {
        edge.x = wrap_mul(
            static_cast<int32_t>(host.random(host.context, static_cast<uint32_t>(world_x))), 0x10000
        );
        edge.z =
            host.random(host.context, 2) != 0 ? 0 : wrap_mul(wrap_add(world_z, 0xffff), 0x10000);
    } else {
        edge.x =
            host.random(host.context, 2) != 0 ? 0 : wrap_mul(wrap_add(world_x, 0xffff), 0x10000);
        edge.z = wrap_mul(
            static_cast<int32_t>(host.random(host.context, static_cast<uint32_t>(world_z))), 0x10000
        );
    }
    order_squad(host, ai, task.squad, Command::patrol, false, 0, &edge);
}

/// Runs the siege task: search the sighted enemy for the richest area, and attack it.
///
/// With members in its squad, the search starts again from the target one run in ten,
/// heading a random way in steps of 320 world units, and takes one step each run. A
/// point the player sees is weighed by the sighted base weight within 160 world units
/// of it, and becomes the target when a draw below its weight beats a draw below the
/// target's. Then every member that can attack is ordered to attack the target, as the
/// Attack command over open ground orders it; one without a movement object only when
/// its first weapon reaches the target. Runs again in 30 plus a random 0..149 ticks.
///
/// @param state computer players and their type table
/// @param host squads, sight, sightings, weapon reach, orders, random stream and world
/// @param ai the player's controller
/// @param[in,out] task siege task; its search state and target change
/// @quirk The sort puts no unit in the siege squad, so a computer player's siege
///        only reschedules itself; only units already in squad 9 take part.
void run_siege(
    const ComputerPlayers* state, const ComputerHost& host, ComputerPlayer& ai, ComputerTask& task
) noexcept {
    task.next_tick =
        tick_of(host) + host.random(host.context, siege_jitter_ticks) + siege_base_ticks;
    if (squad_empty(host, ai, task.squad))
        return;
    if (host.random(host.context, siege_turn_odds) == 0) {
        task.siege_probe = task.siege_target;
        const auto angle = static_cast<uint16_t>(host.random(host.context, 0x10000));
        task.siege_step = {
            -sim::unit_movement::sine_scaled(angle, siege_step_length),
            0,
            -sim::unit_movement::cosine_scaled(angle, siege_step_length)
        };
    }
    task.siege_probe = {
        wrap_add(task.siege_probe.x, task.siege_step.x),
        wrap_add(task.siege_probe.y, task.siege_step.y),
        wrap_add(task.siege_probe.z, task.siege_step.z)
    };
    if (host.point_visible != nullptr &&
        host.point_visible(host.context, ai.player, &task.siege_probe)) {
        const auto* sightings =
            host.sightings != nullptr ? host.sightings(host.context, ai.player) : nullptr;
        const auto weight = sightings != nullptr ? computer_sighted_weight(
                                                       state,
                                                       ai.knowledge,
                                                       *sightings,
                                                       *host.world,
                                                       task.siege_probe,
                                                       siege_search_radius
                                                   )
                                                 : 0;
        const auto draw = host.random(host.context, static_cast<uint32_t>(weight));
        const auto held = host.random(host.context, static_cast<uint32_t>(task.siege_weight));
        if (static_cast<int32_t>(draw) > static_cast<int32_t>(held)) {
            task.siege_target = task.siege_probe;
            task.siege_weight = weight;
        }
    }
    for (uint32_t i = 0; i < squad_size(host, ai, task.squad); ++i) {
        const auto slot = squad_member(host, ai, task.squad, i);
        const auto* unit = unit_at(host, slot);
        const auto* def = unit != nullptr ? oa::world_unit_def_of(host.world, unit) : nullptr;
        if (def == nullptr || (def->abilities & OA_UNIT_DEF_ABILITY_CAN_ATTACK) == 0)
            continue;
        if (unit->movement == 0 && (host.weapon_reaches == nullptr ||
                                    !host.weapon_reaches(host.context, slot, &task.siege_target)))
            continue;
        if (host.order_attack_point != nullptr)
            (void)host.order_attack_point(host.context, slot, &task.siege_target);
    }
}

/// Tells whether the structures task's power toggle runs for a building type.
///
/// @param filter the match's ai.factory-tick-filter rule
/// @param type the building's type
/// @return true for a metal maker (MakesMetal), or under energy-use-32 for a type whose
///         EnergyUse is 32 or more
/// @quirk energy-use-32 reads the float's top byte as a signed number, at least 0x42: any
///        negative EnergyUse fails it.
bool runs_power_toggle(
    const data::match_rules::AiFactoryTickFilter& filter, const ComputerType& type
) noexcept {
    if (filter.power_toggle == data::match_rules::AiFactoryTickFilterPowerToggle::energy_use_32)
        return static_cast<int8_t>(float_bits(type.energy_use) >> 24) >=
               power_toggle_energy_use_top_byte;
    return type.makes_metal != 0;
}

/// Runs the structures task: power users follow the energy surplus; idle factories queue one pick.
///
/// The power toggle runs for metal makers, or under ai.factory-tick-filter's energy-use-32
/// for buildings using 32 energy or more; under its skip-busy-background-queue a building
/// whose secondary order queue holds an order is left alone. Runs again in 30 ticks.
///
/// @param state computer players and their type table
/// @param host squads, orders, random stream and world
/// @param ai the player's controller
/// @param[in,out] task structures task
void run_structures(
    ComputerPlayers* state, const ComputerHost& host, ComputerPlayer& ai, ComputerTask& task
) noexcept {
    task.next_tick = tick_of(host) + structures_interval_ticks;
    const auto& filter = ai_rules(state).factory_tick_filter;
    const auto& player = host.world->game.players[ai.player];
    for (uint32_t i = 0; i < squad_size(host, ai, task.squad); ++i) {
        const auto slot = squad_member(host, ai, task.squad, i);
        const auto* unit = unit_at(host, slot);
        if (unit == nullptr || (unit->flags & OA_UNIT_FLAG_BUILDING) == 0 ||
            !oa::unit_is_live_target(unit->flags))
            continue;
        if (filter.skip_busy_background_queue && host.secondary_order != nullptr &&
            host.secondary_order(host.context, slot))
            continue;
        const auto* type = computer_type(state, unit->type_index);
        if (type == nullptr)
            continue;
        if (runs_power_toggle(filter, *type)) {
            bool on = false;
            if (player.energy > player.metal + player.metal) {
                if (!(player.energy_produced - player.energy_requested > 0.0F))
                    continue;
                if (host.random(host.context, 5) == 0)
                    continue;
                on = true;
            }
            host.set_active(host.context, slot, on);
            continue;
        }
        if (type->build_count == 0)
            continue;
        uint8_t preserve = 0, queue = 0;
        if (host.primary_order(host.context, slot, &preserve, &queue))
            continue;
        const auto pick = computer_pick_build(state, host, ai.player, slot);
        if (pick != 0)
            (void)host.order_factory(host.context, slot, pick, 1);
    }
}

/// Tells whether a build list's entry is a round for its builder's first weapon to
/// stockpile: the type's unit name holds MAKENUKE or MAKEANTI, as the build queue tells it.
///
/// @param type the entry's type; null for one outside the table
/// @return true for a round
bool stockpile_round(const ComputerType* type) noexcept {
    if (type == nullptr)
        return false;
    const auto& name = type->unit_name;
    const std::string_view text(name, std::find(std::begin(name), std::end(name), '\0'));
    return text.find("MAKENUKE") != std::string_view::npos ||
           text.find("MAKEANTI") != std::string_view::npos;
}

/// Runs the builders' task.
///
/// Each builder places a weighted pick near the base; idle ones patrol home.
/// Capturing builders (commanders) build only while the player has fewer than five
/// build-capable units (ai.builder-withhold-threshold), and once idle circle the base
/// from five. A pick that is a round for the builder's weapon is not placed: it is
/// queued as the structures task queues a factory's pick, and the builder stockpiles
/// it. Runs again in 90 ticks.
///
/// @param state computer players and their type table
/// @param host squads, orders, placement queries, random stream and world
/// @param ai the player's controller
/// @param[in,out] task construction task
/// @quirk ai.builder-withhold-threshold moves only the build limit: an idle capturing builder
///        still circles the base from five build-capable units.
void run_construction(
    ComputerPlayers* state, const ComputerHost& host, ComputerPlayer& ai, ComputerTask& task
) noexcept {
    task.next_tick = tick_of(host) + construction_interval_ticks;
    const auto withhold_from = ai_rules(state).builder_withhold_threshold.builders;
    const auto base = base_position(ai);
    for (uint32_t i = 0; i < squad_size(host, ai, task.squad); ++i) {
        const auto slot = squad_member(host, ai, task.squad, i);
        const auto* unit = unit_at(host, slot);
        const auto* type = unit != nullptr ? computer_type(state, unit->type_index) : nullptr;
        if (type == nullptr || type->build_count == 0)
            continue;
        const bool captures = (type->abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE) != 0;
        if (captures &&
            (builder_count(ai) >= withhold_from || tick_of(host) < ai.commander_build_tick))
            continue;
        uint8_t preserve = 0, queue = 0;
        if (host.primary_order(host.context, slot, &preserve, &queue) &&
            (preserve & order_preserve_busy) != 0)
            continue;
        const auto pick = computer_pick_build(state, host, ai.player, slot);
        if (pick == 0)
            continue;
        // A round is no unit to place: its type names a cost and little else, and the
        // queue stockpiles it for the builder's first weapon.
        if (stockpile_round(computer_type(state, pick))) {
            (void)host.order_factory(host.context, slot, pick, 1);
            continue;
        }
        oa::FixedVec3 site{};
        bool placed = computer_place_build(state, host, ai, unit->position, pick, &site);
        if (captures) {
            const auto reach =
                wrap_mul((host.map_cells_x * 16 - 32 + host.map_cells_z * 16 - 128) / 3, 0x10000);
            const auto length = base::game_math::truncated_length(
                wrap_sub(site.x, base.x), 0, wrap_sub(site.z, base.z)
            );
            if (length > reach)
                placed = false;
        }
        if (placed)
            (void)host.order_build(host.context, slot, pick, &site);
    }
    for (uint32_t i = 0; i < squad_size(host, ai, task.squad); ++i) {
        const auto slot = squad_member(host, ai, task.squad, i);
        const auto* unit = unit_at(host, slot);
        const auto* type = unit != nullptr ? computer_type(state, unit->type_index) : nullptr;
        if (type == nullptr)
            continue;
        uint8_t preserve = 0, queue = 0;
        if (host.primary_order(host.context, slot, &preserve, &queue) &&
            (queue & order_queue_idle) == 0)
            continue;
        const bool captures = (type->abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE) != 0;
        if (captures && builder_count(ai) < commander_patrol_quota)
            continue;
        oa::FixedVec3 destination = base;
        if (captures) {
            int32_t offset[3] = {
                wrap_sub(base.x, unit->position.x), 0, wrap_sub(base.z, unit->position.z)
            };
            if (base::game_math::truncated_length(offset[0], offset[1], offset[2]) >
                commander_patrol_radius) {
                const auto angle = static_cast<uint16_t>(host.random(host.context, 0x10000));
                offset[0] = -sim::unit_movement::sine_scaled(angle, commander_patrol_radius);
                offset[1] = 0;
                offset[2] = -sim::unit_movement::cosine_scaled(angle, commander_patrol_radius);
            }
            oa::FixedVec3 circle{
                wrap_add(base.x, offset[0]),
                wrap_add(unit->position.y, offset[1]),
                wrap_add(base.z, offset[2])
            };
            (void)host.order_move(host.context, slot, &circle, false);
            oa::FixedVec3 home{base.x, unit->position.y, base.z};
            (void)host.order_patrol(host.context, slot, &home, true);
            continue;
        }
        int32_t offset[3] = {
            wrap_sub(base.x, unit->position.x),
            wrap_sub(base.y, unit->position.y),
            wrap_sub(base.z, unit->position.z)
        };
        const auto length = base::game_math::truncated_length(offset[0], offset[1], offset[2]);
        if (length < builder_patrol_radius) {
            if (length < builder_nudge_radius) {
                const auto angle = static_cast<uint16_t>(host.random(host.context, 0x10000));
                offset[0] = -sim::unit_movement::sine_scaled(angle, builder_patrol_radius);
                offset[1] = 0;
                offset[2] = -sim::unit_movement::cosine_scaled(angle, builder_patrol_radius);
            } else {
                const auto factor = static_cast<int32_t>(
                    (static_cast<int64_t>(0x140) << 32) / static_cast<int64_t>(length)
                );
                base::game_math::scale_vector_fixed(offset, offset, factor);
            }
            destination = {
                wrap_add(unit->position.x, offset[0]),
                wrap_add(unit->position.y, offset[1]),
                wrap_add(unit->position.z, offset[2])
            };
        }
        (void)host.order_patrol(host.context, slot, &destination, false);
    }
}

/// Sorts the player's unsorted units into squads and sets their standing orders.
///
/// Fire at will, and roam, or manoeuvre for capturing units; under the role-squad rules
/// (ai.squad-assignment), manoeuvre for commanders (OA_UNIT_DEF_ABILITY_COMMANDER).
///
/// @param state computer players and their type table
/// @param host squad assignment and world
/// @param ai the player's controller
void sort_squads(ComputerPlayers* state, const ComputerHost& host, ComputerPlayer& ai) noexcept {
    const auto& player = host.world->game.players[ai.player];
    uint32_t count = 0;
    auto* units = oa::world_player_units(host.world, &player, &count);
    if (units == nullptr)
        return;
    const auto rules = ai_rules(state).squad_assignment.rules;
    const uint32_t manoeuvre_ability = rules == data::match_rules::AiSquadAssignmentRules::base
                                           ? OA_UNIT_DEF_ABILITY_CAN_CAPTURE
                                           : OA_UNIT_DEF_ABILITY_COMMANDER;
    for (uint32_t i = 0; i < count; ++i) {
        auto& unit = units[i];
        if ((unit.flags & OA_UNIT_FLAG_SELECTABLE) == 0)
            continue;
        const auto* type = computer_type(state, unit.type_index);
        if (type == nullptr)
            continue;
        const auto move_order = (type->abilities & manoeuvre_ability) != 0 ? standing_move_manoeuvre
                                                                           : standing_move_roam;
        unit.flags = (unit.flags & ~OA_UNIT_FLAG_MOVE_ORDER_MASK) |
                     (move_order << OA_UNIT_FLAG_MOVE_ORDER_SHIFT);
        unit.flags = (unit.flags & ~OA_UNIT_FLAG_FIRE_ORDER_MASK) |
                     (standing_fire_at_will << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);
        if (unit.squad != 0)
            continue;
        const auto squad = computer_sort_squad(unit, *type, rules);
        if (squad != Squad::none)
            host.set_squad(
                host.context, static_cast<uint16_t>(oa::world_unit_slot(host.world, &unit)), squad
            );
    }
}

void run_task(
    ComputerPlayers* state, const ComputerHost& host, ComputerPlayer& ai, ComputerTask& task
) noexcept {
    switch (task.kind) {
    case TaskKind::structures:
        run_structures(state, host, ai, task);
        break;
    case TaskKind::strike:
        run_strike(state, host, ai, task);
        break;
    case TaskKind::rally:
        run_rally(host, ai, task);
        break;
    case TaskKind::construction:
        run_construction(state, host, ai, task);
        break;
    case TaskKind::air_raid:
        run_air_raid(state, host, ai, task);
        break;
    case TaskKind::siege:
        run_siege(state, host, ai, task);
        break;
    case TaskKind::none:
    case TaskKind::idle:
        break;
    }
}

} // namespace

Squad computer_sort_squad(
    const oa::Unit& unit, const ComputerType& type, data::match_rules::AiSquadAssignmentRules rules
) noexcept {
    if (rules == data::match_rules::AiSquadAssignmentRules::role_squads) {
        if ((unit.flags & OA_UNIT_FLAG_BUILDING) != 0) {
            const auto high_word = static_cast<int16_t>(float_bits(type.energy_use) >> 16);
            return (type.flags & OA_UNIT_DEF_FLAG_BUILDER) != 0 ||
                           high_word > role_squads_energy_use_high_word
                       ? Squad::structures
                       : Squad::armed_structures;
        }
        if ((type.flags & OA_UNIT_DEF_FLAG_BUILDER) != 0)
            return Squad::builders;
        if ((type.flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0)
            return Squad::aircraft;
        if (type.min_water_depth > 0 || type.max_water_depth >= role_squads_navy_max_water_depth ||
            (type.flags & OA_UNIT_DEF_FLAG_AMPHIBIOUS) != 0)
            return Squad::navy;
        return Squad::land_army;
    }
    const bool armed = (unit.flags & OA_UNIT_FLAG_HAS_WEAPONS) != 0;
    if ((unit.flags & OA_UNIT_FLAG_BUILDING) != 0)
        return armed ? Squad::armed_structures : Squad::structures;
    if ((type.flags & OA_UNIT_DEF_FLAG_BUILDER) != 0)
        return Squad::builders;
    if ((type.flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0)
        return Squad::aircraft;
    if (type.min_water_depth > 0)
        return Squad::navy;
    return armed ? Squad::land_army : Squad::none;
}

void computer_player_create(
    ComputerPlayer& ai,
    uint8_t player,
    const oa::Game& game,
    const data::match_rules::AiRules& rules
) noexcept {
    ai.present = 1;
    ai.player = player;
    ai.sort_countdown = static_cast<int32_t>(sort_interval_ticks);
    ai.commander_build_tick = 0;
    for (uint32_t i = 0; i < squad_count; ++i) {
        auto& task = ai.tasks[i];
        task = {};
        task.squad = static_cast<Squad>(i);
    }
    const auto wave = rules.attack_wave_size.units;
    ai.tasks[1].kind = TaskKind::structures;
    ai.tasks[2] = {TaskKind::strike, Squad::land_strike, 0, 3, wave, 20000, Squad::land_army, 0};
    ai.tasks[3] = {TaskKind::rally, Squad::land_army, 0, 0, 0, 0, Squad::land_strike, 0};
    ai.tasks[4].kind = TaskKind::construction;
    ai.tasks[5].kind = rules.squad5_factory_tick.enabled ? TaskKind::structures : TaskKind::idle;
    ai.tasks[6] = {TaskKind::strike, Squad::naval_strike, 0, 3, wave, 50000, Squad::navy, 0};
    ai.tasks[7] = {TaskKind::rally, Squad::navy, 0, 0, 0, 0, Squad::naval_strike, 0};
    ai.tasks[8].kind = TaskKind::air_raid;
    // The siege search starts at the map's centre, and its first step is the
    // centre's own offset.
    const oa::FixedVec3 centre{
        truncate_to_int32(static_cast<double>(game.map_width_world / 2) * 65536.0),
        0,
        truncate_to_int32(static_cast<double>(game.map_height_world / 2) * 65536.0)
    };
    auto& siege = ai.tasks[9];
    siege.kind = TaskKind::siege;
    siege.siege_target = centre;
    siege.siege_probe = centre;
    siege.siege_step = centre;
    siege.siege_weight = 0;
}

bool computer_player_refresh_strike_targets(
    ComputerPlayers* state,
    const ComputerHost& host,
    uint8_t player,
    Squad squad,
    std::span<const uint64_t> sources
) noexcept {
    if (state == nullptr || host.world == nullptr || player >= OA_PLAYER_COUNT ||
        !state->players[player].present || sources.size() != strike_refresh_mask_words ||
        host.allied == nullptr || host.order_attack == nullptr ||
        std::none_of(sources.begin(), sources.end(), [](uint64_t bits) { return bits != 0; }))
        return false;
    const auto& enhanced = state->rules.rules().setup.enhanced_mode;
    if (!enhanced.enabled ||
        !data::enhanced_mode::uses_cohort_target_distribution(enhanced.version))
        return false;
    const auto index = static_cast<uint32_t>(squad);
    if (index >= squad_count)
        return false;
    const auto& ai = state->players[player];
    const auto& task = ai.tasks[index];
    if (task.kind != TaskKind::strike || task.attacking == 0)
        return false;
    return order_strike_cohorts(state, host, ai, task, sources);
}

void computer_player_tick_orders(
    ComputerPlayers* state, const ComputerHost& host, uint8_t player
) noexcept {
    if (state == nullptr || host.world == nullptr || player >= OA_PLAYER_COUNT ||
        !state->players[player].present)
        return;
    auto& ai = state->players[player];
    if (--ai.sort_countdown < 1) {
        ai.sort_countdown = static_cast<int32_t>(sort_interval_ticks);
        if (const auto note = host.note_section)
            note(host.context, player, section_sort);
        sort_squads(state, host, ai);
    }
    for (auto& task : ai.tasks) {
        if (task.kind != TaskKind::none && task.next_tick <= tick_of(host)) {
            if (const auto note = host.note_section; note != nullptr && task.kind != TaskKind::idle)
                note(host.context, player, static_cast<uint8_t>(static_cast<uint32_t>(task.squad)));
            run_task(state, host, ai, task);
        }
    }
}

} // namespace oa::sim::ai
