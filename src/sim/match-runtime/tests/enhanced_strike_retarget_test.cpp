// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced strike groups replace a lost shared target before the ordinary
// refresh, with bounded retries and without running through another seat's turn.
// Continuation cases restore the registered rule-state table into an identical
// running match; the application save container is checked separately.
#include "combat_fixture.hpp"

#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {
using namespace combat_fixture;
namespace ai = oa::sim::ai;
namespace runtime = oa::sim::match_runtime;

constexpr uint8_t computer_player = 1;
constexpr uint8_t enemy_player = 0;
constexpr uint32_t retarget_version = 6;
constexpr uint32_t strike_refresh_ticks = 300;
constexpr uint32_t retarget_interval_ticks = 6;
constexpr uint32_t target_loss_tick = 55;
constexpr uint32_t retry_observation_ticks = 90;
constexpr uint16_t slots_per_player = 8;
constexpr std::size_t strike_members = 4;
constexpr uint32_t durable_health = 30000;
constexpr uint32_t fixed_one = 1u << 16;
constexpr uint32_t target_x = 152;
constexpr uint32_t target_z = 112;
constexpr uint32_t replacement_x = 232;
constexpr uint32_t replacement_z = 200;
constexpr uint32_t member_x = 24;
constexpr uint32_t naval_member_x = 56;
constexpr uint16_t fixture_gun = 1;
constexpr uint16_t fixture_unit_type = 1;
constexpr int32_t movement_gun_range = 32;
constexpr uint16_t east_heading = 0x4000;
constexpr std::size_t leading_member = 2;
constexpr std::array<uint32_t, strike_members> member_z = {48, 80, 112, 144};
constexpr uint32_t strike_squad = static_cast<uint32_t>(ai::Squad::land_strike);
constexpr uint32_t naval_squad = static_cast<uint32_t>(ai::Squad::naval_strike);
constexpr int32_t merge_radius = 20000;
constexpr uint32_t state_magic = 0x47545241u;
constexpr uint32_t state_schema = 2;
constexpr std::size_t state_header_bytes = 24;
constexpr std::size_t target_record_bytes = 20;
constexpr std::size_t tracked_strikes = OA_PLAYER_COUNT * 2;
constexpr std::size_t state_byte_count = state_header_bytes + tracked_strikes * target_record_bytes;
constexpr std::size_t magic_offset = 0;
constexpr std::size_t schema_offset = 4;
constexpr std::size_t unit_count_offset = 8;
constexpr std::size_t next_early_offset = 12;
constexpr std::size_t prepared_tick_offset = 16;
constexpr std::size_t cursor_offset = 20;
constexpr std::size_t prepared_valid_offset = 21;
constexpr std::size_t builtin_players_offset = 22;
constexpr std::size_t target_owner_offset = 2;
constexpr std::size_t target_binding_offset = 3;
constexpr std::size_t target_issue_offset = 4;
constexpr std::size_t target_retry_offset = 8;
constexpr std::size_t native_next_tick_offset = 12;
constexpr std::size_t native_clock_valid_offset = 16;
constexpr std::size_t native_attacking_offset = 17;
constexpr std::size_t target_reserved_offset = 18;
constexpr uint8_t empty_binding = 0;
constexpr uint8_t bound_binding = 1;
constexpr uint8_t lost_binding = 2;
constexpr std::size_t land_target_record =
    state_header_bytes + computer_player * 2 * target_record_bytes;

/// Reads a little-endian integer from a rule-state fixture.
uint32_t read_integer(std::span<const uint8_t> bytes, std::size_t at, std::size_t count = 4) {
    CHECK(at <= bytes.size() && count <= bytes.size() - at);
    uint32_t value = 0;
    for (std::size_t byte = 0; byte < count; ++byte)
        value |= static_cast<uint32_t>(bytes[at + byte]) << (byte * 8);
    return value;
}

/// Writes an integer into a malformed little-endian fixture.
void write_integer(
    std::span<uint8_t> bytes, std::size_t at, uint32_t value, std::size_t count = 4
) {
    CHECK(at <= bytes.size() && count <= bytes.size() - at);
    for (std::size_t byte = 0; byte < count; ++byte)
        bytes[at + byte] = static_cast<uint8_t>(value >> (byte * 8));
}

/// Finds the production rule-state table, with no private-state test override.
const runtime::RuleStateTable& retarget_table(Fixture& fixture) {
    const auto* table = runtime::find_rule_state(fixture.match->rule_state(), "ai-retarget");
    CHECK(table != nullptr && table->bytes != nullptr && table->restore != nullptr);
    return *table;
}

/// Copies the production codec's complete state for exact comparisons.
std::vector<uint8_t> retarget_bytes(Fixture& fixture) {
    const auto& table = retarget_table(fixture);
    const auto bytes = table.bytes(table.context);
    return {bytes.begin(), bytes.end()};
}

/// Restores one complete production table through its registered callback.
void restore_retarget(Fixture& fixture, std::span<const uint8_t> bytes) {
    const auto& table = retarget_table(fixture);
    CHECK(table.restore(table.context, bytes));
}

/// Prepares durable units and one version of the match rules.
Options options_for(uint32_t version) {
    Options options;
    options.rules.setup.enhanced_mode.enabled = version != 0;
    options.rules.setup.enhanced_mode.version = version;
    options.per_player_limit = slots_per_player;
    options.maximum_health = durable_health;
    options.gun_reload_time = "10";
    return options;
}

/// Keeps an idle unit from independently replacing the squad's target.
void hold_fire(sim::unit_spawn::Slot& slot) {
    slot.record.flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
}

/// Reads a primary order when one remains on the unit.
std::optional<runtime::Match::OrderRecordView> primary_record(Fixture& fixture, uint16_t unit) {
    runtime::Match::OrderRecordView record;
    if (fixture.match->queue_records(unit, false, &record, 1) == 0)
        return std::nullopt;
    return record;
}

/// Reads a required primary order without changing the running mission.
runtime::Match::OrderRecordView head_record(Fixture& fixture, uint16_t unit) {
    const auto record = primary_record(fixture, unit);
    CHECK(record.has_value());
    return *record;
}

/// Holds one real computer-controlled strike with two live enemy choices.
struct StrikeFixture {
    Fixture fixture;
    std::array<uint16_t, strike_members> members{};
    std::array<uint16_t, strike_members> naval_members{};
    uint16_t target{};
    uint16_t replacement{};
    ai::ComputerPlayer* controller{};
    uint32_t initial_issue_tick{};
    uint32_t regular_refresh_tick{};

    /// Starts the ordinary strike through the full match controller tick.
    explicit StrikeFixture(uint32_t version, bool short_range_weapon = false)
        : fixture(options_for(version)) {
        if (short_range_weapon) {
            auto gun = fixture.weapons.records()[fixture_gun];
            gun.range = movement_gun_range;
            fixture.weapons.install(gun, {});
            fixture.match->state().game.weapon_defs[fixture_gun] = gun;
        }
        for (std::size_t i = 0; i < members.size(); ++i) {
            auto& member = fixture.spawn(computer_player, member_x, member_z[i]);
            hold_fire(member);
            if (short_range_weapon)
                member.record.heading = east_heading;
            members[i] = member.unit_index;
            fixture.match->set_unit_squad(member.unit_index, strike_squad);
        }
        auto& first = fixture.spawn(enemy_player, target_x, target_z);
        hold_fire(first);
        target = first.unit_index;
        auto& second = fixture.spawn(
            enemy_player, replacement_x, short_range_weapon ? target_z : replacement_z
        );
        hold_fire(second);
        replacement = second.unit_index;

        fixture.match->simulation().players[computer_player].status = OA_PLAYER_STATUS_COMPUTER;
        ai::prepare_match_computer_players(*fixture.match);
        auto* players = ai::match_computer_players(*fixture.match);
        CHECK(players != nullptr);
        controller = &players->players[computer_player];
        CHECK(controller->present != 0);
        controller->sort_countdown = static_cast<int32_t>(strike_refresh_ticks * 10);
        for (auto& task : controller->tasks)
            task.kind = ai::TaskKind::none;
        auto& task = strike();
        task.kind = ai::TaskKind::strike;
        task.squad = ai::Squad::land_strike;
        task.next_tick = fixture.match->simulation().tick;
        task.min_size = static_cast<int32_t>(strike_members - 1);
        task.launch_size = static_cast<int32_t>(strike_members);
        task.merge_radius = merge_radius;
        task.source_squad = ai::Squad::land_army;
        task.attacking = 1;

        fixture.run(1);
        initial_issue_tick = head_record(fixture, members.front()).issue_tick;
        regular_refresh_tick = initial_issue_tick + strike_refresh_ticks;
        CHECK(strike().next_tick == regular_refresh_tick);
        check_attack(target, initial_issue_tick);
    }

    /// Returns the strike task kept by the real built-in controller.
    ai::ComputerTask& strike() { return controller->tasks[strike_squad]; }

    /// Adds a second strike task that competes for the same early-refresh budget.
    std::array<uint16_t, strike_members> add_naval_strike() {
        for (std::size_t i = 0; i < naval_members.size(); ++i) {
            auto& member = fixture.spawn(computer_player, naval_member_x, member_z[i]);
            hold_fire(member);
            naval_members[i] = member.unit_index;
            fixture.match->set_unit_squad(member.unit_index, naval_squad);
        }
        auto& task = controller->tasks[naval_squad];
        task.kind = ai::TaskKind::strike;
        task.squad = ai::Squad::naval_strike;
        task.next_tick = fixture.match->simulation().tick;
        task.min_size = static_cast<int32_t>(strike_members - 1);
        task.launch_size = static_cast<int32_t>(strike_members);
        task.merge_radius = merge_radius;
        task.source_squad = ai::Squad::navy;
        task.attacking = 1;
        fixture.run(1);
        for (const auto member : naval_members) {
            const auto record = head_record(fixture, member);
            CHECK(record.kind == attack_chase_order && record.target == target);
        }
        return naval_members;
    }

    /// Advances full match ticks up to a deterministic simulation deadline.
    void run_until(uint32_t tick) {
        CHECK(fixture.match->simulation().tick <= tick);
        fixture.run(tick - fixture.match->simulation().tick);
    }

    /// Checks every squad member's command target and creation tick.
    void check_attack(uint16_t aimed, uint32_t issue_tick) {
        for (const auto member : members) {
            const auto record = head_record(fixture, member);
            CHECK(record.kind == attack_chase_order);
            CHECK(record.target == aimed);
            CHECK(record.issue_tick == issue_tick);
        }
    }

    /// Reports whether every member has a new command on the given target.
    bool all_retargeted(uint16_t aimed) {
        for (const auto member : members) {
            const auto record = primary_record(fixture, member);
            if (!record || record->kind != attack_chase_order || record->target != aimed ||
                record->issue_tick <= initial_issue_tick)
                return false;
        }
        return true;
    }

    /// Removes an enemy through the match's target-observer and death paths.
    void kill_target(uint16_t unit) {
        kill(fixture, fixture.match->spawn_slot(unit), fixture.match->spawn_slot(members.front()));
    }

    /// Creates a distinct enemy in the freed target's exact global slot.
    void reuse_target_slot() {
        auto* recreated = fixture.match->create(
            {enemy_player,
             1,
             {target_x * fixed_one, 32u * fixed_one, target_z * fixed_one},
             true,
             1,
             target}
        );
        CHECK(recreated != nullptr && recreated->unit != nullptr);
        CHECK(recreated->unit_index == target);
        hold_fire(*recreated);
    }

    /// Waits a bounded number of full ticks for the complete squad to retarget.
    uint32_t wait_for_retarget(uint16_t aimed, uint32_t latest_tick) {
        while (fixture.match->simulation().tick < latest_tick && !all_retargeted(aimed))
            fixture.run(1);
        CHECK(all_retargeted(aimed));
        const auto issued = head_record(fixture, members.front()).issue_tick;
        check_attack(aimed, issued);
        return issued;
    }
};

/// Keeps all earlier presets on the existing regular strike refresh.
void old_presets_keep_the_regular_refresh() {
    for (uint32_t version = 0; version < retarget_version; ++version) {
        StrikeFixture battle(version);
        CHECK(
            runtime::find_rule_state(battle.fixture.match->rule_state(), "ai-retarget") == nullptr
        );
        battle.run_until(target_loss_tick);
        battle.kill_target(battle.target);
        battle.run_until(battle.regular_refresh_tick - 1);
        CHECK(battle.strike().next_tick == battle.regular_refresh_tick);
        CHECK(!battle.all_retargeted(battle.replacement));
        battle.fixture.run(1);
        battle.check_attack(battle.replacement, battle.regular_refresh_tick);
    }
}

/// Retargets the complete strike promptly when its shared target dies.
void dead_target_refreshes_the_squad_before_the_regular_deadline() {
    StrikeFixture battle(retarget_version);
    battle.run_until(target_loss_tick);
    battle.kill_target(battle.target);
    const auto issued =
        battle.wait_for_retarget(battle.replacement, target_loss_tick + retarget_interval_ticks);
    CHECK(issued > target_loss_tick);
    CHECK(issued < battle.regular_refresh_tick);
}

/// Checks that the new attack produces a real Goal and forward movement next tick.
void check_goal_and_motion_after_issue(StrikeFixture& battle, uint16_t aimed, uint32_t issued) {
    CHECK(battle.fixture.match->simulation().tick == issued);
    const auto& leader = battle.fixture.match->spawn_slot(battle.members[leading_member]);
    const int32_t previous_x = leader.record.position.x;
    const int32_t previous_z = leader.record.position.z;
    battle.fixture.run(1);
    CHECK(battle.fixture.match->simulation().tick == issued + 1);
    battle.check_attack(aimed, issued);
    for (const auto member : battle.members) {
        const auto* ground = battle.fixture.match->ground_runtime(member);
        CHECK(ground != nullptr && ground->navigation.goal != nullptr);
        CHECK(
            ground->navigation.goal->order == battle.fixture.match->spawn_slot(member).unit->primary
        );
    }
    CHECK(leader.record.position.x != previous_x || leader.record.position.z != previous_z);
    CHECK(battle.strike().next_tick == issued + strike_refresh_ticks);
}

/// Restores a healthy replacement in an exact free slot on the leader's open lane.
void create_healthy_replacement(StrikeFixture& battle, uint16_t slot, uint32_t x) {
    auto* created = battle.fixture.match->create(
        {enemy_player,
         fixture_unit_type,
         {x * fixed_one, 32u * fixed_one, target_z * fixed_one},
         true,
         1,
         slot}
    );
    CHECK(created != nullptr && created->unit != nullptr);
    CHECK(created->unit_index == slot && created->record.health > 0);
    hold_fire(*created);
}

/// Waits for the remaining short cooldown when a target dies just after its issue.
void early_target_loss_respects_the_short_cooldown_then_moves() {
    StrikeFixture battle(retarget_version, true);
    const uint32_t died = battle.initial_issue_tick + 1;
    const uint32_t next_allowed = battle.initial_issue_tick + retarget_interval_ticks;
    battle.run_until(died);
    battle.kill_target(battle.target);
    while (battle.fixture.match->simulation().tick < next_allowed - 1) {
        battle.fixture.run(1);
        CHECK(!battle.all_retargeted(battle.replacement));
        CHECK(battle.strike().next_tick == battle.regular_refresh_tick);
    }
    battle.fixture.run(1);
    battle.check_attack(battle.replacement, next_allowed);
    CHECK(next_allowed - died <= retarget_interval_ticks);
    std::array<sim::simulation_state::Order*, strike_members> issued_heads{};
    for (std::size_t i = 0; i < battle.members.size(); ++i)
        issued_heads[i] = battle.fixture.match->spawn_slot(battle.members[i]).unit->primary;
    check_goal_and_motion_after_issue(battle, battle.replacement, next_allowed);
    while (battle.fixture.match->simulation().tick < next_allowed + retarget_interval_ticks - 1) {
        battle.fixture.run(1);
        battle.check_attack(battle.replacement, next_allowed);
        CHECK(battle.strike().next_tick == next_allowed + strike_refresh_ticks);
        for (std::size_t i = 0; i < battle.members.size(); ++i)
            CHECK(
                battle.fixture.match->spawn_slot(battle.members[i]).unit->primary == issued_heads[i]
            );
    }
}

/// Keeps goals and movement returning as healthy replacement targets die repeatedly.
void short_lived_healthy_targets_resume_goals_and_motion() {
    StrikeFixture battle(retarget_version, true);
    constexpr uint32_t first_target_lifetime = 4;
    battle.run_until(battle.initial_issue_tick + first_target_lifetime);
    const uint32_t first_death = battle.fixture.match->simulation().tick;
    battle.kill_target(battle.target);
    uint16_t current = battle.replacement;
    uint16_t free_slot = battle.target;
    uint32_t issued = battle.wait_for_retarget(current, first_death + retarget_interval_ticks);
    CHECK(issued - first_death <= retarget_interval_ticks);
    check_goal_and_motion_after_issue(battle, current, issued);
    constexpr std::array<uint32_t, 3> target_lifetimes = {4, 5, 6};
    for (const auto lifetime : target_lifetimes) {
        const uint32_t next_x = free_slot == battle.target ? target_x : replacement_x;
        create_healthy_replacement(battle, free_slot, next_x);
        const uint32_t died = issued + lifetime;
        while (battle.fixture.match->simulation().tick < died) {
            battle.fixture.run(1);
            battle.check_attack(current, issued);
            CHECK(battle.strike().next_tick == issued + strike_refresh_ticks);
        }
        const auto& dying = battle.fixture.match->spawn_slot(current).record;
        CHECK(dying.health > 0 && oa::unit_is_live_target(dying.flags));
        battle.kill_target(current);
        const uint32_t next_issue =
            battle.wait_for_retarget(free_slot, died + retarget_interval_ticks);
        CHECK(next_issue - issued >= retarget_interval_ticks);
        CHECK(next_issue - died <= retarget_interval_ticks);
        check_goal_and_motion_after_issue(battle, free_slot, next_issue);
        CHECK(battle.fixture.match->simulation().tick - died <= retarget_interval_ticks + 1);
        std::swap(current, free_slot);
        issued = next_issue;
    }
}

/// Leaves a live target's orders alone until the regular refresh comes due.
void live_target_does_not_add_command_refreshes() {
    StrikeFixture battle(retarget_version);
    while (battle.fixture.match->simulation().tick < battle.regular_refresh_tick - 1) {
        battle.fixture.run(1);
        battle.check_attack(battle.target, battle.initial_issue_tick);
        CHECK(battle.strike().next_tick == battle.regular_refresh_tick);
    }
    battle.fixture.run(1);
    battle.check_attack(battle.target, battle.regular_refresh_tick);
}

/// Describes a target that is still present but should not receive a fresh attack.
enum class UnhealthyTarget { zero_health_pending, zero_health, pending_positive_health };

/// Keeps an unhealthy target's body live until a unit tick performs its teardown.
void mark_unhealthy_target(StrikeFixture& battle, uint16_t unit, UnhealthyTarget condition) {
    auto& record = battle.fixture.match->spawn_slot(unit).record;
    CHECK(record.type_index != 0 && (record.flags & OA_UNIT_FLAG_LIVE) != 0);
    record.health = condition == UnhealthyTarget::pending_positive_health ? 1 : 0;
    record.flags &= ~OA_UNIT_FLAG_DEATH_PENDING;
    if (condition != UnhealthyTarget::zero_health)
        record.flags |= OA_UNIT_FLAG_DEATH_PENDING;
}

/// Runs the actual native controller at a canonical tick without tearing units down.
void run_controller_at(StrikeFixture& battle, uint32_t tick) {
    battle.fixture.match->state().game.tick = tick;
    battle.fixture.match->run_player_controller(computer_player);
    CHECK(battle.fixture.match->fault() == nullptr);
}

/// Keeps earlier presets' selection of a still-live pending target unchanged.
void earlier_presets_keep_the_pending_target_selection() {
    for (uint32_t version = 0; version < retarget_version; ++version) {
        StrikeFixture battle(version);
        battle.run_until(target_loss_tick - 1);
        mark_unhealthy_target(battle, battle.target, UnhealthyTarget::zero_health_pending);
        battle.strike().next_tick = target_loss_tick;
        run_controller_at(battle, target_loss_tick);
        battle.check_attack(battle.target, target_loss_tick);
        CHECK(battle.strike().next_tick == target_loss_tick + strike_refresh_ticks);
        CHECK(battle.strike().attacking != 0);
        const auto& record = battle.fixture.match->spawn_slot(battle.target).record;
        CHECK(record.type_index != 0 && (record.flags & OA_UNIT_FLAG_DEATH_PENDING) != 0);
    }
}

/// Uses one early strike batch to choose a healthy enemy before the old target tears down.
void enhanced_strike_skips_unhealthy_live_target_bodies() {
    constexpr std::array conditions = {
        UnhealthyTarget::zero_health_pending,
        UnhealthyTarget::zero_health,
        UnhealthyTarget::pending_positive_health
    };
    for (const auto condition : conditions) {
        StrikeFixture battle(retarget_version);
        battle.run_until(target_loss_tick - 1);
        mark_unhealthy_target(battle, battle.target, condition);
        CHECK(battle.strike().next_tick > target_loss_tick);
        run_controller_at(battle, target_loss_tick);
        battle.check_attack(battle.replacement, target_loss_tick);
        CHECK(battle.strike().next_tick == target_loss_tick + strike_refresh_ticks);
        const auto bytes = retarget_bytes(battle.fixture);
        CHECK(bytes.size() == state_byte_count);
        CHECK(read_integer(bytes, schema_offset) == state_schema);
        CHECK(read_integer(bytes, next_early_offset) == target_loss_tick + retarget_interval_ticks);
        CHECK(read_integer(bytes, land_target_record, 2) == battle.replacement);
        CHECK(bytes[land_target_record + target_binding_offset] == bound_binding);
        CHECK(read_integer(bytes, land_target_record + target_issue_offset) == target_loss_tick);
        CHECK(
            read_integer(bytes, land_target_record + target_retry_offset) ==
            target_loss_tick + retarget_interval_ticks
        );
        CHECK(
            read_integer(bytes, land_target_record + native_next_tick_offset) ==
            target_loss_tick + strike_refresh_ticks
        );
        CHECK(bytes[land_target_record + native_clock_valid_offset] == 1);
        CHECK(bytes[land_target_record + native_attacking_offset] == 1);
        std::array<sim::simulation_state::Order*, strike_members> issued_heads{};
        for (std::size_t i = 0; i < battle.members.size(); ++i)
            issued_heads[i] = battle.fixture.match->spawn_slot(battle.members[i]).unit->primary;
        run_controller_at(battle, target_loss_tick);
        for (std::size_t i = 0; i < battle.members.size(); ++i)
            CHECK(
                battle.fixture.match->spawn_slot(battle.members[i]).unit->primary == issued_heads[i]
            );
        CHECK(retarget_bytes(battle.fixture) == bytes);
    }
}

/// Retries at the finite cooldown without reissuing an attack on pending bodies.
void all_pending_targets_keep_retries_bounded_without_new_attacks() {
    StrikeFixture battle(retarget_version);
    battle.run_until(target_loss_tick - 1);
    mark_unhealthy_target(battle, battle.target, UnhealthyTarget::zero_health_pending);
    mark_unhealthy_target(battle, battle.replacement, UnhealthyTarget::zero_health_pending);
    std::array<sim::simulation_state::Order*, strike_members> original_heads{};
    for (std::size_t i = 0; i < battle.members.size(); ++i)
        original_heads[i] = battle.fixture.match->spawn_slot(battle.members[i]).unit->primary;
    uint32_t previous_deadline = battle.strike().next_tick;
    std::optional<uint32_t> previous_attempt;
    uint32_t attempts = 0;
    for (uint32_t elapsed = 0; elapsed < retry_observation_ticks; ++elapsed) {
        const uint32_t tick = target_loss_tick + elapsed;
        run_controller_at(battle, tick);
        const uint32_t deadline = battle.strike().next_tick;
        if (deadline != previous_deadline) {
            ++attempts;
            if (previous_attempt)
                CHECK(tick - *previous_attempt >= retarget_interval_ticks);
            previous_attempt = tick;
            previous_deadline = deadline;
            CHECK(deadline == tick + strike_refresh_ticks);
        }
        CHECK(attempts <= 1 + elapsed / retarget_interval_ticks);
        battle.check_attack(battle.target, battle.initial_issue_tick);
        for (std::size_t i = 0; i < battle.members.size(); ++i)
            CHECK(
                battle.fixture.match->spawn_slot(battle.members[i]).unit->primary ==
                original_heads[i]
            );
        const auto bytes = retarget_bytes(battle.fixture);
        CHECK(bytes.size() == state_byte_count);
        CHECK(read_integer(bytes, land_target_record, 2) == battle.target);
        CHECK(bytes[land_target_record + target_binding_offset] == lost_binding);
        CHECK(
            read_integer(bytes, land_target_record + target_issue_offset) ==
            battle.initial_issue_tick
        );
        CHECK(read_integer(bytes, land_target_record + native_next_tick_offset) == deadline);
    }
    CHECK(
        attempts ==
        (retry_observation_ticks + retarget_interval_ticks - 1) / retarget_interval_ticks
    );
}

/// Bounds unsuccessful retarget attempts when no enemy remains.
void no_enemy_does_not_retry_on_every_tick() {
    StrikeFixture battle(retarget_version);
    battle.run_until(target_loss_tick);
    battle.kill_target(battle.target);
    battle.kill_target(battle.replacement);
    auto previous_deadline = battle.strike().next_tick;
    uint32_t attempts = 0;
    for (uint32_t elapsed = 0; elapsed < retry_observation_ticks; ++elapsed) {
        battle.fixture.run(1);
        const auto deadline = battle.strike().next_tick;
        if (deadline != previous_deadline) {
            ++attempts;
            previous_deadline = deadline;
        }
        for (const auto member : battle.members) {
            const auto record = primary_record(battle.fixture, member);
            CHECK(!record || record->kind != attack_chase_order || record->target == 0);
        }
    }
    CHECK(attempts != 0);
    CHECK(attempts <= 1 + retry_observation_ticks / retarget_interval_ticks);
}

/// Treats a new enemy in the dead target's slot as a different unit lifetime.
void reusing_the_target_slot_still_requires_a_new_command() {
    StrikeFixture battle(retarget_version);
    battle.run_until(target_loss_tick);
    battle.kill_target(battle.target);
    battle.reuse_target_slot();
    const auto issued =
        battle.wait_for_retarget(battle.target, target_loss_tick + retarget_interval_ticks);
    CHECK(issued > target_loss_tick);
    CHECK(issued < battle.regular_refresh_tick);
}

/// Retargets an enemy that has been given to the computer player's own side.
void transferred_target_is_no_longer_a_hostile_binding() {
    StrikeFixture battle(retarget_version);
    battle.run_until(target_loss_tick);
    battle.fixture.match->transfer_unit(battle.target, computer_player);
    const auto issued =
        battle.wait_for_retarget(battle.replacement, target_loss_tick + retarget_interval_ticks);
    CHECK(issued > target_loss_tick);
    CHECK(issued < battle.regular_refresh_tick);
}

/// Rejects malformed codec input without changing any retained state.
void malformed_rule_state_is_rejected_transactionally() {
    StrikeFixture battle(retarget_version);
    const auto original = retarget_bytes(battle.fixture);
    CHECK(original.size() == state_byte_count);
    CHECK(read_integer(original, magic_offset) == state_magic);
    CHECK(read_integer(original, schema_offset) == state_schema);
    CHECK(read_integer(original, land_target_record, 2) == battle.target);
    CHECK(original[land_target_record + target_binding_offset] == bound_binding);
    CHECK(
        read_integer(original, land_target_record + native_next_tick_offset) ==
        battle.strike().next_tick
    );
    CHECK(original[land_target_record + native_clock_valid_offset] == 1);
    CHECK(original[land_target_record + native_attacking_offset] == 1);
    const auto& table = retarget_table(battle.fixture);
    const auto reject = [&](const std::vector<uint8_t>& malformed) {
        CHECK(!table.restore(table.context, malformed));
        CHECK(retarget_bytes(battle.fixture) == original);
    };
    for (const auto length : {std::size_t{0}, state_header_bytes - 1, original.size() - 1}) {
        auto truncated = original;
        truncated.resize(length);
        reject(truncated);
    }
    auto oversized = original;
    oversized.push_back(0);
    reject(oversized);
    const auto reject_integer = [&](std::size_t at, uint32_t value, std::size_t count = 4) {
        auto malformed = original;
        write_integer(malformed, at, value, count);
        reject(malformed);
    };
    reject_integer(magic_offset, state_magic + 1);
    reject_integer(schema_offset, state_schema + 1);
    reject_integer(schema_offset, state_schema - 1);
    reject_integer(unit_count_offset, read_integer(original, unit_count_offset) + 1);
    reject_integer(builtin_players_offset, 1u << OA_PLAYER_COUNT, 2);
    reject_integer(cursor_offset, tracked_strikes, 1);
    reject_integer(prepared_valid_offset, 2, 1);
    reject_integer(land_target_record, 0, 2);
    reject_integer(land_target_record, read_integer(original, unit_count_offset), 2);
    reject_integer(land_target_record + target_owner_offset, OA_PLAYER_COUNT, 1);
    reject_integer(land_target_record + target_binding_offset, lost_binding + 1, 1);
    reject_integer(land_target_record + target_binding_offset, empty_binding, 1);
    reject_integer(land_target_record + native_clock_valid_offset, 2, 1);
    reject_integer(land_target_record + native_attacking_offset, 2, 1);
    reject_integer(land_target_record + target_reserved_offset, 1, 1);
    reject_integer(land_target_record + target_reserved_offset + 1, 1, 1);
    reject_integer(land_target_record + native_clock_valid_offset, 0, 1);
    // Player zero's unused land record must remain completely empty.
    reject_integer(state_header_bytes, battle.target, 2);
    reject_integer(state_header_bytes + target_owner_offset, computer_player, 1);
    reject_integer(state_header_bytes + target_issue_offset, 1);
    reject_integer(state_header_bytes + target_retry_offset, 1);
    reject_integer(state_header_bytes + native_next_tick_offset, 1);
    reject_integer(state_header_bytes + native_attacking_offset, 1, 1);
    auto unprepared = original;
    unprepared[prepared_valid_offset] = 0;
    write_integer(unprepared, prepared_tick_offset, 1);
    reject(unprepared);
}

/// Keeps owner-local target bindings out of the shared match digest.
void owner_local_retarget_state_does_not_change_the_shared_digest() {
    StrikeFixture battle(retarget_version);
    const auto& table = retarget_table(battle.fixture);
    CHECK(!table.digest);
    constexpr uint64_t digest_seed = 0x0123456789abcdefULL;
    const auto original = retarget_bytes(battle.fixture);
    const uint64_t before = battle.fixture.match->fold_rule_state(digest_seed);
    const uint8_t shared_tables =
        runtime::digested_rule_state_count(battle.fixture.match->rule_state());
    Fixture fresh(options_for(retarget_version));
    const auto blank = retarget_bytes(fresh);
    CHECK(blank != original);
    restore_retarget(battle.fixture, blank);
    CHECK(retarget_bytes(battle.fixture) == blank);
    CHECK(battle.fixture.match->fold_rule_state(digest_seed) == before);
    CHECK(runtime::digested_rule_state_count(battle.fixture.match->rule_state()) == shared_tables);
    restore_retarget(battle.fixture, original);
    CHECK(retarget_bytes(battle.fixture) == original);
    CHECK(battle.fixture.match->fold_rule_state(digest_seed) == before);
    CHECK(runtime::digested_rule_state_count(battle.fixture.match->rule_state()) == shared_tables);
}

/// Compares mission commands and exact scheduler bytes in two running matches.
void compare_continuation_state(StrikeFixture& first, StrikeFixture& restored) {
    CHECK(first.fixture.match->simulation().tick == restored.fixture.match->simulation().tick);
    CHECK(first.strike().next_tick == restored.strike().next_tick);
    CHECK(first.strike().attacking == restored.strike().attacking);
    CHECK(
        first.controller->tasks[naval_squad].next_tick ==
        restored.controller->tasks[naval_squad].next_tick
    );
    CHECK(
        first.controller->tasks[naval_squad].attacking ==
        restored.controller->tasks[naval_squad].attacking
    );
    const auto compare_members = [&](std::span<const uint16_t> left_members,
                                     std::span<const uint16_t> right_members) {
        CHECK(left_members.size() == right_members.size());
        for (std::size_t i = 0; i < left_members.size(); ++i) {
            const auto left = primary_record(first.fixture, left_members[i]);
            const auto right = primary_record(restored.fixture, right_members[i]);
            CHECK(left.has_value() == right.has_value());
            if (left && right) {
                CHECK(left->kind == right->kind);
                CHECK(left->target == right->target);
                CHECK(left->issue_tick == right->issue_tick);
                CHECK(left->phase == right->phase);
            }
        }
    };
    compare_members(first.members, restored.members);
    CHECK((first.naval_members.front() != 0) == (restored.naval_members.front() != 0));
    if (first.naval_members.front() != 0)
        compare_members(first.naval_members, restored.naval_members);
    CHECK(retarget_bytes(first.fixture) == retarget_bytes(restored.fixture));
}

/// Clears a populated tracker before restoring the other match's saved bytes.
void restore_and_compare_continuation(StrikeFixture& first, StrikeFixture& restored) {
    compare_continuation_state(first, restored);
    const auto saved = retarget_bytes(first.fixture);
    Fixture fresh(options_for(retarget_version));
    const auto blank = retarget_bytes(fresh);
    CHECK(blank != saved);
    restore_retarget(restored.fixture, blank);
    CHECK(retarget_bytes(restored.fixture) == blank);
    restore_retarget(restored.fixture, saved);
    compare_continuation_state(first, restored);
    for (uint32_t elapsed = 0; elapsed < retry_observation_ticks; ++elapsed) {
        first.fixture.run(1);
        restored.fixture.run(1);
        compare_continuation_state(first, restored);
    }
}

/// Resumes rule state after target loss, including the saved early-refresh cursor.
void rule_state_continues_after_target_loss() {
    StrikeFixture first(retarget_version);
    StrikeFixture restored(retarget_version);
    first.run_until(target_loss_tick);
    restored.run_until(target_loss_tick);
    first.kill_target(first.target);
    restored.kill_target(restored.target);
    const auto saved = retarget_bytes(first.fixture);
    CHECK(saved[land_target_record + target_binding_offset] == lost_binding);
    restore_and_compare_continuation(first, restored);
}

/// Keeps a saved loss when the dead target's slot already holds a new unit.
void rule_state_continues_after_immediate_slot_reuse() {
    StrikeFixture first(retarget_version);
    StrikeFixture restored(retarget_version);
    first.run_until(target_loss_tick);
    restored.run_until(target_loss_tick);
    first.kill_target(first.target);
    restored.kill_target(restored.target);
    first.reuse_target_slot();
    restored.reuse_target_slot();
    const auto saved = retarget_bytes(first.fixture);
    CHECK(read_integer(saved, land_target_record, 2) == first.target);
    CHECK(saved[land_target_record + target_binding_offset] == lost_binding);
    restore_and_compare_continuation(first, restored);
}

/// Keeps the finite retry deadline across a second loss during the cooldown.
void rule_state_continues_during_retarget_cooldown() {
    StrikeFixture first(retarget_version);
    StrikeFixture restored(retarget_version);
    first.run_until(target_loss_tick);
    restored.run_until(target_loss_tick);
    first.kill_target(first.target);
    restored.kill_target(restored.target);
    const auto first_issued =
        first.wait_for_retarget(first.replacement, target_loss_tick + retarget_interval_ticks);
    const auto restored_issued = restored.wait_for_retarget(
        restored.replacement, target_loss_tick + retarget_interval_ticks
    );
    CHECK(first_issued == restored_issued);
    first.kill_target(first.replacement);
    restored.kill_target(restored.replacement);
    const auto saved = retarget_bytes(first.fixture);
    CHECK(read_integer(saved, next_early_offset) > first.fixture.match->simulation().tick);
    CHECK(
        read_integer(saved, land_target_record + target_retry_offset) >
        first.fixture.match->simulation().tick
    );
    restore_and_compare_continuation(first, restored);
}

/// Recreates the built-in AI heap while leaving native task clocks uninitialized.
void recreate_ai_without_task_clocks(StrikeFixture& battle) {
    battle.fixture.match->computer_player_state().reset();
    ai::prepare_match_computer_players(*battle.fixture.match);
    auto* players = ai::match_computer_players(*battle.fixture.match);
    CHECK(players != nullptr);
    battle.controller = &players->players[computer_player];
    CHECK(battle.controller->present != 0);
    battle.controller->sort_countdown = static_cast<int32_t>(strike_refresh_ticks * 10);
    for (auto& task : battle.controller->tasks)
        task.kind = ai::TaskKind::none;
    auto& task = battle.strike();
    task.kind = ai::TaskKind::strike;
    task.squad = ai::Squad::land_strike;
    task.min_size = static_cast<int32_t>(strike_members - 1);
    task.launch_size = static_cast<int32_t>(strike_members);
    task.merge_radius = merge_radius;
    task.source_squad = ai::Squad::land_army;
    task.next_tick = 0;
    task.attacking = 0;
}

/// Distinguishes the lifetime and cooldown states restored into a new AI heap.
enum class FreshAiSavePoint { target_loss, slot_reuse, retry_cooldown };

/// Restores native strike clocks before a freshly initialized AI takes its first turn.
void fresh_ai_heap_continues_saved_strike_clocks() {
    constexpr std::array save_points = {
        FreshAiSavePoint::target_loss,
        FreshAiSavePoint::slot_reuse,
        FreshAiSavePoint::retry_cooldown
    };
    for (const auto save_point : save_points) {
        StrikeFixture first(retarget_version);
        StrikeFixture restored(retarget_version);
        first.run_until(target_loss_tick);
        restored.run_until(target_loss_tick);
        first.kill_target(first.target);
        restored.kill_target(restored.target);
        if (save_point == FreshAiSavePoint::slot_reuse) {
            first.reuse_target_slot();
            restored.reuse_target_slot();
        } else if (save_point == FreshAiSavePoint::retry_cooldown) {
            const auto first_issued = first.wait_for_retarget(
                first.replacement, target_loss_tick + retarget_interval_ticks
            );
            const auto restored_issued = restored.wait_for_retarget(
                restored.replacement, target_loss_tick + retarget_interval_ticks
            );
            CHECK(first_issued == restored_issued);
            first.kill_target(first.replacement);
            restored.kill_target(restored.replacement);
        }
        compare_continuation_state(first, restored);
        const auto saved = retarget_bytes(first.fixture);
        CHECK(saved[land_target_record + target_binding_offset] == lost_binding);
        CHECK(saved[land_target_record + native_clock_valid_offset] == 1);
        CHECK(saved[land_target_record + native_attacking_offset] == 1);
        CHECK(
            read_integer(saved, land_target_record + native_next_tick_offset) ==
            first.strike().next_tick
        );
        if (save_point == FreshAiSavePoint::retry_cooldown)
            CHECK(read_integer(saved, next_early_offset) > first.fixture.match->simulation().tick);

        Fixture fresh(options_for(retarget_version));
        restore_retarget(restored.fixture, retarget_bytes(fresh));
        recreate_ai_without_task_clocks(restored);
        CHECK(restored.strike().next_tick == 0 && restored.strike().attacking == 0);
        restore_retarget(restored.fixture, saved);
        CHECK(retarget_bytes(restored.fixture) == saved);
        // The restore applies its clocks at the next built-in controller call,
        // after the application has initialized the new AI heap.
        for (uint32_t elapsed = 0; elapsed < retry_observation_ticks; ++elapsed) {
            first.fixture.run(1);
            restored.fixture.run(1);
            compare_continuation_state(first, restored);
        }
    }
}

/// Applies saved task clocks before the saved tick's load-pass controller turn.
void load_pass_keeps_an_unexpired_retarget_cooldown() {
    StrikeFixture first(retarget_version);
    StrikeFixture restored(retarget_version);
    first.run_until(target_loss_tick);
    restored.run_until(target_loss_tick);
    first.kill_target(first.target);
    restored.kill_target(restored.target);
    const auto first_issued =
        first.wait_for_retarget(first.replacement, target_loss_tick + retarget_interval_ticks);
    const auto restored_issued = restored.wait_for_retarget(
        restored.replacement, target_loss_tick + retarget_interval_ticks
    );
    CHECK(first_issued == restored_issued);
    first.kill_target(first.replacement);
    restored.kill_target(restored.replacement);
    // A third enemy is available, but the second loss must observe the
    // cooldown rather than issue another attack at the saved tick.
    first.reuse_target_slot();
    restored.reuse_target_slot();
    constexpr uint32_t cooldown_elapsed_ticks = 1;
    const uint32_t saved_tick = first.fixture.match->simulation().tick + cooldown_elapsed_ticks;
    first.run_until(saved_tick);
    restored.run_until(saved_tick);
    compare_continuation_state(first, restored);
    const auto saved = retarget_bytes(first.fixture);
    const uint32_t next_early_tick = read_integer(saved, next_early_offset);
    CHECK(next_early_tick > saved_tick);
    CHECK(saved[land_target_record + target_binding_offset] == lost_binding);
    CHECK(!first.all_retargeted(first.target));
    const uint32_t native_deadline = first.strike().next_tick;
    CHECK(native_deadline != saved_tick + strike_refresh_ticks);

    Fixture fresh(options_for(retarget_version));
    restore_retarget(restored.fixture, retarget_bytes(fresh));
    recreate_ai_without_task_clocks(restored);
    CHECK(restored.strike().next_tick == 0 && restored.strike().attacking == 0);
    restore_retarget(restored.fixture, saved);
    restored.fixture.match->finish_load();
    restored.fixture.match->run_load_pass();
    CHECK(restored.fixture.match->fault() == nullptr);
    CHECK(restored.fixture.match->simulation().tick == saved_tick);
    CHECK(restored.strike().next_tick == native_deadline);
    CHECK(restored.strike().attacking == first.strike().attacking);
    CHECK(!restored.all_retargeted(restored.target));
    const auto after_load = retarget_bytes(restored.fixture);
    CHECK(read_integer(after_load, next_early_offset) == next_early_tick);
    CHECK(after_load[cursor_offset] == saved[cursor_offset]);
    compare_continuation_state(first, restored);

    while (first.fixture.match->simulation().tick <= next_early_tick) {
        first.fixture.run(1);
        restored.fixture.run(1);
        compare_continuation_state(first, restored);
    }
    CHECK(first.all_retargeted(first.target));
    CHECK(restored.all_retargeted(restored.target));
    first.check_attack(first.target, next_early_tick);
    restored.check_attack(restored.target, next_early_tick);
}

/// Shares early retries fairly between two strikes that keep losing their targets.
void repeated_failed_retries_serve_land_and_naval_strikes_fairly() {
    StrikeFixture battle(retarget_version);
    (void)battle.add_naval_strike();
    battle.run_until(target_loss_tick);
    battle.kill_target(battle.target);
    battle.kill_target(battle.replacement);
    std::array<uint32_t, 2> deadlines = {
        battle.strike().next_tick, battle.controller->tasks[naval_squad].next_tick
    };
    std::array<uint32_t, 2> attempts{};
    std::optional<uint32_t> previous_attempt_tick;
    constexpr uint32_t fairness_ticks = retarget_interval_ticks * 5;
    for (uint32_t elapsed = 0; elapsed < fairness_ticks; ++elapsed) {
        battle.fixture.run(1);
        const uint32_t tick = battle.fixture.match->simulation().tick;
        const std::array<uint32_t, 2> next_deadlines = {
            battle.strike().next_tick, battle.controller->tasks[naval_squad].next_tick
        };
        uint32_t changed_tasks = 0;
        for (std::size_t task = 0; task < deadlines.size(); ++task) {
            if (next_deadlines[task] == deadlines[task])
                continue;
            ++attempts[task];
            ++changed_tasks;
        }
        CHECK(changed_tasks <= 1);
        if (changed_tasks != 0) {
            if (previous_attempt_tick)
                CHECK(tick - *previous_attempt_tick >= retarget_interval_ticks);
            previous_attempt_tick = tick;
        }
        deadlines = next_deadlines;
    }
    CHECK(attempts[0] >= 2);
    CHECK(attempts[1] >= 2);
    CHECK(attempts[0] + attempts[1] <= 1 + fairness_ticks / retarget_interval_ticks);
}

/// Resumes the next competing strike after the other task spent the global budget.
void round_robin_cursor_continues_after_restore() {
    StrikeFixture first(retarget_version);
    StrikeFixture restored(retarget_version);
    (void)first.add_naval_strike();
    (void)restored.add_naval_strike();
    first.run_until(target_loss_tick);
    restored.run_until(target_loss_tick);
    first.kill_target(first.target);
    first.kill_target(first.replacement);
    restored.kill_target(restored.target);
    restored.kill_target(restored.replacement);
    const auto old_land_deadline = first.strike().next_tick;
    const auto old_naval_deadline = first.controller->tasks[naval_squad].next_tick;
    first.fixture.run(1);
    restored.fixture.run(1);
    CHECK(first.strike().next_tick != old_land_deadline);
    CHECK(first.controller->tasks[naval_squad].next_tick == old_naval_deadline);
    const auto saved = retarget_bytes(first.fixture);
    CHECK(saved[cursor_offset] == computer_player * 2 + 1);
    CHECK(read_integer(saved, next_early_offset) > first.fixture.match->simulation().tick);
    Fixture fresh(options_for(retarget_version));
    restore_retarget(restored.fixture, retarget_bytes(fresh));
    restore_retarget(restored.fixture, saved);
    const auto next_service_tick = read_integer(saved, next_early_offset);
    const auto retained_land_deadline = first.strike().next_tick;
    while (first.fixture.match->simulation().tick < next_service_tick) {
        first.fixture.run(1);
        restored.fixture.run(1);
        compare_continuation_state(first, restored);
    }
    CHECK(first.strike().next_tick == retained_land_deadline);
    CHECK(
        first.controller->tasks[naval_squad].next_tick == next_service_tick + strike_refresh_ticks
    );
}

/// Counts turns claimed by a custom seat without running the built-in player.
struct ClaimedSeat {
    uint32_t turns{};
    bool claimed{true};

    /// Claims the computer player's turn while leaving its orders untouched.
    static bool play(void* context, runtime::Match&, uint8_t) {
        auto& seat = *static_cast<ClaimedSeat*>(context);
        ++seat.turns;
        return seat.claimed;
    }
};

/// Waits for the built-in controller when a custom seat has claimed its turns.
void claimed_seat_does_not_advance_the_builtin_strike() {
    StrikeFixture battle(retarget_version);
    battle.run_until(target_loss_tick);
    ClaimedSeat seat;
    runtime::SeatHooks hooks;
    hooks.context = &seat;
    hooks.play = ClaimedSeat::play;
    battle.fixture.match->set_seat_hooks(computer_player, hooks);
    battle.kill_target(battle.target);
    constexpr uint32_t claimed_ticks = retarget_interval_ticks * 2;
    battle.fixture.run(claimed_ticks);
    CHECK(seat.turns == claimed_ticks);
    CHECK(battle.strike().next_tick == battle.regular_refresh_tick);
    CHECK(!battle.all_retargeted(battle.replacement));
    battle.fixture.match->set_seat_hooks(computer_player, {});
    const uint32_t resumed_tick = battle.fixture.match->simulation().tick;
    const auto issued =
        battle.wait_for_retarget(battle.replacement, resumed_tick + retarget_interval_ticks);
    CHECK(issued > resumed_tick);
}

/// Retargets normally when a custom hook lets the built-in player take its turn.
void unclaimed_seat_turn_keeps_builtin_retargeting() {
    StrikeFixture battle(retarget_version);
    battle.run_until(target_loss_tick);
    ClaimedSeat seat;
    seat.claimed = false;
    runtime::SeatHooks hooks;
    hooks.context = &seat;
    hooks.play = ClaimedSeat::play;
    battle.fixture.match->set_seat_hooks(computer_player, hooks);
    battle.kill_target(battle.target);
    const auto issued =
        battle.wait_for_retarget(battle.replacement, target_loss_tick + retarget_interval_ticks);
    CHECK(seat.turns != 0);
    CHECK(issued > target_loss_tick);
    CHECK(issued < battle.regular_refresh_tick);
}

/// Runs one case and reports its failure without hiding the remaining cases.
template <typename Function>
int run_case(std::string_view name, Function function) {
    try {
        function();
        std::cout << "PASS " << name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        return 1;
    }
}
} // namespace

/// Runs the actual-match strike target lifetime and scheduling checks.
int main() {
    int failures = 0;
    failures += run_case("earlier presets", old_presets_keep_the_regular_refresh);
    failures +=
        run_case("dead target", dead_target_refreshes_the_squad_before_the_regular_deadline);
    failures += run_case(
        "short cooldown before movement", early_target_loss_respects_the_short_cooldown_then_moves
    );
    failures += run_case(
        "short-lived targets keep moving", short_lived_healthy_targets_resume_goals_and_motion
    );
    failures += run_case("live target", live_target_does_not_add_command_refreshes);
    failures += run_case(
        "earlier pending-target selection", earlier_presets_keep_the_pending_target_selection
    );
    failures +=
        run_case("healthy strike target", enhanced_strike_skips_unhealthy_live_target_bodies);
    failures += run_case(
        "pending targets retry cooldown",
        all_pending_targets_keep_retries_bounded_without_new_attacks
    );
    failures += run_case("no enemy", no_enemy_does_not_retry_on_every_tick);
    failures +=
        run_case("reused target slot", reusing_the_target_slot_still_requires_a_new_command);
    failures += run_case("transferred target", transferred_target_is_no_longer_a_hostile_binding);
    failures += run_case("malformed rule state", malformed_rule_state_is_rejected_transactionally);
    failures += run_case(
        "owner-local rule-state digest",
        owner_local_retarget_state_does_not_change_the_shared_digest
    );
    failures += run_case("saved target loss", rule_state_continues_after_target_loss);
    failures += run_case("saved slot reuse", rule_state_continues_after_immediate_slot_reuse);
    failures += run_case("saved retry cooldown", rule_state_continues_during_retarget_cooldown);
    failures += run_case("fresh AI task clocks", fresh_ai_heap_continues_saved_strike_clocks);
    failures +=
        run_case("load-pass retry cooldown", load_pass_keeps_an_unexpired_retarget_cooldown);
    failures += run_case(
        "land and naval retry fairness", repeated_failed_retries_serve_land_and_naval_strikes_fairly
    );
    failures += run_case("saved round-robin cursor", round_robin_cursor_continues_after_restore);
    failures += run_case("claimed seat", claimed_seat_does_not_advance_the_builtin_strike);
    failures += run_case("unclaimed seat", unclaimed_seat_turn_keeps_builtin_retargeting);
    return failures == 0 ? 0 : 1;
}
