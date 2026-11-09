// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "ai_retarget.hpp"
#include "enhanced_state.hpp"
#include "match_state.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ai.hpp"

#include <algorithm>
#include <limits>

namespace oa::sim::match_runtime {
namespace {
constexpr uint32_t state_magic = 0x47545241u;
constexpr uint32_t state_version = 1;
constexpr size_t magic_offset = 0, version_offset = 4, unit_count_offset = 8;
constexpr size_t next_early_offset = 12, prepared_tick_offset = 16;
constexpr size_t cursor_offset = 20, prepared_valid_offset = 21, builtin_players_offset = 22;
constexpr uint16_t builtin_player_mask = (uint16_t(1) << OA_PLAYER_COUNT) - 1;
constexpr size_t slot_offset = 0, owner_offset = 2, binding_offset = 3;
constexpr size_t issue_tick_offset = 4, retry_tick_offset = 8;

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
    if (state.next_early_tick <= tick) {
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
    state.targets[index] = {
        target,
        aimed.owner_index,
        AiStrikeBinding::bound,
        match.simulation_.tick,
        match.simulation_.tick + ai_retarget_cooldown_ticks
    };
}

} // namespace oa::sim::match_runtime
