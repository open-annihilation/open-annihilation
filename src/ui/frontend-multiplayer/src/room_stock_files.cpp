// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A battle that counts for a ladder, which only the Boneyards launches:
// START is refused while any player's game files differ from Total
// Annihilation's, and the room names that player.
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include "oa/netgame/presence.hpp"
#include "oa/platform/log_files.hpp"

#include <cstdint>
#include <span>

namespace oa::ui::frontend_multiplayer {
namespace {

constexpr const char* kOtherLine = "*** {player}'s game files differ from Total Annihilation's; "
                                   "they must repair them before this battle can start.";
constexpr const char* kOwnLine =
    "*** Your game files differ from Total Annihilation's, so this battle cannot start. Leave "
    "it and repair them in Settings › Game Content.";

/// Tells whether a seat holds a player whose machine's files differ.
///
/// @param lobby Lobby state.
/// @param player_slot The slot.
/// @return True for a human of this machine or another whose record has the flag.
bool seat_differs(Lobby& lobby, int32_t player_slot) noexcept {
    const auto& player = slot_player(lobby, player_slot);
    if (player.in_use == 0 || player.status == kSlotComputer)
        return false;
    const std::span<const uint8_t> record = lobby_presence_record(lobby, player_slot);
    return netgame::presence_files_differ(record);
}

} // namespace

bool lobby_stock_files_room(const Lobby& lobby) noexcept {
    return lobby_launch_block_active(lobby) && lobby.launch_link.block->stock_files != 0;
}

int32_t lobby_stock_files_block(Lobby& lobby) noexcept {
    if (lobby.game == nullptr || !lobby_stock_files_room(lobby))
        return kNoSlot;
    for (int32_t player_slot = 0; player_slot < kSlotCount; ++player_slot)
        if (seat_differs(lobby, player_slot))
            return player_slot;
    return kNoSlot;
}

bool lobby_stock_files_allow_start(Lobby& lobby) noexcept {
    return lobby_stock_files_block(lobby) == kNoSlot;
}

const char* lobby_stock_files_line() noexcept {
    return kOtherLine;
}

const char* lobby_stock_files_own_line() noexcept {
    return kOwnLine;
}

void lobby_note_stock_files(Lobby& lobby) noexcept {
    if (lobby.game == nullptr)
        return;
    const bool counts = lobby_stock_files_room(lobby);
    for (int32_t player_slot = 0; player_slot < kSlotCount; ++player_slot) {
        auto& told = lobby.stock_files_told[player_slot];
        if (!counts || !seat_differs(lobby, player_slot)) {
            told = 0;
            continue;
        }
        const auto& player = slot_player(lobby, player_slot);
        if (told == player.player_id)
            continue;
        told = player.player_id;
        const bool own = player.status == kSlotLocal;
        const char* words = own ? kOwnLine : kOtherLine;
        lobby_post_classic_line(lobby, words, player_slot);
        oa::platform::log_files::write_line(
            own ? "multiplayer: this machine's game files differ from Total Annihilation's; "
                  "START is refused in this battle"
                : "multiplayer: a player's game files differ from Total Annihilation's; START is "
                  "refused in this battle"
        );
    }
}

} // namespace oa::ui::frontend_multiplayer
