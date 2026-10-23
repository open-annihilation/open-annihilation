// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One setup's computer seats: which brain plays which place, and the one
// text those seats are stored as. Nothing here reads a package or starts
// a match.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::brain_library {

/// Which setup a seat record belongs to.
enum class Setup : uint8_t {
    skirmish,    ///< a skirmish
    multiplayer, ///< the battle room on this machine
    campaign,    ///< a campaign
};

/// A brain level's income, in the same order as the game's difficulty.
enum class Difficulty : uint8_t {
    easy,   ///< the easy income
    medium, ///< the medium income
    hard,   ///< the hard income
};

/// One computer seat a setup remembers.
///
/// `place` is the player slot, 0 to 9, in a skirmish or a campaign. In the
/// battle room it is the order this machine seated its computer players,
/// from 0. An empty `brain` is the built-in computer player, chosen on
/// purpose. `version` and `package_sha256` are empty in a setup's memory
/// and are filled when a game is saved. `level` is a brain's level id, or
/// `easy`, `medium` or `hard` for the built-in player; empty means the
/// setup's own difficulty. `options` are in the brain's option order, each
/// value as that option's canonical text.
struct Seat {
    int32_t place{};              ///< the seat, 0 to 9
    std::string brain{};          ///< the brain's id; empty is built-in
    std::string version{};        ///< the brain's version; empty in memory
    std::string package_sha256{}; ///< 64 lower-case hex digits; empty in memory
    std::string level{};          ///< the level id; empty for the setup's difficulty
    std::vector<std::pair<std::string, std::string>> options{}; ///< option id and canonical value
};

/// The most seats one setup remembers.
inline constexpr std::size_t most_seats = 10;

/// The seats one setup remembers, in any order.
struct Seats {
    std::vector<Seat> seats{}; ///< the seats; empty when the setup has none
};

/// Writes seats as their one compact text.
///
/// Members are written in byte order and seats in order of place. An empty
/// member is left out. `place` is always written.
///
/// @param seats the seats
/// @return the text
[[nodiscard]] std::string seats_text(const Seats& seats);

/// Reads the seat text.
///
/// Member order does not matter. A member this does not know is ignored.
/// A text that breaks a rule, or is not that shape, reads as nothing. This
/// never throws and never trusts a count it has not checked.
///
/// @param text the text, at most 16 KiB
/// @return the seats, or nothing
[[nodiscard]] std::optional<Seats> parse_seats(std::string_view text);

/// The level a difficulty plays when a brain's own level was not named.
///
/// The first level of `wanted`, or the last level when none has it. An
/// empty list is 0.
///
/// @param levels each level's difficulty, in the brain's order
/// @param wanted the difficulty asked for
/// @return the index into `levels`, or 0 when `levels` is empty
[[nodiscard]] std::size_t
default_level(std::span<const Difficulty> levels, Difficulty wanted) noexcept;

} // namespace oa::data::brain_library
