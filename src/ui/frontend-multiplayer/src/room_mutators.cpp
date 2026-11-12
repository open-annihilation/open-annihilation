// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether the battle room may start, from the mutators each machine reports.
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::frontend_multiplayer {

namespace {

/// Names a hold the way the host's log prints it.
///
/// @param hold the hold
/// @return its enumerator name
const char* hold_name(MutatorsHold hold) noexcept {
    switch (hold) {
    case MutatorsHold::none:
        return "none";
    case MutatorsHold::host_not_built:
        return "host_not_built";
    case MutatorsHold::host_failed:
        return "host_failed";
    case MutatorsHold::classic_player:
        return "classic_player";
    case MutatorsHold::no_support:
        return "no_support";
    case MutatorsHold::receiving:
        return "receiving";
    case MutatorsHold::lacking:
        return "lacking";
    case MutatorsHold::failed:
        return "failed";
    case MutatorsHold::building:
        return "building";
    case MutatorsHold::other_result:
        return "other_result";
    case MutatorsHold::other_sim_hash:
        return "other_sim_hash";
    }
    return "none";
}

/// The first mutator of a peer that is not installed at the list's release.
///
/// @param states the peer's state word
/// @param total how many mutators the host's list names
/// @param[out] position that mutator's position
/// @return True when one is not has.
bool first_not_has(uint64_t states, uint8_t total, uint8_t* position) noexcept {
    const auto limit = total < netgame::presence_mutators_max
                           ? total
                           : static_cast<uint8_t>(netgame::presence_mutators_max);
    for (uint8_t at = 0; at < limit; ++at) {
        if (netgame::presence_mutator_state(states, at) == static_cast<uint8_t>(MutatorHave::has))
            continue;
        *position = at;
        return true;
    }
    return false;
}

/// The line posted when mutators come on and a 3.1c player is already seated.
constexpr const char* kClassicSeated =
    "*** {player} plays Total Annihilation 3.1c, which cannot play with mutators.";

/// Tells whether a seated remote human or watcher is on 3.1c.
///
/// The block has to be that player's. A computer player is not one.
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
    const bool watcher = player.in_use != 0 && (info->options & option::watcher) != 0;
    if (!slot_remote_playing(lobby, player) && !watcher)
        return false;
    if (lobby.presence_records.block_player[player_slot] != player.player_id)
        return false;
    return !netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info));
}

/// Why one peer holds START, or none.
///
/// @param host_state the host's mutator field
/// @param host_record the host's record
/// @param row_state the peer's mutator field; nothing when it sent none
/// @param row_record the peer's record
/// @param player_slot the peer's slot
/// @return The hold, or none.
MutatorsBlock peer_hold(
    const netgame::PresenceMutatorsState& host_state,
    std::span<const uint8_t> host_record,
    const netgame::PresenceMutatorsState* row_state,
    std::span<const uint8_t> row_record,
    int32_t player_slot
) noexcept {
    const uint8_t none = netgame::presence_mutator_index_none;
    if (row_state == nullptr)
        return {MutatorsHold::no_support, player_slot, none};
    if (row_state->digest != host_state.digest || row_state->held < host_state.total)
        return {MutatorsHold::receiving, player_slot, none};
    uint8_t lacking = 0;
    if (first_not_has(row_state->states, host_state.total, &lacking))
        return {MutatorsHold::lacking, player_slot, lacking};
    if (row_state->failed)
        return {MutatorsHold::failed, player_slot, row_state->index};
    if (!row_state->built)
        return {MutatorsHold::building, player_slot, none};
    if (row_state->layer_digest != host_state.layer_digest) {
        const uint8_t position = row_state->index != host_state.index
                                     ? std::min(row_state->index, host_state.index)
                                     : netgame::presence_mutator_index_none;
        return {MutatorsHold::other_result, player_slot, position};
    }
    const auto host_hash = netgame::presence_sim_hash(host_record.data(), host_record.size());
    const auto row_hash = netgame::presence_sim_hash(row_record.data(), row_record.size());
    if (host_hash && row_hash && *host_hash != *row_hash)
        return {MutatorsHold::other_sim_hash, player_slot, none};
    return {};
}

/// The first reason START waits, looking at every remote player or at one.
///
/// @param lobby Lobby state.
/// @param only_slot One slot, or a negative value for every slot in order.
/// @return The first hold, or none.
MutatorsBlock block_for(Lobby& lobby, int32_t only_slot) noexcept {
    if (lobby.game == nullptr)
        return {};
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot)
        return {};
    if (only_slot >= kSlotCount)
        return {};
    const int32_t first = only_slot < 0 ? 0 : only_slot;
    const int32_t end = only_slot < 0 ? kSlotCount : only_slot + 1;
    if (first < 0)
        return {};
    const auto host_record = lobby_presence_record(lobby, static_cast<int32_t>(host));
    const auto host_state =
        netgame::presence_mutators_state(host_record.data(), host_record.size());
    if (!host_state) {
        for (int32_t player_slot = first; player_slot < end; ++player_slot) {
            if (player_slot == static_cast<int32_t>(host))
                continue;
            if (!lobby_remote_oa_player(lobby, player_slot))
                continue;
            const auto row = lobby_presence_record(lobby, player_slot);
            if (netgame::presence_mutators_state(row.data(), row.size()))
                return {MutatorsHold::receiving, player_slot, netgame::presence_mutator_index_none};
        }
        return {};
    }
    // A failed build is named even when the built flag is also clear.
    if (host_state->failed)
        return {MutatorsHold::host_failed, static_cast<int32_t>(host), host_state->index};
    if (!host_state->built) {
        return {
            MutatorsHold::host_not_built,
            static_cast<int32_t>(host),
            netgame::presence_mutator_index_none
        };
    }
    for (int32_t player_slot = first; player_slot < end; ++player_slot) {
        if (classic_seated(lobby, player_slot))
            return {
                MutatorsHold::classic_player, player_slot, netgame::presence_mutator_index_none
            };
    }
    for (int32_t player_slot = first; player_slot < end; ++player_slot) {
        if (player_slot == static_cast<int32_t>(host))
            continue;
        if (!lobby_remote_oa_player(lobby, player_slot))
            continue;
        const auto row = lobby_presence_record(lobby, player_slot);
        const auto row_state = netgame::presence_mutators_state(row.data(), row.size());
        const auto hold = peer_hold(
            *host_state, host_record, row_state ? &*row_state : nullptr, row, player_slot
        );
        if (hold.hold != MutatorsHold::none)
            return hold;
    }
    return {};
}

} // namespace

bool lobby_remote_oa_player(Lobby& lobby, int32_t player_slot) noexcept {
    if (lobby.game == nullptr || player_slot < 0 || player_slot >= kSlotCount)
        return false;
    const auto& player = slot_player(lobby, player_slot);
    if (!slot_remote_playing(lobby, player))
        return false;
    const auto* info = player_info(lobby, player);
    if (info == nullptr)
        return false;
    return netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info));
}

bool lobby_hosts(Lobby& lobby) noexcept {
    return lobby.game != nullptr && (local_info(lobby).role & kRoleHost) != 0;
}

bool lobby_room_mutator_field(
    RoomMutatorPhase phase,
    const netgame::PresenceDigest& digest,
    const netgame::PresenceDigest& folded,
    uint8_t index,
    uint8_t total,
    uint8_t held,
    std::span<const uint8_t> codes,
    netgame::PresenceMutators* out
) {
    if (out == nullptr || phase == RoomMutatorPhase::none)
        return false;
    if (total < 1 || total > netgame::presence_mutators_max || held > total)
        return false;
    netgame::PresenceMutators field;
    field.digest = digest;
    field.total = total;
    field.held = held;
    field.index = netgame::presence_mutator_index_none;
    switch (phase) {
    case RoomMutatorPhase::receiving:
        break;
    case RoomMutatorPhase::lacking:
        field.lacking = true;
        field.index = index;
        break;
    case RoomMutatorPhase::built:
        field.built = true;
        field.layer_digest = folded;
        field.index = index;
        break;
    case RoomMutatorPhase::failed:
        field.failed = true;
        field.index = index;
        break;
    case RoomMutatorPhase::none:
        break;
    }
    for (uint8_t at = 0; at < total; ++at) {
        const uint8_t code =
            at < codes.size() ? codes[at] : static_cast<uint8_t>(MutatorHave::unknown);
        netgame::presence_mutator_set_state(field.states, at, code);
    }
    *out = std::move(field);
    return true;
}

MutatorHave lobby_mutator_have(Lobby& lobby, int32_t slot, uint8_t position) noexcept {
    if (position >= netgame::presence_mutators_max)
        return MutatorHave::unknown;
    const auto record = lobby_presence_record(lobby, slot);
    const auto state = netgame::presence_mutators_state(record.data(), record.size());
    if (!state || position >= state->total)
        return MutatorHave::unknown;
    switch (netgame::presence_mutator_state(state->states, position)) {
    case 0:
        return MutatorHave::has;
    case 1:
        return MutatorHave::missing;
    case 2:
        return MutatorHave::other_release;
    default:
        return MutatorHave::unknown;
    }
}

MutatorsBlock lobby_mutators_block(Lobby& lobby) noexcept {
    return block_for(lobby, -1);
}

bool lobby_mutators_allow_start(Lobby& lobby) noexcept {
    return lobby_mutators_block(lobby).hold == MutatorsHold::none;
}

std::string
mutator_shown_name(std::string_view installed, std::string_view catalogue, std::string_view id) {
    if (!installed.empty())
        return std::string(installed);
    if (!catalogue.empty())
        return std::string(catalogue);
    return std::string(id);
}

namespace {

/// The start START refused because mutators differ. The main thread only.
std::optional<MutatorsBlock>& refused_start() {
    static std::optional<MutatorsBlock> held;
    return held;
}

} // namespace

void lobby_note_refused_start(const MutatorsBlock& block) {
    refused_start() = block;
}

std::optional<MutatorsBlock> lobby_take_refused_start() {
    std::optional<MutatorsBlock> taken = std::move(refused_start());
    refused_start().reset();
    return taken;
}

std::vector<int32_t> lobby_classic_players(Lobby& lobby) {
    std::vector<int32_t> slots;
    if (lobby.game == nullptr)
        return slots;
    for (int32_t player_slot = 0; player_slot < kSlotCount; ++player_slot) {
        if (classic_seated(lobby, player_slot))
            slots.push_back(player_slot);
    }
    return slots;
}

void lobby_post_classic_line(Lobby& lobby, const char* words, int32_t player_slot) noexcept {
    if (lobby.game == nullptr || words == nullptr || player_slot < 0 || player_slot >= kSlotCount)
        return;
    const auto& named = slot_player(lobby, player_slot);
    const std::string_view who(named.name, ::strnlen(named.name, sizeof named.name));
    // The interface catalogue, not the lobby's Translate hook: a German,
    // French, Spanish or Italian room shows the line in that language.
    // A 3.1c name is code-page game text, so it is read as UTF-8 first.
    const std::string who_utf8 = netgame::chat_utf8(who);
    std::string line(oa::data::languages::interface_text(words));
    const auto mark = line.find("{player}");
    if (mark != std::string::npos)
        line.replace(mark, std::string_view("{player}").size(), who_utf8);
    lobby_post_room_line(lobby, std::move(line));
}

void lobby_post_room_line(Lobby& lobby, std::string line) noexcept {
    if (lobby.game == nullptr)
        return;
    // The chat list draws one stored line on one row and clips the rest, so
    // a line wider than the list goes on in further "*** " lines, as the
    // room's mutator lines do. The measure reads the line as it is stored.
    std::vector<std::string> pieces;
    const ChatMeasure& measure = lobby.chat_measure;
    const int32_t room =
        measure.room != nullptr && measure.width != nullptr ? measure.room(measure.context) : 0;
    if (room > 0)
        pieces = fit_chat_line(line, room, [&measure](std::string_view text) {
            return measure.width(measure.context, text);
        });
    else
        pieces.push_back(std::move(line));
    for (std::string& piece : pieces) {
        // With Unicode chat off the line is stored in the code page, so "à"
        // and "ò" are one glyph.
        if (!lobby.unicode_chat)
            piece = netgame::chat_code_page(piece);
        const auto parts = netgame::chat_parts(piece, kChatLineBytes - 1, netgame::chat_line_parts);
        for (const auto& part : parts)
            lobby_post_chat(lobby, part.c_str());
    }
}

void lobby_note_classic_room(Lobby& lobby) noexcept {
    if (lobby.game == nullptr)
        return;
    const auto host = lobby_host_slot(lobby);
    const auto host_record = host == kNoSlot
                                 ? std::span<const uint8_t>{}
                                 : lobby_presence_record(lobby, static_cast<int32_t>(host));
    const bool playing =
        host != kNoSlot &&
        netgame::presence_mutators_state(host_record.data(), host_record.size()).has_value();
    if (!playing) {
        lobby.classic_lines_posted = false;
        return;
    }
    if (lobby.classic_lines_posted)
        return;
    lobby.classic_lines_posted = true;
    for (int32_t player_slot = 0; player_slot < kSlotCount; ++player_slot) {
        if (!classic_seated(lobby, player_slot))
            continue;
        lobby_post_classic_line(lobby, kClassicSeated, player_slot);
    }
}

bool lobby_mutators_differ_from_host(Lobby& lobby, int32_t slot) noexcept {
    if (lobby.game == nullptr || slot < 0 || slot >= kSlotCount)
        return false;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot || host == slot)
        return false;
    const auto block = block_for(lobby, slot);
    return block.slot == slot && block.hold != MutatorsHold::none &&
           block.hold != MutatorsHold::classic_player;
}

void lobby_log_mutators_hold(Lobby& lobby) noexcept {
    if (!lobby_hosts(lobby))
        return;
    const auto block = lobby_mutators_block(lobby);
    if (lobby.mutators_hold_seen && block.hold == lobby.mutators_hold_logged.hold &&
        block.slot == lobby.mutators_hold_logged.slot &&
        block.position == lobby.mutators_hold_logged.position)
        return;
    lobby.mutators_hold_seen = true;
    lobby.mutators_hold_logged = block;
    if (block.hold == MutatorsHold::none) {
        std::cerr << "multiplayer: mutators allow START\n";
    } else {
        std::cerr << "multiplayer: mutators hold START: " << hold_name(block.hold) << " slot "
                  << block.slot << " position " << static_cast<unsigned>(block.position) << "\n";
    }
    std::cerr.flush();
}

} // namespace oa::ui::frontend_multiplayer
