// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seats a brain match names, the tick it ends on, and the lines it prints.
#include "oa/app/brain_match.hpp"

#include "oa/formats/oabrain/manifest.hpp"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace oa::app {
namespace {

enum class PairSplit { two, one, many, empty };

/// Splits text at one comma into two non-empty parts. Nothing is trimmed.
PairSplit split_pair(std::string_view text, std::string_view& first, std::string_view& second) {
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos)
        return PairSplit::one;
    if (text.find(',', comma + 1) != std::string_view::npos)
        return PairSplit::many;
    first = text.substr(0, comma);
    second = text.substr(comma + 1);
    if (first.empty() || second.empty())
        return PairSplit::empty;
    return PairSplit::two;
}

[[nodiscard]] bool builtin_level(std::string_view level) noexcept {
    return level == "easy" || level == "medium" || level == "hard";
}

[[nodiscard]] const char* end_word(BrainMatchEnd reason) noexcept {
    if (reason == BrainMatchEnd::last_standing)
        return "last-standing";
    if (reason == BrainMatchEnd::draw)
        return "draw";
    return "time";
}

} // namespace

bool brain_match_builtin(std::string_view spec) noexcept {
    return spec == "builtin";
}

bool brain_match_folder(std::string_view spec) noexcept {
    if (spec.empty())
        return false;
    if (spec.front() == '.')
        return true;
    return spec.find('/') != std::string_view::npos || spec.find('\\') != std::string_view::npos;
}

std::optional<std::array<BrainMatchSeat, 2>>
parse_brain_match(std::string_view seats, std::string_view levels, std::string& error) {
    std::string_view first;
    std::string_view second;
    const PairSplit seats_split = split_pair(seats, first, second);
    if (seats_split == PairSplit::empty) {
        error = "--brain-match has an empty seat";
        return std::nullopt;
    }
    if (seats_split != PairSplit::two) {
        error = "--brain-match expects two seats";
        return std::nullopt;
    }

    std::string_view level_first;
    std::string_view level_second;
    if (!levels.empty()) {
        const PairSplit levels_split = split_pair(levels, level_first, level_second);
        if (levels_split == PairSplit::empty) {
            error = "--brain-levels has an empty level";
            return std::nullopt;
        }
        if (levels_split != PairSplit::two) {
            error = "--brain-levels expects two levels";
            return std::nullopt;
        }
    }

    std::array<BrainMatchSeat, 2> parsed;
    const std::string_view specs[2] = {first, second};
    const std::string_view level_text[2] = {level_first, level_second};
    for (std::size_t index = 0; index < 2; ++index) {
        parsed[index].spec = std::string(specs[index]);
        parsed[index].level = std::string(level_text[index]);
        if (brain_match_builtin(specs[index])) {
            if (!parsed[index].level.empty() && !builtin_level(parsed[index].level)) {
                error = "--brain-levels: a built-in level is easy, medium or hard";
                return std::nullopt;
            }
            continue;
        }
        if (brain_match_folder(specs[index]))
            continue;
        if (!oa::formats::oabrain::brain_id(specs[index])) {
            error = "--brain-match: " + parsed[index].spec + " is not a brain id";
            return std::nullopt;
        }
    }
    error.clear();
    return parsed;
}

BrainMatchEnd brain_match_end(
    std::span<const uint32_t, 2> live_units, uint32_t tick, uint32_t ticks, uint8_t& winner
) noexcept {
    const bool left = live_units[0] > 0;
    const bool right = live_units[1] > 0;
    if (left != right) {
        winner = left ? static_cast<uint8_t>(0) : static_cast<uint8_t>(1);
        return BrainMatchEnd::last_standing;
    }
    if (!left) {
        winner = 255;
        return BrainMatchEnd::draw;
    }
    if (tick >= ticks) {
        winner = 255;
        return BrainMatchEnd::time;
    }
    return BrainMatchEnd::going;
}

int64_t brain_match_whole(double value) noexcept {
    if (!std::isfinite(value))
        return 0;
    const double whole = std::trunc(value);
    constexpr double greatest = static_cast<double>(std::numeric_limits<int64_t>::max());
    constexpr double least = static_cast<double>(std::numeric_limits<int64_t>::min());
    if (whole >= greatest)
        return std::numeric_limits<int64_t>::max();
    if (whole <= least)
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(whole);
}

void write_brain_match_heading(
    std::ostream& out, std::string_view map, uint32_t seed, uint64_t ticks
) {
    out << "brain match: map " << map << " seed " << seed << " ticks " << ticks << '\n';
}

void write_brain_match_brain(
    std::ostream& out,
    int seat,
    std::string_view id,
    std::string_view version,
    std::string_view level,
    int side
) {
    out << "brain match: seat " << seat << ' ' << id << ' ' << version << " level " << level
        << " side " << side << '\n';
}

void write_brain_match_builtin(std::ostream& out, int seat, std::string_view level, int side) {
    out << "brain match: seat " << seat << " built-in level " << level << " side " << side << '\n';
}

void write_brain_match_end(std::ostream& out, uint32_t tick, uint8_t winner, BrainMatchEnd reason) {
    const char* who = "none";
    if (winner == 0)
        who = "seat 1";
    else if (winner == 1)
        who = "seat 2";
    out << "brain match: end tick " << tick << " winner " << who << " reason " << end_word(reason)
        << '\n';
}

void write_brain_match_totals(
    std::ostream& out,
    int seat,
    int64_t units,
    int64_t built,
    int64_t lost,
    int64_t kills,
    int64_t metal,
    int64_t energy,
    int64_t metal_produced,
    int64_t energy_produced,
    bool has_instructions,
    uint64_t instructions,
    std::string_view status
) {
    out << "brain match: seat " << seat << " units " << units << " built " << built << " lost "
        << lost << " kills " << kills << " metal " << metal << " energy " << energy
        << " metal-produced " << metal_produced << " energy-produced " << energy_produced
        << " instructions ";
    if (has_instructions)
        out << instructions;
    else
        out << '-';
    out << " status " << status << '\n';
}

void write_brain_match_stopped(std::ostream& out, int seat, std::string_view reason) {
    out << "brain match: seat " << seat << " stopped: " << reason << '\n';
}

void write_brain_match_unseated(
    std::ostream& out, int seat, std::string_view id, std::string_view reason
) {
    out << "brain match: seat " << seat << ' ' << id << " was not seated: " << reason << '\n';
}

void write_brain_match_unplayable(std::ostream& out, std::string_view map) {
    out << "brain match: map " << map << " could not be played\n";
}

void write_brain_match_digest(std::ostream& out, uint64_t digest) {
    std::ostringstream text;
    text << std::hex << std::nouppercase << std::setw(16) << std::setfill('0') << digest;
    out << "brain match: digest " << text.str() << '\n';
}

} // namespace oa::app
