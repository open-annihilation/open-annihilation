// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Two machines in one process over 127.0.0.1: the battle-room model
// hosts and joins through the session-backed LobbyNet, exchanges player
// info and chat, starts, passes the load barrier with negotiated start
// positions, then plays 300 ticks with orders while each side replicates
// its own units; both Worlds must agree. Also covers the packet layer over
// an in-memory transport and the launch helpers.

#include "oa/ui/frontend_multiplayer/launch_block.hpp"
#include "oa/netgame/match/launch.hpp"
#include "oa/netgame/match/net_match.hpp"
#include "oa/netgame/match/packet_layer.hpp"
#include "oa/netgame/match/session_lobby.hpp"
#include "oa/netgame/network.hpp"
#include "oa/netgame/private_channel.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/netgame/unit_state.hpp"
#include "oa/ui/frontend_multiplayer/connect.hpp"
#include "oa/base/text.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace oa;
using namespace oa::netgame;
using namespace oa::netgame::match;
namespace mp = oa::ui::frontend_multiplayer;

namespace {

int failures = 0;
const char* current_test = "";

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::fprintf(stderr, "%s: %s:%d: %s\n", current_test, __FILE__, __LINE__, #condition); \
        }                                                                                          \
    } while (0)

// Names the case every later check reports and prints it at once, so a case
// that ends the test abnormally is still named in its output.
void start_case(const char* name) {
    current_test = name;
    std::printf("net match: %s\n", name);
    std::fflush(stdout);
}

// ---- in-memory transport ----

struct Datagram {
    uint32_t from{};
    uint32_t to{};
    std::vector<uint8_t> bytes;
};

struct Wire {
    std::deque<Datagram> queue;
};

uint32_t
wire_send(void* context, uint32_t from, uint32_t to, uint32_t, const uint8_t* data, uint32_t size) {
    static_cast<Wire*>(context)->queue.push_back(
        {from, to, std::vector<uint8_t>(data, data + size)}
    );
    return transport_result::ok;
}

uint32_t
wire_receive(void* context, uint32_t* from, uint32_t* to, uint8_t* buffer, uint32_t* size) {
    auto* wire = static_cast<Wire*>(context);
    if (wire->queue.empty())
        return transport_result::no_messages;
    const auto& d = wire->queue.front();
    if (d.bytes.size() > *size) {
        *size = static_cast<uint32_t>(d.bytes.size());
        return transport_result::buffer_too_small;
    }
    std::memcpy(buffer, d.bytes.data(), d.bytes.size());
    *size = static_cast<uint32_t>(d.bytes.size());
    *from = d.from;
    *to = d.to;
    wire->queue.pop_front();
    return transport_result::ok;
}

void packet_layer_over_memory() {
    start_case("packet_layer_over_memory");
    Wire wire;
    const NetTransport transport{&wire, wire_send, wire_receive};
    auto sender = std::make_unique<PacketLayer>();
    auto receiver = std::make_unique<PacketLayer>();
    packet_layer_create(sender.get());
    packet_layer_start(sender.get(), transport);
    packet_layer_create(receiver.get());
    packet_layer_start(receiver.get(), transport);
    CHECK(sender->ticks_between_sends == 6);

    ChatRecord chat{};
    oa::base::text::copy_padded(chat.text, "one", sizeof chat.text);
    uint8_t chat_bytes[65];
    CHECK(encode_record(chat, chat_bytes, sizeof chat_bytes, nullptr) == WireError::ok);
    const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
    CHECK(
        packet_layer_send(
            sender.get(), 7, broadcast_destination_id, chat_bytes, sizeof chat_bytes, 100
        ) == WireError::ok
    );
    CHECK(
        packet_layer_send(sender.get(), 7, broadcast_destination_id, &probe, 1, 100) ==
        WireError::ok
    );
    CHECK(packet_layer_send(sender.get(), 7, 9, &probe, 1, 100) == WireError::ok);
    // The first flush is due at once; the next waits six time ticks.
    packet_layer_flush(sender.get(), 100, false);
    CHECK(wire.queue.size() == 2);
    CHECK(
        packet_layer_send(sender.get(), 7, broadcast_destination_id, &probe, 1, 101) ==
        WireError::ok
    );
    packet_layer_flush(sender.get(), 105, false);
    CHECK(wire.queue.size() == 2);
    packet_layer_flush(sender.get(), 106, false);
    CHECK(wire.queue.size() == 3);
    CHECK(wire.queue[0].to == 0 && wire.queue[1].to == 9);
    const auto frame = oa::netgame::network::decode_frame(wire.queue.front().bytes);
    CHECK(
        frame.size() == 4 + 65 + 1 &&
        static_cast<int32_t>(load_u32(frame.data())) == first_broadcast_frame_sequence
    );

    // A system message passes through raw, ahead of the frames behind it.
    wire.queue.push_front({system_message_sender_id, 7, {0x05, 0, 0, 0, 1, 0, 0, 0, 9, 0, 0, 0}});
    Packet packet{};
    CHECK(
        packet_layer_receive(receiver.get(), 0, &packet) && packet.kind == PacketKind::system &&
        packet.size == 12
    );
    // Tick 0 never holds: both records of the broadcast frame, then the unicast.
    CHECK(
        packet_layer_receive(receiver.get(), 0, &packet) && packet.kind == PacketKind::record &&
        packet.from_id == 7 && packet.size == 65 && std::memcmp(packet.data + 1, "one", 3) == 0
    );
    CHECK(
        packet_layer_receive(receiver.get(), 0, &packet) && packet.size == 1 &&
        packet.data[0] == probe
    );
    CHECK(
        packet_layer_receive(receiver.get(), 0, &packet) && packet.to_id == 9 &&
        packet.data[0] == probe
    );
    CHECK(
        packet_layer_receive(receiver.get(), 0, &packet) && packet.to_id == 0 &&
        packet.data[0] == probe
    );
    CHECK(!packet_layer_receive(receiver.get(), 0, &packet));

    // At a live tick a fresh frame spreads its records one tick apart.
    for (int i = 0; i < 3; ++i)
        CHECK(
            packet_layer_send(sender.get(), 7, broadcast_destination_id, &probe, 1, 200) ==
            WireError::ok
        );
    packet_layer_flush(sender.get(), 200, true);
    CHECK(packet_layer_receive(receiver.get(), 500, &packet));
    CHECK(!packet_layer_receive(receiver.get(), 500, &packet));
    CHECK(packet_layer_receive(receiver.get(), 501, &packet));
    CHECK(packet_layer_receive(receiver.get(), 502, &packet));
    CHECK(!packet_layer_receive(receiver.get(), 502, &packet));
    CHECK(receiver->dropped_frames == 0);
}

// The condenser reads the Game's send options for every frame:
// Game.compression_off sends it stored (type 3) even when it would compress,
// and Game.send_error_percent drops that share of frames before the
// transport, still counting them as sent.
void packet_layer_reads_game_send_options() {
    start_case("packet_layer_reads_game_send_options");
    Wire wire;
    const NetTransport transport{&wire, wire_send, wire_receive};
    auto sender = std::make_unique<PacketLayer>();
    packet_layer_create(sender.get());
    packet_layer_start(sender.get(), transport);
    auto game = std::make_unique<Game>();
    sender->send_options = game.get();
    ChatRecord chat{};
    std::memset(chat.text, 'a', sizeof chat.text - 1);
    uint8_t chat_bytes[65];
    CHECK(encode_record(chat, chat_bytes, sizeof chat_bytes, nullptr) == WireError::ok);
    const auto send_chat = [&] {
        CHECK(
            packet_layer_send(
                sender.get(), 7, broadcast_destination_id, chat_bytes, sizeof chat_bytes, 100
            ) == WireError::ok
        );
        packet_layer_flush(sender.get(), 100, true);
    };
    send_chat();
    CHECK(wire.queue.size() == 1 && wire.queue.back().bytes[0] == condenser_type_compressed);
    game->compression_off = 1;
    send_chat();
    CHECK(
        wire.queue.size() == 2 && wire.queue.back().bytes[0] == condenser_type_stored &&
        wire.queue.back().bytes.size() == condenser_header_bytes + 4 + sizeof chat_bytes
    );
    const auto sent = sender->traffic.sent_datagrams;
    game->send_error_percent = 100;
    for (int i = 0; i < 20; ++i)
        send_chat();
    CHECK(wire.queue.size() == 2 && sender->traffic.sent_datagrams == sent + 20);
    // A frame goes out when percent < rand() * 101 / 0x8000.
    constexpr int frames = 200;
    std::srand(1);
    game->send_error_percent = 50;
    for (int i = 0; i < frames; ++i)
        send_chat();
    const auto delivered = static_cast<int>(wire.queue.size()) - 2;
    CHECK(delivered > frames / 4 && delivered < frames * 3 / 4);
    wire.queue.resize(2);
    game->send_error_percent = 0;
    game->compression_off = 0;
    sender->send_options = nullptr;
    send_chat();
    CHECK(wire.queue.size() == 3 && wire.queue.back().bytes[0] == condenser_type_compressed);
}

// Among the local and remote players in use the highest id,
// compared unsigned, takes the host role; a computer player's id never counts.
void host_designation() {
    start_case("host_designation");
    auto world = std::make_unique<World>();
    for (uint32_t i = 0; i < OA_PLAYER_RECORD_COUNT; ++i)
        world_player_record(world.get(), i)->info = oa_ref_from_index(i);
    auto& players = world->game.players;
    const auto seat = [&](uint8_t slot, uint8_t status, uint32_t id) {
        players[slot].in_use = 1;
        players[slot].status = status;
        players[slot].player_id = id;
    };
    seat(0, OA_PLAYER_STATUS_LOCAL, 0x10);
    seat(1, OA_PLAYER_STATUS_MIRRORED, 0x80000001u);
    seat(2, OA_PLAYER_STATUS_COMPUTER, 0xfffffff0u);
    seat(3, OA_PLAYER_STATUS_MIRRORED, 0x30);
    world->player_info[1].role = 0x20;
    net_match_designate_host(world.get());
    CHECK(world->player_info[1].role == 0x21);
    CHECK(world->player_info[0].role == 0 && world->player_info[2].role == 0);
    CHECK(world->player_info[3].role == 0);
    players[1].in_use = 0;
    world->player_info[1].role = 0;
    net_match_designate_host(world.get());
    CHECK((world->player_info[3].role & 0x01) != 0 && world->player_info[1].role == 0);
    // No local or remote player left: the search keeps id 0 and the lookup
    // resolves id 0 like any other, so the slot answering to it is promoted.
    for (uint32_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        seat(static_cast<uint8_t>(i), OA_PLAYER_STATUS_FREE, 0xffffffffu);
        players[i].in_use = 0;
        world->player_info[i].role = 0;
    }
    seat(4, OA_PLAYER_STATUS_COMPUTER, 0x40);
    seat(6, OA_PLAYER_STATUS_COMPUTER, 0);
    net_match_designate_host(world.get());
    CHECK(world->player_info[6].role == 0x01 && world->player_info[4].role == 0);
}

// A transport that reports a message longer than any buffer.
uint32_t endless_receive(void* context, uint32_t*, uint32_t*, uint8_t*, uint32_t* size) {
    ++*static_cast<uint32_t*>(context);
    *size = 0x7fffffffu;
    return transport_result::buffer_too_small;
}

void packet_layer_drops_oversized() {
    start_case("packet_layer_drops_oversized");
    Wire wire;
    const NetTransport transport{&wire, wire_send, wire_receive};
    auto sender = std::make_unique<PacketLayer>();
    auto receiver = std::make_unique<PacketLayer>();
    packet_layer_create(sender.get());
    packet_layer_start(sender.get(), transport);
    packet_layer_create(receiver.get());
    packet_layer_start(receiver.get(), transport);

    // A datagram longer than the condenser storage heads the queue: it is
    // taken off and dropped, and the frame behind it is delivered.
    wire.queue.push_back(
        {7, 0, std::vector<uint8_t>(sizeof receiver->rx + 1, condenser_type_stored)}
    );
    const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
    CHECK(
        packet_layer_send(sender.get(), 7, broadcast_destination_id, &probe, 1, 100) ==
        WireError::ok
    );
    packet_layer_flush(sender.get(), 100, true);
    CHECK(wire.queue.size() == 2);
    Packet packet{};
    CHECK(
        packet_layer_receive(receiver.get(), 0, &packet) && packet.kind == PacketKind::record &&
        packet.from_id == 7 && packet.data[0] == probe
    );
    CHECK(receiver->dropped_frames == 1 && wire.queue.empty());

    // One too long to take off: the call returns instead of spinning.
    uint32_t calls = 0;
    packet_layer_start(receiver.get(), NetTransport{&calls, nullptr, endless_receive});
    CHECK(
        !packet_layer_receive(receiver.get(), 0, &packet) && calls == 1 &&
        receiver->dropped_frames == 2
    );
}

// Ten battle-room senders fill every frame slot; one leaves and a new
// joiner takes its player record, so the joiner takes its frame slot.
void packet_layer_reclaims_lobby_slots() {
    start_case("packet_layer_reclaims_lobby_slots");
    Wire wire;
    const NetTransport transport{&wire, wire_send, wire_receive};
    auto sender = std::make_unique<PacketLayer>();
    auto receiver = std::make_unique<PacketLayer>();
    packet_layer_create(sender.get());
    packet_layer_start(sender.get(), transport);
    packet_layer_create(receiver.get());
    packet_layer_start(receiver.get(), transport);
    auto game = std::make_unique<Game>();
    receiver->receiver.players = game->players;
    const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
    const auto send_from = [&](uint32_t id) {
        CHECK(
            packet_layer_send(sender.get(), id, broadcast_destination_id, &probe, 1, 100) ==
            WireError::ok
        );
        packet_layer_flush(sender.get(), 100, true);
    };
    Packet packet{};
    for (uint32_t id = 101; id <= 110; ++id) {
        game->players[id - 101].player_id = id;
        send_from(id);
        CHECK(packet_layer_receive(receiver.get(), 0, &packet) && packet.from_id == id);
    }
    // Every player record still names its sender: an eleventh is refused.
    send_from(200);
    CHECK(!packet_layer_receive(receiver.get(), 0, &packet) && receiver->dropped_frames == 1);
    packet_layer_release_peer(receiver.get(), 103);
    game->players[2].player_id = 200;
    send_from(200);
    CHECK(
        packet_layer_receive(receiver.get(), 0, &packet) && packet.from_id == 200 &&
        receiver->receiver.peers[2].peer_id == 200 && receiver->dropped_frames == 1
    );
}

// A frame that arrived while its sender's ring still held a
// record waits; the next call parses it and hands out its record before
// reading on, so a departing player's last record (here a relayed 0x1c)
// comes ahead of the session's destroy-player message queued behind it.
void packet_layer_parses_waiting_frame_first() {
    start_case("packet_layer_parses_waiting_frame_first");
    Wire wire;
    const NetTransport transport{&wire, wire_send, wire_receive};
    auto sender = std::make_unique<PacketLayer>();
    auto receiver = std::make_unique<PacketLayer>();
    packet_layer_create(sender.get());
    packet_layer_start(sender.get(), transport);
    packet_layer_create(receiver.get());
    packet_layer_start(receiver.get(), transport);
    const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
    CHECK(
        packet_layer_send(sender.get(), 7, broadcast_destination_id, &probe, 1, 100) ==
        WireError::ok
    );
    CHECK(
        packet_layer_send(sender.get(), 7, broadcast_destination_id, &probe, 1, 100) ==
        WireError::ok
    );
    packet_layer_flush(sender.get(), 100, true);
    DisconnectNoticeRecord leaving{};
    leaving.player_id = 7;
    uint8_t leaving_bytes[5];
    CHECK(encode_record(leaving, leaving_bytes, sizeof leaving_bytes, nullptr) == WireError::ok);
    CHECK(
        packet_layer_send(
            sender.get(), 7, broadcast_destination_id, leaving_bytes, sizeof leaving_bytes, 101
        ) == WireError::ok
    );
    packet_layer_flush(sender.get(), 101, true);
    wire.queue.push_back({system_message_sender_id, 9, {0x05, 0, 0, 0, 1, 0, 0, 0, 7, 0, 0, 0}});
    CHECK(wire.queue.size() == 3);

    Packet packet{};
    // The second probe is due a tick later until the notice's frame, finding
    // it queued, restamps it for this tick and waits.
    CHECK(packet_layer_receive(receiver.get(), 500, &packet) && packet.data[0] == probe);
    CHECK(
        packet_layer_receive(receiver.get(), 500, &packet) && packet.data[0] == probe &&
        receiver->pending
    );
    CHECK(
        packet_layer_receive(receiver.get(), 500, &packet) && packet.kind == PacketKind::record &&
        packet.from_id == 7 && packet.data[0] == leaving_bytes[0] && !receiver->pending
    );
    CHECK(packet_layer_receive(receiver.get(), 500, &packet) && packet.kind == PacketKind::system);
    CHECK(!packet_layer_receive(receiver.get(), 500, &packet) && receiver->dropped_frames == 0);
}

void launch_helpers() {
    start_case("launch_helpers");
    CHECK(unit_def_id_bits_for_count(0) == 0);
    CHECK(unit_def_id_bits_for_count(1) == 1);
    CHECK(unit_def_id_bits_for_count(255) == 8);
    CHECK(unit_def_id_bits_for_count(256) == 9);
    CHECK(unit_def_id_bits_for_count(409) == 9);
    CHECK(launch_random_seed(0x0000000500000007ull) == 12u);
    CHECK(launch_random_seed(0xffffffff00000002ull) == 1u);

    // Ranges follow ascending player id whatever the slot order.
    World* world = world_create();
    const WorldCapacity capacity{3 * OA_PLAYER_COUNT + 1, 2, 0};
    CHECK(world != nullptr && world_alloc_tables(world, &capacity) != 0);
    for (uint32_t i = 0; i < world->unit_slot_count; ++i)
        world->units[i].id = static_cast<uint16_t>(i);
    world->game.units_per_player = 3;
    for (uint8_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        world->game.players[i].index = i;
        world->game.players[i].player_id = no_player_id; // free slots, as a launch leaves them
    }
    world->game.players[0].player_id = 0x300;
    world->game.players[1].player_id = 0x200;
    match_assign_unit_ranges(world);
    // 0x200 (slot 1) first, then 0x300, then the eight free slots.
    CHECK(world->game.players[1].first_unit == oa_ref_from_index(1));
    CHECK(world->game.players[1].base_unit_id == 1);
    CHECK(world->game.players[0].first_unit == oa_ref_from_index(3 + 1));
    CHECK(world->units[3 + 1].owner == oa_ref_from_index(0) && world->units[3].owner_index == 1);
    // A watcher between them still takes its range in id order.
    for (uint8_t i = 0; i < 3; ++i) {
        world->game.players[i].in_use = 1;
        world->game.players[i].info = oa_ref_from_index(i);
    }
    world->game.players[2].player_id = 0x250;
    world->player_info[2].options = OA_SETUP_OPTION_WATCHER;
    CHECK(
        match_slot_watcher(world, 2) && !match_slot_watcher(world, 0) &&
        !match_slot_watcher(world, 9)
    );
    match_assign_unit_ranges(world);
    CHECK(world->game.players[1].first_unit == oa_ref_from_index(1));
    CHECK(world->game.players[2].first_unit == oa_ref_from_index(3 + 1));
    CHECK(world->game.players[0].first_unit == oa_ref_from_index(2 * 3 + 1));
    // A local watcher loses mapping and line of sight (bits 0
    // and 1) and keeps the line-of-sight type and the fog mask bit.
    world->game.visibility_flags = 0x0f;
    world->game.local_player_index = 0;
    match_apply_watcher_view(world);
    CHECK(world->game.visibility_flags == 0x0f);
    world->game.local_player_index = 2;
    match_apply_watcher_view(world);
    CHECK(world->game.visibility_flags == 0x0c);
    // The schema count is one past the highest slot the host's
    // table fills, or the player count without one.
    world->game.player_count = 3;
    CHECK(match_layout_player_count(world->game) == 3);
    world->game.slot_table[0] = 0x300;
    world->game.slot_table[4] = 0x250;
    world->game.slot_table[7] = no_player_id;
    CHECK(match_layout_player_count(world->game) == 5);
    // The ids compare unsigned: an id with the top bit set
    // sorts after small ones. Equal ids (the free slots) keep slot order, as
    // an insertion sort of ten records does.
    world->game.players[2].player_id = 0x80000000u;
    match_assign_unit_ranges(world);
    CHECK(world->game.players[1].first_unit == oa_ref_from_index(1));
    CHECK(world->game.players[0].first_unit == oa_ref_from_index(3 + 1));
    CHECK(world->game.players[2].first_unit == oa_ref_from_index(2 * 3 + 1));
    CHECK(world->game.players[3].first_unit == oa_ref_from_index(3 * 3 + 1));
    CHECK(world->game.players[9].first_unit == oa_ref_from_index(9 * 3 + 1));
    world_destroy(world);
}

/// A side at or past the match's side count is the first slot this machine places.
void launch_finds_a_side_past_the_table() {
    start_case("launch_finds_a_side_past_the_table");
    CHECK(match_first_side_past_table(nullptr) == OA_PLAYER_COUNT);

    World* world = world_create();
    CHECK(world != nullptr);
    world->game.side_count = 2;

    auto& local = world->game.players[0];
    local.in_use = 1;
    local.status = OA_PLAYER_STATUS_LOCAL;
    local.info = oa_ref_from_index(0);
    world->player_info[0].side = 1;
    CHECK(match_first_side_past_table(world) == OA_PLAYER_COUNT);

    auto& computer = world->game.players[1];
    computer.in_use = 1;
    computer.status = OA_PLAYER_STATUS_COMPUTER;
    computer.info = oa_ref_from_index(1);
    world->player_info[1].side = 2;
    CHECK(match_first_side_past_table(world) == 1);

    auto& remote = world->game.players[2];
    remote.in_use = 1;
    remote.status = OA_PLAYER_STATUS_MIRRORED;
    remote.info = oa_ref_from_index(2);
    world->player_info[2].side = 7;
    auto& watcher = world->game.players[3];
    watcher.in_use = 1;
    watcher.status = OA_PLAYER_STATUS_LOCAL;
    watcher.info = oa_ref_from_index(3);
    world->player_info[3].side = 9;
    world->player_info[3].options = OA_SETUP_OPTION_WATCHER;
    CHECK(match_first_side_past_table(world) == 1);

    computer.in_use = 0;
    CHECK(match_first_side_past_table(world) == OA_PLAYER_COUNT);

    computer.in_use = 1;
    world->game.side_count = 0;
    CHECK(match_first_side_past_table(world) == OA_PLAYER_COUNT);
    world_destroy(world);
}

// Under an active launch the match takes the options lock the
// battle room set from it (Game.setup_options bit 0), set or clear; without
// one it keeps its own, as the game switch -b lock sets it.
void launch_carries_the_options_lock() {
    start_case("launch_carries_the_options_lock");
    constexpr uint16_t kLocked = 0x0001;
    constexpr uint16_t kOtherOptions = 0x0100;
    auto game = std::make_unique<Game>();
    auto lobby = std::make_unique<mp::Lobby>();
    mp::lobby_reset(*lobby, *game);
    mp::lobby_seat_local(*lobby, 0, true, "hoster");
    World* world = world_create();
    const WorldCapacity capacity{OA_PLAYER_COUNT + 1, 2, 0};
    CHECK(world != nullptr && world_alloc_tables(world, &capacity) != 0);

    world->game.setup_options = kLocked | kOtherOptions;
    CHECK(match_launch_apply(*lobby, world));
    CHECK(world->game.setup_options == (kLocked | kOtherOptions));

    oa::ui::frontend_multiplayer::launch::LaunchBlock block{};
    lobby->launch_link.block = &block;
    lobby->launch_link.launch_active = [](void*) { return true; };
    game->setup_options = kLocked;
    world->game.setup_options = kOtherOptions;
    CHECK(match_launch_apply(*lobby, world));
    CHECK(world->game.setup_options == (kLocked | kOtherOptions));
    game->setup_options = 0;
    CHECK(match_launch_apply(*lobby, world));
    CHECK(world->game.setup_options == kOtherOptions);
    world_destroy(world);
}

// The match counts the seated players, not the slots up to the last one: with
// slot 1 closed, a player joining a host in slot 0 sits in slot 2, so the two
// make two where the start positions' layout counts three.
void launch_counts_seated_players() {
    start_case("launch_counts_seated_players");
    auto game = std::make_unique<Game>();
    auto lobby = std::make_unique<mp::Lobby>();
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(*lobby, *game);
    lobby->net = mp::loopback_lobby_net(loopback);
    game->players[0].player_id = 0x300;
    mp::lobby_seat_local(*lobby, 0, true, "hoster");
    game->players[1].status = mp::kSlotBlocked;
    mp::LobbyEvent joined{};
    joined.kind = mp::LobbyEventKind::player_joined;
    joined.player_id = 0x302;
    CHECK(mp::lobby_apply_event(*lobby, joined));
    CHECK(mp::slot_for_player_id(*lobby, 0x302) == 2 && mp::lobby_player_count(*game) == 2);
    game->slot_table[0] = 0x300;
    game->slot_table[1] = no_player_id;
    game->slot_table[2] = 0x302;
    CHECK(match_layout_player_count(*game) == 3);
    World* world = world_create();
    const WorldCapacity capacity{OA_PLAYER_COUNT + 1, 2, 0};
    CHECK(world != nullptr && world_alloc_tables(world, &capacity) != 0);
    world->game.player_count = 3;
    CHECK(match_launch_apply(*lobby, world));
    CHECK(world->game.player_count == 2);
    world_destroy(world);
}

// ---- machines seating two players ----

// Delivery to one machine as DirectPlay makes it: a frame to all players
// reaches each of the machine's local players, a frame to one player only
// that player. Every id a frame was sent to is kept.
struct MachineInbox {
    std::vector<uint32_t> local_ids;
    std::vector<uint32_t> sent_to;
    Wire wire;
};

uint32_t machine_send(
    void* context, uint32_t from, uint32_t to, uint32_t, const uint8_t* data, uint32_t size
) {
    auto* inbox = static_cast<MachineInbox*>(context);
    inbox->sent_to.push_back(to);
    for (const auto id : inbox->local_ids)
        if (to == broadcast_destination_id || to == id)
            inbox->wire.queue.push_back({from, id, std::vector<uint8_t>(data, data + size)});
    return transport_result::ok;
}

uint32_t
machine_receive(void* context, uint32_t* from, uint32_t* to, uint8_t* buffer, uint32_t* size) {
    return wire_receive(&static_cast<MachineInbox*>(context)->wire, from, to, buffer, size);
}

struct Seat {
    uint32_t id{};
    uint8_t status{};
    uint32_t machine_group{};
};

World* seated_world(const Seat (&seats)[4], uint32_t unit_slots = 4 * OA_PLAYER_COUNT + 1) {
    World* world = world_create();
    const WorldCapacity capacity{unit_slots, 2, 0};
    CHECK(world != nullptr && world_alloc_tables(world, &capacity) != 0);
    for (uint8_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        auto& p = world->game.players[i];
        p.index = i;
        p.player_id = no_player_id;
        p.info = oa_ref_from_index(i);
        if (i < 4) {
            p.in_use = 1;
            p.player_id = seats[i].id;
            p.status = seats[i].status;
            p.machine_group = seats[i].machine_group;
        }
    }
    world->game.local_player_index = 0;
    world->game.session_flags = kNetFlagLive;
    return world;
}

// A machine seating a human and a computer player receives a broadcast once
// per local player, and the pump neither drops a
// repeated frame nor filters records by addressee: each copy is applied.
// The game avoids that at the sender: once
// some machine group holds two players every broadcast goes as one frame
// to the first remote player of each machine.
void shared_machines_receive_broadcasts_once() {
    start_case("shared_machines_receive_broadcasts_once");
    constexpr uint32_t kSender = 7, kHuman = 9, kComputer = 10, kOther = 11;
    const Seat sender_view[4] = {
        {kSender, OA_PLAYER_STATUS_LOCAL, 1},
        {kHuman, OA_PLAYER_STATUS_MIRRORED, 2},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 2},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    const Seat receiver_view[4] = {
        {kSender, OA_PLAYER_STATUS_MIRRORED, 1},
        {kHuman, OA_PLAYER_STATUS_LOCAL, 2},
        {kComputer, OA_PLAYER_STATUS_COMPUTER, 2},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    World* sender_world = seated_world(sender_view);
    World* receiver_world = seated_world(receiver_view);
    receiver_world->game.local_player_index = 1;
    MachineInbox inbox;
    inbox.local_ids = {kHuman, kComputer};
    NetConnection sender_connection{};
    NetConnection receiver_connection{};
    sender_connection.packets = new PacketLayer();
    receiver_connection.packets = new PacketLayer();
    packet_layer_create(sender_connection.packets);
    packet_layer_start(sender_connection.packets, NetTransport{&inbox, machine_send, nullptr});
    packet_layer_create(receiver_connection.packets);
    packet_layer_start(receiver_connection.packets, NetTransport{&inbox, nullptr, machine_receive});
    auto sender = std::make_unique<NetMatch>();
    auto receiver = std::make_unique<NetMatch>();
    net_match_begin(
        sender.get(), &sender_connection, sender_world, ReplicationSim{}, NetMatchHooks{}
    );
    net_match_begin(
        receiver.get(), &receiver_connection, receiver_world, ReplicationSim{}, NetMatchHooks{}
    );
    net_match_enter_game(sender.get());
    net_match_enter_game(receiver.get());

    UnitKilledRecord killed{};
    killed.unit_index = 1;
    uint8_t record[16];
    std::size_t written = 0;
    CHECK(encode_record(killed, record, sizeof record, &written) == WireError::ok);
    const auto broadcast = [&] {
        inbox.sent_to.clear();
        receiver->records_applied = 0;
        CHECK(net_match_send(sender.get(), kSender, broadcast_destination_id, record, written));
        packet_layer_flush(sender_connection.packets, 0, true);
        (void)net_match_pump(receiver.get());
    };

    // Groups not yet counted: one frame to all players, applied twice.
    CHECK(sender_world->game.shared_machines == 0);
    broadcast();
    CHECK(inbox.sent_to.size() == 1 && inbox.sent_to[0] == broadcast_destination_id);
    CHECK(receiver->records_applied == 2 && receiver->record_errors == 0);

    // Group 2 seats two players: one frame for each other machine.
    ui::frontend_multiplayer::note_shared_machines(sender_world->game);
    CHECK(sender_world->game.shared_machines == 1);
    broadcast();
    CHECK(inbox.sent_to.size() == 2 && inbox.sent_to[0] == kHuman && inbox.sent_to[1] == kOther);
    CHECK(receiver->records_applied == 1);

    // One destination takes a local sender and a remote
    // receiver, neither rejected.
    inbox.sent_to.clear();
    CHECK(net_match_send(sender.get(), kSender, kOther, record, written));
    packet_layer_flush(sender_connection.packets, 0, true);
    CHECK(inbox.sent_to.size() == 1 && inbox.sent_to[0] == kOther);
    CHECK(!net_match_send(sender.get(), kSender, kSender, record, written));
    CHECK(!net_match_send(sender.get(), kHuman, kOther, record, written));
    sender_world->game.players[3].reject_reason = 1;
    CHECK(!net_match_send(sender.get(), kSender, kOther, record, written));
    sender_world->game.players[3].reject_reason = 0;
    sender_world->game.players[0].reject_reason = 1;
    CHECK(!net_match_send(sender.get(), kSender, kOther, record, written));
    sender_world->game.players[0].reject_reason = 0;

    // In a match a player who now watches announces its lobby
    // block (0x20) and team (0x24); the other machines copy both.
    sender_world->player_info[0].options = OA_SETUP_OPTION_WATCHER;
    sender_world->game.players[0].team = 3;
    inbox.sent_to.clear();
    net_match_send_player_status(sender.get(), false);
    (void)net_match_pump(receiver.get());
    CHECK((receiver_world->player_info[0].options & OA_SETUP_OPTION_WATCHER) != 0);
    CHECK(receiver_world->game.players[0].team == 3);
    CHECK(match_slot_watcher(receiver_world, 0));

    // The pump counts the groups again as it ends: the receiver's own
    // machine holds two players too.
    CHECK(receiver_world->game.shared_machines == 1);
    receiver_world->game.players[2].status = OA_PLAYER_STATUS_FREE;
    (void)net_match_pump(receiver.get());
    CHECK(receiver_world->game.shared_machines == 0);

    delete sender_connection.packets;
    delete receiver_connection.packets;
    world_destroy(sender_world);
    world_destroy(receiver_world);
}

// Every local and computer player takes the mean of the six
// load rows and sends it as 0x2a. The loading screen's active players split
// 0x26c pixels from x 0xb, each bar two short of its share and filled by
// percent; a player counts as ready at 100 once its "loaded" record came.
void loading_screen_progress() {
    start_case("loading_screen_progress");
    constexpr uint32_t kSender = 7, kComputer = 8, kReceiver = 9, kOther = 11;
    const Seat sender_view[4] = {
        {kSender, OA_PLAYER_STATUS_LOCAL, 1},
        {kComputer, OA_PLAYER_STATUS_COMPUTER, 1},
        {kReceiver, OA_PLAYER_STATUS_MIRRORED, 2},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    const Seat receiver_view[4] = {
        {kSender, OA_PLAYER_STATUS_MIRRORED, 1},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 1},
        {kReceiver, OA_PLAYER_STATUS_LOCAL, 2},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    World* sender_world = seated_world(sender_view);
    World* receiver_world = seated_world(receiver_view);
    receiver_world->game.local_player_index = 2;
    MachineInbox inbox;
    inbox.local_ids = {kReceiver};
    NetConnection sender_connection{};
    NetConnection receiver_connection{};
    sender_connection.packets = new PacketLayer();
    receiver_connection.packets = new PacketLayer();
    packet_layer_create(sender_connection.packets);
    packet_layer_start(sender_connection.packets, NetTransport{&inbox, machine_send, nullptr});
    packet_layer_create(receiver_connection.packets);
    packet_layer_start(receiver_connection.packets, NetTransport{&inbox, nullptr, machine_receive});
    auto sender = std::make_unique<NetMatch>();
    auto receiver = std::make_unique<NetMatch>();
    net_match_begin(
        sender.get(), &sender_connection, sender_world, ReplicationSim{}, NetMatchHooks{}
    );
    net_match_begin(
        receiver.get(), &receiver_connection, receiver_world, ReplicationSim{}, NetMatchHooks{}
    );
    const auto progress = [](World* world, int slot) -> uint8_t& {
        return world->game.players[slot].load_progress;
    };

    // (100 + 100 + 100 + 100 + 70 + 0) / 6 = 78, on both local players only.
    const uint8_t loading[load_progress_rows] = {100, 100, 100, 100, 70, 0};
    net_match_send_load_progress(sender.get(), loading);
    packet_layer_flush(sender_connection.packets, 0, true);
    (void)net_match_pump(receiver.get());
    CHECK(progress(sender_world, 0) == 78 && progress(sender_world, 1) == 78);
    CHECK(progress(sender_world, 2) == 0 && progress(sender_world, 3) == 0);
    CHECK(progress(receiver_world, 0) == 78 && progress(receiver_world, 1) == 78);
    CHECK(receiver->record_errors == 0 && receiver->records_refused == 0);

    // Four active players: 0x26c / 4 = 155 each, bars 153 wide from x 11.
    LoadingScreenStatus status{};
    net_match_loading_screen_status(receiver.get(), &status);
    CHECK(status.bar_count == 4 && status.ready == 0);
    for (int32_t i = 0; i < status.bar_count; ++i) {
        CHECK(status.bars[i].player == &receiver_world->game.players[i]);
        CHECK(
            status.bars[i].left == 11 + 155 * i && status.bars[i].right == status.bars[i].left + 153
        );
    }
    CHECK(status.bars[0].filled == 11 + 119 && status.bars[1].filled == 166 + 119);
    CHECK(status.bars[2].filled == status.bars[2].left);
    CHECK(std::strcmp(status.text, "Waiting for other players.  0 players ready") == 0);
    // The line's texts are the game's own, in the language shown.
    {
        std::map<std::string, std::string> german{
            {"Waiting for other players", "Warte auf andere Spieler"},
            {"player ready", "Spieler bereit"},
            {"players ready", "Spieler bereit"},
            {"Synchronization complete", "Synchronisation abgeschlossen"}
        };
        NetMatchHooks hooks{};
        hooks.context = &german;
        hooks.translate_game_text = [](void* c, const char* text) -> const char* {
            const auto& words = *static_cast<std::map<std::string, std::string>*>(c);
            const auto found = words.find(text);
            return found != words.end() ? found->second.c_str() : nullptr;
        };
        const auto english = receiver->hooks;
        receiver->hooks = hooks;
        LoadingScreenStatus shown{};
        net_match_loading_screen_status(receiver.get(), &shown);
        CHECK(std::strcmp(shown.text, "Warte auf andere Spieler.  0 Spieler bereit") == 0);
        receiver_world->game.load_flags |= load_flag_barrier_passed;
        net_match_loading_screen_status(receiver.get(), &shown);
        CHECK(std::strcmp(shown.text, "Synchronisation abgeschlossen") == 0);
        receiver_world->game.load_flags &= ~load_flag_barrier_passed;
        receiver->hooks = english;
    }

    // At 100 only the slot whose "loaded" record arrived counts; a closed
    // slot takes no share.
    progress(receiver_world, 0) = load_progress_complete;
    progress(receiver_world, 1) = load_progress_complete;
    receiver->barrier.loaded[0] = 1;
    receiver_world->game.players[3].status = OA_PLAYER_STATUS_CLOSED;
    net_match_loading_screen_status(receiver.get(), &status);
    CHECK(status.bar_count == 3 && status.ready == 1);
    CHECK(status.bars[1].left == 11 + 0x26c / 3 && status.bars[0].filled == status.bars[0].right);
    CHECK(std::strcmp(status.text, "Waiting for other players.  1 player ready") == 0);
    receiver->barrier.loaded[1] = 1;
    net_match_loading_screen_status(receiver.get(), &status);
    CHECK(
        status.ready == 2 &&
        std::strcmp(status.text, "Waiting for other players.  2 players ready") == 0
    );

    receiver_world->game.load_flags |= load_flag_barrier_passed;
    net_match_loading_screen_status(receiver.get(), &status);
    CHECK(status.bar_count == 0 && std::strcmp(status.text, "Synchronization complete") == 0);

    delete sender_connection.packets;
    delete receiver_connection.packets;
    world_destroy(sender_world);
    world_destroy(receiver_world);
}

// ---- one machine's records, decoded ----

// Datagrams between two machines: each send lands in the peer's queue and
// is kept for the test to read back.
struct Link {
    std::deque<Datagram> queue;
    Link* peer{};
    std::vector<Datagram> sent;
};

uint32_t
link_send(void* context, uint32_t from, uint32_t to, uint32_t, const uint8_t* data, uint32_t size) {
    auto* link = static_cast<Link*>(context);
    Datagram datagram{from, to, std::vector<uint8_t>(data, data + size)};
    link->sent.push_back(datagram);
    if (link->peer != nullptr)
        link->peer->queue.push_back(std::move(datagram));
    return transport_result::ok;
}

uint32_t
link_receive(void* context, uint32_t* from, uint32_t* to, uint8_t* buffer, uint32_t* size) {
    auto* link = static_cast<Link*>(context);
    if (link->queue.empty())
        return transport_result::no_messages;
    const auto& d = link->queue.front();
    if (d.bytes.size() > *size) {
        *size = static_cast<uint32_t>(d.bytes.size());
        return transport_result::buffer_too_small;
    }
    std::memcpy(buffer, d.bytes.data(), d.bytes.size());
    *size = static_cast<uint32_t>(d.bytes.size());
    *from = d.from;
    *to = d.to;
    link->queue.pop_front();
    return transport_result::ok;
}

// One record a machine sent, with its datagram's addresses.
struct SentRecord {
    uint32_t from{};
    uint32_t to{};
    std::vector<uint8_t> bytes; // type byte first
};

// The records of one type a machine sent, unwrapped from their frames.
std::vector<SentRecord> records_sent(const Link& link, RecordType type) {
    std::vector<SentRecord> out;
    for (const auto& datagram : link.sent) {
        const auto frame = oa::netgame::network::decode_frame(datagram.bytes);
        for (std::size_t at = frame_header_bytes; at < frame.size();) {
            uint16_t length = 0;
            if (record_wire_length(frame.data() + at, frame.size() - at, &length) !=
                    WireError::ok ||
                length == 0)
                break;
            if (frame[at] == static_cast<uint8_t>(type))
                out.push_back(
                    {datagram.from,
                     datagram.to,
                     std::vector<uint8_t>(frame.begin() + at, frame.begin() + at + length)}
                );
            at += length;
        }
    }
    return out;
}

// Every record a machine sent, in order, unwrapped from their frames.
std::vector<SentRecord> all_records_sent(const Link& link) {
    std::vector<SentRecord> out;
    for (const auto& datagram : link.sent) {
        const auto frame = oa::netgame::network::decode_frame(datagram.bytes);
        for (std::size_t at = frame_header_bytes; at < frame.size();) {
            uint16_t length = 0;
            if (record_wire_length(frame.data() + at, frame.size() - at, &length) !=
                    WireError::ok ||
                length == 0)
                break;
            out.push_back(
                {datagram.from,
                 datagram.to,
                 std::vector<uint8_t>(frame.begin() + at, frame.begin() + at + length)}
            );
            at += length;
        }
    }
    return out;
}

// A record's bytes: its type, then the listed fields little-endian.
std::vector<uint8_t> record_bytes(RecordType type, std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> out{static_cast<uint8_t>(type)};
    for (const auto word : words)
        for (int shift = 0; shift < 32; shift += 8)
            out.push_back(static_cast<uint8_t>(word >> shift));
    return out;
}

// A machine seating four players over a Link, in the game, with its match
// hooks recorded.
struct SeatedMachine {
    World* world{};
    NetConnection connection{};
    Link link;
    std::unique_ptr<NetMatch> match = std::make_unique<NetMatch>();
    std::vector<uint8_t> destroyed; // slots whose units destroy_player_units destroyed
    uint32_t ended{};               // end_local_game calls
    bool won{};                     // what local_player_won answers
    bool debit_covers{true};        // what debit answers
    std::vector<std::pair<uint8_t, float>> credits;
    std::vector<std::pair<uint8_t, float>> debits;

    std::vector<std::string> chats;   // chat lines the chat hook showed
    std::vector<std::string> notices; // lines the notice hook showed

    // Chat lines the seat_chat hook heard: sender, addressee, broadcast, text.
    struct SeatLine {
        uint8_t sender{};
        uint8_t addressee{};
        bool broadcast{};
        std::string text;
    };

    std::vector<SeatLine> seat_lines;
    // What the translate hook gives, by the game's English; a text not here
    // has no translation.
    std::map<std::string, std::string> translations;
    // What the record_seen hook saw, sender id and type byte.
    std::vector<std::pair<uint32_t, uint8_t>> seen;
    // The text of each chat record the record_seen hook saw.
    std::vector<std::string> seen_chat;
    // What the speed_lock_changed hook heard: locked, slowest, fastest.
    std::vector<std::array<int32_t, 3>> speed_locks;
    // Whiteboard batches the whiteboard_marks hook showed, by sender slot.
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> marks;

    SeatedMachine(
        const Seat (&seats)[4],
        uint8_t local_slot,
        const WireRules& rules = {},
        uint32_t unit_slots = 4 * OA_PLAYER_COUNT + 1
    ) {
        world = seated_world(seats, unit_slots);
        world->game.local_player_index = local_slot;
        connection.packets = new PacketLayer();
        packet_layer_create(connection.packets);
        packet_layer_start(connection.packets, NetTransport{&link, link_send, link_receive});
        net_connection_set_rules(&connection, rules);
        NetMatchHooks hooks{};
        hooks.context = this;
        hooks.chat = [](void* c, uint8_t, const char* text) {
            static_cast<SeatedMachine*>(c)->chats.emplace_back(text);
        };
        hooks.seat_chat =
            [](void* c, uint8_t sender, uint8_t addressee, bool broadcast, const char* text) {
                static_cast<SeatedMachine*>(c)->seat_lines.push_back(
                    {sender, addressee, broadcast, text != nullptr ? text : ""}
                );
            };
        hooks.notice = [](void* c, const char* text) {
            static_cast<SeatedMachine*>(c)->notices.emplace_back(text);
        };
        hooks.translate_game_text = [](void* c, const char* text) -> const char* {
            const auto& translations = static_cast<SeatedMachine*>(c)->translations;
            const auto found = translations.find(text);
            return found != translations.end() ? found->second.c_str() : nullptr;
        };
        hooks.record_seen = [](void* c, uint32_t sender, const uint8_t* record, std::size_t size) {
            auto* self = static_cast<SeatedMachine*>(c);
            self->seen.emplace_back(sender, record[0]);
            if (record[0] == static_cast<uint8_t>(RecordType::chat) && size > 1) {
                const auto* text = reinterpret_cast<const char*>(record + 1);
                self->seen_chat.emplace_back(text, ::strnlen(text, size - 1));
            }
        };
        hooks.destroy_player_units = [](void* c, World*, uint8_t slot) {
            static_cast<SeatedMachine*>(c)->destroyed.push_back(slot);
        };
        hooks.speed_lock_changed = [](void* c, bool locked, uint8_t slowest, uint8_t fastest) {
            static_cast<SeatedMachine*>(c)->speed_locks.push_back(
                {locked ? 1 : 0, slowest, fastest}
            );
        };
        hooks.whiteboard_marks = [](void* c, uint8_t slot, const uint8_t* batch, std::size_t size) {
            static_cast<SeatedMachine*>(c)->marks.emplace_back(
                slot, std::vector<uint8_t>(batch, batch + size)
            );
        };
        hooks.end_local_game = [](void* c) { ++static_cast<SeatedMachine*>(c)->ended; };
        hooks.local_player_won = [](void* c) { return static_cast<SeatedMachine*>(c)->won; };
        hooks.credit = [](void* c, World*, uint8_t to, bool, float amount) {
            static_cast<SeatedMachine*>(c)->credits.emplace_back(to, amount);
        };
        hooks.debit = [](void* c, World*, uint8_t from, bool, float amount) {
            auto* self = static_cast<SeatedMachine*>(c);
            self->debits.emplace_back(from, amount);
            return self->debit_covers;
        };
        net_match_begin(match.get(), &connection, world, ReplicationSim{}, hooks);
        net_match_enter_game(match.get());
    }

    ~SeatedMachine() {
        delete connection.packets;
        world_destroy(world);
    }

    Player& player(uint8_t slot) { return world->game.players[slot]; }

    PlayerSetupInfo& info(uint8_t slot) { return world->player_info[slot]; }

    // Flushes every channel and returns the records of one type sent so far.
    std::vector<SentRecord> sent(RecordType type) {
        packet_layer_flush(connection.packets, 0, true);
        return records_sent(link, type);
    }

    void forget_sent() {
        packet_layer_flush(connection.packets, 0, true);
        link.sent.clear();
    }
};

// Joins two seated machines: each one's sends reach the other's pump.
void join(SeatedMachine& a, SeatedMachine& b) {
    a.link.peer = &b.link;
    b.link.peer = &a.link;
}

// Sends a record from one machine and pumps it on the other.
template <class R>
void deliver(
    SeatedMachine& from, SeatedMachine& to, uint32_t from_id, uint32_t to_id, const R& record
) {
    uint8_t bytes[80];
    std::size_t written = 0;
    CHECK(encode_record(record, bytes, sizeof bytes, &written) == WireError::ok);
    CHECK(net_match_send(from.match.get(), from_id, to_id, bytes, written));
    packet_layer_flush(from.connection.packets, 0, true);
    (void)net_match_pump(to.match.get());
}

// Only the Pause key pauses a network game: it goes out as {0x19, 0, bit}
// to every player from the first player on this machine (here its computer
// player), and sets or clears the other machine's pause bit.
void pause_goes_out_from_the_primary() {
    start_case("pause_goes_out_from_the_primary");
    constexpr uint32_t kComputer = 6, kHuman = 7, kOther = 9;
    const Seat a_view[4] = {
        {kComputer, OA_PLAYER_STATUS_COMPUTER, 1},
        {kHuman, OA_PLAYER_STATUS_LOCAL, 1},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 2},
        {}
    };
    const Seat b_view[4] = {
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 1},
        {kHuman, OA_PLAYER_STATUS_MIRRORED, 1},
        {kOther, OA_PLAYER_STATUS_LOCAL, 2},
        {}
    };
    SeatedMachine a(a_view, 1);
    SeatedMachine b(b_view, 2);
    join(a, b);
    net_match_set_pause(a.match.get(), true);
    auto pauses = a.sent(RecordType::pause_speed);
    CHECK(pauses.size() == 1);
    if (pauses.size() == 1) {
        CHECK(pauses[0].from == kComputer && pauses[0].to == broadcast_destination_id);
        CHECK((pauses[0].bytes == std::vector<uint8_t>{0x19, 0, 1}));
    }
    (void)net_match_pump(b.match.get());
    CHECK((b.world->game.sim_run_flags & run_flag_paused) != 0);
    net_match_set_pause(a.match.get(), false);
    pauses = a.sent(RecordType::pause_speed);
    CHECK(pauses.size() == 2 && (pauses.back().bytes == std::vector<uint8_t>{0x19, 0, 0}));
    (void)net_match_pump(b.match.get());
    CHECK((b.world->game.sim_run_flags & run_flag_paused) == 0);
    CHECK(b.match->record_errors == 0 && b.match->records_refused == 0);
}

// With all ten unicast channels taken, a record to an eleventh player is
// refused while every destination still names a player; once one of them
// has left, its channel is started again for the new destination and the
// record arrives there.
void channel_reclaimed_from_a_departed_player() {
    start_case("channel_reclaimed_from_a_departed_player");
    Link link;
    auto layer = std::make_unique<PacketLayer>();
    packet_layer_create(layer.get());
    packet_layer_start(layer.get(), NetTransport{&link, link_send, nullptr});
    auto game = std::make_unique<Game>();
    layer->receiver.players = game->players;
    const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
    for (uint32_t id = 101; id <= 110; ++id) {
        game->players[id - 101].player_id = id;
        CHECK(packet_layer_send(layer.get(), 7, id, &probe, 1, 100) == WireError::ok);
    }
    CHECK(packet_layer_send(layer.get(), 7, 111, &probe, 1, 100) == WireError::overflow);
    game->players[4].player_id = no_player_id; // 105 left
    CHECK(packet_layer_send(layer.get(), 7, 111, &probe, 1, 100) == WireError::ok);
    packet_layer_flush(layer.get(), 100, true);
    std::vector<uint32_t> destinations;
    for (const auto& datagram : link.sent)
        destinations.push_back(datagram.to);
    CHECK(std::count(destinations.begin(), destinations.end(), 111u) == 1);
    CHECK(std::count(destinations.begin(), destinations.end(), 105u) == 0);
    CHECK(destinations.size() == 10);
    // Without a bound player table nothing is reclaimed.
    layer->receiver.players = nullptr;
    CHECK(packet_layer_send(layer.get(), 7, 112, &probe, 1, 100) == WireError::overflow);
}

// Chat follows Game.chat_mode: everyone; allies and enemies by the local
// player's alliance row, among the players simulated elsewhere; the chosen
// players; nobody for this machine only. A line starting with '+' goes to
// everyone in any mode. Each goes out from the first local human player.
void chat_follows_the_chat_mode() {
    start_case("chat_follows_the_chat_mode");
    constexpr uint32_t kComputer = 6, kLocal = 7, kAlly = 9, kEnemy = 10, kOther = 11;
    constexpr uint8_t kChatModeLocalOnly = 4; // Game.chat_mode: this machine only
    const Seat view[4] = {
        {kComputer, OA_PLAYER_STATUS_COMPUTER, 1},
        {kAlly, OA_PLAYER_STATUS_MIRRORED, 2},
        {kEnemy, OA_PLAYER_STATUS_MIRRORED, 3},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 4}
    };
    SeatedMachine m(view, 0);
    // A local human after the computer player: the lines come from it.
    auto& local = m.player(4);
    local.in_use = 1;
    local.status = OA_PLAYER_STATUS_LOCAL;
    local.player_id = kLocal;
    m.world->game.local_player_index = 4;
    local.alliance[1] = 1;
    auto& game = m.world->game;
    const auto say = [&](uint8_t mode, const char* text) {
        m.forget_sent();
        game.chat_mode = mode;
        net_match_say(m.match.get(), text);
        std::vector<uint32_t> to;
        for (const auto& record : m.sent(RecordType::chat)) {
            CHECK(record.from == kLocal && record.bytes.size() == 65);
            CHECK(std::memcmp(record.bytes.data() + 1, text, std::strlen(text)) == 0);
            to.push_back(record.to);
        }
        return to;
    };
    CHECK(
        (say(OA_CHAT_MODE_EVERYONE, "<me> all") == std::vector<uint32_t>{broadcast_destination_id})
    );
    CHECK((say(OA_CHAT_MODE_ALLIES, "<me> allies") == std::vector<uint32_t>{kAlly}));
    CHECK((say(OA_CHAT_MODE_ENEMIES, "<me> enemies") == std::vector<uint32_t>{kEnemy, kOther}));
    game.chat_targets[3] = 1;
    CHECK((say(OA_CHAT_MODE_CHOSEN, "<me> one") == std::vector<uint32_t>{kOther}));
    game.chat_targets[3] = 0;
    CHECK(say(kChatModeLocalOnly, "<me> here").empty());
    CHECK(
        (say(OA_CHAT_MODE_ALLIES, "+SetShareMetal 5") ==
         std::vector<uint32_t>{broadcast_destination_id})
    );
    // The record carries the line's first 64 bytes, zero after a shorter
    // line and with no terminator when the line fills the field.
    const std::string long_line(70, 'x');
    for (const std::string& line : {std::string("<me> short"), long_line}) {
        m.forget_sent();
        game.chat_mode = OA_CHAT_MODE_EVERYONE;
        net_match_say(m.match.get(), line.c_str());
        const auto sent = m.sent(RecordType::chat);
        CHECK(sent.size() == 1 && sent[0].bytes.size() == 65);
        if (sent.size() != 1 || sent[0].bytes.size() != 65)
            continue;
        const std::size_t kept = std::min<std::size_t>(line.size(), 64);
        CHECK(std::memcmp(sent[0].bytes.data() + 1, line.data(), kept) == 0);
        CHECK(
            std::all_of(
                sent[0].bytes.begin() + 1 + static_cast<std::ptrdiff_t>(kept),
                sent[0].bytes.end(),
                [](uint8_t byte) { return byte == 0; }
            )
        );
    }
}

// A chat record addressed to a computer seat of this machine reaches seat_chat
// and not the human chat hook. A broadcast reaches both. A private-channel
// record reaches neither.
void seat_chat_reaches_seats() {
    start_case("seat_chat_reaches_seats");
    constexpr uint32_t kSender = 7, kHuman = 9, kComputer = 10;
    WireRules rules{};
    rules.private_channel = PrivateChannel::sub_id_dispatch;
    const Seat sender_view[4] = {
        {kSender, OA_PLAYER_STATUS_LOCAL, 1},
        {kHuman, OA_PLAYER_STATUS_MIRRORED, 2},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 2},
        {}
    };
    const Seat receiver_view[4] = {
        {kSender, OA_PLAYER_STATUS_MIRRORED, 1},
        {kHuman, OA_PLAYER_STATUS_LOCAL, 2},
        {kComputer, OA_PLAYER_STATUS_COMPUTER, 2},
        {}
    };
    SeatedMachine sender(sender_view, 0, rules);
    SeatedMachine receiver(receiver_view, 1, rules);
    join(sender, receiver);

    ChatRecord line{};
    std::snprintf(line.text, sizeof line.text, "%s", "<Ann> to the seat");
    deliver(sender, receiver, kSender, kComputer, line);
    CHECK(receiver.chats.empty());
    CHECK(receiver.seat_lines.size() == 1);
    if (receiver.seat_lines.size() == 1) {
        CHECK(receiver.seat_lines[0].sender == 0);
        CHECK(receiver.seat_lines[0].addressee == 2);
        CHECK(!receiver.seat_lines[0].broadcast);
        CHECK(receiver.seat_lines[0].text == "<Ann> to the seat");
    }

    receiver.chats.clear();
    receiver.seat_lines.clear();
    std::snprintf(line.text, sizeof line.text, "%s", "<Ann> everyone");
    deliver(sender, receiver, kSender, broadcast_destination_id, line);
    CHECK(receiver.chats.size() == 1 && receiver.chats[0] == "<Ann> everyone");
    CHECK(receiver.seat_lines.size() == 1);
    if (receiver.seat_lines.size() == 1) {
        CHECK(receiver.seat_lines[0].sender == 0);
        CHECK(receiver.seat_lines[0].addressee == 1);
        CHECK(receiver.seat_lines[0].broadcast);
        CHECK(receiver.seat_lines[0].text == "<Ann> everyone");
    }

    receiver.chats.clear();
    receiver.seat_lines.clear();
    uint8_t raw[private_record_bytes]{};
    raw[0] = static_cast<uint8_t>(RecordType::chat);
    CHECK(net_match_send(sender.match.get(), kSender, kComputer, raw, sizeof raw));
    packet_layer_flush(sender.connection.packets, 0, true);
    (void)net_match_pump(receiver.match.get());
    CHECK(receiver.chats.empty());
    CHECK(receiver.seat_lines.empty());
    CHECK(receiver.match->private_records >= 1);
}

// A computer seat sends from its own player id, routed by its own alliance
// row, which here differs from the human's. A command, a mirrored or human
// seat, and a seat without an id send nothing, and said_parts stays put.
void seats_say_from_their_own_id() {
    start_case("seats_say_from_their_own_id");
    constexpr uint32_t kComputer = 6, kLocal = 7, kAlly = 9, kEnemy = 10, kOther = 11;
    const Seat view[4] = {
        {kComputer, OA_PLAYER_STATUS_COMPUTER, 1},
        {kAlly, OA_PLAYER_STATUS_MIRRORED, 2},
        {kEnemy, OA_PLAYER_STATUS_MIRRORED, 3},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 4}
    };
    SeatedMachine machine(view, 0);
    auto& local = machine.player(4);
    local.in_use = 1;
    local.status = OA_PLAYER_STATUS_LOCAL;
    local.player_id = kLocal;
    machine.world->game.local_player_index = 4;
    local.alliance[1] = 1;
    auto& seat = machine.player(0);
    seat.alliance[2] = 1;

    const auto destinations = [&](uint8_t mode, const char* text, const uint8_t* targets) {
        machine.forget_sent();
        machine.match->said_parts = {"kept"};
        net_match_say_from(machine.match.get(), 0, text, mode, targets);
        std::vector<uint32_t> to;
        for (const auto& record : machine.sent(RecordType::chat)) {
            CHECK(record.from == kComputer && record.bytes.size() == 65);
            if (record.bytes.size() == 65)
                CHECK(std::memcmp(record.bytes.data() + 1, text, std::strlen(text)) == 0);
            to.push_back(record.to);
        }
        CHECK((machine.match->said_parts == std::vector<std::string>{"kept"}));
        return to;
    };
    CHECK(
        (destinations(OA_CHAT_MODE_EVERYONE, "<Comp> hi", nullptr) ==
         std::vector<uint32_t>{broadcast_destination_id})
    );
    CHECK((
        destinations(OA_CHAT_MODE_ALLIES, "<Comp> allies", nullptr) == std::vector<uint32_t>{kEnemy}
    ));
    machine.forget_sent();
    machine.world->game.chat_mode = OA_CHAT_MODE_ALLIES;
    net_match_say(machine.match.get(), "<me> allies");
    {
        const auto sent = machine.sent(RecordType::chat);
        CHECK(sent.size() == 1 && sent[0].from == kLocal && sent[0].to == kAlly);
    }
    CHECK(
        (destinations(OA_CHAT_MODE_ENEMIES, "<Comp> enemies", nullptr) ==
         std::vector<uint32_t>{kAlly, kOther})
    );
    uint8_t targets[OA_PLAYER_COUNT]{};
    targets[3] = 1;
    machine.world->game.chat_targets[1] = 1;
    CHECK(
        (destinations(OA_CHAT_MODE_CHOSEN, "<Comp> one", targets) == std::vector<uint32_t>{kOther})
    );
    CHECK(destinations(OA_CHAT_MODE_EVERYONE, "+SetShareMetal 5", nullptr).empty());
    CHECK(destinations(OA_CHAT_MODE_EVERYONE, "<Comp> .take", nullptr).empty());

    const auto sends_nothing = [&](uint8_t slot, const char* text) {
        machine.forget_sent();
        machine.match->said_parts = {"kept"};
        net_match_say_from(machine.match.get(), slot, text, OA_CHAT_MODE_EVERYONE, nullptr);
        CHECK(machine.sent(RecordType::chat).empty());
        CHECK((machine.match->said_parts == std::vector<std::string>{"kept"}));
    };
    sends_nothing(1, "<x> hi");
    sends_nothing(4, "<x> hi");
    seat.in_use = 0;
    sends_nothing(0, "<x> hi");
    seat.in_use = 1;
    seat.player_id = no_player_id;
    sends_nothing(0, "<x> hi");
    seat.player_id = 0;
    sends_nothing(0, "<x> hi");
    seat.player_id = kComputer;
    machine.match->unicode_chat = true;
    CHECK(
        (destinations(OA_CHAT_MODE_EVERYONE, "<Comp> hi", nullptr) ==
         std::vector<uint32_t>{broadcast_destination_id})
    );
    machine.match->unicode_chat = false;
}

// Every 60 ticks a third of the metal above the SetShareMetal threshold and
// half of the energy above the SetShareEnergy threshold go to a player the
// local player allies with that is a human still playing on another
// machine, capped at its free storage; every 450 ticks sight goes to each
// such player. A computer player, a player we do not ally with (even one
// allied with us) and a defeated player get nothing.
void automatic_sharing_follows_the_thresholds() {
    start_case("automatic_sharing_follows_the_thresholds");
    constexpr uint32_t kLocal = 7, kHuman = 9, kComputer = 10, kAllyOfOurs = 11;
    const Seat view[4] = {
        {kLocal, OA_PLAYER_STATUS_LOCAL, 1},
        {kHuman, OA_PLAYER_STATUS_MIRRORED, 2},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 2},
        {kAllyOfOurs, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    SeatedMachine m(view, 0);
    auto& self = m.player(0);
    m.info(0).role = 0x02 | 0x04 | 0x20; // share metal, energy and sight
    m.info(1).state = OA_PLAYER_STATUS_LOCAL;
    m.info(2).state = OA_PLAYER_STATUS_COMPUTER;
    m.info(3).state = OA_PLAYER_STATUS_LOCAL;
    self.alliance[1] = 1;
    self.alliance[2] = 1;
    self.allied_by[3] = 1; // player 3 allies with us, we do not ally with it
    for (uint8_t slot = 0; slot < 4; ++slot) {
        auto& p = m.player(slot);
        p.metal = 100.0F;
        p.energy = 100.0F;
        p.metal_storage = 1000.0F;
        p.energy_storage = 1000.0F;
    }
    self.metal = 800.0F;
    self.energy = 900.0F;
    self.metal_share_threshold = 500.0F;
    self.energy_share_threshold = 300.0F;
    self.shared_metal_storage = 2000.0F; // not what the share compares with
    self.shared_energy_storage = 2000.0F;
    auto& game = m.world->game;
    const auto gives = [&] {
        std::vector<std::pair<uint32_t, uint32_t>> out; // {subtype, to}
        for (const auto& record : m.sent(RecordType::resource_give)) {
            ResourceGiveRecord give{};
            CHECK(decode_record(record.bytes.data(), record.bytes.size(), &give) == WireError::ok);
            CHECK(record.from == kLocal && give.from_id == kLocal && record.to == give.to_id);
            out.emplace_back(give.subtype, give.to_id);
        }
        return out;
    };
    game.tick = 60;
    net_match_after_tick(m.match.get());
    auto sent = gives();
    CHECK((sent == std::vector<std::pair<uint32_t, uint32_t>>{{2, kHuman}, {1, kHuman}}));
    CHECK(m.debits.size() == 2 && m.debits[0].second == 100.0F && m.debits[1].second == 300.0F);
    CHECK(m.credits.size() == 2 && m.credits[0].first == 1 && m.credits[0].second == 100.0F);
    // At or below the thresholds nothing goes.
    m.forget_sent();
    self.metal = 500.0F;
    self.energy = 300.0F;
    net_match_after_tick(m.match.get());
    CHECK(gives().empty());
    // The default thresholds (0) share everything above none.
    self.metal_share_threshold = 0.0F;
    self.energy_share_threshold = 0.0F;
    net_match_after_tick(m.match.get());
    CHECK(gives().size() == 2);
    // A defeated player takes no part.
    m.forget_sent();
    m.player(1).units_created = 3;
    net_match_after_tick(m.match.get());
    CHECK(gives().empty());
    m.player(1).units_created = 0;
    // Sight goes to the player we ally with, not to the one allied with us,
    // nor to a computer player.
    m.forget_sent();
    game.tick = 450;
    net_match_after_tick(m.match.get());
    CHECK((gives() == std::vector<std::pair<uint32_t, uint32_t>>{{3, kHuman}}));
}

// The resources go to the last such player in slot order whose store is
// below the local player's, as 3.1c picks it, not to the poorest.
void automatic_sharing_picks_the_last_poorer_player() {
    start_case("automatic_sharing_picks_the_last_poorer_player");
    constexpr uint32_t kLocal = 7, kPoorest = 9, kPoorer = 10, kRicher = 11;
    const Seat view[4] = {
        {kLocal, OA_PLAYER_STATUS_LOCAL, 1},
        {kPoorest, OA_PLAYER_STATUS_MIRRORED, 2},
        {kPoorer, OA_PLAYER_STATUS_MIRRORED, 3},
        {kRicher, OA_PLAYER_STATUS_MIRRORED, 4}
    };
    SeatedMachine m(view, 0);
    m.info(0).role = 0x02;
    for (uint8_t slot = 1; slot < 4; ++slot) {
        m.info(slot).state = OA_PLAYER_STATUS_LOCAL;
        m.player(0).alliance[slot] = 1;
        m.player(slot).metal_storage = 1000.0F;
    }
    m.player(0).metal = 300.0F;
    m.player(1).metal = 10.0F;
    m.player(2).metal = 200.0F;
    m.player(3).metal = 400.0F;
    m.world->game.tick = 60;
    net_match_after_tick(m.match.get());
    const auto sent = m.sent(RecordType::resource_give);
    CHECK(sent.size() == 1 && sent[0].to == kPoorer);
}

// A give is capped at the giver's store, debited (a debit the store cannot
// cover is ignored) and credited, and goes out as 0x16 when both players
// still take part; a defeated receiver is credited but told nothing.
void give_goes_out_when_both_take_part() {
    start_case("give_goes_out_when_both_take_part");
    constexpr uint32_t kLocal = 7, kOther = 9;
    const Seat view[4] = {
        {kLocal, OA_PLAYER_STATUS_LOCAL, 1}, {kOther, OA_PLAYER_STATUS_MIRRORED, 2}, {}, {}
    };
    SeatedMachine m(view, 0);
    m.player(0).metal = 50.0F;
    m.debit_covers = false;
    net_match_give(m.match.get(), 0, 1, true, 80.0F);
    auto sent = m.sent(RecordType::resource_give);
    CHECK(sent.size() == 1);
    if (sent.size() == 1) {
        CHECK(sent[0].from == kLocal && sent[0].to == kOther);
        const std::vector<uint8_t> expected{
            0x16, 2, 0, 0, 0, 7, 0, 0, 0, 9, 0, 0, 0, 0x00, 0x00, 0x48, 0x42
        };
        CHECK(sent[0].bytes == expected); // 50.0f
    }
    CHECK(m.debits.size() == 1 && m.credits.size() == 1 && m.credits[0].second == 50.0F);
    m.forget_sent();
    m.player(1).units_created = 2; // no units left: defeated
    net_match_give(m.match.get(), 0, 1, false, 10.0F);
    CHECK(m.sent(RecordType::resource_give).empty() && m.credits.size() == 1);
    m.player(0).energy = 10.0F;
    net_match_give(m.match.get(), 0, 1, false, 10.0F);
    CHECK(m.sent(RecordType::resource_give).empty() && m.credits.size() == 2);
}

// The periodic economy (0x28, every fourth 30-tick period) goes only to the
// humans still playing on other machines, not to a computer player there.
void periodic_economy_goes_to_humans_still_playing() {
    start_case("periodic_economy_goes_to_humans_still_playing");
    constexpr uint32_t kLocal = 7, kHuman = 9, kComputer = 10, kWatcher = 11;
    const Seat view[4] = {
        {kLocal, OA_PLAYER_STATUS_LOCAL, 1},
        {kHuman, OA_PLAYER_STATUS_MIRRORED, 2},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 2},
        {kWatcher, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    SeatedMachine m(view, 0);
    m.info(1).state = OA_PLAYER_STATUS_LOCAL;
    m.info(2).state = OA_PLAYER_STATUS_COMPUTER;
    m.info(3).state = 0; // no longer playing
    for (int i = 0; i < 4; ++i)
        net_match_economy_period(m.match.get());
    const auto sent = m.sent(RecordType::economy);
    CHECK(sent.size() == 1 && sent[0].from == kLocal && sent[0].to == kHuman);
    CHECK(sent.size() == 1 && sent[0].bytes.size() == 0x3a && sent[0].bytes[1] == 0);
}

// A 0x28 that asks for a reply gets the 0x29 answer and, until the local
// player has won, the local player's own economy back.
void a_winner_answers_but_sends_no_economy() {
    start_case("a_winner_answers_but_sends_no_economy");
    constexpr uint32_t kSender = 7, kLocal = 9;
    const Seat a_view[4] = {
        {kSender, OA_PLAYER_STATUS_LOCAL, 1}, {kLocal, OA_PLAYER_STATUS_MIRRORED, 2}, {}, {}
    };
    const Seat b_view[4] = {
        {kSender, OA_PLAYER_STATUS_MIRRORED, 1}, {kLocal, OA_PLAYER_STATUS_LOCAL, 2}, {}, {}
    };
    for (const bool won : {false, true}) {
        SeatedMachine a(a_view, 0);
        SeatedMachine b(b_view, 1);
        join(a, b);
        b.won = won;
        EconomyRecord asking{};
        asking.want_reply = 1;
        deliver(a, b, kSender, kLocal, asking);
        const auto replies = b.sent(RecordType::economy_reply);
        CHECK(replies.size() == 1 && replies[0].from == kLocal && replies[0].to == kSender);
        CHECK(replies.size() == 1 && (replies[0].bytes == std::vector<uint8_t>{0x29, 1, 0}));
        const auto own = b.sent(RecordType::economy);
        CHECK(own.size() == (won ? 0u : 1u));
        if (!own.empty())
            CHECK(own[0].from == kLocal && own[0].to == kSender && own[0].bytes[1] == 1);
    }
}

// Rejecting a player simulated elsewhere retires its whole machine only for
// a human still playing: every slot of its machine group, group 0 included,
// takes the reason. A computer player, or a human no longer playing, goes
// alone. Each departure destroys the player's units here, and the reject
// goes out as {0x1b, id, reason} from the first player on this machine.
void rejecting_retires_a_machine_only_for_a_human() {
    start_case("rejecting_retires_a_machine_only_for_a_human");
    constexpr uint32_t kLocal = 7, kHuman = 9, kComputer = 10, kOther = 11;
    const Seat view[4] = {
        {kLocal, OA_PLAYER_STATUS_LOCAL, 1},
        {kHuman, OA_PLAYER_STATUS_MIRRORED, 2},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 2},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    {
        SeatedMachine m(view, 0);
        m.info(1).state = OA_PLAYER_STATUS_LOCAL;
        m.info(2).state = OA_PLAYER_STATUS_COMPUTER;
        m.info(3).state = OA_PLAYER_STATUS_LOCAL;
        const Seat sender_view[4] = {
            {kLocal, OA_PLAYER_STATUS_MIRRORED, 1},
            {kHuman, OA_PLAYER_STATUS_MIRRORED, 2},
            {kComputer, OA_PLAYER_STATUS_MIRRORED, 2},
            {kOther, OA_PLAYER_STATUS_LOCAL, 3}
        };
        SeatedMachine other(sender_view, 3);
        join(other, m);
        RejectRecord reject{};
        reject.player_id = kComputer;
        reject.reason = 6;
        deliver(other, m, kOther, broadcast_destination_id, reject);
        CHECK(m.player(2).in_use == 0 && m.player(2).reject_reason == 6);
        CHECK(m.player(1).in_use != 0 && m.player(1).reject_reason == 0);
        CHECK((m.destroyed == std::vector<uint8_t>{2}));
        auto sent = m.sent(RecordType::reject);
        CHECK(sent.size() == 1 && sent[0].from == kLocal && sent[0].to == broadcast_destination_id);
        CHECK(sent.size() == 1 && (sent[0].bytes == std::vector<uint8_t>{0x1b, 10, 0, 0, 0, 6}));
        // The human takes its machine group with it.
        m.player(2) = Player{};
        m.player(2).index = 2;
        m.player(2).in_use = 1;
        m.player(2).status = OA_PLAYER_STATUS_MIRRORED;
        m.player(2).player_id = kComputer;
        m.player(2).machine_group = 2;
        m.player(2).info = oa_ref_from_index(2);
        m.destroyed.clear();
        reject.player_id = kHuman;
        reject.reason = 1;
        deliver(other, m, kOther, broadcast_destination_id, reject);
        CHECK(m.player(1).in_use == 0 && m.player(2).in_use == 0);
        CHECK(m.player(1).reject_reason == 1 && m.player(2).reject_reason == 1);
        CHECK(m.player(3).in_use != 0 && m.player(3).reject_reason == 0);
        CHECK((m.destroyed == std::vector<uint8_t>{1, 2}));
    }
    {
        // A human whose group is still 0 takes every slot of group 0.
        const Seat unset[4] = {
            {kLocal, OA_PLAYER_STATUS_LOCAL, 1},
            {kHuman, OA_PLAYER_STATUS_MIRRORED, 0},
            {kComputer, OA_PLAYER_STATUS_MIRRORED, 0},
            {kOther, OA_PLAYER_STATUS_MIRRORED, 3}
        };
        const Seat other_view[4] = {
            {kLocal, OA_PLAYER_STATUS_MIRRORED, 1},
            {kHuman, OA_PLAYER_STATUS_MIRRORED, 0},
            {kComputer, OA_PLAYER_STATUS_MIRRORED, 0},
            {kOther, OA_PLAYER_STATUS_LOCAL, 3}
        };
        SeatedMachine m(unset, 0);
        SeatedMachine other(other_view, 3);
        join(other, m);
        m.info(1).state = OA_PLAYER_STATUS_LOCAL;
        m.info(2).state = OA_PLAYER_STATUS_COMPUTER;
        RejectRecord reject{};
        reject.player_id = kHuman;
        reject.reason = 1;
        deliver(other, m, kOther, broadcast_destination_id, reject);
        CHECK(m.player(1).in_use == 0 && m.player(2).in_use == 0 && m.player(2).reject_reason == 1);
        CHECK(m.player(3).in_use != 0 && m.player(0).reject_reason == 0);
    }
}

// A system message image: the type, player type 1 and the player's id,
// then the fields and blocks given.
std::vector<uint8_t> system_image(SystemMessageType type, uint32_t id, std::size_t fixed_bytes) {
    std::vector<uint8_t> image(fixed_bytes);
    store_u32(image.data(), static_cast<uint32_t>(type));
    store_u32(image.data() + 4, dplay::system_message::player_type_player);
    store_u32(image.data() + 8, id);
    return image;
}

// Appends a terminated string to an image and stores its offset in a field.
void append_image_string(std::vector<uint8_t>& image, std::size_t field, const char* text) {
    store_u32(image.data() + field, static_cast<uint32_t>(image.size()));
    image.insert(image.end(), text, text + std::strlen(text) + 1);
}

// A player's name and data changes reach its slot: the new long name
// becomes its name and the short name its second name, and the data
// replaces its setup block from the start. When this machine's player is
// the game's host and its game does not allow watching, a player whose new
// block makes it a watcher is rejected with reason 9. This machine
// becoming the session's name server changes nothing here.
void player_name_and_data_changes_reach_the_slot() {
    start_case("player_name_and_data_changes_reach_the_slot");
    constexpr uint32_t kLocal = 7, kOther = 9;
    const Seat view[4] = {
        {kLocal, OA_PLAYER_STATUS_LOCAL, 1}, {kOther, OA_PLAYER_STATUS_MIRRORED, 2}, {}, {}
    };
    namespace sm = dplay::system_message;
    SeatedMachine m(view, 0);
    m.info(0).role = 0x01; // the game's host
    m.info(1).state = OA_PLAYER_STATUS_LOCAL;
    oa::base::text::copy_terminated(m.player(1).name, "before");
    oa::base::text::copy_terminated(m.player(1).second_name, "kept");

    auto name = system_image(SystemMessageType::player_name_changed, kOther, sm::player_name_bytes);
    store_u32(name.data() + sm::player_name_name, sm::name_bytes);
    append_image_string(
        name, sm::player_name_name + sm::name_long, "A long name of thirty-five letters"
    );
    m.link.queue.push_back({system_message_sender_id, kLocal, name});
    (void)net_match_pump(m.match.get());
    CHECK(std::strcmp(m.player(1).name, "A long name of thirty-five le") == 0);
    CHECK(std::strcmp(m.player(1).second_name, "kept") == 0);
    name = system_image(SystemMessageType::player_name_changed, kOther, sm::player_name_bytes);
    store_u32(name.data() + sm::player_name_name, sm::name_bytes);
    append_image_string(name, sm::player_name_name + sm::name_short, "shorty");
    append_image_string(name, sm::player_name_name + sm::name_long, "Longer");
    m.link.queue.push_back({system_message_sender_id, kLocal, name});
    (void)net_match_pump(m.match.get());
    CHECK(
        std::strcmp(m.player(1).name, "Longer") == 0 &&
        std::strcmp(m.player(1).second_name, "shorty") == 0
    );

    const auto data_image = [&](const uint8_t* block, std::size_t size) {
        auto image =
            system_image(SystemMessageType::player_data_changed, kOther, sm::player_data_bytes);
        store_u32(image.data() + sm::player_data_data, static_cast<uint32_t>(image.size()));
        store_u32(image.data() + sm::player_data_size, static_cast<uint32_t>(size));
        image.insert(image.end(), block, block + size);
        return image;
    };
    // Three bytes replace the start of the block, the map name.
    const uint8_t map[3] = {'X', 'Y', 0};
    m.link.queue.push_back({system_message_sender_id, kLocal, data_image(map, sizeof map)});
    (void)net_match_pump(m.match.get());
    CHECK(std::strcmp(reinterpret_cast<const char*>(&m.info(1)), "XY") == 0);
    CHECK(m.info(1).state == OA_PLAYER_STATUS_LOCAL);

    PlayerSetupInfo block = m.info(1);
    block.options = static_cast<uint16_t>(block.options | OA_SETUP_OPTION_WATCHER);
    block.side = 1;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&block);
    m.forget_sent();
    m.link.queue.push_back({system_message_sender_id, kLocal, data_image(bytes, sizeof block)});
    (void)net_match_pump(m.match.get());
    CHECK(m.info(1).side == 1 && (m.info(1).options & OA_SETUP_OPTION_WATCHER) != 0);
    auto sent = m.sent(RecordType::reject);
    CHECK(sent.size() == 1 && (sent[0].bytes == std::vector<uint8_t>{0x1b, kOther, 0, 0, 0, 9}));
    CHECK(m.player(1).reject_reason == 9);

    // With watching allowed, or on a machine whose player is not the host,
    // a watcher stays.
    SeatedMachine allowed(view, 0);
    allowed.info(0).role = 0x01;
    allowed.info(0).options = OA_SETUP_OPTION_WATCHING_ALLOWED;
    allowed.link.queue.push_back(
        {system_message_sender_id, kLocal, data_image(bytes, sizeof block)}
    );
    SeatedMachine guest(view, 0);
    guest.info(1).role = 0x01;
    guest.link.queue.push_back({system_message_sender_id, kLocal, data_image(bytes, sizeof block)});
    std::vector<uint8_t> host_changed(sm::host_bytes);
    store_u32(host_changed.data(), static_cast<uint32_t>(SystemMessageType::host_changed));
    guest.link.queue.push_back({system_message_sender_id, kLocal, host_changed});
    for (SeatedMachine* machine : {&allowed, &guest}) {
        (void)net_match_pump(machine->match.get());
        CHECK((machine->info(1).options & OA_SETUP_OPTION_WATCHER) != 0);
        CHECK(machine->sent(RecordType::reject).empty() && machine->player(1).reject_reason == 0);
    }
    CHECK(guest.match->record_errors == 0);
}

// A 0x1c notice naming this machine's first local human player is passed
// on and ends the local game; one naming another player is only passed on.
void a_disconnect_notice_naming_this_machine_ends_its_game() {
    start_case("a_disconnect_notice_naming_this_machine_ends_its_game");
    constexpr uint32_t kSender = 7, kLocal = 9, kThird = 11;
    const Seat a_view[4] = {
        {kSender, OA_PLAYER_STATUS_LOCAL, 1},
        {kLocal, OA_PLAYER_STATUS_MIRRORED, 2},
        {kThird, OA_PLAYER_STATUS_MIRRORED, 3},
        {}
    };
    const Seat b_view[4] = {
        {kSender, OA_PLAYER_STATUS_MIRRORED, 1},
        {kLocal, OA_PLAYER_STATUS_LOCAL, 2},
        {kThird, OA_PLAYER_STATUS_MIRRORED, 3},
        {}
    };
    SeatedMachine a(a_view, 0);
    SeatedMachine b(b_view, 1);
    join(a, b);
    // In a started game the local slot stays, so the notice can name it.
    b.world->game.session_flags |= kNetFlagGameStarted;
    DisconnectNoticeRecord notice{};
    notice.player_id = kThird;
    deliver(a, b, kSender, broadcast_destination_id, notice);
    auto relayed = b.sent(RecordType::disconnect_notice);
    CHECK(relayed.size() == 1 && relayed[0].from == kLocal && b.ended == 0);
    CHECK(relayed.size() == 1 && (relayed[0].bytes == std::vector<uint8_t>{0x1c, 11, 0, 0, 0}));
    notice.player_id = kLocal;
    deliver(a, b, kSender, broadcast_destination_id, notice);
    relayed = b.sent(RecordType::disconnect_notice);
    CHECK(relayed.size() == 2 && relayed[1].bytes[1] == kLocal && b.ended == 1);
}

// A ping is echoed from the player it was addressed to: a computer player
// answers for itself, the local human for a ping sent to all.
void ping_is_echoed_from_its_addressee() {
    start_case("ping_is_echoed_from_its_addressee");
    constexpr uint32_t kSender = 7, kLocal = 9, kComputer = 10;
    const Seat a_view[4] = {
        {kSender, OA_PLAYER_STATUS_LOCAL, 1},
        {kLocal, OA_PLAYER_STATUS_MIRRORED, 2},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 2},
        {}
    };
    const Seat b_view[4] = {
        {kSender, OA_PLAYER_STATUS_MIRRORED, 1},
        {kLocal, OA_PLAYER_STATUS_LOCAL, 2},
        {kComputer, OA_PLAYER_STATUS_COMPUTER, 2},
        {}
    };
    SeatedMachine a(a_view, 0);
    SeatedMachine b(b_view, 1);
    join(a, b);
    PingRecord ping{};
    ping.origin_tick_count = 1234;
    ping.origin_player_id = kSender;
    deliver(a, b, kSender, kComputer, ping);
    deliver(a, b, kSender, broadcast_destination_id, ping);
    const auto echoes = b.sent(RecordType::ping);
    CHECK(echoes.size() == 2);
    if (echoes.size() == 2) {
        CHECK(echoes[0].from == kComputer && echoes[0].to == kSender);
        CHECK(echoes[1].from == kLocal && echoes[1].to == kSender);
        PingRecord echo{};
        CHECK(
            decode_record(echoes[0].bytes.data(), echoes[0].bytes.size(), &echo) == WireError::ok
        );
        CHECK(echo.origin_tick_count == 1234 && echo.origin_player_id == kSender);
    }
    CHECK(!b.connection.packets->guaranteed);
}

constexpr uint8_t kHostRole = 0x01; // PlayerSetupInfo.role: the game's host

// Loading ends as the loader ends it: the local player's block gains the
// started option; each local and computer player's 0x20 and then its 0x24
// go to every player from that player; the players still in machine group
// 0 ask the host for theirs (0x21), from the first player on this machine,
// the computer player for its human's machine; and one economy period is
// counted on the connection. The other machine copies the blocks.
void loading_ends_with_status_and_group_requests() {
    start_case("loading_ends_with_status_and_group_requests");
    constexpr uint32_t kHuman = 7, kComputer = 8, kHost = 9, kOther = 11;
    const Seat a_view[4] = {
        {kHuman, OA_PLAYER_STATUS_LOCAL, 2},
        {kComputer, OA_PLAYER_STATUS_COMPUTER, 0},
        {kHost, OA_PLAYER_STATUS_MIRRORED, 1},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 0}
    };
    const Seat b_view[4] = {
        {kHuman, OA_PLAYER_STATUS_MIRRORED, 2},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 0},
        {kHost, OA_PLAYER_STATUS_LOCAL, 1},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 0}
    };
    SeatedMachine a(a_view, 0);
    SeatedMachine b(b_view, 2);
    join(a, b);
    a.info(2).role = kHostRole;
    b.info(2).role = kHostRole;
    a.player(1).team = 4;
    net_match_end_loading(a.match.get());
    CHECK((a.info(0).options & OA_SETUP_OPTION_STARTED) != 0);
    CHECK((a.info(1).options & OA_SETUP_OPTION_STARTED) == 0);
    CHECK(a.connection.economy_periods == 1);
    packet_layer_flush(a.connection.packets, 0, true);
    const auto records = all_records_sent(a.link);
    std::vector<uint8_t> types;
    for (const auto& record : records)
        types.push_back(record.bytes[0]);
    CHECK((types == std::vector<uint8_t>{0x20, 0x24, 0x20, 0x24, 0x21, 0x21}));
    if (types.size() == 6) {
        CHECK(records[0].from == kHuman && records[0].to == broadcast_destination_id);
        CHECK((records[1].bytes == std::vector<uint8_t>{0x24, kHuman, 0, 0, 0, 0}));
        CHECK(records[2].from == kComputer && records[3].from == kComputer);
        CHECK((records[3].bytes == std::vector<uint8_t>{0x24, kComputer, 0, 0, 0, 4}));
        // {0x21, assign, player, player sharing its machine}, to the host.
        CHECK(records[4].from == kHuman && records[4].to == kHost);
        CHECK((records[4].bytes == [] {
            auto bytes = record_bytes(RecordType::machine_group_request, {kComputer, kHuman});
            bytes.insert(bytes.begin() + 1, 1);
            return bytes;
        }()));
        CHECK(records[5].from == kHuman && records[5].to == kHost);
        CHECK((records[5].bytes == [] {
            auto bytes = record_bytes(RecordType::machine_group_request, {kOther, no_player_id});
            bytes.insert(bytes.begin() + 1, 0);
            return bytes;
        }()));
    }
    (void)net_match_pump(b.match.get());
    CHECK((b.info(0).options & OA_SETUP_OPTION_STARTED) != 0);
    CHECK((b.info(1).options & OA_SETUP_OPTION_STARTED) == 0);
    CHECK(b.player(1).team == 4);
    CHECK(b.match->record_errors == 0);

    // On the host's own machine its players still in group 0 take group 1,
    // and nothing is asked; the running game's status asks nothing.
    b.player(2).machine_group = 0;
    b.forget_sent();
    net_match_end_loading(b.match.get());
    CHECK(b.player(2).machine_group == 1);
    CHECK(b.sent(RecordType::machine_group_request).empty());
    a.forget_sent();
    net_match_send_player_status(a.match.get(), false);
    CHECK(
        a.sent(RecordType::player_info).size() == 2 && a.sent(RecordType::player_team).size() == 2
    );
    CHECK(a.sent(RecordType::machine_group_request).empty());
}

// The economy count lives on the connection and is never reset: the end of
// loading counts one period, so the first game's first 0x28 goes out at
// tick 90; a second game on the same connection goes on from the count the
// first left.
void economy_count_runs_across_games() {
    start_case("economy_count_runs_across_games");
    constexpr uint32_t kLocal = 7, kRemote = 9;
    const Seat view[4] = {
        {kLocal, OA_PLAYER_STATUS_LOCAL, 1}, {kRemote, OA_PLAYER_STATUS_MIRRORED, 2}, {}, {}
    };
    SeatedMachine a(view, 0);
    a.info(1).state = 1; // a human still playing
    a.world->game.viewpoint_player = 0;
    // Plays a game from the end of its loading to tick 120 and returns the
    // ticks its 0x28 records went out at.
    const auto play = [&] {
        std::vector<uint32_t> sent_at;
        a.forget_sent();
        net_match_end_loading(a.match.get());
        if (!a.sent(RecordType::economy).empty())
            sent_at.push_back(0);
        for (uint32_t tick = 1; tick <= 120; ++tick) {
            a.world->game.tick = tick;
            a.forget_sent();
            if (tick % 30 == 0)
                net_match_economy_period(a.match.get());
            if (!a.sent(RecordType::economy).empty())
                sent_at.push_back(tick);
        }
        return sent_at;
    };
    CHECK((play() == std::vector<uint32_t>{90}));
    CHECK(a.connection.economy_periods == 5);
    // The second game begins on the same connection: 6 at the end of its
    // loading, 7 at tick 30, 8 at tick 60.
    const auto hooks = a.match->hooks;
    net_match_begin(a.match.get(), &a.connection, a.world, ReplicationSim{}, hooks);
    net_match_enter_game(a.match.get());
    a.world->game.tick = 0;
    CHECK((play() == std::vector<uint32_t>{60}));
}

// The loading screen's work runs at most every 200 ms (six connection time
// ticks): at once the first time, then once six ticks have passed.
void loading_frames_keep_the_loading_pace() {
    start_case("loading_frames_keep_the_loading_pace");
    LoadingPace pace{};
    CHECK(loading_frame_due(&pace, 100));
    CHECK(!loading_frame_due(&pace, 100));
    CHECK(!loading_frame_due(&pace, 105));
    CHECK(loading_frame_due(&pace, 106));
    CHECK(!loading_frame_due(&pace, 111));
    CHECK(loading_frame_due(&pace, 400));
    CHECK(pace.ran && pace.time == 400);
}

// What the in-game team panels tell the other machine: an ALLIES.GUI
// alliance change goes to that player alone as 0x23 and nowhere for a
// player simulated here; SHARE.GUI's metal and energy, already moved here,
// go to the receiver as 0x16 subtypes 2 and 1 with no debit or credit here,
// its map as subtype 3, and nothing goes to a player out of the game;
// CONTROL.GUI removes a watcher with reason 9 and a player with reason 1,
// each broadcast as 0x1b and retired on both machines.
void team_panel_changes_reach_the_other_machine() {
    start_case("team_panel_changes_reach_the_other_machine");
    constexpr uint32_t kLocal = 7, kComputer = 8, kOther = 9, kWatcher = 10;
    const Seat a_view[4] = {
        {kLocal, OA_PLAYER_STATUS_LOCAL, 1},
        {kComputer, OA_PLAYER_STATUS_COMPUTER, 1},
        {kOther, OA_PLAYER_STATUS_MIRRORED, 2},
        {kWatcher, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    const Seat b_view[4] = {
        {kLocal, OA_PLAYER_STATUS_MIRRORED, 1},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 1},
        {kOther, OA_PLAYER_STATUS_LOCAL, 2},
        {kWatcher, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    SeatedMachine a(a_view, 0);
    SeatedMachine b(b_view, 2);
    join(a, b);

    a.player(0).alliance[2] = 1;
    net_match_send_alliance(a.match.get(), 0, 2, 1);
    net_match_send_alliance(a.match.get(), 0, 1, 1);
    const auto alliances = a.sent(RecordType::alliance);
    CHECK(alliances.size() == 1);
    if (alliances.size() == 1) {
        CHECK(alliances[0].from == kLocal && alliances[0].to == kOther);
        CHECK(
            (alliances[0].bytes ==
             std::vector<uint8_t>{0x23, kLocal, 0, 0, 0, kOther, 0, 0, 0, 1, 0, 0, 0, 0})
        );
    }
    (void)net_match_pump(b.match.get());
    CHECK(b.player(2).allied_by[0] == 1 && b.player(0).alliance[2] == 1);

    a.forget_sent();
    net_match_send_give(a.match.get(), 0, 2, true, 100.0F);
    net_match_send_give(a.match.get(), 0, 2, false, 50.0F);
    net_match_share_sight(a.match.get(), 0, 2);
    const auto gives = a.sent(RecordType::resource_give);
    CHECK(gives.size() == 3);
    const auto amount_word = [](float amount) {
        uint32_t word = 0;
        std::memcpy(&word, &amount, sizeof word);
        return word;
    };
    if (gives.size() == 3) {
        for (const auto& give : gives)
            CHECK(give.from == kLocal && give.to == kOther);
        CHECK(
            gives[0].bytes ==
            record_bytes(RecordType::resource_give, {2, kLocal, kOther, amount_word(100.0F)})
        );
        CHECK(
            gives[1].bytes ==
            record_bytes(RecordType::resource_give, {1, kLocal, kOther, amount_word(50.0F)})
        );
        CHECK(gives[2].bytes == record_bytes(RecordType::resource_give, {3, kLocal, kOther, 0}));
    }
    CHECK(a.debits.empty() && a.credits.empty());
    (void)net_match_pump(b.match.get());
    CHECK(
        (b.credits ==
         std::vector<std::pair<uint8_t, float>>{{uint8_t{2}, 100.0F}, {uint8_t{2}, 50.0F}})
    );
    // A receiver whose units are all gone is out of the game.
    a.player(2).units_created = 1;
    a.forget_sent();
    net_match_send_give(a.match.get(), 0, 2, true, 10.0F);
    CHECK(a.sent(RecordType::resource_give).empty());
    a.player(2).units_created = 0;

    a.info(3).options = OA_SETUP_OPTION_WATCHER;
    a.forget_sent();
    net_match_remove_player(a.match.get(), 3, 9);
    net_match_remove_player(a.match.get(), 2, 1);
    const auto rejects = a.sent(RecordType::reject);
    CHECK(rejects.size() == 2);
    if (rejects.size() == 2) {
        CHECK(rejects[0].from == kLocal && rejects[0].to == broadcast_destination_id);
        CHECK((rejects[0].bytes == std::vector<uint8_t>{0x1b, kWatcher, 0, 0, 0, 9}));
        CHECK((rejects[1].bytes == std::vector<uint8_t>{0x1b, kOther, 0, 0, 0, 1}));
    }
    CHECK(a.player(3).in_use == 0 && a.player(3).reject_reason == 9);
    CHECK(a.player(2).in_use == 0 && a.player(2).reject_reason == 1);
    for (int i = 0; i < 2; ++i)
        (void)net_match_pump(b.match.get());
    CHECK(b.player(3).in_use == 0 && b.player(3).reject_reason == 9);
    CHECK(b.player(2).reject_reason == 1);
    CHECK(b.match->record_errors == 0);
}

// ---- two machines over sockets ----

constexpr uint16_t kUnitsPerPlayer = 4;
constexpr uint16_t kCommanderDef = 1;
constexpr int16_t kCommanderHealth = 100;
constexpr int32_t kStartX[2] = {64 << 16, 192 << 16};
constexpr int32_t kStartZ[2] = {64 << 16, 160 << 16};
constexpr int32_t kStep = 1 << 16; // one world unit per tick
constexpr uint8_t kLoopback[4] = {127, 0, 0, 1};

struct Machine {
    const char* name{};
    NetConnection connection{};
    mp::LobbyNet net{};
    std::unique_ptr<Game> game = std::make_unique<Game>();
    std::unique_ptr<mp::Lobby> lobby = std::make_unique<mp::Lobby>();
    std::unique_ptr<mp::ConnectState> connect = std::make_unique<mp::ConnectState>();
    World* world{};
    std::vector<MovementRecord> movement;
    std::unique_ptr<NetMatch> match = std::make_unique<NetMatch>();
    std::vector<std::string> chat;
    std::vector<uint8_t> chat_senders; // the slot each chat line came from
    std::vector<std::string> notices;
    bool has_goal{};
    int32_t goal_x{}, goal_z{};
    uint32_t creates{};
    uint32_t damages{};
    // The machine this one services while it waits (link_machines); null
    // while unlinked.
    Machine* peer{};

    // The peer stops servicing this machine before it goes: whichever is
    // destroyed second leaves its session without touching the freed one.
    // This machine still services the peer while it leaves its own.
    ~Machine() {
        if (peer != nullptr) {
            peer->connection.pump_context = nullptr;
            peer->connection.pump_other = nullptr;
            peer->peer = nullptr;
        }
        net_connection_destroy(&connection);
        world_destroy(world);
    }
};

Machine* g_machines[2]{};

void pump_other(void* context, uint32_t wait_ms) {
    auto* other = static_cast<Machine*>(context);
    if (other->connection.opened)
        sock::host_pump(other->connection.host, wait_ms);
}

// Both machines run in this thread, so each services the other while its
// session waits on a reply, until either is destroyed.
void link_machines(Machine& a, Machine& b) {
    a.connection.pump_context = &b;
    a.connection.pump_other = pump_other;
    a.peer = &b;
    b.connection.pump_context = &a;
    b.connection.pump_other = pump_other;
    b.peer = &a;
}

void pump_both(uint32_t wait_ms) {
    for (auto* m : g_machines)
        if (m->connection.opened)
            sock::host_pump(m->connection.host, wait_ms);
}

// The machines' shared clock: DirectPlay's windows and timeouts run on it in
// simulated time, which moves on whenever a pump finds nothing to read.
uint32_t g_now_ms = 0;
int64_t g_in_flight = 0; // bytes sent between the machines and not yet read

oa::netgame::sock::HostClock shared_clock() {
    oa::netgame::sock::HostClock clock;
    clock.now_ms = [](void*) { return g_now_ms; };
    clock.advance = [](void*, uint32_t ms) { g_now_ms += ms; };
    clock.in_flight = &g_in_flight;
    return clock;
}

uint32_t elapsed_ms(std::chrono::steady_clock::time_point since) {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - since
    )
                                     .count());
}

template <class Done>
bool wait_until(Done done, uint32_t limit_ms = 5000) {
    const auto start = std::chrono::steady_clock::now();
    while (!done()) {
        if (elapsed_ms(start) > limit_ms)
            return false;
        pump_both(2);
    }
    return true;
}

// Apply every lobby event queued for this machine; true when any arrived.
bool drain_lobby(Machine& m) {
    bool any = false;
    mp::LobbyEvent event{};
    while (m.net.receive(m.net.context, &event)) {
        (void)mp::lobby_apply_event(*m.lobby, event);
        any = true;
    }
    return any;
}

// Binds the machine to 127.0.0.1; enum_target is where a blank address
// enumerates.
void setup_lobby(
    Machine& m, const char* nickname, uint32_t seed, const uint8_t* enum_target = kLoopback
) {
    SessionLobbyConfig config{};
    std::memcpy(config.host.bind_ip, kLoopback, 4);
    std::memcpy(config.host.enum_target, enum_target, 4);
    config.host.stream_port_first = config.host.datagram_port_first = config.host.enum_port = 0;
    config.host.seed = seed;
    config.host.clock = shared_clock();
    CHECK(net_connection_create(&m.connection, config));
    m.net = session_lobby_net(&m.connection);
    mp::lobby_reset(*m.lobby, *m.game);
    m.lobby->net = m.net;
    m.lobby->local_version_major = 3;
    std::snprintf(mp::lobby_nickname(*m.game), 17, "%s", nickname);
    std::snprintf(mp::lobby_game_name(*m.game), 17, "%s", "Loopback Game");
}

// The match World: a small unit pool and one mobile ground definition.
// False when the World could not be allocated; the case then stops.
[[nodiscard]] bool setup_world(Machine& m) {
    m.world = world_create();
    const WorldCapacity capacity{kUnitsPerPlayer * OA_PLAYER_COUNT + 1u, 3, 0};
    const bool allocated = m.world != nullptr && world_alloc_tables(m.world, &capacity) != 0;
    CHECK(allocated);
    if (!allocated)
        return false;
    for (uint32_t i = 0; i < m.world->unit_slot_count; ++i)
        m.world->units[i].id = static_cast<uint16_t>(i);
    m.world->unit_defs[kCommanderDef].bm_code = 1;
    m.world->game.units_per_player = kUnitsPerPlayer;
    m.movement.assign(m.world->unit_slot_count, MovementRecord{});
    CHECK(match_launch_apply(*m.lobby, m.world));
    match_assign_unit_ranges(m.world);
    for (auto& player : m.world->game.players)
        player.economy = player.info;
    return true;
}

Unit* commander_of(World* world, const Player& player) {
    uint32_t count = 0;
    return world_player_units(world, &player, &count);
}

void spawn(Unit* unit, uint8_t owner, int32_t x, int32_t z) {
    unit->type_index = kCommanderDef;
    unit->def = oa_ref_from_index(kCommanderDef);
    unit->owner = oa_ref_from_index(owner);
    unit->owner_index = owner;
    unit->position = {x, 0, z};
    unit->health = kCommanderHealth;
    unit->movement = 1;
    unit->flags |= OA_UNIT_FLAG_LIVE;
}

ReplicationSim sim_for(Machine& m) {
    ReplicationSim sim{};
    sim.context = &m;
    sim.movement = [](void* ctx, World* w, Unit* unit) -> MovementRecord* {
        auto* self = static_cast<Machine*>(ctx);
        auto& record = self->movement[world_unit_slot(w, unit)];
        record.driver = MovementClass::ground;
        return &record;
    };
    sim.create_unit = [](void* ctx, World* w, uint8_t owner, const UnitCreatedRecord& r) {
        auto* self = static_cast<Machine*>(ctx);
        Unit* unit = world_unit_at(w, r.unit_index);
        if (unit == nullptr)
            return;
        ++self->creates;
        spawn(unit, owner, r.position[0], r.position[2]);
        unit->type_index = r.unit_def_index;
    };
    sim.apply_damage = [](void* ctx, World* w, const UnitDamageRecord& r) {
        auto* self = static_cast<Machine*>(ctx);
        Unit* unit = world_unit_at(w, r.target_unit_index);
        if (unit == nullptr || unit->type_index == 0)
            return;
        ++self->damages;
        unit->health = static_cast<int16_t>(unit->health - static_cast<int16_t>(r.amount));
    };
    return sim;
}

NetMatchHooks hooks_for(Machine& m) {
    NetMatchHooks hooks{};
    hooks.context = &m;
    hooks.chat = [](void* ctx, uint8_t sender, const char* text) {
        static_cast<Machine*>(ctx)->chat.emplace_back(text);
        static_cast<Machine*>(ctx)->chat_senders.push_back(sender);
    };
    hooks.notice = [](void* ctx, const char* text) {
        static_cast<Machine*>(ctx)->notices.emplace_back(text);
    };
    hooks.rand15 = [](void*) -> int32_t { return 0x4000; };
    return hooks;
}

// Local movement: step toward the goal and keep the route head current.
void simulate_local(Machine& m) {
    auto& game = m.world->game;
    auto& player = game.players[game.local_player_index];
    Unit* unit = commander_of(m.world, player);
    if (unit == nullptr || unit->type_index == 0)
        return;
    auto& record = m.movement[unit->id];
    const auto toward = [](int32_t from, int32_t to) {
        if (from < to)
            return from + std::min(kStep, to - from);
        return from - std::min(kStep, from - to);
    };
    if (m.has_goal) {
        unit->position.x = toward(unit->position.x, m.goal_x);
        unit->position.z = toward(unit->position.z, m.goal_z);
        if (unit->position.x == m.goal_x && unit->position.z == m.goal_z) {
            m.has_goal = false;
            record.ground.path_count = 0;
            record.ground.flags = static_cast<uint8_t>(
                (record.ground.flags & ~ground_driver_path_set) | ground_driver_resend
            );
        }
    }
}

void order(Machine& m, int32_t dx, int32_t dz) {
    auto& game = m.world->game;
    Unit* unit = commander_of(m.world, game.players[game.local_player_index]);
    m.has_goal = true;
    m.goal_x = unit->position.x + dx;
    m.goal_z = unit->position.z + dz;
    auto& record = m.movement[unit->id];
    record.ground.path_count = 1;
    record.ground.path[0][0] = static_cast<int16_t>(m.goal_x >> 16);
    record.ground.path[0][1] = static_cast<int16_t>(m.goal_z >> 16);
    record.ground.flags |= ground_driver_path_set | ground_driver_resend;
}

void tick(Machine& m) {
    auto& game = m.world->game;
    ++game.tick;
    (void)net_match_pump(m.match.get());
    if ((game.sim_run_flags & run_flag_paused) != 0)
        return;
    simulate_local(m);
    net_match_send_player_state(m.match.get(), &game.players[game.local_player_index]);
    net_match_after_tick(m.match.get());
    packet_layer_flush(m.connection.packets, net_connection_time(&m.connection), true);
}

bool same_unit(const Unit& a, const Unit& b) {
    return a.type_index == b.type_index && a.position.x == b.position.x &&
           a.position.y == b.position.y && a.position.z == b.position.z && a.health == b.health;
}

void two_machines_lobby_to_match() {
    start_case("two_machines_lobby_to_match");
    auto host = std::make_unique<Machine>();
    auto client = std::make_unique<Machine>();
    host->name = "host";
    client->name = "client";
    g_machines[0] = host.get();
    g_machines[1] = client.get();
    setup_lobby(*host, "hoster", 0x51);
    setup_lobby(*client, "joiner", 0x77);
    link_machines(*host, *client);

    // Host: TCP/IP, create the game, seat the local player.
    mp::Provider providers[4]{};
    CHECK(
        host->net.providers(host->net.context, providers, 4) == 1 &&
        providers[0].kind == mp::ProviderKind::tcpip
    );
    CHECK(host->net.open(host->net.context, &providers[0], "127.0.0.1"));
    CHECK(mp::connect_host(*host->lobby, *host->connect));
    CHECK(host->connection.hosting && host->connection.local_id != 0);
    CHECK(host->connection.packets->receiver.players == host->game->players);
    CHECK(mp::local_info(*host->lobby).color == 0);
    auto& host_info = mp::local_info(*host->lobby);
    host_info.max_units = kUnitsPerPlayer;
    host_info.metal_hundreds = 10;
    host_info.energy_hundreds = 10;
    host_info.options |= OA_SETUP_OPTION_COMMANDER_MASK & (1u << OA_SETUP_OPTION_COMMANDER_SHIFT);
    mp::lobby_publish_session(*host->lobby);

    // Client: enumerate on the host's enumeration port and join.
    CHECK(client->net.open(client->net.context, &providers[0], "127.0.0.1"));
    client->connection.host->engine.config.enum_port = host->connection.host->enum_port_bound;
    mp::SessionEntry sessions[mp::kMaxSessions]{};
    const auto found = client->net.enumerate(client->net.context, sessions, mp::kMaxSessions);
    CHECK(found == 1);
    CHECK(found == 1 && std::strncmp(sessions[0].name, "Loopback Game", 13) == 0);
    client->connect->chosen = sessions[0];
    CHECK(mp::connect_join(*client->lobby, *client->connect, false));
    const auto host_id = host->connection.local_id;
    const auto client_id = client->connection.local_id;
    CHECK(client_id != 0 && client_id != host_id);
    CHECK(client->connection.packets->receiver.players == client->game->players);
    CHECK(mp::local_info(*client->lobby).color == mp::kNoColor);

    // Both battle rooms learn about each other.
    CHECK(wait_until([&] {
        drain_lobby(*host);
        drain_lobby(*client);
        return mp::slot_for_player_id(*host->lobby, client_id) >= 0 &&
               mp::slot_for_player_id(*client->lobby, host_id) >= 0;
    }));
    // Seated players carry the session's names: the joiner through its
    // create message, the host through the joiner's enumeration.
    CHECK(
        std::strcmp(
            mp::slot_player(*host->lobby, mp::slot_for_player_id(*host->lobby, client_id)).name,
            "joiner"
        ) == 0
    );
    CHECK(
        std::strcmp(
            mp::slot_player(*client->lobby, mp::slot_for_player_id(*client->lobby, host_id)).name,
            "hoster"
        ) == 0
    );
    CHECK(mp::lobby_player_count(*host->game) == 2 && mp::lobby_player_count(*client->game) == 2);
    // The joiner asked the host for colour 0, which the host shows: it is
    // given the next free one.
    CHECK(wait_until([&] {
        drain_lobby(*host);
        drain_lobby(*client);
        const auto client_slot = mp::slot_for_player_id(*host->lobby, client_id);
        return mp::local_info(*client->lobby).color == 1 && client_slot >= 0 &&
               mp::slot_info(*host->lobby, client_slot)->color == 1;
    }));
    mp::local_info(*client->lobby).side = 1;
    mp::lobby_send_player_info(*host->lobby);
    mp::lobby_send_player_info(*client->lobby);
    CHECK(wait_until([&] {
        drain_lobby(*host);
        drain_lobby(*client);
        const auto host_slot = mp::slot_for_player_id(*client->lobby, host_id);
        const auto client_slot = mp::slot_for_player_id(*host->lobby, client_id);
        return host_slot >= 0 && client_slot >= 0 &&
               (mp::slot_info(*client->lobby, host_slot)->role & mp::kRoleHost) != 0 &&
               mp::slot_info(*host->lobby, client_slot)->side == 1;
    }));
    mp::lobby_say(*client->lobby, mp::local_player(*client->lobby), "ready");
    CHECK(wait_until([&] {
        drain_lobby(*host);
        return std::strstr(mp::lobby_chat_line(*host->game, 0), "ready") != nullptr ||
               std::strstr(mp::lobby_chat_line(*host->game, 1), "ready") != nullptr;
    }));

    // Start: the host leaves the battle room and sends 0x08.
    host_info.options |= mp::option::started;
    if (!setup_world(*host)) {
        g_machines[0] = g_machines[1] = nullptr;
        return;
    }
    CHECK(
        launch_host_info(host->world) != nullptr &&
        launch_host_info(host->world)->max_units == kUnitsPerPlayer
    );
    CHECK(host->world->game.session_rules == 1);
    CHECK(host->world->game.unit_def_id_bits == 2);
    net_match_begin(
        host->match.get(), &host->connection, host->world, sim_for(*host), hooks_for(*host)
    );
    // The start record is queued: nothing leaves before the loading screen's
    // first frame flushes it.
    net_match_send_game_start(host->match.get());
    const auto client_started = [&] {
        return (mp::local_info(*client->lobby).options & mp::option::started) != 0;
    };
    for (int i = 0; i < 10; ++i) {
        pump_both(2);
        drain_lobby(*client);
    }
    CHECK(!client_started());
    packet_layer_flush(host->connection.packets, net_connection_time(&host->connection), true);
    CHECK(wait_until([&] {
        drain_lobby(*client);
        return client_started();
    }));
    if (!setup_world(*client)) {
        g_machines[0] = g_machines[1] = nullptr;
        return;
    }
    net_match_begin(
        client->match.get(),
        &client->connection,
        client->world,
        sim_for(*client),
        hooks_for(*client)
    );

    // Both numbered their unit ranges alike despite different slot orders.
    const auto host_slot_on_host = host->world->game.local_player_index;
    const auto host_slot_on_client =
        static_cast<uint8_t>(mp::slot_for_player_id(*client->lobby, host_id));
    CHECK(
        host->world->game.players[host_slot_on_host].first_unit ==
        client->world->game.players[host_slot_on_client].first_unit
    );

    // Load barrier: start positions from the host, then everyone loaded, at
    // the loading screen's pace. Each machine does the loading screen's work
    // at most every six connection time ticks (200 ms), so the host's player
    // sends at most one probe and one load-progress record in each, and the
    // barrier still passes.
    //
    // Each machine then places its commander as the runtime does, once its
    // loading screen says the commanders can be placed: it enters the game,
    // stands its commander on its start position, announces it (0x09) and
    // ends its loading. That comes commander_wait_frames loading frames
    // after its barrier passed, so the 0x09 reaches the other machine after
    // its own barrier has passed, and it takes it.
    net_match_loader_waiting(host->match.get());
    net_match_loader_waiting(client->match.get());
    const uint8_t loaded_rows[load_progress_rows] = {100, 100, 100, 100, 100, 100};
    const auto sent_count = [](Machine& m, RecordType type) {
        return m.connection.packets->traffic
            .record_count[static_cast<uint8_t>(type)][static_cast<uint8_t>(TrafficChannel::sent)];
    };
    const auto probes_before = sent_count(*host, RecordType::probe);
    const auto progress_before = sent_count(*host, RecordType::load_progress);
    const auto loading_from = net_connection_time(&host->connection);
    // The host's machine is first, the client's second.
    Machine* const loading[2] = {host.get(), client.get()};
    LoadingPace paces[2]{};
    bool passed[2]{};
    bool placed[2]{};
    uint32_t passed_time[2]{};
    uint32_t placed_time[2]{};
    uint32_t probes_at_pass = 0;
    uint32_t progress_at_pass = 0;
    uint32_t frames_allowed = 0;
    // Set before the host's loading ends, which publishes the player count.
    host->world->game.player_count = 2;
    const auto place_commander = [](Machine& m) {
        net_match_enter_game(m.match.get());
        auto& game = m.world->game;
        auto& player = game.players[game.local_player_index];
        const auto start = net_match_start_position(m.match.get(), game.local_player_index);
        Unit* unit = commander_of(m.world, player);
        spawn(unit, game.local_player_index, kStartX[start], kStartZ[start]);
        net_match_send_unit_created(m.match.get(), unit);
        net_match_end_loading(m.match.get());
    };
    CHECK(wait_until([&] {
        for (std::size_t i = 0; i < 2; ++i) {
            auto& m = *loading[i];
            if (placed[i])
                continue;
            const bool ready = net_match_paced_loading_frame(m.match.get(), &paces[i], loaded_rows);
            const auto now = net_connection_time(&m.connection);
            if (!passed[i] && (m.world->game.load_flags & load_flag_barrier_passed) != 0) {
                passed[i] = true;
                passed_time[i] = now;
                if (i == 0) {
                    probes_at_pass = sent_count(*host, RecordType::probe);
                    progress_at_pass = sent_count(*host, RecordType::load_progress);
                    frames_allowed = (now - loading_from) / loading_frame_ticks + 1;
                }
            }
            if (ready) {
                place_commander(m);
                placed[i] = true;
                placed_time[i] = now;
            }
        }
        return placed[0] && placed[1];
    }));
    const auto probes = probes_at_pass - probes_before;
    const auto progress = progress_at_pass - progress_before;
    CHECK(probes >= 1 && probes <= frames_allowed);
    CHECK(progress == probes);
    // Nothing more goes out while the commanders wait, but for the 0x09.
    CHECK(sent_count(*host, RecordType::probe) == probes_at_pass);
    CHECK(!host->connection.packets->guaranteed && !client->connection.packets->guaranteed);
    for (std::size_t i = 0; i < 2; ++i)
        CHECK(
            passed[i] && placed[i] &&
            placed_time[i] - passed_time[i] >= commander_wait_frames * loading_frame_ticks
        );
    const auto host_start =
        net_match_start_position(host->match.get(), host->world->game.local_player_index);
    const auto client_start =
        net_match_start_position(client->match.get(), client->world->game.local_player_index);
    CHECK(host_start <= 1 && client_start <= 1 && host_start != client_start);

    // Each machine's copy of the other's commander stands on that
    // commander's start position from its 0x09, the first record that names
    // it: the copy is there before any unit state record (0x2c) went out, and
    // no record was refused.
    const auto client_slot_on_host =
        static_cast<uint8_t>(mp::slot_for_player_id(*host->lobby, client_id));
    CHECK(wait_until([&] {
        (void)net_match_pump(host->match.get());
        (void)net_match_pump(client->match.get());
        return host->creates == 1 && client->creates == 1;
    }));
    CHECK(sent_count(*host, RecordType::unit_state) == 0);
    CHECK(sent_count(*client, RecordType::unit_state) == 0);
    {
        const auto* copy =
            commander_of(client->world, client->world->game.players[host_slot_on_client]);
        CHECK(copy->type_index == kCommanderDef);
        CHECK(copy->position.x == kStartX[host_start] && copy->position.z == kStartZ[host_start]);
        copy = commander_of(host->world, host->world->game.players[client_slot_on_host]);
        CHECK(copy->type_index == kCommanderDef);
        CHECK(
            copy->position.x == kStartX[client_start] && copy->position.z == kStartZ[client_start]
        );
    }
    CHECK(host->match->records_refused == 0 && client->match->records_refused == 0);

    // Loading has ended on both machines; the host has published its session
    // again, which now says the game has started (flag 0x20).
    auto& client_view = client->connection.host->engine.desc;
    CHECK(wait_until([&] {
        return client_view.flags == (session_flag_game | session_flag_join_disabled);
    }));
    // The description is built again from the host's setup block: the full
    // name, map included, and user bytes with the started option and the
    // player count.
    const char* published = host->connection.host->engine.session_name;
    CHECK(std::strlen(published) > 16);
    CHECK(std::strcmp(client->connection.host->engine.session_name, published) == 0);
    const auto published_options = static_cast<uint16_t>(client_view.user[0] >> 16);
    CHECK((published_options & OA_SETUP_OPTION_STARTED) != 0);
    CHECK((published_options & 0xf) == 2);

    // 300 ticks with orders on both sides.
    for (uint32_t t = 0; t < 300; ++t) {
        if (t == 30)
            order(*host, 40 << 16, 0);
        if (t == 60)
            order(*client, 0, 30 << 16);
        if (t == 120)
            net_match_say(host->match.get(), "gg");
        tick(*host);
        tick(*client);
        pump_both(1);
    }
    CHECK(host->creates == 1 && client->creates == 1);
    CHECK(host->match->record_errors == 0 && client->match->record_errors == 0);
    CHECK(host->match->records_applied > 250 && client->match->records_applied > 250);
    CHECK(client->chat.size() == 1 && client->chat.size() == 1 && client->chat[0] == "gg");
    // The line comes from the host's slot on the client.
    CHECK(client->chat_senders.size() == 1 && client->chat_senders[0] == host_slot_on_client);
    CHECK(host->world->game.players[host_slot_on_host].last_sim_tick == 300);

    // Settle: the full record refreshes every unit slot each 4 ticks.
    for (uint32_t t = 0; t < 12; ++t) {
        tick(*host);
        tick(*client);
        pump_both(1);
    }
    const auto compare_all = [&] {
        uint32_t live = 0;
        bool same = true;
        for (uint32_t i = 1; i < host->world->unit_slot_count; ++i) {
            const auto& a = host->world->units[i];
            const auto& b = client->world->units[i];
            if (a.type_index != 0 || b.type_index != 0) {
                ++live;
                same = same && same_unit(a, b);
            }
        }
        return live == 2 && same;
    };
    CHECK(wait_until([&] {
        tick(*host);
        tick(*client);
        return compare_all();
    }));
    const auto* host_commander =
        commander_of(host->world, host->world->game.players[host_slot_on_host]);
    CHECK(host_commander->position.x == kStartX[host_start] + (40 << 16));

    // A health event relayed to the owner comes back in its full record.
    const auto* client_commander =
        commander_of(host->world, host->world->game.players[client_slot_on_host]);
    UnitDamageRecord damage{};
    damage.target_unit_index = client_commander->id;
    damage.source_unit_index = host_commander->id;
    damage.amount = 7;
    damage.kind = 1;
    net_match_send_damage(host->match.get(), net_match_local_route(host->match.get()), damage);
    CHECK(wait_until([&] {
        tick(*host);
        tick(*client);
        return client_commander->health == kCommanderHealth - 7 && compare_all();
    }));
    CHECK(client->damages == 1);

    // Pause and speed travel as 0x19.
    net_match_set_pause(host->match.get(), true);
    net_match_set_speed(client->match.get(), 15, true);
    packet_layer_flush(host->connection.packets, net_connection_time(&host->connection), true);
    packet_layer_flush(client->connection.packets, net_connection_time(&client->connection), true);
    CHECK(wait_until([&] {
        (void)net_match_pump(host->match.get());
        (void)net_match_pump(client->match.get());
        return (client->world->game.sim_run_flags & run_flag_paused) != 0 &&
               host->world->game.requested_speed == 15;
    }));
    net_match_set_pause(host->match.get(), false);

    // Each machine clamps a received speed to its own range: a sender whose
    // range reaches 0 sends 0, which 3.1c's range takes as 1, and a narrower
    // range takes as its slowest.
    client->match->rules.speed_min = 0;
    net_match_set_speed(client->match.get(), 0, true);
    CHECK(client->world->game.requested_speed == 0);
    packet_layer_flush(client->connection.packets, net_connection_time(&client->connection), true);
    CHECK(wait_until([&] {
        (void)net_match_pump(host->match.get());
        (void)net_match_pump(client->match.get());
        return host->world->game.requested_speed == 1;
    }));
    host->match->rules.speed_min = 12;
    host->match->rules.speed_max = 14;
    net_match_set_speed(client->match.get(), 5, true);
    packet_layer_flush(client->connection.packets, net_connection_time(&client->connection), true);
    CHECK(wait_until([&] {
        (void)net_match_pump(host->match.get());
        (void)net_match_pump(client->match.get());
        return host->world->game.requested_speed == 12 && host->world->game.current_speed == 12;
    }));
    host->match->rules.speed_min = min_game_speed;
    host->match->rules.speed_max = max_game_speed;
    client->match->rules.speed_min = min_game_speed;
    net_match_set_speed(host->match.get(), 10, true);
    net_match_set_speed(client->match.get(), 10, false);

    // The timing coordinator sees the peer's tick for the lag throttle.
    base::game_loop::Timing timing{};
    net_match_sync_timing(host->world, &timing);
    CHECK(
        timing.players[client_slot_on_host].present &&
        timing.players[client_slot_on_host].status == OA_PLAYER_STATUS_MIRRORED &&
        timing.players[client_slot_on_host].tick ==
            static_cast<uint32_t>(host->world->game.players[client_slot_on_host].last_sim_tick)
    );
    CHECK(net_match_check_timeouts(host->match.get()) == no_player_id);

    // The timeout scan names a remote player silent past the timeout, unless the
    // console's "Drop 0" bit (OA_CONSOLE_FLAG_NO_DROP) is set:
    // then neither the scan nor the paused baseline runs and the dialog's
    // player stays.
    {
        auto& game = host->world->game;
        auto& remote = game.players[client_slot_on_host];
        const auto heard = remote.last_update_time;
        const auto seconds = game.player_timeout_seconds;
        auto* match = host->match.get();
        game.player_timeout_seconds = 0;
        remote.last_update_time = 0;
        match->timeout_baseline = 0;
        CHECK(net_match_check_timeouts(match) == client_id);
        game.console_flags |= OA_CONSOLE_FLAG_NO_DROP;
        match->timeout_player = no_player_id;
        CHECK(net_match_check_timeouts(match) == no_player_id);
        match->timeout_player = client_id;
        CHECK(net_match_check_timeouts(match) == client_id);
        game.sim_run_flags |= run_flag_paused;
        CHECK(net_match_check_timeouts(match) == client_id && match->timeout_baseline == 0);
        game.console_flags &= static_cast<uint16_t>(~OA_CONSOLE_FLAG_NO_DROP);
        CHECK(net_match_check_timeouts(match) == no_player_id && match->timeout_baseline != 0);
        game.sim_run_flags &= static_cast<uint16_t>(~run_flag_paused);
        remote.last_update_time = heard;
        game.player_timeout_seconds = seconds;
        match->timeout_player = no_player_id;
    }

    // Across the turn of the connections' clock, which reads 30 units a
    // second from the milliseconds and turns over to 0 with them at 2^32
    // milliseconds: a player heard from just before it is neither silent
    // nor dropped just after it, a record queued before it goes out at the
    // first paced flush after it, and a paused match goes on probing.
    {
        auto& game = host->world->game;
        auto& remote = game.players[client_slot_on_host];
        auto* match = host->match.get();
        auto* packets = host->connection.packets;
        g_now_ms = 0xffffffffu - 1000u;
        const auto before = net_connection_time(&host->connection);
        CHECK(before + 40 > base::game_loop::scaled_clock_turn);
        match->timeout_baseline = before;
        remote.last_update_time = before;
        packet_layer_flush(packets, before, true);
        const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
        CHECK(net_match_send(match, host_id, broadcast_destination_id, &probe, 1));
        packet_layer_flush(packets, before + 1, false);
        CHECK(packets->channels[0].queue_count == 1);
        g_now_ms += 2000;
        const auto after = net_connection_time(&host->connection);
        CHECK(after < 40);
        CHECK(net_match_check_timeouts(match) == no_player_id);
        CHECK(!net_match_timeout_expired(match, client_id));
        CHECK(remote.in_use != 0);
        packet_layer_flush(packets, after, false);
        CHECK(packets->channels[0].queue_count == 0);
        match->keepalive_time = before + keepalive_interval;
        game.sim_run_flags |= run_flag_paused;
        net_match_paused_frame(match);
        CHECK(match->keepalive_time == after + keepalive_interval);
        game.sim_run_flags &= static_cast<uint16_t>(~run_flag_paused);
        CHECK(net_match_check_timeouts(match) == no_player_id);
        remote.last_update_time = net_connection_time(&host->connection);
    }

    // Game over: each side resends its economy until the other has
    // answered it, then settles.
    auto& host_local = host->world->game.players[host->world->game.local_player_index];
    auto& client_local = client->world->game.players[client->world->game.local_player_index];
    host_local.units_created = client_local.units_created = 1;
    CHECK(!net_match_final_economy(host->match.get()));
    CHECK(wait_until(
        [&] {
            (void)net_match_pump(host->match.get());
            (void)net_match_pump(client->match.get());
            packet_layer_flush(
                host->connection.packets, net_connection_time(&host->connection), true
            );
            packet_layer_flush(
                client->connection.packets, net_connection_time(&client->connection), true
            );
            const bool host_settled = net_match_final_economy(host->match.get());
            const bool client_settled = net_match_final_economy(client->match.get());
            return host_settled && client_settled;
        },
        20000
    ));

    // A 0x1c naming a third player: the host shows the line, retires the
    // slot and passes the notice on once. The client, which sent it without
    // retiring its own copy, hears the host's relay; its relay back finds the
    // host's slot retired and ends there.
    const auto count_line = [](const std::vector<std::string>& lines, const char* line) {
        return std::count(lines.begin(), lines.end(), std::string(line));
    };
    constexpr uint8_t kThirdSlot = 5;
    constexpr uint32_t kThirdId = 0x999;
    for (auto* m : g_machines) {
        auto& third = m->world->game.players[kThirdSlot];
        CHECK(third.in_use == 0);
        third.in_use = 1;
        third.status = OA_PLAYER_STATUS_MIRRORED;
        third.index = kThirdSlot;
        third.player_id = kThirdId;
        std::snprintf(third.name, sizeof third.name, "%s", "third");
        ++m->world->game.player_count;
    }
    const auto host_count = host->world->game.player_count;
    DisconnectNoticeRecord third_left{};
    third_left.player_id = kThirdId;
    uint8_t third_left_bytes[5];
    CHECK(
        encode_record(third_left, third_left_bytes, sizeof third_left_bytes, nullptr) ==
        WireError::ok
    );
    CHECK(net_match_send(
        client->match.get(),
        client_id,
        broadcast_destination_id,
        third_left_bytes,
        sizeof third_left_bytes
    ));
    CHECK(wait_until([&] {
        tick(*host);
        tick(*client);
        return client->world->game.players[kThirdSlot].in_use == 0;
    }));
    for (int i = 0; i < 20; ++i) {
        tick(*host);
        tick(*client);
        pump_both(1);
    }
    CHECK(
        host->world->game.players[kThirdSlot].in_use == 0 &&
        host->world->game.player_count == host_count - 1
    );
    CHECK(count_line(host->notices, "Player third has disconnected") == 1);
    CHECK(count_line(client->notices, "Player third has disconnected") == 1);

    // The client leaves as a finished game does: its connection finishes,
    // closing the session, and no record announces it. The host retires the
    // client's slot as departed (reason 1) with no chat line; the client's
    // live bit is cleared, and a second finish does nothing.
    const auto host_lines_before = host->notices.size();
    client->world->game.session_flags |= kNetFlagLive;
    net_connection_finish(&client->connection, &client->world->game);
    CHECK(
        !client->connection.opened && !client->connection.in_session &&
        (client->world->game.session_flags & kNetFlagLive) == 0
    );
    net_connection_finish(&client->connection, &client->world->game);
    CHECK(!client->connection.opened);
    CHECK(wait_until([&] {
        tick(*host);
        return host->world->game.players[client_slot_on_host].in_use == 0;
    }));
    CHECK(host->world->game.players[client_slot_on_host].reject_reason == 1);
    CHECK(host->notices.size() == host_lines_before);
    CHECK(count_line(host->notices, "Player joiner has disconnected") == 0);

    g_machines[0] = g_machines[1] = nullptr;
}

} // namespace

// One networked tick on a machine: every player simulated here sends its
// 0x2c record.
void tick_all_local(Machine& m) {
    auto& game = m.world->game;
    ++game.tick;
    (void)net_match_pump(m.match.get());
    for (auto& player : game.players)
        if (player.in_use != 0 &&
            (player.status == OA_PLAYER_STATUS_LOCAL || player.status == OA_PLAYER_STATUS_COMPUTER))
            net_match_send_player_state(m.match.get(), &player);
    net_match_after_tick(m.match.get());
    packet_layer_flush(m.connection.packets, net_connection_time(&m.connection), true);
}

// The game creates the hosted session with an empty password; the game's
// password is the join tag the host checks, so a joiner, which
// opens the session without one, gets in with the right tag.
void password_game_keeps_the_session_open() {
    start_case("password_game_keeps_the_session_open");
    auto host = std::make_unique<Machine>();
    auto client = std::make_unique<Machine>();
    g_machines[0] = host.get();
    g_machines[1] = client.get();
    setup_lobby(*host, "hoster", 0x55);
    setup_lobby(*client, "joiner", 0x7b);
    link_machines(*host, *client);
    std::snprintf(mp::lobby_password(*host->game), 11, "%s", "secret");
    std::snprintf(mp::lobby_password(*client->game), 11, "%s", "secret");
    mp::Provider providers[4]{};
    CHECK(host->net.providers(host->net.context, providers, 4) == 1);
    CHECK(host->net.open(host->net.context, &providers[0], "127.0.0.1"));
    CHECK(mp::connect_host(*host->lobby, *host->connect));
    CHECK(host->connection.host->engine.password[0] == '\0');
    CHECK(std::strcmp(host->connection.join_tag, "secret") == 0);
    mp::lobby_publish_session(*host->lobby);
    CHECK(client->net.open(client->net.context, &providers[0], "127.0.0.1"));
    client->connection.host->engine.config.enum_port = host->connection.host->enum_port_bound;
    mp::SessionEntry sessions[mp::kMaxSessions]{};
    CHECK(client->net.enumerate(client->net.context, sessions, mp::kMaxSessions) == 1);
    client->connect->chosen = sessions[0];
    CHECK(mp::connect_join(*client->lobby, *client->connect, false));
    const auto client_id = client->connection.local_id;
    CHECK(client_id != 0);
    CHECK(wait_until([&] {
        drain_lobby(*host);
        drain_lobby(*client);
        return mp::slot_for_player_id(*host->lobby, client_id) >= 0;
    }));
    g_machines[0] = g_machines[1] = nullptr;
}

// Stage F: the client seats a computer player, which becomes a session
// player of the client's machine. The host puts both of the
// client's players in one machine group, so every broadcast reaches the
// client once, addressed to its human, although DirectPlay would hand a
// frame to all players to each of the client's two players.
void client_computer_is_a_session_player() {
    start_case("client_computer_is_a_session_player");
    auto host = std::make_unique<Machine>();
    auto client = std::make_unique<Machine>();
    g_machines[0] = host.get();
    g_machines[1] = client.get();
    setup_lobby(*host, "hoster", 0x53);
    setup_lobby(*client, "joiner", 0x79);
    link_machines(*host, *client);
    mp::Provider providers[4]{};
    CHECK(host->net.providers(host->net.context, providers, 4) == 1);
    CHECK(host->net.open(host->net.context, &providers[0], "127.0.0.1"));
    CHECK(mp::connect_host(*host->lobby, *host->connect));
    auto& host_info = mp::local_info(*host->lobby);
    host_info.max_units = kUnitsPerPlayer;
    mp::lobby_publish_session(*host->lobby);
    CHECK(client->net.open(client->net.context, &providers[0], "127.0.0.1"));
    client->connection.host->engine.config.enum_port = host->connection.host->enum_port_bound;
    mp::SessionEntry sessions[mp::kMaxSessions]{};
    CHECK(client->net.enumerate(client->net.context, sessions, mp::kMaxSessions) == 1);
    client->connect->chosen = sessions[0];
    CHECK(mp::connect_join(*client->lobby, *client->connect, false));
    const auto host_id = host->connection.local_id;
    const auto client_id = client->connection.local_id;
    CHECK(wait_until([&] {
        drain_lobby(*host);
        drain_lobby(*client);
        return mp::slot_for_player_id(*host->lobby, client_id) >= 0 &&
               mp::slot_for_player_id(*client->lobby, host_id) >= 0;
    }));

    // The computer takes the client's first open slot under its own id.
    int32_t computer_slot = -1;
    for (int32_t slot = 0; slot < mp::kSlotCount && computer_slot < 0; ++slot)
        if (mp::slot_player(*client->lobby, slot).status == mp::kSlotOpen)
            computer_slot = slot;
    mp::lobby_add_computer(*client->lobby, computer_slot);
    const auto& computer = mp::slot_player(*client->lobby, computer_slot);
    const auto computer_id = computer.player_id;
    CHECK(computer.status == mp::kSlotComputer && computer_id != 0 && computer_id != client_id);
    const auto* session_player =
        dplay::engine_find_player(&client->connection.host->engine, computer_id);
    CHECK(
        session_player != nullptr && session_player->local &&
        std::strcmp(session_player->info.short_name, "AI:joiner") == 0
    );

    // Lobby blocks and group requests until every lobby agrees.
    const auto group_of = [](Machine& m, uint32_t id) {
        const auto slot = mp::slot_for_player_id(*m.lobby, id);
        return slot >= 0 ? mp::slot_player(*m.lobby, slot).machine_group : 0xffu;
    };
    CHECK(wait_until([&] {
        mp::lobby_send_player_info(*host->lobby);
        mp::lobby_send_player_info(*client->lobby);
        drain_lobby(*host);
        drain_lobby(*client);
        mp::note_shared_machines(*host->game);
        mp::note_shared_machines(*client->game);
        for (auto* m : {host.get(), client.get()})
            if (group_of(*m, host_id) != 1 || group_of(*m, client_id) != 2 ||
                group_of(*m, computer_id) != 2)
                return false;
        return host->game->shared_machines == 1 && client->game->shared_machines == 1;
    }));
    CHECK(host->game->player_count == 3 && client->game->player_count == 3);
    const auto computer_on_host = mp::slot_for_player_id(*host->lobby, computer_id);
    CHECK(
        computer_on_host >= 0 &&
        mp::slot_player(*host->lobby, computer_on_host).status == mp::kSlotRemote &&
        mp::slot_info(*host->lobby, computer_on_host)->state == OA_PLAYER_STATUS_COMPUTER
    );

    // Launch. The host queues its 0x08 as its battle room closes; nothing
    // leaves until the loading screen's first frame, which runs while the
    // host's world is built: it sends the 0x08, and the host's player sends
    // a probe and its load progress, the mean of the rows. Machines are
    // shared, so all of it goes to the client's human alone and reaches the
    // client's machine once. A later frame carries the rows as they then
    // stand.
    host_info.options |= mp::option::started;
    const auto received = [&](RecordType type) {
        return client->connection.packets->traffic
            .record_count[static_cast<uint8_t>(type)]
                         [static_cast<uint8_t>(TrafficChannel::received)];
    };
    std::vector<uint8_t> progress_seen;
    const auto drain_client = [&] {
        mp::LobbyEvent event{};
        while (client->net.receive(client->net.context, &event)) {
            if (event.kind == mp::LobbyEventKind::record && event.size >= 2 &&
                event.data[0] == static_cast<uint8_t>(RecordType::load_progress))
                progress_seen.push_back(event.data[1]);
            (void)mp::lobby_apply_event(*client->lobby, event);
        }
    };
    const auto client_started = [&] {
        return (mp::local_info(*client->lobby).options & mp::option::started) != 0;
    };
    net_match_queue_game_start(&host->connection, *host->game);
    for (int i = 0; i < 10; ++i) {
        pump_both(2);
        drain_client();
    }
    CHECK(!client_started() && received(RecordType::game_start) == 0);
    const uint8_t building[load_progress_rows] = {100, 40, 0, 0, 0, 0};
    net_match_building_frame(&host->connection, *host->game, building);
    CHECK(wait_until([&] {
        drain_client();
        return client_started() && !progress_seen.empty();
    }));
    const uint8_t built[load_progress_rows] = {100, 100, 100, 100, 100, 100};
    net_match_building_frame(&host->connection, *host->game, built);
    CHECK(wait_until([&] {
        drain_client();
        return progress_seen.size() >= 2;
    }));
    CHECK((progress_seen == std::vector<uint8_t>{23, 100}));
    CHECK(received(RecordType::game_start) == 1 && received(RecordType::probe) == 2);

    // Barrier, one commander per player simulated on each machine.
    if (!setup_world(*host)) {
        g_machines[0] = g_machines[1] = nullptr;
        return;
    }
    net_match_begin(
        host->match.get(), &host->connection, host->world, sim_for(*host), hooks_for(*host)
    );
    if (!setup_world(*client)) {
        g_machines[0] = g_machines[1] = nullptr;
        return;
    }
    net_match_begin(
        client->match.get(),
        &client->connection,
        client->world,
        sim_for(*client),
        hooks_for(*client)
    );
    CHECK(host->world->game.shared_machines == 1 && client->world->game.shared_machines == 1);
    net_match_loader_waiting(host->match.get());
    net_match_loader_waiting(client->match.get());
    CHECK(wait_until([&] {
        const bool a = net_match_loading_frame(host->match.get());
        const bool b = net_match_loading_frame(client->match.get());
        return a && b;
    }));
    CHECK(net_match_start_position(client->match.get(), static_cast<uint8_t>(computer_slot)) <= 2);
    net_match_enter_game(host->match.get());
    net_match_enter_game(client->match.get());
    for (auto* m : g_machines)
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            const auto& player = m->world->game.players[slot];
            if (player.in_use == 0 || (player.status != OA_PLAYER_STATUS_LOCAL &&
                                       player.status != OA_PLAYER_STATUS_COMPUTER))
                continue;
            const auto start = net_match_start_position(m->match.get(), slot);
            Unit* unit = commander_of(m->world, player);
            spawn(unit, slot, kStartX[start % 2], kStartZ[start % 2] + (start / 2) * (16 << 16));
            net_match_send_unit_created(m->match.get(), unit);
        }

    // Each machine applies each record of the other once: the client one
    // 0x2c per host tick, the host one per client player per tick.
    constexpr uint32_t kTicks = 90;
    for (uint32_t t = 0; t < kTicks; ++t) {
        tick_all_local(*host);
        tick_all_local(*client);
        pump_both(1);
    }
    for (uint32_t t = 0; t < 40; ++t) {
        (void)net_match_pump(host->match.get());
        (void)net_match_pump(client->match.get());
        pump_both(1);
    }
    CHECK(host->creates == 2 && client->creates == 1);
    CHECK(host->match->record_errors == 0 && client->match->record_errors == 0);
    // The last sender ticks may still sit in the packet layer's hold window.
    CHECK(
        client->match->records_applied <= kTicks + 1 && client->match->records_applied > kTicks / 2
    );
    CHECK(host->match->records_applied <= 2 * kTicks + 2 && host->match->records_applied > kTicks);
    const auto host_slot_on_client =
        static_cast<uint8_t>(mp::slot_for_player_id(*client->lobby, host_id));
    CHECK(client->world->game.players[host_slot_on_client].last_sim_tick > 0);
    g_machines[0] = g_machines[1] = nullptr;
}

// The TCP/IP address picks where the client enumerates. A blank address
// searches, also after a typed one; both machines are bound to 127.0.0.1,
// so the search has no network to broadcast to and finds the host through
// its copy to this machine. White space around an address is ignored, and
// host names and the shorter numeric forms resolve. An address that
// resolves to nothing opens but fails to enumerate, as "Invalid TCP/IP
// Address" needs. A request that cannot be sent at all lists no games,
// blank address or typed.
void address_picks_the_enumeration_target() {
    start_case("address_picks_the_enumeration_target");
    auto host = std::make_unique<Machine>();
    auto client = std::make_unique<Machine>();
    g_machines[0] = host.get();
    g_machines[1] = client.get();
    setup_lobby(*host, "hoster", 0x57);
    setup_lobby(*client, "joiner", 0x7d, sock::local_networks_ip);
    link_machines(*host, *client);
    mp::Provider providers[4]{};
    CHECK(host->net.providers(host->net.context, providers, 4) == 1);
    CHECK(host->net.open(host->net.context, &providers[0], "127.0.0.1"));
    CHECK(mp::connect_host(*host->lobby, *host->connect));
    mp::lobby_publish_session(*host->lobby);
    const uint16_t host_enum_port = host->connection.host->enum_port_bound;
    CHECK(host_enum_port != 0);

    // Reopens the client with an address and enumerates once; without the
    // host's enumeration port the request cannot be sent.
    const auto enumerate_at = [&](const char* address, bool to_host_port) {
        client->net.close(client->net.context);
        CHECK(client->net.open(client->net.context, &providers[0], address));
        if (to_host_port)
            client->connection.host->engine.config.enum_port = host_enum_port;
        client->connection.session.enum_timeout_ms = 300;
        mp::SessionEntry sessions[mp::kMaxSessions]{};
        const auto found = client->net.enumerate(client->net.context, sessions, mp::kMaxSessions);
        if (found == 1)
            CHECK(std::strncmp(sessions[0].name, "Loopback Game", 13) == 0);
        return found;
    };
    const auto target_is = [&](const uint8_t ip[4]) {
        return std::memcmp(client->connection.host->config.enum_target, ip, 4) == 0;
    };

    CHECK(enumerate_at(" 127.0.0.1\t", true) == 1);
    CHECK(target_is(kLoopback));
    CHECK(enumerate_at("", true) == 1);
    CHECK(target_is(sock::local_networks_ip));
    CHECK(enumerate_at("localhost", true) == 1);
    CHECK(target_is(kLoopback));
    CHECK(enumerate_at("127.1", true) == 1);
    CHECK(target_is(kLoopback));
    CHECK(enumerate_at("   ", true) == 1);
    CHECK(target_is(sock::local_networks_ip));
    CHECK(enumerate_at("bad..address", true) == -1);
    CHECK(enumerate_at(nullptr, true) == 1);
    CHECK(target_is(sock::local_networks_ip));
    CHECK(enumerate_at("", false) == 0);
    CHECK(enumerate_at("127.0.0.1", false) == 0);
    client->net.close(client->net.context);
    g_machines[0] = g_machines[1] = nullptr;
}

namespace {

using Bytes = std::vector<uint8_t>;

// ---- a mod profile's network rules ----

// Rules with the private channel and everything that rides on it.
WireRules channel_rules() {
    WireRules rules{};
    rules.private_channel = PrivateChannel::sub_id_dispatch;
    rules.integrity_check = IntegrityCheck::single_challenge;
    rules.vote_reject = true;
    return rules;
}

// Rules with the recorder presented.
WireRules recorder_rules() {
    WireRules rules{};
    rules.recorder_protocol = recorder_protocol_current;
    rules.recorder_session_commands = true;
    rules.speed_min = 0;
    rules.speed_lock = true;
    return rules;
}

// Sends raw record bytes from one machine and pumps them on the other.
void deliver_bytes(SeatedMachine& from, SeatedMachine& to, uint32_t from_id, const Bytes& bytes) {
    CHECK(net_match_send(
        from.match.get(), from_id, broadcast_destination_id, bytes.data(), bytes.size()
    ));
    packet_layer_flush(from.connection.packets, 0, true);
    (void)net_match_pump(to.match.get());
}

// A private chat line reaches its handler and is never shown under the
// rule; without it, 3.1c's chat shows it as it is.
void private_lines_stay_hidden() {
    start_case("private_lines_stay_hidden");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    Bytes line(private_record_bytes, 0);
    line[0] = 0x05;
    line[2] = private_sub_vote;
    line[3] = 0x41;
    line[5] = vote_op_yes;
    {
        SeatedMachine a(a_view, 0, channel_rules());
        SeatedMachine b(b_view, 1, channel_rules());
        join(a, b);
        deliver_bytes(a, b, kA, line);
        CHECK(b.chats.empty());
        CHECK(b.match->private_records == 1);
    }
    {
        SeatedMachine a(a_view, 0);
        SeatedMachine b(b_view, 1);
        join(a, b);
        deliver_bytes(a, b, kA, line);
        CHECK(b.chats.size() == 1 && b.chats[0].empty());
        CHECK(b.match->private_records == 0);
    }
}

// ---- Unicode chat ----

// Chinese as UTF-8: U+4F60 U+597D, the full-width comma, U+4E16 U+754C
// and the ideographic full stop.
constexpr const char* kHanziLine = "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c"
                                   "\xe3\x80\x82";

// The chat records a machine sent since it last forgot: each one's
// addressee and text.
std::vector<std::pair<uint32_t, std::string>> chat_sent(SeatedMachine& m) {
    std::vector<std::pair<uint32_t, std::string>> out;
    for (const auto& record : m.sent(RecordType::chat)) {
        const auto* text = reinterpret_cast<const char*>(record.bytes.data() + 1);
        out.emplace_back(record.to, std::string(text, ::strnlen(text, record.bytes.size() - 1)));
    }
    return out;
}

// A chat record from raw text bytes, the rest of its 64 bytes zero.
Bytes chat_record_bytes(std::string_view text) {
    Bytes bytes(1 + sizeof(ChatRecord::text), 0);
    bytes[0] = static_cast<uint8_t>(RecordType::chat);
    std::memcpy(bytes.data() + 1, text.data(), std::min(text.size(), sizeof(ChatRecord::text)));
    return bytes;
}

// Tells whether a machine's copy of a player's setup block says UTF-8 chat.
bool block_says_utf8(SeatedMachine& m, uint8_t slot) {
    return announces_unicode_chat(reinterpret_cast<const uint8_t*>(&m.info(slot)));
}

// Every block a match sends carries the engine signature, so a remote OA
// player's block, replaced by those blocks during the match, still says it
// is OA's afterwards.
void engine_signature_in_game_blocks() {
    start_case("engine_signature_in_game_blocks");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0);
    SeatedMachine b(b_view, 1);
    join(a, b);
    net_match_send_player_status(a.match.get(), false);
    net_match_send_player_status(b.match.get(), false);
    const auto blocks = a.sent(RecordType::player_info);
    CHECK(!blocks.empty());
    if (!blocks.empty())
        CHECK(sent_by_open_annihilation(blocks[0].bytes.data() + 1));
    packet_layer_flush(b.connection.packets, 0, true);
    (void)net_match_pump(a.match.get());
    (void)net_match_pump(b.match.get());
    CHECK(sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(&a.info(1))));
    CHECK(sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(&b.info(0))));
}

// Two machines with Unicode chat on, both Chinese: each says so in the
// setup block it sends (its chat signature and flags) and learns it of the
// other. A Chinese line longer than a record goes as four records, each
// "<Name> " and whole characters, arrives intact, and the recording sees
// each record once.
void unicode_chat_between_two_readers() {
    start_case("unicode_chat_between_two_readers");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0);
    SeatedMachine b(b_view, 1);
    join(a, b);
    a.match->unicode_chat = true;
    b.match->unicode_chat = true;
    net_match_send_player_status(a.match.get(), false);
    net_match_send_player_status(b.match.get(), false);
    const auto blocks = a.sent(RecordType::player_info);
    CHECK(!blocks.empty());
    if (!blocks.empty()) {
        const auto& block = blocks[0].bytes;
        CHECK(block.size() == 1 + player_info_block_bytes);
        const auto* chat = block.data() + 1 + player_info_chat_signature_offset;
        CHECK(chat[0] == 'U' && chat[1] == '8' && chat[2] == chat_flag_utf8);
    }
    packet_layer_flush(b.connection.packets, 0, true);
    (void)net_match_pump(a.match.get());
    (void)net_match_pump(b.match.get());
    CHECK(block_says_utf8(a, 1) && block_says_utf8(b, 0));

    a.forget_sent();
    a.seen_chat.clear();
    std::string line = "<Alpha> ";
    for (int i = 0; i < 10; ++i)
        line += kHanziLine;
    net_match_say(a.match.get(), line.c_str());
    const auto sent = chat_sent(a);
    CHECK(sent.size() == chat_line_parts);
    std::string joined;
    std::vector<std::string> texts;
    for (const auto& [to, text] : sent) {
        CHECK(to == broadcast_destination_id);
        CHECK(text.rfind("<Alpha> ", 0) == 0 && chat_strict_utf8(text) == text);
        joined += text.substr(8);
        texts.push_back(text);
    }
    CHECK(joined == line.substr(8));
    CHECK(a.seen_chat == texts);
    CHECK(a.match->said_parts == texts);
    (void)net_match_pump(b.match.get());
    CHECK(b.chats == texts);
    CHECK(b.match->record_errors == 0 && b.match->records_refused == 0);
}

// A room of two Chinese machines with Unicode chat on and an English one
// with it off: the English machine gets each line in the code page, '?'
// for each hanzi, the Chinese one in UTF-8, one copy each, and the
// recording sees the UTF-8 line once. The English machine shows what
// reaches it, a stray UTF-8 line too, as bytes, and its own lines go out
// as they always have.
void unicode_chat_in_a_mixed_room() {
    start_case("unicode_chat_in_a_mixed_room");
    constexpr uint32_t kA = 7, kB = 9, kC = 10;
    const Seat a_view[4] = {
        {kA, OA_PLAYER_STATUS_LOCAL, 1},
        {kB, OA_PLAYER_STATUS_MIRRORED, 2},
        {kC, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    const Seat c_view[4] = {
        {kA, OA_PLAYER_STATUS_MIRRORED, 1},
        {kB, OA_PLAYER_STATUS_MIRRORED, 2},
        {kC, OA_PLAYER_STATUS_LOCAL, 3}
    };
    SeatedMachine a(a_view, 0);
    SeatedMachine c(c_view, 2);
    a.match->unicode_chat = true;
    mark_unicode_chat(reinterpret_cast<uint8_t*>(&a.info(1)), true);
    mark_unicode_chat(reinterpret_cast<uint8_t*>(&c.info(0)), true);
    const std::string line = std::string("<Alpha> ") + kHanziLine;
    net_match_say(a.match.get(), line.c_str());
    const auto sent = chat_sent(a);
    CHECK(sent.size() == 2);
    if (sent.size() == 2) {
        CHECK(sent[0].first == kB && sent[0].second == line);
        CHECK(sent[1].first == kC && sent[1].second == "<Alpha> ??????");
    }
    CHECK((a.seen_chat == std::vector<std::string>{line}));

    // What reaches the English machine, from a sender with the setting off.
    SeatedMachine plain(a_view, 0);
    join(plain, c);
    deliver_bytes(plain, c, kA, chat_record_bytes("<Alpha> ??????"));
    deliver_bytes(plain, c, kA, chat_record_bytes(line));
    CHECK((c.chats == std::vector<std::string>{"<Alpha> ??????", line}));
    c.forget_sent();
    net_match_say(c.match.get(), "<Charlie> Zo\xe9");
    const auto from_c = chat_sent(c);
    CHECK(from_c.size() == 1 && from_c[0].first == broadcast_destination_id);
    CHECK(from_c.size() == 1 && from_c[0].second == "<Charlie> Zo\xe9");
    CHECK(c.match->record_errors == 0);
}

// A peer that says it sends UTF-8 is read strictly: overlong forms,
// surrogates, bytes past U+10FFFF, stray continuations and a character cut
// at the record's end become '?', never bytes of the line. A line from a
// peer without the flag is shown as it came.
void unicode_chat_reads_malformed_lines_safely() {
    start_case("unicode_chat_reads_malformed_lines_safely");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0);
    SeatedMachine b(b_view, 1);
    join(a, b);
    b.match->unicode_chat = true;
    mark_unicode_chat(reinterpret_cast<uint8_t*>(&b.info(0)), true);
    deliver_bytes(
        a, b, kA, chat_record_bytes("<A> \xc0\xaf \xed\xa0\x80 \xf4\x90\x80\x80 \x80\xff")
    );
    // 64 bytes with no terminator, a hanzi cut by the record's end.
    std::string full(62, 'x');
    full += "\xe4\xbd\xa0";
    deliver_bytes(a, b, kA, chat_record_bytes(full));
    CHECK(b.chats.size() == 2);
    if (b.chats.size() == 2) {
        CHECK(b.chats[0] == "<A> ?? ??? ???? ??");
        CHECK(b.chats[1] == std::string(62, 'x') + "??");
    }
    mark_unicode_chat(reinterpret_cast<uint8_t*>(&b.info(0)), false);
    deliver_bytes(a, b, kA, chat_record_bytes("<A> \xc0\xaf"));
    CHECK(b.chats.size() == 3 && b.chats.back() == "<A> \xc0\xaf");
    CHECK(b.match->record_errors == 0);
}

// With Unicode chat on, a command ('+' or '.' first, after the speaker's
// name or without one) goes as one record, as typed, even past 64 bytes.
// With it off a line goes exactly as before: its first 64 bytes cut between
// whole characters, the rest zero, and the setup block's chat bytes stay
// zero.
void unicode_chat_keeps_commands_and_the_plain_wire() {
    start_case("unicode_chat_keeps_commands_and_the_plain_wire");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    SeatedMachine on(a_view, 0);
    SeatedMachine off(a_view, 0);
    on.match->unicode_chat = true;
    mark_unicode_chat(reinterpret_cast<uint8_t*>(&on.info(1)), true);
    const std::string record_command = "<Alpha> .record " + std::string(60, 'n');
    for (const std::string& command : {std::string("+SetShareMetal 5"), record_command}) {
        on.forget_sent();
        off.forget_sent();
        net_match_say(on.match.get(), command.c_str());
        net_match_say(off.match.get(), command.c_str());
        const auto sent_on = on.sent(RecordType::chat);
        const auto sent_off = off.sent(RecordType::chat);
        CHECK(sent_on.size() == 1 && sent_off.size() == 1);
        if (sent_on.size() == 1 && sent_off.size() == 1)
            CHECK(sent_on[0].bytes == sent_off[0].bytes && sent_on[0].to == sent_off[0].to);
        CHECK(on.match->said_parts.empty() && off.match->said_parts.empty());
    }

    // Off: 62 ASCII bytes and a hanzi the 64th byte would cut.
    off.forget_sent();
    std::string line(62, 'x');
    line += kHanziLine;
    net_match_say(off.match.get(), line.c_str());
    const Bytes expected = chat_record_bytes(std::string(62, 'x'));
    const auto sent = off.sent(RecordType::chat);
    CHECK(sent.size() == 1 && sent[0].bytes == expected);
    off.forget_sent();
    net_match_send_player_status(off.match.get(), false);
    const auto blocks = off.sent(RecordType::player_info);
    CHECK(!blocks.empty());
    for (const auto& block : blocks) {
        const auto* chat = block.bytes.data() + 1 + player_info_chat_signature_offset;
        CHECK(chat[0] == 0 && chat[1] == 0 && chat[2] == 0);
    }
}

// The alliance lines and the missing-map line go in English, as English
// 3.1c sends them, and the receiver shows the phrase in its own language,
// the names as they came, while its recording keeps the English: a German
// machine, a Chinese one with Unicode chat on, and an English one, which
// has no translation.
void alliance_lines_show_in_the_language_shown() {
    start_case("alliance_lines_show_in_the_language_shown");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    using Translations = std::map<std::string, std::string>;
    const Translations german{
        {"allied with", "Verb\xc3\xbcndet mit"},
        {"broke alliance with", "k\xc3\xbcndigt Allianz mit"},
        {"does not have this map", "hat diese Karte nicht"},
    };
    const Translations chinese{
        {"allied with", "\xe7\xbb\x93\xe7\x9b\x9f"},
        {"broke alliance with", "\xe8\xa7\xa3\xe9\x99\xa4\xe7\xbb\x93\xe7\x9b\x9f"},
        {"does not have this map", "\xe6\xb2\xa1\xe6\x9c\x89\xe6\xad\xa4\xe5\x9c\xb0\xe5\x9b\xbe"},
    };
    const Translations english{};
    for (const Translations* translations : {&german, &chinese, &english}) {
        SeatedMachine a(a_view, 0);
        SeatedMachine b(b_view, 1);
        join(a, b);
        b.translations = *translations;
        b.match->unicode_chat = translations == &chinese;
        const auto in_language = [&](const std::string& english_phrase) {
            const auto found = translations->find(english_phrase);
            return found != translations->end() ? found->second : english_phrase;
        };
        const std::string lines[] = {
            "<Hans>  allied with Li",
            "<Hans>  broke alliance with Li",
            "<Hans> does not have this map"
        };
        const std::string shown[] = {
            "<Hans>  " + in_language("allied with") + " Li",
            "<Hans>  " + in_language("broke alliance with") + " Li",
            "<Hans> " + in_language("does not have this map"),
        };
        for (std::size_t index = 0; index < std::size(lines); ++index) {
            a.forget_sent();
            b.chats.clear();
            b.seen_chat.clear();
            net_match_say(a.match.get(), lines[index].c_str());
            const auto sent = chat_sent(a);
            CHECK(sent.size() == 1 && sent[0].second == lines[index]);
            (void)net_match_pump(b.match.get());
            CHECK(b.chats.size() == 1 && b.chats[0] == shown[index]);
            CHECK(b.seen_chat.size() == 1 && b.seen_chat[0] == lines[index]);
        }
    }
}

// Two machines on the same program and data challenge each other at tick
// 180, answer with two records back to back and agree, so the tick-600
// report names nobody; a machine whose data differs is reported, and its
// report line goes to everyone.
void integrity_check_answers_and_reports() {
    start_case("integrity_check_answers_and_reports");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    for (const bool same : {true, false}) {
        SeatedMachine a(a_view, 0, channel_rules());
        SeatedMachine b(b_view, 1, channel_rules());
        join(a, b);
        oa::base::text::copy_padded(a.player(0).name, "Alpha", sizeof a.player(0).name);
        // Remote humans still playing are challenged.
        a.info(1).state = 1;
        b.info(0).state = 1;
        a.match->integrity.identity.program[0] = 0x11;
        b.match->integrity.identity.program[0] = 0x11;
        b.match->integrity.identity.game_data[0] = same ? 0 : 0x22;
        a.world->game.tick = integrity_challenge_tick;
        net_match_rules_tick(a.match.get());
        const auto challenges = a.sent(RecordType::chat);
        CHECK(challenges.size() == 1);
        if (challenges.size() == 1) {
            CHECK(challenges[0].to == kB && challenges[0].bytes.size() == private_record_bytes);
            CHECK(challenges[0].bytes[2] == private_sub_integrity);
            CHECK(challenges[0].bytes[5] == integrity_op_challenge);
        }
        (void)net_match_pump(b.match.get());
        const auto replies = b.sent(RecordType::chat);
        CHECK(replies.size() == 2);
        if (replies.size() == 2) {
            CHECK(replies[0].to == kA && replies[1].to == kA);
            CHECK(replies[0].bytes[5] == integrity_op_module_reply);
            CHECK(replies[1].bytes[5] == integrity_op_data_reply);
        }
        // The two records of a frame are spread over two ticks.
        (void)net_match_pump(a.match.get());
        ++a.world->game.tick;
        (void)net_match_pump(a.match.get());
        CHECK(a.match->private_records == 2);
        const auto* peer = integrity_peer(a.match->integrity, kB);
        CHECK(peer != nullptr && peer->program_answered && peer->data_answered);
        CHECK(peer != nullptr && peer->program_matches && peer->data_matches == same);
        CHECK(integrity_issue_count(a.match->integrity) == (same ? 0 : 1));
        a.forget_sent();
        a.world->game.tick = integrity_report_tick;
        net_match_rules_tick(a.match.get());
        const auto reports = a.sent(RecordType::chat);
        CHECK(reports.size() == (same ? 0u : 1u));
        if (!same && reports.size() == 1)
            CHECK(
                std::string(reinterpret_cast<const char*>(reports[0].bytes.data()) + 1) ==
                "Alpha reports VerCheck issues with 1 other players"
            );
        CHECK(a.match->integrity.reported);
    }
}

// A report request makes every machine say what it runs.
void integrity_report_request_is_answered() {
    start_case("integrity_report_request_is_answered");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0, channel_rules());
    SeatedMachine b(b_view, 1, channel_rules());
    join(a, b);
    oa::base::text::copy_padded(b.player(1).name, "Bravo", sizeof b.player(1).name);
    std::snprintf(b.match->recorder.program, sizeof b.match->recorder.program, "Program 1.0");
    Bytes request(private_record_bytes, 0);
    request[0] = 0x05;
    request[2] = private_sub_integrity;
    request[3] = 0x41;
    request[5] = 7;
    deliver_bytes(a, b, kA, request);
    const auto lines = b.sent(RecordType::chat);
    CHECK(lines.size() == 1);
    if (lines.size() == 1)
        CHECK(
            std::string(reinterpret_cast<const char*>(lines[0].bytes.data()) + 1) ==
            "*** Bravo uses Program 1.0"
        );
}

// The vote tally: a manual vote needs two thirds of the players, a timeout
// vote two; an ally of the target must agree; too many noes fail it; an
// undecided timeout vote rejects and an undecided manual one fails and
// keeps its target from a new vote for a while.
void vote_tally_rules() {
    start_case("vote_tally_rules");
    VoteBoard board{};
    auto* vote = vote_open(board, 50, vote_flag_manual, 100, 60 * 30);
    CHECK(vote != nullptr);
    if (vote == nullptr)
        return;
    CHECK(vote_open(board, 50, vote_flag_manual, 100, 60 * 30) == nullptr);
    VoteElectorate four{4, 0};
    CHECK(vote_needed(*vote, 4) == 3);
    vote_cast(*vote, 0, true);
    vote_cast(*vote, 1, true);
    CHECK(vote_tally(*vote, four, 100) == VoteResult::open);
    vote_cast(*vote, 2, true);
    CHECK(vote_tally(*vote, four, 100) == VoteResult::passed);
    four.target_allies = 1u << 3;
    CHECK(vote_tally(*vote, four, 100) == VoteResult::open);
    vote_cast(*vote, 3, true);
    CHECK(vote_tally(*vote, four, 100) == VoteResult::passed);
    vote_cast(*vote, 3, false);
    vote_cast(*vote, 2, false);
    CHECK(vote_tally(*vote, {4, 0}, 100) == VoteResult::failed);
    vote_close(board, *vote, VoteResult::failed, 100);
    CHECK(vote_find(board, 50) == nullptr);
    CHECK(vote_open(board, 50, vote_flag_manual, 200, 60 * 30) == nullptr);
    CHECK(vote_open(board, 50, vote_flag_manual, 100 + 90 * 30, 60 * 30) != nullptr);

    VoteBoard timeouts{};
    auto* timeout = vote_open(timeouts, 60, vote_flag_timeout, 0, 90 * 30);
    CHECK(timeout != nullptr && vote_needed(*timeout, 5) == 2 && vote_needed(*timeout, 1) == 1);
    if (timeout == nullptr)
        return;
    CHECK(vote_seconds_left(*timeout, 30 * 46) == 44);
    CHECK(vote_tally(*timeout, {5, 0}, 10) == VoteResult::open);
    CHECK(vote_tally(*timeout, {5, 0}, 90 * 30) == VoteResult::passed);
    vote_close(timeouts, *timeout, VoteResult::passed, 90 * 30);
    // A passed timeout vote leaves no cooldown.
    CHECK(vote_open(timeouts, 60, vote_flag_manual, 90 * 30 + 1, 60 * 30) != nullptr);
    auto* manual = vote_find(timeouts, 60);
    CHECK(
        manual != nullptr &&
        vote_tally(*manual, {5, 0}, 90 * 30 + 1 + 60 * 30) == VoteResult::failed
    );
}

// A proposal from one machine opens the vote on the other with the
// proposer's yes; the other's yes passes it, and each machine rejects the
// player itself.
void votes_reject_on_every_machine() {
    start_case("votes_reject_on_every_machine");
    constexpr uint32_t kA = 7, kB = 9, kC = 11;
    const Seat a_view[4] = {
        {kA, OA_PLAYER_STATUS_LOCAL, 1},
        {kB, OA_PLAYER_STATUS_MIRRORED, 2},
        {kC, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    const Seat b_view[4] = {
        {kA, OA_PLAYER_STATUS_MIRRORED, 1},
        {kB, OA_PLAYER_STATUS_LOCAL, 2},
        {kC, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    SeatedMachine a(a_view, 0, channel_rules());
    SeatedMachine b(b_view, 1, channel_rules());
    join(a, b);
    // A removal under the rule asks for a vote instead.
    net_match_remove_player(a.match.get(), 2, 1);
    CHECK(a.sent(RecordType::reject).empty());
    const auto* opened = vote_find(a.match->votes, kC);
    CHECK(opened != nullptr && opened->flag == vote_flag_manual && opened->yes == 1u);
    CHECK(!net_match_propose_reject(a.match.get(), 2));
    // A name that holds a later field's braces shows as it is.
    std::snprintf(b.player(2).name, sizeof b.player(2).name, "%s", "{no}{seconds}");
    (void)net_match_pump(b.match.get());
    const auto* heard = vote_find(b.match->votes, kC);
    CHECK(heard != nullptr && heard->yes == 1u);
    CHECK(
        !b.notices.empty() &&
        b.notices.back().rfind("Vote: reject {no}{seconds} (1 yes/0 no/3, ", 0) == 0
    );
    CHECK(net_match_cast_vote(b.match.get(), kC, true));
    // Two of three: passed on the voter's machine at once.
    CHECK(vote_find(b.match->votes, kC) == nullptr);
    const auto b_rejects = b.sent(RecordType::reject);
    CHECK(b_rejects.size() == 1 && b_rejects[0].bytes[1] == kC);
    (void)net_match_pump(a.match.get());
    CHECK(vote_find(a.match->votes, kC) == nullptr);
    CHECK(a.player(2).reject_reason == vote_flag_manual);
    CHECK(!net_match_cast_vote(a.match.get(), kC, true));
    // Without the rule, removal rejects at once.
    SeatedMachine plain(a_view, 0);
    net_match_remove_player(plain.match.get(), 2, 1);
    CHECK(plain.sent(RecordType::reject).size() == 1);
}

// A silent player gets a timeout vote from every machine instead of the
// host's drop, and hearing from it again cancels the vote.
void silence_opens_a_timeout_vote() {
    start_case("silence_opens_a_timeout_vote");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0, channel_rules());
    SeatedMachine b(b_view, 1, channel_rules());
    join(a, b);
    // The connection clock stays at 0 without a socket host: a player last
    // heard far in the past counts as silent.
    a.world->game.player_timeout_seconds = 1;
    a.player(1).last_update_time = 0u - 1000u;
    a.match->timeout_baseline = 0u - 1000u;
    (void)net_match_pump(a.match.get());
    CHECK(a.match->timeout_player == kB);
    const auto* vote = vote_find(a.match->votes, kB);
    CHECK(vote != nullptr && vote->flag == vote_flag_timeout && vote->yes == 0);
    CHECK(!net_match_timeout_expired(a.match.get(), kB));
    const auto proposals = a.sent(RecordType::chat);
    CHECK(proposals.size() == 1 && proposals[0].bytes[2] == private_sub_vote);
    CHECK(proposals.size() == 1 && proposals[0].bytes[10] == vote_flag_timeout);
    // The silent machine ignores a timeout vote on its own player.
    (void)net_match_pump(b.match.get());
    CHECK(vote_find(b.match->votes, kB) == nullptr);
    // Heard from again, the vote is dropped.
    const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
    CHECK(net_match_send(b.match.get(), kB, broadcast_destination_id, &probe, 1));
    packet_layer_flush(b.connection.packets, 0, true);
    (void)net_match_pump(a.match.get());
    CHECK(vote_find(a.match->votes, kB) == nullptr);
}

// The silent player does not count among the voters of its timeout vote:
// between two machines the one still answering rejects it with its own yes.
void timeout_vote_leaves_out_its_target() {
    start_case("timeout_vote_leaves_out_its_target");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0, channel_rules());
    SeatedMachine b(b_view, 1, channel_rules());
    join(a, b);
    a.world->game.player_timeout_seconds = 1;
    a.player(1).last_update_time = 0u - 1000u;
    a.match->timeout_baseline = 0u - 1000u;
    (void)net_match_pump(a.match.get());
    CHECK(vote_find(a.match->votes, kB) != nullptr);
    CHECK(net_match_cast_vote(a.match.get(), kB, true));
    CHECK(vote_find(a.match->votes, kB) == nullptr);
    const auto rejects = a.sent(RecordType::reject);
    CHECK(rejects.size() == 1 && rejects[0].bytes[1] == kB);
}

// An in-game setup block keeps a remote player's colour under the rule.
void remote_colour_survives_in_game_blocks() {
    start_case("remote_colour_survives_in_game_blocks");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    for (const bool keep : {true, false}) {
        WireRules rules{};
        rules.keep_remote_colour = keep;
        SeatedMachine a(a_view, 0, rules);
        SeatedMachine b(b_view, 1, rules);
        join(a, b);
        b.info(0).color = 2;
        a.info(0).color = 5;
        a.info(0).side = 1;
        net_match_send_player_status(a.match.get(), false);
        (void)net_match_pump(b.match.get());
        CHECK(b.info(0).color == (keep ? 2 : 5));
        CHECK(b.info(0).side == 1);
    }
}

// Under the recorder's rules its records reach the match: cameras are kept
// and warp-done counted. A game held for its warps starts once every
// player's warp is done; the in-game setup blocks say no recorder, as a
// recorder's do, and the protocol learnt in the battle room stays.
void recorder_records_reach_the_match() {
    start_case("recorder_records_reach_the_match");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0, recorder_rules());
    SeatedMachine b(b_view, 1, recorder_rules());
    join(a, b);
    a.match->recorder.peer_protocol[1] = recorder_protocol_current;
    b.match->recorder.peer_protocol[0] = recorder_protocol_current;
    b.match->recorder.options.commander_warp = 1;
    net_match_recorder_start(b.match.get());
    CHECK((b.world->game.sim_run_flags & run_flag_paused) != 0);
    CHECK(b.match->recorder.start_paused && !b.match->recorder.autopause_holding);
    net_match_warp_done(b.match.get());
    CHECK(b.match->recorder.warp_done[1]);
    net_match_paused_frame(b.match.get());
    CHECK(!b.match->recorder.warp_released);
    net_match_warp_done(a.match.get());
    const auto warps = a.sent(static_cast<RecordType>(0xfb));
    (void)warps;
    net_match_paused_frame(b.match.get());
    CHECK(b.match->recorder.warp_done[0]);
    CHECK(b.match->recorder.warp_released);
    CHECK((b.world->game.sim_run_flags & run_flag_paused) == 0);
    CHECK(b.match->records_refused == 0);

    net_match_send_camera(a.match.get(), 0x0100, 0x00d0);
    packet_layer_flush(a.connection.packets, 0, true);
    (void)net_match_pump(b.match.get());
    CHECK(b.match->recorder.camera_x[0] == 0x0100 && b.match->recorder.camera_y[0] == 0x00d0);
    CHECK(b.match->recorder_records == 2);
    bool camera_seen = false;
    for (const auto& [sender, type] : b.seen)
        camera_seen = camera_seen || (sender == kA && type == 0xfc);
    CHECK(camera_seen);

    a.info(0).recorder_protocol = recorder_protocol_current;
    b.info(0).recorder_protocol = recorder_protocol_current;
    a.forget_sent();
    net_match_send_player_status(a.match.get(), false);
    const auto blocks = a.sent(RecordType::player_info);
    CHECK(blocks.size() == 1 && blocks[0].bytes[1 + player_info_recorder_protocol_offset] == 0);
    (void)net_match_pump(b.match.get());
    CHECK(
        reinterpret_cast<const uint8_t*>(&b.info(0))[player_info_recorder_protocol_offset] ==
        recorder_protocol_current
    );

    // Under 3.1c's rules a frame stops at a recorder record, as 3.1c's does.
    SeatedMachine c(a_view, 0);
    SeatedMachine d(b_view, 1);
    join(c, d);
    const Bytes camera{0xfc, 0x00, 0x01, 0xd0, 0x00};
    CHECK(
        net_match_send(c.match.get(), kA, broadcast_destination_id, camera.data(), camera.size())
    );
    packet_layer_flush(c.connection.packets, 0, true);
    (void)net_match_pump(d.match.get());
    CHECK(d.match->recorder_records == 0 && d.match->recorder.camera_x[0] == 0);
}

// Under the recorder's rules this machine's camera goes out with its frames
// when it moves, at most once each send interval, and the receiver keeps
// who shares. .sharemappos turns sharing off with a camera record whose
// halves are both off, in a frame of its own, and back on; only the local
// player's command counts. Under 3.1c's rules no camera goes out.
// The camera records a machine sent, read past the recorder's records as a
// recorder reads its frames.
std::vector<SentRecord> cameras_sent(SeatedMachine& m) {
    packet_layer_flush(m.connection.packets, 0, true);
    std::vector<SentRecord> out;
    for (const auto& datagram : m.link.sent) {
        const auto frame = oa::netgame::network::decode_frame(datagram.bytes);
        for (std::size_t at = frame_header_bytes; at < frame.size();) {
            uint16_t length = 0;
            if (recorder_record_length(frame.data() + at, frame.size() - at, &length) !=
                    WireError::ok &&
                record_wire_length(frame.data() + at, frame.size() - at, &length) != WireError::ok)
                break;
            if (length == 0 || at + length > frame.size())
                break;
            if (frame[at] == 0xfc)
                out.push_back(
                    {datagram.from,
                     datagram.to,
                     std::vector<uint8_t>(frame.begin() + at, frame.begin() + at + length)}
                );
            at += length;
        }
    }
    return out;
}

void recorder_cameras_are_shared() {
    start_case("recorder_cameras_are_shared");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0, recorder_rules());
    SeatedMachine b(b_view, 1, recorder_rules());
    join(a, b);
    // No camera noted yet: none goes out.
    a.forget_sent();
    net_match_after_tick(a.match.get());
    CHECK(cameras_sent(a).empty());
    net_match_note_camera(a.match.get(), 0x0100, 0x00d0);
    net_match_after_tick(a.match.get());
    auto cameras = cameras_sent(a);
    CHECK(cameras.size() == 1 && (cameras[0].bytes == Bytes{0xfc, 0x00, 0x01, 0xd0, 0x00}));
    CHECK(cameras.size() == 1 && cameras[0].to == broadcast_destination_id);
    (void)net_match_pump(b.match.get());
    CHECK(b.match->recorder.camera_shared[0]);
    CHECK(b.match->recorder.camera_x[0] == 0x0100 && b.match->recorder.camera_y[0] == 0x00d0);
    // The same place again, or a move within the send interval: nothing more.
    net_match_after_tick(a.match.get());
    CHECK(cameras_sent(a).size() == 1);
    net_match_note_camera(a.match.get(), 0x0200, 0x0300);
    net_match_after_tick(a.match.get());
    CHECK(cameras_sent(a).size() == 1);
    // Once the interval has passed the move goes out; places off the range
    // are clamped, short of the off value.
    a.connection.packets->ticks_between_sends = 0;
    net_match_note_camera(a.match.get(), -5, 0x12345);
    net_match_after_tick(a.match.get());
    cameras = cameras_sent(a);
    CHECK(cameras.size() == 2 && (cameras[1].bytes == Bytes{0xfc, 0x00, 0x00, 0xfe, 0xff}));

    // .sharemappos turns sharing off: the off record goes in a frame alone.
    a.forget_sent();
    net_match_say(a.match.get(), "<Alpha> .sharemappos");
    CHECK(a.match->recorder.local_camera_hidden);
    cameras = cameras_sent(a);
    CHECK(cameras.size() == 1 && (cameras[0].bytes == Bytes{0xfc, 0xff, 0xff, 0xff, 0xff}));
    bool alone = false;
    for (const auto& datagram : a.link.sent) {
        const auto frame = oa::netgame::network::decode_frame(datagram.bytes);
        if (frame.size() > frame_header_bytes && frame[frame_header_bytes] == 0xfc)
            alone = frame.size() == frame_header_bytes + recorder_camera_bytes;
    }
    CHECK(alone);
    (void)net_match_pump(b.match.get());
    CHECK(!b.match->recorder.camera_shared[0]);
    // While it is off the camera stays here.
    net_match_note_camera(a.match.get(), 0x0400, 0x0400);
    net_match_after_tick(a.match.get());
    CHECK(cameras_sent(a).size() == 1);
    // Typed again, the camera goes out with the next frame.
    net_match_say(a.match.get(), "<Alpha> .sharemappos");
    CHECK(!a.match->recorder.local_camera_hidden);
    net_match_after_tick(a.match.get());
    cameras = cameras_sent(a);
    CHECK(cameras.size() == 2 && (cameras[1].bytes == Bytes{0xfc, 0x00, 0x04, 0x00, 0x04}));
    (void)net_match_pump(b.match.get());
    CHECK(b.match->recorder.camera_shared[0] && b.match->recorder.camera_x[0] == 0x0400);
    // Another player's .sharemappos leaves this machine's sharing as it was.
    ChatRecord line{};
    std::snprintf(line.text, sizeof line.text, "%s", "<Bravo> .sharemappos");
    deliver(b, a, kB, broadcast_destination_id, line);
    CHECK(!a.match->recorder.local_camera_hidden);

    SeatedMachine c(a_view, 0);
    c.forget_sent();
    net_match_note_camera(c.match.get(), 1, 1);
    net_match_after_tick(c.match.get());
    CHECK(cameras_sent(c).empty());
}

// ui.whiteboard over the recorder: a batch of marks goes as message kind 0,
// `fb n 00` and the batch, alone in a frame of its own (sequence -1) to
// each recorder of protocol 2 or later, never to a plain peer or one of
// protocol 1. The other machine shows it only while the sender allies its
// player; a payload above 100 bytes is shown nowhere.
void whiteboard_marks_reach_allies() {
    start_case("whiteboard_marks_reach_allies");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0, recorder_rules());
    SeatedMachine b(b_view, 1, recorder_rules());
    join(a, b);
    a.match->recorder.peer_protocol[1] = recorder_protocol_current;
    b.match->recorder.peer_protocol[0] = recorder_protocol_current;
    // One dot marker: count 1, then type 3, pad, x 0x0140, y 0x0280, colour 0xe3, empty text.
    const Bytes dot{0x01, 0x03, 0x00, 0x40, 0x01, 0x80, 0x02, 0xe3, 0x00};
    Bytes record{0xfb, static_cast<uint8_t>(dot.size()), 0x00};
    record.insert(record.end(), dot.begin(), dot.end());

    a.forget_sent();
    CHECK(net_match_send_whiteboard(a.match.get(), dot.data(), dot.size()) == 1);
    Bytes alone{0xff, 0xff, 0xff, 0xff};
    alone.insert(alone.end(), record.begin(), record.end());
    // The 0xfb frames Alpha sent, as the other machine would read them.
    const auto whiteboard_frames = [](const Link& link) {
        std::vector<Datagram> out;
        for (const auto& datagram : link.sent) {
            const auto frame = oa::netgame::network::decode_frame(datagram.bytes);
            if (frame.size() > frame_header_bytes && frame[frame_header_bytes] == 0xfb)
                out.push_back({datagram.from, datagram.to, frame});
        }
        return out;
    };
    packet_layer_flush(a.connection.packets, 0, true);
    const auto sent = whiteboard_frames(a.link);
    CHECK(sent.size() == 1);
    CHECK(sent[0].from == kA && sent[0].to == kB && sent[0].bytes == alone);
    // Bravo's player is not allied by Alpha's: nothing is shown.
    (void)net_match_pump(b.match.get());
    CHECK(b.marks.empty() && b.match->whiteboard_batches == 0);
    CHECK(b.match->records_refused == 0);

    // Once Alpha's player allies Bravo's, the batch is shown as sent.
    b.player(1).allied_by[0] = 1;
    CHECK(net_match_send_whiteboard(a.match.get(), dot.data(), dot.size()) == 1);
    packet_layer_flush(a.connection.packets, 0, true);
    (void)net_match_pump(b.match.get());
    CHECK(b.marks.size() == 1);
    CHECK(b.marks[0].first == 0 && b.marks[0].second == dot);
    CHECK(b.match->whiteboard_batches == 1);

    // A payload past 100 bytes is not shown.
    Bytes oversized{0xfb, 101, 0x00};
    oversized.resize(oversized.size() + 101, 0x00);
    oversized[3] = 0x01;
    const auto read_before = b.match->recorder_records;
    deliver_bytes(a, b, kA, oversized);
    CHECK(b.match->recorder_records == read_before + 1);
    CHECK(b.marks.size() == 1);

    // Nothing goes to a recorder of protocol 1, nor a batch past 100 bytes,
    // nor from a machine that presents no recorder.
    a.match->recorder.peer_protocol[1] = 1;
    CHECK(net_match_send_whiteboard(a.match.get(), dot.data(), dot.size()) == 0);
    a.match->recorder.peer_protocol[1] = recorder_protocol_current;
    const Bytes long_batch(101, 0x00);
    CHECK(net_match_send_whiteboard(a.match.get(), long_batch.data(), long_batch.size()) == 0);
    CHECK(net_match_send_whiteboard(a.match.get(), nullptr, 0) == 0);
    SeatedMachine plain(a_view, 0);
    plain.match->recorder.peer_protocol[1] = recorder_protocol_current;
    CHECK(net_match_send_whiteboard(plain.match.get(), dot.data(), dot.size()) == 0);
    packet_layer_flush(plain.connection.packets, 0, true);
    CHECK(whiteboard_frames(plain.link).empty());
}

/// Installs Simplified Chinese for one test, then puts the compiled
/// available entry back.
struct InstalledSimplifiedChinese {
    InstalledSimplifiedChinese() {
        oa::data::languages::LanguageEntry entry;
        entry.tag = "zh-Hans";
        entry.endonym = "\347\256\200\344\275\223\344\270\255\346\226\207";
        entry.english_name = "Chinese (Simplified)";
        entry.word = "Chinese";
        entry.locales = {"zh-Hans", "zh-CN", "zh-SG", "zh-MY", "zh"};
        entry.needs = oa::data::languages::TextNeeds::modern_fonts;
        const std::array<oa::data::languages::LanguageEntry, 1> installed{entry};
        oa::data::languages::set_pack_languages(installed, {});
    }

    ~InstalledSimplifiedChinese() { oa::data::languages::set_pack_languages({}, {}); }

    InstalledSimplifiedChinese(const InstalledSimplifiedChinese&) = delete;
    InstalledSimplifiedChinese& operator=(const InstalledSimplifiedChinese&) = delete;
};

// Under autopause only the host's unpause starts the game: another's is
// undone with a notice, here and from another machine.
void autopause_holds_for_the_host() {
    [[maybe_unused]] const InstalledSimplifiedChinese chinese_pack;
    start_case("autopause_holds_for_the_host");
    constexpr uint32_t kHost = 7, kB = 9, kC = 11;
    const Seat b_view[4] = {
        {kHost, OA_PLAYER_STATUS_MIRRORED, 1},
        {kB, OA_PLAYER_STATUS_LOCAL, 2},
        {kC, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    const Seat c_view[4] = {
        {kHost, OA_PLAYER_STATUS_MIRRORED, 1},
        {kB, OA_PLAYER_STATUS_MIRRORED, 2},
        {kC, OA_PLAYER_STATUS_LOCAL, 3}
    };
    const Seat host_view[4] = {
        {kHost, OA_PLAYER_STATUS_LOCAL, 1},
        {kB, OA_PLAYER_STATUS_MIRRORED, 2},
        {kC, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    SeatedMachine b(b_view, 1, recorder_rules());
    SeatedMachine c(c_view, 2, recorder_rules());
    SeatedMachine host(host_view, 0, recorder_rules());
    for (auto* m : {&b, &c, &host})
        m->info(0).role = 1;
    b.match->recorder.options.autopause = 1;
    c.match->recorder.options.autopause = 1;
    net_match_recorder_start(b.match.get());
    CHECK(b.match->recorder.autopause_holding);
    // The local unpause of a machine that does not host.
    b.world->game.sim_run_flags =
        static_cast<uint16_t>(b.world->game.sim_run_flags & ~run_flag_paused);
    b.forget_sent();
    net_match_set_pause(b.match.get(), false);
    CHECK((b.world->game.sim_run_flags & run_flag_paused) != 0);
    CHECK(b.sent(RecordType::pause_speed).empty());
    // Another player's unpause arrives and is undone.
    join(c, b);
    PauseSpeedRecord unpause{};
    deliver(c, b, kC, broadcast_destination_id, unpause);
    CHECK((b.world->game.sim_run_flags & run_flag_paused) != 0);
    CHECK(!b.notices.empty() && b.notices.back().find("tried to unpause") != std::string::npos);
    // The host's unpause starts it, and pauses after it are free.
    join(host, b);
    deliver(host, b, kHost, broadcast_destination_id, unpause);
    CHECK((b.world->game.sim_run_flags & run_flag_paused) == 0);
    CHECK(!b.match->recorder.autopause_holding);
}

// Each machine builds the notices about another machine from the game's own
// texts in the language it shows, as 3.1c builds them where it shows them.
// A received speed reads as the speed keys' own line; the disconnection
// line puts the name where its translation's %s stands, or leaves it out;
// the integrity line puts the name first. The engine's own words, as the
// autopause lines, come from the interface catalogue.
void notices_follow_the_language() {
    start_case("notices_follow_the_language");
    constexpr uint32_t kSender = 7, kLocal = 9, kThird = 11;
    const Seat a_view[4] = {
        {kSender, OA_PLAYER_STATUS_LOCAL, 1},
        {kLocal, OA_PLAYER_STATUS_MIRRORED, 2},
        {kThird, OA_PLAYER_STATUS_MIRRORED, 3},
        {}
    };
    const Seat b_view[4] = {
        {kSender, OA_PLAYER_STATUS_MIRRORED, 1},
        {kLocal, OA_PLAYER_STATUS_LOCAL, 2},
        {kThird, OA_PLAYER_STATUS_MIRRORED, 3},
        {}
    };
    {
        SeatedMachine a(a_view, 0);
        SeatedMachine b(b_view, 1);
        join(a, b);
        PauseSpeedRecord speed{};
        speed.kind = 1;
        speed.value = 11;
        deliver(a, b, kSender, broadcast_destination_id, speed);
        CHECK(!b.notices.empty() && b.notices.back() == "Game Speed  +1\n");
        speed.value = 7;
        deliver(a, b, kSender, broadcast_destination_id, speed);
        CHECK(!b.notices.empty() && b.notices.back() == "Game Speed   -3\n");
        b.translations = {{"Game Speed", "Tempo"}, {"Game Speed Normal", "Tempo Normal"}};
        speed.value = 12;
        deliver(a, b, kSender, broadcast_destination_id, speed);
        CHECK(!b.notices.empty() && b.notices.back() == "Tempo  +2\n");
        speed.value = 10;
        deliver(a, b, kSender, broadcast_destination_id, speed);
        CHECK(!b.notices.empty() && b.notices.back() == "Tempo Normal");
    }
    // The last notice a machine shows for a record about the third player,
    // with the third player's line translated as given (null: none).
    const auto shown_for = [&](const auto& record, const char* english, const char* translation) {
        SeatedMachine a(a_view, 0);
        SeatedMachine b(b_view, 1);
        join(a, b);
        std::snprintf(b.player(2).name, sizeof b.player(2).name, "%s", "Third");
        if (translation != nullptr)
            b.translations[english] = translation;
        deliver(a, b, kSender, broadcast_destination_id, record);
        return b.notices.empty() ? std::string{} : b.notices.back();
    };
    DisconnectNoticeRecord left{};
    left.player_id = kThird;
    const char* disconnected = "Player %s has disconnected";
    CHECK(shown_for(left, disconnected, nullptr) == "Player Third has disconnected");
    // 3.1c's German line has no place for the name, and shows none.
    CHECK(
        shown_for(left, disconnected, "Spieler hat die Verbindung gelöst") ==
        "Spieler hat die Verbindung gelöst"
    );
    CHECK(
        shown_for(left, disconnected, "Il giocatore %s si è scollegato") ==
        "Il giocatore Third si è scollegato"
    );
    // The translation is never read as a format: "%%" shows one '%', the
    // first "%s" the name, and any other sequence as it is written.
    CHECK(shown_for(left, disconnected, "100%% %s %d %s") == "100% Third %d %s");
    IntegrityNoticeRecord modified{};
    modified.player_id = kThird;
    const char* breached = "has modified his executable.  Game integrity breached.";
    CHECK(
        shown_for(modified, breached, nullptr) ==
        "Third has modified his executable.  Game integrity breached."
    );
    CHECK(shown_for(modified, breached, "修改了游戏程序") == "Third 修改了游戏程序");

    // The autopause lines in Simplified Chinese, with the name filled in.
    oa::data::languages::InterfaceText catalogue;
    CHECK(catalogue.add(
        "[Autopause: only the host can unpause]\n{\nzh-Hans=自动暂停：只有主持者可以继续游戏;\n}\n"
        "[{player} tried to unpause.]\n{\nzh-Hans={player}试图继续游戏。;\n}\n"
    ));
    const auto* chinese = oa::data::languages::find_by_tag("zh-Hans");
    CHECK(chinese != nullptr);
    if (chinese == nullptr)
        return;
    oa::data::languages::set_interface_language(&catalogue, *chinese);
    constexpr uint32_t kHost = 7, kB = 9, kC = 11;
    const Seat b_seats[4] = {
        {kHost, OA_PLAYER_STATUS_MIRRORED, 1},
        {kB, OA_PLAYER_STATUS_LOCAL, 2},
        {kC, OA_PLAYER_STATUS_MIRRORED, 3}
    };
    const Seat c_seats[4] = {
        {kHost, OA_PLAYER_STATUS_MIRRORED, 1},
        {kB, OA_PLAYER_STATUS_MIRRORED, 2},
        {kC, OA_PLAYER_STATUS_LOCAL, 3}
    };
    SeatedMachine b(b_seats, 1, recorder_rules());
    SeatedMachine c(c_seats, 2, recorder_rules());
    for (auto* m : {&b, &c})
        m->info(0).role = 1;
    std::snprintf(b.player(2).name, sizeof b.player(2).name, "%s", "Gamma");
    b.match->recorder.options.autopause = 1;
    net_match_recorder_start(b.match.get());
    b.world->game.sim_run_flags =
        static_cast<uint16_t>(b.world->game.sim_run_flags & ~run_flag_paused);
    net_match_set_pause(b.match.get(), false);
    CHECK(!b.notices.empty() && b.notices.back() == "自动暂停：只有主持者可以继续游戏");
    join(c, b);
    PauseSpeedRecord unpause{};
    deliver(c, b, kC, broadcast_destination_id, unpause);
    CHECK(!b.notices.empty() && b.notices.back() == "Gamma试图继续游戏。");
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
}

// The recorder's speed lock removes received speeds outside it; speed 0 is
// a speed when the rules allow it.
void speed_lock_and_speed_zero() {
    start_case("speed_lock_and_speed_zero");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0, recorder_rules());
    SeatedMachine b(b_view, 1, recorder_rules());
    join(a, b);
    PauseSpeedRecord speed{};
    speed.kind = 1;
    speed.value = 0;
    deliver(a, b, kA, broadcast_destination_id, speed);
    CHECK(b.world->game.requested_speed == 0);
    b.match->recorder.options.speed_lock = 1;
    b.match->recorder.options.speed_low = 0;
    b.match->recorder.options.speed_high = 5;
    speed.value = 9;
    deliver(a, b, kA, broadcast_destination_id, speed);
    CHECK(b.world->game.requested_speed == 0);
    speed.value = 12;
    deliver(a, b, kA, broadcast_destination_id, speed);
    CHECK(b.world->game.requested_speed == 12);
    // A local change outside the lock is not made nor sent.
    b.forget_sent();
    net_match_set_speed(b.match.get(), 16, true);
    CHECK(b.world->game.requested_speed == 12 && b.sent(RecordType::pause_speed).empty());
    // 3.1c's range is 1..20.
    SeatedMachine c(a_view, 0);
    SeatedMachine d(b_view, 1);
    join(c, d);
    speed.value = 0;
    deliver(c, d, kA, broadcast_destination_id, speed);
    CHECK(d.world->game.requested_speed == 1);
}

// The host's .syncon narrows every machine's speeds and a speed outside the
// lock is set to its nearest end and sent; .syncoff lifts it. Without the
// rules' speed lock both lines are only chat.
void hosts_speed_lock_narrows_the_range() {
    start_case("hosts_speed_lock_narrows_the_range");
    constexpr uint32_t kHost = 7, kB = 9;
    const Seat host_view[4] = {
        {kHost, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}
    };
    const Seat b_view[4] = {{kHost, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    for (const bool rule : {false, true}) {
        auto rules = recorder_rules();
        rules.speed_lock = rule;
        SeatedMachine host(host_view, 0, rules);
        SeatedMachine b(b_view, 1, rules);
        for (auto* m : {&host, &b})
            m->info(0).role = 1;
        join(host, b);
        host.world->game.requested_speed = 10;
        b.world->game.requested_speed = 18;
        b.forget_sent();
        net_match_say(host.match.get(), "<Host> .syncon 0 5");
        packet_layer_flush(host.connection.packets, 0, true);
        (void)net_match_pump(b.match.get());
        PauseSpeedRecord speed{};
        speed.kind = 1;
        if (!rule) {
            CHECK(b.match->recorder.options.speed_lock == 0 && b.speed_locks.empty());
            CHECK(b.world->game.requested_speed == 18 && b.sent(RecordType::pause_speed).empty());
            uint8_t slowest = 0;
            uint8_t fastest = 0;
            CHECK(!net_match_speed_range(b.match.get(), &slowest, &fastest));
            CHECK(slowest == 0 && fastest == 20);
            speed.value = 3;
            deliver(host, b, kHost, broadcast_destination_id, speed);
            CHECK(b.world->game.requested_speed == 3);
            continue;
        }
        CHECK(b.match->recorder.options.speed_lock == 1);
        CHECK((b.speed_locks == std::vector<std::array<int32_t, 3>>{{1, 10, 15}}));
        CHECK((host.speed_locks == std::vector<std::array<int32_t, 3>>{{1, 10, 15}}));
        // The host's speed lay within it: the host sends none.
        CHECK(host.world->game.requested_speed == 10);
        // b's speed lay above the lock: it is set to 15 and sent.
        CHECK(b.world->game.requested_speed == 15);
        const auto sent = b.sent(RecordType::pause_speed);
        CHECK(sent.size() == 1);
        if (sent.size() == 1)
            CHECK(sent[0].from == kB && (sent[0].bytes == std::vector<uint8_t>{0x19, 1, 15}));
        // Speeds outside the lock are neither taken nor made.
        speed.value = 9;
        deliver(host, b, kHost, broadcast_destination_id, speed);
        CHECK(b.world->game.requested_speed == 15);
        b.forget_sent();
        net_match_set_speed(b.match.get(), 16, true);
        CHECK(b.world->game.requested_speed == 15 && b.sent(RecordType::pause_speed).empty());
        speed.value = 12;
        deliver(host, b, kHost, broadcast_destination_id, speed);
        CHECK(b.world->game.requested_speed == 12);
        // Another player's .syncoff is only chat; the host's lifts the lock.
        b.forget_sent();
        net_match_say(b.match.get(), "<B> .syncoff");
        CHECK(b.match->recorder.options.speed_lock == 1 && b.speed_locks.size() == 1);
        net_match_say(host.match.get(), "<Host> .syncoff");
        packet_layer_flush(host.connection.packets, 0, true);
        (void)net_match_pump(b.match.get());
        CHECK(b.match->recorder.options.speed_lock == 0);
        CHECK(
            b.speed_locks.size() == 2 && (b.speed_locks.back() == std::array<int32_t, 3>{0, 0, 20})
        );
        speed.value = 19;
        deliver(host, b, kHost, broadcast_destination_id, speed);
        CHECK(b.world->game.requested_speed == 19);
    }
}

// A remote player's start sync puts its commander where the record says
// while the game is younger than the unit limit, whatever unit the record
// names; the fractions stay.
void commander_start_sync_places_the_commander() {
    start_case("commander_start_sync_places_the_commander");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    for (const bool rule : {true, false}) {
        WireRules rules{};
        rules.commander_sync_tick = rule ? 90 : 0;
        SeatedMachine a(a_view, 0, rules);
        SeatedMachine b(b_view, 1, rules);
        join(a, b);
        b.world->game.units_per_player = 10;
        b.world->game.unit_def_id_bits = 10;
        b.world->game.tick = 5;
        b.player(0).first_unit = oa_unit_ref_from_slot(1);
        b.player(0).last_unit = oa_unit_ref_from_slot(10);
        auto& commander = b.world->units[1];
        commander.position.x = (100 << 16) | 0x8000;
        commander.position.z = (200 << 16) | 0x4000;
        uint8_t storage[unit_state_writer_words * bit_stream_word_bytes];
        BitWriter writer;
        bit_writer_init(&writer, storage, unit_state_writer_words);
        uint16_t length = 0;
        CHECK(
            unit_state_write_start_position(&writer, 90, {54, 9072, 992, 9072, 992}, 10, &length) ==
            WireError::ok
        );
        deliver_bytes(a, b, kA, Bytes(storage, storage + length));
        if (rule) {
            CHECK(commander.position.x == ((9072 << 16) | 0x8000));
            CHECK(commander.position.z == ((992 << 16) | 0x4000));
            CHECK(b.match->commander_syncs_applied == 1);
        } else {
            CHECK(commander.position.x == ((100 << 16) | 0x8000));
            CHECK(b.match->commander_syncs_applied == 0);
        }
    }
}

// A unit state record of one entry, as a start sync writes it.
Bytes moving_record() {
    uint8_t storage[unit_state_writer_words * bit_stream_word_bytes];
    BitWriter writer;
    bit_writer_init(&writer, storage, unit_state_writer_words);
    uint16_t length = 0;
    CHECK(
        unit_state_write_start_position(&writer, 3, {5, 1, 2, 1, 2}, 10, &length) == WireError::ok
    );
    return Bytes(storage, storage + length);
}

// The recorder's session commands in the match: .ready from every player
// still playing unpauses, .fakewatch makes the typer's records load reports
// and empty unit states five seconds on, and a cheat mask is announced.
void recorder_session_commands_in_the_match() {
    start_case("recorder_session_commands_in_the_match");
    constexpr uint32_t kA = 7, kB = 9;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0, recorder_rules());
    SeatedMachine b(b_view, 1, recorder_rules());
    join(a, b);
    for (auto* m : {&a, &b})
        for (uint8_t slot = 0; slot < 2; ++slot)
            m->player(slot).unit_count = 1;
    b.world->game.sim_run_flags =
        static_cast<uint16_t>(b.world->game.sim_run_flags | run_flag_paused);
    net_match_say(b.match.get(), "<Bravo> .ready");
    CHECK(b.match->recorder.ready[1]);
    CHECK((b.world->game.sim_run_flags & run_flag_paused) != 0);
    ChatRecord ready{};
    std::snprintf(ready.text, sizeof ready.text, "%s", "<Alpha> .ready");
    deliver(a, b, kA, broadcast_destination_id, ready);
    CHECK(b.match->recorder.ready[0]);
    CHECK((b.world->game.sim_run_flags & run_flag_paused) == 0);

    net_match_say(a.match.get(), "<Alpha> .fakewatch");
    CHECK(a.match->recorder.fake_watch);
    a.forget_sent();
    const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
    CHECK(net_match_send(a.match.get(), kA, broadcast_destination_id, &probe, 1));
    CHECK(a.sent(RecordType::probe).size() == 1);
    a.world->game.tick = a.match->recorder.fake_watch_tick;
    a.world->game.unit_def_id_bits = 10;
    a.forget_sent();
    CHECK(net_match_send(a.match.get(), kA, broadcast_destination_id, &probe, 1));
    CHECK(a.sent(RecordType::probe).empty());
    const auto reports = a.sent(RecordType::load_progress);
    CHECK(reports.size() == 1 && (reports[0].bytes == std::vector<uint8_t>{0x2a, 100}));
    const auto state = moving_record();
    CHECK(net_match_send(a.match.get(), kA, broadcast_destination_id, state.data(), state.size()));
    const auto states = a.sent(RecordType::unit_state);
    CHECK(states.size() == 1 && states[0].bytes.size() == 11);
    net_match_say(a.match.get(), "still talking");
    CHECK(!a.sent(RecordType::chat).empty());

    a.match->recorder.fake_watch = false;
    const Bytes cheats{0xfb, 0x04, 0x02, 0x01, 0x00, 0x00, 0x00};
    oa::base::text::copy_padded(b.player(0).name, "Alpha", sizeof b.player(0).name);
    deliver_bytes(a, b, kA, cheats);
    CHECK(b.match->recorder.cheat_mask[0] == 1);
    CHECK(!b.notices.empty() && b.notices.back() == "Alpha has cheats enabled");
}

// What the replication hooks of a seated machine were asked to do.
struct ReplicationLog {
    std::vector<UnitCreatedRecord> creates;
    std::vector<uint8_t> create_owners;
    std::vector<std::pair<uint16_t, uint8_t>> flags; // unit, mask switched on
    std::vector<UnitTransferRecord> transfers;
    std::vector<UnitDamageRecord> damage;
};

// Points a seated machine's replication hooks at a log; a created unit is
// live from then on, so a hand-over of it reaches transfer_unit.
void log_replication(SeatedMachine& m, ReplicationLog& log) {
    auto& sim = m.match->sim;
    sim.context = &log;
    sim.create_unit = [](void* c, World* world, uint8_t owner, const UnitCreatedRecord& r) {
        auto& log = *static_cast<ReplicationLog*>(c);
        log.creates.push_back(r);
        log.create_owners.push_back(owner);
        if (auto* unit = world_unit_at(world, r.unit_index)) {
            unit->flags |= OA_UNIT_FLAG_LIVE;
            unit->type_index = r.unit_def_index;
        }
    };
    sim.toggle_state_flags = [](void* c, World*, Unit* unit, uint8_t mask, bool on) {
        if (on && mask != 0)
            static_cast<ReplicationLog*>(c)->flags.emplace_back(unit->id, mask);
    };
    sim.transfer_unit = [](void* c, World*, Unit*, Player*, const UnitTransferRecord& r) {
        static_cast<ReplicationLog*>(c)->transfers.push_back(r);
    };
    sim.apply_damage = [](void* c, World*, const UnitDamageRecord& r) {
        static_cast<ReplicationLog*>(c)->damage.push_back(r);
    };
}

// Gives a seated machine's first two players blocks of ten units each, slots
// 1..10 and 11..20, with every unit's id its slot.
void give_unit_blocks(SeatedMachine& m) {
    auto* world = m.world;
    world->game.units_per_player = 10;
    world->game.unit_def_id_bits = 10;
    for (uint32_t slot = 0; slot < world->unit_slot_count; ++slot)
        world->units[slot].id = static_cast<uint16_t>(slot);
    for (uint8_t player = 0; player < 2; ++player) {
        m.player(player).first_unit = oa_unit_ref_from_slot(1u + 10u * player);
        m.player(player).last_unit = oa_unit_ref_from_slot(10u + 10u * player);
    }
}

bool chat_shown(const SeatedMachine& m, const std::string& line) {
    return std::find(m.chats.begin(), m.chats.end(), line) != m.chats.end();
}

// sharing.recorder-take-give: .give lets a named player take the giver's
// units and .stopgive takes it back; allying grants it too. A .take of a
// player silent for over 30 seconds hands its units that the last full
// records showed finished over, one place of its block each step the pump
// takes, with the health those records gave, and ends with a reject of it
// sent to every player. Another player's .take is a claim this machine
// notes in place of any other and releases at the next reject; one with no
// recorder has the empty slots of its block killed then, and any other has
// nothing killed.
void recorder_take_hands_a_silent_players_units_over() {
    start_case("recorder_take_hands_a_silent_players_units_over");
    constexpr uint32_t kA = 7, kB = 9;
    constexpr uint32_t kSlots = 10 * OA_PLAYER_COUNT + 1;
    const Seat a_view[4] = {{kA, OA_PLAYER_STATUS_LOCAL, 1}, {kB, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat b_view[4] = {{kA, OA_PLAYER_STATUS_MIRRORED, 1}, {kB, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine a(a_view, 0, recorder_rules(), kSlots);
    SeatedMachine b(b_view, 1, recorder_rules(), kSlots);
    join(a, b);
    oa::data::match_rules::MatchRules rules{};
    rules.sharing.recorder_take_give.enabled = true;
    rules.sharing.recorder_take_give.available = true;
    for (auto* m : {&a, &b}) {
        give_unit_blocks(*m);
        oa::base::text::copy_padded(m->player(0).name, "Alpha", sizeof m->player(0).name);
        oa::base::text::copy_padded(m->player(1).name, "Bravo", sizeof m->player(1).name);
        m->match->recorder.peer_protocol[0] = recorder_protocol_current;
        m->match->recorder.peer_protocol[1] = recorder_protocol_current;
        net_match_bind_rules(m->match.get(), &rules, false);
    }
    CHECK(a.match->recorder_units.size() == kSlots);
    ReplicationLog log;
    log_replication(a, log);
    ChatRecord line{};

    // Bravo lets Alpha take its units; Alpha's recorder says so.
    std::snprintf(line.text, sizeof line.text, "%s", "<Bravo> .give alpha");
    deliver(b, a, kB, broadcast_destination_id, line);
    CHECK(a.match->take.granted[1]);
    CHECK(chat_shown(a, "Alpha ready to take units from Bravo"));
    std::snprintf(line.text, sizeof line.text, "%s", "<Bravo> .stopgive Alpha");
    deliver(b, a, kB, broadcast_destination_id, line);
    CHECK(!a.match->take.granted[1]);
    CHECK(chat_shown(a, "Alpha barred from taking units from Bravo"));
    // An alliance with Alpha grants it as well, and .give then says so.
    AllianceRecord alliance{};
    alliance.player_id_a = kB;
    alliance.player_id_b = kA;
    alliance.value = 1;
    deliver(b, a, kB, broadcast_destination_id, alliance);
    CHECK(a.match->take.granted[1] && a.match->take.allied[1]);
    std::snprintf(line.text, sizeof line.text, "%s", "<Bravo> .give Alpha");
    deliver(b, a, kB, broadcast_destination_id, line);
    CHECK(chat_shown(a, "You don't really need to type .give. Allying is enough."));

    // Bravo's units as Bravo's full records last showed them.
    FullUnitRecord finished{};
    finished.unit_def_index = 3;
    finished.health = 900;
    net_match_note_full_record(a.match.get(), 11, finished); // the commander
    net_match_note_full_record(a.match.get(), 12, finished);
    FullUnitRecord building = finished;
    building.health = 300;
    building.build_byte = 0x40;
    net_match_note_full_record(a.match.get(), 13, building);
    finished.health = 650;
    net_match_note_full_record(a.match.get(), 15, finished);
    for (const uint32_t unit : {11u, 12u, 13u, 15u}) {
        a.world->units[unit].flags |= OA_UNIT_FLAG_LIVE;
        a.world->units[unit].type_index = 3;
    }

    // Bravo was heard from just now: nothing is taken.
    a.player(0).unit_count = 4;
    net_match_say(a.match.get(), "<Alpha> .take");
    CHECK(a.match->take.state == RecorderTakeState::idle);
    // Silent for over 30 seconds, it is.
    a.player(1).last_update_time = oa::base::game_loop::scaled_clock_turn - 1000;
    a.forget_sent();
    net_match_say(a.match.get(), "<Alpha> .take");
    CHECK(a.match->take.state == RecorderTakeState::taking);
    CHECK(!a.match->take.granted[1] && a.match->take.cursor[1] == 2);
    CHECK(chat_shown(a, "Alpha taking Bravos units"));
    const auto said = a.sent(RecordType::chat);
    CHECK(
        said.size() == 2 &&
        std::strcmp(reinterpret_cast<const char*>(said[0].bytes.data()) + 1, "<Alpha> .take") == 0
    );
    // One place a step: the finished unit 12 at once, then the unfinished
    // 13 and the empty 14 hand nothing over and end each pump.
    (void)net_match_pump(a.match.get());
    CHECK(log.transfers.size() == 1);
    CHECK(log.transfers[0].unit_index == 12 && log.transfers[0].new_owner_id == kA);
    CHECK(log.transfers[0].health == 900 && log.transfers[0].build_remaining == 0);
    CHECK(log.transfers[0].bank_heading == 0x7f640000u && log.transfers[0].pitch == 0);
    CHECK(a.match->take.cursor[1] == 4);
    for (int pump = 0; pump < 8 && a.match->take.state == RecorderTakeState::taking; ++pump)
        (void)net_match_pump(a.match.get());
    CHECK(log.transfers.size() == 2 && log.transfers[1].unit_index == 15);
    CHECK(log.transfers[1].health == 650);
    CHECK(a.match->taken_units == 2);
    // Past the block's end less two places, Bravo is rejected everywhere.
    CHECK(a.match->take.state == RecorderTakeState::idle && a.match->take.cursor[1] == 0);
    const auto rejects = a.sent(RecordType::reject);
    CHECK(rejects.size() == 1 && rejects[0].from == kA);
    RejectRecord sent_reject{};
    CHECK(
        decode_record(rejects[0].bytes.data(), rejects[0].bytes.size(), &sent_reject) ==
        WireError::ok
    );
    CHECK(sent_reject.player_id == kB && sent_reject.reason == 6);

    // .takecmd starts at the commander.
    a.match->take.granted[1] = true;
    net_match_say(a.match.get(), "<Alpha> .takecmd");
    CHECK(a.match->take.cursor[1] == 1);
    (void)net_match_pump(a.match.get());
    CHECK(log.transfers.size() == 4 && log.transfers[2].unit_index == 11);
    a.match->take = RecorderTake{};

    // Another player's .take is a claim: this machine's own take stops and
    // keeps no place in the taken block, so a later take of another player
    // hands over that player's units alone, and the next reject releases
    // the claim.
    a.match->take.state = RecorderTakeState::taking;
    a.match->take.takes = 1;
    a.match->take.cursor[1] = 6;
    std::snprintf(line.text, sizeof line.text, "%s", "<Bravo> .take");
    deliver(b, a, kB, broadcast_destination_id, line);
    CHECK(chat_shown(a, "Alpha aborting take claim"));
    CHECK(a.match->take.state == RecorderTakeState::claimed);
    CHECK(std::string(a.match->take.claimant) == "Bravo");
    CHECK(a.match->take.cursor[1] == 0);
    RejectRecord reject{};
    reject.player_id = 0x55;
    reject.reason = 6;
    deliver(b, a, kB, broadcast_destination_id, reject);
    CHECK(chat_shown(a, "Bravo take claim released"));
    CHECK(a.match->take.state == RecorderTakeState::idle);

    // A claimant with no recorder has its empty slots, and the slot after
    // its block, killed with the next reject.
    a.match->recorder.peer_protocol[1] = recorder_protocol_plain;
    deliver(b, a, kB, broadcast_destination_id, line);
    CHECK(a.match->take.state == RecorderTakeState::claimed_plain);
    // Units 11, 12, 13 and 15 were seen; 14 and 16..21 were not.
    CHECK(a.match->take.kill_records.size() == 7 * 9);
    a.forget_sent();
    deliver(b, a, kB, broadcast_destination_id, reject);
    const auto kills = a.sent(RecordType::unit_damage);
    CHECK(kills.size() == 7);
    UnitDamageRecord kill{};
    CHECK(decode_record(kills[0].bytes.data(), kills[0].bytes.size(), &kill) == WireError::ok);
    CHECK(kill.target_unit_index == 14 && kill.amount == 30999 && kill.kind == 1);
    CHECK(kill.direction == 0x92 && kill.source_unit_index == 0);
    CHECK(log.damage.size() == 7);
    CHECK(chat_shown(a, "Sending killing packets"));

    // A claim over one not yet released starts afresh: a claimant whose
    // recorder is protocol 1 or 2 has nothing killed at the next reject,
    // even when an earlier claimant with no recorder had slots to kill.
    deliver(b, a, kB, broadcast_destination_id, line);
    CHECK(!a.match->take.kill_records.empty());
    a.match->recorder.peer_protocol[1] = 2;
    deliver(b, a, kB, broadcast_destination_id, line);
    CHECK(a.match->take.state == RecorderTakeState::claimed_plain);
    CHECK(a.match->take.kill_records.empty());
    a.forget_sent();
    deliver(b, a, kB, broadcast_destination_id, reject);
    CHECK(a.sent(RecordType::unit_damage).empty() && log.damage.size() == 7);
    CHECK(a.match->take.state == RecorderTakeState::idle);

    // Without the rule nothing of it runs.
    SeatedMachine c(a_view, 0, recorder_rules(), kSlots);
    join(b, c);
    std::snprintf(line.text, sizeof line.text, "%s", "<Bravo> .give alpha");
    oa::base::text::copy_padded(c.player(0).name, "Alpha", sizeof c.player(0).name);
    deliver(b, c, kB, broadcast_destination_id, line);
    CHECK(!c.match->take.granted[1] && c.chats.size() == 1);
}

// setup.recorder-prebuilt-base: the second unit a player creates marks its
// base's centre; the player's .dobase has the host's machine send it the
// side's buildings around that centre, each created in a spare slot, switched
// on and handed over with the table's health. The host's own base is built
// here from the next player's spare slot.
void recorder_dobase_builds_the_offered_base() {
    start_case("recorder_dobase_builds_the_offered_base");
    constexpr uint32_t kH = 7, kC = 9;
    constexpr uint32_t kSlots = 10 * OA_PLAYER_COUNT + 1;
    const Seat h_view[4] = {{kH, OA_PLAYER_STATUS_LOCAL, 1}, {kC, OA_PLAYER_STATUS_MIRRORED, 2}};
    const Seat c_view[4] = {{kH, OA_PLAYER_STATUS_MIRRORED, 1}, {kC, OA_PLAYER_STATUS_LOCAL, 2}};
    SeatedMachine h(h_view, 0, recorder_rules(), kSlots);
    SeatedMachine c(c_view, 1, recorder_rules(), kSlots);
    join(h, c);
    oa::data::match_rules::MatchRules rules{};
    rules.setup.recorder_prebuilt_base.enabled = true;
    rules.setup.recorder_prebuilt_base.available = true;
    for (auto* m : {&h, &c}) {
        give_unit_blocks(*m);
        m->info(0).role = 1; // the host
        m->info(1).side = 0; // ARM
        m->info(0).side = 1; // CORE
        oa::base::text::copy_padded(m->player(0).name, "Host", sizeof m->player(0).name);
        oa::base::text::copy_padded(m->player(1).name, "Charlie", sizeof m->player(1).name);
        net_match_bind_rules(m->match.get(), &rules, false);
    }
    auto& base = h.match->recorder.base;
    recorder_standard_base(base);
    base.available[0] = base.available[1] = true;
    ReplicationLog c_log;
    log_replication(c, c_log);
    ReplicationLog h_log;
    log_replication(h, h_log);
    ChatRecord line{};
    std::snprintf(line.text, sizeof line.text, "%s", "<Charlie> .dobase");

    // Before Charlie created a second unit there is no centre.
    deliver(c, h, kC, broadcast_destination_id, line);
    CHECK(chat_shown(h, "Please build a building to mark the centre of your base"));
    CHECK(base.available[1]);
    UnitCreatedRecord first{};
    first.unit_def_index = 40;
    first.unit_index = 11; // the commander: not the centre
    first.position[0] = 500 << 16;
    first.position[2] = 600 << 16;
    deliver(c, h, kC, broadcast_destination_id, first);
    CHECK(base.centre[1][0] == 0);
    UnitCreatedRecord centre{};
    centre.unit_def_index = 41;
    centre.unit_index = 12;
    centre.position[0] = (1000 << 16) | 0x8000;
    centre.position[1] = 50 << 16;
    centre.position[2] = (2000 << 16) | 0x4000;
    deliver(c, h, kC, broadcast_destination_id, centre);
    CHECK(base.centre[1][0] == 1000 && base.centre[1][1] == 50 && base.centre[1][2] == 2000);

    h.forget_sent();
    deliver(c, h, kC, broadcast_destination_id, line);
    CHECK(chat_shown(h, "Charlie just built a base"));
    CHECK(!base.available[1] && h.match->base_buildings == 15);
    const auto creates = h.sent(RecordType::unit_created);
    const auto switches = h.sent(RecordType::unit_state_flags);
    const auto gives = h.sent(RecordType::unit_transfer);
    CHECK(creates.size() == 15 && switches.size() == 15 && gives.size() == 15);
    UnitCreatedRecord create{};
    CHECK(
        decode_record(creates[0].bytes.data(), creates[0].bytes.size(), &create) == WireError::ok
    );
    CHECK(creates[0].from == kH && creates[0].to == kC);
    CHECK(create.unit_def_index == 132 && create.unit_index == 9);
    CHECK(create.position[0] == (900 << 16) && create.position[1] == (50 << 16));
    CHECK(create.position[2] == (2140 << 16) && create.bank_heading == 0x74e90000u);
    UnitStateFlagsRecord on{};
    CHECK(decode_record(switches[0].bytes.data(), switches[0].bytes.size(), &on) == WireError::ok);
    CHECK(on.unit_index == 9 && on.state_mask == 1 && switches[0].to == kC);
    UnitTransferRecord give{};
    CHECK(decode_record(gives[0].bytes.data(), gives[0].bytes.size(), &give) == WireError::ok);
    CHECK(give.unit_index == 9 && give.new_owner_id == kC && give.health == 2500);
    CHECK(give.bank_heading == 0x7e640000u && gives[0].to == kC);
    // Charlie's machine takes them as the host's records.
    (void)net_match_pump(c.match.get());
    CHECK(c_log.creates.size() == 15 && c_log.create_owners[0] == 0);
    CHECK(c_log.transfers.size() == 15 && c_log.transfers[14].health == 1700);
    CHECK(c_log.flags.size() == 15);
    // The base is built once.
    h.forget_sent();
    deliver(c, h, kC, broadcast_destination_id, line);
    CHECK(h.sent(RecordType::unit_created).empty());

    // The host's own base: CORE's, from the next player's spare slot, here.
    UnitCreatedRecord own{};
    own.unit_def_index = 41;
    own.unit_index = 2;
    own.position[0] = 300 << 16;
    own.position[2] = 100 << 16;
    uint8_t bytes[32];
    std::size_t written = 0;
    CHECK(encode_record(own, bytes, sizeof bytes, &written) == WireError::ok);
    CHECK(net_match_send(h.match.get(), kH, broadcast_destination_id, bytes, written));
    CHECK(base.centre[0][0] == 300 && base.centre[0][2] == 100);
    h.forget_sent();
    h_log = ReplicationLog{};
    net_match_say(h.match.get(), "<Host> .dobase");
    CHECK(h.sent(RecordType::unit_created).empty());
    // Buildings whose x or z would not be past 0 are left out: of CORE's
    // fifteen, the seven 120 or more north of z 100.
    CHECK(h_log.creates.size() == 8 && h_log.create_owners[0] == 1);
    CHECK(h_log.creates[0].unit_def_index == 274 && h_log.creates[0].unit_index == 19);
    CHECK(h_log.transfers.size() == 8 && h_log.transfers[0].new_owner_id == kH);

    // .baseoff from the host ends every offer.
    base.available[1] = true;
    net_match_say(h.match.get(), "<Host> .baseoff");
    CHECK(!base.enabled && !base.available[1]);
    CHECK(chat_shown(h, "Quick base disabled"));
}

// setup.commander-warp: only under the rule does .cmdwarp turn the warp on,
// and each one turns it on or off; the computer players hold no warp, and an
// unpause ends the warp for everyone.
void commander_warp_follows_its_rule() {
    start_case("commander_warp_follows_its_rule");
    constexpr uint32_t kA = 7, kB = 9, kComputer = 10;
    const Seat a_view[4] = {
        {kA, OA_PLAYER_STATUS_LOCAL, 1},
        {kB, OA_PLAYER_STATUS_MIRRORED, 2},
        {kComputer, OA_PLAYER_STATUS_MIRRORED, 2}
    };
    SeatedMachine a(a_view, 0, recorder_rules());
    a.info(0).role = 1; // the host
    a.info(2).state = OA_PLAYER_STATUS_COMPUTER;
    net_match_say(a.match.get(), "<Alpha> .cmdwarp");
    CHECK(a.match->recorder.options.commander_warp == 0);
    oa::data::match_rules::MatchRules rules{};
    rules.setup.commander_warp.enabled = true;
    rules.setup.commander_warp.available = true;
    net_match_bind_rules(a.match.get(), &rules, false);
    net_match_say(a.match.get(), "<Alpha> .cmdwarp");
    CHECK(a.match->recorder.options.commander_warp == 1);
    net_match_recorder_start(a.match.get());
    CHECK((a.world->game.sim_run_flags & run_flag_paused) != 0);
    net_match_warp_done(a.match.get());
    a.match->recorder.warp_done[1] = true; // Bravo's machine said so
    net_match_paused_frame(a.match.get());
    CHECK(a.match->recorder.warp_released);
    CHECK((a.world->game.sim_run_flags & run_flag_paused) == 0);

    // An unpause before every warp is done ends the warp too.
    SeatedMachine b(a_view, 0, recorder_rules());
    b.info(0).role = 1;
    net_match_bind_rules(b.match.get(), &rules, false);
    b.match->recorder.options.commander_warp = 1;
    net_match_recorder_start(b.match.get());
    net_match_set_pause(b.match.get(), false);
    CHECK(b.match->recorder.warp_released);
    net_match_say(b.match.get(), "<Alpha> .cmdwarp");
    CHECK(b.match->recorder.options.commander_warp == 0);
}

} // namespace

int main() {
    packet_layer_over_memory();
    packet_layer_reads_game_send_options();
    host_designation();
    packet_layer_drops_oversized();
    packet_layer_reclaims_lobby_slots();
    packet_layer_parses_waiting_frame_first();
    launch_helpers();
    launch_finds_a_side_past_the_table();
    launch_carries_the_options_lock();
    launch_counts_seated_players();
    shared_machines_receive_broadcasts_once();
    loading_screen_progress();
    pause_goes_out_from_the_primary();
    channel_reclaimed_from_a_departed_player();
    chat_follows_the_chat_mode();
    seat_chat_reaches_seats();
    seats_say_from_their_own_id();
    automatic_sharing_follows_the_thresholds();
    automatic_sharing_picks_the_last_poorer_player();
    give_goes_out_when_both_take_part();
    periodic_economy_goes_to_humans_still_playing();
    a_winner_answers_but_sends_no_economy();
    rejecting_retires_a_machine_only_for_a_human();
    a_disconnect_notice_naming_this_machine_ends_its_game();
    player_name_and_data_changes_reach_the_slot();
    ping_is_echoed_from_its_addressee();
    loading_ends_with_status_and_group_requests();
    economy_count_runs_across_games();
    loading_frames_keep_the_loading_pace();
    team_panel_changes_reach_the_other_machine();
    two_machines_lobby_to_match();
    password_game_keeps_the_session_open();
    client_computer_is_a_session_player();
    address_picks_the_enumeration_target();
    private_lines_stay_hidden();
    engine_signature_in_game_blocks();
    unicode_chat_between_two_readers();
    unicode_chat_in_a_mixed_room();
    unicode_chat_reads_malformed_lines_safely();
    unicode_chat_keeps_commands_and_the_plain_wire();
    alliance_lines_show_in_the_language_shown();
    integrity_check_answers_and_reports();
    integrity_report_request_is_answered();
    vote_tally_rules();
    votes_reject_on_every_machine();
    silence_opens_a_timeout_vote();
    timeout_vote_leaves_out_its_target();
    remote_colour_survives_in_game_blocks();
    recorder_records_reach_the_match();
    whiteboard_marks_reach_allies();
    recorder_cameras_are_shared();
    autopause_holds_for_the_host();
    notices_follow_the_language();
    speed_lock_and_speed_zero();
    hosts_speed_lock_narrows_the_range();
    commander_start_sync_places_the_commander();
    recorder_session_commands_in_the_match();
    recorder_take_hands_a_silent_players_units_over();
    recorder_dobase_builds_the_offered_base();
    commander_warp_follows_its_rule();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("net match: all tests passed");
    return 0;
}
