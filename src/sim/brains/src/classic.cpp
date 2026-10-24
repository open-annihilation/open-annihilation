// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Recognising Classic. The fingerprint of brains/classic/ is baked in at
// build time. A seat is Classic only when its files hash to that value and
// the value is not empty. brain.shared_random is installed for a Classic
// seat that has a match, and for a shadow. A shadow draws its turn's copy.

#include "oa/sim/brains/classic.hpp"

#include "classic_fingerprint.hpp"
#include "internal.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/player.h"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

namespace oa::sim::brains {
namespace {

bool origin_record(std::string_view path) noexcept {
    return path == ".oa-origin.yaml";
}

bool path_before(std::string_view left, std::string_view right) noexcept {
    const std::size_t count = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto left_byte = static_cast<unsigned char>(left[index]);
        const auto right_byte = static_cast<unsigned char>(right[index]);
        if (left_byte != right_byte)
            return left_byte < right_byte;
    }
    return left.size() < right.size();
}

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

bool same_text(const std::array<char, 65>& got, std::string_view expected) noexcept {
    if (expected.size() != 64 || got[0] == '\0')
        return false;
    for (std::size_t index = 0; index < 64; ++index) {
        if (got[index] != expected[index])
            return false;
    }
    return got[64] == '\0';
}

extern "C" int brain_shared_random(lua_State* state) {
    auto* seat = static_cast<Seat*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (seat == nullptr || seat->match == nullptr)
        return luaL_error(state, "brain.shared_random: there is no match");
    if (lua_gettop(state) != 1 || lua_isinteger(state, 1) == 0)
        return luaL_error(
            state, "brain.shared_random: argument 1 must be an integer from 0 to 4294967295"
        );
    const lua_Integer bound = lua_tointeger(state, 1);
    if (bound < 0 || static_cast<uint64_t>(bound) > uint64_t{4294967295u})
        return luaL_error(
            state, "brain.shared_random: argument 1 must be an integer from 0 to 4294967295"
        );
    if (seat->shadow && !shadow_stream_ready(*seat))
        return luaL_error(state, "brain.shared_random: the stream is not ready");
    if (seat->calls.host.random == nullptr)
        return luaL_error(state, "brain.shared_random: the stream is not ready");
    const uint32_t drawn =
        seat->calls.host.random(seat->calls.host.context, static_cast<uint32_t>(bound));
    lua_pushinteger(state, static_cast<lua_Integer>(drawn));
    return 1;
}

bool section_characters(const char* text, std::size_t length) noexcept {
    if (text == nullptr || length < 1 || length > 15)
        return false;
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const bool letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !digit && byte != '-')
            return false;
    }
    return true;
}

extern "C" int brain_section(lua_State* state) {
    auto* seat = static_cast<Seat*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (seat == nullptr)
        return luaL_error(
            state, "brain.section: a section name is up to 15 letters, digits or hyphens"
        );
    std::size_t length = 0;
    const char* text = nullptr;
    if (lua_type(state, 1) == LUA_TSTRING)
        text = lua_tolstring(state, 1, &length);
    if (!section_characters(text, length))
        return luaL_error(
            state, "brain.section: a section name is up to 15 letters, digits or hyphens"
        );
    if (seat->calls.section != nullptr)
        seat->calls.section(&seat->calls.host, text);
    return 0;
}

extern "C" int brain_setup_random(lua_State* state) {
    auto* seat = static_cast<Seat*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (seat == nullptr)
        return luaL_error(state, "brain.setup_random: the seat is not set up yet");
    if (lua_gettop(state) != 1 || lua_isinteger(state, 1) == 0)
        return luaL_error(
            state, "brain.setup_random: argument 1 must be an integer from 0 to 4294967295"
        );
    const lua_Integer bound = lua_tointeger(state, 1);
    if (bound < 0 || static_cast<uint64_t>(bound) > uint64_t{4294967295u})
        return luaL_error(
            state, "brain.setup_random: argument 1 must be an integer from 0 to 4294967295"
        );
    if (!seat->setup_ready)
        return luaL_error(state, "brain.setup_random: the seat is not set up yet");
    const auto bound_value = static_cast<uint32_t>(bound);
    const uint32_t drawn = seat->setup_copy.bounded(bound_value);
    if (seat->shadow)
        shadow_note_setup_draw(*seat, bound_value, drawn);
    lua_pushinteger(state, static_cast<lua_Integer>(drawn));
    return 1;
}

} // namespace

std::array<char, 65> classic_fingerprint(std::span<const PackageFile> files) noexcept {
    std::array<char, 65> text{};
    std::size_t kept = 0;
    for (const PackageFile& file : files) {
        if (!origin_record(file.path))
            ++kept;
    }
    if (kept == 0)
        return text;
    auto* order = new (std::nothrow) std::size_t[kept];
    if (order == nullptr)
        return text;
    std::size_t filled = 0;
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (origin_record(files[index].path))
            continue;
        std::size_t place = filled;
        while (place > 0 && path_before(files[index].path, files[order[place - 1]].path)) {
            order[place] = order[place - 1];
            --place;
        }
        order[place] = index;
        ++filled;
    }
    oa::base::sha256::Hasher hasher;
    for (std::size_t index = 0; index < filled; ++index) {
        const PackageFile& file = files[order[index]];
        const oa::base::sha256::Digest digest = oa::base::sha256::digest_of(file.bytes);
        const std::array<char, oa::base::sha256::hex_size> hex = oa::base::sha256::to_hex(digest);
        feed(hasher, std::string_view(hex.data(), hex.size()));
        feed_byte(hasher, ' ');
        feed(hasher, file.path);
        feed_byte(hasher, '\n');
    }
    delete[] order;
    const oa::base::sha256::Digest whole = oa::base::sha256::finish(hasher);
    const std::array<char, oa::base::sha256::hex_size> hex = oa::base::sha256::to_hex(whole);
    for (std::size_t index = 0; index < hex.size(); ++index)
        text[index] = hex[index];
    text[hex.size()] = '\0';
    return text;
}

void note_classic(Seat& seat, const SeatSpec& spec) noexcept {
    const std::array<char, 65> got = classic_fingerprint(spec.files);
    std::string_view expected = classic_fingerprint_builtin;
    if (!spec.classic_fingerprint_for_tests.empty())
        expected = spec.classic_fingerprint_for_tests;
    seat.classic = same_text(got, expected);
}

bool seat_is_classic(const match_runtime::Match& match, uint8_t player) noexcept {
    if (player >= OA_PLAYER_COUNT)
        return false;
    const std::shared_ptr<void>& state = match.seat_state();
    if (!state)
        return false;
    const auto* brains = static_cast<const Brains*>(state.get());
    const Seat* seat = brains->seats[player].get();
    return seat != nullptr && seat->classic;
}

void install_named(lua_State* state, Seat& seat, const char* name, lua_CFunction function) {
    if (state == nullptr || name == nullptr || function == nullptr)
        return;
    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return;
    }
    lua_pushlightuserdata(state, &seat);
    lua_pushcclosure(state, function, 1);
    lua_setfield(state, -2, name);
    lua_pop(state, 1);
}

void install_shared_random(lua_State* state, Seat& seat) {
    if (state == nullptr || seat.match == nullptr)
        return;
    if (!seat.classic && !seat.shadow)
        return;
    install_named(state, seat, "shared_random", brain_shared_random);
}

void install_section(lua_State* state, Seat& seat) {
    install_named(state, seat, "section", brain_section);
}

void install_setup_random(lua_State* state, Seat& seat) {
    if (state == nullptr)
        return;
    if (!seat.classic && !seat.shadow)
        return;
    install_named(state, seat, "setup_random", brain_setup_random);
}

} // namespace oa::sim::brains
