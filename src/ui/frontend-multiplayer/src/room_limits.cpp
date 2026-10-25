// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether a battle room whose profile is past 3.1c's limits may start.
#include "oa/ui/frontend_multiplayer/room_limits.hpp"

#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/platform/log_files.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace oa::ui::frontend_multiplayer {

namespace {

// clang-format off
/// "*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than 256 weapon types."
constexpr const char* kWeaponTypesLine =
    "*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than 256 weapon types.";

/// "*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than five sides."
constexpr const char* kSidesLine =
    "*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than five sides.";

/// "*** {player} needs a newer Open Annihilation for this mod."
constexpr const char* kNewerLine =
    "*** {player} needs a newer Open Annihilation for this mod.";

/// "This mod needs every player on Open Annihilation. The host plays Total Annihilation 3.1c."
constexpr const char* kLeaveLine =
    "This mod needs every player on Open Annihilation. The host plays Total Annihilation 3.1c.";
// clang-format on

/// Tells whether a seated remote human or watcher is on 3.1c.
///
/// The block has to be that player's. A computer player is not one.
/// Same seat rules as the mutator room's classic player. Allocates nothing.
///
/// @param lobby Lobby state.
/// @param player_slot Player slot.
/// @return True when that slot's block lacks the engine signature.
bool classic_seated(Lobby& lobby, int32_t player_slot) noexcept {
    if (lobby.game == nullptr || player_slot < 0 || player_slot >= kSlotCount)
        return false;
    const auto& player = slot_player(lobby, player_slot);
    if (player.in_use == 0 || player.status != kSlotRemote)
        return false;
    const auto* info = player_info(lobby, player);
    if (info == nullptr)
        return false;
    const bool watcher = (info->options & option::watcher) != 0;
    if (!slot_remote_playing(lobby, player) && !watcher)
        return false;
    if (lobby.presence_records.block_player[player_slot] != player.player_id)
        return false;
    return !netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info));
}

/// Tells whether a seated remote human or watcher carries the engine signature.
///
/// @param lobby Lobby state.
/// @param player_slot Player slot.
/// @return True when that slot's own block carries the signature.
bool remote_oa_seat(Lobby& lobby, int32_t player_slot) noexcept {
    if (lobby.game == nullptr || player_slot < 0 || player_slot >= kSlotCount)
        return false;
    const auto& player = slot_player(lobby, player_slot);
    if (player.in_use == 0 || player.status != kSlotRemote)
        return false;
    const auto* info = player_info(lobby, player);
    if (info == nullptr)
        return false;
    const bool watcher = (info->options & option::watcher) != 0;
    if (!slot_remote_playing(lobby, player) && !watcher)
        return false;
    if (lobby.presence_records.block_player[player_slot] != player.player_id)
        return false;
    return netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info));
}

/// The catalogue line with `{player}` filled, or the line itself when no seat is named.
///
/// @param lobby Lobby state.
/// @param words The English sentence.
/// @param player_slot The player the line names, or a negative value for none.
/// @return The line the room shows.
std::string shown_line(Lobby& lobby, const char* words, int32_t player_slot) {
    std::string line(oa::data::languages::interface_text(words));
    if (lobby.game == nullptr || player_slot < 0 || player_slot >= kSlotCount)
        return line;
    const auto& named = slot_player(lobby, player_slot);
    const std::string_view who(named.name, ::strnlen(named.name, sizeof named.name));
    const std::string who_utf8 = netgame::chat_utf8(who);
    const auto mark = line.find("{player}");
    if (mark != std::string::npos)
        line.replace(mark, std::string_view("{player}").size(), who_utf8);
    return line;
}

} // namespace

bool lobby_room_oa_only(const Lobby& lobby) noexcept {
    return netgame::oa_only(lobby.wire_rules);
}

const char* lobby_limits_classic_line(const Lobby& lobby) noexcept {
    if (lobby.wire_rules.weapon_types > netgame::wire_base_weapon_types)
        return kWeaponTypesLine;
    return kSidesLine;
}

const char* lobby_limits_newer_line() noexcept {
    return kNewerLine;
}

const char* lobby_limits_leave_line() noexcept {
    return kLeaveLine;
}

bool lobby_limits_covered(Lobby& lobby, int32_t player_slot) noexcept {
    if (player_slot < 0 || player_slot >= kSlotCount)
        return false;
    const auto record = lobby_presence_record(lobby, player_slot);
    const auto limits = netgame::presence_limits(record.data(), record.size());
    if (!limits)
        return false;
    return limits->weapon_types >= lobby.wire_rules.weapon_types &&
           limits->sides >= lobby.wire_rules.sides;
}

bool lobby_limits_remote_oa(Lobby& lobby, int32_t player_slot) noexcept {
    return remote_oa_seat(lobby, player_slot);
}

LimitsBlock lobby_limits_block(Lobby& lobby) noexcept {
    if (lobby.game == nullptr || !netgame::oa_only(lobby.wire_rules))
        return {};
    for (int32_t player_slot = 0; player_slot < kSlotCount; ++player_slot) {
        if (classic_seated(lobby, player_slot))
            return {LimitsHold::classic_player, player_slot};
    }
    for (int32_t player_slot = 0; player_slot < kSlotCount; ++player_slot) {
        if (remote_oa_seat(lobby, player_slot) && !lobby_limits_covered(lobby, player_slot))
            return {LimitsHold::older_build, player_slot};
    }
    return {};
}

bool lobby_limits_allow_start(Lobby& lobby) noexcept {
    return lobby_limits_block(lobby).hold == LimitsHold::none;
}

void lobby_post_limits_line(Lobby& lobby, const char* words, int32_t player_slot) noexcept {
    if (words == nullptr)
        return;
    // Whole, on one line, with the name filled. The chat list may split it.
    const std::string line = shown_line(lobby, words, player_slot);
    oa::platform::log_files::write_line(line);
    if (player_slot >= 0)
        lobby_post_classic_line(lobby, words, player_slot);
}

} // namespace oa::ui::frontend_multiplayer
