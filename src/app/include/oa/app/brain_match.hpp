// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A two-seat headless match between brains: how the command names the seats,
// when the match ends, and the lines it prints.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace oa::app {

/// One seat of a brain match, as the command named it.
///
/// `spec` is `builtin`, an installed brain id, or a folder path. `level` is
/// empty when the command left the level to its default.
struct BrainMatchSeat {
    /// `builtin`, an installed id, or a folder path.
    std::string spec;
    /// The level id, or empty for the default.
    std::string level;
};

/// Tells whether a seat spec names the built-in computer player.
///
/// @param spec one seat's text, not trimmed
/// @return true only for the word `builtin`
[[nodiscard]] bool brain_match_builtin(std::string_view spec) noexcept;

/// Tells whether a seat spec names a brain by its folder.
///
/// A folder holds `/` or `\`, or starts with `.`. `builtin` and an installed
/// id are not folders.
///
/// @param spec one seat's text, not trimmed
/// @return true when the seat is a folder path
[[nodiscard]] bool brain_match_folder(std::string_view spec) noexcept;

/// Reads two seats and their levels.
///
/// `seats` is two non-empty parts split at one comma. A part is not trimmed.
/// `builtin` is the built-in player. A part that holds `/` or `\` or starts
/// with `.` is a folder. Anything else is an installed brain id. An empty
/// `levels` leaves both levels empty. A levels text is two non-empty parts.
/// A built-in level, when set, is `easy`, `medium` or `hard`. A brain's level
/// is not checked here.
///
/// @param seats the `--brain-match` text
/// @param levels the `--brain-levels` text, or empty
/// @param error one sentence when the text is refused
/// @return the two seats, or null when `error` is set
[[nodiscard]] std::optional<std::array<BrainMatchSeat, 2>>
parse_brain_match(std::string_view seats, std::string_view levels, std::string& error);

/// How a brain match stopped.
enum class BrainMatchEnd {
    going,         ///< both seats still have units, and ticks remain
    last_standing, ///< one seat has units and the other has none
    draw,          ///< neither seat has units
    time           ///< both still have units and the tick count is spent
};

/// Decides whether a brain match has ended.
///
/// Units are read first. One seat with units and the other with none: that
/// seat wins, and `winner` is its player index, 0 or 1. Neither with units:
/// a draw, and `winner` is 255. Both with units after `ticks` ticks: time,
/// and `winner` is 255. Otherwise the match is still going and `winner` is
/// left unchanged. The units rule wins when the tick count is spent as well.
///
/// @param live_units live units of player 0 and player 1
/// @param tick the tick just played
/// @param ticks the tick the command asked to play
/// @param winner set to the winning player, or 255 when there is none
/// @return why the match ended, or `going`
[[nodiscard]] BrainMatchEnd brain_match_end(
    std::span<const uint32_t, 2> live_units, uint32_t tick, uint32_t ticks, uint8_t& winner
) noexcept;

/// Truncates a resource total toward zero into a whole number.
///
/// A value that is not finite becomes 0. A value outside the whole-number
/// range becomes the nearest end of that range.
///
/// @param value the stored amount
/// @return the whole number the report prints
[[nodiscard]] int64_t brain_match_whole(double value) noexcept;

/// Prints the match's map, seed and requested length.
///
/// @param out the report
/// @param map the map the command named
/// @param seed the seed the match played
/// @param ticks the tick count the command asked for
void write_brain_match_heading(
    std::ostream& out, std::string_view map, uint32_t seed, uint64_t ticks
);

/// Prints one brain seat's id, version, level and side.
///
/// `seat` is already 1 or 2.
///
/// @param out the report
/// @param seat the seat number
/// @param id the brain's id
/// @param version the brain's version
/// @param level the level's id
/// @param side the seat's side
void write_brain_match_brain(
    std::ostream& out,
    int seat,
    std::string_view id,
    std::string_view version,
    std::string_view level,
    int side
);

/// Prints one built-in seat's level and side.
///
/// `seat` is already 1 or 2. `level` is `easy`, `medium` or `hard`.
///
/// @param out the report
/// @param seat the seat number
/// @param level the built-in level word
/// @param side the seat's side
void write_brain_match_builtin(std::ostream& out, int seat, std::string_view level, int side);

/// Prints the end tick, the winner and the reason.
///
/// `winner` 0 prints `seat 1`, 1 prints `seat 2`, and any other value prints
/// `none`. `going` is not a reason the report prints; it is written as `time`.
///
/// @param out the report
/// @param tick the tick the match stopped on
/// @param winner the winning player, or 255 when there is none
/// @param reason why the match ended
void write_brain_match_end(std::ostream& out, uint32_t tick, uint8_t winner, BrainMatchEnd reason);

/// Prints one seat's units, resources, instructions and status.
///
/// Every number is already a whole decimal. `has_instructions` false prints
/// `-` for the instructions. `status` is `playing`, `stopped-at` and its tick,
/// or `built-in`. `seat` is already 1 or 2.
///
/// @param out the report
/// @param seat the seat number
/// @param units live units
/// @param built units the seat built
/// @param lost units the seat lost
/// @param kills units the seat killed
/// @param metal metal in store
/// @param energy energy in store
/// @param metal_produced metal the seat has produced
/// @param energy_produced energy the seat has produced
/// @param has_instructions true when the seat had a brain
/// @param instructions instructions that brain spent
/// @param status `playing`, `stopped-at N` or `built-in`
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
);

/// Prints why a seated brain stopped.
///
/// @param out the report
/// @param seat the seat number, already 1 or 2
/// @param reason the stop sentence
void write_brain_match_stopped(std::ostream& out, int seat, std::string_view reason);

/// Prints that a seat's brain was not seated.
///
/// `id` is the brain's id, never a path.
///
/// @param out the report
/// @param seat the seat number, already 1 or 2
/// @param id the brain's id
/// @param reason why it was not seated
void write_brain_match_unseated(
    std::ostream& out, int seat, std::string_view id, std::string_view reason
);

/// Prints that the named map could not be played.
///
/// @param out the report
/// @param map the map the command named
void write_brain_match_unplayable(std::ostream& out, std::string_view map);

/// Prints the world digest as 16 lower-case hex digits.
///
/// @param out the report
/// @param digest the world digest
void write_brain_match_digest(std::ostream& out, uint64_t digest);

} // namespace oa::app
