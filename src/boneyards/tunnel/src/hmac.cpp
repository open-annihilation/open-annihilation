// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// HMAC-SHA256 on oa::base::sha256. The inner hash is the key pad then the
// message; the outer hash is the other pad then that digest.
#include "oa/boneyards/tunnel/hmac.hpp"

#include <array>

namespace oa::boneyards::tunnel {

using oa::base::sha256::block_size;
using oa::base::sha256::Digest;
using oa::base::sha256::digest_of;
using oa::base::sha256::finish;
using oa::base::sha256::Hasher;
using oa::base::sha256::update;

namespace {

/// Folds a key down to one block: a long key is hashed, a short key is copied.
///
/// @param key the caller's key
/// @param[out] block the block, zeroed by the caller
void key_block(std::span<const uint8_t> key, std::span<uint8_t> block) {
    if (key.size() > block_size) {
        const Digest hashed = digest_of(key);
        for (size_t index = 0; index < hashed.size(); ++index)
            block[index] = hashed[index];
        return;
    }
    for (size_t index = 0; index < key.size(); ++index)
        block[index] = key[index];
}

} // namespace

Digest hmac_sha256_two(
    std::span<const uint8_t> key,
    std::span<const uint8_t> part_one,
    std::span<const uint8_t> part_two
) noexcept {
    std::array<uint8_t, block_size> reduced{};
    key_block(key, reduced);

    std::array<uint8_t, block_size> inner_pad{};
    std::array<uint8_t, block_size> outer_pad{};
    for (size_t index = 0; index < block_size; ++index) {
        inner_pad[index] = static_cast<uint8_t>(reduced[index] ^ 0x36);
        outer_pad[index] = static_cast<uint8_t>(reduced[index] ^ 0x5c);
    }

    Hasher inner;
    update(inner, inner_pad);
    update(inner, part_one);
    update(inner, part_two);
    const Digest inner_digest = finish(inner);

    Hasher outer;
    update(outer, outer_pad);
    update(outer, inner_digest);
    return finish(outer);
}

Digest hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> message) noexcept {
    return hmac_sha256_two(key, message, {});
}

bool same_hmac(std::span<const uint8_t> one, std::span<const uint8_t> other) noexcept {
    if (one.size() != other.size())
        return false;
    uint8_t difference = 0;
    for (size_t index = 0; index < one.size(); ++index)
        difference = static_cast<uint8_t>(difference | (one[index] ^ other[index]));
    return difference == 0;
}

} // namespace oa::boneyards::tunnel
