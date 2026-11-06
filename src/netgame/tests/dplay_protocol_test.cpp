// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// DirectPlay core-protocol engine tests over an in-memory network.

#include "oa/netgame/dplay/engine.hpp"
#include "oa/netgame/dplay/protocol.hpp"
#include "oa/netgame/session.hpp"
#include "oa/netgame/network.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

using namespace oa::netgame;
using namespace oa::netgame::dplay;
using Bytes = std::vector<uint8_t>;

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

// ---- engine over an in-memory network ----

struct Node {
    Engine engine;
    Address source;
    bool silent{}; ///< the machine has stopped: what reaches it is dropped and it sends nothing
};

struct Wire {
    Node* nodes[4]{};

    struct Packet {
        Node* to = nullptr;
        Address from;
        bool stream = false;
        Bytes bytes;
    };

    std::vector<Packet> in_flight;
    std::vector<Packet> sent; // every message ever sent, in order
};

Wire* g_wire = nullptr;

struct IoContext {
    Node* self = nullptr;
};

Node* node_for(const Address& to, bool stream) {
    for (Node* n : g_wire->nodes)
        if (n != nullptr &&
            (stream ? n->engine.config.stream.port : n->engine.config.datagram.port) == to.port)
            return n;
    return nullptr;
}

bool mem_send_stream(void* context, const Address& to, const uint8_t* bytes, std::size_t size) {
    Node* self = static_cast<Node*>(context);
    Node* target = node_for(to, true);
    if (target == nullptr)
        return false;
    g_wire->in_flight.push_back({target, self->source, true, Bytes(bytes, bytes + size)});
    g_wire->sent.push_back(g_wire->in_flight.back());
    return true;
}

bool mem_send_datagram(void* context, const Address& to, const uint8_t* bytes, std::size_t size) {
    Node* self = static_cast<Node*>(context);
    g_wire->sent.push_back({nullptr, self->source, false, Bytes(bytes, bytes + size)});
    for (Node* n : g_wire->nodes)
        if (n != nullptr && n != self &&
            (n->engine.config.enum_port == to.port || n->engine.config.datagram.port == to.port))
            g_wire->in_flight.push_back({n, self->source, false, Bytes(bytes, bytes + size)});
    return true;
}

// Each node's address on the in-memory network, as the socket host finds
// its local address toward a destination.
bool mem_local_address(void* context, const Address&, uint8_t ip[4]) {
    std::memcpy(ip, static_cast<Node*>(context)->source.ip, 4);
    return true;
}

void deliver_all(uint32_t now) {
    for (int guard = 0; guard < 1000 && !g_wire->in_flight.empty(); ++guard) {
        auto packet = g_wire->in_flight.front();
        g_wire->in_flight.erase(g_wire->in_flight.begin());
        if (packet.to->silent)
            continue;
        if (packet.stream)
            engine_on_stream(
                &packet.to->engine, packet.from, packet.bytes.data(), packet.bytes.size(), now
            );
        else
            engine_on_datagram(
                &packet.to->engine, packet.from, packet.bytes.data(), packet.bytes.size(), now
            );
    }
}

std::unique_ptr<Node> make_node(uint8_t last_octet, uint16_t stream_port, uint32_t seed) {
    auto n = std::make_unique<Node>();
    n->source = Address{{10, 0, 0, last_octet}, 0};
    EngineConfig config{};
    config.stream.port = stream_port;
    config.datagram.port = static_cast<uint16_t>(stream_port + 50);
    config.seed = seed;
    engine_init(&n->engine, config, EngineIo{n.get(), mem_send_stream, mem_send_datagram});
    return n;
}

std::unique_ptr<Node> make_addressed_node(uint8_t last_octet, uint16_t stream_port, uint32_t seed) {
    auto n = make_node(last_octet, stream_port, seed);
    n->engine.io.local_address = mem_local_address;
    return n;
}

/// Decodes the last sent control message with a command.
///
/// @param command Command word looked for.
/// @param[out] out The message.
/// @param[out] bytes Its bytes, when not null.
/// @return False when no such message was sent.
bool last_sent(uint16_t command, Message* out, Bytes* bytes = nullptr) {
    for (auto it = g_wire->sent.rbegin(); it != g_wire->sent.rend(); ++it)
        if (decode_message(it->bytes.data(), it->bytes.size(), out) == ParseError::ok &&
            out->command == command) {
            if (bytes != nullptr)
                *bytes = it->bytes;
            return true;
        }
    return false;
}

uint32_t pop_system_type(Engine* e) {
    uint8_t buffer[512];
    uint32_t from = 1, to = 0, size = sizeof buffer;
    if (engine_receive(e, &from, &to, buffer, &size) != result::ok || from != 0 || size < 4)
        return 0;
    return load_u32(buffer);
}

void engine_rejects_wrong_password_and_full_sessions() {
    current_test = "engine_rejects_wrong_password_and_full_sessions";
    Wire wire;
    g_wire = &wire;
    auto host = make_node(1, 2300, 7);
    auto client = make_node(2, 2301, 9);
    wire.nodes[0] = host.get();
    wire.nodes[1] = client.get();

    SessionDesc desc{};
    desc.flags = session_flag::migrate_host;
    std::memcpy(desc.application_guid, application_guid, 16);
    desc.max_players = 1;
    CHECK(engine_host(&host->engine, desc, "Game", "secret", 0) == result::ok);
    CHECK((host->engine.desc.flags & session_flag::password_required) != 0);
    uint32_t host_player = 0;
    CHECK(engine_create_player(&host->engine, "h", "h", nullptr, 0, 0, &host_player) == result::ok);

    Guid app{};
    std::memcpy(app.bytes, application_guid, 16);
    const uint8_t everywhere[4] = {255, 255, 255, 255};
    // A full, password-protected session is not advertised without the ALL flag.
    CHECK(engine_enum_sessions(&client->engine, app, everywhere, 0) == result::ok);
    deliver_all(0);
    CHECK(client->engine.session_count == 0);

    host->engine.desc.max_players = 4;
    uint8_t request[256];
    const std::size_t n = encode_enum_sessions(
        client->engine.config.stream, app, "secret", enum_flag::available, request, sizeof request
    );
    engine_on_datagram(&host->engine, client->source, request, n, 0);
    deliver_all(0);
    CHECK(client->engine.session_count == 1);
    CHECK(std::strcmp(client->engine.sessions[0].name, "Game") == 0);
    CHECK(
        client->engine.sessions[0].host.ip[3] == 1 && client->engine.sessions[0].host.port == 2300
    );

    const Guid instance = client->engine.sessions[0].instance;
    CHECK(engine_join(&client->engine, instance, "wrong", 0) == result::ok);
    deliver_all(0);
    CHECK(engine_join_status(&client->engine) == result::invalid_password);
    CHECK(client->engine.state == EngineState::idle);

    CHECK(engine_join(&client->engine, instance, "secret", 0) == result::ok);
    deliver_all(0);
    CHECK(engine_join_status(&client->engine) == result::ok);
    CHECK(client->engine.system_id != 0 && client->engine.name_server_id == host->engine.system_id);

    host->engine.desc.max_players = 1;
    uint32_t id = 0;
    CHECK(
        engine_create_player(&client->engine, "c", "c", nullptr, 0, 0, &id) == result::connecting
    );
    deliver_all(0);
    CHECK(engine_create_player_status(&client->engine, &id) == result::cant_create_player);

    host->engine.desc.max_players = 4;
    CHECK(
        engine_create_player(&client->engine, "c", "c", nullptr, 0, 0, &id) == result::connecting
    );
    deliver_all(0);
    CHECK(engine_create_player_status(&client->engine, &id) == result::ok);
    CHECK(
        pop_system_type(&host->engine) == static_cast<uint32_t>(SystemMessageType::player_created)
    );

    // Unguaranteed data with the two optional bytes before the player ids.
    uint8_t datagram[16] = {0x09, 0x2e};
    store_u32(datagram + 2, id);
    store_u32(datagram + 6, host_player);
    datagram[10] = 0x02;
    engine_on_datagram(&host->engine, client->source, datagram, 11, 0);
    uint8_t out[16];
    uint32_t from = 0, to = 0, size = sizeof out;
    CHECK(engine_receive(&host->engine, &from, &to, out, &size) == result::ok);
    CHECK(from == id && to == host_player && size == 1 && out[0] == 0x02);

    // Buffer-too-small leaves the message queued with its size.
    CHECK(engine_send(&client->engine, id, host_player, send_guaranteed, out, 1) == result::ok);
    deliver_all(0);
    size = 0;
    CHECK(
        engine_receive(&host->engine, &from, &to, out, &size) == result::buffer_too_small &&
        size == 1
    );
    size = sizeof out;
    CHECK(engine_receive(&host->engine, &from, &to, out, &size) == result::ok && size == 1);
    CHECK(engine_receive(&host->engine, &from, &to, out, &size) == result::no_messages);

    // The session has the migrate-host flag: when the host leaves, the one
    // machine left takes its place.
    engine_close(&host->engine);
    deliver_all(0);
    CHECK(
        pop_system_type(&client->engine) ==
        static_cast<uint32_t>(SystemMessageType::player_destroyed)
    );
    CHECK(
        pop_system_type(&client->engine) == static_cast<uint32_t>(SystemMessageType::host_changed)
    );
    CHECK(client->engine.state == EngineState::open_host);
    g_wire = nullptr;
}

// A joiner's AddForwardRequest ends with the session's first reserved
// dword, as the enumeration reply gave it; headers carry each sender's own
// address; SessionDescChanged ends with 80 zero bytes and fills the
// description's string fields.
void join_and_description_messages_carry_what_the_game_sends() {
    current_test = "join_and_description_messages_carry_what_the_game_sends";
    Wire wire;
    g_wire = &wire;
    auto host = make_addressed_node(1, 2300, 11);
    auto client = make_addressed_node(2, 2301, 13);
    wire.nodes[0] = host.get();
    wire.nodes[1] = client.get();

    SessionDesc desc{};
    desc.flags = session_flag::migrate_host;
    std::memcpy(desc.application_guid, application_guid, 16);
    desc.max_players = 4;
    CHECK(engine_host(&host->engine, desc, "Game", "", 0) == result::ok);
    const uint32_t key = host->engine.desc.reserved[0];
    CHECK(key == host->engine.id_key && key != 0);
    Guid app{};
    std::memcpy(app.bytes, application_guid, 16);
    const uint8_t host_ip[4] = {10, 0, 0, 1};
    CHECK(engine_enum_sessions(&client->engine, app, host_ip, 0) == result::ok);
    Message m{};
    CHECK(
        last_sent(command::enum_sessions, &m) && m.reply_to.ip[3] == 2 && m.reply_to.port == 2301
    );
    deliver_all(0);
    CHECK(client->engine.session_count == 1 && client->engine.sessions[0].desc.reserved[0] == key);
    CHECK(
        last_sent(command::enum_sessions_reply, &m) && m.reply_to.ip[3] == 1 &&
        m.reply_to.port == 2300
    );

    CHECK(
        engine_join(&client->engine, client->engine.sessions[0].instance, nullptr, 12345) ==
        result::ok
    );
    deliver_all(12345);
    CHECK(engine_join_status(&client->engine) == result::ok);
    CHECK(last_sent(command::add_forward_request, &m));
    CHECK(m.tick == key && m.reply_to.ip[3] == 2 && m.reply_to.ip[0] == 10);

    // The session description changes with its tail and string offsets.
    SessionDesc changed = host->engine.desc;
    changed.user[0] = 0x0e020400;
    CHECK(engine_set_session_desc(&host->engine, changed, "Game  Map", "") == result::ok);
    Bytes bytes;
    CHECK(last_sent(command::session_desc_changed, &m, &bytes));
    CHECK(m.desc.name_pointer == session_desc_changed_name_offset);
    CHECK(m.desc.password_pointer == session_desc_changed_password_offset("Game  Map"));
    CHECK(m.desc.password_pointer == session_desc_changed_name_offset + 2 * (9 + 1));
    CHECK(std::strcmp(m.session_name, "Game  Map") == 0 && m.desc.user[0] == 0x0e020400);
    const std::size_t body = header_bytes + 12 + session_desc_bytes + 2 * (9 + 1) + 2;
    CHECK(bytes.size() == body + session_desc_changed_tail_bytes);
    bool zero_tail = bytes.size() == body + session_desc_changed_tail_bytes;
    for (std::size_t i = body; i < bytes.size(); ++i)
        zero_tail = zero_tail && bytes[i] == 0;
    CHECK(zero_tail);
    deliver_all(0);
    CHECK(client->engine.desc.user[0] == 0x0e020400);

    // Under the session-description rule the host publishes the
    // description again with its password field 0 and no password.
    host->engine.publish_without_password = true;
    CHECK(engine_set_session_desc(&host->engine, changed, "Game  Map", "") == result::ok);
    CHECK(last_sent(command::session_desc_changed, &m, &bytes));
    CHECK(m.desc.password_pointer == 0 && m.desc.name_pointer == session_desc_changed_name_offset);
    CHECK((m.desc.flags & session_flag::password_required) == 0);
    host->engine.publish_without_password = false;
    deliver_all(0);

    // An advertised address is written as it is.
    auto fixed = make_node(3, 2302, 17);
    wire.nodes[2] = fixed.get();
    const uint8_t advertised[4] = {192, 0, 2, 7};
    std::memcpy(fixed->engine.config.stream.ip, advertised, 4);
    fixed->engine.io.local_address = mem_local_address;
    CHECK(engine_enum_sessions(&fixed->engine, app, host_ip, 0) == result::ok);
    CHECK(last_sent(command::enum_sessions, &m) && std::memcmp(m.reply_to.ip, advertised, 4) == 0);
    deliver_all(0);
    g_wire = nullptr;
}

void session_desc_changed_codec_round_trips_with_its_tail() {
    current_test = "session_desc_changed_codec_round_trips_with_its_tail";
    SessionDesc desc{};
    desc.flags = session_flag::migrate_host | session_flag::join_disabled;
    desc.max_players = 3;
    desc.current_players = 2;
    desc.name_pointer = 0x1234;
    desc.password_pointer = 0x5678;
    desc.reserved[0] = 0x11223344;
    desc.user[3] = 0x016701f4;
    uint8_t out[512];
    const Address from{{10, 66, 4, 200}, 2300};
    const std::size_t n = encode_session_desc_changed(from, 0, desc, "Name", "pw", out, sizeof out);
    const std::size_t strings = 2 * (4 + 1) + 2 * (2 + 1);
    CHECK(n == header_bytes + 12 + session_desc_bytes + strings + session_desc_changed_tail_bytes);
    Message m{};
    CHECK(decode_message(out, n, &m) == ParseError::ok);
    CHECK(std::strcmp(m.session_name, "Name") == 0 && std::strcmp(m.password, "pw") == 0);
    CHECK(
        m.desc.name_pointer == 0x1234 && m.desc.password_pointer == 0x5678 &&
        m.desc.reserved[0] == 0x11223344
    );
    CHECK(
        m.desc.flags == desc.flags && m.desc.user[3] == 0x016701f4 && m.desc.current_players == 2
    );
    CHECK(address_equal(m.reply_to, from));
    CHECK(load_u32(out + header_bytes + 4) == session_desc_changed_name_offset);
    CHECK(load_u32(out + header_bytes + 8) == session_desc_changed_password_offset("Name"));
    uint8_t short_buffer[header_bytes + 12 + session_desc_bytes + 20];
    CHECK(
        encode_session_desc_changed(
            from, 0, desc, "Name", "pw", short_buffer, sizeof short_buffer
        ) == 0
    );
}

void join_reject_reasons_follow_the_packet_pump() {
    current_test = "join_reject_reasons_follow_the_packet_pump";
    uint8_t image[system_message::create_bytes + create_player_data_bytes]{};
    store_u32(image, static_cast<uint32_t>(SystemMessageType::player_created));
    store_u32(image + system_message::create_data, system_message::create_bytes);
    store_u32(image + system_message::create_data_size, create_player_data_bytes);
    CreatePlayerData block{};
    std::memcpy(block.tag, "PASS", 4);
    block.version_high = create_player_version_high;
    CHECK(
        encode_create_player_data(
            block, image + system_message::create_bytes, create_player_data_bytes
        ) == WireError::ok
    );
    using oa::netgame::session::session_join_reject_reason;
    CHECK(session_join_reject_reason(image, sizeof image, false, true, "pass") == 0);
    CHECK(session_join_reject_reason(image, sizeof image, false, true, "other") == 4);
    CHECK(session_join_reject_reason(image, sizeof image, true, false, nullptr) == 3);
    image[system_message::create_bytes + 0x13] = 0x51;
    CHECK(session_join_reject_reason(image, sizeof image, false, false, nullptr) == 8);
}

// ---- sessions of several machines ----

constexpr uint8_t everywhere[4] = {255, 255, 255, 255};
constexpr auto created = static_cast<uint32_t>(SystemMessageType::player_created);
constexpr auto destroyed = static_cast<uint32_t>(SystemMessageType::player_destroyed);
constexpr auto became_host = static_cast<uint32_t>(SystemMessageType::host_changed);
constexpr auto lost = static_cast<uint32_t>(SystemMessageType::session_lost);
constexpr auto data_changed = static_cast<uint32_t>(SystemMessageType::player_data_changed);
constexpr auto name_changed = static_cast<uint32_t>(SystemMessageType::player_name_changed);
using Types = std::vector<uint32_t>;

Guid game_application() {
    Guid app{};
    std::memcpy(app.bytes, application_guid, 16);
    return app;
}

/// Hosts a game with one application player.
///
/// @param host Machine hosting.
/// @param flags Session flags.
/// @param[out] player The player's id.
/// @return True when both succeeded.
bool host_with_player(Node* host, uint32_t flags, uint32_t* player) {
    SessionDesc desc{};
    desc.flags = flags;
    std::memcpy(desc.application_guid, application_guid, 16);
    desc.max_players = 8;
    return engine_host(&host->engine, desc, "Game", "", 0) == result::ok &&
           engine_create_player(&host->engine, "host", "host", nullptr, 0, 0, player) == result::ok;
}

/// Searches for the one hosted game, joins it and creates one application player.
///
/// @param joiner Machine joining.
/// @param[out] player The player's id.
/// @return True when every step succeeded.
bool join_with_player(Node* joiner, uint32_t* player) {
    if (engine_enum_sessions(&joiner->engine, game_application(), everywhere, 0) != result::ok)
        return false;
    deliver_all(0);
    if (joiner->engine.session_count != 1 ||
        engine_join(&joiner->engine, joiner->engine.sessions[0].instance, nullptr, 0) != result::ok)
        return false;
    deliver_all(0);
    if (engine_join_status(&joiner->engine) != result::ok ||
        engine_create_player(&joiner->engine, "joiner", "joiner", nullptr, 0, 0, player) !=
            result::connecting)
        return false;
    deliver_all(0);
    return engine_create_player_status(&joiner->engine, player) == result::ok;
}

/// Receives every queued message and returns the types of the system messages among them, in order.
///
/// @param e Engine whose queue is emptied.
/// @return The types.
Types system_types(Engine* e) {
    Types types;
    uint8_t buffer[1024];
    uint32_t from = 0, to = 0, size = sizeof buffer;
    while (engine_receive(e, &from, &to, buffer, &size) == result::ok) {
        if (from == system_message_sender_id && size >= 4)
            types.push_back(load_u32(buffer));
        size = sizeof buffer;
    }
    return types;
}

/// Receives the next queued message, which must be a system message.
///
/// @param e Engine whose queue is read.
/// @return The message's bytes, or none when the next message is not a system message.
Bytes next_system_message(Engine* e) {
    uint8_t buffer[1024];
    uint32_t from = 1, to = 0, size = sizeof buffer;
    if (engine_receive(e, &from, &to, buffer, &size) != result::ok ||
        from != system_message_sender_id)
        return {};
    return Bytes(buffer, buffer + size);
}

/// Counts the control messages sent with a command, to one machine or to any.
///
/// @param command Command word.
/// @param to Machine addressed, or null for any.
/// @return The count.
std::size_t count_sent(uint16_t command, const Node* to = nullptr) {
    std::size_t n = 0;
    Message m{};
    for (const auto& packet : g_wire->sent)
        if ((to == nullptr || packet.to == to) &&
            decode_message(packet.bytes.data(), packet.bytes.size(), &m) == ParseError::ok &&
            m.command == command)
            ++n;
    return n;
}

/// Command word of one in-flight or sent packet, or 0 when it is not a control message.
///
/// @param packet Packet to read.
/// @return The command word.
uint16_t packet_command(const Wire::Packet& packet) {
    Message m{};
    if (decode_message(packet.bytes.data(), packet.bytes.size(), &m) != ParseError::ok)
        return 0;
    return m.command;
}

/// Delivers one packet to its machine.
///
/// @param packet Packet to deliver.
/// @param now Current time in milliseconds.
void deliver_packet(const Wire::Packet& packet, uint32_t now) {
    if (packet.to == nullptr || packet.to->silent)
        return;
    if (packet.stream)
        engine_on_stream(
            &packet.to->engine, packet.from, packet.bytes.data(), packet.bytes.size(), now
        );
    else
        engine_on_datagram(
            &packet.to->engine, packet.from, packet.bytes.data(), packet.bytes.size(), now
        );
}

/// Delivers the first in-flight packet.
///
/// @param now Current time in milliseconds.
void deliver_front(uint32_t now) {
    if (g_wire->in_flight.empty())
        return;
    const Wire::Packet packet = g_wire->in_flight.front();
    g_wire->in_flight.erase(g_wire->in_flight.begin());
    deliver_packet(packet, now);
}

/// Delivers in-flight packets except those `hold` keeps, which stay in order at the front.
///
/// Packets sent while delivering are delivered or held by the same rule.
///
/// @param hold Tells which packets to keep.
/// @param now Current time in milliseconds.
void deliver_except(bool (*hold)(const Wire::Packet&), uint32_t now) {
    std::vector<Wire::Packet> held;
    for (int guard = 0; guard < 1000 && !g_wire->in_flight.empty(); ++guard) {
        const Wire::Packet packet = g_wire->in_flight.front();
        g_wire->in_flight.erase(g_wire->in_flight.begin());
        if (hold != nullptr && hold(packet)) {
            held.push_back(packet);
            continue;
        }
        deliver_packet(packet, now);
    }
    g_wire->in_flight.insert(g_wire->in_flight.begin(), held.begin(), held.end());
}

/// Tells whether a packet is a request to be admitted.
///
/// @param packet Packet to test.
/// @return True for an AddForwardRequest.
bool is_admission(const Wire::Packet& packet) {
    return packet_command(packet) == command::add_forward_request;
}

/// Tells whether a packet is an admission or a player announcement.
///
/// @param packet Packet to test.
/// @return True for an AddForwardRequest or a CreatePlayer.
bool is_admission_or_announcement(const Wire::Packet& packet) {
    const uint16_t word = packet_command(packet);
    return word == command::add_forward_request || word == command::create_player;
}

/// Tells whether a roster packet names a player.
///
/// @param packet SuperEnumPlayersReply to read.
/// @param id Player id.
/// @return True when the roster lists the id.
bool roster_names(const Wire::Packet& packet, uint32_t id) {
    Message m{};
    if (decode_message(packet.bytes.data(), packet.bytes.size(), &m) != ParseError::ok)
        return false;
    for (uint32_t i = 0; i < m.roster_count; ++i)
        if (m.roster[i].id == id)
            return true;
    return false;
}

/// Tells whether a machine was sent a CreatePlayer for an id.
///
/// @param to Machine addressed.
/// @param id Player id.
/// @return True when such an announcement was sent.
bool announced_player_to(const Node* to, uint32_t id) {
    Message m{};
    for (const auto& packet : g_wire->sent)
        if (packet.to == to &&
            decode_message(packet.bytes.data(), packet.bytes.size(), &m) == ParseError::ok &&
            m.command == command::create_player && m.player.id == id)
            return true;
    return false;
}

/// Tells whether a machine holds an application player.
///
/// @param e Engine to search.
/// @param id Player id.
/// @return True when that player is present and is not a system player.
bool has_application_player(const Engine& e, uint32_t id) {
    const EnginePlayer* player = engine_find_player(&e, id);
    return player != nullptr && (player->info.flags & player_flag::system_player) == 0;
}

/// Runs the ping timer of every machine still answering, then delivers what they sent.
///
/// @param now Current time in milliseconds.
void poll_all(uint32_t now) {
    for (Node* n : g_wire->nodes)
        if (n != nullptr && !n->silent)
            engine_poll(&n->engine, now);
    deliver_all(now);
}

/// Tells whether the ids of a session's players occupy distinct slots of the name server's table.
///
/// @param ids Player ids.
/// @param key The session's player-id key.
/// @return True when no two ids share a slot.
bool distinct_slots(const std::vector<uint32_t>& ids, uint32_t key) {
    for (std::size_t i = 0; i < ids.size(); ++i)
        for (std::size_t j = i + 1; j < ids.size(); ++j)
            if (((ids[i] ^ key) & 0xffff) == ((ids[j] ^ key) & 0xffff))
                return false;
    return true;
}

// When the host's machine leaves, the machine whose system player has the
// lowest id takes the name server's place: it tells its own players with
// 0x0101, announces itself to the other machine, answers searches and takes
// new machines in with ids under the session's key.
void host_leaves_and_the_lowest_system_player_takes_its_place() {
    current_test = "host_leaves_and_the_lowest_system_player_takes_its_place";
    Wire wire;
    g_wire = &wire;
    auto a = make_node(1, 2300, 21);
    auto b = make_node(2, 2301, 23);
    auto c = make_node(3, 2302, 25);
    wire.nodes[0] = a.get();
    wire.nodes[1] = b.get();
    wire.nodes[2] = c.get();
    uint32_t a_player = 0, b_player = 0, c_player = 0;
    CHECK(host_with_player(a.get(), session_flag::migrate_host, &a_player));
    CHECK(join_with_player(b.get(), &b_player));
    CHECK(join_with_player(c.get(), &c_player));
    const uint32_t key = a->engine.desc.reserved[0];
    uint8_t instance[16];
    std::memcpy(instance, a->engine.desc.instance_guid, 16);
    (void)system_types(&b->engine);
    (void)system_types(&c->engine);
    Node* lower = b->engine.system_id < c->engine.system_id ? b.get() : c.get();
    Node* higher = lower == b.get() ? c.get() : b.get();
    const uint32_t higher_player = higher == b.get() ? b_player : c_player;
    wire.sent.clear();

    engine_close(&a->engine);
    deliver_all(0);
    CHECK(lower->engine.state == EngineState::open_host);
    CHECK(higher->engine.state == EngineState::open_client);
    CHECK(lower->engine.name_server_id == lower->engine.system_id);
    CHECK(
        higher->engine.name_server_id == lower->engine.system_id &&
        !higher->engine.awaiting_name_server
    );
    CHECK(higher->engine.name_server.port == lower->engine.config.stream.port);
    CHECK(system_types(&lower->engine) == (Types{destroyed, became_host}));
    CHECK(system_types(&higher->engine) == (Types{destroyed}));

    Message m{};
    Bytes bytes;
    CHECK(count_sent(command::i_am_name_server) == 1);
    CHECK(last_sent(command::i_am_name_server, &m, &bytes));
    CHECK(m.id_to == higher->engine.system_id && m.player_id == lower->engine.system_id);
    CHECK(
        m.flags == (player_flag::system_player | player_flag::name_server | player_flag::in_group)
    );
    CHECK(
        m.player.has_addresses && address_ip_is_zero(m.player.stream) &&
        address_ip_is_zero(m.player.datagram)
    );
    CHECK(m.player.stream.port == lower->engine.config.stream.port);
    CHECK(m.player.datagram.port == lower->engine.config.datagram.port);
    CHECK(bytes.size() == header_bytes + 16 + name_server_addresses_bytes);
    CHECK(
        bytes.size() >= header_bytes + 16 &&
        load_u32(bytes.data() + header_bytes + 12) == name_server_addresses_bytes
    );
    const EnginePlayer* announced = engine_find_player(&higher->engine, lower->engine.system_id);
    CHECK(announced != nullptr && (announced->info.flags & player_flag::name_server) != 0);

    // A new machine finds the game at the new name server and joins it.
    auto d = make_node(4, 2303, 27);
    wire.nodes[3] = d.get();
    uint32_t d_player = 0;
    CHECK(join_with_player(d.get(), &d_player));
    CHECK(
        d->engine.session_count == 1 &&
        d->engine.sessions[0].host.port == lower->engine.config.stream.port
    );
    CHECK(
        d->engine.desc.reserved[0] == key &&
        std::memcmp(d->engine.desc.instance_guid, instance, 16) == 0
    );
    CHECK(d->engine.name_server_id == lower->engine.system_id);
    const uint32_t lower_player = lower == b.get() ? b_player : c_player;
    CHECK(distinct_slots(
        {lower->engine.system_id,
         lower_player,
         higher->engine.system_id,
         higher_player,
         d->engine.system_id,
         d_player},
        key
    ));
    CHECK(engine_find_player(&higher->engine, d->engine.system_id) != nullptr);
    CHECK(system_types(&higher->engine) == (Types{created}));
    CHECK(system_types(&lower->engine) == (Types{created}));

    // The other machine's next player takes its id from the new name server.
    uint32_t second = 0;
    CHECK(
        engine_create_player(&higher->engine, "second", "second", nullptr, 0, 0, &second) ==
        result::connecting
    );
    deliver_all(0);
    CHECK(engine_create_player_status(&higher->engine, &second) == result::ok);
    CHECK(engine_find_player(&lower->engine, second) != nullptr);
    CHECK(engine_find_player(&d->engine, second) != nullptr);
    g_wire = nullptr;
}

// An announcement that reaches a machine before the old name server's
// departure does is taken at once; the departure then elects no one.
void an_announcement_ahead_of_the_departure_is_taken() {
    current_test = "an_announcement_ahead_of_the_departure_is_taken";
    Wire wire;
    g_wire = &wire;
    auto a = make_node(1, 2300, 31);
    auto b = make_node(2, 2301, 33);
    auto c = make_node(3, 2302, 35);
    wire.nodes[0] = a.get();
    wire.nodes[1] = b.get();
    wire.nodes[2] = c.get();
    uint32_t a_player = 0, b_player = 0, c_player = 0;
    CHECK(host_with_player(a.get(), session_flag::migrate_host, &a_player));
    CHECK(join_with_player(b.get(), &b_player));
    CHECK(join_with_player(c.get(), &c_player));
    (void)system_types(&b->engine);
    (void)system_types(&c->engine);
    Node* lower = b->engine.system_id < c->engine.system_id ? b.get() : c.get();
    Node* higher = lower == b.get() ? c.get() : b.get();

    engine_close(&a->engine);
    // The machine taking the place hears the departure first, and its
    // announcement overtakes the departure on its way to the other machine.
    auto& flight = wire.in_flight;
    std::stable_partition(flight.begin(), flight.end(), [&](const Wire::Packet& p) {
        return p.to == lower;
    });
    while (!flight.empty() && flight.front().to == lower) {
        const auto packet = flight.front();
        flight.erase(flight.begin());
        engine_on_stream(&lower->engine, packet.from, packet.bytes.data(), packet.bytes.size(), 0);
    }
    std::stable_partition(flight.begin(), flight.end(), [](const Wire::Packet& p) {
        Message m{};
        return decode_message(p.bytes.data(), p.bytes.size(), &m) == ParseError::ok &&
               m.command == command::i_am_name_server;
    });
    deliver_all(0);
    CHECK(lower->engine.state == EngineState::open_host);
    CHECK(higher->engine.state == EngineState::open_client && !higher->engine.awaiting_name_server);
    CHECK(higher->engine.name_server_id == lower->engine.system_id);
    CHECK(engine_find_player(&higher->engine, a->engine.system_id) == nullptr);
    CHECK(system_types(&higher->engine) == (Types{destroyed}));
    g_wire = nullptr;
}

// A name server that hears another machine announce itself tells it that
// it is not in the session, and that machine loses the session.
void a_name_server_turns_away_a_rival() {
    current_test = "a_name_server_turns_away_a_rival";
    Wire wire;
    g_wire = &wire;
    auto a = make_node(1, 2300, 41);
    auto b = make_node(2, 2301, 43);
    wire.nodes[0] = a.get();
    wire.nodes[1] = b.get();
    uint32_t a_player = 0, b_player = 0;
    CHECK(host_with_player(a.get(), session_flag::migrate_host, &a_player));
    CHECK(join_with_player(b.get(), &b_player));
    (void)system_types(&b->engine);

    uint8_t announcement[256];
    const Address b_stream{{0, 0, 0, 0}, b->engine.config.stream.port};
    const Address b_datagram{{0, 0, 0, 0}, b->engine.config.datagram.port};
    const std::size_t n = encode_i_am_name_server(
        b_stream,
        a->engine.system_id,
        b->engine.system_id,
        player_flag::system_player | player_flag::name_server | player_flag::in_group,
        b_stream,
        b_datagram,
        announcement,
        sizeof announcement
    );
    CHECK(n == header_bytes + 16 + name_server_addresses_bytes);
    engine_on_stream(&a->engine, b->source, announcement, n, 0);
    Message m{};
    Bytes bytes;
    CHECK(last_sent(command::you_are_dead, &m, &bytes) && bytes.size() == header_bytes);
    CHECK(m.reply_to.port == a->engine.config.stream.port);
    deliver_all(0);
    CHECK(system_types(&b->engine) == (Types{lost}));
    CHECK(b->engine.session_lost && engine_find_player(&b->engine, a_player) == nullptr);
    CHECK(engine_find_player(&b->engine, b_player) != nullptr);
    CHECK(
        a->engine.state == EngineState::open_host && a->engine.name_server_id == a->engine.system_id
    );
    g_wire = nullptr;
}

// Without the migrate-host flag the session is lost when the host leaves.
void without_migration_the_session_is_lost_with_its_host() {
    current_test = "without_migration_the_session_is_lost_with_its_host";
    Wire wire;
    g_wire = &wire;
    auto a = make_node(1, 2300, 51);
    auto b = make_node(2, 2301, 53);
    wire.nodes[0] = a.get();
    wire.nodes[1] = b.get();
    uint32_t a_player = 0, b_player = 0;
    CHECK(host_with_player(a.get(), 0, &a_player));
    CHECK(join_with_player(b.get(), &b_player));
    (void)system_types(&b->engine);
    wire.sent.clear();
    engine_close(&a->engine);
    deliver_all(0);
    CHECK(system_types(&b->engine) == (Types{destroyed, lost}));
    CHECK(b->engine.state == EngineState::open_client && b->engine.session_lost);
    CHECK(count_sent(command::i_am_name_server) == 0);
    g_wire = nullptr;
}

// With the keep-alive flag the name server pings every machine it has not
// heard from since the ping timer last elapsed, 35 s apart; the others ping
// the name server alone. A ping is answered with its player id and tick
// echoed. A machine due its ninth unanswered ping is dropped with its
// players; the other machines, which do not watch it, keep it.
void keep_alive_drops_a_machine_that_stops_answering() {
    current_test = "keep_alive_drops_a_machine_that_stops_answering";
    Wire wire;
    g_wire = &wire;
    auto a = make_node(1, 2300, 61);
    auto b = make_node(2, 2301, 63);
    auto c = make_node(3, 2302, 65);
    wire.nodes[0] = a.get();
    wire.nodes[1] = b.get();
    wire.nodes[2] = c.get();
    uint32_t a_player = 0, b_player = 0, c_player = 0;
    CHECK(
        host_with_player(a.get(), session_flag::migrate_host | session_flag::keep_alive, &a_player)
    );
    CHECK(join_with_player(b.get(), &b_player));
    CHECK(join_with_player(c.get(), &c_player));
    (void)system_types(&a->engine);
    wire.sent.clear();

    uint32_t now = 1000;
    poll_all(now); // the timers start
    CHECK(count_sent(command::ping) == 0);
    now += ping_period_ms - 1;
    poll_all(now);
    CHECK(count_sent(command::ping) == 0);
    now += 1;
    poll_all(now);
    CHECK(count_sent(command::ping, b.get()) == 1 && count_sent(command::ping, c.get()) == 1);
    CHECK(count_sent(command::ping, a.get()) == 2);
    Message m{};
    bool echoed = false;
    for (const auto& packet : wire.sent)
        if (packet.to == a.get() &&
            decode_message(packet.bytes.data(), packet.bytes.size(), &m) == ParseError::ok &&
            m.command == command::ping_reply)
            echoed = echoed || (m.player_id == a->engine.system_id && m.tick == now);
    CHECK(echoed);
    CHECK(count_sent(command::ping_reply) == 4);

    c->silent = true;
    wire.sent.clear();
    bool dropped_early = false;
    for (int period = 0; period < 20 && engine_find_player(&a->engine, c_player) != nullptr;
         ++period) {
        dropped_early = dropped_early || !system_types(&a->engine).empty();
        now += ping_period_ms;
        poll_all(now);
    }
    CHECK(!dropped_early);
    CHECK(count_sent(command::ping, c.get()) == max_unanswered_pings);
    CHECK(engine_find_player(&a->engine, c_player) == nullptr);
    CHECK(engine_find_player(&a->engine, c->engine.system_id) == nullptr);
    CHECK(system_types(&a->engine) == (Types{destroyed}));
    CHECK(engine_find_player(&a->engine, b_player) != nullptr);
    CHECK(engine_find_player(&b->engine, c_player) != nullptr);
    std::size_t pings_from_b = 0;
    for (const auto& packet : wire.sent)
        if (packet.to == c.get() && address_equal(packet.from, b->source) &&
            decode_message(packet.bytes.data(), packet.bytes.size(), &m) == ParseError::ok &&
            m.command == command::ping)
            ++pings_from_b;
    CHECK(pings_from_b == 0);
    g_wire = nullptr;
}

// A machine that stops hearing the name server drops it after eight
// unanswered pings, and, the session allowing it, takes its place.
void keep_alive_replaces_a_silent_name_server() {
    current_test = "keep_alive_replaces_a_silent_name_server";
    Wire wire;
    g_wire = &wire;
    auto a = make_node(1, 2300, 71);
    auto b = make_node(2, 2301, 73);
    wire.nodes[0] = a.get();
    wire.nodes[1] = b.get();
    uint32_t a_player = 0, b_player = 0;
    CHECK(
        host_with_player(a.get(), session_flag::migrate_host | session_flag::keep_alive, &a_player)
    );
    CHECK(join_with_player(b.get(), &b_player));
    (void)system_types(&b->engine);
    a->silent = true;
    wire.sent.clear();
    uint32_t now = 5000;
    poll_all(now);
    for (int period = 0; period < 20 && b->engine.state == EngineState::open_client; ++period) {
        now += ping_period_ms;
        poll_all(now);
    }
    CHECK(count_sent(command::ping, a.get()) == max_unanswered_pings);
    CHECK(
        b->engine.state == EngineState::open_host && b->engine.name_server_id == b->engine.system_id
    );
    CHECK(system_types(&b->engine) == (Types{destroyed, became_host}));
    g_wire = nullptr;
}

// While a new name server is awaited the timer runs without the keep-alive
// flag, pinging every other machine; when the machine expected to take the
// place never answers, it is dropped and the election runs again.
void an_awaited_name_server_that_never_answers_is_dropped() {
    current_test = "an_awaited_name_server_that_never_answers_is_dropped";
    Wire wire;
    g_wire = &wire;
    auto a = make_node(1, 2300, 81);
    auto b = make_node(2, 2301, 83);
    auto c = make_node(3, 2302, 85);
    wire.nodes[0] = a.get();
    wire.nodes[1] = b.get();
    wire.nodes[2] = c.get();
    uint32_t a_player = 0, b_player = 0, c_player = 0;
    CHECK(host_with_player(a.get(), session_flag::migrate_host, &a_player));
    CHECK(join_with_player(b.get(), &b_player));
    CHECK(join_with_player(c.get(), &c_player));
    Node* lower = b->engine.system_id < c->engine.system_id ? b.get() : c.get();
    Node* higher = lower == b.get() ? c.get() : b.get();
    (void)system_types(&higher->engine);

    // Without the keep-alive flag nothing is pinged.
    uint32_t now = 1000;
    poll_all(now);
    poll_all(now + 3 * ping_period_ms);
    CHECK(count_sent(command::ping) == 0);

    lower->silent = true;
    engine_close(&a->engine);
    deliver_all(now);
    CHECK(higher->engine.awaiting_name_server && higher->engine.name_server_id == 0);
    CHECK(system_types(&higher->engine) == (Types{destroyed}));
    wire.sent.clear();
    poll_all(now);
    for (int period = 0; period < 20 && higher->engine.awaiting_name_server; ++period) {
        now += ping_period_ms;
        poll_all(now);
    }
    CHECK(count_sent(command::ping, lower) == max_unanswered_pings);
    CHECK(count_sent(command::ping) == max_unanswered_pings);
    CHECK(higher->engine.state == EngineState::open_host);
    CHECK(system_types(&higher->engine) == (Types{destroyed, became_host}));
    g_wire = nullptr;
}

// A ping from a player the session does not hold is answered by the name
// server with the message that it is not in the session, and ignored by the
// other machines.
void pings_from_strangers() {
    current_test = "pings_from_strangers";
    Wire wire;
    g_wire = &wire;
    auto a = make_node(1, 2300, 91);
    auto b = make_node(2, 2301, 93);
    auto stranger = make_node(9, 2309, 95);
    stranger->silent = true;
    wire.nodes[0] = a.get();
    wire.nodes[1] = b.get();
    wire.nodes[2] = stranger.get();
    uint32_t a_player = 0, b_player = 0;
    CHECK(host_with_player(a.get(), session_flag::migrate_host, &a_player));
    CHECK(join_with_player(b.get(), &b_player));
    wire.sent.clear();
    uint8_t ping[64];
    const std::size_t n =
        encode_ping(stranger->engine.config.stream, command::ping, 0x1234, 77, ping, sizeof ping);
    engine_on_stream(&b->engine, stranger->source, ping, n, 0);
    CHECK(wire.sent.empty());
    engine_on_stream(&a->engine, stranger->source, ping, n, 0);
    CHECK(count_sent(command::you_are_dead, stranger.get()) == 1 && wire.sent.size() == 1);
    const std::size_t reply = encode_ping(
        stranger->engine.config.stream, command::ping_reply, 0x1234, 77, ping, sizeof ping
    );
    engine_on_stream(&a->engine, stranger->source, ping, reply, 0);
    CHECK(count_sent(command::you_are_dead, stranger.get()) == 2);
    g_wire = nullptr;
}

// A machine renaming its player or replacing its data tells every other
// machine, whose players are told with 0x0103 or 0x0102; with the
// no-data-messages flag data changes stay on the machine.
void player_name_and_data_changes_reach_the_other_machines() {
    current_test = "player_name_and_data_changes_reach_the_other_machines";
    Wire wire;
    g_wire = &wire;
    auto a = make_node(1, 2300, 101);
    auto b = make_node(2, 2301, 103);
    auto c = make_node(3, 2302, 105);
    wire.nodes[0] = a.get();
    wire.nodes[1] = b.get();
    wire.nodes[2] = c.get();
    uint32_t a_player = 0, b_player = 0, c_player = 0;
    CHECK(host_with_player(a.get(), session_flag::migrate_host, &a_player));
    CHECK(join_with_player(b.get(), &b_player));
    CHECK(join_with_player(c.get(), &c_player));
    (void)system_types(&a->engine);
    (void)system_types(&b->engine);
    (void)system_types(&c->engine);
    wire.sent.clear();

    CHECK(engine_set_player_name(&b->engine, b_player, "nick", "Nick Name") == result::ok);
    CHECK(count_sent(command::player_name_changed) == 2);
    Message m{};
    Bytes bytes;
    CHECK(last_sent(command::player_name_changed, &m, &bytes));
    CHECK(
        m.id_to == 0 && m.player_id == b_player && m.player.has_short_name && m.player.has_long_name
    );
    CHECK(
        std::strcmp(m.player.short_name, "nick") == 0 &&
        std::strcmp(m.player.long_name, "Nick Name") == 0
    );
    CHECK(bytes.size() == header_bytes + 16 + 2 * (4 + 1) + 2 * (9 + 1));
    CHECK(bytes.size() >= header_bytes + 16 && load_u32(bytes.data() + header_bytes + 8) == 0x18);
    CHECK(
        bytes.size() >= header_bytes + 16 &&
        load_u32(bytes.data() + header_bytes + 12) == 0x18 + 2 * (4 + 1)
    );
    deliver_all(0);
    for (Node* other : {a.get(), c.get()}) {
        const EnginePlayer* copy = engine_find_player(&other->engine, b_player);
        CHECK(copy != nullptr && std::strcmp(copy->info.long_name, "Nick Name") == 0);
        const Bytes image = next_system_message(&other->engine);
        PlayerNameView view{};
        CHECK(image.size() >= 4 && load_u32(image.data()) == name_changed);
        CHECK(decode_player_name_image(image.data(), image.size(), &view) && view.id == b_player);
        CHECK(view.short_name != nullptr && std::strcmp(view.short_name, "nick") == 0);
        CHECK(view.long_name != nullptr && std::strcmp(view.long_name, "Nick Name") == 0);
    }
    CHECK(system_types(&b->engine).empty());

    // A name change may drop a name.
    CHECK(engine_set_player_name(&b->engine, b_player, "solo", nullptr) == result::ok);
    CHECK(last_sent(command::player_name_changed, &m, &bytes) && !m.player.has_long_name);
    CHECK(bytes.size() >= header_bytes + 16 && load_u32(bytes.data() + header_bytes + 12) == 0);
    deliver_all(0);
    {
        const Bytes image = next_system_message(&a->engine);
        PlayerNameView view{};
        CHECK(
            decode_player_name_image(image.data(), image.size(), &view) && view.long_name == nullptr
        );
        CHECK(view.short_name != nullptr && std::strcmp(view.short_name, "solo") == 0);
    }
    CHECK(system_types(&c->engine) == (Types{name_changed}));

    const uint8_t data[3] = {7, 8, 9};
    CHECK(engine_set_player_data(&b->engine, b_player, data, sizeof data) == result::ok);
    CHECK(last_sent(command::player_data_changed, &m, &bytes));
    CHECK(m.player_id == b_player && m.player.data_size == 3 && m.player.data[2] == 9);
    CHECK(bytes.size() == header_bytes + 16 + 3);
    CHECK(bytes.size() >= header_bytes + 16 && load_u32(bytes.data() + header_bytes + 12) == 0x18);
    deliver_all(0);
    (void)system_types(&a->engine);
    {
        const Bytes image = next_system_message(&c->engine);
        PlayerDataView view{};
        CHECK(image.size() >= 4 && load_u32(image.data()) == data_changed);
        CHECK(decode_player_data_image(image.data(), image.size(), &view) && view.id == b_player);
        CHECK(view.data_size == 3 && view.data != nullptr && view.data[0] == 7);
    }
    const EnginePlayer* copy = engine_find_player(&a->engine, b_player);
    CHECK(copy != nullptr && copy->info.data_size == 3 && copy->info.data[1] == 8);

    // Only this machine's application players can be changed here.
    CHECK(engine_set_player_name(&b->engine, a_player, "x", "x") == result::invalid_player);
    CHECK(
        engine_set_player_name(&b->engine, b->engine.system_id, "x", "x") == result::invalid_player
    );
    CHECK(engine_set_player_data(&b->engine, b_player, nullptr, 1) == result::invalid_params);
    CHECK(
        engine_set_player_data(&b->engine, b_player, data, max_player_data_bytes + 1) ==
        result::invalid_params
    );

    // With the no-data-messages flag the change stays here.
    SessionDesc quiet = a->engine.desc;
    quiet.flags |= session_flag::no_data_messages;
    CHECK(engine_set_session_desc(&a->engine, quiet, "Game", "") == result::ok);
    deliver_all(0);
    const std::size_t before = count_sent(command::player_data_changed);
    CHECK(engine_set_player_data(&b->engine, b_player, data, 1) == result::ok);
    CHECK(count_sent(command::player_data_changed) == before);
    g_wire = nullptr;
}

// The name server announcement, the rejection and the name and data
// changes decode as they were encoded; malformed ones are refused.
void name_server_and_player_change_messages_round_trip() {
    current_test = "name_server_and_player_change_messages_round_trip";
    const Address from{{10, 1, 2, 3}, 2305};
    uint8_t out[512];
    Message m{};

    const Address stream{{0, 0, 0, 0}, 2305};
    const Address datagram{{0, 0, 0, 0}, 2355};
    std::size_t n =
        encode_i_am_name_server(from, 0x11, 0x22, 0x7, stream, datagram, out, sizeof out);
    CHECK(n == header_bytes + 16 + name_server_addresses_bytes);
    CHECK(decode_message(out, n, &m) == ParseError::ok && m.command == command::i_am_name_server);
    CHECK(
        m.id_to == 0x11 && m.player_id == 0x22 && m.flags == 0x7 && address_equal(m.reply_to, from)
    );
    CHECK(
        m.player.has_addresses && address_equal(m.player.stream, stream) &&
        address_equal(m.player.datagram, datagram)
    );
    store_u32(out + header_bytes + 12, 0x40);
    CHECK(decode_message(out, n, &m) == ParseError::truncated);
    CHECK(decode_message(out, header_bytes + 8, &m) == ParseError::truncated);
    CHECK(encode_i_am_name_server(from, 0x11, 0x22, 0x7, stream, datagram, out, 0x3b) == 0);

    n = encode_you_are_dead(from, out, sizeof out);
    CHECK(
        n == header_bytes && decode_message(out, n, &m) == ParseError::ok &&
        m.command == command::you_are_dead
    );

    const uint8_t data[5] = {1, 2, 3, 4, 5};
    n = encode_player_data_changed(from, 0, 0x33, data, sizeof data, out, sizeof out);
    CHECK(n == header_bytes + 16 + 5);
    CHECK(
        decode_message(out, n, &m) == ParseError::ok && m.command == command::player_data_changed
    );
    CHECK(
        m.player_id == 0x33 && m.player.data_size == 5 && std::memcmp(m.player.data, data, 5) == 0
    );
    CHECK(decode_message(out, n - 1, &m) == ParseError::truncated);
    store_u32(out + header_bytes + 12, 0x400);
    CHECK(decode_message(out, n, &m) == ParseError::bad_offset);
    CHECK(encode_player_data_changed(from, 0, 0x33, nullptr, 1, out, sizeof out) == 0);
    n = encode_player_data_changed(from, 0, 0x33, nullptr, 0, out, sizeof out);
    CHECK(decode_message(out, n, &m) == ParseError::ok && m.player.data_size == 0);
    uint8_t big[max_player_data_bytes + 1]{};
    n = encode_player_data_changed(from, 0, 0x33, big, sizeof big, out, sizeof out);
    CHECK(decode_message(out, n, &m) == ParseError::unsupported);

    n = encode_player_name_changed(from, 0, 0x44, "ab", "Long", out, sizeof out);
    CHECK(n == header_bytes + 16 + 6 + 10);
    CHECK(
        decode_message(out, n, &m) == ParseError::ok && m.command == command::player_name_changed
    );
    CHECK(
        m.player_id == 0x44 && m.player.has_short_name &&
        std::strcmp(m.player.short_name, "ab") == 0
    );
    CHECK(m.player.has_long_name && std::strcmp(m.player.long_name, "Long") == 0);
    CHECK(decode_message(out, n - 2, &m) == ParseError::truncated);
    store_u32(out + header_bytes + 8, 0x300);
    CHECK(decode_message(out, n, &m) == ParseError::bad_offset);
    n = encode_player_name_changed(from, 0, 0x44, nullptr, nullptr, out, sizeof out);
    CHECK(n == header_bytes + 16 && decode_message(out, n, &m) == ParseError::ok);
    CHECK(!m.player.has_short_name && !m.player.has_long_name);

    // System message images: short, another type or data outside the image.
    uint8_t image[system_message::player_data_bytes + 4]{};
    store_u32(image, data_changed);
    store_u32(image + 4, system_message::player_type_player);
    store_u32(image + system_message::player_data_id, 0x55);
    store_u32(image + system_message::player_data_data, system_message::player_data_bytes);
    store_u32(image + system_message::player_data_size, 4);
    PlayerDataView data_view{};
    CHECK(decode_player_data_image(image, sizeof image, &data_view) && data_view.data_size == 4);
    CHECK(!decode_player_data_image(image, sizeof image - 1, &data_view));
    CHECK(!decode_player_data_image(image, system_message::player_data_bytes - 1, &data_view));
    store_u32(image, name_changed);
    CHECK(!decode_player_data_image(image, sizeof image, &data_view));
    uint8_t names[system_message::player_name_bytes + 2]{};
    store_u32(names, name_changed);
    store_u32(names + 4, system_message::player_type_player);
    store_u32(
        names + system_message::player_name_name + system_message::name_short,
        system_message::player_name_bytes
    );
    names[system_message::player_name_bytes] = 'x';
    PlayerNameView name_view{};
    CHECK(
        decode_player_name_image(names, sizeof names, &name_view) && name_view.long_name == nullptr
    );
    CHECK(!decode_player_name_image(names, sizeof names - 1, &name_view));
}

/// Hosts a game under one application GUID.
///
/// @param host Machine hosting.
/// @param application Application GUID the session description carries.
/// @return True when the host is up.
bool host_under(Node* host, const Guid& application) {
    SessionDesc desc{};
    desc.flags = session_flag::migrate_host;
    std::memcpy(desc.application_guid, application.bytes, 16);
    desc.max_players = 8;
    uint32_t player = 0;
    return engine_host(&host->engine, desc, "Game", "", 0) == result::ok &&
           engine_create_player(&host->engine, "host", "host", nullptr, 0, 0, &player) ==
               result::ok;
}

/// Searches and reports whether exactly one game came back.
///
/// @param joiner Machine searching.
/// @param application Application GUID the search asks for.
/// @return True when one game is listed.
bool finds_one_game(Node* joiner, const Guid& application) {
    if (engine_enum_sessions(&joiner->engine, application, everywhere, 0) != result::ok)
        return false;
    deliver_all(0);
    return joiner->engine.session_count == 1;
}

// A host and a joiner that name the same application GUID see each other.
// A joiner that names Total Annihilation's GUID does not see a host under
// another one, and a joiner under that other GUID does not see a host
// under Total Annihilation's.
void a_search_finds_only_a_game_under_its_application_guid() {
    current_test = "a_search_finds_only_a_game_under_its_application_guid";
    const uint8_t custom_bytes[16] = {
        0xdd,
        0xcc,
        0xbb,
        0xaa,
        0xff,
        0xee,
        0x22,
        0x11,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xaa,
    };
    Guid custom{};
    std::memcpy(custom.bytes, custom_bytes, 16);
    const Guid original = game_application();

    {
        Wire wire;
        g_wire = &wire;
        auto host = make_node(1, 2300, 41);
        auto same = make_node(2, 2301, 43);
        auto other = make_node(3, 2302, 45);
        wire.nodes[0] = host.get();
        wire.nodes[1] = same.get();
        wire.nodes[2] = other.get();
        CHECK(host_under(host.get(), custom));
        CHECK(finds_one_game(same.get(), custom));
        CHECK(
            same->engine.session_count == 1 &&
            std::memcmp(same->engine.sessions[0].desc.application_guid, custom.bytes, 16) == 0
        );
        CHECK(engine_enum_sessions(&other->engine, original, everywhere, 0) == result::ok);
        deliver_all(0);
        CHECK(other->engine.session_count == 0);
        g_wire = nullptr;
    }
    {
        Wire wire;
        g_wire = &wire;
        auto host = make_node(1, 2300, 47);
        auto same = make_node(2, 2301, 49);
        auto other = make_node(3, 2302, 51);
        wire.nodes[0] = host.get();
        wire.nodes[1] = same.get();
        wire.nodes[2] = other.get();
        CHECK(host_under(host.get(), original));
        CHECK(finds_one_game(same.get(), original));
        CHECK(engine_enum_sessions(&other->engine, custom, everywhere, 0) == result::ok);
        deliver_all(0);
        CHECK(other->engine.session_count == 0);
        g_wire = nullptr;
    }
}

// A host and one joiner exchange one player announcement: the joiner's own.
// The host's players travel in the roster, and admitting the joiner tells
// no further machine.
void a_two_player_game_announces_the_joining_player_once() {
    current_test = "a_two_player_game_announces_the_joining_player_once";
    Wire wire;
    g_wire = &wire;
    auto host = make_node(1, 2300, 81);
    auto joiner = make_node(2, 2301, 83);
    wire.nodes[0] = host.get();
    wire.nodes[1] = joiner.get();
    uint32_t host_player = 0;
    uint32_t joiner_player = 0;
    CHECK(host_with_player(host.get(), session_flag::migrate_host, &host_player));
    wire.sent.clear();
    CHECK(join_with_player(joiner.get(), &joiner_player));
    CHECK(count_sent(command::create_player) == 1);
    Message m{};
    CHECK(last_sent(command::create_player, &m));
    CHECK(m.player.id == joiner_player);
    CHECK((m.player.flags & player_flag::system_player) == 0);
    CHECK(has_application_player(host->engine, joiner_player));
    CHECK(has_application_player(joiner->engine, host_player));
    g_wire = nullptr;
}

// Three machines ask to join before the host has admitted any of them. One
// creates its player while the host is admitting another, and that player's
// announcement is still on its way to the host when the new machine's roster
// is built. Each machine that already has a player sends it straight to the
// machine it has just met, and every machine ends with every player.
void a_player_created_during_another_join_reaches_every_machine() {
    current_test = "a_player_created_during_another_join_reaches_every_machine";
    Wire wire;
    g_wire = &wire;
    auto host = make_node(1, 2300, 91);
    auto ann = make_node(2, 2301, 93);
    auto bee = make_node(3, 2302, 95);
    auto cam = make_node(4, 2303, 97);
    wire.nodes[0] = host.get();
    wire.nodes[1] = ann.get();
    wire.nodes[2] = bee.get();
    wire.nodes[3] = cam.get();
    uint32_t host_player = 0;
    CHECK(host_with_player(host.get(), session_flag::migrate_host, &host_player));

    Node* joiners[3] = {ann.get(), bee.get(), cam.get()};
    for (Node* joiner : joiners) {
        CHECK(
            engine_enum_sessions(&joiner->engine, game_application(), everywhere, 0) == result::ok
        );
    }
    deliver_all(0);
    for (Node* joiner : joiners) {
        CHECK(joiner->engine.session_count == 1);
        CHECK(
            engine_join(&joiner->engine, joiner->engine.sessions[0].instance, nullptr, 0) ==
            result::ok
        );
    }
    // All three admissions sit at the host. None has been taken yet.
    deliver_except(is_admission, 0);
    CHECK(g_wire->in_flight.size() == 3);
    CHECK(is_admission(g_wire->in_flight[0]));
    CHECK(is_admission(g_wire->in_flight[1]));
    CHECK(is_admission(g_wire->in_flight[2]));

    // Admit the first. Its roster carries the host's player; it has none yet.
    deliver_front(0);
    deliver_except(is_admission, 0);
    CHECK(engine_join_status(&ann->engine) == result::ok);
    CHECK(g_wire->in_flight.size() == 2);

    // The first creates its player while the other two are still waiting to
    // be admitted. Hold that announcement so the host's next roster is built
    // without the player.
    uint32_t ann_player = 0;
    CHECK(
        engine_create_player(&ann->engine, "ann", "ann", nullptr, 0, 0, &ann_player) ==
        result::connecting
    );
    deliver_except(is_admission_or_announcement, 0);
    CHECK(engine_create_player_status(&ann->engine, &ann_player) == result::ok);
    CHECK(g_wire->in_flight.size() == 3);
    CHECK(is_admission(g_wire->in_flight[0]));
    CHECK(is_admission(g_wire->in_flight[1]));
    CHECK(packet_command(g_wire->in_flight[2]) == command::create_player);
    CHECK(g_wire->in_flight[2].to == host.get());
    Message held{};
    CHECK(
        decode_message(
            g_wire->in_flight[2].bytes.data(), g_wire->in_flight[2].bytes.size(), &held
        ) == ParseError::ok
    );
    CHECK(held.player.id == ann_player);
    CHECK((held.player.flags & player_flag::system_player) == 0);
    CHECK(engine_find_player(&host->engine, ann_player) == nullptr);

    // Admit the second while that announcement has not reached the host.
    deliver_front(0);
    const Wire::Packet* bee_roster = nullptr;
    for (const auto& packet : g_wire->in_flight)
        if (packet.to == bee.get() && packet_command(packet) == command::super_enum_players_reply)
            bee_roster = &packet;
    CHECK(bee_roster != nullptr && !roster_names(*bee_roster, ann_player));
    deliver_all(0);

    // The straight announcement, not the roster, is what told the others.
    CHECK(announced_player_to(bee.get(), ann_player));
    CHECK(announced_player_to(cam.get(), ann_player));
    CHECK(has_application_player(host->engine, ann_player));
    CHECK(has_application_player(bee->engine, ann_player));
    CHECK(has_application_player(cam->engine, ann_player));
    CHECK(engine_join_status(&bee->engine) == result::ok);
    CHECK(engine_join_status(&cam->engine) == result::ok);

    uint32_t bee_player = 0;
    uint32_t cam_player = 0;
    CHECK(
        engine_create_player(&bee->engine, "bee", "bee", nullptr, 0, 0, &bee_player) ==
        result::connecting
    );
    deliver_all(0);
    CHECK(engine_create_player_status(&bee->engine, &bee_player) == result::ok);
    CHECK(
        engine_create_player(&cam->engine, "cam", "cam", nullptr, 0, 0, &cam_player) ==
        result::connecting
    );
    deliver_all(0);
    CHECK(engine_create_player_status(&cam->engine, &cam_player) == result::ok);

    const uint32_t players[4] = {host_player, ann_player, bee_player, cam_player};
    const Engine* machines[4] = {&host->engine, &ann->engine, &bee->engine, &cam->engine};
    for (const Engine* machine : machines)
        for (uint32_t id : players)
            CHECK(has_application_player(*machine, id));
    g_wire = nullptr;
}

} // namespace

int main() {
    engine_rejects_wrong_password_and_full_sessions();
    join_and_description_messages_carry_what_the_game_sends();
    session_desc_changed_codec_round_trips_with_its_tail();
    join_reject_reasons_follow_the_packet_pump();
    host_leaves_and_the_lowest_system_player_takes_its_place();
    an_announcement_ahead_of_the_departure_is_taken();
    a_name_server_turns_away_a_rival();
    without_migration_the_session_is_lost_with_its_host();
    keep_alive_drops_a_machine_that_stops_answering();
    keep_alive_replaces_a_silent_name_server();
    an_awaited_name_server_that_never_answers_is_dropped();
    pings_from_strangers();
    player_name_and_data_changes_reach_the_other_machines();
    name_server_and_player_change_messages_round_trip();
    a_search_finds_only_a_game_under_its_application_guid();
    a_two_player_game_announces_the_joining_player_once();
    a_player_created_during_another_join_reaches_every_machine();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("dplay protocol: all tests passed");
    return 0;
}
