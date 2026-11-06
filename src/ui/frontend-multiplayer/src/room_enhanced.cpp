// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room requires every machine to agree on Enhanced Mode before START.
#include "oa/ui/frontend_multiplayer/room_enhanced.hpp"

#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/room_limits.hpp"

#include <cstdint>
#include <span>

namespace oa::ui::frontend_multiplayer {

namespace {

constexpr const char* mode_line = "*** Enhanced Mode differs. Every player must select the same "
                                  "mode before joining the battle room.";
constexpr const char* version_line =
    "*** Enhanced Mode requires the same supported version on every machine.";
constexpr const char* rules_line =
    "*** Enhanced Mode rules differ. Every player must use the same mod and match rules.";

/// Tells whether a seat belongs to a machine whose agreement is required.
///
/// Local and remote computer seats are covered by the human who runs them.
/// An unreceived setup block cannot exempt a seated remote machine.
///
/// @param lobby Lobby state.
/// @param slot Player slot.
/// @return True for a seated human or watcher.
bool machine_seat(Lobby& lobby, int32_t slot) noexcept {
    const auto& player = slot_player(lobby, slot);
    if (player.in_use == 0)
        return false;
    if (player.status == kSlotLocal)
        return true;
    if (player.status != kSlotRemote)
        return false;
    const auto* info = player_info(lobby, player);
    if (lobby.presence_records.block_player[slot] != player.player_id || info == nullptr)
        return true;
    return info->state != kInfoStateDefeated || (info->options & option::watcher) != 0;
}

/// A machine's own presence record, only after its own setup block identifies it.
///
/// @param lobby Lobby state.
/// @param slot Player slot.
/// @return The record, or an empty span while the machine is unknown or classic.
std::span<const uint8_t> machine_record(Lobby& lobby, int32_t slot) noexcept {
    const auto& player = slot_player(lobby, slot);
    if (player.status == kSlotLocal)
        return lobby_presence_record(lobby, slot);
    const auto* info = player_info(lobby, player);
    if (info == nullptr || lobby.presence_records.block_player[slot] != player.player_id ||
        !netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info)))
        return {};
    return lobby_presence_record(lobby, slot);
}

} // namespace

const char* lobby_enhanced_start_problem(Lobby& lobby) noexcept {
    if (lobby.game == nullptr)
        return nullptr;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot)
        return nullptr;
    const auto host_record = machine_record(lobby, host);
    const auto host_version =
        netgame::presence_enhanced_mode_version(host_record.data(), host_record.size()).value_or(0);
    const auto host_hash = netgame::presence_sim_hash(host_record.data(), host_record.size());
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        if (!machine_seat(lobby, slot))
            continue;
        const auto record = machine_record(lobby, slot);
        const auto version =
            netgame::presence_enhanced_mode_version(record.data(), record.size()).value_or(0);
        if ((host_version == 0) != (version == 0))
            return mode_line;
        if (host_version == 0)
            continue;
        if (version != host_version)
            return version_line;
        const auto hash = netgame::presence_sim_hash(record.data(), record.size());
        if (!host_hash || !hash || *host_hash != *hash)
            return rules_line;
    }
    return nullptr;
}

bool lobby_enhanced_allow_start(Lobby& lobby) noexcept {
    return lobby_enhanced_start_problem(lobby) == nullptr;
}

void lobby_note_enhanced_refused_start(Lobby& lobby) noexcept {
    if (const char* problem = lobby_enhanced_start_problem(lobby))
        lobby_post_limits_line(lobby, problem, lobby.game->local_player_index);
}

} // namespace oa::ui::frontend_multiplayer
