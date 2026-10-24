// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// brain.record. The checks and the charge run whether or not a file is
// open. A sample is handed over only while a match is recording.

#include "internal.hpp"
#include "lua_networks.hpp"

#include "handles_internal.hpp"

#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstring>
#include <span>

namespace oa::sim::brains {
namespace {

constexpr char name_message[] =
    "brain.record: a feature name is 1 to 31 letters, digits or underscores";
constexpr char features_message[] = "brain.record: features must be a table of 1 to 16 tensors";

bool feature_name_ok(const char* text, std::size_t length) noexcept {
    if (text == nullptr || length < 1 || length > 31)
        return false;
    for (std::size_t index = 0; index < length; ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        const bool letter = byte >= 'a' && byte <= 'z';
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !digit && byte != '_')
            return false;
    }
    return true;
}

void copy_feature_name(char* out, const char* text, std::size_t length) noexcept {
    for (std::size_t index = 0; index < length; ++index)
        out[index] = text[index];
    out[length] = '\0';
}

struct HeldFeature {
    char name[32]{};
    TensorView view{};
};

void fill_feature(FeatureView& feature, const HeldFeature& held) noexcept {
    copy_feature_name(feature.name, held.name, std::strlen(held.name));
    feature.rank = held.view.rank;
    for (uint8_t axis = 0; axis < 4; ++axis)
        feature.dims[axis] = held.view.dims[axis];
    feature.values = std::span<const int32_t>(held.view.values, held.view.count);
}

int brain_record(lua_State* state) {
    if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TTABLE)
        return luaL_error(state, "brain.record: expects one table");
    Seat* seat = seat_of(state);
    if (seat == nullptr)
        return luaL_error(state, "brain.record: expects one table");

    lua_getfield(state, 1, "features");
    if (lua_type(state, -1) != LUA_TTABLE)
        return luaL_error(state, features_message);
    const int features = lua_absindex(state, -1);

    HeldFeature held[record_feature_cap];
    uint8_t count = 0;
    lua_pushnil(state);
    while (lua_next(state, features) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING)
            return luaL_error(state, name_message);
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -2, &length);
        if (!feature_name_ok(text, length))
            return luaL_error(state, name_message);
        for (uint8_t earlier = 0; earlier < count; ++earlier) {
            if (std::strcmp(held[earlier].name, text) == 0)
                return luaL_error(state, "brain.record: feature '%s' is repeated", text);
        }
        TensorView view{};
        if (!tensor_view(state, -1, view))
            return luaL_error(state, "brain.record: feature '%s' is not a tensor", text);
        if (count == record_feature_cap)
            return luaL_error(state, features_message);
        copy_feature_name(held[count].name, text, length);
        held[count].view = view;
        ++count;
        lua_pop(state, 1);
    }
    if (count == 0)
        return luaL_error(state, features_message);

    for (uint8_t index = 1; index < count; ++index) {
        const HeldFeature chosen = held[index];
        uint8_t place = index;
        while (place > 0 && std::strcmp(held[place - 1].name, chosen.name) > 0) {
            held[place] = held[place - 1];
            --place;
        }
        held[place] = chosen;
    }

    lua_getfield(state, 1, "decision");
    DecisionView decision{};
    uint64_t elements = 0;
    for (uint8_t index = 0; index < count; ++index)
        elements += held[index].view.count;
    if (lua_isinteger(state, -1) != 0) {
        decision.whole = static_cast<int64_t>(lua_tointeger(state, -1));
    } else {
        TensorView view{};
        if (!tensor_view(state, -1, view)) {
            return luaL_error(state, "brain.record: decision must be an integer or a tensor");
        }
        decision.tensor = true;
        decision.rank = view.rank;
        for (uint8_t axis = 0; axis < 4; ++axis)
            decision.dims[axis] = view.dims[axis];
        decision.values = std::span<const int32_t>(view.values, view.count);
        elements += view.count;
    }
    if (elements > record_element_cap)
        return luaL_error(state, "brain.record: at most 4194304 elements");

    const uint64_t charge = 64u + elements / 64u;
    script_host::charge_work(state, charge);

    const LogHooks hooks = seat_log_hooks(*seat);
    if (seat->match != nullptr && hooks.sample != nullptr) {
        RecordSample sample{};
        sample.player = seat->player;
        sample.tick = seat->match->state().game.tick;
        sample.feature_count = count;
        for (uint8_t index = 0; index < count; ++index)
            fill_feature(sample.features[index], held[index]);
        sample.decision = decision;
        hooks.sample(hooks.context, sample);
    }
    return 0;
}

} // namespace

void install_record(lua_State* state, Seat& seat) {
    (void)seat;
    if (state == nullptr)
        return;
    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    lua_pushcfunction(state, brain_record);
    lua_setfield(state, -2, "record");
    lua_setglobal(state, "brain");
}

} // namespace oa::sim::brains
