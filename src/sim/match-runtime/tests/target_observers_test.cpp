// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// An order that watches a unit and is then pointed at another unit, or at
// no unit, leaves the first unit's list. Freeing that order and hitting the
// first unit wakes only the orders that still watch it.
#include "oa/data/enhanced_mode.hpp"
#include "combat_fixture.hpp"
#include "match_tick_access.hpp"
#include "order_ownership.hpp"
#include "oa/data/persist/save_orders.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using namespace combat_fixture;
using sim::match_runtime::MatchTickAccess;

constexpr uint32_t damage_wake = 0x10; // the damage reaction wakes watchers with it

Options mode_options(uint32_t version) {
    Options options;
    options.per_player_limit = 32;
    oa::data::enhanced_mode::apply_alias(
        options.rules.setup.enhanced_mode, static_cast<uint32_t>(version)
    );
    return options;
}

// One order keeps watching the first unit. A second order starts there and
// is then pointed at `other` (null leaves it watching nobody). The second
// order is cancelled, and a hit on the first unit must still wake the order
// that stayed.
void retarget_leaves_the_old_unit(bool other_unit) {
    Options options;
    options.per_player_limit = 8;
    Fixture f(options);
    auto& watcher = f.spawn(0, 32, 64);
    auto& mover = f.spawn(0, 48, 64);
    auto& first = f.spawn(0, 96, 64);
    sim::unit_spawn::Slot* second = nullptr;
    if (other_unit)
        second = &f.spawn(0, 128, 64);

    auto& staying = f.match->issue_guard(watcher.unit_index, first.unit_index, false);
    auto& moving = f.match->issue_guard(mover.unit_index, first.unit_index, false);
    CHECK(!f.match->order_refused(staying) && !f.match->order_refused(moving));
    const auto place = MatchTickAccess::observer_place(*f.match, moving);
    CHECK(place.node != nullptr);
    CHECK(MatchTickAccess::list_holds(*f.match, *first.unit, place));
    const auto staying_place = MatchTickAccess::observer_place(*f.match, staying);
    CHECK(MatchTickAccess::list_holds(*f.match, *first.unit, staying_place));

    MatchTickAccess::link_target(*f.match, moving, second != nullptr ? second->unit : nullptr);
    CHECK(!MatchTickAccess::list_holds(*f.match, *first.unit, place));
    CHECK(MatchTickAccess::list_holds(*f.match, *first.unit, staying_place));
    if (second != nullptr)
        CHECK(MatchTickAccess::list_holds(*f.match, *second->unit, place));
    CHECK(f.match->fault() == nullptr);

    f.match->stop_orders(mover.unit_index);
    CHECK(!MatchTickAccess::list_holds(*f.match, *first.unit, place));
    if (second != nullptr)
        CHECK(!MatchTickAccess::list_holds(*f.match, *second->unit, place));
    f.match->apply_damage_event(first, nullptr, 1, weapon_hit, 0);
    CHECK((staying.raised_events & damage_wake) != 0);
    CHECK(f.match->fault() == nullptr);
}

void retarget_to_another_unit_leaves_the_first() {
    retarget_leaves_the_old_unit(true);
    std::cout << "retarget to another unit leaves the first passed\n";
}

void retarget_to_no_unit_leaves_the_first() {
    retarget_leaves_the_old_unit(false);
    std::cout << "retarget to no unit leaves the first passed\n";
}

void ownership_index_retains_collisions_and_reused_addresses() {
    using Index = sim::match_runtime::OrderOwnershipIndex;
    Index index(1);
    std::array<uint32_t, 32> keys{};
    for (size_t i = 0; i < 12; ++i) {
        CHECK(index.fresh(&keys[i]));
        CHECK(index.remember_owner(&keys[i], i));
        index.find(&keys[i])->observer_slot = static_cast<uint32_t>(i);
        index.find(&keys[i])->observer_previous = &keys[(i + 1) % 12];
    }
    CHECK(!index.fresh(&keys[12]));
    for (size_t i = 0; i < 12; i += 2)
        index.forget(&keys[i]);
    for (size_t i = 1; i < 12; i += 2) {
        CHECK(index.find(&keys[i]) != nullptr);
        CHECK(index.find(&keys[i])->owner_index == i);
        CHECK(index.find(&keys[i])->observer_slot == i);
        CHECK(index.find(&keys[i])->observer_previous == &keys[(i + 1) % 12]);
    }
    for (size_t i = 0; i < 12; i += 2) {
        CHECK(index.fresh(&keys[i]));
        CHECK(index.find(&keys[i])->observer_slot == Index::no_observer);
        CHECK(index.find(&keys[i])->observer_previous == nullptr);
    }
    CHECK(index.begin_tick(1) == false);
    index.removed();
    CHECK(index.active_count(4) == 3);
    CHECK(!index.begin_tick(1));
    CHECK(index.begin_tick(2));
    index.compacted();
    CHECK(!index.begin_tick(3));
}

// Cancellation destroys records immediately, retains survivor creation order,
// and compaction on a later tick does not change that presentation order.
void cancellations_keep_stable_ownership(uint32_t version, bool overflow) {
    Fixture f(mode_options(version));
    if (overflow)
        CHECK(MatchTickAccess::reset_order_index(*f.match, 1));
    auto& first = f.spawn(0, 32, 64);
    auto& second = f.spawn(0, 48, 64);
    auto& third = f.spawn(0, 64, 64);
    auto& target = f.spawn(0, 224, 64);
    std::array<const sim::simulation_state::Order*, 64> before{}, after{};
    for (unsigned i = 0; i < 8; ++i) {
        (void)f.match->issue_guard(first.unit_index, target.unit_index, i != 0);
        (void)f.match->issue_guard(second.unit_index, target.unit_index, i != 0);
        (void)f.match->issue_guard(third.unit_index, target.unit_index, i != 0);
    }
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    const auto count = MatchTickAccess::ownership_sequence(*f.match, before);
    CHECK(count == 24);
    f.match->stop_orders(second.unit_index);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    CHECK(MatchTickAccess::ownership_sequence(*f.match, after) == 16);
    size_t at = 0;
    for (size_t i = 0; i < count; ++i)
        if (i % 3 != 1)
            CHECK(before[i] == after[at++]);
    f.run(1);
    // A fresh record after a tick triggers stable compaction; removed addresses
    // can be reused without inheriting their former target's certificate.
    auto& replacement = f.match->issue_guard(second.unit_index, first.unit_index, false);
    CHECK(!f.match->order_refused(replacement));
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    std::array<const sim::simulation_state::Order*, 64> compacted{};
    CHECK(MatchTickAccess::ownership_sequence(*f.match, compacted) == 17);
    for (size_t i = 0; i < 16; ++i)
        CHECK(after[i] == compacted[i]);
    CHECK(compacted[16] == &replacement);
    MatchTickAccess::link_target(*f.match, replacement, target.unit);
    MatchTickAccess::link_target(*f.match, replacement, nullptr);
    MatchTickAccess::link_target(*f.match, replacement, nullptr);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    f.match->stop_orders(first.unit_index);
    f.match->stop_orders(third.unit_index);
    f.match->stop_orders(second.unit_index);
    CHECK(MatchTickAccess::ownership_sequence(*f.match, after) == 0);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    CHECK(f.match->fault() == nullptr);
}

void saved_observers_release_a_common_target(uint32_t version) {
    Fixture f(mode_options(version));
    auto& owner = f.spawn(0, 32, 64);
    auto& target = f.spawn(0, 224, 64);
    auto tails = f.match->saved_order_tails(owner.unit_index);
    std::array<sim::simulation_state::Order*, 24> restored{};
    for (auto& order : restored) {
        data::persist::SavedOrder saved{};
        saved.kind = 18; // FollowGround
        saved.target_id = target.unit_index;
        data::persist::SavedGoal goal{};
        order = &f.match->restore_saved_order(owner.unit_index, saved, goal, tails);
        CHECK(!f.match->order_refused(*order));
    }
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    const sim::match_runtime::KillOutcome outcome{sim::match_runtime::DeathKind::dismissed};
    f.match->apply_kill(target.unit_index, outcome, 0, OA_PLAYER_COUNT);
    for (const auto* order : restored)
        CHECK((order->raised_events & 8u) != 0);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    f.match->stop_orders(owner.unit_index);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    CHECK(f.match->fault() == nullptr);
}

void full_audit_detects_untracked_duplicate_heads() {
    Fixture f(mode_options(2));
    auto& owner = f.spawn(0, 32, 64);
    auto& target = f.spawn(0, 128, 64);
    auto& other = f.spawn(0, 224, 64);
    auto& order = f.match->issue_guard(owner.unit_index, target.unit_index, false);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    const auto previous = MatchTickAccess::replace_observer_head(
        *f.match, *other.unit, MatchTickAccess::observer_place(*f.match, order)
    );
    CHECK(!MatchTickAccess::observer_lists_valid(*f.match));
    (void)MatchTickAccess::replace_observer_head(*f.match, *other.unit, previous);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
}

void full_capacity_replacement_reuses_storage() {
    Fixture f(mode_options(2));
    auto& owner = f.spawn(0, 32, 64);
    auto& target = f.spawn(0, 128, 64);
    (void)f.match->issue_guard(owner.unit_index, target.unit_index, false);
    const auto capacity = MatchTickAccess::shrink_order_capacity(*f.match);
    CHECK(capacity == 1);
    for (unsigned i = 0; i < 32; ++i) {
        f.match->stop_orders(owner.unit_index);
        (void)f.match->issue_guard(owner.unit_index, target.unit_index, false);
        CHECK(MatchTickAccess::order_capacity(*f.match) == capacity);
        CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    }
}

void retargeting_the_tail_of_a_long_live_chain() {
    auto options = mode_options(2);
    options.per_player_limit = 64;
    Fixture f(options);
    auto& target = f.spawn(0, 224, 224);
    auto& other = f.spawn(0, 208, 224);
    sim::simulation_state::Order* oldest = nullptr;
    uint16_t first_owner = 0;
    for (unsigned i = 0; i < 40; ++i) {
        auto& owner = f.spawn(0, 16 + i % 10 * 16, 16 + i / 10 * 16);
        for (unsigned queued = 0; queued < 8; ++queued) {
            auto& order = f.match->issue_guard(owner.unit_index, target.unit_index, queued != 0);
            CHECK(!f.match->order_refused(order));
            if (!oldest) {
                oldest = &order;
                first_owner = owner.unit_index;
            }
        }
    }
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    MatchTickAccess::link_target(*f.match, *oldest, other.unit);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    f.match->stop_orders(first_owner);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
    CHECK(f.match->fault() == nullptr);
}

// Invoked separately by the assertion negative control. Retargeting must reject
// a false certificate before modifying the actual old target's chain.
void corrupt_certificate() {
    Fixture f(mode_options(2));
    auto& owner = f.spawn(0, 32, 64);
    auto& target = f.spawn(0, 128, 64);
    auto& other = f.spawn(0, 224, 64);
    auto& order = f.match->issue_guard(owner.unit_index, target.unit_index, false);
    CHECK(MatchTickAccess::replace_observer_certificate(*f.match, order, *other.unit));
    MatchTickAccess::link_target(*f.match, order, nullptr);
    throw std::runtime_error("corrupt observer certificate was accepted");
}

void corrupt_missing_link() {
    Fixture f(mode_options(2));
    auto& owner = f.spawn(0, 32, 64);
    auto& target = f.spawn(0, 128, 64);
    auto& order = f.match->issue_guard(owner.unit_index, target.unit_index, false);
    (void)MatchTickAccess::replace_observer_head(*f.match, *target.unit, {});
    MatchTickAccess::link_target(*f.match, order, nullptr);
    throw std::runtime_error("missing observer link was accepted");
}

void corrupt_previous_link() {
    Fixture f(mode_options(2));
    auto& first = f.spawn(0, 32, 64);
    auto& second = f.spawn(0, 48, 64);
    auto& target = f.spawn(0, 128, 64);
    auto& order = f.match->issue_guard(first.unit_index, target.unit_index, false);
    (void)f.match->issue_guard(second.unit_index, target.unit_index, false);
    CHECK(MatchTickAccess::replace_observer_previous(*f.match, order, nullptr));
    MatchTickAccess::link_target(*f.match, order, nullptr);
    throw std::runtime_error("corrupt preceding observer was accepted");
}

void cancelling_behind_a_new_observer() {
    Fixture f(mode_options(2));
    auto& first = f.spawn(0, 32, 64);
    auto& second = f.spawn(0, 48, 64);
    auto& target = f.spawn(0, 128, 64);
    (void)f.match->issue_guard(first.unit_index, target.unit_index, false);
    (void)f.match->issue_guard(second.unit_index, target.unit_index, false);
    f.match->stop_orders(first.unit_index);
    CHECK(MatchTickAccess::observer_lists_valid(*f.match));
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--enhanced-lifecycle") {
            cancellations_keep_stable_ownership(2, false);
            saved_observers_release_a_common_target(2);
            retargeting_the_tail_of_a_long_live_chain();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--bad-observer-certificate")
            corrupt_certificate();
        if (argc == 2 && std::string_view(argv[1]) == "--missing-observer-link")
            corrupt_missing_link();
        if (argc == 2 && std::string_view(argv[1]) == "--bad-observer-predecessor")
            corrupt_previous_link();
        if (argc == 2 && std::string_view(argv[1]) == "--preceding-link-lifecycle") {
            cancelling_behind_a_new_observer();
            return 0;
        }
        retarget_to_another_unit_leaves_the_first();
        retarget_to_no_unit_leaves_the_first();
        ownership_index_retains_collisions_and_reused_addresses();
        for (uint32_t version : {0u, 1u, 2u}) {
            cancellations_keep_stable_ownership(version, false);
            saved_observers_release_a_common_target(version);
        }
        cancellations_keep_stable_ownership(2, true);
        full_audit_detects_untracked_duplicate_heads();
        full_capacity_replacement_reuses_storage();
        retargeting_the_tail_of_a_long_live_chain();
        cancelling_behind_a_new_observer();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
