// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A seated brain's report, and a second start after the profile is reloaded.
// Both run under the phase allowance in that seat's sandbox. A failure
// hands the seat back, the same way any other hook's failure does.

#include "internal.hpp"

#include "lua_game.hpp"

#include "oa/data/script_host/plain_data.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::sim::brains {
namespace {

struct ProbeBox {
    Seat* seat{};
    const char* hook{};
    int present{};
};

void probe_hook(void* context, lua_State* state) {
    auto* box = static_cast<ProbeBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->seat->hooks_ref);
    lua_getfield(state, -1, box->hook);
    box->present = lua_type(state, -1) == LUA_TFUNCTION ? 1 : 0;
    lua_pop(state, 2);
}

Seat* find_seat(match_runtime::Match& match, uint8_t player) noexcept {
    if (player >= OA_PLAYER_COUNT)
        return nullptr;
    auto& state = match.seat_state();
    if (!state)
        return nullptr;
    auto* brains = static_cast<Brains*>(state.get());
    Seat* seat = brains->seats[player].get();
    if (seat == nullptr || seat->shadow || seat->stopped || !seat->sandbox.has_value() ||
        seat->hooks_ref < 0)
        return nullptr;
    if (!brain_seated(match, player))
        return nullptr;
    return seat;
}

/// @return 1 when the hook is a function, 0 when it is not, -1 when the probe failed
int probe(Seat& seat, const char* hook, script_host::ScriptError& error) {
    ProbeBox box;
    box.seat = &seat;
    box.hook = hook;
    if (!seat.sandbox->run_host(hook, &box, probe_hook, error)) {
        hand_back(seat, error);
        return -1;
    }
    note_prints(seat);
    return box.present;
}

struct ReportBox {
    Seat* seat{};
    char* dest{};
    std::size_t room{};
    std::size_t written{};
};

int push_report(void* context, lua_State* state) {
    auto* box = static_cast<ReportBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->seat->hooks_ref);
    lua_getfield(state, -1, "report");
    lua_remove(state, -2);
    return 0;
}

void read_report(void* context, lua_State* state, int results) {
    auto* box = static_cast<ReportBox*>(context);
    if (results < 1 || lua_type(state, lua_gettop(state) - results + 1) != LUA_TSTRING)
        return;
    std::size_t length = 0;
    const char* text = lua_tolstring(state, lua_gettop(state) - results + 1, &length);
    std::size_t count = length;
    if (count > script_host::string_length_limit)
        count = script_host::string_length_limit;
    if (count > box->room)
        count = box->room;
    if (text == nullptr || box->dest == nullptr)
        count = 0;
    for (std::size_t index = 0; index < count; ++index)
        box->dest[index] = text[index];
    box->written = count;
}

struct RefreshBox {
    Seat* seat{};
};

void refresh_profile(void* context, lua_State* state) {
    auto* box = static_cast<RefreshBox*>(context);
    refresh_ai_profile(state, *box->seat);
}

struct SaveBox {
    Seat* seat{};
    script_host::PlainData* encoded{};
    script_host::PlainDataProblem* problem{};
    int status{1};
};

int push_save(void* context, lua_State* state) {
    auto* box = static_cast<SaveBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->seat->hooks_ref);
    lua_getfield(state, -1, "save");
    lua_remove(state, -2);
    return 0;
}

void read_save(void* context, lua_State* state, int results) {
    auto* box = static_cast<SaveBox*>(context);
    if (results < 1 || box->encoded == nullptr || box->problem == nullptr)
        return;
    const int index = lua_gettop(state) - results + 1;
    if (!script_host::encode_plain_data(state, index, *box->encoded, *box->problem))
        return;
    box->status = 0;
}

struct StartBox {
    Seat* seat{};
    const uint8_t* saved{};
    std::size_t saved_count{};
};

int push_start(void* context, lua_State* state) {
    auto* box = static_cast<StartBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->seat->hooks_ref);
    lua_getfield(state, -1, "start");
    lua_remove(state, -2);
    lua_getglobal(state, "game");
    lua_getglobal(state, "me");
    if (box->saved == nullptr || box->saved_count == 0)
        lua_pushnil(state);
    else
        script_host::push_plain_data(state, std::span<const uint8_t>(box->saved, box->saved_count));
    return 3;
}

} // namespace

ReportResult brain_report(match_runtime::Match& match, uint8_t player, std::span<char> out) {
    ReportResult result;
    Seat* seat = find_seat(match, player);
    if (seat == nullptr) {
        result.outcome = ReportOutcome::absent;
        return result;
    }
    script_host::ScriptError error;
    const int present = probe(*seat, "report", error);
    if (present < 0) {
        result.outcome = ReportOutcome::failed;
        return result;
    }
    if (present == 0) {
        result.outcome = ReportOutcome::absent;
        return result;
    }
    ReportBox box;
    box.seat = seat;
    box.dest = out.data();
    box.room = out.size();
    script_host::CallHooks hooks;
    hooks.context = &box;
    hooks.push = push_report;
    hooks.read = read_report;
    if (!seat->sandbox->call("report", hooks, error)) {
        hand_back(*seat, error);
        result.outcome = ReportOutcome::failed;
        return result;
    }
    note_prints(*seat);
    result.outcome = ReportOutcome::written;
    result.length = box.written;
    return result;
}

RestartOutcome restart_brain(match_runtime::Match& match, uint8_t player) {
    Seat* seat = find_seat(match, player);
    if (seat == nullptr)
        return RestartOutcome::absent;
    script_host::ScriptError error;
    RefreshBox refresh;
    refresh.seat = seat;
    if (!seat->sandbox->run_host("profile", &refresh, refresh_profile, error)) {
        hand_back(*seat, error);
        return RestartOutcome::failed;
    }
    note_prints(*seat);

    script_host::PlainData encoded;
    const int save_present = probe(*seat, "save", error);
    if (save_present < 0)
        return RestartOutcome::failed;
    const uint8_t* saved = nullptr;
    std::size_t saved_count = 0;
    if (save_present != 0) {
        script_host::PlainDataProblem problem;
        SaveBox save_box;
        save_box.seat = seat;
        save_box.encoded = &encoded;
        save_box.problem = &problem;
        script_host::CallHooks save_hooks;
        save_hooks.context = &save_box;
        save_hooks.push = push_save;
        save_hooks.read = read_save;
        if (!seat->sandbox->call("save", save_hooks, error)) {
            hand_back(*seat, error);
            return RestartOutcome::failed;
        }
        note_prints(*seat);
        if (save_box.status == 0) {
            const std::span<const uint8_t> bytes = encoded.bytes();
            saved = bytes.data();
            saved_count = bytes.size();
        }
    }

    const int start_present = probe(*seat, "start", error);
    if (start_present < 0)
        return RestartOutcome::failed;
    if (start_present == 0)
        return RestartOutcome::restarted;

    StartBox start_box;
    start_box.seat = seat;
    start_box.saved = saved;
    start_box.saved_count = saved_count;
    script_host::CallHooks start_hooks;
    start_hooks.context = &start_box;
    start_hooks.push = push_start;
    start_hooks.read = nullptr;
    if (!seat->sandbox->call("start", start_hooks, error)) {
        hand_back(*seat, error);
        return RestartOutcome::failed;
    }
    note_prints(*seat);
    return RestartOutcome::restarted;
}

} // namespace oa::sim::brains
