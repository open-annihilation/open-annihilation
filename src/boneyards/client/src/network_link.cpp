// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The machine's interfaces, other than the loopback, for the entry check.
#include "oa/boneyards/network_link.hpp"

#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace oa::boneyards {
namespace {

NetworkInterfaceList g_list = nullptr;
void* g_list_context = nullptr;

/// Keeps `item` in `out`.
///
/// While `out` has room the item is appended. Once it is full, an up
/// non-loopback item replaces the last entry, so a longer list still
/// reports a usable interface when it has one.
///
/// @param[in,out] out interfaces already kept
/// @param capacity entries `out` can hold
/// @param written entries already kept
/// @param item interface to keep
/// @return entries kept after `item`
std::size_t keep_interface(
    NetworkInterface* out, std::size_t capacity, std::size_t written, NetworkInterface item
) {
    if (written < capacity) {
        out[written] = item;
        return written + 1;
    }
    if (item.up && !item.loopback)
        out[capacity - 1] = item;
    return written;
}

#if defined(_WIN32)
/// Writes the machine's interfaces from the adapter list.
///
/// An adapter whose operational state is up has `up` set. The software
/// loopback is marked loopback. A list the system will not give is empty.
///
/// @param[out] out receives the interfaces
/// @param capacity entries `out` can hold
/// @return how many were written
std::size_t list_machine_interfaces(void*, NetworkInterface* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return 0;
    constexpr ULONG kByteLimit = 1u << 20;
    ULONG size = 16 * 1024;
    auto* bytes = static_cast<unsigned char*>(std::malloc(size));
    if (bytes == nullptr)
        return 0;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(
        AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes), &size
    );
    if (result == ERROR_BUFFER_OVERFLOW && size > 0 && size <= kByteLimit) {
        std::free(bytes);
        bytes = static_cast<unsigned char*>(std::malloc(size));
        if (bytes == nullptr)
            return 0;
        result = GetAdaptersAddresses(
            AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes), &size
        );
    }
    std::size_t written = 0;
    if (result == ERROR_SUCCESS) {
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes); adapter != nullptr;
             adapter = adapter->Next) {
            NetworkInterface item{};
            item.up = adapter->OperStatus == IfOperStatusUp;
            item.loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
            written = keep_interface(out, capacity, written, item);
        }
    }
    std::free(bytes);
    return written;
}
#else
/// Writes the machine's interfaces from the interface address list.
///
/// An interface with the up flag has `up` set, and one with the loopback
/// flag is marked loopback. A list the system will not give is empty.
///
/// @param[out] out receives the interfaces
/// @param capacity entries `out` can hold
/// @return how many were written
std::size_t list_machine_interfaces(void*, NetworkInterface* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return 0;
    ifaddrs* listed = nullptr;
    if (getifaddrs(&listed) != 0)
        return 0;
    std::size_t written = 0;
    for (const ifaddrs* entry = listed; entry != nullptr; entry = entry->ifa_next) {
        NetworkInterface item{};
        item.up = (entry->ifa_flags & IFF_UP) != 0;
        item.loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;
        written = keep_interface(out, capacity, written, item);
    }
    freeifaddrs(listed);
    return written;
}
#endif

} // namespace

bool network_link_present(const NetworkInterface* interfaces, std::size_t count) {
    if (interfaces == nullptr)
        return false;
    for (std::size_t i = 0; i < count; ++i)
        if (interfaces[i].up && !interfaces[i].loopback)
            return true;
    return false;
}

void rover_session_set_network_interfaces(NetworkInterfaceList list, void* context) {
    g_list = list;
    g_list_context = context;
}

bool network_link_available() {
    NetworkInterface listed[kNetworkInterfaceCapacity]{};
    const NetworkInterfaceList list = g_list != nullptr ? g_list : list_machine_interfaces;
    void* context = g_list != nullptr ? g_list_context : nullptr;
    std::size_t count = list(context, listed, kNetworkInterfaceCapacity);
    if (count > kNetworkInterfaceCapacity)
        count = kNetworkInterfaceCapacity;
    return network_link_present(listed, count);
}

} // namespace oa::boneyards
