// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Built-in rally tasks retain active distributed ground marches, including
// Patrol's native cloned return leg, while idle members still join the strike.
#include "combat_fixture.hpp"
#include "saved_game.hpp"
#include "support/match_tick_access.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/ground_orders/orders.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace ai = oa::sim::ai;

namespace enhanced = oa::data::enhanced_mode;
using FeatureSet = enhanced::FeatureSet;

/// Revision 1 without spatial cohorts: whole-strike retargeting with attack
/// and rally retention, over Revision 1's movement.
const FeatureSet whole_strike = enhanced::revision_features(1)
                                    .without(enhanced::Feature::dispatch_cohort_retention)
                                    .without(enhanced::Feature::paced_cohort_targets);
/// Strike retargeting with attack and rally retention, and no other feature
/// but capacity.
const FeatureSet retention_only = enhanced::close_features(
    FeatureSet{}
        .with(enhanced::Feature::ai_attack_retention)
        .with(enhanced::Feature::ai_rally_retention)
);
/// The sets without retention: Off, capacity alone, and strike retargeting alone.
const std::array<FeatureSet, 3> without_retention{
    {FeatureSet{},
     FeatureSet{}.with(enhanced::Feature::capacity),
     enhanced::close_features(FeatureSet{}.with(enhanced::Feature::ai_strike_retarget))}
};

/// Writes a set into a match's rules; the empty set is Off.
///
/// @param[out] mode the rules record
/// @param features the set
void store_features(oa::data::match_rules::SetupEnhancedMode& mode, const FeatureSet& features) {
    enhanced::store_choice(
        mode, features.empty() ? enhanced::Choice{} : enhanced::Choice{1, features}
    );
}

constexpr uint8_t owner = 1;
constexpr int32_t pixel = 65536;
constexpr uint32_t rally_interval = 150;
constexpr uint32_t army = static_cast<uint32_t>(ai::Squad::land_army);
constexpr uint32_t strike = static_cast<uint32_t>(ai::Squad::land_strike);
constexpr uint8_t ground_layer = 1, land_height = 60, sea_level = 30;
constexpr uint32_t bridge_width = 64, bridge_height = 24;
constexpr uint32_t cancel_event = 2;

enum class March { move, patrol, repair_patrol };

/// Supplies room and durable units for one real rally task.
Options options_for(const FeatureSet& features, bool bridge) {
    Options options;
    store_features(options.rules.setup.enhanced_mode, features);
    options.per_player_limit = 8;
    options.maximum_health = 30000;
    options.sight_cells = bridge ? 129 : 9;
    return options;
}

/// Holds an active army member, an idle member and a separate strike centroid.
struct Rally {
    Fixture fixture;
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    uint16_t actor{}, idle{}, leader{};
    ai::ComputerPlayer* controller{};

    /// Starts a genuine march before enabling only the computer's rally task.
    explicit Rally(
        const FeatureSet& features = whole_strike, March march = March::patrol, bool bridge = false
    )
        : fixture(options_for(features, bridge)) {
        if (bridge)
            rebuild_bridge(features);
        actor = fixture.spawn(owner, bridge ? 48 : 24, bridge ? 208 : 112).unit_index;
        idle = fixture.spawn(owner, bridge ? 64 : 24, bridge ? 64 : 208).unit_index;
        leader = fixture.spawn(owner, bridge ? 976 : 232, bridge ? 208 : 224).unit_index;
        for (const auto id : {actor, idle, leader}) {
            auto& record = match().spawn_slot(id).record;
            record.flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
            if (bridge)
                match().place_unit_at(
                    id, record.position.x, land_height * pixel, record.position.z, ground_layer
                );
        }
        const ground::Point destination{
            (bridge ? 960 : 200) * pixel, 0, (bridge ? 64 : 112) * pixel
        };
        sim::simulation_state::Order* issued{};
        if (march == March::move)
            issued = &match().issue_ground_move(actor, destination, false);
        else if (march == March::repair_patrol)
            issued = &match().issue_repair_patrol(actor, destination, false);
        else
            issued = &match().issue_patrol(actor, destination, false);
        CHECK(!match().order_refused(*issued));
        for (uint32_t ticks = 0; ticks < 12 && movement().navigation.goal == nullptr; ++ticks)
            fixture.run(1);
        CHECK(movement().navigation.goal != nullptr);
        CHECK(movement().navigation.goal->order == unit().primary);
        CHECK(match().spawn_slot(idle).unit->primary == nullptr);
        match().set_unit_squad(actor, army);
        match().set_unit_squad(idle, army);
        match().set_unit_squad(leader, strike);
        match().simulation().players[owner].status = OA_PLAYER_STATUS_COMPUTER;
        ai::prepare_match_computer_players(match());
        auto* players = ai::match_computer_players(match());
        ai::computer_players_adopt(players, ai::match_computer_host(match()), owner);
        controller = &players->players[owner];
        CHECK(controller->present != 0);
        controller->sort_countdown = 100000;
        for (auto& task : controller->tasks)
            task.kind = ai::TaskKind::none;
        auto& task = rally();
        task.kind = ai::TaskKind::rally;
        task.squad = ai::Squad::land_army;
        task.source_squad = ai::Squad::land_strike;
    }

    /// Returns the running match.
    runtime::Match& match() { return *fixture.match; }

    /// Returns the active army member's native unit and queue.
    sim::simulation_state::Unit& unit() { return *match().spawn_slot(actor).unit; }

    /// Returns the army member's actual ground movement and navigation.
    ground::GroundRuntime& movement() {
        auto* result = match().ground_runtime(actor);
        CHECK(result != nullptr);
        return *result;
    }

    /// Returns the only enabled task, the built-in army rally.
    ai::ComputerTask& rally() { return controller->tasks[army]; }

    /// Runs the real controller scope without ticking movement or searches.
    void refresh() {
        rally().next_tick = match().simulation().tick;
        match().run_player_controller(owner);
        CHECK(match().fault() == nullptr);
        CHECK(rally().next_tick == match().simulation().tick + rally_interval);
    }

    /// Builds an FP2 neck that requires an actual retained fine-search job.
    void rebuild_bridge(const FeatureSet& features) {
        fixture.match.reset();
        fixture.map.attribute_width = bridge_width;
        fixture.map.attribute_height = bridge_height;
        fixture.map.sea_level = sea_level;
        fixture.map.attributes.resize(bridge_width * bridge_height);
        fixture.terrain_values.resize(bridge_width * bridge_height);
        fixture.collision_plots.resize(bridge_width * bridge_height);
        fixture.altitude_cells.resize((bridge_width / 2) * (bridge_height / 2));
        const auto clear_origin = [](uint32_t x, uint32_t z) {
            return (((x >= 1 && x <= 4) || (x >= 11 && x <= bridge_width - 3)) && z >= 2 &&
                    z <= 13) ||
                   (x >= 5 && x <= 10 && z >= 5 && z <= 8);
        };
        for (uint32_t z = 0; z < bridge_height; ++z)
            for (uint32_t x = 0; x < bridge_width; ++x) {
                const bool land = clear_origin(x, z) || (x != 0 && clear_origin(x - 1, z)) ||
                                  (z != 0 && clear_origin(x, z - 1)) ||
                                  (x != 0 && z != 0 && clear_origin(x - 1, z - 1));
                fixture.map.attributes[z * bridge_width + x].height = land ? land_height : 0;
            }
        for (uint32_t z = 0; z < bridge_height; ++z)
            for (uint32_t x = 0; x < bridge_width; ++x) {
                auto& plot = fixture.collision_plots[z * bridge_width + x];
                plot.low_height = plot.high_height =
                    fixture.map.attributes[z * bridge_width + x].height;
                for (uint32_t dz = 0; dz < 2 && z + dz < bridge_height; ++dz)
                    for (uint32_t dx = 0; dx < 2 && x + dx < bridge_width; ++dx) {
                        const auto height =
                            fixture.map.attributes[(z + dz) * bridge_width + x + dx].height;
                        plot.low_height = std::min(plot.low_height, height);
                        plot.high_height = std::max(plot.high_height, height);
                    }
            }
        fixture.types[1].footprint_x = fixture.types[1].footprint_z = 2;
        fixture.loaded[1].type = fixture.types[1];
        fixture.fields[1].movement_class = 1;
        fixture.fields[1].yard_mask = yard;
        fixture.metadata.footprint_x = fixture.metadata.footprint_z = 2;
        fixture.metadata.min_water_depth = -10000;
        fixture.metadata.max_water_depth = 0;
        fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
        fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
        runtime::OfflineInputs input{
            fixture.map,
            fixture.loaded,
            fixture.types,
            fixture.fields,
            fixture.weapons,
            fixture.terrain_values,
            fixture.masks,
            bridge_width / 2,
            bridge_height / 2,
            8,
            2,
            0,
            sea_level,
            1,
            &fixture.scenario,
            {},
            fixture.collision_plots,
            sim::visibility_state::AltitudeSightData{
                bridge_width / 2,
                bridge_height / 2,
                fixture.altitude_cells,
                fixture.altitude_patterns
            }
        };
        store_features(input.rules.setup.enhanced_mode, features);
        input.unit_player_mask = 3;
        fixture.match = std::make_unique<runtime::Match>(input, fixture.services);
        match().configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t player = 0; player < 2; ++player) {
            match().simulation().players[player].present = true;
            match().simulation().players[player].status = OA_PLAYER_STATUS_LOCAL;
            std::array<uint8_t, OA_PLAYER_COUNT> allies{};
            allies[player] = 1;
            match().configure_player_alliances(player, allies);
            if (player == 0)
                match().configure_outcomes(player, allies, false);
        }
        match().path_search_jobs().tick_credit = 8;
    }
};

/// Checks the complete native patrol queue and navigation, not just its head kind.
void an_active_two_leg_patrol_survives_while_idle_members_rally(const FeatureSet& features) {
    Rally battle(features);
    auto* head = battle.unit().primary;
    auto* tail = head->next;
    CHECK(tail != nullptr && tail->next == nullptr);
    const auto navigation = battle.movement().navigation;
    const auto issue_tick = head->issue_tick;
    const auto tail_issue_tick = tail->issue_tick;
    const auto phase = head->phase;
    const auto wake_tick = head->wake_tick;
    battle.refresh();
    CHECK(battle.unit().primary == head && head->next == tail && tail->next == nullptr);
    CHECK(head->issue_tick == issue_tick && tail->issue_tick == tail_issue_tick);
    CHECK(head->phase == phase && head->wake_tick == wake_tick);
    const auto& after = battle.movement().navigation;
    CHECK(after.goal == navigation.goal && after.goal->order == head);
    CHECK(after.points == navigation.points && after.count == navigation.count);
    CHECK(after.flags == navigation.flags && after.last_search_tick == navigation.last_search_tick);
    std::array<runtime::Match::OrderRecordView, 2> idle_orders{};
    CHECK(
        battle.match().queue_records(battle.idle, false, idle_orders.data(), idle_orders.size()) ==
        1
    );
    CHECK(idle_orders[0].kind == ground::move_ground_kind);
    const auto& centroid = battle.match().spawn_slot(battle.leader).record.position;
    CHECK(idle_orders[0].point == ground::Point({centroid.x, centroid.y, centroid.z}));
    CHECK(runtime::MatchTickAccess::observer_lists_valid(battle.match()));
}

/// Keeps a native patrol's owned goal while its idle wait has selected phase one.
void a_native_phase_one_patrol_wait_survives_the_rally(const FeatureSet& features) {
    Rally battle(features);
    battle.rally().next_tick = battle.match().simulation().tick + 100000;
    bool waiting = false;
    for (uint32_t ticks = 0; ticks < 90; ++ticks) {
        const auto* head = battle.unit().primary;
        const auto* goal = battle.movement().navigation.goal;
        if (head != nullptr && head->phase == 1 && goal != nullptr && goal->order == head &&
            !ground::goal_contains(
                *goal, battle.unit().record.cell_x, battle.unit().record.cell_z
            )) {
            waiting = true;
            break;
        }
        battle.fixture.run(1);
    }
    CHECK(waiting);
    auto* head = battle.unit().primary;
    auto* tail = head->next;
    CHECK(tail != nullptr && tail->next == nullptr);
    const auto navigation = battle.movement().navigation;
    const auto issue_tick = head->issue_tick;
    const auto tail_issue_tick = tail->issue_tick;
    const auto wake_tick = head->wake_tick;
    CHECK(
        (head->raised_events & (ground::arrived_event | ground::path_failed_event |
                                ground::goal_replaced_event | cancel_event)) == 0
    );
    battle.refresh();
    CHECK(battle.unit().primary == head && head->next == tail && tail->next == nullptr);
    CHECK(head->phase == 1 && head->wake_tick == wake_tick && head->issue_tick == issue_tick);
    CHECK(tail->issue_tick == tail_issue_tick);
    const auto& after = battle.movement().navigation;
    CHECK(after.goal == navigation.goal && after.goal->order == head);
    CHECK(after.points == navigation.points && after.count == navigation.count);
    CHECK(after.flags == navigation.flags && after.last_search_tick == navigation.last_search_tick);
    std::array<runtime::Match::OrderRecordView, 2> idle_orders{};
    CHECK(
        battle.match().queue_records(battle.idle, false, idle_orders.data(), idle_orders.size()) ==
        1
    );
    CHECK(idle_orders[0].kind == ground::move_ground_kind);
    const auto& centroid = battle.match().spawn_slot(battle.leader).record.position;
    CHECK(idle_orders[0].point == ground::Point({centroid.x, centroid.y, centroid.z}));
}

/// Retains the other supported active ground marches through the same real rally.
void move_and_repair_patrol_continue(const FeatureSet& features) {
    for (const auto march : {March::move, March::repair_patrol}) {
        Rally battle(features, march);
        auto* head = battle.unit().primary;
        const auto navigation = battle.movement().navigation;
        battle.refresh();
        CHECK(battle.unit().primary == head);
        CHECK(battle.movement().navigation.goal == navigation.goal);
        CHECK(battle.movement().navigation.points == navigation.points);
        CHECK(battle.movement().navigation.flags == navigation.flags);
    }
}

/// Keeps the actual fine-search worker, owned goal, cursor and remaining credits.
void a_pending_native_search_survives_the_rally(const FeatureSet& features) {
    Rally battle(features, March::patrol, true);
    ground::SearchRecord before{};
    for (uint32_t tick = 0; tick < 120; ++tick) {
        before = runtime::MatchTickAccess::active_path_search(battle.match());
        if (before.unit == &battle.unit())
            break;
        battle.fixture.run(1);
    }
    CHECK(before.unit == &battle.unit());
    CHECK(before.navigation == &battle.movement().navigation);
    CHECK(before.goal == battle.movement().navigation.goal && before.movement_map != nullptr);
    const auto budget = battle.match().path_search_worker().budget();
    const auto start = battle.match().path_search_worker().start_cell();
    const auto credit = battle.match().path_search_jobs().credit;
    const auto picks = battle.match().path_search_jobs().job_picks;
    auto* head = battle.unit().primary;
    auto* tail = head->next;
    const auto navigation = battle.movement().navigation;
    battle.refresh();
    const auto after = runtime::MatchTickAccess::active_path_search(battle.match());
    CHECK(after.unit == before.unit && after.navigation == before.navigation);
    CHECK(after.goal == before.goal && after.movement_map == before.movement_map);
    CHECK(battle.match().path_search_worker().budget() == budget);
    CHECK(battle.match().path_search_worker().start_cell() == start);
    CHECK(battle.match().path_search_jobs().credit == credit);
    CHECK(battle.match().path_search_jobs().job_picks == picks);
    CHECK(battle.unit().primary == head && head->next == tail);
    CHECK(battle.movement().navigation.points == navigation.points);
    CHECK(battle.movement().navigation.flags == navigation.flags);
}

/// Leaves the ordinary whole-queue replacement of every older preset unchanged.
void older_presets_keep_their_rally_replacement() {
    for (const auto& features : without_retention) {
        Rally battle(features);
        auto* head = battle.unit().primary;
        battle.refresh();
        CHECK(battle.unit().primary != head);
        CHECK(battle.unit().primary->kind == ground::move_ground_kind);
        CHECK(battle.unit().primary->next == nullptr);
        CHECK(battle.movement().navigation.goal == nullptr);
    }
}

/// Replaces interrupted, completed, absent or mismatched goal bindings normally.
void terminal_or_unowned_work_does_not_block_a_rally(const FeatureSet& features) {
    for (const auto event :
         {cancel_event,
          ground::arrived_event,
          ground::path_failed_event,
          ground::goal_replaced_event}) {
        Rally battle(features);
        auto* head = battle.unit().primary;
        head->raised_events |= event;
        battle.refresh();
        CHECK(battle.unit().primary != head);
        CHECK(battle.unit().primary->kind == ground::move_ground_kind);
    }
    {
        Rally battle(features);
        auto* head = battle.unit().primary;
        battle.movement().navigation.goal = nullptr;
        battle.refresh();
        CHECK(battle.unit().primary != head);
    }
    {
        Rally battle(features);
        auto* head = battle.unit().primary;
        const_cast<ground::Goal*>(battle.movement().navigation.goal)->order = head->next;
        battle.refresh();
        CHECK(battle.unit().primary != head);
    }
    {
        Rally battle(features);
        auto* head = battle.unit().primary;
        head->kind = ground::standby_kind;
        battle.refresh();
        CHECK(battle.unit().primary != head);
    }
}

/// Sends a move from a custom controller's actual claimed scope.
struct CustomSeat {
    uint16_t actor{};
    bool issued{};

    /// Replaces a march as a custom brain, not the built-in rally.
    static bool play(void* context, runtime::Match& match, uint8_t) {
        auto& seat = *static_cast<CustomSeat*>(context);
        seat.issued = !match.order_refused(
            match.issue_ground_move(seat.actor, {96 * pixel, 0, 96 * pixel}, false)
        );
        return true;
    }
};

/// Keeps explicit moves, queued moves and custom controllers on their native paths.
void manual_queued_and_custom_commands_keep_their_semantics(const FeatureSet& features) {
    {
        Rally battle(features);
        auto* head = battle.unit().primary;
        CHECK(!battle.match().order_refused(
            battle.match().issue_ground_move(battle.actor, {96 * pixel, 0, 96 * pixel}, false)
        ));
        CHECK(battle.unit().primary != head && battle.unit().primary->next == nullptr);
    }
    {
        Rally battle(features);
        auto* head = battle.unit().primary;
        auto* tail = head->next;
        auto& appended =
            battle.match().issue_ground_move(battle.actor, {96 * pixel, 0, 96 * pixel}, true);
        CHECK(!battle.match().order_refused(appended));
        CHECK(battle.unit().primary == head && head->next != nullptr);
        const std::array topology{head, head->next, head->next->next};
        CHECK(topology[2] != nullptr && topology[2]->next == nullptr);
        CHECK(std::find(topology.begin(), topology.end(), tail) != topology.end());
        CHECK(std::find(topology.begin(), topology.end(), &appended) != topology.end());
        std::array<runtime::Match::OrderRecordView, 3> before{};
        CHECK(battle.match().queue_records(battle.actor, false, before.data(), before.size()) == 3);
        battle.refresh();
        CHECK(battle.unit().primary == topology[0]);
        CHECK(topology[0]->next == topology[1] && topology[1]->next == topology[2]);
        CHECK(topology[2]->next == nullptr);
        std::array<runtime::Match::OrderRecordView, 3> after{};
        CHECK(battle.match().queue_records(battle.actor, false, after.data(), after.size()) == 3);
        for (std::size_t index = 0; index < before.size(); ++index) {
            CHECK(after[index].kind == before[index].kind);
            CHECK(after[index].point == before[index].point);
            CHECK(after[index].target == before[index].target);
            CHECK(after[index].issue_tick == before[index].issue_tick);
            CHECK(after[index].phase == before[index].phase);
            CHECK(after[index].preserve_flags == before[index].preserve_flags);
            CHECK(after[index].command_flags == before[index].command_flags);
            CHECK(after[index].flags == before[index].flags);
        }
    }
    {
        Rally battle(features);
        auto* head = battle.unit().primary;
        CustomSeat seat{battle.actor};
        runtime::SeatHooks hooks;
        hooks.context = &seat;
        hooks.play = CustomSeat::play;
        battle.match().set_seat_hooks(owner, hooks);
        battle.match().run_player_controller(owner);
        CHECK(seat.issued && battle.unit().primary != head);
        CHECK(battle.match().fault() == nullptr);
    }
}

/// Retains both restored patrol legs and their restored owned head goal.
void restored_patrol_queues_continue_after_the_rally(const FeatureSet& features) {
    Rally battle(features);
    saved_game::ScopedBank bank;
    data::persist::bank_open_account(bank.get(), "Units");
    const auto count = saved_game::write_orders(
        battle.match(), battle.match().spawn_slot(battle.actor), bank.get()
    );
    CHECK(count == 2);
    battle.match().stop_orders(battle.actor);
    saved_game::restore_orders(
        battle.match(), battle.match().spawn_slot(battle.actor), bank.get(), count
    );
    auto* head = battle.unit().primary;
    auto* tail = head->next;
    const auto* goal = battle.movement().navigation.goal;
    CHECK(tail != nullptr && tail->next == nullptr && goal != nullptr && goal->order == head);
    battle.refresh();
    CHECK(battle.unit().primary == head && head->next == tail);
    CHECK(battle.movement().navigation.goal == goal);
    CHECK(runtime::MatchTickAccess::observer_lists_valid(battle.match()));
}
} // namespace

int main() {
    try {
        older_presets_keep_their_rally_replacement();
        for (const auto& features : {retention_only, whole_strike}) {
            std::cout << "Enhanced rally retention: "
                      << (features == retention_only ? "retention alone" : "whole strikes") << '\n';
            an_active_two_leg_patrol_survives_while_idle_members_rally(features);
            a_native_phase_one_patrol_wait_survives_the_rally(features);
            move_and_repair_patrol_continue(features);
            a_pending_native_search_survives_the_rally(features);
            terminal_or_unowned_work_does_not_block_a_rally(features);
            manual_queued_and_custom_commands_keep_their_semantics(features);
            restored_patrol_queues_continue_after_the_rally(features);
        }
        std::cout << "Enhanced rally retention: 15 groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Enhanced rally retention: " << error.what() << '\n';
        return 1;
    }
}
