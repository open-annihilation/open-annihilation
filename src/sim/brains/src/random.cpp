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
#include <cstring>
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

int logged_math_random(lua_State* state) {
    auto* seat = static_cast<Seat*>(lua_touserdata(state, lua_upvalueindex(2)));
    const int argc = lua_gettop(state);
    int64_t low = 0;
    int64_t high = 0;
    if (argc == 1 && lua_isinteger(state, 1) != 0)
        high = static_cast<int64_t>(lua_tointeger(state, 1));
    else if (argc >= 2 && lua_isinteger(state, 1) != 0 && lua_isinteger(state, 2) != 0) {
        low = static_cast<int64_t>(lua_tointeger(state, 1));
        high = static_cast<int64_t>(lua_tointeger(state, 2));
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, argc, 1);
    if (seat == nullptr || !seat->log_open)
        return 1;
    if (argc <= 0) {
        if (lua_type(state, -1) == LUA_TNUMBER) {
            const double value = lua_tonumber(state, -1);
            uint64_t bits = 0;
            static_assert(sizeof(double) == sizeof(uint64_t), "a double is 64 bits");
            std::memcpy(&bits, &value, sizeof bits);
            log_own_float(*seat, bits);
        }
    } else if (argc == 1) {
        const int64_t drawn = lua_isinteger(state, -1) != 0
                                  ? static_cast<int64_t>(lua_tointeger(state, -1))
                                  : int64_t{0};
        if (high == 0)
            log_own_integer(*seat, 0, 0, drawn);
        else
            log_own_integer(*seat, 1, high, drawn);
    } else if (argc == 2) {
        const int64_t drawn = lua_isinteger(state, -1) != 0
                                  ? static_cast<int64_t>(lua_tointeger(state, -1))
                                  : int64_t{0};
        log_own_integer(*seat, low, high, drawn);
    }
    return 1;
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

void install_random(lua_State* state, Seat& seat) {
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
    lua_pushlightuserdata(state, &seat);
    lua_pushcclosure(state, logged_math_random, 2);
    lua_pushvalue(state, -1);
    lua_setfield(state, -3, "random");
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
