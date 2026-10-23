// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Hands a failed seat back to the built-in player and drains print lines.
// The simulation writes no file and posts no message: the report hooks do.

#include "internal.hpp"

#include "oa/sim/match_runtime.hpp"

#include <exception>
#include <new>

namespace oa::sim::brains {
namespace {

struct PrintNote {
    ReportHooks hooks{};
    uint8_t player{};
    uint32_t tick{};
};

void forward_print(void* context, std::string_view text) {
    auto* note = static_cast<PrintNote*>(context);
    if (note == nullptr || note->hooks.printed == nullptr)
        return;
    try {
        note->hooks.printed(note->hooks.context, note->player, note->tick, text);
    } catch (const std::exception&) {
        // A report hook must not escape into the tick.
    }
}

ReportHooks hooks_of(const Seat& seat) {
    if (seat.match == nullptr)
        return {};
    const auto& state = seat.match->seat_state();
    if (!state)
        return {};
    return static_cast<const Brains*>(state.get())->reports;
}

} // namespace

void note_prints(Seat& seat) {
    if (!seat.sandbox.has_value())
        return;
    PrintNote note;
    note.hooks = hooks_of(seat);
    note.player = seat.player;
    if (seat.match != nullptr)
        note.tick = seat.match->state().game.tick;
    if (note.hooks.printed == nullptr)
        seat.sandbox->drain_printed(nullptr, nullptr);
    else
        seat.sandbox->drain_printed(&note, forward_print);
}

void hand_back(Seat& seat, const script_host::ScriptError& error) {
    match_runtime::Match* const match = seat.match;
    if (match == nullptr) {
        seat.stopped = true;
        seat.sandbox.reset();
        return;
    }
    if (!seat.stopped) {
        seat.stopped = true;
        note_prints(seat);
        const ReportHooks hooks = hooks_of(seat);
        if (hooks.stopped != nullptr) {
            try {
                hooks.stopped(hooks.context, seat.player, match->state().game.tick, error);
            } catch (const std::exception&) {
                // A report hook must not escape into the tick.
            }
        }
    }
    match->set_seat_hooks(seat.player, {});
    seat.sandbox.reset();
}

void set_report_hooks(match_runtime::Match& match, ReportHooks hooks) noexcept {
    auto& state = match.seat_state();
    if (!state) {
        Brains* const created = new (std::nothrow) Brains;
        if (created == nullptr)
            return;
        state.reset(created, [](void* pointer) { delete static_cast<Brains*>(pointer); });
    }
    static_cast<Brains*>(state.get())->reports = hooks;
}

} // namespace oa::sim::brains
