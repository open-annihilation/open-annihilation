// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/overlay_launch.hpp"

#include <cstdint>
#include <string>

namespace oa::boneyards {
namespace {

constexpr char kReserved[] = "{}[]<>:=%";
constexpr std::size_t kRelayHostMax = 64;
constexpr std::size_t kRelayDecodedMax = 80;

/// Finds the first occurrence of needle in text.
///
/// @param text the text to search
/// @param needle the bytes to find
/// @return the index, or npos when needle is absent
std::size_t find_bytes(std::string_view text, std::string_view needle) {
    if (needle.empty() || needle.size() > text.size())
        return std::string_view::npos;
    for (std::size_t index = 0; index + needle.size() <= text.size(); ++index) {
        if (text.substr(index, needle.size()) == needle)
            return index;
    }
    return std::string_view::npos;
}

/// Returns the value of the first `<...:key` field, up to the next `>`.
///
/// @param text the message text
/// @param key the key including the equals sign, such as "relay="
/// @return the value, or nothing when the key is absent
std::optional<std::string_view> value_of(std::string_view text, std::string_view key) {
    const std::size_t at = find_bytes(text, key);
    if (at == std::string_view::npos)
        return std::nullopt;
    const std::size_t start = at + key.size();
    const std::size_t end = text.find('>', start);
    if (end == std::string_view::npos)
        return text.substr(start);
    return text.substr(start, end - start);
}

/// Decodes one percent escape. A `%` and one digit become the reserved byte
/// of that index. Anything else is copied.
///
/// @param text the escaped text
/// @param[out] out receives the decoded bytes
/// @param capacity bytes available in out
/// @return the bytes written, or npos when they do not fit
std::size_t percent_decode(std::string_view text, char* out, std::size_t capacity) {
    std::size_t written = 0;
    for (std::size_t index = 0; index < text.size();) {
        if (written >= capacity)
            return std::string_view::npos;
        const char mark = text[index];
        if (mark == '%' && index + 1 < text.size() && text[index + 1] >= '0' &&
            text[index + 1] <= '9') {
            const std::size_t digit = static_cast<std::size_t>(text[index + 1] - '0');
            if (digit < sizeof kReserved - 1) {
                out[written++] = kReserved[digit];
                index += 2;
                continue;
            }
        }
        out[written++] = mark;
        ++index;
    }
    return written;
}

/// Reads a decimal integer in range, or nothing.
///
/// @param text the digits
/// @param maximum the largest value accepted
/// @param allow_zero whether 0 is a value
/// @return the number, or nothing
std::optional<uint64_t> parse_decimal(std::string_view text, uint64_t maximum, bool allow_zero) {
    if (text.empty())
        return std::nullopt;
    uint64_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        if (value > maximum / 10u)
            return std::nullopt;
        value = value * 10u + static_cast<uint64_t>(digit - '0');
        if (value > maximum)
            return std::nullopt;
    }
    if (!allow_zero && value == 0)
        return std::nullopt;
    return value;
}

/// Reads a dotted overlay address in 10.66.0.0/16.
///
/// @param text the address, with no port
/// @param[out] ip the four octets
/// @return false when text is not such an address
bool parse_overlay_ip(std::string_view text, uint8_t ip[4]) {
    std::size_t parts = 0;
    unsigned current = 0;
    bool any = false;
    for (const char digit : text) {
        if (digit == '.') {
            if (!any || parts >= 3)
                return false;
            ip[parts++] = static_cast<uint8_t>(current);
            current = 0;
            any = false;
            continue;
        }
        if (digit < '0' || digit > '9')
            return false;
        current = current * 10u + static_cast<unsigned>(digit - '0');
        if (current > 255u)
            return false;
        any = true;
    }
    if (!any || parts != 3)
        return false;
    ip[3] = static_cast<uint8_t>(current);
    return ip[0] == 10 && ip[1] == 66;
}

/// Decodes a hex key into out.
///
/// @param text the hex characters
/// @param[out] out the key bytes
/// @param capacity bytes available in out
/// @return the bytes written, or nothing when the text is not a whole key
std::optional<std::size_t> decode_hex(std::string_view text, uint8_t* out, std::size_t capacity) {
    if (text.empty() || text.size() % 2u != 0 || text.size() / 2u > capacity)
        return std::nullopt;
    const auto nybble = [](char digit) -> int {
        if (digit >= '0' && digit <= '9')
            return digit - '0';
        if (digit >= 'a' && digit <= 'f')
            return digit - 'a' + 10;
        if (digit >= 'A' && digit <= 'F')
            return digit - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = nybble(text[index]);
        const int low = nybble(text[index + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        out[index / 2u] =
            static_cast<uint8_t>((static_cast<unsigned>(high) << 4u) | static_cast<unsigned>(low));
    }
    return text.size() / 2u;
}

/// Splits host:port on the last colon. The port is 1 to 65535.
///
/// @param text the decoded endpoint
/// @param[out] host the host, pointing into text
/// @param[out] port the port
/// @return false when the endpoint has no such port
bool split_host_port(std::string_view text, std::string_view& host, uint16_t& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    const std::optional<uint64_t> parsed = parse_decimal(text.substr(colon + 1), 65535u, false);
    if (!parsed.has_value())
        return false;
    host = text.substr(0, colon);
    port = static_cast<uint16_t>(*parsed);
    return true;
}

const OverlayLaunch* g_current_overlay = nullptr;

} // namespace

bool OverlayLaunch::static_peer(uint8_t peer[4]) const noexcept {
    if (creator || !address_present)
        return false;
    for (std::size_t index = 0; index < 4; ++index) {
        if (address[index] != overlay_ip[index]) {
            for (std::size_t octet = 0; octet < 4; ++octet)
                peer[octet] = address[octet];
            return true;
        }
    }
    return false;
}

std::optional<OverlayLaunch> parse_overlay_launch(std::string_view text) {
    const std::size_t begin = find_bytes(text, "command=launch_game");
    if (begin == std::string_view::npos)
        return std::nullopt;
    const std::size_t close = find_bytes(text.substr(begin), "]}");
    const std::string_view message =
        close == std::string_view::npos ? text.substr(begin) : text.substr(begin, close + 2);
    if (const std::optional<std::string_view> result = value_of(message, "result=");
        result.has_value() && *result != "0")
        return std::nullopt;

    const std::optional<std::string_view> overlay = value_of(message, "overlay_ip=");
    if (!overlay.has_value())
        return std::nullopt;
    OverlayLaunch launch;
    if (!parse_overlay_ip(*overlay, launch.overlay_ip))
        return std::nullopt;

    if (const std::optional<std::string_view> address = value_of(message, ":address=");
        address.has_value() && parse_overlay_ip(*address, launch.address))
        launch.address_present = true;

    const std::optional<std::string_view> key_hex = value_of(message, "key=");
    if (!key_hex.has_value())
        return std::nullopt;
    const std::optional<std::size_t> key_len = decode_hex(*key_hex, launch.key, kOverlayKeyMax);
    if (!key_len.has_value())
        return std::nullopt;
    launch.key_len = *key_len;

    const std::optional<std::string_view> key_id = value_of(message, "key_id=");
    const std::optional<uint64_t> key_id_value =
        key_id.has_value() ? parse_decimal(*key_id, 0xffffffffu, true) : std::nullopt;
    if (!key_id_value.has_value())
        return std::nullopt;
    launch.key_id = static_cast<uint32_t>(*key_id_value);

    if (const std::optional<std::string_view> expiry = value_of(message, "key_expiry=");
        expiry.has_value()) {
        if (const std::optional<uint64_t> parsed = parse_decimal(*expiry, UINT64_MAX, true);
            parsed.has_value())
            launch.key_expiry = *parsed;
    }

    const std::optional<std::string_view> relay = value_of(message, "relay=");
    if (!relay.has_value() || relay->size() > kRelayDecodedMax)
        return std::nullopt;
    char decoded[kRelayDecodedMax];
    const std::size_t decoded_len = percent_decode(*relay, decoded, sizeof decoded);
    if (decoded_len == std::string_view::npos)
        return std::nullopt;
    std::string_view host;
    uint16_t port = 0;
    if (!split_host_port(std::string_view(decoded, decoded_len), host, port))
        return std::nullopt;
    if (host.empty() || host.size() > kRelayHostMax)
        return std::nullopt;
    launch.relay_host.assign(host);
    launch.relay_port = port;
    launch.creator = value_of(message, "creator=") == std::optional<std::string_view>("1");
    return launch;
}

const OverlayLaunch* current_overlay_launch() noexcept {
    return g_current_overlay;
}

void set_current_overlay_launch(const OverlayLaunch* launch) noexcept {
    g_current_overlay = launch;
}

} // namespace oa::boneyards
