// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Seats the brains a skirmish or a campaign mission named, before the
// match's first tick. A seat that cannot take its brain plays as the
// built-in computer player.

#include "oa/app/runtime.hpp"

#include "oa/core/player.h"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/brains/chat.hpp"
#include "oa/sim/brains/seat.hpp"

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

} // namespace

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
            const oa::sim::brains::SeatOutcome outcome = oa::sim::brains::seat_brain(*match_, spec);
            if (outcome.result != oa::sim::brains::SeatResult::seated)
                report_seat(player, choice->id, failure_text(outcome));
            else
                seated_one = true;
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
