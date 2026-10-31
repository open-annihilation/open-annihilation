// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A two-seat headless skirmish. Each seat is an installed brain, a brain
// folder, or the built-in computer player. The match stops when one seat or
// neither has units left, or after the tick count, and prints a fixed report.

#include "oa/app/runtime.hpp"

#include "oa/app/brain_match.hpp"
#include "oa/core/game_state.h"
#include "oa/platform/files.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/brains/seat.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app {
namespace {

/// The last path piece of a folder seat. A separator is `/` or `\`.
std::string folder_id(std::string_view spec) {
    std::size_t start = 0;
    for (std::size_t index = 0; index < spec.size(); ++index) {
        if (spec[index] == '/' || spec[index] == '\\')
            start = index + 1;
    }
    if (start >= spec.size())
        return {};
    return std::string(spec.substr(start));
}

/// The id the report may print. A folder contributes its last piece, never
/// the path.
std::string seat_name(std::string_view recorded, const BrainMatchSeat& seat) {
    if (!recorded.empty())
        return std::string(recorded);
    if (brain_match_folder(seat.spec)) {
        const std::string id = folder_id(seat.spec);
        if (!id.empty())
            return id;
        return "brain";
    }
    if (!seat.spec.empty())
        return seat.spec;
    return "brain";
}

std::string builtin_level_word(std::string_view level) {
    if (level.empty())
        return "hard";
    return std::string(level);
}

int32_t builtin_difficulty(std::string_view level) {
    if (level == "easy")
        return OA_DIFFICULTY_EASY;
    if (level == "medium")
        return OA_DIFFICULTY_MEDIUM;
    return OA_DIFFICULTY_HARD;
}

} // namespace

int Runtime::run_headless_brain_match(std::size_t ticks) {
    std::string parse_error;
    const auto parsed = parse_brain_match(options_.brain_match, options_.brain_levels, parse_error);
    if (!parsed)
        throw std::runtime_error(parse_error);
    const std::array<BrainMatchSeat, 2>& seats = *parsed;

    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    // A remembered skirmish seat must not play. The click copied those seats
    // in; drop them, and drop any list already held for the next match.
    // Seating goes through the choices below, which the match start reads.
    cancel_brain_seats_once();
    setup_brain_seats(oa::data::brain_library::Setup::skirmish) = {};
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
        set_brain_seat(player, std::nullopt);

    skirmish_settings_.slot_count = 2;
    skirmish_settings_.map_name = options_.brain_map;
    for (std::size_t index = 0; index < 2; ++index) {
        entry::SkirmishSlot& slot = skirmish_settings_.slots[index];
        slot.controller = entry::controller::computer;
        slot.side = static_cast<int32_t>(index);
        slot.alliance = entry::unassigned_alliance;
        slot.color = static_cast<int32_t>(index);
    }
    for (std::size_t index = 2; index < skirmish_settings_.slots.size(); ++index)
        skirmish_settings_.slots[index].controller = entry::controller::disabled;
    state_.player_count = 2;
    if (select_map(options_.brain_map) == 0 || map_player_capacity() < 2) {
        write_brain_match_unplayable(std::cout, options_.brain_map);
        std::cout.flush();
        return 1;
    }

    for (uint8_t player = 0; player < 2; ++player) {
        if (brain_match_builtin(seats[player].spec))
            continue;
        BrainSeatChoice choice;
        if (brain_match_folder(seats[player].spec)) {
            choice.folder = oa::platform::path_from_utf8(seats[player].spec);
            choice.id = folder_id(seats[player].spec);
        } else {
            choice.id = seats[player].spec;
        }
        choice.level = seats[player].level;
        set_brain_seat(player, std::move(choice));
    }

    apply_skirmish_players();
    entry::reset_mission_results(state_);
    state_.pending_signal = frontend::signal_id::proceed;
    if (match_ && !altitude_sight_blocked_)
        enter_match_view();
    if (screen_ != Screen::match || !match_) {
        write_brain_match_unplayable(std::cout, options_.brain_map);
        std::cout.flush();
        return 1;
    }

    bool refused = false;
    for (uint8_t player = 0; player < 2; ++player) {
        if (brain_match_builtin(seats[player].spec))
            continue;
        const bool seated = oa::sim::brains::brain_seated(*match_, player);
        if (seated && brain_unseated_reason_[player].empty())
            continue;
        const std::string id = seat_name(brain_unseated_id_[player], seats[player]);
        const std::string reason = brain_unseated_reason_[player].empty()
                                       ? std::string("the brain could not be seated")
                                       : brain_unseated_reason_[player];
        write_brain_match_unseated(std::cout, static_cast<int>(player) + 1, id, reason);
        refused = true;
    }
    if (refused) {
        std::cout.flush();
        return 1;
    }

    for (uint8_t player = 0; player < 2; ++player) {
        if (!brain_match_builtin(seats[player].spec))
            continue;
        oa::sim::ai::computer_players_set_difficulty(
            oa::sim::ai::match_computer_players(*match_),
            player,
            builtin_difficulty(seats[player].level)
        );
    }

    const auto live_units = [this] {
        std::array<uint32_t, 2> live{};
        for (const auto& slot : match_->world().slots) {
            if (slot.unit == nullptr || slot.record.type_index == 0)
                continue;
            const uint8_t owner = slot.record.owner_index;
            if (owner < live.size())
                ++live[owner];
        }
        return live;
    };

    const uint32_t limit = ticks > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ticks);
    BrainMatchEnd reason = BrainMatchEnd::time;
    uint8_t winner = 255;
    for (std::size_t step = 1; step <= ticks; ++step) {
        try {
            step_match_simulation();
        } catch (const std::exception& failure) {
            report_match_tick_error(failure.what());
        }
        const std::array<uint32_t, 2> live = live_units();
        reason = brain_match_end(live, match_->state().game.tick, limit, winner);
        if (reason != BrainMatchEnd::going)
            break;
    }
    if (reason == BrainMatchEnd::going) {
        reason = BrainMatchEnd::time;
        winner = 255;
    }

    const std::array<uint32_t, 2> live = live_units();
    write_brain_match_heading(std::cout, options_.brain_map, brain_match_seed_, ticks);
    for (uint8_t player = 0; player < 2; ++player) {
        const int seat = static_cast<int>(player) + 1;
        if (brain_match_builtin(seats[player].spec))
            write_brain_match_builtin(
                std::cout, seat, builtin_level_word(seats[player].level), static_cast<int>(player)
            );
        else {
            const std::string id = seat_name(brain_seat_id_[player], seats[player]);
            const std::string level =
                brain_seat_level_[player].empty() ? seats[player].level : brain_seat_level_[player];
            write_brain_match_brain(
                std::cout, seat, id, brain_seat_version_[player], level, static_cast<int>(player)
            );
        }
    }
    write_brain_match_end(std::cout, match_->state().game.tick, winner, reason);
    for (uint8_t player = 0; player < 2; ++player) {
        const oa::Player& record = match_->state().game.players[player];
        const bool builtin = brain_match_builtin(seats[player].spec);
        const bool stopped = !builtin && brain_seat_stopped_[player];
        std::string status = "playing";
        if (builtin)
            status = "built-in";
        else if (stopped)
            status = "stopped-at " + std::to_string(brain_stop_tick_[player]);
        const int seat = static_cast<int>(player) + 1;
        write_brain_match_totals(
            std::cout,
            seat,
            static_cast<int64_t>(live[player]),
            static_cast<int64_t>(record.units_created),
            static_cast<int64_t>(record.losses),
            static_cast<int64_t>(record.kills),
            brain_match_whole(static_cast<double>(record.metal)),
            brain_match_whole(static_cast<double>(record.energy)),
            brain_match_whole(record.metal_produced_total),
            brain_match_whole(record.energy_produced_total),
            !builtin,
            oa::sim::brains::seat_instructions(*match_, player),
            status
        );
        if (stopped)
            write_brain_match_stopped(std::cout, seat, brain_stop_text_[player]);
    }
    write_brain_match_digest(std::cout, match_world_digest());
    std::cout.flush();
    write_brain_record_results(winner, live.data());
    return 0;
}

} // namespace oa::app
