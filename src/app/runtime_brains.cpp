// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Seats the brains a skirmish or a campaign mission named, before the
// match's first tick. A seat that cannot take its brain plays as the
// built-in computer player.

#include "oa/app/runtime.hpp"

#include "oa/core/player.h"
#include "oa/data/script_host/script_host.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/sim/brains/chat.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/messages.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

void report_seat(uint8_t player, std::string_view id, std::string_view reason) {
    std::cerr << "open-annihilation: brains: seat " << static_cast<int>(player) << ": " << id
              << ": " << reason << '\n';
}

std::string failure_text(const oa::sim::brains::SeatOutcome& outcome) {
    if (outcome.reason[0] != '\0')
        return std::string(outcome.reason.data());
    const std::string described = oa::data::script_host::describe(outcome.error);
    if (!described.empty())
        return described;
    return "the brain could not be seated";
}

std::string notice_reason(const oa::data::script_host::ScriptError& error) {
    if (error.chunk.empty())
        return error.message;
    return error.chunk + ":" + std::to_string(error.line) + ": " + error.message;
}

} // namespace

void Runtime::prepare_brain_report(
    uint8_t player,
    std::string_view name,
    std::string_view version,
    std::string_view id,
    std::string_view level
) {
    if (match_ == nullptr || player >= OA_PLAYER_COUNT)
        return;
    oa::sim::brains::ReportHooks hooks;
    hooks.context = this;
    hooks.printed = &Runtime::brain_printed;
    hooks.stopped = &Runtime::brain_stopped;
    oa::sim::brains::set_report_hooks(*match_, hooks);
    try {
        brain_logs_[player].begin(
            oa::platform::preferences::data_directory(),
            player,
            name,
            version,
            id,
            level,
            brain_match_seed_
        );
    } catch (const std::exception&) {
        brain_logs_[player].fail(player);
    }
}

void Runtime::report_unseated_brain(
    uint8_t player, std::string_view name, const oa::sim::brains::SeatOutcome& outcome
) {
    if (player >= OA_PLAYER_COUNT)
        return;
    const uint32_t tick = match_ != nullptr ? match_->state().game.tick : 0;
    BrainLog& log = brain_logs_[player];
    std::string reason;
    if (outcome.result == oa::sim::brains::SeatResult::failed &&
        outcome.error.kind != oa::data::script_host::ErrorKind::none) {
        log.stop(tick, outcome.error);
        reason = notice_reason(outcome.error);
    } else {
        reason =
            outcome.reason[0] != '\0' ? std::string(outcome.reason.data()) : failure_text(outcome);
        log.stop_text(tick, reason);
    }
    const std::string_view words = outcome.result == oa::sim::brains::SeatResult::does_not_fit
                                       ? brain_does_not_fit_words
                                       : brain_could_not_start_words;
    try {
        post_match_message(brain_notice(words, name, reason), oa::sim::messages::kind_status);
    } catch (const std::exception&) {
        // The notice must not stop seating the other players.
    }
}

void Runtime::brain_printed(void* context, uint8_t player, uint32_t tick, std::string_view line) {
    auto* runtime = static_cast<Runtime*>(context);
    if (runtime == nullptr || player >= OA_PLAYER_COUNT)
        return;
    try {
        runtime->brain_logs_[player].line(tick, line);
    } catch (const std::exception&) {
        // A printed line must not escape into the tick.
    }
}

void Runtime::brain_stopped(
    void* context, uint8_t player, uint32_t tick, const oa::data::script_host::ScriptError& error
) {
    auto* runtime = static_cast<Runtime*>(context);
    if (runtime == nullptr || player >= OA_PLAYER_COUNT)
        return;
    try {
        BrainLog& log = runtime->brain_logs_[player];
        log.stop(tick, error);
        runtime->post_match_message(
            brain_notice(brain_stopped_words, log.name(), notice_reason(error)),
            oa::sim::messages::kind_status
        );
    } catch (const std::exception&) {
        // The stop must not escape into the tick.
    }
}

void Runtime::set_brain_seat(uint8_t player, std::optional<BrainSeatChoice> choice) {
    if (player >= OA_PLAYER_COUNT)
        return;
    brain_seat_choices_[player] = std::move(choice);
}

uint8_t Runtime::brain_chat_say(
    void* context, uint8_t seat, std::string_view text, uint8_t mode, uint8_t target
) {
    auto* runtime = static_cast<Runtime*>(context);
    if (runtime == nullptr)
        return static_cast<uint8_t>(seat_chat::Said::not_a_seat);
    return static_cast<uint8_t>(runtime->say_for_seat(seat, text, mode, target));
}

void Runtime::brain_chat_heard(
    void* context, uint8_t seat, uint8_t from, uint8_t mode, std::string_view words
) {
    auto* runtime = static_cast<Runtime*>(context);
    if (runtime == nullptr || runtime->match_ == nullptr)
        return;
    oa::sim::brains::hear_chat(*runtime->match_, seat, from, mode, words);
}

void Runtime::attach_brain_chat() {
    if (match_ == nullptr)
        return;
    bool any = false;
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        if (oa::sim::brains::brain_seated(*match_, player)) {
            any = true;
            break;
        }
    }
    if (!any)
        return;
    oa::sim::brains::ChatHooks hooks;
    hooks.context = this;
    hooks.say = &Runtime::brain_chat_say;
    oa::sim::brains::set_chat_hooks(*match_, hooks);
    seat_chat::Listener listener;
    listener.context = this;
    listener.heard = &Runtime::brain_chat_heard;
    set_seat_chat_listener(listener);
}

void Runtime::seat_chosen_brains() {
    if (match_ == nullptr)
        return;
    const std::filesystem::path brains_folder = user_folder() / std::string(brains_folder_name);
    bool seated_one = false;
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const std::optional<BrainSeatChoice>& choice = brain_seat_choices_[player];
        if (!choice.has_value())
            continue;
        const oa::Player& record = match_->state().game.players[player];
        if (record.in_use == 0 || record.status != OA_PLAYER_STATUS_COMPUTER)
            continue;
        try {
            std::string error;
            const std::optional<InstalledBrain> installed =
                read_installed_brain(brains_folder, choice->id, error);
            if (!installed) {
                report_seat(
                    player, choice->id, error.empty() ? "the brain could not be read" : error
                );
                continue;
            }
            ResolvedSeat resolved;
            if (!resolve_seat(*installed, *choice, resolved, error)) {
                report_seat(
                    player, choice->id, error.empty() ? "the brain could not be seated" : error
                );
                continue;
            }
            std::vector<oa::sim::brains::PackageFile> files;
            std::vector<oa::sim::brains::OptionValue> options;
            std::vector<oa::sim::brains::NetworkFile> networks;
            const oa::sim::brains::SeatSpec spec =
                seat_spec(player, *installed, resolved, files, options, networks);
            prepare_brain_report(
                player,
                installed->manifest.name,
                resolved.version,
                installed->manifest.id,
                resolved.level
            );
            const oa::sim::brains::SeatOutcome outcome = oa::sim::brains::seat_brain(*match_, spec);
            if (outcome.result != oa::sim::brains::SeatResult::seated) {
                report_seat(player, choice->id, failure_text(outcome));
                report_unseated_brain(player, installed->manifest.name, outcome);
            } else {
                seated_one = true;
            }
        } catch (const std::exception& ex) {
            const char* text = ex.what();
            report_seat(
                player, choice->id, text != nullptr ? text : "the brain could not be seated"
            );
        }
    }
    if (seated_one) {
        oa::sim::brains::ChatHooks hooks;
        hooks.context = this;
        hooks.say = &Runtime::brain_chat_say;
        oa::sim::brains::set_chat_hooks(*match_, hooks);
        seat_chat::Listener listener;
        listener.context = this;
        listener.heard = &Runtime::brain_chat_heard;
        set_seat_chat_listener(listener);
    }
}

} // namespace oa::app
