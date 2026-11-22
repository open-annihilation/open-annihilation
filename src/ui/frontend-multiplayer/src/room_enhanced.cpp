// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's Enhanced Mode: the room plays the lowest of every
// machine's highest revision while the host's switch is on, Off when any
// machine has none, and START waits until every machine plays the host's rules.
#include "oa/ui/frontend_multiplayer/room_enhanced.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/platform/log_files.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"
#include "oa/ui/frontend_multiplayer/room_limits.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::frontend_multiplayer {

namespace {

constexpr const char* kRulesLine =
    "*** Enhanced Mode rules differ. Every player must use the same mod and match rules.";
constexpr std::string_view kRevisionLine = "*** Enhanced Mode: Revision {n}.";
constexpr std::string_view kOffLine = "*** Enhanced Mode: Off.";
constexpr std::string_view kLacksLine =
    "*** Enhanced Mode is off: {player}'s game doesn't have it.";
constexpr std::string_view kLowerLine =
    "*** Enhanced Mode is Revision {n}: {player}'s game has no later revision.";
constexpr std::string_view kRevisionWord = "Revision {n}";
constexpr std::string_view kOffWord = "Off";
constexpr std::string_view kLacksReason = "{player}'s game doesn't have it.";
constexpr std::string_view kLowerReason = "{player}'s game has no later revision.";
constexpr std::string_view kLaunchLock = "The launch keeps Enhanced Mode off.";
constexpr std::string_view kGuestLock = "Only the host can change Enhanced Mode.";

/// One place in a catalogue sentence and the value it takes.
struct Field {
    std::string_view name;  ///< the place's name, without braces
    std::string_view value; ///< what it becomes
};

/// Looks a sentence up in the interface catalogue and fills its places.
///
/// @param english the sentence, in English
/// @param fields each place's name and value
/// @return the sentence, in the language shown
std::string filled(std::string_view english, std::initializer_list<Field> fields = {}) {
    std::string text(oa::data::languages::interface_text(english));
    for (const auto& [name, value] : fields) {
        const std::string place = "{" + std::string(name) + "}";
        for (std::size_t at = text.find(place); at != std::string::npos;
             at = text.find(place, at + value.size()))
            text.replace(at, place.size(), value);
    }
    return text;
}

/// Tells whether the switch is in effect: on, and not held off by a launch.
///
/// @param reason why the room plays what it plays
/// @return true for plays, lacks and lower
bool switch_in_effect(EnhancedRoomReason reason) noexcept {
    return reason != EnhancedRoomReason::switch_off && reason != EnhancedRoomReason::locked;
}

/// Resolves the room as the host would with its switch on.
///
/// @param seats the seated humans and watchers, in slot order
/// @return the room; Off with `lacks` when a seat announces none
EnhancedRoom resolve_switched_on(std::span<const EnhancedSeat> seats) {
    EnhancedRoom room;
    room.reason = EnhancedRoomReason::plays;
    const EnhancedSeat* none = nullptr;
    const EnhancedSeat* lowest = nullptr;
    uint32_t highest = 0;
    for (const EnhancedSeat& seat : seats) {
        if (seat.announce == EnhancedAnnounce::not_known) {
            room.waiting = true;
            continue;
        }
        if (seat.announce == EnhancedAnnounce::none) {
            if (none == nullptr)
                none = &seat;
            continue;
        }
        if (lowest == nullptr || seat.highest < lowest->highest)
            lowest = &seat;
        if (seat.highest > highest)
            highest = seat.highest;
    }
    if (none != nullptr) {
        room.reason = EnhancedRoomReason::lacks;
        room.cause = none->slot;
        room.cause_player = none->player;
        return room;
    }
    if (lowest == nullptr) {
        // No seat is known yet: nothing to play but Off, and nothing to name.
        room.reason = EnhancedRoomReason::switch_off;
        return room;
    }
    room.revision = lowest->highest;
    if (lowest->highest < highest) {
        room.reason = EnhancedRoomReason::lower;
        room.cause = lowest->slot;
        room.cause_player = lowest->player;
    }
    return room;
}

/// The line a resolution posts when it is a change.
///
/// @param room the room as resolved now
/// @return the line, in the language shown
std::string room_line(const EnhancedRoom& room) {
    const std::string number = std::to_string(room.revision);
    if (room.revision != 0) {
        if (room.reason == EnhancedRoomReason::lower)
            return filled(kLowerLine, {{"n", number}, {"player", room.cause_player}});
        return filled(kRevisionLine, {{"n", number}});
    }
    if (room.reason == EnhancedRoomReason::lacks)
        return filled(kLacksLine, {{"player", room.cause_player}});
    return filled(kOffLine);
}

/// Adds the lines a guest's resolution posts, and keeps an Off line waiting
/// for the seat to name.
///
/// @param[in,out] room the room as resolved now; its off_line_since is set
///     while the Off line waits
/// @param before the room as last resolved; nothing on entering it
/// @param now_ms the time now, in milliseconds
/// @param[in,out] lines the lines to post, added to in order
void guest_lines(
    EnhancedRoom& room,
    const std::optional<EnhancedRoom>& before,
    uint64_t now_ms,
    std::vector<std::string>& lines
) {
    if (!before) {
        // Entering the room posts only a revision it plays.
        if (room.revision != 0)
            lines.push_back(room_line(room));
        return;
    }
    const std::optional<uint64_t> since = before->off_line_since;
    if (room.revision != before->revision) {
        // The host turned the room back before the Off line had a name: that
        // change still posts its line, before this one.
        if (since)
            lines.push_back(room_line(EnhancedRoom{}));
        if (room.revision == 0 && room.reason != EnhancedRoomReason::lacks) {
            // No seat this machine sees announces none yet: its record may
            // still be on its way.
            room.off_line_since = now_ms;
            return;
        }
        lines.push_back(room_line(room));
        return;
    }
    if (!since)
        return;
    const bool waited = now_ms < *since || now_ms - *since >= enhanced_off_line_wait_ms;
    if (room.reason == EnhancedRoomReason::lacks || waited) {
        lines.push_back(room_line(room));
        return;
    }
    room.off_line_since = since;
}

/// Tells whether a machine's seat counts: a seated human or a watcher.
///
/// Local and remote computer seats are covered by the human who runs them.
/// An unreceived setup block cannot leave a seated remote machine out.
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
/// @return The record, or an empty span while the machine is unknown or plays 3.1c.
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

/// Tells whether a remote seat's own setup block has arrived.
///
/// @param lobby Lobby state.
/// @param slot Player slot.
/// @return True once the slot's block is its player's.
bool block_arrived(Lobby& lobby, int32_t slot) noexcept {
    const auto& player = slot_player(lobby, slot);
    return player_info(lobby, player) != nullptr &&
           lobby.presence_records.block_player[slot] == player.player_id;
}

/// Tells whether a remote seat plays 3.1c: its own block lacks the engine signature.
///
/// @param lobby Lobby state.
/// @param slot Player slot.
/// @return True for a 3.1c machine whose block has arrived.
bool classic_block(Lobby& lobby, int32_t slot) noexcept {
    const auto& player = slot_player(lobby, slot);
    const auto* info = player_info(lobby, player);
    return block_arrived(lobby, slot) &&
           !netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info));
}

} // namespace

EnhancedRoomChange
resolve_enhanced_room(const EnhancedRoomInputs& inputs, const std::optional<EnhancedRoom>& before) {
    EnhancedRoomChange change;
    EnhancedRoom& room = change.room;
    if (inputs.hosting) {
        if (inputs.locked) {
            room.reason = EnhancedRoomReason::locked;
        } else if (!inputs.switch_on) {
            room.reason = EnhancedRoomReason::switch_off;
        } else {
            room = resolve_switched_on(inputs.seats);
        }
        for (const EnhancedSeat& seat : inputs.seats)
            if (seat.announce == EnhancedAnnounce::not_known)
                room.waiting = true;
    } else {
        const EnhancedRoom seen = resolve_switched_on(inputs.seats);
        if (!inputs.announced) {
            // The host has not said yet: nothing changes and nothing posts.
            room = before.value_or(EnhancedRoom{});
            room.waiting = true;
            return change;
        }
        room.revision = *inputs.announced;
        room.waiting = seen.waiting;
        if (room.revision == 0) {
            room.reason = EnhancedRoomReason::switch_off;
            if (seen.reason == EnhancedRoomReason::lacks) {
                room.reason = EnhancedRoomReason::lacks;
                room.cause = seen.cause;
                room.cause_player = seen.cause_player;
            }
        } else {
            room.reason = EnhancedRoomReason::plays;
            if (seen.reason == EnhancedRoomReason::lower && seen.revision == room.revision) {
                room.reason = EnhancedRoomReason::lower;
                room.cause = seen.cause;
                room.cause_player = seen.cause_player;
            }
        }
        guest_lines(room, before, inputs.now_ms, change.lines);
        return change;
    }

    bool changed = false;
    if (!before) {
        changed = room.revision != 0;
    } else {
        changed = room.revision != before->revision ||
                  switch_in_effect(room.reason) != switch_in_effect(before->reason);
    }
    if (changed)
        change.lines.push_back(room_line(room));
    return change;
}

bool enhanced_room_switch_start(bool settings_on, bool stock_files, bool locked) noexcept {
    return settings_on && !stock_files && !locked;
}

std::string enhanced_revision_word(uint32_t revision) {
    if (revision == 0)
        return filled(kOffWord);
    return filled(kRevisionWord, {{"n", std::to_string(revision)}});
}

std::string enhanced_room_reason_text(const EnhancedRoom& room) {
    if (room.reason == EnhancedRoomReason::lacks)
        return filled(kLacksReason, {{"player", room.cause_player}});
    if (room.reason == EnhancedRoomReason::lower)
        return filled(kLowerReason, {{"player", room.cause_player}});
    return {};
}

std::string enhanced_room_launch_lock_text() {
    return filled(kLaunchLock);
}

std::string enhanced_room_guest_lock_text() {
    return filled(kGuestLock);
}

std::string enhanced_waiting_line(std::string_view player, uint32_t revision) {
    return start_reason(PlayerLack{std::string(player), enhanced_revision_word(revision)});
}

std::vector<EnhancedSeat> lobby_enhanced_seats(Lobby& lobby, std::optional<uint32_t> local) {
    std::vector<EnhancedSeat> seats;
    if (lobby.game == nullptr)
        return seats;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        if (!machine_seat(lobby, slot))
            continue;
        const auto& player = slot_player(lobby, slot);
        EnhancedSeat seat;
        seat.slot = slot;
        seat.player = netgame::chat_utf8(
            std::string_view(player.name, ::strnlen(player.name, sizeof player.name))
        );
        if (player.status == kSlotLocal) {
            seat.announce = local ? EnhancedAnnounce::revision : EnhancedAnnounce::none;
            seat.highest = local.value_or(0);
        } else if (!block_arrived(lobby, slot)) {
            seat.announce = EnhancedAnnounce::not_known;
        } else if (classic_block(lobby, slot)) {
            seat.announce = EnhancedAnnounce::none;
        } else {
            const auto record = machine_record(lobby, slot);
            if (record.empty() || !netgame::presence_well_framed(record.data(), record.size())) {
                seat.announce = EnhancedAnnounce::not_known;
            } else if (
                const auto highest =
                    netgame::presence_enhanced_highest_revision(record.data(), record.size())
            ) {
                seat.announce = EnhancedAnnounce::revision;
                seat.highest = *highest;
            } else {
                seat.announce = EnhancedAnnounce::none;
            }
        }
        seats.push_back(std::move(seat));
    }
    return seats;
}

std::optional<uint32_t> lobby_enhanced_announced(Lobby& lobby) noexcept {
    if (lobby.game == nullptr)
        return std::nullopt;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot)
        return std::nullopt;
    const auto& player = slot_player(lobby, host);
    if (player.status == kSlotRemote) {
        if (!block_arrived(lobby, host))
            return std::nullopt;
        if (classic_block(lobby, host))
            return 0u;
    }
    const auto record = machine_record(lobby, host);
    if (record.empty() || !netgame::presence_well_framed(record.data(), record.size()))
        return std::nullopt;
    return netgame::presence_enhanced_room_revision(record.data(), record.size()).value_or(0);
}

uint32_t lobby_enhanced_room_revision(Lobby& lobby) noexcept {
    if (lobby.game == nullptr)
        return 0;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot)
        return 0;
    const auto record = machine_record(lobby, host);
    return netgame::presence_enhanced_room_revision(record.data(), record.size()).value_or(0);
}

EnhancedBlock lobby_enhanced_block(Lobby& lobby) noexcept {
    EnhancedBlock block;
    if (lobby.game == nullptr)
        return block;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot)
        return block;
    const auto host_record = machine_record(lobby, host);
    block.revision =
        netgame::presence_enhanced_room_revision(host_record.data(), host_record.size())
            .value_or(0);
    if (block.revision == 0)
        return block;
    const auto host_hash = netgame::presence_sim_hash(host_record.data(), host_record.size());
    if (!host_hash) {
        block.hold = EnhancedHold::rules_differ;
        block.slot = host;
        return block;
    }
    const auto local = lobby.game->local_player_index;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        // The host waits for every machine. A guest checks its own agreement
        // with the host; another guest's record need not have arrived here.
        if (slot == host || (host != local && slot != local))
            continue;
        if (!machine_seat(lobby, slot))
            continue;
        const auto record = machine_record(lobby, slot);
        if (record.empty() || !netgame::presence_well_framed(record.data(), record.size())) {
            block.hold =
                classic_block(lobby, slot) ? EnhancedHold::rules_differ : EnhancedHold::waiting;
            block.slot = slot;
            return block;
        }
        const auto hash = netgame::presence_sim_hash(record.data(), record.size());
        if (!hash || *hash != *host_hash) {
            block.hold = EnhancedHold::rules_differ;
            block.slot = slot;
            return block;
        }
    }
    return block;
}

const char* lobby_enhanced_start_problem(Lobby& lobby) noexcept {
    return lobby_enhanced_block(lobby).hold == EnhancedHold::none ? nullptr : kRulesLine;
}

bool lobby_enhanced_allow_start(Lobby& lobby) noexcept {
    return lobby_enhanced_block(lobby).hold == EnhancedHold::none;
}

void lobby_note_enhanced_refused_start(Lobby& lobby) noexcept {
    if (lobby.game == nullptr || lobby_enhanced_block(lobby).hold != EnhancedHold::rules_differ)
        return;
    lobby_post_limits_line(lobby, kRulesLine, lobby.game->local_player_index);
}

void lobby_post_enhanced_line(Lobby& lobby, const std::string& line) noexcept {
    if (lobby.game == nullptr)
        return;
    try {
        oa::platform::log_files::write_line(line);
        lobby_post_room_line(lobby, line);
    } catch (...) {
        // A line that cannot be written leaves the room as it is.
    }
}

} // namespace oa::ui::frontend_multiplayer
