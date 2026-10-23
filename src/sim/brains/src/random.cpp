// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seed of one brain's own xoshiro256** stream, and brain.random.
// The stream is the sandbox's math.random. brain.random is that function
// as it stood when the profile installed, so a script that replaces
// math.random still draws from the same stream through brain.random.

#include "internal.hpp"

#include "oa/base/sha256.hpp"

#include "lua.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace oa::sim::brains {
namespace {

void feed(oa::base::sha256::Hasher& hasher, std::string_view text) noexcept {
    if (text.empty())
        return;
    oa::base::sha256::update(
        hasher, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size())
    );
}

void feed_byte(oa::base::sha256::Hasher& hasher, uint8_t byte) noexcept {
    oa::base::sha256::update(hasher, std::span<const uint8_t>(&byte, 1));
}

} // namespace

uint64_t brain_random_seed(
    uint32_t match_seed, uint8_t player, std::string_view id, std::string_view version
) noexcept {
    oa::base::sha256::Hasher hasher;
    uint8_t seed_bytes[8]{};
    for (int shift = 0; shift < 4; ++shift)
        seed_bytes[shift] = static_cast<uint8_t>((match_seed >> (8 * shift)) & 0xffu);
    oa::base::sha256::update(hasher, std::span<const uint8_t>(seed_bytes, 8));
    feed_byte(hasher, player);
    feed(hasher, id);
    feed_byte(hasher, 0);
    feed(hasher, version);
    const oa::base::sha256::Digest digest = oa::base::sha256::finish(hasher);
    uint64_t mixed = 0;
    for (int shift = 0; shift < 8; ++shift) {
        mixed |= static_cast<uint64_t>(digest[static_cast<std::size_t>(shift)]) << (8 * shift);
    }
    return mixed;
}

void install_random(lua_State* state) {
    if (state == nullptr)
        return;
    lua_getglobal(state, "math");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return;
    }
    lua_getfield(state, -1, "random");
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 2);
        return;
    }
    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 3);
        return;
    }
    lua_pushvalue(state, -2);
    lua_setfield(state, -2, "random");
    lua_pop(state, 3);
}

} // namespace oa::sim::brains
