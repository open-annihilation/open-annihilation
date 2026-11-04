// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether this machine has a network interface that is up, other than the
// loopback. Enter Boneyards asks before it opens the login console.
#pragma once

#include <cstddef>

namespace oa::boneyards {

// Entries the check reads at once. A longer machine list still keeps one up,
// non-loopback interface in the entries that fit, when the machine has one.
inline constexpr std::size_t kNetworkInterfaceCapacity = 64;

// One interface the entry check reads.
struct NetworkInterface {
    bool up;       ///< the interface is up
    bool loopback; ///< the interface is the machine's loopback
};

/// Writes interfaces into `out`, up to `capacity`, and returns how many it wrote.
///
/// A count above `capacity` is not written. The pointer may be null when the
/// list keeps no context of its own.
using NetworkInterfaceList =
    std::size_t (*)(void* context, NetworkInterface* out, std::size_t capacity);

/// Tells whether any interface in the list is up and is not loopback.
///
/// A null list, or a count of zero, is no such interface.
///
/// @param interfaces interfaces to read; null is an empty list
/// @param count entries at `interfaces`
/// @return true when one of them is up and is not loopback
bool network_link_present(const NetworkInterface* interfaces, std::size_t count);

/// Names the list the entry check reads.
///
/// A null list restores this machine's own interfaces. The list is asked on
/// the thread that checks, and it must not throw.
///
/// @param list called with `context` to fill a buffer, or null
/// @param context passed back to `list`
void rover_session_set_network_interfaces(NetworkInterfaceList list, void* context);

/// Tells whether an interface other than the loopback is up.
///
/// Reads the list named with rover_session_set_network_interfaces, or this
/// machine's interfaces when none is named. A list that reports more entries
/// than the check holds is read only up to that limit.
///
/// @return true when such an interface is up
bool network_link_available();

} // namespace oa::boneyards
