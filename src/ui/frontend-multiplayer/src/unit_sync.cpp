// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unit-content sync table behind the battleroom and the restriction panel.
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/player_slots.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/platform/log_files.hpp"
#include "oa/ui/frontend_multiplayer/room_limits.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::frontend_multiplayer {

namespace {

constexpr uint8_t kSubtypeGreeting = 0;
constexpr uint8_t kSubtypeDefCount = 1;
constexpr uint8_t kSubtypeChecksum = 2;
constexpr uint8_t kSubtypeVerdict = 3;
constexpr uint8_t kSubtypeReceived = 4;
constexpr uint32_t kDisabledByDefaultLimit = 0;
constexpr int32_t kUnlimited = -1;
constexpr int32_t kChecksumsPerTick = 4;
constexpr uint32_t kUnknownChecksum = 0;

const UnitSyncRecord* find(const UnitSync& sync, uint32_t key) noexcept {
    for (int32_t index = 0; index < sync.record_count; ++index)
        if (sync.records[index].key == key)
            return &sync.records[index];
    return nullptr;
}

UnitSyncRecord* find(UnitSync& sync, uint32_t key) noexcept {
    return const_cast<UnitSyncRecord*>(find(std::as_const(sync), key));
}

// The side flags are read as 16-bit words: local with local_high, remote with remote_high.
bool both_sides(const UnitSyncRecord& record) noexcept {
    const auto local = static_cast<uint16_t>(record.local | record.local_high << 8);
    const auto remote = static_cast<uint16_t>(record.remote | record.remote_high << 8);
    return local != 0 && remote != 0;
}

UnitSyncRecord* upsert(UnitSync& sync, uint32_t key) noexcept {
    if (auto* existing = find(sync, key))
        return existing;
    if (sync.record_count >= static_cast<int32_t>(kMaxSyncUnits))
        return nullptr;
    auto& record = sync.records[sync.record_count++];
    record = UnitSyncRecord{};
    record.key = key;
    return &record;
}

// A peer the host still waits on: seated, not defeated and not played
// by the computer. Peers without a slot are not waited on.
bool peer_waited_on(Lobby& lobby, uint32_t player_id) noexcept {
    auto* player = netgame::player_of_id(*lobby.game, player_id);
    if (player == nullptr || slot_remote_defeated(lobby, *player))
        return false;
    return player->in_use == 0 || player->status != kSlotComputer;
}

// Peers from version 1.2 on (PlayerSetupInfo.version_major and
// version_minor) have their checksums compared.
bool reports_checksums(const PlayerSetupInfo& info) noexcept {
    return info.version_major >= 2 || (info.version_major == 1 && info.version_minor >= 2);
}

// Index of a key among the peer's reported ones, or received when absent.
uint32_t reported_index(const UnitSyncPeer& peer, uint32_t key) noexcept {
    uint32_t index = 0;
    while (index < peer.received && peer.keys[index] != key)
        ++index;
    return index;
}

// Reported its unit count, sent every checksum and acknowledged every verdict. A machine
// acknowledges every 0x1a record it handled, those sent to its computer players included: a
// computer player the host greeted as a playing peer before its info told otherwise leaves
// the machine's count above what the host sent its human, and the count still covers them.
bool peer_complete(const UnitSyncPeer& peer) noexcept {
    return peer.expected != 0 && peer.received == peer.expected && peer.acknowledged >= peer.sent;
}

/// Returns the player id of the first host-flagged slot, an open slot included.
///
/// @param lobby Lobby state.
/// @return The id, or no_player_id when no slot is flagged host or the flagged slot is free.
uint32_t host_player_id(Lobby& lobby) noexcept {
    for (uint8_t slot = 0; slot < kSlotCount; ++slot) {
        const auto* info = slot_info(lobby, slot);
        if (info != nullptr && (info->role & kRoleHost) != 0)
            return netgame::player_slot_id(*lobby.game, slot);
    }
    return netgame::no_player_id;
}

// One 14-byte 0x1a record {subtype, key, value} to a player.
void send_handshake(
    Lobby& lobby, uint32_t to, uint8_t subtype, uint32_t key, uint32_t value
) noexcept {
    netgame::UnitDefHandshakeRecord record{};
    record.subtype = subtype;
    record.key = key;
    record.value = value;
    uint8_t wire[14];
    std::size_t written = 0;
    if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
        unit_sync_send(lobby, to, wire);
}

} // namespace

void unit_sync_create(Lobby& lobby, bool host) noexcept {
    auto& sync = lobby.sync;
    sync = UnitSync{};
    sync.host = host;
    for (int32_t type = 1; type < lobby.unit_count; ++type) {
        const auto& unit = lobby.units[type];
        auto* record = upsert(sync, unit.fbi_hash);
        if (record == nullptr)
            return;
        record->checksum = 0;
        record->local = 1;
        record->remote = host ? 1 : 0;
        record->limit = (unit.abilities & kUnitDisabledDefault) != 0
                            ? static_cast<int32_t>(kDisabledByDefaultLimit)
                            : kUnlimited;
    }
}

namespace {

/// Sends the greeting unit_sync_tick would, and keeps the peer in the table.
///
/// @param lobby Lobby state.
/// @param player_id the peer
void greet_peer_now(Lobby& lobby, uint32_t player_id) noexcept {
    if (lobby.sync.peer_count >= kSlotCount)
        return;
    auto& peer = lobby.sync.peers[lobby.sync.peer_count++];
    peer = UnitSyncPeer{};
    peer.player_id = player_id;
    send_handshake(lobby, peer.player_id, kSubtypeGreeting, 0, 0);
    ++peer.sent;
}

/// Compares every stored checksum with the host's units after a reload.
///
/// @param lobby Lobby state.
void reevaluate_units(Lobby& lobby) noexcept {
    for (int32_t index = 0; index < lobby.sync.record_count; ++index) {
        const uint32_t key = lobby.sync.records[index].key;
        uint32_t checksum = kUnknownChecksum;
        for (int32_t type = 1; type < lobby.unit_count; ++type) {
            if (lobby.units[type].fbi_hash != key)
                continue;
            checksum = unit_sync_content_checksum(lobby, lobby.units[type]);
            break;
        }
        unit_sync_update_record(lobby, key, checksum);
    }
}

// The verdict as it is sent: local, remote, then the limit.
uint32_t verdict_value(const UnitSyncRecord& record) noexcept {
    return static_cast<uint32_t>(record.local) | static_cast<uint32_t>(record.remote) << 8U |
           static_cast<uint32_t>(static_cast<uint16_t>(record.limit)) << 16U;
}

// True when this pass already stored this key with the same verdict.
bool verdict_repeats_this_pass(const UnitSync& sync, uint32_t key, uint32_t value) noexcept {
    const auto* stored = find(sync, key);
    return stored != nullptr && stored->from_host != 0 && verdict_value(*stored) == value;
}

// The host's setup block carries the Open Annihilation signature. A missing block does not.
bool host_open_annihilation(Lobby& lobby) noexcept {
    const auto host_id = host_player_id(lobby);
    if (host_id == netgame::no_player_id)
        return false;
    const auto* player = netgame::player_of_id(*lobby.game, host_id);
    if (player == nullptr)
        return false;
    const auto* info = player_info(lobby, *player);
    return info != nullptr &&
           netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info));
}

// TADR's replayer names its session with this prefix, then the map. An Open
// Annihilation host whose player chose such a name is not the replayer.
bool recorder_session(Lobby& lobby) noexcept {
    if (lobby.game == nullptr)
        return false;
    constexpr char kPrefix[] = "TADR ";
    return std::strncmp(lobby.joined_session, kPrefix, sizeof(kPrefix) - 1) == 0 &&
           !host_open_annihilation(lobby);
}

// Distinct keys the recorder gave with its first-pass status.
uint32_t first_pass_units(const UnitSync& sync) noexcept {
    uint32_t count = 0;
    for (int32_t index = 0; index < sync.record_count; ++index)
        if (sync.records[index].first_given != 0)
            ++count;
    return count;
}

// The count TADR's replayer waits for when a repeated verdict starts its
// check again. Zero when this record does not. The first pass, status
// 0x0001, waits for two. The second pass, status 0x0101, waits for the
// first pass's units plus two.
uint32_t count_again_at(
    Lobby& lobby, const UnitSync& sync, uint8_t subtype, uint32_t key, uint32_t value
) noexcept {
    if (!recorder_session(lobby) || subtype != kSubtypeVerdict)
        return 0;
    if (!verdict_repeats_this_pass(sync, key, value))
        return 0;
    const auto pass = static_cast<uint16_t>(value);
    if (pass == 0x0001)
        return 2;
    if (pass == 0x0101)
        return first_pass_units(sync) + 2U;
    return 0;
}

void clear_host_verdicts(UnitSync& sync) noexcept {
    for (int32_t at = 0; at < sync.record_count; ++at)
        sync.records[at].from_host = 0;
}

} // namespace

uint32_t unit_sync_handled_count(uint32_t handled, uint8_t subtype, uint32_t again_at) noexcept {
    if (subtype >= netgame::handshake_subtype_limit)
        return handled;
    if (again_at != 0 && subtype != static_cast<uint8_t>(netgame::HandshakeSubtype::announce))
        return again_at;
    return handled + 1U;
}

void unit_sync_rebuild(Lobby& lobby) noexcept {
    // The local player's role decides. sync.host is unset until the table
    // exists, and a host must not be rebuilt as a guest.
    const bool host = lobby.game != nullptr && (local_info(lobby).role & kRoleHost) != 0;
    UnitSyncPeer kept[kSlotCount]{};
    uint32_t dropped[kSlotCount]{};
    int32_t kept_count = 0;
    int32_t dropped_count = 0;
    if (host) {
        for (int32_t index = 0; index < lobby.sync.peer_count; ++index) {
            const auto& peer = lobby.sync.peers[index];
            const auto* player = netgame::player_of_id(*lobby.game, peer.player_id);
            if (player == nullptr)
                continue;
            const auto* info = player_info(lobby, *player);
            // No setup block, or an Open Annihilation peer: dropped and
            // greeted again now. A 3.1c peer keeps what it reported, so
            // turning mutators on and off does not make it report again.
            if (info == nullptr ||
                netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info))) {
                if (dropped_count < kSlotCount)
                    dropped[dropped_count++] = peer.player_id;
                continue;
            }
            kept[kept_count] = peer;
            ++kept_count;
        }
    }
    unit_sync_create(lobby, host);
    for (int32_t index = 0; index < kept_count; ++index)
        lobby.sync.peers[index] = kept[index];
    lobby.sync.peer_count = kept_count;
    if (!host)
        return;
    for (int32_t index = 0; index < dropped_count; ++index)
        greet_peer_now(lobby, dropped[index]);
    reevaluate_units(lobby);
}

void unit_sync_regreet(Lobby& lobby, uint32_t player_id) noexcept {
    if (lobby.game == nullptr || (local_info(lobby).role & kRoleHost) == 0)
        return;
    auto& sync = lobby.sync;
    for (int32_t index = 0; index < sync.peer_count; ++index) {
        if (sync.peers[index].player_id != player_id)
            continue;
        for (int32_t later = index + 1; later < sync.peer_count; ++later)
            sync.peers[later - 1] = sync.peers[later];
        --sync.peer_count;
        greet_peer_now(lobby, player_id);
        reevaluate_units(lobby);
        return;
    }
}

void unit_sync_destroy(Lobby& lobby) noexcept {
    lobby.sync = UnitSync{};
}

void unit_sync_send(Lobby& lobby, uint32_t player_id, const uint8_t* record) noexcept {
    const auto to = lobby.sync.host ? player_id : host_player_id(lobby);
    if (to == netgame::no_player_id)
        return;
    uint8_t wire[14];
    std::memcpy(wire, record, sizeof(wire));
    std::memset(wire + 2, 0, 4);
    if (lobby.net.send != nullptr)
        lobby.net.send(lobby.net.context, to, wire, sizeof(wire));
}

void unit_sync_send_verdict(
    Lobby& lobby, UnitSyncPeer& peer, const UnitSyncRecord& record
) noexcept {
    if (lobby.sync.finished)
        return;
    netgame::UnitDefHandshakeRecord verdict{};
    verdict.subtype = kSubtypeVerdict;
    verdict.key = record.key;
    verdict.value = static_cast<uint32_t>(record.local) |
                    static_cast<uint32_t>(record.remote) << 8 |
                    static_cast<uint32_t>(static_cast<uint16_t>(record.limit)) << 16;
    uint8_t wire[14];
    std::size_t written = 0;
    if (netgame::encode_record(verdict, wire, sizeof(wire), &written) != netgame::WireError::ok)
        return;
    unit_sync_send(lobby, peer.player_id, wire);
    ++peer.sent;
}

void unit_sync_relay(Lobby& lobby, uint32_t key) noexcept {
    auto& sync = lobby.sync;
    if (sync.finished)
        return;
    if (sync.host) {
        if (const auto* record = find(sync, key))
            for (int32_t index = 0; index < sync.peer_count; ++index)
                unit_sync_send_verdict(lobby, sync.peers[index], *record);
    }
    for (int32_t index = 0; index < sync.pending_count; ++index)
        if (sync.pending[index] == key)
            return;
    if (sync.pending_count < static_cast<int32_t>(kMaxSyncUnits))
        sync.pending[sync.pending_count++] = key;
}

void unit_sync_receive(Lobby& lobby, std::span<const uint8_t> bytes, uint8_t from_slot) noexcept {
    auto& sync = lobby.sync;
    netgame::UnitDefHandshakeRecord record{};
    if (netgame::decode_record(bytes.data(), bytes.size(), &record) != netgame::WireError::ok)
        return;
    const uint8_t subtype = record.subtype;
    if (subtype >= netgame::handshake_subtype_limit || sync.finished)
        return;
    const auto key = record.key;
    const uint32_t again_at =
        sync.host ? 0U : count_again_at(lobby, sync, subtype, key, record.value);
    if (again_at != 0)
        clear_host_verdicts(sync);
    sync.records_handled = unit_sync_handled_count(sync.records_handled, subtype, again_at);
    if (!sync.host) {
        // A greeting starts this machine's report again. The next ticks
        // send the count and then the checksums from the first unit. The
        // handled count keeps going: a greeting is not a pass that starts
        // again. TADR's replayer starts the check again, and the viewer
        // then counts that pass as TADR does. Every other host, the
        // original game's included, is counted as the original game counts.
        if (subtype == kSubtypeGreeting)
            sync.next_unit = 0;
        if (subtype != kSubtypeVerdict)
            return;
        auto* entry = upsert(sync, key);
        if (entry == nullptr)
            return;
        // A verdict's value holds the local and remote bytes, then the limit.
        const bool opened =
            static_cast<uint16_t>(record.value) == 0x0001 && entry->first_given == 0;
        entry->checksum = 0;
        entry->local = static_cast<uint8_t>(record.value);
        entry->local_high = 0;
        entry->remote = static_cast<uint8_t>(record.value >> 8U);
        entry->remote_high = 0;
        entry->limit = static_cast<int16_t>(record.value >> 16U);
        entry->from_host = 1;
        if (opened)
            entry->first_given = 1;
        unit_sync_relay(lobby, key);
        return;
    }
    const auto sender = slot_player(lobby, from_slot).player_id;
    UnitSyncPeer* peer = nullptr;
    for (int32_t index = 0; index < sync.peer_count; ++index)
        if (sync.peers[index].player_id == sender)
            peer = &sync.peers[index];
    if (peer == nullptr)
        return;
    const auto value = record.value;
    if (subtype == kSubtypeDefCount) {
        peer->expected = value;
    } else if (subtype == kSubtypeChecksum) {
        if (reported_index(*peer, key) != peer->received || peer->received >= kMaxSyncUnits)
            return;
        peer->keys[peer->received] = key;
        peer->checksums[peer->received] = value;
        ++peer->received;
        unit_sync_update_record(lobby, key, value);
    } else if (subtype == kSubtypeReceived && peer->acknowledged < value) {
        peer->acknowledged = value;
    }
}

void unit_sync_update_record(Lobby& lobby, uint32_t key, uint32_t checksum) noexcept {
    auto& sync = lobby.sync;
    auto* record = sync.finished ? nullptr : find(sync, key);
    if (record == nullptr)
        return;
    uint8_t everyone = 1;
    if (checksum != kUnknownChecksum) {
        if (record->checksum == kUnknownChecksum && lobby.unit_count > 1)
            for (int32_t type = 1; type < lobby.unit_count; ++type)
                if (lobby.units[type].fbi_hash == key) {
                    record->checksum = unit_sync_content_checksum(lobby, lobby.units[type]);
                    break;
                }
        if (checksum != record->checksum)
            everyone = 0;
    }
    for (int32_t index = 0; everyone != 0 && index < sync.peer_count; ++index) {
        const auto& peer = sync.peers[index];
        bool compare = false;
        if (checksum != kUnknownChecksum) {
            const auto* player = netgame::player_of_id(*lobby.game, peer.player_id);
            if (player == nullptr)
                break;
            const auto* info = player_info(lobby, *player);
            compare = info != nullptr && reports_checksums(*info);
        }
        const auto at = reported_index(peer, key);
        if (at == peer.received || (compare && peer.checksums[at] != checksum)) {
            everyone = 0;
            break;
        }
    }
    record->remote = everyone;
    record->remote_high = 0;
    unit_sync_relay(lobby, key);
}

void unit_sync_tick(Lobby& lobby) noexcept {
    auto& sync = lobby.sync;
    if (sync.finished)
        return;
    if (!sync.host) {
        if (sync.records_handled == 0)
            return;
        if (sync.next_unit >= lobby.unit_count) {
            send_handshake(lobby, host_player_id(lobby), kSubtypeReceived, 0, sync.records_handled);
            return;
        }
        if (sync.next_unit == 0) {
            if (host_player_id(lobby) != netgame::no_player_id) {
                send_handshake(
                    lobby,
                    host_player_id(lobby),
                    kSubtypeDefCount,
                    0,
                    static_cast<uint32_t>(lobby.unit_count - 1)
                );
                sync.next_unit = 1;
            }
            return;
        }
        for (int32_t sent = 0; sent < kChecksumsPerTick && sync.next_unit < lobby.unit_count;
             ++sent) {
            auto& unit = lobby.units[sync.next_unit];
            const auto checksum = unit_sync_content_checksum(lobby, unit);
            send_handshake(lobby, host_player_id(lobby), kSubtypeChecksum, unit.fbi_hash, checksum);
            ++sync.next_unit;
        }
        return;
    }
    bool roster_changed = false;
    for (int32_t index = 0; index < sync.peer_count;) {
        const auto* player = netgame::player_of_id(*lobby.game, sync.peers[index].player_id);
        if (player != nullptr && slot_remote_playing(lobby, *player)) {
            ++index;
            continue;
        }
        for (int32_t later = index + 1; later < sync.peer_count; ++later)
            sync.peers[later - 1] = sync.peers[later];
        --sync.peer_count;
        roster_changed = true;
    }
    for (int32_t slot = 0; slot < kSlotCount && sync.peer_count < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (!slot_remote_playing(lobby, player))
            continue;
        bool known = false;
        for (int32_t index = 0; index < sync.peer_count && !known; ++index)
            known = sync.peers[index].player_id == player.player_id;
        if (known)
            continue;
        auto& peer = sync.peers[sync.peer_count++];
        peer = UnitSyncPeer{};
        peer.player_id = player.player_id;
        send_handshake(lobby, peer.player_id, kSubtypeGreeting, 0, 0);
        ++peer.sent;
        roster_changed = true;
    }
    if (roster_changed)
        for (int32_t index = 0; index < sync.record_count; ++index)
            unit_sync_update_record(lobby, sync.records[index].key, kUnknownChecksum);
}

uint32_t unit_sync_content_checksum(Lobby& lobby, LobbyUnit& unit) noexcept {
    if (unit.content_checksum == kUnknownChecksum && lobby.services.unit_checksum != nullptr)
        unit.content_checksum = lobby.services.unit_checksum(lobby.services.context, unit);
    return unit.content_checksum;
}

bool unit_sync_pop_changed(Lobby& lobby, UnitSyncRecord* out) noexcept {
    auto& sync = lobby.sync;
    if (sync.pending_count == 0)
        return false;
    const auto key = sync.pending[0];
    std::memmove(
        sync.pending,
        sync.pending + 1,
        sizeof(sync.pending[0]) * static_cast<std::size_t>(sync.pending_count - 1)
    );
    --sync.pending_count;
    if (const auto* record = find(sync, key))
        *out = *record;
    else
        *out = UnitSyncRecord{key, 0, 0, 0, 0, 0, 0};
    return true;
}

bool unit_sync_lookup(Lobby& lobby, uint32_t key, UnitSyncRecord* out) noexcept {
    const auto* record = find(lobby.sync, key);
    *out = record != nullptr ? *record : UnitSyncRecord{key, 0, 0, 0, 0, 0, 0};
    return out->local != 0 && out->remote != 0;
}

bool unit_sync_set_enabled(Lobby& lobby, uint32_t key, bool enabled) noexcept {
    auto* record = find(lobby.sync, key);
    if (record == nullptr)
        return false;
    record->local = enabled ? 1 : 0;
    unit_sync_relay(lobby, key);
    return record->local != 0 && record->remote != 0;
}

void unit_sync_set_limit(Lobby& lobby, uint32_t key, int32_t limit) noexcept {
    auto* record = find(lobby.sync, key);
    if (record == nullptr)
        return;
    record->limit = limit;
    unit_sync_relay(lobby, key);
}

void unit_sync_mark_units(const UnitSync& sync, UnitDef* records, uint32_t count) noexcept {
    if (sync.finished)
        return;
    for (uint32_t type = 1; type < count; ++type) {
        auto& unit = records[type];
        const auto* record = find(sync, unit.fbi_hash);
        unit.flags &= ~OA_UNIT_DEF_FLAG_AVAILABLE;
        if (record == nullptr) {
            unit.player_limit = 0;
            continue;
        }
        if (both_sides(*record))
            unit.flags |= OA_UNIT_DEF_FLAG_AVAILABLE;
        unit.player_limit = record->limit;
    }
}

bool unit_sync_complete(Lobby& lobby) noexcept {
    auto& sync = lobby.sync;
    if (sync.finished || !sync.host)
        return true;
    for (int32_t index = 0; index < sync.peer_count; ++index) {
        const auto& peer = sync.peers[index];
        if (!peer_waited_on(lobby, peer.player_id))
            continue;
        if (!peer_complete(peer))
            return false;
    }
    return true;
}

bool unit_sync_peer_complete(Lobby& lobby, uint32_t player_id) noexcept {
    auto& sync = lobby.sync;
    if (!sync.host)
        return false;
    if (sync.finished || !peer_waited_on(lobby, player_id))
        return true;
    for (int32_t index = 0; index < sync.peer_count; ++index)
        if (sync.peers[index].player_id == player_id)
            return peer_complete(sync.peers[index]);
    return false;
}

const char* unit_sync_diagnostic(Lobby& lobby, char* out, std::size_t capacity) noexcept {
    auto& sync = lobby.sync;
    if (!sync.host)
        return nullptr;
    for (int32_t index = 0; index < sync.peer_count; ++index) {
        const auto& peer = sync.peers[index];
        if (peer.expected == 0)
            return "No units_expected sent from player";
        if (peer.received != peer.expected) {
            std::snprintf(
                out,
                capacity,
                "expected %d units, got %d",
                static_cast<int>(peer.expected),
                static_cast<int>(peer.received)
            );
            return out;
        }
        if (peer.acknowledged < peer.sent) {
            std::snprintf(
                out,
                capacity,
                "packets sent=%d  ackd=%d",
                static_cast<int>(peer.sent),
                static_cast<int>(peer.acknowledged)
            );
            return out;
        }
    }
    if (const uint16_t differ = lobby.unit_report.found.total; differ > 0) {
        std::snprintf(
            out,
            capacity,
            differ == 1 ? "%d unit differs" : "%d units differ",
            static_cast<int>(differ)
        );
        return out;
    }
    return "OK";
}

namespace {

/// The most units one line names; the rest of its group is counted.
constexpr std::size_t kNamedUnits = 3;
/// The tag of a unit a machine does not have, in a comparison.
constexpr uint64_t kAbsent = 0x1'0000'0000ULL;
/// The tag of a unit a machine has that does not report checksums.
constexpr uint64_t kPresent = 0x2'0000'0000ULL;

/// One machine of the comparison: the host or a peer still waited on.
struct Machine {
    uint32_t id{};              ///< its human's player id
    bool known{};               ///< its presence record says it plays known files
    const UnitSyncPeer* peer{}; ///< null for the host
    bool checksums{};           ///< its copies are compared by checksum
};

/// One unit compared across the machines.
struct Compared {
    uint32_t key{};
    std::string name{};            ///< this machine's FBI UnitName; empty when it has no such unit
    std::vector<uint32_t> named{}; ///< the machines it differs on, sorted
};

/// The slot of a player id, or kNoSlot.
int32_t slot_of(Lobby& lobby, uint32_t id) noexcept {
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        const auto& player = slot_player(lobby, slot);
        if (player.in_use != 0 && player.player_id == id)
            return slot;
    }
    return kNoSlot;
}

/// FNV-1a over the words the finding is made from.
void mix(uint64_t& hash, uint64_t word) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (word >> shift) & 0xffU;
        hash *= 0x100000001b3ULL;
    }
}

/// The state of the host's sync: its records' verdicts and each peer's report.
uint64_t sync_state(const UnitSync& sync) noexcept {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (int32_t index = 0; index < sync.record_count; ++index) {
        const auto& record = sync.records[index];
        mix(hash, record.key);
        mix(hash,
            static_cast<uint64_t>(record.local) | static_cast<uint64_t>(record.remote) << 8U |
                static_cast<uint64_t>(static_cast<uint16_t>(record.limit)) << 16U);
    }
    for (int32_t index = 0; index < sync.peer_count; ++index) {
        const auto& peer = sync.peers[index];
        mix(hash, peer.player_id);
        mix(hash, static_cast<uint64_t>(peer.received) << 32U | peer.expected);
        mix(hash, peer.acknowledged);
    }
    mix(hash, static_cast<uint64_t>(sync.peer_count));
    return hash;
}

/// The machines that do not hold the room's copy of one unit.
///
/// @param machines every machine
/// @param tags each machine's copy, in the same order
/// @return the machines it differs on, sorted; empty when every copy is one
std::vector<uint32_t>
differing_machines(const std::vector<Machine>& machines, const std::vector<uint64_t>& tags) {
    // A machine that only says it has the unit has any copy that is there.
    std::vector<uint64_t> copies;
    for (const uint64_t tag : tags)
        if (tag != kAbsent && tag != kPresent &&
            std::find(copies.begin(), copies.end(), tag) == copies.end())
            copies.push_back(tag);
    std::vector<uint64_t> resolved = tags;
    for (uint64_t& tag : resolved)
        if (tag == kPresent)
            tag = copies.empty() ? kPresent : copies.front();
    std::vector<uint64_t> groups;
    for (const uint64_t tag : resolved)
        if (std::find(groups.begin(), groups.end(), tag) == groups.end())
            groups.push_back(tag);
    if (groups.size() < 2)
        return {};
    // The room's copy: the most machines that play known files, then the most machines.
    int64_t best_score = -1;
    int best_count = 0;
    for (const uint64_t group : groups) {
        int64_t known = 0;
        int64_t count = 0;
        for (std::size_t at = 0; at < machines.size(); ++at)
            if (resolved[at] == group) {
                ++count;
                known += machines[at].known ? 1 : 0;
            }
        const int64_t score = known * 64 + count;
        if (score > best_score) {
            best_score = score;
            best_count = 1;
        } else if (score == best_score) {
            ++best_count;
        }
    }
    std::vector<uint32_t> named;
    for (std::size_t at = 0; at < machines.size(); ++at) {
        int64_t known = 0;
        int64_t count = 0;
        for (std::size_t other = 0; other < machines.size(); ++other)
            if (resolved[other] == resolved[at]) {
                ++count;
                known += machines[other].known ? 1 : 0;
            }
        // With no group in the lead every machine is named.
        if (best_count > 1 || known * 64 + count != best_score)
            named.push_back(machines[at].id);
    }
    std::sort(named.begin(), named.end());
    return named;
}

/// This machine's unit of a key, or null.
const LobbyUnit* unit_of_key(Lobby& lobby, uint32_t key) noexcept {
    for (int32_t type = 1; type < lobby.unit_count; ++type)
        if (lobby.units[type].fbi_hash == key)
            return &lobby.units[type];
    return nullptr;
}

/// This machine's unit of an FBI UnitName, matched without case, or null.
const LobbyUnit* unit_of_name(Lobby& lobby, std::string_view name) noexcept {
    for (int32_t type = 1; type < lobby.unit_count; ++type) {
        const char* unit_name = lobby.units[type].unit_name;
        if (unit_name == nullptr || std::strlen(unit_name) != name.size())
            continue;
        bool same = true;
        for (std::size_t at = 0; at < name.size() && same; ++at)
            same = std::tolower(static_cast<unsigned char>(unit_name[at])) ==
                   std::tolower(static_cast<unsigned char>(name[at]));
        if (same)
            return &lobby.units[type];
    }
    return nullptr;
}

/// Fills one token of a sentence.
std::string filled(std::string text, std::string_view token, std::string_view value) {
    const auto mark = text.find(token);
    if (mark != std::string::npos)
        text.replace(mark, token.size(), value);
    return text;
}

/// A player's name as the room shows it, read as UTF-8.
std::string player_name(Lobby& lobby, uint32_t id) {
    const int32_t slot = slot_of(lobby, id);
    if (slot == kNoSlot)
        return std::string(oa::data::languages::interface_text("a player who left"));
    const auto& player = slot_player(lobby, slot);
    return netgame::chat_utf8(
        std::string_view(player.name, ::strnlen(player.name, sizeof player.name))
    );
}

/// The units of one group, three at most by name and the rest counted.
std::string unit_list(Lobby& lobby, const netgame::PresenceUnitGroup& group) {
    namespace languages = oa::data::languages;
    std::vector<std::string> names;
    std::size_t unnamed = group.count > group.units.size() ? group.count - group.units.size() : 0;
    for (const auto& unit : group.units) {
        const LobbyUnit* mine =
            unit.name.empty() ? unit_of_key(lobby, unit.key) : unit_of_name(lobby, unit.name);
        const std::string code = mine != nullptr && mine->unit_name != nullptr
                                     ? std::string(mine->unit_name)
                                     : unit.name;
        if (code.empty()) {
            ++unnamed;
            continue;
        }
        if (names.size() == kNamedUnits) {
            ++unnamed;
            continue;
        }
        const char* shown = mine != nullptr ? mine->name : nullptr;
        names.push_back(
            shown != nullptr && shown[0] != '\0' ? std::string(shown) + " (" + code + ")" : code
        );
    }
    if (unnamed > 0)
        names.push_back(
            unnamed == 1 ? std::string(languages::interface_text("1 other unit"))
                         : filled(
                               std::string(languages::interface_text("{count} other units")),
                               "{count}",
                               std::to_string(unnamed)
                           )
        );
    if (names.empty())
        return {};
    std::string list = names.front();
    for (std::size_t at = 1; at < names.size(); ++at) {
        if (at + 1 < names.size())
            list += ", " + names[at];
        else
            list = filled(
                filled(std::string(languages::interface_text("{list} and {last}")), "{list}", list),
                "{last}",
                names[at]
            );
    }
    return list;
}

} // namespace

netgame::PresenceUnitSync unit_sync_differences(Lobby& lobby) {
    netgame::PresenceUnitSync found;
    auto& sync = lobby.sync;
    // A host that has not read its unit table compares nothing.
    if (lobby.game == nullptr || lobby.units == nullptr || lobby.unit_count <= 1 || !sync.host ||
        sync.finished || sync.peer_count == 0 || !unit_sync_complete(lobby))
        return found;
    std::vector<Machine> machines;
    Machine host;
    host.id = local_player(lobby).player_id;
    host.known = netgame::presence_files_known(
        {lobby.presence_records.local, lobby.presence_records.local_size}
    );
    host.checksums = true;
    machines.push_back(host);
    for (int32_t index = 0; index < sync.peer_count; ++index) {
        const auto& peer = sync.peers[index];
        if (!peer_waited_on(lobby, peer.player_id))
            continue;
        const auto* player = netgame::player_of_id(*lobby.game, peer.player_id);
        if (player == nullptr)
            continue;
        const auto* info = player_info(lobby, *player);
        Machine machine;
        machine.id = peer.player_id;
        machine.peer = &peer;
        machine.checksums = info != nullptr && reports_checksums(*info);
        const int32_t slot = slot_of(lobby, peer.player_id);
        machine.known =
            slot != kNoSlot && netgame::presence_files_known(lobby_presence_record(lobby, slot));
        machines.push_back(machine);
    }
    if (machines.size() < 2)
        return found;
    std::vector<Compared> compared;
    const auto compare = [&](uint32_t key, const LobbyUnit* mine) {
        std::vector<uint64_t> tags;
        tags.reserve(machines.size());
        for (const Machine& machine : machines) {
            if (machine.peer == nullptr) {
                tags.push_back(
                    mine != nullptr
                        ? unit_sync_content_checksum(lobby, *const_cast<LobbyUnit*>(mine))
                        : kAbsent
                );
                continue;
            }
            const uint32_t at = reported_index(*machine.peer, key);
            if (at == machine.peer->received)
                tags.push_back(kAbsent);
            else
                tags.push_back(machine.checksums ? machine.peer->checksums[at] : kPresent);
        }
        std::vector<uint32_t> named = differing_machines(machines, tags);
        if (named.empty())
            return;
        Compared unit;
        unit.key = key;
        if (mine != nullptr && mine->unit_name != nullptr)
            unit.name = mine->unit_name;
        unit.named = std::move(named);
        compared.push_back(std::move(unit));
    };
    for (int32_t type = 1; type < lobby.unit_count; ++type) {
        const LobbyUnit& unit = lobby.units[type];
        const auto* record = find(sync, unit.fbi_hash);
        // A unit no one can build in this battle is not a difference.
        if (record == nullptr || record->local == 0 || record->limit == 0)
            continue;
        compare(unit.fbi_hash, &unit);
    }
    for (const Machine& machine : machines) {
        if (machine.peer == nullptr)
            continue;
        for (uint32_t at = 0; at < machine.peer->received; ++at) {
            const uint32_t key = machine.peer->keys[at];
            if (find(sync, key) != nullptr)
                continue;
            bool seen = false;
            for (const Compared& unit : compared)
                seen = seen || unit.key == key;
            if (!seen)
                compare(key, nullptr);
        }
    }
    for (const Compared& unit : compared) {
        auto group = std::find_if(found.groups.begin(), found.groups.end(), [&](const auto& held) {
            return held.machines == unit.named;
        });
        if (group == found.groups.end()) {
            if (found.groups.size() == netgame::presence_unit_sync_groups_max)
                group = found.groups.end() - 1;
            else {
                found.groups.emplace_back();
                group = found.groups.end() - 1;
                group->machines = unit.named;
                if (group->machines.size() > netgame::presence_unit_sync_groups_max)
                    group->machines.resize(netgame::presence_unit_sync_groups_max);
            }
        }
        if (group->count < 0xffff)
            ++group->count;
        netgame::PresenceUnit listed;
        listed.name = unit.name;
        listed.key = unit.name.empty() ? unit.key : 0;
        group->units.push_back(std::move(listed));
        if (found.total < 0xffff)
            ++found.total;
    }
    return found;
}

std::vector<std::string> unit_sync_lines(Lobby& lobby, const netgame::PresenceUnitSync& found) {
    namespace languages = oa::data::languages;
    std::vector<std::string> lines;
    for (const auto& group : found.groups) {
        const std::string units = unit_list(lobby, group);
        if (units.empty() || group.machines.empty())
            continue;
        const bool one = group.count == 1;
        std::string line;
        if (group.machines.size() == 1)
            line = filled(
                std::string(
                    languages::interface_text(
                        one ? "*** {units} differs on {player}'s machine and can't be built in "
                              "this "
                              "battle."
                            : "*** {units} differ on {player}'s machine and can't be built in this "
                              "battle."
                    )
                ),
                "{player}",
                player_name(lobby, group.machines.front())
            );
        else if (group.machines.size() == 2)
            line = filled(
                filled(
                    std::string(
                        languages::interface_text(
                            one ? "*** {units} differs between {first}'s and {second}'s machines "
                                  "and "
                                  "can't be built in this battle."
                                : "*** {units} differ between {first}'s and {second}'s machines "
                                  "and "
                                  "can't be built in this battle."
                        )
                    ),
                    "{first}",
                    player_name(lobby, group.machines[0])
                ),
                "{second}",
                player_name(lobby, group.machines[1])
            );
        else
            line = filled(
                std::string(
                    languages::interface_text(
                        one ? "*** {units} differs between {count} players' machines and can't be "
                              "built in this battle."
                            : "*** {units} differ between {count} players' machines and can't be "
                              "built in this battle."
                    )
                ),
                "{count}",
                std::to_string(group.machines.size())
            );
        lines.push_back(filled(std::move(line), "{units}", units));
    }
    return lines;
}

void lobby_note_unit_sync(Lobby& lobby) {
    if (lobby.game == nullptr)
        return;
    auto& report = lobby.unit_report;
    netgame::PresenceUnitSync now;
    if (lobby.sync.host) {
        // Another hold keeps START grey and says why; the units wait for it.
        const bool settled = !lobby.sync.finished && lobby.sync.peer_count > 0 &&
                             unit_sync_complete(lobby) && lobby_content_allows_start(lobby) &&
                             lobby_stock_files_allow_start(lobby) &&
                             lobby_mutators_allow_start(lobby) && lobby_limits_allow_start(lobby) &&
                             lobby_sides_allow_start(lobby);
        if (!settled) {
            if (!report.found.groups.empty())
                report.accepted = false;
            report.found = {};
            report.state = 0;
            report.told = {};
            return;
        }
        const uint64_t state = sync_state(lobby.sync);
        if (state == report.state)
            return;
        report.state = state;
        now = unit_sync_differences(lobby);
        if (now != report.found)
            report.accepted = false;
        report.found = now;
    } else {
        const auto host = lobby_host_slot(lobby);
        const auto record = host == kNoSlot
                                ? std::span<const uint8_t>{}
                                : lobby_presence_record(lobby, static_cast<int32_t>(host));
        std::span<const uint8_t> value;
        uint64_t heard = 0;
        if (!record.empty() &&
            netgame::find_presence_field(
                record.data(), record.size(), netgame::PresenceTag::unit_sync, &value
            )) {
            heard = 0xcbf29ce484222325ULL;
            for (const uint8_t byte : value)
                mix(heard, byte);
        }
        if (heard == report.heard)
            return;
        report.heard = heard;
        if (heard != 0)
            if (auto read = netgame::presence_unit_sync(record))
                now = std::move(*read);
    }
    if (now.groups.empty()) {
        report.told = {};
        return;
    }
    if (now == report.told)
        return;
    report.told = now;
    for (std::string& line : unit_sync_lines(lobby, now)) {
        oa::platform::log_files::write_line(
            "multiplayer: unit sync: " + line.substr(line.rfind("*** ", 0) == 0 ? 4 : 0)
        );
        lobby_post_room_line(lobby, std::move(line));
    }
}

bool lobby_unit_sync_take_start(Lobby& lobby) {
    auto& report = lobby.unit_report;
    if (!lobby.sync.host || report.found.groups.empty() || report.accepted)
        return true;
    report.accepted = true;
    lobby_post_room_line(
        lobby,
        std::string(
            oa::data::languages::interface_text(
                "*** Press START again to start anyway. Those units stay unavailable in this "
                "battle."
            )
        )
    );
    oa::platform::log_files::write_line(
        "multiplayer: unit sync: START waits; the next press starts anyway"
    );
    return false;
}

} // namespace oa::ui::frontend_multiplayer
