// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The relay tunnel as the route one battle installs. Datagrams that do not
// fit in one relay record are refused. A stream is split into records the
// relay accepts, and the far side joins those records back into one stream.
#include "boneyards_launch.hpp"

#include "oa/boneyards/overlay_launch.hpp"
#include "oa/boneyards/tunnel.hpp"
#include "oa/boneyards/tunnel/stream.hpp"
#include "oa/netgame/overlay_route.hpp"

#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace oa::app {

void boneyards_console_show_notice(const char* text);

namespace {

namespace api = oa::app::netgame::extension_api;
namespace tunnel = oa::boneyards::tunnel;

struct RouteState {
    tunnel::Tunnel tunnel{};
    oa::boneyards::OverlayLaunch launch{};
    std::vector<uint8_t> held{};
};

std::unique_ptr<RouteState> g_state;
bool g_failure_shown = false;

/// Shows the relay-failure line once until the route is removed.
void show_relay_failure() {
    if (g_failure_shown)
        return;
    g_failure_shown = true;
    boneyards_console_show_notice(oa::boneyards::kRelayFailedText);
    api::post_battle_line(oa::boneyards::kRelayFailedText);
}

/// True for an address in 10.66.0.0/16.
///
/// @param context unused
/// @param ip the address
/// @return true when the route carries ip
bool route_claims(void* /*context*/, const uint8_t ip[4]) {
    return ip != nullptr && ip[0] == 10 && ip[1] == 66;
}

/// Writes this client's overlay address.
///
/// @param context the route state
/// @param[out] ip the address
/// @return false when the state is missing
bool route_self(void* context, uint8_t ip[4]) {
    const auto* state = static_cast<const RouteState*>(context);
    if (state == nullptr || ip == nullptr)
        return false;
    std::memcpy(ip, state->launch.overlay_ip, 4);
    return true;
}

tunnel::OverlayIp address_of(const uint8_t ip[4]) {
    tunnel::OverlayIp overlay{};
    if (ip != nullptr)
        std::memcpy(overlay.octet, ip, 4);
    return overlay;
}

/// Carries one datagram. A datagram larger than one record is refused.
bool route_send_datagram(
    void* context,
    const uint8_t to[4],
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t* bytes,
    std::size_t size
) {
    auto* state = static_cast<RouteState*>(context);
    if (state == nullptr || size > tunnel::max_payload_bytes)
        return false;
    const std::span<const uint8_t> payload(size == 0 ? nullptr : bytes, size);
    return state->tunnel.send_datagram(address_of(to), source_port, destination_port, payload);
}

/// Carries a stream, split into records the relay accepts.
bool route_send_stream(
    void* context,
    const uint8_t to[4],
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t* bytes,
    std::size_t size
) {
    auto* state = static_cast<RouteState*>(context);
    if (state == nullptr)
        return false;
    const tunnel::OverlayIp destination = address_of(to);
    std::size_t offset = 0;
    do {
        const std::size_t chunk =
            size - offset > tunnel::max_payload_bytes ? tunnel::max_payload_bytes : size - offset;
        const std::span<const uint8_t> payload(chunk == 0 ? nullptr : bytes + offset, chunk);
        if (!state->tunnel.send_stream(destination, source_port, destination_port, payload))
            return false;
        offset += chunk;
    } while (offset < size);
    return true;
}

/// Takes the next packet. Its bytes stay valid until the next take or pump.
bool route_take(void* context, oa::netgame::sock::OverlayPacket* packet) {
    auto* state = static_cast<RouteState*>(context);
    if (state == nullptr || packet == nullptr)
        return false;
    tunnel::Packet taken;
    if (!state->tunnel.take(taken))
        return false;
    state->held = std::move(taken.bytes);
    std::memcpy(packet->source, taken.source.octet, 4);
    packet->source_port = taken.source_port;
    std::memcpy(packet->destination, taken.destination.octet, 4);
    packet->destination_port = taken.destination_port;
    packet->stream = taken.channel == tunnel::Channel::stream;
    packet->bytes = state->held.empty() ? nullptr : state->held.data();
    packet->size = state->held.size();
    return true;
}

void route_pump(void* /*context*/) {
    boneyards_pump_overlay_route();
}

} // namespace

void boneyards_install_overlay_route(const oa::boneyards::OverlayLaunch& launch) noexcept {
    boneyards_remove_overlay_route();
    auto state = std::make_unique<RouteState>();
    state->launch = launch;
    tunnel::OverlayIp self{};
    std::memcpy(self.octet, launch.overlay_ip, 4);
    if (!state->tunnel.connect(
            launch.relay_host, launch.relay_port, self, launch.key_bytes(), launch.key_id
        )) {
        show_relay_failure();
        return;
    }
    g_state = std::move(state);
    oa::netgame::sock::OverlayRoute route{};
    route.context = g_state.get();
    route.claims = route_claims;
    route.self_address = route_self;
    route.send_datagram = route_send_datagram;
    route.send_stream = route_send_stream;
    route.take = route_take;
    route.pump = route_pump;
    api::set_overlay_route(&route);
}

void boneyards_remove_overlay_route() noexcept {
    api::set_overlay_route(nullptr);
    if (g_state != nullptr) {
        g_state->tunnel.close();
        g_state.reset();
    }
    g_failure_shown = false;
}

void boneyards_match_ended() noexcept {
    boneyards_remove_overlay_route();
}

bool boneyards_match_event_leaves(uint8_t event) noexcept {
    // MatchEvent::finished is 0 and MatchEvent::left is 2. torn_down (1) also
    // runs from bootstrap_match before the battle starts, so it is not a way
    // out; a real leave has already sent left.
    constexpr uint8_t finished = 0;
    constexpr uint8_t left = 2;
    return event == finished || event == left;
}

void boneyards_overlay_lost(bool match_still_played) noexcept {
    if (match_still_played)
        return;
    boneyards_remove_overlay_route();
}

void boneyards_runtime_released() noexcept {
    boneyards_remove_overlay_route();
}

void boneyards_pump_overlay_route() noexcept {
    if (g_state == nullptr)
        return;
    g_state->tunnel.pump();
    const tunnel::Fault fault = g_state->tunnel.fault();
    if (fault == tunnel::Fault::cannot_connect || fault == tunnel::Fault::cannot_resolve)
        show_relay_failure();
}

} // namespace oa::app
