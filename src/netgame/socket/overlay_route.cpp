// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/overlay_route.hpp"

namespace oa::netgame::sock {
namespace {

OverlayRoute g_route{};
bool g_installed = false;
uint64_t g_direct_packets = 0;

} // namespace

void set_overlay_route(const OverlayRoute* route) noexcept {
    if (route == nullptr) {
        g_installed = false;
        g_route = OverlayRoute{};
        return;
    }
    g_route = *route;
    g_installed = true;
    g_direct_packets = 0;
}

const OverlayRoute* overlay_route() noexcept {
    return g_installed ? &g_route : nullptr;
}

uint64_t overlay_direct_packets() noexcept {
    return g_direct_packets;
}

void note_overlay_direct(const uint8_t ip[4]) noexcept {
    const OverlayRoute* route = overlay_route();
    if (route == nullptr || ip == nullptr)
        return;
    const bool loopback = ip[0] == 127;
    const bool claimed = route->claims != nullptr && route->claims(route->context, ip);
    if (loopback || claimed)
        ++g_direct_packets;
}

} // namespace oa::netgame::sock
