// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How the engine calls the hooks of an extension table (extension.hpp), of
// the switch handler it returns (command_line::SwitchHandler) and of a replay
// it opened (ReplayHooks): through call_hook, which
// catches whatever the hook throws before it reaches engine code, and the
// two calls built on it, which handle the error as the hook's documentation
// says. Each hook's handling is listed here (HookTraits) and nowhere else;
// a hook with no entry does not compile, and the check-hook-calls test
// (tools/check_hook_calls.py) fails when engine code calls a hook pointer
// other than through call_hook. The combined table (extension_list.cpp) is
// the table itself: it calls each extension's hook directly, and what one
// throws reaches the guarded call that called the combined hook.
#pragma once

#include "oa/app/command_line.hpp"
#include "oa/app/extension.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace oa::app {

// What the engine does with an error a hook throws.
enum class HookErrorHandling : uint8_t {
    // The engine raises the hook's message as its own error at the call, and
    // the error takes the engine's path from there: the start, the run or
    // the check stops, or a match start the frontend falls back from is
    // abandoned (extension.hpp).
    raise,
    // The engine reports the message and carries on as it does for a null
    // hook.
    report,
    // The hook must not throw; one that throws all the same is reported and
    // the engine carries on as it does for a null hook.
    must_not_throw,
};

// A hook's name, as reports give it, and the handling of its errors.
struct HookEntry {
    const char* name{};
    HookErrorHandling handling{};
};

// The entry of each hook; a hook with none has no specialisation, so a call
// of it does not compile.
template <auto Hook>
struct HookTraits;

#define OA_HOOK_ENTRY(table, hook, handling_kind)                                                  \
    template <>                                                                                    \
    struct HookTraits<&table::hook> {                                                              \
        static constexpr HookEntry entry{#hook, HookErrorHandling::handling_kind};                 \
    }

OA_HOOK_ENTRY(Extension, take_option, raise);
OA_HOOK_ENTRY(Extension, check_options, raise);
OA_HOOK_ENTRY(Extension, switch_handler, raise);
OA_HOOK_ENTRY(Extension, text, raise);
OA_HOOK_ENTRY(Extension, startup, raise);
OA_HOOK_ENTRY(Extension, register_screens, raise);
OA_HOOK_ENTRY(Extension, register_settings_pages, raise);
OA_HOOK_ENTRY(Extension, ready, raise);
OA_HOOK_ENTRY(Extension, frontend_entry, raise);
OA_HOOK_ENTRY(Extension, frontend_states, raise);
OA_HOOK_ENTRY(Extension, run_mode, raise);
OA_HOOK_ENTRY(Extension, start_scene, raise);
OA_HOOK_ENTRY(Extension, shutdown, raise);
OA_HOOK_ENTRY(Extension, select_multiplayer, raise);
OA_HOOK_ENTRY(Extension, frontend_game, raise);
OA_HOOK_ENTRY(Extension, keep_stored_password, raise);
OA_HOOK_ENTRY(Extension, check_multiplayer_menu, raise);
OA_HOOK_ENTRY(Extension, state, raise);
OA_HOOK_ENTRY(Extension, frame, raise);
OA_HOOK_ENTRY(Extension, simulation_step, report);
OA_HOOK_ENTRY(Extension, outcome_ready, raise);
OA_HOOK_ENTRY(Extension, match_game, raise);
OA_HOOK_ENTRY(Extension, match_event, report);
OA_HOOK_ENTRY(Extension, disconnect_text, raise);
OA_HOOK_ENTRY(Extension, give_resources, raise);
OA_HOOK_ENTRY(Extension, message_hooks, report);
OA_HOOK_ENTRY(Extension, player_gone, report);
OA_HOOK_ENTRY(Extension, console_host, raise);
OA_HOOK_ENTRY(Extension, check_console, raise);
OA_HOOK_ENTRY(Extension, draw_loading, report);
OA_HOOK_ENTRY(Extension, draw_match_hud, report);
OA_HOOK_ENTRY(Extension, draw_match_overlay, report);
OA_HOOK_ENTRY(Extension, pause_changed, report);
OA_HOOK_ENTRY(Extension, load_progress, report);
OA_HOOK_ENTRY(Extension, team_panel_host, raise);
OA_HOOK_ENTRY(Extension, close_requested, raise);
OA_HOOK_ENTRY(Extension, return_label, raise);
OA_HOOK_ENTRY(Extension, speed_changed, must_not_throw);
OA_HOOK_ENTRY(Extension, app_mode_set, must_not_throw);
OA_HOOK_ENTRY(Extension, open_recording, raise);
OA_HOOK_ENTRY(Extension, release_runtime, must_not_throw);
OA_HOOK_ENTRY(Extension, replay_frame, must_not_throw);
OA_HOOK_ENTRY(Extension, replay_hold, must_not_throw);
OA_HOOK_ENTRY(Extension, replay_play, must_not_throw);
OA_HOOK_ENTRY(Extension, replay_pause, must_not_throw);
OA_HOOK_ENTRY(Extension, replay_set_speed, must_not_throw);
OA_HOOK_ENTRY(Extension, replay_step_speed, must_not_throw);
OA_HOOK_ENTRY(Extension, replay_seek, must_not_throw);
OA_HOOK_ENTRY(Extension, replay_state, must_not_throw);
OA_HOOK_ENTRY(command_line::SwitchHandler, take, raise);
OA_HOOK_ENTRY(command_line::SwitchHandler, reset, raise);
OA_HOOK_ENTRY(ReplayHooks, step, must_not_throw);
OA_HOOK_ENTRY(ReplayHooks, status, must_not_throw);
OA_HOOK_ENTRY(ReplayHooks, close, must_not_throw);

#undef OA_HOOK_ENTRY

// What a guarded call caught.
struct HookError {
    bool caught{};       // the hook threw
    std::string message; // what it threw: its what(), or a line saying it was no std::exception
};

// The table a hook belongs to and the answer it gives, from its member pointer.
template <typename Member>
struct HookSignature;

template <typename Table, typename Answer, typename... Parameters>
struct HookSignature<Answer (*Table::*)(void*, Parameters...)> {
    using HookTable = Table;
    using HookAnswer = Answer;
};

template <auto Hook>
using HookTableOf = typename HookSignature<decltype(Hook)>::HookTable;

template <auto Hook>
using HookAnswerOf = typename HookSignature<decltype(Hook)>::HookAnswer;

/// Keeps what a hook threw in a guarded call's error.
///
/// @param[out] error the call's error, caught from here on
/// @param text the exception's text
inline void keep_hook_error(HookError& error, const char* text) noexcept {
    error.caught = true;
    try {
        error.message = text != nullptr ? text : "";
    } catch (...) {
        error.message.clear();
    }
}

/// Calls a hook of a table, catching whatever it throws: the one call of a
/// hook pointer engine code makes.
///
/// A null hook is not called. A hook that throws a std::exception leaves its
/// what() in `error`; one that throws anything else leaves a line that says
/// so.
///
/// @tparam Hook the hook, a member pointer of Extension, SwitchHandler or
///         ReplayHooks with an entry in HookTraits
/// @param table the table that holds the hook, whose context goes first
/// @param[out] error cleared, then what the hook threw, when it threw
/// @param arguments what follows the context
/// @return the hook's answer; the answer's zero value when the hook is null
///         or threw
template <auto Hook, typename... Arguments>
HookAnswerOf<Hook>
call_hook(const HookTableOf<Hook>& table, HookError& error, Arguments&&... arguments) noexcept {
    static_assert(HookTraits<Hook>::entry.name != nullptr);
    using Answer = HookAnswerOf<Hook>;
    error.caught = false;
    error.message.clear();
    const auto hook = table.*Hook;
    if (hook != nullptr) {
        try {
            return hook(table.context, std::forward<Arguments>(arguments)...);
        } catch (const std::exception& thrown) {
            keep_hook_error(error, thrown.what());
        } catch (...) {
            keep_hook_error(error, "an exception that is not a std::exception");
        }
    }
    if constexpr (!std::is_void_v<Answer>)
        return Answer{};
}

/// Calls a hook whose error the engine raises as its own
/// (HookErrorHandling::raise).
///
/// Throws std::runtime_error with the hook's message when the hook threw;
/// the hook's own exception never leaves the guarded call.
///
/// @tparam Hook the hook
/// @param table the table that holds it
/// @param arguments what follows the context
/// @return the hook's answer; the answer's zero value when the hook is null
template <auto Hook, typename... Arguments>
HookAnswerOf<Hook> call_hook_or_raise(const HookTableOf<Hook>& table, Arguments&&... arguments) {
    static_assert(
        HookTraits<Hook>::entry.handling == HookErrorHandling::raise,
        "the hook's error is reported, not raised: call it with call_hook_or_report"
    );
    HookError error;
    if constexpr (std::is_void_v<HookAnswerOf<Hook>>) {
        call_hook<Hook>(table, error, std::forward<Arguments>(arguments)...);
        if (error.caught)
            throw std::runtime_error(error.message);
    } else {
        auto answer = call_hook<Hook>(table, error, std::forward<Arguments>(arguments)...);
        if (error.caught)
            throw std::runtime_error(error.message);
        return answer;
    }
}

/// Hands a caught error to its report, which must not stop the call.
///
/// A report that throws all the same is replaced by one line on standard
/// error.
///
/// @param report takes the hook's name and the message
/// @param name the hook's name
/// @param error the caught error
template <typename Report>
void pass_hook_error(Report& report, const char* name, const HookError& error) noexcept {
    try {
        report(name, error.message.c_str());
    } catch (...) {
        std::fprintf(
            stderr, "open-annihilation: extension hook %s: %s\n", name, error.message.c_str()
        );
    }
}

/// Calls a hook whose error the engine reports and carries on from
/// (HookErrorHandling::report and must_not_throw).
///
/// @tparam Hook the hook
/// @param table the table that holds it
/// @param report takes the hook's name and the message, when the hook threw
/// @param arguments what follows the context
/// @return the hook's answer; the answer's zero value when the hook is null
///         or threw
template <auto Hook, typename Report, typename... Arguments>
HookAnswerOf<Hook> call_hook_or_report(
    const HookTableOf<Hook>& table, Report&& report, Arguments&&... arguments
) noexcept {
    static_assert(
        HookTraits<Hook>::entry.handling != HookErrorHandling::raise,
        "the hook's error is raised, not reported: call it with call_hook_or_raise"
    );
    HookError error;
    if constexpr (std::is_void_v<HookAnswerOf<Hook>>) {
        call_hook<Hook>(table, error, std::forward<Arguments>(arguments)...);
        if (error.caught)
            pass_hook_error(report, HookTraits<Hook>::entry.name, error);
    } else {
        auto answer = call_hook<Hook>(table, error, std::forward<Arguments>(arguments)...);
        if (error.caught)
            pass_hook_error(report, HookTraits<Hook>::entry.name, error);
        return answer;
    }
}

} // namespace oa::app
