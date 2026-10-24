// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// brain.say, and the lines a seat has heard since its last turn. Hearing a
// line only queues it. The seat's turn delivers it.

#include "internal.hpp"

#include "handles_internal.hpp"

#include "oa/sim/brains/chat.hpp"
#include "oa/sim/match_runtime.hpp"

#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/data/script_host/script_host.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>

namespace oa::sim::brains {
namespace {

constexpr uint64_t say_charge = 16;
constexpr char say_to_message[] =
    "say expects \"all\", \"allies\", \"enemies\" or a player from 0 to 9";

Brains* brains_of(match_runtime::Match& match) noexcept {
    auto& held = match.seat_state();
    if (!held)
        return nullptr;
    return static_cast<Brains*>(held.get());
}

const Brains* brains_of(const match_runtime::Match& match) noexcept {
    const std::shared_ptr<void>& held = const_cast<match_runtime::Match&>(match).seat_state();
    if (!held)
        return nullptr;
    return static_cast<const Brains*>(held.get());
}

/// Bytes of `words` that fit in `limit`, ending on a whole character.
std::size_t whole_characters(std::string_view words, std::size_t limit) noexcept {
    const std::size_t available = words.size() < limit ? words.size() : limit;
    std::size_t accepted = 0;
    std::size_t index = 0;
    while (index < available) {
        const auto lead = static_cast<unsigned char>(words[index]);
        std::size_t need = 1;
        if (lead < 0x80U)
            need = 1;
        else if ((lead & 0xE0U) == 0xC0U)
            need = 2;
        else if ((lead & 0xF0U) == 0xE0U)
            need = 3;
        else if ((lead & 0xF8U) == 0xF0U)
            need = 4;
        else
            break;
        if (index + need > words.size() || index + need > limit)
            break;
        bool whole = true;
        for (std::size_t extra = 1; extra < need; ++extra) {
            const auto cont = static_cast<unsigned char>(words[index + extra]);
            if ((cont & 0xC0U) != 0x80U) {
                whole = false;
                break;
            }
        }
        if (!whole)
            break;
        index += need;
        accepted = index;
    }
    return accepted;
}

bool same_word(const char* text, const char* literal) noexcept {
    if (text == nullptr || literal == nullptr)
        return false;
    while (*text != '\0' && *text == *literal) {
        ++text;
        ++literal;
    }
    return *text == '\0' && *literal == '\0';
}

const char* say_reason(uint8_t answer) noexcept {
    if (answer == 1)
        return "not_a_seat";
    if (answer == 2)
        return "empty";
    if (answer == 3)
        return "command";
    if (answer == 4)
        return "too_soon";
    if (answer == 5)
        return "no_target";
    return "no_chat";
}

int refuse_chat(lua_State* state) {
    lua_pushboolean(state, 0);
    lua_pushstring(state, "no_chat");
    return 2;
}

extern "C" {
static int brain_say(lua_State* state) {
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "say expects text");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 1, &length);
    if (text == nullptr) {
        text = "";
        length = 0;
    }
    uint8_t mode = OA_CHAT_MODE_EVERYONE;
    uint8_t target = 0;
    const int top = lua_gettop(state);
    if (top >= 2 && lua_type(state, 2) != LUA_TNIL) {
        if (lua_type(state, 2) == LUA_TSTRING) {
            const char* word = lua_tostring(state, 2);
            if (same_word(word, "all"))
                mode = OA_CHAT_MODE_EVERYONE;
            else if (same_word(word, "allies"))
                mode = OA_CHAT_MODE_ALLIES;
            else if (same_word(word, "enemies"))
                mode = OA_CHAT_MODE_ENEMIES;
            else
                return luaL_error(state, "%s", say_to_message);
        } else if (lua_isinteger(state, 2) != 0) {
            const lua_Integer player = lua_tointeger(state, 2);
            if (player < 0 || player >= static_cast<lua_Integer>(OA_PLAYER_COUNT))
                return luaL_error(state, "%s", say_to_message);
            mode = OA_CHAT_MODE_CHOSEN;
            target = static_cast<uint8_t>(player);
        } else
            return luaL_error(state, "%s", say_to_message);
    }

    script_host::charge_work(state, say_charge + script_host::units_for_bytes(length));

    Seat* seat = seat_of(state);
    if (seat == nullptr || seat->match == nullptr)
        return refuse_chat(state);
    if (seat->calls.refuse != nullptr) {
        seat->calls.refuse(&seat->calls.host, "say", 0);
        lua_pushboolean(state, 1);
        return 1;
    }
    Brains* brains = brains_of(*seat->match);
    if (brains == nullptr || brains->chat.say == nullptr)
        return refuse_chat(state);
    const uint8_t answer = brains->chat.say(
        brains->chat.context, seat->player, std::string_view{text, length}, mode, target
    );
    if (answer == 0) {
        lua_pushboolean(state, 1);
        return 1;
    }
    lua_pushboolean(state, 0);
    lua_pushstring(state, say_reason(answer));
    return 2;
}
}

} // namespace

void set_chat_hooks(match_runtime::Match& match, ChatHooks hooks) noexcept {
    auto& held = match.seat_state();
    if (!held) {
        Brains* created = nullptr;
        try {
            created = new Brains;
        } catch (const std::bad_alloc&) {
            return;
        }
        try {
            held.reset(created, [](void* pointer) { delete static_cast<Brains*>(pointer); });
        } catch (...) {
            // A reset that throws has already passed `created` to the deleter.
            return;
        }
    }
    static_cast<Brains*>(held.get())->chat = hooks;
}

void hear_chat(
    match_runtime::Match& match, uint8_t seat, uint8_t from, uint8_t mode, std::string_view words
) noexcept {
    if (seat >= OA_PLAYER_COUNT)
        return;
    Brains* brains = brains_of(match);
    if (brains == nullptr)
        return;
    Seat* playing = brains->seats[seat].get();
    if (playing == nullptr)
        return;
    ChatQueue& queue = playing->chat_incoming;
    if (queue.count >= max_chat_lines) {
        ++playing->dropped_chat;
        return;
    }
    ChatLine& line = queue.lines[queue.count];
    line = ChatLine{};
    line.from = from;
    line.mode = mode;
    const std::size_t kept = whole_characters(words, chat_line_bytes);
    for (std::size_t index = 0; index < kept; ++index)
        line.words[index] = words[index];
    line.length = static_cast<uint16_t>(kept);
    ++queue.count;
}

uint32_t dropped_chat_lines(const match_runtime::Match& match, uint8_t seat) noexcept {
    if (seat >= OA_PLAYER_COUNT)
        return 0;
    const Brains* brains = brains_of(match);
    if (brains == nullptr)
        return 0;
    const Seat* playing = brains->seats[seat].get();
    if (playing == nullptr)
        return 0;
    return playing->dropped_chat;
}

void install_chat(lua_State* state, Seat& seat) {
    store_seat(state, seat);
    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    lua_pushcfunction(state, brain_say);
    lua_setfield(state, -2, "say");
    lua_setglobal(state, "brain");
}

} // namespace oa::sim::brains
