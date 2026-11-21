// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "ai_retarget.hpp"
#include "enhanced_state.hpp"
#include "match_state.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime/command.hpp"

#include <algorithm>
#include <limits>

namespace oa::sim::match_runtime {
namespace {
constexpr uint32_t state_magic = 0x47545241u;
constexpr uint32_t state_version = 2;
constexpr size_t magic_offset = 0, version_offset = 4, unit_count_offset = 8;
constexpr size_t next_early_offset = 12, prepared_tick_offset = 16;
constexpr size_t cursor_offset = 20, prepared_valid_offset = 21, builtin_players_offset = 22;
constexpr uint16_t builtin_player_mask = (uint16_t(1) << OA_PLAYER_COUNT) - 1;
constexpr size_t slot_offset = 0, owner_offset = 2, binding_offset = 3;
constexpr size_t issue_tick_offset = 4, retry_tick_offset = 8;
constexpr size_t native_next_offset = 12, native_clock_offset = 16, native_attacking_offset = 17;
constexpr size_t target_reserved_offset = 18;

/// Writes the low bytes of an integer in little-endian order.
void put(std::span<uint8_t> bytes, size_t at, uint32_t value, size_t count = 4) noexcept {
    for (size_t byte = 0; byte < count; ++byte)
        bytes[at + byte] = static_cast<uint8_t>(value >> (byte * 8));
}

/// Reads an integer from its little-endian bytes.
uint32_t get(std::span<const uint8_t> bytes, size_t at, size_t count = 4) noexcept {
    uint32_t value = 0;
    for (size_t byte = 0; byte < count; ++byte)
        value |= static_cast<uint32_t>(bytes[at + byte]) << (byte * 8);
    return value;
}

/// Maps a retained target to the canonical strike task index.
size_t strike_squad(size_t target) noexcept {
    return static_cast<size_t>(
        target == 0 ? sim::ai::Squad::land_strike : sim::ai::Squad::naval_strike
    );
}
} // namespace

void AiRetargetState::lose_target(uint16_t slot) noexcept {
    for (auto& target : targets)
        if (target.binding == AiStrikeBinding::bound && target.slot == slot)
            target.binding = AiStrikeBinding::lost;
}

std::span<const uint8_t> AiRetargetState::state_bytes(size_t unit_count) noexcept {
    encoded.fill(0);
    put(encoded, magic_offset, state_magic);
    put(encoded, version_offset, state_version);
    put(encoded, unit_count_offset, static_cast<uint32_t>(unit_count));
    put(encoded, next_early_offset, next_early_tick);
    put(encoded, prepared_tick_offset, prepared_tick);
    encoded[cursor_offset] = selection_cursor;
    encoded[prepared_valid_offset] = prepared_valid;
    put(encoded, builtin_players_offset, builtin_players, 2);
    for (size_t index = 0; index < targets.size(); ++index) {
        const auto& target = targets[index];
        const size_t at = ai_retarget_header_bytes + index * ai_retarget_target_bytes;
        put(encoded, at + slot_offset, target.slot, 2);
        encoded[at + owner_offset] = target.owner;
        encoded[at + binding_offset] = static_cast<uint8_t>(target.binding);
        put(encoded, at + issue_tick_offset, target.last_issue_tick);
        put(encoded, at + retry_tick_offset, target.retry_not_before);
        put(encoded, at + native_next_offset, target.native_next_tick);
        encoded[at + native_clock_offset] = target.native_clock_valid;
        encoded[at + native_attacking_offset] = target.native_attacking;
    }
    return encoded;
}

bool AiRetargetState::restore(std::span<const uint8_t> bytes, size_t unit_count) noexcept {
    if (bytes.size() != ai_retarget_state_bytes ||
        unit_count > size_t(std::numeric_limits<uint16_t>::max()) + 1 || unit_count == 0 ||
        get(bytes, magic_offset) != state_magic || get(bytes, version_offset) != state_version ||
        get(bytes, unit_count_offset) != unit_count || bytes[cursor_offset] >= targets.size() ||
        bytes[prepared_valid_offset] > 1 ||
        (get(bytes, builtin_players_offset, 2) & ~uint32_t(builtin_player_mask)) != 0 ||
        (bytes[prepared_valid_offset] == 0 && get(bytes, prepared_tick_offset) != 0))
        return false;
    AiRetargetState restored;
    restored.next_early_tick = get(bytes, next_early_offset);
    restored.prepared_tick = get(bytes, prepared_tick_offset);
    restored.selection_cursor = bytes[cursor_offset];
    restored.prepared_valid = bytes[prepared_valid_offset] != 0;
    restored.builtin_players = static_cast<uint16_t>(get(bytes, builtin_players_offset, 2));
    for (size_t index = 0; index < targets.size(); ++index) {
        auto& target = restored.targets[index];
        const size_t at = ai_retarget_header_bytes + index * ai_retarget_target_bytes;
        target.slot = static_cast<uint16_t>(get(bytes, at + slot_offset, 2));
        target.owner = bytes[at + owner_offset];
        const auto binding = bytes[at + binding_offset];
        if (binding > static_cast<uint8_t>(AiStrikeBinding::lost))
            return false;
        target.binding = static_cast<AiStrikeBinding>(binding);
        target.last_issue_tick = get(bytes, at + issue_tick_offset);
        target.retry_not_before = get(bytes, at + retry_tick_offset);
        target.native_next_tick = get(bytes, at + native_next_offset);
        if (bytes[at + native_clock_offset] > 1 || bytes[at + native_attacking_offset] > 1 ||
            bytes[at + target_reserved_offset] != 0 || bytes[at + target_reserved_offset + 1] != 0)
            return false;
        target.native_clock_valid = bytes[at + native_clock_offset] != 0;
        target.native_attacking = bytes[at + native_attacking_offset] != 0;
        if (!target.native_clock_valid && (target.native_next_tick != 0 || target.native_attacking))
            return false;
        restored.restore_native_clock[index] = target.native_clock_valid;
        if (target.binding == AiStrikeBinding::empty) {
            if (target.slot != 0 || target.owner != 0 || target.last_issue_tick != 0 ||
                target.retry_not_before != 0)
                return false;
        } else if (target.slot == 0 || target.slot >= unit_count || target.owner >= OA_PLAYER_COUNT)
            return false;
    }
    *this = restored;
    return true;
}

void Match::EnhancedState::prepare_ai_retarget(Match& match, uint8_t player) {
    auto* computers = static_cast<sim::ai::ComputerPlayers*>(match.computer_player_state().get());
    if (computers == nullptr)
        return;
    if (!computers->players[player].present)
        sim::ai::computer_players_adopt(computers, sim::ai::match_computer_host(match), player);
    auto& state = ai_retarget;
    const uint32_t tick = match.simulation_.tick;
    const auto computer_player = [&](size_t owner) {
        const auto& record = match_player(match, owner);
        return record.in_use != 0 && record.status == OA_PLAYER_STATUS_COMPUTER &&
               sim::ai::player_has_controller(record) && computers->players[owner].present;
    };
    const auto eligible_player = [&](size_t owner) {
        return computer_player(owner) && (state.builtin_players & (uint16_t(1) << owner)) != 0;
    };
    const auto task_for = [&](size_t index) -> sim::ai::ComputerTask& {
        return computers->players[index / ai_strike_tasks_per_player]
            .tasks[strike_squad(index % ai_strike_tasks_per_player)];
    };
    // The native controller is rebuilt on load. Its retained strike clocks
    // apply after initialization and before either periodic or early dispatch.
    for (size_t task_index = 0; task_index < ai_strike_tasks_per_player; ++task_index) {
        const size_t index = size_t(player) * ai_strike_tasks_per_player + task_index;
        if (!state.restore_native_clock[index])
            continue;
        state.restore_native_clock[index] = false;
        const auto& target = state.targets[index];
        auto& task = task_for(index);
        if (target.native_clock_valid && task.kind == sim::ai::TaskKind::strike) {
            task.next_tick = target.native_next_tick;
            task.attacking = target.native_attacking;
        }
    }
    if (!state.prepared_valid || state.prepared_tick != tick) {
        state.prepared_valid = true;
        state.prepared_tick = tick;
        for (size_t index = 0; index < state.targets.size(); ++index) {
            auto& target = state.targets[index];
            const size_t owner = index / ai_strike_tasks_per_player;
            if (!computer_player(owner) || task_for(index).kind != sim::ai::TaskKind::strike) {
                target = {};
                continue;
            }
            if (target.binding != AiStrikeBinding::bound)
                continue;
            const auto* slot =
                target.slot < match.slots_.size() ? &match.slots_[target.slot] : nullptr;
            if (slot == nullptr || slot->unit == nullptr || slot->record.type_index == 0 ||
                !oa::unit_is_live_target(slot->record.flags) || slot->record.health <= 0 ||
                slot->record.owner_index != target.owner ||
                match.allied(static_cast<uint8_t>(owner), target.owner))
                target.binding = AiStrikeBinding::lost;
        }
    }
    const bool cohorts = match.enhanced_features_.has(data::enhanced_mode::Feature::cohort_targets);
    if (!cohorts && state.next_early_tick <= tick) {
        for (size_t step = 0; step < state.targets.size(); ++step) {
            const size_t index = (state.selection_cursor + step) % state.targets.size();
            auto& target = state.targets[index];
            const size_t owner = index / ai_strike_tasks_per_player;
            if (!eligible_player(owner))
                continue;
            auto& task = task_for(index);
            if (target.binding != AiStrikeBinding::lost || target.retry_not_before > tick ||
                task.kind != sim::ai::TaskKind::strike || task.attacking == 0 ||
                task.next_tick <= tick)
                continue;
            // A seat is serviced only after it actually falls through to the
            // built-in controller. A later seat waits for its own callback.
            if (owner == player) {
                task.next_tick = tick;
                target.retry_not_before = tick + ai_retarget_cooldown_ticks;
                state.next_early_tick = tick + ai_retarget_cooldown_ticks;
                state.selection_cursor = static_cast<uint8_t>((index + 1) % state.targets.size());
            }
            break;
        }
    }
    // A natural deadline that finds no enemy also observes the retry interval.
    for (size_t task_index = 0; task_index < ai_strike_tasks_per_player; ++task_index) {
        const size_t index = size_t(player) * ai_strike_tasks_per_player + task_index;
        auto& target = state.targets[index];
        const auto& task = task_for(index);
        if (target.binding != AiStrikeBinding::empty && task.kind == sim::ai::TaskKind::strike &&
            task.next_tick <= tick)
            target.retry_not_before = tick + ai_retarget_cooldown_ticks;
    }
}

void Match::EnhancedState::refresh_ai_cohort_targets(Match& match, uint8_t player) {
    if (!match.enhanced_features_.has(data::enhanced_mode::Feature::cohort_targets))
        return;
    auto* computers = static_cast<sim::ai::ComputerPlayers*>(match.computer_player_state().get());
    if (computers == nullptr || !computers->players[player].present)
        return;
    auto& state = ai_retarget;
    const uint32_t tick = match.simulation_.tick;
    if (state.next_early_tick > tick)
        return;
    for (size_t step = 0; step < state.targets.size(); ++step) {
        const size_t index = (state.selection_cursor + step) % state.targets.size();
        const size_t owner = index / ai_strike_tasks_per_player;
        const auto& record = match_player(match, owner);
        if (record.in_use == 0 || record.status != OA_PLAYER_STATUS_COMPUTER ||
            !sim::ai::player_has_controller(record) || !computers->players[owner].present ||
            (state.builtin_players & (uint16_t(1) << owner)) == 0)
            continue;
        auto& task =
            computers->players[owner].tasks[strike_squad(index % ai_strike_tasks_per_player)];
        auto& retained = state.targets[index];
        if (task.kind != sim::ai::TaskKind::strike || task.attacking == 0 ||
            task.next_tick <= tick || retained.retry_not_before > tick ||
            !retained.native_clock_valid || retained.native_next_tick != task.next_tick ||
            match.squad_members(static_cast<uint8_t>(owner), static_cast<uint32_t>(task.squad))
                .empty())
            continue;
        // The next eligible seat keeps the shared budget until its built-in
        // controller actually runs. One squad is inspected per cooldown.
        if (owner != player)
            break;
        state.next_early_tick = tick + ai_retarget_cooldown_ticks;
        state.selection_cursor = static_cast<uint8_t>((index + 1) % state.targets.size());
        std::array<uint64_t, sim::ai::strike_refresh_mask_words> sources{};
        uint32_t count = 0;
        uint32_t player_unit_count = 0;
        const auto* units = oa::world_player_units(&match.state(), &record, &player_unit_count);
        for (uint32_t at = 0; units != nullptr && at < player_unit_count; ++at) {
            const auto& unit = units[at];
            if (unit.type_index == 0 || unit.owner_index != owner ||
                unit.squad != static_cast<int32_t>(task.squad) ||
                !oa::unit_is_live_target(unit.flags) || unit.health <= 0 ||
                unit.build_remaining != 0)
                continue;
            const auto slot = oa::world_unit_slot(&match.state(), &unit);
            if (slot >= match.slots_.size() || match.slots_[slot].unit == nullptr)
                continue;
            const auto* current = match.runtime_order(match.slots_[slot].unit->primary);
            bool lost = match.slots_[slot].unit->primary == nullptr;
            if (current != nullptr && current->unit == match.slots_[slot].unit &&
                current->order.kind == attack_chase_kind && current->order.next == nullptr &&
                current->attack.leash == 0 &&
                (current->extra.command_flags & command_has_target) != 0 &&
                (current->extra.command_flags & command_has_point) == 0) {
                const auto* target = current->attack.target;
                lost = target == nullptr;
                if (target != nullptr) {
                    const auto target_slot = oa::world_unit_slot(&match.state(), &target->record);
                    lost = target_slot >= match.slots_.size() ||
                           match.slots_[target_slot].unit != target ||
                           target->record.type_index == 0 ||
                           !oa::unit_is_live_target(target->record.flags) ||
                           target->record.health <= 0 ||
                           match.allied(player, target->record.owner_index);
                }
            }
            if (!lost)
                continue;
            constexpr size_t mask_bits = std::numeric_limits<uint64_t>::digits;
            if (slot / mask_bits >= sources.size())
                continue;
            sources[slot / mask_bits] |= uint64_t(1) << (slot % mask_bits);
            ++count;
        }
        if (count != 0) {
            // Empty bindings must keep their empty codec representation.
            // Successful callbacks retain their own target and retry deadline.
            if (retained.binding != AiStrikeBinding::empty)
                retained.retry_not_before = tick + ai_retarget_cooldown_ticks;
            (void)sim::ai::computer_player_refresh_strike_targets(
                computers, sim::ai::match_computer_host(match), player, task.squad, sources
            );
        }
        break;
    }
}

void Match::EnhancedState::remember_ai_attack(Match& match, uint16_t source, uint16_t target) {
    auto& state = ai_retarget;
    if (!state.builtin_dispatch || source >= match.slots_.size() || target >= match.slots_.size())
        return;
    const auto& from = match.slots_[source].record;
    const auto& aimed = match.slots_[target].record;
    if (from.owner_index != state.builtin_player ||
        match.allied(from.owner_index, aimed.owner_index))
        return;
    size_t task_index = 0;
    if (from.squad == static_cast<int32_t>(sim::ai::Squad::land_strike))
        task_index = 0;
    else if (from.squad == static_cast<int32_t>(sim::ai::Squad::naval_strike))
        task_index = 1;
    else
        return;
    const size_t index = size_t(from.owner_index) * ai_strike_tasks_per_player + task_index;
    auto& retained = state.targets[index];
    retained.slot = target;
    retained.owner = aimed.owner_index;
    retained.binding = AiStrikeBinding::bound;
    retained.last_issue_tick = match.simulation_.tick;
    retained.retry_not_before = match.simulation_.tick + ai_retarget_cooldown_ticks;
}

void Match::EnhancedState::capture_ai_retarget(Match& match, uint8_t player) {
    const auto* computers =
        static_cast<const sim::ai::ComputerPlayers*>(match.computer_player_state().get());
    if (computers == nullptr || !computers->players[player].present)
        return;
    for (size_t task_index = 0; task_index < ai_strike_tasks_per_player; ++task_index) {
        const auto& task = computers->players[player].tasks[strike_squad(task_index)];
        auto& target =
            ai_retarget.targets[size_t(player) * ai_strike_tasks_per_player + task_index];
        if (task.kind != sim::ai::TaskKind::strike)
            continue;
        target.native_clock_valid = true;
        target.native_next_tick = task.next_tick;
        target.native_attacking = task.attacking != 0;
    }
}

} // namespace oa::sim::match_runtime
