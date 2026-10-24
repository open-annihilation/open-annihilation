// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Seats the brains a skirmish or a campaign mission named, before the
// match's first tick. A seat that cannot take its brain plays as the
// built-in computer player.

#include "oa/app/runtime.hpp"

#include "oa/app/brain_match.hpp"
#include "oa/core/player.h"
#include "oa/data/script_host/script_host.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/sim/brains/chat.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/brains/shadow.hpp"
#include "oa/sim/messages.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
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

const char* shadow_start_word(oa::sim::brains::ShadowStart start) {
    if (start == oa::sim::brains::ShadowStart::same)
        return "same";
    if (start == oa::sim::brains::ShadowStart::differs)
        return "differs";
    return "not compared";
}

void write_shadow_text(const std::filesystem::path& path, const char* text) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return;
    if (text != nullptr)
        out << text;
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
    brain_seat_id_[player] = std::string(id);
    brain_seat_version_[player] = std::string(version);
    brain_seat_level_[player] = std::string(level);
    brain_seat_stopped_[player] = false;
    brain_stop_tick_[player] = 0;
    brain_stop_text_[player].clear();
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
        runtime->brain_seat_stopped_[player] = true;
        runtime->brain_stop_tick_[player] = tick;
        std::string described = oa::data::script_host::describe(error);
        if (described.empty())
            described = notice_reason(error);
        runtime->brain_stop_text_[player] = std::move(described);
        BrainLog& log = runtime->brain_logs_[player];
        log.stop(tick, error);
        const bool shadow = runtime->match_ != nullptr &&
                            oa::sim::brains::brain_is_shadow(*runtime->match_, player);
        if (!shadow)
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
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        brain_unseated_id_[player].clear();
        brain_unseated_reason_[player].clear();
    }
    const std::filesystem::path installed_root = user_folder() / std::string(brains_folder_name);
    bool seated_one = false;
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const std::optional<BrainSeatChoice>& choice = brain_seat_choices_[player];
        if (!choice.has_value())
            continue;
        const oa::Player& record = match_->state().game.players[player];
        if (record.in_use == 0 || record.status != OA_PLAYER_STATUS_COMPUTER)
            continue;
        std::filesystem::path brains_folder = installed_root;
        std::string id = choice->id;
        if (!choice->folder.empty()) {
            brains_folder = choice->folder.parent_path();
            const std::string named = choice->folder.filename().string();
            if (!named.empty())
                id = named;
        }
        const auto refuse = [&](std::string_view reason) {
            report_seat(player, id, reason);
            brain_unseated_id_[player] = id;
            brain_unseated_reason_[player] = std::string(reason);
        };
        try {
            std::string error;
            const std::optional<InstalledBrain> installed =
                read_installed_brain(brains_folder, id, error);
            if (!installed) {
                refuse(error.empty() ? "the brain could not be read" : error);
                continue;
            }
            ResolvedSeat resolved;
            if (!resolve_seat(*installed, *choice, resolved, error)) {
                refuse(error.empty() ? "the brain could not be seated" : error);
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
            store_brain_game_facts();
            const oa::sim::brains::SeatOutcome outcome = oa::sim::brains::seat_brain(*match_, spec);
            if (outcome.result != oa::sim::brains::SeatResult::seated) {
                refuse(failure_text(outcome));
                report_unseated_brain(player, installed->manifest.name, outcome);
            } else {
                seated_one = true;
            }
        } catch (const std::exception& ex) {
            const char* text = ex.what();
            refuse(text != nullptr ? text : "the brain could not be seated");
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

void Runtime::seat_shadow_brains() {
    if (match_ == nullptr || options_.brain_shadow.empty())
        return;
    if (multiplayer_session() || (current_extension_state() &
                                  (extension_state::shared_match | extension_state::replay)) != 0)
        return;
    shadow_report_printed_ = false;
    std::filesystem::path brains_folder = user_folder() / std::string(brains_folder_name);
    std::string id = options_.brain_shadow;
    if (brain_match_folder(options_.brain_shadow)) {
        const std::filesystem::path folder{options_.brain_shadow};
        brains_folder = folder.parent_path();
        const std::string named = folder.filename().string();
        if (!named.empty())
            id = named;
    }
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const oa::Player& record = match_->state().game.players[player];
        if (record.in_use == 0 || record.status != OA_PLAYER_STATUS_COMPUTER)
            continue;
        if (oa::sim::brains::brain_seated(*match_, player) ||
            oa::sim::brains::brain_is_shadow(*match_, player))
            continue;
        try {
            std::string error;
            const std::optional<InstalledBrain> installed =
                read_installed_brain(brains_folder, id, error);
            if (!installed) {
                report_seat(player, id, error.empty() ? "the brain could not be read" : error);
                continue;
            }
            BrainSeatChoice choice;
            choice.id = installed->manifest.id;
            ResolvedSeat resolved;
            if (!resolve_seat(*installed, choice, resolved, error)) {
                report_seat(player, id, error.empty() ? "the brain could not be seated" : error);
                continue;
            }
            std::vector<oa::sim::brains::PackageFile> files;
            std::vector<oa::sim::brains::OptionValue> options;
            std::vector<oa::sim::brains::NetworkFile> networks;
            oa::sim::brains::SeatSpec spec =
                seat_spec(player, *installed, resolved, files, options, networks);
            spec.shadow = true;
            spec.shadow_sections = options_.brain_shadow_sections;
            prepare_brain_report(
                player,
                installed->manifest.name,
                resolved.version,
                installed->manifest.id,
                resolved.level
            );
            store_brain_game_facts();
            const oa::sim::brains::SeatOutcome outcome = oa::sim::brains::seat_brain(*match_, spec);
            if (outcome.result != oa::sim::brains::SeatResult::seated)
                report_seat(player, id, failure_text(outcome));
        } catch (const std::exception& ex) {
            const char* text = ex.what();
            report_seat(player, id, text != nullptr ? text : "the brain could not be seated");
        }
    }
}

void Runtime::print_shadow_reports() {
    if (shadow_report_printed_ || match_ == nullptr || options_.brain_shadow.empty())
        return;
    shadow_report_printed_ = true;
    const bool write_logs = !options_.brain_shadow_log.empty();
    std::filesystem::path directory;
    if (write_logs) {
        directory = std::filesystem::path(options_.brain_shadow_log);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
            directory.clear();
    }
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const oa::sim::brains::ShadowReport* report =
            oa::sim::brains::shadow_report(*match_, player);
        if (report == nullptr)
            continue;
        const int seat = static_cast<int>(player);
        if (report->stopped) {
            const char* message = report->message != nullptr ? report->message : "";
            std::cout << "brain shadow: seat " << seat << ": stopped at tick "
                      << report->stopped_tick << ": " << message << '\n';
        } else {
            std::cout << "brain shadow: seat " << seat << ": start "
                      << shadow_start_word(report->start) << ", " << report->ticks << " ticks, ";
            if (report->differ_count == 0)
                std::cout << "no difference\n";
            else
                std::cout << "first difference at tick " << report->first_tick << " in "
                          << report->section << ", " << report->differ_count << " ticks differ\n";
        }
        if (directory.empty())
            continue;
        const std::string prefix = "seat-" + std::to_string(seat);
        write_shadow_text(directory / (prefix + "-start-builtin.log"), report->start_builtin);
        write_shadow_text(directory / (prefix + "-start-brain.log"), report->start_brain);
        const uint8_t files = report->tick_files < 3 ? report->tick_files : 3;
        for (uint8_t index = 0; index < files; ++index) {
            const std::string tick_name =
                prefix + "-tick-" + std::to_string(report->tick_numbers[index]);
            write_shadow_text(
                directory / (tick_name + "-builtin.log"), report->tick_builtin[index]
            );
            write_shadow_text(directory / (tick_name + "-brain.log"), report->tick_brain[index]);
        }
    }
    std::cout.flush();
}

} // namespace oa::app
