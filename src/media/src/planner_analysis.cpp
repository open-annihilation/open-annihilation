// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the planner reads from a timeline before it places a shot: the
// combatants and their teams, each unit's life from creation to death with
// its samples, each player's buildings and builders, the units factories
// finish, the water map summed, the first engagement, the commander death
// that decides the game, long-range fire matched from launch to impact, the
// events that weigh in hot spots and the big moments.

#include "oa/core/weapon_def.h"
#include "oa/sim/match_runtime.hpp"
#include "planner_internal.hpp"
#include "planner_tuning.hpp"

#include <algorithm>
#include <numeric>

namespace oa::media::director::planning {
namespace {

using oa::sim::match_runtime::DeathKind;

/// Unit slots a timeline names: every uint16_t value.
constexpr size_t slot_count{size_t{1} << 16};
/// Weapon flags that make a missile: launched vertically, or stockpiled.
constexpr uint32_t missile_flags{OA_WEAPON_FLAG_VLAUNCH | OA_WEAPON_FLAG_STOCKPILE};
/// The death kind of the unit a new unit displaces from its slot
/// (KillOutcome::kind 0). A replay can create a commander, displace it this
/// way with a new commander of the same player on the same tick, and play
/// on with the new one.
constexpr uint8_t displaced_death_kind{0};

/// Tells whether a death is one a battle shows: not a capture, a reclaim, a
/// dismissal or a cancelled frame.
///
/// @param kind TimelineEvent::damage_kind of a death
/// @return true for a death to show
bool battle_death(uint8_t kind) noexcept {
    return kind != static_cast<uint8_t>(DeathKind::captured) &&
           kind != static_cast<uint8_t>(DeathKind::reclaim) &&
           kind != static_cast<uint8_t>(DeathKind::dismissed) &&
           kind != static_cast<uint8_t>(DeathKind::cancelled);
}

/// Tells whether a commander's death is its replacement: it is displaced
/// from its slot, and another commander of its player is created in that
/// slot on the same tick.
///
/// @param analysis the analysis; its lives are followed
/// @param event the death, of a player below player_slots
/// @param life the life the death names
/// @return true for a commander replaced
bool commander_replaced(
    const Analysis& analysis, const TimelineEvent& event, uint32_t life
) noexcept {
    if (event.damage_kind != displaced_death_kind)
        return false;
    const std::vector<uint32_t>& commanders{analysis.commanders[event.owner]};
    return std::any_of(commanders.begin(), commanders.end(), [&](uint32_t other) {
        const Life& record{analysis.lives[other]};
        return other != life && record.slot == event.unit && record.born == event.tick;
    });
}

/// Tells whether a death is a combatant's loss of a commander: a
/// commander's life ends, owned by a player with a team, and no commander
/// of that player replaces it.
///
/// @param analysis the analysis; its lives are followed
/// @param event the death
/// @param life the life the death names, or no_index
/// @return true for a commander lost
bool commander_lost(const Analysis& analysis, const TimelineEvent& event, uint32_t life) noexcept {
    return event.kind == EventKind::death && life != no_index && analysis.lives[life].commander &&
           event.owner < player_slots && analysis.team[event.owner] != no_player &&
           !commander_replaced(analysis, event, life);
}

/// Tells whether a weapon's shots are long-range fire.
///
/// @param weapon the weapon, or null
/// @return true for a range of at least long_range_pixels, or a missile
bool long_range(const TimelineWeapon* weapon) noexcept {
    return weapon != nullptr &&
           (weapon->range >= long_range_pixels || (weapon->flags & missile_flags) != 0);
}

/// Returns a detonation's score: its weapon's area of effect times its
/// damage over blast_score_divisor.
///
/// @param weapon the weapon, or null
/// @return the score, at least 0
int64_t blast_score(const TimelineWeapon* weapon) noexcept {
    if (weapon == nullptr)
        return 0;
    const int64_t area{std::max<int64_t>(0, weapon->area_of_effect)};
    const int64_t damage{std::max<int64_t>(0, weapon->damage)};
    return area * damage / tuning::blast_score_divisor;
}

/// Tells whether a detonation is a big moment.
///
/// @param weapon the weapon, or null
/// @return true for an area of effect of at least big_blast_area, or a
///         shake with a score of at least tuning::shaking_blast_min_score
bool big_blast(const TimelineWeapon* weapon) noexcept {
    return weapon != nullptr && (weapon->area_of_effect >= big_blast_area ||
                                 (weapon->shake_magnitude > 0 &&
                                  blast_score(weapon) >= tuning::shaking_blast_min_score));
}

/// Returns a grid cell along one axis.
///
/// @param pixels the coordinate, map pixels
/// @param cells the cells along the axis, at least 1
/// @return the cell, held within [0, cells - 1]
int32_t cell_of(int32_t pixels, int32_t cells) noexcept {
    const int64_t cell{floor_div(pixels, hot_cell_pixels)};
    return static_cast<int32_t>(std::min<int64_t>(std::max<int64_t>(cell, 0), cells - 1));
}

/// Fills the lookup tables of unit types and weapons.
///
/// @param[in,out] analysis the analysis; its timeline is set
void index_types(Analysis& analysis) {
    const TimelineHeader& header{analysis.timeline->header};
    uint16_t largest{};
    for (const TimelineUnitType& type : header.unit_types)
        largest = std::max(largest, type.type_index);
    analysis.types.assign(size_t{largest} + 1u, nullptr);
    for (const TimelineUnitType& type : header.unit_types)
        if (analysis.types[type.type_index] == nullptr)
            analysis.types[type.type_index] = &type;
    uint16_t largest_weapon{};
    bool any_weapon{false};
    for (const TimelineWeapon& weapon : header.weapons) {
        if (!any_weapon || weapon.weapon_id > largest_weapon) {
            largest_weapon = weapon.weapon_id;
            any_weapon = true;
        }
    }
    analysis.weapons.assign(
        any_weapon ? static_cast<std::size_t>(largest_weapon) + 1u : 0u, nullptr
    );
    for (const TimelineWeapon& weapon : header.weapons)
        if (weapon.weapon_id < analysis.weapons.size() &&
            analysis.weapons[weapon.weapon_id] == nullptr)
            analysis.weapons[weapon.weapon_id] = &weapon;
}

/// Finds the combatants, their starts and their teams.
///
/// @param[in,out] analysis the analysis; its timeline is set
void find_combatants(Analysis& analysis) {
    const TimelineHeader& header{analysis.timeline->header};
    analysis.team.fill(no_player);
    std::array<uint16_t, player_slots> allies{};
    std::array<bool, player_slots> seen{};
    for (const TimelinePlayer& player : header.players) {
        if (player.index >= player_slots || seen[player.index])
            continue;
        seen[player.index] = true;
        if (!player.owned_units || player.watcher || player.viewer ||
            player.index == header.viewer_player)
            continue;
        analysis.combatants.push_back(player.index);
        analysis.has_start[player.index] = true;
        analysis.start[player.index] = ground_of(player.start);
        allies[player.index] = player.allies;
    }
    std::sort(analysis.combatants.begin(), analysis.combatants.end());
    for (const uint8_t player : analysis.combatants)
        analysis.team[player] = player;
    // Joins mutual allies until no team changes: each team is the smallest
    // index among the players it reaches.
    for (bool changed{true}; changed;) {
        changed = false;
        for (const uint8_t a : analysis.combatants)
            for (const uint8_t b : analysis.combatants) {
                const bool mutual{((allies[a] >> b) & 1u) != 0 && ((allies[b] >> a) & 1u) != 0};
                if (!mutual || analysis.team[a] == analysis.team[b])
                    continue;
                const uint8_t team{std::min(analysis.team[a], analysis.team[b])};
                analysis.team[a] = team;
                analysis.team[b] = team;
                changed = true;
            }
    }
}

/// Puts the events and samples before the end in tick order.
///
/// @param[in,out] analysis the analysis; its timeline and end are set
void order_records(Analysis& analysis) {
    const Timeline& timeline{*analysis.timeline};
    for (uint32_t index{}; index < timeline.events.size(); ++index)
        if (timeline.events[index].tick < analysis.end_tick)
            analysis.event_order.push_back(index);
    std::stable_sort(
        analysis.event_order.begin(), analysis.event_order.end(), [&](uint32_t a, uint32_t b) {
            return timeline.events[a].tick < timeline.events[b].tick;
        }
    );
    for (uint32_t index{}; index < timeline.samples.size(); ++index)
        if (timeline.samples[index].tick < analysis.end_tick)
            analysis.sample_order.push_back(index);
    std::stable_sort(
        analysis.sample_order.begin(), analysis.sample_order.end(), [&](uint32_t a, uint32_t b) {
            const UnitSample& first{timeline.samples[a]};
            const UnitSample& second{timeline.samples[b]};
            return first.tick != second.tick ? first.tick < second.tick : first.unit < second.unit;
        }
    );
}

/// Starts a unit's life.
///
/// @param[in,out] analysis the analysis
/// @param slot the unit's slot
/// @param unit_type its type's index
/// @param owner its owner
/// @param tick the tick it was created on
/// @param at where it was created
/// @param commander it is its side's commander
/// @return the new life's index
uint32_t start_life(
    Analysis& analysis,
    uint16_t slot,
    uint16_t unit_type,
    uint8_t owner,
    uint32_t tick,
    Point at,
    bool commander
) {
    Life life{};
    life.slot = slot;
    life.unit_type = unit_type;
    life.owner = owner;
    life.commander = commander;
    life.born = tick;
    life.created_at = at;
    life.died_at = at;
    if (const TimelineUnitType* type{type_of(analysis, unit_type)}; type != nullptr) {
        life.mobile = type->mobile;
        life.builder = type->builder;
        life.factory = !type->mobile && type->builder;
        life.armed = std::any_of(type->weapons.begin(), type->weapons.end(), [](uint16_t weapon) {
            return weapon != 0;
        });
        life.value = std::max<int64_t>(1, type->metal_cost);
        life.max_damage = std::max<int64_t>(1, type->max_damage);
        life.build_reach = int64_t{type->build_distance} + tuning::build_reach_slack_pixels;
    }
    analysis.lives.push_back(life);
    return static_cast<uint32_t>(analysis.lives.size() - 1u);
}

/// Follows every unit slot through the events and samples, in tick order,
/// giving each unit a life and each event and sample the lives it names.
///
/// A unit seen with no created event, or seen again after its death, gets
/// a life from where it was first seen.
///
/// @param[in,out] analysis the analysis; its records are ordered
void follow_lives(Analysis& analysis) {
    const Timeline& timeline{*analysis.timeline};
    std::vector<uint32_t> slot_life(slot_count, no_index);
    analysis.event_lives.assign(timeline.events.size(), EventLives{});
    analysis.sample_lives.assign(timeline.samples.size(), no_index);
    const auto live_life{[&](uint16_t slot, uint32_t tick) -> uint32_t {
        const uint32_t life{slot_life[slot]};
        return life != no_index && analysis.lives[life].died >= tick ? life : no_index;
    }};
    size_t next_event{};
    size_t next_sample{};
    while (next_event < analysis.event_order.size() || next_sample < analysis.sample_order.size()) {
        const bool sample_first{
            next_sample < analysis.sample_order.size() &&
            (next_event == analysis.event_order.size() ||
             timeline.samples[analysis.sample_order[next_sample]].tick <
                 timeline.events[analysis.event_order[next_event]].tick)
        };
        if (sample_first) {
            const uint32_t index{analysis.sample_order[next_sample++]};
            const UnitSample& sample{timeline.samples[index]};
            uint32_t life{live_life(sample.unit, sample.tick)};
            if (life == no_index || analysis.lives[life].died <= sample.tick ||
                analysis.lives[life].unit_type != sample.unit_type) {
                life = start_life(
                    analysis,
                    sample.unit,
                    sample.unit_type,
                    sample.owner,
                    sample.tick,
                    ground_of(sample.at),
                    false
                );
                slot_life[sample.unit] = life;
            }
            analysis.sample_lives[index] = life;
            continue;
        }
        const uint32_t index{analysis.event_order[next_event++]};
        const TimelineEvent& event{timeline.events[index]};
        EventLives& lives{analysis.event_lives[index]};
        const bool commander{(event.flags & event_flag::commander) != 0};
        if (event.kind == EventKind::created) {
            if (event.unit == 0)
                continue;
            lives.unit = start_life(
                analysis,
                event.unit,
                event.unit_type,
                event.owner,
                event.tick,
                ground_of(event.at),
                commander
            );
            slot_life[event.unit] = lives.unit;
            continue;
        }
        if (event.unit != 0) {
            lives.unit = live_life(event.unit, event.tick);
            const bool names_type{
                event.kind == EventKind::finished || event.kind == EventKind::damage ||
                event.kind == EventKind::death
            };
            if (lives.unit == no_index && names_type) {
                lives.unit = start_life(
                    analysis,
                    event.unit,
                    event.unit_type,
                    event.owner,
                    event.tick,
                    ground_of(event.at),
                    commander
                );
                slot_life[event.unit] = lives.unit;
            }
        }
        if (event.other_unit != 0)
            lives.other = live_life(event.other_unit, event.tick);
        if (event.kind == EventKind::death && lives.unit != no_index) {
            Life& life{analysis.lives[lives.unit]};
            if (life.died == never) {
                life.died = event.tick;
                life.died_at = ground_of(event.at);
            }
            if (commander)
                life.commander = true;
        }
    }
    // Groups the samples by life; sample_order is in tick order, so each
    // life's samples are too.
    for (const uint32_t index : analysis.sample_order)
        ++analysis.lives[analysis.sample_lives[index]].sample_count;
    uint32_t first{};
    for (Life& life : analysis.lives) {
        life.first_sample = first;
        first += life.sample_count;
        life.sample_count = 0;
    }
    analysis.life_samples.assign(first, 0);
    for (const uint32_t index : analysis.sample_order) {
        Life& life{analysis.lives[analysis.sample_lives[index]]};
        analysis.life_samples[life.first_sample + life.sample_count++] = index;
    }
    for (uint32_t life{}; life < analysis.lives.size(); ++life) {
        const Life& record{analysis.lives[life]};
        if (record.commander && record.owner < player_slots)
            analysis.commanders[record.owner].push_back(life);
    }
    for (const uint32_t index : analysis.event_order) {
        const TimelineEvent& event{timeline.events[index]};
        if (event.kind == EventKind::created && event.owner < player_slots &&
            analysis.event_lives[index].unit != no_index)
            analysis.created[event.owner].push_back(index);
    }
}

/// Lists each player's buildings and builders, and the mobile units
/// factories finished.
///
/// @param[in,out] analysis the analysis; its lives are followed
void list_builders(Analysis& analysis) {
    const Timeline& timeline{*analysis.timeline};
    for (uint32_t life{}; life < analysis.lives.size(); ++life) {
        const Life& record{analysis.lives[life]};
        if (record.owner >= player_slots)
            continue;
        if (!record.mobile)
            analysis.buildings[record.owner].push_back(life);
        else if (record.builder && !record.commander)
            analysis.builders[record.owner].push_back(life);
    }
    std::vector<uint32_t> finished(analysis.lives.size(), 0);
    for (const uint32_t index : analysis.event_order) {
        const TimelineEvent& event{timeline.events[index]};
        const EventLives& lives{analysis.event_lives[index]};
        if (event.kind != EventKind::finished || lives.unit == no_index || lives.other == no_index)
            continue;
        const Life& unit{analysis.lives[lives.unit]};
        if (!unit.mobile || !analysis.lives[lives.other].factory)
            continue;
        analysis.productions.push_back(
            Production{
                event.tick, lives.other, lives.unit, finished[lives.other]++, ground_of(event.at)
            }
        );
    }
}

/// Sums the timeline's water map, when it covers the map.
///
/// @param[in,out] analysis the analysis; its timeline is set
void sum_water(Analysis& analysis) {
    const TimelineHeader& header{analysis.timeline->header};
    const int64_t columns{header.water_columns};
    const int64_t rows{header.water_rows};
    if (columns <= 0 || rows <= 0 ||
        header.water.size() != static_cast<size_t>(columns) * static_cast<size_t>(rows))
        return;
    analysis.water_columns = columns;
    analysis.water_rows = rows;
    const size_t stride{static_cast<size_t>(columns) + 1u};
    analysis.water_sums.assign(stride * (static_cast<size_t>(rows) + 1u), 0);
    for (size_t row{}; row < static_cast<size_t>(rows); ++row)
        for (size_t column{}; column < static_cast<size_t>(columns); ++column)
            analysis.water_sums[(row + 1u) * stride + column + 1u] =
                int64_t{header.water[row * static_cast<size_t>(columns) + column]} +
                analysis.water_sums[row * stride + column + 1u] +
                analysis.water_sums[(row + 1u) * stride + column] -
                analysis.water_sums[row * stride + column];
}

/// Finds the first engagement: the first damage one combatant deals
/// another's team.
///
/// @param[in,out] analysis the analysis; its lives are followed
void find_engagement(Analysis& analysis) {
    const Timeline& timeline{*analysis.timeline};
    for (const uint32_t index : analysis.event_order) {
        const TimelineEvent& event{timeline.events[index]};
        if (event.kind == EventKind::damage && event.damage_kind != healing_kind &&
            event.amount > 0 && enemies(analysis, event.owner, event.other_owner)) {
            analysis.engagement_tick = event.tick;
            return;
        }
    }
}

/// Finds the deciding commander death: the first after which at most one
/// team has a live commander. A death that leaves two teams or more with
/// one decides nothing, so a game that ends that way, or whose usable end
/// comes before its deciding death, has none and is directed to its end.
///
/// @param[in,out] analysis the analysis; its lives are followed
void find_deciding_death(Analysis& analysis) {
    const Timeline& timeline{*analysis.timeline};
    const auto teams_with_commanders{[&](uint32_t tick) {
        std::array<bool, player_slots> live{};
        for (const uint8_t player : analysis.combatants)
            for (const uint32_t life : analysis.commanders[player])
                if (analysis.lives[life].born <= tick && analysis.lives[life].died > tick)
                    live[analysis.team[player]] = true;
        return std::count(live.begin(), live.end(), true);
    }};
    // The commanders lost, each death ending its life, in tick order.
    std::vector<uint32_t> deaths{};
    for (const uint32_t index : analysis.event_order) {
        const TimelineEvent& event{timeline.events[index]};
        const uint32_t life{analysis.event_lives[index].unit};
        if (commander_lost(analysis, event, life) && analysis.lives[life].died == event.tick)
            deaths.push_back(index);
    }
    uint32_t deciding{no_index};
    for (size_t order{}; order < deaths.size() && deciding == no_index; ++order) {
        // Every death of the tick counts before the teams are; the tick's
        // first names the deciding death.
        const size_t first{order};
        const uint32_t tick{timeline.events[deaths[first]].tick};
        while (order + 1u < deaths.size() && timeline.events[deaths[order + 1u]].tick == tick)
            ++order;
        if (teams_with_commanders(tick) <= 1)
            deciding = deaths[first];
    }
    if (deciding == no_index)
        return;
    const TimelineEvent& death{timeline.events[deciding]};
    analysis.deciding_tick = death.tick;
    analysis.deciding_at = ground_of(death.at);
    const uint32_t life{analysis.event_lives[deciding].unit};
    analysis.deciding_score = analysis.lives[life].value * tuning::commander_death_moment_factor;
}

/// A long-range shot waiting for its detonation.
struct PendingShot {
    uint32_t tick{};
    uint16_t shooter{};
    uint16_t weapon{};
    Point origin{};
};

/// Adds a moment unless one of at least its kind lies close by in time and
/// space; a closer moment of a lesser kind takes its kind and place, and
/// the long-range fire it names when it names none.
///
/// @param[in,out] moments the moments, in tick order
/// @param moment the new moment, no earlier than the last
void add_moment(std::vector<Moment>& moments, const Moment& moment) {
    constexpr int64_t reach{int64_t{tuning::moment_merge_pixels} * tuning::moment_merge_pixels};
    for (size_t index{moments.size()}; index > 0; --index) {
        Moment& kept{moments[index - 1u]};
        if (moment.tick - kept.tick > tuning::moment_merge_ticks)
            break;
        if (distance_squared(kept.at, moment.at) > reach)
            continue;
        if (moment.kind > kept.kind) {
            kept.kind = moment.kind;
            kept.at = moment.at;
        }
        if (kept.fire == no_index)
            kept.fire = moment.fire;
        kept.score = std::max(kept.score, moment.score);
        return;
    }
    moments.push_back(moment);
}

/// Scores the events that weigh in hot spots, matches long-range fire and
/// finds the moments.
///
/// @param[in,out] analysis the analysis; its lives are followed
void weigh_events(Analysis& analysis) {
    const Timeline& timeline{*analysis.timeline};
    const Stage& stage{analysis.stage};
    analysis.cells_x =
        static_cast<int32_t>(std::max<int64_t>(1, ceil_div(stage.map_width, hot_cell_pixels)));
    analysis.cells_z =
        static_cast<int32_t>(std::max<int64_t>(1, ceil_div(stage.map_height, hot_cell_pixels)));
    std::vector<PendingShot> pending{};
    std::vector<Moment> found{};
    for (const uint32_t index : analysis.event_order) {
        const TimelineEvent& event{timeline.events[index]};
        const EventLives& lives{analysis.event_lives[index]};
        const TimelineWeapon* weapon{weapon_of(analysis, event.weapon)};
        HotEvent hot{};
        hot.tick = event.tick;
        hot.at = ground_of(event.at);
        switch (event.kind) {
        case EventKind::shot:
            if (event.weapon != 0 && long_range(weapon)) {
                std::erase_if(pending, [&](const PendingShot& shot) {
                    return later(shot.tick, tuning::long_range_flight_ticks) < event.tick;
                });
                pending.push_back(PendingShot{event.tick, event.unit, event.weapon, hot.at});
            }
            continue;
        case EventKind::detonation: {
            if (event.weapon == 0)
                continue;
            if (long_range(weapon)) {
                const auto match{
                    std::find_if(pending.begin(), pending.end(), [&](const PendingShot& shot) {
                        return shot.shooter == event.unit && shot.weapon == event.weapon &&
                               later(shot.tick, tuning::long_range_flight_ticks) >= event.tick;
                    })
                };
                if (match != pending.end()) {
                    LongRangeFire fire{};
                    fire.shot_tick = match->tick;
                    fire.detonation_tick = event.tick;
                    fire.origin = match->origin;
                    fire.impact = hot.at;
                    fire.shooter = match->shooter;
                    fire.weapon = event.weapon;
                    fire.launched = (weapon->flags & missile_flags) != 0;
                    fire.score = blast_score(weapon);
                    pending.erase(match);
                    analysis.fires.push_back(fire);
                    hot.fire = static_cast<uint32_t>(analysis.fires.size() - 1u);
                }
            }
            hot.score = blast_score(weapon);
            hot.life = lives.other;
            hot.attacker = lives.unit;
            if (big_blast(weapon))
                found.push_back(Moment{event.tick, MomentKind::big_blast, hot.at, hot.score});
            break;
        }
        case EventKind::damage: {
            if (lives.unit == no_index || event.damage_kind == healing_kind ||
                event.damage_kind == paralysis_kind || event.amount <= 0 ||
                !drawn(analysis, lives.unit, event.tick))
                continue;
            const Life& life{analysis.lives[lives.unit]};
            hot.score = int64_t{event.amount} * life.value / life.max_damage;
            hot.life = lives.unit;
            hot.attacker = lives.other;
            break;
        }
        case EventKind::death: {
            if (lives.unit == no_index)
                continue;
            const Life& life{analysis.lives[lives.unit]};
            if (commander_lost(analysis, event, lives.unit))
                found.push_back(
                    Moment{
                        event.tick,
                        MomentKind::commander_death,
                        hot.at,
                        life.value * tuning::commander_death_moment_factor,
                    }
                );
            if (!battle_death(event.damage_kind) || !drawn(analysis, lives.unit, event.tick))
                continue;
            hot.score = tuning::death_score_factor * life.value;
            hot.life = lives.unit;
            hot.attacker = lives.other;
            break;
        }
        case EventKind::created:
        case EventKind::finished:
            continue;
        }
        if (hot.score <= 0)
            continue;
        hot.cell_x = cell_of(hot.at.x, analysis.cells_x);
        hot.cell_z = cell_of(hot.at.row, analysis.cells_z);
        analysis.hot_events.push_back(hot);
    }
    // Long-range fire whose launch and impact one view cannot show gives a
    // moment at each: every missile, the blast of its impact a big one, and
    // a gun's first shot landing ashore and then one at most every
    // launch_spacing_ticks, its shell landing just after.
    std::vector<uint32_t> last_launch(slot_count, never);
    for (uint32_t index{}; index < analysis.fires.size(); ++index) {
        const LongRangeFire& fire{analysis.fires[index]};
        const std::array<Point, 2> ends{fire.origin, fire.impact};
        if (framed_height(stage, ends) <= stage.fit)
            continue;
        uint32_t& last{last_launch[fire.shooter]};
        if (!fire.launched &&
            ((last != never &&
              fire.shot_tick - std::min(fire.shot_tick, last) < tuning::launch_spacing_ticks) ||
             water_at(analysis, fire.impact) > tuning::launch_water_limit_percent))
            continue;
        last = fire.shot_tick;
        const int64_t score{blast_score(weapon_of(analysis, fire.weapon))};
        found.push_back(Moment{fire.shot_tick, MomentKind::launch, fire.origin, score, index});
        found.push_back(
            Moment{
                fire.detonation_tick,
                fire.launched ? MomentKind::big_blast : MomentKind::impact,
                fire.impact,
                score,
                index,
            }
        );
    }
    std::stable_sort(found.begin(), found.end(), [](const Moment& a, const Moment& b) {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        return a.kind > b.kind;
    });
    for (const Moment& moment : found)
        add_moment(analysis.moments, moment);
}

} // namespace

Analysis analyse(const Timeline& timeline, const PlannerSettings& settings) {
    Analysis analysis{};
    analysis.timeline = &timeline;
    analysis.stage = make_stage(timeline.header, settings);
    analysis.first_tick = timeline.header.first_tick;
    analysis.end_tick = std::max(timeline.usable_end_tick, later(analysis.first_tick, 1));
    index_types(analysis);
    find_combatants(analysis);
    order_records(analysis);
    follow_lives(analysis);
    list_builders(analysis);
    sum_water(analysis);
    find_engagement(analysis);
    find_deciding_death(analysis);
    weigh_events(analysis);
    return analysis;
}

bool alive(const Life& life, uint32_t tick) noexcept {
    return life.born <= tick && tick < life.died;
}

const UnitSample* latest_sample(const Analysis& analysis, uint32_t life, uint32_t tick) noexcept {
    const Life& record{analysis.lives[life]};
    const auto first{analysis.life_samples.begin() + record.first_sample};
    const auto last{first + record.sample_count};
    const auto after{std::upper_bound(first, last, tick, [&](uint32_t wanted, uint32_t index) {
        return wanted < analysis.timeline->samples[index].tick;
    })};
    return after == first ? nullptr : &analysis.timeline->samples[*(after - 1)];
}

Point ground_point(const Analysis& analysis, uint32_t life, uint32_t tick) noexcept {
    const Life& record{analysis.lives[life]};
    if (tick >= record.died)
        return record.died_at;
    const UnitSample* sample{latest_sample(analysis, life, tick)};
    return sample != nullptr ? ground_of(sample->at) : record.created_at;
}

bool drawn(const Analysis& analysis, uint32_t life, uint32_t tick) noexcept {
    const UnitSample* sample{latest_sample(analysis, life, tick)};
    return sample == nullptr || ((sample->flags & sample_flag::visible) != 0 &&
                                 (sample->flags & sample_flag::cloaked) == 0);
}

bool framable(const Analysis& analysis, uint32_t life, uint32_t tick) noexcept {
    return alive(analysis.lives[life], tick) && drawn(analysis, life, tick);
}

bool enemies(const Analysis& analysis, uint8_t a, uint8_t b) noexcept {
    return a < player_slots && b < player_slots && analysis.team[a] != no_player &&
           analysis.team[b] != no_player && analysis.team[a] != analysis.team[b];
}

uint8_t side_of(const Analysis& analysis, Point at) noexcept {
    uint8_t side{no_player};
    int64_t nearest{};
    for (const uint8_t player : analysis.combatants) {
        if (!analysis.has_start[player])
            continue;
        const int64_t apart{distance_squared(at, analysis.start[player])};
        if (side == no_player || apart < nearest) {
            side = analysis.team[player];
            nearest = apart;
        }
    }
    return side;
}

const TimelineUnitType* type_of(const Analysis& analysis, uint16_t type_index) noexcept {
    return type_index < analysis.types.size() ? analysis.types[type_index] : nullptr;
}

const TimelineWeapon* weapon_of(const Analysis& analysis, uint16_t weapon_id) noexcept {
    return weapon_id < analysis.weapons.size() ? analysis.weapons[weapon_id] : nullptr;
}

} // namespace oa::media::director::planning
