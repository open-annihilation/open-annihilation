// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// HMAC-SHA256 (RFC 2104) on the engine's SHA-256. A key longer than one block
// is hashed before it is used. The comparison of two tags takes the same time
// for equal-length tags whatever their contents.

#include "oa/base/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::boneyards::tunnel {

/// Computes HMAC-SHA256 of one message.
///
/// @param key the key, of any length
/// @param message the message
/// @return the 32-byte tag
[[nodiscard]] oa::base::sha256::Digest
hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> message) noexcept;

/// Computes HMAC-SHA256 of two message parts, as if they were one buffer.
///
/// @param key the key, of any length
/// @param part_one the first part
/// @param part_two the second part, which may be empty
/// @return the 32-byte tag
[[nodiscard]] oa::base::sha256::Digest hmac_sha256_two(
    std::span<const uint8_t> key,
    std::span<const uint8_t> part_one,
    std::span<const uint8_t> part_two
) noexcept;

/// Compares two tags without stopping at the first differing byte.
///
/// @param one the first tag
/// @param other the second tag
/// @return false when the lengths differ or any byte differs
[[nodiscard]] bool same_hmac(std::span<const uint8_t> one, std::span<const uint8_t> other) noexcept;

} // namespace oa::boneyards::tunnel
