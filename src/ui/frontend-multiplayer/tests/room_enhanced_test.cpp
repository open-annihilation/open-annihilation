// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced Mode's version and simulation agreement before a battle room starts.
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/room_enhanced.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace mp = oa::ui::frontend_multiplayer;
namespace ng = oa::netgame;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

std::vector<uint8_t> record(uint32_t version, std::optional<uint8_t> hash = uint8_t{7}) {
    ng::PresenceRecord value{};
    if (version != 0)
        value.enhanced_mode_version = version;
    if (hash) {
        value.sim_hash = ng::PresenceDigest{};
        value.sim_hash->fill(*hash);
    }
    std::array<uint8_t, ng::presence_record_max_bytes> bytes{};
    std::size_t size = 0;
    expect(
        ng::encode_presence(value, bytes.data(), bytes.size(), &size) == ng::WireError::ok,
        "fixture presence encodes"
    );
    return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)};
}

struct Room {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};

    explicit Room(bool hosting = true) {
        mp::lobby_reset(lobby, *game);
        game->players[0].player_id = hosting ? 100u : 200u;
        mp::lobby_seat_local(lobby, 0, hosting, hosting ? "Host" : "Guest");
        remote(1, hosting ? 200u : 100u, !hosting);
    }

    void remote(int32_t slot, uint32_t id, bool hosting = false, bool classic = false) {
        auto& player = mp::slot_player(lobby, slot);
        player.in_use = 1;
        player.player_id = id;
        player.index = static_cast<uint8_t>(slot);
        mp::slot_set_status(lobby, player, mp::kSlotRemote);
        auto* info = mp::slot_info(lobby, slot);
        info->state = mp::kInfoStatePlaying;
        info->net_id = id;
        info->role = hosting ? mp::kRoleHost : 0;
        info->engine_signature[0] = classic ? 0 : ng::engine_signature_first;
        info->engine_signature[1] = classic ? 0 : ng::engine_signature_second;
        lobby.presence_records.block_player[slot] = id;
    }

    void presence(int32_t slot, std::span<const uint8_t> bytes) {
        auto& records = lobby.presence_records;
        if (slot == 0) {
            std::memcpy(records.local, bytes.data(), bytes.size());
            records.local_size = static_cast<uint16_t>(bytes.size());
        } else {
            std::memcpy(records.peer[slot], bytes.data(), bytes.size());
            records.peer_size[slot] = static_cast<uint16_t>(bytes.size());
            records.peer_id[slot] = mp::slot_player(lobby, slot).player_id;
        }
    }
};

void classic_rooms_keep_their_start_rules() {
    Room room{};
    room.remote(1, 200, false, true);
    expect(mp::lobby_enhanced_allow_start(room.lobby), "classic Off room is allowed");
    room.presence(0, record(0, 2));
    room.presence(1, record(0, 3));
    expect(mp::lobby_enhanced_allow_start(room.lobby), "Off room does not add a sim hash gate");
    expect(mp::lobby_enhanced_start_problem(room.lobby) == nullptr, "Off has no refusal line");
    mp::Lobby empty{};
    expect(mp::lobby_enhanced_allow_start(empty), "a lobby without a game adds no hold");
}

void matching_machines_can_start() {
    for (bool hosting : {false, true}) {
        Room room(hosting);
        room.presence(0, record(1));
        room.presence(1, record(1));
        expect(mp::lobby_enhanced_allow_start(room.lobby), "matching enhanced peers may start");
        expect(
            mp::lobby_enhanced_start_problem(room.lobby) == nullptr,
            "matching enhanced peers have no refusal"
        );
    }
}

void enhanced_rooms_require_matching_acknowledgments() {
    Room room{};
    room.presence(0, record(1));
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "an absent peer record holds START");
    room.presence(1, record(0));
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "an Off peer holds enhanced START");
    expect(
        std::strstr(mp::lobby_enhanced_start_problem(room.lobby), "same mode") != nullptr,
        "the refusal explains selecting the same mode"
    );
    room.presence(1, record(2));
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "another version holds START");
    expect(
        std::strstr(mp::lobby_enhanced_start_problem(room.lobby), "version") != nullptr,
        "a version mismatch has a version refusal"
    );
    room.presence(1, record(1));
    room.remote(1, 200, false, true);
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "a classic setup cannot acknowledge");
    room.remote(1, 200);
    room.lobby.presence_records.block_player[1] = 201;
    expect(
        !mp::lobby_enhanced_allow_start(room.lobby), "an earlier setup block cannot acknowledge"
    );
    room.lobby.presence_records.block_player[1] = 200;
    room.lobby.presence_records.peer_id[1] = 201;
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "an earlier presence cannot acknowledge");
}

void enhanced_rules_must_agree() {
    Room room{};
    room.presence(0, record(1));
    room.presence(1, record(1, 8));
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "different rules hold START");
    room.presence(1, record(1, std::nullopt));
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "a missing peer hash holds START");
    room.presence(0, record(1, std::nullopt));
    room.presence(1, record(1));
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "a missing host hash holds START");
    expect(
        std::strstr(mp::lobby_enhanced_start_problem(room.lobby), "rules") != nullptr,
        "a rules mismatch has a rules refusal"
    );
}

void guests_do_not_silently_adopt_another_mode() {
    Room host{};
    host.presence(0, record(0));
    host.presence(1, record(1));
    expect(!mp::lobby_enhanced_allow_start(host.lobby), "an enhanced guest holds an Off host");
    Room guest(false);
    guest.presence(0, record(1));
    expect(
        !mp::lobby_enhanced_allow_start(guest.lobby), "an enhanced guest waits for the host record"
    );
    guest.presence(1, record(0));
    expect(!mp::lobby_enhanced_allow_start(guest.lobby), "a guest refuses its Off host");
    guest.presence(1, record(2));
    expect(!mp::lobby_enhanced_allow_start(guest.lobby), "a guest refuses a newer host version");
}

void watchers_agree_and_computers_use_their_machine() {
    Room room{};
    room.presence(0, record(1));
    room.presence(1, record(1));
    room.remote(2, 300);
    auto* info = mp::slot_info(room.lobby, 2);
    info->state = mp::kInfoStateDefeated;
    expect(mp::lobby_enhanced_allow_start(room.lobby), "remote computer needs no separate record");
    info->options = mp::option::watcher;
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "a watcher must acknowledge");
    room.presence(2, record(1));
    expect(mp::lobby_enhanced_allow_start(room.lobby), "an agreeing watcher permits START");
    auto& computer = mp::slot_player(room.lobby, 3);
    computer.in_use = 1;
    computer.index = 3;
    mp::slot_set_status(room.lobby, computer, mp::kSlotComputer);
    expect(mp::lobby_enhanced_allow_start(room.lobby), "local computer uses its machine's record");
}

void the_host_checks_every_machine_and_guests_check_the_host() {
    Room guest(false);
    guest.presence(0, record(1));
    guest.presence(1, record(1));
    guest.remote(2, 300);
    expect(
        mp::lobby_enhanced_allow_start(guest.lobby),
        "a guest need not receive another guest's acknowledgment before starting"
    );
    guest.presence(0, record(0));
    expect(
        !mp::lobby_enhanced_allow_start(guest.lobby),
        "the guest must still agree with its enhanced host"
    );

    Room host{};
    host.presence(0, record(1));
    host.presence(1, record(1));
    host.remote(2, 300);
    expect(
        !mp::lobby_enhanced_allow_start(host.lobby),
        "the host waits for every seated machine's acknowledgment"
    );
    host.presence(2, record(1));
    expect(
        mp::lobby_enhanced_allow_start(host.lobby), "the host can start once every machine agrees"
    );
}

void malformed_presence_cannot_acknowledge() {
    Room room{};
    room.presence(0, record(1));
    auto bytes = record(1);
    bytes.pop_back();
    room.presence(1, bytes);
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "a truncated record holds START");
}

} // namespace

int main() {
    classic_rooms_keep_their_start_rules();
    matching_machines_can_start();
    enhanced_rooms_require_matching_acknowledgments();
    enhanced_rules_must_agree();
    guests_do_not_silently_adopt_another_mode();
    watchers_agree_and_computers_use_their_machine();
    the_host_checks_every_machine_and_guests_check_the_host();
    malformed_presence_cannot_acknowledge();
    return failures == 0 ? 0 : 1;
}
