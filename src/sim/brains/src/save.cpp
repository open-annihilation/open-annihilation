// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Asks a seated brain for the plain data it wants a saved game to keep.
// The bytes are accepted only after save returns. A failure hands the seat
// back, the same way any other hook's failure does.

#include "internal.hpp"

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
    int present{};
};

void probe_save(void* context, lua_State* state) {
    auto* box = static_cast<ProbeBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->seat->hooks_ref);
    lua_getfield(state, -1, "save");
    box->present = lua_type(state, -1) == LUA_TFUNCTION ? 1 : 0;
    lua_pop(state, 2);
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

Seat* find_seat(match_runtime::Match& match, uint8_t player) noexcept {
    if (player >= OA_PLAYER_COUNT)
        return nullptr;
    auto& state = match.seat_state();
    if (!state)
        return nullptr;
    auto* brains = static_cast<Brains*>(state.get());
    Seat* seat = brains->seats[player].get();
    if (seat == nullptr || !seat->sandbox.has_value() || seat->hooks_ref < 0)
        return nullptr;
    return seat;
}

} // namespace

SaveOutcome save_brain_state(match_runtime::Match& match, uint8_t player, SaveHooks hooks) {
    Seat* seat = find_seat(match, player);
    if (seat == nullptr)
        return SaveOutcome::absent;
    script_host::ScriptError error;
    ProbeBox probe;
    probe.seat = seat;
    if (!seat->sandbox->run_host("save", &probe, probe_save, error)) {
        hand_back(*seat, error);
        return SaveOutcome::failed;
    }
    if (probe.present == 0) {
        note_prints(*seat);
        return SaveOutcome::absent;
    }
    // These live outside the frame a raise inside save can skip.
    script_host::PlainData encoded;
    script_host::PlainDataProblem problem;
    SaveBox box;
    box.seat = seat;
    box.encoded = &encoded;
    box.problem = &problem;
    script_host::CallHooks call_hooks;
    call_hooks.context = &box;
    call_hooks.push = push_save;
    call_hooks.read = read_save;
    if (!seat->sandbox->call("save", call_hooks, error)) {
        hand_back(*seat, error);
        return SaveOutcome::failed;
    }
    note_prints(*seat);
    if (box.status != 0)
        return SaveOutcome::omitted;
    const std::span<const uint8_t> bytes = encoded.bytes();
    if (!bytes.empty() && hooks.accept != nullptr)
        hooks.accept(hooks.context, bytes);
    return SaveOutcome::stored;
}

} // namespace oa::sim::brains
