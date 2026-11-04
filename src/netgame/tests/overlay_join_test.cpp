// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A host and a client find, join and play one game through an installed
// route, as a battle carried by a relay does. Each machine has its own route
// with its own overlay address, and the test carries every packet one route
// hands over to the route of the address it names, the way a relay does. Both
// machines bind their sockets to any free port (a DirectPlay port of 0), so the
// client has no port of its own to look for the host at.

#include "oa/netgame/dplay.hpp"
#include "oa/netgame/dplay/engine.hpp"
#include "oa/netgame/overlay_route.hpp"
#include "oa/netgame/session.hpp"
#include "oa/netgame/socket_host.hpp"
#include "oa/netgame/wire.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

using namespace oa::netgame;
namespace sock = oa::netgame::sock;
namespace ses = oa::netgame::session;

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

constexpr uint32_t wait_limit_ms = 5000;
constexpr std::size_t machine_count = 2;

/// One packet on its way between two routes.
struct Carried {
    uint8_t source[4]{};
    uint16_t source_port{};
    uint8_t destination[4]{};
    uint16_t destination_port{};
    bool stream{};
    std::vector<uint8_t> bytes;
};

/// One machine's route: its overlay address and what waits for it.
struct TestRoute {
    uint8_t self[4]{};
    std::deque<Carried> waiting;
    std::vector<uint8_t> held;
    /// Session searches this machine sent, and the port each went to.
    std::vector<uint16_t> search_ports;
};

TestRoute g_routes[machine_count]{};
sock::Host* g_hosts[machine_count]{};
std::size_t g_driven = 0;

/// The route whose overlay address is ip, or null.
TestRoute* route_at(const uint8_t ip[4]) {
    for (auto& route : g_routes)
        if (std::memcmp(route.self, ip, 4) == 0)
            return &route;
    return nullptr;
}

bool claims(void*, const uint8_t ip[4]) {
    return ip[0] == 10 && ip[1] == 66;
}

bool self_address(void* context, uint8_t ip[4]) {
    std::memcpy(ip, static_cast<TestRoute*>(context)->self, 4);
    return true;
}

/// Hands a packet to the route of the address it names, as the relay does.
bool carry(
    TestRoute* from,
    const uint8_t to[4],
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t* bytes,
    std::size_t size,
    bool stream
) {
    TestRoute* target = route_at(to);
    if (target == nullptr)
        return true; // the relay drops it, as it does for an address no one holds
    Carried packet;
    std::memcpy(packet.source, from->self, 4);
    packet.source_port = source_port;
    std::memcpy(packet.destination, to, 4);
    packet.destination_port = destination_port;
    packet.stream = stream;
    if (size != 0)
        packet.bytes.assign(bytes, bytes + size);
    target->waiting.push_back(std::move(packet));
    return true;
}

bool send_datagram(
    void* context,
    const uint8_t to[4],
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t* bytes,
    std::size_t size
) {
    auto* from = static_cast<TestRoute*>(context);
    DplayEnvelope envelope{};
    if (decode_dplay_envelope(bytes, size, &envelope, nullptr) == WireError::ok &&
        envelope.has_play_header &&
        envelope.command == static_cast<uint16_t>(DplayCommand::enum_sessions))
        from->search_ports.push_back(destination_port);
    return carry(from, to, source_port, destination_port, bytes, size, false);
}

bool send_stream(
    void* context,
    const uint8_t to[4],
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t* bytes,
    std::size_t size
) {
    return carry(
        static_cast<TestRoute*>(context), to, source_port, destination_port, bytes, size, true
    );
}

bool take(void* context, sock::OverlayPacket* packet) {
    auto* route = static_cast<TestRoute*>(context);
    if (route->waiting.empty())
        return false;
    Carried next = std::move(route->waiting.front());
    route->waiting.pop_front();
    route->held = std::move(next.bytes);
    std::memcpy(packet->source, next.source, 4);
    packet->source_port = next.source_port;
    std::memcpy(packet->destination, next.destination, 4);
    packet->destination_port = next.destination_port;
    packet->stream = next.stream;
    packet->bytes = route->held.empty() ? nullptr : route->held.data();
    packet->size = route->held.size();
    return true;
}

/// Installs one machine's route, as that machine's battle does.
///
/// @param machine index of the machine whose session is driven next
void drive(std::size_t machine) {
    g_driven = machine;
    sock::OverlayRoute route{};
    route.context = &g_routes[machine];
    route.claims = claims;
    route.self_address = self_address;
    route.send_datagram = send_datagram;
    route.send_stream = send_stream;
    route.take = take;
    sock::set_overlay_route(&route);
}

/// Pumps every machine with its own route, then puts back the driven machine's.
void pump_all(void*, uint32_t wait_ms) {
    const std::size_t driven = g_driven;
    for (std::size_t i = 0; i < machine_count; ++i) {
        if (g_hosts[i] == nullptr)
            continue;
        drive(i);
        sock::host_pump(g_hosts[i], wait_ms / machine_count);
    }
    drive(driven);
}

ses::Backend backend_for(sock::Host* host) {
    ses::Backend b = sock::host_session_backend(host);
    b.pump = pump_all;
    return b;
}

uint32_t g_now_ms = 0;
int64_t g_in_flight = 0;

sock::HostClock shared_clock() {
    sock::HostClock clock;
    clock.now_ms = [](void*) { return g_now_ms; };
    clock.advance = [](void*, uint32_t ms) { g_now_ms += ms; };
    clock.in_flight = &g_in_flight;
    return clock;
}

uint32_t elapsed_since(std::chrono::steady_clock::time_point start) {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start
    )
                                     .count());
}

struct Received {
    uint32_t from{};
    uint32_t to{};
    std::vector<uint8_t> bytes;
};

/// Next message through a machine's transport, pumping every machine until one arrives.
bool receive(std::size_t machine, const NetTransport& transport, Received* out) {
    const auto start = std::chrono::steady_clock::now();
    uint8_t buffer[0x10000];
    while (elapsed_since(start) < wait_limit_ms) {
        drive(machine);
        uint32_t size = sizeof buffer;
        const uint32_t r =
            transport.receive(transport.context, &out->from, &out->to, buffer, &size);
        if (r == transport_result::ok) {
            out->bytes.assign(buffer, buffer + size);
            return true;
        }
        pump_all(nullptr, 10);
    }
    return false;
}

bool receive_system(
    std::size_t machine, const NetTransport& transport, SystemMessageType type, Received* out
) {
    while (receive(machine, transport, out))
        if (out->from == system_message_sender_id && out->bytes.size() >= 4 &&
            load_u32(out->bytes.data()) == static_cast<uint32_t>(type))
            return true;
    return false;
}

/// A message other than a system message, pumping every machine until one arrives.
bool receive_player(std::size_t machine, const NetTransport& transport, Received* out) {
    while (receive(machine, transport, out))
        if (out->from != system_message_sender_id)
            return true;
    return false;
}

// A host and a client whose DirectPlay ports are 0 meet through their routes.
// The client looks for the host at the overlay address its launch names: its
// search goes through the route to the standard port, the host answers it,
// the client joins, and the two machines exchange messages both ways, sent
// guaranteed and not.
void client_joins_host_through_routes_on_free_ports() {
    current_test = "client_joins_host_through_routes_on_free_ports";
    const uint8_t host_overlay[4] = {10, 66, 1, 10};
    const uint8_t client_overlay[4] = {10, 66, 1, 11};
    std::memcpy(g_routes[0].self, host_overlay, 4);
    std::memcpy(g_routes[1].self, client_overlay, 4);
    auto host_machine = std::make_unique<sock::Host>();
    auto client_machine = std::make_unique<sock::Host>();
    g_hosts[0] = host_machine.get();
    g_hosts[1] = client_machine.get();

    sock::HostConfig config{};
    config.clock = shared_clock();
    const uint8_t loopback[4] = {127, 0, 0, 1};
    std::memcpy(config.bind_ip, loopback, 4);
    config.stream_port_first = config.datagram_port_first = config.enum_port = 0;
    std::memcpy(config.enum_target, host_overlay, 4);
    config.seed = 0x31;
    CHECK(sock::host_open(host_machine.get(), config));
    config.seed = 0x37;
    CHECK(sock::host_open(client_machine.get(), config));
    // The client was told no port to look for games at.
    CHECK(client_machine->engine.config.enum_port == 0);

    dplay::Guid app{};
    std::memcpy(app.bytes, application_guid, 16);
    auto host = std::make_unique<ses::Session>();
    auto client = std::make_unique<ses::Session>();
    ses::session_init_multiplay(host.get(), backend_for(host_machine.get()), app);
    ses::session_init_defaults(host.get());
    host->max_players = 4;
    ses::session_init_multiplay(client.get(), backend_for(client_machine.get()), app);
    ses::session_init_defaults(client.get());

    drive(0);
    CHECK(ses::session_create_game(host.get(), "Relay Game", "", 0x0e010400, 0xd, 0, 0));
    uint32_t host_player = 0;
    CHECK(
        ses::session_add_player(
            host.get(), &host_player, "hoster", "hoster", "", 0, create_player_version_high
        )
    );

    drive(1);
    ses::GameEntry games[ses::max_game_entries]{};
    CHECK(ses::session_get_games(client.get(), games, ses::max_game_entries) == 1);
    CHECK(std::strcmp(games[0].session_name, "Relay Game") == 0);
    // The search went through the route to the standard port.
    CHECK(!g_routes[1].search_ports.empty());
    for (const uint16_t port : g_routes[1].search_ports)
        CHECK(port == dplay::enum_port);
    // The host's answer named the client's overlay address and its stream port.
    const auto& heard = client_machine->engine.message;
    CHECK(heard.command == dplay::command::enum_sessions_reply);
    CHECK(std::memcmp(heard.reply_to.ip, host_overlay, 4) == 0);
    CHECK(heard.reply_to.port == host_machine->engine.config.stream.port);

    drive(1);
    CHECK(ses::session_join_game(client.get(), games[0].instance));
    CHECK(client_machine->engine.name_server_id == host_machine->engine.system_id);
    CHECK(dplay::engine_find_player(&client_machine->engine, host_player) != nullptr);
    uint32_t client_player = 0;
    CHECK(
        ses::session_add_player(
            client.get(), &client_player, "joiner", "joiner", "", 0, create_player_version_high
        )
    );
    CHECK(client_player != 0 && client_player != host_player);

    const NetTransport host_transport = ses::session_transport(host.get());
    const NetTransport client_transport = ses::session_transport(client.get());
    Received created;
    CHECK(receive_system(0, host_transport, SystemMessageType::player_created, &created));
    CHECK(created.to == host_player);
    CHECK(dplay::engine_player_count(&host_machine->engine) == 2);
    CHECK(dplay::engine_player_count(&client_machine->engine) == 2);

    // Guaranteed (a stream) and unguaranteed (a datagram), each way.
    const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    const uint8_t again[] = {'a', 'g', 'a', 'i', 'n'};

    const struct {
        std::size_t from_machine;
        const NetTransport* from;
        uint32_t from_id;
        std::size_t to_machine;
        const NetTransport* to;
        uint32_t to_id;
        uint32_t flags;
        const uint8_t* bytes;
    } exchanges[] = {
        {1,
         &client_transport,
         client_player,
         0,
         &host_transport,
         host_player,
         send_flag_guaranteed,
         hello},
        {1, &client_transport, client_player, 0, &host_transport, host_player, 0, again},
        {0,
         &host_transport,
         host_player,
         1,
         &client_transport,
         client_player,
         send_flag_guaranteed,
         hello},
        {0, &host_transport, host_player, 1, &client_transport, client_player, 0, again},
    };

    for (const auto& exchange : exchanges) {
        drive(exchange.from_machine);
        CHECK(
            exchange.from->send(
                exchange.from->context,
                exchange.from_id,
                exchange.to_id,
                exchange.flags,
                exchange.bytes,
                5
            ) == transport_result::ok
        );
        Received got;
        CHECK(receive_player(exchange.to_machine, *exchange.to, &got));
        CHECK(got.from == exchange.from_id && got.to == exchange.to_id);
        CHECK(got.bytes.size() == 5 && std::memcmp(got.bytes.data(), exchange.bytes, 5) == 0);
    }
    drive(1);
    CHECK(ses::session_quit_game(client.get()));
    Received destroyed;
    CHECK(receive_system(0, host_transport, SystemMessageType::player_destroyed, &destroyed));

    drive(0);
    ses::session_uninit(host.get());
    drive(1);
    ses::session_uninit(client.get());
    sock::host_close(host_machine.get());
    sock::host_close(client_machine.get());
    sock::set_overlay_route(nullptr);
    for (auto& machine : g_hosts)
        machine = nullptr;
}

// Without a route, a client that was told no port to look for games at sends
// no search at all, as before routes existed.
void client_on_a_free_port_sends_no_search_without_a_route() {
    current_test = "client_on_a_free_port_sends_no_search_without_a_route";
    sock::set_overlay_route(nullptr);
    auto client_machine = std::make_unique<sock::Host>();
    g_hosts[1] = client_machine.get();
    sock::HostConfig config{};
    config.clock = shared_clock();
    const uint8_t loopback[4] = {127, 0, 0, 1};
    std::memcpy(config.bind_ip, loopback, 4);
    std::memcpy(config.enum_target, loopback, 4);
    config.stream_port_first = config.datagram_port_first = config.enum_port = 0;
    config.seed = 0x41;
    CHECK(sock::host_open(client_machine.get(), config));
    dplay::Guid app{};
    std::memcpy(app.bytes, application_guid, 16);
    CHECK(
        dplay::engine_enum_sessions(&client_machine->engine, app, loopback, g_now_ms) ==
        dplay::result::no_connection
    );
    sock::host_close(client_machine.get());
    g_hosts[1] = nullptr;
}

} // namespace

int main() {
    client_joins_host_through_routes_on_free_ports();
    client_on_a_free_port_sends_no_search_without_a_route();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("overlay join: all tests passed");
    return 0;
}
