// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Built-in strike refreshes continue an unchanged ground attack only under
// distributed battle rules; native goals, searches and weapon state stay owned.
#include "combat_fixture.hpp"
#include "saved_game.hpp"
#include "support/match_tick_access.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/weapon_execution/retaliation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace ai = oa::sim::ai;

constexpr uint32_t distributed_version = oa::data::enhanced_mode::distributed_battle_flow_version;
constexpr uint8_t computer = 1, enemy = 0;
constexpr uint32_t strike_squad = static_cast<uint32_t>(ai::Squad::land_strike);
constexpr int32_t pixel = 65536;
constexpr uint32_t refresh_ticks = 300;
constexpr uint8_t ground_layer = 1, land_height = 60, sea_level = 30;
constexpr uint32_t bridge_width = 64, bridge_height = 24;
constexpr uint32_t cancel_event = 2, target_lost_event = 8, attack_abort_event = 0x800;

/// Supplies durable targets and room for a small built-in strike.
Options options_for(uint32_t version, bool bridge) {
    Options options;
    options.rules.setup.enhanced_mode.enabled = version != 0;
    options.rules.setup.enhanced_mode.version = version;
    options.per_player_limit = 8;
    options.maximum_health = 30000;
    options.gun_reload_time = "10";
    options.sight_cells = bridge ? 129 : 9;
    return options;
}

/// Keeps each unit from independently choosing another weapon target.
void hold_fire(sim::unit_spawn::Slot& slot) {
    slot.record.flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
}

/// Reads the primary command's saved-facing target and creation tick.
runtime::Match::OrderRecordView head_record(runtime::Match& match, uint16_t actor) {
    runtime::Match::OrderRecordView record;
    CHECK(match.queue_records(actor, false, &record, 1) == 1);
    return record;
}

/// Holds a real built-in strike, optionally across a neck requiring fine search.
struct Battle {
    Fixture fixture;
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    uint16_t actor{}, target{}, replacement{};
    ai::ComputerPlayer* controller{};

    /// Starts a strike and waits for its actual owned chase goal.
    Battle(uint32_t version = distributed_version, bool bridge = false)
        : fixture(options_for(version, bridge)) {
        auto gun = fixture.weapons.records()[1];
        gun.range = 32;
        fixture.weapons.install(gun, {});
        fixture.match->state().game.weapon_defs[1] = gun;
        if (bridge)
            rebuild_bridge(version);
        auto& member = fixture.spawn(computer, bridge ? 48 : 24, bridge ? 208 : 112);
        actor = member.unit_index;
        hold_fire(member);
        auto& first = fixture.spawn(enemy, bridge ? 960 : 232, bridge ? 64 : 112);
        target = first.unit_index;
        hold_fire(first);
        auto& second = fixture.spawn(enemy, bridge ? 1000 : 232, bridge ? 208 : 224);
        replacement = second.unit_index;
        hold_fire(second);
        if (bridge) {
            for (const auto id : {actor, target, replacement}) {
                auto& unit = match().spawn_slot(id).record;
                match().place_unit_at(
                    id, unit.position.x, land_height * pixel, unit.position.z, ground_layer
                );
            }
            match().path_search_jobs().tick_credit = 8;
        }
        match().set_unit_squad(actor, strike_squad);
        match().simulation().players[computer].status = OA_PLAYER_STATUS_COMPUTER;
        ai::prepare_match_computer_players(match());
        controller = &ai::match_computer_players(match())->players[computer];
        controller->sort_countdown = 100000;
        for (auto& task : controller->tasks)
            task.kind = ai::TaskKind::none;
        auto& task = strike();
        task.kind = ai::TaskKind::strike;
        task.squad = ai::Squad::land_strike;
        task.source_squad = ai::Squad::land_army;
        task.min_size = 0;
        task.launch_size = 1;
        task.merge_radius = 20000;
        task.attacking = 1;
        refresh();
        CHECK(head_record(match(), actor).target == target);
        for (uint32_t tick = 0; tick < 12 && movement().navigation.goal == nullptr; ++tick)
            fixture.run(1);
        CHECK(movement().navigation.goal != nullptr);
        CHECK(unit().primary != nullptr && unit().primary->kind == attack_chase_order);
        CHECK(unit().primary->phase <= 3);
    }

    /// Returns the running match.
    runtime::Match& match() { return *fixture.match; }

    /// Returns the strike member's canonical view and primary order.
    sim::simulation_state::Unit& unit() { return *match().spawn_slot(actor).unit; }

    /// Returns the member's actual native navigator.
    ground::GroundRuntime& movement() {
        auto* ground = match().ground_runtime(actor);
        CHECK(ground != nullptr);
        return *ground;
    }

    /// Returns the built-in land strike's real task.
    ai::ComputerTask& strike() { return controller->tasks[strike_squad]; }

    /// Runs only the real controller scope, without ticking movement or searches.
    void refresh() {
        strike().next_tick = match().simulation().tick;
        match().run_player_controller(computer);
        CHECK(match().fault() == nullptr);
        CHECK(strike().next_tick == match().simulation().tick + refresh_ticks);
    }

    /// Rebuilds the native match around a FP2 neck between coarse route anchors.
    void rebuild_bridge(uint32_t version) {
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
        input.rules.setup.enhanced_mode.enabled = version != 0;
        input.rules.setup.enhanced_mode.version = version;
        input.unit_player_mask = 3;
        fixture.match = std::make_unique<runtime::Match>(input, fixture.services);
        match().configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t owner = 0; owner < 2; ++owner) {
            match().simulation().players[owner].present = true;
            match().simulation().players[owner].status = OA_PLAYER_STATUS_LOCAL;
            std::array<uint8_t, OA_PLAYER_COUNT> allies{};
            allies[owner] = 1;
            match().configure_player_alliances(owner, allies);
            if (owner == 0)
                match().configure_outcomes(owner, allies, false);
        }
    }
};

/// Keeps a refresh's owned order, navigation and weapon binding without new tick work.
void unchanged_builtin_attack_keeps_live_work() {
    Battle battle;
    auto* order = battle.unit().primary;
    auto* goal = battle.movement().navigation.goal;
    const auto navigation = battle.movement().navigation;
    const auto phase = order->phase;
    const auto issue_tick = order->issue_tick;
    auto& actor = battle.match().spawn_slot(battle.actor).record;
    auto& target = battle.match().spawn_slot(battle.target).record;
    sim::weapon_execution::aim_slot_at_unit(actor, target, 0);
    const auto weapon = actor.weapons[0];
    battle.refresh();
    CHECK(
        battle.unit().primary == order && order->phase == phase && order->issue_tick == issue_tick
    );
    const auto& after = battle.movement().navigation;
    CHECK(after.goal == goal && goal->order == order);
    CHECK(after.points == navigation.points && after.count == navigation.count);
    CHECK(after.flags == navigation.flags && after.last_search_tick == navigation.last_search_tick);
    CHECK(
        actor.weapons[0].target_a == weapon.target_a && actor.weapons[0].target_b == weapon.target_b
    );
    CHECK(actor.weapons[0].flags == weapon.flags && actor.weapons[0].reload == weapon.reload);
    CHECK(runtime::MatchTickAccess::observer_lists_valid(battle.match()));
}

/// Keeps the actual worker, its remaining budget and every native search field.
void a_pending_native_search_survives_the_refresh() {
    Battle battle(distributed_version, true);
    ground::SearchRecord before{};
    for (uint32_t tick = 0; tick < 120; ++tick) {
        before = runtime::MatchTickAccess::active_path_search(battle.match());
        if (before.unit == &battle.unit())
            break;
        battle.fixture.run(1);
    }
    CHECK(before.unit == &battle.unit());
    CHECK(before.navigation == &battle.movement().navigation && before.goal != nullptr);
    CHECK(before.goal == battle.movement().navigation.goal && before.movement_map != nullptr);
    const auto budget = battle.match().path_search_worker().budget();
    const auto start = battle.match().path_search_worker().start_cell();
    const auto credit = battle.match().path_search_jobs().credit;
    const auto picks = battle.match().path_search_jobs().job_picks;
    auto* order = battle.unit().primary;
    const auto navigation = battle.movement().navigation;
    battle.refresh();
    const auto after = runtime::MatchTickAccess::active_path_search(battle.match());
    CHECK(after.unit == before.unit && after.navigation == before.navigation);
    CHECK(after.goal == before.goal && after.movement_map == before.movement_map);
    CHECK(battle.match().path_search_worker().budget() == budget);
    CHECK(battle.match().path_search_worker().start_cell() == start);
    CHECK(battle.match().path_search_jobs().credit == credit);
    CHECK(battle.match().path_search_jobs().job_picks == picks);
    CHECK(battle.unit().primary == order && navigation.goal == after.goal);
    CHECK(battle.movement().navigation.points == navigation.points);
    CHECK(battle.movement().navigation.flags == navigation.flags);
    CHECK(battle.movement().navigation.last_search_tick == navigation.last_search_tick);
}

/// Leaves every earlier preset's periodic command replacement unchanged.
void older_presets_replace_the_attack() {
    for (uint32_t version = 0; version < distributed_version; ++version) {
        Battle battle(version);
        auto* order = battle.unit().primary;
        battle.refresh();
        CHECK(battle.unit().primary != order);
        CHECK(battle.unit().primary->phase == 0);
        CHECK(battle.movement().navigation.goal == nullptr);
    }
}

/// Replaces interrupted attacks instead of retaining a failed or terminal goal.
void interrupted_orders_replace_the_attack() {
    for (const auto event :
         {cancel_event,
          target_lost_event,
          attack_abort_event,
          ground::arrived_event,
          ground::path_failed_event,
          ground::goal_replaced_event}) {
        Battle battle;
        auto* order = battle.unit().primary;
        order->raised_events |= event;
        battle.refresh();
        CHECK(battle.unit().primary != order);
        CHECK(battle.movement().navigation.goal == nullptr);
    }
    Battle battle;
    auto* order = battle.unit().primary;
    order->phase = 4;
    battle.refresh();
    CHECK(battle.unit().primary != order);
}

/// Replaces an attack when a tail, a point or a different target changes the command.
void changed_commands_replace_the_attack() {
    {
        Battle battle;
        auto* order = battle.unit().primary;
        CHECK(!battle.match().order_refused(
            battle.match().issue_patrol(battle.actor, {80 * pixel, 0, 144 * pixel}, true)
        ));
        CHECK(order->next != nullptr);
        battle.refresh();
        CHECK(battle.unit().primary != order && battle.unit().primary->next == nullptr);
    }
    {
        Battle battle;
        CHECK(
            battle.match().issue_attack_command(battle.actor, battle.replacement, false, nullptr)
        );
        auto* order = battle.unit().primary;
        battle.refresh();
        CHECK(battle.unit().primary != order);
        CHECK(head_record(battle.match(), battle.actor).target == battle.target);
    }
    {
        Battle battle;
        const ground::Point point{200 * pixel, 0, 112 * pixel};
        CHECK(battle.match().issue_attack_command(battle.actor, battle.target, false, &point));
        auto* order = battle.unit().primary;
        battle.refresh();
        CHECK(battle.unit().primary != order);
    }
}

/// Rejects dying bindings and targets with no health before the native refresh.
void dying_targets_get_replaced() {
    for (const bool death_pending : {false, true}) {
        Battle battle;
        auto* order = battle.unit().primary;
        auto& target = battle.match().spawn_slot(battle.target).record;
        if (death_pending)
            target.flags |= OA_UNIT_FLAG_DEATH_PENDING;
        else
            target.health = 0;
        battle.refresh();
        CHECK(battle.unit().primary != order);
        CHECK(head_record(battle.match(), battle.actor).target == battle.replacement);
    }
}

/// Gives an attack from a custom seat's actual claimed turn.
struct CustomSeat {
    uint16_t actor{}, target{};
    bool issued{};

    static bool play(void* context, runtime::Match& match, uint8_t) {
        auto& seat = *static_cast<CustomSeat*>(context);
        seat.issued = match.issue_attack_command(seat.actor, seat.target, false, nullptr);
        return true;
    }
};

/// Leaves player commands and a custom controller's commands on the normal path.
void manual_and_custom_commands_replace_the_attack() {
    Battle battle;
    auto* previous = battle.unit().primary;
    CHECK(battle.match().issue_attack_command(battle.actor, battle.target, false, nullptr));
    CHECK(battle.unit().primary != previous);
    previous = battle.unit().primary;
    CustomSeat seat{battle.actor, battle.target};
    runtime::SeatHooks hooks;
    hooks.context = &seat;
    hooks.play = CustomSeat::play;
    battle.match().set_seat_hooks(computer, hooks);
    battle.match().run_player_controller(computer);
    CHECK(seat.issued && battle.unit().primary != previous);
    CHECK(battle.match().fault() == nullptr);
}

/// Continues an actual restored order and its restored owned goal on AI refresh.
void restored_orders_continue_without_a_new_schema() {
    Battle battle;
    saved_game::ScopedBank bank;
    data::persist::bank_open_account(bank.get(), "Units");
    const auto count = saved_game::write_orders(
        battle.match(), battle.match().spawn_slot(battle.actor), bank.get()
    );
    CHECK(count == 1);
    battle.match().stop_orders(battle.actor);
    saved_game::restore_orders(
        battle.match(), battle.match().spawn_slot(battle.actor), bank.get(), count
    );
    auto* order = battle.unit().primary;
    auto* goal = battle.movement().navigation.goal;
    CHECK(order != nullptr && goal != nullptr && goal->order == order);
    battle.refresh();
    CHECK(battle.unit().primary == order && battle.movement().navigation.goal == goal);
    CHECK(runtime::MatchTickAccess::observer_lists_valid(battle.match()));
}

/// Replaces a sole saved automatic attack carrying its original movement leash.
void leashed_attacks_take_the_explicit_ai_command() {
    Battle battle;

    struct Saved {
        data::persist::SavedOrder order{};
        data::persist::SavedGoal goal{};
        bool present{};
    } saved;

    battle.match().visit_saved_orders(
        battle.actor,
        [](void* context,
           const data::persist::SavedOrder* order,
           const data::persist::SavedGoal* goal) {
            auto& copy = *static_cast<Saved*>(context);
            CHECK(!copy.present && order != nullptr && goal != nullptr);
            copy.order = *order;
            copy.goal = *goal;
            copy.present = true;
        },
        &saved
    );
    CHECK(saved.present);
    saved.order.parameter_3 = 128;
    battle.match().stop_orders(battle.actor);
    auto tails = battle.match().saved_order_tails(battle.actor);
    (void)battle.match().restore_saved_order(battle.actor, saved.order, saved.goal, tails);
    battle.match().install_head_goal(battle.actor);
    auto* order = battle.unit().primary;
    CHECK(order != nullptr && order->next == nullptr);
    battle.refresh();
    CHECK(battle.unit().primary != order);
}
} // namespace

int main() {
    try {
        unchanged_builtin_attack_keeps_live_work();
        a_pending_native_search_survives_the_refresh();
        older_presets_replace_the_attack();
        interrupted_orders_replace_the_attack();
        changed_commands_replace_the_attack();
        dying_targets_get_replaced();
        manual_and_custom_commands_replace_the_attack();
        restored_orders_continue_without_a_new_schema();
        leashed_attacks_take_the_explicit_ai_command();
    } catch (const std::exception& error) {
        std::cerr << "Enhanced attack retention: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Enhanced attack retention: 9 groups passed\n";
    return 0;
}
