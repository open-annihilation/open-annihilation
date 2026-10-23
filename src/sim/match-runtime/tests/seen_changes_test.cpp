// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seen_changed hook reports the enemies a player's seen list gained and
// lost at the end of a knowledge refresh. A slot a new unit has taken is one
// lost entry and one gained entry, told apart by generation.
#include "combat_fixture.hpp"

#include "oa/sim/detection.hpp"
#include "oa/sim/match_runtime/event_hooks.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <vector>

namespace {

using namespace combat_fixture;
namespace detection = oa::sim::detection;
using sim::match_runtime::EventHooks;

/// One seen_changed report, with the lists copied out of the call.
struct SeenReport {
    uint8_t player{};
    std::vector<detection::SightedUnit> gained;
    std::vector<detection::SightedUnit> lost;
};

/// Keeps every seen_changed report.
struct Recorder {
    std::vector<SeenReport> reports;

    /// Returns hooks that record seen_changed into this recorder.
    ///
    /// @return the table, with this recorder as the context
    EventHooks hooks() {
        EventHooks table;
        table.context = this;
        table.seen_changed = [](void* context,
                                const World&,
                                uint8_t player,
                                std::span<const detection::SightedUnit> gained,
                                std::span<const detection::SightedUnit> lost) {
            static_cast<Recorder*>(context)->reports.push_back(
                SeenReport{player, {gained.begin(), gained.end()}, {lost.begin(), lost.end()}}
            );
        };
        return table;
    }
};

/// Returns the options of a match whose sight covers the cells round a unit.
///
/// @return sight three cells across, centred on the unit
Options sight_options() {
    Options options;
    options.sight_cells = 3;
    return options;
}

/// Creates a finished unit that holds its fire.
///
/// @param f the fixture
/// @param player owner
/// @param x whole world units
/// @param z whole world units
/// @return the unit's slot
sim::unit_spawn::Slot& idle(Fixture& f, uint8_t player, uint32_t x, uint32_t z) {
    auto& slot = f.spawn(player, x, z);
    slot.unit->flags &= ~(OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK);
    return slot;
}

/// Returns the generation of the unit in a slot.
///
/// @param f the fixture
/// @param unit_slot the unit's slot
/// @return its instance generation
uint32_t unit_generation(const Fixture& f, uint16_t unit_slot) {
    return f.match->runtime_state(unit_slot).instance_generation;
}

/// Returns the reports for one player, in the order they were made.
///
/// @param recorder the recording
/// @param player the player
/// @return that player's reports
std::vector<SeenReport> for_player(const Recorder& recorder, uint8_t player) {
    std::vector<SeenReport> kept;
    for (const auto& report : recorder.reports)
        if (report.player == player)
            kept.push_back(report);
    return kept;
}

/// Tells whether two sighted lists name the same slots and generations.
///
/// @param left one list
/// @param right the other list
/// @return true when they match entry for entry
bool same_units(
    const std::vector<detection::SightedUnit>& left,
    const std::vector<detection::SightedUnit>& right
) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (left[index].unit != right[index].unit ||
            left[index].generation != right[index].generation)
            return false;
    return true;
}

/// Tells whether a list names a slot.
///
/// @param list the sighted units
/// @param unit the slot
/// @return true when one entry has the slot
bool has_unit(const std::vector<detection::SightedUnit>& list, uint16_t unit) {
    for (const auto& entry : list)
        if (entry.unit == unit)
            return true;
    return false;
}

/// Tells whether a list holds one slot at one generation.
///
/// @param list the sighted units
/// @param unit the slot
/// @param seen_generation the generation
/// @return true when that entry is present
bool has_entry(
    const std::vector<detection::SightedUnit>& list, uint16_t unit, uint32_t seen_generation
) {
    for (const auto& entry : list)
        if (entry.unit == unit && entry.generation == seen_generation)
            return true;
    return false;
}

// An enemy in sight is gained at the first refresh. One out of sight is not,
// and the player's own unit and an allied unit never appear.
void enemy_in_sight_is_gained_and_the_others_are_not() {
    Fixture f(sight_options());
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    allies[2] = 1;
    f.match->configure_player_alliances(0, allies);
    auto& own = idle(f, 0, 64, 64);
    auto& near = idle(f, 1, 96, 64);
    auto& ally_unit = idle(f, 2, 64, 96);
    auto& far = idle(f, 1, 240, 240);
    Recorder recorder;
    f.match->event_hooks = recorder.hooks();
    f.run(29);
    CHECK(recorder.reports.empty());
    CHECK(f.match->sightings(0).refreshed_tick == 0);
    f.run(1);
    CHECK(f.match->unit_visible(0, near.unit_index));
    CHECK(f.match->unit_visible(0, ally_unit.unit_index));
    CHECK(!f.match->unit_visible(0, far.unit_index));
    CHECK(f.match->sightings(0).refreshed_tick == 30);
    CHECK(f.match->sightings(0).seen_count == 1);
    CHECK(f.match->sightings(0).seen[0] == near.unit_index);
    const auto reports = for_player(recorder, 0);
    CHECK(reports.size() == 1);
    CHECK(reports[0].lost.empty());
    CHECK(reports[0].gained.size() == 1);
    CHECK(reports[0].gained[0].unit == near.unit_index);
    CHECK(reports[0].gained[0].generation == unit_generation(f, near.unit_index));
    CHECK(!has_unit(reports[0].gained, own.unit_index));
    CHECK(!has_unit(reports[0].gained, ally_unit.unit_index));
    CHECK(!has_unit(reports[0].gained, far.unit_index));
    std::cout << "enemy in sight is gained passed\n";
}

// Two refreshes with the same enemies report nothing the second time.
void unchanged_refresh_reports_nothing() {
    Fixture f(sight_options());
    idle(f, 0, 64, 64);
    idle(f, 1, 96, 64);
    Recorder recorder;
    f.match->event_hooks = recorder.hooks();
    f.run(30);
    CHECK(!recorder.reports.empty());
    recorder.reports.clear();
    f.run(30);
    CHECK(recorder.reports.empty());
    CHECK(f.match->sightings(0).refreshed_tick == 60);
    std::cout << "unchanged refresh reports nothing passed\n";
}

// An enemy killed in sight is lost at the next refresh, still under the
// generation it had while it was seen.
void enemy_killed_in_sight_is_lost() {
    Fixture f(sight_options());
    auto& own = idle(f, 0, 64, 64);
    auto& near = idle(f, 1, 96, 64);
    Recorder recorder;
    f.match->event_hooks = recorder.hooks();
    f.run(30);
    const auto slot = near.unit_index;
    const auto old_generation = unit_generation(f, slot);
    recorder.reports.clear();
    f.match->apply_kill(slot, {sim::match_runtime::DeathKind::dismissed, 0, 0}, own.unit_index, 0);
    f.run(30);
    const auto reports = for_player(recorder, 0);
    CHECK(reports.size() == 1);
    CHECK(reports[0].gained.empty());
    CHECK(reports[0].lost.size() == 1);
    CHECK(reports[0].lost[0].unit == slot);
    CHECK(reports[0].lost[0].generation == old_generation);
    CHECK(f.match->sightings(0).seen_count == 0);
    std::cout << "enemy killed in sight is lost passed\n";
}

// A new enemy made in the dead unit's slot before the next refresh is lost
// under the old generation and gained under the new one, in one report.
void reused_slot_is_lost_and_gained() {
    Fixture f(sight_options());
    auto& own = idle(f, 0, 64, 64);
    auto& near = idle(f, 1, 96, 64);
    Recorder recorder;
    f.match->event_hooks = recorder.hooks();
    f.run(30);
    const auto slot = near.unit_index;
    const auto old_generation = unit_generation(f, slot);
    recorder.reports.clear();
    f.match->apply_kill(slot, {sim::match_runtime::DeathKind::dismissed, 0, 0}, own.unit_index, 0);
    const sim::unit_spawn::Request request{
        1, 1, {96u << 16, 32u << 16, 64u << 16}, true, sim::unit_spawn::ground_occupancy_state, slot
    };
    auto* created = f.match->create(request);
    CHECK(created != nullptr && created->unit_index == slot);
    created->unit->flags &= ~(OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK);
    const auto new_generation = unit_generation(f, slot);
    CHECK(new_generation != old_generation);
    f.run(30);
    const auto reports = for_player(recorder, 0);
    CHECK(reports.size() == 1);
    CHECK(reports[0].lost.size() == 1);
    CHECK(reports[0].gained.size() == 1);
    CHECK(reports[0].lost[0].unit == slot && reports[0].lost[0].generation == old_generation);
    CHECK(reports[0].gained[0].unit == slot && reports[0].gained[0].generation == new_generation);
    std::cout << "reused slot is lost and gained passed\n";
}

// An enemy the player's row comes to ally, and one marked off the seen
// lists, are both lost at the next refresh.
void ally_and_unsighted_enemies_are_lost() {
    Fixture f(sight_options());
    idle(f, 0, 64, 64);
    auto& will_ally = idle(f, 1, 96, 64);
    auto& will_hide = idle(f, 3, 64, 96);
    Recorder recorder;
    f.match->event_hooks = recorder.hooks();
    f.run(30);
    const auto ally_slot = will_ally.unit_index;
    const auto hide_slot = will_hide.unit_index;
    const auto ally_generation = unit_generation(f, ally_slot);
    const auto hide_generation = unit_generation(f, hide_slot);
    CHECK(f.match->sightings(0).seen_count == 2);
    recorder.reports.clear();
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    allies[1] = 1;
    f.match->configure_player_alliances(0, allies);
    will_hide.unit->flags |= detection::unsighted;
    f.run(30);
    const auto reports = for_player(recorder, 0);
    CHECK(reports.size() == 1);
    CHECK(reports[0].gained.empty());
    CHECK(reports[0].lost.size() == 2);
    CHECK(reports[0].lost[0].unit < reports[0].lost[1].unit);
    CHECK(has_entry(reports[0].lost, ally_slot, ally_generation));
    CHECK(has_entry(reports[0].lost, hide_slot, hide_generation));
    std::cout << "ally and unsighted enemies are lost passed\n";
}

// The primary table and a listener are given the same lists.
void primary_and_listener_receive_the_same_lists() {
    Fixture f(sight_options());
    idle(f, 0, 64, 64);
    idle(f, 1, 96, 64);
    Recorder primary;
    Recorder listener;
    f.match->event_hooks = primary.hooks();
    CHECK(f.match->add_event_listener(listener.hooks()) != 0);
    f.run(30);
    CHECK(!primary.reports.empty());
    CHECK(primary.reports.size() == listener.reports.size());
    for (std::size_t index = 0; index < primary.reports.size(); ++index) {
        CHECK(primary.reports[index].player == listener.reports[index].player);
        CHECK(same_units(primary.reports[index].gained, listener.reports[index].gained));
        CHECK(same_units(primary.reports[index].lost, listener.reports[index].lost));
    }
    std::cout << "primary and listener receive the same lists passed\n";
}

// A listener added after a refresh hears nothing while the list stays as it
// was, and then only the enemy that actually arrived.
void listener_added_mid_match_hears_only_changes() {
    Fixture f(sight_options());
    idle(f, 0, 64, 64);
    auto& near = idle(f, 1, 96, 64);
    f.run(30);
    CHECK(f.match->sightings(0).seen_count == 1);
    Recorder recorder;
    CHECK(f.match->add_event_listener(recorder.hooks()) != 0);
    f.run(30);
    CHECK(recorder.reports.empty());
    auto& later = idle(f, 1, 96, 96);
    f.run(30);
    const auto reports = for_player(recorder, 0);
    CHECK(reports.size() == 1);
    CHECK(reports[0].lost.empty());
    CHECK(reports[0].gained.size() == 1);
    CHECK(reports[0].gained[0].unit == later.unit_index);
    CHECK(reports[0].gained[0].generation == unit_generation(f, later.unit_index));
    CHECK(!has_unit(reports[0].gained, near.unit_index));
    std::cout << "listener added mid-match hears only changes passed\n";
}

} // namespace

int main() {
    try {
        enemy_in_sight_is_gained_and_the_others_are_not();
        unchanged_refresh_reports_nothing();
        enemy_killed_in_sight_is_lost();
        reused_slot_is_lost_and_gained();
        ally_and_unsighted_enemies_are_lost();
        primary_and_listener_receive_the_same_lists();
        listener_added_mid_match_hears_only_changes();
    } catch (const std::exception& error) {
        std::cerr << "seen changes: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
