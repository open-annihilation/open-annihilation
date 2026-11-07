// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Orders written to a savegame and restored into another match: the queues,
// the words each mission's handler keeps, the target links and the head
// order's goal.
#include "combat_fixture.hpp"
#include "saved_game.hpp"

#include "oa/data/persist/save_orders.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace combat_fixture;
using saved_game::restore_orders;
using saved_game::ScopedBank;
using saved_game::write_orders;

namespace {

constexpr uint8_t building_build = 12, build_weapon = 13, follow_ground = 18, move_ground = 26,
                  patrol = 29;
// Order flags bit: the order saw its target unit and kept the cell in seen_cell.
constexpr uint8_t order_target_seen = 0x20;

sim::ground_orders::Point point(int32_t x, int32_t z) {
    return {x << 16, 0, z << 16};
}

// Each order's blob and its goal's.
struct Exported {
    std::vector<std::vector<uint8_t>> blobs;
    std::vector<std::vector<uint8_t>> goals;
};

Exported exported(sim::match_runtime::Match& match, uint16_t unit) {
    Exported out;
    match.visit_saved_orders(
        unit,
        [](void* walk,
           const data::persist::SavedOrder* order,
           const data::persist::SavedGoal* goal) {
            auto& into = *static_cast<Exported*>(walk);
            std::vector<uint8_t> blob(data::persist::order_blob_bytes);
            data::persist::save_encode_order(order, blob.data());
            into.blobs.push_back(blob);
            std::vector<uint8_t> goal_blob(data::persist::goal_blob_max_bytes);
            goal_blob.resize(data::persist::save_encode_goal(goal, goal_blob.data()));
            into.goals.push_back(goal_blob);
        },
        &out
    );
    return out;
}

bool same(const Exported& a, const Exported& b) {
    return a.blobs == b.blobs && a.goals == b.goals;
}

uint16_t saved_air_target(sim::match_runtime::Match& match, uint16_t owner) {
    uint16_t target = 0;
    match.visit_saved_orders(
        owner,
        [](void* walk, const data::persist::SavedOrder*, const data::persist::SavedGoal* goal) {
            CHECK(goal->kind == static_cast<int32_t>(data::persist::SavedGoalKind::air_target));
            *static_cast<uint16_t*>(walk) = goal->air_target.target_id;
        },
        &target
    );
    return target;
}

std::vector<uint8_t> kinds(const sim::simulation_state::Order* head) {
    std::vector<uint8_t> out;
    for (auto* order = head; order != nullptr && out.size() < 16; order = order->next)
        out.push_back(order->kind);
    return out;
}

void orders_survive_a_save() {
    Fixture saving;
    auto& mover = saving.spawn(0, 40, 40);
    auto& yard = saving.spawn(0, 200, 200);
    saving.match->issue_ground_move(mover.unit_index, point(200, 40), false);
    (void)saving.match->issue_patrol(mover.unit_index, point(40, 200), true);
    (void)saving.match->issue_guard(mover.unit_index, yard.unit_index, true);
    // The move takes its goal as it runs.
    saving.run(3);
    CHECK(head_is(mover, move_ground));
    CHECK(saving.match->ground_runtime(mover.unit_index)->navigation.goal != nullptr);
    (void)saving.match->issue_building_build(yard.unit_index, 1, 2, false);
    // A second request for the type adds to the queued count (the second parameter).
    saving.match->queue_factory_build(yard.unit_index, 1, 3);
    (void)saving.match->issue_build_weapon(yard.unit_index, 0, 4);
    // The overlays saw the guarded yard's cell (seen_x and seen_z, order flags bit 0x20).
    saving.match->note_order_target_seen(mover.unit_index, 2, 200, 200);

    const auto mover_orders = exported(*saving.match, mover.unit_index);
    const auto yard_orders = exported(*saving.match, yard.unit_index);
    CHECK(mover_orders.blobs.size() == 3 && yard_orders.blobs.size() == 2);
    data::persist::SavedOrder first, guard, build, weapon;
    data::persist::save_decode_order(mover_orders.blobs[0].data(), &first);
    data::persist::save_decode_order(mover_orders.blobs[2].data(), &guard);
    data::persist::save_decode_order(yard_orders.blobs[0].data(), &build);
    data::persist::save_decode_order(yard_orders.blobs[1].data(), &weapon);
    CHECK(first.kind == move_ground && first.point[0] == 200 << 16 && first.point[2] == 40 << 16);
    CHECK(first.goal_kind == static_cast<int32_t>(data::persist::SavedGoalKind::circle));
    CHECK(
        mover_orders.goals[0].size() == data::persist::circle_goal_blob_bytes &&
        mover_orders.goals[1].empty()
    );
    CHECK(guard.kind == follow_ground && guard.target_id == yard.record.id);
    CHECK(guard.seen_cell == (200u | 200u << 16) && (guard.flags & order_target_seen) != 0);
    CHECK(first.seen_cell == 0 && (first.flags & order_target_seen) == 0);
    CHECK(build.kind == building_build && build.parameter_1 == 1 && build.parameter_2 == 5);
    CHECK(weapon.kind == build_weapon && (weapon.flags & 4) != 0 && weapon.parameter_2 == 4);

    ScopedBank bank;
    data::persist::bank_open_account(bank.get(), "Units");
    const auto mover_count = write_orders(*saving.match, mover, bank.get());
    const auto yard_count = write_orders(*saving.match, yard, bank.get());
    CHECK(mover_count == 3 && yard_count == 2);

    Fixture loading;
    auto& loaded_mover = loading.spawn(0, 40, 40);
    auto& loaded_yard = loading.spawn(0, 200, 200);
    CHECK(loaded_mover.record.id == mover.record.id && loaded_yard.record.id == yard.record.id);
    // The saved queues replace what the units hold when their orders load;
    // a queue with no saved orders keeps its own.
    (void)loading.match->issue_patrol(loaded_mover.unit_index, point(10, 10), false);
    (void)loading.match->issue_build_weapon(loaded_mover.unit_index, 0, 1);
    constexpr uint32_t load_tick = 0x1234;
    loading.match->state().game.tick = load_tick;
    restore_orders(*loading.match, loaded_mover, bank.get(), mover_count);
    restore_orders(*loading.match, loaded_yard, bank.get(), yard_count);
    // The mover's kept secondary order follows its restored primary queue.
    auto mover_loaded = exported(*loading.match, mover.unit_index);
    CHECK(mover_loaded.blobs.size() == 4);
    mover_loaded.blobs.pop_back();
    mover_loaded.goals.pop_back();
    CHECK(same(mover_loaded, mover_orders));
    CHECK(same(exported(*loading.match, yard.unit_index), yard_orders));
    CHECK(
        (kinds(loaded_mover.unit->primary) ==
         std::vector<uint8_t>{move_ground, patrol, follow_ground})
    );
    CHECK((kinds(loaded_mover.unit->secondary) == std::vector<uint8_t>{build_weapon}));
    CHECK((kinds(loaded_yard.unit->primary) == std::vector<uint8_t>{building_build}));
    CHECK((kinds(loaded_yard.unit->secondary) == std::vector<uint8_t>{build_weapon}));
    // Each restored order is stamped with the tick it loaded on.
    sim::match_runtime::Match::OrderRecordView views[4]{};
    CHECK(loading.match->queue_records(mover.unit_index, false, views, 4) == 3);
    CHECK(views[2].seen_x == 200 && views[2].seen_z == 200);
    for (std::size_t at = 0; at < 3; ++at)
        CHECK(views[at].issue_tick == load_tick);
    // install_head_goal gave the head order's goal to the navigator.
    const auto* goal = loading.match->ground_runtime(mover.unit_index)->navigation.goal;
    CHECK(goal != nullptr && goal->order == loaded_mover.unit->primary);
    // The restored match runs on with them.
    loading.run(2);
    CHECK(head_is(loaded_mover, move_ground));
}

// An attack by a unit free to chase (issue_attack's mobile branch): the order
// keeps the leash in its third parameter and where the chase began in its
// anchor.
void attack_orders_survive_a_save() {
    constexpr uint8_t attack_chase = 6;
    constexpr int32_t leash = 300;
    Fixture saving;
    saving.def.maneuver_leash_length = leash;
    auto& hunter = saving.spawn(0, 40, 56);
    auto& quarry = saving.spawn(1, 120, 40);
    CHECK(saving.match->issue_attack(hunter.unit_index, quarry.unit_index, false));
    const auto hunter_orders = exported(*saving.match, hunter.unit_index);
    data::persist::SavedOrder chase;
    std::size_t found = 0;
    for (const auto& blob : hunter_orders.blobs) {
        data::persist::SavedOrder order;
        data::persist::save_decode_order(blob.data(), &order);
        if (order.kind == attack_chase) {
            chase = order;
            ++found;
        } else
            CHECK(order.parameter_3 == 0 && order.anchor[0] == 0 && order.anchor[1] == 0);
    }
    CHECK(found == 1 && hunter_orders.blobs.size() == 2);
    CHECK(chase.target_id == quarry.record.id && chase.parameter_3 == leash);
    CHECK(chase.anchor[0] == 40 && chase.anchor[1] == 56);

    ScopedBank bank;
    data::persist::bank_open_account(bank.get(), "Units");
    const auto count = write_orders(*saving.match, hunter, bank.get());

    Fixture loading;
    loading.def.maneuver_leash_length = leash;
    auto& loaded_hunter = loading.spawn(0, 40, 56);
    auto& loaded_quarry = loading.spawn(1, 120, 40);
    CHECK(loaded_quarry.record.id == quarry.record.id);
    restore_orders(*loading.match, loaded_hunter, bank.get(), count);
    CHECK(same(exported(*loading.match, hunter.unit_index), hunter_orders));
    CHECK(kinds(loaded_hunter.unit->primary) == kinds(hunter.unit->primary));
}

// A restored air goal can belong to a ground unit. Its pointer is separate
// from the order's target-observer link and still needs death cleanup.
void restored_ground_owned_air_goal_loses_dead_target() {
    for (const uint32_t version : {0u, 1u, 2u}) {
        Options options;
        options.rules.setup.enhanced_mode.enabled = version != 0;
        options.rules.setup.enhanced_mode.version = version;
        Fixture f(options);
        auto& owner = f.spawn(0, 64, 64);
        auto& target = f.spawn(1, 128, 64);
        CHECK(f.match->air_driver(owner.unit_index) == nullptr);
        data::persist::SavedOrder order{};
        order.kind = move_ground;
        order.goal_kind = static_cast<int32_t>(data::persist::SavedGoalKind::air_target);
        data::persist::SavedGoal goal{};
        goal.kind = order.goal_kind;
        goal.air_target.unit_id = owner.unit_index;
        goal.air_target.target_id = target.unit_index;
        auto tails = f.match->saved_order_tails(owner.unit_index);
        (void)f.match->restore_saved_order(owner.unit_index, order, goal, tails);
        auto before = exported(*f.match, owner.unit_index);
        CHECK(before.goals.size() == 1);
        CHECK(saved_air_target(*f.match, owner.unit_index) == target.unit_index);

        const sim::match_runtime::KillOutcome outcome{sim::match_runtime::DeathKind::dismissed};
        f.match->apply_kill(target.unit_index, outcome, 0, OA_PLAYER_COUNT);
        auto after = exported(*f.match, owner.unit_index);
        CHECK(after.goals.size() == 1);
        CHECK(saved_air_target(*f.match, owner.unit_index) == 0);
        CHECK(f.match->fault() == nullptr);
    }
}

} // namespace

int main() {
    try {
        orders_survive_a_save();
        attack_orders_survive_a_save();
        restored_ground_owned_air_goal_loses_dead_target();
    } catch (const std::exception& failure) {
        std::cerr << "saved orders: " << failure.what() << '\n';
        return 1;
    }
    std::cout << "saved orders: ok\n";
    return 0;
}
