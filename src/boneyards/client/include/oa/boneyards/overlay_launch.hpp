// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The overlay fields of one launch_game record. The text is the record as it
// arrived, which may bundle several messages and may percent-escape the relay
// endpoint. Nothing here opens a connection.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oa::boneyards {

/// Most hex characters accepted for an overlay key.
inline constexpr std::size_t kOverlayKeyHexMax = 128;

/// Most bytes of a decoded overlay key.
inline constexpr std::size_t kOverlayKeyMax = kOverlayKeyHexMax / 2;

/// The status line shown when the relay cannot be reached.
///
/// The spelling is the one the login status already uses.
inline constexpr const char* kRelayFailedText = "Transmision Relay Failure";

/// One usable overlay launch: this client's address, its key and the relay.
struct OverlayLaunch {
    /// This client's overlay address, four octets, first octet first.
    uint8_t overlay_ip[4]{};
    /// The launch address when it is also an overlay address.
    bool address_present{};
    /// The launch address. Meaningful when address_present is set.
    uint8_t address[4]{};
    /// The decoded key. Only the first key_len bytes are the key.
    uint8_t key[kOverlayKeyMax]{};
    /// How many bytes of key are the key.
    std::size_t key_len{};
    /// The key's identifier.
    uint32_t key_id{};
    /// Unix seconds the key expires, or 0 when the record names none.
    uint64_t key_expiry{};
    /// The relay's host name or numeric address, without the port.
    std::string relay_host{};
    /// The relay's port.
    uint16_t relay_port{};
    /// True when this client hosts the battle (creator is 1).
    bool creator{};

    /// Returns the decoded key.
    ///
    /// @return the key bytes
    [[nodiscard]] std::span<const uint8_t> key_bytes() const noexcept {
        return std::span<const uint8_t>(key, key_len);
    }

    /// Returns the peer this record names by itself.
    ///
    /// A joiner's launch address is the host's overlay address, and that is
    /// the peer. The host, or a launch whose address is missing, not an
    /// overlay address, or this client's own address, names no peer: the peer
    /// arrives as the source of the first packet. The peer is never computed
    /// from this client's own address.
    ///
    /// @param[out] peer the peer's overlay address, when there is one
    /// @return false when this record names no peer
    [[nodiscard]] bool static_peer(uint8_t peer[4]) const noexcept;
};

/// Reads an overlay launch from a launch_game record.
///
/// The search starts at command=launch_game and stops at the record's close,
/// so a buffer that holds several messages still yields this launch. A result
/// other than 0, a missing key, key identifier or relay, a key that is not
/// hex, or an overlay address outside 10.66.0.0/16 yields nothing, which is
/// the path a launch without an overlay takes. The relay value may use the
/// record's percent escapes (`%6` is the colon).
///
/// @param text the record text, which need not be terminated
/// @return the launch, or nothing when the record does not carry one
[[nodiscard]] std::optional<OverlayLaunch> parse_overlay_launch(std::string_view text);

/// The overlay launch of the launch_game callback now running, or null.
///
/// The pointer is valid only while that callback runs, through the screens'
/// launch handler. It is null when the record carried no usable overlay.
///
/// @return the launch, or null
[[nodiscard]] const OverlayLaunch* current_overlay_launch() noexcept;

/// Remembers the launch the callback is handing to the screens.
///
/// @param launch the launch, or null to clear it
void set_current_overlay_launch(const OverlayLaunch* launch) noexcept;

} // namespace oa::boneyards
